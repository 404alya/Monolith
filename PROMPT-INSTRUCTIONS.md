# monolith — Project Instructions

Engine and technical reference for anyone (human or AI assistant) changing this project. Read it before changing code. Instructions for carrying out map edit requests (the request workflow, the map format and editing rules) are in **`AI-MAP-TOOL.md`**. `CLAUDE.md` imports both files.

## 1. What this is

**monolith** is an **AI-assisted map designer** for brutalist concrete levels: a real-time, first-person 3D viewer and editor rendered with raylib + R3D in plain C. You can walk through a level or fly around it with a spectator camera. In spectator mode the developer points at a spot, selects boxes, types a request and submits it. The game writes a request bundle and opens **Claude Code in a new terminal**, which edits the map file. The game **hot-reloads** the map, so the change appears in place.

The focus is on how the levels look: light, shadow, fog, scale and composition.

Design principles:

- **No modelling pipeline.** All architecture is **axis-aligned boxes**, listed in a plain-text map file (`maps/*.map`, one primitive per line). The concrete look comes from a **procedural shader** with no textures.
- **Light and composition over detail.** Shadows, light shafts through volumetric fog, distance fog swallowing huge silhouettes, a human-scale viewer among enormous structures.
- **Plain C on a lightweight renderer.** raylib + R3D rather than a full engine.

Current state: the default map `maps/monolith.map` (§6.1), a map loader with hot reload, walk and spectator cameras, crosshair picking and selection, an in-game prompt box, request bundles with a terminal + Claude Code launcher and result polling, live effect toggles, offscreen screenshots and a benchmark mode.

## 2. Technology

| Item | Details |
|---|---|
| Language | C11 (`-std=c11 -Wall -Wextra`). The program is the modules in `src/*.c` (§6), plus a small Linux build shim, `src/glibc_compat.c`. |
| Windowing / input | raylib 5.5 (bundled with R3D, linked statically) |
| Renderer | **R3D** 0.10.x (Bigfoot71/r3d): deferred PBR on top of raylib. Provides shadows, SSAO, SSIL, SSGI, SSR, fog, volumetric fog, bloom, tonemapping, procedural sky, IBL, instancing, surface shaders. |
| Assimp | Linked because R3D depends on it; not used directly |
| OpenGL | 3.3 core (through R3D/raylib). `libGL` (`opengl32` on Windows) is linked only for `glEnable`/`glFinish`/`glGetString`, declared by hand in `main.c` and `bench.c` (see §7.2). |
| Platform | Linux (X11/GLFW), the main target. Windows x86_64 is cross-compiled with MinGW-w64 (`./build.sh --win`) and tested under wine. |
| Dev machine | CachyOS, NVIDIA RTX 4050 Laptop GPU, driver 615.71 |

**R3D location:** vendored as a plain copy (not a git submodule) in **`submodules/r3d`**, with its bundled raylib and assimp. `build.sh` builds it on first use as static libraries: `submodules/r3d/build-linux` for Linux and `submodules/r3d/build-mingw` for Windows. Headers are in `submodules/r3d/include/r3d/`, docs in `submodules/r3d/docs/shaders/*.md`, examples in `submodules/r3d/examples/`. The R3D source is the best reference for its behaviour; check it rather than guessing. Paths like `shaders/deferred/…` or `src/modules/…` elsewhere in this file are relative to `submodules/r3d`.

> **R3D changes are patches.** Work around R3D limitations from the app side where reasonable. When R3D itself has to change (e.g. a real bug), change `submodules/r3d` minimally and **record every change in `submodules/r3d/patches.md`** (symptom, cause, files, diff), so the copy can be refreshed from upstream later. Don't modify `~/Projects/r3d` (the original checkout) or other projects in `~/Projects` (e.g. `gameproject2`).

## 3. Layout

```
monolith/
├── build.sh                     # build script (see §4)
├── .clangd                      # include paths for clangd (absolute paths into submodules/r3d)
├── .gitignore                   # build/, screenshots, benchmark/, requests/, .clangd, unused R3D parts
├── cmake/
│   └── mingw-w64-x86_64.cmake   # toolchain for cross-building R3D for Windows (--win)
├── CLAUDE.md                    # read automatically by Claude Code: project summary, hard rules, imports the two below
├── AI-MAP-TOOL.md               # the AI side of the map designer: request workflow, map format, editing + verification
├── PROMPT-INSTRUCTIONS.md       # this file (engine / technical reference)
├── AI-MAP-DESIGNER-PROMPT.md    # the original task description for the map designer + caching work
├── maps/
│   └── monolith.map             # the default map (§6.1); format in src/map.c and AI-MAP-TOOL.md §5
├── requests/                    # request bundles written by the game (gitignored), see AI-MAP-TOOL.md §2
├── benchmark/                   # --bench reports: <timestamp>_shadow-<size>-<range>.md (gitignored, local)
├── src/                         # the program, one module per concern (§6)
│   ├── main.c                   # header comment (controls + flags), args, data paths, init, main loop, Submit
│   ├── common.h                 # shared includes, resourcesDir / benchmarkDir
│   ├── world.c/.h               # World (boxes + back-references + groups + version), MAX_BOXES
│   ├── map.c/.h                 # map format parser, LoadMap, hot reload (MapPoll), MapGetLine
│   ├── player.c/.h              # walking player, collision, BoxesOverlap
│   ├── spectator.c/.h           # spectator camera, VIEWPOINTS[], LookDir/LookAngles
│   ├── designer.c/.h            # crosshair picking, selection, overlay, HUD lines, prompt box
│   ├── launcher.c/.h            # request bundles, terminal + Claude Code launch, result.md polling
│   ├── atmosphere.c/.h          # SUN_DIR, SetupEnvironment()
│   ├── shadow.c/.h              # sun shadow: static world-fixed tiles; camera-fitted reuse (--shadow-camera)
│   ├── bench.c/.h               # --bench statistics and Markdown report
│   └── glibc_compat.c           # Linux only: binds a few symbols to old glibc versions (§4)
├── resources/
│   └── shaders/
│       └── concrete.glsl        # procedural board-formed concrete (R3D surface shader)
└── submodules/
    └── r3d/                     # vendored R3D (+ raylib, assimp); local fixes in patches.md
```

`InitDataPaths()` (`main.c`) sets the runtime `resourcesDir` / `benchmarkDir` and the maps directory (the code uses those, not the macros). On Linux, the repo directories baked in at compile time (`-DRESOURCES_PATH=".../resources/"`, `-DBENCHMARK_PATH`, `-DMAPS_PATH`) win while they exist. Otherwise, as on Windows, `resources/`, `benchmark/` and `maps/` next to the executable are used (`GetApplicationDirectory()`). So the binary runs from any working directory, and `build/linux/` and `build/win/` can be moved to other computers. `build.sh` copies `resources/` and `maps/` into both output folders. On the dev machine the game therefore loads and hot-reloads the **repo's** `maps/monolith.map`, not the copy in `build/`.

`launcher.c` decides on its own where `requests/` goes and which directory Claude Code is started in. On Linux it uses the repo root baked in as `-DPROJECT_PATH` while that exists, and otherwise the executable's directory. On Windows it always uses the executable's directory.

## 4. Build and run

```bash
./build.sh              # -> build/linux/monolith + build/linux/resources/  (clang, -O2)
./build.sh run [args]   # build, then run with args
./build.sh debug        # -g -O0 + AddressSanitizer/UBSan, same output folder
R3D_DIR=/other/r3d ./build.sh   # use a different R3D source tree (default submodules/r3d)
CC=gcc ./build.sh               # use another compiler

./build.sh --win                # -> build/win/monolith.exe + build/win/resources/ (MinGW-w64, -O2)
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
- Data paths: the repo's `resources/` and `benchmark/` (baked in with `-DRESOURCES_PATH`/`-DBENCHMARK_PATH`) are used while they exist, so on this machine shader edits apply without rebuilding and reports land in the repo. On any other computer the folders next to the executable are used (`resources/` is copied there by `build.sh`; `benchmark/` is created on the first `--bench`). Note that a portability test on this machine still uses the repo paths unless they're hidden, e.g. `unshare -rm sh -c 'mount -t tmpfs none ~/Projects/monolith && /path/to/copy/monolith --shot /tmp/x.png --view 2'`.
- x86-64 only (no ARM build).

**Windows (`--win`)** cross-compiles with `x86_64-w64-mingw32-gcc`:
- R3D is configured in `$R3D_DIR/build-mingw` with `cmake/mingw-w64-x86_64.cmake`: Release, static libraries, vendored raylib + assimp + zlib, no examples.
- The exe is linked **fully static** (`-static`: R3D, raylib, assimp, zlib, libstdc++, libgcc, winpthread), so `build/win/` holds just `monolith.exe` (~7 MB, stripped) and a copy of `resources/`, with no DLLs to ship. Its imports are only system DLLs: `opengl32`, `gdi32`, `user32`, `kernel32`, `shell32`, `winmm` and the UCRT `api-ms-win-crt-*` set (part of Windows 10+). Check with `x86_64-w64-mingw32-objdump -p build/win/monolith.exe | grep "DLL Name"`.
- It's a console-subsystem exe, so `--bench` tables and the **P** output are visible; started from Explorer it opens a console window next to the game.
- Bench reports from the Windows build go to `build/win/benchmark/`, not the repo's `benchmark/`.
- `--win debug` is refused: ASan/UBSan don't work with MinGW.
- Verify like Linux, under wine (it uses the host GL driver, so shots are pixel-identical to the Linux build): `wine build/win/monolith.exe --shot win.png --view 2`, or `__GL_SYNC_TO_VBLANK=0 wine build/win/monolith.exe --bench --headless --bench-frames 150`. The `MESA-EGL: warning` lines wine prints on NVIDIA are harmless.

A change is done only when it **builds with no warnings** on both targets (`./build.sh` and `./build.sh --win`) and has been **checked visually** (see §8).

Notes on the build type (unoptimised vs. Release R3D on Linux) and on how it affects comparability with older benchmark reports may follow here.


## 5. Controls and command line

The header comment at the top of `src/main.c` is the source of truth; keep it updated when adding controls or flags.

**Walk mode (default)**

| Key | Action |
|---|---|
| WASD + mouse | Walk and look |
| Shift | Walk faster |
| Space | Jump |
| F | Toggle fly (noclip) |
| 1–6 | Toggle volumetric fog / distance fog / SSAO / SSIL / bloom / sun |
| F1 | Toggle HUD (also hides the crosshair and selection overlay) |
| F12 | Save `screenshot.png` |
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
| Crosshair | The box under it gets a white outline and a cyan hit marker. The HUD shows its group, map line (with the line's text), box index and distance. |
| LMB | Select the box under the crosshair (replaces the selection) |
| Shift + LMB | Add / remove that box |
| Ctrl + LMB | Select the box's whole group (replaces the selection) |
| Ctrl + Shift + LMB | Add the whole group |
| RMB | Clear the selection |
| Enter | Open the prompt box |
| C | Snip mode (screen regions): camera frozen, cursor free. Drag = a region (replaces the others), Shift + drag = add one (up to 16), Enter = prompt box with the regions and the box selection attached, RMB / Esc / C = leave and drop the regions. Esc in that prompt box goes back to snip mode with the regions; a successful submit ends it. |
| R | Fly back to the camera of the most recently finished request |

Selected boxes get an orange tint and outline, plus a faint always-on-top outline so they can be found behind walls. The HUD shows how many boxes are selected and their group names. The selection survives map hot reloads: boxes are matched by identical geometry first, then by the same map line + index within that line + group name. The HUD reports any that were lost.

**Prompt box.** Typing; Backspace / Delete (Ctrl+Backspace deletes a word); ← → ↑ ↓; Home / End (with Ctrl: the whole text); Ctrl+V paste; Shift+Enter new line; **Enter sends** the request; **Esc cancels** and discards the text. The limit is 4095 bytes, shown as a counter. While the box is open, every game key and the camera are frozen and the mouse cursor is free (they stay frozen for 2 more frames after it closes). An empty prompt can't be sent. If sending fails, the box stays open with the text, and the HUD shows the error and the bundle path.

**Command line** (the `main.c` header comment is authoritative)

| Flag | Meaning |
|---|---|
| `--pos x y z` | Player start position (**feet**) |
| `--eye x y z` | Start position of the **eye** (= `--pos x y-1.62 z`). With `--view`, it moves the spectator camera there instead. Request bundles use it in their reproduction command. |
| `--look yawDeg pitchDeg` | Start view angles. With `--view`, they override the viewpoint's angles. |
| `--view n` | Start in spectator mode at viewpoint *n* (1-based) |
| `--off <digits>` | Start with these effect toggles off, e.g. `--off 16` |
| `--ao <mode>` | Screen-space AO / indirect trade-off (`atmosphere.c`, §7.5): `full` (default), `ssao` (SSIL off), `lite` (SSIL off, SSAO 8 samples), `off` (SSAO + SSIL off). Measured 2026-10-10 (headless, spinning): −0.62 / −0.67 / −0.93 ms per frame; the look barely changes (soft darkening along wall feet in close, shaded views goes away; no visible bounce loss). |
| `--bloom on\|off` | `off` skips the bloom pass (−0.25 ms) and raises exposure ×1.19 instead: bloom (threshold 0, MIX 0.04) is mostly a uniform ~20% lift here, so the mean brightness stays within ±1.2/255; only the faint halo around bright openings goes. |
| `--fx key=value` | Override one effect setting, repeatable, applied after `--ao`/`--bloom`: `ssao.samples`, `ssao.radius`, `ssao.intensity`, `ssao.power`, `ssil.samples`, `ssil.radius`, `ssil.gi`, `ssil.ao`, `bloom.levels`, `bloom.intensity`, `exposure`. For experiments. |
| `--map <file>` | Load this map instead of `maps/monolith.map` (path relative to the working directory). It is hot-reloaded like the default map. |
| `--dump-boxes <file>` | Load the map, write every box as hex floats (`%a`, one `min max` line per box) to `<file>` and exit **without opening a window**. Use it to check that an edit keeps the exact box list, and as a quick parse check (a broken map prints `file:line: message` and exits 1). |
| `--shot <file>` | Render 30 frames offscreen, save the image to `<file>` (full path OK, 1600×900 unless `--shot-size w h`), exit. Uses a **hidden window** (`FLAG_WINDOW_HIDDEN`), so the window manager (i3) never maps, shows or focuses it. Hot reload and request polling are off during shots. |
| `--shadow <size> <range>` | Sun shadow map size (px) and shadow distance (m). Default `8192 100` (clamped to 256–16384 px, 10–2000 m). |
| `--shadow-camera` | Sun shadow from R3D's camera-fitted map, re-rendered only when the cached map no longer covers the view (the pre-2026-10-10 path, §7.3). The default is static world-fixed tiles drawn once per map load (R3D patch 7). For A/B benchmarks and debugging. |
| `--shadow-always` | Camera-fitted map re-rendered every frame (the original behaviour). For A/B benchmarks and debugging. |
| `--fog-cache <voxel> <sub>` | **Trade-off, off by default.** Bake the volumetric fog's sun visibility into an 8-bit 3D texture once per map load (voxel size in m, sub³ samples per voxel; `0.5 2` = 290×202×510 voxels, 28.5 MB, 17–55 ms bake, logged) and let the fog march fetch it instead of the shadow map (R3D patch 8). −0.2 ms per frame; shaft edges in the hall get ~0.5 m softer (0.5% of pixels change by > 2/255, max 20). The baked region is fixed and sized for the default map; outside it the march falls back to the shadow map. Voxel size is clamped to 0.25–8 m, sub to 1–4. |
| `--no-prepass` | No depth pre-pass before the G-buffer pass (R3D patch 6). Same image; for A/B tests (without it the concrete shader runs for every overdrawn fragment). |
| `--bench` | Benchmark: for each viewpoint, 60 warmup frames, then 600 measured frames while spinning the camera 360°. Prints a table and writes a **Markdown report** to `benchmark/<timestamp>_shadow-<size>-<range>.md` (GPU, driver, resolution, settings, command; avg ms, avg FPS, 1% low, worst frame and number of shadow-map re-renders per view). Reports are written only for completed runs. |
| `--headless` | With `--bench`: hidden window (never mapped), offscreen 1920×1006 target, `glFinish` after every frame. Doesn't touch the screen or need workspace 10 shown. Absolute times are ~10% higher than visible runs (no CPU/GPU overlap) and noisier if the machine is in use, so compare headless only with headless. |
| `--bench-frames <n>` | Timed frames per viewpoint for `--bench` (10–600, default 600). Use ~150 for quick screening runs (~10 s); keep 600 for numbers you report. Fewer frames = faster spin, so more shadow re-renders. |
| `--bench-still` | Like `--bench`, but the camera doesn't spin (the still-camera case: no shadow re-renders). Report suffix `_still`. |
| `--no-frame-reuse` | Always render with R3D. By default the **interactive** window reuses the last R3D image while nothing that affects it changed (camera, effect toggles, sun, map version, window size); such a frame is only the cached image + HUD/overlay, the HUD fps reads `(cached)`, and the loop then sleeps to the monitor's refresh rate instead of spinning (§6, Frame reuse). |
| `--frame-reuse` | Turn frame reuse on where it is off by default: in `--shot` (to prove a reused frame equals a fresh one; logs how many frames were reused) and in `--bench-still` (times a reused frame; report suffix `_reuse`, no GPU profile). |
| `--no-profile` | With `--bench`: no per-pass GPU table. The R3D GPU profiler (patch 5) is compiled in by `build.sh` (`R3D_GPU_PROFILER`) and switched on only by `--bench`; every report then lists the GPU time per pass (geometry, shadows, SSAO, SSIL, fog march, …). |
| `--note "text"` | Free-form note placed at the top of the benchmark report (e.g. what changed). |

**Designer debug flags** (for testing without a visible window)

| Flag | Meaning |
|---|---|
| `--pick` | With `--shot`: print what the crosshair (screen centre) hits on the shot frame: box index, group, map line + its text, point, normal, distance, box min/max. Works in walk mode too. |
| `--select <line>` | Select every box made by this map line (repeatable, up to 64) |
| `--region x y w h` | Attach a screen region (pixels of the shot, top-left origin) as if dragged in snip mode (repeatable, up to 16). Drawn with `--shot-hud`, sent with `--submit`. |
| `--submit "text"` | With `--shot`: after saving the shot, run the real submit path. It renders `view.png` + `view-marked.png` offscreen and calls `LauncherSubmit` with the centre pick, the `--select` boxes and the `--region` regions, then prints `submit: ok/failed: <message>`. **It writes a real bundle into `requests/` and launches a real terminal + Claude Code**, so for tests set `MONOLITH_TERMINAL` to a stub script that only logs its arguments, and move the bundle to the trash afterwards (`gio trash requests/<id>`, never `rm`). |
| `--type "text"` | Open the prompt box with this text (cursor at the end) |
| `--shot-hud` | With `--shot`: draw the HUD, crosshair, selection overlay, regions and prompt box into the shot. Plain shots never contain them. |
| `--shot-size w h` | Size of `--shot` images (default 1600 × 900). Request bundles pass the window size in their reproduction command so it frames like `view.png`. |
| `--reload-test <file>` | With `--shot`: after 10 frames, load `<file>` through the real hot-reload path, then render the rest. The shot must equal a plain `--shot --map <file>`: this proves that every static cache (instances, shadow tiles, fog volume, frame reuse) is rebuilt on reload. |

In `--submit` and `--type` text, a literal `\n` becomes a new line.

**Environment variables** (launcher)

| Variable | Meaning |
|---|---|
| `MONOLITH_TERMINAL` | Terminal to open, as a program name or path plus optional extra arguments (`"kitty --single-instance"`). It takes precedence over `TERMINAL`. |
| `TERMINAL` | Used when `MONOLITH_TERMINAL` is unset. If neither is set, the first of `alacritty`, `kitty`, `foot`, `wezterm`, `xterm` found in `PATH` is used. |
| `MONOLITH_CLAUDE_ARGS` | Extra Claude Code flags, split on whitespace (no quoting), e.g. a permission mode or a model. Default: none. Permissions are never bypassed by default. |

Vsync is **off** and rendered frames are uncapped; reused (cached) frames sleep to the monitor refresh rate (frame reuse, §6). For benchmarks, also disable driver vsync:
`__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench` (NVIDIA) or `vblank_mode=0` (Mesa).

## 6. Code architecture (`src/*.c`)

The program is split into modules, and each one has a header with the interface. `build.sh` compiles `src/*.c`, so a new module needs no build change. Larger files use `// #region` blocks.

1. **World** (`world.h`, `world.c`): `World world` holds a fixed array of `BoundingBox` (`MAX_BOXES` 8192). Every box is used **both for rendering and for collision**. Parallel to the boxes, `refs[]` holds a `BoxRef` for each box: its **source line**, **primitive index** and **group index**. Also stored: the group names (`MAX_GROUPS` 256, up to 63 characters), a **`version`** counter that goes up on every successful load (static caches key on it), and the map's path, mtime and FNV-1a 64 hash.
2. **Map** (`map.h`, `map.c`): the map format (reference in the header comment of `map.c`, and in `AI-MAP-TOOL.md` §5) and its loader.
   - The primitive builders `Box`, `Block`, `Stairs` and `WaffleFacade` are the former `BuildWorld()` helpers, with their float math kept exactly as it was. The prefixes are `repeat n dx dy dz` (up to 3 per line, for grids) and `at x y z`.
   - `LoadMap()` parses into a temporary `World` and swaps it in **only if the whole file is valid**. Rendering and collision therefore never see a half-loaded map. Errors are reported as `file:line: message`, and the loader never crashes on bad input.
   - `MapPoll()` is the **hot reload**. Every 0.5 s it checks the file's mtime with `GetFileModTime`. After an mtime change it keeps re-reading for 2 s, because mtime has 1 s resolution and editors may write in steps. It reloads when the content hash differs from the last attempt. On `MAP_RELOADED`, `main()` rebuilds the instance buffer (the new one is created **before** the old one is freed: R3D caches the buffer names it last bound and would otherwise keep drawing the old boxes when GL hands out the same names again), and the sun shadow tiles, the fog volume and the frame-reuse key all re-key on `world.version`, and the HUD shows `map reloaded (N boxes)`. On `MAP_RELOAD_FAILED` the old map stays and the error stays in the HUD. Collision uses the new boxes at once; a player who ends up inside a box is left there.
   - `MapGetLine()` returns a line of the loaded map's text, so the HUD and the bundles can quote it.
   - `main()` loads the map **before opening a window**, so a broken map on the command line just prints the error and exits 1.
3. **Player** (`player.c`): feet-based AABB (r 0.3, h 1.75, eye 1.62). Movement is per axis with collision against all boxes using **`BoxesOverlap` (strict)**; see §7.4. It includes step-up for stairs (`STEP_HEIGHT` 0.45), gravity, jump, fly/noclip, smoothed acceleration and head bob. `PlayerCamera()` builds the camera.
4. **Spectator camera** (`spectator.c`): the `Spectator` state, the `VIEWPOINTS[]` table (name, eye, target), `UpdateSpectator()`, `SpectatorFlyTo()` / `SpectatorFlyToPose()` / `SpectatorJumpTo()`, and `SpectatorCamera()`. Yaw/pitch convention: `LookDir(yaw, pitch) = (cos p · sin y, sin p, cos p · cos y)`, so yaw 0 looks toward +Z and yaw 180° toward −Z. Camera right is `(−cos y, 0, sin y)`.
5. **Designer** (`designer.c`): spectator-mode editing UI. It holds one `Designer` state, owned by `main()`.
   - **Picking**: `DesignerPick()` is a slab test of the camera's centre ray against every box. Boxes that contain the eye are skipped, so you can pick out of a wall you are inside. The result (box, point, axis-aligned face normal, distance) is refreshed every frame in spectator mode and in `--shot` runs.
   - **Selection**: up to 512 box indices with keys, and `DesignerSync()` remaps them after a reload (§5).
   - **Overlay**: `DesignerDrawOverlay()` draws with raylib after `R3D_End()`, inside `BeginMode3D` with the same camera. It is depth-tested against R3D's depth, which `R3D_End` copies into the target, with a 4-step depth bias in the projection matrix and slightly inflated boxes against z-fighting. It also draws the crosshair and the HUD lines.
   - **Prompt box**: UTF-8 editing, word wrap with raylib's default font, scrolling, 4095-byte limit. `DesignerCapturesInput()` tells `main()` to suppress game keys and the camera.
   - `DesignerBuildRequest()` snapshots everything the launcher needs into a `DesignRequest`: prompt, eye, yaw/pitch, fovy, size, hit, selection, map version.
6. **Launcher** (`launcher.c`): see its header comment.
   - `LauncherSubmit()` writes `requests/<YYYY-MM-DD_HHMMSS>_<slug>/` with `view.png`, `view-marked.png`, `request.md` and `request.json` (contents in `AI-MAP-TOOL.md` §2). Then it starts the terminal with `fork` + `execve`, from an argv array with no shell, so the user's text never reaches a command line.
     - The child calls `setsid` and `chdir`s to the repo root, with stdin from `/dev/null`. Exec failures come back through a close-on-exec pipe. Children are reaped with `waitpid(pid, WNOHANG)`.
     - Each terminal's "run this argv" syntax (title, working directory, exec flag) comes from the `TERMINALS[]` table; an unknown terminal gets `-e <command>`. The window title is `monolith-claude: <slug>`, for i3 rules.
     - `claude` is searched in `PATH`, then in `~/.local/bin`, and passed as an absolute path.
     - If launching fails, the bundle is kept, the HUD names its path, and the log prints the command to run by hand.
   - `LauncherUpdate()` polls every open request (up to 16) for `result.md` and shows its first line in the HUD for 20 s. `LauncherLastDone()` gives the camera for **R**.
   - **Windows**: the same bundle, then `cmd /c start "monolith-claude: <slug>" /D <exe dir> cmd /k "<claude>" … "<prompt>"` via `_spawnvp`. This path is only compile-tested.
7. **Atmosphere** (`atmosphere.c`): `SUN_DIR` (the direction light *travels*) and `SetupEnvironment()`. It sets the procedural sky, the IBL ambient map (generated from a separate ambient-only sky, §7.5), distance fog, volumetric fog, SSAO, SSIL, bloom, tonemap and colour grading.
8. **Sun shadow** (`shadow.c`): the default **static world-fixed tiles** (`SunShadowTileSelect`, drawn once per map load, §7.3), and the camera-fitted reuse path for `--shadow-camera` / `--shadow-always` (`SunShadowFit`, `ShadowEdgeFade`, `SunShadowNeedsUpdate`). `SunShadowBoxMatrix` gives the light matrix for the `--fog-cache` bake.
9. **Benchmark** (`bench.c`): frame statistics and the Markdown report for `--bench`.
10. **Main** (`main.c`):
    - the header comment (controls and flags), argument parsing, `InitDataPaths()`, and map loading (+ `--dump-boxes`);
    - the shadow-size hint, `R3D_Init`, `glEnable(GL_DEPTH_CLAMP)`, and the world as **one instanced draw** (`LoadWorldInstances()`: a unit cube; per-instance position = box centre, scale = box size);
    - the material + surface shader, and the sun light + shadow map, all kept in a `Scene` struct;
    - the main loop: toggles, TAB, map hot reload, designer update, camera update, draw (with frame reuse, below), overlay, HUD, `--shot` / `--bench` handling;
    - **depth pre-pass** (R3D patch 6): `concrete.alphaCutoff = 0` (the concrete never discards) lets R3D lay depth first, so the procedural shader runs once per visible pixel instead of once per overdrawn fragment. `--no-prepass` turns it off for A/B tests;
    - **frame reuse** (comment block "Frame reuse" in `main.c`): a `FrameKey` holds everything that changes R3D's image (camera, the whole `R3D_Environment`, sun light + shadow structs, toggle 6, map version, window size), compared bytewise each frame. While it changes, R3D renders straight into the window. On the first unchanged frame R3D renders once more into a cache target (colour + depth *texture*); later unchanged frames only composite it with a shader that also writes `gl_FragDepth` (the window is 4× MSAA, so no blit), and the overlay/HUD go on top. A reused frame costs ~0.2 ms instead of ~4–5 ms, and the loop then sleeps to the monitor's refresh rate. Interactive only; off in `--shot` / `--bench` unless `--frame-reuse`. **Anything new that changes the image (a uniform, a light, a setting) must go into `FrameKey`**, or a still camera will keep showing the old image;
    - `Submit()`, which renders the request images through the same `DrawScene()` as the main view, and cleanup.

Rendering per frame (`DrawScene()`; `--shot` and the request images render into a `RenderTexture`, the window view into the default framebuffer):
```c
R3D_BeginPro((R3D_View){ .camera = R3D_CameraFromRL(camera), .target = target });
    int tile = SunShadowTileSelect(&sunTiles, ..., camera.position, world.version, &update);
    R3D_PushLightFixedShadow(sun, sunShadow[tile], sunTiles.tile[tile].viewProj, update); // drawn once per map load
    // (--shadow-camera: update = SunShadowNeedsUpdate(...); R3D_PushLightEx(sun, sunShadow[0], update))
    R3D_DrawMeshInstanced(cube, concrete, instances, world.count);
    // --fog-cache: R3D_BakeFogSunVisibility(...) once per map load
R3D_End();
// then, on the same target: DesignerDrawOverlay (depth-tested), crosshair, HUD, prompt box
```

### 6.1 The default map (`maps/monolith.map`; metres; Y up; the player walks toward −Z)

The default map is the original hand-built level, moved out of the former `BuildWorld()`. It produces exactly the same box list (279 boxes, 13 groups). The group names are how humans and AIs refer to its parts:

| # | Area | Groups | Location |
|---|---|---|---|
| 0 | Ground slab, 800 × 800 m, top at y = 0 | `ground` | x, z ±400 |
| 1 | Entry canyon: 34 m walls, roof segments with thin light slits; the player starts at `(0,0,90)` | `entry canyon: walls`, `entry canyon: roof`, `entry canyon: back wall` | x ±4.5, z 40…97 |
| 2 | Hypostyle hall: 2.2 m pillars on a 9 m grid, 20 m tall, slatted roof beams; a massive block sits on the roof | `hypostyle hall: pillars`, `hypostyle hall: roof`, `hypostyle hall: cantilever` | x ±40, z −36…39 |
| 3 | Monumental stairs: 40 steps, 12 m rise, with side walls | `monumental stairs` | z −40…−70 |
| 4 | Upper terrace (y = 12) with a waffle-facade tower (to y = 96) and a lone pillar | `upper terrace`, `tower`, `terrace pillar` | z −70…−140 |
| 5 | Distant megastructures (up to 260 m tall) and a bridge at y 120; fog silhouettes | `megastructures`, `bridge` | 120–300 m out |

### 6.2 Concrete shader (`resources/shaders/concrete.glsl`)

An R3D **surface shader** (`#pragma usage opaque shadow`, `void fragment()`). Everything is derived from the **world-space `POSITION`** and **`NORMAL`**, so boxes of any size keep a constant texel density without UVs. Because boxes are axis-aligned, it projects onto the dominant axis plane (not full triplanar blending).

- **Vertical faces:** 2.4 × 1.2 m formwork panels with per-panel tone, seams, 15 cm board marks, 2×2 tie holes per panel, and patchy rain streaks.
- **Top faces:** 6 m expansion joints and wet patches (low roughness).
- **All faces:** fbm mottling and grain; grime and a moss tint near the ground (y < 2.2).
- **Base albedo:** about `0.22 + 0.12·noise`, kept dark so sunlit concrete doesn't blow out to white.
- **Uniform:** `u_scale` (pattern scale, 1 = metres).

Fragment built-ins are listed in `submodules/r3d/docs/shaders/surface_shader.md` (`ALBEDO`, `ROUGHNESS`, `METALNESS`, `SPECULAR`, `EMISSION`, `NORMAL_MAP`, …).

## 7. R3D pitfalls already discovered (read before touching lighting)

These were found by debugging. They explain why some settings look the way they do.

### 7.1 Volumetric fog turns into a black curtain if dense
R3D's volumetric fog is **lit only by lights, not by the sky/IBL**. Fog in shadow therefore absorbs light without adding any, and with high density, buildings 30–60 m away go **pitch black depending on view direction**.

The fix is in place: low `scatteringDensity` (0.012), low absorption (0.001), and a small **`emissionEnergy`** (0.12, coloured like the distance fog) standing in for sky light. Raise the shafts with `sun.fogEnergy` rather than with density.

### 7.2 Directional shadow map is camera-fitted; casters get clipped
R3D fits the sun's ortho shadow map to a bounding sphere of the camera frustum out to `light.range`. Its near plane reaches only about `radius + 100 m` toward the sun, so tall casters (the top of a 260 m megastructure sits about 330 m up the light ray) were **clipped depending on where the camera looked, and their shadows popped**.

The fix is **shadow pancaking**: `glEnable(GL_DEPTH_CLAMP)` once after `R3D_Init`. R3D never touches that state, so it applies to R3D's shadow pass. The side effect is that geometry closer than the 1 cm camera near plane (raylib's `RL_CULL_DISTANCE_NEAR` 0.01) is clamped instead of clipped.

**Do not "fix" shadow problems by disabling shadow casting.** That hides the symptom; fix the cause.

### 7.3 Sun shadow: resolution and cost
- **Texel size:** about `2 · radius / size`, with `radius ≈ 1.5 · range` (70° fov, 16:9). At 1920×1006, range 100 m: radius ≈ 159 m.
- **Fade:** R3D fades directional shadows out between 75% and 100% of `range`, by **distance from the eye** (R3D patch 4; upstream uses view depth, which changes when the camera only turns, see §7.6), and over the outer 1% of the map (x, y and depth; upstream 5%, changed by R3D patch 7, no visible difference).
- **Range lower bound:** `range` must stay at or above `volumetricFog.length` (90 m) or the light shafts fade early.
- **Current setting:** 8192 px / 100 m, about 3.7 cm texels. VRAM is 16-bit depth × 2 layers, about 256 MB.
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

**Static sun shadow tiles (default since 2026-10-10, R3D patch 7).** Sun and boxes are static, so the shadow depth is a pure function of the map. Instead of R3D's camera-fitted map, the app supplies the light matrix (`R3D_PushLightFixedShadow`): `SunShadowTileSelect()` (`src/shadow.c`) lays world-fixed orthographic tiles on a grid in light space, each with exactly the radius, depth window and texel lattice of R3D's fit at 70°/16:9 (3.69 cm texels at 8192/100), so pancaking, biases and PCF footprint are unchanged. The tile is chosen **by eye position only**, so turning never re-renders. A tile is used only while everything within `range` of the eye lies inside its fade-free area, so fades are exactly 1 wherever shadows show. The two tiles live in R3D's two existing directional shadow layers (VRAM unchanged); with `TILE_HOME` placed as it is, all 7 viewpoints and the verification shots fall into those two cells, so in the play area each tile is drawn **once per map load**. Far out, a new tile is drawn every ~90 m of travel. Image vs. the camera-fitted map: 326 pixels > 2/255 over 26 shots (16-bit depth rounding, max 15/255). The gain is mostly CPU: the per-frame coverage check below (~0.4 ms) goes away, plus the re-renders while moving (~1.2 ms + 0.35 ms min/max each). **If R3D's fit (`light_dir_view_proj`), its zExtension or `SHADOW_DIR_EDGE_FADE` change, update the `TILE_*` / `SHADOW_EDGE_FADE` constants in `shadow.c`.** The camera-fitted reuse path described next is kept as `--shadow-camera`.

**Camera-fitted shadow-map reuse (`--shadow-camera`; the default before the tiles).** The scene is static, so the map is re-rendered only when needed: `R3D_PushLightEx(sun, sunShadow, update)`. R3D stores the light matrix with the cached map, so a reused map is always sampled consistently. And since R3D's fit radius depends only on range/fov/aspect and the centre is snapped to texels, every fit lays the **same world-space texel grid**, so the reused map has the same content where it covers. `SunShadowNeedsUpdate()` replicates R3D's fit (`light_dir_view_proj`) on the CPU and samples the view frustum out to `range` (48 depth slices × 33×17); it keeps the map only while R3D's fades (edge × distance, the distance fade by distance from the eye as in patch 4) from the cached map are within 1/255 of a fresh fit's at every sample. The check costs ~0.1–0.4 ms CPU (0.43 ms measured in the bench spin, 2026-10-10). **If R3D's fit or fade code changes, update `SunShadowFit()` / `ShadowEdgeFade()` / the `distFade` in `SunShadowNeedsUpdate()` (`src/shadow.c`) to match.**
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

**Caching results (2026-10-10).** Headless `--bench`, 1920×1006, interleaved runs, 3 per row unless noted, all in one session (reports `benchmark/2026-10-10_1742*`–`1748*`). "Original" = the build before the map designer work (patches 1–3); "p3base" = Release R3D + profiler + patch 4, before caching.

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

### 7.4 Other gotchas
- **`CheckCollisionBoxes` is inclusive:** touching counts as a hit, so a player standing on the ground could never move. Use the project's strict `BoxesOverlap()`.
- **Brightness:** the sun at energy 3.5 with exposure 1.1 made the buildings look white. The current values are sun 2.0, exposure 0.9 (AgX), ambient 0.9 and fog colour grey 150. If things look washed out, lower the **sun energy** or **exposure**. Don't brighten the fog colour.
- **Sun angle:** a low sun from behind the player left every visible face in shadow and produced no shafts. `SUN_DIR = (0.55, −0.78, 0.30)` (high, from the side) is chosen so light falls through the roof gaps and rakes across facades.
- **Distance fog:** EXP2 at density 0.0085 makes aerial and long views (>150 m) very milky. Spectator viewpoints are kept fairly close for that reason.
- **Instance attributes:** the buffer holds only `POSITION | SCALE`, with no rotation. All world boxes are axis-aligned, and both collision and the shader rely on that.
- **Surface-shader uniforms:** they are uploaded at `R3D_End()`, so the last value set in a frame wins. They can't vary per draw within a frame.
- **Hidden-window screenshots:** in `--shot` mode the window is never mapped, so reading the default framebuffer (`TakeScreenshot`) would be undefined: no pixels are owned. Shots are rendered with `R3D_BeginPro` into a `RenderTexture` and read back with `LoadImageFromTexture`, flipped vertically. Don't switch back to `TakeScreenshot`.
- **NVIDIA accepts GLSL that AMD/Intel reject.** Wine uses the host driver, so on this machine neither Linux nor wine shows these errors. Two found so far (shaders failed to compile on a friend's Windows PC with an AMD RX 9060 XT): a function named `noise3` in `concrete.glsl` (clashes with the deprecated built-in `noise3()`, which returns `vec3`; now `valueNoise`), and a uniform-block member named `length` in R3D (`uVFog.length`, patched; see `submodules/r3d/patches.md`). Avoid GLSL built-in names (`noise1`–`noise4`, `length`, `distance`, …) for functions and members, and check shader changes with **`glslangValidator`** (installed), which rejects the same things. For `concrete.glsl`, wrap it in a stub that declares the built-ins it uses and calls `fragment()` from `main()`.
- **Instance buffers on reload:** R3D caches the GL buffer names it last bound for instancing (`instances_enable` in `src/modules/r3d_render.c`) and skips re-binding when they match. Freeing the old instance buffer before creating the new one let GL hand out the same names, so a hot-reloaded map was drawn with the **old** boxes. `main()` therefore creates the new buffer first and frees the old one after. The proper fix would be in R3D (`R3D_UnloadInstanceBuffer` resetting that cache); not patched.
- **Ambient colour vs. ambient map:** setting `ambient.map = {0}` and using a plain `ambient.color` produced no ambient light in testing. Keep the IBL ambient map generated from a procedural sky (§7.5).

### 7.5 Shadowed undersides: the ambient sky's ground is the fill light
Downward-facing surfaces (waffle soffits, beam and bridge undersides, the hall ceiling under the cantilevered block) used to render **near pitch black**. Measured with `--off` (2026-10-10):
- **Cause: the IBL's lower hemisphere.** The ambient map was built from the visible sky, whose ground is `groundBottomColor` (48,46,44) ≈ 0.03 linear. R3D's default `groundHorizonCurve` 0.02 makes the *whole* lower hemisphere that colour, so a down-facing normal got almost no ambient. With the sun off (`--off 6`) soffits and the hall ceiling were exactly (0,0,0). The near-zero base also made the warm volumetric in-scatter on them come out **maroon**, e.g. (19,0,0), once AgX, contrast and the 0.8 saturation were applied.
- **Not the cause: SSAO and SSIL.** Each changes whole-frame mean brightness by < 0.5/255 in every test view, before and after the fix. SSIL only bounces light from lit pixels on screen, so it can never stand in for the ground when looking up. SSAO (1.2), SSIL (`giIntensity` 1.2) and `ambient.energy` (0.9) are unchanged.
- **Their cost vs. their effect (2026-10-10):** SSIL ≈ 0.5 ms and SSAO ≈ 0.2 ms GPU per frame (plus the normal/radiance downsamples; the depth pyramid itself stays, volumetric fog needs it). Net, SSIL *darkens* (its own 5 m AO term outweighs its bounce), so turning it off brightens slightly. With both off the largest change in the 26 verification shots is the soft darkening band along wall feet in close, shaded views (view 1: up to +37/255 in that band, +1.35/255 mean); views 2/3/5 and the steep view change < 0.5/255 mean. Presets: `--ao ssao|lite|off`, §5.

**Fix (in place):** `SetupEnvironment()` builds the ambient map from a second, **ambient-only** procedural sky (256², unloaded right after `R3D_GenAmbientMap`). It is a copy of the visible sky with `groundBottomColor = (120,116,108)` ≈ 0.19 linear, slightly warm, and `groundHorizonCurve = 0.1`. The visible background sky, the distance fog and the sun are untouched, and the frame cost is unchanged: no new pass or light, only init time. The upper hemisphere stays the cool sky, so shadowed faces get warm bounce from below and cool light from above.
- Fully sunlit 0.25-albedo ground would be ~0.12 linear (albedo · sun 2.0 · cos 39° / π); 0.19 was picked by eye. Result: shadowed undersides at ~10–25% of sunlit faces in linear terms (hall ceiling (0,0,0) → (47,43,37), facade soffit (38,26,6) → (64,56,45), sunlit pillar unchanged at (149,141,130)). Formwork joints, mottling and corner AO read in shadow; sun patches, shafts and shadow shapes are unchanged.
- To tune the fill, change `ap.groundBottomColor` (brightness/warmth of under-lighting) and `ap.groundHorizonCurve` (how much of the bright `groundHorizonColor` reaches near-vertical faces). Keep the visible sky's ground dark: it shows below the horizon on high views.
- **Don't add a fill light for this.** A shadowless directional "bounce" light isn't needed. It would also light the volumetric fog unless `fogEnergy = 0` (§7.1).
- Side effect, physically right: the ambient *reflection* (prefilter) now uses the brighter ground too, so wet (low-roughness) patches facing sideways pick up a bit of it.

### 7.6 Effects must not change when the camera only turns (R3D patch 4)
From a fixed eye, merely rotating the camera used to change how a given world spot looked, most visibly the fog. Examples: the hall's far opening glowed in the screen centre and dimmed at the edge, and the distance fog thinned toward the screen edges. **Cause:** four terms used the pixel's **view depth** (distance along the camera's forward axis, = distance · cos of the angle off-axis, only 0.57 · distance at a 16:9 corner) where they mean distance from the eye. The terms were the volumetric-fog shadow fade and patch 2's skip (`vfog_radiance.frag`), the fog transmittance (`vfog_transmittance.frag`), distance fog (`fog.frag`) and the sun-shadow fade on surfaces (`lighting.frag`). **Fix (patch 4):** all four now use the distance from the eye. `src/shadow.c` replicates the new fade. Details and measurements are in `submodules/r3d/patches.md`.
- **Rule:** any fade, fog or cache term must be a function of the world position (and the view ray), never of view depth. Static caches (shadow, fog, AO) would otherwise bake a view-dependent error in.
- Still view-dependent, by nature or unused: the screen-space passes (SSAO, SSIL, bloom, FXAA, the fog's per-pixel jitter and bilateral upsample) stay at reprojection-noise level. `scene/forward.frag` and `scene/unlit.frag` still use view depth, but monolith doesn't draw forward/unlit objects. The camera-fitted shadow map (`--shadow-camera`) still moves with the view, which affects re-render cost but not the image (§7.3, proposal 3); the default static tiles don't.

## 8. How to verify changes

There's no test suite; verification is **visual and numeric**:

1. **Build cleanly:** `./build.sh` and `./build.sh --win` with no warnings.
2. **Screenshots from fixed views:**
   ```bash
   ./build/linux/monolith --shot a.png --view 2                       # preset viewpoint
   ./build/linux/monolith --shot b.png --pos 0 0 -5 --look 120 5      # exact view
   ./build/linux/monolith --shot c.png --view 2 --off 16              # with effects off
   ```
   For anything lighting- or shadow-related, take a **sweep of yaw angles from the same position** (e.g. 0/60/…/300) and compare. Both the black-curtain and shadow-popping bugs only showed up when rotating.
3. **Isolate effects** with `--off` (1 volumetric fog, 2 fog, 3 SSAO, 4 SSIL, 5 bloom, 6 sun) to find which one causes a problem.
4. **Performance:** `__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench [--shadow S R] --note "what changed"`. Each run writes a report into `benchmark/`. Laptop GPUs vary, so alternate A/B runs several times and compare averages (run-to-run noise is ~1–2%). Screen ideas with `--bench-frames 150` (~10 s a run); use full runs ×3 only for numbers you report. Unlike `--shot`, `--bench` opens a **visible** window on purpose (it measures real presentation).
   **Automated / AI-driven runs: use `--bench --headless`** so the developer's screen is never taken over. A *visible* bench window must actually be shown: on a workspace that isn't displayed (i3 unmaps it) the run stalls at 100% CPU with the GPU idle and never finishes. Use visible runs only when the developer asks for presentation-inclusive numbers.
5. **Window titles and i3:** automated runs use their own titles (`monolith-bench`, `monolith-shot`; normal play is `monolith`). The developer's i3 config (`~/.config/i3/config`) has
   `for_window [title="^monolith-(bench|shot)$"] move container to workspace number $ws10`,
   so automated runs open on workspace 10 and never take over the focused workspace. Keep these titles if you change window creation. Terminals launched for requests are titled `monolith-claude: <slug>`, so i3 rules can place them too.
6. **Movement:** collision and controls can't be checked from screenshots; test them by playing.
7. **Map loader and map edits:**
   - `--dump-boxes <file>` writes the exact box list (hex floats) without a window. `diff` the dumps from before and after a loader change; they must be identical for the default map. Diff them after a map edit to confirm that only the intended boxes changed.
   - A broken map given with `--map` (or the default one) makes the program print `file:line: message` and exit 1 before opening a window. In the running game a broken save keeps the old map instead.
   - Hot reload itself only runs in the interactive game, never in `--shot` / `--bench`.
8. **Designer and launcher without a window:**
   - `--shot x.png --view 2 --pick` prints the centre hit. It must be a `hypostyle hall: pillars` box from map line 47, at distance ≈ 42.07 m.
   - `--shot-hud` (with `--select <line>` / `--type "text"`) puts the overlay, HUD and prompt box into the shot.
   - `--submit "text"` runs the real submit path. **Always** with `MONOLITH_TERMINAL=/path/to/stub.sh` (a script that only logs its arguments), so no real terminal or Claude session starts. Move the bundle to the trash afterwards (`gio trash requests/<id>`, never `rm`).
   - Mouse and keyboard input (clicks, typing, paste, Esc) can only be tested by hand.

## 9. Common modifications

**Add architecture.** Edit the map file (`maps/monolith.map`, or another one loaded with `--map`): add `box` / `block` / `stairs` / `waffle` lines in a well-named `group`. The format and the editing rules are in `AI-MAP-TOOL.md` §5–6. The running game hot-reloads the file, so there is no rebuild. Rendering, shadows, collision and picking pick the new boxes up automatically. Keep boxes axis-aligned and keep the total under `MAX_BOXES`. Think in real scale: player 1.8 m, pillars 20 m, buildings 50–260 m. Or just submit the change as a request from inside the game.

**Add a map primitive.** Add a builder next to `Box`/`Block`/`Stairs`/`WaffleFacade` in `map.c`, plus a `PRIMS[]` entry: the name, the argument count, and per argument the axis it is shifted along (`AX_X/Y/Z`), `AX_NONE` for sizes, or `AX_INT` for counts. Then add its call where `ParseStatement()` dispatches. That code currently switches on the argument count (box and block, both 6, are told apart by the `PRIMS[]` entry), so a new primitive with 6–8 arguments means switching on the entry instead. Builders must return false when the world is full, and loops must add a box per iteration so `MAX_BOXES` bounds them. Document the primitive in the `map.c` header comment, at the top of `maps/monolith.map` and in `AI-MAP-TOOL.md` §5.

**Change what a request bundle contains.** Change `launcher.c` (`BuildBundle`, `WriteRequestMd`, `WriteRequestJson`; bump `"format"` in the JSON when keys change) and update the bundle description in `AI-MAP-TOOL.md` §2. Add a terminal to `TERMINALS[]` in `launcher.c` (its title, working-directory and exec flags).

**Add a spectator viewpoint.** Fly there in spectator mode, press **P**, and add an entry `{ "Name", {eye}, {target} }` to `VIEWPOINTS[]`. Note that P prints *feet* position (eye − 1.62) for `--pos`, while `VIEWPOINTS` stores the *eye*. `--bench` automatically covers new viewpoints.

**Tune the look.** Everything is in `SetupEnvironment()`, plus the sun setup in `main()` (energy, `fogEnergy`, `range`, shadow `softness`). Respect §7.1–7.3. To see what one effect contributes, toggle it live with keys 1–6.

**Change the concrete.** Edit `concrete.glsl`; no recompile is needed, since it loads at start-up. Keep the albedo dark (concrete is about 0.2–0.4 linear) and derive everything from world position.

**Add a new effect toggle.** Extend the `TOGGLE(KEY_x, 'x')` block in the main loop, the HUD string, and the header comment.

**Add a CLI flag.** Parse it in the loop at the top of `main()` and document it in the header comment and in §5 here.

## 10. Conventions

- **English** for all code, identifiers, comments and project names.
- **Match the existing style:** 4-space indent, `static` functions, PascalCase for functions, `UPPER_CASE` macros, `// #region` sections, and short comments explaining *why* rather than what.
- **One module per concern** (§6): each `src/<module>.c` has a `.h` with its interface and a header comment saying what it does. File-local functions are `static`. Shared state is minimal (`world`, the data paths). Keep `build.sh` compiling `src/*.c`, so a new module needs no build change. (`src/glibc_compat.c` is build plumbing, not program code; it compiles to nothing outside Linux.)
- **The level is data.** Geometry belongs in `maps/*.map`, not in C. C code describes primitives, not specific buildings.
- **No textures or models unless deliberately introduced.** The procedural, model-free approach is a core constraint.
- **Comment non-obvious engine workarounds** in the code with their reason (as for §7.1 and §7.2), so nobody "cleans them up".
- **Don't commit `build/`, screenshots, `benchmark/` or `requests/`.**

## 11. Known limitations and next steps

- **Map format:** only axis-aligned boxes and the four primitives, with no per-box material, colour or rotation (the instance buffer holds only position + scale, §7.4). Instances are rebuilt on every reload. Hot reload works for the map only: code and `concrete.glsl` changes need a restart.
- **Interactive input is untested by automation.** Clicks, typing, paste, Esc handling and camera freezing can only be checked by hand. Automated checks cover picking, selection by line, bundles and the launcher argv (`--pick`, `--select`, `--type`, `--shot-hud`, `--submit` with a stub terminal).
- **Windows launcher is only compile-tested.** It uses `cmd /c start … cmd /k claude …` via `_spawnvp`. A failure of `start` itself can't be detected. On Windows, and in any copied build folder, `requests/` and Claude Code's working directory are the executable's folder, which contains `maps/` and `resources/` but no sources and no `CLAUDE.md` / `AI-MAP-TOOL.md`.
- **Request flow details:** the reproduction `--shot` passes `--shot-size` with the window size, so it matches `view.png`'s framing. Only the 16 most recent requests are tracked in the HUD. If launching the terminal fails, the bundle stays and the prompt box keeps the text; pressing Enter again writes a second bundle (delete the stale one by hand). The terminal process exiting early (kitty `--single-instance`, wezterm mux) is reported but doesn't count as a failure.
- **Decals:** none yet. R3D supports them (`r3d_decal.h`); water stains, rust streaks and graffiti would add a lot.
- **Outdoor haze:** high and outdoor views are very hazy because sunlit volumetric fog fills the whole view. Height-dependent fog would help; R3D's volumetric fog is uniform.
- **Shadow cascades / cheaper fog shadow lookups:** see the R3D proposals in §7.3.
- **Vsync:** off with no toggle; screen tearing is possible.
- **Platform:** Linux, plus a Windows x86_64 cross-build (`./build.sh --win`). Tested under wine (NVIDIA driver). The first run on a real Windows PC (AMD Radeon RX 9060 XT) exposed the strict-GLSL issues in §7.4; the fixes are verified with `glslangValidator` but not yet on that PC. There's no native Windows build script (MSVC / CMake for monolith itself) and no macOS support. The Windows build has no sanitizer mode.
