/* r3d_target.c -- Internal R3D render target module.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#include "./r3d_target.h"
#include <r3d_config.h>
#include <stddef.h>
#include <string.h>

#include "../common/r3d_helper.h"
#include "../common/r3d_math.h"

// ========================================
// MODULE STATE
// ========================================

struct r3d_mod_target R3D_MOD_TARGET;

// ========================================
// INTERNAL OPENGL FORMAT TABLE
// ========================================

typedef struct {
    GLenum internal;
    GLenum format;
    GLenum type;
} target_format_t;

typedef enum {
    FORMAT_R8,   FORMAT_RG8,   FORMAT_RGB8,   FORMAT_RGBA8,
    FORMAT_R16,  FORMAT_RG16,  FORMAT_RGB16,  FORMAT_RGBA16,
    FORMAT_R8UI, FORMAT_RG8UI, FORMAT_RGB8UI, FORMAT_RGBA8UI,
    FORMAT_R16F, FORMAT_RG16F, FORMAT_RGB16F, FORMAT_RGBA16F,
    FORMAT_R32F, FORMAT_RG32F, FORMAT_RGB32F, FORMAT_RGBA32F,
    FORMAT_R11G11B10F,
} target_format_enum_t;

static const target_format_t TARGET_FORMAT[] = {
    [FORMAT_R8]         = { GL_R8,             GL_RED,          GL_UNSIGNED_BYTE },
    [FORMAT_RG8]        = { GL_RG8,            GL_RG,           GL_UNSIGNED_BYTE },
    [FORMAT_RGB8]       = { GL_RGB8,           GL_RGB,          GL_UNSIGNED_BYTE },
    [FORMAT_RGBA8]      = { GL_RGBA8,          GL_RGBA,         GL_UNSIGNED_BYTE },
    [FORMAT_R16]        = { GL_R16,            GL_RED,          GL_UNSIGNED_SHORT },
    [FORMAT_RG16]       = { GL_RG16,           GL_RG,           GL_UNSIGNED_SHORT },
    [FORMAT_RGB16]      = { GL_RGB16,          GL_RGB,          GL_UNSIGNED_SHORT },
    [FORMAT_RGBA16]     = { GL_RGBA16,         GL_RGBA,         GL_UNSIGNED_SHORT },
    [FORMAT_R8UI]       = { GL_R8UI,           GL_RED_INTEGER,  GL_UNSIGNED_BYTE },
    [FORMAT_RG8UI]      = { GL_RG8UI,          GL_RG_INTEGER,   GL_UNSIGNED_BYTE },
    [FORMAT_RGB8UI]     = { GL_RGB8UI,         GL_RGB_INTEGER,  GL_UNSIGNED_BYTE },
    [FORMAT_RGBA8UI]    = { GL_RGBA8UI,        GL_RGBA_INTEGER, GL_UNSIGNED_BYTE },
    [FORMAT_R16F]       = { GL_R16F,           GL_RED,          GL_HALF_FLOAT },
    [FORMAT_RG16F]      = { GL_RG16F,          GL_RG,           GL_HALF_FLOAT },
    [FORMAT_RGB16F]     = { GL_RGB16F,         GL_RGB,          GL_HALF_FLOAT },
    [FORMAT_RGBA16F]    = { GL_RGBA16F,        GL_RGBA,         GL_HALF_FLOAT },
    [FORMAT_R32F]       = { GL_R32F,           GL_RED,          GL_FLOAT },
    [FORMAT_RG32F]      = { GL_RG32F,          GL_RG,           GL_FLOAT },
    [FORMAT_RGB32F]     = { GL_RGB32F,         GL_RGB,          GL_FLOAT },
    [FORMAT_RGBA32F]    = { GL_RGBA32F,        GL_RGBA,         GL_FLOAT },
    [FORMAT_R11G11B10F] = { GL_R11F_G11F_B10F, GL_RGB,          GL_FLOAT },
};

// ========================================
// INTERNAL TARGET FUNCTIONS
// ========================================

typedef struct {
    target_format_enum_t format;
    int resolutionDiv;              // 1 = full res, 2 = half res, 4 = quarter res... 0 = 1x1
    GLenum minFilter;
    GLenum magFilter;
    int numLevels;                  // 0 = auto (full mip chain), >0 = fixed number of native levels
    float clear[4];
} target_config_t;

static const target_config_t TARGET_CONFIG[] = {
    [R3D_TARGET_SCENE_0]     = { FORMAT_RGBA16F,    1, GL_LINEAR,                 GL_LINEAR,  1, {0} },
    [R3D_TARGET_SCENE_1]     = { FORMAT_RGBA16F,    1, GL_LINEAR,                 GL_LINEAR,  1, {0} },
    [R3D_TARGET_ALBEDO]      = { FORMAT_RGB8,       1, GL_NEAREST,                GL_NEAREST, 1, {0} },
    [R3D_TARGET_NORMAL]      = { FORMAT_RG16,       1, GL_NEAREST,                GL_NEAREST, 2, {0} },
    [R3D_TARGET_GEOM_NORMAL] = { FORMAT_RG16,       1, GL_NEAREST,                GL_NEAREST, 1, {0} },
    [R3D_TARGET_ORM]         = { FORMAT_RGBA8,      1, GL_NEAREST,                GL_NEAREST, 1, {0} },
    [R3D_TARGET_DEPTH]       = { FORMAT_R16F,       1, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST, 0, {65504.0f, 65504.0f, 65504.0f, 65504.0f} },
    [R3D_TARGET_SELECTOR]    = { FORMAT_R8UI,       2, GL_NEAREST,                GL_NEAREST, 0, {0} },
    [R3D_TARGET_RADIANCE]    = { FORMAT_RGB16F,     1, GL_NEAREST,                GL_NEAREST, 2, {0} },
    [R3D_TARGET_SPECULAR]    = { FORMAT_RGB16F,     1, GL_NEAREST,                GL_NEAREST, 2, {0} },
    [R3D_TARGET_VFOG_RAD]    = { FORMAT_R11G11B10F, 2, GL_LINEAR,                 GL_LINEAR,  1, {0} },
    [R3D_TARGET_SSAO_0]      = { FORMAT_R8,         2, GL_LINEAR,                 GL_LINEAR,  1, {0} },
    [R3D_TARGET_SSAO_1]      = { FORMAT_R8,         2, GL_LINEAR,                 GL_LINEAR,  1, {0} },
    [R3D_TARGET_SSIL_0]      = { FORMAT_RGBA16F,    2, GL_LINEAR,                 GL_LINEAR,  1, {0} },
    [R3D_TARGET_SSIL_1]      = { FORMAT_RGBA16F,    2, GL_LINEAR,                 GL_LINEAR,  1, {0} },
    [R3D_TARGET_SSGI_0]      = { FORMAT_RGB16F,     2, GL_LINEAR,                 GL_LINEAR,  1, {0} },
    [R3D_TARGET_SSGI_1]      = { FORMAT_RGB16F,     2, GL_LINEAR,                 GL_LINEAR,  1, {0} },
    [R3D_TARGET_SSR]         = { FORMAT_RGBA16F,    2, GL_LINEAR_MIPMAP_LINEAR,   GL_LINEAR,  0, {0} },
    [R3D_TARGET_DOF_COC]     = { FORMAT_R16F,       1, GL_LINEAR,                 GL_LINEAR,  1, {0} },
    [R3D_TARGET_DOF_0]       = { FORMAT_RGBA16F,    2, GL_LINEAR,                 GL_LINEAR,  1, {0} },
    [R3D_TARGET_DOF_1]       = { FORMAT_RGBA16F,    2, GL_LINEAR,                 GL_LINEAR,  1, {0} },
    [R3D_TARGET_BLOOM]       = { FORMAT_RGB16F,     2, GL_LINEAR_MIPMAP_LINEAR,   GL_LINEAR,  0, {0} },
    [R3D_TARGET_LUMINANCE]   = { FORMAT_R16F,       2, GL_NEAREST,                GL_NEAREST, 0, {0} },
    [R3D_TARGET_EXPOSURE_0]  = { FORMAT_RG16F,      0, GL_NEAREST,                GL_NEAREST, 1, {1.0f, R3D_LOG018, 0.0f, 1.0f} },
    [R3D_TARGET_EXPOSURE_1]  = { FORMAT_RG16F,      0, GL_NEAREST,                GL_NEAREST, 1, {1.0f, R3D_LOG018, 0.0f, 1.0f} },
    [R3D_TARGET_SMAA_EDGES]  = { FORMAT_RG8,        1, GL_LINEAR,                 GL_LINEAR,  1, {0} },
    [R3D_TARGET_SMAA_BLEND]  = { FORMAT_RGBA8,      1, GL_LINEAR,                 GL_LINEAR,  1, {0} },
};

static void alloc_target_texture(r3d_target_t target)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, R3D_MOD_TARGET.targetTextures[target]);

    const target_config_t* config = &TARGET_CONFIG[target];
    const target_format_t* format = &TARGET_FORMAT[config->format];

    int minLevel  = r3d_target_get_min_level(target);
    int numLevels = r3d_target_get_num_levels(target);

    for (int i = 0; i < numLevels; i++)
    {
        int wLevel = 0, hLevel = 0;
        r3d_target_get_resolution(&wLevel, &hLevel, minLevel + i);
        glTexImage2D(GL_TEXTURE_2D, i, format->internal, wLevel, hLevel, 0, format->format, format->type, NULL);
    }

    // NOTE: By default, sampling is locked at the first level
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,  0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, config->minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, config->magFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    R3D_MOD_TARGET.targetStates[target] = (r3d_target_state_t) {0};
    R3D_MOD_TARGET.targetLoaded[target] = true;
}

static void alloc_depth_stencil_texture(void)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, R3D_MOD_TARGET.depthTexture);

    int maxLevel = r3d_target_get_max_level(R3D_TARGET_DEPTH);

    for (int level = 0; level <= maxLevel; level++)
    {
        int w = 0, h = 0;
        r3d_target_get_resolution(&w, &h, level);
        glTexImage2D(
            GL_TEXTURE_2D, level, GL_DEPTH24_STENCIL8,
            w, h, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL
        );
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);
}

/*
 * Returns the index of the FBO in the cache.
 * If the combination doesn't exist, creates a new FBO and caches it.
 */
static int get_or_create_fbo(const r3d_target_t* targets, int count, bool depth)
{
    R3D_ASSERT(targets || (!targets && count == 0));
    R3D_ASSERT(count <= R3D_TARGET_MAX_ATTACHMENTS);
    R3D_ASSERT(count > 0 || (count == 0 && depth));

    // Search if the combination is already cached
    for (int i = 0; i < R3D_MOD_TARGET.fboCount; i++)
    {
        const r3d_target_fbo_t* fbo = &R3D_MOD_TARGET.fbo[i];
        if (fbo->targetCount == count && fbo->hasDepth == depth)
        {
            if (count == 0 || memcmp(fbo->targets, targets, count * sizeof(*targets)) == 0)
            {
                return i;
            }
        }
    }

    // Otherwise create the FBO and cache it
    R3D_ASSERT(R3D_MOD_TARGET.fboCount < R3D_TARGET_MAX_FRAMEBUFFERS);

    int newIndex = R3D_MOD_TARGET.fboCount++;
    r3d_target_fbo_t* fbo = &R3D_MOD_TARGET.fbo[newIndex];

    glGenFramebuffers(1, &fbo->id);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo->id);

    GLenum glColor[R3D_TARGET_MAX_ATTACHMENTS];
    int locCount = 0;

    for (int i = 0; i < count; i++)
    {
        if (!R3D_MOD_TARGET.targetLoaded[targets[i]])
        {
            alloc_target_texture(targets[i]);
        }

        GLuint texture = R3D_MOD_TARGET.targetTextures[targets[i]];
        fbo->targets[i] = targets[i];

        // Temporary binding at native level 0, solely to validate completeness
        // Will be overwritten by the first real bind via the sentinel boundLevel = -1
        GLenum attachment = GL_COLOR_ATTACHMENT0 + locCount;
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, texture, 0);
        glColor[locCount++] = attachment;
    }

    if (depth)
    {
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
            GL_TEXTURE_2D, R3D_MOD_TARGET.depthTexture, 0
        );
    }

    fbo->targetCount = count;
    fbo->hasDepth    = depth;
    fbo->boundLevel  = -1;      // -1 forces the first r3d_target_set_write_level() to re-attach

    if (locCount > 0)
    {
        glDrawBuffers(locCount, glColor);
    }
    else
    {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        R3D_TRACELOG(LOG_ERROR, "Framebuffer incomplete (status: 0x%04x)", status);
    }

    return newIndex;
}

// ========================================
// MODULE FUNCTIONS
// ========================================

bool r3d_target_init(int resW, int resH)
{
    memset(&R3D_MOD_TARGET, 0, sizeof(R3D_MOD_TARGET));

    glGenTextures(R3D_TARGET_COUNT, R3D_MOD_TARGET.targetTextures);
    glGenTextures(1, &R3D_MOD_TARGET.depthTexture);

    R3D_MOD_TARGET.currentFbo = -1;

    R3D_MOD_TARGET.resW = resW;
    R3D_MOD_TARGET.resH = resH;
    R3D_MOD_TARGET.txlW = 1.0f / resW;
    R3D_MOD_TARGET.txlH = 1.0f / resH;

    alloc_depth_stencil_texture();

    return true;
}

void r3d_target_quit(void)
{
    glDeleteTextures(R3D_TARGET_COUNT, R3D_MOD_TARGET.targetTextures);
    glDeleteTextures(1, &R3D_MOD_TARGET.depthTexture);

    for (int i = 0; i < R3D_MOD_TARGET.fboCount; i++)
    {
        if (R3D_MOD_TARGET.fbo[i].id != 0)
        {
            glDeleteFramebuffers(1, &R3D_MOD_TARGET.fbo[i].id);
        }
    }
}

void r3d_target_resize(uint32_t resW, uint32_t resH)
{
    if (R3D_MOD_TARGET.resW == resW && R3D_MOD_TARGET.resH == resH)
    {
        return;
    }

    R3D_MOD_TARGET.resW = resW;
    R3D_MOD_TARGET.resH = resH;
    R3D_MOD_TARGET.txlW = 1.0f / (float)resW;
    R3D_MOD_TARGET.txlH = 1.0f / (float)resH;

    alloc_depth_stencil_texture();

    for (int i = 0; i < R3D_TARGET_COUNT; i++)
    {
        if (R3D_MOD_TARGET.targetLoaded[i])
        {
            alloc_target_texture(i);
        }
    }
}

int r3d_target_get_min_level(r3d_target_t target)
{
    const target_config_t* config = &TARGET_CONFIG[target];

    if (config->resolutionDiv <= 0)
    {
        return r3d_get_mip_levels_2d(R3D_MOD_TARGET.resW, R3D_MOD_TARGET.resH) - 1;
    }

    return r3d_log2i_fast(config->resolutionDiv);
}

int r3d_target_get_max_level(r3d_target_t target)
{
    const target_config_t* config = &TARGET_CONFIG[target];

    if (config->numLevels <= 0)
    {
        return r3d_get_mip_levels_2d(R3D_MOD_TARGET.resW, R3D_MOD_TARGET.resH) - 1;
    }

    return r3d_target_get_min_level(target) + config->numLevels - 1;
}

int r3d_target_get_num_levels(r3d_target_t target)
{
    const target_config_t* config = &TARGET_CONFIG[target];

    if (config->numLevels > 0)
    {
        return config->numLevels;
    }

    int minLevel = r3d_target_get_min_level(target);
    int maxLevel = r3d_target_get_max_level(target);

    return maxLevel - minLevel + 1;
}

void r3d_target_get_resolution(int* w, int* h, int level)
{
    int rw = (int)(R3D_MOD_TARGET.resW >> level);
    int rh = (int)(R3D_MOD_TARGET.resH >> level);

    if (w) *w = rw > 0 ? rw : 1;
    if (h) *h = rh > 0 ? rh : 1;
}

Vector2 r3d_target_get_texel_size(int level)
{
    float scale = (float)(1 << level);
    float tx = R3D_MOD_TARGET.txlW * scale;
    float ty = R3D_MOD_TARGET.txlH * scale;

    return (Vector2) {tx, ty};
}

r3d_target_t r3d_target_swap_scene(r3d_target_t scene)
{
    if (scene == R3D_TARGET_SCENE_0)
    {
        return R3D_TARGET_SCENE_1;
    }
    return R3D_TARGET_SCENE_0;
}

void r3d_target_bind_clear(const r3d_target_t* targets, int count, int level, bool depth)
{
    r3d_target_bind_load(targets, count, level, depth);

    for (int i = 0; i < count; i++)
    {
        glClearBufferfv(GL_COLOR, i, TARGET_CONFIG[targets[i]].clear);
    }

    if (depth)
    {
        glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
    }
}

void r3d_target_bind_load(const r3d_target_t* targets, int count, int level, bool depth)
{
    R3D_ASSERT(count > 0 || depth);

    int fboIndex = get_or_create_fbo(targets, count, depth);
    if (fboIndex != R3D_MOD_TARGET.currentFbo)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, R3D_MOD_TARGET.fbo[fboIndex].id);
        R3D_MOD_TARGET.currentFbo = fboIndex;
    }

    r3d_target_set_write_level(level);
    r3d_target_set_viewport(level);
}

void r3d_target_set_viewport(int level)
{
    int vpW = 0, vpH = 0;
    r3d_target_get_resolution(&vpW, &vpH, level);
    glViewport(0, 0, vpW, vpH);
}

void r3d_target_set_write_level(int level)
{
    R3D_ASSERT(R3D_MOD_TARGET.currentFbo >= 0);

    r3d_target_fbo_t* fbo = &R3D_MOD_TARGET.fbo[R3D_MOD_TARGET.currentFbo];

    if (level != fbo->boundLevel)
    {
        for (int i = 0; i < fbo->targetCount; i++)
        {
            r3d_target_t target = fbo->targets[i];
            int minLevel = r3d_target_get_min_level(target);
            int maxLevel = r3d_target_get_max_level(target);

            R3D_ASSERT(level >= minLevel && "Level below target's native resolution");
            R3D_ASSERT(level <= maxLevel && "Level exceeds target's mip chain");

            int nativeLevel = level - minLevel;

            glFramebufferTexture2D(
                GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i,
                GL_TEXTURE_2D, R3D_MOD_TARGET.targetTextures[target], nativeLevel
            );
        }

        if (fbo->hasDepth)
        {
            R3D_ASSERT(level >= 0 && level <= r3d_target_get_max_level(R3D_TARGET_DEPTH));
            glFramebufferTexture2D(
                GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                GL_TEXTURE_2D, R3D_MOD_TARGET.depthTexture, level
            );
        }

        fbo->boundLevel = level;
    }
}

void r3d_target_set_read_level(r3d_target_t target, int level)
{
    r3d_target_set_read_levels(target, level, level);
}

void r3d_target_set_read_levels(r3d_target_t target, int baseLevel, int maxLevel)
{
    R3D_ASSERT(R3D_MOD_TARGET.targetLoaded[target]);

    int minLevel = r3d_target_get_min_level(target);
    int maxValid = r3d_target_get_max_level(target);

    R3D_ASSERT(baseLevel >= minLevel && maxLevel >= minLevel);
    R3D_ASSERT(baseLevel <= maxValid);
    R3D_ASSERT(maxLevel  <= maxValid);

    int nativeBase = baseLevel - minLevel;
    int nativeMax  = maxLevel - minLevel;

    r3d_target_state_t* state = &R3D_MOD_TARGET.targetStates[target];

    if (state->baseLevel != nativeBase || state->maxLevel != nativeMax)
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, R3D_MOD_TARGET.targetTextures[target]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, nativeBase);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,  nativeMax);
        glBindTexture(GL_TEXTURE_2D, 0);

        state->baseLevel = nativeBase;
        state->maxLevel  = nativeMax;
    }
}

void r3d_target_gen_mipmap(r3d_target_t target)
{
    GLuint id = r3d_target_get_all_levels(target);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, id);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}

bool r3d_target_exists(r3d_target_t target)
{
    return (r3d_target_get_or_null(target) != 0);
}

GLuint r3d_target_get(r3d_target_t target)
{
    R3D_ASSERT(target > R3D_TARGET_INVALID && target < R3D_TARGET_COUNT);
    R3D_ASSERT(R3D_MOD_TARGET.targetLoaded[target]);
    return R3D_MOD_TARGET.targetTextures[target];
}

GLuint r3d_target_get_or_null(r3d_target_t target)
{
    if (target <= R3D_TARGET_INVALID || target >= R3D_TARGET_COUNT) return 0;
    if (!R3D_MOD_TARGET.targetLoaded[target]) return 0;
    return R3D_MOD_TARGET.targetTextures[target];
}

GLuint r3d_target_get_level(r3d_target_t target, int level)
{
    return r3d_target_get_levels(target, level, level);
}

GLuint r3d_target_get_levels(r3d_target_t target, int baseLevel, int maxLevel)
{
    R3D_ASSERT(target > R3D_TARGET_INVALID && target < R3D_TARGET_COUNT);
    r3d_target_set_read_levels(target, baseLevel, maxLevel);
    return R3D_MOD_TARGET.targetTextures[target];
}

GLuint r3d_target_get_all_levels(r3d_target_t target)
{
    R3D_ASSERT(target > R3D_TARGET_INVALID && target < R3D_TARGET_COUNT);

    int minLevel = r3d_target_get_min_level(target);
    int maxLevel = r3d_target_get_max_level(target);

    r3d_target_set_read_levels(target, minLevel, maxLevel);

    return R3D_MOD_TARGET.targetTextures[target];
}

GLuint r3d_target_get_depth_buffer(void)
{
    return R3D_MOD_TARGET.depthTexture;
}

void r3d_target_blit(r3d_target_t target, bool depth, GLuint dstFbo, int dstX, int dstY, int dstW, int dstH, bool linear)
{
    R3D_ASSERT(target > R3D_TARGET_INVALID || depth);

    int fboIndex = -1;
    if (target > R3D_TARGET_INVALID)
    {
        fboIndex = get_or_create_fbo(&target, 1, depth);
    }
    else
    {
        fboIndex = get_or_create_fbo(NULL, 0, true);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, R3D_MOD_TARGET.fbo[fboIndex].id);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);

    if (linear)
    {
        if (target > R3D_TARGET_INVALID)
        {
            glBlitFramebuffer(
                0, 0, R3D_MOD_TARGET.resW, R3D_MOD_TARGET.resH,
                dstX, dstY, dstX + dstW, dstY + dstH, GL_COLOR_BUFFER_BIT,
                GL_LINEAR
            );
        }
        if (depth)
        {
            glBlitFramebuffer(
                0, 0, R3D_MOD_TARGET.resW, R3D_MOD_TARGET.resH,
                dstX, dstY, dstX + dstW, dstY + dstH, GL_DEPTH_BUFFER_BIT,
                GL_NEAREST
            );
        }
    }
    else
    {
        GLbitfield mask = GL_NONE;
        if (target > R3D_TARGET_INVALID) mask |= GL_COLOR_BUFFER_BIT;
        if (depth) mask |= GL_DEPTH_BUFFER_BIT;
        glBlitFramebuffer(
            0, 0, R3D_MOD_TARGET.resW, R3D_MOD_TARGET.resH,
            dstX, dstY, dstX + dstW, dstY + dstH, mask,
            GL_NEAREST
        );
    }
}

void r3d_target_invalidate_cache(void)
{
    R3D_MOD_TARGET.currentFbo = -1;
}
