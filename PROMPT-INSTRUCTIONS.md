# monolith — Project Instructions

Engine and technical reference for anyone (human or AI assistant) changing this project. Read it before changing code. Map edit requests (workflow, editing rules) are in **`AI-MAP-TOOL.md`**, the map format and the asset, import and export workflows in **`docs/knowledge/`** (index: its `README.md`; how to run larger work: `working-method.md`); measurement history and the numbers behind the caches and trade-offs are in **`docs/performance.md`**. `CLAUDE.md` holds the guard rails and hard rules and says which file to read for which task; it imports nothing.

## 1. What this is

**monolith** is an AI-assisted map designer: a first-person 3D viewer and editor in plain C on raylib + R3D. The developer walks or flies through a level; in spectator mode they point, select objects or screen regions and submit a request, which the game writes as a bundle and hands to Claude Code in a new terminal. The game hot-reloads the edited map package (product overview: `AI-MAP-TOOL.md` §1).

Design principles:
- **The level is data.** A map package (`maps/<name>/`): the map file, its shaders, models, textures and a design brief (`MAP.md`). Nothing in the engine assumes a style.
- **One runtime for editor and games.** `runtime/monolith_scene.h/.c` (§6.1) loads, draws, hot-reloads, collides with and picks a package, in the editor and in any raylib + R3D program (`--export`, `examples/`), so a scene can't drift between the two.
- **Assets are traceable.** Recorded source and licence for every external file; models are restyled through material overrides, never by editing the file.
- **Plain C on a lightweight renderer.**

Reference maps: `maps/monolith/` (the default: brutalist concrete, boxes only, one procedural shader) and `maps/pine_forest/` (stylised low-poly forest at dusk, restyled CC0 models, a texture, point lights). Engine changes keep the default map pixel-identical.

## 2. Technology

| Item | Details |
|---|---|
| Language | C11 (`-std=c11 -Wall -Wextra`): `src/*.c` (§6), `runtime/monolith_scene.c` (§6.1), the Linux shim `src/glibc_compat.c` |
| Windowing / input | raylib 5.5 (bundled with R3D, static) |
| Renderer | **R3D** 0.10.x (Bigfoot71/r3d): deferred PBR; shadows, SSAO, SSIL, SSGI, SSR, fog, volumetric fog, bloom, tonemapping, procedural sky, IBL, instancing, surface shaders |
| Assimp | R3D's model importer (glTF/GLB, OBJ, FBX, DAE); `--inspect-model` calls it directly |
| OpenGL | 3.3 core. `libGL` / `opengl32` linked only for `glEnable` (`MS_InitRenderer`, §7.2), `glFinish` (`--headless`) and `glGetString` (`bench.c`), declared by hand |
| Platform | Linux (X11/GLFW); Windows x86_64 cross-compiled with MinGW-w64, tested under wine |
| Dev machine | CachyOS, NVIDIA RTX 4050 Laptop GPU, driver 615.71 |

**R3D** is vendored as a plain copy in **`submodules/r3d`** (with raylib and assimp). Headers `include/r3d/`, shader docs `docs/shaders/*.md`, examples `examples/`. Check its source rather than guessing. Paths like `shaders/deferred/…` or `src/modules/…` in this file are relative to `submodules/r3d`.

> **R3D changes are patches.** Work around R3D from the app side where reasonable. When R3D itself must change, change it minimally and **record every change in `submodules/r3d/patches.md`** (symptom, cause, files, diff). Don't modify `~/Projects/r3d` or other projects in `~/Projects`.

## 3. Layout

```
monolith/
├── build.sh, cmake/mingw-w64-x86_64.cmake, .clangd, .gitignore
├── CLAUDE.md                    # always loaded: guard rails, hard rules, which file to read for which task
├── AI-MAP-TOOL.md               # request sessions' entry point: workflow, locating, editing rules, verification, result.md
├── PROMPT-INSTRUCTIONS.md       # this file
├── docs/                        # design-v2.md (design record), performance.md (measurements), QA-REPORT.md
│   └── knowledge/               # read on demand: map-format, models-and-textures, import-export, working-method, history
├── maps/                        # map packages (docs/knowledge/map-format.md), copied next to the executable by build.sh
│   ├── monolith/                # default map: monolith.map, MAP.md, ASSETS.md, shaders/concrete.glsl, ...
│   └── pine_forest/             # pine_forest.map, MAP.md, ASSETS.md, models/ (+ sidecars), textures/
├── runtime/                     # scene runtime (§6.1): monolith_scene.h/.c, monolith_capture.h (§6.3),
│                                #   export/ (viewer.c + build.sh templates for --export)
├── tools/                       # fetch_asset.py, normalize_model.py, record_asset.py (docs/knowledge/models-and-textures.md);
│                                #   leakcheck.sh + lsan.supp + lsan_report.py (§4); stub-terminal.sh
├── examples/                    # 01_minimal (+ scene/), 02_existing_project, 03_live_editing, 04_import_source,
│                                #   common/shot.h, build.sh (§6.3)
├── tests/models/                # small CC0 test package: every statement kind once
├── export/, requests/, benchmark/   # gitignored: F9 exports, request bundles, --bench reports
├── src/                         # the editor, one module per concern (§6)
│   ├── main.c                   # header comment (controls + flags), args, data paths, init, main loop, Submit
│   ├── common.h                 # shared includes, the global `MS_Scene *scene`, benchmarkDir
│   ├── player.c, spectator.c    # walking player + collision; spectator camera, LookDir/LookAngles
│   ├── designer.c, launcher.c   # picking, selection, overlay, HUD, prompt box; bundles, terminal launch, polling
│   ├── atmosphere.c             # --ao / --bloom / --fx overrides on top of the map's environment
│   ├── newmap.c, export.c, import.c, dirscan.c   # --new-map, --export / F9, --import, safe folder walk
│   ├── inspect.c, dumpscene.c   # --inspect-model, --dump-scene
│   ├── bench.c, windowemu.c     # --bench report; --bench-window stand-in
│   ├── settings.c, settingspanel.c   # graphics settings + file; F2 panel
│   └── glibc_compat.c           # Linux only: old glibc symbol versions (§4)
└── submodules/r3d/              # vendored R3D; local fixes in patches.md
```

**Data paths.** `InitDataPaths()` (`main.c`) sets `benchmarkDir` and the maps directory. On Linux the repo directories baked in (`-DBENCHMARK_PATH`, `-DMAPS_PATH`) win while they exist, so on the dev machine the game loads and hot-reloads the **repo's** `maps/`, not the copy in `build/`. Otherwise (and on Windows) the folders next to the executable are used. `build.sh` copies `maps/` and `runtime/` (+ `viewer.c`, for `--export`) into both output folders. `launcher.c` puts `requests/` and starts Claude Code in the repo root (`-DPROJECT_PATH`) while it exists, else (always on Windows) in the executable's directory.

## 4. Build and run

```bash
./build.sh              # -> build/linux/monolith + build/linux/maps/  (clang, -O2)
./build.sh run [args]   # build, then run with args
./build.sh debug        # -g -O0 + ASan/UBSan -> build/linux-debug/
./build.sh lsan         # -O1 -g + LeakSanitizer -> build/linux-lsan/; examples: examples/build.sh lsan
tools/leakcheck.sh [--asan]     # every main code path under LSan (or ASan + UBSan); exit 1 on a leak
R3D_DIR=/other/r3d ./build.sh   # another R3D tree;  CC=gcc ./build.sh: another compiler
./build.sh --win [run [args]]   # -> build/win/monolith.exe + build/win/maps/ (MinGW-w64); run under wine
```

Both output folders are **self-contained** (copy or zip as-is).

**R3D** is built by `build.sh`: configured on first use in `$R3D_DIR/build-linux` / `build-mingw` (a few minutes, assimp), then an incremental `cmake --build` each time (~1 s no-op; log in `<build dir>/monolith-build.log`). Delete the build dir to force a fresh configure.

**Linux:**
- R3D config: static, vendored raylib + assimp + zlib (`ASSIMP_BUILD_ZLIB`), bundled minizip (`ASSIMP_BUILD_MINIZIP`; a system minizip breaks the build), no examples, GPU profiler compiled in (`R3D_GPU_PROFILER`, on only under `--bench`; patch 5), `CMAKE_BUILD_TYPE=Release`. Options are remembered in `build-linux/monolith-configure.args`; a change re-configures (full R3D rebuild). `R3D_BUILD_TYPE=None` gives the old unoptimised build; any other value is passed as `CMAKE_BUILD_TYPE`. Bench reports state the R3D build type (no measurable difference: `docs/performance.md`).
- Links R3D, raylib, assimp, zlib and **libstdc++ statically** (`-l:libstdc++.a`). Runtime deps only `libc`, `libm`, `libgcc_s.so.1`, `libGL.so.1` (check: `readelf -d build/linux/monolith | grep NEEDED`).
- **glibc ≥ 2.34.** `build.sh` passes `-Wl,--wrap=` for newer symbols and `src/glibc_compat.c` routes them to old versions (libm to `@GLIBC_2.2.5`, `arc4random` → `getrandom`, `__isoc23_strtoul` → `strtoul`). After changing compiler, glibc or dependencies, `objdump -T build/linux/monolith | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1` must still print `GLIBC_2.34`; add any newer symbol to `GLIBC_WRAP` in `build.sh` and to `glibc_compat.c`.
- A portability test on this machine still uses the repo paths unless they're hidden: `unshare -rm sh -c 'mount -t tmpfs none ~/Projects/monolith && /path/to/copy/monolith --shot /tmp/x.png --view 2'`. x86-64 only.

**Windows (`--win`)**, `x86_64-w64-mingw32-gcc`:
- R3D in `$R3D_DIR/build-mingw` with `cmake/mingw-w64-x86_64.cmake` (Release, static). The exe is **fully static** (`-static`): `build/win/` is `monolith.exe` + `maps/`, no DLLs. Imports only system DLLs (`opengl32`, `gdi32`, `user32`, `kernel32`, `shell32`, `winmm`, UCRT `api-ms-win-crt-*`); check with `x86_64-w64-mingw32-objdump -p build/win/monolith.exe | grep "DLL Name"`.
- Console-subsystem exe (bench tables and **P** output visible). Bench reports go to `build/win/benchmark/`. `--win debug` is refused (no sanitizers with MinGW).
- Verify under wine (host GL driver, shots pixel-identical to Linux): `wine build/win/monolith.exe --shot win.png --view 2`. wine's `MESA-EGL: warning` lines on NVIDIA are harmless.

A change is done only when it **builds with no warnings** on both targets, **`tools/leakcheck.sh` reports zero leaks**, and it has been **checked visually** (§8).

**Leak check** (`tools/leakcheck.sh`, after every code change, not map-only edits). Builds `build/linux-lsan/monolith` and `examples/build/linux-lsan/` when a source is newer, then runs every main path in hidden windows or none: the dump modes, shots of the default map (incl. `--shadow-camera --fog-cache`), `tests/models/`, `examples/01_minimal/scene` and `maps/pine_forest`, render scale, `--ao baked` / `--ao-volume` (also across a reload), `--reload-test` both ways and into a broken map, a broken map on the command line, `--shot-hud` with `--select` / `--region`, `--submit` on both maps with the stub terminal, `--export`, `--inspect-model`, `--new-map`, `--import`, `--classify`, every example's `--shot` and the capture shim. Created bundles, exports and packages go to the trash. Logs: `build/leakcheck/latest/logs/`. `tools/lsan_report.py` ignores a leak only when the frame that **allocated** it is in a driver or system library listed in `tools/lsan.supp` (GL drivers, libglvnd, X11/xcb, D-Bus, the loader); anything allocated in the monolith binary (incl. raylib, GLFW, R3D, assimp: all static) counts. On the dev machine the only ignored leak is NVIDIA's (8 allocations, 366 bytes, context creation). `--bench` adds a 10-frame headless bench (opt-in: one benchmark at a time); `--asan` also fails on any sanitizer error. GL objects never deleted are not heap leaks, so `Unload*` calls still need review.

**Memory rules** (details in `docs/design-v2.md` §2.7):
- **No fixed-size content buffers.** Anything sized by content (map objects, lines, selection, regions, prompt, open requests, child processes, bench frames, export file lists) is a heap array (`MS_RESERVE` / `MS_PUSH`, `runtime/monolith_scene.h`, used by `src/` too) or a heap string (`MS_StrDup`). Fixed `char` arrays only for stack scratch formatting one path or message, and single values the loader bounds.
- **Sanity caps instead of limits**, each with a clear message: `MS_MAX_OBJECTS` (1 000 000 boxes + model instances + lights), `MS_MAX_FILE_BYTES` (4 MB map), `MS_PATH_MAX` (1024), 3 `repeat`s per line, a 1 MB prompt, `BENCH_FRAMES_LIMIT`.
- **The per-frame path never allocates** (drawing, picking, HUD, collision): scratch arrays grow to the largest size seen. Allocation happens on (re)load, input events, submit / export / report.
- **Everything is freed** (`MS_Unload`, editor state at exit); the leak check proves it.

## 5. Controls and command line

The header comment at the top of `src/main.c` is the source of truth; keep it updated when adding controls or flags.

**Walk mode (default):** WASD + mouse walk/look, Shift faster, Space jump, F fly (noclip), **1–6** toggle volumetric fog / distance fog / SSAO / SSIL / bloom / sun relative to the map's setting (survive reloads), F1 HUD, F2 graphics settings panel, F12 `screenshot.png`, F9 export to `export/<map>-<stamp>/` (not while the package has a load error), TAB spectator mode, Esc closes the prompt box / leaves snip mode / closes the panel (Esc does not quit; close the window; `SetExitKey(KEY_NULL)` keeps raylib from quitting on it). The bottom-left HUD line shows `map reloaded (N boxes)` or the parse error in red until the file is valid again.

**Spectator mode:** WASD + mouse, E / Q (or Space) up / down (Ctrl is the group-select modifier), wheel base speed 1–400 m/s, Shift ×4 / Alt ×0.25, ← / → fly to the previous / next viewpoint, **P** print and copy the view as `--pos … --look …` (**feet**), G move the player to the camera.

**Map designer (spectator mode, `designer.c`, `launcher.c`):**

| Input | Action |
|---|---|
| Crosshair | White outline + cyan hit marker on the object under it; HUD: group, material, map line + text, index, distance (models: file, transform) |
| LMB / Shift+LMB | Select the object (replace) / add or remove it |
| Ctrl+LMB / Ctrl+Shift+LMB | Select / add the object's whole group |
| RMB | Clear the selection |
| Enter | Prompt box |
| C | Snip mode: camera frozen, cursor free; drag = a region (replaces), Shift+drag = add, Enter = prompt box with regions + selection, RMB / Esc / C = leave. Esc in that prompt box returns to snip mode; a successful submit ends it. |
| R | Fly back to the camera of the most recently finished request |
| K / Shift+K | Close the terminal of the latest / all finished requests |
| Ctrl+K, then n | Close request [n] of the HUD list (1–9, 0 = 10); a running one needs n again within 3 s; Esc cancels |

- **Request list and closing.** Requests are numbered `[n]`; finished ones show 20 s, then a dim line while their terminal is open. On i3 / sway the workspace helper marks each terminal `_monolith_<id>`; a close runs `i3-msg '[con_mark="^_monolith_<id>$"] mark --add …, kill'` (that window only, no focus change). If the process is alive 3 s later or there is no mark: SIGTERM to its process group (started with `setsid`), SIGKILL after 3 s, only while it is the game's unreaped child. kitty `--single-instance` / wezterm mux exit at once, so only the i3 route reaches them. Windows: the entry only leaves the list.
- **Selection** gets an orange tint and outline plus a faint always-on-top outline. It survives reloads: matched by identical geometry (models: world bounds), then by map line + index within the line + group; lost ones are reported.
- **Prompt box:** UTF-8 editing, Ctrl+Backspace word, arrows, Home / End (Ctrl: whole text), Ctrl+V, Shift+Enter newline, **Enter sends**, **Esc cancels**. 1 MB cap, byte counter. Game keys and camera stay frozen while open (+2 frames). Empty prompts can't be sent; on failure the box keeps the text and the HUD shows the error and bundle path.

**Command line.** An unknown flag, a missing or unparsable value, a value that is itself a `--flag`, or a non-finite / out-of-range number (positions, angles, regions: |v| ≤ 1e6) prints a message and exits 1 before any window opens.

| Flag | Meaning |
|---|---|
| `--pos x y z` | Player start (**feet**; default: the map's `spawn`) |
| `--eye x y z` | Start **eye** (= `--pos x y-1.62 z`); with `--view` it moves the spectator camera. Used by bundle reproduction commands. |
| `--look yawDeg pitchDeg` | Start angles; with `--view` they override the viewpoint's |
| `--view n` | Start in spectator mode at viewpoint *n* (1-based) |
| `--off <digits>` | Start with these effect toggles off (`--off 16`) |
| `--map <path>` | Package folder (loads `<folder>/<folder name>.map`, else its only `.map`) or `.map` file instead of `maps/monolith/`; hot-reloaded |
| `--ao full\|ssao\|lite\|off\|baked` | AO trade-off (`atmosphere.c`): `ssao` = SSIL off, `lite` = SSIL off + SSAO 8 samples, `off` = both off, `baked` = both off + the occlusion volume (`--ao-volume 0.5 0.6`) |
| `--ao-volume <voxel> <intensity>` | **Trade-off, off.** World-space occlusion volume (signed distance to boxes, 1 byte/voxel, ≤ 32 MB, ~60 ms CPU bake per load) darkening the ambient light (patch 11). View-independent, ~0.03 ms/frame. Boxes only. |
| `--render-scale <s>` | **Trade-off, default 1.** R3D renders at `s` (0.25–1) × the output size; `--upscale nearest\|linear\|bicubic\|lanczos` (default bicubic) |
| `--window-msaa` | 4× multisampled window (smoother overlay lines, ~0.1 ms); scene image unchanged. Default single-sampled. |
| `--bloom on\|off` | `off` skips bloom and multiplies exposure by the map's `env bloom.offExposure` (default 1.19) so the mean brightness stays |
| `--quality low\|medium\|high\|ultra` | Preset for this run (not saved). Default: the settings file in the interactive game, else ultra (= the map exactly) |
| `--settings <file>` / `--no-settings` | Use this settings file instead of `~/.config/monolith/settings.ini` (`$XDG_CONFIG_HOME`; Windows `%APPDATA%\monolith\`) / read and save none |
| `--settings-set key=value` | Change a setting as in the panel and **save** it (repeatable, applied last; works with `--dump-boxes` without a window). Keys are the file's: `preset=medium`, `render_scale=0.75`, `ao=baked`, `shadow_size=4096`, `fps_cap=monitor`, … |
| `--settings-change key=value` | With `--shot`: apply after 10 frames through the panel's live path; must equal a shot started with `--settings-set` (verified: 0 pixels differ) |
| `--settings-panel` | With `--shot --shot-hud`: draw the open panel |
| `--fx key=value` | One effect override, after `--ao`/`--bloom`: `ssao.samples\|radius\|intensity\|power`, `ssil.samples\|radius\|gi\|ao`, `bloom.levels\|intensity`, `exposure`, `vfog.step\|length`. Range-checked like the `env` key. |
| `--new-map <name\|path>` | Create `maps/<name>/` (or the folder a path with `/` names; its parent must exist): map with ground slab, default material, built-in environment written out, spawn, two viewpoints; `MAP.md` template; empty `shaders/ models/ textures/`, `ASSETS.md`. Checks it loads, no window, refuses to overwrite. |
| `--export <dir>` | No window: `<dir>/<name>/` (every referenced file + `MAP.md`, `ASSETS.md`, sidecars, files model files refer to), `monolith_scene.h/.c`, `viewer.c`, `README.md`. Refuses a non-empty dir; checks the copy loads (§6.3). |
| `--import <project dir> <name\|path>` | Package as `--new-map` + an import bundle `requests/<stamp>_import-<name>/` (`kind: "import"`, heavy tier) and a Claude Code session; no window (`docs/knowledge/import-export.md` §1) |
| `--dump-boxes <file>` | No window: every box as hex floats (`%a`, `min max` per line). Exact-box-list check and full parse check (exit 1 + `file:line: message`); model files only checked for existence |
| `--dump-scene <file>` | Also model files, instances (transform, world bounds), colliders, materials, lights (`src/dumpscene.c`); hidden window when the map has models |
| `--dump-scene-diff <file>` | The same without object indices and map lines (names instead), so a diff shows only changed objects across inserted lines |
| `--inspect-model <file>` | No window: bounds, origin, meshes/triangles, material slots, textures, up axis / unit hints, glTF extensions, warnings, a placement line. Refuses files whose references leave their folder. |
| `--shot <file>` | Render 30 frames into a **hidden window** (never mapped), save, exit (1600×900 unless `--shot-size`). No hot reload or request polling. |
| `--shadow <size> <range>` | Override `env shadow.size` / `sun.range` (default `8192 100`; clamped 256–16384 px, 10–2000 m) |
| `--shadow-camera` / `--shadow-always` | Sun shadow from R3D's camera-fitted map re-rendered when needed / every frame, instead of the static tiles (§7.3). A/B and debugging. |
| `--fog-cache <voxel> <sub>` | **Trade-off, off.** Bake the fog's sun visibility into an 8-bit 3D texture per load (voxel 0.25–8 m, sub³ samples, sub 1–4; patch 8); region from `env fogCache.region` or the content bounds (§7.3); outside it the march uses the shadow map |
| `--no-prepass` | No depth pre-pass (patch 6); same image, slower. A/B tests. |
| `--bench` | Per viewpoint: 60 warmup + 600 measured frames spinning 360°; table + Markdown report in `benchmark/<stamp>_shadow-<size>-<range>.md` (completed runs only) |
| `--headless` | With `--bench`: hidden window, offscreen 1920×1006, `glFinish` per frame. **Required for automated runs.** ~10% slower than visible; compare headless only with headless. |
| `--bench-frames <n>` | Frames per viewpoint (≥ 10, default 600). ~150 for screening; 600 for reported numbers. |
| `--bench-still` | Camera doesn't spin (suffix `_still`) |
| `--bench-window <samples>` | With `--bench --headless`: offscreen stand-in for the window (`windowemu.c`; 4 = `--window-msaa`, 0 = today's) with HUD and overlay, resolved like the swap |
| `--no-profile` | No per-pass GPU table (the profiler is on only under `--bench`) |
| `--note "text"` | Note at the top of the bench report |
| `--no-frame-reuse` / `--frame-reuse` | Turn the interactive frame reuse off (§6) / on where it is off by default (`--shot`, to prove a reused frame equals a fresh one; `--bench-still`, suffix `_reuse`) |

**Designer debug flags** (testing without a visible window):

| Flag | Meaning |
|---|---|
| `--pick` | With `--shot`: print the centre hit (object, group, map line + text, point, normal, distance, bounds; models: transform) |
| `--pick-at x y` | With `--shot`: the same for pixel (x, y), top-left origin (repeatable) |
| `--select <line>` | Select every object of this map line (repeatable) |
| `--region x y w h` | Attach a screen region (repeatable) |
| `--submit "text"` | With `--shot`: run the real submit path (bundle + **real terminal**): always with `MONOLITH_TERMINAL=tools/stub-terminal.sh`, and `gio trash` the bundle afterwards |
| `--submit-wait <s>` / `--close-submitted` | Keep polling the request / then close it like Ctrl+K n and report when the terminal process is gone |
| `--type "text"` | Open the prompt box with this text |
| `--classify "text"` | No window: print the model tier the launcher would pick (context: map, `--select`, `--region`, hit; `--classify-kind edit\|import\|new-map`) |
| `--shot-hud` | Draw HUD, crosshair, overlay, regions, prompt box into the shot |
| `--shot-size w h` | Shot size (64–8192) |
| `--reload-test <path>` | With `--shot`: load `<path>` through the hot-reload path after 10 frames; must equal a plain `--shot --map <path>` (proves every static cache is rebuilt) |

In `--submit`, `--type` and `--classify` text a literal `\n` becomes a new line.

**Environment variables (launcher)**

| Variable | Meaning |
|---|---|
| `MONOLITH_TERMINAL` | Terminal program + optional args (`"kitty --single-instance"`); over `TERMINAL`; else the first of `alacritty`, `kitty`, `foot`, `wezterm`, `xterm` in `PATH` |
| `MONOLITH_CLAUDE_ARGS` | Extra Claude Code flags (whitespace-split), after the two permission flags and `--model <name>`; a `--model` here replaces the chosen one |
| `MONOLITH_CLAUDE_SAFE` | Non-empty and not `0`: drop the permission flags |
| `MONOLITH_MODEL` | One model for every request (turns the tier choice off) |
| `MONOLITH_MODEL_FAST` / `_DEFAULT` / `_HEAVY` | Model per tier (`haiku`, `sonnet`, `opus`). `ClassifyRequest` ("Model choice" in `launcher.c`): **heavy** for imports / new maps, an empty map or no `MAP.md`, asset, look/engine or large design words, > 280 bytes, 3+ sentences or > 2 regions; **fast** for one short sentence (≤ 120 bytes) with a local edit word, no creation word and a target; **default** otherwise. Written to `request.md` / `request.json`. |
| `@fast` / `@default` / `@heavy` (also `@haiku` / `@sonnet` / `@opus`) | Prompt prefix picking the tier, stripped from the bundle (`model.override: true`); `MONOLITH_MODEL` and a `--model` in the args still win |
| `MONOLITH_WORKSPACE` | i3 / sway workspace for the terminal (default `10`, empty = stay). `i3-msg` / `swaymsg` on `[title="^monolith-claude: <slug> <HHMMSS>$"]`, retried by a detached helper every 0.1 s for 10 s; also sets the `_monolith_<id>` mark. An i3 `for_window [title="^monolith-claude: "] …` rule avoids the brief flash. |
| `MONOLITH_CLOSE_ON_EXIT` | `1`: close finished requests' terminals when the game quits |
| `MONOLITH_CLOSE_METHOD` | `wm` or `signal` only (tests; default both, WM first) |

**Other variables:** `MONOLITH_CAPTURE` / `MONOLITH_CAPTURE_FRAME` (capture shim output folder / frame, default 3), `MONOLITH_DOWNLOADS` (`fetch_asset.py`, default `~/.cache/monolith/downloads`), `POLY_PIZZA_API_KEY` (or `~/.config/monolith/poly-pizza.key`; never printed), `MONOLITH_STUB_LOG` (stub terminal's argv log), `__GL_SYNC_TO_VBLANK=0` / `vblank_mode=0` (driver vsync off for benches).

### Graphics settings (F2, `settings.c`, `settingspanel.c`, runtime `MS_Quality`)

**F2** opens a panel: preset selector, Display / Shadows / Ambient occlusion / Fog / Post / Performance rows, a frame time readout (fps, CPU ms, GPU ms; the profiler runs while it is open), Reset to preset, Close. UP/DOWN/LEFT/RIGHT or the mouse; values that differ from the preset are blue. Heavy changes (shadow map size, AO mode, fog cache, shadow mode/range, preset) apply one frame later behind "applying …"; a new shadow map size re-initialises R3D and reloads the map (~1 s, selection kept); window MSAA at the next start. ESC / F2 close and save.

**The map stays the source of the look.** Every scene setting is relative to the map's environment: ultra renders exactly the map (pixel-identical, the default), an effect the map turns off stays off. Toggles 1–6 and `--fx` apply on top. Everything that changes the image is in `FrameKey` (the whole `GfxSettings` + R3D's AA mode).

Settings (file keys): Display — `render_scale` 0.5–1, `upscale`, `anti_aliasing` none/fxaa/smaa, `window_msaa`, exposure / brightness / contrast / saturation trims. Shadows — `sun_shadow`, `shadow_size` (map's / 2048–16384), `shadow_range` (map's / 60–300 m), `shadow_softness`, `shadow_mode` tiles/camera/always. Below the map's `shadow.size`, the sun's depth and slope biases are scaled by the texel ratio (`SunShadowMap()`) so low suns get no acne. A range below the fog length fades the shafts early. AO — `ao` full/ssao/lite/baked/off, baked voxel/intensity, SSAO / SSIL parameters. Fog — volumetric fog, `fog_step` (× the map's step), length, fog cache voxel/samples, distance fog. Post — bloom, bloom intensity, SSR, SSGI (off / map's / on). Performance — frame reuse, fps cap (off/monitor/30–240), vsync, depth pre-pass. Costs: `docs/performance.md`.

**Presets** (`MS_QualityPreset`): **ultra** = the map exactly; **high** = AO baked (0.5 m, 0.6) + fog cache 0.5 m; **medium** = high + render scale 0.75, fog step ×2, shadow map 4096; **low** = render scale 0.5, AO off, bloom off, fog cache 0.5 m + step ×2, shadow map 4096. (Default map, headless: 3.17 / 2.07 / 1.32 / 0.77 ms.)

**The settings file** is `key = value` (`preset = …` sets every scene setting, later keys override). Only the interactive game reads the default file; **`--shot`, `--bench`, `--dump-*`, `--export` and `--classify` never do** unless `--settings` names one, so verification stays deterministic. Command-line flags override for one run and are never saved (panel changes are written field by field). Unknown keys and bad values are logged and ignored.

**In your own program:** `MS_QualityPreset(level)`, `MS_ApplyQuality(scene, &q)` after every `MS_ApplyEnvironment`, then `MS_ApplyRenderResolution`; `q.shadowSize` goes to `MS_LoadOptions.shadowSizeOverride`. `examples/01_minimal --quality medium` renders the same pixels as `monolith --quality medium`.

Vsync is **off** by default and rendered frames uncapped (settings change that in the interactive game only); reused frames sleep to the monitor refresh rate.

## 6. Code architecture (`runtime/`, `src/*.c`)

The scene (loading, drawing, collision, picking, hot reload) is the **scene runtime** (§6.1). The editor in `src/` is split into modules with a header each; `build.sh` compiles `src/*.c` and `runtime/*.c`, so a new module needs no build change. Larger files use `// #region` blocks.

1. **Scene runtime**: the game holds one scene in the global `MS_Scene *scene` (`common.h`).
2. **Player** (`player.c`): feet-based AABB (r 0.3, h 1.75, eye 1.62), per-axis movement against `scene->colliders` (boxes, then model colliders) with the strict **`BoxesOverlap`** (like `MS_Collides`, §7.4); step-up (`STEP_HEIGHT` 0.45), gravity, jump, fly, smoothing, head bob. Starts at `spawn` unless `--pos` / `--eye` / `--look`.
3. **Spectator** (`spectator.c`): state, the map's viewpoints (`scene->viewpoints`), `UpdateSpectator()`, `SpectatorFlyTo()` / `FlyToPose()` / `JumpTo()`, `SpectatorCamera()`. `LookDir(yaw, pitch) = (cos p · sin y, sin p, cos p · cos y)` (yaw 0 → +Z); right = `(−cos y, 0, sin y)`.
4. **Designer** (`designer.c`), one `Designer` state owned by `main()`:
   - **Picking**: `DesignerPick()` → `MS_Raycast()`: slab test against every box, then models (bounds, then triangles through a BVH). Boxes containing the eye are skipped. Refreshed every frame in spectator mode and `--shot` runs.
   - **Selection**: `MS_ObjRef` heap arrays with an index → position table; `DesignerSync()` remaps after a reload through a hash of exact bounds.
   - **Screen regions**: at submit `RegionPick` samples each with up to 4096 rays (per-region candidate lists, culled per 8×8-ray tile): objects with ray shares (also per map line), centre hit, depth range, sky share, and **edge objects** (`AnalyzeRegionEdge`: up to 64 rays per side on a ring just outside the rect, nearest first, with sides).
   - **Overlay**: `DesignerDrawOverlay()` draws with raylib after `R3D_End()` in `BeginMode3D`, depth-tested against R3D's depth (copied into the target by `R3D_End`) with a 4-step depth bias in the projection and slightly inflated boxes (models: oriented bounds). Also crosshair, snip drawing, HUD.
   - **Prompt box**: UTF-8, word wrap, scrolling, heap text; `DesignerCapturesInput()` freezes game keys and camera.
   - `DesignerBuildRequest()` snapshots prompt, eye, angles, fovy, size, hit, selection, regions, map version into a `DesignRequest`.
5. **Launcher** (`launcher.c`, see its header comment):
   - `LauncherSubmit()` writes `requests/<YYYY-MM-DD_HHMMSS>_<slug>/` (`view.png`, `view-marked.png`, `region-<n>.png`, `request.md`, `request.json`, format 2: `AI-MAP-TOOL.md` §2), then starts the terminal with `fork` + `execve` from an argv array, no shell: the user's text never reaches a command line. The child `setsid`s, `chdir`s to the repo root, stdin `/dev/null`; exec failures return through a close-on-exec pipe; children are reaped with `waitpid(WNOHANG)`.
   - Terminal syntax (title, working dir, exec flag) from `TERMINALS[]`; unknown terminals get `-e`. Title `monolith-claude: <slug> <HHMMSS>[-n]`, unique per request. `claude` is looked up in `PATH`, then `~/.local/bin`, passed absolute, with the permission flags (unless `MONOLITH_CLAUDE_SAFE`), `--model <tier's model>` and `MONOLITH_CLAUDE_ARGS` (a bare `--model` there is dropped with a warning). A detached helper moves and marks the window. On launch failure the bundle stays, the HUD names it and the log prints the command.
   - `LauncherUpdate()` polls open requests for `result.md` (HUD 20 s); `LauncherLastDone()` gives the camera for **R**.
   - **Windows**: `cmd /c start "<title>" /D <exe dir> cmd /k "<claude>" … "<prompt>"` via `_spawnvp`; compile-tested only.
6. **Atmosphere** (`atmosphere.c`): the environment is map data (`env` lines; defaults in `BuiltinEnvironment()`, applied by `MS_ApplyEnvironment`). Graphics settings (`MS_ApplyQuality`, incl. `--ao` / `--bloom`) go on top, then `--fx`; `main.c`'s `ApplyEnvironment()` re-applies environment → quality → `--fx` → toggle flips → `MS_ApplyRenderResolution` after every reload.
7. **Graphics settings** (`settings.c`, `settingspanel.c`): `GfxSettings` = `MS_Quality` + window options (MSAA, vsync, fps cap, frame reuse).
8. **New map, export, import, inspection, dumps** (`newmap.c`, `export.c`, `import.c`, `inspect.c`, `dumpscene.c`): §6.3, §5.
9. **Benchmark** (`bench.c`, `windowemu.c`): statistics, the Markdown report, the `--bench-window` stand-in.
10. **Main** (`main.c`):
    - header comment, argument parsing, `InitDataPaths()`, the map's `noGpu` parse check **before any window**, and the windowless modes;
    - shadow-size hint (`MS_SetInitHints`), `R3D_Init`, `MS_InitRenderer()` (`GL_DEPTH_CLAMP`, §7.2), `MS_Load`, `MS_SetRenderSettings` (shadow mode, fog cache, pre-pass, render scale, upscale, AO volume), `MS_ApplyRenderResolution` (also on resize);
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
    MS_DrawGeometry(scene);    // R3D_DrawMeshInstanced per material with boxes, R3D_DrawModelInstanced
                               // per (model file, slot materials, shadow flag); fog / AO bakes once per load
R3D_End();
// then on the same target: DesignerDrawOverlay (depth-tested), crosshair, HUD, regions, prompt box, settings panel
```

### 6.1 Scene runtime (`runtime/monolith_scene.h` + `runtime/monolith_scene.c`)

A separately compilable C11 module depending only on raylib (raymath, rlgl) and R3D. `monolith_scene.h` is the API contract (read its comments); the header comment of `monolith_scene.c` is the map format reference.

**Two builds of one source.** monolith, the examples and the capture shim compile it with `-DMONOLITH_EDITOR`: hot reload, source lines, `brief` / `hash` / `mtime`, the dependency list, the environment writers, `MS_CheckModelReferences`, `MS_EnvKeyRange` and the `sunOn` toggle live in `#ifdef MONOLITH_EDITOR` blocks. `--export` writes the file without those blocks (`StripEditor` in `src/export.c`), so a game gets only loading, drawing, quality presets and queries. Rules for runtime changes: tool-only code goes into such a block (never nested; `#ifndef MONOLITH_EDITOR … #endif` for game-only code, `#else` allowed); comments outside the blocks must not mention the editor, requests, Claude Code, hot reload or monolith's command-line flags, because they ship; the runtime must build with zero warnings with and without `MONOLITH_EDITOR` (and without `MS_PATCHED_R3D`).

**What it owns** (all freed by `MS_Unload`):
- the parsed package: boxes (`scene->boxes`, `boxRefs[]`: source line, statement index, group, material), `modelFiles[]`, `models[]` (transform, world bounds, collide mode, slot overrides), colliders (boxes first), materials (`[0]` = `"default"`), groups, lights, sun, spawn, viewpoints, bounds, path, package root, a **`version`** that goes up on every successful (re)load (static caches key on it); editor builds also the map text (`MS_GetLine` / `MS_LineText`), `brief`, FNV-1a hash and mtime;
- GPU: one surface shader instance per material (uniforms are global per shader, §7.4), textures (`wrap repeat`, trilinear + mipmaps), one instance buffer per material with boxes (a unit cube scaled per instance), one instanced draw per (model file, slot materials, shadow flag), the visible sky and the ambient map (§7.5);
- the sun shadow (`MS_SHADOW_TILES`, or `MS_SHADOW_CAMERA` / `MS_SHADOW_ALWAYS`), extra lights' shadow maps, the fog cache and AO volume, all placed by the map's bounds (§7.3);
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
- `MS_Poll` checks every dependency's mtime about twice a second; after a change it keeps re-reading for 2 s (1 s mtime resolution, editors writing in steps) and reloads when the combined content hash differs from the last attempt. The watch list is the last *attempt*'s, so a missing file is picked up when it appears. A reload creates every new GPU resource **before** freeing the old ones (§7.4), and all static caches re-key on `scene->version`. `shadow.size` only changes with a restart.

**Patched vs. upstream R3D (`MS_PATCHED_R3D`).** With `-DMS_PATCHED_R3D` (set by `build.sh` and `examples/build.sh`) the runtime uses the patched APIs (`R3D_PushLightFixedShadow`, `R3D_SetDepthPrepass`, the fog bake, the occlusion volume, the profiler); without it, it falls back to `R3D_PushLightEx` with the camera-fitted map (`MS_SHADOW_TILES` → `MS_SHADOW_CAMERA`), no fog cache or AO volume. The export README lists the image differences (patch 4 visible when turning, 7/9 rounding level, patch 1 needed for volumetric fog on AMD / Intel). The upstream build is compile-checked only.

### 6.2 Surface shaders in map packages

A material's `shader "shaders/x.glsl"` is an R3D **surface shader** (`#pragma usage opaque shadow`, `void fragment()`), loaded per material from the package and hot-reloaded; `uniform` / `sampler` keys set its uniforms. Built-ins: `submodules/r3d/docs/shaders/surface_shader.md` (`ALBEDO`, `ROUGHNESS`, `METALNESS`, `SPECULAR`, `EMISSION`, `NORMAL_MAP`, `POSITION`, `NORMAL`, …). Boxes have per-face 0–1 UVs, so a pattern that should look the same on every box size uses world-space `POSITION` / `NORMAL` (like `concrete.glsl`). Opaque shaders that never discard want `alpha_cutoff 0` (pre-pass). Check every shader with `glslangValidator` (§7.4).

### 6.3 Export, import, examples

- **Export** (`src/export.c`, `--export <dir> [--map …]`, F9 = the open map): a self-contained folder that builds with one command. Copies the runtime's dependency list plus `MAP.md`, `ASSETS.md`, sidecars, licence files and the files model files refer to (`.gltf` URIs, `.obj` → `.mtl` → maps); the map byte for byte as `<name>.map`. Adds the runtime without its `MONOLITH_EDITOR` blocks, `viewer.c` and `build.sh` from `runtime/export/` (`@MAP@` = the map's name), the R3D tree build.sh built against (`R3D_SOURCE_PATH`; only `R3D_KEEP` minus `R3D_SKIP`, no dot files: ~33 MB, raylib and assimp included), the static libraries of `submodules/r3d/build-linux` and `build-mingw` as `prebuilt/<linux|win>/lib/` when their last build is current (`AddPrebuilt`; the package's `build.sh` links them and falls back to a source build), `cmake/mingw-w64-x86_64.cmake`, `LICENSES/` and a README (bundled build first, own R3D as the alternative, patched vs. upstream, quality presets, depth clamp, licences + the `ASSETS.md` table). Checks every source first (nothing is written if one is missing), writes `<dir>.partial`, loads the copy, renames it. Needs the repo. Changing `runtime/export/viewer.c` / `build.sh` or what R3D's build reads: export both test maps, build them from a copy outside the repo and compare viewpoint shots with `monolith --shot --view n` (docs/knowledge/import-export.md §2).
- **Import** (`src/import.c`): `NewMap()`, a bundle written like the launcher's, `LauncherStartBundle()` (model from `LauncherChooseModel`, `kind = "import"`). The session follows `docs/knowledge/import-export.md` §1.
- **Capture shim** (`runtime/monolith_capture.h`, `-include`d into a program): routes `R3D_LoadModel`, `R3D_GenMeshCube`, `R3D_GenProceduralSky`, `R3D_LoadCubemap`, `R3D_LoadSurfaceShader`, `R3D_Begin*`, `R3D_PushLight*`, `R3D_DrawMesh*`, `R3D_DrawModel*`, `R3D_End` through recorders; with `MONOLITH_CAPTURE=<dir>` writes a package from one frame plus `CAPTURE.md` (what it couldn't translate). Limits: only generated cubes and loaded models are read back, rotated cubes become AABBs, uniforms and material changes on loaded models are only reported. A cross-check, not a replacement for reading the source.
- **Examples** (`examples/build.sh [--win]` → `examples/build/<linux|win>/<example>/`): `01_minimal`, `02_existing_project` (`before.c` / `after.c`), `03_live_editing` (`MS_Poll`, `--reload`), `04_import_source` (import test input; also `import_source_capture`). Same flags and static linking as the game, `-DMS_PATCHED_R3D -DMONOLITH_EDITOR`; each takes `--shot <file>` (hidden window `monolith-shot`, `examples/common/shot.h`).

## 7. R3D pitfalls already discovered (read before touching lighting)

Engine facts found by debugging; they hold for every map (numbers are usually the default map's; its look is explained in `maps/monolith/MAP.md`). Measurements behind them: `docs/performance.md`.

### 7.1 Volumetric fog turns into a black curtain if dense
R3D's volumetric fog is **lit only by lights, not by the sky/IBL**: fog in shadow absorbs without adding light, so dense fog turns buildings 30–60 m away **pitch black depending on view direction**. Every map needs: low `volumetricFog.scatteringDensity` (both maps 0.012), low absorption (0.001), a small **`volumetricFog.emissionEnergy`** (0.12 / 0.25, coloured like the distance fog) standing in for sky light. Raise shafts with `sun.fogEnergy`, not density. To keep distant buildings clear, fade the fog by distance (`volumetricFog.fadeStart` / `fadeEnd`, patch 13) instead of thinning it everywhere. With a fade, the sun shadow of each fog step is also faded by that step's own distance (patch 14) instead of the surface's, so there's no hard line in the haze at 75% of `sun.range` (costs ~0.35 ms: far and sky rays now take shadow lookups). A sunlit eye makes the whole view brighter (the fog around it scatters toward a camera looking into the sun): `volumetricFog.fadeNear` fades the density in from the eye (patch 15) so the look doesn't depend on whether the player stands in sun or shade.

### 7.2 Directional shadow map is camera-fitted; casters get clipped
R3D fits the sun's ortho map to a sphere around the camera frustum out to `range`; its near plane reaches only ~`radius + 100 m` toward the sun, so tall casters were clipped depending on view and their shadows popped. Fix: **shadow pancaking**, `glEnable(GL_DEPTH_CLAMP)` once after `R3D_Init` in `MS_InitRenderer()` (every program calls it). Side effect: geometry closer than the 1 cm near plane is clamped instead of clipped.

**Do not "fix" shadow problems by disabling shadow casting.** Fix the cause.

### 7.3 Sun shadow: resolution, caches, cost
- **Texel size** ≈ `2 · radius / size`, `radius ≈ 1.5 · range` (70° fov, 16:9): 8192 px / 100 m (the default, `env shadow.size`, `sun.range`, `--shadow`) ≈ 3.7 cm texels, ~256 MB VRAM (16-bit × 2 layers). 6144/100 is the fallback for weaker GPUs; cost tracks map size, not range.
- **Fade:** between 75% and 100% of `range` by **distance from the eye** (patch 4), and over the outer 1% of the map (patch 7).
- **`range` ≥ `volumetricFog.length`** (90 m), or shafts fade early.
- **No cascaded shadow maps** in R3D for directional lights: the real long-term fix (proposals in `docs/performance.md`).
- **The volumetric fog march is the expensive part** (half resolution, up to `length / stepSize` steps per pixel, 8-tap PCF each). Patches 2/3/9 skip or cheapen those lookups bit-identically or at rounding level; patch 8 (`--fog-cache`) bakes them (a look trade-off). A larger `stepSize` or one tap per step is cheaper but changes the image (not applied; decide by eye).

**Static sun shadow tiles (default, patch 7).** Sun and boxes are static, so the app supplies the light matrix (`R3D_PushLightFixedShadow`): `SunShadowTileSelect()` lays world-fixed orthographic tiles on a light-space grid, each with exactly the radius, depth window and texel lattice of R3D's fit, so pancaking, biases and PCF are unchanged. A tile is chosen **by eye position only** (turning never re-renders) and used only while everything within `range` lies in its fade-free area. Two tiles live in R3D's two directional layers; in the play area each is drawn once per map load. **If R3D's fit (`light_dir_view_proj`), its zExtension or `SHADOW_DIR_EDGE_FADE` change, update the `TILE_*` / `SHADOW_EDGE_FADE` constants in `runtime/monolith_scene.c`.**

**Camera-fitted reuse (`--shadow-camera`).** `R3D_PushLightEx(sun, sunShadow, update)`: R3D stores the light matrix with the cached map and every fit lays the same world-space texel grid, so a reused map is consistent. `SunShadowNeedsUpdate()` replicates R3D's fit on the CPU and keeps the map only while its fades are within 1/255 of a fresh fit's over a frustum sample grid. **If R3D's fit or fade code changes, update `SunShadowFit()` / `ShadowEdgeFade()` / the `distFade` in `SunShadowNeedsUpdate()` to match.**

**Caches follow the map** (`runtime/monolith_scene.c`, region "Caches follow the map"): shadow tiles, fog cache and AO volume are placed from the loaded map:
- a box is *ground-like* when its smaller horizontal extent is ≥ 100 m and its height ≤ 5% of that; the *content bounds* are all other boxes plus every model's world bounds (everything, if only ground-like boxes exist);
- **shadow tile home** = centre of the viewpoint eyes' AABB (else the spawn, else the content bounds); `env shadow.home` overrides;
- **fog cache region** = content bounds + 2 m, shrunk around the play centre to ≤ 64 M voxels; `env fogCache.region` overrides. The **AO volume** uses the same derivation with 32 M voxels (boxes only). Both are cut to `GL_MAX_3D_TEXTURE_SIZE` per axis (logged);
- `maps/monolith/monolith.map` carries both hints (`env shadow.home 10 38 2`, `env fogCache.region -70 -1 -150 75 100 105`) to stay pixel-identical with its pre-package layout. New maps leave them out.

### 7.4 Other gotchas
- **`CheckCollisionBoxes` is inclusive** (touching = hit, so a player on the ground couldn't move). Use the strict `BoxesOverlap()`.
- **Brightness:** with AgX, sunlit mid-grey albedos blow out to white quickly (default map: sun 2.0, exposure 0.9). If a map looks washed out, lower `sun.energy` or `tonemap.exposure`; don't brighten the fog colour.
- **Instance attributes:** box instances hold only `POSITION | SCALE` (no rotation; collision, picking and world-position shaders rely on it). Model instances add `ROTATION`; R3D doesn't inverse-scale their normals, so per-axis scaled models light approximately. R3D doesn't frustum-cull instanced draws.
- **Surface-shader uniforms** are uploaded at `R3D_End()`: the last value set in a frame wins; they can't vary per draw.
- **Hidden-window screenshots:** in `--shot` the window is never mapped, so `TakeScreenshot` would read undefined pixels. Shots render with `R3D_BeginPro` into a `RenderTexture`, read back with `LoadImageFromTexture`, flipped. Don't switch back.
- **NVIDIA accepts GLSL that AMD/Intel reject** (and wine uses the host driver). Found so far on an AMD RX 9060 XT: a function named `noise3` (clashes with the deprecated built-in; now `valueNoise`) and a uniform-block member named `length` in R3D (patched). Avoid built-in names (`noise1`–`noise4`, `length`, `distance`, …) and check shaders with **`glslangValidator`**; wrap a surface shader in a stub that declares its built-ins and calls `fragment()` from `main()`.
- **Instance buffers on reload:** R3D caches the GL buffer names it last bound for instancing (`instances_enable`, `src/modules/r3d_render.c`). Freeing the old buffer first let GL reuse the names, so a reloaded map drew the **old** boxes. The runtime creates every new buffer first and frees the old ones after (proper fix in R3D, not patched).
- **Unshadowed point / spot lights shine through walls and floors** (their range is a sphere). Shadow maps for many interior lights are too big (2048 px cubes); give such lights a `clip` box instead (patch 16: R3D_Light `clip` / `clipMin` / `clipMax`, tested in deferred lighting, the forward pass and the fog march; `docs/knowledge/map-format.md` §2.6).
- **Ambient colour vs. ambient map:** `ambient.map = {0}` with a plain `ambient.color` gave no ambient light. Keep the IBL map from a procedural sky (§7.5).

### 7.5 Shadowed undersides: the ambient sky's ground is the fill light
Downward-facing surfaces (soffits, beam undersides, the hall ceiling) rendered **near black**. **Cause:** the IBL's lower hemisphere: the ambient map came from the visible sky, whose ground (`groundBottomColor` ≈ 0.03 linear, with R3D's `groundHorizonCurve` 0.02 covering the whole lower hemisphere) gives a down-facing normal almost no ambient; the near-zero base also turned warm fog in-scatter on them maroon. **Not the cause:** SSAO and SSIL (each < 0.5/255 mean; SSIL only bounces light from on-screen pixels).

**Fix (in place):** the ambient map comes from a second, **ambient-only** procedural sky (`ambientSky.mode procedural`, `ambientSky.size 256`, unloaded after `R3D_GenAmbientMap`): a copy of the visible sky with the `ambientSky.*` keys changed. Defaults (and the default map): `ambientSky.groundBottomColor 120 116 108` (≈ 0.19 linear, slightly warm), `ambientSky.groundHorizonCurve 0.1`; the pine forest uses a brighter, bluer copy. Visible sky, distance fog, sun and frame cost unchanged; shadowed faces get warm bounce from below and cool light from above.
- Tune with `ambientSky.groundBottomColor` (under-lighting) and `ambientSky.groundHorizonCurve` (how much of `groundHorizonColor` reaches near-vertical faces). Keep the visible `sky.groundBottomColor` dark if high views show it. `ambientSky.mode same` suits panorama skies.
- **Don't add a fill light for this:** a shadowless "bounce" directional light would also light the volumetric fog unless `fogEnergy = 0` (§7.1).
- Side effect (physically right): the ambient reflection uses the brighter ground too, so sideways wet patches pick some of it up.

### 7.6 Effects must not change when the camera only turns (R3D patch 4)
Rotating the camera from a fixed eye used to change how a world spot looked (fog glowing in the screen centre, distance fog thinning toward the edges): four terms used **view depth** where they meant distance from the eye — the fog shadow fade and patch 2's skip (`vfog_radiance.frag`), fog transmittance (`vfog_transmittance.frag`), distance fog (`fog.frag`) and the sun-shadow fade (`lighting.frag`). Patch 4 switched all four to distance from the eye; `runtime/monolith_scene.c` replicates the fade. Details: `submodules/r3d/patches.md`.
- **Rule:** any fade, fog or cache term must be a function of world position (and the view ray), never of view depth; static caches would otherwise bake a view-dependent error in.
- Still view-dependent: the screen-space passes (SSAO, SSIL, bloom, FXAA, fog jitter + upsample) at reprojection-noise level; `scene/forward.frag` and `scene/unlit.frag` (materials with `transparency alpha` or `unlit on`); the camera-fitted shadow map's re-render cost (not its image).

## 8. How to verify changes

No test suite; verification is **visual and numeric**:

1. **Build cleanly:** `./build.sh` and `./build.sh --win`, no warnings; `examples/build.sh` (and `--win`) when the runtime or an example changed. Shaders pass `glslangValidator`.
2. **No leaks:** `tools/leakcheck.sh` ends with `leakcheck: OK: zero leaks of ours`. After larger changes also `--asan`.
3. **The default map is unchanged** by engine work: identical `--dump-boxes` and pixel-identical `--shot`s (the 7 viewpoints, yaw sweeps 0/60/…/300 at eye heights 1.6, 60 and 100, the steep view `--pos -52.22 61.62 39.53 --look 140.2 -39.6`, a `--reload-test`) against a build from before the change. Same for `maps/pine_forest` and `tests/models` when models, materials, lights or environment code changed.
4. **Fixed views:**
   ```bash
   ./build/linux/monolith --shot a.png --view 2                          # viewpoint 2
   ./build/linux/monolith --shot b.png --pos 0 0 -5 --look 120 5         # exact view
   ./build/linux/monolith --shot c.png --view 2 --off 16                 # effects off
   ./build/linux/monolith --shot d.png --view 3 --map maps/pine_forest   # another package
   ```
   For lighting or shadows, take a **yaw sweep from one position**: the black-curtain and shadow-popping bugs only showed when rotating.
5. **Isolate effects** with `--off` (1 volumetric fog, 2 fog, 3 SSAO, 4 SSIL, 5 bloom, 6 sun).
6. **Performance:** `__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless --note "what changed"`. Verify every claimed gain or regression with interleaved A/B pairs (≥ 3, two binaries, same flags); screen with `--bench-frames 150`, report full runs; one benchmark at a time. **Automated runs: always `--headless`.** Without it `--bench` opens a visible window that must actually be shown: on an undisplayed i3 workspace it stalls at 100% CPU forever.
7. **Window titles:** automated runs are titled `monolith-bench` / `monolith-shot` (also the examples), normal play `monolith`; the developer's i3 config moves the first two to workspace 10 (`for_window [title="^monolith-(bench|shot)$"] …`). Keep these titles. Request terminals: `monolith-claude: <slug> <HHMMSS>`.
8. **Movement** (collision, controls) can only be tested by playing.
9. **Map loader and edits:** diff `--dump-boxes` / `--dump-scene` / `--dump-scene-diff` before and after (identical for the default map after a loader change). A broken map prints `file:line: message` and exits 1 before any window; the running game keeps the old map. Hot reload runs only interactively; `--reload-test` exercises it in a shot.
10. **Designer and launcher without a window:**
    - `--shot x.png --view 2 --pick` on the default map must hit box 46, `hypostyle hall: pillars`, map line 193, material `concrete`, distance ≈ 42.07 m.
    - `--shot-hud` with `--select` / `--region` / `--type` / `--settings-panel` puts the overlay into the shot.
    - `--submit` **always** with `MONOLITH_TERMINAL=tools/stub-terminal.sh` (logs to `$MONOLITH_STUB_LOG`) and `MONOLITH_WORKSPACE=`; `gio trash requests/<id>` afterwards. `--classify` checks the model choice without a bundle.
    - Mouse and keyboard input can only be tested by hand.
11. **Export and examples:** export into a scratch folder, build its `viewer.c` outside the repo (the README's compiler line), compare its `--shot` with `monolith --shot` (pixel-identical for the shipped packages).

## 9. Common modifications

**Edit or start a level.** Edit the package following its `MAP.md` (format: `docs/knowledge/map-format.md`; rules: `AI-MAP-TOOL.md` §5); it hot-reloads, no rebuild. New level: `--new-map <name>`, fill in `MAP.md`, build (or submit a first request: the session writes the brief first), or `--import` a raylib + R3D project. Models: `docs/knowledge/models-and-textures.md`.

**Add a map primitive or statement.** A builder next to `Box`/`Block`/`Stairs`/`WaffleFacade` in `runtime/monolith_scene.c` plus a `PRIMS[]` entry (name, argument count, per argument its shift axis `AX_X/Y/Z`, `AX_NONE` for sizes, `AX_INT` for counts), and its call where `ParseStatement()` dispatches. That code switches on the argument count (box and block, both 6, differ by `PRIMS[]` entry), so a new 6–8-argument primitive means switching on the entry. Builders return false when the map is full (`MS_MAX_OBJECTS`) or out of memory, and loops add a box per iteration so the cap bounds them. New statements need `monolith 2` (bump `MS_FORMAT_VERSION` only for changes older runtimes would misread). Document it in the `monolith_scene.c` header comment and `docs/knowledge/map-format.md` §2.

**Add an environment key.** Key tables in `monolith_scene.c` (region "Key tables": `ENV_KEYS` for `R3D_Environment` fields, sky / sun tables otherwise, a range in `ENV_RANGES`), its built-in default in `BuiltinEnvironment()` equal to today's value, written by `MS_WriteEnvironment`. Image changes without a reload must reach `FrameKey`.

**Change a request bundle.** `launcher.c` (`BuildBundle`, `WriteRequestMd`, `WriteRequestJson`; bump `"format"` when keys change) and `AI-MAP-TOOL.md` §2 (keep older formats described). New terminal: `TERMINALS[]`.

**Add a viewpoint.** Spectator mode, **P** (prints *feet*), then `viewpoint "Name" ex ey ez tx ty tz` with the *eye*. `--bench` covers it automatically.

**Tune a map's look / shader.** Its `env` lines, materials and `shaders/` (hot-reloaded); respect §7.1–7.5 and the map's `MAP.md`; check shaders with `glslangValidator`. Changing the default map's look needs the developer's approval.

**Add a graphics setting.** A field in `MS_Quality` (preset values, `MS_QualityEqual`, `MS_ApplyQuality`), the file key and panel row in `settings.c` / `settingspanel.c`; ultra stays the map exactly.

**Add an effect toggle.** The `TOGGLE(KEY_x, 'x')` block in the main loop, the HUD string, the header comment.

**Add a CLI flag.** Parse it at the top of `main()`; document it in the header comment and §5.

## 10. Conventions

- **English** for code, identifiers, comments and names.
- **Match the style:** 4-space indent, `static` functions, PascalCase functions, `UPPER_CASE` macros, `// #region` sections, short comments explaining *why*.
- **One module per concern:** each `src/<module>.c` has a `.h` interface and a header comment; file-local functions `static`; minimal shared state (`scene`, data paths).
- **The runtime stays portable:** `runtime/` depends only on raylib and R3D, compiles without `MS_PATCHED_R3D`, knows nothing about the editor. Editor-only code stays in `src/`.
- **The level is data:** geometry, materials, lights, environment live in `maps/<name>/`; C describes primitives and statements, not buildings or a look. Nothing outside a package assumes a style.
- **Assets are recorded** (`ASSETS.md` row + sidecar; CC0 / PD / CC-BY with attribution).
- **Memory:** heap arrays for content, no per-frame allocation, everything freed (§4).
- **Comment non-obvious engine workarounds** with their reason (as for §7.1, §7.2), so nobody "cleans them up".
- **Don't commit `build/`, screenshots, `benchmark/`, `requests/` or `export/`.**

## 11. Known limitations and next steps

- **Map format:** boxes are axis-aligned with per-face 0–1 UVs (textures stretch); only models rotate; models are static (no animation or skins); instances are rebuilt on every reload; code changes and `shadow.size` need a restart (or the panel's renderer restart).
- **Models:** picking hits triangles, collision uses AABBs; `noGpu` loads don't read model files (bounds and `origin` shifts final only in GPU loads); FBX / DAE references aren't followed for hot reload or export; the AO volume ignores models.
- **Interactive input** (clicks, drags, typing, paste, Esc, the panel, camera freezing) is tested only by hand.
- **Windows launcher** is compile-tested only; `start` failures go undetected; workspace moves and closing are i3 / sway only. On Windows and in copied build folders, `requests/` and Claude Code's working directory are the executable's folder (no sources, no `CLAUDE.md`).
- **Request flow:** a failed launch keeps the bundle and the text; Enter again writes a second bundle (trash the stale one). A terminal process exiting early (kitty `--single-instance`, wezterm mux) is reported, not a failure; closing then needs the i3 / sway mark.
- **Upstream R3D** builds are compile-checked only.
- **Ideas:** decals (`r3d_decal.h`); height-dependent fog for hazy outdoor views; shadow cascades and cheaper fog lookups (`docs/performance.md`).
- **Platform:** Linux + Windows cross-build (tested under wine). The strict-GLSL fixes (§7.4) are verified with `glslangValidator` but not yet on the AMD PC. No native Windows build script, no macOS, no Windows sanitizers.
