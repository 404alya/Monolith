// Unit check of the draw mode's rasterisers and selection masks (src/drawtools.c): exact texels of
// every shape on small grids (written as ASCII art: '#' in, '.' out), the ellipse's symmetry, pixel-
// perfect on a staircase, flood fill (tolerance, contiguous / global, a region with a hole), the
// gradient, blur and jumble, spray's determinism, every selection operation, degenerate inputs.
// Runs under ASan + UBSan (leaks too) through ./build.sh test drawtools.
#include "drawtools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void Expect(const char *what, bool ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

#define ROWS(...) ((const char *[]){ __VA_ARGS__, NULL })

static int RowCount(const char **rows)
{
    int n = 0;
    while (rows[n]) n++;
    return n;
}

// The art's texels at (x0, y0): every '#' once, nothing else (a point outside the art fails)
static bool PointsMatch(const char *what, const DtPoints *p, int x0, int y0, const char **rows)
{
    int h = RowCount(rows), w = (int)strlen(rows[0]);
    int *count = calloc((size_t)w * h, sizeof(int));
    bool ok = !p->oom;
    for (int i = 0; i < p->n; i++) {
        int x = p->p[i].x - x0, y = p->p[i].y - y0;
        if (x < 0 || y < 0 || x >= w || y >= h) { ok = false; continue; }
        count[y * w + x]++;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) ok = ok && count[y * w + x] == (rows[y][x] == '#' ? 1 : 0);
    if (!ok) {
        printf("     %s: got (2+: repeated)\n", what);
        for (int y = 0; y < h; y++) {
            printf("       \"");
            for (int x = 0; x < w; x++) putchar(count[y * w + x] > 1 ? '2' : count[y * w + x] ? '#' : '.');
            printf("\"\n");
        }
    }
    free(count);
    return ok;
}

static bool MaskFromArt(DtMask *m, int x0, int y0, const char **rows)
{
    int h = RowCount(rows), w = (int)strlen(rows[0]);
    if (!DtMaskInit(m, x0, y0, w, h)) return false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) m->a[y * w + x] = rows[y][x] == '#' ? 255 : 0;
    return true;
}

static bool MaskMatch(const char *what, const DtMask *m, const char **rows)
{
    int h = RowCount(rows), w = (int)strlen(rows[0]);
    bool ok = m->w == w && m->h == h && m->a;
    for (int y = 0; ok && y < h; y++)
        for (int x = 0; x < w; x++) ok = ok && m->a[y * w + x] == (rows[y][x] == '#' ? 255 : 0);
    if (!ok) {
        printf("     %s: got %d x %d\n", what, m->w, m->h);
        for (int y = 0; m->a && y < m->h; y++) {
            printf("       \"");
            for (int x = 0; x < m->w; x++) putchar(m->a[y * m->w + x] == 255 ? '#' : m->a[y * m->w + x] ? '?' : '.');
            printf("\"\n");
        }
    }
    return ok;
}

static bool Adjacent(DtPoint a, DtPoint b)
{
    int dx = abs(a.x - b.x), dy = abs(a.y - b.y);
    return dx <= 1 && dy <= 1 && (dx || dy);
}

// Every consecutive pair (and the last to the first when closed) are 8-neighbours
static bool Connected(const DtPoints *p, bool closed)
{
    for (int i = 0; i + 1 < p->n; i++)
        if (!Adjacent(p->p[i], p->p[i + 1])) return false;
    return !closed || p->n < 2 || Adjacent(p->p[p->n - 1], p->p[0]);
}

static bool Has(const DtPoints *p, int x, int y)
{
    for (int i = 0; i < p->n; i++)
        if (p->p[i].x == x && p->p[i].y == y) return true;
    return false;
}

#define P(x, y) ((DtPoint){ (x), (y) })

// ======================================================================================

static void TestMasks(void)
{
    DtMask m, z, c = { 0 };
    Expect("mask: zero size is empty, no memory", DtMaskInit(&z, 3, 4, 0, 5) && !z.a && z.w == 0 && z.h == 0);
    Expect("mask: zero size reads 0 and ignores sets", (DtMaskSet(&z, 3, 4, 255), DtMaskGet(&z, 3, 4) == 0));
    Expect("mask: negative size is empty", DtMaskInit(&m, 0, 0, -3, 2) && !m.a && DtMaskEmpty(&m));
    DtMaskFree(&m);

    DtMaskInit(&m, 10, 20, 4, 3);
    Expect("mask: new mask is empty", DtMaskEmpty(&m));
    int x0, y0, x1, y1;
    Expect("mask: no bounds when empty", !DtMaskBounds(&m, &x0, &y0, &x1, &y1));
    DtMaskSet(&m, 9, 20, 255);
    DtMaskSet(&m, 14, 20, 255);
    DtMaskSet(&m, 10, 23, 255);
    Expect("mask: sets outside are ignored", DtMaskEmpty(&m) && DtMaskGet(&m, 9, 20) == 0);
    DtMaskSet(&m, 11, 21, 255);
    DtMaskSet(&m, 13, 22, 7);
    Expect("mask: get at atlas coordinates", DtMaskGet(&m, 11, 21) == 255 && DtMaskGet(&m, 13, 22) == 7 &&
           DtMaskGet(&m, 10, 20) == 0);
    Expect("mask: bounds inclusive", DtMaskBounds(&m, &x0, &y0, &x1, &y1) && x0 == 11 && y0 == 21 && x1 == 13 && y1 == 22);

    uint64_t h = DtMaskHash(&m);
    Expect("mask: copy into {0}", DtMaskCopy(&c, &m) && c.a != m.a && DtMaskHash(&c) == h);
    Expect("mask: copy over a mask", DtMaskCopy(&c, &z) && !c.a && DtMaskHash(&c) == DtMaskHash(&z));
    DtMaskCopy(&c, &m);
    c.x++;
    Expect("mask: hash covers the position", DtMaskHash(&c) != h);
    c.x--;
    DtMaskSet(&c, 12, 20, 255);
    Expect("mask: hash covers the bytes", DtMaskHash(&c) != h);
    Expect("mask: hash of an empty mask is the FNV-1a of 16 bytes",
           DtMaskHash(&(DtMask){ 0 }) == 0x88201fb960ff6465ull);
    DtMaskFree(&c);
    DtMaskFree(&m);
    DtMaskFree(&z);
    Expect("mask: free leaves {0}", !z.a && z.w == 0);
}

static void TestLines(void)
{
    DtPoints p = { 0 };
    DtLine(&p, P(2, 3), P(2, 3));
    Expect("line: one point", p.n == 1 && p.p[0].x == 2 && p.p[0].y == 3);

    DtPointsClear(&p);
    DtLine(&p, P(0, 0), P(6, 2));
    Expect("line: shallow Bresenham", PointsMatch("shallow", &p, 0, 0, ROWS(
        "##.....",
        "..###..",
        ".....##")));
    Expect("line: starts at a, ends at b, connected",
           p.p[0].x == 0 && p.p[0].y == 0 && p.p[p.n - 1].x == 6 && p.p[p.n - 1].y == 2 && Connected(&p, false));

    DtPointsClear(&p);
    DtLine(&p, P(2, 5), P(0, 0));
    Expect("line: steep, backwards", PointsMatch("steep", &p, 0, 0, ROWS(
        "#..",
        "#..",
        ".#.",
        ".#.",
        "..#",
        "..#")));
    Expect("line: backwards starts at its a", p.p[0].x == 2 && p.p[0].y == 5 && p.p[p.n - 1].x == 0);

    DtPointsClear(&p);
    DtLine(&p, P(-3, 1), P(1, -3));
    Expect("line: diagonal, negative coordinates", PointsMatch("diagonal", &p, -3, -3, ROWS(
        "....#",
        "...#.",
        "..#..",
        ".#...",
        "#....")));

    // appends: two lines in one list
    DtLine(&p, P(5, 5), P(6, 5));
    Expect("line: appends to the list", p.n == 7 && p.p[5].x == 5 && p.p[6].x == 6);
    DtPointsFree(&p);
}

static void TestPixelPerfect(void)
{
    DtPoints p = { 0 };
    // a freehand staircase (4-connected, as a mouse drag samples it), with a repeat
    int stair[][2] = { { 0, 0 }, { 1, 0 }, { 1, 0 }, { 1, 1 }, { 2, 1 }, { 2, 2 }, { 3, 2 }, { 3, 3 }, { 4, 3 } };
    for (int i = 0; i < 9; i++) DtPointsPush(&p, stair[i][0], stair[i][1]);
    DtPixelPerfect(&p);
    Expect("pixel-perfect: staircase becomes a diagonal", PointsMatch("staircase", &p, 0, 0, ROWS(
        "#....",
        ".#...",
        "..#..",
        "...##")));
    Expect("pixel-perfect: ends kept, connected", p.p[0].x == 0 && p.p[p.n - 1].x == 4 && p.p[p.n - 1].y == 3 &&
           Connected(&p, false));

    DtPointsClear(&p);
    DtLine(&p, P(0, 0), P(7, 3));
    int n = p.n;
    DtPixelPerfect(&p);
    Expect("pixel-perfect: a Bresenham line is unchanged", p.n == n);

    // a single L corner: (0,0) (1,0) (1,1) -> the corner's middle goes
    DtPointsClear(&p);
    DtPointsPush(&p, 0, 0);
    DtPointsPush(&p, 1, 0);
    DtPointsPush(&p, 1, 1);
    DtPixelPerfect(&p);
    Expect("pixel-perfect: L corner", p.n == 2 && p.p[1].x == 1 && p.p[1].y == 1);

    // a right angle of longer arms keeps its corner texel (only 1-texel L's go)
    DtPointsClear(&p);
    DtLine(&p, P(0, 0), P(3, 0));
    DtLine(&p, P(3, 1), P(3, 3));
    DtPixelPerfect(&p);
    Expect("pixel-perfect: a long right angle loses its corner", PointsMatch("right angle", &p, 0, 0, ROWS(
        "###.",
        "...#",
        "...#",
        "...#")));

    DtPointsClear(&p);
    DtPointsPush(&p, 4, 4);
    DtPointsPush(&p, 4, 4);
    DtPixelPerfect(&p);
    Expect("pixel-perfect: repeats of one point", p.n == 1);
    DtPointsClear(&p);
    DtPixelPerfect(&p);
    Expect("pixel-perfect: empty", p.n == 0);
    DtPointsFree(&p);
}

static void TestRect(void)
{
    DtPoints p = { 0 };
    DtRectOutline(&p, P(4, 3), P(0, 0));
    Expect("rect outline: 5 x 4, each texel once", PointsMatch("rect", &p, 0, 0, ROWS(
        "#####",
        "#...#",
        "#...#",
        "#####")) && p.n == 14 && Connected(&p, true));
    DtPointsClear(&p);
    DtRectOutline(&p, P(1, 1), P(1, 4));
    Expect("rect outline: one column", PointsMatch("column", &p, 1, 1, ROWS("#", "#", "#", "#")));
    DtPointsClear(&p);
    DtRectOutline(&p, P(1, 1), P(2, 2));
    Expect("rect outline: 2 x 2", PointsMatch("2x2", &p, 1, 1, ROWS("##", "##")) && p.n == 4);
    DtPointsClear(&p);
    DtRectOutline(&p, P(7, 7), P(7, 7));
    Expect("rect outline: one point", p.n == 1);

    DtMask m;
    DtMaskInit(&m, 0, 0, 5, 4);
    DtRectFill(&m, P(3, -2), P(9, 1));
    DtRectFill(&m, P(0, 3), P(0, 3));
    Expect("rect fill: clipped to the mask", MaskMatch("rect fill", &m, ROWS(
        "...##",
        "...##",
        ".....",
        "#....")));
    DtMaskFree(&m);
    DtPointsFree(&p);
}

static void EllipseCase(int w, int h, const char **rows)
{
    char what[64];
    snprintf(what, sizeof(what), "ellipse %d x %d", w, h);
    DtPoints p = { 0 };
    DtEllipseOutline(&p, P(10, 20), P(10 + w - 1, 20 + h - 1));
    Expect(what, PointsMatch(what, &p, 10, 20, rows));
    DtPointsFree(&p);
}

// Mirror symmetry, the four sides touched, each texel once, corners given either way round
static bool EllipseSymmetric(int w, int h)
{
    DtPoints p = { 0 }, q = { 0 };
    DtEllipseOutline(&p, P(0, 0), P(w - 1, h - 1));
    DtEllipseOutline(&q, P(w - 1, h - 1), P(0, 0));
    bool ok = p.n > 0 && p.n == q.n;
    int minx = w, maxx = -1, miny = h, maxy = -1;
    for (int i = 0; ok && i < p.n; i++) {
        int x = p.p[i].x, y = p.p[i].y;
        ok = Has(&p, w - 1 - x, y) && Has(&p, x, h - 1 - y) && Has(&q, x, y) && (w != h || Has(&p, y, x));
        for (int k = i + 1; ok && k < p.n; k++) ok = p.p[k].x != x || p.p[k].y != y;
        minx = x < minx ? x : minx; maxx = x > maxx ? x : maxx;
        miny = y < miny ? y : miny; maxy = y > maxy ? y : maxy;
    }
    ok = ok && minx == 0 && maxx == w - 1 && miny == 0 && maxy == h - 1;
    DtPointsFree(&p);
    DtPointsFree(&q);
    return ok;
}

static void TestEllipse(void)
{
    EllipseCase(1, 1, ROWS("#"));
    EllipseCase(2, 2, ROWS("##", "##"));
    EllipseCase(1, 5, ROWS("#", "#", "#", "#", "#"));
    EllipseCase(5, 1, ROWS("#####"));
    EllipseCase(2, 5, ROWS("##", "##", "##", "##", "##"));
    EllipseCase(6, 2, ROWS("######", "######"));
    EllipseCase(3, 3, ROWS(
        ".#.",
        "#.#",
        ".#."));
    EllipseCase(4, 4, ROWS(
        ".##.",
        "#..#",
        "#..#",
        ".##."));
    EllipseCase(5, 5, ROWS(
        ".###.",
        "#...#",
        "#...#",
        "#...#",
        ".###."));
    EllipseCase(7, 7, ROWS(
        "..###..",
        ".#...#.",
        "#.....#",
        "#.....#",
        "#.....#",
        ".#...#.",
        "..###.."));
    EllipseCase(11, 11, ROWS(
        "...#####...",
        "..#.....#..",
        ".#.......#.",
        "#.........#",
        "#.........#",
        "#.........#",
        "#.........#",
        "#.........#",
        ".#.......#.",
        "..#.....#..",
        "...#####..."));
    EllipseCase(10, 6, ROWS(
        "..######..",
        ".#......#.",
        "#........#",
        "#........#",
        ".#......#.",
        "..######.."));
    EllipseCase(9, 5, ROWS(
        "..#####..",
        ".#.....#.",
        "#.......#",
        ".#.....#.",
        "..#####.."));
    EllipseCase(8, 4, ROWS(
        "..####..",
        "##....##",
        "##....##",
        "..####.."));
    EllipseCase(11, 3, ROWS(
        "..#######..",
        "##.......##",
        "..#######.."));
    EllipseCase(3, 9, ROWS(
        ".#.",
        ".#.",
        "#.#",
        "#.#",
        "#.#",
        "#.#",
        "#.#",
        ".#.",
        ".#."));

    bool sym = true;
    for (int w = 1; w <= 24; w++)
        for (int h = 1; h <= 24; h++)
            if (!EllipseSymmetric(w, h)) { printf("     not symmetric: %d x %d\n", w, h); sym = false; }
    Expect("ellipse: every size 1..24 x 1..24 symmetric, each texel once, touches its rectangle", sym);

    bool conn = true;
    for (int s = 2; s <= 40; s++) {
        DtPoints p = { 0 };
        DtEllipseOutline(&p, P(0, 0), P(s - 1, s - 1));
        if (!Connected(&p, true)) { printf("     circle %d not a closed path\n", s); conn = false; }
        DtPointsFree(&p);
    }
    Expect("ellipse: circles 2..40 are closed 8-connected paths", conn);

    DtPoints big = { 0 };
    DtEllipseOutline(&big, P(0, 0), P(4095, 4095));
    Expect("ellipse: 4096 x 4096 (64-bit error terms) is a closed path", !big.oom && Connected(&big, true) &&
           Has(&big, 0, 2048) && Has(&big, 4095, 2047) && Has(&big, 2047, 0));
    DtPointsFree(&big);

    DtMask m;
    DtMaskInit(&m, 0, 0, 9, 7);
    DtEllipseFill(&m, P(1, 0), P(7, 6));
    Expect("ellipse fill: outline and inside", MaskMatch("ellipse fill", &m, ROWS(
        "...###...",
        "..#####..",
        ".#######.",
        ".#######.",
        ".#######.",
        "..#####..",
        "...###...")));
    DtMaskFree(&m);
    DtMaskInit(&m, 2, 2, 3, 3);
    DtEllipseFill(&m, P(0, 0), P(6, 6));
    Expect("ellipse fill: clipped to the mask", MaskMatch("ellipse fill clip", &m, ROWS("###", "###", "###")));
    DtMaskFree(&m);
}

static void TestCurve(void)
{
    DtPoints p = { 0 };
    DtCurve(&p, P(0, 7), P(0, 0), P(9, 0), P(9, 7));
    Expect("curve: an arch", PointsMatch("arch", &p, 0, 0, ROWS(
        "..........",
        "..........",
        "..######..",
        ".##....##.",
        ".#......#.",
        "#........#",
        "#........#",
        "#........#")));
    Expect("curve: from a to b, connected", p.p[0].x == 0 && p.p[0].y == 7 && p.p[p.n - 1].x == 9 &&
           p.p[p.n - 1].y == 7 && Connected(&p, false));

    DtPointsClear(&p);
    DtCurve(&p, P(0, 0), P(12, 0), P(0, 9), P(11, 9));
    Expect("curve: an S, each texel once, connected", PointsMatch("S", &p, 0, 0, ROWS(
        "#####.......",
        "....##......",
        ".....##.....",
        "......#.....",
        "......#.....",
        "......#.....",
        "......#.....",
        "......#.....",
        "......###...",
        "........####")) && Connected(&p, false));

    DtPointsClear(&p);
    DtCurve(&p, P(0, 2), P(2, 2), P(5, 2), P(6, 2));
    Expect("curve: straight control points give the row", PointsMatch("straight", &p, 0, 2, ROWS("#######")));
    DtPointsClear(&p);
    DtCurve(&p, P(3, 3), P(3, 3), P(3, 3), P(3, 3));
    Expect("curve: one point", p.n == 1 && p.p[0].x == 3);

    // a loop crosses itself: still each texel once
    DtPointsClear(&p);
    DtCurve(&p, P(0, 6), P(14, 0), P(-6, 0), P(8, 6));
    bool once = !p.oom && p.n > 0;
    for (int i = 0; once && i < p.n; i++)
        for (int k = i + 1; k < p.n; k++) once = once && (p.p[i].x != p.p[k].x || p.p[i].y != p.p[k].y);
    Expect("curve: a loop lists each texel once", once && Has(&p, 0, 6) && Has(&p, 8, 6));
    DtPointsFree(&p);
}

static void TestPolygon(void)
{
    DtMask m;
    DtMaskInit(&m, 0, 0, 7, 7);
    DtPoint arch[] = { P(0, 0), P(6, 0), P(6, 6), P(4, 6), P(4, 2), P(2, 2), P(2, 6), P(0, 6) };
    DtPolygonFill(&m, arch, 8);
    Expect("polygon: concave (an arch), even-odd plus its edges", MaskMatch("arch", &m, ROWS(
        "#######",
        "#######",
        "#######",
        "###.###",
        "###.###",
        "###.###",
        "###.###")));
    DtMaskFree(&m);

    DtMaskInit(&m, 0, 0, 9, 9);
    DtPoint tri[] = { P(0, 0), P(8, 8), P(0, 8) };
    DtPolygonFill(&m, tri, 3);
    Expect("polygon: triangle", MaskMatch("triangle", &m, ROWS(
        "#........",
        "##.......",
        "###......",
        "####.....",
        "#####....",
        "######...",
        "#######..",
        "########.",
        "#########")));
    DtMaskFree(&m);

    // a pentagram: even-odd leaves its centre out, its tips in
    DtMaskInit(&m, 0, 0, 21, 21);
    DtPoint star[] = { P(10, 0), P(16, 20), P(0, 7), P(20, 7), P(4, 20) };
    DtPolygonFill(&m, star, 5);
    Expect("polygon: self-crossing star is even-odd", DtMaskGet(&m, 10, 11) == 0 && DtMaskGet(&m, 10, 3) == 255 &&
           DtMaskGet(&m, 3, 8) == 255 && DtMaskGet(&m, 10, 7) == 255 && DtMaskGet(&m, 0, 0) == 0);
    DtMaskFree(&m);

    // a thin contour (all on one line) still paints its outline; one point paints one texel
    DtMaskInit(&m, 0, 0, 5, 2);
    DtPoint thin[] = { P(0, 0), P(4, 0), P(2, 0) };
    DtPolygonFill(&m, thin, 3);
    DtPoint one[] = { P(3, 1) };
    DtPolygonFill(&m, one, 1);
    DtPolygonFill(&m, one, 0);
    Expect("polygon: degenerate contours", MaskMatch("thin", &m, ROWS("#####", "...#.")));
    DtMaskFree(&m);

    // clipped: vertices far outside the mask
    DtMaskInit(&m, 2, 2, 3, 2);
    DtPoint huge[] = { P(-100, -100), P(100, -100), P(100, 100), P(-100, 100) };
    DtPolygonFill(&m, huge, 4);
    Expect("polygon: clipped to the mask", MaskMatch("huge", &m, ROWS("###", "###")));
    DtMaskFree(&m);
}

// ======================================================================================

typedef struct { unsigned char *px; DtImage img; } TestImage;

// Colours by letter: . transparent black, t transparent white, r g b, R (r near: 250 0 0), k black, w white
static void ImageFromArt(TestImage *t, const char **rows)
{
    int h = RowCount(rows), w = (int)strlen(rows[0]);
    t->px = calloc((size_t)w * h * 4, 1);
    t->img = (DtImage){ t->px, w, h, w * 4 };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char *p = t->px + ((size_t)y * w + x) * 4, c[4] = { 0, 0, 0, 0 };
            switch (rows[y][x]) {
            case 't': memcpy(c, (unsigned char[4]){ 255, 255, 255, 0 }, 4); break;
            case 'r': memcpy(c, (unsigned char[4]){ 255, 0, 0, 255 }, 4); break;
            case 'R': memcpy(c, (unsigned char[4]){ 250, 0, 0, 255 }, 4); break;
            case 'g': memcpy(c, (unsigned char[4]){ 0, 255, 0, 255 }, 4); break;
            case 'b': memcpy(c, (unsigned char[4]){ 0, 0, 255, 255 }, 4); break;
            case 'k': memcpy(c, (unsigned char[4]){ 0, 0, 0, 255 }, 4); break;
            case 'w': memcpy(c, (unsigned char[4]){ 255, 255, 255, 255 }, 4); break;
            default: break;
            }
            memcpy(p, c, 4);
        }
}

static void TestFlood(void)
{
    TestImage t;
    ImageFromArt(&t, ROWS(
        "rrRgrr",
        "rgggrr",
        "rgrgkk",
        "rgggk.",
        "rrrrkt"));
    DtMask all, out;
    DtMaskInit(&all, 0, 0, 6, 5);
    memset(all.a, 255, 30);
    DtMaskInit(&out, 0, 0, 6, 5);

    Expect("flood: contiguous, tolerance 0", DtFlood(&out, &t.img, &all, P(0, 0), 0, true) && MaskMatch("c0", &out, ROWS(
        "##....",
        "#.....",
        "#.....",
        "#.....",
        "####..")));
    Expect("flood: contiguous, tolerance 5 takes the near red", DtFlood(&out, &t.img, &all, P(0, 0), 5, true) &&
           MaskMatch("c5", &out, ROWS(
        "###...",
        "#.....",
        "#.....",
        "#.....",
        "####..")));
    Expect("flood: 4-connected (the inner red is not reached diagonally)", DtMaskGet(&out, 2, 2) == 0);
    Expect("flood: global, tolerance 0", DtFlood(&out, &t.img, &all, P(0, 0), 0, false) && MaskMatch("g0", &out, ROWS(
        "##..##",
        "#...##",
        "#.#...",
        "#.....",
        "####..")));
    Expect("flood: tolerance 255 takes the region", DtFlood(&out, &t.img, &all, P(3, 3), 255, true) &&
           MaskMatch("c255", &out, ROWS("######", "######", "######", "######", "######")));
    Expect("flood: transparent texels match whatever their RGB", DtFlood(&out, &t.img, &all, P(5, 3), 0, true) &&
           MaskMatch("transparent", &out, ROWS("......", "......", "......", ".....#", ".....#")));

    // a UV-island-like region with a hole: the flood stays inside it, the hole blocks it
    DtMask island;
    MaskFromArt(&island, 0, 0, ROWS(
        "####..",
        "#..#..",
        "#..##.",
        "#####.",
        "...##."));
    Expect("flood: inside a region with a hole", DtFlood(&out, &t.img, &island, P(0, 3), 0, true) &&
           MaskMatch("island", &out, ROWS(
        "##....",
        "#.....",
        "#.....",
        "#.....",
        "......")));
    Expect("flood: global inside the region", DtFlood(&out, &t.img, &island, P(0, 0), 0, false) &&
           MaskMatch("island global", &out, ROWS(
        "##....",
        "#.....",
        "#.....",
        "#.....",
        "...#..")));
    Expect("flood: seed outside the region fails, out cleared", !DtFlood(&out, &t.img, &island, P(1, 1), 0, true) &&
           DtMaskEmpty(&out));
    Expect("flood: seed outside the image fails", !DtFlood(&out, &t.img, &all, P(6, 0), 0, true));
    DtMask other;
    DtMaskInit(&other, 0, 0, 5, 5);
    Expect("flood: out of another rectangle fails", !DtFlood(&other, &t.img, &all, P(0, 0), 0, true));
    DtMask zr = { 0 }, zo = { 0 };
    Expect("flood: zero-size region fails", !DtFlood(&zo, &t.img, &zr, P(0, 0), 0, true));

    // a region larger than the image (atlas edge): texels past the image are never taken
    DtMask wide, wout;
    DtMaskInit(&wide, -2, -2, 10, 9);
    memset(wide.a, 255, 90);
    DtMaskInit(&wout, -2, -2, 10, 9);
    Expect("flood: region past the image", DtFlood(&wout, &t.img, &wide, P(0, 0), 255, true) &&
           DtMaskGet(&wout, -1, 0) == 0 && DtMaskGet(&wout, 5, 4) == 255 && DtMaskGet(&wout, 6, 4) == 0);

    DtMaskFree(&wide); DtMaskFree(&wout); DtMaskFree(&other);
    DtMaskFree(&island); DtMaskFree(&all); DtMaskFree(&out);
    free(t.px);

    unsigned char a[4] = { 10, 20, 30, 255 }, b[4] = { 13, 18, 30, 252 };
    Expect("colour near: largest channel difference", DtColorNear(a, b, 3) && !DtColorNear(a, b, 2) && DtColorNear(a, a, 0));
}

static bool Near(float a, float b) { return a > b - 1e-5f && a < b + 1e-5f; }

static void TestGradient(void)
{
    DtPoint a = P(0, 0), b = P(10, 0);
    Expect("gradient: linear 0 at a, 1 at b, 0.5 between", Near(DtGradientT(0, 3, a, b, false), 0.0f) &&
           Near(DtGradientT(10, 0, a, b, false), 1.0f) && Near(DtGradientT(5, -7, a, b, false), 0.5f));
    Expect("gradient: linear clamped", Near(DtGradientT(-4, 0, a, b, false), 0.0f) && Near(DtGradientT(14, 0, a, b, false), 1.0f));
    Expect("gradient: diagonal projection", Near(DtGradientT(4, 0, P(0, 0), P(4, 4), false), 0.5f));
    Expect("gradient: radial", Near(DtGradientT(0, 5, a, b, true), 0.5f) && Near(DtGradientT(6, 8, a, b, true), 1.0f) &&
           Near(DtGradientT(-3, -4, a, b, true), 0.5f) && Near(DtGradientT(0, 0, a, b, true), 0.0f));
    Expect("gradient: a == b", Near(DtGradientT(3, 3, P(3, 3), P(3, 3), false), 0.0f) &&
           Near(DtGradientT(3, 3, P(3, 3), P(3, 3), true), 0.0f) && Near(DtGradientT(4, 3, P(3, 3), P(3, 3), true), 1.0f));
}

static bool Rgba(const unsigned char *p, int r, int g, int b, int a)
{
    bool ok = p[0] == r && p[1] == g && p[2] == b && p[3] == a;
    if (!ok) printf("     got %d %d %d %d, want %d %d %d %d\n", p[0], p[1], p[2], p[3], r, g, b, a);
    return ok;
}

static void TestBlur(void)
{
    // red, transparent blue, green: the transparent texel's colour has no weight
    TestImage t;
    ImageFromArt(&t, ROWS("r.g"));
    t.px[4 + 2] = 255;      // the transparent texel is blue (alpha 0)
    DtMask region, where;
    MaskFromArt(&region, 0, 0, ROWS("###"));
    MaskFromArt(&where, 0, 0, ROWS("###"));
    unsigned char out[12];
    memset(out, 99, sizeof(out));
    DtBlur(out, &t.img, &region, &where);
    Expect("blur: alpha-weighted colour, plain mean alpha", Rgba(out + 4, 128, 128, 0, 170));
    Expect("blur: edge texel reads only texels in the image", Rgba(out, 255, 0, 0, 128) && Rgba(out + 8, 0, 255, 0, 128));

    // the region leaves the green out: it is neither read nor written
    DtMaskFree(&region);
    MaskFromArt(&region, 0, 0, ROWS("##."));
    memset(out, 99, sizeof(out));
    DtBlur(out, &t.img, &region, &where);
    Expect("blur: only region texels are read", Rgba(out + 4, 255, 0, 0, 128));
    Expect("blur: texels outside the region are left as they are", Rgba(out + 8, 99, 99, 99, 99));

    // all transparent: own colour, alpha 0
    TestImage z;
    ImageFromArt(&z, ROWS("tt"));
    DtMask r2, w2;
    MaskFromArt(&r2, 0, 0, ROWS("##"));
    MaskFromArt(&w2, 1, 0, ROWS("#"));
    unsigned char o2[4] = { 1, 1, 1, 1 };
    DtBlur(o2, &z.img, &r2, &w2);
    Expect("blur: all transparent keeps its colour, alpha 0", Rgba(o2, 255, 255, 255, 0));

    DtMask empty = { 0 };
    DtBlur(o2, &z.img, &r2, &empty);
    Expect("blur: empty where writes nothing", Rgba(o2, 255, 255, 255, 0));
    DtMaskFree(&r2); DtMaskFree(&w2); free(z.px);
    DtMaskFree(&region); DtMaskFree(&where); free(t.px);
}

static void TestJumble(void)
{
    int w = 16, h = 16;
    unsigned char *px = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        px[i * 4 + 0] = (unsigned char)i;
        px[i * 4 + 1] = (unsigned char)(i * 7);
        px[i * 4 + 2] = 0;
        px[i * 4 + 3] = 255;
    }
    DtImage img = { px, w, h, w * 4 };
    DtMask region, where;
    DtMaskInit(&region, 0, 0, w, h);
    memset(region.a, 255, (size_t)w * h);
    DtMaskSet(&region, 8, 8, 0);
    DtMaskInit(&where, 0, 0, w, h);
    memset(where.a, 255, (size_t)w * h);
    unsigned char *a = calloc((size_t)w * h * 4, 1), *b = calloc((size_t)w * h * 4, 1), *c = calloc((size_t)w * h * 4, 1);
    DtJumble(a, &img, &region, &where, 1234);
    DtJumble(b, &img, &region, &where, 1234);
    DtJumble(c, &img, &region, &where, 1235);
    Expect("jumble: same seed, same pixels", memcmp(a, b, (size_t)w * h * 4) == 0);
    Expect("jumble: another seed, other pixels", memcmp(a, c, (size_t)w * h * 4) != 0);

    // each texel's colour is one of its 8 neighbours' in the region (each colour here is unique by index)
    bool fromNeighbour = true;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (x == 8 && y == 8) continue;
            int src = a[(y * w + x) * 4];
            int sx = src % w, sy = src / w;
            bool ok = abs(sx - x) <= 1 && abs(sy - y) <= 1 && (sx != x || sy != y) && !(sx == 8 && sy == 8);
            fromNeighbour = fromNeighbour && ok;
        }
    Expect("jumble: a neighbour in the region, never itself nor a texel outside", fromNeighbour);
    Expect("jumble: texel outside the region left as it is", a[(8 * w + 8) * 4 + 3] == 0);

    // the pick doesn't depend on where's shape
    DtMask one;
    DtMaskInit(&one, 5, 6, 1, 1);
    one.a[0] = 255;
    unsigned char o[4];
    DtJumble(o, &img, &region, &one, 1234);
    Expect("jumble: a one-texel stamp picks as the whole", memcmp(o, a + (6 * w + 5) * 4, 4) == 0);

    // a texel with no neighbour in the region keeps its colour
    DtMask lone;
    DtMaskInit(&lone, 3, 3, 1, 1);
    lone.a[0] = 255;
    DtJumble(o, &img, &lone, &lone, 99);
    Expect("jumble: no neighbour keeps its own colour", memcmp(o, px + (3 * w + 3) * 4, 4) == 0);

    DtMaskFree(&lone); DtMaskFree(&one); DtMaskFree(&region); DtMaskFree(&where);
    free(a); free(b); free(c); free(px);
}

static void TestSpray(void)
{
    DtPoints a = { 0 }, b = { 0 }, c = { 0 };
    DtSpray(&a, 5.5f, 5.5f, 4.0f, 6, 1u);
    DtSpray(&b, 5.5f, 5.5f, 4.0f, 6, 1u);
    DtSpray(&c, 5.5f, 5.5f, 4.0f, 6, 2u);
    Expect("spray: same seed, same texels", a.n == 6 && b.n == 6 && memcmp(a.p, b.p, 6 * sizeof(DtPoint)) == 0);
    Expect("spray: another seed, other texels", memcmp(a.p, c.p, 6 * sizeof(DtPoint)) != 0);
    // the integer hash makes these the texels on every platform
    int want[6][2] = { { 5, 2 }, { 3, 4 }, { 5, 4 }, { 7, 3 }, { 7, 4 }, { 8, 6 } };
    bool golden = a.n == 6;
    for (int i = 0; golden && i < 6; i++) golden = a.p[i].x == want[i][0] && a.p[i].y == want[i][1];
    Expect("spray: seed 1 gives the recorded texels", golden);

    DtPointsClear(&a);
    DtSpray(&a, 20.25f, -3.5f, 6.0f, 4000, 77u);
    bool inDisc = a.n == 4000;
    int left = 0;
    for (int i = 0; i < a.n; i++) {
        float dx = a.p[i].x + 0.5f - 20.25f, dy = a.p[i].y + 0.5f - -3.5f;
        inDisc = inDisc && dx * dx + dy * dy <= (6.0f + 0.71f) * (6.0f + 0.71f);
        left += a.p[i].x + 0.5f < 20.25f;
    }
    Expect("spray: every texel in the disc", inDisc);
    Expect("spray: about half left of the centre", left > 1700 && left < 2300);

    DtPointsClear(&a);
    DtSpray(&a, 2.7f, 3.2f, 0.0f, 3, 5u);
    Expect("spray: radius 0 is the centre's texel", a.n == 3 && a.p[0].x == 2 && a.p[2].y == 3);
    DtPointsClear(&a);
    DtSpray(&a, 0, 0, 3, 0, 5u);
    DtSpray(&a, 0, 0, 3, -2, 5u);
    Expect("spray: count 0 adds nothing", a.n == 0);
    DtPointsFree(&a); DtPointsFree(&b); DtPointsFree(&c);
}

// ======================================================================================

static void TestSelection(void)
{
    DtMask d, s, r;
    const char **D = ROWS("##..", "##..", "....");
    const char **S = ROWS(".##.", ".##.", ".##.");
    MaskFromArt(&s, 0, 0, S);

    MaskFromArt(&d, 0, 0, D);
    DtMaskCombine(&d, &s, DT_SEL_REPLACE);
    Expect("select: replace", MaskMatch("replace", &d, S));
    DtMaskFree(&d);
    MaskFromArt(&d, 0, 0, D);
    DtMaskCombine(&d, &s, DT_SEL_ADD);
    Expect("select: add", MaskMatch("add", &d, ROWS("###.", "###.", ".##.")));
    DtMaskFree(&d);
    MaskFromArt(&d, 0, 0, D);
    DtMaskCombine(&d, &s, DT_SEL_SUB);
    Expect("select: subtract", MaskMatch("sub", &d, ROWS("#...", "#...", "....")));
    DtMaskFree(&d);
    MaskFromArt(&d, 0, 0, D);
    DtMaskCombine(&d, &s, DT_SEL_AND);
    Expect("select: intersect", MaskMatch("and", &d, ROWS(".#..", ".#..", "....")));

    // an island-like region with a hole: invert stays inside it
    MaskFromArt(&r, 0, 0, ROWS("####", "#.##", "###."));
    DtMaskInvert(&d, &r);
    Expect("select: invert within the region", MaskMatch("invert", &d, ROWS("#.##", "#.##", "###.")));
    DtMaskInvert(&d, &r);
    Expect("select: invert twice is the selection clipped", MaskMatch("invert twice", &d, ROWS(".#..", "....", "....")));
    DtMaskFree(&d);
    MaskFromArt(&d, 0, 0, ROWS("####", "####", "####"));
    DtMaskClip(&d, &r);
    Expect("select: clip to the region", MaskMatch("clip", &d, ROWS("####", "#.##", "###.")));

    DtMask moved;
    DtMaskInit(&moved, 0, 0, 4, 3);
    memset(moved.a, 255, 12);
    DtMaskShift(&moved, &s, 2, 1);
    Expect("select: shift, clipped at the edges", MaskMatch("shift", &moved, ROWS("....", "...#", "...#")));
    DtMaskShift(&moved, &s, -1, -1);
    Expect("select: shift back past the corner", MaskMatch("shift neg", &moved, ROWS("##..", "##..", "....")));
    DtMaskShift(&moved, &s, 2000000000, -2000000000);
    Expect("select: shift far away is empty", DtMaskEmpty(&moved));

    // degenerate: zero-size masks on either side
    DtMask z = { 0 };
    DtMaskCombine(&z, &s, DT_SEL_ADD);
    DtMaskInvert(&z, &r);
    DtMaskClip(&z, &r);
    DtMaskShift(&z, &s, 1, 1);
    DtMaskCombine(&d, &z, DT_SEL_ADD);
    Expect("select: zero-size masks change nothing", DtMaskEmpty(&z) && MaskMatch("zero", &d, ROWS("####", "#.##", "###.")));
    DtMaskCombine(&d, &z, DT_SEL_AND);
    Expect("select: intersect with nothing is empty", DtMaskEmpty(&d));

    DtMaskFree(&moved); DtMaskFree(&d); DtMaskFree(&s); DtMaskFree(&r);
}

int main(void)
{
    TestMasks();
    TestLines();
    TestPixelPerfect();
    TestRect();
    TestEllipse();
    TestCurve();
    TestPolygon();
    TestFlood();
    TestGradient();
    TestBlur();
    TestJumble();
    TestSpray();
    TestSelection();
    if (fails) printf("drawtools: %d FAILED\n", fails);
    else printf("drawtools: all passed\n");
    return fails ? 1 : 0;
}
