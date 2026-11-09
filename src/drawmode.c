// drawmode -- B in spectator mode: painting on boxes and models into the paint layer (design:
// docs/painting.md)
//
// Paint lives in per-material atlases (textures/paint/<material>.png; a model slot drawn with its own
// material: textures/paint/<file>_s<N>.png), RGBA8 with straight alpha, transparent where unpainted.
// Every painted surface has a rectangle there: a box face (or a group of a box's faces) or a model
// slot of one instance, written in the map as a `paint` statement. The runtime composites the atlas
// over what the material computed (its paint stage), so the object looks exactly as before wherever
// it isn't painted. This file owns each atlas's pixels in memory while it paints: the runtime gets
// MS_PaintAtlasProvide (the image the next live edit loads) and MS_TextureUpdate (a frame's stamps).
//
// The first stroke on a face or slot without a rectangle is one undoable edit (EDIT_PAINT_ADD,
// mapedit.c): the object's line gets an id, the material its `paint` key, the file's paint block a
// statement with the rectangle the packer chose (bottom-left on 4-texel cells with a 2-texel border,
// around the rectangles the text holds; the atlas grows by doubling up to 4096 x 4096). A stroke that
// goes on onto another face of the same box allocates there too, amended onto the same step
// (MapEditAmendPaint). Strokes on surfaces that have a rectangle are steps of paint alone. Every step
// keeps the 64 x 64 tiles it touched as they were before it, and the atlas's size before and after;
// undo and redo swap them with the atlas (mapedit.c calls the hooks of this file).
//
// A model slot whose own UVs can't carry paint (tiled, overlapping, missing) gets an unwrap of its
// whole file on its first stroke (paintunwrap.c, xatlas), provided to the runtime and written as the
// model's .paintuv sidecar on save. Interactively the note "unwrapping x.glb ..." shows for a frame
// first (a scanned rock takes ~3 s).
//
// A stroke: LMB down to up. Every frame the segment from the last cursor position to the current
// one is cut in screen space until consecutive hits lie at most `spacing` texels apart on the same
// region (at most 10 levels deep, on a fixed stack); a stamp at each sample, clipped to the region
// (the entry's rectangle, or a model's UV island in it), composited source-over in sRGB bytes from
// the pixels the stroke started from, with the highest coverage any stamp gave a texel (a mask per
// touched tile), so a stroke never darkens where it crosses itself. The eraser lowers alpha. Fill
// fills the region at LMB down. The stroke keeps its samples in atlas texels, so a rebase can replay
// it onto a PNG that changed on disk.
//
// CTRL+S writes every live atlas with unsaved pixels and the unwraps the text uses, then the map
// (MapEditSave), then the licence records (ASSETS.md rows and .asset.md sidecars of the files it
// wrote first), then MS_EditMarkSaved, so the hot reload doesn't reload the editor's own writes.
#include "drawmode.h"
#include "paintunwrap.h"
#include "png.h"
#include "ui.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>

#define PAINT_DIR       "textures/paint/"
#define CURSOR_COLOR    (Color){ 120, 210, 255, 255 }   // the brush cursor (scene-side: drawn on the surface in 3D)
#define WARN_TIME       3.0f
#define SUBDIV_DEPTH    10
#define STROKE_KEY      1000        // EditModeConfirm's key for a stroke (LMB again within 3 s)
#define BORDER          2           // transparent texels around a rectangle
#define CELL            4           // the packer's grid (mip levels 1 and 2 stay inside the border)
#define SHADER_LIMIT    16384       // R3D's limit of a surface shader's source (bytes)
#define STAGE_BYTES     1800        // what the paint stage adds to a shader (the empty one: ~1.7 KB)

static bool Ctrl(void) { return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL); }
static bool Shift(void) { return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }
static bool Alt(void) { return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT); }

// The mode's message line (an event, never per frame): a literal (SayStatic) or formatted text
// (Sayf; "%s" for text of any other lifetime). Shown for 5 s (an error: 8 s, red) and logged
static void SayShown(DrawMode *dm, bool error)
{
    dm->messageTime = error ? 8.0f : 5.0f;
    dm->messageError = error;
    dm->messageSaid = true;
    TraceLog(error ? LOG_WARNING : LOG_INFO, "monolith: draw: %s", MsgText(&dm->message));
}

static void SayStatic(DrawMode *dm, bool error, const char *literal)
{
    MsgSetStatic(&dm->message, literal);
    SayShown(dm, error);
}

static void Sayf(DrawMode *dm, bool error, const char *fmt, ...) MSG_PRINTF(3, 4);
static void Sayf(DrawMode *dm, bool error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    MsgSetv(&dm->message, fmt, ap);
    va_end(ap);
    SayShown(dm, error);
}

// ======================================================================================
// #region Small helpers: strings, hashes, pixels
// ======================================================================================

typedef struct { char *s; int len, cap; bool oom; } Sb;

static void SbAdd(Sb *b, const char *p, int n)
{
    if (b->oom || n < 0) return;
    if (!MS_RESERVE(b->s, b->cap, b->len + n + 1)) { b->oom = true; return; }
    memcpy(b->s + b->len, p, (size_t)n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void SbStr(Sb *b, const char *s) { SbAdd(b, s, (int)strlen(s)); }

static void SbFmt(Sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void SbFmt(Sb *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *piece = MonoStrv(fmt, ap);     // (sized to fit: a long path or cell is never cut)
    va_end(ap);
    if (!piece) { b->oom = true; return; }
    SbStr(b, piece);
    free(piece);
}

static unsigned long long PixelHash(const Image *img)
{
    unsigned long long h = 14695981039346656037ULL;
    const unsigned char *p = img->data;
    size_t n = img->data ? (size_t)img->width * (size_t)img->height * 4 : 0;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    h ^= (unsigned long long)img->width * 31u + (unsigned long long)img->height;
    return h ? h : 1;      // (0 means "never written")
}

static int Pow2Ceil(float x)
{
    int p = 1;
    while (p < x && p < (1 << 20)) p <<= 1;
    return p;
}

static int ClampI(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float Clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// A blank RGBA8 image (zeroed); data NULL when out of memory
static Image NewImage(int w, int h)
{
    Image img = { 0 };
    img.data = calloc((size_t)w * (size_t)h, 4);
    if (!img.data) return img;
    img.width = w;
    img.height = h;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return img;
}

static void FreeImage(Image *img)
{
    free(img->data);    // (images of this file are malloc'd; raylib's loads go through UnloadImage first)
    *img = (Image){ 0 };
}

// A raylib image (LoadImage) as one of this file's: RGBA8 in malloc'd memory
static Image AdoptImage(Image src)
{
    Image img = { 0 };
    if (!src.data) return img;
    if (src.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) ImageFormat(&src, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    if (src.data && src.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) {
        img = NewImage(src.width, src.height);
        if (img.data) memcpy(img.data, src.data, (size_t)src.width * (size_t)src.height * 4);
    }
    UnloadImage(src);
    return img;
}

// Bilinear sample at (u, v) (0..1 over the w x h texels at (x0, y0) of im, clamped to them), RGBA
static void SampleRect(const Image *im, int x0, int y0, int w, int h, float u, float v, float out[4])
{
    float x = u * w - 0.5f, y = v * h - 0.5f;
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    const unsigned char *d = im->data;
    for (int c = 0; c < 4; c++) out[c] = 0.0f;
    for (int k = 0; k < 4; k++) {
        int px = x0 + ClampI(xi + (k & 1), 0, w - 1), py = y0 + ClampI(yi + (k >> 1), 0, h - 1);
        float wt = ((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fy : 1.0f - fy);
        const unsigned char *p = d + ((size_t)py * im->width + px) * 4;
        // (premultiplied while filtering, so transparent texels add no colour)
        float a = p[3] / 255.0f;
        for (int c = 0; c < 3; c++) out[c] += wt * p[c] * a;
        out[3] += wt * p[3];
    }
    if (out[3] > 0.0f)
        for (int c = 0; c < 3; c++) out[c] = out[c] * 255.0f / out[3];
}

// Source-over in sRGB bytes (as the PNG stores them): s with alpha a (0..1, colour alpha x coverage)
static void Over(unsigned char *dst, const unsigned char *d, Color s, float a)
{
    float da = d[3] / 255.0f, oa = a + da * (1.0f - a);
    if (oa <= 0.0f) { dst[0] = dst[1] = dst[2] = dst[3] = 0; return; }
    float sc[3] = { s.r, s.g, s.b };
    for (int c = 0; c < 3; c++) dst[c] = (unsigned char)ClampI((int)lroundf((sc[c] * a + d[c] * da * (1.0f - a)) / oa), 0, 255);
    dst[3] = (unsigned char)ClampI((int)lroundf(oa * 255.0f), 0, 255);
}

static const char *const PEN_NAMES[PEN_COUNT] = { "hard round", "square", "soft round", "eraser", "fill" };
static const char *const FACE_NAMES[6] = { "+x", "-x", "+y", "-y", "+z", "-z" };

static void FacesText(unsigned char faces, char *out, int size)
{
    int n = 0;
    out[0] = '\0';
    for (int f = 0; f < 6 && n < size; f++)
        if (faces & (1u << f)) n += snprintf(out + n, (size_t)(size - n), "%s%s", n ? "," : "", FACE_NAMES[f]);
}

// The package's name (its folder), the author of what the draw mode makes
static void PackageName(char *out, int size)
{
    // (the root's last component: it ends with '/')
    int n = (int)strlen(scene->root) - 1, s;
    if (n < 0) n = 0;
    for (s = n; s > 0 && scene->root[s - 1] != '/' && scene->root[s - 1] != '\\'; s--) { }
    snprintf(out, (size_t)size, "%.*s", n - s, scene->root + s);
}

static bool PackageFileExists(const char *rel)
{
    char path[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", scene->root, rel);
    return FileExists(path);
}

// #endregion

// ======================================================================================
// #region The scene: materials, model slots, paint entries
// ======================================================================================

static int MaterialIndex(const char *name)
{
    for (int m = 0; name && m < scene->materialCount; m++)
        if (strcmp(scene->materials[m].name, name) == 0) return m;
    return -1;
}

// The material a model instance draws slot `slot` with (material.N, material, use), -1: its own
static int SlotMaterial(const MS_ModelInstance *M, int slot)
{
    for (int i = 0; i < M->slotCount; i++)
        if (scene->modelSlots[M->slotFirst + i].slot == slot) return scene->modelSlots[M->slotFirst + i].material;
    return M->ref.material;
}

static bool HitIsBox(const MS_SurfaceHit *h)
{
    return h->hit.obj.kind == MS_OBJ_BOX && h->hit.obj.index >= 0 && h->hit.obj.index < scene->boxCount && h->face >= 0 && h->face < 6;
}

static bool HitIsModel(const MS_SurfaceHit *h)
{
    return h->hit.obj.kind == MS_OBJ_MODEL && h->hit.obj.index >= 0 && h->hit.obj.index < scene->modelCount && h->slot >= 0;
}

// The same surface: object, and its face (boxes) or slot (models)
static bool SameSurface(const MS_SurfaceHit *a, const MS_SurfaceHit *b)
{
    if (a->hit.obj.kind != b->hit.obj.kind || a->hit.obj.index != b->hit.obj.index) return false;
    if (a->hit.obj.kind == MS_OBJ_BOX) return a->face == b->face;
    if (a->hit.obj.kind == MS_OBJ_MODEL) return a->slot == b->slot;
    return true;
}

// The atlas path of material m (-1: model file `file`'s slot `slot` drawn with its own), NULL:
// the material has no paint key yet; *w x *h its image's size now (0: none)
static const char *AtlasRelOf(int m, int file, int slot, int *w, int *h, bool *nearest)
{
    MS_PaintAtlas A;
    if (!MS_PaintAtlasOf(scene, m, file, slot, &A)) A = (MS_PaintAtlas){ 0 };
    if (w) *w = A.width;
    if (h) *h = A.height;
    if (nearest) *nearest = A.nearest;
    return A.rel;
}

// The atlas path of an entry
static const char *EntryAtlasRel(const MS_PaintEntry *e)
{
    int file = e->obj.kind == MS_OBJ_MODEL && e->obj.index >= 0 && e->obj.index < scene->modelCount ? scene->models[e->obj.index].file : -1;
    return AtlasRelOf(e->material, file, e->slot, NULL, NULL, NULL);
}

// The entry of object obj's face `face` (boxes) or slot `slot` (models) in material m: the active
// one's MS_PaintEntryAt index, -1 when none; *inactive: an inactive one holds it (its index)
static int EntryFor(MS_ObjRef obj, int m, int face, int slot, int *inactive)
{
    if (inactive) *inactive = -1;
    int n = MS_PaintEntryCount(scene);
    for (int i = 0; i < n; i++) {
        const MS_PaintEntry *e = MS_PaintEntryAt(scene, i);
        if (e->obj.kind != obj.kind || e->obj.index != obj.index || e->material != m) continue;
        if (obj.kind == MS_OBJ_BOX ? !(face >= 0 && (e->faces & (1u << face))) : e->slot != slot) continue;
        if (e->active) return i;
        if (inactive) *inactive = i;
    }
    return -1;
}

// The world size of a box along its own axes
static Vector3 BoxSize(int b)
{
    return scene->boxTurned && scene->boxTurned[b] >= 0 ? scene->turnedBoxes[scene->boxTurned[b]].size
                                                        : Vector3Subtract(scene->boxes[b].max, scene->boxes[b].min);
}

// Face f of box b: its size along the face's u and v (R3D's unit cube: +-x faces u along z, +-y
// faces v along z)
static void FaceDims(int b, int f, float *su, float *sv)
{
    Vector3 s = BoxSize(b);
    int axis = f / 2;
    *su = axis == 0 ? s.z : s.x;
    *sv = axis == 1 ? s.z : s.y;
}

// A box face group's rectangle at density rho (docs/painting.md 5.2): the largest face of the group,
// each side ceil(rho * its edge), 4..2048
static void BoxRectSize(float rho, int b, unsigned char faces, int *w, int *h)
{
    int best = -1;
    float bestA = -1.0f;
    for (int f = 0; f < 6; f++) {
        if (!(faces & (1u << f))) continue;
        float su, sv;
        FaceDims(b, f, &su, &sv);
        if (su * sv > bestA) { bestA = su * sv; best = f; }
    }
    float su = 0.0f, sv = 0.0f;
    if (best >= 0) FaceDims(b, best, &su, &sv);
    *w = ClampI((int)ceilf(rho * su - 1e-4f), DRAW_RECT_MIN, DRAW_RECT_MAX);
    *h = ClampI((int)ceilf(rho * sv - 1e-4f), DRAW_RECT_MIN, DRAW_RECT_MAX);
}

// A model slot's world area in this instance
static double SlotArea(int model, int slot)
{
    const MS_ModelInstance *M = &scene->models[model];
    int n = MS_ModelSlotTriangleCount(scene, M->file, slot);
    double area = 0.0;
    for (int k = 0; k < n; k++) {
        Vector2 uv[3];
        Vector3 p[3];
        if (!MS_ModelSlotTriangle(scene, M->file, slot, k, uv, p) && !MS_ModelSlotPaintTriangle(scene, M->file, slot, k, uv, p)) continue;
        for (int v = 0; v < 3; v++) p[v] = Vector3Transform(p[v], M->transform);
        area += 0.5 * Vector3Length(Vector3CrossProduct(Vector3Subtract(p[1], p[0]), Vector3Subtract(p[2], p[0])));
    }
    return area;
}

// A model slot's square: clamp(pow2ceil(rho * sqrt(A)), 16, 2048)
static int SlotRectSide(float rho, int model, int slot)
{
    return ClampI(Pow2Ceil(rho * (float)sqrt(SlotArea(model, slot))), DRAW_SLOT_MIN, DRAW_RECT_MAX);
}

// Whether material m's shader can take the paint stage (docs/painting.md 3.4): at most 2 samplers,
// and its source without comments plus the stage within R3D's 16 KB. Cached per material and scene
// version (it reads the shader file)
static bool ShaderTakesPaint(DrawMode *dm, int m)
{
    if (m < 0 || m >= scene->materialCount || !scene->materials[m].shader) return true;
    if (dm->shaderOkVersion != scene->version || dm->shaderOkCount != scene->materialCount) {
        if (!MS_RESERVE(dm->shaderOk, dm->shaderOkCap, scene->materialCount)) return true;
        dm->shaderOkCount = scene->materialCount;
        dm->shaderOkVersion = scene->version;
        memset(dm->shaderOk, -1, (size_t)scene->materialCount);
    }
    if (dm->shaderOk[m] >= 0) return dm->shaderOk[m] != 0;
    char path[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", scene->root, scene->materials[m].shader);
    char *src = FileExists(path) ? LoadFileText(path) : NULL;
    int samplers = 0, bytes = 0;
    bool lineStart = true;
    for (const char *p = src; p && *p; ) {
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            p = e ? e + 2 : p + strlen(p);
            continue;
        }
        if (lineStart && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) { p++; continue; }
        lineStart = *p == '\n';
        if (strncmp(p, "uniform", 7) == 0 && (p == src || !(isalnum((unsigned char)p[-1]) || p[-1] == '_'))) {
            const char *q = p + 7;
            while (*q == ' ' || *q == '\t') q++;
            samplers += strncmp(q, "sampler", 7) == 0;      // (a sampler uniform the shader declares)
        }
        bytes++;
        p++;
    }
    if (src) UnloadFileText(src);
    bool ok = samplers <= 2 && bytes + STAGE_BYTES <= SHADER_LIMIT;
    dm->shaderOk[m] = (signed char)(ok ? 1 : 0);
    return ok;
}

// The ASSETS.md row of a package file (its cells after the first, heap; NULL: none)
static char *AssetsRow(const char *rel)
{
    char path[MS_PATH_MAX + 16];
    snprintf(path, sizeof(path), "%sASSETS.md", scene->root);
    char *text = FileExists(path) ? LoadFileText(path) : NULL;
    if (!text) return NULL;
    char key[MS_PATH_MAX + 8];
    snprintf(key, sizeof(key), "| `%s` |", rel);
    char *row = NULL;
    for (char *l = text; l && *l; ) {
        char *nl = strchr(l, '\n');
        size_t len = nl ? (size_t)(nl - l) : strlen(l);
        if (len >= strlen(key) && strncmp(l, key, strlen(key)) == 0) { row = MS_StrNDup(l + strlen(key), len - strlen(key)); break; }
        l = nl ? nl + 1 : NULL;
    }
    UnloadFileText(text);
    return row;
}

// #endregion

// ======================================================================================
// #region Atlases
// ======================================================================================

static int AtlasByRel(const DrawMode *dm, const char *rel)
{
    for (int i = 0; rel && i < dm->atlasCount; i++)
        if (strcmp(dm->atlases[i].rel, rel) == 0) return i;
    return -1;
}

static bool TilesInit(Atlas *a)
{
    free(a->tileSlot);
    a->tileSlot = NULL;
    a->tilesX = (a->img.width + DRAW_TILE - 1) / DRAW_TILE;
    a->tilesY = (a->img.height + DRAW_TILE - 1) / DRAW_TILE;
    if (a->tilesX * a->tilesY == 0) return true;
    a->tileSlot = malloc(sizeof(int) * (size_t)(a->tilesX * a->tilesY));
    if (!a->tileSlot) return false;
    for (int i = 0; i < a->tilesX * a->tilesY; i++) a->tileSlot[i] = -1;
    return true;
}

static void FreeAtlas(Atlas *a)
{
    free(a->rel);
    free(a->material);
    free(a->modelPath);
    free(a->tileSlot);
    FreeImage(&a->img);
    memset(a, 0, sizeof(*a));
}

static void Provide(Atlas *a)
{
    a->provided = a->img.data && MS_PaintAtlasProvide(scene, a->rel, &a->img);
}

static void Unprovide(Atlas *a)
{
    MS_PaintAtlasProvide(scene, a->rel, NULL);
    a->provided = false;
}

// Uploads the atlas's pixels in the rectangle to the scene (MS_TextureUpdate; nothing when no texture
// of its size shows it yet: the next live edit loads the provided image). raylib logs a line per
// mipmap generation: quiet here
static void Upload(Atlas *a, int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0 || !a->img.data) return;
    SetTraceLogLevel(LOG_WARNING);
    MS_TextureUpdate(scene, a->rel, &a->img, (Rectangle){ (float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0) });
    SetTraceLogLevel(LOG_INFO);
}

// The frame's stamps go up (one update per atlas per frame, docs/painting.md 9.1)
static void Flush(DrawMode *dm)
{
    for (int i = 0; i < dm->atlasCount; i++) {
        Atlas *a = &dm->atlases[i];
        if (!a->dirty) continue;
        Upload(a, a->dx0, a->dy0, a->dx1, a->dy1);
        a->dirty = false;
    }
}

static void MarkDirty(Atlas *a, int x0, int y0, int x1, int y1)
{
    if (!a->dirty) { a->dx0 = x0; a->dy0 = y0; a->dx1 = x1; a->dy1 = y1; a->dirty = true; return; }
    a->dx0 = x0 < a->dx0 ? x0 : a->dx0;
    a->dy0 = y0 < a->dy0 ? y0 : a->dy0;
    a->dx1 = x1 > a->dx1 ? x1 : a->dx1;
    a->dy1 = y1 > a->dy1 ? y1 : a->dy1;
}

// Some material (or own entry) of the current text names the atlas
static bool AtlasLive(const Atlas *a)
{
    if (a->material) {
        int m = MaterialIndex(a->material);
        const char *rel = m >= 0 ? AtlasRelOf(m, -1, -1, NULL, NULL, NULL) : NULL;
        return rel && strcmp(rel, a->rel) == 0;
    }
    int n = MS_PaintEntryCount(scene);
    for (int i = 0; i < n; i++) {
        const MS_PaintEntry *e = MS_PaintEntryAt(scene, i);
        const char *rel = e->material < 0 ? EntryAtlasRel(e) : NULL;
        if (rel && strcmp(rel, a->rel) == 0) return true;
    }
    return false;
}

static bool AtlasUnsaved(const Atlas *a) { return a->img.data && PixelHash(&a->img) != a->savedHash; }

// The image becomes w x h: what fits is kept where it is, the rest is transparent (growth keeps every
// texel in place, docs/painting.md 5.1; an undo of a growth crops it back)
static bool AtlasResize(Atlas *a, int w, int h)
{
    if (a->img.data && a->img.width == w && a->img.height == h) return true;
    Image n = NewImage(w, h);
    if (!n.data) return false;
    if (a->img.data) {
        int cw = a->img.width < w ? a->img.width : w, ch = a->img.height < h ? a->img.height : h;
        for (int y = 0; y < ch; y++)
            memcpy((unsigned char *)n.data + (size_t)y * w * 4, (const unsigned char *)a->img.data + (size_t)y * a->img.width * 4, (size_t)cw * 4);
    }
    FreeImage(&a->img);
    a->img = n;
    return true;
}

// The atlas `rel` (of material `material`, NULL: model file modelPath's slot drawn with its own),
// opened: from the list, else from its PNG when there is one, else empty (its first rectangle makes
// the image). -1 when out of memory
static int AtlasOpen(DrawMode *dm, const char *rel, const char *material, const char *modelPath, int slot)
{
    int ai = AtlasByRel(dm, rel);
    if (ai >= 0 && !dm->atlases[ai].stale) return ai;
    Atlas a = { 0 };
    a.rel = MS_StrDup(rel);
    a.material = material ? MS_StrDup(material) : NULL;
    a.modelPath = modelPath ? MS_StrDup(modelPath) : NULL;
    a.slot = slot;
    char path[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", scene->root, rel);
    if (FileExists(path)) {
        a.img = AdoptImage(LoadImage(path));
        if (a.img.data) a.savedHash = PixelHash(&a.img);
    }
    char *row = AssetsRow(rel);
    a.recorded = row != NULL;
    free(row);
    if (!a.rel || (material && !a.material) || (modelPath && !a.modelPath) || !TilesInit(&a)) { FreeAtlas(&a); return -1; }
    if (ai >= 0) {
        // (a stale atlas: read again, its steps are gone)
        Atlas *o = &dm->atlases[ai];
        a.refs = o->refs;
        a.recorded = a.recorded || o->recorded;
        a.written = o->written;
        FreeAtlas(o);
        *o = a;
    } else {
        Atlas *slotA = MS_PUSH(dm->atlases, dm->atlasCount, dm->atlasCap);
        if (!slotA) { FreeAtlas(&a); return -1; }
        *slotA = a;
        ai = dm->atlasCount - 1;
    }
    if (dm->atlases[ai].img.data) Provide(&dm->atlases[ai]);
    return ai;
}

// A path for a material's new atlas (docs/painting.md 4.1): textures/paint/<material>.png with the
// name kept to letters, digits, '-', '_'; -2, -3, ... when another atlas of the text, a file on disk
// or another material's atlas of this session has it
static void NewAtlasRel(const DrawMode *dm, const char *material, char *out, int size)
{
    char base[128];
    snprintf(base, sizeof(base), "%s", material);
    for (char *c = base; *c; c++) if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') *c = '_';
    for (int k = 1;; k++) {
        if (k == 1) snprintf(out, (size_t)size, PAINT_DIR "%s.png", base);
        else snprintf(out, (size_t)size, PAINT_DIR "%s-%d.png", base, k);
        int ai = AtlasByRel(dm, out);
        if (ai >= 0) {
            if (dm->atlases[ai].material && strcmp(dm->atlases[ai].material, material) == 0) return;    // (ours, from an undone stroke)
            continue;
        }
        bool taken = PackageFileExists(out);
        for (int m = 0; !taken && m < scene->materialCount; m++) {
            const char *rel = AtlasRelOf(m, -1, -1, NULL, NULL, NULL);
            taken = rel && strcmp(rel, out) == 0;
        }
        for (int f = 0; !taken && f < scene->modelFileCount; f++)
            for (int s = 0; !taken && s < scene->modelFiles[f].model.materialCount; s++) {
                const char *rel = AtlasRelOf(-1, f, s, NULL, NULL, NULL);
                taken = rel && strcmp(rel, out) == 0;
            }
        if (!taken || k > 999) return;
    }
}

// #endregion

// ======================================================================================
// #region The packer (docs/painting.md 5.1)
// ======================================================================================

// A free place for a w x h rectangle in a W x H atlas that holds rects[] (x y w h): bottom-left on
// 4-texel cells. Each rectangle owns the cells from its own to 4 texels past its far edges (its border
// of 2 and the next one's), so rectangles placed here start on the grid, keep 2 texels of transparent
// border on every side, and a rectangle the text placed anywhere is avoided. The atlas's edges need
// no border (the sampler clamps)
static bool PackFind(const int *rects, int n, int W, int H, int w, int h, int *outX, int *outY)
{
    int gw = W / CELL, gh = H / CELL;
    if (w > W || h > H || gw <= 0 || gh <= 0) return false;
    int *sat = calloc((size_t)(gw + 1) * (size_t)(gh + 1), sizeof(int));
    unsigned char *occ = calloc((size_t)gw * (size_t)gh, 1);
    bool found = false;
    if (sat && occ) {
        for (int i = 0; i < n; i++) {
            const int *r = &rects[4 * i];
            int cx0 = r[0] / CELL, cy0 = r[1] / CELL;
            int cx1 = (r[0] + r[2] + 2 * BORDER + CELL - 1) / CELL, cy1 = (r[1] + r[3] + 2 * BORDER + CELL - 1) / CELL;
            for (int cy = cy0; cy < cy1 && cy < gh; cy++)
                for (int cx = cx0; cx < cx1 && cx < gw; cx++) occ[(size_t)cy * gw + cx] = 1;
        }
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
                sat[(size_t)(y + 1) * (gw + 1) + x + 1] = occ[(size_t)y * gw + x] + sat[(size_t)y * (gw + 1) + x + 1] +
                                                          sat[(size_t)(y + 1) * (gw + 1) + x] - sat[(size_t)y * (gw + 1) + x];
        int cw = (w + 2 * BORDER + CELL - 1) / CELL, ch = (h + 2 * BORDER + CELL - 1) / CELL;
        for (int cy = 0; cy * CELL + h <= H && !found; cy++)
            for (int cx = 0; cx * CELL + w <= W && !found; cx++) {
                int x1 = cx + cw < gw ? cx + cw : gw, y1 = cy + ch < gh ? cy + ch : gh;
                int used = sat[(size_t)y1 * (gw + 1) + x1] - sat[(size_t)cy * (gw + 1) + x1] - sat[(size_t)y1 * (gw + 1) + cx] +
                           sat[(size_t)cy * (gw + 1) + cx];
                if (used == 0) {
                    *outX = cx * CELL;
                    *outY = cy * CELL;
                    found = true;
                }
            }
    }
    free(sat);
    free(occ);
    return found;
}

// A rectangle of w x h in atlas `rel` (W x H now, 0: no image yet) next to every entry the text
// has in it (but `exclude`, an MS_PaintEntryAt index or -1): *x *y, and the atlas's size after (it
// grows: its width doubles, then its height, up to 4096 x 4096). False: full
static bool AllocRect(const char *rel, int W, int H, int w, int h, int exclude, int *x, int *y, int *outW, int *outH)
{
    int n = MS_PaintEntryCount(scene), count = 0;
    int *rects = malloc(sizeof(int) * 4 * (size_t)(n > 0 ? n : 1));
    if (!rects) return false;
    for (int i = 0; rel && i < n; i++) {
        const MS_PaintEntry *e = MS_PaintEntryAt(scene, i);
        const char *er = i == exclude ? NULL : EntryAtlasRel(e);
        if (!er || strcmp(er, rel) != 0) continue;
        int *r = &rects[4 * count++];
        r[0] = e->x; r[1] = e->y; r[2] = e->w; r[3] = e->h;
    }
    if (W <= 0 || H <= 0) W = H = ClampI(Pow2Ceil((float)(w > h ? w : h)), DRAW_ATLAS_MIN, DRAW_ATLAS_MAX);
    bool ok = false;
    for (;;) {
        if (PackFind(rects, count, W, H, w, h, x, y)) { ok = true; break; }
        if (W >= DRAW_ATLAS_MAX && H >= DRAW_ATLAS_MAX) break;
        if ((W <= H && W < DRAW_ATLAS_MAX) || H >= DRAW_ATLAS_MAX) W *= 2;
        else H *= 2;
    }
    free(rects);
    *outW = W;
    *outH = H;
    return ok;
}

// #endregion

// ======================================================================================
// #region UV islands of a model slot in paint UV space (docs/painting.md 7.3)
// ======================================================================================

// Union-find of the island build
static int Root(int *parent, int i)
{
    while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
    return i;
}

typedef struct { unsigned int a[2], b[2]; int tri; } EdgeKey;

static unsigned int FloatBits(float f)
{
    unsigned int u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static float SegmentDistance(Vector2 p, Vector2 a, Vector2 b)
{
    Vector2 ab = Vector2Subtract(b, a), ap = Vector2Subtract(p, a);
    float l2 = Vector2DotProduct(ab, ab), t = l2 > 0.0f ? Vector2DotProduct(ap, ab) / l2 : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return Vector2Distance(p, Vector2Add(a, Vector2Scale(ab, t)));
}

// The size of JoinUvEdges's table for n triangles (a power of two, at least twice the edges)
static int UvEdgeCap(int n)
{
    int cap = 16;
    while (cap < 6 * n) cap <<= 1;
    return cap;
}

// Joins (parent[], a union-find over n triangles, each its own root on entry) the triangles that
// share an edge with the same two UV points, either direction (exact float bits); table: cap
// entries of scratch (UvEdgeCap)
static void JoinUvEdges(const Vector2 *uv, int n, int *parent, EdgeKey *table, int cap)
{
    for (int i = 0; i < cap; i++) table[i].tri = -1;
    for (int k = 0; k < n; k++)
        for (int e = 0; e < 3; e++) {
            Vector2 p = uv[3 * k + e], q = uv[3 * k + (e + 1) % 3];
            unsigned int a[2] = { FloatBits(p.x), FloatBits(p.y) }, b[2] = { FloatBits(q.x), FloatBits(q.y) };
            if (a[0] > b[0] || (a[0] == b[0] && a[1] > b[1])) {
                unsigned int t0 = a[0], t1 = a[1];
                a[0] = b[0]; a[1] = b[1]; b[0] = t0; b[1] = t1;
            }
            unsigned long long h = (a[0] * 73856093u) ^ (a[1] * 19349663u) ^ (b[0] * 83492791u) ^ ((unsigned long long)b[1] * 2654435761u);
            for (unsigned int i = (unsigned int)(h & (unsigned long long)(cap - 1));; i = (i + 1) & (unsigned int)(cap - 1)) {
                EdgeKey *s = &table[i];
                if (s->tri < 0) { *s = (EdgeKey){ { a[0], a[1] }, { b[0], b[1] }, k }; break; }
                if (s->a[0] == a[0] && s->a[1] == a[1] && s->b[0] == b[0] && s->b[1] == b[1]) {
                    int r1 = Root(parent, k), r2 = Root(parent, s->tri);
                    if (r1 != r2) parent[r1] = r2;
                    break;
                }
            }
        }
}

// A model slot's islands rasterised at a w x h rectangle: per texel the island of a triangle that
// touches it (-1: none; shared[]: more than one), and each slot triangle's island
typedef struct DrawIslands {
    bool valid;
    int file, slot, w, h;
    unsigned int version;
    int *texel;
    unsigned char *shared;
    int *triIsland;
    int triCount;
} DrawIslands;

static void FreeIslands(DrawIslands *I)
{
    if (!I) return;
    free(I->texel);
    free(I->shared);
    free(I->triIsland);
    memset(I, 0, sizeof(*I));
}

// The islands of model file `file`'s slot `slot` at w x h (cached: the last one built). Triangles are
// joined over shared paint-UV edges; the raster is conservative (a texel counts when its centre is
// inside the triangle or within half a texel diagonal of an edge), so the texel under a thin
// triangle belongs to its island. NULL when out of memory or the slot has no paint UVs
static const DrawIslands *IslandsFor(DrawMode *dm, int file, int slot, int w, int h)
{
    DrawIslands *I = dm->islands;
    if (!I && !(I = dm->islands = calloc(1, sizeof(DrawIslands)))) return NULL;
    if (I->valid && I->file == file && I->slot == slot && I->w == w && I->h == h && I->version == scene->version) return I;
    FreeIslands(I);
    int n = MS_ModelSlotTriangleCount(scene, file, slot);
    if (n <= 0 || w <= 0 || h <= 0) return NULL;
    Vector2 *uv = malloc(sizeof(Vector2) * 3 * (size_t)n);
    int *parent = malloc(sizeof(int) * (size_t)n);
    int cap = UvEdgeCap(n);
    EdgeKey *table = malloc(sizeof(EdgeKey) * (size_t)cap);
    I->texel = malloc(sizeof(int) * (size_t)w * (size_t)h);
    I->shared = calloc((size_t)w * (size_t)h, 1);
    I->triIsland = malloc(sizeof(int) * (size_t)n);
    bool ok = uv && parent && table && I->texel && I->shared && I->triIsland;
    for (int k = 0; ok && k < n; k++) {
        Vector3 pos[3];
        if (!MS_ModelSlotPaintTriangle(scene, file, slot, k, &uv[3 * k], pos)) ok = false;
        parent[k] = k;
    }
    if (ok) JoinUvEdges(uv, n, parent, table, cap);
    for (int k = 0; ok && k < n; k++) I->triIsland[k] = -1;
    int islands = 0;
    for (int k = 0; ok && k < n; k++) {
        int r = Root(parent, k);
        if (I->triIsland[r] < 0) I->triIsland[r] = islands++;
        I->triIsland[k] = I->triIsland[r];
    }
    for (int i = 0; ok && i < w * h; i++) I->texel[i] = -1;
    for (int k = 0; ok && k < n; k++) {
        Vector2 t[3];
        for (int v = 0; v < 3; v++) t[v] = (Vector2){ uv[3 * k + v].x * w, uv[3 * k + v].y * h };
        float minx = fminf(t[0].x, fminf(t[1].x, t[2].x)), maxx = fmaxf(t[0].x, fmaxf(t[1].x, t[2].x));
        float miny = fminf(t[0].y, fminf(t[1].y, t[2].y)), maxy = fmaxf(t[0].y, fmaxf(t[1].y, t[2].y));
        int id = I->triIsland[k];
        float area = (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (t[1].y - t[0].y);
        for (int y = ClampI((int)floorf(miny - 1.0f), 0, h - 1); y <= ClampI((int)floorf(maxy + 1.0f), 0, h - 1); y++)
            for (int x = ClampI((int)floorf(minx - 1.0f), 0, w - 1); x <= ClampI((int)floorf(maxx + 1.0f), 0, w - 1); x++) {
                Vector2 q = { x + 0.5f, y + 0.5f };
                float w0 = (t[1].x - q.x) * (t[2].y - q.y) - (t[2].x - q.x) * (t[1].y - q.y);
                float w1 = (t[2].x - q.x) * (t[0].y - q.y) - (t[0].x - q.x) * (t[2].y - q.y);
                float w2 = (t[0].x - q.x) * (t[1].y - q.y) - (t[1].x - q.x) * (t[0].y - q.y);
                bool in = area > 0.0f ? (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) : area < 0.0f && (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f);
                if (!in && fminf(SegmentDistance(q, t[0], t[1]), fminf(SegmentDistance(q, t[1], t[2]), SegmentDistance(q, t[2], t[0]))) > 0.7072f)
                    continue;
                size_t i = (size_t)y * w + x;
                if (I->texel[i] < 0) I->texel[i] = id;
                else if (I->texel[i] != id) I->shared[i] = 1;
            }
    }
    free(uv);
    free(parent);
    free(table);
    if (!ok) { FreeIslands(I); return NULL; }
    I->valid = true;
    I->file = file;
    I->slot = slot;
    I->w = w;
    I->h = h;
    I->version = scene->version;
    I->triCount = n;
    return I;
}

// #endregion

// ======================================================================================
// #region Paint steps and stamps
// ======================================================================================

// A tile of the atlas as it was before the step (undo / redo swap it), and the stroke's coverage of
// it (the max-coverage mask; freed when the stroke ends)
typedef struct PaintTile {
    int tx, ty, w, h;
    unsigned char *pixels, *mask;
} PaintTile;

// Where stamps go: an entry's rectangle (texels of the atlas), and for a model the island in it
typedef struct {
    int x, y, w, h;
    int file, slot;         // a model slot (its islands); -1 for a box face group
    int island;             // -1: the whole rectangle
} PaintRegion;

// A sample of the stroke: atlas texels, its region, joined to the one before it (one stretch)
typedef struct {
    float tx, ty;
    int region;
    bool joined;
} Sample;

typedef enum { STEP_STROKE = 0, STEP_EDIT } StepKind;

struct PaintStep {
    int atlas;
    StepKind kind;          // a stroke (samples, replayable), or a panel edit (faces, resize, clear)
    bool created;           // the step gave the runtime the atlas (it wasn't provided): undo takes it out
    int oldW, oldH, newW, newH;     // the atlas's size before / after the step
    bool sized;             // oldW / oldH recorded (at its first rectangle or at the step's start)
    bool pending;           // not in the history yet: released, its pixels go back
    PaintTile *tiles;
    int tileCount, tileCap;
    // The stroke as input (a rebase replays it onto a PNG that changed on disk)
    PenKind pen;
    int size;
    float hardness;
    Color color;
    Sample *samples;
    int sampleCount, sampleCap;
    PaintRegion *regions;
    int regionCount, regionCap;
};

static PaintStep *NewStep(DrawMode *dm, int ai, StepKind kind)
{
    PaintStep *p = calloc(1, sizeof(PaintStep));
    if (!p) return NULL;
    Atlas *a = &dm->atlases[ai];
    p->atlas = ai;
    p->kind = kind;
    p->pending = true;
    p->pen = dm->pen;
    p->size = dm->size;
    p->hardness = dm->hardness;
    p->color = dm->color;
    p->oldW = p->newW = a->img.width;
    p->oldH = p->newH = a->img.height;
    p->created = !a->provided;
    // (a stroke's samples: room for a long drag up front, so frames rarely grow it)
    if (kind == STEP_STROKE && !MS_RESERVE(p->samples, p->sampleCap, 1024)) p->sampleCap = 0;
    a->refs++;
    return p;
}

static void FreeTiles(PaintStep *p)
{
    for (int i = 0; i < p->tileCount; i++) {
        free(p->tiles[i].pixels);
        free(p->tiles[i].mask);
    }
    free(p->tiles);
    p->tiles = NULL;
    p->tileCount = p->tileCap = 0;
}

static void SwapTiles(Atlas *a, PaintStep *p, int *x0, int *y0, int *x1, int *y1);

static void ReleaseStep(DrawMode *dm, PaintStep *p)
{
    if (!p) return;
    if (p->atlas >= 0 && p->atlas < dm->atlasCount) {
        Atlas *a = &dm->atlases[p->atlas];
        a->refs--;
        // (a step that never got into the history: what it painted goes back)
        if (p->pending && p->tileCount > 0 && a->img.data) {
            int x0, y0, x1, y1;
            SwapTiles(a, p, &x0, &y0, &x1, &y1);
            if (a->provided) Provide(a);
            Upload(a, x0, y0, x1, y1);
        }
        if (a->tileSlot)
            for (int i = 0; i < p->tileCount; i++) {
                int ti = p->tiles[i].ty * a->tilesX + p->tiles[i].tx;
                if (ti >= 0 && ti < a->tilesX * a->tilesY) a->tileSlot[ti] = -1;
            }
    }
    FreeTiles(p);
    free(p->samples);
    free(p->regions);
    free(p);
}

static long long StepBytes(const PaintStep *p)
{
    long long b = (long long)sizeof(PaintStep) + (long long)p->tileCap * (long long)sizeof(PaintTile) +
                  (long long)p->sampleCap * (long long)sizeof(Sample) + (long long)p->regionCap * (long long)sizeof(PaintRegion);
    for (int i = 0; i < p->tileCount; i++) b += (long long)p->tiles[i].w * p->tiles[i].h * (p->tiles[i].mask ? 5 : 4);
    return b;
}

// The stroke ended: the masks go, the tiles stay (the step's undo copy)
static void EndTiles(Atlas *a, PaintStep *p)
{
    for (int i = 0; i < p->tileCount; i++) {
        free(p->tiles[i].mask);
        p->tiles[i].mask = NULL;
        int ti = p->tiles[i].ty * a->tilesX + p->tiles[i].tx;
        if (a->tileSlot && ti >= 0 && ti < a->tilesX * a->tilesY) a->tileSlot[ti] = -1;
    }
}

// The atlas changed size during a step (it grew): the tile grid anew, with the step's tiles in it
static bool TilesRemap(Atlas *a, const PaintStep *p)
{
    if (!TilesInit(a)) return false;
    for (int i = 0; p && i < p->tileCount; i++) {
        int ti = p->tiles[i].ty * a->tilesX + p->tiles[i].tx;
        if (p->tiles[i].mask && ti >= 0 && ti < a->tilesX * a->tilesY) a->tileSlot[ti] = i;
    }
    return true;
}

// The step's tile of texel (x, y), copied from the atlas the first time the step touches it
static PaintTile *TileAt(Atlas *a, PaintStep *p, int x, int y)
{
    int tx = x / DRAW_TILE, ty = y / DRAW_TILE, ti = ty * a->tilesX + tx;
    if (!a->tileSlot || ti < 0 || ti >= a->tilesX * a->tilesY) return NULL;
    if (a->tileSlot[ti] >= 0) return &p->tiles[a->tileSlot[ti]];
    int w = a->img.width - tx * DRAW_TILE, h = a->img.height - ty * DRAW_TILE;
    w = w > DRAW_TILE ? DRAW_TILE : w;
    h = h > DRAW_TILE ? DRAW_TILE : h;
    unsigned char *px = malloc((size_t)w * h * 4), *mask = calloc((size_t)w * h, 1);
    PaintTile *t = px && mask ? MS_PUSH(p->tiles, p->tileCount, p->tileCap) : NULL;
    if (!t) { free(px); free(mask); return NULL; }
    *t = (PaintTile){ tx, ty, w, h, px, mask };
    for (int r = 0; r < h; r++)
        memcpy(px + (size_t)r * w * 4, (unsigned char *)a->img.data + ((size_t)(ty * DRAW_TILE + r) * a->img.width + tx * DRAW_TILE) * 4,
               (size_t)w * 4);
    a->tileSlot[ti] = p->tileCount - 1;
    return t;
}

// One texel at coverage cov (0..1): composited from the pixel the stroke started from, only when cov
// is more than any earlier stamp of the stroke gave it. The eraser lowers alpha by the coverage
// (docs/painting.md 7.2: the colour stays for an anti-aliased edge; at full coverage 0 0 0 0)
static void ApplyTexel(Atlas *a, PaintStep *p, int x, int y, float cov)
{
    PaintTile *t = TileAt(a, p, x, y);
    if (!t) return;
    int lx = x - t->tx * DRAW_TILE, ly = y - t->ty * DRAW_TILE;
    unsigned char cv = (unsigned char)ClampI((int)lroundf(cov * 255.0f), 0, 255);
    unsigned char *m = &t->mask[ly * t->w + lx];
    if (cv == 0 || cv <= *m) return;
    *m = cv;
    const unsigned char *orig = t->pixels + ((size_t)ly * t->w + lx) * 4;
    unsigned char *dst = (unsigned char *)a->img.data + ((size_t)y * a->img.width + x) * 4;
    if (p->pen == PEN_ERASER) {
        int na = (int)lroundf(orig[3] * (1.0f - cv / 255.0f));
        if (na <= 0) dst[0] = dst[1] = dst[2] = dst[3] = 0;
        else { memcpy(dst, orig, 3); dst[3] = (unsigned char)na; }
    } else Over(dst, orig, p->color, cv / 255.0f * (p->color.a / 255.0f));
    MarkDirty(a, x, y, x + 1, y + 1);
}

// One texel set to rgba (a panel edit: clearing, copying), its tile kept for undo first
static void SetTexel(Atlas *a, PaintStep *p, int x, int y, const unsigned char rgba[4])
{
    if (x < 0 || y < 0 || x >= a->img.width || y >= a->img.height) return;
    unsigned char *dst = (unsigned char *)a->img.data + ((size_t)y * a->img.width + x) * 4;
    if (memcmp(dst, rgba, 4) == 0 || !TileAt(a, p, x, y)) return;
    memcpy(dst, rgba, 4);
    MarkDirty(a, x, y, x + 1, y + 1);
}

// The texels of a rectangle and its border become transparent (a freed rectangle, or a new one over
// pixels an earlier text left there)
static void ClearRect(Atlas *a, PaintStep *p, int x, int y, int w, int h)
{
    static const unsigned char zero[4] = { 0, 0, 0, 0 };
    for (int ty = y - BORDER; ty < y + h + BORDER; ty++)
        for (int tx = x - BORDER; tx < x + w + BORDER; tx++)
            if (tx >= 0 && ty >= 0 && tx < a->img.width && ty < a->img.height &&
                ((const unsigned char *)a->img.data)[((size_t)ty * a->img.width + tx) * 4 + 3] != 0)
                SetTexel(a, p, tx, ty, zero);
}

static int RegionIndex(PaintStep *p, PaintRegion r)
{
    for (int i = 0; i < p->regionCount; i++)
        if (memcmp(&p->regions[i], &r, sizeof(r)) == 0) return i;
    PaintRegion *s = MS_PUSH(p->regions, p->regionCount, p->regionCap);
    if (!s) return -1;
    *s = r;
    return p->regionCount - 1;
}

static bool InRegion(const PaintRegion *r, const DrawIslands *I, int x, int y)
{
    if (x < r->x || y < r->y || x >= r->x + r->w || y >= r->y + r->h) return false;
    if (r->island < 0) return true;
    if (!I) return false;
    size_t i = (size_t)(y - r->y) * I->w + (x - r->x);
    return I->texel[i] == r->island || I->shared[i];
}

static float SmoothStep(float e0, float e1, float x)
{
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}

typedef struct { Atlas *a; PaintStep *p; const PaintRegion *r; const DrawIslands *I; } StampCtx;

static void StampTexel(const StampCtx *c, int x, int y, float cov)
{
    if (InRegion(c->r, c->I, x, y)) ApplyTexel(c->a, c->p, x, y, cov);
}

// The pixel-exact footprints (docs/painting.md 7.2) with (x0, y0) their first texel: the square pen's
// s x s texels, the round pens' one texel at size 1 and 2 x 2 at size 2
static void StampKernel(const StampCtx *c, int x0, int y0)
{
    int size = c->p->pen == PEN_SQUARE ? (c->p->size < 1 ? 1 : c->p->size) : c->p->size >= 2 ? 2 : 1;
    for (int y = y0; y < y0 + size; y++)
        for (int x = x0; x < x0 + size; x++) StampTexel(c, x, y, 1.0f);
}

// One stamp of the step's pen at sample s (docs/painting.md 7.2), clipped to its region. prev: the
// sample before it on the same stretch of the stroke (NULL: the stroke starts or jumps there). The
// round pens from size 3 sweep a capsule from prev to s at their sub-texel positions (the texels
// whose centre is closer than s / 2 to the segment), so a straight drag has straight edges and is s
// texels wide; the pixel-exact footprints (square, round 1 and 2) are stamped at every texel the
// segment passes through
static void Stamp(DrawMode *dm, Atlas *a, PaintStep *p, Sample s, const Sample *prev)
{
    if (s.region < 0 || s.region >= p->regionCount || !a->img.data) return;
    const PaintRegion *r = &p->regions[s.region];
    StampCtx c = { a, p, r, r->island >= 0 ? IslandsFor(dm, r->file, r->slot, r->w, r->h) : NULL };
    if (p->pen == PEN_FILL) {
        for (int y = r->y; y < r->y + r->h; y++)
            for (int x = r->x; x < r->x + r->w; x++) StampTexel(&c, x, y, 1.0f);
        return;
    }
    Vector2 b = { s.tx, s.ty }, pa = prev ? (Vector2){ prev->tx, prev->ty } : b;
    int size = p->size < 1 ? 1 : p->size;
    if (p->pen == PEN_SQUARE || size <= 2) {
        // (every half texel along the segment: no texel it crosses is skipped)
        int steps = ClampI((int)ceilf(Vector2Distance(pa, b) * 2.0f), 0, 1 << 16);
        // (square: the sample's texel at floor(s / 2); round size 2: the 2 x 2 texels around the
        // sample's sub-texel position, so a drag is 2 texels wide)
        float shift = p->pen != PEN_SQUARE && size == 2 ? 0.5f : 0.0f;
        int back = p->pen == PEN_SQUARE ? size / 2 : 0;
        int lx = INT32_MIN, ly = INT32_MIN;
        for (int i = 0; i <= steps; i++) {
            float t = steps ? (float)i / steps : 1.0f;
            int kx = (int)floorf(pa.x + (b.x - pa.x) * t - shift) - back, ky = (int)floorf(pa.y + (b.y - pa.y) * t - shift) - back;
            if (kx == lx && ky == ly) continue;
            StampKernel(&c, kx, ky);
            lx = kx;
            ly = ky;
        }
        return;
    }
    float rad = size * 0.5f, hard = p->hardness < 0.0f ? 0.0f : p->hardness > 0.99f ? 0.99f : p->hardness;
    int x0 = (int)floorf(fminf(pa.x, b.x) - rad), x1 = (int)ceilf(fmaxf(pa.x, b.x) + rad);
    int y0 = (int)floorf(fminf(pa.y, b.y) - rad), y1 = (int)ceilf(fmaxf(pa.y, b.y) + rad);
    x0 = x0 > r->x ? x0 : r->x;
    y0 = y0 > r->y ? y0 : r->y;
    x1 = x1 < r->x + r->w ? x1 : r->x + r->w;
    y1 = y1 < r->y + r->h ? y1 : r->y + r->h;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float d = SegmentDistance((Vector2){ x + 0.5f, y + 0.5f }, pa, b);
            if (d >= rad) continue;
            float cov = p->pen == PEN_SOFT ? 1.0f - SmoothStep(hard, 1.0f, d / rad) : 1.0f;
            StampTexel(&c, x, y, cov);
        }
}

// #endregion

// ======================================================================================
// #region Where paint at a surface goes, its rectangle (allocated by the first stroke)
// ======================================================================================

#define UV_HIT_K (-2)       // MS_SurfaceHit.mesh of a --paint-script `uv` hit: triangle is the slot triangle k

static float Spacing(const DrawMode *dm) { return fmaxf(1.0f, dm->size / 4.0f); }

// The slot triangle of a model hit (MS_ModelSlotTriangle's k), -1
static int HitTriangle(const MS_SurfaceHit *h)
{
    if (!HitIsModel(h)) return -1;
    if (h->mesh == UV_HIT_K) return h->triangle;
    return MS_ModelSlotTriangleIndex(scene, scene->models[h->hit.obj.index].file, h->mesh, h->triangle);
}

// The hit's paint coordinate now (a model's from its triangle's paint UVs: they exist once an unwrap
// went live, after the hit was cast); false when it has none
static bool HitPaintUv(const MS_SurfaceHit *h, Vector2 *uv)
{
    if (HitIsBox(h)) { *uv = h->paintUv; return true; }
    int k = HitTriangle(h);
    Vector2 puv[3];
    Vector3 pos[3];
    if (k < 0 || !MS_ModelSlotPaintTriangle(scene, scene->models[h->hit.obj.index].file, h->slot, k, puv, pos)) return false;
    *uv = (Vector2){ h->bary.x * puv[0].x + h->bary.y * puv[1].x + h->bary.z * puv[2].x,
                     h->bary.x * puv[0].y + h->bary.y * puv[1].y + h->bary.z * puv[2].y };
    return true;
}

// What paint at h goes into, or why it can't be painted (docs/painting.md 3.4); *g its rectangle.
// Runs every frame (the hover, the panel's target), so it says why with a code, and RefusalPrint
// words it. Returns:
//   0  paintable (*g)
//   1  nothing there (no text)
//   2  a light; 3 nothing to paint here; 4 the map changed under the hit; 5 the material's shader
//      can't take the paint stage; 6 a skinned model; 7 a model slot that can't be painted; 8 its
//      paint entry is inactive
static unsigned char Resolve(DrawMode *dm, const MS_SurfaceHit *h, DrawGeom *g)
{
    memset(g, 0, sizeof(*g));
    g->entry = -1;
    g->x = g->y = -1;
    g->uv = (Vector2){ -1.0f, -1.0f };
    MS_ObjKind kind = h->hit.obj.kind;
    if (kind == MS_OBJ_NONE) return 1;
    if (kind == MS_OBJ_LIGHT) return 2;
    if (!HitIsBox(h) && !HitIsModel(h)) return 3;
    int m = h->material;
    if (m >= scene->materialCount) return 4;
    if (m >= 0 && !ShaderTakesPaint(dm, m)) return 5;
    int file = -1;
    if (HitIsModel(h)) {
        file = scene->models[h->hit.obj.index].file;
        MS_PaintUvKind k = MS_ModelPaintUv(scene, file, h->slot);
        if (k == MS_PAINTUV_SKINNED) return 6;
        if (k == MS_PAINTUV_INVALID || h->slot > 255) return 7;
        g->needsUnwrap = k == MS_PAINTUV_NEEDS_UNWRAP;
        g->own = m < 0;
    }
    int inactive;
    int e = EntryFor(h->hit.obj, m, h->face, h->slot, &inactive);
    if (e < 0 && inactive >= 0) return 8;
    AtlasRelOf(m, file, h->slot, &g->atlasW, &g->atlasH, &g->nearest);
    const char *rel = AtlasRelOf(m, file, h->slot, NULL, NULL, NULL);
    if (!rel || (m < 0 && g->atlasW <= 0)) g->nearest = dm->pixelLook;      // (no atlas yet: the pixel look switch)
    if (e >= 0) {
        const MS_PaintEntry *E = MS_PaintEntryAt(scene, e);
        g->entry = e;
        g->x = E->x;
        g->y = E->y;
        g->w = E->w;
        g->h = E->h;
        g->faces = E->faces;
    } else {
        g->fresh = true;
        if (HitIsBox(h)) {
            g->faces = (unsigned char)(1u << h->face);
            BoxRectSize(dm->density, h->hit.obj.index, g->faces, &g->w, &g->h);
        } else g->w = g->h = SlotRectSide(dm->density, h->hit.obj.index, h->slot);
    }
    if (!HitPaintUv(h, &g->uv)) g->uv = (Vector2){ -1.0f, -1.0f };
    g->ok = true;
    return 0;
}

// Resolve's refusal `code` at h in words, snprintf-style into out (size bytes; NULL / 0 measures);
// "" for 0 and 1. Right after Resolve (the scene as it saw it)
static int RefusalPrint(char *out, size_t size, unsigned char code, const MS_SurfaceHit *h)
{
    const char *name = code == 6 || code == 7 ? GetFileName(scene->modelFiles[scene->models[h->hit.obj.index].file].path) : "";
    int inactive = -1;
    if (code == 8) EntryFor(h->hit.obj, h->material, h->face, h->slot, &inactive);
    switch (code) {
    case 2: return snprintf(out, size, "a light: nothing to paint");
    case 3: return snprintf(out, size, "nothing to paint here");
    case 4: return snprintf(out, size, "the map changed: wait a frame");
    case 5: return snprintf(out, size, "material %s's shader can't take the paint stage (3 or more samplers, or too long for R3D)",
                            scene->materials[h->material].name);
    case 6: return snprintf(out, size, "%s is skinned: its bone channels are in use (can't be painted)", name);
    case 7: return snprintf(out, size, "%s slot %d can't be painted", name, h->slot);
    case 8: return snprintf(out, size, "its paint entry (map line %d) is inactive (see the log): fix or remove that line",
                            inactive >= 0 ? MS_PaintEntryAt(scene, inactive)->line : 0);
    default: return snprintf(out, size, "%s", "");
    }
}

// Resolve's refusal (an event: a stroke refused) on the message line, red; 1 (nothing there): fallback
static void SayRefusal(DrawMode *dm, unsigned char code, const MS_SurfaceHit *h, const char *fallback)
{
    int n = code >= 2 ? RefusalPrint(NULL, 0, code, h) : -1;
    char *text = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (text) RefusalPrint(text, (size_t)n + 1, code, h);
    if (code < 2) SayStatic(dm, true, fallback);
    else Sayf(dm, true, "%s", text ? text : "out of memory");
    free(text);
}

// #endregion

// ======================================================================================
// #region Unwraps (docs/painting.md 4.5)
// ======================================================================================

static void ProvideUnwrap(const DrawUnwrap *u)
{
    for (int f = 0; f < scene->modelFileCount; f++)
        if (strcmp(scene->modelFiles[f].path, u->modelPath) == 0) MS_PaintUnwrapProvide(scene, f, &u->u);
}

// Model file `file` gets its paint unwrap (xatlas, at the slot's rectangle size: texelsHint) when a
// slot needs one: made, kept, provided (the next live edit uses it). Returns 0 done (or not needed);
// 1 not made, why in MS_LastError() (no licence row: the sidecar derives from the model's; a
// degenerate file; out of memory)
static unsigned char EnsureUnwrap(DrawMode *dm, int file, int slot, int texels)
{
    if (MS_ModelPaintUv(scene, file, slot) != MS_PAINTUV_NEEDS_UNWRAP) return 0;
    const char *path = scene->modelFiles[file].path;
    for (int i = 0; i < dm->unwrapCount; i++)
        if (strcmp(dm->unwraps[i].modelPath, path) == 0) { ProvideUnwrap(&dm->unwraps[i]); return 0; }
    char *row = AssetsRow(path);
    if (!row) { MS_SetError("%s has no licence row in ASSETS.md (its paint unwrap derives from it)", path); return 1; }
    free(row);
    DrawUnwrap u = { 0 };
    double t0 = GetTime();
    if (!PaintUnwrapModelFile(scene, file, texels, &u.u)) {
        MS_SetError("%s can't be unwrapped for paint: %s", path, MS_LastError());     // (MS_SetError may quote the last error)
        return 1;
    }
    u.modelPath = MS_StrDup(path);
    DrawUnwrap *slotU = u.modelPath ? MS_PUSH(dm->unwraps, dm->unwrapCount, dm->unwrapCap) : NULL;
    if (!slotU) {
        free(u.modelPath);
        MS_PaintUnwrapFree(&u.u);
        MS_SetError("out of memory");
        return 1;
    }
    *slotU = u;
    ProvideUnwrap(slotU);
    TraceLog(LOG_INFO, "monolith: draw: unwrapped %s for paint in %.0f ms (%d texels)", path, (GetTime() - t0) * 1000.0, texels);
    return 0;
}

// #endregion

// ======================================================================================
// #region Allocation: the first stroke on a face or slot (EDIT_PAINT_ADD)
// ======================================================================================

// Pushes op with the step (the step's first text) or amends the step with it (a later one). Returns
// 0 pushed / amended; 1 not, why in MS_LastError() (a failed push releases the step, its pixels go
// back: dm->stroke is gone then)
static unsigned char PushOp(DrawMode *dm, PaintStep *p, bool *pushed, MS_ObjRef obj, const EditOp *op, const char *what)
{
    MapEditor *E = &dm->em->ed;
    unsigned char r;
    if (!*pushed) {
        MS_EntityId *ids;
        int count;
        r = MapEditPushPaint(E, &obj, 1, op, p, what, &ids, &count);
        free(ids);
        if (r == 0) {       // pushed
            *pushed = true;
            p->pending = false;
        }
    } else r = MapEditAmendPaint(E, &obj, 1, op);
    if (r != 0) return 1;       // (not pushed / amended: the reason is the last error)
    char stats[400];
    MapEditStatsText(&E->lastStats, stats, sizeof(stats));
    TraceLog(LOG_INFO, "monolith: draw: %s", MapEditNote(E));
    TraceLog(LOG_INFO, "monolith: live edit %s", stats);
    return 0;
}

// A new rectangle w x h for object obj's faces / slot in material m (-1: own) of step p's atlas
// (exclude: an entry whose rectangle may be reused, -1): the atlas grows when needed, the op goes
// in (pushed with the step, or amended onto it), the rectangle is cleared. *rx *ry where it went.
// Returns 0 done; 1 not, why in MS_LastError() (when the step wasn't pushed yet it is released then:
// its pixels go back)
static unsigned char AddRect(DrawMode *dm, PaintStep *p, bool *pushed, MS_ObjRef obj, int m, unsigned char faces, int slot, int w, int h,
                             int exclude, EditOpKind kind, int *rx, int *ry)
{
    Atlas *a = &dm->atlases[p->atlas];
    int x, y, W, H;
    if (!AllocRect(a->rel, a->img.width, a->img.height, w, h, exclude, &x, &y, &W, &H)) {
        MS_SetError("the paint atlas of %s is full at %dx%d: lower the density", a->material ? a->material : a->rel,
                    DRAW_ATLAS_MAX, DRAW_ATLAS_MAX);
        if (!*pushed) ReleaseStep(dm, p);
        return 1;
    }
    int oldW = a->img.width, oldH = a->img.height;
    bool wasProvided = a->provided;
    if (!AtlasResize(a, W, H) || !TilesRemap(a, p)) {
        MS_SetError("out of memory");
        if (!*pushed) ReleaseStep(dm, p);
        return 1;
    }
    Provide(a);
    // The op: the material gets its paint key with its first entry (the pixel look then too)
    const char *mat = m >= 0 ? scene->materials[m].name : NULL;
    const char *rel = m >= 0 ? AtlasRelOf(m, -1, -1, NULL, NULL, NULL) : NULL;
    EditOp op = { .kind = kind };
    op.paint.material = mat;
    op.paint.atlas = a->rel;
    op.paint.faces = faces;
    op.paint.slot = obj.kind == MS_OBJ_MODEL ? slot : -1;
    op.paint.rect[0] = x;
    op.paint.rect[1] = y;
    op.paint.rect[2] = w;
    op.paint.rect[3] = h;
    op.paint.filter = -1;
    bool ownEntries = false;
    if (!mat) {
        int n = MS_PaintEntryCount(scene);
        for (int i = 0; i < n && !ownEntries; i++) {
            const MS_PaintEntry *e = MS_PaintEntryAt(scene, i);
            const char *er = e->material < 0 ? EntryAtlasRel(e) : NULL;
            ownEntries = er && strcmp(er, a->rel) == 0;
        }
        bool near;
        AtlasRelOf(-1, scene->models[obj.index].file, slot, NULL, NULL, &near);
        op.paint.nearest = ownEntries ? near : dm->pixelLook;
    } else op.paint.nearest = !rel && dm->pixelLook;
    char *what = MonoStrf("a %dx%d rectangle in %s", w, h, GetFileName(a->rel));
    bool haveWhat = what != NULL;
    unsigned char r = haveWhat ? PushOp(dm, p, pushed, obj, &op, what) : 1;
    free(what);
    if (r != 0) {       // (not pushed)
        // (the atlas as it was; a failed push released the step, which put its pixels back; the
        // reason is kept across the restore)
        char *why = haveWhat ? MS_StrDup(MS_LastError()) : NULL;
        if (!haveWhat && !*pushed) ReleaseStep(dm, p);
        AtlasResize(a, oldW > 0 ? oldW : W, oldH > 0 ? oldH : H);
        TilesInit(a);
        if (wasProvided) Provide(a);
        else Unprovide(a);
        MS_SetError("%s", why ? why : "out of memory");
        free(why);
        return 1;
    }
    if (!p->sized) {
        p->oldW = oldW;
        p->oldH = oldH;
        p->created = !wasProvided;
        p->sized = true;
    }
    p->newW = W;
    p->newH = H;
    ClearRect(a, p, x, y, w, h);
    *rx = x;
    *ry = y;
    return 0;
}

// #endregion

// ======================================================================================
// #region Strokes: begin, samples along the cursor's path, end
// ======================================================================================

typedef struct {
    Vector2 px;
    bool ok;                // on the stroke's surface
    Sample s;
} Probe;

static MS_SurfaceHit CastAt(Camera3D cam, int w, int h, Vector2 px)
{
    Ray ray = GetScreenToWorldRayEx(px, cam, w, h);
    MS_SurfaceHit hit;
    MS_RaycastSurface(scene, ray.position, ray.direction, 0.0f, &hit);
    return hit;
}

static void StrokeAbort(DrawMode *dm)
{
    dm->stroke = NULL;
    dm->stroking = false;
    dm->strokePushed = false;
}

// The sample of hit h for the stroke going on: on its object and material (a box: any face; a
// model: its slot). A face without an entry gets one (its first stroke: the stroke's step holds the
// text). False: not on the stroke's surface, or it can't be painted (said)
static bool StrokeSample(DrawMode *dm, const MS_SurfaceHit *h, Sample *s)
{
    if (!dm->stroking || !dm->stroke) return false;
    if (h->hit.obj.kind != dm->strokeObj.kind || h->hit.obj.index != dm->strokeObj.index || h->material != dm->strokeMaterial) return false;
    bool model = HitIsModel(h);
    if (model ? h->slot != dm->strokeSlot : !HitIsBox(h)) return false;
    PaintStep *p = dm->stroke;
    int e = EntryFor(h->hit.obj, h->material, h->face, h->slot, NULL);
    if (e < 0) {
        int w, hh, x, y;
        if (model) w = hh = SlotRectSide(dm->density, h->hit.obj.index, h->slot);
        else BoxRectSize(dm->density, h->hit.obj.index, (unsigned char)(1u << h->face), &w, &hh);
        if (AddRect(dm, p, &dm->strokePushed, h->hit.obj, h->material, model ? 0 : (unsigned char)(1u << h->face), h->slot, w, hh, -1,
                    EDIT_PAINT_ADD, &x, &y) != 0) {     // (not added: why in the last error)
            Sayf(dm, true, "%s", MS_LastError());
            if (!dm->strokePushed) StrokeAbort(dm);
            return false;
        }
        e = EntryFor(h->hit.obj, h->material, h->face, h->slot, NULL);
        if (e < 0) {
            SayStatic(dm, true, "the new paint entry doesn't draw (see the log)");
            return false;
        }
    }
    const MS_PaintEntry *E = MS_PaintEntryAt(scene, e);
    Vector2 uv;
    if (!HitPaintUv(h, &uv)) return false;
    PaintRegion r = { E->x, E->y, E->w, E->h, -1, -1, -1 };
    if (model) {
        int file = scene->models[h->hit.obj.index].file;
        const DrawIslands *I = IslandsFor(dm, file, h->slot, E->w, E->h);
        int k = HitTriangle(h);
        r.file = file;
        r.slot = h->slot;
        r.island = I && k >= 0 && k < I->triCount ? I->triIsland[k] : -1;
        if (r.island < 0) return false;
    }
    s->region = RegionIndex(p, r);
    s->tx = E->x + Clamp01(uv.x) * E->w;
    s->ty = E->y + Clamp01(uv.y) * E->h;
    s->joined = false;
    return s->region >= 0;
}

static Probe ProbeHit(DrawMode *dm, const MS_SurfaceHit *h, Vector2 px)
{
    Probe p = { .px = px };
    p.ok = StrokeSample(dm, h, &p.s);
    return p;
}

// A stamp at the probe: joined to the stroke's sample before it when that is on the same region and
// near (the subdivision's spacing, with room for a frame cut at its depth limit), so the pen sweeps
// the segment between them
static void StampProbe(DrawMode *dm, const Probe *pr)
{
    if (!dm->stroke) return;
    PaintStep *st = dm->stroke;
    Atlas *a = &dm->atlases[st->atlas];
    Sample s = pr->s;
    const Sample *prev = st->sampleCount > 0 ? &st->samples[st->sampleCount - 1] : NULL;
    if (prev) s.joined = prev->region == s.region && hypotf(s.tx - prev->tx, s.ty - prev->ty) <= 2.0f * Spacing(dm) + 1.0f;
    Stamp(dm, a, st, s, s.joined ? prev : NULL);
    Sample *slot = MS_PUSH(st->samples, st->sampleCount, st->sampleCap);
    if (slot) *slot = s;
}

// The cursor moved from a to b this frame: samples between them, cut in screen space until
// consecutive hits on the same region are at most `spacing` texels apart (docs/painting.md 7.1)
static void StrokeSegment(DrawMode *dm, Camera3D cam, int w, int h, Probe a, Probe b)
{
    struct { Probe a, b; int depth; } stack[2 * SUBDIV_DEPTH + 4];
    int n = 0;
    stack[n].a = a;
    stack[n].b = b;
    stack[n++].depth = 0;
    float spacing = Spacing(dm);
    while (n > 0 && dm->stroking) {
        Probe A = stack[n - 1].a, B = stack[n - 1].b;
        int d = stack[--n].depth;
        float dpx = Vector2Distance(A.px, B.px);
        bool close = A.ok && B.ok && A.s.region == B.s.region && hypotf(A.s.tx - B.s.tx, A.s.ty - B.s.ty) <= spacing;
        if (close || d >= SUBDIV_DEPTH || dpx < 0.25f || (!A.ok && !B.ok && d >= 3) || n + 2 > (int)(sizeof(stack) / sizeof(stack[0]))) {
            if (B.ok) StampProbe(dm, &B);
            continue;
        }
        Vector2 mid = Vector2Scale(Vector2Add(A.px, B.px), 0.5f);
        MS_SurfaceHit hm = CastAt(cam, w, h, mid);
        Probe M = ProbeHit(dm, &hm, mid);
        stack[n].a = M; stack[n].b = B; stack[n++].depth = d + 1;
        stack[n].a = A; stack[n].b = M; stack[n++].depth = d + 1;
    }
}

// LMB down on hit: the stroke's atlas (opened or new) and step; the first sample (which allocates
// a rectangle on a surface without one) is the caller's. False with the reason said
static bool StrokeBegin(DrawMode *dm, EditMode *em, const MS_SurfaceHit *h)
{
    DrawGeom g;
    unsigned char refused = Resolve(dm, h, &g);
    if (refused != 0) { SayRefusal(dm, refused, h, "nothing to paint here"); return false; }
    if (!em->ed.work && !MapEditBegin(&em->ed)) { SayStatic(dm, true, "this map can't be painted (a compiled map?): load its .map"); return false; }
    bool model = HitIsModel(h);
    int file = model ? scene->models[h->hit.obj.index].file : -1;
    if (g.needsUnwrap && EnsureUnwrap(dm, file, h->slot, g.w) != 0) { Sayf(dm, true, "%s", MS_LastError()); return false; }
    int m = h->material;
    char rel[MS_PATH_MAX];
    const char *have = AtlasRelOf(m, file, h->slot, NULL, NULL, NULL);
    if (have) snprintf(rel, sizeof(rel), "%s", have);
    else NewAtlasRel(dm, scene->materials[m].name, rel, (int)sizeof(rel));
    int ai = AtlasOpen(dm, rel, m >= 0 ? scene->materials[m].name : NULL, model ? scene->modelFiles[file].path : NULL, h->slot);
    if (ai < 0) { SayStatic(dm, true, "out of memory"); return false; }
    PaintStep *p = NewStep(dm, ai, STEP_STROKE);
    if (!p) { SayStatic(dm, true, "out of memory"); return false; }
    dm->stroke = p;
    dm->strokePushed = false;
    dm->strokeObj = h->hit.obj;
    dm->strokeMaterial = m;
    dm->strokeSlot = model ? h->slot : -1;
    dm->strokeAtlas = ai;
    dm->stroking = true;
    dm->lastHit = *h;
    dm->lastHitSet = true;
    return true;
}

// LMB up: the step goes into the history (one that allocated is there already); auto save writes
static bool StrokeEnd(DrawMode *dm, EditMode *em)
{
    if (!dm->stroking) return false;
    PaintStep *p = dm->stroke;
    Atlas *a = &dm->atlases[p->atlas];
    Flush(dm);
    EndTiles(a, p);
    int tiles = p->tileCount;
    const char *verb = p->pen == PEN_ERASER ? "erased" : p->pen == PEN_FILL ? "filled" : "painted";
    if (!dm->strokePushed) {
        if (tiles > 0) {
            MS_EntityId *ids;
            int count;
            char *what = MonoStrf("%s %d tile%s of %s", verb, tiles, tiles == 1 ? "" : "s", GetFileName(a->rel));
            // (out of memory for the text: the history's own "painted")
            unsigned char r = MapEditPushPaint(&em->ed, &dm->strokeObj, 1, NULL, p, what, &ids, &count);
            free(what);
            free(ids);
            if (r == 0) p->pending = false;     // pushed
            Sayf(dm, r != 0, "%s", r == 0 ? MapEditNote(&em->ed) : MS_LastError());
        } else ReleaseStep(dm, p);
    } else Sayf(dm, false, "%s %d tile%s of %s", verb, tiles, tiles == 1 ? "" : "s", GetFileName(a->rel));
    StrokeAbort(dm);
    if (em->ed.autoSave && tiles > 0 && DrawModeSave(dm, em) == 2)     // (2: not saved)
        Sayf(dm, true, "auto save: %s", MS_LastError());
    return true;
}

// #endregion

// ======================================================================================
// #region The history's hooks (mapedit.c calls them): undo, redo, rebase, release, save
// ======================================================================================

static void SwapTiles(Atlas *a, PaintStep *p, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = *y0 = INT32_MAX;
    *x1 = *y1 = -1;
    for (int i = 0; i < p->tileCount; i++) {
        PaintTile *t = &p->tiles[i];
        if ((t->tx * DRAW_TILE + t->w) > a->img.width || (t->ty * DRAW_TILE + t->h) > a->img.height) continue;
        for (int r = 0; r < t->h; r++) {
            unsigned char *x = t->pixels + (size_t)r * t->w * 4;
            unsigned char *y = (unsigned char *)a->img.data + ((size_t)(t->ty * DRAW_TILE + r) * a->img.width + t->tx * DRAW_TILE) * 4;
            for (int k = 0; k < t->w * 4; k++) { unsigned char v = x[k]; x[k] = y[k]; y[k] = v; }
        }
        *x0 = t->tx * DRAW_TILE < *x0 ? t->tx * DRAW_TILE : *x0;
        *y0 = t->ty * DRAW_TILE < *y0 ? t->ty * DRAW_TILE : *y0;
        *x1 = t->tx * DRAW_TILE + t->w > *x1 ? t->tx * DRAW_TILE + t->w : *x1;
        *y1 = t->ty * DRAW_TILE + t->h > *y1 ? t->ty * DRAW_TILE + t->h : *y1;
    }
}

// Undo: the tiles go back, the atlas to its size before the step (a step that gave the runtime the
// atlas takes it out again: the material loses its paint key with the text). Redo: the other way.
// Both run before the step's text goes live, so the image the text's material loads is provided
static bool HookSwap(DrawMode *dm, PaintStep *p, bool undo)
{
    if (p->atlas < 0 || p->atlas >= dm->atlasCount) return false;
    Atlas *a = &dm->atlases[p->atlas];
    int fromW = undo ? p->newW : p->oldW, fromH = undo ? p->newH : p->oldH;
    int toW = undo ? p->oldW : p->newW, toH = undo ? p->oldH : p->newH;
    // (the image is the step's size on the side it starts from; the larger one while tiles swap)
    int bigW = fromW > toW ? fromW : toW, bigH = fromH > toH ? fromH : toH;
    if (bigW > 0 && bigH > 0 && !AtlasResize(a, bigW, bigH)) return false;
    int x0, y0, x1, y1;
    SwapTiles(a, p, &x0, &y0, &x1, &y1);
    if (toW > 0 && toH > 0 && !AtlasResize(a, toW, toH)) return false;
    if (!TilesInit(a)) return false;
    if (undo && p->created) {
        Unprovide(a);
        return true;
    }
    Provide(a);
    if (a->img.data) Upload(a, x0, y0, x1, y1);
    return a->provided;
}

static bool HookUndo(void *user, PaintStep *p) { return HookSwap(user, p, true); }
static bool HookRedo(void *user, PaintStep *p) { return HookSwap(user, p, false); }
static void HookRelease(void *user, PaintStep *p) { ReleaseStep(user, p); }
static long long HookBytes(void *user, const PaintStep *p) { (void)user; return StepBytes(p); }
// MapEditSaveAll's values, as DrawModeSave's: 0 saved, 1 nothing to save (the editor's note); 2 not
// saved (the last error)
static unsigned char HookSave(void *user)
{
    DrawMode *dm = user;
    return DrawModeSave(dm, dm->em);
}

// A replay (rebase onto a changed PNG): the stroke done again on the atlas as it is now; its undo
// copy (tiles) taken anew. A panel edit (faces, resize, clear) isn't replayed
static bool ReplayStep(DrawMode *dm, PaintStep *p)
{
    Atlas *a = &dm->atlases[p->atlas];
    if (p->kind != STEP_STROKE) return false;
    FreeTiles(p);
    if (!TilesInit(a)) return false;
    for (int i = 0; i < p->sampleCount; i++) Stamp(dm, a, p, p->samples[i], i > 0 && p->samples[i].joined ? &p->samples[i - 1] : NULL);
    EndTiles(a, p);
    a->dirty = false;
    return true;
}

// 0 the paint is provided again, 1 it can't be (why: the last error)
static unsigned char HookRebase(void *user, PaintStep *p)
{
    DrawMode *dm = user;
    if (p->atlas < 0 || p->atlas >= dm->atlasCount) { MS_SetError("its atlas is gone"); return 1; }
    Atlas *a = &dm->atlases[p->atlas];
    if (a->rebaseGen != dm->rebaseGen) {
        // The first step of this atlas in this rebase: is its PNG still what it was?
        a->rebaseGen = dm->rebaseGen;
        a->replaying = false;
        a->replayed = 0;
        char path[MS_PATH_MAX + MS_PATH_MAX];
        snprintf(path, sizeof(path), "%s%s", scene->root, a->rel);
        Image disk = FileExists(path) ? AdoptImage(LoadImage(path)) : (Image){ 0 };
        if (disk.data && PixelHash(&disk) != a->savedHash) {
            FreeImage(&a->img);
            a->img = disk;
            a->savedHash = PixelHash(&disk);
            a->replaying = true;
            if (!TilesInit(a)) { MS_SetError("out of memory"); return 1; }
        } else FreeImage(&disk);
    }
    // (the atlas at least the size the step left it: the rectangles of its text fit)
    if (p->newW > 0 && (a->img.width < p->newW || a->img.height < p->newH)) {
        if (!AtlasResize(a, a->img.width > p->newW ? a->img.width : p->newW, a->img.height > p->newH ? a->img.height : p->newH) ||
            !TilesInit(a)) {
            MS_SetError("out of memory");
            return 1;
        }
    }
    // (a step that gave the runtime the atlas makes its key again; any other needs it in the text)
    if (!p->created && !AtlasLive(a)) { MS_SetError("no material paints into %s any more", a->rel); return 1; }
    if (a->replaying) {
        if (!ReplayStep(dm, p)) {
            MS_SetError("%s changed on disk and this edit of it can't be replayed", a->rel);
            return 1;
        }
        a->replayed++;
    }
    Provide(a);
    Upload(a, 0, 0, a->img.width, a->img.height);
    return 0;
}

// #endregion

// ======================================================================================
// #region Saving: PNGs, unwrap sidecars, licence records (docs/painting.md 11)
// ======================================================================================

static void Today(char *out, int size)
{
    time_t now = time(NULL);
    const struct tm *tm = localtime(&now);
    if (tm) strftime(out, (size_t)size, "%Y-%m-%d", tm);
    else snprintf(out, (size_t)size, "-");
}

// The cells of an ASSETS.md row after its file cell: source, author, licence, date, sha256, changes
// (heap, sized to fit; a missing cell is ""). False when out of memory (the cells made are freed)
static bool RowCells(const char *row, char *cells[6])
{
    bool ok = true;
    const char *p = row;
    for (int i = 0; i < 6; i++) {
        cells[i] = NULL;
        if (!p || !*p) { cells[i] = MS_StrDup(""); ok = ok && cells[i]; continue; }
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && !(*e == '|' && (e == p || e[-1] != '\\'))) e++;
        const char *t = e;
        while (t > p && t[-1] == ' ') t--;
        cells[i] = MS_StrNDup(p, (size_t)(t - p));
        ok = ok && cells[i];
        p = *e ? e + 1 : e;
    }
    if (!ok)
        for (int i = 0; i < 6; i++) { free(cells[i]); cells[i] = NULL; }
    return ok;
}

// Puts `row` (whose first cell is key) into the ASSETS.md at path: over the file's row, else
// appended to the table (made, with the file, when there is none)
static bool PutAssetsRow(const char *path, const char *key, const char *rowText, const char *pkg)
{
    char *text = FileExists(path) ? LoadFileText(path) : NULL;
    Sb out = { 0 };
    Sb row = { 0 };
    SbStr(&row, rowText);
    static const char *HEADER = "| File | Source URL | Author | Licence (+ URL) | Downloaded | sha256 of the original | Changes |";
    bool placed = false, inTable = false, hasHeader = text && strstr(text, "| File |") != NULL;
    if (!text) SbFmt(&out, "# %s: external assets\n", pkg);
    for (const char *l = text; l && *l; ) {
        const char *nl = strchr(l, '\n');
        int len = nl ? (int)(nl - l) : (int)strlen(l);
        bool tableLine = len > 0 && l[0] == '|';
        if (!placed && inTable && !tableLine) { SbAdd(&out, row.s, row.len); SbStr(&out, "\n"); placed = true; }
        inTable = tableLine && hasHeader;
        if (!placed && len >= (int)strlen(key) && strncmp(l, key, strlen(key)) == 0) {
            SbAdd(&out, row.s, row.len);
            placed = true;
        } else SbAdd(&out, l, len);
        if (nl) SbStr(&out, "\n");
        l = nl ? nl + 1 : NULL;
    }
    if (!placed) {
        if (out.len && out.s[out.len - 1] != '\n') SbStr(&out, "\n");
        if (!hasHeader) SbFmt(&out, "\n%s\n|---|---|---|---|---|---|---|\n", HEADER);
        SbAdd(&out, row.s, row.len);
        SbStr(&out, "\n");
    }
    bool ok = !out.oom && !row.oom && SaveFileText(path, out.s);
    if (text) UnloadFileText(text);
    free(out.s);
    free(row.s);
    return ok;
}

// The row and sidecar of a file the draw mode made (docs/painting.md 11.2): an atlas (made in
// monolith, CC0) or an unwrap sidecar (derived from its model's row: its author, licence)
static bool RecordFile(const char *rel, const char *derivedFrom, const char *size)
{
    char path[MS_PATH_MAX + 32], date[32], pkg[256], key[MS_PATH_MAX + 8];
    snprintf(path, sizeof(path), "%sASSETS.md", scene->root);
    Today(date, sizeof(date));
    PackageName(pkg, sizeof(pkg));
    snprintf(key, sizeof(key), "| `%s` |", rel);
    Sb row = { 0 }, sc = { 0 };
    char *cells[6] = { 0 };
    if (derivedFrom) {
        char *mrow = AssetsRow(derivedFrom);
        if (!mrow) return false;
        bool got = RowCells(mrow, cells);
        free(mrow);
        if (!got) return false;
        SbFmt(&row, "%s paint unwrap of `%s`, generated in monolith | %s | %s | %s | - | made from the model by monolith's draw mode (xatlas) |",
              key, derivedFrom, cells[1], cells[2], date);
    } else
        SbFmt(&row, "%s made in monolith (painted by hand) | %s | CC0 1.0, https://creativecommons.org/publicdomain/zero/1.0/ | %s | - | none |",
              key, pkg, date);
    SbFmt(&sc, "# %s\n\nProvenance of `%s` in this package (written by monolith's draw mode).\n\n| Field | Value |\n|---|---|\n", rel, rel);
    SbFmt(&sc, "| Name | %s |\n| Package path | `%s` |\n", GetFileName(rel), rel);
    if (derivedFrom) {
        SbFmt(&sc, "| Source | paint unwrap of `%s`, generated in monolith |\n", derivedFrom);
        SbFmt(&sc, "| Author(s) | %s |\n| Licence | %s |\n", cells[1], cells[2]);
    } else {
        SbStr(&sc, "| Source | made in monolith (painted by hand) |\n");
        SbFmt(&sc, "| Author(s) | %s (the package) |\n| Licence | CC0 1.0 (https://creativecommons.org/publicdomain/zero/1.0/) |\n", pkg);
    }
    SbFmt(&sc, "| Recorded | %s (by monolith's draw mode) |\n| AI-generated | no |\n| Size | %s |\n", date, size);
    SbFmt(&sc, "\n## Changes to the file\n\n- %s\n", derivedFrom ? "generated from the model's meshes by monolith's draw mode (xatlas charts)"
                                                              : "painted by hand in monolith's draw mode");
    bool ok = !row.oom && !sc.oom && PutAssetsRow(path, key, row.s, pkg);
    char scPath[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(scPath, sizeof(scPath), "%s%s.asset.md", scene->root, rel);
    ok = ok && SaveFileText(scPath, sc.s);
    free(row.s);
    free(sc.s);
    for (int i = 0; i < 6; i++) free(cells[i]);
    return ok;
}

// The sidecar path of a model file: its extension replaced by .paintuv (docs/painting.md 4.5)
static void SidecarRel(const char *modelPath, char *out, int size)
{
    const char *dot = strrchr(modelPath, '.'), *slash = strrchr(modelPath, '/');
    int n = dot && (!slash || dot > slash) ? (int)(dot - modelPath) : (int)strlen(modelPath);
    snprintf(out, (size_t)size, "%.*s.paintuv", n, modelPath);
}

// The text paints an instance of this model file
static bool UnwrapUsed(const DrawUnwrap *u)
{
    int n = MS_PaintEntryCount(scene);
    for (int i = 0; i < n; i++) {
        const MS_PaintEntry *e = MS_PaintEntryAt(scene, i);
        if (e->obj.kind == MS_OBJ_MODEL && e->obj.index >= 0 && e->obj.index < scene->modelCount &&
            strcmp(scene->modelFiles[scene->models[e->obj.index].file].path, u->modelPath) == 0)
            return true;
    }
    return false;
}

// A save's failure (only the last one is said): *text replaced (out of memory: NULL), *failed set
static void SaveFailed(bool *failed, char **text, const char *fmt, ...) MSG_PRINTF(3, 4);
static void SaveFailed(bool *failed, char **text, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    free(*text);
    *text = MonoStrv(fmt, ap);
    va_end(ap);
    *failed = true;
}

unsigned char DrawModeSave(DrawMode *dm, EditMode *em)
{
    MapEditor *E = &em->ed;
    if (dm->stroking) StrokeEnd(dm, em);
    if (E->work && !MapEditDiskUnchanged(E)) {
        MS_SetError("not saved: %s changed on disk (a request? another editor?); it reloads and the edits are applied again",
                    scene->path);
        return 2;           // (not logged here: the caller says it, as before)
    }
    int written = 0, records = 0;
    bool failed = false;
    char *failedText = NULL;
    char dir[MS_PATH_MAX + 32];
    snprintf(dir, sizeof(dir), "%s" PAINT_DIR, scene->root);
    // 1. the atlases (straight alpha RGBA8), 2. the unwrap sidecars
    for (int i = 0; i < dm->atlasCount; i++) {
        Atlas *a = &dm->atlases[i];
        if (!AtlasLive(a) || !AtlasUnsaved(a)) continue;
        char path[MS_PATH_MAX + MS_PATH_MAX];
        snprintf(path, sizeof(path), "%s%s", scene->root, a->rel);
        if (!DirectoryExists(dir)) MakeDirectory(dir);
        if (!PngWrite(a->img, path)) { SaveFailed(&failed, &failedText, "cannot write %s", a->rel); continue; }
        a->savedHash = PixelHash(&a->img);
        a->written = true;
        written++;
    }
    for (int i = 0; i < dm->unwrapCount; i++) {
        DrawUnwrap *u = &dm->unwraps[i];
        if (u->written || !UnwrapUsed(u)) continue;
        char rel[MS_PATH_MAX], path[MS_PATH_MAX + MS_PATH_MAX];
        SidecarRel(u->modelPath, rel, (int)sizeof(rel));
        snprintf(path, sizeof(path), "%s%s", scene->root, rel);
        if (!MS_PaintUnwrapWrite(path, &u->u)) { SaveFailed(&failed, &failedText, "cannot write %.200s", rel); continue; }
        u->written = true;
        written++;
        if (RecordFile(rel, u->modelPath, TextFormat("%d meshes, binary (MSPAINTUV 1)", u->u.meshCount))) records++;
        else SaveFailed(&failed, &failedText, "cannot record the licence of %.150s in ASSETS.md", rel);
    }
    // 3. the map (its text: the editor's note, or the last error when not saved), 4. the licence
    // records of new atlases
    unsigned char saved = MapEditSave(E);
    for (int i = 0; saved != 2 && i < dm->atlasCount; i++) {       // (1, nothing to save, is no failure)
        Atlas *a = &dm->atlases[i];
        if (a->recorded || !a->written || !AtlasLive(a) || AtlasUnsaved(a)) continue;
        if (RecordFile(a->rel, NULL, TextFormat("%d x %d px, RGBA8 PNG (straight alpha)", a->img.width, a->img.height))) {
            a->recorded = true;
            records++;
        } else SaveFailed(&failed, &failedText, "cannot record the licence of %s in ASSETS.md", a->rel);
    }
    // 5. the map's watch list: the files as written (MapEditSave did it when the text was unsaved)
    if (saved != 2 && written > 0 && MapEditDiskUnchanged(E)) MS_EditMarkSaved(scene);
    // The text: the map's, then what the paint files did (code 2: all of it in the last error)
    unsigned char code;
    if (saved == 2) {
        // (MapEditSave's reason is still the last error: nothing since touched it)
        if (written) MS_AppendError(" (%d paint file%s written)", written, written == 1 ? "" : "s");
        code = 2;
    } else if (written == 0 && records == 0) {
        // the map's own text (a paint file that failed makes it a failure, in the same words)
        if (failed) MS_SetError("%s", MapEditNote(E));
        code = failed ? 2 : saved == 1 ? 1 : 0;
    } else {
        char *text = MonoStrf("%s; %d paint file%s written%s%s%s%s", saved == 1 ? "saved" : MapEditNote(E), written, written == 1 ? "" : "s",
                              records ? TextFormat(", %d licence record%s", records, records == 1 ? "" : "s") : "", failed ? " (" : "",
                              !failed ? "" : failedText ? failedText : "out of memory", failed ? ")" : "");
        if (failed) MS_SetError("%s", text ? text : "out of memory");
        else if (text) MsgSetf(&E->note, "%s", text);
        else MsgSetStatic(&E->note, "out of memory");
        free(text);
        code = failed ? 2 : 0;
    }
    free(failedText);
    TraceLog(code != 2 ? LOG_INFO : LOG_WARNING, "monolith: %s", code == 2 ? MS_LastError() : MapEditNote(E));
    return code;
}

int DrawModeUnsavedAtlases(const DrawMode *dm)
{
    int n = 0;
    for (int i = 0; i < dm->atlasCount; i++)
        if (AtlasLive(&dm->atlases[i]) && AtlasUnsaved(&dm->atlases[i])) n++;
    return n;
}

bool DrawModeWriteUnsaved(const DrawMode *dm, const char *dir)
{
    bool ok = true;
    for (int i = 0; i < dm->atlasCount; i++) {
        const Atlas *a = &dm->atlases[i];
        if (!AtlasLive(a) || !AtlasUnsaved(a)) continue;
        char path[MS_PATH_MAX * 3];
        snprintf(path, sizeof(path), "%s/%s", dir, a->rel);
        char folder[MS_PATH_MAX * 3];
        snprintf(folder, sizeof(folder), "%s", GetDirectoryPath(path));
        if (!DirectoryExists(folder)) MakeDirectory(folder);
        bool w = PngWrite(a->img, path);
        TraceLog(w ? LOG_WARNING : LOG_ERROR, "monolith: unsaved paint %s %s %s", a->rel, w ? "written to" : "could not be written to", path);
        ok = ok && w;
    }
    for (int i = 0; i < dm->unwrapCount; i++) {
        const DrawUnwrap *u = &dm->unwraps[i];
        if (u->written || !UnwrapUsed(u)) continue;
        char rel[MS_PATH_MAX], path[MS_PATH_MAX * 3], folder[MS_PATH_MAX * 3];
        SidecarRel(u->modelPath, rel, (int)sizeof(rel));
        snprintf(path, sizeof(path), "%s/%s", dir, rel);
        snprintf(folder, sizeof(folder), "%s", GetDirectoryPath(path));
        if (!DirectoryExists(folder)) MakeDirectory(folder);
        ok = MS_PaintUnwrapWrite(path, &u->u) && ok;
    }
    return ok;
}

// #endregion

// ======================================================================================
// #region Entry edits: face groups, resize to density, pixel look, clear (docs/painting.md 5.3)
// ======================================================================================

// A panel edit's step on the atlas of material m (-1: own atlas of file / slot); NULL when out of
// memory or the surface has no atlas (said)
static PaintStep *EditStepOn(DrawMode *dm, int m, int file, int slot, const char *what)
{
    const char *rel = AtlasRelOf(m, file, slot, NULL, NULL, NULL);
    if (!rel) { Sayf(dm, true, "%s: this surface has no paint yet", what); return NULL; }
    char copy[MS_PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", rel);
    int ai = AtlasOpen(dm, copy, m >= 0 ? scene->materials[m].name : NULL, file >= 0 ? scene->modelFiles[file].path : NULL, slot);
    PaintStep *p = ai >= 0 ? NewStep(dm, ai, STEP_EDIT) : NULL;
    if (!p) { SayStatic(dm, true, "out of memory"); return NULL; }
    if (!dm->atlases[ai].img.data) {
        ReleaseStep(dm, p);
        Sayf(dm, true, "%s: the atlas %s has no image (see the log)", what, copy);
        return NULL;
    }
    p->sized = true;
    return p;
}

// After a panel edit's ops: the step's tiles end; not pushed: nothing changed (released)
static DrawModeResult EditStepEnd(DrawMode *dm, PaintStep *p, bool pushed, const char *msgOk)
{
    Atlas *a = &dm->atlases[p->atlas];
    Flush(dm);
    EndTiles(a, p);
    if (!pushed) { ReleaseStep(dm, p); return DRAWMODE_NONE; }
    if (msgOk) Sayf(dm, false, "%s", msgOk);
    return DRAWMODE_RELOADED;
}

// The surface a panel action acts on: a box or a model slot with paint (its entry, active)
static bool ActionEntry(DrawMode *dm, const MS_SurfaceHit *h, const char *what, int *entry)
{
    if (!HitIsBox(h) && !HitIsModel(h)) {
        Sayf(dm, true, "%s: point at a painted box or model first (the panel acts on the surface the cursor rests on)", what);
        return false;
    }
    if (!dm->em->ed.work && !MapEditBegin(&dm->em->ed)) { SayStatic(dm, true, "this map can't be painted (a compiled map?)"); return false; }
    *entry = EntryFor(h->hit.obj, h->material, h->face, h->slot, NULL);
    if (*entry < 0) { Sayf(dm, true, "%s: %s has no paint yet: paint it first", what, HitIsBox(h) ? "this face" : "this slot"); return false; }
    return true;
}

// The face groups of box hit h in its material (DrawTarget.groups): faces in one entry share a
// number, a face without paint has one of its own; numbered in face order from 0
static void BoxGroups(const MS_SurfaceHit *h, unsigned char groups[6], int entries[6])
{
    int n = MS_PaintEntryCount(scene), next = 0;
    for (int f = 0; f < 6; f++) { groups[f] = 255; if (entries) entries[f] = -1; }
    for (int f = 0; f < 6; f++) {
        if (groups[f] != 255) continue;
        int e = -1;
        for (int i = 0; i < n && e < 0; i++) {
            const MS_PaintEntry *E = MS_PaintEntryAt(scene, i);
            if (E->obj.kind == MS_OBJ_BOX && E->obj.index == h->hit.obj.index && E->material == h->material && (E->faces & (1u << f))) e = i;
        }
        if (e < 0) { groups[f] = (unsigned char)next++; continue; }
        const MS_PaintEntry *E = MS_PaintEntryAt(scene, e);
        for (int g = 0; g < 6; g++)
            if (E->faces & (1u << g)) {
                groups[g] = (unsigned char)next;
                if (entries) entries[g] = e;
            }
        next++;
    }
}

// The panel's Faces (docs/painting.md 5.3): faces with the same number share one rectangle. Each
// group with paint keeps the entry that holds most of it (EDIT_PAINT_SET: the others' faces join);
// a group split from an entry that another group kept gets a new rectangle of the same size with a
// copy of its paint (EDIT_PAINT_ADD); a group without paint stays without; a rectangle no face uses
// any more is cleared. One undo step
static DrawModeResult ActFaces(DrawMode *dm, const MS_SurfaceHit *h, const unsigned char want[6])
{
    if (!HitIsBox(h)) { SayStatic(dm, true, "faces: point at a box (its faces are grouped here)"); return DRAWMODE_NONE; }
    if (!dm->em->ed.work && !MapEditBegin(&dm->em->ed)) { SayStatic(dm, true, "this map can't be painted (a compiled map?)"); return DRAWMODE_NONE; }
    // The entries before (a snapshot: the ops change the scene's list)
    typedef struct { int x, y, w, h; unsigned char faces; bool kept; } Snap;
    Snap snaps[6];
    int snapCount = 0, entries[6];
    unsigned char groups[6], g[6];
    BoxGroups(h, groups, entries);
    for (int f = 0; f < 6; f++) {
        if (entries[f] < 0) continue;
        const MS_PaintEntry *E = MS_PaintEntryAt(scene, entries[f]);
        bool seen = false;
        for (int s = 0; s < snapCount && !seen; s++) seen = snaps[s].faces == E->faces;
        if (!seen) snaps[snapCount++] = (Snap){ E->x, E->y, E->w, E->h, E->faces, false };
    }
    if (snapCount == 0) { SayStatic(dm, true, "faces: this box has no paint yet: paint it first, then group its faces"); return DRAWMODE_NONE; }
    for (int f = 0; f < 6; f++) g[f] = want[f] < 6 ? want[f] : 0;
    PaintStep *p = EditStepOn(dm, h->material, -1, -1, "faces");
    if (!p) return DRAWMODE_NONE;
    Atlas *a = &dm->atlases[p->atlas];
    bool pushed = false, ok = true;
    char *why = NULL;       // (a failed op's reason, kept from the last error: heap, NULL when out of memory)
    for (int n = 0; n < 6 && ok; n++) {
        unsigned char G = 0;
        for (int f = 0; f < 6; f++) if (g[f] == n) G |= (unsigned char)(1u << f);
        if (!G) continue;
        // the entry that holds most of the group (not kept by an earlier group), and where its paint is
        int best = -1, bestCount = 0, src = -1, srcCount = 0;
        for (int s = 0; s < snapCount; s++) {
            int c = 0;
            for (int f = 0; f < 6; f++) c += (G & snaps[s].faces & (1u << f)) != 0;
            if (c > srcCount) { src = s; srcCount = c; }
            if (!snaps[s].kept && c > bestCount) { best = s; bestCount = c; }
        }
        if (src < 0) continue;      // (no paint on any of its faces: they stay without)
        if (best >= 0) {
            snaps[best].kept = true;
            if (snaps[best].faces == G) continue;
            EditOp op = { .kind = EDIT_PAINT_SET };
            op.paint.material = scene->materials[h->material].name;
            op.paint.faces = G;
            op.paint.slot = -1;
            op.paint.filter = -1;
            op.paint.rect[0] = snaps[best].x;       // (the entry is found by its rectangle)
            op.paint.rect[1] = snaps[best].y;
            op.paint.rect[2] = snaps[best].w;
            op.paint.rect[3] = snaps[best].h;
            ok = PushOp(dm, p, &pushed, h->hit.obj, &op, "faces") == 0;     // (0: pushed / amended)
        } else {
            // a split: a new rectangle of the source's size with a copy of its paint
            int x, y;
            const Snap *S = &snaps[src];
            ok = AddRect(dm, p, &pushed, h->hit.obj, h->material, G, -1, S->w, S->h, -1, EDIT_PAINT_ADD, &x, &y) == 0;     // (0: added)
            for (int ty = 0; ok && ty < S->h; ty++)
                for (int tx = 0; tx < S->w; tx++) {
                    unsigned char px[4];
                    memcpy(px, (const unsigned char *)a->img.data + ((size_t)(S->y + ty) * a->img.width + S->x + tx) * 4, 4);
                    SetTexel(a, p, x + tx, y + ty, px);
                }
        }
        if (!ok) why = MS_StrDup(MS_LastError());
        if (!ok && !pushed) {       // (the step was released)
            Sayf(dm, true, "%s", why ? why : "out of memory");
            free(why);
            return DRAWMODE_NONE;
        }
    }
    // Rectangles no entry has any more: cleared
    for (int s = 0; s < snapCount && pushed; s++) {
        bool used = false;
        int n = MS_PaintEntryCount(scene);
        for (int i = 0; i < n && !used; i++) {
            const MS_PaintEntry *E = MS_PaintEntryAt(scene, i);
            const char *er = EntryAtlasRel(E);
            used = er && strcmp(er, a->rel) == 0 && E->x == snaps[s].x && E->y == snaps[s].y && E->w == snaps[s].w && E->h == snaps[s].h;
        }
        if (!used) ClearRect(a, p, snaps[s].x, snaps[s].y, snaps[s].w, snaps[s].h);
    }
    if (!ok) Sayf(dm, true, "%s", why ? why : "out of memory");
    else if (!pushed) SayStatic(dm, false, "faces: nothing changes (a face without paint gets a rectangle with its first stroke)");
    free(why);
    char gt[32];
    snprintf(gt, sizeof(gt), "%d %d %d %d %d %d", g[0], g[1], g[2], g[3], g[4], g[5]);
    return EditStepEnd(dm, p, pushed, ok ? TextFormat("faces: groups %s", gt) : NULL);
}

// Resize to density: the entry gets a rectangle of the size the density gives, its paint resampled
// into it (nearest with paint_filter nearest, else bilinear), the old one cleared
static DrawModeResult ActResize(DrawMode *dm, const MS_SurfaceHit *h)
{
    int entry;
    if (!ActionEntry(dm, h, "resize", &entry)) return DRAWMODE_NONE;
    MS_PaintEntry E = *MS_PaintEntryAt(scene, entry);
    bool model = HitIsModel(h);
    int file = model ? scene->models[h->hit.obj.index].file : -1;
    int w, hh;
    if (model) w = hh = SlotRectSide(dm->density, h->hit.obj.index, h->slot);
    else BoxRectSize(dm->density, h->hit.obj.index, E.faces, &w, &hh);
    if (w == E.w && hh == E.h) {
        Sayf(dm, false, "resize: the rectangle is %dx%d already at %g texels/m", w, hh, dm->density);
        return DRAWMODE_NONE;
    }
    PaintStep *p = EditStepOn(dm, h->material, file, h->slot, "resize");
    if (!p) return DRAWMODE_NONE;
    Atlas *a = &dm->atlases[p->atlas];
    bool nearest;
    AtlasRelOf(h->material, file, h->slot, NULL, NULL, &nearest);
    // The old paint, before anything moves
    Image old = NewImage(E.w, E.h);
    if (!old.data) { ReleaseStep(dm, p); SayStatic(dm, true, "out of memory"); return DRAWMODE_NONE; }
    for (int y = 0; y < E.h; y++)
        memcpy((unsigned char *)old.data + (size_t)y * E.w * 4, (const unsigned char *)a->img.data + ((size_t)(E.y + y) * a->img.width + E.x) * 4,
               (size_t)E.w * 4);
    bool pushed = false;
    int x, y;
    bool ok = AddRect(dm, p, &pushed, h->hit.obj, h->material, model ? 0 : E.faces, h->slot, w, hh, entry, EDIT_PAINT_SET, &x, &y) == 0;
    if (ok) {
        a = &dm->atlases[p->atlas];
        // (the old rectangle cleared, but where the new one lies: it was cleared already, and is drawn next)
        ClearRect(a, p, E.x, E.y, E.w, E.h);
        for (int ty = 0; ty < hh; ty++)
            for (int tx = 0; tx < w; tx++) {
                unsigned char px[4];
                if (nearest) memcpy(px, (const unsigned char *)old.data + ((size_t)(ty * E.h / hh) * E.w + tx * E.w / w) * 4, 4);
                else {
                    float s[4];
                    SampleRect(&old, 0, 0, E.w, E.h, (tx + 0.5f) / w, (ty + 0.5f) / hh, s);
                    for (int c = 0; c < 4; c++) px[c] = (unsigned char)ClampI((int)lroundf(s[c]), 0, 255);
                    if (px[3] == 0) px[0] = px[1] = px[2] = 0;
                }
                SetTexel(a, p, x + tx, y + ty, px);
            }
    } else Sayf(dm, true, "%s", MS_LastError());      // (not added: why)
    FreeImage(&old);
    if (!pushed) return DRAWMODE_NONE;      // (the step was released)
    return EditStepEnd(dm, p, pushed, ok ? TextFormat("resized the rectangle %dx%d -> %dx%d at %d %d", E.w, E.h, w, hh, x, y) : NULL);
}

// Clear paint: the entry goes (EDIT_PAINT_REMOVE), its rectangle becomes transparent
static DrawModeResult ActClear(DrawMode *dm, const MS_SurfaceHit *h)
{
    int entry;
    if (!ActionEntry(dm, h, "clear", &entry)) return DRAWMODE_NONE;
    MS_PaintEntry E = *MS_PaintEntryAt(scene, entry);
    bool model = HitIsModel(h);
    int file = model ? scene->models[h->hit.obj.index].file : -1;
    PaintStep *p = EditStepOn(dm, h->material, file, h->slot, "clear");
    if (!p) return DRAWMODE_NONE;
    EditOp op = { .kind = EDIT_PAINT_REMOVE };
    op.paint.material = h->material >= 0 ? scene->materials[h->material].name : NULL;
    op.paint.faces = model ? 0 : E.faces;
    op.paint.slot = model ? h->slot : -1;
    op.paint.filter = -1;
    bool pushed = false;
    if (PushOp(dm, p, &pushed, h->hit.obj, &op, "cleared") != 0) {      // (not pushed: why)
        Sayf(dm, true, "%s", MS_LastError());
        return pushed ? EditStepEnd(dm, p, true, NULL) : DRAWMODE_NONE;
    }
    ClearRect(&dm->atlases[p->atlas], p, E.x, E.y, E.w, E.h);
    char faces[32] = "";
    if (!model) FacesText(E.faces, faces, sizeof(faces));
    return EditStepEnd(dm, p, pushed, TextFormat("cleared the paint of %s (%dx%d at %d %d)", model ? TextFormat("slot %d", h->slot) : faces,
                                                 E.w, E.h, E.x, E.y));
}

// Pixel look (docs/painting.md 4.1): the atlas's paint_filter (a material's key, or an own atlas's
// entries); without an atlas only new ones
static DrawModeResult ActPixelLook(DrawMode *dm, const MS_SurfaceHit *h, bool on)
{
    dm->pixelLook = on;
    bool model = HitIsModel(h);
    int file = model ? scene->models[h->hit.obj.index].file : -1;
    const char *rel = HitIsBox(h) || model ? AtlasRelOf(h->material, file, h->slot, NULL, NULL, NULL) : NULL;
    if (!rel) {
        SayStatic(dm, false, on ? "pixel look on: new atlases get paint_filter nearest" : "pixel look off: new atlases are filtered");
        return DRAWMODE_NONE;
    }
    if (!dm->em->ed.work && !MapEditBegin(&dm->em->ed)) return DRAWMODE_NONE;
    MS_EntityId *ids;
    int count;
    unsigned char r = 1;
    bool none = false;      // (an own atlas without entries: nothing applied)
    if (h->material >= 0) {
        EditOp op = { .kind = EDIT_MATERIAL_SET, .matName = scene->materials[h->material].name, .matKeys = on ? "paint_filter nearest" : "-paint_filter" };
        r = MapEditApply(&dm->em->ed, NULL, 0, &op, &ids, &count);
    } else {
        // every entry of the own atlas (the runtime samples it nearest when one asks)
        MS_ObjRef *objs = NULL;
        int n = 0, cap = 0, total = MS_PaintEntryCount(scene);
        for (int i = 0; i < total; i++) {
            const MS_PaintEntry *e = MS_PaintEntryAt(scene, i);
            const char *er = e->material < 0 ? EntryAtlasRel(e) : NULL;
            if (!er || strcmp(er, rel) != 0 || e->slot != h->slot) continue;
            MS_ObjRef *s = MS_PUSH(objs, n, cap);
            if (s) *s = e->obj;
        }
        EditOp op = { .kind = EDIT_PAINT_SET };
        op.paint.slot = h->slot;
        op.paint.filter = (signed char)(on ? 1 : 0);
        if (n > 0) r = MapEditApply(&dm->em->ed, objs, n, &op, &ids, &count);
        else { none = true; ids = NULL; }
        free(objs);
    }
    free(ids);
    // (1: not applied, why in the last error; 0 / 2: applied (2: the auto save failed), the editor's note)
    if (none) Sayf(dm, true, "pixel look: no entry in %s yet", rel);
    else Sayf(dm, r == 1, "%s", r != 1 ? MapEditNote(&dm->em->ed) : MS_LastError());
    return r != 1 ? DRAWMODE_RELOADED : DRAWMODE_NONE;
}

DrawModeResult DrawModePanelAction(DrawMode *dm, EditMode *em, int action, const unsigned char faces[6])
{
    (void)em;
    const MS_SurfaceHit *h = &dm->target.hit;
    bool set = dm->target.version != 0;
    if (action == DRAW_ACT_PIXEL_LOOK) return ActPixelLook(dm, set ? h : &(MS_SurfaceHit){ 0 }, dm->pixelLook);
    if (!set) {
        SayStatic(dm, true, "point at a painted box or model first (the panel acts on the surface the cursor rests on)");
        return DRAWMODE_NONE;
    }
    if (!dm->target.ok && dm->target.refusalCode >= 2) { Sayf(dm, true, "%s", MsgText(&dm->target.refusal)); return DRAWMODE_NONE; }     // (a refusal)
    if (action == DRAW_ACT_RESIZE) return ActResize(dm, h);
    if (action == DRAW_ACT_FACES && faces) return ActFaces(dm, h, faces);
    if (action == DRAW_ACT_CLEAR) return ActClear(dm, h);
    return DRAWMODE_NONE;
}

// #endregion

// ======================================================================================
// #region The eyedropper (docs/painting.md 7.4)
// ======================================================================================

static void AddRecent(DrawMode *dm, Color c)
{
    int at = dm->recentCount < DRAW_RECENT_COLORS ? dm->recentCount : DRAW_RECENT_COLORS - 1;
    for (int i = 0; i < dm->recentCount; i++)
        if (memcmp(&dm->recent[i], &c, sizeof(Color)) == 0) { at = i; break; }
    if (at == dm->recentCount) dm->recentCount++;
    memmove(&dm->recent[1], &dm->recent[0], sizeof(Color) * (size_t)at);
    dm->recent[0] = c;
}

static void SetColor(DrawMode *dm, Color c)
{
    if (memcmp(&dm->color, &c, sizeof(Color)) != 0) dm->prevColor = dm->color;
    dm->color = c;
    AddRecent(dm, c);
}

static unsigned char LinearToSrgb(unsigned char v)
{
    float l = v / 255.0f;
    float s = l <= 0.0031308f ? 12.92f * l : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
    return (unsigned char)ClampI((int)lroundf(s * 255.0f), 0, 255);
}

// What the surface's material computed at pixel px: R3D's albedo G-buffer (its ALBEDO output mode),
// rendered once more into a texture of the view's size with the view's camera, and read back at
// px (linear bytes, as sRGB). A click's cost: one extra scene render
static bool AlbedoAt(Camera3D cam, int w, int h, Vector2 px, Color *out)
{
    int x = (int)px.x, y = (int)px.y;
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x >= w || y >= h) return false;
    RenderTexture2D rt = LoadRenderTexture(w, h);
    if (!rt.id) return false;
    R3D_OutputMode mode = R3D_GetOutputMode();
    R3D_SetOutputMode(R3D_OUTPUT_ALBEDO);
    R3D_BeginPro((R3D_View){ .camera = R3D_CameraFromRL(cam), .target = rt });
        MS_PushLights(scene, cam, (float)w / h);
        MS_DrawGeometry(scene);
    R3D_End();
    R3D_SetOutputMode(mode);
    Image img = LoadImageFromTexture(rt.texture);
    UnloadRenderTexture(rt);
    bool ok = img.data != NULL;
    if (ok) {
        if (img.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        // (a render texture's rows run bottom-up)
        const unsigned char *p = (const unsigned char *)img.data + ((size_t)(h - 1 - y) * w + x) * 4;
        *out = (Color){ LinearToSrgb(p[0]), LinearToSrgb(p[1]), LinearToSrgb(p[2]), 255 };
    }
    UnloadImage(img);
    return ok;
}

// ALT+LMB: the paint texel when it is opaque, else what the surface shows under the paint (the
// G-buffer albedo at the cursor). *fromPaint: the texel
static bool Eyedropper(DrawMode *dm, const MS_SurfaceHit *h, Camera3D cam, int w, int hh, Vector2 px, Color *out, bool *fromPaint)
{
    *fromPaint = false;
    if (!HitIsBox(h) && !HitIsModel(h)) return false;
    int file = HitIsModel(h) ? scene->models[h->hit.obj.index].file : -1;
    int e = EntryFor(h->hit.obj, h->material, h->face, h->slot, NULL);
    Vector2 uv;
    const char *rel = AtlasRelOf(h->material, file, h->slot, NULL, NULL, NULL);
    int ai = rel ? AtlasByRel(dm, rel) : -1;
    if (e >= 0 && rel && ai < 0) {
        char copy[MS_PATH_MAX];
        snprintf(copy, sizeof(copy), "%s", rel);
        ai = AtlasOpen(dm, copy, h->material >= 0 ? scene->materials[h->material].name : NULL, file >= 0 ? scene->modelFiles[file].path : NULL,
                       h->slot);
    }
    if (e >= 0 && ai >= 0 && dm->atlases[ai].img.data && HitPaintUv(h, &uv)) {
        const MS_PaintEntry *E = MS_PaintEntryAt(scene, e);
        const Atlas *a = &dm->atlases[ai];
        int x = ClampI(E->x + (int)floorf(Clamp01(uv.x) * E->w), E->x, E->x + E->w - 1);
        int y = ClampI(E->y + (int)floorf(Clamp01(uv.y) * E->h), E->y, E->y + E->h - 1);
        if (x < a->img.width && y < a->img.height) {
            const unsigned char *p = (const unsigned char *)a->img.data + ((size_t)y * a->img.width + x) * 4;
            if (p[3] == 255) {
                *out = (Color){ p[0], p[1], p[2], 255 };
                *fromPaint = true;
                return true;
            }
        }
    }
    return AlbedoAt(cam, w, hh, px, out);
}

// #endregion

// What a per-frame text at h depends on (with Resolve's code and the rectangle g, NULL: none)
static DrawTextKey TextKeyOf(unsigned char code, const MS_SurfaceHit *h, bool ok, const DrawGeom *g)
{
    DrawTextKey k;
    memset(&k, 0, sizeof(k));       // (compared with memcmp: no stray padding bytes)
    k.set = true;
    k.code = code;
    k.kind = (int)h->hit.obj.kind;
    k.index = h->hit.obj.index;
    k.face = h->face;
    k.slot = h->slot;
    k.material = h->material;
    k.version = scene->version;
    k.ok = ok;
    if (g) {
        k.fresh = g->fresh;
        k.needsUnwrap = g->needsUnwrap;
        k.atlasW = g->atlasW;
        k.atlasH = g->atlasH;
        k.w = g->w;
        k.h = g->h;
        k.x = g->x;
        k.y = g->y;
    }
    return k;
}

// Resolve's refusal words for code at h into m, rebuilt only when what they depend on changed (runs
// every frame: no allocation while the cursor stays on one surface)
static void RefusalUpdate(MonoMsg *m, DrawTextKey *key, unsigned char code, const MS_SurfaceHit *h)
{
    DrawTextKey k = TextKeyOf(code, h, code == 0, NULL);
    if (memcmp(&k, key, sizeof(k)) == 0) return;
    *key = k;
    if (code < 2) { MsgSetStatic(m, ""); return; }
    int n = RefusalPrint(NULL, 0, code, h);
    char *text = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (text) {
        RefusalPrint(text, (size_t)n + 1, code, h);
        MsgSetf(m, "%s", text);
        free(text);
    } else MsgSetStatic(m, "out of memory");
}

// ======================================================================================
// #region The surface under the cursor and the panel's target: texts, groups
// ======================================================================================

#define SETTLE_TIME 0.3f        // s the cursor rests on a surface before the panel shows it

static void HoverHit(DrawMode *dm, const MS_SurfaceHit *h)
{
    dm->hover = *h;
    dm->hoverVersion = 0;       // (no ray: HoverAt sets it)
    dm->refusalCode = Resolve(dm, h, &dm->hoverGeom);
    dm->hoverOk = dm->refusalCode == 0;
    RefusalUpdate(&dm->refusal, &dm->refusalKey, dm->refusalCode, h);    // (per frame: rebuilt only when it changes)
}

static MS_SurfaceHit CastRay(Ray ray)
{
    MS_SurfaceHit hit;
    MS_RaycastSurface(scene, ray.position, ray.direction, 0.0f, &hit);
    return hit;
}

// The hover at px (its ray kept, so it can be found again after the scene changed)
static void HoverAt(DrawMode *dm, Camera3D cam, Vector2 px, int w, int h)
{
    dm->cursorPx = px;
    Ray ray = GetScreenToWorldRayEx(px, cam, w, h);
    MS_SurfaceHit hit = CastRay(ray);
    HoverHit(dm, &hit);
    dm->hoverRay = ray;
    dm->hoverVersion = scene->version;
}

// "concrete's atlas" / "the own atlas of barrel.glb slot 1"
static void AtlasName(const MS_SurfaceHit *h, char *out, int size)
{
    if (h->material >= 0) snprintf(out, (size_t)size, "%s's atlas", scene->materials[h->material].name);
    else snprintf(out, (size_t)size, "the own atlas of %s slot %d", GetFileName(scene->modelFiles[scene->models[h->hit.obj.index].file].path), h->slot);
}

// The panel's three lines about its target. Runs every frame: the texts (they hold names: heap) are
// rebuilt only when what they show changed (textKey); the face groups every time (no allocation)
static void TargetText(DrawMode *dm)
{
    DrawTarget *T = &dm->target;
    const MS_SurfaceHit *h = &T->hit;
    T->facesOk = false;
    T->faceKey = -1;
    if (HitIsBox(h) && T->ok) {
        T->facesOk = true;
        T->faceKey = h->hit.obj.index * 1024 + (h->material & 1023);
        BoxGroups(h, T->groups, NULL);
    }
    DrawTextKey k = TextKeyOf(T->refusalCode, h, T->ok, &T->geom);
    if (memcmp(&k, &T->textKey, sizeof(k)) == 0) return;
    T->textKey = k;
    MsgSetStatic(&T->into, "");
    MsgSetStatic(&T->texel, "");
    T->intoSet = T->texelSet = false;
    if (HitIsBox(h)) {
        const MS_Ref *r = MS_ObjectRef(scene, h->hit.obj);
        int e = MS_ObjectEntity(scene, h->hit.obj), first = e;
        while (first > 0 && e >= 0 && scene->entitySources[first - 1].prim == scene->entitySources[e].prim) first--;
        bool turned = scene->boxTurned && scene->boxTurned[h->hit.obj.index] >= 0;
        MsgSetf(&T->what, "%s line %d #%d, face %s", turned ? "turned box" : "box", r ? r->line : 0, e >= 0 ? e - first : 0,
                FACE_NAMES[h->face]);
    } else if (HitIsModel(h)) {
        const MS_Ref *r = MS_ObjectRef(scene, h->hit.obj);
        int file = scene->models[h->hit.obj.index].file;
        MsgSetf(&T->what, "model %s slot %d, line %d", GetFileName(scene->modelFiles[file].path), h->slot, r ? r->line : 0);
    } else if (h->hit.obj.kind == MS_OBJ_LIGHT) {
        MsgSetStatic(&T->what, "a light: nothing to paint");
        return;
    } else {
        MsgSetStatic(&T->what, "point at a box or a model");
        return;
    }
    if (!T->ok) return;
    const DrawGeom *g = &T->geom;
    // (the atlas's name: "<material>'s atlas" / "the own atlas of <file> slot N", heap: it holds names)
    char *atlas = h->material >= 0 ? MonoStrf("%s's atlas", scene->materials[h->material].name)
                : MonoStrf("the own atlas of %s slot %d", GetFileName(scene->modelFiles[scene->models[h->hit.obj.index].file].path), h->slot);
    const char *an = atlas ? atlas : "out of memory";
    if (!g->fresh) MsgSetf(&T->into, "%s %d×%d: %d×%d at %d %d", an, g->atlasW, g->atlasH, g->w, g->h, g->x, g->y);
    else if (g->atlasW > 0) MsgSetf(&T->into, "the first stroke adds %d×%d to %.120s (%d×%d)", g->w, g->h, an, g->atlasW, g->atlasH);
    else MsgSetf(&T->into, "the first stroke makes %.120s with a %d×%d rectangle", an, g->w, g->h);
    free(atlas);
    T->intoSet = true;
    if (g->needsUnwrap)
        MsgAppendf(&T->into, "; %s is unwrapped for paint first", GetFileName(scene->modelFiles[scene->models[h->hit.obj.index].file].path));
    // The size of a texel here (mm along the face's two edges; a model: about)
    const char *will = g->fresh ? "will be" : "is";
    if (HitIsBox(h)) {
        float su, sv;
        FaceDims(h->hit.obj.index, h->face, &su, &sv);
        if (g->w > 0 && g->h > 0) {
            MsgSetf(&T->texel, "face %s: a texel %s %.1f × %.1f mm%s", FACE_NAMES[h->face], will, su * 1000.0f / g->w,
                    sv * 1000.0f / g->h, g->w >= DRAW_RECT_MAX || g->h >= DRAW_RECT_MAX ? " (the rectangle is clamped to 2048)" : "");
            T->texelSet = true;
        }
    } else if (g->w > 0) {
        double A = SlotArea(h->hit.obj.index, h->slot);
        if (A > 0.0) {
            MsgSetf(&T->texel, "slot %d: a texel %s about %.1f mm (%d² over %.2f m²)", h->slot, will, 1000.0 * sqrt(A) / g->w, g->w, A);
            T->texelSet = true;
        }
    }
}

// The target becomes the surface under the cursor (the same surface: only its hit moves on)
static void TargetTake(DrawMode *dm)
{
    DrawTarget *T = &dm->target;
    if (!(T->version != 0 && SameSurface(&T->hit, &dm->hover))) T->gen++;
    T->hit = dm->hover;
    T->ok = dm->hoverOk;
    T->refusalCode = dm->refusalCode;
    RefusalUpdate(&T->refusal, &T->refusalKey, T->refusalCode, &T->hit);
    T->geom = dm->hoverGeom;
    T->ray = dm->hoverRay;
    T->version = scene->version;
}

// Per frame: the target found again after the scene changed (the same ray), its rectangle and lines
// brought up to date (the density, the pixel look)
static void TargetRefresh(DrawMode *dm)
{
    DrawTarget *T = &dm->target;
    if (T->version == 0) return;
    if (T->version != scene->version) {
        MS_SurfaceHit h = CastRay(T->ray);
        if (!SameSurface(&h, &T->hit)) T->gen++;
        T->hit = h;
        T->version = scene->version;
    }
    T->refusalCode = Resolve(dm, &T->hit, &T->geom);
    T->ok = T->refusalCode == 0;
    RefusalUpdate(&T->refusal, &T->refusalKey, T->refusalCode, &T->hit);
    TargetText(dm);
}

// The hover found again after the scene changed (the cursor was on the panel: an action reloaded it)
static void HoverRefresh(DrawMode *dm)
{
    if (dm->hoverVersion == 0 || dm->hoverVersion == scene->version) return;
    Ray ray = dm->hoverRay;
    MS_SurfaceHit h = CastRay(ray);
    HoverHit(dm, &h);
    dm->hoverRay = ray;
    dm->hoverVersion = scene->version;
}

// The target follows the hover once the cursor has rested on a surface for SETTLE_TIME (at once:
// force, a click)
static void Settle(DrawMode *dm, float dt, bool force)
{
    if (force || dm->target.version == 0 || SameSurface(&dm->hover, &dm->target.hit)) {
        TargetTake(dm);
        dm->settleTime = 0.0f;
        dm->settling = dm->hover;
        return;
    }
    if (!SameSurface(&dm->hover, &dm->settling)) {
        dm->settling = dm->hover;
        dm->settleTime = 0.0f;
    }
    dm->settleTime += dt;
    if (dm->settleTime >= SETTLE_TIME) TargetTake(dm);
}

static void PrepareCursor(DrawMode *dm);

void DrawModeHover(DrawMode *dm, Camera3D cam, Vector2 px, int w, int h)
{
    HoverAt(dm, cam, px, w, h);
    Settle(dm, 0.0f, true);
    TargetRefresh(dm);
    PrepareCursor(dm);
}

// #endregion

// ======================================================================================
// #region Open, close, keys, the update
// ======================================================================================

void DrawModeOpen(DrawMode *dm, EditMode *em, Designer *d)
{
    if (d->volumeMode) DesignerVolumeModeEnd(d);
    em->active = false;
    dm->active = true;
    dm->leaveWarnTime = 0.0f;
    dm->onPanel = false;
    dm->target.version = 0;     // (the panel's target: the first surface the cursor is over)
    dm->hoverVersion = 0;
    dm->hoverOk = false;
    dm->hover.hit.obj.kind = MS_OBJ_NONE;
    if (!em->ed.work && !MapEditBegin(&em->ed)) SayStatic(dm, true, "this map can't be painted (a compiled map?): load its .map");
}

void DrawModeClose(DrawMode *dm)
{
    dm->active = false;
    dm->hoverOk = false;
    dm->unwrapWant = false;     // (an unwrap asked for and not made yet: no stroke after leaving)
}

static DrawModeResult History(DrawMode *dm, EditMode *em, Designer *d, bool undo)
{
    if (!EditModeConfirm(em, undo ? KEY_Z : KEY_Y)) return DRAWMODE_NONE;
    if (!em->ed.work && !MapEditBegin(&em->ed)) return DRAWMODE_NONE;
    MS_EntityId *ids;
    int count;
    unsigned char r = undo ? MapEditUndo(&em->ed, &ids, &count) : MapEditRedo(&em->ed, &ids, &count);
    char *text = MS_StrDup(r != 1 ? MapEditNote(&em->ed) : MS_LastError());     // (DesignerSync runs first)
    DesignerSync(d);        // (the selection stays: the draw mode doesn't select)
    free(ids);
    Sayf(dm, r == 1, "%s", text ? text : "out of memory");      // (1: not done; 2: done, the auto save failed)
    free(text);
    return r != 1 ? DRAWMODE_RELOADED : DRAWMODE_NONE;
}

// Leaves the mode for `next` (B / ESC: spectator; M edit, V volume, C snip mode); with unsaved
// edits the first press only says so
static bool Leave(DrawMode *dm, EditMode *em, Designer *d, int key)
{
    int unsaved = MapEditUnsaved(&em->ed);
    if (unsaved > 0 && dm->leaveWarnTime <= 0.0f && (key == KEY_B || key == KEY_ESCAPE)) {
        dm->leaveWarnTime = WARN_TIME;
        Sayf(dm, true, "%d unsaved edit%s: CTRL+S saves, B again leaves them unsaved (still live)", unsaved, unsaved == 1 ? "" : "s");
        return false;
    }
    if (dm->stroking) StrokeEnd(dm, em);
    DrawModeClose(dm);
    if (key == KEY_M) EditModeOpen(em, d);
    else if (key == KEY_V) DesignerVolumeModeBegin(d);
    else if (key == KEY_C) DesignerSnipBegin(d);
    return true;
}

static void SizeStep(DrawMode *dm, int dir)
{
    int s = dm->size;
    if (dir > 0) s = s < 8 ? s + 1 : (int)ceilf(s * 1.25f);
    else s = s <= 8 ? s - 1 : (int)floorf(s / 1.25f);
    dm->size = ClampI(s, 1, DRAW_SIZE_MAX);
}

// LMB down on hit h at px: the stroke begins with a stamp there (its first one on a surface without a
// rectangle allocates one). True when the text changed live
static bool PressStroke(DrawMode *dm, EditMode *em, const MS_SurfaceHit *h, Vector2 px)
{
    unsigned int version = scene->version;
    if (!StrokeBegin(dm, em, h)) return false;
    Probe p = ProbeHit(dm, h, px);
    if (p.ok) StampProbe(dm, &p);
    dm->lastPx = px;
    dm->lastOk = p.ok;
    dm->lastTx = p.s.tx;
    dm->lastTy = p.s.ty;
    dm->lastRegion = p.s.region;
    return scene->version != version;
}

DrawModeResult DrawModeUpdate(DrawMode *dm, EditMode *em, Designer *d, Camera3D cam, bool cursorOnPanel, float dt)
{
    if (dm->messageTime > 0.0f) dm->messageTime -= dt;
    if (dm->leaveWarnTime > 0.0f) dm->leaveWarnTime -= dt;
    if (em->messageTime > 0.0f) em->messageTime -= dt;      // (EditModeUpdate isn't called meanwhile)
    if (em->warnTime > 0.0f) em->warnTime -= dt;
    if (!dm->active) return DRAWMODE_NONE;
    int w = GetScreenWidth(), h = GetScreenHeight();
    bool flying = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
    DrawModeResult res = DRAWMODE_NONE;
    if (dm->stroking && (flying || !IsMouseButtonDown(MOUSE_BUTTON_LEFT))) StrokeEnd(dm, em);
    dm->onPanel = cursorOnPanel;
    if (!cursorOnPanel && !flying) {
        HoverAt(dm, cam, GetMousePosition(), w, h);
        Settle(dm, dt, false);
    } else {
        HoverRefresh(dm);
        if (flying) dm->hoverOk = false;
    }

    // The frame after the unwrap's note: the unwrap, then the stroke where LMB went down
    if (dm->unwrapWant && !dm->stroking) {
        dm->unwrapWant = false;
        if (dm->unwrapVersion == scene->version && !flying) {
            if (PressStroke(dm, em, &dm->unwrapHit, dm->unwrapPx)) res = DRAWMODE_RELOADED;
            if (dm->stroking && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) StrokeEnd(dm, em);     // (a click: one dab)
            if (!cursorOnPanel && !flying) HoverAt(dm, cam, GetMousePosition(), w, h);
        } else SayStatic(dm, true, "the unwrap was dropped: the map changed (click again)");
    }

    bool ctrl = Ctrl(), shift = Shift(), alt = Alt();
    if (!dm->stroking && !dm->keysBlocked) {
        if (ctrl && IsKeyPressed(KEY_Z)) return History(dm, em, d, !shift);
        if (ctrl && IsKeyPressed(KEY_Y)) return History(dm, em, d, false);
        if (ctrl && IsKeyPressed(KEY_S)) {
            if (EditModeConfirm(em, KEY_S)) {
                unsigned char r = DrawModeSave(dm, em);
                Sayf(dm, r == 2, "%s", r == 2 ? MS_LastError() : MapEditNote(&em->ed));     // (2: not saved)
            }
            return DRAWMODE_NONE;
        }
        if (ctrl && shift && IsKeyPressed(KEY_DELETE) && em->ed.failedCount > 0) {
            MapEditClearFailed(&em->ed);
            SayStatic(dm, false, "list of edits that could not be re-applied cleared");
        }
        static const int LEAVE[] = { KEY_B, KEY_ESCAPE, KEY_M, KEY_V, KEY_C };
        for (int k = 0; k < 5 && !ctrl && !alt; k++)
            if (IsKeyPressed(LEAVE[k])) { Leave(dm, em, d, LEAVE[k]); return DRAWMODE_NONE; }
        for (int k = 0; k < PEN_COUNT && !ctrl && !alt; k++)
            if (IsKeyPressed(KEY_ONE + k)) dm->pen = (PenKind)k;
        if (!ctrl && !alt && IsKeyPressed(KEY_X)) {
            Color c = dm->color;
            dm->color = dm->prevColor;
            dm->prevColor = c;
        }
        if (!ctrl && !alt && IsKeyPressed(KEY_TAB)) dm->panelHidden = !dm->panelHidden;
        if (IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressedRepeat(KEY_LEFT_BRACKET)) SizeStep(dm, -1);
        if (IsKeyPressed(KEY_RIGHT_BRACKET) || IsKeyPressedRepeat(KEY_RIGHT_BRACKET)) SizeStep(dm, 1);
    }
    float wheel = flying || cursorOnPanel ? 0.0f : GetMouseWheelMove();
    if (wheel != 0.0f && ctrl) SizeStep(dm, wheel > 0.0f ? 1 : -1);
    else if (wheel != 0.0f && shift) dm->color.a = (unsigned char)ClampI(dm->color.a + (wheel > 0.0f ? 8 : -8), 0, 255);

    Vector2 px = GetMousePosition();
    if (!cursorOnPanel && !flying && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !dm->unwrapWant) {
        Settle(dm, 0.0f, true);     // (a click: the panel shows that surface at once)
        if (alt) {
            Color c;
            bool paint;
            if (Eyedropper(dm, &dm->hover, cam, w, h, px, &c, &paint)) {
                SetColor(dm, c);
                Sayf(dm, false, "picked #%02X%02X%02X a%d (%s)", c.r, c.g, c.b, c.a, paint ? "the paint" : "the surface's albedo");
            }
        } else if (dm->hoverOk && EditModeConfirm(em, STROKE_KEY)) {
            if (dm->hoverGeom.needsUnwrap) {
                // (the note shows for a frame, then the next update unwraps: a large scan takes seconds)
                int file = scene->models[dm->hover.hit.obj.index].file;
                char *row = AssetsRow(scene->modelFiles[file].path);
                if (!row) Sayf(dm, true, "%s has no licence row in ASSETS.md (its paint unwrap derives from it)", scene->modelFiles[file].path);
                else {
                    dm->unwrapWant = true;
                    dm->unwrapHit = dm->hover;
                    dm->unwrapPx = px;
                    dm->unwrapVersion = scene->version;
                    Sayf(dm, false, "unwrapping %s for paint (a large model takes a few seconds) ...", GetFileName(scene->modelFiles[file].path));
                }
                free(row);
            } else if (PressStroke(dm, em, &dm->hover, px)) res = DRAWMODE_RELOADED;
        } else if (!dm->hoverOk && dm->refusalCode >= 2) Sayf(dm, true, "%s", MsgText(&dm->refusal));     // (a refusal)
    } else if (dm->stroking && dm->pen != PEN_FILL && (px.x != dm->lastPx.x || px.y != dm->lastPx.y)) {
        unsigned int version = scene->version;
        Probe a = { .px = dm->lastPx, .ok = dm->lastOk, .s = { dm->lastTx, dm->lastTy, dm->lastRegion, false } };
        Probe b = ProbeHit(dm, &dm->hover, px);
        StrokeSegment(dm, cam, w, h, a, b);
        dm->lastPx = px;
        dm->lastOk = b.ok;
        dm->lastTx = b.s.tx;
        dm->lastTy = b.s.ty;
        dm->lastRegion = b.s.region;
        if (scene->version != version) res = DRAWMODE_RELOADED;
    }
    Flush(dm);
    TargetRefresh(dm);
    PrepareCursor(dm);
    return res;
}

// #endregion

// ======================================================================================
// #region Sync after a reload, HUD, the brush cursor
// ======================================================================================

void DrawModeBeforeSync(DrawMode *dm)
{
    // (a stroke going on ends first: the history the rebase rewrites may hold its step)
    if (dm->stroking) StrokeEnd(dm, dm->em);
    dm->rebaseGen++;
    for (int i = 0; i < dm->atlasCount; i++)
        if (dm->atlases[i].provided) Provide(&dm->atlases[i]);
    for (int i = 0; i < dm->unwrapCount; i++) ProvideUnwrap(&dm->unwraps[i]);
}

void DrawModeSync(DrawMode *dm, EditMode *em, bool newScene)
{
    (void)em;
    if (newScene) {
        // a renderer restart: the textures came from disk, the atlases have the unsaved strokes
        for (int i = 0; i < dm->atlasCount; i++)
            if (dm->atlases[i].provided) Upload(&dm->atlases[i], 0, 0, dm->atlases[i].img.width, dm->atlases[i].img.height);
        return;
    }
    int provided = 0;
    for (int i = 0; i < dm->atlasCount; i++) {
        Atlas *a = &dm->atlases[i];
        if (a->rebaseGen == dm->rebaseGen && a->replaying && a->replayed > 0)
            Sayf(dm, false, "re-applied %d stroke%s onto %s, which changed on disk", a->replayed, a->replayed == 1 ? "" : "s", a->rel);
        if (a->refs == 0) {
            // nothing holds its pixels: the next stroke reads the file again
            a->stale = true;
            if (a->provided) Unprovide(a);
        }
        provided += a->provided;
    }
    // The scene was loaded from disk before the atlases were provided again, and a rebase's steps of
    // paint alone change no text: one live edit of the same text loads the atlases from memory (an
    // atlas of another size than its PNG: a new texture), then their pixels go up
    MapEditor *E = &dm->em->ed;
    if (provided > 0 && E->work && E->workHash == scene->hash &&
        MS_EditSetText(scene, E->work, E->workSize, &E->lastStats) != MS_RELOADED)
        TraceLog(LOG_WARNING, "monolith: draw: the paint atlases could not be loaded again: %s", MS_LastError());
    for (int i = 0; i < dm->atlasCount; i++)
        if (dm->atlases[i].provided) Upload(&dm->atlases[i], 0, 0, dm->atlases[i].img.width, dm->atlases[i].img.height);
}

int DrawModeDrawHud(const DrawMode *dm, const EditMode *em, int x, int y)
{
    if (!dm->active) return 0;
    char target[300] = "";
    const MS_SurfaceHit *h = &dm->hover;
    const DrawGeom *g = &dm->hoverGeom;
    if (dm->hoverOk && (HitIsBox(h) || HitIsModel(h))) {
        char what[160], atlas[200];
        if (HitIsModel(h))
            snprintf(what, sizeof(what), "%s slot %d", GetFileName(scene->modelFiles[scene->models[h->hit.obj.index].file].path), h->slot);
        else snprintf(what, sizeof(what), "face %s", FACE_NAMES[h->face]);
        AtlasName(h, atlas, (int)sizeof(atlas));
        if (!g->fresh) snprintf(target, sizeof(target), "  |  %.120s %d×%d, %.100s", atlas, g->atlasW, g->atlasH, what);
        else snprintf(target, sizeof(target), "  |  %.60s: a new %d×%d in %.120s%s", what, g->w, g->h, atlas, g->needsUnwrap ? " (unwrap first)" : "");
    }
    char unsaved[64] = "";
    int n = EditModeUnsaved(em);
    if (n > 0) snprintf(unsaved, sizeof(unsaved), "  |  %d unsaved edit%s", n, n == 1 ? "" : "s");
    Color c = dm->color;
    // The mode name as a mark in the draw mode's colour, the rest in HUD text (their backdrop
    // strips abut: UiHudText's starts 6 px left of its text)
    int rx = x + UiHudMark("DRAW", x, y, UI_HUD_LINE, UI_S, UI_MODE_DRAW);
    UiHudText(TextFormat("%s%s  #%02X%02X%02X a%d%s%s", PEN_NAMES[dm->pen], dm->pen == PEN_FILL ? "" : TextFormat(" %d px", dm->size),
                         c.r, c.g, c.b, c.a, target, unsaved), rx, y, UI_HUD_LINE, UI_S, UI_TEXT);
    return UI_HUD_LINE;
}

int DrawModeDrawMessages(const DrawMode *dm, int x, int y)
{
    if (!dm->active) return 0;
    int y0 = y;
    if (dm->refusalCode >= 2 && !dm->onPanel) {     // (a refusal)
        UiHudMark(MsgText(&dm->refusal), x, y, UI_HUD_LINE, UI_S, UI_ERROR);
        y += UI_HUD_LINE;
    }
    if (dm->messageTime > 0.0f) {
        float fade = fminf(dm->messageTime, 1.0f);
        if (dm->messageError) UiHudMark(MsgText(&dm->message), x, y, UI_HUD_LINE, UI_S, UiAlpha(UI_ERROR, fade));
        else UiHudText(MsgText(&dm->message), x, y, UI_HUD_LINE, UI_S, UiAlpha(UI_TEXT, fade));
        y += UI_HUD_LINE;
    }
    return y - y0;
}

// A point on face f of box b at face coordinates fu (0..1, as R3D's cube maps them)
static Vector3 FacePoint(int b, int f, Vector2 fu)
{
    Vector3 l;
    switch (f) {
    case 0:  l = (Vector3){ 0.5f, fu.y - 0.5f, 0.5f - fu.x }; break;
    case 1:  l = (Vector3){ -0.5f, fu.y - 0.5f, fu.x - 0.5f }; break;
    case 2:  l = (Vector3){ fu.x - 0.5f, 0.5f, 0.5f - fu.y }; break;
    case 3:  l = (Vector3){ fu.x - 0.5f, -0.5f, fu.y - 0.5f }; break;
    case 4:  l = (Vector3){ fu.x - 0.5f, fu.y - 0.5f, 0.5f }; break;
    default: l = (Vector3){ 0.5f - fu.x, fu.y - 0.5f, -0.5f }; break;
    }
    if (scene->boxTurned && scene->boxTurned[b] >= 0) {
        const MS_TurnedBox *T = &scene->turnedBoxes[scene->boxTurned[b]];
        Vector3 p = T->center;
        p = Vector3Add(p, Vector3Scale(T->axis[0], l.x * T->size.x));
        p = Vector3Add(p, Vector3Scale(T->axis[1], l.y * T->size.y));
        return Vector3Add(p, Vector3Scale(T->axis[2], l.z * T->size.z));
    }
    BoundingBox B = scene->boxes[b];
    Vector3 c = Vector3Scale(Vector3Add(B.min, B.max), 0.5f), s = Vector3Subtract(B.max, B.min);
    return (Vector3){ c.x + l.x * s.x, c.y + l.y * s.y, c.z + l.z * s.z };
}

// A model slot's triangles (paint UVs, model-space positions) and islands, kept for the brush cursor
// (built when the cursor goes onto another slot or the scene changed), and the border of one island
// (the fill pen's outline)
typedef struct DrawSlotTris {
    bool valid;
    int file, slot;
    unsigned int version;
    int n;
    Vector2 *uv;            // 3 per triangle (paint UVs)
    Vector3 *pos;           // 3 per triangle
    int *root;              // each triangle's island (its union-find root)
    int outlineRoot;        // the island of outline[] (-1: none)
    Vector3 *outline;       // its border: pairs of points (edges no other triangle of it shares)
    int outlineCount, outlineCap;
} DrawSlotTris;

static void FreeSlotTris(DrawSlotTris *S)
{
    if (!S) return;
    free(S->uv);
    free(S->pos);
    free(S->root);
    free(S->outline);
    memset(S, 0, sizeof(*S));
}

static const DrawSlotTris *SlotTrisFor(DrawMode *dm, int file, int slot)
{
    DrawSlotTris *S = dm->slotTris;
    if (!S && !(S = dm->slotTris = calloc(1, sizeof(DrawSlotTris)))) return NULL;
    if (S->valid && S->file == file && S->slot == slot && S->version == scene->version) return S;
    free(S->uv);
    free(S->pos);
    free(S->root);
    S->uv = NULL;
    S->pos = NULL;
    S->root = NULL;
    S->valid = false;
    S->outlineRoot = -1;
    S->outlineCount = 0;
    int n = MS_ModelSlotTriangleCount(scene, file, slot);
    if (n <= 0) return NULL;
    int cap = UvEdgeCap(n);
    S->uv = malloc(sizeof(Vector2) * 3 * (size_t)n);
    S->pos = malloc(sizeof(Vector3) * 3 * (size_t)n);
    S->root = malloc(sizeof(int) * (size_t)n);
    EdgeKey *table = malloc(sizeof(EdgeKey) * (size_t)cap);
    bool ok = S->uv && S->pos && S->root && table;
    for (int k = 0; ok && k < n; k++) {
        ok = MS_ModelSlotPaintTriangle(scene, file, slot, k, &S->uv[3 * k], &S->pos[3 * k]);
        S->root[k] = k;
    }
    if (ok) JoinUvEdges(S->uv, n, S->root, table, cap);
    for (int k = 0; ok && k < n; k++) S->root[k] = Root(S->root, k);
    free(table);
    if (!ok) {
        free(S->uv);
        free(S->pos);
        free(S->root);
        S->uv = NULL;
        S->pos = NULL;
        S->root = NULL;
        return NULL;
    }
    S->file = file;
    S->slot = slot;
    S->version = scene->version;
    S->n = n;
    S->valid = true;
    return S;
}

// The border of island `root` of the slot in dm->slotTris: the UV edges only one of its triangles has
static void SlotOutline(DrawMode *dm, int root)
{
    DrawSlotTris *S = dm->slotTris;
    if (!S || !S->valid || S->outlineRoot == root) return;
    S->outlineRoot = root;
    S->outlineCount = 0;
    int m = 0;
    for (int k = 0; k < S->n; k++) m += S->root[k] == root;
    int cap = UvEdgeCap(m);
    typedef struct { EdgeKey key; int edge, count; } Entry;
    Entry *table = malloc(sizeof(Entry) * (size_t)cap);
    if (!table) return;
    for (int i = 0; i < cap; i++) table[i].key.tri = -1;
    for (int k = 0; k < S->n; k++) {
        if (S->root[k] != root) continue;
        for (int e = 0; e < 3; e++) {
            Vector2 p = S->uv[3 * k + e], q = S->uv[3 * k + (e + 1) % 3];
            unsigned int a[2] = { FloatBits(p.x), FloatBits(p.y) }, b[2] = { FloatBits(q.x), FloatBits(q.y) };
            if (a[0] > b[0] || (a[0] == b[0] && a[1] > b[1])) {
                unsigned int t0 = a[0], t1 = a[1];
                a[0] = b[0]; a[1] = b[1]; b[0] = t0; b[1] = t1;
            }
            unsigned long long h = (a[0] * 73856093u) ^ (a[1] * 19349663u) ^ (b[0] * 83492791u) ^ ((unsigned long long)b[1] * 2654435761u);
            for (unsigned int i = (unsigned int)(h & (unsigned long long)(cap - 1));; i = (i + 1) & (unsigned int)(cap - 1)) {
                Entry *s = &table[i];
                if (s->key.tri < 0) { *s = (Entry){ { { a[0], a[1] }, { b[0], b[1] }, k }, e, 1 }; break; }
                if (s->key.a[0] == a[0] && s->key.a[1] == a[1] && s->key.b[0] == b[0] && s->key.b[1] == b[1]) { s->count++; break; }
            }
        }
    }
    for (int i = 0; i < cap; i++) {
        const Entry *s = &table[i];
        if (s->key.tri < 0 || s->count != 1) continue;
        if (!MS_RESERVE(S->outline, S->outlineCap, S->outlineCount + 2)) break;
        S->outline[S->outlineCount++] = S->pos[3 * s->key.tri + s->edge];
        S->outline[S->outlineCount++] = S->pos[3 * s->key.tri + (s->edge + 1) % 3];
    }
    free(table);
}

// Per frame, after the hover: what the brush cursor of a model needs (DrawModeDrawCursor is const)
static void PrepareCursor(DrawMode *dm)
{
    const MS_SurfaceHit *h = &dm->hover;
    if (!dm->active || !dm->hoverOk || !dm->hoverGeom.ok || dm->hoverGeom.needsUnwrap || !HitIsModel(h)) return;
    int file = scene->models[h->hit.obj.index].file;
    const DrawSlotTris *S = SlotTrisFor(dm, file, h->slot);
    int k = S ? HitTriangle(h) : -1;
    if (S && dm->pen == PEN_FILL && k >= 0 && k < S->n) SlotOutline(dm, S->root[k]);
}

// The eye of the brush cursor being drawn. Its lines lie on the surface itself, where the overlay's
// depth bias alone still z-fights (seen as dashes on a face seen head-on), so each point moves
// 0.3 % of its distance toward the eye: along its own view ray, so it stays on the same pixel
static Vector3 cursorEye;

static void CursorLine(Vector3 a, Vector3 b, Color col)
{
    const float lift = 0.003f;
    DrawLine3D(Vector3Lerp(a, cursorEye, lift), Vector3Lerp(b, cursorEye, lift), col);
}

// Cuts segment a-b to the convex polygon poly (n corners, either winding); false: nothing is left
static bool ClipSegment(Vector2 *a, Vector2 *b, const Vector2 *poly, int n)
{
    float area = 0.0f;
    for (int i = 0; i < n; i++) {
        Vector2 p = poly[i], q = poly[(i + 1) % n];
        area += p.x * q.y - q.x * p.y;
    }
    if (fabsf(area) < 1e-14f) return false;
    float sign = area > 0.0f ? 1.0f : -1.0f, t0 = 0.0f, t1 = 1.0f;
    Vector2 o = *a, d = Vector2Subtract(*b, *a);
    for (int i = 0; i < n; i++) {
        Vector2 p = poly[i], e = Vector2Subtract(poly[(i + 1) % n], p);
        float fa = sign * (e.x * (o.y - p.y) - e.y * (o.x - p.x));     // >= 0: inside this edge
        float fd = sign * (e.x * d.y - e.y * d.x);
        if (fabsf(fd) < 1e-20f) {
            if (fa < 0.0f) return false;
            continue;
        }
        float t = -fa / fd;
        if (fd > 0.0f) t0 = fmaxf(t0, t);
        else t1 = fminf(t1, t);
        if (t0 > t1) return false;
    }
    *a = Vector2Add(o, Vector2Scale(d, t0));
    *b = Vector2Add(o, Vector2Scale(d, t1));
    return true;
}

// A rectangle-texel point (around the hit texel (tx, ty)) in the surface's paint coordinates: face
// coordinates 0..1 (boxes) or the slot's paint UV (models)
static Vector2 TexelToSurface(const DrawGeom *g, Vector2 p, float tx, float ty)
{
    return (Vector2){ g->uv.x + (p.x - tx) / g->w, g->uv.y + (p.y - ty) / g->h };
}

// A point of a slot triangle (its paint uv corners, model-space corners) at paint coordinate t
static Vector3 TriPoint(const Vector2 *uv, const Vector3 *pos, Vector2 t)
{
    float den = (uv[1].y - uv[2].y) * (uv[0].x - uv[2].x) + (uv[2].x - uv[1].x) * (uv[0].y - uv[2].y);
    float a = ((uv[1].y - uv[2].y) * (t.x - uv[2].x) + (uv[2].x - uv[1].x) * (t.y - uv[2].y)) / den;
    float b = ((uv[2].y - uv[0].y) * (t.x - uv[2].x) + (uv[0].x - uv[2].x) * (t.y - uv[2].y)) / den;
    return Vector3Add(Vector3Add(Vector3Scale(pos[0], a), Vector3Scale(pos[1], b)), Vector3Scale(pos[2], 1.0f - a - b));
}

// A closed or open polyline of n rectangle-texel points drawn on the hovered surface: on a box face
// (exact: the face is linear in its coordinates), cut at the face's edges; on a model, through each
// triangle of the hit triangle's island that it crosses (each triangle's affine map), cut at the
// triangle's edges. dashed: every other pair of segments
static void CursorPolyline(const DrawMode *dm, const Vector2 *pts, int n, float tx, float ty, Color col, bool dashed)
{
    const MS_SurfaceHit *h = &dm->hover;
    const DrawGeom *g = &dm->hoverGeom;
    if (h->hit.obj.kind == MS_OBJ_BOX) {
        static const Vector2 face[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        for (int i = 0; i + 1 < n; i++) {
            if (dashed && ((i / 2) & 1)) continue;
            Vector2 a = TexelToSurface(g, pts[i], tx, ty), b = TexelToSurface(g, pts[i + 1], tx, ty);
            if (ClipSegment(&a, &b, face, 4)) CursorLine(FacePoint(h->hit.obj.index, h->face, a), FacePoint(h->hit.obj.index, h->face, b), col);
        }
        return;
    }
    const MS_ModelInstance *M = &scene->models[h->hit.obj.index];
    const DrawSlotTris *S = dm->slotTris;
    int k = HitTriangle(h);
    if (!S || !S->valid || S->file != M->file || S->slot != h->slot || S->version != scene->version || k < 0 || k >= S->n) return;
    // The footprint's box in paint coordinates (the triangles to look at)
    Vector2 lo = { INFINITY, INFINITY }, hi = { -INFINITY, -INFINITY };
    for (int i = 0; i < n; i++) {
        Vector2 s = TexelToSurface(g, pts[i], tx, ty);
        lo = (Vector2){ fminf(lo.x, s.x), fminf(lo.y, s.y) };
        hi = (Vector2){ fmaxf(hi.x, s.x), fmaxf(hi.y, s.y) };
    }
    for (int t = 0; t < S->n; t++) {
        if (S->root[t] != S->root[k]) continue;
        const Vector2 *uvs = &S->uv[3 * t];
        const Vector3 *pos = &S->pos[3 * t];
        if (fmaxf(uvs[0].x, fmaxf(uvs[1].x, uvs[2].x)) < lo.x || fminf(uvs[0].x, fminf(uvs[1].x, uvs[2].x)) > hi.x ||
            fmaxf(uvs[0].y, fmaxf(uvs[1].y, uvs[2].y)) < lo.y || fminf(uvs[0].y, fminf(uvs[1].y, uvs[2].y)) > hi.y)
            continue;
        for (int i = 0; i + 1 < n; i++) {
            if (dashed && ((i / 2) & 1)) continue;
            Vector2 a = TexelToSurface(g, pts[i], tx, ty), b = TexelToSurface(g, pts[i + 1], tx, ty);
            if (!ClipSegment(&a, &b, uvs, 3)) continue;
            CursorLine(Vector3Transform(TriPoint(uvs, pos, a), M->transform), Vector3Transform(TriPoint(uvs, pos, b), M->transform), col);
        }
    }
}

// A texel-space circle of radius r about (cx, cy) on the hovered surface
static void CursorRing(const DrawMode *dm, float cx, float cy, float r, float tx, float ty, Color col, bool dashed)
{
    enum { SEGMENTS = 48 };
    Vector2 pts[SEGMENTS + 1];
    for (int i = 0; i <= SEGMENTS; i++) {
        float a = 2.0f * PI * i / SEGMENTS;
        pts[i] = (Vector2){ cx + r * cosf(a), cy + r * sinf(a) };
    }
    CursorPolyline(dm, pts, SEGMENTS + 1, tx, ty, col, dashed);
}

// A texel-space square (x0, y0) .. (x0 + s, y0 + s) on the hovered surface
static void CursorQuad(const DrawMode *dm, float x0, float y0, float s, float tx, float ty, Color col)
{
    Vector2 pts[5] = { { x0, y0 }, { x0 + s, y0 }, { x0 + s, y0 + s }, { x0, y0 + s }, { x0, y0 } };
    CursorPolyline(dm, pts, 5, tx, ty, col, false);
}

// The fill pen: the outline of what one click fills (a box: the faces of the hit face's entry; a
// model: the hit triangle's UV island)
static void CursorFill(const DrawMode *dm, Color col)
{
    const MS_SurfaceHit *h = &dm->hover;
    const DrawGeom *g = &dm->hoverGeom;
    if (h->hit.obj.kind == MS_OBJ_BOX) {
        static const Vector2 q[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        for (int f = 0; f < 6; f++) {
            if (!(g->faces & (1u << f))) continue;
            for (int i = 0; i < 4; i++)
                CursorLine(FacePoint(h->hit.obj.index, f, q[i]), FacePoint(h->hit.obj.index, f, q[(i + 1) % 4]), col);
        }
        return;
    }
    const MS_ModelInstance *M = &scene->models[h->hit.obj.index];
    const DrawSlotTris *S = dm->slotTris;
    int k = HitTriangle(h);
    if (!S || !S->valid || S->file != M->file || S->slot != h->slot || S->version != scene->version || k < 0 || k >= S->n ||
        S->outlineRoot != S->root[k])
        return;
    for (int i = 0; i + 1 < S->outlineCount; i += 2)
        CursorLine(Vector3Transform(S->outline[i], M->transform), Vector3Transform(S->outline[i + 1], M->transform), col);
}

void DrawModeDrawCursor(const DrawMode *dm, Camera3D cam)
{
    const MS_SurfaceHit *h = &dm->hover;
    const DrawGeom *g = &dm->hoverGeom;
    if (!dm->active || !dm->hoverOk || dm->onPanel || !g->ok || dm->hoverVersion != scene->version) return;
    if (!HitIsBox(h) && !HitIsModel(h)) return;
    if (g->uv.x < 0.0f || g->w <= 0 || g->h <= 0) return;     // (a model before its unwrap: no paint coordinates yet)
    DesignerBeginOverlay3D(cam);
    cursorEye = cam.position;
    Color col = Fade(CURSOR_COLOR, 0.95f);
    if (dm->pen == PEN_FILL) CursorFill(dm, col);
    else {
        // The pen's footprint in the rectangle's texels about the hit (docs/painting.md 7.2)
        float tx = Clamp01(g->uv.x) * g->w, ty = Clamp01(g->uv.y) * g->h;
        int s = dm->size < 1 ? 1 : dm->size;
        if (dm->pen == PEN_SQUARE) CursorQuad(dm, floorf(tx) - (float)(s / 2), floorf(ty) - (float)(s / 2), (float)s, tx, ty, col);
        else if (s == 1) CursorQuad(dm, floorf(tx), floorf(ty), 1.0f, tx, ty, col);
        else if (s == 2) CursorQuad(dm, floorf(tx - 0.5f), floorf(ty - 0.5f), 2.0f, tx, ty, col);
        else {
            // (a ring: the texels whose centre is inside it are painted; the eraser's is dashed, the
            // soft pen's solid core a fainter ring inside)
            CursorRing(dm, tx, ty, s * 0.5f, tx, ty, col, dm->pen == PEN_ERASER);
            if (dm->pen == PEN_SOFT && dm->hardness > 0.05f && dm->hardness < 0.95f)
                CursorRing(dm, tx, ty, s * 0.5f * dm->hardness, tx, ty, Fade(CURSOR_COLOR, 0.45f), false);
        }
    }
    DesignerEndOverlay3D();
}

// #endregion

// ======================================================================================
// #region Init, free, --paint-script
// ======================================================================================

void DrawModeInit(DrawMode *dm, EditMode *em)
{
    memset(dm, 0, sizeof(*dm));
    dm->pen = PEN_ROUND;
    dm->size = 4;
    dm->hardness = 0.5f;
    dm->color = (Color){ 200, 50, 50, 255 };
    dm->prevColor = (Color){ 255, 255, 255, 255 };
    dm->density = DRAW_DENSITY_DEFAULT;
    dm->em = em;
    dm->strokeAtlas = -1;
    em->ed.paintHooks = (MapEditPaintHooks){ dm, HookUndo, HookRedo, HookRebase, HookRelease, HookBytes, HookSave };
}

void DrawModeFree(DrawMode *dm)
{
    if (dm->stroke && !dm->strokePushed) ReleaseStep(dm, dm->stroke);
    for (int i = 0; i < dm->atlasCount; i++) FreeAtlas(&dm->atlases[i]);
    free(dm->atlases);
    for (int i = 0; i < dm->unwrapCount; i++) {
        free(dm->unwraps[i].modelPath);
        MS_PaintUnwrapFree(&dm->unwraps[i].u);
    }
    free(dm->unwraps);
    free(dm->shaderOk);
    FreeSlotTris(dm->slotTris);
    free(dm->slotTris);
    FreeIslands(dm->islands);
    free(dm->islands);
    MsgFree(&dm->message);
    MsgFree(&dm->refusal);
    MsgFree(&dm->target.refusal);
    MsgFree(&dm->target.what);
    MsgFree(&dm->target.into);
    MsgFree(&dm->target.texel);
    memset(dm, 0, sizeof(*dm));
}

// A surface hit made from paint coordinates (a script's `uv`): box face f at face coordinates (u, v),
// or model slot s at paint UV (u, v) (the first slot triangle that holds it)
static bool UvHit(MS_ObjRef obj, int faceOrSlot, float u, float v, MS_SurfaceHit *h)
{
    memset(h, 0, sizeof(*h));
    h->hit.obj = obj;
    h->material = h->face = h->slot = h->mesh = h->triangle = h->paintEntry = -1;
    h->faceUv = h->paintUv = (Vector2){ u, v };
    if (obj.kind == MS_OBJ_BOX) {
        if (obj.index < 0 || obj.index >= scene->boxCount || faceOrSlot < 0 || faceOrSlot > 5) return false;
        h->face = faceOrSlot;
        h->material = scene->boxRefs[obj.index].material;
        h->hit.point = FacePoint(obj.index, h->face, h->faceUv);
        BoundingBox B = MS_ObjectBounds(scene, obj);
        Vector3 c = FacePoint(obj.index, h->face, (Vector2){ 0.5f, 0.5f }), o = Vector3Scale(Vector3Add(B.min, B.max), 0.5f);
        h->hit.normal = Vector3Normalize(Vector3Subtract(c, o));
        return true;
    }
    if (obj.kind != MS_OBJ_MODEL || obj.index < 0 || obj.index >= scene->modelCount) return false;
    const MS_ModelInstance *M = &scene->models[obj.index];
    h->slot = faceOrSlot;
    h->material = SlotMaterial(M, faceOrSlot);
    int n = MS_ModelSlotTriangleCount(scene, M->file, faceOrSlot);
    for (int k = 0; k < n; k++) {
        Vector2 t[3];
        Vector3 p[3];
        if (!MS_ModelSlotPaintTriangle(scene, M->file, faceOrSlot, k, t, p)) return false;
        float den = (t[1].y - t[2].y) * (t[0].x - t[2].x) + (t[2].x - t[1].x) * (t[0].y - t[2].y);
        if (fabsf(den) < 1e-12f) continue;
        float a = ((t[1].y - t[2].y) * (u - t[2].x) + (t[2].x - t[1].x) * (v - t[2].y)) / den;
        float b = ((t[2].y - t[0].y) * (u - t[2].x) + (t[0].x - t[2].x) * (v - t[2].y)) / den;
        if (a < -1e-5f || b < -1e-5f || a + b > 1.0f + 1e-5f) continue;
        h->mesh = UV_HIT_K;
        h->triangle = k;
        h->bary = (Vector3){ a, b, 1.0f - a - b };
        Vector3 q = Vector3Add(Vector3Add(Vector3Scale(p[0], a), Vector3Scale(p[1], b)), Vector3Scale(p[2], 1.0f - a - b));
        h->hit.point = Vector3Transform(q, M->transform);
        return true;
    }
    return false;
}

// "id 12:0" / "entity a7d95ba7:0" (as `select` writes them): the object
static bool ParseEntity(char **w, int n, int *at, MS_ObjRef *obj)
{
    if (*at + 1 >= n) return false;
    const char *kind = w[*at], *val = w[*at + 1];
    const char *colon = strchr(val, ':');
    char *end;
    MS_EntityId id;
    if (strcmp(kind, "id") == 0) id.stmt = (unsigned int)strtoul(val, &end, 10);
    else if (strcmp(kind, "entity") == 0) id.stmt = (unsigned int)strtoul(val, &end, 16);
    else return false;
    id.index = colon ? (int)strtol(colon + 1, NULL, 10) : 0;
    if (end == val || (*end && *end != ':')) return false;
    int e = MS_FindEntity(scene, id);
    if (e < 0) return false;
    *obj = scene->entityObjects[e];
    *at += 2;
    return true;
}

static bool ScriptNum(const char *s, float *out)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end || !isfinite(v)) return false;
    *out = (float)v;
    return true;
}

// Stamps between paint-coordinate points a and b of the stroke's surface (a script's `uv` stroke)
static void UvSegment(DrawMode *dm, MS_ObjRef obj, int fs, Vector2 a, Vector2 b)
{
    MS_SurfaceHit ha;
    if (!UvHit(obj, fs, a.x, a.y, &ha)) return;
    int e = EntryFor(obj, ha.material, ha.face, ha.slot, NULL);
    const MS_PaintEntry *E = e >= 0 ? MS_PaintEntryAt(scene, e) : NULL;
    float du = (b.x - a.x) * (E ? E->w : 1), dv = (b.y - a.y) * (E ? E->h : 1);
    int steps = ClampI((int)ceilf(hypotf(du, dv) / Spacing(dm)), 1, 100000);
    for (int i = 1; i <= steps && dm->stroking; i++) {
        float t = (float)i / steps;
        MS_SurfaceHit h;
        if (!UvHit(obj, fs, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, &h)) continue;
        Probe p = ProbeHit(dm, &h, (Vector2){ 0 });
        if (p.ok) StampProbe(dm, &p);
    }
}

// --paint-script `print`: the atlases, the entries, the history
static void PrintState(DrawMode *dm, EditMode *em)
{
    for (int i = 0; i < dm->atlasCount; i++) {
        const Atlas *a = &dm->atlases[i];
        printf("paint:   atlas %s %dx%d %s%s unsaved %s hash %016llx refs %d%s%s\n", a->rel, a->img.width, a->img.height,
               a->material ? "material " : "own ", a->material ? a->material : a->modelPath ? a->modelPath : "-",
               AtlasLive(a) && AtlasUnsaved(a) ? "yes" : "no", PixelHash(&a->img), a->refs, a->provided ? " provided" : "",
               a->stale ? " (stale)" : "");
    }
    int n = MS_PaintEntryCount(scene);
    for (int i = 0; i < n; i++) {
        const MS_PaintEntry *e = MS_PaintEntryAt(scene, i);
        char where[48];
        if (e->obj.kind == MS_OBJ_BOX) FacesText(e->faces, where, (int)sizeof(where));
        else snprintf(where, sizeof(where), "slot %d", e->slot);
        printf("paint:   entry line %d %s%s%s %u:%d %s %d %d %d %d%s%s\n", e->line, e->material >= 0 ? "\"" : "own",
               e->material >= 0 ? scene->materials[e->material].name : "", e->material >= 0 ? "\"" : "", e->entity.stmt, e->entity.index, where,
               e->x, e->y, e->w, e->h, e->nearest ? " nearest" : "", e->active ? "" : " (inactive)");
    }
    for (int i = 0; i < dm->unwrapCount; i++)
        printf("paint:   unwrap %s %d meshes%s\n", dm->unwraps[i].modelPath, dm->unwraps[i].u.meshCount, dm->unwraps[i].written ? " written" : "");
    printf("paint:   %d unsaved edit%s, %d failed, paint history %lld bytes\n", MapEditUnsaved(&em->ed),
           MapEditUnsaved(&em->ed) == 1 ? "" : "s", em->ed.failedCount, MapEditPaintBytes(&em->ed));
    for (int i = 0; i < em->ed.failedCount; i++) printf("paint:   failed: %s\n", em->ed.failed[i].what);
}

// The map (or a package file) changed on disk: the hot reload's path (reload, rebase, paint hooks).
// Returns 0 reloaded, 1 the reload failed; the script's line in *line either way
static unsigned char ScriptReload(DrawMode *dm, EditMode *em, Designer *d, MonoMsg *line)
{
    if (MS_Reload(scene, NULL) != MS_RELOADED) { MsgSetf(line, "the reload failed: %s", MS_LastError()); return 1; }
    DrawModeBeforeSync(dm);
    EditModeSync(em, d, false);
    DrawModeSync(dm, em, false);
    for (int i = 0; i < em->ed.failedCount; i++) printf("paint:   failed: %s\n", em->ed.failed[i].what);
    MsgSetf(line, "reloaded: %.300s%s%.250s", em->messageTime > 0.0f ? MsgText(&em->message) : "no unsaved edits",
            dm->messageTime > 0.0f ? "; " : "", dm->messageTime > 0.0f ? MsgText(&dm->message) : "");
    return 0;
}

// One stroke (or a dab: one stamp) of the script: LMB down on the first hit, then each next point
// one frame of LMB drag (px: through the screen-space sampling; uv: straight in paint coordinates).
// Returns 0 done, 1 failed; the script's line in *line either way
static unsigned char ScriptStroke(DrawMode *dm, EditMode *em, Camera3D cam, int w, int h, bool uv, MS_ObjRef obj, int fs,
                                  const Vector2 *pts, int count, MonoMsg *line)
{
    MS_SurfaceHit hit;
    if (uv) {
        // (a model that needs an unwrap: made first, so its paint coordinates exist; synchronous here)
        if (obj.kind == MS_OBJ_MODEL && obj.index >= 0 && obj.index < scene->modelCount &&
            MS_ModelPaintUv(scene, scene->models[obj.index].file, fs) == MS_PAINTUV_NEEDS_UNWRAP) {
            const MS_ModelInstance *M = &scene->models[obj.index];
            int file = M->file;
            if (EnsureUnwrap(dm, file, fs, SlotRectSide(dm->density, obj.index, fs)) != 0) {     // (not made: why)
                MsgSetf(line, "%s", MS_LastError());
                return 1;
            }
            // (the unwrap goes live with a text edit; until the first stroke's, the script makes one now
            // to read the paint coordinates: the same text again)
            MapEditor *E = &em->ed;
            if (MS_EditSetText(scene, E->work, E->workSize, &E->lastStats) != MS_RELOADED) {
                MsgSetf(line, "the unwrap can't go live: %s", MS_LastError());
                return 1;
            }
        }
        if (!UvHit(obj, fs, pts[0].x, pts[0].y, &hit)) { MsgSetStatic(line, "no such surface point (face / slot, paint uv)"); return 1; }
    } else hit = CastAt(cam, w, h, pts[0]);
    HoverHit(dm, &hit);
    if (!dm->hoverOk) {
        if (dm->refusalCode >= 2) MsgSetf(line, "%s", MsgText(&dm->refusal));      // (a refusal)
        else MsgSetStatic(line, "nothing to paint there");
        return 1;
    }
    unsigned int version = scene->version;
    MsgSetStatic(&dm->message, "");     // (what the stroke says, alone)
    dm->messageError = false;
    if (!StrokeBegin(dm, em, &hit)) { MsgSetf(line, "%s", MsgText(&dm->message)); return 1; }
    Probe a = ProbeHit(dm, &hit, pts[0]);
    if (!dm->stroking) { MsgSetf(line, "%s", MsgText(&dm->message)); return 1; }
    if (a.ok) StampProbe(dm, &a);
    Flush(dm);
    for (int i = 1; i < count && dm->pen != PEN_FILL && dm->stroking; i++) {
        if (uv) UvSegment(dm, obj, fs, pts[i - 1], pts[i]);
        else {
            MS_SurfaceHit hb = CastAt(cam, w, h, pts[i]);
            Probe b = ProbeHit(dm, &hb, pts[i]);
            StrokeSegment(dm, cam, w, h, a, b);
            a = b;
        }
        Flush(dm);     // (a frame: one upload per atlas)
    }
    int samples = dm->stroke ? dm->stroke->sampleCount : 0;
    bool allocated = scene->version != version;
    bool failed = dm->messageError;
    char *failMessage = failed ? MS_StrDup(MsgText(&dm->message)) : NULL;     // (the stroke's own message: StrokeEnd replaces it)
    StrokeEnd(dm, em);
    MsgSetf(line, "%s (%d sample%s%s)%s%s", MsgText(&dm->message), samples, samples == 1 ? "" : "s", allocated ? ", allocated" : "",
            failed ? " -- " : "", !failed ? "" : failMessage ? failMessage : "out of memory");
    free(failMessage);
    return failed || dm->messageError ? 1 : 0;
}

// --paint-script <file> (docs/painting.md 14): one command per line, '#' starts a comment; each prints
// a "paint: file:line: ..." line, the first failing one stops the script (exit 1):
//   pen <round|square|soft|eraser|fill>   size <1-256>   hardness <0-1>   color <r> <g> <b> [<a>]
//   density <8-512>                    texels per metre of new rectangles (and resize)
//   pixel <on|off>                     the pixel look: new atlases, and the atlas painted last
//   faces <a b c d e f>                the face groups of the box painted last (5.3), applied
//   resize                             the rectangle painted last to the size of the current density
//   clear                              the paint of the face / slot painted last (its entry goes)
//   stroke px <x0> <y0> <x1> <y1> ...  an LMB drag through these screen pixels of the shot's camera
//                                      (one segment per frame, cut in screen space as the mouse is)
//   stroke uv <id N:i | entity HEX:i> <face|slot> <u0> <v0> [<u1> <v1> ...]
//                                      the same in paint coordinates: a box face (0..5 = +x -x +y -y +z
//                                      -z, u v 0..1 on it) or a model slot (u v its paint UV; a model
//                                      that needs an unwrap gets it first)
//   dab px <x> <y> / dab uv <obj> <face|slot> <u> <v>   one stamp (fill: fills there)
//   hover px <x> <y>                   what the cursor is over there (the HUD and the brush cursor)
//   pick px <x> <y>                    the eyedropper (prints the colour; it becomes the colour)
//   undo / redo / save / print         print: the atlases (size, unsaved, pixel hash), the entries,
//                                      the unwraps, the history
//   poll                               the hot reload's check (8 x 0.6 s): after a save nothing reloads
//   disk <file>                        the map file becomes the file's bytes, then the hot reload's path
//                                      (reload, the unsaved edits and strokes rebased)
//   put <file> <package path>          a package file becomes the file's bytes (a PNG a request
//                                      repainted), no reload yet; reload: the reload now
int DrawModeRunScript(DrawMode *dm, EditMode *em, Designer *d, Camera3D cam, int w, int h, const char *path)
{
    char *text = LoadFileText(path);
    if (!text) { fprintf(stderr, "monolith: cannot read the paint script %s\n", path); return 1; }
    if (!em->ed.work && !MapEditBegin(&em->ed)) {
        fprintf(stderr, "monolith: --paint-script: %s has no map text to paint (a compiled map?)\n", scene->path);
        UnloadFileText(text);
        return 1;
    }
    dm->active = true;
    em->active = false;
    int status = 0, lineNo = 0;
    Vector2 *pts = NULL;
    int ptCap = 0;
    MonoMsg msg = { 0 };        // (each command's line)
    for (char *line = text; line && status == 0; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        lineNo++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *wd[64];
        int n = 0;
        for (char *tok = strtok(line, " \t\r"); tok && n < 64; tok = strtok(NULL, " \t\r")) wd[n++] = tok;
        line = nl ? nl + 1 : NULL;
        if (n == 0) continue;
        bool ok = false;
        float f[4];
        bool stroke = strcmp(wd[0], "stroke") == 0, dab = strcmp(wd[0], "dab") == 0;
        MsgSetStatic(&msg, "");
        // (each command's own message: what it said, if anything)
        MsgSetStatic(&dm->message, "");
        dm->messageSaid = false;
        if (strcmp(wd[0], "pen") == 0 && n == 2) {
            static const char *const NAMES[PEN_COUNT] = { "round", "square", "soft", "eraser", "fill" };
            for (int k = 0; k < PEN_COUNT; k++) if (strcmp(wd[1], NAMES[k]) == 0) { dm->pen = (PenKind)k; ok = true; }
            MsgSetf(&msg, ok ? "pen %s" : "pen <round|square|soft|eraser|fill>, not %s", wd[1]);
        } else if (strcmp(wd[0], "size") == 0 && n == 2 && ScriptNum(wd[1], &f[0]) && f[0] >= 1 && f[0] <= DRAW_SIZE_MAX) {
            dm->size = (int)f[0];
            ok = true;
            MsgSetf(&msg, "size %d", dm->size);
        } else if (strcmp(wd[0], "hardness") == 0 && n == 2 && ScriptNum(wd[1], &f[0]) && f[0] >= 0 && f[0] <= 1) {
            dm->hardness = f[0];
            ok = true;
            MsgSetf(&msg, "hardness %g", f[0]);
        } else if (strcmp(wd[0], "color") == 0 && (n == 4 || n == 5)) {
            ok = true;
            for (int k = 0; k < n - 1; k++) ok = ok && ScriptNum(wd[1 + k], &f[k]) && f[k] >= 0 && f[k] <= 255;
            if (ok) SetColor(dm, (Color){ (unsigned char)f[0], (unsigned char)f[1], (unsigned char)f[2], n == 5 ? (unsigned char)f[3] : 255 });
            if (ok) MsgSetf(&msg, "color %d %d %d %d", dm->color.r, dm->color.g, dm->color.b, dm->color.a);
            else MsgSetStatic(&msg, "color <r> <g> <b> [<a>] (0-255)");
        } else if (strcmp(wd[0], "density") == 0 && n == 2 && ScriptNum(wd[1], &f[0]) && f[0] >= 8 && f[0] <= 512) {
            dm->density = f[0];
            ok = true;
            MsgSetf(&msg, "density %g texels/m", f[0]);
        } else if (strcmp(wd[0], "pixel") == 0 && n == 2 && (strcmp(wd[1], "on") == 0 || strcmp(wd[1], "off") == 0)) {
            bool on = wd[1][1] == 'n';
            DrawModeResult r = ActPixelLook(dm, dm->lastHitSet ? &dm->lastHit : &(MS_SurfaceHit){ 0 }, on);
            ok = r == DRAWMODE_RELOADED || !dm->messageError;
            MsgSetf(&msg, "pixel look %s: %s", wd[1], MsgText(&dm->message));
        } else if (strcmp(wd[0], "faces") == 0 && n == 7) {
            unsigned char cells[6];
            ok = dm->lastHitSet;
            for (int k = 0; k < 6 && ok; k++) {
                ok = ScriptNum(wd[1 + k], &f[0]) && f[0] >= 0 && f[0] <= 5;
                cells[k] = (unsigned char)(ok ? f[0] : 0);
            }
            if (ok) ok = ActFaces(dm, &dm->lastHit, cells) == DRAWMODE_RELOADED;
            if (dm->messageSaid) MsgSetf(&msg, "%s", MsgText(&dm->message));
            else MsgSetStatic(&msg, "faces <a b c d e f> (0-5), after a stroke on a box");
        } else if ((strcmp(wd[0], "resize") == 0 || strcmp(wd[0], "clear") == 0) && n == 1) {
            bool resize = wd[0][0] == 'r';
            ok = dm->lastHitSet && (resize ? ActResize(dm, &dm->lastHit) : ActClear(dm, &dm->lastHit)) == DRAWMODE_RELOADED;
            MsgSetf(&msg, "%s: %s", wd[0], dm->messageSaid ? MsgText(&dm->message) : "nothing painted yet");
        } else if ((stroke || dab) && n >= 4 && (strcmp(wd[1], "px") == 0 || strcmp(wd[1], "uv") == 0)) {
            bool uv = wd[1][0] == 'u';
            int at = 2, fs = -1;
            MS_ObjRef obj = { MS_OBJ_NONE, -1 };
            ok = true;
            if (uv) {
                float v = 0.0f;
                ok = ParseEntity(wd, n, &at, &obj) && at < n && ScriptNum(wd[at], &v) && v >= 0;
                fs = (int)v;
                at++;
            }
            int count = (n - at) / 2;
            ok = ok && (n - at) % 2 == 0 && count >= 1 && (!dab || count == 1) && MS_RESERVE(pts, ptCap, count);
            for (int k = 0; ok && k < count; k++) ok = ScriptNum(wd[at + 2 * k], &pts[k].x) && ScriptNum(wd[at + 2 * k + 1], &pts[k].y);
            if (!ok) MsgSetf(&msg, "%s px <x> <y> ... | %s uv <id N:i | entity HEX:i> <face|slot> <u> <v> ...", wd[0], wd[0]);
            else ok = ScriptStroke(dm, em, cam, w, h, uv, obj, fs, pts, count, &msg) == 0;     // (0: done)
        } else if (strcmp(wd[0], "hover") == 0 && n == 4 && strcmp(wd[1], "px") == 0 && ScriptNum(wd[2], &f[0]) && ScriptNum(wd[3], &f[1])) {
            DrawModeHover(dm, cam, (Vector2){ f[0], f[1] }, w, h);
            ok = true;
            bool refused = dm->refusalCode >= 2;    // (a refusal; the panel's lines where they apply)
            const DrawTarget *T = &dm->target;
            MsgSetf(&msg, "hover %s%s%s%s%s%s%s", dm->hoverOk ? "paintable" : "not paintable", refused ? ": " : "",
                    refused ? MsgText(&dm->refusal) : "", T->intoSet ? " | " : "", T->intoSet ? MsgText(&T->into) : "", T->texelSet ? " | " : "",
                    T->texelSet ? MsgText(&T->texel) : "");
        } else if (strcmp(wd[0], "pick") == 0 && n == 4 && strcmp(wd[1], "px") == 0 && ScriptNum(wd[2], &f[0]) && ScriptNum(wd[3], &f[1])) {
            MS_SurfaceHit hit = CastAt(cam, w, h, (Vector2){ f[0], f[1] });
            Color c = { 0 };
            bool paint = false;
            ok = Eyedropper(dm, &hit, cam, w, h, (Vector2){ f[0], f[1] }, &c, &paint);
            if (ok) SetColor(dm, c);
            if (ok) MsgSetf(&msg, "picked %d %d %d %d (%s)", c.r, c.g, c.b, c.a, paint ? "the paint" : "the albedo");
            else MsgSetStatic(&msg, "nothing there");
        } else if ((strcmp(wd[0], "undo") == 0 || strcmp(wd[0], "redo") == 0) && n == 1) {
            ok = History(dm, em, d, wd[0][0] == 'u') == DRAWMODE_RELOADED;
            Flush(dm);
            MsgSetf(&msg, "%s", MsgText(&dm->message));
        } else if (strcmp(wd[0], "save") == 0 && n == 1) {
            unsigned char r = DrawModeSave(dm, em);
            ok = r != 2;        // (2: not saved)
            MsgSetf(&msg, "%s", r == 2 ? MS_LastError() : MapEditNote(&em->ed));
        } else if (strcmp(wd[0], "poll") == 0 && n == 1) {
            // The hot reload's check, as a few seconds of frames would make it (after a save: nothing
            // reloads, the editor's own writes are known)
            int reloads = 0, failed = 0;
            for (int k = 0; k < 8; k++) {
                MS_ReloadResult r = MS_Poll(scene, 0.6f);
                reloads += r == MS_RELOADED;
                failed += r == MS_RELOAD_FAILED;
            }
            if (reloads > 0) {
                DrawModeBeforeSync(dm);
                EditModeSync(em, d, false);
                DrawModeSync(dm, em, false);
            }
            ok = true;
            MsgSetf(&msg, "poll: %d reload%s, %d failed", reloads, reloads == 1 ? "" : "s", failed);
        } else if (strcmp(wd[0], "print") == 0 && n == 1) {
            PrintState(dm, em);
            ok = true;
            MsgSetf(&msg, "%d atlas%s, %d entr%s", dm->atlasCount, dm->atlasCount == 1 ? "" : "es", MS_PaintEntryCount(scene),
                    MS_PaintEntryCount(scene) == 1 ? "y" : "ies");
        } else if ((strcmp(wd[0], "disk") == 0 && n == 2) || (strcmp(wd[0], "put") == 0 && n == 3) || (strcmp(wd[0], "reload") == 0 && n == 1)) {
            // disk <file>: the map file becomes its bytes; put <file> <package path>: a package file
            // does (a PNG a request repainted); then the hot reload's path (reload, rebase)
            int size = 0;
            unsigned char *bytes = wd[0][0] == 'r' ? NULL : LoadFileData(wd[1], &size);
            char dst[MS_PATH_MAX * 2];
            if (wd[0][0] == 'p') snprintf(dst, sizeof(dst), "%s%s", scene->root, wd[2]);
            else snprintf(dst, sizeof(dst), "%s", scene->path);
            if (wd[0][0] != 'r' && !bytes) MsgSetf(&msg, "cannot read %s", wd[1]);
            else if (bytes && !SaveFileData(dst, bytes, size)) MsgSetf(&msg, "cannot write %.600s", dst);
            else if (wd[0][0] == 'p') { ok = true; MsgSetf(&msg, "wrote %.600s (no reload yet)", dst); }
            else ok = ScriptReload(dm, em, d, &msg) == 0;       // (0: reloaded)
            if (bytes) UnloadFileData(bytes);
        } else MsgSetf(&msg, "unknown command '%s' (pen size hardness color density pixel faces resize clear stroke dab "
                             "hover pick undo redo save print poll disk put reload)", wd[0]);
        printf("paint: %s:%d: %s%s\n", path, lineNo, ok ? "" : "FAILED: ", MsgText(&msg));
        if (!ok) status = 1;
    }
    Flush(dm);
    free(pts);
    MsgFree(&msg);
    fflush(stdout);
    UnloadFileText(text);
    return status;
}

// #endregion
