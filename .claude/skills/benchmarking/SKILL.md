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

- **Measure first.** `--bench --headless` has a per-pass GPU table (R3D patch 5); put the breakdown in front of any optimisation. The frame is fill-bound; vertex work is tiny. For the CPU side (draw submission, the editor UI, map loading and hot reload) profile first: "CPU profiling" below.
- **One step at a time,** each measured on its own with interleaved A/B pairs (§1), kept only if it helps. Order the candidates by expected gain ÷ risk.
- **Static work is done once per map load.** Every cache is keyed on `scene->version` (and the settings it depends on) and rebuilt on hot reload; prove it with `--reload-test`. Keep the rebuild short (< 1–2 s on the default map; log it) and track VRAM (the 8192² shadow array is ~256 MB).
- **No view-dependent terms in anything cached** (`.claude/skills/lighting-fog-shadows/r3d-pitfalls.md` §6): fix view dependence before a cache bakes it in.
- **A still camera** reuses the last frame (`FrameKey`); `--bench` spins the camera so it measures real rendering. Anything new that changes the image goes into `FrameKey`.
- Ideas tried, rejected or not yet done, with numbers: `.claude/skills/benchmarking/measurements.md`. Record new measurements there, not in a skill, in a section that names its machine.

### CPU profiling

`perf` on a headless run: a benchmark like any other (one at a time, never a visible window). It needs no sudo with `kernel.perf_event_paranoid` ≤ 2 (user space of your own processes; never lower it yourself, ask the developer). Kernel and driver-kernel time stay invisible: a sample whose IP is a kernel address is skid at a syscall boundary, not kernel work.

```bash
./build.sh profile                     # build/linux-profile/: -O2 -g + frame pointers; R3D/raylib/assimp RelWithDebInfo
tools/profile.sh spin -- --bench --headless --note "profile: spinning"                        # frames
tools/profile.sh still -- --bench --headless --bench-still --frame-reuse --note "profile: still"
tools/profile.sh hud -- --bench --headless --bench-window 4 --note "profile: HUD + overlay"
tools/profile.sh load -- --shot /tmp/x.png --view 1 --reload-test maps/<name>             # load + hot reload
tools/profile.sh --report benchmark/profile/<dir>          # the reports again (same binary)
```

Output in `benchmark/profile/<stamp>_<name>/` (gitignored): `report-frame.txt` (parts, time by DSO, the driver time by the function and the file:line that called into it, top symbols by self and inclusive time, the call trees under `R3D_End` and `Run`), `report-load.txt` (init, load and reload split into parse, models, BVH, hidden faces, box meshes, shaders, textures, caches), `frame.folded` (flame-graph input), `perf-stats.txt`. The report script is our own (`tools/profile_report.py`); don't download FlameGraph or similar scripts (code from downloads never runs).

- **Call graphs: `--call-graph dwarf`.** The NVIDIA driver has no frame pointers, so `fp` stacks break inside it, but it has `.eh_frame` (driver 615.71: ~47k FDEs), so DWARF unwinding goes through it back to `main` (~91% of samples reach `_start`, almost all the rest stop in `Run`, whose locals fill the 16 KB stack copy). `--call-graph lbr` (the machine-B recipe) is Intel-only.
- **Event: `cycles:u` with a fixed period (`-c`), not `-F` or `task-clock`.** `task-clock` adds ~0.25 ms of CPU per frame (a perf hook on every context switch of the driver's wait loop); `-F` frequency mode gives samples that aren't proportional to CPU time here. Weight by period, i.e. cycles.
- **Stay under the kernel's sample-rate limit.** The kernel lowers `kernel.perf_event_max_sample_rate` by itself after slow sampling interrupts (dwarf stacks are slow: it read 2000/s on machine A). Above ~half of it perf is throttled, drops samples in bursts and biases every share. `profile.sh` picks the period from the CPU's top clock (40% of the limit) and writes the throttle count to `perf-stats.txt`; a run with throttle events doesn't count.
- **`__GL_YIELD=USLEEP`** (profile.sh sets it), so `glFinish`'s wait shows as the part "wait" (sleep calls), not as driver work. Its polling (the driver reading the clock through the vdso) is the part "glFinish", found by the return address of the headless bench's `glFinish()` in `Run`. Both are outside the bench's *CPU until finish*.
- **Cross-check every frame profile** against its own bench report: profile.sh scales the part "frame" to the report's *CPU until finish*, so `R3D_End`'s inclusive share must come out at *CPU in R3D_End* ÷ *CPU until finish* (within a few points; machine A: 84–85% vs 84.5% on the default map). Two runs per configuration must agree on the top 10.
- **The driver has no symbols:** its time is named by our caller (the report's "driver time by call site" table, resolved with `addr2line`: e.g. `r3d_draw.c:1709`, the instanced draw call). raylib call sites can resolve to a wrong function name (identical code folding); the line is right.
- **Wall time:** the profile measures user cycles only. Load phases in ms come from the runtime's own log: every load and reload prints `monolith_scene: load|reload <total> ms: parse, models, collider BVH, surface shaders (n compiled), textures (n), hidden faces, box meshes, rest`, and the first two frames after it `first frames after the load a + b ms` (the frames compile the shader variants they draw with; pine_forest's reload compiles in the second). Report the median of 3 `--shot <file> --view 1 --reload-test <same map>` runs per binary, interleaved with the other binary, with `__GL_SYNC_TO_VBLANK=0`, with and without `__GL_SHADER_DISK_CACHE=0` (NVIDIA's shader disk cache hides most compile time; a driver update empties it). Finer splits: a scratch build (`GetTime()` around the calls, `getrusage` for user vs. sys) that is never committed.

## 3. Reporting

The machine (GPU, driver, CPU, and laptop or desktop), before/after tables from full runs with the relative change, the number of interleaved pairs and their paired differences, the bench report paths, and the look: every change is either pixel-identical (prove it with `--shot` diffs) or presented as a trade-off with before/after images (`CLAUDE.md`).
