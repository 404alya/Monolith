#!/usr/bin/env bash
# Build: ./build.sh                -> Linux x86_64 build in build/linux/:
#                                     monolith (R3D, raylib, assimp and libstdc++ linked
#                                     statically) + maps/ (map packages), the folder can be
#                                     copied as-is
#        ./build.sh run [args]     -> build and run
#        ./build.sh debug          -> debug build (-g -O0, AddressSanitizer + UBSan, leak check at
#                                     exit) in build/linux-debug/
#        ./build.sh lsan           -> LeakSanitizer-only build (-O1 -g, fast) in build/linux-lsan/,
#                                     what tools/leakcheck.sh runs
#        ./build.sh --win          -> Windows x86_64 build in build/win/ (MinGW-w64):
#                                     monolith.exe (statically linked) + maps/ (packages),
#                                     the folder can be zipped and copied as-is
#        ./build.sh --win run [args]  -> Windows build, then run it under wine
# R3D is vendored in submodules/r3d (with local fixes, see submodules/r3d/patches.md)
# and built on first use as static libs: into $R3D_DIR/build-linux for Linux and into
# $R3D_DIR/build-mingw for --win (toolchain: cmake/mingw-w64-x86_64.cmake). Later runs
# only rebuild what changed; if the configure options changed (e.g. R3D_BUILD_TYPE),
# the build dir is re-configured in place (a full R3D rebuild). Delete those
# directories to force a fresh configure.
# R3D_DIR can be overridden: R3D_DIR=/path/to/r3d ./build.sh
# The runtime (runtime/monolith_scene.c) is built with -DMONOLITH_EDITOR: hot reload, source
# lines and the writers; --export writes it without them (src/export.c).
# R3D_BUILD_TYPE (Linux only): CMake build type of R3D/raylib/assimp, default Release.
#   R3D_BUILD_TYPE=None ./build.sh  -> the old unoptimised build (no CMAKE_BUILD_TYPE),
#   only for comparisons with benchmark reports from before 2026-10-10.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R3D_DIR="${R3D_DIR:-$ROOT/submodules/r3d}"
CC="${CC:-clang}"

# BuildR3D <build dir> <description> [cmake configure args...]
# Configures once, then runs an incremental build; output goes to <build dir>/monolith-build.log.
# The configure args are remembered in <build dir>/monolith-configure.args; when they
# differ (or a dir from before that file existed is found), cmake is re-run on the
# existing dir, which rebuilds what the new options affect (all of it for a new build type).
BuildR3D() {
  local dir="$1" what="$2"
  shift 2
  local log="$dir/monolith-build.log"
  local argsFile="$dir/monolith-configure.args"
  local args
  args="$(printf '%s\n' "$@")"
  mkdir -p "$dir"
  if [[ ! -f "$dir/CMakeCache.txt" || ! -f "$argsFile" || "$(cat "$argsFile")" != "$args" ]]; then
    if [[ -f "$dir/CMakeCache.txt" ]]; then
      echo "R3D configure options changed: re-configuring $dir (a new build type rebuilds all of R3D, raylib and assimp: minutes) ..."
    else
      echo "configuring R3D for $what in $dir ..."
    fi
    rm -f "$argsFile"
    cmake -S "$R3D_DIR" -B "$dir" "$@" > "$log" 2>&1 || { cat "$log"; exit 1; }
    printf '%s\n' "$args" > "$argsFile"
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
  if [[ "${1:-}" == "debug" || "${1:-}" == "lsan" ]]; then
    echo "build.sh: --win ${1} is not supported (the sanitizers don't work with MinGW)." >&2
    echo "          Use ./build.sh ${1} for a sanitizer build on Linux." >&2
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
    -DR3D_BUILD_EXAMPLES=OFF -DR3D_GPU_PROFILER=ON

  mkdir -p "$OUT"
  # -static: libgcc, libstdc++ (assimp) and winpthread are linked in as well,
  # leaving only system DLLs (opengl32, gdi32, winmm, kernel32, ...) as imports.
  "$WIN_CC" -std=c11 -Wall -Wextra -Wno-missing-field-initializers -O2 \
    -I"$R3D_DIR/include" \
    -I"$R3D_WIN/generated/include" \
    -I"$R3D_WIN/external/raylib/raylib/include" \
    -I"$R3D_DIR/external/assimp/include" -I"$R3D_WIN/external/assimp/include" \
    -DR3D_BUILD_TYPE_NAME='"Release"' \
    -I"$ROOT/runtime" -DMS_PATCHED_R3D -DMONOLITH_EDITOR \
    "$ROOT"/src/*.c "$ROOT"/runtime/*.c -o "$OUT/monolith.exe" \
    -s -static -L"$R3D_WIN/lib" -lr3d -lraylib -lassimp -lzlibstatic -lstdc++ \
    -lopengl32 -lgdi32 -lwinmm -lshell32 -lm

  # Map packages are found next to the .exe (see InitDataPaths in src/main.c); resources/ is
  # from builds before map packages (the shaders now live in the packages), runtime/ from
  # builds before --export needed the repo (src/export.c)
  rm -rf "$OUT/resources" "$OUT/maps" "$OUT/runtime"
  cp -r "$ROOT/maps" "$OUT/maps"

  echo "built: build/win/monolith.exe"
  if [[ "${1:-}" == "run" ]]; then
    shift
    exec wine "$OUT/monolith.exe" "$@"
  fi
  exit 0
fi

# Static libs, so the executable needs nothing beyond what every desktop Linux has
# (glibc, libm, libGL from the GPU driver). Release since 2026-10-10; before that there
# was no CMAKE_BUILD_TYPE (R3D unoptimised, like the original ../r3d/build), so frame
# times in older benchmark reports aren't comparable (R3D_BUILD_TYPE=None rebuilds that).
# R3D_GPU_PROFILER: per-pass GPU timer queries, off at runtime unless --bench or the F2 panel
# turns them on (see submodules/r3d/patches.md). ASSIMP_BUILD_ZLIB: bundled zlib, linked in too.
# ASSIMP_BUILD_MINIZIP: use assimp's bundled unzip; a system minizip (Arch:
# /usr/include/minizip/) is found by pkg-config but its headers aren't where assimp's
# #include <unzip.h> looks, so the build fails.
R3D_LINUX="$R3D_DIR/build-linux"
R3D_BUILD_TYPE="${R3D_BUILD_TYPE:-Release}"
[[ "$R3D_BUILD_TYPE" == "None" ]] && R3D_CMAKE_TYPE="" || R3D_CMAKE_TYPE="$R3D_BUILD_TYPE"
BuildR3D "$R3D_LINUX" Linux \
  -DCMAKE_BUILD_TYPE="$R3D_CMAKE_TYPE" \
  -DBUILD_SHARED_LIBS=OFF -DR3D_RAYLIB_VENDORED=ON -DR3D_ASSIMP_VENDORED=ON \
  -DASSIMP_BUILD_ZLIB=ON -DASSIMP_BUILD_MINIZIP=ON -DR3D_BUILD_EXAMPLES=OFF \
  -DR3D_GPU_PROFILER=ON

OUT="$ROOT/build/linux"
MODE="${1:-}"
INCLUDES=(
  -I"$R3D_DIR/include"
  -I"$R3D_LINUX/generated/include"
  -I"$R3D_LINUX/external/raylib/raylib/include"
  # assimp's headers: --inspect-model (src/inspect.c) reads model files with assimp directly
  -I"$R3D_DIR/external/assimp/include"
  -I"$R3D_LINUX/external/assimp/include"
)

CFLAGS=(-std=c11 -Wall -Wextra -Wno-missing-field-initializers -O2)
# The sanitizer builds go into their own folders, so they never replace the normal binary.
# R3D, raylib and assimp stay the normal Release static libraries (not instrumented, but
# their mallocs are intercepted, so leaks inside them are found too).
if [[ "$MODE" == "debug" ]]; then
  CFLAGS=(-std=c11 -Wall -Wextra -Wno-missing-field-initializers -g -O0 -fno-omit-frame-pointer -fsanitize=address,undefined)
  OUT="$ROOT/build/linux-debug"
elif [[ "$MODE" == "lsan" ]]; then
  CFLAGS=(-std=c11 -Wall -Wextra -Wno-missing-field-initializers -g -O1 -fno-omit-frame-pointer -fsanitize=leak)
  OUT="$ROOT/build/linux-lsan"
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
  -DBENCHMARK_PATH="\"$ROOT/benchmark/\"" \
  -DMAPS_PATH="\"$ROOT/maps/\"" -DPROJECT_PATH="\"$ROOT/\"" -DR3D_SOURCE_PATH="\"$R3D_DIR/\"" \
  -DR3D_BUILD_TYPE_NAME="\"$R3D_BUILD_TYPE\"" \
  -I"$ROOT/runtime" -DMS_PATCHED_R3D -DMONOLITH_EDITOR \
  "$ROOT"/src/*.c "$ROOT"/runtime/*.c -o "$OUT/monolith" \
  -L"$R3D_LINUX/lib" -lr3d -lraylib -lassimp -lzlibstatic \
  -l:libstdc++.a -lgcc_s -lGL -lm -lpthread -ldl -Wl,--wrap="${GLIBC_WRAP//,/,--wrap=}"

rm -rf "$OUT/resources" "$OUT/maps" "$OUT/runtime"
cp -r "$ROOT/maps" "$OUT/maps"

echo "built: ${OUT#"$ROOT/"}/monolith"
if [[ "${1:-}" == "run" ]]; then
  shift
  exec "$OUT/monolith" "$@"
fi
