#!/usr/bin/env bash
# Build monolith: ./build.sh [run [args] | debug | lsan | profile]  or  ./build.sh --win [run [args]]
# The small programs in tools/ (build/tools/<name>, build/run-sh.o): ./build.sh --tools [name...]
# The examples: ./build.sh examples [--win | test | lsan]; the unit tests: ./build.sh test [name...]
# (every mode, the environment variables R3D_DIR, CC, CXX, R3D_BUILD_TYPE and what each mode builds are
# described at the top of nob.c).
#
# This script is only the launcher: the build itself is the C program nob.c (written on nob.h,
# src/vendor/nob/), which runs every step, prints how long each took (wall and CPU time) and a summary.
# The launcher compiles nob.c into build/nob when that binary is missing or older than nob.c or nob.h,
# then replaces itself with it, passing on all of its arguments.
#
# -e: stop at the first failing command (the compile of nob.c); -u: an unset variable is an error;
# -o pipefail: a failure inside a pipe counts (there is none, kept for safety).
set -euo pipefail

# The repo root: this script's folder, as an absolute path (also when run as ../build.sh or via a
# symlinked folder: `pwd` keeps the path as typed). nob takes the root from its own path, build/nob.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NOB="$ROOT/build/nob"

# Compile the build program when it is missing (a fresh checkout) or older than its source or nob.h.
# The compiler is ${CC:-clang}, the one the game is built with; C11, every warning (nob.c builds with zero),
# -O1 (the build program does little work of its own). It is compiled into a temporary file and moved into
# place, so two builds started at once never run a half-written build/nob. build/ is gitignored.
if [[ ! -x "$NOB" || "$ROOT/nob.c" -nt "$NOB" || "$ROOT/src/vendor/nob/nob.h" -nt "$NOB" ]]; then
  mkdir -p "$ROOT/build"
  "${CC:-clang}" -std=c11 -Wall -Wextra -O1 -o "$NOB.tmp.$$" "$ROOT/nob.c"
  mv -f "$NOB.tmp.$$" "$NOB"
fi

# Run it by its absolute path (it finds the repo root from that) with this script's arguments; `exec`
# replaces this shell, so the exit code (and, for run, the game itself) is nob's.
exec "$NOB" "$@"
