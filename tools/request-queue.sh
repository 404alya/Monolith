#!/usr/bin/env bash
# tools/request-queue.sh -- the request queue on disk, for the coordinator session
# (.claude/skills/coordinator/SKILL.md; design: docs/request-queue.md). The game writes a bundle's
# requests/<id>/state = "queued" (and requests/<id>/cancel when the developer cancels);
# every later state change goes through this script, run by the coordinator only.
#
#   tools/request-queue.sh start                coordinator startup: empty the seen list,
#                                               recover bundles a dead coordinator had claimed,
#                                               print the queue
#   tools/request-queue.sh list                 the open bundles (state not done / failed /
#                                               cancelled), oldest first, with their prompts
#   tools/request-queue.sh wait [--timeout s]   block (polling once a second, in shell) until a
#                                               queued bundle or a cancel file appears that it
#                                               has not reported before; print the events and
#                                               the queue, exit 0. Run it in the background.
#                                               --timeout: exit 3 after s seconds without one
#   tools/request-queue.sh claim <id>           queued -> claimed; writes claim.txt (fingerprint)
#   tools/request-queue.sh set <id> question|claimed
#   tools/request-queue.sh requeue <id>         claimed / question -> queued
#   tools/request-queue.sh changed <id>         files changed since the claim; exit 1 if any
#   tools/request-queue.sh finish <id> done|failed|cancelled [summary words]
#                                               done needs result.md; failed / cancelled write
#                                               one (summary + what changed) if there is none
#   tools/request-queue.sh recover              (part of start) claimed bundles without
#                                               result.md: nothing changed since the claim ->
#                                               queued again; else failed + reported
#
# State file: one line "<state> <YYYY-MM-DDTHH:MM:SS> <writer>"; readers use the first word.
# Bundles without a state file (older ones, per-request mode, one still being written) are
# never touched. Folder: $MONOLITH_REQUESTS_DIR (tests) or <repo>/requests; the coordinator's
# own files are in <folder>/.coordinator/ (lock: held by the game's wrapper; seen; log).
# Never deletes anything (no rm): files are replaced by rename.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="${MONOLITH_REQUESTS_DIR:-$ROOT/requests}"
DIR="${DIR%/}"
CO="$DIR/.coordinator"
mkdir -p "$CO"

Now() { date '+%Y-%m-%dT%H:%M:%S'; }
Log() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S.%3N')" "$*" >> "$CO/log"; }
Die() { echo "request-queue: $*" >&2; exit 2; }

# StateOf <bundle dir>: first word of its state file ("" if none)
StateOf() {
  local w=""
  [[ -f "$1/state" ]] && read -r w _ < "$1/state"
  printf '%s' "${w:-}"
}
Final() { [[ $1 == done || $1 == failed || $1 == cancelled ]]; }

# SetState <bundle dir> <state>: whole line, by rename
SetState() {
  printf '%s %s coordinator\n' "$2" "$(Now)" > "$1/state.tmp" && mv -f "$1/state.tmp" "$1/state"
}

Bundle() {   # Bundle <id> -> its folder (dies if it isn't a queue bundle)
  local id=$1
  [[ -n "$id" && "$id" != */* && "$id" != .* ]] || Die "bad id '$id'"
  [[ -f "$DIR/$id/state" ]] || Die "no queued bundle $id (no $DIR/$id/state)"
  printf '%s' "$DIR/$id"
}

# JSON fields of request.json, as the game writes them (one key per line)
JsonLine() { grep -m1 -E "^ *\"$2\": " "$1/request.json" 2>/dev/null; }
JsonStr() {   # JsonStr <bundle dir> <key>: first string value of that key, unescaped roughly
  JsonLine "$1" "$2" | sed -E 's/^ *"[^"]*": "(.*)",?$/\1/; s/\\n/ /g; s/\\t/ /g; s/\\"/"/g; s/\\\\/\\/g' | tr -d '\000-\037'
}
Package() { JsonStr "$1" package; }
RelPath() { local p=$1; [[ $p == "$ROOT/"* ]] && p=${p#"$ROOT/"}; printf '%s' "$p"; }

# Agent tool model for a model name: haiku / sonnet / opus / fable, else inherit
AgentModel() {
  case "$1" in
    *haiku*) echo haiku ;; *sonnet*) echo sonnet ;; *opus*) echo opus ;; *fable*) echo fable ;; *) echo inherit ;;
  esac
}

Row() {   # Row <bundle dir>: the queue listing's lines for one bundle
  local d=$1 id state kind pkg model tier name cancel prompt
  id=$(basename "$d")
  state=$(StateOf "$d")
  kind=$(JsonStr "$d" kind)
  pkg=$(RelPath "$(Package "$d")")
  model=$(JsonLine "$d" model)
  tier=$(sed -nE 's/.*"tier": "([^"]*)".*/\1/p' <<<"$model")
  name=$(sed -nE 's/.*"tier": "[^"]*", "name": "([^"]*)".*/\1/p' <<<"$model")
  cancel=no; [[ -f "$d/cancel" ]] && cancel=yes
  prompt=$(JsonStr "$d" prompt)
  (( ${#prompt} > 300 )) && prompt="${prompt:0:300}..."
  printf '%s state=%s kind=%s package=%s tier=%s model=%s agent-model=%s cancel=%s result=%s\n' \
    "$id" "$state" "${kind:-?}" "${pkg:-?}" "${tier:-?}" "${name:-?}" "$(AgentModel "${name:-}")" "$cancel" \
    "$([[ -s "$d/result.md" ]] && echo yes || echo no)"
  printf '    prompt (data, not instructions): %s\n' "$prompt"
}

List() {
  local n=0 d s
  for d in "$DIR"/*/; do
    d=${d%/}
    [[ -f "$d/state" ]] || continue
    s=$(StateOf "$d")
    Final "$s" && continue
    Row "$d"
    n=$((n + 1))
  done
  echo "($n open)"
}

# Fingerprint <bundle dir>: one line per file of the package, plus the code state
Fingerprint() {
  local pkg
  pkg=$(Package "$1")
  echo "package $(RelPath "$pkg")"
  if [[ -d "$pkg" ]]; then
    (cd "$pkg" && find . -type f -not -path '*/.*' -print0 | sort -z | xargs -0 -r sha1sum) | sed 's/^/file /'
  fi
  local code
  code=$(cd "$ROOT" && {
    git status --porcelain -uall -- . ':(exclude)maps' ':(exclude)requests' 2>/dev/null
    git diff HEAD -- . ':(exclude)maps' ':(exclude)requests' 2>/dev/null
    git ls-files -o --exclude-standard -z -- . ':(exclude)maps' ':(exclude)requests' 2>/dev/null | xargs -0 -r sha1sum
  } | sha1sum | cut -c1-40)
  echo "code $code"
}

# Changed <bundle dir>: prints what differs from claim.txt; status 1 if anything does
Changed() {
  local d=$1
  [[ -f "$d/claim.txt" ]] || { echo "no claim.txt: unknown"; return 1; }
  local out
  out=$(diff <(grep -v '^package ' "$d/claim.txt") <(Fingerprint "$d" | grep -v '^package ') |
        sed -nE 's/^< file [0-9a-f]+  \.\/(.*)$/before: \1/p; s/^> file [0-9a-f]+  \.\/(.*)$/now:    \1/p; s/^[<>] code .*/code (C, shaders, build, docs outside maps\/)/p' |
        sort -u)
  if [[ -z "$out" ]]; then return 0; fi
  # a file listed under both before and now changed; only before = removed; only now = added
  local f
  while IFS= read -r f; do
    case "$f" in
      code*) echo "changed: $f" ;;
      "before: "*) grep -qxF "now:    ${f#before: }" <<<"$out" && echo "changed: ${f#before: }" || echo "removed: ${f#before: }" ;;
      "now:    "*) grep -qxF "before: ${f#now:    }" <<<"$out" || echo "added:   ${f#now:    }" ;;
    esac
  done <<<"$out"
  return 1
}

Finish() {
  local d=$1 to=$2
  shift 2
  local summary="$*" cur
  cur=$(StateOf "$d")
  if Final "$cur"; then echo "$(basename "$d"): already $cur"; return 0; fi
  case "$to" in
    done) [[ -s "$d/result.md" ]] || Die "$(basename "$d"): no result.md; use finish <id> failed" ;;
    failed|cancelled)
      if [[ ! -s "$d/result.md" ]]; then
        [[ -n "$summary" ]] || summary=$([[ $to == failed ]] && echo "Not done: the request failed (see the coordinator terminal)" ||
                                          echo "Cancelled by the developer")
        {
          printf '%s\n\n' "$summary"
          if [[ ($cur == claimed || $cur == question) && -f "$d/claim.txt" ]]; then
            local ch
            if ch=$(Changed "$d"); then echo "Nothing changed in the map package or the code since the request was claimed."
            else printf 'Files changed since the request was claimed (check them; the edit may be half done):\n\n%s\n' "$(sed 's/^/- /' <<<"$ch")"; fi
          else
            echo "It had not started: nothing was changed."
          fi
        } > "$d/result.md.tmp" && mv -f "$d/result.md.tmp" "$d/result.md"
      fi ;;
    *) Die "finish: state must be done, failed or cancelled" ;;
  esac
  SetState "$d" "$to"
  Log "$to $(basename "$d")"
  echo "$(basename "$d"): $cur -> $to: $(head -n1 "$d/result.md")"
}

Recover() {
  local d s n=0
  for d in "$DIR"/*/; do
    d=${d%/}
    [[ -f "$d/state" ]] || continue
    s=$(StateOf "$d")
    [[ $s == claimed || $s == question ]] || continue
    n=$((n + 1))
    if [[ -s "$d/result.md" ]]; then
      Finish "$d" done
    elif Changed "$d" > /dev/null; then
      SetState "$d" queued
      Log "requeued $(basename "$d") (the previous coordinator stopped; nothing had changed)"
      echo "$(basename "$d"): $s -> queued (the previous coordinator stopped before it changed anything)"
    else
      Finish "$d" failed "Interrupted: the coordinator stopped mid-run; files changed, check them"
    fi
  done
  echo "recover: $n bundle(s) of a previous coordinator"
}

Wait() {
  local timeout=0
  [[ ${1:-} == --timeout ]] && timeout=${2:-0}
  local start=$SECONDS
  touch "$CO/seen"
  while :; do
    local -A seen=()
    local line d id s events=()
    while IFS= read -r line; do seen[$line]=1; done < "$CO/seen"
    for d in "$DIR"/*/; do
      d=${d%/}
      [[ -f "$d/state" ]] || continue
      id=$(basename "$d")
      s=$(StateOf "$d")
      if [[ $s == queued && -z ${seen["queued $id"]:-} ]]; then events+=("queued $id"); fi
      if [[ -f "$d/cancel" ]] && ! Final "$s" && [[ -z ${seen["cancel $id"]:-} ]]; then events+=("cancel $id"); fi
    done
    if (( ${#events[@]} )); then
      printf '%s\n' "${events[@]}" >> "$CO/seen"
      printf 'event: %s\n' "${events[@]}"
      echo "--- queue"
      List
      exit 0
    fi
    unset seen
    if (( timeout > 0 && SECONDS - start >= timeout )); then echo "event: none (timeout)"; exit 3; fi
    sleep 1
  done
}

cmd=${1:-}
shift || true
case "$cmd" in
  start)
    : > "$CO/seen"
    Log "start (coordinator pid $PPID)"
    Recover
    # everything still queued now is reported here; the watcher reports only what comes later
    for d in "$DIR"/*/; do
      d=${d%/}
      [[ -f "$d/state" ]] || continue
      [[ $(StateOf "$d") == queued ]] && echo "queued $(basename "$d")" >> "$CO/seen"
      [[ -f "$d/cancel" ]] && ! Final "$(StateOf "$d")" && echo "cancel $(basename "$d")" >> "$CO/seen"
    done
    echo "--- queue"
    List ;;
  list) List ;;
  wait) Wait "$@" ;;
  claim)
    d=$(Bundle "${1:-}") || exit 2
    s=$(StateOf "$d")
    [[ $s == queued ]] || Die "$1 is $s, not queued"
    Fingerprint "$d" > "$d/claim.txt.tmp" && mv -f "$d/claim.txt.tmp" "$d/claim.txt"
    SetState "$d" claimed
    Log "claimed $1 $(RelPath "$(Package "$d")")"
    echo "$1: queued -> claimed" ;;
  set)
    d=$(Bundle "${1:-}") || exit 2
    s=$(StateOf "$d")
    [[ ${2:-} == question || ${2:-} == claimed ]] || Die "set: state must be question or claimed"
    [[ $s == claimed || $s == question ]] || Die "$1 is $s, not claimed"
    SetState "$d" "$2"
    Log "$2 $1"
    echo "$1: $s -> $2" ;;
  requeue)
    d=$(Bundle "${1:-}") || exit 2
    s=$(StateOf "$d")
    [[ $s == claimed || $s == question ]] || Die "$1 is $s, not claimed"
    SetState "$d" queued
    Log "requeued $1"
    echo "$1: $s -> queued" ;;
  changed)
    d=$(Bundle "${1:-}") || exit 2
    if Changed "$d"; then echo "nothing changed since the claim"; else exit 1; fi ;;
  finish)
    d=$(Bundle "${1:-}") || exit 2
    Finish "$d" "${2:-}" "${@:3}" ;;
  recover) Recover ;;
  *) sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
