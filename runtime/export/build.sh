#!/usr/bin/env bash
# Build the viewer for this map with the bundled R3D, raylib and assimp (everything is in
# this folder; nothing is downloaded).
#
#   ./build.sh                 Linux x86_64 -> build/linux/viewer
#   ./build.sh --win           Windows x86_64 with MinGW-w64 (cross-compiler
#                              x86_64-w64-mingw32-gcc, e.g. from Linux) -> build/win/viewer.exe
#   ./build.sh [--win] --from-source   build R3D, raylib and assimp from r3d/ even when
#                              prebuilt/ has them
#
# R3D, raylib and assimp: the static libraries in prebuilt/<linux|win>/lib/ (built by monolith
# from r3d/) when the export has them for the target, so only the viewer and the scene runtime
# are compiled. If linking against them fails (a compiler or C library older than the one they
# were built with), or with --from-source, they are built from r3d/ with CMake into
# build/r3d-linux/ (or build/r3d-mingw/): a few minutes the first time, later runs only rebuild
# what changed. Delete those folders to force a fresh configure.
#
# The map package is copied next to the program, so build/linux/ (or build/win/) can be
# moved anywhere as it is.
#
# Needs: a C compiler and the C++ runtime (assimp is C++); on Linux the OpenGL / X11
# libraries. Building from source also needs cmake (3.22 or newer), python3 (R3D turns its
# shaders into C headers at build time), a C++ compiler and on Linux the X11 / OpenGL
# development headers raylib needs (Debian / Ubuntu: libx11-dev libxrandr-dev libxinerama-dev
# libxcursor-dev libxi-dev libgl1-mesa-dev; Fedora: libX11-devel libXrandr-devel
# libXinerama-devel libXcursor-devel libXi-devel mesa-libGL-devel; Arch: libx11 libxrandr
# libxinerama libxcursor libxi mesa).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAP="@MAP@"
R3D="$HERE/r3d"

WIN=0
FROM_SOURCE=0
for a in "$@"; do
  case "$a" in
    --win) WIN=1 ;;
    --from-source) FROM_SOURCE=1 ;;
    *) echo "usage: ./build.sh [--win] [--from-source]" >&2; exit 2 ;;
  esac
done

# R3D with its vendored raylib and assimp, as static libraries. MS_PATCHED_R3D (below) tells
# the scene runtime that this is the patched R3D (r3d/patches.md).
CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
  -DR3D_RAYLIB_VENDORED=ON -DR3D_ASSIMP_VENDORED=ON -DASSIMP_BUILD_ZLIB=ON
  -DR3D_BUILD_EXAMPLES=OFF -DR3D_WARNINGS_AS_ERRORS=OFF
)

# BuildR3D <build dir> [cmake configure args...]: configure once, then an incremental build
BuildR3D() {
  local dir="$1"
  shift
  mkdir -p "$dir"
  if [[ ! -f "$dir/CMakeCache.txt" ]]; then
    echo "configuring R3D in ${dir#"$HERE/"} ..."
    cmake -S "$R3D" -B "$dir" "$@" > "$dir/configure.log" 2>&1 || { cat "$dir/configure.log"; exit 1; }
  fi
  echo "building R3D, raylib and assimp from source (log: ${dir#"$HERE/"}/build.log) ..."
  cmake --build "$dir" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" > "$dir/build.log" 2>&1 \
    || { tail -50 "$dir/build.log"; exit 1; }
}

# The public R3D headers and raylib's (the same files raylib installs)
CFLAGS=(-std=c11 -O2 -Wall -Wextra -Wno-missing-field-initializers -DMS_PATCHED_R3D
        -I"$HERE" -I"$R3D/include" -I"$R3D/external/raylib/src")
SOURCES=("$HERE/viewer.c" "$HERE/monolith_scene.c")

if [[ $WIN == 1 ]]; then
  TARGET=win
  OUT="$HERE/build/win"
  EXE="$OUT/viewer.exe"
  # -static: libgcc, libstdc++ (assimp) and winpthread go into the .exe too, leaving only
  # system DLLs (opengl32, gdi32, winmm, ...)
  Link() {  # Link <lib dir>
    x86_64-w64-mingw32-gcc "${CFLAGS[@]}" "${SOURCES[@]}" -o "$EXE" \
      -s -static -L"$1" -lr3d -lraylib -lassimp -lzlibstatic -lstdc++ -lopengl32 -lgdi32 -lwinmm -lshell32 -lm
  }
  SOURCE_LIBS="$HERE/build/r3d-mingw"
  SOURCE_ARGS=(-DCMAKE_TOOLCHAIN_FILE="$HERE/cmake/mingw-w64-x86_64.cmake")
else
  TARGET=linux
  OUT="$HERE/build/linux"
  EXE="$OUT/viewer"
  Link() {
    "${CC:-cc}" "${CFLAGS[@]}" "${SOURCES[@]}" -o "$EXE" \
      -L"$1" -lr3d -lraylib -lassimp -lzlibstatic -lstdc++ -lGL -lm -lpthread -ldl
  }
  SOURCE_LIBS="$HERE/build/r3d-linux"
  # ASSIMP_BUILD_MINIZIP: assimp's bundled unzip (a system minizip can be found whose
  # headers aren't where assimp looks)
  SOURCE_ARGS=(-DASSIMP_BUILD_MINIZIP=ON)
fi
mkdir -p "$OUT"

PREBUILT="$HERE/prebuilt/$TARGET/lib"
linked=0
if [[ $FROM_SOURCE == 0 && -f "$PREBUILT/libr3d.a" ]]; then
  echo "linking against the prebuilt libraries (prebuilt/$TARGET/lib) ..."
  if Link "$PREBUILT"; then
    linked=1
  else
    echo "linking against the prebuilt libraries failed (see above): building them from r3d/ instead" >&2
  fi
fi
if [[ $linked == 0 ]]; then
  BuildR3D "$SOURCE_LIBS" "${CMAKE_ARGS[@]}" "${SOURCE_ARGS[@]}"
  Link "$SOURCE_LIBS/lib"
fi

mkdir -p "$OUT/$MAP"
cp -R "$HERE/$MAP/." "$OUT/$MAP/"
echo "built: ${EXE#"$HERE/"} (run it; the map package is next to it)"
