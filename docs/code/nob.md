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
| `./build.sh --tools [name...]` | The small programs that replaced the repo's shell scripts (`TOOLS`; [architecture.md §8](../architecture.md#8-tools)): `tools/<name>.c` into `build/tools/<name>`, `tools/run-sh.c` into `build/run-sh.o`. All of them, or only the ones named; only what is missing or older than its source, `tools/tool.h` or nob.h is compiled. The plain build doesn't build them. Each program then rebuilds itself when its source changes (`tools/tool.h`, `ToolRebuildSelf`). `library` is the one program with vendored libraries: its SQLite, sqlite-vec and llama.cpp are compiled first (`LibraryVendor`, below), and it rebuilds itself through `./build.sh --tools library` (`ToolInitBuilt`). |
| `./build.sh examples [--win \| test \| lsan]` | The examples: `examples/nob.c`, compiled into `examples/build/nob` when missing or older, then run with these arguments ([nob-examples.md](nob-examples.md) lists what each mode builds). |
| `./build.sh test [name...] [-- args...]` | The unit tests in `tests/unit/` (`UNIT_TESTS`, region "Build steps"): all of them or the ones named, each built with ASan + UBSan into `build/unit/<name>/` and run; exit 1 when any fails. Arguments after `--` are added to the run of every test that runs (use it with one named test: `./build.sh test paintunwrap -- <model files>`, `./build.sh test export_strip -- <export dir>`). |

Any other first argument builds like the plain Linux build but writes no `compile_commands.json` (as the shell script did: only no argument and `run` write it).

## Environment

| Variable | Meaning |
|---|---|
| `R3D_DIR` | Another R3D tree. Default: `submodules/r3d`, with local fixes (its `patches.md`). |
| `CC`, `CXX` | The Linux compilers. Default clang; `CXX` follows `CC` (clang → clang++, gcc → g++, otherwise c++). `--win` always uses `x86_64-w64-mingw32-gcc` / `g++`. |
| `R3D_BUILD_TYPE` | Linux only: the CMake build type of R3D / raylib / assimp, default Release. `R3D_BUILD_TYPE=None ./build.sh` gives the old unoptimised build (no `CMAKE_BUILD_TYPE`), only for comparisons with benchmark reports from before 2026-10-10. `profile` ignores it (always RelWithDebInfo, in its own build folder). |
| `JOBS` | How many of monolith's compile commands run at once (a positive number). Default: the CPUs the build may use (`nproc`: the affinity mask, `OMP_NUM_THREADS` / `OMP_THREAD_LIMIT`), the same count R3D's `cmake --build -j` gets; `JOBS` doesn't change that `-j`. `JOBS=1` compiles one file at a time. The examples' build reads it too. |

## R3D, xatlas and the runtime

- **R3D** is built on first use as static libraries: into `$R3D_DIR/build-linux` for Linux and into `$R3D_DIR/build-mingw` for `--win` (toolchain: `cmake/mingw-w64-x86_64.cmake`). Later runs only rebuild what changed. If the configure options changed (e.g. `R3D_BUILD_TYPE`), the build folder is re-configured in place (a full R3D rebuild). Delete those folders to force a fresh configure.
- **xatlas** (vendored in `src/vendor/xatlas/`, MIT; C++, the paint unwrap) is compiled once per build folder into `<build folder>-obj/xatlas.o` with its own flags (`Xatlas`) and linked in.
- **monolith's C files** (`src/*.c` and `runtime/*.c`; `--win` leaves out `launcher.c` and `classifier.c`) are compiled one object per file into `<build folder>-obj/`, at the source's path below the repo root (`build/linux-obj/src/main.o`, `build/win-obj/runtime/monolith_scene.o`), in parallel (`CompileObjects`, below), then linked by one command with the same flags, the objects in source order, `xatlas.o` and the libraries. Each build folder has its own objects (`build/linux-obj`, `linux-debug-obj`, `linux-lsan-obj`, `linux-profile-obj`, `win-obj`), so modes never share an object.
- **Only what is out of date compiles.** An object compiles again when it is missing, older than its source or any header it includes (the compiler writes them into `<obj>.d` with `-MMD -MF`; system headers are left out, R3D's and raylib's are in), or older than `nob.c`; and every object of a folder compiles again when the compiler or its flags differ from the ones in `<build folder>-obj/objects.flags` (the stamp: the compile command without the per-file part, written once all objects compiled with it, so a build that failed half-way is redone). The largest sources start first, so `runtime/monolith_scene.c`, the longest compile, bounds the pool's time instead of starting last.
- **The link is skipped** when nothing it reads changed: the binary exists, the link command is the one in `<build folder>-obj/monolith.link` (the last successful link's), and no object, `xatlas.o`, R3D library (`libr3d.a`, `libraylib.a`, `libassimp.a`, `libzlibstatic.a`) or `nob.c` is newer than the binary. The build then prints `up to date: <binary> (…: not linked)` before the usual `built:` line. A compiler upgrade under the same name isn't seen by the stamps (as for xatlas): delete the `-obj` folder after one.
- **The library tool's vendored code** (SQLite, sqlite-vec and llama.cpp in `src/vendor/`; [library.md](library.md), `docs/model-library.md` §10) is built only by `./build.sh --tools library` and `./build.sh test library` (`LibraryVendor`): 239 files, collected with `CollectCompileJobs` (the first half of `CompileObjects`: the same staleness rules, `.d` files and `objects.flags` stamps) into three folders of `build/tools-obj/library/` with their own flags (`sqlite/`: C, portable; `ggml-c/`: C, `-march=x86-64-v3`; `cxx/`: C++17, `-march=x86-64-v3`), compiled in one pool and packed into `build/tools-obj/library/libvendor.a` (`ar rcsD`, again when an object or `nob.c` is newer). `-w -O2 -DNDEBUG`, never `-march=native`, no OpenMP, no GPU backend; the flags and why are in each vendor folder's `README.md`. CMake's two generated llama.cpp headers are written into `build/tools-obj/library/gen/` (`WriteIfChanged`). `build/tools/library` is `tools/library.c` compiled with the tools' flags, the vendored headers (`LIBRARY_INCLUDES`), the archive and `-lstdc++ -lm -lpthread` (`LIBRARY_LIBS`), when it is missing or older than its source, `tools/tool.h`, nob.h, the archive or `nob.c` (`BuildLibraryTool`). The plain build, `--win`, the examples and the other modes never touch any of it: their commands are the same as before it existed.
- **The runtime** (`runtime/monolith_scene.c`) is built with `-DMONOLITH_EDITOR`: hot reload, source lines and the writers. `--export` writes it without them (`src/export.c`; [export.md](export.md)).

## Timing

A command runs either alone, through `RunTimed()`, or with others in a pool, through `RunPool()` (monolith's compile commands). Both print after each command how long it took (wall time) and how much CPU time it and every process it waited for used (user + sys), and a summary table ends the build. nob.h measures neither: its `nob_cmd_run()` forks and `waitpid()`s, which returns no resource usage.
- **`RunTimed`** (R3D's cmake, xatlas, the link, the profile tool, the tests): CPU time is the `getrusage(RUSAGE_CHILDREN)` difference around the command. That is exact because nothing else runs meanwhile: `cmake --build -j` counts all of its compiler processes, because make waits for them and cmake waits for make.
- **`RunPool(jobs, label, size)`**: runs a list of commands, up to `size` at once (`PoolSize`: `JOBS` or `nproc`). It starts each child itself (fork + exec) and reaps it with `wait4()`, which returns that child's own resource usage, so each command's CPU time stays its own while they overlap; its wall time runs from its start to its reap. Each command's stdout and stderr go to two temporary files and are printed whole when it ends, with its `[INFO] CMD:` echo and its `[TIME]` line (stdout to stdout, stderr to stderr), so parallel compilers' messages never mix mid-line. After the first failure no new command starts; the running ones are waited for, then the build stops.
- **The summary table** has one row per command. A pool is a row of its own (its elapsed time; CPU: the sum of its commands) with its commands indented below it; their wall times overlap, so the total wall time counts the pool's row, not theirs, while the CPU total counts each command once. The heading says so.

No CPU cycle counts: the developer chose wall and CPU time per command (2026-11-09).

## Output

nob's `[INFO] CMD: ...` echo of each command and the `[TIME]` lines go to stdout, like the script's progress lines (`configuring ...`, `compiling <n> of <m> files ...`, `up to date: ...`, `built: ...`, whose text scripts and people read); warnings and errors go to stderr. A pooled command's echo, output and `[TIME]` line come together when it ends, in the order the commands end. So `./build.sh lsan > /dev/null` (as `build/tools/leakcheck` runs it) stays quiet unless something fails.

## Adding a mode or a flag

- A **flag** goes into the argument list of the command it belongs to. The CFLAGS-like lists are built in `BuildLinux` / `BuildWindows`, in the same order the compiler sees them; keep the `compile_commands.json` entries in step when a list they copy changes.
- A **mode** is a branch in `main()` or `BuildLinux` that picks its output folder and flags.
- Run every command through `RunTimed()` or `RunPool()` (never `nob_cmd_run` directly, never `.async`), so it is timed on its own. A `RunTimed` command never runs while a pool does (`RunPool` returns only after reaping all its children), which keeps `RUSAGE_CHILDREN` exact.
- A **new C file** in `src/` or `runtime/` is found by the glob and gets its object; nothing to add.
