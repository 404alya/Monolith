// Edit mode -- M in spectator mode: move, turn, remove, duplicate and add objects by hand
//
// The edits act on the designer's selection (LMB, SHIFT+LMB, CTRL+LMB for a group, under the
// cursor: in the mode the camera flies only while RMB is held, main.c). Each one is
// a minimal text edit (mapedit.c) of the working copy, the map text the scene shows, and goes
// live at once (MS_EditSetText: only what changed is rebuilt, nothing is read from disk); the
// selection follows the edited objects through their entity ids. CTRL+S writes the working copy
// to the map file; in auto save mode (settings: Editor, edit_save = auto) every edit, undo and
// redo is written at once. The HUD shows "N unsaved edits" in every spectator mode. When the
// file changes on disk meanwhile (a request finished, another editor), the unsaved edits are
// applied again on top of it (EditModeSync); those that no longer apply are listed in red until
// CTRL+SHIFT+DELETE. No request is made. Keys in the mode:
//   arrows / PGUP / PGDN   move one snap step along the world axis nearest to the view (as
//                          volume mode); the first selected object lands on the snap grid,
//                          the others keep their offsets. CTRL: exactly one step, no grid
//   N                      snap step 0.1 / 0.25 / 0.5 / 1 / 2 m (shared with volume mode)
//   G / R / S              the transform modal (transform.c): grab, rotate, scale the selection with
//                          the mouse or a typed value, X / Y / Z (SHIFT: plane) locks, LMB / ENTER
//                          commit one edit, RMB / ESC cancel
//   ALT+R / ALT+SHIFT+R    turn one step: models +-15 degrees of yaw (CTRL: 1), boxes +-90 (box and
//                          block lines: the footprint turns about its centre; stairs and waffles can't)
//   DELETE                 remove
//   CTRL+D                 duplicate, the copies one footprint further along the view's right
//   I                      the model picker (modelpicker.c, opened by main.c): the package's models/
//                          files listed in ASSETS.md; ENTER adds the chosen one at the point under the
//                          cursor (origin bottom, EditModeAddModel), after the line of the object
//                          there, in its group
//   CTRL+I                 give the selection's line an id: with objects of one line selected, the
//                          id panel (a number field, prefilled with the line's id or the next free
//                          one; digits, BACKSPACE, ENTER sets it, ESC cancels; empty = the next
//                          free id); with objects of several lines, each line without an id gets
//                          the next free one at once. Only the `id n` prefix changes (mapedit.c)
//   M / ESC                leave the mode (V and C leave it too); with unsaved edits the first
//                          press only says so (again within 3 s leaves; the edits stay live)
// CTRL+Z / CTRL+SHIFT+Z (or CTRL+Y) undo / redo manual edits anywhere in spectator mode, CTRL+S
// saves. When a request of this game run is queued or running on the map, the first edit after
// the last save (and a save) first warns; the same key again within 3 s goes ahead (until
// another request is sent).
#include "editmode.h"
#include "launcher.h"
#include "dirscan.h"
#include "ui.h"
#include <ctype.h>
#include <math.h>

#define EDIT_COLOR      (Color){ 255, 214, 90, 255 }
#define TURN_MODEL_DEG  15.0
#define TURN_FINE_DEG   1.0
#define WARN_TIME       3.0f

static bool KeyRepeat(int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); }
static bool Ctrl(void) { return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL); }
static bool Shift(void) { return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }
static bool Alt(void) { return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT); }

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

void EditModeScanModels(EditMode *m)
{
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
}

// #endregion

// ======================================================================================
// #region Edits
// ======================================================================================

// A request on this map: the first press warns, the same key again within WARN_TIME edits.
// Unsaved edits don't touch the file: only the first edit after the last save warns (and a save)
static bool Confirmed(EditMode *m, int key)
{
    if (key != KEY_S && !m->ed.autoSave && MapEditUnsaved(&m->ed) > 0) return true;
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
                            "Press again to %s anyway", newest, open > 1 ? TextFormat(" (+%d more)", open - 1) : "",
                            key == KEY_S ? "save" : "edit"));
    return false;
}

// The working copy starts at the first edit (the map as loaded); false for a compiled map
static bool EnsureWork(EditMode *m)
{
    return m->ed.work || MapEditBegin(&m->ed);
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
    char stats[400];
    MapEditStatsText(&m->ed.lastStats, stats, sizeof(stats));
    TraceLog(LOG_INFO, "monolith: edit: %s", msg);
    TraceLog(LOG_INFO, "monolith: live edit %s", stats);
    return EDITMODE_RELOADED;
}

static EditModeResult ApplyTo(EditMode *m, Designer *d, int key, const MS_ObjRef *sel, int selCount, const EditOp *op)
{
    if (op->kind != EDIT_ADD && selCount == 0) {
        Say(m, true, "nothing selected (LMB selects, SHIFT+LMB adds, CTRL+LMB a group)");
        return EDITMODE_NONE;
    }
    if (!Confirmed(m, key)) return EDITMODE_NONE;
    EnsureWork(m);      // (a compiled map has none: MapEditApply says why)
    MS_EntityId *ids;
    int count;
    char msg[512];
    EditResult r = MapEditApply(&m->ed, sel, selCount, op, &ids, &count, msg, sizeof(msg));
    return Done(m, d, r, ids, count, msg);
}

static EditModeResult Apply(EditMode *m, Designer *d, int key, const EditOp *op)
{
    return ApplyTo(m, d, key, d->selected, d->selectedCount, op);
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

EditModeResult EditModeAddModel(EditMode *m, Designer *d, const char *modelPath)
{
    if (!modelPath) {
        Say(m, true, "no model to add: the package's models/ has no file listed in ASSETS.md (new assets: a request)");
        return EDITMODE_NONE;
    }
    if (ObjIsNone(d->hit.obj)) {
        Say(m, true, "no surface under the cursor");
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
    EditOp op = { .kind = EDIT_ADD, .modelPath = modelPath, .anchor = d->hit.obj,
                  .pos = { round(p.x * 1e4) / 1e4, round(p.y * 1e4) / 1e4, round(p.z * 1e4) / 1e4 } };
    return Apply(m, d, KEY_ENTER, &op);
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

EditModeResult EditModeApplyTo(EditMode *m, Designer *d, int key, const MS_ObjRef *sel, int selCount, const EditOp *op)
{
    return ApplyTo(m, d, key, sel, selCount, op);
}

EditModeResult EditModeIdKey(EditMode *m, Designer *d) { return IdKey(m, d); }

EditModeResult EditModeIdFieldUpdate(EditMode *m, Designer *d) { return m->idOpen ? IdPanel(m, d) : EDITMODE_NONE; }

static EditModeResult History(EditMode *m, Designer *d, bool undo)
{
    if (!Confirmed(m, undo ? KEY_Z : KEY_Y)) return EDITMODE_NONE;
    EnsureWork(m);
    MS_EntityId *ids;
    int count;
    char msg[512];
    EditResult r = undo ? MapEditUndo(&m->ed, &ids, &count, msg, sizeof(msg)) : MapEditRedo(&m->ed, &ids, &count, msg, sizeof(msg));
    return Done(m, d, r, ids, count, msg);
}

// The transform modal's commit: one edit of the objects that follow, then the modal ends (a
// refused edit puts the preview back). A request open on the map: the modal stays open until
// LMB / ENTER comes again within 3 s (or it is cancelled)
static EditModeResult ModalCommit(EditMode *m, Designer *d, const EditOp *op)
{
    if (!Confirmed(m, KEY_ENTER)) return EDITMODE_NONE;
    const MS_ObjRef *sel;
    int n = TransformCommitObjects(&m->modal, &sel);
    EditModeResult r = ApplyTo(m, d, KEY_ENTER, sel, n, op);
    TransformEnd(&m->modal, d);
    if (r != EDITMODE_RELOADED) DesignerSync(d);
    return r;
}

static EditModeResult ModalUpdate(EditMode *m, Designer *d, Camera3D cam)
{
    EditOp op;
    TransformResult r = TransformUpdate(&m->modal, d, cam, &op);
    if (r == TRANSFORM_CANCELLED) {
        // (LMB / ENTER at a value that changes nothing, e.g. a square box turned 90 deg: say so)
        const char *note = TransformCancelNote(&m->modal);
        Say(m, note != NULL, note ? note : "transform cancelled: everything is back");
        return EDITMODE_NONE;
    }
    if (r == TRANSFORM_COMMIT) return ModalCommit(m, d, &op);
    return EDITMODE_NONE;
}

static EditModeResult ModalStart(EditMode *m, Designer *d, TransformKind kind)
{
    char msg[256];
    if (!TransformBegin(&m->modal, d, kind, msg, sizeof(msg))) Say(m, true, msg);
    return EDITMODE_NONE;
}

bool EditModeModalRunning(const EditMode *m) { return TransformRunning(&m->modal); }

bool EditModeOpenModal(EditMode *m, Designer *d, Camera3D cam, const char *spec)
{
    char msg[256];
    if (!m->active) EditModeOpen(m, d);
    if (TransformOpenScripted(&m->modal, d, cam, spec, msg, sizeof(msg))) return true;
    Say(m, true, msg);
    TraceLog(LOG_WARNING, "monolith: --edit-modal: %s", msg);
    return false;
}

EditModeResult EditModeFinishModal(EditMode *m, Designer *d, bool commit)
{
    EditOp op;
    TransformResult r = TransformFinish(&m->modal, d, commit, &op);
    if (r != TRANSFORM_COMMIT) {
        const char *note = TransformCancelNote(&m->modal);
        if (note) Say(m, true, note);
        TraceLog(LOG_INFO, "monolith: --edit-modal: cancelled (everything back)%s%s", note ? ": " : "", note ? note : "");
        return EDITMODE_NONE;
    }
    EditModeResult res = ModalCommit(m, d, &op);
    TraceLog(LOG_INFO, "monolith: --edit-modal: %s%s", res == EDITMODE_RELOADED ? "" : "FAILED: ", m->message);
    return res;
}

void EditModeDrawGuides(const EditMode *m, Camera3D cam)
{
    TransformDrawGuides(&m->modal, cam);
}

// #endregion

// ======================================================================================
// #region Update, HUD
// ======================================================================================

void EditModeOpen(EditMode *m, Designer *d)
{
    if (d->volumeMode) DesignerVolumeModeEnd(d);
    m->active = true;
    EditModeScanModels(m);
}

bool EditModeTakesArrows(const EditMode *m) { return m->active; }

bool EditModeCapturesInput(const EditMode *m) { return m->idOpen; }

int EditModeUnsaved(const EditMode *m)
{
    return MapEditUnsaved(&m->ed);      // (auto save: 0 unless a write failed)
}

bool EditModeSave(EditMode *m, char *msg, int msgSize)
{
    bool ok = MapEditSave(&m->ed, msg, msgSize);
    Say(m, !ok, msg);
    TraceLog(ok ? LOG_INFO : LOG_WARNING, "monolith: %s", msg);
    return ok;
}

void EditModeSync(EditMode *m, Designer *d, bool newScene)
{
    MapEditor *E = &m->ed;
    // A reload (from disk, or a renderer restart) ended the preview and renumbered the objects:
    // the modal stops
    if (TransformRunning(&m->modal)) {
        m->modal.previewing = false;
        TransformEnd(&m->modal, d);
        Say(m, true, "the map was reloaded: the transform was cancelled");
    }
    if (!E->work || (!newScene && scene->loadVersion == E->loadVersion)) return;
    char msg[512];
    if (newScene && scene->hash == E->diskHash) {
        // The renderer restarted and loaded the same file: the working copy goes live again
        E->loadVersion = scene->loadVersion;
        if (E->workHash != scene->hash) {
            char err[512];
            if (MS_EditSetText(scene, E->work, E->workSize, &E->lastStats, err, sizeof(err)) != MS_RELOADED)
                Say(m, true, TextFormat("the unsaved edits could not be shown again: %s", err));
            DesignerSync(d);
        }
        return;
    }
    if (MapEditUnsaved(E) == 0) {
        bool had = E->undoCount + E->redoCount > 0;
        MapEditBegin(E);
        if (had) Say(m, true, "the map changed on disk (not by a manual edit): undo history cleared");
        return;
    }
    MS_EntityId *ids;
    int count;
    int failed = MapEditRebase(E, &ids, &count, msg, sizeof(msg));
    DesignerClearSelection(d);
    DesignerSync(d);
    for (int i = 0; i < count; i++) {
        int e = MS_FindEntity(scene, ids[i]);
        if (e >= 0) DesignerSelectObject(d, scene->entityObjects[e]);
    }
    free(ids);
    Say(m, failed > 0, msg);
    TraceLog(failed > 0 ? LOG_WARNING : LOG_INFO, "monolith: %s", msg);
    for (int i = 0; i < E->failedCount; i++) TraceLog(LOG_WARNING, "monolith:   %s", E->failed[i].what);
}

EditModeResult EditModeUpdate(EditMode *m, Designer *d, Camera3D cam, float dt)
{
    if (m->messageTime > 0.0f) m->messageTime -= dt;
    if (m->warnTime > 0.0f) m->warnTime -= dt;
    if (m->leaveWarnTime > 0.0f) m->leaveWarnTime -= dt;
    if (TransformRunning(&m->modal)) return ModalUpdate(m, d, cam);     // (it takes every key)
    if (m->active && (d->volumeMode || d->snipping)) m->active = false;    // V / C took over
    if (!m->active) m->idOpen = false;
    if (m->idOpen) return IdPanel(m, d);

    bool ctrl = Ctrl(), shift = Shift();
    if (ctrl && IsKeyPressed(KEY_Z)) return History(m, d, !shift);
    if (ctrl && IsKeyPressed(KEY_Y)) return History(m, d, false);
    if (ctrl && IsKeyPressed(KEY_S)) {
        char msg[512];
        if (MapEditUnsaved(&m->ed) == 0) Say(m, false, "nothing to save");
        else if (Confirmed(m, KEY_S)) EditModeSave(m, msg, sizeof(msg));
        return EDITMODE_NONE;
    }
    if (ctrl && shift && IsKeyPressed(KEY_DELETE) && m->ed.failedCount > 0) {
        MapEditClearFailed(&m->ed);
        Say(m, false, "list of edits that could not be re-applied cleared");
        return EDITMODE_NONE;
    }

    if (IsKeyPressed(KEY_M) || (m->active && IsKeyPressed(KEY_ESCAPE))) {
        int unsaved = EditModeUnsaved(m);
        if (m->active && unsaved > 0 && m->leaveWarnTime <= 0.0f) {
            m->leaveWarnTime = WARN_TIME;
            Say(m, true, TextFormat("%d unsaved edit%s: CTRL+S saves, M again leaves them unsaved (still live)", unsaved,
                                    unsaved == 1 ? "" : "s"));
            return EDITMODE_NONE;
        }
        m->leaveWarnTime = 0.0f;
        if (m->active) m->active = false;
        else EditModeOpen(m, d);
        return EDITMODE_NONE;
    }
    if (!m->active) return EDITMODE_NONE;

    if (IsKeyPressed(KEY_N)) Say(m, false, TextFormat("snap %g m", DesignerCycleSnap(d)));

    Vector3 fwd, right;
    ViewAxes(d, &fwd, &right);
    static const int KEYS[6] = { KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_PAGE_UP, KEY_PAGE_DOWN };
    const Vector3 dirs[6] = { fwd, Vector3Negate(fwd), right, Vector3Negate(right), { 0, 1, 0 }, { 0, -1, 0 } };
    for (int k = 0; k < 6; k++)
        if (KeyRepeat(KEYS[k])) return MoveStep(m, d, KEYS[k], dirs[k]);
    // RMB held flies the camera (main.c): the letter keys are the mode's only without it
    bool flying = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
    if (IsKeyPressed(KEY_R) && Alt()) return Turn(m, d, KEY_R);
    if (!flying && !ctrl && !Alt()) {
        if (IsKeyPressed(KEY_G)) return ModalStart(m, d, TRANSFORM_GRAB);
        if (IsKeyPressed(KEY_R)) return ModalStart(m, d, TRANSFORM_ROTATE);
        if (IsKeyPressed(KEY_S)) return ModalStart(m, d, TRANSFORM_SCALE);
    }
    if (IsKeyPressed(KEY_DELETE) && !(ctrl && shift)) return Apply(m, d, KEY_DELETE, &(EditOp){ .kind = EDIT_REMOVE });
    if (ctrl && IsKeyPressed(KEY_D)) return Duplicate(m, d, KEY_D);
    if (ctrl && IsKeyPressed(KEY_I)) return IdKey(m, d);
    // (plain I opens the model picker: main.c)
    return EDITMODE_NONE;
}

static void HudText(const char *text, int x, int y, Color c)
{
    UiHudText(text, x, y, UI_HUD_LINE, UI_S, c);
}

int EditModeDrawHud(const EditMode *m, const Designer *d, int x, int y)
{
    int y0 = y;
    Color dim = UI_DIM;
    if (m->active) {
        y += TransformDrawHud(&m->modal, x, y);
        // (state only: the keys are in the keybindings panel, ESC)
        UiHudText(TextFormat("EDIT MODE  |  snap %g m", DesignerSnap(d)), x, y, UI_HUD_LINE, UI_S, Fade(EDIT_COLOR, 0.95f));
        y += UI_HUD_LINE;
        char sel[160] = "nothing selected";
        if (d->selectedCount > 0) {
            char where[128];
            MapEditDescribe(d->selected[0], where, sizeof(where));
            snprintf(sel, sizeof(sel), "%d selected, first: %s %s", d->selectedCount,
                     d->selected[0].kind == MS_OBJ_MODEL ? "model" : d->selected[0].kind == MS_OBJ_LIGHT ? "light" : "box", where);
        }
        HudText(TextFormat("%s  |  undo %d, redo %d  |  %s", sel,
                           m->ed.undoCount, m->ed.redoCount, m->ed.autoSave ? "auto save" : "manual save"), x, y, dim);
        y += UI_HUD_LINE;
    }
    // Unsaved edits and the ones a rebase could not apply: in every spectator mode
    int unsaved = EditModeUnsaved(m);
    if (unsaved > 0) {
        UiHudHints(TextFormat("%d unsaved edit%s|{CTRL+S} save", unsaved, unsaved == 1 ? "" : "s"), x, y, UI_HUD_LINE, UI_S,
                   Fade(EDIT_COLOR, 0.95f));
        y += UI_HUD_LINE;
    }
    for (int i = 0; i < m->ed.failedCount; i++) {
        HudText(m->ed.failed[i].what, x, y, (Color){ 255, 110, 90, 255 });
        y += UI_HUD_LINE;
    }
    if (m->ed.failedCount > 0) {
        UiHudHints("{CTRL+SHIFT+DEL} clear this list", x, y, UI_HUD_LINE, UI_S, UI_DIM);
        y += UI_HUD_LINE;
    }
    if (m->idOpen) {
        // The id panel: a boxed number field under the mode's lines
        y += 8;
        const char *title = TextFormat("ID for line %d  (now: %s)", m->idLine,
                                       m->idHas ? TextFormat("id %d", m->idHas) : "implicit, changes when the line does");
        const char *hint = TextFormat("{ENTER} set|{ESC} cancel|empty: the next free id (%d)|1..2147483647, unique in the map",
                                      m->idNext);
        int w = 24 + (int)fmaxf((float)UiMeasureBold(title, UI_S), (float)UiHintsWidth(hint, UI_S));
        UiPanel((Rectangle){ (float)(x - 6), (float)(y - 4), (float)w, 88 }, UI_PANEL, Fade(EDIT_COLOR, 0.9f));
        UiTextBold(title, x + 6, y + 4, UI_S, EDIT_COLOR);
        UiPanel((Rectangle){ (float)(x + 6), (float)(y + 28), 160, 26 }, (Color){ 0, 0, 0, 200 }, Fade(EDIT_COLOR, 0.5f));
        UiText(TextFormat("%s_", m->idText), x + 14, y + 28 + (26 - UiLineHeight(UI_M)) / 2, UI_M, UI_INK);
        UiHints(hint, x + 6, y + 60, UI_S, UI_DIM);
        y += 92;
    }
    if (m->messageTime > 0.0f) {
        Color c = m->messageError ? (Color){ 255, 110, 90, 255 } : EDIT_COLOR;
        HudText(m->message, x, y, Fade(c, fminf(m->messageTime, 1.0f)));
        y += UI_HUD_LINE;
    }
    return y - y0;
}

void EditModeFree(EditMode *m)
{
    TransformFree(&m->modal);
    FreeModels(m);
    free(m->models);
    MapEditFree(&m->ed);
    memset(m, 0, sizeof(*m));
}

// #endregion
