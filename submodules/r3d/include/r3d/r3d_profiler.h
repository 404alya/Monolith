/* r3d_profiler.h -- R3D GPU pass profiler (local monolith patch, see patches.md).
 *
 * Per-pass GPU timings of R3D_End(), measured with GL_TIME_ELAPSED queries plus two
 * GL_TIMESTAMP queries for the whole R3D_End() span. Queries live in a ring of frames
 * and are read back a few frames later, only once available, so the CPU never waits
 * for the GPU (except when the ring is full, which the driver's frame queue prevents).
 *
 * The code is compiled in only when R3D is built with -DR3D_GPU_PROFILER=ON. Without it
 * these functions still exist: R3D_IsProfilerAvailable() returns false and the rest
 * are no-ops. Even when compiled in, the profiler is off until R3D_SetProfilerEnabled
 * (true); while off, each pass costs one predictable branch and issues no GL calls.
 * Rendering itself is never changed by the profiler.
 */

#ifndef R3D_PROFILER_H
#define R3D_PROFILER_H

#include "./r3d_platform.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The timed passes, in R3D_End() order.
 */
typedef enum R3D_ProfilerPass {
    R3D_PROFILER_SHADOWS,             ///< Shadow map rendering (only on frames that update a map)
    R3D_PROFILER_SHADOW_MINMAX,       ///< Directional shadow min/max tile reduction (local patch 3)
    R3D_PROFILER_PROBES,              ///< Environment probe capture
    R3D_PROFILER_GEOMETRY,            ///< G-buffer fill (opaque geometry, surface shaders)
    R3D_PROFILER_DECALS,              ///< Deferred decals
    R3D_PROFILER_LIGHTS,              ///< Deferred direct lighting (incl. shadow PCF)
    R3D_PROFILER_PYRAMID,             ///< Depth pyramid + normal/radiance downsamples
    R3D_PROFILER_SSAO,                ///< SSAO (incl. its blur)
    R3D_PROFILER_SSIL,                ///< SSIL (incl. its blur/history)
    R3D_PROFILER_SSGI,                ///< SSGI
    R3D_PROFILER_SSR,                 ///< SSR
    R3D_PROFILER_AMBIENT,             ///< Deferred ambient / IBL / indirect
    R3D_PROFILER_COMPOSE,             ///< Deferred compose into the scene target
    R3D_PROFILER_BACKGROUND,          ///< Sky / background
    R3D_PROFILER_FOG,                 ///< Distance fog
    R3D_PROFILER_VFOG_TRANSMITTANCE,  ///< Volumetric fog: transmittance (full res)
    R3D_PROFILER_VFOG_RADIANCE,       ///< Volumetric fog: radiance march per light (half res)
    R3D_PROFILER_VFOG_COMPOSE,        ///< Volumetric fog: bilateral upsample + compose
    R3D_PROFILER_FORWARD,             ///< Forward (unlit / blended) objects
    R3D_PROFILER_DOF,                 ///< Depth of field
    R3D_PROFILER_BLOOM,               ///< Bloom
    R3D_PROFILER_AUTO_EXPOSURE,       ///< Auto exposure
    R3D_PROFILER_OUTPUT,              ///< Tonemap / color adjustments (post output)
    R3D_PROFILER_AA,                  ///< FXAA or SMAA
    R3D_PROFILER_BLIT,                ///< Final blit / visualisation to the screen or target
    R3D_PROFILER_PASS_COUNT
} R3D_ProfilerPass;

/**
 * @brief GPU timings of one R3D_End() call.
 *
 * passMs[i] is 0 for passes that didn't run that frame (ran[i] == false), so averaging
 * passMs over frames gives the average cost per frame. totalMs is the GPU time between
 * the start and the end of R3D_End(); totalMs - sum(passMs) is the untimed rest
 * (G-buffer clear, screen shaders, gaps between passes).
 */
typedef struct R3D_ProfilerFrame {
    uint64_t id;                                ///< Sequence number (counts profiled frames)
    int tag;                                    ///< Value of R3D_SetProfilerTag() at R3D_End()
    float totalMs;                              ///< Whole R3D_End() span on the GPU
    float passMs[R3D_PROFILER_PASS_COUNT];      ///< Per pass
    bool ran[R3D_PROFILER_PASS_COUNT];          ///< Pass ran this frame
} R3D_ProfilerFrame;

/** @brief True if R3D was built with R3D_GPU_PROFILER. */
R3DAPI bool R3D_IsProfilerAvailable(void);

/**
 * @brief Turns timing on or off (default off). Turning it off deletes the GL queries
 * and drops unread results; call it with false before R3D_Close() to free them.
 */
R3DAPI void R3D_SetProfilerEnabled(bool enabled);

/** @brief True while the profiler records. */
R3DAPI bool R3D_IsProfilerEnabled(void);

/** @brief Tag stored with the frames recorded from now on (e.g. viewpoint/frame index, -1 to ignore). */
R3DAPI void R3D_SetProfilerTag(int tag);

/**
 * @brief Takes the oldest finished frame from the result queue (oldest first).
 * Results arrive 1-3 frames after the R3D_End() that recorded them.
 * @return false if no finished frame is waiting.
 */
R3DAPI bool R3D_PopProfilerFrame(R3D_ProfilerFrame* frame);

/**
 * @brief Waits for all recorded frames and queues their results (blocks on the GPU).
 * Use once at the end of a measurement, not per frame.
 */
R3DAPI void R3D_FlushProfiler(void);

/** @brief Number of results dropped because the queue was full (not popped in time). */
R3DAPI int R3D_GetProfilerDroppedCount(void);

/** @brief Short display name of a pass, e.g. "vfog radiance". */
R3DAPI const char* R3D_GetProfilerPassName(R3D_ProfilerPass pass);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // R3D_PROFILER_H
