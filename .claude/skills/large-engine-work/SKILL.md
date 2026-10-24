---
name: large-engine-work
description: Plan and run larger monolith engine or feature work — a development round, a refactor across modules, a QA pass, or anything using several agents or a workflow. Decisions and defaults, the developer's control over the look, interfaces first, worktree isolation, merge order, one GPU / one benchmark, independent verification, keeping docs current, the final summary. Use when the work spans several modules or agents, or when the developer hands over a task prompt.
---

# Large engine work

How the developer wants larger changes to monolith carried out: the engine, the editor, the runtime, R3D, tooling. Distilled from the task prompts of the two development rounds and the QA pass (the `project-history` skill). The hard rules in `CLAUDE.md` apply throughout; the day-to-day rules are the `engine-code` skill, the reference `docs/architecture.md`. Map edit requests follow the `map-request` skill instead. To write the task prompt for a new round: `/task-prompt`.

## 1. Before you start

- Load the `engine-code` skill and read `docs/architecture.md` in full for anything that touches rendering: its §4 (modules, frame reuse and `FrameKey`), `.claude/skills/lighting-fog-shadows/r3d-pitfalls.md` (with the `lighting-fog-shadows` skill) and the `engine-code` skill's verification are where earlier work lost the most time.
- Read the source instead of guessing: `src/main.c`'s header comment (controls and flags), `runtime/monolith_scene.h` (the runtime API), R3D's `src/r3d_draw.c` (pass order), `src/modules/r3d_light.c`, `shaders/`, `include/r3d/*.h`, `docs/`, and `submodules/r3d/patches.md` (every local patch, with symptom, cause and diff).
- `.claude/skills/benchmarking/measurements.md` holds the measurements and the ideas tried or rejected; the `project-history` skill the reasons behind the package format, the runtime API and the bundle format.

## 2. Decisions

- When something is the developer's call (a visual trade-off, a key binding, a package-layout detail, a large refactor), choose a sensible default, **say which one you chose and why**, and keep going. Ask only when you are actually blocked.
- **The look is the developer's.** Every change is either pixel-identical to the build before it (prove it: `--shot` pixel diffs with a count of changed pixels, plus `--dump-boxes`, on every map in `maps/`) or kept behind a switch and presented as a trade-off with before/after images and the measured gain. Never ship an image change silently.
- **Fix causes, not symptoms.** Don't turn off shadow casting to hide a shadow bug, don't brighten the fog to hide a dark map (`.claude/skills/lighting-fog-shadows/r3d-pitfalls.md`).
- **The level is data.** Nothing in the engine, the docs or the AI instructions may assume one style. Style lives in a map's package and its `MAP.md`; map-editing examples in the docs come from more than one style.
- **Work around R3D from the app side** where reasonable. When R3D itself must change, change it minimally and record it in `submodules/r3d/patches.md`.

## 3. Multi-agent work

When the developer asks for several agents (or a workflow), this is the method that worked in both rounds:

- **You are the lead.** You own the plan, the shared interfaces and the merge order, and you integrate and verify the result. Hand the work to agents with self-contained briefs.
- **Interfaces first, alone.** Before fanning out, write a short design note (as `docs/architecture.md` §5 is) and the headers it implies: the data records, the API, the file formats that several streams share. Agents work against them; implementation notes are appended as streams merge.
- **A brief names**: the files the agent may touch; the hard rules (repeat them, especially no visible windows, headless benches only, `gio trash` instead of `rm`, no commits); what "done" means; and what to report back (a diff summary, verification evidence such as box-dump diffs, pixel-diff counts and bench reports, open questions).
- **Isolation:** every code-writing agent gets its own git worktree (`isolation: "worktree"`). Streams that touch the same file (e.g. one R3D shader, or the map loader) run in sequence or go to one agent.
- **Build cost in worktrees:** `submodules/r3d/build-linux` is untracked, so a fresh worktree rebuilds assimp (minutes). Agents that don't change R3D build with `R3D_DIR=<main checkout>/submodules/r3d ./build.sh`; agents that do, build their own copy. `BENCHMARK_PATH`, `MAPS_PATH` and `PROJECT_PATH` are baked to the worktree's paths.
- **Merge one stream at a time.** After each merge: both targets build with zero warnings, the leak check passes, every map's box dump and verification shots are unchanged.
- **One GPU, one benchmark.** Serialise every `--bench` run through the lead or one bench-runner agent that takes A/B binary pairs and runs the interleaved pairs. `--shot` correctness runs may run in parallel.
- **Research agents stay read-only** and report with file:line (or URL) evidence. Don't let several agents re-read the same large files for the same question.
- **Independent verification:** after each merged stream, a verifier agent re-checks the claims (shots, dumps, benches) instead of trusting the report. For a QA pass: independent testers on copies (never editing the repo), then fixers with a regression test per finding, then the lead re-verifies (the `project-history` skill).
- **Internet access** only for the streams that need it (asset sourcing, import tests); their briefs repeat the download and licence rules (the `assets-and-licences` skill).
- **Keep the fan-out proportionate:** roughly 6–10 agents over a whole round. Don't spawn agents for trivial edits.

## 4. Performance work

The `benchmarking` skill (rules, interleaved pairs, the performance method).

## 5. Verification and reporting

- Verification is visual and numeric (the `engine-code` skill, "How to verify changes"): both builds without warnings (and `examples/build.sh` when the runtime or an example changed), `glslangValidator` on shaders, `tools/leakcheck.sh`, every map's identical box list and pixel-identical shots (its viewpoints, yaw sweeps, the extra views its `MAP.md` lists under *Engine verification*, a `--reload-test`), with `tests/models` when models, materials, lights or environment code changed.
- The launcher is tested with `MONOLITH_TERMINAL=tools/stub-terminal.sh` and `MONOLITH_REQUESTS_DIR=<scratch folder>` (so the developer's own coordinator never sees the test bundles), never with a real Claude session. The queue and the coordinator's scheduling are tested end to end by `tools/test-coordinator.sh`: the stub terminal runs `tools/stub-coordinator.sh` in place of `claude` (`MONOLITH_STUB_RUN`, `MONOLITH_CLAUDE`), which applies the same rules as the `coordinator` skill through `tools/request-queue.sh`; change both when the rules change. Check window placement with `i3-msg -t get_tree`, never by switching to the workspace. Never edit the developer's window-manager config (e.g. `~/.config/i3/config`): give them the exact line to add.
- Interactive input (clicks, drags, typing, the panel) can only be tested by hand: give the developer exact steps to try.
- **Keep the docs current** in the same change: `src/main.c`'s header comment (controls, flags), `docs/architecture.md` (layout, architecture), `docs/command-line.md` (flags), `docs/user-guide.md` (keys, settings), `.claude/skills/lighting-fog-shadows/r3d-pitfalls.md` (pitfalls with "if R3D changes X, update Y" warnings), `map-format.md`, the skills in `.claude/skills/` (workflows), `patches.md`, measurements in `.claude/skills/benchmarking/measurements.md`.
- **Final summary** for the developer: what was built, how to try each part (exact keys, flags and steps), before/after benchmark tables from full runs, every R3D patch added, and the open decisions, each with before/after screenshots where the look changes.
