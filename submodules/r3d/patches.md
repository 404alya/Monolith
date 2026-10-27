# R3D: vendored copy and local patches

This directory is a **copy** of R3D, not a git submodule, so monolith can carry fixes
that upstream doesn't have yet. Every change made here relative to upstream is listed
below. Re-apply them (or check that upstream fixed them) whenever this copy is refreshed.

## Source

| | |
|---|---|
| Upstream | https://github.com/Bigfoot71/r3d |
| Commit | `48ade1127de768a08e7641f6972e67a254469e01` (2026-09-11, "Merge pull request #338 from Bigfoot71/camera-review") |
| `external/raylib` | `c1ab645ca298a2801097931d1079b10ff7eb9df8` (raylib 5.5) |
| `external/assimp` | `cf7b652190827878a4726709f0998ce170e55ce8` |
| Copied | 2026-10-08 from `~/Projects/r3d` (working tree) |

Left out of the copy: `.git/`, the submodules' `.git` pointer files, and the build
directories `build/` and `build-mingw/`. `build.sh` recreates the builds here:
`build-linux/` (Linux) and `build-mingw/` (Windows), both static libraries.

## Changes in the source tree before copying (not made for monolith)

`~/Projects/r3d` had uncommitted local changes, so they are in this copy as well.
They only touch the examples, which monolith doesn't build (`R3D_BUILD_EXAMPLES=OFF`):

- `examples/CMakeLists.txt`: 1 line added (registers the flashlight example)
- `examples/basic.c`: small edits (+9 / −3 lines)
- `examples/flashlight.c`: new file

## Patch 1: volumetric fog shaders fail to compile on AMD / Intel (2026-10-08)

**Symptom.** On Windows with an AMD Radeon RX 9060 XT (and, by the same rule, other non-NVIDIA GPUs), the `vfogTransmittance` and `vfogRadiance`
shaders fail to load:

```
ERROR: 0:27: 'length' : does not operate on this type: layout( column_major std140 offset=176) uniform structure{ ... global float length, ... }
```

**Cause.** The `FX_VFog` uniform block has a member named `length`. Strict GLSL front
ends (AMD and Intel drivers, glslang) parse `uVFog.length` as the array `.length()`
method and reject it. NVIDIA accepts it, so the bug never shows up there. The same
error reproduces on Linux with `glslangValidator`.

**Fix.** Rename the GLSL member and its C mirror (they are matched by std140 layout,
not by name) from `length` to `maxDistance`. R3D's public API is unchanged:
`R3D_Environment.volumetricFog.length` keeps its name.

| File | Change |
|---|---|
| `shaders/include/ubo/fx.glsl` | `float length;` → `float maxDistance;` in `struct FX_VFog` |
| `shaders/deferred/vfog_transmittance.frag` | `uVFog.length` → `uVFog.maxDistance` |
| `shaders/deferred/vfog_radiance.frag` | `uVFog.length` → `uVFog.maxDistance` (1 use, 1 comment) |
| `src/modules/r3d_shader.h` | `uVFog.length` → `uVFog.maxDistance` in `struct r3d_shader_block_fx_vfog` |
| `src/r3d_draw.c` | `block.uVFog.length = …` → `block.uVFog.maxDistance = …` |

**Verified.** All 60 embedded shaders pass `glslangValidator` after the fix (before it,
only these two failed). Rendering on NVIDIA is pixel-identical.

**Upstream.** Not fixed upstream as of the commit above. Worth reporting: it breaks
volumetric fog on every non-NVIDIA desktop driver.

```diff
--- a/shaders/include/ubo/fx.glsl
+++ b/shaders/include/ubo/fx.glsl
@@ -90,7 +90,7 @@
     float anisotropy;
     float emissionEnergy;
     float skyAffect;
-    float length;
+    float maxDistance;
     float stepSize;
     bool enabled;
 };
--- a/shaders/deferred/vfog_transmittance.frag
+++ b/shaders/deferred/vfog_transmittance.frag
@@ -41,7 +41,7 @@
 void main()
 {
     float depth = texelFetch(uDepthTex, ivec2(gl_FragCoord.xy), 0).r;
-    float rayDist = min(depth, uVFog.length);
+    float rayDist = min(depth, uVFog.maxDistance);
--- a/shaders/deferred/vfog_radiance.frag
+++ b/shaders/deferred/vfog_radiance.frag
@@ -65,7 +65,7 @@
 // Computes the march range [tEnter, tExit] along rayDir (from rayOrigin),
-// clamped to uVFog.length and to the light's range sphere when applicable.
+// clamped to uVFog.maxDistance and to the light's range sphere when applicable.
@@ -110,7 +110,7 @@
     float tEnter, tExit;
-    VFog_GetMarchRange(uView.position, rayDir, uVFog.length, tEnter, tExit);
+    VFog_GetMarchRange(uView.position, rayDir, uVFog.maxDistance, tEnter, tExit);
--- a/src/modules/r3d_shader.h
+++ b/src/modules/r3d_shader.h
@@ -542,7 +542,7 @@
         alignas(4) float emissionEnergy;
         alignas(4) float skyAffect;
-        alignas(4) float length;
+        alignas(4) float maxDistance;
         alignas(4) float stepSize;
--- a/src/r3d_draw.c
+++ b/src/r3d_draw.c
@@ -941,7 +941,7 @@
         block.uVFog.skyAffect         = env->volumetricFog.skyAffect;
-        block.uVFog.length            = env->volumetricFog.length;
+        block.uVFog.maxDistance       = env->volumetricFog.length;
         block.uVFog.stepSize          = env->volumetricFog.stepSize;
```

## Patch 2: volumetric fog does 8-tap shadow PCF it then discards (2026-10-10)

**Symptom.** Frame time grows with camera height: the higher the view, the more of the
screen is sky or geometry > 100 m away. Headless height sweep before the fix (fixed
x/z/yaw, pitch −20°): 5.84 ms at eye y 2 → 9.25 ms at y 60. With volumetric fog off, the
same sweep gets *faster* with height (4.6 → 3.5 ms).

**Cause.** `vfog_radiance.frag` calls `L_SampleShadowDir(uLight, marchPos, depth, …)` on
every march step (up to `length / stepSize` = 120 steps at half resolution), and each
call does the full 8-tap Vogel PCF. `L_SampleShadowDir` then blends the result with
`distFade = smoothstep(range, 0.75 * range, Zvs)`, where `Zvs` is the pixel's view
depth (`depth`, constant along the ray). At `depth >= range` (all sky pixels, which have
`depth = far`, and anything beyond the shadow distance) `distFade` is exactly 0, so
`mix(1.0, shadow, 0.0)` returns 1.0 and all 8 × n taps were thrown away. Those pixels
also march the full fog length, so they were the most expensive ones on screen.

**Fix.** For directional lights, clear `hasShadow` when `depth >= uLight.range`. The
result is bit-identical (the skipped value was weighted by exactly 0); only the wasted
lookups go away.

| File | Change |
|---|---|
| `shaders/deferred/vfog_radiance.frag` | skip the march's shadow lookups for a directional light when `depth >= uLight.range` (+5 lines) |

**Verified.** Passes `glslangValidator`. 25 `--shot` images (all viewpoints, yaw sweeps
at y 1.6 / 60 / 100) are pixel-identical before/after (0 pixels differ at all), Linux
and wine. Headless `--bench`, 3 alternating runs each: All 7.354 → 5.873 ms
(e.g. Tower 8.84 → 4.99 ms, Terrace 7.86 → 5.09 ms); the height sweep above becomes
5.12 ms at y 2 and 6.15 ms at y 60 (4.87 at y 100).

**Upstream.** Not fixed upstream as of the commit above. Note that the fog's shadow fade
uses the *pixel's* depth rather than the march position's, so fog in front of distant
geometry or sky is never shadowed. That's an upstream behaviour quirk, kept as is here.

```diff
--- a/shaders/deferred/vfog_radiance.frag
+++ b/shaders/deferred/vfog_radiance.frag
@@ -143,6 +143,11 @@ void main()
     mat2 diskRot = L_ShadowDebandingMatrix(gl_FragCoord.xy);
     bool hasShadow = uLight.shadowLayer >= 0 && uLight.shadowOpacity != 0.0;
 
+    // L_SampleShadowDir fades the shadow out by the pixel's view depth (constant along
+    // the ray) and weights it by exactly 0 at depth >= range, e.g. on sky pixels.
+    // Skip its taps there: same result, no wasted PCF lookups on every march step.
+    if (uLight.type == LIGHT_DIR && depth >= uLight.range) hasShadow = false;
+
     // Precompute inverses used inside the loop
     float invRange = 1.0 / max(uLight.range, 1e-4);
     float invEpsilon = 1.0 / max(uLight.innerCutOff - uLight.outerCutOff, 1e-4);
```

## Patch 3: fog shadow lookups skipped where the 8 PCF taps must agree (2026-10-10)

**Symptom.** After patch 2, steep downward views from height were still the most
expensive: `--pos -52.22 61.62 39.53 --look 140.2 -39.6` (eye y 63, pitch −40°) ran at
~7.1 ms vs. ~5 ms at ground level. With volumetric fog off it ran at ~4.5 ms.

**Cause.** That's real fog work, not waste. Looking down from 63 m, every pixel hits ground
80–100 m away, so every ray marches the full fog length (~120 steps at half resolution)
inside the shadow range. Each step does `L_SampleShadowDir`'s 8 hardware-PCF taps.
Most steps are far from any shadow edge, though, so all 8 taps return the same value
(fully lit or fully shadowed).

**Fix.** Exact shortcut via a min/max depth map:

- A new reduction pass (`pass_scene_shadow_minmax`, shader `shadow_minmax.frag`) builds
  the min/max depth of every 16×16-texel tile of a directional shadow layer
  (8192² → 512² `GL_RG32F`, via a 1024² intermediate). It runs only for layers rendered
  this frame (all layers after a reallocation, or after volumetric fog was off), and
  never while volumetric fog is off. Cost per re-render ~0.35 ms on an RTX 4050 Laptop. Reading raw
  depth needs compare mode off on the shadow array during the pass, so it's switched
  off and back on. Pass 0 uses `textureGather` (`GL_ARB_texture_gather`, `#ifdef`-guarded,
  with a `texelFetch` fallback): it is texel-fetch-rate bound, ~1.5 ms without gather.
- `vfog_radiance.frag` replaces `L_SampleShadowDir` with `VFog_SampleShadowDir`, the same
  code plus a test before the taps. It fetches the 2×2 tiles covering the step's PCF footprint
  (Vogel radius ≤ 1·softness texels + bilinear neighbour + margin). If
  `clamp(ref,0,1) <= min − 2/65535` every tap passes, so `shadow = 1`. If `ref > max + 2/65535`
  every tap fails, so `shadow = 0`. Otherwise, or when the footprint doesn't fit in 2×2 tiles
  (softness > ~5 texels), the original 8 taps run. The fades and the final `mix` are
  unchanged.

Only the fog uses the shortcut. The lighting pass still calls `L_SampleShadowDir`.

| File | Change |
|---|---|
| `shaders/prepare/shadow_minmax.frag` | new: 2-pass min/max reduction (`uPass` 0: shadow layer → 8×8, 1: 2×2 → 16×16 tiles) |
| `shaders/deferred/vfog_radiance.frag` | `uShadowDirMinMaxTex` sampler, `VFog_SampleShadowDir()`, used for `LIGHT_DIR` in the march |
| `CMakeLists.txt` | embed `shadow_minmax.frag` |
| `src/modules/r3d_light.h/.c` | min/max textures + FBO on the directional shadow array (allocated on expand, freed on destroy), accessors, `R3D_SHADOW_MINMAX_TILE` (16) |
| `src/modules/r3d_shader.h/.c` | `prepare.shadowMinMax` program + loader, sampler slot `R3D_SHADER_SAMPLER_SHADOW_DIR_MINMAX` (17, a free unit), `uShadowDirMinMaxTex` on `vfogRadiance` |
| `src/r3d_draw.c` | `pass_scene_shadow_minmax()` after `pass_scene_shadows()`; bind the min/max map for the fog pass |

**Pitfall hit while writing it.** `r3d_shader_bind_sampler()` resets the active texture
unit to 0, so the compare-mode restore after the loop must select the shadow map's unit
again, or the shadow map is left in raw-depth mode and every shadow lookup breaks.

**Verified.** All shaders pass `glslangValidator` (and the `texelFetch` fallback too).
On the GPU, the reduction output matched a CPU recomputation exactly (debug readback,
removed). Pixel-identical (0 pixels differ) to patch 2 alone on 32 still shots (all
viewpoints, yaw sweeps at y 1.6 / 60 / 100 and at the view above), 7 shots after 30
frames of turning and flying (shadow re-renders), and 5 shots with fog switched on
mid-flight; Linux and wine.

**Performance** (headless, 1920×1006, vs. patch 2 alone). The view above, still camera:
6.82 → 5.66 ms; spinning: 8.09 → 5.95 ms (3 runs each, noisy). Overview −0.98 ms. Clean
interleaved pairs (6 each, spinning): Canyon −0.07 ms, Hall −0.14 ms. Trade-off: the
cheapest views (eye y 100, mostly beyond the shadow range, so almost nothing to skip)
get +0.11 / +0.16 ms while spinning: the reduction runs on each shadow re-render
(~21% of bench frames) and the tile fetches cost a little where steps fall back.

**Upstream.** Not upstream. A simpler alternative that changes the image: one PCF tap per
step (−1.4 ms at the view above, but visible grain in the hall's light shafts).

```diff
diff --git a/CMakeLists.txt b/CMakeLists.txt
index 9b90da1..62c4e16 100644
--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -318,6 +318,7 @@ embed_shaders(${PROJECT_NAME}
     "${R3D_ROOT_PATH}/shaders/prepare/luminance.frag"
     "${R3D_ROOT_PATH}/shaders/prepare/exposure_adapt.frag"
     "${R3D_ROOT_PATH}/shaders/prepare/pyramid.frag"
+    "${R3D_ROOT_PATH}/shaders/prepare/shadow_minmax.frag"
     "${R3D_ROOT_PATH}/shaders/prepare/ssao.frag"
     "${R3D_ROOT_PATH}/shaders/prepare/ssil.frag"
     "${R3D_ROOT_PATH}/shaders/prepare/ssgi.frag"
diff --git a/src/modules/r3d_light.c b/src/modules/r3d_light.c
index e66d7e6..0606638 100644
--- a/src/modules/r3d_light.c
+++ b/src/modules/r3d_light.c
@@ -81,6 +81,9 @@ static void shadow_array_destroy(r3d_light_shadow_array_t* arr)
 {
     if (arr->texture != 0)     glDeleteTextures(1, &arr->texture);
     if (arr->framebuffer != 0) glDeleteFramebuffers(1, &arr->framebuffer);
+    if (arr->minMaxTexture != 0)     glDeleteTextures(1, &arr->minMaxTexture);
+    if (arr->minMaxFramebuffer != 0) glDeleteFramebuffers(1, &arr->minMaxFramebuffer);
+    if (arr->minMaxMidTexture != 0)  glDeleteTextures(1, &arr->minMaxMidTexture);
 
     R3D_LIST_DESTROY(arr->cache);
     R3D_LIST_DESTROY(arr->freeList);
@@ -123,6 +126,34 @@ static bool shadow_array_expand(r3d_light_shadow_array_t* arr, uint32_t growth)
     }
     arr->texture = newTexture;
 
+    // Min/max map (local patch): reallocate with the new layer count; its content is
+    // rebuilt from the copied shadow layers by the next frame's reduction pass
+    if (arr->hasMinMax)
+    {
+        int mmSize = (arr->size + R3D_SHADOW_MINMAX_TILE - 1) / R3D_SHADOW_MINMAX_TILE;
+        if (arr->minMaxTexture == 0) glGenTextures(1, &arr->minMaxTexture);
+        if (arr->minMaxFramebuffer == 0) glGenFramebuffers(1, &arr->minMaxFramebuffer);
+        glBindTexture(GL_TEXTURE_2D_ARRAY, arr->minMaxTexture);
+        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RG32F, mmSize, mmSize, (GLsizei)newLayerCount, 0, GL_RG, GL_FLOAT, NULL);
+        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
+        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
+        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
+        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
+        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
+        arr->minMaxStale = true;
+
+        if (arr->minMaxMidTexture == 0)
+        {
+            int midSize = (arr->size + 7) / 8;
+            glGenTextures(1, &arr->minMaxMidTexture);
+            glBindTexture(GL_TEXTURE_2D, arr->minMaxMidTexture);
+            glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, midSize, midSize, 0, GL_RG, GL_FLOAT, NULL);
+            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
+            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
+            glBindTexture(GL_TEXTURE_2D, 0);
+        }
+    }
+
     // Resize the cache (new entries zero-initialized => valid = false)
     R3D_LIST_RESIZE(arr->cache, newLayerCount);
 
@@ -608,6 +639,7 @@ bool r3d_light_init(void)
     memset(&R3D_MOD_LIGHT, 0, sizeof(R3D_MOD_LIGHT));
 
     R3D_MOD_LIGHT.shadowArrays[R3D_LIGHT_DIR]  = shadow_array_create(GL_TEXTURE_2D_ARRAY, R3D_HINT(R3D_HINT_SHADOW_DIR_SIZE), SHADOW_DIR_LAYER_GROWTH);
+    R3D_MOD_LIGHT.shadowArrays[R3D_LIGHT_DIR].hasMinMax = true;
     R3D_MOD_LIGHT.shadowArrays[R3D_LIGHT_SPOT] = shadow_array_create(GL_TEXTURE_2D_ARRAY, R3D_HINT(R3D_HINT_SHADOW_SPOT_SIZE), SHADOW_SPOT_LAYER_GROWTH);
     R3D_MOD_LIGHT.shadowArrays[R3D_LIGHT_OMNI] = shadow_array_create(GL_TEXTURE_CUBE_MAP_ARRAY, R3D_HINT(R3D_HINT_SHADOW_OMNI_SIZE), SHADOW_OMNI_LAYER_GROWTH);
 
@@ -735,6 +767,55 @@ GLuint r3d_light_shadow_map(R3D_LightType type)
     return R3D_MOD_LIGHT.shadowArrays[type].texture;
 }
 
+GLuint r3d_light_shadow_minmax_map(void)
+{
+    return R3D_MOD_LIGHT.shadowArrays[R3D_LIGHT_DIR].minMaxTexture;
+}
+
+int r3d_light_shadow_minmax_size(void)
+{
+    int size = R3D_MOD_LIGHT.shadowArrays[R3D_LIGHT_DIR].size;
+    return (size + R3D_SHADOW_MINMAX_TILE - 1) / R3D_SHADOW_MINMAX_TILE;
+}
+
+uint32_t r3d_light_shadow_layer_count(R3D_LightType type)
+{
+    return R3D_MOD_LIGHT.shadowArrays[type].layerCount;
+}
+
+void r3d_light_bind_shadow_minmax_fbo(int layer)
+{
+    r3d_light_shadow_array_t* arr = &R3D_MOD_LIGHT.shadowArrays[R3D_LIGHT_DIR];
+    int size = r3d_light_shadow_minmax_size();
+
+    glBindFramebuffer(GL_FRAMEBUFFER, arr->minMaxFramebuffer);
+    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, arr->minMaxTexture, 0, layer);
+    glViewport(0, 0, size, size);
+}
+
+void r3d_light_bind_shadow_minmax_mid_fbo(void)
+{
+    r3d_light_shadow_array_t* arr = &R3D_MOD_LIGHT.shadowArrays[R3D_LIGHT_DIR];
+    int size = (arr->size + 7) / 8;
+
+    glBindFramebuffer(GL_FRAMEBUFFER, arr->minMaxFramebuffer);
+    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, arr->minMaxMidTexture, 0);
+    glViewport(0, 0, size, size);
+}
+
+GLuint r3d_light_shadow_minmax_mid_map(void)
+{
+    return R3D_MOD_LIGHT.shadowArrays[R3D_LIGHT_DIR].minMaxMidTexture;
+}
+
+bool r3d_light_shadow_minmax_take_stale(void)
+{
+    r3d_light_shadow_array_t* arr = &R3D_MOD_LIGHT.shadowArrays[R3D_LIGHT_DIR];
+    bool stale = arr->minMaxStale;
+    arr->minMaxStale = false;
+    return stale;
+}
+
 r3d_light_shadow_cache_t* r3d_light_shadow_cache(R3D_LightType type, int layer)
 {
     R3D_ASSERT(layer >= 0);
diff --git a/src/modules/r3d_light.h b/src/modules/r3d_light.h
index f565e2b..a2208b6 100644
--- a/src/modules/r3d_light.h
+++ b/src/modules/r3d_light.h
@@ -88,6 +88,13 @@ typedef struct {
     uint32_t    layerCount;     // total number of allocated layers (GL side)
     int         size;           // shadow map resolution
     int         growth;         // number of layers added per expand
+    // Min/max depth per R3D_SHADOW_MINMAX_TILE^2 texels, one layer per shadow layer.
+    // Only for directional lights (local patch, see patches.md).
+    bool        hasMinMax;
+    bool        minMaxStale;    // reallocated: every layer must be reduced again
+    GLuint      minMaxFramebuffer;
+    GLuint      minMaxTexture;  // GL_TEXTURE_2D_ARRAY, GL_RG32F
+    GLuint      minMaxMidTexture;   // GL_TEXTURE_2D, GL_RG32F: 8x8 texel step of the reduction
 } r3d_light_shadow_array_t;
 
 // ========================================
@@ -143,6 +150,19 @@ int r3d_light_shadow_map_size(R3D_LightType type);
 /* Get a shadow map array texture ID */
 GLuint r3d_light_shadow_map(R3D_LightType type);
 
+/*
+ * Min/max depth map of the directional shadow layers (local patch, see patches.md).
+ * Tiles are R3D_SHADOW_MINMAX_TILE texels wide (must match shadow_minmax.frag).
+ */
+#define R3D_SHADOW_MINMAX_TILE 16
+GLuint r3d_light_shadow_minmax_map(void);
+int r3d_light_shadow_minmax_size(void);
+uint32_t r3d_light_shadow_layer_count(R3D_LightType type);
+void r3d_light_bind_shadow_minmax_fbo(int layer);
+void r3d_light_bind_shadow_minmax_mid_fbo(void);
+GLuint r3d_light_shadow_minmax_mid_map(void);
+bool r3d_light_shadow_minmax_take_stale(void);
+
 /**/
 r3d_light_shadow_cache_t* r3d_light_shadow_cache(R3D_LightType type, int layer);
 
diff --git a/src/modules/r3d_shader.c b/src/modules/r3d_shader.c
index b84db52..ad65ea0 100644
--- a/src/modules/r3d_shader.c
+++ b/src/modules/r3d_shader.c
@@ -27,6 +27,7 @@
 #include <shaders/blur_down.frag.h>
 #include <shaders/blur_up.frag.h>
 #include <shaders/pyramid.frag.h>
+#include <shaders/shadow_minmax.frag.h>
 #include <shaders/ssao.frag.h>
 #include <shaders/ssil.frag.h>
 #include <shaders/ssgi.frag.h>
@@ -391,6 +392,23 @@ bool r3d_shader_load_prepare_pyramid(r3d_shader_custom_t* custom)
     return true;
 }
 
+bool r3d_shader_load_prepare_shadow_minmax(r3d_shader_custom_t* custom)
+{
+    R3D_UNUSED(custom);
+
+    DECL_SHADER(r3d_shader_prepare_shadow_minmax_t, prepare, shadowMinMax);
+    LOAD_SHADER(shadowMinMax, SCREEN_VERT, SHADOW_MINMAX_FRAG);
+
+    GET_LOCATION(shadowMinMax, uLayer);
+    GET_LOCATION(shadowMinMax, uPass);
+
+    USE_SHADER(shadowMinMax);
+    SET_SAMPLER(shadowMinMax, uShadowTex, R3D_SHADER_SAMPLER_SHADOW_DIR);
+    SET_SAMPLER(shadowMinMax, uSourceTex, R3D_SHADER_SAMPLER_SOURCE_2D_0);
+
+    return true;
+}
+
 bool r3d_shader_load_prepare_downsample(r3d_shader_custom_t* custom)
 {
     R3D_UNUSED(custom);
@@ -1487,6 +1505,7 @@ bool r3d_shader_load_deferred_vfog_radiance(r3d_shader_custom_t* custom)
     SET_SAMPLER(vfogRadiance, uShadowDirTex, R3D_SHADER_SAMPLER_SHADOW_DIR);
     SET_SAMPLER(vfogRadiance, uShadowSpotTex, R3D_SHADER_SAMPLER_SHADOW_SPOT);
     SET_SAMPLER(vfogRadiance, uShadowOmniTex, R3D_SHADER_SAMPLER_SHADOW_OMNI);
+    SET_SAMPLER(vfogRadiance, uShadowDirMinMaxTex, R3D_SHADER_SAMPLER_SHADOW_DIR_MINMAX);
 
     return true;
 }
@@ -1852,6 +1871,7 @@ void r3d_shader_quit(void)
     UNLOAD_SHADER(prepare.blurDown);
     UNLOAD_SHADER(prepare.blurUp);
     UNLOAD_SHADER(prepare.pyramid);
+    UNLOAD_SHADER(prepare.shadowMinMax);
     UNLOAD_SHADER(prepare.downsample);
     UNLOAD_SHADER(prepare.ssao);
     UNLOAD_SHADER(prepare.ssil);
diff --git a/src/modules/r3d_shader.h b/src/modules/r3d_shader.h
index 5409db6..f25e95e 100644
--- a/src/modules/r3d_shader.h
+++ b/src/modules/r3d_shader.h
@@ -296,6 +296,9 @@ typedef enum {
     // Scene miscs
     R3D_SHADER_SAMPLER_BONE_MATRICES        = 16,
 
+    // Min/max depth tiles of the directional shadow map (local patch, see patches.md)
+    R3D_SHADER_SAMPLER_SHADOW_DIR_MINMAX    = 17,
+
     // Buffers
     R3D_SHADER_SAMPLER_BUFFER_SCENE         = 20,
     R3D_SHADER_SAMPLER_BUFFER_ALBEDO        = 21,
@@ -354,6 +357,7 @@ static const GLenum R3D_MOD_SHADER_SAMPLER_TYPES[R3D_SHADER_SAMPLER_COUNT] =
     [R3D_SHADER_SAMPLER_IBL_PREFILTER]          = GL_TEXTURE_CUBE_MAP_ARRAY,
     [R3D_SHADER_SAMPLER_IBL_BRDF_LUT]           = GL_TEXTURE_2D,
     [R3D_SHADER_SAMPLER_BONE_MATRICES]          = GL_TEXTURE_1D,
+    [R3D_SHADER_SAMPLER_SHADOW_DIR_MINMAX]      = GL_TEXTURE_2D_ARRAY,
     [R3D_SHADER_SAMPLER_BUFFER_SCENE]           = GL_TEXTURE_2D,
     [R3D_SHADER_SAMPLER_BUFFER_ALBEDO]          = GL_TEXTURE_2D,
     [R3D_SHADER_SAMPLER_BUFFER_NORMAL]          = GL_TEXTURE_2D,
@@ -667,6 +671,14 @@ typedef struct {
     r3d_shader_uniform_sampler_t uDepthTex;
 } r3d_shader_prepare_pyramid_t;
 
+typedef struct {
+    GLuint id;
+    r3d_shader_uniform_sampler_t uShadowTex;
+    r3d_shader_uniform_sampler_t uSourceTex;
+    r3d_shader_uniform_int_t uLayer;
+    r3d_shader_uniform_int_t uPass;
+} r3d_shader_prepare_shadow_minmax_t;
+
 typedef struct {
     GLuint id;
     r3d_shader_uniform_sampler_t uSelectorTex;
@@ -1066,6 +1078,7 @@ typedef struct {
     r3d_shader_uniform_sampler_t uShadowDirTex;
     r3d_shader_uniform_sampler_t uShadowSpotTex;
     r3d_shader_uniform_sampler_t uShadowOmniTex;
+    r3d_shader_uniform_sampler_t uShadowDirMinMaxTex;
 } r3d_shader_deferred_vfog_radiance_t;
 
 typedef struct {
@@ -1229,6 +1242,7 @@ extern struct r3d_mod_shader {
         r3d_shader_prepare_blur_down_t blurDown;
         r3d_shader_prepare_blur_up_t blurUp;
         r3d_shader_prepare_pyramid_t pyramid;
+        r3d_shader_prepare_shadow_minmax_t shadowMinMax;
         r3d_shader_prepare_downsample_t downsample;
         r3d_shader_prepare_ssao_t ssao;
         r3d_shader_prepare_ssil_t ssil;
@@ -1309,6 +1323,7 @@ bool r3d_shader_load_prepare_denoiser_sparse(r3d_shader_custom_t* custom);
 bool r3d_shader_load_prepare_blur_down(r3d_shader_custom_t* custom);
 bool r3d_shader_load_prepare_blur_up(r3d_shader_custom_t* custom);
 bool r3d_shader_load_prepare_pyramid(r3d_shader_custom_t* custom);
+bool r3d_shader_load_prepare_shadow_minmax(r3d_shader_custom_t* custom);
 bool r3d_shader_load_prepare_downsample(r3d_shader_custom_t* custom);
 bool r3d_shader_load_prepare_ssao(r3d_shader_custom_t* custom);
 bool r3d_shader_load_prepare_ssil(r3d_shader_custom_t* custom);
@@ -1382,6 +1397,7 @@ static const struct r3d_shader_loader {
         r3d_shader_loader_func blurDown;
         r3d_shader_loader_func blurUp;
         r3d_shader_loader_func pyramid;
+        r3d_shader_loader_func shadowMinMax;
         r3d_shader_loader_func downsample;
         r3d_shader_loader_func ssao;
         r3d_shader_loader_func ssaoBlur;
@@ -1460,6 +1476,7 @@ static const struct r3d_shader_loader {
         .blurDown = r3d_shader_load_prepare_blur_down,
         .blurUp = r3d_shader_load_prepare_blur_up,
         .pyramid = r3d_shader_load_prepare_pyramid,
+        .shadowMinMax = r3d_shader_load_prepare_shadow_minmax,
         .downsample = r3d_shader_load_prepare_downsample,
         .ssao = r3d_shader_load_prepare_ssao,
         .ssil = r3d_shader_load_prepare_ssil,
diff --git a/src/r3d_draw.c b/src/r3d_draw.c
index fe8c25f..4ace8b1 100644
--- a/src/r3d_draw.c
+++ b/src/r3d_draw.c
@@ -73,6 +73,7 @@ static void raster_forward(const r3d_render_call_t* call);
 static void raster_unlit(const r3d_render_call_t* call, bool opaque);
 
 static void pass_scene_shadows(void);
+static void pass_scene_shadow_minmax(void);
 static void pass_scene_probes(void);
 static void pass_scene_geometry(void);
 static void pass_scene_decals(void);
@@ -160,6 +161,8 @@ void R3D_End(void)
         pass_scene_shadows();
     }
 
+    pass_scene_shadow_minmax();
+
     r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_SHADOW_DIR, r3d_light_shadow_map(R3D_LIGHT_DIR));
     r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_SHADOW_SPOT, r3d_light_shadow_map(R3D_LIGHT_SPOT));
     r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_SHADOW_OMNI, r3d_light_shadow_map(R3D_LIGHT_OMNI));
@@ -1646,6 +1649,69 @@ void pass_scene_shadows(void)
     #undef COND
 }
 
+// Local patch (see patches.md): rebuilds the min/max depth tiles of the directional
+// shadow layers rendered this frame (or all of them after a reallocation). The
+// volumetric fog uses them to skip the PCF taps of fully lit / fully shadowed steps.
+void pass_scene_shadow_minmax(void)
+{
+    // Only the volumetric fog reads the tiles: while it's off, just remember that
+    // every layer has to be rebuilt once it comes back on
+    static bool pendingAll = false;
+    bool all = r3d_light_shadow_minmax_take_stale() || pendingAll;
+    uint32_t layerCount = r3d_light_shadow_layer_count(R3D_LIGHT_DIR);
+    if (r3d_light_shadow_minmax_map() == 0 || layerCount == 0) return;
+
+    bool any = all;
+    R3D_LIGHT_FOR_EACH_SHADOW_JOB(job) {
+        if (job->type == R3D_LIGHT_DIR) any = true;
+    }
+    if (!any) return;
+
+    pendingAll = !R3D.environment.volumetricFog.enabled;
+    if (pendingAll) return;
+
+    r3d_driver_disable(GL_STENCIL_TEST);
+    r3d_driver_disable(GL_DEPTH_TEST);
+    r3d_driver_disable(GL_CULL_FACE);
+    r3d_driver_disable(GL_BLEND);
+    r3d_driver_disable(GL_SCISSOR_TEST);
+
+    R3D_SHADER_USE(prepare.shadowMinMax);
+
+    // Read raw depth: the shadow array has compare mode on, which a plain sampler2DArray
+    // must not see (undefined results), so switch it off for this pass only
+    r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_SHADOW_DIR, r3d_light_shadow_map(R3D_LIGHT_DIR));
+    glActiveTexture(GL_TEXTURE0 + R3D_SHADER_SAMPLER_SHADOW_DIR);
+    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_NONE);
+
+    for (int layer = 0; layer < (int)layerCount; layer++)
+    {
+        bool rendered = all;
+        R3D_LIGHT_FOR_EACH_SHADOW_JOB(job) {
+            if (job->type == R3D_LIGHT_DIR && job->shadowLayer == layer) rendered = true;
+        }
+        if (!rendered) continue;
+
+        R3D_SHADER_BIND_SAMPLER(prepare.shadowMinMax, uSourceTex, 0); //< No feedback loop with the mid target
+        r3d_light_bind_shadow_minmax_mid_fbo();
+        R3D_SHADER_SET_INT(prepare.shadowMinMax, uPass, 0);
+        R3D_SHADER_SET_INT(prepare.shadowMinMax, uLayer, layer);
+        R3D_RENDER_SCREEN();
+
+        r3d_light_bind_shadow_minmax_fbo(layer);
+        R3D_SHADER_SET_INT(prepare.shadowMinMax, uPass, 1);
+        R3D_SHADER_BIND_SAMPLER(prepare.shadowMinMax, uSourceTex, r3d_light_shadow_minmax_mid_map());
+        R3D_RENDER_SCREEN();
+    }
+
+    // r3d_shader_bind_sampler() resets the active unit, so select the shadow map again
+    glActiveTexture(GL_TEXTURE0 + R3D_SHADER_SAMPLER_SHADOW_DIR);
+    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
+    glActiveTexture(GL_TEXTURE0);
+
+    r3d_target_invalidate_cache(); //< We bound our own framebuffer
+}
+
 void pass_scene_probes(void)
 {
     #define RASTER_PROBE(opaque)                                    \
@@ -2270,6 +2336,7 @@ void pass_deferred_volumetric_fog(r3d_target_t sceneTarget)
 
     R3D_SHADER_USE(deferred.vfogRadiance);
     R3D_SHADER_BIND_SAMPLER(deferred.vfogRadiance, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 1));
+    R3D_SHADER_BIND_SAMPLER(deferred.vfogRadiance, uShadowDirMinMaxTex, r3d_light_shadow_minmax_map());
 
     R3D_LIGHT_FOR_EACH_VISIBLE(light)
     {
--- a/shaders/deferred/vfog_radiance.frag
+++ b/shaders/deferred/vfog_radiance.frag
@@ -44,6 +44,8 @@
 uniform sampler2DArrayShadow uShadowSpotTex;
 uniform samplerCubeArrayShadow uShadowOmniTex;
 
+uniform sampler2DArray uShadowDirMinMaxTex;     // RG = min/max depth per 16x16 texels (local patch)
+
 // ================================
 // Helper Includes
 // ================================
@@ -64,6 +66,63 @@
     return nom / denom;
 }
 
+// L_SampleShadowDir (NoL = 1) with an exact shortcut (local patch, see patches.md):
+// when the reference depth is in front of (or behind) every texel the 8 PCF taps can
+// touch, each tap returns exactly 1 (or 0), so the taps are skipped. The texels come
+// from the min/max depth tiles of uShadowDirMinMaxTex, 2x2 tiles cover the footprint.
+float VFog_SampleShadowDir(vec3 Pws, float Zvs, mat2 diskRot)
+{
+    vec4 Pls = uLight.viewProj * vec4(Pws, 1.0);
+    vec3 projCoords = Pls.xyz / Pls.w * 0.5 + 0.5;
+    float bias = uLight.shadowDepthBias + uLight.shadowSlopeBias * (1.0 - 1.0);
+    float compareDepth = projCoords.z - bias;
+
+    // Footprint in texels: largest Vogel offset (< 1.0 * softness) plus the bilinear
+    // neighbour, with margin. Clamped like CLAMP_TO_EDGE clamps the taps.
+    const int TILE_SIZE = 16;   // must match R3D_SHADOW_MINMAX_TILE
+    float mapSize = float(textureSize(uShadowDirTex, 0).x);
+    float radius = uLight.shadowSoftness * mapSize + 2.0;
+    vec2 center = projCoords.xy * mapSize - 0.5;
+    ivec2 last = ivec2(textureSize(uShadowDirTex, 0).xy) - 1;
+    ivec2 t0 = clamp(ivec2(floor(center - radius)), ivec2(0), last) / TILE_SIZE;
+    ivec2 t1 = clamp(ivec2(floor(center + radius)) + 1, ivec2(0), last) / TILE_SIZE;
+
+    vec2 a = texelFetch(uShadowDirMinMaxTex, ivec3(t0.x, t0.y, uLight.shadowLayer), 0).rg;
+    vec2 b = texelFetch(uShadowDirMinMaxTex, ivec3(t1.x, t0.y, uLight.shadowLayer), 0).rg;
+    vec2 c = texelFetch(uShadowDirMinMaxTex, ivec3(t0.x, t1.y, uLight.shadowLayer), 0).rg;
+    vec2 d = texelFetch(uShadowDirMinMaxTex, ivec3(t1.x, t1.y, uLight.shadowLayer), 0).rg;
+    float dMin = min(min(a.x, b.x), min(c.x, d.x));
+    float dMax = max(max(a.y, b.y), max(c.y, d.y));
+
+    // The hardware compares clamp(ref, 0, 1) <= stored; keep 2 units of the 16-bit
+    // depth format as margin so rounding of the reference can't flip a tap
+    const float EPS = 2.0 / 65535.0;
+    float ref = clamp(compareDepth, 0.0, 1.0);
+
+    // 2x2 tiles only cover footprints narrower than a tile (softness up to ~5 texels)
+    bool covered = all(lessThanEqual(t1 - t0, ivec2(1)));
+
+    float shadow;
+    if (covered && ref <= dMin - EPS) shadow = 1.0;
+    else if (covered && ref > dMax + EPS) shadow = 0.0;
+    else
+    {
+        shadow = 0.0;
+        for (int i = 0; i < SHADOW_SAMPLES; ++i)
+        {
+            vec2 offset = diskRot * VOGEL_DISK[i] * uLight.shadowSoftness;
+            shadow += texture(uShadowDirTex, vec4(projCoords.xy + offset, uLight.shadowLayer, compareDepth));
+        }
+        shadow /= float(SHADOW_SAMPLES);
+    }
+
+    vec3 distToBorder = min(projCoords, 1.0 - projCoords);
+    float edgeFade = smoothstep(0.0, 0.05, min(distToBorder.x, min(distToBorder.y, distToBorder.z)));
+    float distFade = smoothstep(uLight.range, uLight.range * 0.75, Zvs);
+
+    return mix(1.0, shadow, edgeFade * distFade * uLight.shadowOpacity);
+}
+
 // Computes the march range [tEnter, tExit] along rayDir (from rayOrigin),
 // clamped to uVFog.maxDistance and to the light's range sphere when applicable.
 // tEnter/tExit are both 0.0 when the ray never enters the light's range
@@ -166,7 +225,7 @@
         {
             switch (uLight.type)
             {
-            case LIGHT_DIR:  visibility *= L_SampleShadowDir(uLight, marchPos, depth, 1.0, diskRot); break;
+            case LIGHT_DIR:  visibility *= VFog_SampleShadowDir(marchPos, depth, diskRot); break;
             case LIGHT_SPOT: visibility *= L_SampleShadowSpot(uLight, marchPos, 1.0, diskRot); break;
             case LIGHT_OMNI: visibility *= L_SampleShadowOmni(uLight, marchPos, 1.0, diskRot); break;
             }
--- /dev/null
+++ b/shaders/prepare/shadow_minmax.frag
@@ -0,0 +1,74 @@
+/* shadow_minmax.frag -- Min/max depth per tile of a directional shadow map layer
+ *
+ * Local monolith patch (see patches.md). Used by vfog_radiance.frag to skip the
+ * PCF taps of march steps that are fully lit or fully shadowed.
+ * Two passes (one thread per 256 texels is too serial to keep the GPU busy):
+ *   uPass 0: shadow layer -> min/max of 8x8 texels   (uShadowTex -> RG32F, 1/8 size)
+ *   uPass 1: 2x2 of those -> 16x16 tiles            (uSourceTex -> RG32F, 1/16 size)
+ */
+
+#version 330 core
+
+// Pass 0 is bound by the texel fetch rate: gathering 2x2 texels per fetch makes it
+// ~4x faster (8192^2 map: ~0.35 ms instead of ~1.5 ms). Same result either way.
+#extension GL_ARB_texture_gather : enable
+
+uniform sampler2DArray uShadowTex;  // depth values: compare mode is off during this pass
+uniform sampler2D uSourceTex;
+uniform int uLayer;
+uniform int uPass;
+
+out vec2 FragMinMax;
+
+void main()
+{
+    float dMin = 1.0;
+    float dMax = 0.0;
+
+    if (uPass == 0)
+    {
+#ifdef GL_ARB_texture_gather
+        // A sample exactly on the corner between 4 texels gathers those 4 texels
+        vec2 base = floor(gl_FragCoord.xy) * 8.0;
+        vec2 invSize = 1.0 / vec2(textureSize(uShadowTex, 0).xy);
+        for (int y = 0; y < 4; y++)
+        {
+            for (int x = 0; x < 4; x++)
+            {
+                vec2 uv = (base + vec2(2 * x + 1, 2 * y + 1)) * invSize;
+                vec4 d = textureGather(uShadowTex, vec3(uv, float(uLayer)));
+                dMin = min(dMin, min(min(d.x, d.y), min(d.z, d.w)));
+                dMax = max(dMax, max(max(d.x, d.y), max(d.z, d.w)));
+            }
+        }
+#else
+        ivec2 base = ivec2(gl_FragCoord.xy) * 8;
+        ivec2 last = textureSize(uShadowTex, 0).xy - 1;
+        for (int y = 0; y < 8; y++)
+        {
+            for (int x = 0; x < 8; x++)
+            {
+                float d = texelFetch(uShadowTex, ivec3(min(base + ivec2(x, y), last), uLayer), 0).r;
+                dMin = min(dMin, d);
+                dMax = max(dMax, d);
+            }
+        }
+#endif
+    }
+    else
+    {
+        ivec2 base = ivec2(gl_FragCoord.xy) * 2;
+        ivec2 last = textureSize(uSourceTex, 0) - 1;
+        for (int y = 0; y < 2; y++)
+        {
+            for (int x = 0; x < 2; x++)
+            {
+                vec2 mm = texelFetch(uSourceTex, min(base + ivec2(x, y), last), 0).rg;
+                dMin = min(dMin, mm.x);
+                dMax = max(dMax, mm.y);
+            }
+        }
+    }
+
+    FragMinMax = vec2(dMin, dMax);
+}
```

## Patch 4: fog and sun-shadow fades change when the camera only turns (2026-10-10)

**Symptom.** From a fixed eye, merely rotating the camera changes how a given world spot
looks, most visibly the fog: in the hypostyle hall the far opening glows with sunlit fog
while it is in the middle of the screen and goes dim near the screen edge; on the terrace
and in steep downward views the distance fog and the fog's density thin out toward the
screen edges and move with the view.

**Cause.** Four terms used the pixel's **view depth** (distance along the camera's
forward axis) where they mean distance from the eye. For a fixed world point, view depth
= distance · cos(angle off the view axis), so it changes on a pure rotation (at the 16:9
screen corner it is only 0.57 · distance).

| Term | Where (before the patch) | Effect |
|---|---|---|
| Volumetric fog shadow fade | `vfog_radiance.frag`: `VFog_SampleShadowDir(marchPos, depth, …)` → `distFade = smoothstep(range, 0.75·range, depth)`, and patch 2's skip `depth >= range` | Rays whose surface is ≥ `range` *deep* march fully unshadowed fog (the hall's glowing opening); near the edge the same ray is shadowed |
| Volumetric fog transmittance / emission | `vfog_transmittance.frag`: `rayDist = min(depth, maxDistance)` | Fog thinner toward the edges; also inconsistent with the radiance march, which already integrates over distance |
| Distance fog | `fog.frag`: `FogColorAlpha(depth, …)` | EXP2 fog thinner toward the edges |
| Sun shadow fade (surfaces) | `lighting.frag`: `L_SampleShadowDir(uLight, P, depth, …)` | Shadows on surfaces 75–174 m away appear/disappear with the view direction |

The camera-fitted map itself (`light_dir_view_proj`, centre 0.5 · `range` ahead of the
camera) moves with the view direction too, and `edgeFade` fades its outer 5 % (x, y and
depth). With the fades above made distance-based it no longer matters: every on-screen
point within `range` of the eye stays ≥ 0.19 (16:9, 1920×1006 and 21:9; ≥ 0.12 at 1:1) of
the map size from every border (Monte-Carlo over all view directions, sun direction of
monolith), far from the 0.05 fade band, so `edgeFade` is exactly 1 wherever the shadow is
weighted. Texel grid and content are unchanged (radius and snapping are untouched).

**Fix.** Use the distance from the eye in all four places. Volumetric fog keeps upstream's
per-ray behaviour (patch 2 note) but keyed on the distance to the surface behind the march
(`fragDist`, already computed) instead of its view depth, so the fog is a function of the
world position and the view ray only (what a later world-space visibility cache needs).
Looking straight ahead the result is unchanged at the screen centre; off-centre it is
what the centre shows for the same spot. Patch 2's skip becomes `fragDist >= range` (still
exactly the steps weighted by 0, and since `fragDist >= depth` it skips at least as often).
Patch 3 is untouched (its shortcut doesn't involve the fades).

| File | Change |
|---|---|
| `shaders/deferred/vfog_radiance.frag` | `VFog_SampleShadowDir(…, fadeDist, …)` gets `fragDist`; patch 2's skip uses `fragDist` |
| `shaders/deferred/vfog_transmittance.frag` | `rayDist = min(length(V_GetViewPosition(vTexCoord, depth)), maxDistance)`; includes `wrap/view.glsl` |
| `shaders/deferred/fog.frag` | fog factor from the view-space distance (sky test still by depth); includes `wrap/view.glsl` |
| `shaders/deferred/lighting.frag` | `L_SampleShadowDir(…, length(P - uView.position), …)` |

App side, `src/shadow.c` (`SunShadowNeedsUpdate`) replicates the fade: its `distFade` is
now by distance from the eye. Reuse gets slightly more lenient (frustum corners beyond
`range` no longer need coverage); the "same texels where it covers" guarantee is unchanged.

**Still orientation-dependent in R3D (not changed):**
- `scene/forward.frag` (`L_SampleShadowDir(…, vLinearDepth, …)` and `FogColorMix(…, vLinearDepth)`)
  and `scene/unlit.frag` (`FogColorMix`): forward / unlit objects only, which monolith doesn't
  draw. Same fix applies (`length(vPosition - uViewPosition)`; unlit has no camera
  position uniform).
- Screen-space by nature: the fog's half-resolution march jitter and Vogel-disk rotation
  (`M_HashIGN(gl_FragCoord)`), the bilateral upsample (`vfog_compose.frag`, weights by view
  depth), SSAO, SSIL, bloom, FXAA. Measured below; they stay at the reprojection noise level.
- The camera-fitted shadow map still moves with the view (re-render cost, not image): a
  rotation-stable fit (centre on the eye) would let the app never re-render when only turning.

**Verified.**
- `glslangValidator` passes all four shaders (includes resolved with `scripts/glsl_processor.py`).
  `./build.sh` and `./build.sh --win`: zero warnings.
- Rotation invariance, measured: from a fixed eye, shots at yaw ±10/20/30° (and pitch −15/−7/+13°
  in the hall) are reprojected into the reference shot with the pure-rotation homography and
  diffed on the overlap (4× downscaled to drop resampling noise). Mean |Δ| (0–255) / share of
  pixels with |Δ| > 6, averaged over 6–9 angles each, before → after:

  | Location (`--pos`, base `--look`) | all effects | `--off 345` | `--off 1345` (dist. fog only) | `--off 12345` (no fog) |
  |---|---|---|---|---|
  | Hall (−6 −0.02 34, 160 7) | 0.90 / 0.92 % → 0.60 / 0.19 % | 0.89 / 0.87 % → 0.60 / 0.14 % | 1.48 / 3.54 % → 0.65 / 0.22 % | 0.81 / 0.84 % → 0.81 / 0.83 % |
  | Canyon (0 −0.02 88, 180 3) | 0.84 / 1.03 % → 0.68 / 0.68 % | 0.71 / 0.46 % → 0.56 / 0.16 % | 0.73 / 0.64 % → 0.57 / 0.10 % | 0.62 / 0.32 % → 0.62 / 0.30 % |
  | Terrace (8 11.98 −70, 195 10) | 0.69 / 0.04 % → 0.51 / 0.02 % | 0.63 / 0.01 % → 0.50 / 0.01 % | 2.41 / 9.00 % → 0.65 / 0.04 % | 0.82 / 0.52 % → 0.82 / 0.52 % |
  | Steep (−52.22 61.62 39.53, 140 −40) | 0.90 / 0.25 % → 0.41 / 0.00 % | 0.94 / 0.35 % → 0.42 / 0.00 % | 3.66 / 17.4 % → 0.49 / 0.00 % | 3.47 / 10.4 % → 0.83 / 0.54 % |

  Volumetric fog alone (signed difference `--off 345` − `--off 1345`, reprojected): hall
  1.14 / 1.50 % (max 70) → 0.48 / 0.01 %, canyon 0.59 / 0.30 % → 0.37 / 0.02 %, terrace
  2.21 / 7.06 % → 0.52 / 0.00 %, steep 3.47 / 16.1 % → 0.46 / 0.00 %. Per term (fixed one at a
  time): sun shadow fade = the steep `12345` column; distance fog = the `1345` column;
  transmittance = most of the terrace/steep fog error; the shadow fade by view depth = the hall
  and canyon fog error (hall 0.64 % → 0.01 %). Screen-space residual (SSAO / SSIL / bloom, no
  fog): within ±0.5 % of the `12345` noise floor.
- Look vs. before (26 verification shots): 127 534 pixels differ by > 8/255 in total (0.34 %);
  10 images have none. Largest: yaw sweep y 1.6 / yaw 240 (51 221 px, max 74: the opening at the
  right screen edge now glows with sunlit fog as it does at the centre; region 11 % brighter), Stairs view 4
  (37 717 px, max 19: far facade beyond 75 m, slightly brighter: off-centre, shadows now fade and fog thickens by true distance), Tower view 6 (13 152 px, max 28).
  Eye y 100 sweeps: max 6/255.
- Shadow reuse still exact: 7 shots after 30 frames of turning (2–12°/frame, some also moving)
  match `--shadow-always` (≤ 136 pixels > 2/255, max 13: the 16-bit depth-quantisation noise).

```diff
--- a/shaders/deferred/lighting.frag
+++ b/shaders/deferred/lighting.frag
@@ -137,7 +137,9 @@
         mat2 diskRot = L_ShadowDebandingMatrix(gl_FragCoord.xy);
         switch (uLight.type)
         {
-        case LIGHT_DIR:  shadow *= L_SampleShadowDir(uLight, P, depth, NoL, diskRot); break;
+        // Fade by distance from the eye, not view depth: view depth changes when the
+        // camera only turns (local patch 4, see patches.md)
+        case LIGHT_DIR:  shadow *= L_SampleShadowDir(uLight, P, length(P - uView.position), NoL, diskRot); break;
         case LIGHT_SPOT: shadow *= L_SampleShadowSpot(uLight, P, NoL, diskRot); break;
         case LIGHT_OMNI: shadow *= L_SampleShadowOmni(uLight, P, NoL, diskRot); break;
         }
--- a/shaders/deferred/fog.frag
+++ b/shaders/deferred/fog.frag
@@ -10,6 +10,7 @@
 
 #include <ubo/view.glsl>
 #include <wrap/fog.glsl>
+#include <wrap/view.glsl>
 
 noperspective in vec2 vTexCoord;
 uniform sampler2D uDepthTex;
@@ -18,5 +19,9 @@
 void main()
 {
     float depth = texelFetch(uDepthTex, ivec2(gl_FragCoord.xy), 0).r;
-    FragColor = FogColorAlpha(depth, uView.far);
+
+    // Fog by distance from the eye, not view depth: view depth changes when the
+    // camera only turns (local patch 4, see patches.md)
+    float dist = length(V_GetViewPosition(vTexCoord, depth));
+    FragColor = vec4(uFog.color.rgb, (depth >= uView.far) ? uFog.skyAffect : FogFactor(dist));
 }
--- a/shaders/deferred/vfog_transmittance.frag
+++ b/shaders/deferred/vfog_transmittance.frag
@@ -15,6 +15,7 @@
 #include <lib/math.glsl>
 #include <ubo/view.glsl>
 #include <ubo/fx.glsl>
+#include <wrap/view.glsl>
 
 // =========================
 // In - Varyings
@@ -41,7 +42,9 @@
 void main()
 {
     float depth = texelFetch(uDepthTex, ivec2(gl_FragCoord.xy), 0).r;
-    float rayDist = min(depth, uVFog.maxDistance);
+    // Distance along the view ray, not view depth: view depth changes when the camera
+    // only turns, and the radiance march integrates over distance (local patch 4)
+    float rayDist = min(length(V_GetViewPosition(vTexCoord, depth)), uVFog.maxDistance);
 
     // Analytic transmittance over the full ray distance
     float sigmaT = uVFog.scatteringDensity + uVFog.absortionDensity;
--- a/shaders/deferred/vfog_radiance.frag
+++ b/shaders/deferred/vfog_radiance.frag
@@ -70,7 +70,11 @@
 // when the reference depth is in front of (or behind) every texel the 8 PCF taps can
 // touch, each tap returns exactly 1 (or 0), so the taps are skipped. The texels come
 // from the min/max depth tiles of uShadowDirMinMaxTex, 2x2 tiles cover the footprint.
-float VFog_SampleShadowDir(vec3 Pws, float Zvs, mat2 diskRot)
+// fadeDist replaces L_SampleShadowDir's view depth (local patch 4): the distance from
+// the eye to the surface behind the march, so the fade doesn't change when the camera
+// only turns. Within that distance (< range) every step on screen lies well inside the
+// camera-fitted map, so edgeFade is 1 there and doesn't depend on the camera either.
+float VFog_SampleShadowDir(vec3 Pws, float fadeDist, mat2 diskRot)
 {
     vec4 Pls = uLight.viewProj * vec4(Pws, 1.0);
     vec3 projCoords = Pls.xyz / Pls.w * 0.5 + 0.5;
@@ -118,7 +122,7 @@
 
     vec3 distToBorder = min(projCoords, 1.0 - projCoords);
     float edgeFade = smoothstep(0.0, 0.05, min(distToBorder.x, min(distToBorder.y, distToBorder.z)));
-    float distFade = smoothstep(uLight.range, uLight.range * 0.75, Zvs);
+    float distFade = smoothstep(uLight.range, uLight.range * 0.75, fadeDist);
 
     return mix(1.0, shadow, edgeFade * distFade * uLight.shadowOpacity);
 }
@@ -202,10 +206,10 @@
     mat2 diskRot = L_ShadowDebandingMatrix(gl_FragCoord.xy);
     bool hasShadow = uLight.shadowLayer >= 0 && uLight.shadowOpacity != 0.0;
 
-    // L_SampleShadowDir fades the shadow out by the pixel's view depth (constant along
-    // the ray) and weights it by exactly 0 at depth >= range, e.g. on sky pixels.
-    // Skip its taps there: same result, no wasted PCF lookups on every march step.
-    if (uLight.type == LIGHT_DIR && depth >= uLight.range) hasShadow = false;
+    // VFog_SampleShadowDir fades the shadow out by the distance to the surface behind the
+    // march (constant along the ray) and weights it by exactly 0 at fragDist >= range,
+    // e.g. on sky pixels. Skip its taps there: same result, no wasted PCF lookups.
+    if (uLight.type == LIGHT_DIR && fragDist >= uLight.range) hasShadow = false;
 
     // Precompute inverses used inside the loop
     float invRange = 1.0 / max(uLight.range, 1e-4);
@@ -225,7 +229,7 @@
         {
             switch (uLight.type)
             {
-            case LIGHT_DIR:  visibility *= VFog_SampleShadowDir(marchPos, depth, diskRot); break;
+            case LIGHT_DIR:  visibility *= VFog_SampleShadowDir(marchPos, fragDist, diskRot); break;
             case LIGHT_SPOT: visibility *= L_SampleShadowSpot(uLight, marchPos, 1.0, diskRot); break;
             case LIGHT_OMNI: visibility *= L_SampleShadowOmni(uLight, marchPos, 1.0, diskRot); break;
             }
```

## Patch 5: per-pass GPU profiler (2026-10-10)

**Symptom.** Not a bug: Phase 3 (render static work once) needs to know where the GPU time
goes before anything gets cached. Until now the only numbers were whole-frame times and
A/B runs with effects switched off, which can't separate e.g. the volumetric fog's march
from its upsample, or show what a shadow re-render costs.

**Cause / approach.** R3D has no timing hooks. `R3D_End()` runs a fixed sequence of
passes, so timer queries around those calls give a complete breakdown.

**Change.** `GL_TIME_ELAPSED` queries around each pass call in `R3D_End()` (the volumetric
fog's three sub-passes are timed separately inside `pass_deferred_volumetric_fog()`), plus
two `GL_TIMESTAMP` queries at the start and end of `R3D_End()` for the whole span.
- Queries sit in a ring of 4 frames. At the start of each `R3D_End()` (and in
  `R3D_PopProfilerFrame()`), finished frames are read back oldest first, only when
  `GL_QUERY_RESULT_AVAILABLE` says their last query is done, so the CPU never waits on the
  GPU. (It would block only if the GPU fell more than 4 frames behind, which the driver's
  frame queue prevents.) Results go into a 16-entry queue for the app.
- `TIME_ELAPSED` queries can't nest, so only top-level pass calls are timed: one query per
  pass per frame. Passes that don't run on a frame report 0, so averages over frames are
  the average cost per frame (e.g. a shadow re-render on 21% of frames shows as 21% of its
  cost). Not timed: the G-buffer clear, screen-shader stages (none in monolith) and the
  gaps between passes. Together they are `total − sum(passes)`, ~0.06 ms here.
- **Compile-time switch:** CMake option `R3D_GPU_PROFILER` (default OFF;
  monolith's `build.sh` turns it ON for Linux and Windows). Without it the `PROF_*` macros
  expand to `((void)0)` and the public functions are stubs (`R3D_IsProfilerAvailable()`
  returns false), so the app code needs no `#ifdef`.
- **Runtime switch:** compiled in, the profiler stays off until
  `R3D_SetProfilerEnabled(true)`. While off, each pass costs one predictable branch and no
  GL calls. Rendering is never touched: no state changes, only query objects.
- API (`include/r3d/r3d_profiler.h`, not included by `r3d.h`): `R3D_IsProfilerAvailable`,
  `R3D_SetProfilerEnabled` (false also deletes the queries; call it before `R3D_Close`),
  `R3D_IsProfilerEnabled`, `R3D_SetProfilerTag` (an int stored with the next frames, so the
  app can map late results to the frame that produced them), `R3D_PopProfilerFrame`,
  `R3D_FlushProfiler` (blocking, once at the end of a measurement),
  `R3D_GetProfilerDroppedCount`, `R3D_GetProfilerPassName`. The GL entry points
  (`glQueryCounter`, `glGetQueryObjectui64v`, ...) come through R3D's glad loader, which is
  why it also builds with MinGW.

| File | Change |
|---|---|
| `include/r3d/r3d_profiler.h` | new: pass enum, `R3D_ProfilerFrame`, API |
| `src/r3d_draw.c` | profiler ring + read-back (`#ifdef R3D_GPU_PROFILER`), `PROF_PASS()`/`PROF_BEGIN`/`PROF_END` around the passes in `R3D_End()` and the 3 vfog sub-passes, API functions (stubs without the define) |
| `CMakeLists.txt` | `option(R3D_GPU_PROFILER ...)` → `target_compile_definitions(r3d PRIVATE R3D_GPU_PROFILER)` |

**Verified.**
- Image: `--shot` sweeps (26 shots: 7 viewpoints, yaw sweeps at y 1.6/60/100, the steep view)
  with the profiler compiled in, compiled in **and enabled** during the shot (temporary
  test build), and both with the unoptimised and the Release R3D: 0 pixels differ from the
  pre-patch baseline in every case.
- Cost: interleaved headless pairs (full 600-frame runs), Release, profiler on vs.
  `--no-profile`: 5.021/5.094/4.972 vs. 4.987/5.158/5.097 ms. No measurable overhead.
  Compiled out vs. compiled in but off (unoptimised R3D, 4 rotated rounds, noisy GPU clock
  states): best runs 5.007 vs. 5.003 ms, i.e. no difference either.
- Sanity: sum of passes + untimed rest = GPU total; GPU total + CPU time ≈ headless frame
  time (headless runs `glFinish` every frame, so CPU and GPU don't overlap). The shadow-map
  row divided by the re-render fraction gives ~1.2 ms per re-render, matching the earlier
  A/B estimate in `.claude/skills/lighting-fog-shadows/r3d-pitfalls.md` §3; min/max reduction ~0.37 ms per re-render
  (§7.3 said ~0.35).
- Builds without warnings with `-Werror` (R3D's own flags) on Linux (unoptimised and
  Release builds) and MinGW; runs under wine too.

**Upstream.** Not upstream; monolith-specific tooling. Worth offering if upstream wants a
profiler, but the pass list would have to follow their `R3D_End()`.

```diff
diff --git a/CMakeLists.txt b/CMakeLists.txt
index 62c4e16..3059644 100644
--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -35,6 +35,7 @@ option(R3D_ASSIMP_VENDORED "Use vendored assimp from submodule" OFF)
 option(R3D_SUPPORT_ASSIMP "Build with Assimp importer support" ON)
 option(R3D_BUILD_EXAMPLES "Build the examples" ${R3D_IS_MAIN})
 option(R3D_BUILD_DOCS "Build the doxygen documentation" OFF)
+option(R3D_GPU_PROFILER "Compile in the per-pass GPU timer queries (local patch, see patches.md)" OFF)
 
 # ========================================
 # Output Directories
@@ -427,6 +428,11 @@ if(CMAKE_VERSION VERSION_GREATER 3.12)
     set_property(TARGET ${PROJECT_NAME} PROPERTY C_STANDARD 11)
 endif()
 
+# Local patch (see patches.md): per-pass GPU profiler, see include/r3d/r3d_profiler.h
+if(R3D_GPU_PROFILER)
+    target_compile_definitions(${PROJECT_NAME} PRIVATE R3D_GPU_PROFILER)
+endif()
+
 if(MSVC)
     target_compile_options(${PROJECT_NAME} PRIVATE /experimental:c11atomics)
     target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
diff --git a/src/r3d_draw.c b/src/r3d_draw.c
index 4ace8b1..cde6150 100644
--- a/src/r3d_draw.c
+++ b/src/r3d_draw.c
@@ -7,9 +7,11 @@
  */
 
 #include <r3d/r3d_draw.h>
+#include <r3d/r3d_profiler.h>
 #include <r3d_config.h>
 #include <raymath.h>
 #include <stddef.h>
+#include <string.h>
 #include <float.h>
 #include <rlgl.h>
 #include <glad.h>
@@ -52,6 +54,155 @@
 #define IS_SHADOW_CAST_ONLY(mode)               \
     ((R3D_SHADOW_CAST_ONLY_MASK & (1 << (mode))) != 0)
 
+// ========================================
+// GPU PROFILER (local patch, see patches.md)
+// ========================================
+
+// Per-pass GL_TIME_ELAPSED queries around the passes of R3D_End(), plus GL_TIMESTAMP
+// queries at its start and end. Each frame records into one slot of a ring; a slot is
+// read back only once its last query is available, and before the ring wraps around,
+// so the CPU doesn't wait on the GPU. Compiled in with -DR3D_GPU_PROFILER; without it
+// PROF_* expand to nothing and the public functions are stubs.
+
+#ifdef R3D_GPU_PROFILER
+
+#define PROF_RING  4    //< frames in flight; drivers queue at most ~3, so a full ring never stalls
+#define PROF_QUEUE 16   //< finished results waiting for R3D_PopProfilerFrame()
+
+typedef struct {
+    GLuint pass[R3D_PROFILER_PASS_COUNT];
+    GLuint tsBegin, tsEnd;
+    bool used[R3D_PROFILER_PASS_COUNT];
+    bool pending;                       //< recorded, not read back yet
+    uint64_t id;
+    int tag;
+} prof_slot_t;
+
+static struct {
+    bool enabled;
+    bool created;
+    bool recording;                     //< inside R3D_End() of a profiled frame
+    int active;                         //< open pass or -1 (TIME_ELAPSED queries can't nest)
+    int slot;                           //< slot of the current / last recorded frame
+    int tag;
+    uint64_t nextId;
+    prof_slot_t slots[PROF_RING];
+    R3D_ProfilerFrame queue[PROF_QUEUE];
+    int queueHead, queueCount, dropped;
+} PROF = { .active = -1, .tag = -1 };
+
+static void prof_read_slot(prof_slot_t* s)
+{
+    R3D_ProfilerFrame f = { .id = s->id, .tag = s->tag };
+
+    GLuint64 t0 = 0, t1 = 0;
+    glGetQueryObjectui64v(s->tsBegin, GL_QUERY_RESULT, &t0);
+    glGetQueryObjectui64v(s->tsEnd, GL_QUERY_RESULT, &t1);
+    f.totalMs = (float)((double)(t1 - t0) * 1e-6);
+
+    for (int i = 0; i < R3D_PROFILER_PASS_COUNT; i++) {
+        if (!s->used[i]) continue;
+        GLuint64 ns = 0;
+        glGetQueryObjectui64v(s->pass[i], GL_QUERY_RESULT, &ns);
+        f.passMs[i] = (float)((double)ns * 1e-6);
+        f.ran[i] = true;
+    }
+    s->pending = false;
+
+    if (PROF.queueCount == PROF_QUEUE) {
+        PROF.queueHead = (PROF.queueHead + 1) % PROF_QUEUE;
+        PROF.queueCount--;
+        PROF.dropped++;
+    }
+    PROF.queue[(PROF.queueHead + PROF.queueCount) % PROF_QUEUE] = f;
+    PROF.queueCount++;
+}
+
+// Reads back finished slots, oldest first; stops at the first unfinished one so results
+// stay in order. Non-blocking unless 'wait' is set.
+static void prof_collect(bool wait)
+{
+    for (int k = 1; k <= PROF_RING; k++) {
+        prof_slot_t* s = &PROF.slots[(PROF.slot + k) % PROF_RING];
+        if (!s->pending) continue;
+        if (!wait) {
+            GLuint available = GL_FALSE;
+            glGetQueryObjectuiv(s->tsEnd, GL_QUERY_RESULT_AVAILABLE, &available);
+            if (!available) break;
+        }
+        prof_read_slot(s);
+    }
+}
+
+static void prof_frame_begin(void)
+{
+    if (!PROF.enabled) return;
+
+    if (!PROF.created) {
+        for (int i = 0; i < PROF_RING; i++) {
+            glGenQueries(R3D_PROFILER_PASS_COUNT, PROF.slots[i].pass);
+            glGenQueries(1, &PROF.slots[i].tsBegin);
+            glGenQueries(1, &PROF.slots[i].tsEnd);
+        }
+        PROF.created = true;
+    }
+
+    prof_collect(false);
+
+    PROF.slot = (PROF.slot + 1) % PROF_RING;
+    prof_slot_t* s = &PROF.slots[PROF.slot];
+    if (s->pending) prof_read_slot(s);  //< ring full: only if the GPU is > PROF_RING frames behind
+
+    memset(s->used, 0, sizeof(s->used));
+    s->id = PROF.nextId++;
+    s->tag = PROF.tag;
+    glQueryCounter(s->tsBegin, GL_TIMESTAMP);
+    PROF.recording = true;
+}
+
+static void prof_frame_end(void)
+{
+    if (!PROF.recording) return;
+    prof_slot_t* s = &PROF.slots[PROF.slot];
+    glQueryCounter(s->tsEnd, GL_TIMESTAMP);
+    s->pending = true;
+    PROF.recording = false;
+}
+
+static void prof_begin(R3D_ProfilerPass pass)
+{
+    if (!PROF.recording) return;
+    prof_slot_t* s = &PROF.slots[PROF.slot];
+    if (PROF.active >= 0 || s->used[pass]) return;  //< no nesting, one query per pass and frame
+    glBeginQuery(GL_TIME_ELAPSED, s->pass[pass]);
+    s->used[pass] = true;
+    PROF.active = (int)pass;
+}
+
+static void prof_end(void)
+{
+    if (PROF.active < 0) return;
+    glEndQuery(GL_TIME_ELAPSED);
+    PROF.active = -1;
+}
+
+#define PROF_FRAME_BEGIN()  prof_frame_begin()
+#define PROF_FRAME_END()    prof_frame_end()
+#define PROF_BEGIN(pass)    prof_begin(pass)
+#define PROF_END()          prof_end()
+
+#else
+
+#define PROF_FRAME_BEGIN()  ((void)0)
+#define PROF_FRAME_END()    ((void)0)
+#define PROF_BEGIN(pass)    ((void)0)
+#define PROF_END()          ((void)0)
+
+#endif // R3D_GPU_PROFILER
+
+// Times one pass call: PROF_PASS(R3D_PROFILER_SSAO, ssaoSource = pass_prepare_ssao());
+#define PROF_PASS(pass, ...) do { PROF_BEGIN(pass); __VA_ARGS__; PROF_END(); } while (0)
+
 // ========================================
 // INTERNAL FUNCTIONS
 // ========================================
@@ -140,6 +291,8 @@ void R3D_BeginPro(R3D_View view)
 
 void R3D_End(void)
 {
+    PROF_FRAME_BEGIN();
+
     r3d_render_prepare_drawing(); // bind global VAO
 
     /* --- Invalidates OpenGL cache and save some infos --- */
@@ -158,10 +311,10 @@ void R3D_End(void)
 
     if (r3d_light_has_shadow_job())
     {
-        pass_scene_shadows();
+        PROF_PASS(R3D_PROFILER_SHADOWS, pass_scene_shadows());
     }
 
-    pass_scene_shadow_minmax();
+    PROF_PASS(R3D_PROFILER_SHADOW_MINMAX, pass_scene_shadow_minmax());
 
     r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_SHADOW_DIR, r3d_light_shadow_map(R3D_LIGHT_DIR));
     r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_SHADOW_SPOT, r3d_light_shadow_map(R3D_LIGHT_SPOT));
@@ -177,7 +330,7 @@ void R3D_End(void)
 
         if (r3d_env_has_any_probe_jobs())
         {
-            pass_scene_probes();
+            PROF_PASS(R3D_PROFILER_PROBES, pass_scene_probes());
         }
     }
 
@@ -210,10 +363,10 @@ void R3D_End(void)
 
     if (r3d_render_has_opaque())
     {
-        pass_scene_geometry();
+        PROF_PASS(R3D_PROFILER_GEOMETRY, pass_scene_geometry());
 
-        if (r3d_render_has_decals()) pass_scene_decals();
-        if (r3d_light_has_visible()) pass_deferred_lights();
+        if (r3d_render_has_decals()) PROF_PASS(R3D_PROFILER_DECALS, pass_scene_decals());
+        if (r3d_light_has_visible()) PROF_PASS(R3D_PROFILER_LIGHTS, pass_deferred_lights());
 
         bool ssao = R3D.environment.ssao.enabled;
         bool ssil = R3D.environment.ssil.enabled;
@@ -224,20 +377,22 @@ void R3D_End(void)
 
         if (ssao || ssil || ssgi || ssr || vfog || dof)
         {
+            PROF_BEGIN(R3D_PROFILER_PYRAMID);
             pass_prepare_pyramid();
 
             if (ssao || ssil || ssgi || ssr) pass_prepare_downsample(R3D_TARGET_NORMAL, 1);
             // skip ssr check for radiance, we re-downsample during ssr for the full ambient reflection
             if (ssil || ssgi) pass_prepare_downsample(R3D_TARGET_RADIANCE, 1);
+            PROF_END();
         }
 
-        if (ssao) ssaoSource = pass_prepare_ssao();
-        if (ssil) ssilSource = pass_prepare_ssil();
-        if (ssgi) ssgiSource = pass_prepare_ssgi();
-        pass_deferred_ambient(ssaoSource, ssilSource, ssgiSource);
+        if (ssao) PROF_PASS(R3D_PROFILER_SSAO, ssaoSource = pass_prepare_ssao());
+        if (ssil) PROF_PASS(R3D_PROFILER_SSIL, ssilSource = pass_prepare_ssil());
+        if (ssgi) PROF_PASS(R3D_PROFILER_SSGI, ssgiSource = pass_prepare_ssgi());
+        PROF_PASS(R3D_PROFILER_AMBIENT, pass_deferred_ambient(ssaoSource, ssilSource, ssgiSource));
 
-        if (ssr) ssrSource = pass_prepare_ssr();
-        pass_deferred_compose(sceneTarget, ssrSource);
+        if (ssr) PROF_PASS(R3D_PROFILER_SSR, ssrSource = pass_prepare_ssr());
+        PROF_PASS(R3D_PROFILER_COMPOSE, pass_deferred_compose(sceneTarget, ssrSource));
     }
     else
     {
@@ -251,11 +406,11 @@ void R3D_End(void)
 
     /* --- Then background/fog and forward rendering --- */
 
-    pass_scene_background(sceneTarget);
+    PROF_PASS(R3D_PROFILER_BACKGROUND, pass_scene_background(sceneTarget));
 
     if (R3D.environment.fog.mode != R3D_FOG_DISABLED)
     {
-        pass_deferred_fog(sceneTarget);
+        PROF_PASS(R3D_PROFILER_FOG, pass_deferred_fog(sceneTarget));
     }
 
     if (R3D.environment.volumetricFog.enabled)
@@ -263,7 +418,7 @@ void R3D_End(void)
         pass_deferred_volumetric_fog(sceneTarget);
     }
 
-    pass_scene_forward(sceneTarget);
+    PROF_PASS(R3D_PROFILER_FORWARD, pass_scene_forward(sceneTarget));
 
     /* --- Applying effects over the scene and final blit --- */
 
@@ -272,31 +427,31 @@ void R3D_End(void)
 
     if (R3D.environment.dof.mode != R3D_DOF_DISABLED)
     {
-        sceneTarget = pass_post_dof(sceneTarget);
+        PROF_PASS(R3D_PROFILER_DOF, sceneTarget = pass_post_dof(sceneTarget));
     }
 
     if (R3D.environment.bloom.mode != R3D_BLOOM_DISABLED)
     {
-        sceneTarget = pass_post_bloom(sceneTarget);
+        PROF_PASS(R3D_PROFILER_BLOOM, sceneTarget = pass_post_bloom(sceneTarget));
     }
 
     if (R3D.environment.autoExposure.enabled)
     {
-        sceneTarget = pass_post_auto_exposure(sceneTarget);
+        PROF_PASS(R3D_PROFILER_AUTO_EXPOSURE, sceneTarget = pass_post_auto_exposure(sceneTarget));
     }
 
     sceneTarget = pass_post_screen(R3D_SCREEN_SHADER_STAGE_POST, sceneTarget);
-    sceneTarget = pass_post_output(sceneTarget);
+    PROF_PASS(R3D_PROFILER_OUTPUT, sceneTarget = pass_post_output(sceneTarget));
 
     sceneTarget = pass_post_screen(R3D_SCREEN_SHADER_STAGE_OUTPUT, sceneTarget);
 
     switch (R3D.aaMode)
     {
     case R3D_ANTI_ALIASING_MODE_FXAA:
-        sceneTarget = pass_post_fxaa(sceneTarget);
+        PROF_PASS(R3D_PROFILER_AA, sceneTarget = pass_post_fxaa(sceneTarget));
         break;
     case R3D_ANTI_ALIASING_MODE_SMAA:
-        sceneTarget = pass_post_smaa(sceneTarget);
+        PROF_PASS(R3D_PROFILER_AA, sceneTarget = pass_post_smaa(sceneTarget));
         break;
     default:
         break;
@@ -304,6 +459,7 @@ void R3D_End(void)
 
     sceneTarget = pass_post_screen(R3D_SCREEN_SHADER_STAGE_FINAL, sceneTarget);
 
+    PROF_BEGIN(R3D_PROFILER_BLIT);
     switch (R3D.outputMode)
     {
     case R3D_OUTPUT_SCENE:    blit_to_screen(r3d_target_swap_scene(sceneTarget)); break;
@@ -319,11 +475,14 @@ void R3D_End(void)
     case R3D_OUTPUT_BLOOM:    visualize_to_screen(R3D_TARGET_BLOOM); break;
     case R3D_OUTPUT_DOF:      visualize_to_screen(R3D_TARGET_DOF_COC); break;
     }
+    PROF_END();
 
     /* --- Reset internal stuff and states changed by R3D --- */
 
     r3d_stack_reset(R3D.stack);
     cleanup_after_render();
+
+    PROF_FRAME_END();
 }
 
 void R3D_BeginCluster(BoundingBox aabb)
@@ -2320,13 +2479,16 @@ void pass_deferred_volumetric_fog(r3d_target_t sceneTarget)
 
     R3D_TARGET_BIND_LOAD(0, false, sceneTarget);
 
+    PROF_BEGIN(R3D_PROFILER_VFOG_TRANSMITTANCE);
     R3D_SHADER_USE(deferred.vfogTransmittance);
     R3D_SHADER_BIND_SAMPLER(deferred.vfogTransmittance, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 0));
 
     R3D_RENDER_SCREEN();
+    PROF_END();
 
     /* --- Accumulate radiance in half resolution --- */
 
+    PROF_BEGIN(R3D_PROFILER_VFOG_RADIANCE);
     R3D_TARGET_BIND_CLEAR(1, false, R3D_TARGET_VFOG_RAD);
 
     r3d_driver_enable(GL_SCISSOR_TEST);
@@ -2372,9 +2534,11 @@ void pass_deferred_volumetric_fog(r3d_target_t sceneTarget)
 
     r3d_driver_set_scissor(0, 0, R3D_TARGET_SIZE_W, R3D_TARGET_SIZE_H);
     r3d_driver_disable(GL_SCISSOR_TEST);
+    PROF_END();
 
     /* --- Compose radiance to the scene --- */
 
+    PROF_BEGIN(R3D_PROFILER_VFOG_COMPOSE);
     R3D_TARGET_BIND_LOAD(0, false, sceneTarget);
     R3D_SHADER_USE(deferred.vfogCompose);
 
@@ -2385,6 +2549,7 @@ void pass_deferred_volumetric_fog(r3d_target_t sceneTarget)
     R3D_SHADER_BIND_SAMPLER(deferred.vfogCompose, uDepthTex, r3d_target_get_levels(R3D_TARGET_DEPTH, 0, 1));
 
     R3D_RENDER_SCREEN();
+    PROF_END();
 }
 
 void pass_scene_forward(r3d_target_t sceneTarget)
@@ -2988,3 +3153,106 @@ void cleanup_after_render(void)
     }
 #endif
 }
+
+// ========================================
+// GPU PROFILER API (local patch, see patches.md)
+// ========================================
+
+static const char* PROF_PASS_NAMES[R3D_PROFILER_PASS_COUNT] = {
+    [R3D_PROFILER_SHADOWS]            = "shadow maps",
+    [R3D_PROFILER_SHADOW_MINMAX]      = "shadow min/max",
+    [R3D_PROFILER_PROBES]             = "probes",
+    [R3D_PROFILER_GEOMETRY]           = "geometry",
+    [R3D_PROFILER_DECALS]             = "decals",
+    [R3D_PROFILER_LIGHTS]             = "lights",
+    [R3D_PROFILER_PYRAMID]            = "depth pyramid",
+    [R3D_PROFILER_SSAO]               = "SSAO",
+    [R3D_PROFILER_SSIL]               = "SSIL",
+    [R3D_PROFILER_SSGI]               = "SSGI",
+    [R3D_PROFILER_SSR]                = "SSR",
+    [R3D_PROFILER_AMBIENT]            = "ambient",
+    [R3D_PROFILER_COMPOSE]            = "compose",
+    [R3D_PROFILER_BACKGROUND]         = "background",
+    [R3D_PROFILER_FOG]                = "fog",
+    [R3D_PROFILER_VFOG_TRANSMITTANCE] = "vfog transmit",
+    [R3D_PROFILER_VFOG_RADIANCE]      = "vfog radiance",
+    [R3D_PROFILER_VFOG_COMPOSE]       = "vfog upsample",
+    [R3D_PROFILER_FORWARD]            = "forward",
+    [R3D_PROFILER_DOF]                = "DoF",
+    [R3D_PROFILER_BLOOM]              = "bloom",
+    [R3D_PROFILER_AUTO_EXPOSURE]      = "auto exposure",
+    [R3D_PROFILER_OUTPUT]             = "tonemap",
+    [R3D_PROFILER_AA]                 = "FXAA/SMAA",
+    [R3D_PROFILER_BLIT]               = "blit",
+};
+
+const char* R3D_GetProfilerPassName(R3D_ProfilerPass pass)
+{
+    if ((int)pass < 0 || pass >= R3D_PROFILER_PASS_COUNT) return "?";
+    return PROF_PASS_NAMES[pass];
+}
+
+#ifdef R3D_GPU_PROFILER
+
+bool R3D_IsProfilerAvailable(void)
+{
+    return true;
+}
+
+void R3D_SetProfilerEnabled(bool enabled)
+{
+    if (enabled == PROF.enabled) return;
+    if (!enabled && PROF.created) {
+        for (int i = 0; i < PROF_RING; i++) {
+            glDeleteQueries(R3D_PROFILER_PASS_COUNT, PROF.slots[i].pass);
+            glDeleteQueries(1, &PROF.slots[i].tsBegin);
+            glDeleteQueries(1, &PROF.slots[i].tsEnd);
+        }
+        memset(PROF.slots, 0, sizeof(PROF.slots));
+        PROF.created = false;
+        PROF.queueHead = PROF.queueCount = 0;
+    }
+    PROF.enabled = enabled;
+}
+
+bool R3D_IsProfilerEnabled(void)
+{
+    return PROF.enabled;
+}
+
+void R3D_SetProfilerTag(int tag)
+{
+    PROF.tag = tag;
+}
+
+bool R3D_PopProfilerFrame(R3D_ProfilerFrame* frame)
+{
+    if (PROF.enabled) prof_collect(false);
+    if (PROF.queueCount == 0) return false;
+    *frame = PROF.queue[PROF.queueHead];
+    PROF.queueHead = (PROF.queueHead + 1) % PROF_QUEUE;
+    PROF.queueCount--;
+    return true;
+}
+
+void R3D_FlushProfiler(void)
+{
+    if (PROF.enabled && PROF.created) prof_collect(true);
+}
+
+int R3D_GetProfilerDroppedCount(void)
+{
+    return PROF.dropped;
+}
+
+#else
+
+bool R3D_IsProfilerAvailable(void) { return false; }
+void R3D_SetProfilerEnabled(bool enabled) { (void)enabled; }
+bool R3D_IsProfilerEnabled(void) { return false; }
+void R3D_SetProfilerTag(int tag) { (void)tag; }
+bool R3D_PopProfilerFrame(R3D_ProfilerFrame* frame) { (void)frame; return false; }
+void R3D_FlushProfiler(void) { }
+int R3D_GetProfilerDroppedCount(void) { return 0; }
+
+#endif // R3D_GPU_PROFILER
diff --git a/include/r3d/r3d_profiler.h b/include/r3d/r3d_profiler.h
new file mode 100644
index 0000000..41313be
--- /dev/null
+++ b/include/r3d/r3d_profiler.h
@@ -0,0 +1,112 @@
+/* r3d_profiler.h -- R3D GPU pass profiler (local monolith patch, see patches.md).
+ *
+ * Per-pass GPU timings of R3D_End(), measured with GL_TIME_ELAPSED queries plus two
+ * GL_TIMESTAMP queries for the whole R3D_End() span. Queries live in a ring of frames
+ * and are read back a few frames later, only once available, so the CPU never waits
+ * for the GPU (except when the ring is full, which the driver's frame queue prevents).
+ *
+ * The code is compiled in only when R3D is built with -DR3D_GPU_PROFILER=ON. Without it
+ * these functions still exist: R3D_IsProfilerAvailable() returns false and the rest
+ * are no-ops. Even when compiled in, the profiler is off until R3D_SetProfilerEnabled
+ * (true); while off, each pass costs one predictable branch and issues no GL calls.
+ * Rendering itself is never changed by the profiler.
+ */
+
+#ifndef R3D_PROFILER_H
+#define R3D_PROFILER_H
+
+#include "./r3d_platform.h"
+#include <stdbool.h>
+#include <stdint.h>
+
+#ifdef __cplusplus
+extern "C" {
+#endif
+
+/**
+ * @brief The timed passes, in R3D_End() order.
+ */
+typedef enum R3D_ProfilerPass {
+    R3D_PROFILER_SHADOWS,             ///< Shadow map rendering (only on frames that update a map)
+    R3D_PROFILER_SHADOW_MINMAX,       ///< Directional shadow min/max tile reduction (local patch 3)
+    R3D_PROFILER_PROBES,              ///< Environment probe capture
+    R3D_PROFILER_GEOMETRY,            ///< G-buffer fill (opaque geometry, surface shaders)
+    R3D_PROFILER_DECALS,              ///< Deferred decals
+    R3D_PROFILER_LIGHTS,              ///< Deferred direct lighting (incl. shadow PCF)
+    R3D_PROFILER_PYRAMID,             ///< Depth pyramid + normal/radiance downsamples
+    R3D_PROFILER_SSAO,                ///< SSAO (incl. its blur)
+    R3D_PROFILER_SSIL,                ///< SSIL (incl. its blur/history)
+    R3D_PROFILER_SSGI,                ///< SSGI
+    R3D_PROFILER_SSR,                 ///< SSR
+    R3D_PROFILER_AMBIENT,             ///< Deferred ambient / IBL / indirect
+    R3D_PROFILER_COMPOSE,             ///< Deferred compose into the scene target
+    R3D_PROFILER_BACKGROUND,          ///< Sky / background
+    R3D_PROFILER_FOG,                 ///< Distance fog
+    R3D_PROFILER_VFOG_TRANSMITTANCE,  ///< Volumetric fog: transmittance (full res)
+    R3D_PROFILER_VFOG_RADIANCE,       ///< Volumetric fog: radiance march per light (half res)
+    R3D_PROFILER_VFOG_COMPOSE,        ///< Volumetric fog: bilateral upsample + compose
+    R3D_PROFILER_FORWARD,             ///< Forward (unlit / blended) objects
+    R3D_PROFILER_DOF,                 ///< Depth of field
+    R3D_PROFILER_BLOOM,               ///< Bloom
+    R3D_PROFILER_AUTO_EXPOSURE,       ///< Auto exposure
+    R3D_PROFILER_OUTPUT,              ///< Tonemap / color adjustments (post output)
+    R3D_PROFILER_AA,                  ///< FXAA or SMAA
+    R3D_PROFILER_BLIT,                ///< Final blit / visualisation to the screen or target
+    R3D_PROFILER_PASS_COUNT
+} R3D_ProfilerPass;
+
+/**
+ * @brief GPU timings of one R3D_End() call.
+ *
+ * passMs[i] is 0 for passes that didn't run that frame (ran[i] == false), so averaging
+ * passMs over frames gives the average cost per frame. totalMs is the GPU time between
+ * the start and the end of R3D_End(); totalMs - sum(passMs) is the untimed rest
+ * (G-buffer clear, screen shaders, gaps between passes).
+ */
+typedef struct R3D_ProfilerFrame {
+    uint64_t id;                                ///< Sequence number (counts profiled frames)
+    int tag;                                    ///< Value of R3D_SetProfilerTag() at R3D_End()
+    float totalMs;                              ///< Whole R3D_End() span on the GPU
+    float passMs[R3D_PROFILER_PASS_COUNT];      ///< Per pass
+    bool ran[R3D_PROFILER_PASS_COUNT];          ///< Pass ran this frame
+} R3D_ProfilerFrame;
+
+/** @brief True if R3D was built with R3D_GPU_PROFILER. */
+R3DAPI bool R3D_IsProfilerAvailable(void);
+
+/**
+ * @brief Turns timing on or off (default off). Turning it off deletes the GL queries
+ * and drops unread results; call it with false before R3D_Close() to free them.
+ */
+R3DAPI void R3D_SetProfilerEnabled(bool enabled);
+
+/** @brief True while the profiler records. */
+R3DAPI bool R3D_IsProfilerEnabled(void);
+
+/** @brief Tag stored with the frames recorded from now on (e.g. viewpoint/frame index, -1 to ignore). */
+R3DAPI void R3D_SetProfilerTag(int tag);
+
+/**
+ * @brief Takes the oldest finished frame from the result queue (oldest first).
+ * Results arrive 1-3 frames after the R3D_End() that recorded them.
+ * @return false if no finished frame is waiting.
+ */
+R3DAPI bool R3D_PopProfilerFrame(R3D_ProfilerFrame* frame);
+
+/**
+ * @brief Waits for all recorded frames and queues their results (blocks on the GPU).
+ * Use once at the end of a measurement, not per frame.
+ */
+R3DAPI void R3D_FlushProfiler(void);
+
+/** @brief Number of results dropped because the queue was full (not popped in time). */
+R3DAPI int R3D_GetProfilerDroppedCount(void);
+
+/** @brief Short display name of a pass, e.g. "vfog radiance". */
+R3DAPI const char* R3D_GetProfilerPassName(R3D_ProfilerPass pass);
+
+#ifdef __cplusplus
+} // extern "C"
+#endif
+
+#endif // R3D_PROFILER_H
```

## Patch 6: depth pre-pass for the G-buffer pass (2026-10-10)

**Symptom.** The geometry (G-buffer) pass was the largest single GPU pass in monolith's
profile: ~1.07 ms per frame with the bench camera spinning, ~1.45 ms with it still
(headless, 1920×1006, RTX 4050 Laptop), 25–32% of the GPU frame.

**Cause.** The whole map (279 axis-aligned boxes) is one instanced draw of a unit cube with
the procedural concrete surface shader (fbm, formwork, tie holes, streaks; ~7–12 value
noises per pixel). An instanced draw has no front-to-back order, so the expensive shader
ran for every fragment that passed the depth test *at the time*, not just the visible one.
Measured with `GL_SAMPLES_PASSED` queries (temporary instrumentation, 1600×900 shots):

| View | rasterized | shaded (passed GL_LESS) | visible (GL_EQUAL afterwards) | shaded / visible |
|---|---:|---:|---:|---:|
| 1 Canyon | 3.16 M | 1.79 M | 1.43 M | 1.25 |
| 2 Hypostyle hall | 3.85 M | 1.98 M | 1.43 M | 1.39 |
| 3 Above the roof | 4.44 M | 3.20 M | 1.35 M | 2.36 |
| 4 Stairs | 4.98 M | 1.61 M | 1.33 M | 1.21 |
| 5 Terrace | 3.70 M | 2.16 M | 1.35 M | 1.60 |
| 6 Tower | 2.65 M | 1.84 M | 1.11 M | 1.65 |
| 7 Overview | 2.98 M | 2.53 M | 1.30 M | 1.94 |
| steep view | 2.93 M | 2.54 M | 1.41 M | 1.79 |

Upper bound of the shading share, from a temporary trivial surface shader
(`ALBEDO = const`): geometry pass 1.07 → 0.26 ms spinning, 1.40 → 0.34 ms still. So ~75%
of the pass was concrete shading, of which 20–58% was spent on hidden fragments.
R3D had no pre-pass or early-Z option, and two details kept even the existing order from
culling well: the geometry shader always contains `discard` (the alpha test in
`SceneFragment()`, twice) and the pass writes stencil (the material's `REPLACE`), which
together keep the driver from doing early depth/stencil tests reliably.

**Fix.** An opt-in depth pre-pass, `R3D_SetDepthPrepass(true)`:
1. **Pre-pass:** every eligible opaque draw is rendered with colour writes masked and
   `uDepthOnly = true` (the fragment shader returns at once). Depth writes on, stencil
   writes off.
2. **Shading pass:** the same draws again with `GL_EQUAL`, depth writes off. The surface
   shader now runs about once per visible pixel.
3. **Same program in both passes.** Both use a new geometry-program variant
   (`scene.geometryPrepass`: defines `GEOMETRY_PREPASS` + `R3D_NO_ALPHA_TEST`). Its vertex
   shader is the same source with the same defines, so `gl_Position` and the depth are
   bit-identical between the passes and `GL_EQUAL` never misses. The variant has **no
   `discard`** (`ALPHA_TEST()` expands to nothing), so the driver keeps early depth/stencil
   tests in the shading pass even though it writes stencil. (A first version reused the
   regular program with its `discard`: the pass got only ~0.25 ms cheaper instead of ~0.7.)
4. **Coplanar ties keep their winner.** Coplanar faces of different boxes can produce
   bit-identical depths. Upstream (`GL_LESS`) the first one drawn wins; with `GL_EQUAL` all
   of them would pass and the *last* would win. That changed ~11k pixels in the 26 test
   shots (z-fighting speckles on the tower facade and elsewhere). So in the shading pass the
   first passing fragment sets stencil bit `0x80` (`GL_NOTEQUAL` test on that bit, `REPLACE`)
   and later ones at the same pixel fail. One masked `glClearBufferiv(GL_STENCIL)` clears
   the bit after the pass. Result: same winner as before, 0 pixels differ.
5. **Eligibility** (`geometry_call_prepassable()`): `alphaCutoff <= 0` (the variant ignores
   alpha), depth test `LESS`/`LEQUAL`, stencil test `ALWAYS` with `opZFail = KEEP`,
   `opPass` `KEEP` or `REPLACE` and `ref` without bit `0x80`. Other draws go through the
   regular program as before, after the pre-pass. Limitation: while the pre-pass is on,
   stencil bit `0x80` is reserved during the geometry pass.
6. **Driver fix needed for this:** `r3d_driver_set_stencil_func()` cached its compare mask
   in the same pipeline-cache slot as `glStencilMask`'s write mask (`stencilMask`). After
   `glStencilFunc(..., mask 0x80)` the cache believed the write mask was `0x80`, so a later
   `r3d_driver_set_stencil_mask()` with the same value would be skipped while GL still had
   the old write mask. The compare mask now has its own slot (`stencilFuncMask`). Upstream
   never mixes different values there, so nothing changes for the existing passes.

monolith turns it on in `main()` (`scene.concrete.alphaCutoff = 0` (the concrete's alpha
is always 1, so this changes nothing on screen) + `R3D_SetDepthPrepass(true)`); `--no-prepass`
switches it off for A/B tests.

| File | Change |
|---|---|
| `include/r3d/r3d_core.h`, `src/r3d_core.c`, `src/r3d_core_state.h` | `R3D_SetDepthPrepass()` / `R3D_GetDepthPrepass()`, `R3D.depthPrepass` (default off) |
| `src/r3d_draw.c` | `pass_scene_geometry()`: pre-pass loop, then shading loop (`GL_EQUAL` for eligible draws), clears the tie-break bit; `raster_geometry()` takes a mode and uses the variant's program pointer; `geometry_call_prepassable()` |
| `src/modules/r3d_shader.h`, `src/modules/r3d_shader.c` | `scene.geometryPrepass` variant (module + custom programs, loader table, unload/delete), `uDepthOnly` uniform |
| `shaders/scene/geometry.frag` | `uDepthOnly` early return, only in the `GEOMETRY_PREPASS` variant |
| `shaders/include/user/scene.frag` | `ALPHA_TEST()` is empty when `R3D_NO_ALPHA_TEST` is defined |
| `src/modules/r3d_driver.c` | separate cache slot for the stencil compare mask |

**Verified.**
- Image: `$S/shots.sh` (26 shots: 7 viewpoints, yaw sweeps at eye y 1.6/60/100, the steep
  view) with the pre-pass on and with `--no-prepass`: 0 pixels differ from the reference
  (fog-fix build) in both cases. Without the stencil tie-break: 11,355 pixels differed
  (max 32/255), which is how the coplanar-tie issue was found.
- Cost: interleaved headless runs, full 600 frames, 3 rounds each, A = the merged Phase 3
  base build, B1 = this patch (concrete shader unchanged):

  | | geometry pass | GPU total | frame time |
  |---|---:|---:|---:|
  | spinning, A | 1.065 ms | 3.903 ms | 4.785 ms |
  | spinning, B1 | 0.611 ms (−0.454) | 3.401 ms (−0.502) | 4.277 ms (−0.507) |
  | still, A | 1.452 ms | 4.472 ms | 5.356 ms |
  | still, B1 | 0.722 ms (−0.730) | 3.684 ms (−0.788) | 4.561 ms (−0.795) |

  Per run (frame time, ALL): spinning A 4.760/4.800/4.794, B1 4.236/4.307/4.289; still
  A 5.345/5.375/5.349, B1 4.558/4.559/4.566. With the pre-pass the geometry pass costs
  about the same in every view (0.46–0.70 ms) instead of 0.85–1.90 ms.
  The rest of the pass is now mostly the visible pixels' shading: with a trivial surface
  shader it would be ~0.16–0.19 ms.
- All shaders pass `glslangValidator` (geometry vertex shader, base and pre-pass fragment
  variants with and without the concrete user code, the depth variant with it; includes
  resolved with `scripts/glsl_processor.py`). The pre-pass variant contains no `discard`
  after preprocessing; the base variant still has its 2.
- Builds without warnings on Linux (Release) and MinGW.

**Upstream.** Not upstream. A general version would want a per-material opt-in and a
proper tie-break without reserving a stencil bit (e.g. drawing the shading pass in
reverse order); the driver cache fix (step 6) is a real upstream bug worth reporting.

```diff
--- a/include/r3d/r3d_core.h
+++ b/include/r3d/r3d_core.h
@@ -271,6 +271,25 @@
 R3DAPI void R3D_SetOutputMode(R3D_OutputMode mode);
 
 /**
+ * @brief Returns whether the depth pre-pass of the deferred geometry pass is enabled.
+ */
+R3DAPI bool R3D_GetDepthPrepass(void);
+
+/**
+ * @brief Enables or disables a depth-only pre-pass before the deferred geometry pass.
+ *
+ * When enabled, opaque draws whose material has `alphaCutoff <= 0` and a LESS or
+ * LEQUAL depth test are first rendered depth-only, then shaded with an EQUAL depth
+ * test and depth writes off, so the material/surface shader runs about once per
+ * visible pixel instead of once per rasterized (overdrawn) fragment. Worth it when
+ * the surface shader is expensive and the scene has depth complexity; the output is
+ * unchanged (both passes run the same program, so depths match exactly).
+ * Draws with alpha cutoff > 0 are rendered as before (they may discard).
+ * Disabled by default.
+ */
+R3DAPI void R3D_SetDepthPrepass(bool enabled);
+
+/**
  * @brief Sets the default texture filtering mode.
  * 
  * This function defines the default texture filter that will be applied to all subsequently 
--- a/shaders/include/user/scene.frag
+++ b/shaders/include/user/scene.frag
@@ -111,7 +111,12 @@
     //  -> -1.0 : Inverted test for hybrid blended
     //  ->  0.0 : Disables alpha test
 
-#define ALPHA_TEST(alpha) if (cutoffSign * (alpha - alphaCutoff) < 0.0) discard;
+#if defined(R3D_NO_ALPHA_TEST)
+    // Monolith patch: variants for draws that never alpha test contain no `discard`
+#   define ALPHA_TEST(alpha)
+#else
+#   define ALPHA_TEST(alpha) if (cutoffSign * (alpha - alphaCutoff) < 0.0) discard;
+#endif
 
     /* --- Fill input variables --- */
 
--- a/shaders/scene/geometry.frag
+++ b/shaders/scene/geometry.frag
@@ -54,6 +54,15 @@
 uniform float uMetalness;
 uniform float uSpecular;
 
+#if defined(GEOMETRY_PREPASS)
+// Monolith patch (depth pre-pass): this variant is used both for the depth-only
+// pre-pass (uDepthOnly, colour writes masked) and for the shading pass after it,
+// so gl_Position, and thus the depth tested with GL_EQUAL, comes from the same
+// program. It has no alpha test (R3D_NO_ALPHA_TEST): without any `discard` the
+// driver can keep early depth/stencil tests while the shading pass writes stencil.
+uniform bool uDepthOnly;
+#endif
+
 // ================================
 // User Override
 // ================================
@@ -66,6 +75,10 @@
 
 void main()
 {
+#if defined(GEOMETRY_PREPASS)
+    if (uDepthOnly) return;
+#endif
+
     SceneFragment(vTexCoord, vTBN, uAlphaCutoff, 1.0);
 
     mat3 TBN = mat3(TANGENT, BITANGENT, NORMAL);
--- a/src/modules/r3d_driver.c
+++ b/src/modules/r3d_driver.c
@@ -99,7 +99,8 @@
     pipeline_entry_t depthFactor;
     pipeline_entry_t stencilFunc;
     pipeline_entry_t stencilRef;
-    pipeline_entry_t stencilMask;
+    pipeline_entry_t stencilFuncMask;   //< glStencilFunc's compare mask (monolith patch: was shared with the write mask)
+    pipeline_entry_t stencilMask;       //< glStencilMask's write mask
     pipeline_entry_t stencilOpFail;
     pipeline_entry_t stencilOpZFail;
     pipeline_entry_t stencilOpZPass;
@@ -341,10 +342,10 @@
 
 void r3d_driver_set_stencil_func(GLenum func, uint8_t ref, uint8_t mask)
 {
-    if (CHECK_PIPE(stencilFunc, enum, func) || CHECK_PIPE(stencilRef, byte, ref) || CHECK_PIPE(stencilMask, byte, mask))
+    if (CHECK_PIPE(stencilFunc, enum, func) || CHECK_PIPE(stencilRef, byte, ref) || CHECK_PIPE(stencilFuncMask, byte, mask))
     {
         SET_PIPE(stencilFunc, enum, func);
-        SET_PIPE(stencilMask, byte, mask);
+        SET_PIPE(stencilFuncMask, byte, mask);
         SET_PIPE(stencilRef, byte, ref);
         glStencilFunc(func, ref, mask);
     }
--- a/src/modules/r3d_shader.c
+++ b/src/modules/r3d_shader.c
@@ -839,10 +839,12 @@
     return true;
 }
 
-bool r3d_shader_load_scene_geometry(r3d_shader_custom_t* custom)
+// Monolith patch: `prepass` builds the depth pre-pass variant (see geometry.frag).
+// Its vertex shader is the same source with the same defines as the base variant.
+static bool load_scene_geometry(r3d_shader_custom_t* custom, bool prepass)
 {
     const char* VS_DEFINES[] = {"STAGE_VERT", "GEOMETRY"};
-    const char* FS_DEFINES[] = {"STAGE_FRAG", "GEOMETRY"};
+    const char* FS_DEFINES[] = {"STAGE_FRAG", "GEOMETRY", "GEOMETRY_PREPASS", "R3D_NO_ALPHA_TEST"};
 
     const char* userCode = custom ? custom->program->userCode : NULL;
 
@@ -852,11 +854,13 @@
         .vsDefineCount = R3D_ARRAY_SIZE(VS_DEFINES),
         .fsTemplate    = GEOMETRY_FRAG,
         .fsDefines     = FS_DEFINES,
-        .fsDefineCount = R3D_ARRAY_SIZE(FS_DEFINES),
+        .fsDefineCount = prepass ? R3D_ARRAY_SIZE(FS_DEFINES) : 2,
         .userCode      = userCode,
     };
 
-    DECL_SHADER_SELECT(r3d_shader_scene_geometry_t, scene, geometry, custom);
+    r3d_shader_scene_geometry_t* geometry = (custom == NULL)
+        ? (prepass ? &R3D_MOD_SHADER.scene.geometryPrepass : &R3D_MOD_SHADER.scene.geometry)
+        : (prepass ? &custom->program->scene.geometryPrepass : &custom->program->scene.geometry);
     LOAD_SHADER_EX(geometry, desc);
 
     SET_UNIFORM_BUFFER(geometry, FrameBlock, R3D_SHADER_BLOCK_SLOT_FRAME);
@@ -883,6 +887,7 @@
     GET_LOCATION(geometry, uRoughness);
     GET_LOCATION(geometry, uMetalness);
     GET_LOCATION(geometry, uSpecular);
+    GET_LOCATION(geometry, uDepthOnly);
 
     USE_SHADER(geometry);
 
@@ -900,6 +905,16 @@
     return true;
 }
 
+bool r3d_shader_load_scene_geometry(r3d_shader_custom_t* custom)
+{
+    return load_scene_geometry(custom, false);
+}
+
+bool r3d_shader_load_scene_geometry_prepass(r3d_shader_custom_t* custom)
+{
+    return load_scene_geometry(custom, true);
+}
+
 bool r3d_shader_load_scene_forward(r3d_shader_custom_t* custom)
 {
     char defNumIlluminationProbes[32] = {0};
@@ -1890,6 +1905,7 @@
     UNLOAD_SHADER(prepare.cubemapProceduralSky);
 
     UNLOAD_SHADER(scene.geometry);
+    UNLOAD_SHADER(scene.geometryPrepass);
     UNLOAD_SHADER(scene.forward);
     UNLOAD_SHADER(scene.unlit);
     UNLOAD_SHADER(scene.background);
@@ -2028,6 +2044,7 @@
     {
         DELETE_PROGRAM(custom->program->prepare.cubemapCustomSky.id);
         DELETE_PROGRAM(custom->program->scene.geometry.id);
+        DELETE_PROGRAM(custom->program->scene.geometryPrepass.id);
         DELETE_PROGRAM(custom->program->scene.forward.id);
         DELETE_PROGRAM(custom->program->scene.unlit.id);
         DELETE_PROGRAM(custom->program->scene.depth.id);
--- a/src/modules/r3d_shader.h
+++ b/src/modules/r3d_shader.h
@@ -844,6 +844,7 @@
     r3d_shader_uniform_float_t uRoughness;
     r3d_shader_uniform_float_t uMetalness;
     r3d_shader_uniform_float_t uSpecular;
+    r3d_shader_uniform_int_t uDepthOnly;
 } r3d_shader_scene_geometry_t;
 
 typedef struct {
@@ -1191,6 +1192,7 @@
         // Must follow the same naming pattern as `r3d_mod_shader`
         struct {
             r3d_shader_scene_geometry_t geometry;
+            r3d_shader_scene_geometry_t geometryPrepass;
             r3d_shader_scene_forward_t forward;
             r3d_shader_scene_unlit_t unlit;
             r3d_shader_scene_depth_t depth;
@@ -1266,6 +1268,7 @@
     // Scene shaders
     struct {
         r3d_shader_scene_geometry_t geometry;
+        r3d_shader_scene_geometry_t geometryPrepass;
         r3d_shader_scene_forward_t forward;
         r3d_shader_scene_unlit_t unlit;
         r3d_shader_scene_background_t background;
@@ -1350,6 +1353,7 @@
 bool r3d_shader_load_prepare_cubemap_procedural_sky(r3d_shader_custom_t* custom);
 bool r3d_shader_load_prepare_cubemap_custom_sky(r3d_shader_custom_t* custom);
 bool r3d_shader_load_scene_geometry(r3d_shader_custom_t* custom);
+bool r3d_shader_load_scene_geometry_prepass(r3d_shader_custom_t* custom);
 bool r3d_shader_load_scene_forward(r3d_shader_custom_t* custom);
 bool r3d_shader_load_scene_unlit(r3d_shader_custom_t* custom);
 bool r3d_shader_load_scene_background(r3d_shader_custom_t* custom);
@@ -1423,6 +1427,7 @@
     // Scene shaders
     struct {
         r3d_shader_loader_func geometry;
+        r3d_shader_loader_func geometryPrepass;
         r3d_shader_loader_func forward;
         r3d_shader_loader_func unlit;
         r3d_shader_loader_func background;
@@ -1506,6 +1511,7 @@
 
     .scene = {
         .geometry = r3d_shader_load_scene_geometry,
+        .geometryPrepass = r3d_shader_load_scene_geometry_prepass,
         .forward = r3d_shader_load_scene_forward,
         .unlit = r3d_shader_load_scene_unlit,
         .background = r3d_shader_load_scene_background,
--- a/src/r3d_core.c
+++ b/src/r3d_core.c
@@ -311,6 +311,16 @@
     R3D.outputMode = mode;
 }
 
+bool R3D_GetDepthPrepass(void)
+{
+    return R3D.depthPrepass;
+}
+
+void R3D_SetDepthPrepass(bool enabled)
+{
+    R3D.depthPrepass = enabled;
+}
+
 void R3D_SetTextureFilter(TextureFilter filter)
 {
     R3D.textureFilter = filter;
--- a/src/r3d_core_state.h
+++ b/src/r3d_core_state.h
@@ -68,6 +68,7 @@
     R3D_UpscaleMode upscaleMode;        //< Upscaling mode used during the final blit
     R3D_DownscaleMode downscaleMode;    //< Downscaling mode used during the final blit
     R3D_OutputMode outputMode;          //< Defines which buffer we should output in R3D_End()
+    bool depthPrepass;                  //< Depth-only pre-pass before the geometry pass (monolith patch)
     TextureFilter textureFilter;        //< Default texture filter for model loading
     TextureWrap textureWrap;            //< Default texture wrap for material map loading
     Matrix matCubeViews[6];             //< Pre-computed view matrices for cubemap faces
--- a/src/r3d_draw.c
+++ b/src/r3d_draw.c
@@ -218,7 +218,15 @@
 static void raster_depth_cube(const r3d_render_call_t* call, const Matrix* viewProj, const r3d_light_shadow_job_t* shadowJob);
 static void raster_probe_forward(const r3d_render_call_t* call, const r3d_env_probe_job_t* job, int face, bool opaque);
 static void raster_probe_unlit(const r3d_render_call_t* call, const r3d_env_probe_job_t* job, int face, bool opaque);
-static void raster_geometry(const r3d_render_call_t* call);
+// How a geometry-pass draw is rasterized (monolith patch: depth pre-pass)
+typedef enum {
+    GEOMETRY_RASTER_FULL,       //< Material depth state, full shading (upstream behaviour)
+    GEOMETRY_RASTER_DEPTH_ONLY, //< Pre-pass: depth only, the fragment shader returns at once
+    GEOMETRY_RASTER_EQUAL,      //< After the pre-pass: shade only the fragment that won the depth test
+} geometry_raster_mode_t;
+
+static bool geometry_call_prepassable(const r3d_render_call_t* call);
+static void raster_geometry(const r3d_render_call_t* call, geometry_raster_mode_t mode);
 static void raster_decal(const r3d_render_call_t* call);
 static void raster_forward(const r3d_render_call_t* call);
 static void raster_unlit(const r3d_render_call_t* call, bool opaque);
@@ -1452,7 +1460,27 @@
     }
 }
 
-void raster_geometry(const r3d_render_call_t* call)
+// Stencil bit that marks "this pixel has been shaded" in the GL_EQUAL pass, cleared after it
+#define GEOMETRY_PREPASS_STENCIL_BIT 0x80
+
+bool geometry_call_prepassable(const r3d_render_call_t* call)
+{
+    // A pre-passed draw must produce exactly the depth the shading pass will produce:
+    // no discard (alpha test off) and an ordinary "nearest wins" depth test. The stencil
+    // state must leave the top bit free for the tie-break (see raster_geometry()) and
+    // must not depend on depth-failing fragments, which the EQUAL pass doesn't replay.
+    const R3D_Material* material = &call->mesh.material;
+    R3D_CompareMode depthMode = material->depth.mode;
+    R3D_StencilState stencil = material->stencil;
+    return material->alphaCutoff <= 0.0f
+        && (depthMode == R3D_COMPARE_LESS || depthMode == R3D_COMPARE_LEQUAL)
+        && stencil.mode == R3D_COMPARE_ALWAYS
+        && stencil.opZFail == R3D_STENCIL_KEEP
+        && (stencil.opPass == R3D_STENCIL_KEEP || stencil.opPass == R3D_STENCIL_REPLACE)
+        && (stencil.ref & GEOMETRY_PREPASS_STENCIL_BIT) == 0;
+}
+
+void raster_geometry(const r3d_render_call_t* call, geometry_raster_mode_t mode)
 {
     R3D_ASSERT(call->type == R3D_RENDER_CALL_MESH); //< Paranoid assert, should be fine
 
@@ -1461,58 +1489,70 @@
 
     /* --- Use shader --- */
 
+    // Pre-passed draws use the pre-pass variant in both passes (same vertex transform,
+    // no alpha test); everything else uses the regular geometry program.
     R3D_SurfaceShader* shader = call->mesh.material.shader;
-    R3D_SHADER_USE_SELECT(scene.geometry, shader);
+    r3d_shader_scene_geometry_t* program = NULL;
+    if (mode == GEOMETRY_RASTER_FULL)
+    {
+        R3D_SHADER_USE_SELECT(scene.geometry, shader);
+        program = R3D_SHADER_SELECT(scene.geometry, shader);
+    }
+    else
+    {
+        R3D_SHADER_USE_SELECT(scene.geometryPrepass, shader);
+        program = R3D_SHADER_SELECT(scene.geometryPrepass, shader);
+    }
 
     /* --- Send matrices --- */
 
     Matrix matNormal = r3d_matrix_normal(&group->transform);
 
-    R3D_SHADER_SET_MAT4_SELECT(scene.geometry, shader, uMatModel, group->transform);
-    R3D_SHADER_SET_MAT4_SELECT(scene.geometry, shader, uMatNormal, matNormal);
+    R3D_SHADER__SET_MAT4_IMPL(program, uMatModel, group->transform);
+    R3D_SHADER__SET_MAT4_IMPL(program, uMatNormal, matNormal);
 
     /* --- Send skinning related data --- */
 
     if (group->skinTexture > 0)
     {
-        R3D_SHADER_BIND_SAMPLER_SELECT(scene.geometry, shader, uBoneMatricesTex, group->skinTexture);
-        R3D_SHADER_SET_INT_SELECT(scene.geometry, shader, uSkinning, true);
+        R3D_SHADER__BIND_SAMPLER_IMPL(program, uBoneMatricesTex, group->skinTexture);
+        R3D_SHADER__SET_INT_IMPL(program, uSkinning, true);
     }
     else
     {
-        R3D_SHADER_SET_INT_SELECT(scene.geometry, shader, uSkinning, false);
+        R3D_SHADER__SET_INT_IMPL(program, uSkinning, false);
     }
 
     /* --- Send billboard related data --- */
 
-    R3D_SHADER_SET_INT_SELECT(scene.geometry, shader, uBillboard, material->billboardMode);
+    R3D_SHADER__SET_INT_IMPL(program, uBillboard, material->billboardMode);
 
     /* --- Set factor material maps --- */
 
-    R3D_SHADER_SET_FLOAT_SELECT(scene.geometry, shader, uEmissionEnergy, material->emission.energy);
-    R3D_SHADER_SET_FLOAT_SELECT(scene.geometry, shader, uAlphaCutoff, material->alphaCutoff);
-    R3D_SHADER_SET_FLOAT_SELECT(scene.geometry, shader, uNormalScale, material->normal.scale);
-    R3D_SHADER_SET_FLOAT_SELECT(scene.geometry, shader, uOcclusion, Clamp(material->orm.occlusion, 0.0f, 1.0f));
-    R3D_SHADER_SET_FLOAT_SELECT(scene.geometry, shader, uRoughness, Clamp(material->orm.roughness, 0.0f, 1.0f));
-    R3D_SHADER_SET_FLOAT_SELECT(scene.geometry, shader, uMetalness, Clamp(material->orm.metalness, 0.0f, 1.0f));
-    R3D_SHADER_SET_FLOAT_SELECT(scene.geometry, shader, uSpecular, Clamp(material->orm.specular, 0.0f, 1.0f));
+    R3D_SHADER__SET_FLOAT_IMPL(program, uEmissionEnergy, material->emission.energy);
+    R3D_SHADER__SET_FLOAT_IMPL(program, uAlphaCutoff, material->alphaCutoff);
+    R3D_SHADER__SET_FLOAT_IMPL(program, uNormalScale, material->normal.scale);
+    R3D_SHADER__SET_FLOAT_IMPL(program, uOcclusion, Clamp(material->orm.occlusion, 0.0f, 1.0f));
+    R3D_SHADER__SET_FLOAT_IMPL(program, uRoughness, Clamp(material->orm.roughness, 0.0f, 1.0f));
+    R3D_SHADER__SET_FLOAT_IMPL(program, uMetalness, Clamp(material->orm.metalness, 0.0f, 1.0f));
+    R3D_SHADER__SET_FLOAT_IMPL(program, uSpecular, Clamp(material->orm.specular, 0.0f, 1.0f));
 
     /* --- Set texcoord offset/scale --- */
 
-    R3D_SHADER_SET_VEC2_SELECT(scene.geometry, shader, uTexCoordOffset, material->uvOffset);
-    R3D_SHADER_SET_VEC2_SELECT(scene.geometry, shader, uTexCoordScale, material->uvScale);
+    R3D_SHADER__SET_VEC2_IMPL(program, uTexCoordOffset, material->uvOffset);
+    R3D_SHADER__SET_VEC2_IMPL(program, uTexCoordScale, material->uvScale);
 
     /* --- Set color material maps --- */
 
-    R3D_SHADER_SET_COL4_SELECT(scene.geometry, shader, uAlbedoColor, material->albedo.color);
-    R3D_SHADER_SET_COL3_SELECT(scene.geometry, shader, uEmissionColor, material->emission.color);
+    R3D_SHADER__SET_COL4_IMPL(program, uAlbedoColor, material->albedo.color);
+    R3D_SHADER__SET_COL3_IMPL(program, uEmissionColor, material->emission.color);
 
     /* --- Bind active texture maps --- */
 
-    R3D_SHADER_BIND_SAMPLER_SELECT(scene.geometry, shader, uAlbedoMap, R3D_TEXTURE_SELECT(material->albedo.texture.id, WHITE));
-    R3D_SHADER_BIND_SAMPLER_SELECT(scene.geometry, shader, uNormalMap, R3D_TEXTURE_SELECT(material->normal.texture.id, NORMAL));
-    R3D_SHADER_BIND_SAMPLER_SELECT(scene.geometry, shader, uEmissionMap, R3D_TEXTURE_SELECT(material->emission.texture.id, WHITE));
-    R3D_SHADER_BIND_SAMPLER_SELECT(scene.geometry, shader, uOrmMap, R3D_TEXTURE_SELECT(material->orm.texture.id, WHITE));
+    R3D_SHADER__BIND_SAMPLER_IMPL(program, uAlbedoMap, R3D_TEXTURE_SELECT(material->albedo.texture.id, WHITE));
+    R3D_SHADER__BIND_SAMPLER_IMPL(program, uNormalMap, R3D_TEXTURE_SELECT(material->normal.texture.id, NORMAL));
+    R3D_SHADER__BIND_SAMPLER_IMPL(program, uEmissionMap, R3D_TEXTURE_SELECT(material->emission.texture.id, WHITE));
+    R3D_SHADER__BIND_SAMPLER_IMPL(program, uOrmMap, R3D_TEXTURE_SELECT(material->orm.texture.id, WHITE));
 
     /* --- Applying material parameters that are independent of shaders --- */
 
@@ -1520,16 +1560,49 @@
     r3d_driver_set_depth_state(material->depth);
     r3d_driver_set_stencil_state(material->stencil);
 
+    /* --- Depth pre-pass handling (monolith patch) --- */
+
+    if (mode != GEOMETRY_RASTER_FULL)
+    {
+        R3D_SHADER__SET_INT_IMPL(program, uDepthOnly, mode == GEOMETRY_RASTER_DEPTH_ONLY);
+    }
+
+    if (mode == GEOMETRY_RASTER_DEPTH_ONLY)
+    {
+        // Stencil is written by the shading pass, where only the visible fragment passes
+        r3d_driver_set_stencil_mask(0x00);
+        r3d_driver_set_depth_mask(GL_TRUE);
+    }
+    else if (mode == GEOMETRY_RASTER_EQUAL)
+    {
+        // Coplanar faces can produce bit-identical depths. Without the pre-pass the
+        // first one drawn wins (GL_LESS); with GL_EQUAL all of them would pass and the
+        // last one would win. So the first fragment that passes sets the top stencil bit
+        // and later ones at the same pixel fail the stencil test: same winner as before.
+        uint8_t ref = material->stencil.ref | GEOMETRY_PREPASS_STENCIL_BIT;
+        bool replace = (material->stencil.opPass == R3D_STENCIL_REPLACE);
+        r3d_driver_set_stencil_func(GL_NOTEQUAL, ref, GEOMETRY_PREPASS_STENCIL_BIT);
+        r3d_driver_set_stencil_op(GL_KEEP, GL_KEEP, GL_REPLACE);
+        r3d_driver_set_stencil_mask(replace ? 0xFF : GEOMETRY_PREPASS_STENCIL_BIT);
+        r3d_driver_set_depth_func(GL_EQUAL);
+        r3d_driver_set_depth_mask(GL_FALSE);
+    }
+    else
+    {
+        r3d_driver_set_stencil_mask(0xFF);
+        r3d_driver_set_depth_mask(GL_TRUE);
+    }
+
     /* --- Rendering the object corresponding to the draw call --- */
 
     if (r3d_render_has_instances(group))
     {
-        R3D_SHADER_SET_INT_SELECT(scene.geometry, shader, uInstancing, true);
+        R3D_SHADER__SET_INT_IMPL(program, uInstancing, true);
         r3d_render_draw_instanced(call);
     }
     else
     {
-        R3D_SHADER_SET_INT_SELECT(scene.geometry, shader, uInstancing, false);
+        R3D_SHADER__SET_INT_IMPL(program, uInstancing, false);
         r3d_render_draw(call);
     }
 }
@@ -1999,12 +2072,40 @@
     R3D_TARGET_BIND_LOAD(0, true, R3D_TARGET_GBUFFER);
 
     #define COND (IS_MESH_VISIBLE_CAMERA(call->mesh.instance) && (!call->mesh.material.unlit))
+
+    if (R3D.depthPrepass)
+    {
+        // Monolith patch: depth pre-pass. With one big instanced draw there is no
+        // front-to-back order, so the (expensive) surface shader would otherwise run
+        // for every overdrawn fragment. Lay down the depth first, then shade with
+        // GL_EQUAL so only the visible fragment of each pixel runs the shader.
+        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
+        R3D_RENDER_FOR_EACH(call, COND && geometry_call_prepassable(call), &R3D.viewState.frustum, R3D_RENDER_LIST_OPAQUE_INST, R3D_RENDER_LIST_OPAQUE)
+        {
+            raster_geometry(call, GEOMETRY_RASTER_DEPTH_ONLY);
+        }
+        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
+    }
+
+    bool anyPrepassed = false;
     R3D_RENDER_FOR_EACH(call, COND, &R3D.viewState.frustum, R3D_RENDER_LIST_OPAQUE_INST, R3D_RENDER_LIST_OPAQUE)
     {
-        raster_geometry(call);
+        bool prepassed = R3D.depthPrepass && geometry_call_prepassable(call);
+        raster_geometry(call, prepassed ? GEOMETRY_RASTER_EQUAL : GEOMETRY_RASTER_FULL);
+        anyPrepassed |= prepassed;
     }
     #undef COND
 
+    if (anyPrepassed)
+    {
+        // Clear the tie-break bit again (the clear only touches the bits in the write mask)
+        const GLint zero = 0;
+        r3d_driver_set_stencil_mask(GEOMETRY_PREPASS_STENCIL_BIT);
+        glClearBufferiv(GL_STENCIL, 0, &zero);
+    }
+
+    r3d_driver_set_depth_mask(GL_TRUE);
+    r3d_driver_set_stencil_mask(0xFF);
     r3d_driver_set_depth_offset(0.0f, 0.0f);
     r3d_driver_set_depth_range(0.0f, 1.0f);
 }
```

## Patch 7: caller-fitted (static) directional shadow maps (2026-10-10)

> Patches 7–9 were developed in parallel with patch 6 (depth pre-pass) on a tree with patches 1–5, where they were numbered 6–8; "patch 1–5 build" below means that tree. After merging, the full build's 26 shots equal that tree's exactly (0 pixels differ), so patch 6 doesn't interact with them.

**Goal / symptom.** Sun and geometry are static, so the sun's shadow depth is a pure
function of the map, yet R3D only offers a directional shadow map fitted to the camera
frustum (`light_dir_view_proj`: a sphere 0.5 · `range` ahead of the camera). Its coverage
moves with every step and every turn, so the app had to re-render it (8192²: ~1.2 ms +
0.37 ms min/max per re-render) whenever its cached copy stopped covering the view, and
check that on the CPU every frame (`SunShadowNeedsUpdate()`: ~0.4 ms CPU per frame,
measured below; §7.3 had estimated 0.1 ms).

**Cause.** No way to hand R3D a light-space matrix: the fit is hard-wired in `light_dir_push()`.

**Change.**
- `R3D_PushLightFixedShadow(light, map, lightViewProj, updateShadow)` (public, `r3d_draw.h`):
  like `R3D_PushLightEx()`, but when the map is (re)rendered it uses `lightViewProj`
  instead of the camera fit. Internally `r3d_light_push()` gets an optional
  `const Matrix* fixedViewProj` (NULL = upstream behaviour). The cached matrix is still the
  one the layer was rendered with, so a reused map is always sampled consistently.
- Directional edge fade band 0.05 → 0.01 (`SHADOW_DIR_EDGE_FADE` in `wrap/light.glsl`, also
  used by `VFog_SampleShadowDir`). A world-fixed map loses less of its area to the fade.
  Wherever shadows are weighted (within `range` of the eye) both the camera-fitted map and
  monolith's tiles keep every point far inside the map (patch 4's analysis: ≥ 0.12 of the
  map from every border for the fitted map), so the fade is exactly 1 there and the image
  doesn't change: `--shadow-camera` shots (the old path with this patch) are bit-identical
  to the pre-patch reference (0 pixels differ in all 26 shots).

App side (`src/shadow.c`, `SunShadowTileSelect()`): world-fixed tiles on a light-space grid,
each with exactly the radius, depth window (2·R + 100 m) and texel lattice of R3D's fit for a
70°, 16:9 view (8192 px / 100 m: 3.69 cm texels), chosen by **eye position only**. A tile is
used while everything within `range` of the eye stays inside its fade-free area, so the
shadow equals the camera-fitted one (minus 16-bit depth rounding). Two tiles are cached in
R3D's two directional layers (already allocated, no extra VRAM); the default map's play area
(all 7 viewpoints and verification shots) falls into two cells, so there each tile is drawn
once per map load and never again. Elsewhere a tile is drawn when the eye enters a new cell
(cells ~90 m across the sun direction, ~190 m along it). Tiles key on `world.version`.
**If R3D's fit or fades change, update `FitRadius()` / `TileReach()` / `SHADOW_EDGE_FADE`
in `src/shadow.c`** (and `SunShadowNeedsUpdate()` for `--shadow-camera`).

| File | Change |
|---|---|
| `include/r3d/r3d_draw.h` | `R3D_PushLightFixedShadow()` |
| `src/r3d_draw.c` | its implementation; `r3d_light_push(..., NULL)` in the two existing push functions |
| `src/modules/r3d_light.h/.c` | `fixedViewProj` parameter through `r3d_light_push()` → `light_dir_push()` |
| `shaders/include/wrap/light.glsl` | `SHADOW_DIR_EDGE_FADE` (0.01) in `L_SampleShadowDir` |
| `shaders/deferred/vfog_radiance.frag` | `SHADOW_DIR_EDGE_FADE` in `VFog_SampleShadowDir`, comment |

**Verified.**
- `glslangValidator`: `lighting.frag`, `vfog_radiance.frag` pass (includes resolved with
  `scripts/glsl_processor.py`; `scene/forward.frag` needs R3D's runtime-injected defines and
  surface code, so it can't be checked standalone). `./build.sh`, `./build.sh --win`: no new
  warnings (only the pre-existing raylib/assimp ones in `external/`); wine shot = Linux shot.
- Image vs the patch 1–5 reference (26 shots, 1600×900): 326 pixels differ by > 2/255 in
  total (max 15, mean |Δ| 0.001), 19 285 by ≥ 1: 16-bit depth rounding at shadow edges (the
  depth window starts elsewhere). Crops: no visible change. 8 shots far outside the play
  area (other tiles: near megastructures, on the bridge, 180 m up): ≤ 1 pixel > 2/255 each
  vs `--shadow-camera`. At 1920×1006 the camera-fitted map had 3.88 cm texels (its radius
  grows with the aspect), the tiles 3.69 cm: slightly sharper (Hall 8 710 px > 2/255, max 31,
  in dithered fog/penumbra noise).
- Hot reload (`--reload-test`, a map edited with a high slab over the hall and a canyon
  slit closed): the shot after the reload equals a fresh `--map` shot of the edited map
  bit for bit (0 pixels differ, views 1/2 and an upward view, also with `--shadow-always`).
- Bench (headless 1920×1006, interleaved, 3 pairs of full runs each), vs patch 1–5 build:
  spinning 4.806 → 4.477 ms (−0.33), still 5.164 → 4.992 ms (−0.17); 0 shadow re-renders in
  timed frames. The gain is CPU: "CPU until finish" 0.86 → 0.39 ms (no per-frame coverage
  check); GPU passes read +3–6% across the board (also geometry, which shadows don't
  touch; the same binary with `--shadow-camera` shows it too), most likely the laptop GPU
  clocking lower when it idles less between frames. Note: the patch 1–5 build already
  re-rendered almost never while the bench spins (1 / 4200 frames; patch 4 made the reuse
  check lenient), so the spinning bench no longer shows re-render spikes either way; what
  the tiles remove are the re-renders while *moving* (§7.3: one per 2.5–4.5 m sideways
  and per ~65 m forward, 1.6 ms each) and the per-frame CPU check.

**Upstream.** Not upstream. A general "caller-provided light matrix" hook would be useful
upstream; cascaded shadow maps remain the real long-term feature.

The `r3d_draw.h` declaration and `r3d_draw.c` implementation of `R3D_PushLightFixedShadow()` sit in the same hunks as patch 8's API and are shown in its diff.

```diff
diff --git a/shaders/deferred/vfog_radiance.frag b/shaders/deferred/vfog_radiance.frag
--- a/shaders/deferred/vfog_radiance.frag
+++ b/shaders/deferred/vfog_radiance.frag
@@ -73,7 +81,8 @@ float PhaseFunction_Schlick(vec3 w0, vec3 w1)
 // fadeDist replaces L_SampleShadowDir's view depth (local patch 4): the distance from
 // the eye to the surface behind the march, so the fade doesn't change when the camera
 // only turns. Within that distance (< range) every step on screen lies well inside the
-// camera-fitted map, so edgeFade is 1 there and doesn't depend on the camera either.
+// camera-fitted map, and inside monolith's static tiles (patch 7) as well, so edgeFade
+// is 1 there and doesn't depend on the camera either.
 float VFog_SampleShadowDir(vec3 Pws, float fadeDist, mat2 diskRot)
 {
     vec4 Pls = uLight.viewProj * vec4(Pws, 1.0);
diff --git a/shaders/include/wrap/light.glsl b/shaders/include/wrap/light.glsl
--- a/shaders/include/wrap/light.glsl
+++ b/shaders/include/wrap/light.glsl
@@ -32,6 +32,13 @@ vec3 L_Specular(vec3 F0, float roughness, float NoV, float NoL, float NoH, float
 
 #define SHADOW_SAMPLES 8
 
+// Directional shadows fade out over this outer fraction of the map (x, y and depth).
+// Upstream: 0.05. Local patch (see patches.md): 0.01, so a world-fixed map (monolith's
+// static sun tiles) loses less of its area to the fade. Wherever shadows are weighted,
+// both the camera-fitted and the tiled maps keep every point far inside the map, so
+// the fade is exactly 1 there either way and the image doesn't change.
+#define SHADOW_DIR_EDGE_FADE 0.01
+
 const vec2 VOGEL_DISK[8] = vec2[8](
     vec2(0.250000, 0.000000),
     vec2(-0.319290, 0.292496),
@@ -66,7 +73,7 @@ float L_SampleShadowDir(Light light, vec3 Pws, float Zvs, float NoL, mat2 diskRo
     shadow /= float(SHADOW_SAMPLES);
 
     vec3 distToBorder = min(projCoords, 1.0 - projCoords);
-    float edgeFade = smoothstep(0.0, 0.05, min(distToBorder.x, min(distToBorder.y, distToBorder.z)));
+    float edgeFade = smoothstep(0.0, SHADOW_DIR_EDGE_FADE, min(distToBorder.x, min(distToBorder.y, distToBorder.z)));
     float distFade = smoothstep(light.range, light.range * 0.75, Zvs);
 
     return mix(1.0, shadow, edgeFade * distFade * light.shadowOpacity);
diff --git a/src/modules/r3d_light.c b/src/modules/r3d_light.c
--- a/src/modules/r3d_light.c
+++ b/src/modules/r3d_light.c
@@ -391,7 +392,8 @@ static bool light_volume_screen_ndc(const r3d_light_volume_t* volume, Vector2* m
     return true;
 }
 
-static void light_dir_push(const R3D_Light* light, const R3D_ShadowMap* map, R3D_Camera camera, double aspect, bool updateShadow)
+static void light_dir_push(const R3D_Light* light, const R3D_ShadowMap* map, R3D_Camera camera, double aspect, bool updateShadow,
+                           const Matrix* fixedViewProj)
 {
     r3d_light_data_t data = {
         .volume      = {.center = {0}, .radius = FLT_MAX},
@@ -414,7 +416,8 @@ static void light_dir_push(const R3D_Light* light, const R3D_ShadowMap* map, R3D
 
         if (!cache->rendered || updateShadow)
         {
-            cache->viewProj = light_dir_view_proj(data.direction, data.range, camera, aspect);
+            // Caller-fitted map (local patch, see patches.md): render with the given matrix
+            cache->viewProj = fixedViewProj ? *fixedViewProj : light_dir_view_proj(data.direction, data.range, camera, aspect);
             cache->rendered = true; // Assume it will be rendered in advance
 
             r3d_light_shadow_job_t job = {
@@ -670,7 +677,7 @@ void r3d_light_push(const R3D_Light* light, const R3D_ShadowMap* map, bool updat
     switch (light->type)
     {
     case R3D_LIGHT_DIR:
-        light_dir_push(light, map, R3D.viewState.camera, R3D.viewState.aspect, updateShadow);
+        light_dir_push(light, map, R3D.viewState.camera, R3D.viewState.aspect, updateShadow, fixedViewProj);
         break;
 
     case R3D_LIGHT_SPOT:
diff --git a/src/modules/r3d_light.h b/src/modules/r3d_light.h
--- a/src/modules/r3d_light.h
+++ b/src/modules/r3d_light.h
@@ -117,8 +140,9 @@ bool r3d_light_init(void);
 /* Deinitialize module (called once during R3D_Close) */
 void r3d_light_quit(void);
 
-/**/
-void r3d_light_push(const R3D_Light* light, const R3D_ShadowMap* map, bool updateShadow);
+/* fixedViewProj: directional lights only, caller-fitted shadow matrix (local patch, see
+ * patches.md), NULL = R3D's camera-fitted map */
+void r3d_light_push(const R3D_Light* light, const R3D_ShadowMap* map, bool updateShadow, const Matrix* fixedViewProj);
 
 /**/
 r3d_light_data_t* r3d_light_get(int lightIndex);
```

## Patch 8: baked sun visibility for the volumetric fog (2026-10-10)

**Goal.** With a static sun and static geometry, the shadow visibility the fog march
samples (patches 2–4: up to 120 steps per half-res pixel, each a min/max tile test and, near
edges, 8 PCF taps) is a fixed function of the world position. Bake it once per map load.

**Change.**
- `R3D_BakeFogSunVisibility(map, lightViewProj, region, voxelSize, subsamples)`, called
  between `R3D_Begin()` and `R3D_End()` after the frame's draws: queues a shadow job that
  renders `map`'s layer with `lightViewProj` (must cover `region` in light space; the
  layer keeps that matrix), and after the shadow / min/max passes `pass_scene_fog_sunvol()`
  fills a `GL_R8` 3D texture over `region` (one draw per z slice, `prepare/fog_sunvol.frag`):
  each voxel = mean of `subsamples³` hardware-PCF taps spread evenly through the voxel (a
  box prefilter: slits thinner than a voxel get dimmer instead of aliasing). The bake is
  timed with `glFinish` and logged; `R3D_GetFogSunVisibilityBakeTime()`,
  `R3D_ClearFogSunVisibility()`.
- `vfog_radiance.frag`: `VFog_SunVisibility()` takes one trilinear fetch inside the volume
  (inset by half a voxel) and applies the same `distFade` (by `fragDist`) and opacity; outside
  it falls back to `VFog_SampleShadowDir()` (PCF + patch 3). Applies to directional lights.
- New sampler slot `R3D_SHADER_SAMPLER_FOG_SUNVOL` (18, a free unit).

App side (`src/main.c`, **off by default: `--fog-cache <voxel> <sub>`**, it changes the
image slightly): bakes once per `world.version` into the shadow layer that frame doesn't
sample (that tile slot is then forgotten and redrawn when needed), region
`FOG_CACHE_REGION` = x −70..75, y −1..100, z −150..105 (the default map's play area),
light matrix from `SunShadowBoxMatrix()`.

| File | Change |
|---|---|
| `include/r3d/r3d_draw.h` | `R3D_BakeFogSunVisibility`, `R3D_ClearFogSunVisibility`, `R3D_GetFogSunVisibilityBakeTime` |
| `src/r3d_draw.c` | API, `pass_scene_fog_sunvol()` after the min/max pass, volume uniforms for the fog pass |
| `src/modules/r3d_light.h/.c` | volume state, request (queues the shadow job), texture/FBO allocation, slice binding, freed on quit |
| `src/modules/r3d_shader.h/.c` | `prepare.fogSunVol` program, sampler slot 18, `vfogRadiance` volume uniforms |
| `shaders/prepare/fog_sunvol.frag` | new: bake one slice |
| `shaders/deferred/vfog_radiance.frag` | `VFog_SunVisibility()` and its uniforms |
| `CMakeLists.txt` | embed `fog_sunvol.frag` |

**Verified.** `glslangValidator`: `fog_sunvol.frag`, `vfog_radiance.frag` pass. Linux and
MinGW builds without new warnings; wine shot = Linux shot (0 px differ).

| Voxel / sub | VRAM | Bake (RTX 4050) | 26 shots vs reference: px > 2 / > 8 / max | `vfog radiance` (spin, A → B, on top of patch 9) |
|---|---:|---:|---|---|
| 0.5 m / 2³ | 28.5 MB (290×202×510) | 17–34 ms (+ one shadow render) | 194 398 / 6 254 / 20 | 0.437 → 0.229 ms |
| 0.5 m / 4³ | 28.5 MB | — | 203 095 / 6 459 / 21 | — |
| 0.25 m / 2³ | 228 MB (580×404×1020) | 61 ms | 29 087 / 245 / 16 | (before patch 9: 0.823 → 0.644, vs 0.813 → 0.618 at 0.5 m) |

Differences sit at the edges of the hall's shafts (softer by ~0.5 m) and in the fog's
dither pattern; canyon slit close-ups ≤ 9/255. Crops in the G3 report. Hot reload with
`--fog-cache 0.5 2`: after `--reload-test` the image equals a fresh `--map` shot of the
edited map bit for bit (volume rebaked: 17 ms).

**Upstream.** Not upstream (static-scene specific). A natural extension: cache the integrated
in-scatter too, or bake in light space (2D) per cascade.

```diff
diff --git a/CMakeLists.txt b/CMakeLists.txt
--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -320,6 +320,7 @@ embed_shaders(${PROJECT_NAME}
     "${R3D_ROOT_PATH}/shaders/prepare/exposure_adapt.frag"
     "${R3D_ROOT_PATH}/shaders/prepare/pyramid.frag"
     "${R3D_ROOT_PATH}/shaders/prepare/shadow_minmax.frag"
+    "${R3D_ROOT_PATH}/shaders/prepare/fog_sunvol.frag"
     "${R3D_ROOT_PATH}/shaders/prepare/ssao.frag"
     "${R3D_ROOT_PATH}/shaders/prepare/ssil.frag"
     "${R3D_ROOT_PATH}/shaders/prepare/ssgi.frag"
diff --git a/include/r3d/r3d_draw.h b/include/r3d/r3d_draw.h
--- a/include/r3d/r3d_draw.h
+++ b/include/r3d/r3d_draw.h
@@ -154,6 +154,48 @@ R3DAPI void R3D_PushLight(R3D_Light light);
  */
 R3DAPI void R3D_PushLightEx(R3D_Light light, R3D_ShadowMap map, bool updateShadow);
 
+/**
+ * @brief Like R3D_PushLightEx() for a directional light, but the caller fits the shadow map.
+ *
+ * Local monolith patch (see patches.md). R3D normally fits a directional shadow map to
+ * the camera frustum, so its coverage moves whenever the camera moves or turns. Here
+ * the map is rendered with lightViewProj instead (an orthographic light-space matrix,
+ * e.g. a world-fixed tile), and sampled with the matrix it was last rendered with, so
+ * a static scene can render it once and only sample it afterwards.
+ *
+ * The map is rendered when updateShadow is true or when it has never been rendered;
+ * otherwise lightViewProj is ignored. Shadows fade by distance from the eye over
+ * 75..100% of light.range, as usual, and over the outer 1% of the map.
+ * For other light types this is R3D_PushLightEx() (lightViewProj ignored).
+ */
+R3DAPI void R3D_PushLightFixedShadow(R3D_Light light, R3D_ShadowMap map, Matrix lightViewProj, bool updateShadow);
+
+/**
+ * @brief Bakes the sun visibility the volumetric fog samples into a 3D texture.
+ *
+ * Local monolith patch (see patches.md). For a static directional light over static
+ * geometry, the fog march's shadow lookups are a fixed function of the world position.
+ * Call this between R3D_Begin() and R3D_End(), after the frame's draws (they are the
+ * shadow casters): during R3D_End() 'map' is rendered with lightViewProj (which must
+ * cover 'region' in light space), then every voxel of 'region' stores the mean shadow
+ * visibility over subsamples^3 points inside it (GL_R8, trilinear). From then on the
+ * fog's march takes one fetch per step for directional lights inside the region and
+ * falls back to the shadow map outside it. Bias and softness: the map's depthBias;
+ * the PCF softness is replaced by the voxel box filter.
+ *
+ * The baked layer keeps lightViewProj as its matrix (re-render it before using it as a
+ * normal shadow map again). The volume stays until the next bake or
+ * R3D_ClearFogSunVisibility(); re-bake whenever the geometry or the light changes.
+ * VRAM: one byte per voxel.
+ */
+R3DAPI void R3D_BakeFogSunVisibility(R3D_ShadowMap map, Matrix lightViewProj, BoundingBox region, float voxelSize, int subsamples);
+
+/** @brief Drops the baked fog sun visibility: the fog samples the shadow map again everywhere. */
+R3DAPI void R3D_ClearFogSunVisibility(void);
+
+/** @brief GPU time of the last fog sun-visibility bake in ms (0 = none yet), measured with glFinish. */
+R3DAPI float R3D_GetFogSunVisibilityBakeTime(void);
+
 /**
  * @brief Queues a probe to be rendered for the current frame.
  *
diff --git a/shaders/deferred/vfog_radiance.frag b/shaders/deferred/vfog_radiance.frag
--- a/shaders/deferred/vfog_radiance.frag
+++ b/shaders/deferred/vfog_radiance.frag
@@ -46,6 +46,14 @@ uniform samplerCubeArrayShadow uShadowOmniTex;
 
 uniform sampler2DArray uShadowDirMinMaxTex;     // RG = min/max depth per 16x16 texels (local patch)
 
+// Baked sun visibility of directional lights (local patch, see patches.md): R = mean
+// shadow visibility per voxel over the box [uFogSunVolMin, uFogSunVolMin + extent]
+uniform sampler3D uFogSunVolTex;
+uniform bool uFogSunVolEnabled;
+uniform vec3 uFogSunVolMin;
+uniform vec3 uFogSunVolInvExtent;   // 1 / extent
+uniform vec3 uFogSunVolHalfTexel;   // half a voxel in texture coordinates
+
 // ================================
 // Helper Includes
 // ================================
@@ -121,12 +130,27 @@ float VFog_SampleShadowDir(vec3 Pws, float fadeDist, mat2 diskRot)
     }
 
     vec3 distToBorder = min(projCoords, 1.0 - projCoords);
-    float edgeFade = smoothstep(0.0, 0.05, min(distToBorder.x, min(distToBorder.y, distToBorder.z)));
+    float edgeFade = smoothstep(0.0, SHADOW_DIR_EDGE_FADE, min(distToBorder.x, min(distToBorder.y, distToBorder.z)));
     float distFade = smoothstep(uLight.range, uLight.range * 0.75, fadeDist);
 
     return mix(1.0, shadow, edgeFade * distFade * uLight.shadowOpacity);
 }
 
+// Directional shadow visibility at Pws: one trilinear fetch of the baked volume inside
+// it (local patch, see patches.md), else VFog_SampleShadowDir. Same fades as there:
+// distFade by fadeDist; edgeFade is 1 wherever the shadow is weighted (see above).
+float VFog_SunVisibility(vec3 Pws, float fadeDist, mat2 diskRot)
+{
+    vec3 uvw = (Pws - uFogSunVolMin) * uFogSunVolInvExtent;
+    if (uFogSunVolEnabled && all(greaterThanEqual(uvw, uFogSunVolHalfTexel)) && all(lessThanEqual(uvw, 1.0 - uFogSunVolHalfTexel)))
+    {
+        float shadow = texture(uFogSunVolTex, uvw).r;
+        float distFade = smoothstep(uLight.range, uLight.range * 0.75, fadeDist);
+        return mix(1.0, shadow, distFade * uLight.shadowOpacity);
+    }
+    return VFog_SampleShadowDir(Pws, fadeDist, diskRot);
+}
+
 // Computes the march range [tEnter, tExit] along rayDir (from rayOrigin),
 // clamped to uVFog.maxDistance and to the light's range sphere when applicable.
 // tEnter/tExit are both 0.0 when the ray never enters the light's range
@@ -229,7 +288,7 @@ void main()
         {
             switch (uLight.type)
             {
-            case LIGHT_DIR:  visibility *= VFog_SampleShadowDir(marchPos, fragDist, diskRot); break;
+            case LIGHT_DIR:  visibility *= VFog_SunVisibility(marchPos, fragDist, diskRot); break;
             case LIGHT_SPOT: visibility *= L_SampleShadowSpot(uLight, marchPos, 1.0, diskRot); break;
             case LIGHT_OMNI: visibility *= L_SampleShadowOmni(uLight, marchPos, 1.0, diskRot); break;
             }
diff --git a/shaders/prepare/fog_sunvol.frag b/shaders/prepare/fog_sunvol.frag
new file mode 100644
--- /dev/null
+++ b/shaders/prepare/fog_sunvol.frag
@@ -0,0 +1,42 @@
+/* fog_sunvol.frag -- Bakes one slice of the volumetric fog's sun-visibility volume
+ *
+ * Local monolith patch (see patches.md). For a static directional light and static
+ * geometry the shadow visibility the fog march samples is a fixed function of the world
+ * position, so it is baked once into a 3D texture (one voxel per fragment, one draw per
+ * z slice) and vfog_radiance.frag takes one trilinear fetch per step instead of the
+ * shadow-map PCF. Each voxel stores the mean visibility over uSub^3 points spread
+ * evenly through the voxel (a box prefilter, so slits thinner than a voxel come out
+ * dimmer instead of aliasing), each a hardware 2x2 PCF tap of the shadow map.
+ */
+
+#version 330 core
+
+uniform sampler2DArrayShadow uShadowTex;
+uniform mat4 uLightViewProj;    // the matrix the shadow layer was rendered with
+uniform int uLayer;
+uniform vec3 uVolMin;           // world position of the volume's minimum corner
+uniform vec3 uVoxel;            // voxel size per axis (m)
+uniform int uSlice;             // z index of the slice being written
+uniform int uSub;               // subsamples per axis
+uniform float uBias;            // shadow depth bias (as the fog's lookups use it)
+
+out float FragVis;
+
+void main()
+{
+    vec3 corner = uVolMin + vec3(floor(gl_FragCoord.xy), float(uSlice)) * uVoxel;
+    float invSub = 1.0 / float(uSub);
+    float sum = 0.0;
+
+    for (int z = 0; z < uSub; z++)
+    for (int y = 0; y < uSub; y++)
+    for (int x = 0; x < uSub; x++)
+    {
+        vec3 P = corner + (vec3(x, y, z) + 0.5) * invSub * uVoxel;
+        vec4 Pls = uLightViewProj * vec4(P, 1.0);
+        vec3 proj = Pls.xyz / Pls.w * 0.5 + 0.5;
+        sum += texture(uShadowTex, vec4(proj.xy, float(uLayer), proj.z - uBias));
+    }
+
+    FragVis = sum * invSub * invSub * invSub;
+}
diff --git a/src/modules/r3d_light.c b/src/modules/r3d_light.c
--- a/src/modules/r3d_light.c
+++ b/src/modules/r3d_light.c
@@ -16,6 +16,7 @@
 #include <float.h>
 
 #include "../common/r3d_helper.h"
+#include "./r3d_driver.h"
 #include "../r3d_core_state.h"
 
 // ========================================
@@ -656,11 +659,15 @@ void r3d_light_quit(void)
         shadow_array_destroy(&R3D_MOD_LIGHT.shadowArrays[i]);
     }
 
+    r3d_light_fog_sunvol_t* vol = &R3D_MOD_LIGHT.fogSunVol;
+    if (vol->texture != 0)     glDeleteTextures(1, &vol->texture);
+    if (vol->framebuffer != 0) glDeleteFramebuffers(1, &vol->framebuffer);
+
     R3D_LIST_DESTROY(R3D_MOD_LIGHT.listShadowJobs);
     R3D_LIST_DESTROY(R3D_MOD_LIGHT.listLightData);
 }
 
-void r3d_light_push(const R3D_Light* light, const R3D_ShadowMap* map, bool updateShadow)
+void r3d_light_push(const R3D_Light* light, const R3D_ShadowMap* map, bool updateShadow, const Matrix* fixedViewProj)
 {
     if (map && !light_check_shadow_validity(light, map))
     {
@@ -821,3 +828,112 @@ r3d_light_shadow_cache_t* r3d_light_shadow_cache(R3D_LightType type, int layer)
     R3D_ASSERT(layer >= 0);
     return &R3D_LIST_GET(R3D_MOD_LIGHT.shadowArrays[type].cache, r3d_light_shadow_cache_t, layer);
 }
+
+// ========================================
+// FOG SUN-VISIBILITY VOLUME (local patch, see patches.md)
+// ========================================
+
+bool r3d_light_request_fog_sunvol(const R3D_ShadowMap* map, Matrix viewProj, BoundingBox region, float voxelSize, int subsamples)
+{
+    if (map->type != R3D_LIGHT_DIR || !r3d_light_shadow_layer_is_valid(R3D_LIGHT_DIR, (int)map->handle - 1))
+    {
+        R3D_TRACELOG(LOG_WARNING, "Fog sun volume -> needs a valid directional shadow map");
+        return false;
+    }
+
+    Vector3 size = Vector3Subtract(region.max, region.min);
+    if (voxelSize <= 0.0f || size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f)
+    {
+        R3D_TRACELOG(LOG_WARNING, "Fog sun volume -> invalid region or voxel size");
+        return false;
+    }
+
+    GLint max3D = 0;
+    glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &max3D);
+
+    r3d_light_fog_sunvol_request_t req = {
+        .pending  = true,
+        .layer    = (int)map->handle - 1,
+        .viewProj = viewProj,
+        .min      = region.min,
+        .sub      = R3D_CLAMP(subsamples, 1, 8),
+        .bias     = map->depthBias,
+    };
+    float axis[3] = { size.x, size.y, size.z };
+    for (int i = 0; i < 3; i++)
+    {
+        req.dims[i] = (int)ceilf(axis[i] / voxelSize);
+        if (req.dims[i] < 2 || req.dims[i] > max3D)
+        {
+            R3D_TRACELOG(LOG_WARNING, "Fog sun volume -> %d voxels on an axis (limit %d)", req.dims[i], max3D);
+            return false;
+        }
+    }
+    req.voxel = (Vector3){ size.x / req.dims[0], size.y / req.dims[1], size.z / req.dims[2] };
+
+    // Render the layer with the bake's matrix this frame; it is then sampled consistently
+    // if the caller keeps pushing it without updates
+    r3d_light_shadow_cache_t* cache = r3d_light_shadow_cache(R3D_LIGHT_DIR, req.layer);
+    cache->viewProj = viewProj;
+    cache->rendered = true;
+
+    r3d_light_shadow_job_t job = {
+        .frustum     = R3D_ComputeFrustum(viewProj),
+        .viewProj    = viewProj,
+        .cullMask    = map->cullMask,
+        .type        = R3D_LIGHT_DIR,
+        .shadowLayer = req.layer,
+    };
+    R3D_LIST_PUSH(R3D_MOD_LIGHT.listShadowJobs, job);
+
+    R3D_MOD_LIGHT.fogSunVol.request = req;
+    return true;
+}
+
+void r3d_light_clear_fog_sunvol(void)
+{
+    R3D_MOD_LIGHT.fogSunVol.valid = false;
+    R3D_MOD_LIGHT.fogSunVol.request.pending = false;
+}
+
+bool r3d_light_prepare_fog_sunvol(void)
+{
+    r3d_light_fog_sunvol_t* vol = &R3D_MOD_LIGHT.fogSunVol;
+    const r3d_light_fog_sunvol_request_t* req = &vol->request;
+
+    if (vol->texture == 0 || memcmp(vol->dims, req->dims, sizeof(vol->dims)) != 0)
+    {
+        if (vol->texture == 0) glGenTextures(1, &vol->texture);
+        if (vol->framebuffer == 0) glGenFramebuffers(1, &vol->framebuffer);
+
+        glBindTexture(GL_TEXTURE_3D, vol->texture);
+        r3d_driver_clear_errors();
+        glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, req->dims[0], req->dims[1], req->dims[2], 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
+        if (r3d_driver_check_error("Fog sun volume -> texture allocation"))
+        {
+            glBindTexture(GL_TEXTURE_3D, 0);
+            memset(vol->dims, 0, sizeof(vol->dims));
+            vol->valid = false;
+            return false;
+        }
+        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
+        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
+        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
+        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
+        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
+        glBindTexture(GL_TEXTURE_3D, 0);
+        memcpy(vol->dims, req->dims, sizeof(vol->dims));
+    }
+
+    vol->min = req->min;
+    vol->extent = (Vector3){ req->voxel.x * req->dims[0], req->voxel.y * req->dims[1], req->voxel.z * req->dims[2] };
+    return true;
+}
+
+void r3d_light_bind_fog_sunvol_slice(int slice)
+{
+    r3d_light_fog_sunvol_t* vol = &R3D_MOD_LIGHT.fogSunVol;
+    glBindFramebuffer(GL_FRAMEBUFFER, vol->framebuffer);
+    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, vol->texture, 0, slice);
+    glViewport(0, 0, vol->dims[0], vol->dims[1]);
+}
diff --git a/src/modules/r3d_light.h b/src/modules/r3d_light.h
--- a/src/modules/r3d_light.h
+++ b/src/modules/r3d_light.h
@@ -97,6 +97,28 @@ typedef struct {
     GLuint      minMaxMidTexture;   // GL_TEXTURE_2D, GL_RG32F: 8x8 texel step of the reduction
 } r3d_light_shadow_array_t;
 
+// Sun-visibility volume of the volumetric fog (local patch, see patches.md)
+typedef struct {
+    bool        pending;        // a bake was requested this frame (its shadow job is queued)
+    int         layer;          // directional shadow layer the bake reads
+    Matrix      viewProj;       // matrix that layer is rendered with
+    Vector3     min;            // world-space minimum corner of the volume
+    Vector3     voxel;          // voxel size per axis
+    int         dims[3];        // voxel counts
+    int         sub;            // subsamples per axis
+    float       bias;           // depth bias of the shadow map
+} r3d_light_fog_sunvol_request_t;
+
+typedef struct {
+    GLuint      texture;        // GL_TEXTURE_3D, GL_R8, 0 = none
+    GLuint      framebuffer;
+    bool        valid;          // baked and usable by the fog
+    int         dims[3];
+    Vector3     min;
+    Vector3     extent;         // dims * voxel
+    r3d_light_fog_sunvol_request_t request;
+} r3d_light_fog_sunvol_t;
+
 // ========================================
 // MODULE STATE
 // ========================================
@@ -105,6 +127,7 @@ extern struct r3d_light {
     r3d_light_shadow_array_t shadowArrays[R3D_LIGHT_TYPE_COUNT];
     r3d_list_t* listShadowJobs;
     r3d_list_t* listLightData;
+    r3d_light_fog_sunvol_t fogSunVol;   // local patch, see patches.md
 } R3D_MOD_LIGHT;
 
 // ========================================
@@ -166,6 +190,18 @@ bool r3d_light_shadow_minmax_take_stale(void);
 /**/
 r3d_light_shadow_cache_t* r3d_light_shadow_cache(R3D_LightType type, int layer);
 
+/*
+ * Sun-visibility volume of the volumetric fog (local patch, see patches.md).
+ * request: queues a shadow job rendering 'map' with 'viewProj' this frame and a bake of
+ * 'region' from it after the shadow passes. Returns false on invalid arguments.
+ */
+bool r3d_light_request_fog_sunvol(const R3D_ShadowMap* map, Matrix viewProj, BoundingBox region, float voxelSize, int subsamples);
+void r3d_light_clear_fog_sunvol(void);
+/* (Re)allocates the 3D texture for the pending request; false if it can't be allocated */
+bool r3d_light_prepare_fog_sunvol(void);
+/* Attaches z slice 'slice' of the volume as the colour target, viewport = one slice */
+void r3d_light_bind_fog_sunvol_slice(int slice);
+
 // ========================================
 // INLINE QUERIES
 // ========================================
diff --git a/src/modules/r3d_shader.c b/src/modules/r3d_shader.c
--- a/src/modules/r3d_shader.c
+++ b/src/modules/r3d_shader.c
@@ -28,6 +28,7 @@
 #include <shaders/blur_up.frag.h>
 #include <shaders/pyramid.frag.h>
 #include <shaders/shadow_minmax.frag.h>
+#include <shaders/fog_sunvol.frag.h>
 #include <shaders/ssao.frag.h>
 #include <shaders/ssil.frag.h>
 #include <shaders/ssgi.frag.h>
@@ -409,6 +410,27 @@ bool r3d_shader_load_prepare_shadow_minmax(r3d_shader_custom_t* custom)
     return true;
 }
 
+bool r3d_shader_load_prepare_fog_sunvol(r3d_shader_custom_t* custom)
+{
+    R3D_UNUSED(custom);
+
+    DECL_SHADER(r3d_shader_prepare_fog_sunvol_t, prepare, fogSunVol);
+    LOAD_SHADER(fogSunVol, SCREEN_VERT, FOG_SUNVOL_FRAG);
+
+    GET_LOCATION(fogSunVol, uLightViewProj);
+    GET_LOCATION(fogSunVol, uLayer);
+    GET_LOCATION(fogSunVol, uVolMin);
+    GET_LOCATION(fogSunVol, uVoxel);
+    GET_LOCATION(fogSunVol, uSlice);
+    GET_LOCATION(fogSunVol, uSub);
+    GET_LOCATION(fogSunVol, uBias);
+
+    USE_SHADER(fogSunVol);
+    SET_SAMPLER(fogSunVol, uShadowTex, R3D_SHADER_SAMPLER_SHADOW_DIR);
+
+    return true;
+}
+
 bool r3d_shader_load_prepare_downsample(r3d_shader_custom_t* custom)
 {
     R3D_UNUSED(custom);
@@ -1506,6 +1528,12 @@ bool r3d_shader_load_deferred_vfog_radiance(r3d_shader_custom_t* custom)
     SET_SAMPLER(vfogRadiance, uShadowSpotTex, R3D_SHADER_SAMPLER_SHADOW_SPOT);
     SET_SAMPLER(vfogRadiance, uShadowOmniTex, R3D_SHADER_SAMPLER_SHADOW_OMNI);
     SET_SAMPLER(vfogRadiance, uShadowDirMinMaxTex, R3D_SHADER_SAMPLER_SHADOW_DIR_MINMAX);
+    SET_SAMPLER(vfogRadiance, uFogSunVolTex, R3D_SHADER_SAMPLER_FOG_SUNVOL);
+
+    GET_LOCATION(vfogRadiance, uFogSunVolEnabled);
+    GET_LOCATION(vfogRadiance, uFogSunVolMin);
+    GET_LOCATION(vfogRadiance, uFogSunVolInvExtent);
+    GET_LOCATION(vfogRadiance, uFogSunVolHalfTexel);
 
     return true;
 }
@@ -1872,6 +1900,7 @@ void r3d_shader_quit(void)
     UNLOAD_SHADER(prepare.blurUp);
     UNLOAD_SHADER(prepare.pyramid);
     UNLOAD_SHADER(prepare.shadowMinMax);
+    UNLOAD_SHADER(prepare.fogSunVol);
     UNLOAD_SHADER(prepare.downsample);
     UNLOAD_SHADER(prepare.ssao);
     UNLOAD_SHADER(prepare.ssil);
diff --git a/src/modules/r3d_shader.h b/src/modules/r3d_shader.h
--- a/src/modules/r3d_shader.h
+++ b/src/modules/r3d_shader.h
@@ -299,6 +299,9 @@ typedef enum {
     // Min/max depth tiles of the directional shadow map (local patch, see patches.md)
     R3D_SHADER_SAMPLER_SHADOW_DIR_MINMAX    = 17,
 
+    // Sun-visibility volume of the volumetric fog (local patch, see patches.md)
+    R3D_SHADER_SAMPLER_FOG_SUNVOL           = 18,
+
     // Buffers
     R3D_SHADER_SAMPLER_BUFFER_SCENE         = 20,
     R3D_SHADER_SAMPLER_BUFFER_ALBEDO        = 21,
@@ -358,6 +361,7 @@ static const GLenum R3D_MOD_SHADER_SAMPLER_TYPES[R3D_SHADER_SAMPLER_COUNT] =
     [R3D_SHADER_SAMPLER_IBL_BRDF_LUT]           = GL_TEXTURE_2D,
     [R3D_SHADER_SAMPLER_BONE_MATRICES]          = GL_TEXTURE_1D,
     [R3D_SHADER_SAMPLER_SHADOW_DIR_MINMAX]      = GL_TEXTURE_2D_ARRAY,
+    [R3D_SHADER_SAMPLER_FOG_SUNVOL]             = GL_TEXTURE_3D,
     [R3D_SHADER_SAMPLER_BUFFER_SCENE]           = GL_TEXTURE_2D,
     [R3D_SHADER_SAMPLER_BUFFER_ALBEDO]          = GL_TEXTURE_2D,
     [R3D_SHADER_SAMPLER_BUFFER_NORMAL]          = GL_TEXTURE_2D,
@@ -679,6 +683,18 @@ typedef struct {
     r3d_shader_uniform_int_t uPass;
 } r3d_shader_prepare_shadow_minmax_t;
 
+typedef struct {
+    GLuint id;
+    r3d_shader_uniform_sampler_t uShadowTex;
+    r3d_shader_uniform_mat4_t uLightViewProj;
+    r3d_shader_uniform_int_t uLayer;
+    r3d_shader_uniform_vec3_t uVolMin;
+    r3d_shader_uniform_vec3_t uVoxel;
+    r3d_shader_uniform_int_t uSlice;
+    r3d_shader_uniform_int_t uSub;
+    r3d_shader_uniform_float_t uBias;
+} r3d_shader_prepare_fog_sunvol_t;
+
 typedef struct {
     GLuint id;
     r3d_shader_uniform_sampler_t uSelectorTex;
@@ -1079,6 +1095,11 @@ typedef struct {
     r3d_shader_uniform_sampler_t uShadowSpotTex;
     r3d_shader_uniform_sampler_t uShadowOmniTex;
     r3d_shader_uniform_sampler_t uShadowDirMinMaxTex;
+    r3d_shader_uniform_sampler_t uFogSunVolTex;
+    r3d_shader_uniform_int_t uFogSunVolEnabled;
+    r3d_shader_uniform_vec3_t uFogSunVolMin;
+    r3d_shader_uniform_vec3_t uFogSunVolInvExtent;
+    r3d_shader_uniform_vec3_t uFogSunVolHalfTexel;
 } r3d_shader_deferred_vfog_radiance_t;
 
 typedef struct {
@@ -1243,6 +1264,7 @@ extern struct r3d_mod_shader {
         r3d_shader_prepare_blur_up_t blurUp;
         r3d_shader_prepare_pyramid_t pyramid;
         r3d_shader_prepare_shadow_minmax_t shadowMinMax;
+        r3d_shader_prepare_fog_sunvol_t fogSunVol;
         r3d_shader_prepare_downsample_t downsample;
         r3d_shader_prepare_ssao_t ssao;
         r3d_shader_prepare_ssil_t ssil;
@@ -1324,6 +1346,7 @@ bool r3d_shader_load_prepare_blur_down(r3d_shader_custom_t* custom);
 bool r3d_shader_load_prepare_blur_up(r3d_shader_custom_t* custom);
 bool r3d_shader_load_prepare_pyramid(r3d_shader_custom_t* custom);
 bool r3d_shader_load_prepare_shadow_minmax(r3d_shader_custom_t* custom);
+bool r3d_shader_load_prepare_fog_sunvol(r3d_shader_custom_t* custom);
 bool r3d_shader_load_prepare_downsample(r3d_shader_custom_t* custom);
 bool r3d_shader_load_prepare_ssao(r3d_shader_custom_t* custom);
 bool r3d_shader_load_prepare_ssil(r3d_shader_custom_t* custom);
@@ -1398,6 +1421,7 @@ static const struct r3d_shader_loader {
         r3d_shader_loader_func blurUp;
         r3d_shader_loader_func pyramid;
         r3d_shader_loader_func shadowMinMax;
+        r3d_shader_loader_func fogSunVol;
         r3d_shader_loader_func downsample;
         r3d_shader_loader_func ssao;
         r3d_shader_loader_func ssaoBlur;
@@ -1477,6 +1501,7 @@ static const struct r3d_shader_loader {
         .blurUp = r3d_shader_load_prepare_blur_up,
         .pyramid = r3d_shader_load_prepare_pyramid,
         .shadowMinMax = r3d_shader_load_prepare_shadow_minmax,
+        .fogSunVol = r3d_shader_load_prepare_fog_sunvol,
         .downsample = r3d_shader_load_prepare_downsample,
         .ssao = r3d_shader_load_prepare_ssao,
         .ssil = r3d_shader_load_prepare_ssil,
diff --git a/src/r3d_draw.c b/src/r3d_draw.c
--- a/src/r3d_draw.c
+++ b/src/r3d_draw.c
@@ -225,6 +225,7 @@ static void raster_unlit(const r3d_render_call_t* call, bool opaque);
 
 static void pass_scene_shadows(void);
 static void pass_scene_shadow_minmax(void);
+static void pass_scene_fog_sunvol(void);
 static void pass_scene_probes(void);
 static void pass_scene_geometry(void);
 static void pass_scene_decals(void);
@@ -316,6 +317,11 @@ void R3D_End(void)
 
     PROF_PASS(R3D_PROFILER_SHADOW_MINMAX, pass_scene_shadow_minmax());
 
+    if (R3D_MOD_LIGHT.fogSunVol.request.pending)
+    {
+        pass_scene_fog_sunvol();
+    }
+
     r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_SHADOW_DIR, r3d_light_shadow_map(R3D_LIGHT_DIR));
     r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_SHADOW_SPOT, r3d_light_shadow_map(R3D_LIGHT_SPOT));
     r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_SHADOW_OMNI, r3d_light_shadow_map(R3D_LIGHT_OMNI));
@@ -503,12 +509,34 @@ void R3D_EndCluster(void)
 
 void R3D_PushLight(R3D_Light light)
 {
-    r3d_light_push(&light, NULL, false);
+    r3d_light_push(&light, NULL, false, NULL);
 }
 
 void R3D_PushLightEx(R3D_Light light, R3D_ShadowMap map, bool updateShadow)
 {
-    r3d_light_push(&light, &map, updateShadow);
+    r3d_light_push(&light, &map, updateShadow, NULL);
+}
+
+void R3D_PushLightFixedShadow(R3D_Light light, R3D_ShadowMap map, Matrix lightViewProj, bool updateShadow)
+{
+    r3d_light_push(&light, &map, updateShadow, &lightViewProj);
+}
+
+static float fogSunVolBakeMs;   //< local patch, see pass_scene_fog_sunvol()
+
+void R3D_BakeFogSunVisibility(R3D_ShadowMap map, Matrix lightViewProj, BoundingBox region, float voxelSize, int subsamples)
+{
+    r3d_light_request_fog_sunvol(&map, lightViewProj, region, voxelSize, subsamples);
+}
+
+void R3D_ClearFogSunVisibility(void)
+{
+    r3d_light_clear_fog_sunvol();
+}
+
+float R3D_GetFogSunVisibilityBakeTime(void)
+{
+    return fogSunVolBakeMs;
 }
 
 void R3D_PushProbe(R3D_Probe probe, bool updateProbe)
@@ -1871,6 +1899,52 @@ void pass_scene_shadow_minmax(void)
     r3d_target_invalidate_cache(); //< We bound our own framebuffer
 }
 
+// Local patch (see patches.md): bakes the fog's sun-visibility volume from the shadow
+// layer the request rendered this frame (pass_scene_shadows), one draw per z slice.
+// Runs once per bake (map load), so it may block: glFinish around it for the bake time.
+void pass_scene_fog_sunvol(void)
+{
+    r3d_light_fog_sunvol_t* vol = &R3D_MOD_LIGHT.fogSunVol;
+    r3d_light_fog_sunvol_request_t req = vol->request;
+    vol->request.pending = false;
+
+    glFinish();
+    double t0 = GetTime();
+
+    if (!r3d_light_prepare_fog_sunvol()) return;
+
+    r3d_driver_disable(GL_STENCIL_TEST);
+    r3d_driver_disable(GL_DEPTH_TEST);
+    r3d_driver_disable(GL_CULL_FACE);
+    r3d_driver_disable(GL_BLEND);
+    r3d_driver_disable(GL_SCISSOR_TEST);
+
+    R3D_SHADER_USE(prepare.fogSunVol);
+    r3d_shader_bind_sampler(R3D_SHADER_SAMPLER_SHADOW_DIR, r3d_light_shadow_map(R3D_LIGHT_DIR));
+    R3D_SHADER_SET_MAT4(prepare.fogSunVol, uLightViewProj, req.viewProj);
+    R3D_SHADER_SET_INT(prepare.fogSunVol, uLayer, req.layer);
+    R3D_SHADER_SET_VEC3(prepare.fogSunVol, uVolMin, req.min);
+    R3D_SHADER_SET_VEC3(prepare.fogSunVol, uVoxel, req.voxel);
+    R3D_SHADER_SET_INT(prepare.fogSunVol, uSub, req.sub);
+    R3D_SHADER_SET_FLOAT(prepare.fogSunVol, uBias, req.bias);
+
+    for (int z = 0; z < req.dims[2]; z++)
+    {
+        r3d_light_bind_fog_sunvol_slice(z);
+        R3D_SHADER_SET_INT(prepare.fogSunVol, uSlice, z);
+        R3D_RENDER_SCREEN();
+    }
+
+    r3d_target_invalidate_cache(); //< We bound our own framebuffer
+    vol->valid = true;
+
+    glFinish();
+    fogSunVolBakeMs = (float)((GetTime() - t0) * 1000.0);
+    R3D_TRACELOG(LOG_INFO, "Fog sun volume baked: %dx%dx%d voxels (%.2f MB), %.1f ms",
+                 req.dims[0], req.dims[1], req.dims[2],
+                 (double)req.dims[0] * req.dims[1] * req.dims[2] / (1024.0 * 1024.0), fogSunVolBakeMs);
+}
+
 void pass_scene_probes(void)
 {
     #define RASTER_PROBE(opaque)                                    \
@@ -2500,6 +2574,19 @@ void pass_deferred_volumetric_fog(r3d_target_t sceneTarget)
     R3D_SHADER_BIND_SAMPLER(deferred.vfogRadiance, uDepthTex, r3d_target_get_level(R3D_TARGET_DEPTH, 1));
     R3D_SHADER_BIND_SAMPLER(deferred.vfogRadiance, uShadowDirMinMaxTex, r3d_light_shadow_minmax_map());
 
+    // Baked sun visibility (local patch, see pass_scene_fog_sunvol)
+    const r3d_light_fog_sunvol_t* sunVol = &R3D_MOD_LIGHT.fogSunVol;
+    R3D_SHADER_SET_INT(deferred.vfogRadiance, uFogSunVolEnabled, sunVol->valid ? 1 : 0);
+    if (sunVol->valid)
+    {
+        R3D_SHADER_BIND_SAMPLER(deferred.vfogRadiance, uFogSunVolTex, sunVol->texture);
+        R3D_SHADER_SET_VEC3(deferred.vfogRadiance, uFogSunVolMin, sunVol->min);
+        R3D_SHADER_SET_VEC3(deferred.vfogRadiance, uFogSunVolInvExtent,
+            (Vector3){ 1.0f / sunVol->extent.x, 1.0f / sunVol->extent.y, 1.0f / sunVol->extent.z });
+        R3D_SHADER_SET_VEC3(deferred.vfogRadiance, uFogSunVolHalfTexel,
+            (Vector3){ 0.5f / sunVol->dims[0], 0.5f / sunVol->dims[1], 0.5f / sunVol->dims[2] });
+    }
+
     R3D_LIGHT_FOR_EACH_VISIBLE(light)
     {
         if (light->fogEnergy == 0.0f) continue;
```

## Patch 9: directional fog march does per-step work that is constant along the ray (2026-10-10)

**Symptom.** After patches 2–4, `vfog radiance` still cost ~0.8 ms per frame (bench average),
also on views where almost no step needs a shadow lookup.

**Cause.** The march loop is written for all light types: every step computes the light
vector (`length`, a division), the Schlick phase function and the in-scatter product, and
branches on the light type, although for a directional light `L` and so the whole factor
`scatteringDensity · scatteringColor · phase · lightColor` are constant along the ray. Rays
with no shadow weighting (sky, surfaces ≥ `range` away: patch 2 / 4's `hasShadow = false`)
add that constant times the running transmittance, a geometric series.

**Fix.** A `LIGHT_DIR` branch before the generic loop: the factor is computed once; the loop
only fetches the visibility (patch 8 / `VFog_SampleShadowDir`), multiplies and accumulates
in the same order as before. Unshadowed rays use the closed form
`LiDir · T(1 − Tⁿ)/(1 − T) · stepSize` with `n = ceil(rayDist / stepSize)` (the loop's step
count up to rounding of the accumulated `l`; `n` steps of 1 without extinction). Other light
types run the unchanged loop.

| File | Change |
|---|---|
| `shaders/deferred/vfog_radiance.frag` | `LIGHT_DIR` fast path in `main()` (+35 lines) |

**Verified.** `glslangValidator` passes. Not bit-identical (the compiler orders the hoisted
products differently): 26 shots vs before: 8 pixels > 2/255 (max 10), the rest ±1–2 on a few
hundred pixels per image; the closed form adds ≤ 1/255 (0 pixels > 2). Bench (interleaved,
3 pairs, spinning, static tiles, no fog cache): hoisting `vfog radiance` 0.844 → 0.517 ms,
frame −0.33 ms; closed form on top 0.508 → 0.433 ms, frame −0.06 ms.

**Upstream.** Worth offering: generic, exact up to rounding, no API change.

```diff
diff --git a/shaders/deferred/vfog_radiance.frag b/shaders/deferred/vfog_radiance.frag
--- a/shaders/deferred/vfog_radiance.frag
+++ b/shaders/deferred/vfog_radiance.frag
@@ -215,6 +239,41 @@ void main()
     float invRange = 1.0 / max(uLight.range, 1e-4);
     float invEpsilon = 1.0 / max(uLight.innerCutOff - uLight.outerCutOff, 1e-4);
 
+    // Directional light (local patch 9, see patches.md): L, and so the phase function and
+    // the whole in-scatter factor, are constant along the ray; only the visibility varies.
+    // Same arithmetic in the same order as the generic loop below, just hoisted.
+    if (uLight.type == LIGHT_DIR)
+    {
+        float phase = PhaseFunction_Schlick(-uLight.direction, fragToCameraNorm);
+        vec3 LiDir = uVFog.scatteringDensity * uVFog.scatteringColor.rgb * phase * lightColor;
+
+        // Unshadowed rays (sky, surfaces >= range away): every step adds LiDir times the
+        // transmittance so far, a geometric series with a closed form. n = the loop's
+        // step count (equal up to float rounding of the accumulated l).
+        if (!hasShadow)
+        {
+            float n = ceil(rayDist / uVFog.stepSize);
+            float series = (transmittanceStep < 0.99999)
+                ? transmittanceStep * (1.0 - pow(transmittanceStep, n)) / (1.0 - transmittanceStep)
+                : n;    // no extinction: n steps of 1
+            radiance = LiDir * series * uVFog.stepSize;
+            rayDist = 0.0;
+        }
+
+        for (float l = 0; l < rayDist; l += uVFog.stepSize)
+        {
+            float visibility = VFog_SunVisibility(marchPos, fragDist, diskRot);
+            vec3 Li = LiDir * visibility;
+
+            transmittance *= transmittanceStep;
+            radiance += Li * transmittance * uVFog.stepSize;
+
+            marchPos += deltaStep;
+        }
+
+        rayDist = 0.0;  // skip the generic loop
+    }
+
     for (float l = 0; l < rayDist; l += uVFog.stepSize)
     {
         // Compute light direction and distance at each march step
```


## Patch 10: post passes that only move pixels around (2026-10-11)

**Symptom.** After the scene is lit, R3D spends ~0.4 ms per frame (1920×1006, bench average)
on passes that compute nothing new: the depth pyramid down to 1×1, a full-resolution bloom
composite pass whose result the tonemap pass reads right back, and a final copy blit after
FXAA. Plus the CPU cost of ~12 extra passes (framebuffer re-attachments, draws).

**Cause.**
1. `pass_prepare_pyramid()` builds every depth mip level, but only SSR reads levels above 1
   (SSAO, SSIL, SSGI, both fogs, DoF and the ambient pass use levels 0 and 1).
2. `pass_post_bloom()` ends with `post/bloom.frag`, which mixes the bloom texture into the
   scene (RGBA16F write + read at full resolution) right before `post/output.frag` reads it.
3. When the internal resolution equals the destination, `blit_to_screen()` is a plain
   `common_copy` of the FXAA result (colour + depth).

**Fix.**
1. Without SSR the pyramid stops at level 1.
2. When nothing runs between bloom and the output pass (no auto exposure, no POST stage
   screen shaders), the bloom chain runs without its composite pass and `output.frag` mixes
   the bloom in (same code). The mixed colour is rounded to a half float the way the RGBA16F
   target stored it (`RoundToHalf`, a bit mask: NVIDIA *truncates* float→half on render
   target writes; measured: with truncation bit-identical, with round-to-nearest-even 5% of
   pixels ±1/255 and FXAA turned a few into up to 12/255). On GPUs that round to nearest the
   difference is one half-float ulp before tonemapping.
3. `final_pass_direct()`: scene output, no FINAL stage screen shaders, viewport at the origin
   and of the internal size → FXAA renders straight into the final target and writes the scene
   depth there (`gl_FragDepth`, as the copy did); `blit_to_screen()` is skipped. FXAA into an
   internal target writes no depth (no depth attachment, depth test off).

| File | Change |
|---|---|
| `src/r3d_draw.c` | `pass_prepare_pyramid(maxLevel)`, `pass_post_bloom(…, apply)`, `pass_post_output(…, bloomFused)`, `final_pass_direct()`, `pass_post_fxaa_to_screen()`, `R3D_End()` wiring |
| `shaders/post/output.frag` | optional bloom composite (`uBloomFused`, `uBloomTex`), `RoundToHalf()` |
| `shaders/post/fxaa.frag` | `uDepthTex`, writes `gl_FragDepth` |
| `src/modules/r3d_shader.[ch]` | the new uniforms / samplers |

**Verified.** Bit-identical: 27 verification shots and the models test map vs. before, 0 pixels
differ at all (`any: 0`). `glslangValidator` passes `output.frag`, `fxaa.frag`. Bench (headless,
1920×1006, spinning, interleaved): depth pyramid 0.156 → 0.062 ms, bloom 0.181 → 0.140 ms,
FXAA + blit 0.152 → 0.100 ms, i.e. −0.19 ms GPU, and ~−0.08 ms CPU per frame.

**Upstream.** 1 and 3 are generic and exact; 2 is exact only where the half rounding matches.

```diff
diff --git a/shaders/post/fxaa.frag b/shaders/post/fxaa.frag
index 1d52160..f81b4c0 100644
--- a/shaders/post/fxaa.frag
+++ b/shaders/post/fxaa.frag
@@ -73,4 +73,5 @@ out vec4 FragColor;
 
 uniform sampler2D uSceneTex;
+uniform sampler2D uDepthTex;    //< monolith patch 10, see main()
 
 // ================================
@@ -110,3 +111,9 @@ void main()
 
     FragColor = vec4(aa.rgb, alpha);
+
+    // Monolith patch 10: when FXAA renders straight into the final target (instead of an
+    // internal scene target followed by a copy blit), it also writes the scene depth there,
+    // as the copy blit did. Into an internal target the depth test is off, so this has no
+    // effect there.
+    gl_FragDepth = texelFetch(uDepthTex, ivec2(gl_FragCoord.xy), 0).r;
 }
diff --git a/shaders/post/output.frag b/shaders/post/output.frag
index def70e1..65885ef 100644
--- a/shaders/post/output.frag
+++ b/shaders/post/output.frag
@@ -37,4 +37,22 @@ out vec4 FragColor;
 
 uniform sampler2D uSceneTex;
+uniform sampler2D uBloomTex;    //< monolith patch 10: bloom is applied here when uBloomFused
+uniform bool uBloomFused;
+
+// ================================
+// Monolith patch 10: 16-bit float rounding
+// ================================
+
+// Rounds to a half float the way storing into the RGBA16F scene target did when bloom was
+// composited in its own pass. NVIDIA truncates (rounds toward zero) on float -> half render
+// target writes; measured: with truncation the fused pass is bit-identical there, with
+// round-to-nearest-even 5% of pixels moved by 1/255 (and FXAA turned some into up to 12/255).
+// Other GPUs may round to nearest instead; the difference is then one half-float ulp before
+// tonemapping. Normal half range only: values below the smallest normal half (6.1e-5) keep
+// their extra bits, far below 1/255 after tonemapping.
+vec3 RoundToHalf(vec3 v)
+{
+    return uintBitsToFloat(floatBitsToUint(v) & uvec3(0xFFFFE000u));
+}
 
 // ================================
@@ -218,4 +236,28 @@ void main()
     vec4 color = texelFetch(uSceneTex, ivec2(gl_FragCoord.xy), 0);
 
+    // Monolith patch 10: the bloom composite (post/bloom.frag, same math) folded into this
+    // pass, which saves a full-resolution write + read of the scene. The mixed colour is
+    // rounded to a half float like the scene target stored it (RoundToHalf).
+    if (uBloomFused)
+    {
+        vec3 bloom = texture(uBloomTex, vTexCoord).rgb;
+
+        if (uBloom.mode == BLOOM_MIX)
+        {
+            color.rgb = mix(color.rgb, bloom, uBloom.intensity);
+        }
+        else if (uBloom.mode == BLOOM_ADDITIVE)
+        {
+            color.rgb += bloom * uBloom.intensity;
+        }
+        else if (uBloom.mode == BLOOM_SCREEN)
+        {
+            bloom = clamp(bloom * uBloom.intensity, vec3(0.0), vec3(1.0));
+            color.rgb = max((color.rgb + bloom) - (color.rgb * bloom), vec3(0.0));
+        }
+
+        color.rgb = RoundToHalf(color.rgb);
+    }
+
     color.rgb = Tonemapping(color.rgb, uTonemap.exposure, uTonemap.white);
     color.rgb = Adjustments(color.rgb, uBcs.brightness, uBcs.contrast, uBcs.saturation);
diff --git a/src/modules/r3d_shader.c b/src/modules/r3d_shader.c
index 0e17e82..44dda52 100644
--- a/src/modules/r3d_shader.c
+++ b/src/modules/r3d_shader.c
@@ -1660,4 +1667,6 @@ bool r3d_shader_load_post_output(r3d_shader_custom_t* custom)
     USE_SHADER(output);
     SET_SAMPLER(output, uSceneTex, R3D_SHADER_SAMPLER_BUFFER_SCENE);
+    SET_SAMPLER(output, uBloomTex, R3D_SHADER_SAMPLER_BUFFER_BLOOM);
+    GET_LOCATION(output, uBloomFused);
 
     return true;
@@ -1687,4 +1696,5 @@ static bool load_post_fxaa(r3d_shader_custom_t* custom, int index)
     USE_SHADER(fxaa);
     SET_SAMPLER(fxaa, uSceneTex, R3D_SHADER_SAMPLER_BUFFER_SCENE);
+    SET_SAMPLER(fxaa, uDepthTex, R3D_SHADER_SAMPLER_BUFFER_DEPTH);
 
     return true;
diff --git a/src/modules/r3d_shader.h b/src/modules/r3d_shader.h
index 0d55871..e17518a 100644
--- a/src/modules/r3d_shader.h
+++ b/src/modules/r3d_shader.h
@@ -1138,4 +1148,6 @@ typedef struct {
     GLuint id;
     r3d_shader_uniform_sampler_t uSceneTex;
+    r3d_shader_uniform_sampler_t uBloomTex;     //< monolith patch 10: bloom applied here
+    r3d_shader_uniform_int_t uBloomFused;
 } r3d_shader_post_output_t;
 
@@ -1143,4 +1155,5 @@ typedef struct {
     GLuint id;
     r3d_shader_uniform_sampler_t uSceneTex;
+    r3d_shader_uniform_sampler_t uDepthTex;     //< monolith patch 10: FXAA writes the final target
 } r3d_shader_post_fxaa_t;
 
diff --git a/src/r3d_draw.c b/src/r3d_draw.c
index beb67f7..7e2266b 100644
--- a/src/r3d_draw.c
+++ b/src/r3d_draw.c
@@ -239,5 +239,5 @@ static void pass_scene_geometry(void);
 static void pass_scene_decals(void);
 
-static void pass_prepare_pyramid(void);
+static void pass_prepare_pyramid(int maxLevel);
 static void pass_prepare_downsample(r3d_target_t target, int level);
 static r3d_target_t pass_prepare_ssao(void);
@@ -257,9 +257,11 @@ static void pass_scene_background(r3d_target_t sceneTarget);
 static r3d_target_t pass_post_setup(r3d_target_t sceneTarget);
 static r3d_target_t pass_post_dof(r3d_target_t sceneTarget);
-static r3d_target_t pass_post_bloom(r3d_target_t sceneTarget);
+static r3d_target_t pass_post_bloom(r3d_target_t sceneTarget, bool apply);
 static r3d_target_t pass_post_auto_exposure(r3d_target_t sceneTarget);
 static r3d_target_t pass_post_screen(R3D_ScreenShaderStage stage, r3d_target_t sceneTarget);
-static r3d_target_t pass_post_output(r3d_target_t sceneTarget);
+static r3d_target_t pass_post_output(r3d_target_t sceneTarget, bool bloomFused);
 static r3d_target_t pass_post_fxaa(r3d_target_t sceneTarget);
+static bool final_pass_direct(void);
+static void pass_post_fxaa_to_screen(r3d_target_t sceneTarget);
 static r3d_target_t pass_post_smaa(r3d_target_t sceneTarget);
 
@@ -393,5 +395,7 @@ void R3D_End(void)
         {
             PROF_BEGIN(R3D_PROFILER_PYRAMID);
-            pass_prepare_pyramid();
+            // Monolith patch 10: only SSR reads depth levels above 1 (SSAO, SSIL, SSGI, the
+            // fog and DoF use levels 0 and 1), so without SSR the pyramid stops at level 1
+            pass_prepare_pyramid(ssr ? r3d_target_get_max_level(R3D_TARGET_DEPTH) : 1);
 
             if (ssao || ssil || ssgi || ssr) pass_prepare_downsample(R3D_TARGET_NORMAL, 1);
@@ -445,7 +449,16 @@ void R3D_End(void)
     }
 
-    if (R3D.environment.bloom.mode != R3D_BLOOM_DISABLED)
+    // Monolith patch 10: the bloom composite is folded into the output (tonemap) pass when
+    // nothing runs between the two (no auto exposure, no POST stage screen shaders)
+    bool bloomOn = R3D.environment.bloom.mode != R3D_BLOOM_DISABLED;
+    bool bloomFused = bloomOn && !R3D.environment.autoExposure.enabled;
+    for (int i = 0; bloomFused && i < (int)R3D_ARRAY_SIZE(R3D.screenShaders[R3D_SCREEN_SHADER_STAGE_POST]); i++)
     {
-        PROF_PASS(R3D_PROFILER_BLOOM, sceneTarget = pass_post_bloom(sceneTarget));
+        if (R3D.screenShaders[R3D_SCREEN_SHADER_STAGE_POST][i] != NULL) bloomFused = false;
+    }
+
+    if (bloomOn)
+    {
+        PROF_PASS(R3D_PROFILER_BLOOM, sceneTarget = pass_post_bloom(sceneTarget, !bloomFused));
     }
 
@@ -456,12 +469,24 @@ void R3D_End(void)
 
     sceneTarget = pass_post_screen(R3D_SCREEN_SHADER_STAGE_POST, sceneTarget);
-    PROF_PASS(R3D_PROFILER_OUTPUT, sceneTarget = pass_post_output(sceneTarget));
+    PROF_PASS(R3D_PROFILER_OUTPUT, sceneTarget = pass_post_output(sceneTarget, bloomFused));
 
     sceneTarget = pass_post_screen(R3D_SCREEN_SHADER_STAGE_OUTPUT, sceneTarget);
 
+    // Monolith patch 10: FXAA renders straight into the final target when the final blit
+    // would only copy its result (same size, scene output, no FINAL stage screen shaders)
+    bool finalDone = false;
+
     switch (R3D.aaMode)
     {
     case R3D_ANTI_ALIASING_MODE_FXAA:
-        PROF_PASS(R3D_PROFILER_AA, sceneTarget = pass_post_fxaa(sceneTarget));
+        if (final_pass_direct())
+        {
+            PROF_PASS(R3D_PROFILER_AA, pass_post_fxaa_to_screen(sceneTarget));
+            finalDone = true;
+        }
+        else
+        {
+            PROF_PASS(R3D_PROFILER_AA, sceneTarget = pass_post_fxaa(sceneTarget));
+        }
         break;
     case R3D_ANTI_ALIASING_MODE_SMAA:
@@ -477,5 +502,5 @@ void R3D_End(void)
     switch (R3D.outputMode)
     {
-    case R3D_OUTPUT_SCENE:    blit_to_screen(r3d_target_swap_scene(sceneTarget)); break;
+    case R3D_OUTPUT_SCENE:    if (!finalDone) blit_to_screen(r3d_target_swap_scene(sceneTarget)); break;
     case R3D_OUTPUT_ALBEDO:   visualize_to_screen(R3D_TARGET_ALBEDO); break;
     case R3D_OUTPUT_NORMAL:   visualize_to_screen(R3D_TARGET_NORMAL); break;
@@ -2210,5 +2285,5 @@ void pass_scene_decals(void)
 }
 
-void pass_prepare_pyramid(void)
+void pass_prepare_pyramid(int maxLevel)
 {
     r3d_driver_disable(GL_STENCIL_TEST);
@@ -2224,5 +2299,5 @@ void pass_prepare_pyramid(void)
     R3D_SHADER_USE(prepare.pyramid);
 
-    int maxLevel = r3d_target_get_max_level(R3D_TARGET_DEPTH);
+    maxLevel = R3D_MIN(maxLevel, r3d_target_get_max_level(R3D_TARGET_DEPTH));
 
     for (int iDst = 1; iDst <= maxLevel; iDst++)
@@ -2879,5 +2965,5 @@ r3d_target_t pass_post_dof(r3d_target_t sceneTarget)
 }
 
-r3d_target_t pass_post_bloom(r3d_target_t sceneTarget)
+r3d_target_t pass_post_bloom(r3d_target_t sceneTarget, bool apply)
 {
     r3d_target_t sceneSource = r3d_target_swap_scene(sceneTarget);
@@ -2940,4 +3026,7 @@ r3d_target_t pass_post_bloom(r3d_target_t sceneTarget)
     r3d_driver_disable(GL_BLEND);
 
+    // Monolith patch 10: without `apply` the output pass composites the bloom (uBloomFused)
+    if (!apply) return sceneTarget;
+
     /* --- Apply bloom to the scene --- */
 
@@ -3040,5 +3129,5 @@ r3d_target_t pass_post_screen(R3D_ScreenShaderStage stage, r3d_target_t sceneTar
 }
 
-r3d_target_t pass_post_output(r3d_target_t sceneTarget)
+r3d_target_t pass_post_output(r3d_target_t sceneTarget, bool bloomFused)
 {
     R3D_TARGET_BIND_AND_SWAP_SCENE(sceneTarget);
@@ -3046,4 +3135,10 @@ r3d_target_t pass_post_output(r3d_target_t sceneTarget)
 
     R3D_SHADER_BIND_SAMPLER(post.output, uSceneTex, r3d_target_get(sceneTarget));
+    R3D_SHADER_SET_INT(post.output, uBloomFused, bloomFused ? 1 : 0);
+    if (bloomFused)
+    {
+        int minLevel = r3d_target_get_min_level(R3D_TARGET_BLOOM);
+        R3D_SHADER_BIND_SAMPLER(post.output, uBloomTex, r3d_target_get_level(R3D_TARGET_BLOOM, minLevel));
+    }
     R3D_RENDER_SCREEN();
 
@@ -3062,4 +3157,56 @@ r3d_target_t pass_post_fxaa(r3d_target_t sceneTarget)
 }
 
+// Monolith patch 10: whether the last post pass may write the final target itself. That is
+// the case when blit_to_screen() would be a plain copy: scene output, internal resolution ==
+// destination viewport size, viewport at the origin (the copy and FXAA address texels by
+// gl_FragCoord), and no FINAL stage screen shaders after the AA pass.
+bool final_pass_direct(void)
+{
+    if (R3D.outputMode != R3D_OUTPUT_SCENE) return false;
+
+    for (int i = 0; i < (int)R3D_ARRAY_SIZE(R3D.screenShaders[R3D_SCREEN_SHADER_STAGE_FINAL]); i++)
+    {
+        if (R3D.screenShaders[R3D_SCREEN_SHADER_STAGE_FINAL][i] != NULL) return false;
+    }
+
+    Rectangle viewport = R3D.viewState.viewport;
+    int dstX = (int)(viewport.x + 0.5f);
+    int dstY = (int)(viewport.y + 0.5f);
+    int dstW = (int)(viewport.width + 0.5f);
+    int dstH = (int)(viewport.height + 0.5f);
+
+    GLuint dstId = R3D.screen.id;
+    int targetH = dstId ? R3D.screen.texture.height : GetRenderHeight();
+    int glDstY = targetH - dstY - dstH;
+
+    int srcW = 0, srcH = 0;
+    r3d_target_get_resolution(&srcW, &srcH, 0);
+
+    return dstX == 0 && glDstY == 0 && dstW == srcW && dstH == srcH;
+}
+
+// FXAA into the final target, with the scene depth like blit_to_screen()'s copy
+void pass_post_fxaa_to_screen(r3d_target_t sceneTarget)
+{
+    r3d_target_t source = r3d_target_swap_scene(sceneTarget);
+
+    int srcW = 0, srcH = 0;
+    r3d_target_get_resolution(&srcW, &srcH, 0);
+
+    glBindFramebuffer(GL_FRAMEBUFFER, R3D.screen.id);
+    glViewport(0, 0, srcW, srcH);
+
+    r3d_driver_enable(GL_DEPTH_TEST);
+    r3d_driver_disable(GL_BLEND);
+
+    r3d_driver_set_depth_func(GL_ALWAYS);
+    r3d_driver_set_depth_mask(GL_TRUE);
+
+    R3D_SHADER_USE(post.fxaa[R3D.aaPreset]);
+    R3D_SHADER_BIND_SAMPLER(post.fxaa[R3D.aaPreset], uSceneTex, r3d_target_get(source));
+    R3D_SHADER_BIND_SAMPLER(post.fxaa[R3D.aaPreset], uDepthTex, r3d_target_get_depth_buffer());
+    R3D_RENDER_SCREEN();
+}
+
 r3d_target_t pass_post_smaa(r3d_target_t sceneTarget)
 {
```

## Patch 11: baked world-space occlusion for the ambient light (2026-10-11)

**Symptom.** SSAO + SSIL cost ~0.75 ms per frame (plus normal/radiance downsamples) for a
small effect in this scene (.claude/skills/lighting-fog-shadows/r3d-pitfalls.md §5), and being screen-space they change when
the camera only turns and miss off-screen occluders. The scene is static, so its occlusion is a
function of the world position.

**Fix (an option, off unless the app sets a volume).** `R3D_SetWorldOcclusion()` uploads a
caller-built 3D texture (GL_R8, trilinear) holding the signed distance from each voxel centre to
the nearest surface; `R3D_ClearWorldOcclusion()` drops it (also in `R3D_Close`). The ambient
pass multiplies the pixel's ambient occlusion by a distance-field AO estimate: 5 fetches along
the normal at 0.15–3.2 m, occlusion = weighted shortfall of the distance against the height,
times an intensity; it fades to 1 at the volume border. It multiplies with SSAO/SSIL if those
stay on. monolith's runtime builds the volume from the boxes on the CPU (`BakeOcclusionVolume`,
default map: 393×208×413 voxels at 0.5 m, 32 MB, ~60 ms per map load) and the game uses it as
`--ao baked` (SSAO + SSIL off, intensity 0.6).

| File | Change |
|---|---|
| `include/r3d/r3d_draw.h` | `R3D_SetWorldOcclusion()`, `R3D_ClearWorldOcclusion()` |
| `src/r3d_draw.c` | volume state + the two functions; uniforms in `pass_deferred_ambient()` |
| `shaders/deferred/ambient.frag` | `WorldOcclusion()`, applied to `orm.x` |
| `src/modules/r3d_shader.[ch]` | sampler slot 19 (`R3D_SHADER_SAMPLER_WORLD_AO`, 3D), uniforms |
| `src/r3d_core.c` | `R3D_Close()` frees the volume |

**Verified.** Without a volume the image is unchanged (27 shots: 0 pixels differ). With
`--ao baked` vs. the default: mean luminance −0.7/255 over 5 views, 11.7% of pixels > 2/255
(darkening along wall feet and in the canyon corners is crisper and also comes from off-screen
geometry; the faint SSIL bounce is gone). Cost of the AO term: ambient pass +0.03 ms; frame
−0.75 ms GPU vs. SSAO + SSIL. `--reload-test` re-bakes and matches a fresh load.
`glslangValidator` passes `ambient.frag`.

**Upstream.** A generic "static AO volume" API; the bake belongs to the app.

```diff
diff --git a/include/r3d/r3d_draw.h b/include/r3d/r3d_draw.h
index f66e24a..b48ede3 100644
--- a/include/r3d/r3d_draw.h
+++ b/include/r3d/r3d_draw.h
@@ -197,4 +197,23 @@ R3DAPI void R3D_ClearFogSunVisibility(void);
 R3DAPI float R3D_GetFogSunVisibilityBakeTime(void);
 
+/**
+ * @brief Sets a baked world-space occlusion volume for the ambient light.
+ *
+ * Local monolith patch 11 (see patches.md). 'distance' is a w x h x d grid (x fastest, then
+ * y, then z) of one byte per voxel: the signed distance from the voxel centre to the nearest
+ * surface (negative inside geometry), decoded as value / 255 * (maxDistance - minDistance) +
+ * minDistance metres. Voxel (i, j, k) is centred at min + (i + 0.5, j + 0.5, k + 0.5) * voxelSize.
+ * The ambient pass then scales the ambient occlusion of every pixel inside the volume by a
+ * distance-field AO estimate (a few trilinear fetches along the normal, 0.15 to 3.2 m out),
+ * times 'intensity' (1 = full); outside it, and near its border, the factor fades to 1.
+ * It works with or without SSAO/SSIL (it multiplies with them). VRAM: one byte per voxel.
+ * The volume stays until the next call or R3D_ClearWorldOcclusion().
+ */
+R3DAPI void R3D_SetWorldOcclusion(const unsigned char *distance, int w, int h, int d, Vector3 min, float voxelSize,
+                                  float minDistance, float maxDistance, float intensity);
+
+/** @brief Drops the world occlusion volume (and frees its texture). */
+R3DAPI void R3D_ClearWorldOcclusion(void);
+
 /**
  * @brief Queues a probe to be rendered for the current frame.
diff --git a/shaders/deferred/ambient.frag b/shaders/deferred/ambient.frag
index 8d44b80..45604c8 100644
--- a/shaders/deferred/ambient.frag
+++ b/shaders/deferred/ambient.frag
@@ -54,4 +54,12 @@ uniform samplerCubeArray uPrefilterTex;
 uniform sampler2D uBrdfLutTex;
 
+// Local patch 11: baked world-space occlusion (R3D_SetWorldOcclusion)
+uniform sampler3D uWorldAoTex;      //< signed distance to the nearest surface, encoded in [0, 1]
+uniform bool uWorldAoEnabled;
+uniform vec3 uWorldAoMin;           //< volume corner (world)
+uniform vec3 uWorldAoInvExtent;     //< 1 / volume size
+uniform vec2 uWorldAoDecode;        //< distance (m) = value * x + y
+uniform float uWorldAoIntensity;
+
 // ================================
 // Helper Includes
@@ -61,4 +69,36 @@ uniform sampler2D uBrdfLutTex;
 #include <wrap/env.glsl>
 
+// ================================
+// World-space occlusion (local patch 11)
+// ================================
+
+// Distance-field ambient occlusion: how far the surface is from other surfaces at a few
+// heights above it along its normal. A point h metres out from a surface with nothing else
+// near is h from every surface; any nearer surface makes it less, and the shortfall is the
+// occlusion. Static (a function of the world position and normal only), so it never
+// changes when the camera turns, and it sees occluders that are off screen.
+float WorldOcclusion(vec3 P, vec3 N)
+{
+    const float H[5] = float[5](0.15, 0.4, 0.9, 1.8, 3.2);
+
+    vec3 uvw = (P - uWorldAoMin) * uWorldAoInvExtent;
+    vec3 edge = min(uvw, 1.0 - uvw);
+    float inside = clamp(min(edge.x, min(edge.y, edge.z)) * 32.0, 0.0, 1.0);
+    if (inside <= 0.0) return 1.0;
+
+    float occ = 0.0, wsum = 0.0, w = 1.0;
+    for (int i = 0; i < 5; i++)
+    {
+        vec3 q = (P + N * H[i] - uWorldAoMin) * uWorldAoInvExtent;
+        float d = texture(uWorldAoTex, q).r * uWorldAoDecode.x + uWorldAoDecode.y;
+        occ += w * clamp((H[i] - d) / H[i], 0.0, 1.0);
+        wsum += w;
+        w *= 0.75;
+    }
+
+    float ao = clamp(1.0 - uWorldAoIntensity * occ / wsum, 0.0, 1.0);
+    return mix(1.0, ao, inside);
+}
+
 // ================================
 // Main Function
@@ -114,4 +154,9 @@ void main()
     }
 
+    if (uWorldAoEnabled)
+    {
+        orm.x *= WorldOcclusion(P, N);
+    }
+
     vec3 diff = vec3(0.0);
     vec3 spec = vec3(0.0);
diff --git a/src/modules/r3d_shader.c b/src/modules/r3d_shader.c
index 0e17e82..44dda52 100644
--- a/src/modules/r3d_shader.c
+++ b/src/modules/r3d_shader.c
@@ -1448,4 +1448,11 @@ bool r3d_shader_load_deferred_ambient(r3d_shader_custom_t* custom)
     SET_SAMPLER(ambient, uBrdfLutTex, R3D_SHADER_SAMPLER_IBL_BRDF_LUT);
 
+    SET_SAMPLER(ambient, uWorldAoTex, R3D_SHADER_SAMPLER_WORLD_AO);
+    GET_LOCATION(ambient, uWorldAoEnabled);
+    GET_LOCATION(ambient, uWorldAoMin);
+    GET_LOCATION(ambient, uWorldAoInvExtent);
+    GET_LOCATION(ambient, uWorldAoDecode);
+    GET_LOCATION(ambient, uWorldAoIntensity);
+
     return true;
 }
diff --git a/src/modules/r3d_shader.h b/src/modules/r3d_shader.h
index 0d55871..e17518a 100644
--- a/src/modules/r3d_shader.h
+++ b/src/modules/r3d_shader.h
@@ -303,4 +303,7 @@ typedef enum {
     R3D_SHADER_SAMPLER_FOG_SUNVOL           = 18,
 
+    // Baked world-space occlusion volume (local patch 11, see patches.md)
+    R3D_SHADER_SAMPLER_WORLD_AO             = 19,
+
     // Buffers
     R3D_SHADER_SAMPLER_BUFFER_SCENE         = 20,
@@ -363,4 +366,5 @@ static const GLenum R3D_MOD_SHADER_SAMPLER_TYPES[R3D_SHADER_SAMPLER_COUNT] =
     [R3D_SHADER_SAMPLER_SHADOW_DIR_MINMAX]      = GL_TEXTURE_2D_ARRAY,
     [R3D_SHADER_SAMPLER_FOG_SUNVOL]             = GL_TEXTURE_3D,
+    [R3D_SHADER_SAMPLER_WORLD_AO]               = GL_TEXTURE_3D,
     [R3D_SHADER_SAMPLER_BUFFER_SCENE]           = GL_TEXTURE_2D,
     [R3D_SHADER_SAMPLER_BUFFER_ALBEDO]          = GL_TEXTURE_2D,
@@ -1057,4 +1061,10 @@ typedef struct {
     r3d_shader_uniform_sampler_t uPrefilterTex;
     r3d_shader_uniform_sampler_t uBrdfLutTex;
+    r3d_shader_uniform_sampler_t uWorldAoTex;       //< local patch 11
+    r3d_shader_uniform_int_t uWorldAoEnabled;
+    r3d_shader_uniform_vec3_t uWorldAoMin;
+    r3d_shader_uniform_vec3_t uWorldAoInvExtent;
+    r3d_shader_uniform_vec2_t uWorldAoDecode;
+    r3d_shader_uniform_float_t uWorldAoIntensity;
 } r3d_shader_deferred_ambient_t;
 
diff --git a/src/r3d_core.c b/src/r3d_core.c
index aa9dd04..5b30ff4 100644
--- a/src/r3d_core.c
+++ b/src/r3d_core.c
@@ -8,4 +8,5 @@
 
 #include <r3d/r3d_core.h>
+#include <r3d/r3d_draw.h>     // R3D_ClearWorldOcclusion (local patch 11)
 #include <raymath.h>
 #include <float.h>
@@ -223,4 +224,5 @@ bool R3D_Init(int resWidth, int resHeight)
 void R3D_Close(void)
 {
+    R3D_ClearWorldOcclusion();  // local patch 11
     r3d_stack_destroy(R3D.stack);
     r3d_texture_quit();
diff --git a/src/r3d_draw.c b/src/r3d_draw.c
index beb67f7..7e2266b 100644
--- a/src/r3d_draw.c
+++ b/src/r3d_draw.c
@@ -533,4 +558,54 @@ void R3D_PushLightFixedShadow(R3D_Light light, R3D_ShadowMap map, Matrix lightVi
 static float fogSunVolBakeMs;   //< local patch, see pass_scene_fog_sunvol()
 
+// Local patch 11: baked world-space occlusion volume, see R3D_SetWorldOcclusion()
+static struct {
+    GLuint texture;
+    Vector3 min, invExtent;
+    Vector2 decode;         //< distance = value * x + y (value in [0, 1])
+    float intensity;
+} worldAo;
+
+void R3D_SetWorldOcclusion(const unsigned char *distance, int w, int h, int d, Vector3 min, float voxelSize,
+                           float minDistance, float maxDistance, float intensity)
+{
+    if (!distance || w <= 0 || h <= 0 || d <= 0 || voxelSize <= 0.0f || maxDistance <= minDistance)
+    {
+        R3D_TRACELOG(LOG_WARNING, "R3D_SetWorldOcclusion: invalid volume");
+        R3D_ClearWorldOcclusion();
+        return;
+    }
+
+    if (worldAo.texture == 0) glGenTextures(1, &worldAo.texture);
+
+    glBindTexture(GL_TEXTURE_3D, worldAo.texture);
+    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
+    r3d_driver_clear_errors();
+    glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, w, h, d, 0, GL_RED, GL_UNSIGNED_BYTE, distance);
+    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
+    if (r3d_driver_check_error("World occlusion -> texture upload"))
+    {
+        glBindTexture(GL_TEXTURE_3D, 0);
+        R3D_ClearWorldOcclusion();
+        return;
+    }
+    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
+    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
+    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
+    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
+    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
+    glBindTexture(GL_TEXTURE_3D, 0);
+
+    worldAo.min = min;
+    worldAo.invExtent = (Vector3){ 1.0f / (voxelSize * w), 1.0f / (voxelSize * h), 1.0f / (voxelSize * d) };
+    worldAo.decode = (Vector2){ maxDistance - minDistance, minDistance };
+    worldAo.intensity = intensity;
+}
+
+void R3D_ClearWorldOcclusion(void)
+{
+    if (worldAo.texture != 0) glDeleteTextures(1, &worldAo.texture);
+    memset(&worldAo, 0, sizeof(worldAo));
+}
+
 void R3D_BakeFogSunVisibility(R3D_ShadowMap map, Matrix lightViewProj, BoundingBox region, float voxelSize, int subsamples)
 {
@@ -2597,4 +2672,15 @@ void pass_deferred_ambient(r3d_target_t ssaoSource, r3d_target_t ssilSource, r3d
     R3D_SHADER_BIND_SAMPLER(deferred.ambient, uOrmTex, r3d_target_get_level(R3D_TARGET_ORM, 0));
 
+    // Local patch 11: baked world occlusion
+    R3D_SHADER_SET_INT(deferred.ambient, uWorldAoEnabled, worldAo.texture != 0 ? 1 : 0);
+    if (worldAo.texture != 0)
+    {
+        R3D_SHADER_BIND_SAMPLER(deferred.ambient, uWorldAoTex, worldAo.texture);
+        R3D_SHADER_SET_VEC3(deferred.ambient, uWorldAoMin, worldAo.min);
+        R3D_SHADER_SET_VEC3(deferred.ambient, uWorldAoInvExtent, worldAo.invExtent);
+        R3D_SHADER_SET_VEC2(deferred.ambient, uWorldAoDecode, worldAo.decode);
+        R3D_SHADER_SET_FLOAT(deferred.ambient, uWorldAoIntensity, worldAo.intensity);
+    }
+
     R3D_RENDER_SCREEN();
 }
```

## Patch 12: the model importer leaks uthash's table for every model with textures (2026-10-11)

**Symptom.** LeakSanitizer (`tools/leakcheck.sh`, monolith stream M) reported, for every model
file with at least one texture loaded with `R3D_LoadModel*` / `R3D_LoadMaterials*`: a direct
leak of 64 bytes (uthash's `UT_hash_table`) and an indirect one of 512 bytes (its bucket
array), allocated in `r3d_importer_load_texture_cache` (`maps/pine_forest`: 2 files, 128 + 1024
bytes per load). Every hot reload of such a package leaked it again.

**Cause.** `r3d_importer_load_texture_cache` deduplicates the material textures in a uthash
table whose entries live on R3D's scratch stack (`R3D_STACK_SCOPE`), so nothing frees them,
which is right; but `HASH_ADD` also `malloc`s the table header and the buckets, and the
function returned without `HASH_CLEAR`, so those were lost when the scope ended.

**Fix.** `HASH_CLEAR(hh, hashTable)` once the collection loop is done (the table is not used
after it). `HASH_CLEAR` frees only uthash's own table and buckets, never the entries.

**Files.** `src/importer/r3d_importer_texture.c`.

```diff
@@ -480,6 +480,10 @@ r3d_importer_texture_cache_t* r3d_importer_load_texture_cache(
             }
         }
 
+        // The entries live on the scratch stack, but uthash malloc'd its table and buckets:
+        // free those (HASH_CLEAR frees no entries). Local patch 12, see patches.md.
+        HASH_CLEAR(hh, hashTable);
+
         // Parallel texture loading to RAM
         loader_context_t ctx = {
             .importer = importer,
```

**Verification.** `tools/leakcheck.sh`: the `shot-pine-forest` run went from 2 direct + 2
indirect leaks of ours (1152 bytes) to none; every other run unchanged (zero leaks of ours).
Rendering is unaffected (the table is freed after its last use): `--shot` of
`maps/pine_forest` views 1-3 and of `tests/models` pixel-identical with and without the patch.

## Patch 13: volumetric fog density fade by distance from the eye (2026-10-13)

**Symptom.** Not a bug, a missing control. In `maps/monolith`, looking toward the sun across
the open terrace, the sunlit volumetric fog veiled the apartment tower ~100 m away. Thinning the
fog (`scatteringDensity`, `sun.fogEnergy`) also weakened the light shafts in the hall, which the
developer rejected. And the fog stops hard at `volumetricFog.length` (no fade at the end of the
march).

**Change.** Two new `R3D_VolumetricFog` fields, `fadeStart` and `fadeEnd` (default 0 = off).
With `fadeEnd > 0` the fog density is multiplied by `1 - smoothstep(fadeStart, fadeEnd, t)`,
t = distance from the eye along the view ray (never view depth, .claude/skills/lighting-fog-shadows/r3d-pitfalls.md 6):
- `vfog_radiance.frag`: per step the in-scatter and the step's extinction are scaled by the
  fade; the march stops at `fadeEnd` (the density is 0 beyond it). The directional light's
  closed form for unshadowed rays (patch 9) is skipped when fading (the per-step factors vary).
  Without a fade every path runs the unpatched arithmetic.
- `vfog_transmittance.frag`: the optical depth uses the analytic integral of the fade,
  `a + w (u - u^3 + u^4 / 2)` (u = clamped (d - fadeStart) / w, w = fadeEnd - fadeStart,
  a = min(fadeStart, d)), so transmittance and emission thin out the same way.
- `FX_VFog` / `r3d_shader_block_fx_vfog` get the two floats (std140: C struct padded to 80 bytes),
  `r3d_draw.c` uploads them.

**Build note.** R3D's CMake does not track `#include`s of shaders: after changing
`shaders/include/ubo/fx.glsl`, only the shaders whose own file changed were regenerated, and the
others kept the old FxBlock layout (a nearly black image). Touch every shader source
(`find shaders -type f -exec touch {} +`) after changing a shared include.

**monolith side.** `env volumetricFog.fadeStart` / `fadeEnd` (EnvDef keys, so a map using them
also parses against upstream R3D, where they are ignored); `MS_ApplyEnvironment` sets the fields
under `MS_PATCHED_R3D`. `MS_WriteEnvironment` writes them only when a map sets a fade.

**Files.** `include/r3d/r3d_environment.h`, `shaders/include/ubo/fx.glsl`,
`shaders/deferred/vfog_radiance.frag`, `shaders/deferred/vfog_transmittance.frag`,
`src/modules/r3d_shader.h`, `src/r3d_draw.c`.

```diff
diff --git a/include/r3d/r3d_environment.h b/include/r3d/r3d_environment.h
index 466d1e9..9028b74 100644
--- a/include/r3d/r3d_environment.h
+++ b/include/r3d/r3d_environment.h
@@ -115,6 +115,8 @@
             .skyAffect         = 0.5f,                  \
             .length            = 50.0f,                 \
             .stepSize          = 1.0f,                  \
+            .fadeStart         = 0.0f,                  \
+            .fadeEnd           = 0.0f,                  \
             .enabled           = false,                 \
         },                                              \
         .dof = {                                        \
@@ -313,6 +315,8 @@ typedef struct R3D_VolumetricFog {
     float skyAffect;            ///< Fog influence on the skybox [0-1] (default: 0.5)
     float length;               ///< Maximum fog render distance in world units (default: 50.0)
     float stepSize;             ///< Ray-march step size; smaller = higher quality, lower performance (default: 1.0)
+    float fadeStart;            ///< Distance from the eye where the fog starts thinning out (local patch 13; default: 0.0)
+    float fadeEnd;              ///< Distance where it is gone, smoothstep in between; <= 0 disables the fade (local patch 13; default: 0.0)
     bool enabled;               ///< Enables or disables the volumetric fog (default: false)
 } R3D_VolumetricFog;
 
diff --git a/shaders/deferred/vfog_radiance.frag b/shaders/deferred/vfog_radiance.frag
index d18e236..c97c51f 100644
--- a/shaders/deferred/vfog_radiance.frag
+++ b/shaders/deferred/vfog_radiance.frag
@@ -151,6 +151,15 @@ float VFog_SunVisibility(vec3 Pws, float fadeDist, mat2 diskRot)
     return VFog_SampleShadowDir(Pws, fadeDist, diskRot);
 }
 
+// Distance fade of the fog density (local patch 13, see patches.md): 1 up to
+// uVFog.fadeStart from the eye, smoothstep down to 0 at uVFog.fadeEnd. Only called when
+// uVFog.fadeEnd > 0; without a fade the march below runs the unpatched arithmetic.
+float VFog_DensityFade(float t)
+{
+    float u = clamp((t - uVFog.fadeStart) / max(uVFog.fadeEnd - uVFog.fadeStart, 1e-3), 0.0, 1.0);
+    return 1.0 - u * u * (3.0 - 2.0 * u);
+}
+
 // Computes the march range [tEnter, tExit] along rayDir (from rayOrigin),
 // clamped to uVFog.maxDistance and to the light's range sphere when applicable.
 // tEnter/tExit are both 0.0 when the ray never enters the light's range
@@ -221,6 +230,12 @@ void main()
     vec3 radiance = vec3(0.0);
     float transmittance = 1.0;
 
+    // Density fade (local patch 13): t = distance of the march position from the eye.
+    // Past fadeEnd the density is 0, so those steps add nothing: stop the march there.
+    bool fadeOn = uVFog.fadeEnd > 0.0;
+    float tStart = tEnter + uVFog.stepSize * jitter;
+    if (fadeOn) rayDist = min(rayDist, uVFog.fadeEnd - tStart);
+
     // Extinction coefficient and per-step transmittance factor (constant along the ray)
     float sigmaT = uVFog.scatteringDensity + uVFog.absortionDensity;
     float transmittanceStep = exp(-sigmaT * uVFog.stepSize);
@@ -247,10 +262,25 @@ void main()
         float phase = PhaseFunction_Schlick(-uLight.direction, fragToCameraNorm);
         vec3 LiDir = uVFog.scatteringDensity * uVFog.scatteringColor.rgb * phase * lightColor;
 
+        // With a density fade (local patch 13) the per-step factors vary: plain loop.
+        if (fadeOn)
+        {
+            for (float l = 0; l < rayDist; l += uVFog.stepSize)
+            {
+                float visibility = hasShadow ? VFog_SunVisibility(marchPos, fragDist, diskRot) : 1.0;
+                float fade = VFog_DensityFade(tStart + l);
+
+                transmittance *= exp(-sigmaT * uVFog.stepSize * fade);
+                radiance += LiDir * (visibility * fade) * transmittance * uVFog.stepSize;
+
+                marchPos += deltaStep;
+            }
+            rayDist = 0.0;
+        }
         // Unshadowed rays (sky, surfaces >= range away): every step adds LiDir times the
         // transmittance so far, a geometric series with a closed form. n = the loop's
         // step count (equal up to float rounding of the accumulated l).
-        if (!hasShadow)
+        else if (!hasShadow)
         {
             float n = ceil(rayDist / uVFog.stepSize);
             float series = (transmittanceStep < 0.99999)
@@ -313,7 +343,13 @@ void main()
         vec3 Li = uVFog.scatteringDensity * uVFog.scatteringColor.rgb * phase * lightColor * visibility;
 
         // Accumulate transmittance then integrate radiance (order matters)
-        transmittance *= transmittanceStep;
+        if (fadeOn)
+        {
+            float fade = VFog_DensityFade(tStart + l);
+            Li *= fade;
+            transmittance *= exp(-sigmaT * uVFog.stepSize * fade);
+        }
+        else transmittance *= transmittanceStep;
         radiance += Li * transmittance * uVFog.stepSize;
 
         marchPos += deltaStep;
diff --git a/shaders/deferred/vfog_transmittance.frag b/shaders/deferred/vfog_transmittance.frag
index 9413740..7a40855 100644
--- a/shaders/deferred/vfog_transmittance.frag
+++ b/shaders/deferred/vfog_transmittance.frag
@@ -50,6 +50,18 @@ void main()
     float sigmaT = uVFog.scatteringDensity + uVFog.absortionDensity;
     float transmittance = exp(-sigmaT * rayDist);
 
+    // Density fade (local patch 13): the density is 1 - smoothstep(fadeStart, fadeEnd, t),
+    // so the optical depth is sigmaT times its integral over [0, rayDist]. With
+    // s(u) = 3u^2 - 2u^3 and w = fadeEnd - fadeStart: int_0^u s = u^3 - u^4 / 2.
+    if (uVFog.fadeEnd > 0.0)
+    {
+        float a = min(uVFog.fadeStart, rayDist);
+        float w = max(uVFog.fadeEnd - uVFog.fadeStart, 1e-3);
+        float u = clamp((rayDist - uVFog.fadeStart) / w, 0.0, 1.0);
+        float depthInt = a + w * (u - (u * u * u - 0.5 * u * u * u * u));
+        transmittance = exp(-sigmaT * depthInt);
+    }
+
     // Emission fills the volume proportionally to extinction
     float extinction = 1.0 - transmittance;
     vec3 emission = uVFog.emissionColor.rgb * (uVFog.emissionEnergy * extinction);
diff --git a/shaders/include/ubo/fx.glsl b/shaders/include/ubo/fx.glsl
index 898f0be..34c2414 100644
--- a/shaders/include/ubo/fx.glsl
+++ b/shaders/include/ubo/fx.glsl
@@ -93,6 +93,8 @@ struct FX_VFog {
     float maxDistance;
     float stepSize;
     bool enabled;
+    float fadeStart;    // local patch 13: density fades out by distance from the eye
+    float fadeEnd;      // <= 0: no fade
 };
 
 struct FX_Dof {
diff --git a/src/modules/r3d_shader.h b/src/modules/r3d_shader.h
index e17518a..cf7ee93 100644
--- a/src/modules/r3d_shader.h
+++ b/src/modules/r3d_shader.h
@@ -557,6 +557,9 @@ typedef struct {
         alignas(4) float maxDistance;
         alignas(4) float stepSize;
         alignas(4) int32_t enabled;
+        alignas(4) float fadeStart;     // local patch 13
+        alignas(4) float fadeEnd;
+        uint8_t _pad[8];                // std140: the struct is padded to 16 bytes
     } uVFog;
 
     struct r3d_shader_block_fx_dof {
diff --git a/src/r3d_draw.c b/src/r3d_draw.c
index 7e2266b..31314a5 100644
--- a/src/r3d_draw.c
+++ b/src/r3d_draw.c
@@ -1217,6 +1217,8 @@ void upload_fx_block(void)
         block.uVFog.maxDistance       = env->volumetricFog.length;
         block.uVFog.stepSize          = env->volumetricFog.stepSize;
         block.uVFog.enabled           = env->volumetricFog.enabled;
+        block.uVFog.fadeStart         = env->volumetricFog.fadeStart;   // local patch 13
+        block.uVFog.fadeEnd           = env->volumetricFog.fadeEnd;
     }
 
     if (env->dof.mode != R3D_DOF_DISABLED)
```

**Verification.** All embedded shaders that compile standalone pass `glslangValidator`
(the 5 that do not are templates needing surface-shader injection, unchanged). Without a fade
(old and new binary, same map files): `maps/pine_forest` views 1-3, `tests/models` view 1, the
default map's request view, steep view and `--fog-cache` view identical; default map views
1-7: 5 of 7 identical, views 3 and 5 differ in 2 pixels each (max 6/255, shader codegen
rounding). `--dump-scene-diff` identical for all three packages. `tools/leakcheck.sh`: zero
leaks of ours. Both targets and the examples build without warnings. Performance not benchmarked
(with a fade the sun's march stops at fadeEnd instead of `length`, but sky rays lose the patch 9
closed form).

## Patch 14: hard line in the volumetric fog where the ground crosses 75 % of sun.range (2026-10-13)

**Symptom.** In `maps/monolith`, looking down across open ground from ~14 m up (request
`fog-ends-so-instantly-sharply-as-you-can`), the haze over the ground stopped abruptly: the
ground within ~75 m lay under dark, nearly clear air, and everything beyond it under bright,
beige, sunlit fog, with a hard line between them. The canyon exit and the hall openings glowed
the same way.

**Cause.** The fog's sun shadow is faded by `fadeDist` = the distance from the eye to the
*surface behind the march* (upstream: its view depth; patch 4 made it a distance), constant along
the ray. Rays that end on a surface past 0.75 `range` get an unshadowed sun for **every** step,
even the steps a few metres from the eye that lie in deep shadow. So the whole fog column in
front of far ground or sky switches from shadowed to sunlit over 75-100 m of *surface* distance,
which on screen is a few pixels. Noted as an upstream quirk in patch 2.

**Change.** Only in the density-fade path (patch 13, `fadeEnd > 0`; without a fade every path is
unchanged): the shadow of each step is faded by that step's own distance from the eye `t`
(`VFog_SunVisibility(marchPos, t, …)`), steps at `t >= range` skip the lookup (visibility 1), and
the per-ray skip `fragDist >= range` no longer applies (sky and far rays now take shadow lookups
for their steps closer than `range`). Steps closer than `range` lie inside the fade-free area of
monolith's static tiles (patch 7) and of R3D's camera fit, so edgeFade stays 1, as before.

**Files.** `shaders/deferred/vfog_radiance.frag`.

```diff
--- a/shaders/deferred/vfog_radiance.frag
+++ b/shaders/deferred/vfog_radiance.frag
@@ -248,7 +248,9 @@
     // VFog_SampleShadowDir fades the shadow out by the distance to the surface behind the
     // march (constant along the ray) and weights it by exactly 0 at fragDist >= range,
     // e.g. on sky pixels. Skip its taps there: same result, no wasted PCF lookups.
-    if (uLight.type == LIGHT_DIR && fragDist >= uLight.range) hasShadow = false;
+    // With a density fade (local patch 14) the shadow fades by each step's own distance
+    // from the eye instead, so the per-step branch below decides.
+    if (uLight.type == LIGHT_DIR && fragDist >= uLight.range && !fadeOn) hasShadow = false;
 
     // Precompute inverses used inside the loop
     float invRange = 1.0 / max(uLight.range, 1e-4);
@@ -265,10 +267,16 @@
         // With a density fade (local patch 13) the per-step factors vary: plain loop.
         if (fadeOn)
         {
+            // Shadow fade by the step's distance from the eye (local patch 14), not by
+            // fragDist: fragDist is constant along the ray, so all fog in front of a surface
+            // past 0.75 range went fully sunlit at once, a hard line across the haze where
+            // the ground crosses that distance. Steps closer than range lie inside the shadow
+            // map's fade-free area (static tiles and camera fit alike), so edgeFade stays 1.
             for (float l = 0; l < rayDist; l += uVFog.stepSize)
             {
-                float visibility = hasShadow ? VFog_SunVisibility(marchPos, fragDist, diskRot) : 1.0;
-                float fade = VFog_DensityFade(tStart + l);
+                float t = tStart + l;
+                float visibility = (hasShadow && t < uLight.range) ? VFog_SunVisibility(marchPos, t, diskRot) : 1.0;
+                float fade = VFog_DensityFade(t);
 
                 transmittance *= exp(-sigmaT * uVFog.stepSize * fade);
                 radiance += LiDir * (visibility * fade) * transmittance * uVFog.stepSize;
```

**Verification.** `glslangValidator` passes on the preprocessed shader. `maps/pine_forest`
(no fade) views 1-3: 0 pixels differ. `maps/monolith`: the request view loses the line (the fog
over near and far ground matches; the edge left there is the real ground shadow of a
megastructure); views 1-7 vs. before: 1 (Canyon) and 2 (Hypostyle hall) lose the false sunlit glow
at the far end (max 62-64/255), 4-6 slightly less beige haze in front of distant shadowed
buildings, 3 and 7 unchanged (max 6/255). Both targets and the examples build without warnings;
`tools/leakcheck.sh`: zero leaks of ours. **Cost** (headless `--bench`, 3 interleaved A/B pairs,
RTX 4050 laptop): vfog radiance 0.504 / 0.433 / 0.529 → 0.763 / 0.852 / 0.860 ms (+0.34 ms,
Tower +0.8 ms, Overview and Terrace +0.5 ms, Canyon +0.01 ms); frame time +0.3-0.4 ms at matching GPU clocks
(8.11 / 8.03 → 8.37 / 8.42 ms; 4.36 → 4.75 ms in the low-clock pair).

## Patch 15: volumetric fog fade-in near the eye (2026-10-13)

**Symptom.** In `maps/monolith` the apartment tower looked washed out when the player stood in
sunlight and normal when they stood in shadow (request "player location changes how apartment
looks"). Shots from a sunlit and a shaded spot on the terrace, both looking at the same tower
windows (toward the sun): mean grey of the tower 95 (sun) vs 64 (shade); with `--off 1` (no
volumetric fog) 74 vs 73.

**Cause.** Not a bug in the march: the fog around a sunlit eye is lit, and looking toward the
sun (anisotropy 0.4) it scatters strongly into the camera, so it veils the whole frame. Fog
limited to 0-15 m from the eye alone gave 98 in the sunlit shot; fog from 12 m or 20 m onward
gave 78 / 72. Patch 13's fade only thins the far end, and thinning the fog everywhere was
rejected (weaker shafts).

**Change.** A new `R3D_VolumetricFog` field `fadeNear` (default 0 = off). With `fadeNear > 0`
the density is also multiplied by `smoothstep(0, fadeNear, t)` (t = distance from the eye):
- `vfog_radiance.frag`: `VFog_DensityFade` multiplies both ramps; `fadeOn` is true when either
  fade is set (so a fade-in alone takes the per-step path, incl. patch 14's per-step shadow
  fade); the march is still cut at `fadeEnd` only when a fade-out is set.
- `vfog_transmittance.frag`: the optical depth loses `n (v - v^3 + v^4 / 2)` (v = clamped
  d / n). Exact while `fadeNear <= fadeStart` (the ramps don't overlap); monolith clamps
  `fadeNear` to `fadeStart` when a fade-out is set. The patch 13 integral is unchanged.
- `FX_VFog` / `r3d_shader_block_fx_vfog` get the float (C padding 8 -> 4 bytes, still 80),
  `r3d_draw.c` uploads it. Touched every shader source before building (patch 13 build note).

**monolith side.** `env volumetricFog.fadeNear` (EnvDef key, range 0-10000, written by
`MS_WriteEnvironment` only when set), applied under `MS_PATCHED_R3D`. `maps/monolith`:
`fadeStart 10 -> 15`, `fadeNear 15`.

**Files.** `include/r3d/r3d_environment.h`, `shaders/include/ubo/fx.glsl`,
`shaders/deferred/vfog_radiance.frag`, `shaders/deferred/vfog_transmittance.frag`,
`src/modules/r3d_shader.h`, `src/r3d_draw.c`.

```diff
--- a/include/r3d/r3d_environment.h
+++ b/include/r3d/r3d_environment.h
@@ -117,6 +117,7 @@
             .stepSize          = 1.0f,                  \
             .fadeStart         = 0.0f,                  \
             .fadeEnd           = 0.0f,                  \
+            .fadeNear          = 0.0f,                  \
             .enabled           = false,                 \
         },                                              \
         .dof = {                                        \
@@ -317,6 +318,7 @@
     float stepSize;             ///< Ray-march step size; smaller = higher quality, lower performance (default: 1.0)
     float fadeStart;            ///< Distance from the eye where the fog starts thinning out (local patch 13; default: 0.0)
     float fadeEnd;              ///< Distance where it is gone, smoothstep in between; <= 0 disables the fade (local patch 13; default: 0.0)
+    float fadeNear;             ///< Density fades in from 0 at the eye to full at this distance (smoothstep); <= 0: off (local patch 15; default: 0.0)
     bool enabled;               ///< Enables or disables the volumetric fog (default: false)
 } R3D_VolumetricFog;
 
--- a/shaders/include/ubo/fx.glsl
+++ b/shaders/include/ubo/fx.glsl
@@ -95,6 +95,7 @@
     bool enabled;
     float fadeStart;    // local patch 13: density fades out by distance from the eye
     float fadeEnd;      // <= 0: no fade
+    float fadeNear;     // local patch 15: density fades in from 0 at the eye; <= 0: none
 };
 
 struct FX_Dof {
--- a/shaders/deferred/vfog_radiance.frag
+++ b/shaders/deferred/vfog_radiance.frag
@@ -152,12 +152,24 @@
 }
 
 // Distance fade of the fog density (local patch 13, see patches.md): 1 up to
-// uVFog.fadeStart from the eye, smoothstep down to 0 at uVFog.fadeEnd. Only called when
-// uVFog.fadeEnd > 0; without a fade the march below runs the unpatched arithmetic.
+// uVFog.fadeStart from the eye, smoothstep down to 0 at uVFog.fadeEnd. Local patch 15:
+// with uVFog.fadeNear > 0 it also fades in, smoothstep from 0 at the eye to 1 at fadeNear,
+// so the fog around the eye (sunlit or not) doesn't veil the whole view. Only called when
+// a fade is set; without one the march below runs the unpatched arithmetic.
 float VFog_DensityFade(float t)
 {
-    float u = clamp((t - uVFog.fadeStart) / max(uVFog.fadeEnd - uVFog.fadeStart, 1e-3), 0.0, 1.0);
-    return 1.0 - u * u * (3.0 - 2.0 * u);
+    float f = 1.0;
+    if (uVFog.fadeEnd > 0.0)
+    {
+        float u = clamp((t - uVFog.fadeStart) / max(uVFog.fadeEnd - uVFog.fadeStart, 1e-3), 0.0, 1.0);
+        f = 1.0 - u * u * (3.0 - 2.0 * u);
+    }
+    if (uVFog.fadeNear > 0.0)
+    {
+        float v = clamp(t / uVFog.fadeNear, 0.0, 1.0);
+        f *= v * v * (3.0 - 2.0 * v);
+    }
+    return f;
 }
 
 // Computes the march range [tEnter, tExit] along rayDir (from rayOrigin),
@@ -232,9 +244,10 @@
 
     // Density fade (local patch 13): t = distance of the march position from the eye.
     // Past fadeEnd the density is 0, so those steps add nothing: stop the march there.
-    bool fadeOn = uVFog.fadeEnd > 0.0;
+    // A near fade-in alone (local patch 15) takes the same per-step path.
+    bool fadeOn = uVFog.fadeEnd > 0.0 || uVFog.fadeNear > 0.0;
     float tStart = tEnter + uVFog.stepSize * jitter;
-    if (fadeOn) rayDist = min(rayDist, uVFog.fadeEnd - tStart);
+    if (uVFog.fadeEnd > 0.0) rayDist = min(rayDist, uVFog.fadeEnd - tStart);
 
     // Extinction coefficient and per-step transmittance factor (constant along the ray)
     float sigmaT = uVFog.scatteringDensity + uVFog.absortionDensity;
--- a/shaders/deferred/vfog_transmittance.frag
+++ b/shaders/deferred/vfog_transmittance.frag
@@ -53,12 +53,25 @@
     // Density fade (local patch 13): the density is 1 - smoothstep(fadeStart, fadeEnd, t),
     // so the optical depth is sigmaT times its integral over [0, rayDist]. With
     // s(u) = 3u^2 - 2u^3 and w = fadeEnd - fadeStart: int_0^u s = u^3 - u^4 / 2.
-    if (uVFog.fadeEnd > 0.0)
+    // Near fade-in (local patch 15): the density is also multiplied by s(t / fadeNear), which
+    // removes n (v - v^3 + v^4 / 2) of it (v = clamped d / n). Exact while fadeNear <=
+    // fadeStart (the two ramps don't overlap); monolith clamps it to that.
+    if (uVFog.fadeEnd > 0.0 || uVFog.fadeNear > 0.0)
     {
-        float a = min(uVFog.fadeStart, rayDist);
-        float w = max(uVFog.fadeEnd - uVFog.fadeStart, 1e-3);
-        float u = clamp((rayDist - uVFog.fadeStart) / w, 0.0, 1.0);
-        float depthInt = a + w * (u - (u * u * u - 0.5 * u * u * u * u));
+        float depthInt = rayDist;
+        if (uVFog.fadeEnd > 0.0)
+        {
+            float a = min(uVFog.fadeStart, rayDist);
+            float w = max(uVFog.fadeEnd - uVFog.fadeStart, 1e-3);
+            float u = clamp((rayDist - uVFog.fadeStart) / w, 0.0, 1.0);
+            depthInt = a + w * (u - (u * u * u - 0.5 * u * u * u * u));
+        }
+        if (uVFog.fadeNear > 0.0)
+        {
+            float n = uVFog.fadeNear;
+            float v = clamp(rayDist / n, 0.0, 1.0);
+            depthInt = max(depthInt - n * (v - (v * v * v - 0.5 * v * v * v * v)), 0.0);
+        }
         transmittance = exp(-sigmaT * depthInt);
     }
 
--- a/src/modules/r3d_shader.h
+++ b/src/modules/r3d_shader.h
@@ -559,7 +559,8 @@
         alignas(4) int32_t enabled;
         alignas(4) float fadeStart;     // local patch 13
         alignas(4) float fadeEnd;
-        uint8_t _pad[8];                // std140: the struct is padded to 16 bytes
+        alignas(4) float fadeNear;      // local patch 15
+        uint8_t _pad[4];                // std140: the struct is padded to 16 bytes
     } uVFog;
 
     struct r3d_shader_block_fx_dof {
@@ -612,6 +613,8 @@
     alignas(4)  float   shadowFar;
     alignas(4)  int32_t shadowLayer;
     alignas(4)  int32_t type;
+    alignas(16) Vector3 clipMin;    // local patch 16
+    alignas(16) Vector3 clipMax;
 } r3d_shader_block_light_t;
 
 typedef struct {
--- a/src/r3d_draw.c
+++ b/src/r3d_draw.c
@@ -1054,6 +1054,8 @@
         data->shadowFar       = light->shadowFar;
         data->shadowLayer     = shadow ? light->shadowLayer : -1;
         data->type            = light->type;
+        data->clipMin         = light->clipMin;
+        data->clipMax         = light->clipMax;
 
         if (++lights.uNumLights == R3D_HINT(R3D_HINT_FORWARD_LIGHT_PER_MESH))
         {
@@ -1219,6 +1221,7 @@
         block.uVFog.enabled           = env->volumetricFog.enabled;
         block.uVFog.fadeStart         = env->volumetricFog.fadeStart;   // local patch 13
         block.uVFog.fadeEnd           = env->volumetricFog.fadeEnd;
+        block.uVFog.fadeNear          = env->volumetricFog.fadeNear;    // local patch 15
     }
 
     if (env->dof.mode != R3D_DOF_DISABLED)
@@ -2632,6 +2635,8 @@
             .shadowFar       = light->shadowFar,
             .shadowLayer     = light->shadowLayer,
             .type            = light->type,
+            .clipMin         = light->clipMin,
+            .clipMax         = light->clipMax,
         };
         r3d_shader_set_uniform_block(R3D_SHADER_BLOCK_LIGHT, &data, true);
 
@@ -2802,6 +2807,8 @@
             .shadowFar       = light->shadowFar,
             .shadowLayer     = light->shadowLayer,
             .type            = light->type,
+            .clipMin         = light->clipMin,
+            .clipMax         = light->clipMax,
         };
         r3d_shader_set_uniform_block(R3D_SHADER_BLOCK_LIGHT, &data, true);
 
```

**Verification.** Without `fadeNear` (old and new binary, same map files): default map views
1-7, the request view and `maps/pine_forest` view 2 pixel-identical (0 pixels differ).
`vfog_radiance`, `vfog_transmittance`, `vfog_compose` pass `glslangValidator` (includes
resolved with `scripts/glsl_processor.py`). With `fadeNear 15` on the default map: tower from
the sunlit spot 95 -> 75, from the shaded spot 64 -> 66 (no fog: 74 / 73); the request's own
view (eye in a narrow sunlit strip) 108 -> 89 vs 64 -> 67 in shade (no fog 77 / 73: the rest
is sunlit fog 15-50 m away). Hall shafts (views 1, 2) unchanged by eye; view 5 (Terrace) mean
120 -> 106. `--dump-boxes` identical. Both targets and the examples build without warnings;
`tools/leakcheck.sh`: zero leaks of ours. Performance not benchmarked (one smoothstep per
march step, on a path the map already ran).

## Patch 16: per-light clip box (unshadowed lights shine through floors) (2026-10-13)

**Symptom.** In `maps/monolith`'s tower every flat has an unshadowed point or spot light (range
5-9 m). The 0.3 m floor slabs don't stop them: the storey below (and the lobby) got warm pools
on the upper walls and floor from the lamps one floor up. Shadow maps for ~290 omni lights would
take ~100 MB each at the default 2048 px cube size.

**Cause.** R3D's point / spot lights reach every surface inside their range sphere; only a
shadow map occludes them.

**Change.** `R3D_Light` gets `bool clip; Vector3 clipMin, clipMax;` (zero-initialised = off, so
existing callers are unchanged). The light only reaches world points inside the box: deferred
lighting returns 0 outside it, the forward pass skips the light, and the volumetric fog march is
cut to the ray's span inside the box. Lights without a clip carry +-1e30 bounds
(`R3D_LIGHT_NO_CLIP`), which no point fails, and the fog skips the slab test for them.
monolith: `light ... clip x0 y0 z0 x1 y1 z1` in map files (`runtime/monolith_scene.c`, under
`MS_PATCHED_R3D`).

Files: `include/r3d/r3d_lighting.h`, `src/modules/r3d_light.h`, `src/modules/r3d_light.c`,
`src/modules/r3d_shader.h`, `src/r3d_draw.c`, `shaders/include/ubo/light.glsl`,
`shaders/deferred/lighting.frag`, `shaders/scene/forward.frag`, `shaders/deferred/vfog_radiance.frag`.

```diff
--- include/r3d/r3d_lighting.h  (struct R3D_Light)
     R3D_LightType type;     ///< Light type (directional/spot/omni)
+    bool clip;              ///< Light only reaches points inside [clipMin, clipMax] (local patch 16)
+    Vector3 clipMin;        ///< World-space clip box minimum corner (when clip is true)
+    Vector3 clipMax;        ///< World-space clip box maximum corner (when clip is true)
 } R3D_Light;
--- src/modules/r3d_light.h  (r3d_light_data_t)
     R3D_LightType type;
+    Vector3 clipMin, clipMax;   // World-space clip box, +-R3D_LIGHT_NO_CLIP without one (local patch 16)
 } r3d_light_data_t;
+
+// Clip box bound of a light without one (local patch 16): every point is inside
+#define R3D_LIGHT_NO_CLIP 1e30f
--- src/modules/r3d_light.c  (r3d_light_push)
+    // Clip box (local patch 16): set on the entry the type's push adds (none when culled)
+    size_t before = R3D_MOD_LIGHT.listLightData->elemCount;
     switch (light->type)
     ...
     }
+
+    if (R3D_MOD_LIGHT.listLightData->elemCount > before)
+    {
+        r3d_light_data_t* data = r3d_light_get((int)before);
+        bool clip = light->clip;
+        data->clipMin = clip ? light->clipMin : (Vector3){ -R3D_LIGHT_NO_CLIP, -R3D_LIGHT_NO_CLIP, -R3D_LIGHT_NO_CLIP };
+        data->clipMax = clip ? light->clipMax : (Vector3){ R3D_LIGHT_NO_CLIP, R3D_LIGHT_NO_CLIP, R3D_LIGHT_NO_CLIP };
+    }
 }
--- src/modules/r3d_shader.h  (r3d_shader_block_light_t; std140: each vec3 on a 16-byte boundary)
     alignas(4)  int32_t type;
+    alignas(16) Vector3 clipMin;    // local patch 16
+    alignas(16) Vector3 clipMax;
 } r3d_shader_block_light_t;
--- shaders/include/ubo/light.glsl  (struct Light)
     int type;
+    vec3 clipMin;           //< world-space clip box (local patch 16); +-1e30 without one
+    vec3 clipMax;
 };
--- src/r3d_draw.c  (forward light array, deferred lighting pass, volumetric fog pass)
+        data->clipMin         = light->clipMin;
+        data->clipMax         = light->clipMax;
 ...
+            .clipMin         = light->clipMin,      (both designated initialisers)
+            .clipMax         = light->clipMax,
--- shaders/deferred/lighting.frag  (after P is reconstructed)
+    if (any(lessThan(P, uLight.clipMin)) || any(greaterThan(P, uLight.clipMax)))
+    {
+        FragRadiance = vec4(0.0);
+        FragSpecular = vec4(0.0);
+        return;
+    }
--- shaders/scene/forward.frag  (light loop)
         Light light = uLights[i];
+        if (any(lessThan(vPosition, light.clipMin)) || any(greaterThan(vPosition, light.clipMax))) continue;
--- shaders/deferred/vfog_radiance.frag  (end of VFog_GetMarchRange)
+    if (uLight.clipMin.x > -1e29)
+    {
+        vec3 invDir = 1.0 / rayDir;
+        vec3 t0 = (uLight.clipMin - rayOrigin) * invDir;
+        vec3 t1 = (uLight.clipMax - rayOrigin) * invDir;
+        vec3 tMin = min(t0, t1), tMax = max(t0, t1);
+        tEnter = max(tEnter, max(max(tMin.x, tMin.y), tMin.z));
+        tExit = min(tExit, min(min(tMax.x, tMax.y), tMax.z));
+        if (tExit <= tEnter) { tEnter = 0.0; tExit = 0.0; }
+    }
```

**Verification.** Without clip boxes, old vs new binary: `maps/pine_forest` views 2 and 3 and
`tests/models` view 1 pixel-identical (0 pixels differ; pine_forest has a shadowed and several
unshadowed point lights). `maps/monolith` with the 288 tower lights clipped to their storey:
the lobby and storey-1 views lose the warm pools from the lamps one floor up, and each storey
stays lit by its own lights (request `2026-10-13_104917_dark-places-gets-lightning-from-up`,
before/after images). `lighting.frag`, `vfog_radiance.frag`, `forward.frag` pass
`glslangValidator` (includes resolved with `scripts/glsl_processor.py -I shaders/include`,
forward.frag with the runtime `MAX_*` defines). Both targets and the examples build without
warnings; `tools/leakcheck.sh`: zero leaks of ours. Performance not benchmarked (one box test
per light pixel; clipped lights still use their full scissor rect).

## Patch 17: draw order depended on heap addresses, so identical runs rendered differently (2026-10-24)

**Symptom.** The same program rendering the same view of `maps/monolith` produced 2-6 slightly
different images over 6-15 runs (1-550 of 1.44 M pixels, up to 34 levels: isolated pixels at
coplanar faces and in accumulated passes), in monolith's `--shot`, in the examples and in an
exported package. Which images appeared also changed with the build and with the length of a
path argument. Maps with few shader materials (`maps/pine_forest`, `tests/models`) were stable.

**Cause.** `sort_fill_state_data` (`src/modules/r3d_render.c`) puts the material's custom shader
*pointer* into the draw sort key (`state->shader = (uintptr_t)material.shader`), right after the
priority. Heap addresses are not reproducible: the GL driver's threads allocate from the same
heap at timing-dependent moments, and a different build changes the allocation sizes. So the
draws of materials with different surface shaders (monolith: one shader instance per map
material, ~100 on the default map) came in a different order on each run, and with them the
winner at equal depths and the order of accumulated results.

**Change.** Every custom shader instance gets a creation serial (`r3d_shader_custom_t.serial`,
1, 2, ... in `r3d_shader_custom_alloc` and `r3d_shader_custom_clone`), and the sort key is the
serial (0 without a shader, as the NULL pointer was). Loading order is deterministic, so the
draw order is. Draws are still grouped by shader exactly as before; only the order between
shader groups is now fixed (creation order) instead of address order.

Files: `src/modules/r3d_shader.h`, `src/modules/r3d_shader.c`, `src/modules/r3d_render.h`,
`src/modules/r3d_render.c`.

```diff
--- src/modules/r3d_shader.h  (r3d_shader_custom_t)
     bool programOwner;
+    uint32_t serial;    // creation order (1, 2, ...): the draw sort key instead of the address (local patch 17)
 } r3d_shader_custom_t;
--- src/modules/r3d_shader.c
+static uint32_t r3d_shader_custom_serial = 0;
 r3d_shader_custom_t* r3d_shader_custom_alloc(void)
     shader->programOwner = true;
+    shader->serial = ++r3d_shader_custom_serial;
 r3d_shader_custom_t* r3d_shader_custom_clone(r3d_shader_custom_t* custom)
     clone->programOwner = false;
+    clone->serial = ++r3d_shader_custom_serial;
--- src/modules/r3d_render.h  (r3d_render_sort_state_t)
-    uintptr_t shader;       ///< Shader program pointer
+    uintptr_t shader;       ///< Custom shader's creation serial, 0 = none (local patch 17; was its pointer)
--- src/modules/r3d_render.c
+#include "./r3d_shader.h"      // r3d_shader_custom_t (shader_sort_key, local patch 17)
+static inline uintptr_t shader_sort_key(const struct R3D_ShaderCustom* shader)
+{
+    return shader ? (uintptr_t)shader->serial : 0;
+}
-        state->shader       = (uintptr_t)call->mesh.material.shader;
+        state->shader       = shader_sort_key(call->mesh.material.shader);
-        state->shader   = (uintptr_t)call->decal.instance.shader;
+        state->shader   = shader_sort_key(call->decal.instance.shader);
```

**Verification.** Determinism: `maps/monolith`, monolith `--shot` of views 3, 4, 7 15 runs each,
`examples/01_minimal` views 1 and 4 15 runs each, `examples/05_entities` 10 runs on each of the
three maps, a standalone program built against an `--export` of the map (its own R3D copy and
prebuilt libraries) views 4 and 7 10 runs each: one image per view every time (before: 2-6). A
sweep of 53 shots (every viewpoint, yaw 0-300 at two heights, `--reload-test`, all three maps)
run twice: identical. Against the build before the patch: `maps/pine_forest` and `tests/models`
pixel-identical (33 shots); `maps/monolith` 6 of 20 shots identical to one pre-patch run, 3 more
equal to an image the pre-patch build produced in some run, the other 11 differ from every
pre-patch image seen by 2-553 pixels (max 34 levels; the pre-patch build differed from itself by
up to 258 pixels / 21 levels): isolated pixels, invisible at 5x zoom. Both targets and the
examples build without warnings; `tools/leakcheck.sh`: zero leaks of ours.

**If R3D changes the sort:** keep every sort key free of addresses (pointers, `uintptr_t` of
heap objects); GL object ids and creation counters are reproducible.

## Patch 18: misspelled visibility enum (2026-10-25)

**Symptom.** The render module's visibility enum was spelled `R3D_RENDER_VISBILITY_*`, and its
comments were vague about what "visible" is relative to.

**Cause.** A typo upstream.

**Change.** Renamed `R3D_RENDER_VISBILITY_UNKNOWN / _FALSE / _TRUE` to
`R3D_RENDER_VISIBILITY_*` (all 15 uses), and reworded the three comments: the state is per
view (camera, shadow map or probe face) and is reset before each view's cull. A rename only:
the values and the generated code are unchanged. When refreshing from upstream, check whether
upstream fixed the spelling; if not, redo the rename with
`sed -i 's/R3D_RENDER_VISBILITY_/R3D_RENDER_VISIBILITY_/g' src/modules/r3d_render.[ch]`.

Files: `src/modules/r3d_render.h`, `src/modules/r3d_render.c`.

```diff
--- src/modules/r3d_render.h  (r3d_render_visibility_enum_t)
-    R3D_RENDER_VISBILITY_UNKNOWN = -1,    //< Visibility has not been evaluated yet
-    R3D_RENDER_VISBILITY_FALSE = 0,       //< Determined to be not visible (culled)
-    R3D_RENDER_VISBILITY_TRUE = 1,        //< Determined to be visible
+    R3D_RENDER_VISIBILITY_UNKNOWN = -1,   //< Not tested yet for the current view
+    R3D_RENDER_VISIBILITY_FALSE = 0,      //< Outside the view (culled)
+    R3D_RENDER_VISIBILITY_TRUE = 1,       //< Inside the view (drawn)
--- src/modules/r3d_render.c  (12 uses)
-R3D_RENDER_VISBILITY_
+R3D_RENDER_VISIBILITY_
```

**Verification.** No `VISBILITY` left in the tree; both targets build without warnings;
`tools/leakcheck.sh`: zero leaks of ours.

## Patch 19: framebuffer switches and clears that change nothing (2026-10-25)

**Symptom.** In the CPU profile of `maps/monolith` (2026-10-25, machine A) `glBindFramebuffer`
cost 0.13 ms per frame (22 binds; NVIDIA validates the FBO on each bind) and `glClearBuffer*`
0.03 ms (30 calls): 15–22% of the frame's CPU time.

**Cause.** (Found with a scratch build that logged every target bind and clear of one frame.)
1. The screen-space passes that depth-test against the depth pyramid (`pass_prepare_downsample`,
   `pass_prepare_ssao`, `pass_prepare_ssil`: 8 binds per frame at ultra) bind with
   `R3D_TARGET_BIND_CLEAR(level, true, ...)` after turning depth writes off and the stencil
   write mask to 0. `glClearBufferfi(GL_DEPTH_STENCIL)` honours both write masks, so those 8
   depth/stencil clears changed nothing.
2. `pass_prepare_pyramid` clears both colour attachments and the depth of each level that it then
   overwrites completely: a full-screen triangle, depth test `ALWAYS` with depth writes on, no
   blending, no `discard` (`shaders/prepare/pyramid.frag` writes both outputs and `gl_FragDepth`).
   2 colour clears and 1 depth clear per level, all overwritten.
3. The fog passes (`pass_deferred_fog`, the transmittance and compose steps of
   `pass_deferred_volumetric_fog`) bind the scene target *without* the depth attachment: another
   FBO than the one `pass_deferred_compose`, `pass_scene_background` and `pass_scene_forward`
   use around them (the scene target *with* depth). So the frame switched compose / background
   → fog → vfog radiance → fog → forward: 2 switches more than needed.

**Change.**
1. `r3d_target_bind_clear` skips the depth/stencil clear when the driver's cached state is known
   to have depth writes off and a zero stencil write mask (`r3d_driver_depth_stencil_writes_off()`,
   new; it returns false while that state is unknown, e.g. right after
   `r3d_driver_invalidate_cache()`, and the clear then happens as before).
2. The pyramid binds with `R3D_TARGET_BIND_LOAD` instead of `R3D_TARGET_BIND_CLEAR`.
3. The fog passes bind `R3D_TARGET_BIND_LOAD(0, true, sceneTarget)`. Their depth test is off,
   so the attached depth buffer is neither tested nor written, and none of them samples the
   hardware depth texture (they read the `R3D_TARGET_DEPTH` colour target), so attaching it
   makes no feedback loop.

Looked at and not done: the frame-start clear binds an 8-attachment FBO (G-buffer, specular,
scene) only to clear it, 1 switch; clearing the G-buffer through the geometry pass's FBO and the
other two where they are first bound would save it, but moves clears across `R3D_End`'s branches
(no visible lights, no opaque draws, the debug output modes) for one bind. The SSIL denoise
ping-pong (4 binds) and the bloom mip chain (10 re-attachments of one FBO) would need FBOs per
level or shared attachments; whether binding a cached FBO per mip is cheaper on NVIDIA than
re-attaching would have to be measured. `glViewport` (34 per frame) is set on every target bind;
caching it would need every other `glViewport` call in R3D (sky, cubemaps, probes, shadows,
blits) to invalidate the cache, for a cheap call.

Files: `src/modules/r3d_driver.h`, `src/modules/r3d_driver.c` (`r3d_driver_depth_stencil_writes_off`),
`src/modules/r3d_target.c` (`r3d_target_bind_clear`), `src/r3d_draw.c` (`pass_prepare_pyramid`,
`pass_deferred_fog`, `pass_deferred_volumetric_fog`).

```diff
--- src/modules/r3d_driver.c
+bool r3d_driver_depth_stencil_writes_off(void)
+{
+    const pipeline_cache_t* pipe = &R3D_MOD_DRIVER.pipeCache;
+    return pipe->depthMask.known && pipe->depthMask.vbool == GL_FALSE
+        && pipe->stencilMask.known && pipe->stencilMask.vbyte == 0;
+}
--- src/modules/r3d_target.c  (r3d_target_bind_clear; now includes "./r3d_driver.h")
-    if (depth)
+    if (depth && !r3d_driver_depth_stencil_writes_off())
     {
         glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
     }
--- src/r3d_draw.c
 pass_prepare_pyramid:
-        R3D_TARGET_BIND_CLEAR(iDst, true, R3D_TARGET_DEPTH, R3D_TARGET_SELECTOR);
+        R3D_TARGET_BIND_LOAD(iDst, true, R3D_TARGET_DEPTH, R3D_TARGET_SELECTOR);
 pass_deferred_fog, pass_deferred_volumetric_fog (transmittance and compose):
-    R3D_TARGET_BIND_LOAD(0, false, sceneTarget);
+    R3D_TARGET_BIND_LOAD(0, true, sceneTarget);
```

**Verification.** Call counts per frame (a scratch build that wraps R3D's GL calls and counts them; not kept), on every view counted
of the three maps: `glBindFramebuffer` 22 → 20 (`tests/models`: 20 → 18), `glClearBufferfv`
20 → 18 (19 → 17), `glClearBufferfi` 10 → 1; `glFramebufferTexture2D` (10) and `glViewport` (34)
unchanged. The bind log of one frame after the patch: compose, background, fog and vfog
transmittance on one FBO, vfog radiance, then vfog compose and forward on the first again.
Image: 90 files (every viewpoint of `maps/monolith`, `maps/pine_forest` and `tests/models`, yaw sweeps 0–300 at three heights, `--reload-test` both ways, `--off 16`, the steep view, picks, box / scene / entity dumps) against the build before: 0 differ. Builds
without warnings on Linux and MinGW; `tools/leakcheck.sh`: zero leaks of ours.

**Measured** (machine A of `.claude/skills/benchmarking/measurements.md`, `--bench --headless`, default map, 3 interleaved full pairs, on top of the dropped sort cache described there): *CPU until finish* −0.029 ms per frame (−5 %; pairs −0.016 / −0.023 / −0.049), *CPU in R3D_End* −0.026 ms; frame time −0.037 ms (the frame is GPU-bound). With patch 20, without the sort cache, 4 pairs: −0.025 ms *CPU until finish* for both.

**If R3D changes these passes:** a pass that binds with `BIND_CLEAR` and *wants* its depth
cleared must have depth writes on when it binds (as the frame-start clear does); the pyramid may
only stay without a clear while its draw covers every pixel without blending or `discard`; a
fog pass that turns the depth test on or samples `r3d_target_get_depth_buffer()` must go back to
the scene FBO without depth.

## Patch 20: geometry draws re-uploaded unchanged matrices and toggled depth / stencil state (2026-10-25)

**Symptom.** `raster_geometry` (`src/r3d_draw.c`) cost 0.31 ms of CPU per frame on
`maps/monolith` (CPU profile 2026-10-25, machine A), 0.019 ms of it the model-matrix uniform
alone. Counted per frame (view 2): 874 `glUniformMatrix4fv`, 351 `glDepthFunc`, 345
`glStencilFunc`.

**Cause.**
1. `R3D_SHADER__SET_MAT4_IMPL` uploaded every matrix on every call; unlike the scalar and vector
   setters it kept no copy of the last value. Each geometry draw uploads its model and normal
   matrix, and with the depth pre-pass (patch 6) each pre-passed draw comes twice. monolith's map
   geometry is instanced with the identity as group transform, so nearly all of these were the
   same matrix on the same program.
2. The normal matrix (an inverse transpose) was recomputed for every draw from the same transform.
3. In the pre-pass's shading pass (`GEOMETRY_RASTER_EQUAL`) `raster_geometry` first set the
   material's depth state (`LESS`) and stencil state (`ALWAYS`, ...), then overrode the depth
   function with `GL_EQUAL` and the stencil function with the tie-break test: 2 `glDepthFunc`
   and 2 `glStencilFunc` per draw that only flipped the state back and forth.

**Change.**
1. `r3d_shader_uniform_mat4_t` stores the last uploaded matrix; the setter uploads only when the
   new one differs bit for bit (`memcmp`). The cache starts zeroed with the program struct (GL's
   default uniform value is the zero matrix too), the same discipline as the other uniform
   caches; custom shader clones share the program struct and with it the caches. It applies to
   every matrix uniform in R3D (geometry, forward, unlit, decals, shadow depth, probes, sky):
   the uploaded values are unchanged, only repeats are skipped.
2. `raster_geometry` keeps the last transform and its normal matrix and recomputes only when the
   transform changes (a pure function of the transform, so the result is identical).
3. In `GEOMETRY_RASTER_EQUAL` mode only the material's depth range and offset are set, not its
   depth function or its stencil function and operations, which the code that follows sets
   anyway. The GL state at the draw is the same as before.

Looked at and not done: the pre-pass already draws in the shading pass's order, which the
material sort groups by surface shader (one program per shader), so sorting it "by program"
would drop no program switches; a `GL_LESS` pre-pass leaves the minimum depth whatever the order,
and the shading pass's order (the stencil tie-break of patch 6) is untouched. Each pre-passed
material keeps its own program in the pre-pass (patch 6: the same program in both passes keeps
the depths bit-identical). Leaving the material uniforms and textures out of the pre-pass draws
was tried and removed: a custom `vertex()` may read any material value (`COLOR`, `TEXCOORD`,
`EMISSION`, `MATRIX_NORMAL`; `maps/monolith` has surface shaders with vertex code), so they could
only be skipped for the built-in program, and with the uniform caches that saved 0 calls on every
tested view (the pre-passed draws there all use custom shaders; `maps/pine_forest` has no
pre-passed draws).

Files: `src/modules/r3d_shader.h` (`r3d_shader_uniform_mat4_t`, `R3D_SHADER__SET_MAT4_IMPL`,
`#include <string.h>`), `src/r3d_draw.c` (`raster_geometry`).

```diff
--- src/modules/r3d_shader.h
-typedef struct { int loc; } r3d_shader_uniform_mat4_t;
+typedef struct { Matrix val; int loc; } r3d_shader_uniform_mat4_t;
 #define R3D_SHADER__SET_MAT4_IMPL(target, uniform, value)                       \
 do {                                                                            \
-    glUniformMatrix4fv((target)->uniform.loc, 1, GL_TRUE, (float*)&(value));    \
+    const Matrix* r3d__m = &(value);                                            \
+    if (memcmp(&(target)->uniform.val, r3d__m, sizeof(Matrix)) != 0)            \
+    {                                                                           \
+        (target)->uniform.val = *r3d__m;                                        \
+        glUniformMatrix4fv((target)->uniform.loc, 1, GL_TRUE, (const float*)r3d__m); \
+    }                                                                           \
 } while (0)
--- src/r3d_draw.c  (raster_geometry)
-    Matrix matNormal = r3d_matrix_normal(&group->transform);
+    static Matrix normalFor, matNormal;
+    static bool normalKnown = false;
+    if (!normalKnown || memcmp(&normalFor, &group->transform, sizeof(Matrix)) != 0)
+    {
+        matNormal = r3d_matrix_normal(&group->transform);
+        normalFor = group->transform;
+        normalKnown = true;
+    }
     ...
     r3d_driver_set_cull_mode(material->cullMode);
-    r3d_driver_set_depth_state(material->depth);
-    r3d_driver_set_stencil_state(material->stencil);
+    if (mode == GEOMETRY_RASTER_EQUAL)
+    {
+        r3d_driver_set_depth_range(material->depth.rangeNear, material->depth.rangeFar);
+        r3d_driver_set_depth_offset(material->depth.offsetUnits, material->depth.offsetFactor);
+    }
+    else
+    {
+        r3d_driver_set_depth_state(material->depth);
+        r3d_driver_set_stencil_state(material->stencil);
+    }
```

**Verification.** Call counts per frame with a still camera (a scratch build that wraps R3D's GL calls and counts them; not kept):
`glUniformMatrix4fv` 798–1106 → 0 on the `maps/monolith` views counted (1, 2, 3, 7, the steep
view), 370–376 → 0 on `maps/pine_forest`, 28 → 0 on `tests/models` (a moving camera still uploads
the view-dependent ones: the sky's inverse view and projection, and a shadow map's matrices when
it is redrawn); `glDepthFunc` 321–467 → 13 and `glStencilFunc` 315–461 → 7 on `maps/monolith`
(unchanged on the other maps: no pre-passed draws); `glUseProgram`, draws and texture binds
unchanged. Image: the same 90 files against the build before: 0 differ,
and two runs of the patched build: 0 differ. Builds without warnings on Linux and MinGW;
`tools/leakcheck.sh`: zero leaks of ours.

**Measured** (as for patch 19, 3 interleaved full pairs on top of it): *CPU until finish* −0.027 ms per frame (pairs −0.034 / −0.024 / −0.022), *CPU in R3D_End* −0.028 ms. The profile's driver time under the geometry pass (with the runtime's shader aliases, which share one program per shader file): 0.148 → ~0.05 ms per frame.

**If R3D adds a path that sets a matrix uniform with `glUniformMatrix4fv` directly** (outside
`R3D_SHADER__SET_MAT4_IMPL`), or reloads a program without zeroing its struct, the matrix cache
must be reset there, as the other uniform caches would need.

## Patch 21: the omni shadow array reserved four cube layers for one light (2026-10-26)

**Symptom.** The first shadowed point light allocated 192 MiB of shadow maps: four cube layers
(4 × 6 faces × 2048² × 16-bit depth) while it uses one (48 MiB). Every map in `maps/` has one
shadowed point light, so 144 MiB sat unused.

**Cause.** `shadow_array_acquire_layer` grows an array by `arr->growth` layers when its free
list is empty, and upstream sets `SHADOW_OMNI_LAYER_GROWTH` to 4. For a 2D array (spot) a
layer is one face; for the cube array a layer is six, so the same growth costs six times as
much.

**Change.** Omni growth 4 → 1. Each further shadowed point light now adds one cube layer
(48 MiB at the default 2048) with one `shadow_array_expand` (new texture, copy of the existing
layers, old texture deleted). Monolith acquires all shadow layers at load before anything is
drawn, so the extra expansions happen once per load. A hot reload acquires the new map's layers
before the old map frees its own (`runtime/monolith_scene.c`, the swap at the end of `LoadBytes`),
so after a reload the array holds old + new layers (2 cubes, 96 MiB, for one light) and keeps
that size: arrays never shrink. That is still half of what one light cost before. DIR (growth 2,
used by the two sun tiles) and spot (growth 4, 8 MiB per layer) are unchanged.

Files: `src/modules/r3d_light.c`.

```diff
--- src/modules/r3d_light.c  (constants)
-#define SHADOW_OMNI_LAYER_GROWTH    4
+#define SHADOW_OMNI_LAYER_GROWTH    1   // local patch: one cube layer is 6 faces (48 MiB at 2048), so grow by one
```

**Verification.** Every map in `maps/` against the build before the change: all viewpoints,
`--reload-test` (which takes the expansion path: 1 → 2 cube layers), the yaw sweeps and steep
view from `maps/monolith/MAP.md`, and four yaws next to each map's shadowed point light:
43 files, 0 differ; `--dump-boxes` identical. Linux builds without warnings (the MinGW build was
not run: no cross compiler on that machine); `tools/leakcheck.sh`: zero leaks of ours. VRAM
figures are from the formats and sizes in the code, not measured.

## Patch 22: clip-boxed lights were culled and scissored by their whole sphere (2026-10-26)

**Symptom.** On an Intel HD Graphics 5500 (machine C of `.claude/skills/benchmarking/measurements.md`) the default map ran at ~5 fps looking at the tower from the terrace: the deferred lights pass took ~95 ms of a ~185 ms frame. The ~290 unshadowed flat lights (patch 16 clip boxes, one storey each) cost ~10 ns per pixel of their scissor rects, and those rects added up to ~4.7 screens.

**Cause.** `light_omni_push` / `light_spot_push` cull a light by its range sphere and set the scissor rect to the sphere's projection. The clip box (patch 16) was only tested in the shaders, so every pixel of a 14–16 m sphere's rect ran the lighting shader, although a storey's box is 4 m tall and the shader returns 0 outside it.

**Change.** After the clip box is set on a pushed point or spot light, `light_clip_bounds` intersects the sphere's AABB with the clip box, drops the light when that box is empty or outside the view frustum, and narrows the rect to the box's projected corners (intersected with the sphere's rect, padded by 0.01 NDC because `r3d_light_screen_rect` truncates; unchanged when a corner is behind the camera). The box is kept on the light data (`bounds`, `hasBounds`) for patch 23. Lights without a clip box and directional lights are untouched. The image can't change: outside the box and beyond `range` the deferred lighting, the forward pass and the fog march all give 0 (`r3d-pitfalls.md` notes it).

Files: `src/modules/r3d_light.c`, `src/modules/r3d_light.h`.

```diff
--- src/modules/r3d_light.h  (r3d_light_data_t)
     Vector3 clipMin, clipMax;   // World-space clip box, +-R3D_LIGHT_NO_CLIP without one (local patch 16)
+    BoundingBox bounds;         // Volume inside the clip box, if hasBounds (local patch 22)
+    bool hasBounds;
--- src/modules/r3d_light.c  (new, before r3d_light_push)
+static bool light_clip_bounds(r3d_light_data_t* data)
+{
+    // box = sphere AABB ∩ clip box; false if empty or outside R3D.viewState.frustum
+    // project its 8 corners with R3D.viewState.viewProj (keep the sphere rect if a corner has w <= 1e-4)
+    // minNdc/maxNdc = sphere rect ∩ (corner rect ± 0.01); false if that is empty
+}
--- src/modules/r3d_light.c  (r3d_light_push, after the clip box is set)
+        if (clip && light->type != R3D_LIGHT_DIR && !light_clip_bounds(data))
+        {
+            r3d_light_data_t culled;
+            R3D_LIST_POP(R3D_MOD_LIGHT.listLightData, &culled);
+        }
```

**Verification.** `maps/monolith`, `maps/pine_forest` and `tests/models` against the build before the change: every viewpoint, yaw sweeps (0–300°, pitch 0 and 25) from five positions around and inside the tower (terrace, beside it, inside a storey-1 flat, the lobby, high on the facade), a pine_forest yaw and `--reload-test`: 76 shots, 0 differ. Measured with patch 23 below.

## Patch 23: lights behind walls ran the lighting shader on the wall's pixels (2026-10-26)

**Symptom.** With patch 22 the terrace view of the tower still spent ~63 ms in the lights pass on machine C: from outside, most pixels of a storey light's rect are the facade in front of the room, which the shader rejects only after reading the G-buffer and writing zeros through the additive blend.

**Cause.** `pass_deferred_lights` draws every light as a full-screen quad with depth test `GL_GREATER` against the far plane: it only rejects the sky, not surfaces in front of the light's volume.

**Change.** When the camera is outside a light's patch-22 box (by more than 4 × the near plane, so the near plane can't clip its faces), `light_volume_mark` draws the box (the cube shape, `CUBEMAP_VERT` + `COLOR_FRAG`, new program `deferred.lightVolume`) with both faces, no colour writes and depth test `GL_LEQUAL`, setting stencil bit `GEOMETRY_PREPASS_STENCIL_BIT` (0x80, which the prepass leaves 0 between passes) where a face is not behind the scene: exactly the pixels whose surface is behind the box's front faces. The light's quad then runs with stencil `GL_EQUAL 0x80` and ops `KEEP / ZERO / ZERO` (write mask 0x80), so it shades only those pixels and clears the bit for the next light. Occluded pixels never start the lighting shader (early stencil). The box grows by 5 cm + 0.2 % of its distance so depth precision can't reject a surface lying on a face; the shader still decides exactly. Other lights draw as before (stencil off). The stencil write mask is restored after the pass (new `r3d_driver_get_stencil_mask`). Limitation: a material whose stencil `ref` sets bit 0x80 loses that bit under such a light's box (monolith's materials don't use stencil; the prepass already skips those materials).

Files: `src/r3d_draw.c`, `src/modules/r3d_shader.c`, `src/modules/r3d_shader.h`, `src/modules/r3d_driver.c`, `src/modules/r3d_driver.h`.

```diff
--- src/r3d_draw.c  (new, before pass_deferred_lights)
+#define LIGHT_VOLUME_BIT GEOMETRY_PREPASS_STENCIL_BIT
+static bool light_volume_mark(const r3d_light_data_t* light)
+{
+    // false without bounds or when the camera is within 4 near planes of the grown box
+    // deferred.lightVolume: uMatProj = proj, uMatView = scale(size) * translate(center) * view
+    // colour mask off; stencil ALWAYS ref/mask 0x80, ops KEEP/KEEP/REPLACE; depth LEQUAL; R3D_RENDER_CUBE()
+    // colour mask on; depth GREATER; stencil EQUAL 0x80, ops KEEP/ZERO/ZERO; back to deferred.lighting
+}
--- src/r3d_draw.c  (pass_deferred_lights)
+    uint8_t stencilMask;
+    bool stencilMaskKnown = r3d_driver_get_stencil_mask(&stencilMask);
 ...
-        // Accumulate this light!
+        // Accumulate this light! (only where its box marked the stencil, local patch 23)
+        if (!light_volume_mark(light)) r3d_driver_disable(GL_STENCIL_TEST);
         R3D_RENDER_SCREEN();
     }
+    r3d_driver_disable(GL_STENCIL_TEST);
+    if (stencilMaskKnown) r3d_driver_set_stencil_mask(stencilMask);
+    else r3d_driver_set_stencil_mask(0xFF);
--- src/modules/r3d_shader.h / .c
+typedef struct { GLuint id; r3d_shader_uniform_mat4_t uMatProj, uMatView; } r3d_shader_deferred_light_volume_t;
+// deferred.lightVolume: struct member, loader-table entry, r3d_shader_load_deferred_light_volume
+// (LOAD_SHADER(lightVolume, CUBEMAP_VERT, COLOR_FRAG), uMatProj, uMatView), UNLOAD_SHADER in r3d_shader_quit
--- src/modules/r3d_driver.h / .c
+bool r3d_driver_get_stencil_mask(uint8_t* mask);   // cached write mask; false when unknown
```

**Verification.** The 76 shots of patch 22 with both patches: 0 differ (the terrace and beside-the-tower views show lit flats through the windows, so the masked path runs; inside a flat it falls back). Linux build and `examples/build.sh` (+ `test`) without warnings; the MinGW build was not run (no cross compiler on machine C). `tools/leakcheck.sh`: zero leaks of ours in all 67 runs (`export-default` / `export-models` exit 1 before and after these patches: `runtime/export/` is missing from the checkout, see `.gitignore`'s `export/`).

**Measured** (machine C, `--bench --headless`, the developer's settings file, a copy of `maps/monolith` with only the Terrace and Tower viewpoints, 100 frames, 3 interleaved rounds of base / patch 22 / patches 22 + 23; medians, paired differences agree within a few ms):

| ms | base | + patch 22 | + patch 23 |
|---|---|---|---|
| lights, still, Terrace | 97.8 | 64.2 | 13.2 |
| lights, still, Tower | 55.9 | 27.1 | 10.1 |
| frame, still, Terrace | 204.9 | 169.1 | 117.8 |
| frame, still, Tower | 150.7 | 120.6 | 104.5 |
| lights, spinning, Terrace / Tower | 44.3 / 27.2 | | 7.8 / 6.6 |
| frame, spinning, Terrace / Tower | 129.0 / 107.1 | | 92.7 / 86.5 |
| CPU until finish, still | 16.1 | 14.8 | 26.6 |

The cost: patch 23 adds ~10 ms of CPU per still frame on machine C (~4 ms spinning) for ~130 extra draws, two program switches and stencil state changes per marked light; the frame stays GPU-bound there. With the tower's lights removed the lights pass is ~5–6 ms, so little is left to gain on the GPU side.

## Patch 24: three full-screen blends for the fogs and a depth copy nobody reads (2026-10-27)

**Symptom.** On an integrated GPU (machine C of `.claude/skills/benchmarking/measurements.md`: HD Graphics 5500, single-channel DDR3) the post-lighting chain is bandwidth-bound: each full-screen pass costs its bytes per pixel at ~10–12 GB/s. Distance fog, vfog transmittance and vfog composition were three passes of ~3.4 ms each at 1920 × 1006, each a read + write of the RGBA16F scene; the FXAA pass that writes the final target (patch 10) also copied the scene depth there, ~1.9 ms with the HiZ resolve the depth texture needs, in frames where nothing reads it.

**Cause.** `pass_deferred_fog`, the transmittance and the composition of `pass_deferred_volumetric_fog` each blend into the scene, one after the other, with nothing in between. `pass_post_fxaa_to_screen` always writes `gl_FragDepth`, like the copy blit it replaced, for applications that draw depth-tested content over the frame afterwards.

**Change.**
1. With the volumetric fog on, `vfog_compose.frag` computes all three terms (the code of `fog.frag` and `vfog_transmittance.frag`, unchanged, plus the upsampled radiance) and blends once with dual-source blending (GL 3.3 core): `FragColor = (fogColor f T + emission + radiance, f)`, `FragFactor = ((1 - f) T, 1 - f)`, blend `GL_ONE, GL_SRC1_COLOR` / `GL_ONE, GL_SRC1_ALPHA`, which is the three blends' result (alpha `f + a (1 - f)` as the fog blend gave). The radiance march now runs before it. Without volumetric fog the distance fog pass runs as before. `vfog_transmittance.frag` and the profiler slot "vfog transmit" are unused; the composition is reported as "fog + vfog compose" (and "fog" reads 0 with the volumetric fog on).
2. `R3D_SetOutputDepth(bool)` / `R3D_GetOutputDepth()` (default true: as before). With false, `pass_post_fxaa_to_screen` disables the depth test, binds the blank texture instead of the depth buffer and `fxaa.frag` (`uWriteDepth`) reads no depth. Only that path honours it; the blit paths still copy depth. monolith passes false when no overlay is drawn over the frame (walk mode, `--bench`, `--shot` without `--shot-hud`), true for the frame-reuse cache and request images.

Files: `shaders/deferred/vfog_compose.frag`, `shaders/post/fxaa.frag`, `src/r3d_draw.c`, `src/r3d_core.c`, `src/r3d_core_state.h`, `include/r3d/r3d_core.h`, `src/modules/r3d_shader.c`, `src/modules/r3d_shader.h`; monolith: `src/main.c` (`DrawScene(…, depth)`).

```diff
--- src/r3d_draw.c  (R3D_End)
-    if (R3D.environment.fog.mode != R3D_FOG_DISABLED) PROF_PASS(R3D_PROFILER_FOG, pass_deferred_fog(sceneTarget));
-    if (R3D.environment.volumetricFog.enabled) pass_deferred_volumetric_fog(sceneTarget);
+    if (R3D.environment.volumetricFog.enabled) pass_deferred_volumetric_fog(sceneTarget);
+    else if (R3D.environment.fog.mode != R3D_FOG_DISABLED) PROF_PASS(R3D_PROFILER_FOG, pass_deferred_fog(sceneTarget));
--- src/r3d_draw.c  (pass_deferred_volumetric_fog)
-    /* --- Apply transmittance --- */  (blend ONE, SRC_ALPHA / ZERO, ONE; deferred.vfogTransmittance; R3D_RENDER_SCREEN)
 ...
-    r3d_driver_set_blend_func_separate(GL_FUNC_ADD, GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
+    r3d_driver_set_blend_func_separate(GL_FUNC_ADD, GL_ONE, GL_SRC1_COLOR, GL_ONE, GL_SRC1_ALPHA);
--- src/r3d_draw.c  (pass_post_fxaa_to_screen)
+    if (R3D.outputDepth) { depth test on, ALWAYS, mask on } else { depth test off }
+    uDepthTex = R3D.outputDepth ? depth buffer : R3D_TEXTURE_BLANK;  uWriteDepth = R3D.outputDepth
--- src/r3d_draw.c  (PROF_PASS_NAMES)
-    [R3D_PROFILER_VFOG_COMPOSE]       = "vfog upsample",
+    [R3D_PROFILER_VFOG_COMPOSE]       = "fog + vfog compose",
--- shaders/deferred/vfog_compose.frag
+    // fog factor f (fog.frag), transmittance T and emission (vfog_transmittance.frag), radiance (as before)
+    layout(location = 0, index = 0) out vec4 FragColor;   layout(location = 0, index = 1) out vec4 FragFactor;
--- shaders/post/fxaa.frag
-    gl_FragDepth = texelFetch(uDepthTex, ivec2(gl_FragCoord.xy), 0).r;
+    gl_FragDepth = uWriteDepth ? texelFetch(uDepthTex, ivec2(gl_FragCoord.xy), 0).r : gl_FragCoord.z;
--- src/modules/r3d_shader.[ch]
+    vfogCompose: SET_UNIFORM_BUFFER ViewBlock, FxBlock;  fxaa: uWriteDepth (GET_LOCATION)
--- src/r3d_core.c / r3d_core_state.h / include/r3d/r3d_core.h
+    bool outputDepth (true in R3D_Init); R3D_GetOutputDepth / R3D_SetOutputDepth
```

**Verification.** Not pixel-identical (1, rounding level): the three blends rounded the scene to half floats in between, the fused one doesn't. Shots of every viewpoint of both maps against the build before (machine C): 5.2–8.5 % of the pixels differ, all but 11–266 per image by 1/255, the rest by up to 29/255 where FXAA's edge test flips on a single bright pixel. With the volumetric fog off (`--off 1`, distance fog only): 0 pixels differ, which also shows that 2 leaves the colour alone (that shot runs without the depth copy). `--shot-hud --select` draws the depth-tested selection overlay as before. Linux build and `examples/build.sh` (+ `test`) without warnings; MinGW not run (no cross compiler on machine C). `tools/leakcheck.sh`: zero leaks of ours in all 70 runs (`export-default` / `export-models` exit 1 before and after, `runtime/export/` missing from the checkout). `glslangValidator` is not installed on machine C: the shaders were checked by Mesa's compiler only.

**Measured.** Machine C (HD Graphics 5500, Mesa 25.0.7 iris, i5-5300U; the developer's settings file: FXAA, volumetric and distance fog on), `--bench --headless`, 1920 × 1006, 150 frames per view (screening runs), the build before vs this one. Balanced order with a cool-down below 70 °C before every run (after, before, before, after; reports `benchmark/2026-10-27_235953`, `2026-10-28_000244`, `_000545`, `_000834`):

| ms, all views | before (2 runs) | after (2 runs) | change |
|---|---|---|---|
| fog + vfog transmit + vfog upsample | 10.60 / 10.78 | — | |
| fog + vfog compose (fused) | — | 4.91 / 4.76 | **−5.9** |
| FXAA (to the target) | 5.50 / 5.55 | 2.46 / 2.34 | **−3.1** |
| other passes (same code) | | | within ±0.5, except vfog radiance (GPU clock, see below) |
| GPU total | 66.66 / 68.98 | 57.61 / 56.78 | −10.6 |
| frame | 75.40 / 79.33 | 66.57 / 67.71 | **−10.2 (−13%), 13.0 → 14.9 fps** |

The two changed passes save ~9 ms; the rest of the GPU total's −10.6 is the GPU clock (750 MHz median in the "after" runs, 600–650 in the "before" runs, which moves the ALU-bound vfog radiance by ~0.7 ms; the bandwidth-bound passes don't move). A first run of 3 interleaved pairs with the cool-down only before each pair put every "after" run on a hotter chip (CPU throttled from 2.29 to 1.1 GHz, CPU in `R3D_End` +3 ms), frame −8.2 / −4.5 / −4.9 ms with GPU total −11.1 / −8.5 / −8.3: interleave with a cool-down before every run on this machine. With the volumetric fog off (2 alone, 60 frames): FXAA 5.43 → 2.49 ms. At the developer's window size (1366 × 768, 54 % of the pixels) expect about half.
