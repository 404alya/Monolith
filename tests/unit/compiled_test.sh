#!/usr/bin/env bash
# tests/unit/compiled_test.sh -- corruption test of the compiled-map reader under ASan + UBSan
# (tests/unit/compiled_test.c). Needs ./build.sh first (build/linux/monolith and R3D's libraries).
# Copies tests/models and tests/rotated (turned boxes) into build/compiled-test/, compiles them with
# monolith --compile, builds the test against the runtime as a game has it (no MONOLITH_EDITOR)
# and runs it on both.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
R3D_DIR="${R3D_DIR:-$ROOT/submodules/r3d}"
LIBDIR="$R3D_DIR/build-linux"
OUT="$ROOT/build/compiled-test"
CC="${CC:-clang}"

[[ -x "$ROOT/build/linux/monolith" && -f "$LIBDIR/lib/libr3d.a" ]] || { echo "compiled_test: run ./build.sh first" >&2; exit 1; }
PACKAGES=(models rotated)
for p in "${PACKAGES[@]}"; do
  mkdir -p "$OUT/$p"
  cp -r "$ROOT/tests/$p/." "$OUT/$p/"     # (refreshes the copy; nothing is removed)
  "$ROOT/build/linux/monolith" --map "$OUT/$p" --compile "$OUT/$p/$p.mapb" >/dev/null 2>"$OUT/compile.log" ||
    { cat "$OUT/compile.log" >&2; exit 1; }
done

"$CC" -std=c11 -Wall -Wextra -Wno-missing-field-initializers -g -O1 -fno-omit-frame-pointer \
  -fsanitize=address,undefined -fno-sanitize-recover=all -D_GNU_SOURCE -DMS_PATCHED_R3D \
  -I"$R3D_DIR/include" -I"$LIBDIR/external/raylib/raylib/include" -I"$ROOT/runtime" \
  "$ROOT/tests/unit/compiled_test.c" "$ROOT/runtime/monolith_scene.c" -o "$OUT/compiled_test" \
  -L"$LIBDIR/lib" -lr3d -lraylib -lassimp -lzlibstatic -lstdc++ -lGL -lm -lpthread -ldl
for p in "${PACKAGES[@]}"; do
  echo "== $p"
  ASAN_OPTIONS=detect_leaks=1 "$OUT/compiled_test" "$OUT/$p"
done
