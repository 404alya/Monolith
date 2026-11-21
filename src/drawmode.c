// drawmode -- B in spectator mode: painting on boxes and models into the paint layer (design:
// docs/painting.md)
//
// Paint lives in per-material RGBA8 atlases (textures/paint/), composited by the runtime over
// what the material computed, so an object looks exactly as before wherever it isn't painted.
// This file owns each atlas's pixels while it paints; strokes are undo steps in the edit history.
// On a material with a paint grid, new rectangles are the faces' grid cells and strokes map through
// them, so paint texels lie on the map's own pixel grid (docs/painting.md 17).
// Reference: docs/code/drawmode.md
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
#define STAGE_BYTES     2000        // what the paint stage adds to a shader (the empty one: ~1.9 KB)

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
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    MsgSetv(&dm->message, fmt, arg_ptr);
    va_end(arg_ptr);
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
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    char *piece = MonoStrv(fmt, arg_ptr);     // (sized to fit: a long path or cell is never cut)
    va_end(arg_ptr);
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

// Face f of box b on a grid of rho texels per metre (docs/painting.md 17.2): the cells it covers
// (w x h) and where its 0 and 1 lie in them (texels from the first cell), as the runtime's paint
// table maps the face
typedef struct { int w, h; float u0, u1, v0, v1; } FaceCells;

static bool FaceGridCells(int b, int f, float rho, FaceCells *c)
{
    memset(c, 0, sizeof(*c));
    Vector2 off;
    if (!MS_BoxFaceCells(scene, b, f, rho, &c->w, &c->h, &off)) return false;
    float su, sv;
    FaceDims(b, f, &su, &sv);
    c->u0 = off.x;
    c->v0 = off.y;
    c->u1 = (float)fmax(fmin((double)off.x + (double)rho * su, (double)c->w), (double)off.x);
    c->v1 = (float)fmax(fmin((double)off.y + (double)rho * sv, (double)c->h), (double)off.y);
    return true;
}

// A box face group's rectangle on a grid (docs/painting.md 17.3): per axis the cells of the group's
// largest face. False when a face covers more than DRAW_RECT_MAX cells along an axis (*big: that
// face, *cells: how many), so a rectangle never stretches
static bool BoxGroupCells(int b, unsigned char faces, float rho, int *w, int *h, int *big, int *cells)
{
    *w = *h = 1;
    *big = -1;
    *cells = 0;
    for (int f = 0; f < 6; f++) {
        FaceCells c;
        if (!(faces & (1u << f)) || !FaceGridCells(b, f, rho, &c)) continue;
        *w = c.w > *w ? c.w : *w;
        *h = c.h > *h ? c.h : *h;
        int most = c.w > c.h ? c.w : c.h;
        if (most > DRAW_RECT_MAX && most > *cells) { *big = f; *cells = most; }
    }
    return *big < 0;
}

// The largest of a model instance's three scales (docs/painting.md 17.5: the unwrap's texels per unit)
static float InstanceScale(int model)
{
    Vector3 s = scene->models[model].scale;
    float k = fmaxf(fabsf(s.x), fmaxf(fabsf(s.y), fabsf(s.z)));
    return k > 0.0f ? k : 1.0f;
}

// A model slot in this instance: its world area *A, and the share *a of the 0..1 paint domain its
// triangles cover (0 while it has no paint UVs)
static void SlotAreas(int model, int slot, double *A, double *a)
{
    const MS_ModelInstance *M = &scene->models[model];
    int n = MS_ModelSlotTriangleCount(scene, M->file, slot);
    *A = *a = 0.0;
    for (int k = 0; k < n; k++) {
        Vector2 uv[3], puv[3];
        Vector3 p[3];
        bool paint = MS_ModelSlotPaintTriangle(scene, M->file, slot, k, puv, p);
        if (!paint && !MS_ModelSlotTriangle(scene, M->file, slot, k, uv, p)) continue;
        for (int v = 0; v < 3; v++) p[v] = Vector3Transform(p[v], M->transform);
        *A += 0.5 * Vector3Length(Vector3CrossProduct(Vector3Subtract(p[1], p[0]), Vector3Subtract(p[2], p[0])));
        if (paint)
            *a += 0.5 * fabs((double)(puv[1].x - puv[0].x) * (puv[2].y - puv[0].y) -
                             (double)(puv[1].y - puv[0].y) * (puv[2].x - puv[0].x));
    }
}

// The unwrap of model file `file` made this session (NULL: none)
static const DrawUnwrap *SessionUnwrap(const DrawMode *dm, int file)
{
    for (int i = 0; file >= 0 && i < dm->unwrapCount; i++)
        if (strcmp(dm->unwraps[i].modelPath, scene->modelFiles[file].path) == 0) return &dm->unwraps[i];
    return NULL;
}

// The texels per metre new paint on material m gets (docs/painting.md 17.7): its paint grid (m -1: a
// model's own material, the map's), else the panel's density
static float RhoOf(const DrawMode *dm, int m)
{
    float g = MS_PaintGridOf(scene, m);
    return g > 0.0f ? g : dm->density;
}

#define UNWRAP_FILL 1.35f       // an unwrap's square over sqrt(its charts' area): xatlas packs ~55 %

// The rectangle slot `slot` of model instance `model` drawn with material m gets (docs/painting.md
// 17.4, 17.5) at rho texels per metre (the grid, else the density): the albedo texture's size when the
// slot's own UVs carry it texel for texel (halved until both sides are at most 2048); else the square
// of the file's unwrap made this session; else (others) the square another entry of the same file and
// slot has; else ceil(rho sqrt(A / a)) (A the slot's world area here, a its share of the paint domain),
// so the texels have the grid's size. *estimate: the slot still needs its unwrap (about UNWRAP_FILL x
// rho sqrt(A) until it is made). Without a grid at most 2048 a side
static void SlotSize(const DrawMode *dm, int model, int slot, int m, float rho, bool others, int *w, int *h, bool *estimate)
{
    const MS_ModelInstance *M = &scene->models[model];
    bool grid = MS_PaintGridOf(scene, m) > 0.0f;
    *estimate = false;
    if (MS_PaintSlotTexels(scene, m, M->file, slot, w, h)) {
        while (*w > DRAW_RECT_MAX || *h > DRAW_RECT_MAX) {
            *w = (*w + 1) / 2;
            *h = (*h + 1) / 2;
        }
        return;
    }
    const DrawUnwrap *u = SessionUnwrap(dm, M->file);
    if (u && slot < u->sideCount && u->sides[slot] > 0) {
        // (the file's unwrap was made for the slot painted first: another slot of it can need more
        // than 2048 at that texel size; without a grid it is clamped like any rectangle, on a grid
        // Resolve refuses it with code 9)
        *w = *h = u->sides[slot];
        if (!grid && *w > DRAW_RECT_MAX) *w = *h = DRAW_RECT_MAX;
        return;
    }
    int n = others ? MS_PaintEntryCount(scene) : 0;
    for (int i = 0; i < n; i++) {
        const MS_PaintEntry *e = MS_PaintEntryAt(scene, i);
        if (e->obj.kind != MS_OBJ_MODEL || e->obj.index < 0 || e->obj.index >= scene->modelCount || e->slot != slot ||
            scene->models[e->obj.index].file != M->file)
            continue;
        *w = e->w;
        *h = e->h;
        return;
    }
    double A, a;
    SlotAreas(model, slot, &A, &a);
    double side;
    if (MS_ModelPaintUv(scene, M->file, slot) == MS_PAINTUV_NEEDS_UNWRAP || a <= 0.0) {
        *estimate = MS_ModelPaintUv(scene, M->file, slot) == MS_PAINTUV_NEEDS_UNWRAP;
        side = UNWRAP_FILL * rho * sqrt(A);
    } else side = rho * sqrt(A / a);
    *w = *h = (int)fmin(ceil(side - 1e-4), 1e9);
    if (*w < 1) *w = *h = 1;
    if (!grid && *w > DRAW_RECT_MAX) *w = *h = DRAW_RECT_MAX;
}

// A first stroke's rectangle (SlotSize, a rectangle the file's slot has elsewhere counts)
static void SlotNewSize(const DrawMode *dm, int model, int slot, int m, float rho, int *w, int *h, bool *estimate)
{
    SlotSize(dm, model, slot, m, rho, true, w, h, estimate);
}

// Fit to grid / Resize to density on a model slot (SlotSize at the material's grid or the density)
static void SlotFitSize(const DrawMode *dm, int model, int slot, int m, int *w, int *h, bool *estimate)
{
    SlotSize(dm, model, slot, m, RhoOf(dm, m), false, w, h, estimate);
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
    // The tools of docs/painting.md 18.2 as input (a rebase replays them; StepRender draws them): the
    // tool, its options, the colours (color: the start, color2 the gradient's end), the shape's points
    // in atlas texels on regions[shapeRegion], the move's offset, and the selection it was clipped to
    DrawTool tool;
    bool pixelPerfect;      // the pencil at size 1: its samples' texels thinned (DtPixelPerfect)
    Color color2;
    DrawBucketMode bucket;
    int tolerance;
    bool radial;
    uint32_t seed;          // jumble: the pick of each texel's neighbour
    DtPoints pts;
    int shapeRegion;
    int dx, dy;
    bool selOn;             // painting is clipped to sel on selRegion (the move: what moves)
    DtMask sel;
    PaintRegion selRegion;
};

static PaintStep *NewStep(DrawMode *dm, int ai, StepKind kind)
{
    PaintStep *p = calloc(1, sizeof(PaintStep));
    if (!p) return NULL;
    Atlas *a = &dm->atlases[ai];
    p->atlas = ai;
    p->kind = kind;
    p->pending = true;
    p->pen = DrawModeStampPen(dm);
    p->size = dm->size;
    p->hardness = dm->hardness;
    p->color = dm->fg;
    p->tool = kind == STEP_STROKE ? dm->tool : TOOL_PENCIL;
    p->color2 = dm->bg;
    p->bucket = dm->bucket;
    p->tolerance = dm->tolerance;
    p->radial = dm->gradientRadial;
    p->seed = dm->seed;
    p->shapeRegion = -1;
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
    DtPointsFree(&p->pts);
    DtMaskFree(&p->sel);
    free(p);
}

static long long StepBytes(const PaintStep *p)
{
    long long b = (long long)sizeof(PaintStep) + (long long)p->tileCap * (long long)sizeof(PaintTile) +
                  (long long)p->sampleCap * (long long)sizeof(Sample) + (long long)p->regionCap * (long long)sizeof(PaintRegion) +
                  (long long)p->pts.cap * (long long)sizeof(DtPoint) + (long long)p->sel.w * p->sel.h;
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

// The pixel of (x, y) as the stroke started: its tile's undo copy once the stroke touched the tile,
// else the atlas (untouched). NULL outside the atlas
static const unsigned char *StartPx(const Atlas *a, const PaintStep *p, int x, int y)
{
    if (!a->img.data || x < 0 || y < 0 || x >= a->img.width || y >= a->img.height) return NULL;
    int tx = x / DRAW_TILE, ty = y / DRAW_TILE, ti = ty * a->tilesX + tx;
    if (a->tileSlot && ti < a->tilesX * a->tilesY && a->tileSlot[ti] >= 0 && a->tileSlot[ti] < p->tileCount) {
        const PaintTile *t = &p->tiles[a->tileSlot[ti]];
        return t->pixels + ((size_t)(y - ty * DRAW_TILE) * t->w + (x - tx * DRAW_TILE)) * 4;
    }
    return (const unsigned char *)a->img.data + ((size_t)y * a->img.width + x) * 4;
}

// One texel set to value v at coverage cov (the max-coverage rule of ApplyTexel): over, source-over
// of v (its alpha times cov: the gradient, the bucket); else the start pixel moved toward v by cov
// (blur, jumble, move: the pixel becomes v where the coverage is full)
static void ApplyValue(Atlas *a, PaintStep *p, int x, int y, float cov, const unsigned char v[4], bool over)
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
    if (over) Over(dst, orig, (Color){ v[0], v[1], v[2], v[3] }, cv / 255.0f * (v[3] / 255.0f));
    else
        for (int k = 0; k < 4; k++) dst[k] = (unsigned char)ClampI((int)lroundf(orig[k] + (v[k] - orig[k]) * (cv / 255.0f)), 0, 255);
    MarkDirty(a, x, y, x + 1, y + 1);
}

// Where a step's texels go: its region (and the region's islands), and whether the step's selection
// clips it (the selection is on this region)
typedef struct { Atlas *a; PaintStep *p; const PaintRegion *r; const DrawIslands *I; bool sel; } StampCtx;

static StampCtx MakeCtx(DrawMode *dm, Atlas *a, PaintStep *p, int region)
{
    const PaintRegion *r = &p->regions[region];
    StampCtx c = { a, p, r, r->island >= 0 ? IslandsFor(dm, r->file, r->slot, r->w, r->h) : NULL, false };
    c.sel = p->selOn && memcmp(r, &p->selRegion, sizeof(*r)) == 0;
    return c;
}

// Blur and jumble at (x, y): from the start pixels of its 3 x 3 neighbours in the region (DtBlur on that
// window; jumble's pick keyed on the texel, DtTexelHash, as DtJumble). False: nothing to take
static bool FilterTexel(const StampCtx *c, int x, int y, unsigned char out[4])
{
    unsigned char px[36] = { 0 }, reg[9] = { 0 }, one = 255;
    const unsigned char *cand[8];       // (one texel's neighbours: stack scratch)
    int n = 0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            const unsigned char *s = StartPx(c->a, c->p, x + i, y + j);
            if (!s || !InRegion(c->r, c->I, x + i, y + j)) continue;
            memcpy(px + ((j + 1) * 3 + (i + 1)) * 4, s, 4);
            reg[(j + 1) * 3 + (i + 1)] = 255;
            if (i != 0 || j != 0) cand[n++] = s;
        }
    if (!reg[4]) return false;
    if (c->p->tool == TOOL_JUMBLE) {
        memcpy(out, n ? cand[DtTexelHash(c->p->seed, x, y) % (uint32_t)n] : px + 16, 4);
        return true;
    }
    DtImage img = { px, 3, 3, 12 };
    DtMask R = { 0, 0, 3, 3, reg }, W = { 1, 1, 1, 1, &one };
    DtBlur(out, &img, &R, &W);
    return true;
}

static void StampTexel(const StampCtx *c, int x, int y, float cov)
{
    if (!InRegion(c->r, c->I, x, y)) return;
    if (c->sel) {
        cov *= DtMaskGet(&c->p->sel, x, y) / 255.0f;
        if (cov <= 0.0f) return;
    }
    if (c->p->tool == TOOL_BLUR || c->p->tool == TOOL_JUMBLE) {
        unsigned char v[4];
        if (FilterTexel(c, x, y, v)) ApplyValue(c->a, c->p, x, y, cov, v, false);
        return;
    }
    ApplyTexel(c->a, c->p, x, y, cov);
}

// A texel of the region set to v (ApplyValue), clipped to the selection unless noClip (the move)
static void StampValue(const StampCtx *c, int x, int y, const unsigned char v[4], bool over, bool noClip)
{
    if (!InRegion(c->r, c->I, x, y)) return;
    float cov = 1.0f;
    if (c->sel && !noClip) {
        cov = DtMaskGet(&c->p->sel, x, y) / 255.0f;
        if (cov <= 0.0f) return;
    }
    ApplyValue(c->a, c->p, x, y, cov, v, over);
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
    StampCtx c = MakeCtx(dm, a, p, s.region);
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

// The region's texels as a mask (255) over its rectangle (heap; false: out of memory)
static bool RegionMaskOf(DrawMode *dm, const PaintRegion *r, DtMask *m)
{
    if (!DtMaskInit(m, r->x, r->y, r->w, r->h)) return false;
    const DrawIslands *I = r->island >= 0 ? IslandsFor(dm, r->file, r->slot, r->w, r->h) : NULL;
    for (int y = 0; m->a && y < m->h; y++)
        for (int x = 0; x < m->w; x++)
            if (InRegion(r, I, r->x + x, r->y + y)) m->a[(size_t)y * m->w + x] = 255;
    return true;
}

// The stroke's tiles back to its start pixels, their coverage cleared: a shape (or a pixel-perfect
// stroke) is drawn again from its input
static void StepRestore(Atlas *a, PaintStep *p)
{
    for (int i = 0; i < p->tileCount; i++) {
        PaintTile *t = &p->tiles[i];
        if (t->tx * DRAW_TILE + t->w > a->img.width || t->ty * DRAW_TILE + t->h > a->img.height) continue;
        for (int r = 0; r < t->h; r++)
            memcpy((unsigned char *)a->img.data + ((size_t)(t->ty * DRAW_TILE + r) * a->img.width + t->tx * DRAW_TILE) * 4,
                   t->pixels + (size_t)r * t->w * 4, (size_t)t->w * 4);
        if (t->mask) memset(t->mask, 0, (size_t)t->w * t->h);
        MarkDirty(a, t->tx * DRAW_TILE, t->ty * DRAW_TILE, t->tx * DRAW_TILE + t->w, t->ty * DRAW_TILE + t->h);
    }
}

// A pencil stroke at size 1 with pixel-perfect (docs/painting.md 18.1): each stretch of joined samples
// as the texels its segments pass through (as Stamp's size-1 footprint walks them), thinned by
// DtPixelPerfect, one texel each
static void RenderPixelPerfect(DrawMode *dm, Atlas *a, PaintStep *p)
{
    DtPoints path = { 0 };
    for (int i = 0; i < p->sampleCount; ) {
        int region = p->samples[i].region, j = i;
        DtPointsClear(&path);
        do {
            Sample s = p->samples[j];
            Vector2 b = { s.tx, s.ty }, pa = j > i ? (Vector2){ p->samples[j - 1].tx, p->samples[j - 1].ty } : b;
            int steps = ClampI((int)ceilf(Vector2Distance(pa, b) * 2.0f), 0, 1 << 16);
            for (int k = 0; k <= steps; k++) {
                float t = steps ? (float)k / steps : 1.0f;
                int kx = (int)floorf(pa.x + (b.x - pa.x) * t), ky = (int)floorf(pa.y + (b.y - pa.y) * t);
                if (path.n > 0 && path.p[path.n - 1].x == kx && path.p[path.n - 1].y == ky) continue;
                DtPointsPush(&path, kx, ky);
            }
            j++;
        } while (j < p->sampleCount && p->samples[j].joined && p->samples[j].region == region);
        DtPixelPerfect(&path);
        if (region >= 0 && region < p->regionCount) {
            StampCtx c = MakeCtx(dm, a, p, region);
            for (int k = 0; k < path.n; k++) StampTexel(&c, path.p[k].x, path.p[k].y, 1.0f);
        }
        i = j;
    }
    DtPointsFree(&path);
}

// The brush at each texel of an outline (line, curve, rectangle, ellipse)
static void RenderOutline(DrawMode *dm, Atlas *a, PaintStep *p, const DtPoints *o)
{
    for (int i = 0; i < o->n; i++) Stamp(dm, a, p, (Sample){ o->p[i].x + 0.5f, o->p[i].y + 0.5f, p->shapeRegion, false }, NULL);
}

// Every texel of mask m (> 0) set to the step's colour (source-over), clipped to the region and the selection
static void RenderMask(DrawMode *dm, Atlas *a, PaintStep *p, const DtMask *m)
{
    StampCtx c = MakeCtx(dm, a, p, p->shapeRegion);
    const unsigned char v[4] = { p->color.r, p->color.g, p->color.b, p->color.a };
    for (int y = 0; m->a && y < m->h; y++)
        for (int x = 0; x < m->w; x++)
            if (m->a[(size_t)y * m->w + x]) StampValue(&c, m->x + x, m->y + y, v, true, false);
}

// The bucket (contiguous / global): the flood of the start pixels from the seed within tolerance
static void RenderBucket(DrawMode *dm, Atlas *a, PaintStep *p)
{
    const PaintRegion *r = &p->regions[p->shapeRegion];
    DtMask region = { 0 }, out = { 0 };
    DtImage img = { a->img.data, a->img.width, a->img.height, a->img.width * 4 };
    if (RegionMaskOf(dm, r, &region) && DtMaskInit(&out, r->x, r->y, r->w, r->h) &&
        DtFlood(&out, &img, &region, p->pts.p[0], p->tolerance, p->bucket == DRAW_BUCKET_CONTIGUOUS))
        RenderMask(dm, a, p, &out);
    DtMaskFree(&region);
    DtMaskFree(&out);
}

// The gradient over the region (and the selection): color at pts[0] to color2 at pts[1], in sRGB bytes
// with alpha, source-over
static void RenderGradient(DrawMode *dm, Atlas *a, PaintStep *p)
{
    const PaintRegion *r = &p->regions[p->shapeRegion];
    StampCtx c = MakeCtx(dm, a, p, p->shapeRegion);
    const unsigned char c0[4] = { p->color.r, p->color.g, p->color.b, p->color.a };
    const unsigned char c1[4] = { p->color2.r, p->color2.g, p->color2.b, p->color2.a };
    for (int y = r->y; y < r->y + r->h; y++)
        for (int x = r->x; x < r->x + r->w; x++) {
            float t = DtGradientT(x, y, p->pts.p[0], p->pts.p[1], p->radial);
            unsigned char v[4];
            for (int k = 0; k < 4; k++) v[k] = (unsigned char)ClampI((int)lroundf(c0[k] + (c1[k] - c0[k]) * t), 0, 255);
            StampValue(&c, x, y, v, true, false);
        }
}

// The move: the selected texels (no selection: the region's) by dx, dy; where they land they replace
// the pixels, where they left and nothing landed the texels become transparent; within the region
static void RenderMove(DrawMode *dm, Atlas *a, PaintStep *p)
{
    const PaintRegion *r = &p->regions[p->shapeRegion];
    StampCtx c = MakeCtx(dm, a, p, p->shapeRegion);
    DtMask own = { 0 };
    const DtMask *src = c.sel ? &p->sel : &own;
    if (!c.sel && !RegionMaskOf(dm, r, &own)) return;
    static const unsigned char zero[4] = { 0, 0, 0, 0 };
    for (int y = r->y; y < r->y + r->h; y++)
        for (int x = r->x; x < r->x + r->w; x++) {
            int sx = x - p->dx, sy = y - p->dy;
            const unsigned char *from = DtMaskGet(src, sx, sy) && InRegion(r, c.I, sx, sy) ? StartPx(a, p, sx, sy) : NULL;
            if (from) {
                unsigned char v[4];
                memcpy(v, from, 4);         // (a copy: the write below may make this tile's copy first)
                StampValue(&c, x, y, v, false, true);
            } else if (DtMaskGet(src, x, y)) StampValue(&c, x, y, zero, false, true);
        }
    DtMaskFree(&own);
}

// The whole step drawn from its input onto the atlas (its tiles at their start pixels): a rebase's
// replay, a shape's preview and its end. The old pens (pencil, eraser, the whole-region fill) stamp
// their samples exactly as a live stroke did
static void StepRender(DrawMode *dm, Atlas *a, PaintStep *p)
{
    bool shape = p->shapeRegion >= 0 && p->shapeRegion < p->regionCount;
    DtPoints o = { 0 };
    DtMask m = { 0 };
    const DtPoint *q = p->pts.p;
    switch (shape ? p->tool : TOOL_PENCIL) {
    case TOOL_BUCKET:
        if (p->pts.n >= 1) RenderBucket(dm, a, p);
        break;
    case TOOL_GRADIENT:
        if (p->pts.n >= 2) RenderGradient(dm, a, p);
        break;
    case TOOL_MOVE:
        RenderMove(dm, a, p);
        break;
    case TOOL_LINE:
        if (p->pts.n >= 2) DtLine(&o, q[0], q[1]);
        break;
    case TOOL_CURVE:
        if (p->pts.n >= 4) DtCurve(&o, q[0], q[2], q[3], q[1]);
        break;
    case TOOL_RECT:
        if (p->pts.n >= 2) DtRectOutline(&o, q[0], q[1]);
        break;
    case TOOL_ELLIPSE:
        if (p->pts.n >= 2) DtEllipseOutline(&o, q[0], q[1]);
        break;
    case TOOL_RECT_FILL:
    case TOOL_ELLIPSE_FILL:
    case TOOL_CONTOUR:
    case TOOL_POLYGON: {
        const PaintRegion *r = &p->regions[p->shapeRegion];
        if (p->pts.n < 1 || !DtMaskInit(&m, r->x, r->y, r->w, r->h)) break;
        if (p->tool == TOOL_RECT_FILL && p->pts.n >= 2) DtRectFill(&m, q[0], q[1]);
        else if (p->tool == TOOL_ELLIPSE_FILL && p->pts.n >= 2) DtEllipseFill(&m, q[0], q[1]);
        else if (p->tool == TOOL_CONTOUR || p->tool == TOOL_POLYGON) DtPolygonFill(&m, q, p->pts.n);
        RenderMask(dm, a, p, &m);
        break;
    }
    default:
        if (p->pixelPerfect) RenderPixelPerfect(dm, a, p);
        else
            for (int i = 0; i < p->sampleCount; i++)
                Stamp(dm, a, p, p->samples[i], i > 0 && p->samples[i].joined ? &p->samples[i - 1] : NULL);
        break;
    }
    if (o.n > 0 && !o.oom) RenderOutline(dm, a, p, &o);
    DtPointsFree(&o);
    DtMaskFree(&m);
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

// An inactive entry of box hit h (its index) whose rectangle is smaller than the cells of the face
// on the material's grid (docs/painting.md 17.2: the runtime's PAINT_GRID_CELLS; the box grew, or the
// rectangle was made before the grid): Fit to grid gives it its cells. -1: none
static int GridTooSmall(const MS_SurfaceHit *h, int inactive)
{
    float rho = MS_PaintGridOf(scene, h->material);
    if (inactive < 0 || !HitIsBox(h) || rho <= 0.0f) return -1;
    const MS_PaintEntry *E = MS_PaintEntryAt(scene, inactive);
    for (int f = 0; f < 6; f++) {
        FaceCells c;
        if ((E->faces & (1u << f)) && FaceGridCells(h->hit.obj.index, f, rho, &c) && (c.w > E->w || c.h > E->h)) return inactive;
    }
    return -1;
}

// Whether a new atlas of a material on a paint grid of `grid` texels per metre (0: none) gets
// paint_filter nearest: on a grid unless Pixel look was turned off for grids this session (one texel
// is one cell of the surface's pixel art, docs/painting.md 17.7), else the kept pixel look
static bool NewAtlasNearest(const DrawMode *dm, float grid)
{
    return grid > 0.0f ? !dm->gridFiltered : dm->pixelLook;
}

// What paint at h goes into, or why it can't be painted (docs/painting.md 3.4); *g its rectangle.
// Runs every frame (the hover, the panel's target), so it says why with a code, and RefusalPrint
// words it. Returns:
//   0  paintable (*g)
//   1  nothing there (no text)
//   2  a light; 3 nothing to paint here; 4 the map changed under the hit; 5 the material's shader
//      can't take the paint stage; 6 a skinned model; 7 a model slot that can't be painted; 8 its
//      paint entry is inactive; 9 a new rectangle on the grid would pass 2048 texels a side (17.7);
//      10 its paint entry is inactive because its rectangle is smaller than the face's cells of the
//      grid (Fit to grid gives it them; *g has the entry, its grid and mapping)
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
    g->grid = MS_PaintGridOf(scene, m);
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
    int small = e < 0 ? GridTooSmall(h, inactive) : -1;
    if (e < 0 && inactive >= 0 && small < 0) return 8;
    AtlasRelOf(m, file, h->slot, &g->atlasW, &g->atlasH, &g->nearest);
    const char *rel = AtlasRelOf(m, file, h->slot, NULL, NULL, NULL);
    if (!rel || (m < 0 && g->atlasW <= 0)) g->nearest = NewAtlasNearest(dm, g->grid);     // (no atlas yet: what it will get)
    if (e >= 0 || small >= 0) {
        const MS_PaintEntry *E = MS_PaintEntryAt(scene, e >= 0 ? e : small);
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
            if (g->grid > 0.0f) {
                int big, cells;
                if (!BoxGroupCells(h->hit.obj.index, g->faces, g->grid, &g->w, &g->h, &big, &cells)) return 9;
            } else BoxRectSize(dm->density, h->hit.obj.index, g->faces, &g->w, &g->h);
        } else {
            SlotNewSize(dm, h->hit.obj.index, h->slot, m, RhoOf(dm, m), &g->w, &g->h, &g->estimate);
            if (g->grid > 0.0f && !g->estimate && (g->w > DRAW_RECT_MAX || g->h > DRAW_RECT_MAX)) return 9;
        }
    }
    // The mapping of the surface onto the rectangle's texels (a box face on a grid: its cells, 17.2)
    g->u1 = (float)g->w;
    g->v1 = (float)g->h;
    g->cellsW = g->w;
    g->cellsH = g->h;
    FaceCells c;
    if (HitIsBox(h) && g->grid > 0.0f && FaceGridCells(h->hit.obj.index, h->face, g->grid, &c)) {
        g->u0 = c.u0;
        g->u1 = c.u1;
        g->v0 = c.v0;
        g->v1 = c.v1;
        g->cellsW = c.w < g->w ? c.w : g->w;
        g->cellsH = c.h < g->h ? c.h : g->h;
    }
    if (!HitPaintUv(h, &g->uv)) g->uv = (Vector2){ -1.0f, -1.0f };
    if (small >= 0) return 10;
    g->ok = true;
    return 0;
}

// Resolve's refusal `code` at h in words, snprintf-style into out (size bytes; NULL / 0 measures);
// "" for 0 and 1. Right after Resolve (the scene as it saw it)
static int RefusalPrint(char *out, size_t size, unsigned char code, const MS_SurfaceHit *h)
{
    const char *name = code == 6 || code == 7 || (code == 9 && HitIsModel(h)) ? GetFileName(scene->modelFiles[scene->models[h->hit.obj.index].file].path) : "";
    int inactive = -1;
    if (code == 8 || code == 10) EntryFor(h->hit.obj, h->material, h->face, h->slot, &inactive);
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
    case 9: {
        float rho = MS_PaintGridOf(scene, h->material);
        if (HitIsBox(h)) {
            int w, hh, big, cells;
            BoxGroupCells(h->hit.obj.index, (unsigned char)(1u << h->face), rho, &w, &hh, &big, &cells);
            return snprintf(out, size, "face %s covers %d texels of the grid (%g/m); the most is %d", FACE_NAMES[big >= 0 ? big : h->face],
                            cells, (double)rho, DRAW_RECT_MAX);
        }
        return snprintf(out, size, "%s slot %d needs more than %d texels a side on the grid (%g/m)", name, h->slot, DRAW_RECT_MAX,
                        (double)rho);
    }
    case 10: {
        const MS_PaintEntry *E = inactive >= 0 ? MS_PaintEntryAt(scene, inactive) : NULL;
        FaceCells c = { 0 };
        if (HitIsBox(h)) FaceGridCells(h->hit.obj.index, h->face, MS_PaintGridOf(scene, h->material), &c);
        return snprintf(out, size, "its paint (map line %d, %dx%d) is smaller than the face's %dx%d cells of the grid: Fit to grid",
                        E ? E->line : 0, E ? E->w : 0, E ? E->h : 0, c.w, c.h);
    }
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

static void FreeUnwrapOf(DrawUnwrap *u)
{
    free(u->modelPath);
    free(u->sides);
    MS_PaintUnwrapFree(&u->u);
    memset(u, 0, sizeof(*u));
}

#define UNWRAP_TRIES 3          // unwraps of a file without a grid while its slot's square passes 2048

// Model file `file` gets its paint unwrap (docs/painting.md 17.5) when slot `slot` (of instance
// `model`, drawn with material m) needs one: xatlas at a fixed texel size, rho (the grid's texels per
// metre, else the panel's density) times the instance's largest scale per model unit; made, kept with
// each slot's square, provided (the next live edit uses it). Without a grid a square larger than 2048
// is made again at a lower density (the free density's clamp); on a grid it is refused. Returns 0 done
// (or not needed); 1 not made, why in MS_LastError() (no licence row: the sidecar derives from the
// model's; a degenerate file; too large for the grid; out of memory)
static unsigned char EnsureUnwrap(DrawMode *dm, int model, int slot, int m)
{
    int file = scene->models[model].file;
    if (MS_ModelPaintUv(scene, file, slot) != MS_PAINTUV_NEEDS_UNWRAP) return 0;
    const char *path = scene->modelFiles[file].path;
    for (int i = 0; i < dm->unwrapCount; i++)
        if (strcmp(dm->unwraps[i].modelPath, path) == 0) { ProvideUnwrap(&dm->unwraps[i]); return 0; }
    char *row = AssetsRow(path);
    if (!row) { MS_SetError("%s has no licence row in ASSETS.md (its paint unwrap derives from it)", path); return 1; }
    free(row);
    bool grid = MS_PaintGridOf(scene, m) > 0.0f;
    float scale = InstanceScale(model), rho = RhoOf(dm, m), tpu = rho * scale;
    int count = scene->modelFiles[file].model.materialCount > slot ? scene->modelFiles[file].model.materialCount : slot + 1;
    DrawUnwrap u = { 0 };
    u.sides = malloc(sizeof(int) * (size_t)(count > 0 ? count : 1));
    u.sideCount = count;
    if (!u.sides) { MS_SetError("out of memory"); return 1; }
    double t0 = GetTime();
    for (int t = 0;; t++) {
        if (!PaintUnwrapModelFileAt(scene, file, tpu, &u.u, u.sides, u.sideCount)) {
            MS_SetError("%s can't be unwrapped for paint: %s", path, MS_LastError());     // (MS_SetError may quote the last error)
            FreeUnwrapOf(&u);
            return 1;
        }
        int side = u.sides[slot];
        if (side <= DRAW_RECT_MAX) break;
        MS_PaintUnwrapFree(&u.u);
        if (grid) {
            MS_SetError("%s slot %d needs a %dx%d rectangle at the grid's %g texels/m (scale %g); the most is %d", path, slot, side, side,
                        (double)rho, (double)scale, DRAW_RECT_MAX);
            FreeUnwrapOf(&u);
            return 1;
        }
        if (t + 1 >= UNWRAP_TRIES) {
            MS_SetError("%s slot %d needs more than %d texels a side: lower the density", path, slot, DRAW_RECT_MAX);
            FreeUnwrapOf(&u);
            return 1;
        }
        tpu *= 0.98f * DRAW_RECT_MAX / side;
    }
    u.texelsPerUnit = tpu;
    u.scale = scale;
    u.modelPath = MS_StrDup(path);
    DrawUnwrap *slotU = u.modelPath ? MS_PUSH(dm->unwraps, dm->unwrapCount, dm->unwrapCap) : NULL;
    if (!slotU) {
        FreeUnwrapOf(&u);
        MS_SetError("out of memory");
        return 1;
    }
    *slotU = u;
    ProvideUnwrap(slotU);
    TraceLog(LOG_INFO, "monolith: draw: unwrapped %s for paint in %.0f ms (%.3g texels per unit, slot %d: %d texels a side)", path,
             (GetTime() - t0) * 1000.0, (double)tpu, slot, u.sides[slot]);
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
        MS_SetError("the paint atlas of %s is full at %dx%d%s", a->material ? a->material : a->rel, DRAW_ATLAS_MAX, DRAW_ATLAS_MAX,
                    MS_PaintGridOf(scene, m) > 0.0f ? "" : ": lower the density");
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
        op.paint.nearest = ownEntries ? near : NewAtlasNearest(dm, MS_PaintGridOf(scene, -1));
    } else op.paint.nearest = !rel && NewAtlasNearest(dm, MS_PaintGridOf(scene, m));
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
static bool HitRegion(DrawMode *dm, const MS_SurfaceHit *h, int e, PaintRegion *out, float *tx, float *ty);

static bool StrokeSample(DrawMode *dm, const MS_SurfaceHit *h, Sample *s)
{
    if (!dm->stroking || !dm->stroke) return false;
    if (h->hit.obj.kind != dm->strokeObj.kind || h->hit.obj.index != dm->strokeObj.index || h->material != dm->strokeMaterial) return false;
    bool model = HitIsModel(h);
    if (model ? h->slot != dm->strokeSlot : !HitIsBox(h)) return false;
    PaintStep *p = dm->stroke;
    int e = EntryFor(h->hit.obj, h->material, h->face, h->slot, NULL);
    if (e < 0) {
        // (a new rectangle: a box face's cells on a grid, else at the density; a model slot's 17.4 / 17.5)
        int w, hh, x, y;
        DrawGeom g;
        unsigned char refused = Resolve(dm, h, &g);
        if (refused != 0) {     // (a face the stroke went onto that can't take paint: skipped, said)
            if (refused >= 2) SayRefusal(dm, refused, h, "nothing to paint here");
            return false;
        }
        w = g.w;
        hh = g.h;
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
    PaintRegion r;
    if (!HitRegion(dm, h, e, &r, &s->tx, &s->ty)) return false;
    s->region = RegionIndex(p, r);
    s->joined = false;
    return s->region >= 0;
}

// The region and texel (atlas pixels, sub-texel) of hit h on its entry e: a box face on a grid its
// cells (the texels it shows: the rectangle's lower-left part, 17.3) and the hit's texel from its
// first cell (17.6), else the face's 0..1 over the whole rectangle; a model the hit triangle's island.
// False: no paint coordinate or island there
static bool HitRegion(DrawMode *dm, const MS_SurfaceHit *h, int e, PaintRegion *out, float *tx, float *ty)
{
    bool model = HitIsModel(h);
    float rho = MS_PaintGridOf(scene, h->material);
    const MS_PaintEntry *E = MS_PaintEntryAt(scene, e);
    Vector2 uv;
    if (!HitPaintUv(h, &uv)) return false;
    PaintRegion r = { E->x, E->y, E->w, E->h, -1, -1, -1 };
    FaceCells fc;
    bool grid = !model && rho > 0.0f && FaceGridCells(h->hit.obj.index, h->face, rho, &fc);
    if (grid) {
        r.w = fc.w < E->w ? fc.w : E->w;
        r.h = fc.h < E->h ? fc.h : E->h;
    }
    if (model) {
        int file = scene->models[h->hit.obj.index].file;
        const DrawIslands *I = IslandsFor(dm, file, h->slot, E->w, E->h);
        int k = HitTriangle(h);
        r.file = file;
        r.slot = h->slot;
        r.island = I && k >= 0 && k < I->triCount ? I->triIsland[k] : -1;
        if (r.island < 0) return false;
    }
    *out = r;
    if (grid) {
        *tx = E->x + fc.u0 + Clamp01(uv.x) * (fc.u1 - fc.u0);
        *ty = E->y + fc.v0 + Clamp01(uv.y) * (fc.v1 - fc.v0);
    } else {
        *tx = E->x + Clamp01(uv.x) * E->w;
        *ty = E->y + Clamp01(uv.y) * E->h;
    }
    return true;
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
    // (pixel-perfect: the stroke is drawn again from its samples before the frame's upload, StrokeFrame)
    if (st->pixelPerfect) dm->ppDirty = true;
    else Stamp(dm, a, st, s, s.joined ? prev : NULL);
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
    if (g.needsUnwrap && EnsureUnwrap(dm, h->hit.obj.index, h->slot, h->material) != 0) { Sayf(dm, true, "%s", MS_LastError()); return false; }
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

static void StrokeFrame(DrawMode *dm);

// LMB up: the step goes into the history (one that allocated is there already); auto save writes.
// The pencil's and the eraser's last point stays for SHIFT+click
static bool StrokeEnd(DrawMode *dm, EditMode *em)
{
    if (!dm->stroking) return false;
    StrokeFrame(dm);
    PaintStep *p = dm->stroke;
    Atlas *a = &dm->atlases[p->atlas];
    if ((p->tool == TOOL_PENCIL || p->tool == TOOL_ERASER) && p->sampleCount > 0) {
        const Sample *s = &p->samples[p->sampleCount - 1];
        const PaintRegion *r = &p->regions[s->region];
        dm->lastPaintSet = true;
        dm->lastPaintRegion = (DrawRegion){ r->x, r->y, r->w, r->h, r->file, r->slot, r->island };
        dm->lastPaintAtlas = p->atlas;
        dm->lastPaintTx = s->tx;
        dm->lastPaintTy = s->ty;
    }
    Flush(dm);
    EndTiles(a, p);
    int tiles = p->tileCount;
    const char *verb = p->pen == PEN_ERASER ? "erased" : p->pen == PEN_FILL ? "filled" : p->tool == TOOL_MOVE ? "moved pixels in" : "painted";
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
// #region The tools: brush strokes' extras, shapes, the selection, the move (docs/painting.md 18.2,
// 18.4, 18.5)
// ======================================================================================

static Vector3 FacePoint(int b, int f, Vector2 fu);
static void OutlineBuild(DrawMode *dm, const DtMask *m, const MS_SurfaceHit *h, DrawLines *out);

static DrawRegion ToDrawRegion(PaintRegion r) { return (DrawRegion){ r.x, r.y, r.w, r.h, r.file, r.slot, r.island }; }
static PaintRegion ToPaintRegion(DrawRegion r) { return (PaintRegion){ r.x, r.y, r.w, r.h, r.file, r.slot, r.island }; }
static bool SameRegion(DrawRegion a, PaintRegion b)
{
    PaintRegion pa = ToPaintRegion(a);
    return memcmp(&pa, &b, sizeof(pa)) == 0;
}

static DtPoint ClampToRegion(DtPoint p, const PaintRegion *r)
{
    return (DtPoint){ ClampI(p.x, r->x, r->x + r->w - 1), ClampI(p.y, r->y, r->y + r->h - 1) };
}

// A stroke that must not go into the history (a shape cancelled, a press that found no texel): what it
// painted goes back. A step that allocated a rectangle is in the history already: it stays (CTRL+Z)
static void StrokeDrop(DrawMode *dm)
{
    if (!dm->stroking || !dm->stroke) { StrokeAbort(dm); return; }
    PaintStep *p = dm->stroke;
    Atlas *a = &dm->atlases[p->atlas];
    StepRestore(a, p);
    Flush(dm);
    if (!dm->strokePushed) ReleaseStep(dm, p);
    else EndTiles(a, p);
    StrokeAbort(dm);
}

// The step clipped to the selection when it is on the step's atlas (the copy is the step's input)
static void StepSelection(DrawMode *dm, PaintStep *p)
{
    if (!dm->sel.active || dm->sel.atlas != p->atlas) return;
    p->selOn = DtMaskCopy(&p->sel, &dm->sel.mask);
    p->selRegion = ToPaintRegion(dm->sel.region);
}

// The tool input of a stroke that just began: the button's colours, the options, the selection
static void StepTool(DrawMode *dm, PaintStep *p, DrawTool tool, bool rmb)
{
    p->tool = tool;
    p->color = rmb ? dm->bg : dm->fg;
    p->color2 = rmb ? dm->fg : dm->bg;      // (the gradient runs bg -> fg with RMB)
    p->pixelPerfect = tool == TOOL_PENCIL && dm->pixelPerfect && dm->size == 1;
    p->seed = dm->seed++;
    StepSelection(dm, p);
}

// A pixel-perfect stroke's samples since the last frame: the stroke drawn again (the thinning can
// change texels behind the newest)
static void StrokeFrame(DrawMode *dm)
{
    if (!dm->ppDirty || !dm->stroking || !dm->stroke) return;
    dm->ppDirty = false;
    Atlas *a = &dm->atlases[dm->stroke->atlas];
    StepRestore(a, dm->stroke);
    StepRender(dm, a, dm->stroke);
}

// One spray tick (1/60 s) at the probe: spraySpeed dots, each a brush stamp at a random texel in the
// disc of the spray width, recorded as samples (a replay stamps them again)
static void SprayTick(DrawMode *dm, const Probe *pr)
{
    PaintStep *p = dm->stroke;
    if (!p || !pr->ok) return;
    Atlas *a = &dm->atlases[p->atlas];
    DtPoints dots = { 0 };
    DtSpray(&dots, pr->s.tx, pr->s.ty, dm->sprayWidth * 0.5f, dm->spraySpeed, p->seed ^ ((uint32_t)p->sampleCount * 2654435761u));
    for (int i = 0; i < dots.n; i++) {
        Sample s = { dots.p[i].x + 0.5f, dots.p[i].y + 0.5f, pr->s.region, false };
        Stamp(dm, a, p, s, NULL);
        Sample *slot = MS_PUSH(p->samples, p->sampleCount, p->sampleCap);
        if (slot) *slot = s;
    }
    DtPointsFree(&dots);
}

// The region, texel and atlas of the paint under h (an entry must exist: selections and the move
// work on pixels). False: no paint there
static bool SurfaceRegion(DrawMode *dm, const MS_SurfaceHit *h, PaintRegion *r, float *tx, float *ty, int *atlas)
{
    if (!HitIsBox(h) && !HitIsModel(h)) return false;
    int e = EntryFor(h->hit.obj, h->material, h->face, h->slot, NULL);
    if (e < 0 || !HitRegion(dm, h, e, r, tx, ty)) return false;
    const char *rel = EntryAtlasRel(MS_PaintEntryAt(scene, e));
    if (!rel) return false;
    int ai = AtlasByRel(dm, rel);
    if (ai < 0 || dm->atlases[ai].stale) {
        char copy[MS_PATH_MAX];
        snprintf(copy, sizeof(copy), "%s", rel);
        int file = HitIsModel(h) ? scene->models[h->hit.obj.index].file : -1;
        ai = AtlasOpen(dm, copy, h->material >= 0 ? scene->materials[h->material].name : NULL, file >= 0 ? scene->modelFiles[file].path : NULL,
                       h->slot);
    }
    if (ai < 0 || !dm->atlases[ai].img.data) return false;
    *atlas = ai;
    return true;
}

// The texel the ray points at on the shape's region (docs/painting.md 18.4): the hit's when it lies on
// the region; else the ray through the plane of the last hit on it (a box face's plane, a model's
// triangle), extended past the surface; clamped to the region's rectangle. False: nowhere (the ray is
// parallel to the plane or points away), the point stays
static bool ShapeTexel(DrawMode *dm, Ray ray, DtPoint *out)
{
    DrawShape *S = &dm->shapeDrag;
    PaintRegion want = ToPaintRegion(S->region);
    MS_SurfaceHit h;
    MS_RaycastSurface(scene, ray.position, ray.direction, 0.0f, &h);
    PaintRegion r;
    float tx, ty;
    int e = HitIsBox(&h) || HitIsModel(&h) ? EntryFor(h.hit.obj, h.material, h.face, h.slot, NULL) : -1;
    if (e >= 0 && h.hit.obj.kind == S->hit.hit.obj.kind && h.hit.obj.index == S->hit.hit.obj.index && h.material == S->hit.material &&
        HitRegion(dm, &h, e, &r, &tx, &ty) && memcmp(&r, &want, sizeof(r)) == 0) {
        S->lastOn = h;
        *out = ClampToRegion((DtPoint){ (int)floorf(tx), (int)floorf(ty) }, &want);
        return true;
    }
    const MS_SurfaceHit *L = &S->lastOn;
    e = EntryFor(L->hit.obj, L->material, L->face, L->slot, NULL);
    if (e < 0) return false;
    const MS_PaintEntry *E = MS_PaintEntryAt(scene, e);
    Vector3 O, U, V;
    Vector2 o2 = { 0, 0 }, u2 = { 1, 0 }, v2 = { 0, 1 };     // (the paint coordinates of O, O + U, O + V)
    if (HitIsBox(L)) {
        O = FacePoint(L->hit.obj.index, L->face, (Vector2){ 0, 0 });
        U = Vector3Subtract(FacePoint(L->hit.obj.index, L->face, (Vector2){ 1, 0 }), O);
        V = Vector3Subtract(FacePoint(L->hit.obj.index, L->face, (Vector2){ 0, 1 }), O);
    } else {
        int k = HitTriangle(L);
        Vector2 puv[3];
        Vector3 pos[3];
        if (k < 0 || !MS_ModelSlotPaintTriangle(scene, scene->models[L->hit.obj.index].file, L->slot, k, puv, pos)) return false;
        const Matrix T = scene->models[L->hit.obj.index].transform;
        O = Vector3Transform(pos[0], T);
        U = Vector3Subtract(Vector3Transform(pos[1], T), O);
        V = Vector3Subtract(Vector3Transform(pos[2], T), O);
        o2 = puv[0];
        u2 = puv[1];
        v2 = puv[2];
    }
    Vector3 N = Vector3CrossProduct(U, V);
    float den = Vector3DotProduct(ray.direction, N);
    if (fabsf(den) < 1e-12f) return false;
    float t = Vector3DotProduct(Vector3Subtract(O, ray.position), N) / den;
    if (t < 0.0f) return false;
    Vector3 d = Vector3Subtract(Vector3Add(ray.position, Vector3Scale(ray.direction, t)), O);
    float uu = Vector3DotProduct(U, U), uv = Vector3DotProduct(U, V), vv = Vector3DotProduct(V, V);
    float du = Vector3DotProduct(d, U), dv = Vector3DotProduct(d, V), g = uu * vv - uv * uv;
    if (fabsf(g) < 1e-20f) return false;
    float a = (du * vv - dv * uv) / g, b = (dv * uu - du * uv) / g;       // d = a U + b V
    Vector2 p = { o2.x + a * (u2.x - o2.x) + b * (v2.x - o2.x), o2.y + a * (u2.y - o2.y) + b * (v2.y - o2.y) };
    FaceCells fc;
    float rho = MS_PaintGridOf(scene, L->material);
    if (HitIsBox(L) && rho > 0.0f && FaceGridCells(L->hit.obj.index, L->face, rho, &fc)) {
        tx = E->x + fc.u0 + p.x * (fc.u1 - fc.u0);
        ty = E->y + fc.v0 + p.y * (fc.v1 - fc.v0);
    } else {
        tx = E->x + p.x * E->w;
        ty = E->y + p.y * E->h;
    }
    if (!isfinite(tx) || !isfinite(ty)) return false;
    tx = fminf(fmaxf(tx, -1e6f), 1e6f);
    ty = fminf(fmaxf(ty, -1e6f), 1e6f);
    *out = ClampToRegion((DtPoint){ (int)floorf(tx), (int)floorf(ty) }, &want);
    return true;
}

static bool ShapePaints(DrawTool t) { return DrawToolKindOf(t) != TOOLKIND_SELECT && t != TOOL_EYEDROPPER; }

// A shape, a bucket click or a move begins at hit h (docs/painting.md 18.4): its stroke's step (the
// atlas; a first rectangle when the surface has none) and its first point; selection tools: the
// paint's region under h, no step. False with the reason said
static bool ShapeBegin(DrawMode *dm, EditMode *em, const MS_SurfaceHit *h, DrawTool tool, bool rmb, DtSelOp op)
{
    DrawShape *S = &dm->shapeDrag;
    PaintRegion r;
    float tx, ty;
    int ai;
    if (ShapePaints(tool)) {
        if (!StrokeBegin(dm, em, h)) return false;
        Sample s;
        if (!StrokeSample(dm, h, &s)) {
            if (!dm->messageError) SayStatic(dm, true, "no paint texel there");
            StrokeDrop(dm);
            return false;
        }
        PaintStep *p = dm->stroke;
        StepTool(dm, p, tool, rmb);
        p->shapeRegion = s.region;
        r = p->regions[s.region];
        tx = s.tx;
        ty = s.ty;
        ai = p->atlas;
    } else if (!SurfaceRegion(dm, h, &r, &tx, &ty, &ai)) {
        SayStatic(dm, true, "nothing painted here: a selection needs the surface's paint (paint it first)");
        return false;
    }
    S->on = true;
    S->tool = tool;
    S->rmb = rmb;
    S->selOp = op;
    S->stage = 0;
    S->hit = *h;
    S->lastOn = *h;
    S->region = ToDrawRegion(r);
    S->atlas = ai;
    S->down = true;
    S->clickTime = (float)GetTime();
    S->outline.n = 0;
    DtPointsClear(&S->pts);
    DtPoint p0 = ClampToRegion((DtPoint){ (int)floorf(tx), (int)floorf(ty) }, &r);
    int n = tool == TOOL_CURVE ? 4 : tool == TOOL_BUCKET || tool == TOOL_WAND || tool == TOOL_CONTOUR || tool == TOOL_LASSO ? 1 : 2;
    for (int i = 0; i < n; i++) DtPointsPush(&S->pts, p0.x, p0.y);
    return !S->pts.oom;
}

// SHIFT: lines snap to 45 degrees, rectangles and ellipses become squares and circles (b moves)
static DtPoint Constrain(DrawTool tool, DtPoint a, DtPoint b)
{
    int dx = b.x - a.x, dy = b.y - a.y, ax = abs(dx), ay = abs(dy);
    if ((tool == TOOL_LINE || tool == TOOL_CURVE) && ax > 2 * ay) dy = 0;
    else if ((tool == TOOL_LINE || tool == TOOL_CURVE) && ay > 2 * ax) dx = 0;
    else {
        int m = ax > ay ? ax : ay;
        dx = dx < 0 ? -m : m;
        dy = dy < 0 ? -m : m;
    }
    return (DtPoint){ a.x + dx, a.y + dy };
}

// A selection shape's mask so far (its preview) and its outline on the surface
static void SelectPreview(DrawMode *dm)
{
    DrawShape *S = &dm->shapeDrag;
    PaintRegion r = ToPaintRegion(S->region);
    DtMaskFree(&S->preview);
    if (!DtMaskInit(&S->preview, r.x, r.y, r.w, r.h)) return;
    const DtPoint *q = S->pts.p;
    if (S->tool == TOOL_MARQUEE && S->pts.n >= 2) DtRectFill(&S->preview, q[0], q[1]);
    else if (S->tool == TOOL_MARQUEE_ELLIPSE && S->pts.n >= 2) DtEllipseFill(&S->preview, q[0], q[1]);
    else if (S->tool == TOOL_LASSO || S->tool == TOOL_LASSO_POLY) DtPolygonFill(&S->preview, q, S->pts.n);
    OutlineBuild(dm, &S->preview, &S->hit, &S->outline);
}

// The shape drawn again from its points: a painting tool into the atlas (its stroke's tiles back to
// their start pixels first), a selection tool as its preview outline
static void ShapeRedraw(DrawMode *dm)
{
    DrawShape *S = &dm->shapeDrag;
    if (!S->on) return;
    if (!ShapePaints(S->tool)) { SelectPreview(dm); return; }
    PaintStep *p = dm->stroke;
    if (!dm->stroking || !p) return;
    Atlas *a = &dm->atlases[p->atlas];
    DtPointsClear(&p->pts);
    for (int i = 0; i < S->pts.n; i++) DtPointsPush(&p->pts, S->pts.p[i].x, S->pts.p[i].y);
    if (S->tool == TOOL_MOVE && S->pts.n >= 2) {
        p->dx = S->pts.p[1].x - S->pts.p[0].x;
        p->dy = S->pts.p[1].y - S->pts.p[0].y;
    }
    StepRestore(a, p);
    StepRender(dm, a, p);
}

// The selection becomes mask m (op: replace, add, subtract, intersect) on region r of atlas ai, made on
// hit h. A selection on another region is replaced; texels outside the region never stay selected
static void SelectionApply(DrawMode *dm, const PaintRegion *r, int ai, const MS_SurfaceHit *h, const DtMask *m, DtSelOp op)
{
    DrawSelection *S = &dm->sel;
    if (!S->active || S->atlas != ai || !SameRegion(S->region, *r)) {
        DtMask rm = { 0 }, nm = { 0 };
        if (!RegionMaskOf(dm, r, &rm) || !DtMaskInit(&nm, r->x, r->y, r->w, r->h)) {
            DtMaskFree(&rm);
            SayStatic(dm, true, "out of memory");
            return;
        }
        DtMaskFree(&S->mask);
        DtMaskFree(&S->regionMask);
        S->mask = nm;
        S->regionMask = rm;
        S->region = ToDrawRegion(*r);
        S->atlas = ai;
    }
    S->hit = *h;
    DtMaskCombine(&S->mask, m, op);
    DtMaskClip(&S->mask, &S->regionMask);
    S->active = !DtMaskEmpty(&S->mask);
    S->gen++;
    int x0, y0, x1, y1;
    if (DtMaskBounds(&S->mask, &x0, &y0, &x1, &y1)) Sayf(dm, false, "selection: %d×%d texels", x1 - x0 + 1, y1 - y0 + 1);
    else SayStatic(dm, false, "selection: none");
}

// The selection dropped (CTRL+D: kept for CTRL+SHIFT+D)
static void SelectionDrop(DrawMode *dm, bool keep)
{
    DrawSelection *S = &dm->sel;
    if (!S->active) return;
    if (keep && DtMaskCopy(&S->last, &S->mask)) {
        S->lastSet = true;
        S->lastRegion = S->region;
        S->lastAtlas = S->atlas;
        S->lastHit = S->hit;
    }
    S->active = false;
    S->gen++;
}

// Whether region r of atlas ai still exists: an active entry of that atlas starts at its corner and
// holds it (gone, moved or shrunk after a reload or an undo: false)
static bool RegionLive(const DrawMode *dm, DrawRegion r, int ai)
{
    if (ai < 0 || ai >= dm->atlasCount) return false;
    int n = MS_PaintEntryCount(scene);
    for (int i = 0; i < n; i++) {
        const MS_PaintEntry *E = MS_PaintEntryAt(scene, i);
        const char *rel = E->active ? EntryAtlasRel(E) : NULL;
        if (rel && strcmp(rel, dm->atlases[ai].rel) == 0 && E->x == r.x && E->y == r.y && r.w <= E->w && r.h <= E->h) return true;
    }
    return false;
}

// After a reload or an undo: a selection (and the one CTRL+D kept) whose region is gone is dropped
static void SelectionValidate(DrawMode *dm)
{
    DrawSelection *S = &dm->sel;
    if (S->active && !RegionLive(dm, S->region, S->atlas)) {
        S->active = false;
        S->gen++;
    }
    if (S->lastSet && !RegionLive(dm, S->lastRegion, S->lastAtlas)) S->lastSet = false;
}

// The selection's outline for this frame (rebuilt only when the selection or the scene changed)
static void PrepareSelection(DrawMode *dm)
{
    DrawSelection *S = &dm->sel;
    if (!S->active) return;
    if (S->outlineSet && S->outlineGen == S->gen && S->outlineVersion == scene->version) return;
    OutlineBuild(dm, &S->mask, &S->hit, &S->outline);
    S->outlineSet = true;
    S->outlineGen = S->gen;
    S->outlineVersion = scene->version;
}

// CTRL+A on hit h (the paint under the cursor, else the selection's region): all of the region
static void SelectAllAt(DrawMode *dm, const MS_SurfaceHit *h)
{
    PaintRegion r;
    float tx, ty;
    int ai;
    if (h && SurfaceRegion(dm, h, &r, &tx, &ty, &ai)) {
        DtMask m = { 0 };
        if (RegionMaskOf(dm, &r, &m)) SelectionApply(dm, &r, ai, h, &m, DT_SEL_REPLACE);
        DtMaskFree(&m);
    } else if (dm->sel.active) {
        DtMaskCombine(&dm->sel.mask, &dm->sel.regionMask, DT_SEL_REPLACE);
        dm->sel.gen++;
        SayStatic(dm, false, "selection: all of its region");
    } else SayStatic(dm, true, "nothing painted here to select");
}

static void SelectAll(DrawMode *dm) { SelectAllAt(dm, dm->hoverOk ? &dm->hover : NULL); }

static void Deselect(DrawMode *dm)
{
    if (!dm->sel.active) return;
    SelectionDrop(dm, true);
    SayStatic(dm, false, "deselected (CTRL+SHIFT+D brings it back)");
}

static void Reselect(DrawMode *dm)
{
    DrawSelection *S = &dm->sel;
    SelectionValidate(dm);
    if (!S->lastSet) { SayStatic(dm, true, "nothing to reselect"); return; }
    PaintRegion r = ToPaintRegion(S->lastRegion);
    DtMask keep = { 0 };
    if (!DtMaskCopy(&keep, &S->last)) { SayStatic(dm, true, "out of memory"); return; }
    S->active = false;
    SelectionApply(dm, &r, S->lastAtlas, &S->lastHit, &keep, DT_SEL_REPLACE);
    DtMaskFree(&keep);
}

static void SelectInvert(DrawMode *dm)
{
    if (!dm->sel.active) { SayStatic(dm, true, "nothing selected to invert"); return; }
    DtMaskInvert(&dm->sel.mask, &dm->sel.regionMask);
    dm->sel.active = !DtMaskEmpty(&dm->sel.mask);
    dm->sel.gen++;
    SayStatic(dm, false, dm->sel.active ? "selection inverted" : "selection inverted: none");
}

// The magic wand at hit h: the bucket's flood (tolerance, contiguous / global) of the paint as a selection
static void WandAt(DrawMode *dm, const MS_SurfaceHit *h, DtSelOp op)
{
    PaintRegion r;
    float tx, ty;
    int ai;
    if (!SurfaceRegion(dm, h, &r, &tx, &ty, &ai)) { SayStatic(dm, true, "nothing painted here to select"); return; }
    const Atlas *a = &dm->atlases[ai];
    DtImage img = { a->img.data, a->img.width, a->img.height, a->img.width * 4 };
    DtMask region = { 0 }, out = { 0 };
    DtPoint seed = ClampToRegion((DtPoint){ (int)floorf(tx), (int)floorf(ty) }, &r);
    if (RegionMaskOf(dm, &r, &region) && DtMaskInit(&out, r.x, r.y, r.w, r.h) &&
        DtFlood(&out, &img, &region, seed, dm->tolerance, dm->bucket != DRAW_BUCKET_GLOBAL))
        SelectionApply(dm, &r, ai, h, &out, op);
    else SayStatic(dm, true, "the magic wand found nothing there");
    DtMaskFree(&region);
    DtMaskFree(&out);
}

// The shape ends: a painting shape is drawn once more and its stroke goes into the history (one
// step), a move takes the selection with it; a selection shape becomes the selection
static void ShapeCommit(DrawMode *dm, EditMode *em)
{
    DrawShape *S = &dm->shapeDrag;
    if (!S->on) return;
    if (!ShapePaints(S->tool)) {
        SelectPreview(dm);
        PaintRegion r = ToPaintRegion(S->region);
        SelectionApply(dm, &r, S->atlas, &S->hit, &S->preview, S->selOp);
        S->on = false;
        S->outline.n = 0;
        return;
    }
    ShapeRedraw(dm);
    PaintStep *p = dm->stroke;
    if (S->tool == TOOL_MOVE && p && p->selOn && dm->sel.active && dm->sel.atlas == p->atlas &&
        SameRegion(dm->sel.region, p->regions[p->shapeRegion])) {
        DtMask moved = { 0 };
        if (DtMaskInit(&moved, dm->sel.mask.x, dm->sel.mask.y, dm->sel.mask.w, dm->sel.mask.h)) {
            DtMaskShift(&moved, &dm->sel.mask, p->dx, p->dy);
            DtMaskClip(&moved, &dm->sel.regionMask);
            DtMaskFree(&dm->sel.mask);
            dm->sel.mask = moved;
            dm->sel.active = !DtMaskEmpty(&moved);
            dm->sel.gen++;
        }
    }
    S->on = false;
    StrokeEnd(dm, em);
}

// ESC, a tool change, leaving: the shape in progress dropped (its pixels back, no step in the
// history; a rectangle its first point allocated stays, CTRL+Z removes it). True when there was one
static bool ShapeCancel(DrawMode *dm)
{
    DrawShape *S = &dm->shapeDrag;
    if (!S->on) return false;
    if (ShapePaints(S->tool)) StrokeDrop(dm);
    S->on = false;
    S->outline.n = 0;
    return true;
}

// ENTER: closes the polygon / polygonal lasso (its fixed points), ends the curve (no control point
// yet: straight; after the first: c2 = c1)
static void ShapeEnter(DrawMode *dm, EditMode *em)
{
    DrawShape *S = &dm->shapeDrag;
    if (!S->on) return;
    if (S->tool == TOOL_POLYGON || S->tool == TOOL_LASSO_POLY) {
        if (S->pts.n > 1) S->pts.n--;       // (the point that follows the cursor)
        if (S->pts.n >= 3) ShapeCommit(dm, em);
        else if (ShapeCancel(dm)) SayStatic(dm, false, "a polygon needs 3 points");
    } else if (S->tool == TOOL_CURVE && S->stage >= 1) {
        if (S->stage == 1) {
            S->pts.p[2] = S->pts.p[0];
            S->pts.p[3] = S->pts.p[1];
        } else S->pts.p[3] = S->pts.p[2];
        ShapeCommit(dm, em);
    }
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
    StepRender(dm, a, p);
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
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    free(*text);
    *text = MonoStrv(fmt, arg_ptr);
    va_end(arg_ptr);
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

// The surface a panel action acts on: a box or a model slot with paint (its entry, active; small:
// also an inactive one whose rectangle is smaller than its face's cells of the grid, Resolve's 10)
static bool ActionEntry(DrawMode *dm, const MS_SurfaceHit *h, const char *what, int *entry, bool small)
{
    if (!HitIsBox(h) && !HitIsModel(h)) {
        Sayf(dm, true, "%s: point at a painted box or model first (the panel acts on the surface the cursor rests on)", what);
        return false;
    }
    if (!dm->em->ed.work && !MapEditBegin(&dm->em->ed)) { SayStatic(dm, true, "this map can't be painted (a compiled map?)"); return false; }
    int inactive;
    *entry = EntryFor(h->hit.obj, h->material, h->face, h->slot, &inactive);
    if (*entry < 0 && small) *entry = GridTooSmall(h, inactive);
    if (*entry < 0) { Sayf(dm, true, "%s: %s has no paint yet: paint it first", what, HitIsBox(h) ? "this face" : "this slot"); return false; }
    return true;
}

// How MoveEntry carries the old paint over
typedef enum { MOVE_TEXELS = 0, MOVE_NEAREST, MOVE_BILINEAR } MoveMode;

// The entry of object obj that holds `faces` (or `slot`) in material m, with the rectangle o (ox oy
// ow oh) of step p's atlas, gets a new rectangle w x h (EDIT_PAINT_SET; exclude: its index, so its
// old room may be reused): its paint copied texel for texel into the new one's lower-left corner
// (MOVE_TEXELS: a box face on a grid shows its texels from its first cell, so the surface doesn't
// change where the rectangle still covers it) or resampled onto it, the old rectangle cleared. *rx
// *ry where it went. Returns 0 done; 1 not, why in MS_LastError() (when the step wasn't pushed it is
// released then)
static unsigned char MoveEntry(DrawMode *dm, PaintStep *p, bool *pushed, MS_ObjRef obj, int m, unsigned char faces, int slot,
                               int exclude, int ox, int oy, int ow, int oh, int w, int h, MoveMode mode, int *rx, int *ry)
{
    Atlas *a = &dm->atlases[p->atlas];
    // The old paint, before anything moves
    Image old = NewImage(ow, oh);
    if (!old.data) {
        MS_SetError("out of memory");
        if (!*pushed) ReleaseStep(dm, p);
        return 1;
    }
    for (int y = 0; y < oh; y++)
        memcpy((unsigned char *)old.data + (size_t)y * ow * 4, (const unsigned char *)a->img.data + ((size_t)(oy + y) * a->img.width + ox) * 4,
               (size_t)ow * 4);
    if (AddRect(dm, p, pushed, obj, m, faces, slot, w, h, exclude, EDIT_PAINT_SET, rx, ry) != 0) {     // (not added: why)
        FreeImage(&old);
        return 1;
    }
    a = &dm->atlases[p->atlas];
    // (the old rectangle cleared, but where the new one lies: it was cleared already, and is drawn next)
    ClearRect(a, p, ox, oy, ow, oh);
    for (int ty = 0; ty < h; ty++)
        for (int tx = 0; tx < w; tx++) {
            unsigned char px[4] = { 0, 0, 0, 0 };
            if (mode == MOVE_TEXELS) {
                if (tx >= ow || ty >= oh) continue;
                memcpy(px, (const unsigned char *)old.data + ((size_t)ty * ow + tx) * 4, 4);
            } else if (mode == MOVE_NEAREST) memcpy(px, (const unsigned char *)old.data + ((size_t)(ty * oh / h) * ow + tx * ow / w) * 4, 4);
            else {
                float s[4];
                SampleRect(&old, 0, 0, ow, oh, (tx + 0.5f) / w, (ty + 0.5f) / h, s);
                for (int c = 0; c < 4; c++) px[c] = (unsigned char)ClampI((int)lroundf(s[c]), 0, 255);
                if (px[3] == 0) px[0] = px[1] = px[2] = 0;
            }
            SetTexel(a, p, *rx + tx, *ry + ty, px);
        }
    FreeImage(&old);
    return 0;
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
    float rho = MS_PaintGridOf(scene, h->material);
    for (int n = 0; n < 6 && ok; n++) {
        unsigned char G = 0;
        for (int f = 0; f < 6; f++) if (g[f] == n) G |= (unsigned char)(1u << f);
        if (!G) continue;
        // On a grid the group's rectangle is its cells (17.3: per axis its largest face's)
        int cw = 0, ch = 0, big, cells;
        if (rho > 0.0f && !BoxGroupCells(h->hit.obj.index, G, rho, &cw, &ch, &big, &cells)) {
            MS_SetError("faces: face %s covers %d texels of the grid (%g/m); the most is %d", FACE_NAMES[big], cells, (double)rho,
                        DRAW_RECT_MAX);
            why = MS_StrDup(MS_LastError());
            ok = false;
            break;      // (the step, not pushed by an earlier group, is released by EditStepEnd)
        }
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
            Snap *B = &snaps[best];
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
            // (on a grid, a group that now covers more cells than its rectangle grows to them, its
            // paint moved texel for texel: what its faces showed stays)
            if (ok && rho > 0.0f && (B->w < cw || B->h < ch)) {
                int first = 0, inactive, x, y;
                while (!(G & (1u << first))) first++;
                int e = EntryFor(h->hit.obj, h->material, first, -1, &inactive);
                ok = MoveEntry(dm, p, &pushed, h->hit.obj, h->material, G, -1, e >= 0 ? e : inactive, B->x, B->y, B->w, B->h,
                               B->w > cw ? B->w : cw, B->h > ch ? B->h : ch, MOVE_TEXELS, &x, &y) == 0;     // (0: moved)
                if (ok) *B = (Snap){ x, y, B->w > cw ? B->w : cw, B->h > ch ? B->h : ch, G, true };
            }
        } else {
            // a split: a new rectangle of the source's size (on a grid: the group's cells) with a copy of
            // its paint (texel for texel, the source's lower-left part)
            int x, y;
            const Snap *S = &snaps[src];
            int w = rho > 0.0f ? cw : S->w, hh = rho > 0.0f ? ch : S->h;
            ok = AddRect(dm, p, &pushed, h->hit.obj, h->material, G, -1, w, hh, -1, EDIT_PAINT_ADD, &x, &y) == 0;     // (0: added)
            for (int ty = 0; ok && ty < S->h && ty < hh; ty++)
                for (int tx = 0; tx < S->w && tx < w; tx++) {
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

#define FIT_SAME_CELLS 1        // Fit to grid moves paint texel for texel when the rectangle differs from
                                // its cells by at most this many texels along each axis (a box moved by
                                // a fraction of a texel covers one cell more or less); else it resamples

// Resize to density (docs/painting.md 5.3): the entry gets a rectangle of the size the density gives,
// its paint resampled into it (nearest with paint_filter nearest, else bilinear), the old one cleared.
// On a material with a paint grid it is Fit to grid (17.7): the rectangle becomes the cells (a box
// face group: its largest face's per axis; a model slot: 17.4 / 17.5), the paint moved texel for
// texel when the old rectangle was on the grid (its size within one texel of the cells: the box was
// moved since; the faces show the same texels where they still cover them), else resampled
// nearest (a rectangle made at a density before the grid; a model slot, whose paint domain spans the
// whole rectangle)
static DrawModeResult ActResize(DrawMode *dm, const MS_SurfaceHit *h)
{
    float rho = MS_PaintGridOf(scene, h->material);
    const char *what = rho > 0.0f ? "fit to grid" : "resize";
    int entry;
    if (!ActionEntry(dm, h, what, &entry, rho > 0.0f)) return DRAWMODE_NONE;
    MS_PaintEntry E = *MS_PaintEntryAt(scene, entry);
    bool model = HitIsModel(h);
    int file = model ? scene->models[h->hit.obj.index].file : -1;
    bool nearest;
    AtlasRelOf(h->material, file, h->slot, NULL, NULL, &nearest);
    int w, hh;
    MoveMode mode = nearest ? MOVE_NEAREST : MOVE_BILINEAR;
    if (model) {
        bool estimate;
        SlotFitSize(dm, h->hit.obj.index, h->slot, h->material, &w, &hh, &estimate);
        if (rho > 0.0f) mode = MOVE_NEAREST;
        if (rho > 0.0f && (w > DRAW_RECT_MAX || hh > DRAW_RECT_MAX)) {
            Sayf(dm, true, "%s: slot %d needs %dx%d texels of the grid (%g/m); the most is %d", what, h->slot, w, hh, (double)rho, DRAW_RECT_MAX);
            return DRAWMODE_NONE;
        }
    } else if (rho > 0.0f) {
        int big, cells;
        if (!BoxGroupCells(h->hit.obj.index, E.faces, rho, &w, &hh, &big, &cells)) {
            Sayf(dm, true, "%s: face %s covers %d texels of the grid (%g/m); the most is %d", what, FACE_NAMES[big], cells, (double)rho,
                 DRAW_RECT_MAX);
            return DRAWMODE_NONE;
        }
        // (texel for texel when its texels already have about the grid's size: a rectangle made on the
        // grid whose box moved or was resized since; else, made at a density before the grid, resampled)
        // (a ratio rule, "within a quarter", moved paint made at 16..26 texels/m before a 20/m grid
        // texel for texel: its layout distorted or texels dropped; the verification of 2026-11-21)
        mode = abs(E.w - w) <= FIT_SAME_CELLS && abs(E.h - hh) <= FIT_SAME_CELLS ? MOVE_TEXELS : MOVE_NEAREST;
    } else BoxRectSize(dm->density, h->hit.obj.index, E.faces, &w, &hh);
    if (w == E.w && hh == E.h) {
        if (rho > 0.0f) Sayf(dm, false, "fit to grid: the rectangle is %dx%d already, on the grid (%g texels/m)", w, hh, (double)rho);
        else Sayf(dm, false, "resize: the rectangle is %dx%d already at %g texels/m", w, hh, (double)dm->density);
        return DRAWMODE_NONE;
    }
    PaintStep *p = EditStepOn(dm, h->material, file, h->slot, what);
    if (!p) return DRAWMODE_NONE;
    bool pushed = false;
    int x = 0, y = 0;
    bool ok = MoveEntry(dm, p, &pushed, h->hit.obj, h->material, model ? 0 : E.faces, h->slot, entry, E.x, E.y, E.w, E.h, w, hh, mode, &x,
                        &y) == 0;       // (0: moved)
    if (!ok) Sayf(dm, true, "%s", MS_LastError());      // (not moved: why)
    if (!pushed) return DRAWMODE_NONE;      // (the step was released)
    static const char *const HOW[3] = { "moved texel for texel", "resampled nearest", "resampled" };
    const char *said = !ok ? NULL
                     : rho > 0.0f ? TextFormat("fit to grid: %dx%d -> %dx%d cells at %d %d (%s)", E.w, E.h, w, hh, x, y, HOW[mode])
                     : TextFormat("resized the rectangle %dx%d -> %dx%d at %d %d", E.w, E.h, w, hh, x, y);
    return EditStepEnd(dm, p, pushed, said);
}

// Clear paint: the entry goes (EDIT_PAINT_REMOVE), its rectangle becomes transparent
static DrawModeResult ActClear(DrawMode *dm, const MS_SurfaceHit *h)
{
    int entry;
    if (!ActionEntry(dm, h, "clear", &entry, true)) return DRAWMODE_NONE;
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
// entries); without an atlas only new ones: on a paint grid the session's switch for grids (new grid
// atlases are nearest unless it is off, 17.7), else the kept pixel look; no surface (a script before
// its first stroke): both
static DrawModeResult ActPixelLook(DrawMode *dm, const MS_SurfaceHit *h, bool on)
{
    bool model = HitIsModel(h), surface = HitIsBox(h) || model;
    float grid = surface ? MS_PaintGridOf(scene, h->material) : 0.0f;
    if (!surface || grid <= 0.0f) dm->pixelLook = on;
    if (!surface || grid > 0.0f) dm->gridFiltered = !on;
    int file = model ? scene->models[h->hit.obj.index].file : -1;
    const char *rel = surface ? AtlasRelOf(h->material, file, h->slot, NULL, NULL, NULL) : NULL;
    if (!rel) {
        if (grid > 0.0f)
            SayStatic(dm, false, on ? "pixel look on: a new atlas on a grid gets paint_filter nearest"
                                    : "pixel look off: new atlases on a grid are filtered (this session)");
        else SayStatic(dm, false, on ? "pixel look on: new atlases get paint_filter nearest" : "pixel look off: new atlases are filtered");
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
    if (action == DRAW_ACT_PIXEL_LOOK) {
        // (a toggle of what the panel shows: the target atlas's filter, or what a new one there gets)
        const DrawTarget *T = &dm->target;
        bool now = set && T->ok && T->geom.ok ? T->geom.nearest : dm->pixelLook;
        return ActPixelLook(dm, set ? h : &(MS_SurfaceHit){ 0 }, !now);
    }
    if (!set) {
        SayStatic(dm, true, "point at a painted box or model first (the panel acts on the surface the cursor rests on)");
        return DRAWMODE_NONE;
    }
    // (a refusal; an entry too small for its grid cells can still be fitted or cleared)
    bool small = dm->target.refusalCode == 10 && (action == DRAW_ACT_RESIZE || action == DRAW_ACT_CLEAR);
    if (!dm->target.ok && dm->target.refusalCode >= 2 && !small) { Sayf(dm, true, "%s", MsgText(&dm->target.refusal)); return DRAWMODE_NONE; }
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
    dm->fg = c;     // (the background colour stays: X swaps them, docs/painting.md 18.1)
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
        k.estimate = g->estimate;
        k.grid = g->grid;
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
    T->grid = T->geom.grid;
    // (an entry to fit: an active one, or one too small for its cells; an unpainted face has none)
    T->fitOk = T->grid > 0.0f && ((T->ok && h->paintEntry >= 0) || T->refusalCode == 10);
    DrawTextKey k = TextKeyOf(T->refusalCode, h, T->ok, &T->geom);
    k.density = dm->density;
    if (memcmp(&k, &T->textKey, sizeof(k)) == 0) return;
    T->textKey = k;
    MsgSetStatic(&T->into, "");
    MsgSetStatic(&T->texel, "");
    MsgSetStatic(&T->gridLine, "");
    T->intoSet = T->texelSet = T->gridLineSet = false;
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
    // The grid the paint follows (docs/painting.md 17.7), or the density it gets without one
    const DrawGeom *g = &T->geom;
    if (T->ok || T->refusalCode == 10 || T->refusalCode == 9) {
        if (g->grid > 0.0f && h->material >= 0)
            MsgSetf(&T->gridLine, "follows the grid of %.120s: %g texels/m (%.1f cm)", scene->materials[h->material].name, (double)g->grid,
                    100.0 / g->grid);
        else if (g->grid > 0.0f) MsgSetf(&T->gridLine, "follows the map's grid: %g texels/m (%.1f cm)", (double)g->grid, 100.0 / g->grid);
        else if (h->material >= 0)
            MsgSetf(&T->gridLine, "%.120s has no paint grid: %g texels/m from the panel", scene->materials[h->material].name,
                    (double)dm->density);
        else MsgSetf(&T->gridLine, "the map has no paint grid: %g texels/m from the panel", (double)dm->density);
        T->gridLineSet = true;
    }
    if (!T->ok) return;
    // (the atlas's name: "<material>'s atlas" / "the own atlas of <file> slot N", heap: it holds names)
    char *atlas = h->material >= 0 ? MonoStrf("%s's atlas", scene->materials[h->material].name)
                : MonoStrf("the own atlas of %s slot %d", GetFileName(scene->modelFiles[scene->models[h->hit.obj.index].file].path), h->slot);
    const char *an = atlas ? atlas : "out of memory";
    // (a box on a grid: its rectangle in cells of the grid, 17.7)
    bool cells = HitIsBox(h) && g->grid > 0.0f;
    char gridTag[48] = "";
    if (g->grid > 0.0f) snprintf(gridTag, sizeof(gridTag), " (%g/m)", (double)g->grid);
    if (!g->fresh && cells && (g->cellsW != g->w || g->cellsH != g->h))
        // (the face shows part of its rectangle: a face group with a larger face, or a box that shrank)
        MsgSetf(&T->into, "%s %d×%d: %d×%d at %d %d%s, face %s covers %d×%d cells of it", an, g->atlasW, g->atlasH, g->w, g->h, g->x, g->y,
                gridTag, FACE_NAMES[h->face], g->cellsW, g->cellsH);
    else if (!g->fresh) MsgSetf(&T->into, "%s %d×%d: %d×%d%s at %d %d%s", an, g->atlasW, g->atlasH, g->w, g->h, cells ? " cells" : "", g->x, g->y, gridTag);
    else if (g->atlasW > 0)
        MsgSetf(&T->into, "the first stroke adds %s%d×%d%s%s to %.120s (%d×%d)", g->estimate ? "~" : "", g->w, g->h, cells ? " cells" : "",
                gridTag, an, g->atlasW, g->atlasH);
    else if (cells) MsgSetf(&T->into, "the first stroke makes %.120s with %d×%d cells%s", an, g->w, g->h, gridTag);
    else MsgSetf(&T->into, "the first stroke makes %.120s with a %s%d×%d rectangle%s", an, g->estimate ? "~" : "", g->w, g->h, gridTag);
    free(atlas);
    T->intoSet = true;
    if (g->needsUnwrap)
        MsgAppendf(&T->into, "; %s is unwrapped for paint first", GetFileName(scene->modelFiles[scene->models[h->hit.obj.index].file].path));
    // The size of a texel here (mm along the face's two edges; a model: about)
    const char *will = g->fresh ? "will be" : "is";
    if (HitIsBox(h)) {
        float su, sv;
        FaceDims(h->hit.obj.index, h->face, &su, &sv);
        if (g->grid > 0.0f) {
            MsgSetf(&T->texel, "face %s: a texel %s %.1f mm, on the grid (%d×%d cells)", FACE_NAMES[h->face], will, 1000.0 / g->grid,
                    g->cellsW, g->cellsH);
            T->texelSet = true;
        } else if (g->w > 0 && g->h > 0) {
            MsgSetf(&T->texel, "face %s: a texel %s %.1f × %.1f mm%s", FACE_NAMES[h->face], will, su * 1000.0f / g->w,
                    sv * 1000.0f / g->h, g->w >= DRAW_RECT_MAX || g->h >= DRAW_RECT_MAX ? " (the rectangle is clamped to 2048)" : "");
            T->texelSet = true;
        }
    } else if (g->w > 0) {
        // (the slot's world area over the texels its paint UVs cover; before its unwrap: the grid's or
        // the density's texel, which the unwrap is made at)
        double A, a;
        SlotAreas(h->hit.obj.index, h->slot, &A, &a);
        if (A > 0.0 && a > 0.0) {
            double mm = 1000.0 * sqrt(A / (a * g->w * g->h));
            MsgSetf(&T->texel, "slot %d: a texel %s about %.1f mm (%d×%d over %.2f m²)", h->slot, will, mm, g->w, g->h, A);
            const DrawUnwrap *u = SessionUnwrap(dm, scene->models[h->hit.obj.index].file);
            float s = InstanceScale(h->hit.obj.index);
            if (u && fabsf(u->scale - s) > 0.01f * s) MsgAppendf(&T->texel, ": the unwrap was made at scale %g", (double)u->scale);
            T->texelSet = true;
        } else if (A > 0.0) {
            MsgSetf(&T->texel, "slot %d: a texel %s about %.1f mm", h->slot, will, 1000.0 / RhoOf(dm, h->material));
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

// Leaving: a shape in progress and the selection are dropped (docs/painting.md 18.5)
void DrawModeClose(DrawMode *dm)
{
    ShapeCancel(dm);
    SelectionDrop(dm, false);
    dm->sel.lastSet = false;
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
    SelectionValidate(dm);  // (the draw mode's selection goes with its region)
    free(ids);
    Sayf(dm, r == 1, "%s", text ? text : "out of memory");      // (1: not done; 2: done, the auto save failed)
    free(text);
    return r != 1 ? DRAWMODE_RELOADED : DRAWMODE_NONE;
}

// ESC with nothing in progress: leaves the mode for spectator mode; with unsaved edits the first ESC
// only says so (again within WARN_TIME leaves)
static bool Leave(DrawMode *dm, EditMode *em)
{
    int unsaved = MapEditUnsaved(&em->ed);
    if (unsaved > 0 && dm->leaveWarnTime <= 0.0f) {
        dm->leaveWarnTime = WARN_TIME;
        Sayf(dm, true, "%d unsaved edit%s: CTRL+S saves, ESC again leaves them unsaved (still live)", unsaved, unsaved == 1 ? "" : "s");
        return false;
    }
    ShapeCancel(dm);
    if (dm->stroking) StrokeEnd(dm, em);
    DrawModeClose(dm);
    return true;
}

// A tool key (docs/painting.md 18.3, the table in drawkeys.c): the tool, U / SHIFT+U cycling to the
// filled shape and back, I on the eyedropper toggling its mode. A shape in progress is dropped
static void ToolKeys(DrawMode *dm, bool shift)
{
    for (int i = 0; i < DrawToolKeyCount(); i++) {
        const DrawToolKey *k = DrawToolKeyAt(i);
        if (k->shift != shift || !IsKeyPressed(k->key)) continue;
        if (k->tool == TOOL_EYEDROPPER && dm->tool == TOOL_EYEDROPPER) {
            dm->pickTool = dm->pickTool == DRAW_PICK_SCREEN ? DRAW_PICK_PAINT : DRAW_PICK_SCREEN;
            Sayf(dm, false, "eyedropper: %s", dm->pickTool == DRAW_PICK_SCREEN ? "the screen" : "the paint");
            return;
        }
        int t = DrawToolForKey(k->key, shift, dm->tool);
        if (t < 0) return;
        if ((DrawTool)t != dm->tool) ShapeCancel(dm);
        dm->tool = (DrawTool)t;
        Sayf(dm, false, "tool: %s", DrawToolName(dm->tool));
        return;
    }
}

static void SizeStep(DrawMode *dm, int dir)
{
    int s = dm->size;
    if (dir > 0) s = s < 8 ? s + 1 : (int)ceilf(s * 1.25f);
    else s = s <= 8 ? s - 1 : (int)floorf(s / 1.25f);
    dm->size = ClampI(s, 1, DRAW_SIZE_MAX);
}

// SHIFT+click with the pencil or the eraser: a straight line from where the tool painted last, when
// that is on the probe's region of the same atlas (Aseprite's); false: no such point
static bool ShiftLine(DrawMode *dm, const Probe *pr)
{
    PaintStep *p = dm->stroke;
    if (!p || !pr->ok || !dm->lastPaintSet || dm->lastPaintAtlas != p->atlas || !SameRegion(dm->lastPaintRegion, p->regions[pr->s.region]))
        return false;
    Sample a = { dm->lastPaintTx, dm->lastPaintTy, pr->s.region, false }, b = pr->s;
    b.joined = true;
    Sample *slot = MS_PUSH(p->samples, p->sampleCount, p->sampleCap);
    if (slot) *slot = a;
    slot = MS_PUSH(p->samples, p->sampleCount, p->sampleCap);
    if (slot) *slot = b;
    if (p->pixelPerfect) dm->ppDirty = true;
    else {
        Atlas *at = &dm->atlases[p->atlas];
        Stamp(dm, at, p, a, NULL);
        Stamp(dm, at, p, b, &a);
    }
    return true;
}

// A press of a brush tool (or the whole-region bucket) on hit h at px: the stroke begins with a stamp
// there (its first one on a surface without a rectangle allocates one; the spray's first tick; SHIFT
// with the pencil or the eraser: the line from its last point). True when the text changed live
static bool PressStroke(DrawMode *dm, EditMode *em, const MS_SurfaceHit *h, Vector2 px, bool rmb, bool shiftLine)
{
    unsigned int version = scene->version;
    if (!StrokeBegin(dm, em, h)) return false;
    dm->strokeRmb = rmb;
    dm->sprayTime = 0.0f;
    StepTool(dm, dm->stroke, dm->tool, rmb);
    Probe p = ProbeHit(dm, h, px);
    bool line = shiftLine && (dm->tool == TOOL_PENCIL || dm->tool == TOOL_ERASER) && ShiftLine(dm, &p);
    if (p.ok && !line) {
        if (dm->tool == TOOL_SPRAY) SprayTick(dm, &p);
        else StampProbe(dm, &p);
    }
    dm->lastPx = px;
    dm->lastOk = p.ok;
    dm->lastTx = p.s.tx;
    dm->lastTy = p.s.ty;
    dm->lastRegion = p.s.region;
    return scene->version != version;
}

// The eyedropper (the tool, or ALT+click) at px: paint mode picks now (docs/painting.md 7.4), screen
// mode asks main.c for the pixel of the scene's image (18.8). RMB: into the background colour
static void Pick(DrawMode *dm, Camera3D cam, int w, int h, Vector2 px, bool rmb, DrawPickMode mode)
{
    if (mode == DRAW_PICK_SCREEN) {
        dm->screenWant = true;
        dm->screenX = (int)px.x;
        dm->screenY = (int)px.y;
        dm->screenToFg = !rmb;
        return;
    }
    Color c;
    bool paint;
    if (!Eyedropper(dm, &dm->hover, cam, w, h, px, &c, &paint)) return;
    if (rmb) {
        dm->bg = c;
        AddRecent(dm, c);
    } else SetColor(dm, c);
    Sayf(dm, false, "picked #%02X%02X%02X a%d into %s (%s)", c.r, c.g, c.b, c.a, rmb ? "bg" : "fg", paint ? "the paint" : "the surface's albedo");
}

// The selection operation of a selection tool's press: SHIFT add, ALT subtract, both intersect
static DtSelOp SelOpOf(bool shift, bool alt)
{
    return shift && alt ? DT_SEL_AND : shift ? DT_SEL_ADD : alt ? DT_SEL_SUB : DT_SEL_REPLACE;
}

// A press (LMB, or RMB: the background colour) off the panel with the current tool (docs/painting.md
// 18.2, 18.3). True when the text changed live
static bool Press(DrawMode *dm, EditMode *em, Camera3D cam, int w, int h, Vector2 px, bool rmb, bool shift, bool alt)
{
    Settle(dm, 0.0f, true);     // (a click: the panel shows that surface at once)
    DrawTool t = dm->tool;
    DrawToolKind kind = DrawToolKindOf(t);
    if (alt && kind != TOOLKIND_SELECT) { Pick(dm, cam, w, h, px, rmb, dm->pickAlt); return false; }
    if (t == TOOL_EYEDROPPER) { Pick(dm, cam, w, h, px, rmb, dm->pickTool); return false; }
    if (!dm->hoverOk) {
        if (dm->refusalCode >= 2) Sayf(dm, true, "%s", MsgText(&dm->refusal));     // (a refusal)
        return false;
    }
    if (kind == TOOLKIND_SELECT) {
        if (rmb) return false;      // (the selection tools ignore RMB)
        if (t == TOOL_WAND) WandAt(dm, &dm->hover, SelOpOf(shift, alt));
        else if (ShapeBegin(dm, em, &dm->hover, t, false, SelOpOf(shift, alt))) ShapeRedraw(dm);
        return false;
    }
    if (!EditModeConfirm(em, STROKE_KEY)) return false;
    unsigned int version = scene->version;
    if (kind == TOOLKIND_BRUSH || (t == TOOL_BUCKET && dm->bucket == DRAW_BUCKET_REGION)) {
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
                dm->strokeRmb = rmb;
                Sayf(dm, false, "unwrapping %s for paint (a large model takes a few seconds) ...", GetFileName(scene->modelFiles[file].path));
            }
            free(row);
            return false;
        }
        PressStroke(dm, em, &dm->hover, px, rmb, shift);
    } else if (t == TOOL_MOVE && dm->hoverGeom.fresh) {
        SayStatic(dm, true, "nothing painted here to move");
    } else if (ShapeBegin(dm, em, &dm->hover, t, rmb, DT_SEL_REPLACE)) {
        if (t == TOOL_BUCKET) ShapeCommit(dm, em);     // (a click: one step)
        else ShapeRedraw(dm);
    }
    return scene->version != version;
}

// A shape in progress this frame (docs/painting.md 18.2, 18.4): its moving point follows the cursor
// (clamped to the region), a release ends a drag, clicks add a polygon's points or a curve's control
// points. Nothing moves while the camera has the mouse
static void ShapeInput(DrawMode *dm, EditMode *em, Camera3D cam, int w, int h, Vector2 px, bool nav, bool shift, bool lP, bool rP)
{
    DrawShape *S = &dm->shapeDrag;
    if (nav) return;
    bool pressed = S->rmb ? rP : lP;
    bool released = S->down && !IsMouseButtonDown(S->rmb ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_LEFT);
    if (released) S->down = false;
    DtPoint q;
    bool have = ShapeTexel(dm, GetScreenToWorldRayEx(px, cam, w, h), &q);
    DtPoint *P = S->pts.p;
    int n = S->pts.n;
    bool changed = false;
    switch (S->tool) {
    case TOOL_CONTOUR:
    case TOOL_LASSO:
        if (have && (P[n - 1].x != q.x || P[n - 1].y != q.y)) changed = DtPointsPush(&S->pts, q.x, q.y);
        if (released) { ShapeCommit(dm, em); return; }
        break;
    case TOOL_POLYGON:
    case TOOL_LASSO_POLY:
        if (have && (P[n - 1].x != q.x || P[n - 1].y != q.y)) {
            P[n - 1] = q;
            changed = true;
        }
        if (pressed) {
            float now = (float)GetTime();
            bool twice = now - S->clickTime < 0.4f && n >= 2 && P[n - 2].x == P[n - 1].x && P[n - 2].y == P[n - 1].y;
            bool first = n - 1 >= 3 && abs(P[n - 1].x - P[0].x) <= 1 && abs(P[n - 1].y - P[0].y) <= 1;
            S->clickTime = now;
            if ((twice && n - 1 >= 3) || first) {
                S->pts.n--;
                ShapeCommit(dm, em);
                return;
            }
            changed = DtPointsPush(&S->pts, P[n - 1].x, P[n - 1].y);
        }
        break;
    case TOOL_CURVE:
        if (S->stage == 0) {
            if (have) {
                if (shift) q = Constrain(S->tool, P[0], q);
                changed = P[1].x != q.x || P[1].y != q.y;
                P[1] = q;
                P[2] = P[0];
                P[3] = P[1];
            }
            if (released) S->stage = 1;
        } else {
            DtPoint *c = &P[S->stage == 1 ? 2 : 3];
            if (have && (c->x != q.x || c->y != q.y)) {
                *c = q;
                if (S->stage == 1) P[3] = q;
                changed = true;
            }
            if (pressed) S->down = true;
            if (released) {
                if (S->stage == 2) { ShapeCommit(dm, em); return; }
                S->stage = 2;
            }
        }
        break;
    default:        // line, rectangles, ellipses, gradient, marquees, move: a drag a -> b
        if (have) {
            if (shift && (S->tool == TOOL_LINE || S->tool == TOOL_RECT || S->tool == TOOL_RECT_FILL || S->tool == TOOL_ELLIPSE ||
                          S->tool == TOOL_ELLIPSE_FILL))
                q = Constrain(S->tool, P[0], q);
            changed = P[1].x != q.x || P[1].y != q.y;
            P[1] = q;
        }
        if (released) { ShapeCommit(dm, em); return; }
        break;
    }
    if (changed && S->on) ShapeRedraw(dm);
}

DrawModeResult DrawModeUpdate(DrawMode *dm, EditMode *em, Designer *d, Camera3D cam, bool cursorOnPanel, float dt)
{
    if (dm->messageTime > 0.0f) dm->messageTime -= dt;
    if (dm->leaveWarnTime > 0.0f) dm->leaveWarnTime -= dt;
    if (em->messageTime > 0.0f) em->messageTime -= dt;      // (EditModeUpdate isn't called meanwhile)
    if (em->warnTime > 0.0f) em->warnTime -= dt;
    if (!dm->active) return DRAWMODE_NONE;
    int w = GetScreenWidth(), h = GetScreenHeight();
    // The camera's (spectator.c, mouseNav): nothing is painted while SPACE or MMB is down, and a brush
    // stroke going on ends when either goes down (or its button comes up); a shape waits
    bool nav = IsKeyDown(KEY_SPACE) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
    DrawModeResult res = DRAWMODE_NONE;
    if (dm->stroking && !dm->shapeDrag.on && (nav || !IsMouseButtonDown(dm->strokeRmb ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_LEFT)))
        StrokeEnd(dm, em);
    dm->onPanel = cursorOnPanel;
    if (!cursorOnPanel) {
        HoverAt(dm, cam, GetMousePosition(), w, h);
        Settle(dm, dt, false);
    } else HoverRefresh(dm);

    // The frame after the unwrap's note: the unwrap, then the stroke where the button went down
    if (dm->unwrapWant && !dm->stroking) {
        dm->unwrapWant = false;
        if (dm->unwrapVersion == scene->version && !nav) {
            if (PressStroke(dm, em, &dm->unwrapHit, dm->unwrapPx, dm->strokeRmb, false)) res = DRAWMODE_RELOADED;
            if (dm->stroking && !IsMouseButtonDown(dm->strokeRmb ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_LEFT)) StrokeEnd(dm, em);     // (a click: one dab)
            if (!cursorOnPanel) HoverAt(dm, cam, GetMousePosition(), w, h);
        } else SayStatic(dm, true, "the unwrap was dropped: the map changed (click again)");
    }

    bool ctrl = Ctrl(), shift = Shift(), alt = Alt();
    bool escUsed = false;
    // A shape in progress: ESC drops it, ENTER closes a polygon or ends a curve
    if (!dm->keysBlocked && dm->shapeDrag.on && !ctrl && !alt) {
        if (IsKeyPressed(KEY_ESCAPE)) {
            escUsed = ShapeCancel(dm);
            SayStatic(dm, false, "shape cancelled");
        } else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) ShapeEnter(dm, em);
    }
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
        // The selection (CTRL+A, CTRL+D, CTRL+SHIFT+D, CTRL+SHIFT+I)
        if (ctrl && !alt && !shift && IsKeyPressed(KEY_A)) SelectAll(dm);
        if (ctrl && !alt && IsKeyPressed(KEY_D)) {
            if (shift) Reselect(dm);
            else Deselect(dm);
        }
        if (ctrl && !alt && shift && IsKeyPressed(KEY_I)) SelectInvert(dm);
        if (!ctrl && !alt) {
            // ESC with nothing in progress: leave (B no longer leaves: it is the pencil)
            if (!escUsed && IsKeyPressed(KEY_ESCAPE) && Leave(dm, em)) return DRAWMODE_NONE;
            ToolKeys(dm, shift);
            for (int k = 0; k < PEN_COUNT; k++)
                if (IsKeyPressed(KEY_ONE + k)) {
                    ShapeCancel(dm);
                    DrawModeSetPen(dm, (PenKind)k);
                }
            if (IsKeyPressed(KEY_X)) {
                Color c = dm->fg;
                dm->fg = dm->bg;
                dm->bg = c;
            }
            if (IsKeyPressed(KEY_TAB)) dm->panelHidden = !dm->panelHidden;
        }
        if (IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressedRepeat(KEY_LEFT_BRACKET)) SizeStep(dm, -1);
        if (IsKeyPressed(KEY_RIGHT_BRACKET) || IsKeyPressedRepeat(KEY_RIGHT_BRACKET)) SizeStep(dm, 1);
    }
    float wheel = cursorOnPanel ? 0.0f : GetMouseWheelMove();     // (without CTRL / SHIFT: the camera's)
    if (wheel != 0.0f && ctrl) SizeStep(dm, wheel > 0.0f ? 1 : -1);
    else if (wheel != 0.0f && shift) dm->fg.a = (unsigned char)ClampI(dm->fg.a + (wheel > 0.0f ? 8 : -8), 0, 255);

    Vector2 px = GetMousePosition();
    bool lP = IsMouseButtonPressed(MOUSE_BUTTON_LEFT), rP = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
    unsigned int version = scene->version;
    if (dm->shapeDrag.on) ShapeInput(dm, em, cam, w, h, px, nav, shift, lP, rP);
    else if (!cursorOnPanel && !nav && (lP || rP) && !dm->stroking && !dm->unwrapWant) Press(dm, em, cam, w, h, px, !lP, shift, alt);
    else if (dm->stroking && dm->stroke && dm->stroke->tool == TOOL_SPRAY) {
        // (the spray: speed dots per 1/60 s at the cursor, moving or not; a slow frame sprays at most 8 ticks)
        dm->sprayTime = fminf(dm->sprayTime + dt * 60.0f, 8.0f);
        Probe b = ProbeHit(dm, &dm->hover, px);
        for (; dm->sprayTime >= 1.0f; dm->sprayTime -= 1.0f) SprayTick(dm, &b);
    } else if (dm->stroking && dm->stroke && dm->stroke->pen != PEN_FILL && (px.x != dm->lastPx.x || px.y != dm->lastPx.y)) {
        Probe a = { .px = dm->lastPx, .ok = dm->lastOk, .s = { dm->lastTx, dm->lastTy, dm->lastRegion, false } };
        Probe b = ProbeHit(dm, &dm->hover, px);
        StrokeSegment(dm, cam, w, h, a, b);
        dm->lastPx = px;
        dm->lastOk = b.ok;
        dm->lastTx = b.s.tx;
        dm->lastTy = b.s.ty;
        dm->lastRegion = b.s.region;
    }
    if (scene->version != version) res = DRAWMODE_RELOADED;
    StrokeFrame(dm);
    Flush(dm);
    TargetRefresh(dm);
    PrepareSelection(dm);       // (before the cursor's: both may build the slot triangles, the cursor's last)
    PrepareCursor(dm);
    return res;
}

// #endregion

// ======================================================================================
// #region Sync after a reload, HUD, the brush cursor
// ======================================================================================

void DrawModeBeforeSync(DrawMode *dm)
{
    // (a shape in progress is dropped, a stroke going on ends first: the history the rebase rewrites
    // may hold its step)
    ShapeCancel(dm);
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
    SelectionValidate(dm);
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
        char grid[40] = "";     // (the paint grid, docs/painting.md 17.7)
        if (g->grid > 0.0f) snprintf(grid, sizeof(grid), ", grid %g/m", (double)g->grid);
        if (!g->fresh) snprintf(target, sizeof(target), "  |  %.120s %d×%d, %.100s%s", atlas, g->atlasW, g->atlasH, what, grid);
        else snprintf(target, sizeof(target), "  |  %.60s: a new %s%d×%d%s in %.120s%s%s", what, g->estimate ? "~" : "", g->w, g->h,
                      HitIsBox(h) && g->grid > 0.0f ? " cells" : "", atlas, grid, g->needsUnwrap ? " (unwrap first)" : "");
    }
    char unsaved[64] = "";
    int n = EditModeUnsaved(em);
    if (n > 0) snprintf(unsaved, sizeof(unsaved), "  |  %d unsaved edit%s", n, n == 1 ? "" : "s");
    Color c = dm->fg;
    // The mode name as a mark in the draw mode's colour, the rest in HUD text (their backdrop
    // strips abut: UiHudText's starts 6 px left of its text)
    int rx = x + UiHudMark("DRAW", x, y, UI_HUD_LINE, UI_S, UI_MODE_DRAW);
    // The tool (the pencil with its brush, the brush tools and outlines with their size), the colour
    // and the selection's extent
    DrawTool t = dm->tool;
    bool sized = DrawToolKindOf(t) == TOOLKIND_BRUSH || t == TOOL_LINE || t == TOOL_CURVE || t == TOOL_RECT || t == TOOL_ELLIPSE;
    char tool[96], sel[48] = "";
    snprintf(tool, sizeof(tool), "%s%s%s", DrawToolName(t), t == TOOL_PENCIL ? TextFormat(" (%s)", PEN_NAMES[DrawModeStampPen(dm)]) : "",
             t == TOOL_SPRAY ? TextFormat(" %d px, width %d", dm->size, dm->sprayWidth) : sized ? TextFormat(" %d px", dm->size) : "");
    int x0, y0, x1, y1;
    if (dm->sel.active && DtMaskBounds(&dm->sel.mask, &x0, &y0, &x1, &y1)) snprintf(sel, sizeof(sel), "  |  selection %d×%d", x1 - x0 + 1, y1 - y0 + 1);
    UiHudText(TextFormat("%s  #%02X%02X%02X a%d%s%s%s", tool, c.r, c.g, c.b, c.a, sel, target, unsaved), rx, y, UI_HUD_LINE, UI_S, UI_TEXT);
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
    if (S && DrawModeStampPen(dm) == PEN_FILL && k >= 0 && k < S->n) SlotOutline(dm, S->root[k]);
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
// coordinates 0..1 (boxes; on a grid through the face's cells, docs/painting.md 17.2, so texel edges
// land on the surface's texel lines) or the slot's paint UV (models)
static Vector2 TexelToSurface(const DrawGeom *g, Vector2 p, float tx, float ty)
{
    float du = g->u1 - g->u0, dv = g->v1 - g->v0;
    return (Vector2){ g->uv.x + (p.x - tx) / (du > 0.0f ? du : 1.0f), g->uv.y + (p.y - ty) / (dv > 0.0f ? dv : 1.0f) };
}

// A point of a slot triangle (its paint uv corners, model-space corners) at paint coordinate t
static Vector3 TriPoint(const Vector2 *uv, const Vector3 *pos, Vector2 t)
{
    float den = (uv[1].y - uv[2].y) * (uv[0].x - uv[2].x) + (uv[2].x - uv[1].x) * (uv[0].y - uv[2].y);
    float a = ((uv[1].y - uv[2].y) * (t.x - uv[2].x) + (uv[2].x - uv[1].x) * (t.y - uv[2].y)) / den;
    float b = ((uv[2].y - uv[0].y) * (t.x - uv[2].x) + (uv[0].x - uv[2].x) * (t.y - uv[2].y)) / den;
    return Vector3Add(Vector3Add(Vector3Scale(pos[0], a), Vector3Scale(pos[1], b)), Vector3Scale(pos[2], 1.0f - a - b));
}

static void LinesPush(DrawLines *L, Vector3 a, Vector3 b)
{
    if (!MS_RESERVE(L->p, L->cap, L->n + 2)) return;
    L->p[L->n++] = a;
    L->p[L->n++] = b;
}

// One border segment a-b (atlas texel corners) of a mask on hit h's surface into L: a box face through
// its entry's mapping (on a grid its cells), cut at the face's edges; a model slot through every slot
// triangle it crosses (each triangle's affine map), cut at the triangle's edges
static void OutlineSegment(DrawMode *dm, const MS_SurfaceHit *h, const MS_PaintEntry *E, const FaceCells *fc, Vector2 a, Vector2 b,
                           DrawLines *L)
{
    if (HitIsBox(h)) {
        static const Vector2 face[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        Vector2 ua, ub;
        if (fc) {
            ua = (Vector2){ (a.x - E->x - fc->u0) / (fc->u1 - fc->u0), (a.y - E->y - fc->v0) / (fc->v1 - fc->v0) };
            ub = (Vector2){ (b.x - E->x - fc->u0) / (fc->u1 - fc->u0), (b.y - E->y - fc->v0) / (fc->v1 - fc->v0) };
        } else {
            ua = (Vector2){ (a.x - E->x) / E->w, (a.y - E->y) / E->h };
            ub = (Vector2){ (b.x - E->x) / E->w, (b.y - E->y) / E->h };
        }
        if (ClipSegment(&ua, &ub, face, 4))
            LinesPush(L, FacePoint(h->hit.obj.index, h->face, ua), FacePoint(h->hit.obj.index, h->face, ub));
        return;
    }
    const MS_ModelInstance *M = &scene->models[h->hit.obj.index];
    const DrawSlotTris *S = SlotTrisFor(dm, M->file, h->slot);
    if (!S) return;
    Vector2 ua = { (a.x - E->x) / E->w, (a.y - E->y) / E->h }, ub = { (b.x - E->x) / E->w, (b.y - E->y) / E->h };
    Vector2 lo = { fminf(ua.x, ub.x), fminf(ua.y, ub.y) }, hi = { fmaxf(ua.x, ub.x), fmaxf(ua.y, ub.y) };
    for (int t = 0; t < S->n; t++) {
        const Vector2 *uvs = &S->uv[3 * t];
        if (fmaxf(uvs[0].x, fmaxf(uvs[1].x, uvs[2].x)) < lo.x || fminf(uvs[0].x, fminf(uvs[1].x, uvs[2].x)) > hi.x ||
            fmaxf(uvs[0].y, fmaxf(uvs[1].y, uvs[2].y)) < lo.y || fminf(uvs[0].y, fminf(uvs[1].y, uvs[2].y)) > hi.y)
            continue;
        Vector2 p = ua, q = ub;
        if (!ClipSegment(&p, &q, uvs, 3)) continue;
        LinesPush(L, Vector3Transform(TriPoint(uvs, &S->pos[3 * t], p), M->transform),
                  Vector3Transform(TriPoint(uvs, &S->pos[3 * t], q), M->transform));
    }
}

// The border of mask m (between its texels > 0 and the others; runs merged) as lines on hit h's
// surface (docs/painting.md 18.5): built when the selection or a selection shape changed, drawn by
// DrawModeDrawCursor
static void OutlineBuild(DrawMode *dm, const DtMask *m, const MS_SurfaceHit *h, DrawLines *out)
{
    out->n = 0;
    if (!m->a || (!HitIsBox(h) && !HitIsModel(h))) return;
    int e = EntryFor(h->hit.obj, h->material, h->face, h->slot, NULL);
    if (e < 0) return;
    const MS_PaintEntry *E = MS_PaintEntryAt(scene, e);
    FaceCells fc;
    float rho = MS_PaintGridOf(scene, h->material);
    bool grid = HitIsBox(h) && rho > 0.0f && FaceGridCells(h->hit.obj.index, h->face, rho, &fc) && fc.u1 > fc.u0 && fc.v1 > fc.v0;
    if (E->w <= 0 || E->h <= 0) return;
#define IN(x, y) ((x) >= 0 && (y) >= 0 && (x) < m->w && (y) < m->h && m->a[(size_t)(y) * m->w + (x)])
    for (int y = 0; y <= m->h; y++)         // the row boundaries: horizontal runs
        for (int x = 0, start = -1; x <= m->w; x++) {
            bool edge = x < m->w && IN(x, y - 1) != IN(x, y);
            if (edge && start < 0) start = x;
            if (!edge && start >= 0) {
                OutlineSegment(dm, h, E, grid ? &fc : NULL, (Vector2){ (float)(m->x + start), (float)(m->y + y) },
                               (Vector2){ (float)(m->x + x), (float)(m->y + y) }, out);
                start = -1;
            }
        }
    for (int x = 0; x <= m->w; x++)         // the column boundaries: vertical runs
        for (int y = 0, start = -1; y <= m->h; y++) {
            bool edge = y < m->h && IN(x - 1, y) != IN(x, y);
            if (edge && start < 0) start = y;
            if (!edge && start >= 0) {
                OutlineSegment(dm, h, E, grid ? &fc : NULL, (Vector2){ (float)(m->x + x), (float)(m->y + start) },
                               (Vector2){ (float)(m->x + x), (float)(m->y + y) }, out);
                start = -1;
            }
        }
#undef IN
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
            FaceCells c;
            if (g->grid > 0.0f && FaceGridCells(h->hit.obj.index, f, g->grid, &c) && c.u1 > c.u0 && c.v1 > c.v0) {
                // (on a grid the fill covers the hit face's cells: on each face of the entry the part of
                // them it shows, through that face's own cells)
                Vector2 r[4];
                for (int i = 0; i < 4; i++)
                    r[i] = (Vector2){ (q[i].x * g->cellsW - c.u0) / (c.u1 - c.u0), (q[i].y * g->cellsH - c.v0) / (c.v1 - c.v0) };
                for (int i = 0; i < 4; i++) {
                    Vector2 a = r[i], b = r[(i + 1) % 4];
                    if (ClipSegment(&a, &b, q, 4)) CursorLine(FacePoint(h->hit.obj.index, f, a), FacePoint(h->hit.obj.index, f, b), col);
                }
                continue;
            }
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

static void DrawLinesOn(const DrawLines *L, Color col)
{
    for (int i = 0; i + 1 < L->n; i += 2) CursorLine(L->p[i], L->p[i + 1], col);
}

// The selection's outline and a selection shape's preview (UI_MODE_DRAW), then the tool's cursor on
// the surface under it: the brush footprint (the brush tools and the outlines the brush draws), the
// spray's disc, the whole-region fill's outline, else a texel marker with a cross
void DrawModeDrawCursor(const DrawMode *dm, Camera3D cam)
{
    const MS_SurfaceHit *h = &dm->hover;
    const DrawGeom *g = &dm->hoverGeom;
    bool selLines = dm->active && dm->sel.active && dm->sel.outlineSet && dm->sel.outlineVersion == scene->version && dm->sel.outline.n > 0;
    bool shapeLines = dm->active && dm->shapeDrag.on && dm->shapeDrag.outline.n > 0;
    bool cursor = dm->active && dm->hoverOk && !dm->onPanel && g->ok && dm->hoverVersion == scene->version && (HitIsBox(h) || HitIsModel(h)) &&
                  g->uv.x >= 0.0f && g->w > 0 && g->h > 0;     // (a model before its unwrap: no paint coordinates yet)
    if (!selLines && !shapeLines && !cursor) return;
    DesignerBeginOverlay3D(cam);
    cursorEye = cam.position;
    if (selLines) DrawLinesOn(&dm->sel.outline, UI_MODE_DRAW);
    if (shapeLines) DrawLinesOn(&dm->shapeDrag.outline, Fade(UI_MODE_DRAW, 0.8f));
    if (!cursor) {
        DesignerEndOverlay3D();
        return;
    }
    Color col = Fade(CURSOR_COLOR, 0.95f);
    PenKind pen = DrawModeStampPen(dm);
    DrawTool t = dm->tool;
    // The tool's footprint in the rectangle's texels about the hit (docs/painting.md 7.2; on a grid the
    // face's cells, 17.2)
    float tx = g->u0 + Clamp01(g->uv.x) * (g->u1 - g->u0), ty = g->v0 + Clamp01(g->uv.y) * (g->v1 - g->v0);
    bool brush = t == TOOL_PENCIL || t == TOOL_ERASER || t == TOOL_BLUR || t == TOOL_JUMBLE || t == TOOL_LINE || t == TOOL_CURVE ||
                 t == TOOL_RECT || t == TOOL_ELLIPSE;
    if (pen == PEN_FILL) CursorFill(dm, col);
    else if (t == TOOL_SPRAY) CursorRing(dm, tx, ty, dm->sprayWidth * 0.5f, tx, ty, col, true);
    else if (brush) {
        int s = dm->size < 1 ? 1 : dm->size;
        if (pen == PEN_SQUARE) CursorQuad(dm, floorf(tx) - (float)(s / 2), floorf(ty) - (float)(s / 2), (float)s, tx, ty, col);
        else if (s == 1) CursorQuad(dm, floorf(tx), floorf(ty), 1.0f, tx, ty, col);
        else if (s == 2) CursorQuad(dm, floorf(tx - 0.5f), floorf(ty - 0.5f), 2.0f, tx, ty, col);
        else {
            // (a ring: the texels whose centre is inside it are painted; the eraser's is dashed, the
            // soft pen's solid core a fainter ring inside)
            CursorRing(dm, tx, ty, s * 0.5f, tx, ty, col, pen == PEN_ERASER);
            if (pen == PEN_SOFT && dm->hardness > 0.05f && dm->hardness < 0.95f)
                CursorRing(dm, tx, ty, s * 0.5f * dm->hardness, tx, ty, Fade(CURSOR_COLOR, 0.45f), false);
        }
    } else {
        // (the click, shape, selection and move tools: the texel under the cursor and a cross 2 texels out)
        float cx = floorf(tx), cy = floorf(ty);
        CursorQuad(dm, cx, cy, 1.0f, tx, ty, col);
        Vector2 a[2] = { { cx - 2.0f, cy + 0.5f }, { cx, cy + 0.5f } }, b[2] = { { cx + 1.0f, cy + 0.5f }, { cx + 3.0f, cy + 0.5f } };
        Vector2 c[2] = { { cx + 0.5f, cy - 2.0f }, { cx + 0.5f, cy } }, d[2] = { { cx + 0.5f, cy + 1.0f }, { cx + 0.5f, cy + 3.0f } };
        CursorPolyline(dm, a, 2, tx, ty, col, false);
        CursorPolyline(dm, b, 2, tx, ty, col, false);
        CursorPolyline(dm, c, 2, tx, ty, col, false);
        CursorPolyline(dm, d, 2, tx, ty, col, false);
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
    dm->tool = TOOL_PENCIL;
    dm->shape = PEN_ROUND;
    dm->size = 4;
    dm->hardness = 0.5f;
    dm->fg = (Color){ 200, 50, 50, 255 };
    dm->bg = (Color){ 255, 255, 255, 255 };
    dm->density = DRAW_DENSITY_DEFAULT;
    // (the new tools' options: Aseprite's defaults, docs/painting.md 18.6; drawsettings.c's
    // DrawStateDefaults holds the same values)
    dm->sprayWidth = 16;
    dm->spraySpeed = 32;
    dm->bucket = DRAW_BUCKET_CONTIGUOUS;
    dm->tolerance = 0;
    dm->pickAlt = DRAW_PICK_PAINT;
    dm->pickTool = DRAW_PICK_SCREEN;
    dm->sel.atlas = dm->sel.lastAtlas = dm->shapeDrag.atlas = -1;
    dm->em = em;
    dm->strokeAtlas = -1;
    em->ed.paintHooks = (MapEditPaintHooks){ dm, HookUndo, HookRedo, HookRebase, HookRelease, HookBytes, HookSave };
}

void DrawModeFree(DrawMode *dm)
{
    if (dm->stroke && !dm->strokePushed) ReleaseStep(dm, dm->stroke);
    for (int i = 0; i < dm->atlasCount; i++) FreeAtlas(&dm->atlases[i]);
    free(dm->atlases);
    for (int i = 0; i < dm->unwrapCount; i++) FreeUnwrapOf(&dm->unwraps[i]);
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
    MsgFree(&dm->target.gridLine);
    free(dm->palette);
    free(dm->sel.mask.a);
    free(dm->sel.regionMask.a);
    free(dm->sel.last.a);
    free(dm->shapeDrag.pts.p);
    free(dm->shapeDrag.preview.a);
    free(dm->shapeDrag.outline.p);
    free(dm->sel.outline.p);
    memset(dm, 0, sizeof(*dm));
}

void DrawModeSetPen(DrawMode *dm, PenKind pen)
{
    if (pen == PEN_ERASER) dm->tool = TOOL_ERASER;
    else if (pen == PEN_FILL) {
        dm->tool = TOOL_BUCKET;
        dm->bucket = DRAW_BUCKET_REGION;
    } else if (pen >= PEN_ROUND && pen <= PEN_SOFT) {
        dm->tool = TOOL_PENCIL;
        dm->shape = pen;
    }
}

PenKind DrawModeStampPen(const DrawMode *dm)
{
    if (dm->tool == TOOL_ERASER) return PEN_ERASER;
    if (dm->tool == TOOL_BUCKET && dm->bucket == DRAW_BUCKET_REGION) return PEN_FILL;
    return dm->shape >= PEN_ROUND && dm->shape <= PEN_SOFT ? dm->shape : PEN_ROUND;
}

// (the kept fields of DrawState and DrawMode, one list: Get, Set and Differs follow it)
#define KEPT_SCALARS(X) \
    X(size) X(hardness) X(pixelPerfect) X(fg) X(bg) X(recentCount) X(sprayWidth) X(spraySpeed) X(bucket) \
    X(tolerance) X(gradientRadial) X(pickAlt) X(pickTool) X(density) X(pixelLook) X(panelHidden)

bool DrawModeGetState(const DrawMode *dm, DrawState *s)
{
    s->tool = (int)dm->tool;
    s->shape = (int)dm->shape;
#define GET(f) s->f = dm->f;
    KEPT_SCALARS(GET)
#undef GET
    memcpy(s->recent, dm->recent, sizeof(s->recent));
    s->paletteCount = 0;
    if (dm->paletteCount > 0 && !MS_RESERVE(s->palette, s->paletteCap, dm->paletteCount)) return false;
    if (dm->paletteCount > 0) memcpy(s->palette, dm->palette, sizeof(Color) * (size_t)dm->paletteCount);
    s->paletteCount = dm->paletteCount;
    return true;
}

bool DrawModeSetState(DrawMode *dm, const DrawState *s)
{
    dm->tool = s->tool >= 0 && s->tool < TOOL_COUNT ? (DrawTool)s->tool : TOOL_PENCIL;
    dm->shape = s->shape >= PEN_ROUND && s->shape <= PEN_SOFT ? (PenKind)s->shape : PEN_ROUND;
#define SET(f) dm->f = s->f;
    KEPT_SCALARS(SET)
#undef SET
    dm->size = ClampI(dm->size, 1, DRAW_SIZE_MAX);
    dm->hardness = Clamp01(dm->hardness);
    dm->recentCount = ClampI(dm->recentCount, 0, DRAW_RECENT_COLORS);
    memcpy(dm->recent, s->recent, sizeof(dm->recent));
    dm->sprayWidth = ClampI(dm->sprayWidth, 1, 256);
    dm->spraySpeed = ClampI(dm->spraySpeed, 1, 256);
    if ((int)dm->bucket < DRAW_BUCKET_CONTIGUOUS || dm->bucket > DRAW_BUCKET_REGION) dm->bucket = DRAW_BUCKET_CONTIGUOUS;
    dm->tolerance = ClampI(dm->tolerance, 0, 255);
    if (dm->pickAlt != DRAW_PICK_SCREEN) dm->pickAlt = DRAW_PICK_PAINT;
    if (dm->pickTool != DRAW_PICK_PAINT) dm->pickTool = DRAW_PICK_SCREEN;
    if (!(dm->density >= 8.0f && dm->density <= 512.0f)) dm->density = DRAW_DENSITY_DEFAULT;
    int n = s->paletteCount > 0 ? s->paletteCount : 0;
    if (n > 0 && !MS_RESERVE(dm->palette, dm->paletteCap, n)) return false;
    if (n > 0) memcpy(dm->palette, s->palette, sizeof(Color) * (size_t)n);
    dm->paletteCount = n;
    return true;
}

bool DrawModeStateDiffers(const DrawMode *dm, const DrawState *s)
{
    if (s->tool != (int)dm->tool || s->shape != (int)dm->shape) return true;
#define DIFF(f) if (memcmp(&s->f, &dm->f, sizeof(s->f)) != 0) return true;
    KEPT_SCALARS(DIFF)
#undef DIFF
    if (memcmp(s->recent, dm->recent, sizeof(Color) * (size_t)ClampI(dm->recentCount, 0, DRAW_RECENT_COLORS)) != 0) return true;
    if (s->paletteCount != dm->paletteCount) return true;
    return dm->paletteCount > 0 && memcmp(s->palette, dm->palette, sizeof(Color) * (size_t)dm->paletteCount) != 0;
}

bool DrawModeScreenWanted(const DrawMode *dm, int *x, int *y)
{
    if (!dm->active || !dm->screenWant) return false;
    *x = dm->screenX;
    *y = dm->screenY;
    return true;
}

// (phase 5 says what it picked, as the paint eyedropper does)
void DrawModeScreenPicked(DrawMode *dm, bool ok, Color c)
{
    dm->screenWant = false;
    if (!ok) { SayStatic(dm, true, "the screen pixel could not be read"); return; }
    c.a = 255;
    if (dm->screenToFg) SetColor(dm, c);
    else dm->bg = c;
    Sayf(dm, false, "picked #%02X%02X%02X (the screen)", c.r, c.g, c.b);
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
        // (on a paint grid: the hit's texel from the face's first cell, as MS_RaycastSurface gives it)
        h->paintGrid = MS_PaintGridOf(scene, h->material);
        FaceCells fc;
        if (h->paintGrid > 0.0f && FaceGridCells(obj.index, h->face, h->paintGrid, &fc)) {
            h->paintTexel = (Vector2){ fc.u0 + Clamp01(u) * (fc.u1 - fc.u0), fc.v0 + Clamp01(v) * (fc.v1 - fc.v0) };
            h->paintCellsW = fc.w;
            h->paintCellsH = fc.h;
        }
        h->paintEntry = EntryFor(obj, h->material, h->face, -1, NULL);
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
    DrawGeom g;
    Resolve(dm, &ha, &g);       // (the surface's texels: its rectangle, a box face on a grid its cells)
    float du = (b.x - a.x) * (g.u1 > g.u0 ? g.u1 - g.u0 : 1.0f), dv = (b.y - a.y) * (g.v1 > g.v0 ? g.v1 - g.v0 : 1.0f);
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
        float grid = MS_PaintGridOf(scene, a->material ? MaterialIndex(a->material) : -1);
        printf("paint:   atlas %s %dx%d %s%s grid %g unsaved %s hash %016llx refs %d%s%s\n", a->rel, a->img.width, a->img.height,
               a->material ? "material " : "own ", a->material ? a->material : a->modelPath ? a->modelPath : "-", (double)grid,
               AtlasLive(a) && AtlasUnsaved(a) ? "yes" : "no", PixelHash(&a->img), a->refs, a->provided ? " provided" : "",
               a->stale ? " (stale)" : "");
    }
    int n = MS_PaintEntryCount(scene);
    for (int i = 0; i < n; i++) {
        const MS_PaintEntry *e = MS_PaintEntryAt(scene, i);
        char where[48], cells[48] = "";
        if (e->obj.kind == MS_OBJ_BOX) FacesText(e->faces, where, (int)sizeof(where));
        else snprintf(where, sizeof(where), "slot %d", e->slot);
        // (a box on a grid: the cells its faces cover, 17.3)
        float grid = MS_PaintGridOf(scene, e->material);
        int cw, ch, big, many;
        if (e->obj.kind == MS_OBJ_BOX && e->obj.index >= 0 && e->obj.index < scene->boxCount && grid > 0.0f) {
            BoxGroupCells(e->obj.index, e->faces, grid, &cw, &ch, &big, &many);
            snprintf(cells, sizeof(cells), " cells %dx%d", cw, ch);
        }
        printf("paint:   entry line %d %s%s%s %u:%d %s %d %d %d %d%s%s%s\n", e->line, e->material >= 0 ? "\"" : "own",
               e->material >= 0 ? scene->materials[e->material].name : "", e->material >= 0 ? "\"" : "", e->entity.stmt, e->entity.index, where,
               e->x, e->y, e->w, e->h, cells, e->nearest ? " nearest" : "", e->active ? "" : " (inactive)");
    }
    for (int i = 0; i < dm->unwrapCount; i++) {
        const DrawUnwrap *u = &dm->unwraps[i];
        printf("paint:   unwrap %s %d meshes %g texels/unit (scale %g) sides", u->modelPath, u->u.meshCount, (double)u->texelsPerUnit,
               (double)u->scale);
        for (int k = 0; k < u->sideCount; k++) printf(" %d", u->sides[k]);
        printf("%s\n", u->written ? " written" : "");
    }
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
                                  const Vector2 *pts, int count, bool rmb, MonoMsg *line)
{
    DrawToolKind kind = DrawToolKindOf(dm->tool);
    bool bucket = dm->tool == TOOL_BUCKET && dm->bucket != DRAW_BUCKET_REGION;
    if (kind != TOOLKIND_BRUSH && !(dm->tool == TOOL_BUCKET)) {
        MsgSetf(line, "%s is not a brush or click tool: use %s", DrawToolScriptName(dm->tool),
                kind == TOOLKIND_SELECT ? "select" : dm->tool == TOOL_MOVE ? "move" : dm->tool == TOOL_EYEDROPPER ? "pick" : "shape");
        return 1;
    }
    MS_SurfaceHit hit;
    if (uv) {
        // (a model that needs an unwrap: made first, so its paint coordinates exist; synchronous here)
        if (obj.kind == MS_OBJ_MODEL && obj.index >= 0 && obj.index < scene->modelCount &&
            MS_ModelPaintUv(scene, scene->models[obj.index].file, fs) == MS_PAINTUV_NEEDS_UNWRAP) {
            if (EnsureUnwrap(dm, obj.index, fs, SlotMaterial(&scene->models[obj.index], fs)) != 0) {     // (not made: why)
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
    if (bucket) {       // (contiguous / global: one click at the first point, one step)
        bool begun = ShapeBegin(dm, em, &hit, TOOL_BUCKET, rmb, DT_SEL_REPLACE);
        if (begun) ShapeCommit(dm, em);
        MsgSetf(line, "bucket: %s", MsgText(&dm->message));
        return begun && !dm->messageError ? 0 : 1;
    }
    if (!StrokeBegin(dm, em, &hit)) { MsgSetf(line, "%s", MsgText(&dm->message)); return 1; }
    dm->strokeRmb = rmb;
    StepTool(dm, dm->stroke, dm->tool, rmb);
    bool spray = dm->tool == TOOL_SPRAY;
    Probe a = ProbeHit(dm, &hit, pts[0]);
    if (!dm->stroking) { MsgSetf(line, "%s", MsgText(&dm->message)); return 1; }
    if (a.ok && spray) SprayTick(dm, &a);       // (each point of a spray stroke: one tick, 1/60 s)
    else if (a.ok) StampProbe(dm, &a);
    StrokeFrame(dm);
    Flush(dm);
    for (int i = 1; i < count && DrawModeStampPen(dm) != PEN_FILL && dm->stroking; i++) {
        if (spray) {
            MS_SurfaceHit hb;
            if (uv ? UvHit(obj, fs, pts[i].x, pts[i].y, &hb) : (hb = CastAt(cam, w, h, pts[i]), true)) {
                Probe b = ProbeHit(dm, &hb, pts[i]);
                SprayTick(dm, &b);
            }
        } else if (uv) UvSegment(dm, obj, fs, pts[i - 1], pts[i]);
        else {
            MS_SurfaceHit hb = CastAt(cam, w, h, pts[i]);
            Probe b = ProbeHit(dm, &hb, pts[i]);
            StrokeSegment(dm, cam, w, h, a, b);
            a = b;
        }
        StrokeFrame(dm);
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

// A script shape's point on the shape's region: px through the shot's camera as the mouse (ShapeTexel:
// off the region, its plane), uv straight from paint coordinates; clamped to the region
static bool ScriptShapeTexel(DrawMode *dm, Camera3D cam, int w, int h, bool uv, MS_ObjRef obj, int fs, Vector2 pt, DtPoint *out)
{
    if (!uv) return ShapeTexel(dm, GetScreenToWorldRayEx(pt, cam, w, h), out);
    MS_SurfaceHit hit;
    PaintRegion r, want = ToPaintRegion(dm->shapeDrag.region);
    float tx, ty;
    int e;
    if (!UvHit(obj, fs, pt.x, pt.y, &hit) || (e = EntryFor(hit.hit.obj, hit.material, hit.face, hit.slot, NULL)) < 0 ||
        !HitRegion(dm, &hit, e, &r, &tx, &ty))
        return false;
    *out = ClampToRegion((DtPoint){ (int)floorf(tx), (int)floorf(ty) }, &want);
    return true;
}

// `shape` and `select` (docs/painting.md 18.10): the tool through these points as one press, drag (or
// clicks) and release. Returns 0 done, 1 failed; the script's line in *line either way
static unsigned char ScriptShape(DrawMode *dm, EditMode *em, Camera3D cam, int w, int h, DrawTool tool, bool uv, MS_ObjRef obj, int fs,
                                 const Vector2 *pts, int count, bool rmb, DtSelOp op, MonoMsg *line)
{
    bool many = tool == TOOL_CONTOUR || tool == TOOL_POLYGON || tool == TOOL_LASSO || tool == TOOL_LASSO_POLY;
    int need = tool == TOOL_CURVE ? 4 : tool == TOOL_WAND ? 1 : 2;
    if (many ? count < 3 : count != need) {
        if (many) MsgSetf(line, "%s needs 3 or more points, not %d", DrawToolScriptName(tool), count);
        else MsgSetf(line, "%s needs %d point%s, not %d", DrawToolScriptName(tool), need, need == 1 ? "" : "s", count);
        return 1;
    }
    MS_SurfaceHit hit;
    if (uv ? !UvHit(obj, fs, pts[0].x, pts[0].y, &hit) : (hit = CastAt(cam, w, h, pts[0]), false)) {
        MsgSetStatic(line, "no such surface point (face / slot, paint uv)");
        return 1;
    }
    HoverHit(dm, &hit);
    if (!dm->hoverOk) {
        if (dm->refusalCode >= 2) MsgSetf(line, "%s", MsgText(&dm->refusal));
        else MsgSetStatic(line, "nothing to paint there");
        return 1;
    }
    MsgSetStatic(&dm->message, "");
    dm->messageError = false;
    if (tool == TOOL_WAND) WandAt(dm, &hit, op);
    else if (ShapeBegin(dm, em, &hit, tool, rmb, op)) {
        DrawShape *S = &dm->shapeDrag;
        DtPointsClear(&S->pts);
        for (int i = 0; i < count; i++) {
            DtPoint q;
            if (!ScriptShapeTexel(dm, cam, w, h, uv, obj, fs, pts[i], &q)) {
                ShapeCancel(dm);
                MsgSetf(line, "%s: point %d is not on the surface's plane", DrawToolScriptName(tool), i + 1);
                return 1;
            }
            DtPointsPush(&S->pts, q.x, q.y);
        }
        ShapeCommit(dm, em);
    }
    MsgSetf(line, "%s: %s", DrawToolScriptName(tool), MsgText(&dm->message));
    return dm->messageError ? 1 : 0;
}

// --paint-script `print`'s tool and selection lines
static void PrintTool(const DrawMode *dm)
{
    static const char *const SHAPES[3] = { "round", "square", "soft" };
    printf("paint:   tool %s brush %s size %d pixelperfect %s fg %d %d %d %d bg %d %d %d %d spray %d %d seed %u bucket %s %d gradient %s palette %d",
           DrawToolScriptName(dm->tool), SHAPES[DrawModeStampPen(dm) <= PEN_SOFT ? DrawModeStampPen(dm) : 0], dm->size,
           dm->pixelPerfect ? "on" : "off", dm->fg.r, dm->fg.g, dm->fg.b, dm->fg.a, dm->bg.r, dm->bg.g, dm->bg.b, dm->bg.a, dm->sprayWidth,
           dm->spraySpeed, (unsigned int)dm->seed, dm->bucket == DRAW_BUCKET_GLOBAL ? "global" : dm->bucket == DRAW_BUCKET_REGION ? "region" : "contiguous",
           dm->tolerance, dm->gradientRadial ? "radial" : "linear", dm->paletteCount);
    for (int i = 0; i < dm->paletteCount; i++) printf(" #%02X%02X%02X%02X", dm->palette[i].r, dm->palette[i].g, dm->palette[i].b, dm->palette[i].a);
    printf("\n");
    const DrawSelection *S = &dm->sel;
    int x0, y0, x1, y1;
    if (!S->active || S->atlas < 0 || S->atlas >= dm->atlasCount || !DtMaskBounds(&S->mask, &x0, &y0, &x1, &y1)) {
        printf("paint:   selection none%s\n", S->lastSet ? " (reselect kept)" : "");
        return;
    }
    printf("paint:   selection %s region %d %d %d %d island %d bounds %d %d %d %d hash %016llx\n", dm->atlases[S->atlas].rel, S->region.x,
           S->region.y, S->region.w, S->region.h, S->region.island, x0, y0, x1, y1, (unsigned long long)DtMaskHash(&S->mask));
}

// A script's points: `px <x> <y> ...` or `uv <id N:i | entity HEX:i> <face|slot> <u> <v> ...` from w[*at]
// (n words); false: not that form
static bool ScriptPoints(char **wd, int n, int at, bool *uv, MS_ObjRef *obj, int *fs, Vector2 **pts, int *ptCap, int *count)
{
    if (at >= n || (strcmp(wd[at], "px") != 0 && strcmp(wd[at], "uv") != 0)) return false;
    *uv = wd[at][0] == 'u';
    at++;
    *fs = -1;
    *obj = (MS_ObjRef){ MS_OBJ_NONE, -1 };
    if (*uv) {
        float v = 0.0f;
        if (!ParseEntity(wd, n, &at, obj) || at >= n || !ScriptNum(wd[at], &v) || v < 0) return false;
        *fs = (int)v;
        at++;
    }
    *count = (n - at) / 2;
    if ((n - at) % 2 != 0 || *count < 1 || !MS_RESERVE(*pts, *ptCap, *count)) return false;
    for (int k = 0; k < *count; k++)
        if (!ScriptNum(wd[at + 2 * k], &(*pts)[k].x) || !ScriptNum(wd[at + 2 * k + 1], &(*pts)[k].y)) return false;
    return true;
}

static bool ScriptColor(char **wd, int n, Color *c)
{
    float f[4];
    if (n != 4 && n != 5) return false;
    for (int k = 0; k < n - 1; k++)
        if (!ScriptNum(wd[1 + k], &f[k]) || f[k] < 0 || f[k] > 255) return false;
    *c = (Color){ (unsigned char)f[0], (unsigned char)f[1], (unsigned char)f[2], n == 5 ? (unsigned char)f[3] : 255 };
    return true;
}

// --paint-script <file> (docs/painting.md 14): one command per line, '#' starts a comment; each prints
// a "paint: file:line: ..." line, the first failing one stops the script (exit 1):
//   pen <round|square|soft|eraser|fill>   size <1-256>   hardness <0-1>   color <r> <g> <b> [<a>]
//   density <8-512>                    texels per metre of new rectangles (and resize) without a paint grid
//   pixel <on|off>                     the pixel look: new atlases, and the atlas painted last
//   faces <a b c d e f>                the face groups of the box painted (or hovered) last (5.3), applied
//   resize                             the rectangle painted (or hovered) last to the size of the current
//                                      density; on a material with a paint grid: fit to grid (17.7)
//   clear                              the paint of the face / slot painted (or hovered) last (its entry goes)
//   stroke px <x0> <y0> <x1> <y1> ...  an LMB drag through these screen pixels of the shot's camera
//                                      (one segment per frame, cut in screen space as the mouse is)
//   stroke uv <id N:i | entity HEX:i> <face|slot> <u0> <v0> [<u1> <v1> ...]
//                                      the same in paint coordinates: a box face (0..5 = +x -x +y -y +z
//                                      -z, u v 0..1 on it) or a model slot (u v its paint UV; a model
//                                      that needs an unwrap gets it first)
//   dab px <x> <y> / dab uv <obj> <face|slot> <u> <v>   one stamp (fill: fills there)
//   hover px <x> <y>                   what the cursor is over there (the HUD, the brush cursor and the
//                                      panel's lines, the grid's among them); faces, resize and clear
//                                      then act on that surface (until the next stroke); pixel doesn't
//   pick px <x> <y>                    the eyedropper (prints the colour; it becomes the colour)
//   undo / redo / save / print         print: the atlases (size, unsaved, pixel hash), the entries,
//                                      the unwraps, the history
//   poll                               the hot reload's check (8 x 0.6 s): after a save nothing reloads
//   disk <file>                        the map file becomes the file's bytes, then the hot reload's path
//                                      (reload, the unsaved edits and strokes rebased)
//   put <file> <package path>          a package file becomes the file's bytes (a PNG a request
//                                      repainted), no reload yet; reload: the reload now
// The tools (docs/painting.md 18.10); `stroke` and `dab` use the current tool when it is a brush or the
// bucket (spray: each point one tick of dots; bucket contiguous / global: a click at the first point):
//   tool <name>                        pencil eraser spray bucket gradient line curve rect rectfill ellipse
//                                      ellipsefill contour polygon marquee ellipsemarquee lasso polylasso
//                                      wand move blur jumble eyedropper
//   brush <round|square|soft>   pixelperfect <on|off>   swap   gradient <linear|radial>
//   fg <r> <g> <b> [<a>] / bg ...      the colours (color is fg)
//   spray <width> <speed> <seed>       the spray's options and the next strokes' seed (+1 per stroke)
//   bucket <contiguous|global|region> [<tolerance>]
//   button <left|right>                the button of the next strokes, shapes and picks (right: bg)
//   shape px <x0> <y0> ... / shape uv <obj> <face|slot> <u0> <v0> ...
//                                      the current shape tool through these points as one stroke (line,
//                                      rect, ellipse, gradient: 2; curve: 4, a b c1 c2; contour, polygon: 3+);
//                                      a point off the surface maps through its plane, clamped to the region
//   shiftclick px <x> <y>              SHIFT+click with the pencil / eraser (a line from its last point)
//   select <rect|ellipse|lasso|wand> px|uv ... [add|sub|and]    a selection (2, 2, 3+, 1 points)
//   selectall / deselect / reselect / invert    on the surface painted (or hovered) last
//   move <dx> <dy>                     the move tool: the selection's pixels (none: the region's)
//   pick paint px <x> <y>              the paint eyedropper into fg (button right: bg)
//   print                              also: the tool and its options, the colours, the palette, the
//                                      selection (its region, bounds and DtMaskHash)
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
    // The surface `hover` named last (faces, resize and clear act on it while it is newer than the last
    // stroke; pixel keeps acting on the last stroke's surface, as before the paint-grid round)
    MS_SurfaceHit hoverHit = { 0 };
    bool hoverNewer = false;
    bool rmb = false;           // `button right`: the next strokes, shapes and picks use the background colour
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
            for (int k = 0; k < PEN_COUNT; k++) if (strcmp(wd[1], NAMES[k]) == 0) { DrawModeSetPen(dm, (PenKind)k); ok = true; }
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
            if (ok) MsgSetf(&msg, "color %d %d %d %d", dm->fg.r, dm->fg.g, dm->fg.b, dm->fg.a);
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
            ok = dm->lastHitSet || hoverNewer;
            for (int k = 0; k < 6 && ok; k++) {
                ok = ScriptNum(wd[1 + k], &f[0]) && f[0] >= 0 && f[0] <= 5;
                cells[k] = (unsigned char)(ok ? f[0] : 0);
            }
            if (ok) ok = ActFaces(dm, hoverNewer ? &hoverHit : &dm->lastHit, cells) == DRAWMODE_RELOADED;
            if (dm->messageSaid) MsgSetf(&msg, "%s", MsgText(&dm->message));
            else MsgSetStatic(&msg, "faces <a b c d e f> (0-5), after a stroke on a box");
        } else if ((strcmp(wd[0], "resize") == 0 || strcmp(wd[0], "clear") == 0) && n == 1) {
            bool resize = wd[0][0] == 'r';
            const MS_SurfaceHit *on = hoverNewer ? &hoverHit : &dm->lastHit;
            ok = (dm->lastHitSet || hoverNewer) && (resize ? ActResize(dm, on) : ActClear(dm, on)) == DRAWMODE_RELOADED;
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
            else ok = ScriptStroke(dm, em, cam, w, h, uv, obj, fs, pts, count, rmb, &msg) == 0;     // (0: done)
            hoverNewer = false;
        } else if (strcmp(wd[0], "hover") == 0 && n == 4 && strcmp(wd[1], "px") == 0 && ScriptNum(wd[2], &f[0]) && ScriptNum(wd[3], &f[1])) {
            DrawModeHover(dm, cam, (Vector2){ f[0], f[1] }, w, h);
            ok = true;
            // (faces, resize and clear act on it now, as the panel acts on its target: also on an entry no
            // stroke can reach, one too small for its grid cells; pixel stays on the last stroke's surface,
            // so `pixel on` before a first stroke still sets the switch for new atlases)
            if (HitIsBox(&dm->target.hit) || HitIsModel(&dm->target.hit)) {
                hoverHit = dm->target.hit;
                hoverNewer = true;
            }
            bool refused = dm->refusalCode >= 2;    // (a refusal; the panel's lines where they apply)
            const DrawTarget *T = &dm->target;
            MsgSetf(&msg, "hover %s%s%s%s%s%s%s%s%s", dm->hoverOk ? "paintable" : "not paintable", refused ? ": " : "",
                    refused ? MsgText(&dm->refusal) : "", T->intoSet ? " | " : "", T->intoSet ? MsgText(&T->into) : "", T->texelSet ? " | " : "",
                    T->texelSet ? MsgText(&T->texel) : "", T->gridLineSet ? " | " : "", T->gridLineSet ? MsgText(&T->gridLine) : "");
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
            PrintTool(dm);
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
        } else if (strcmp(wd[0], "tool") == 0 && n == 2) {
            int t = DrawToolByScriptName(wd[1]);
            ok = t >= 0;
            if (ok) {
                ShapeCancel(dm);
                dm->tool = (DrawTool)t;
                MsgSetf(&msg, "tool %s", DrawToolName(dm->tool));
            } else MsgSetf(&msg, "tool: no tool named %s", wd[1]);
        } else if (strcmp(wd[0], "brush") == 0 && n == 2) {
            static const char *const SHAPES[3] = { "round", "square", "soft" };
            for (int k = 0; k < 3; k++)
                if (strcmp(wd[1], SHAPES[k]) == 0) { dm->shape = (PenKind)k; ok = true; }
            MsgSetf(&msg, ok ? "brush %s" : "brush <round|square|soft>, not %s", wd[1]);
        } else if (strcmp(wd[0], "pixelperfect") == 0 && n == 2 && (strcmp(wd[1], "on") == 0 || strcmp(wd[1], "off") == 0)) {
            dm->pixelPerfect = wd[1][1] == 'n';
            ok = true;
            MsgSetf(&msg, "pixel-perfect %s", wd[1]);
        } else if ((strcmp(wd[0], "fg") == 0 || strcmp(wd[0], "bg") == 0) && (n == 4 || n == 5)) {
            Color c;
            ok = ScriptColor(wd, n, &c);
            if (ok && wd[0][0] == 'f') SetColor(dm, c);
            else if (ok) {
                dm->bg = c;
                AddRecent(dm, c);
            }
            if (ok) MsgSetf(&msg, "%s %d %d %d %d", wd[0], c.r, c.g, c.b, c.a);
            else MsgSetf(&msg, "%s <r> <g> <b> [<a>] (0-255)", wd[0]);
        } else if (strcmp(wd[0], "swap") == 0 && n == 1) {
            Color c = dm->fg;
            dm->fg = dm->bg;
            dm->bg = c;
            ok = true;
            MsgSetStatic(&msg, "swapped fg and bg");
        } else if (strcmp(wd[0], "spray") == 0 && n == 4 && ScriptNum(wd[1], &f[0]) && ScriptNum(wd[2], &f[1]) && ScriptNum(wd[3], &f[2]) &&
                   f[0] >= 1 && f[0] <= 256 && f[1] >= 1 && f[1] <= 256 && f[2] >= 0 && f[2] <= 4294967295.0f) {
            dm->sprayWidth = (int)f[0];
            dm->spraySpeed = (int)f[1];
            dm->seed = (uint32_t)strtoul(wd[3], NULL, 10);
            ok = true;
            MsgSetf(&msg, "spray width %d speed %d seed %u", dm->sprayWidth, dm->spraySpeed, (unsigned int)dm->seed);
        } else if (strcmp(wd[0], "bucket") == 0 && (n == 2 || n == 3)) {
            static const char *const MODES[3] = { "contiguous", "global", "region" };
            int mode = -1;
            for (int k = 0; k < 3; k++)
                if (strcmp(wd[1], MODES[k]) == 0) mode = k;
            ok = mode >= 0 && (n == 2 || (ScriptNum(wd[2], &f[0]) && f[0] >= 0 && f[0] <= 255));
            if (ok) {
                dm->bucket = (DrawBucketMode)mode;
                if (n == 3) dm->tolerance = (int)f[0];
                MsgSetf(&msg, "bucket %s tolerance %d", MODES[mode], dm->tolerance);
            } else MsgSetStatic(&msg, "bucket <contiguous|global|region> [<tolerance 0-255>]");
        } else if (strcmp(wd[0], "gradient") == 0 && n == 2 && (strcmp(wd[1], "linear") == 0 || strcmp(wd[1], "radial") == 0)) {
            dm->gradientRadial = wd[1][0] == 'r';
            ok = true;
            MsgSetf(&msg, "gradient %s", wd[1]);
        } else if (strcmp(wd[0], "button") == 0 && n == 2 && (strcmp(wd[1], "left") == 0 || strcmp(wd[1], "right") == 0)) {
            rmb = wd[1][0] == 'r';
            ok = true;
            MsgSetf(&msg, "button %s", wd[1]);
        } else if (strcmp(wd[0], "shape") == 0 || strcmp(wd[0], "select") == 0) {
            // shape px|uv ...: the current shape tool; select <rect|ellipse|lasso|wand> px|uv ... [add|sub|and]
            bool sel = wd[0][1] == 'e';
            DtSelOp op = DT_SEL_REPLACE;
            int words = n;
            if (sel && n > 2) {
                const char *last = wd[n - 1];
                op = strcmp(last, "add") == 0 ? DT_SEL_ADD : strcmp(last, "sub") == 0 ? DT_SEL_SUB : strcmp(last, "and") == 0 ? DT_SEL_AND : DT_SEL_REPLACE;
                if (op != DT_SEL_REPLACE) words--;
            }
            int tool = -1;
            if (!sel) tool = DrawToolKindOf(dm->tool) == TOOLKIND_SHAPE ? (int)dm->tool : -1;
            else if (n > 1) {
                static const char *const KINDS[4] = { "rect", "ellipse", "lasso", "wand" };
                static const DrawTool TOOLS[4] = { TOOL_MARQUEE, TOOL_MARQUEE_ELLIPSE, TOOL_LASSO, TOOL_WAND };
                for (int k = 0; k < 4; k++)
                    if (strcmp(wd[1], KINDS[k]) == 0) tool = TOOLS[k];
            }
            bool uv;
            int count, fs;
            MS_ObjRef obj;
            if (tool < 0 && !sel) MsgSetf(&msg, "shape: %s is not a shape tool (line, curve, rect, rectfill, ellipse, ellipsefill, gradient, contour, polygon)",
                                          DrawToolScriptName(dm->tool));
            else if (tool < 0) MsgSetStatic(&msg, "select <rect|ellipse|lasso|wand> px|uv ... [add|sub|and]");
            else if (!ScriptPoints(wd, words, sel ? 2 : 1, &uv, &obj, &fs, &pts, &ptCap, &count))
                MsgSetf(&msg, "%s px <x> <y> ... | %s uv <id N:i | entity HEX:i> <face|slot> <u> <v> ...", wd[0], wd[0]);
            else ok = ScriptShape(dm, em, cam, w, h, (DrawTool)tool, uv, obj, fs, pts, count, rmb && !sel, op, &msg) == 0;
            hoverNewer = false;
        } else if (strcmp(wd[0], "shiftclick") == 0 && n == 4 && strcmp(wd[1], "px") == 0 && ScriptNum(wd[2], &f[0]) && ScriptNum(wd[3], &f[1])) {
            if (dm->tool != TOOL_PENCIL && dm->tool != TOOL_ERASER) MsgSetStatic(&msg, "shiftclick: the pencil or the eraser");
            else {
                MS_SurfaceHit hit = CastAt(cam, w, h, (Vector2){ f[0], f[1] });
                HoverHit(dm, &hit);
                MsgSetStatic(&dm->message, "");
                dm->messageError = false;
                if (dm->hoverOk) PressStroke(dm, em, &hit, (Vector2){ f[0], f[1] }, rmb, true);
                if (!dm->hoverOk) MsgSetStatic(&msg, "shiftclick: nothing to paint there");
                else if (dm->stroking) {
                    int samples = dm->stroke->sampleCount;
                    StrokeEnd(dm, em);
                    ok = !dm->messageError;
                    MsgSetf(&msg, "shiftclick: %s (%d samples)", MsgText(&dm->message), samples);
                } else MsgSetf(&msg, "shiftclick: %s", MsgText(&dm->message));
            }
            hoverNewer = false;
        } else if ((strcmp(wd[0], "selectall") == 0 || strcmp(wd[0], "deselect") == 0 || strcmp(wd[0], "reselect") == 0 ||
                    strcmp(wd[0], "invert") == 0) && n == 1) {
            dm->messageError = false;
            if (wd[0][0] == 's') SelectAllAt(dm, hoverNewer ? &hoverHit : dm->lastHitSet ? &dm->lastHit : NULL);
            else if (wd[0][0] == 'd') {
                ok = dm->sel.active;
                Deselect(dm);
            } else if (wd[0][0] == 'r') Reselect(dm);
            else SelectInvert(dm);
            ok = !dm->messageError && (wd[0][0] != 'd' || ok);
            MsgSetf(&msg, "%s: %s", wd[0], dm->messageSaid ? MsgText(&dm->message) : "nothing selected");
        } else if (strcmp(wd[0], "move") == 0 && n == 3 && ScriptNum(wd[1], &f[0]) && ScriptNum(wd[2], &f[1])) {
            // the move tool on the selection's region (none: the region of the surface painted or hovered last)
            const MS_SurfaceHit *on = dm->sel.active ? &dm->sel.hit : hoverNewer ? &hoverHit : dm->lastHitSet ? &dm->lastHit : NULL;
            MsgSetStatic(&dm->message, "");
            dm->messageError = false;
            if (!on) MsgSetStatic(&msg, "move: nothing selected or painted yet");
            else if (ShapeBegin(dm, em, on, TOOL_MOVE, false, DT_SEL_REPLACE)) {
                DrawShape *S = &dm->shapeDrag;
                S->pts.p[1] = (DtPoint){ S->pts.p[0].x + (int)f[0], S->pts.p[0].y + (int)f[1] };
                ShapeCommit(dm, em);
                ok = !dm->messageError;
                MsgSetf(&msg, "move %d %d: %s", (int)f[0], (int)f[1], MsgText(&dm->message));
            } else MsgSetf(&msg, "move: %s", MsgText(&dm->message));
        } else if (strcmp(wd[0], "pick") == 0 && n == 5 && strcmp(wd[2], "px") == 0 && strcmp(wd[1], "paint") == 0 && ScriptNum(wd[3], &f[0]) &&
                   ScriptNum(wd[4], &f[1])) {
            MS_SurfaceHit hit = CastAt(cam, w, h, (Vector2){ f[0], f[1] });
            Color c = { 0 };
            bool paint = false;
            ok = Eyedropper(dm, &hit, cam, w, h, (Vector2){ f[0], f[1] }, &c, &paint);
            if (ok && rmb) {
                dm->bg = c;
                AddRecent(dm, c);
            } else if (ok) SetColor(dm, c);
            if (ok) MsgSetf(&msg, "picked %d %d %d %d into %s (%s)", c.r, c.g, c.b, c.a, rmb ? "bg" : "fg", paint ? "the paint" : "the albedo");
            else MsgSetStatic(&msg, "nothing there");
        } else if (strcmp(wd[0], "pick") == 0 && n >= 2 && strcmp(wd[1], "screen") == 0) {
            // (docs/painting.md 18.8: main.c's screenRead draws the scene with the shot's camera into the
            // shot target and reads the one pixel, no HUD)
            Color c = { 0 };
            ok = n == 5 && strcmp(wd[2], "px") == 0 && ScriptNum(wd[3], &f[0]) && ScriptNum(wd[4], &f[1]) &&
                 f[0] >= 0 && f[1] >= 0 && f[0] < w && f[1] < h;
            if (!ok) MsgSetStatic(&msg, "pick screen px <x> <y> (inside the shot)");
            else if (!dm->screenRead) {
                ok = false;
                MsgSetStatic(&msg, "pick screen: no scene image to read here");
            } else if (!(ok = dm->screenRead(dm->screenUser, (int)f[0], (int)f[1], &c))) {
                MsgSetStatic(&msg, "pick screen: the pixel could not be read");
            } else {
                c.a = 255;
                if (rmb) {
                    dm->bg = c;
                    AddRecent(dm, c);
                } else SetColor(dm, c);
                MsgSetf(&msg, "picked %d %d %d %d into %s (the screen)", c.r, c.g, c.b, c.a, rmb ? "bg" : "fg");
            }
        } else MsgSetf(&msg, "unknown command '%s' (pen size hardness color density pixel faces resize clear stroke dab hover pick undo redo "
                             "save print poll disk put reload tool brush pixelperfect fg bg swap spray bucket gradient button shape "
                             "shiftclick select selectall deselect reselect invert move)", wd[0]);
        printf("paint: %s:%d: %s%s\n", path, lineNo, ok ? "" : "FAILED: ", MsgText(&msg));
        if (!ok) status = 1;
    }
    Flush(dm);
    PrepareSelection(dm);       // (the shot shows the selection's outline)
    free(pts);
    MsgFree(&msg);
    fflush(stdout);
    UnloadFileText(text);
    return status;
}

// #endregion
