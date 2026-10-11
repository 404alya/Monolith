#!/usr/bin/env bash
# tools/leakcheck.sh -- run monolith's main code paths under LeakSanitizer; exit 1 on any leak
#
#   tools/leakcheck.sh              build the LSan binaries if a source is newer, run everything
#   tools/leakcheck.sh --no-build   use the existing build/linux-lsan + examples/build/linux-lsan
#   tools/leakcheck.sh --bench      also a short headless bench (--bench --headless --bench-frames 10
#                                   --bench-window 4): only when no other benchmark runs (one GPU)
#   tools/leakcheck.sh --asan       the same paths with the ASan + UBSan build (./build.sh debug,
#                                   build/linux-debug): use-after-free, overflows, UB; leaks too
#
# Standing rule (CLAUDE.md): after any change to code (C, shaders, build), this must report
# zero leaks before the change counts as done. Map-only edits don't need it.
#
# Every run uses a hidden window (--shot, --dump-*, ...) or none: nothing appears on screen.
# What it runs: --dump-boxes, --dump-scene (with and without models), --shot of the default map
# (two views, camera-fitted shadow and fog cache variants; every quality preset, settings files,
# live settings changes incl. the renderer restart, the settings panel), of tests/models (models, textured /
# shader materials, shadowed lights), of examples/01_minimal/scene and maps/pine_forest
# (textured ground, many restyled models), --reload-test both ways
# and into a broken map, a broken map on the command line, --shot-hud with --select / --region,
# --submit (tools/stub-terminal.sh instead of a terminal, MONOLITH_WORKSPACE=) on both maps,
# --export, --inspect-model, --new-map, --import (stub terminal), --classify, and every
# example's --shot (plus the capture shim). Files it creates (bundles in requests/, the export,
# the new map packages) go to the trash afterwards (gio trash), never rm.
#
# Leaks are classified by tools/lsan_report.py with tools/lsan.supp: only memory allocated by
# GPU driver / system library code is ignored (see the comment at the top of lsan.supp); every
# leak allocated by our code, raylib, R3D or assimp counts. Logs: build/leakcheck/latest/logs/.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

BUILD=1
BENCH=0
MODE=lsan
for a in "$@"; do
  case "$a" in
    --no-build) BUILD=0 ;;
    --asan) MODE=debug ;;
    --bench) BENCH=1 ;;
    *) echo "usage: tools/leakcheck.sh [--no-build] [--asan] [--bench]" >&2; exit 2 ;;
  esac
done
if [[ $MODE == lsan ]]; then BIN_DIR="$ROOT/build/linux-lsan"; EX_DIR="$ROOT/examples/build/linux-lsan"
else BIN_DIR="$ROOT/build/linux-debug"; EX_DIR=""; fi
M="$BIN_DIR/monolith"

Trash() {   # moves files we created to the trash (never rm)
  for p in "$@"; do
    [[ -e "$p" ]] || continue
    gio trash "$p" 2>/dev/null || echo "leakcheck: could not move $p to the trash; remove it by hand" >&2
  done
}

# --- Build (incremental: only when a source is newer than the binary)
# (R3D's sources count too: build.sh then rebuilds the static library incrementally)
R3D_SRC="${R3D_DIR:-$ROOT/submodules/r3d}"
Newer() { [[ ! -x "$1" ]] || [[ -n "$(find "$ROOT/src" "$ROOT/runtime" "$ROOT/build.sh" "$ROOT/examples" \
                                          "$R3D_SRC/src" "$R3D_SRC/include" "$R3D_SRC/shaders" -newer "$1" \
                                          \( -name '*.c' -o -name '*.h' -o -name '*.sh' -o -name '*.glsl' -o -name '*.vert' -o -name '*.frag' \) \
                                          -not -path '*/build/*' -print -quit 2>/dev/null)" ]]; }
if [[ $BUILD == 1 ]]; then
  if Newer "$M"; then
    echo "leakcheck: building ($MODE) ..."
    ./build.sh "$MODE" > /dev/null || { echo "leakcheck: ./build.sh $MODE failed" >&2; exit 1; }
  fi
  if [[ $MODE == lsan ]] && Newer "$EX_DIR/01_minimal/minimal"; then
    examples/build.sh lsan > /dev/null || { echo "leakcheck: examples/build.sh lsan failed" >&2; exit 1; }
  fi
fi
[[ -x "$M" ]] || { echo "leakcheck: $M missing (run without --no-build)" >&2; exit 1; }

# --- Output: build/leakcheck/<stamp>/ (logs stay; build/ is not committed), `latest` points
# at it; tmp/ (shots, exports) goes to the trash at the end
STAMP="$(date +%Y%m%d%H%M%S)"
OUT="$ROOT/build/leakcheck/$STAMP"
LOG="$OUT/logs"
TMP="$OUT/tmp"
mkdir -p "$LOG" "$TMP"
ln -sfn "$STAMP" "$ROOT/build/leakcheck/latest"
: > "$LOG/runs.txt"

# LeakSanitizer: full reports into $LOG/<run>.<pid> (no suppressions here: lsan_report.py
# classifies by the allocating frame); the program's own exit status is kept (exitcode=0).
# fast_unwind_on_malloc=0: R3D / raylib / the driver have no frame pointers, so unwind with
# DWARF to get the whole stack. ASan mode: the same plus halt on the first error.
SAN_COMMON="exitcode=0:fast_unwind_on_malloc=0:malloc_context_size=40"
FAILED=0
# Run <name> <expected exit status> <command...>
Run() {
  local name=$1 want=$2
  shift 2
  if [[ $MODE == lsan ]]; then
    LSAN_OPTIONS="$SAN_COMMON:log_path=$LOG/$name" "$@" > "$LOG/$name.out" 2>&1
  else
    ASAN_OPTIONS="detect_leaks=1:$SAN_COMMON:log_path=$LOG/$name" UBSAN_OPTIONS="print_stacktrace=1:log_path=$LOG/$name" \
      LSAN_OPTIONS="$SAN_COMMON" "$@" > "$LOG/$name.out" 2>&1
  fi
  local st=$?
  echo "$name $st" >> "$LOG/runs.txt"
  if [[ $st != "$want" ]]; then
    echo "leakcheck: $name exited with $st (expected $want); see $LOG/$name.out" >&2
    FAILED=1
  fi
  # ASan / UBSan errors (not leaks) land in the same log files
  if [[ $MODE == debug ]] && grep -lqE 'ERROR: AddressSanitizer|runtime error:' "$LOG/$name".* 2>/dev/null; then
    echo "leakcheck: $name: AddressSanitizer / UBSan error, see $LOG/$name.*" >&2
    FAILED=1
  fi
}

STUB="$ROOT/tools/stub-terminal.sh"
REQ="$ROOT/requests"
mkdir -p "$REQ"
BEFORE="$(ls "$REQ")"

echo "leakcheck: running monolith ($MODE build) ..."
Run dump-boxes 0        "$M" --dump-boxes "$TMP/boxes.txt"
Run dump-scene 0        "$M" --dump-scene "$TMP/scene.txt"
Run dump-scene-models 0 "$M" --dump-scene "$TMP/scene-models.txt" --map tests/models
Run shot-view2 0        "$M" --shot "$TMP/v2.png" --view 2
Run shot-view5-camera 0 "$M" --shot "$TMP/v5.png" --view 5 --shadow-camera --fog-cache 1 2
Run shot-models 0       "$M" --shot "$TMP/models.png" --view 1 --map tests/models
Run shot-models-close 0 "$M" --shot "$TMP/models2.png" --view 2 --map tests/models --shadow-always --no-prepass
Run shot-courtyard 0    "$M" --shot "$TMP/courtyard.png" --view 1 --map examples/01_minimal/scene
[[ -d maps/pine_forest ]] && Run shot-pine-forest 0 "$M" --shot "$TMP/pine.png" --view 1 --map maps/pine_forest
Run shot-render-scale 0 "$M" --shot "$TMP/scale.png" --view 2 --render-scale 0.5 --upscale lanczos
Run shot-ao-baked 0     "$M" --shot "$TMP/aobaked.png" --view 1 --ao baked
Run shot-ao-volume 0    "$M" --shot "$TMP/aovol.png" --view 2 --map tests/models --ao-volume 0.5 0.6 --render-scale 0.75
Run reload-ao-volume 0  "$M" --shot "$TMP/aovol2.png" --view 1 --ao baked --reload-test tests/models
# Graphics settings (settings.c, settingspanel.c, MS_Quality): every preset, a settings file
# written / read / rejected, live changes incl. the renderer restart (shadow map size), the panel
for q in low medium high ultra; do
  Run "shot-quality-$q" 0 "$M" --shot "$TMP/q-$q.png" --view 1 --quality "$q"
done
Run settings-write 0    "$M" --settings "$TMP/settings.ini" --settings-set preset=medium --settings-set bloom=off \
                            --settings-set fps_cap=monitor --dump-boxes "$TMP/sb.txt"
Run settings-read 0     "$M" --shot "$TMP/sr.png" --view 2 --settings "$TMP/settings.ini" --quality low --render-scale 0.6
printf 'preset = high\nbogus = 1\nrender_scale = abc\n' > "$TMP/bad-settings.ini"
Run settings-bad-lines 0 "$M" --shot "$TMP/sbad.png" --view 2 --settings "$TMP/bad-settings.ini"
Run settings-bad-flag 1 "$M" --dump-boxes "$TMP/sb2.txt" --settings-set nonsense=1
Run settings-restart 0  "$M" --shot "$TMP/srs.png" --view 2 --quality low --settings-change preset=ultra
Run settings-restart-models 0 "$M" --shot "$TMP/srm.png" --view 1 --map tests/models --settings-change shadow_size=4096 \
                            --settings-change ao=baked --settings-change ssr=on --settings-change ssgi=on
Run settings-panel 0    "$M" --shot "$TMP/panel.png" --view 2 --shot-hud --settings-panel --quality high
if [[ $BENCH == 1 ]]; then
  Run bench-window 0    "$M" --bench --headless --bench-frames 10 --bench-window 4 --no-profile --note "leakcheck (not a measurement)"
fi
Run reload-to-models 0  "$M" --shot "$TMP/r1.png" --view 2 --reload-test tests/models
Run reload-to-default 0 "$M" --shot "$TMP/r2.png" --view 1 --map tests/models --reload-test maps/monolith --frame-reuse
printf 'monolith 2\nbox 0 0 0 1 1 1\nbogus statement here\n' > "$TMP/broken.map"
Run reload-to-broken 0  "$M" --shot "$TMP/r3.png" --view 1 --map tests/models --reload-test "$TMP/broken.map"
Run broken-map 1        "$M" --dump-boxes "$TMP/broken.txt" --map "$TMP/broken.map"
Run shot-hud 0          "$M" --shot "$TMP/hud.png" --view 2 --shot-hud --select 192 --select 198 \
                            --region 100 100 300 200 --region 800 300 200 200 --type 'make it\ntaller' --pick
Run submit-default 0    env MONOLITH_TERMINAL="$STUB" MONOLITH_STUB_LOG="$TMP/stub1.txt" MONOLITH_WORKSPACE= \
                            "$M" --shot "$TMP/s1.png" --view 2 --select 192 --select 193 --region 100 100 300 200 \
                            --region 800 300 200 200 --submit 'leakcheck: make these taller'
Run submit-models 0     env MONOLITH_TERMINAL="$STUB" MONOLITH_STUB_LOG="$TMP/stub2.txt" MONOLITH_WORKSPACE= \
                            "$M" --shot "$TMP/s2.png" --view 1 --map tests/models --select 36 --region 0 0 1600 900 \
                            --submit '@heavy leakcheck: restyle the trees'
Run export-default 0    "$M" --export "$TMP/export-default"
Run export-models 0     "$M" --export "$TMP/export-models" --map tests/models
Run inspect-model 0     "$M" --inspect-model tests/models/models/tree_oak.glb
Run inspect-missing 1   "$M" --inspect-model "$TMP/missing.glb"
NEWMAP="_leakcheck_$STAMP"
Run new-map 0           "$M" --new-map "$NEWMAP"
Run classify 0          "$M" --classify 'make the pillar taller' --select 192
IMPORTMAP="_leakcheck_import_$STAMP"
Run import 0            env MONOLITH_TERMINAL="$STUB" MONOLITH_STUB_LOG="$TMP/stub3.txt" MONOLITH_WORKSPACE= \
                            "$M" --import examples/04_import_source "$IMPORTMAP"

if [[ $MODE == lsan ]]; then
  echo "leakcheck: running the examples ..."
  Run ex-01-minimal 0         "$EX_DIR/01_minimal/minimal" --shot "$TMP/e1.png"
  Run ex-01-minimal-v2 0      "$EX_DIR/01_minimal/minimal" --shot "$TMP/e1b.png" --view 2
  Run ex-01-minimal-quality 0 "$EX_DIR/01_minimal/minimal" --shot "$TMP/e1q.png" --view 2 --quality medium
  Run ex-02-before 0          "$EX_DIR/02_existing_project/before" --shot "$TMP/e2a.png"
  Run ex-02-after 0           "$EX_DIR/02_existing_project/after" --shot "$TMP/e2b.png"
  Run ex-03-live-editing 0    "$EX_DIR/03_live_editing/live_editing" --shot "$TMP/e3.png"
  Run ex-04-import-source 0   "$EX_DIR/04_import_source/import_source" --shot "$TMP/e4.png"
  Run ex-04-capture 0         env MONOLITH_CAPTURE="$TMP/capture" "$EX_DIR/04_import_source/import_source_capture" --shot "$TMP/e4c.png"
fi

# --- Clean up what the runs created (to the trash)
for d in $(ls "$REQ"); do grep -qxF "$d" <<<"$BEFORE" || Trash "$REQ/$d"; done
Trash "$ROOT/maps/$NEWMAP" "$ROOT/maps/$IMPORTMAP" "$TMP"

# --- Report
echo "leakcheck: results ($(wc -l < "$LOG/runs.txt") runs; logs in ${LOG#"$ROOT/"}/)"
python3 "$ROOT/tools/lsan_report.py" "$ROOT/tools/lsan.supp" "$LOG"
LEAKS=$?
if [[ $LEAKS == 0 && $FAILED == 0 ]]; then
  echo "leakcheck: OK: zero leaks of ours"
  exit 0
fi
[[ $LEAKS != 0 ]] && echo "leakcheck: FAILED: leaks of ours (stacks above)"
[[ $FAILED != 0 ]] && echo "leakcheck: FAILED: a run exited unexpectedly or hit a sanitizer error (see above)"
exit 1
