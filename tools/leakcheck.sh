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
# What it runs: --dump-boxes, --dump-scene (with and without models, and -diff), --shot of the default map
# (two views, camera-fitted shadow and fog cache variants; every quality preset, settings files,
# live settings changes incl. the renderer restart, the settings panel), of tests/models (models, textured /
# shader materials, shadowed lights), of examples/01_minimal/scene and maps/pine_forest
# (textured ground, many restyled models), --reload-test both ways
# and into a broken map, a broken map on the command line, --bake-surface (and a data_sampler reading
# its tile), --shot-hud with --select / --region / --volume,
# the render inspector (outline modes, labels, its panel, buffer views, the draw filter with a live
# edit, a preview and a reload), manual edits (--apply-edits on a copy of tests/models: move, turn, split, duplicate, add, id, undo, redo and
# a failing command; `id` / `id remove` with a failing one; --dump-entities; the edit-mode HUD), the id panel
# (--id-panel unfiltered, filtered, and with live id edits and the number field), live edits (--edit-script with --shot and
# --dump-entities: moves, undo / redo, duplicate, add, a preview, save, and a rebase after `disk`
# with a failing step; with the fog cache and the AO volume; refused without --shot / a dump),
# the transform ops (turn with pitch / roll / local / pivot, scale, size: --apply-edits with a failing
# command, --edit-script with turn / scale previews) and the transform modal (--edit-modal: open with
# the HUD, committed, cancelled; --input-script: mouse and keys through the input shim, a no-op turn), the
# rotate gizmo, the R step and reset, turned boxes (shots with the caches, a reload, live edits),
# the model picker (--model-picker: thumbnails of the scene's files and of one loaded for the render,
# a filter, freed and rendered again across a reload and a renderer restart, ENTER and ESC),
# --submit (tools/stub-terminal.sh instead of a terminal, MONOLITH_WORKSPACE=) on both maps, in
# coordinator mode (queue + coordinator start, a cancel with no coordinator) and per-request mode,
# --export, --compile and compiled maps (dumps, a shot with a reload, a damaged file, a stale one
# and its refused export), --inspect-model, --new-map, --import (stub terminal), --classify, and
# every example's --shot (plus the capture shim; the game-style ones load compiled maps), and the
# profiling tool (tools/profile.c) on the fixtures in tests/profile/ (--report-only, no perf run).
# Bundles go to its own requests folder
# (MONOLITH_REQUESTS_DIR=<tmp>/requests), never to requests/: a coordinator running meanwhile
# never sees them, and bundles queued meanwhile are left alone. Files it creates (that folder,
# the export, the new map packages) go to the trash afterwards (gio trash), never rm.
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
  # the profiling tool (./build.sh profile builds the real one): plain C, libc only
  if [[ ! -x "$BIN_DIR/profile" || "$ROOT/tools/profile.c" -nt "$BIN_DIR/profile" ]]; then
    SAN=leak; [[ $MODE == debug ]] && SAN=address,undefined
    "${CC:-clang}" -std=c11 -Wall -Wextra -Wno-missing-field-initializers -g -O1 -fno-omit-frame-pointer \
      -fsanitize=$SAN "$ROOT/tools/profile.c" -o "$BIN_DIR/profile" \
      || { echo "leakcheck: building tools/profile.c failed" >&2; exit 1; }
  fi
fi
[[ -x "$M" ]] || { echo "leakcheck: $M missing (run without --no-build)" >&2; exit 1; }
PROFILE="$BIN_DIR/profile"
[[ -x "$PROFILE" ]] || { echo "leakcheck: $PROFILE missing (run without --no-build)" >&2; exit 1; }

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
REQ="$TMP/requests"         # (inside TMP: trashed with it)
mkdir -p "$REQ"

echo "leakcheck: running monolith ($MODE build) ..."
Run dump-boxes 0        "$M" --dump-boxes "$TMP/boxes.txt"
Run dump-scene 0        "$M" --dump-scene "$TMP/scene.txt"
Run dump-scene-models 0 "$M" --dump-scene "$TMP/scene-models.txt" --map tests/models
Run dump-scene-diff 0   "$M" --dump-scene-diff "$TMP/scene-diff.txt" --map tests/models
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
# --bake-surface (bake.c): a shader with an MS_BAKE path, 3 levels into an atlas, then a map that
# reads the tile through a data_sampler; and a material that doesn't exist (exit 1)
mkdir -p "$TMP/bake/shaders"
printf '#pragma usage opaque\nvoid fragment()\n{\n#ifdef MS_BAKE\n    ALBEDO = vec3(fract(POSITION.xy), float(MS_BAKE_LEVEL) / 8.0);\n#else\n    ALBEDO = vec3(0.5);\n#endif\n    ROUGHNESS = 0.5;\n}\n' > "$TMP/bake/shaders/s.glsl"
printf 'monolith 2\nmaterial "s" shader "shaders/s.glsl" alpha_cutoff 0\nbox 0 0 0 1 1 1\n' > "$TMP/bake/bake.map"
Run bake-surface 0      "$M" --map "$TMP/bake" --bake-surface s "$TMP/bake/tile" --bake-size 64 --bake-levels 3 --bake-face side
printf '#pragma usage opaque\nuniform sampler2D u_tile;\nvoid fragment()\n{\n    ALBEDO = texelFetch(u_tile, ivec2(fract(POSITION.xz) * 64.0), 0).rgb;\n}\n' > "$TMP/bake/shaders/t.glsl"
printf 'material "t" shader "shaders/t.glsl" data_sampler u_tile "tile.png" alpha_cutoff 0\nuse "t"\nbox 2 0 0 3 1 1\n' >> "$TMP/bake/bake.map"
Run shot-data-sampler 0 "$M" --shot "$TMP/bake.png" --map "$TMP/bake"
Run bake-no-material 1  "$M" --map "$TMP/bake" --bake-surface nope "$TMP/bake/x"
# Manual edits (mapedit.c) on a copy; the last command fails on purpose (exit 1)
cp -r tests/models "$TMP/edit-models"
cat > "$TMP/edits.txt" <<'EDITS'
select line 47
move 0 1 0
select line 36 copy 1
move 0.5 0 0
turn 15
select line 32
turn 90
duplicate 0 0 3
select line 30 copy 2
remove
select line 26
add "models/tree_oak.glb" 8 0 8
undo
undo
redo
print
select id 1
id 40
select line 26
also id 40
id
undo
select line 9999
remove
EDITS
Run apply-edits 1       "$M" --apply-edits "$TMP/edits.txt" --map "$TMP/edit-models"
# Live edits (--edit-script: the working copy, MS_EditSetText, the caches redrawn in part, the
# preview, save, and the rebase after an outside change with a step that no longer applies)
cp -r tests/models "$TMP/live-models"
grep -v '^repeat 3 4 0 0 model "models/tree_oak.glb"' tests/models/models.map > "$TMP/live-changed.map"
printf 'group "added on disk"\nid 1 box 8 0 8 9 1 9\n' >> "$TMP/live-changed.map"
cat > "$TMP/live.txt" <<EDITS
select line 30 copy 1
also line 36 copy 1
also line 45
preview move 0 1 0
preview move 1 1 0
preview end
preview move 0.5 0 0
move 0.5 0 0
undo
redo
select line 34
duplicate 0 0 3
turn 90
select line 26
add "models/rock_largeA.glb" 2 0 5
save
select line 44
move 0 0 1
select id 1
move 0 1 0
select line 49
remove
print
disk $TMP/live-changed.map
print
select line 30
EDITS
Run edit-script-shot 0  "$M" --shot "$TMP/live.png" --view 1 --map "$TMP/live-models" --edit-script "$TMP/live.txt" \
                            --fog-cache 1 2 --ao-volume 0.5 0.6 --shot-hud --edit-mode
cp -r tests/models "$TMP/live-models2"
Run edit-script-dump 0  "$M" --dump-entities "$TMP/live-entities.txt" --map "$TMP/live-models2" --edit-script "$TMP/live.txt"
Run edit-script-alone 1 "$M" --map "$TMP/live-models2" --edit-script "$TMP/live.txt"
# A step undone past the save point, then a new edit (the undone step moves to lost[]), then a
# change on disk: the lost step is reported failed
cp -r tests/models "$TMP/lost-models"
{ cat tests/models/models.map; echo "# changed on disk"; } > "$TMP/lost-changed.map"
printf 'select line 40\nturn 0 25 15\nsave\nundo\nselect line 30\nduplicate 0 0 -3\nprint\ndisk %s\nprint\n' \
       "$TMP/lost-changed.map" > "$TMP/lost.txt"
Run edit-script-lost 0  "$M" --dump-boxes "$TMP/lost-boxes.txt" --map "$TMP/lost-models" --edit-script "$TMP/lost.txt"
Run dump-entities 0     "$M" --dump-entities "$TMP/entities.txt" --map tests/models
Run edit-mode-hud 0     "$M" --shot "$TMP/edit.png" --view 1 --map tests/models --shot-hud --edit-mode --select 36
Run edit-id-panel 0     "$M" --shot "$TMP/edit-id.png" --view 1 --map tests/models --shot-hud --edit-mode --select 30 --edit-id 57
# `id` and `id remove` (the last command fails on purpose: line 30 has no id any more); the id panel
# (F5) open on the default map, filtered, and after live edits that give and remove ids (its rows
# rebuilt) with the edit mode's number field open beside it
cp -r tests/models "$TMP/id-models"
printf 'select line 30\nid\nalso line 36\nid\nid remove\nundo\nredo\nselect line 30\nid remove\n' > "$TMP/id.txt"
Run apply-id-remove 1   "$M" --apply-edits "$TMP/id.txt" --map "$TMP/id-models"
Run id-panel 0          "$M" --shot "$TMP/idpanel.png" --view 1 --shot-hud --id-panel
Run keys-panel 0        "$M" --shot "$TMP/keys.png" --view 1 --shot-hud --keys-panel grab
Run id-panel-filter 0   "$M" --shot "$TMP/idpanel2.png" --view 1 --shot-hud --id-panel block
cp -r tests/models "$TMP/id-models2"
printf 'select line 30\nid 5\nselect line 36\nalso line 40\nid\nselect id 5\nid remove\n' > "$TMP/id-live.txt"
Run id-panel-live 0     "$M" --shot "$TMP/idpanel3.png" --view 1 --map "$TMP/id-models2" --shot-hud --id-panel tree \
                            --edit-script "$TMP/id-live.txt" --edit-mode --select 45 --edit-id 9
# The transform ops (mapedit.c) on a copy, the last command fails on purpose (a light can't be
# scaled); live with turn / scale previews; the transform modal (transform.c) open, committed, cancelled
cp -r tests/models "$TMP/xform-models"
cat > "$TMP/xform.txt" <<'EDITS'
select line 41
turn 0 30 0
scale 1.25
select line 40
turn 0 0 15 local
turn 10 20 30 pivot 1 0 1
scale 1 2 1
select line 36 copy 1
turn 37.5 pivot 0 0 -2
select line 32
size 0 0.5 3
turn 90 pivot 0 0 -8
select line 30 copy 2
scale 1.25 1 2
select line 30
turn -90 pivot 0 0 -8
undo
redo
select line 49
scale 2
EDITS
Run apply-xform 1       "$M" --apply-edits "$TMP/xform.txt" --map "$TMP/xform-models"
cp -r tests/models "$TMP/xform-live"
{ printf 'select line 30\nalso line 36\nalso line 40\nalso line 45\npreview turn 90 pivot 0 0 0\npreview scale 1.5 pivot 0 0 0\npreview end\n'
  head -n 18 "$TMP/xform.txt"; } > "$TMP/xform-live.txt"
Run edit-script-xform 0 "$M" --shot "$TMP/xform.png" --view 1 --map "$TMP/xform-live" --edit-script "$TMP/xform-live.txt" \
                            --fog-cache 1 2 --ao-volume 0.5 0.6
Run edit-modal-hud 0    "$M" --shot "$TMP/modal.png" --view 1 --map tests/models --shot-hud --select 30 --select 36 --select 40 \
                            --edit-modal "r xx 30"
cp -r tests/models "$TMP/modal-models"
Run edit-modal-commit 0 "$M" --shot "$TMP/modal-commit.png" --view 1 --map "$TMP/modal-models" --select 30 --select 40 \
                            --edit-modal "s Y 1.5 commit"
Run edit-modal-cancel 0 "$M" --shot "$TMP/modal-cancel.png" --view 1 --map tests/models --select 30 --select 40 --select 45 \
                            --edit-modal "g x 1.25 cancel"
# The rotate gizmo and the R step / reset (docs/live-editing.md 11.10), on a model and on turned boxes
Run gizmo-hover 0       "$M" --shot "$TMP/gizmo.png" --view 1 --map tests/models --shot-hud --select 40 --edit-modal "gizmo hover y"
cp -r tests/models "$TMP/gizmo-models"
Run gizmo-ring 0        "$M" --shot "$TMP/gizmo-ring.png" --view 1 --map "$TMP/gizmo-models" --shot-hud --select 40 \
                            --edit-modal "ring y 37.5 commit"
Run gizmo-ball 0        "$M" --shot "$TMP/gizmo-ball.png" --view 1 --map "$TMP/gizmo-models" --select 40 --edit-modal "ball 30 -10 commit"
Run gizmo-view 0        "$M" --shot "$TMP/gizmo-view.png" --view 1 --map "$TMP/gizmo-models" --select 40 --edit-modal "ring view 20 cancel"
cp -r tests/rotated "$TMP/gizmo-rotated"
Run step-r90 0          "$M" --shot "$TMP/step.png" --view 1 --map "$TMP/gizmo-rotated" --select 155 --edit-modal "r90"
Run reset-rotation 0    "$M" --shot "$TMP/reset.png" --view 1 --map "$TMP/gizmo-rotated" --select 147 --edit-modal "reset"
Run gizmo-ring-box 0    "$M" --shot "$TMP/gizmo-box.png" --view 1 --map "$TMP/gizmo-rotated" --select 142 --edit-modal "ring x 15 commit"
# Turned boxes (box / block lines with `rot`): merged and instanced meshes, the caches, a reload, live
# edits that add, change and drop a rotation, previews
Run rotated-shot 0      "$M" --shot "$TMP/rotated.png" --view 2 --map tests/rotated --fog-cache 1 2 --ao-volume 0.5 0.6
Run rotated-reload 0    "$M" --shot "$TMP/rotated-reload.png" --view 1 --map tests/rotated --reload-test tests/models
cp -r tests/rotated "$TMP/rotated-live"
printf 'select line 147\npreview turn 0 30 0\npreview end\nturn 0 30 0 local\nrotation 0 0 0\nselect line 142\nturn 37.5\nturn -37.5\nundo\nselect line 149\nturn 15 pivot 0 0 0\n' \
    > "$TMP/rotated-live.txt"
Run rotated-edits 0     "$M" --shot "$TMP/rotated-live.png" --view 1 --map "$TMP/rotated-live" --edit-script "$TMP/rotated-live.txt" \
                            --ao-volume 0.5 0.6
# The modal through the input shim (--input-script): a click selects a repeat copy, R / G / S with
# the mouse, a lock, typed digits, LMB / ENTER / RMB, the cursor hidden and shown again, CTRL+S
cp -r tests/models "$TMP/input-models"
cat > "$TMP/input.txt" <<'INPUT'
at 1180 440
click left
wait 2
down ALT
press R
up ALT
move 140 0 10
click left
wait 2
press G
press X
move 60 0 5
press ENTER
press S
move 80 0 5
click right
down ALT
press R
up ALT
press Y
press 4
press 5
press ENTER
wait 2
down CTRL
press S
up CTRL
INPUT
Run input-script 0      "$M" --shot "$TMP/input.png" --view 2 --map "$TMP/input-models" --shot-hud --edit-mode --frame-reuse \
                            --input-script "$TMP/input.txt"
printf 'select line 32\nsize 2 0 2\n' > "$TMP/input-square.txt"
cp -r tests/models "$TMP/input-square"
Run input-square-prep 0 "$M" --apply-edits "$TMP/input-square.txt" --map "$TMP/input-square"
printf 'at 860 352\ndown ALT\npress R\nup ALT\nclick left\n' > "$TMP/input-noop.txt"
Run input-noop 0        "$M" --shot "$TMP/input-noop.png" --view 1 --map "$TMP/input-square" --shot-hud --select 32 --edit-mode \
                            --input-script "$TMP/input-noop.txt"
printf 'press NOKEY\n' > "$TMP/input-bad.txt"
Run input-bad 1         "$M" --shot "$TMP/input-bad.png" --input-script "$TMP/input-bad.txt"
# The render inspector: outline modes, labels, its panel, buffer views, the draw filter (also
# with a live edit, a preview and a reload while it hides something)
Run inspect-material 0  "$M" --shot "$TMP/insp1.png" --view 2 --shot-hud --inspect material --output-mode normal
Run inspect-shader-panel 0 "$M" --shot "$TMP/insp2.png" --view 1 --map tests/models --shot-hud --inspect shader \
                            --inspector-panel --inspect-hide leaves --output-mode ssao
Run inspect-draw-isolate 0 "$M" --shot "$TMP/insp3.png" --view 1 --map maps/pine_forest --inspect draw --inspect-isolate needles
cp -r tests/models "$TMP/insp-models"
Run inspect-filter-edit 0 "$M" --shot "$TMP/insp4.png" --view 1 --map "$TMP/insp-models" --edit-script "$TMP/live.txt" \
                            --inspect material --inspect-hide bark --fog-cache 1 2
Run inspect-filter-reload 0 "$M" --shot "$TMP/insp6.png" --view 1 --map tests/models --inspect-hide rock --reload-test maps/pine_forest
Run inspect-bad-mode 1  "$M" --shot "$TMP/insp5.png" --inspect nothing
# The model picker: thumbnails (the scene's files and one loaded for the render), filtered, freed and
# rendered again by a reload, a renderer restart, ENTER (a live edit) and ESC
Run picker-hud 0        "$M" --shot "$TMP/pick1.png" --view 1 --map maps/pine_forest --shot-hud --model-picker
Run picker-loaded 0     "$M" --shot "$TMP/pick2.png" --view 2 --shot-hud --model-picker sedan
Run picker-reload 0     "$M" --shot "$TMP/pick3.png" --view 1 --map tests/models --model-picker --reload-test tests/models
Run picker-restart 0    "$M" --shot "$TMP/pick4.png" --view 1 --map tests/models --model-picker rock --settings-change shadow_size=2048
cp -r tests/models "$TMP/pick-models"
Run picker-enter 0      "$M" --shot "$TMP/pick5.png" --view 1 --map "$TMP/pick-models" --model-picker oak --model-picker-key enter
Run picker-close 0      "$M" --shot "$TMP/pick6.png" --view 1 --map tests/models --model-picker --model-picker-key close
Run picker-bad-key 1    "$M" --shot "$TMP/pick7.png" --model-picker --model-picker-key maybe
Run shot-hud 0          "$M" --shot "$TMP/hud.png" --view 2 --shot-hud --select 192 --select 198 \
                            --region 100 100 300 200 --region 800 300 200 200 --volume -11 0 -34 -7 20 34 \
                            --volume 3 4 12 -4 0 4 --type 'make it\ntaller' --pick --pick-at 10 10 --pick-at 1590 890
Run submit-default 0    env MONOLITH_REQUESTS_DIR="$REQ" MONOLITH_TERMINAL="$STUB" MONOLITH_STUB_LOG="$TMP/stub1.txt" MONOLITH_WORKSPACE= \
                            "$M" --shot "$TMP/s1.png" --view 2 --select 192 --select 193 --region 100 100 300 200 \
                            --region 800 300 200 200 --volume -11 0 -34 -7 20 34 --volume -4 0 4 3 4 12 \
                            --submit 'leakcheck: make these taller'
Run submit-models 0     env MONOLITH_REQUESTS_DIR="$REQ" MONOLITH_TERMINAL="$STUB" MONOLITH_STUB_LOG="$TMP/stub2.txt" MONOLITH_WORKSPACE= \
                            MONOLITH_LAUNCH=per-request "$M" --shot "$TMP/s2.png" --view 1 --map tests/models --select 36 \
                            --region 0 0 1600 900 --volume -50 -1 -50 50 30 50 --submit '@heavy leakcheck: restyle the trees' --submit-wait 1 --close-submitted
Run submit-cancel 0     env MONOLITH_REQUESTS_DIR="$REQ" MONOLITH_TERMINAL="$STUB" MONOLITH_CLAUDE=/nonexistent/claude MONOLITH_WORKSPACE= \
                            "$M" --shot "$TMP/s3.png" --view 2 --submit 'leakcheck: cancel me' --submit-wait 1 --close-submitted
Run export-default 0    "$M" --export "$TMP/export-default"
Run export-models 0     "$M" --export "$TMP/export-models" --map tests/models
# Compiled maps: a copy of tests/models compiled, loaded (dumps, a shot with a reload), a damaged
# file, then its source changed (stale: a warning when loaded, the export refuses it)
CM="$TMP/compiled-models"
mkdir -p "$CM" && cp -r tests/models/. "$CM/"
Run compile 0           "$M" --compile "$CM/models.mapb" --map "$CM"
Run compile-broken 1    "$M" --compile "$TMP/broken.mapb" --map "$TMP/missing-package"
Run dump-scene-compiled 0 "$M" --dump-scene "$TMP/scene-compiled.txt" --map "$CM/models.mapb"
Run shot-compiled 0     "$M" --shot "$TMP/compiled.png" --view 1 --map "$CM/models.mapb" --reload-test "$CM/models.mapb"
head -c 4000 "$CM/models.mapb" > "$CM/damaged.mapb"
Run compiled-damaged 1  "$M" --dump-boxes "$TMP/damaged.txt" --map "$CM/damaged.mapb"
echo '# changed' >> "$CM/models.map"
Run compiled-stale 0    "$M" --dump-entities "$TMP/stale.txt" --map "$CM/models.mapb"
Run export-stale 1      "$M" --export "$TMP/export-stale" --map "$CM/models.mapb"
Run inspect-model 0     "$M" --inspect-model tests/models/models/tree_oak.glb
Run inspect-missing 1   "$M" --inspect-model "$TMP/missing.glb"
NEWMAP="_leakcheck_$STAMP"
Run new-map 0           "$M" --new-map "$NEWMAP"
Run new-map-path 0      "$M" --new-map "$TMP/newmap_path"
Run classify 0          "$M" --classify 'make the pillar taller' --select 192
IMPORTMAP="_leakcheck_import_$STAMP"
Run import 0            env MONOLITH_REQUESTS_DIR="$REQ" MONOLITH_TERMINAL="$STUB" MONOLITH_STUB_LOG="$TMP/stub3.txt" MONOLITH_WORKSPACE= \
                            "$M" --import examples/04_import_source "$IMPORTMAP"

# The profiling tool: its report on a cut-down real profile and bench report (tests/profile/),
# a missing file, and a run it must refuse (it would open a window) before perf starts
echo "leakcheck: running the profiling tool ..."
Run profile-report 0      "$PROFILE" --report-only tests/profile/perf.txt
Run profile-report-all 0  "$PROFILE" --report-only tests/profile/perf.txt --all --top 5
Run profile-report-gpu 0  "$PROFILE" --report-only tests/profile/perf.txt --bench tests/profile/bench.md --finish-site Run+0x5c02
Run profile-report-gpu-all 0 "$PROFILE" --report-only tests/profile/perf.txt --bench tests/profile/bench.md --all
Run profile-missing 1     "$PROFILE" --report-only "$TMP/missing-perf.txt"
Run profile-refuse 2      "$PROFILE" --bin "$M" refused -- --view 2

if [[ $MODE == lsan ]]; then
  echo "leakcheck: running the examples ..."
  Run ex-01-minimal 0         "$EX_DIR/01_minimal/minimal" --shot "$TMP/e1.png"
  Run ex-01-minimal-v2 0      "$EX_DIR/01_minimal/minimal" --shot "$TMP/e1b.png" --view 2
  Run ex-01-minimal-quality 0 "$EX_DIR/01_minimal/minimal" --shot "$TMP/e1q.png" --view 2 --quality medium
  Run ex-02-before 0          "$EX_DIR/02_existing_project/before" --shot "$TMP/e2a.png"
  Run ex-02-after 0           "$EX_DIR/02_existing_project/after" --shot "$TMP/e2b.png"
  Run ex-03-live-editing 0    "$EX_DIR/03_live_editing/live_editing" --shot "$TMP/e3.png"
  Run ex-05-entities 0        "$EX_DIR/05_entities/entities" --shot "$TMP/e5.png" --find a7d95ba7:0
  Run ex-04-import-source 0   "$EX_DIR/04_import_source/import_source" --shot "$TMP/e4.png"
  Run ex-04-capture 0         env MONOLITH_CAPTURE="$TMP/capture" "$EX_DIR/04_import_source/import_source_capture" --shot "$TMP/e4c.png"
fi

# --- Clean up what the runs created (to the trash)
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
