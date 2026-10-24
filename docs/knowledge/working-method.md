# Working method for engine and feature work

How the developer wants larger changes to monolith carried out: the engine, the editor, the runtime, R3D, tooling. Distilled from the task prompts of the two development rounds (`history.md`) and the QA pass (`docs/QA-REPORT.md`). The hard rules in `CLAUDE.md` apply throughout; the technical reference is `PROMPT-INSTRUCTIONS.md`. Map edit requests follow `AI-MAP-TOOL.md` instead.

## 1. Before you start

- Read `PROMPT-INSTRUCTIONS.md` in full for anything that touches rendering: §6 (modules, frame reuse and `FrameKey`), §7 (R3D pitfalls) and §8 (verification) are where earlier work lost the most time.
- Read the source instead of guessing: `src/main.c`'s header comment (controls and flags), `runtime/monolith_scene.h` (the runtime API), R3D's `src/r3d_draw.c` (pass order), `src/modules/r3d_light.c`, `shaders/`, `include/r3d/*.h`, `docs/`, and `submodules/r3d/patches.md` (every local patch, with symptom, cause and diff).
- `docs/performance.md` holds the measurements and the ideas tried or rejected; `docs/design-v2.md` the reasons behind the package format, the runtime API and the bundle format.

## 2. Decisions

- When something is the developer's call (a visual trade-off, a key binding, a package-layout detail, a large refactor), choose a sensible default, **say which one you chose and why**, and keep going. Ask only when you are actually blocked.
- **The look is the developer's.** Every change is either pixel-identical to the build before it (prove it: `--shot` pixel diffs with a count of changed pixels, plus `--dump-boxes`) or kept behind a switch and presented as a trade-off with before/after images and the measured gain. Never ship an image change silently.
- **Fix causes, not symptoms.** Don't turn off shadow casting to hide a shadow bug, don't brighten the fog to hide a dark map (`PROMPT-INSTRUCTIONS.md` §7).
- **The level is data.** Nothing in the engine, the docs or the AI instructions may assume one style. Style lives in a map's package and its `MAP.md`; map-editing examples in the docs come from more than one style.
- **Work around R3D from the app side** where reasonable. When R3D itself must change, change it minimally and record it in `submodules/r3d/patches.md`.

## 3. Multi-agent work

When the developer asks for several agents (or a workflow), this is the method that worked in both rounds:

- **You are the lead.** You own the plan, the shared interfaces and the merge order, and you integrate and verify the result. Hand the work to agents with self-contained briefs.
- **Interfaces first, alone.** Before fanning out, write a short design note (as `docs/design-v2.md` was) and the headers it implies: the data records, the API, the file formats that several streams share. Agents work against them; implementation notes are appended as streams merge.
- **A brief names**: the files the agent may touch; the hard rules (repeat them, especially no visible windows, headless benches only, `gio trash` instead of `rm`, no commits); what "done" means; and what to report back (a diff summary, verification evidence such as box-dump diffs, pixel-diff counts and bench reports, open questions).
- **Isolation:** every code-writing agent gets its own git worktree (`isolation: "worktree"`). Streams that touch the same file (e.g. one R3D shader, or the map loader) run in sequence or go to one agent.
- **Build cost in worktrees:** `submodules/r3d/build-linux` is untracked, so a fresh worktree rebuilds assimp (minutes). Agents that don't change R3D build with `R3D_DIR=~/Projects/monolith/submodules/r3d ./build.sh`; agents that do, build their own copy. `BENCHMARK_PATH`, `MAPS_PATH` and `PROJECT_PATH` are baked to the worktree's paths.
- **Merge one stream at a time.** After each merge: both targets build with zero warnings, the leak check passes, the default map's box dump and verification shots are unchanged.
- **One GPU, one benchmark.** Serialise every `--bench` run through the lead or one bench-runner agent that takes A/B binary pairs and runs the interleaved pairs. `--shot` correctness runs may run in parallel.
- **Research agents stay read-only** and report with file:line (or URL) evidence. Don't let several agents re-read the same large files for the same question.
- **Independent verification:** after each merged stream, a verifier agent re-checks the claims (shots, dumps, benches) instead of trusting the report. For a QA pass: independent testers on copies (never editing the repo), then fixers with a regression test per finding, then the lead re-verifies (`docs/QA-REPORT.md`).
- **Internet access** only for the streams that need it (asset sourcing, import tests); their briefs repeat the download and licence rules (`models-and-textures.md`).
- **Keep the fan-out proportionate:** roughly 6–10 agents over a whole round. Don't spawn agents for trivial edits.

## 4. Performance work

- **Measure first.** `--bench --headless` has a per-pass GPU table (R3D patch 5); put the breakdown in front of any optimisation. The frame is fill-bound; vertex work is tiny.
- **One step at a time,** each measured on its own with interleaved A/B pairs (`CLAUDE.md`), kept only if it helps. Order the candidates by expected gain ÷ risk.
- **Static work is done once per map load.** Every cache is keyed on `scene->version` (and the settings it depends on) and rebuilt on hot reload; prove it with `--reload-test`. Keep the rebuild short (< 1–2 s on the default map; log it) and track VRAM (the 8192² shadow array is ~256 MB).
- **No view-dependent terms in anything cached** (`PROMPT-INSTRUCTIONS.md` §7.6): fix view dependence before a cache bakes it in.
- **A still camera** reuses the last frame (`FrameKey`); `--bench` spins the camera so it measures real rendering. Anything new that changes the image goes into `FrameKey`.
- Ideas tried, rejected or not yet done, with numbers: `docs/performance.md`.

## 5. Verification and reporting

- Verification is visual and numeric (`PROMPT-INSTRUCTIONS.md` §8): both builds without warnings (and `examples/build.sh` when the runtime or an example changed), `glslangValidator` on shaders, `tools/leakcheck.sh`, the default map's identical box list and pixel-identical shots (7 viewpoints, yaw sweeps 0/60/…/300 at eye y 1.6, 60 and 100, the steep view, a `--reload-test`), the same for `maps/pine_forest` and `tests/models` when models, materials, lights or environment code changed.
- The launcher is tested with `MONOLITH_TERMINAL=tools/stub-terminal.sh` and `MONOLITH_REQUESTS_DIR=<scratch folder>` (so the developer's own coordinator never sees the test bundles), never with a real Claude session. The queue and the coordinator's scheduling are tested end to end by `tools/test-coordinator.sh`: the stub terminal runs `tools/stub-coordinator.sh` in place of `claude` (`MONOLITH_STUB_RUN`, `MONOLITH_CLAUDE`), which applies the same rules as `AI-COORDINATOR.md` through `tools/request-queue.sh`; change both when the rules change. Check window placement with `i3-msg -t get_tree`, never by switching to the workspace. Don't edit `~/.config/i3/config`: give the developer the exact line to add.
- Interactive input (clicks, drags, typing, the panel) can only be tested by hand: give the developer exact steps to try.
- **Keep the docs current** in the same change: `src/main.c`'s header comment (controls, flags), `PROMPT-INSTRUCTIONS.md` (§3 layout, §5 flags, §6 architecture, §7 pitfalls with "if R3D changes X, update Y" warnings), `docs/knowledge/` (map format, workflows), `patches.md`, measurements in `docs/performance.md`.
- **Final summary** for the developer: what was built, how to try each part (exact keys, flags and steps), before/after benchmark tables from full runs, every R3D patch added, and the open decisions, each with before/after screenshots where the look changes.

## 6. Writing a task prompt

A task prompt for a new round only needs what is new; everything above, the hard rules (`CLAUDE.md`) and the references (`PROMPT-INSTRUCTIONS.md`, `docs/knowledge/`) are already the agent's knowledge. A good task prompt has:

1. **Goal**: what the developer wants, as numbered outcomes, and why.
2. **Scope**: what may change (files, R3D, the default map's look: normally not) and what must not.
3. **Phases** with their own "verify" step, in dependency order; the interfaces the phases share.
4. **Agents** (if wanted): the suggested split and which streams must be sequential.
5. **Done**: the deliverables and the evidence that proves each (shots, dumps, benches, leak check).
6. **The developer's calls**: which choices to make with a default and report, and which to ask about.

When a round is finished, move what it taught into this folder or `PROMPT-INSTRUCTIONS.md` and its reasons into `history.md`; don't keep the task prompt as an instruction file.
