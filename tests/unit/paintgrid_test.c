// tests/unit/paintgrid_test.c: paint on a grid lands in the cell under the brush outline, sharp
// (docs/painting.md 17.2, 17.6, 17.7). `./build.sh test paintgrid` (after ./build.sh: part B runs
// build/linux/monolith):
//
//   paintgrid_test <repo root> <scratch folder>
//
// A. The mapping, without a GPU: a map written into the scratch folder (axis-aligned boxes off and on
//    the grid's lines, at negative coordinates, turned boxes; grids of 8, 20 and 24 texels per metre)
//    loaded noGpu. For every face of every box: MS_BoxFaceCells gives the face's cells and the place
//    of its 0 corner, and MS_RaycastSurface at several points of the face gives that face, its
//    paintTexel (the hit in texels from the face's first cell), paintCellsW / H and faceUv, all
//    against the formula of 17.2 computed here from the box and the hit point: g = rho * sign * c
//    along the face's u and v (c the world coordinate, a turned box's along its own axis), the cells
//    floor(g0 + 1/256) .. ceil(g1 - 1/256), the texel g - first. faceUv is the paint stage's ms_uv,
//    so paintTexel = frac + faceUv * (end - frac) is the texel the paint table draws there.
// B. Draw mode end to end: monolith --paint-script + --shot --draw-mode --shot-hud (volumetric fog, SSAO,
//    SSIL and bloom off: they blur the cell's edges) on a small package
//    of unlit white boxes (merged, turned, instanced) on an 8 / m grid. One size-1 square black dab per
//    case, the cursor left on it: the black cell's bounds equal the brush outline's (the cursor's
//    colour) within 2 px, on every face direction, and the cell is sharp (the ring of pixels 3..6 px
//    around it is the surface's white: trilinear magnification darkens it). The first stroke on the
//    grid wrote `paint_filter nearest` into the map; `pixel off` before it (the control) writes no
//    key and gives the soft cell. A face of a face group says on the target line how many of the
//    rectangle's cells it covers.
#include "monolith_scene.h"

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;

#define FAIL(...) do { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } while (0)

// A heap copy of the formatted text (exits when memory is out: a test has nothing better to do)
static char *Format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *Format(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (!s) { fprintf(stderr, "paintgrid_test: out of memory\n"); exit(2); }
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

static bool WriteFile(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "paintgrid_test: cannot write %s\n", path); return false; }
    fputs(text, f);
    return fclose(f) == 0;
}

// The whole file as a heap string (NULL: unreadable)
static char *ReadFile(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (s) s[fread(s, 1, (size_t)n, f)] = 0;
    fclose(f);
    return s;
}

// ======================================================================================
// #region A. The mapping (noGpu)
// ======================================================================================

#define GRID_EPS (1.0 / 256.0)      // grid coordinates this close to a cell edge count as on it (17.2)

// docs/painting.md 17.2: the axis (0 x, 1 y, 2 z) and direction each face's u and v run along
static const int U_AXIS[6] = { 2, 2, 0, 0, 0, 0 }, U_SIGN[6] = { -1, 1, 1, 1, 1, -1 };
static const int V_AXIS[6] = { 1, 1, 2, 2, 1, 1 }, V_SIGN[6] = { 1, 1, -1, 1, 1, 1 };
static const char *const FACE[6] = { "+x", "-x", "+y", "-y", "+z", "-z" };

// One axis of a face on the grid: g0 .. g1 the face, first the first cell, cells how many
typedef struct { double g0, g1, first; int cells; } Span;

// The box's own axis k (world axes for an axis-aligned box) and its centre and size
static void BoxFrame(const MS_Scene *s, int b, Vector3 axis[3], Vector3 *center, Vector3 *size)
{
    if (s->boxTurned && s->boxTurned[b] >= 0) {
        const MS_TurnedBox *t = &s->turnedBoxes[s->boxTurned[b]];
        for (int k = 0; k < 3; k++) axis[k] = t->axis[k];
        *center = t->center;
        *size = t->size;
        return;
    }
    axis[0] = (Vector3){ 1, 0, 0 };
    axis[1] = (Vector3){ 0, 1, 0 };
    axis[2] = (Vector3){ 0, 0, 1 };
    BoundingBox B = s->boxes[b];
    *center = (Vector3){ (B.min.x + B.max.x) / 2, (B.min.y + B.max.y) / 2, (B.min.z + B.max.z) / 2 };
    *size = (Vector3){ B.max.x - B.min.x, B.max.y - B.min.y, B.max.z - B.min.z };
}

static double Dot(Vector3 a, Vector3 b) { return (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z; }
static double Comp(Vector3 v, int k) { return k == 0 ? v.x : k == 1 ? v.y : v.z; }

// The face's span along a box axis k in direction sign, on a grid of rho texels per metre: the box's
// world bounds (axis-aligned) or its extent along its own axis through the world origin (turned)
static Span SpanOf(const MS_Scene *s, int b, int k, int sign, double rho)
{
    double lo, hi;
    if (s->boxTurned && s->boxTurned[b] >= 0) {
        Vector3 axis[3], c, size;
        BoxFrame(s, b, axis, &c, &size);
        lo = Dot(axis[k], c) - Comp(size, k) / 2;
        hi = Dot(axis[k], c) + Comp(size, k) / 2;
    } else {
        lo = Comp(s->boxes[b].min, k);
        hi = Comp(s->boxes[b].max, k);
    }
    Span sp;
    sp.g0 = sign > 0 ? rho * lo : -rho * hi;
    sp.g1 = sign > 0 ? rho * hi : -rho * lo;
    sp.first = floor(sp.g0 + GRID_EPS);
    double last = ceil(sp.g1 - GRID_EPS);
    if (last < sp.first + 1) last = sp.first + 1;
    sp.cells = (int)(last - sp.first);
    return sp;
}

// The texel of a grid coordinate g on a span (from its first cell, inside the face's extent)
static double TexelOf(Span sp, double g)
{
    double lo = fmax(sp.g0 - sp.first, 0.0), hi = fmin(sp.g1, sp.first + sp.cells) - sp.first;
    double t = g - sp.first;
    return t < lo ? lo : t > hi ? hi : t;
}

static void Near(bool *ok, const char *what, double got, double want, double tol, const char *where)
{
    if (fabs(got - want) <= tol) return;
    FAIL("%s: %s is %.5f, want %.5f", where, what, got, want);
    *ok = false;
}

// Every face of box b on its material's grid: cells, offset, and the surface hit at several points
static void CheckBox(const MS_Scene *s, int b, const char *name)
{
    int m = s->boxRefs[b].material;
    double rho = MS_PaintGridOf(s, m);
    if (!(rho > 0)) { FAIL("%s: no paint grid", name); return; }
    Vector3 axis[3], c, size;
    BoxFrame(s, b, axis, &c, &size);
    static const float PTS[4][2] = { { 0.13f, 0.21f }, { 0.5f, 0.5f }, { 0.87f, 0.64f }, { 0.31f, 0.93f } };
    for (int f = 0; f < 6; f++) {
        bool ok = true;
        char where[96];
        snprintf(where, sizeof(where), "%s face %s", name, FACE[f]);
        Span su = SpanOf(s, b, U_AXIS[f], U_SIGN[f], rho), sv = SpanOf(s, b, V_AXIS[f], V_SIGN[f], rho);
        int w, h;
        Vector2 off;
        if (!MS_BoxFaceCells(s, b, f, (float)rho, &w, &h, &off)) { FAIL("%s: MS_BoxFaceCells failed", where); continue; }
        Near(&ok, "cells w", w, su.cells, 0, where);
        Near(&ok, "cells h", h, sv.cells, 0, where);
        Near(&ok, "offset u", off.x, TexelOf(su, su.g0), 1e-4, where);
        Near(&ok, "offset v", off.y, TexelOf(sv, sv.g0), 1e-4, where);
        // The face's normal axis and the two others (in order), its points in the box's own frame
        int n = f / 2, a = n == 0 ? 1 : 0, bb = n == 2 ? 1 : 2;
        double out = f % 2 == 0 ? 0.5 : -0.5;
        for (int p = 0; p < 4; p++) {
            double l[3];
            l[n] = out;
            l[a] = PTS[p][0] - 0.5;
            l[bb] = PTS[p][1] - 0.5;
            Vector3 P = c;
            for (int k = 0; k < 3; k++) {
                double d = l[k] * Comp(size, k);
                P.x += (float)(d * axis[k].x);
                P.y += (float)(d * axis[k].y);
                P.z += (float)(d * axis[k].z);
            }
            Vector3 nrm = { (float)(out * 2 * axis[n].x), (float)(out * 2 * axis[n].y), (float)(out * 2 * axis[n].z) };
            Vector3 o = { P.x + 0.3f * nrm.x, P.y + 0.3f * nrm.y, P.z + 0.3f * nrm.z };
            MS_SurfaceHit hit;
            if (!MS_RaycastSurface(s, o, (Vector3){ -nrm.x, -nrm.y, -nrm.z }, 0.0f, &hit) || hit.hit.obj.kind != MS_OBJ_BOX ||
                hit.hit.obj.index != b || hit.face != f) {
                FAIL("%s point %d: the ray hit object %d face %d", where, p, hit.hit.obj.index, hit.face);
                ok = false;
                continue;
            }
            // (the grid coordinates of the hit point, 17.2)
            double gu = rho * U_SIGN[f] * Dot(axis[U_AXIS[f]], hit.hit.point), gv = rho * V_SIGN[f] * Dot(axis[V_AXIS[f]], hit.hit.point);
            double tu = TexelOf(su, gu), tv = TexelOf(sv, gv);
            Near(&ok, "paintTexel u", hit.paintTexel.x, tu, 2e-3, where);
            Near(&ok, "paintTexel v", hit.paintTexel.y, tv, 2e-3, where);
            Near(&ok, "paintCellsW", hit.paintCellsW, su.cells, 0, where);
            Near(&ok, "paintCellsH", hit.paintCellsH, sv.cells, 0, where);
            Near(&ok, "paintGrid", hit.paintGrid, rho, 0, where);
            Near(&ok, "faceUv u", hit.faceUv.x, (gu - su.g0) / (su.g1 - su.g0), 1e-4, where);
            Near(&ok, "faceUv v", hit.faceUv.y, (gv - sv.g0) / (sv.g1 - sv.g0), 1e-4, where);
            // (the paint table maps the face's 0..1 onto frac .. end of its cells: the texel drawn there)
            double du = TexelOf(su, su.g0) + hit.faceUv.x * (TexelOf(su, su.g1) - TexelOf(su, su.g0));
            double dv = TexelOf(sv, sv.g0) + hit.faceUv.y * (TexelOf(sv, sv.g1) - TexelOf(sv, sv.g0));
            Near(&ok, "drawn texel u", hit.paintTexel.x, du, 2e-3, where);
            Near(&ok, "drawn texel v", hit.paintTexel.y, dv, 2e-3, where);
            if (floor(hit.paintTexel.x) < 0 || floor(hit.paintTexel.x) >= su.cells || floor(hit.paintTexel.y) < 0 ||
                floor(hit.paintTexel.y) >= sv.cells) {
                FAIL("%s point %d: texel %.3f %.3f outside the %dx%d cells", where, p, hit.paintTexel.x, hit.paintTexel.y, su.cells, sv.cells);
                ok = false;
            }
        }
        printf("%s %s: %dx%d cells\n", ok ? "ok  " : "FAIL", where, w, h);
    }
}

static bool PartA(const char *dir)
{
    char *path = Format("%s/grid.map", dir);
    bool written = WriteFile(path,
        "monolith 3\n"
        "env paint.grid 8\n"
        "material \"g\" albedo 200 200 200\n"
        "material \"g20\" albedo 200 200 200 paint_grid 20\n"
        "material \"g24\" albedo 200 200 200 paint_grid 24\n"
        "use \"g\"\n"
        "box 0.37 30.13 30.21 1.13 30.9 30.83\n"                  // 0: off the grid's lines
        "box 3.37 30.13 30.21 4.13 30.9 30.83 rot 30 20 10\n"     // 1: turned
        "box -5.31 -2.2 -7.77 -3.9 -1.03 -6.01\n"                 // 2: negative coordinates
        "box 9.9 0.05 0.05 9.95 0.1 0.1\n"                        // 3: smaller than a cell
        "use \"g20\"\n"
        "box 0.35 0.7 1.05 1.05 1.4 1.75\n"                       // 4: sides on grid lines, as floats a hair off
        "use \"g24\"\n"
        "box -12.3 4.1 9.7 -10.05 6.6 11.2 rot 45 0 0\n");        // 5: yawed 45 degrees
    if (!written) { free(path); return false; }
    MS_Scene *s = MS_Load(path, &(MS_LoadOptions){ .noGpu = true });
    free(path);
    if (!s) { fprintf(stderr, "paintgrid_test: %s\n", MS_LastError()); return false; }
    static const char *const NAMES[] = { "box off the lines", "turned box", "box at negative coordinates", "box under a cell",
                                         "box on the lines (20/m)", "yawed box (24/m)" };
    for (int b = 0; b < s->boxCount && b < 6; b++) CheckBox(s, b, NAMES[b]);
    if (s->boxCount != 6) FAIL("the map has %d boxes, want 6", s->boxCount);
    MS_Unload(s);
    return true;
}

// #endregion

// ======================================================================================
// #region B. Draw mode end to end (build/linux/monolith)
// ======================================================================================

// Runs monolith with args (NULL-terminated) into log; its exit status (-1: not run)
static int Run(const char *monolith, const char *log, const char *const *args)
{
    const char *argv[40];
    int n = 0;
    argv[n++] = monolith;
    for (int i = 0; args[i] && n < 39; i++) argv[n++] = args[i];
    argv[n] = NULL;
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        FILE *f = freopen(log, "w", stdout);
        if (!f || dup2(fileno(stdout), 2) < 0) _exit(127);
        execv(monolith, (char *const *)argv);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// The package of part B: unlit white boxes on an 8 / m grid (no light, so a pixel is its albedo)
static const char PACKAGE[] =
    "monolith 3\n"
    "env paint.grid 8\n"
    "material \"white\" albedo 230 230 230 unlit on\n"
    "material \"white inst\" albedo 230 230 230 unlit on instanced on\n"
    "viewpoint \"v\" 0 31 25 0.75 30.5 30.5\n"
    "use \"white\"\n"
    "box 0.37 30.13 30.21 1.13 30.9 30.83\n"
    "box 3.37 30.13 30.21 4.13 30.9 30.83 rot 30 20 10\n"
    "use \"white inst\"\n"
    "box 6.37 30.13 30.21 7.13 30.9 30.83\n";

// One click: the camera (eye, yaw, pitch) and the cursor (shot pixels)
typedef struct { const char *name; float eye[3], yaw, pitch; int px, py; } Click;

static const Click CLICKS[] = {
    { "box -x", { -0.6f, 30.515f, 30.52f }, 90, 0, 950, 350 },
    { "box +x", { 2.1f, 30.515f, 30.52f }, 270, 0, 950, 350 },
    { "box -z", { 0.75f, 30.515f, 29.3f }, 0, 0, 950, 350 },
    { "box +z", { 0.75f, 30.515f, 31.8f }, 180, 0, 950, 350 },
    { "box +y", { 0.75f, 31.7f, 30.52f }, 0, -89, 950, 350 },
    { "box -y", { 0.75f, 29.3f, 30.52f }, 0, 89, 950, 350 },
    { "turned box, its -x", { 2.6f, 30.515f, 30.52f }, 90, 0, 860, 400 },
    { "turned box, its +y", { 3.75f, 31.7f, 30.52f }, 0, -89, 850, 400 },
    { "instanced box -x", { 5.4f, 30.515f, 30.52f }, 90, 0, 950, 350 },
    { "instanced box +y", { 6.75f, 31.7f, 30.52f }, 0, -89, 650, 520 },
};

typedef struct { int x0, y0, x1, y1, n; } Bounds;

static void Grow(Bounds *b, int x, int y)
{
    if (b->n++ == 0) { b->x0 = b->x1 = x; b->y0 = b->y1 = y; return; }
    if (x < b->x0) b->x0 = x;
    if (x > b->x1) b->x1 = x;
    if (y < b->y0) b->y0 = y;
    if (y > b->y1) b->y1 = y;
}

static bool IsCursor(Color c) { return c.b > 200 && c.g > 160 && c.r < 170; }   // (120 210 255 drawn on white)
static int Lum(Color c) { return c.r + c.g + c.b; }

// Shot `png`: the brush outline's and the black cell's bounds (below the HUD lines), and how many
// pixels of the ring 3..6 px around the cell are darker than the surface (its brightest pixel there,
// less 10 %: trilinear magnification darkens it by a quarter and more). False: unreadable
static bool Measure(const char *png, Bounds *outline, Bounds *cell, int *dark, int *ring)
{
    Image im = LoadImage(png);
    if (!im.data) return false;
    Color *px = LoadImageColors(im);
    *outline = *cell = (Bounds){ 0 };
    *dark = *ring = 0;
    for (int y = 80; y < im.height; y++)
        for (int x = 0; x < im.width; x++)
            if (IsCursor(px[y * im.width + x])) Grow(outline, x, y);
    // (the black cell: near the outline only, the sky may be dark)
    for (int y = outline->y0 - 30; outline->n && y <= outline->y1 + 30; y++)
        for (int x = outline->x0 - 30; x <= outline->x1 + 30; x++)
            if (x >= 0 && y >= 80 && x < im.width && y < im.height && Lum(px[y * im.width + x]) < 3 * 40) Grow(cell, x, y);
    for (int pass = 0, surface = 0; pass < 2 && cell->n; pass++)
        for (int y = cell->y0 - 6; y <= cell->y1 + 6; y++)
            for (int x = cell->x0 - 6; x <= cell->x1 + 6; x++) {
                if (x < 0 || y < 0 || x >= im.width || y >= im.height) continue;
                if (x >= cell->x0 - 2 && x <= cell->x1 + 2 && y >= cell->y0 - 2 && y <= cell->y1 + 2) continue;
                Color c = px[y * im.width + x];
                if (IsCursor(c)) continue;
                if (pass == 0) { if (Lum(c) > surface) surface = Lum(c); continue; }
                (*ring)++;
                if (Lum(c) * 10 < surface * 9) (*dark)++;
            }
    UnloadImageColors(px);
    UnloadImage(im);
    return true;
}

// One click in a fresh copy of the package (pixel: "", or "pixel off\n" for the control): its shot
// measured; *nearest whether the map then has paint_filter nearest. False: monolith failed
static bool Click1(const char *monolith, const char *dir, int i, const char *pixel, Bounds *outline, Bounds *cell, int *dark, int *ring,
                   bool *nearest)
{
    const Click *k = &CLICKS[i];
    char *pkg = Format("%s/click%d%s", dir, i, pixel[0] ? "c" : "");
    mkdir(pkg, 0755);
    char *map = Format("%s/grid.map", pkg), *script = Format("%s/click.txt", pkg), *png = Format("%s/shot.png", pkg);
    char *log = Format("%s/log.txt", pkg);
    char *text = Format("tool pencil\nbrush square\nsize 1\nfg 0 0 0 255\n%shover px %d %d\ndab px %d %d\nhover px %d %d\nsave\n", pixel,
                        k->px, k->py, k->px, k->py, k->px, k->py);
    char ex[32], ey[32], ez[32], yaw[32], pitch[32], cx[16], cy[16];
    snprintf(ex, sizeof(ex), "%g", k->eye[0]);
    snprintf(ey, sizeof(ey), "%g", k->eye[1]);
    snprintf(ez, sizeof(ez), "%g", k->eye[2]);
    snprintf(yaw, sizeof(yaw), "%g", k->yaw);
    snprintf(pitch, sizeof(pitch), "%g", k->pitch);
    snprintf(cx, sizeof(cx), "%d", k->px);
    snprintf(cy, sizeof(cy), "%d", k->py);
    const char *args[] = { "--map", map, "--shot", png, "--view", "1", "--eye", ex, ey, ez, "--look", yaw, pitch, "--paint-script", script,
                           "--draw-mode", "--shot-hud", "--cursor", cx, cy, "--off", "1345", NULL };
    bool ok = WriteFile(map, PACKAGE) && WriteFile(script, text);
    int status = ok ? Run(monolith, log, args) : -1;
    if (status != 0) {
        FAIL("%s: monolith exited %d (its output: %s)", k->name, status, log);
        ok = false;
    }
    if (ok && !Measure(png, outline, cell, dark, ring)) {
        FAIL("%s: cannot read %s", k->name, png);
        ok = false;
    }
    char *saved = ok ? ReadFile(map) : NULL;
    *nearest = saved && strstr(saved, "paint_filter nearest");
    free(saved);
    free(pkg);
    free(map);
    free(script);
    free(png);
    free(log);
    free(text);
    return ok;
}

static void PartB(const char *root, const char *dir)
{
    char *monolith = Format("%s/build/linux/monolith", root);
    if (access(monolith, X_OK) != 0) {
        FAIL("%s is not built: run ./build.sh first", monolith);
        free(monolith);
        return;
    }
    for (int i = 0; i < (int)(sizeof(CLICKS) / sizeof(CLICKS[0])); i++) {
        Bounds o, c;
        int dark, ring;
        bool nearest;
        if (!Click1(monolith, dir, i, "", &o, &c, &dark, &ring, &nearest)) continue;
        bool same = o.n > 0 && c.n > 0 && abs(o.x0 - c.x0) <= 2 && abs(o.x1 - c.x1) <= 2 && abs(o.y0 - c.y0) <= 2 && abs(o.y1 - c.y1) <= 2;
        bool big = c.x1 - c.x0 >= 12 && c.y1 - c.y0 >= 12;
        bool sharp = ring > 0 && dark * 50 <= ring;
        printf("%s %-22s outline %d %d .. %d %d, cell %d %d .. %d %d; %d of %d ring pixels dark; paint_filter nearest: %s\n",
               same && big && sharp && nearest ? "ok  " : "FAIL", CLICKS[i].name, o.x0, o.y0, o.x1, o.y1, c.x0, c.y0, c.x1, c.y1, dark, ring,
               nearest ? "yes" : "no");
        if (!same) FAIL("%s: the painted cell is not the cell under the outline", CLICKS[i].name);
        if (!big) FAIL("%s: the painted cell is smaller than 12 px (the camera moved?)", CLICKS[i].name);
        if (!sharp) FAIL("%s: the painted cell is not sharp", CLICKS[i].name);
        if (!nearest) FAIL("%s: the first stroke on the grid wrote no paint_filter nearest", CLICKS[i].name);
    }

    // The control: Pixel look off before the first stroke: no key, the cell is soft (so the check sees it)
    Bounds o, c;
    int dark, ring;
    bool nearest;
    if (Click1(monolith, dir, 0, "pixel off\n", &o, &c, &dark, &ring, &nearest)) {
        bool soft = dark * 4 >= ring;
        printf("%s %-22s %d of %d ring pixels dark; paint_filter nearest: %s\n", soft && !nearest ? "ok  " : "FAIL", "control (pixel off)",
               dark, ring, nearest ? "yes" : "no");
        if (!soft || nearest) FAIL("control: with pixel off the cell should be filtered and the map without the key");
    }

    // A face of a face group: the target line says how many of the rectangle's cells it covers
    char *pkg = Format("%s/group", dir);
    mkdir(pkg, 0755);
    char *map = Format("%s/grid.map", pkg), *script = Format("%s/group.txt", pkg), *png = Format("%s/shot.png", pkg);
    char *log = Format("%s/log.txt", pkg);
    const char *args[] = { "--map", map, "--shot", png, "--view", "1", "--eye", "-0.6", "30.515", "30.52", "--look", "90", "0",
                           "--paint-script", script, NULL };
    if (WriteFile(map, PACKAGE) &&
        WriteFile(script, "tool pencil\nsize 1\nhover px 950 350\ndab px 950 350\nfaces 0 0 0 0 0 0\nhover px 950 350\n")) {
        int status = Run(monolith, log, args);
        char *out = ReadFile(log);
        const char *want = "white's atlas 256\xc3\x97" "256: 8\xc3\x97" "7 at 0 0 (8/m), face -x covers 6\xc3\x97" "7 cells of it";
        bool ok = status == 0 && out && strstr(out, want);
        printf("%s target line of a grouped face: \"%s\"\n", ok ? "ok  " : "FAIL", want);
        if (!ok) FAIL("the target line of a grouped face (exit %d, output %s)", status, log);
        free(out);
    }
    free(pkg);
    free(map);
    free(script);
    free(png);
    free(log);
    free(monolith);
}

// #endregion

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: paintgrid_test <repo root> <scratch folder>\n");
        return 2;
    }
    SetTraceLogLevel(LOG_WARNING);
    mkdir(argv[2], 0755);
    if (!PartA(argv[2])) fails++;
    PartB(argv[1], argv[2]);
    printf("paintgrid_test: %s (%d failed)\n", fails ? "FAILED" : "OK", fails);
    MS_ClearError();
    return fails ? 1 : 0;
}
