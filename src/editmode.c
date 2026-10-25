// Edit mode -- M in spectator mode: move, turn, remove, duplicate and add objects by hand
//
// The edits act on the designer's selection (LMB, SHIFT+LMB, CTRL+LMB for a group) and are
// written straight into the map file as minimal text edits (mapedit.c), which the game then
// reloads; the selection follows the edited objects through their entity ids. No request is
// made. Keys in the mode:
//   arrows / PGUP / PGDN   move one snap step along the world axis nearest to the view (as
//                          volume mode); the first selected object lands on the snap grid,
//                          the others keep their offsets. CTRL: exactly one step, no grid
//   N                      snap step 0.1 / 0.25 / 0.5 / 1 / 2 m (shared with volume mode)
//   R / SHIFT+R            turn: models +-15 degrees of yaw (CTRL: 1), boxes +-90 (box and block
//                          lines: the footprint turns about its centre; stairs and waffles can't)
//   DELETE                 remove
//   CTRL+D                 duplicate, the copies one footprint further along the view's right
//   [ ]                    pick the model to add (the package's models/ files listed in ASSETS.md)
//   I                      add it at the crosshair (origin bottom), after the line of the object
//                          under it, in its group
//   CTRL+I                 give the selection's line an id: with objects of one line selected, the
//                          id panel (a number field, prefilled with the line's id or the next free
//                          one; digits, BACKSPACE, ENTER sets it, ESC cancels; empty = the next
//                          free id); with objects of several lines, each line without an id gets
//                          the next free one at once. Only the `id n` prefix changes (mapedit.c)
//   M / ESC                leave the mode (V and C leave it too)
// CTRL+Z / CTRL+SHIFT+Z (or CTRL+Y) undo / redo manual edits anywhere in spectator mode. When
// a request of this game run is queued or running on the map, an edit first warns; the same
// key again within 3 s edits anyway (until another request is sent).
#include "editmode.h"
#include "launcher.h"
#include "dirscan.h"
#include <ctype.h>
#include <math.h>

#define EDIT_COLOR      (Color){ 255, 214, 90, 255 }
#define TURN_MODEL_DEG  15.0
#define TURN_FINE_DEG   1.0
#define WARN_TIME       3.0f

static bool KeyRepeat(int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); }
static bool Ctrl(void) { return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL); }
static bool Shift(void) { return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }

static void Say(EditMode *m, bool error, const char *text)
{
    snprintf(m->message, sizeof(m->message), "%s", text);
    m->messageTime = error ? 8.0f : 5.0f;
    m->messageError = error;
}

// ======================================================================================
// #region The package's models (models/, recorded in ASSETS.md)
// ======================================================================================

typedef struct { EditMode *m; const char *assets; } ModelScan;

static bool HasModelExt(const char *rel)
{
    const char *dot = strrchr(rel, '.');
    if (!dot) return false;
    char ext[8];
    int n = 0;
    for (const char *p = dot + 1; *p && n < 7; p++) ext[n++] = (char)tolower((unsigned char)*p);
    ext[n] = '\0';
    return !strcmp(ext, "glb") || !strcmp(ext, "gltf") || !strcmp(ext, "obj") || !strcmp(ext, "fbx") || !strcmp(ext, "dae");
}

static void ScanModel(const char *path, const char *rel, void *user)
{
    (void)path;
    ModelScan *s = user;
    if (!HasModelExt(rel)) return;
    size_t size = strlen(rel) + 8;
    char *full = malloc(size);
    if (!full) return;
    snprintf(full, size, "models/%s", rel);
    // Only files with a licence row (assets-and-licences): ASSETS.md names them
    if (!s->assets || !strstr(s->assets, full)) { free(full); return; }
    char **slot = MS_PUSH(s->m->models, s->m->modelCount, s->m->modelCap);
    if (!slot) { free(full); return; }
    *slot = full;
}

static int CompareStrings(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void FreeModels(EditMode *m)
{
    for (int i = 0; i < m->modelCount; i++) free(m->models[i]);
    m->modelCount = 0;
}

static void ScanModels(EditMode *m)
{
    const char *keep = m->modelPick < m->modelCount ? m->models[m->modelPick] : NULL;
    char *kept = keep ? MS_StrDup(keep) : NULL;
    FreeModels(m);
    char path[MS_PATH_MAX + 16];
    snprintf(path, sizeof(path), "%sASSETS.md", scene->root);
    char *assets = FileExists(path) ? LoadFileText(path) : NULL;
    ModelScan s = { m, assets };
    snprintf(path, sizeof(path), "%smodels", scene->root);
    DirScanStats stats;
    if (DirectoryExists(path)) DirScan(path, true, ScanModel, &s, &stats);
    if (assets) UnloadFileText(assets);
    qsort(m->models, (size_t)m->modelCount, sizeof(char *), CompareStrings);
    m->modelPick = 0;
    for (int i = 0; kept && i < m->modelCount; i++) if (!strcmp(m->models[i], kept)) m->modelPick = i;
    free(kept);
}

// #endregion

// ======================================================================================
// #region Edits
// ======================================================================================

// A request on this map: the first press warns, the same key again within WARN_TIME edits
static bool Confirmed(EditMode *m, int key)
{
    int newest, open = LauncherOpenOnMap(scene->path, &newest);
    if (open == 0 || (open == m->ackCount && newest == m->ackNewest)) return true;
    if (m->warnTime > 0.0f && m->warnKey == key) {
        m->ackCount = open;
        m->ackNewest = newest;
        m->warnTime = 0.0f;
        return true;
    }
    m->warnKey = key;
    m->warnTime = WARN_TIME;
    Say(m, true, TextFormat("request [%d]%s is queued or running on this map: its session may change the same lines. "
                            "Press again to edit anyway", newest, open > 1 ? TextFormat(" (+%d more)", open - 1) : ""));
    return false;
}

// After an edit / undo / redo: the selection becomes the edited objects
static EditModeResult Done(EditMode *m, Designer *d, EditResult r, MS_EntityId *ids, int count, const char *msg)
{
    if (r != EDIT_DONE) {
        Say(m, true, msg);
        free(ids);
        return EDITMODE_NONE;
    }
    DesignerClearSelection(d);
    DesignerSync(d);
    for (int i = 0; i < count; i++) {
        int e = MS_FindEntity(scene, ids[i]);
        if (e >= 0) DesignerSelectObject(d, scene->entityObjects[e]);
    }
    free(ids);
    Say(m, false, msg);
    TraceLog(LOG_INFO, "monolith: edit: %s", msg);
    return EDITMODE_RELOADED;
}

static EditModeResult Apply(EditMode *m, Designer *d, int key, const EditOp *op)
{
    if (op->kind != EDIT_ADD && d->selectedCount == 0) {
        Say(m, true, "nothing selected (LMB selects, SHIFT+LMB adds, CTRL+LMB a group)");
        return EDITMODE_NONE;
    }
    if (!Confirmed(m, key)) return EDITMODE_NONE;
    MS_EntityId *ids;
    int count;
    char msg[512];
    EditResult r = MapEditApply(&m->ed, d->selected, d->selectedCount, op, &ids, &count, msg, sizeof(msg));
    return Done(m, d, r, ids, count, msg);
}

// The world axis nearest to the view's forward (horizontal), and its right
static void ViewAxes(const Designer *d, Vector3 *fwd, Vector3 *right)
{
    Vector3 v = d->viewFwd;
    *fwd = fabsf(v.x) > fabsf(v.z) ? (Vector3){ v.x > 0 ? 1.0f : -1.0f, 0, 0 } : (Vector3){ 0, 0, v.z >= 0 ? 1.0f : -1.0f };
    *right = (Vector3){ -fwd->z, 0.0f, fwd->x };   // as the spectator's right = (-cos yaw, 0, sin yaw)
}

static int AxisOf(Vector3 dir) { return dir.x != 0.0f ? 0 : dir.y != 0.0f ? 1 : 2; }

static EditModeResult MoveStep(EditMode *m, Designer *d, int key, Vector3 dir)
{
    if (d->selectedCount == 0) return Apply(m, d, key, &(EditOp){ .kind = EDIT_MOVE });
    int a = AxisOf(dir);
    double sign = dir.x + dir.y + dir.z, s = DesignerSnap(d), anchor[3];
    MapEditAnchor(d->selected[0], anchor);
    double target = anchor[a] + sign * s;
    if (!Ctrl()) {
        target = round(target / s) * s;
        if ((target - anchor[a]) * sign < 1e-6) target += sign * s;     // snapped back onto the anchor
    }
    EditOp op = { .kind = EDIT_MOVE };
    op.delta[a] = round((target - anchor[a]) * 1e6) / 1e6;
    return Apply(m, d, key, &op);
}

static EditModeResult Turn(EditMode *m, Designer *d, int key)
{
    int boxes = 0;
    for (int i = 0; i < d->selectedCount; i++) boxes += d->selected[i].kind == MS_OBJ_BOX;
    double step = boxes > 0 && boxes == d->selectedCount ? 90.0 : Ctrl() ? TURN_FINE_DEG : TURN_MODEL_DEG;
    return Apply(m, d, key, &(EditOp){ .kind = EDIT_ROTATE, .degrees = Shift() ? -step : step });
}

// The copies go one footprint of the selection (whole snap steps, at least one) to the right
static EditModeResult Duplicate(EditMode *m, Designer *d, int key)
{
    if (d->selectedCount == 0) return Apply(m, d, key, &(EditOp){ .kind = EDIT_DUPLICATE });
    Vector3 fwd, right;
    ViewAxes(d, &fwd, &right);
    int a = AxisOf(right);
    float lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i < d->selectedCount; i++) {
        BoundingBox b = MS_ObjectBounds(scene, d->selected[i]);
        lo = fminf(lo, ((const float *)&b.min)[a]);
        hi = fmaxf(hi, ((const float *)&b.max)[a]);
    }
    double s = DesignerSnap(d), width = ceil(fmax(hi - lo, 0.0) / s - 1e-6) * s;
    EditOp op = { .kind = EDIT_DUPLICATE };
    op.delta[a] = (width > s ? width : s) * (right.x + right.z);
    op.delta[a] = round(op.delta[a] * 1e6) / 1e6;
    return Apply(m, d, key, &op);
}

static EditModeResult AddModel(EditMode *m, Designer *d, int key)
{
    if (m->modelCount == 0) {
        Say(m, true, "no model to add: the package's models/ has no file listed in ASSETS.md (new assets: a request)");
        return EDITMODE_NONE;
    }
    if (ObjIsNone(d->hit.obj)) {
        Say(m, true, "no surface under the crosshair");
        return EDITMODE_NONE;
    }
    // Snapped on the axes along the hit face (as volume mode), CTRL: exact
    Vector3 p = d->hit.point, n = d->hit.normal;
    float s = DesignerSnap(d);
    if (!Ctrl()) {
        if (fabsf(n.x) < 0.9f) p.x = roundf(p.x / s) * s;
        if (fabsf(n.y) < 0.9f) p.y = roundf(p.y / s) * s;
        if (fabsf(n.z) < 0.9f) p.z = roundf(p.z / s) * s;
    }
    EditOp op = { .kind = EDIT_ADD, .modelPath = m->models[m->modelPick], .anchor = d->hit.obj,
                  .pos = { round(p.x * 1e4) / 1e4, round(p.y * 1e4) / 1e4, round(p.z * 1e4) / 1e4 } };
    return Apply(m, d, key, &op);
}

// CTRL+I: the panel for one line; several lines get the next free ids at once
static EditModeResult IdKey(EditMode *m, Designer *d)
{
    if (d->selectedCount == 0) {
        Say(m, true, "nothing selected (LMB selects, SHIFT+LMB adds, CTRL+LMB a group)");
        return EDITMODE_NONE;
    }
    int first = MS_ObjectEntity(scene, d->selected[0]);
    for (int i = 1; i < d->selectedCount && first >= 0; i++) {
        int e = MS_ObjectEntity(scene, d->selected[i]);
        if (e >= 0 && scene->entitySources[e].prim != scene->entitySources[first].prim)
            return Apply(m, d, KEY_I, &(EditOp){ .kind = EDIT_ID });
    }
    if (first < 0) return EDITMODE_NONE;
    EditModeOpenIdPanel(m, d, NULL);
    return EDITMODE_NONE;
}

void EditModeOpenIdPanel(EditMode *m, Designer *d, const char *text)
{
    int e = d->selectedCount > 0 ? MS_ObjectEntity(scene, d->selected[0]) : -1;
    if (e < 0) return;
    MS_EntityId id = scene->entityIds[e];
    m->idOpen = true;
    m->idLine = scene->entitySources[e].line;
    m->idHas = MS_ENTITY_EXPLICIT(id) ? (int)id.stmt : 0;
    m->idNext = MapEditNextId();
    if (text) snprintf(m->idText, sizeof(m->idText), "%s", text);
    else snprintf(m->idText, sizeof(m->idText), "%d", m->idHas ? m->idHas : m->idNext);
    while (GetCharPressed()) {}     // (nothing typed before the panel opened goes into it)
}

static EditModeResult IdPanel(EditMode *m, Designer *d)
{
    if (IsKeyPressed(KEY_ESCAPE)) { m->idOpen = false; return EDITMODE_NONE; }
    size_t n = strlen(m->idText);
    if (KeyRepeat(KEY_BACKSPACE) && n > 0) m->idText[--n] = '\0';
    for (int c = GetCharPressed(); c; c = GetCharPressed())
        if (c >= '0' && c <= '9' && n < 10 && !(n == 0 && c == '0')) { m->idText[n++] = (char)c; m->idText[n] = '\0'; }
    if (!IsKeyPressed(KEY_ENTER) && !IsKeyPressed(KEY_KP_ENTER)) return EDITMODE_NONE;
    long v = n ? strtol(m->idText, NULL, 10) : 0;
    if (v > 2147483647L) { Say(m, true, "an id is a number from 1 to 2147483647"); return EDITMODE_NONE; }
    EditModeResult r = Apply(m, d, KEY_ENTER, &(EditOp){ .kind = EDIT_ID, .id = (int)v });
    if (r == EDITMODE_RELOADED) m->idOpen = false;      // (refused: stays open with the reason shown)
    return r;
}

static EditModeResult History(EditMode *m, Designer *d, bool undo)
{
    if (!Confirmed(m, undo ? KEY_Z : KEY_Y)) return EDITMODE_NONE;
    MS_EntityId *ids;
    int count;
    char msg[512];
    EditResult r = undo ? MapEditUndo(&m->ed, &ids, &count, msg, sizeof(msg)) : MapEditRedo(&m->ed, &ids, &count, msg, sizeof(msg));
    return Done(m, d, r, ids, count, msg);
}

// #endregion

// ======================================================================================
// #region Update, HUD
// ======================================================================================

void EditModeOpen(EditMode *m, Designer *d)
{
    if (d->volumeMode) DesignerVolumeModeEnd(d);
    m->active = true;
    ScanModels(m);
}

bool EditModeTakesArrows(const EditMode *m) { return m->active; }

bool EditModeCapturesInput(const EditMode *m) { return m->idOpen; }

void EditModeSync(EditMode *m)
{
    char msg[256];
    if (MapEditSync(&m->ed, msg, sizeof(msg))) Say(m, true, msg);
}

EditModeResult EditModeUpdate(EditMode *m, Designer *d, float dt)
{
    if (m->messageTime > 0.0f) m->messageTime -= dt;
    if (m->warnTime > 0.0f) m->warnTime -= dt;
    if (m->active && (d->volumeMode || d->snipping)) m->active = false;    // V / C took over
    if (!m->active) m->idOpen = false;
    if (m->idOpen) return IdPanel(m, d);

    bool ctrl = Ctrl(), shift = Shift();
    if (ctrl && IsKeyPressed(KEY_Z)) return History(m, d, !shift);
    if (ctrl && IsKeyPressed(KEY_Y)) return History(m, d, false);

    if (IsKeyPressed(KEY_M) || (m->active && IsKeyPressed(KEY_ESCAPE))) {
        if (m->active) m->active = false;
        else EditModeOpen(m, d);
        return EDITMODE_NONE;
    }
    if (!m->active) return EDITMODE_NONE;

    if (IsKeyPressed(KEY_N)) Say(m, false, TextFormat("snap %g m", DesignerCycleSnap(d)));
    if (m->modelCount > 0 && IsKeyPressed(KEY_LEFT_BRACKET)) m->modelPick = (m->modelPick + m->modelCount - 1) % m->modelCount;
    if (m->modelCount > 0 && IsKeyPressed(KEY_RIGHT_BRACKET)) m->modelPick = (m->modelPick + 1) % m->modelCount;

    Vector3 fwd, right;
    ViewAxes(d, &fwd, &right);
    static const int KEYS[6] = { KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_PAGE_UP, KEY_PAGE_DOWN };
    const Vector3 dirs[6] = { fwd, Vector3Negate(fwd), right, Vector3Negate(right), { 0, 1, 0 }, { 0, -1, 0 } };
    for (int k = 0; k < 6; k++)
        if (KeyRepeat(KEYS[k])) return MoveStep(m, d, KEYS[k], dirs[k]);
    if (IsKeyPressed(KEY_R)) return Turn(m, d, KEY_R);
    if (IsKeyPressed(KEY_DELETE)) return Apply(m, d, KEY_DELETE, &(EditOp){ .kind = EDIT_REMOVE });
    if (ctrl && IsKeyPressed(KEY_D)) return Duplicate(m, d, KEY_D);
    if (ctrl && IsKeyPressed(KEY_I)) return IdKey(m, d);
    if (IsKeyPressed(KEY_I)) return AddModel(m, d, KEY_I);
    return EDITMODE_NONE;
}

static void HudText(const char *text, int x, int y, Color c)
{
    DrawText(text, x + 1, y + 1, 16, (Color){ 0, 0, 0, (unsigned char)(c.a / 2) });
    DrawText(text, x, y, 16, c);
}

int EditModeDrawHud(const EditMode *m, const Designer *d, int x, int y)
{
    int y0 = y;
    Color dim = { 230, 230, 230, 130 };
    if (m->active) {
        HudText(TextFormat("EDIT MODE (M)  |  snap %g m (N, CTRL: none)  |  arrows / PGUP / PGDN move  |  R / SHIFT+R turn "
                           "(models 15 deg, CTRL 1; boxes 90)  |  DEL remove  |  CTRL+D duplicate  |  [ ] I add model  |  CTRL+I id",
                           DesignerSnap(d)), x, y, Fade(EDIT_COLOR, 0.95f));
        y += 20;
        char sel[160] = "nothing selected";
        if (d->selectedCount > 0) {
            char where[128];
            MapEditDescribe(d->selected[0], where, sizeof(where));
            snprintf(sel, sizeof(sel), "%d selected, first: %s %s", d->selectedCount,
                     d->selected[0].kind == MS_OBJ_MODEL ? "model" : d->selected[0].kind == MS_OBJ_LIGHT ? "light" : "box", where);
        }
        HudText(TextFormat("%s  |  add: %s%s  |  CTRL+Z undo (%d)  CTRL+SHIFT+Z redo (%d)", sel,
                           m->modelCount ? m->models[m->modelPick] : "(no models in ASSETS.md)",
                           m->modelCount ? TextFormat(" (%d of %d)", m->modelPick + 1, m->modelCount) : "",
                           m->ed.undoCount, m->ed.redoCount), x, y, dim);
        y += 20;
    }
    if (m->idOpen) {
        // The id panel: a boxed number field under the mode's lines
        y += 8;
        const char *title = TextFormat("ID for line %d  (now: %s)", m->idLine,
                                       m->idHas ? TextFormat("id %d", m->idHas) : "implicit, changes when the line does");
        const char *hint = TextFormat("ENTER set  |  ESC cancel  |  empty: the next free id (%d)  |  1..2147483647, unique in the map",
                                      m->idNext);
        int w = 24 + (int)fmaxf((float)MeasureText(title, 16), (float)MeasureText(hint, 16));
        DrawRectangle(x - 6, y - 4, w, 74, (Color){ 20, 20, 24, 210 });
        DrawRectangleLines(x - 6, y - 4, w, 74, Fade(EDIT_COLOR, 0.9f));
        HudText(title, x + 6, y + 2, EDIT_COLOR);
        DrawRectangle(x + 6, y + 24, 140, 22, (Color){ 0, 0, 0, 200 });
        HudText(TextFormat("%s_", m->idText), x + 12, y + 27, RAYWHITE);
        HudText(hint, x + 6, y + 50, Fade(RAYWHITE, 0.7f));
        y += 80;
    }
    if (m->messageTime > 0.0f) {
        Color c = m->messageError ? (Color){ 255, 110, 90, 255 } : EDIT_COLOR;
        HudText(m->message, x, y, Fade(c, fminf(m->messageTime, 1.0f)));
        y += 20;
    }
    return y - y0;
}

void EditModeFree(EditMode *m)
{
    FreeModels(m);
    free(m->models);
    MapEditFree(&m->ed);
    memset(m, 0, sizeof(*m));
}

// #endregion
