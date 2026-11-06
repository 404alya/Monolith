// Test of the paint unwrap (src/paintunwrap.c, xatlas; docs/painting.md 4.5) without the scene
// runtime's model loading: synthetic meshes (a cube with every face on the same UVs, a cube of 8
// shared vertices without UVs, a mirrored quad pair whose UVs overlap, several slots and meshes,
// an empty mesh, a non-indexed mesh, a degenerate triangle) and model files read with R3D's
// importer exactly as the runtime reads them (R3D_LoadModelEx with R3D_IMPORT_RETAIN_MESH_DATA,
// in a hidden window: R3D's model loader needs a GL context). Built and run under ASan + UBSan by
// tests/unit/paintunwrap_test.sh:
//
//   paintunwrap_test [--scene <package>] [--scratch <dir>] <model file>...
//
// Checks per input, each slot on its own: the triangles keep their order and count (new vertex
// sources give back the input's index of every corner), every source and index is valid, no
// texel centre of a 1024 x 1024 raster is covered by two triangles (exact integer raster with a
// top-left rule, so neighbours sharing an edge never both count), the charts (connected triangles)
// are at least 2 empty texels apart at the texelsHint resolution (conservative raster), and two
// runs give the same bytes. With --scratch, each unwrap is written with MS_PaintUnwrapWrite and
// read back with MS_PaintUnwrapRead (skipped while those are the runtime's phase-0 stubs). With
// --scene, PaintUnwrapModelFile on every model file of the loaded package must give the same bytes
// as the direct unwrap of that file (skipped while MS_ModelMeshData is a stub).
#define _POSIX_C_SOURCE 199309L     // clock_gettime
#include "monolith_scene.h"
#include "paintunwrap.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEXELS 1024             // texelsHint of every unwrap here (the editor's PAINTUNWRAP_TEXELS)
#define OVERLAP_RES 1024        // the texel-centre raster of the overlap check
#define MIN_GAP 2               // empty texels between two charts at TEXELS

static int failures;

#define FAIL(...) do { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } while (0)

static double Now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ======================================================================================
// #region Checks
// ======================================================================================

static int Corners(const PaintUnwrapInput *m) { return m->indices ? m->indexCount : m->vertexCount; }
static unsigned int InputIndex(const PaintUnwrapInput *m, int i) { return m->indices ? m->indices[i] : (unsigned int)i; }

// Union-find over a mesh's new vertices: triangles that share a vertex are one chart
static int Find(int *parent, int v)
{
    while (parent[v] != v) { parent[v] = parent[parent[v]]; v = parent[v]; }
    return v;
}

// Twice the signed area of a triangle of unorm16 UVs (0: degenerate, covers no texel)
static int64_t Area2(const unsigned short *a, const unsigned short *b, const unsigned short *c)
{
    return ((int64_t)b[0] - a[0]) * ((int64_t)c[1] - a[1]) - ((int64_t)b[1] - a[1]) * ((int64_t)c[0] - a[0]);
}

// The texel-centre raster of one slot: per texel the first triangle that covers it (+1) and whether
// a second one does too (1: a coincident triangle, the same three positions, e.g. a duplicated
// face, which draws to the same pixels and may share paint; 2: any other, an overlap)
typedef struct {
    const PaintUnwrapInput *in;
    const MS_PaintUnwrap *u;
    int *triMesh, *triIndex;    // per slot triangle: its mesh and first corner
    int *first;
    unsigned char *flag;
} CentreRaster;

static int ComparePos(const void *a, const void *b)
{
    const float *x = a, *y = b;
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

// Do slot triangles i and j have the same three positions (in any order)?
static bool Coincident(const CentreRaster *cr, int i, int j)
{
    float p[2][3][3];
    int tri[2] = { i, j };
    for (int k = 0; k < 2; k++) {
        const PaintUnwrapInput *im = &cr->in[cr->triMesh[tri[k]]];
        for (int c = 0; c < 3; c++) memcpy(p[k][c], &im->positions[3 * InputIndex(im, cr->triIndex[tri[k]] + c)], sizeof(float) * 3);
        qsort(p[k], 3, sizeof(p[k][0]), ComparePos);
    }
    return memcmp(p[0], p[1], sizeof(p[0])) == 0;
}

// Zero area in 3D (the triangle draws no pixel, so its paint UVs don't matter: not checked)
static bool Degenerate3D(const PaintUnwrapInput *m, int corner)
{
    const float *p[3];
    for (int c = 0; c < 3; c++) p[c] = &m->positions[3 * InputIndex(m, corner + c)];
    double e[2][3];
    for (int i = 0; i < 3; i++) { e[0][i] = (double)p[1][i] - p[0][i]; e[1][i] = (double)p[2][i] - p[0][i]; }
    return e[0][1] * e[1][2] - e[0][2] * e[1][1] == 0.0 && e[0][2] * e[1][0] - e[0][0] * e[1][2] == 0.0 &&
           e[0][0] * e[1][1] - e[0][1] * e[1][0] == 0.0;
}

// Exact raster by texel centres (top-left rule) into cr (OVERLAP_RES^2). Coordinates are scaled by
// 2 * OVERLAP_RES * 65535: a vertex is q * 2N, the centre of texel x is (2x + 1) * 65535.
static void RasterCentres(const unsigned short *a, const unsigned short *b, const unsigned short *c, int tri, CentreRaster *cr)
{
    const int64_t N = OVERLAP_RES;
    int64_t area = Area2(a, b, c);
    if (area == 0) return;
    if (area < 0) { const unsigned short *t = b; b = c; c = t; }       // counter-clockwise
    int64_t p[3][2] = { { a[0] * 2 * N, a[1] * 2 * N }, { b[0] * 2 * N, b[1] * 2 * N }, { c[0] * 2 * N, c[1] * 2 * N } };
    int64_t minX = p[0][0], maxX = p[0][0], minY = p[0][1], maxY = p[0][1];
    for (int i = 1; i < 3; i++) {
        minX = p[i][0] < minX ? p[i][0] : minX; maxX = p[i][0] > maxX ? p[i][0] : maxX;
        minY = p[i][1] < minY ? p[i][1] : minY; maxY = p[i][1] > maxY ? p[i][1] : maxY;
    }
    int64_t x0 = minX / (2 * 65535) - 1, x1 = maxX / (2 * 65535) + 1, y0 = minY / (2 * 65535) - 1, y1 = maxY / (2 * 65535) + 1;
    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 > N - 1 ? N - 1 : x1; y1 = y1 > N - 1 ? N - 1 : y1;
    for (int64_t y = y0; y <= y1; y++)
        for (int64_t x = x0; x <= x1; x++) {
            int64_t cx = (2 * x + 1) * 65535, cy = (2 * y + 1) * 65535;
            bool inside = true;
            for (int e = 0; e < 3 && inside; e++) {
                const int64_t *s = p[e], *t = p[(e + 1) % 3];
                int64_t dx = t[0] - s[0], dy = t[1] - s[1];
                int64_t w = dx * (cy - s[1]) - dy * (cx - s[0]);
                // (an edge on the centre belongs to one of the two triangles that share it)
                bool owns = dy > 0 || (dy == 0 && dx < 0);
                inside = w > 0 || (w == 0 && owns);
            }
            if (!inside) continue;
            size_t at = (size_t)(y * N + x);
            if (!cr->first[at]) { cr->first[at] = tri + 1; continue; }
            unsigned char kind = Coincident(cr, cr->first[at] - 1, tri) ? 1 : 2;
            if (kind > cr->flag[at]) cr->flag[at] = kind;
        }
}

// Conservative raster at TEXELS: every texel whose square touches the triangle gets `chart`
// (or -1 when another chart is there already: footprints overlap)
static void RasterChart(const unsigned short *a, const unsigned short *b, const unsigned short *c, int chart, int *owner)
{
    const int R = TEXELS;
    double p[3][2];
    const unsigned short *v[3] = { a, b, c };
    for (int i = 0; i < 3; i++) { p[i][0] = v[i][0] / 65535.0 * R; p[i][1] = v[i][1] / 65535.0 * R; }
    double minX = fmin(p[0][0], fmin(p[1][0], p[2][0])), maxX = fmax(p[0][0], fmax(p[1][0], p[2][0]));
    double minY = fmin(p[0][1], fmin(p[1][1], p[2][1])), maxY = fmax(p[0][1], fmax(p[1][1], p[2][1]));
    int x0 = (int)floor(minX) - 1, x1 = (int)floor(maxX), y0 = (int)floor(minY) - 1, y1 = (int)floor(maxY);
    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 > R - 1 ? R - 1 : x1; y1 = y1 > R - 1 ? R - 1 : y1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            if (x + 1 < minX || x > maxX || y + 1 < minY || y > maxY) continue;
            // Separating axis: each edge's normal
            bool separated = false;
            for (int e = 0; e < 3 && !separated; e++) {
                double nx = -(p[(e + 1) % 3][1] - p[e][1]), ny = p[(e + 1) % 3][0] - p[e][0];
                double tmin = INFINITY, tmax = -INFINITY, smin = INFINITY, smax = -INFINITY;
                for (int i = 0; i < 3; i++) {
                    double d = nx * p[i][0] + ny * p[i][1];
                    tmin = fmin(tmin, d); tmax = fmax(tmax, d);
                }
                for (int i = 0; i < 4; i++) {
                    double d = nx * (x + (i & 1)) + ny * (y + (i >> 1));
                    smin = fmin(smin, d); smax = fmax(smax, d);
                }
                separated = smax < tmin || smin > tmax;
            }
            if (separated) continue;
            int *o = &owner[y * R + x];
            if (*o == 0) *o = chart + 1;
            else if (*o != chart + 1) *o = -1;
        }
}

// A vertex by its paint UV (for joining a chart's split vertices)
typedef struct { uint32_t key; int vertex; } UvKey;

static int CompareKeys(const void *a, const void *b)
{
    const UvKey *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return (x->vertex > y->vertex) - (x->vertex < y->vertex);
}

typedef struct {
    int charts, triangles, overlap, coincident, footprintOverlap, minGap;  // minGap: MIN_GAP + 1 = "more"
} SlotStats;

// The checks of one slot: overlap of texel centres, chart footprints and their gaps
static SlotStats CheckSlot(const MS_PaintUnwrap *u, const PaintUnwrapInput *in, int slot)
{
    SlotStats st = { 0, 0, 0, 0, 0, MIN_GAP + 1 };
    int triangles = 0;
    for (int m = 0; m < u->meshCount; m++) if (u->meshes[m].slot == slot) triangles += u->meshes[m].indexCount / 3;
    CentreRaster cr = {
        in, u, malloc(sizeof(int) * (size_t)(triangles + 1)), malloc(sizeof(int) * (size_t)(triangles + 1)),
        calloc((size_t)OVERLAP_RES * OVERLAP_RES, sizeof(int)), calloc((size_t)OVERLAP_RES * OVERLAP_RES, 1),
    };
    int *owner = calloc((size_t)TEXELS * TEXELS, sizeof(int));
    if (!cr.triMesh || !cr.triIndex || !cr.first || !cr.flag || !owner) {
        FAIL("out of memory");
        free(cr.triMesh); free(cr.triIndex); free(cr.first); free(cr.flag); free(owner);
        return st;
    }
    // Charts: triangles joined by a shared vertex, or by vertices at the same paint UV (xatlas keeps
    // the input's split vertices apart inside a chart; two charts never share a UV point)
    int total = 0;
    for (int m = 0; m < u->meshCount; m++) if (u->meshes[m].slot == slot) total += u->meshes[m].vertexCount;
    int *parent = malloc(sizeof(int) * (size_t)(total + 1)), *chartOf = malloc(sizeof(int) * (size_t)(total + 1));
    UvKey *keys = malloc(sizeof(UvKey) * (size_t)(total + 1));
    if (!parent || !chartOf || !keys) {
        FAIL("out of memory");
        free(parent); free(chartOf); free(keys); free(cr.triMesh); free(cr.triIndex); free(cr.first); free(cr.flag); free(owner);
        return st;
    }
    for (int v = 0; v < total; v++) { parent[v] = v; chartOf[v] = -1; }
    for (int m = 0, base = 0; m < u->meshCount; m++) {
        const MS_PaintUnwrapMesh *o = &u->meshes[m];
        if (o->slot != slot) continue;
        for (int t = 0; t + 2 < o->indexCount; t += 3) {
            int r0 = Find(parent, base + (int)o->indices[t]), r1 = Find(parent, base + (int)o->indices[t + 1]);
            parent[r1] = r0;
            parent[Find(parent, base + (int)o->indices[t + 2])] = r0;
        }
        for (int v = 0; v < o->vertexCount; v++)
            keys[base + v] = (UvKey){ (uint32_t)o->uv[2 * v] << 16 | o->uv[2 * v + 1], base + v };
        base += o->vertexCount;
    }
    qsort(keys, (size_t)total, sizeof(UvKey), CompareKeys);
    for (int i = 1; i < total; i++)
        if (keys[i].key == keys[i - 1].key) parent[Find(parent, keys[i].vertex)] = Find(parent, keys[i - 1].vertex);

    int charts = 0;
    for (int m = 0, base = 0; m < u->meshCount; m++) {
        const MS_PaintUnwrapMesh *o = &u->meshes[m];
        if (o->slot != slot) continue;
        for (int t = 0; t + 2 < o->indexCount; t += 3) {
            const unsigned short *a = &o->uv[2 * o->indices[t]], *b = &o->uv[2 * o->indices[t + 1]], *c = &o->uv[2 * o->indices[t + 2]];
            int tri = st.triangles++;
            cr.triMesh[tri] = m;
            cr.triIndex[tri] = t;
            if (Area2(a, b, c) == 0 || Degenerate3D(&in[m], t)) continue;
            int r = Find(parent, base + (int)o->indices[t]);
            if (chartOf[r] < 0) chartOf[r] = charts++;
            RasterCentres(a, b, c, tri, &cr);
            RasterChart(a, b, c, chartOf[r], owner);
        }
        base += o->vertexCount;
    }
    free(parent);
    free(chartOf);
    free(keys);
    int chartBase = charts;
    st.charts = chartBase;
    for (size_t i = 0; i < (size_t)OVERLAP_RES * OVERLAP_RES; i++) {
        st.overlap += cr.flag[i] == 2;
        st.coincident += cr.flag[i] == 1;
    }
    for (int y = 0; y < TEXELS; y++)
        for (int x = 0; x < TEXELS; x++) {
            int c = owner[y * TEXELS + x];
            if (c < 0) { st.footprintOverlap++; continue; }
            if (c == 0) continue;
            for (int dy = -MIN_GAP; dy <= MIN_GAP; dy++)
                for (int dx = -MIN_GAP; dx <= MIN_GAP; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= TEXELS || ny >= TEXELS) continue;
                    int d = owner[ny * TEXELS + nx];
                    if (d == 0 || d == c) continue;
                    int gap = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) - 1;
                    if (gap < st.minGap) st.minGap = gap;
                }
        }
    free(cr.triMesh);
    free(cr.triIndex);
    free(cr.first);
    free(cr.flag);
    free(owner);
    return st;
}

// Byte equality of two unwraps
static bool SameUnwrap(const MS_PaintUnwrap *a, const MS_PaintUnwrap *b)
{
    if (a->meshCount != b->meshCount || a->fileHash != b->fileHash) return false;
    for (int m = 0; m < a->meshCount; m++) {
        const MS_PaintUnwrapMesh *x = &a->meshes[m], *y = &b->meshes[m];
        if (x->slot != y->slot || x->vertexCount != y->vertexCount || x->indexCount != y->indexCount) return false;
        if (x->vertexCount && (memcmp(x->source, y->source, sizeof(unsigned int) * (size_t)x->vertexCount) ||
                               memcmp(x->uv, y->uv, sizeof(unsigned short) * 2 * (size_t)x->vertexCount)))
            return false;
        if (x->indexCount && memcmp(x->indices, y->indices, sizeof(unsigned int) * (size_t)x->indexCount)) return false;
    }
    return true;
}

static const char *scratchDir;

// Unwraps `in` twice and checks everything; name labels the output line
static void Check(const char *name, const PaintUnwrapInput *in, int meshCount)
{
    MS_PaintUnwrap u, again;
    double t0 = Now();
    bool ok = PaintUnwrapMeshes(in, meshCount, TEXELS, &u);
    double ms = (Now() - t0) * 1000.0;
    if (!ok) { FAIL("%s: %s", name, MS_LastError()); return; }
    if (!PaintUnwrapMeshes(in, meshCount, TEXELS, &again)) { FAIL("%s: second run: %s", name, MS_LastError()); MS_PaintUnwrapFree(&u); return; }
    bool same = SameUnwrap(&u, &again);
    if (!same) FAIL("%s: two runs differ", name);
    MS_PaintUnwrapFree(&again);

    // Order, count and validity
    int bad = 0, triangles = 0, vertices = 0, inVertices = 0;
    if (u.meshCount != meshCount) { FAIL("%s: %d meshes for %d", name, u.meshCount, meshCount); bad++; }
    for (int m = 0; m < meshCount && m < u.meshCount; m++) {
        const PaintUnwrapInput *im = &in[m];
        const MS_PaintUnwrapMesh *o = &u.meshes[m];
        triangles += Corners(im) / 3;
        vertices += o->vertexCount;
        inVertices += im->vertexCount;
        if (o->slot != im->slot) { FAIL("%s: mesh %d slot %d for %d", name, m, o->slot, im->slot); bad++; }
        if (o->indexCount != Corners(im)) { FAIL("%s: mesh %d: %d indices for %d", name, m, o->indexCount, Corners(im)); bad++; continue; }
        for (int v = 0; v < o->vertexCount; v++)
            if (o->source[v] >= (unsigned int)im->vertexCount) { FAIL("%s: mesh %d: vertex %d source %u", name, m, v, o->source[v]); bad++; break; }
        for (int i = 0; i < o->indexCount; i++) {
            if (o->indices[i] >= (unsigned int)o->vertexCount) { FAIL("%s: mesh %d: index %d out of range", name, m, i); bad++; break; }
            if (o->source[o->indices[i]] != InputIndex(im, i)) {
                FAIL("%s: mesh %d: corner %d (triangle %d) comes from vertex %u, the input's is %u", name, m, i, i / 3,
                     o->source[o->indices[i]], InputIndex(im, i));
                bad++;
                break;
            }
        }
    }

    // Per slot: overlap and gaps
    int *slots = malloc(sizeof(int) * (size_t)(u.meshCount + 1));
    int slotCount = 0, charts = 0, overlap = 0, coincident = 0, footprint = 0, minGap = MIN_GAP + 1;
    if (!slots) { FAIL("out of memory"); bad++; }
    for (int m = 0; m < u.meshCount && !bad; m++) {
        int s = u.meshes[m].slot, k = 0;
        while (k < slotCount && slots[k] != s) k++;
        if (k < slotCount) continue;
        slots[slotCount++] = s;
        SlotStats st = CheckSlot(&u, in, s);
        charts += st.charts;
        overlap += st.overlap;
        coincident += st.coincident;
        footprint += st.footprintOverlap;
        if (st.minGap < minGap) minGap = st.minGap;
        if (st.overlap) FAIL("%s: slot %d: %d texel centres of %d^2 covered twice", name, s, st.overlap, OVERLAP_RES);
        if (st.footprintOverlap) FAIL("%s: slot %d: chart footprints share %d texels at %d^2", name, s, st.footprintOverlap, TEXELS);
        if (st.minGap < MIN_GAP) FAIL("%s: slot %d: charts %d texels apart at %d^2", name, s, st.minGap, TEXELS);
    }
    free(slots);

    // The sidecar round trip (the runtime's writer and reader)
    const char *trip = "not run";
    if (scratchDir && !bad) {
        char path[1100];
        snprintf(path, sizeof(path), "%s/unwrap.paintuv", scratchDir);
        MS_PaintUnwrap back;
        if (!MS_PaintUnwrapWrite(path, &u)) trip = "skipped (MS_PaintUnwrapWrite: phase-0 stub)";
        else if (!MS_PaintUnwrapRead(path, &back)) { trip = "FAILED (read)"; FAIL("%s: MS_PaintUnwrapRead", name); }
        else {
            bool eq = SameUnwrap(&u, &back);
            trip = eq ? "identical" : "FAILED (differs)";
            if (!eq) FAIL("%s: the sidecar reads back different", name);
            MS_PaintUnwrapFree(&back);
        }
    }
    char gap[16];
    if (minGap > MIN_GAP) snprintf(gap, sizeof(gap), ">%d", MIN_GAP);
    else snprintf(gap, sizeof(gap), "%d", minGap);
    printf("%s %-28s meshes %3d  slots %2d  triangles %6d  vertices %6d -> %6d  charts %5d  overlap %d (coincident faces %d)  footprints %d  "
           "min gap %s  %s  %.1f ms  sidecar: %s\n",
           bad || overlap || footprint || minGap < MIN_GAP || !same ? "FAIL" : "ok  ", name, meshCount, slotCount,
           triangles, inVertices, vertices, charts, overlap, coincident, footprint, gap, same ? "deterministic" : "NOT deterministic", ms, trip);
    MS_PaintUnwrapFree(&u);
}

// #endregion

// ======================================================================================
// #region Inputs
// ======================================================================================

// A unit cube of 24 vertices whose six faces all use UVs 0..1 (every face on the same texels)
static void CubeCase(void)
{
    static const float N[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    float pos[24 * 3], uv[24 * 2];
    unsigned int idx[36];
    for (int f = 0; f < 6; f++) {
        const float *n = N[f];
        float a[3] = { n[1] != 0 || n[2] != 0 ? 1.0f : 0.0f, 0, 0 }, b[3];
        if (n[0] != 0) { a[0] = 0; a[1] = 0; a[2] = 1; }
        b[0] = n[1] * a[2] - n[2] * a[1]; b[1] = n[2] * a[0] - n[0] * a[2]; b[2] = n[0] * a[1] - n[1] * a[0];
        for (int k = 0; k < 4; k++) {
            float s = (k & 1) ? 0.5f : -0.5f, t = (k & 2) ? 0.5f : -0.5f;
            for (int i = 0; i < 3; i++) pos[(f * 4 + k) * 3 + i] = n[i] * 0.5f + a[i] * s + b[i] * t;
            uv[(f * 4 + k) * 2] = (k & 1) ? 1.0f : 0.0f;
            uv[(f * 4 + k) * 2 + 1] = (k & 2) ? 1.0f : 0.0f;
        }
        unsigned int q[6] = { 0, 1, 3, 0, 3, 2 };
        for (int i = 0; i < 6; i++) idx[f * 6 + i] = (unsigned int)(f * 4) + q[i];
    }
    PaintUnwrapInput in = { pos, uv, idx, 24, 36, 0 };
    Check("cube (faces share UVs)", &in, 1);
    in.texcoords = NULL;
    Check("cube (no UVs)", &in, 1);
}

// A cube of 8 shared vertices without UVs (xatlas must cut it to unfold it)
static void SharedCubeCase(void)
{
    float pos[8 * 3];
    for (int v = 0; v < 8; v++) { pos[v * 3] = (v & 1) ? 1.0f : 0.0f; pos[v * 3 + 1] = (v & 2) ? 1.0f : 0.0f; pos[v * 3 + 2] = (v & 4) ? 1.0f : 0.0f; }
    static const unsigned int idx[36] = {
        0, 2, 3, 0, 3, 1,  4, 5, 7, 4, 7, 6,  0, 1, 5, 0, 5, 4,  2, 6, 7, 2, 7, 3,  0, 4, 6, 0, 6, 2,  1, 3, 7, 1, 7, 5,
    };
    PaintUnwrapInput in = { pos, NULL, idx, 8, 36, 3 };
    Check("cube (8 shared vertices)", &in, 1);
}

// Two quads side by side, the second mirrored in UV: both cover UV 0..1 (Kenney's mirrored halves)
static void MirroredCase(void)
{
    static const float pos[8 * 3] = { 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0,  1, 0, 0, 2, 0, 0, 1, 1, 0, 2, 1, 0 };
    static const float uv[8 * 2] = { 0, 0, 1, 0, 0, 1, 1, 1,  1, 0, 0, 0, 1, 1, 0, 1 };
    static const unsigned int idx[12] = { 0, 1, 3, 0, 3, 2,  4, 5, 7, 4, 7, 6 };
    PaintUnwrapInput in = { pos, uv, idx, 8, 12, 0 };
    Check("mirrored quads", &in, 1);
}

// Several meshes and slots: slot 1 in meshes 0 and 2 (one domain), slot 0, an empty mesh (slot 2),
// a non-indexed mesh (slot 4) and a degenerate triangle among normal ones (slot 0)
static void MultiCase(void)
{
    static const float quad[4 * 3] = { 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1 };
    static const float quadUv[4 * 2] = { 0, 0, 3, 0, 0, 3, 3, 3 };        // tiled
    static const unsigned int quadIdx[6] = { 0, 2, 3, 0, 3, 1 };
    static const float tri[9 * 3] = { 0, 0, 0, 0, 1, 0, 1, 0, 0,  2, 0, 0, 2, 1, 0, 3, 0, 0,  5, 5, 5, 6, 6, 6, 7, 7, 7 };
    static const float strip[5 * 3] = { 0, 0, 0, 0, 2, 0, 1, 0, 0, 1, 2, 0, 0.5f, 3, 0 };
    static const unsigned int stripIdx[9] = { 0, 1, 2, 2, 1, 3, 1, 4, 3 };
    PaintUnwrapInput in[5] = {
        { quad, quadUv, quadIdx, 4, 6, 1 },
        { strip, NULL, stripIdx, 5, 9, 0 },
        { quad, quadUv, quadIdx, 4, 6, 1 },
        { NULL, NULL, NULL, 0, 0, 2 },
        { tri, NULL, NULL, 9, 0, 4 },       // the third triangle is a line (degenerate)
    };
    Check("meshes and slots", in, 5);
}

// Bad input is refused with a message
static void BadInputCases(void)
{
    static const float pos[3 * 3] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    static const unsigned int badIdx[3] = { 0, 1, 7 }, shortIdx[2] = { 0, 1 };
    PaintUnwrapInput cases[3] = { { pos, NULL, badIdx, 3, 3, 0 }, { pos, NULL, shortIdx, 3, 2, 0 }, { pos, NULL, badIdx, 3, 3, -1 } };
    for (int i = 0; i < 3; i++) {
        MS_ClearError();            // (so a refusal without a message would show)
        MS_PaintUnwrap u;
        if (PaintUnwrapMeshes(&cases[i], 1, TEXELS, &u)) { FAIL("bad input %d accepted", i); MS_PaintUnwrapFree(&u); }
        else if (!MS_LastError()[0] || u.meshCount || u.meshes) FAIL("bad input %d: no message or not empty", i);
        else printf("ok   bad input refused: %s\n", MS_LastError());
    }
}

// float16 -> float (R3D_Vertex.texcoord)
static float Half(uint16_t h)
{
    int e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? INFINITY : ldexpf((float)(m | 0x400), e - 25);
    return (h & 0x8000) ? -v : v;
}

// Inputs of a model as R3D loaded it; arrays[] holds what to free
typedef struct {
    PaintUnwrapInput *in;
    float **arrays;
    int count;
} ModelInput;

static bool ModelInputs(const R3D_Model *model, ModelInput *mi)
{
    mi->count = model->meshCount;
    mi->in = calloc((size_t)(model->meshCount + 1), sizeof(PaintUnwrapInput));
    mi->arrays = calloc((size_t)(2 * model->meshCount + 1), sizeof(float *));
    if (!mi->in || !mi->arrays || !model->meshData) return false;
    for (int m = 0; m < model->meshCount; m++) {
        const R3D_MeshData *md = &model->meshData[m];
        float *pos = mi->arrays[2 * m] = malloc(sizeof(float) * 3 * (size_t)(md->vertexCount + 1));
        float *uv = mi->arrays[2 * m + 1] = malloc(sizeof(float) * 2 * (size_t)(md->vertexCount + 1));
        if (!pos || !uv) return false;
        for (int v = 0; v < md->vertexCount; v++) {
            pos[3 * v] = md->vertices[v].position.x;
            pos[3 * v + 1] = md->vertices[v].position.y;
            pos[3 * v + 2] = md->vertices[v].position.z;
            uv[2 * v] = Half(md->vertices[v].texcoord[0]);
            uv[2 * v + 1] = Half(md->vertices[v].texcoord[1]);
        }
        mi->in[m] = (PaintUnwrapInput){ pos, uv, md->indexCount > 0 ? md->indices : NULL, md->vertexCount,
                                        md->indexCount, model->meshMaterials ? model->meshMaterials[m] : 0 };
    }
    return true;
}

static void FreeModelInputs(ModelInput *mi)
{
    if (mi->arrays) for (int i = 0; i < 2 * mi->count; i++) free(mi->arrays[i]);
    free(mi->arrays);
    free(mi->in);
}

// UV range of a model's own texture coordinates (why it needs an unwrap)
static void UvRange(const ModelInput *mi, float r[4])
{
    r[0] = r[1] = INFINITY; r[2] = r[3] = -INFINITY;
    for (int m = 0; m < mi->count; m++)
        for (int v = 0; v < mi->in[m].vertexCount; v++) {
            r[0] = fminf(r[0], mi->in[m].texcoords[2 * v]); r[1] = fminf(r[1], mi->in[m].texcoords[2 * v + 1]);
            r[2] = fmaxf(r[2], mi->in[m].texcoords[2 * v]); r[3] = fmaxf(r[3], mi->in[m].texcoords[2 * v + 1]);
        }
}

static void ModelCase(const char *path)
{
    R3D_Model model = R3D_LoadModelEx(path, R3D_IMPORT_RETAIN_MESH_DATA);
    if (model.meshCount <= 0) { FAIL("%s: R3D could not load it", path); return; }
    ModelInput mi = { 0 };
    if (!ModelInputs(&model, &mi)) FAIL("%s: out of memory or no mesh data", path);
    else {
        float r[4];
        UvRange(&mi, r);
        const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        printf("     %s: own UVs %.2f..%.2f x %.2f..%.2f\n", name, r[0], r[2], r[1], r[3]);
        Check(name, mi.in, mi.count);
    }
    FreeModelInputs(&mi);
    R3D_UnloadModel(model, true);
}

// PaintUnwrapModelFile on a loaded package must equal the direct unwrap of the same file
static void SceneCase(const char *package)
{
    MS_Scene *scene = MS_Load(package, NULL);
    if (!scene) { FAIL("--scene %s: %s", package, MS_LastError()); return; }
    for (int f = 0; f < scene->modelFileCount; f++) {
        const MS_ModelFile *mf = &scene->modelFiles[f];
        MS_PaintUnwrap viaScene;
        if (!PaintUnwrapModelFile(scene, f, TEXELS, &viaScene)) {
            R3D_MeshData probe;
            if (!MS_ModelMeshData(scene, f, 0, &probe)) printf("skip %s: PaintUnwrapModelFile (MS_ModelMeshData: phase-0 stub)\n", mf->path);
            else FAIL("PaintUnwrapModelFile %s: %s", mf->path, MS_LastError());
            continue;
        }
        char full[1100];
        snprintf(full, sizeof(full), "%s%s", scene->root, mf->path);
        R3D_Model model = R3D_LoadModelEx(full, R3D_IMPORT_RETAIN_MESH_DATA);
        ModelInput mi = { 0 };
        MS_PaintUnwrap direct;
        if (!ModelInputs(&model, &mi) || !PaintUnwrapMeshes(mi.in, mi.count, PAINTUNWRAP_TEXELS, &direct))
            FAIL("%s: direct unwrap", mf->path);
        else {
            direct.fileHash = viaScene.fileHash;
            if (!SameUnwrap(&direct, &viaScene)) FAIL("%s: PaintUnwrapModelFile differs from the direct unwrap", mf->path);
            else printf("ok   %s: PaintUnwrapModelFile = the direct unwrap (file hash %016llx)\n", mf->path, viaScene.fileHash);
            MS_PaintUnwrapFree(&direct);
        }
        FreeModelInputs(&mi);
        R3D_UnloadModel(model, true);
        MS_PaintUnwrapFree(&viaScene);
    }
    MS_Unload(scene);
}

// #endregion

int main(int argc, char **argv)
{
    const char *scenePath = NULL;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && first + 1 < argc; first += 2) {
        if (strcmp(argv[first], "--scene") == 0) scenePath = argv[first + 1];
        else if (strcmp(argv[first], "--scratch") == 0) scratchDir = argv[first + 1];
        else { fprintf(stderr, "unknown option %s\n", argv[first]); return 2; }
    }

    CubeCase();
    SharedCubeCase();
    MirroredCase();
    MultiCase();
    BadInputCases();

    // Models: R3D's loader needs a GL context (a hidden window, never shown)
    if (first < argc || scenePath) {
        SetTraceLogLevel(LOG_WARNING);
        SetConfigFlags(FLAG_WINDOW_HIDDEN);
        InitWindow(64, 64, "monolith-shot");
        R3D_Init(64, 64);
        MS_InitRenderer();
        for (int i = first; i < argc; i++) ModelCase(argv[i]);
        if (scenePath) SceneCase(scenePath);
        R3D_Close();
        CloseWindow();
    }
    printf("%s: %d failure%s\n", failures ? "FAIL" : "OK", failures, failures == 1 ? "" : "s");
    MS_ClearError();
    return failures ? 1 : 0;
}
