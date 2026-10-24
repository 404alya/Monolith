---
name: benchmarking
description: Measure monolith's frame time with --bench --headless — screening runs vs. full runs, interleaved A/B pairs of two binaries, GPU clock-state noise, the per-pass GPU table, reporting numbers, and the performance method (measure first, one step at a time, caches keyed on scene->version). Use for any performance claim, optimisation, regression check or bench run.
---

# Benchmarking

## 1. Rules

- **Results belong to one machine.** Frame times depend on the GPU, CPU, driver, power mode (laptops: plugged in or on battery), compositor and resolution, so the project's earlier numbers (`measurements.md`, other docs) are not a target or a baseline for your run. Measure base and change **on the same machine in the same session** (interleaved pairs, below), and state results as **relative changes** (ms and % against that baseline) together with the machine: the GPU, driver and CPU lines of the bench report's *Setup* table. Never compare your absolute numbers with those of another machine or another day, and don't call something a regression because it is slower than a number in the docs.
- **Before you start, look at the machine:** `nvidia-smi` / `glxinfo -B` (or the first bench report's *Setup* table) for the GPU and driver, and whether it is a laptop. If the machine is new to `measurements.md`, add it to its *Machines* table when you record results.
- **Never take over the developer's screen.** Automated runs are always `--bench --headless` (hidden window, offscreen 1920 × 1006, `glFinish` per frame). Without `--headless`, `--bench` opens a visible window that must actually be shown: on an undisplayed workspace it stalls at 100% CPU forever. Never switch the developer's workspace to "watch" a run. Ask before any visible-window run.
- **Headless only against headless:** visible runs differ (on machine A headless was ~10% slower; the gap depends on the machine and its compositor).
- **Only one benchmark at a time** (one GPU). In multi-agent work, serialise every `--bench` run through the lead or one bench-runner agent (the `large-engine-work` skill).
- **Short runs to screen, full runs to report.** Screen ideas with `--bench-frames 150` (~10 s); report only numbers from full runs (600 frames, the default), and say which.
- **Benchmarks are noisy, and how noisy depends on the machine:** laptop GPUs can flip between clock states from one run to the next (~10% on machine A of `measurements.md`, much more than the 1–2% between runs in one state); desktops are usually steadier, but don't assume it: if you haven't measured this machine's noise yet, run the base binary 3× first and look at the spread. A single run, even a 3× full A/B, can show a regression that isn't there. **Verify every claimed gain or regression with interleaved A/B pairs**: two binaries side by side (base and change), same flags, A, B, A, B, … ≥ 3 pairs, and compare the paired differences. If one viewpoint looks slower, rerun just it as tightly interleaved pairs (a viewpoint-subset build or map).
- **Every run:** vsync off and a note, e.g. `__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless --note "what changed"` (`vblank_mode=0` for Mesa). The Bash tool may run zsh: an unquoted `$VAR` holding `A=1 B=2` does not word-split, so put env overrides inline or in an array.
- **Map edits never need a benchmark.**

```bash
__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless --bench-frames 150 --note "screen: what changed"   # screening
__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless --note "what changed"                            # full run
```

Reports go to `benchmark/<stamp>_shadow-<size>-<range>.md` (gitignored, so they stay on the machine that made them; completed runs only). Their *Setup* table names the GPU, driver, CPU, system and resolution: quote it with any result you report or record. Other flags (`--bench-still`, `--bench-window`, `--no-profile`, `--shadow`, `--map`): `docs/command-line.md`. Window titles of automated runs are `monolith-bench` / `monolith-shot`; keep them.

## 2. Performance method

- **Measure first.** `--bench --headless` has a per-pass GPU table (R3D patch 5); put the breakdown in front of any optimisation. The frame is fill-bound; vertex work is tiny.
- **One step at a time,** each measured on its own with interleaved A/B pairs (§1), kept only if it helps. Order the candidates by expected gain ÷ risk.
- **Static work is done once per map load.** Every cache is keyed on `scene->version` (and the settings it depends on) and rebuilt on hot reload; prove it with `--reload-test`. Keep the rebuild short (< 1–2 s on the default map; log it) and track VRAM (the 8192² shadow array is ~256 MB).
- **No view-dependent terms in anything cached** (`.claude/skills/lighting-fog-shadows/r3d-pitfalls.md` §6): fix view dependence before a cache bakes it in.
- **A still camera** reuses the last frame (`FrameKey`); `--bench` spins the camera so it measures real rendering. Anything new that changes the image goes into `FrameKey`.
- Ideas tried, rejected or not yet done, with numbers: `.claude/skills/benchmarking/measurements.md`. Record new measurements there, not in a skill, in a section that names its machine.

## 3. Reporting

The machine (GPU, driver, CPU, and laptop or desktop), before/after tables from full runs with the relative change, the number of interleaved pairs and their paired differences, the bench report paths, and the look: every change is either pixel-identical (prove it with `--shot` diffs) or presented as a trade-off with before/after images (`CLAUDE.md`).
