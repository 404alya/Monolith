---
name: shaders
description: Any GLSL in monolith — which kinds of shader exist (R3D's engine shaders in submodules/r3d/shaders, map surface shaders, the editor's raylib shader), how each is built, injected with defines and compiled, how to validate them with glslangValidator (check_surface_shader.py, check_r3d_shaders.py), compile cost and variants, uniform/UBO rules, and how to verify a shader change (pixel-identical shots, per-pass GPU cost). Use before changing or adding any shader, when a shader fails to compile on some GPU, or when a shader's cost or look is in question.
---

# Shaders

## 1. The kinds of shader

| Kind | Where | Built and compiled | Rules |
|---|---|---|---|
| **R3D engine shaders** (G-buffer, lights, SSAO/SSIL, fog, post) | `submodules/r3d/shaders/` (`scene/`, `deferred/`, `prepare/`, `post/`, `blit/`, `generic/`; shared code in `include/`) | Embedded at build time (CMake `embed_shaders`; `scripts/glsl_processor.py` resolves `#include <…>` from `include/` and minifies). `src/modules/r3d_shader.c` adds `#define` lines after `#version` per program and variant, and compiles them at init or on first use. **A change needs a rebuild** (`./build.sh`), not a hot reload | the `engine-code` skill; every change is an R3D patch recorded in `submodules/r3d/patches.md`; pixel-identical or a trade-off the developer approves |
| **Surface shaders** (a material's `shader` key) | `maps/<name>/shaders/*.glsl`, also `tests/models/shaders/`, `examples/*/scene/shaders/` | R3D's parser (`src/common/r3d_rshade.h`) moves the uniforms into a block, turns `varying` into `out`/`in`, and puts the shared code plus `vertex()` / `fragment()` into the templates `include/user/scene.vert` / `scene.frag` of each program (geometry, pre-pass, shadow, forward, probe). The runtime compiles each file once and gives every material an alias of it. Hot-reloaded | the `surface-shaders` skill (rules and the per-pixel cost budget) |
| **Editor shader** | `COMPOSITE_FS` in `src/main.c` (frame reuse: the cached colour and depth composited under the HUD) | a C string loaded with raylib's `LoadShaderFromMemory` | the `engine-code` skill |

R3D's sky and screen shaders (`R3D_LoadSkyShader`, `R3D_LoadScreenShader`) aren't used by monolith. The SMAA and FXAA headers in `shaders/external/` are third-party code: don't edit them.

## 2. Validate every changed shader

`CLAUDE.md`: shaders pass `glslangValidator`. NVIDIA accepts GLSL that AMD and Intel reject, so a shader that renders here can still fail on another GPU. Use the two checkers in this folder; both exit 1 on a failure and print the errors with the source's line numbers.

```bash
python3 -I .claude/skills/shaders/check_surface_shader.py maps/<name>/shaders/*.glsl   # surface shaders
python3 -I .claude/skills/shaders/check_r3d_shaders.py                                  # all of R3D (or: deferred/lighting.frag …)
```

- **`check_surface_shader.py`** rebuilds what R3D's parser makes of the file: the vertex stage, the fragment stage and the fragment stage with `DEPTH` defined (the depth / pre-pass variant). Each gets stub declarations of its stage's built-ins, typed as in R3D's templates (`TANGENT` is `vec4` in `vertex()`, `vec3` in `fragment()`). It catches syntax and type errors, built-ins used in the wrong stage, code that breaks only under `#ifdef DEPTH`, and built-in names such as `noise3`.
- **`check_r3d_shaders.py`** resolves includes with R3D's own `glsl_processor.py` and compiles each shader once per define set R3D uses for it (`QUALITY_PRESET` 0–3, the scene programs' stage defines, the UBO limits from `CMakeLists.txt`). If `r3d_shader.c` gains a define or a variant, update its `VARIANTS` table.
- **The editor's shader:** extract the string and run `glslangValidator -S frag` on it.
- **Pitfall:** `glslangValidator -D…` only works together with `-l` (linking). Without it the tool prints a usage error that isn't an `ERROR:` line. The checkers put `#define` lines into the source instead, as R3D does, and treat any non-zero exit as a failure.
- **Portability rules:** no names of GLSL built-ins (`noise1`–`noise4`, `length`, `distance`, …) for functions, variables or block members (R3D patch 1 renamed a `length` member). Details: `.claude/skills/lighting-fog-shadows/r3d-pitfalls.md` §4.

## 3. Uniforms, blocks and variants

- **std140 blocks are matched to their C mirror by layout, not by name.** A member renamed or added in `shaders/include/ubo/*.glsl` needs the same change in the C struct (`src/modules/r3d_shader.h`) in the same position.
- **Defines switch code paths:** R3D compiles a template several times with different defines (`MAX_LIGHTS_FORWARD` switches `ubo/light.glsl` from the single light to the forward light array; `DEPTH`, `GEOMETRY_PREPASS`, `PROBE`, … select the scene program). A change under one `#if` must be checked in every variant: `check_r3d_shaders.py` does that.
- **Surface-shader uniforms are per frame:** the last value set in a frame wins (`r3d-pitfalls.md` §4).
- **Compile cost:** variants are compiled on first use, so a new variant shows up as a slower first frame (`monolith_scene: first frames after the load a + b ms`). NVIDIA's shader disk cache hides most of it: measure with `__GL_SHADER_DISK_CACHE=0` (the `benchmarking` skill, CPU profiling). Each surface shader file is compiled once per load, whatever the number of materials using it.

## 4. Verifying a shader change

- **The look:** engine shader changes keep every map in `maps/` pixel-identical (`--shot` diffs of every viewpoint, yaw sweeps, `--reload-test`; the `engine-code` skill §3) or are presented as a trade-off with before/after shots. Nothing cached may depend on the view (`r3d-pitfalls.md` §6): check by turning the camera (yaw sweep), not only by moving it.
- **Isolating an effect:** `--off <digits>` turns off volumetric fog, fog, SSAO, SSIL, bloom or the sun; for a surface shader, force `ALBEDO` to a constant in a scratch copy of the package to see which stage of the shader makes a pixel dark (the `lighting-fog-shadows` skill).
- **The cost:** the frame is fill-bound, so per-pixel work in full-screen passes and in large materials is what counts. Read the bench report's per-pass GPU table before and after (`--bench --headless`, interleaved A/B pairs, the `benchmarking` skill). For single draws, the `benchmarking` skill's nsys section. An example of what one heavy shader costs: the `surface-shaders` skill, §2.
- **Both platforms:** `./build.sh` and `./build.sh --win` with zero warnings after an R3D shader change; the Windows build runs the same GLSL on the host driver under wine, so it doesn't replace `glslangValidator` for AMD / Intel.
- **No leak check for map-only shader edits** (the `leak-check` skill); R3D and editor shaders are code: `tools/leakcheck.sh`.
