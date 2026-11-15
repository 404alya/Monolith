// Benchmark report
#ifndef MONOLITH_BENCH_H
#define MONOLITH_BENCH_H

#include "common.h"
#include <r3d/r3d_profiler.h>

#define BENCH_WARMUP 60
#define BENCH_FRAMES 600     // timed frames per viewpoint by default; --bench-frames changes it
#define BENCH_FRAMES_LIMIT 1000000  // sanity cap of --bench-frames (the tables are heap arrays)

extern int benchFrames;
extern bool benchStill;      // --bench-still: the camera doesn't spin
extern bool benchFrameReuse; // --frame-reuse: timed frames composite the cached scene image
extern bool benchProfile;    // per-pass GPU timings (R3D profiler), default on if compiled in
extern const char *benchQuality;    // quality preset name of the run (settings.c), for the report
extern const char *benchViewsArg;   // --bench-views: 1-based viewpoints to visit, "1,3,4" (NULL = all)

// The viewpoints the bench visits, in order: indices into scene->viewpoints (BenchInit).
// Bench view v (tables, profiler tags, report columns) is viewpoint benchViewList[v].
extern int *benchViewList;
extern int benchViewCount;

// Profiler tag of a timed bench frame (warmup frames use -1)
#define BENCH_TAG(view, frame) ((view) * benchFrames + (frame))

// The view list (from benchViewsArg and the loaded scene) and the per-view profile tables
// (heap). Returns 0, 1 out of memory, 2 a bad --bench-views list (MS_LastError says why).
// BenchFree releases them.
unsigned char BenchInit(void);
void BenchFree(void);

// Accumulates finished R3D profiler frames of timed bench frames; call once per frame
// (results arrive a few frames late, so also once after the run with flush = true).
void BenchCollectProfile(bool flush);
// CPU times of a timed frame: loop start until just before the headless glFinish, and
// the R3D_End() call alone (command submission; the GPU runs asynchronously meanwhile)
void BenchAddCpuTime(int view, double frameSeconds, double r3dEndSeconds);

// times[view * benchFrames + frame]
void WriteBenchReport(const double *times, const int *shadowUpdates, int shadowSize,
                      float shadowRange, bool shadowReuse, bool headless, const char *note, int argc, char **argv);

#endif
