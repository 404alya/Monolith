// The model picker -- I in edit mode: choose one of the package's models and place it
//
// Lists the package's model files that have a licence row in ASSETS.md: the edit mode's list
// (EditModeScanModels, scanned again on every open), each row with a thumbnail, the file,
// triangles, the bounds' size in metres and the licence from the file's ASSETS.md row. Typing
// filters (file and licence, case-insensitive); UP / DOWN / PGUP / PGDN / HOME / END select
// (the shared list widget, ui.h); ENTER places the selected model at the point under the cursor
// (EditModeAddModel: snapped on the hit face's axes, CTRL exact; one live edit, undoable, saved
// with CTRL+S) and closes the picker, so G then moves it (the selection is the new model). ESC
// closes, and I too while the filter is empty (otherwise it types an i). Models of other
// packages are not listed: bringing one in is an asset import with a licence row (a request).
//
// Thumbnails (docs/live-editing.md 8.3): PICKER_THUMB_SIZE square colour-only render targets,
// rendered lazily while the picker is open, a few per frame (ModelPickerRenderThumbs, before
// the frame's scene draw), rows without one show a placeholder. Each is one R3D frame of the
// model file alone with its own materials, far beyond the map's bounds (so the AO volume, the
// fog cache and every map light are out of reach; none of the map's draws are submitted), in
// a neutral studio environment swapped in for the render: R3D's environment (global) is copied
// byte for byte before and restored byte for byte after, with the output mode and the depth
// output flag. One directional light without a shadow map, no fog, SSAO, SSIL, bloom: no
// shadow map, static cache or history buffer is drawn or read, so the next frame's image is
// the same as without the render (the --model-picker shots prove it). Files the scene already
// has loaded are drawn from there; others are loaded for the render and unloaded right after
// (their references checked first, as the map loader does). All thumbnails go when
// scene->loadVersion changes, on a renderer restart (ModelPickerDropThumbs) and at exit.
// R3D's per-frame counter (FRAME_INDEX in surface shaders) advances once per thumbnail; no map
// shader reads it.
#include "modelpicker.h"
#include <ctype.h>
#include <math.h>

#define ROW_H       104         // a 96 px thumbnail + padding
#define THUMB_SHOW  96

static bool Ctrl(void) { return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL); }

// ======================================================================================
// #region The list (the edit mode's models + their ASSETS.md licence)
// ======================================================================================

static void FreeThumb(PickerModel *pm)
{
    if (pm->thumb.id) {
        rlUnloadTexture(pm->thumb.texture.id);
        rlUnloadFramebuffer(pm->thumb.id);
    }
    pm->thumb = (RenderTexture2D){ 0 };
    pm->infoReady = pm->failed = false;
    pm->triangles = 0;
    pm->size = (Vector3){ 0 };
}

static void FreeModel(PickerModel *pm)
{
    FreeThumb(pm);
    free(pm->path);
    free(pm->licence);
    memset(pm, 0, sizeof(*pm));
}

// The licence cell of the file's ASSETS.md row ("| `models/x.glb` | source | author | licence |
// ..."), shortened to the licence's name: cut before ", http" and " (" ("CC0 1.0, https://... "
// -> "CC0 1.0"). "" when the file has no row.
static char *LicenceOf(const char *assets, const char *path)
{
    if (!assets) return MS_StrDup("");
    size_t n = strlen(path);
    for (const char *line = assets; *line; ) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        // the row's first cell names the file in backticks
        const char *p = line;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p < end && *p == '|') {
            p++;
            while (p < end && *p == ' ') p++;
            if (p + n + 2 <= end && p[0] == '`' && !strncmp(p + 1, path, n) && p[n + 1] == '`') {
                // cells 2, 3 (source, author), then the licence
                const char *c = p;
                for (int k = 0; k < 3 && c; k++) {
                    c = memchr(c, '|', (size_t)(end - c));
                    if (c) c++;
                }
                if (!c) return MS_StrDup("");
                const char *ce = memchr(c, '|', (size_t)(end - c));
                if (!ce) ce = end;
                while (c < ce && *c == ' ') c++;
                const char *cut = ce;
                for (const char *q = c; q < ce; q++)
                    if ((q + 6 <= ce && !strncmp(q, ", http", 6)) || (q + 2 <= ce && !strncmp(q, " (", 2))) { cut = q; break; }
                while (cut > c && cut[-1] == ' ') cut--;
                size_t len = (size_t)(cut - c);
                char *s = malloc(len + 1);
                if (!s) return NULL;
                memcpy(s, c, len);
                s[len] = 0;
                return s;
            }
        }
        line = *end ? end + 1 : end;
    }
    return MS_StrDup("");
}

static bool ContainsNoCase(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *h = hay; *h; h++) {
        size_t k = 0;
        while (k < n && h[k] && tolower((unsigned char)h[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == n) return true;
    }
    return n == 0;
}

static bool MatchModel(int item, const char *filter, void *user)
{
    const ModelPicker *p = user;
    return ContainsNoCase(p->models[item].path, filter) || ContainsNoCase(p->models[item].licence, filter);
}

// models[] = the edit mode's list; entries of files still listed keep their info and thumbnail
static void SyncList(ModelPicker *p, EditMode *m)
{
    EditModeScanModels(m);
    char path[MS_PATH_MAX + 16];
    snprintf(path, sizeof(path), "%sASSETS.md", scene->root);
    char *assets = FileExists(path) ? LoadFileText(path) : NULL;
    PickerModel *old = p->models;
    int oldCount = p->modelCount;
    p->models = NULL;
    p->modelCount = p->modelCap = 0;
    if (m->modelCount > 0 && MS_RESERVE(p->models, p->modelCap, m->modelCount)) {
        for (int i = 0; i < m->modelCount; i++) {
            PickerModel pm = { 0 };
            for (int k = 0; k < oldCount; k++)
                if (old[k].path && !strcmp(old[k].path, m->models[i])) {
                    pm = old[k];
                    memset(&old[k], 0, sizeof(old[k]));    // (taken)
                    break;
                }
            if (!pm.path) pm.path = MS_StrDup(m->models[i]);
            free(pm.licence);
            pm.licence = LicenceOf(assets, pm.path);
            if (!pm.path || !pm.licence) { FreeModel(&pm); continue; }
            p->models[p->modelCount++] = pm;
        }
    }
    for (int k = 0; k < oldCount; k++) FreeModel(&old[k]);
    free(old);
    if (assets) UnloadFileText(assets);
    p->list.dirty = true;
    UiListRefilter(&p->list, p->modelCount, MatchModel, p);
}

void ModelPickerOpen(ModelPicker *p, EditMode *m, const char *filter)
{
    if (!m->active) return;
    SyncList(p, m);
    UiListClear(&p->list);
    size_t n = filter ? strlen(filter) : 0;
    if (n > 0 && MS_RESERVE(p->list.filter, p->list.filterCap, (int)n + 1)) {
        memcpy(p->list.filter, filter, n + 1);
        p->list.filterLen = (int)n;
    }
    UiListRefilter(&p->list, p->modelCount, MatchModel, p);
    p->open = true;
    while (GetCharPressed()) {}     // (the I that opened it doesn't go into the filter)
    TraceLog(LOG_INFO, "monolith: model picker: %d of %d models listed%s%s", p->list.rowCount, p->modelCount,
             n ? ", filter " : "", n ? filter : "");
}

void ModelPickerClose(ModelPicker *p) { p->open = false; }

bool ModelPickerCapturesInput(const ModelPicker *p) { return p->open; }

// #endregion

// ======================================================================================
// #region Input
// ======================================================================================

EditModeResult ModelPickerEnter(ModelPicker *p, EditMode *m, Designer *d)
{
    if (p->list.dirty) UiListRefilter(&p->list, p->modelCount, MatchModel, p);
    int item = UiListSelected(&p->list);
    EditModeResult r = EditModeAddModel(m, d, item >= 0 ? p->models[item].path : NULL);
    // placed: the picker closes, so G moves the new model; refused: it stays with the reason shown
    double at[3] = { 0 };
    if (r == EDITMODE_RELOADED) {
        p->open = false;
        if (d->selectedCount > 0) MapEditAnchor(d->selected[0], at);
    }
    // (the position as written: --model-picker-key enter is checked against --apply-edits `add`)
    TraceLog(r == EDITMODE_RELOADED ? LOG_INFO : LOG_WARNING, "monolith: model picker: %s%s", m->message,
             r == EDITMODE_RELOADED ? TextFormat(" at %g %g %g", at[0], at[1], at[2]) : "");
    return r;
}

EditModeResult ModelPickerUpdate(ModelPicker *p, EditMode *m, Designer *d)
{
    if (!p->open) return EDITMODE_NONE;
    if (!m->active) { p->open = false; return EDITMODE_NONE; }
    if (IsKeyPressed(KEY_ESCAPE)) { p->open = false; return EDITMODE_NONE; }
    // I closes while nothing is typed (then it is a letter of the filter), like the inspector's I
    if (IsKeyPressed(KEY_I) && p->list.filterLen == 0 && !Ctrl()) {
        while (GetCharPressed()) {}
        p->open = false;
        return EDITMODE_NONE;
    }
    UiListKeys(&p->list, p->visibleRows);
    if (p->list.dirty) UiListRefilter(&p->list, p->modelCount, MatchModel, p);
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) return ModelPickerEnter(p, m, d);
    return EDITMODE_NONE;
}

// #endregion

// ======================================================================================
// #region Thumbnails
// ======================================================================================

// A colour-only target (no depth buffer: R3D writes its own depth internally and only the
// colour reaches the target, R3D_SetOutputDepth(false))
static RenderTexture2D LoadColorTarget(int size)
{
    RenderTexture2D t = { 0 };
    t.id = rlLoadFramebuffer();
    if (!t.id) return t;
    rlEnableFramebuffer(t.id);
    t.texture.id = rlLoadTexture(NULL, size, size, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
    t.texture.width = t.texture.height = size;
    t.texture.mipmaps = 1;
    t.texture.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    rlFramebufferAttach(t.id, t.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    bool ok = t.texture.id && rlFramebufferComplete(t.id);
    rlDisableFramebuffer();
    if (!ok) {
        if (t.texture.id) rlUnloadTexture(t.texture.id);
        rlUnloadFramebuffer(t.id);
        return (RenderTexture2D){ 0 };
    }
    SetTextureFilter(t.texture, TEXTURE_FILTER_BILINEAR);
    return t;
}

static int TriangleCount(const R3D_Model *model)
{
    int tris = 0;
    for (int i = 0; i < model->meshCount; i++) {
        const R3D_Mesh *mesh = &model->meshes[i];
        if (mesh->primitiveType != R3D_PRIMITIVE_TRIANGLES) continue;
        tris += mesh->indexCount > 0 ? mesh->indexCount / 3 : mesh->vertexCount / 3;
    }
    return tris;
}

// The neutral studio: a flat dark-grey background, a grey ambient colour, no sky, no ambient
// map, no screen-space effects, no fog, no bloom
static R3D_Environment Studio(void)
{
    R3D_Environment e = R3D_ENVIRONMENT_BASE;
    e.background.color = (Color){ 46, 47, 52, 255 };
    e.background.energy = 1.0f;
    e.ambient.color = (Color){ 150, 152, 160, 255 };
    e.ambient.energy = 0.6f;
    e.tonemap.mode = R3D_TONEMAP_ACES;
    e.tonemap.exposure = 1.0f;
    e.tonemap.white = 1.0f;
    return e;
}

// One thumbnail: the model framed from the front right, a little above
static void RenderThumb(PickerModel *pm)
{
    const R3D_Model *model = NULL;
    R3D_Model own = { 0 };
    BoundingBox b = { 0 };
    for (int i = 0; i < scene->modelFileCount && !model; i++) {
        const MS_ModelFile *F = &scene->modelFiles[i];
        if (F->loaded && !strcmp(F->path, pm->path)) {
            model = &F->model;
            b = F->bounds;
            pm->triangles = F->triangleCount;
        }
    }
    if (!model) {
        char err[512];
        if (!MS_CheckModelReferences(scene->root, pm->path, err, sizeof(err))) {
            TraceLog(LOG_WARNING, "monolith: model picker: %s: %s", pm->path, err);
            pm->infoReady = pm->failed = true;
            return;
        }
        char full[MS_PATH_MAX + MS_PATH_MAX];
        snprintf(full, sizeof(full), "%s%s", scene->root, pm->path);
        own = R3D_LoadModel(full);
        if (own.meshCount <= 0 || !own.meshes) {
            TraceLog(LOG_WARNING, "monolith: model picker: %s did not load", pm->path);
            if (own.meshes || own.materials) R3D_UnloadModel(own, true);
            pm->infoReady = pm->failed = true;
            return;
        }
        model = &own;
        b = own.aabb;
        pm->triangles = TriangleCount(&own);
    }
    pm->size = Vector3Subtract(b.max, b.min);
    pm->infoReady = true;
    pm->thumb = LoadColorTarget(PICKER_THUMB_SIZE);

    if (pm->thumb.id) {
        // Far beyond the map (AO volume, fog cache, lights), at a spot that depends on the map only
        Vector3 c = Vector3Scale(Vector3Add(b.min, b.max), 0.5f);
        float r = 0.5f * Vector3Length(pm->size);
        if (!(r > 1e-4f)) r = 1e-4f;
        Vector3 top = scene->bounds.max;
        if (!isfinite(top.x) || !isfinite(top.y) || !isfinite(top.z)) top = (Vector3){ 0 };    // (an empty map)
        Vector3 far = { top.x + 1000.0f + 2.0f * r, top.y + 1000.0f + 2.0f * r, top.z + 1000.0f + 2.0f * r };
        Vector3 at = Vector3Add(far, c);
        // Front right, 22 degrees above: fit the bounds' corners exactly (square view)
        const float yaw = 35.0f * DEG2RAD, pitch = 22.0f * DEG2RAD, fovy = 30.0f;
        Vector3 back = { sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch) };   // target -> eye
        Vector3 fwd = Vector3Negate(back);
        Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, (Vector3){ 0, 1, 0 }));
        Vector3 up = Vector3CrossProduct(right, fwd);
        float t = tanf(0.5f * fovy * DEG2RAD), dist = 0.0f;
        for (int i = 0; i < 8; i++) {
            Vector3 q = { (i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z };
            q = Vector3Subtract(q, c);
            float along = Vector3DotProduct(q, back);
            dist = fmaxf(dist, along + fabsf(Vector3DotProduct(q, right)) / t);
            dist = fmaxf(dist, along + fabsf(Vector3DotProduct(q, up)) / t);
        }
        dist = fmaxf(dist * 1.02f, 1e-3f);     // (the bounds' corners just inside the square)
        Camera3D cam = { .position = Vector3Add(at, Vector3Scale(back, dist)), .target = at, .up = { 0, 1, 0 },
                         .fovy = fovy, .projection = CAMERA_PERSPECTIVE };
        R3D_Camera rc = R3D_CameraFromRL(cam);
        rc.nearPlane = fmax(dist - 1.5 * r, dist * 0.01);
        rc.farPlane = dist + 1.5 * r;
        // The key light: from above, the camera's left and a little from the front
        Vector3 toLight = Vector3Normalize(Vector3Add(Vector3Add(Vector3Scale(right, -0.6f), Vector3Scale(back, 0.5f)),
                                                      (Vector3){ 0, 1.0f, 0 }));
        R3D_Light key = R3D_CreateDirLight(Vector3Negate(toLight), WHITE, 2.4f);

        // Swap the studio in (every byte of R3D's environment kept), render, put everything back
        R3D_Environment *env = R3D_GetEnvironment();
        R3D_Environment saved;
        memcpy(&saved, env, sizeof(saved));
        R3D_OutputMode mode = R3D_GetOutputMode();
        bool depth = R3D_GetOutputDepth();
        R3D_Environment studio = Studio();
        R3D_SetEnvironment(&studio);
        R3D_SetOutputMode(R3D_OUTPUT_SCENE);
        R3D_BeginPro((R3D_View){ .camera = rc, .target = pm->thumb });
            R3D_PushLight(key);
            R3D_DrawModel(*model, far, 1.0f);
        R3D_SetOutputDepth(false);
        R3D_End();
        memcpy(env, &saved, sizeof(saved));
        R3D_SetOutputMode(mode);
        R3D_SetOutputDepth(depth);
    }
    TraceLog(LOG_INFO, "monolith: model picker: thumbnail %s (%s)%s", pm->path, own.meshCount > 0 ? "loaded for it" : "the scene's",
             pm->thumb.id ? "" : ": no render target");
    if (own.meshCount > 0) R3D_UnloadModel(own, true);
}

void ModelPickerDropThumbs(ModelPicker *p)
{
    for (int i = 0; i < p->modelCount; i++) FreeThumb(&p->models[i]);
}

static bool Pending(const PickerModel *pm) { return !pm->infoReady; }

int ModelPickerPending(ModelPicker *p)
{
    if (p->list.dirty) UiListRefilter(&p->list, p->modelCount, MatchModel, p);
    int n = 0;
    for (int k = 0; k < p->list.rowCount; k++) n += Pending(&p->models[p->list.rows[k]]);
    return n;
}

void ModelPickerRenderThumbs(ModelPicker *p, int maxPerFrame)
{
    if (p->scene != scene || p->loadVersion != scene->loadVersion) {
        ModelPickerDropThumbs(p);
        p->scene = scene;
        p->loadVersion = scene->loadVersion;
    }
    if (!p->open) return;
    if (p->list.dirty) UiListRefilter(&p->list, p->modelCount, MatchModel, p);
    // the visible rows first (from the scroll position on), then the rest that pass the filter
    int n = p->list.rowCount, done = 0;
    for (int k = 0; k < n && done < maxPerFrame; k++) {
        PickerModel *pm = &p->models[p->list.rows[(p->list.scroll + k) % n]];
        if (!Pending(pm)) continue;
        RenderThumb(pm);
        done++;
    }
}

// #endregion

// ======================================================================================
// #region Drawing
// ======================================================================================

// "models/tree_oak.glb" -> the text cut with "..." to fit `room` px
static void FitText(const char *text, int x, int y, int size, int room, Color c)
{
    if (UiMeasure(text, size) <= room) { UiText(text, x, y, size, c); return; }
    char cut[200];
    int n = (int)strlen(text);
    if (n > (int)sizeof(cut) - 4) n = (int)sizeof(cut) - 4;
    for (; n > 0; n--) {
        snprintf(cut, sizeof(cut), "%.*s...", n, text);
        if (UiMeasure(cut, size) <= room) break;
    }
    UiText(cut, x, y, size, c);
}

static void DrawModelRow(int item, Rectangle row, bool selected, void *user)
{
    const ModelPicker *p = user;
    const PickerModel *pm = &p->models[item];
    int x = (int)row.x + 12, y = (int)row.y + ((int)row.height - THUMB_SHOW) / 2;
    Rectangle box = { (float)x, (float)y, THUMB_SHOW, THUMB_SHOW };
    if (pm->thumb.id) {
        // (render targets are stored bottom-up)
        DrawTexturePro(pm->thumb.texture, (Rectangle){ 0, 0, PICKER_THUMB_SIZE, -PICKER_THUMB_SIZE }, box, (Vector2){ 0 }, 0.0f, WHITE);
        DrawRectangleLinesEx(box, 1.0f, UI_LINE);
    } else {
        UiPanel(box, UI_RAISED, UI_LINE);
        const char *ph = pm->failed ? "no image" : "...";
        UiText(ph, x + (THUMB_SHOW - UiMeasure(ph, UI_S)) / 2, y + (THUMB_SHOW - UiLineHeight(UI_S)) / 2, UI_S,
               pm->failed ? UI_ERROR : UI_FAINT);
    }
    int tx = x + THUMB_SHOW + 16, room = (int)(row.x + row.width) - 10 - tx;
    // the file (without "models/"), then triangles and size, then the licence
    const char *name = strncmp(pm->path, "models/", 7) == 0 ? pm->path + 7 : pm->path;
    FitText(name, tx, y + 8, UI_M, room, selected ? UI_INK : UI_TEXT);
    const char *info = pm->failed ? "the file did not load"
                     : !pm->infoReady ? "..."
                     : TextFormat("%d triangles   %.2f x %.2f x %.2f m", pm->triangles, pm->size.x, pm->size.y, pm->size.z);
    FitText(info, tx, y + 8 + 28, UI_S, room, pm->failed ? UI_ERROR : UI_DIM);
    if (pm->licence[0]) FitText(pm->licence, tx, y + 8 + 52, UI_S, room, UI_OK);
    else FitText("no licence row in ASSETS.md", tx, y + 8 + 52, UI_S, room, UI_WARN);
}

// On the right, its bottom above the help bars and toasts (the HUD lines stay free), as the
// inspector panel
static Rectangle PanelRect(int screenW, int screenH)
{
    float w = fminf(560.0f, (float)screenW - 40.0f), h = fminf(720.0f, (float)screenH - 200.0f);
    if (h < 240.0f) h = fminf(240.0f, (float)screenH - 20.0f);
    float y = (float)screenH - 90.0f - h;
    return (Rectangle){ (float)screenW - w - 20.0f, y > 10.0f ? y : 10.0f, w, h };
}

void ModelPickerDraw(ModelPicker *p, int screenW, int screenH)
{
    if (!p->open) return;
    if (p->list.dirty) UiListRefilter(&p->list, p->modelCount, MatchModel, p);
    p->visibleRows = UiListDraw(&p->list, PanelRect(screenW, screenH), "Add a model of this package", ROW_H, DrawModelRow, p,
                                "{UP}/{DOWN} select|{ENTER} place at the cursor|type to filter|{ESC} close");
}

void ModelPickerFree(ModelPicker *p)
{
    for (int i = 0; i < p->modelCount; i++) FreeModel(&p->models[i]);
    free(p->models);
    UiListFree(&p->list);
    memset(p, 0, sizeof(*p));
}

// #endregion
