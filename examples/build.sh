#!/usr/bin/env bash
# Build the examples: examples/build.sh          -> examples/build/linux/<example>/
#                     examples/build.sh --win    -> examples/build/win/<example>/ (MinGW-w64)
#                     examples/build.sh test     -> examples/build/linux-test/<example>/ (test build)
#                     examples/build.sh lsan     -> examples/build/linux-lsan/<example>/ (test build with
#                                                   LeakSanitizer, -O1 -g; tools/leakcheck.sh runs these)
# Each output folder holds the program(s) and the data they load (scene/ or assets/), so it
# runs from anywhere.
#
# The examples are plain integration code. The test builds compile each example's main file
# with -include tests/examples/harness.h: the same program then needs --shot <file>, renders 30
# frames in a hidden window, saves the image and exits (it never opens a visible window), e.g.
# examples/build/linux-test/01_minimal/minimal --shot /tmp/x.png --view 2
#
# Builds against monolith's R3D copy (submodules/r3d, with -DMS_PATCHED_R3D) and its static
# libraries, which ../build.sh builds (run it once first, or ../build.sh --win for --win).
# Same compiler flags as the game (zero warnings), same static linking and glibc
# compatibility shim (src/glibc_compat.c) on Linux. R3D_DIR overrides the R3D tree. The runtime
# is built like an exported game (no -DMONOLITH_EDITOR), except for 03_live_editing (MS_Poll)
# and the capture shim (MS_WriteR3DEnvironment). A game build reads only compiled maps, so the
# game-style examples get their scene compiled next to the copied text (<name>.mapb, through
# ../build/linux/monolith --compile, also for --win); 03_live_editing loads the text.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
R3D_DIR="${R3D_DIR:-$ROOT/submodules/r3d}"

WIN=0
[[ "${1:-}" == "--win" ]] && WIN=1
LSAN=0
[[ "${1:-}" == "lsan" ]] && LSAN=1
TEST=$LSAN
[[ "${1:-}" == "test" ]] && TEST=1

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
  [[ $TEST == 1 ]] && OUT="$HERE/build/linux-test"
  [[ $LSAN == 1 ]] && OUT="$HERE/build/linux-lsan"
  EXE=
  # Like ../build.sh: libstdc++ static, symbols newer than glibc 2.34 routed to old versions
  GLIBC_WRAP=acosf,asinf,atan2f,sqrtf,fmod,fmodf,hypot,hypotf,__isoc23_strtoul,__isoc23_strtol,arc4random
  EXTRA_SRC=("$ROOT/src/glibc_compat.c")
  LIBS=(-L"$LIBDIR/lib" -lr3d -lraylib -lassimp -lzlibstatic -l:libstdc++.a -lgcc_s -lGL -lm -lpthread -ldl
        -Wl,--wrap="${GLIBC_WRAP//,/,--wrap=}")
fi
MONOLITH="$ROOT/build/linux/monolith"
if [[ ! -x "$MONOLITH" ]]; then
  echo "examples/build.sh: $MONOLITH not found (it compiles the examples' scenes): run ./build.sh in the repo root first" >&2
  exit 1
fi
if [[ ! -f "$LIBDIR/lib/libr3d.a" ]]; then
  echo "examples/build.sh: $LIBDIR/lib/libr3d.a not found: run ./build.sh$([[ $WIN == 1 ]] && echo ' --win') in the repo root first" >&2
  exit 1
fi
INCLUDES=(-I"$R3D_DIR/include" -I"$LIBDIR/external/raylib/raylib/include" -I"$ROOT/runtime")

# Example <dir> <program> <main file> <flags...> [-- <more sources...>]: the main file is
# compiled on its own (in the test builds with the harness force-included, which must not reach
# the runtime), then linked with the other sources. Flags: -DMONOLITH_EDITOR for the editor
# runtime, -DHARNESS_MAIN_NO_ARGS for a main(void), -include ... for a shim.
HARNESS=()
[[ $TEST == 1 ]] && HARNESS=(-include "$ROOT/tests/examples/harness.h")
Example() {
  local dir="$1" prog="$2" main="$3"
  shift 3
  local flags=() more=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do flags+=("$1"); shift; done
  [[ $# -gt 0 ]] && { shift; more=("$@"); }
  local editor=()
  for f in "${flags[@]}"; do [[ "$f" == -DMONOLITH_EDITOR ]] && editor=(-DMONOLITH_EDITOR); done
  local obj="$OUT-obj/$dir/$prog.o"     # (outside the program folders, which stay copyable)
  mkdir -p "$OUT/$dir" "$OUT-obj/$dir"
  "$CC" "${CFLAGS[@]}" "${INCLUDES[@]}" -DMS_PATCHED_R3D "${flags[@]}" "${HARNESS[@]}" -c "$main" -o "$obj"
  "$CC" "${CFLAGS[@]}" "${INCLUDES[@]}" -DMS_PATCHED_R3D "${editor[@]}" "$obj" "${more[@]}" "${EXTRA_SRC[@]}" \
    -o "$OUT/$dir/$prog$EXE" "${LIBS[@]}"
  echo "built: ${OUT#"$ROOT/"}/$dir/$prog$EXE"
}

# Data next to the programs (cp -r into a fresh copy: stale files from older builds stay
# only if they still exist in the source)
Data() {
  local dir="$1" from="$2" to="$3"
  mkdir -p "$OUT/$dir/$to"
  cp -r "$from"/. "$OUT/$dir/$to/"
}

# Compiled <dir> <package>: compiles the copied package's only .map into <name>.mapb next to it
Compiled() {
  local pkg="$OUT/$1/$2" maps=()
  maps=("$pkg"/*.map)
  [[ ${#maps[@]} == 1 && -f "${maps[0]}" ]] || { echo "examples/build.sh: $pkg must hold exactly one .map file" >&2; exit 1; }
  "$MONOLITH" --map "${maps[0]}" --compile "${maps[0]}b" >"$OUT-obj/compile.log" 2>&1 ||
    { cat "$OUT-obj/compile.log" >&2; exit 1; }
  echo "compiled: ${maps[0]#"$ROOT/"}b"
}

Example 01_minimal minimal "$HERE/01_minimal/main.c" -- "${RUNTIME[@]}"
Data 01_minimal "$HERE/01_minimal/scene" scene
Compiled 01_minimal scene

Example 02_existing_project before "$HERE/02_existing_project/before.c" -DHARNESS_MAIN_NO_ARGS
Example 02_existing_project after "$HERE/02_existing_project/after.c" -- "${RUNTIME[@]}"
Data 02_existing_project "$HERE/01_minimal/scene" scene
Compiled 02_existing_project scene

Example 03_live_editing live_editing "$HERE/03_live_editing/main.c" -DMONOLITH_EDITOR -- "${RUNTIME[@]}"
Data 03_live_editing "$HERE/01_minimal/scene" scene

Example 05_entities entities "$HERE/05_entities/main.c" -- "${RUNTIME[@]}"
Data 05_entities "$HERE/01_minimal/scene" scene
Compiled 05_entities scene

Example 04_import_source import_source "$HERE/04_import_source/main.c" -DHARNESS_MAIN_NO_ARGS
Data 04_import_source "$HERE/04_import_source/assets" assets

# The same program with the capture shim compiled in (runtime/monolith_capture.h), unchanged
# source: -include puts the shim in front of main.c (in the test builds before the harness,
# which then wraps its recorders), and only main.c gets its implementation.
# MONOLITH_CAPTURE=<dir> ./import_source_capture
Example 04_import_source import_source_capture "$HERE/04_import_source/main.c" -DHARNESS_MAIN_NO_ARGS -DMONOLITH_EDITOR \
  -include monolith_capture.h -DMONOLITH_CAPTURE_IMPLEMENTATION -- "${RUNTIME[@]}"
