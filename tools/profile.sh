#!/usr/bin/env bash
# profile.sh -- CPU profile of one headless monolith run with perf (no sudo, user space only)
#
#   tools/profile.sh [--period N] [--spin] [--keep-data] [--out DIR] [--bin PATH] <name> -- <monolith args...>
#   tools/profile.sh [--bin PATH] --report DIR    write DIR's reports again from its perf.txt
#
#   tools/profile.sh spin -- --bench --headless --note "profile: spinning"
#   tools/profile.sh still -- --bench --headless --bench-still --frame-reuse --note "profile: still"
#   tools/profile.sh load -- --shot /tmp/x.png --map maps/pine_forest --view 1 --reload-test maps/pine_forest
#
# Runs build/linux-profile/monolith (./build.sh profile: -O2 -g and frame pointers, R3D/raylib/
# assimp RelWithDebInfo) under `perf record -e cycles:u -c <period> --call-graph dwarf,16384`, a
# sample every --period user-mode cycles. The default keeps the rate at the CPU's top clock at 40%
# of the kernel's perf_event_max_sample_rate (which the kernel lowers by itself after slow sampling
# interrupts, e.g. to 2000/s; measured on machine A: 80% of it still throttles, 45% doesn't). When
# perf is throttled it drops samples in bursts and biases every share: perf-stats.txt counts the
# throttle events, and profile.sh warns. With __GL_SYNC_TO_VBLANK=0 and
# __GL_YIELD=USLEEP (--spin: the driver's
# default spin-wait in glFinish), then writes into --out (default benchmark/profile/<stamp>_<name>/):
#   perf.txt              the `perf script` text of the recording (perf.data, ~16 KB per sample, is
#                         recorded into a temporary folder under /tmp and deleted from there;
#                         --keep-data moves it into --out instead)
#   run.log               the game's output (the bench report path is in it)
#   report-frame.txt      tools/profile_report.py over the frame samples: parts, DSOs, symbols, the
#                         driver time by call site, the call trees under R3D_End and Run; bench
#                         runs scaled to the report's "CPU until finish" (ms per frame)
#   report-load.txt       the same over init, load, reload, caches and lazy shader compiles
#   frame.folded          folded stacks of the frame samples (flame-graph input)
#   bench.txt             bench runs: the report the profile was scaled to
#   perf-stats.txt        the period and perf's sample, throttle and lost counts
# --report needs the binary that recorded DIR (call sites are offsets into it).
# Only --bench --headless and --shot runs (no visible window). perf counts as a benchmark: one
# run at a time on the GPU. The `benchmarking` skill, "CPU profiling", has the method.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PERIOD="" SPIN=0 KEEP=0 OUT="" REPORT="" NAME="" BIN="$ROOT/build/linux-profile/monolith"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --period) PERIOD="$2"; shift 2 ;;
    --keep-data) KEEP=1; shift ;;
    --spin) SPIN=1; shift ;;
    --out) OUT="$2"; shift 2 ;;
    --bin) BIN="$2"; shift 2 ;;
    --report) REPORT="$2"; shift 2 ;;
    --) shift; break ;;
    -*) echo "profile.sh: unknown option $1" >&2; exit 2 ;;
    *) NAME="$1"; shift ;;
  esac
done
ARGS=("$@")
[[ -x "$BIN" ]] || { echo "profile.sh: no $BIN: run ./build.sh profile first" >&2; exit 1; }

if [[ -z "$REPORT" ]]; then
  if [[ -z "$NAME" || ${#ARGS[@]} -eq 0 ]]; then
    sed -n '3,9p' "${BASH_SOURCE[0]}" >&2
    exit 2
  fi
  has() { local x; for x in "${ARGS[@]}"; do [[ "$x" == "$1" ]] && return 0; done; return 1; }
  if ! { has --bench && has --headless; } && ! has --shot; then
    echo "profile.sh: only --bench --headless or --shot runs (anything else opens a visible window)" >&2
    exit 2
  fi
  command -v perf > /dev/null || { echo "profile.sh: perf not found" >&2; exit 1; }
  [[ -n "$OUT" ]] || OUT="$ROOT/benchmark/profile/$(date +%Y-%m-%d_%H%M%S)_$NAME"
  mkdir -p "$OUT"
  ENV=(__GL_SYNC_TO_VBLANK=0)
  [[ $SPIN == 1 ]] || ENV+=(__GL_YIELD=USLEEP)
  if [[ -z "$PERIOD" ]]; then
    # max clock (kHz) x 2.5 / max sample rate: 40% of the limit at the top clock
    khz=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq 2> /dev/null || echo 5000000)
    rate=$(cat /proc/sys/kernel/perf_event_max_sample_rate)
    PERIOD=$(( khz * 2500 / rate ))
  fi
  echo "profile.sh: $NAME: perf record every $PERIOD cycles into $OUT ..."
  # perf.data is large: it goes into a temporary folder of this script's own under /tmp (the only
  # place where the repo's rules allow a plain delete), never into the repo
  TMP="$(mktemp -d /tmp/monolith-profile.XXXXXX)"
  trap 'rm -rf "$TMP"' EXIT
  # (cd: the game resolves relative paths such as --shot files against the repo, like a plain run)
  ( cd "$ROOT" && env "${ENV[@]}" perf record -q -e cycles:u -c "$PERIOD" --call-graph dwarf,16384 \
      -o "$TMP/perf.data" -- "$BIN" "${ARGS[@]}" ) > "$OUT/run.log" 2>&1 \
    || { tail -20 "$OUT/run.log"; echo "profile.sh: the run failed (log: $OUT/run.log)" >&2; exit 1; }
  perf script -i "$TMP/perf.data" -F comm,tid,time,period,ip,sym,symoff,dso > "$OUT/perf.txt" 2> /dev/null
  # perf's own counts: lost samples and throttling (should be 0: then every share is unbiased)
  { echo "period $PERIOD cycles, perf_event_max_sample_rate $(cat /proc/sys/kernel/perf_event_max_sample_rate)"
    perf report -i "$TMP/perf.data" --stats 2> /dev/null | grep -E '^ *(SAMPLE|THROTTLE|LOST|LOST_SAMPLES) events' | sort -u; } > "$OUT/perf-stats.txt"
  if grep -q 'THROTTLE events' "$OUT/perf-stats.txt"; then
    echo "profile.sh: WARNING: perf was throttled (see $OUT/perf-stats.txt): use a larger --period" >&2
  fi
  if [[ $KEEP == 1 ]]; then mv "$TMP/perf.data" "$OUT/perf.data"; fi
else
  OUT="$REPORT"
  [[ -f "$OUT/perf.txt" && -f "$OUT/run.log" ]] || { echo "profile.sh: no $OUT/perf.txt or run.log" >&2; exit 1; }
fi

# The headless bench's glFinish(): the return address after `call glFinish@plt` in Run(), as perf
# prints a caller (return address - 1)
REP=(--binary "$BIN" --sites 8)
run=$(nm "$BIN" | awk '$3 == "Run" { print $1 }')
ret=$(objdump -d --no-show-raw-insn "$BIN" | awk '/^[0-9a-f]+ <Run>:/ { f = 1; next } /^[0-9a-f]+ <.*>:/ { f = 0 }
      f && /call.*<glFinish@plt>/ { getline; sub(":", "", $1); if (!r) r = $1 } END { print r }')
if [[ -n "$run" && -n "$ret" ]]; then
  REP+=(--finish-site "$(printf 'Run+0x%x' $(( 0x$ret - 0x$run - 1 )))")
fi

# A bench run: frames = views x (warmup + timed frames); scaled to the report's CPU until finish
# (a bench whose frames are all reused has no CPU rows: then cycles per frame)
report=$(awk '/^report: / { print $2 }' "$OUT/run.log" | tail -1)
if [[ -n "$report" && -f "$report" ]]; then
  ms=$(awk -F'|' '$2 ~ /^ CPU until finish / { v = $(NF-1); gsub(/[ *]/, "", v); print v }' "$report")
  views=$(awk -F'|' '/^\| View \| Avg ms/ { t = 1; next } t && /^\|---/ { next } t && /^\| \*\*All/ { exit } t && /^\|/ { n++ } END { print n + 0 }' "$report")
  timed=$(sed -n 's/^| Method | per viewpoint: \([0-9]*\) warmup frames, then \([0-9]*\) timed frames.*/\1 \2/p' "$report")
  if [[ $views -gt 0 && -n "$timed" ]]; then
    read -r warmup frames <<< "$timed"
    REP+=(--frames $(( views * (warmup + frames) )))
    [[ -n "$ms" ]] && REP+=(--frame-ms "$ms")
  fi
  echo "bench report: $report (CPU until finish ${ms:-none} ms, $views views, ${timed:-?} warmup + timed frames)" > "$OUT/bench.txt"
fi

python3 -I "$ROOT/tools/profile_report.py" "$OUT/perf.txt" "${REP[@]}" --root R3D_End --root Run --depth 3 \
  --folded "$OUT/frame.folded" > "$OUT/report-frame.txt"
python3 -I "$ROOT/tools/profile_report.py" "$OUT/perf.txt" "${REP[@]}" --select '^(init|load|reload|cache|lazy)' \
  --root MS_Load --root MS_Reload --root MS_PushLights --depth 4 > "$OUT/report-load.txt" || true
echo "profile.sh: $OUT/report-frame.txt, $OUT/report-load.txt"
