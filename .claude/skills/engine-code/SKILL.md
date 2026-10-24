---
name: engine-code
description: Change monolith's code — C in src/, the scene runtime (runtime/monolith_scene.c/.h), R3D under submodules/r3d, shaders outside a map package, build.sh, tools, examples. Build both targets with zero warnings, memory rules, the verification that keeps every map pixel-identical, common modifications (new map statement, env key, CLI flag, bundle change, graphics setting), conventions. Use before any code, shader, build or R3D change.
---

# Engine code

How to change code. Reference: `docs/knowledge/engine.md` (technology, layout, architecture, runtime API), `docs/knowledge/cli.md` (controls, flags, environment variables), `docs/knowledge/r3d-pitfalls.md` (read all of it, with the **`lighting-fog-shadows`** skill, before touching lighting, fog or shadows), `submodules/r3d/patches.md` (before changing R3D), `docs/performance.md` (measurements). Larger or multi-agent work: the **`large-engine-work`** skill. After every code change: the **`leak-check`** skill; performance claims: the **`benchmarking`** skill. Before you finish, ask the developer to restart the game: code changes don't hot-reload.

> **R3D changes are patches.** Work around R3D from the app side where reasonable. When R3D itself must change, change it minimally and **record every change in `submodules/r3d/patches.md`** (symptom, cause, files, diff). Don't modify `~/Projects/r3d` or other projects in `~/Projects`.

## 1. Build and run

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

A change is done only when it **builds with no warnings** on both targets, **`tools/leakcheck.sh` reports zero leaks**, and it has been **checked visually** (§3).


**Memory rules** (details in `docs/design-v2.md` §2.7):
- **No fixed-size content buffers.** Anything sized by content (map objects, lines, selection, regions, prompt, open requests, child processes, bench frames, export file lists) is a heap array (`MS_RESERVE` / `MS_PUSH`, `runtime/monolith_scene.h`, used by `src/` too) or a heap string (`MS_StrDup`). Fixed `char` arrays only for stack scratch formatting one path or message, and single values the loader bounds.
- **Sanity caps instead of limits**, each with a clear message: `MS_MAX_OBJECTS` (1 000 000 boxes + model instances + lights), `MS_MAX_FILE_BYTES` (4 MB map), `MS_PATH_MAX` (1024), 3 `repeat`s per line, a 1 MB prompt, `BENCH_FRAMES_LIMIT`.
- **The per-frame path never allocates** (drawing, picking, HUD, collision): scratch arrays grow to the largest size seen. Allocation happens on (re)load, input events, submit / export / report.
- **Everything is freed** (`MS_Unload`, editor state at exit); the leak check proves it.

## 2. Common modifications

**Edit or start a level.** Edit the package following its `MAP.md` (format: `docs/knowledge/map-format.md`; rules: the `map-editing` skill); it hot-reloads, no rebuild. New level: `--new-map <name>`, fill in `MAP.md`, build (or submit a first request: the session writes the brief first), or `--import` a raylib + R3D project. Models: the `assets-and-licences` skill.

**Add a map primitive or statement.** A builder next to `Box`/`Block`/`Stairs`/`WaffleFacade` in `runtime/monolith_scene.c` plus a `PRIMS[]` entry (name, argument count, per argument its shift axis `AX_X/Y/Z`, `AX_NONE` for sizes, `AX_INT` for counts), and its call where `ParseStatement()` dispatches. That code switches on the argument count (box and block, both 6, differ by `PRIMS[]` entry), so a new 6–8-argument primitive means switching on the entry. Builders return false when the map is full (`MS_MAX_OBJECTS`) or out of memory, and loops add a box per iteration so the cap bounds them. New statements need `monolith 2` (bump `MS_FORMAT_VERSION` only for changes older runtimes would misread). Document it in the `monolith_scene.c` header comment and `docs/knowledge/map-format.md` §2.

**Add an environment key.** Key tables in `monolith_scene.c` (region "Key tables": `ENV_KEYS` for `R3D_Environment` fields, sky / sun tables otherwise, a range in `ENV_RANGES`), its built-in default in `BuiltinEnvironment()` equal to today's value, written by `MS_WriteEnvironment`. Image changes without a reload must reach `FrameKey`.

**Change a request bundle.** `launcher.c` (`BuildBundle`, `WriteRequestMd`, `WriteRequestJson`; bump `"format"` when keys change) and `docs/knowledge/request-bundle.md` (keep older formats described; the `map-request` skill if the workflow changes); `tools/request-queue.sh` reads `kind`, `prompt`, `model` and `map.package` from `request.json` line by line. New terminal: `TERMINALS[]`.

**Add a viewpoint.** Spectator mode, **P** (prints *feet*), then `viewpoint "Name" ex ey ez tx ty tz` with the *eye*. `--bench` covers it automatically.

**Tune a map's look / shader.** Its `env` lines, materials and `shaders/` (hot-reloaded); respect `docs/knowledge/r3d-pitfalls.md` §1–5 (the `lighting-fog-shadows` skill) and the map's `MAP.md`; check shaders with `glslangValidator`. Changing a map's look needs the developer's approval.

**Add a graphics setting.** A field in `MS_Quality` (preset values, `MS_QualityEqual`, `MS_ApplyQuality`), the file key and panel row in `settings.c` / `settingspanel.c`; ultra stays the map exactly.

**Add an effect toggle.** The `TOGGLE(KEY_x, 'x')` block in the main loop, the HUD string, the header comment.

**Add a CLI flag.** Parse it at the top of `main()`; document it in the header comment and `docs/knowledge/cli.md`.

## 3. How to verify changes

No test suite; verification is **visual and numeric**:

1. **Build cleanly:** `./build.sh` and `./build.sh --win`, no warnings; `examples/build.sh` (and `--win`) when the runtime or an example changed. Shaders pass `glslangValidator`.
2. **No leaks:** `tools/leakcheck.sh` ends with `leakcheck: OK: zero leaks of ours` (the `leak-check` skill). After larger changes also `--asan`.
3. **Every map is unchanged** by engine work: for each map in `maps/` (always the default map; the others and `tests/models` at least when models, materials, lights or environment code changed), identical `--dump-boxes` and pixel-identical `--shot`s against a build from before the change: every viewpoint, yaw sweeps 0/60/…/300 from one position at a few eye heights, a `--reload-test`, plus the extra views the map's `MAP.md` lists under *Engine verification* (if it has that section).
4. **Fixed views:**
   ```bash
   ./build/linux/monolith --shot a.png --view 2                          # viewpoint 2
   ./build/linux/monolith --shot b.png --pos 0 0 -5 --look 120 5         # exact view
   ./build/linux/monolith --shot c.png --view 2 --off 16                 # effects off
   ./build/linux/monolith --shot d.png --view 3 --map maps/<name>         # another package
   ```
   For lighting or shadows, take a **yaw sweep from one position**: the black-curtain and shadow-popping bugs only showed when rotating.
5. **Isolate effects** with `--off` (1 volumetric fog, 2 fog, 3 SSAO, 4 SSIL, 5 bloom, 6 sun).
6. **Performance:** the `benchmarking` skill.
7. **Window titles:** automated runs are titled `monolith-bench` / `monolith-shot` (also the examples), normal play `monolith`; the developer's i3 config moves the first two to workspace 10 (`for_window [title="^monolith-(bench|shot)$"] …`). Keep these titles. Request terminals: `monolith-claude: <slug> <HHMMSS>`.
8. **Movement** (collision, controls) can only be tested by playing.
9. **Map loader and edits:** diff `--dump-boxes` / `--dump-scene` / `--dump-scene-diff` before and after (identical for every map after a loader change). A broken map prints `file:line: message` and exits 1 before any window; the running game keeps the old map. Hot reload runs only interactively; `--reload-test` exercises it in a shot.
10. **Designer and launcher without a window:**
    - `--shot x.png --view n --pick` must still report the same hit as before the change (the map's `MAP.md` may list a reference pick under *Engine verification*).
    - `--shot-hud` with `--select` / `--region` / `--type` / `--settings-panel` puts the overlay into the shot.
    - `--submit` **always** with `MONOLITH_REQUESTS_DIR=<scratch folder>`, `MONOLITH_TERMINAL=tools/stub-terminal.sh` (logs to `$MONOLITH_STUB_LOG`) and `MONOLITH_WORKSPACE=`; `gio trash` the folder afterwards. `tools/test-coordinator.sh` tests the queue end to end (one terminal for many submits, scheduling, cancel, recovery after killing the coordinator, per-request mode). `--classify` checks the model choice without a bundle.
    - Mouse and keyboard input can only be tested by hand.
11. **Export and examples:** export into a scratch folder, build its `viewer.c` outside the repo (the README's compiler line), compare its `--shot` with `monolith --shot` (pixel-identical for the shipped packages).

## 4. Conventions

- **English** for code, identifiers, comments and names.
- **Match the style:** 4-space indent, `static` functions, PascalCase functions, `UPPER_CASE` macros, `// #region` sections, short comments explaining *why*.
- **One module per concern:** each `src/<module>.c` has a `.h` interface and a header comment; file-local functions `static`; minimal shared state (`scene`, data paths).
- **The runtime stays portable:** `runtime/` depends only on raylib and R3D, compiles without `MS_PATCHED_R3D`, knows nothing about the editor. Editor-only code stays in `src/`.
- **The level is data:** geometry, materials, lights, environment live in `maps/<name>/`; C describes primitives and statements, not buildings or a look. Nothing outside a package assumes a style.
- **Assets are recorded** (`ASSETS.md` row + sidecar; CC0 / PD / CC-BY with attribution).
- **Memory:** heap arrays for content, no per-frame allocation, everything freed (§1, memory rules).
- **Comment non-obvious engine workarounds** with their reason (as for `r3d-pitfalls.md` §1, §2), so nobody "cleans them up".
- **Don't commit `build/`, screenshots, `benchmark/`, `requests/` or `export/`.**
- **Keep the docs current** in the same change: `src/main.c`'s header comment (controls, flags), `docs/knowledge/engine.md` (layout, architecture), `cli.md` (flags), `r3d-pitfalls.md` (pitfalls, with "if R3D changes X, update Y" warnings), `map-format.md`, the skills when a workflow changes, `patches.md`, measurements in `docs/performance.md`.
