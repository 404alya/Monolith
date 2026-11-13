// harness.h -- test build of the examples: headless screenshots, no change to their source
//
// Not part of the examples: they are plain integration code. `./build.sh examples test` (and
// lsan, for the leak check; examples/nob.c) compiles each example's main file with
// `-include harness.h`, which turns the unchanged program into a test run:
//
//   <example> --shot <file> [the example's own arguments]
//
// It opens a hidden window that is never mapped (title "monolith-shot", so a window manager
// rule can route it), renders HARNESS_FRAMES frames into a texture, saves the last one to
// <file> and lets the program end normally (WindowShouldClose turns true), so its cleanup
// runs (the leak check needs that). Without --shot (or --bench) it refuses to run (exit 1): a
// test binary never opens a visible window. Frame time is a fixed 1/60 s, so an image never
// depends on the machine's speed. The example's own window title, flags, FPS cap and cursor
// calls are ignored.
//
// How: the program's raylib / R3D / scene calls that decide where a frame goes are macros to
// the Harness* functions below (main included, which becomes HarnessExampleMain), and every
// frame that would reach the window goes into the harness's texture instead: R3D_Begin /
// R3D_BeginEx / R3D_BeginPro / MS_Draw without a target get it. R3D itself always writes framebuffer 0 when
// it has no target, which a hidden window can't read back. With the capture shim in front
// (`-include monolith_capture.h -include harness.h`), its recorders are wrapped the same way.
//
// Bench mode (docs/painting.md 14): the frame time of a program that draws a scene with MS_Draw
// (the exported viewer.c, examples/01_minimal), measured like `monolith --bench --headless`:
//
//   <example> --bench <report.md> [--bench-frames N] [--note "what changed"] [the example's arguments]
//
// The same hidden window (title "monolith-bench"), always HARNESS_BENCH_W x HARNESS_BENCH_H (the
// window size of `monolith --bench --headless`, whatever size the program asks for; its
// MS_ApplyRenderResolution calls get that size too). From the first MS_Draw on, for each viewpoint
// of that scene, the camera passed to MS_Draw is replaced by monolith's spectator camera at the
// viewpoint (eye, yaw and pitch towards its target, fovy 70) spinning as `monolith --bench` spins
// it: HARNESS_BENCH_WARMUP frames still, then N timed frames (default 600, 10 .. 1000000) with the
// yaw turning 360 degrees. glFinish after every frame; a frame's time is from the end of the
// previous frame's glFinish to the end of its own. The report (Markdown, at <report.md>) has the
// Setup table of `monolith --bench` (GPU, driver, CPU, system, resolution, map, shadow, effects,
// vsync, window, method, command, the note) and per viewpoint and overall its statistics (average
// ms and FPS, 1 % low FPS, worst ms, shadow updates) plus the median, p95 and p99 frame time; the
// same table goes to stdout. Not in it: the per-pass GPU table (the harness leaves R3D's profiler
// off, so a run costs the same with an R3D built with or without it). The program's own work per
// frame (input, its 2D text) is part of the frame, as monolith's loop is. Run it like any
// benchmark: one at a time, __GL_SYNC_TO_VBLANK=0, a note (the benchmarking skill); compare its
// numbers only with the same harness's. build/tools/bench-export-ab runs A/B pairs of two exports.
#ifndef MONOLITH_TEST_HARNESS_H
#define MONOLITH_TEST_HARNESS_H

#include <raylib.h>
#include <r3d/r3d.h>
#include "monolith_scene.h"     // (declarations only: programs without the runtime never call it)
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#   include <sys/utsname.h>
#endif

#define HARNESS_FRAMES 30

// Bench mode: as monolith's --bench --headless (src/bench.h, src/main.c)
#define HARNESS_BENCH_W 1920
#define HARNESS_BENCH_H 1006
#define HARNESS_BENCH_WARMUP 60
#define HARNESS_BENCH_FRAMES 600
#define HARNESS_BENCH_LIMIT 1000000
#define HARNESS_BENCH_NO_DRAW 600       // frames without an MS_Draw before the bench gives up

static struct {
    const char *path;       // --shot <file>
    RenderTexture target;   // where the frames go (made right after InitWindow)
    int frame;
    bool done;              // the shot is saved: WindowShouldClose returns true
} harness;

// Bench mode state; the tables are heap arrays sized by the scene's viewpoints
static struct {
    const char *report;     // --bench <report.md> (NULL: no bench)
    const char *note;       // --note
    int frames;             // timed frames per viewpoint
    int argc;               // the whole command line, for the report
    char **argv;
    MS_Scene *scene;        // the scene of the first MS_Draw (the one that is measured)
    int views;
    double *times;          // [view * frames + frame], seconds
    int *shadowUpdates;     // per view: timed frames whose MS_Draw re-rendered the sun's shadow
    int view, frame;        // frame < 0: warmup
    double last;            // GetTime() at the end of the previous frame
    bool failed;
} harnessBench = { .frames = HARNESS_BENCH_FRAMES };

// The calls below use the real functions (or, with the capture shim, its recorders): the
// macros that route the program's calls here are only defined at the end of this file
static inline void HarnessSetConfigFlags(unsigned int flags)
{
    SetConfigFlags((flags & ~(unsigned int)(FLAG_WINDOW_RESIZABLE | FLAG_FULLSCREEN_MODE | FLAG_VSYNC_HINT)) | FLAG_WINDOW_HIDDEN);
}

static inline void HarnessInitWindow(int width, int height, const char *title)
{
    (void)title;
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    if (harnessBench.report) InitWindow(HARNESS_BENCH_W, HARNESS_BENCH_H, "monolith-bench");
    else InitWindow(width, height, "monolith-shot");
    harness.target = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
}

static inline void HarnessCloseWindow(void)
{
    if (harness.target.id) UnloadRenderTexture(harness.target);
    harness.target = (RenderTexture){ 0 };
    CloseWindow();
}

// #region Bench mode

// Only needed for glFinish(). Declared by hand rather than via <GL/gl.h> / <windows.h> (which
// clash with raylib); on Windows x64 there is a single calling convention (as src/main.c).
extern void glFinish(void);
#define HARNESS_GL_VENDOR   0x1F00
#define HARNESS_GL_RENDERER 0x1F01
#define HARNESS_GL_VERSION  0x1F02
extern const unsigned char *glGetString(unsigned int name);

// The camera of timed frame `frame` (< 0: warmup) at viewpoint `view`: monolith's spectator at the
// viewpoint's eye (src/spectator.c: LookAngles, SpectatorCamera), its yaw turned as src/main.c's
// bench loop turns it
static inline Camera3D HarnessBenchCamera(const MS_Scene *scene, int view, int frame, int frames)
{
    Vector3 eye = scene->viewpoints[view].eye, target = scene->viewpoints[view].target;
    Vector3 d = { target.x - eye.x, target.y - eye.y, target.z - eye.z };
    float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len > 0.0f) { d.x /= len; d.y /= len; d.z /= len; }
    float yaw = atan2f(d.x, d.z);
    float pitch = asinf(d.y < -1.0f ? -1.0f : d.y > 1.0f ? 1.0f : d.y);
    yaw += 2.0f * PI * (float)(frame > 0 ? frame : 0) / frames;
    Vector3 dir = { cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw) };
    return (Camera3D){
        .position = eye,
        .target = { eye.x + dir.x, eye.y + dir.y, eye.z + dir.z },
        .up = { 0, 1, 0 },
        .fovy = 70.0f,
        .projection = CAMERA_PERSPECTIVE,
    };
}

// The CPU's model name, as src/bench.c's CpuName: Linux the first "model name" of /proc/cpuinfo,
// Windows PROCESSOR_IDENTIFIER. False when unknown.
static inline bool HarnessCpuName(char *out, size_t size)
{
#ifdef _WIN32
    const char *id = getenv("PROCESSOR_IDENTIFIER");
    if (!id || !id[0]) return false;
    snprintf(out, size, "%s", id);
    return true;
#else
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "model name", 10) != 0) continue;
        const char *v = strchr(line, ':');
        if (!v) continue;
        v++;
        while (*v == ' ' || *v == '\t') v++;
        snprintf(out, size, "%s", v);
        out[strcspn(out, "\r\n")] = 0;
        found = out[0] != 0;
    }
    fclose(f);
    return found;
#endif
}

// Statistics of one viewpoint's frame times (or all of them): src/bench.c's ComputeBenchStats
// (average, 1 % low = the average of the slowest 1 % of frames, worst) plus nearest-rank
// percentiles of the sorted times
typedef struct {
    double avgMs, avgFps, lowFps, worstMs, medianMs, p95Ms, p99Ms;
} HarnessBenchStats;

static inline int HarnessCompareTimes(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static inline HarnessBenchStats HarnessComputeStats(const double *times, int n)
{
    HarnessBenchStats st = { 0 };
    double *sorted = malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
    if (!sorted || n <= 0) { free(sorted); return st; }
    double sum = 0.0;
    for (int i = 0; i < n; i++) { sum += times[i]; sorted[i] = times[i]; }
    qsort(sorted, (size_t)n, sizeof(double), HarnessCompareTimes);      // ascending
    int lowCount = n / 100 > 0 ? n / 100 : 1;
    double slow = 0.0;
    for (int a = 0; a < lowCount; a++) slow += sorted[n - 1 - a];
    slow /= lowCount;
    double avg = sum / n;
    int p95 = (int)ceil(0.95 * n) - 1, p99 = (int)ceil(0.99 * n) - 1;
    st.avgMs = avg * 1000.0;
    st.avgFps = 1.0 / avg;
    st.lowFps = 1.0 / slow;
    st.worstMs = sorted[n - 1] * 1000.0;
    st.medianMs = (n % 2 ? sorted[n / 2] : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2])) * 1000.0;
    st.p95Ms = sorted[p95 < 0 ? 0 : p95] * 1000.0;
    st.p99Ms = sorted[p99 < 0 ? 0 : p99] * 1000.0;
    free(sorted);
    return st;
}

// Prints the results and writes the Markdown report (the layout of src/bench.c's WriteBenchReport,
// without the GPU pass table); false when it can't be written
static inline bool HarnessWriteBenchReport(void)
{
    const MS_Scene *scene = harnessBench.scene;
    int views = harnessBench.views, n = harnessBench.frames;
    HarnessBenchStats *stats = calloc((size_t)views + 1, sizeof(HarnessBenchStats));
    if (!stats) { fprintf(stderr, "bench: out of memory\n"); return false; }
    for (int v = 0; v < views; v++) stats[v] = HarnessComputeStats(harnessBench.times + (size_t)v * n, n);
    stats[views] = HarnessComputeStats(harnessBench.times, views * n);     // all frames together
    int rw = 0, rh = 0;
    R3D_GetResolution(&rw, &rh);
    int shadowSize = scene->sun.shadowSize;
    float shadowRange = scene->sun.light.range;

    // stdout (as monolith's)
    printf("\nbench: %dx%d (R3D %dx%d), shadow %d px / %.0f m, %d frames per view, spinning camera, harness\n",
           harness.target.texture.width, harness.target.texture.height, rw, rh, shadowSize, shadowRange, n);
    printf("%-16s %9s %9s %9s %9s %9s %9s %9s\n", "view", "avg ms", "avg fps", "1% low", "median", "p95", "p99", "sh.upd");
    for (int v = 0; v < views; v++)
        printf("%-16s %9.3f %9.1f %9.1f %9.3f %9.3f %9.3f %9d\n", scene->viewpoints[v].name, stats[v].avgMs,
               stats[v].avgFps, stats[v].lowFps, stats[v].medianMs, stats[v].p95Ms, stats[v].p99Ms,
               harnessBench.shadowUpdates[v]);
    printf("%-16s %9.3f %9.1f %9.1f %9.3f %9.3f %9.3f\n", "ALL", stats[views].avgMs, stats[views].avgFps,
           stats[views].lowFps, stats[views].medianMs, stats[views].p95Ms, stats[views].p99Ms);

    FILE *f = fopen(harnessBench.report, "w");
    if (!f) { perror(harnessBench.report); free(stats); return false; }
    time_t now = time(NULL);
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
    const char *vsyncEnv = getenv("__GL_SYNC_TO_VBLANK"), *vblankEnv = getenv("vblank_mode");
    R3D_Environment *env = R3D_GetEnvironment();

    fprintf(f, "# Benchmark %s\n\n", when);
    if (harnessBench.note && harnessBench.note[0]) fprintf(f, "> %s\n\n", harnessBench.note);
    fprintf(f, "## Setup\n\n");
    fprintf(f, "| | |\n|---|---|\n");
    fprintf(f, "| GPU | %s |\n", (const char *)glGetString(HARNESS_GL_RENDERER));
    fprintf(f, "| Driver | %s / %s |\n", (const char *)glGetString(HARNESS_GL_VENDOR), (const char *)glGetString(HARNESS_GL_VERSION));
    char cpu[256];
    if (HarnessCpuName(cpu, sizeof(cpu))) fprintf(f, "| CPU | %s |\n", cpu);
#ifdef _WIN32
    fprintf(f, "| System | Windows (x86_64, MinGW-w64 build) |\n");
#else
    struct utsname un;
    if (uname(&un) == 0) fprintf(f, "| System | %s %s (%s) |\n", un.sysname, un.release, un.machine);
#endif
    fprintf(f, "| Resolution | %d x %d (R3D %d x %d) |\n", harness.target.texture.width, harness.target.texture.height, rw, rh);
    fprintf(f, "| Map | `%s` (format %d, %d boxes, %d materials, %d lights) |\n", scene->path, scene->formatVersion,
            scene->boxCount, scene->materialCount, scene->lightCount);
    fprintf(f, "| Program | `%s`: the scene runtime as this program builds it (tests/examples/harness.h bench mode) |\n",
            harnessBench.argv[0]);
    fprintf(f, "| Sun shadow | %d px, range %.0f m |\n", shadowSize, shadowRange);
    fprintf(f, "| Effects | volFog %s, fog %s, SSAO %s, SSIL %s, bloom %s, exposure %.3g |\n",
            env->volumetricFog.enabled ? "on" : "off", env->fog.mode ? "on" : "off",
            env->ssao.enabled ? TextFormat("on (%d samples, radius %.2g)", env->ssao.sampleCount, env->ssao.radius) : "off",
            env->ssil.enabled ? TextFormat("on (%d samples, radius %.2g)", env->ssil.sampleCount, env->ssil.radius) : "off",
            env->bloom.mode ? TextFormat("on (intensity %.2g, levels %.2g)", env->bloom.intensity, env->bloom.levels) : "off",
            env->tonemap.exposure);
    fprintf(f, "| Vsync | app off; `__GL_SYNC_TO_VBLANK=%s`, `vblank_mode=%s` |\n",
            vsyncEnv ? vsyncEnv : "(unset)", vblankEnv ? vblankEnv : "(unset)");
    fprintf(f, "| Window | headless: hidden window, offscreen target, glFinish per frame (no presentation) |\n");
    fprintf(f, "| Method | per viewpoint: %d warmup frames, then %d timed frames while the camera spins 360 deg "
               "(the camera passed to MS_Draw is replaced) |\n", HARNESS_BENCH_WARMUP, n);
    fprintf(f, "| GPU profiler | off (the harness doesn't enable it) |\n");
    fprintf(f, "| Command |");
    for (int i = 0; i < harnessBench.argc; i++) fprintf(f, " `%s`", harnessBench.argv[i]);
    fprintf(f, " |\n\n");

    fprintf(f, "## Results\n\n");
    fprintf(f, "| View | Avg ms | Avg FPS | 1%% low FPS | Worst ms | Median ms | p95 ms | p99 ms | Shadow updates |\n"
               "|---|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    int allUpdates = 0;
    for (int v = 0; v < views; v++) {
        fprintf(f, "| %s | %.3f | %.1f | %.1f | %.2f | %.3f | %.3f | %.3f | %d / %d |\n", scene->viewpoints[v].name,
                stats[v].avgMs, stats[v].avgFps, stats[v].lowFps, stats[v].worstMs, stats[v].medianMs, stats[v].p95Ms,
                stats[v].p99Ms, harnessBench.shadowUpdates[v], n);
        allUpdates += harnessBench.shadowUpdates[v];
    }
    fprintf(f, "| **All** | **%.3f** | **%.1f** | %.1f | %.2f | %.3f | %.3f | %.3f | %d / %d |\n", stats[views].avgMs,
            stats[views].avgFps, stats[views].lowFps, stats[views].worstMs, stats[views].medianMs, stats[views].p95Ms,
            stats[views].p99Ms, allUpdates, views * n);
    bool ok = fclose(f) == 0;
    free(stats);
    printf("report: %s\n", harnessBench.report);
    fflush(stdout);
    return ok;
}

// After every frame: wait for the GPU, time the frame, step the viewpoint; the report after the last
static inline void HarnessBenchEndFrame(void)
{
    glFinish();
    double now = GetTime();
    if (!harnessBench.scene) {      // nothing measured yet: the program hasn't drawn a scene
        if (++harness.frame >= HARNESS_BENCH_NO_DRAW) {
            fprintf(stderr, "bench: no MS_Draw in %d frames (bench mode measures a scene drawn with MS_Draw)\n", harness.frame);
            harnessBench.failed = harness.done = true;
        }
        return;
    }
    int n = harnessBench.frames;
    if (harnessBench.frame >= 0) harnessBench.times[(size_t)harnessBench.view * n + harnessBench.frame] = now - harnessBench.last;
    harnessBench.last = now;
    if (++harnessBench.frame < n) return;
    harnessBench.frame = -HARNESS_BENCH_WARMUP;
    if (++harnessBench.view < harnessBench.views) return;
    if (!HarnessWriteBenchReport()) harnessBench.failed = true;
    harness.done = true;
}

// #endregion

static inline void HarnessEndDrawing(void)
{
    EndDrawing();
    if (harnessBench.report) {
        if (!harness.done) HarnessBenchEndFrame();
        return;
    }
    if (harness.done || ++harness.frame < HARNESS_FRAMES) return;
    Image img = LoadImageFromTexture(harness.target.texture);
    ImageFlipVertical(&img);                                // GL textures are bottom-up
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);     // no alpha, like monolith --shot
    bool ok = ExportImage(img, harness.path);
    UnloadImage(img);
    printf("%s %s\n", ok ? "saved" : "failed to save", harness.path);
    fflush(stdout);
    harness.done = true;
}

static inline bool HarnessWindowShouldClose(void) { return harness.done || WindowShouldClose(); }
static inline float HarnessGetFrameTime(void) { return 1.0f / 60.0f; }
static inline void HarnessNoInt(int v) { (void)v; }
static inline void HarnessNoArgs(void) { }

static inline void HarnessBeginPro(R3D_View view)
{
    if (view.target.id == 0) view.target = harness.target;
    R3D_BeginPro(view);
}

static inline void HarnessBeginEx(R3D_Camera camera)
{
    HarnessBeginPro((R3D_View){ .camera = camera, .target = harness.target });
}

static inline void HarnessBegin(Camera3D camera)
{
    HarnessBeginPro((R3D_View){ .camera = R3D_CameraFromRL(camera), .target = harness.target });
}

static inline bool HarnessDraw(MS_Scene *scene, Camera3D camera, RenderTexture target)
{
    if (!harnessBench.report || harness.done) return MS_Draw(scene, camera, target.id ? target : harness.target);

    // Bench mode: the first scene drawn is the one measured, with the bench's camera
    if (!harnessBench.scene) {
        int views = scene->viewpointCount;
        harnessBench.times = calloc((size_t)(views > 0 ? views : 1) * (size_t)harnessBench.frames, sizeof(double));
        harnessBench.shadowUpdates = calloc((size_t)(views > 0 ? views : 1), sizeof(int));
        if (views <= 0 || !harnessBench.times || !harnessBench.shadowUpdates) {
            fprintf(stderr, "bench: %s\n", views <= 0 ? "the scene has no viewpoints" : "out of memory");
            harnessBench.failed = harness.done = true;
            return MS_Draw(scene, camera, target.id ? target : harness.target);
        }
        harnessBench.scene = scene;
        harnessBench.views = views;
        harnessBench.view = 0;
        harnessBench.frame = -HARNESS_BENCH_WARMUP;
    }
    if (scene == harnessBench.scene)
        camera = HarnessBenchCamera(scene, harnessBench.view, harnessBench.frame, harnessBench.frames);
    bool shadow = MS_Draw(scene, camera, target.id ? target : harness.target);
    if (scene == harnessBench.scene && shadow && harnessBench.frame >= 0) harnessBench.shadowUpdates[harnessBench.view]++;
    return shadow;
}

// Bench mode: the render resolution follows the bench window, whatever size the program asks for
static inline void HarnessApplyRenderResolution(MS_Scene *scene, int width, int height)
{
    if (harnessBench.report) { width = GetScreenWidth(); height = GetScreenHeight(); }
    MS_ApplyRenderResolution(scene, width, height);
}

// The example's main, renamed (HARNESS_MAIN_NO_ARGS: it is `int main(void)`)
#ifdef HARNESS_MAIN_NO_ARGS
int HarnessExampleMain(void);
#define HARNESS_CALL_MAIN(argc, argv) ((void)(argc), (void)(argv), HarnessExampleMain())
#else
int HarnessExampleMain(int argc, char **argv);
#define HARNESS_CALL_MAIN(argc, argv) HarnessExampleMain((argc), (argv))
#endif

// --shot <file>, --bench <report.md>, --bench-frames <n> and --note <text> are the harness's;
// every other argument goes to the example
int main(int argc, char **argv)
{
    char **args = malloc(sizeof(char *) * (size_t)(argc + 1));
    if (!args) return 1;
    int n = 0;
    bool framesSet = false;
    for (int i = 0; i < argc; i++) {
        if (i > 0 && strcmp(argv[i], "--shot") == 0 && i + 1 < argc) harness.path = argv[++i];
        else if (i > 0 && strcmp(argv[i], "--bench") == 0 && i + 1 < argc) harnessBench.report = argv[++i];
        else if (i > 0 && strcmp(argv[i], "--bench-frames") == 0 && i + 1 < argc) {
            long v = strtol(argv[++i], NULL, 10);
            harnessBench.frames = v < 10 ? 10 : v > HARNESS_BENCH_LIMIT ? HARNESS_BENCH_LIMIT : (int)v;
            framesSet = true;
        }
        else if (i > 0 && strcmp(argv[i], "--note") == 0 && i + 1 < argc) harnessBench.note = argv[++i];
        else args[n++] = argv[i];
    }
    args[n] = NULL;
    const char *refuse = NULL;
    if (harness.path && harnessBench.report) refuse = "--shot and --bench can't be combined";
    else if (!harness.path && !harnessBench.report)
        refuse = "test build: needs --shot <file> or --bench <report.md> (it never opens a visible window; the plain "
                 "example is examples/build/<platform>/)";
    else if (!harnessBench.report && (framesSet || harnessBench.note)) refuse = "--bench-frames and --note need --bench";
    if (refuse) {
        fprintf(stderr, "%s: %s\n", argv[0], refuse);
        free(args);
        return 1;
    }
    harnessBench.argc = argc;
    harnessBench.argv = argv;
    int status = HARNESS_CALL_MAIN(n, args);
    free(args);
    free(harnessBench.times);
    free(harnessBench.shadowUpdates);
    return harnessBench.failed && status == 0 ? 1 : status;
}

#define main                HarnessExampleMain
#define SetConfigFlags      HarnessSetConfigFlags
#define InitWindow          HarnessInitWindow
#define CloseWindow         HarnessCloseWindow
#define EndDrawing          HarnessEndDrawing
#define WindowShouldClose   HarnessWindowShouldClose
#define GetFrameTime        HarnessGetFrameTime
#define SetTargetFPS        HarnessNoInt
#define DisableCursor       HarnessNoArgs
#define EnableCursor        HarnessNoArgs
#undef R3D_Begin
#undef R3D_BeginEx
#undef R3D_BeginPro
#define R3D_Begin           HarnessBegin
#define R3D_BeginEx         HarnessBeginEx
#define R3D_BeginPro        HarnessBeginPro
#define MS_Draw             HarnessDraw
#define MS_ApplyRenderResolution HarnessApplyRenderResolution

#endif
