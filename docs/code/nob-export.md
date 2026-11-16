# `runtime/export/nob.c`

The build program of an exported folder ([export.md](export.md)). `--export` copies it into every export as `nob.c`, with the map's name filled in (`MAP_NAME`). This page is part of the monolith repository only; it is not copied into exports.

`./build.sh` compiles it into `build/nob` (when that is missing or older than `nob.c` or `nob.h`) and runs it with its own arguments; `build/nob` run directly rebuilds itself the same way. It is written on nob.h (next to it in the export: a single-header C library by Alexey Kutepov, MIT licence or public domain, `LICENSES/nob.txt`), which runs the commands.

## Usage

| Command | Builds |
|---|---|
| `./build.sh` | Linux x86_64 → `build/linux/viewer` |
| `./build.sh --win` | Windows x86_64 with MinGW-w64 (the cross-compiler `x86_64-w64-mingw32-gcc`, e.g. from Linux) → `build/win/viewer.exe` |
| `./build.sh [--win] --from-source` | Builds R3D, raylib and assimp from `r3d/` even when `prebuilt/` has them |

Any other argument prints the usage and exits 2.

## R3D, raylib and assimp

- When the folder has them for the target, the static libraries in `prebuilt/<linux|win>/lib/` (built by monolith from `r3d/`) are used, so only the viewer and the scene runtime are compiled.
- If linking against them fails (a compiler or C library older than the one they were built with), or with `--from-source`, they are built from `r3d/` with CMake into `build/r3d-linux/` (or `build/r3d-mingw/`): a few minutes the first time; later runs only rebuild what changed. Delete those folders to force a fresh configure.

The map package is copied next to the program, so `build/linux/` (or `build/win/`) can be moved anywhere as it is.

## Needs

- A C compiler and the C++ runtime (assimp is C++); on Linux the OpenGL / X11 libraries.
- Building from source also needs cmake (3.22 or newer), python3 (R3D turns its shaders into C headers at build time), a C++ compiler and, on Linux, the X11 / OpenGL development headers raylib needs:

| Distribution | Packages |
|---|---|
| Debian / Ubuntu | `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev` |
| Fedora | `libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel mesa-libGL-devel` |
| Arch | `libx11 libxrandr libxinerama libxcursor libxi mesa` |

## Environment

`CC`: the Linux C compiler (default `cc`); it also compiles this program. `--win` always uses `x86_64-w64-mingw32-gcc`.

## Output

- The progress lines (`linking against ...`, `configuring R3D ...`, `building R3D ...`, `built: ...`) on stdout.
- The compiler's own messages as it prints them.
- A failed CMake step prints its log: the configure log whole, the last 50 lines of the build log.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | built |
| 1 | a step failed |
| 2 | usage |
