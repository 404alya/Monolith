/* r3d_env.h -- Internal R3D environment module.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#ifndef R3D_MODULE_ENV_H
#define R3D_MODULE_ENV_H

#include <r3d/r3d_frustum.h>
#include <r3d/r3d_probe.h>
#include <raylib.h>
#include <stdint.h>
#include <glad.h>

#include "../common/r3d_list.h"

// ========================================
// HELPER MACROS
// ========================================

#define R3D_ENV_FOR_EACH_ILLUMINATION_PROBE(probe) \
    R3D_LIST_FOR_EACH(R3D_MOD_ENV.listProbeIllumination, R3D_Probe, probe)

#define R3D_ENV_FOR_EACH_REFLECTION_PROBE(probe) \
    R3D_LIST_FOR_EACH(R3D_MOD_ENV.listProbeReflection, R3D_Probe, probe)

#define R3D_ENV_FOR_EACH_PROBE_JOB(job) \
    R3D_LIST_FOR_EACH(R3D_MOD_ENV.listProbeJobs, r3d_env_probe_job_t, job)

// ========================================
// TYPES
// ========================================

typedef struct {
    R3D_Frustum   frustum[6];
    Matrix        view[6];
    Matrix        viewProj[6];
    Matrix        invView[6];
    Matrix        invProj;
    Vector3       position;
    R3D_ProbeType probeType;
    bool          interior;
    bool          shadows;
    int           layer;
} r3d_env_probe_job_t;

typedef struct {
    bool acquired;              // true from acquire until release; drives handle validity checks
    bool rendered;              // true once the layer's content has been captured at least once since (re)acquired
} r3d_env_cubemap_layer_state_t;

typedef struct {
    GLuint      framebuffer;
    GLuint      texture;        // GL_TEXTURE_CUBE_MAP_ARRAY handle, 0 until first expand
    r3d_list_t* freeList;       // list<int> of currently free layer indices
    r3d_list_t* layerStates;    // list<r3d_env_cubemap_layer_state_t> indexed by layer
    uint32_t    layerCount;     // total number of allocated layers (GL side)
    int         size;           // cubemap face resolution
    int         mipLevels;      // 1 if not mipmapped
} r3d_env_cubemap_array_t;

typedef struct {
    GLuint      framebuffer;
    GLuint      texture;        // GL_TEXTURE_CUBE_MAP handle
    GLuint      depthStencil;   // depth-stencil renderbuffer used while capturing the scene
    int         size;           // cubemap face resolution
    int         mipLevels;      // 1 if not mipmapped
} r3d_env_cubemap_t;

// ========================================
// MODULE STATE
// ========================================

extern struct r3d_env {
    r3d_env_cubemap_t irradianceCapture;
    r3d_env_cubemap_t prefilterCapture;
    r3d_env_cubemap_array_t irradiance;
    r3d_env_cubemap_array_t prefilter;
    r3d_list_t* listProbeIllumination;
    r3d_list_t* listProbeReflection;
    r3d_list_t* listProbeJobs;
} R3D_MOD_ENV;

// ========================================
// MODULE FUNCTIONS
// ========================================

/* Initialize module (called once during R3D_Init) */
bool r3d_env_init(void);

/* Deinitialize module (called once during R3D_Close) */
void r3d_env_quit(void);

/**/
void r3d_env_push_probe(const R3D_Probe* probe, bool updateProbe);

/**/
R3D_Probe* r3d_env_probe_get(R3D_ProbeType type, int probeIndex);

/**/
void r3d_env_probe_clear(void);

/* Returns if the the probe layer is valid */
bool r3d_env_probe_layer_is_valid(R3D_ProbeType type, int layer);

/* Bind probe capture framebuffer for the given face */
void r3d_env_probe_capture_bind_fbo(R3D_ProbeType type, int face);

/* Generate the mip chain of the probe capture */
void r3d_env_probe_capture_gen_mipmaps(R3D_ProbeType type);

/* Get probe capture cubemap texture ID */
GLuint r3d_env_probe_capture_get(R3D_ProbeType type);

/* Get probe capture cubemap texture size */
int r3d_env_probe_capture_size(R3D_ProbeType type);

/**/
const char* r3d_env_probe_type_name(R3D_ProbeType type);

/* Reserve a new irradiance map layer (returns -1 on failure) */
int r3d_env_irradiance_acquire_layer(void);

/* Release an irradiance map layer */
void r3d_env_irradiance_release_layer(int layer);

/* Bind irradiance framebuffer for the given layer and face */
void r3d_env_irradiance_bind_fbo(int layer, int face);

/**/
bool r3d_env_irradiance_layer_is_valid(int layer);

/* Get irradiance cubemap array texture ID */
GLuint r3d_env_irradiance_get(void);

/* Reserve a new prefilter map layer (returns -1 on failure) */
int r3d_env_prefilter_acquire_layer(void);

/* Release a prefilter map layer */
void r3d_env_prefilter_release_layer(int layer);

/* Bind prefilter framebuffer for the given layer, face and mip level */
void r3d_env_prefilter_bind_fbo(int layer, int face, int mipLevel);

/**/
bool r3d_env_prefilter_layer_is_valid(int layer);

/* Get prefiltered cubemap array texture ID */
GLuint r3d_env_prefilter_get(void);

// ========================================
// INLINE QUERIES
// ========================================

static inline bool r3d_env_has_illumination_probes(void)
{
    return !R3D_LIST_EMPTY(R3D_MOD_ENV.listProbeIllumination);
}

static inline bool r3d_env_has_reflection_probes(void)
{
    return !R3D_LIST_EMPTY(R3D_MOD_ENV.listProbeReflection);
}

static inline bool r3d_env_has_any_probes(void)
{
    return r3d_env_has_illumination_probes()
        || r3d_env_has_reflection_probes();
}

static inline bool r3d_env_has_any_probe_jobs(void)
{
    return !R3D_LIST_EMPTY(R3D_MOD_ENV.listProbeJobs);
}

#endif // R3D_MODULE_ENV_H
