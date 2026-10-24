---
name: lighting-fog-shadows
description: Work on monolith's lighting, sun shadows, volumetric and distance fog, ambient light / IBL, tonemapping and exposure, surface shaders and the renderer's static caches (shadow tiles, fog cache, AO volume) — in the engine, in R3D, or in a map's env lines and shaders. Covers the R3D pitfalls to respect, how to debug black or too-dark surfaces, view-dependence, and how to verify with yaw sweeps and --off. Use for any change or bug about light, shadow, fog, sky, ambient, exposure, contrast, black patches, flicker or crawling pixels.
---

# Lighting, fog and shadows

**Read `docs/knowledge/r3d-pitfalls.md` in full first.** It holds the engine facts found by debugging (black-curtain fog, camera-fitted shadows and pancaking, shadow resolution and caches, other gotchas, shadowed undersides, view-independent effects). The rules below follow from it. Code changes also follow the `engine-code` skill; map `env` / shader edits the `map-editing` skill and the map's `MAP.md` (its *Lighting and atmosphere* section).

## Rules

- **The look is the developer's.** Change a map's look only when the request asks for it or the developer approves. Engine changes keep every map in `maps/` pixel-identical, or are offered as a trade-off behind a switch with before/after images.
- **Fix causes, not symptoms.** Don't disable shadow casting to hide a shadow bug, don't brighten the fog colour or the albedo to hide a dark map, don't add a shadowless fill light for dark undersides (it lights the volumetric fog too): see the pitfalls' §1, §2, §4, §5.
- **Any fade, fog or cache term is a function of world position (and the view ray), never of view depth** (pitfalls §6). Fix view dependence before a cache bakes it in.
- **Anything new that changes the image** (a uniform, a light, a setting) goes into `FrameKey` (`docs/knowledge/engine.md` §4, "Frame reuse"), or a still camera keeps showing the old image.
- **R3D changes are patches:** work around R3D from the app side where reasonable; otherwise change it minimally and record it in `submodules/r3d/patches.md` (symptom, cause, files, diff, verification). If R3D's shadow fit, fades or zExtension change, update the matching constants in `runtime/monolith_scene.c` (pitfalls §3).
- **Shaders:** check every changed shader with `glslangValidator`; avoid GLSL built-in names (`noise1`–`noise4`, `length`, `distance`, …): NVIDIA accepts what AMD / Intel reject (pitfalls §4).

## Debugging

- **When a result looks bizarre, check the harness before the renderer.** Example: env overrides passed as `env $F ./monolith` with `F="A=1 B=2"` under zsh set only `A` (to `"1 B=2"`), which produced a red ambient ground that looked like an R3D bug. Put overrides inline or in an array.
- **Isolate effects** with `--off <digits>` (1 volumetric fog, 2 distance fog, 3 SSAO, 4 SSIL, 5 bloom, 6 sun).
- **Black or too-dark surfaces:** usually a low final albedo meeting a `color.contrast` above 1 (pitfalls §4), not NaN or broken normals. Confirm by forcing `ALBEDO = vec3(0.5)` in a scratch copy of the package. Count pure-black pixels on shaded views. To find which stage of a surface shader darkens it, output `EMISSION = vec3(step(t, albedo.g))` with `ALBEDO = vec3(0)` in the scratch copy, moving the threshold `t` and the stage you output. Keep final albedos above the map's floor (its `MAP.md`).
- **Crawling specks or popping when the camera turns:** pixels near a clipping threshold (contrast) or a view-dependent term; compare a yaw sweep.

## Verification

- **Take a yaw sweep from one position** (yaw 0/60/…/300, at walking height and high up): the black-curtain fog and the shadow-popping bugs only showed when rotating.
- Before/after `--shot`s of the map's viewpoints, pixel-diffed (count of changed pixels), plus `--reload-test` when a cache is involved. Engine changes: the full list in the `engine-code` skill, "How to verify changes".
- Costs: shadows and the fog march are the expensive parts; any performance claim follows the `benchmarking` skill.
