# monolith — Project Instructions

Engine and technical reference for anyone (human or AI assistant) changing this project. Read it before changing code. Instructions for carrying out map edit requests (the request workflow, the map format and editing rules) are in **`AI-MAP-TOOL.md`**. `CLAUDE.md` imports both files.

## 1. What this is

**monolith** is an **AI-assisted map designer** for 3D levels of any style: a real-time, first-person 3D viewer and editor rendered with raylib + R3D in plain C. You can walk through a level or fly around it with a spectator camera. In spectator mode the developer points at a spot, selects objects or drags out screen regions, types a request and submits it. The game writes a request bundle and opens **Claude Code in a new terminal**, which edits the map package. The game **hot-reloads** the package, so the change appears in place.

Design principles:

- **The level is data.** A level is a **map package** (`maps/<name>/`): a plain-text map file (one statement per line: axis-aligned boxes, placed models, materials, lights, environment, spawn, viewpoints), its shaders, models and textures, and a design brief (`MAP.md`). Nothing in the engine assumes a style; each map's look and rules live in its package.
- **One runtime for editor and games.** The scene runtime (`runtime/monolith_scene.h/.c`, §6.1) loads, draws, hot-reloads, collides with and picks a package. The editor uses it, and so can any raylib + R3D program (`--export`, `examples/`), so a scene can't drift between the two.
- **Assets are traceable.** Every external file in a package has a recorded source and licence (CC0 / public domain, or CC-BY with attribution); models are restyled through material overrides in the map, never by editing the file.
- **Plain C on a lightweight renderer.** raylib + R3D rather than a full engine.

Two maps ship with the repo: the default map `maps/monolith/` (a brutalist concrete complex; boxes only, one procedural shader; its brief is `maps/monolith/MAP.md`) and `maps/pine_forest/` (a stylised low-poly forest at dusk with restyled CC0 models, a texture and point lights). Both are the reference test scenes: engine changes keep the default map pixel-identical.

Current state: the scene runtime (map format 2 with materials, models, environment, lights, spawn and viewpoints as data; hot reload of the whole package; caches that follow the map's bounds; quality presets), walk and spectator cameras, crosshair picking and selection of boxes and models, screen-region selection (snip mode), an in-game prompt box, request bundles (format 2) with a terminal + Claude Code launcher (model tiers, workspace 10, permission flags), result polling and closing of request terminals, a graphics settings panel and file, `--new-map`, `--export`, `--import` and a capture shim, `--inspect-model`, `examples/`, live effect toggles, offscreen screenshots, a benchmark mode and a leak check.

## 2. Technology

| Item | Details |
|---|---|
| Language | C11 (`-std=c11 -Wall -Wextra`). The program is the modules in `src/*.c` (§6) and the scene runtime `runtime/monolith_scene.c` (§6.1), plus a small Linux build shim, `src/glibc_compat.c`. |
| Windowing / input | raylib 5.5 (bundled with R3D, linked statically) |
| Renderer | **R3D** 0.10.x (Bigfoot71/r3d): deferred PBR on top of raylib. Provides shadows, SSAO, SSIL, SSGI, SSR, fog, volumetric fog, bloom, tonemapping, procedural sky, IBL, instancing, surface shaders. |
| Assimp | R3D's model importer (glTF/GLB, OBJ, FBX, DAE, …); `--inspect-model` (`src/inspect.c`) also calls it directly |
| OpenGL | 3.3 core (through R3D/raylib). `libGL` (`opengl32` on Windows) is linked only for `glEnable` (the runtime's `MS_InitRenderer`, §7.2), `glFinish` (`main.c`, `--headless`) and `glGetString` (`bench.c`), declared by hand. |
| Platform | Linux (X11/GLFW), the main target. Windows x86_64 is cross-compiled with MinGW-w64 (`./build.sh --win`) and tested under wine. |
| Dev machine | CachyOS, NVIDIA RTX 4050 Laptop GPU, driver 615.71 |

**R3D location:** vendored as a plain copy (not a git submodule) in **`submodules/r3d`**, with its bundled raylib and assimp. `build.sh` builds it on first use as static libraries: `submodules/r3d/build-linux` for Linux and `submodules/r3d/build-mingw` for Windows. Headers are in `submodules/r3d/include/r3d/`, docs in `submodules/r3d/docs/shaders/*.md`, examples in `submodules/r3d/examples/`. The R3D source is the best reference for its behaviour; check it rather than guessing. Paths like `shaders/deferred/…` or `src/modules/…` elsewhere in this file are relative to `submodules/r3d`.

> **R3D changes are patches.** Work around R3D limitations from the app side where reasonable. When R3D itself has to change (e.g. a real bug), change `submodules/r3d` minimally and **record every change in `submodules/r3d/patches.md`** (symptom, cause, files, diff), so the copy can be refreshed from upstream later. Don't modify `~/Projects/r3d` (the original checkout) or other projects in `~/Projects` (e.g. `gameproject2`).

## 3. Layout

```
monolith/
├── build.sh                     # build script (see §4)
├── .clangd                      # include paths for clangd (absolute paths into submodules/r3d)
├── .gitignore                   # build/, screenshots, benchmark/, requests/, export/, .clangd, unused R3D parts
├── cmake/
│   └── mingw-w64-x86_64.cmake   # toolchain for cross-building R3D for Windows (--win)
├── CLAUDE.md                    # read automatically by Claude Code: product line, guard rails, hard rules, imports the two below
├── AI-MAP-TOOL.md               # the AI side of the map designer: requests, packages, map format, models, verification
├── PROMPT-INSTRUCTIONS.md       # this file (engine / technical reference)
├── AI-MAP-DESIGNER-PROMPT.md    # the task description of the first round (map designer + caching)
├── AI-MAP-DESIGNER-V2-PROMPT.md # the task description of the second round (any style, models, export/import, regions)
├── docs/design-v2.md            # design record of the second round (interfaces as planned, stream notes)
├── maps/                        # map packages (AI-MAP-TOOL.md §5), copied next to the executable by build.sh
│   ├── monolith/                # the default map: brutalist concrete, boxes only (brief: its MAP.md)
│   │   ├── monolith.map         # format 2 (reference: the header comment of runtime/monolith_scene.c)
│   │   ├── MAP.md               # the map's design brief (style, scale, materials, light, rules, groups)
│   │   ├── ASSETS.md            # external assets (none)
│   │   └── shaders/concrete.glsl    # procedural board-formed concrete (R3D surface shader)
│   └── pine_forest/             # stylised low-poly pine forest at dusk: pine_forest.map, MAP.md, ASSETS.md,
│                                #   models/ (CC0 Kenney, Poly Pizza, Poly Haven + sidecars), textures/ (ambientCG)
├── runtime/                     # the scene runtime (§6.1): monolith_scene.h (API) + monolith_scene.c,
│                                #   monolith_capture.h (capture shim for imports, §6.3)
├── tools/                       # fetch_asset.py (licence-safe downloads: Poly Haven, ambientCG, Kenney, Poly Pizza
│                                #   API), normalize_model.py (headless Blender), record_asset.py (ASSETS.md row +
│                                #   <file>.asset.md sidecar): AI-MAP-TOOL.md §12; leakcheck.sh (LeakSanitizer over
│                                #   the main code paths, §4), lsan.supp + lsan_report.py (which leaks are the
│                                #   driver's); stub-terminal.sh (a fake terminal for launcher tests)
├── examples/                    # raylib + R3D programs using map packages + examples/build.sh (§6.3):
│                                #   01_minimal (+ scene/ sample package), 02_existing_project, 03_live_editing,
│                                #   04_import_source, common/shot.h
├── tests/models/                # small CC0 test package: every statement kind once (leakcheck, loader tests)
├── export/                      # F9 exports (gitignored)
├── requests/                    # request bundles written by the game (gitignored), see AI-MAP-TOOL.md §2
├── benchmark/                   # --bench reports: <timestamp>_shadow-<size>-<range>.md (gitignored, local)
├── src/                         # the editor, one module per concern (§6)
│   ├── main.c                   # header comment (controls + flags), args, data paths, init, main loop, Submit
│   ├── common.h                 # shared includes, the global `MS_Scene *scene`, benchmarkDir
│   ├── player.c/.h              # walking player, collision, BoxesOverlap
│   ├── spectator.c/.h           # spectator camera (viewpoints come from the map), LookDir/LookAngles
│   ├── designer.c/.h            # crosshair picking, selection, overlay, HUD lines, prompt box
│   ├── launcher.c/.h            # request bundles, terminal + Claude Code launch, result.md polling
│   ├── atmosphere.c/.h          # --ao / --bloom / --fx overrides on top of the map's environment
│   ├── newmap.c/.h              # --new-map: creates a map package
│   ├── export.c/.h              # --export / F9: package + runtime + README for other projects (§6.3)
│   ├── import.c/.h              # --import: new package + an import request for Claude Code (§6.3)
│   ├── dirscan.c/.h             # folder walk for import/export: heap paths, no symlinked folders, entry caps
│   ├── inspect.c/.h             # --inspect-model: what a model file contains (assimp directly, no window)
│   ├── dumpscene.c/.h           # --dump-scene: boxes, models, colliders, materials, lights as text
│   ├── bench.c/.h               # --bench statistics and Markdown report
│   ├── windowemu.c/.h           # --bench-window: offscreen stand-in for the visible window (MSAA, HUD, resolve)
│   ├── settings.c/.h            # graphics settings: MS_Quality + window options, rows, settings file
│   ├── settingspanel.c/.h       # F2: the in-game graphics settings panel
│   └── glibc_compat.c           # Linux only: binds a few symbols to old glibc versions (§4)
└── submodules/
    └── r3d/                     # vendored R3D (+ raylib, assimp); local fixes in patches.md
```

`InitDataPaths()` (`main.c`) sets the runtime `benchmarkDir` and the maps directory (the code uses those, not the macros). On Linux, the repo directories baked in at compile time (`-DBENCHMARK_PATH`, `-DMAPS_PATH`) win while they exist. Otherwise, as on Windows, `benchmark/` and `maps/` next to the executable are used (`GetApplicationDirectory()`). So the binary runs from any working directory, and `build/linux/` and `build/win/` can be moved to other computers. `build.sh` copies `maps/` (the packages, with their shaders, models and textures) and `runtime/` (the runtime sources + `viewer.c`, for `--export`) into both output folders. On the dev machine the game therefore loads and hot-reloads the **repo's** `maps/monolith/`, not the copy in `build/`.

`launcher.c` decides on its own where `requests/` goes and which directory Claude Code is started in. On Linux it uses the repo root baked in as `-DPROJECT_PATH` while that exists, and otherwise the executable's directory. On Windows it always uses the executable's directory.

## 4. Build and run

```bash
./build.sh              # -> build/linux/monolith + build/linux/maps/  (clang, -O2)
./build.sh run [args]   # build, then run with args
./build.sh debug        # -g -O0 + AddressSanitizer/UBSan -> build/linux-debug/ (never replaces build/linux/)
./build.sh lsan         # -O1 -g + LeakSanitizer only (fast) -> build/linux-lsan/; examples: examples/build.sh lsan
tools/leakcheck.sh      # builds the lsan binaries if needed, runs every main code path, exit 1 on a leak
tools/leakcheck.sh --asan   # the same paths with the ASan + UBSan build (memory errors, UB, leaks)
R3D_DIR=/other/r3d ./build.sh   # use a different R3D source tree (default submodules/r3d)
CC=gcc ./build.sh               # use another compiler

./build.sh --win                # -> build/win/monolith.exe + build/win/maps/ (MinGW-w64, -O2)
./build.sh --win run [args]     # build, then run under wine
```

Both output folders, `build/linux/` and `build/win/`, are **self-contained**: copy or zip the folder as-is to run it on another computer.

**R3D** is built by `build.sh` itself: on first use it configures `$R3D_DIR/build-linux` (Linux) or `$R3D_DIR/build-mingw` (Windows) and after that runs an incremental `cmake --build` each time (a ~1 s no-op unless R3D's sources changed; output in `<build dir>/monolith-build.log`). The first build compiles assimp and takes a few minutes. Delete the build directory to force a fresh configure.

**Linux:**
- R3D is configured in `$R3D_DIR/build-linux`: static libraries, vendored raylib + assimp + zlib (`ASSIMP_BUILD_ZLIB`), assimp's bundled unzip (`ASSIMP_BUILD_MINIZIP`; a system minizip breaks the build), no examples, the GPU pass profiler compiled in (`R3D_GPU_PROFILER`, off at runtime unless `--bench` turns it on; R3D patch 5), and **`CMAKE_BUILD_TYPE=Release`** (since 2026-10-10). Before that there was no build type, so R3D/raylib/assimp were compiled **without optimisation**, like the original `~/Projects/r3d/build`.
  - `build.sh` remembers the configure options in `build-linux/monolith-configure.args` and re-configures the existing dir when they change (the first build after this change, or after switching `R3D_BUILD_TYPE`, rebuilds all of R3D/raylib/assimp: a few minutes). `R3D_BUILD_TYPE=None ./build.sh` brings back the old unoptimised build for comparisons; any other value is passed as `CMAKE_BUILD_TYPE`.
  - **Comparability:** every bench report now states the R3D build type (`R3D build` row; reports without that row are unoptimised builds). Measured with interleaved headless pairs (4 pairs, 2026-10-10), Release vs. unoptimised made **no measurable difference**: 4.952 vs. 4.969 ms average, CPU time per frame 0.500 vs. 0.517 ms, time inside `R3D_End()` 0.358 vs. 0.371 ms. The CPU side is dominated by GL driver calls, not R3D's own code, and the frame is GPU-bound (~4.4 of ~5.0 ms). So older reports stay roughly comparable; the break is noted anyway.
- The executable links R3D, raylib, assimp, zlib and **libstdc++ statically** (`-l:libstdc++.a`; the host's libstdc++ is newer than most distros'). Its only runtime dependencies are `libc`, `libm`, `libgcc_s.so.1` and the GPU driver's `libGL.so.1`, all present on any desktop Linux. No rpath, no `LD_LIBRARY_PATH`. Check with `readelf -d build/linux/monolith | grep NEEDED`.
- **glibc ≥ 2.34** (Ubuntu 22.04+, Debian 12+, Fedora 35+, RHEL 9+). Linking against the host's glibc 2.44 would otherwise require e.g. `acosf@GLIBC_2.43`. `build.sh` passes `-Wl,--wrap=` for those symbols, and `src/glibc_compat.c` routes them to old versions (libm functions to `@GLIBC_2.2.5`, `arc4random` to `getrandom`, `__isoc23_strtoul` to `strtoul`). After changing compiler, glibc or dependencies, check `objdump -T build/linux/monolith | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1` still prints `GLIBC_2.34`, and add any newer symbol to `GLIBC_WRAP` in `build.sh` and to `glibc_compat.c`.
- Data paths: the repo's `maps/` and `benchmark/` (baked in with `-DMAPS_PATH`/`-DBENCHMARK_PATH`) are used while they exist, so on this machine map and shader edits hot-reload and reports land in the repo. On any other computer the folders next to the executable are used (`maps/` is copied there by `build.sh`; `benchmark/` is created on the first `--bench`). Note that a portability test on this machine still uses the repo paths unless they're hidden, e.g. `unshare -rm sh -c 'mount -t tmpfs none ~/Projects/monolith && /path/to/copy/monolith --shot /tmp/x.png --view 2'`.
- x86-64 only (no ARM build).

**Windows (`--win`)** cross-compiles with `x86_64-w64-mingw32-gcc`:
- R3D is configured in `$R3D_DIR/build-mingw` with `cmake/mingw-w64-x86_64.cmake`: Release, static libraries, vendored raylib + assimp + zlib, no examples.
- The exe is linked **fully static** (`-static`: R3D, raylib, assimp, zlib, libstdc++, libgcc, winpthread), so `build/win/` holds just `monolith.exe` (~7 MB, stripped) and a copy of `maps/`, with no DLLs to ship. Its imports are only system DLLs: `opengl32`, `gdi32`, `user32`, `kernel32`, `shell32`, `winmm` and the UCRT `api-ms-win-crt-*` set (part of Windows 10+). Check with `x86_64-w64-mingw32-objdump -p build/win/monolith.exe | grep "DLL Name"`.
- It's a console-subsystem exe, so `--bench` tables and the **P** output are visible; started from Explorer it opens a console window next to the game.
- Bench reports from the Windows build go to `build/win/benchmark/`, not the repo's `benchmark/`.
- `--win debug` is refused: ASan/UBSan don't work with MinGW.
- Verify like Linux, under wine (it uses the host GL driver, so shots are pixel-identical to the Linux build): `wine build/win/monolith.exe --shot win.png --view 2`, or `__GL_SYNC_TO_VBLANK=0 wine build/win/monolith.exe --bench --headless --bench-frames 150`. The `MESA-EGL: warning` lines wine prints on NVIDIA are harmless.

A change is done only when it **builds with no warnings** on both targets (`./build.sh` and `./build.sh --win`), **`tools/leakcheck.sh` reports zero leaks**, and it has been **checked visually** (see §8).

**Leak check** (`tools/leakcheck.sh`; required after every code change, not after map-only edits). It builds `build/linux-lsan/monolith` and `examples/build/linux-lsan/` (LeakSanitizer, `-O1 -g`) when a source is newer, then runs, all in hidden windows or without one: `--dump-boxes`, `--dump-scene` (with and without models), `--shot` of the default map (incl. `--shadow-camera --fog-cache`), of the test package `tests/models/` (models, textured and shader materials, shadowed lights; CC0), of `examples/01_minimal/scene` and `maps/pine_forest`, `--render-scale` / `--upscale`, `--ao baked` / `--ao-volume` (also across a reload), `--reload-test` in both directions and into a broken map, a broken map on the command line, `--shot-hud` with `--select` / `--region`, `--submit` on both maps with `tools/stub-terminal.sh` as the terminal and `MONOLITH_WORKSPACE=`, `--export`, `--inspect-model`, `--new-map`, `--import` (stub terminal), `--classify`, and every example's `--shot` (and the capture shim). Created bundles, exports and map packages go to the trash afterwards. It prints one line per run and exits 1 if any leak is ours or a run failed; logs are in `build/leakcheck/latest/logs/`. Classification (`tools/lsan_report.py`): a leak is ignored only when the frame that **allocated** it (the first frame after malloc / operator new / strdup and libc internals) is in a GPU driver or system library listed in `tools/lsan.supp` (NVIDIA / Mesa GL drivers, libglvnd, X11 / xcb, D-Bus, the dynamic loader); anything allocated in the monolith binary (our code, raylib, GLFW, R3D, assimp: all static) always counts. On the dev machine the only ignored leak is the NVIDIA driver's: 8 allocations, 366 bytes, from context creation, in every program that opens a window. `--bench` adds a 10-frame headless bench with `--bench-window` (opt-in: only one benchmark may run at a time). `--asan` runs the same paths with the ASan + UBSan build and also fails on any sanitizer error. GL objects that are never deleted are not heap leaks (the driver frees them with the context), so `Unload*` calls still need care by review.

**Memory rules** (editor and runtime; details in `docs/design-v2.md` §2.7):
- **No fixed-size content buffers.** Anything whose size depends on the content (the map's boxes, models, groups, materials, lights, lines; the selection, regions, prompt text, open requests, child processes, bench frames, export file lists) is a heap array grown with `MS_RESERVE` / `MS_PUSH` (`runtime/monolith_scene.h`, used by `src/` too) or a heap string (`MS_StrDup`). Fixed `char` arrays remain only for stack scratch that formats one path or message, and for single values the loader bounds.
- **Sanity caps instead of limits**, each failing with a clear message: `MS_MAX_OBJECTS` (1 000 000 boxes + model instances + lights per map), `MS_MAX_FILE_BYTES` (4 MB map file), `MS_PATH_MAX` (1024-byte paths the loader builds), 3 `repeat`s per line, a 1 MB prompt, `BENCH_FRAMES_LIMIT`.
- **The per-frame path never allocates** (drawing, picking, HUD, collision): scratch arrays grow to the largest size seen and are reused. Allocation happens on (re)load, on input events, and at submit / export / report time.
- **Everything is freed**: `MS_Unload` frees all the runtime owns, the editor frees its state at exit, and `tools/leakcheck.sh` proves it.


## 5. Controls and command line

The header comment at the top of `src/main.c` is the source of truth; keep it updated when adding controls or flags.

**Walk mode (default)**

| Key | Action |
|---|---|
| WASD + mouse | Walk and look |
| Shift | Walk faster |
| Space | Jump |
| F | Toggle fly (noclip) |
| 1–6 | Toggle volumetric fog / distance fog / SSAO / SSIL / bloom / sun, each relative to the map's setting (the flips survive hot reloads) |
| F1 | Toggle HUD (also hides the crosshair and selection overlay) |
| F2 | Graphics settings panel (both modes; "Graphics settings" below) |
| F12 | Save `screenshot.png` |
| F9 | Export the map into `export/<map>-<YYYY-MM-DD_HHMMSS>/` next to `requests/` (as `--export`; the result shows in the bottom HUD line; not while the package has a load error) |
| TAB | Toggle spectator mode |
| Esc | Quit (closes the prompt box / leaves snip mode instead while open) |

Esc quitting is handled in the main loop (`SetExitKey(KEY_NULL)`), so that Esc can close the prompt box. The window's close button still quits. The bottom-left HUD line shows map reload messages: `map reloaded (N boxes)`, or the parse error in red, which stays until the file is valid again. The list of open and finished requests is shown in both modes.

**Spectator mode (TAB)**

| Key | Action |
|---|---|
| WASD + mouse | Move and look |
| E / Q (or Space) | Up / down (Ctrl isn't "down": it's the group-select modifier) |
| Mouse wheel | Base speed, 1–400 m/s, exponential steps |
| Shift / Alt | ×4 faster / ×0.25 slower |
| ← / → | Fly to the previous / next preset viewpoint (eased, arcs upward on long trips) |
| P | Print the current view as `--pos … --look …` (**feet** position) and copy it to the clipboard |
| G | Move the player to the camera position |

**Map designer (spectator mode)**: `designer.c`, `launcher.c`

| Key | Action |
|---|---|
| Crosshair | The box or model under it gets a white outline (models: their rotated bounds) and a cyan hit marker. The HUD shows its group, material, map line (with the line's text), index and distance (models: file and transform too). |
| LMB | Select the object under the crosshair (replaces the selection) |
| Shift + LMB | Add / remove that object |
| Ctrl + LMB | Select the object's whole group (replaces the selection) |
| Ctrl + Shift + LMB | Add the whole group |
| RMB | Clear the selection |
| Enter | Open the prompt box |
| C | Snip mode (screen regions): camera frozen, cursor free. Drag = a region (replaces the others), Shift + drag = add one, Enter = prompt box with the regions and the box selection attached, RMB / Esc / C = leave and drop the regions. Esc in that prompt box goes back to snip mode with the regions; a successful submit ends it. |
| R | Fly back to the camera of the most recently finished request |
| K | Close the terminal window of the most recently finished request |
| Shift + K | Close the terminals of all finished requests |
| Ctrl + K, then n | Close request [n] of the HUD list (1–9, 0 = 10), running ones too: a request without `result.md` needs n pressed again within 3 s. Esc cancels; meanwhile the digits don't toggle effects. |

**Request list and closing.** The HUD numbers the requests (`[n]`, kept while listed). Finished ones show for 20 s, then stay as a dim line while their terminal is still open. A closed request leaves the list; its bundle stays. Closing (`launcher.c`, region "Closing request windows"): on i3 / sway the workspace helper gives each terminal window the mark `_monolith_<id>` when it appears (Claude Code may retitle the window later, the mark stays), and a close runs `i3-msg '[con_mark="^_monolith_<id>$"] mark --add …, kill'`: exactly that window, no focus or workspace change. If the terminal process is still alive 3 s later, or there is no i3 / mark, the launcher sends SIGTERM to the terminal's process group (it was started with `setsid`), then SIGKILL after 3 s, only while it is the game's own unreaped child. The HUD says `closed <slug>` or `<slug>: window already gone`. kitty `--single-instance` / wezterm mux exit at once, so only the i3 route reaches their windows. Windows: the entry only leaves the list (close the window by hand).

Selected objects get an orange tint and outline, plus a faint always-on-top outline so they can be found behind walls. The HUD shows how many objects are selected and their group names. The selection survives map hot reloads: objects are matched by identical geometry first (models: world bounds), then by the same map line + index within that line + group name. The HUD reports any that were lost.

**Prompt box.** Typing; Backspace / Delete (Ctrl+Backspace deletes a word); ← → ↑ ↓; Home / End (with Ctrl: the whole text); Ctrl+V paste; Shift+Enter new line; **Enter sends** the request; **Esc cancels** and discards the text. The text has no fixed limit (a sanity cap of 1 MB stops a runaway paste); its size in bytes is shown as a counter. While the box is open, every game key and the camera are frozen and the mouse cursor is free (they stay frozen for 2 more frames after it closes). An empty prompt can't be sent. If sending fails, the box stays open with the text, and the HUD shows the error and the bundle path.

**Command line** (the `main.c` header comment is authoritative). An unknown flag, a missing or unparsable value, a value that is itself a `--flag`, or a non-finite / out-of-range number (positions, angles and regions: |v| ≤ 1e6) prints a message and exits 1 before any window opens.

| Flag | Meaning |
|---|---|
| `--pos x y z` | Player start position (**feet**; default: the map's `spawn`) |
| `--eye x y z` | Start position of the **eye** (= `--pos x y-1.62 z`). With `--view`, it moves the spectator camera there instead. Request bundles use it in their reproduction command. |
| `--look yawDeg pitchDeg` | Start view angles. With `--view`, they override the viewpoint's angles. |
| `--view n` | Start in spectator mode at the map's viewpoint *n* (1-based, the order of its `viewpoint` lines) |
| `--off <digits>` | Start with these effect toggles off, e.g. `--off 16` |
| `--ao <mode>` | Screen-space AO / indirect trade-off (`atmosphere.c`, §7.5): `full` (default), `ssao` (SSIL off), `lite` (SSIL off, SSAO 8 samples), `off` (SSAO + SSIL off), `baked` (SSAO + SSIL off, replaced by the baked occlusion volume, `--ao-volume 0.5 0.6`; §7.3 "Trade-off knobs"). Measured on the default map 2026-10-10 (headless, spinning): −0.62 / −0.67 / −0.93 ms per frame; the look barely changes (soft darkening along wall feet in close, shaded views goes away; no visible bounce loss). |
| `--ao-volume <voxel> <intensity>` | **Trade-off, off by default.** Bake a world-space occlusion volume (signed distance to the nearest box, 1 byte per voxel, ≤ 32 MB, ~60 ms on the CPU per map load) and darken the ambient light with it (R3D patch 11, `MS_RenderSettings.aoVolumeVoxel` / `aoVolumeIntensity`). View-independent, sees off-screen occluders, ~0.03 ms per frame. On top of SSAO/SSIL unless `--ao baked`. Boxes only (models are not in it). |
| `--render-scale <s>` | **Trade-off, default 1.** R3D renders at `s` (0.25–1) times the window / shot size and upscales (`--upscale nearest\|linear\|bicubic\|lanczos`, default bicubic; `MS_RenderSettings.renderScale` / `upscale`, `MS_ApplyRenderResolution`). |
| `--window-msaa` | 4× multisampled window, as before 2026-10-11: smoother designer overlay lines for ~0.1 ms GPU per frame. The scene image is the same either way (R3D writes one colour per pixel). Default: single-sampled. |
| `--bloom on\|off` | `off` skips the bloom pass (−0.25 ms) and raises exposure by the map's `env bloom.offExposure` instead (default ×1.19): in the default map bloom (threshold 0, MIX 0.04) is mostly a uniform ~20% lift, so the mean brightness stays within ±1.2/255; only the faint halo around bright openings goes. A map with a stronger bloom sets its own factor (`maps/pine_forest`: 1.3, measured over its 5 viewpoints). |
| `--quality low\|medium\|high\|ultra` | Graphics quality preset for this run ("Graphics settings" below; `MS_Quality` in the runtime). Default: the settings file in the interactive game, else ultra (= the map exactly). Not saved. |
| `--settings <file>` | Graphics settings file to read (and, from the panel, write) instead of `~/.config/monolith/settings.ini` (`$XDG_CONFIG_HOME`; Windows `%APPDATA%\monolith\settings.ini`; else next to the executable). |
| `--no-settings` | Neither read nor save a settings file. |
| `--settings-set key=value` | Change one setting as if in the panel (repeatable, applied after every other flag): it applies to this run **and is saved** to the settings file (if one is in use). Keys are the file's, e.g. `preset=medium`, `render_scale=0.75`, `ao=baked`, `shadow_size=4096`, `fps_cap=monitor`. Works without a window with `--dump-boxes` (to write the file). |
| `--settings-change key=value` | With `--shot`: after 10 frames apply the change through the panel's live path (repeatable; `shadow_size` restarts the renderer). The shot must equal one started with `--settings-set key=value` (verified for every preset, shadow size, AO, fog cache, render scale, range, AA, sun shadow: 0 pixels differ). |
| `--settings-panel` | With `--shot --shot-hud`: draw the open settings panel into the shot (to review it). |
| `--fx key=value` | Override one effect setting, repeatable, applied after `--ao`/`--bloom`: `ssao.samples`, `ssao.radius`, `ssao.intensity`, `ssao.power`, `ssil.samples`, `ssil.radius`, `ssil.gi`, `ssil.ao`, `bloom.levels`, `bloom.intensity`, `exposure`, `vfog.step`, `vfog.length`. For experiments. |
| `--map <path>` | Load this map package (folder) or `.map` file instead of `maps/monolith/` (path relative to the working directory). A folder loads `<folder>/<folder name>.map`, else its only `.map`. It is hot-reloaded like the default map (every file of the package). |
| `--new-map <name\|path>` | Create the package `maps/<name>/` (`<name>.map` with a ground slab, the default material, the built-in environment written out, a spawn and two viewpoints; a `MAP.md` template; empty `shaders/ models/ textures/` and `ASSETS.md`), check that it loads, and exit without a window. An argument with a `/` is a path: the package is that folder (its parent must exist; the map name is the last component), e.g. a draft in a scratch folder. Refuses to overwrite. |
| `--export <dir>` | Write the map (`--map` or the default) for use in another raylib + R3D project and exit **without a window**: `<dir>/<name>/` (the package with every file the map references, `MAP.md`, `ASSETS.md`, model sidecars and the files model files refer to), `monolith_scene.h/.c`, `viewer.c` (= `examples/01_minimal/main.c`) and `README.md`. Refuses a non-empty `<dir>`; checks that the copy loads. §6.3. |
| `--import <project dir> <name\|path>` | Create `maps/<name>/` or the folder a path names (as `--new-map`) and open a Claude Code **import session** on a bundle `requests/<stamp>_import-<name>/` (`request.md` with an *Import* section, `request.json` `kind: "import"` + `import` object, heavy model tier); no window. Same terminal, permission flags and workspace as edit requests. The session follows `AI-MAP-TOOL.md` §10. |
| `--dump-boxes <file>` | Load the map, write every box as hex floats (`%a`, one `min max` line per box) to `<file>` and exit **without opening a window**. Use it to check that an edit keeps the exact box list, and as a quick parse check (a broken map prints `file:line: message` and exits 1). Model files are only checked for existence. |
| `--dump-scene <file>` | Like `--dump-boxes`, plus model files, model instances (transform and world bounds as hex floats), model colliders, materials and lights (format: `src/dumpscene.c`). No window without models; a map with models loads them in a hidden window. Diff two dumps to check a model edit. |
| `--dump-scene-diff <file>` | The same items without object indices and map lines; groups, materials and model files by name, a model's collider count only (`src/dumpscene.c`). Rows don't change when lines are inserted above them, so the diff of two such dumps shows only the objects an edit changed (inserting a lantern model + its light into `pine_forest`: 1504 changed rows with `--dump-scene`, 6 with `-diff`). |
| `--inspect-model <file>` | Print what a model file contains and exit, no window: bounds, size, origin, mesh/triangle counts, materials (the slots for `material.N`) and textures (embedded/external, sizes), up axis and unit hints, glTF extensions, warnings (unlit or fully metallic materials, > 50k triangles, textures > 2K, unsupported extensions, no meshes), a placement line. A model whose references (glTF buffer/image URIs, OBJ `mtllib`, .mtl texture maps) leave its folder is refused before anything is read (exit 1), like the map loader does for the package. |
| `--shot <file>` | Render 30 frames offscreen, save the image to `<file>` (full path OK, 1600×900 unless `--shot-size w h`), exit. Uses a **hidden window** (`FLAG_WINDOW_HIDDEN`), so the window manager (i3) never maps, shows or focuses it. Hot reload and request polling are off during shots. |
| `--shadow <size> <range>` | Sun shadow map size (px) and shadow distance (m), overriding the map's `env shadow.size` / `sun.range` (built-in default and default map: `8192 100`). Clamped to 256–16384 px, 10–2000 m. |
| `--shadow-camera` | Sun shadow from R3D's camera-fitted map, re-rendered only when the cached map no longer covers the view (the pre-2026-10-10 path, §7.3). The default is static world-fixed tiles drawn once per map load (R3D patch 7). For A/B benchmarks and debugging. |
| `--shadow-always` | Camera-fitted map re-rendered every frame (the original behaviour). For A/B benchmarks and debugging. |
| `--fog-cache <voxel> <sub>` | **Trade-off, off by default.** Bake the volumetric fog's sun visibility into an 8-bit 3D texture once per map load (voxel size in m, sub³ samples per voxel) and let the fog march fetch it instead of the shadow map (R3D patch 8). The region is the map's `env fogCache.region`, else its content bounds + 2 m, shrunk to at most 64 M voxels (§7.3); outside it the march falls back to the shadow map. Default map, `0.5 2`: 290×202×510 voxels, 28.5 MB, 17–55 ms bake (logged), −0.2 ms per frame, shaft edges in the hall ~0.5 m softer (0.5% of pixels change by > 2/255, max 20). Voxel size is clamped to 0.25–8 m, sub to 1–4. |
| `--no-prepass` | No depth pre-pass before the G-buffer pass (R3D patch 6). Same image; for A/B tests (without it a material's surface shader, e.g. the default map's concrete, runs for every overdrawn fragment). |
| `--bench` | Benchmark: for each of the map's viewpoints, 60 warmup frames, then 600 measured frames while spinning the camera 360°. Prints a table and writes a **Markdown report** to `benchmark/<timestamp>_shadow-<size>-<range>.md` (GPU, driver, resolution, settings, command; avg ms, avg FPS, 1% low, worst frame and number of shadow-map re-renders per view). Reports are written only for completed runs. |
| `--bench-window <samples>` | With `--bench --headless`: render into an offscreen stand-in for the visible window (`windowemu.c`: a framebuffer with `<samples>` samples, 4 = `--window-msaa`, 0 = today's window), draw the spectator HUD and overlay on top and resolve it like the swap, so the window-only costs can be timed without a visible window. |
| `--headless` | With `--bench`: hidden window (never mapped), offscreen 1920×1006 target, `glFinish` after every frame. Doesn't touch the screen or need workspace 10 shown. Absolute times are ~10% higher than visible runs (no CPU/GPU overlap) and noisier if the machine is in use, so compare headless only with headless. |
| `--bench-frames <n>` | Timed frames per viewpoint for `--bench` (at least 10, default 600; sanity cap 1 000 000, the tables are heap arrays). Use ~150 for quick screening runs (~10 s); keep 600 for numbers you report. Fewer frames = faster spin, so more shadow re-renders. |
| `--bench-still` | Like `--bench`, but the camera doesn't spin (the still-camera case: no shadow re-renders). Report suffix `_still`. |
| `--no-frame-reuse` | Always render with R3D. By default the **interactive** window reuses the last R3D image while nothing that affects it changed (camera, effect toggles, sun, map version, window size); such a frame is only the cached image + HUD/overlay, the HUD fps reads `(cached)`, and the loop then sleeps to the monitor's refresh rate instead of spinning (§6, Frame reuse). |
| `--frame-reuse` | Turn frame reuse on where it is off by default: in `--shot` (to prove a reused frame equals a fresh one; logs how many frames were reused) and in `--bench-still` (times a reused frame; report suffix `_reuse`, no GPU profile). |
| `--no-profile` | With `--bench`: no per-pass GPU table. The R3D GPU profiler (patch 5) is compiled in by `build.sh` (`R3D_GPU_PROFILER`) and switched on only by `--bench`; every report then lists the GPU time per pass (geometry, shadows, SSAO, SSIL, fog march, …). |
| `--note "text"` | Free-form note placed at the top of the benchmark report (e.g. what changed). |

**Designer debug flags** (for testing without a visible window)

| Flag | Meaning |
|---|---|
| `--pick` | With `--shot`: print what the crosshair (screen centre) hits on the shot frame: box or model index, group, map line + its text, point, normal, distance, min/max (models: file, position, rotation, scale, origin, world bounds). Works in walk mode too. |
| `--pick-at x y` | With `--shot`: the same for the ray through pixel (x, y) of the shot (top-left origin, like `--region` and `view.png`; repeatable, each after a `pick-at x y` line). The shot centre gives exactly `--pick`. For probing the edges of a gap or any other point of a request's view. |
| `--select <line>` | Select every box or model made by this map line (repeatable) |
| `--region x y w h` | Attach a screen region (pixels of the shot, top-left origin) as if dragged in snip mode (repeatable). Drawn with `--shot-hud`, sent with `--submit`. |
| `--submit "text"` | With `--shot`: after saving the shot, run the real submit path. It renders `view.png` + `view-marked.png` offscreen and calls `LauncherSubmit` with the centre pick, the `--select` boxes and the `--region` regions, then prints `submit: ok/failed: <message>`. **It writes a real bundle into `requests/` and launches a real terminal + Claude Code**, so for tests set `MONOLITH_TERMINAL` to a stub script that only logs its arguments, and move the bundle to the trash afterwards (`gio trash requests/<id>`, never `rm`). |
| `--submit-wait <s>` | With `--submit`: keep polling the request (`result.md`, terminal process) for `<s>` seconds before exiting. |
| `--close-submitted` | With `--submit`: then close the submitted request like Ctrl+K n (twice if it is still running), wait up to 10 s for its terminal process to go, and print `close [n]: …` / `close: terminal process gone after X s`. Test with a stub terminal. |
| `--type "text"` | Open the prompt box with this text (cursor at the end) |
| `--classify "text"` | Print the model the launcher would choose for this prompt (tier, model, reason, source, the prompt after stripping an `@tier` token) and exit **without opening a window**. Context: the loaded map (empty? `MAP.md`?), the `--select` lines, the `--region` count and a crosshair hit; `--classify-kind edit\|import\|new-map` sets the request kind (default `edit`). Honours `MONOLITH_MODEL*` and `MONOLITH_CLAUDE_ARGS`. |
| `--shot-hud` | With `--shot`: draw the HUD, crosshair, selection overlay, regions and prompt box into the shot. Plain shots never contain them. |
| `--shot-size w h` | Size of `--shot` images (default 1600 × 900, clamped to 64–8192). Request bundles pass the window size in their reproduction command so it frames like `view.png`. |
| `--reload-test <path>` | With `--shot`: after 10 frames, load `<path>` (package or `.map` file) through the real hot-reload path, then render the rest. The shot must equal a plain `--shot --map <path>`: this proves that every static cache (instances, models, shadow tiles, fog volume, AO volume, frame reuse) is rebuilt on reload. |

In `--submit`, `--type` and `--classify` text, a literal `\n` becomes a new line.

**Environment variables** (launcher)

| Variable | Meaning |
|---|---|
| `MONOLITH_TERMINAL` | Terminal to open, as a program name or path plus optional extra arguments (`"kitty --single-instance"`). It takes precedence over `TERMINAL`. |
| `TERMINAL` | Used when `MONOLITH_TERMINAL` is unset. If neither is set, the first of `alacritty`, `kitty`, `foot`, `wezterm`, `xterm` found in `PATH` is used. |
| `MONOLITH_CLAUDE_ARGS` | Extra Claude Code flags, split on whitespace (no quoting). Default: none. They follow `--allow-dangerously-skip-permissions --dangerously-skip-permissions` (every session gets them by default; the guard rails are in `CLAUDE.md` / `AI-MAP-TOOL.md`) and `--model <name>`. A `--model x` / `--model=x` in them replaces the chosen model (no second `--model` is added). |
| `MONOLITH_MODEL` | One model for every request (any `--model` value, e.g. `opus`); turns the per-request choice off. |
| `MONOLITH_MODEL_FAST`, `MONOLITH_MODEL_DEFAULT`, `MONOLITH_MODEL_HEAVY` | Model of each tier (defaults `haiku`, `sonnet`, `opus`: Claude Code's aliases for the latest model of each family). Every request gets a tier from its prompt and context (`ClassifyRequest`, rules in the "Model choice" comment of `launcher.c`): **heavy** for imports / new maps, an empty map or one without `MAP.md`, asset words (model, tree, statue, lamp, ...), look/engine words (shader, material, light, fog, sky, sun, ...), large design words (build, design, district, building, ...), > 280 bytes, 3+ sentences or > 2 regions; **fast** for one short sentence (<= 120 bytes) with a local edit word (move, raise, taller, delete, rename, mirror, ...), no creation word (add, put, place, ...) and a target (hit, selection or one region); **default** otherwise. The choice is written to `request.md` (`Model:` line) and `request.json` (`model`), and the HUD says `request sent (<model>): ...`. |
| `@fast` / `@default` / `@heavy` (prompt prefix) | Not a variable: a leading `@fast`, `@default`, `@heavy` (or `@haiku`, `@sonnet`, `@opus`, same tiers) in the typed prompt picks the tier for that request and is stripped from the prompt written to the bundle (`request.json` `model.override: true`). `MONOLITH_MODEL` and a `--model` in `MONOLITH_CLAUDE_ARGS` still win. |
| `MONOLITH_CLAUDE_SAFE` | Set to `1` (any value but empty or `0`) to drop the two permission flags, so Claude Code asks before each command as usual. |
| `MONOLITH_WORKSPACE` | i3 / sway workspace the new terminal is moved to (default `10`; empty = leave it where it opens). A number uses `workspace number N`, anything else a workspace name. Done with `i3-msg` (`swaymsg` when `SWAYSOCK` is set) and the criteria `[title="^monolith-claude: <slug> <HHMMSS>$"]` (titles are unique per request), retried by a detached helper every 0.1 s for up to 10 s; the visible workspace doesn't change and focus returns to the previous window. Without a `for_window` rule the window flashes up briefly where it opened; `for_window [title="^monolith-claude: "] move container to workspace number $ws10` in the i3 config avoids that. Other window managers: nothing happens. The same command marks the window `_monolith_<id>` (also when the variable is empty), for closing it. |
| `MONOLITH_CLOSE_ON_EXIT` | `1`: when the game quits, close the terminals of all finished requests (default off). |
| `MONOLITH_CLOSE_METHOD` | `wm` or `signal`: close terminals only through i3 / sway or only by signals (for tests; default both, WM first). |

**Other environment variables**

| Variable | Used by | Meaning |
|---|---|---|
| `MONOLITH_CAPTURE`, `MONOLITH_CAPTURE_FRAME` | the capture shim (`runtime/monolith_capture.h`) | Output package folder; the frame to record (default 3). Without `MONOLITH_CAPTURE` the shim does nothing. |
| `MONOLITH_DOWNLOADS` | `tools/fetch_asset.py` | Parent folder for downloads (default `~/.cache/monolith/downloads`). |
| `POLY_PIZZA_API_KEY` | `tools/fetch_asset.py` | The developer's Poly Pizza API key (or `~/.config/monolith/poly-pizza.key`); never printed or written. |
| `MONOLITH_STUB_LOG` | `tools/stub-terminal.sh` | File the stub terminal writes its argv to. |
| `XDG_CONFIG_HOME` / `APPDATA` | `settings.c` | Where the default settings file lives (`--settings`). |
| `__GL_SYNC_TO_VBLANK=0` / `vblank_mode=0` | the GL driver (NVIDIA / Mesa) | Driver vsync off, for `--bench`. |

### Graphics settings (F2, `settings.c`, `settingspanel.c`, runtime `MS_Quality`)

**F2** (walk and spectator mode) opens a panel: the preset selector, then Display, Shadows, Ambient occlusion, Fog, Post and Performance rows, a frame time readout (fps, CPU ms, GPU ms from R3D's profiler, which the panel switches on while it is open), **Reset to preset** and **Close**. UP/DOWN select, LEFT/RIGHT change (or the mouse: hover, click the arrows or either half of the value, wheel); values that differ from the preset are drawn in blue. Changes apply at once; heavy ones (shadow map size, AO mode, fog cache, shadow mode/range, preset) one frame later behind an "applying ..." note. A new **shadow map size** closes and re-initialises R3D and loads the map again (~1 s; the selection is kept); window MSAA applies at the next start. While the panel is open the camera and game keys are frozen and the cursor is free. ESC / F2 close it, and closing saves the settings file.

**The map stays the source of the look.** Every scene setting is relative to the map's environment (0 / x1 / "map's" = unchanged): ultra renders exactly what the map describes (pixel-identical, the default), and an effect the map turns off stays off. The toggles 1-6 still flip effects on top, `--fx` still applies after the settings. Everything that changes the image is in `FrameKey` (the whole `GfxSettings` plus R3D's AA mode).

**Settings and their cost** (single-knob measurements on the default map from §7.3 "Trade-off knobs", headless 1920×1006, default 3.59 ms in that session):

| Group | Setting (file key) | Values | Cost / effect |
|---|---|---|---|
| Display | render scale (`render_scale`), upscale filter (`upscale`) | 0.5–1, nearest/linear/bicubic/lanczos | 0.75: −39%, 0.5: −55%; softer edges and grain |
| | anti-aliasing (`anti_aliasing`) | none / fxaa / smaa | FXAA is today's; SMAA sharper, a bit more GPU |
| | window MSAA (`window_msaa`) | on/off, next start | ~0.1 ms, overlay lines only |
| | exposure, brightness, contrast, saturation | multipliers, 1 = the map's | display trims, free |
| Shadows | sun shadow (`sun_shadow`), map size (`shadow_size`), range (`shadow_range`), softness (`shadow_softness`), mode (`shadow_mode`) | on/off; map's/2048–16384; map's/60–300 m; map's/0.5–6; tiles/camera/always | 8192 = 3.7 cm texels, 256 MB VRAM; 4096 = 7.4 cm, 64 MB. Below the map's `shadow.size` the sun's depth and slope biases are scaled by the texel ratio (`SunShadowMap()`), so low sun angles get no acne stripes at 4096. Range below the fog length (90 m) fades the shafts early |
| Ambient occlusion | mode (`ao`), baked voxel / intensity, SSAO samples/radius/intensity, SSIL samples/radius | full/ssao/lite/baked/off | baked: −27% (crisper wall-foot contact shadow, no SSIL bounce); off: ~−0.9 ms |
| Fog | volumetric fog, march step (`fog_step`, x the map's 0.75 m), length, fog cache voxel/samples, distance fog | | fog cache 0.5 m + step x2: −16%, shaft edges ~0.5 m softer |
| Post | bloom, bloom intensity, SSR, SSGI (off / map's / on) | | bloom off: −6% (exposure x the map's `bloom.offExposure`, default 1.19, keeps the mean); SSR/SSGI cost extra (off in the default map) |
| Performance | frame reuse, fps cap (off/monitor/30–240), vsync, depth pre-pass | | pre-pass off = same image, slower |

**Presets** (`MS_QualityPreset`; measured 2026-10-11 on the default map: headless `--bench`, 1920×1006, spinning camera, 3 interleaved rounds):

| Preset | What changes vs. ultra | Frame (headless, default map) |
|---|---|---:|
| **ultra** (default) | nothing: the map exactly | 3.17 ms |
| **high** | AO baked (0.5 m, 0.6) instead of SSAO + SSIL; fog cache 0.5 m | 2.07 ms |
| **medium** | high + render scale 0.75 (bicubic), fog march step x2, shadow map 4096 | 1.32 ms |
| **low** | render scale 0.5, AO off, bloom off (exposure x `bloom.offExposure`), fog cache 0.5 m + step x2, shadow map 4096 | 0.77 ms |

**The settings file** is plain `key = value` text (`settings.c`: `preset = low|medium|high|ultra` sets every scene setting, later keys override single ones; `custom` files list every key). Only the interactive game reads the default file; **`--shot`, `--bench`, `--dump-*`, `--export` and `--classify` never read it** unless `--settings <file>` names one, so verification shots and benchmarks stay deterministic. Command-line flags (`--quality` first, then `--ao`, `--bloom`, `--shadow`, `--render-scale`, ...) override for one run and are never saved, also when the panel saves later: a panel change is written to the saved settings field by field, so a flag's override of another field stays out of the file. Unknown keys and bad values are logged and ignored.

**In your own program** the same presets come from the runtime: `MS_QualityPreset(level)`, `MS_ApplyQuality(scene, &q)` right after every `MS_ApplyEnvironment`, then `MS_ApplyRenderResolution`; `q.shadowSize` goes to `MS_LoadOptions.shadowSizeOverride` (an R3D init hint). `examples/01_minimal --quality medium` (= every export's `viewer.c`) renders the same pixels as `monolith --quality medium`.

Vsync is **off** by default and rendered frames are uncapped (the settings' vsync and fps cap change that in the interactive game only); reused (cached) frames sleep to the monitor refresh rate (frame reuse, §6). For benchmarks, also disable driver vsync:
`__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench` (NVIDIA) or `vblank_mode=0` (Mesa).

## 6. Code architecture (`runtime/`, `src/*.c`)

The scene itself (loading, drawing, collision, picking, hot reload) is the **scene runtime** in `runtime/` (§6.1). The editor in `src/` is split into modules, and each one has a header with the interface. `build.sh` compiles `src/*.c` and `runtime/*.c`, so a new module needs no build change. Larger files use `// #region` blocks.

1. **Scene runtime** (`runtime/monolith_scene.h/.c`, §6.1): the game holds one scene in the global `MS_Scene *scene` (`common.h`). It replaced the former `world.c`, `map.c`, `shadow.c` and the scene part of `atmosphere.c` / `main.c`.
2. **Player** (`player.c`): feet-based AABB (r 0.3, h 1.75, eye 1.62). Movement is per axis with collision against `scene->colliders` (boxes, then model colliders) using **`BoxesOverlap` (strict overlap, like the runtime's `MS_Collides`)**; see §7.4. It includes step-up for stairs (`STEP_HEIGHT` 0.45), gravity, jump, fly/noclip, smoothed acceleration and head bob. `PlayerCamera()` builds the camera. The player starts at the map's `spawn` unless `--pos` / `--eye` / `--look`.
3. **Spectator camera** (`spectator.c`): the `Spectator` state, the map's viewpoints (`scene->viewpoints`: name, eye, target; `viewpoint` lines), `UpdateSpectator()`, `SpectatorFlyTo()` / `SpectatorFlyToPose()` / `SpectatorJumpTo()`, and `SpectatorCamera()`. Yaw/pitch convention: `LookDir(yaw, pitch) = (cos p · sin y, sin p, cos p · cos y)`, so yaw 0 looks toward +Z and yaw 180° toward −Z. Camera right is `(−cos y, 0, sin y)`.
4. **Designer** (`designer.c`): spectator-mode editing UI. It holds one `Designer` state, owned by `main()`.
   - **Picking**: `DesignerPick()` calls `MS_Raycast()`: a slab test of the camera's centre ray against every box, then models (bounds, then triangles through a BVH). Boxes that contain the eye are skipped, so you can pick out of a wall you are inside. The result (object, point, face normal, distance) is refreshed every frame in spectator mode and in `--shot` runs.
   - **Selection**: any number of objects (`MS_ObjRef`: box or model; heap arrays, with an index -> position table for O(1) membership), with keys; `DesignerSync()` remaps them after a reload (§5) through a hash of their exact bounds.
   - **Screen regions** (snip mode, `--region`): rectangles in window pixels; at submit `RegionPick` samples each with up to 4096 rays (per-region candidate lists, culled per 8×8-ray tile) and lists the objects it shows with their share of the rays (also summed per map line in the bundle), the centre hit, the depth range and the sky share, plus the **edge objects**: up to 64 rays per side on a ring a few pixels outside the rect (`AnalyzeRegionEdge`), the objects they hit nearest first with their sides, i.e. what frames a gap the region looks through.
   - **Overlay**: `DesignerDrawOverlay()` draws with raylib after `R3D_End()`, inside `BeginMode3D` with the same camera. It is depth-tested against R3D's depth, which `R3D_End` copies into the target, with a 4-step depth bias in the projection matrix and slightly inflated boxes against z-fighting (models: their oriented bounds). It also draws the crosshair, the snip-mode drawing and the HUD lines.
   - **Prompt box**: UTF-8 editing, word wrap with raylib's default font, scrolling, heap text (1 MB sanity cap). `DesignerCapturesInput()` tells `main()` to suppress game keys and the camera.
   - `DesignerBuildRequest()` snapshots everything the launcher needs into a `DesignRequest`: prompt, eye, yaw/pitch, fovy, size, hit, selection, regions, map version.
5. **Launcher** (`launcher.c`): see its header comment.
   - `LauncherSubmit()` writes `requests/<YYYY-MM-DD_HHMMSS>_<slug>/` with `view.png`, `view-marked.png`, one `region-<n>.png` per region, `request.md` and `request.json` (format 2; contents in `AI-MAP-TOOL.md` §2). Then it starts the terminal with `fork` + `execve`, from an argv array with no shell, so the user's text never reaches a command line.
     - The child calls `setsid` and `chdir`s to the repo root, with stdin from `/dev/null`. Exec failures come back through a close-on-exec pipe. Children are reaped with `waitpid(pid, WNOHANG)`.
     - Each terminal's "run this argv" syntax (title, working directory, exec flag) comes from the `TERMINALS[]` table; an unknown terminal gets `-e <command>`. The window title is `monolith-claude: <slug> <HHMMSS>[-n]` (unique per request, so closing one never hits another); i3 rules match the prefix `^monolith-claude: `. A `--model` without a name in `MONOLITH_CLAUDE_ARGS` is dropped with a warning.
     - `claude` is searched in `PATH`, then in `~/.local/bin`, and passed as an absolute path, followed by `--allow-dangerously-skip-permissions --dangerously-skip-permissions` (unless `MONOLITH_CLAUDE_SAFE`), `--model <name>` (the request's tier, "Model choice" in `launcher.c`, `ClassifyRequest`) and `MONOLITH_CLAUDE_ARGS`.
     - A detached helper moves the window to i3 / sway workspace `MONOLITH_WORKSPACE` (default 10) and marks it `_monolith_<id>` for closing it later (K, Shift+K, Ctrl+K n; region "Closing request windows").
     - If launching fails, the bundle is kept, the HUD names its path, and the log prints the command to run by hand.
   - `LauncherUpdate()` polls every open request (a heap list; finished ones leave it once the HUD stops showing them and their terminal is gone) for `result.md` and shows its first line in the HUD for 20 s. `LauncherLastDone()` gives the camera for **R**.
   - **Windows**: the same bundle, then `cmd /c start "monolith-claude: <slug> <HHMMSS>" /D <exe dir> cmd /k "<claude>" … "<prompt>"` via `_spawnvp`. This path is only compile-tested.
6. **Atmosphere** (`atmosphere.c`): the environment is map data (`env` lines; built-in defaults in `BuiltinEnvironment()` in `monolith_scene.c`, applied by `MS_ApplyEnvironment`). The graphics settings (`MS_ApplyQuality`, which also covers `--ao` / `--bloom`) go on top; `atmosphere.c` keeps the `--ao` mode names and the `--fx` overrides, applied after them; `main.c` (`ApplyEnvironment()`) re-applies map environment, quality, `--fx` and the 1–6 toggle flips after every reload.
7. **Graphics settings** (`settings.c`, `settingspanel.c`): `GfxSettings` = the runtime's `MS_Quality` plus window options (MSAA, vsync, fps cap, frame reuse); the settings file, the command-line overrides and the F2 panel (§5 "Graphics settings").
8. **New map, export, import, inspection** (`newmap.c`, `export.c`, `import.c`, `inspect.c`, `dumpscene.c`): `--new-map`, `--export` / F9, `--import`, `--inspect-model` (assimp directly, no window), `--dump-scene` (§6.3, §5).
9. **Benchmark** (`bench.c`, `windowemu.c`): frame statistics and the Markdown report for `--bench`; `--bench-window`'s offscreen stand-in for the visible window.
10. **Main** (`main.c`):
    - the header comment (controls and flags), argument parsing, `InitDataPaths()`, and the map's parse check (+ `--dump-boxes`, `--dump-scene`, `--new-map`, `--export`, `--import`, `--classify`, all without a window or with a hidden one);
    - the shadow-size hint (`MS_SetInitHints`), `R3D_Init`, `MS_InitRenderer()` (`glEnable(GL_DEPTH_CLAMP)`, §7.2), then `MS_Load` and `MS_SetRenderSettings` (shadow mode, fog cache, pre-pass, the trade-off knobs `renderScale` / `upscale` / `aoVolumeVoxel`), then `MS_ApplyRenderResolution` (R3D's internal resolution = window × `renderScale`; also on resize). The window is single-sampled (`--window-msaa` brings back 4× MSAA);
    - the graphics settings (`settings.c`: the file, then `--quality` and the single flags, then `--settings-set`), applied by `ApplyEnvironment()` (map environment → `MS_ApplyQuality` → `--fx` → toggles → `MS_ApplyRenderResolution`); the F2 panel changes them live, and `RestartRenderer()` handles a new shadow map size (`MS_Unload`, `R3D_Close`, hint, `R3D_Init`, `MS_Load`);
    - the main loop: toggles, TAB, settings panel, map hot reload (`MS_Poll`), designer update, camera update, draw (with frame reuse, below), overlay, HUD, `--shot` / `--bench` handling;
    - **depth pre-pass** (R3D patch 6): a material with `alpha_cutoff 0` (it never discards; the default map's concrete) lets R3D lay depth first, so its surface shader runs once per visible pixel instead of once per overdrawn fragment. `--no-prepass` turns it off for A/B tests;
    - **frame reuse** (comment block "Frame reuse" in `main.c`): a `FrameKey` holds everything that changes R3D's image (camera, the whole `R3D_Environment`, the runtime's `MS_RenderSettings` incl. toggle 6, the graphics settings and R3D's AA mode, `scene->version`, window size; materials, models, lights and the sun only change with a reload, i.e. a new version), compared bytewise each frame. While it changes, R3D renders straight into the window. On the first unchanged frame R3D renders once more into a cache target (colour + depth *texture*); later unchanged frames only composite it with a shader that also writes `gl_FragDepth` (with `--window-msaa` the window is multisampled, and a blit into it would be an error), and the overlay/HUD go on top. A reused frame costs ~0.2 ms instead of a full render, and the loop then sleeps to the monitor's refresh rate. Interactive only; off in `--shot` / `--bench` unless `--frame-reuse`. **Anything new that changes the image (a uniform, a light, a setting) must go into `FrameKey`**, or a still camera will keep showing the old image;
    - `Submit()`, which renders the request images through the same `DrawScene()` as the main view, and cleanup.

Rendering per frame (`DrawScene()` in `main.c`, the same as the runtime's `MS_Draw`; `--shot` and the request images render into a `RenderTexture`, the window view into the default framebuffer):
```c
R3D_BeginPro((R3D_View){ .camera = R3D_CameraFromRL(camera), .target = target });
    MS_PushLights(scene, camera, aspect);
        // sun: tile = SunShadowTileSelect(..., camera.position, scene->version, &update);
        //      R3D_PushLightFixedShadow(sun, sunShadow[tile], tiles.tile[tile].viewProj, update); // once per map load
        //      (--shadow-camera: update = SunShadowNeedsUpdate(...); R3D_PushLightEx(sun, sunShadow[0], update))
        // then the map's point / spot / dir lights
    MS_DrawGeometry(scene);    // R3D_DrawMeshInstanced per material with boxes, R3D_DrawModelInstanced
                               // per (model file, slot materials, shadow flag);
                               // --ao-volume / --fog-cache: their bakes, once per map load
R3D_End();
// then, on the same target: DesignerDrawOverlay (depth-tested), crosshair, HUD, regions, prompt box, settings panel
```

### 6.1 Scene runtime (`runtime/monolith_scene.h` + `runtime/monolith_scene.c`)

A separately compilable C11 module that depends only on raylib (raymath, rlgl) and R3D. The editor, the examples and every exported scene use it; `monolith_scene.h` is the API contract (read its comments), and the header comment of `monolith_scene.c` is the map format reference.

**What it owns**, per loaded scene (everything is freed by `MS_Unload`):
- the parsed package: boxes (`scene->boxes`, `boxRefs[]` with each box's source line, statement index, group and material), model files and instances (`modelFiles[]`, `models[]`: transform, world bounds, collide mode, slot overrides), colliders (boxes first, then model colliders), materials (`[0]` = `"default"`), groups, lights, the sun, spawn, viewpoints, bounds, the map text (`MS_GetLine` / `MS_LineText`), path, package root, `MAP.md` path (`brief`), FNV-1a hash, mtime and a **`version`** that goes up on every successful (re)load (static caches key on it);
- the GPU side: one surface shader instance per material (uniforms are global per shader, §7.4), textures (`wrap repeat` by default, trilinear + mipmaps), one instance buffer per material with boxes (a unit cube scaled per instance), one instanced draw per (model file, slot material set, shadow flag), the visible sky and the ambient map (from the ambient-only sky, §7.5);
- the sun's shadow: static world-fixed tiles (`MS_SHADOW_TILES`, §7.3) or R3D's camera-fitted map (`MS_SHADOW_CAMERA` / `MS_SHADOW_ALWAYS`); the extra lights' shadow maps; the `--fog-cache` bake and the `--ao-volume` occlusion volume, all placed by the map's bounds ("Caches follow the map", §7.3);
- picking (`MS_Raycast`, `MS_RaycastModel`: models through a BVH over a CPU copy of their triangles), collision (`MS_Collides`, strict) and hot reload (`MS_Poll`).

**API** (`monolith_scene.h`):

| Group | Functions |
|---|---|
| Loading | `MS_Load(path, &MS_LoadOptions{ noGpu, shadowSizeOverride, shadowRangeOverride }, err, size)`, `MS_Unload`, `MS_Poll(scene, dt)` → `MS_UNCHANGED` / `MS_RELOADED` / `MS_RELOAD_FAILED`, `MS_Reload`, `MS_Error`, `MS_SetInitHints` (shadow map size: before `R3D_Init`), `MS_InitRenderer` (after `R3D_Init`), `MS_GetLine`, `MS_LineText` |
| Environment and drawing | `MS_ApplyEnvironment`, `MS_GetRenderSettings` / `MS_SetRenderSettings` (`MS_RenderSettings`: sun on, shadow mode, fog cache, pre-pass, render scale, upscale, AO volume, shadow overrides), `MS_QualityPreset` / `MS_QualityName` / `MS_QualityFromName` / `MS_QualityMatch` / `MS_QualityEqual`, `MS_ApplyQuality`, `MS_ApplyRenderResolution`, `MS_Draw` (a whole frame), `MS_PushLights` + `MS_DrawGeometry` (inside your own `R3D_Begin` … `R3D_End`), `MS_WriteEnvironment` |
| Queries | `MS_Raycast`, `MS_RaycastModel`, `MS_Collides`, `MS_ObjectRef`, `MS_ObjectBounds` |
| Package files | `MS_DependencyCount` / `MS_Dependency` (the watched files: map, shaders, textures, panoramas, models), `MS_WriteR3DEnvironment` (any `R3D_Environment` as `env` lines; the capture shim) |
| Memory helpers | `MS_ArrayReserve`, `MS_RESERVE`, `MS_PUSH`, `MS_StrDup`, `MS_StrNDup` (also used by `src/`, §4 "Memory rules") |

**Call order** in a program: `InitWindow`; optionally a `noGpu` peek load + `MS_SetInitHints` + `MS_Unload` (the shadow map size is an R3D init hint; with a quality preset pass `q.shadowSize` as `shadowSizeOverride`); `R3D_Init`; `MS_InitRenderer`; `MS_Load`; `MS_ApplyEnvironment`, then `MS_ApplyQuality` (if presets are used) and `MS_ApplyRenderResolution`; per frame `MS_Poll` (re-apply the environment and quality on `MS_RELOADED`) and `MS_Draw`; `MS_Unload` before `R3D_Close`. `examples/01_minimal/main.c` is the complete version.

**Loading and hot reload.**
- `MS_Load` / `MS_Reload` parse into a new scene (and upload it) and swap it in **only if the whole package is valid**, so rendering and collision never see a half-loaded map. Errors are `file:line: message`; the loader never crashes on bad input. Every referenced file must exist at parse time, also for `noGpu` loads, so `--dump-boxes` is a full package check (model files are only checked for existence there: their bounds and colliders are final only in GPU loads).
- `MS_Poll` checks the mtimes of every dependency about twice a second. After an mtime change it keeps re-reading for 2 s (mtime has 1 s resolution and editors may write in steps) and reloads when the combined content hash differs from the last attempt. The watch list is the files of the last *attempt*, so a file that is still missing after a failed reload is picked up when it appears. A reload creates every new GPU resource (instance buffers, shaders, textures, models, sky) **before** the old ones are freed (§7.4, instance buffer names), and the shadow tiles, the fog and AO bakes and the editor's frame-reuse key all re-key on `scene->version`. `shadow.size` only changes with a restart.
- `main()` parses the map (`noGpu`) **before opening a window**, so a broken map on the command line just prints the error and exits 1.

**Patched vs. upstream R3D (`MS_PATCHED_R3D`).** monolith's R3D copy has local patches (`submodules/r3d/patches.md`). The runtime compiles against both: with `-DMS_PATCHED_R3D` (set by `build.sh` and `examples/build.sh`, which build against `submodules/r3d`) it uses the patched APIs (`R3D_PushLightFixedShadow` for the static shadow tiles, `R3D_SetDepthPrepass`, the fog bake, the occlusion volume, the GPU profiler); without it, it falls back to `R3D_PushLightEx` with the camera-fitted map re-rendered when needed (`MS_SHADOW_TILES` becomes `MS_SHADOW_CAMERA`), and the fog cache and AO volume stay off. The export README lists the image differences that implies (patch 4 visible when turning, patches 7/9 at rounding level, patch 1 needed for volumetric fog on AMD / Intel). The upstream build is compile-checked only.

The default map is described in `maps/monolith/MAP.md` (style, reference sizes, groups and landmarks, viewpoints, the concrete material and the reasons behind its sun, sky and fog values).

### 6.2 Surface shaders in map packages

A material's `shader "shaders/x.glsl"` is an R3D **surface shader** (`#pragma usage opaque shadow`, `void fragment()`), loaded once per material from the package's `shaders/` folder and hot-reloaded like the map; `uniform` / `sampler` keys set its uniforms. Fragment built-ins are listed in `submodules/r3d/docs/shaders/surface_shader.md` (`ALBEDO`, `ROUGHNESS`, `METALNESS`, `SPECULAR`, `EMISSION`, `NORMAL_MAP`, `POSITION`, `NORMAL`, …). Boxes have per-face 0–1 UVs, so a shader that should look the same on boxes of every size derives its pattern from the world-space `POSITION` and `NORMAL` instead (the default map's `concrete.glsl` does, see its `MAP.md`). Opaque shaders that never discard should come with `alpha_cutoff 0` on the material (depth pre-pass). Check every shader with `glslangValidator` (§7.4: NVIDIA accepts GLSL that AMD / Intel reject).

### 6.3 Export, import, examples

- **Export** (`src/export.c`, `--export <dir>`, **F9**): copies the files the runtime lists (`MS_DependencyCount` / `MS_Dependency`: map, shaders, textures, panoramas, models) plus `MAP.md`, `ASSETS.md`, each model's `<file>.asset.md` and the files a model file refers to (`.gltf` URIs, `.obj` → `.mtl` → maps). The map file is copied byte for byte (the loader only accepts package-relative paths, so nothing needs rewriting) and renamed to `<name>.map`. The README is generated: build lines (plain compiler, CMake), minimal code, the "inside your own R3D frame" variant, hot reload, `MS_PATCHED_R3D` vs. upstream R3D (static tiles vs. camera-fitted shadow, no pre-pass toggle, no fog cache; image differences: patch 4 visible when turning, patch 7/9 rounding level, patch 1 breaks volumetric fog on AMD/Intel upstream), depth clamp, and the `ASSETS.md` table. The runtime sources come from the repo, or from `runtime/` next to the executable (`build.sh` copies `runtime/` and the viewer there). Tested: an export built outside the repo against `submodules/r3d`'s headers and libraries renders pixel-identical to `monolith --shot` (default map, views 1/2/6; the courtyard sample, views 1-3); against `~/Projects/r3d/include` (upstream) it compiles without `MS_PATCHED_R3D`.
- **Import** (`src/import.c`, `--import`): `NewMap()`, then a bundle written like the launcher's, then `LauncherStartBundle()` (the launcher's `Launch()` with its terminal / flags / workspace handling; the model from `LauncherChooseModel` with `kind = "import"`). The procedure for the session is `AI-MAP-TOOL.md` §10 ("Import requests").
- **Capture shim** (`runtime/monolith_capture.h`): a header compiled into the user's program (`-include`, no source change) that routes `R3D_LoadModel`, `R3D_GenMeshCube`, `R3D_GenProceduralSky`, `R3D_LoadCubemap`, `R3D_LoadSurfaceShader`, `R3D_Begin*`, `R3D_PushLight*`, `R3D_DrawMesh*`, `R3D_DrawModel*` and `R3D_End` through recorders. With `MONOLITH_CAPTURE=<dir>` it writes a package from one frame (boxes from axis-aligned cubes, models with their transforms, plain materials + read-back textures, lights, sun, every `env` key via `MS_WriteR3DEnvironment`, the procedural sky, the camera) and `CAPTURE.md` with what it couldn't translate. Limits: meshes other than generated cubes and instanced draws can't be read back (their data lives on the GPU), rotated cubes become AABBs, shader uniforms and material changes on loaded models are only reported. A cross-check for imports, not a replacement for reading the source.
- **Examples** (`examples/`, `examples/build.sh [--win]` → `examples/build/<linux|win>/<example>/`, gitignored): `01_minimal` (load + orbit; it is also the export's `viewer.c`), `02_existing_project` (`before.c` / `after.c`), `03_live_editing` (`MS_Poll`, `--reload` test), `04_import_source` (the import test input, also built with the capture shim as `import_source_capture`). Same flags and static linking as the game, against `submodules/r3d` with `-DMS_PATCHED_R3D`; each takes `--shot <file>` (hidden window titled `monolith-shot`, 30 frames into a `RenderTexture`, `examples/common/shot.h`).

## 7. R3D pitfalls already discovered (read before touching lighting)

These were found by debugging. They are engine facts and hold for every map; the numbers quoted are usually the default map's (its look and the reasons for it are in `maps/monolith/MAP.md`).

### 7.1 Volumetric fog turns into a black curtain if dense
R3D's volumetric fog is **lit only by lights, not by the sky/IBL**. Fog in shadow therefore absorbs light without adding any, and with high density, buildings 30–60 m away go **pitch black depending on view direction**.

The fix is in the map's `env` lines, so every map needs it: low `volumetricFog.scatteringDensity` (default map 0.012, pine forest 0.012), low absorption (0.001), and a small **`volumetricFog.emissionEnergy`** (default map 0.12, pine forest 0.25, coloured like the distance fog) standing in for sky light. Raise the shafts with `sun.fogEnergy` rather than with density.

### 7.2 Directional shadow map is camera-fitted; casters get clipped
R3D fits the sun's ortho shadow map to a bounding sphere of the camera frustum out to `light.range`. Its near plane reaches only about `radius + 100 m` toward the sun, so tall casters (the top of a 260 m megastructure sits about 330 m up the light ray) were **clipped depending on where the camera looked, and their shadows popped**.

The fix is **shadow pancaking**: `glEnable(GL_DEPTH_CLAMP)` once after `R3D_Init`, done by the runtime helper `MS_InitRenderer()` (every program that draws a scene calls it: the game, the examples, exported viewers). R3D never touches that state, so it applies to R3D's shadow pass. The side effect is that geometry closer than the 1 cm camera near plane (raylib's `RL_CULL_DISTANCE_NEAR` 0.01) is clamped instead of clipped.

**Do not "fix" shadow problems by disabling shadow casting.** That hides the symptom; fix the cause.

### 7.3 Sun shadow: resolution and cost
- **Texel size:** about `2 · radius / size`, with `radius ≈ 1.5 · range` (70° fov, 16:9). At 1920×1006, range 100 m: radius ≈ 159 m.
- **Fade:** R3D fades directional shadows out between 75% and 100% of `range`, by **distance from the eye** (R3D patch 4; upstream uses view depth, which changes when the camera only turns, see §7.6), and over the outer 1% of the map (x, y and depth; upstream 5%, changed by R3D patch 7, no visible difference).
- **Range lower bound:** `range` must stay at or above `volumetricFog.length` (90 m) or the light shafts fade early.
- **Default setting** (built-in, both shipped maps; `env shadow.size`, `sun.range`, or `--shadow`): 8192 px / 100 m, about 3.7 cm texels. VRAM is 16-bit depth × 2 layers, about 256 MB.
- **Root limitation:** there are no cascaded shadow maps in R3D for directional lights. That is the real long-term fix (a library change).

**Where the shadow time goes** (8192/100, `--bench` averages of 3 runs, measured 2026-10-08 with temporary switches; reports in `benchmark/2026-10-08_1157*`–`1206*`):

| Part | Cost | Measured as |
|---|---:|---|
| Rendering the shadow map (every frame) | ~1.2 ms | every-frame vs. map frozen after the first frame |
| Sun's volumetric-fog march, mostly its shadow lookups | ~2.6 ms | default vs. `sun.fogEnergy = 0` |
| Everything shadow-related | ~3.5 ms | default vs. sun pushed without a shadow map |
| Size penalty 8192 vs 4096 | 1.7 ms (fog on), 1.0 ms (fog off) | so ~0.7 ms is the fog's lookups missing the texture cache on the bigger map |

The fog is the big one: `vfog_radiance.frag` runs at half resolution, up to `length / stepSize` = 120 steps per pixel, and **each step does the full 8-tap PCF** (`L_SampleShadowDir`), i.e. up to ~460 M shadow taps per frame. The lighting pass does only 8 taps per pixel (~0.2 ms).

**Fog cost vs. camera height (fixed 2026-10-10, R3D patch 2).** Frame time used to grow with camera height (Canyon 5.3 ms vs. Tower/Overview ~8–9 ms). A height sweep showed that only volumetric fog scaled with height; with fog off, high views were *faster*. Cause: the fog shadow is faded by the **pixel's** depth (view depth then; distance from the eye since patch 4), so on sky pixels and anything beyond `sun.range` it is weighted by exactly 0, yet those pixels march the full fog length doing 8 taps per step. `vfog_radiance.frag` now skips the taps there. The output is bit-identical. Headless `--bench` average 7.35 → 5.87 ms (Tower 8.84 → 4.99 ms), and views with sky in frame are no longer the slowest. Steep downward views from height (e.g. `--pos -52.22 61.62 39.53 --look 140.2 -39.6`) stay the most expensive: every ray hits ground 80–100 m away and marches the full fog length with shadow lookups that are actually used. **R3D patch 3** makes those lookups cheaper without changing the image. A 512² min/max depth map of the shadow map (16×16-texel tiles, rebuilt only when the shadow map is re-rendered, ~0.35 ms) lets the fog skip the 8 PCF taps wherever they must all agree (fully lit or fully shadowed). That view goes 6.8 → 5.7 ms with a still camera. Trade-off: views with almost no shadowed fog (eye y 100) get +0.1–0.16 ms while the camera spins, from the extra reduction on each re-render. Cheaper still, but it changes the image: one tap per step instead of 8 (visible grain in the hall shafts), or a larger `stepSize`. The numbers in the table above predate this patch. Side effect of that fade, kept as upstream has it (per ray): fog in front of sky, or in front of geometry more than `range` **from the eye**, is never shadowed. Since **R3D patch 4** the fade and patch 2's skip are keyed on the distance from the eye to the surface behind the march (`fragDist`), not its view depth, so the result no longer changes when the camera only turns. Patch 3 is unaffected.

**Static sun shadow tiles (default since 2026-10-10, R3D patch 7).** Sun and boxes are static, so the shadow depth is a pure function of the map. Instead of R3D's camera-fitted map, the app supplies the light matrix (`R3D_PushLightFixedShadow`): `SunShadowTileSelect()` (`runtime/monolith_scene.c`) lays world-fixed orthographic tiles on a grid in light space, each with exactly the radius, depth window and texel lattice of R3D's fit at 70°/16:9 (3.69 cm texels at 8192/100), so pancaking, biases and PCF footprint are unchanged. The tile is chosen **by eye position only**, so turning never re-renders. A tile is used only while everything within `range` of the eye lies inside its fade-free area, so fades are exactly 1 wherever shadows show. The two tiles live in R3D's two existing directional shadow layers (VRAM unchanged); with the default map's tile home (`env shadow.home 10 38 2`, below), all 7 viewpoints and the verification shots fall into those two cells, so in the play area each tile is drawn **once per map load**. Far out, a new tile is drawn every ~90 m of travel. Image vs. the camera-fitted map: 326 pixels > 2/255 over 26 shots (16-bit depth rounding, max 15/255). The gain is mostly CPU: the per-frame coverage check below (~0.4 ms) goes away, plus the re-renders while moving (~1.2 ms + 0.35 ms min/max each). **If R3D's fit (`light_dir_view_proj`), its zExtension or `SHADOW_DIR_EDGE_FADE` change, update the `TILE_*` / `SHADOW_EDGE_FADE` constants in `runtime/monolith_scene.c`.** The camera-fitted reuse path described next is kept as `--shadow-camera`.

**Caches follow the map** (`runtime/monolith_scene.c`, region "Caches follow the map"). The static shadow tiles, the `--fog-cache` region and the `--ao-volume` region are placed from the loaded map, not from constants:
- a box is *ground-like* when its smaller horizontal extent is ≥ 100 m and its height ≤ 5% of that; the *content bounds* are all other boxes plus every model's world bounds (everything, if only ground-like boxes exist);
- the **shadow tile home** is the centre of the AABB of the viewpoint eyes (a map without viewpoints gets one from its spawn), else of the content bounds; `env shadow.home x y z` overrides it;
- the **fog cache region** is the content bounds + 2 m, shrunk around the play centre (keeping its proportions, staying inside the bounds) until it has at most 64 M voxels at the requested voxel size; `env fogCache.region` overrides it. The **AO volume** uses the same derivation with a 32 M-voxel budget (boxes only: models are not in it). Both are also cut to `GL_MAX_3D_TEXTURE_SIZE` voxels per axis around the play centre, so on very long maps they cover only the part around it (logged);
- derivation does not reproduce the layout the default map had before map packages, so `maps/monolith/monolith.map` carries both hints (`env shadow.home 10 38 2`, `env fogCache.region -70 -1 -150 75 100 105`) and stays pixel-identical. New maps should leave the hints out.

**Camera-fitted shadow-map reuse (`--shadow-camera`; the default before the tiles).** The scene is static, so the map is re-rendered only when needed: `R3D_PushLightEx(sun, sunShadow, update)`. R3D stores the light matrix with the cached map, so a reused map is always sampled consistently. And since R3D's fit radius depends only on range/fov/aspect and the centre is snapped to texels, every fit lays the **same world-space texel grid**, so the reused map has the same content where it covers. `SunShadowNeedsUpdate()` replicates R3D's fit (`light_dir_view_proj`) on the CPU and samples the view frustum out to `range` (48 depth slices × 33×17); it keeps the map only while R3D's fades (edge × distance, the distance fade by distance from the eye as in patch 4) from the cached map are within 1/255 of a fresh fit's at every sample. The check costs ~0.1–0.4 ms CPU (0.43 ms measured in the bench spin, 2026-10-10). **If R3D's fit or fade code changes, update `SunShadowFit()` / `ShadowEdgeFade()` / the `distFade` in `SunShadowNeedsUpdate()` (`runtime/monolith_scene.c`) to match.**
- Re-renders: walking forward ~1 per 65 m, sideways ~1 per 2.5–4.5 m, a still camera never; the bench spin (0.6°/frame) ~21% of frames before R3D patch 4, ~1 in 4200 after it (the distance fade no longer depends on the view direction).
- R3D's fit is very generous (the 159 m sphere sits 50 m ahead of the camera, so it covers everything within ~109 m in *any* direction). Even a stale map is almost always right; the check is deliberately strict anyway.
- Verified: static yaw sweeps are pixel-identical to the old build; after 90 frames of turning/walking/strafing/flying, reuse matches every-frame rendering (≤ 0.005% of pixels differ by > 2/255: 16-bit depth-quantisation noise that every-frame rendering has too), while a frozen map differs on up to 21% of pixels (180 m flight).

Measured with `--bench` (RTX 4050 Laptop, 1920×1006, vsync off, averaged over runs):

| Shadow setting | Texel | Frame time | FPS |
|---|---|---|---|
| 4096 / 140 m (old) | ~10.7 cm | 6.00 ms | 167 |
| 4096 / 100 m, re-rendered every frame | ~7.4 cm | 5.85 ms* | 171 |
| 6144 / 100 m, re-rendered every frame | ~4.9 cm | 7.27 ms | 138 |
| 8192 / 100 m, re-rendered every frame (`--shadow-always`) | ~3.7 cm | 7.55 ms* | 132 |
| **8192 / 100 m, reused (current)** | ~3.7 cm | **6.62 ms*** | **151** |

\* 2026-10-08 runs (`benchmark/2026-10-08_1224*`–`1226*` for the last two). The older rows were measured on an earlier day; the GPU ran ~5% faster today, so compare starred rows with each other. The bench spins the camera non-stop, which is the worst case for reuse; with a still camera the frame cost is ~6.3 ms (the shadow-render share disappears).

The cost tracks map size, not range. 6144/100 is the recommended fallback for weaker GPUs.

**Not applied: larger fog `stepSize`** (quality trade-off, decide by eye). Screening (`--bench-frames 150`): 0.75 → 1.0 saves ~0.38 ms, → 1.25 ~0.69 ms. Stills look the same, but 2–5% of hall pixels change by up to ~25/255 (the march noise pattern), and thin shafts may alias differently in motion.

**R3D-side proposals** (library changes, not made here):
0. *Done locally since:* patch 9 hoists the per-step constants of the directional march out of the loop and uses a closed form for rays with no shadow weighting (fog march 0.84 → 0.43 ms, rounding-level differences: 8 pixels > 2/255 over 26 shots, default on); patch 8 optionally replaces the march's shadow lookups with a baked 3D sun-visibility texture (`--fog-cache`, a look trade-off, off by default).
1. *Fog shadow lookups* (`shaders/deferred/vfog_radiance.frag`, the `LIGHT_DIR` case in the march loop): use one hardware-PCF tap per step instead of `L_SampleShadowDir`'s 8-tap Vogel disk; the per-pixel jitter plus the bilateral upsample already blur it. Up to ~8× fewer taps, i.e. most of the ~2 ms. (The `distFade == 0` part, sky pixels and surfaces ≥ range from the eye, is done: patch 2, keyed on distance by patch 4, in `submodules/r3d/patches.md`.)
2. *Cascaded shadow maps* for directional lights (`light_dir_view_proj` / `light_dir_push` in `src/modules/r3d_light.c`, `L_SampleShadowDir` in `shaders/include/wrap/light.glsl`): e.g. 2–3 × 2048–4096 cascades give finer texels near the camera than today's single 8192 map at a fraction of the fill and cache cost.
3. *Rotation-stable fit:* centre the fit on the camera position (radius = distance to the far corners) instead of 50 m ahead, so turning on the spot never invalidates a cached map. App-side reuse would then only re-render on movement.

**Caching results (2026-10-10).** Default map, headless `--bench`, 1920×1006, interleaved runs, 3 per row unless noted, all in one session (reports `benchmark/2026-10-10_1742*`–`1748*`). "Original" = the build before the map designer work (patches 1–3); "p3base" = Release R3D + profiler + patch 4, before caching.

| Build / flags | Camera | ms / frame | FPS | mean 1% low | GPU | CPU |
|---|---|---:|---:|---:|---:|---:|
| original | spinning | 4.60 | 217 | 144 | – | – |
| p3base | spinning | 4.54 | 220 | 175 | 3.72 | 0.77 |
| **final default** | spinning | **3.35** | **299** | 211 | 2.94 | 0.36 |
| final `--fog-cache 0.5 2` (trade-off) | spinning | 3.15 | 317 | 249 | 2.75 | 0.36 |
| final `--ao off --bloom off` (trade-off, 1 run) | spinning | 2.25 | 444 | 348 | 1.96 | 0.25 |
| p3base | still | 5.03 | 199 | 163 | 4.20 | 0.77 |
| **final default** | still | **3.70** | **271** | 216 | 3.29 | 0.36 |
| final, reused frame (`--frame-reuse`, 1 run) | still | **0.17** | ~5900 | – | – | – |

Where the default's gain comes from: depth pre-pass + streak-fbm skip (geometry pass 0.98 → 0.55 ms spinning, 1.30 → 0.66 ms still), patch 9 (fog march 0.77 → 0.42 ms spinning), static shadow tiles (CPU 0.77 → 0.36 ms: no per-frame coverage check). In the interactive game a still camera costs only the reused frame plus HUD, and the loop sleeps to the refresh rate.

**Pass trimming and the window (2026-10-11).** Default map, headless `--bench`, 1920×1006, 4 interleaved full pairs per row (report notes "P final A/B (round 2)"); per-pass GPU ms are averages over all views.

| | main 4455608 spinning | now spinning | main still | now still |
|---|---:|---:|---:|---:|
| frame | 3.73 | **3.45** (−7.6%) | 4.45 | **4.09** |
| GPU total | 3.19 | 3.03 | 3.78 | 3.58 |
| CPU until finish | 0.45 | 0.33 | 0.54 | 0.38 |
| depth pyramid (+ normal/radiance downsample) | 0.164 | 0.063 | 0.183 | 0.063 |
| bloom | 0.190 | 0.145 | 0.210 | 0.146 |
| FXAA + final blit | 0.175 | 0.106 | 0.231 | 0.118 |

All of it is R3D patch 10 (pyramid only as deep as something reads it, bloom composite folded into tonemapping, FXAA writing the final target): bit-identical, 0 pixels differ in the 27 shots and the models test map. Still pairs: three of four within −0.16..+0.02 ms, one disturbed baseline run; the honest still gain is ~−0.15 ms. The other passes are unchanged code; their per-pass split moves a little between runs (GPU timer attribution), the totals are what count.

*The visible window* (`--bench-window`, an offscreen stand-in with the HUD and overlay drawn and resolved like the swap, 3 interleaved triples): plain headless 3.50 ms, single-sampled window 3.78, 4× MSAA window (as before) 3.94. So the window's own cost is ~0.45 ms in serial headless terms: 4× MSAA ~0.15 ms GPU (R3D's last pass writing 4 samples + the resolve; now off by default, `--window-msaa`), HUD text + overlay + designer pick ~0.15 ms CPU (raylib `DrawText`, ~1500 glyph quads with the shadows; it overlaps the GPU in the real window) and the resolve/copy. That doesn't explain ~200 fps visible vs. ~300 headless. What does is outside the game: this desktop runs **picom** (glx backend, `vsync = true`, `use-damage = false` = full-screen repaint on every damage, dual-kawase blur behind the 80%-opaque bar), so every game frame is also copied and the screen recomposited on the same GPU. A fullscreen window is unredirected (`unredir-if-possible = true`), i.e. no compositor cost (but then the driver's `SyncToVBlank` caps at 144 Hz unless `__GL_SYNC_TO_VBLANK=0`). Not measured (no visible windows in automated runs).

**Trade-off knobs** (they change the image; off by default; for quality presets). Measured on the default map. Gain = headless frame ms vs. the default (3 interleaved full runs each, default 3.59 ms); images: before/after sheets of views 1/2/3/5 + steep.

| Knob | What it does | Frame | Visual cost (5 views) |
|---|---|---:|---|
| `--render-scale 0.75` (`MS_RenderSettings.renderScale`, `upscale`) | R3D at 75% of the output size, bicubic upscale | 2.19 (−39%) | softer edges and concrete grain; 3.8% of pixels > 2/255, mean ±0 |
| `--render-scale 0.5` | 50%, bicubic | 1.62 (−55%) | visibly soft, aliasing on thin slits; 6.3% |
| `--ao baked` (`aoVolumeVoxel` 0.5, `aoVolumeIntensity` 0.6; R3D patch 11) | SSAO + SSIL off, baked world-space distance-field AO instead (32 MB, ~60 ms bake per map load) | 2.62 (−27%) | crisper contact darkening at wall feet and canyon corners, also from off-screen geometry, never changes when turning; faint SSIL bounce gone; 11.7%, mean −0.7/255 |
| `--ao off` | SSAO + SSIL off | ~2.6 (screening run) | soft wall-foot darkening gone; 3.5%, mean +0.4 |
| `--fog-cache 0.5 2 --fx vfog.step=1.5` | baked fog sun visibility + double march step | 3.02 (−16%) | shaft edges ~0.5 m softer; 0.2% |
| `--bloom off` | no bloom, exposure ×1.19 | 3.35 (−6%) | faint halo at bright openings gone; 6.4% (±1–3/255), mean −0.1 |

They stack (render scale scales everything else). Not done, ideas with numbers: the default map's concrete shader costs ~0.3 ms of the 0.58 ms geometry pass (measured with a plain material), a distance LOD for its grain/board noise would change the image; SSIL's three half-res denoise passes and SSAO's one compute the same geometric weights (fusable, ~0.05 ms); a quarter-resolution fog march (R3D change) would save ~0.3 ms on top of the fog cache.


### 7.4 Other gotchas
- **`CheckCollisionBoxes` is inclusive:** touching counts as a hit, so a player standing on the ground could never move. Use the project's strict `BoxesOverlap()`.
- **Brightness:** with AgX, sunlit mid-grey albedos blow out to white quickly (the default map: sun 3.5 with exposure 1.1 made the buildings look white; it now uses sun 2.0, exposure 0.9). If a map looks washed out, lower `sun.energy` or `tonemap.exposure`; don't brighten the fog colour. The default map's sun angle and fog density are mood choices, explained in `maps/monolith/MAP.md`.
- **Instance attributes:** box instance buffers hold only `POSITION | SCALE`, with no rotation: boxes are axis-aligned, and collision, picking and world-position shaders rely on that. Model instances carry `POSITION | ROTATION | SCALE`; R3D rotates their normals but doesn't inverse-scale them, so per-axis scaled models light approximately. R3D doesn't frustum-cull instanced draws (boxes or models).
- **Surface-shader uniforms:** they are uploaded at `R3D_End()`, so the last value set in a frame wins. They can't vary per draw within a frame.
- **Hidden-window screenshots:** in `--shot` mode the window is never mapped, so reading the default framebuffer (`TakeScreenshot`) would be undefined: no pixels are owned. Shots are rendered with `R3D_BeginPro` into a `RenderTexture` and read back with `LoadImageFromTexture`, flipped vertically. Don't switch back to `TakeScreenshot`.
- **NVIDIA accepts GLSL that AMD/Intel reject.** Wine uses the host driver, so on this machine neither Linux nor wine shows these errors. Two found so far (shaders failed to compile on a friend's Windows PC with an AMD RX 9060 XT): a function named `noise3` in `concrete.glsl` (clashes with the deprecated built-in `noise3()`, which returns `vec3`; now `valueNoise`), and a uniform-block member named `length` in R3D (`uVFog.length`, patched; see `submodules/r3d/patches.md`). Avoid GLSL built-in names (`noise1`–`noise4`, `length`, `distance`, …) for functions and members, and check shader changes with **`glslangValidator`** (installed), which rejects the same things. For a surface shader (e.g. `maps/<name>/shaders/*.glsl`), wrap it in a stub that declares the built-ins it uses and calls `fragment()` from `main()`.
- **Instance buffers on reload:** R3D caches the GL buffer names it last bound for instancing (`instances_enable` in `src/modules/r3d_render.c`) and skips re-binding when they match. Freeing the old instance buffer before creating the new one let GL hand out the same names, so a hot-reloaded map was drawn with the **old** boxes. The runtime therefore creates every new buffer (boxes and models) first and frees the old ones after. The proper fix would be in R3D (`R3D_UnloadInstanceBuffer` resetting that cache); not patched.
- **Ambient colour vs. ambient map:** setting `ambient.map = {0}` and using a plain `ambient.color` produced no ambient light in testing. Keep the IBL ambient map generated from a procedural sky (§7.5).

### 7.5 Shadowed undersides: the ambient sky's ground is the fill light
Downward-facing surfaces (in the default map: waffle soffits, beam and bridge undersides, the hall ceiling under the cantilevered block) used to render **near pitch black**. Measured with `--off` on the default map (2026-10-10):
- **Cause: the IBL's lower hemisphere.** The ambient map was built from the visible sky, whose ground is `groundBottomColor` (48,46,44) ≈ 0.03 linear. R3D's default `groundHorizonCurve` 0.02 makes the *whole* lower hemisphere that colour, so a down-facing normal got almost no ambient. With the sun off (`--off 6`) soffits and the hall ceiling were exactly (0,0,0). The near-zero base also made the warm volumetric in-scatter on them come out **maroon**, e.g. (19,0,0), once AgX, contrast and the 0.8 saturation were applied.
- **Not the cause: SSAO and SSIL.** Each changes whole-frame mean brightness by < 0.5/255 in every test view, before and after the fix. SSIL only bounces light from lit pixels on screen, so it can never stand in for the ground when looking up. SSAO (1.2), SSIL (`giIntensity` 1.2) and `ambient.energy` (0.9) are unchanged.
- **Their cost vs. their effect (2026-10-10):** SSIL ≈ 0.5 ms and SSAO ≈ 0.2 ms GPU per frame (plus the normal/radiance downsamples; the depth pyramid itself stays, volumetric fog needs it). Net, SSIL *darkens* (its own 5 m AO term outweighs its bounce), so turning it off brightens slightly. With both off the largest change in the 26 verification shots is the soft darkening band along wall feet in close, shaded views (view 1: up to +37/255 in that band, +1.35/255 mean); views 2/3/5 and the steep view change < 0.5/255 mean. Presets: `--ao ssao|lite|off`, §5.

**Fix (in place):** the runtime builds the ambient map from a second, **ambient-only** procedural sky (`ambientSky.mode procedural`, `ambientSky.size 256`, unloaded right after `R3D_GenAmbientMap`): a copy of the visible sky with the `ambientSky.*` keys changed. The default map sets `ambientSky.groundBottomColor 120 116 108` ≈ 0.19 linear, slightly warm, and `ambientSky.groundHorizonCurve 0.1` (these are also the built-in defaults); the pine forest uses a brighter, bluer copy for blue-violet shadows. The visible background sky, the distance fog and the sun are untouched, and the frame cost is unchanged: no new pass or light, only init time. The upper hemisphere stays the cool sky, so shadowed faces get warm bounce from below and cool light from above.
- Fully sunlit 0.25-albedo ground would be ~0.12 linear (albedo · sun 2.0 · cos 39° / π); 0.19 was picked by eye. Result: shadowed undersides at ~10–25% of sunlit faces in linear terms (hall ceiling (0,0,0) → (47,43,37), facade soffit (38,26,6) → (64,56,45), sunlit pillar unchanged at (149,141,130)). Formwork joints, mottling and corner AO read in shadow; sun patches, shafts and shadow shapes are unchanged.
- To tune the fill, change `ambientSky.groundBottomColor` (brightness/warmth of under-lighting) and `ambientSky.groundHorizonCurve` (how much of the bright `groundHorizonColor` reaches near-vertical faces). Keep the visible sky's ground (`sky.groundBottomColor`) dark if high views show it below the horizon. `ambientSky.mode same` (the visible sky itself) is the right choice with panorama skies.
- **Don't add a fill light for this.** A shadowless directional "bounce" light isn't needed. It would also light the volumetric fog unless `fogEnergy = 0` (§7.1).
- Side effect, physically right: the ambient *reflection* (prefilter) now uses the brighter ground too, so wet (low-roughness) patches facing sideways pick up a bit of it.

### 7.6 Effects must not change when the camera only turns (R3D patch 4)
From a fixed eye, merely rotating the camera used to change how a given world spot looked, most visibly the fog. Examples: the hall's far opening glowed in the screen centre and dimmed at the edge, and the distance fog thinned toward the screen edges. **Cause:** four terms used the pixel's **view depth** (distance along the camera's forward axis, = distance · cos of the angle off-axis, only 0.57 · distance at a 16:9 corner) where they mean distance from the eye. The terms were the volumetric-fog shadow fade and patch 2's skip (`vfog_radiance.frag`), the fog transmittance (`vfog_transmittance.frag`), distance fog (`fog.frag`) and the sun-shadow fade on surfaces (`lighting.frag`). **Fix (patch 4):** all four now use the distance from the eye. `runtime/monolith_scene.c` replicates the new fade. Details and measurements are in `submodules/r3d/patches.md`.
- **Rule:** any fade, fog or cache term must be a function of the world position (and the view ray), never of view depth. Static caches (shadow, fog, AO) would otherwise bake a view-dependent error in.
- Still view-dependent, by nature or unused: the screen-space passes (SSAO, SSIL, bloom, FXAA, the fog's per-pixel jitter and bilateral upsample) stay at reprojection-noise level. `scene/forward.frag` and `scene/unlit.frag` still use view depth; neither shipped map uses them, but materials with `transparency alpha` or `unlit on` may go through them. The camera-fitted shadow map (`--shadow-camera`) still moves with the view, which affects re-render cost but not the image (§7.3, proposal 3); the default static tiles don't.

## 8. How to verify changes

There's no test suite; verification is **visual and numeric**:

1. **Build cleanly:** `./build.sh` and `./build.sh --win` with no warnings; `examples/build.sh` (and `--win`) too when the runtime or an example changed. Shaders pass `glslangValidator` (§7.4).
2. **No leaks:** `tools/leakcheck.sh` ends with `leakcheck: OK: zero leaks of ours` (§4). After larger changes also `tools/leakcheck.sh --asan` (memory errors, UB).
3. **The default map is unchanged** by engine work: identical `--dump-boxes` output and pixel-identical `--shot` images (the 7 viewpoints, yaw sweeps 0/60/…/300 at eye heights 1.6, 60 and 100, the steep view `--pos -52.22 61.62 39.53 --look 140.2 -39.6`, a `--reload-test`), compared against a build from before the change. Check `maps/pine_forest` and `tests/models` the same way when models, materials, lights or the environment code changed.
4. **Screenshots from fixed views:**
   ```bash
   ./build/linux/monolith --shot a.png --view 2                                   # the map's viewpoint 2
   ./build/linux/monolith --shot b.png --pos 0 0 -5 --look 120 5                  # exact view
   ./build/linux/monolith --shot c.png --view 2 --off 16                          # with effects off
   ./build/linux/monolith --shot d.png --view 3 --map maps/pine_forest            # another package
   ```
   For anything lighting- or shadow-related, take a **sweep of yaw angles from the same position** (e.g. 0/60/…/300) and compare. Both the black-curtain and shadow-popping bugs only showed up when rotating.
5. **Isolate effects** with `--off` (1 volumetric fog, 2 fog, 3 SSAO, 4 SSIL, 5 bloom, 6 sun) to find which one causes a problem.
6. **Performance:** `__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless [--shadow S R] --note "what changed"`. Each run writes a report into `benchmark/`. GPU clock states swing results by ~10%, so verify every claimed gain or regression with interleaved A/B pairs (≥ 3 pairs, two binaries, same flags) and compare averages. Screen ideas with `--bench-frames 150` (~10 s a run); use full runs for numbers you report. Only one benchmark at a time.
   **Automated / AI-driven runs: always `--bench --headless`** so the developer's screen is never taken over. Without `--headless`, `--bench` opens a **visible** window on purpose (it measures real presentation), and that window must actually be shown: on a workspace that isn't displayed (i3 unmaps it) the run stalls at 100% CPU with the GPU idle and never finishes. Use visible runs only when the developer asks for presentation-inclusive numbers; `--bench-window` approximates the window's own cost headless.
7. **Window titles and i3:** automated runs use their own titles (`monolith-bench`, `monolith-shot`, also the examples' `--shot`; normal play is `monolith`). The developer's i3 config (`~/.config/i3/config`) has
   `for_window [title="^monolith-(bench|shot)$"] move container to workspace number $ws10`,
   so automated runs open on workspace 10 and never take over the focused workspace. Keep these titles if you change window creation. Terminals launched for requests are titled `monolith-claude: <slug> <HHMMSS>` and moved to workspace 10 by the launcher (`MONOLITH_WORKSPACE`, §5).
8. **Movement:** collision and controls can't be checked from screenshots; test them by playing.
9. **Map loader and map edits:**
   - `--dump-boxes <file>` writes the exact box list (hex floats) without a window; `--dump-scene <file>` adds models, colliders, materials and lights (`--dump-scene-diff`: without indices and lines, for diffs across inserted lines). `diff` the dumps from before and after a loader change; they must be identical for the default map. Diff them after a map edit to confirm that only the intended objects changed.
   - A broken map given with `--map` (or the default one) makes the program print `file:line: message` and exit 1 before opening a window. In the running game a broken save keeps the old map instead.
   - Hot reload itself only runs in the interactive game, never in `--shot` / `--bench`; `--reload-test` exercises the same reload path inside a shot.
10. **Designer and launcher without a window:**
    - `--shot x.png --view 2 --pick` prints the centre hit. For the default map it must be box 46, a `hypostyle hall: pillars` box from map line 193 (material `concrete`), at distance ≈ 42.07 m.
    - `--shot-hud` (with `--select <line>` / `--region x y w h` / `--type "text"` / `--settings-panel`) puts the overlay, HUD, regions, prompt box or settings panel into the shot.
    - `--submit "text"` runs the real submit path. **Always** with `MONOLITH_TERMINAL=tools/stub-terminal.sh` (it only logs its arguments to `$MONOLITH_STUB_LOG`), so no real terminal or Claude session starts, and with `MONOLITH_WORKSPACE=` unless the workspace move is what you test. Move the bundle to the trash afterwards (`gio trash requests/<id>`, never `rm`). `--classify "text"` checks the model choice without a bundle.
    - Mouse and keyboard input (clicks, snip-mode drags, typing, paste, Esc, the settings panel) can only be tested by hand.
11. **Export and examples:** `--export <dir>` into a scratch folder, build its `viewer.c` outside the repo (the README's compiler line) and compare its `--shot` with `monolith --shot` from the same view (pixel-identical for the shipped packages); `examples/build/linux/<example>/… --shot <file>`.

## 9. Common modifications

**Edit a level.** Edit the package (`maps/<name>/<name>.map`, or another one loaded with `--map`), following its `MAP.md`: boxes, models, materials, lights and `env` lines in well-named groups. The format and the editing rules are in `AI-MAP-TOOL.md` §6–7. The running game hot-reloads the package, so there is no rebuild. Rendering, shadows, collision and picking pick the new objects up automatically. Boxes stay axis-aligned; only models rotate. Or just submit the change as a request from inside the game.

**Start a new level.** `./build/linux/monolith --new-map <name>` (no window), fill in `maps/<name>/MAP.md`, then build (or submit a first request on the empty map: the session writes the brief first). Or import an existing raylib + R3D project with `--import` (§6.3).

**Add a model.** `AI-MAP-TOOL.md` §12: a licence-safe download with `tools/fetch_asset.py`, `--inspect-model`, normalisation, `tools/record_asset.py`, a `model` line with material overrides from the brief.

**Add a map primitive or statement.** Add a builder next to `Box`/`Block`/`Stairs`/`WaffleFacade` in `runtime/monolith_scene.c`, plus a `PRIMS[]` entry: the name, the argument count, and per argument the axis it is shifted along (`AX_X/Y/Z`), `AX_NONE` for sizes, or `AX_INT` for counts. Then add its call where `ParseStatement()` dispatches. That code currently switches on the argument count (box and block, both 6, are told apart by the `PRIMS[]` entry), so a new primitive with 6–8 arguments means switching on the entry instead. Builders must return false when the map is full (`MS_MAX_OBJECTS`, the sanity cap) or out of memory, and loops must add a box per iteration so that cap bounds them. New statements that format-1 files don't know need `monolith 2` (bump `MS_FORMAT_VERSION` only for changes older runtimes would misread). Document it in the header comment of `runtime/monolith_scene.c` and in `AI-MAP-TOOL.md` §6.

**Add an environment key.** Add it to the key tables in `monolith_scene.c` (region "Key tables": `ENV_KEYS` for `R3D_Environment` fields, the sky / sun tables otherwise; a range in `ENV_RANGES`), with its built-in default in `BuiltinEnvironment()` equal to today's value so no map changes, and make sure `MS_WriteEnvironment` writes it. Anything that changes the image without a reload must also reach the game's `FrameKey`.

**Change what a request bundle contains.** Change `launcher.c` (`BuildBundle`, `WriteRequestMd`, `WriteRequestJson`; bump `"format"` in the JSON when keys change) and update the bundle description in `AI-MAP-TOOL.md` §2, keeping older formats described there. Add a terminal to `TERMINALS[]` in `launcher.c` (its title, working-directory and exec flags).

**Add a spectator viewpoint.** Fly there in spectator mode, press **P**, and add a line `viewpoint "Name" ex ey ez tx ty tz` to the map file. Note that P prints *feet* position (eye − 1.62) for `--pos`, while `viewpoint` takes the *eye*. `--bench` automatically covers new viewpoints.

**Tune the look of a map.** Everything is in the map's `env` lines (sky, ambient sky, sun incl. `fogEnergy`, `range`, `softness`, fog, effects, tonemap) and its materials; the reasons are in its `MAP.md`. Respect §7.1–7.5. To see what one effect contributes, toggle it live with keys 1–6. Changing the default map's look needs the developer's approval.

**Change a map's shader.** Edit `maps/<name>/shaders/<x>.glsl`; it hot-reloads with the package (no rebuild, no restart). Follow the map's brief (the default map's concrete stays dark and world-position based, see its `MAP.md`) and check it with `glslangValidator`.

**Add a graphics setting.** Add the field to `MS_Quality` (runtime: preset values, `MS_QualityEqual`, `MS_ApplyQuality`), the file key and panel row in `settings.c` / `settingspanel.c`, and keep ultra = the map exactly (pixel-identical).

**Add a new effect toggle.** Extend the `TOGGLE(KEY_x, 'x')` block in the main loop, the HUD string, and the header comment.

**Add a CLI flag.** Parse it in the loop at the top of `main()` and document it in the header comment and in §5 here.

## 10. Conventions

- **English** for all code, identifiers, comments and project names.
- **Match the existing style:** 4-space indent, `static` functions, PascalCase for functions, `UPPER_CASE` macros, `// #region` sections, and short comments explaining *why* rather than what.
- **One module per concern** (§6): each `src/<module>.c` has a `.h` with its interface and a header comment saying what it does. File-local functions are `static`. Shared state is minimal (`scene`, the data paths). Keep `build.sh` compiling `src/*.c` and `runtime/*.c`, so a new module needs no build change. (`src/glibc_compat.c` is build plumbing, not program code; it compiles to nothing outside Linux.)
- **The runtime stays portable.** `runtime/` depends only on raylib and R3D, compiles without `MS_PATCHED_R3D`, and knows nothing about the editor. Editor-only code (designer, launcher, bench, settings panel, frame reuse, player, spectator) stays in `src/`.
- **The level is data.** Geometry, materials, lights and the environment belong in `maps/<name>/`, not in C. C code describes primitives and statements, not specific buildings or a specific look.
- **Style-neutral engine and docs.** Nothing outside a map package assumes a style; a map's style, palette and rules live in its `MAP.md`.
- **Assets are recorded.** Every external file in a package has a row in its `ASSETS.md` and (models) a `<file>.asset.md` sidecar; CC0 / public domain, or CC-BY with the attribution recorded (`AI-MAP-TOOL.md` §12).
- **Memory:** content-sized data in heap arrays (`MS_RESERVE` / `MS_PUSH`), no allocation per frame, everything freed, zero leaks (§4 "Memory rules").
- **Comment non-obvious engine workarounds** in the code with their reason (as for §7.1 and §7.2), so nobody "cleans them up".
- **Don't commit `build/`, screenshots, `benchmark/`, `requests/` or `export/`.**

## 11. Known limitations and next steps

- **Map format:** boxes are axis-aligned (instance buffer: position + scale, §7.4); rotation exists only for models. Box UVs are per face (0–1), so textures stretch with the box size. Models are static (animations and skins are ignored). Instances are rebuilt on every reload. Code changes need a restart, and `shadow.size` only changes with a restart (or the F2 panel's renderer restart).
- **Models:** picking hits triangles, but collision uses AABBs (`collide aabb` or model-space boxes). A `noGpu` load (`--dump-boxes`, the start-up parse check) doesn't read model files, so model bounds and `origin bottom|center` shifts are only final in GPU loads. FBX / DAE files' own references are not followed for hot reload or export. The AO volume ignores models.
- **Interactive input is untested by automation.** Clicks, snip-mode drags, typing, paste, Esc handling, the settings panel and camera freezing can only be checked by hand. Automated checks cover picking, selection by line, regions, bundles, the launcher argv and closing (`--pick`, `--select`, `--region`, `--type`, `--shot-hud`, `--submit` and `--close-submitted` with a stub terminal, `--classify`).
- **Windows launcher is only compile-tested.** It uses `cmd /c start … cmd /k claude …` via `_spawnvp`. A failure of `start` itself can't be detected, the workspace move and window closing are i3 / sway only, and on Windows a closed request only leaves the HUD list. On Windows, and in any copied build folder, `requests/` and Claude Code's working directory are the executable's folder, which contains `maps/` and `runtime/` but no sources and no `CLAUDE.md` / `AI-MAP-TOOL.md`.
- **Request flow details:** the reproduction `--shot` passes `--shot-size` with the window size, so it matches `view.png`'s framing. If launching the terminal fails, the bundle stays and the prompt box keeps the text; pressing Enter again writes a second bundle (move the stale one to the trash). The terminal process exiting early (kitty `--single-instance`, wezterm mux) is reported but doesn't count as a failure; closing then only works through the i3 / sway mark.
- **Upstream R3D:** the runtime compiles against upstream R3D without `MS_PATCHED_R3D` (camera-fitted shadow, no fog cache, AO volume or pre-pass toggle), but that build is only compile-checked, not run.
- **Decals:** none yet. R3D supports them (`r3d_decal.h`); stains, streaks and graffiti would add a lot.
- **Outdoor haze:** high and outdoor views get hazy when sunlit volumetric fog fills the whole view. Height-dependent fog would help; R3D's volumetric fog is uniform.
- **Shadow cascades / cheaper fog shadow lookups:** see the R3D proposals in §7.3.
- **Vsync:** off by default; the settings file / F2 panel have vsync and an fps cap for the interactive game.
- **Platform:** Linux, plus a Windows x86_64 cross-build (`./build.sh --win`). Tested under wine (NVIDIA driver). The first run on a real Windows PC (AMD Radeon RX 9060 XT) exposed the strict-GLSL issues in §7.4; the fixes are verified with `glslangValidator` but not yet on that PC. There's no native Windows build script (MSVC / CMake for monolith itself) and no macOS support. The Windows build has no sanitizer mode.
