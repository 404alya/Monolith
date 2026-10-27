# Architecture

How monolith is built, for anyone reading or changing the code: the technology, the folder layout, the modules, the scene runtime, the request queue, and the known limitations. Keys and flags: [user-guide.md](user-guide.md), [command-line.md](command-line.md); the map format: [map-format.md](map-format.md). "R3D pitfall N" refers to section N of `.claude/skills/lighting-fog-shadows/r3d-pitfalls.md`, a list of R3D behaviours found by debugging (fog, shadows, caches, ambient light). It is kept with the Claude Code skills but is plain reading. The frame-time measurements behind the caches and presets are in `.claude/skills/benchmarking/measurements.md`, each labelled with the machine it was measured on.

## 1. What this is

**monolith** is an AI-assisted map designer: a first-person 3D viewer and editor in plain C on raylib + R3D. You walk or fly through a level; in spectator mode you point, select objects, screen regions or world-space volumes and submit a request, which the game writes as a bundle and queues for Claude Code (§5). The game hot-reloads the edited map package ([user-guide.md](user-guide.md)).

Design principles:
- **The level is data.** A map package (`maps/<name>/`): the map file, its shaders, models, textures and a design brief (`MAP.md`). Nothing in the engine assumes a style.
- **One runtime for editor and games.** `runtime/monolith_scene.h/.c` (§4.1) loads, draws, hot-reloads, collides with and picks a package, in the editor and in any raylib + R3D program (`--export`, `examples/`), so a scene can't drift between the two. The editor reads the map's text; games read its compiled form ([compiled-map.md](compiled-map.md)), which holds exactly what the text parser produces.
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
| Tested on | Linux (CachyOS) with an NVIDIA RTX 4050 Laptop GPU (driver 615.71) and a GTX 1660 Ti desktop; strict-GLSL problems were found on an AMD RX 9060 XT (§6). Any OpenGL 3.3 GPU should work; performance numbers in this repo always name their machine |

**R3D** is vendored as a plain copy in **`submodules/r3d`** (with raylib and assimp). Headers `include/r3d/`, shader docs `docs/shaders/*.md`, examples `examples/`. When in doubt, its source is the reference. Paths like `shaders/deferred/…` or `src/modules/…` in this file are relative to `submodules/r3d`.

> **R3D changes are patches.** Work around R3D from the app side where reasonable. When R3D itself must change, change it minimally and **record every change in `submodules/r3d/patches.md`** (symptom, cause, files, diff). Other R3D checkouts and other projects are never modified from here.

## 3. Layout

```
monolith/
├── build.sh, cmake/mingw-w64-x86_64.cmake, .gitignore
├── README.md, CONTRIBUTING.md, LICENSE (MIT)
├── CLAUDE.md                    # Claude Code: guard rails, hard rules, index of skills (always loaded)
├── .claude/skills/<topic>/      # Claude Code: workflows and lessons (map-request, engine-code, ...), loaded per task
├── .claude/commands/            # Claude Code: slash commands (/task-prompt)
├── docs/                        # this documentation: user guide, command line, map format, export, architecture,
│                                #   request queue, known issues, troubleshooting, glossary
├── maps/                        # map packages (docs/map-format.md), copied next to the executable by build.sh
│   ├── monolith/                # default map (kept outside the repository): monolith.map, MAP.md, shaders/, ...
│   └── pine_forest/             # pine_forest.map, MAP.md, ASSETS.md, models/ (+ sidecars), textures/
├── runtime/                     # scene runtime (§4.1): monolith_scene.h/.c, monolith_capture.h (§4.3),
│                                #   export/ (viewer.c + build.sh templates for --export)
├── resources/fonts/             # the editor's font (Outfit, SIL OFL 1.1): .ttf sources, licence, ASSETS.md + sidecars
├── tools/                       # fetch_asset.py, normalize_model.py, record_asset.py (models and licences);
│                                #   embed_font.py (resources/fonts -> src/font_outfit.h);
│                                #   leakcheck.sh + lsan.supp + lsan_report.py (build and run); request-queue.sh (the coordinator's queue);
│                                #   profile.c (CPU profiles with perf of ./build.sh profile, which builds it: build/linux-profile/profile);
│                                #   stub-terminal.sh, stub-coordinator.sh, test-coordinator.sh (launcher tests)
├── examples/                    # 01_minimal (+ scene/), 02_existing_project, 03_live_editing, 04_import_source, 05_entities,
│                                #   build.sh (§4.3)
├── tests/examples/harness.h     # test build of the examples: --shot in a hidden window (§4.3)
├── tests/models/                # small CC0 test package: every statement kind once
├── tests/unit/                  # wincmd_test.c: the Windows request command line (build line in its header);
│                                #   compiled_test.c/.sh: the compiled-map reader under corruption (ASan + UBSan);
│                                #   export_strip_test.sh: the exported runtime has no text parser
├── tests/profile/               # perf.txt + bench.md: a cut-down real profile and bench report (leakcheck runs tools/profile.c on them)
├── export/, requests/, benchmark/   # gitignored: F9 exports, request bundles, --bench reports
├── src/                         # the editor, one module per concern (§4)
│   ├── main.c                   # header comment (controls + flags), args, data paths, init, main loop, Submit
│   ├── common.h                 # shared includes, the global `MS_Scene *scene`, benchmarkDir
│   ├── player.c, spectator.c    # walking player + collision; spectator camera, LookDir/LookAngles
│   ├── designer.c, launcher.c   # picking, selection, overlay, HUD, prompt box; bundles, queue + coordinator launch, polling
│   ├── mapedit.c, editmode.c    # manual edits as minimal text edits + undo / redo, --apply-edits; the edit mode (M): keys, HUD
│   ├── atmosphere.c             # --ao / --bloom / --fx overrides on top of the map's environment
│   ├── newmap.c, export.c, import.c, dirscan.c   # --new-map, --export / F9, --import, safe folder walk
│   ├── inspect.c, dumpscene.c   # --inspect-model, --dump-scene, --dump-entities
│   ├── bake.c                   # --bake-surface: a material's surface shader baked into a tile (R3D's G-buffer)
│   ├── bench.c, windowemu.c     # --bench report; --bench-window stand-in
│   ├── settings.c, settingspanel.c   # graphics settings + file; F2 panel
│   ├── ui.c, font_outfit.h      # editor UI: embedded font, type scale, colour tokens, text / panel / key-hint helpers
│   ├── png.c                    # PNG writer for --shot and request images (zlib level 1, ~4x faster than ExportImage)
│   └── glibc_compat.c           # Linux only: old glibc symbol versions (build and run)
└── submodules/r3d/              # vendored R3D; local fixes in patches.md
```

**Data paths.** `InitDataPaths()` (`main.c`) sets `benchmarkDir` and the maps directory. On Linux the repo directories baked in (`-DBENCHMARK_PATH`, `-DMAPS_PATH`) win while they exist, so in a repo checkout the game loads and hot-reloads the **repo's** `maps/`, not the copy in `build/`. Otherwise (and on Windows) the folders next to the executable are used. `build.sh` copies `maps/` and `runtime/` (+ `viewer.c`, for `--export`) into both output folders, and the font's licence as `OFL-Outfit.txt` (the font itself is compiled in). `launcher.c` puts `requests/` and starts Claude Code in the repo root (`-DPROJECT_PATH`) while it exists, else (always on Windows) in the executable's directory.

## 4. Code architecture (`runtime/`, `src/*.c`)

The scene (loading, drawing, collision, picking, hot reload) is the **scene runtime** (§4.1). The editor in `src/` is split into modules with a header each; `build.sh` compiles `src/*.c` and `runtime/*.c`, so a new module needs no build change. Larger files use `// #region` blocks.

1. **Scene runtime**: the game holds one scene in the global `MS_Scene *scene` (`common.h`).
2. **Player** (`player.c`): feet-based AABB (r 0.3, h 1.75, eye 1.62), per-axis movement against `scene->colliders` (boxes, then model colliders) with the strict **`BoxesOverlap`** (like `MS_Collides`, R3D pitfall 4); step-up (`STEP_HEIGHT` 0.45), gravity, jump, fly, smoothing, head bob. Starts at `spawn` unless `--pos` / `--eye` / `--look`.
3. **Spectator** (`spectator.c`): state, the map's viewpoints (`scene->viewpoints`), `UpdateSpectator()`, `SpectatorFlyTo()` / `FlyToPose()` / `JumpTo()`, `SpectatorCamera()`. `LookDir(yaw, pitch) = (cos p · sin y, sin p, cos p · cos y)` (yaw 0 → +Z); right = `(−cos y, 0, sin y)`.
4. **Designer** (`designer.c`), one `Designer` state owned by `main()`:
   - **Picking**: `DesignerPick()` → `MS_Raycast()`: slab test against every box, then models (bounds, then triangles through a BVH). Boxes containing the eye are skipped. Refreshed every frame in spectator mode and `--shot` runs.
   - **Selection**: `MS_ObjRef` heap arrays with an index → position table; `DesignerSync()` remaps after a reload through a hash of exact bounds.
   - **Screen regions**: at submit `RegionPick` samples each with up to 4096 rays (per-region candidate lists, culled per 8×8-ray tile): objects with ray shares (also per map line), centre hit, depth range, sky share, and **edge objects** (`AnalyzeRegionEdge`: up to 64 rays per side on a ring just outside the rect, nearest first, with sides).
   - **Volumes** (volume mode, **V**): world-space `BoundingBox` heap array, made from crosshair clicks (snapped on the axes along the clicked face) plus a height step that freezes the camera; `DesignerTakesArrows()` keeps the arrows from the spectator while they edit the active volume. At submit `DesignerAnalyzeVolume` lists every box and model whose world bounds overlap (shares of volume and of object, fully inside), the empty share over a grid of ≤ 4096 cells (exact AABB overlaps per cell, summed and capped), and the floor (a ray down from the centre). `DesignerDrawVolumes()` draws them (an always-on-top faint pass, then depth-tested faces and outlines) with V1, V2 tabs.
   - **Overlay**: `DesignerDrawOverlay()` draws with raylib after `R3D_End()` in `BeginMode3D`, depth-tested against R3D's depth (copied into the target by `R3D_End`) with a 4-step depth bias in the projection and slightly inflated boxes (models: oriented bounds). Also crosshair, snip drawing, HUD.
   - **Prompt box**: UTF-8, word wrap, scrolling, heap text; `DesignerCapturesInput()` freezes game keys and camera.
   - `DesignerBuildRequest()` snapshots prompt, eye, angles, fovy, size, hit, selection, regions, volumes, map version into a `DesignRequest`.
5. **Manual edits** (`mapedit.c`, `editmode.c`; [map-format.md](map-format.md) §2.9):
   - **The map file is the only state.** `MapEditApply()` turns an operation on the selection (move, turn, remove, duplicate, add) into hunks against the loaded map text: per affected statement, the line is re-tokenized with byte offsets (`Analyze`: `id` / `repeat … [from k]` / `at` prefixes, keyword, the positional numbers via `MS_PrimitiveArgs`, model `rot`, light `clip`) and rewritten by splices of the original bytes (`Rewrite`), so comments, spacing and every untouched line stay byte-identical. It checks that the file on disk is the loaded one (FNV-1a = `scene->hash`), writes it in place and `MS_Reload`s; a patch that would not load is written back and refused.
   - **Splitting a `repeat` line** walks the copy grid outer level first: runs of untouched rows become pieces, the changed copy gets a plain line (world coordinates, new `id`). Each piece's changed levels are folded into the start values only where `FoldExact` proves, by redoing the parser's float math for every copy, that the floats stay bit-identical; otherwise the level keeps `repeat n d from k`.
   - **Ids:** an edited line gets `id <max + 1>` unless it has one; the step records the selection before and after as `MS_EntityId`s, and `editmode.c` re-selects through `MS_FindEntity` after the reload.
   - **Undo / redo:** an `EditStep` is the hunks plus the file hashes before and after; undo applies the hunks inverted (offsets shifted by the earlier hunks' size changes), only while the file is the one the step produced. `MapEditSync()` (after every hot reload) drops the history when the map changed elsewhere.
   - **Edit mode** (`editmode.c`): M, then the selection keys of the designer plus the edit keys; arrows / R are taken from the spectator (`EditModeTakesArrows`) and WASD while Ctrl is down (`Spectator.holdMove`, for Ctrl+D). Moves snap the first selected object's anchor (`MapEditAnchor`) to the shared snap step. `LauncherOpenOnMap()` drives the warning when a request of this run is queued or running on the map. Ctrl+I opens the id panel (`EditModeCapturesInput`: while it is open it takes every key and the designer isn't updated) or, for several lines, gives each the next free id; the edit (`EDIT_ID`) rewrites only the lines' `id` prefixes. `--apply-edits` runs the same operations from a script on a noGpu load.
6. **Launcher** (`launcher.c`, see its header comment):
   - `LauncherSubmit()` writes `requests/<YYYY-MM-DD_HHMMSS>_<slug>/` (`view.png`, `view-marked.png`, `region-<n>.png`, `request.md`, `request.json`, format 3), then (coordinator mode) `state` = `queued` and, unless `requests/.coordinator/lock` is held (`flock`), starts the coordinator's terminal (a `/bin/sh` wrapper holds the lock around `claude`; region "Coordinator and queue", §5), or (per-request mode) the request's own terminal. Terminals start with `fork` + `execve` from an argv array, no shell: the user's text never reaches a command line. The child `setsid`s, `chdir`s to the repo root, stdin `/dev/null`; exec failures return through a close-on-exec pipe; children are reaped with `waitpid(WNOHANG)`.
   - Terminal syntax (title, working dir, exec flag) from `TERMINALS[]`; unknown terminals get `-e`. Title `monolith-claude: <slug> <HHMMSS>[-n]`, unique per request. `claude` is looked up in `PATH`, then `~/.local/bin`, passed absolute, with the permission flags (unless `MONOLITH_CLAUDE_SAFE`), `--model <tier's model>` and `MONOLITH_CLAUDE_ARGS` (a bare `--model` there is dropped with a warning). A detached helper moves and marks the window. On launch failure the bundle stays, the HUD names it and the log prints the command.
   - `LauncherUpdate()` polls open requests for `state` and `result.md` (HUD 20 s) and tests the coordinator's lock every 2 s; `LauncherLastDone()` gives the camera for **R**.
   - **Queue**: after `queued` only the coordinator writes `state`, through `tools/request-queue.sh` (claim with a fingerprint in `claim.txt`, question, finish, requeue, recovery after a crash, the background watcher `wait`); the game asks for a cancel with a `cancel` file. Test: `tools/test-coordinator.sh` (stub terminal + `tools/stub-coordinator.sh`, which applies the same scheduling rules).
   - **Windows**: `cmd /c start "<title>" /D <exe dir> cmd /k "<claude>" … "<prompt>"` via `_spawnvp` (the coordinator's with ` 9>"<lock>"`, tested with an exclusive `_sopen_s`); compile-tested only.
7. **Atmosphere** (`atmosphere.c`): the environment is map data (`env` lines; defaults in `BuiltinEnvironment()`, applied by `MS_ApplyEnvironment`). Graphics settings (`MS_ApplyQuality`, incl. `--ao` / `--bloom`) go on top, then `--fx`; `main.c`'s `ApplyEnvironment()` re-applies environment → quality → `--fx` → toggle flips → `MS_ApplyRenderResolution` after every reload.
8. **Graphics settings** (`settings.c`, `settingspanel.c`): `GfxSettings` = `MS_Quality` + window options (MSAA, vsync, fps cap, frame reuse).
9. **New map, export, import, inspection, dumps** (`newmap.c`, `export.c`, `import.c`, `inspect.c`, `dumpscene.c`), and tile bakes (`bake.c`): §4.3, [command-line.md](command-line.md).
10. **Benchmark** (`bench.c`, `windowemu.c`): statistics, the Markdown report, the `--bench-window` stand-in.
11. **Main** (`main.c`):
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
// then on the same target: DesignerDrawOverlay (depth-tested), DesignerDrawVolumes, crosshair, HUD, regions, prompt box, settings panel
```

### 4.1 Scene runtime (`runtime/monolith_scene.h` + `runtime/monolith_scene.c`)

A separately compilable C11 module depending only on raylib (raymath, rlgl) and R3D. `monolith_scene.h` is the API contract (read its comments); the header comment of `monolith_scene.c` is the map format reference.

**Two builds of one source.** monolith, `03_live_editing` and the capture shim compile it with `-DMONOLITH_EDITOR`: the **text parser** (tokenizer, statements, `repeat` expansion, the model-file reference scan), the compiler (`MS_Compile`), hot reload, source lines, `brief` / `hash` / `mtime`, the dependency list, the environment writers, `MS_CheckModelReferences`, `MS_EnvKeyRange` and the `sunOn` toggle live in `#ifdef MONOLITH_EDITOR` blocks. `--export` writes the file without those blocks (`StripEditor` in `src/export.c`), so a game gets only the compiled-map reader, loading, drawing, quality presets and queries; the game-style examples (`01_minimal`, `02_existing_project`, `05_entities`) are built the same way.

**Two inputs, one load.** A load fills a CPU-only `SceneData` from either input, then runs the same steps: `LoadPath` → `LoadBytes` (the file's first bytes: the compiled map's magic or text) → `InitData` (paths, built-in environment), then `ParseScene` (text, editor builds only) or `ReadCompiled` (compiled, both builds) → `FinishLoad`: `FinishParse` (sun light, default viewpoint, load options), model files (`LoadModelFiles`, or `CheckModelFiles` without the GPU), `ResolveModels`, `FinishGeometry`, the collider BVH, `BuildEntities`, `UploadGpu` → `Publish`. Every load with the GPU logs where its time went (`monolith_scene: load|reload <ms>: parse, models, collider BVH, surface shaders (n compiled), textures, hidden faces, box meshes, rest`), and the next `MS_PushLights` calls log the first two frames after it (`first frames after the load a + b ms`: they compile the shader variants they draw with). The hidden-face search (`FindHiddenFaces`) runs on up to 8 threads (pthreads; MinGW's winpthreads is linked into the static `.exe` already), each taking chunks of boxes; a box's faces depend only on the boxes, so the result is the same with any number of threads. The compiled file ([compiled-map.md](compiled-map.md)) holds the `SceneData` as the parser leaves it (before `FinishParse`), value for value, so both inputs give the same scene and image. `ReadCompiled` checks the whole file first (bounds, counts, indices, strings, floats, paths; the env checks are the parser's own, `CheckEnvValue`) and refuses a bad one with `file: message`. `MS_Compile` parses, writes (`WriteCompiled`), runs the rest of a `noGpu` load as the package check and reads its own output back before it renames it into place. Rules for runtime changes: tool-only code goes into such a block (never nested; `#ifndef MONOLITH_EDITOR … #endif` for game-only code, `#else` allowed); comments outside the blocks must not mention the editor, requests, Claude Code, hot reload or monolith's command-line flags, because they ship; the runtime must build with zero warnings with and without `MONOLITH_EDITOR` (and without `MS_PATCHED_R3D`).

**What it owns** (all freed by `MS_Unload`):
- the parsed package: boxes (`scene->boxes`, `boxRefs[]`: source line, statement index, group, material, `repeat` copy and part), `modelFiles[]`, `models[]` (transform, world bounds, collide mode, slot overrides), colliders (boxes first), materials (`[0]` = `"default"`), groups, lights, sun, spawn, viewpoints, bounds, path, package root, a **`version`** that goes up on every successful (re)load (static caches key on it); editor builds also the map text (`MS_GetLine` / `MS_LineText`), `brief`, FNV-1a hash and mtime;
- GPU: one compiled surface shader per shader file, and per material an alias of it (`R3D_LoadSurfaceShaderAlias`: its own uniforms and samplers, the shared program and shader variants; made from a copy no material draws with, so a material that leaves a uniform unset gets the shader's default, not another material's value; R3D pitfall 4), textures (`wrap repeat`, trilinear + mipmaps), per material and 32 m cell, the faces of its boxes that no opaque box hides as one static mesh, or with `instanced on` one instance buffer of unit cubes (R3D pitfall 4), each a clustered draw (frustum culling); one instanced draw per (model file, slot materials, shadow flag), split into one clustered draw per cell, the visible sky and the ambient map (R3D pitfall 5);
- the sun shadow (`MS_SHADOW_TILES`, or `MS_SHADOW_CAMERA` / `MS_SHADOW_ALWAYS`), extra lights' shadow maps, the fog cache and AO volume, all placed by the map's bounds (R3D pitfall 3);
- **entities** (`BuildEntities`, after every load): every box, model instance and light in map order, with its components in parallel heap arrays (`entityIds`, `entityObjects`, `entityTransforms`, `entitySources`, `entityModels`), object → entity per kind, and an id → entity hash (`MS_FindEntity`). The id is the statement's key (its `id n`, else a hash of its tokens with bit 31 set, the next free key for identical statements; `AddStatement`) plus the object's index in the statement. New components are new arrays next to these; systems (per-object updates, animation) are not built yet;
- picking (`MS_Raycast`, `MS_RaycastModel`: BVH over a CPU copy of the triangles), collision (`MS_Collides`, strict), hot reload (`MS_Poll`, editor builds).

**API:**

| Group | Functions |
|---|---|
| Loading | `MS_Load(path, &MS_LoadOptions{ noGpu, shadowSizeOverride, shadowRangeOverride }, err, size)` (a compiled map or a package folder holding one; editor builds also the text), `MS_Unload`, `MS_SetInitHints` (before `R3D_Init`), `MS_InitRenderer` (after) |
| Compiled maps (editor) | `MS_Compile(mapPath, outPath, err, size)`, `MS_CompiledSource(scene, &sourcePath)` → `MS_SOURCE_TEXT` / `CURRENT` / `STALE` / `MISSING` |
| Hot reload, source lines (editor) | `MS_Poll(scene, dt)` → `MS_UNCHANGED` / `MS_RELOADED` / `MS_RELOAD_FAILED`, `MS_Reload`, `MS_Error`, `MS_GetLine`, `MS_LineText`, `MS_MapText`, `MS_PrimitiveArgs` |
| Environment and drawing | `MS_ApplyEnvironment`, `MS_Get/SetRenderSettings`, `MS_QualityPreset` / `Name` / `FromName` / `Match` / `Equal`, `MS_ApplyQuality`, `MS_ApplyRenderResolution`, `MS_Draw`, `MS_PushLights` + `MS_DrawGeometry` (inside your own `R3D_Begin` … `R3D_End`) |
| Queries | `MS_Raycast`, `MS_RaycastModel`, `MS_Collides`, `MS_ObjectRef`, `MS_ObjectBounds`, `MS_ObjectEntity`, `MS_FindEntity` |
| Package files, writers (editor) | `MS_DependencyCount` / `MS_Dependency` (watched files), `MS_WriteEnvironment`, `MS_WriteR3DEnvironment` (any `R3D_Environment` as `env` lines), `MS_CheckModelReferences`, `MS_EnvKeyRange` |
| Memory | `MS_ArrayReserve`, `MS_RESERVE`, `MS_PUSH`, `MS_StrDup`, `MS_StrNDup` |

**Call order:** `InitWindow`; optionally a `noGpu` peek load + `MS_SetInitHints` + `MS_Unload`; `R3D_Init`; `MS_InitRenderer`; `MS_Load`; `MS_ApplyEnvironment` (+ `MS_ApplyQuality`) + `MS_ApplyRenderResolution`; per frame `MS_Poll` (re-apply on `MS_RELOADED`) and `MS_Draw`; `MS_Unload` before `R3D_Close`. Complete version: `examples/01_minimal/main.c`.

**Loading and hot reload.**
- `MS_Load` / `MS_Reload` read (parse or, for a compiled map, check and copy) and upload into a new scene and swap it in **only if the whole package is valid**. A package folder resolves to `<name>.map` (editor builds; else its only `.map`, else a compiled map) or `<name>.mapb` (game builds; else its only `.mapb`). Errors are `file:line: message`; the loader never crashes on bad input. Every referenced file must exist, also for `noGpu` loads (`--dump-boxes` is a full package check; model bounds and colliders are final only in GPU loads).
- `MS_Poll` checks every dependency's mtime about twice a second; after a change it keeps re-reading for 2 s (1 s mtime resolution, editors writing in steps) and reloads when the combined content hash differs from the last attempt. The watch list is the last *attempt*'s, so a missing file is picked up when it appears. A reload creates every new GPU resource **before** freeing the old ones (R3D pitfall 4), and all static caches re-key on `scene->version`. `shadow.size` only changes with a restart.

**Patched vs. upstream R3D (`MS_PATCHED_R3D`).** With `-DMS_PATCHED_R3D` (set by `build.sh` and `examples/build.sh`) the runtime uses the patched APIs (`R3D_PushLightFixedShadow`, `R3D_SetDepthPrepass`, the fog bake, the occlusion volume, the profiler); without it, it falls back to `R3D_PushLightEx` with the camera-fitted map (`MS_SHADOW_TILES` → `MS_SHADOW_CAMERA`), no fog cache or AO volume. The export README lists the image differences (patch 4 visible when turning, 7/9/24/25/26 rounding level, patch 1 needed for volumetric fog on AMD / Intel). The upstream build is compile-checked only.

### 4.2 Surface shaders in map packages

A material's `shader "shaders/x.glsl"` is an R3D **surface shader** (`#pragma usage opaque shadow`, `void fragment()`), compiled once per file on every load (so an edited file recompiles on hot reload), each material drawing with its own alias of it; `uniform` / `sampler` keys set its uniforms. Built-ins: `submodules/r3d/docs/shaders/surface_shader.md` (`ALBEDO`, `ROUGHNESS`, `METALNESS`, `SPECULAR`, `EMISSION`, `NORMAL_MAP`, `POSITION`, `NORMAL`, …). Boxes have per-face 0–1 UVs, so a pattern that should look the same on every box size uses world-space `POSITION` / `NORMAL` (like `concrete.glsl`). Opaque shaders that never discard want `alpha_cutoff 0` (pre-pass). Shaders that read `INSTANCE_*` or `POSITION` in `vertex()` need `instanced on` on their material (R3D pitfall 4). Check every shader with `glslangValidator` (R3D pitfall 4).

### 4.3 Export, import, examples

- **Export** (`src/export.c`, `--export <dir> [--map …]`, F9 = the open map): a self-contained folder that builds with one command; what it contains, how its build works and how to use it: [export.md](export.md). It copies the runtime's dependency list (plus `MAP.md`, `ASSETS.md`, sidecars, licence files and the files model files refer to), adds the runtime without its `MONOLITH_EDITOR` blocks (`StripEditor`; no text parser left, `tests/unit/export_strip_test.sh`), `viewer.c` / `build.sh` from `runtime/export/`, the R3D tree (`R3D_KEEP` minus `R3D_SKIP`) and its prebuilt libraries when current (`AddPrebuilt`); every source is checked first, the folder is written as `<dir>.partial`, its `.map` compiled there into `<name>.mapb` (what the viewer loads), the compiled map loaded again and the folder renamed. A compiled map exports from its source text (refused when stale). Needs the repo.
- **Import** (`src/import.c`): `NewMap()`, a bundle written like the launcher's, `LauncherStartBundle()` (model from `LauncherChooseModel`, `kind = "import"`). Claude follows its import workflow (`.claude/skills/import-request`).
- **Capture shim** (`runtime/monolith_capture.h`, `-include`d into a program): routes `R3D_LoadModel`, `R3D_GenMeshCube`, `R3D_GenProceduralSky`, `R3D_LoadCubemap`, `R3D_LoadSurfaceShader`, `R3D_Begin*`, `R3D_PushLight*`, `R3D_DrawMesh*`, `R3D_DrawModel*`, `R3D_End` through recorders; with `MONOLITH_CAPTURE=<dir>` writes a package from one frame plus `CAPTURE.md` (what it couldn't translate). Limits: only generated cubes and loaded models are read back, rotated cubes become AABBs, uniforms and material changes on loaded models are only reported. A cross-check, not a replacement for reading the source.
- **Examples** (`examples/build.sh [--win]` → `examples/build/<linux|win>/<example>/`): `01_minimal`, `02_existing_project` (`before.c` / `after.c`), `03_live_editing` (`MS_Poll`), `05_entities` (entities, components, the game's own systems; built like an export, without `-DMONOLITH_EDITOR`), `04_import_source` (import test input; also `import_source_capture`). Same flags and static linking as the game, `-DMS_PATCHED_R3D`, the runtime as an export has it (`-DMONOLITH_EDITOR` only for `03_live_editing` and the capture build); the game-style examples' copied `scene/` gets its map compiled by `build/linux/monolith --compile`. They are plain integration code without tool flags. **Test build** (`examples/build.sh test` → `linux-test/`, `lsan` → `linux-lsan/` for the leak check): each main file is compiled on its own with `-include tests/examples/harness.h`, which renames `main`, requires `--shot <file>`, and redirects `InitWindow` (hidden window `monolith-shot` + a render texture), `R3D_Begin*` / `MS_Draw` without a target (into that texture), `EndDrawing` (saves frame 30), `WindowShouldClose`, `GetFrameTime` (fixed 1/60 s) and the cursor / FPS calls; with the capture shim in front it wraps the shim's recorders. The same header screenshots any raylib + R3D program (import verification).

### 4.4 Editor details

- **Selection** survives reloads: objects are matched by identical geometry (models: world bounds), then by map line + index within the line + group; lost ones are reported. After a manual edit the selection is set from the entity ids the edit returns instead.
- **Editor UI** (`ui.c`): everything the editor draws over the scene (HUD, designer overlay, prompt box, edit mode, launcher lines, F2 panel) goes through its helpers, never raylib's `DrawText`. The font is Outfit (Regular + SemiBold), compiled in from `src/font_outfit.h` (generated by `tools/embed_font.py` from `resources/fonts/`), so the build folders stay copy-as-is. It loads on first use: every size of the type scale (14, 16, 18, 20, 24 px em) in both weights is rasterised at its exact pixel size and packed into one atlas, which also holds the white block raylib's shapes use (`SetShapesTexture`), anti-aliased corner discs and rings (rounded panels and pills as nine-slices) and a baked key cap per line size (a 3-slice), so text and panels draw in one batch with few quads. Codepoints: what the font has of ASCII, Latin-1 and Latin Extended-A (Turkish included); others draw as `?`. Text is laid out by `ui.c` itself, exactly as raylib's `DrawTextEx` / `MeasureTextEx` do, but with a codepoint → glyph table per face instead of raylib's linear `GetGlyphIndex`. Key hints use a small markup (`{KEY}` key cap, `|` separator, `~off~` / `^on^` toggles). The runtime, `--export` and the examples keep raylib's default font. Plain `--shot`s draw no UI and never load it.
- **Prompt box:** UTF-8 editing, Ctrl+Backspace deletes a word, arrows, Home / End (Ctrl: the whole text), Ctrl+V, Shift+Enter newline. 1 MB cap with a byte counter. Wrapping and the cursor measure with `UiAdvance`, the same metrics the text is drawn with. Game keys and the camera stay frozen while it is open (+2 frames). On a failed submit the box keeps the text and the HUD shows the error and the bundle path.
- **Graphics settings** (`settings.c`, `settingspanel.c`, the runtime's `MS_Quality`): every scene setting is relative to the map's environment; toggles 1–6 and `--fx` apply on top. Everything that changes the image is in `FrameKey` (the whole `GfxSettings` + R3D's AA mode). Below the map's `shadow.size` the sun's depth and slope biases are scaled by the texel ratio (`SunShadowMap()`) so low suns get no acne. In your own program: `MS_QualityPreset(level)`, `MS_ApplyQuality(scene, &q)` after every `MS_ApplyEnvironment`, then `MS_ApplyRenderResolution`; `q.shadowSize` goes to `MS_LoadOptions.shadowSizeOverride` (`examples/01_minimal --quality medium` renders the same pixels as `monolith --quality medium`).

## 5. Request queue and coordinator

How requests travel from the game to Claude Code (the queue on disk, the coordinator session, the lock, recovery, scheduling, cancelling, tests) is described in [request-queue.md](request-queue.md).

## 6. Known limitations and next steps

- **Manual edits:** lights can be edited only through `--apply-edits` (the crosshair doesn't pick lights); no drag on the clicked face (arrow steps only); one `id` per line, so splitting a row gives the other pieces new implicit keys; the request warning sees only this game run's requests. **Entities** have components but no systems yet (per-object update functions, animation, scripted behaviour): the next part of the ECS.
- **Map format:** boxes are axis-aligned with per-face 0–1 UVs (textures stretch); only models rotate; models are static (no animation or skins); instances are rebuilt on every reload; code changes and `shadow.size` need a restart (or the panel's renderer restart).
- **Models:** picking hits triangles, collision uses AABBs; `noGpu` loads import model files on the CPU only to check them (bounds and `origin` shifts are final only in GPU loads); FBX / DAE references aren't followed for hot reload or export; the AO volume ignores models.
- **Interactive input** (clicks, drags, typing, paste, Esc, the panel, camera freezing) is tested only by hand.
- **Windows launcher** is compile-tested only; `start` failures go undetected; workspace moves and closing are i3 / sway only. On Windows and in copied build folders, `requests/` and Claude Code's working directory are the executable's folder (no sources, no `CLAUDE.md`).
- **Request flow:** coordinator mode: a bundle stays queued when the coordinator can't start (the HUD says why; the next submit tries again), and the prompt box closes. Per-request mode: a failed launch keeps the bundle and the text; Enter again writes a second bundle (trash the stale one). The coordinator itself has only been tested through the stub; its HUD can't show other game runs' requests. A terminal process exiting early (kitty `--single-instance`, wezterm mux) is reported, not a failure; closing then needs the i3 / sway mark.
- **Upstream R3D** builds are compile-checked only.
- **Ideas:** decals (`r3d_decal.h`); height-dependent fog for hazy outdoor views; shadow cascades and cheaper fog lookups (`.claude/skills/benchmarking/measurements.md`).
- **Platform:** Linux + Windows cross-build (tested under wine). The strict-GLSL problems (R3D pitfall 4) were found on an AMD RX 9060 XT; the fixes are verified with `glslangValidator` but not yet re-tested on that card. No native Windows build script, no macOS, no Windows sanitizers.

Open decisions and things only testable by hand: [known-issues.md](known-issues.md).

## 7. GPU memory (VRAM)

Where the game and R3D allocate GPU memory, from a read of the code (2026-10-26). The figures
are arithmetic from the formats and sizes in the code, **not measurements**; "1080p" means a
1920 × 1080 window, "S" the sun shadow size (`env shadow.size`, the panel's shadow size or
`--shadow`; 8192 in the shipped maps). MiB = 2²⁰ bytes.

### 7.1 What costs what

| What | Where | Format and size | Lifetime |
|---|---|---|---|
| Sun shadow maps (2 static tiles) | `MS_Load` (`R3D_LoadShadowMap(R3D_LIGHT_DIR)` × 2); R3D `r3d_light.c` (DIR growth 2) | 2 layers × S² × 2 B (DEPTH16) + min/max tiles and mid map (RG32F): **~268 MiB at 8192**, ~67 MiB at 4096, ~1 GiB at 16384 | Scene lifetime; the size is an init hint, so changing it restarts the renderer (`RestartRenderer` in `main.c`) |
| Shadowed point light | `UploadGpu` (`R3D_LoadShadowMap`); R3D omni cube array, growth 1 (patch 21) | 6 × 2048² × 2 B = **48 MiB per light** | Per load; the array never shrinks, and after a hot reload it holds the old and new layers (96 MiB for one light) |
| Shadowed spot light | same; R3D spot array, growth 4 | 2048² × 2 B = 8 MiB per layer, **32 MiB on the first** | Per load; never shrinks |
| Render targets (G-buffer, lighting, scene ping-pong, HW depth) | R3D `r3d_target.c` (table at the top) | ~109 MiB at 1080p; scales with the render resolution | Allocated on first use, resized with the resolution |
| Effect targets (SSAO, SSIL, SSGI, SSR, DoF, bloom, volumetric fog, SMAA, exposure) | same | up to ~52 MiB at 1080p with everything on | Allocated when an effect first runs, **kept until exit** |
| Procedural sky cubemap | `UploadGpu` (`R3D_GenProceduralSky`); R3D `r3d_cubemap.c` | RGB16F with mips: 48 × `sky.size`² B, **48 MiB at 1024** | Per load |
| Panorama sky cubemap | `UploadGpu` (`R3D_LoadCubemap`, panorama layout) | face size = the image's height: 192 MiB at 2048, 768 MiB at 4096; `sky.size` is ignored | Per load |
| Ambient / reflection (IBL) | `R3D_GenAmbientMap`; R3D `r3d_env.c` | 32² irradiance + 128² prefiltered layers, < 1 MiB each; arrays grow by 8 | Per load; arrays never shrink |
| Sun fog visibility volume | `R3D_BakeFogSunVisibility`; R3D `r3d_light.c` | R8 3D texture, capped by `FOG_CACHE_BUDGET` (64 MiB) | Rebuilt when its dimensions change; freed only at `R3D_Close` |
| Baked AO volume | `BakeOcclusionVolume` → `R3D_SetWorldOcclusion` | R8 3D texture, capped by `AO_VOLUME_BUDGET` (32 MiB) | Per load and AO setting; freed on clear |
| Map textures | `GetTexture` → `R3D_LoadTextureEx` | the file's channels × w × h, + ⅓ for mips (trilinear) | Per load; cached within one load by (path, colour space, wrap) |
| Model files | `LoadModelFiles` → `R3D_LoadModelEx` | 36 B per vertex, 4 B per index, plus **every** material texture in the file | Per load |
| Merged box meshes | `BuildBoxMesh` | 168 B per visible face (4 vertices + 6 indices) | Per load |
| Box and model instances | `R3D_LoadInstanceBuffer` | 24 B per box (position, scale), 40 B per model instance | Per load |
| Global vertex / index buffers | R3D `r3d_render.c` | 2.25 MiB + 0.5 MiB at init, doubled when full | Never shrink |
| Editor UI atlas | `ui.c` | one 1024-wide gray-alpha texture | Loaded on first UI use |
| Frame-reuse cache | `LoadSceneCache` in `main.c` | RGBA8 + depth at window size (~8 MiB + depth at 1080p) | Created on first unchanged frame; kept until exit |
| `--shot` / headless target, submit target | `main.c` | RGBA8 + depth at window size | `--shot`: run lifetime; submit: per request |

A hot reload builds the new map's GPU state completely before freeing the old one (the swap at
the end of `LoadBytes`; R3D caches instance buffer names), so during a reload the per-load rows
above are held twice.

### 7.2 Known excess

What the code allocates beyond what it uses, largest first. None of these is fixed yet, except
the omni growth (R3D patch 21: one shadowed point light used to reserve 192 MiB).

- **Sun shadow maps with the sun shadow off.** `MS_Load` allocates both tiles unconditionally;
  with `sun.shadow off` or the panel's sun shadow off they are never drawn. ~268 MiB at 8192.
- **Model textures under map materials.** R3D uploads every texture of a model file; a slot
  the map overrides with its own material (or `inherit albedo`, which keeps only the albedo)
  leaves the file's other textures unused on the GPU. R3D has no import flag to skip them.
- **Effect targets after the effect is turned off** stay allocated, and are resized with the
  resolution (up to ~52 MiB at 1080p).
- **Geometric normal target** (RG16, full resolution, ~8 MiB at 1080p) is written every frame
  but read only by decals, which monolith doesn't use.
- **Half-resolution mips** of the normal, radiance and specular targets (~8 MiB at 1080p) are
  read only by SSAO, SSIL, SSGI and SSR.
- **Shadow and IBL arrays never shrink**, and an expansion holds the old and new texture at once.
- **Unreferenced materials** in a map still upload their textures and shaders.
- **Panorama skies have no size cap**: the face size is the image's height.
