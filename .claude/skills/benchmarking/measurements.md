# Measurements and performance history

Measurement history and the reasoning behind the engine's caches and trade-off knobs. The rules that follow from it are in `.claude/skills/lighting-fog-shadows/r3d-pitfalls.md` and the `benchmarking` skill; this file holds the numbers, how they were measured and the ideas not taken. Numbers are for the default map unless noted, on the machine each section names (below). Run-to-run noise differs per machine (machine A swings ~10% between GPU clock states): compare rows of one session only.

## Machines

**Every number below belongs to the machine it was measured on.** Absolute frame times don't carry over to another GPU, CPU, driver, power mode or desktop: use them only to know what was tried and how large an effect was *relative to its own baseline*. Before comparing anything with these numbers, measure your own baseline on your machine. Each section names its machine (bench reports from 2026-10-08 to 2026-10-13 in `benchmark/` confirm machine A).

| Machine | Hardware | Sections |
|---|---|---|
| **A** | Laptop: NVIDIA GeForce RTX 4050 Laptop GPU, driver 615.71, AMD Ryzen 7 7435HS (CPU recorded 2026-10-24, same GPU and driver), CachyOS (Linux), picom compositor | everything up to 2026-10-13, editor UI font, CPU profile (2026-10-25) |
| **B** | NVIDIA GeForce GTX 1660 Ti, Intel i5-10400F (desktop) | box culling, collider BVH (2026-10-20) |

When you add a section, start it with the machine: the bench report's *Setup* table (GPU, driver, CPU, system, resolution), or a new row in this table.

## R3D build type (machine A)

Before 2026-10-10 R3D/raylib/assimp were built without a build type (no optimisation), like the original `~/Projects/r3d/build`. Measured with interleaved headless pairs (4 pairs, 2026-10-10), Release vs. unoptimised made **no measurable difference**: 4.952 vs. 4.969 ms average, CPU time per frame 0.500 vs. 0.517 ms, time inside `R3D_End()` 0.358 vs. 0.371 ms. The CPU side is dominated by GL driver calls, not R3D's own code, and the frame is GPU-bound (~4.4 of ~5.0 ms). Older reports (without an `R3D build` row) stay roughly comparable.

## Sun shadow (machine A)

**Where the shadow time goes** (8192/100, `--bench` averages of 3 runs, measured 2026-10-08 with temporary switches; reports in `benchmark/2026-10-08_1157*`–`1206*`; predates patches 2–4, 7–9):

| Part | Cost | Measured as |
|---|---:|---|
| Rendering the shadow map (every frame) | ~1.2 ms | every-frame vs. map frozen after the first frame |
| Sun's volumetric-fog march, mostly its shadow lookups | ~2.6 ms | default vs. `sun.fogEnergy = 0` |
| Everything shadow-related | ~3.5 ms | default vs. sun pushed without a shadow map |
| Size penalty 8192 vs 4096 | 1.7 ms (fog on), 1.0 ms (fog off) | so ~0.7 ms is the fog's lookups missing the texture cache on the bigger map |

The fog is the big one: `vfog_radiance.frag` runs at half resolution, up to `length / stepSize` = 120 steps per pixel, and each step does the full 8-tap PCF (`L_SampleShadowDir`), i.e. up to ~460 M shadow taps per frame. The lighting pass does only 8 taps per pixel (~0.2 ms).

**Fog cost vs. camera height (fixed 2026-10-10, R3D patch 2).** Frame time used to grow with camera height (Canyon 5.3 ms vs. Tower/Overview ~8–9 ms). Only volumetric fog scaled with height; with fog off, high views were faster. Cause: the fog shadow is faded by the pixel's depth, so on sky pixels and anything beyond `sun.range` it is weighted by exactly 0, yet those pixels marched the full fog length doing 8 taps per step. `vfog_radiance.frag` now skips the taps there; bit-identical output. Headless `--bench` average 7.35 → 5.87 ms (Tower 8.84 → 4.99 ms). Steep downward views from height (`--pos -52.22 61.62 39.53 --look 140.2 -39.6`) stay the most expensive: every ray hits ground 80–100 m away and marches the full length with lookups that are used. **Patch 3**: a 512² min/max depth map of the shadow map (16×16-texel tiles, rebuilt only when the shadow map is re-rendered, ~0.35 ms) lets the fog skip the 8 PCF taps wherever they must agree (fully lit or fully shadowed): that view 6.8 → 5.7 ms still. Trade-off: views with almost no shadowed fog (eye y 100) get +0.1–0.16 ms while spinning. Cheaper still but image-changing: one tap per step (visible grain in the hall shafts) or a larger `stepSize`. Since patch 4 the fade and the skip are keyed on distance from the eye (`fragDist`), not view depth. Side effect kept from upstream: fog in front of sky, or in front of geometry more than `range` from the eye, is never shadowed.

**Static sun shadow tiles (default since 2026-10-10, patch 7).** Image vs. the camera-fitted map: 326 pixels > 2/255 over 26 shots (16-bit depth rounding, max 15/255). The gain is mostly CPU: the per-frame coverage check (~0.4 ms) goes away, plus re-renders while moving (~1.2 ms + 0.35 ms min/max each). With the default map's tile home all 7 viewpoints and the verification shots fall into two cells, so in the play area each tile is drawn once per map load; far out, a new tile every ~90 m of travel.

**Camera-fitted shadow-map reuse (`--shadow-camera`, the default before the tiles).** R3D stores the light matrix with the cached map, so a reused map is always sampled consistently; R3D's fit radius depends only on range/fov/aspect and the centre is snapped to texels, so every fit lays the same world-space texel grid. `SunShadowNeedsUpdate()` replicates R3D's fit on the CPU and samples the view frustum out to `range` (48 depth slices × 33×17); it keeps the map only while R3D's fades from the cached map are within 1/255 of a fresh fit's at every sample. Cost ~0.1–0.4 ms CPU (0.43 ms in the bench spin).
- Re-renders: walking forward ~1 per 65 m, sideways ~1 per 2.5–4.5 m, a still camera never; the bench spin (0.6°/frame) ~21% of frames before patch 4, ~1 in 4200 after.
- R3D's fit is generous (the 159 m sphere sits 50 m ahead of the camera, covering everything within ~109 m in any direction); the check is deliberately strict anyway.
- Verified: static yaw sweeps pixel-identical to the old build; after 90 frames of turning/walking/strafing/flying, reuse matches every-frame rendering (≤ 0.005% of pixels > 2/255: 16-bit depth noise), while a frozen map differs on up to 21% of pixels (180 m flight).

**Shadow size / range** (`--bench`, 1920×1006, vsync off):

| Shadow setting | Texel | Frame time | FPS |
|---|---|---|---|
| 4096 / 140 m (old) | ~10.7 cm | 6.00 ms | 167 |
| 4096 / 100 m, re-rendered every frame | ~7.4 cm | 5.85 ms* | 171 |
| 6144 / 100 m, re-rendered every frame | ~4.9 cm | 7.27 ms | 138 |
| 8192 / 100 m, re-rendered every frame (`--shadow-always`) | ~3.7 cm | 7.55 ms* | 132 |
| 8192 / 100 m, reused | ~3.7 cm | 6.62 ms* | 151 |

\* 2026-10-08 runs (`benchmark/2026-10-08_1224*`–`1226*`); the older rows were measured on a day the GPU ran ~5% slower. The cost tracks map size, not range. 6144/100 is the recommended fallback for weaker GPUs.

**Not applied: larger fog `stepSize`.** Screening (`--bench-frames 150`): 0.75 → 1.0 saves ~0.38 ms, → 1.25 ~0.69 ms. Stills look the same, but 2–5% of hall pixels change by up to ~25/255 (the march noise pattern), and thin shafts may alias differently in motion.

**R3D-side proposals** (library changes, not made):
0. *Done locally since:* patch 9 hoists the per-step constants of the directional march out of the loop and uses a closed form for rays with no shadow weighting (fog march 0.84 → 0.43 ms; 8 pixels > 2/255 over 26 shots); patch 8 optionally replaces the march's shadow lookups with a baked 3D sun-visibility texture (`--fog-cache`).
1. *Fog shadow lookups* (`shaders/deferred/vfog_radiance.frag`, `LIGHT_DIR` case): one hardware-PCF tap per step instead of the 8-tap Vogel disk; jitter + bilateral upsample already blur it. Up to ~8× fewer taps.
2. *Cascaded shadow maps* for directional lights (`light_dir_view_proj` / `light_dir_push` in `src/modules/r3d_light.c`, `L_SampleShadowDir` in `shaders/include/wrap/light.glsl`): 2–3 × 2048–4096 cascades give finer texels near the camera at a fraction of the fill and cache cost.
3. *Rotation-stable fit:* centre the fit on the camera position so turning on the spot never invalidates a cached map.

## Caching results (2026-10-10, machine A)

Default map, headless `--bench`, 1920×1006, interleaved, 3 runs per row unless noted (reports `benchmark/2026-10-10_1742*`–`1748*`). "Original" = before the map designer work (patches 1–3); "p3base" = Release R3D + profiler + patch 4, before caching.

| Build / flags | Camera | ms / frame | FPS | mean 1% low | GPU | CPU |
|---|---|---:|---:|---:|---:|---:|
| original | spinning | 4.60 | 217 | 144 | – | – |
| p3base | spinning | 4.54 | 220 | 175 | 3.72 | 0.77 |
| final default | spinning | 3.35 | 299 | 211 | 2.94 | 0.36 |
| final `--fog-cache 0.5 2` | spinning | 3.15 | 317 | 249 | 2.75 | 0.36 |
| final `--ao off --bloom off` (1 run) | spinning | 2.25 | 444 | 348 | 1.96 | 0.25 |
| p3base | still | 5.03 | 199 | 163 | 4.20 | 0.77 |
| final default | still | 3.70 | 271 | 216 | 3.29 | 0.36 |
| final, reused frame (`--frame-reuse`, 1 run) | still | 0.17 | ~5900 | – | – | – |

Where the gain comes from: depth pre-pass + streak-fbm skip (geometry pass 0.98 → 0.55 ms spinning, 1.30 → 0.66 ms still), patch 9 (fog march 0.77 → 0.42 ms spinning), static shadow tiles (CPU 0.77 → 0.36 ms).

## Pass trimming and the window (2026-10-11, machine A)

Headless `--bench`, 4 interleaved full pairs per row (notes "P final A/B (round 2)"); per-pass GPU ms averaged over all views.

| | main 4455608 spinning | now spinning | main still | now still |
|---|---:|---:|---:|---:|
| frame | 3.73 | 3.45 (−7.6%) | 4.45 | 4.09 |
| GPU total | 3.19 | 3.03 | 3.78 | 3.58 |
| CPU until finish | 0.45 | 0.33 | 0.54 | 0.38 |
| depth pyramid (+ normal/radiance downsample) | 0.164 | 0.063 | 0.183 | 0.063 |
| bloom | 0.190 | 0.145 | 0.210 | 0.146 |
| FXAA + final blit | 0.175 | 0.106 | 0.231 | 0.118 |

All of it is R3D patch 10 (pyramid only as deep as something reads it, bloom composite folded into tonemapping, FXAA writing the final target): bit-identical over 27 shots and the models test map. The honest still gain is ~−0.15 ms.

*The visible window* (`--bench-window`, 3 interleaved triples): plain headless 3.50 ms, single-sampled window 3.78, 4× MSAA window 3.94. The window's own cost is ~0.45 ms in serial terms: 4× MSAA ~0.15 ms GPU (now off by default, `--window-msaa`), HUD text + overlay + pick ~0.15 ms CPU (~1500 glyph quads), and the resolve. ~200 fps visible vs. ~300 headless is explained outside the game: the desktop runs **picom** (glx backend, `vsync = true`, `use-damage = false`, dual-kawase blur), so every frame is also composited on the same GPU. A fullscreen window is unredirected (no compositor cost, but then the driver's vsync caps at 144 Hz unless `__GL_SYNC_TO_VBLANK=0`). Not measured with visible windows.

## Trade-off knobs (machine A)

Headless frame ms vs. the default (3 interleaved full runs each, default 3.59 ms); images: before/after sheets of views 1/2/3/5 + steep.

| Knob | What it does | Frame | Visual cost (5 views) |
|---|---|---:|---|
| `--render-scale 0.75` | R3D at 75% of the output size, bicubic upscale | 2.19 (−39%) | softer edges and concrete grain; 3.8% of pixels > 2/255, mean ±0 |
| `--render-scale 0.5` | 50%, bicubic | 1.62 (−55%) | visibly soft, aliasing on thin slits; 6.3% |
| `--ao baked` (0.5, 0.6; patch 11) | SSAO + SSIL off, baked distance-field AO (32 MB, ~60 ms bake) | 2.62 (−27%) | crisper contact darkening at wall feet, also from off-screen geometry, stable when turning; faint SSIL bounce gone; 11.7%, mean −0.7/255 |
| `--ao off` | SSAO + SSIL off | ~2.6 (screening) | soft wall-foot darkening gone; 3.5%, mean +0.4 |
| `--fog-cache 0.5 2 --fx vfog.step=1.5` | baked fog sun visibility + double step | 3.02 (−16%) | shaft edges ~0.5 m softer; 0.2% |
| `--bloom off` | no bloom, exposure ×1.19 | 3.35 (−6%) | faint halo at bright openings gone; 6.4% (±1–3/255), mean −0.1 |

`--ao` modes (2026-10-10, spinning): `ssao` −0.62, `lite` −0.67, `off` −0.93 ms per frame. `--fog-cache 0.5 2` alone: 290×202×510 voxels, 28.5 MB, 17–55 ms bake, −0.2 ms per frame, 0.5% of pixels change by > 2/255 (max 20).

Ideas not done: the concrete shader costs ~0.3 ms of the 0.58 ms geometry pass (a distance LOD for its grain would change the image); SSIL's three half-res denoise passes and SSAO's one compute the same geometric weights (fusable, ~0.05 ms); a quarter-resolution fog march (R3D change) would save ~0.3 ms on top of the fog cache.

## Quality presets (2026-10-11, machine A)

Headless `--bench`, 1920×1006, spinning, 3 interleaved rounds: ultra 3.17 ms, high 2.07, medium 1.32, low 0.77.

Single-knob costs behind the settings panel (default 3.59 ms in that session): render scale 0.75 −39% / 0.5 −55%; shadow map 8192 = 3.7 cm texels, 256 MB VRAM, 4096 = 7.4 cm, 64 MB; AO baked −27%, off ~−0.9 ms; fog cache 0.5 m + step ×2 −16%; bloom off −6%; window MSAA ~0.1 ms.

## Shadowed undersides (machine A; `r3d-pitfalls.md` §5)

Measured with `--off` on the default map (2026-10-10). Before the ambient-only sky, with the sun off (`--off 6`) soffits and the hall ceiling were exactly (0,0,0), and warm volumetric in-scatter on them came out maroon, e.g. (19,0,0), after AgX, contrast and 0.8 saturation. SSAO and SSIL each changed whole-frame mean brightness by < 0.5/255 in every test view. SSIL ≈ 0.5 ms and SSAO ≈ 0.2 ms GPU; net, SSIL darkens (its own 5 m AO term outweighs its bounce). With both off the largest change in the 26 verification shots is the darkening band along wall feet in close shaded views (view 1: up to +37/255 in that band, +1.35/255 mean); views 2/3/5 and the steep view < 0.5/255 mean.

After the fix: fully sunlit 0.25-albedo ground would be ~0.12 linear (albedo · sun 2.0 · cos 39° / π); 0.19 was picked by eye. Shadowed undersides at ~10–25% of sunlit faces in linear terms (hall ceiling (0,0,0) → (47,43,37), facade soffit (38,26,6) → (64,56,45), sunlit pillar unchanged at (149,141,130)).

## Camera turning (R3D patch 4, machine A)

View depth is distance · cos of the angle off-axis, only 0.57 · distance at a 16:9 corner, so terms keyed on it changed when the camera only turned: the hall's far opening glowed in the screen centre and dimmed at the edge, distance fog thinned toward the edges. Measurements in `submodules/r3d/patches.md`.

## Box culling and hidden faces (2026-10-20, machine B)

The default map had grown to 46,322 boxes (555,864 triangles, 85 materials). R3D never culls an instanced draw outside a cluster, so all of it was vertex-processed every frame, twice with the depth pre-pass. Headless `--bench`, 1920×1006, spinning, 600 frames, the map snapshot of that day: the geometry pass was 70% of GPU time at `low` (1.66 of 2.38 ms), and `--off 12345` on top of `low` gained nothing.

- **Clusters** (one per 32 m cell and material, models 128 m; 307 box draws): low 4.41 → 3.44 ms, ultra 10.51 → 9.56 ms (3 interleaved pairs). Models at 32 m cells made pine_forest 25% slower (513 draws for 719 instances); at 128 m (93 draws) it is within noise.
- **Hidden faces** (opaque boxes as one mesh per cluster without the faces covered by other opaque boxes: 70,754 faces, 414,356 triangles drawn): vs. clusters alone low 2.73 → 2.56 ms, ultra 7.70 → 7.53 ms (3 interleaved triples). Requiring 0.25 m of cover (`HIDE_MIN_COVER`): 2.69 / 7.59 ms. Load +~110 ms, ~32 MB of vertex/index data.
- **Both vs. before** (same session, 3 pairs): low 3.28 → 2.61 ms (305 → 383 fps), ultra 8.39 → 7.62 ms; pine_forest low 1.09 → 1.11 ms.

Session-to-session swings were large (the clusters-alone low run measured 3.44 in one session and 2.73 in the next): compare only rows of one session. Not done: merging coplanar faces into larger quads (UV and T-junction risk), a larger near plane.

## CPU: collider BVH (2026-10-20, machine B)

Profiled with `perf record --call-graph lbr` (Intel only; on AMD + NVIDIA the recipe is the `benchmarking` skill's "CPU profiling", DWARF stacks) and `__GL_YIELD=USLEEP` (so `glFinish`'s spin-wait in headless benches doesn't pose as work). On the default map the crosshair pick (`DesignerPickCamera` → linear `PickBoxes` over 46,322 boxes) was ~21% of the CPU samples, ~0.57 ms per frame in spectator mode; walking (`player.c` `Overlap`, 3–5 linear scans per frame) and `MS_Raycast` / `MS_Collides` had the same shape.

A BVH over the published colliders (built at load, leaves of ≤ 4, also for noGpu loads) now serves `MS_Raycast`, `MS_Collides` and the new `MS_FirstCollision`; the designer's pick and the player go through them. Results are bit-identical to the linear scans (in-process self-test, 700k random/axis-aligned/corner rays and touching/apart boxes over the three test maps: 0 mismatches). A ray: 383 → 2.3 µs. Headless bench, 3 interleaved pairs: low 2.71 → 2.20 ms (CPU until finish 1.07 → 0.57 ms), ultra 8.52 → 7.87 ms. After it, `R3D_End` (draw submission, ~0.34 ms of it the geometry pass) is the largest CPU item.

## Run-to-run noise: GPU clock states (2026-10, machine A)

Headless `--bench` runs on machine A (a laptop GPU) flip between two speed levels about 10% apart (e.g. Canyon 4.8 vs. 5.4 ms) depending on the GPU's clock state, far more than the 1-2% noise measured earlier between runs in one state. In 2026-10 one 3x full A/B made Canyon look 8% slower; 6 tightly interleaved short pairs showed it wasn't. Hence the rule in the `benchmarking` skill: compare paired differences of interleaved A/B runs, never single runs.

## Editor UI font and overlay (2026-10-25, machine A)

Outfit embedded font + restyled overlay (`src/ui.c`) against the build before it (HEAD `2cedea8`), default map, `--bench-window 4` (the bench draws the spectator HUD and overlay like the window). Driver 615.71.09.
- **Whole frames are too noisy for this:** 11 interleaved full pairs of `--bench-still --frame-reuse --bench-window 4` swung 0.66–1.98 ms *within one binary* (clock states), paired differences −36% … +49%. Useless for a UI costing tens of microseconds.
- **Timing the UI block itself** (an instrumented scratch build: `glFinish` + `GetTime` before the HUD/overlay block and after `EndTextureMode`, averaged over the bench frames; never committed) is stable to ~1%: base 0.213–0.230 ms per frame.
  - First version, raylib's `DrawRectangleRounded` / `DrawRectangleRoundedLinesEx` for panels, pills and ~60 key caps: **0.36 ms (+0.14)**. Switches: plain rectangles 0.229, no text 0.25, no backdrops 0.338 → the rounded shapes were the cost (dozens of triangles each, draw-mode switches in the batch).
  - Rounded shapes as nine-slices from atlas discs / rings: 0.266 ms (+0.05); without the key-cap boxes 0.207 → the caps (~15 quads each) were the rest.
  - Key caps baked per line size into the atlas, drawn as 3-slices: **0.220 vs 0.216 ms reused (+0.004), 0.247 vs 0.235 ms spinning (+0.011)**, 4 interleaved pairs each; whole frames 0.611 → 0.613 ms and 5.31 → 5.33 ms (+0.3%).
- Final full runs (600 frames, uninstrumented), interleaved: still + reuse 4 pairs −0.5 / +3.5 / +6.3 / +0.8% (median +2.2%), spinning 3 pairs −2.5 / +1.2 / +5.8% — within the clock-state noise.

## CPU profile: frames and map loads (2026-10-25, machine A)

Measuring only; nothing below is implemented. Method: the `benchmarking` skill, "CPU profiling" (`./build.sh profile`, `tools/profile.sh`). Setup: NVIDIA GeForce RTX 4050 Laptop GPU, driver 615.71.09, AMD Ryzen 7 7435HS, Linux 7.2.5-1-cachyos, 1920 × 1006 headless, ultra; perf 7.2.11, `cycles:u` every 5,692,478 cycles (`perf_event_max_sample_rate` was 2000, lowered by the kernel itself), `--call-graph dwarf,16384`, `__GL_YIELD=USLEEP`, no throttle events in any run used here. Data in `benchmark/profile/2026-10-25_round3/` (frames, 2 runs per configuration of 2400 frames per view, still 6000) and `_round2/` (loads, 5 runs per map); `_round1/` was throttled and is not used.

**The profile build** is representative: against Release, 26 shots (both maps: every viewpoint, a 6-yaw sweep, a `--reload-test`) pixel-identical, `--dump-boxes` and `--dump-scene` identical, frame time −1.1 / −0.7 / −1.3% and CPU until finish −4.1 / −5.0 / +1.9% (3 interleaved short pairs). perf itself (3 interleaved short pairs, profile build alone vs. under `profile.sh`): CPU until finish +0.3 / +0.1 / +1.3%, frame time −0.2 / −0.3 / −0.1%.

**Cross-checks.** Each frame profile is scaled to its own report's *CPU until finish*; `R3D_End`'s share of the frame's samples then matches *CPU in R3D_End* ÷ *CPU until finish*: default map spinning 84.7 / 86.0% vs. 84.1 / 85.2%, HUD 64.8 / 62.9% vs. 62.1 / 63.6%, pine_forest spinning 78.6 / 78.6% vs. 77.2 / 78.2%, HUD 49.9 / 49.6% vs. 50.2 / 49.6%. The two runs of a configuration have the same top 6–7 self symbols (neighbours less than 0.3 points apart swap places), and ranks 8–15 are within 0.2–0.4 points of each other (HUD: the same top 10). Every driver sample has a monolith caller in its stack (100.0%: DWARF unwinds through the driver).

### Frames

Spinning, default map: *CPU until finish* 0.92 ms (0.99 / 0.84 in the two runs: CPU clock state), GPU 4.6 ms. Of the frame loop's samples 35–38% are work, 4% the driver polling the clock in `glFinish`, 57–59% the `glFinish` sleep. Self time by DSO: NVIDIA `libnvidia-glcore` 75%, monolith (with R3D, raylib) 17%, libc 5% (qsort, list growth, the driver's memcpy), libm 1.4%, GL dispatch / GLX / X11 ~2%. The frame's CPU time is GL calls, not R3D's own code.

| ms per frame (spinning) | default map | pine_forest |
|---|---:|---:|
| *CPU until finish* | 0.92 | 0.59 |
| `R3D_End` (draw submission) | 0.78 | 0.47 |
| · `pass_scene_geometry` → `raster_geometry` | 0.34 → 0.31 | 0.10 → 0.08 |
| · · driver in the instanced draw call (`r3d_draw.c:1709`) | 0.164 | 0.023 |
| · · `glUseProgram` (`:1611`), model matrix uniform (`:1619`) | 0.035, 0.019 | –, 0.003 |
| · `glBindFramebuffer` (`r3d_target.c:397`, every pass) | 0.134 | 0.130 |
| · `glClearBuffer*` (`r3d_target.c:381`) | 0.030 | 0.027 |
| · bloom, lights, vol. fog, SSIL, SSAO, forward | 0.060, 0.055, 0.030, 0.032, 0.028, 0.021 | 0.062, 0.024, 0.034, 0.032, 0.026, 0.006 |
| · draw-call sort (`r3d_render_sort_list` + `qsort_r`) | 0.037 | 0.013 |
| `MS_DrawGeometry`, `MS_PushLights` | 0.034, 0.019 | 0.023, 0.004 |
| `EndDrawing` (the hidden window's swap) | 0.035 | 0.031 |
| `ClearBackground` (`main.c:1673`) | ~0.011 | 0.020 |
| crosshair pick (`DesignerPickCamera` → `MS_Raycast`) | 0.005 | 0.028 |

- **Spectator HUD and overlay** (`--bench-window 4`): +0.36 ms (default map, 1.28 vs. 0.92) and +0.29 ms (pine_forest). The key-hint lines (`ui.c` `Hints`) 0.115 / 0.119 ms, `DesignerDrawHud` 0.077 / 0.089 of which `MS_LineText` 0.046 / 0.015 (it walks the map text from the start to the hovered line, every frame), raylib's `GetGlyphIndex` (a linear search over the atlas glyphs per character) 0.041 / 0.055, `rlVertex3f` (the text and quad vertices) 0.066 / 0.087, the window stand-in's resolve 0.034 / 0.030.
- **Still camera with frame reuse:** 0.18–0.22 ms per frame including `glFinish`; the work is ~180–210 thousand cycles (~0.05 ms): the hidden window's swap ~70k, the cached-frame blit 30k, `BeginTextureMode` 20k, `ClearBackground` 15k, the crosshair pick 6k (default map) and 50k (pine_forest: 24% of a reused frame, the models' triangle BVH; the camera hasn't moved).
- The frame is GPU-bound at ultra (4.6 ms GPU vs. 0.9 ms CPU): a CPU saving here shortens CPU time and power; it shows in frame time only partly in the headless bench (`glFinish` per frame) and where the GPU is faster (lower presets, another GPU).

### Map load and hot reload

Wall times from an instrumented scratch build (`GetTime`/`getrusage` around the steps; median of 3), shares from perf (5 runs merged, user cycles: ~4.2 GHz for the CPU-bound steps). Wall ≈ process CPU time for every step: the load never waits.

| ms | default map: load | reload | pine_forest: load | reload |
|---|---:|---:|---:|---:|
| `MS_Load` / `MS_Reload` | 418 | 383 | 84 | 63 |
| parse | 26 | 24 | 2.3 | 2.2 |
| surface shaders (88 loads of 17 files) | 186 | 186 | – | – |
| textures | 83 | 59 | 35 | 27 |
| hidden faces (`FindHiddenFaces`) | 56 | 55 | – | – |
| box meshes (rest of `UploadBoxes`) | 40 | 38 | 1 | 0.1 |
| model files (assimp) | – | – | 37 | 28 |
| collider BVH | 8.1 | 8.0 | 0.1 | 0.1 |
| watch + hash, entities, sky + ambient, free old map | 11.5 | 8.4 | 5.3 | 4.4 |
| first frame after it (a normal frame: ~4.5 ms; pine_forest's reload: the frame after that) | 111 | 96 | 18 | 19 |

- **A hot reload of the default map is ~480 ms** (383 + ~92 in the first frame). By perf's user cycles (1.445 Gc ≈ 344 ms per reload; the rest is kernel time): surface shaders 53%, hidden faces 16%, textures 13%, parse 7%, box meshes 7%, BVH 2.4%. Kernel time (`getrusage`): textures 36% of their time, `UploadBoxes` 19%, shaders 7%.
- **Surface shaders:** every material with a shader compiles its own copy, 88 compiles of 17 files per load, the same again on reload (R3D keeps no compiled programs). The first frame after a load then compiles the variants the frame needs (~60 ms: `load_shader` / `link_shader` under `R3D_End`) and pays the driver's first-draw work in the draw call (~30 ms). That is with NVIDIA's shader disk cache; **without it** (`__GL_SHADER_DISK_CACHE=0`, as after a driver update) the shaders take 10.0 s per load and reload and the first frame 7.9 / 7.2 s: a reload ~17 s (pine_forest: first frame 553 ms).
- **Hidden faces + box meshes** (93 ms): `FindHiddenFaces`' own loops 35%, sorting the grid entries (`qsort`) 15%, `FaceHidden` and its cover tests 16%, the mesh uploads (`R3D_LoadMesh` → driver) 18%.
- **Cold start** (to the end of the first frame: 744 ms default map, 263 ms pine_forest): 45 ms before `InitWindow` (the hint-reading peek load parses the whole map), `InitWindow` 130–210 ms (GLX context, driver), `R3D_Init` 0.6 ms, then the load and the first frame.
- **Optional caches:** the AO volume (`--ao-volume 0.5 0.6`) bakes in 141 ms at load and 125 ms at reload (perf: ~0.51 Gc per bake); the fog sun-visibility bake (`--fog-cache 0.5 2`) adds ~21 ms to the first frame's `R3D_End` (GPU work behind a `glFinish`). The sun's shadow tile is rendered in the first frame.
- **`--shot` PNGs:** `ExportImage` → `stbi_write_png` takes ~200 ms of CPU for 1600 × 900 (85% in stb's zlib); every verification shot and every image of a request bundle pays it.

### Candidates (as measured before the optimisation round; results in the next section)

Ordered by expected gain ÷ risk. Load items are what the developer waits on at every hot reload; frame items save CPU time, not frame time, on this machine at ultra.

| # | Candidate | Measured | Expected gain | Risk |
|---|---|---|---|---|
| 1 | One compiled surface shader per file, the other materials as `R3D_LoadSurfaceShaderAlias` (own uniforms, shared program and lazily compiled variants) | 186 ms per load and reload + ~60 ms in the first frame; 88 compiles of 17 files | ~−150 ms per load / reload and ~−45 ms first frame: reload ~480 → ~280 ms; without the disk cache ~17 s → ~3.5 s | low–medium: runtime only; aliases must be unloaded before their original; verify draw order (sort keys) stays pixel-identical |
| 2 | Line-start index for `MS_LineText` (built with the text) | 0.046 ms per HUD frame (default map; grows with the file, up to 4 MB) | −0.045 ms per HUD frame, also on reused frames | low |
| 3 | Glyph table for the editor's text (`src/ui.c`), instead of raylib's linear `GetGlyphIndex` | 0.041 / 0.055 ms per HUD frame | ~−0.04 ms per HUD frame | low (same quads, pixel-identical) |
| 4 | Faster PNG writing for shots and bundles (stb's compression level) | ~200 ms per 1600 × 900 image | ~−100 ms per image (not measured) | low (same pixels, other bytes) |
| 5 | Keep compiled shaders and uploaded textures across a reload when their file is unchanged | after 1: ~36 ms shaders + 59 ms textures per reload (pine_forest: 27 ms textures) | ~−90 ms per reload (pine_forest ~−27) | medium: ownership across the swap; keyed on file content like the watch list, so every changed file still reloads |
| 6 | Hidden faces in parallel over cells (8 cores), without the grid sort | 93 ms per load with the box meshes | ~−50…−70 ms per load | medium: the result must stay identical (box dumps, shots); threads in the runtime |
| 7 | Fewer framebuffer switches in R3D (one FBO per target set; NVIDIA validates on bind) | `glBindFramebuffer` 0.13 ms + clears 0.03 ms per frame (15–22% of frame CPU) | ~−0.05…−0.1 ms per frame | medium: R3D patch across many passes |
| 8 | Leaner geometry submission: pre-pass draws without material uniforms and textures, pre-pass sorted by program | `raster_geometry` 0.31 ms (draw call 0.16, program switches 0.035) | ~−0.05…−0.1 ms per frame | medium: R3D patch; the pre-pass and its stencil tie-break (patch 6) must hold |
| 9 | Cache the draw-call sort keys between frames | 0.037 ms per frame | ~−0.02 ms | low–medium (R3D) |
| 10 | Skip the crosshair pick while camera and `scene->version` are unchanged | pine_forest 0.012 ms per reused frame (24% of it), 0.028 ms spinning | tiny; power on still frames | low |
| 11 | No peek parse before `InitWindow` (read only the hints) | ~25 ms at cold start only | small | low–medium |

Not covered: kernel and driver-kernel time (`perf_event_paranoid` 2; a spinning headless bench spends 2.6% of its CPU time in the kernel, 0.24 of 9.2 s), and the windowed present path (the bench's window stand-in only).

## CPU optimisations: loads, editor frames, R3D submission (2026-10-25, machine A)

The candidates above, implemented and measured one by one. Setup as in the section above (RTX 4050 Laptop, driver 615.71.09, Ryzen 7 7435HS, Linux 7.2.5-1-cachyos, 1920 × 1006 headless, ultra). Base: the tree before the round plus the load-timing log (that log changes no image or dump). Every kept change: the 90-file shot set (every viewpoint of `maps/monolith`, `maps/pine_forest`, `tests/models`; yaw sweeps 0–300 at heights 1.6 / 30 / 60; `--reload-test` of each map and both ways between the maps; `--off 16`; the steep view; picks; box, scene and entity dumps) 0 differ against the base, `--shot-hud` shots (selection, regions, prompt box with non-Latin text, settings panel, edit mode) 0 differ, both builds and the examples without warnings, leak check OK. Loads: the runtime's load log, median of 5 `--shot --view 1 --reload-test <same map>` runs per binary interleaved (no disk cache: 3). Frames: `--bench --headless` full runs in interleaved sets, *CPU until finish* (CPU in the frame before its `glFinish`).

**Load log** (new, permanent): `monolith_scene: load|reload <ms>: parse, models, collider BVH, surface shaders (n compiled), textures (n), hidden faces, box meshes, rest` per load with the GPU, and `first frames after the load a + b ms` (the two frames after it, each from one `MS_PushLights` to the next: they compile the shader variants; pine_forest's reload compiles in the second).

| ms, median | default map before | after | pine_forest before | after |
|---|---:|---:|---:|---:|
| load | 418.5 | 222.5 | 85.6 | 87.6 |
| reload | 387.8 | 198.2 | 63.2 | 64.0 |
| · surface shaders (reload) | 191.4 (88 compiled) | 41.3 (17 compiled) | – | – |
| · hidden faces (reload) | 55.2 | 14.3 | – | – |
| first frames after the load | 115.2 + 2.1 | 42.0 + 0.8 | 17.4 + 4.1 | 19.1 + 1.6 |
| first frames after the reload | 100.9 + 4.4 | 25.9 + 9.0 | 0.9 + 20.3 | 0.8 + 20.3 |
| **reload + its first frames** | **493** | **233 (−53 %)** | 84 | 85 |
| no shader disk cache: reload | 10 314 | 2 793 | 64.0 | 64.1 |
| no shader disk cache: first frames after the reload | 7 365 | 1 882 | 18.9 | 18.9 |
| **no disk cache: reload + first frames** | **17.7 s** | **4.7 s (−74 %)** | | |

| ms per frame, interleaved full runs | before | after | change |
|---|---:|---:|---:|
| default map spinning, *CPU until finish* (4 pairs) | 0.699 | 0.561 | −0.138 (−20 %) |
| · *CPU in R3D_End* | 0.591 | 0.454 | −0.137 (−23 %) |
| · frame time (GPU-bound) | 5.303 | 5.165 | −0.138 (−2.6 %) |
| default map, HUD + overlay (`--bench-window 4`, 3 pairs, before the sort cache was dropped) | 1.026 | 0.734 | −0.293 (−29 %) |
| pine_forest spinning (3 + 3 pairs) | 0.440 | ~0.43–0.45 | within noise (±0.02) |
| pine_forest still with frame reuse and HUD, frame time (3 pairs) | 0.673 | 0.589 | −0.085 (−13 %) |

What each change brought, and the decision:
1. **One compiled surface shader per file, aliases per material** (runtime): kept. Reload shaders 191 → 41 ms, first frames −75 ms, without the disk cache −13 s per reload cycle. Unexpected frame gain: the aliases share one GL program per file, so R3D skips the program switches between materials of one shader and the driver's per-draw work drops: spinning *CPU until finish* −0.116 ms (−17 %, 3 pairs: −0.108 / −0.109 / −0.132) with all the runtime and editor changes but no R3D patch; in the profile the geometry pass 0.194 → 0.077 ms per frame (driver time in the draw path 0.148 → ~0.05, with patch 20). Draw order unchanged (aliases are created in material order; the sort key is the creation serial).
6. **Hidden faces on threads, a hash instead of the sorted grid** (runtime, pthreads, up to 8 threads in chunks of 512 boxes): kept. 55 → 13–14 ms per load; the hidden-face masks are bit-identical (FNV hash and count of the masks compared on all three maps; 79 108 faces on the default map).
2. **Line-start index for `MS_LineText`** (editor block): kept. HUD profile: `MS_LineText` 0.037 ms per frame → not visible; `DesignerDrawHud` 0.061 → 0.022 ms.
3. **Glyph table in `ui.c`** (raylib's `DrawTextEx` / `MeasureTextEx` layout with a table instead of `GetGlyphIndex`): kept. HUD profile: `GetGlyphIndex` gone (pine_forest still frame: 197 of 1 402 kcycles), the key-hint lines 0.100 → 0.060 ms, text drawing 0.078 → 0.057 ms.
4. **PNG writing:** kept as a new writer (`src/png.c`: zlib level 1 on Sub-filtered rows, assimp's zlib), not stb's level. stb's knobs (level 5–8, a forced filter) gave at best 200 → 140 ms for 1600 × 900; zlib level 1 + Sub: ExportImage 178 / 176 / 155 ms → 48 / 44 / 39 ms on three 1600 × 900 images (300 × 200 crop: 7.5 → 2.0 ms), files ~20 % smaller (1.99 → 1.58 MB); decoded pixels identical (shots and every bundle image). raylib's own `CompressData` (sdefl) was slower than stb (173 ms unfiltered, 1.3 s Sub).
10. **Crosshair pick skipped while camera and scene are unchanged** (`DesignerPickCamera`): kept. pine_forest still frame: `MS_RaycastModel` gone from the profile; `--pick` output identical.
9. **Draw-call sort cached between frames** (R3D): **dropped.** The profile showed the `qsort` gone but the key fill staying (−0.01 ms per frame); benches: +0.006 ms (pairs −0.022 / +0.010 / +0.030), not measurable. Below the ~0.02 ms bar for an R3D patch: a memo with its own invariant isn't worth it.
7. **Fewer framebuffer binds and clears** (R3D patch 19): kept, −0.029 ms *CPU until finish* (3 pairs). Only 2 of 22 binds per frame could go (fog passes on the scene FBO with depth) and 9 of 10 depth/stencil clears (nothing written); the rest are real pass switches.
8. **Leaner geometry submission** (R3D patch 20: cached matrix uniforms, normal matrix only when the transform changes, no depth/stencil state set only to be overwritten after the pre-pass): kept, −0.027 ms (3 pairs). Not done: sorting the pre-pass by program (it already runs in the shading pass's shader-grouped order: no switches to save) and drawing it without material uniforms and textures (a custom `vertex()` may read them; 0 calls saved for the built-in program). Patches 19 + 20 without the sort cache: −0.025 ms (4 pairs; the pairs scatter by ±0.03).
11. **No peek load before `InitWindow`:** not done. A hints-only read would duplicate the parser, and without the parse a broken map would open the window before it fails (today it exits with `file:line` before any window); skipping only the peek's collider BVH and entities needs a new `MS_LoadOptions` field (public API, the developer's call) for ~10–20 ms at cold start only.
5. **Keeping shaders and textures across a reload:** not done (needs the developer's approval of a content-keyed cache that survives a reload).

Profiles: `benchmark/profile/2026-10-25_round4-base/` (spin, still, hud, still-pine) and `_round4-final/` (spin without the sort cache; still, hud and still-pine with it). In this round's frame profiles 26–30 % of the frame's driver samples have no monolith caller ("stack ends in the driver"; the round above had ~0 %): compare call trees between the two runs here, not with that round.
