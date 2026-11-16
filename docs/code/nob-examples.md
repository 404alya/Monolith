# `examples/nob.c`

The build program of the examples.

`./build.sh examples [mode]` (the repo's `nob.c`, `BuildExamples`) compiles this file into `examples/build/nob` when that is missing or older than this file or nob.h, then runs it in its place with the mode. Run directly (`examples/build/nob [mode]`), it rebuilds itself the same way (`NOB_GO_REBUILD_URSELF`).

It is written on nob.h (`src/vendor/nob/nob.h`, vendored unmodified), like the repo's own build program `nob.c`, whose structure and helpers it follows. It replaced `examples/build.sh` (2026-11-13): the same commands with the same arguments in the same order, the same output folders and data copies.

## Modes

The mode is the first argument. Any other argument, or none, is the plain build, as the script read only `$1`.

| Command | Output folder | Build |
|---|---|---|
| `./build.sh examples` | `examples/build/linux/<example>/` | clang, `-O2` |
| `./build.sh examples --win` | `examples/build/win/<example>/` | MinGW-w64: `x86_64-w64-mingw32-gcc` |
| `./build.sh examples test` | `examples/build/linux-test/<example>/` | the test build (below) |
| `./build.sh examples lsan` | `examples/build/linux-lsan/<example>/` | the test build with LeakSanitizer, `-O1 -g`; the leak check runs these |

Each output folder holds the program(s) and the data they load (`scene/` or `assets/`), so it runs from anywhere. The object files go next to it, into `<output folder>-obj/<example>/` (outside the program folders, which stay copyable), as does the log of the last scene compile (`<output folder>-obj/compile.log`).

## The test builds

The examples are plain integration code. The test builds compile each example's main file with `-include tests/examples/harness.h`: the same program then needs `--shot <file>`, renders 30 frames in a hidden window, saves the image and exits (it never opens a visible window), e.g.

```sh
examples/build/linux-test/01_minimal/minimal --shot /tmp/x.png --view 2
```

or, as a benchmark of the runtime as a game builds it (the harness's bench mode; one at a time, `__GL_SYNC_TO_VBLANK=0`):

```sh
examples/build/linux-test/01_minimal/minimal --bench report.md <package>
```

## How the examples are built

- They build against monolith's R3D copy (`submodules/r3d`, with `-DMS_PATCHED_R3D`) and its static libraries, which the repo's `./build.sh` builds: run it once first (or `./build.sh --win` for `--win`).
- The same compiler flags as the monolith binary (zero warnings), the same static linking and the same glibc compatibility shim (`src/glibc_compat.c`) on Linux.
- The runtime is built like an exported game (no `-DMONOLITH_EDITOR`), except for `03_live_editing` (`MS_Poll`).
- A game build reads only compiled maps, so the game-style examples get their scene compiled next to the copied text (`<name>.mapb`, through `build/linux/monolith --compile`, also for `--win`). `03_live_editing` loads the text.

## Environment

| Variable | Meaning |
|---|---|
| `R3D_DIR` | Another R3D tree (default: `submodules/r3d`). Its `build-linux/` (or `build-mingw/` for `--win`) must hold the static libraries, `include/` and raylib's headers. |
| `CC` | The Linux compiler (default clang). `--win` always uses `x86_64-w64-mingw32-gcc`. |

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Every example built. |
| 1 | Something is missing (`build/linux/monolith`, the R3D libraries), a command failed (its output above says why) or a scene did not compile (its log is printed). |

## Output

Like the repo's `nob.c`. Every command runs through `RunTimed()`, which echoes it (`[INFO] CMD: ...`) and prints how long it took (`[TIME] ...`); a summary table ends the run. Those lines and the progress lines (`built: <path>`, `compiled: <path>`, the text the script printed) go to stdout, errors to stderr, so `./build.sh examples lsan > /dev/null` (the leak check) stays quiet unless something fails.

## Keep in step

The repo's `nob.c` mirrors these programs and their flags in `compile_commands.json` for clangd (`WriteCompileCommands`, "the examples"), and has the same glibc `--wrap` list (`glibcWrap`). A flag change here changes those lines there.
