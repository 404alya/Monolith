// Benchmark report
#ifndef MONOLITH_BENCH_H
#define MONOLITH_BENCH_H

#include "common.h"
#include <r3d/r3d_profiler.h>

#define BENCH_WARMUP 60
#define BENCH_FRAMES 600     // timed frames per viewpoint (max); --bench-frames lowers it

extern int benchFrames;
extern bool benchStill;      // --bench-still: the camera doesn't spin
extern bool benchFrameReuse; // --frame-reuse: timed frames composite the cached scene image
extern bool benchProfile;    // per-pass GPU timings (R3D profiler), default on if compiled in

// Profiler tag of a timed bench frame (warmup frames use -1)
#define BENCH_TAG(view, frame) ((view) * BENCH_FRAMES + (frame))

// Accumulates finished R3D profiler frames of timed bench frames; call once per frame
// (results arrive a few frames late, so also once after the run with flush = true).
void BenchCollectProfile(bool flush);
// CPU times of a timed frame: loop start until just before the headless glFinish, and
// the R3D_End() call alone (command submission; the GPU runs asynchronously meanwhile)
void BenchAddCpuTime(int view, double frameSeconds, double r3dEndSeconds);

void WriteBenchReport(double times[][BENCH_FRAMES], const int *shadowUpdates, int shadowSize,
                      float shadowRange, bool shadowReuse, bool headless, const char *note, int argc, char **argv);

#endif
