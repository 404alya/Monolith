#!/usr/bin/env bash
# tests/unit/msg_test.sh -- the editor's message text (src/msg.c, tests/unit/msg_test.c) under
# ASan + UBSan with leak detection: static vs heap texts, replace, append, long strings, out of memory.
# Plain C + libc: needs no ./build.sh first.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/build/msg-test"
CC="${CC:-clang}"
mkdir -p "$OUT"
"$CC" -std=c11 -Wall -Wextra -Wno-missing-field-initializers -g -O1 -fno-omit-frame-pointer \
  -fsanitize=address,undefined -fno-sanitize-recover=all -D_GNU_SOURCE -I"$ROOT/src" \
  "$ROOT/tests/unit/msg_test.c" -o "$OUT/msg_test"
ASAN_OPTIONS=detect_leaks=1 "$OUT/msg_test"
