#!/usr/bin/env bash
# tests/unit/paintunwrap_test.sh -- the paint unwrap (src/paintunwrap.c + the vendored xatlas) under
# ASan + UBSan (tests/unit/paintunwrap_test.c). Needs ./build.sh first (R3D's libraries).
# Builds xatlas with the sanitizers into build/paintunwrap-test/, the test with the editor runtime
# (MONOLITH_EDITOR: MS_PaintUnwrapWrite / Read, MS_ModelMeshData) and runs it on the synthetic
# meshes and every model file in maps/*/models/ (read by R3D in a hidden window), with --scene on
# the package with the most models. xatlas gets -fno-sanitize=function: its OpenNL part calls
# matrix destructors through a pointer of the base struct type (UB by the letter, ABI-compatible).
# Extra arguments go to the test (e.g. other model files).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
R3D_DIR="${R3D_DIR:-$ROOT/submodules/r3d}"
LIBDIR="$R3D_DIR/build-linux"
OUT="$ROOT/build/paintunwrap-test"
CC="${CC:-clang}"
CXX="${CXX:-clang++}"

[[ -f "$LIBDIR/lib/libr3d.a" ]] || { echo "paintunwrap_test: run ./build.sh first" >&2; exit 1; }
mkdir -p "$OUT/scratch"
SAN=(-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all)
X="$ROOT/src/vendor/xatlas"
if [[ ! -f "$OUT/xatlas.o" || "$X/xatlas.cpp" -nt "$OUT/xatlas.o" || "${BASH_SOURCE[0]}" -nt "$OUT/xatlas.o" ]]; then
  echo "compiling xatlas with ASan + UBSan ..."
  "$CXX" -std=c++14 -w -DNDEBUG -DXATLAS_C_API=1 -DXA_MULTITHREADED=0 "${SAN[@]}" -fno-sanitize=function -c "$X/xatlas.cpp" -o "$OUT/xatlas.o"
fi
"$CC" -std=c11 -Wall -Wextra -Wno-missing-field-initializers "${SAN[@]}" -D_GNU_SOURCE -DMS_PATCHED_R3D -DMONOLITH_EDITOR \
  -I"$R3D_DIR/include" -I"$LIBDIR/external/raylib/raylib/include" -I"$ROOT/runtime" -I"$ROOT/src" \
  "$ROOT/tests/unit/paintunwrap_test.c" "$ROOT/src/paintunwrap.c" "$ROOT/runtime/monolith_scene.c" "$OUT/xatlas.o" \
  -o "$OUT/paintunwrap_test" -L"$LIBDIR/lib" -lr3d -lraylib -lassimp -lzlibstatic -lstdc++ -lGL -lm -lpthread -ldl

MODELS=()
SCENE=()
most=0
for pkg in "$ROOT"/maps/*/; do
  found=0
  for m in "$pkg"models/*.glb "$pkg"models/*.gltf "$pkg"models/*.obj; do
    [[ -f "$m" ]] && { MODELS+=("$m"); found=$((found + 1)); }
  done
  (( found > most )) && { most=$found; SCENE=(--scene "${pkg%/}"); }
done
# (the GPU driver's own allocations at context creation: tools/lsan.supp, any-frame rule)
ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS="suppressions=$ROOT/tools/lsan.supp" "$OUT/paintunwrap_test" --scratch "$OUT/scratch" "${SCENE[@]}" "${MODELS[@]}" "$@"
