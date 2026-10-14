#!/usr/bin/env bash
# Build the examples: examples/build.sh          -> examples/build/linux/<example>/
#                     examples/build.sh --win    -> examples/build/win/<example>/ (MinGW-w64)
#                     examples/build.sh lsan     -> examples/build/linux-lsan/<example>/ (LeakSanitizer,
#                                                   -O1 -g; tools/leakcheck.sh runs these)
# Each output folder holds the program(s) and the data they load (scene/ or assets/), so it
# runs from anywhere: e.g. examples/build/linux/01_minimal/minimal --shot /tmp/x.png
#
# Builds against monolith's R3D copy (submodules/r3d, with -DMS_PATCHED_R3D) and its static
# libraries, which ../build.sh builds (run it once first, or ../build.sh --win for --win).
# Same compiler flags as the game (zero warnings), same static linking and glibc
# compatibility shim (src/glibc_compat.c) on Linux. R3D_DIR overrides the R3D tree. The
# runtime is built with -DMONOLITH_EDITOR like in the game (03_live_editing uses MS_Poll,
# the capture shim MS_WriteR3DEnvironment).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
R3D_DIR="${R3D_DIR:-$ROOT/submodules/r3d}"

WIN=0
[[ "${1:-}" == "--win" ]] && WIN=1
LSAN=0
[[ "${1:-}" == "lsan" ]] && LSAN=1

CFLAGS=(-std=c11 -Wall -Wextra -Wno-missing-field-initializers -O2)
[[ $LSAN == 1 ]] && CFLAGS=(-std=c11 -Wall -Wextra -Wno-missing-field-initializers -g -O1 -fno-omit-frame-pointer -fsanitize=leak)
RUNTIME=("$ROOT/runtime/monolith_scene.c")
if [[ $WIN == 1 ]]; then
  CC=x86_64-w64-mingw32-gcc
  LIBDIR="$R3D_DIR/build-mingw"
  OUT="$HERE/build/win"
  EXE=.exe
  EXTRA_SRC=()
  LIBS=(-s -static -L"$LIBDIR/lib" -lr3d -lraylib -lassimp -lzlibstatic -lstdc++ -lopengl32 -lgdi32 -lwinmm -lshell32 -lm)
else
  CC="${CC:-clang}"
  LIBDIR="$R3D_DIR/build-linux"
  OUT="$HERE/build/linux"
  [[ $LSAN == 1 ]] && OUT="$HERE/build/linux-lsan"
  EXE=
  # Like ../build.sh: libstdc++ static, symbols newer than glibc 2.34 routed to old versions
  GLIBC_WRAP=acosf,asinf,atan2f,sqrtf,fmod,fmodf,hypot,hypotf,__isoc23_strtoul,arc4random
  EXTRA_SRC=("$ROOT/src/glibc_compat.c")
  LIBS=(-L"$LIBDIR/lib" -lr3d -lraylib -lassimp -lzlibstatic -l:libstdc++.a -lgcc_s -lGL -lm -lpthread -ldl
        -Wl,--wrap="${GLIBC_WRAP//,/,--wrap=}")
fi
if [[ ! -f "$LIBDIR/lib/libr3d.a" ]]; then
  echo "examples/build.sh: $LIBDIR/lib/libr3d.a not found: run ./build.sh$([[ $WIN == 1 ]] && echo ' --win') in the repo root first" >&2
  exit 1
fi
INCLUDES=(-I"$R3D_DIR/include" -I"$LIBDIR/external/raylib/raylib/include" -I"$ROOT/runtime")

# Example <dir> <program> <sources...> -- builds one program into $OUT/<dir>/
Example() {
  local dir="$1" prog="$2"
  shift 2
  mkdir -p "$OUT/$dir"
  "$CC" "${CFLAGS[@]}" "${INCLUDES[@]}" -DMS_PATCHED_R3D -DMONOLITH_EDITOR "$@" "${EXTRA_SRC[@]}" -o "$OUT/$dir/$prog$EXE" "${LIBS[@]}"
  echo "built: ${OUT#"$ROOT/"}/$dir/$prog$EXE"
}

# Data next to the programs (cp -r into a fresh copy: stale files from older builds stay
# only if they still exist in the source)
Data() {
  local dir="$1" from="$2" to="$3"
  mkdir -p "$OUT/$dir/$to"
  cp -r "$from"/. "$OUT/$dir/$to/"
}

Example 01_minimal minimal "$HERE/01_minimal/main.c" "${RUNTIME[@]}"
Data 01_minimal "$HERE/01_minimal/scene" scene

Example 02_existing_project before "$HERE/02_existing_project/before.c"
Example 02_existing_project after "$HERE/02_existing_project/after.c" "${RUNTIME[@]}"
Data 02_existing_project "$HERE/01_minimal/scene" scene

Example 03_live_editing live_editing "$HERE/03_live_editing/main.c" "${RUNTIME[@]}"
Data 03_live_editing "$HERE/01_minimal/scene" scene

Example 04_import_source import_source "$HERE/04_import_source/main.c"
Data 04_import_source "$HERE/04_import_source/assets" assets

# The same program with the capture shim compiled in (runtime/monolith_capture.h), unchanged
# source: -include puts the shim in front of main.c. Two steps, so the shim's implementation
# only goes into main.c, not into the runtime it links. MONOLITH_CAPTURE=<dir> ./import_source_capture
mkdir -p "$OUT/04_import_source"
"$CC" "${CFLAGS[@]}" "${INCLUDES[@]}" -DMS_PATCHED_R3D -DMONOLITH_EDITOR -include monolith_capture.h -DMONOLITH_CAPTURE_IMPLEMENTATION \
  -c "$HERE/04_import_source/main.c" -o "$OUT/04_import_source/main-capture.o"
Example 04_import_source import_source_capture "$OUT/04_import_source/main-capture.o" "${RUNTIME[@]}"
