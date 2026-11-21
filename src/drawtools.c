// drawtools -- the draw mode's rasterisers and selection masks (drawtools.h; docs/painting.md 18.4, 18.5).
// Pure C on integer texel grids: no raylib, no scene, no globals but constants; every buffer is heap.
// The shapes are aliased as Aseprite draws them (Bresenham lines, Zingl's ellipse in a rectangle);
// the random tools use a 32-bit integer hash only, so a seed gives the same texels on every platform.
#include "drawtools.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int MinI(int a, int b) { return a < b ? a : b; }
static int MaxI(int a, int b) { return a > b ? a : b; }
static int AbsI(int a) { return a < 0 ? -a : a; }

// ======================================================================================
// #region Point lists and masks
// ======================================================================================

void DtPointsClear(DtPoints *l) { l->n = 0; l->oom = false; }

void DtPointsFree(DtPoints *l)
{
    free(l->p);
    *l = (DtPoints){ 0 };
}

bool DtPointsPush(DtPoints *l, int x, int y)
{
    if (l->n >= l->cap) {
        if (l->cap > (1 << 28)) { l->oom = true; return false; }
        int cap = l->cap ? l->cap * 2 : 64;
        DtPoint *p = realloc(l->p, (size_t)cap * sizeof(*p));
        if (!p) { l->oom = true; return false; }
        l->p = p;
        l->cap = cap;
    }
    l->p[l->n++] = (DtPoint){ x, y };
    return true;
}

bool DtMaskInit(DtMask *m, int x, int y, int w, int h)
{
    *m = (DtMask){ x, y, w > 0 ? w : 0, h > 0 ? h : 0, NULL };
    if (m->w == 0 || m->h == 0) { m->w = m->h = 0; return true; }
    m->a = calloc((size_t)m->w * (size_t)m->h, 1);
    if (!m->a) { *m = (DtMask){ x, y, 0, 0, NULL }; return false; }
    return true;
}

void DtMaskFree(DtMask *m)
{
    free(m->a);
    *m = (DtMask){ 0 };
}

bool DtMaskCopy(DtMask *dst, const DtMask *src)
{
    if (dst == src) return true;
    DtMask n;
    if (!DtMaskInit(&n, src->x, src->y, src->w, src->h)) return false;
    if (n.a) memcpy(n.a, src->a, (size_t)n.w * n.h);
    DtMaskFree(dst);
    *dst = n;
    return true;
}

static bool MaskHas(const DtMask *m, int x, int y)
{
    return m->a && x >= m->x && y >= m->y && x < m->x + m->w && y < m->y + m->h;
}

unsigned char DtMaskGet(const DtMask *m, int x, int y)
{
    return MaskHas(m, x, y) ? m->a[(size_t)(y - m->y) * m->w + (x - m->x)] : 0;
}

void DtMaskSet(DtMask *m, int x, int y, unsigned char v)
{
    if (MaskHas(m, x, y)) m->a[(size_t)(y - m->y) * m->w + (x - m->x)] = v;
}

bool DtMaskEmpty(const DtMask *m)
{
    size_t n = (size_t)m->w * m->h;
    for (size_t i = 0; m->a && i < n; i++)
        if (m->a[i]) return false;
    return true;
}

bool DtMaskBounds(const DtMask *m, int *x0, int *y0, int *x1, int *y1)
{
    int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
    bool any = false;
    for (int y = 0; m->a && y < m->h; y++)
        for (int x = 0; x < m->w; x++) {
            if (!m->a[(size_t)y * m->w + x]) continue;
            if (!any) { bx0 = bx1 = x; by0 = by1 = y; any = true; continue; }
            bx0 = MinI(bx0, x); bx1 = MaxI(bx1, x);
            by0 = MinI(by0, y); by1 = MaxI(by1, y);
        }
    if (!any) return false;
    *x0 = m->x + bx0; *y0 = m->y + by0;
    *x1 = m->x + bx1; *y1 = m->y + by1;
    return true;
}

static uint64_t Fnv(uint64_t h, const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

uint64_t DtMaskHash(const DtMask *m)
{
    // (x y w h as 32-bit little-endian, so the hash is the same on every platform)
    int32_t v[4] = { m->x, m->y, m->w, m->h };
    unsigned char b[16];
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) b[i * 4 + k] = (unsigned char)(((uint32_t)v[i] >> (8 * k)) & 255u);
    uint64_t h = Fnv(14695981039346656037ull, b, sizeof(b));
    return m->a ? Fnv(h, m->a, (size_t)m->w * m->h) : h;
}

// #endregion

// ======================================================================================
// #region Outlines
// ======================================================================================

void DtLine(DtPoints *out, DtPoint a, DtPoint b)
{
    int dx = AbsI(b.x - a.x), sx = a.x < b.x ? 1 : -1;
    int dy = -AbsI(b.y - a.y), sy = a.y < b.y ? 1 : -1;
    int err = dx + dy, x = a.x, y = a.y;
    for (;;) {
        if (!DtPointsPush(out, x, y)) return;
        if (x == b.x && y == b.y) return;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
}

// Aseprite's IntertwineAsPixelPerfect: consecutive repeats go first; then a texel whose neighbours
// before and after share a row or column with it but not with each other is the middle of an L and
// is dropped, and the texel after a dropped one is always kept (so a staircase keeps every step)
void DtPixelPerfect(DtPoints *path)
{
    int n = 0;
    for (int i = 0; i < path->n; i++)
        if (n == 0 || path->p[i].x != path->p[n - 1].x || path->p[i].y != path->p[n - 1].y) path->p[n++] = path->p[i];
    path->n = n;
    if (n < 3) return;

    // (reads use the original neighbours: `prev` holds p[c - 1] before the write index can reach it)
    int w = 0;
    DtPoint prev = path->p[0];
    for (int c = 0; c < n; c++) {
        DtPoint cur = path->p[c];
        if (c > 0 && c + 1 < n) {
            DtPoint next = path->p[c + 1];
            if ((prev.x == cur.x || prev.y == cur.y) && (next.x == cur.x || next.y == cur.y) &&
                prev.x != next.x && prev.y != next.y) {
                c++;                    // drop cur, keep the one after it unchecked
                prev = path->p[c];
                path->p[w++] = prev;
                continue;
            }
        }
        prev = cur;
        path->p[w++] = cur;
    }
    path->n = w;
}

// Drops every repeat of a texel from out->p[from ..] (first occurrence kept), with a bitmap over the
// points' bounds
static void DedupeFrom(DtPoints *out, int from)
{
    if (out->n - from < 2) return;
    int x0 = out->p[from].x, x1 = x0, y0 = out->p[from].y, y1 = y0;
    for (int i = from; i < out->n; i++) {
        x0 = MinI(x0, out->p[i].x); x1 = MaxI(x1, out->p[i].x);
        y0 = MinI(y0, out->p[i].y); y1 = MaxI(y1, out->p[i].y);
    }
    size_t w = (size_t)x1 - x0 + 1, h = (size_t)y1 - y0 + 1;
    unsigned char *seen = calloc(w * h, 1);
    if (!seen) { out->oom = true; return; }
    int k = from;
    for (int i = from; i < out->n; i++) {
        size_t at = (size_t)(out->p[i].y - y0) * w + (size_t)(out->p[i].x - x0);
        if (seen[at]) continue;
        seen[at] = 1;
        out->p[k++] = out->p[i];
    }
    out->n = k;
    free(seen);
}

// The Bezier is sampled at twice its control polygon's length (no two samples a texel apart), each
// sample rounded to its texel and joined to the last by a Bresenham line; repeats are dropped
void DtCurve(DtPoints *out, DtPoint a, DtPoint c1, DtPoint c2, DtPoint b)
{
    int from = out->n;
    double len = hypot(c1.x - a.x, c1.y - a.y) + hypot(c2.x - c1.x, c2.y - c1.y) + hypot(b.x - c2.x, b.y - c2.y);
    int steps = (int)ceil(len * 2.0);
    steps = steps < 1 ? 1 : steps > (1 << 20) ? (1 << 20) : steps;
    DtPoint last = a;
    if (!DtPointsPush(out, a.x, a.y)) return;
    for (int i = 1; i <= steps; i++) {
        double t = (double)i / steps, u = 1.0 - t;
        double w0 = u * u * u, w1 = 3.0 * u * u * t, w2 = 3.0 * u * t * t, w3 = t * t * t;
        DtPoint p = (i == steps) ? b : (DtPoint){
            (int)floor(w0 * a.x + w1 * c1.x + w2 * c2.x + w3 * b.x + 0.5),
            (int)floor(w0 * a.y + w1 * c1.y + w2 * c2.y + w3 * b.y + 0.5) };
        if (p.x == last.x && p.y == last.y) continue;
        int before = out->n;
        DtLine(out, last, p);
        if (out->oom) return;
        // (the line's first texel is `last`, already in the list)
        memmove(&out->p[before], &out->p[before + 1], (size_t)(out->n - before - 1) * sizeof(DtPoint));
        out->n--;
        last = p;
    }
    DedupeFrom(out, from);
}

void DtRectOutline(DtPoints *out, DtPoint a, DtPoint b)
{
    int x0 = MinI(a.x, b.x), x1 = MaxI(a.x, b.x), y0 = MinI(a.y, b.y), y1 = MaxI(a.y, b.y);
    if (x0 == x1 || y0 == y1) {     // one row or column: a line, each texel once
        DtLine(out, (DtPoint){ x0, y0 }, (DtPoint){ x1, y1 });
        return;
    }
    // clockwise from the top-left corner, each corner once
    for (int x = x0; x <= x1; x++) if (!DtPointsPush(out, x, y0)) return;
    for (int y = y0 + 1; y <= y1; y++) if (!DtPointsPush(out, x1, y)) return;
    for (int x = x1 - 1; x >= x0; x--) if (!DtPointsPush(out, x, y1)) return;
    for (int y = y1 - 1; y > y0; y--) if (!DtPointsPush(out, x0, y)) return;
}

static bool PushRange(DtPoints *out, const DtPoints *src, bool reversed)
{
    for (int i = 0; i < src->n; i++) {
        DtPoint p = src->p[reversed ? src->n - 1 - i : i];
        if (!DtPointsPush(out, p.x, p.y)) return false;
    }
    return true;
}

// Alois Zingl's plotEllipseRect (as Aseprite's algo_ellipse): the ellipse inscribed in the inclusive
// rectangle, for every width and height (even, 1 and 2 wide, a circle). Its four quadrants come out
// together; they are collected apart and joined clockwise from the top, the tips of a flat ellipse
// (the "too early stop" loop) at the top and bottom, repeats dropped. Where the shape has spurs (the
// 1-texel tips of a flat ellipse) no path visits each texel once, and the list jumps there
void DtEllipseOutline(DtPoints *out, DtPoint pa, DtPoint pb)
{
    if (pa.x == pb.x || pa.y == pb.y) {     // one row or column: Zingl's texels, in line order
        DtLine(out, pa, pb);
        return;
    }
    // (64-bit: the error terms grow as the size cubed, past a 32-bit long on Windows)
    int64_t x0 = pa.x, y0 = pa.y, x1 = pb.x, y1 = pb.y;
    int64_t a = x1 > x0 ? x1 - x0 : x0 - x1, b = y1 > y0 ? y1 - y0 : y0 - y1, b1 = b & 1;
    int64_t dx = 4 * (1 - a) * b * b, dy = 4 * (b1 + 1) * a * a;
    int64_t err = dx + dy + b1 * a * a, e2;
    if (x0 > x1) { x0 = x1; x1 += a; }
    if (y0 > y1) y0 = y1;
    y0 += (b + 1) / 2;
    y1 = y0 - b1;
    a *= 8 * a;
    b1 = 8 * b * b;

    // q1: right, down from the middle; q2: left, down; q3: left, up; q4: right, up. Tips: top / bottom,
    // left / right column of the centre, outward from the middle
    DtPoints q[4] = { 0 }, tip[4] = { 0 };     // tip: 0 top right, 1 top left, 2 bottom right, 3 bottom left
    bool ok = true;
    do {
        ok = ok && DtPointsPush(&q[0], (int)x1, (int)y0) && DtPointsPush(&q[1], (int)x0, (int)y0) &&
             DtPointsPush(&q[2], (int)x0, (int)y1) && DtPointsPush(&q[3], (int)x1, (int)y1);
        e2 = 2 * err;
        if (e2 <= dy) { y0++; y1--; err += dy += a; }
        if (e2 >= dx || 2 * err > dy) { x0++; x1--; err += dx += b1; }
    } while (x0 <= x1 && ok);
    while (ok && y0 - y1 <= b) {
        ok = DtPointsPush(&tip[3], (int)(x0 - 1), (int)y0) && DtPointsPush(&tip[2], (int)(x1 + 1), (int)y0) &&
             DtPointsPush(&tip[1], (int)(x0 - 1), (int)y1) && DtPointsPush(&tip[0], (int)(x1 + 1), (int)y1);
        y0++;
        y1--;
    }

    int from = out->n;
    if (ok) {
        ok = PushRange(out, &tip[0], true) && PushRange(out, &q[3], true) && PushRange(out, &q[0], false) &&
             PushRange(out, &tip[2], false) && PushRange(out, &tip[3], true) && PushRange(out, &q[1], true) &&
             PushRange(out, &q[2], false) && PushRange(out, &tip[1], false);
    } else out->oom = true;
    for (int i = 0; i < 4; i++) { DtPointsFree(&q[i]); DtPointsFree(&tip[i]); }
    if (ok) DedupeFrom(out, from);
}

// #endregion

// ======================================================================================
// #region Filled shapes
// ======================================================================================

static void FillSpan(DtMask *m, int y, int x0, int x1)
{
    if (!m->a || y < m->y || y >= m->y + m->h) return;
    x0 = MaxI(x0, m->x);
    x1 = MinI(x1, m->x + m->w - 1);
    if (x0 > x1) return;
    memset(&m->a[(size_t)(y - m->y) * m->w + (x0 - m->x)], 255, (size_t)(x1 - x0 + 1));
}

void DtRectFill(DtMask *out, DtPoint a, DtPoint b)
{
    int y0 = MaxI(MinI(a.y, b.y), out->y), y1 = MinI(MaxI(a.y, b.y), out->y + out->h - 1);
    for (int y = y0; y <= y1; y++) FillSpan(out, y, MinI(a.x, b.x), MaxI(a.x, b.x));
}

// Each row from the leftmost to the rightmost outline texel on it (the ellipse is convex)
void DtEllipseFill(DtMask *out, DtPoint a, DtPoint b)
{
    DtPoints o = { 0 };
    DtEllipseOutline(&o, a, b);
    if (o.oom || o.n == 0) { DtPointsFree(&o); return; }
    int y0 = MinI(a.y, b.y), h = AbsI(b.y - a.y) + 1;
    int *lo = malloc((size_t)h * sizeof(int)), *hi = malloc((size_t)h * sizeof(int));
    if (lo && hi) {
        for (int i = 0; i < h; i++) { lo[i] = INT32_MAX; hi[i] = INT32_MIN; }
        for (int i = 0; i < o.n; i++) {
            int r = o.p[i].y - y0;
            if (r < 0 || r >= h) continue;
            lo[r] = MinI(lo[r], o.p[i].x);
            hi[r] = MaxI(hi[r], o.p[i].x);
        }
        for (int i = 0; i < h; i++)
            if (lo[i] <= hi[i]) FillSpan(out, y0 + i, lo[i], hi[i]);
    }
    free(lo);
    free(hi);
    DtPointsFree(&o);
}

static int CmpDouble(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

// Scanlines at texel centres: with every vertex at its texel's centre, a centre is inside when an odd
// number of edges cross its row to its right (half-open in y, so a vertex on the row counts once);
// then every edge's Bresenham line, so the outline is always painted
void DtPolygonFill(DtMask *out, const DtPoint *pts, int n)
{
    if (n <= 0 || !out->a) return;
    int y0 = pts[0].y, y1 = pts[0].y;
    for (int i = 1; i < n; i++) { y0 = MinI(y0, pts[i].y); y1 = MaxI(y1, pts[i].y); }
    y0 = MaxI(y0, out->y);
    y1 = MinI(y1, out->y + out->h - 1);
    double *xs = n >= 3 ? malloc((size_t)n * sizeof(double)) : NULL;
    for (int y = y0; xs && y <= y1; y++) {
        int k = 0;
        for (int i = 0; i < n; i++) {
            DtPoint p = pts[i], q = pts[(i + 1) % n];
            if ((p.y > y) == (q.y > y)) continue;
            xs[k++] = p.x + (double)(y - p.y) * (q.x - p.x) / (double)(q.y - p.y);
        }
        qsort(xs, (size_t)k, sizeof(double), CmpDouble);
        for (int i = 0; i + 1 < k; i += 2) {
            int xa = (int)ceil(xs[i]), xb = (int)floor(xs[i + 1]);
            if (xa <= xb) FillSpan(out, y, xa, xb);
        }
    }
    free(xs);

    DtPoints e = { 0 };
    for (int i = 0; i < n; i++) {
        DtPointsClear(&e);
        DtLine(&e, pts[i], pts[(i + 1) % n]);
        for (int k = 0; k < e.n; k++) DtMaskSet(out, e.p[k].x, e.p[k].y, 255);
    }
    DtPointsFree(&e);
}

// #endregion

// ======================================================================================
// #region Pixels
// ======================================================================================

static const unsigned char *Px(const DtImage *img, int x, int y)
{
    if (!img->rgba || x < 0 || y < 0 || x >= img->w || y >= img->h) return NULL;
    return img->rgba + (size_t)y * img->stride + (size_t)x * 4;
}

bool DtColorNear(const unsigned char a[4], const unsigned char b[4], int tolerance)
{
    if (a[3] == 0 && b[3] == 0) return true;        // two transparent texels match whatever their RGB
    for (int i = 0; i < 4; i++)
        if (AbsI(a[i] - b[i]) > tolerance) return false;
    return true;
}

bool DtFlood(DtMask *out, const DtImage *img, const DtMask *region, DtPoint seed, int tolerance, bool contiguous)
{
    if (out->x != region->x || out->y != region->y || out->w != region->w || out->h != region->h) return false;
    if (out->a) memset(out->a, 0, (size_t)out->w * out->h);
    const unsigned char *sp = Px(img, seed.x, seed.y);
    if (!sp || !DtMaskGet(region, seed.x, seed.y)) return false;
    unsigned char ref[4];
    memcpy(ref, sp, 4);

    if (!contiguous) {
        for (int y = region->y; y < region->y + region->h; y++)
            for (int x = region->x; x < region->x + region->w; x++) {
                const unsigned char *p = DtMaskGet(region, x, y) ? Px(img, x, y) : NULL;
                if (p && DtColorNear(p, ref, tolerance)) DtMaskSet(out, x, y, 255);
            }
        return true;
    }

    // 4-connected with an explicit stack; out doubles as the visited set (a texel is set when pushed)
    DtPoints stack = { 0 };
    DtMaskSet(out, seed.x, seed.y, 255);
    bool ok = DtPointsPush(&stack, seed.x, seed.y);
    static const int D[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    while (ok && stack.n > 0) {
        DtPoint c = stack.p[--stack.n];
        for (int d = 0; d < 4 && ok; d++) {
            int x = c.x + D[d][0], y = c.y + D[d][1];
            if (!DtMaskGet(region, x, y) || DtMaskGet(out, x, y)) continue;
            const unsigned char *p = Px(img, x, y);
            if (!p || !DtColorNear(p, ref, tolerance)) continue;
            DtMaskSet(out, x, y, 255);
            ok = DtPointsPush(&stack, x, y);
        }
    }
    DtPointsFree(&stack);
    return ok;
}

float DtGradientT(int x, int y, DtPoint a, DtPoint b, bool radial)
{
    // (texel centres on both sides: the +0.5 cancels in every difference)
    double px = x - a.x, py = y - a.y, dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    double t;
    if (radial) t = l2 > 0.0 ? sqrt((px * px + py * py) / l2) : (px == 0.0 && py == 0.0 ? 0.0 : 1.0);
    else t = l2 > 0.0 ? (px * dx + py * dy) / l2 : 0.0;
    return (float)(t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t);
}

static bool InBoth(const DtMask *where, const DtMask *region, int x, int y)
{
    return DtMaskGet(where, x, y) && DtMaskGet(region, x, y);
}

// Integer rounding of n / d for n >= 0, d > 0
static unsigned char DivRound(uint64_t n, uint64_t d) { return (unsigned char)((n + d / 2) / d); }

void DtBlur(unsigned char *out, const DtImage *img, const DtMask *region, const DtMask *where)
{
    for (int y = where->y; out && y < where->y + where->h; y++)
        for (int x = where->x; x < where->x + where->w; x++) {
            const unsigned char *self = Px(img, x, y);
            if (!self || !InBoth(where, region, x, y)) continue;
            uint64_t c[3] = { 0, 0, 0 }, sa = 0, n = 0;
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++) {
                    const unsigned char *p = DtMaskGet(region, x + i, y + j) ? Px(img, x + i, y + j) : NULL;
                    if (!p) continue;
                    for (int k = 0; k < 3; k++) c[k] += (uint64_t)p[k] * p[3];
                    sa += p[3];
                    n++;
                }
            unsigned char *o = out + ((size_t)(y - where->y) * where->w + (x - where->x)) * 4;
            // (all transparent: the colour stays the texel's own, the alpha 0)
            for (int k = 0; k < 3; k++) o[k] = sa ? DivRound(c[k], sa) : self[k];
            o[3] = DivRound(sa, n);
        }
}

// A 32-bit integer hash (Chris Wellons' lowbias32): the only source of randomness, so a seed gives
// the same numbers on every platform and compiler
static uint32_t Hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint32_t DtTexelHash(uint32_t seed, int x, int y)
{
    return Hash32(seed ^ Hash32((uint32_t)x * 0x9e3779b1u ^ Hash32((uint32_t)y + 0x632be5abu)));
}

void DtJumble(unsigned char *out, const DtImage *img, const DtMask *region, const DtMask *where, uint32_t seed)
{
    for (int y = where->y; out && y < where->y + where->h; y++)
        for (int x = where->x; x < where->x + where->w; x++) {
            const unsigned char *self = Px(img, x, y);
            if (!self || !InBoth(where, region, x, y)) continue;
            const unsigned char *cand[8];      // (stack scratch for one texel's neighbours, not content)
            int n = 0;
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++) {
                    if (i == 0 && j == 0) continue;
                    const unsigned char *p = DtMaskGet(region, x + i, y + j) ? Px(img, x + i, y + j) : NULL;
                    if (p) cand[n++] = p;
                }
            // (keyed on the texel, not on the visiting order: the same seed picks the same neighbour
            // whatever the mask's shape)
            uint32_t h = DtTexelHash(seed, x, y);
            unsigned char *o = out + ((size_t)(y - where->y) * where->w + (x - where->x)) * 4;
            memcpy(o, n ? cand[h % (uint32_t)n] : self, 4);
        }
}

// Uniform in the disc by rejection from its square (no sin / cos: libm may round differently between
// platforms); 24-bit uniforms are exact in double
void DtSpray(DtPoints *out, float cx, float cy, float radius, int count, uint32_t seed)
{
    uint32_t state = Hash32(seed ^ 0xa511e9b3u);
    for (int i = 0; i < count; i++) {
        double u = 0.0, v = 0.0;
        for (int tries = 0; tries < 64; tries++) {
            state = Hash32(state + 0x9e3779b9u);
            u = (double)(state >> 8) / 8388608.0 - 1.0;       // [-1, 1)
            state = Hash32(state + 0x9e3779b9u);
            v = (double)(state >> 8) / 8388608.0 - 1.0;
            if (u * u + v * v <= 1.0) break;
            u = v = 0.0;
        }
        if (!DtPointsPush(out, (int)floor(cx + u * radius), (int)floor(cy + v * radius))) return;
    }
}

// #endregion

// ======================================================================================
// #region Selection operations
// ======================================================================================

void DtMaskCombine(DtMask *dst, const DtMask *src, DtSelOp op)
{
    for (int y = dst->y; dst->a && y < dst->y + dst->h; y++)
        for (int x = dst->x; x < dst->x + dst->w; x++) {
            unsigned char *d = &dst->a[(size_t)(y - dst->y) * dst->w + (x - dst->x)];
            int s = DtMaskGet(src, x, y);
            switch (op) {
            case DT_SEL_REPLACE: *d = (unsigned char)s; break;
            case DT_SEL_ADD:     *d = (unsigned char)MaxI(*d, s); break;
            case DT_SEL_SUB:     *d = (unsigned char)MinI(*d, 255 - s); break;
            case DT_SEL_AND:     *d = (unsigned char)MinI(*d, s); break;
            }
        }
}

void DtMaskInvert(DtMask *m, const DtMask *region)
{
    for (int y = m->y; m->a && y < m->y + m->h; y++)
        for (int x = m->x; x < m->x + m->w; x++) {
            unsigned char *d = &m->a[(size_t)(y - m->y) * m->w + (x - m->x)];
            *d = DtMaskGet(region, x, y) ? (unsigned char)(255 - *d) : 0;
        }
}

void DtMaskClip(DtMask *m, const DtMask *region)
{
    for (int y = m->y; m->a && y < m->y + m->h; y++)
        for (int x = m->x; x < m->x + m->w; x++) {
            unsigned char *d = &m->a[(size_t)(y - m->y) * m->w + (x - m->x)];
            *d = (unsigned char)MinI(*d, DtMaskGet(region, x, y));
        }
}

void DtMaskShift(DtMask *dst, const DtMask *src, int dx, int dy)
{
    if (!dst->a) return;
    memset(dst->a, 0, (size_t)dst->w * dst->h);
    if (!src->a) return;
    // (64-bit: a shift far past the mask must not overflow)
    for (int y = 0; y < dst->h; y++)
        for (int x = 0; x < dst->w; x++) {
            int64_t sx = (int64_t)dst->x + x - dx - src->x, sy = (int64_t)dst->y + y - dy - src->y;
            if (sx >= 0 && sy >= 0 && sx < src->w && sy < src->h)
                dst->a[(size_t)y * dst->w + x] = src->a[(size_t)sy * src->w + (size_t)sx];
        }
}

// #endregion
