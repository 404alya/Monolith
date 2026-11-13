#!/usr/bin/env bash
# Build the viewer for this map with the bundled R3D, raylib and assimp (everything is in
# this folder; nothing is downloaded).
#
#   ./build.sh                 Linux x86_64 -> build/linux/viewer
#   ./build.sh --win           Windows x86_64 with MinGW-w64 (cross-compiler
#                              x86_64-w64-mingw32-gcc, e.g. from Linux) -> build/win/viewer.exe
#   ./build.sh [--win] --from-source   build R3D, raylib and assimp from r3d/ even when
#                              prebuilt/ has them
#
# This script is only the launcher: the build itself is the C program nob.c (written on nob.h,
# next to it), whose header comment describes every step (the prebuilt libraries, the fallback
# to building them from r3d/ with CMake, the map package copied next to the program) and what
# a build needs. The launcher compiles nob.c into build/nob with ${CC:-cc} when that program is
# missing or older than nob.c or nob.h, then replaces itself with it, passing on its arguments.
#
# -e: stop at the first failing command (the compile of nob.c); -u: an unset variable is an
# error; -o pipefail: a failure inside a pipe counts (there is none, kept for safety).
set -euo pipefail

# This folder, as an absolute path (also when run as ../build.sh or through a symlinked folder:
# `pwd` keeps the path as typed). nob takes the folder from its own path, build/nob.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NOB="$HERE/build/nob"

# Compile the build program when it is missing or older than its source or nob.h: C11, every
# warning, -O1 (it does little work of its own). It is compiled into a temporary file and
# moved into place, so two builds started at once never run a half-written build/nob.
if [[ ! -x "$NOB" || "$HERE/nob.c" -nt "$NOB" || "$HERE/nob.h" -nt "$NOB" ]]; then
  mkdir -p "$HERE/build"
  "${CC:-cc}" -std=c11 -Wall -Wextra -O1 -o "$NOB.tmp.$$" "$HERE/nob.c"
  mv -f "$NOB.tmp.$$" "$NOB"
fi

# Run it by its absolute path with this script's arguments; `exec` replaces this shell, so the
# exit code is nob's.
exec "$NOB" "$@"
