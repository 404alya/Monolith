#!/usr/bin/env bash
# tests/unit/collision_test.sh -- the walking physics against turned boxes under ASan + UBSan
# (tests/unit/collision_test.c): src/player.c's PlayerStep on a noGpu load of tests/rotated.
# Needs ./build.sh first (R3D's libraries). Builds the test with the editor's runtime (it parses
# the map text) and runs it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
R3D_DIR="${R3D_DIR:-$ROOT/submodules/r3d}"
LIBDIR="$R3D_DIR/build-linux"
OUT="$ROOT/build/collision-test"
CC="${CC:-clang}"

[[ -f "$LIBDIR/lib/libr3d.a" ]] || { echo "collision_test: run ./build.sh first" >&2; exit 1; }
mkdir -p "$OUT"
"$CC" -std=c11 -Wall -Wextra -Wno-missing-field-initializers -g -O1 -fno-omit-frame-pointer \
  -fsanitize=address,undefined -fno-sanitize-recover=all -D_GNU_SOURCE -DMS_PATCHED_R3D -DMONOLITH_EDITOR \
  -I"$R3D_DIR/include" -I"$LIBDIR/external/raylib/raylib/include" -I"$ROOT/runtime" -I"$ROOT/src" \
  "$ROOT/tests/unit/collision_test.c" "$ROOT/src/player.c" "$ROOT/runtime/monolith_scene.c" -o "$OUT/collision_test" \
  -L"$LIBDIR/lib" -lr3d -lraylib -lassimp -lzlibstatic -lstdc++ -lGL -lm -lpthread -ldl
ASAN_OPTIONS=detect_leaks=1 "$OUT/collision_test" "$ROOT/tests/rotated"
