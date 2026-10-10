#include "bench.h"
#include "spectator.h"
#include <time.h>
#ifndef _WIN32
#   include <sys/utsname.h>
#endif

// ======================================================================================
// #region Benchmark report
// ======================================================================================

int benchFrames = BENCH_FRAMES;
bool benchStill = false;
bool benchFrameReuse = false;
bool benchProfile = true;

#ifndef R3D_BUILD_TYPE_NAME
#   define R3D_BUILD_TYPE_NAME "unknown"   // set by build.sh
#endif

// Per-viewpoint sums over the timed frames
static double profPassMs[VIEWPOINT_COUNT][R3D_PROFILER_PASS_COUNT];
static double profTotalMs[VIEWPOINT_COUNT];
static int profFrames[VIEWPOINT_COUNT];
static double cpuSeconds[VIEWPOINT_COUNT];
static double r3dEndSeconds[VIEWPOINT_COUNT];

void BenchCollectProfile(bool flush)
{
    if (flush) R3D_FlushProfiler();
    R3D_ProfilerFrame pf;
    while (R3D_PopProfilerFrame(&pf)) {
        if (pf.tag < 0) continue;   // warmup
        int v = pf.tag / BENCH_FRAMES;
        if (v >= VIEWPOINT_COUNT) continue;
        for (int p = 0; p < R3D_PROFILER_PASS_COUNT; p++) profPassMs[v][p] += pf.passMs[p];
        profTotalMs[v] += pf.totalMs;
        profFrames[v]++;
    }
}

void BenchAddCpuTime(int view, double frameSeconds, double r3dEndSecs)
{
    cpuSeconds[view] += frameSeconds;
    r3dEndSeconds[view] += r3dEndSecs;
}

// Per-pass table: passes as rows, viewpoints + the average over all as columns.
// md = false prints a fixed-width table to stdout, true a Markdown table to f.
static void WriteProfileTable(FILE *f, bool md, const double *frameMs, double allFrameMs)
{
    int totalFrames = 0;
    for (int v = 0; v < VIEWPOINT_COUNT; v++) totalFrames += profFrames[v];

    // Header
    if (md) {
        fprintf(f, "| GPU ms per frame |");
        for (int v = 0; v < VIEWPOINT_COUNT; v++) fprintf(f, " %s |", VIEWPOINTS[v].name);
        fprintf(f, " **All** |\n|---|");
        for (int v = 0; v <= VIEWPOINT_COUNT; v++) fprintf(f, "---:|");
        fprintf(f, "\n");
    } else {
        fprintf(f, "%-18s", "GPU ms/frame");
        for (int v = 0; v < VIEWPOINT_COUNT; v++) fprintf(f, " %9.9s", VIEWPOINTS[v].name);
        fprintf(f, " %9s\n", "ALL");
    }

    // One row: per-view values (already per frame) and their frame-weighted mean
    #define ROW(label, bold, expr) do {                                                     \
        double all_ = 0.0;                                                                  \
        for (int v = 0; v < VIEWPOINT_COUNT; v++) all_ += (expr) * profFrames[v];           \
        all_ = totalFrames ? all_ / totalFrames : 0.0;                                      \
        if (md) {                                                                           \
            fprintf(f, bold ? "| **%s** |" : "| %s |", label);                              \
            for (int v = 0; v < VIEWPOINT_COUNT; v++) fprintf(f, " %.3f |", (expr));        \
            fprintf(f, " **%.3f** |\n", all_);                                              \
        } else {                                                                            \
            fprintf(f, "%-18s", label);                                                     \
            for (int v = 0; v < VIEWPOINT_COUNT; v++) fprintf(f, " %9.3f", (expr));         \
            fprintf(f, " %9.3f\n", all_);                                                   \
        }                                                                                   \
    } while (0)
    #define PER_FRAME(sum) (profFrames[v] ? (sum) / profFrames[v] : 0.0)

    for (int p = 0; p < R3D_PROFILER_PASS_COUNT; p++) {
        bool ran = false;   // only passes that ran at all (e.g. no SSGI/SSR/DoF rows)
        for (int v = 0; v < VIEWPOINT_COUNT; v++) ran |= profPassMs[v][p] > 0.0;
        if (ran) ROW(R3D_GetProfilerPassName((R3D_ProfilerPass)p), false, PER_FRAME(profPassMs[v][p]));
    }
    double sum[VIEWPOINT_COUNT];
    for (int v = 0; v < VIEWPOINT_COUNT; v++) {
        sum[v] = 0.0;
        for (int p = 0; p < R3D_PROFILER_PASS_COUNT; p++) sum[v] += profPassMs[v][p];
    }
    ROW("sum of passes", true, PER_FRAME(sum[v]));
    ROW("untimed rest", false, PER_FRAME(profTotalMs[v] - sum[v]));
    ROW("GPU total (R3D)", true, PER_FRAME(profTotalMs[v]));
    ROW("CPU until finish", false, cpuSeconds[v] * 1000.0 / benchFrames);
    ROW("CPU in R3D_End", false, r3dEndSeconds[v] * 1000.0 / benchFrames);
    #undef PER_FRAME
    #undef ROW

    // Frame time: the regular bench average (frame-weighted over the same timed frames)
    if (md) {
        fprintf(f, "| **Frame time** |");
        for (int v = 0; v < VIEWPOINT_COUNT; v++) fprintf(f, " %.3f |", frameMs[v]);
        fprintf(f, " **%.3f** |\n", allFrameMs);
    } else {
        fprintf(f, "%-18s", "frame time");
        for (int v = 0; v < VIEWPOINT_COUNT; v++) fprintf(f, " %9.3f", frameMs[v]);
        fprintf(f, " %9.3f\n", allFrameMs);
    }
}

// Only needed to put the GPU/driver into the benchmark report
#define GL_VENDOR   0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION  0x1F02
extern const unsigned char *glGetString(unsigned int name);

typedef struct {
    double avgMs, avgFps, lowFps, worstMs;
} BenchStats;

static BenchStats ComputeBenchStats(const double *times, int n)
{
    static double sorted[BENCH_FRAMES];
    double sum = 0.0;
    for (int i = 0; i < n; i++) { sum += times[i]; sorted[i] = times[i]; }

    // Partial selection sort: the slowest 1% of frames end up first
    int lowCount = n / 100 > 0 ? n / 100 : 1;
    for (int a = 0; a < lowCount; a++)
        for (int b = a + 1; b < n; b++)
            if (sorted[b] > sorted[a]) { double t = sorted[a]; sorted[a] = sorted[b]; sorted[b] = t; }
    double slow = 0.0;
    for (int a = 0; a < lowCount; a++) slow += sorted[a];
    slow /= lowCount;

    double avg = sum / n;
    return (BenchStats){ avg * 1000.0, 1.0 / avg, 1.0 / slow, sorted[0] * 1000.0 };
}

// Prints the results and writes them as Markdown to benchmarkDir/<timestamp>_shadow-<size>-<range>[_still].md
void WriteBenchReport(double times[][BENCH_FRAMES], const int *shadowUpdates, int shadowSize,
                             float shadowRange, bool shadowReuse, bool headless, const char *note, int argc, char **argv)
{
    BenchStats stats[VIEWPOINT_COUNT];
    double total = 0.0;
    for (int v = 0; v < VIEWPOINT_COUNT; v++) {
        stats[v] = ComputeBenchStats(times[v], benchFrames);
        for (int f = 0; f < benchFrames; f++) total += times[v][f];
    }
    int frames = VIEWPOINT_COUNT * benchFrames;
    double allMs = total / frames * 1000.0, allFps = frames / total;
    double viewMs[VIEWPOINT_COUNT];
    for (int v = 0; v < VIEWPOINT_COUNT; v++) viewMs[v] = stats[v].avgMs;
    bool haveProfile = benchProfile && R3D_IsProfilerAvailable();

    // stdout
    printf("\nbench: %dx%d, shadow %d px / %.0f m, %d frames per view, %s camera, R3D %s build\n",
           GetRenderWidth(), GetRenderHeight(), shadowSize, shadowRange, benchFrames,
           benchStill ? "still" : "spinning", R3D_BUILD_TYPE_NAME);
    if (benchFrameReuse) printf("frame reuse ON: unchanged frames composite the cached scene image (no R3D rendering)\n");
    printf("%-16s %9s %9s %9s %9s\n", "view", "avg ms", "avg fps", "1% low", "sh.upd");
    for (int v = 0; v < VIEWPOINT_COUNT; v++)
        printf("%-16s %9.3f %9.1f %9.1f %9d\n", VIEWPOINTS[v].name, stats[v].avgMs, stats[v].avgFps, stats[v].lowFps, shadowUpdates[v]);
    printf("%-16s %9.3f %9.1f\n", "ALL", allMs, allFps);
    if (haveProfile) {
        printf("\n");
        WriteProfileTable(stdout, false, viewMs, allMs);
        if (R3D_GetProfilerDroppedCount() > 0) printf("profiler: %d results dropped\n", R3D_GetProfilerDroppedCount());
    }

    // Markdown report
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char stamp[32], when[32], path[1024];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", lt);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", lt);
    if (snprintf(path, sizeof(path), "%s%s_shadow-%d-%.0f%s%s.md", benchmarkDir, stamp, shadowSize, shadowRange,
                 benchStill ? "_still" : "", benchFrameReuse ? "_reuse" : "") >= (int)sizeof(path)) {
        fprintf(stderr, "bench: report path too long: %s\n", benchmarkDir);
        return;
    }

    if (!DirectoryExists(benchmarkDir)) MakeDirectory(benchmarkDir);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }

    const char *vsyncEnv = getenv("__GL_SYNC_TO_VBLANK");
    const char *vblankEnv = getenv("vblank_mode");
#ifndef _WIN32
    struct utsname un;
    bool haveUname = uname(&un) == 0;
#endif
    R3D_Environment *env = R3D_GetEnvironment();

    fprintf(f, "# Benchmark %s\n\n", when);
    if (note && note[0]) fprintf(f, "> %s\n\n", note);

    fprintf(f, "## Setup\n\n");
    fprintf(f, "| | |\n|---|---|\n");
    fprintf(f, "| GPU | %s |\n", (const char *)glGetString(GL_RENDERER));
    fprintf(f, "| Driver | %s / %s |\n", (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_VERSION));
#ifdef _WIN32
    fprintf(f, "| System | Windows (x86_64, MinGW-w64 build) |\n");
#else
    if (haveUname) fprintf(f, "| System | %s %s (%s) |\n", un.sysname, un.release, un.machine);
#endif
    fprintf(f, "| Resolution | %d x %d |\n", GetRenderWidth(), GetRenderHeight());
    // Frame times from before 2026-10-10 were measured with an unoptimised R3D
    fprintf(f, "| R3D build | %s%s |\n", R3D_BUILD_TYPE_NAME, strcmp(R3D_BUILD_TYPE_NAME, "None") == 0
            ? " (no CMAKE_BUILD_TYPE, unoptimised: like the reports before 2026-10-10)" : "");
    fprintf(f, "| Sun shadow | %d px, range %.0f m, %s |\n", shadowSize, shadowRange,
            shadowReuse ? "re-rendered only when the view leaves the cached map" : "re-rendered every frame (`--shadow-always`)");
    fprintf(f, "| Effects | volFog %s, fog %s, SSAO %s, SSIL %s, bloom %s, exposure %.3g |\n",
            env->volumetricFog.enabled ? "on" : "off", env->fog.mode ? "on" : "off",
            env->ssao.enabled ? TextFormat("on (%d samples, radius %.2g)", env->ssao.sampleCount, env->ssao.radius) : "off",
            env->ssil.enabled ? TextFormat("on (%d samples, radius %.2g)", env->ssil.sampleCount, env->ssil.radius) : "off",
            env->bloom.mode ? TextFormat("on (intensity %.2g, levels %.2g)", env->bloom.intensity, env->bloom.levels) : "off",
            env->tonemap.exposure);
    fprintf(f, "| Vsync | app off; `__GL_SYNC_TO_VBLANK=%s`, `vblank_mode=%s` |\n",
            vsyncEnv ? vsyncEnv : "(unset)", vblankEnv ? vblankEnv : "(unset)");
    fprintf(f, "| Window | %s |\n", headless ? "headless: hidden window, offscreen target, glFinish per frame (no presentation)"
                                             : "visible (includes presentation)");
    fprintf(f, "| Method | per viewpoint: %d warmup frames, then %d timed frames %s |\n",
            BENCH_WARMUP, benchFrames, benchStill ? "with a still camera (`--bench-still`)" : "while the camera spins 360 deg");
    if (benchFrameReuse)
        fprintf(f, "| Frame reuse | on (`--frame-reuse`): unchanged frames only composite the cached scene image; "
                   "with a still camera the timed frames measure that, not R3D rendering |\n");
    fprintf(f, "| GPU profiler | %s |\n", haveProfile ? "on: per-pass GL_TIME_ELAPSED queries (R3D patch 5)"
                                       : R3D_IsProfilerAvailable() ? "off (`--no-profile`)" : "not compiled in");
    fprintf(f, "| Command |");
    for (int i = 0; i < argc; i++) fprintf(f, " `%s`", argv[i]);
    fprintf(f, " |\n\n");

    fprintf(f, "## Results\n\n");
    fprintf(f, "| View | Avg ms | Avg FPS | 1%% low FPS | Worst ms | Shadow updates |\n|---|---:|---:|---:|---:|---:|\n");
    int allUpdates = 0;
    for (int v = 0; v < VIEWPOINT_COUNT; v++) {
        fprintf(f, "| %s | %.3f | %.1f | %.1f | %.2f | %d / %d |\n", VIEWPOINTS[v].name,
                stats[v].avgMs, stats[v].avgFps, stats[v].lowFps, stats[v].worstMs, shadowUpdates[v], benchFrames);
        allUpdates += shadowUpdates[v];
    }
    fprintf(f, "| **All** | **%.3f** | **%.1f** | | | %d / %d |\n", allMs, allFps, allUpdates, frames);

    if (haveProfile) {
        fprintf(f, "\n## GPU passes\n\n");
        fprintf(f, "Average GPU time per frame of each R3D pass (passes that didn't run on a frame count as 0, "
                   "so e.g. shadow maps show their re-render cost spread over all frames). "
                   "*GPU total* spans `R3D_End()`; *untimed rest* is that minus the passes "
                   "(G-buffer clear, state changes, gaps). *CPU until finish* is the CPU time from the start of "
                   "the frame until the %s, *CPU in R3D_End* the part spent inside `R3D_End()` (command submission); *frame time* is the regular average above.\n\n",
                   headless ? "`glFinish` that waits for the GPU" : "end of `EndDrawing()` (includes the swap)");
        WriteProfileTable(f, true, viewMs, allMs);
        if (R3D_GetProfilerDroppedCount() > 0) fprintf(f, "\n%d profiler results were dropped.\n", R3D_GetProfilerDroppedCount());
    }
    fclose(f);

    printf("report: %s\n", path);
    fflush(stdout);
}

// #endregion
