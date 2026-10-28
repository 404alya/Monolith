// The id panel (docs/live-editing.md 8.1): F5 in spectator mode, in or out of the edit mode.
//
// Lists every explicit `id N` of the map (a line's prefix that names its objects for good:
// runtime/monolith_scene.h, "Entities"), one row per line in map order: the id, the line, the
// kind (the box primitive's name, model, light), how many objects the line makes, the model file
// or the material, the group. The list is the shared UiList widget (ui.h): UP / DOWN / PGUP /
// PGDN / HOME / END move, typing filters (the id, the line, the kind, the file or material and
// the group; case-insensitive). Keys of the panel:
//   ENTER          fly the spectator in front of the line's objects (their bounds fill the view)
//                  and select them; the panel closes (the flight runs with the camera keys)
//   DELETE         remove the line's `id N` prefix only: the objects stay, their entity ids become
//                  implicit (EDIT_ID_REMOVE)
//   SHIFT+DELETE   remove the line's objects (EDIT_REMOVE, as DELETE in the edit mode)
//   CTRL+I         give the designer's selection's line an id: the edit mode's number field (one
//                  line), or the next free ids at once (several lines)
//   ESC / F5       close
// Edits go through the edit mode (EditModeApplyTo): live, undoable, saved with CTRL+S, with the
// warning when a request is open on the map. The panel takes every key while open and frees the
// cursor (as the settings panel does). The rows are rebuilt from the scene's entities when
// scene->version changes (heap arrays that only grow, only while the panel is open); drawing
// allocates nothing.
#include "idpanel.h"
#include <ctype.h>
#include <math.h>

static bool Shift(void) { return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }
static bool Ctrl(void) { return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL); }

// #region The items

// The statement keyword of a map line after its prefixes (`id n`, `repeat n dx dy dz [from k]`,
// `at x y z`), into out (cut to outSize - 1); "" when there is none
static void LineKeyword(int line, char *out, int outSize)
{
    out[0] = '\0';
    int len;
    const char *s = MS_LineText(scene, line, &len);
    if (!s) return;
    int p = 0, skip = 0;
    if (len >= 3 && memcmp(s, "\xEF\xBB\xBF", 3) == 0) p = 3;
    for (;;) {
        while (p < len && (s[p] == ' ' || s[p] == '\t')) p++;
        if (p >= len || s[p] == '#' || s[p] == '"') return;
        int e = p;
        while (e < len && s[e] != ' ' && s[e] != '\t' && s[e] != '#' && s[e] != '"') e++;
        int n = e - p;
        if (skip > 0) skip--;
        else if (n == 2 && memcmp(s + p, "id", 2) == 0) skip = 1;
        else if (n == 6 && memcmp(s + p, "repeat", 6) == 0) skip = 4;
        else if (n == 4 && memcmp(s + p, "from", 4) == 0) skip = 1;
        else if (n == 2 && memcmp(s + p, "at", 2) == 0) skip = 3;
        else {
            snprintf(out, (size_t)outSize, "%.*s", n, s + p);
            return;
        }
        p = e;
    }
}

static void Rebuild(IdPanel *p)
{
    p->itemCount = 0;
    p->scene = scene;
    p->version = scene->version;
    for (int e = 0; e < scene->entityCount; ) {
        int prim = scene->entitySources[e].prim, end = e + 1;
        while (end < scene->entityCount && scene->entitySources[end].prim == prim) end++;
        MS_EntityId id = scene->entityIds[e];
        if (prim >= 0 && MS_ENTITY_EXPLICIT(id)) {
            IdEntry *it = MS_PUSH(p->items, p->itemCount, p->itemCap);
            if (!it) break;     // (out of memory: the list stops there)
            MS_ObjRef o = scene->entityObjects[e];
            const MS_Ref *r = MS_ObjectRef(scene, o);
            *it = (IdEntry){ .id = (int)id.stmt, .line = scene->entitySources[e].line, .stmt = prim, .firstEntity = e,
                             .objects = end - e, .group = r && r->group >= 0 && r->group < scene->groupCount ? scene->groups[r->group] : NULL };
            if (o.kind == MS_OBJ_MODEL) {
                snprintf(it->kind, sizeof(it->kind), "model");
                int f = scene->models[o.index].file;
                it->what = f >= 0 && f < scene->modelFileCount ? scene->modelFiles[f].path : NULL;
            } else if (o.kind == MS_OBJ_LIGHT) {
                snprintf(it->kind, sizeof(it->kind), "light");
                R3D_LightType t = scene->lights[o.index].light.type;
                it->what = t == R3D_LIGHT_DIR ? "directional" : t == R3D_LIGHT_SPOT ? "spot" : "point";
            } else {
                LineKeyword(it->line, it->kind, (int)sizeof(it->kind));
                if (!it->kind[0]) snprintf(it->kind, sizeof(it->kind), "box");      // (a compiled map has no text)
                int mat = r ? r->material : -1;
                it->what = mat >= 0 && mat < scene->materialCount ? scene->materials[mat].name : NULL;
            }
            it->bounds = MS_ObjectBounds(scene, o);
            for (int k = e + 1; k < end; k++) {
                BoundingBox b = MS_ObjectBounds(scene, scene->entityObjects[k]);
                it->bounds.min = Vector3Min(it->bounds.min, b.min);
                it->bounds.max = Vector3Max(it->bounds.max, b.max);
            }
        }
        e = end;
    }
    p->list.dirty = true;
}

// Case-insensitive substring
static bool Contains(const char *s, const char *needle)
{
    if (!s) return false;
    for (; *s; s++) {
        int k = 0;
        while (needle[k] && s[k] && tolower((unsigned char)s[k]) == tolower((unsigned char)needle[k])) k++;
        if (!needle[k]) return true;
    }
    return false;
}

static bool MatchItem(int item, const char *filter, void *user)
{
    const IdPanel *p = user;
    const IdEntry *it = &p->items[item];
    char num[16];
    snprintf(num, sizeof(num), "%d", it->id);
    if (Contains(num, filter)) return true;
    snprintf(num, sizeof(num), "%d", it->line);
    return Contains(num, filter) || Contains(it->kind, filter) || Contains(it->what, filter) || Contains(it->group, filter);
}

// The items describe the current scene (after any reload, live edit or renderer restart)
static void Refresh(IdPanel *p)
{
    if (p->scene != scene || p->version != scene->version) Rebuild(p);
    if (p->list.dirty) UiListRefilter(&p->list, p->itemCount, MatchItem, p);
}

// #endregion

// #region Open, close, keys

void IdPanelOpen(IdPanel *p, const char *filter)
{
    p->open = true;
    UiListClear(&p->list);
    for (const char *c = filter ? filter : ""; *c; c++) {
        if (!MS_RESERVE(p->list.filter, p->list.filterCap, p->list.filterLen + 2)) break;
        p->list.filter[p->list.filterLen++] = *c;
        p->list.filter[p->list.filterLen] = '\0';
    }
    p->scene = NULL;        // (rebuilt on first use)
    while (GetCharPressed()) {}     // (nothing typed before it opened goes into the filter)
    if (p->interactive) EnableCursor();
}

void IdPanelClose(IdPanel *p)
{
    if (!p->open) return;
    p->open = false;
    p->freezeFrames = 2;    // GLFW can report one big mouse jump when the cursor is captured again
    if (p->interactive) DisableCursor();
}

bool IdPanelCapturesInput(const IdPanel *p) { return p->open || p->freezeFrames > 0; }

// The selected entry's objects into p->objs; their count (0: none, or out of memory)
static int EntryObjects(IdPanel *p, const IdEntry *it)
{
    if (!MS_RESERVE(p->objs, p->objCap, it->objects > 0 ? it->objects : 1)) return 0;
    for (int k = 0; k < it->objects; k++) p->objs[k] = scene->entityObjects[it->firstEntity + k];
    return it->objects;
}

// In front of the bounds: from the side the camera is on (horizontally), looking down 20
// degrees, at the distance where a sphere around the bounds fills the vertical view
static void FlyTo(Spectator *spec, BoundingBox b)
{
    Vector3 c = Vector3Scale(Vector3Add(b.min, b.max), 0.5f);
    float radius = fmaxf(0.5f * Vector3Distance(b.min, b.max), 0.5f);
    float halfFov = 0.5f * SpectatorCamera(spec).fovy * DEG2RAD;
    float dist = fmaxf(radius / sinf(halfFov) * 1.1f, 2.0f);
    Vector3 to = Vector3Subtract(c, spec->pos);
    float yaw = fabsf(to.x) + fabsf(to.z) > 1e-3f ? atan2f(to.x, to.z) : spec->yaw;
    float pitch = -20.0f * DEG2RAD;
    Vector3 eye = Vector3Subtract(c, Vector3Scale(LookDir(yaw, pitch), dist));
    SpectatorFlyToPose(spec, eye, yaw, pitch);
}

EditModeResult IdPanelUpdate(IdPanel *p, EditMode *m, Designer *d, Spectator *spec)
{
    if (!p->open) {
        if (p->freezeFrames > 0) p->freezeFrames--;
        return EDITMODE_NONE;
    }
    // The edit mode's number field (CTRL+I) takes the keys while it is open
    if (EditModeCapturesInput(m)) return EditModeIdFieldUpdate(m, d);
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_F5)) { IdPanelClose(p); return EDITMODE_NONE; }
    Refresh(p);
    bool ctrl = Ctrl(), shift = Shift();
    if (ctrl) while (GetCharPressed() > 0) { }      // (CTRL+I is no 'i' in the filter)
    UiListKeys(&p->list, p->visibleRows);
    if (p->list.dirty) UiListRefilter(&p->list, p->itemCount, MatchItem, p);
    if (ctrl && IsKeyPressed(KEY_I)) return EditModeIdKey(m, d);
    int item = UiListSelected(&p->list);
    if (item < 0) return EDITMODE_NONE;
    const IdEntry *it = &p->items[item];
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        BoundingBox b = it->bounds;
        int n = EntryObjects(p, it);
        DesignerClearSelection(d);
        for (int k = 0; k < n; k++) DesignerSelectObject(d, p->objs[k]);     // (lights can't be selected)
        FlyTo(spec, b);
        SpectatorMessage(spec, TextFormat("id %d: line %d (%d object%s)", it->id, it->line, it->objects, it->objects == 1 ? "" : "s"));
        IdPanelClose(p);
        return EDITMODE_NONE;
    }
    if (IsKeyPressed(KEY_DELETE)) {
        int n = EntryObjects(p, it);
        if (n == 0) return EDITMODE_NONE;
        // (the warning's "press again" key: DELETE and SHIFT+DELETE are different edits)
        EditOp op = { .kind = shift ? EDIT_REMOVE : EDIT_ID_REMOVE };
        return EditModeApplyTo(m, d, shift ? -KEY_DELETE : KEY_DELETE, p->objs, n, &op);
    }
    return EDITMODE_NONE;
}

// #endregion

// #region Drawing

#define HINTS "{UP}/{DOWN} select|{ENTER} fly to, select|{DEL} remove the id|{SHIFT+DEL} remove the objects|" \
              "{CTRL+I} id for the selection|type to filter|{ESC} close"

// Text cut by characters until it fits in room px ("..." added); drawn at x
static void FitText(const char *s, int x, int y, int room, int size, Color c)
{
    if (room <= 0 || !s || !s[0]) return;
    if (UiMeasure(s, size) <= room) { UiText(s, x, y, size, c); return; }
    char cut[160];
    int n = (int)strlen(s);
    if (n > (int)sizeof(cut) - 4) n = (int)sizeof(cut) - 4;
    for (; n > 0; n--) {
        snprintf(cut, sizeof(cut), "%.*s...", n, s);
        if (UiMeasure(cut, size) <= room) break;
    }
    if (n > 0) UiText(cut, x, y, size, c);
}

static void DrawItemRow(int item, Rectangle row, bool selected, void *user)
{
    const IdPanel *p = user;
    const IdEntry *it = &p->items[item];
    int ty = (int)row.y + ((int)row.height - UiLineHeight(UI_M)) / 2;
    int sy = (int)row.y + ((int)row.height - UiLineHeight(UI_S)) / 2;
    int x = (int)row.x + 12, right = (int)(row.x + row.width) - 10;
    Color ink = selected ? UI_INK : UI_TEXT;
    UiTextBold(TextFormat("id %d", it->id), x, ty, UI_M, UI_ACCENT);
    x += 96;
    UiText(TextFormat("line %d", it->line), x, sy, UI_S, UI_DIM);
    x += 86;
    FitText(it->kind, x, sy, 62, UI_S, ink);
    x += 68;
    const char *count = TextFormat("%d obj", it->objects);
    UiText(count, x + 52 - UiMeasure(count, UI_S), sy, UI_S, UI_DIM);
    x += 66;
    // the group on the right (at most a third of what is left), the file or material before it
    int room = right - x;
    int gw = 0;
    if (it->group) {
        gw = UiMeasure(it->group, UI_S);
        if (gw > room / 3) gw = room / 3;
        FitText(it->group, right - gw, sy, gw, UI_S, UI_DIM);
        gw += 14;
    }
    FitText(it->what, x, ty, room - gw, UI_M, ink);
}

void IdPanelDraw(IdPanel *p, int screenW, int screenH)
{
    if (!p->open) return;
    Refresh(p);
    // On the right like the inspector panel, wide enough for the hints; the bottom above the help
    // bars and toasts
    float w = fminf(fmaxf(760.0f, (float)UiHintsWidth(HINTS, UI_XS) + 40.0f), (float)screenW - 40.0f);
    float h = fminf(640.0f, (float)screenH - 260.0f);
    if (h < 200.0f) h = fminf(200.0f, (float)screenH - 20.0f);
    float y = (float)screenH - 90.0f - h;
    Rectangle r = { (float)screenW - w - 20.0f, y > 10.0f ? y : 10.0f, w, h };
    p->visibleRows = UiListDraw(&p->list, r, "Ids of the map", 24, DrawItemRow, p, HINTS);
}

void IdPanelFree(IdPanel *p)
{
    UiListFree(&p->list);
    free(p->items);
    free(p->objs);
    memset(p, 0, sizeof(*p));
}

// #endregion
