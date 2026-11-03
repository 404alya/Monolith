#!/usr/bin/env bash
# tests/unit/parse_error_test.sh -- error messages of the map-text parser under ASan + UBSan
# (tests/unit/parse_error_test.c): long bad tokens arrive whole, small buffers get a clean cut, short
# messages keep their text. Needs ./build.sh first (R3D's libraries). Builds the test against the
# runtime as the editor has it (MONOLITH_EDITOR: the parser) and runs it in build/parse-error-test/.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
R3D_DIR="${R3D_DIR:-$ROOT/submodules/r3d}"
LIBDIR="$R3D_DIR/build-linux"
OUT="$ROOT/build/parse-error-test"
CC="${CC:-clang}"

[[ -f "$LIBDIR/lib/libr3d.a" ]] || { echo "parse_error_test: run ./build.sh first" >&2; exit 1; }
mkdir -p "$OUT"
"$CC" -std=c11 -Wall -Wextra -Wno-missing-field-initializers -g -O1 -fno-omit-frame-pointer \
  -fsanitize=address,undefined -fno-sanitize-recover=all -D_GNU_SOURCE -DMS_PATCHED_R3D -DMONOLITH_EDITOR \
  -I"$R3D_DIR/include" -I"$LIBDIR/external/raylib/raylib/include" -I"$ROOT/runtime" \
  "$ROOT/tests/unit/parse_error_test.c" "$ROOT/runtime/monolith_scene.c" -o "$OUT/parse_error_test" \
  -L"$LIBDIR/lib" -lr3d -lraylib -lassimp -lzlibstatic -lstdc++ -lGL -lm -lpthread -ldl
ASAN_OPTIONS=detect_leaks=1 "$OUT/parse_error_test" "$OUT"
