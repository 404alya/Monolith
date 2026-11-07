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
#        ./build.sh profile        -> CPU-profiling build in build/linux-profile/: -O2 -g
#                                     -fno-omit-frame-pointer, with R3D/raylib/assimp as
#                                     RelWithDebInfo + frame pointers in $R3D_DIR/build-linux-profile
#                                     (so build-linux stays Release), plus the profiling tool
#                                     build/linux-profile/profile (tools/profile.c, libc only)
#                                     The plain build (and run) also writes compile_commands.json
#                                     in the repo root for clangd (see WriteCompileCommands below)
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
# The vendored xatlas (src/vendor/xatlas/, MIT; C++, the paint unwrap) is compiled once per build
# folder into <build folder>-obj/xatlas.o with its own flags (Xatlas below) and linked in.
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

# Xatlas <c++ compiler> <object> <flags...>: compiles the vendored xatlas (src/vendor/xatlas/, MIT;
# the editor's paint unwrap, src/paintunwrap.c, through its C API) into <object> when the object is
# missing, older than the source or this script, or was built with other flags. It is not our code:
# it gets -w (no warnings of its own), -DNDEBUG (no debug checks or debug files),
# XA_MULTITHREADED=0 (no threads: the same model always gives the same unwrap) and its C API.
Xatlas() {
  local cxx="$1" obj="$2"
  shift 2
  local dir="$ROOT/src/vendor/xatlas"
  local flags=(-std=c++14 -w -DNDEBUG -DXATLAS_C_API=1 -DXA_MULTITHREADED=0 "$@")
  local stamp="$obj.flags"
  mkdir -p "$(dirname "$obj")"
  if [[ ! -f "$obj" || "$dir/xatlas.cpp" -nt "$obj" || "$dir/xatlas.h" -nt "$obj" || "$dir/xatlas_c.h" -nt "$obj" ||
        "$ROOT/build.sh" -nt "$obj" || ! -f "$stamp" || "$(cat "$stamp")" != "$cxx ${flags[*]}" ]]; then
    echo "compiling xatlas ($cxx) ..."
    "$cxx" "${flags[@]}" -c "$dir/xatlas.cpp" -o "$obj"
    printf '%s\n' "$cxx ${flags[*]}" > "$stamp"
  fi
}

# WriteCompileCommands <R3D build dir> <entry>...: writes compile_commands.json in the repo root, the
# compilation database clangd (and other tools) read for go-to-definition, references and diagnostics.
# Each <entry> is "@@" <compiler> <args...> <source file>: the arguments the file is compiled with,
# with no shell in between (the path defines' quotes stay as they are). R3D's own database (from
# CMAKE_EXPORT_COMPILE_COMMANDS) is merged in with assimp's entries dropped (C++, huge, not needed to
# follow calls into R3D and raylib). Machine-specific absolute paths: the file is gitignored. Written
# to a temporary file and renamed, so clangd never reads half a file; without python3 it is skipped.
WriteCompileCommands() {
  local r3dBuild="$1"
  shift
  if ! command -v python3 >/dev/null; then
    echo "build.sh: python3 not found: compile_commands.json not written" >&2
    return 0
  fi
  python3 -I -c '
import json, os, sys
root, out, r3dDb, assimp = sys.argv[1:5]
db, cur = [], None
for a in sys.argv[5:]:
    if a == "@@":
        cur = []
        db.append(cur)
    else:
        cur.append(a)
db = [{"directory": root, "file": e[-1], "arguments": e[:-1] + ["-c", e[-1]]} for e in db]
if os.path.isfile(r3dDb):
    with open(r3dDb) as f:
        for e in json.load(f):
            path = os.path.normpath(os.path.join(e["directory"], e["file"]))
            if not path.startswith(assimp):
                db.append(e)
else:
    print("build.sh: " + r3dDb + " not found: compile_commands.json has no R3D or raylib entries", file=sys.stderr)
with open(out + ".tmp", "w") as f:
    json.dump(db, f, indent=1)
    f.write("\n")
os.replace(out + ".tmp", out)
' "$ROOT" "$ROOT/compile_commands.json" "$r3dBuild/compile_commands.json" \
    "$(realpath -m "$R3D_DIR/external/assimp")/" "$@"
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
  XATLAS_OBJ="$ROOT/build/win-obj/xatlas.o"
  Xatlas x86_64-w64-mingw32-g++ "$XATLAS_OBJ" -O2
  # -static: libgcc, libstdc++ (assimp) and winpthread are linked in as well,
  # leaving only system DLLs (opengl32, gdi32, winmm, kernel32, ...) as imports.
  "$WIN_CC" -std=c11 -Wall -Wextra -Wno-missing-field-initializers -O2 \
    -I"$R3D_DIR/include" \
    -I"$R3D_WIN/generated/include" \
    -I"$R3D_WIN/external/raylib/raylib/include" \
    -I"$R3D_DIR/external/assimp/include" -I"$R3D_WIN/external/assimp/include" \
    -I"$R3D_DIR/external/assimp/contrib/zlib" -I"$R3D_WIN/external/assimp/contrib/zlib" \
    -DR3D_BUILD_TYPE_NAME='"Release"' \
    -I"$ROOT/runtime" -DMS_PATCHED_R3D -DMONOLITH_EDITOR \
    "$ROOT"/src/*.c "$ROOT"/runtime/*.c "$XATLAS_OBJ" -o "$OUT/monolith.exe" \
    -s -static -L"$R3D_WIN/lib" -lr3d -lraylib -lassimp -lzlibstatic -lstdc++ \
    -lopengl32 -lgdi32 -lwinmm -lshell32 -lm

  # Map packages are found next to the .exe (see InitDataPaths in src/main.c); resources/ is
  # from builds before map packages (the shaders now live in the packages), runtime/ from
  # builds before --export needed the repo (src/export.c)
  rm -rf "$OUT/resources" "$OUT/maps" "$OUT/runtime"
  cp -r "$ROOT/maps" "$OUT/maps"
  # The editor's fonts are compiled in (src/font_outfit.h, src/font_pixelify.h); the OFL wants their licences next to them
  cp "$ROOT/resources/fonts/outfit/OFL.txt" "$OUT/OFL-Outfit.txt"
  cp "$ROOT/resources/fonts/pixelify/OFL.txt" "$OUT/OFL-PixelifySans.txt"

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
# The profile build has its own R3D dir: RelWithDebInfo (-O2 -g) with frame pointers, so
# perf --call-graph fp unwinds through R3D, raylib and assimp (tools/profile.c).
# CMAKE_EXPORT_COMPILE_COMMANDS: R3D's and raylib's entries for our compile_commands.json (no
# change to how anything compiles).
MODE="${1:-}"
R3D_LINUX="$R3D_DIR/build-linux"
R3D_EXTRA=()
if [[ "$MODE" == "profile" ]]; then
  R3D_LINUX="$R3D_DIR/build-linux-profile"
  R3D_BUILD_TYPE=RelWithDebInfo
  R3D_EXTRA=(-DCMAKE_C_FLAGS="-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer"
             -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer")
fi
R3D_BUILD_TYPE="${R3D_BUILD_TYPE:-Release}"
[[ "$R3D_BUILD_TYPE" == "None" ]] && R3D_CMAKE_TYPE="" || R3D_CMAKE_TYPE="$R3D_BUILD_TYPE"
BuildR3D "$R3D_LINUX" "Linux ($R3D_BUILD_TYPE)" \
  -DCMAKE_BUILD_TYPE="$R3D_CMAKE_TYPE" "${R3D_EXTRA[@]}" \
  -DBUILD_SHARED_LIBS=OFF -DR3D_RAYLIB_VENDORED=ON -DR3D_ASSIMP_VENDORED=ON \
  -DASSIMP_BUILD_ZLIB=ON -DASSIMP_BUILD_MINIZIP=ON -DR3D_BUILD_EXAMPLES=OFF \
  -DR3D_GPU_PROFILER=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

OUT="$ROOT/build/linux"
INCLUDES=(
  -I"$R3D_DIR/include"
  -I"$R3D_LINUX/generated/include"
  -I"$R3D_LINUX/external/raylib/raylib/include"
  # assimp's headers: --inspect-model (src/inspect.c) reads model files with assimp directly
  -I"$R3D_DIR/external/assimp/include"
  -I"$R3D_LINUX/external/assimp/include"
  # assimp's zlib: the PNG writer (src/png.c)
  -I"$R3D_DIR/external/assimp/contrib/zlib"
  -I"$R3D_LINUX/external/assimp/contrib/zlib"
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
elif [[ "$MODE" == "profile" ]]; then
  CFLAGS=(-std=c11 -Wall -Wextra -Wno-missing-field-initializers -O2 -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer)
  OUT="$ROOT/build/linux-profile"
fi

mkdir -p "$OUT"
# xatlas (C++) with this build's optimisation and sanitizer flags, one object per build folder
case "$CC" in
  *clang*) CXX="${CXX:-${CC/clang/clang++}}" ;;
  *gcc*) CXX="${CXX:-${CC/gcc/g++}}" ;;
  *) CXX="${CXX:-c++}" ;;
esac
XATLAS_FLAGS=()
for f in "${CFLAGS[@]}"; do [[ "$f" == -std=* || "$f" == -W* ]] || XATLAS_FLAGS+=("$f"); done
# (UBSan's -fsanitize=function would stop at xatlas's OpenNL part, which calls matrix destructors
# through a pointer of the base struct type: UB by the letter, ABI-compatible; clang only)
[[ "$MODE" == debug && "$CXX" == *clang* ]] && XATLAS_FLAGS+=(-fno-sanitize=function)
XATLAS_OBJ="$OUT-obj/xatlas.o"
Xatlas "$CXX" "$XATLAS_OBJ" "${XATLAS_FLAGS[@]}"
# libstdc++ (needed by assimp) is linked statically as well: the host's libstdc++ is
# usually newer than other distros' and would fail to load there. libgcc_s stays
# shared (every distro has it; the static unwinder would need glibc 2.35).
# --wrap: route symbols whose newest glibc version is newer than 2.34 to older
# versions, see src/glibc_compat.c. The result runs on glibc >= 2.34.
# The baked-in repo paths are only used while they exist (see InitDataPaths in src/main.c).
GLIBC_WRAP=acosf,asinf,atan2f,sqrtf,fmod,fmodf,hypot,hypotf,__isoc23_strtoul,__isoc23_strtol,arc4random
DEFINES=(
  -DBENCHMARK_PATH="\"$ROOT/benchmark/\""
  -DMAPS_PATH="\"$ROOT/maps/\"" -DPROJECT_PATH="\"$ROOT/\"" -DR3D_SOURCE_PATH="\"$R3D_DIR/\""
  -DR3D_BUILD_TYPE_NAME="\"$R3D_BUILD_TYPE\""
  -I"$ROOT/runtime" -DMS_PATCHED_R3D -DMONOLITH_EDITOR
)
"$CC" "${CFLAGS[@]}" "${INCLUDES[@]}" "${DEFINES[@]}" \
  "$ROOT"/src/*.c "$ROOT"/runtime/*.c "$XATLAS_OBJ" -o "$OUT/monolith" \
  -L"$R3D_LINUX/lib" -lr3d -lraylib -lassimp -lzlibstatic \
  -l:libstdc++.a -lgcc_s -lGL -lm -lpthread -ldl -Wl,--wrap="${GLIBC_WRAP//,/,--wrap=}"

rm -rf "$OUT/resources" "$OUT/maps" "$OUT/runtime"
cp -r "$ROOT/maps" "$OUT/maps"
# The editor's fonts are compiled in (src/font_outfit.h, src/font_pixelify.h); the OFL wants their licences next to them
cp "$ROOT/resources/fonts/outfit/OFL.txt" "$OUT/OFL-Outfit.txt"
cp "$ROOT/resources/fonts/pixelify/OFL.txt" "$OUT/OFL-PixelifySans.txt"

echo "built: ${OUT#"$ROOT/"}/monolith"

# clangd's compilation database, from the arrays above (the plain build only: the sanitizer and
# profile builds and --win leave it alone). The game's files with the game's flags; tools/profile.c,
# xatlas, the examples and tests/unit with the flags their own scripts use (examples/build.sh,
# tests/unit/*.sh: keep these lines in step when those change). One entry per file: 04_import_source
# is listed as the plain program, not its capture-shim build.
if [[ "$MODE" == "" || "$MODE" == "run" ]]; then
  ENTRIES=()
  for f in "$ROOT"/src/*.c "$ROOT"/runtime/*.c; do
    ENTRIES+=(@@ "$CC" "${CFLAGS[@]}" "${INCLUDES[@]}" "${DEFINES[@]}" "$f")
  done
  ENTRIES+=(@@ "$CXX" -std=c++14 -w -DNDEBUG -DXATLAS_C_API=1 -DXA_MULTITHREADED=0 "${XATLAS_FLAGS[@]}"
            "$ROOT/src/vendor/xatlas/xatlas.cpp")
  ENTRIES+=(@@ "$CC" -std=c11 -Wall -Wextra -Wno-missing-field-initializers -O2 -g "$ROOT/tools/profile.c")
  EX_FLAGS=("$CC" "${CFLAGS[@]}" -I"$R3D_DIR/include" -I"$R3D_LINUX/external/raylib/raylib/include"
            -I"$ROOT/runtime" -DMS_PATCHED_R3D)
  ENTRIES+=(@@ "${EX_FLAGS[@]}" "$ROOT/examples/01_minimal/main.c")
  ENTRIES+=(@@ "${EX_FLAGS[@]}" -DHARNESS_MAIN_NO_ARGS "$ROOT/examples/02_existing_project/before.c")
  ENTRIES+=(@@ "${EX_FLAGS[@]}" "$ROOT/examples/02_existing_project/after.c")
  ENTRIES+=(@@ "${EX_FLAGS[@]}" -DMONOLITH_EDITOR "$ROOT/examples/03_live_editing/main.c")
  ENTRIES+=(@@ "${EX_FLAGS[@]}" -DHARNESS_MAIN_NO_ARGS "$ROOT/examples/04_import_source/main.c")
  ENTRIES+=(@@ "${EX_FLAGS[@]}" "$ROOT/examples/05_entities/main.c")
  UNIT_FLAGS=("$CC" -std=c11 -Wall -Wextra -Wno-missing-field-initializers -g -O1 -fno-omit-frame-pointer
              -fsanitize=address,undefined -fno-sanitize-recover=all -D_GNU_SOURCE -DMS_PATCHED_R3D
              -I"$R3D_DIR/include" -I"$R3D_LINUX/external/raylib/raylib/include" -I"$ROOT/runtime")
  ENTRIES+=(@@ "${UNIT_FLAGS[@]}" "$ROOT/tests/unit/compiled_test.c")
  ENTRIES+=(@@ "${UNIT_FLAGS[@]}" -DMONOLITH_EDITOR "$ROOT/tests/unit/parse_error_test.c")
  ENTRIES+=(@@ "${UNIT_FLAGS[@]}" -DMONOLITH_EDITOR -I"$ROOT/src" "$ROOT/tests/unit/collision_test.c")
  ENTRIES+=(@@ "${UNIT_FLAGS[@]}" -DMONOLITH_EDITOR -I"$ROOT/src" "$ROOT/tests/unit/paintunwrap_test.c")
  ENTRIES+=(@@ "$CC" -std=c11 -Wall -Wextra -I"$ROOT/src" "$ROOT/tests/unit/wincmd_test.c")
  WriteCompileCommands "$R3D_LINUX" "${ENTRIES[@]}"
  echo "wrote: compile_commands.json"
fi
# The profiling tool runs perf on the binary next to it. Not a game flag: the game stays as it is.
# Linux only (perf is); plain C, links only libc.
if [[ "$MODE" == "profile" ]]; then
  "$CC" -std=c11 -Wall -Wextra -Wno-missing-field-initializers -O2 -g "$ROOT/tools/profile.c" -o "$OUT/profile"
  echo "built: ${OUT#"$ROOT/"}/profile"
fi
if [[ "${1:-}" == "run" ]]; then
  shift
  exec "$OUT/monolith" "$@"
fi
