# pine_forest: design brief

Read this file first on every request for this map. It decides how new content looks and
how it is built.

## Style and mood

A foggy pine forest at dusk, in a **stylised low-poly** look: faceted trees, rocks and props
with flat, muted colours, no fine detail. The sun has just dropped below the canopy: warm
light catches the tree tops and slants between the trunks as shafts in the haze, while the
forest floor is already in cool blue-violet shadow. A small camp in a clearing (tent, logs,
a campfire) and a few lanterns along the path are the only warm, man-made light. Quiet,
a little lonely, safe near the fire.

## Scale and walkability

- Units are metres, +Y is up. Ground slab 600 x 600 m, top at y = 0; trees reach about
  ±115 m, fog hides everything beyond ~80 m.
- Player: radius 0.3 m, height 1.75 m, eye 1.62 m; steps <= 0.45 m; jumps ~0.9 m;
  headroom >= 2.2 m; openings >= 0.6 m (better >= 1 m). Spawn: feet (0, 0, 48), facing -Z
  down the path toward the clearing.
- Reference sizes: pines 11-25 m tall (crowns 4-7 m wide, trunks ~0.6 m), young pines 3-5 m;
  tent 2.7 m wide; camp logs 2.5 m; lanterns 0.4 m on 1.2 m posts; the ridge terraces are
  2, 4.4 and 6.8 m high with 0.4 m steps.
- Tree trunks collide (a model-space box around the trunk); crowns don't. Lanterns don't
  collide. Keep the path (5 m either side of its centre line) and the clearing (radius 13 m
  around the fire) free of trees.

## Materials and palette

| Material | Use | Colour (sRGB) |
|---|---|---|
| `forest floor` | the ground slab only: ambientCG Ground037 (albedo + normal), 2.1 m tile via `uv_scale 286 286` on the 600 m box, tinted 150 150 140 | photo texture |
| `needles`, `needles_dark`, `needles_blue` | pine crowns (three tones, mixed at random) | 62 94 72 / 48 76 62 / 54 84 86 |
| `bark` | trunks, stumps, log bark | 80 64 58 |
| `wood` | cut wood, posts, tent poles, log ends | 120 84 56 |
| `rock`, `moss` | box terraces and low-poly rocks; moss caps | 84 82 86 / 58 74 50 |
| `rock scan` | the Poly Haven rock faces: palette colour + the scan's own normal map (`inherit normal`) | 98 94 94 |
| `canvas`, `camp stone`, `iron` | tent cloth, fire ring, the cooking frame | 150 96 62 / 86 86 92 / 40 40 44 |
| `embers`, `lantern` | emissive fire bed (255 110 40, energy 5); the lantern model's own flat atlas (`inherit albedo`, not metallic) | |

- Everything except the ground is a flat colour: no new photo textures on models or boxes.
  Albedos stay muted and mid-dark (sRGB 40-150); saturated colours only for emissive light
  sources. Metalness 0 except real metal (`iron`).
- Box textures stretch with the box (UVs are per face, 0-1), so a textured material only
  works on boxes of one size; prefer flat materials for boxes.

## Lighting and atmosphere

- Sun low and warm (`sun.direction 0.7 -0.32 0.64`: from the north-west, ~19 degrees up,
  colour 255 164 100, energy 2.2), shadow range 100 m. It sits ahead-left of the spawn, so the
  path view is half backlit.
- Sky: deep blue top (26 38 80), mauve horizon (152 118 140), a bright warm sun glow; the
  sky's ground colour matches the fog so the horizon line disappears.
- Ambient (IBL) from a brighter, bluer copy of the sky (`ambientSky.*`, ambient energy 1.5):
  shadows are blue-violet, never black.
- Distance fog exp2, density 0.022, colour 62 70 102. Volumetric fog: scattering 0.012,
  emission 0.25 in the fog colour, anisotropy 0.6 (shafts toward the sun), sun fogEnergy 1.6.
  If it turns orange-pink overall, lower `sun.fogEnergy` or the campfire's `fogEnergy`, not
  the sky.
- Point lights: the campfire (255 130 50, energy 5, range 18, shadowed) and the lanterns
  (255 180 110, energy 1-1.2, range 6-8, unshadowed). New light sources follow these.
- AgX tonemap, exposure 1.15, contrast 1.08, saturation 0.9.

## Rules for new content

- **Models are the default for organic things** (trees, rocks, props, logs). Reuse the models
  in `models/` first (7 Kenney pines, 2 rocks, a stump, a log, the Poly Pizza campfire, tent
  and lantern, the Poly Haven rock face). New ones: low-poly CC0 / CC-BY from the sources in
  `AI-MAP-TOOL.md` ("Models and textures"); Kenney and Poly Pizza low-poly fit best.
- **Boxes** for terrain shaping (terraces, ledges, steps, platforms), posts and anything
  architectural, with flat palette materials.
- **Restyle every model** with material overrides in the map file (`material` /
  `material.N`), never by editing the file: pines get `needles*` + `bark`, rocks `rock` +
  `moss`, wood `bark` / `wood`. Photoscanned models keep at most their normal map
  (`inherit normal`) with a palette albedo. Kenney kits declare `metallic 1`: always override.
- Stand models on their support with `origin bottom`; sink rocks 0.3-0.4 m into the ground.
- Budget: <= 10k triangles per model (the forest is 700 instances), textures <= 1K, the whole
  package <= 5 MB. Normalise heavier downloads with `tools/normalize_model.py`.
- Every downloaded file: a row in `ASSETS.md` and a `<file>.asset.md` sidecar
  (`tools/record_asset.py`).

## Groups and landmarks

| Group | What it is | Where |
|---|---|---|
| `ground` | Ground slab, 600 x 600 m, forest floor texture | x, z ±300 |
| `clearing: campfire` | Campfire model, ember bed, the shadowed fire light | (0, 0, 0) |
| `clearing: camp` | Tent, two seat logs, a stump with a lantern, a lantern by the tent | within 7 m of the fire |
| `path: lantern posts` | Three posts with lanterns and lights along the path | x ±2.6, z 20...40 |
| `ridge: terraces` | Rock outcrop: three box terraces (tops y 2, 4.4, 6.8) | x 22...52, z -42...-10 |
| `ridge: steps` | 0.4 m steps climbing +X to each terrace | x 17...35, z -28...-25 |
| `ridge: rock faces` | Five Poly Haven rock faces softening the terrace edges | around the ridge |
| `meadow: rocks` | Low-poly rocks and a stump in an opening to the south-west | around (-45, 0, 36) |
| `forest: pines near` / `far` | The forest: Kenney pines, random tones, spacing 5.2 m near, 6.5-8 m far | ±115 m, outside path / clearing / ridge / meadow |
| `forest: young pines` | 3-5 m young pines around the clearing | 13.5-30 m from the fire |

Viewpoints (`--view 1..5`): Forest path (= spawn), Campfire clearing, Ridge lookout, Meadow
edge, Above the canopy.
