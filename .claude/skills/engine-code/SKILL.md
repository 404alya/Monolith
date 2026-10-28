---
name: engine-code
description: Change monolith's code — C in src/, the scene runtime (runtime/monolith_scene.c/.h), R3D under submodules/r3d, shaders outside a map package, build.sh, tools, examples. Build both targets with zero warnings, memory rules, the verification that keeps every map pixel-identical, common modifications (new map statement, env key, CLI flag, bundle change, graphics setting), conventions. Use before any code, shader, build or R3D change.
---

# Engine code

How to change code. Reference: `docs/architecture.md` (technology, layout, architecture, runtime API), `docs/command-line.md` (controls, flags, environment variables), `.claude/skills/lighting-fog-shadows/r3d-pitfalls.md` (read all of it, with the **`lighting-fog-shadows`** skill, before touching lighting, fog or shadows), `submodules/r3d/patches.md` (before changing R3D), `.claude/skills/benchmarking/measurements.md` (measurements). Larger or multi-agent work: the **`large-engine-work`** skill. After every code change: the **`leak-check`** skill; performance claims: the **`benchmarking`** skill. Before you finish, ask the developer to restart the game: code changes don't hot-reload.

> **R3D changes are patches.** Work around R3D from the app side where reasonable. When R3D itself must change, change it minimally and **record every change in `submodules/r3d/patches.md`** (symptom, cause, files, diff). Never modify other R3D checkouts or projects outside this repo.

## 1. Build and run

```bash
./build.sh              # -> build/linux/monolith + build/linux/maps/  (clang, -O2)
./build.sh run [args]   # build, then run with args
./build.sh debug        # -g -O0 + ASan/UBSan -> build/linux-debug/
./build.sh lsan         # -O1 -g + LeakSanitizer -> build/linux-lsan/; examples: examples/build.sh lsan (test build)
./build.sh profile      # -O2 -g + frame pointers (R3D/raylib/assimp RelWithDebInfo) -> build/linux-profile/, + its profile tool (tools/profile.c)
tools/leakcheck.sh [--asan]     # every main code path under LSan (or ASan + UBSan); exit 1 on a leak
R3D_DIR=/other/r3d ./build.sh   # another R3D tree;  CC=gcc ./build.sh: another compiler
./build.sh --win [run [args]]   # -> build/win/monolith.exe + build/win/maps/ (MinGW-w64); run under wine
```

Both output folders are **self-contained** (copy or zip as-is).

**R3D** is built by `build.sh`: configured on first use in `$R3D_DIR/build-linux` / `build-mingw` (a few minutes, assimp), then an incremental `cmake --build` each time (~1 s no-op; log in `<build dir>/monolith-build.log`). Delete the build dir to force a fresh configure.

**Linux:**
- R3D config: static, vendored raylib + assimp + zlib (`ASSIMP_BUILD_ZLIB`), bundled minizip (`ASSIMP_BUILD_MINIZIP`; a system minizip breaks the build), no examples, GPU profiler compiled in (`R3D_GPU_PROFILER`, on only under `--bench`; patch 5), `CMAKE_BUILD_TYPE=Release`. Options are remembered in `build-linux/monolith-configure.args`; a change re-configures (full R3D rebuild). `R3D_BUILD_TYPE=None` gives the old unoptimised build; any other value is passed as `CMAKE_BUILD_TYPE`. Bench reports state the R3D build type (no measurable difference: `.claude/skills/benchmarking/measurements.md`).
- Links R3D, raylib, assimp, zlib and **libstdc++ statically** (`-l:libstdc++.a`). Runtime deps only `libc`, `libm`, `libgcc_s.so.1`, `libGL.so.1` (check: `readelf -d build/linux/monolith | grep NEEDED`).
- **glibc ≥ 2.34.** `build.sh` passes `-Wl,--wrap=` for newer symbols and `src/glibc_compat.c` routes them to old versions (libm to `@GLIBC_2.2.5`, `arc4random` → `getrandom`, `__isoc23_strtoul` → `strtoul`, `__isoc23_strtol` → `strtol`). After changing compiler, glibc or dependencies, `objdump -T build/linux/monolith | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1` must still print `GLIBC_2.34`; add any newer symbol to `GLIBC_WRAP` in `build.sh` and `examples/build.sh` (two copies of the list) and to `glibc_compat.c`.
- A portability test on this machine still uses the repo paths unless they're hidden: `unshare -rm sh -c 'mount -t tmpfs none <repo path> && /path/to/copy/monolith --shot /tmp/x.png --view 2'`. x86-64 only.

**Windows (`--win`)**, `x86_64-w64-mingw32-gcc`:
- R3D in `$R3D_DIR/build-mingw` with `cmake/mingw-w64-x86_64.cmake` (Release, static). The exe is **fully static** (`-static`): `build/win/` is `monolith.exe` + `maps/`, no DLLs. Imports only system DLLs (`opengl32`, `gdi32`, `user32`, `kernel32`, `shell32`, `winmm`, UCRT `api-ms-win-crt-*`); check with `x86_64-w64-mingw32-objdump -p build/win/monolith.exe | grep "DLL Name"`.
- Console-subsystem exe (bench tables and **P** output visible). Bench reports go to `build/win/benchmark/`. `--win debug` is refused (no sanitizers with MinGW).
- Verify under wine (host GL driver, shots pixel-identical to Linux): `wine build/win/monolith.exe --shot win.png --view 2`. wine's `MESA-EGL: warning` lines on NVIDIA are harmless.

A change is done only when it **builds with no warnings** on both targets, **`tools/leakcheck.sh` reports zero leaks**, and it has been **checked visually** (§3).


**Memory rules:**
- **No fixed-size content buffers.** Anything sized by content (map objects, lines, selection, regions, prompt, open requests, child processes, bench frames, export file lists) is a heap array (`MS_RESERVE` / `MS_PUSH`, `runtime/monolith_scene.h`, used by `src/` too) or a heap string (`MS_StrDup`). Fixed `char` arrays only for stack scratch formatting one path or message, and single values the loader bounds.
- **Sanity caps instead of limits**, each with a clear message: `MS_MAX_OBJECTS` (1 000 000 boxes + model instances + lights), `MS_MAX_FILE_BYTES` (4 MB map), `MS_PATH_MAX` (1024), 3 `repeat`s per line, a 1 MB prompt, `BENCH_FRAMES_LIMIT`.
- **The per-frame path never allocates** (drawing, picking, HUD, collision): scratch arrays grow to the largest size seen. Allocation happens on (re)load, input events, submit / export / report.
- **Everything is freed** (`MS_Unload`, editor state at exit); the leak check proves it.

## 2. Common modifications

**Edit or start a level.** Edit the package following its `MAP.md` (format: `docs/map-format.md`; rules: the `map-editing` skill); it hot-reloads, no rebuild. New level: `--new-map <name>`, fill in `MAP.md`, build (or submit a first request: the session writes the brief first), or `--import` a raylib + R3D project. Models: the `assets-and-licences` skill.

**Add a map primitive or statement.** A builder next to `Box`/`Block`/`Stairs`/`WaffleFacade` in `runtime/monolith_scene.c` plus a `PRIMS[]` entry (name, argument count, per argument its shift axis `AX_X/Y/Z`, `AX_NONE` for sizes, `AX_INT` for counts), and its call where `ParseStatement()` dispatches. That code switches on the argument count (box and block, both 6, differ by `PRIMS[]` entry), so a new 6–8-argument primitive means switching on the entry. Builders return false when the map is full (`MS_MAX_OBJECTS`) or out of memory, and loops add a box per iteration so the cap bounds them. New statements need `monolith 2` (bump `MS_FORMAT_VERSION` only for changes older runtimes would misread). **Compiled maps** (`docs/compiled-map.md`, region "Compiled maps"): a statement that only makes boxes needs nothing more (boxes are stored expanded), but anything new the parser stores in `SceneData` (a field, a list, a new kind of object) needs a compiled record or field: written by `WriteCompiled`, read and checked by `ReadCompiled` (with the parser's own limits), `_Static_assert`s updated, the layout in `docs/compiled-map.md`, and `CM_VERSION` bumped; then the round trip (§3, step 9) and `tests/unit/compiled_test.sh`. Document it in the `monolith_scene.c` header comment and `docs/map-format.md` §2. Manual edits (`src/mapedit.c`, `Analyze`) learn a new box primitive's position arguments from `PRIMS[]` through `MS_PrimitiveArgs`; a new statement of another shape (like `light` / `model`) needs its position numbers added to `Analyze`, or the edit mode can't move it. Manual edits are live (`docs/live-editing.md`): `MS_EditSetText` carries over every GPU resource that doesn't depend on objects, so anything new that a statement uploads or caches must either be carried over there or make the non-object hash differ (a full upload); a new static cache must take the dirty regions (re-stamp, invalidate what they reach), and `--edit-script` vs. `--apply-edits` shots (0 changed pixels) are the test.

**Add an environment key.** Key tables in `monolith_scene.c` (region "Key tables": `ENV_KEYS` for `R3D_Environment` fields, sky / sun tables otherwise, a range in `ENV_RANGES`; any other check in `CheckEnvValue`, which the text parser and compiled maps share), its built-in default in `BuiltinEnvironment()` equal to today's value, written by `MS_WriteEnvironment`. Compiled maps store env keys by name, so a key of an existing type needs no format change; a new value type (`T_*`, appended at the end) needs `CmPutEnv` / `CmReadEnvRecord` and a `CM_VERSION` bump. Image changes without a reload must reach `FrameKey`.

**Change a request bundle.** The C strings in `src/launcher.c` (`request.md`, `request.json` `instructions`, the coordinator's start prompt) and `src/import.c` name `CLAUDE.md`, `.claude/skills/map-request/SKILL.md` (and its section 3), `.claude/skills/import-request/SKILL.md` and `.claude/skills/coordinator/SKILL.md`: don't rename, move or renumber those without changing the strings in the same change. `launcher.c` (`BuildBundle`, `WriteRequestMd`, `WriteRequestJson`; bump `"format"` when keys change) and `.claude/skills/map-request/bundle-reference.md` (keep older formats described; the `map-request` skill if the workflow changes); `tools/request-queue.sh` reads `kind`, `prompt`, `model` and `map.package` from `request.json` line by line. New terminal: `TERMINALS[]`.

**Add a viewpoint.** Spectator mode, **P** (prints *feet*), then `viewpoint "Name" ex ey ez tx ty tz` with the *eye*. `--bench` covers it automatically.

**Tune a map's look / shader.** Its `env` lines, materials and `shaders/` (hot-reloaded); respect `.claude/skills/lighting-fog-shadows/r3d-pitfalls.md` §1–5 (the `lighting-fog-shadows` skill) and the map's `MAP.md`; check shaders with `glslangValidator`. Changing a map's look needs the developer's approval.

**Add a graphics setting.** A field in `MS_Quality` (preset values, `MS_QualityEqual`, `MS_ApplyQuality`), the file key and panel row in `settings.c` / `settingspanel.c`; ultra stays the map exactly.

**Add an effect toggle.** The `TOGGLE(KEY_x, 'x')` block in the main loop, the HUD string, the header comment.

**Draw editor text or a panel.** Through `src/ui.h` only (`UiText` / `UiTextBold` / `UiMeasure`, `UiHudText` / `UiHudHints` for top-left HUD lines on their backdrop, `UiHints` markup for keys: `{KEY}` cap, `|` separator, `~off~`, `^on^`; `UiPanel`, `UiPill`, `UiToast`), with the type-scale sizes (`UI_XS` … `UI_XL`) and colour tokens; never `DrawText` / `MeasureText` / `GetFontDefault` (raylib's font), and no other size: the atlas holds only the scale. Measure and draw with the same helper and size (the prompt box wraps with `UiAdvance`), or the cursor drifts. Text is drawn at integer positions. No `DrawRectangleRounded*` in the overlay: they emit dozens of triangles and switch the batch's draw mode, and with ~60 key caps they cost +0.13 ms a frame (more than the whole UI before); `UiPanel` / `UiPill` / the key caps draw rounded shapes from the atlas instead. Time an overlay change with an instrumented build (glFinish before and after the UI block, average over `--bench-still --frame-reuse --bench-window 4`): whole-frame benches on a laptop are too noisy to see tens of microseconds. Keep `{ } | ~ ^` out of markup that contains map data (group or file names): draw that with `UiHudText`. The runtime, `runtime/export/` and the examples stay on raylib's font. **Change the font:** put the static `.ttf` files and their `OFL.txt` in `resources/fonts/<name>/` with an `ASSETS.md` row and sidecars (the `assets-and-licences` skill), regenerate `src/font_outfit.h` with the command in `tools/embed_font.py` (run under `python3 -I`), adjust `EM_TO_PX` in `ui.c` to the font's ascender − descender, and take `--shot-hud` before/after images.

**Add a CLI flag.** Parse it at the top of `main()`; a new mode that opens no window or only a hidden one also goes into the `windowless` check after the flag loop (otherwise the game refuses it in an agent's shell); document it in the header comment and `docs/command-line.md`.

## 3. How to verify changes

No test suite; verification is **visual and numeric**:

1. **Build cleanly:** `./build.sh` and `./build.sh --win`, no warnings; `examples/build.sh` (and `--win`, `test`) when the runtime or an example changed. The examples are plain integration code (no tool flags); `--shot` exists only in their test build, `tests/examples/harness.h` force-included by `examples/build.sh test` / `lsan`: keep test hooks there, not in the examples. Shaders pass `glslangValidator`.
2. **No leaks:** `tools/leakcheck.sh` ends with `leakcheck: OK: zero leaks of ours` (the `leak-check` skill). After larger changes also `--asan`.
3. **Every map is unchanged** by engine work: for each map in `maps/` (and `tests/models` when models, materials, lights or environment code changed), identical `--dump-boxes` and pixel-identical `--shot`s against a build from before the change: every viewpoint, yaw sweeps 0/60/…/300 from one position at a few eye heights, a `--reload-test`, plus the extra views the map's `MAP.md` lists under *Engine verification* (if it has that section). Rendering is deterministic (the same build gives the same image on every run; R3D patch 17 fixed draw order that followed heap addresses): a shot that differs between two runs of one build is a bug, so check with two runs of the new build before blaming the change.
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
7. **Window titles:** automated runs are titled `monolith-bench` / `monolith-shot` (also the examples' test build), normal play `monolith`; a window-manager rule can move the first two out of the way (i3: `for_window [title="^monolith-(bench|shot)$"] …`). Keep these titles. Request terminals: `monolith-claude: coordinator`, per-request mode `monolith-claude: <slug> <HHMMSS>`.
8. **Movement** (collision, controls) can only be tested by playing.
9. **Map loader and edits:** diff `--dump-boxes` / `--dump-scene` / `--dump-scene-diff` before and after (identical for every map after a loader change). After a change to the parser, the compiled format or `SceneData`: the **round trip** too: `--compile` every map in `maps/` and `tests/models` into a scratch copy of its package (twice: byte-identical), then its `--dump-*` (all but the `map "<path>"` line) and `--shot`s from the `.mapb` must equal the text's; `tests/unit/compiled_test.sh` (corruption test, ASan + UBSan) and `tests/unit/export_strip_test.sh` (the export's runtime has no parser). A broken map prints `file:line: message` and exits 1 before any window; the running game keeps the old map. Hot reload runs only interactively; `--reload-test` exercises it in a shot.
10. **Designer and launcher without a window:**
    - `--shot x.png --view n --pick` must still report the same hit as before the change (the map's `MAP.md` may list a reference pick under *Engine verification*).
    - `--shot-hud` with `--select` / `--region` / `--type` / `--settings-panel` puts the overlay into the shot.
    - `--submit` **always** with `MONOLITH_REQUESTS_DIR=<scratch folder>`, `MONOLITH_TERMINAL=tools/stub-terminal.sh` (logs to `$MONOLITH_STUB_LOG`) and `MONOLITH_WORKSPACE=`; `gio trash` the folder afterwards. `tools/test-coordinator.sh` tests the queue end to end (one terminal for many submits, scheduling, cancel, recovery after killing the coordinator, per-request mode). `--classify` checks the model choice without a bundle.
    - Mouse and keyboard input: `--input-script` plays scripted keys, clicks and mouse motion through the interactive input path in a hidden `--shot` (`src/input.c`: every `src/` module reads input through it, so call raylib's input functions only via `common.h`); the feel of a real mouse and GLFW itself can only be tested by hand.
11. **Export and examples:** export into a scratch folder and build it there (its `build.sh`). The exported viewer and the game-style examples load the compiled map; moving the export's `.map` out of its package must not change a shot. Its `viewer.c` has no screenshot flag: render it with the test harness (compile `viewer.c` with `-include <repo>/tests/examples/harness.h -I<repo>/runtime` and run with `--shot`), or with `examples/build/linux-test/01_minimal/minimal <export>/<map> --shot … --view n`; compare with `monolith --shot … --view n` (pixel-identical for the shipped packages).

## 3a. Export changes

After changing the export, the runtime's editor blocks, `runtime/export/*` or what R3D's build reads: export every map in `maps/` (and `tests/models`) into a scratch folder, copy each into a fresh folder and run its `./build.sh` (and `--win`), then render every viewpoint with a scratch harness linked against that package's runtime and bundled R3D (hidden window, 1600 × 900, FXAA, `MS_InitRenderer`, 30 frames into a `RenderTexture`) and compare with `monolith --map … --shot … --view n`: they must match. Grep the package for tool leftovers (`--shot`, `bench`, `hidden`, `MS_Poll`, `reload`, `editor`, `request`, `Claude`): only the map's own files (its comments, `MAP.md`, `ASSETS.md`) and R3D's own sources may hit.

## 4. Conventions

- **English** for code, identifiers, comments and names.
- **Match the style:** 4-space indent, `static` functions, PascalCase functions, `UPPER_CASE` macros, `// #region` sections, short comments explaining *why*.
- **One module per concern:** each `src/<module>.c` has a `.h` interface and a header comment; file-local functions `static`; minimal shared state (`scene`, data paths).
- **The runtime stays portable:** `runtime/` depends only on raylib and R3D, compiles without `MS_PATCHED_R3D`, knows nothing about the editor. The text parser is editor-only: code the game build needs (the compiled-map reader and what it checks with) stays outside the `MONOLITH_EDITOR` blocks and must not parse map text. Editor-only code stays in `src/`; tool-only code inside `runtime/` goes into `#ifdef MONOLITH_EDITOR` blocks (never nested; `#ifndef MONOLITH_EDITOR … #endif` for game-only code, `#else` allowed). Comments outside those blocks ship with `--export`, so they must not mention the editor, requests, Claude Code, hot reload or monolith's command-line flags. The runtime builds with zero warnings with and without `MONOLITH_EDITOR` (and without `MS_PATCHED_R3D`): check all three after a runtime change.
- **The level is data:** geometry, materials, lights, environment live in `maps/<name>/`; C describes primitives and statements, not buildings or a look. Nothing outside a package assumes a style.
- **Assets are recorded** (`ASSETS.md` row + sidecar; CC0 / PD / CC-BY with attribution).
- **Memory:** heap arrays for content, no per-frame allocation, everything freed (§1, memory rules).
- **Comment non-obvious engine workarounds** with their reason (as for R3D pitfalls §1, §2), so nobody "cleans them up".
- **Don't commit `build/`, screenshots, `benchmark/`, `requests/` or `export/`.**
- **Keep the docs current** in the same change: `src/main.c`'s header comment (controls, flags), `docs/architecture.md` (layout, architecture), `docs/command-line.md` (flags), `docs/user-guide.md` (keys, settings), `docs/request-queue.md`, `docs/troubleshooting.md` and `docs/glossary.md` where they are affected, `CONTRIBUTING.md` (rules for people), `.claude/skills/lighting-fog-shadows/r3d-pitfalls.md` (pitfalls, with "if R3D changes X, update Y" warnings), `map-format.md`, the skills when a workflow changes, `patches.md`, measurements in `.claude/skills/benchmarking/measurements.md`.
