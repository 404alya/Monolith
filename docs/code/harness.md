# `tests/examples/harness.h`: the examples' test build

Headless screenshots and benchmarks of the examples, with no change to their source. The harness is not part of the examples, which are plain integration code. `./build.sh examples test` (and `lsan`, for the leak check; [nob-examples.md](nob-examples.md)) compiles each example's main file with `-include harness.h`, which turns the unchanged program into a test run.

## Shots

```
<example> --shot <file> [the example's own arguments]
```

- It opens a hidden window that is never mapped (title `monolith-shot`, so a window manager rule can route it), renders `HARNESS_FRAMES` frames into a texture, saves the last one to `<file>` and lets the program end normally (`WindowShouldClose` turns true), so its cleanup runs (the leak check needs that).
- Without `--shot` (or `--bench`) it refuses to run (exit 1): a test binary never opens a visible window.
- Frame time is a fixed 1/60 s, so an image never depends on the machine's speed.
- The example's own window title, flags, FPS cap and cursor calls are ignored.

## How

The program's raylib / R3D / scene calls that decide where a frame goes are macros to the `Harness*` functions (`main` included, which becomes `HarnessExampleMain`), and every frame that would reach the window goes into the harness's texture instead: `R3D_Begin` / `R3D_BeginEx` / `R3D_BeginPro` / `MS_Draw` without a target get it. R3D itself always writes framebuffer 0 when it has no target, which a hidden window can't read back.

## Bench mode

The frame time of a program that draws a scene with `MS_Draw` (the exported `viewer.c`, `examples/01_minimal`), measured like `monolith --bench --headless` ([painting.md](../painting.md) §14):

```
<example> --bench <report.md> [--bench-frames N] [--note "what changed"] [the example's arguments]
```

- **Window:** the same hidden window (title `monolith-bench`), always `HARNESS_BENCH_W` × `HARNESS_BENCH_H` (the window size of `monolith --bench --headless`, whatever size the program asks for; its `MS_ApplyRenderResolution` calls get that size too).
- **Camera:** from the first `MS_Draw` on, for each viewpoint of that scene, the camera passed to `MS_Draw` is replaced by monolith's spectator camera at the viewpoint (eye, yaw and pitch towards its target, fovy 70), spinning as `monolith --bench` spins it: `HARNESS_BENCH_WARMUP` frames still, then N timed frames (default 600, 10 .. 1 000 000) with the yaw turning 360 degrees.
- **Timing:** `glFinish` after every frame; a frame's time is from the end of the previous frame's `glFinish` to the end of its own. The program's own work per frame (input, its 2D text) is part of the frame, as monolith's loop is.
- **Report** (Markdown, at `<report.md>`; the same table goes to stdout): the Setup table of `monolith --bench` (GPU, driver, CPU, system, resolution, map, shadow, effects, vsync, window, method, command, the note), and per viewpoint and overall its statistics (average ms and FPS, 1 % low FPS, worst ms, shadow updates) plus the median, p95 and p99 frame time. Not in it: the per-pass GPU table (the harness leaves R3D's profiler off, so a run costs the same with an R3D built with or without it).
- **Running it:** like any benchmark: one at a time, `__GL_SYNC_TO_VBLANK=0`, a note. Compare its numbers only with the same harness's. `build/tools/bench-export-ab` runs A/B pairs of two exports ([bench-export-ab.md](bench-export-ab.md)).
