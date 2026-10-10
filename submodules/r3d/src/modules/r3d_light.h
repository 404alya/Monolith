/* r3d_light.h -- Internal R3D light module.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#ifndef R3D_MODULE_LIGHT_H
#define R3D_MODULE_LIGHT_H

#include <r3d/r3d_lighting.h>
#include <r3d/r3d_frustum.h>
#include <r3d/r3d_camera.h>
#include <r3d_config.h>
#include <raylib.h>
#include <glad.h>

#include "../common/r3d_list.h"
#include "../common/r3d_math.h"

// ========================================
// HELPER MACROS
// ========================================

#define R3D_LIGHT_FOR_EACH_VISIBLE(light) \
    R3D_LIST_FOR_EACH(R3D_MOD_LIGHT.listLightData, r3d_light_data_t, light)

#define R3D_LIGHT_FOR_EACH_SHADOW_JOB(job) \
    R3D_LIST_FOR_EACH(R3D_MOD_LIGHT.listShadowJobs, r3d_light_shadow_job_t, job)

// ========================================
// TYPES
// ========================================

typedef struct {
    Vector3 center;
    float   radius;
} r3d_light_volume_t;

typedef struct {
    r3d_light_volume_t volume;  // Light volume (sphere) (spot/omni)
    Vector2 minNdc, maxNdc;     // Light area to the screen in NDC
    Matrix viewProj;            // Used for shadow projection (dir/spot)
    Vector3 color;
    Vector3 position;           // Light position (spot/omni)
    Vector3 direction;          // Light direction (dir/spot)
    float energy;
    float specular;
    float range;                // Maximum distance (spot/omni)
    float falloff;              // Distance falloff factor (spot/omni)
    float innerCutOff;          // Spot light inner cutoff angle
    float outerCutOff;          // Spot light outer cutoff angle
    float fogEnergy;            // Volumetric fog energy multiplier
    float shadowSoftness;       // Softness factor for penumbra
    float shadowOpacity;        // Shadow opacity factor
    float shadowDepthBias;      // Constant depth bias
    float shadowSlopeBias;      // Slope-scaled depth bias
    float shadowFar;            // Far plane for shadow projection
    int shadowLayer;            // Shadow map layer index, -1 if no shadow
    R3D_LightType type;
} r3d_light_data_t;

typedef struct {
    R3D_Frustum   frustum;
    Matrix        viewProj;
    Vector3       position;
    float         far;
    R3D_Layer     cullMask;
    R3D_LightType type;
    int           shadowLayer;
    int           layerFace;
} r3d_light_shadow_job_t;

typedef struct {
    Matrix viewProj;            // stored for projection (dir/spot)
    float  far;                 // stored for projection (omni)
    bool   acquired;            // true from acquire until release; drives handle validity checks
    bool   rendered;            // true once shadow content has been rendered at least once since (re)acquired
} r3d_light_shadow_cache_t;

typedef struct {
    GLuint      framebuffer;
    GLuint      texture;        // GL_TEXTURE_2D_ARRAY or GL_TEXTURE_CUBE_MAP_ARRAY handle, 0 until first expand
    GLenum      target;         // GL_TEXTURE_2D_ARRAY (dir/spot) or GL_TEXTURE_CUBE_MAP_ARRAY (omni)
    r3d_list_t* freeList;       // list<int> of currently free layer indices
    r3d_list_t* cache;          // list<r3d_light_shadow_cache_t> indexed by layer
    uint32_t    layerCount;     // total number of allocated layers (GL side)
    int         size;           // shadow map resolution
    int         growth;         // number of layers added per expand
    // Min/max depth per R3D_SHADOW_MINMAX_TILE^2 texels, one layer per shadow layer.
    // Only for directional lights (local patch, see patches.md).
    bool        hasMinMax;
    bool        minMaxStale;    // reallocated: every layer must be reduced again
    GLuint      minMaxFramebuffer;
    GLuint      minMaxTexture;  // GL_TEXTURE_2D_ARRAY, GL_RG32F
    GLuint      minMaxMidTexture;   // GL_TEXTURE_2D, GL_RG32F: 8x8 texel step of the reduction
} r3d_light_shadow_array_t;

// Sun-visibility volume of the volumetric fog (local patch, see patches.md)
typedef struct {
    bool        pending;        // a bake was requested this frame (its shadow job is queued)
    int         layer;          // directional shadow layer the bake reads
    Matrix      viewProj;       // matrix that layer is rendered with
    Vector3     min;            // world-space minimum corner of the volume
    Vector3     voxel;          // voxel size per axis
    int         dims[3];        // voxel counts
    int         sub;            // subsamples per axis
    float       bias;           // depth bias of the shadow map
} r3d_light_fog_sunvol_request_t;

typedef struct {
    GLuint      texture;        // GL_TEXTURE_3D, GL_R8, 0 = none
    GLuint      framebuffer;
    bool        valid;          // baked and usable by the fog
    int         dims[3];
    Vector3     min;
    Vector3     extent;         // dims * voxel
    r3d_light_fog_sunvol_request_t request;
} r3d_light_fog_sunvol_t;

// ========================================
// MODULE STATE
// ========================================

extern struct r3d_light {
    r3d_light_shadow_array_t shadowArrays[R3D_LIGHT_TYPE_COUNT];
    r3d_list_t* listShadowJobs;
    r3d_list_t* listLightData;
    r3d_light_fog_sunvol_t fogSunVol;   // local patch, see patches.md
} R3D_MOD_LIGHT;

// ========================================
// MODULE FUNCTIONS
// ========================================

/* Initialize module (called once during R3D_Init) */
bool r3d_light_init(void);

/* Deinitialize module (called once during R3D_Close) */
void r3d_light_quit(void);

/* fixedViewProj: directional lights only, caller-fitted shadow matrix (local patch, see
 * patches.md), NULL = R3D's camera-fitted map */
void r3d_light_push(const R3D_Light* light, const R3D_ShadowMap* map, bool updateShadow, const Matrix* fixedViewProj);

/**/
r3d_light_data_t* r3d_light_get(int lightIndex);

/**/
void r3d_light_clear(void);

/* Returns the screen-space rectangle covered by the light's influence */
r3d_rect_t r3d_light_screen_rect(const r3d_light_data_t* light, int w, int h);

/**/
const char* r3d_light_type_name(R3D_LightType type);

/**/
int r3d_light_acquire_shadow_layer(R3D_LightType type);

/**/
void r3d_light_release_shadow_layer(R3D_LightType type, int layer);

/* Bind shadow framebuffer for a light type */
void r3d_light_bind_shadow_fbo(R3D_LightType type, int layer, int face);

/* Returns if the shadow map layer is valid */
bool r3d_light_shadow_layer_is_valid(R3D_LightType type, int layer);

/* Get the shadow map dimensions */
int r3d_light_shadow_map_size(R3D_LightType type);

/* Get a shadow map array texture ID */
GLuint r3d_light_shadow_map(R3D_LightType type);

/*
 * Min/max depth map of the directional shadow layers (local patch, see patches.md).
 * Tiles are R3D_SHADOW_MINMAX_TILE texels wide (must match shadow_minmax.frag).
 */
#define R3D_SHADOW_MINMAX_TILE 16
GLuint r3d_light_shadow_minmax_map(void);
int r3d_light_shadow_minmax_size(void);
uint32_t r3d_light_shadow_layer_count(R3D_LightType type);
void r3d_light_bind_shadow_minmax_fbo(int layer);
void r3d_light_bind_shadow_minmax_mid_fbo(void);
GLuint r3d_light_shadow_minmax_mid_map(void);
bool r3d_light_shadow_minmax_take_stale(void);

/**/
r3d_light_shadow_cache_t* r3d_light_shadow_cache(R3D_LightType type, int layer);

/*
 * Sun-visibility volume of the volumetric fog (local patch, see patches.md).
 * request: queues a shadow job rendering 'map' with 'viewProj' this frame and a bake of
 * 'region' from it after the shadow passes. Returns false on invalid arguments.
 */
bool r3d_light_request_fog_sunvol(const R3D_ShadowMap* map, Matrix viewProj, BoundingBox region, float voxelSize, int subsamples);
void r3d_light_clear_fog_sunvol(void);
/* (Re)allocates the 3D texture for the pending request; false if it can't be allocated */
bool r3d_light_prepare_fog_sunvol(void);
/* Attaches z slice 'slice' of the volume as the colour target, viewport = one slice */
void r3d_light_bind_fog_sunvol_slice(int slice);

// ========================================
// INLINE QUERIES
// ========================================

static inline bool r3d_light_has_visible(void)
{
    return !R3D_LIST_EMPTY(R3D_MOD_LIGHT.listLightData);
}

static inline bool r3d_light_has_shadow_job(void)
{
    return !R3D_LIST_EMPTY(R3D_MOD_LIGHT.listShadowJobs);
}

#endif // R3D_MODULE_LIGHT_H
