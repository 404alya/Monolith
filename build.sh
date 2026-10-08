#!/usr/bin/env bash
# Build: ./build.sh                -> Linux x86_64 build in build/linux/:
#                                     monolith (R3D, raylib, assimp and libstdc++ linked
#                                     statically) + resources/, the folder can be copied as-is
#        ./build.sh run [args]     -> build and run
#        ./build.sh debug          -> debug build (-g -O0 + sanitizers), same output folder
#        ./build.sh --win          -> Windows x86_64 build in build/win/ (MinGW-w64):
#                                     monolith.exe (statically linked) + resources/,
#                                     the folder can be zipped and copied as-is
#        ./build.sh --win run [args]  -> Windows build, then run it under wine
# R3D is vendored in submodules/r3d (with local fixes, see submodules/r3d/patches.md)
# and built on first use as static libs: into $R3D_DIR/build-linux for Linux and into
# $R3D_DIR/build-mingw for --win (toolchain: cmake/mingw-w64-x86_64.cmake). Later runs
# only rebuild what changed. Delete those directories to force a fresh configure.
# R3D_DIR can be overridden: R3D_DIR=/path/to/r3d ./build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R3D_DIR="${R3D_DIR:-$ROOT/submodules/r3d}"
CC="${CC:-clang}"

# BuildR3D <build dir> <description> [cmake configure args...]
# Configures once, then runs an incremental build; output goes to <build dir>/monolith-build.log.
BuildR3D() {
  local dir="$1" what="$2"
  shift 2
  local log="$dir/monolith-build.log"
  mkdir -p "$dir"
  if [[ ! -f "$dir/CMakeCache.txt" ]]; then
    echo "configuring R3D for $what in $dir ..."
    cmake -S "$R3D_DIR" -B "$dir" "$@" > "$log" 2>&1 || { cat "$log"; exit 1; }
  fi
  echo "building R3D for $what (log: $log) ..."
  cmake --build "$dir" -j"$(nproc)" >> "$log" 2>&1 || { tail -50 "$log"; exit 1; }
}

WIN=0
if [[ "${1:-}" == "--win" ]]; then
  WIN=1
  shift
fi

if [[ $WIN == 1 ]]; then
  if [[ "${1:-}" == "debug" ]]; then
    echo "build.sh: --win debug is not supported (AddressSanitizer/UBSan don't work with MinGW)." >&2
    echo "          Use ./build.sh debug for a sanitizer build on Linux." >&2
    exit 1
  fi

  WIN_CC=x86_64-w64-mingw32-gcc
  R3D_WIN="$R3D_DIR/build-mingw"
  OUT="$ROOT/build/win"

  # R3D, raylib and assimp as static libraries, so the .exe needs no DLLs besides
  # the ones every Windows system has.
  BuildR3D "$R3D_WIN" Windows \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64-x86_64.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DR3D_RAYLIB_VENDORED=ON -DR3D_ASSIMP_VENDORED=ON -DASSIMP_BUILD_ZLIB=ON \
    -DR3D_BUILD_EXAMPLES=OFF

  mkdir -p "$OUT"
  # -static: libgcc, libstdc++ (assimp) and winpthread are linked in as well,
  # leaving only system DLLs (opengl32, gdi32, winmm, kernel32, ...) as imports.
  "$WIN_CC" -std=c11 -Wall -Wextra -Wno-missing-field-initializers -O2 \
    -I"$R3D_DIR/include" \
    -I"$R3D_WIN/generated/include" \
    -I"$R3D_WIN/external/raylib/raylib/include" \
    "$ROOT"/src/*.c -o "$OUT/monolith.exe" \
    -s -static -L"$R3D_WIN/lib" -lr3d -lraylib -lassimp -lzlibstatic -lstdc++ \
    -lopengl32 -lgdi32 -lwinmm -lshell32 -lm

  # Resources are found next to the .exe (see InitDataPaths in src/main.c)
  rm -rf "$OUT/resources"
  cp -r "$ROOT/resources" "$OUT/resources"

  echo "built: build/win/monolith.exe"
  if [[ "${1:-}" == "run" ]]; then
    shift
    exec wine "$OUT/monolith.exe" "$@"
  fi
  exit 0
fi

# Static libs, so the executable needs nothing beyond what every desktop Linux has
# (glibc, libm, libGL from the GPU driver). No CMAKE_BUILD_TYPE, like the original
# ../r3d/build (so R3D itself is unoptimised), which keeps benchmarks comparable with
# the earlier reports in benchmark/. ASSIMP_BUILD_ZLIB: bundled zlib, linked in too.
# ASSIMP_BUILD_MINIZIP: use assimp's bundled unzip; a system minizip (Arch:
# /usr/include/minizip/) is found by pkg-config but its headers aren't where assimp's
# #include <unzip.h> looks, so the build fails.
R3D_LINUX="$R3D_DIR/build-linux"
BuildR3D "$R3D_LINUX" Linux \
  -DBUILD_SHARED_LIBS=OFF -DR3D_RAYLIB_VENDORED=ON -DR3D_ASSIMP_VENDORED=ON \
  -DASSIMP_BUILD_ZLIB=ON -DASSIMP_BUILD_MINIZIP=ON -DR3D_BUILD_EXAMPLES=OFF

OUT="$ROOT/build/linux"
INCLUDES=(
  -I"$R3D_DIR/include"
  -I"$R3D_LINUX/generated/include"
  -I"$R3D_LINUX/external/raylib/raylib/include"
)

CFLAGS=(-std=c11 -Wall -Wextra -Wno-missing-field-initializers -O2)
if [[ "${1:-}" == "debug" ]]; then
  CFLAGS=(-std=c11 -Wall -Wextra -Wno-missing-field-initializers -g -O0 -fsanitize=address,undefined)
fi

mkdir -p "$OUT"
# libstdc++ (needed by assimp) is linked statically as well: the host's libstdc++ is
# usually newer than other distros' and would fail to load there. libgcc_s stays
# shared (every distro has it; the static unwinder would need glibc 2.35).
# --wrap: route symbols whose newest glibc version is newer than 2.34 to older
# versions, see src/glibc_compat.c. The result runs on glibc >= 2.34.
# The baked-in repo paths are only used while they exist (see InitDataPaths in src/main.c).
GLIBC_WRAP=acosf,asinf,atan2f,sqrtf,fmod,fmodf,hypot,hypotf,__isoc23_strtoul,arc4random
"$CC" "${CFLAGS[@]}" "${INCLUDES[@]}" \
  -DRESOURCES_PATH="\"$ROOT/resources/\"" -DBENCHMARK_PATH="\"$ROOT/benchmark/\"" \
  "$ROOT"/src/*.c -o "$OUT/monolith" \
  -L"$R3D_LINUX/lib" -lr3d -lraylib -lassimp -lzlibstatic \
  -l:libstdc++.a -lgcc_s -lGL -lm -lpthread -ldl -Wl,--wrap="${GLIBC_WRAP//,/,--wrap=}"

rm -rf "$OUT/resources"
cp -r "$ROOT/resources" "$OUT/resources"

echo "built: build/linux/monolith"
if [[ "${1:-}" == "run" ]]; then
  shift
  exec "$OUT/monolith" "$@"
fi
