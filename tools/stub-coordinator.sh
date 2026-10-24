#!/usr/bin/env bash
# tools/stub-coordinator.sh -- a fake request coordinator for tests: no Claude, no window.
# The game starts it in place of claude (MONOLITH_CLAUDE=tools/stub-coordinator.sh, with
# MONOLITH_TERMINAL=tools/stub-terminal.sh and MONOLITH_STUB_RUN=<log>, see
# tools/test-coordinator.sh). It ignores its arguments and applies the scheduling rules of
# the coordinator skill (.claude/skills/coordinator/SKILL.md) through tools/request-queue.sh, so the tests exercise the real queue:
#   - a queued request starts only if no claimed request uses the same map package, and an
#     older queued one on that package goes first;
#   - a code request runs alone: it starts only when nothing else runs, and while it waits
#     or runs nothing newer starts;
#   - at most 3 at once.
# A "subagent" is a background shell that sleeps $MONOLITH_STUB_WORK seconds (default 3) and
# writes result.md. Markers in the prompt: "[code]" = a code request; "[edit]" = the worker
# appends to <package>/stub-edit.txt right after the claim (for the recovery test).
# A cancel file stops the worker and finishes the bundle as cancelled.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
Q="$ROOT/tools/request-queue.sh"
WORK=${MONOLITH_STUB_WORK:-3}
MAX=3
T() { date '+%H:%M:%S.%3N'; }

echo "$(T) stub-coordinator: started (pid $$), args: $#"
"$Q" start

declare -A worker=()
while :; do
  rows=()
  prompts=()
  while IFS= read -r line; do
    if [[ $line == "    prompt "* ]]; then prompts+=("${line#*: }")
    elif [[ $line == *" state="* ]]; then rows+=("$line"); fi
  done < <("$Q" list)

  declare -A busy=()
  running=0 code=0
  # Running ones: finished (result.md), cancelled, or still busy
  for i in "${!rows[@]}"; do
    read -r id rest <<<"${rows[$i]}"; rest=" $rest"
    state=$(sed -nE 's/.* state=([^ ]*).*/\1/p' <<<"$rest")
    pkg=$(sed -nE 's/.* package=([^ ]*).*/\1/p' <<<"$rest")
    [[ $state == claimed || $state == question ]] || continue
    if [[ $rest == *" result=yes"* ]]; then
      "$Q" finish "$id" done > /dev/null && echo "$(T) end $id"
      unset "worker[$id]"
    elif [[ $rest == *" cancel=yes"* ]]; then
      [[ -n ${worker[$id]:-} ]] && kill "${worker[$id]}" 2>/dev/null
      unset "worker[$id]"
      "$Q" finish "$id" cancelled "Cancelled by the developer while running" > /dev/null && echo "$(T) cancelled $id"
    else
      running=$((running + 1))
      busy[$pkg]=1
      [[ ${prompts[$i]} == *"[code]"* ]] && code=1
    fi
  done
  # Queued ones, oldest first
  for i in "${!rows[@]}"; do
    read -r id rest <<<"${rows[$i]}"; rest=" $rest"
    state=$(sed -nE 's/.* state=([^ ]*).*/\1/p' <<<"$rest")
    pkg=$(sed -nE 's/.* package=([^ ]*).*/\1/p' <<<"$rest")
    [[ $state == queued ]] || continue
    if [[ $rest == *" cancel=yes"* ]]; then
      "$Q" finish "$id" cancelled "Cancelled by the developer before it started" > /dev/null && echo "$(T) cancelled $id (queued)"
      continue
    fi
    (( code )) && break                                  # a code request runs or waits: nothing newer starts
    isCode=0; [[ ${prompts[$i]} == *"[code]"* ]] && isCode=1
    if (( isCode && running > 0 )); then code=1; echo "$(T) wait $id (code request: waits until it is alone)"; break; fi
    if [[ -n ${busy[$pkg]:-} ]]; then continue; fi       # same package is claimed (or an older one waits)
    (( running >= MAX )) && break
    "$Q" claim "$id" > /dev/null || continue
    echo "$(T) start $id $pkg$( (( isCode )) && echo " [code]")"
    abs=$pkg; [[ $abs == /* ]] || abs="$ROOT/$abs"
    edit=0; [[ ${prompts[$i]} == *"[edit]"* ]] && edit=1
    (
      (( edit )) && echo "stub edit for $id" >> "$abs/stub-edit.txt"
      sleep "$WORK"
      printf 'Stub coordinator: %s done\n\nNothing was changed (test double).\n' "$id" > "${MONOLITH_REQUESTS_DIR:-$ROOT/requests}/$id/result.md"
    ) &
    worker[$id]=$!
    running=$((running + 1))
    busy[$pkg]=1
    (( isCode )) && code=1
  done
  unset busy
  # Wait for the next event, or a second (workers finish without an event)
  "$Q" wait --timeout 1 > /dev/null
done
