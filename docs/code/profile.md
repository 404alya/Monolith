# `tools/profile.c`: CPU profile of a monolith run

`tools/profile.c` takes a CPU profile of one headless monolith run with `perf`: no sudo, user space only. The method is in the `benchmarking` skill, "CPU profiling".

## Usage

| Command | What it does |
|---|---|
| `profile [--period N] [--spin] [--keep-data] [--out DIR] [--bin PATH] <name> -- <monolith args...>` | Profiles one monolith run. |
| `profile [--bin PATH] --report DIR` | Writes `DIR`'s `report.txt` again from its `perf.txt`. |
| `profile --report-only <perf.txt> [--bench REPORT.md] [--finish-site SYM+0xOFF] [--all] [--top N]` | Prints a report of any `perf.txt` (no binary needed). |

Examples:

```
build/linux-profile/profile spin -- --bench --headless --note "profile: spinning"
build/linux-profile/profile still -- --bench --headless --bench-still --frame-reuse --note "profile: still"
build/linux-profile/profile load -- --shot /tmp/x.png --view 1 --reload-test maps/<name>
```

## Building

Built by `./build.sh profile` into `build/linux-profile/profile`, next to the binary it profiles: `build/linux-profile/monolith` (`-O2 -g` and frame pointers; R3D, raylib and assimp in RelWithDebInfo). Linux only (as `perf` is); it links only libc.

## A profile run, step by step

`main()` at the bottom of the file reads the same way.

1. **Refuse runs that would open a window:** only `--bench --headless` and `--shot` runs.
2. **Pick the sampling period:** a sample every N user-mode cycles (`cycles:u`), N chosen so that at the CPU's top clock the rate is 40% of the kernel's `perf_event_max_sample_rate`. The kernel lowers that limit by itself after slow sampling interrupts (dwarf stacks are slow: e.g. to 2000/s); above about half of it, `perf` is throttled, drops samples in bursts and biases every share (measured on machine A: 80% of it throttles, 45% doesn't). `--period` overrides.
3. **Record:** `perf record -e cycles:u -c <period> --call-graph dwarf,16384` of monolith, run in the repo root (it resolves relative paths such as `--shot` files against the repo, like a plain run) with `__GL_SYNC_TO_VBLANK=0` and `__GL_YIELD=USLEEP` (`--spin`: the driver's default spin-wait in `glFinish`). monolith's output goes to `run.log`. `perf.data` (~16 KB per sample) goes into a temporary folder under `/tmp`, deleted at the end (`--keep-data` moves it to `--out`).
4. **Convert:** `perf script` writes `perf.txt`, the text the report reads.
5. **`perf-stats.txt`:** the period and `perf`'s sample, throttle and lost counts; a warning when `perf` was throttled (such a run doesn't count: rerun with a larger `--period`).
6. **Find the headless bench's `glFinish()` call** in `Run()` (`nm` + `objdump`), so the report can leave out the driver polling there (waiting for the GPU isn't CPU work).
7. **Find the bench report** from the `report: ` line in `run.log` (bench runs only).
8. **Write `report.txt`** (see [The report](#the-report)).

**Output folder:** `--out`, default `benchmark/profile/<stamp>_<name>/` (gitignored).

## How it runs commands

Every command is printed before it runs, and runs without a shell: `fork`, then `execvp` with an argv array. Its output goes to a file (`dup2`) or comes back through a pipe, and its exit status is checked (`waitpid`). `perf` counts as a benchmark: one run at a time on the GPU.

## The report

**CPU.** Every sample of `perf.txt` (weighted by its period, i.e. cycles) is charged to the first function of our own binary on its stack, so a function's share includes the driver and library work it calls directly (the driver has no symbols).

- Waiting for the GPU isn't CPU work and is left out: sleeps and yields, and the driver polling at the `glFinish()` call of step 6.
- One-time work (init, map loads and reloads, shader compiles, cache bakes, exit) is left out too, so the table is the per-frame work. `--all` keeps it. Bench runs leave it out; `--shot` runs keep it (there the load is the point).

**GPU.** The GPU work is per R3D pass, not per function: the bench report's "GPU passes" table (`GL_TIME_ELAPSED` queries), ranked by its All column.

`--report` needs the binary that recorded `DIR` (the `glFinish` site is an offset into it).
