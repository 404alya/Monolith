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
