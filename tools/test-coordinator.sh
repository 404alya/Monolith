#!/usr/bin/env bash
# tools/test-coordinator.sh -- end-to-end test of the request queue + coordinator launch
# (src/launcher.c, tools/request-queue.sh), with no Claude session and no window:
# MONOLITH_TERMINAL=tools/stub-terminal.sh runs the command it is given (MONOLITH_STUB_RUN),
# MONOLITH_CLAUDE=tools/stub-coordinator.sh stands in for claude, MONOLITH_REQUESTS_DIR keeps
# everything in a scratch folder (build/coordinator-test/<stamp>/), so the developer's own
# coordinator and requests/ are never touched. The game runs only with --shot (hidden window).
#
#   tools/test-coordinator.sh [--keep]    --keep: leave the scratch folder (else: gio trash)
#
# Checks: three submits open exactly one terminal; two requests on the same map package never
# overlap; requests on two packages run in parallel; a code request runs alone; the HUD side
# follows the state file; CTRL+K n (--close-submitted) cancels a running request; killing the
# coordinator mid-run requeues untouched requests and reports touched ones on the next start;
# without a coordinator a queued request is cancelled by the game itself; per-request mode
# still opens one terminal per request without a state file.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1
KEEP=0
[[ ${1:-} == --keep ]] && KEEP=1
M="$ROOT/build/linux/monolith"
[[ -x $M ]] || { echo "test-coordinator: $M missing (./build.sh)" >&2; exit 1; }
command -v flock > /dev/null || { echo "test-coordinator: flock (util-linux) missing" >&2; exit 1; }

T="$ROOT/build/coordinator-test/$(date +%Y%m%d%H%M%S)"
R="$T/requests"
mkdir -p "$R"
export MONOLITH_REQUESTS_DIR="$R" MONOLITH_TERMINAL="$ROOT/tools/stub-terminal.sh" MONOLITH_STUB_RUN="$T/coordinator.out" \
       MONOLITH_STUB_OPENED="$T/opened.txt" MONOLITH_CLAUDE="$ROOT/tools/stub-coordinator.sh" MONOLITH_WORKSPACE= \
       MONOLITH_LAUNCH=coordinator MONOLITH_STUB_WORK=6
unset MONOLITH_STUB_LOG MONOLITH_CLAUDE_ARGS MONOLITH_MODEL
LOG="$R/.coordinator/log"
FAIL=0
Ok()   { echo "  ok: $*"; }
Bad()  { echo "  FAIL: $*"; FAIL=1; }
Opened() { [[ -f $T/opened.txt ]] && wc -l < "$T/opened.txt" || echo 0; }
StateOf() { local w=""; [[ -f "$R/$1/state" ]] && read -r w _ < "$R/$1/state"; echo "${w:-none}"; }

N=0
LAST=""
# Submit <package> <text> [extra game flags]: sets LAST to the new bundle id
Submit() {
  local map=$1 text=$2
  shift 2
  N=$((N + 1))
  "$M" --shot "$T/shot.png" --shot-size 320 180 --map "$map" "$@" --submit "$text" > "$T/submit-$N.out" 2>&1
  LAST=$(sed -nE 's/^submit: ok: request queued \[[0-9]+\][^:]*: ([^ ]+) .*/\1/p' "$T/submit-$N.out" | head -n1)
  [[ -n $LAST ]] || LAST=$(sed -nE 's/^submit: ok: request sent \([^)]*\): ([^ ]+) .*/\1/p' "$T/submit-$N.out" | head -n1)
  echo "  submit $N ($(basename "$map")): ${LAST:-FAILED} ($(grep -m1 '^submit:' "$T/submit-$N.out" | cut -c1-160))"
  [[ -n $LAST ]] || Bad "submit $N failed (see $T/submit-$N.out)"
}
# WaitFor <seconds> <condition command...>
WaitFor() {
  local s=$1
  shift
  local t0=$SECONDS
  while ! "$@"; do (( SECONDS - t0 >= s )) && return 1; sleep 0.2; done
}
AllFinal() { local id s; for id in "$@"; do s=$(StateOf "$id"); [[ $s == done || $s == failed || $s == cancelled ]] || return 1; done; }
AllClaimed() { local id; for id in "$@"; do [[ $(StateOf "$id") == claimed ]] || return 1; done; }
LockFree() { flock -n "$R/.coordinator/lock" true; }
# Interval <id>: "<claimed s> <end s>" from the queue log (seconds of the day, ms precision)
Sec() { awk -F'[: ]' '{ printf "%.3f", $2 * 3600 + $3 * 60 + $4 }' <<<"$1"; }
Interval() {
  local c e
  c=$(grep -E " claimed $1( |$)" "$LOG" | tail -n1 | cut -d' ' -f1,2)
  e=$(grep -E " (done|failed|cancelled) $1$" "$LOG" | tail -n1 | cut -d' ' -f1,2)
  [[ -n $c && -n $e ]] || { echo "? ?"; return; }
  echo "$(Sec "$c") $(Sec "$e")"
}
Overlap() {   # Overlap <id> <id>: status 0 if their run intervals overlap
  read -r a0 a1 <<<"$(Interval "$1")"
  read -r b0 b1 <<<"$(Interval "$2")"
  [[ $a0 != "?" && $b0 != "?" ]] || return 2
  awk -v a0="$a0" -v a1="$a1" -v b0="$b0" -v b1="$b1" 'BEGIN { exit !(a0 < b1 && b0 < a1) }'
}
Show() { local id; for id in "$@"; do printf '    %-60s %-9s %s\n' "$id" "$(StateOf "$id")" "$(Interval "$id")"; done; }

echo "test-coordinator: scratch folder ${T#"$ROOT/"}"
"$M" --new-map "$T/map_a" > "$T/newmap-a.out" 2>&1 || { echo "--new-map failed" >&2; exit 1; }
"$M" --new-map "$T/map_b" > "$T/newmap-b.out" 2>&1 || { echo "--new-map failed" >&2; exit 1; }
A="$T/map_a" B="$T/map_b"

echo "1. three submits (two on map_a, one on map_b): one terminal, same map in sequence, two maps in parallel"
Submit "$A" "test one on map a"; A1=$LAST
Submit "$A" "test two on map a"; A2=$LAST
Submit "$B" "test one on map b"; B1=$LAST
(( $(Opened) == 1 )) && Ok "exactly one terminal opened for three submits" || Bad "$(Opened) terminals opened (want 1)"
WaitFor 60 AllFinal "$A1" "$A2" "$B1" || Bad "requests not finished after 60 s"
Show "$A1" "$A2" "$B1"
Overlap "$A1" "$A2"; st=$?
(( st == 1 )) && Ok "same package: $A1 and $A2 did not overlap" || Bad "same package: overlap (status $st)"
if Overlap "$A1" "$B1" || Overlap "$A2" "$B1"; then Ok "different packages ran in parallel"; else Bad "map_b's request did not run in parallel with map_a's"; fi
(( $(Opened) == 1 )) && Ok "still one terminal" || Bad "$(Opened) terminals"

echo "2. a code request runs alone; the game's HUD side follows the state"
Submit "$A" "[code] test engine change"; C1=$LAST
Submit "$B" "test two on map b" --submit-wait 20; B2=$LAST
WaitFor 60 AllFinal "$C1" "$B2" || Bad "not finished after 60 s"
Show "$C1" "$B2"
Overlap "$C1" "$B2"; st=$?
(( st == 1 )) && Ok "the code request ran alone" || Bad "the code request overlapped (status $st)"
# (the game counts a request done as soon as result.md has a line, so it may never see
# "claimed -> done": the stub writes result.md a moment before finishing the state)
grep -q "request $B2: queued -> claimed" "$T/submit-$N.out" && grep -q "request $B2 done: Stub coordinator" "$T/submit-$N.out" &&
  Ok "the game saw queued -> claimed, then done with result.md line 1" ||
  Bad "the game's log lacks the state changes of $B2 (see $T/submit-$N.out)"

echo "3. CTRL+K n on a running request cancels it"
Submit "$A" "test cancel me" --submit-wait 2.5 --close-submitted; X1=$LAST
grep -q "^close \[[0-9]*\]: cancelling" "$T/submit-$N.out" && Ok "the game asked for the cancel ($(grep -m1 '^close .*cancelling' "$T/submit-$N.out"))" ||
  Bad "no cancel message (see $T/submit-$N.out)"
WaitFor 20 AllFinal "$X1" || Bad "not finished"
[[ $(StateOf "$X1") == cancelled ]] && Ok "state cancelled: $(head -n1 "$R/$X1/result.md")" || Bad "state $(StateOf "$X1")"

echo "4. killing the coordinator mid-run: the next submit starts a new one, which requeues / reports"
Submit "$A" "[edit] test interrupted on map a"; A3=$LAST
Submit "$B" "test untouched on map b"; B3=$LAST
WaitFor 30 AllClaimed "$A3" "$B3" || Bad "not claimed"
PG=$(tail -n1 "$T/opened.txt" | cut -d' ' -f2)
kill -TERM -- "-$PG" 2> /dev/null && Ok "killed the coordinator's process group $PG while both were claimed" || Bad "kill $PG failed"
WaitFor 10 LockFree && Ok "the lock was released" || Bad "the lock is still held"
[[ $(StateOf "$A3") == claimed && $(StateOf "$B3") == claimed ]] && Ok "both still claimed after the kill" || Bad "states: $(StateOf "$A3") $(StateOf "$B3")"
Submit "$B" "test after restart on map b"; B4=$LAST
(( $(Opened) == 2 )) && Ok "the submit started a second coordinator" || Bad "$(Opened) terminals (want 2)"
WaitFor 60 AllFinal "$A3" "$B3" "$B4" || Bad "not finished after 60 s"
Show "$A3" "$B3" "$B4"
grep -q "requeued $B3" "$LOG" && [[ $(StateOf "$B3") == done ]] && Ok "untouched $B3 was requeued and done" || Bad "$B3: $(StateOf "$B3")"
[[ $(StateOf "$A3") == failed ]] && head -n1 "$R/$A3/result.md" | grep -q '^Interrupted' && grep -q 'stub-edit.txt' "$R/$A3/result.md" &&
  Ok "touched $A3 was reported: $(head -n1 "$R/$A3/result.md") (lists stub-edit.txt)" || Bad "$A3: $(StateOf "$A3")"
Overlap "$B3" "$B4"; st=$?
(( st == 1 )) && Ok "after the restart map_b's requests still ran in sequence" || Bad "overlap $B3 / $B4 (status $st)"

echo "5. no coordinator: a queued request is cancelled by the game itself"
PG=$(tail -n1 "$T/opened.txt" | cut -d' ' -f2)
kill -TERM -- "-$PG" 2> /dev/null
WaitFor 10 LockFree || Bad "the lock is still held"
MONOLITH_CLAUDE=/nonexistent/claude Submit "$A" "test no coordinator" --submit-wait 0.5 --close-submitted; Y1=$LAST
grep -q "coordinator could not start" "$T/submit-$N.out" && Ok "the start failure was reported, the request stayed queued" || Bad "no start failure message"
[[ $(StateOf "$Y1") == cancelled ]] && Ok "the game cancelled it: $(head -n1 "$R/$Y1/result.md")" || Bad "state $(StateOf "$Y1")"

echo "6. per-request mode (MONOLITH_LAUNCH=per-request): one terminal per request, no state file"
MONOLITH_LAUNCH=per-request MONOLITH_STUB_RUN= MONOLITH_STUB_LOG="$T/per-request-argv.txt" Submit "$A" "test per request mode"; P1=$LAST
[[ -n $P1 && ! -e "$R/$P1/state" ]] && grep -qx "Map edit request: read requests/$P1/request.md and carry it out." "$T/per-request-argv.txt" &&
  Ok "per-request terminal got the old prompt; no state file" || Bad "per-request mode (see $T/per-request-argv.txt)"

echo "--- coordinator output: ${T#"$ROOT/"}/coordinator.out, queue log: ${LOG#"$ROOT/"}"
if [[ $KEEP == 0 ]]; then gio trash "$T" 2> /dev/null || echo "test-coordinator: could not trash $T"; fi
if [[ $FAIL == 0 ]]; then echo "test-coordinator: OK"; exit 0; fi
echo "test-coordinator: FAILED"
exit 1
