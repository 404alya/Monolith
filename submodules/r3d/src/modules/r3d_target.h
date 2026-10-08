/* r3d_target.h -- Internal R3D render target module.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#ifndef R3D_MODULE_TARGET_H
#define R3D_MODULE_TARGET_H

#include <raylib.h>
#include <glad.h>

// ========================================
// TARGET ENUM
// ========================================

/*
 * Enums for all internal render targets.
 * To add a new target, define a new enum before `R3D_TARGET_DEPTH`,
 * then add its creation parameters in `TARGET_CONFIG` in `r3d_target.c`.
 * Loading happens on-demand during bind operations.
 */
typedef enum {
    R3D_TARGET_INVALID = -1,
    R3D_TARGET_SCENE_0,         //< Full - Mip 1 - RGBA16F
    R3D_TARGET_SCENE_1,         //< Full - Mip 1 - RGBA16F
    R3D_TARGET_ALBEDO,          //< Full - Mip 1 - RGB8
    R3D_TARGET_NORMAL,          //< Full - Mip 2 - RG16
    R3D_TARGET_GEOM_NORMAL,     //< Full - Mip 1 - RG16
    R3D_TARGET_ORM,             //< Full - Mip 1 - RGBA8
    R3D_TARGET_DEPTH,           //< Full - Mip N - R16F
    R3D_TARGET_SELECTOR,        //< Half - Mip N - R8UI
    R3D_TARGET_RADIANCE,        //< Full - Mip 2 - RGB16F
    R3D_TARGET_SPECULAR,        //< Full - Mip 2 - RGB16F
    R3D_TARGET_VFOG_RAD,        //< Half - Mip 1 - R11G11B10F
    R3D_TARGET_SSAO_0,          //< Half - Mip 1 - R8
    R3D_TARGET_SSAO_1,          //< Half - Mip 1 - R8
    R3D_TARGET_SSIL_0,          //< Half - Mip 1 - RGBA16F
    R3D_TARGET_SSIL_1,          //< Half - Mip 1 - RGBA16F
    R3D_TARGET_SSGI_0,          //< Half - Mip 1 - RGB16F
    R3D_TARGET_SSGI_1,          //< Half - Mip 1 - RGB16F
    R3D_TARGET_SSR,             //< Half - Mip N - RGBA16F
    R3D_TARGET_DOF_COC,         //< Full - Mip 1 - R16F
    R3D_TARGET_DOF_0,           //< Half - Mip 1 - RGBA16F
    R3D_TARGET_DOF_1,           //< Half - Mip 1 - RGBA16F
    R3D_TARGET_BLOOM,           //< Half - Mip N - RGB16F
    R3D_TARGET_LUMINANCE,       //< Half - Mip N - R16F
    R3D_TARGET_EXPOSURE_0,      //< 1x1  - Mip 1 - RG16F
    R3D_TARGET_EXPOSURE_1,      //< 1x1  - Mip 1 - RG16F
    R3D_TARGET_SMAA_EDGES,      //< Full - Mip 1 - RG8
    R3D_TARGET_SMAA_BLEND,      //< Full - Mip 1 - RGBA8
    R3D_TARGET_COUNT
} r3d_target_t;

// ========================================
// HELPER TARGET PACKS
// ========================================

#define R3D_TARGET_GBUFFER      \
    R3D_TARGET_ALBEDO,          \
    R3D_TARGET_NORMAL,          \
    R3D_TARGET_GEOM_NORMAL,     \
    R3D_TARGET_ORM,             \
    R3D_TARGET_DEPTH,           \
    R3D_TARGET_RADIANCE

#define R3D_TARGET_DECAL        \
    R3D_TARGET_ALBEDO,          \
    R3D_TARGET_NORMAL,          \
    R3D_TARGET_ORM,             \
    R3D_TARGET_RADIANCE

// ========================================
// HELPER MACROS
// ========================================

#define R3D_TARGET_SIZE_W   R3D_MOD_TARGET.resW
#define R3D_TARGET_SIZE_H   R3D_MOD_TARGET.resH
#define R3D_TARGET_TEXEL_W  R3D_MOD_TARGET.txlW
#define R3D_TARGET_TEXEL_H  R3D_MOD_TARGET.txlH

#define R3D_TARGET_BIND_CLEAR(level, depth, ...)                        \
    r3d_target_bind_clear(                                              \
        (r3d_target_t[]) {__VA_ARGS__},                                 \
        sizeof((r3d_target_t[]) {__VA_ARGS__}) / sizeof(r3d_target_t),  \
        (level), (depth)                                                \
    )

#define R3D_TARGET_BIND_LOAD(level, depth, ...)                         \
    r3d_target_bind_load(                                               \
        (r3d_target_t[]){ __VA_ARGS__ },                                \
        sizeof((r3d_target_t[]) {__VA_ARGS__}) / sizeof(r3d_target_t),  \
        (level), (depth)                                                \
    )

/*
 * Binds the target, then swaps to the alternate scene target.
 * Modifies the target parameter to point to the other buffer.
 */
#define R3D_TARGET_BIND_AND_SWAP_SCENE(target)  \
do {                                            \
    R3D_TARGET_BIND_LOAD(0, false, target);     \
    target = r3d_target_swap_scene(target);     \
} while(0)

// ========================================
// TARGET FBO STRUCTURE
// ========================================

#define R3D_TARGET_MAX_FRAMEBUFFERS 32
#define R3D_TARGET_MAX_ATTACHMENTS  8

typedef struct {
    r3d_target_t targets[R3D_TARGET_MAX_ATTACHMENTS];
    int targetCount;
    int boundLevel;
    bool hasDepth;
    GLuint id;
} r3d_target_fbo_t;

// ========================================
// TARGET BUFFER STRUCTURE
// ========================================

typedef struct {
    int baseLevel;
    int maxLevel;
} r3d_target_state_t;

// ========================================
// MODULE STATE
// ========================================

extern struct r3d_mod_target {

    r3d_target_fbo_t fbo[R3D_TARGET_MAX_FRAMEBUFFERS];  //< FBO combination cache. FBOs are automatically generated as needed during bind
    int currentFbo;                                     //< Cache index of currently bound FBO, -1 if none bound. Reset via `r3d_target_invalidate_cache()`
    int fboCount;                                       //< Number of FBOs created

    r3d_target_state_t targetStates[R3D_TARGET_COUNT];  //< Array of target states
    GLuint targetTextures[R3D_TARGET_COUNT];            //< Array of target IDs (textures)
    bool targetLoaded[R3D_TARGET_COUNT];                //< Indicates whether the targets have been allocated

    GLuint depthTexture;        //< Internal HW depth buffer
    uint32_t resW, resH;        //< Full internal resolution
    float txlW, txlH;           //< Size of a texel for full resolution

} R3D_MOD_TARGET;

// ========================================
// MODULE FUNCTIONS
// ========================================

/*
 * Module initialization function.
 * Called once during `R3D_Init()`
 */
bool r3d_target_init(int resW, int resH);

/*
 * Module deinitialization function.
 * Called once during `R3D_Close()`
 */
void r3d_target_quit(void);

/*
 * Resizes the internal resolution.
 * Performs a reallocation of all targets already allocated.
 * Ignore the operation if the new resolution is identical to the one already defined.
 */
void r3d_target_resize(uint32_t resW, uint32_t resH);

/*
 * Returns the minimum virtual mip level for the target.
 * Levels below this value don't exist for the target and will trigger an assert.
 * Example: a half-resolution target has a minimum level of 1 (its physical level 0
 * is exposed publicly as level 1, as if it were a mip of a full-resolution texture).
 */
int r3d_target_get_min_level(r3d_target_t target);

/*
 * Returns the maximum virtual mip level for the target.
 */
int r3d_target_get_max_level(r3d_target_t target);

/*
 * Returns the total of physical mip levels of the target.
 */
int r3d_target_get_num_levels(r3d_target_t target);

/*
 * Returns the internal resolution for the specified mip level.
 */
void r3d_target_get_resolution(int* w, int* h, int level);

/*
 * Returns the texel size for the specified mip level.
 */
Vector2 r3d_target_get_texel_size(int level);

/*
 * Returns target '1' if target '0' is provided, otherwise returns target '0'.
 */
r3d_target_t r3d_target_swap_scene(r3d_target_t scene);

/*
 * Creates, binds and clear the FBO with the requested attachment combination.
 * Attachment locations follow the order provided.
 *
 * This function attaches the targets at the specified level and sets the corresponding viewport.
 * Ensure that the provided target combination is compatible with the specified level.
 * The depth buffer can only be attached when the level is zero.
 */
void r3d_target_bind_clear(const r3d_target_t* targets, int count, int level, bool depth);

/*
 * Creates (or retrieves) and binds an FBO with the requested attachment combination.
 * Attachment locations follow the order of targets (COLOR0 = targets[0], etc).
 *
 * This function attaches the targets at the specified level and sets the corresponding viewport.
 * Ensure that the provided target combination is compatible with the specified level.
 * The depth buffer can only be attached when the level is zero.
 */
void r3d_target_bind_load(const r3d_target_t* targets, int count, int level, bool depth);

/*
 * Sets the viewport according to the specified level.
 */
void r3d_target_set_viewport(int level);

/*
 * Changes the mip level of the currently bound FBO.
 * Asserts that a valid FBO is currently bound and that the level is valid.
 */
void r3d_target_set_write_level(int level);

/*
 * Defines the sampling level of the target.
 * This function locks the sampling to a single level.
 * See `r3d_target_set_read_levels` for multi-level sampling.
 */
void r3d_target_set_read_level(r3d_target_t target, int level);

/*
 * Defines the sampling levels of the target.
 * baseLevel defines the first level and maxLevel the last.
 * Asserts that the target has already been created/used and that the levels are valid.
 */
void r3d_target_set_read_levels(r3d_target_t target, int baseLevel, int maxLevel);

/*
 * Generates mipmaps for the specified target.
 * Asserts that the target has already been created.
 */
void r3d_target_gen_mipmap(r3d_target_t target);

/*
 * Returns whether the requested target has a valid texture ID.
 * Returns false if the target has not been created yet or if the enum is invalid.
 */
bool r3d_target_exists(r3d_target_t target);

/*
 * Returns the texture ID corresponding to the requested target.
 * Asserts that the requested target has been created and if the target enum is valid.
 * If not created yet, it means we never bound this target, so it would be empty.
 */
GLuint r3d_target_get(r3d_target_t target);

/*
 * Returns the texture ID corresponding to the requested target.
 * Or returns 0 if the target has not been created or if the enum is invalid.
 */
GLuint r3d_target_get_or_null(r3d_target_t target);

/*
 * Returns the texture ID corresponding to the requested target and sampling level.
 * Asserts that the requested target has been created and that the level is valid.
 */
GLuint r3d_target_get_level(r3d_target_t target, int level);

/*
 * Returns the texture ID corresponding to the requested target with base and max levels configured.
 * Asserts that the requested target has been created and if the target enum is valid.
 * If not created yet, it means we never bound this target, so it would be empty.
 */
GLuint r3d_target_get_levels(r3d_target_t target, int baseLevel, int maxLevel);

/*
 * Returns the texture ID corresponding to the requested target all levels configured to be sampled.
 * Asserts that the requested target has been created and if the target enum is valid.
 * If not created yet, it means we never bound this target, so it would be empty.
 */
GLuint r3d_target_get_all_levels(r3d_target_t target);

/*
 * Returns the ID of the internal HW depth texture,
 * not the linear depth texture accessible via `r3d_target_get`.
 */
GLuint r3d_target_get_depth_buffer(void);

/*
 * Blits the provided targets to the specified FBO.
 * Supports blitting with only a depth target.
 */
void r3d_target_blit(r3d_target_t target, bool depth, GLuint dstFbo, int dstX, int dstY, int dstW, int dstH, bool linear);

/*
 * Invalidate the internal state cache as the FBO target currently binds.
 */
void r3d_target_invalidate_cache(void);

#endif // R3D_MODULE_TARGET_H
