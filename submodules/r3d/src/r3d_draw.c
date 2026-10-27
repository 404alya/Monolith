/* r3d_draw.h -- R3D Draw Module.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#include <r3d/r3d_draw.h>
#include <r3d/r3d_profiler.h>
#include <r3d_config.h>
#include <raymath.h>
#include <stddef.h>
#include <string.h>
#include <float.h>
#include <rlgl.h>
#include <glad.h>

#include "./r3d_core_state.h"

#include "./common/r3d_helper.h"
#include "./common/r3d_stack.h"
#include "./common/r3d_pass.h"
#include "./common/r3d_math.h"

#include "./modules/r3d_texture.h"
#include "./modules/r3d_driver.h"
#include "./modules/r3d_target.h"
#include "./modules/r3d_shader.h"
#include "./modules/r3d_render.h"
#include "./modules/r3d_light.h"
#include "./modules/r3d_env.h"

// ========================================
// HELPER MACROS
// ========================================

#define IS_MESH_VALID(mesh) \
    (((mesh).vertexCount > 0) && ((mesh).layerMask != 0))

#define IS_MESH_VISIBLE(mesh, cullMask) \
    (R3D_BIT_ANY((cullMask), (mesh).layerMask))

#define IS_MESH_VISIBLE_CAMERA(mesh) \
    (IS_MESH_VISIBLE((mesh), R3D.viewState.camera.cullMask))

#define SHADOW_CAST_ONLY_MASK (                 \
    (1 << R3D_SHADOW_CAST_ONLY_AUTO) |          \
    (1 << R3D_SHADOW_CAST_ONLY_DOUBLE_SIDED) |  \
    (1 << R3D_SHADOW_CAST_ONLY_FRONT_SIDE) |    \
    (1 << R3D_SHADOW_CAST_ONLY_BACK_SIDE)       \
)

#define IS_SHADOW_CAST_ONLY(mode)               \
    ((R3D_SHADOW_CAST_ONLY_MASK & (1 << (mode))) != 0)

// ========================================
// GPU PROFILER (local patch, see patches.md)
// ========================================

// Per-pass GL_TIME_ELAPSED queries around the passes of R3D_End(), plus GL_TIMESTAMP
// queries at its start and end. Each frame records into one slot of a ring; a slot is
// read back only once its last query is available, and before the ring wraps around,
// so the CPU doesn't wait on the GPU. Compiled in with -DR3D_GPU_PROFILER; without it
// PROF_* expand to nothing and the public functions are stubs.

#ifdef R3D_GPU_PROFILER

#define PROF_RING  4    //< frames in flight; drivers queue at most ~3, so a full ring never stalls
#define PROF_QUEUE 16   //< finished results waiting for R3D_PopProfilerFrame()

typedef struct {
    GLuint pass[R3D_PROFILER_PASS_COUNT];
    GLuint tsBegin, tsEnd;
    bool used[R3D_PROFILER_PASS_COUNT];
    bool pending;                       //< recorded, not read back yet
    uint64_t id;
    int tag;
} prof_slot_t;

static struct {
    bool enabled;
    bool created;
    bool recording;                     //< inside R3D_End() of a profiled frame
    int active;                         //< open pass or -1 (TIME_ELAPSED queries can't nest)
    int slot;                           //< slot of the current / last recorded frame
    int tag;
    uint64_t nextId;
    prof_slot_t slots[PROF_RING];
    R3D_ProfilerFrame queue[PROF_QUEUE];
    int queueHead, queueCount, dropped;
} PROF = { .active = -1, .tag = -1 };

static void prof_read_slot(prof_slot_t* s)
{
    R3D_ProfilerFrame f = { .id = s->id, .tag = s->tag };

    GLuint64 t0 = 0, t1 = 0;
    glGetQueryObjectui64v(s->tsBegin, GL_QUERY_RESULT, &t0);
    glGetQueryObjectui64v(s->tsEnd, GL_QUERY_RESULT, &t1);
    f.totalMs = (float)((double)(t1 - t0) * 1e-6);

    for (int i = 0; i < R3D_PROFILER_PASS_COUNT; i++) {
        if (!s->used[i]) continue;
        GLuint64 ns = 0;
        glGetQueryObjectui64v(s->pass[i], GL_QUERY_RESULT, &ns);
        f.passMs[i] = (float)((double)ns * 1e-6);
        f.ran[i] = true;
    }
    s->pending = false;

    if (PROF.queueCount == PROF_QUEUE) {
        PROF.queueHead = (PROF.queueHead + 1) % PROF_QUEUE;
        PROF.queueCount--;
        PROF.dropped++;
    }
    PROF.queue[(PROF.queueHead + PROF.queueCount) % PROF_QUEUE] = f;
    PROF.queueCount++;
}

// Reads back finished slots, oldest first; stops at the first unfinished one so results
// stay in order. Non-blocking unless 'wait' is set.
static void prof_collect(bool wait)
{
    for (int k = 1; k <= PROF_RING; k++) {
        prof_slot_t* s = &PROF.slots[(PROF.slot + k) % PROF_RING];
        if (!s->pending) continue;
        if (!wait) {
            GLuint available = GL_FALSE;
            glGetQueryObjectuiv(s->tsEnd, GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available) break;
        }
        prof_read_slot(s);
    }
}

static void prof_frame_begin(void)
{
    if (!PROF.enabled) return;

    if (!PROF.created) {
        for (int i = 0; i < PROF_RING; i++) {
            glGenQueries(R3D_PROFILER_PASS_COUNT, PROF.slots[i].pass);
            glGenQueries(1, &PROF.slots[i].tsBegin);
            glGenQueries(1, &PROF.slots[i].tsEnd);
        }
        PROF.created = true;
    }

    prof_collect(false);

    PROF.slot = (PROF.slot + 1) % PROF_RING;
    prof_slot_t* s = &PROF.slots[PROF.slot];
    if (s->pending) prof_read_slot(s);  //< ring full: only if the GPU is > PROF_RING frames behind

    memset(s->used, 0, sizeof(s->used));
    s->id = PROF.nextId++;
    s->tag = PROF.tag;
    glQueryCounter(s->tsBegin, GL_TIMESTAMP);
    PROF.recording = true;
}

static void prof_frame_end(void)
{
    if (!PROF.recording) return;
    prof_slot_t* s = &PROF.slots[PROF.slot];
    glQueryCounter(s->tsEnd, GL_TIMESTAMP);
    s->pending = true;
    PROF.recording = false;
}

static void prof_begin(R3D_ProfilerPass pass)
{
    if (!PROF.recording) return;
    prof_slot_t* s = &PROF.slots[PROF.slot];
    if (PROF.active >= 0 || s->used[pass]) return;  //< no nesting, one query per pass and frame
    glBeginQuery(GL_TIME_ELAPSED, s->pass[pass]);
    s->used[pass] = true;
    PROF.active = (int)pass;
}

static void prof_end(void)
{
    if (PROF.active < 0) return;
    glEndQuery(GL_TIME_ELAPSED);
    PROF.active = -1;
}

#define PROF_FRAME_BEGIN()  prof_frame_begin()
#define PROF_FRAME_END()    prof_frame_end()
#define PROF_BEGIN(pass)    prof_begin(pass)
#define PROF_END()          prof_end()

#else

#define PROF_FRAME_BEGIN()  ((void)0)
#define PROF_FRAME_END()    ((void)0)
#define PROF_BEGIN(pass)    ((void)0)
#define PROF_END()          ((void)0)

#endif // R3D_GPU_PROFILER

// Times one pass call: PROF_PASS(R3D_PROFILER_SSAO, ssaoSource = pass_prepare_ssao());
#define PROF_PASS(pass, ...) do { PROF_BEGIN(pass); __VA_ARGS__; PROF_END(); } while (0)

// ========================================
// INTERNAL FUNCTIONS
// ========================================

static void update_view_state(R3D_View view);
static void upload_light_array_block_for_mesh(const r3d_render_call_t* call, bool shadow);
static void upload_frame_block(void);
static void upload_view_block(void);
static void upload_env_block(void);
static void upload_fx_block(void);

static void raster_depth(const r3d_render_call_t* call, const Matrix* viewProj, const r3d_light_shadow_job_t* shadowJob);
static void raster_depth_cube(const r3d_render_call_t* call, const Matrix* viewProj, const r3d_light_shadow_job_t* shadowJob);
static void raster_probe_forward(const r3d_render_call_t* call, const r3d_env_probe_job_t* job, int face, bool opaque);
static void raster_probe_unlit(const r3d_render_call_t* call, const r3d_env_probe_job_t* job, int face, bool opaque);
// How a geometry-pass draw is rasterized (monolith patch: depth pre-pass)
typedef enum {
    GEOMETRY_RASTER_FULL,       //< Material depth state, full shading (upstream behaviour)
    GEOMETRY_RASTER_DEPTH_ONLY, //< Pre-pass: depth only, the fragment shader returns at once
    GEOMETRY_RASTER_EQUAL,      //< After the pre-pass: shade only the fragment that won the depth test
} geometry_raster_mode_t;

static bool geometry_call_prepassable(const r3d_render_call_t* call);
static void raster_geometry(const r3d_render_call_t* call, geometry_raster_mode_t mode);
static void raster_decal(const r3d_render_call_t* call);
static void raster_forward(const r3d_render_call_t* call);
static void raster_unlit(const r3d_render_call_t* call, bool opaque);

static void pass_scene_shadows(void);
static void pass_scene_shadow_minmax(void);
static void pass_scene_fog_sunvol(void);
static void pass_scene_probes(void);
static void pass_scene_geometry(void);
static void pass_scene_decals(void);

static void pass_prepare_pyramid(int maxLevel);
static void pass_prepare_downsample(r3d_target_t target, int level);
static r3d_target_t pass_prepare_ssao(void);
static r3d_target_t pass_prepare_ssil(void);
static r3d_target_t pass_prepare_ssgi(void);
static r3d_target_t pass_prepare_ssr(void);

static void pass_deferred_lights(void);
static void pass_deferred_ambient(r3d_target_t ssaoSource, r3d_target_t ssilSource, r3d_target_t ssgiSource, r3d_target_t composeTarget);
static void pass_deferred_compose(r3d_target_t sceneTarget, r3d_target_t ssrSource);
static void pass_deferred_fog(r3d_target_t sceneTarget);
static void pass_deferred_volumetric_fog(r3d_target_t sceneTarget);

static void pass_scene_forward(r3d_target_t sceneTarget);
static void pass_scene_background(r3d_target_t sceneTarget);

static r3d_target_t pass_post_setup(r3d_target_t sceneTarget);
static r3d_target_t pass_post_dof(r3d_target_t sceneTarget);
static r3d_target_t pass_post_bloom(r3d_target_t sceneTarget, bool apply);
static r3d_target_t pass_post_auto_exposure(r3d_target_t sceneTarget);
static r3d_target_t pass_post_screen(R3D_ScreenShaderStage stage, r3d_target_t sceneTarget);
static r3d_target_t pass_post_output(r3d_target_t sceneTarget, bool bloomFused);
static r3d_target_t pass_post_fxaa(r3d_target_t sceneTarget);
static bool final_pass_direct(void);
static void pass_post_fxaa_to_screen(r3d_target_t sceneTarget);
static r3d_target_t pass_post_smaa(r3d_target_t sceneTarget);

static void blit_to_screen(r3d_target_t source);
static void visualize_to_screen(r3d_target_t source);

static void cleanup_after_render(void);

// ========================================
// PUBLIC API
// ========================================

void R3D_Begin(Camera3D camera)
{
    R3D_BeginEx(R3D_CameraFromRL(camera));
}

void R3D_BeginEx(R3D_Camera camera)
{
    R3D_View view = {
        .camera = camera,
        .viewport = {0},
        .target = {0},
    };

    R3D_BeginPro(view);
}

void R3D_BeginPro(R3D_View view)
{
    rlDrawRenderBatchActive();
    update_view_state(view);
    R3D.screen = view.target;
    r3d_env_probe_clear();
    r3d_render_clear();
    r3d_light_clear();
}

void R3D_End(void)
{
    PROF_FRAME_BEGIN();

    r3d_render_prepare_drawing(); // bind global VAO

    /* --- Invalidates OpenGL cache and save some infos --- */

    r3d_driver_invalidate_cache();
    r3d_driver_store_viewport();

    /* --- Upload and bind uniform buffers --- */

    upload_frame_block();
    upload_view_block();
    upload_env_block();
    upload_fx_block();

    /* --- Render all shadow maps and bind them --- */

    if (r3d_light_has_shadow_job())
    {
        PROF_PASS(R3D_PROFILER_SHADOWS, pass_scene_shadows());
    }

    PROF_PASS(R3D_PROFILER_SHADOW_MINMAX, pass_scene_shadow_minmax());

    if (R3D_MOD_LIGHT.fogSunVol.request.pending)
    {
        pass_scene_fog_sunvol();
    }

    r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_SHADOW_DIR, r3d_light_shadow_map(R3D_LIGHT_DIR));
    r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_SHADOW_SPOT, r3d_light_shadow_map(R3D_LIGHT_SPOT));
    r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_SHADOW_OMNI, r3d_light_shadow_map(R3D_LIGHT_OMNI));

    /* --- Update all visible environment probes and render their cubemaps --- */

    if (r3d_env_has_any_probes() || R3D.environment.ambient.map.flags != 0)
    {
        r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_IBL_IRRADIANCE, r3d_env_irradiance_get());
        r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_IBL_PREFILTER, r3d_env_prefilter_get());
        r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_IBL_BRDF_LUT, r3d_texture_get(R3D_TEXTURE_BRDF_LUT));

        if (r3d_env_has_any_probe_jobs())
        {
            PROF_PASS(R3D_PROFILER_PROBES, pass_scene_probes());
        }
    }

    /* --- Cull groups and sort all draw calls before rendering --- */

    r3d_render_cull_groups(&R3D.viewState.frustum);

    r3d_render_sort_list(R3D_RENDER_LIST_OPAQUE, R3D.viewState.camera.position, R3D_RENDER_SORT_FRONT_TO_BACK);
    r3d_render_sort_list(R3D_RENDER_LIST_BLEND, R3D.viewState.camera.position, R3D_RENDER_SORT_BACK_TO_FRONT);
    r3d_render_sort_list(R3D_RENDER_LIST_DECAL, R3D.viewState.camera.position, R3D_RENDER_SORT_MATERIAL_ONLY);

    r3d_render_sort_list(R3D_RENDER_LIST_OPAQUE_INST, R3D.viewState.camera.position, R3D_RENDER_SORT_MATERIAL_ONLY);
    r3d_render_sort_list(R3D_RENDER_LIST_BLEND_INST, R3D.viewState.camera.position, R3D_RENDER_SORT_MATERIAL_ONLY);
    r3d_render_sort_list(R3D_RENDER_LIST_DECAL_INST, R3D.viewState.camera.position, R3D_RENDER_SORT_MATERIAL_ONLY);

    /* --- Clear all G-Buffer before writing in it --- */

    r3d_driver_set_depth_mask(GL_TRUE);
    r3d_driver_set_stencil_mask(0xFF);

    R3D_TARGET_BIND_CLEAR(0, true, R3D_TARGET_GBUFFER, R3D_TARGET_SPECULAR, R3D_TARGET_SCENE_0);

    /* --- Deferred path for opaques and decals --- */

    r3d_target_t sceneTarget = R3D_TARGET_SCENE_0;
    r3d_target_t ssaoSource  = R3D_TARGET_INVALID;
    r3d_target_t ssilSource  = R3D_TARGET_INVALID;
    r3d_target_t ssgiSource  = R3D_TARGET_INVALID;
    r3d_target_t ssrSource   = R3D_TARGET_INVALID;

    if (r3d_render_has_opaque())
    {
        PROF_PASS(R3D_PROFILER_GEOMETRY, pass_scene_geometry());

        if (r3d_render_has_decals()) PROF_PASS(R3D_PROFILER_DECALS, pass_scene_decals());
        if (r3d_light_has_visible()) PROF_PASS(R3D_PROFILER_LIGHTS, pass_deferred_lights());

        bool ssao = R3D.environment.ssao.enabled;
        bool ssil = R3D.environment.ssil.enabled;
        bool ssgi = R3D.environment.ssgi.enabled;
        bool ssr  = R3D.environment.ssr.enabled;
        bool vfog = R3D.environment.volumetricFog.enabled;
        bool dof  = R3D.environment.dof.mode;

        if (ssao || ssil || ssgi || ssr || vfog || dof)
        {
            PROF_BEGIN(R3D_PROFILER_PYRAMID);
            // Monolith patch 10: only SSR reads depth levels above 1 (SSAO, SSIL, SSGI, the
            // fog and DoF use levels 0 and 1), so without SSR the pyramid stops at level 1
            pass_prepare_pyramid(ssr ? r3d_target_get_max_level(R3D_TARGET_DEPTH) : 1);

            if (ssao || ssil || ssgi || ssr) pass_prepare_downsample(R3D_TARGET_NORMAL, 1);
            // skip ssr check for radiance, we re-downsample during ssr for the full ambient reflection
            if (ssil || ssgi) pass_prepare_downsample(R3D_TARGET_RADIANCE, 1);
            PROF_END();
        }

        if (ssao) PROF_PASS(R3D_PROFILER_SSAO, ssaoSource = pass_prepare_ssao());
        if (ssil) PROF_PASS(R3D_PROFILER_SSIL, ssilSource = pass_prepare_ssil());
        if (ssgi) PROF_PASS(R3D_PROFILER_SSGI, ssgiSource = pass_prepare_ssgi());
        // Local patch 26: without SSR (which reads the radiance after the ambient pass) and
        // with the scene as output (the debug outputs show RADIANCE / SPECULAR), the ambient
        // pass writes the composed scene itself; "compose" then reads 0 in the profiler
        bool fuseCompose = !ssr && R3D.outputMode == R3D_OUTPUT_SCENE;
        PROF_PASS(R3D_PROFILER_AMBIENT, pass_deferred_ambient(ssaoSource, ssilSource, ssgiSource,
                                                              fuseCompose ? sceneTarget : R3D_TARGET_INVALID));

        if (ssr) PROF_PASS(R3D_PROFILER_SSR, ssrSource = pass_prepare_ssr());
        if (!fuseCompose) PROF_PASS(R3D_PROFILER_COMPOSE, pass_deferred_compose(sceneTarget, ssrSource));
    }
    else
    {
        // And clear all depth levels, needed for next passes like DoF
        int numLevels = r3d_target_get_num_levels(R3D_TARGET_DEPTH);
        for (int i = 1; i < numLevels; i++)
        {
            R3D_TARGET_BIND_CLEAR(i, true, R3D_TARGET_DEPTH);
        }
    }

    /* --- Then background/fog and forward rendering --- */

    PROF_PASS(R3D_PROFILER_BACKGROUND, pass_scene_background(sceneTarget));

    // Local patch 24: with the volumetric fog on, its composition pass also applies the
    // distance fog and the transmittance (one blend into the scene instead of three)
    if (R3D.environment.volumetricFog.enabled)
    {
        pass_deferred_volumetric_fog(sceneTarget);
    }
    else if (R3D.environment.fog.mode != R3D_FOG_DISABLED)
    {
        PROF_PASS(R3D_PROFILER_FOG, pass_deferred_fog(sceneTarget));
    }

    PROF_PASS(R3D_PROFILER_FORWARD, pass_scene_forward(sceneTarget));

    /* --- Applying effects over the scene and final blit --- */

    sceneTarget = pass_post_setup(sceneTarget);
    sceneTarget = pass_post_screen(R3D_SCREEN_SHADER_STAGE_SCENE, sceneTarget);

    if (R3D.environment.dof.mode != R3D_DOF_DISABLED)
    {
        PROF_PASS(R3D_PROFILER_DOF, sceneTarget = pass_post_dof(sceneTarget));
    }

    // Monolith patch 10: the bloom composite is folded into the output (tonemap) pass when
    // nothing runs between the two (no auto exposure, no POST stage screen shaders)
    bool bloomOn = R3D.environment.bloom.mode != R3D_BLOOM_DISABLED;
    bool bloomFused = bloomOn && !R3D.environment.autoExposure.enabled;
    for (int i = 0; bloomFused && i < (int)R3D_ARRAY_SIZE(R3D.screenShaders[R3D_SCREEN_SHADER_STAGE_POST]); i++)
    {
        if (R3D.screenShaders[R3D_SCREEN_SHADER_STAGE_POST][i] != NULL) bloomFused = false;
    }

    if (bloomOn)
    {
        PROF_PASS(R3D_PROFILER_BLOOM, sceneTarget = pass_post_bloom(sceneTarget, !bloomFused));
    }

    if (R3D.environment.autoExposure.enabled)
    {
        PROF_PASS(R3D_PROFILER_AUTO_EXPOSURE, sceneTarget = pass_post_auto_exposure(sceneTarget));
    }

    sceneTarget = pass_post_screen(R3D_SCREEN_SHADER_STAGE_POST, sceneTarget);
    PROF_PASS(R3D_PROFILER_OUTPUT, sceneTarget = pass_post_output(sceneTarget, bloomFused));

    sceneTarget = pass_post_screen(R3D_SCREEN_SHADER_STAGE_OUTPUT, sceneTarget);

    // Monolith patch 10: FXAA renders straight into the final target when the final blit
    // would only copy its result (same size, scene output, no FINAL stage screen shaders)
    bool finalDone = false;

    switch (R3D.aaMode)
    {
    case R3D_ANTI_ALIASING_MODE_FXAA:
        if (final_pass_direct())
        {
            PROF_PASS(R3D_PROFILER_AA, pass_post_fxaa_to_screen(sceneTarget));
            finalDone = true;
        }
        else
        {
            PROF_PASS(R3D_PROFILER_AA, sceneTarget = pass_post_fxaa(sceneTarget));
        }
        break;
    case R3D_ANTI_ALIASING_MODE_SMAA:
        PROF_PASS(R3D_PROFILER_AA, sceneTarget = pass_post_smaa(sceneTarget));
        break;
    default:
        break;
    }

    sceneTarget = pass_post_screen(R3D_SCREEN_SHADER_STAGE_FINAL, sceneTarget);

    PROF_BEGIN(R3D_PROFILER_BLIT);
    switch (R3D.outputMode)
    {
    case R3D_OUTPUT_SCENE:    if (!finalDone) blit_to_screen(r3d_target_swap_scene(sceneTarget)); break;
    case R3D_OUTPUT_ALBEDO:   visualize_to_screen(R3D_TARGET_ALBEDO); break;
    case R3D_OUTPUT_NORMAL:   visualize_to_screen(R3D_TARGET_NORMAL); break;
    case R3D_OUTPUT_ORM:      visualize_to_screen(R3D_TARGET_ORM); break;
    case R3D_OUTPUT_RADIANCE: visualize_to_screen(R3D_TARGET_RADIANCE); break;
    case R3D_OUTPUT_SPECULAR: visualize_to_screen(R3D_TARGET_SPECULAR); break;
    case R3D_OUTPUT_SSAO:     visualize_to_screen(ssaoSource); break;
    case R3D_OUTPUT_SSIL:     visualize_to_screen(ssilSource); break;
    case R3D_OUTPUT_SSGI:     visualize_to_screen(ssgiSource); break;
    case R3D_OUTPUT_SSR:      visualize_to_screen(ssrSource); break;
    case R3D_OUTPUT_BLOOM:    visualize_to_screen(R3D_TARGET_BLOOM); break;
    case R3D_OUTPUT_DOF:      visualize_to_screen(R3D_TARGET_DOF_COC); break;
    }
    PROF_END();

    /* --- Reset internal stuff and states changed by R3D --- */

    r3d_stack_reset(R3D.stack);
    cleanup_after_render();

    PROF_FRAME_END();
}

void R3D_BeginCluster(BoundingBox aabb)
{
    if (!r3d_render_cluster_begin(aabb))
    {
        R3D_TRACELOG(LOG_WARNING, "Failed to begin cluster");
    }
}

void R3D_EndCluster(void)
{
    if (!r3d_render_cluster_end())
    {
        R3D_TRACELOG(LOG_WARNING, "Failed to end cluster");
    }
}

void R3D_PushLight(R3D_Light light)
{
    r3d_light_push(&light, NULL, false, NULL);
}

void R3D_PushLightEx(R3D_Light light, R3D_ShadowMap map, bool updateShadow)
{
    r3d_light_push(&light, &map, updateShadow, NULL);
}

void R3D_PushLightFixedShadow(R3D_Light light, R3D_ShadowMap map, Matrix lightViewProj, bool updateShadow)
{
    r3d_light_push(&light, &map, updateShadow, &lightViewProj);
}

static float fogSunVolBakeMs;   //< local patch, see pass_scene_fog_sunvol()

// Local patch 11: baked world-space occlusion volume, see R3D_SetWorldOcclusion()
static struct {
    GLuint texture;
    Vector3 min, invExtent;
    Vector2 decode;         //< distance = value * x + y (value in [0, 1])
    float intensity;
} worldAo;

void R3D_SetWorldOcclusion(const unsigned char *distance, int w, int h, int d, Vector3 min, float voxelSize,
                           float minDistance, float maxDistance, float intensity)
{
    if (!distance || w <= 0 || h <= 0 || d <= 0 || voxelSize <= 0.0f || maxDistance <= minDistance)
    {
        R3D_TRACELOG(LOG_WARNING, "R3D_SetWorldOcclusion: invalid volume");
        R3D_ClearWorldOcclusion();
        return;
    }

    if (worldAo.texture == 0) glGenTextures(1, &worldAo.texture);

    glBindTexture(GL_TEXTURE_3D, worldAo.texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    r3d_driver_clear_errors();
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, w, h, d, 0, GL_RED, GL_UNSIGNED_BYTE, distance);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (r3d_driver_check_error("World occlusion -> texture upload"))
    {
        glBindTexture(GL_TEXTURE_3D, 0);
        R3D_ClearWorldOcclusion();
        return;
    }
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_3D, 0);

    worldAo.min = min;
    worldAo.invExtent = (Vector3){ 1.0f / (voxelSize * w), 1.0f / (voxelSize * h), 1.0f / (voxelSize * d) };
    worldAo.decode = (Vector2){ maxDistance - minDistance, minDistance };
    worldAo.intensity = intensity;
}

void R3D_ClearWorldOcclusion(void)
{
    if (worldAo.texture != 0) glDeleteTextures(1, &worldAo.texture);
    memset(&worldAo, 0, sizeof(worldAo));
}

void R3D_BakeFogSunVisibility(R3D_ShadowMap map, Matrix lightViewProj, BoundingBox region, float voxelSize, int subsamples)
{
    r3d_light_request_fog_sunvol(&map, lightViewProj, region, voxelSize, subsamples);
}

void R3D_ClearFogSunVisibility(void)
{
    r3d_light_clear_fog_sunvol();
}

float R3D_GetFogSunVisibilityBakeTime(void)
{
    return fogSunVolBakeMs;
}

void R3D_PushProbe(R3D_Probe probe, bool updateProbe)
{
    r3d_env_push_probe(&probe, updateProbe);
}

void R3D_DrawMesh(R3D_Mesh mesh, R3D_Material material, Vector3 position, float scale)
{
    Matrix transform = r3d_matrix_st((Vector3) {scale, scale, scale}, position);
    R3D_DrawMeshPro(mesh, material, transform);
}

void R3D_DrawMeshEx(R3D_Mesh mesh, R3D_Material material, Vector3 position, Quaternion rotation, Vector3 scale)
{
    Matrix transform = r3d_matrix_srt_quat(scale, rotation, position);
    R3D_DrawMeshPro(mesh, material, transform);
}

void R3D_DrawMeshPro(R3D_Mesh mesh, R3D_Material material, Matrix transform)
{
    if (!IS_MESH_VALID(mesh)) return;

    r3d_render_group_t drawGroup = {0};
    drawGroup.transform = transform;
    drawGroup.obb = R3D_GetOrientedBox(mesh.aabb, transform);

    r3d_render_group_push(&drawGroup);

    r3d_render_call_t drawCall = {0};
    drawCall.type = R3D_RENDER_CALL_MESH;
    drawCall.mesh.material = material;
    drawCall.mesh.instance = mesh;

    r3d_render_call_push(&drawCall);
}

void R3D_DrawMeshInstanced(R3D_Mesh mesh, R3D_Material material, R3D_InstanceBuffer instances, int count)
{
    R3D_DrawMeshInstancedPro(mesh, material, instances, 0, count, R3D_MATRIX_IDENTITY);
}

void R3D_DrawMeshInstancedEx(R3D_Mesh mesh, R3D_Material material, R3D_InstanceBuffer instances, int offset, int count)
{
    R3D_DrawMeshInstancedPro(mesh, material, instances, offset, count, R3D_MATRIX_IDENTITY);
}

void R3D_DrawMeshInstancedPro(R3D_Mesh mesh, R3D_Material material, R3D_InstanceBuffer instances, int offset, int count, Matrix transform)
{
    if (count <= 0) return;
    if (!IS_MESH_VALID(mesh)) return;

    r3d_render_group_t drawGroup = {0};
    drawGroup.transform = transform;
    drawGroup.instances = instances;
    drawGroup.instanceOffset = R3D_CLAMP(offset, 0, instances.capacity);
    drawGroup.instanceCount = R3D_CLAMP(count, 0, instances.capacity - offset);

    r3d_render_group_push(&drawGroup);

    r3d_render_call_t drawCall = {0};
    drawCall.type = R3D_RENDER_CALL_MESH;
    drawCall.mesh.material = material;
    drawCall.mesh.instance = mesh;

    r3d_render_call_push(&drawCall);
}

void R3D_DrawModel(R3D_Model model, Vector3 position, float scale)
{
    Matrix transform = r3d_matrix_st((Vector3) {scale, scale, scale}, position);
    R3D_DrawModelPro(model, transform);
}

void R3D_DrawModelEx(R3D_Model model, Vector3 position, Quaternion rotation, Vector3 scale)
{
    Matrix transform = r3d_matrix_srt_quat(scale, rotation, position);
    R3D_DrawModelPro(model, transform);
}

void R3D_DrawModelPro(R3D_Model model, Matrix transform)
{
    r3d_render_group_t drawGroup = {0};
    drawGroup.transform = transform;
    drawGroup.obb = R3D_GetOrientedBox(model.aabb, transform);
    drawGroup.skinTexture = model.skeleton.skinTexture;

    r3d_render_group_push(&drawGroup);

    for (int i = 0; i < model.meshCount; i++)
    {
        const R3D_Mesh* mesh = &model.meshes[i];
        if (!IS_MESH_VALID(*mesh)) continue;

        r3d_render_call_t drawCall = {0};
        drawCall.type = R3D_RENDER_CALL_MESH;
        drawCall.mesh.material = model.materials[model.meshMaterials[i]];
        drawCall.mesh.instance = *mesh;

        r3d_render_call_push(&drawCall);
    }
}

void R3D_DrawModelInstanced(R3D_Model model, R3D_InstanceBuffer instances, int count)
{
    R3D_DrawModelInstancedPro(model, instances, 0, count, R3D_MATRIX_IDENTITY);
}

void R3D_DrawModelInstancedEx(R3D_Model model, R3D_InstanceBuffer instances, int offset, int count)
{
    R3D_DrawModelInstancedPro(model, instances, offset, count, R3D_MATRIX_IDENTITY);
}

void R3D_DrawModelInstancedPro(R3D_Model model, R3D_InstanceBuffer instances, int offset, int count, Matrix transform)
{
    if (count <= 0) return;

    r3d_render_group_t drawGroup = {0};
    drawGroup.transform = transform;
    drawGroup.skinTexture = model.skeleton.skinTexture;
    drawGroup.instances = instances;
    drawGroup.instanceOffset = R3D_CLAMP(offset, 0, instances.capacity);
    drawGroup.instanceCount = R3D_CLAMP(count, 0, instances.capacity - offset);

    r3d_render_group_push(&drawGroup);

    for (int i = 0; i < model.meshCount; i++)
    {
        const R3D_Mesh* mesh = &model.meshes[i];
        if (!IS_MESH_VALID(*mesh)) continue;

        r3d_render_call_t drawCall = {0};
        drawCall.type = R3D_RENDER_CALL_MESH;
        drawCall.mesh.material = model.materials[model.meshMaterials[i]];
        drawCall.mesh.instance = *mesh;

        r3d_render_call_push(&drawCall);
    }
}

void R3D_DrawAnimatedModel(R3D_Model model, R3D_AnimationPlayer player, Vector3 position, float scale)
{
    Matrix transform = r3d_matrix_st((Vector3) {scale, scale, scale}, position);
    R3D_DrawAnimatedModelPro(model, player, transform);
}

void R3D_DrawAnimatedModelEx(R3D_Model model, R3D_AnimationPlayer player, Vector3 position, Quaternion rotation, Vector3 scale)
{
    Matrix transform = r3d_matrix_srt_quat(scale, rotation, position);
    R3D_DrawAnimatedModelPro(model, player, transform);
}

void R3D_DrawAnimatedModelPro(R3D_Model model, R3D_AnimationPlayer player, Matrix transform)
{
    r3d_render_group_t drawGroup = {0};
    drawGroup.transform = transform;
    drawGroup.obb = R3D_GetOrientedBox(model.aabb, transform);

    drawGroup.skinTexture = (player.skinTexture > 0)
        ? player.skinTexture : model.skeleton.skinTexture;

    r3d_render_group_push(&drawGroup);

    for (int i = 0; i < model.meshCount; i++)
    {
        const R3D_Mesh* mesh = &model.meshes[i];
        if (!IS_MESH_VALID(*mesh)) continue;

        r3d_render_call_t drawCall = {0};
        drawCall.type = R3D_RENDER_CALL_MESH;
        drawCall.mesh.material = model.materials[model.meshMaterials[i]];
        drawCall.mesh.instance = *mesh;

        r3d_render_call_push(&drawCall);
    }
}

void R3D_DrawAnimatedModelInstanced(R3D_Model model, R3D_AnimationPlayer player, R3D_InstanceBuffer instances, int count)
{
    R3D_DrawAnimatedModelInstancedPro(model, player, instances, 0, count, R3D_MATRIX_IDENTITY);
}

void R3D_DrawAnimatedModelInstancedEx(R3D_Model model, R3D_AnimationPlayer player, R3D_InstanceBuffer instances, int offset, int count)
{
    R3D_DrawAnimatedModelInstancedPro(model, player, instances, offset, count, R3D_MATRIX_IDENTITY);
}

void R3D_DrawAnimatedModelInstancedPro(R3D_Model model, R3D_AnimationPlayer player, R3D_InstanceBuffer instances, int offset, int count, Matrix transform)
{
    if (count <= 0) return;

    r3d_render_group_t drawGroup = {0};
    drawGroup.transform = transform;
    drawGroup.instances = instances;
    drawGroup.instanceOffset = R3D_CLAMP(offset, 0, instances.capacity);
    drawGroup.instanceCount = R3D_CLAMP(count, 0, instances.capacity - offset);

    drawGroup.skinTexture = (player.skinTexture > 0)
        ? player.skinTexture : model.skeleton.skinTexture;

    r3d_render_group_push(&drawGroup);

    for (int i = 0; i < model.meshCount; i++)
    {
        const R3D_Mesh* mesh = &model.meshes[i];
        if (!IS_MESH_VALID(*mesh)) continue;

        r3d_render_call_t drawCall = {0};
        drawCall.type = R3D_RENDER_CALL_MESH;
        drawCall.mesh.material = model.materials[model.meshMaterials[i]];
        drawCall.mesh.instance = *mesh;

        r3d_render_call_push(&drawCall);
    }
}

void R3D_DrawDecal(R3D_Decal decal, Vector3 position, float scale)
{
    Matrix transform = r3d_matrix_st((Vector3) {scale, scale, scale}, position);
    R3D_DrawDecalPro(decal, transform);
}

void R3D_DrawDecalEx(R3D_Decal decal, Vector3 position, Quaternion rotation, Vector3 scale)
{
    Matrix transform = r3d_matrix_srt_quat(scale, rotation, position);
    R3D_DrawDecalPro(decal, transform);
}

void R3D_DrawDecalPro(R3D_Decal decal, Matrix transform)
{
    decal.normalThreshold = (decal.normalThreshold == 0.0) ? PI * 2 : decal.normalThreshold * DEG2RAD;
    decal.fadeWidth = decal.fadeWidth * DEG2RAD;

    r3d_render_group_t drawGroup = {0};
    drawGroup.transform = transform;
    drawGroup.obb = R3D_GetOrientedBox(R3D_AABB_UNIT, transform);

    r3d_render_group_push(&drawGroup);

    r3d_render_call_t drawCall = {0};
    drawCall.type = R3D_RENDER_CALL_DECAL;
    drawCall.decal.instance = decal;

    r3d_render_call_push(&drawCall);
}

void R3D_DrawDecalInstanced(R3D_Decal decal, R3D_InstanceBuffer instances, int count)
{
    R3D_DrawDecalInstancedPro(decal, instances, 0, count, R3D_MATRIX_IDENTITY);
}

void R3D_DrawDecalInstancedEx(R3D_Decal decal, R3D_InstanceBuffer instances, int offset, int count)
{
    R3D_DrawDecalInstancedPro(decal, instances, offset, count, R3D_MATRIX_IDENTITY);
}

void R3D_DrawDecalInstancedPro(R3D_Decal decal, R3D_InstanceBuffer instances, int offset, int count, Matrix transform)
{
    if (count <= 0) return;

    decal.normalThreshold = (decal.normalThreshold == 0.0) ? PI * 2 : decal.normalThreshold * DEG2RAD;
    decal.fadeWidth = decal.fadeWidth * DEG2RAD;

    r3d_render_group_t drawGroup = {0};
    drawGroup.transform = transform;
    drawGroup.instances = instances;
    drawGroup.instanceOffset = R3D_CLAMP(offset, 0, instances.capacity);
    drawGroup.instanceCount = R3D_CLAMP(count, 0, instances.capacity - offset);

    r3d_render_group_push(&drawGroup);

    r3d_render_call_t drawCall = {0};
    drawCall.type = R3D_RENDER_CALL_DECAL;
    drawCall.decal.instance = decal;

    r3d_render_call_push(&drawCall);
}

// ========================================
// INTERNAL FUNCTIONS
// ========================================

static inline bool view_has_target(RenderTexture target)
{
    return target.id != 0 && target.texture.id != 0;
}

static void view_get_target_size(RenderTexture target, int* width, int* height)
{
    if (view_has_target(target))
    {
        *width  = target.texture.width;
        *height = target.texture.height;
    }
    else
    {
        *width  = GetRenderWidth();
        *height = GetRenderHeight();
    }

    if (*width  <= 0) *width  = 1;
    if (*height <= 0) *height = 1;
}

static Rectangle view_resolve_viewport(R3D_View view)
{
    int targetW = 1;
    int targetH = 1;

    view_get_target_size(view.target, &targetW, &targetH);

    if (view.viewport.width <= 0.0f || view.viewport.height <= 0.0f)
    {
        return (Rectangle) {
            0.0f,
            0.0f,
            (float)targetW,
            (float)targetH
        };
    }

    return view.viewport;
}

static Rectangle view_fit_aspect(Rectangle rect, double aspect)
{
    if (rect.width <= 0.0f || rect.height <= 0.0f || aspect <= 0.0)
    {
        return rect;
    }

    double rectAspect = (double)rect.width / (double)rect.height;

    if (aspect > rectAspect) {
        float newH = (float)((double)rect.width / aspect);
        rect.y += (rect.height - newH) * 0.5f;
        rect.height = newH;
    }
    else {
        float newW = (float)((double)rect.height * aspect);
        rect.x += (rect.width - newW) * 0.5f;
        rect.width = newW;
    }

    return rect;
}

static Rectangle view_resolve_present_rect(R3D_View view)
{
    Rectangle viewport = view_resolve_viewport(view);

    switch (R3D.aspectMode)
    {
    case R3D_ASPECT_KEEP:
    {
        int srcW = 1;
        int srcH = 1;

        r3d_target_get_resolution(&srcW, &srcH, 0);

        if (srcW <= 0) srcW = 1;
        if (srcH <= 0) srcH = 1;

        double srcAspect = (double)srcW / (double)srcH;
        return view_fit_aspect(viewport, srcAspect);
    }

    case R3D_ASPECT_EXPAND:
    default:
        return viewport;
    }
}

void update_view_state(R3D_View view)
{
    Rectangle viewport = view_resolve_present_rect(view);

    double aspect = 1.0;
    if (viewport.height > 0.0f)
    {
        aspect = (double)viewport.width / (double)viewport.height;
    }

    Matrix matView = R3D_GetCameraView(view.camera);
    Matrix matProj = R3D_GetCameraProjection(view.camera, aspect);
    Matrix matViewProj = MatrixMultiply(matView, matProj);

    R3D.viewState.camera = view.camera;
    R3D.viewState.viewport = viewport;
    R3D.viewState.frustum = R3D_ComputeFrustum(matViewProj);
    R3D.viewState.view = matView;
    R3D.viewState.proj = matProj;
    R3D.viewState.invView = MatrixInvert(matView);
    R3D.viewState.invProj = MatrixInvert(matProj);
    R3D.viewState.viewProj = matViewProj;
    R3D.viewState.aspect = aspect;
}

void upload_light_array_block_for_mesh(const r3d_render_call_t* call, bool shadow)
{
    R3D_ASSERT(call->type == R3D_RENDER_CALL_MESH); //< Paranoid assert, should be fine

    r3d_shader_block_light_array_t lights = {0};

    R3D_LIGHT_FOR_EACH_VISIBLE(light)
    {
        // Check if the geometry touches the light volume
        if (light->type != R3D_LIGHT_DIR)
        {
            if (!CheckCollisionBoxSphere(call->mesh.instance.aabb, light->volume.center, light->volume.radius))
            {
                continue;
            }
        }

        r3d_shader_block_light_t* data = &lights.uLights[lights.uNumLights];
        data->viewProj        = MatrixTranspose(light->viewProj);
        data->color           = light->color;
        data->position        = light->position;
        data->direction       = light->direction;
        data->energy          = light->energy;
        data->specular        = light->specular;
        data->range           = light->range;
        data->falloff         = light->falloff;
        data->innerCutOff     = light->innerCutOff;
        data->outerCutOff     = light->outerCutOff;
        data->fogEnergy       = light->fogEnergy;
        data->shadowSoftness  = light->shadowSoftness;
        data->shadowOpacity   = light->shadowOpacity;
        data->shadowDepthBias = light->shadowDepthBias;
        data->shadowSlopeBias = light->shadowSlopeBias;
        data->shadowFar       = light->shadowFar;
        data->shadowLayer     = shadow ? light->shadowLayer : -1;
        data->type            = light->type;
        data->clipMin         = light->clipMin;
        data->clipMax         = light->clipMax;

        if (++lights.uNumLights == R3D_HINT(R3D_HINT_FORWARD_LIGHT_PER_MESH))
        {
            break;
        }
    }

    r3d_shader_set_uniform_block(R3D_SHADER_BLOCK_LIGHT_ARRAY, &lights, true);
}

void upload_frame_block(void)
{
    static int frameIndex = 0;

    r3d_shader_block_frame_t frame = {
        .screenSize = (Vector2) {(float)R3D_TARGET_SIZE_W, (float)R3D_TARGET_SIZE_H},
        .texelSize = (Vector2) {R3D_TARGET_TEXEL_W, R3D_TARGET_TEXEL_H},
        .time = (float)GetTime(),
        .index = frameIndex++,
    };

    r3d_shader_set_uniform_block(R3D_SHADER_BLOCK_FRAME, &frame, false);
}

void upload_view_block(void)
{
    r3d_shader_block_view_t view = {
        .position = R3D.viewState.camera.position,
        .view = MatrixTranspose(R3D.viewState.view),
        .invView = MatrixTranspose(R3D.viewState.invView),
        .proj = MatrixTranspose(R3D.viewState.proj),
        .invProj = MatrixTranspose(R3D.viewState.invProj),
        .viewProj = MatrixTranspose(R3D.viewState.viewProj),
        .projMode = R3D.viewState.camera.projection,
        .aspect = (float)R3D.viewState.aspect,
        .near = (float)R3D.viewState.camera.nearPlane,
        .far = (float)R3D.viewState.camera.farPlane,
    };

    r3d_shader_set_uniform_block(R3D_SHADER_BLOCK_VIEW, &view, false);
}

void upload_env_block(void)
{
    R3D_STACK_SCOPE(&R3D.stack, sizeof(r3d_shader_block_env_t))
    {
        const R3D_EnvBackground* background = &R3D.environment.background;
        const R3D_EnvAmbient* ambient = &R3D.environment.ambient;

        r3d_shader_block_env_t* env = r3d_stack_alloc(&R3D.stack, sizeof(r3d_shader_block_env_t));

        int iIlluminationProbe = 0;
        R3D_ENV_FOR_EACH_ILLUMINATION_PROBE(probe)
        {
            env->uIlluminationProbes[iIlluminationProbe] = (r3d_shader_block_env_probe_t) {
                .position = probe->position,
                .falloff  = probe->falloff,
                .range    = probe->range,
                .layer    = (int)probe->handle - 1,
            };
            if (++iIlluminationProbe >= R3D_HINT(R3D_HINT_PROBE_ILLUMINATION_MAX_ACTIVE))
            {
                break;
            }
        }

        int iReflectionProbe = 0;
        R3D_ENV_FOR_EACH_REFLECTION_PROBE(probe)
        {
            env->uReflectionProbes[iReflectionProbe] = (r3d_shader_block_env_probe_t) {
                .position = probe->position,
                .falloff  = probe->falloff,
                .range    = probe->range,
                .layer    = (int)probe->handle - 1,
            };
            if (++iReflectionProbe >= R3D_HINT(R3D_HINT_PROBE_REFLECTION_MAX_ACTIVE))
            {
                break;
            }
        }

        env->uAmbient.rotation   = background->rotation;
        env->uAmbient.color      = r3d_color_srgb_to_linear_vec4(ambient->color);
        env->uAmbient.energy     = ambient->energy;
        env->uAmbient.irradiance = (int)ambient->map.irradiance - 1;
        env->uAmbient.prefilter  = (int)ambient->map.prefilter - 1;

        env->uNumIlluminationProbes = iIlluminationProbe;
        env->uNumReflectionProbes   = iReflectionProbe;
        env->uNumPrefilterLevels    = r3d_get_mip_levels_1d(R3D_HINT(R3D_HINT_IBL_PREFILTER_SIZE));

        r3d_shader_set_uniform_block(R3D_SHADER_BLOCK_ENV, env, false);
    }
}

void upload_fx_block(void)
{
    const R3D_Environment* env = &R3D.environment;

    r3d_shader_block_fx_t block = {0};

    if (env->ssao.enabled)
    {
        block.uSsao.sampleCount = env->ssao.sampleCount;
        block.uSsao.intensity   = env->ssao.intensity;
        block.uSsao.power       = env->ssao.power;
        block.uSsao.radius      = env->ssao.radius;
        block.uSsao.bias        = env->ssao.bias;
        block.uSsao.enabled     = env->ssao.enabled;
    }

    if (env->ssil.enabled)
    {
        block.uSsil.sampleCount = env->ssil.sampleCount;
        block.uSsil.giIntensity = env->ssil.giIntensity;
        block.uSsil.aoIntensity = env->ssil.aoIntensity;
        block.uSsil.aoPower     = env->ssil.aoPower;
        block.uSsil.radius      = env->ssil.radius;
        block.uSsil.bias        = env->ssil.bias;
        block.uSsil.enabled     = env->ssil.enabled;
    }

    if (env->ssgi.enabled)
    {
        block.uSsgi.sliceCount      = env->ssgi.sliceCount;
        block.uSsgi.edgeFade        = env->ssgi.edgeFade;
        block.uSsgi.distanceFalloff = env->ssgi.distanceFalloff;
        block.uSsgi.normalRejection = env->ssgi.normalRejection;
        block.uSsgi.intensity       = env->ssgi.intensity;
        block.uSsgi.enabled         = env->ssgi.enabled;
    }

    if (env->ssr.enabled)
    {
        block.uSsr.maxLevel      = r3d_target_get_num_levels(R3D_TARGET_SSR) - 1;
        block.uSsr.maxIterations = env->ssr.maxIterations;
        block.uSsr.thickness     = env->ssr.thickness;
        block.uSsr.edgeFade      = env->ssr.edgeFade;
        block.uSsr.enabled       = env->ssr.enabled;
    }

    if (env->fog.mode != R3D_FOG_DISABLED)
    {
        block.uFog.color     = r3d_color_srgb_to_linear_vec4(env->fog.color);
        block.uFog.start     = env->fog.start;
        block.uFog.end       = env->fog.end;
        block.uFog.density   = env->fog.density;
        block.uFog.skyAffect = env->fog.skyAffect;
        block.uFog.mode      = env->fog.mode;
    }

    if (env->volumetricFog.enabled)
    {
        block.uVFog.scatteringColor   = r3d_color_srgb_to_linear_vec4(env->volumetricFog.scatteringColor);
        block.uVFog.emissionColor     = r3d_color_srgb_to_linear_vec4(env->volumetricFog.emissionColor);
        block.uVFog.scatteringDensity = env->volumetricFog.scatteringDensity;
        block.uVFog.absortionDensity  = env->volumetricFog.absortionDensity;
        block.uVFog.anisotropy        = env->volumetricFog.anisotropy;
        block.uVFog.emissionEnergy    = env->volumetricFog.emissionEnergy;
        block.uVFog.skyAffect         = env->volumetricFog.skyAffect;
        block.uVFog.maxDistance       = env->volumetricFog.length;
        block.uVFog.stepSize          = env->volumetricFog.stepSize;
        block.uVFog.enabled           = env->volumetricFog.enabled;
        block.uVFog.fadeStart         = env->volumetricFog.fadeStart;   // local patch 13
        block.uVFog.fadeEnd           = env->volumetricFog.fadeEnd;
        block.uVFog.fadeNear          = env->volumetricFog.fadeNear;    // local patch 15
    }

    if (env->dof.mode != R3D_DOF_DISABLED)
    {
        block.uDof.focusPoint  = env->dof.focusPoint;
        block.uDof.focusScale  = env->dof.focusScale;
        block.uDof.nearScale   = env->dof.nearScale;
        block.uDof.maxBlurSize = env->dof.maxBlurSize * 0.5f;
        block.uDof.mode        = env->dof.mode;
    }

    if (env->bloom.mode != R3D_BLOOM_DISABLED)
    {
        float knee = env->bloom.threshold * env->bloom.softThreshold;
        block.uBloom.prefilter.x = env->bloom.threshold;
        block.uBloom.prefilter.y = env->bloom.threshold - knee;
        block.uBloom.prefilter.z = 2.0f * knee;
        block.uBloom.prefilter.w = 0.25f / (knee + 0.00001f);
        block.uBloom.intensity   = env->bloom.intensity;
        block.uBloom.mode        = env->bloom.mode;
    }

    block.uTonemap.mode     = env->tonemap.mode;
    block.uTonemap.exposure = env->tonemap.exposure;
    block.uTonemap.white    = env->tonemap.white;

    block.uBcs.brightness = env->color.brightness;
    block.uBcs.contrast   = env->color.contrast;
    block.uBcs.saturation = env->color.saturation;

    r3d_shader_set_uniform_block(R3D_SHADER_BLOCK_FX, &block, false);
}

void raster_depth(const r3d_render_call_t* call, const Matrix* viewProj, const r3d_light_shadow_job_t* shadowJob)
{
    R3D_ASSERT(call->type == R3D_RENDER_CALL_MESH); //< Paranoid assert, should be fine
    R3D_ASSERT(shadowJob != NULL);                  //< Only used for shadow maps

    const r3d_render_group_t* group = r3d_render_get_call_group(call);
    const R3D_Material* material = &call->mesh.material;
    const R3D_Mesh* mesh = &call->mesh.instance;

    /* --- Use shader --- */

    R3D_SurfaceShader* shader = call->mesh.material.shader;
    R3D_SHADER_USE_SELECT(scene.depth, shader);

    /* --- Send matrices --- */

    R3D_SHADER_SET_MAT4_SELECT(scene.depth, shader, uMatModel, group->transform);
    R3D_SHADER_SET_MAT4_SELECT(scene.depth, shader, uMatViewProj, *viewProj);

    /* --- Send skinning related data --- */

    if (group->skinTexture > 0)
    {
        R3D_SHADER_BIND_SAMPLER_SELECT(scene.depth, shader, uBoneMatricesTex, group->skinTexture);
        R3D_SHADER_SET_INT_SELECT(scene.depth, shader, uSkinning, true);
    }
    else
    {
        R3D_SHADER_SET_INT_SELECT(scene.depth, shader, uSkinning, false);
    }

    /* --- Send billboard related data --- */

    R3D_SHADER_SET_INT_SELECT(scene.depth, shader, uBillboard, material->billboardMode);
    if (material->billboardMode != R3D_BILLBOARD_DISABLED)
    {
        R3D_SHADER_SET_MAT4_SELECT(scene.depth, shader, uMatInvView, R3D.viewState.invView);
    }

    /* --- Set texcoord offset/scale --- */

    R3D_SHADER_SET_VEC2_SELECT(scene.depth, shader, uTexCoordOffset, material->uvOffset);
    R3D_SHADER_SET_VEC2_SELECT(scene.depth, shader, uTexCoordScale, material->uvScale);

    /* --- Set transparency material data --- */

    R3D_SHADER_BIND_SAMPLER_SELECT(scene.depth, shader, uAlbedoMap, R3D_TEXTURE_SELECT(material->albedo.texture.id, WHITE));
    R3D_SHADER_SET_COL4_SELECT(scene.depth, shader, uAlbedoColor, material->albedo.color);
    R3D_SHADER_SET_FLOAT_SELECT(scene.depth, shader, uAlphaCutoff, material->alphaCutoff);

    /* --- Applying material parameters that are independent of shaders --- */

    r3d_driver_set_shadow_cast_mode(mesh->shadowCastMode, material->cullMode);

    /* --- Rendering the object corresponding to the draw call --- */

    if (r3d_render_has_instances(group))
    {
        R3D_SHADER_SET_INT_SELECT(scene.depth, shader, uInstancing, true);
        r3d_render_draw_instanced(call);
    }
    else
    {
        R3D_SHADER_SET_INT_SELECT(scene.depth, shader, uInstancing, false);
        r3d_render_draw(call);
    }
}

void raster_depth_cube(const r3d_render_call_t* call, const Matrix* viewProj, const r3d_light_shadow_job_t* shadowJob)
{
    R3D_ASSERT(call->type == R3D_RENDER_CALL_MESH); //< Paranoid assert, should be fine
    R3D_ASSERT(shadowJob != NULL);                  //< Only used for shadow maps

    const r3d_render_group_t* group = r3d_render_get_call_group(call);
    const R3D_Material* material = &call->mesh.material;
    const R3D_Mesh* mesh = &call->mesh.instance;

    /* --- Use shader --- */

    R3D_SurfaceShader* shader = call->mesh.material.shader;
    R3D_SHADER_USE_SELECT(scene.depthCube, shader);

    /* --- Set shadow related data --- */

    R3D_SHADER_SET_FLOAT_SELECT(scene.depthCube, shader, uFar, shadowJob->far);
    R3D_SHADER_SET_VEC3_SELECT(scene.depthCube, shader, uViewPosition, shadowJob->position);

    /* --- Send matrices --- */

    R3D_SHADER_SET_MAT4_SELECT(scene.depthCube, shader, uMatModel, group->transform);
    R3D_SHADER_SET_MAT4_SELECT(scene.depthCube, shader, uMatViewProj, *viewProj);

    /* --- Send skinning related data --- */

    if (group->skinTexture > 0)
    {
        R3D_SHADER_BIND_SAMPLER_SELECT(scene.depthCube, shader, uBoneMatricesTex, group->skinTexture);
        R3D_SHADER_SET_INT_SELECT(scene.depthCube, shader, uSkinning, true);
    }
    else
    {
        R3D_SHADER_SET_INT_SELECT(scene.depthCube, shader, uSkinning, false);
    }

    /* --- Send billboard related data --- */

    R3D_SHADER_SET_INT_SELECT(scene.depthCube, shader, uBillboard, material->billboardMode);
    if (material->billboardMode != R3D_BILLBOARD_DISABLED)
    {
        R3D_SHADER_SET_MAT4_SELECT(scene.depthCube, shader, uMatInvView, R3D.viewState.invView);
    }

    /* --- Set texcoord offset/scale --- */

    R3D_SHADER_SET_VEC2_SELECT(scene.depthCube, shader, uTexCoordOffset, material->uvOffset);
    R3D_SHADER_SET_VEC2_SELECT(scene.depthCube, shader, uTexCoordScale, material->uvScale);

    /* --- Set transparency material data --- */

    R3D_SHADER_BIND_SAMPLER_SELECT(scene.depthCube, shader, uAlbedoMap, R3D_TEXTURE_SELECT(material->albedo.texture.id, WHITE));
    R3D_SHADER_SET_COL4_SELECT(scene.depthCube, shader, uAlbedoColor, material->albedo.color);
    R3D_SHADER_SET_FLOAT_SELECT(scene.depthCube, shader, uAlphaCutoff, material->alphaCutoff);

    /* --- Applying material parameters that are independent of shaders --- */

    r3d_driver_set_shadow_cast_mode(mesh->shadowCastMode, material->cullMode);

    /* --- Rendering the object corresponding to the draw call --- */

    if (r3d_render_has_instances(group))
    {
        R3D_SHADER_SET_INT_SELECT(scene.depthCube, shader, uInstancing, true);
        r3d_render_draw_instanced(call);
    }
    else
    {
        R3D_SHADER_SET_INT_SELECT(scene.depthCube, shader, uInstancing, false);
        r3d_render_draw(call);
    }
}

void raster_probe_forward(const r3d_render_call_t* call, const r3d_env_probe_job_t* job, int face, bool opaque)
{
    R3D_ASSERT(call->type == R3D_RENDER_CALL_MESH); //< Paranoid assert, should be fine

    const r3d_render_group_t* group = r3d_render_get_call_group(call);
    const R3D_Material* material = &call->mesh.material;

    /* --- Use shader --- */

    R3D_SurfaceShader* shader = call->mesh.material.shader;
    R3D_SHADER_USE_SELECT(scene.probeForward, shader);

    /* --- Set probe related data --- */

    R3D_SHADER_SET_VEC3_SELECT(scene.probeForward, shader, uViewPosition, job->position);
    R3D_SHADER_SET_INT_SELECT(scene.probeForward, shader, uProbeInterior, job->interior);

    /* --- Send matrices --- */

    Matrix matNormal = r3d_matrix_normal(&group->transform);

    R3D_SHADER_SET_MAT4_SELECT(scene.probeForward, shader, uMatModel, group->transform);
    R3D_SHADER_SET_MAT4_SELECT(scene.probeForward, shader, uMatNormal, matNormal);
    R3D_SHADER_SET_MAT4_SELECT(scene.probeForward, shader, uMatView, job->view[face]);
    R3D_SHADER_SET_MAT4_SELECT(scene.probeForward, shader, uMatInvView, job->invView[face]);
    R3D_SHADER_SET_MAT4_SELECT(scene.probeForward, shader, uMatViewProj, job->viewProj[face]);

    /* --- Send skinning related data --- */

    if (group->skinTexture > 0)
    {
        R3D_SHADER_BIND_SAMPLER_SELECT(scene.probeForward, shader, uBoneMatricesTex, group->skinTexture);
        R3D_SHADER_SET_INT_SELECT(scene.probeForward, shader, uSkinning, true);
    }
    else
    {
        R3D_SHADER_SET_INT_SELECT(scene.probeForward, shader, uSkinning, false);
    }

    /* --- Send billboard related data --- */

    R3D_SHADER_SET_INT_SELECT(scene.probeForward, shader, uBillboard, material->billboardMode);

    /* --- Set factor material maps --- */

    bool hybrid = (material->transparencyMode == R3D_TRANSPARENCY_HYBRID);
    float cutoffSign = opaque ? 1.0f : (hybrid ? -1.0f : 0.0f);

    R3D_SHADER_SET_FLOAT_SELECT(scene.probeForward, shader, uEmissionEnergy, material->emission.energy);
    R3D_SHADER_SET_FLOAT_SELECT(scene.probeForward, shader, uAlphaCutoff, material->alphaCutoff);
    R3D_SHADER_SET_FLOAT_SELECT(scene.probeForward, shader, uCutoffSign, cutoffSign);
    R3D_SHADER_SET_FLOAT_SELECT(scene.probeForward, shader, uNormalScale, material->normal.scale);
    R3D_SHADER_SET_FLOAT_SELECT(scene.probeForward, shader, uOcclusion, Clamp(material->orm.occlusion, 0.0f, 1.0f));
    R3D_SHADER_SET_FLOAT_SELECT(scene.probeForward, shader, uRoughness, Clamp(material->orm.roughness, 0.0f, 1.0f));
    R3D_SHADER_SET_FLOAT_SELECT(scene.probeForward, shader, uMetalness, Clamp(material->orm.metalness, 0.0f, 1.0f));
    R3D_SHADER_SET_FLOAT_SELECT(scene.probeForward, shader, uSpecular, Clamp(material->orm.specular, 0.0f, 1.0f));

    /* --- Set texcoord offset/scale --- */

    R3D_SHADER_SET_VEC2_SELECT(scene.probeForward, shader, uTexCoordOffset, material->uvOffset);
    R3D_SHADER_SET_VEC2_SELECT(scene.probeForward, shader, uTexCoordScale, material->uvScale);

    /* --- Set color material maps --- */

    R3D_SHADER_SET_COL4_SELECT(scene.probeForward, shader, uAlbedoColor, material->albedo.color);
    R3D_SHADER_SET_COL3_SELECT(scene.probeForward, shader, uEmissionColor, material->emission.color);

    /* --- Bind active texture maps --- */

    R3D_SHADER_BIND_SAMPLER_SELECT(scene.probeForward, shader, uAlbedoMap, R3D_TEXTURE_SELECT(material->albedo.texture.id, WHITE));
    R3D_SHADER_BIND_SAMPLER_SELECT(scene.probeForward, shader, uNormalMap, R3D_TEXTURE_SELECT(material->normal.texture.id, NORMAL));
    R3D_SHADER_BIND_SAMPLER_SELECT(scene.probeForward, shader, uEmissionMap, R3D_TEXTURE_SELECT(material->emission.texture.id, WHITE));
    R3D_SHADER_BIND_SAMPLER_SELECT(scene.probeForward, shader, uOrmMap, R3D_TEXTURE_SELECT(material->orm.texture.id, WHITE));

    /* --- Applying material parameters that are independent of shaders --- */

    r3d_driver_set_depth_state(material->depth);
    r3d_driver_set_stencil_state(material->stencil);
    r3d_driver_set_blend_mode(material->blendMode);
    r3d_driver_set_cull_mode(material->cullMode);

    /* --- Rendering the object corresponding to the draw call --- */

    if (r3d_render_has_instances(group))
    {
        R3D_SHADER_SET_INT_SELECT(scene.probeForward, shader, uInstancing, true);
        r3d_render_draw_instanced(call);
    }
    else
    {
        R3D_SHADER_SET_INT_SELECT(scene.probeForward, shader, uInstancing, false);
        r3d_render_draw(call);
    }
}

void raster_probe_unlit(const r3d_render_call_t* call, const r3d_env_probe_job_t* job, int face, bool opaque)
{
    R3D_ASSERT(call->type == R3D_RENDER_CALL_MESH); //< Paranoid assert, should be fine

    const r3d_render_group_t* group = r3d_render_get_call_group(call);
    const R3D_Material* material = &call->mesh.material;

    /* --- Use shader --- */

    R3D_SurfaceShader* shader = call->mesh.material.shader;
    R3D_SHADER_USE_SELECT(scene.probeUnlit, shader);

    /* --- Send matrices --- */

    Matrix matNormal = r3d_matrix_normal(&group->transform);

    R3D_SHADER_SET_MAT4_SELECT(scene.probeUnlit, shader, uMatModel, group->transform);
    R3D_SHADER_SET_MAT4_SELECT(scene.probeUnlit, shader, uMatNormal, matNormal);
    R3D_SHADER_SET_MAT4_SELECT(scene.probeUnlit, shader, uMatView, job->view[face]);
    R3D_SHADER_SET_MAT4_SELECT(scene.probeUnlit, shader, uMatInvView, job->invView[face]);
    R3D_SHADER_SET_MAT4_SELECT(scene.probeUnlit, shader, uMatViewProj, job->viewProj[face]);

    /* --- Send skinning related data --- */

    if (group->skinTexture > 0)
    {
        R3D_SHADER_BIND_SAMPLER_SELECT(scene.probeUnlit, shader, uBoneMatricesTex, group->skinTexture);
        R3D_SHADER_SET_INT_SELECT(scene.probeUnlit, shader, uSkinning, true);
    }
    else
    {
        R3D_SHADER_SET_INT_SELECT(scene.probeUnlit, shader, uSkinning, false);
    }

    /* --- Send billboard related data --- */

    R3D_SHADER_SET_INT_SELECT(scene.probeUnlit, shader, uBillboard, material->billboardMode);

    /* --- Set texcoord offset/scale --- */

    R3D_SHADER_SET_VEC2_SELECT(scene.probeUnlit, shader, uTexCoordOffset, material->uvOffset);
    R3D_SHADER_SET_VEC2_SELECT(scene.probeUnlit, shader, uTexCoordScale, material->uvScale);

    /* --- Set color material maps --- */

    bool hybrid = (material->transparencyMode == R3D_TRANSPARENCY_HYBRID);
    float cutoffSign = opaque ? 1.0f : (hybrid ? -1.0f : 0.0f);

    R3D_SHADER_SET_COL4_SELECT(scene.probeUnlit, shader, uAlbedoColor, material->albedo.color);
    R3D_SHADER_SET_FLOAT_SELECT(scene.probeUnlit, shader, uAlphaCutoff, material->alphaCutoff);
    R3D_SHADER_SET_FLOAT_SELECT(scene.probeUnlit, shader, uCutoffSign, cutoffSign);

    /* --- Bind active texture maps --- */

    R3D_SHADER_BIND_SAMPLER_SELECT(scene.probeUnlit, shader, uAlbedoMap, R3D_TEXTURE_SELECT(material->albedo.texture.id, WHITE));

    /* --- Applying material parameters that are independent of shaders --- */

    r3d_driver_set_depth_state(material->depth);
    r3d_driver_set_stencil_state(material->stencil);
    r3d_driver_set_blend_mode(material->blendMode);
    r3d_driver_set_cull_mode(material->cullMode);

    /* --- Rendering the object corresponding to the draw call --- */

    if (r3d_render_has_instances(group))
    {
        R3D_SHADER_SET_INT_SELECT(scene.probeUnlit, shader, uInstancing, true);
        r3d_render_draw_instanced(call);
    }
    else
    {
        R3D_SHADER_SET_INT_SELECT(scene.probeUnlit, shader, uInstancing, false);
        r3d_render_draw(call);
    }
}

// Stencil bit that marks "this pixel has been shaded" in the GL_EQUAL pass, cleared after it
#define GEOMETRY_PREPASS_STENCIL_BIT 0x80

bool geometry_call_prepassable(const r3d_render_call_t* call)
{
    // A pre-passed draw must produce exactly the depth the shading pass will produce:
    // no discard (alpha test off) and an ordinary "nearest wins" depth test. The stencil
    // state must leave the top bit free for the tie-break (see raster_geometry()) and
    // must not depend on depth-failing fragments, which the EQUAL pass doesn't replay.
    const R3D_Material* material = &call->mesh.material;
    R3D_CompareMode depthMode = material->depth.mode;
    R3D_StencilState stencil = material->stencil;
    return material->alphaCutoff <= 0.0f
        && (depthMode == R3D_COMPARE_LESS || depthMode == R3D_COMPARE_LEQUAL)
        && stencil.mode == R3D_COMPARE_ALWAYS
        && stencil.opZFail == R3D_STENCIL_KEEP
        && (stencil.opPass == R3D_STENCIL_KEEP || stencil.opPass == R3D_STENCIL_REPLACE)
        && (stencil.ref & GEOMETRY_PREPASS_STENCIL_BIT) == 0;
}

void raster_geometry(const r3d_render_call_t* call, geometry_raster_mode_t mode)
{
    R3D_ASSERT(call->type == R3D_RENDER_CALL_MESH); //< Paranoid assert, should be fine

    const r3d_render_group_t* group = r3d_render_get_call_group(call);
    const R3D_Material* material = &call->mesh.material;

    /* --- Use shader --- */

    // Pre-passed draws use the pre-pass variant in both passes (same vertex transform,
    // no alpha test); everything else uses the regular geometry program.
    R3D_SurfaceShader* shader = call->mesh.material.shader;
    r3d_shader_scene_geometry_t* program = NULL;
    if (mode == GEOMETRY_RASTER_FULL)
    {
        R3D_SHADER_USE_SELECT(scene.geometry, shader);
        program = R3D_SHADER_SELECT(scene.geometry, shader);
    }
    else
    {
        R3D_SHADER_USE_SELECT(scene.geometryPrepass, shader);
        program = R3D_SHADER_SELECT(scene.geometryPrepass, shader);
    }

    /* --- Send matrices --- */

    // Local patch 21: the normal matrix depends on the transform alone, so it is recomputed
    // only when the transform changes (instanced map geometry: the identity for every draw,
    // and each pre-passed draw comes twice per frame)
    static Matrix normalFor, matNormal;
    static bool normalKnown = false;
    if (!normalKnown || memcmp(&normalFor, &group->transform, sizeof(Matrix)) != 0)
    {
        matNormal = r3d_matrix_normal(&group->transform);
        normalFor = group->transform;
        normalKnown = true;
    }

    R3D_SHADER__SET_MAT4_IMPL(program, uMatModel, group->transform);
    R3D_SHADER__SET_MAT4_IMPL(program, uMatNormal, matNormal);

    /* --- Send skinning related data --- */

    if (group->skinTexture > 0)
    {
        R3D_SHADER__BIND_SAMPLER_IMPL(program, uBoneMatricesTex, group->skinTexture);
        R3D_SHADER__SET_INT_IMPL(program, uSkinning, true);
    }
    else
    {
        R3D_SHADER__SET_INT_IMPL(program, uSkinning, false);
    }

    /* --- Send billboard related data --- */

    R3D_SHADER__SET_INT_IMPL(program, uBillboard, material->billboardMode);

    /* --- Set factor material maps --- */

    R3D_SHADER__SET_FLOAT_IMPL(program, uEmissionEnergy, material->emission.energy);
    R3D_SHADER__SET_FLOAT_IMPL(program, uAlphaCutoff, material->alphaCutoff);
    R3D_SHADER__SET_FLOAT_IMPL(program, uNormalScale, material->normal.scale);
    R3D_SHADER__SET_FLOAT_IMPL(program, uOcclusion, Clamp(material->orm.occlusion, 0.0f, 1.0f));
    R3D_SHADER__SET_FLOAT_IMPL(program, uRoughness, Clamp(material->orm.roughness, 0.0f, 1.0f));
    R3D_SHADER__SET_FLOAT_IMPL(program, uMetalness, Clamp(material->orm.metalness, 0.0f, 1.0f));
    R3D_SHADER__SET_FLOAT_IMPL(program, uSpecular, Clamp(material->orm.specular, 0.0f, 1.0f));

    /* --- Set texcoord offset/scale --- */

    R3D_SHADER__SET_VEC2_IMPL(program, uTexCoordOffset, material->uvOffset);
    R3D_SHADER__SET_VEC2_IMPL(program, uTexCoordScale, material->uvScale);

    /* --- Set color material maps --- */

    R3D_SHADER__SET_COL4_IMPL(program, uAlbedoColor, material->albedo.color);
    R3D_SHADER__SET_COL3_IMPL(program, uEmissionColor, material->emission.color);

    /* --- Bind active texture maps --- */

    R3D_SHADER__BIND_SAMPLER_IMPL(program, uAlbedoMap, R3D_TEXTURE_SELECT(material->albedo.texture.id, WHITE));
    R3D_SHADER__BIND_SAMPLER_IMPL(program, uNormalMap, R3D_TEXTURE_SELECT(material->normal.texture.id, NORMAL));
    R3D_SHADER__BIND_SAMPLER_IMPL(program, uEmissionMap, R3D_TEXTURE_SELECT(material->emission.texture.id, WHITE));
    R3D_SHADER__BIND_SAMPLER_IMPL(program, uOrmMap, R3D_TEXTURE_SELECT(material->orm.texture.id, WHITE));

    /* --- Applying material parameters that are independent of shaders --- */

    r3d_driver_set_cull_mode(material->cullMode);

    if (mode == GEOMETRY_RASTER_EQUAL)
    {
        // Local patch 21: the EQUAL pass overrides the material's depth function and its
        // whole stencil function and operations below; setting them first only toggled
        // the GL state back and forth on every draw (2 glDepthFunc + 2 glStencilFunc)
        r3d_driver_set_depth_range(material->depth.rangeNear, material->depth.rangeFar);
        r3d_driver_set_depth_offset(material->depth.offsetUnits, material->depth.offsetFactor);
    }
    else
    {
        r3d_driver_set_depth_state(material->depth);
        r3d_driver_set_stencil_state(material->stencil);
    }

    /* --- Depth pre-pass handling (monolith patch) --- */

    if (mode != GEOMETRY_RASTER_FULL)
    {
        R3D_SHADER__SET_INT_IMPL(program, uDepthOnly, mode == GEOMETRY_RASTER_DEPTH_ONLY);
    }

    if (mode == GEOMETRY_RASTER_DEPTH_ONLY)
    {
        // Stencil is written by the shading pass, where only the visible fragment passes
        r3d_driver_set_stencil_mask(0x00);
        r3d_driver_set_depth_mask(GL_TRUE);
    }
    else if (mode == GEOMETRY_RASTER_EQUAL)
    {
        // Coplanar faces can produce bit-identical depths. Without the pre-pass the
        // first one drawn wins (GL_LESS); with GL_EQUAL all of them would pass and the
        // last one would win. So the first fragment that passes sets the top stencil bit
        // and later ones at the same pixel fail the stencil test: same winner as before.
        uint8_t ref = material->stencil.ref | GEOMETRY_PREPASS_STENCIL_BIT;
        bool replace = (material->stencil.opPass == R3D_STENCIL_REPLACE);
        r3d_driver_set_stencil_func(GL_NOTEQUAL, ref, GEOMETRY_PREPASS_STENCIL_BIT);
        r3d_driver_set_stencil_op(GL_KEEP, GL_KEEP, GL_REPLACE);
        r3d_driver_set_stencil_mask(replace ? 0xFF : GEOMETRY_PREPASS_STENCIL_BIT);
        r3d_driver_set_depth_func(GL_EQUAL);
        r3d_driver_set_depth_mask(GL_FALSE);
    }
    else
    {
        r3d_driver_set_stencil_mask(0xFF);
        r3d_driver_set_depth_mask(GL_TRUE);
    }

    /* --- Rendering the object corresponding to the draw call --- */

    if (r3d_render_has_instances(group))
    {
        R3D_SHADER__SET_INT_IMPL(program, uInstancing, true);
        r3d_render_draw_instanced(call);
    }
    else
    {
        R3D_SHADER__SET_INT_IMPL(program, uInstancing, false);
        r3d_render_draw(call);
    }
}

void raster_decal(const r3d_render_call_t* call)
{
    R3D_ASSERT(call->type == R3D_RENDER_CALL_DECAL); //< Paranoid assert, should be fine

    const r3d_render_group_t* group = r3d_render_get_call_group(call);
    const R3D_Decal* decal = &call->decal.instance;

    /* --- Use shader --- */

    R3D_SurfaceShader* shader = call->decal.instance.shader;
    R3D_SHADER_USE_SELECT(scene.decal, shader);

    /* --- Bind global textures --- */

    R3D_SHADER_BIND_SAMPLER_SELECT(scene.decal, shader, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 0));
    R3D_SHADER_BIND_SAMPLER_SELECT(scene.decal, shader, uGeomNormalTex, r3d_target_get(R3D_TARGET_GEOM_NORMAL));

    /* --- Set additional matrix uniforms --- */

    Matrix matNormal = r3d_matrix_normal(&group->transform);

    R3D_SHADER_SET_MAT4_SELECT(scene.decal, shader, uMatModel, group->transform);
    R3D_SHADER_SET_MAT4_SELECT(scene.decal, shader, uMatNormal, matNormal);

    /* --- Set factor material maps --- */

    R3D_SHADER_SET_FLOAT_SELECT(scene.decal, shader, uEmissionEnergy, decal->emission.energy);
    R3D_SHADER_SET_FLOAT_SELECT(scene.decal, shader, uAlphaCutoff, decal->alphaCutoff);
    R3D_SHADER_SET_FLOAT_SELECT(scene.decal, shader, uNormalScale, decal->normal.scale);
    R3D_SHADER_SET_FLOAT_SELECT(scene.decal, shader, uOcclusion, Clamp(decal->orm.occlusion, 0.0f, 1.0f));
    R3D_SHADER_SET_FLOAT_SELECT(scene.decal, shader, uRoughness, Clamp(decal->orm.roughness, 0.0f, 1.0f));
    R3D_SHADER_SET_FLOAT_SELECT(scene.decal, shader, uMetalness, Clamp(decal->orm.metalness, 0.0f, 1.0f));
    R3D_SHADER_SET_FLOAT_SELECT(scene.decal, shader, uSpecular, Clamp(decal->orm.specular, 0.0f, 1.0f));

    /* --- Set texcoord offset/scale --- */

    R3D_SHADER_SET_VEC2_SELECT(scene.decal, shader, uTexCoordOffset, decal->uvOffset);
    R3D_SHADER_SET_VEC2_SELECT(scene.decal, shader, uTexCoordScale, decal->uvScale);

    /* --- Set color material maps --- */

    R3D_SHADER_SET_COL4_SELECT(scene.decal, shader, uAlbedoColor, decal->albedo.color);
    R3D_SHADER_SET_COL3_SELECT(scene.decal, shader, uEmissionColor, decal->emission.color);

    /* --- Set decal specific values --- */

    R3D_SHADER_SET_FLOAT_SELECT(scene.decal, shader, uNormalThreshold, decal->normalThreshold);
    R3D_SHADER_SET_FLOAT_SELECT(scene.decal, shader, uFadeWidth, decal->fadeWidth);
    R3D_SHADER_SET_INT_SELECT(scene.decal, shader, uApplyColor, decal->applyColor && (decal->albedo.texture.id != 0));

    /* --- Bind active texture maps --- */

    R3D_SHADER_BIND_SAMPLER_SELECT(scene.decal, shader, uAlbedoMap, R3D_TEXTURE_SELECT(decal->albedo.texture.id, WHITE));
    R3D_SHADER_BIND_SAMPLER_SELECT(scene.decal, shader, uNormalMap, R3D_TEXTURE_SELECT(decal->normal.texture.id, NORMAL));
    R3D_SHADER_BIND_SAMPLER_SELECT(scene.decal, shader, uEmissionMap, R3D_TEXTURE_SELECT(decal->emission.texture.id, WHITE));
    R3D_SHADER_BIND_SAMPLER_SELECT(scene.decal, shader, uOrmMap, R3D_TEXTURE_SELECT(decal->orm.texture.id, WHITE));

    /* --- Rendering the object corresponding to the draw call --- */

    if (r3d_render_has_instances(group))
    {
        R3D_SHADER_SET_INT_SELECT(scene.decal, shader, uInstancing, true);
        r3d_render_draw_instanced(call);
    }
    else
    {
        R3D_SHADER_SET_INT_SELECT(scene.decal, shader, uInstancing, false);
        r3d_render_draw(call);
    }
}

void raster_forward(const r3d_render_call_t* call)
{
    R3D_ASSERT(call->type == R3D_RENDER_CALL_MESH); //< Paranoid assert, should be fine

    const r3d_render_group_t* group = r3d_render_get_call_group(call);
    const R3D_Material* material = &call->mesh.material;

    /* --- Use shader --- */

    R3D_SurfaceShader* shader = call->mesh.material.shader;
    R3D_SHADER_USE_SELECT(scene.forward, shader);

    /* --- Set view related data --- */

    // NOTE: We don't use the UBO view position because this shader is reused by probes with their own view position
    R3D_SHADER_SET_VEC3_SELECT(scene.forward, shader, uViewPosition, R3D.viewState.camera.position);

    /* --- Send matrices --- */

    Matrix matNormal = r3d_matrix_normal(&group->transform);

    R3D_SHADER_SET_MAT4_SELECT(scene.forward, shader, uMatModel, group->transform);
    R3D_SHADER_SET_MAT4_SELECT(scene.forward, shader, uMatNormal, matNormal);

    /* --- Send skinning related data --- */

    if (group->skinTexture > 0)
    {
        R3D_SHADER_BIND_SAMPLER_SELECT(scene.forward, shader, uBoneMatricesTex, group->skinTexture);
        R3D_SHADER_SET_INT_SELECT(scene.forward, shader, uSkinning, true);
    }
    else
    {
        R3D_SHADER_SET_INT_SELECT(scene.forward, shader, uSkinning, false);
    }

    /* --- Send billboard related data --- */

    R3D_SHADER_SET_INT_SELECT(scene.forward, shader, uBillboard, material->billboardMode);

    /* --- Set factor material maps --- */

    R3D_SHADER_SET_FLOAT_SELECT(scene.forward, shader, uEmissionEnergy, material->emission.energy);
    R3D_SHADER_SET_FLOAT_SELECT(scene.forward, shader, uAlphaCutoff, material->alphaCutoff);
    R3D_SHADER_SET_FLOAT_SELECT(scene.forward, shader, uCutoffSign, (material->transparencyMode == R3D_TRANSPARENCY_HYBRID) ? -1.0f : 0.0f);
    R3D_SHADER_SET_FLOAT_SELECT(scene.forward, shader, uNormalScale, material->normal.scale);
    R3D_SHADER_SET_FLOAT_SELECT(scene.forward, shader, uOcclusion, Clamp(material->orm.occlusion, 0.0f, 1.0f));
    R3D_SHADER_SET_FLOAT_SELECT(scene.forward, shader, uRoughness, Clamp(material->orm.roughness, 0.0f, 1.0f));
    R3D_SHADER_SET_FLOAT_SELECT(scene.forward, shader, uMetalness, Clamp(material->orm.metalness, 0.0f, 1.0f));
    R3D_SHADER_SET_FLOAT_SELECT(scene.forward, shader, uSpecular, Clamp(material->orm.specular, 0.0f, 1.0f));

    /* --- Set texcoord offset/scale --- */

    R3D_SHADER_SET_VEC2_SELECT(scene.forward, shader, uTexCoordOffset, material->uvOffset);
    R3D_SHADER_SET_VEC2_SELECT(scene.forward, shader, uTexCoordScale, material->uvScale);

    /* --- Set color material maps --- */

    R3D_SHADER_SET_COL4_SELECT(scene.forward, shader, uAlbedoColor, material->albedo.color);
    R3D_SHADER_SET_COL3_SELECT(scene.forward, shader, uEmissionColor, material->emission.color);

    /* --- Bind active texture maps --- */

    R3D_SHADER_BIND_SAMPLER_SELECT(scene.forward, shader, uAlbedoMap, R3D_TEXTURE_SELECT(material->albedo.texture.id, WHITE));
    R3D_SHADER_BIND_SAMPLER_SELECT(scene.forward, shader, uNormalMap, R3D_TEXTURE_SELECT(material->normal.texture.id, NORMAL));
    R3D_SHADER_BIND_SAMPLER_SELECT(scene.forward, shader, uEmissionMap, R3D_TEXTURE_SELECT(material->emission.texture.id, WHITE));
    R3D_SHADER_BIND_SAMPLER_SELECT(scene.forward, shader, uOrmMap, R3D_TEXTURE_SELECT(material->orm.texture.id, WHITE));

    /* --- Applying material parameters that are independent of shaders --- */

    r3d_driver_set_depth_state(material->depth);
    r3d_driver_set_stencil_state(material->stencil);
    r3d_driver_set_blend_mode(material->blendMode);
    r3d_driver_set_cull_mode(material->cullMode);

    /* --- Rendering the object corresponding to the draw call --- */

    if (r3d_render_has_instances(group))
    {
        R3D_SHADER_SET_INT_SELECT(scene.forward, shader, uInstancing, true);
        r3d_render_draw_instanced(call);
    }
    else
    {
        R3D_SHADER_SET_INT_SELECT(scene.forward, shader, uInstancing, false);
        r3d_render_draw(call);
    }
}

void raster_unlit(const r3d_render_call_t* call, bool opaque)
{
    R3D_ASSERT(call->type == R3D_RENDER_CALL_MESH); //< Paranoid assert, should be fine

    const r3d_render_group_t* group = r3d_render_get_call_group(call);
    const R3D_Material* material = &call->mesh.material;

    /* --- Use shader --- */

    R3D_SurfaceShader* shader = call->mesh.material.shader;
    R3D_SHADER_USE_SELECT(scene.unlit, shader);

    /* --- Send matrices --- */

    Matrix matNormal = r3d_matrix_normal(&group->transform);

    R3D_SHADER_SET_MAT4_SELECT(scene.unlit, shader, uMatModel, group->transform);
    R3D_SHADER_SET_MAT4_SELECT(scene.unlit, shader, uMatNormal, matNormal);

    /* --- Send skinning related data --- */

    if (group->skinTexture > 0)
    {
        R3D_SHADER_BIND_SAMPLER_SELECT(scene.unlit, shader, uBoneMatricesTex, group->skinTexture);
        R3D_SHADER_SET_INT_SELECT(scene.unlit, shader, uSkinning, true);
    }
    else
    {
        R3D_SHADER_SET_INT_SELECT(scene.unlit, shader, uSkinning, false);
    }

    /* --- Send billboard related data --- */

    R3D_SHADER_SET_INT_SELECT(scene.unlit, shader, uBillboard, material->billboardMode);

    /* --- Set texcoord offset/scale --- */

    R3D_SHADER_SET_VEC2_SELECT(scene.unlit, shader, uTexCoordOffset, material->uvOffset);
    R3D_SHADER_SET_VEC2_SELECT(scene.unlit, shader, uTexCoordScale, material->uvScale);

    /* --- Set color material maps --- */

    bool hybrid = (material->transparencyMode == R3D_TRANSPARENCY_HYBRID);
    float cutoffSign = opaque ? 1.0f : (hybrid ? -1.0f : 0.0f);

    R3D_SHADER_SET_COL4_SELECT(scene.unlit, shader, uAlbedoColor, material->albedo.color);
    R3D_SHADER_SET_FLOAT_SELECT(scene.unlit, shader, uAlphaCutoff, material->alphaCutoff);
    R3D_SHADER_SET_FLOAT_SELECT(scene.unlit, shader, uCutoffSign, cutoffSign);

    /* --- Bind active texture maps --- */

    R3D_SHADER_BIND_SAMPLER_SELECT(scene.unlit, shader, uAlbedoMap, R3D_TEXTURE_SELECT(material->albedo.texture.id, WHITE));

    /* --- Applying material parameters that are independent of shaders --- */

    r3d_driver_set_depth_state(material->depth);
    r3d_driver_set_stencil_state(material->stencil);
    if (!opaque) r3d_driver_set_blend_mode(material->blendMode);
    r3d_driver_set_cull_mode(material->cullMode);

    /* --- Rendering the object corresponding to the draw call --- */

    if (r3d_render_has_instances(group))
    {
        R3D_SHADER_SET_INT_SELECT(scene.unlit, shader, uInstancing, true);
        r3d_render_draw_instanced(call);
    }
    else
    {
        R3D_SHADER_SET_INT_SELECT(scene.unlit, shader, uInstancing, false);
        r3d_render_draw(call);
    }
}

void pass_scene_shadows(void)
{
    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_enable(GL_DEPTH_TEST);

    r3d_driver_set_depth_func(GL_LEQUAL);
    r3d_driver_set_depth_mask(GL_TRUE);

    #define COND (                                                          \
        (call->mesh.instance.shadowCastMode != R3D_SHADOW_CAST_DISABLED) && \
        IS_MESH_VISIBLE(call->mesh.instance, job->cullMask)                 \
    )

    R3D_LIGHT_FOR_EACH_SHADOW_JOB(job)
    {
        r3d_light_bind_shadow_fbo(job->type, job->shadowLayer, job->layerFace);
        glClear(GL_DEPTH_BUFFER_BIT);

        const R3D_Frustum* frustum = &job->frustum;
        r3d_render_cull_groups(frustum);

        R3D_RENDER_FOR_EACH(call, COND, frustum, R3D_RENDER_LIST_OPAQUE_INST, R3D_RENDER_LIST_OPAQUE)
        {
            if (r3d_render_should_cast_shadow(call))
            {
                if (job->type == R3D_LIGHT_OMNI)
                {
                    raster_depth_cube(call, &job->viewProj, job);
                }
                else
                {
                    raster_depth(call, &job->viewProj, job);
                }
            }
        }
    }

    #undef COND
}

// Local patch (see patches.md): rebuilds the min/max depth tiles of the directional
// shadow layers rendered this frame (or all of them after a reallocation). The
// volumetric fog uses them to skip the PCF taps of fully lit / fully shadowed steps.
void pass_scene_shadow_minmax(void)
{
    // Only the volumetric fog reads the tiles: while it's off, just remember that
    // every layer has to be rebuilt once it comes back on
    static bool pendingAll = false;
    bool all = r3d_light_shadow_minmax_take_stale() || pendingAll;
    uint32_t layerCount = r3d_light_shadow_layer_count(R3D_LIGHT_DIR);
    if (r3d_light_shadow_minmax_map() == 0 || layerCount == 0) return;

    bool any = all;
    R3D_LIGHT_FOR_EACH_SHADOW_JOB(job) {
        if (job->type == R3D_LIGHT_DIR) any = true;
    }
    if (!any) return;

    pendingAll = !R3D.environment.volumetricFog.enabled;
    if (pendingAll) return;

    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_DEPTH_TEST);
    r3d_driver_disable(GL_CULL_FACE);
    r3d_driver_disable(GL_BLEND);
    r3d_driver_disable(GL_SCISSOR_TEST);

    R3D_SHADER_USE(prepare.shadowMinMax);

    // Read raw depth: the shadow array has compare mode on, which a plain sampler2DArray
    // must not see (undefined results), so switch it off for this pass only
    r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_SHADOW_DIR, r3d_light_shadow_map(R3D_LIGHT_DIR));
    glActiveTexture(GL_TEXTURE0 + R3D_SHADER_SAMPLER_SHADOW_DIR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_NONE);

    for (int layer = 0; layer < (int)layerCount; layer++)
    {
        bool rendered = all;
        R3D_LIGHT_FOR_EACH_SHADOW_JOB(job) {
            if (job->type == R3D_LIGHT_DIR && job->shadowLayer == layer) rendered = true;
        }
        if (!rendered) continue;

        R3D_SHADER_BIND_SAMPLER(prepare.shadowMinMax, uSourceTex, 0); //< No feedback loop with the mid target
        r3d_light_bind_shadow_minmax_mid_fbo();
        R3D_SHADER_SET_INT(prepare.shadowMinMax, uPass, 0);
        R3D_SHADER_SET_INT(prepare.shadowMinMax, uLayer, layer);
        R3D_RENDER_SCREEN();

        r3d_light_bind_shadow_minmax_fbo(layer);
        R3D_SHADER_SET_INT(prepare.shadowMinMax, uPass, 1);
        R3D_SHADER_BIND_SAMPLER(prepare.shadowMinMax, uSourceTex, r3d_light_shadow_minmax_mid_map());
        R3D_RENDER_SCREEN();
    }

    // r3d_shader_bind_sampler() resets the active unit, so select the shadow map again
    glActiveTexture(GL_TEXTURE0 + R3D_SHADER_SAMPLER_SHADOW_DIR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glActiveTexture(GL_TEXTURE0);

    r3d_target_invalidate_cache(); //< We bound our own framebuffer
}

// Local patch (see patches.md): bakes the fog's sun-visibility volume from the shadow
// layer the request rendered this frame (pass_scene_shadows), one draw per z slice.
// Runs once per bake (map load), so it may block: glFinish around it for the bake time.
void pass_scene_fog_sunvol(void)
{
    r3d_light_fog_sunvol_t* vol = &R3D_MOD_LIGHT.fogSunVol;
    r3d_light_fog_sunvol_request_t req = vol->request;
    vol->request.pending = false;

    glFinish();
    double t0 = GetTime();

    if (!r3d_light_prepare_fog_sunvol()) return;

    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_DEPTH_TEST);
    r3d_driver_disable(GL_CULL_FACE);
    r3d_driver_disable(GL_BLEND);
    r3d_driver_disable(GL_SCISSOR_TEST);

    R3D_SHADER_USE(prepare.fogSunVol);
    r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_SHADOW_DIR, r3d_light_shadow_map(R3D_LIGHT_DIR));
    R3D_SHADER_SET_MAT4(prepare.fogSunVol, uLightViewProj, req.viewProj);
    R3D_SHADER_SET_INT(prepare.fogSunVol, uLayer, req.layer);
    R3D_SHADER_SET_VEC3(prepare.fogSunVol, uVolMin, req.min);
    R3D_SHADER_SET_VEC3(prepare.fogSunVol, uVoxel, req.voxel);
    R3D_SHADER_SET_INT(prepare.fogSunVol, uSub, req.sub);
    R3D_SHADER_SET_FLOAT(prepare.fogSunVol, uBias, req.bias);

    for (int z = 0; z < req.dims[2]; z++)
    {
        r3d_light_bind_fog_sunvol_slice(z);
        R3D_SHADER_SET_INT(prepare.fogSunVol, uSlice, z);
        R3D_RENDER_SCREEN();
    }

    r3d_target_invalidate_cache(); //< We bound our own framebuffer
    vol->valid = true;

    glFinish();
    fogSunVolBakeMs = (float)((GetTime() - t0) * 1000.0);
    R3D_TRACELOG(LOG_INFO, "Fog sun volume baked: %dx%dx%d voxels (%.2f MB), %.1f ms",
                 req.dims[0], req.dims[1], req.dims[2],
                 (double)req.dims[0] * req.dims[1] * req.dims[2] / (1024.0 * 1024.0), fogSunVolBakeMs);
}

void pass_scene_probes(void)
{
    #define RASTER_PROBE(opaque)                                    \
    do {                                                            \
        if (!call->mesh.material.unlit)                             \
        {                                                           \
            upload_light_array_block_for_mesh(call, job->shadows);  \
            raster_probe_forward(call, job, iFace, (opaque));       \
        }                                                           \
        else                                                        \
        {                                                           \
            raster_probe_unlit(call, job, iFace, (opaque));         \
        }                                                           \
    } while(0)

    const R3D_EnvBackground* bg = &R3D.environment.background;
    const R3D_EnvFog* fog = &R3D.environment.fog;

    R3D_ENV_FOR_EACH_PROBE_JOB(job)
    {
        for (int iFace = 0; iFace < 6; iFace++)
        {
            // Generates the list of visible groups for the current face of the capture
            const R3D_Frustum* frustum = &job->frustum[iFace];
            r3d_render_cull_groups(frustum);

            // Render scene
            r3d_driver_enable(GL_STENCIL_TEST);
            r3d_driver_enable(GL_DEPTH_TEST);
            r3d_driver_enable(GL_BLEND);

            r3d_driver_set_depth_mask(GL_TRUE);

            r3d_env_probe_capture_bind_fbo(job->probeType, iFace);
            glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

            R3D_RENDER_FOR_EACH(call, true, frustum, R3D_RENDER_LIST_OPAQUE_INST, R3D_RENDER_LIST_OPAQUE)
            {
                RASTER_PROBE(true);
            }

            R3D_RENDER_FOR_EACH(call, true, frustum, R3D_RENDER_LIST_BLEND_INST, R3D_RENDER_LIST_BLEND)
            {
                RASTER_PROBE(false);
            }

            r3d_driver_set_depth_offset(0.0f, 0.0f);
            r3d_driver_set_depth_range(0.0f, 1.0f);

            // Render background
            r3d_driver_disable(GL_STENCIL_TEST);
            r3d_driver_disable(GL_CULL_FACE);
            r3d_driver_disable(GL_BLEND);

            r3d_driver_set_depth_func(GL_LEQUAL);
            r3d_driver_set_depth_mask(GL_FALSE);

            if (bg->sky.texture != 0)
            {
                R3D_SHADER_USE(scene.skybox);
                float lod = (float)r3d_get_mip_levels_1d(bg->sky.size);
                R3D_SHADER_BIND_SAMPLER(scene.skybox, uSkyMap, bg->sky.texture);
                R3D_SHADER_SET_FLOAT(scene.skybox, uEnergy, bg->energy);
                R3D_SHADER_SET_FLOAT(scene.skybox, uLod, bg->skyBlur * lod);
                R3D_SHADER_SET_VEC4(scene.skybox, uRotation, bg->rotation);
                R3D_SHADER_SET_MAT4(scene.skybox, uMatInvView, job->invView[iFace]);
                R3D_SHADER_SET_MAT4(scene.skybox, uMatInvProj, job->invProj);
            }
            else
            {
                Vector3 bgColor = r3d_color_srgb_to_linear_vec3(bg->color);
                bgColor = Vector3Scale(bgColor, bg->energy);
                if (fog->mode != R3D_FOG_DISABLED)
                {
                    Vector3 fogColor = r3d_color_srgb_to_linear_vec3(fog->color);
                    bgColor = Vector3Lerp(bgColor, fogColor, fog->skyAffect);
                }
                R3D_SHADER_USE(scene.background);
                R3D_SHADER_SET_VEC4(scene.background, uColor, (Vector4) {bgColor.x, bgColor.y, bgColor.z, 1.0f});
            }

            R3D_RENDER_SCREEN();
        }

        // Generate irradiance/prefilter map

        switch (job->probeType)
        {
        case R3D_PROBE_ILLUMINATION:
            {
                GLuint captureTex = r3d_env_probe_capture_get(job->probeType);
                r3d_pass_prepare_irradiance(job->layer, captureTex);
            }
            break;

        case R3D_PROBE_REFLECTION:
            {
                r3d_env_probe_capture_gen_mipmaps(job->probeType);
                GLuint captureTex = r3d_env_probe_capture_get(job->probeType);
                int captureSize   = r3d_env_probe_capture_size(job->probeType);
                r3d_pass_prepare_prefilter(job->layer, captureTex, captureSize);
            }
            break;

        default:
            R3D_ASSERT(false);
            break;
        }

        r3d_target_invalidate_cache(); //< The IBL gen functions bind framebuffers; resetting them prevents any problems
    }

    #undef RASTER_PROBE
}

void pass_scene_geometry(void)
{
    r3d_driver_enable(GL_STENCIL_TEST);
    r3d_driver_enable(GL_DEPTH_TEST);
    r3d_driver_enable(GL_CULL_FACE);
    r3d_driver_disable(GL_BLEND);

    r3d_driver_set_depth_mask(GL_TRUE);
    r3d_driver_set_stencil_mask(0xFF);

    R3D_TARGET_BIND_LOAD(0, true, R3D_TARGET_GBUFFER);

    #define COND (IS_MESH_VISIBLE_CAMERA(call->mesh.instance) && (!call->mesh.material.unlit))

    if (R3D.depthPrepass)
    {
        // Monolith patch: depth pre-pass. With one big instanced draw there is no
        // front-to-back order, so the (expensive) surface shader would otherwise run
        // for every overdrawn fragment. Lay down the depth first, then shade with
        // GL_EQUAL so only the visible fragment of each pixel runs the shader.
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        R3D_RENDER_FOR_EACH(call, COND && geometry_call_prepassable(call), &R3D.viewState.frustum, R3D_RENDER_LIST_OPAQUE_INST, R3D_RENDER_LIST_OPAQUE)
        {
            raster_geometry(call, GEOMETRY_RASTER_DEPTH_ONLY);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }

    bool anyPrepassed = false;
    R3D_RENDER_FOR_EACH(call, COND, &R3D.viewState.frustum, R3D_RENDER_LIST_OPAQUE_INST, R3D_RENDER_LIST_OPAQUE)
    {
        bool prepassed = R3D.depthPrepass && geometry_call_prepassable(call);
        raster_geometry(call, prepassed ? GEOMETRY_RASTER_EQUAL : GEOMETRY_RASTER_FULL);
        anyPrepassed |= prepassed;
    }
    #undef COND

    if (anyPrepassed)
    {
        // Clear the tie-break bit again (the clear only touches the bits in the write mask)
        const GLint zero = 0;
        r3d_driver_set_stencil_mask(GEOMETRY_PREPASS_STENCIL_BIT);
        glClearBufferiv(GL_STENCIL, 0, &zero);
    }

    r3d_driver_set_depth_mask(GL_TRUE);
    r3d_driver_set_stencil_mask(0xFF);
    r3d_driver_set_depth_offset(0.0f, 0.0f);
    r3d_driver_set_depth_range(0.0f, 1.0f);
}

void pass_scene_decals(void)
{
    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_DEPTH_TEST);
    r3d_driver_enable(GL_CULL_FACE);
    r3d_driver_enable(GL_BLEND);

    r3d_driver_set_cull_face(GL_FRONT); // Only render back faces to avoid clipping issues

    R3D_TARGET_BIND_LOAD(0, true, R3D_TARGET_DECAL);

    // FIXME: The decal shader uses the alpha channel of the ORM attachment as a blend factor,
    //        but this channel now stores the material specular (F0) written during the geometry
    //        pass. We mask alpha writes to preserve the underlying specular, at the cost of
    //        making orm.specular ineffective for decals.
    glColorMaski(2, GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);

    R3D_RENDER_FOR_EACH(call, true, &R3D.viewState.frustum, R3D_RENDER_LIST_DECAL_INST, R3D_RENDER_LIST_DECAL)
    {
        raster_decal(call);
    }

    glColorMaski(2, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void pass_prepare_pyramid(int maxLevel)
{
    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_CULL_FACE);
    r3d_driver_disable(GL_BLEND);

    r3d_driver_enable(GL_DEPTH_TEST);

    r3d_driver_set_stencil_mask(0x00);
    r3d_driver_set_depth_mask(GL_TRUE);
    r3d_driver_set_depth_func(GL_ALWAYS);

    R3D_SHADER_USE(prepare.pyramid);

    maxLevel = R3D_MIN(maxLevel, r3d_target_get_max_level(R3D_TARGET_DEPTH));

    for (int iDst = 1; iDst <= maxLevel; iDst++)
    {
        // Local patch 20: no clear, the full-screen draw below writes every pixel of both
        // attachments and the depth (depth test ALWAYS, no blend, no discard)
        R3D_TARGET_BIND_LOAD(iDst, true, R3D_TARGET_DEPTH, R3D_TARGET_SELECTOR);
        R3D_SHADER_BIND_SAMPLER(prepare.pyramid, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, iDst - 1));
        R3D_RENDER_SCREEN();
    }
}

void pass_prepare_downsample(r3d_target_t target, int level)
{
    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_CULL_FACE);
    r3d_driver_disable(GL_BLEND);

    r3d_driver_enable(GL_DEPTH_TEST);

    r3d_driver_set_stencil_mask(0x00);
    r3d_driver_set_depth_mask(GL_FALSE);
    r3d_driver_set_depth_func(GL_GREATER);

    R3D_TARGET_BIND_CLEAR(level, true, target);

    R3D_SHADER_USE(prepare.downsample);
    R3D_SHADER_BIND_SAMPLER(prepare.downsample, uSelectorTex, r3d_target_get_level(R3D_TARGET_SELECTOR, level));
    R3D_SHADER_BIND_SAMPLER(prepare.downsample, uSourceTex, r3d_target_get_level(target, level - 1));
    R3D_RENDER_SCREEN();
}

r3d_target_t pass_prepare_ssao(void)
{
    /* --- Setup OpenGL pipeline --- */

    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_CULL_FACE);
    r3d_driver_disable(GL_BLEND);

    r3d_driver_enable(GL_DEPTH_TEST);

    r3d_driver_set_stencil_mask(0x00);
    r3d_driver_set_depth_mask(GL_FALSE);
    r3d_driver_set_depth_func(GL_GREATER);

    /* --- Calculate SSAO --- */

    R3D_TARGET_BIND_CLEAR(1, true, R3D_TARGET_SSAO_0);
    R3D_SHADER_USE(prepare.ssao);

    R3D_SHADER_BIND_SAMPLER(prepare.ssao, uNormalTex, r3d_target_get_level(R3D_TARGET_NORMAL, 1));
    R3D_SHADER_BIND_SAMPLER(prepare.ssao, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 1));

    R3D_RENDER_SCREEN();

    /* --- Denoise SSAO --- */

    R3D_TARGET_BIND_CLEAR(1, true, R3D_TARGET_SSAO_1);
    R3D_SHADER_USE(prepare.denoiserSparse);

    R3D_SHADER_BIND_SAMPLER(prepare.denoiserSparse, uNormalTex, r3d_target_get_level(R3D_TARGET_NORMAL, 1));
    R3D_SHADER_BIND_SAMPLER(prepare.denoiserSparse, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 1));
    R3D_SHADER_BIND_SAMPLER(prepare.denoiserSparse, uSourceTex, r3d_target_get(R3D_TARGET_SSAO_0));

    const float radius = 4.0f;

    R3D_SHADER_SET_FLOAT(prepare.denoiserSparse, uNormalSharpness, 20.0f);
    R3D_SHADER_SET_FLOAT(prepare.denoiserSparse, uDepthSharpness, 100.0f);
    R3D_SHADER_SET_FLOAT(prepare.denoiserSparse, uInvBlurRadius2, 1.0f / (radius * radius));
    R3D_SHADER_SET_FLOAT(prepare.denoiserSparse, uBlurRadius, radius);

    R3D_RENDER_SCREEN();

    return R3D_TARGET_SSAO_1;
}

r3d_target_t pass_prepare_ssil(void)
{
    /* --- Setup OpenGL pipeline --- */

    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_CULL_FACE);
    r3d_driver_disable(GL_BLEND);

    r3d_driver_enable(GL_DEPTH_TEST);

    r3d_driver_set_stencil_mask(0x00);
    r3d_driver_set_depth_mask(GL_FALSE);
    r3d_driver_set_depth_func(GL_GREATER);

    /* --- Calculate SSIL --- */

    R3D_TARGET_BIND_CLEAR(1, true, R3D_TARGET_SSIL_0);
    R3D_SHADER_USE(prepare.ssil);

    R3D_SHADER_BIND_SAMPLER(prepare.ssil, uRadianceTex, r3d_target_get_level(R3D_TARGET_RADIANCE, 1));
    R3D_SHADER_BIND_SAMPLER(prepare.ssil, uNormalTex, r3d_target_get_level(R3D_TARGET_NORMAL, 1));
    R3D_SHADER_BIND_SAMPLER(prepare.ssil, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 1));

    R3D_RENDER_SCREEN();

    /* --- Denoise SSIL --- */

    r3d_target_t src = R3D_TARGET_SSIL_0;
    r3d_target_t dst = R3D_TARGET_SSIL_1;

    R3D_SHADER_USE(prepare.denoiserSparse);

    R3D_SHADER_BIND_SAMPLER(prepare.denoiserSparse, uNormalTex, r3d_target_get_level(R3D_TARGET_NORMAL, 1));
    R3D_SHADER_BIND_SAMPLER(prepare.denoiserSparse, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 1));

    R3D_SHADER_SET_FLOAT(prepare.denoiserSparse, uNormalSharpness, 20.0f);
    R3D_SHADER_SET_FLOAT(prepare.denoiserSparse, uDepthSharpness, 100.0f);

    float radius = 16.0f;
    for (int i = 0; i < 3; i++, radius *= 0.5f)
    {
        R3D_TARGET_BIND_CLEAR(1, true, dst);
        R3D_SHADER_SET_FLOAT(prepare.denoiserSparse, uBlurRadius, radius);
        R3D_SHADER_SET_FLOAT(prepare.denoiserSparse, uInvBlurRadius2, 1.0f / (radius * radius));
        R3D_SHADER_BIND_SAMPLER(prepare.denoiserSparse, uSourceTex, r3d_target_get(src));
        R3D_RENDER_SCREEN();

        R3D_SWAP(r3d_target_t, src, dst);
    }

    return src;
}

r3d_target_t pass_prepare_ssgi(void)
{
    /* --- Setup OpenGL pipeline --- */

    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_CULL_FACE);
    r3d_driver_disable(GL_BLEND);

    r3d_driver_enable(GL_DEPTH_TEST);

    r3d_driver_set_stencil_mask(0x00);
    r3d_driver_set_depth_mask(GL_FALSE);
    r3d_driver_set_depth_func(GL_GREATER);

    /* --- Calculate SSGI (RAW) --- */

    R3D_TARGET_BIND_CLEAR(1, true, R3D_TARGET_SSGI_0);
    R3D_SHADER_USE(prepare.ssgi);

    R3D_SHADER_BIND_SAMPLER(prepare.ssgi, uRadianceTex, r3d_target_get_level(R3D_TARGET_RADIANCE, 1));
    R3D_SHADER_BIND_SAMPLER(prepare.ssgi, uNormalTex, r3d_target_get_level(R3D_TARGET_NORMAL, 1));
    R3D_SHADER_BIND_SAMPLER(prepare.ssgi, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 1));

    R3D_RENDER_SCREEN();

    /*
        A-trous step schedule (largest -> smallest).

        We use a fixed pyramid: {16, 8, 4, 2, 1}.
        When fewer iterations are requested we simply truncate the list.

        The largest steps are what stabilize the filter in motion.
        The small ones mostly refine the result and hide the pattern left
        by the large kernels.

        If we derived the pyramid from the iteration count (e.g. 16,8,4,2,1
        for 5 steps), the max radius would shrink and the result becomes
        noticeably less stable when the camera moves.

        Keeping the same large radii and only dropping the final refinement
        passes preserves the spatial stability of the 5-step filter while
        allowing cheaper configurations.

        Examples:
            5 steps : 16  8  4  2  1
            4 steps : 16  8  4  2
            3 steps : 16  8  4
    */

    r3d_target_t src = R3D_TARGET_SSGI_0;
    r3d_target_t dst = R3D_TARGET_SSGI_1;

    int steps = R3D.environment.ssgi.denoiseSteps;

    if (steps > 0)
    {
        R3D_SHADER_USE(prepare.denoiserAtrous);

        R3D_SHADER_BIND_SAMPLER(prepare.denoiserAtrous, uNormalTex, r3d_target_get_level(R3D_TARGET_NORMAL, 1));
        R3D_SHADER_BIND_SAMPLER(prepare.denoiserAtrous, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 1));

        R3D_SHADER_SET_FLOAT(prepare.denoiserAtrous, uNormalSharpness, 20.0f);
        R3D_SHADER_SET_FLOAT(prepare.denoiserAtrous, uDepthSharpness, 100.0f);

        int stepWidth[] = {16, 8, 4, 2, 1};
        steps = R3D_MIN(steps, (int)R3D_ARRAY_SIZE(stepWidth));

        for (int i = 0; i < steps; i++)
        {
            float invStepWidth2 = 1.0f / (stepWidth[i]*stepWidth[i]);

            R3D_TARGET_BIND_CLEAR(1, true, dst);
            R3D_SHADER_BIND_SAMPLER(prepare.denoiserAtrous, uSourceTex, r3d_target_get(src));
            R3D_SHADER_SET_FLOAT(prepare.denoiserAtrous, uInvStepWidth2, invStepWidth2);
            R3D_SHADER_SET_INT(prepare.denoiserAtrous, uStepWidth, stepWidth[i]);
            R3D_RENDER_SCREEN();

            R3D_SWAP(r3d_target_t, src, dst);
        }
    }

    return src;
}

r3d_target_t pass_prepare_ssr(void)
{
    /* --- Downsample needed buffers --- */

    pass_prepare_downsample(R3D_TARGET_RADIANCE, 1);
    pass_prepare_downsample(R3D_TARGET_SPECULAR, 1);

    /* --- Setup OpenGL pipeline --- */

    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_CULL_FACE);
    r3d_driver_disable(GL_BLEND);

    r3d_driver_enable(GL_DEPTH_TEST);

    r3d_driver_set_stencil_mask(0x00);
    r3d_driver_set_depth_mask(GL_FALSE);
    r3d_driver_set_depth_func(GL_GREATER);

    int minLevel = r3d_target_get_min_level(R3D_TARGET_SSR);
    int maxLevel = r3d_target_get_max_level(R3D_TARGET_SSR);

    /* --- Calculate SSR --- */

    R3D_TARGET_BIND_CLEAR(minLevel, true, R3D_TARGET_SSR);
    R3D_SHADER_USE(prepare.ssr);

    R3D_SHADER_BIND_SAMPLER(prepare.ssr, uRadianceTex, r3d_target_get_level(R3D_TARGET_RADIANCE, 1));
    R3D_SHADER_BIND_SAMPLER(prepare.ssr, uSpecularTex, r3d_target_get_level(R3D_TARGET_SPECULAR, 1));
    R3D_SHADER_BIND_SAMPLER(prepare.ssr, uNormalTex, r3d_target_get_level(R3D_TARGET_NORMAL, 1));
    R3D_SHADER_BIND_SAMPLER(prepare.ssr, uDepthTex, r3d_target_get_levels(R3D_TARGET_DEPTH, minLevel, maxLevel));

    R3D_RENDER_SCREEN();

    /* --- Downsample --- */

    R3D_SHADER_USE(prepare.blurDown);
    R3D_SHADER_BIND_SAMPLER(prepare.blurDown, uSourceTex, r3d_target_get(R3D_TARGET_SSR));

    for (int iDst = minLevel + 1; iDst <= maxLevel; iDst++)
    {
        r3d_target_set_read_level(R3D_TARGET_SSR, iDst - 1);
        r3d_target_set_write_level(iDst);
        r3d_target_set_viewport(iDst);

        R3D_RENDER_SCREEN();
    }

    /* --- Upsample --- */

    R3D_SHADER_USE(prepare.blurUp);
    R3D_SHADER_BIND_SAMPLER(prepare.blurUp, uSourceTex, r3d_target_get(R3D_TARGET_SSR));

    for (int iDst = minLevel + 1; iDst < maxLevel; iDst++)
    {
        r3d_target_set_read_level(R3D_TARGET_SSR, iDst + 1);
        r3d_target_set_write_level(iDst);
        r3d_target_set_viewport(iDst);

        R3D_RENDER_SCREEN();
    }

    r3d_target_set_read_levels(R3D_TARGET_SSR, minLevel, maxLevel);

    return R3D_TARGET_SSR;
}

// Local patch 23: a light the camera is well outside of (its volume inside the clip box, local
// patch 22) lights only pixels whose surface lies behind that box's front faces. Its box, drawn
// with both faces against the scene depth and no colour, sets the prepass's stencil bit (0
// between passes unless a material's stencil ref sets it, which would lose it under such a
// light's box; monolith's materials don't use stencil) there; the light's quad then runs
// only on those pixels and clears the bit again. Pixels whose surface is in front of the box
// (a facade in front of a room) never reach the lighting shader, which would give them 0.
// The box is grown with the distance so depth precision can't reject a surface lying on it,
// and skipped near the camera, where the near plane would clip its faces.
#define LIGHT_VOLUME_BIT GEOMETRY_PREPASS_STENCIL_BIT

static bool light_volume_mark(const r3d_light_data_t* light)
{
    if (!light->hasBounds) return false;

    Vector3 cam = R3D.viewState.camera.position;
    BoundingBox b = light->bounds;
    Vector3 d = {
        fmaxf(fmaxf(b.min.x - cam.x, cam.x - b.max.x), 0.0f),
        fmaxf(fmaxf(b.min.y - cam.y, cam.y - b.max.y), 0.0f),
        fmaxf(fmaxf(b.min.z - cam.z, cam.z - b.max.z), 0.0f),
    };
    float dist = Vector3Length(d);
    float grow = 0.05f + 0.002f * dist;
    if (dist - grow <= 4.0f * (float)R3D.viewState.camera.nearPlane) return false;

    Vector3 size = Vector3Add(Vector3Subtract(b.max, b.min), (Vector3){ 2.0f * grow, 2.0f * grow, 2.0f * grow });
    Vector3 center = Vector3Scale(Vector3Add(b.min, b.max), 0.5f);
    Matrix model = MatrixMultiply(MatrixScale(size.x, size.y, size.z), MatrixTranslate(center.x, center.y, center.z));
    Matrix modelView = MatrixMultiply(model, R3D.viewState.view);

    R3D_SHADER_USE(deferred.lightVolume);
    R3D_SHADER_SET_MAT4(deferred.lightVolume, uMatProj, R3D.viewState.proj);
    R3D_SHADER_SET_MAT4(deferred.lightVolume, uMatView, modelView);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    r3d_driver_enable(GL_STENCIL_TEST);
    r3d_driver_set_stencil_mask(LIGHT_VOLUME_BIT);
    r3d_driver_set_stencil_func(GL_ALWAYS, LIGHT_VOLUME_BIT, LIGHT_VOLUME_BIT);
    r3d_driver_set_stencil_op(GL_KEEP, GL_KEEP, GL_REPLACE);
    r3d_driver_set_depth_func(GL_LEQUAL);
    R3D_RENDER_CUBE();

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    r3d_driver_set_depth_func(GL_GREATER);
    r3d_driver_set_stencil_func(GL_EQUAL, LIGHT_VOLUME_BIT, LIGHT_VOLUME_BIT);
    r3d_driver_set_stencil_op(GL_KEEP, GL_ZERO, GL_ZERO);
    R3D_SHADER_USE(deferred.lighting);
    return true;
}

// Local patch 25: the light uniform block of one light (the lights pass, the volumetric fog
// march and, for the sun, the ambient pass)
static void upload_light_block(const r3d_light_data_t* light)
{
    r3d_shader_block_light_t data = {
        .viewProj        = MatrixTranspose(light->viewProj),
        .color           = light->color,
        .position        = light->position,
        .direction       = light->direction,
        .energy          = light->energy,
        .specular        = light->specular,
        .range           = light->range,
        .falloff         = light->falloff,
        .innerCutOff     = light->innerCutOff,
        .outerCutOff     = light->outerCutOff,
        .fogEnergy       = light->fogEnergy,
        .shadowSoftness  = light->shadowSoftness,
        .shadowOpacity   = light->shadowOpacity,
        .shadowDepthBias = light->shadowDepthBias,
        .shadowSlopeBias = light->shadowSlopeBias,
        .shadowFar       = light->shadowFar,
        .shadowLayer     = light->shadowLayer,
        .type            = light->type,
        .clipMin         = light->clipMin,
        .clipMax         = light->clipMax,
    };
    r3d_shader_set_uniform_block(R3D_SHADER_BLOCK_LIGHT, &data, true);
}

// Local patch 25: the light the ambient pass lights (the first visible directional light);
// the lights pass skips it
static const r3d_light_data_t* deferred_sun(void)
{
    R3D_LIGHT_FOR_EACH_VISIBLE(light)
    {
        if (light->type == R3D_LIGHT_DIR) return light;
    }
    return NULL;
}

void pass_deferred_lights(void)
{
    /* --- Setup OpenGL pipeline --- */

    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_CULL_FACE);

    r3d_driver_enable(GL_SCISSOR_TEST);
    r3d_driver_enable(GL_DEPTH_TEST);
    r3d_driver_enable(GL_BLEND);

    r3d_driver_set_blend_func(GL_FUNC_ADD, GL_ONE, GL_ONE);
    r3d_driver_set_depth_func(GL_GREATER);
    r3d_driver_set_depth_mask(GL_FALSE);

    /* --- Bind FBO and shader then setup constant stuff --- */

    uint8_t stencilMask;
    bool stencilMaskKnown = r3d_driver_get_stencil_mask(&stencilMask);

    R3D_TARGET_BIND_LOAD(0, true, R3D_TARGET_RADIANCE, R3D_TARGET_SPECULAR);
    R3D_SHADER_USE(deferred.lighting);

    R3D_SHADER_BIND_SAMPLER(deferred.lighting, uAlbedoTex, r3d_target_get_level(R3D_TARGET_ALBEDO, 0));
    R3D_SHADER_BIND_SAMPLER(deferred.lighting, uNormalTex, r3d_target_get_level(R3D_TARGET_NORMAL, 0));
    R3D_SHADER_BIND_SAMPLER(deferred.lighting, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 0));
    R3D_SHADER_BIND_SAMPLER(deferred.lighting, uOrmTex, r3d_target_get_level(R3D_TARGET_ORM, 0));

    /* --- Calculate lighting contributions --- */

    const r3d_light_data_t* sun = deferred_sun();

    R3D_LIGHT_FOR_EACH_VISIBLE(light)
    {
        if (light == sun) continue;   // lit by the ambient pass (local patch 25)

        // Set scissors rect
        r3d_rect_t dst = r3d_light_screen_rect(light, R3D_TARGET_SIZE_W, R3D_TARGET_SIZE_H);
        r3d_driver_set_scissor(dst.x, dst.y, dst.w, dst.h);

        // Send light data to the GPU
        upload_light_block(light);   // (local patch 25: shared helper)

        // Accumulate this light! (only where its box marked the stencil, local patch 23)
        if (!light_volume_mark(light)) r3d_driver_disable(GL_STENCIL_TEST);
        R3D_RENDER_SCREEN();
    }

    r3d_driver_disable(GL_STENCIL_TEST);
    if (stencilMaskKnown) r3d_driver_set_stencil_mask(stencilMask);
    else r3d_driver_set_stencil_mask(0xFF);

    /* --- Reset undesired states --- */

    r3d_driver_set_scissor(0, 0, R3D_TARGET_SIZE_W, R3D_TARGET_SIZE_H);
    r3d_driver_disable(GL_SCISSOR_TEST);
}

void pass_deferred_ambient(r3d_target_t ssaoSource, r3d_target_t ssilSource, r3d_target_t ssgiSource, r3d_target_t composeTarget)
{
    /* --- Setup OpenGL pipeline --- */

    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_CULL_FACE);

    r3d_driver_enable(GL_DEPTH_TEST);

    r3d_driver_set_depth_func(GL_GREATER);
    r3d_driver_set_depth_mask(GL_FALSE);

    /* --- Calculation and composition of ambient/indirect lighting --- */

    // Local patch 26: with a compose target, write the composed scene there (as
    // pass_deferred_compose does: no blending), reading the two light targets
    bool compose = composeTarget != R3D_TARGET_INVALID;
    if (compose)
    {
        r3d_driver_disable(GL_BLEND);
        r3d_driver_set_stencil_mask(0x00);
        R3D_TARGET_BIND_LOAD(0, true, composeTarget);
    }
    else
    {
        // Set additive blending to accumulate light contributions
        r3d_driver_enable(GL_BLEND);
        r3d_driver_set_blend_func(GL_FUNC_ADD, GL_ONE, GL_ONE);
        R3D_TARGET_BIND_LOAD(0, true, R3D_TARGET_RADIANCE, R3D_TARGET_SPECULAR);
    }
    R3D_SHADER_USE(deferred.ambient);

    R3D_SHADER_SET_INT(deferred.ambient, uCompose, compose ? 1 : 0);
    if (compose)
    {
        R3D_SHADER_BIND_SAMPLER(deferred.ambient, uRadianceTex, r3d_target_get_level(R3D_TARGET_RADIANCE, 0));
        R3D_SHADER_BIND_SAMPLER(deferred.ambient, uSpecularTex, r3d_target_get_level(R3D_TARGET_SPECULAR, 0));
    }

    R3D_SHADER_BIND_SAMPLER(deferred.ambient, uAlbedoTex, r3d_target_get_level(R3D_TARGET_ALBEDO, 0));
    R3D_SHADER_BIND_SAMPLER(deferred.ambient, uNormalTex, r3d_target_get_level(R3D_TARGET_NORMAL, 0));
    R3D_SHADER_BIND_SAMPLER(deferred.ambient, uDepthTex, r3d_target_get_levels(R3D_TARGET_DEPTH, 0, 1));
    R3D_SHADER_BIND_SAMPLER(deferred.ambient, uSsaoTex, r3d_target_get_or_null(ssaoSource));
    R3D_SHADER_BIND_SAMPLER(deferred.ambient, uSsilTex, r3d_target_get_or_null(ssilSource));
    R3D_SHADER_BIND_SAMPLER(deferred.ambient, uSsgiTex, r3d_target_get_or_null(ssgiSource));
    R3D_SHADER_BIND_SAMPLER(deferred.ambient, uOrmTex, r3d_target_get_level(R3D_TARGET_ORM, 0));

    // Local patch 11: baked world occlusion
    R3D_SHADER_SET_INT(deferred.ambient, uWorldAoEnabled, worldAo.texture != 0 ? 1 : 0);
    if (worldAo.texture != 0)
    {
        R3D_SHADER_BIND_SAMPLER(deferred.ambient, uWorldAoTex, worldAo.texture);
        R3D_SHADER_SET_VEC3(deferred.ambient, uWorldAoMin, worldAo.min);
        R3D_SHADER_SET_VEC3(deferred.ambient, uWorldAoInvExtent, worldAo.invExtent);
        R3D_SHADER_SET_VEC2(deferred.ambient, uWorldAoDecode, worldAo.decode);
        R3D_SHADER_SET_FLOAT(deferred.ambient, uWorldAoIntensity, worldAo.intensity);
    }

    // Local patch 25: the sun, see deferred_sun()
    const r3d_light_data_t* sun = deferred_sun();
    R3D_SHADER_SET_INT(deferred.ambient, uSunEnabled, sun != NULL ? 1 : 0);
    if (sun != NULL)
    {
        GLuint minMax = r3d_light_shadow_minmax_map();
        upload_light_block(sun);
        R3D_SHADER_SET_INT(deferred.ambient, uSunMinMax, minMax != 0 ? 1 : 0);
        if (minMax != 0) R3D_SHADER_BIND_SAMPLER(deferred.ambient, uShadowDirMinMaxTex, minMax);
    }

    R3D_RENDER_SCREEN();
}

void pass_deferred_compose(r3d_target_t sceneTarget, r3d_target_t ssrSource)
{
    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_CULL_FACE);
    r3d_driver_disable(GL_BLEND);

    r3d_driver_enable(GL_DEPTH_TEST);

    r3d_driver_set_stencil_mask(0x00);
    r3d_driver_set_depth_mask(GL_FALSE);
    r3d_driver_set_depth_func(GL_GREATER);

    R3D_TARGET_BIND_LOAD(0, true, sceneTarget);
    R3D_SHADER_USE(deferred.compose);

    R3D_SHADER_BIND_SAMPLER(deferred.compose, uAlbedoTex, r3d_target_get_level(R3D_TARGET_ALBEDO, 0));
    R3D_SHADER_BIND_SAMPLER(deferred.compose, uRadianceTex, r3d_target_get_level(R3D_TARGET_RADIANCE, 0));
    R3D_SHADER_BIND_SAMPLER(deferred.compose, uSpecularTex, r3d_target_get_level(R3D_TARGET_SPECULAR, 0));
    R3D_SHADER_BIND_SAMPLER(deferred.compose, uOrmTex, r3d_target_get_level(R3D_TARGET_ORM, 0));
    R3D_SHADER_BIND_SAMPLER(deferred.compose, uSsrTex, R3D_TEXTURE_SELECT(r3d_target_get_or_null(ssrSource), BLANK));

    R3D_SHADER_SET_FLOAT(deferred.compose, uSsrNumLevels, (float)r3d_target_get_num_levels(R3D_TARGET_SSR));

    R3D_RENDER_SCREEN();
}

void pass_deferred_fog(r3d_target_t sceneTarget)
{
    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_DEPTH_TEST);
    r3d_driver_disable(GL_CULL_FACE);

    r3d_driver_enable(GL_BLEND);
    r3d_driver_set_blend_func_separate(GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // Local patch 20: bind the scene with the depth attachment, the FBO the passes before and
    // after use, to save two framebuffer switches; the depth test is off here, so the
    // attached depth buffer is neither tested nor written
    R3D_TARGET_BIND_LOAD(0, true, sceneTarget);
    R3D_SHADER_USE(deferred.fog);

    R3D_SHADER_BIND_SAMPLER(deferred.fog, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 0));
    R3D_RENDER_SCREEN();
}

void pass_deferred_volumetric_fog(r3d_target_t sceneTarget)
{
    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_DEPTH_TEST);
    r3d_driver_disable(GL_CULL_FACE);

    // Local patch 24: no separate transmittance pass (vfog_transmittance.frag); the
    // composition below applies it, together with the distance fog (pass_deferred_fog)

    /* --- Accumulate radiance in half resolution --- */

    PROF_BEGIN(R3D_PROFILER_VFOG_RADIANCE);
    R3D_TARGET_BIND_CLEAR(1, false, R3D_TARGET_VFOG_RAD);

    r3d_driver_enable(GL_SCISSOR_TEST);
    r3d_driver_enable(GL_BLEND);

    r3d_driver_set_blend_func(GL_FUNC_ADD, GL_ONE, GL_ONE);

    R3D_SHADER_USE(deferred.vfogRadiance);
    R3D_SHADER_BIND_SAMPLER(deferred.vfogRadiance, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 1));
    R3D_SHADER_BIND_SAMPLER(deferred.vfogRadiance, uShadowDirMinMaxTex, r3d_light_shadow_minmax_map());

    // Baked sun visibility (local patch, see pass_scene_fog_sunvol)
    const r3d_light_fog_sunvol_t* sunVol = &R3D_MOD_LIGHT.fogSunVol;
    R3D_SHADER_SET_INT(deferred.vfogRadiance, uFogSunVolEnabled, sunVol->valid ? 1 : 0);
    if (sunVol->valid)
    {
        R3D_SHADER_BIND_SAMPLER(deferred.vfogRadiance, uFogSunVolTex, sunVol->texture);
        R3D_SHADER_SET_VEC3(deferred.vfogRadiance, uFogSunVolMin, sunVol->min);
        R3D_SHADER_SET_VEC3(deferred.vfogRadiance, uFogSunVolInvExtent,
            (Vector3){ 1.0f / sunVol->extent.x, 1.0f / sunVol->extent.y, 1.0f / sunVol->extent.z });
        R3D_SHADER_SET_VEC3(deferred.vfogRadiance, uFogSunVolHalfTexel,
            (Vector3){ 0.5f / sunVol->dims[0], 0.5f / sunVol->dims[1], 0.5f / sunVol->dims[2] });
    }

    R3D_LIGHT_FOR_EACH_VISIBLE(light)
    {
        if (light->fogEnergy == 0.0f) continue;

        r3d_rect_t dst = r3d_light_screen_rect(light, R3D_TARGET_SIZE_W / 2, R3D_TARGET_SIZE_H / 2);
        r3d_driver_set_scissor(dst.x, dst.y, dst.w, dst.h);

        upload_light_block(light);   // (local patch 25: shared helper)

        R3D_RENDER_SCREEN();
    }

    r3d_driver_set_scissor(0, 0, R3D_TARGET_SIZE_W, R3D_TARGET_SIZE_H);
    r3d_driver_disable(GL_SCISSOR_TEST);
    PROF_END();

    /* --- Compose radiance to the scene --- */

    PROF_BEGIN(R3D_PROFILER_VFOG_COMPOSE);
    R3D_TARGET_BIND_LOAD(0, true, sceneTarget);   // with depth: see pass_deferred_fog (local patch 20)
    R3D_SHADER_USE(deferred.vfogCompose);

    // Local patch 24: scene = added light + scene * factor, alpha = fog + alpha (1 - fog),
    // with both factors from the second (dual-source) output, see vfog_compose.frag
    r3d_driver_enable(GL_BLEND);
    r3d_driver_set_blend_func_separate(GL_FUNC_ADD, GL_ONE, GL_SRC1_COLOR, GL_ONE, GL_SRC1_ALPHA);

    R3D_SHADER_BIND_SAMPLER(deferred.vfogCompose, uRadianceTex, r3d_target_get_level(R3D_TARGET_VFOG_RAD, 1));
    R3D_SHADER_BIND_SAMPLER(deferred.vfogCompose, uDepthTex, r3d_target_get_levels(R3D_TARGET_DEPTH, 0, 1));

    R3D_RENDER_SCREEN();
    PROF_END();
}

void pass_scene_forward(r3d_target_t sceneTarget)
{
    R3D_TARGET_BIND_LOAD(0, true, sceneTarget);

    r3d_driver_enable(GL_STENCIL_TEST);
    r3d_driver_enable(GL_DEPTH_TEST);

    /* --- Render all unlit opaque --- */

    r3d_driver_disable(GL_BLEND);
    r3d_driver_set_depth_mask(GL_TRUE);

    #define COND (IS_MESH_VISIBLE_CAMERA(call->mesh.instance) && (call->mesh.material.unlit))
    R3D_RENDER_FOR_EACH(call, COND, &R3D.viewState.frustum, R3D_RENDER_LIST_OPAQUE_INST, R3D_RENDER_LIST_OPAQUE)
    {
        raster_unlit(call, true);
    }
    #undef COND

    /* --- Render all lit/unlit blended --- */

    r3d_driver_enable(GL_BLEND);
    r3d_driver_set_depth_mask(GL_FALSE);

    R3D_RENDER_FOR_EACH(call, IS_MESH_VISIBLE_CAMERA(call->mesh.instance), &R3D.viewState.frustum, R3D_RENDER_LIST_BLEND_INST, R3D_RENDER_LIST_BLEND)
    {
        if (!call->mesh.material.unlit)
        {
            upload_light_array_block_for_mesh(call, true);
            raster_forward(call);
        }
        else
        {
            raster_unlit(call, false);
        }
    }

    /* --- Reset undesired states --- */

    r3d_driver_set_depth_offset(0.0f, 0.0f);
    r3d_driver_set_depth_range(0.0f, 1.0f);
}

void pass_scene_background(r3d_target_t sceneTarget)
{
    R3D_TARGET_BIND_LOAD(0, true, sceneTarget);

    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_CULL_FACE);
    r3d_driver_disable(GL_BLEND);

    r3d_driver_enable(GL_DEPTH_TEST);
    r3d_driver_set_depth_func(GL_LEQUAL);
    r3d_driver_set_depth_mask(GL_FALSE);

    const R3D_EnvBackground* bg = &R3D.environment.background;

    if (bg->sky.texture != 0)
    {
        R3D_SHADER_USE(scene.skybox);
        float lod = (float)r3d_get_mip_levels_1d(bg->sky.size);
        R3D_SHADER_BIND_SAMPLER(scene.skybox, uSkyMap, bg->sky.texture);
        R3D_SHADER_SET_FLOAT(scene.skybox, uEnergy, bg->energy);
        R3D_SHADER_SET_FLOAT(scene.skybox, uLod, bg->skyBlur * lod);
        R3D_SHADER_SET_VEC4(scene.skybox, uRotation, bg->rotation);
        R3D_SHADER_SET_MAT4(scene.skybox, uMatInvView, R3D.viewState.invView);
        R3D_SHADER_SET_MAT4(scene.skybox, uMatInvProj, R3D.viewState.invProj);
    }
    else
    {
        Vector4 bgColor = r3d_color_srgb_to_linear_vec4(bg->color);
        float bgPremult = bg->energy * bgColor.w;

        R3D_SHADER_USE(scene.background);
        R3D_SHADER_SET_VEC4(scene.background, uColor, (Vector4) {
            bgColor.x * bgPremult,
            bgColor.y * bgPremult,
            bgColor.z * bgPremult,
            bgColor.w
        });
    }

    R3D_RENDER_SCREEN();
}

r3d_target_t pass_post_setup(r3d_target_t sceneTarget)
{
    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_DEPTH_TEST);
    r3d_driver_disable(GL_CULL_FACE);
    r3d_driver_disable(GL_BLEND);

    return r3d_target_swap_scene(sceneTarget);
}

r3d_target_t pass_post_dof(r3d_target_t sceneTarget)	
{
    /* --- Calculate CoC --- */

    R3D_TARGET_BIND_CLEAR(0, false, R3D_TARGET_DOF_COC);
    R3D_SHADER_USE(prepare.dofCoc);

    R3D_SHADER_BIND_SAMPLER(prepare.dofCoc, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 0));
    R3D_RENDER_SCREEN();

    /* --- Downsample CoC to half resolution --- */

    R3D_TARGET_BIND_CLEAR(1, false, R3D_TARGET_DOF_0);

    R3D_SHADER_USE(prepare.dofDown);
    R3D_SHADER_BIND_SAMPLER(prepare.dofDown, uSceneTex, r3d_target_get(r3d_target_swap_scene(sceneTarget)));
    R3D_SHADER_BIND_SAMPLER(prepare.dofDown, uCoCTex, r3d_target_get(R3D_TARGET_DOF_COC));

    R3D_RENDER_SCREEN();

    /* --- Calculate DoF in half resolution --- */

    R3D_TARGET_BIND_CLEAR(1, false, R3D_TARGET_DOF_1);

    R3D_SHADER_USE(prepare.dofBlur);
    R3D_SHADER_BIND_SAMPLER(prepare.dofBlur, uSceneTex, r3d_target_get(R3D_TARGET_DOF_0));
    R3D_SHADER_BIND_SAMPLER(prepare.dofBlur, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 1));

    R3D_RENDER_SCREEN();

    /* --- Compose DoF with the scene ---  */

    R3D_TARGET_BIND_AND_SWAP_SCENE(sceneTarget);
    R3D_SHADER_USE(post.dof);

    R3D_SHADER_BIND_SAMPLER(post.dof, uSceneTex, r3d_target_get(sceneTarget));
    R3D_SHADER_BIND_SAMPLER(post.dof, uBlurTex, r3d_target_get(R3D_TARGET_DOF_1));

    R3D_RENDER_SCREEN();

    return sceneTarget;
}

r3d_target_t pass_post_bloom(r3d_target_t sceneTarget, bool apply)
{
    r3d_target_t sceneSource = r3d_target_swap_scene(sceneTarget);
    GLuint sceneSourceID = r3d_target_get(sceneSource);

    /* --- Compute mip count to sample --- */

    int minLevel = r3d_target_get_min_level(R3D_TARGET_BLOOM);
    int maxLevel = r3d_target_get_max_level(R3D_TARGET_BLOOM);

    maxLevel = (int)(Lerp((float)(minLevel + 1), (float)(maxLevel), R3D.environment.bloom.levels) + 0.5f);

    /* --- Karis average for the first downsampling to half res --- */

    R3D_TARGET_BIND_CLEAR(minLevel, false, R3D_TARGET_BLOOM);

    R3D_SHADER_USE(prepare.bloomDown);
    R3D_SHADER_BIND_SAMPLER(prepare.bloomDown, uTexture, sceneSourceID);
    R3D_SHADER_SET_VEC2(prepare.bloomDown, uTexelSize, r3d_target_get_texel_size(0));
    R3D_SHADER_SET_INT(prepare.bloomDown, uFirstPass, true);

    R3D_RENDER_SCREEN();

    /* --- Bloom Downsampling --- */

    R3D_SHADER_BIND_SAMPLER(prepare.bloomDown, uTexture, r3d_target_get(R3D_TARGET_BLOOM));
    R3D_SHADER_SET_INT(prepare.bloomDown, uFirstPass, false);

    for (int dstLevel = minLevel + 1; dstLevel <= maxLevel; dstLevel++)
    {
        r3d_target_set_read_level(R3D_TARGET_BLOOM, dstLevel - 1);
        r3d_target_set_write_level(dstLevel);
        r3d_target_set_viewport(dstLevel);

        R3D_SHADER_SET_VEC2(prepare.bloomDown, uTexelSize, r3d_target_get_texel_size(dstLevel - 1));
        R3D_RENDER_SCREEN();
    }

    /* --- Bloom Upsampling --- */

    r3d_driver_enable(GL_BLEND);
    r3d_driver_set_blend_func(GL_FUNC_ADD, GL_ONE, GL_ONE);

    R3D_SHADER_USE(prepare.bloomUp);
    R3D_SHADER_BIND_SAMPLER(prepare.bloomUp, uTexture, r3d_target_get(R3D_TARGET_BLOOM));

    for (int dstLevel = maxLevel - 1; dstLevel >= minLevel; dstLevel--)
    {
        r3d_target_set_read_level(R3D_TARGET_BLOOM, dstLevel + 1);
        r3d_target_set_write_level(dstLevel);
        r3d_target_set_viewport(dstLevel);

        Vector2 filterRadius = r3d_target_get_texel_size(dstLevel + 1);
        filterRadius = Vector2Scale(filterRadius, R3D.environment.bloom.filterRadius);
        R3D_SHADER_SET_VEC2(prepare.bloomUp, uFilterRadius, filterRadius);

        R3D_RENDER_SCREEN();
    }

    r3d_driver_disable(GL_BLEND);

    // Monolith patch 10: without `apply` the output pass composites the bloom (uBloomFused)
    if (!apply) return sceneTarget;

    /* --- Apply bloom to the scene --- */

    R3D_TARGET_BIND_AND_SWAP_SCENE(sceneTarget);

    R3D_SHADER_USE(post.bloom);
    R3D_SHADER_BIND_SAMPLER(post.bloom, uSceneTex, sceneSourceID);
    R3D_SHADER_BIND_SAMPLER(post.bloom, uBloomTex, r3d_target_get_level(R3D_TARGET_BLOOM, minLevel));

    R3D_RENDER_SCREEN();

    return sceneTarget;
}

r3d_target_t pass_post_auto_exposure(r3d_target_t sceneTarget)
{
    r3d_target_t sceneSource = r3d_target_swap_scene(sceneTarget);
    GLuint sceneSourceID = r3d_target_get(sceneSource);

    /* --- Build log-luminance pyramid --- */

    R3D_TARGET_BIND_CLEAR(1, false, R3D_TARGET_LUMINANCE);

    R3D_SHADER_USE(prepare.luminance);
    R3D_SHADER_BIND_SAMPLER(prepare.luminance, uSourceTex, sceneSourceID);
    R3D_RENDER_SCREEN();

    r3d_target_gen_mipmap(R3D_TARGET_LUMINANCE);

    /* --- Update auto-exposure history on GPU --- */

    const R3D_EnvAutoExposure *autoExposure = &R3D.environment.autoExposure;

    float minLogLum = R3D_LOG018 + autoExposure->minEV * R3D_LOG2;
    float maxLogLum = R3D_LOG018 + autoExposure->maxEV * R3D_LOG2;

    float timeToBright = fmaxf(autoExposure->adaptationToBright, 1e-3f);
    float timeToDark   = fmaxf(autoExposure->adaptationToDark,   1e-3f);

    float speedUp   = 1.0f / timeToBright;
    float speedDown = 1.0f / timeToDark;

    float exposureCompLog = autoExposure->exposureCompensation * R3D_LOG2;

    static r3d_target_t EXPOSURE_DST = R3D_TARGET_EXPOSURE_0;
    static r3d_target_t EXPOSURE_SRC = R3D_TARGET_EXPOSURE_1;

    if (!r3d_target_exists(EXPOSURE_SRC))
    {
        R3D_TARGET_BIND_CLEAR(r3d_target_get_max_level(EXPOSURE_SRC), false, EXPOSURE_SRC);
    }

    R3D_TARGET_BIND_CLEAR(r3d_target_get_max_level(EXPOSURE_DST), false, EXPOSURE_DST);
    R3D_SHADER_USE(prepare.exposureAdapt);

    R3D_SHADER_BIND_SAMPLER(prepare.exposureAdapt, uMeasuredLogLumTex, r3d_target_get_level(R3D_TARGET_LUMINANCE, r3d_target_get_max_level(R3D_TARGET_LUMINANCE)));
    R3D_SHADER_BIND_SAMPLER(prepare.exposureAdapt, uPrevAutoExposureTex, r3d_target_get(EXPOSURE_SRC));

    R3D_SHADER_SET_FLOAT(prepare.exposureAdapt, uDeltaTime, GetFrameTime());
    R3D_SHADER_SET_FLOAT(prepare.exposureAdapt, uMinLogLum, minLogLum);
    R3D_SHADER_SET_FLOAT(prepare.exposureAdapt, uMaxLogLum, maxLogLum);
    R3D_SHADER_SET_FLOAT(prepare.exposureAdapt, uSpeedUp, speedUp);
    R3D_SHADER_SET_FLOAT(prepare.exposureAdapt, uSpeedDown, speedDown);
    R3D_SHADER_SET_FLOAT(prepare.exposureAdapt, uExposureCompLog, exposureCompLog);

    R3D_RENDER_SCREEN();

    R3D_SWAP(r3d_target_t, EXPOSURE_DST, EXPOSURE_SRC);

    /* --- Apply exposure --- */

    R3D_TARGET_BIND_AND_SWAP_SCENE(sceneTarget);

    R3D_SHADER_USE(post.autoExposure);
    R3D_SHADER_BIND_SAMPLER(post.autoExposure, uSceneTex, sceneSourceID);
    R3D_SHADER_BIND_SAMPLER(post.autoExposure, uExposureTex, r3d_target_get(EXPOSURE_SRC));
    R3D_RENDER_SCREEN();

    return sceneTarget;
}

r3d_target_t pass_post_screen(R3D_ScreenShaderStage stage, r3d_target_t sceneTarget)
{
    for (int i = 0; i < (int)R3D_ARRAY_SIZE(R3D.screenShaders[stage]); i++)
    {
        R3D_ScreenShader* shader = R3D.screenShaders[stage][i];
        if (shader == NULL) continue;

        R3D_TARGET_BIND_AND_SWAP_SCENE(sceneTarget);
        R3D_SHADER_USE_CUSTOM(R3D.screenShaders[stage][i], post.screen);

        R3D_SHADER_BIND_SAMPLER_CUSTOM(shader, post.screen, uSceneTex, r3d_target_get(sceneTarget));
        R3D_SHADER_BIND_SAMPLER_CUSTOM(shader, post.screen, uNormalTex, r3d_target_get_level(R3D_TARGET_NORMAL, 0));
        R3D_SHADER_BIND_SAMPLER_CUSTOM(shader, post.screen, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 0));

        R3D_RENDER_SCREEN();
    }

    return sceneTarget;
}

r3d_target_t pass_post_output(r3d_target_t sceneTarget, bool bloomFused)
{
    R3D_TARGET_BIND_AND_SWAP_SCENE(sceneTarget);
    R3D_SHADER_USE(post.output);

    R3D_SHADER_BIND_SAMPLER(post.output, uSceneTex, r3d_target_get(sceneTarget));
    R3D_SHADER_SET_INT(post.output, uBloomFused, bloomFused ? 1 : 0);
    if (bloomFused)
    {
        int minLevel = r3d_target_get_min_level(R3D_TARGET_BLOOM);
        R3D_SHADER_BIND_SAMPLER(post.output, uBloomTex, r3d_target_get_level(R3D_TARGET_BLOOM, minLevel));
    }
    R3D_RENDER_SCREEN();

    return sceneTarget;
}

r3d_target_t pass_post_fxaa(r3d_target_t sceneTarget)
{
    R3D_TARGET_BIND_AND_SWAP_SCENE(sceneTarget);
    R3D_SHADER_USE(post.fxaa[R3D.aaPreset]);

    R3D_SHADER_BIND_SAMPLER(post.fxaa[R3D.aaPreset], uSceneTex, r3d_target_get(sceneTarget));
    R3D_RENDER_SCREEN();

    return sceneTarget;
}

// Monolith patch 10: whether the last post pass may write the final target itself. That is
// the case when blit_to_screen() would be a plain copy: scene output, internal resolution ==
// destination viewport size, viewport at the origin (the copy and FXAA address texels by
// gl_FragCoord), and no FINAL stage screen shaders after the AA pass.
bool final_pass_direct(void)
{
    if (R3D.outputMode != R3D_OUTPUT_SCENE) return false;

    for (int i = 0; i < (int)R3D_ARRAY_SIZE(R3D.screenShaders[R3D_SCREEN_SHADER_STAGE_FINAL]); i++)
    {
        if (R3D.screenShaders[R3D_SCREEN_SHADER_STAGE_FINAL][i] != NULL) return false;
    }

    Rectangle viewport = R3D.viewState.viewport;
    int dstX = (int)(viewport.x + 0.5f);
    int dstY = (int)(viewport.y + 0.5f);
    int dstW = (int)(viewport.width + 0.5f);
    int dstH = (int)(viewport.height + 0.5f);

    GLuint dstId = R3D.screen.id;
    int targetH = dstId ? R3D.screen.texture.height : GetRenderHeight();
    int glDstY = targetH - dstY - dstH;

    int srcW = 0, srcH = 0;
    r3d_target_get_resolution(&srcW, &srcH, 0);

    return dstX == 0 && glDstY == 0 && dstW == srcW && dstH == srcH;
}

// FXAA into the final target, with the scene depth like blit_to_screen()'s copy
void pass_post_fxaa_to_screen(r3d_target_t sceneTarget)
{
    r3d_target_t source = r3d_target_swap_scene(sceneTarget);

    int srcW = 0, srcH = 0;
    r3d_target_get_resolution(&srcW, &srcH, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, R3D.screen.id);
    glViewport(0, 0, srcW, srcH);

    r3d_driver_disable(GL_BLEND);

    // Local patch 24: without R3D_SetOutputDepth the depth isn't copied, and the depth
    // texture isn't bound (a bound depth texture costs a HiZ resolve on some GPUs)
    if (R3D.outputDepth)
    {
        r3d_driver_enable(GL_DEPTH_TEST);
        r3d_driver_set_depth_func(GL_ALWAYS);
        r3d_driver_set_depth_mask(GL_TRUE);
    }
    else
    {
        r3d_driver_disable(GL_DEPTH_TEST);
    }

    R3D_SHADER_USE(post.fxaa[R3D.aaPreset]);
    R3D_SHADER_BIND_SAMPLER(post.fxaa[R3D.aaPreset], uSceneTex, r3d_target_get(source));
    R3D_SHADER_BIND_SAMPLER(post.fxaa[R3D.aaPreset], uDepthTex,
        R3D.outputDepth ? r3d_target_get_depth_buffer() : r3d_texture_get(R3D_TEXTURE_BLANK));
    R3D_SHADER_SET_INT(post.fxaa[R3D.aaPreset], uWriteDepth, R3D.outputDepth ? 1 : 0);
    R3D_RENDER_SCREEN();
}

r3d_target_t pass_post_smaa(r3d_target_t sceneTarget)
{
    r3d_target_t sceneSource = r3d_target_swap_scene(sceneTarget);

    /* --- Clear previous content --- */

    // Bind and clear the stencil buffer. Since AA is the last post-processing
    // pass, clearing it here is safe. The stencil is used to avoid running the
    // blending weight calculation on pixels that have no edges, pass 1 writes 1
    // to the stencil for each edge pixel (non-edge pixels are discarded by the
    // shader), then pass 2 only executes where stencil == 1.

    r3d_driver_enable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_DEPTH_TEST);

    r3d_driver_set_stencil_mask(0xFF);

    R3D_TARGET_BIND_CLEAR(0, true, R3D_TARGET_SMAA_EDGES, R3D_TARGET_SMAA_BLEND);

    /* --- Edge detection ---  */

    r3d_driver_set_stencil_func(GL_ALWAYS, 1, 0xFF);
    r3d_driver_set_stencil_op(GL_KEEP, GL_KEEP, GL_REPLACE);

    R3D_TARGET_BIND_LOAD(0, true, R3D_TARGET_SMAA_EDGES);
    R3D_SHADER_USE(prepare.smaaEdgeDetection[R3D.aaPreset]);

    R3D_SHADER_BIND_SAMPLER(prepare.smaaEdgeDetection[R3D.aaPreset], uSceneTex, r3d_target_get(sceneSource));

    R3D_RENDER_SCREEN();

    /* --- Compute blending weights --- */

    r3d_driver_set_stencil_func(GL_EQUAL, 1, 0xFF);
    r3d_driver_set_stencil_op(GL_KEEP, GL_KEEP, GL_KEEP);

    R3D_TARGET_BIND_LOAD(0, true, R3D_TARGET_SMAA_BLEND);
    R3D_SHADER_USE(prepare.smaaBlendingWeights[R3D.aaPreset]);

    R3D_SHADER_BIND_SAMPLER(prepare.smaaBlendingWeights[R3D.aaPreset], uEdgesTex, r3d_target_get(R3D_TARGET_SMAA_EDGES));
    R3D_SHADER_BIND_SAMPLER(prepare.smaaBlendingWeights[R3D.aaPreset], uAreaTex, r3d_texture_get(R3D_TEXTURE_SMAA_AREA));
    R3D_SHADER_BIND_SAMPLER(prepare.smaaBlendingWeights[R3D.aaPreset], uSearchTex, r3d_texture_get(R3D_TEXTURE_SMAA_SEARCH));

    R3D_RENDER_SCREEN();

    /* --- Apply anti aliasing to the scene --- */

    r3d_driver_disable(GL_STENCIL_TEST);

    R3D_TARGET_BIND_AND_SWAP_SCENE(sceneTarget);
    R3D_SHADER_USE(post.smaa[R3D.aaPreset]);

    R3D_SHADER_BIND_SAMPLER(post.smaa[R3D.aaPreset], uSceneTex, r3d_target_get(sceneTarget));
    R3D_SHADER_BIND_SAMPLER(post.smaa[R3D.aaPreset], uBlendTex, r3d_target_get(R3D_TARGET_SMAA_BLEND));

    R3D_RENDER_SCREEN();

    return sceneTarget;
}

void blit_to_screen(r3d_target_t source)
{
    if (!r3d_target_exists(source))
    {
        // TODO: Put a log
        return;
    }

    GLuint dstId = R3D.screen.id;

    int targetW = dstId ? R3D.screen.texture.width  : GetRenderWidth();
    int targetH = dstId ? R3D.screen.texture.height : GetRenderHeight();

    if (targetW <= 0) targetW = 1;
    if (targetH <= 0) targetH = 1;

    Rectangle viewport = R3D.viewState.viewport;

    int dstX = (int)(viewport.x + 0.5f);
    int dstY = (int)(viewport.y + 0.5f);
    int dstW = (int)(viewport.width + 0.5f);
    int dstH = (int)(viewport.height + 0.5f);

    if (dstW <= 0 || dstH <= 0)
    {
        // TODO: Put a log
        return;
    }

    int glDstY = targetH - dstY - dstH;
    int srcW = 0;
    int srcH = 0;

    r3d_target_get_resolution(&srcW, &srcH, 0);

    if (srcW <= 0 || srcH <= 0)
    {
        // TODO: Put a log
        return;
    }

    int dstSq = dstW * dstH;
    int srcSq = srcW * srcH;
    int sign = (dstSq > srcSq) - (dstSq < srcSq);

    glBindFramebuffer(GL_FRAMEBUFFER, dstId);
    glViewport(dstX, glDstY, dstW, dstH);

    r3d_driver_enable(GL_DEPTH_TEST);
    r3d_driver_disable(GL_BLEND);

    r3d_driver_set_depth_func(GL_ALWAYS);
    r3d_driver_set_depth_mask(GL_TRUE);

    if (sign > 0)
    {
        switch (R3D.upscaleMode)
        {
        case R3D_UPSCALE_NEAREST:
            R3D_SHADER_USE(blit.commonNearest);
            R3D_SHADER_BIND_SAMPLER(blit.commonNearest, uSourceTex, r3d_target_get(source));
            R3D_SHADER_BIND_SAMPLER(blit.commonNearest, uDepthTex, r3d_target_get_depth_buffer());
            R3D_RENDER_SCREEN();
            break;
        case R3D_UPSCALE_LINEAR:
            R3D_SHADER_USE(blit.commonLinear);
            R3D_SHADER_BIND_SAMPLER(blit.commonLinear, uSourceTex, r3d_target_get(source));
            R3D_SHADER_BIND_SAMPLER(blit.commonLinear, uDepthTex, r3d_target_get_depth_buffer());
            R3D_RENDER_SCREEN();
            break;
        case R3D_UPSCALE_BICUBIC:
            R3D_SHADER_USE(blit.upBicubic);
            R3D_SHADER_BIND_SAMPLER(blit.upBicubic, uSourceTex, r3d_target_get(source));
            R3D_SHADER_BIND_SAMPLER(blit.upBicubic, uDepthTex, r3d_target_get_depth_buffer());
            R3D_RENDER_SCREEN();
            break;
        case R3D_UPSCALE_LANCZOS:
            R3D_SHADER_USE(blit.upLanczos);
            R3D_SHADER_BIND_SAMPLER(blit.upLanczos, uSourceTex, r3d_target_get(source));
            R3D_SHADER_BIND_SAMPLER(blit.upLanczos, uDepthTex, r3d_target_get_depth_buffer());
            R3D_RENDER_SCREEN();
            break;
        default:
            break;
        }
    }
    else if (sign < 0)
    {
        switch (R3D.downscaleMode)
        {
        case R3D_DOWNSCALE_NEAREST:
            R3D_SHADER_USE(blit.commonNearest);
            R3D_SHADER_BIND_SAMPLER(blit.commonNearest, uSourceTex, r3d_target_get(source));
            R3D_SHADER_BIND_SAMPLER(blit.commonNearest, uDepthTex, r3d_target_get_depth_buffer());
            R3D_RENDER_SCREEN();
            break;
        case R3D_DOWNSCALE_LINEAR:
            R3D_SHADER_USE(blit.commonLinear);
            R3D_SHADER_BIND_SAMPLER(blit.commonLinear, uSourceTex, r3d_target_get(source));
            R3D_SHADER_BIND_SAMPLER(blit.commonLinear, uDepthTex, r3d_target_get_depth_buffer());
            R3D_RENDER_SCREEN();
            break;
        case R3D_DOWNSCALE_RGSS:
            R3D_SHADER_USE(blit.downRgss);
            R3D_SHADER_SET_VEC2(blit.downRgss, uDestTexel, (Vector2) {1.0f / dstW, 1.0f / dstH});
            R3D_SHADER_BIND_SAMPLER(blit.downRgss, uSourceTex, r3d_target_get(source));
            R3D_RENDER_SCREEN();
            break;
        case R3D_DOWNSCALE_PDSS:
            R3D_SHADER_USE(blit.downPdss);
            R3D_SHADER_SET_VEC2(blit.downPdss, uDestTexel, (Vector2) {1.0f / dstW, 1.0f / dstH});
            R3D_SHADER_BIND_SAMPLER(blit.downPdss, uSourceTex, r3d_target_get(source));
            R3D_RENDER_SCREEN();
            break;
        default:
            break;
        }
    }
    else
    {
        R3D_SHADER_USE(blit.commonCopy);
        R3D_SHADER_BIND_SAMPLER(blit.commonCopy, uSourceTex, r3d_target_get(source));
        R3D_SHADER_BIND_SAMPLER(blit.commonCopy, uDepthTex, r3d_target_get_depth_buffer());
        R3D_RENDER_SCREEN();
    }
}

void visualize_to_screen(r3d_target_t source)
{
    if (!r3d_target_exists(source))
    {
        // TODO: Put a log
        return;
    }

    GLuint dstId = R3D.screen.id;
    int dstW = dstId ? R3D.screen.texture.width  : GetRenderWidth();
    int dstH = dstId ? R3D.screen.texture.height : GetRenderHeight();

    int dstX = 0, dstY = 0;
    if (R3D.aspectMode == R3D_ASPECT_KEEP)
    {
        float srcRatio = (float)R3D_TARGET_SIZE_W / R3D_TARGET_SIZE_H;
        float dstRatio = (float)dstW / dstH;
        if (srcRatio > dstRatio)
        {
            int newH = (int)(dstW / srcRatio + 0.5f);
            dstY = (dstH - newH) / 2;
            dstH = newH;
        }
        else
        {
            int newW = (int)(dstH * srcRatio + 0.5f);
            dstX = (dstW - newW) / 2;
            dstW = newW;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, dstId);
    glViewport(dstX, dstY, dstW, dstH);

    R3D_SHADER_USE(post.visualizer);
    R3D_SHADER_SET_INT(post.visualizer, uOutputMode, R3D.outputMode);
    R3D_SHADER_BIND_SAMPLER(post.visualizer, uSourceTex, r3d_target_get(source));

    R3D_RENDER_SCREEN();

    r3d_target_blit(-1, true, dstId, dstX, dstY, dstW, dstH, false);
}

void cleanup_after_render(void)
{
    r3d_shader_invalidate_cache();
    r3d_target_invalidate_cache();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);

    r3d_driver_restore_viewport();

    r3d_driver_disable(GL_STENCIL_TEST);
    r3d_driver_disable(GL_DEPTH_TEST);
    r3d_driver_enable(GL_CULL_FACE);
    r3d_driver_enable(GL_BLEND);

    r3d_driver_set_blend_func(GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    r3d_driver_set_depth_offset(0.0f, 0.0f);
    r3d_driver_set_depth_range(0.0f, 1.0f);
    r3d_driver_set_depth_func(GL_LEQUAL);
    r3d_driver_set_depth_mask(GL_TRUE);
    r3d_driver_set_cull_face(GL_BACK);

    // Here we re-define the blend mode via rlgl to ensure its internal state
    // matches what we've just set manually with OpenGL.

    // It's not enough to change the blend mode only through rlgl, because if we
    // previously used a different blend mode (not "alpha") but rlgl still thinks it's "alpha",
    // then rlgl won't correctly apply the intended blend mode.

    // We do this at the end because calling rlSetBlendMode can trigger a draw call for
    // any content accumulated by rlgl, and we want that to be rendered into the main
    // framebuffer, not into one of R3D's internal framebuffers that will be discarded afterward.

    rlSetBlendMode(RL_BLEND_ALPHA);

    // Here we reset the target sampling levels to facilitate debugging with RenderDoc
    // WARNING: Make sure that everything that affects levels works in release mode!

#ifndef NDEBUG
    for (int iTarget = 0; iTarget < R3D_TARGET_COUNT; iTarget++)
    {
        if (r3d_target_exists(iTarget))
        {
            int minLevel = r3d_target_get_min_level(iTarget);
            int maxLevel = r3d_target_get_max_level(iTarget);
            r3d_target_set_read_levels(iTarget, minLevel, maxLevel);
        }
    }
#endif
}

// ========================================
// GPU PROFILER API (local patch, see patches.md)
// ========================================

static const char* PROF_PASS_NAMES[R3D_PROFILER_PASS_COUNT] = {
    [R3D_PROFILER_SHADOWS]            = "shadow maps",
    [R3D_PROFILER_SHADOW_MINMAX]      = "shadow min/max",
    [R3D_PROFILER_PROBES]             = "probes",
    [R3D_PROFILER_GEOMETRY]           = "geometry",
    [R3D_PROFILER_DECALS]             = "decals",
    [R3D_PROFILER_LIGHTS]             = "lights",
    [R3D_PROFILER_PYRAMID]            = "depth pyramid",
    [R3D_PROFILER_SSAO]               = "SSAO",
    [R3D_PROFILER_SSIL]               = "SSIL",
    [R3D_PROFILER_SSGI]               = "SSGI",
    [R3D_PROFILER_SSR]                = "SSR",
    [R3D_PROFILER_AMBIENT]            = "ambient",
    [R3D_PROFILER_COMPOSE]            = "compose",
    [R3D_PROFILER_BACKGROUND]         = "background",
    [R3D_PROFILER_FOG]                = "fog",
    [R3D_PROFILER_VFOG_TRANSMITTANCE] = "vfog transmit",
    [R3D_PROFILER_VFOG_RADIANCE]      = "vfog radiance",
    [R3D_PROFILER_VFOG_COMPOSE]       = "fog + vfog compose",
    [R3D_PROFILER_FORWARD]            = "forward",
    [R3D_PROFILER_DOF]                = "DoF",
    [R3D_PROFILER_BLOOM]              = "bloom",
    [R3D_PROFILER_AUTO_EXPOSURE]      = "auto exposure",
    [R3D_PROFILER_OUTPUT]             = "tonemap",
    [R3D_PROFILER_AA]                 = "FXAA/SMAA",
    [R3D_PROFILER_BLIT]               = "blit",
};

const char* R3D_GetProfilerPassName(R3D_ProfilerPass pass)
{
    if ((int)pass < 0 || pass >= R3D_PROFILER_PASS_COUNT) return "?";
    return PROF_PASS_NAMES[pass];
}

#ifdef R3D_GPU_PROFILER

bool R3D_IsProfilerAvailable(void)
{
    return true;
}

void R3D_SetProfilerEnabled(bool enabled)
{
    if (enabled == PROF.enabled) return;
    if (!enabled && PROF.created) {
        for (int i = 0; i < PROF_RING; i++) {
            glDeleteQueries(R3D_PROFILER_PASS_COUNT, PROF.slots[i].pass);
            glDeleteQueries(1, &PROF.slots[i].tsBegin);
            glDeleteQueries(1, &PROF.slots[i].tsEnd);
        }
        memset(PROF.slots, 0, sizeof(PROF.slots));
        PROF.created = false;
        PROF.queueHead = PROF.queueCount = 0;
    }
    PROF.enabled = enabled;
}

bool R3D_IsProfilerEnabled(void)
{
    return PROF.enabled;
}

void R3D_SetProfilerTag(int tag)
{
    PROF.tag = tag;
}

bool R3D_PopProfilerFrame(R3D_ProfilerFrame* frame)
{
    if (PROF.enabled) prof_collect(false);
    if (PROF.queueCount == 0) return false;
    *frame = PROF.queue[PROF.queueHead];
    PROF.queueHead = (PROF.queueHead + 1) % PROF_QUEUE;
    PROF.queueCount--;
    return true;
}

void R3D_FlushProfiler(void)
{
    if (PROF.enabled && PROF.created) prof_collect(true);
}

int R3D_GetProfilerDroppedCount(void)
{
    return PROF.dropped;
}

#else

bool R3D_IsProfilerAvailable(void) { return false; }
void R3D_SetProfilerEnabled(bool enabled) { (void)enabled; }
bool R3D_IsProfilerEnabled(void) { return false; }
void R3D_SetProfilerTag(int tag) { (void)tag; }
bool R3D_PopProfilerFrame(R3D_ProfilerFrame* frame) { (void)frame; return false; }
void R3D_FlushProfiler(void) { }
int R3D_GetProfilerDroppedCount(void) { return 0; }

#endif // R3D_GPU_PROFILER
