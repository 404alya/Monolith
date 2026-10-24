---
name: benchmarking
description: Measure monolith's frame time with --bench --headless — screening runs vs. full runs, interleaved A/B pairs of two binaries, GPU clock-state noise, the per-pass GPU table, reporting numbers, and the performance method (measure first, one step at a time, caches keyed on scene->version). Use for any performance claim, optimisation, regression check or bench run.
---

# Benchmarking

## 1. Rules

- **Never take over the developer's screen.** Automated runs are always `--bench --headless` (hidden window, offscreen 1920 × 1006, `glFinish` per frame). Without `--headless`, `--bench` opens a visible window that must actually be shown: on an undisplayed workspace it stalls at 100% CPU forever. Never switch the developer's workspace to "watch" a run. Ask before any visible-window run.
- **Headless only against headless:** absolute times are ~10% higher than visible runs.
- **Only one benchmark at a time** (one GPU). In multi-agent work, serialise every `--bench` run through the lead or one bench-runner agent (the `large-engine-work` skill).
- **Short runs to screen, full runs to report.** Screen ideas with `--bench-frames 150` (~10 s); report only numbers from full runs (600 frames, the default), and say which.
- **Benchmarks are noisy:** runs swing ~10% between GPU clock states (laptop GPUs flip between two speed levels from one run to the next), much more than the 1–2% between runs in one state. A single run, even a 3× full A/B, can show a regression that isn't there. **Verify every claimed gain or regression with interleaved A/B pairs**: two binaries side by side (base and change), same flags, A, B, A, B, … ≥ 3 pairs, and compare the paired differences. If one viewpoint looks slower, rerun just it as tightly interleaved pairs (a viewpoint-subset build or map).
- **Every run:** vsync off and a note, e.g. `__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless --note "what changed"` (`vblank_mode=0` for Mesa). The Bash tool may run zsh: an unquoted `$VAR` holding `A=1 B=2` does not word-split, so put env overrides inline or in an array.
- **Map edits never need a benchmark.**

```bash
__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless --bench-frames 150 --note "screen: what changed"   # screening
__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless --note "what changed"                            # full run
```

Reports go to `benchmark/<stamp>_shadow-<size>-<range>.md` (gitignored; completed runs only). Other flags (`--bench-still`, `--bench-window`, `--no-profile`, `--shadow`, `--map`): `docs/command-line.md`. Window titles of automated runs are `monolith-bench` / `monolith-shot`; keep them.

## 2. Performance method

- **Measure first.** `--bench --headless` has a per-pass GPU table (R3D patch 5); put the breakdown in front of any optimisation. The frame is fill-bound; vertex work is tiny.
- **One step at a time,** each measured on its own with interleaved A/B pairs (§1), kept only if it helps. Order the candidates by expected gain ÷ risk.
- **Static work is done once per map load.** Every cache is keyed on `scene->version` (and the settings it depends on) and rebuilt on hot reload; prove it with `--reload-test`. Keep the rebuild short (< 1–2 s on the default map; log it) and track VRAM (the 8192² shadow array is ~256 MB).
- **No view-dependent terms in anything cached** (`.claude/skills/lighting-fog-shadows/r3d-pitfalls.md` §6): fix view dependence before a cache bakes it in.
- **A still camera** reuses the last frame (`FrameKey`); `--bench` spins the camera so it measures real rendering. Anything new that changes the image goes into `FrameKey`.
- Ideas tried, rejected or not yet done, with numbers: `.claude/skills/benchmarking/measurements.md`. Record new measurements there, not in a skill.

## 3. Reporting

Before/after tables from full runs, the number of interleaved pairs and their paired differences, the bench report paths, and the look: every change is either pixel-identical (prove it with `--shot` diffs) or presented as a trade-off with before/after images (`CLAUDE.md`).
