// The draw mode's rasterisers and selection masks (drawtools.c; docs/painting.md 18.4, 18.5): pure
// code on texel grids, no window, no scene, no raylib calls, unit-tested in tests/unit/drawtools_test.c.
// drawmode.c turns their output into stamps of one stroke (the compositing rules of 7.1).
//
// Coordinates are integer atlas texels (x right, y down as the atlas image's rows). A mask covers a
// rectangle [x, x + w) x [y, y + h) of the atlas with one byte per texel (0: out, 255: in; shapes
// are aliased, as Aseprite's). Every buffer is heap (MS_RESERVE / MS_PUSH style growth): a mask
// sized by its region, point lists by the shape.
#ifndef MONOLITH_DRAWTOOLS_H
#define MONOLITH_DRAWTOOLS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct { int x, y; } DtPoint;

// A growing list of texels (heap); DtPointsFree releases it
typedef struct {
    DtPoint *p;
    int n, cap;
    bool oom;               // a push failed: the list is short (the caller says so, draws nothing)
} DtPoints;

void DtPointsClear(DtPoints *l);            // n = 0, keeps the memory
void DtPointsFree(DtPoints *l);
bool DtPointsPush(DtPoints *l, int x, int y);

// One byte per texel of [x, x + w) x [y, y + h)
typedef struct {
    int x, y, w, h;
    unsigned char *a;       // w * h (heap), NULL when w or h is 0
} DtMask;

bool DtMaskInit(DtMask *m, int x, int y, int w, int h);     // all 0; false: out of memory (m empty)
void DtMaskFree(DtMask *m);
bool DtMaskCopy(DtMask *dst, const DtMask *src);            // dst: a mask or {0}, freed first
unsigned char DtMaskGet(const DtMask *m, int x, int y);     // 0 outside it
void DtMaskSet(DtMask *m, int x, int y, unsigned char v);   // ignored outside it
bool DtMaskEmpty(const DtMask *m);                          // no texel > 0
// The bounds of its texels > 0, inclusive (false: none)
bool DtMaskBounds(const DtMask *m, int *x0, int *y0, int *x1, int *y1);
uint64_t DtMaskHash(const DtMask *m);                       // FNV-1a 64 of x y w h and the bytes (--paint-script print)

// ======================================================================================
// Outlines: the texels a shape's line passes through, in order, each once (drawmode stamps the
// brush at each; a size-1 pencil sets exactly these texels). Each appends to `out` (DtPointsClear first
// for a new list); a failed push sets out->oom and ends it
// ======================================================================================

void DtLine(DtPoints *out, DtPoint a, DtPoint b);           // Bresenham, both ends included
// Aseprite's pixel-perfect: drops the middle texel of every L-shaped corner (a texel whose
// neighbours before and after touch each other diagonally and it them orthogonally) from a freehand
// path, so a 1-texel stroke has no doubled corners. In place
void DtPixelPerfect(DtPoints *path);
// A cubic Bezier a -> b with control points c1, c2 (Aseprite's curve), as connected texels (each step
// to an 8-neighbour; its L corners stay, DtPixelPerfect thins them), each once
void DtCurve(DtPoints *out, DtPoint a, DtPoint c1, DtPoint c2, DtPoint b);
void DtRectOutline(DtPoints *out, DtPoint a, DtPoint b);    // a, b: opposite corners, inclusive
// Inscribed in the rectangle a .. b (inclusive; Zingl's midpoint ellipse, as Aseprite's), clockwise from
// the top; a flat ellipse's 1-texel tips are spurs, where the order jumps
void DtEllipseOutline(DtPoints *out, DtPoint a, DtPoint b);

// ======================================================================================
// Filled shapes: set 255 in `out` (which already covers the region; texels outside it are skipped)
// ======================================================================================

void DtRectFill(DtMask *out, DtPoint a, DtPoint b);
void DtEllipseFill(DtMask *out, DtPoint a, DtPoint b);      // the outline of DtEllipseOutline and its inside
// A closed polygon (contour, polygon, lasso, polygonal lasso): even-odd at texel centres, plus every
// texel its edges pass through (so a thin contour still paints its outline)
void DtPolygonFill(DtMask *out, const DtPoint *pts, int n);

// ======================================================================================
// Pixels: RGBA8, straight alpha, `stride` bytes per row, (0, 0) the image's first texel
// ======================================================================================

typedef struct {
    const unsigned char *rgba;
    int w, h, stride;
} DtImage;

// Tolerance: Aseprite's, 0..255 on the largest channel difference (RGBA) from the seed's colour; two
// texels of alpha 0 are near whatever their RGB (Aseprite's)
bool DtColorNear(const unsigned char a[4], const unsigned char b[4], int tolerance);
// Bucket / magic wand: the texels of `region` (> 0) whose colour is near the seed's: contiguous (4-
// connected from the seed, Aseprite's) or every such texel of the region (global). out must cover the
// region's rectangle exactly; it is cleared, then the texels set 255. False: out's rectangle is not the
// region's, the seed is outside the region or the image, or out of memory (out then partial)
bool DtFlood(DtMask *out, const DtImage *img, const DtMask *region, DtPoint seed, int tolerance, bool contiguous);

// Gradient: t in 0..1 of texel centre (x + 0.5, y + 0.5) between a (t 0) and b (t 1); linear: the
// projection on a -> b, radial: the distance from a over |b - a|. Clamped
float DtGradientT(int x, int y, DtPoint a, DtPoint b, bool radial);

// Blur: out[x, y] for every texel of `where` (> 0) inside `region` (> 0): the alpha-weighted mean of
// the 3 x 3 texels around it that lie in the region (colour weighted by alpha, alpha the plain mean),
// read from img (all of them transparent: the texel's own colour, alpha 0). out is RGBA8 of where's
// size (w * h * 4, heap, the caller's); its other texels are left as they are
void DtBlur(unsigned char *out, const DtImage *img, const DtMask *region, const DtMask *where);
// Jumble: each texel of `where` inside `region` takes the colour of a random neighbour (8-connected)
// that lies in the region, from img (none: its own); `seed` makes it repeatable (a replay gives the
// same pixels; the pick is a hash of seed and texel, so it doesn't depend on where's shape). out as DtBlur's
void DtJumble(unsigned char *out, const DtImage *img, const DtMask *region, const DtMask *where, uint32_t seed);
// The random number of texel (x, y) for `seed` (32-bit integer hash; jumble's pick is it modulo the
// neighbours found, in row order without the texel itself): the same on every platform
uint32_t DtTexelHash(uint32_t seed, int x, int y);
// Spray: `count` random texels (repeats possible) in the disc of `radius` around c (uniform over the
// disc; the texel holding the point), from `seed`; appended to out. Integer hash only: the same texels
// on every platform
void DtSpray(DtPoints *out, float cx, float cy, float radius, int count, uint32_t seed);

// ======================================================================================
// Selection operations (docs/painting.md 18.5): dst and src cover the same rectangle (the region's)
// ======================================================================================

typedef enum {
    DT_SEL_REPLACE = 0,     // dst = src
    DT_SEL_ADD,             // SHIFT: union
    DT_SEL_SUB,             // ALT: dst minus src
    DT_SEL_AND,             // SHIFT+ALT: intersection
} DtSelOp;

void DtMaskCombine(DtMask *dst, const DtMask *src, DtSelOp op);
// Every texel of region (> 0) flips; texels outside the region stay 0
void DtMaskInvert(DtMask *m, const DtMask *region);
// dst &= region (a selection never holds texels outside its region)
void DtMaskClip(DtMask *m, const DtMask *region);
// Shifted copy (the move tool): dst (cleared first; not src itself) gets src moved by dx, dy, clipped to
// dst's rectangle
void DtMaskShift(DtMask *dst, const DtMask *src, int dx, int dy);

#endif
