# monolith: performance notes and measurements

Measurement history and the reasoning behind the engine's caches and trade-off knobs. The rules that follow from it are in `docs/knowledge/r3d-pitfalls.md` and the `benchmarking` skill; this file holds the numbers, how they were measured and the ideas not taken. Numbers are from the dev machine (RTX 4050 Laptop, driver 615.71), default map, unless noted. GPU clock states swing results by ~10%: compare rows of one session only.

## R3D build type

Before 2026-10-10 R3D/raylib/assimp were built without a build type (no optimisation), like the original `~/Projects/r3d/build`. Measured with interleaved headless pairs (4 pairs, 2026-10-10), Release vs. unoptimised made **no measurable difference**: 4.952 vs. 4.969 ms average, CPU time per frame 0.500 vs. 0.517 ms, time inside `R3D_End()` 0.358 vs. 0.371 ms. The CPU side is dominated by GL driver calls, not R3D's own code, and the frame is GPU-bound (~4.4 of ~5.0 ms). Older reports (without an `R3D build` row) stay roughly comparable.

## Sun shadow

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

## Caching results (2026-10-10)

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

## Pass trimming and the window (2026-10-11)

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

## Trade-off knobs

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

## Quality presets (2026-10-11)

Headless `--bench`, 1920×1006, spinning, 3 interleaved rounds: ultra 3.17 ms, high 2.07, medium 1.32, low 0.77.

Single-knob costs behind the settings panel (default 3.59 ms in that session): render scale 0.75 −39% / 0.5 −55%; shadow map 8192 = 3.7 cm texels, 256 MB VRAM, 4096 = 7.4 cm, 64 MB; AO baked −27%, off ~−0.9 ms; fog cache 0.5 m + step ×2 −16%; bloom off −6%; window MSAA ~0.1 ms.

## Shadowed undersides (`docs/knowledge/r3d-pitfalls.md` §5)

Measured with `--off` on the default map (2026-10-10). Before the ambient-only sky, with the sun off (`--off 6`) soffits and the hall ceiling were exactly (0,0,0), and warm volumetric in-scatter on them came out maroon, e.g. (19,0,0), after AgX, contrast and 0.8 saturation. SSAO and SSIL each changed whole-frame mean brightness by < 0.5/255 in every test view. SSIL ≈ 0.5 ms and SSAO ≈ 0.2 ms GPU; net, SSIL darkens (its own 5 m AO term outweighs its bounce). With both off the largest change in the 26 verification shots is the darkening band along wall feet in close shaded views (view 1: up to +37/255 in that band, +1.35/255 mean); views 2/3/5 and the steep view < 0.5/255 mean.

After the fix: fully sunlit 0.25-albedo ground would be ~0.12 linear (albedo · sun 2.0 · cos 39° / π); 0.19 was picked by eye. Shadowed undersides at ~10–25% of sunlit faces in linear terms (hall ceiling (0,0,0) → (47,43,37), facade soffit (38,26,6) → (64,56,45), sunlit pillar unchanged at (149,141,130)).

## Camera turning (R3D patch 4)

View depth is distance · cos of the angle off-axis, only 0.57 · distance at a 16:9 corner, so terms keyed on it changed when the camera only turned: the hall's far opening glowed in the screen centre and dimmed at the edge, distance fog thinned toward the edges. Measurements in `submodules/r3d/patches.md`.

## Box culling and hidden faces (2026-10-20, GTX 1660 Ti)

The default map had grown to 46,322 boxes (555,864 triangles, 85 materials). R3D never culls an instanced draw outside a cluster, so all of it was vertex-processed every frame, twice with the depth pre-pass. Headless `--bench`, 1920×1006, spinning, 600 frames, the map snapshot of that day: the geometry pass was 70% of GPU time at `low` (1.66 of 2.38 ms), and `--off 12345` on top of `low` gained nothing.

- **Clusters** (one per 32 m cell and material, models 128 m; 307 box draws): low 4.41 → 3.44 ms, ultra 10.51 → 9.56 ms (3 interleaved pairs). Models at 32 m cells made pine_forest 25% slower (513 draws for 719 instances); at 128 m (93 draws) it is within noise.
- **Hidden faces** (opaque boxes as one mesh per cluster without the faces covered by other opaque boxes: 70,754 faces, 414,356 triangles drawn): vs. clusters alone low 2.73 → 2.56 ms, ultra 7.70 → 7.53 ms (3 interleaved triples). Requiring 0.25 m of cover (`HIDE_MIN_COVER`): 2.69 / 7.59 ms. Load +~110 ms, ~32 MB of vertex/index data.
- **Both vs. before** (same session, 3 pairs): low 3.28 → 2.61 ms (305 → 383 fps), ultra 8.39 → 7.62 ms; pine_forest low 1.09 → 1.11 ms.

Session-to-session swings were large (the clusters-alone low run measured 3.44 in one session and 2.73 in the next): compare only rows of one session. Not done: merging coplanar faces into larger quads (UV and T-junction risk), a larger near plane.

## CPU: collider BVH (2026-10-20, i5-10400F + GTX 1660 Ti)

Profiled with `perf record --call-graph lbr` (LBR stacks unwind through the NVIDIA driver, which has no CFI) and `__GL_YIELD=USLEEP` (so `glFinish`'s spin-wait in headless benches doesn't pose as work). On the default map the crosshair pick (`DesignerPickCamera` → linear `PickBoxes` over 46,322 boxes) was ~21% of the CPU samples, ~0.57 ms per frame in spectator mode; walking (`player.c` `Overlap`, 3–5 linear scans per frame) and `MS_Raycast` / `MS_Collides` had the same shape.

A BVH over the published colliders (built at load, leaves of ≤ 4, also for noGpu loads) now serves `MS_Raycast`, `MS_Collides` and the new `MS_FirstCollision`; the designer's pick and the player go through them. Results are bit-identical to the linear scans (in-process self-test, 700k random/axis-aligned/corner rays and touching/apart boxes over the three test maps: 0 mismatches). A ray: 383 → 2.3 µs. Headless bench, 3 interleaved pairs: low 2.71 → 2.20 ms (CPU until finish 1.07 → 0.57 ms), ultra 8.52 → 7.87 ms. After it, `R3D_End` (draw submission, ~0.34 ms of it the geometry pass) is the largest CPU item.

## Run-to-run noise: GPU clock states (2026-10)

Headless `--bench` runs on the RTX 4050 laptop flip between two speed levels about 10% apart (e.g. Canyon 4.8 vs. 5.4 ms) depending on the GPU's clock state, far more than the 1-2% noise measured earlier between runs in one state. In 2026-10 one 3x full A/B made Canyon look 8% slower; 6 tightly interleaved short pairs showed it wasn't. Hence the rule in the `benchmarking` skill: compare paired differences of interleaved A/B runs, never single runs.
