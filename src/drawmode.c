// drawmode -- B in spectator mode: painting on boxes and models (design: docs/painting.md)
//
// A canvas is an RGBA8 image of one package PNG (textures/paint/paint_<id>.png, or in shared mode a
// texture the map has) with its base image (the eraser puts it back), its box face layout
// (uv_faces) or its model slot's UV islands. The runtime only gets pixels: MS_TextureProvide for the
// next live edit that loads the path, MS_TextureUpdate for the rectangle a frame's stamps changed
// (one call per canvas per frame).
//
// The first stroke on an object without a canvas of its own is one undoable edit (EDIT_PAINT_MATERIAL,
// mapedit.c): a copy of its material under the name paint_<id> (a model slot: paint_<id>_s<N>) with
// albedo 255 255 255, the canvas as albedo_map (a box: uv_faces 0 1 2 3 4 5) and the source's other
// keys; the canvas holds what the surface showed (the flat colour, or the texture with its tint and
// tiling baked in, a model slot's own texture read back from the GPU). Strokes after it are steps of
// paint alone. Every step keeps the 64 x 64 tiles a stroke touched as they were before it; undo and
// redo swap them with the canvas (mapedit.c calls the hooks of this file), so one copy serves both.
//
// A box whose material has a surface shader is baked on its first stroke (docs/painting.md 3.6):
// LMB asks for the bake, DrawModeRunBake renders it before the frame's scene draw (surfacebake.c:
// albedo into the canvas, an ORM map, a normal map when the shader bends the normal), and the next
// DrawModeUpdate begins the stroke there; the copy has no shader, the ORM / normal maps go with the
// canvas (provided, saved, recorded, taken out of the scene by undo). A script bakes at once.
//
// A stroke: LMB down to up. Every frame the segment from the last cursor position to the current
// one is cut in screen space until consecutive hits lie at most `spacing` texels apart on the same
// face / island (at most 10 levels deep, on a fixed stack); a stamp at each sample, clipped to the
// hit's face cell (boxes) or UV island (models), composited source-over in sRGB bytes from the
// pixels the stroke started from, with the highest coverage any stamp gave a texel (a mask per
// touched tile), so a stroke never darkens where it crosses itself. Fill fills the region at LMB
// down. The stroke keeps its samples in texture space, so a rebase can replay it onto a PNG that
// changed on disk.
//
// CTRL+S writes every canvas with unsaved pixels (PngWrite), then the map (MapEditSave), then the
// licence records (ASSETS.md rows and .asset.md sidecars for the PNGs it wrote first), then
// MS_EditMarkSaved, so the hot reload doesn't reload the editor's own writes.
#include "drawmode.h"
#include "png.h"
#include "surfacebake.h"
#include "ui.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>

#define PAINT_DIR       "textures/paint/"
#define DRAW_COLOR      (Color){ 120, 210, 255, 255 }
#define WARN_TIME       3.0f
#define SUBDIV_DEPTH    10
#define STROKE_KEY      1000        // EditModeConfirm's key for a stroke (LMB again within 3 s)

static bool Ctrl(void) { return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL); }
static bool Shift(void) { return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }
static bool Alt(void) { return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT); }

static void Say(DrawMode *dm, bool error, const char *text)
{
    snprintf(dm->message, sizeof(dm->message), "%.*s", (int)sizeof(dm->message) - 1, text);
    dm->messageTime = error ? 8.0f : 5.0f;
    dm->messageError = error;
    TraceLog(error ? LOG_WARNING : LOG_INFO, "monolith: draw: %s", text);
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
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    SbAdd(b, tmp, n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1);
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

static bool StartsWith(const char *s, const char *prefix) { return s && strncmp(s, prefix, strlen(prefix)) == 0; }

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

static Image CopyImage(Image src)
{
    Image img = NewImage(src.width, src.height);
    if (img.data) memcpy(img.data, src.data, (size_t)src.width * (size_t)src.height * 4);
    return img;
}

static void FreeImage(Image *img)
{
    free(img->data);    // (images of this file are malloc'd; raylib's loads go through UnloadImage first)
    *img = (Image){ 0 };
}

// A raylib image (LoadImage, MS_TextureRead) as one of this file's: RGBA8 in malloc'd memory
static Image AdoptImage(Image src)
{
    Image img = { 0 };
    if (!src.data) return img;
    if (src.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) ImageFormat(&src, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    if (src.data && src.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) img = CopyImage(src);
    UnloadImage(src);
    return img;
}

// Texel (x, y) of an image with the wrap mode (0 repeat, 1 clamp, 2 mirror)
static int WrapIndex(int i, int n, int wrap)
{
    if (wrap == 1) return ClampI(i, 0, n - 1);
    if (wrap == 2) {
        int p = ((i % (2 * n)) + 2 * n) % (2 * n);
        return p < n ? p : 2 * n - 1 - p;
    }
    return ((i % n) + n) % n;
}

static float WrapCoord(float u, int wrap)
{
    if (wrap == 1) return u < 0.0f ? 0.0f : u > 0.999999f ? 0.999999f : u;
    if (wrap == 2) {
        float t = fmodf(fabsf(u), 2.0f);
        t = t > 1.0f ? 2.0f - t : t;
        return t > 0.999999f ? 0.999999f : t;
    }
    float t = u - floorf(u);
    return t > 0.999999f ? 0.999999f : t;
}

// Bilinear sample at (u, v) (0..1 over the image, wrapped), RGBA 0..255
static void SampleBilinear(const Image *im, float u, float v, int wrap, float out[4])
{
    float x = u * im->width - 0.5f, y = v * im->height - 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    const unsigned char *d = im->data;
    for (int c = 0; c < 4; c++) out[c] = 0.0f;
    for (int k = 0; k < 4; k++) {
        int xi = WrapIndex(x0 + (k & 1), im->width, wrap), yi = WrapIndex(y0 + (k >> 1), im->height, wrap);
        float w = ((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fy : 1.0f - fy);
        const unsigned char *p = d + ((size_t)yi * im->width + xi) * 4;
        for (int c = 0; c < 4; c++) out[c] += w * p[c];
    }
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

// #endregion

// ======================================================================================
// #region The map text: a material's keys, every material's albedo map
// ======================================================================================

typedef struct { const char *s; int n; bool q; } Tok;      // n bytes at s (quoted: with the quotes)
typedef struct { Tok *t; int n, cap; } Toks;

// One line's tokens (words, "..." strings; '#' ends the line)
static bool LineToks(const char *s, int len, Toks *out)
{
    out->n = 0;
    int p = 0;
    for (;;) {
        while (p < len && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) p++;
        if (p >= len || s[p] == '#') return true;
        Tok t = { s + p, 0, s[p] == '"' };
        if (t.q) {
            const char *close = memchr(s + p + 1, '"', (size_t)(len - p - 1));
            if (!close) return false;
            t.n = (int)(close - (s + p)) + 1;
        } else
            while (p + t.n < len && s[p + t.n] != ' ' && s[p + t.n] != '\t' && s[p + t.n] != '\r' && s[p + t.n] != '#' &&
                   s[p + t.n] != '"') t.n++;
        Tok *slot = MS_PUSH(out->t, out->n, out->cap);
        if (!slot) return false;
        *slot = t;
        p += t.n;
    }
}

static bool TokIs(const Tok *t, const char *w) { return !t->q && t->n == (int)strlen(w) && memcmp(t->s, w, (size_t)t->n) == 0; }

static bool TokNumber(const Tok *t)
{
    if (t->q || t->n == 0 || t->n > 60) return false;
    char buf[64];
    memcpy(buf, t->s, (size_t)t->n);
    buf[t->n] = '\0';
    char *end;
    strtod(buf, &end);
    return end != buf && *end == '\0';
}

// The tokens of the key at k and its values: [k, end) (the rules of mapedit.c's KeyEnd)
static int KeyEnd(const Toks *T, int k)
{
    static const char *const WORD[] = { "filter", "wrap", "cull", "transparency", "blend", "unlit", "instanced", "inherit" };
    int e = k + 1;
    for (size_t i = 0; i < sizeof(WORD) / sizeof(WORD[0]); i++)
        if (TokIs(&T->t[k], WORD[i])) return e < T->n ? e + 1 : e;
    if (TokIs(&T->t[k], "sampler") || TokIs(&T->t[k], "data_sampler")) return k + 3 <= T->n ? k + 3 : T->n;
    if (TokIs(&T->t[k], "uniform") && e < T->n) e++;
    while (e < T->n && (T->t[e].q || TokNumber(&T->t[e]))) e++;
    return e;
}

// Calls fn for every `material "name" ...` statement of text (name NULL: every material): the
// line's tokens (key tokens from index 2)
typedef void (*MatLineFn)(void *user, const Toks *T, const char *name, int nameLen);
static void ForMaterialLines(const char *text, int size, const char *name, MatLineFn fn, void *user)
{
    Toks T = { 0 };
    int p = 0;
    while (p < size) {
        int s = p;
        while (p < size && text[p] != '\n') p++;
        int len = p - s;
        if (p < size) p++;
        int i = 0;
        while (i < len && (text[s + i] == ' ' || text[s + i] == '\t')) i++;
        if (len - i < 9 || memcmp(text + s + i, "material", 8) != 0) continue;
        if (!LineToks(text + s, len, &T) || T.n < 2 || !TokIs(&T.t[0], "material") || !T.t[1].q) continue;
        int nl = T.t[1].n - 2;
        if (name && (nl != (int)strlen(name) || memcmp(T.t[1].s + 1, name, (size_t)nl) != 0)) continue;
        fn(user, &T, T.t[1].s + 1, nl);
    }
    free(T.t);
}

// A material's keys as written (its lines' keys, joined by single blanks)
static void AddKeys(void *user, const Toks *T, const char *name, int nameLen)
{
    (void)name; (void)nameLen;
    Sb *b = user;
    for (int k = 2; k < T->n; k++) {
        if (b->len > 0) SbStr(b, " ");
        SbAdd(b, T->t[k].s, T->t[k].n);
    }
}

// Per material of the scene: the keys that matter here (from its lines)
typedef struct DrawMat {
    char *albedoMap;        // package path, NULL
    int wrap;
    bool nearest;
    bool dataSampled;       // its albedo map is also some material's data_sampler (holds data)
    bool bakedMaps;         // an orm_map / normal_map under textures/paint/: a baked shader's (3.6)
    int canvas;             // the canvas of albedoMap, -1
} DrawMat;

typedef struct { DrawMode *dm; } MatScan;

static int MaterialIndex(const char *name, int nameLen)
{
    for (int m = 0; m < scene->materialCount; m++)
        if ((int)strlen(scene->materials[m].name) == nameLen && memcmp(scene->materials[m].name, name, (size_t)nameLen) == 0) return m;
    return -1;
}

static void ScanMat(void *user, const Toks *T, const char *name, int nameLen)
{
    DrawMode *dm = ((MatScan *)user)->dm;
    int m = MaterialIndex(name, nameLen);
    if (m < 0 || m >= dm->matCount) return;
    DrawMat *D = &dm->mats[m];
    for (int k = 2; k < T->n; k = KeyEnd(T, k)) {
        const Tok *v = k + 1 < T->n ? &T->t[k + 1] : NULL;
        if (TokIs(&T->t[k], "albedo_map") && v && v->q) {
            free(D->albedoMap);
            D->albedoMap = MS_StrNDup(v->s + 1, (size_t)(v->n - 2));
        } else if ((TokIs(&T->t[k], "orm_map") || TokIs(&T->t[k], "normal_map")) && v && v->q &&
                   v->n - 2 >= (int)strlen(PAINT_DIR) && memcmp(v->s + 1, PAINT_DIR, strlen(PAINT_DIR)) == 0) {
            D->bakedMaps = true;
        } else if (TokIs(&T->t[k], "wrap") && v) D->wrap = TokIs(v, "clamp") ? 1 : TokIs(v, "mirror") ? 2 : 0;
        else if (TokIs(&T->t[k], "filter") && v) D->nearest = TokIs(v, "nearest");
    }
}

static void ScanDataSamplers(void *user, const Toks *T, const char *name, int nameLen)
{
    (void)name; (void)nameLen;
    DrawMode *dm = ((MatScan *)user)->dm;
    for (int k = 2; k < T->n; k = KeyEnd(T, k)) {
        if (!TokIs(&T->t[k], "data_sampler") || k + 2 >= T->n || !T->t[k + 2].q) continue;
        const Tok *v = &T->t[k + 2];
        for (int m = 0; m < dm->matCount; m++)
            if (dm->mats[m].albedoMap && (int)strlen(dm->mats[m].albedoMap) == v->n - 2 &&
                memcmp(dm->mats[m].albedoMap, v->s + 1, (size_t)(v->n - 2)) == 0) dm->mats[m].dataSampled = true;
    }
}

static int CanvasByRel(const DrawMode *dm, const char *rel)
{
    for (int i = 0; rel && i < dm->canvasCount; i++)
        if (strcmp(dm->canvases[i].rel, rel) == 0) return i;
    return -1;
}

static void FreeMats(DrawMode *dm)
{
    for (int m = 0; m < dm->matCount; m++) free(dm->mats[m].albedoMap);
    dm->matCount = 0;
    dm->matValid = false;
}

// mats[] for the scene's current text (once per scene version)
static void RefreshMats(DrawMode *dm)
{
    if (dm->matValid && dm->matVersion == scene->version && dm->matCount == scene->materialCount) return;
    FreeMats(dm);
    if (!MS_RESERVE(dm->mats, dm->matCap, scene->materialCount)) return;
    dm->matCount = scene->materialCount;
    for (int m = 0; m < dm->matCount; m++) dm->mats[m] = (DrawMat){ .canvas = -1 };
    int size;
    const char *text = MS_MapText(scene, &size);
    MatScan s = { dm };
    if (text) {
        ForMaterialLines(text, size, NULL, ScanMat, &s);
        ForMaterialLines(text, size, NULL, ScanDataSamplers, &s);
    }
    for (int m = 0; m < dm->matCount; m++) dm->mats[m].canvas = CanvasByRel(dm, dm->mats[m].albedoMap);
    dm->matVersion = scene->version;
    dm->matValid = true;
}

static void InvalidateMats(DrawMode *dm) { dm->matValid = false; }

// Some material of the current text shows the canvas (its albedo_map)
static bool CanvasLive(DrawMode *dm, int ci)
{
    RefreshMats(dm);
    for (int m = 0; m < dm->matCount; m++)
        if (dm->mats[m].albedoMap && strcmp(dm->mats[m].albedoMap, dm->canvases[ci].rel) == 0) return true;
    return false;
}

// The material a model instance draws slot `slot` with (material.N, material, use), -1: its own
static int SlotMaterial(const MS_ModelInstance *M, int slot)
{
    for (int i = 0; i < M->slotCount; i++)
        if (scene->modelSlots[M->slotFirst + i].slot == slot) return scene->modelSlots[M->slotFirst + i].material;
    return M->ref.material;
}

// #endregion

// ======================================================================================
// #region Canvases: layout, islands, tiles
// ======================================================================================

// The texel rectangle of face f's cell (no layout: the whole image)
static void CellRect(const Canvas *c, int f, int *x0, int *y0, int *x1, int *y1)
{
    if (!c->layout || f < 0 || f > 5) { *x0 = *y0 = 0; *x1 = c->img.width; *y1 = c->img.height; return; }
    Rectangle r = MS_FaceCellRect(c->cells, f, false);
    *x0 = (int)lroundf(r.x * c->img.width);
    *y0 = (int)lroundf(r.y * c->img.height);
    *x1 = (int)lroundf((r.x + r.width) * c->img.width);
    *y1 = (int)lroundf((r.y + r.height) * c->img.height);
}

static void LayoutGrid(const unsigned char cells[6], int *cols, int *rows)
{
    int k = 0;
    for (int f = 0; f < 6; f++) k = cells[f] + 1 > k ? cells[f] + 1 : k;
    int c = 1;
    while (c * c < k) c++;
    *cols = c;
    *rows = (k + c - 1) / c;
}

static bool TilesInit(Canvas *c)
{
    free(c->tileSlot);
    c->tilesX = (c->img.width + DRAW_TILE - 1) / DRAW_TILE;
    c->tilesY = (c->img.height + DRAW_TILE - 1) / DRAW_TILE;
    c->tileSlot = malloc(sizeof(int) * (size_t)(c->tilesX * c->tilesY));
    if (!c->tileSlot) return false;
    for (int i = 0; i < c->tilesX * c->tilesY; i++) c->tileSlot[i] = -1;
    return true;
}

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
// entries of scratch (UvEdgeCap). The islands of docs/painting.md 7.3
static bool JoinUvEdges(const Vector2 *uv, int n, int *parent, EdgeKey *table, int cap)
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
    return true;
}

// The UV islands of the canvas's model slot (docs/painting.md 7.3): triangles joined where they
// share an edge with the same UVs, each texel labelled with the island of a triangle covering it
// (sharedTexel where several islands do)
static bool BuildIslands(Canvas *c)
{
    free(c->island);
    free(c->triIsland);
    free(c->sharedTexel);
    free(c->islandAll);
    c->island = c->triIsland = NULL;
    c->sharedTexel = c->islandAll = NULL;
    c->triCount = 0;
    if (c->file < 0) return true;
    int n = MS_ModelSlotTriangleCount(scene, c->file, c->slot);
    int W = c->img.width, H = c->img.height;
    Vector2 *uv = malloc(sizeof(Vector2) * 3 * (size_t)(n > 0 ? n : 1));
    int *parent = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    int cap = UvEdgeCap(n);
    EdgeKey *table = malloc(sizeof(EdgeKey) * (size_t)cap);
    c->island = malloc(sizeof(int) * (size_t)W * (size_t)H);
    c->sharedTexel = calloc((size_t)W * (size_t)H, 1);
    c->triIsland = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    bool ok = uv && parent && table && c->island && c->sharedTexel && c->triIsland;
    for (int k = 0; ok && k < n; k++) {
        Vector3 pos[3];
        if (!MS_ModelSlotTriangle(scene, c->file, c->slot, k, &uv[3 * k], pos))
            for (int v = 0; v < 3; v++) uv[3 * k + v] = (Vector2){ 0 };
        parent[k] = k;
    }
    ok = ok && JoinUvEdges(uv, n, parent, table, cap);
    // Island ids 0.. by root
    for (int k = 0; ok && k < n; k++) c->triIsland[k] = -1;
    int islands = 0;
    for (int k = 0; ok && k < n; k++) {
        int r = Root(parent, k);
        if (c->triIsland[r] < 0) c->triIsland[r] = islands++;
        c->triIsland[k] = c->triIsland[r];
    }
    for (int i = 0; ok && i < W * H; i++) c->island[i] = -1;
    free(c->islandAll);
    c->islandAll = ok ? calloc((size_t)(islands > 0 ? islands : 1), 1) : NULL;
    ok = ok && c->islandAll;
    // Rasterise conservatively: every texel the triangle touches (its centre inside it, or within
    // half a texel diagonal of an edge), in texel space, wrapped into the image; a texel two islands
    // touch is shared (both may paint it)
    for (int k = 0; ok && k < n; k++) {
        Vector2 t[3];
        for (int v = 0; v < 3; v++) t[v] = (Vector2){ uv[3 * k + v].x * W, uv[3 * k + v].y * H };
        float minx = fminf(t[0].x, fminf(t[1].x, t[2].x)), maxx = fmaxf(t[0].x, fmaxf(t[1].x, t[2].x));
        float miny = fminf(t[0].y, fminf(t[1].y, t[2].y)), maxy = fmaxf(t[0].y, fmaxf(t[1].y, t[2].y));
        int id = c->triIsland[k];
        // (a triangle over a whole texture or more, tiled UVs: its island may paint everywhere)
        if (maxx - minx >= W - 1 || maxy - miny >= H - 1) { c->islandAll[id] = 1; continue; }
        float area = (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (t[1].y - t[0].y);
        for (int y = (int)floorf(miny - 1.0f); y <= (int)floorf(maxy + 1.0f); y++)
            for (int x = (int)floorf(minx - 1.0f); x <= (int)floorf(maxx + 1.0f); x++) {
                Vector2 q = { x + 0.5f, y + 0.5f };
                float w0 = (t[1].x - q.x) * (t[2].y - q.y) - (t[2].x - q.x) * (t[1].y - q.y);
                float w1 = (t[2].x - q.x) * (t[0].y - q.y) - (t[0].x - q.x) * (t[2].y - q.y);
                float w2 = (t[0].x - q.x) * (t[1].y - q.y) - (t[1].x - q.x) * (t[0].y - q.y);
                bool in = area > 0.0f ? (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) : area < 0.0f && (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f);
                if (!in && fminf(SegmentDistance(q, t[0], t[1]), fminf(SegmentDistance(q, t[1], t[2]), SegmentDistance(q, t[2], t[0]))) > 0.7072f)
                    continue;
                size_t i = (size_t)WrapIndex(y, H, 0) * W + WrapIndex(x, W, 0);
                if (c->island[i] < 0) c->island[i] = id;
                else if (c->island[i] != id) c->sharedTexel[i] = 1;
            }
    }
    c->triCount = n;
    free(uv);
    free(parent);
    free(table);
    if (!ok) {
        free(c->island);
        free(c->triIsland);
        free(c->sharedTexel);
        free(c->islandAll);
        c->island = c->triIsland = NULL;
        c->sharedTexel = c->islandAll = NULL;
        c->triCount = 0;
    }
    return ok;
}

static void FreeCanvas(Canvas *c)
{
    free(c->rel);
    free(c->material);
    free(c->island);
    free(c->source);
    free(c->triIsland);
    free(c->sharedTexel);
    free(c->islandAll);
    free(c->tileSlot);
    FreeImage(&c->img);
    FreeImage(&c->base);
    FreeImage(&c->orm);
    FreeImage(&c->normal);
    free(c->ormRel);
    free(c->normalRel);
    free(c->bakeShader);
    free(c->bakeTextures);
    memset(c, 0, sizeof(*c));
}

// Uploads the canvas's pixels in rect to the scene (MS_TextureUpdate), or provides the whole image
// when no texture of its size shows it yet. raylib logs a line per mipmap generation: quiet here
static void Upload(Canvas *c, int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0) return;
    SetTraceLogLevel(LOG_WARNING);
    bool ok = MS_TextureUpdate(scene, c->rel, &c->img, (Rectangle){ (float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0) });
    SetTraceLogLevel(LOG_INFO);
    if (!ok && c->provided) MS_TextureProvide(scene, c->rel, &c->img);
}

// (a baked shader's ORM and normal maps go with it: the copy's material loads them live)
static void Provide(Canvas *c)
{
    c->provided = MS_TextureProvide(scene, c->rel, &c->img);
    if (c->ormRel) c->provided = MS_TextureProvide(scene, c->ormRel, &c->orm) && c->provided;
    if (c->normalRel) c->provided = MS_TextureProvide(scene, c->normalRel, &c->normal) && c->provided;
}

static void Unprovide(Canvas *c)
{
    MS_TextureProvide(scene, c->rel, NULL);
    if (c->ormRel) MS_TextureProvide(scene, c->ormRel, NULL);
    if (c->normalRel) MS_TextureProvide(scene, c->normalRel, NULL);
    c->provided = false;
}

// The frame's stamps go up (one update per canvas per frame, docs/painting.md 9.1)
static void Flush(DrawMode *dm)
{
    for (int i = 0; i < dm->canvasCount; i++) {
        Canvas *c = &dm->canvases[i];
        if (!c->dirty) continue;
        Upload(c, c->dx0, c->dy0, c->dx1, c->dy1);
        c->dirty = false;
    }
}

static void MarkDirty(Canvas *c, int x0, int y0, int x1, int y1)
{
    if (!c->dirty) { c->dx0 = x0; c->dy0 = y0; c->dx1 = x1; c->dy1 = y1; c->dirty = true; return; }
    c->dx0 = x0 < c->dx0 ? x0 : c->dx0;
    c->dy0 = y0 < c->dy0 ? y0 : c->dy0;
    c->dx1 = x1 > c->dx1 ? x1 : c->dx1;
    c->dy1 = y1 > c->dy1 ? y1 : c->dy1;
}

// #endregion

// ======================================================================================
// #region Paint steps and stamps
// ======================================================================================

// A tile of the canvas as it was before the stroke (undo / redo swap it), and the stroke's
// coverage of it (the max-coverage mask; freed when the stroke ends)
typedef struct PaintTile {
    int tx, ty, w, h;
    unsigned char *pixels, *mask;
} PaintTile;

// Where a stamp goes: the canvas's texture space (0..1, wrapped), and the region: a box face
// (its cell; -1 without a layout: the whole image) or a model's slot triangle (its island)
typedef struct {
    float u, v;
    int region;
    bool joined;            // the stroke went on straight from the sample before it (one stretch)
} Sample;

typedef enum { STEP_STROKE = 0, STEP_IMAGE } StepKind;

struct PaintStep {
    int canvas;
    StepKind kind;
    bool created;           // the step made the canvas (a first stroke): undo takes it out of the scene
    PaintTile *tiles;
    int tileCount, tileCap;
    // The stroke as input (a rebase replays it onto a PNG that changed on disk)
    PenKind pen;
    int size;
    float hardness;
    Color color;
    Sample *samples;
    int sampleCount, sampleCap;
    // STEP_IMAGE (a layout change, a resize): the canvas before (swapped on undo / redo)
    Image other, otherBase;
    bool otherLayout;
    unsigned char otherCells[6];
    bool rebind;            // no text goes with it (a resize): the material is rebuilt to load the new size
    // ... what it did, for a replay
    bool toLayout;
    unsigned char toCells[6];
    int toW, toH;
    bool toNearest;
    bool detached;          // being pushed: a failed push must not free other / otherBase (the caller puts them back)
};

static PaintStep *NewStep(DrawMode *dm, int ci, StepKind kind)
{
    PaintStep *p = calloc(1, sizeof(PaintStep));
    if (!p) return NULL;
    p->canvas = ci;
    p->kind = kind;
    p->pen = dm->pen;
    p->size = dm->size;
    p->hardness = dm->hardness;
    p->color = dm->color;
    // (a stroke's samples: room for a long drag up front, so frames rarely grow it)
    if (kind == STEP_STROKE && !MS_RESERVE(p->samples, p->sampleCap, 1024)) p->sampleCap = 0;
    dm->canvases[ci].refs++;
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

static void ReleaseStep(DrawMode *dm, PaintStep *p)
{
    if (!p) return;
    if (p->canvas >= 0 && p->canvas < dm->canvasCount) dm->canvases[p->canvas].refs--;
    FreeTiles(p);
    free(p->samples);
    if (!p->detached) {
        FreeImage(&p->other);
        FreeImage(&p->otherBase);
    }
    free(p);
}

static long long StepBytes(const PaintStep *p)
{
    long long b = (long long)sizeof(PaintStep) + (long long)p->tileCap * (long long)sizeof(PaintTile) +
                  (long long)p->sampleCap * (long long)sizeof(Sample);
    for (int i = 0; i < p->tileCount; i++) b += (long long)p->tiles[i].w * p->tiles[i].h * (p->tiles[i].mask ? 5 : 4);
    b += (long long)p->other.width * p->other.height * 4 + (long long)p->otherBase.width * p->otherBase.height * 4;
    return b;
}

// The stroke ended: the masks go, the tiles stay (the step's undo copy)
static void EndTiles(Canvas *c, PaintStep *p)
{
    for (int i = 0; i < p->tileCount; i++) {
        free(p->tiles[i].mask);
        p->tiles[i].mask = NULL;
        int ti = p->tiles[i].ty * c->tilesX + p->tiles[i].tx;
        if (c->tileSlot && ti >= 0 && ti < c->tilesX * c->tilesY) c->tileSlot[ti] = -1;
    }
}

// The step's tile of texel (x, y), copied from the canvas the first time the stroke touches it
static PaintTile *TileAt(Canvas *c, PaintStep *p, int x, int y)
{
    int tx = x / DRAW_TILE, ty = y / DRAW_TILE, ti = ty * c->tilesX + tx;
    if (!c->tileSlot) return NULL;
    if (c->tileSlot[ti] >= 0) return &p->tiles[c->tileSlot[ti]];
    int w = c->img.width - tx * DRAW_TILE, h = c->img.height - ty * DRAW_TILE;
    w = w > DRAW_TILE ? DRAW_TILE : w;
    h = h > DRAW_TILE ? DRAW_TILE : h;
    unsigned char *px = malloc((size_t)w * h * 4), *mask = calloc((size_t)w * h, 1);
    PaintTile *t = px && mask ? MS_PUSH(p->tiles, p->tileCount, p->tileCap) : NULL;
    if (!t) { free(px); free(mask); return NULL; }
    *t = (PaintTile){ tx, ty, w, h, px, mask };
    for (int r = 0; r < h; r++)
        memcpy(px + (size_t)r * w * 4, (unsigned char *)c->img.data + ((size_t)(ty * DRAW_TILE + r) * c->img.width + tx * DRAW_TILE) * 4,
               (size_t)w * 4);
    c->tileSlot[ti] = p->tileCount - 1;
    return t;
}

// One texel at coverage cov (0..1): composited from the pixel the stroke started from, only when
// cov is more than any earlier stamp of the stroke gave it
static void ApplyTexel(Canvas *c, PaintStep *p, int x, int y, float cov)
{
    PaintTile *t = TileAt(c, p, x, y);
    if (!t) return;
    int lx = x - t->tx * DRAW_TILE, ly = y - t->ty * DRAW_TILE;
    unsigned char cv = (unsigned char)ClampI((int)lroundf(cov * 255.0f), 0, 255);
    unsigned char *m = &t->mask[ly * t->w + lx];
    if (cv == 0 || cv <= *m) return;
    *m = cv;
    const unsigned char *orig = t->pixels + ((size_t)ly * t->w + lx) * 4;
    size_t at = ((size_t)y * c->img.width + x) * 4;
    unsigned char *dst = (unsigned char *)c->img.data + at;
    if (p->pen == PEN_ERASER) {
        const unsigned char *b = (const unsigned char *)c->base.data + at;
        for (int k = 0; k < 4; k++) dst[k] = (unsigned char)ClampI((int)lroundf(orig[k] + (b[k] - orig[k]) * (cv / 255.0f)), 0, 255);
    } else Over(dst, orig, p->color, cv / 255.0f * (p->color.a / 255.0f));
    MarkDirty(c, x, y, x + 1, y + 1);
}

typedef struct { int x0, y0, x1, y1, island; } Region;

static void RegionOf(const Canvas *c, int region, Region *r)
{
    r->island = -1;
    if (c->file >= 0) {
        r->x0 = r->y0 = 0;
        r->x1 = c->img.width;
        r->y1 = c->img.height;
        if (c->triIsland && region >= 0 && region < c->triCount) r->island = c->triIsland[region];
        return;
    }
    CellRect(c, region, &r->x0, &r->y0, &r->x1, &r->y1);
}

static bool InRegion(const Canvas *c, const Region *r, int x, int y)
{
    if (x < r->x0 || y < r->y0 || x >= r->x1 || y >= r->y1) return false;
    if (!c->island || (r->island >= 0 && c->islandAll && c->islandAll[r->island])) return true;
    size_t i = (size_t)y * c->img.width + x;
    return r->island >= 0 ? c->island[i] == r->island || (c->sharedTexel && c->sharedTexel[i]) : c->island[i] >= 0;
}

static float SmoothStep(float e0, float e1, float x)
{
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}

// A texel of a stamp at coverage cov: wrapped (repeat) or dropped (clamp, mirror) outside the
// image when the canvas has no layout, clipped to the region
static void StampTexel(Canvas *c, PaintStep *p, const Region *r, int x, int y, float cov)
{
    int W = c->img.width, H = c->img.height;
    if (!c->layout && (x < 0 || y < 0 || x >= W || y >= H)) {
        if (c->wrap != 0) return;       // (clamp, mirror: the stamp ends at the edge)
        x = WrapIndex(x, W, 0);
        y = WrapIndex(y, H, 0);
    }
    if (InRegion(c, r, x, y)) ApplyTexel(c, p, x, y, cov);
}

// The pixel-exact footprints (docs/painting.md 7.2) with (x0, y0) their first texel: the square pen's
// s x s texels, the round pens' one texel at size 1 and 2 x 2 at size 2
static void StampKernel(Canvas *c, PaintStep *p, const Region *r, int x0, int y0)
{
    int size = p->pen == PEN_SQUARE ? (p->size < 1 ? 1 : p->size) : p->size >= 2 ? 2 : 1;
    for (int y = y0; y < y0 + size; y++)
        for (int x = x0; x < x0 + size; x++) StampTexel(c, p, r, x, y, 1.0f);
}

// One stamp of the step's pen at sample s (docs/painting.md 7.2), clipped to its region. prev: the
// sample before it on the same stretch of the stroke (NULL: the stroke starts or jumps there). The
// round pens from size 3 sweep a capsule from prev to s at their sub-texel positions (the texels
// whose centre is closer than s / 2 to the segment), so a straight drag has straight edges and is s
// texels wide; the pixel-exact footprints (square, round 1 and 2) are stamped at every texel the
// segment passes through
static void Stamp(Canvas *c, PaintStep *p, Sample s, const Sample *prev)
{
    int W = c->img.width, H = c->img.height;
    Region r;
    RegionOf(c, s.region, &r);
    if (p->pen == PEN_FILL) {
        for (int y = r.y0; y < r.y1; y++)
            for (int x = r.x0; x < r.x1; x++)
                if (InRegion(c, &r, x, y)) ApplyTexel(c, p, x, y, 1.0f);
        return;
    }
    Vector2 b = { s.u * W, s.v * H }, a = prev ? (Vector2){ prev->u * W, prev->v * H } : b;
    int size = p->size < 1 ? 1 : p->size;
    if (p->pen == PEN_SQUARE || size <= 2) {
        // (every half texel along the segment: no texel it crosses is skipped)
        int steps = (int)ceilf(Vector2Distance(a, b) * 2.0f);
        steps = ClampI(steps, 0, 1 << 16);
        // (square: the sample's texel at floor(s / 2); round size 2: the 2 x 2 texels around the
        // sample's sub-texel position, so a drag is 2 texels wide)
        float shift = p->pen != PEN_SQUARE && size == 2 ? 0.5f : 0.0f;
        int back = p->pen == PEN_SQUARE ? size / 2 : 0;
        int lx = INT32_MIN, ly = INT32_MIN;
        for (int i = 0; i <= steps; i++) {
            float t = steps ? (float)i / steps : 1.0f;
            int cx = ClampI((int)floorf(a.x + (b.x - a.x) * t), 0, W - 1), cy = ClampI((int)floorf(a.y + (b.y - a.y) * t), 0, H - 1);
            int kx = (int)floorf(a.x + (b.x - a.x) * t - shift) - back, ky = (int)floorf(a.y + (b.y - a.y) * t - shift) - back;
            if (shift == 0.0f) { kx = cx - back; ky = cy - back; }
            if (kx == lx && ky == ly) continue;
            StampKernel(c, p, &r, kx, ky);
            lx = kx;
            ly = ky;
        }
        return;
    }
    float rad = size * 0.5f, hard = p->hardness < 0.0f ? 0.0f : p->hardness > 0.99f ? 0.99f : p->hardness;
    int x0 = (int)floorf(fminf(a.x, b.x) - rad), x1 = (int)ceilf(fmaxf(a.x, b.x) + rad);
    int y0 = (int)floorf(fminf(a.y, b.y) - rad), y1 = (int)ceilf(fmaxf(a.y, b.y) + rad);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float d = SegmentDistance((Vector2){ x + 0.5f, y + 0.5f }, a, b);
            if (d >= rad) continue;
            float cov = p->pen == PEN_SOFT ? 1.0f - SmoothStep(hard, 1.0f, d / rad) : 1.0f;
            StampTexel(c, p, &r, x, y, cov);
        }
}

static bool RecordSample(PaintStep *p, Sample s)
{
    Sample *slot = MS_PUSH(p->samples, p->sampleCount, p->sampleCap);
    if (slot) *slot = s;
    return slot != NULL;
}

// #endregion

// ======================================================================================
// #region Targets: what the cursor is over, opening and making canvases
// ======================================================================================

#define UV_HIT_K (-2)

// The canvas sample of a surface hit on canvas ci (false: the hit isn't on it)
static bool HitSample(DrawMode *dm, int ci, const MS_SurfaceHit *h, Sample *s)
{
    if (ci < 0 || h->hit.obj.kind == MS_OBJ_NONE) return false;
    RefreshMats(dm);
    int m = h->material;
    if (m < 0 || m >= dm->matCount || dm->mats[m].canvas != ci) return false;
    const Canvas *c = &dm->canvases[ci];
    if (h->hit.obj.kind == MS_OBJ_MODEL) {
        if (c->file < 0 || scene->models[h->hit.obj.index].file != c->file) return false;
        // (mesh UV_HIT_K: a --paint-script `uv` hit, triangle is the slot triangle k already)
        s->region = h->mesh == UV_HIT_K ? h->triangle : MS_ModelSlotTriangleIndex(scene, c->file, h->mesh, h->triangle);
    } else {
        if (c->file >= 0) return false;
        s->region = c->layout ? h->face : -1;
    }
    s->u = WrapCoord(h->uv.x, c->wrap);
    s->v = WrapCoord(h->uv.y, c->wrap);
    return true;
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

// A texture of the package as RGBA8 (the scene's GPU copy when it has one, else the file)
static Image ReadPackageTexture(const char *rel)
{
    Texture2D t = MS_TextureFind(scene, rel);
    Image img = { 0 };
    if (t.id && MS_TextureRead(t, &img)) return AdoptImage(img);
    char path[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", scene->root, rel);
    return FileExists(path) ? AdoptImage(LoadImage(path)) : (Image){ 0 };
}

typedef struct {
    int canvas;             // a canvas to paint (open)
    bool create;            // a first stroke: the object's own material and canvas
    bool sharedModel;       // ... in shared mode: a model slot's own texture for every instance (3.4)
    bool bake;              // ... a box whose material has a surface shader: baked into the canvas (3.6)
    bool open;              // a texture to open as a canvas (rel)
    const char *rel;
    int material;
    char why[200];          // refused ("" = it can be painted)
} Target;

static void ResolveTarget(DrawMode *dm, const MS_SurfaceHit *h, Target *t)
{
    memset(t, 0, sizeof(*t));
    t->canvas = -1;
    t->material = h->material;
    if (h->hit.obj.kind == MS_OBJ_NONE) { snprintf(t->why, sizeof(t->why), "nothing under the cursor"); return; }
    RefreshMats(dm);
    int m = h->material;
    bool model = h->hit.obj.kind == MS_OBJ_MODEL;
    if (m >= dm->matCount) { snprintf(t->why, sizeof(t->why), "the map changed: wait a frame"); return; }
    if (m >= 0 && scene->materials[m].shader) {
        // A box gets the shader baked into its own canvas (3.6); a model slot can't (3.5), and there
        // is no texture to share. R3D draws a blended or unlit material in the forward pass, which
        // has no G-buffer view to bake from
        const R3D_Material *R = &scene->materials[m].material;
        const char *name = scene->materials[m].name;
        if (model) snprintf(t->why, sizeof(t->why), "material %s has a surface shader: a model slot with one can't be painted", name);
        else if (dm->shared)
            snprintf(t->why, sizeof(t->why), "shared: material %s has a surface shader, no texture to share (H off bakes it into the box's own canvas)", name);
        else if (R->transparencyMode == R3D_TRANSPARENCY_BLEND || R->unlit)
            snprintf(t->why, sizeof(t->why), "material %s is %s: its shader can't be baked (no G-buffer view of it)", name,
                     R->unlit ? "unlit" : "transparent");
        else t->create = t->bake = true;
        return;
    }
    if (model) {
        int file = scene->models[h->hit.obj.index].file;
        Vector2 uv[3];
        Vector3 pos[3];
        if (h->slot < 0 || MS_ModelSlotTriangleCount(scene, file, h->slot) == 0 || !MS_ModelSlotTriangle(scene, file, h->slot, 0, uv, pos)) {
            snprintf(t->why, sizeof(t->why), "this model has no texture coordinates");
            return;
        }
    }
    const char *am = m >= 0 ? dm->mats[m].albedoMap : NULL;
    if (am && dm->mats[m].canvas >= 0 && !dm->canvases[dm->mats[m].canvas].stale) { t->canvas = dm->mats[m].canvas; return; }
    if (am && (StartsWith(am, PAINT_DIR) || dm->shared)) {
        if (dm->mats[m].dataSampled) { snprintf(t->why, sizeof(t->why), "%s is a data_sampler texture (data, not colour)", am); return; }
        if (!IsFileExtension(am, ".png")) { snprintf(t->why, sizeof(t->why), "only PNG textures are painted in place (%s)", am); return; }
        t->open = true;
        t->rel = am;
        return;
    }
    if (dm->shared && m >= 0) {
        snprintf(t->why, sizeof(t->why), "shared: material %s has no texture to paint (H: its own canvas instead)", scene->materials[m].name);
        return;
    }
    t->sharedModel = dm->shared;    // (a model slot drawn with its own material)
    t->create = true;
}

// The objects of the hit object's statement copy (a stairs' steps, a model), heap
static int CopyObjects(MS_ObjRef obj, MS_ObjRef **out, int *copiesOut)
{
    *out = NULL;
    *copiesOut = 0;
    int e = MS_ObjectEntity(scene, obj);
    if (e < 0) return 0;
    const MS_Source *src = &scene->entitySources[e];
    int n = 0, cap = 0, copies = 0;
    for (int k = 0; k < scene->entityCount; k++) {
        const MS_Source *s = &scene->entitySources[k];
        if (s->prim != src->prim) continue;
        copies = s->copy + 1 > copies ? s->copy + 1 : copies;
        if (s->copy != src->copy) continue;
        MS_ObjRef *slot = MS_PUSH(*out, n, cap);
        if (slot) *slot = scene->entityObjects[k];
    }
    *copiesOut = copies;
    return n;
}

static bool MaterialNameUsed(const DrawMode *dm, const char *name, const char *rel)
{
    char path[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", scene->root, rel);
    return MaterialIndex(name, (int)strlen(name)) >= 0 || CanvasByRel(dm, rel) >= 0 || FileExists(path);
}

// The keys of the source material that the copy keeps (docs/painting.md 3.3): everything but its
// colour, maps of the albedo, tiling and layout (the copy gets its own); `inherit albedo` /
// `inherit all` become the other three
static void CopyKeys(const char *keys, bool dropFilter, const char *const *also, int alsoCount, Sb *out)
{
    Toks T = { 0 };
    if (!LineToks(keys, (int)strlen(keys), &T)) { free(T.t); return; }
    for (int k = 0; k < T.n; ) {
        int e = KeyEnd(&T, k);
        const Tok *v = k + 1 < T.n ? &T.t[k + 1] : NULL;
        bool drop = TokIs(&T.t[k], "albedo") || TokIs(&T.t[k], "albedo_map") || TokIs(&T.t[k], "uv_scale") ||
                    TokIs(&T.t[k], "uv_offset") || TokIs(&T.t[k], "uv_faces") || (dropFilter && TokIs(&T.t[k], "filter"));
        for (int a = 0; a < alsoCount && !drop; a++) drop = TokIs(&T.t[k], also[a]);
        if (TokIs(&T.t[k], "inherit") && v && (TokIs(v, "albedo") || TokIs(v, "all"))) {
            drop = true;
            if (TokIs(v, "all")) SbStr(out, out->len ? " inherit normal inherit orm inherit emission" : "inherit normal inherit orm inherit emission");
        }
        if (!drop)
            for (int i = k; i < e; i++) {
                if (out->len) SbStr(out, " ");
                SbAdd(out, T.t[i].s, T.t[i].n);
            }
        k = e;
    }
    free(T.t);
}

static bool KeysInheritAlbedo(const char *keys)
{
    Toks T = { 0 };
    bool yes = false;
    if (LineToks(keys, (int)strlen(keys), &T))
        for (int k = 0; k + 1 < T.n; k = KeyEnd(&T, k))
            if (TokIs(&T.t[k], "inherit") && (TokIs(&T.t[k + 1], "albedo") || TokIs(&T.t[k + 1], "all"))) yes = true;
    free(T.t);
    return yes;
}

// A model slot drawn with its own material: the scalar keys of its R3D_Material the format has
static void SlotKeys(const R3D_Material *R, Sb *out)
{
    static const char *const CULL[] = { "none", "back", "front" };
    static const char *const TR[] = { "off", "prepass", "alpha" };
    static const char *const BL[] = { "alpha", "additive", "add_alpha", "multiply", "premultiplied" };
    SbStr(out, "inherit normal inherit orm inherit emission");
    SbFmt(out, " roughness %g metalness %g occlusion %g normal_scale %g", R->orm.roughness, R->orm.metalness, R->orm.occlusion,
          R->normal.scale);
    SbFmt(out, " emission %d %d %d emission_energy %g", R->emission.color.r, R->emission.color.g, R->emission.color.b,
          R->emission.energy);
    if ((int)R->cullMode >= 0 && (int)R->cullMode < 3) SbFmt(out, " cull %s", CULL[R->cullMode]);
    if ((int)R->transparencyMode >= 0 && (int)R->transparencyMode < 3) SbFmt(out, " transparency %s", TR[R->transparencyMode]);
    if ((int)R->blendMode >= 0 && (int)R->blendMode < 5) SbFmt(out, " blend %s", BL[R->blendMode]);
    SbFmt(out, " alpha_cutoff %g unlit %s", R->alphaCutoff, R->unlit ? "on" : "off");
}

// The size side of a new box canvas's cell (docs/painting.md 5.1): L the longest edge of the boxes
static int CellSide(float density, const MS_ObjRef *objs, int n, int cols, int rows)
{
    float L = 0.0f;
    for (int i = 0; i < n; i++) {
        if (objs[i].kind != MS_OBJ_BOX) continue;
        int b = objs[i].index;
        Vector3 s = scene->boxTurned && scene->boxTurned[b] >= 0 ? scene->turnedBoxes[scene->boxTurned[b]].size
                  : Vector3Subtract(scene->boxes[b].max, scene->boxes[b].min);
        L = fmaxf(L, fmaxf(s.x, fmaxf(s.y, s.z)));
    }
    int maxCell = DRAW_CANVAS_MAX / (cols > rows ? cols : rows);
    return ClampI(Pow2Ceil(density * L), DRAW_CANVAS_MIN, maxCell);
}

// A model slot's canvas side: the slot's world area in this instance
static int SlotSide(float density, int model, int slot)
{
    const MS_ModelInstance *M = &scene->models[model];
    int n = MS_ModelSlotTriangleCount(scene, M->file, slot);
    double area = 0.0;
    for (int k = 0; k < n; k++) {
        Vector2 uv[3];
        Vector3 p[3];
        if (!MS_ModelSlotTriangle(scene, M->file, slot, k, uv, p)) continue;
        for (int v = 0; v < 3; v++) p[v] = Vector3Transform(p[v], M->transform);
        area += 0.5 * Vector3Length(Vector3CrossProduct(Vector3Subtract(p[1], p[0]), Vector3Subtract(p[2], p[0])));
    }
    return ClampI(Pow2Ceil(density * (float)sqrt(area)), DRAW_CANVAS_MIN, DRAW_CANVAS_MAX);
}

// A source pixel for canvas texture coordinate (u, v) of the surface (before the copy's own
// tiling): the source texture where the surface sampled it, times the tint
static void SourceColor(const Image *src, Vector2 uv, Vector2 scale, Vector2 off, int wrap, Color tint, unsigned char out[4])
{
    float s[4] = { 255.0f, 255.0f, 255.0f, 255.0f };
    if (src && src->data) SampleBilinear(src, off.x + uv.x * scale.x, off.y + uv.y * scale.y, wrap, s);
    float t[4] = { tint.r, tint.g, tint.b, tint.a };
    for (int c = 0; c < 4; c++) out[c] = (unsigned char)ClampI((int)lroundf(s[c] * t[c] / 255.0f), 0, 255);
}

static bool PushCanvas(DrawMode *dm, Canvas *c, int *ci)
{
    Canvas *slot = MS_PUSH(dm->canvases, dm->canvasCount, dm->canvasCap);
    if (!slot) return false;
    *slot = *c;
    *ci = dm->canvasCount - 1;
    return true;
}

// The first stroke on an object (docs/painting.md 3.1): its own material and canvas, one undoable
// edit (EDIT_PAINT_MATERIAL with the canvas's step). The canvas index, -1 with why
// Every instance of model file `file` that draws slot `slot` with the file's own material, heap
static int OwnSlotInstances(int file, int slot, MS_ObjRef **out)
{
    *out = NULL;
    int n = 0, cap = 0;
    for (int i = 0; i < scene->modelCount; i++) {
        if (scene->models[i].file != file || SlotMaterial(&scene->models[i], slot) >= 0) continue;
        MS_ObjRef *s = MS_PUSH(*out, n, cap);
        if (s) *s = (MS_ObjRef){ MS_OBJ_MODEL, i };
    }
    return n;
}

// sharedModel: shared mode on a model slot drawn with its own material (docs/painting.md 3.4): the
// slot's texture is extracted to textures/paint/<file>_s<N>.png, and every instance of the file that
// draws the slot with its own material gets material.N of paint_<file>_s<N>
// The textures a shader material's samplers read (its keys `sampler` / `data_sampler <name>
// "<path>"`), each once, one per line into *out (heap; NULL: none). False with why when one of them
// has no licence row in ASSETS.md: the bake derives from it (docs/painting.md 3.6)
static bool BakeTextures(const char *keys, char **out, char *why, int whySize)
{
    *out = NULL;
    Toks T = { 0 };
    Sb list = { 0 };
    bool ok = true;
    if (keys && LineToks(keys, (int)strlen(keys), &T))
        for (int k = 0; k < T.n && ok; k = KeyEnd(&T, k)) {
            if (!(TokIs(&T.t[k], "sampler") || TokIs(&T.t[k], "data_sampler")) || k + 2 >= T.n || !T.t[k + 2].q) continue;
            char path[MS_PATH_MAX];
            snprintf(path, sizeof(path), "%.*s", T.t[k + 2].n - 2, T.t[k + 2].s + 1);
            char line[MS_PATH_MAX + 2];
            snprintf(line, sizeof(line), "%s\n", path);
            if (list.s && (strncmp(list.s, line, strlen(line)) == 0 || strstr(list.s, TextFormat("\n%s", line)))) continue;
            char *row = AssetsRow(path);
            if (!row) {
                snprintf(why, (size_t)whySize, "%.200s (a sampler of the shader) has no licence row in ASSETS.md: it can't be baked", path);
                ok = false;
            }
            free(row);
            SbStr(&list, line);
        }
    free(T.t);
    if (!ok || list.oom) { free(list.s); if (ok) snprintf(why, (size_t)whySize, "out of memory"); return false; }
    *out = list.s;
    return true;
}

// The material's alpha_cutoff (its last one) is 0: a bake with holes needs one to cut them
static bool CutoffZero(const char *keys)
{
    Toks T = { 0 };
    bool zero = false;
    if (keys && LineToks(keys, (int)strlen(keys), &T))
        for (int k = 0; k + 1 < T.n; k = KeyEnd(&T, k))
            if (TokIs(&T.t[k], "alpha_cutoff") && TokNumber(&T.t[k + 1])) zero = strtod(T.t[k + 1].s, NULL) <= 0.0;
    free(T.t);
    return zero;
}

// The paths of a bake's ORM and normal maps beside canvas paint_<name>.png
static void BakeRels(const char *name, char *orm, char *normal, int size)
{
    snprintf(orm, (size_t)size, PAINT_DIR "%s_orm.png", name);
    snprintf(normal, (size_t)size, PAINT_DIR "%s_normal.png", name);
}

static bool PackageFileExists(const char *rel)
{
    char path[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", scene->root, rel);
    return FileExists(path);
}

// Renders the bake of box `box` for a W x H canvas (all faces separate, as a first stroke lays it
// out) into dm->bake
static bool BakeInto(DrawMode *dm, int box, int W, int H, char *why, int whySize)
{
    if (!dm->bake && !(dm->bake = calloc(1, sizeof(SurfaceBake)))) { snprintf(why, (size_t)whySize, "out of memory"); return false; }
    SurfaceBakeFree(dm->bake);
    static const unsigned char CELLS[6] = { 0, 1, 2, 3, 4, 5 };
    if (!SurfaceBakeBox(scene, box, CELLS, W, H, dm->bake, why, whySize)) return false;
    int m = scene->boxRefs[box].material;
    TraceLog(LOG_INFO, "monolith: draw: baked the shader of material %s (box %d) into %dx%d in %.1f ms%s%s", scene->materials[m].name, box, W,
             H, dm->bake->ms, dm->bake->holes ? ", with holes" : "", dm->bake->normal.data ? ", with a normal map" : "");
    dm->bakeW = W;
    dm->bakeH = H;
    return true;
}

static void DropBake(DrawMode *dm)
{
    dm->bakeWant = dm->bakeReady = false;
    if (dm->bake) SurfaceBakeFree(dm->bake);
}

// The bake for a first stroke at h on a W x H canvas: the one DrawModeRunBake made for it, or made
// now (a script, between its frames). NULL with why
static SurfaceBake *TakeBake(DrawMode *dm, const MS_SurfaceHit *h, int W, int H, char *why, int whySize)
{
    if (dm->bakeReady && dm->bake && dm->bakeHit.hit.obj.kind == MS_OBJ_BOX && dm->bakeHit.hit.obj.index == h->hit.obj.index &&
        dm->bakeVersion == scene->version && dm->bakeW == W && dm->bakeH == H)
        return dm->bake;
    if (!dm->bakeNow) { snprintf(why, (size_t)whySize, "the shader's bake is not ready (click again)"); return NULL; }
    return BakeInto(dm, h->hit.obj.index, W, H, why, whySize) ? dm->bake : NULL;
}

static int CreateOwnCanvas(DrawMode *dm, EditMode *em, const MS_SurfaceHit *h, bool sharedModel, char *why, int whySize)
{
    MS_ObjRef obj = h->hit.obj;
    bool model = obj.kind == MS_OBJ_MODEL;
    // (a box with a surface-shader material: the shader baked into the canvas, docs/painting.md 3.6)
    bool bake = !model && !sharedModel && h->material >= 0 && h->material < scene->materialCount && scene->materials[h->material].shader;
    MS_ObjRef *objs;
    int copies, n = sharedModel ? OwnSlotInstances(scene->models[obj.index].file, h->slot, &objs) : CopyObjects(obj, &objs, &copies);
    int e = MS_ObjectEntity(scene, obj);
    if (n == 0 || e < 0) { free(objs); snprintf(why, whySize, "this object doesn't come from a map line"); return -1; }
    MS_EntityId eid = scene->entityIds[e];
    int idNum = sharedModel ? 0 : MS_ENTITY_EXPLICIT(eid) && copies == 1 ? (int)eid.stmt : MapEditNextId();
    char base[96], name[120], rel[200];
    if (sharedModel) {
        // (the file's name, kept to letters, digits, - and _)
        char file[64];
        snprintf(file, sizeof(file), "%s", GetFileNameWithoutExt(scene->modelFiles[scene->models[obj.index].file].path));
        for (char *c = file; *c; c++) if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') *c = '_';
        snprintf(base, sizeof(base), "paint_%s_s%d", file, h->slot);
    } else if (model) snprintf(base, sizeof(base), "paint_%d_s%d", idNum, h->slot);
    else snprintf(base, sizeof(base), "paint_%d", idNum);
    for (int k = 1;; k++) {
        if (k == 1) snprintf(name, sizeof(name), "%s", base);
        else snprintf(name, sizeof(name), "%s-%d", base, k);
        // (shared: textures/paint/<file>_s<N>.png, without the paint_ of the material's name)
        snprintf(rel, sizeof(rel), PAINT_DIR "%s.png", sharedModel ? name + 6 : name);
        char ormRel[200], normalRel[200];
        BakeRels(name, ormRel, normalRel, (int)sizeof(ormRel));
        if (!MaterialNameUsed(dm, name, rel) && !(bake && (PackageFileExists(ormRel) || PackageFileExists(normalRel)))) break;
    }
    // The source: material m (or the slot's own), its tint, tiling, wrap and texture
    int m = h->material, slot = h->slot;
    const MS_ModelFile *F = model ? &scene->modelFiles[scene->models[obj.index].file] : NULL;
    const R3D_Material *R = m >= 0 ? &scene->materials[m].material
                          : F && slot >= 0 && slot < F->model.materialCount && F->model.materials ? &F->model.materials[slot] : NULL;
    Color tint = R ? R->albedo.color : WHITE;
    Vector2 scale = R ? R->uvScale : (Vector2){ 1, 1 }, off = R ? R->uvOffset : (Vector2){ 0, 0 };
    int wrap = m >= 0 ? dm->mats[m].wrap : 0;
    Sb srcKeys = { 0 };
    if (m >= 0) {
        if (m == 0) SbStr(&srcKeys, "roughness 0.9 metalness 0");    // (the built-in "default")
        int size;
        const char *text = MS_MapText(scene, &size);
        if (text) ForMaterialLines(text, size, scene->materials[m].name, AddKeys, &srcKeys);
    }
    bool inheritAlbedo = m >= 0 && model && srcKeys.s && KeysInheritAlbedo(srcKeys.s);
    Image src = { 0 };
    const char *srcPath = NULL;
    if (!bake && m >= 0 && dm->mats[m].albedoMap) {
        srcPath = dm->mats[m].albedoMap;
        src = ReadPackageTexture(srcPath);
    } else if (model && (m < 0 || inheritAlbedo)) {
        Texture2D t = MS_ModelSlotTexture(scene, scene->models[obj.index].file, slot);
        Image img;
        if (t.id && MS_TextureRead(t, &img)) {
            src = AdoptImage(img);
            srcPath = F->path;
        }
        if (inheritAlbedo && F->model.materials && slot < F->model.materialCount) {
            const R3D_Material *own = &F->model.materials[slot];
            tint = own->albedo.color;
            scale = own->uvScale;
            off = own->uvOffset;
        }
    }
    char *row = NULL, *bakeTex = NULL;
    bool holes = false;
    int ci = -1;
    Canvas c = { 0 };
    Sb keys = { 0 };
    if (sharedModel && !src.data) {
        snprintf(why, whySize, "shared: this slot has no texture of its own (H off gives the object its own canvas)");
        goto fail;
    }
    if (srcPath && !(row = AssetsRow(srcPath))) {
        snprintf(why, whySize, "%s has no licence row in ASSETS.md", srcPath);
        goto fail;
    }
    if (bake && !BakeTextures(srcKeys.s, &bakeTex, why, whySize)) goto fail;
    // Size, layout and pixels (docs/painting.md 3.3, 5.1)
    c.file = c.slot = -1;
    c.wrap = wrap;
    if (!model) {
        for (int f = 0; f < 6; f++) c.cells[f] = (unsigned char)f;
        c.layout = true;
        int cols, rows;
        LayoutGrid(c.cells, &cols, &rows);
        int side = CellSide(dm->density, objs, n, cols, rows);
        if (bake) {
            // The shader as the scene draws this box, from the G-buffer (docs/painting.md 3.6)
            SurfaceBake *B = TakeBake(dm, h, cols * side, rows * side, why, whySize);
            if (!B) goto fail;
            c.img = AdoptImage(B->albedo);
            c.orm = AdoptImage(B->orm);
            c.normal = B->normal.data ? AdoptImage(B->normal) : (Image){ 0 };
            holes = B->holes;
            B->albedo = B->orm = B->normal = (Image){ 0 };
            DropBake(dm);
            if (!c.img.data || !c.orm.data) { snprintf(why, whySize, "out of memory"); goto fail; }
        } else c.img = NewImage(cols * side, rows * side);
        if (!c.img.data) { snprintf(why, whySize, "out of memory"); goto fail; }
        for (int f = 0; f < 6 && !bake; f++) {
            int x0, y0, x1, y1;
            CellRect(&c, f, &x0, &y0, &x1, &y1);
            Rectangle r = MS_FaceCellRect(c.cells, f, true);
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    Vector2 fu = { ((x + 0.5f) / c.img.width - r.x) / r.width, ((y + 0.5f) / c.img.height - r.y) / r.height };
                    fu.x = fu.x < 0.0f ? 0.0f : fu.x > 1.0f ? 1.0f : fu.x;
                    fu.y = fu.y < 0.0f ? 0.0f : fu.y > 1.0f ? 1.0f : fu.y;
                    SourceColor(&src, fu, scale, off, wrap, tint, (unsigned char *)c.img.data + ((size_t)y * c.img.width + x) * 4);
                }
        }
    } else {
        c.file = scene->models[obj.index].file;
        c.slot = slot;
        bool identity = scale.x == 1.0f && scale.y == 1.0f && off.x == 0.0f && off.y == 0.0f;
        int W, H;
        if (src.data && identity) { W = src.width; H = src.height; }
        else W = H = SlotSide(dm->density, obj.index, slot);
        c.img = NewImage(W, H);
        if (!c.img.data) { snprintf(why, whySize, "out of memory"); goto fail; }
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                unsigned char *o = (unsigned char *)c.img.data + ((size_t)y * W + x) * 4;
                if (src.data && identity) {
                    const unsigned char *s = (const unsigned char *)src.data + ((size_t)y * W + x) * 4;
                    float t[4] = { tint.r, tint.g, tint.b, tint.a };
                    for (int k = 0; k < 4; k++) o[k] = (unsigned char)ClampI((int)lroundf(s[k] * t[k] / 255.0f), 0, 255);
                } else SourceColor(&src, (Vector2){ (x + 0.5f) / W, (y + 0.5f) / H }, scale, off, wrap, tint, o);
            }
    }
    // The copy's keys (a bake: without the shader and what it reads, the ORM's scalars 1 so the map's
    // values pass through, the normal map's scale 1; a material that cut holes keeps cutting them)
    char ormRel[200], normalRel[200];
    BakeRels(name, ormRel, normalRel, (int)sizeof(ormRel));
    bool cutoff0 = bake && holes && CutoffZero(srcKeys.s);
    static const char *const BAKED[] = { "shader", "uniform", "sampler", "data_sampler", "instanced", "orm_map", "occlusion",
                                         "roughness", "metalness", "normal_map", "normal_scale", "alpha_cutoff" };
    const char *drop[12];
    int dropCount = 0;
    for (int i = 0; bake && i < 12; i++)
        if ((i != 9 && i != 10) || c.normal.data) {
            if (i != 11 || cutoff0) drop[dropCount++] = BAKED[i];
        }
    if (model && m < 0 && R) SlotKeys(R, &keys);
    else if (srcKeys.s) CopyKeys(srcKeys.s, dm->pixelLook, drop, dropCount, &keys);
    SbFmt(&keys, "%salbedo 255 255 255 albedo_map \"%s\"", keys.len ? " " : "", rel);
    if (bake) {
        SbFmt(&keys, " orm_map \"%s\" occlusion 1 roughness 1 metalness 1", ormRel);
        if (c.normal.data) SbFmt(&keys, " normal_map \"%s\" normal_scale 1", normalRel);
        if (cutoff0) SbStr(&keys, " alpha_cutoff 0.5");
    }
    if (!model) SbStr(&keys, " uv_faces 0 1 2 3 4 5");
    if (dm->pixelLook) SbStr(&keys, " filter nearest");
    c.base = CopyImage(c.img);
    c.rel = MS_StrDup(rel);
    c.material = MS_StrDup(name);
    c.source = srcPath ? MS_StrDup(srcPath) : NULL;
    if (bake) {
        c.ormRel = MS_StrDup(ormRel);
        c.normalRel = c.normal.data ? MS_StrDup(normalRel) : NULL;
        c.bakeShader = MS_StrDup(scene->materials[m].shader);
        c.bakeTextures = bakeTex;
        bakeTex = NULL;
    }
    if (keys.oom || !c.base.data || !c.rel || !c.material || (srcPath && !c.source) || !TilesInit(&c) ||
        (model && !BuildIslands(&c)) || (bake && (!c.ormRel || (c.normal.data && !c.normalRel) || !c.bakeShader))) {
        snprintf(why, whySize, "out of memory");
        goto fail;
    }
    if (!PushCanvas(dm, &c, &ci)) { snprintf(why, whySize, "out of memory"); goto fail; }
    memset(&c, 0, sizeof(c));
    Provide(&dm->canvases[ci]);
    {
        PaintStep *p = NewStep(dm, ci, STEP_STROKE);
        if (!p) { snprintf(why, whySize, "out of memory"); goto fail; }
        p->created = true;
        EditOp op = { .kind = EDIT_PAINT_MATERIAL, .matName = name, .matKeys = keys.s, .slot = model ? slot : -1 };
        MS_EntityId *ids;
        int count;
        char msg[512], what[300];
        snprintf(what, sizeof(what), "new canvas %s %dx%d", rel, dm->canvases[ci].img.width, dm->canvases[ci].img.height);
        EditResult r = MapEditPushPaint(&em->ed, objs, n, &op, p, what, &ids, &count, msg, sizeof(msg));
        free(ids);
        if (r != EDIT_DONE) {
            snprintf(why, whySize, "%.*s", whySize > 0 ? whySize - 1 : 0, msg);
            Unprovide(&dm->canvases[ci]);
            FreeCanvas(&dm->canvases[ci]);
            dm->canvasCount--;
            ci = -1;
            goto fail;
        }
        dm->stroke = p;
        dm->strokePushed = true;
        InvalidateMats(dm);
        char stats[400];
        MapEditStatsText(&em->ed.lastStats, stats, sizeof(stats));
        TraceLog(LOG_INFO, "monolith: draw: %s", msg);
        TraceLog(LOG_INFO, "monolith: live edit %s", stats);
    }
    free(objs);
    free(srcKeys.s);
    free(keys.s);
    free(row);
    FreeImage(&src);
    return ci;
fail:
    FreeCanvas(&c);
    free(objs);
    free(srcKeys.s);
    free(keys.s);
    free(row);
    free(bakeTex);
    FreeImage(&src);
    if (bake) DropBake(dm);
    return -1;
}

// A texture of the map opened as a canvas: an earlier session's textures/paint/ PNG, or in shared
// mode the material's albedo map. The pixels the file has are its base and saved state
static int OpenCanvas(DrawMode *dm, const MS_SurfaceHit *h, const char *relIn, char *why, int whySize)
{
    char *rel = MS_StrDup(relIn);       // (relIn points into mats[], which a reopen rebuilds)
    if (!rel) { snprintf(why, whySize, "out of memory"); return -1; }
    int ci = CanvasByRel(dm, rel);
    bool shared = !StartsWith(rel, PAINT_DIR);
    char *row = AssetsRow(rel);
    if (shared && !row) {
        snprintf(why, whySize, "%s has no licence row in ASSETS.md", rel);
        free(rel);
        return -1;
    }
    char path[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", scene->root, rel);
    Image img = FileExists(path) ? AdoptImage(LoadImage(path)) : (Image){ 0 };
    Texture2D t = MS_TextureFind(scene, rel);
    if (!img.data) img = ReadPackageTexture(rel);
    if (!img.data || (t.id && (t.width != img.width || t.height != img.height))) {
        snprintf(why, whySize, "%s can't be read%s", rel, img.data ? " (its size differs from the scene's texture)" : "");
        FreeImage(&img);
        free(row);
        free(rel);
        return -1;
    }
    Canvas c = { 0 };
    c.img = img;
    c.base = CopyImage(img);
    c.rel = rel;
    c.material = h->material >= 0 ? MS_StrDup(scene->materials[h->material].name) : NULL;
    c.savedHash = PixelHash(&img);
    c.shared = shared;
    c.recorded = row != NULL;
    c.wrap = h->material >= 0 ? dm->mats[h->material].wrap : 0;
    c.file = c.slot = -1;
    int cols, rows;
    c.layout = h->material >= 0 && MS_MaterialFaceLayout(scene, h->material, c.cells, &cols, &rows);
    if (h->hit.obj.kind == MS_OBJ_MODEL) {
        c.file = scene->models[h->hit.obj.index].file;
        c.slot = h->slot;
    }
    free(row);
    if (!c.base.data || !TilesInit(&c) || (c.file >= 0 && !BuildIslands(&c))) {
        FreeCanvas(&c);
        snprintf(why, whySize, "out of memory");
        return -1;
    }
    if (ci >= 0) {
        // (a stale canvas: read again, its steps are gone)
        Canvas *o = &dm->canvases[ci];
        c.refs = o->refs;
        c.recorded = c.recorded || o->recorded;
        FreeCanvas(o);
        *o = c;
    } else if (!PushCanvas(dm, &c, &ci)) {
        FreeCanvas(&c);
        snprintf(why, whySize, "out of memory");
        return -1;
    }
    Provide(&dm->canvases[ci]);
    InvalidateMats(dm);
    return ci;
}

// #endregion

// ======================================================================================
// #region Strokes: begin, samples along the cursor's path, end
// ======================================================================================

static float Spacing(const DrawMode *dm) { return fmaxf(1.0f, dm->size / 4.0f); }

typedef struct {
    Vector2 px;
    bool ok;                // on the stroke's canvas
    Sample s;
    float tx, ty;           // s in texels
} Probe;

static Probe ProbeHit(DrawMode *dm, const MS_SurfaceHit *h, Vector2 px)
{
    Probe p = { .px = px };
    p.ok = dm->strokeCanvas >= 0 && HitSample(dm, dm->strokeCanvas, h, &p.s);
    if (p.ok) {
        const Canvas *c = &dm->canvases[dm->strokeCanvas];
        p.tx = p.s.u * c->img.width;
        p.ty = p.s.v * c->img.height;
    }
    return p;
}

static MS_SurfaceHit CastAt(Camera3D cam, int w, int h, Vector2 px)
{
    Ray ray = GetScreenToWorldRayEx(px, cam, w, h);
    MS_SurfaceHit hit;
    MS_RaycastSurface(scene, ray.position, ray.direction, 0.0f, &hit);
    return hit;
}

// A stamp at the probe: joined to the stroke's sample before it when that is on the same region and
// near (the subdivision's spacing, with room for a frame cut at its depth limit), so the pen sweeps
// the segment between them
static void StampProbe(DrawMode *dm, const Probe *p)
{
    Canvas *c = &dm->canvases[dm->strokeCanvas];
    PaintStep *st = dm->stroke;
    Sample s = p->s;
    const Sample *prev = st->sampleCount > 0 ? &st->samples[st->sampleCount - 1] : NULL;
    if (prev) {
        float dx = (s.u - prev->u) * c->img.width, dy = (s.v - prev->v) * c->img.height;
        s.joined = prev->region == s.region && hypotf(dx, dy) <= 2.0f * Spacing(dm) + 1.0f;
    }
    Stamp(c, st, s, s.joined ? prev : NULL);
    RecordSample(st, s);
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
    while (n > 0) {
        Probe A = stack[n - 1].a, B = stack[n - 1].b;
        int d = stack[--n].depth;
        float dpx = Vector2Distance(A.px, B.px);
        bool close = A.ok && B.ok && A.s.region == B.s.region && hypotf(A.tx - B.tx, A.ty - B.ty) <= spacing;
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

// LMB down on hit: the canvas (made by a first stroke, or opened) and the stroke's step; false
// with the reason said. *reloaded: the text changed live (a first stroke)
static bool StrokeBegin(DrawMode *dm, EditMode *em, const MS_SurfaceHit *h, bool *reloaded)
{
    *reloaded = false;
    Target t;
    ResolveTarget(dm, h, &t);
    if (t.why[0]) { Say(dm, true, t.why); return false; }
    if (!em->ed.work && !MapEditBegin(&em->ed)) { Say(dm, true, "this map can't be painted (a compiled map?): load its .map"); return false; }
    char why[256] = "";
    int ci = t.canvas;
    dm->strokePushed = false;
    dm->stroke = NULL;
    if (t.create) {
        ci = CreateOwnCanvas(dm, em, h, t.sharedModel, why, sizeof(why));
        *reloaded = ci >= 0;
    } else if (t.open) ci = OpenCanvas(dm, h, t.rel, why, sizeof(why));
    if (ci < 0) { Say(dm, true, why[0] ? why : "can't paint here"); return false; }
    if (!dm->stroke && !(dm->stroke = NewStep(dm, ci, STEP_STROKE))) { Say(dm, true, "out of memory"); return false; }
    dm->strokeCanvas = ci;
    dm->lastCanvas = ci;
    dm->stroking = true;
    return true;
}

// LMB up: the step goes into the history (a first stroke's is there already); auto save writes
static bool StrokeEnd(DrawMode *dm, EditMode *em)
{
    if (!dm->stroking) return false;
    PaintStep *p = dm->stroke;
    Canvas *c = &dm->canvases[dm->strokeCanvas];
    Flush(dm);
    EndTiles(c, p);
    int tiles = p->tileCount;
    char what[200], msg[512] = "";
    snprintf(what, sizeof(what), "%s %d tile%s of %s", p->pen == PEN_ERASER ? "erased" : p->pen == PEN_FILL ? "filled" : "painted",
             tiles, tiles == 1 ? "" : "s", GetFileName(c->rel));
    if (!dm->strokePushed) {
        if (tiles > 0) {
            MS_EntityId *ids;
            int count;
            EditResult r = MapEditPushPaint(&em->ed, NULL, 0, NULL, p, what, &ids, &count, msg, sizeof(msg));
            free(ids);
            if (r == EDIT_DONE) Say(dm, false, msg);
            else Say(dm, true, msg);
        } else ReleaseStep(dm, p);
    } else Say(dm, false, what);
    dm->stroke = NULL;
    dm->stroking = false;
    dm->strokePushed = false;
    dm->strokeCanvas = -1;
    if (em->ed.autoSave && tiles > 0) {
        char m[512];
        if (!DrawModeSave(dm, em, m, sizeof(m))) Say(dm, true, TextFormat("auto save: %s", m));
    }
    return true;
}

// #endregion

// ======================================================================================
// #region The history's hooks (mapedit.c calls them): undo, redo, rebase, release, save
// ======================================================================================

static void SwapTiles(Canvas *c, PaintStep *p, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = *y0 = INT32_MAX;
    *x1 = *y1 = -1;
    for (int i = 0; i < p->tileCount; i++) {
        PaintTile *t = &p->tiles[i];
        for (int r = 0; r < t->h; r++) {
            unsigned char *a = t->pixels + (size_t)r * t->w * 4;
            unsigned char *b = (unsigned char *)c->img.data + ((size_t)(t->ty * DRAW_TILE + r) * c->img.width + t->tx * DRAW_TILE) * 4;
            for (int k = 0; k < t->w * 4; k++) { unsigned char v = a[k]; a[k] = b[k]; b[k] = v; }
        }
        *x0 = t->tx * DRAW_TILE < *x0 ? t->tx * DRAW_TILE : *x0;
        *y0 = t->ty * DRAW_TILE < *y0 ? t->ty * DRAW_TILE : *y0;
        *x1 = t->tx * DRAW_TILE + t->w > *x1 ? t->tx * DRAW_TILE + t->w : *x1;
        *y1 = t->ty * DRAW_TILE + t->h > *y1 ? t->ty * DRAW_TILE + t->h : *y1;
    }
}

// The material that shows a canvas loads its texture again (a canvas of a new size without a text
// change, docs/painting.md 5.3): the text without its albedo_map, then the text, both live
static bool Rebind(DrawMode *dm, Canvas *c)
{
    MapEditor *E = &dm->em->ed;
    if (!E->work) return false;
    char key[MS_PATH_MAX + 16];
    snprintf(key, sizeof(key), "albedo_map \"%s\"", c->rel);
    Sb t = { 0 };
    const char *p = E->work, *end = E->work + E->workSize;
    bool any = false;
    for (;;) {
        const char *q = p < end ? strstr(p, key) : NULL;
        if (!q || q >= end) break;
        SbAdd(&t, p, (int)(q - p));
        p = q + strlen(key);
        any = true;
    }
    SbAdd(&t, p, (int)(end - p));
    bool ok = !t.oom && any;
    char err[512];
    MS_EditStats st;
    if (ok) ok = MS_EditSetText(scene, t.s, t.len, &st, err, sizeof(err)) == MS_RELOADED;
    if (ok) ok = MS_EditSetText(scene, E->work, E->workSize, &E->lastStats, err, sizeof(err)) == MS_RELOADED;
    if (!ok && any) TraceLog(LOG_WARNING, "monolith: draw: the canvas %s could not be loaded again: %s", c->rel, err);
    free(t.s);
    InvalidateMats(dm);
    return ok;
}

// A canvas laid out anew or resized: what depends on its size
static bool CanvasResized(Canvas *c)
{
    return TilesInit(c) && (c->file < 0 || BuildIslands(c));
}

static bool HookSwap(DrawMode *dm, PaintStep *p, bool undo)
{
    if (p->canvas < 0 || p->canvas >= dm->canvasCount) return false;
    Canvas *c = &dm->canvases[p->canvas];
    if (p->kind == STEP_IMAGE) {
        Image t = c->img, tb = c->base;
        c->img = p->other;
        c->base = p->otherBase;
        p->other = t;
        p->otherBase = tb;
        bool l = c->layout;
        unsigned char cells[6];
        memcpy(cells, c->cells, 6);
        c->layout = p->otherLayout;
        memcpy(c->cells, p->otherCells, 6);
        p->otherLayout = l;
        memcpy(p->otherCells, cells, 6);
        if (!CanvasResized(c)) return false;
        Provide(c);
        Upload(c, 0, 0, c->img.width, c->img.height);     // (a cached texture of this size: fresh pixels)
        if (p->rebind) Rebind(dm, c);
        return true;
    }
    int x0, y0, x1, y1;
    SwapTiles(c, p, &x0, &y0, &x1, &y1);
    if (p->created && undo) {
        // the material that showed it goes with the text: the canvas stays for redo
        Unprovide(c);
        return true;
    }
    if (p->created) { Provide(c); return c->provided; }
    if (c->provided) MS_TextureProvide(scene, c->rel, &c->img);
    Upload(c, x0, y0, x1, y1);
    return true;
}

static bool HookUndo(void *user, PaintStep *p) { return HookSwap(user, p, true); }
static bool HookRedo(void *user, PaintStep *p) { return HookSwap(user, p, false); }
static void HookRelease(void *user, PaintStep *p) { ReleaseStep(user, p); }
static long long HookBytes(void *user, const PaintStep *p) { (void)user; return StepBytes(p); }
static bool HookSave(void *user, char *msg, int msgSize)
{
    DrawMode *dm = user;
    return DrawModeSave(dm, dm->em, msg, msgSize);
}

// Lays canvas c out as cells[] (docs/painting.md 5.2): each new cell takes the old cell of its first
// face, nearest when the cell side changed; into *img / *base (new images)
static bool Relayout(const Canvas *c, const unsigned char cells[6], Image *img, Image *base)
{
    int oc, orw, nc, nr;
    LayoutGrid(c->cells, &oc, &orw);
    LayoutGrid(cells, &nc, &nr);
    int side = c->img.width / oc;
    int maxCell = DRAW_CANVAS_MAX / (nc > nr ? nc : nr);
    side = ClampI(side, DRAW_CANVAS_MIN, maxCell);
    *img = NewImage(nc * side, nr * side);
    *base = NewImage(nc * side, nr * side);
    if (!img->data || !base->data) { FreeImage(img); FreeImage(base); return false; }
    Canvas nw = { .img = *img, .layout = true };
    memcpy(nw.cells, cells, 6);
    bool done[6] = { false };
    for (int f = 0; f < 6; f++) {
        if (done[cells[f]]) continue;
        done[cells[f]] = true;
        int a0, b0, a1, b1, x0, y0, x1, y1;
        CellRect(c, f, &a0, &b0, &a1, &b1);
        CellRect(&nw, f, &x0, &y0, &x1, &y1);
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                int sx = a0 + (int)((long long)(x - x0) * (a1 - a0) / (x1 - x0));
                int sy = b0 + (int)((long long)(y - y0) * (b1 - b0) / (y1 - y0));
                size_t d = ((size_t)y * img->width + x) * 4, s = ((size_t)sy * c->img.width + sx) * 4;
                memcpy((unsigned char *)img->data + d, (const unsigned char *)c->img.data + s, 4);
                memcpy((unsigned char *)base->data + d, (const unsigned char *)c->base.data + s, 4);
            }
    }
    return true;
}

// The image resampled to w x h: nearest or bilinear
static Image Resampled(const Image *src, int w, int h, bool nearest, int wrap)
{
    Image img = NewImage(w, h);
    for (int y = 0; img.data && y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char *o = (unsigned char *)img.data + ((size_t)y * w + x) * 4;
            if (nearest) {
                int sx = (int)((long long)x * src->width / w), sy = (int)((long long)y * src->height / h);
                memcpy(o, (const unsigned char *)src->data + ((size_t)sy * src->width + sx) * 4, 4);
            } else {
                float s[4];
                SampleBilinear(src, (x + 0.5f) / w, (y + 0.5f) / h, wrap == 0 ? 1 : wrap, s);
                for (int k = 0; k < 4; k++) o[k] = (unsigned char)ClampI((int)lroundf(s[k]), 0, 255);
            }
        }
    return img;
}

// A replay (rebase onto a changed PNG): the step done again on the canvas as it is now; its undo
// copy (tiles, the image before) taken anew
static bool ReplayStep(DrawMode *dm, PaintStep *p)
{
    Canvas *c = &dm->canvases[p->canvas];
    if (p->kind == STEP_IMAGE) {
        Image img, base;
        if (p->toLayout && memcmp(p->toCells, c->cells, 6) != 0 && c->layout) {
            if (!Relayout(c, p->toCells, &img, &base)) return false;
        } else {
            img = Resampled(&c->img, p->toW, p->toH, p->toNearest, c->wrap);
            base = Resampled(&c->base, p->toW, p->toH, false, c->wrap);
        }
        if (!img.data || !base.data) { FreeImage(&img); FreeImage(&base); return false; }
        FreeImage(&p->other);
        FreeImage(&p->otherBase);
        p->other = c->img;
        p->otherBase = c->base;
        p->otherLayout = c->layout;
        memcpy(p->otherCells, c->cells, 6);
        c->img = img;
        c->base = base;
        if (p->toLayout) { c->layout = true; memcpy(c->cells, p->toCells, 6); }
        return CanvasResized(c);
    }
    FreeTiles(p);
    for (int i = 0; i < p->sampleCount; i++) Stamp(c, p, p->samples[i], i > 0 && p->samples[i].joined ? &p->samples[i - 1] : NULL);
    EndTiles(c, p);
    c->dirty = false;
    return true;
}

static bool HookRebase(void *user, PaintStep *p, char *msg, int msgSize)
{
    DrawMode *dm = user;
    if (p->canvas < 0 || p->canvas >= dm->canvasCount) { snprintf(msg, msgSize, "its canvas is gone"); return false; }
    Canvas *c = &dm->canvases[p->canvas];
    if (c->rebaseGen != dm->rebaseGen) {
        // The first step of this canvas in this rebase: is its PNG still what it was?
        c->rebaseGen = dm->rebaseGen;
        c->replaying = false;
        c->replayed = 0;
        char path[MS_PATH_MAX + MS_PATH_MAX];
        snprintf(path, sizeof(path), "%s%s", scene->root, c->rel);
        Image disk = FileExists(path) ? AdoptImage(LoadImage(path)) : (Image){ 0 };
        if (disk.data && PixelHash(&disk) != c->savedHash) {
            Image base = CopyImage(disk);
            if (!base.data) { FreeImage(&disk); snprintf(msg, msgSize, "out of memory"); return false; }
            FreeImage(&c->img);
            FreeImage(&c->base);
            c->img = disk;
            c->base = base;
            c->savedHash = PixelHash(&disk);
            c->replaying = true;
            if (!CanvasResized(c)) { snprintf(msg, msgSize, "out of memory"); return false; }
        } else FreeImage(&disk);
    }
    // (a step that made the canvas makes its material again; any other needs it in the text)
    if (!p->created) {
        InvalidateMats(dm);
        if (!CanvasLive(dm, p->canvas)) { snprintf(msg, msgSize, "no material shows %s any more", c->rel); return false; }
    }
    if (c->replaying) {
        if (!ReplayStep(dm, p)) { snprintf(msg, msgSize, "out of memory"); return false; }
        c->replayed++;
    }
    Provide(c);
    Upload(c, 0, 0, c->img.width, c->img.height);
    return true;
}

// #endregion

// ======================================================================================
// #region Saving: PNGs, licence records (docs/painting.md 11)
// ======================================================================================

static bool CanvasUnsaved(const Canvas *c) { return c->img.data && PixelHash(&c->img) != c->savedHash; }

static void Today(char *out, int size)
{
    time_t now = time(NULL);
    const struct tm *tm = localtime(&now);
    if (tm) strftime(out, (size_t)size, "%Y-%m-%d", tm);
    else snprintf(out, (size_t)size, "-");
}

// The cells of an ASSETS.md row after its file cell: source, author, licence, date, sha256, changes
static void RowCells(const char *row, char cells[6][512])
{
    for (int i = 0; i < 6; i++) cells[i][0] = '\0';
    const char *p = row;
    for (int i = 0; i < 6 && p && *p; i++) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && !(*e == '|' && e[-1] != '\\')) e++;
        const char *t = e;
        while (t > p && t[-1] == ' ') t--;
        snprintf(cells[i], 512, "%.*s", (int)(t - p), p);
        p = *e ? e + 1 : e;
    }
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

// The rows and sidecars of a baked shader's three PNGs (docs/painting.md 3.6): derived from the
// shader file (its ASSETS.md row if it has one, else made in monolith, CC0 like a new canvas) and
// from the textures its samplers read (their rows' authors and licences, each kept)
static bool RecordBaked(Canvas *c)
{
    char path[MS_PATH_MAX + 32], date[32], pkg[256];
    snprintf(path, sizeof(path), "%sASSETS.md", scene->root);
    Today(date, sizeof(date));
    snprintf(pkg, sizeof(pkg), "%s", GetFileName(TextFormat("%.*s", (int)strlen(scene->root) - 1, scene->root)));
    // What it derives from: "`shaders/x.glsl` (made in monolith) and `textures/y.jpg` (<its source>)",
    // the authors and the licences (each once)
    Sb from = { 0 }, authors = { 0 }, licences = { 0 };
    char cells[6][512];
    char *shaderRow = AssetsRow(c->bakeShader);
    if (shaderRow) {
        RowCells(shaderRow, cells);
        SbFmt(&from, "`%s`", c->bakeShader);
        SbStr(&authors, cells[1]);
        SbStr(&licences, cells[2]);
    } else {
        SbFmt(&from, "`%s` (made in monolith)", c->bakeShader);
        SbFmt(&authors, "%s", pkg);
        SbStr(&licences, "CC0 1.0, https://creativecommons.org/publicdomain/zero/1.0/");
    }
    free(shaderRow);
    for (const char *t = c->bakeTextures; t && *t; ) {
        const char *nl = strchr(t, '\n');
        int len = nl ? (int)(nl - t) : (int)strlen(t);
        char rel[MS_PATH_MAX];
        snprintf(rel, sizeof(rel), "%.*s", len, t);
        t = nl ? nl + 1 : t + len;
        char *r = AssetsRow(rel);
        if (!r) continue;           // (refused at the first stroke; removed from ASSETS.md since)
        RowCells(r, cells);
        free(r);
        SbFmt(&from, " and `%s`", rel);
        if (!authors.s || !strstr(authors.s, cells[1])) SbFmt(&authors, "; %s", cells[1]);
        if (!licences.s || !strstr(licences.s, cells[2])) SbFmt(&licences, "; %s", cells[2]);
    }
    bool ok = !from.oom && !authors.oom && !licences.oom;
    const char *rels[3] = { c->rel, c->ormRel, c->normalRel };
    static const char *const WHAT[3] = { "albedo (the canvas)", "occlusion, roughness, metalness (orm_map)", "tangent-space normals (normal_map)" };
    for (int k = 0; k < 3 && ok; k++) {
        if (!rels[k]) continue;
        const Image *img = k == 0 ? &c->img : k == 1 ? &c->orm : &c->normal;
        char key[MS_PATH_MAX + 8];
        snprintf(key, sizeof(key), "| `%s` |", rels[k]);
        Sb row = { 0 };
        SbFmt(&row, "%s baked in monolith from %s | %s | %s | %s | - | the shader's %s rendered in monolith's draw mode%s |", key,
              from.s, authors.s, licences.s, date, WHAT[k], k == 0 ? ", painted in monolith" : "");
        ok = !row.oom && PutAssetsRow(path, key, row.s, pkg);
        free(row.s);
        Sb sc = { 0 };
        SbFmt(&sc, "# %s\n\nProvenance of `%s` in this package (written by monolith's draw mode).\n\n| Field | Value |\n|---|---|\n",
              rels[k], rels[k]);
        SbFmt(&sc, "| Name | %s |\n| Package path | `%s` |\n", GetFileName(rels[k]), rels[k]);
        SbFmt(&sc, "| Source | baked in monolith from %s: the shader's %s |\n", from.s, WHAT[k]);
        SbFmt(&sc, "| Author(s) | %s |\n| Licence | %s |\n", authors.s, licences.s);
        SbFmt(&sc, "| Recorded | %s (by monolith's draw mode) |\n| AI-generated | no |\n", date);
        SbFmt(&sc, "| Size | %d x %d px, RGBA8 PNG |\n", img->width, img->height);
        SbFmt(&sc, "\n## Changes to the file\n\n- rendered from the shader (R3D's G-buffer) by monolith's draw mode\n%s",
              k == 0 ? "- painted by hand in monolith's draw mode\n" : "");
        char scPath[MS_PATH_MAX + MS_PATH_MAX];
        snprintf(scPath, sizeof(scPath), "%s%s.asset.md", scene->root, rels[k]);
        ok = ok && !sc.oom && SaveFileText(scPath, sc.s);
        free(sc.s);
    }
    free(from.s);
    free(authors.s);
    free(licences.s);
    if (ok) c->recorded = true;
    return ok;
}

// The row for canvas c (docs/painting.md 11.2) into ASSETS.md (replacing its row) and its sidecar
static bool RecordLicence(DrawMode *dm, Canvas *c)
{
    char path[MS_PATH_MAX + 32], date[32];
    snprintf(path, sizeof(path), "%sASSETS.md", scene->root);
    Today(date, sizeof(date));
    if (c->bakeShader) return RecordBaked(c);
    char key[MS_PATH_MAX + 8];
    snprintf(key, sizeof(key), "| `%s` |", c->rel);
    Sb row = { 0 };
    char pkg[256];
    snprintf(pkg, sizeof(pkg), "%s", GetFileName(TextFormat("%.*s", (int)strlen(scene->root) - 1, scene->root)));
    char src[6][512];
    char *srcRow = c->source ? AssetsRow(c->source) : NULL;
    if (c->shared) {
        // A recorded file painted in place: its row gains "modified in monolith" once
        char *own = AssetsRow(c->rel);
        if (!own) return false;
        if (strstr(own, "modified in monolith")) { free(own); return true; }
        RowCells(own, src);
        SbFmt(&row, "%s %s | %s | %s | %s | %s | %s%s |", key, src[0], src[1], src[2], src[3], src[4],
              strcmp(src[5], "none") == 0 || !src[5][0] ? "" : src[5], strcmp(src[5], "none") == 0 || !src[5][0] ? "modified in monolith" : "; modified in monolith");
        free(own);
    } else if (srcRow) {
        RowCells(srcRow, src);
        SbFmt(&row, "%s derived from `%s` (%s) | %s | %s | %s | %s | painted in monolith from `%s` |", key, c->source, src[0], src[1],
              src[2], date, src[4][0] ? src[4] : "-", c->source);
    } else
        SbFmt(&row, "%s made in monolith (painted by hand) | %s | CC0 1.0, https://creativecommons.org/publicdomain/zero/1.0/ | %s | - | none |",
              key, pkg, date);
    bool ok = !row.oom && PutAssetsRow(path, key, row.s, pkg);
    // The sidecar (new and derived files; a shared one has its own)
    if (ok && !c->shared) {
        Sb sc = { 0 };
        SbFmt(&sc, "# %s\n\nProvenance of `%s` in this package (written by monolith's draw mode).\n\n| Field | Value |\n|---|---|\n",
              c->rel, c->rel);
        SbFmt(&sc, "| Name | %s |\n| Package path | `%s` |\n", GetFileName(c->rel), c->rel);
        if (srcRow) {
            SbFmt(&sc, "| Source | derived from `%s` (%s), painted in monolith |\n", c->source, src[0]);
            SbFmt(&sc, "| Author(s) | %s |\n| Licence | %s |\n", src[1], src[2]);
        } else {
            SbStr(&sc, "| Source | made in monolith (painted by hand) |\n");
            SbFmt(&sc, "| Author(s) | %s (the package) |\n| Licence | CC0 1.0 (https://creativecommons.org/publicdomain/zero/1.0/) |\n", pkg);
        }
        SbFmt(&sc, "| Recorded | %s (by monolith's draw mode) |\n| AI-generated | no |\n", date);
        SbFmt(&sc, "| Size | %d x %d px, RGBA8 PNG |\n", c->img.width, c->img.height);
        SbStr(&sc, "\n## Changes to the file\n\n- painted by hand in monolith's draw mode\n");
        snprintf(path, sizeof(path), "%s%s.asset.md", scene->root, c->rel);
        ok = !sc.oom && SaveFileText(path, sc.s);
        free(sc.s);
    }
    free(row.s);
    free(srcRow);
    if (ok) c->recorded = true;
    (void)dm;
    return ok;
}

bool DrawModeSave(DrawMode *dm, EditMode *em, char *msg, int msgSize)
{
    MapEditor *E = &em->ed;
    if (dm->stroking) StrokeEnd(dm, em);
    if (E->work && !MapEditDiskUnchanged(E)) {
        snprintf(msg, msgSize, "not saved: %s changed on disk (a request? another editor?); it reloads and the edits are applied again",
                 scene->path);
        return false;
    }
    int written = 0, records = 0;
    char failed[256] = "";
    char dir[MS_PATH_MAX + 32];
    snprintf(dir, sizeof(dir), "%s" PAINT_DIR, scene->root);
    for (int i = 0; i < dm->canvasCount; i++) {
        Canvas *c = &dm->canvases[i];
        if (!CanvasLive(dm, i) || !CanvasUnsaved(c)) continue;
        char path[MS_PATH_MAX + MS_PATH_MAX];
        snprintf(path, sizeof(path), "%s%s", scene->root, c->rel);
        if (StartsWith(c->rel, PAINT_DIR) && !DirectoryExists(dir)) MakeDirectory(dir);
        if (!PngWrite(c->img, path)) { snprintf(failed, sizeof(failed), "cannot write %s", c->rel); continue; }
        c->savedHash = PixelHash(&c->img);
        c->written = true;
        written++;
        // (a baked shader's ORM and normal maps: written once, they are not painted)
        if (c->ormRel && !c->extrasWritten) {
            bool w = PngWrite(c->orm, TextFormat("%s%s", scene->root, c->ormRel));
            if (w && c->normalRel) w = PngWrite(c->normal, TextFormat("%s%s", scene->root, c->normalRel));
            if (w) c->extrasWritten = true;
            else snprintf(failed, sizeof(failed), "cannot write the baked maps of %s", c->rel);
        }
    }
    char m[512] = "";
    bool ok = MapEditSave(E, m, sizeof(m));
    for (int i = 0; ok && i < dm->canvasCount; i++) {
        Canvas *c = &dm->canvases[i];
        if ((c->recorded && !c->shared) || !c->written || !CanvasLive(dm, i) || CanvasUnsaved(c)) continue;
        if (c->shared) {
            char *own = AssetsRow(c->rel);
            bool marked = own && strstr(own, "modified in monolith");
            free(own);
            if (marked) { c->recorded = true; continue; }
        } else if (c->recorded) continue;
        if (RecordLicence(dm, c)) records++;
        else snprintf(failed, sizeof(failed), "cannot record the licence of %s in ASSETS.md", c->rel);
    }
    // (the map's watch list: the PNGs as written; MapEditSave did it when the text was unsaved)
    if (ok && written > 0 && MapEditDiskUnchanged(E)) MS_EditMarkSaved(scene);
    if (!ok) snprintf(msg, msgSize, "%s%s", m, written ? TextFormat(" (%d canvas%s written)", written, written == 1 ? "" : "es") : "");
    else if (written == 0 && records == 0) snprintf(msg, msgSize, "%s", m);
    else snprintf(msg, msgSize, "%s; %d canvas%s written%s%s", StartsWith(m, "nothing to save") ? "saved" : m, written, written == 1 ? "" : "es",
                  records ? TextFormat(", %d licence record%s", records, records == 1 ? "" : "s") : "", failed[0] ? TextFormat(" (%s)", failed) : "");
    if (failed[0]) ok = false;
    TraceLog(ok ? LOG_INFO : LOG_WARNING, "monolith: %s", msg);
    return ok;
}

int DrawModeUnsavedCanvases(const DrawMode *dm)
{
    int n = 0;
    for (int i = 0; i < dm->canvasCount; i++)
        if (CanvasLive((DrawMode *)dm, i) && CanvasUnsaved(&dm->canvases[i])) n++;
    return n;
}

bool DrawModeWriteUnsaved(const DrawMode *dm, const char *dir)
{
    bool ok = true;
    for (int i = 0; i < dm->canvasCount; i++) {
        const Canvas *c = &dm->canvases[i];
        if (!CanvasLive((DrawMode *)dm, i) || !CanvasUnsaved(c)) continue;
        char path[MS_PATH_MAX * 3];
        snprintf(path, sizeof(path), "%s/%s", dir, c->rel);
        char folder[MS_PATH_MAX * 3];
        snprintf(folder, sizeof(folder), "%s", GetDirectoryPath(path));
        if (!DirectoryExists(folder)) MakeDirectory(folder);
        bool w = PngWrite(c->img, path);
        TraceLog(w ? LOG_WARNING : LOG_ERROR, "monolith: unsaved paint %s %s %s", c->rel, w ? "written to" : "could not be written to", path);
        ok = ok && w;
        for (int k = 0; k < 2; k++) {
            const char *rel = k == 0 ? c->ormRel : c->normalRel;
            if (!rel || c->extrasWritten) continue;
            snprintf(path, sizeof(path), "%s/%s", dir, rel);
            ok = PngWrite(k == 0 ? c->orm : c->normal, path) && ok;
        }
    }
    return ok;
}

// #endregion

// ======================================================================================
// #region Canvas edits: layout, resize, pixel look (docs/painting.md 5.2, 5.3, 4.2)
// ======================================================================================

static int CanvasMaterial(const Canvas *c)
{
    return c->material ? MaterialIndex(c->material, (int)strlen(c->material)) : -1;
}

// Pushes a STEP_IMAGE step whose new image is in the canvas already (provided, uploaded); a failed
// push puts the old image back. op: the text that goes with it (NULL: a resize, the material is
// loaded again)
static DrawModeResult PushImageStep(DrawMode *dm, EditMode *em, int ci, PaintStep *p, const EditOp *op, const char *what)
{
    Canvas *c = &dm->canvases[ci];
    p->detached = true;
    Image oldImg = p->other, oldBase = p->otherBase;
    bool oldLayout = p->otherLayout;
    unsigned char oldCells[6];
    memcpy(oldCells, p->otherCells, 6);
    if (!op) p->rebind = true;
    MS_EntityId *ids;
    int count;
    char msg[512];
    if (!op) Rebind(dm, c);
    EditResult r = MapEditPushPaint(&em->ed, NULL, 0, op, p, what, &ids, &count, msg, sizeof(msg));
    free(ids);
    if (r != EDIT_DONE) {
        FreeImage(&c->img);
        FreeImage(&c->base);
        c->img = oldImg;
        c->base = oldBase;
        c->layout = oldLayout;
        memcpy(c->cells, oldCells, 6);
        CanvasResized(c);
        Provide(c);
        Upload(c, 0, 0, c->img.width, c->img.height);
        if (!op) Rebind(dm, c);
        Say(dm, true, msg);
        return op || r == EDIT_DONE ? DRAWMODE_NONE : DRAWMODE_RELOADED;
    }
    p->detached = false;
    InvalidateMats(dm);
    Say(dm, false, msg);
    return DRAWMODE_RELOADED;
}

static bool OwnBoxCanvas(DrawMode *dm, int ci, const char *what)
{
    if (ci < 0 || ci >= dm->canvasCount) { Say(dm, true, TextFormat("%s: no canvas under the cursor (paint it first)", what)); return false; }
    Canvas *c = &dm->canvases[ci];
    if (c->stale || !CanvasLive(dm, ci) || CanvasMaterial(c) < 0) { Say(dm, true, TextFormat("%s: %s is not in the map any more", what, c->rel)); return false; }
    return true;
}

static DrawModeResult LayoutCanvas(DrawMode *dm, EditMode *em, int ci, const unsigned char want[6])
{
    if (!OwnBoxCanvas(dm, ci, "faces")) return DRAWMODE_NONE;
    Canvas *c = &dm->canvases[ci];
    if (!c->layout || c->file >= 0) { Say(dm, true, "faces: only a box canvas with a face layout (uv_faces) has face groups"); return DRAWMODE_NONE; }
    int cm = CanvasMaterial(c);
    RefreshMats(dm);
    if (c->ormRel || (cm >= 0 && cm < dm->matCount && dm->mats[cm].bakedMaps)) {
        // (its baked ORM / normal maps are laid out as the canvas is: they would no longer match)
        Say(dm, true, "faces: this canvas holds a baked shader, whose ORM and normal maps keep its layout");
        return DRAWMODE_NONE;
    }
    // (an unused number in between is closed up)
    unsigned char cells[6];
    int map[6] = { -1, -1, -1, -1, -1, -1 }, next = 0;
    for (int v = 0; v < 6; v++)
        for (int f = 0; f < 6; f++)
            if (want[f] == v && map[v] < 0) map[v] = next++;
    for (int f = 0; f < 6; f++) cells[f] = (unsigned char)(want[f] < 6 ? map[want[f]] : 0);
    if (memcmp(cells, c->cells, 6) == 0) { Say(dm, false, "faces: the layout is the same"); return DRAWMODE_NONE; }
    Image img, base;
    if (!Relayout(c, cells, &img, &base)) { Say(dm, true, "out of memory"); return DRAWMODE_NONE; }
    PaintStep *p = NewStep(dm, ci, STEP_IMAGE);
    if (!p) { FreeImage(&img); FreeImage(&base); Say(dm, true, "out of memory"); return DRAWMODE_NONE; }
    p->other = c->img;
    p->otherBase = c->base;
    p->otherLayout = c->layout;
    memcpy(p->otherCells, c->cells, 6);
    p->toLayout = true;
    memcpy(p->toCells, cells, 6);
    c->img = img;
    c->base = base;
    memcpy(c->cells, cells, 6);
    p->toW = c->img.width;
    p->toH = c->img.height;
    CanvasResized(c);
    Provide(c);
    Upload(c, 0, 0, c->img.width, c->img.height);
    char keys[64], what[200];
    snprintf(keys, sizeof(keys), "uv_faces %d %d %d %d %d %d", cells[0], cells[1], cells[2], cells[3], cells[4], cells[5]);
    snprintf(what, sizeof(what), "faces of %s: %s (%dx%d)", GetFileName(c->rel), keys + 9, c->img.width, c->img.height);
    EditOp op = { .kind = EDIT_MATERIAL_SET, .matName = c->material, .matKeys = keys };
    return PushImageStep(dm, em, ci, p, &op, what);
}

// The size docs/painting.md 5.1 gives the canvas for the current density
static bool DensitySize(DrawMode *dm, const Canvas *c, int *w, int *h)
{
    int m = CanvasMaterial(c);
    if (m < 0) return false;
    if (c->file < 0) {
        MS_ObjRef *objs = NULL;
        int n = 0, cap = 0;
        for (int b = 0; b < scene->boxCount; b++)
            if (scene->boxRefs[b].material == m) {
                MS_ObjRef *s = MS_PUSH(objs, n, cap);
                if (s) *s = (MS_ObjRef){ MS_OBJ_BOX, b };
            }
        int cols = 1, rows = 1;
        if (c->layout) LayoutGrid(c->cells, &cols, &rows);
        int side = CellSide(dm->density, objs, n, cols, rows);
        free(objs);
        *w = cols * side;
        *h = rows * side;
        return n > 0;
    }
    for (int i = 0; i < scene->modelCount; i++)
        if (scene->models[i].file == c->file && SlotMaterial(&scene->models[i], c->slot) == m) {
            *w = *h = SlotSide(dm->density, i, c->slot);
            return true;
        }
    return false;
}

static DrawModeResult ResizeCanvas(DrawMode *dm, EditMode *em, int ci)
{
    if (!OwnBoxCanvas(dm, ci, "resize")) return DRAWMODE_NONE;
    Canvas *c = &dm->canvases[ci];
    if (c->shared) { Say(dm, true, "resize: a texture painted in place keeps its size"); return DRAWMODE_NONE; }
    int w, h;
    if (!DensitySize(dm, c, &w, &h)) { Say(dm, true, "resize: no object uses the canvas"); return DRAWMODE_NONE; }
    if (w == c->img.width && h == c->img.height) {
        Say(dm, false, TextFormat("resize: %s is %dx%d already at %g texels/m", GetFileName(c->rel), w, h, dm->density));
        return DRAWMODE_NONE;
    }
    int m = CanvasMaterial(c);
    RefreshMats(dm);
    bool nearest = m >= 0 && m < dm->matCount && dm->mats[m].nearest;
    Image img = Resampled(&c->img, w, h, nearest, c->wrap), base = Resampled(&c->base, w, h, nearest, c->wrap);
    PaintStep *p = img.data && base.data ? NewStep(dm, ci, STEP_IMAGE) : NULL;
    if (!p) { FreeImage(&img); FreeImage(&base); Say(dm, true, "out of memory"); return DRAWMODE_NONE; }
    char what[200];
    snprintf(what, sizeof(what), "resized %s %dx%d -> %dx%d", GetFileName(c->rel), c->img.width, c->img.height, w, h);
    p->other = c->img;
    p->otherBase = c->base;
    p->otherLayout = c->layout;
    memcpy(p->otherCells, c->cells, 6);
    p->toW = w;
    p->toH = h;
    p->toNearest = nearest;
    c->img = img;
    c->base = base;
    CanvasResized(c);
    Provide(c);
    Upload(c, 0, 0, w, h);
    return PushImageStep(dm, em, ci, p, NULL, what);
}

static DrawModeResult PixelLook(DrawMode *dm, EditMode *em, int ci, bool on)
{
    if (!OwnBoxCanvas(dm, ci, "pixel look")) return DRAWMODE_NONE;
    Canvas *c = &dm->canvases[ci];
    EditOp op = { .kind = EDIT_MATERIAL_SET, .matName = c->material, .matKeys = on ? "filter nearest" : "-filter" };
    MS_EntityId *ids;
    int count;
    char msg[512];
    EditResult r = MapEditApply(&em->ed, NULL, 0, &op, &ids, &count, msg, sizeof(msg));
    free(ids);
    Say(dm, r != EDIT_DONE, msg);
    InvalidateMats(dm);
    return r == EDIT_DONE ? DRAWMODE_RELOADED : DRAWMODE_NONE;
}

static bool HitIsBox(const MS_SurfaceHit *h);
static bool HitIsModel(const MS_SurfaceHit *h);

DrawModeResult DrawModePanelAction(DrawMode *dm, EditMode *em, int action, const unsigned char faces[6])
{
    // (the panel's target: the surface the cursor rested on last)
    int ci = dm->target.version != 0 ? dm->target.canvas : -1;
    if (ci < 0 && action == 3) {
        Say(dm, false, dm->pixelLook ? "pixel look on: new canvases get filter nearest" : "pixel look off: new canvases are filtered");
        return DRAWMODE_NONE;
    }
    if (ci < 0 && action >= 1 && action <= 2) {
        const char *what = action == 1 ? "resize" : "faces";
        bool some = dm->target.version != 0 && (HitIsBox(&dm->target.hit) || HitIsModel(&dm->target.hit));
        if (!some) Say(dm, true, TextFormat("%s: point at a painted box or model first (the panel acts on the surface the cursor rests on)", what));
        else if (!dm->target.ok) Say(dm, true, TextFormat("%s: %s", what, dm->target.refusal));
        else Say(dm, true, TextFormat("%s: %s has no canvas yet: paint it first", what, dm->target.what));
        return DRAWMODE_NONE;
    }
    if (action == 1) return ResizeCanvas(dm, em, ci);
    if (action == 2 && faces) return LayoutCanvas(dm, em, ci, faces);
    if (action == 3) return PixelLook(dm, em, ci, dm->pixelLook);
    return DRAWMODE_NONE;
}

// #endregion

// ======================================================================================
// #region The eyedropper
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

// ALT+LMB (docs/painting.md 7.5): the texel the surface shows, from the canvas, else the albedo
// texture times the albedo colour, else the colour alone
static bool Eyedropper(DrawMode *dm, const MS_SurfaceHit *h, Color *out)
{
    if (h->hit.obj.kind == MS_OBJ_NONE) return false;
    RefreshMats(dm);
    int m = h->material;
    if (m >= dm->matCount) return false;
    Sample s;
    if (m >= 0 && dm->mats[m].canvas >= 0 && HitSample(dm, dm->mats[m].canvas, h, &s)) {
        const Canvas *c = &dm->canvases[dm->mats[m].canvas];
        int x = ClampI((int)floorf(s.u * c->img.width), 0, c->img.width - 1), y = ClampI((int)floorf(s.v * c->img.height), 0, c->img.height - 1);
        const unsigned char *p = (const unsigned char *)c->img.data + ((size_t)y * c->img.width + x) * 4;
        *out = (Color){ p[0], p[1], p[2], p[3] };
        return true;
    }
    const R3D_Material *R = NULL;
    Texture2D tex = { 0 };
    char key[MS_PATH_MAX + 32] = "";
    int wrap = 0;
    if (m >= 0) {
        R = &scene->materials[m].material;
        if (dm->mats[m].albedoMap) {
            snprintf(key, sizeof(key), "%s", dm->mats[m].albedoMap);
            tex = MS_TextureFind(scene, key);
            wrap = dm->mats[m].wrap;
        }
    } else if (h->hit.obj.kind == MS_OBJ_MODEL) {
        int file = scene->models[h->hit.obj.index].file;
        const MS_ModelFile *F = &scene->modelFiles[file];
        if (h->slot >= 0 && h->slot < F->model.materialCount && F->model.materials) R = &F->model.materials[h->slot];
        tex = MS_ModelSlotTexture(scene, file, h->slot);
        snprintf(key, sizeof(key), "%s#%d", F->path, h->slot);
    }
    Color tint = R ? R->albedo.color : WHITE;
    float t[4] = { 255.0f, 255.0f, 255.0f, 255.0f };
    if (tex.id) {
        if (!dm->pickRel || strcmp(dm->pickRel, key) != 0 || dm->pickLoad != scene->loadVersion) {
            free(dm->pickRel);
            FreeImage(&dm->pickImg);
            Image img;
            dm->pickRel = MS_StrDup(key);
            dm->pickImg = MS_TextureRead(tex, &img) ? AdoptImage(img) : (Image){ 0 };
            dm->pickLoad = scene->loadVersion;
        }
        if (dm->pickImg.data) {
            int x = ClampI((int)floorf(WrapCoord(h->uv.x, wrap) * dm->pickImg.width), 0, dm->pickImg.width - 1);
            int y = ClampI((int)floorf(WrapCoord(h->uv.y, wrap) * dm->pickImg.height), 0, dm->pickImg.height - 1);
            const unsigned char *p = (const unsigned char *)dm->pickImg.data + ((size_t)y * dm->pickImg.width + x) * 4;
            for (int k = 0; k < 4; k++) t[k] = p[k];
        }
    }
    *out = (Color){ (unsigned char)lroundf(t[0] * tint.r / 255.0f), (unsigned char)lroundf(t[1] * tint.g / 255.0f),
                    (unsigned char)lroundf(t[2] * tint.b / 255.0f), (unsigned char)lroundf(t[3] * tint.a / 255.0f) };
    return true;
}

// #endregion

// ======================================================================================
// #region The surface under the cursor and the panel's target: texture space, sizes, texts
// ======================================================================================

#define SETTLE_TIME 0.3f        // s the cursor rests on a surface before the panel shows it

// A new canvas's size per surface (two entries: the hover and the panel's target)
typedef struct DrawSizeCache {
    bool valid;
    int kind, index, slot;
    bool shared;
    float density;
    unsigned int version;
    int w, h;
} DrawSizeCache;

// The same surface: object, and its face (boxes) or slot (models)
static bool SameSurface(const MS_SurfaceHit *a, const MS_SurfaceHit *b)
{
    if (a->hit.obj.kind != b->hit.obj.kind || a->hit.obj.index != b->hit.obj.index) return false;
    if (a->hit.obj.kind == MS_OBJ_BOX) return a->face == b->face;
    if (a->hit.obj.kind == MS_OBJ_MODEL) return a->slot == b->slot;
    return true;
}

static bool HitIsBox(const MS_SurfaceHit *h)
{
    return h->hit.obj.kind == MS_OBJ_BOX && h->hit.obj.index >= 0 && h->hit.obj.index < scene->boxCount && h->face >= 0 && h->face < 6;
}

static bool HitIsModel(const MS_SurfaceHit *h)
{
    return h->hit.obj.kind == MS_OBJ_MODEL && h->hit.obj.index >= 0 && h->hit.obj.index < scene->modelCount;
}

// The size the first stroke gives the surface's canvas (as CreateOwnCanvas: docs/painting.md 3.3, 5.1)
static bool NewCanvasSize(DrawMode *dm, const MS_SurfaceHit *h, bool sharedModel, int *w, int *hh)
{
    if (!dm->sizes && !(dm->sizes = calloc(2, sizeof(DrawSizeCache)))) return false;
    MS_ObjRef obj = h->hit.obj;
    bool model = obj.kind == MS_OBJ_MODEL;
    int slot = model ? h->slot : -1;
    for (int i = 0; i < 2; i++) {
        const DrawSizeCache *e = &dm->sizes[i];
        if (e->valid && e->kind == (int)obj.kind && e->index == obj.index && e->slot == slot && e->shared == sharedModel &&
            e->density == dm->density && e->version == scene->version) {
            *w = e->w;
            *hh = e->h;
            return true;
        }
    }
    int W = 0, H = 0;
    if (!model) {
        MS_ObjRef *objs;
        int copies, n = CopyObjects(obj, &objs, &copies);
        unsigned char cells[6] = { 0, 1, 2, 3, 4, 5 };
        int cols, rows;
        LayoutGrid(cells, &cols, &rows);
        int side = n > 0 ? CellSide(dm->density, objs, n, cols, rows) : 0;
        free(objs);
        W = cols * side;
        H = rows * side;
    } else {
        // The source's texture keeps its size when the copy is texel for texel (identity tiling)
        int m = h->material, file = scene->models[obj.index].file;
        const MS_ModelFile *F = &scene->modelFiles[file];
        const R3D_Material *own = F->model.materials && slot >= 0 && slot < F->model.materialCount ? &F->model.materials[slot] : NULL;
        const R3D_Material *R = m >= 0 ? &scene->materials[m].material : own;
        Vector2 scale = R ? R->uvScale : (Vector2){ 1, 1 }, off = R ? R->uvOffset : (Vector2){ 0, 0 };
        bool inherit = false;
        if (m >= 0) {
            Sb keys = { 0 };
            int size;
            const char *text = MS_MapText(scene, &size);
            if (text) ForMaterialLines(text, size, scene->materials[m].name, AddKeys, &keys);
            inherit = keys.s && KeysInheritAlbedo(keys.s);
            free(keys.s);
        }
        if (inherit && own) {
            scale = own->uvScale;
            off = own->uvOffset;
        }
        Texture2D t = { 0 };
        RefreshMats(dm);
        if (m >= 0 && m < dm->matCount && dm->mats[m].albedoMap) t = MS_TextureFind(scene, dm->mats[m].albedoMap);
        else if (m < 0 || inherit) t = MS_ModelSlotTexture(scene, file, slot);
        bool identity = scale.x == 1.0f && scale.y == 1.0f && off.x == 0.0f && off.y == 0.0f;
        if (t.id && identity) { W = t.width; H = t.height; }
        else W = H = SlotSide(dm->density, obj.index, slot);
    }
    if (W <= 0 || H <= 0) return false;
    // (the older entry goes)
    int at = !dm->sizes[0].valid ? 0 : !dm->sizes[1].valid ? 1 : -1;
    if (at < 0) {
        dm->sizes[0] = dm->sizes[1];
        at = 1;
    }
    dm->sizes[at] = (DrawSizeCache){ true, (int)obj.kind, obj.index, slot, sharedModel, dm->density, scene->version, W, H };
    *w = W;
    *hh = H;
    return true;
}

// The texture space of a resolved surface (DrawGeom)
static void SurfaceGeom(DrawMode *dm, const MS_SurfaceHit *h, const Target *t, DrawGeom *g)
{
    memset(g, 0, sizeof(*g));
    g->scale = (Vector2){ 1, 1 };
    if (t->why[0] || h->hit.obj.kind == MS_OBJ_NONE) return;
    RefreshMats(dm);
    int m = h->material;
    if (m >= 0 && m < dm->matCount) {
        g->wrap = dm->mats[m].wrap;
        g->nearest = dm->mats[m].nearest;
        g->scale = scene->materials[m].material.uvScale;
    }
    g->uv = h->uv;
    if (t->canvas >= 0) {
        const Canvas *c = &dm->canvases[t->canvas];
        g->w = c->img.width;
        g->h = c->img.height;
        g->layout = c->layout;
        memcpy(g->cells, c->cells, 6);
        g->wrap = c->wrap;
        g->ok = g->w > 0 && g->h > 0;
        return;
    }
    if (t->open) {
        Texture2D tx = MS_TextureFind(scene, t->rel);
        int cols, rows;
        g->w = tx.width;
        g->h = tx.height;
        g->layout = m >= 0 && MS_MaterialFaceLayout(scene, m, g->cells, &cols, &rows);
        g->ok = tx.id && g->w > 0 && g->h > 0;
        return;
    }
    // A first stroke: the copy has uv_scale 1 1 (a box: uv_faces 0 1 2 3 4 5)
    if (!NewCanvasSize(dm, h, t->sharedModel, &g->w, &g->h)) return;
    g->fresh = true;
    g->nearest = dm->pixelLook;
    g->scale = (Vector2){ 1, 1 };
    if (HitIsBox(h)) {
        for (int f = 0; f < 6; f++) g->cells[f] = (unsigned char)f;
        g->layout = true;
        Rectangle r = MS_FaceCellRect(g->cells, h->face, true);
        g->uv = (Vector2){ r.x + h->faceUv.x * r.width, r.y + h->faceUv.y * r.height };
    } else g->uv = h->faceUv;
    g->ok = true;
}

// What paint at h goes into: its canvas, whether it can be painted (why not) and its texture space
static void ResolveSurface(DrawMode *dm, const MS_SurfaceHit *h, int *canvas, bool *ok, char *refusal, int refusalSize, DrawGeom *g)
{
    Target t;
    ResolveTarget(dm, h, &t);
    if (t.open && StartsWith(t.rel, PAINT_DIR)) {
        // (an earlier session's canvas: opened at once, so the HUD and the panel show it)
        char why[256];
        if (OpenCanvas(dm, h, t.rel, why, sizeof(why)) >= 0) ResolveTarget(dm, h, &t);
    }
    *ok = !t.why[0];
    *canvas = t.canvas;
    snprintf(refusal, (size_t)refusalSize, "%s", h->hit.obj.kind == MS_OBJ_NONE ? "" : t.why);
    SurfaceGeom(dm, h, &t, g);
}

static void HoverHit(DrawMode *dm, const MS_SurfaceHit *h)
{
    dm->hover = *h;
    dm->hoverVersion = 0;       // (no ray: HoverAt sets it)
    ResolveSurface(dm, h, &dm->hoverCanvas, &dm->hoverOk, dm->refusal, sizeof(dm->refusal), &dm->hoverGeom);
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

// The panel's three lines about its target
static void TargetText(DrawMode *dm)
{
    DrawTarget *T = &dm->target;
    const MS_SurfaceHit *h = &T->hit;
    T->what[0] = T->into[0] = T->texel[0] = 0;
    int m = h->material;
    const char *mat = m >= 0 && m < scene->materialCount ? scene->materials[m].name : NULL;
    if (HitIsBox(h)) {
        int line = MS_ObjectRef(scene, h->hit.obj)->line;
        bool turned = scene->boxTurned && scene->boxTurned[h->hit.obj.index] >= 0;
        snprintf(T->what, sizeof(T->what), "%s line %d, face %s", turned ? "turned box" : "box", line, FACE_NAMES[h->face]);
    } else if (HitIsModel(h)) {
        int line = MS_ObjectRef(scene, h->hit.obj)->line;
        int file = scene->models[h->hit.obj.index].file;
        snprintf(T->what, sizeof(T->what), "model %s slot %d, line %d", GetFileName(scene->modelFiles[file].path), h->slot, line);
    } else if (h->hit.obj.kind == MS_OBJ_LIGHT) {
        snprintf(T->what, sizeof(T->what), "a light: nothing to paint");
        return;
    } else {
        snprintf(T->what, sizeof(T->what), "point at a box or a model");
        return;
    }
    if (!T->ok) return;
    const DrawGeom *g = &T->geom;
    if (T->canvas >= 0) {
        const Canvas *c = &dm->canvases[T->canvas];
        if (c->shared) snprintf(T->into, sizeof(T->into), "%s %d×%d, the map's own texture: every object with %s changes",
                                GetFileName(c->rel), c->img.width, c->img.height, mat ? mat : "it");
        else snprintf(T->into, sizeof(T->into), "%s %d×%d", GetFileName(c->rel), c->img.width, c->img.height);
    } else if (g->ok && !g->fresh)
        snprintf(T->into, sizeof(T->into), "%s %d×%d in place: every object with %s changes",
                 mat && m < dm->matCount && dm->mats[m].albedoMap ? GetFileName(dm->mats[m].albedoMap) : "its texture", g->w, g->h, mat ? mat : "it");
    else if (g->ok && HitIsModel(h) && dm->shared)
        snprintf(T->into, sizeof(T->into), "first stroke: its own texture as a %d×%d canvas, for every %s that shows it", g->w, g->h,
                 GetFileName(scene->modelFiles[scene->models[h->hit.obj.index].file].path));
    else if (g->ok && HitIsBox(h) && m >= 0 && m < scene->materialCount && scene->materials[m].shader)
        snprintf(T->into, sizeof(T->into), "first stroke: a new %d×%d canvas, %s with its shader baked in", g->w, g->h, mat);
    else if (g->ok) snprintf(T->into, sizeof(T->into), "first stroke: a new %d×%d canvas, a copy of %s", g->w, g->h, mat ? mat : "its own material");
    if (!g->ok) return;
    // The size of a texel here (mm along the face's two edges; a model: about, from the hit triangle)
    const char *will = g->fresh ? "will be" : "is";
    if (HitIsBox(h)) {
        int b = h->hit.obj.index;
        Vector3 s = scene->boxTurned && scene->boxTurned[b] >= 0 ? scene->turnedBoxes[scene->boxTurned[b]].size
                  : Vector3Subtract(scene->boxes[b].max, scene->boxes[b].min);
        int axis = h->face / 2;
        float su = axis == 0 ? s.z : s.x, sv = axis == 1 ? s.z : s.y;   // the face's two edges (u, v)
        Rectangle r = g->layout ? MS_FaceCellRect(g->cells, h->face, true) : (Rectangle){ 0, 0, 1, 1 };
        float pu = r.width * fabsf(g->scale.x) * g->w, pv = r.height * fabsf(g->scale.y) * g->h;
        if (pu > 0.0f && pv > 0.0f)
            snprintf(T->texel, sizeof(T->texel), "face %s: a texel %s %.1f × %.1f mm", FACE_NAMES[h->face], will, su * 1000.0f / pu, sv * 1000.0f / pv);
    } else {
        const MS_ModelInstance *M = &scene->models[h->hit.obj.index];
        int k = MS_ModelSlotTriangleIndex(scene, M->file, h->mesh, h->triangle);
        Vector2 uv[3];
        Vector3 p[3];
        if (k >= 0 && MS_ModelSlotTriangle(scene, M->file, h->slot, k, uv, p)) {
            for (int v = 0; v < 3; v++) p[v] = Vector3Transform(p[v], M->transform);
            float A = 0.5f * Vector3Length(Vector3CrossProduct(Vector3Subtract(p[1], p[0]), Vector3Subtract(p[2], p[0])));
            float a = 0.5f * fabsf((uv[1].x - uv[0].x) * (uv[2].y - uv[0].y) - (uv[2].x - uv[0].x) * (uv[1].y - uv[0].y));
            float texels = a * fabsf(g->scale.x * g->scale.y) * g->w * g->h;
            if (texels > 0.0f && A > 0.0f) snprintf(T->texel, sizeof(T->texel), "here a texel %s about %.1f mm", will, 1000.0f * sqrtf(A / texels));
        }
    }
}

// The target becomes the surface under the cursor (the same surface: only its hit moves on)
static void TargetTake(DrawMode *dm)
{
    DrawTarget *T = &dm->target;
    if (!(T->version != 0 && SameSurface(&T->hit, &dm->hover))) T->gen++;
    T->hit = dm->hover;
    T->canvas = dm->hoverCanvas;
    T->ok = dm->hoverOk;
    snprintf(T->refusal, sizeof(T->refusal), "%s", dm->refusal);
    T->geom = dm->hoverGeom;
    T->ray = dm->hoverRay;
    T->version = scene->version;
}

// Per frame: the target found again after the scene changed (the same ray), its canvas, texture
// space and lines brought up to date (the density, the shared switch)
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
    ResolveSurface(dm, &T->hit, &T->canvas, &T->ok, T->refusal, sizeof(T->refusal), &T->geom);
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
    if (!em->ed.work && !MapEditBegin(&em->ed)) Say(dm, true, "this map can't be painted (a compiled map?): load its .map");
}

void DrawModeClose(DrawMode *dm)
{
    dm->active = false;
    dm->hoverOk = false;
    DropBake(dm);       // (a bake asked for and not used yet: no stroke after leaving)
}

static DrawModeResult History(DrawMode *dm, EditMode *em, Designer *d, bool undo)
{
    if (!EditModeConfirm(em, undo ? KEY_Z : KEY_Y)) return DRAWMODE_NONE;
    if (!em->ed.work && !MapEditBegin(&em->ed)) return DRAWMODE_NONE;
    MS_EntityId *ids;
    int count;
    char msg[512];
    EditResult r = undo ? MapEditUndo(&em->ed, &ids, &count, msg, sizeof(msg)) : MapEditRedo(&em->ed, &ids, &count, msg, sizeof(msg));
    InvalidateMats(dm);
    DesignerSync(d);        // (the selection stays: the draw mode doesn't select)
    free(ids);
    Say(dm, r != EDIT_DONE, msg);
    return r == EDIT_DONE ? DRAWMODE_RELOADED : DRAWMODE_NONE;
}

// Leaves the mode for `next` (B / ESC: spectator; M edit, V volume, C snip mode); with unsaved
// edits the first press only says so
static bool Leave(DrawMode *dm, EditMode *em, Designer *d, int key)
{
    int unsaved = MapEditUnsaved(&em->ed);
    if (unsaved > 0 && dm->leaveWarnTime <= 0.0f && (key == KEY_B || key == KEY_ESCAPE)) {
        dm->leaveWarnTime = WARN_TIME;
        Say(dm, true, TextFormat("%d unsaved edit%s: CTRL+S saves, B again leaves them unsaved (still live)", unsaved, unsaved == 1 ? "" : "s"));
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

// LMB down on the hover at px: the stroke begins (a first stroke makes the canvas) with a stamp
// there. True when the text changed live
static bool PressStroke(DrawMode *dm, EditMode *em, Camera3D cam, Vector2 px, int w, int h)
{
    bool reloaded;
    if (!StrokeBegin(dm, em, &dm->hover, &reloaded)) return false;
    if (reloaded) DrawModeHover(dm, cam, px, w, h);       // (the surface has its new material)
    Probe p = ProbeHit(dm, &dm->hover, px);
    if (p.ok) StampProbe(dm, &p);
    dm->lastPx = px;
    dm->lastOk = p.ok;
    dm->lastTx = p.tx;
    dm->lastTy = p.ty;
    dm->lastRegion = p.s.region;
    return reloaded;
}

// LMB down on a box whose first stroke bakes its shader (docs/painting.md 3.6): the bake is asked
// for (DrawModeRunBake renders it before this frame's scene draw) and the stroke waits for the next
// frame. False when no bake is needed (or it was refused: said)
static bool RequestBake(DrawMode *dm, EditMode *em, const MS_SurfaceHit *h, Vector2 px)
{
    Target t;
    ResolveTarget(dm, h, &t);
    if (t.why[0] || !t.bake) return false;
    if (!em->ed.work && !MapEditBegin(&em->ed)) { Say(dm, true, "this map can't be painted (a compiled map?): load its .map"); return true; }
    int W, H;
    Sb keys = { 0 };
    int size;
    const char *text = MS_MapText(scene, &size);
    if (text) ForMaterialLines(text, size, scene->materials[h->material].name, AddKeys, &keys);
    char *tex = NULL, why[300] = "";
    bool licensed = BakeTextures(keys.s, &tex, why, (int)sizeof(why));
    free(keys.s);
    free(tex);
    if (!licensed) { Say(dm, true, why); return true; }
    if (!NewCanvasSize(dm, h, false, &W, &H)) { Say(dm, true, "this object doesn't come from a map line"); return true; }
    DropBake(dm);
    dm->bakeWant = true;
    dm->bakeHit = *h;
    dm->bakePx = px;
    dm->bakeVersion = scene->version;
    dm->bakeW = W;
    dm->bakeH = H;
    Say(dm, false, TextFormat("baking the shader of %s into a %dx%d canvas ...", scene->materials[h->material].name, W, H));
    return true;
}

void DrawModeRunBake(DrawMode *dm)
{
    if (!dm->bakeWant) return;
    dm->bakeWant = false;
    if (dm->bakeVersion != scene->version || dm->bakeHit.hit.obj.kind != MS_OBJ_BOX) return;
    char why[300];
    int W = dm->bakeW, H = dm->bakeH;
    if (BakeInto(dm, dm->bakeHit.hit.obj.index, W, H, why, (int)sizeof(why))) dm->bakeReady = true;
    else Say(dm, true, TextFormat("the shader can't be baked: %s", why));
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

    // The frame after a shader bake: its first stroke begins where LMB went down (docs/painting.md 3.6)
    if (dm->bakeReady && !dm->stroking) {
        if (dm->bakeVersion == scene->version && !flying) {
            HoverHit(dm, &dm->bakeHit);
            if (PressStroke(dm, em, cam, dm->bakePx, w, h)) res = DRAWMODE_RELOADED;
            if (dm->stroking && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) StrokeEnd(dm, em);     // (a click: one dab)
            if (!cursorOnPanel && !flying) HoverAt(dm, cam, GetMousePosition(), w, h);
        } else Say(dm, true, "the shader's bake was dropped: the map changed (click again)");
        DropBake(dm);
    }

    bool ctrl = Ctrl(), shift = Shift(), alt = Alt();
    if (!dm->stroking && !dm->keysBlocked) {
        if (ctrl && IsKeyPressed(KEY_Z)) return History(dm, em, d, !shift);
        if (ctrl && IsKeyPressed(KEY_Y)) return History(dm, em, d, false);
        if (ctrl && IsKeyPressed(KEY_S)) {
            char msg[512];
            if (EditModeConfirm(em, KEY_S)) {
                bool ok = DrawModeSave(dm, em, msg, sizeof(msg));
                Say(dm, !ok, msg);
            }
            return DRAWMODE_NONE;
        }
        if (ctrl && shift && IsKeyPressed(KEY_DELETE) && em->ed.failedCount > 0) {
            MapEditClearFailed(&em->ed);
            Say(dm, false, "list of edits that could not be re-applied cleared");
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
        if (!ctrl && !alt && IsKeyPressed(KEY_H)) {
            dm->shared = !dm->shared;
            Say(dm, false, dm->shared ? "shared: paints the texture the surface shows (every object using it)" : "own: each object gets its own canvas");
        }
        if (!ctrl && !alt && IsKeyPressed(KEY_TAB)) dm->panelHidden = !dm->panelHidden;
        if (IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressedRepeat(KEY_LEFT_BRACKET)) SizeStep(dm, -1);
        if (IsKeyPressed(KEY_RIGHT_BRACKET) || IsKeyPressedRepeat(KEY_RIGHT_BRACKET)) SizeStep(dm, 1);
    }
    float wheel = flying || cursorOnPanel ? 0.0f : GetMouseWheelMove();
    if (wheel != 0.0f && ctrl) SizeStep(dm, wheel > 0.0f ? 1 : -1);
    else if (wheel != 0.0f && shift) dm->color.a = (unsigned char)ClampI(dm->color.a + (wheel > 0.0f ? 8 : -8), 0, 255);

    Vector2 px = GetMousePosition();
    if (!cursorOnPanel && !flying && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        Settle(dm, 0.0f, true);     // (a click: the panel shows that surface at once)
        if (alt) {
            Color c;
            if (Eyedropper(dm, &dm->hover, &c)) {
                SetColor(dm, c);
                Say(dm, false, TextFormat("picked #%02X%02X%02X a%d", c.r, c.g, c.b, c.a));
            }
        } else if (dm->hoverOk && EditModeConfirm(em, STROKE_KEY)) {
            if (!RequestBake(dm, em, &dm->hover, px) && PressStroke(dm, em, cam, px, w, h)) res = DRAWMODE_RELOADED;
        } else if (!dm->hoverOk && dm->refusal[0]) Say(dm, true, dm->refusal);
    } else if (dm->stroking && dm->strokeCanvas >= 0 && dm->pen != PEN_FILL &&
               (px.x != dm->lastPx.x || px.y != dm->lastPx.y)) {
        const Canvas *c = &dm->canvases[dm->strokeCanvas];
        Probe a = { .px = dm->lastPx, .ok = dm->lastOk, .s = { dm->lastTx / c->img.width, dm->lastTy / c->img.height, dm->lastRegion },
                    .tx = dm->lastTx, .ty = dm->lastTy };
        Probe b = ProbeHit(dm, &dm->hover, px);
        StrokeSegment(dm, cam, w, h, a, b);
        dm->lastPx = px;
        dm->lastOk = b.ok;
        dm->lastTx = b.tx;
        dm->lastTy = b.ty;
        dm->lastRegion = b.s.region;
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
    InvalidateMats(dm);
    for (int i = 0; i < dm->canvasCount; i++)
        if (dm->canvases[i].provided) Provide(&dm->canvases[i]);
}

void DrawModeSync(DrawMode *dm, EditMode *em, bool newScene)
{
    (void)em;
    InvalidateMats(dm);
    if (newScene) {
        // a renderer restart: the textures came from disk, the canvases have the unsaved strokes
        for (int i = 0; i < dm->canvasCount; i++)
            if (dm->canvases[i].provided) Upload(&dm->canvases[i], 0, 0, dm->canvases[i].img.width, dm->canvases[i].img.height);
        return;
    }
    for (int i = 0; i < dm->canvasCount; i++) {
        Canvas *c = &dm->canvases[i];
        if (c->rebaseGen == dm->rebaseGen && c->replaying && c->replayed > 0)
            Say(dm, false, TextFormat("re-applied %d stroke%s onto %s, which changed on disk", c->replayed, c->replayed == 1 ? "" : "s", c->rel));
        if (c->refs == 0) {
            // nothing holds its pixels: the next stroke reads the file (or the scene's texture) again
            c->stale = true;
            if (c->provided) Unprovide(c);
        }
    }
}

int DrawModeDrawHud(const DrawMode *dm, const EditMode *em, int x, int y)
{
    if (!dm->active) return 0;
    char target[300] = "";
    const MS_SurfaceHit *h = &dm->hover;
    const DrawGeom *g = &dm->hoverGeom;
    if (dm->hoverOk && (HitIsBox(h) || HitIsModel(h))) {
        char what[160];
        if (HitIsModel(h))
            snprintf(what, sizeof(what), "%s slot %d", GetFileName(scene->modelFiles[scene->models[h->hit.obj.index].file].path), h->slot);
        else snprintf(what, sizeof(what), "face %s", FACE_NAMES[h->face]);
        int m = h->material;
        RefreshMats((DrawMode *)dm);    // (cached per scene version)
        if (dm->hoverCanvas >= 0) {
            const Canvas *c = &dm->canvases[dm->hoverCanvas];
            snprintf(target, sizeof(target), "  |  %s %d×%d %s", GetFileName(c->rel), c->img.width, c->img.height, what);
        } else if (g->ok && g->fresh) snprintf(target, sizeof(target), "  |  %s: a new %d×%d canvas", what, g->w, g->h);
        else if (g->ok && m >= 0 && m < dm->matCount && dm->mats[m].albedoMap)
            snprintf(target, sizeof(target), "  |  %s %d×%d %s (in place)", GetFileName(dm->mats[m].albedoMap), g->w, g->h, what);
        else snprintf(target, sizeof(target), "  |  %s", what);
    }
    char unsaved[64] = "";
    int n = EditModeUnsaved(em);
    if (n > 0) snprintf(unsaved, sizeof(unsaved), "  |  %d unsaved edit%s", n, n == 1 ? "" : "s");
    Color c = dm->color;
    UiHudText(TextFormat("DRAW  %s%s  #%02X%02X%02X a%d%s%s%s", PEN_NAMES[dm->pen], dm->pen == PEN_FILL ? "" : TextFormat(" %d px", dm->size), c.r, c.g, c.b, c.a, target,
                         dm->shared ? "  |  shared" : "", unsaved), x, y, UI_HUD_LINE, UI_S, Fade(DRAW_COLOR, 0.95f));
    return UI_HUD_LINE;
}

int DrawModeDrawMessages(const DrawMode *dm, int x, int y)
{
    if (!dm->active) return 0;
    int y0 = y;
    if (dm->refusal[0] && !dm->onPanel) {
        UiHudText(dm->refusal, x, y, UI_HUD_LINE, UI_S, UI_ERROR);
        y += UI_HUD_LINE;
    }
    if (dm->messageTime > 0.0f) {
        Color mc = dm->messageError ? UI_ERROR : DRAW_COLOR;
        UiHudText(dm->message, x, y, UI_HUD_LINE, UI_S, Fade(mc, fminf(dm->messageTime, 1.0f)));
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

// A model slot's triangles (texture coordinates, model-space positions) and UV islands, kept for
// the brush cursor (built when the cursor goes onto another slot or the scene changed), and the
// border of one island (the fill pen's outline)
typedef struct DrawSlotTris {
    bool valid;
    int file, slot;
    unsigned int version;
    int n;
    Vector2 *uv;            // 3 per triangle
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
        if (!MS_ModelSlotTriangle(scene, file, slot, k, &S->uv[3 * k], &S->pos[3 * k]))
            for (int v = 0; v < 3; v++) { S->uv[3 * k + v] = (Vector2){ 0 }; S->pos[3 * k + v] = (Vector3){ 0 }; }
        S->root[k] = k;
    }
    ok = ok && JoinUvEdges(S->uv, n, S->root, table, cap);
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
    if (!dm->active || !dm->hoverOk || !dm->hoverGeom.ok || !HitIsModel(h)) return;
    int file = scene->models[h->hit.obj.index].file;
    const DrawSlotTris *S = SlotTrisFor(dm, file, h->slot);
    int k = S ? MS_ModelSlotTriangleIndex(scene, file, h->mesh, h->triangle) : -1;
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

// A texel-space point of the hover's texture (around the hit texel (tx, ty)) in the surface's own
// coordinates: face coordinates 0..1 (boxes) or the file's texture coordinates (models)
static Vector2 TexelToSurface(const MS_SurfaceHit *h, const DrawGeom *g, Vector2 p, float tx, float ty)
{
    Vector2 d = { (p.x - tx) / g->w, (p.y - ty) / g->h };
    if (g->wrap == 2) {
        // (mirror: an odd tile runs backwards)
        if ((int)floorf(g->uv.x) & 1) d.x = -d.x;
        if ((int)floorf(g->uv.y) & 1) d.y = -d.y;
    }
    d.x /= g->scale.x != 0.0f ? g->scale.x : 1.0f;
    d.y /= g->scale.y != 0.0f ? g->scale.y : 1.0f;
    if (h->hit.obj.kind == MS_OBJ_BOX) {
        Rectangle r = g->layout ? MS_FaceCellRect(g->cells, h->face, true) : (Rectangle){ 0, 0, 1, 1 };
        return (Vector2){ h->faceUv.x + d.x / r.width, h->faceUv.y + d.y / r.height };
    }
    return Vector2Add(h->faceUv, d);
}

// A point of a slot triangle (its uv corners, model-space corners) at texture coordinate t
static Vector3 TriPoint(const Vector2 *uv, const Vector3 *pos, Vector2 t)
{
    float den = (uv[1].y - uv[2].y) * (uv[0].x - uv[2].x) + (uv[2].x - uv[1].x) * (uv[0].y - uv[2].y);
    float a = ((uv[1].y - uv[2].y) * (t.x - uv[2].x) + (uv[2].x - uv[1].x) * (t.y - uv[2].y)) / den;
    float b = ((uv[2].y - uv[0].y) * (t.x - uv[2].x) + (uv[0].x - uv[2].x) * (t.y - uv[2].y)) / den;
    return Vector3Add(Vector3Add(Vector3Scale(pos[0], a), Vector3Scale(pos[1], b)), Vector3Scale(pos[2], 1.0f - a - b));
}

// A closed or open polyline of n texel-space points drawn on the hovered surface: on a box face
// (exact: the face is linear in its coordinates), cut at the face's edges; on a model, through each
// triangle of the hit triangle's UV island that it crosses (each triangle's affine map), cut at the
// triangle's edges. dashed: every other pair of segments
static void CursorPolyline(const DrawMode *dm, const Vector2 *pts, int n, float tx, float ty, Color col, bool dashed)
{
    const MS_SurfaceHit *h = &dm->hover;
    const DrawGeom *g = &dm->hoverGeom;
    if (h->hit.obj.kind == MS_OBJ_BOX) {
        static const Vector2 face[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        for (int i = 0; i + 1 < n; i++) {
            if (dashed && ((i / 2) & 1)) continue;
            Vector2 a = TexelToSurface(h, g, pts[i], tx, ty), b = TexelToSurface(h, g, pts[i + 1], tx, ty);
            if (ClipSegment(&a, &b, face, 4)) CursorLine(FacePoint(h->hit.obj.index, h->face, a), FacePoint(h->hit.obj.index, h->face, b), col);
        }
        return;
    }
    const MS_ModelInstance *M = &scene->models[h->hit.obj.index];
    const DrawSlotTris *S = dm->slotTris;
    int k = MS_ModelSlotTriangleIndex(scene, M->file, h->mesh, h->triangle);
    bool cached = S && S->valid && S->file == M->file && S->slot == h->slot && S->version == scene->version && k >= 0 && k < S->n;
    // The footprint's box in texture coordinates (the triangles to look at)
    Vector2 lo = { INFINITY, INFINITY }, hi = { -INFINITY, -INFINITY };
    for (int i = 0; i < n; i++) {
        Vector2 s = TexelToSurface(h, g, pts[i], tx, ty);
        lo = (Vector2){ fminf(lo.x, s.x), fminf(lo.y, s.y) };
        hi = (Vector2){ fmaxf(hi.x, s.x), fmaxf(hi.y, s.y) };
    }
    int first = cached ? 0 : k, last = cached ? S->n - 1 : k;
    for (int t = first; t >= 0 && t <= last; t++) {
        Vector2 uvs[3];
        Vector3 pos[3];
        if (cached) {
            if (S->root[t] != S->root[k]) continue;
            memcpy(uvs, &S->uv[3 * t], sizeof(uvs));
            memcpy(pos, &S->pos[3 * t], sizeof(pos));
        } else if (!MS_ModelSlotTriangle(scene, M->file, h->slot, t, uvs, pos)) return;
        if (fmaxf(uvs[0].x, fmaxf(uvs[1].x, uvs[2].x)) < lo.x || fminf(uvs[0].x, fminf(uvs[1].x, uvs[2].x)) > hi.x ||
            fmaxf(uvs[0].y, fmaxf(uvs[1].y, uvs[2].y)) < lo.y || fminf(uvs[0].y, fminf(uvs[1].y, uvs[2].y)) > hi.y)
            continue;
        for (int i = 0; i + 1 < n; i++) {
            if (dashed && ((i / 2) & 1)) continue;
            Vector2 a = TexelToSurface(h, g, pts[i], tx, ty), b = TexelToSurface(h, g, pts[i + 1], tx, ty);
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

// The fill pen: the outline of what one click fills (a box: the faces of the hit face's cell; a
// model: the hit triangle's UV island)
static void CursorFill(const DrawMode *dm, Color col)
{
    const MS_SurfaceHit *h = &dm->hover;
    const DrawGeom *g = &dm->hoverGeom;
    if (h->hit.obj.kind == MS_OBJ_BOX) {
        static const Vector2 q[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        for (int f = 0; f < 6; f++) {
            // (without a layout every face shows the whole image)
            if (g->layout ? g->cells[f] != g->cells[h->face] : false) continue;
            for (int i = 0; i < 4; i++)
                CursorLine(FacePoint(h->hit.obj.index, f, q[i]), FacePoint(h->hit.obj.index, f, q[(i + 1) % 4]), col);
        }
        return;
    }
    const MS_ModelInstance *M = &scene->models[h->hit.obj.index];
    const DrawSlotTris *S = dm->slotTris;
    int k = MS_ModelSlotTriangleIndex(scene, M->file, h->mesh, h->triangle);
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
    DesignerBeginOverlay3D(cam);
    cursorEye = cam.position;
    Color col = Fade(DRAW_COLOR, 0.95f);
    if (dm->pen == PEN_FILL) CursorFill(dm, col);
    else {
        // The pen's footprint in texel space about the hit (docs/painting.md 7.2 as phase 2 stamps it)
        float tx = WrapCoord(g->uv.x, g->wrap) * g->w, ty = WrapCoord(g->uv.y, g->wrap) * g->h;
        int s = dm->size < 1 ? 1 : dm->size;
        if (dm->pen == PEN_SQUARE) CursorQuad(dm, floorf(tx) - (float)(s / 2), floorf(ty) - (float)(s / 2), (float)s, tx, ty, col);
        else if (s == 1) CursorQuad(dm, floorf(tx), floorf(ty), 1.0f, tx, ty, col);
        else if (s == 2) CursorQuad(dm, floorf(tx - 0.5f), floorf(ty - 0.5f), 2.0f, tx, ty, col);
        else {
            // (a ring: the texels whose centre is inside it are painted; the eraser's is dashed, the
            // soft pen's solid core a fainter ring inside)
            CursorRing(dm, tx, ty, s * 0.5f, tx, ty, col, dm->pen == PEN_ERASER);
            if (dm->pen == PEN_SOFT && dm->hardness > 0.05f && dm->hardness < 0.95f)
                CursorRing(dm, tx, ty, s * 0.5f * dm->hardness, tx, ty, Fade(DRAW_COLOR, 0.45f), false);
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
    dm->strokeCanvas = dm->lastCanvas = dm->hoverCanvas = -1;
    em->ed.paintHooks = (MapEditPaintHooks){ dm, HookUndo, HookRedo, HookRebase, HookRelease, HookBytes, HookSave };
}

void DrawModeFree(DrawMode *dm)
{
    if (dm->stroke && !dm->strokePushed) ReleaseStep(dm, dm->stroke);
    for (int i = 0; i < dm->canvasCount; i++) FreeCanvas(&dm->canvases[i]);
    free(dm->canvases);
    FreeMats(dm);
    free(dm->mats);
    free(dm->pickRel);
    FreeImage(&dm->pickImg);
    free(dm->sizes);
    FreeSlotTris(dm->slotTris);
    free(dm->slotTris);
    DropBake(dm);
    free(dm->bake);
    memset(dm, 0, sizeof(*dm));
}

// A surface hit made from texture space (a script's `uv`): box face f at face coordinates (u, v),
// or model slot s at texture coordinate (u, v) (the first triangle of the slot that holds it)
static bool UvHit(MS_ObjRef obj, int faceOrSlot, float u, float v, MS_SurfaceHit *h)
{
    memset(h, 0, sizeof(*h));
    h->hit.obj = obj;
    h->material = h->face = h->slot = h->mesh = h->triangle = -1;
    h->faceUv = (Vector2){ u, v };
    if (obj.kind == MS_OBJ_BOX) {
        if (obj.index < 0 || obj.index >= scene->boxCount || faceOrSlot < 0 || faceOrSlot > 5) return false;
        h->face = faceOrSlot;
        h->material = scene->boxRefs[obj.index].material;
        Vector2 uv = { u, v };
        unsigned char cells[6];
        int cols, rows;
        if (MS_MaterialFaceLayout(scene, h->material, cells, &cols, &rows)) {
            Rectangle r = MS_FaceCellRect(cells, h->face, true);
            uv = (Vector2){ r.x + u * r.width, r.y + v * r.height };
        }
        const R3D_Material *R = &scene->materials[h->material].material;
        h->uv = (Vector2){ R->uvOffset.x + uv.x * R->uvScale.x, R->uvOffset.y + uv.y * R->uvScale.y };
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
        if (!MS_ModelSlotTriangle(scene, M->file, faceOrSlot, k, t, p)) return false;
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
        const MS_ModelFile *F = &scene->modelFiles[M->file];
        const R3D_Material *R = h->material >= 0 ? &scene->materials[h->material].material
                              : F->model.materials && faceOrSlot < F->model.materialCount ? &F->model.materials[faceOrSlot] : NULL;
        h->uv = R ? (Vector2){ R->uvOffset.x + u * R->uvScale.x, R->uvOffset.y + v * R->uvScale.y } : (Vector2){ u, v };
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

// Stamps between texture-space points a and b of the stroke's surface (a script's `uv` stroke)
static void UvSegment(DrawMode *dm, MS_ObjRef obj, int fs, Vector2 a, Vector2 b)
{
    const Canvas *c = &dm->canvases[dm->strokeCanvas];
    MS_SurfaceHit ha, hb;
    if (!UvHit(obj, fs, a.x, a.y, &ha) || !UvHit(obj, fs, b.x, b.y, &hb)) return;
    float du = (hb.uv.x - ha.uv.x) * c->img.width, dv = (hb.uv.y - ha.uv.y) * c->img.height;
    int steps = (int)ceilf(hypotf(du, dv) / Spacing(dm));
    steps = ClampI(steps, 1, 100000);
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / steps;
        MS_SurfaceHit h;
        if (!UvHit(obj, fs, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, &h)) continue;
        Probe p = ProbeHit(dm, &h, (Vector2){ 0 });
        if (p.ok) StampProbe(dm, &p);
    }
}

static void PrintCanvases(DrawMode *dm, EditMode *em)
{
    for (int i = 0; i < dm->canvasCount; i++) {
        const Canvas *c = &dm->canvases[i];
        printf("paint:   canvas %s %dx%d material %s%s%s unsaved %s hash %016llx base %016llx refs %d%s\n", c->rel, c->img.width,
               c->img.height, c->material ? c->material : "-", c->layout ? " layout " : "",
               c->layout ? TextFormat("%d %d %d %d %d %d", c->cells[0], c->cells[1], c->cells[2], c->cells[3], c->cells[4], c->cells[5]) : "",
               CanvasLive(dm, i) && CanvasUnsaved(c) ? "yes" : "no", PixelHash(&c->img), PixelHash(&c->base), c->refs,
               c->stale ? " (stale)" : "");
    }
    printf("paint:   %d unsaved edit%s, %d failed, paint history %lld bytes\n", MapEditUnsaved(&em->ed),
           MapEditUnsaved(&em->ed) == 1 ? "" : "s", em->ed.failedCount, MapEditPaintBytes(&em->ed));
    for (int i = 0; i < em->ed.failedCount; i++) printf("paint:   failed: %s\n", em->ed.failed[i].what);
}

// The map (or a package file) changed on disk: the hot reload's path (reload, rebase, paint hooks)
static bool ScriptReload(DrawMode *dm, EditMode *em, Designer *d, char *msg, int msgSize)
{
    char err[512];
    if (MS_Reload(scene, NULL, err, sizeof(err)) != MS_RELOADED) { snprintf(msg, msgSize, "the reload failed: %.400s", err); return false; }
    DrawModeBeforeSync(dm);
    EditModeSync(em, d, false);
    DrawModeSync(dm, em, false);
    for (int i = 0; i < em->ed.failedCount; i++) printf("paint:   failed: %s\n", em->ed.failed[i].what);
    snprintf(msg, msgSize, "reloaded: %.300s%s%.250s", em->messageTime > 0.0f ? em->message : "no unsaved edits",
             dm->messageTime > 0.0f ? "; " : "", dm->messageTime > 0.0f ? dm->message : "");
    return true;
}

// One stroke (or a dab: one stamp) of the script: LMB down on the first hit, then each next point
// one frame of LMB drag (px: through the screen-space sampling; uv: straight in texture space)
static bool ScriptStroke(DrawMode *dm, EditMode *em, Camera3D cam, int w, int h, bool uv, MS_ObjRef obj, int fs,
                         const Vector2 *pts, int count, char *msg, int msgSize)
{
    MS_SurfaceHit hit;
    if (uv) {
        if (!UvHit(obj, fs, pts[0].x, pts[0].y, &hit)) { snprintf(msg, msgSize, "no such surface point (face / slot, uv)"); return false; }
    } else hit = CastAt(cam, w, h, pts[0]);
    HoverHit(dm, &hit);
    if (!dm->hoverOk) { snprintf(msg, msgSize, "%s", dm->refusal[0] ? dm->refusal : "nothing to paint there"); return false; }
    bool reloaded;
    if (!StrokeBegin(dm, em, &hit, &reloaded)) { snprintf(msg, msgSize, "%s", dm->message); return false; }
    if (reloaded) {
        // (the surface has its new material)
        if (uv) UvHit(obj, fs, pts[0].x, pts[0].y, &hit);
        else hit = CastAt(cam, w, h, pts[0]);
        HoverHit(dm, &hit);
    }
    Probe a = ProbeHit(dm, &hit, pts[0]);
    if (a.ok) StampProbe(dm, &a);
    Flush(dm);
    for (int i = 1; i < count && dm->pen != PEN_FILL; i++) {
        if (uv) UvSegment(dm, obj, fs, pts[i - 1], pts[i]);
        else {
            MS_SurfaceHit hb = CastAt(cam, w, h, pts[i]);
            Probe b = ProbeHit(dm, &hb, pts[i]);
            StrokeSegment(dm, cam, w, h, a, b);
            a = b;
        }
        Flush(dm);     // (a frame: one upload per canvas)
    }
    int samples = dm->stroke ? dm->stroke->sampleCount : 0;
    StrokeEnd(dm, em);
    snprintf(msg, msgSize, "%s (%d sample%s)%s", dm->message, samples, samples == 1 ? "" : "s", a.ok || count > 1 ? "" : " (off the canvas)");
    return !dm->messageError;
}

// --paint-script <file> (docs/painting.md 14): one command per line, '#' starts a comment; each prints
// a "paint: file:line: ..." line, the first failing one stops the script (exit 1):
//   pen <round|square|soft|eraser|fill>   size <1-256>   hardness <0-1>   color <r> <g> <b> [<a>]
//   density <8-512>                    texels per metre of new canvases (and resize)
//   shared <on|off>                    paint the texture the surface shows (H)
//   pixel <on|off>                     new canvases, and the canvas painted last: filter nearest / not
//   faces <a b c d e f>                the face groups of the canvas painted last (EDIT_MATERIAL_SET, 5.2)
//   resize                             the canvas painted last to the size of the current density (5.3)
//   stroke px <x0> <y0> <x1> <y1> ...  an LMB drag through these screen pixels of the shot's camera
//                                      (one segment per frame, cut in screen space as the mouse is)
//   stroke uv <id N:i | entity HEX:i> <face|slot> <u0> <v0> [<u1> <v1> ...]
//                                      the same in texture space: a box face (0..5 = +x -x +y -y +z -z,
//                                      u v 0..1 on it) or a model slot (u v its texture coordinate)
//   dab px <x> <y> / dab uv <obj> <face|slot> <u> <v>   one stamp (fill: fills there)
//   hover px <x> <y>                   what the cursor is over there (the HUD and the brush cursor)
//   pick px <x> <y>                    the eyedropper (prints the colour; it becomes the colour)
//   undo / redo / save / print         print: the canvases, sizes, unsaved state, pixel hashes, history
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
    dm->bakeNow = true;         // (a first stroke on a shader box bakes at once: the script has no frames)
    int status = 0, lineNo = 0;
    Vector2 *pts = NULL;
    int ptCap = 0;
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
        char msg[700] = "";
        bool ok = false;
        float f[4];
        bool stroke = strcmp(wd[0], "stroke") == 0, dab = strcmp(wd[0], "dab") == 0;
        if (strcmp(wd[0], "pen") == 0 && n == 2) {
            static const char *const NAMES[PEN_COUNT] = { "round", "square", "soft", "eraser", "fill" };
            for (int k = 0; k < PEN_COUNT; k++) if (strcmp(wd[1], NAMES[k]) == 0) { dm->pen = (PenKind)k; ok = true; }
            snprintf(msg, sizeof(msg), ok ? "pen %s" : "pen <round|square|soft|eraser|fill>, not %s", wd[1]);
        } else if (strcmp(wd[0], "size") == 0 && n == 2 && ScriptNum(wd[1], &f[0]) && f[0] >= 1 && f[0] <= DRAW_SIZE_MAX) {
            dm->size = (int)f[0];
            ok = true;
            snprintf(msg, sizeof(msg), "size %d", dm->size);
        } else if (strcmp(wd[0], "hardness") == 0 && n == 2 && ScriptNum(wd[1], &f[0]) && f[0] >= 0 && f[0] <= 1) {
            dm->hardness = f[0];
            ok = true;
            snprintf(msg, sizeof(msg), "hardness %g", f[0]);
        } else if (strcmp(wd[0], "color") == 0 && (n == 4 || n == 5)) {
            ok = true;
            for (int k = 0; k < n - 1; k++) ok = ok && ScriptNum(wd[1 + k], &f[k]) && f[k] >= 0 && f[k] <= 255;
            if (ok) SetColor(dm, (Color){ (unsigned char)f[0], (unsigned char)f[1], (unsigned char)f[2], n == 5 ? (unsigned char)f[3] : 255 });
            if (ok) snprintf(msg, sizeof(msg), "color %d %d %d %d", dm->color.r, dm->color.g, dm->color.b, dm->color.a);
            else snprintf(msg, sizeof(msg), "color <r> <g> <b> [<a>] (0-255)");
        } else if (strcmp(wd[0], "density") == 0 && n == 2 && ScriptNum(wd[1], &f[0]) && f[0] >= 8 && f[0] <= 512) {
            dm->density = f[0];
            ok = true;
            snprintf(msg, sizeof(msg), "density %g texels/m", f[0]);
        } else if ((strcmp(wd[0], "shared") == 0 || strcmp(wd[0], "pixel") == 0) && n == 2 &&
                   (strcmp(wd[1], "on") == 0 || strcmp(wd[1], "off") == 0)) {
            bool on = wd[1][1] == 'n';
            if (wd[0][0] == 's') {
                dm->shared = on;
                ok = true;
                snprintf(msg, sizeof(msg), "shared %s", wd[1]);
            } else {
                dm->pixelLook = on;
                ok = dm->lastCanvas < 0 || PixelLook(dm, em, dm->lastCanvas, on) == DRAWMODE_RELOADED;
                snprintf(msg, sizeof(msg), "pixel look %s: %s", wd[1], dm->lastCanvas < 0 ? "new canvases" : dm->message);
            }
        } else if (strcmp(wd[0], "faces") == 0 && n == 7) {
            unsigned char cells[6];
            ok = true;
            for (int k = 0; k < 6 && ok; k++) {
                ok = ScriptNum(wd[1 + k], &f[0]) && f[0] >= 0 && f[0] <= 5;
                cells[k] = (unsigned char)(ok ? f[0] : 0);
            }
            if (ok) ok = LayoutCanvas(dm, em, dm->lastCanvas, cells) == DRAWMODE_RELOADED;
            snprintf(msg, sizeof(msg), "faces: %s", ok || dm->message[0] ? dm->message : "faces <a b c d e f> (0-5)");
        } else if (strcmp(wd[0], "resize") == 0 && n == 1) {
            ok = ResizeCanvas(dm, em, dm->lastCanvas) == DRAWMODE_RELOADED;
            snprintf(msg, sizeof(msg), "resize: %s", dm->message);
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
            if (!ok) snprintf(msg, sizeof(msg), "%s px <x> <y> ... | %s uv <id N:i | entity HEX:i> <face|slot> <u> <v> ...", wd[0], wd[0]);
            else ok = ScriptStroke(dm, em, cam, w, h, uv, obj, fs, pts, count, msg, sizeof(msg));
        } else if (strcmp(wd[0], "hover") == 0 && n == 4 && strcmp(wd[1], "px") == 0 && ScriptNum(wd[2], &f[0]) && ScriptNum(wd[3], &f[1])) {
            DrawModeHover(dm, cam, (Vector2){ f[0], f[1] }, w, h);
            ok = true;
            snprintf(msg, sizeof(msg), "hover %s%s", dm->hoverOk ? "paintable" : "not paintable", dm->refusal[0] ? TextFormat(": %s", dm->refusal) : "");
        } else if (strcmp(wd[0], "pick") == 0 && n == 4 && strcmp(wd[1], "px") == 0 && ScriptNum(wd[2], &f[0]) && ScriptNum(wd[3], &f[1])) {
            MS_SurfaceHit hit = CastAt(cam, w, h, (Vector2){ f[0], f[1] });
            Color c = { 0 };
            ok = Eyedropper(dm, &hit, &c);
            if (ok) SetColor(dm, c);
            snprintf(msg, sizeof(msg), ok ? "picked %d %d %d %d" : "nothing there", c.r, c.g, c.b, c.a);
        } else if ((strcmp(wd[0], "undo") == 0 || strcmp(wd[0], "redo") == 0) && n == 1) {
            ok = History(dm, em, d, wd[0][0] == 'u') == DRAWMODE_RELOADED;
            Flush(dm);
            snprintf(msg, sizeof(msg), "%s", dm->message);
        } else if (strcmp(wd[0], "save") == 0 && n == 1) {
            ok = DrawModeSave(dm, em, msg, sizeof(msg));
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
            snprintf(msg, sizeof(msg), "poll: %d reload%s, %d failed", reloads, reloads == 1 ? "" : "s", failed);
        } else if (strcmp(wd[0], "print") == 0 && n == 1) {
            PrintCanvases(dm, em);
            ok = true;
            snprintf(msg, sizeof(msg), "%d canvas%s", dm->canvasCount, dm->canvasCount == 1 ? "" : "es");
        } else if ((strcmp(wd[0], "disk") == 0 && n == 2) || (strcmp(wd[0], "put") == 0 && n == 3) || (strcmp(wd[0], "reload") == 0 && n == 1)) {
            // disk <file>: the map file becomes its bytes; put <file> <package path>: a package file
            // does (a PNG a request repainted); then the hot reload's path (reload, rebase)
            int size = 0;
            unsigned char *bytes = wd[0][0] == 'r' ? NULL : LoadFileData(wd[1], &size);
            char dst[MS_PATH_MAX * 2];
            snprintf(dst, sizeof(dst), "%s", wd[0][0] == 'p' ? TextFormat("%s%s", scene->root, wd[2]) : scene->path);
            if (wd[0][0] != 'r' && !bytes) snprintf(msg, sizeof(msg), "cannot read %s", wd[1]);
            else if (bytes && !SaveFileData(dst, bytes, size)) snprintf(msg, sizeof(msg), "cannot write %.600s", dst);
            else if (wd[0][0] == 'p') { ok = true; snprintf(msg, sizeof(msg), "wrote %.600s (no reload yet)", dst); }
            else ok = ScriptReload(dm, em, d, msg, sizeof(msg));
            if (bytes) UnloadFileData(bytes);
        } else snprintf(msg, sizeof(msg), "unknown command '%s' (pen size hardness color density shared pixel faces resize stroke dab "
                                          "hover pick undo redo save print disk put reload)", wd[0]);
        printf("paint: %s:%d: %s%s\n", path, lineNo, ok ? "" : "FAILED: ", msg);
        if (!ok) status = 1;
    }
    Flush(dm);
    dm->bakeNow = false;
    free(pts);
    fflush(stdout);
    UnloadFileText(text);
    return status;
}

// #endregion
