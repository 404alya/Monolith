---
name: surface-shaders
description: Write or change a surface shader in a map package (maps/<name>/shaders/*.glsl, a material's `shader` key) — what R3D gives a surface shader, the rules that keep the depth pre-pass and merged box meshes working, portable GLSL (glslangValidator), and the per-pixel cost budget for procedural materials (value noise, fbm, hashes) with how to measure a shader's cost. Use for any new or edited .glsl surface shader, a procedural material, or a question about what a material costs.
---

# Surface shaders

A material's `shader "shaders/x.glsl"` is an R3D surface shader: `vertex()` and `fragment()` functions that R3D compiles into its geometry, pre-pass, shadow and forward programs. The format (built-ins such as `POSITION`, `NORMAL`, `ALBEDO`, `ROUGHNESS`, `MATRIX_INV_VIEW`, `#pragma usage`, varyings) is in `submodules/r3d/docs/shaders/surface_shader.md`; the material keys (`uniform`, `sampler`, `alpha_cutoff`, `instanced`) in `docs/map-format.md`. Shader edits are map edits: the `map-editing` skill's verification applies, and the game hot-reloads a changed shader.

## 1. Correctness

- **Keep the depth pre-pass:** `alpha_cutoff 0` on the material and no `discard` or `ALPHA` in an opaque shader. Then R3D lays down depth first and runs `fragment()` once per visible pixel (R3D patch 6). A material that can discard is shaded for every overdrawn fragment.
- **World-position shaders and merged boxes:** a material without `instanced on` is drawn as one merged mesh of visible faces per 32 m cell, so `POSITION` in `fragment()` is the world position, but the instance data isn't there. Reading `INSTANCE_*`, or `POSITION` in `vertex()`, needs `instanced on` (the loader warns and does it anyway). Details: `.claude/skills/lighting-fog-shadows/r3d-pitfalls.md` §4.
- **Uniforms are per frame, not per draw:** the last value set in a frame wins. Per-material values are separate materials with their own `uniform` keys (each is an alias of the one compiled program).
- **Portable GLSL:** NVIDIA accepts what AMD and Intel reject. Don't name anything after a built-in (`noise1`–`noise4`, `length`, `distance`, …), and check every changed shader with `python3 -I .claude/skills/shaders/check_surface_shader.py <file>`. It compiles the vertex, fragment and depth variants the way R3D builds them, with `glslangValidator` (the `shaders` skill, §2).
- **Dark output:** a final albedo below ~0.1 linear goes black in shade with `color.contrast` above 1 (`r3d-pitfalls.md` §4; the `lighting-fog-shadows` skill for finding which stage darkens a surface).

## 2. Cost: shade once, but cheaply

The frame is fill-bound, and the depth pre-pass doesn't make a shader cheap: it only stops overdraw. A surface shader still runs once for **every visible pixel of its material**, so a shader's cost is roughly *its work per pixel × the share of the screen it covers*. A map's main material (ground, walls, floors: often half the screen or more) has to be cheap. A shader on a few small props can afford much more.

- **Count the noise.** A 3-D value noise built on a hash costs 8 hashes plus the interpolation. A 4-octave fbm costs 4 noises (32 hashes). Count what runs **unconditionally** for every pixel. **Guide for a material that covers large surfaces: at most ~4 noises (one fbm) per pixel unconditionally.** More only behind a gate (below), or from a texture.
- **Gate rare features cheaply.** Veins, cracks, streaks and stains that show on a small share of pixels: compute a cheap test first (one low-frequency noise or a hash) and skip the expensive part when its result is 0 (`if (area > 0.0) { … }`). A gate only pays off when it is 0 for most pixels: an fbm whose result goes through a `smoothstep` still costs its full fbm everywhere.
- **Use textures for slow layers.** Low-frequency tone, mottling and banding that every pixel needs can come from a noise texture (one fetch instead of 8 hashes per noise) or from a baked tile (`sampler` key). This changes the exact pattern, so it is a look change: the developer decides from before/after shots.
- **Fade detail with distance,** where the material already has a distance level (a coarser texel grid farther away): drop or reduce the features that become sub-pixel there instead of computing them and fading them out. This too is a look change.
- **Map edits never need a benchmark** (`CLAUDE.md`), so keep shaders cheap by construction: stay inside the guide above, and say in the material's comment roughly what a pixel computes when it goes beyond it.

**Bake it.** A world-space procedural material whose pattern depends only on the position *in its face plane* (plus a little that doesn't, like a per-plane hash or a slow 3-D tone) can be baked into tiles with `monolith --bake-surface` (`src/bake.c`, `docs/command-line.md`) and fetched with `texelFetch` from a `data_sampler` (linear, no sRGB decode). One file does both: under `#ifdef MS_BAKE` the full procedural code writes data channels into `ALBEDO` / `ROUGHNESS`; without it, the shader fetches the tile and computes only what a tile can't hold (height terms, slow noises that must stay continuous across box corners, per-plane variety). What it takes, from the concrete (`maps/monolith/shaders/concrete.glsl`):
- **Periodic noise.** Every noise lattice gets a whole number of cells per tile period (frequency rounded) and wraps there; a rotated lattice wraps modulo the two lattice vectors one period maps to (pick the stretch so they are whole). Never cross-fade edges. Check: no seam in a 2 × 2 montage and the same neighbour step across the edge as inside.
- **A pixel grid with distance levels** needs every level baked (`--bake-levels 6`, one atlas): fetching the finest tile at a coarse texel breaks 1-texel lines into dots. Per-level fades (`cov`) can then be baked in exactly.
- **A representative tile.** The tile is the only sample the whole map shows, and slow layers have few cells per tile, so one slice can be far from the shader's average (the concrete's first slice had 1.9× the vein area). Measure the feature cover of the procedural shader over many planes and bake the slice that matches.
- **Variety:** give each face plane its own offset into the tile (from its plane hash, in whole coarsest-level texels so every level stays on its grid).
- **8-bit channels:** scale values to the range (e.g. albedo × 2), map "no effect" to an exact byte (a feature factor of 1 → 128), pack low-precision masks into bits. R3D's surface-shader source limit (16 KB) counts the bake path too.
- Re-bake after any change to the `MS_BAKE` path; write the commands into the map's `MAP.md`.

**History: the concrete shader (2026-10-27, machine A, RTX 4050 Laptop GPU).** A procedural stone shader evaluated ~23 value noises per pixel unconditionally: four 4-octave fbms plus single noises for tone, tint, grain, pits and feature areas. It covered most of the default map's screen. With the pre-pass on, it was still 66% of the geometry pass, and one of its merged meshes alone was 47%. Making its `fragment()` a flat colour cut the frame by 1.28 ms (−26%). On the integrated GPU of machine C the geometry pass was 42 ms of a 112 ms frame. Numbers and method: `.claude/skills/benchmarking/measurements.md`, "nsys: the geometry pass and the concrete shader".

## 3. Measuring a shader's cost (when the developer asks)

1. **Upper bound:** copy the package to a scratch folder and reduce the shader's `fragment()` in the copy to a flat `ALBEDO` / `ROUGHNESS`, keeping the same `#pragma usage` and `alpha_cutoff`. Bench the copy against an unchanged copy with `--map <copy>/<name>.map`: interleaved full-run pairs, the `geometry` row of the per-pass table and the frame time (the `benchmarking` skill). Never edit `maps/` to measure.
2. **Which draw:** the per-draw GPU timestamps of a scratch build, or KHR_debug ranges in an nsys capture (the `benchmarking` skill, "GPU per draw: nsys"). They show which material and which merged mesh carry the time.
3. A cheaper version is a look change unless shots prove it pixel-identical: present before/after shots of the map's viewpoints with the measured gain.
