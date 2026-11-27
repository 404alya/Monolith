// Edit mode -- M in spectator mode: move, turn, remove, duplicate and add objects by hand
//
// Each edit is a minimal text edit (mapedit.c) of the working copy, the map text the scene
// shows, and goes live at once (MS_EditSetText); the selection follows the edited objects
// through their entity ids. CTRL+S (or auto save) writes the map file; unsaved edits are
// applied again when the file changes on disk (EditModeSync). No request is made.
// Reference: docs/code/editmode.md
#include "editmode.h"
#include "launcher.h"
#include "dirscan.h"
#include "ui.h"
#include <ctype.h>
#include <math.h>

#define STEP_TURN_DEG   90.0    // R / SHIFT+R (docs/live-editing.md 11.10)
#define WARN_TIME       3.0f

static bool KeyRepeat(int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); }
static bool Ctrl(void) { return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL); }
static bool Shift(void) { return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }
static bool Alt(void) { return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT); }

// The mode's HUD line (an event, never per frame): a literal (SayStatic) or formatted text (Sayf;
// text of any other lifetime goes in as "%s")
static void SayShown(EditMode *m, bool error)
{
    m->messageTime = error ? 8.0f : 5.0f;
    m->messageError = error;
}

static void SayStatic(EditMode *m, bool error, const char *literal)
{
    MsgSetStatic(&m->message, literal);
    SayShown(m, error);
}

static void Sayf(EditMode *m, bool error, const char *fmt, ...) MSG_PRINTF(3, 4);
static void Sayf(EditMode *m, bool error, const char *fmt, ...)
{
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    MsgSetv(&m->message, fmt, arg_ptr);
    va_end(arg_ptr);
    SayShown(m, error);
}

// The texts of the transform modal's start codes 1 and 2 (3 quotes the spec: formatted where it is)
static const char *const TRANSFORM_START_TEXTS[3] = {
    "", "nothing selected (LMB selects, SHIFT+LMB adds, CTRL+LMB a group)", "out of memory",
};

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
    // (qsort with a NULL array is undefined even for 0 elements: a package without models)
    if (m->modelCount > 1) qsort(m->models, (size_t)m->modelCount, sizeof(char *), CompareStrings);
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
    Sayf(m, true, "request [%d]%s is queued or running on this map: its session may change the same lines. "
                  "Press again to %s anyway", newest, open > 1 ? TextFormat(" (+%d more)", open - 1) : "",
                  key == KEY_S ? "save" : "edit");
    return false;
}

bool EditModeConfirm(EditMode *m, int key) { return Confirmed(m, key); }

// The working copy starts at the first edit (the map as loaded); false for a compiled map
static bool EnsureWork(EditMode *m)
{
    return MapEditHasWork(&m->ed) || MapEditBegin(&m->ed);
}

// After an edit / undo / redo (r: MapEditApply's / MapEditUndo's code): the selection becomes the
// edited objects
static EditModeResult Done(EditMode *m, Designer *d, unsigned char r, MS_EntityId *ids, int count)
{
    if (r == 1) {       // not done: why is the last error
        Sayf(m, true, "%s", MS_LastError());
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
    // (2, done but the auto save failed, is shown as before: not in red, the note says it)
    Sayf(m, false, "%s", MapEditNote(&m->ed));
    char stats[400];
    MapEditStatsText(&m->ed.lastStats, stats, sizeof(stats));
    TraceLog(LOG_INFO, "monolith: edit: %s", MapEditNote(&m->ed));
    TraceLog(LOG_INFO, "monolith: live edit %s", stats);
    return EDITMODE_RELOADED;
}

static EditModeResult ApplyTo(EditMode *m, Designer *d, int key, const MS_ObjRef *sel, int selCount, const EditOp *op)
{
    // (the group ops by name need no selection: it only rides along)
    bool byName = op->kind == EDIT_GROUP_RENAME || op->kind == EDIT_GROUP_DELETE || op->kind == EDIT_GROUP_DUPLICATE;
    if (op->kind != EDIT_ADD && op->kind != EDIT_BARRIER_ADD && op->kind != EDIT_SHAPE_ADD && !byName && selCount == 0) {
        SayStatic(m, true, "nothing selected (LMB selects, SHIFT+LMB adds, CTRL+LMB a group)");
        return EDITMODE_NONE;
    }
    if (!Confirmed(m, key)) return EDITMODE_NONE;
    EnsureWork(m);      // (a compiled map has none: MapEditApply says why)
    MS_EntityId *ids;
    int count;
    unsigned char r = MapEditApply(&m->ed, sel, selCount, op, &ids, &count);
    return Done(m, d, r, ids, count);
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

// --- Groups as units (docs/editor-tools.md 3.3) ----------------------------------------------------

// The selection is whole groups (docs/editor-tools.md 3.1): every selected object is in a group, and
// every group statement with a selected object has all its objects selected (lights, which can't be
// selected, don't count). *groups (heap, the caller frees; per scene group index): the selected
// statements. *byName: also every statement of each of their names is selected (or has no
// selectable object): Ctrl+D copies a group by name. False (and *groups NULL): not whole groups.
static bool WholeGroups(const Designer *d, bool **groups, bool *byName)
{
    *groups = NULL;
    *byName = false;
    int n = scene->groupCount;
    if (d->selectedCount == 0 || n == 0) return false;
    int *selCount = calloc((size_t)n, sizeof(int)), *total = calloc((size_t)n, sizeof(int));
    bool whole = selCount && total;
    for (int i = 0; whole && i < d->selectedCount; i++) {
        const MS_Ref *r = MS_ObjectRef(scene, d->selected[i]);
        whole = r && r->group >= 0 && r->group < n;
        if (whole) selCount[r->group]++;
    }
    for (int e = 0; whole && e < scene->entityCount; e++) {
        MS_ObjRef o = scene->entityObjects[e];
        const MS_Ref *r = o.kind != MS_OBJ_LIGHT ? MS_ObjectRef(scene, o) : NULL;
        if (r && r->group >= 0 && r->group < n) total[r->group]++;
    }
    for (int g = 0; whole && g < n; g++) whole = selCount[g] == 0 || selCount[g] == total[g];
    if (whole && (*groups = calloc((size_t)n, sizeof(bool)))) {
        for (int g = 0; g < n; g++) (*groups)[g] = selCount[g] > 0;
        *byName = true;
        for (int g = 0; g < n && *byName; g++) {
            if (!(*groups)[g]) continue;
            for (int h = 0; h < n && *byName; h++)
                if (!(*groups)[h] && total[h] > 0 && strcmp(scene->groups[h], scene->groups[g]) == 0) *byName = false;
        }
    }
    free(selCount);
    free(total);
    return *groups != NULL;
}

// The centre of the selection's world bounds, rounded to 0.1 mm (the transform modal's pivot)
static void SelectionCentre(const Designer *d, double out[3])
{
    BoundingBox bb = { { INFINITY, INFINITY, INFINITY }, { -INFINITY, -INFINITY, -INFINITY } };
    for (int i = 0; i < d->selectedCount; i++) {
        BoundingBox b = MS_ObjectBounds(scene, d->selected[i]);
        bb.min = Vector3Min(bb.min, b.min);
        bb.max = Vector3Max(bb.max, b.max);
    }
    const float c[3] = { 0.5f * (bb.min.x + bb.max.x), 0.5f * (bb.min.y + bb.max.y), 0.5f * (bb.min.z + bb.max.z) };
    for (int a = 0; a < 3; a++) out[a] = round((double)c[a] * 1e4) / 1e4;
}

// R / SHIFT+R on whole groups: one turn of every object of those group statements (their lights too,
// which orbit the pivot) about the vertical through the selection's centre
static EditModeResult GroupTurn(EditMode *m, Designer *d, int key, const bool *groups, double degrees)
{
    MS_ObjRef *objs = malloc(sizeof(MS_ObjRef) * (size_t)(scene->entityCount > 0 ? scene->entityCount : 1));
    if (!objs) { SayStatic(m, true, "out of memory"); return EDITMODE_NONE; }
    int n = 0;
    for (int e = 0; e < scene->entityCount; e++) {
        const MS_Ref *r = MS_ObjectRef(scene, scene->entityObjects[e]);
        if (r && r->group >= 0 && r->group < scene->groupCount && groups[r->group]) objs[n++] = scene->entityObjects[e];
    }
    EditOp op = { .kind = EDIT_ROTATE, .degrees = degrees, .hasPivot = true };
    SelectionCentre(d, op.pivot);
    EditModeResult r = ApplyTo(m, d, key, objs, n, &op);
    free(objs);
    return r;
}

// A group name that is free: "<name> 2", or the next free number; a name that ends in " <number>"
// counts on from its base ("plain: house 2" gives "plain: house 5" when 3 and 4 are taken). Heap;
// NULL: out of memory
static char *FreeGroupName(const char *name)
{
    size_t len = strlen(name), base = len;
    while (base > 0 && isdigit((unsigned char)name[base - 1])) base--;
    if (base == len || base < 2 || name[base - 1] != ' ') base = len;     // (no " <number>" at its end)
    else base--;
    for (int k = 2; k < 1000000; k++) {
        char *c = MonoStrf("%.*s %d", (int)base, name, k);
        if (!c) return NULL;
        bool taken = false;
        for (int g = 0; g < scene->groupCount && !taken; g++) taken = strcmp(scene->groups[g], c) == 0;
        if (!taken) return c;
        free(c);
    }
    return NULL;
}

EditModeResult EditModeDuplicateGroups(EditMode *m, Designer *d, int key, const char *const *names, int count)
{
    if (count <= 0) { SayStatic(m, true, "no group to duplicate"); return EDITMODE_NONE; }
    if (!Confirmed(m, key)) return EDITMODE_NONE;
    EnsureWork(m);
    // The shift: one footprint of every object of the groups (whole snap steps, at least one) to the
    // view's right, as CTRL+D shifts a selection
    Vector3 fwd, right;
    ViewAxes(d, &fwd, &right);
    int a = AxisOf(right);
    float lo = INFINITY, hi = -INFINITY;
    for (int e = 0; e < scene->entityCount; e++) {
        MS_ObjRef o = scene->entityObjects[e];
        const MS_Ref *r = o.kind != MS_OBJ_LIGHT ? MS_ObjectRef(scene, o) : NULL;
        bool in = false;
        for (int k = 0; r && r->group >= 0 && r->group < scene->groupCount && k < count && !in; k++)
            in = strcmp(scene->groups[r->group], names[k]) == 0;
        if (!in) continue;
        BoundingBox b = MS_ObjectBounds(scene, o);
        lo = fminf(lo, ((const float *)&b.min)[a]);
        hi = fmaxf(hi, ((const float *)&b.max)[a]);
    }
    double s = DesignerSnap(d), width = lo <= hi ? ceil(fmax(hi - lo, 0.0) / s - 1e-6) * s : s;
    EditOp op = { .kind = EDIT_GROUP_DUPLICATE };
    op.delta[a] = round((width > s ? width : s) * (right.x + right.z) * 1e6) / 1e6;

    // (the names are copied: every edit gives the scene new group names)
    char **own = calloc((size_t)count, sizeof(char *));
    MS_EntityId *all = NULL;
    int allCount = 0, allCap = 0, done = 0;
    bool failed = !own;
    for (int k = 0; own && k < count; k++) failed = failed || !(own[k] = MS_StrDup(names[k]));
    if (failed) SayStatic(m, true, "out of memory");
    for (int k = 0; !failed && k < count; k++) {
        char *nw = FreeGroupName(own[k]);
        if (!nw) { SayStatic(m, true, "out of memory"); break; }
        op.groupName = own[k];
        op.groupNew = nw;
        MS_EntityId *ids;
        int n;
        unsigned char r = MapEditApply(&m->ed, d->selected, d->selectedCount, &op, &ids, &n);
        DesignerSync(d);    // (the edit renumbered the objects: the selection rides along with the next one)
        free(nw);
        if (r == 1) { Sayf(m, true, "%s", MS_LastError()); free(ids); break; }
        for (int i = 0; i < n; i++) {
            MS_EntityId *slot = MS_PUSH(all, allCount, allCap);
            if (slot) *slot = ids[i];
        }
        free(ids);
        done++;
    }
    for (int k = 0; own && k < count; k++) free(own[k]);
    free(own);
    if (done == 0) { free(all); return EDITMODE_NONE; }
    // (the copies of every group become the selection)
    DesignerClearSelection(d);
    DesignerSync(d);
    for (int i = 0; i < allCount; i++) {
        int e = MS_FindEntity(scene, all[i]);
        if (e >= 0) DesignerSelectObject(d, scene->entityObjects[e]);
    }
    free(all);
    if (done < count) TraceLog(LOG_WARNING, "monolith: edit: %d of %d groups duplicated: %s", done, count, MsgText(&m->message));
    else if (done == 1) Sayf(m, false, "%s", MapEditNote(&m->ed));
    else Sayf(m, false, "duplicated %d groups (%d undo steps)", done, done);
    char stats[400];
    MapEditStatsText(&m->ed.lastStats, stats, sizeof(stats));
    TraceLog(LOG_INFO, "monolith: edit: %s", MapEditNote(&m->ed));
    TraceLog(LOG_INFO, "monolith: live edit %s", stats);
    return EDITMODE_RELOADED;
}

// CTRL+D on whole groups (by name): each group duplicated as a new group (EditModeDuplicateGroups);
// EDITMODE_NONE with *handled false: not whole groups (the plain duplicate then)
static EditModeResult GroupDuplicateKey(EditMode *m, Designer *d, int key, bool *handled)
{
    bool *groups, byName;
    *handled = false;
    if (!WholeGroups(d, &groups, &byName) || !byName) { free(groups); return EDITMODE_NONE; }
    *handled = true;
    const char **names = malloc(sizeof(char *) * (size_t)scene->groupCount);
    int n = 0;
    for (int g = 0; names && g < scene->groupCount; g++) {
        if (!groups[g]) continue;
        bool seen = false;
        for (int k = 0; k < n && !seen; k++) seen = strcmp(names[k], scene->groups[g]) == 0;
        if (!seen) names[n++] = scene->groups[g];
    }
    free(groups);
    EditModeResult r = names ? EditModeDuplicateGroups(m, d, key, names, n) : EDITMODE_NONE;
    if (!names) SayStatic(m, true, "out of memory");
    free(names);
    return r;
}

// R / SHIFT+R: the plain turn (only `degrees`: each object about its own centre / origin); on whole
// groups one turn of the groups about the selection's centre (GroupTurn)
static EditModeResult StepTurn(EditMode *m, Designer *d, int key, double degrees)
{
    bool *groups, byName;
    if (WholeGroups(d, &groups, &byName)) {
        EditModeResult r = GroupTurn(m, d, key, groups, degrees);
        free(groups);
        return r;
    }
    return Apply(m, d, key, &(EditOp){ .kind = EDIT_ROTATE, .degrees = degrees });
}

// ALT+SHIFT+R (ypr 0 0 0) and --edit-modal "rot y p r": the rotation set, each object about its own
// centre / origin
static EditModeResult SetRotation(EditMode *m, Designer *d, int key, double yaw, double pitch, double roll)
{
    EditOp op = { .kind = EDIT_SET_ROTATION, .ypr = { yaw, pitch, roll } };
    return Apply(m, d, key, &op);
}

// The copies go one footprint of the selection (whole snap steps, at least one) to the right; whole
// groups are copied as new groups (GroupDuplicateKey)
static EditModeResult Duplicate(EditMode *m, Designer *d, int key)
{
    if (d->selectedCount == 0) return Apply(m, d, key, &(EditOp){ .kind = EDIT_DUPLICATE });
    bool handled;
    EditModeResult gr = GroupDuplicateKey(m, d, key, &handled);
    if (handled) return gr;
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
        SayStatic(m, true, "no model to add: the package's models/ has no file listed in ASSETS.md (new assets: a request)");
        return EDITMODE_NONE;
    }
    if (ObjIsNone(d->hit.obj)) {
        SayStatic(m, true, "no surface under the cursor");
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

EditModeResult EditModeAddShape(EditMode *m, Designer *d, int kind)
{
    if (kind < 0 || kind >= MS_SHAPE_KIND_COUNT) return EDITMODE_NONE;
    Vector3 p, n;
    WorldPointKind at = DesignerPickWorldPoint(d, Alt() ? WORLD_POINT_PLANE : WORLD_POINT_SURFACE, &p, &n);
    if (at == WORLD_POINT_NONE) {
        SayStatic(m, true, "neither a surface nor the work plane under the cursor (raise the plane in volume mode: PGUP)");
        return EDITMODE_NONE;
    }
    bool plane = kind == MS_SHAPE_PLANE;
    Vector3 size = plane ? (Vector3){ 2.0f, 0.0f, 2.0f } : (Vector3){ 1.0f, 1.0f, 1.0f };
    // Its box against the face: on top of an upward face (or the work plane), under a downward one,
    // beside a wall (pushed out by half its footprint along the normal, centred on the point's height)
    Vector3 c = p;
    float y0 = p.y;
    if (at == WORLD_POINT_ON_SURFACE && n.y < -0.5f) y0 = p.y - size.y;
    else if (at == WORLD_POINT_ON_SURFACE && n.y < 0.5f) {
        float h = sqrtf(n.x * n.x + n.z * n.z);
        if (h > 0.0f) {
            c.x += n.x / h * 0.5f * size.x;
            c.z += n.z / h * 0.5f * size.z;
        }
        y0 = p.y - 0.5f * size.y;
    }
    // (only a box, model or shape anchors the line: a barrier's line is in the `barriers` group)
    MS_ObjKind hk = d->hit.obj.kind;
    bool anchored = at == WORLD_POINT_ON_SURFACE && (hk == MS_OBJ_BOX || hk == MS_OBJ_MODEL || hk == MS_OBJ_SHAPE);
    EditOp op = { .kind = EDIT_SHAPE_ADD, .shapeKind = kind, .anchor = anchored ? d->hit.obj : (MS_ObjRef){ MS_OBJ_NONE, -1 },
                  .pos = { round(c.x * 1e4) / 1e4, round(y0 * 1e4) / 1e4, round(c.z * 1e4) / 1e4 },
                  .size = { size.x, size.y, size.z } };
    return Apply(m, d, KEY_ENTER, &op);
}

// A barrier made by hand (the designer's three clicks after H, or volume mode's H on a volume): one
// `barrier` line (EDIT_BARRIER_ADD, corners rounded to 0.1 mm like a placed model), after the first
// selected object's line, else in the main file's `# barriers` block; the new barrier is selected
static EditModeResult BarrierAdd(EditMode *m, Designer *d, BoundingBox b)
{
    EditOp op = { .kind = EDIT_BARRIER_ADD, .anchor = d->selectedCount > 0 ? d->selected[0] : (MS_ObjRef){ MS_OBJ_NONE, -1 } };
    const float c[6] = { b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z };
    for (int i = 0; i < 6; i++) op.corners[i] = round(c[i] * 1e4) / 1e4;
    for (int a = 0; a < 3; a++)     // (rounding never makes it flat)
        if (op.corners[a + 3] <= op.corners[a]) op.corners[a + 3] = op.corners[a] + 1e-4;
    EditModeResult r = Apply(m, d, KEY_ENTER, &op);
    if (r == EDITMODE_RELOADED && !d->showBarriers)
        Sayf(m, false, "%s (barriers are hidden: O shows them)", MapEditNote(&m->ed));
    return r;
}

// CTRL+I: the panel for one line; several lines get the next free ids at once
static EditModeResult IdKey(EditMode *m, Designer *d)
{
    if (d->selectedCount == 0) {
        SayStatic(m, true, "nothing selected (LMB selects, SHIFT+LMB adds, CTRL+LMB a group)");
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
    m->idFile = scene->entitySources[e].file;
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
    if (v > 2147483647L) { SayStatic(m, true, "an id is a number from 1 to 2147483647"); return EDITMODE_NONE; }
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
    unsigned char r = undo ? MapEditUndo(&m->ed, &ids, &count) : MapEditRedo(&m->ed, &ids, &count);
    return Done(m, d, r, ids, count);
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
        const char *note = TransformCancelNote(&m->modal);     // (a static sentence)
        SayStatic(m, note != NULL, note ? note : "transform cancelled: everything is back");
        return EDITMODE_NONE;
    }
    if (r == TRANSFORM_COMMIT) return ModalCommit(m, d, &op);
    return EDITMODE_NONE;
}

static EditModeResult ModalStart(EditMode *m, Designer *d, TransformKind kind)
{
    unsigned char r = TransformBegin(&m->modal, d, kind);
    if (r != 0) SayStatic(m, true, TRANSFORM_START_TEXTS[r]);      // nothing selected / out of memory
    return EDITMODE_NONE;
}

bool EditModeModalRunning(const EditMode *m) { return TransformRunning(&m->modal); }

// LMB on a gizmo part: the drag (the modal with that source)
static EditModeResult GizmoStart(EditMode *m, Designer *d, Camera3D cam, GizmoPart part)
{
    unsigned char r = TransformGizmoBegin(&m->modal, d, cam, part);
    if (r == 3) SayStatic(m, true, "nothing of the gizmo there");
    else if (r != 0) SayStatic(m, true, TRANSFORM_START_TEXTS[r]);      // nothing selected / out of memory
    return EDITMODE_NONE;
}

bool EditModeGizmoHover(EditMode *m, Designer *d, Camera3D cam, Vector2 px, bool enabled)
{
    if (!m->active || m->idOpen) enabled = false;
    return TransformGizmoHover(&m->modal, d, cam, px, enabled) != GIZMO_NONE;
}

void EditModeDrawGizmo(EditMode *m, Designer *d, Camera3D cam)
{
    if (m->active && !m->idOpen) TransformDrawGizmo(&m->modal, d, cam);
}

static const char *const GIZMO_PART_NAMES[] = { "", "x", "y", "z", "view", "ball" };

bool EditModeOpenModal(EditMode *m, Designer *d, Camera3D cam, const char *spec, int end, bool *reloaded)
{
    *reloaded = false;
    if (!m->active) EditModeOpen(m, d);
    char w0[16] = "";
    double v[3] = { 0, 0, 0 };
    int n = sscanf(spec, "%15s %lf %lf %lf", w0, &v[0], &v[1], &v[2]);
    bool step = !strcmp(w0, "r90") || !strcmp(w0, "r-90"), reset = !strcmp(w0, "reset"), rot = !strcmp(w0, "rot");
    if (step || reset || rot) {
        // The keys' one-shot edits (R / SHIFT+R, ALT+SHIFT+R, the typed rotation): applied now,
        // or nothing with "cancel"
        if ((step || reset) && n != 1) {
            Sayf(m, true, "--edit-modal \"%s\" takes no numbers", w0);
        } else if (rot && n != 4) {
            Sayf(m, true, "--edit-modal \"rot <yaw> <pitch> <roll>\": not '%s'", spec);
        } else if (end == 2) {
            TraceLog(LOG_INFO, "monolith: --edit-modal: %s cancelled (nothing applied)", w0);
            return true;
        } else {
            EditModeResult r = step ? StepTurn(m, d, KEY_R, w0[1] == '-' ? -STEP_TURN_DEG : STEP_TURN_DEG)
                             : SetRotation(m, d, KEY_R, rot ? v[0] : 0.0, rot ? v[1] : 0.0, rot ? v[2] : 0.0);
            *reloaded = r == EDITMODE_RELOADED;
            TraceLog(LOG_INFO, "monolith: --edit-modal: %s%s", *reloaded ? "" : "FAILED: ", MsgText(&m->message));
            return *reloaded;
        }
    } else if (!strcmp(w0, "gizmo")) {
        // Just the gizmo (with a hovered part)
        char w1[16] = "", w2[16] = "";
        int k = sscanf(spec, "%*s %15s %15s", w1, w2);
        m->modal.forceHover = GIZMO_NONE;
        if (k >= 1 && strcmp(w1, "hover") == 0) {
            for (int p = 1; p <= GIZMO_BALL; p++)
                if (k == 2 && !strcmp(w2, GIZMO_PART_NAMES[p])) m->modal.forceHover = (GizmoPart)p;
            if (m->modal.forceHover == GIZMO_NONE) {
                Sayf(m, true, "--edit-modal \"gizmo [hover <x|y|z|view|ball>]\": not '%s'", spec);
                TraceLog(LOG_WARNING, "monolith: --edit-modal: %s", MsgText(&m->message));
                return false;
            }
        } else if (k != -1 && k != 0) {
            Sayf(m, true, "--edit-modal \"gizmo [hover <x|y|z|view|ball>]\": not '%s'", spec);
            TraceLog(LOG_WARNING, "monolith: --edit-modal: %s", MsgText(&m->message));
            return false;
        }
        m->modal.hover = m->modal.forceHover;
        return true;
    } else {
        // The modal (a key's or a gizmo ring's / the trackball's): the texts of the codes are here
        bool gizmo = !strcmp(w0, "ring") || !strcmp(w0, "ball");
        unsigned char r = gizmo ? TransformGizmoScripted(&m->modal, d, cam, spec) : TransformOpenScripted(&m->modal, d, cam, spec);
        if (r == 0) return true;    // started
        if (r != 3) SayStatic(m, true, TRANSFORM_START_TEXTS[r]);     // nothing selected / out of memory
        else if (gizmo) Sayf(m, true, "--edit-modal \"ring <x|y|z|xx|yy|zz|view> <deg> | ball <dx> <dy> [ctrl] [step <deg>] [type <v>]\": not '%s'", spec);
        else Sayf(m, true, "--edit-modal \"<g|r|s> [x|y|z|X|Y|Z] [typed]\": not '%s'", spec);
    }
    TraceLog(LOG_WARNING, "monolith: --edit-modal: %s", MsgText(&m->message));
    return false;
}

EditModeResult EditModeFinishModal(EditMode *m, Designer *d, bool commit)
{
    EditOp op;
    TransformResult r = TransformFinish(&m->modal, d, commit, &op);
    if (r != TRANSFORM_COMMIT) {
        const char *note = TransformCancelNote(&m->modal);     // (a static sentence)
        if (note) SayStatic(m, true, note);
        TraceLog(LOG_INFO, "monolith: --edit-modal: cancelled (everything back)%s%s", note ? ": " : "", note ? note : "");
        return EDITMODE_NONE;
    }
    EditModeResult res = ModalCommit(m, d, &op);
    TraceLog(LOG_INFO, "monolith: --edit-modal: %s%s", res == EDITMODE_RELOADED ? "" : "FAILED: ", MsgText(&m->message));
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

// 0 saved, 1 nothing to save (the text: the editor's note), 2 not saved (MS_LastError())
unsigned char EditModeSave(EditMode *m)
{
    unsigned char r = MapEditSaveAll(&m->ed);     // (with the draw mode's paint atlases)
    Sayf(m, r == 2, "%s", r == 2 ? MS_LastError() : MapEditNote(&m->ed));
    TraceLog(r == 2 ? LOG_WARNING : LOG_INFO, "monolith: %s", MsgText(&m->message));
    return r;
}

void EditModeSync(EditMode *m, Designer *d, bool newScene)
{
    MapEditor *E = &m->ed;
    // A reload (from disk, or a renderer restart) ended the preview and renumbered the objects:
    // the modal stops
    if (TransformRunning(&m->modal)) {
        m->modal.previewing = false;
        TransformEnd(&m->modal, d);
        SayStatic(m, true, "the map was reloaded: the transform was cancelled");
    }
    if (!MapEditHasWork(E) || (!newScene && scene->loadVersion == E->loadVersion)) return;
    if (newScene && MapEditSceneOnDisk(E)) {
        // The renderer restarted and loaded the same files: the working copy goes live again
        E->loadVersion = scene->loadVersion;
        if (E->workHash != scene->hash) {
            if (MapEditShowWork(E) != MS_RELOADED)
                Sayf(m, true, "the unsaved edits could not be shown again: %s", MS_LastError());
            DesignerSync(d);
        }
        return;
    }
    if (MapEditUnsaved(E) == 0) {
        bool had = E->undoCount + E->redoCount > 0;
        MapEditBegin(E);
        if (had) SayStatic(m, true, "the map changed on disk (not by a manual edit): undo history cleared");
        return;
    }
    MS_EntityId *ids;
    int count;
    int failed;
    unsigned char r = MapEditRebase(E, &ids, &count, &failed);
    // 0 all applied again, 1 some not (the note); 2 the map can't be edited any more, 3 out of
    // memory (the last error): a warning from 1 on
    bool bad = r != 0;
    Sayf(m, bad, "%s", r >= 2 ? MS_LastError() : MapEditNote(E));
    DesignerClearSelection(d);
    DesignerSync(d);
    for (int i = 0; i < count; i++) {
        int e = MS_FindEntity(scene, ids[i]);
        if (e >= 0) DesignerSelectObject(d, scene->entityObjects[e]);
    }
    free(ids);
    TraceLog(bad ? LOG_WARNING : LOG_INFO, "monolith: %s", MsgText(&m->message));
    for (int i = 0; i < E->failedCount; i++) TraceLog(LOG_WARNING, "monolith:   %s", E->failed[i].what);
}

EditModeResult EditModeUpdate(EditMode *m, Designer *d, Camera3D cam, float dt)
{
    if (m->messageTime > 0.0f) m->messageTime -= dt;
    if (m->warnTime > 0.0f) m->warnTime -= dt;
    if (m->leaveWarnTime > 0.0f) m->leaveWarnTime -= dt;
    if (TransformRunning(&m->modal)) return ModalUpdate(m, d, cam);     // (it takes every key)
    if (m->active && ((d->volumeMode && !d->barrierPlace) || d->snipping)) m->active = false;    // V / C took over
    if (!m->active) m->idOpen = false;
    if (m->idOpen) return IdPanel(m, d);
    // A barrier made by hand is written here (also from volume mode: the edit itself needs no open mode);
    // while one is being made the three clicks own the keys
    BoundingBox barrier;
    if (MONOLITH_EDITING && DesignerTakeBarrier(d, &barrier)) return BarrierAdd(m, d, barrier);
    if (d->barrierPlace) return EDITMODE_NONE;

    bool ctrl = Ctrl(), shift = Shift();
    // The view-only build (common.h): the mode never opens and nothing is edited, saved or undone
    if (!MONOLITH_EDITING) {
        if (IsKeyPressed(KEY_M) || (ctrl && (IsKeyPressed(KEY_S) || IsKeyPressed(KEY_Z) || IsKeyPressed(KEY_Y))))
            DesignerMessageStatic(d, true, MSG_VIEW_ONLY);
        return EDITMODE_NONE;
    }
    if (ctrl && IsKeyPressed(KEY_Z)) return History(m, d, !shift);
    if (ctrl && IsKeyPressed(KEY_Y)) return History(m, d, false);
    if (ctrl && IsKeyPressed(KEY_S)) {
        if (MapEditUnsaved(&m->ed) == 0) SayStatic(m, false, "nothing to save");
        else if (Confirmed(m, KEY_S)) EditModeSave(m);     // (it says how it went)
        return EDITMODE_NONE;
    }
    if (ctrl && shift && IsKeyPressed(KEY_DELETE) && m->ed.failedCount > 0) {
        MapEditClearFailed(&m->ed);
        SayStatic(m, false, "list of edits that could not be re-applied cleared");
        return EDITMODE_NONE;
    }

    if (IsKeyPressed(KEY_M) || (m->active && IsKeyPressed(KEY_ESCAPE))) {
        int unsaved = EditModeUnsaved(m);
        if (m->active && unsaved > 0 && m->leaveWarnTime <= 0.0f) {
            m->leaveWarnTime = WARN_TIME;
            Sayf(m, true, "%d unsaved edit%s: CTRL+S saves, M again leaves them unsaved (still live)", unsaved,
                 unsaved == 1 ? "" : "s");
            return EDITMODE_NONE;
        }
        m->leaveWarnTime = 0.0f;
        if (m->active) m->active = false;
        else EditModeOpen(m, d);
        return EDITMODE_NONE;
    }
    if (!m->active) return EDITMODE_NONE;

    // RMB held flies the camera (main.c): the letter keys and the gizmo are the mode's only without it
    bool flying = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
    if (!flying && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && m->modal.hover != GIZMO_NONE)
        return GizmoStart(m, d, cam, m->modal.hover);    // (main.c kept the click from selecting)

    if (IsKeyPressed(KEY_N) && !ctrl) {
        if (shift) Sayf(m, false, "angle snap %g deg (CTRL in a rotate)", DesignerCycleAngleSnap(d));
        else Sayf(m, false, "snap %g m", DesignerCycleSnap(d));
    }

    Vector3 fwd, right;
    ViewAxes(d, &fwd, &right);
    static const int KEYS[6] = { KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_PAGE_UP, KEY_PAGE_DOWN };
    const Vector3 dirs[6] = { fwd, Vector3Negate(fwd), right, Vector3Negate(right), { 0, 1, 0 }, { 0, -1, 0 } };
    for (int k = 0; k < 6; k++)
        if (KeyRepeat(KEYS[k])) return MoveStep(m, d, KEYS[k], dirs[k]);
    if (!flying && !ctrl && IsKeyPressed(KEY_R)) {
        // R / SHIFT+R a 90 degree step, ALT+R the keyboard rotate modal, ALT+SHIFT+R reset
        if (Alt()) return shift ? SetRotation(m, d, KEY_R, 0.0, 0.0, 0.0) : ModalStart(m, d, TRANSFORM_ROTATE);
        return StepTurn(m, d, KEY_R, shift ? -STEP_TURN_DEG : STEP_TURN_DEG);
    }
    if (!flying && !ctrl && !Alt()) {
        if (IsKeyPressed(KEY_H)) {      // a barrier: the volume mode's three clicks (designer.c)
            DesignerBarrierPlaceBegin(d);
            SayStatic(m, false, "barrier: LMB first corner (a surface, else the work plane; ALT: the plane), LMB opposite corner, mouse / wheel height, LMB / ENTER");
            return EDITMODE_NONE;
        }
        if (IsKeyPressed(KEY_G)) return ModalStart(m, d, TRANSFORM_GRAB);
        if (IsKeyPressed(KEY_S)) return ModalStart(m, d, TRANSFORM_SCALE);
        if (IsKeyPressed(KEY_L)) {
            m->modal.gizmoLocal = !m->modal.gizmoLocal;
            SayStatic(m, false, m->modal.gizmoLocal ? "gizmo: local axes (the first selected box or model)" : "gizmo: global axes");
        }
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

// A HUD line that starts with a mark (the mode's name, a status: its bar and text in c) and goes
// on with hints in UI_TEXT; the two backdrop strips join (UiHudText's 6 px pad on either side)
static void HudMarkHints(const char *name, Color c, const char *markup, int x, int y)
{
    int dx = UiHudMark(name, x, y, UI_HUD_LINE, UI_S, c);
    UiHudHints(markup, x + dx, y, UI_HUD_LINE, UI_S, UI_TEXT);
}

int EditModeDrawHud(const EditMode *m, const Designer *d, int x, int y)
{
    int y0 = y;
    Color dim = UI_DIM;
    if (m->active) {
        y += TransformDrawHud(&m->modal, x, y);
        // (state only: the keys are in the keybindings panel, ESC)
        HudMarkHints("EDIT MODE", UI_MODE_EDIT, TextFormat("snap %g m, %g deg|gizmo %s", DesignerSnap(d), DesignerAngleSnap(d),
                                                           m->modal.gizmoLocal ? "local" : "global"), x, y);
        y += UI_HUD_LINE;
        char sel[160] = "nothing selected";
        if (d->selectedCount > 0) {
            char where[128];
            MapEditDescribe(d->selected[0], where, sizeof(where));
            snprintf(sel, sizeof(sel), "%d selected, first: %s %s", d->selectedCount,
                     d->selected[0].kind == MS_OBJ_MODEL ? "model" : d->selected[0].kind == MS_OBJ_LIGHT ? "light"
                     : d->selected[0].kind == MS_OBJ_BARRIER ? "barrier" : d->selected[0].kind == MS_OBJ_SHAPE ? "shape" : "box", where);
        }
        HudText(TextFormat("%s  |  undo %d, redo %d  |  %s", sel,
                           m->ed.undoCount, m->ed.redoCount, m->ed.autoSave ? "auto save" : "manual save"), x, y, dim);
        y += UI_HUD_LINE;
    }
    // Unsaved edits and the ones a rebase could not apply: in every spectator mode
    int unsaved = EditModeUnsaved(m);
    if (unsaved > 0) {
        HudMarkHints(TextFormat("%d unsaved edit%s", unsaved, unsaved == 1 ? "" : "s"), UI_CHANGED, "{CTRL+S} save", x, y);
        y += UI_HUD_LINE;
    }
    for (int i = 0; i < m->ed.failedCount; i++) {
        UiHudMark(m->ed.failed[i].what, x, y, UI_HUD_LINE, UI_S, UI_ERROR);
        y += UI_HUD_LINE;
    }
    if (m->ed.failedCount > 0) {
        UiHudHints("{CTRL+SHIFT+DEL} clear this list", x, y, UI_HUD_LINE, UI_S, UI_DIM);
        y += UI_HUD_LINE;
    }
    if (m->idOpen) {
        // The id panel: a boxed number field under the mode's lines
        y += 8;
        const char *title = TextFormat("ID for line %s%s%d  (now: %s)", MAPEDIT_LINE_AT(m->idFile, m->idLine),
                                       m->idHas ? TextFormat("id %d", m->idHas) : "implicit, changes when the line does");
        const char *hint = TextFormat("{ENTER} set|{ESC} cancel|empty: the next free id (%d)|1..2147483647, unique in the map",
                                      m->idNext);
        int w = 24 + (int)fmaxf((float)UiMeasureBold(title, UI_S), (float)UiHintsWidth(hint, UI_S));
        UiSheet((Rectangle){ (float)(x - 6), (float)(y - 4), (float)w, 88 });
        UiTextBold(title, x + 6, y + 4, UI_S, UI_INK);
        UiPanel((Rectangle){ (float)(x + 6), (float)(y + 28), 160, 26 }, UI_INSET, UI_LINE);
        int ty = y + 28 + (26 - UiLineHeight(UI_M)) / 2;
        UiText(m->idText, x + 14, ty, UI_M, UI_INK);
        UiText("_", x + 14 + UiMeasure(m->idText, UI_M), ty, UI_M, UI_HOT);
        UiHints(hint, x + 6, y + 60, UI_S, UI_DIM);
        y += 92;
    }
    if (m->messageTime > 0.0f) {
        float a = fminf(m->messageTime, 1.0f);
        if (m->messageError) UiHudMark(MsgText(&m->message), x, y, UI_HUD_LINE, UI_S, UiAlpha(UI_ERROR, a));
        else HudText(MsgText(&m->message), x, y, UiAlpha(UI_INK, a));
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
    MsgFree(&m->message);
    memset(m, 0, sizeof(*m));
}

// #endregion
