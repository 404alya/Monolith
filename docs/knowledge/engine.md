# monolith: engine reference

Technology, layout, data paths, code architecture, the scene runtime and known limitations, for anyone (human or AI assistant) changing this project. How to change code (build, memory rules, verification, common modifications, conventions) is the `engine-code` skill (`.claude/skills/engine-code/SKILL.md`); controls, flags and environment variables are `cli.md`; the R3D pitfalls `r3d-pitfalls.md`; measurement history and the numbers behind the caches and trade-offs `docs/performance.md`. `CLAUDE.md` holds the guard rails, the hard rules and the index of skills.

## 1. What this is

**monolith** is an AI-assisted map designer: a first-person 3D viewer and editor in plain C on raylib + R3D. The developer walks or flies through a level; in spectator mode they point, select objects or screen regions and submit a request, which the game writes as a bundle and hands to Claude Code in a new terminal. The game hot-reloads the edited map package (the request workflow: the `map-request` skill).

Design principles:
- **The level is data.** A map package (`maps/<name>/`): the map file, its shaders, models, textures and a design brief (`MAP.md`). Nothing in the engine assumes a style.
- **One runtime for editor and games.** `runtime/monolith_scene.h/.c` (§4.1) loads, draws, hot-reloads, collides with and picks a package, in the editor and in any raylib + R3D program (`--export`, `examples/`), so a scene can't drift between the two.
- **Assets are traceable.** Recorded source and licence for every external file; models are restyled through material overrides, never by editing the file.
- **Plain C on a lightweight renderer.**

Reference maps: `maps/monolith/` (the default: brutalist concrete, boxes only, one procedural shader) and `maps/pine_forest/` (stylised low-poly forest at dusk, restyled CC0 models, a texture, point lights). Engine changes keep the default map pixel-identical.

## 2. Technology

| Item | Details |
|---|---|
| Language | C11 (`-std=c11 -Wall -Wextra`): `src/*.c` (§4), `runtime/monolith_scene.c` (§4.1), the Linux shim `src/glibc_compat.c` |
| Windowing / input | raylib 5.5 (bundled with R3D, static) |
| Renderer | **R3D** 0.10.x (Bigfoot71/r3d): deferred PBR; shadows, SSAO, SSIL, SSGI, SSR, fog, volumetric fog, bloom, tonemapping, procedural sky, IBL, instancing, surface shaders |
| Assimp | R3D's model importer (glTF/GLB, OBJ, FBX, DAE); `--inspect-model` calls it directly |
| OpenGL | 3.3 core. `libGL` / `opengl32` linked only for `glEnable` (`MS_InitRenderer`, `r3d-pitfalls.md` §2), `glFinish` (`--headless`) and `glGetString` (`bench.c`), declared by hand |
| Platform | Linux (X11/GLFW); Windows x86_64 cross-compiled with MinGW-w64, tested under wine |
| Dev machine | CachyOS, NVIDIA RTX 4050 Laptop GPU, driver 615.71 |

**R3D** is vendored as a plain copy in **`submodules/r3d`** (with raylib and assimp). Headers `include/r3d/`, shader docs `docs/shaders/*.md`, examples `examples/`. Check its source rather than guessing. Paths like `shaders/deferred/…` or `src/modules/…` in this file are relative to `submodules/r3d`.

> **R3D changes are patches.** Work around R3D from the app side where reasonable. When R3D itself must change, change it minimally and **record every change in `submodules/r3d/patches.md`** (symptom, cause, files, diff). Don't modify `~/Projects/r3d` or other projects in `~/Projects`.

## 3. Layout

```
monolith/
├── build.sh, cmake/mingw-w64-x86_64.cmake, .clangd, .gitignore
├── CLAUDE.md                    # always loaded: guard rails, hard rules, index of skills
├── .claude/skills/<topic>/SKILL.md   # workflows (map-request, map-editing, engine-code, ...), loaded when the task matches
├── .claude/commands/            # slash-command templates (/task-prompt)
├── docs/                        # design-v2.md, design-coordinator.md (design records), performance.md (measurements), QA-REPORT.md
│   └── knowledge/               # reference read on demand (index: README.md): engine (this file), cli, r3d-pitfalls,
│                                #   map-format, request-bundle, export, history
├── maps/                        # map packages (docs/knowledge/map-format.md), copied next to the executable by build.sh
│   ├── monolith/                # default map: monolith.map, MAP.md, ASSETS.md, shaders/concrete.glsl, ...
│   └── pine_forest/             # pine_forest.map, MAP.md, ASSETS.md, models/ (+ sidecars), textures/
├── runtime/                     # scene runtime (§4.1): monolith_scene.h/.c, monolith_capture.h (§4.3),
│                                #   export/ (viewer.c + build.sh templates for --export)
├── tools/                       # fetch_asset.py, normalize_model.py, record_asset.py (`assets-and-licences` skill);
│                                #   leakcheck.sh + lsan.supp + lsan_report.py (`leak-check` skill); request-queue.sh (the coordinator's queue);
│                                #   stub-terminal.sh, stub-coordinator.sh, test-coordinator.sh (launcher tests)
├── examples/                    # 01_minimal (+ scene/), 02_existing_project, 03_live_editing, 04_import_source,
│                                #   common/shot.h, build.sh (§4.3)
├── tests/models/                # small CC0 test package: every statement kind once
├── export/, requests/, benchmark/   # gitignored: F9 exports, request bundles, --bench reports
├── src/                         # the editor, one module per concern (§4)
│   ├── main.c                   # header comment (controls + flags), args, data paths, init, main loop, Submit
│   ├── common.h                 # shared includes, the global `MS_Scene *scene`, benchmarkDir
│   ├── player.c, spectator.c    # walking player + collision; spectator camera, LookDir/LookAngles
│   ├── designer.c, launcher.c   # picking, selection, overlay, HUD, prompt box; bundles, queue + coordinator launch, polling
│   ├── atmosphere.c             # --ao / --bloom / --fx overrides on top of the map's environment
│   ├── newmap.c, export.c, import.c, dirscan.c   # --new-map, --export / F9, --import, safe folder walk
│   ├── inspect.c, dumpscene.c   # --inspect-model, --dump-scene
│   ├── bench.c, windowemu.c     # --bench report; --bench-window stand-in
│   ├── settings.c, settingspanel.c   # graphics settings + file; F2 panel
│   └── glibc_compat.c           # Linux only: old glibc symbol versions (`engine-code` skill)
└── submodules/r3d/              # vendored R3D; local fixes in patches.md
```

**Data paths.** `InitDataPaths()` (`main.c`) sets `benchmarkDir` and the maps directory. On Linux the repo directories baked in (`-DBENCHMARK_PATH`, `-DMAPS_PATH`) win while they exist, so on the dev machine the game loads and hot-reloads the **repo's** `maps/`, not the copy in `build/`. Otherwise (and on Windows) the folders next to the executable are used. `build.sh` copies `maps/` and `runtime/` (+ `viewer.c`, for `--export`) into both output folders. `launcher.c` puts `requests/` and starts Claude Code in the repo root (`-DPROJECT_PATH`) while it exists, else (always on Windows) in the executable's directory.

## 4. Code architecture (`runtime/`, `src/*.c`)

The scene (loading, drawing, collision, picking, hot reload) is the **scene runtime** (§4.1). The editor in `src/` is split into modules with a header each; `build.sh` compiles `src/*.c` and `runtime/*.c`, so a new module needs no build change. Larger files use `// #region` blocks.

1. **Scene runtime**: the game holds one scene in the global `MS_Scene *scene` (`common.h`).
2. **Player** (`player.c`): feet-based AABB (r 0.3, h 1.75, eye 1.62), per-axis movement against `scene->colliders` (boxes, then model colliders) with the strict **`BoxesOverlap`** (like `MS_Collides`, `r3d-pitfalls.md` §4); step-up (`STEP_HEIGHT` 0.45), gravity, jump, fly, smoothing, head bob. Starts at `spawn` unless `--pos` / `--eye` / `--look`.
3. **Spectator** (`spectator.c`): state, the map's viewpoints (`scene->viewpoints`), `UpdateSpectator()`, `SpectatorFlyTo()` / `FlyToPose()` / `JumpTo()`, `SpectatorCamera()`. `LookDir(yaw, pitch) = (cos p · sin y, sin p, cos p · cos y)` (yaw 0 → +Z); right = `(−cos y, 0, sin y)`.
4. **Designer** (`designer.c`), one `Designer` state owned by `main()`:
   - **Picking**: `DesignerPick()` → `MS_Raycast()`: slab test against every box, then models (bounds, then triangles through a BVH). Boxes containing the eye are skipped. Refreshed every frame in spectator mode and `--shot` runs.
   - **Selection**: `MS_ObjRef` heap arrays with an index → position table; `DesignerSync()` remaps after a reload through a hash of exact bounds.
   - **Screen regions**: at submit `RegionPick` samples each with up to 4096 rays (per-region candidate lists, culled per 8×8-ray tile): objects with ray shares (also per map line), centre hit, depth range, sky share, and **edge objects** (`AnalyzeRegionEdge`: up to 64 rays per side on a ring just outside the rect, nearest first, with sides).
   - **Overlay**: `DesignerDrawOverlay()` draws with raylib after `R3D_End()` in `BeginMode3D`, depth-tested against R3D's depth (copied into the target by `R3D_End`) with a 4-step depth bias in the projection and slightly inflated boxes (models: oriented bounds). Also crosshair, snip drawing, HUD.
   - **Prompt box**: UTF-8, word wrap, scrolling, heap text; `DesignerCapturesInput()` freezes game keys and camera.
   - `DesignerBuildRequest()` snapshots prompt, eye, angles, fovy, size, hit, selection, regions, map version into a `DesignRequest`.
5. **Launcher** (`launcher.c`, see its header comment):
   - `LauncherSubmit()` writes `requests/<YYYY-MM-DD_HHMMSS>_<slug>/` (`view.png`, `view-marked.png`, `region-<n>.png`, `request.md`, `request.json`, format 2: `request-bundle.md`), then (coordinator mode) `state` = `queued` and, unless `requests/.coordinator/lock` is held (`flock`), starts the coordinator's terminal (a `/bin/sh` wrapper holds the lock around `claude`; region "Coordinator and queue", `docs/design-coordinator.md`), or (per-request mode) the request's own terminal. Terminals start with `fork` + `execve` from an argv array, no shell: the user's text never reaches a command line. The child `setsid`s, `chdir`s to the repo root, stdin `/dev/null`; exec failures return through a close-on-exec pipe; children are reaped with `waitpid(WNOHANG)`.
   - Terminal syntax (title, working dir, exec flag) from `TERMINALS[]`; unknown terminals get `-e`. Title `monolith-claude: <slug> <HHMMSS>[-n]`, unique per request. `claude` is looked up in `PATH`, then `~/.local/bin`, passed absolute, with the permission flags (unless `MONOLITH_CLAUDE_SAFE`), `--model <tier's model>` and `MONOLITH_CLAUDE_ARGS` (a bare `--model` there is dropped with a warning). A detached helper moves and marks the window. On launch failure the bundle stays, the HUD names it and the log prints the command.
   - `LauncherUpdate()` polls open requests for `state` and `result.md` (HUD 20 s) and tests the coordinator's lock every 2 s; `LauncherLastDone()` gives the camera for **R**.
   - **Queue**: after `queued` only the coordinator writes `state`, through `tools/request-queue.sh` (claim with a fingerprint in `claim.txt`, question, finish, requeue, recovery after a crash, the background watcher `wait`); the game asks for a cancel with a `cancel` file. Test: `tools/test-coordinator.sh` (stub terminal + `tools/stub-coordinator.sh`, which applies the same scheduling rules).
   - **Windows**: `cmd /c start "<title>" /D <exe dir> cmd /k "<claude>" … "<prompt>"` via `_spawnvp` (the coordinator's with ` 9>"<lock>"`, tested with an exclusive `_sopen_s`); compile-tested only.
6. **Atmosphere** (`atmosphere.c`): the environment is map data (`env` lines; defaults in `BuiltinEnvironment()`, applied by `MS_ApplyEnvironment`). Graphics settings (`MS_ApplyQuality`, incl. `--ao` / `--bloom`) go on top, then `--fx`; `main.c`'s `ApplyEnvironment()` re-applies environment → quality → `--fx` → toggle flips → `MS_ApplyRenderResolution` after every reload.
7. **Graphics settings** (`settings.c`, `settingspanel.c`): `GfxSettings` = `MS_Quality` + window options (MSAA, vsync, fps cap, frame reuse).
8. **New map, export, import, inspection, dumps** (`newmap.c`, `export.c`, `import.c`, `inspect.c`, `dumpscene.c`): §4.3, `cli.md`.
9. **Benchmark** (`bench.c`, `windowemu.c`): statistics, the Markdown report, the `--bench-window` stand-in.
10. **Main** (`main.c`):
    - header comment, argument parsing, `InitDataPaths()`, the map's `noGpu` parse check **before any window**, and the windowless modes;
    - shadow-size hint (`MS_SetInitHints`), `R3D_Init`, `MS_InitRenderer()` (`GL_DEPTH_CLAMP`, `r3d-pitfalls.md` §2), `MS_Load`, `MS_SetRenderSettings` (shadow mode, fog cache, pre-pass, render scale, upscale, AO volume), `MS_ApplyRenderResolution` (also on resize);
    - settings (file, then `--quality` and single flags, then `--settings-set`); `RestartRenderer()` for a new shadow map size (`MS_Unload`, `R3D_Close`, hint, `R3D_Init`, `MS_Load`);
    - main loop: toggles, TAB, settings panel, `MS_Poll`, designer, camera, draw, overlay, HUD, `--shot` / `--bench`;
    - **depth pre-pass** (patch 6): materials with `alpha_cutoff 0` (never discard) get depth laid first, so their surface shader runs once per visible pixel;
    - **frame reuse** (comment block "Frame reuse"): a `FrameKey` (camera, the whole `R3D_Environment`, `MS_RenderSettings` incl. toggle 6, graphics settings, AA mode, `scene->version`, window size) is compared bytewise each frame. While it changes, R3D renders into the window; on the first unchanged frame it renders once more into a cache target (colour + depth *texture*); later unchanged frames composite that with a shader writing `gl_FragDepth` (a blit into a multisampled window would be an error), overlay and HUD on top (~0.2 ms), and the loop sleeps to the refresh rate. Interactive only. **Anything new that changes the image (a uniform, a light, a setting) must go into `FrameKey`**, or a still camera keeps showing the old image;
    - `Submit()` renders the request images through the same `DrawScene()` as the main view.

Rendering per frame (`DrawScene()`, the same as `MS_Draw`; shots and request images render into a `RenderTexture`):
```c
R3D_BeginPro((R3D_View){ .camera = R3D_CameraFromRL(camera), .target = target });
    MS_PushLights(scene, camera, aspect);
        // sun: tile = SunShadowTileSelect(..., camera.position, scene->version, &update);
        //      R3D_PushLightFixedShadow(sun, sunShadow[tile], tiles.tile[tile].viewProj, update); // once per map load
        //      (--shadow-camera: update = SunShadowNeedsUpdate(...); R3D_PushLightEx(sun, sunShadow[0], update))
        // then the map's point / spot / dir lights
    MS_DrawGeometry(scene);    // per 32 m cell: R3D cluster + R3D_DrawMeshInstancedEx (boxes, per material: the faces no box hides as one
                               // mesh, or unit cubes with `instanced on`) / R3D_DrawModelInstancedEx (per model file, slot materials,
                               // shadow flag); fog / AO bakes once per load
R3D_End();
// then on the same target: DesignerDrawOverlay (depth-tested), crosshair, HUD, regions, prompt box, settings panel
```

### 4.1 Scene runtime (`runtime/monolith_scene.h` + `runtime/monolith_scene.c`)

A separately compilable C11 module depending only on raylib (raymath, rlgl) and R3D. `monolith_scene.h` is the API contract (read its comments); the header comment of `monolith_scene.c` is the map format reference.

**Two builds of one source.** monolith, the examples and the capture shim compile it with `-DMONOLITH_EDITOR`: hot reload, source lines, `brief` / `hash` / `mtime`, the dependency list, the environment writers, `MS_CheckModelReferences`, `MS_EnvKeyRange` and the `sunOn` toggle live in `#ifdef MONOLITH_EDITOR` blocks. `--export` writes the file without those blocks (`StripEditor` in `src/export.c`), so a game gets only loading, drawing, quality presets and queries. Rules for runtime changes: tool-only code goes into such a block (never nested; `#ifndef MONOLITH_EDITOR … #endif` for game-only code, `#else` allowed); comments outside the blocks must not mention the editor, requests, Claude Code, hot reload or monolith's command-line flags, because they ship; the runtime must build with zero warnings with and without `MONOLITH_EDITOR` (and without `MS_PATCHED_R3D`).

**What it owns** (all freed by `MS_Unload`):
- the parsed package: boxes (`scene->boxes`, `boxRefs[]`: source line, statement index, group, material), `modelFiles[]`, `models[]` (transform, world bounds, collide mode, slot overrides), colliders (boxes first), materials (`[0]` = `"default"`), groups, lights, sun, spawn, viewpoints, bounds, path, package root, a **`version`** that goes up on every successful (re)load (static caches key on it); editor builds also the map text (`MS_GetLine` / `MS_LineText`), `brief`, FNV-1a hash and mtime;
- GPU: one surface shader instance per material (uniforms are global per shader, `r3d-pitfalls.md` §4), textures (`wrap repeat`, trilinear + mipmaps), per material and 32 m cell, the faces of its boxes that no opaque box hides as one static mesh, or with `instanced on` one instance buffer of unit cubes (`r3d-pitfalls.md` §4), each a clustered draw (frustum culling); one instanced draw per (model file, slot materials, shadow flag), split into one clustered draw per cell, the visible sky and the ambient map (`r3d-pitfalls.md` §5);
- the sun shadow (`MS_SHADOW_TILES`, or `MS_SHADOW_CAMERA` / `MS_SHADOW_ALWAYS`), extra lights' shadow maps, the fog cache and AO volume, all placed by the map's bounds (`r3d-pitfalls.md` §3);
- picking (`MS_Raycast`, `MS_RaycastModel`: BVH over a CPU copy of the triangles), collision (`MS_Collides`, strict), hot reload (`MS_Poll`, editor builds).

**API:**

| Group | Functions |
|---|---|
| Loading | `MS_Load(path, &MS_LoadOptions{ noGpu, shadowSizeOverride, shadowRangeOverride }, err, size)`, `MS_Unload`, `MS_SetInitHints` (before `R3D_Init`), `MS_InitRenderer` (after) |
| Hot reload, source lines (editor) | `MS_Poll(scene, dt)` → `MS_UNCHANGED` / `MS_RELOADED` / `MS_RELOAD_FAILED`, `MS_Reload`, `MS_Error`, `MS_GetLine`, `MS_LineText` |
| Environment and drawing | `MS_ApplyEnvironment`, `MS_Get/SetRenderSettings`, `MS_QualityPreset` / `Name` / `FromName` / `Match` / `Equal`, `MS_ApplyQuality`, `MS_ApplyRenderResolution`, `MS_Draw`, `MS_PushLights` + `MS_DrawGeometry` (inside your own `R3D_Begin` … `R3D_End`) |
| Queries | `MS_Raycast`, `MS_RaycastModel`, `MS_Collides`, `MS_ObjectRef`, `MS_ObjectBounds` |
| Package files, writers (editor) | `MS_DependencyCount` / `MS_Dependency` (watched files), `MS_WriteEnvironment`, `MS_WriteR3DEnvironment` (any `R3D_Environment` as `env` lines), `MS_CheckModelReferences`, `MS_EnvKeyRange` |
| Memory | `MS_ArrayReserve`, `MS_RESERVE`, `MS_PUSH`, `MS_StrDup`, `MS_StrNDup` |

**Call order:** `InitWindow`; optionally a `noGpu` peek load + `MS_SetInitHints` + `MS_Unload`; `R3D_Init`; `MS_InitRenderer`; `MS_Load`; `MS_ApplyEnvironment` (+ `MS_ApplyQuality`) + `MS_ApplyRenderResolution`; per frame `MS_Poll` (re-apply on `MS_RELOADED`) and `MS_Draw`; `MS_Unload` before `R3D_Close`. Complete version: `examples/01_minimal/main.c`.

**Loading and hot reload.**
- `MS_Load` / `MS_Reload` parse and upload into a new scene and swap it in **only if the whole package is valid**. Errors are `file:line: message`; the loader never crashes on bad input. Every referenced file must exist, also for `noGpu` loads (`--dump-boxes` is a full package check; model bounds and colliders are final only in GPU loads).
- `MS_Poll` checks every dependency's mtime about twice a second; after a change it keeps re-reading for 2 s (1 s mtime resolution, editors writing in steps) and reloads when the combined content hash differs from the last attempt. The watch list is the last *attempt*'s, so a missing file is picked up when it appears. A reload creates every new GPU resource **before** freeing the old ones (`r3d-pitfalls.md` §4), and all static caches re-key on `scene->version`. `shadow.size` only changes with a restart.

**Patched vs. upstream R3D (`MS_PATCHED_R3D`).** With `-DMS_PATCHED_R3D` (set by `build.sh` and `examples/build.sh`) the runtime uses the patched APIs (`R3D_PushLightFixedShadow`, `R3D_SetDepthPrepass`, the fog bake, the occlusion volume, the profiler); without it, it falls back to `R3D_PushLightEx` with the camera-fitted map (`MS_SHADOW_TILES` → `MS_SHADOW_CAMERA`), no fog cache or AO volume. The export README lists the image differences (patch 4 visible when turning, 7/9 rounding level, patch 1 needed for volumetric fog on AMD / Intel). The upstream build is compile-checked only.

### 4.2 Surface shaders in map packages

A material's `shader "shaders/x.glsl"` is an R3D **surface shader** (`#pragma usage opaque shadow`, `void fragment()`), loaded per material from the package and hot-reloaded; `uniform` / `sampler` keys set its uniforms. Built-ins: `submodules/r3d/docs/shaders/surface_shader.md` (`ALBEDO`, `ROUGHNESS`, `METALNESS`, `SPECULAR`, `EMISSION`, `NORMAL_MAP`, `POSITION`, `NORMAL`, …). Boxes have per-face 0–1 UVs, so a pattern that should look the same on every box size uses world-space `POSITION` / `NORMAL` (like `concrete.glsl`). Opaque shaders that never discard want `alpha_cutoff 0` (pre-pass). Shaders that read `INSTANCE_*` or `POSITION` in `vertex()` need `instanced on` on their material (`r3d-pitfalls.md` §4). Check every shader with `glslangValidator` (`r3d-pitfalls.md` §4).

### 4.3 Export, import, examples

- **Export** (`src/export.c`, `--export <dir> [--map …]`, F9 = the open map): a self-contained folder that builds with one command. Copies the runtime's dependency list plus `MAP.md`, `ASSETS.md`, sidecars, licence files and the files model files refer to (`.gltf` URIs, `.obj` → `.mtl` → maps); the map byte for byte as `<name>.map`. Adds the runtime without its `MONOLITH_EDITOR` blocks, `viewer.c` and `build.sh` from `runtime/export/` (`@MAP@` = the map's name), the R3D tree build.sh built against (`R3D_SOURCE_PATH`; only `R3D_KEEP` minus `R3D_SKIP`, no dot files: ~33 MB, raylib and assimp included), the static libraries of `submodules/r3d/build-linux` and `build-mingw` as `prebuilt/<linux|win>/lib/` when their last build is current (`AddPrebuilt`; the package's `build.sh` links them and falls back to a source build), `cmake/mingw-w64-x86_64.cmake`, `LICENSES/` and a README (bundled build first, own R3D as the alternative, patched vs. upstream, quality presets, depth clamp, licences + the `ASSETS.md` table). Checks every source first (nothing is written if one is missing), writes `<dir>.partial`, loads the copy, renames it. Needs the repo. Changing `runtime/export/viewer.c` / `build.sh` or what R3D's build reads: export both test maps, build them from a copy outside the repo and compare viewpoint shots with `monolith --shot --view n` (`export.md`).
- **Import** (`src/import.c`): `NewMap()`, a bundle written like the launcher's, `LauncherStartBundle()` (model from `LauncherChooseModel`, `kind = "import"`). The session follows the `import-request` skill.
- **Capture shim** (`runtime/monolith_capture.h`, `-include`d into a program): routes `R3D_LoadModel`, `R3D_GenMeshCube`, `R3D_GenProceduralSky`, `R3D_LoadCubemap`, `R3D_LoadSurfaceShader`, `R3D_Begin*`, `R3D_PushLight*`, `R3D_DrawMesh*`, `R3D_DrawModel*`, `R3D_End` through recorders; with `MONOLITH_CAPTURE=<dir>` writes a package from one frame plus `CAPTURE.md` (what it couldn't translate). Limits: only generated cubes and loaded models are read back, rotated cubes become AABBs, uniforms and material changes on loaded models are only reported. A cross-check, not a replacement for reading the source.
- **Examples** (`examples/build.sh [--win]` → `examples/build/<linux|win>/<example>/`): `01_minimal`, `02_existing_project` (`before.c` / `after.c`), `03_live_editing` (`MS_Poll`, `--reload`), `04_import_source` (import test input; also `import_source_capture`). Same flags and static linking as the game, `-DMS_PATCHED_R3D -DMONOLITH_EDITOR`; each takes `--shot <file>` (hidden window `monolith-shot`, `examples/common/shot.h`).

## 5. Known limitations and next steps

- **Map format:** boxes are axis-aligned with per-face 0–1 UVs (textures stretch); only models rotate; models are static (no animation or skins); instances are rebuilt on every reload; code changes and `shadow.size` need a restart (or the panel's renderer restart).
- **Models:** picking hits triangles, collision uses AABBs; `noGpu` loads don't read model files (bounds and `origin` shifts final only in GPU loads); FBX / DAE references aren't followed for hot reload or export; the AO volume ignores models.
- **Interactive input** (clicks, drags, typing, paste, Esc, the panel, camera freezing) is tested only by hand.
- **Windows launcher** is compile-tested only; `start` failures go undetected; workspace moves and closing are i3 / sway only. On Windows and in copied build folders, `requests/` and Claude Code's working directory are the executable's folder (no sources, no `CLAUDE.md`).
- **Request flow:** coordinator mode: a bundle stays queued when the coordinator can't start (the HUD says why; the next submit tries again), and the prompt box closes. Per-request mode: a failed launch keeps the bundle and the text; Enter again writes a second bundle (trash the stale one). The coordinator itself (the `coordinator` skill) has only been tested through the stub; its HUD can't show other game runs' requests. A terminal process exiting early (kitty `--single-instance`, wezterm mux) is reported, not a failure; closing then needs the i3 / sway mark.
- **Upstream R3D** builds are compile-checked only.
- **Ideas:** decals (`r3d_decal.h`); height-dependent fog for hazy outdoor views; shadow cascades and cheaper fog lookups (`docs/performance.md`).
- **Platform:** Linux + Windows cross-build (tested under wine). The strict-GLSL fixes (`r3d-pitfalls.md` §4) are verified with `glslangValidator` but not yet on the AMD PC. No native Windows build script, no macOS, no Windows sanitizers.
