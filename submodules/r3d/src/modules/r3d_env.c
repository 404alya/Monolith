/* r3d_env.c -- Internal R3D environment module.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#include "./r3d_env.h"

#include <r3d/r3d_frustum.h>
#include <r3d/r3d_probe.h>
#include <r3d_config.h>
#include <raymath.h>
#include <string.h>

#include "../common/r3d_helper.h"
#include "../r3d_core_state.h"

// ========================================
// CONSTANTS
// ========================================

#define R3D_ENV_CUBEMAP_ARRAY_INIT_CAPACITY 16
#define R3D_ENV_CUBEMAP_ARRAY_GROWTH        8

// ========================================
// MODULE STATE
// ========================================

struct r3d_env R3D_MOD_ENV;

// ========================================
// TEXTURE FUNCTIONS
// ========================================

static bool alloc_depth_stencil_renderbuffer(GLuint renderbuffer, int size)
{
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, size, size);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    return true;
}

// ========================================
// CUBEMAP ARRAY FUNCTIONS
// ========================================

static void cubemap_array_allocate_texture(GLuint texture, int size, int mipLevels, uint32_t layers)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, texture);

    for (int level = 0; level < mipLevels; level++)
    {
        int mipSize = R3D_MAX(size >> level, 1);
        glTexImage3D(
            GL_TEXTURE_CUBE_MAP_ARRAY, level, GL_RGB16F,
            mipSize, mipSize, (int)layers * 6,
            0, GL_RGB, GL_FLOAT, NULL
        );
    }

    GLenum minFilter = (mipLevels > 1) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAX_LEVEL, mipLevels - 1);

    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);
}

static r3d_env_cubemap_array_t cubemap_array_create(int size, bool mipmapped)
{
    r3d_env_cubemap_array_t arr = {0};

    arr.freeList    = R3D_LIST_CREATE(int, R3D_ENV_CUBEMAP_ARRAY_INIT_CAPACITY);
    arr.layerStates = R3D_LIST_CREATE(r3d_env_cubemap_layer_state_t, R3D_ENV_CUBEMAP_ARRAY_INIT_CAPACITY);
    arr.size        = size;
    arr.mipLevels   = mipmapped ? r3d_get_mip_levels_1d(size) : 1;

    glGenFramebuffers(1, &arr.framebuffer);

    return arr;
}

static void cubemap_array_destroy(r3d_env_cubemap_array_t* arr)
{
    if (arr->texture != 0)     glDeleteTextures(1, &arr->texture);
    if (arr->framebuffer != 0) glDeleteFramebuffers(1, &arr->framebuffer);

    R3D_LIST_DESTROY(arr->layerStates);
    R3D_LIST_DESTROY(arr->freeList);

    *arr = (r3d_env_cubemap_array_t){0};
}

static bool cubemap_array_expand(r3d_env_cubemap_array_t* arr, uint32_t growth)
{
    uint32_t newLayerCount = arr->layerCount + growth;

    GLuint newTexture;
    glGenTextures(1, &newTexture);
    cubemap_array_allocate_texture(newTexture, arr->size, arr->mipLevels, newLayerCount);

    // Copy existing content into the new texture if any
    if (arr->layerCount > 0 && arr->texture != 0)
    {
        glActiveTexture(GL_TEXTURE0);

        glBindFramebuffer(GL_FRAMEBUFFER, arr->framebuffer);
        glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, newTexture);

        for (int level = 0; level < arr->mipLevels; level++)
        {
            int mipSize = R3D_MAX(arr->size >> level, 1);
            for (uint32_t layer = 0; layer < arr->layerCount; layer++)
            {
                for (int face = 0; face < 6; face++)
                {
                    int layerFace = (int)layer * 6 + face;
                    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, arr->texture, level, layerFace);
                    glCopyTexSubImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, level, 0, 0, layerFace, 0, 0, mipSize, mipSize);
                }
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);
    }

    if (arr->texture != 0)
    {
        glDeleteTextures(1, &arr->texture);
    }
    arr->texture = newTexture;

    // Resize layer states cache (new entries are zeroed)
    R3D_LIST_RESIZE(arr->layerStates, newLayerCount);

    // Newly allocated layers become available
    for (uint32_t layer = arr->layerCount; layer < newLayerCount; layer++)
    {
        int l = (int)layer;
        R3D_LIST_PUSH(arr->freeList, l);
    }

    arr->layerCount = newLayerCount;

    return true;
}

static int cubemap_array_acquire_layer(r3d_env_cubemap_array_t* arr)
{
    if (R3D_LIST_EMPTY(arr->freeList))
    {
        if (!cubemap_array_expand(arr, R3D_ENV_CUBEMAP_ARRAY_GROWTH))
        {
            return -1;
        }
    }

    int layer = -1;
    R3D_LIST_POP(arr->freeList, &layer);

    r3d_env_cubemap_layer_state_t* state = &R3D_LIST_GET(arr->layerStates, r3d_env_cubemap_layer_state_t, layer);
    state->acquired = true;
    state->rendered = false;

    return layer;
}

static void cubemap_array_release_layer(r3d_env_cubemap_array_t* arr, int layer)
{
    r3d_env_cubemap_layer_state_t* state = &R3D_LIST_GET(arr->layerStates, r3d_env_cubemap_layer_state_t, layer);
    state->acquired = false;
    state->rendered = false;

    R3D_LIST_PUSH(arr->freeList, layer);
}

// ========================================
// CUBEMAP FUNCTIONS
// ========================================

static void cubemap_allocate_texture(GLuint texture, int size, int mipLevels)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, texture);

    for (int level = 0; level < mipLevels; level++)
    {
        int mipSize = R3D_MAX(size >> level, 1);
        for (int face = 0; face < 6; face++)
        {
            glTexImage2D(
                GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, level, GL_RGB16F,
                mipSize, mipSize, 0, GL_RGB, GL_FLOAT, NULL
            );
        }
    }

    GLenum minFilter = (mipLevels > 1) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, mipLevels - 1);

    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

static r3d_env_cubemap_t cubemap_create(int size, bool mipmapped)
{
    r3d_env_cubemap_t cm = {0};

    cm.size      = size;
    cm.mipLevels = mipmapped ? r3d_get_mip_levels_1d(size) : 1;

    glGenTextures(1, &cm.texture);
    cubemap_allocate_texture(cm.texture, cm.size, cm.mipLevels);

    glGenRenderbuffers(1, &cm.depthStencil);
    alloc_depth_stencil_renderbuffer(cm.depthStencil, cm.size);

    glGenFramebuffers(1, &cm.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, cm.framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, cm.depthStencil);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return cm;
}

static void cubemap_destroy(r3d_env_cubemap_t* cm)
{
    if (cm->texture != 0)      glDeleteTextures(1, &cm->texture);
    if (cm->depthStencil != 0) glDeleteRenderbuffers(1, &cm->depthStencil);
    if (cm->framebuffer != 0)  glDeleteFramebuffers(1, &cm->framebuffer);

    *cm = (r3d_env_cubemap_t){0};
}

static void cubemap_bind_fbo(r3d_env_cubemap_t* cm, int face)
{
    glBindFramebuffer(GL_FRAMEBUFFER, cm->framebuffer);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
        cm->texture, 0
    );
    glViewport(0, 0, cm->size, cm->size);
}

static void cubemap_gen_mipmaps(r3d_env_cubemap_t* cm)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cm->texture);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

// ========================================
// PROBE FUNCTIONS
// ========================================

static void probe_job_init(r3d_env_probe_job_t* job, const R3D_Probe* probe)
{
    static const Vector3 DIRS[6] = {
        { 1.0,  0.0,  0.0}, {-1.0,  0.0,  0.0},  // +X, -X
        { 0.0,  1.0,  0.0}, { 0.0, -1.0,  0.0},  // +Y, -Y
        { 0.0,  0.0,  1.0}, { 0.0,  0.0, -1.0}   // +Z, -Z
    };

    static const Vector3 UPS[6] = {
        { 0.0, -1.0,  0.0 }, { 0.0, -1.0,  0.0},  // +X, -X
        { 0.0,  0.0,  1.0 }, { 0.0,  0.0, -1.0},  // +Y, -Y
        { 0.0, -1.0,  0.0 }, { 0.0, -1.0,  0.0}   // +Z, -Z
    };

    Matrix proj  = MatrixPerspective(90 * DEG2RAD, 1.0, 0.05f, probe->range);
    job->invProj = MatrixInvert(proj);

    for (int face = 0; face < 6; face++)
    {
        Vector3 target      = Vector3Add(probe->position, DIRS[face]);
        job->view[face]     = MatrixLookAt(probe->position, target, UPS[face]);
        job->viewProj[face] = MatrixMultiply(job->view[face], proj);
        job->frustum[face]  = R3D_ComputeFrustum(job->viewProj[face]);
        job->invView[face]  = MatrixInvert(job->view[face]);
    }

    job->position  = probe->position;
    job->probeType = probe->type;
    job->interior  = probe->interior;
    job->shadows   = probe->shadows;
    job->layer     = (int)probe->handle - 1;
}

static void probe_push(const R3D_Probe* probe, r3d_env_cubemap_array_t* cubemapArray, r3d_list_t** targetList, bool updateProbe)
{
    if (probe->range <= 0.0f) return;

    int layer = (int)probe->handle - 1;
    r3d_env_cubemap_layer_state_t* state = &R3D_LIST_GET(cubemapArray->layerStates, r3d_env_cubemap_layer_state_t, layer);

    bool mustCapture = !state->rendered || updateProbe;
    bool visible = R3D_FrustumIntersectsSphere(&R3D.viewState.frustum, probe->position, probe->range);

    if (mustCapture)
    {
        r3d_env_probe_job_t job = {0};
        probe_job_init(&job, probe);
        R3D_LIST_PUSH(R3D_MOD_ENV.listProbeJobs, job);
    }

    if (visible)
    {
        R3D_Probe p = {
            .type     = probe->type,
            .handle   = probe->handle,
            .position = probe->position,
            .falloff  = R3D_MAX(probe->falloff, 1e-4f),
            .range    = probe->range,
            .interior = probe->interior,
            .shadows  = probe->shadows,
        };

        r3d_list_push(targetList, &p);
    }
}

// ========================================
// MODULE FUNCTIONS
// ========================================

bool r3d_env_init(void)
{
    memset(&R3D_MOD_ENV, 0, sizeof(R3D_MOD_ENV));

    R3D_MOD_ENV.irradianceCapture = cubemap_create(R3D_HINT(R3D_HINT_IBL_IRRADIANCE_SIZE), false);
    R3D_MOD_ENV.prefilterCapture  = cubemap_create(R3D_HINT(R3D_HINT_IBL_PREFILTER_SIZE), true);

    R3D_MOD_ENV.irradiance = cubemap_array_create(R3D_HINT(R3D_HINT_IBL_IRRADIANCE_SIZE), false);
    R3D_MOD_ENV.prefilter  = cubemap_array_create(R3D_HINT(R3D_HINT_IBL_PREFILTER_SIZE), true);

    R3D_MOD_ENV.listProbeIllumination = R3D_LIST_CREATE(R3D_Probe, 16);
    R3D_MOD_ENV.listProbeReflection   = R3D_LIST_CREATE(R3D_Probe, 16);
    R3D_MOD_ENV.listProbeJobs         = R3D_LIST_CREATE(r3d_env_probe_job_t, 16);

    return true;
}

void r3d_env_quit(void)
{
    cubemap_destroy(&R3D_MOD_ENV.irradianceCapture);
    cubemap_destroy(&R3D_MOD_ENV.prefilterCapture);

    cubemap_array_destroy(&R3D_MOD_ENV.irradiance);
    cubemap_array_destroy(&R3D_MOD_ENV.prefilter);

    R3D_LIST_DESTROY(R3D_MOD_ENV.listProbeIllumination);
    R3D_LIST_DESTROY(R3D_MOD_ENV.listProbeReflection);
    R3D_LIST_DESTROY(R3D_MOD_ENV.listProbeJobs);
}

void r3d_env_push_probe(const R3D_Probe* probe, bool updateProbe)
{
    if (!r3d_env_probe_layer_is_valid(probe->type, (int)probe->handle - 1))
    {
        const char* pType = r3d_env_probe_type_name(probe->type);
        R3D_TRACELOG(LOG_WARNING, "Invalid pushed probe (type: %s - handle: %d)", pType, probe->handle);

        return;
    }

    switch (probe->type)
    {
    case R3D_PROBE_ILLUMINATION:
        probe_push(probe, &R3D_MOD_ENV.irradiance, &R3D_MOD_ENV.listProbeIllumination, updateProbe);
        break;
    case R3D_PROBE_REFLECTION:
        probe_push(probe, &R3D_MOD_ENV.prefilter, &R3D_MOD_ENV.listProbeReflection, updateProbe);
        break;
    default:
        R3D_ASSERT(false);
        break;
    }
}

R3D_Probe* r3d_env_probe_get(R3D_ProbeType type, int probeIndex)
{
    switch (type)
    {
    case R3D_PROBE_ILLUMINATION:
        return &R3D_LIST_GET(R3D_MOD_ENV.listProbeIllumination, R3D_Probe, probeIndex);
    case R3D_PROBE_REFLECTION:
        return &R3D_LIST_GET(R3D_MOD_ENV.listProbeReflection, R3D_Probe, probeIndex);
    default:
        R3D_ASSERT(false);
        break;
    }
    return NULL;
}

void r3d_env_probe_clear(void)
{
    R3D_LIST_CLEAR(R3D_MOD_ENV.listProbeIllumination);
    R3D_LIST_CLEAR(R3D_MOD_ENV.listProbeReflection);
    R3D_LIST_CLEAR(R3D_MOD_ENV.listProbeJobs);
}

bool r3d_env_probe_layer_is_valid(R3D_ProbeType type, int layer)
{
    switch (type)
    {
    case R3D_PROBE_ILLUMINATION:
        return r3d_env_irradiance_layer_is_valid(layer);
    case R3D_PROBE_REFLECTION:
        return r3d_env_prefilter_layer_is_valid(layer);
    default:
        R3D_ASSERT(false);
        break;
    }

    return false;
}

void r3d_env_probe_capture_bind_fbo(R3D_ProbeType type, int face)
{
    switch (type)
    {
    case R3D_PROBE_ILLUMINATION:
        cubemap_bind_fbo(&R3D_MOD_ENV.irradianceCapture, face);
        break;
    case R3D_PROBE_REFLECTION:
        cubemap_bind_fbo(&R3D_MOD_ENV.prefilterCapture, face);
        break;
    default:
        R3D_ASSERT(false);
        break;
    }
}

void r3d_env_probe_capture_gen_mipmaps(R3D_ProbeType type)
{
    R3D_ASSERT(type == R3D_PROBE_REFLECTION);
    cubemap_gen_mipmaps(&R3D_MOD_ENV.prefilterCapture);
}

GLuint r3d_env_probe_capture_get(R3D_ProbeType type)
{
    switch (type)
    {
    case R3D_PROBE_ILLUMINATION:
        return R3D_MOD_ENV.irradianceCapture.texture;
    case R3D_PROBE_REFLECTION:
        return R3D_MOD_ENV.prefilterCapture.texture;
    default:
        R3D_ASSERT(false);
        break;
    }
    return 0;
}

int r3d_env_probe_capture_size(R3D_ProbeType type)
{
    switch (type)
    {
    case R3D_PROBE_ILLUMINATION:
        return R3D_MOD_ENV.irradianceCapture.size;
    case R3D_PROBE_REFLECTION:
        return R3D_MOD_ENV.prefilterCapture.size;
    default:
        R3D_ASSERT(false);
        break;
    }
    return 0;
}

const char* r3d_env_probe_type_name(R3D_ProbeType type)
{
    switch (type)
    {
    case R3D_PROBE_ILLUMINATION: return "Illumination";
    case R3D_PROBE_REFLECTION:   return "Reflection";
    default: break;
    }
    return NULL;
}

int r3d_env_irradiance_acquire_layer(void)
{
    return cubemap_array_acquire_layer(&R3D_MOD_ENV.irradiance);
}

void r3d_env_irradiance_release_layer(int layer)
{
    if (r3d_env_irradiance_layer_is_valid(layer))
    {
        cubemap_array_release_layer(&R3D_MOD_ENV.irradiance, layer);
    }
}

void r3d_env_irradiance_bind_fbo(int layer, int face)
{
    r3d_env_cubemap_array_t* arr = &R3D_MOD_ENV.irradiance;

    r3d_env_cubemap_layer_state_t* state = &R3D_LIST_GET(arr->layerStates, r3d_env_cubemap_layer_state_t, layer);
    state->rendered = true;

    glBindFramebuffer(GL_FRAMEBUFFER, arr->framebuffer);
    glFramebufferTextureLayer(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        arr->texture, 0, layer * 6 + face
    );

    glViewport(0, 0, arr->size, arr->size);
}

bool r3d_env_irradiance_layer_is_valid(int layer)
{
    r3d_env_cubemap_array_t* arr = &R3D_MOD_ENV.irradiance;

    if (layer < 0 || (uint32_t)layer >= arr->layerCount)
    {
        return false;
    }

    r3d_env_cubemap_layer_state_t* state = &R3D_LIST_GET(arr->layerStates, r3d_env_cubemap_layer_state_t, layer);
    return state->acquired;
}

GLuint r3d_env_irradiance_get(void)
{
    return R3D_MOD_ENV.irradiance.texture;
}

int r3d_env_prefilter_acquire_layer(void)
{
    return cubemap_array_acquire_layer(&R3D_MOD_ENV.prefilter);
}

void r3d_env_prefilter_release_layer(int layer)
{
    if (r3d_env_prefilter_layer_is_valid(layer))
    {
        cubemap_array_release_layer(&R3D_MOD_ENV.prefilter, layer);
    }
}

void r3d_env_prefilter_bind_fbo(int layer, int face, int mipLevel)
{
    r3d_env_cubemap_array_t* arr = &R3D_MOD_ENV.prefilter;

    R3D_ASSERT(mipLevel < arr->mipLevels);

    r3d_env_cubemap_layer_state_t* state = &R3D_LIST_GET(arr->layerStates, r3d_env_cubemap_layer_state_t, layer);
    state->rendered = true;

    glBindFramebuffer(GL_FRAMEBUFFER, arr->framebuffer);
    glFramebufferTextureLayer(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        arr->texture, mipLevel, layer * 6 + face
    );

    int mipSize = arr->size >> mipLevel;
    glViewport(0, 0, mipSize, mipSize);
}

bool r3d_env_prefilter_layer_is_valid(int layer)
{
    r3d_env_cubemap_array_t* arr = &R3D_MOD_ENV.prefilter;

    if (layer < 0 || (uint32_t)layer >= arr->layerCount)
    {
        return false;
    }

    r3d_env_cubemap_layer_state_t* state = &R3D_LIST_GET(arr->layerStates, r3d_env_cubemap_layer_state_t, layer);
    return state->acquired;
}

GLuint r3d_env_prefilter_get(void)
{
    return R3D_MOD_ENV.prefilter.texture;
}
