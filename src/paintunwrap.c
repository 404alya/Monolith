// Paint unwrap of a model file with xatlas (docs/painting.md 4.5; interface: paintunwrap.h)
//
// Per slot (in the order the slots first appear among the meshes) one xatlas atlas holds all of
// the slot's meshes: ComputeCharts with the default chart options (the mesh's own UVs only as a
// seam hint), then PackCharts into one R x R domain (R = texelsHint) with 2 texels of padding and
// bilinear borders. xatlas packs a fixed resolution into as many atlases as it needs, so the
// texels per unit start at its own estimate for R and shrink until everything fits into one.
// xatlas can leave folds inside a chart (scanned meshes: a few triangles whose paint UVs overlap
// others of the same chart), so the result is checked exactly (unorm16 triangles, separating axes
// on a grid) and every overlapping triangle becomes a chart of its own (an xatlas material group
// of its own) for another pass, until nothing overlaps. Triangles of zero area in 3D (they draw no
// pixel) and coincident duplicates (same three positions: they draw the same pixels) are exempt.
// xatlas is compiled with XA_MULTITHREADED=0 (build.sh): the same input gives the same bytes.
#include "paintunwrap.h"
#include "vendor/xatlas/xatlas_c.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ======================================================================================
// #region Paint unwrap
// ======================================================================================

#define UNWRAP_PADDING 2            // texels between charts at the texelsHint resolution
#define UNWRAP_MIN_TEXELS 16
#define UNWRAP_MAX_TEXELS 4096
#define UNWRAP_SHRINK 0.9f          // texels per unit, per repack that needed a second atlas
#define UNWRAP_MAX_REPACKS 80       // (0.9^80 ~ 2e-4 of the estimate: far below any real need)
#define UNWRAP_MAX_PASSES 8         // unwraps of a slot while folds overlap (each splits them off)

static void UnwrapError(char *err, int errSize, const char *fmt, ...)
{
    if (!err || errSize <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, (size_t)errSize, fmt, ap);
    va_end(ap);
}

static void FreeUnwrap(MS_PaintUnwrap *u)
{
    for (int m = 0; m < u->meshCount; m++) {
        free(u->meshes[m].source);
        free(u->meshes[m].uv);
        free(u->meshes[m].indices);
    }
    free(u->meshes);
    memset(u, 0, sizeof(*u));
}

// Index count of an input mesh: its triangles' corners
static int TriangleCorners(const PaintUnwrapInput *m)
{
    return m->indices ? m->indexCount : m->vertexCount;
}

// Checks one input mesh before xatlas sees it (xatlas only reports a bad index as an error code)
static bool CheckInput(const PaintUnwrapInput *m, int i, char *err, int errSize)
{
    int corners = TriangleCorners(m);
    if (m->vertexCount < 0 || m->indexCount < 0) {
        UnwrapError(err, errSize, "paint unwrap: mesh %d: negative count", i);
        return false;
    }
    if (corners % 3 != 0) {
        UnwrapError(err, errSize, "paint unwrap: mesh %d: %d indices are not whole triangles", i, corners);
        return false;
    }
    if (m->vertexCount > 0 && !m->positions) {
        UnwrapError(err, errSize, "paint unwrap: mesh %d: no positions", i);
        return false;
    }
    if (m->slot < 0) {
        UnwrapError(err, errSize, "paint unwrap: mesh %d: slot %d", i, m->slot);
        return false;
    }
    if (m->indices) {
        for (int k = 0; k < m->indexCount; k++) {
            if (m->indices[k] < (unsigned int)m->vertexCount) continue;
            UnwrapError(err, errSize, "paint unwrap: mesh %d: index %d is %u, the mesh has %d vertices", i, k,
                        m->indices[k], m->vertexCount);
            return false;
        }
    }
    return true;
}

// A paint UV coordinate (texels of the R x R domain) as unorm16
static unsigned short ToUnorm16(float texels, float r)
{
    float t = texels / r;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return (unsigned short)(t * 65535.0f + 0.5f);
}

// Packs the charts into one r x r atlas: xatlas's estimate first (single atlas, any size), then
// fixed r x r packs with fewer texels per unit until one atlas holds every chart
static bool PackIntoOne(xatlasAtlas *atlas, int r, char *err, int errSize)
{
    xatlasPackOptions pack;
    xatlasPackOptionsInit(&pack);
    pack.padding = UNWRAP_PADDING;
    pack.bilinear = true;
    pack.resolution = (uint32_t)r;
    pack.texelsPerUnit = 0.0f;              // estimate for r (no limit on the atlas size)
    xatlasPackCharts(atlas, &pack);
    if (atlas->chartCount == 0) return true;   // (only degenerate triangles: nothing to place)
    float tpu = atlas->texelsPerUnit;
    if (!(tpu > 0.0f)) {
        UnwrapError(err, errSize, "paint unwrap: xatlas found no texel scale (no charts?)");
        return false;
    }
    for (int i = 0; i < UNWRAP_MAX_REPACKS; i++) {
        pack.texelsPerUnit = tpu;
        xatlasPackCharts(atlas, &pack);
        if (atlas->atlasCount == 1 && atlas->width == (uint32_t)r && atlas->height == (uint32_t)r) return true;
        tpu *= UNWRAP_SHRINK;
    }
    UnwrapError(err, errSize, "paint unwrap: the charts don't fit into one %d x %d domain", r, r);
    return false;
}

// Frees the output meshes of list[0..count) (before a new pass writes them)
static void ClearSlot(const int *list, int count, MS_PaintUnwrap *out)
{
    for (int k = 0; k < count; k++) {
        MS_PaintUnwrapMesh *o = &out->meshes[list[k]];
        free(o->source);
        free(o->uv);
        free(o->indices);
        *o = (MS_PaintUnwrapMesh){ .slot = o->slot };
    }
}

// One xatlas run over the meshes list[0..count) (all of one slot) into out->meshes[list[k]].
// faceGroup[k] (NULL: all 0) keeps faces of different groups out of each other's charts.
static bool UnwrapPass(const PaintUnwrapInput *meshes, const int *list, int count, uint32_t *const *faceGroup, int r,
                       MS_PaintUnwrap *out, char *err, int errSize)
{
    xatlasAtlas *atlas = xatlasCreate();
    if (!atlas) {
        UnwrapError(err, errSize, "paint unwrap: out of memory");
        return false;
    }
    bool ok = false;
    int *added = malloc(sizeof(int) * (size_t)(count > 0 ? count : 1));    // atlas mesh -> input mesh
    int addedCount = 0;
    if (!added) {
        UnwrapError(err, errSize, "paint unwrap: out of memory");
        goto done;
    }

    // Meshes without triangles stay empty (xatlas refuses them)
    for (int k = 0; k < count; k++) {
        const PaintUnwrapInput *m = &meshes[list[k]];
        if (TriangleCorners(m) == 0) continue;
        xatlasMeshDecl decl;
        xatlasMeshDeclInit(&decl);
        decl.vertexPositionData = m->positions;
        decl.vertexPositionStride = sizeof(float) * 3;
        decl.vertexCount = (uint32_t)m->vertexCount;
        if (m->texcoords) {
            decl.vertexUvData = m->texcoords;
            decl.vertexUvStride = sizeof(float) * 2;
        }
        if (m->indices) {
            decl.indexData = m->indices;
            decl.indexCount = (uint32_t)m->indexCount;
            decl.indexFormat = XATLAS_INDEX_FORMAT_UINT32;
        }
        decl.faceMaterialData = faceGroup ? faceGroup[k] : NULL;
        xatlasAddMeshError e = xatlasAddMesh(atlas, &decl, (uint32_t)count);
        if (e != XATLAS_ADD_MESH_ERROR_SUCCESS) {
            UnwrapError(err, errSize, "paint unwrap: mesh %d: %s", list[k], xatlasAddMeshErrorString(e));
            goto done;
        }
        added[addedCount++] = list[k];
    }
    if (addedCount == 0) { ok = true; goto done; }

    xatlasChartOptions charts;
    xatlasChartOptionsInit(&charts);
    xatlasComputeCharts(atlas, &charts);
    if (!PackIntoOne(atlas, r, err, errSize)) goto done;
    if (atlas->meshCount != (uint32_t)addedCount) {
        UnwrapError(err, errSize, "paint unwrap: xatlas returned %u meshes for %d", atlas->meshCount, addedCount);
        goto done;
    }

    // Output: xatlas keeps every triangle at its source position (indexArray[3t + k])
    for (int k = 0; k < addedCount; k++) {
        const xatlasMesh *xm = &atlas->meshes[k];
        const PaintUnwrapInput *m = &meshes[added[k]];
        MS_PaintUnwrapMesh *o = &out->meshes[added[k]];
        if (xm->indexCount != (uint32_t)TriangleCorners(m)) {
            UnwrapError(err, errSize, "paint unwrap: mesh %d: xatlas returned %u indices for %d", added[k],
                        xm->indexCount, TriangleCorners(m));
            goto done;
        }
        o->vertexCount = (int)xm->vertexCount;
        o->indexCount = (int)xm->indexCount;
        o->source = malloc(sizeof(unsigned int) * (size_t)(xm->vertexCount ? xm->vertexCount : 1));
        o->uv = malloc(sizeof(unsigned short) * 2 * (size_t)(xm->vertexCount ? xm->vertexCount : 1));
        o->indices = malloc(sizeof(unsigned int) * (size_t)(xm->indexCount ? xm->indexCount : 1));
        if (!o->source || !o->uv || !o->indices) {
            UnwrapError(err, errSize, "paint unwrap: out of memory");
            goto done;
        }
        for (uint32_t v = 0; v < xm->vertexCount; v++) {
            const xatlasVertex *xv = &xm->vertexArray[v];
            if (xv->xref >= (uint32_t)m->vertexCount) {
                UnwrapError(err, errSize, "paint unwrap: mesh %d: xatlas vertex %u refers to vertex %u", added[k], v, xv->xref);
                goto done;
            }
            o->source[v] = xv->xref;
            bool charted = xv->atlasIndex >= 0;     // (-1: a degenerate triangle's vertex)
            o->uv[2 * v] = charted ? ToUnorm16(xv->uv[0], (float)r) : 0;
            o->uv[2 * v + 1] = charted ? ToUnorm16(xv->uv[1], (float)r) : 0;
        }
        for (uint32_t i = 0; i < xm->indexCount; i++) {
            if (xm->indexArray[i] >= xm->vertexCount) {
                UnwrapError(err, errSize, "paint unwrap: mesh %d: xatlas index %u out of range", added[k], i);
                goto done;
            }
            o->indices[i] = xm->indexArray[i];
        }
    }
    ok = true;

done:
    free(added);
    xatlasDestroy(atlas);
    return ok;
}

// A triangle of the slot's result for the overlap search: its paint UVs (unorm16), bounds and origin
typedef struct {
    int64_t x[3], y[3];
    int minX, minY, maxX, maxY;     // grid cells
    int k, t;                       // list[k]'s mesh, triangle t
} UvTriangle;

#define OVERLAP_GRID 128            // cells per axis of the overlap search (512 unorm16 units each)

// The input positions of corner c of triangle t
static const float *CornerPosition(const PaintUnwrapInput *m, int t, int c)
{
    unsigned int v = m->indices ? m->indices[3 * t + c] : (unsigned int)(3 * t + c);
    return &m->positions[3 * (size_t)v];
}

// Zero area in 3D (it draws no pixel, so where its paint lands doesn't matter)
static bool Degenerate3D(const PaintUnwrapInput *m, int t)
{
    const float *a = CornerPosition(m, t, 0), *b = CornerPosition(m, t, 1), *c = CornerPosition(m, t, 2);
    double e[2][3];
    for (int i = 0; i < 3; i++) { e[0][i] = (double)b[i] - a[i]; e[1][i] = (double)c[i] - a[i]; }
    double cx = e[0][1] * e[1][2] - e[0][2] * e[1][1], cy = e[0][2] * e[1][0] - e[0][0] * e[1][2];
    double cz = e[0][0] * e[1][1] - e[0][1] * e[1][0];
    return cx == 0.0 && cy == 0.0 && cz == 0.0;
}

static int ComparePositions(const void *a, const void *b)
{
    const float *x = *(const float *const *)a, *y = *(const float *const *)b;
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

// The same three positions in any order: a duplicated face, which draws to the same pixels as the
// other one and may share its paint
static bool Coincident(const PaintUnwrapInput *ma, int ta, const PaintUnwrapInput *mb, int tb)
{
    const float *p[3], *q[3];
    for (int c = 0; c < 3; c++) { p[c] = CornerPosition(ma, ta, c); q[c] = CornerPosition(mb, tb, c); }
    qsort(p, 3, sizeof(p[0]), ComparePositions);
    qsort(q, 3, sizeof(q[0]), ComparePositions);
    for (int c = 0; c < 3; c++) if (memcmp(p[c], q[c], sizeof(float) * 3) != 0) return false;
    return true;
}

// Do the interiors of two UV triangles intersect? Separating axes (the six edge normals), exact in
// integers; touching (a shared edge or corner) is not an overlap.
static bool InteriorsOverlap(const UvTriangle *a, const UvTriangle *b)
{
    const UvTriangle *tri[2] = { a, b };
    for (int s = 0; s < 2; s++)
        for (int e = 0; e < 3; e++) {
            const UvTriangle *T = tri[s];
            int64_t nx = -(T->y[(e + 1) % 3] - T->y[e]), ny = T->x[(e + 1) % 3] - T->x[e];
            int64_t amin = INT64_MAX, amax = INT64_MIN, bmin = INT64_MAX, bmax = INT64_MIN;
            for (int i = 0; i < 3; i++) {
                int64_t da = nx * a->x[i] + ny * a->y[i], db = nx * b->x[i] + ny * b->y[i];
                amin = da < amin ? da : amin; amax = da > amax ? da : amax;
                bmin = db < bmin ? db : bmin; bmax = db > bmax ? db : bmax;
            }
            if (amax <= bmin || bmax <= amin) return false;
        }
    return true;
}

// Marks in flag[k][t] every triangle of the slot's result whose paint UVs overlap another one's
// (folds xatlas left in a chart); returns how many, -1 when out of memory. Triangles that are
// degenerate in UV or 3D, and coincident pairs, don't count.
static int FindOverlaps(const PaintUnwrapInput *meshes, const int *list, int count, const MS_PaintUnwrap *out,
                        bool **flag)
{
    int total = 0, found = 0;
    for (int k = 0; k < count; k++) {
        total += out->meshes[list[k]].indexCount / 3;
        memset(flag[k], 0, sizeof(bool) * (size_t)(out->meshes[list[k]].indexCount / 3));
    }
    UvTriangle *tris = malloc(sizeof(UvTriangle) * (size_t)(total + 1));
    int *cellStart = calloc((size_t)OVERLAP_GRID * OVERLAP_GRID + 1, sizeof(int));
    int *cellFill = NULL, *cells = NULL;
    if (!tris || !cellStart) { found = -1; goto done; }

    // The triangles that cover area, and how many land in each grid cell
    int n = 0;
    size_t refs = 0;
    for (int k = 0; k < count; k++) {
        const MS_PaintUnwrapMesh *o = &out->meshes[list[k]];
        const PaintUnwrapInput *m = &meshes[list[k]];
        for (int t = 0; t < o->indexCount / 3; t++) {
            UvTriangle *T = &tris[n];
            for (int c = 0; c < 3; c++) {
                T->x[c] = o->uv[2 * o->indices[3 * t + c]];
                T->y[c] = o->uv[2 * o->indices[3 * t + c] + 1];
            }
            int64_t area = (T->x[1] - T->x[0]) * (T->y[2] - T->y[0]) - (T->y[1] - T->y[0]) * (T->x[2] - T->x[0]);
            if (area == 0 || Degenerate3D(m, t)) continue;
            int64_t lx = T->x[0], hx = T->x[0], ly = T->y[0], hy = T->y[0];
            for (int c = 1; c < 3; c++) {
                lx = T->x[c] < lx ? T->x[c] : lx; hx = T->x[c] > hx ? T->x[c] : hx;
                ly = T->y[c] < ly ? T->y[c] : ly; hy = T->y[c] > hy ? T->y[c] : hy;
            }
            const int64_t cell = 65536 / OVERLAP_GRID;
            T->minX = (int)(lx / cell); T->maxX = (int)(hx / cell); T->minY = (int)(ly / cell); T->maxY = (int)(hy / cell);
            T->k = k;
            T->t = t;
            for (int y = T->minY; y <= T->maxY; y++)
                for (int x = T->minX; x <= T->maxX; x++) cellStart[y * OVERLAP_GRID + x + 1]++;
            refs += (size_t)(T->maxX - T->minX + 1) * (size_t)(T->maxY - T->minY + 1);
            n++;
        }
    }
    for (int c = 0; c < OVERLAP_GRID * OVERLAP_GRID; c++) cellStart[c + 1] += cellStart[c];
    cellFill = malloc(sizeof(int) * (size_t)OVERLAP_GRID * OVERLAP_GRID);
    cells = malloc(sizeof(int) * (refs + 1));
    if (!cellFill || !cells) { found = -1; goto done; }
    memcpy(cellFill, cellStart, sizeof(int) * (size_t)OVERLAP_GRID * OVERLAP_GRID);
    for (int i = 0; i < n; i++)
        for (int y = tris[i].minY; y <= tris[i].maxY; y++)
            for (int x = tris[i].minX; x <= tris[i].maxX; x++) cells[cellFill[y * OVERLAP_GRID + x]++] = i;

    // Every pair that shares a cell
    for (int c = 0; c < OVERLAP_GRID * OVERLAP_GRID; c++)
        for (int i = cellStart[c]; i < cellStart[c + 1]; i++)
            for (int j = i + 1; j < cellStart[c + 1]; j++) {
                const UvTriangle *A = &tris[cells[i]], *B = &tris[cells[j]];
                bool *fa = &flag[A->k][A->t], *fb = &flag[B->k][B->t];
                if (*fa && *fb) continue;
                if (!InteriorsOverlap(A, B) || Coincident(&meshes[list[A->k]], A->t, &meshes[list[B->k]], B->t)) continue;
                found += !*fa + !*fb;
                *fa = *fb = true;
            }

done:
    free(tris);
    free(cellStart);
    free(cellFill);
    free(cells);
    return found;
}

// Unwraps one slot: a pass, then while folds overlap, each overlapping triangle becomes a chart of
// its own (an xatlas material group of its own) and the slot is unwrapped again
static bool UnwrapSlot(const PaintUnwrapInput *meshes, const int *list, int count, int r, MS_PaintUnwrap *out,
                       char *err, int errSize)
{
    uint32_t **group = calloc((size_t)(count > 0 ? count : 1), sizeof(uint32_t *));
    bool **flag = calloc((size_t)(count > 0 ? count : 1), sizeof(bool *));
    bool ok = group && flag;
    for (int k = 0; ok && k < count; k++) {
        size_t faces = (size_t)TriangleCorners(&meshes[list[k]]) / 3 + 1;
        group[k] = calloc(faces, sizeof(uint32_t));
        flag[k] = calloc(faces, sizeof(bool));
        ok = group[k] && flag[k];
    }
    if (!ok) UnwrapError(err, errSize, "paint unwrap: out of memory");
    for (int pass = 0; ok; pass++) {
        ClearSlot(list, count, out);
        ok = UnwrapPass(meshes, list, count, pass ? group : NULL, r, out, err, errSize);
        if (!ok) break;
        int folds = FindOverlaps(meshes, list, count, out, flag);
        if (folds == 0) break;
        if (folds < 0) { UnwrapError(err, errSize, "paint unwrap: out of memory"); ok = false; break; }
        if (pass == UNWRAP_MAX_PASSES - 1) {
            UnwrapError(err, errSize, "paint unwrap: slot %d: %d triangles still overlap after %d passes",
                        meshes[list[0]].slot, folds, UNWRAP_MAX_PASSES);
            ok = false;
            break;
        }
        // (the flags stay set: a triangle once split off stays on its own)
        for (int k = 0; k < count; k++)
            for (int t = 0; t < TriangleCorners(&meshes[list[k]]) / 3; t++)
                if (flag[k][t]) group[k][t] = (uint32_t)t + 1;
    }
    for (int k = 0; k < count && group && flag; k++) { free(group[k]); free(flag[k]); }
    free(group);
    free(flag);
    return ok;
}

bool PaintUnwrapMeshes(const PaintUnwrapInput *meshes, int meshCount, int texelsHint, MS_PaintUnwrap *out,
                       char *err, int errSize)
{
    memset(out, 0, sizeof(*out));
    if (meshCount < 0 || (meshCount > 0 && !meshes)) {
        UnwrapError(err, errSize, "paint unwrap: no meshes");
        return false;
    }
    for (int i = 0; i < meshCount; i++)
        if (!CheckInput(&meshes[i], i, err, errSize)) return false;
    int r = texelsHint < UNWRAP_MIN_TEXELS ? UNWRAP_MIN_TEXELS : texelsHint > UNWRAP_MAX_TEXELS ? UNWRAP_MAX_TEXELS : texelsHint;

    // Quiet (warnings would go to stdout) and the C library's allocator
    xatlasSetPrint(NULL, false);
    out->meshes = calloc((size_t)(meshCount > 0 ? meshCount : 1), sizeof(MS_PaintUnwrapMesh));
    bool *done = calloc((size_t)(meshCount > 0 ? meshCount : 1), sizeof(bool));
    int *list = malloc(sizeof(int) * (size_t)(meshCount > 0 ? meshCount : 1));
    bool ok = out->meshes && done && list;
    if (!ok) UnwrapError(err, errSize, "paint unwrap: out of memory");
    else {
        out->meshCount = meshCount;
        for (int i = 0; i < meshCount; i++) out->meshes[i].slot = meshes[i].slot;
    }

    // One atlas per slot, the slots in the order they first appear
    for (int i = 0; ok && i < meshCount; i++) {
        if (done[i]) continue;
        int count = 0;
        for (int j = i; j < meshCount; j++) {
            if (done[j] || meshes[j].slot != meshes[i].slot) continue;
            list[count++] = j;
            done[j] = true;
        }
        ok = UnwrapSlot(meshes, list, count, r, out, err, errSize);
    }
    free(done);
    free(list);
    if (!ok) FreeUnwrap(out);
    return ok;
}

// #endregion

// ======================================================================================
// #region Model files of a loaded scene
// ======================================================================================

// float16 (R3D_Vertex.texcoord) -> float
static float HalfToFloat(uint16_t h)
{
    uint32_t sign = (uint32_t)(h >> 15) << 31, exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu, bits;
    if (exp == 0) {
        if (mant == 0) bits = sign;
        else {      // subnormal: normalise
            exp = 127 - 15 + 1;
            while (!(mant & 0x400u)) { mant <<= 1; exp--; }
            bits = sign | (exp << 23) | ((mant & 0x3FFu) << 13);
        }
    } else if (exp == 31) bits = sign | 0x7F800000u | (mant << 13);
    else bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

// FNV-1a 64 of a file's bytes (the sidecar's staleness check); false when it can't be read
static bool HashFile(const char *path, unsigned long long *hash)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned long long h = 14695981039346656037ULL;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        for (size_t i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ULL; }
    bool ok = !ferror(f);
    fclose(f);
    *hash = h;
    return ok;
}

bool PaintUnwrapModelFile(const MS_Scene *scene, int file, int texelsHint, MS_PaintUnwrap *out, char *err, int errSize)
{
    memset(out, 0, sizeof(*out));
    if (!scene || file < 0 || file >= scene->modelFileCount) {
        UnwrapError(err, errSize, "paint unwrap: no model file %d", file);
        return false;
    }
    const MS_ModelFile *mf = &scene->modelFiles[file];
    if (!mf->loaded) {
        UnwrapError(err, errSize, "paint unwrap: %s is not loaded", mf->path);
        return false;
    }
    if (mf->model.skeleton.boneCount > 0) {
        UnwrapError(err, errSize, "paint unwrap: %s is skinned", mf->path);
        return false;
    }
    int meshCount = mf->model.meshCount;
    PaintUnwrapInput *in = calloc((size_t)(meshCount > 0 ? meshCount : 1), sizeof(PaintUnwrapInput));
    float **arrays = calloc((size_t)(meshCount > 0 ? meshCount : 1) * 2, sizeof(float *));
    bool ok = in && arrays;
    if (!ok) UnwrapError(err, errSize, "paint unwrap: out of memory");

    // Positions and UVs as floats (R3D keeps UVs as float16)
    for (int m = 0; ok && m < meshCount; m++) {
        R3D_MeshData md;
        if (!MS_ModelMeshData(scene, file, m, &md)) {
            UnwrapError(err, errSize, "paint unwrap: %s: no mesh data for mesh %d", mf->path, m);
            ok = false;
            break;
        }
        int vc = md.vertexCount > 0 ? md.vertexCount : 0;
        float *pos = arrays[2 * m] = malloc(sizeof(float) * 3 * (size_t)(vc ? vc : 1));
        float *uv = arrays[2 * m + 1] = malloc(sizeof(float) * 2 * (size_t)(vc ? vc : 1));
        if (!pos || !uv) {
            UnwrapError(err, errSize, "paint unwrap: out of memory");
            ok = false;
            break;
        }
        for (int v = 0; v < vc; v++) {
            const R3D_Vertex *rv = &md.vertices[v];
            pos[3 * v] = rv->position.x;
            pos[3 * v + 1] = rv->position.y;
            pos[3 * v + 2] = rv->position.z;
            uv[2 * v] = HalfToFloat(rv->texcoord[0]);
            uv[2 * v + 1] = HalfToFloat(rv->texcoord[1]);
        }
        int slot = mf->model.meshMaterials ? mf->model.meshMaterials[m] : 0;
        in[m] = (PaintUnwrapInput){
            .positions = pos, .texcoords = uv,
            .indices = md.indexCount > 0 ? md.indices : NULL,
            .vertexCount = vc, .indexCount = md.indexCount > 0 ? md.indexCount : 0,
            .slot = slot < 0 ? 0 : slot,
        };
    }
    if (ok) ok = PaintUnwrapMeshes(in, meshCount, texelsHint, out, err, errSize);
    if (ok) {
        size_t size = strlen(scene->root) + strlen(mf->path) + 1;
        char *full = malloc(size);
        if (full) snprintf(full, size, "%s%s", scene->root, mf->path);
        if (!full || !HashFile(full, &out->fileHash)) {
            UnwrapError(err, errSize, "paint unwrap: can't read %s%s", scene->root, mf->path);
            FreeUnwrap(out);
            ok = false;
        }
        free(full);
    }
    if (arrays)
        for (int i = 0; i < 2 * meshCount; i++) free(arrays[i]);
    free(arrays);
    free(in);
    return ok;
}

// #endregion
