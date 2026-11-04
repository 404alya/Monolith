#!/usr/bin/env bash
# tools/bench-export-ab.sh -- interleaved A/B benchmark pairs of two exported maps (monolith
# --export), each drawn by its own exported runtime (the game's frame time, not the editor's)
#
#   tools/bench-export-ab.sh <base export> <new export> [--map <name>] [--pairs N] [--frames N]
#                            [--note "what changed"] [--out <dir>] [--dry-run]
#   tools/bench-export-ab.sh --summarize <out dir>
#
# <base export> / <new export>: folders written by `monolith --export` (or F9). The script compiles
# each one's viewer.c with the test harness's bench mode (tests/examples/harness.h, force-included
# into viewer.c only) against that folder's own runtime (monolith_scene.c/.h) and R3D libraries
# (prebuilt/linux/lib, or build/r3d-linux/lib when ./build.sh built them from source), into
# <out>/A/viewer-bench and <out>/B/viewer-bench. Then it runs A, B, A, B, ... (N pairs, default 3,
# at least 3) on the map package in each folder (--map <name>: the package folder; default the one
# folder holding a <name>.mapb), every run a hidden-window bench (`--bench <report>`, 1920 x 1006,
# glFinish per frame, the camera spinning at every viewpoint as in `monolith --bench`) with
# __GL_SYNC_TO_VBLANK=0 and vblank_mode=0, and prints per pair the overall average frame time of A
# and B and their difference, then the mean and median of the paired differences, overall and per
# viewpoint. Reports: <out>/A-<pair>.md, <out>/B-<pair>.md (default <out>: benchmark/export-ab/<stamp>
# in the repo). --frames: timed frames per viewpoint (default 600, the full run; 150 to screen).
# --dry-run builds both binaries and prints the commands without running them. --summarize prints
# the table again from the reports in <out dir>.
#
# A benchmark like any other (the benchmarking skill): one at a time on the machine (one GPU),
# never beside another benchmark or shot, results relative to the base on the same machine.
# Linux only (the bench runs here; a Windows export is built with ./build.sh --win as before).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HARNESS="$ROOT/tests/examples/harness.h"

Usage() { sed -n '4,7p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2; exit 2; }

# Summarize <out dir>: the paired table from <out>/A-<i>.md and B-<i>.md
Summarize() {
  local out="$1"
  python3 -I - "$out" <<'PY'
import glob, os, re, statistics, sys
out = sys.argv[1]

def results(path):
    """{view: avg ms} and the overall avg ms from a bench report's Results table"""
    views, total = {}, None
    rows = False
    for line in open(path, encoding="utf-8"):
        if line.startswith("## Results"):
            rows = True
            continue
        if rows and line.startswith("## "):
            break
        if not rows or not line.startswith("| ") or line.startswith("| View") or line.startswith("|---"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        name, avg = cells[0], float(cells[1].strip("*"))
        if name == "**All**":
            total = avg
        else:
            views[name] = avg
    return views, total

pairs = []
for a in sorted(glob.glob(os.path.join(out, "A-*.md")), key=lambda p: int(re.findall(r"A-(\d+)\.md$", p)[0])):
    i = re.findall(r"A-(\d+)\.md$", a)[0]
    b = os.path.join(out, f"B-{i}.md")
    if os.path.exists(b):
        pairs.append((int(i), results(a), results(b)))
if not pairs:
    sys.exit(f"bench-export-ab: no A-<n>.md / B-<n>.md pairs in {out}")

print(f"\n{'pair':>4} {'A ms':>9} {'B ms':>9} {'B-A ms':>9} {'B-A %':>8}")
diffs, rel = [], []
for i, (av, at), (bv, bt) in pairs:
    d = bt - at
    diffs.append(d)
    rel.append(d / at * 100.0)
    print(f"{i:>4} {at:9.3f} {bt:9.3f} {d:+9.3f} {d / at * 100.0:+7.2f}%")
med = statistics.median(diffs)
print(f"\noverall: mean of paired differences {statistics.mean(diffs):+.3f} ms ({statistics.mean(rel):+.2f} %), "
      f"median {med:+.3f} ms ({statistics.median(rel):+.2f} %), range {min(diffs):+.3f} .. {max(diffs):+.3f} ms, "
      f"{len(pairs)} pairs")
print(f"\n{'view':<24} {'A ms (mean)':>12} {'B-A ms (mean)':>14} {'B-A %':>8} {'B-A ms (median)':>16}")
for v in pairs[0][1][0]:
    a = [p[1][0][v] for p in pairs if v in p[1][0] and v in p[2][0]]
    d = [p[2][0][v] - p[1][0][v] for p in pairs if v in p[1][0] and v in p[2][0]]
    if not d:
        continue
    am = statistics.mean(a)
    print(f"{v:<24} {am:12.3f} {statistics.mean(d):+14.3f} {statistics.mean(d) / am * 100.0:+7.2f}% {statistics.median(d):+16.3f}")
PY
}

if [[ "${1:-}" == "--summarize" ]]; then
  [[ $# == 2 ]] || Usage
  Summarize "$2"
  exit
fi
[[ $# -ge 2 ]] || Usage
A_DIR="$(cd "$1" && pwd)"; B_DIR="$(cd "$2" && pwd)"
shift 2
MAP="" PAIRS=3 FRAMES=600 NOTE="" OUT="" DRY=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --map) MAP="$2"; shift 2 ;;
    --pairs) PAIRS="$2"; shift 2 ;;
    --frames) FRAMES="$2"; shift 2 ;;
    --note) NOTE="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    *) Usage ;;
  esac
done
[[ "$PAIRS" =~ ^[0-9]+$ && $PAIRS -ge 3 ]] || { echo "bench-export-ab: --pairs must be 3 or more" >&2; exit 2; }
[[ "$FRAMES" =~ ^[0-9]+$ ]] || { echo "bench-export-ab: --frames must be a number" >&2; exit 2; }
[[ -n "$NOTE" ]] || { echo "bench-export-ab: give a --note (what the two exports differ in)" >&2; exit 2; }
[[ -n "$OUT" ]] || OUT="$ROOT/benchmark/export-ab/$(date +%Y-%m-%d_%H%M%S)"
mkdir -p "$OUT/A" "$OUT/B"
OUT="$(cd "$OUT" && pwd)"

# Package <export dir>: the map package folder in it (--map, or the one folder with a <name>.mapb)
Package() {
  local dir="$1" found=()
  if [[ -n "$MAP" ]]; then
    [[ -f "$dir/$MAP/$MAP.mapb" ]] || { echo "bench-export-ab: $dir/$MAP/$MAP.mapb not found" >&2; exit 1; }
    echo "$dir/$MAP"
    return
  fi
  for m in "$dir"/*/*.mapb; do
    [[ -f "$m" && "$(basename "$m" .mapb)" == "$(basename "$(dirname "$m")")" ]] && found+=("$(dirname "$m")")
  done
  [[ ${#found[@]} == 1 ]] || { echo "bench-export-ab: $dir: ${#found[@]} map packages; name one with --map" >&2; exit 1; }
  echo "${found[0]}"
}

# Build <export dir> <out sub dir>: viewer.c with the harness (its own object, so the harness never
# reaches the runtime), the export's runtime and libraries, the export's compiler flags
Build() {
  local dir="$1" out="$2" lib=""
  for l in "$dir/prebuilt/linux/lib" "$dir/build/r3d-linux/lib"; do [[ -f "$l/libr3d.a" ]] && { lib="$l"; break; }; done
  [[ -n "$lib" ]] || { echo "bench-export-ab: $dir has no R3D libraries (prebuilt/linux/lib or build/r3d-linux/lib)" >&2; exit 1; }
  [[ -f "$dir/viewer.c" && -f "$dir/monolith_scene.c" ]] || { echo "bench-export-ab: $dir is not an export" >&2; exit 1; }
  local cc="${CC:-cc}"
  local flags=(-std=c11 -O2 -Wall -Wextra -Wno-missing-field-initializers -DMS_PATCHED_R3D
               -I"$dir" -I"$dir/r3d/include" -I"$dir/r3d/external/raylib/src")
  "$cc" "${flags[@]}" -include "$HARNESS" -c "$dir/viewer.c" -o "$out/viewer.o"
  "$cc" "${flags[@]}" -c "$dir/monolith_scene.c" -o "$out/monolith_scene.o"
  "$cc" "$out/viewer.o" "$out/monolith_scene.o" -o "$out/viewer-bench" \
    -L"$lib" -lr3d -lraylib -lassimp -lzlibstatic -lstdc++ -lGL -lm -lpthread -ldl
  echo "built: $out/viewer-bench (${lib#"$dir/"})"
}

A_PKG="$(Package "$A_DIR")"; B_PKG="$(Package "$B_DIR")"
Build "$A_DIR" "$OUT/A"
Build "$B_DIR" "$OUT/B"
{
  echo "base (A): $A_DIR ($A_PKG)"
  echo "new  (B): $B_DIR ($B_PKG)"
  echo "pairs: $PAIRS, frames per viewpoint: $FRAMES, note: $NOTE"
} | tee "$OUT/runs.txt"

# Run <A|B> <pair>
Run() {
  local side="$1" i="$2" bin="$OUT/$1/viewer-bench" pkg
  [[ "$side" == A ]] && pkg="$A_PKG" || pkg="$B_PKG"
  local cmd=(env __GL_SYNC_TO_VBLANK=0 vblank_mode=0 "$bin" --bench "$OUT/$side-$i.md" --bench-frames "$FRAMES"
             --note "$NOTE ($side = $([[ $side == A ]] && echo base || echo new), pair $i of $PAIRS)" "$pkg")
  echo "run $side $i: ${cmd[*]}" >> "$OUT/runs.txt"
  if [[ $DRY == 1 ]]; then printf '%q ' "${cmd[@]}"; echo; return; fi
  echo "bench-export-ab: $side, pair $i of $PAIRS ..."
  "${cmd[@]}" > "$OUT/$side-$i.log" 2>&1 || { echo "bench-export-ab: run $side $i failed (see $OUT/$side-$i.log)" >&2; exit 1; }
}
for ((i = 1; i <= PAIRS; i++)); do
  Run A "$i"
  Run B "$i"
done
[[ $DRY == 1 ]] && exit 0
Summarize "$OUT" | tee "$OUT/summary.txt"
echo "reports: $OUT"
