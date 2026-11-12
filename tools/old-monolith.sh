#!/usr/bin/env bash
# tools/old-monolith.sh -- build monolith as it was at an older commit; print the binary's path
#
#   tools/old-monolith.sh <commit>
#
# The migrate-map skill uses it to build a map format's last reader (the `last-reader:` lines of
# .claude/skills/migrate-map/versions.md) and prove a migration against it.
#
# - <commit> is resolved in the main repository (the checkout that owns this repo's git objects, also
#   when this script runs from a worktree). Folders are named after its first 12 hex digits.
# - Reuse: if /tmp/monolith-old-<hash12>-<n>/ holds .old-monolith-done with the full hash and its
#   build/linux/monolith exists, that binary is printed without building (the lowest such n wins).
# - Otherwise: a fresh worktree /tmp/monolith-old-<hash12>-<n>/ (the first n with nothing of that name,
#   nor a <n>.log), `git worktree add --detach`; submodules/r3d initialised at the commit's gitlink from
#   the main checkout's objects when the commit has it as a submodule (no network; commits that vendor
#   R3D as a plain tree need nothing); then the commit's own ./build.sh, with R3D_DIR unset (the
#   commit's own R3D) and PATH led by rm/rmdir/unlink shims that only log. Log:
#   /tmp/monolith-old-<hash12>-<n>.log. On success it writes .old-monolith-done.
# - stdout: exactly one line on success, the binary's absolute path. Progress and errors go to stderr
#   ("old-monolith: ..."); on failure: FAILED, the log, its last 20 lines, and always "left behind:".
# - It never deletes anything (no rm, no git worktree remove / prune); it lists what it leaves.
# - Old binaries may lack the agent-shell window guard: run them only with --dump-boxes, --dump-scene,
#   --dump-scene-diff or --shot. Build one old commit at a time (a full R3D build each).
#
# Exit codes: 0 built or reused; 1 usage; 2 no git history or commit not found;
#             3 worktree or submodule setup failed; 4 the build failed (non-zero exit, or no binary).
set -euo pipefail

PROG="old-monolith"
say() { printf '%s: %s\n' "$PROG" "$*" >&2; }

CREATED=()   # everything this run created, for "left behind:"
LOG=""

left_behind() {
    if ((${#CREATED[@]})); then
        say "left behind: ${CREATED[*]}"
    else
        say "left behind: nothing"
    fi
}

# fail <exit code> <reason>
fail() {
    local code="$1"; shift
    say "FAILED: $*"
    if [[ -n "$LOG" && -f "$LOG" ]]; then
        say "log: $LOG"
        say "last log lines:"
        tail -n 20 "$LOG" >&2 || true
    fi
    left_behind
    exit "$code"
}

usage() {
    say "usage: tools/old-monolith.sh <commit>   (build monolith as it was at <commit>; print the binary's path)"
    exit 1
}

[[ $# -eq 1 && -n "$1" && "$1" != -* ]] || usage
WANT="$1"

# The repo this script lives in, and the main checkout that owns its objects (the common git dir's
# parent: a worktree's --git-common-dir is <main>/.git).
SELF_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMMON="$(git -C "$SELF_ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)" \
    || fail 2 "no git history at $SELF_ROOT"
if [[ "$(basename "$COMMON")" == ".git" ]]; then
    MAIN="$(dirname "$COMMON")"
else
    MAIN="$SELF_ROOT"   # a bare or unusual layout: use the script's own checkout
fi

FULL="$(git -C "$MAIN" rev-parse --verify --quiet "${WANT}^{commit}" 2>/dev/null)" \
    || fail 2 "commit not found: $WANT (in $MAIN)"
H12="${FULL:0:12}"
BASE="/tmp/monolith-old-$H12"

# Reuse a finished build: the lowest n whose marker holds the full hash and whose binary exists.
REUSE=""
REUSE_N=""
for d in "$BASE"-*; do
    [[ -d "$d" ]] || continue
    n="${d##*-}"
    [[ "$n" =~ ^[0-9]+$ ]] || continue
    [[ -f "$d/.old-monolith-done" && -x "$d/build/linux/monolith" ]] || continue
    [[ "$(cat "$d/.old-monolith-done")" == "$FULL" ]] || continue
    if [[ -z "$REUSE_N" ]] || ((n < REUSE_N)); then
        REUSE="$d"; REUSE_N="$n"
    fi
done
if [[ -n "$REUSE" ]]; then
    say "reusing the finished build of $H12 in $REUSE"
    printf '%s\n' "$REUSE/build/linux/monolith"
    exit 0
fi

# A fresh name: nothing called <base>-<n> (file, folder or dangling link) and no <base>-<n>.log.
n=1
while [[ -e "$BASE-$n" || -L "$BASE-$n" || -e "$BASE-$n.log" || -L "$BASE-$n.log" ]]; do
    n=$((n + 1))
done
WT="$BASE-$n"
LOG="$BASE-$n.log"
: > "$LOG" || fail 3 "cannot create the log $LOG"
CREATED+=("$LOG")
say "building $H12 ($FULL) in $WT (log: $LOG)"

if ! git -C "$MAIN" worktree add --detach "$WT" "$FULL" >>"$LOG" 2>&1; then
    [[ -e "$WT" ]] && CREATED+=("$WT")
    fail 3 "git worktree add failed"
fi
CREATED+=("$WT" "worktree record $COMMON/worktrees/$(basename "$WT")")

# init_gitlink <path in worktree> <commit> <candidate source repos...>
# Clones a submodule from a local repository that has the commit (GIT_ALLOW_PROTOCOL=file: never the
# network) and checks the commit out detached. Prints the source used.
init_gitlink() {
    local dst="$1" sha="$2"; shift 2
    local src
    for src in "$@"; do
        [[ -e "$src" ]] || continue
        git -C "$src" cat-file -e "${sha}^{commit}" 2>/dev/null || continue
        if GIT_ALLOW_PROTOCOL=file git clone --quiet --no-checkout --no-hardlinks "$src" "$dst" >>"$LOG" 2>&1 \
            && git -C "$dst" checkout --quiet --detach "$sha" >>"$LOG" 2>&1; then
            printf '%s\n' "$src"
            return 0
        fi
        return 1
    done
    return 1
}

# gitlink <commit-ish> <path> [repo]: the gitlink's commit when <path> is a submodule there, else empty.
gitlink() {
    git -C "${3:-$MAIN}" ls-tree "$1" -- "$2" | awk '$1 == "160000" { print $3 }'
}

R3D_SHA="$(gitlink "$FULL" submodules/r3d)"
if [[ -n "$R3D_SHA" ]]; then
    say "submodules/r3d is a submodule at $R3D_SHA: initialising it from local objects"
    R3D_SRC="$(init_gitlink "$WT/submodules/r3d" "$R3D_SHA" \
        "$COMMON/modules/submodules/r3d" "$MAIN/submodules/r3d")" \
        || fail 3 "no local repository has submodules/r3d at $R3D_SHA (no network is used)"
    # R3D's own submodules (raylib, assimp, ... when they are submodules at that commit)
    while read -r mode _ sha path; do
        [[ "$mode" == "160000" ]] || continue
        init_gitlink "$WT/submodules/r3d/$path" "$sha" \
            "$R3D_SRC/modules/$path" "$(git -C "$R3D_SRC" rev-parse --absolute-git-dir 2>/dev/null)/modules/$path" \
            "$MAIN/submodules/r3d/$path" >/dev/null \
            || fail 3 "no local repository has submodules/r3d/$path at $sha (no network is used)"
    done < <(git -C "$WT/submodules/r3d" ls-tree -r HEAD)
else
    say "submodules/r3d is a plain tree at $H12: nothing to initialise"
fi

[[ -f "$WT/build.sh" ]] || fail 4 "the commit has no build.sh"

# The rm shims: they log their arguments and delete nothing (the build is in a fresh folder, so a
# missing delete leaves only stale files that the build overwrites).
SHIM="$WT/.old-monolith-shim"
mkdir -p "$SHIM" || fail 3 "cannot create $SHIM"
for tool in rm rmdir unlink; do
    cat > "$SHIM/$tool" <<EOF
#!/bin/sh
echo "$tool shim (not deleting): \$*" >&2
exit 0
EOF
    chmod +x "$SHIM/$tool"
done

say "running the commit's ./build.sh (the first build of a commit builds R3D: a few minutes)"
START=$SECONDS
set +e
(cd "$WT" && env -u R3D_DIR PATH="$SHIM:$PATH" ./build.sh) >>"$LOG" 2>&1
STATUS=$?
set -e
ELAPSED=$((SECONDS - START))
printf 'old-monolith: build exit=%s secs=%s\n' "$STATUS" "$ELAPSED" >>"$LOG"

if ((STATUS != 0)); then
    fail 4 "./build.sh exited with $STATUS after ${ELAPSED}s"
fi
BIN="$WT/build/linux/monolith"
[[ -x "$BIN" ]] || fail 4 "./build.sh succeeded but $BIN does not exist"

printf '%s\n' "$FULL" > "$WT/.old-monolith-done" || fail 4 "cannot write $WT/.old-monolith-done"
say "built $H12 in ${ELAPSED}s"
left_behind
printf '%s\n' "$BIN"
