// The render inspector (docs/live-editing.md 8.2): shows what the runtime actually draws.
//
// F3 cycles the outline colours (off -> by material -> by shader -> by draw kind). Every object
// of every drawn range (MS_GetDrawRange: a merged cell mesh, an instanced box range or a model
// group) within OUTLINE_DIST of the eye and in the view frustum is outlined (boxes: their edges,
// models: their rotated bounds), at most OUTLINE_MAX objects, nearest first; ranges farther away
// get their range bounds outlined. Labels: the range under the crosshair (or the cursor while
// it is free) and the LABEL_COUNT nearest on-screen ranges, skipping any that would overlap an
// earlier one. SHIFT+F3 opens the panel (the UiList widget): the materials, then the distinct
// shaders, each with its range count and triangles; SPACE hides / shows the selected item, I
// isolates it (again: everything back), typing filters. Hiding is the runtime's draw filter
// (MS_SetDrawFilter: hidden ranges leave the camera and the shadow passes). F4 cycles R3D's
// buffer views (R3D_SetOutputMode). Off by default: with the mode off, no filter and the scene
// view, nothing is drawn or set, so the image is the normal one.
//
// What is hidden is kept by name ("m:<material>", "s:<shader path>"; "*:<name>" from the
// command line matches either), so it survives reloads and live edits; the per-range arrays are
// derived from it whenever scene->version changes (InspectorApply). Every array only grows, and
// the overlay's scratch is sized for every object and range at that point, so drawing a frame
// never allocates.
#include "inspector.h"
#include <ctype.h>
#include <math.h>

#define OUTLINE_DIST    80.0f       // objects nearer than this get their own outline (m)
#define OUTLINE_MAX     20000       // at most this many objects, the nearest
#define LABEL_COUNT     24          // labels besides the hovered range's
#define GOLDEN          0.618033988749895

typedef struct { float dist; int range, obj; } Candidate;   // obj -1: the range's bounds

static const char *const MODE_NAMES[INSPECT_MODE_COUNT] = { "off", "material", "shader", "draw" };
static const char *const OUTPUT_NAMES[] = { "scene", "albedo", "normal", "orm", "radiance", "specular",
                                            "ssao", "ssil", "ssgi", "ssr", "bloom", "dof" };
#define OUTPUT_COUNT ((int)(sizeof(OUTPUT_NAMES) / sizeof(OUTPUT_NAMES[0])))
_Static_assert(R3D_OUTPUT_DOF == OUTPUT_COUNT - 1, "R3D_OutputMode changed: update OUTPUT_NAMES");

// #region Names and state

const char *InspectorOutputName(R3D_OutputMode mode)
{
    return (int)mode >= 0 && (int)mode < OUTPUT_COUNT ? OUTPUT_NAMES[mode] : "?";
}

bool InspectorSetMode(Inspector *in, const char *name)
{
    for (int m = 0; m < INSPECT_MODE_COUNT; m++)
        if (strcmp(name, MODE_NAMES[m]) == 0) { in->mode = (InspectMode)m; return true; }
    return false;
}

bool InspectorSetOutput(Inspector *in, const char *name)
{
    for (int m = 0; m < OUTPUT_COUNT; m++)
        if (strcmp(name, OUTPUT_NAMES[m]) == 0) { in->output = (R3D_OutputMode)m; return true; }
    return false;
}

void InspectorOpenPanel(Inspector *in)
{
    in->panelOpen = true;
    in->list.dirty = true;
}

bool InspectorCapturesInput(const Inspector *in) { return in->panelOpen; }

// Does a stored name ("m:x", "s:x", "*:x") name material / shader `name`?
static bool NameIs(const char *entry, char kind, const char *name)
{
    return name && (entry[0] == kind || entry[0] == '*') && entry[1] == ':' && strcmp(entry + 2, name) == 0;
}

static int HiddenIndex(const Inspector *in, char kind, const char *name)
{
    for (int k = 0; k < in->hiddenCount; k++)
        if (NameIs(in->hidden[k], kind, name)) return k;
    return -1;
}

static char *Entry(char kind, const char *name)
{
    size_t n = strlen(name);
    char *e = malloc(n + 3);
    if (!e) return NULL;
    e[0] = kind;
    e[1] = ':';
    memcpy(e + 2, name, n + 1);
    return e;
}

static bool AddHidden(Inspector *in, char kind, const char *name)
{
    char *e = Entry(kind, name);
    char **slot = e ? MS_PUSH(in->hidden, in->hiddenCount, in->hiddenCap) : NULL;
    if (!slot) { free(e); return false; }
    *slot = e;
    in->filterDirty = true;
    in->filterGeneration++;
    return true;
}

static void RemoveHidden(Inspector *in, int k)
{
    free(in->hidden[k]);
    in->hidden[k] = in->hidden[--in->hiddenCount];
    in->filterDirty = true;
    in->filterGeneration++;
}

static void SetIsolated(Inspector *in, char *entry)
{
    free(in->isolated);
    in->isolated = entry;
    in->filterDirty = true;
    in->filterGeneration++;
}

bool InspectorHide(Inspector *in, const char *name) { return name[0] && AddHidden(in, '*', name); }

// A hidden / isolated name no material or shader of the scene has (the first), NULL = all known
const char *InspectorUnknownName(const Inspector *in, const MS_Scene *s)
{
    for (int k = -1; k < in->hiddenCount; k++) {
        const char *e = k < 0 ? in->isolated : in->hidden[k];
        if (!e) continue;
        const char *name = e + 2;
        bool known = false;
        for (int m = 0; m < s->materialCount && !known; m++)
            known = (e[0] != 's' && !strcmp(s->materials[m].name, name)) ||
                    (e[0] != 'm' && s->materials[m].shader && !strcmp(s->materials[m].shader, name));
        if (!known) return name;
    }
    return NULL;
}

bool InspectorIsolate(Inspector *in, const char *name)
{
    char *e = name[0] ? Entry('*', name) : NULL;
    if (!e) return false;
    SetIsolated(in, e);
    return true;
}

// #endregion

// #region Arrays per scene version

// List items: materials 0..materialCount-1, then shaders
static int ItemCount(const Inspector *in) { return in->scene ? in->scene->materialCount + in->shaderCount : 0; }

static const char *ItemName(const Inspector *in, int item, char *kind)
{
    int mc = in->scene->materialCount;
    if (item < mc) { *kind = 'm'; return in->scene->materials[item].name; }
    *kind = 's';
    return in->shaders[item - mc];
}

static bool ItemHidden(const Inspector *in, int item)
{
    int mc = in->scene->materialCount;
    return item < mc ? in->hideMaterial[item] : in->hideShader[item - mc];
}

// Does range r draw with list item `item` (its material or its shader; a model: a slot's)?
static bool RangeUses(const Inspector *in, const MS_DrawRangeInfo *R, int item)
{
    int mc = in->scene->materialCount;
    if (R->kind != MS_DRAW_MODELS) return item < mc ? R->material == item : in->shaderOf[R->material] == item - mc;
    for (int s = 0; s < R->slotCount; s++) {
        int m = R->slotMaterials[s];
        if (m >= 0 && (item < mc ? m == item : in->shaderOf[m] == item - mc)) return true;
    }
    return false;
}

static bool RangeVisible(const Inspector *in, const MS_DrawRangeInfo *R, int isolatedItem, bool isolating)
{
    if (isolating && (isolatedItem < 0 || !RangeUses(in, R, isolatedItem))) return false;
    if (R->kind != MS_DRAW_MODELS)
        return !in->hideMaterial[R->material] && !(in->shaderOf[R->material] >= 0 && in->hideShader[in->shaderOf[R->material]]);
    for (int s = 0; s < R->slotCount; s++) {
        int m = R->slotMaterials[s];
        if (m >= 0 && (in->hideMaterial[m] || (in->shaderOf[m] >= 0 && in->hideShader[in->shaderOf[m]]))) return false;
    }
    return true;
}

static int IsolatedItem(const Inspector *in)
{
    if (!in->isolated) return -1;
    for (int i = 0; i < ItemCount(in); i++) {
        char kind;
        const char *name = ItemName(in, i, &kind);
        if (NameIs(in->isolated, kind, name)) return i;
    }
    return -1;
}

// Everything derived from the scene: ranges, shaders, object -> range, item totals, scratch
static bool Rebuild(Inspector *in)
{
    const MS_Scene *s = scene;
    in->scene = s;
    in->version = s->version;
    in->filterDirty = true;
    in->list.dirty = true;
    int nr = MS_DrawRangeCount(s), mc = s->materialCount;
    in->rangeCount = 0;
    in->shaderCount = 0;
    if (!MS_RESERVE(in->ranges, in->rangeInfoCap, nr > 0 ? nr : 1) || !MS_RESERVE(in->rangeHidden, in->rangeCap, nr > 0 ? nr : 1) ||
        !MS_RESERVE(in->shaderOf, in->shaderOfCap, mc > 0 ? mc : 1) || !MS_RESERVE(in->shaders, in->shaderCap, mc > 0 ? mc : 1) ||
        !MS_RESERVE(in->boxRange, in->boxRangeCap, s->boxCount > 0 ? s->boxCount : 1) ||
        !MS_RESERVE(in->modelRange, in->modelRangeCap, s->modelCount > 0 ? s->modelCount : 1))
        return false;
    // distinct shader files, in material order
    for (int m = 0; m < mc; m++) {
        const char *sh = s->materials[m].shader;
        in->shaderOf[m] = -1;
        if (!sh) continue;
        int k = 0;
        while (k < in->shaderCount && strcmp(in->shaders[k], sh) != 0) k++;
        if (k == in->shaderCount) in->shaders[in->shaderCount++] = sh;
        in->shaderOf[m] = k;
    }
    int items = mc + in->shaderCount;
    if (!MS_RESERVE(in->hideMaterial, in->hideCap, items > 0 ? items : 1) ||
        !MS_RESERVE(in->itemRanges, in->itemRangesCap, items > 0 ? items : 1) ||
        !MS_RESERVE(in->itemTris, in->itemTrisCap, items > 0 ? items : 1))
        return false;
    in->hideShader = in->hideMaterial + mc;     // (one array: materials, then shaders)
    for (int b = 0; b < s->boxCount; b++) in->boxRange[b] = -1;
    for (int i = 0; i < s->modelCount; i++) in->modelRange[i] = -1;
    for (int r = 0; r < nr; r++) {
        if (!MS_GetDrawRange(s, r, &in->ranges[r])) return false;
        const MS_DrawRangeInfo *R = &in->ranges[r];
        int *of = R->kind == MS_DRAW_MODELS ? in->modelRange : in->boxRange;
        for (int k = 0; k < R->objectCount; k++) of[R->objects[k]] = r;
    }
    in->rangeCount = nr;
    // What the runtime draws, as its load line counts it (logged when it changes: not per preview step)
    int merged = 0, meshes = 0, instanced = 0, modelDraws = 0, objects = 0;
    long long boxTris = 0, modelTris = 0;
    for (int r = 0; r < nr; r++) {
        const MS_DrawRangeInfo *R = &in->ranges[r];
        objects += R->objectCount;
        if (R->kind == MS_DRAW_MODELS) { modelDraws++; modelTris += R->triangles; continue; }
        boxTris += R->triangles;
        if (R->kind == MS_DRAW_BOXES_MERGED) { merged++; meshes += R->triangles > 0; }
        else instanced++;
    }
    static long long lastLogged = -1;
    long long logKey = ((long long)nr << 32) ^ boxTris ^ (modelTris << 16) ^ objects;
    if (logKey != lastLogged) {
        lastLogged = logKey;
        TraceLog(LOG_INFO, "monolith: inspector: %d box draws (%d merged, %d of them with a mesh; %d instanced; %lld triangles), "
                 "%d model draws (%lld triangles), %d objects", merged + instanced, merged, meshes, instanced, boxTris, modelDraws,
                 modelTris, objects);
    }
    for (int i = 0; i < items; i++) {
        in->itemRanges[i] = 0;
        in->itemTris[i] = 0;
        for (int r = 0; r < nr; r++)
            if (RangeUses(in, &in->ranges[r], i)) { in->itemRanges[i]++; in->itemTris[i] += in->ranges[r].triangles; }
    }
    // scratch: every object and every range a candidate at most
    int cand = s->boxCount + s->modelCount + nr;
    if (cand < 1) cand = 1;
    if (in->scratchCap < cand) {
        void *p = realloc(in->scratch, sizeof(Candidate) * (size_t)cand);
        if (!p) return false;
        in->scratch = p;
        in->scratchCap = cand;
    }
    if (!MS_RESERVE(in->labelPick, in->labelPickCap, nr + 1) || !MS_RESERVE(in->labelRects, in->labelRectCap, LABEL_COUNT + 2))
        return false;
    return true;
}

// rangeHidden[] from the names, then the runtime's filter
static void ApplyFilter(Inspector *in)
{
    const MS_Scene *s = in->scene;
    for (int m = 0; m < s->materialCount; m++) in->hideMaterial[m] = HiddenIndex(in, 'm', s->materials[m].name) >= 0;
    for (int k = 0; k < in->shaderCount; k++) in->hideShader[k] = HiddenIndex(in, 's', in->shaders[k]) >= 0;
    int iso = IsolatedItem(in);
    bool any = false;
    for (int r = 0; r < in->rangeCount; r++) {
        in->rangeHidden[r] = !RangeVisible(in, &in->ranges[r], iso, in->isolated != NULL);
        any |= in->rangeHidden[r];
    }
    MS_SetDrawFilter(scene, any ? in->rangeHidden : NULL, any ? in->rangeCount : 0);
    in->filterDirty = false;
}

void InspectorApply(Inspector *in)
{
    if (R3D_GetOutputMode() != in->output) R3D_SetOutputMode(in->output);
    bool active = in->mode != INSPECT_OFF || in->panelOpen || in->hiddenCount > 0 || in->isolated || in->scene;
    if (!active) return;        // never used: nothing to keep in step
    if (in->scene != scene || in->version != scene->version) {
        if (!Rebuild(in)) {
            // out of memory: no overlay, no filter
            in->rangeCount = 0;
            in->shaderCount = 0;
            in->scene = NULL;
            MS_SetDrawFilter(scene, NULL, 0);
            return;
        }
    }
    if (in->filterDirty) ApplyFilter(in);
}

// #endregion

// #region Keys and the panel

static bool Shift(void) { return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }
static bool Ctrl(void) { return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL); }

// Case-insensitive substring
static bool Contains(const char *s, const char *needle)
{
    for (; *s; s++) {
        int k = 0;
        while (needle[k] && s[k] && tolower((unsigned char)s[k]) == tolower((unsigned char)needle[k])) k++;
        if (!needle[k]) return true;
    }
    return false;
}

static bool MatchItem(int item, const char *filter, void *user)
{
    const Inspector *in = user;
    char kind;
    const char *name = ItemName(in, item, &kind);
    return Contains(name, filter) || Contains(kind == 'm' ? "material" : "shader", filter);
}

static void ToggleHide(Inspector *in, int item)
{
    char kind;
    const char *name = ItemName(in, item, &kind);
    int k = HiddenIndex(in, kind, name);
    if (k >= 0) {
        while (k >= 0) { RemoveHidden(in, k); k = HiddenIndex(in, kind, name); }   // ("*:" entries too)
    } else AddHidden(in, kind, name);
}

static void ToggleIsolate(Inspector *in, int item)
{
    char kind;
    const char *name = ItemName(in, item, &kind);
    if (in->isolated && NameIs(in->isolated, kind, name)) { SetIsolated(in, NULL); return; }
    // isolating shows that item alone: everything hidden comes back first
    while (in->hiddenCount > 0) RemoveHidden(in, in->hiddenCount - 1);
    SetIsolated(in, Entry(kind, name));
}

void InspectorUpdate(Inspector *in)
{
    if (IsKeyPressed(KEY_F3)) {
        if (Shift()) {
            if (in->panelOpen) in->panelOpen = false;
            else InspectorOpenPanel(in);
        } else in->mode = (InspectMode)((in->mode + 1) % INSPECT_MODE_COUNT);
    }
    if (IsKeyPressed(KEY_F4)) in->output = (R3D_OutputMode)((in->output + (Shift() ? OUTPUT_COUNT - 1 : 1)) % OUTPUT_COUNT);
    if (!in->panelOpen) return;
    if (IsKeyPressed(KEY_ESCAPE)) { in->panelOpen = false; return; }
    if (!in->scene) return;     // (InspectorApply builds the items first)
    // SPACE never types into this filter; I isolates while the filter is empty (CTRL+I always)
    bool space = IsKeyPressed(KEY_SPACE);
    bool iso = IsKeyPressed(KEY_I) && (Ctrl() || in->list.filterLen == 0);
    if (space || iso) while (GetCharPressed() > 0) { }
    UiListKeys(&in->list, in->visibleRows);
    if (in->list.dirty) UiListRefilter(&in->list, ItemCount(in), MatchItem, in);
    int item = UiListSelected(&in->list);
    if (item >= 0 && space) ToggleHide(in, item);
    if (item >= 0 && iso) ToggleIsolate(in, item);
}

static Color Hue(int i, float alpha)
{
    double h = fmod(i * GOLDEN, 1.0) * 360.0;
    return Fade(ColorFromHSV((float)h, 0.72f, 1.0f), alpha);
}

#define NO_SHADER_COLOR (Color){ 170, 170, 170, 255 }

static Color ItemColor(const Inspector *in, int item)
{
    return item < in->scene->materialCount ? Hue(item, 1.0f) : Hue(item - in->scene->materialCount, 1.0f);
}

static void DrawItemRow(int item, Rectangle row, bool selected, void *user)
{
    const Inspector *in = user;
    char kind;
    const char *name = ItemName(in, item, &kind);
    int ty = (int)row.y + ((int)row.height - UiLineHeight(UI_M)) / 2;
    bool hidden = ItemHidden(in, item);
    bool isolated = in->isolated && NameIs(in->isolated, kind, name);
    DrawRectangle((int)row.x + 10, (int)row.y + ((int)row.height - 10) / 2, 10, 10, Fade(ItemColor(in, item), hidden ? 0.3f : 1.0f));
    int x = (int)row.x + 28;
    UiText(kind == 'm' ? "material" : "shader", x, ty + 2, UI_XS, UI_DIM);
    x += 64;
    // the counts and the state on the right; the name gets what is left
    const char *state = isolated ? "isolated" : hidden ? "hidden" : "";
    const char *counts = TextFormat("%d range%s  %lld tris", in->itemRanges[item], in->itemRanges[item] == 1 ? "" : "s", in->itemTris[item]);
    int right = (int)(row.x + row.width) - 10;
    int sw = state[0] ? UiMeasure(state, UI_S) : 0;
    if (state[0]) UiText(state, right - sw, ty + 1, UI_S, isolated ? UI_ACCENT : UI_WARN);
    int cw = UiMeasure(counts, UI_S);
    int cx = right - (sw ? sw + 14 : 0) - cw;
    UiText(counts, cx, ty + 1, UI_S, UI_DIM);
    Color ink = hidden ? UI_FAINT : selected ? UI_INK : UI_TEXT;
    // (cut a long name by characters until it fits before the counts)
    int room = cx - 12 - x;
    if (UiMeasure(name, UI_M) <= room) { UiText(name, x, ty, UI_M, ink); return; }
    char cut[160];
    int n = (int)strlen(name);
    if (n > (int)sizeof(cut) - 4) n = (int)sizeof(cut) - 4;
    for (; n > 0; n--) {
        snprintf(cut, sizeof(cut), "%.*s...", n, name);
        if (UiMeasure(cut, UI_M) <= room) break;
    }
    UiText(cut, x, ty, UI_M, ink);
}

// The panel: on the right, its bottom above the help bars and toasts (the HUD lines stay free)
static Rectangle PanelRect(int screenW, int screenH)
{
    float w = fminf(620.0f, (float)screenW - 40.0f), h = fminf(640.0f, (float)screenH - 260.0f);
    if (h < 200.0f) h = fminf(200.0f, (float)screenH - 20.0f);
    float y = (float)screenH - 90.0f - h;
    return (Rectangle){ (float)screenW - w - 20.0f, y > 10.0f ? y : 10.0f, w, h };
}

void InspectorDrawPanel(Inspector *in, int screenW, int screenH)
{
    if (!in->panelOpen || !in->scene) return;
    if (in->list.dirty) UiListRefilter(&in->list, ItemCount(in), MatchItem, in);
    Rectangle r = PanelRect(screenW, screenH);
    in->visibleRows = UiListDraw(&in->list, r, "Render inspector: materials and shaders", 24, DrawItemRow, in,
                                 "{UP}/{DOWN} select|{SPACE} hide / show|{I} isolate (again: all)|type to filter|{ESC} close");
}

int InspectorDrawHud(const Inspector *in, int x, int y)
{
    int hidden = 0;
    for (int r = 0; in->scene && r < in->rangeCount; r++) hidden += in->rangeHidden[r];
    if (in->mode == INSPECT_OFF && in->output == R3D_OUTPUT_SCENE && hidden == 0 && !in->panelOpen) return 0;
    const char *filter = hidden ? TextFormat("|%d of %d draws hidden", hidden, in->rangeCount) : "";
    UiHudHints(TextFormat("inspector|outlines: %s|view: %s%s", MODE_NAMES[in->mode],
                          InspectorOutputName(in->output), filter),
               x, y, UI_HUD_LINE, UI_S, UI_TEXT);
    return UI_HUD_LINE;
}

// #endregion

// #region The overlay

static float BoxDistance(BoundingBox b, Vector3 p)
{
    Vector3 c = { Clamp(p.x, b.min.x, b.max.x), Clamp(p.y, b.min.y, b.max.y), Clamp(p.z, b.min.z, b.max.z) };
    return Vector3Distance(c, p);
}

typedef struct { Vector4 plane[6]; } Frustum;

static Frustum MakeFrustum(Camera3D cam, float aspect)
{
    Matrix view = MatrixLookAt(cam.position, cam.target, cam.up);
    Matrix proj = MatrixPerspective(cam.fovy * DEG2RAD, aspect, rlGetCullDistanceNear(), rlGetCullDistanceFar());
    Matrix m = MatrixMultiply(view, proj);
    // rows of the clip matrix (raymath: m0 m4 m8 m12 is the first row)
    Vector4 r0 = { m.m0, m.m4, m.m8, m.m12 }, r1 = { m.m1, m.m5, m.m9, m.m13 };
    Vector4 r2 = { m.m2, m.m6, m.m10, m.m14 }, r3 = { m.m3, m.m7, m.m11, m.m15 };
    Frustum f;
    f.plane[0] = (Vector4){ r3.x + r0.x, r3.y + r0.y, r3.z + r0.z, r3.w + r0.w };
    f.plane[1] = (Vector4){ r3.x - r0.x, r3.y - r0.y, r3.z - r0.z, r3.w - r0.w };
    f.plane[2] = (Vector4){ r3.x + r1.x, r3.y + r1.y, r3.z + r1.z, r3.w + r1.w };
    f.plane[3] = (Vector4){ r3.x - r1.x, r3.y - r1.y, r3.z - r1.z, r3.w - r1.w };
    f.plane[4] = (Vector4){ r3.x + r2.x, r3.y + r2.y, r3.z + r2.z, r3.w + r2.w };
    f.plane[5] = (Vector4){ r3.x - r2.x, r3.y - r2.y, r3.z - r2.z, r3.w - r2.w };
    return f;
}

static bool InFrustum(const Frustum *f, BoundingBox b)
{
    for (int i = 0; i < 6; i++) {
        Vector4 p = f->plane[i];
        float x = p.x >= 0.0f ? b.max.x : b.min.x, y = p.y >= 0.0f ? b.max.y : b.min.y, z = p.z >= 0.0f ? b.max.z : b.min.z;
        if (p.x * x + p.y * y + p.z * z + p.w < 0.0f) return false;
    }
    return true;
}

// The OUTLINE_MAX nearest of c[0..n) into c[0..OUTLINE_MAX) (quickselect on the distance)
static void KeepNearest(Candidate *c, int n, int keep)
{
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        float pivot = c[(lo + hi) / 2].dist;
        int i = lo, j = hi;
        while (i <= j) {
            while (c[i].dist < pivot) i++;
            while (c[j].dist > pivot) j--;
            if (i <= j) { Candidate t = c[i]; c[i] = c[j]; c[j] = t; i++; j--; }
        }
        if (keep - 1 <= j) hi = j;
        else if (keep - 1 >= i) lo = i;
        else break;
    }
}

static Color RangeColor(const Inspector *in, const MS_DrawRangeInfo *R)
{
    int mc = in->scene->materialCount;
    if (in->mode == INSPECT_DRAW)
        return R->kind == MS_DRAW_BOXES_MERGED ? (Color){ 90, 165, 255, 255 }
             : R->kind == MS_DRAW_BOXES_INSTANCED ? (Color){ 255, 165, 60, 255 } : (Color){ 120, 230, 120, 255 };
    int m = R->material;
    if (R->kind == MS_DRAW_MODELS) {
        m = -1;
        for (int s = 0; s < R->slotCount && m < 0; s++) if (R->slotMaterials[s] >= 0) m = R->slotMaterials[s];
    }
    if (in->mode == INSPECT_MATERIAL) return m >= 0 ? Hue(m, 1.0f) : Hue(mc + R->modelFile, 1.0f);  // (own materials: by file)
    int sh = m >= 0 ? in->shaderOf[m] : -1;
    return sh >= 0 ? Hue(sh, 1.0f) : NO_SHADER_COLOR;
}

// Overlay lines over an R3D frame, depth-tested against its depth, pulled 4 depth steps toward
// the camera (the designer overlay's BeginOverlay3D, designer.c)
static void BeginOverlay(Camera3D cam)
{
    BeginMode3D(cam);
    Matrix proj = rlGetMatrixProjection();
    const float bias = 4.0f / 16777216.0f * 2.0f;
    proj.m2 -= bias * proj.m3;
    proj.m6 -= bias * proj.m7;
    proj.m10 -= bias * proj.m11;
    proj.m14 -= bias * proj.m15;
    rlSetMatrixProjection(proj);
    rlEnableDepthTest();
    rlDisableDepthMask();
}

static void EndOverlay(void)
{
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
    rlDisableDepthTest();
    EndMode3D();
}

static void DrawBoxOutline(BoundingBox b, Vector3 eye, Color c)
{
    float e = fminf(0.004f + 0.0015f * BoxDistance(b, eye), 0.1f);
    Vector3 center = Vector3Scale(Vector3Add(b.min, b.max), 0.5f);
    Vector3 size = Vector3Add(Vector3Subtract(b.max, b.min), (Vector3){ 2 * e, 2 * e, 2 * e });
    DrawCubeWiresV(center, size, c);
}

// A turned box (docs/live-editing.md 11.2): its oriented box, pushed out like DrawBoxOutline;
// false for an axis-aligned one
static bool DrawTurnedBoxOutline(int i, Vector3 eye, Color c)
{
    if (!scene->boxTurned || scene->boxTurned[i] < 0) return false;
    const MS_TurnedBox *t = &scene->turnedBoxes[scene->boxTurned[i]];
    float e = fminf(0.004f + 0.0015f * BoxDistance(scene->boxes[i], eye), 0.1f);
    Vector3 q[8];
    for (int k = 0; k < 8; k++) {
        Vector3 dir = { 0 };
        dir = Vector3Add(dir, Vector3Scale(t->axis[0], (k & 1) ? t->size.x * 0.5f : -t->size.x * 0.5f));
        dir = Vector3Add(dir, Vector3Scale(t->axis[1], (k & 2) ? t->size.y * 0.5f : -t->size.y * 0.5f));
        dir = Vector3Add(dir, Vector3Scale(t->axis[2], (k & 4) ? t->size.z * 0.5f : -t->size.z * 0.5f));
        float len = Vector3Length(dir);
        q[k] = Vector3Add(t->center, len > 0.0f ? Vector3Scale(dir, (len + e) / len) : dir);
    }
    for (int k = 0; k < 8; k++)
        for (int b = 1; b < 8; b <<= 1)
            if (!(k & b)) DrawLine3D(q[k], q[k | b], c);
    return true;
}

static void DrawModelOutline(const MS_ModelInstance *m, Vector3 eye, Color c)
{
    BoundingBox lb = scene->modelFiles[m->file].bounds;
    Vector3 ctr = Vector3Scale(Vector3Add(m->bounds.min, m->bounds.max), 0.5f);
    float e = fminf(0.004f + 0.0015f * BoxDistance(m->bounds, eye), 0.1f);
    Vector3 q[8];
    for (int i = 0; i < 8; i++) {
        Vector3 p = { (i & 1) ? lb.max.x : lb.min.x, (i & 2) ? lb.max.y : lb.min.y, (i & 4) ? lb.max.z : lb.min.z };
        p = Vector3Transform(p, m->transform);
        Vector3 dir = Vector3Subtract(p, ctr);
        float len = Vector3Length(dir);
        q[i] = len > 0.0f ? Vector3Add(p, Vector3Scale(dir, e / len)) : p;
    }
    for (int i = 0; i < 8; i++)
        for (int b = 1; b < 8; b <<= 1)
            if (!(i & b)) DrawLine3D(q[i], q[i | b], c);
}

static const char *KindName(MS_DrawKind k)
{
    return k == MS_DRAW_BOXES_MERGED ? "merged cell mesh" : k == MS_DRAW_BOXES_INSTANCED ? "instanced" : "model";
}

// A label of range r centred above screen point p: false (nothing drawn) when it would overlap
// one of rects[0..*n) or leave the screen
static bool DrawLabel(const Inspector *in, int r, Vector2 p, int w, int h, Rectangle *rects, int *n, bool hovered)
{
    const MS_DrawRangeInfo *R = &in->ranges[r];
    char head[256], line[256];
    if (R->kind == MS_DRAW_MODELS) {
        const char *mat = NULL;
        for (int s = 0; s < R->slotCount && !mat; s++) if (R->slotMaterials[s] >= 0) mat = scene->materials[R->slotMaterials[s]].name;
        const char *sh = NULL;
        for (int s = 0; s < R->slotCount && !sh; s++) if (R->slotMaterials[s] >= 0) sh = scene->materials[R->slotMaterials[s]].shader;
        snprintf(head, sizeof(head), "%s", scene->modelFiles[R->modelFile].path);
        snprintf(line, sizeof(line), "%s%s%s  |  %s  |  %d instance%s  |  %lld tris  |  %s", mat ? "material " : "own materials",
                 mat ? mat : "", sh ? TextFormat("  |  %s", sh) : "", KindName(R->kind), R->objectCount,
                 R->objectCount == 1 ? "" : "s", R->triangles, R->castShadow ? "shadow" : "no shadow");
    } else {
        const char *sh = R->shader;
        snprintf(head, sizeof(head), "%s", scene->materials[R->material].name);
        snprintf(line, sizeof(line), "%s  |  %s  |  %d box%s  |  %lld tris  |  %s", sh ? sh : "no shader", KindName(R->kind),
                 R->objectCount, R->objectCount == 1 ? "" : "es", R->triangles, R->castShadow ? "shadow" : "no shadow");
    }
    int tw = (int)fmaxf((float)UiMeasureBold(head, UI_S), (float)UiMeasure(line, UI_XS)) + 16;
    int th = UiLineHeight(UI_S) + UiLineHeight(UI_XS) + 10;
    Rectangle rect = { (float)((int)p.x - tw / 2), (float)((int)p.y - th - 6), (float)tw, (float)th };
    if (hovered) {
        // the hovered one always shows: kept on screen
        if (rect.x + rect.width > w - 2) rect.x = (float)(w - 2) - rect.width;
        if (rect.x < 2) rect.x = 2;
        if (rect.y < 2) rect.y = 2;
        if (rect.y + rect.height > h - 2) rect.y = (float)(h - 2) - rect.height;
        // (beside the open panel rather than under it: rects[0] is the panel then)
        if (in->panelOpen && *n > 0 && CheckCollisionRecs(rect, rects[0]) && rects[0].x - rect.width - 4 >= 2)
            rect.x = rects[0].x - rect.width - 4;
    } else {
        if (rect.x < 2 || rect.y < 2 || rect.x + rect.width > w - 2 || rect.y + rect.height > h - 2) return false;
        for (int k = 0; k < *n; k++) if (CheckCollisionRecs(rect, rects[k])) return false;
    }
    rects[(*n)++] = rect;
    Color c = RangeColor(in, R);
    UiPanel(rect, UI_PANEL, hovered ? UI_ACCENT : UI_LINE);
    DrawRectangle((int)rect.x + 1, (int)rect.y + 4, 3, (int)rect.height - 8, c);
    UiTextBold(head, (int)rect.x + 9, (int)rect.y + 4, UI_S, UI_INK);
    UiText(line, (int)rect.x + 9, (int)rect.y + 4 + UiLineHeight(UI_S) + 1, UI_XS, UI_DIM);
    return true;
}

void InspectorDrawOverlay(const Inspector *in, Camera3D cam, int screenW, int screenH, bool labels)
{
    if (in->mode == INSPECT_OFF || !in->scene || in->scene != scene || in->rangeCount == 0) return;
    Vector3 eye = cam.position;
    Frustum fr = MakeFrustum(cam, (float)screenW / (float)screenH);
    Candidate *c = in->scratch;
    int n = 0, far = in->scratchCap;    // objects from the front, ranges' bounds from the back
    for (int r = 0; r < in->rangeCount; r++) {
        if (in->rangeHidden[r]) continue;
        const MS_DrawRangeInfo *R = &in->ranges[r];
        if (!InFrustum(&fr, R->bounds)) continue;
        float d = BoxDistance(R->bounds, eye);
        if (d > OUTLINE_DIST) { c[--far] = (Candidate){ d, r, -1 }; continue; }
        bool model = R->kind == MS_DRAW_MODELS;
        for (int k = 0; k < R->objectCount && n < far; k++) {
            int o = R->objects[k];
            BoundingBox b = model ? scene->models[o].bounds : scene->boxes[o];
            float od = BoxDistance(b, eye);
            if (od <= OUTLINE_DIST && InFrustum(&fr, b)) c[n++] = (Candidate){ od, r, o };
        }
    }
    if (n > OUTLINE_MAX) {
        KeepNearest(c, n, OUTLINE_MAX);
        n = OUTLINE_MAX;
    }

    BeginOverlay(cam);
    for (int k = far; k < in->scratchCap; k++) {
        const MS_DrawRangeInfo *R = &in->ranges[c[k].range];
        DrawBoundingBox(R->bounds, Fade(RangeColor(in, R), 0.55f));
    }
    for (int k = 0; k < n; k++) {
        const MS_DrawRangeInfo *R = &in->ranges[c[k].range];
        Color col = Fade(RangeColor(in, R), 0.9f);
        if (R->kind == MS_DRAW_MODELS) DrawModelOutline(&scene->models[c[k].obj], eye, col);
        else if (!DrawTurnedBoxOutline(c[k].obj, eye, col)) DrawBoxOutline(scene->boxes[c[k].obj], eye, col);
    }
    EndOverlay();
    if (!labels) return;

    // The hovered range: under the crosshair, or under the cursor while it is free
    Vector2 at = { screenW * 0.5f, screenH * 0.5f };
    if (!IsCursorHidden() && !IsWindowState(FLAG_WINDOW_HIDDEN)) at = GetMousePosition();
    Ray ray = GetScreenToWorldRayEx(at, cam, screenW, screenH);
    MS_Hit hit = MS_Raycast(scene, ray.position, ray.direction, 0.0f);
    int hovered = -1;
    if (hit.obj.kind == MS_OBJ_BOX) hovered = in->boxRange[hit.obj.index];
    else if (hit.obj.kind == MS_OBJ_MODEL) hovered = in->modelRange[hit.obj.index];
    if (hovered >= 0 && in->rangeHidden[hovered]) hovered = -1;

    // On-screen ranges (their bounds' centre in front of the camera and on the screen), nearest
    // first: label candidates
    Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    int m = 0;
    for (int r = 0; r < in->rangeCount; r++) {
        if (in->rangeHidden[r] || r == hovered) continue;
        const MS_DrawRangeInfo *R = &in->ranges[r];
        Vector3 ctr = Vector3Scale(Vector3Add(R->bounds.min, R->bounds.max), 0.5f);
        if (Vector3DotProduct(Vector3Subtract(ctr, eye), fwd) <= 0.1f || !InFrustum(&fr, R->bounds)) continue;
        Vector2 p = GetWorldToScreenEx(ctr, cam, screenW, screenH);
        if (p.x < 0 || p.y < 0 || p.x >= screenW || p.y >= screenH) continue;
        in->labelPick[m++] = r;
    }
    // (insertion of the nearest LABEL_COUNT * 4 by a partial selection sort: labels skipped for
    // overlapping are replaced by the next ones)
    int want = m < LABEL_COUNT * 4 ? m : LABEL_COUNT * 4;
    for (int i = 0; i < want; i++) {
        int best = i;
        float bd = BoxDistance(in->ranges[in->labelPick[i]].bounds, eye);
        for (int j = i + 1; j < m; j++) {
            float dj = BoxDistance(in->ranges[in->labelPick[j]].bounds, eye);
            if (dj < bd) { bd = dj; best = j; }
        }
        int t = in->labelPick[i];
        in->labelPick[i] = in->labelPick[best];
        in->labelPick[best] = t;
    }
    int placed = 0, shown = 0;
    if (in->panelOpen) in->labelRects[placed++] = PanelRect(screenW, screenH);     // (labels keep off the panel)
    if (hovered >= 0) DrawLabel(in, hovered, GetWorldToScreenEx(hit.point, cam, screenW, screenH), screenW, screenH, in->labelRects,
                                &placed, true);
    for (int i = 0; i < want && shown < LABEL_COUNT; i++) {
        const MS_DrawRangeInfo *R = &in->ranges[in->labelPick[i]];
        Vector3 ctr = Vector3Scale(Vector3Add(R->bounds.min, R->bounds.max), 0.5f);
        if (DrawLabel(in, in->labelPick[i], GetWorldToScreenEx(ctr, cam, screenW, screenH), screenW, screenH, in->labelRects,
                      &placed, false))
            shown++;
    }
}

// #endregion

void InspectorFree(Inspector *in)
{
    for (int k = 0; k < in->hiddenCount; k++) free(in->hidden[k]);
    free(in->hidden);
    free(in->isolated);
    free(in->hideMaterial);
    free(in->shaders);
    free(in->rangeHidden);
    free(in->ranges);
    free(in->shaderOf);
    free(in->boxRange);
    free(in->modelRange);
    free(in->itemRanges);
    free(in->itemTris);
    free(in->scratch);
    free(in->labelPick);
    free(in->labelRects);
    UiListFree(&in->list);
    memset(in, 0, sizeof(*in));
}
