# `src/bake.c`

`--bake-surface`: bakes a material's surface shader into a tile texture.

## Why

A procedural world-space material (a surface shader that computes its look from `POSITION`) costs its full noise work for every pixel it covers. Most of that work doesn't depend on where the surface is, only on its position within the surface plane, so it can be computed once into a tile that a cheaper shader fetches.

## How

The bake renders the material onto one square quad with an orthographic camera, through R3D's own G-buffer: `R3D_OUTPUT_ALBEDO` and `R3D_OUTPUT_ORM`, both 8-bit linear targets, shown 1:1 on a target of the same size. So the tile comes from the same GLSL as the live material and can be made again whenever the shader changes. Nothing is lit: the bytes are what the shader wrote.

## The quad and the tile

The quad covers [0, P) × [0, P) metres, where P = size / texels (the tile's period).

| Face | Plane | Normal | Tile u | Tile v |
|---|---|---|---|---|
| top | y = 0 | +y | world x | world z |
| side | z = 0 | +z | world x | world y |

Texel (i, j) of the PNG (row j from the top of the file, i.e. `texelFetch`'s `ivec2(i, j)` once the PNG is a texture) is the pixel whose centre is at u = (i + 0.5) / texels, v = (j + 0.5) / texels.

## Output

`<prefix>.png`, RGBA 8-bit: RGB = the shader's `ALBEDO`, A = its `ROUGHNESS`. A material that bakes data rather than a colour writes its channels there; the map loads such a tile with `data_sampler`, which skips the sRGB decode.

## Defines

The shader is compiled with these lines in front of its source (`MS_LoadOptions.shaderDefines`):

| Define | Value |
|---|---|
| `MS_BAKE` | 1 |
| `MS_BAKE_FACE` | 0 \| 1: top \| side |
| `MS_BAKE_LEVEL` | n, the level being baked (0 = the finest) |
| `MS_BAKE_PERIOD` | p, the tile's period in metres (size / texels) |
| `MS_BAKE_TEXELS` | t, level-0 texels per metre |

So the shader can force its texel grid to level n (instead of choosing one by distance), make its noise periodic over `MS_BAKE_PERIOD` (or the tile shows a seam) and leave out what it computes at run time.

Only the material's own shader file gets the defines: others may sit at R3D's 16 KB limit.

## Levels

With `--bake-levels n` (n > 1), level L is the tile baked with `MS_BAKE_LEVEL L` at size / 2^L px (texels / 2^L per metre, the same period). All levels share one atlas of (size + size / 2) × size px:

- level 0 at (0, 0);
- level L ≥ 1 at x = size, y = size − size / 2^(L−1): level 1 at the top right, each next one below the last.

Every level reloads the map with that level's defines. It reloads the whole package, so a shader that fails to compile fails the bake with R3D's log.
