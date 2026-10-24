# Architecture

How monolith is built, for anyone reading or changing the code: the technology, the folder layout, the modules, the scene runtime, the request queue, and the known limitations. Keys and flags: [user-guide.md](user-guide.md), [command-line.md](command-line.md); the map format: [map-format.md](map-format.md). "R3D pitfall N" refers to the engine lessons in `.claude/skills/lighting-fog-shadows/r3d-pitfalls.md`; measurements are in `.claude/skills/benchmarking/measurements.md`.

## 1. What this is

**monolith** is an AI-assisted map designer: a first-person 3D viewer and editor in plain C on raylib + R3D. The developer walks or flies through a level; in spectator mode they point, select objects or screen regions and submit a request, which the game writes as a bundle and hands to Claude Code in a new terminal. The game hot-reloads the edited map package ([user-guide.md](user-guide.md)).

Design principles:
- **The level is data.** A map package (`maps/<name>/`): the map file, its shaders, models, textures and a design brief (`MAP.md`). Nothing in the engine assumes a style.
- **One runtime for editor and games.** `runtime/monolith_scene.h/.c` (§4.1) loads, draws, hot-reloads, collides with and picks a package, in the editor and in any raylib + R3D program (`--export`, `examples/`), so a scene can't drift between the two.
- **Assets are traceable.** Recorded source and licence for every external file; models are restyled through material overrides, never by editing the file.
- **Plain C on a lightweight renderer.**

Reference maps: `maps/monolith/` (the default: brutalist concrete, boxes only, one procedural shader) and `maps/pine_forest/` (stylised low-poly forest at dusk, restyled CC0 models, a texture, point lights). Engine changes keep every map pixel-identical.

## 2. Technology

| Item | Details |
|---|---|
| Language | C11 (`-std=c11 -Wall -Wextra`): `src/*.c` (§4), `runtime/monolith_scene.c` (§4.1), the Linux shim `src/glibc_compat.c` |
| Windowing / input | raylib 5.5 (bundled with R3D, static) |
| Renderer | **R3D** 0.10.x (Bigfoot71/r3d): deferred PBR; shadows, SSAO, SSIL, SSGI, SSR, fog, volumetric fog, bloom, tonemapping, procedural sky, IBL, instancing, surface shaders |
| Assimp | R3D's model importer (glTF/GLB, OBJ, FBX, DAE); `--inspect-model` calls it directly |
| OpenGL | 3.3 core. `libGL` / `opengl32` linked only for `glEnable` (`MS_InitRenderer`, R3D pitfall 2), `glFinish` (`--headless`) and `glGetString` (`bench.c`), declared by hand |
| Platform | Linux (X11/GLFW); Windows x86_64 cross-compiled with MinGW-w64, tested under wine |
| Dev machine | CachyOS, NVIDIA RTX 4050 Laptop GPU, driver 615.71 |

**R3D** is vendored as a plain copy in **`submodules/r3d`** (with raylib and assimp). Headers `include/r3d/`, shader docs `docs/shaders/*.md`, examples `examples/`. Check its source rather than guessing. Paths like `shaders/deferred/…` or `src/modules/…` in this file are relative to `submodules/r3d`.

> **R3D changes are patches.** Work around R3D from the app side where reasonable. When R3D itself must change, change it minimally and **record every change in `submodules/r3d/patches.md`** (symptom, cause, files, diff). Don't modify `~/Projects/r3d` or other projects in `~/Projects`.

## 3. Layout

```
monolith/
├── build.sh, cmake/mingw-w64-x86_64.cmake, .clangd, .gitignore
├── README.md
├── CLAUDE.md                    # Claude Code: guard rails, hard rules, index of skills (always loaded)
├── .claude/skills/<topic>/      # Claude Code: workflows and lessons (map-request, engine-code, ...), loaded per task
├── .claude/commands/            # Claude Code: slash commands (/task-prompt)
├── docs/                        # this documentation: user guide, command line, map format, export, architecture
├── maps/                        # map packages (docs/map-format.md), copied next to the executable by build.sh
│   ├── monolith/                # default map: monolith.map, MAP.md, ASSETS.md, shaders/concrete.glsl, ...
│   └── pine_forest/             # pine_forest.map, MAP.md, ASSETS.md, models/ (+ sidecars), textures/
├── runtime/                     # scene runtime (§4.1): monolith_scene.h/.c, monolith_capture.h (§4.3),
│                                #   export/ (viewer.c + build.sh templates for --export)
├── tools/                       # fetch_asset.py, normalize_model.py, record_asset.py (models and licences);
│                                #   leakcheck.sh + lsan.supp + lsan_report.py (build and run); request-queue.sh (the coordinator's queue);
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
│   └── glibc_compat.c           # Linux only: old glibc symbol versions (build and run)
└── submodules/r3d/              # vendored R3D; local fixes in patches.md
```

**Data paths.** `InitDataPaths()` (`main.c`) sets `benchmarkDir` and the maps directory. On Linux the repo directories baked in (`-DBENCHMARK_PATH`, `-DMAPS_PATH`) win while they exist, so on the dev machine the game loads and hot-reloads the **repo's** `maps/`, not the copy in `build/`. Otherwise (and on Windows) the folders next to the executable are used. `build.sh` copies `maps/` and `runtime/` (+ `viewer.c`, for `--export`) into both output folders. `launcher.c` puts `requests/` and starts Claude Code in the repo root (`-DPROJECT_PATH`) while it exists, else (always on Windows) in the executable's directory.

## 4. Code architecture (`runtime/`, `src/*.c`)

The scene (loading, drawing, collision, picking, hot reload) is the **scene runtime** (§4.1). The editor in `src/` is split into modules with a header each; `build.sh` compiles `src/*.c` and `runtime/*.c`, so a new module needs no build change. Larger files use `// #region` blocks.

1. **Scene runtime**: the game holds one scene in the global `MS_Scene *scene` (`common.h`).
2. **Player** (`player.c`): feet-based AABB (r 0.3, h 1.75, eye 1.62), per-axis movement against `scene->colliders` (boxes, then model colliders) with the strict **`BoxesOverlap`** (like `MS_Collides`, R3D pitfall 4); step-up (`STEP_HEIGHT` 0.45), gravity, jump, fly, smoothing, head bob. Starts at `spawn` unless `--pos` / `--eye` / `--look`.
3. **Spectator** (`spectator.c`): state, the map's viewpoints (`scene->viewpoints`), `UpdateSpectator()`, `SpectatorFlyTo()` / `FlyToPose()` / `JumpTo()`, `SpectatorCamera()`. `LookDir(yaw, pitch) = (cos p · sin y, sin p, cos p · cos y)` (yaw 0 → +Z); right = `(−cos y, 0, sin y)`.
4. **Designer** (`designer.c`), one `Designer` state owned by `main()`:
   - **Picking**: `DesignerPick()` → `MS_Raycast()`: slab test against every box, then models (bounds, then triangles through a BVH). Boxes containing the eye are skipped. Refreshed every frame in spectator mode and `--shot` runs.
   - **Selection**: `MS_ObjRef` heap arrays with an index → position table; `DesignerSync()` remaps after a reload through a hash of exact bounds.
   - **Screen regions**: at submit `RegionPick` samples each with up to 4096 rays (per-region candidate lists, culled per 8×8-ray tile): objects with ray shares (also per map line), centre hit, depth range, sky share, and **edge objects** (`AnalyzeRegionEdge`: up to 64 rays per side on a ring just outside the rect, nearest first, with sides).
   - **Overlay**: `DesignerDrawOverlay()` draws with raylib after `R3D_End()` in `BeginMode3D`, depth-tested against R3D's depth (copied into the target by `R3D_End`) with a 4-step depth bias in the projection and slightly inflated boxes (models: oriented bounds). Also crosshair, snip drawing, HUD.
   - **Prompt box**: UTF-8, word wrap, scrolling, heap text; `DesignerCapturesInput()` freezes game keys and camera.
   - `DesignerBuildRequest()` snapshots prompt, eye, angles, fovy, size, hit, selection, regions, map version into a `DesignRequest`.
5. **Launcher** (`launcher.c`, see its header comment):
   - `LauncherSubmit()` writes `requests/<YYYY-MM-DD_HHMMSS>_<slug>/` (`view.png`, `view-marked.png`, `region-<n>.png`, `request.md`, `request.json`, format 2), then (coordinator mode) `state` = `queued` and, unless `requests/.coordinator/lock` is held (`flock`), starts the coordinator's terminal (a `/bin/sh` wrapper holds the lock around `claude`; region "Coordinator and queue", `docs/architecture.md` §5), or (per-request mode) the request's own terminal. Terminals start with `fork` + `execve` from an argv array, no shell: the user's text never reaches a command line. The child `setsid`s, `chdir`s to the repo root, stdin `/dev/null`; exec failures return through a close-on-exec pipe; children are reaped with `waitpid(WNOHANG)`.
   - Terminal syntax (title, working dir, exec flag) from `TERMINALS[]`; unknown terminals get `-e`. Title `monolith-claude: <slug> <HHMMSS>[-n]`, unique per request. `claude` is looked up in `PATH`, then `~/.local/bin`, passed absolute, with the permission flags (unless `MONOLITH_CLAUDE_SAFE`), `--model <tier's model>` and `MONOLITH_CLAUDE_ARGS` (a bare `--model` there is dropped with a warning). A detached helper moves and marks the window. On launch failure the bundle stays, the HUD names it and the log prints the command.
   - `LauncherUpdate()` polls open requests for `state` and `result.md` (HUD 20 s) and tests the coordinator's lock every 2 s; `LauncherLastDone()` gives the camera for **R**.
   - **Queue**: after `queued` only the coordinator writes `state`, through `tools/request-queue.sh` (claim with a fingerprint in `claim.txt`, question, finish, requeue, recovery after a crash, the background watcher `wait`); the game asks for a cancel with a `cancel` file. Test: `tools/test-coordinator.sh` (stub terminal + `tools/stub-coordinator.sh`, which applies the same scheduling rules).
   - **Windows**: `cmd /c start "<title>" /D <exe dir> cmd /k "<claude>" … "<prompt>"` via `_spawnvp` (the coordinator's with ` 9>"<lock>"`, tested with an exclusive `_sopen_s`); compile-tested only.
6. **Atmosphere** (`atmosphere.c`): the environment is map data (`env` lines; defaults in `BuiltinEnvironment()`, applied by `MS_ApplyEnvironment`). Graphics settings (`MS_ApplyQuality`, incl. `--ao` / `--bloom`) go on top, then `--fx`; `main.c`'s `ApplyEnvironment()` re-applies environment → quality → `--fx` → toggle flips → `MS_ApplyRenderResolution` after every reload.
7. **Graphics settings** (`settings.c`, `settingspanel.c`): `GfxSettings` = `MS_Quality` + window options (MSAA, vsync, fps cap, frame reuse).
8. **New map, export, import, inspection, dumps** (`newmap.c`, `export.c`, `import.c`, `inspect.c`, `dumpscene.c`): §4.3, [command-line.md](command-line.md).
9. **Benchmark** (`bench.c`, `windowemu.c`): statistics, the Markdown report, the `--bench-window` stand-in.
10. **Main** (`main.c`):
    - header comment, argument parsing, `InitDataPaths()`, the map's `noGpu` parse check **before any window**, and the windowless modes;
    - shadow-size hint (`MS_SetInitHints`), `R3D_Init`, `MS_InitRenderer()` (`GL_DEPTH_CLAMP`, R3D pitfall 2), `MS_Load`, `MS_SetRenderSettings` (shadow mode, fog cache, pre-pass, render scale, upscale, AO volume), `MS_ApplyRenderResolution` (also on resize);
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
- GPU: one surface shader instance per material (uniforms are global per shader, R3D pitfall 4), textures (`wrap repeat`, trilinear + mipmaps), per material and 32 m cell, the faces of its boxes that no opaque box hides as one static mesh, or with `instanced on` one instance buffer of unit cubes (R3D pitfall 4), each a clustered draw (frustum culling); one instanced draw per (model file, slot materials, shadow flag), split into one clustered draw per cell, the visible sky and the ambient map (R3D pitfall 5);
- the sun shadow (`MS_SHADOW_TILES`, or `MS_SHADOW_CAMERA` / `MS_SHADOW_ALWAYS`), extra lights' shadow maps, the fog cache and AO volume, all placed by the map's bounds (R3D pitfall 3);
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
- `MS_Poll` checks every dependency's mtime about twice a second; after a change it keeps re-reading for 2 s (1 s mtime resolution, editors writing in steps) and reloads when the combined content hash differs from the last attempt. The watch list is the last *attempt*'s, so a missing file is picked up when it appears. A reload creates every new GPU resource **before** freeing the old ones (R3D pitfall 4), and all static caches re-key on `scene->version`. `shadow.size` only changes with a restart.

**Patched vs. upstream R3D (`MS_PATCHED_R3D`).** With `-DMS_PATCHED_R3D` (set by `build.sh` and `examples/build.sh`) the runtime uses the patched APIs (`R3D_PushLightFixedShadow`, `R3D_SetDepthPrepass`, the fog bake, the occlusion volume, the profiler); without it, it falls back to `R3D_PushLightEx` with the camera-fitted map (`MS_SHADOW_TILES` → `MS_SHADOW_CAMERA`), no fog cache or AO volume. The export README lists the image differences (patch 4 visible when turning, 7/9 rounding level, patch 1 needed for volumetric fog on AMD / Intel). The upstream build is compile-checked only.

### 4.2 Surface shaders in map packages

A material's `shader "shaders/x.glsl"` is an R3D **surface shader** (`#pragma usage opaque shadow`, `void fragment()`), loaded per material from the package and hot-reloaded; `uniform` / `sampler` keys set its uniforms. Built-ins: `submodules/r3d/docs/shaders/surface_shader.md` (`ALBEDO`, `ROUGHNESS`, `METALNESS`, `SPECULAR`, `EMISSION`, `NORMAL_MAP`, `POSITION`, `NORMAL`, …). Boxes have per-face 0–1 UVs, so a pattern that should look the same on every box size uses world-space `POSITION` / `NORMAL` (like `concrete.glsl`). Opaque shaders that never discard want `alpha_cutoff 0` (pre-pass). Shaders that read `INSTANCE_*` or `POSITION` in `vertex()` need `instanced on` on their material (R3D pitfall 4). Check every shader with `glslangValidator` (R3D pitfall 4).

### 4.3 Export, import, examples

- **Export** (`src/export.c`, `--export <dir> [--map …]`, F9 = the open map): a self-contained folder that builds with one command. Copies the runtime's dependency list plus `MAP.md`, `ASSETS.md`, sidecars, licence files and the files model files refer to (`.gltf` URIs, `.obj` → `.mtl` → maps); the map byte for byte as `<name>.map`. Adds the runtime without its `MONOLITH_EDITOR` blocks, `viewer.c` and `build.sh` from `runtime/export/` (`@MAP@` = the map's name), the R3D tree build.sh built against (`R3D_SOURCE_PATH`; only `R3D_KEEP` minus `R3D_SKIP`, no dot files: ~33 MB, raylib and assimp included), the static libraries of `submodules/r3d/build-linux` and `build-mingw` as `prebuilt/<linux|win>/lib/` when their last build is current (`AddPrebuilt`; the package's `build.sh` links them and falls back to a source build), `cmake/mingw-w64-x86_64.cmake`, `LICENSES/` and a README (bundled build first, own R3D as the alternative, patched vs. upstream, quality presets, depth clamp, licences + the `ASSETS.md` table). Checks every source first (nothing is written if one is missing), writes `<dir>.partial`, loads the copy, renames it. Needs the repo. Changing `runtime/export/viewer.c` / `build.sh` or what R3D's build reads: export both test maps, build them from a copy outside the repo and compare viewpoint shots with `monolith --shot --view n` ([export.md](export.md)).
- **Import** (`src/import.c`): `NewMap()`, a bundle written like the launcher's, `LauncherStartBundle()` (model from `LauncherChooseModel`, `kind = "import"`). Claude follows its import workflow (`.claude/skills/import-request`).
- **Capture shim** (`runtime/monolith_capture.h`, `-include`d into a program): routes `R3D_LoadModel`, `R3D_GenMeshCube`, `R3D_GenProceduralSky`, `R3D_LoadCubemap`, `R3D_LoadSurfaceShader`, `R3D_Begin*`, `R3D_PushLight*`, `R3D_DrawMesh*`, `R3D_DrawModel*`, `R3D_End` through recorders; with `MONOLITH_CAPTURE=<dir>` writes a package from one frame plus `CAPTURE.md` (what it couldn't translate). Limits: only generated cubes and loaded models are read back, rotated cubes become AABBs, uniforms and material changes on loaded models are only reported. A cross-check, not a replacement for reading the source.
- **Examples** (`examples/build.sh [--win]` → `examples/build/<linux|win>/<example>/`): `01_minimal`, `02_existing_project` (`before.c` / `after.c`), `03_live_editing` (`MS_Poll`, `--reload`), `04_import_source` (import test input; also `import_source_capture`). Same flags and static linking as the game, `-DMS_PATCHED_R3D -DMONOLITH_EDITOR`; each takes `--shot <file>` (hidden window `monolith-shot`, `examples/common/shot.h`).

### 4.4 Editor details

- **Selection** survives reloads: objects are matched by identical geometry (models: world bounds), then by map line + index within the line + group; lost ones are reported.
- **Prompt box:** UTF-8 editing, Ctrl+Backspace deletes a word, arrows, Home / End (Ctrl: the whole text), Ctrl+V, Shift+Enter newline. 1 MB cap with a byte counter. Game keys and the camera stay frozen while it is open (+2 frames). On a failed submit the box keeps the text and the HUD shows the error and the bundle path.
- **Graphics settings** (`settings.c`, `settingspanel.c`, the runtime's `MS_Quality`): every scene setting is relative to the map's environment; toggles 1–6 and `--fx` apply on top. Everything that changes the image is in `FrameKey` (the whole `GfxSettings` + R3D's AA mode). Below the map's `shadow.size` the sun's depth and slope biases are scaled by the texel ratio (`SunShadowMap()`) so low suns get no acne. In your own program: `MS_QualityPreset(level)`, `MS_ApplyQuality(scene, &q)` after every `MS_ApplyEnvironment`, then `MS_ApplyRenderResolution`; `q.shadowSize` goes to `MS_LoadOptions.shadowSizeOverride` (`examples/01_minimal --quality medium` renders the same pixels as `monolith --quality medium`).

## 5. Request queue and coordinator

Since 2026-10-24 every request goes through a queue on disk, worked through by one long-lived Claude Code session, the **coordinator**, which hands each request to a subagent. `MONOLITH_LAUNCH=per-request` restores the older behaviour (one terminal and session per request). The coordinator's own instructions are `.claude/skills/coordinator/SKILL.md`.

### 5.1 Parts

| Part | Where | Job |
|---|---|---|
| Game (submit, `--import`) | `src/launcher.c` | writes the bundle, then `state` = `queued` as its last file; starts the coordinator terminal if none holds the lock; shows the queue in the HUD |
| Coordinator lock wrapper | `src/launcher.c` (an `/bin/sh -c` script; Windows: a `9>` redirection) | holds `requests/.coordinator/lock` while the coordinator's `claude` runs |
| Queue tool | `tools/request-queue.sh` | every state change the coordinator makes (claim, finish, requeue, recover), the watcher (`wait`), the queue listing (`list`) |
| Coordinator | `claude` started by the game with `.claude/skills/coordinator/SKILL.md` | schedules, starts subagents, relays questions, finishes bundles. Reads only `request.json` metadata and the queue listing; never the images, never edits maps |
| Subagent | Agent tool inside the coordinator | does one request under `.claude/skills/map-request/SKILL.md`, returns a one-line status |
| Test doubles | `tools/stub-terminal.sh` (`MONOLITH_STUB_RUN`), `tools/stub-coordinator.sh`, `tools/test-coordinator.sh` | the same scheduling rules in shell, no Claude session |

### 5.2 Files on disk

**`requests/<id>/state`**: one line, `<state> <YYYY-MM-DDTHH:MM:SS> [detail]`. Readers use the first word. Written whole (temp file + rename where possible).

| State | Written by | Meaning |
|---|---|---|
| `queued` | game / `--import` | the bundle is complete and waits for the coordinator. **A bundle without `state` is never picked up** (older bundles, per-request mode, a bundle still being written) |
| `claimed` | `request-queue.sh claim` | a subagent has it. `claim.txt` next to it holds the fingerprint taken at the claim (§5.4) |
| `question` | `request-queue.sh set <id> question` | the subagent asked something (`question.md`, line 1 = the question); the coordinator asks the developer in its terminal. Back to `claimed` when answered |
| `done` | `request-queue.sh finish <id> done` | the subagent finished; `result.md` exists (required) |
| `failed` | `finish <id> failed` / `recover` | not done; `result.md` says why (written by the tool when the subagent left none) |
| `cancelled` | `finish <id> cancelled`; the game, only when no coordinator holds the lock | the developer cancelled it; `result.md` says whether anything had changed |

Transitions: `queued → claimed → (question ↔ claimed) → done | failed | cancelled`, and `claimed → queued` (requeue after a restart). After `queued` **only the coordinator writes `state`**: the game asks for a cancel by creating **`requests/<id>/cancel`** (one line, the time), so game and coordinator never race on the same file. The single exception: when no coordinator holds the lock, the game turns a `queued` bundle with a cancel straight into `cancelled` (+ `result.md`) itself, since nobody else can.

**`requests/.coordinator/`** (dot folder: invisible to `ls`-based tools such as the leak check's cleanup):
- `lock`: held (Linux: `flock(2)`) by the wrapper around the coordinator's `claude` for as long as it runs. "Is a coordinator running?" = "is the lock held?"; the kernel drops it when the process dies, however it dies.
- `seen`: the events the watcher has reported in this coordinator's life (`queued <id>`, `cancel <id>`); `start` empties it.
- `log`: append-only, one line per event: `<time with ms> <event> <id> [package]`. The tests read their timestamps from it; the developer can `tail -f` it.

### 5.3 Handoff and waking

The game can't type into a running session, so the handoff is the file system:
1. The game writes the bundle, then `state` (`queued`).
2. If the lock is free (and no coordinator was started by this game in the last 20 s), it opens **one** terminal titled `monolith-claude: coordinator` running `claude [permission flags] --model <coordinator model> [MONOLITH_CLAUDE_ARGS] "<fixed prompt>"`, wrapped so the wrapper holds the lock; a second wrapper that finds the lock taken (two quick submits) waits 2 s for it, says so and exits. The fixed prompt names `.claude/skills/coordinator/SKILL.md` and the requests folder, never user text.
3. The coordinator keeps **`tools/request-queue.sh wait`** running in the background (Bash `run_in_background`). It polls the folder once a second in shell (no tokens) and **exits** when a `queued` bundle or a `cancel` file appears that it has not reported yet; it prints the events and the queue listing. The exit wakes the coordinator through the harness. Subagent completions wake it the same way. An idle queue costs nothing.
4. On every wake the coordinator first re-arms the watcher, then acts. Because `seen` records what was reported, a bundle queued between the watcher's exit and its restart still fires the new watcher.

### 5.4 Recovery

- A dead coordinator releases the lock; the next submit starts a new one.
- On startup the coordinator runs `request-queue.sh start`: empties `seen`, **recovers**, prints the queue. Only one coordinator can hold the lock, so every `claimed` / `question` bundle without `result.md` belongs to a dead one.
- **Fingerprint** (`claim.txt`, taken at the claim): `sha1sum` of every file in the request's map package, and one hash of the repo's code state (`git status --porcelain` + `git diff HEAD`, outside `maps/` and `requests/`).
  - Nothing changed since the claim: back to `queued` (it runs again from the start).
  - Something changed: `failed`, with `result.md` = `Interrupted: the coordinator stopped mid-run; files changed, check them` plus the list of changed files. It is **reported, not re-run**: a half-applied edit run a second time would apply twice. The coordinator also tells the developer in its terminal.
- Attribution is sound because of the scheduling rules: while a request is claimed nobody else may change its package, and the code hash only changes under a code request, which runs alone.

### 5.5 Scheduling

A queued request starts (oldest first) only when no other running request uses the same map package, no code request is running or waiting (code requests share `build/`, the leak check and the GPU, so they run alone), and fewer than 3 subagents run (one GPU renders every `--shot`). A request that turns out to need code stops before changing any, and resumes once it runs alone.

#### Models

- The game chooses the tier per request (`LauncherChooseModel`) and records it in `request.json` `model` (`tier`, `name`). The coordinator passes it as the subagent's `model`: a name containing `haiku` / `sonnet` / `opus` / `fable` maps to that alias; anything else means "omit `model`" (the subagent inherits the coordinator's). `request-queue.sh list` prints the mapped value (`agent-model=`).
- `@fast` / `@default` / `@heavy` (and `@haiku` / `@sonnet` / `@opus`), `MONOLITH_MODEL` and `MONOLITH_MODEL_FAST/_DEFAULT/_HEAVY` decide `model.name`.
- The coordinator runs on one fixed model: **`MONOLITH_COORDINATOR_MODEL`**, default `sonnet` (scheduling and relaying need little). `MONOLITH_CLAUDE_ARGS` go on the coordinator's command line; a `--model` in them sets the coordinator's model and, as before, every request's `model.name`.

### 5.6 Closing and cancelling

A cancel writes `requests/<id>/cancel`; the coordinator stops the subagent and the request's `result.md` lists what had changed. With no coordinator running the game cancels a queued request itself. On i3 / sway the game marks the coordinator's terminal `_monolith_coordinator` (per-request mode: each terminal `_monolith_<id>`) and closes it with `i3-msg '[con_mark=…] kill'` (that window only, no focus change); if the process is still alive 3 s later or there is no mark, it sends SIGTERM to the process group (started with `setsid`) and SIGKILL after 3 s, only while it is the game's own child. kitty `--single-instance` and wezterm mux exit at once, so only the i3 route reaches them. Windows: the entry only leaves the list. `MONOLITH_CLOSE_ON_EXIT=1` closes the coordinator when the game quits if none of its requests is open.

- `MONOLITH_LAUNCH=per-request` restores the old behaviour exactly (no `state` file, one terminal per request, the old K keys). Default `coordinator`. Without `flock` (util-linux) on Linux the game falls back to per-request and logs why.
- Windows: the same queue. The coordinator starts through the existing `cmd /c start ... cmd /k` path with ` 9>"<requests>\.coordinator\lock"` appended: cmd keeps the file open while `claude` runs; the game tests the lock by opening it with `_SH_DENYRW` (fails while cmd holds it). The scripts run in Claude Code's Git Bash. Compile-tested only; closing the window from the game is not possible there (as before).

### 5.7 Tests

- `MONOLITH_REQUESTS_DIR` (tests only) moves `requests/` (bundles, queue, lock) elsewhere, so tests never meet a real coordinator and the leak check no longer trashes real bundles queued during its run (the old hazard: its cleanup trashed every new folder in `requests/`).
- `MONOLITH_CLAUDE` replaces the `claude` program (tests: `tools/stub-coordinator.sh`).
- `tools/stub-terminal.sh` with `MONOLITH_STUB_RUN=<log>` runs the command after `-e` like a terminal would (no window) and appends one line per "window" to `$MONOLITH_STUB_OPENED`.
- `tools/test-coordinator.sh` proves: three submits open one terminal; same-package requests never overlap, different packages overlap; a code request runs alone; killing the coordinator mid-run requeues untouched requests and reports touched ones on the next start.

## 6. Known limitations and next steps

- **Map format:** boxes are axis-aligned with per-face 0–1 UVs (textures stretch); only models rotate; models are static (no animation or skins); instances are rebuilt on every reload; code changes and `shadow.size` need a restart (or the panel's renderer restart).
- **Models:** picking hits triangles, collision uses AABBs; `noGpu` loads don't read model files (bounds and `origin` shifts final only in GPU loads); FBX / DAE references aren't followed for hot reload or export; the AO volume ignores models.
- **Interactive input** (clicks, drags, typing, paste, Esc, the panel, camera freezing) is tested only by hand.
- **Windows launcher** is compile-tested only; `start` failures go undetected; workspace moves and closing are i3 / sway only. On Windows and in copied build folders, `requests/` and Claude Code's working directory are the executable's folder (no sources, no `CLAUDE.md`).
- **Request flow:** coordinator mode: a bundle stays queued when the coordinator can't start (the HUD says why; the next submit tries again), and the prompt box closes. Per-request mode: a failed launch keeps the bundle and the text; Enter again writes a second bundle (trash the stale one). The coordinator itself has only been tested through the stub; its HUD can't show other game runs' requests. A terminal process exiting early (kitty `--single-instance`, wezterm mux) is reported, not a failure; closing then needs the i3 / sway mark.
- **Upstream R3D** builds are compile-checked only.
- **Ideas:** decals (`r3d_decal.h`); height-dependent fog for hazy outdoor views; shadow cascades and cheaper fog lookups (`.claude/skills/benchmarking/measurements.md`).
- **Platform:** Linux + Windows cross-build (tested under wine). The strict-GLSL fixes (R3D pitfall 4) are verified with `glslangValidator` but not yet on the AMD PC. No native Windows build script, no macOS, no Windows sanitizers.

**Open decisions** (from the QA pass of 2026-10-11, still open):
1. **R3D's contrast step clips dark values to black.** `color.contrast` is applied in linear space after tonemapping (`submodules/r3d/shaders/post/output.frag`, `Adjustments()`): everything below 0.5·(1−1/c) becomes pure black. Fixing it in R3D (contrast around mid-grey in a perceptual space) would change the default map's look, so maps keep contrast 1 where it matters instead.
2. **R3D texture path overflow:** `r3d_importer_texture.c` joins the model folder and the texture path into a 256-byte stack buffer without a bounds check. The map loader rejects joins that would reach 256 bytes, so maps can't trigger it; R3D itself is unpatched.
3. **An assimp leak** of about 1.4 KB when a glTF buffer file is shorter than declared, in assimp's own glTF code; not patched.
4. **Not checked yet:** FBX / DAE external references; symlinks inside a package that point outside it.
5. Earlier calls still open: an i3 `for_window` rule for `monolith-claude:` terminals (avoids the flash before the move); the Quaternius licence policy; the medium / low shadow map size and the low preset's render scale; regenerating the Poly Pizza key.

**Only testable by hand:** snip mode (C, drag, Shift+drag, Esc / right click / C, Enter); closing request terminals (K, Shift+K, Ctrl+K n); the F2 panel (presets, rows, Reset, a restart to check the settings stick); walking through the maps (collision, passing under tree crowns, steps).
