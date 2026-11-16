# `nob.c`: monolith's build program

`./build.sh` compiles `nob.c` (in the repo root) into `build/nob` and runs it with its own arguments. Everything the build does is in `nob.c`, written on nob.h (`src/vendor/nob/nob.h`, vendored unmodified). For the rules a change must pass, see [CONTRIBUTING.md](../../CONTRIBUTING.md); for the tools, [architecture.md §8](../architecture.md#8-tools).

## Modes

The arguments of `./build.sh`, unchanged from the shell script `nob.c` replaces.

| Command | Builds |
|---|---|
| `./build.sh` | Linux x86_64 build in `build/linux/`: `monolith` (R3D, raylib, assimp and libstdc++ linked statically) + `maps/` (map packages); the folder can be copied as-is. Also writes `compile_commands.json` in the repo root for clangd (`WriteCompileCommands`). |
| `./build.sh run [args]` | Builds, then runs monolith with `args` (replaces this process). |
| `./build.sh debug` | Debug build (`-g -O0`, AddressSanitizer + UBSan, leak check at exit) in `build/linux-debug/`. |
| `./build.sh lsan` | LeakSanitizer-only build (`-O1 -g`, fast) in `build/linux-lsan/`: what `build/tools/leakcheck` runs. |
| `./build.sh profile` | CPU-profiling build in `build/linux-profile/`: `-O2 -g -fno-omit-frame-pointer`, with R3D / raylib / assimp as RelWithDebInfo + frame pointers in `$R3D_DIR/build-linux-profile` (so `build-linux` stays Release), plus the profiling tool `build/linux-profile/profile` (`tools/profile.c`, libc only; [profile.md](profile.md)). |
| `./build.sh --win` | Windows x86_64 build in `build/win/` (MinGW-w64): `monolith.exe` (statically linked) + `maps/`; the folder can be zipped and copied as-is. It is the view-only editor: benchmark, view, debug a map; no editing or requests (`MONOLITH_EDITING` in `src/common.h`; [main.md](main.md#view-only-build)). |
| `./build.sh --win run [args]` | Windows build, then runs it under wine (replaces this process): the way to open the view-only build on Linux (from an agent's shell only with `--shot` or `--bench --headless`, as the Linux build). |
| `./build.sh --win debug\|lsan` | Refused: the sanitizers don't work with MinGW. |
| `./build.sh --tools [name...]` | The small programs that replaced the repo's shell scripts (`TOOLS`; [architecture.md §8](../architecture.md#8-tools)): `tools/<name>.c` into `build/tools/<name>`, `tools/run-sh.c` into `build/run-sh.o`. All of them, or only the ones named; only what is missing or older than its source, `tools/tool.h` or nob.h is compiled. The plain build doesn't build them. Each program then rebuilds itself when its source changes (`tools/tool.h`, `ToolRebuildSelf`). |
| `./build.sh examples [--win \| test \| lsan]` | The examples: `examples/nob.c`, compiled into `examples/build/nob` when missing or older, then run with these arguments ([nob-examples.md](nob-examples.md) lists what each mode builds). |
| `./build.sh test [name...] [-- args...]` | The unit tests in `tests/unit/` (`UNIT_TESTS`, region "Build steps"): all of them or the ones named, each built with ASan + UBSan into `build/unit/<name>/` and run; exit 1 when any fails. Arguments after `--` are added to the run of every test that runs (use it with one named test: `./build.sh test paintunwrap -- <model files>`, `./build.sh test export_strip -- <export dir>`). |

Any other first argument builds like the plain Linux build but writes no `compile_commands.json` (as the shell script did: only no argument and `run` write it).

## Environment

| Variable | Meaning |
|---|---|
| `R3D_DIR` | Another R3D tree. Default: `submodules/r3d`, with local fixes (its `patches.md`). |
| `CC`, `CXX` | The Linux compilers. Default clang; `CXX` follows `CC` (clang → clang++, gcc → g++, otherwise c++). `--win` always uses `x86_64-w64-mingw32-gcc` / `g++`. |
| `R3D_BUILD_TYPE` | Linux only: the CMake build type of R3D / raylib / assimp, default Release. `R3D_BUILD_TYPE=None ./build.sh` gives the old unoptimised build (no `CMAKE_BUILD_TYPE`), only for comparisons with benchmark reports from before 2026-10-10. `profile` ignores it (always RelWithDebInfo, in its own build folder). |

## R3D, xatlas and the runtime

- **R3D** is built on first use as static libraries: into `$R3D_DIR/build-linux` for Linux and into `$R3D_DIR/build-mingw` for `--win` (toolchain: `cmake/mingw-w64-x86_64.cmake`). Later runs only rebuild what changed. If the configure options changed (e.g. `R3D_BUILD_TYPE`), the build folder is re-configured in place (a full R3D rebuild). Delete those folders to force a fresh configure.
- **xatlas** (vendored in `src/vendor/xatlas/`, MIT; C++, the paint unwrap) is compiled once per build folder into `<build folder>-obj/xatlas.o` with its own flags (`Xatlas`) and linked in.
- **The runtime** (`runtime/monolith_scene.c`) is built with `-DMONOLITH_EDITOR`: hot reload, source lines and the writers. `--export` writes it without them (`src/export.c`; [export.md](export.md)).

## Timing

Every command runs through `RunTimed()`, which prints after it how long it took (wall time) and how much CPU time it and every process it waited for used (user + sys, from `getrusage(RUSAGE_CHILDREN)`), and prints a summary table at the end. nob.h measures neither: its `nob_cmd_run()` forks and `waitpid()`s, which returns no resource usage. The commands run one after another (never nob's `.async`), so the `RUSAGE_CHILDREN` difference around one command is that command's own: `cmake --build -j` counts all of its compiler processes, because make waits for them and cmake waits for make. No CPU cycle counts: the developer chose time only (2026-11-09).

## Output

nob's `[INFO] CMD: ...` echo of each command and the `[TIME]` lines go to stdout, like the script's progress lines (`configuring ...`, `built: ...`, whose text scripts and people read); warnings and errors go to stderr. So `./build.sh lsan > /dev/null` (as `build/tools/leakcheck` runs it) stays quiet unless something fails.

## Adding a mode or a flag

- A **flag** goes into the argument list of the command it belongs to. The CFLAGS-like lists are built in `BuildLinux` / `BuildWindows`, in the same order the compiler sees them; keep the `compile_commands.json` entries in step when a list they copy changes.
- A **mode** is a branch in `main()` or `BuildLinux` that picks its output folder and flags.
- Run every command through `RunTimed()` (never `nob_cmd_run` directly, never `.async`), so it is timed and stays sequential.
