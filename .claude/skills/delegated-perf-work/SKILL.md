---
name: delegated-perf-work
description: Run a performance investigation or optimisation through subagents while the main session only plans, checks and reports — which model and effort per job, the order of the phases (code analysis, distance sweep, on/off, cost floor, change with pixel proof, A/B), what every brief must carry, scratch packages with their own viewpoints, and what the main session verifies itself. Use when the developer asks to investigate why something is slow, or to optimise it, and to hand the code reading, edits or benchmarks to subagents.
---

# Delegated performance work

The main session keeps the context small: it writes briefs, spot-checks the claims that decide the
plan, and reports to the developer. Subagents read the code, edit, shoot and benchmark. Every number
still follows the `benchmarking` skill; every change still follows its own skill (`surface-shaders`,
`engine-code`, `map-editing`).

## 1. Who does what

| Job | Model, effort | Notes |
|---|---|---|
| Read the code: how the thing is built, cost per pixel / per draw, which passes draw it | Haiku, high | read-only, no game runs |
| Distance sweep, on/off, cost floor, A/B | Haiku, medium | the GPU job: **one at a time**, never next to another bench or shot run |
| Mechanical change (reorder, flag, map key) with a pixel proof | Haiku, high | give the exact edit in the brief |
| Design-heavy change (a bake, a new code path, R3D) | Sonnet, high (or the main session) | the main session writes the design first |

Run the code reader and a GPU job in parallel; never two GPU jobs. Full runs take ~4 min each, so
a 6-run A/B is ~25 min: launch it in the background and keep working on non-GPU steps (docs, skill,
design).

## 2. Phases

1. **How it works**: the code reader reports map lines → mesh / instancing → shader, per-fragment
   cost (texture reads, loops, early outs), and the passes that draw it (pre-pass or not, `discard`
   and early-Z, `cull`, shadow and cube passes), with `file:line`.
2. **Where the cost goes**: a sweep (e.g. 30 / 15 / 8 / 4 / 2 / 1 m screening runs), then full-run
   on/off pairs at a far and a near view. The delta of the pass that grows is the object's cost.
3. **Plan**: rank the fixes by cost and risk; pixel-identical ones first.
4. **Cost floor before an expensive fix**: replace the shader in a scratch copy with a trivial one of
   similar coverage (and variants such as `cull back`) and measure it against on and off. The gap
   between current and floor is all an optimisation of that shader can win; if the floor is most of
   the cost, fix the draw (culling, pre-pass, overdraw), not the shader.
5. **Change**: the agent saves the base file to scratch first, edits, validates (`check_surface_shader.py`
   / `glslangValidator`), and shoots every viewpoint plus the near views with base and change:
   differing-pixel count per pair (0 for a pixel-identical claim). A look change goes to the
   developer as before / after images.
6. **A/B**: two packages identical except the change, full runs A,B,B,A,A,B, paired deltas.
7. **Record**: `measurements.md` (machine, method, deltas), the map's `MAP.md` for a map's shader.

## 3. What every brief carries

The agent knows nothing else, so each brief states:
- the repo path, the scratch directory, and "do not edit tracked files" (or exactly which one to edit);
- the guard rails: no visible window (`--bench --headless`, `--shot`, dump flags only), no `rm`
  (`gio trash`), one benchmark at a time, `__GL_SYNC_TO_VBLANK=0` and `--note` on every run, zsh/fish
  safe commands (full paths, no unquoted `$VAR` splitting), no commit;
- the skill to read first (`benchmarking`, `surface-shaders`, …);
- the exact target (map lines, the sheet / box, the viewpoint numbers) and the exact edit;
- the report shape and a word limit (tables, numbers, report folder names, no speculation).
Don't hand the agent your hypothesis for the code reader: ask what the passes do, not "confirm X".

## 4. Scratch packages

- A test package is a copy of the map package in scratch (`cp -r <pkg> <S>/<name>/<map>`: check that
  `<S>/<name>/<map>/<map>.map` exists; `cp -r` into a missing directory flattens the tree), run with
  `--map`. Its viewpoint lines are replaced by the test cameras; benches use `--bench-still` (the
  default bench turns the camera).
- "Off" = the object's group lines commented out in the copy, never an edit of the real map.
- Base and change packages must differ only in the changed file: `diff -rq` them before the A/B.
- A map package may be a git submodule: `git -C maps/<map> diff`, and the developer may have
  uncommitted work in it; the change goes on top, nothing of theirs is touched.

## 5. What the main session checks itself

Subagent reports are claims. Before acting on one, check the few facts the plan rests on with a
quick read (the line that disables the pre-pass, the loop that does the reads), the diff of every
edit, and that the A/B packages differ only where intended. Then report to the developer in their
language: what was measured (machine, runs), the deltas, what is verified and what is not.
