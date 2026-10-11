/* r3d_draw.h -- R3D Draw Module.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#ifndef R3D_DRAW_H
#define R3D_DRAW_H

#include "./r3d_animation_player.h"
#include "./r3d_instance.h"
#include "./r3d_platform.h"
#include "./r3d_lighting.h"
#include "./r3d_camera.h"
#include "./r3d_model.h"
#include "./r3d_decal.h"
#include "./r3d_probe.h"
#include <raylib.h>

/**
 * @defgroup Draw
 * @{
 */

// ========================================
// STRUCT TYPES
// ========================================

/**
 * @brief Describes an R3D rendering view.
 *
 * A view defines the camera and output area used for a single rendering
 * session. It combines an R3D camera, an optional render target and an optional
 * viewport inside that target.
 *
 * If `target` is zero-initialized, rendering is directed to the default
 * framebuffer. If `viewport.width` or `viewport.height` is less than or equal
 * to zero, the full target size is used.
 */
typedef struct R3D_View {
    R3D_Camera camera;      ///< Camera used for this view.
    RenderTexture target;   ///< Render target. Zero-initialized means screen/backbuffer.
    Rectangle viewport;     ///< Viewport inside the target. If width or height <= 0, the full target is used.
} R3D_View;

// ========================================
// PUBLIC API
// ========================================

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Begins a rendering session using the given raylib camera.
 *
 * Rendering output is directed to the default framebuffer.
 *
 * The given Camera3D is converted internally to an R3D_Camera. Since raylib
 * cameras do not store near/far clipping planes, the converted camera uses the
 * current rlgl culling distances for those values.
 *
 * @param camera Camera used to render the scene.
 */
R3DAPI void R3D_Begin(Camera3D camera);

/**
 * @brief Begins a rendering session using an R3D camera.
 *
 * Rendering output is directed to the default framebuffer.
 *
 * This entry point provides access to R3D-specific camera features such as
 * layer masks, custom near/far clipping planes and quaternion-based orientation.
 *
 * @param camera Camera used to render the scene.
 */
R3DAPI void R3D_BeginEx(R3D_Camera camera);

/**
 * @brief Begins a rendering session using a complete R3D view descriptor.
 *
 * This is the advanced entry point. It allows the caller to specify the camera,
 * render target and viewport used for the rendering session.
 *
 * The view camera is used as-is, including its near/far clipping planes and
 * layer mask. If the camera was created from a raylib Camera3D using
 * `R3D_CameraFromRL()`, its near/far planes come from the current rlgl culling
 * distances because raylib cameras do not store those values directly.
 *
 * Use this function for render-to-texture workflows, custom viewports,
 * multipass rendering, editor views, minimaps, probes or any case where the
 * default framebuffer is not enough.
 *
 * @param view View descriptor used to render the scene.
 */
R3DAPI void R3D_BeginPro(R3D_View view);

/**
 * @brief Ends the current rendering session.
 *
 * This function is the one that actually performs the full
 * rendering of the described scene. It carries out culling,
 * sorting, shadow rendering, scene rendering, and screen /
 * post-processing effects.
 */
R3DAPI void R3D_End(void);

/**
 * @brief Begins a clustered draw pass.
 *
 * All draw calls submitted in this pass are first tested against the
 * cluster AABB. If the cluster fails the scene/shadow frustum test,
 * none of the contained objects are tested or drawn.
 *
 * @param aabb Bounding box used as the cluster-level frustum test.
 */
R3DAPI void R3D_BeginCluster(BoundingBox aabb);

/**
 * @brief Ends the current clustered draw pass.
 *
 * Stops submitting draw calls to the active cluster.
 */
R3DAPI void R3D_EndCluster(void);

/**
 * @brief Queues a light to be rendered for the current frame, without shadows.
 *
 * The light is submitted for rendering during R3D_End(), and must be pushed
 * again on every frame between R3D_Begin() and R3D_End() to remain visible.
 *
 * @param light The light data to submit (see R3D_Light).
 */
R3DAPI void R3D_PushLight(R3D_Light light);

/**
 * @brief Queues a light to be rendered for the current frame, with an associated shadow map.
 *
 * The light is submitted for rendering during R3D_End(), and must be pushed
 * again on every frame between R3D_Begin() and R3D_End() to remain visible.
 *
 * The shadow map is re-rendered whenever updateShadow is true, or when it has never
 * been rendered before (so static lights can safely pass false from the start and
 * still get valid shadows). This happens even if the light is currently off-screen,
 * so you can safely ignore visibility when driving updateShadow, or factor it in
 * yourself if you want to defer updates for off-screen lights.
 *
 * @param light The light data to submit (see R3D_Light).
 * @param map The shadow map to associate with this light for this frame.
 * @param updateShadow Whether the shadow map should be re-rendered this frame.
 *                     If false, the previously rendered content is reused instead.
 */
R3DAPI void R3D_PushLightEx(R3D_Light light, R3D_ShadowMap map, bool updateShadow);

/**
 * @brief Like R3D_PushLightEx() for a directional light, but the caller fits the shadow map.
 *
 * Local monolith patch (see patches.md). R3D normally fits a directional shadow map to
 * the camera frustum, so its coverage moves whenever the camera moves or turns. Here
 * the map is rendered with lightViewProj instead (an orthographic light-space matrix,
 * e.g. a world-fixed tile), and sampled with the matrix it was last rendered with, so
 * a static scene can render it once and only sample it afterwards.
 *
 * The map is rendered when updateShadow is true or when it has never been rendered;
 * otherwise lightViewProj is ignored. Shadows fade by distance from the eye over
 * 75..100% of light.range, as usual, and over the outer 1% of the map.
 * For other light types this is R3D_PushLightEx() (lightViewProj ignored).
 */
R3DAPI void R3D_PushLightFixedShadow(R3D_Light light, R3D_ShadowMap map, Matrix lightViewProj, bool updateShadow);

/**
 * @brief Bakes the sun visibility the volumetric fog samples into a 3D texture.
 *
 * Local monolith patch (see patches.md). For a static directional light over static
 * geometry, the fog march's shadow lookups are a fixed function of the world position.
 * Call this between R3D_Begin() and R3D_End(), after the frame's draws (they are the
 * shadow casters): during R3D_End() 'map' is rendered with lightViewProj (which must
 * cover 'region' in light space), then every voxel of 'region' stores the mean shadow
 * visibility over subsamples^3 points inside it (GL_R8, trilinear). From then on the
 * fog's march takes one fetch per step for directional lights inside the region and
 * falls back to the shadow map outside it. Bias and softness: the map's depthBias;
 * the PCF softness is replaced by the voxel box filter.
 *
 * The baked layer keeps lightViewProj as its matrix (re-render it before using it as a
 * normal shadow map again). The volume stays until the next bake or
 * R3D_ClearFogSunVisibility(); re-bake whenever the geometry or the light changes.
 * VRAM: one byte per voxel.
 */
R3DAPI void R3D_BakeFogSunVisibility(R3D_ShadowMap map, Matrix lightViewProj, BoundingBox region, float voxelSize, int subsamples);

/** @brief Drops the baked fog sun visibility: the fog samples the shadow map again everywhere. */
R3DAPI void R3D_ClearFogSunVisibility(void);

/** @brief GPU time of the last fog sun-visibility bake in ms (0 = none yet), measured with glFinish. */
R3DAPI float R3D_GetFogSunVisibilityBakeTime(void);

/**
 * @brief Sets a baked world-space occlusion volume for the ambient light.
 *
 * Local monolith patch 11 (see patches.md). 'distance' is a w x h x d grid (x fastest, then
 * y, then z) of one byte per voxel: the signed distance from the voxel centre to the nearest
 * surface (negative inside geometry), decoded as value / 255 * (maxDistance - minDistance) +
 * minDistance metres. Voxel (i, j, k) is centred at min + (i + 0.5, j + 0.5, k + 0.5) * voxelSize.
 * The ambient pass then scales the ambient occlusion of every pixel inside the volume by a
 * distance-field AO estimate (a few trilinear fetches along the normal, 0.15 to 3.2 m out),
 * times 'intensity' (1 = full); outside it, and near its border, the factor fades to 1.
 * It works with or without SSAO/SSIL (it multiplies with them). VRAM: one byte per voxel.
 * The volume stays until the next call or R3D_ClearWorldOcclusion().
 */
R3DAPI void R3D_SetWorldOcclusion(const unsigned char *distance, int w, int h, int d, Vector3 min, float voxelSize,
                                  float minDistance, float maxDistance, float intensity);

/** @brief Drops the world occlusion volume (and frees its texture). */
R3DAPI void R3D_ClearWorldOcclusion(void);

/**
 * @brief Queues a probe to be rendered for the current frame.
 *
 * The probe is submitted for rendering during R3D_End(), and must be pushed
 * again on every frame between R3D_Begin() and R3D_End() to remain active.
 *
 * The probe is re-captured whenever updateProbe is true, or when it has never been
 * captured before (so static probes can safely pass false from the start and still
 * get valid data). This happens even if the probe is currently off-screen, so you
 * can safely ignore visibility when driving updateProbe, or factor it in yourself
 * if you want to defer updates for off-screen probes.
 *
 * @param probe The probe data to submit (see R3D_Probe).
 * @param updateProbe Whether the probe's capture should be re-rendered this frame.
 *                    If false, the previously captured content is reused instead.
 */
R3DAPI void R3D_PushProbe(R3D_Probe probe, bool updateProbe);

/**
 * @brief Queues a mesh draw command with position and uniform scale.
 * 
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawMesh(R3D_Mesh mesh, R3D_Material material, Vector3 position, float scale);

/**
 * @brief Queues a mesh draw command with position, rotation and non-uniform scale.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawMeshEx(R3D_Mesh mesh, R3D_Material material, Vector3 position, Quaternion rotation, Vector3 scale);

/**
 * @brief Queues a mesh draw command using a full transform matrix.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawMeshPro(R3D_Mesh mesh, R3D_Material material, Matrix transform);

/**
 * @brief Queues an instanced mesh draw command.
 *
 * Draws multiple instances using the provided instance buffer.
 * Does nothing if the number of instances is <= 0.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawMeshInstanced(R3D_Mesh mesh, R3D_Material material, R3D_InstanceBuffer instances, int count);

/**
 * @brief Queues an instanced mesh draw command with an instance range.
 *
 * Draws 'count' instances starting at 'offset' in the instance buffer.
 * Both 'offset' and 'count' are clamped to stay within [0, instances.capacity]:
 *   - offset is clamped to [0, capacity]
 *   - count is clamped to [0, capacity - offset]
 * Does nothing if the resulting count is <= 0.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawMeshInstancedEx(R3D_Mesh mesh, R3D_Material material, R3D_InstanceBuffer instances, int offset, int count);

/**
 * @brief Queues an instanced mesh draw command with an instance range and an additional transform.
 *
 * Draws 'count' instances starting at 'offset' in the instance buffer.
 * Both 'offset' and 'count' are clamped to stay within [0, instances.capacity]:
 *   - offset is clamped to [0, capacity]
 *   - count is clamped to [0, capacity - offset]
 * Does nothing if the resulting count is <= 0.
 * The transform is applied to all instances.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawMeshInstancedPro(R3D_Mesh mesh, R3D_Material material, R3D_InstanceBuffer instances, int offset, int count, Matrix transform);

/**
 * @brief Queues a model draw command with position and uniform scale.
 * 
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawModel(R3D_Model model, Vector3 position, float scale);

/**
 * @brief Queues a model draw command with position, rotation and non-uniform scale.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawModelEx(R3D_Model model, Vector3 position, Quaternion rotation, Vector3 scale);

/**
 * @brief Queues a model draw command using a full transform matrix.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawModelPro(R3D_Model model, Matrix transform);

/**
 * @brief Queues an instanced model draw command.
 *
 * Draws multiple instances using the provided instance buffer.
 * Does nothing if the number of instances is <= 0.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawModelInstanced(R3D_Model model, R3D_InstanceBuffer instances, int count);

/**
 * @brief Queues an instanced model draw command with an instance range.
 *
 * Draws 'count' instances starting at 'offset' in the instance buffer.
 * Both 'offset' and 'count' are clamped to stay within [0, instances.capacity]:
 *   - offset is clamped to [0, capacity]
 *   - count is clamped to [0, capacity - offset]
 * Does nothing if the resulting count is <= 0.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawModelInstancedEx(R3D_Model model, R3D_InstanceBuffer instances, int offset, int count);

/**
 * @brief Queues an instanced model draw command with an instance range and an additional transform.
 *
 * Draws 'count' instances starting at 'offset' in the instance buffer.
 * Both 'offset' and 'count' are clamped to stay within [0, instances.capacity]:
 *   - offset is clamped to [0, capacity]
 *   - count is clamped to [0, capacity - offset]
 * Does nothing if the resulting count is <= 0.
 * The transform is applied to all instances.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawModelInstancedPro(R3D_Model model, R3D_InstanceBuffer instances, int offset, int count, Matrix transform);

/**
 * @brief Queues an animated model draw command.
 *
 * Uses the provided animation player to compute the pose.
 * 
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawAnimatedModel(R3D_Model model, R3D_AnimationPlayer player, Vector3 position, float scale);

/**
 * @brief Queues an animated model draw command with position, rotation and non-uniform scale.
 *
 * Uses the provided animation player to compute the pose.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawAnimatedModelEx(R3D_Model model, R3D_AnimationPlayer player, Vector3 position, Quaternion rotation, Vector3 scale);

/**
 * @brief Queues an animated model draw command using a full transform matrix.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawAnimatedModelPro(R3D_Model model, R3D_AnimationPlayer player, Matrix transform);

/**
 * @brief Queues an instanced animated model draw command.
 *
 * Draws multiple animated instances using the provided instance buffer.
 * Does nothing if the number of instances is <= 0.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawAnimatedModelInstanced(R3D_Model model, R3D_AnimationPlayer player, R3D_InstanceBuffer instances, int count);

/**
 * @brief Queues an instanced animated model draw command with an instance range.
 *
 * Draws 'count' animated instances starting at 'offset' in the instance buffer.
 * Both 'offset' and 'count' are clamped to stay within [0, instances.capacity]:
 *   - offset is clamped to [0, capacity]
 *   - count is clamped to [0, capacity - offset]
 * Does nothing if the resulting count is <= 0.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawAnimatedModelInstancedEx(R3D_Model model, R3D_AnimationPlayer player, R3D_InstanceBuffer instances, int offset, int count);

/**
 * @brief Queues an instanced animated model draw command with an instance range and an additional transform.
 *
 * Draws 'count' animated instances starting at 'offset' in the instance buffer.
 * Both 'offset' and 'count' are clamped to stay within [0, instances.capacity]:
 *   - offset is clamped to [0, capacity]
 *   - count is clamped to [0, capacity - offset]
 * Does nothing if the resulting count is <= 0.
 * The transform is applied to all instances.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawAnimatedModelInstancedPro(R3D_Model model, R3D_AnimationPlayer player, R3D_InstanceBuffer instances, int offset, int count, Matrix transform);

/**
 * @brief Queues a decal draw command with position and uniform scale.
 * 
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawDecal(R3D_Decal decal, Vector3 position, float scale);

/**
 * @brief Queues a decal draw command with position, rotation and non-uniform scale.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawDecalEx(R3D_Decal decal, Vector3 position, Quaternion rotation, Vector3 scale);

/**
 * @brief Queues a decal draw command using a full transform matrix.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawDecalPro(R3D_Decal decal, Matrix transform);

/**
 * @brief Queues an instanced decal draw command.
 *
 * Draws multiple instances using the provided instance buffer.
 * Does nothing if the number of instances is <= 0.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawDecalInstanced(R3D_Decal decal, R3D_InstanceBuffer instances, int count);

/**
 * @brief Queues an instanced decal draw command with an instance range.
 *
 * Draws 'count' instances starting at 'offset' in the instance buffer.
 * Both 'offset' and 'count' are clamped to stay within [0, instances.capacity]:
 *   - offset is clamped to [0, capacity]
 *   - count is clamped to [0, capacity - offset]
 * Does nothing if the resulting count is <= 0.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawDecalInstancedEx(R3D_Decal decal, R3D_InstanceBuffer instances, int offset, int count);

/**
 * @brief Queues an instanced decal draw command with an instance range and an additional transform.
 *
 * Draws 'count' instances starting at 'offset' in the instance buffer.
 * Both 'offset' and 'count' are clamped to stay within [0, instances.capacity]:
 *   - offset is clamped to [0, capacity]
 *   - count is clamped to [0, capacity - offset]
 * Does nothing if the resulting count is <= 0.
 * The transform is applied to all instances.
 *
 * The command is executed during R3D_End().
 */
R3DAPI void R3D_DrawDecalInstancedPro(R3D_Decal decal, R3D_InstanceBuffer instances, int offset, int count, Matrix transform);

#ifdef __cplusplus
} // extern "C"
#endif

/** @} */ // end of Draw

#endif // R3D_DRAW_H
