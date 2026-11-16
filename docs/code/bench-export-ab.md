# `tools/bench-export-ab.c`: A/B benchmark of two exports

`tools/bench-export-ab.c` runs interleaved A/B benchmark pairs of two exported maps (`monolith --export`), each drawn by its own exported runtime: it measures the game's frame time, not the editor's. It replaced `tools/bench-export-ab.sh` (2026-11-13) with the same arguments, output, files and exit codes; its table is the script's embedded Python, ported line by line (`Summarize`). Overview of all the tools: [architecture.md §8](../architecture.md#8-tools).

## Usage

```
build/tools/bench-export-ab <base export> <new export> [--map <name>] [--pairs N] [--frames N]
                            [--note "what changed"] [--out <dir>] [--dry-run]
build/tools/bench-export-ab --summarize <out dir>
```

| Option | Meaning |
|---|---|
| `--map <name>` | The package folder in both exports (default: the one folder `<name>/` holding `<name>.mapb`). |
| `--pairs N` | The number of A/B pairs (default 3, at least 3). |
| `--frames N` | Timed frames per viewpoint (default 600, the full run; 150 to screen). |
| `--note "..."` | What the two exports differ in (required); goes into every report. |
| `--out <dir>` | The output folder (created). Default: `benchmark/export-ab/<stamp>` in the repo. |
| `--dry-run` | Builds both binaries and prints the commands (shell-quoted, as bash's `printf %q`) without running them. |
| `--summarize <d>` | Prints the table again from the reports in `<d>` (nothing is built or run). |

## What it does

`<base export>` and `<new export>` are folders written by `monolith --export` (or F9).

1. **Build.** It compiles each export's `viewer.c` with the test harness's bench mode (`tests/examples/harness.h`, force-included into `viewer.c` only) against that folder's own runtime (`monolith_scene.c/.h`) and R3D libraries (`prebuilt/linux/lib`, or `build/r3d-linux/lib` when the export's `./build.sh` built them from source), into `<out>/A/viewer-bench` and `<out>/B/viewer-bench`.
2. **Run.** It runs A, B, A, B, ... (N pairs, default 3, at least 3) on the map package in each folder. Every run is a hidden-window bench (`--bench <report>`, 1920 x 1006, `glFinish` per frame, the camera spinning at every viewpoint as in `monolith --bench`) with `__GL_SYNC_TO_VBLANK=0` and `vblank_mode=0`.
3. **Report.** It prints, per pair, the overall average frame time of A and B and their difference, then the mean and median of the paired differences, overall and per viewpoint.

## Files written

| File | Content |
|---|---|
| `<out>/A-<pair>.md`, `<out>/B-<pair>.md` | The bench reports. |
| `<out>/<A\|B>-<pair>.log` | Each run's output. |
| `<out>/runs.txt` | The plan and every run's command. |
| `<out>/summary.txt` | The table. |

## Rules

A benchmark like any other (the `benchmarking` skill): one at a time on the machine (one GPU), never beside another benchmark or shot, and results relative to the base on the same machine. Linux only (the bench runs here; a Windows export is built with its `./build.sh --win` as before).

## Environment

| Variable | Use |
|---|---|
| `CC` | The compiler of the two bench binaries (default `cc`, as an export's `build.sh` uses). |

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Done (or planned, with `--dry-run`). |
| 1 | A build or run failed, an export or package wasn't found, or `--summarize` found no report pairs. |
| 2 | Usage (also a bad `--pairs` / `--frames` or a missing `--note`). |
| the compiler's | A failing compile exits with the compiler's status, as the script's `set -e` did. |

## Called by

The developer and the `benchmarking` skill (an export A/B). Built by `./build.sh --tools`.
