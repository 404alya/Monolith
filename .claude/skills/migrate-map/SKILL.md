---
name: migrate-map
description: Migrate a monolith map (.map text) from an older map format to the one this monolith reads, by the rewrite rules in versions.md, each step proven against a build of the format's last reader (box dump, scene dump, pixel-compared shots), refusing with a report when anything fails. Use when the developer runs /migrate-map <package or .map>, asks to migrate or upgrade a map, or a load fails with "map format N is older than this monolith".
argument-hint: <package folder or .map file>
---

# Migrate a map

Monolith reads exactly one map format (`MS_FORMAT_VERSION` in `runtime/monolith_scene.h`). An older map is migrated here, on request: rewritten step by step by the rules in `versions.md` (next to this file; read the entries you apply in full), and every step proven against a build of the commit that last read the old format. Nothing is written next to the map unless every proof passes.

**Rules for the whole procedure** (with the guard rails in `CLAUDE.md`):
- Never `rm`, `rmdir`, `unlink`, `find -delete`, `git worktree remove` or `prune`, not even in `/tmp`. Scratch folders get fresh names and are kept; list them in the report.
- Never a visible window. Run every binary, old or current, only with `--dump-boxes`, `--dump-scene`, `--dump-scene-diff` or `--shot` (old builds may lack the agent-shell window guard). Never `MONOLITH_ALLOW_WINDOW`.
- One old-commit build at a time (each builds R3D: about a minute on 8+ cores, longer elsewhere).
- Commands below are for zsh or bash: every variable is quoted. Run anything longer than one command as a script file in the scratch folder with `bash <file>` (the agent shell may refuse compound commands).
- Don't commit. `tests/migrate/` holds old-format fixtures on purpose: never migrate them.

## Procedure

1. **Resolve the path.** A folder means `<folder>/<folder>.map`, else its only `.map`; a `.map` file is itself. The map's folder is the package root (copied whole in step 4). The map must be inside the repo:
   ```bash
   MAP="$(realpath "<path>/<name>.map")"; ROOT="$(git rev-parse --show-toplevel)"
   case "$MAP" in "$ROOT"/*) echo inside ;; *) echo outside ;; esac
   ```
   Outside: say "copy the package into `maps/` first, then run `/migrate-map maps/<name>`" and stop (refusal reason `outside the repo`). No `.map` there: `not a map`.
   - `examples/<n>/scene/` packages migrate like any other. Under `tests/`: find the test that uses the map first (`grep -rn "<file>" tests tools`); a test map may be old or broken on purpose. Never `tests/migrate/`.
   - A map in a git submodule (its own repo) or a gitignored one migrates the same way; say in the report that the `.v<N>` copy is its only backup when `git check-ignore -q "$MAP"` succeeds or the file is untracked.
   - Record the hash now: `sha256sum "$MAP"`.

2. **Read the versions.** The map's version N is the first statement's (after a UTF-8 BOM, skipping blank and comment lines): `monolith N`, or 1 when the first statement is anything else. The current version M:
   ```bash
   awk 'NR == 1 { sub(/^\xef\xbb\xbf/, "") } { sub(/#.*/, "") } NF { print ($1 == "monolith" ? $2 : 1); exit }' "$MAP"
   awk '$1 == "#define" && $2 == "MS_FORMAT_VERSION" { print $3 }' runtime/monolith_scene.h
   ```
   No output for the map: only comments, nothing to migrate (monolith says `map is empty`). N = M: say "already format M" and stop. N > M: refuse, `newer than this monolith` ("update monolith").

3. **Build every reader first.** The steps are the `versions.md` entries N → N+1 … M−1 → M. Build each distinct `last-reader:` commit, one at a time, then the current monolith:
   ```bash
   OLD_2="$(build/tools/old-monolith <last-reader of 1 -> 2>)"; echo "exit $?"
   ./build.sh
   CUR="$ROOT/build/linux/monolith"
   ```
   `build/tools/old-monolith` prints only the binary's path on stdout (exit 0), reuses a finished build instantly, and on failure (exit 2–4) prints `FAILED`, the log's last lines and `left behind:` on stderr. Any failure here: refuse, `last-reader build failed`, with that stderr as evidence. Keep every `left behind:` line for the report.

4. **Rewrite, one copy per step.** A fresh scratch folder, then a whole-package copy per version, with the map renamed `step-<K>.map` (K = the format it holds), so assets resolve and shots render:
   ```bash
   S="<scratchpad or /tmp>/migrate-<name>-$(date +%Y%m%d-%H%M%S)"; mkdir "$S"     # must not exist yet
   N=<map's version>; cp -a "$(dirname "$MAP")" "$S/step-$N"; mv "$S/step-$N/<name>.map" "$S/step-$N/step-$N.map"
   K=$N; cp -a "$S/step-$K" "$S/step-$((K+1))"; mv "$S/step-$((K+1))/step-$K.map" "$S/step-$((K+1))/step-$((K+1)).map"   # then edit it; repeat per step
   ```
   A format-2 map starts at `step-2`, a format-1 map at `step-1`. Apply the step's rewrite rules to `step-<K+1>.map` in order, by editing the file (Edit tool, or a short `python3 -I` script you write). A line no rule names stays byte for byte, comments and spacing included; on a changed line, the text after the last rewritten number (spaces before a `#` comment too) stays as it was. Find the candidate lines with `grep -nE '(^|[[:space:]])at[[:space:]]' step-2.map` and skip hits inside a comment or a `"string"`. A line a rule should cover but can't (e.g. `at` with 3 `repeat`s): refuse, `rule not covered: line L`, quoting the line. Count the lines each rule changed, and whether a format line was inserted (it moves every later line by one).

5. **Prove each step, then the chain.** Write "The proof script" (below) to `$S/prove.sh` once and run it per step. All checks must pass.

   | Proof | before | after | line shift |
   |---|---|---|---|
   | step K → K+1 | that step's last reader on `step-K.map` | the reader of K+1: the next step's last reader, or `$CUR` when K+1 = M | 1 if that step inserted a line, else 0 |
   | whole chain (more than one step) | first step's last reader on `step-N.map` | `$CUR` on `step-M.map` | lines inserted in all steps |

   ```bash
   bash "$S/prove.sh" "$OLD_2" "$S/step-1/step-1.map" "$OLD_2" "$S/step-2/step-2.map" "$S/proof-1-2" 1
   bash "$S/prove.sh" "$OLD_3" "$S/step-2/step-2.map" "$CUR" "$S/step-3/step-3.map" "$S/proof-2-3" 0
   ```
   **A fold that isn't exact** (2 → 3, rule 2a): when the box dump differs, diff `$S/proof-…/before.scene.cmp` against `after.scene.cmp`. Every differing `box <i> … line L` (or `light … line L`, `instance … line L`) names a line whose fold changed a float. Rewrite each such line in form (b) from its text in `step-K.map` (`at x y z` removed, `repeat 1 x y z from 1` as the first repeat, after `id n`, the numbers as they were), then prove the step again into a new folder (`$S/proof-2-3-r2`, `-r3`, …; never reuse a proof folder). The *last proof dir* in step 7 is the last one that passed. A folded `light` or `model` line only shows in the scene dump, never in the box dump.

6. **Any failure: refuse.** Print the refusal report exactly in this form, write nothing next to the map, keep `$S`, and show `sha256sum "$MAP"` again (it must equal step 1's):
   ```
   /migrate-map: REFUSED <map path> (format N -> M)
   reason: <one of: last-reader build failed | proof failed: <boxes|scene|shot <view>> | rule not covered: line L | newer than this monolith | outside the repo | not a map>
   step: N -> N+1 (last-reader <commit>)
   evidence:
     <for a build failure: old-monolith's FAILED line, the exit code, and the log path and last log lines when it made a log (exit 2 makes none)>
     <for a proof failure: the first 20 lines of the dump diff, or the image pair's paths and the changed-pixel count>
     <for rule not covered: the line, quoted>
   map: unchanged (sha256 <hash> before and after); nothing written next to it
   scratch: <scratch folder (kept for inspection), or: none (refused before step 4)>
   left in /tmp: <what old-monolith listed as left behind, or: reused <folder>, or: nothing>
   ```

7. **Success only: write.**
   - If `<name>.map.v<N>` already exists next to the map, stop and ask the developer (nothing written yet). Otherwise:
     ```bash
     cp -p "$MAP" "$MAP.v<N>"
     cp "$S/step-<M>/step-<M>.map" "$MAP"
     "$CUR" --map "$MAP" --dump-boxes "$S/final.boxes" && cmp "$S/final.boxes" "$S/<last proof dir>/after.boxes"
     ```
     The last proof dir is the chain's, or the only step's. If this check fails, copy `$MAP.v<N>` back over `$MAP` and refuse.
   - `MAP.md`: update only text that quotes syntax that changed (an `at` line, `monolith 2`): `grep -nE 'monolith [0-9]|`[^`]*(^|[[:space:]])at -?[0-9.]+ -?[0-9.]+ -?[0-9.]+' MAP.md` (syntax in backticks or code blocks; prose like "eye at 1.62 m" stays). Nothing else.
   - A `<name>.mapb` next to the map: don't touch it; the report says "compile it again with `--compile`".
   - Print the success report. Add `compiled: <name>.mapb is now stale: compile it again with --compile` for a `.mapb`, and `backup: <name>.map.v<N> is the only copy of the original (the map is gitignored or untracked)` when step 1 found that:
     ```
     /migrate-map: MIGRATED <map path> (format N -> M)
     steps: N -> N+1 (rules used, lines changed) ...; proofs: boxes identical, scene equal (map line only), shots 0 changed pixels at <k> views
     original kept: <name>.map.v<N> (remove it when you no longer need it)
     MAP.md: <updated lines | unchanged>
     ```
     Then list the scratch folder and what `build/tools/old-monolith` left in `/tmp`.

## The proof script

Copy it as it is (the Python lines start at column 0).

```bash
#!/usr/bin/env bash
# prove.sh <before-bin> <before.map> <after-bin> <after.map> <out dir> [line shift]
# The three checks of one proof. Prints one line per check; exit 0 only if all pass.
set -u
B="$1"; BM="$2"; A="$3"; AM="$4"; OUT="$5"; SHIFT="${6:-0}"
mkdir -p "$OUT"; fail=0
run() { "$@" >>"$OUT/run.log" 2>&1 || { echo "RUN FAILED: $* (see $OUT/run.log)"; fail=1; }; }

run "$B" --map "$BM" --dump-boxes "$OUT/before.boxes"
run "$A" --map "$AM" --dump-boxes "$OUT/after.boxes"
if cmp -s "$OUT/before.boxes" "$OUT/after.boxes"; then echo "boxes: identical"
else echo "boxes: DIFFER"; diff "$OUT/before.boxes" "$OUT/after.boxes" | head -n 20; fail=1; fi

for k in scene scene-diff; do
  run "$B" --map "$BM" --dump-$k "$OUT/before.$k"
  run "$A" --map "$AM" --dump-$k "$OUT/after.$k"
  # drop the `map "<path>" format N` line; move the before side's `line L` by SHIFT (L > 0: line 0 is built in)
  python3 -I -c 'import re,sys
s=int(sys.argv[3])
for src,dst,sh in ((sys.argv[1],sys.argv[1]+".cmp",s),(sys.argv[2],sys.argv[2]+".cmp",0)):
    t=[l for l in open(src) if not l.startswith("map \"")]
    open(dst,"w").writelines(re.sub(r" line (\d+)",lambda m:" line %d"%(int(m.group(1))+(sh if int(m.group(1)) else 0)),l) for l in t)' \
    "$OUT/before.$k" "$OUT/after.$k" "$SHIFT"
  if cmp -s "$OUT/before.$k.cmp" "$OUT/after.$k.cmp"; then echo "$k: equal (map line only)"
  else echo "$k: DIFFER"; diff "$OUT/before.$k.cmp" "$OUT/after.$k.cmp" | head -n 20; fail=1; fi
done

nv=$(grep -cE '^[[:space:]]*viewpoint[[:space:]]' "$AM")
read -r sx sy sz < <(awk '$1 == "spawn" { print $2, $3, $4; f = 1; exit } END { if (!f) print 0, 0, 0 }' "$AM")
views=(start); for ((v = 1; v <= nv; v++)); do views+=("v$v"); done; views+=(y0 y90 y180 y270)
for view in "${views[@]}"; do
  case "$view" in
    start) args=() ;;
    v*)    args=(--view "${view#v}") ;;
    y*)    args=(--pos "$sx" "$sy" "$sz" --look "${view#y}" 0) ;;
  esac
  run "$B" --map "$BM" "${args[@]}" --shot "$OUT/before-$view.png"
  run "$A" --map "$AM" "${args[@]}" --shot "$OUT/after-$view.png"
  ae=$(python3 -I -c 'import sys; from PIL import Image, ImageChops
a, b = (Image.open(p).convert("RGBA") for p in sys.argv[1:3])
print(-1 if a.size != b.size else sum(1 for px in ImageChops.difference(a, b).getdata() if any(px)))' "$OUT/before-$view.png" "$OUT/after-$view.png" 2>/dev/null || echo error)
  if [[ "$ae" == "0" ]]; then echo "shot $view: 0 changed pixels"
  else echo "shot $view: $ae changed pixels (-1: sizes differ) ($OUT/before-$view.png $OUT/after-$view.png)"; fail=1; fi
done
exit "$fail"
```

## Adding a step

A change to the map format adds its entry to `versions.md` in the same change: the `engine-code` skill, "Change the map format". Its entry follows the entry format at the top of `versions.md`; this procedure needs no change unless the proof itself changes. Add a fixture under `tests/migrate/` with its `expected.map`.
