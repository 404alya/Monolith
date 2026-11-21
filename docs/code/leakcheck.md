# `tools/leakcheck.c`: the leak check

`tools/leakcheck.c` runs monolith's main code paths under LeakSanitizer (or ASan + UBSan) and exits 1 on any leak of ours. It is built by `./build.sh --tools leakcheck` into `build/tools/leakcheck`. It replaced `tools/leakcheck.sh` (2026-11-13) with the same runs, arguments, environment, expected exit codes, output lines and exit codes. Overview of all the tools: [architecture.md §8](../architecture.md#8-tools).

**Standing rule** (`CLAUDE.md`): after any change to code (C, shaders, build), the leak check must report zero leaks before the change counts as done. Map-only edits don't need it.

## Usage

| Command | What it does |
|---|---|
| `build/tools/leakcheck` | Builds the LSan binaries if a source is newer, then runs everything. |
| `build/tools/leakcheck --no-build` | Uses the existing `build/linux-lsan` and `examples/build/linux-lsan` (and the sanitizer builds of the tools made by an earlier run). |
| `build/tools/leakcheck --bench` | Also runs a short headless bench (`--bench --headless --bench-frames 10 --bench-window 4`): only when no other benchmark runs (one GPU). |
| `build/tools/leakcheck --asan` | The same paths with the ASan + UBSan build (`./build.sh debug`, `build/linux-debug`): use-after-free, overflows, undefined behaviour; leaks too. |

The flags combine; anything else prints the usage line and exits 2.

## Sections

The sections run in this order; each is marked "Section" in `main()`.

1. **Build** (incremental):
   - monolith's sanitizer build (`./build.sh lsan` or `debug`) when a source is newer than the binary;
   - the examples' (`./build.sh examples lsan`, LSan mode only);
   - the profiling tool (`tools/profile.c`), `run-sh.o` and every program of `build/tools/`, with the same sanitizer, into the build folder (`library` after `./build.sh --tools library`, linked against its vendored archive `build/tools-obj/library/libvendor.a`, which is not instrumented: only our code is);
   - the paint unwrap test, against the build's own xatlas object;
   - the real `build/tools/stub-terminal` (`./build.sh --tools stub-terminal`) when it is missing.
2. **Output folder:** `build/leakcheck/<YYYYmmddHHMMSS>/` with `logs/` (kept; `build/` is not committed) and `tmp/` (shots, exports, scratch copies: trashed at the end). `build/leakcheck/latest` points at it.
3. **Each run** (`Run` in the code): one command with the sanitizer's options in its environment. Its stdout and stderr go to `logs/<name>.out`, its sanitizer report to `logs/<name>.<pid>`, and `<name> <status>` is appended to `logs/runs.txt`. A run whose exit status is not the expected one fails the check.
4. **Clean-up:** the files the runs created (`tmp/`, the new map packages in `maps/`) go to the trash (`gio trash`), never `rm`. So `gio` is checked right after the flags, before section 1: without it the leak check stops at once (`leakcheck: gio not found in PATH: …`, exit 1) instead of failing here, minutes later, with both left behind.
5. **Report:** `tools/lsan_report.py` (Python, kept) classifies the reports with `tools/lsan.supp` and prints one line per run and the stack of every leak of ours; then the final line.

## What it runs

Every run uses a hidden window (`--shot`, `--dump-*`, ...) or none: nothing appears on screen.

**Dumps and shots**

- `--dump-boxes`, `--dump-scene` (with and without models, and `--dump-scene-diff`).
- `--shot` of the default map: two views, camera-fitted shadow and fog cache variants; every quality preset, settings files, live settings changes including the renderer restart, the settings panel.
- `--shot` of `tests/models` (models, textured / shader materials, shadowed lights), of `examples/01_minimal/scene` and of `maps/pine_forest` (textured ground, many restyled models).

**Loading and reloading**

- `--reload-test` both ways and into a broken map; a broken map on the command line.
- Failing loads with long messages (a package with many `.map` files: on the command line and as a reload).
- `--bake-surface` (and a `data_sampler` reading its tile).

**Editor panels and inspection**

- `--shot-hud` with `--select` / `--region` / `--volume`.
- The render inspector: outline modes, labels, its panel, buffer views, the draw filter with a live edit, a preview and a reload.
- The id panel (`--id-panel` unfiltered, filtered, and with live id edits and the number field).
- The draw panel (`--draw-panel` whole, one section, clicks on its widgets through `--input-script`, a bad section).

**Edits**

- Manual edits: `--apply-edits` on a copy of `tests/models` (move, turn, split, duplicate, add, id, undo, redo and a failing command; `id` / `id remove` with a failing one); `--dump-entities`; the edit-mode HUD.
- Painting: `--paint-script` into the paint layer: every pen; first strokes on plain, textured, shader, turned and instanced boxes and model slots (a Kenney model unwrapped); a stroke onto a second face; face groups, resize, pixel look, clear, undo / redo; the eyedropper (paint and G-buffer albedo); save; a rebase onto an atlas changed on disk. `--apply-edits` with a paint script and with `paint-add` / `set` / `remove`. The draw mode's HUD and strokes through the input shim; a click that unwraps a model.
- Live edits: `--edit-script` with `--shot` and `--dump-entities` (moves, undo / redo, duplicate, add, a preview, save, and a rebase after `disk` with a failing step); with the fog cache and the AO volume; refused without `--shot` or a dump.
- The transform ops (turn with pitch / roll / local / pivot, scale, size): `--apply-edits` with a failing command, `--edit-script` with turn / scale previews.
- The transform modal (`--edit-modal`: open with the HUD, committed, cancelled; `--input-script`: mouse and keys through the input shim, a no-op turn), the rotate gizmo, the R step and reset.
- Turned boxes: shots with the caches, a reload, live edits.
- The model picker (`--model-picker`): thumbnails of the scene's files and of one loaded for the render, a filter, freed and rendered again across a reload and a renderer restart, ENTER and ESC.

**Requests and the AI classifier**

- `--submit` (with `build/tools/stub-terminal` instead of a terminal, `MONOLITH_WORKSPACE=`) on both maps, in coordinator mode (queue + coordinator start, a cancel with no coordinator) and in per-request mode.
- The AI classifier: `MONOLITH_CLASSIFIER` is the sanitizer copy of `build/tools/stub-classifier` for every run, never a real `claude -p`; an answer, a quit and a cancel while classifying, a failure.
- `--classify`: an answer, garbage, a timeout, off, an override.

**Export and compiled maps**

- `--export`, `--compile` and compiled maps: dumps, a shot with a reload, a damaged file, a stale one and its refused export.
- `--inspect-model`, `--new-map`.
- Every example's `--shot` (the game-style ones load compiled maps).

**Tools and tests**

- The profiling tool (`tools/profile.c`) on the fixtures in `tests/profile/` (`--report-only`, no perf run).
- The paint unwrap test (`tests/unit/paintunwrap_test.c`): xatlas on synthetic meshes and every model file in `maps/*/models/`.
- `run-sh.o` (`tools/run-sh.c`, built with the sanitizer into the build folder): `--check` on allowed, refused and unterminated lines, `-c`, `--hook` on Bash / Monitor / bad JSON; and its unit test (`./build.sh test run_sh`: ASan + UBSan, with its own sanitizer build of `run-sh.c` on every case).
- The other programs of `build/tools/`, each built with the sanitizer into `<build folder>/tools/`, on harmless inputs only:
  - `stub-terminal` with a log;
  - `request-queue` on a scratch requests folder (usage, list, wait, a bad id);
  - `todo` / `ai-todo`: `list` and refused arguments in a scratch copy of the repo root (their lists are copies; only where `submodules/simple-todo` is checked out);
  - `stub-coordinator` and `test-coordinator` in that scratch root (they stop at their first check: no queue program, no monolith);
  - `bench-export-ab --summarize` on hand-made reports;
  - `old-monolith` with bad arguments and as its `rm` stand-in;
  - `library` on scratch databases with the fixture `tests/library/catalog.jsonl`: ingest (new, unchanged, refused), `embed` with the stub embedder and without the GGUF (exit 3), `search` with every filter and weights, `show` for each source, `pin`, `check`, `dump`, `eval` and usage errors; and, when the GGUF is in `$MONOLITH_EMBED_DIR` or `~/.cache/monolith/embed/`, the real model on a second database (ingest, embed, search, check), else a line saying those runs were skipped.

**What it never runs:** a real benchmark (beside `--bench`), a real old build, a real coordinator.

Bundles go to its own requests folder (`MONOLITH_REQUESTS_DIR=<tmp>/requests`), never to `requests/`: a coordinator running meanwhile never sees them, and bundles queued meanwhile are left alone.

## How leaks are classified

`tools/lsan_report.py` classifies the leaks with `tools/lsan.supp`. Only memory allocated by GPU driver or system library code is ignored (see the comment at the top of `lsan.supp`); every leak allocated by our code, raylib, R3D or assimp counts. Logs: `build/leakcheck/latest/logs/`.

## Environment

| Variable | Use |
|---|---|
| `CC` | The compiler of the sanitizer builds made here (profile, `run-sh.o`, the tools, the paint unwrap test); default `clang`. `./build.sh` reads it too. |
| `R3D_DIR` | Another R3D tree (default `submodules/r3d`): its sources count for "a source is newer", and the paint unwrap test links its libraries from `$R3D_DIR/build-linux`. |

Set for the runs (and put back after each):

| Mode | Variables |
|---|---|
| LSan | `LSAN_OPTIONS` |
| `--asan` | `ASAN_OPTIONS`, `UBSAN_OPTIONS` and `LSAN_OPTIONS` |
| `run-sh.o`'s runs | `RUN_SH_DIR` (unset afterwards, as the script did) |

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Zero leaks of ours and every run exited as expected (`leakcheck: OK: zero leaks of ours`). |
| 1 | A leak of ours, an unexpected exit status or a sanitizer error (`leakcheck: FAILED: ...`), or a build failed / a binary is missing, or `gio` is not in `PATH` (checked first: `leakcheck: gio not found in PATH: …`). |
| 2 | A bad argument. |

## Called by

Every session after a code change (`CLAUDE.md`, the `leak-check` skill), and the developer.
