# Map packages and the map format

Everything inside a map package: the folder layout, the design brief (`MAP.md`) and the `.map` file format. You can write and edit maps by hand with any text editor while the game runs: it reloads the map on every save. The parser's header comment in `runtime/monolith_scene.c` is authoritative where this page and the code differ. Examples come from the two maps the project was built with: `maps/pine_forest` (in this repository) and the default map `maps/monolith` (kept outside it). "Patch N" refers to a local R3D change ([glossary.md](glossary.md)). Exporting a map to your own program: [export.md](export.md).

## 1. Map packages and the design brief

### 1.1 Package layout

```
maps/<name>/
├── <name>.map     geometry, models, materials, lights, environment, spawn, viewpoints (format 2)
├── MAP.md         the design brief: the map's style and rules (Claude reads it first on every request)
├── shaders/       surface shaders the map's materials use (e.g. maps/monolith/shaders/concrete.glsl)
├── models/        model files (+ the .bin / textures a .gltf names), each with a <file>.asset.md sidecar
├── textures/      textures (PNG, JPG; HDR panoramas for the sky)
└── ASSETS.md      every external file: source URL, author, licence (+ URL), date, sha256, changes
```

- Every path in a map file is **relative to the package root** (the folder of the map file). Absolute paths and `..` are rejected, so a package can be copied anywhere (and exported, [export.md](export.md)).
- `--map <path>` takes a package folder (it loads `<folder>/<folder name>.map`, else its only `.map`) or a `.map` file. A single `.map` file outside a package (format 1 or 2) loads too; its folder is its root.
- Hot reload watches every file the package uses: the map, shaders, textures, panoramas, model files and the files a model file names.

### 1.2 `MAP.md`: the design brief

Free-form Markdown with these sections (the `--new-map` template has them): *Style and mood*, *Scale and walkability* (player, steps, headroom, typical dimensions, routes that must stay walkable), *Materials and palette* (each material and what it is for, colours, how dark or bright albedos may be), *Lighting and atmosphere* (sun, sky, fog, lights, and why), *Rules for new content* (boxes or models, how models are restyled, polygon and texture budgets), *Groups and landmarks*. Examples: `maps/monolith/MAP.md` (brutalist, boxes only, one procedural concrete shader), `maps/pine_forest/MAP.md` (stylised low-poly, models restyled to a flat palette).

The brief is what keeps a map consistent: every request on the map is carried out in its style, and Claude updates it when a request adds a landmark, a group or a material.

- **New maps.** `monolith --new-map <name>` creates `maps/<name>/` (an argument with a `/` creates the package in that folder instead, e.g. a scratch draft): a 200 × 200 m ground slab (group `ground`), the plain grey `"default"` material, the built-in environment written out as `env` lines (daylight), a `spawn`, two viewpoints, empty `shaders/ models/ textures/`, an empty `ASSETS.md` and a `MAP.md` **template** with placeholders. Fill in the brief yourself, or send a first request that describes the map ("a foggy pine forest at dusk", "a neon market street at night, wet asphalt, stalls on both sides"): Claude writes the brief from it and then builds to it.

### 1.3 Format versions

The first statement `monolith 2` makes a file format 2: everything in §2. Without it a file is **format 1** (only `group`, `box`, `block`, `stairs`, `waffle`, `repeat`, `at`; everything else takes the built-in defaults, and boxes use the plain `"default"` material). To give a format-1 file materials, models or `env` lines, add `monolith 2` as its first statement. A file with `monolith N` newer than the runtime supports is rejected with a clear message.

### 1.4 The compiled form

The `.map` text is the source and stays authoritative: the editor, hot reload, requests and manual edits all work on it. Programs that only play a map (an export, a game) load its compiled form, `<name>.mapb`, which `monolith --compile` and `--export` write from the text: the same scene, already parsed, read without parsing ([compiled-map.md](compiled-map.md)). It is build output: never edit it, never commit it; after changing the text, compile again (a compiled file whose `.map` has changed since loads with a warning in monolith, and `--export` refuses it).

## 2. Map format reference

Each statement takes one line, so a line number identifies an object. The parser is in `runtime/monolith_scene.c`, and the header comment there is authoritative.

```
# comment                      '#' starts a comment anywhere outside "quotes"
monolith 2                     format version, the first statement
group "name"                   every following box, model and light belongs to this group, until the next group
```

### 2.1 Box primitives

Numbers are plain decimals (`12`, `-4.5`, `0.75`). Corners can be given in any order.

| Primitive | Arguments | Makes |
|---|---|---|
| `box` | `x0 y0 z0 x1 y1 z1` | 1 box between two opposite corners |
| `block` | `cx y0 cz sx sy sz` | 1 box centred on (cx, cz), from y0 up to y0 + sy, size sx × sy × sz |
| `stairs` | `cx baseY startZ width steps rise run` | `steps` boxes (integer) climbing toward **−Z**. Step *i* (0-based) spans z from `startZ − i·run` to `startZ − (i+1)·run`, x `cx ± width/2`, y from `baseY` up to `baseY + (i+1)·rise`. The steps are solid down to baseY. |
| `waffle` | `x0 x1 y0 y1 z depth bayW floorH` | A facade facing **+Z**: a back wall (x0..x1, y0..y1, z−1..z), 0.6 m vertical fins centred at x = x0, x0+bayW, … while ≤ x1, and 0.5 m ledges with their tops at y0+floorH, y0+2·floorH, … while ≤ y1. Fins and ledges stick out from z to z + depth. bayW and floorH must be > 0. |

Examples, from the default brutalist map and the pine forest, plus a sketch for a neon street:

```
box -4.5 0 40 -3 34 95                  # monolith: canyon wall, 1.5 m thick, 34 m tall, 55 m long
stairs 0 0 -40 22 40 0.3 0.75           # monolith: 40 steps 22 m wide, 0.3 m rise, 0.75 m run: z -40 -> -70, up to y 12
waffle -30 30 12 92 -100 2 4 4          # monolith: tower facade, fins every 4 m, ledges every 4 m, 2 m deep
box 22 0 -42 52 2 -10                   # pine_forest: the lowest rock terrace of the ridge, top at y 2
block 6 0 -12 3 2.6 2                   # neon street: a 3 x 2 m market stall, 2.6 m high, against the facade
```

### 2.2 Placement prefixes

They go in front of a primitive (box primitive, `model` or `light`) on the same line.

- `repeat n dx dy dz` makes n copies (n ≥ 0). Copy *i* (0-based) is shifted by `i · (dx, dy, dz)`. Up to **3** repeats on one line make a grid. The first repeat is the outer loop, so it decides the object order.
- `repeat n dx dy dz from k` counts the copies from *k*: copy *i* = k … k+n−1 is shifted by `i · (dx, dy, dz)`. It gives exactly the same floats as copies k… of the longer row, so a row split in two (`repeat 3 0 0 2.6 …` + `repeat 5 0 0 2.6 from 3 …`) keeps every position bit for bit, even with steps like 2.6 where moving the start value into the coordinates would change the last bit. Manual edits write it when they split a row (§2.9); for whole and half-number steps they write plain start values instead.
- `id n` gives the line's objects a stable name (an integer 1 … 2147483647, unique in the file, written first: `id 12 repeat 4 0 0 3 box …`). It changes nothing in the scene. It is the key of the line's entities (§2.9), so they keep their identity when the line's numbers change. Manual edits add it to the lines they change; keep it when you edit such a line, and when you split it, keep it on the first piece only (two lines with the same id fail to load).
- `at x y z` shifts the primitive by (x, y, z), so its own coordinates become relative to that point. Only one `at` is allowed per line.
- Shifts move **position arguments only** (box: all six numbers; block: `cx y0 cz`; stairs: `cx baseY startZ`; waffle: `x0 x1 y0 y1 z`; model and light: `x y z`, never a direction). Sizes, `steps`, `rise`, `run`, `depth`, `bayW`, `floorH`, rotations and scales are never shifted.

```
repeat 4 9 0 0 repeat 8 0 0 9 block -36 0 -32 2.2 20 2.2   # monolith: 4 x 8 pillar grid, x -36..-9, z -32..31
repeat 5 1 0.4 0 box 17 -1 -28 18 0.4 -25                  # pine_forest: 5 steps climbing +X, 0.4 m each
repeat 8 4 0 0 light point -14 3.2 -11 color 255 40 160 energy 2 range 6   # neon street: a row of pink signs
```

**`at` is legacy:** it reproduces the default map's original C loops bit for bit (`(at + i·d) + value` in float). For new content write the first copy's real coordinates and a `repeat`.

### 2.3 Groups

A box, model or light belongs to the most recent `group` above it. Each `group` statement creates a new group, even when the name repeats, so **don't reuse a group name** in a second place: add the lines to the existing block instead. In-game Ctrl+click selects one group statement's objects. Group names can be any length but can't contain `"`. A map's groups are listed in its `MAP.md` (*Groups and landmarks*).

### 2.4 Materials

```
material "name" key value [key value ...]     define a material, or add keys to one (long definitions may span lines)
use "name"                                    following boxes (and models, as an override) use it, like `group`
```

- `"default"` always exists (plain PBR: albedo 180 180 180, roughness 0.9, metalness 0) and may be redefined. A material must be defined before a `use` names it; `use "default"` goes back.
- Keys: `shader "shaders/x.glsl"` (an R3D surface shader), `uniform <name> f [f f f]`, `sampler <name> "textures/x.png"`, `data_sampler <name> "textures/x.png"` (the same for a texture of data, not colour, e.g. a tile baked with `--bake-surface` and read with `texelFetch`: loaded linear, without the sRGB decode, nearest, no mipmaps), `albedo R G B [A]` (0–255 sRGB), `albedo_map`, `normal_map`, `normal_scale`, `orm_map`, `occlusion`, `roughness`, `metalness`, `specular`, `emission R G B`, `emission_energy`, `emission_map`, `uv_scale u v`, `uv_offset u v`, `wrap repeat|clamp|mirror` (default repeat), `alpha_cutoff` (0 = never discards, allows the depth pre-pass), `cull back|front|none`, `transparency off|prepass|alpha`, `blend alpha|additive|add_alpha|multiply|premultiplied`, `unlit on|off`, `instanced on|off` (default off: keep this material's boxes as instanced unit cubes, needed by shaders that read `INSTANCE_*` or the model-space `POSITION` in `vertex()`; the loader warns and does it anyway when it finds those in the shader source), `priority n`, and `inherit albedo|normal|orm|emission|all` (model overrides only: keep that map of the model's own material). Unset keys keep R3D's defaults.
- Boxes are drawn per material and 32 m cell: as one mesh of the faces no other box hides (a face is dropped only when opaque boxes cover all of it, so what you see and the shadows don't change), or, with `instanced on` / a shader that needs the cube, as instanced unit cubes. A handful of materials costs nothing; dozens are fine.
- **Box UVs are per face (0–1)**: a texture stretches with the box. A textured material only looks right on boxes of one size, or with a `uv_scale` matched to that size (`maps/pine_forest`: `uv_scale 286 286` tiles a 2.1 m texture over the 600 m ground). For many box sizes, prefer flat colours or a world-position shader like the default map's `concrete.glsl`.

```
material "concrete" shader "shaders/concrete.glsl" uniform u_scale 1 alpha_cutoff 0             # monolith
material "needles" albedo 62 94 72 roughness 0.9 metalness 0                                    # pine_forest
material "embers" albedo 30 14 8 emission 255 110 40 emission_energy 5 roughness 1 metalness 0  # pine_forest
material "wet asphalt" albedo 28 28 32 roughness 0.15 metalness 0                               # neon street
```

### 2.5 Environment

`env <key> <value...>`, one key per line. Every key has a built-in default (`BuiltinEnvironment()` in `runtime/monolith_scene.c`: the values the default map was built with); `--new-map` writes them all out, so a map's file shows its whole look.

- **R3D environment:** every `R3D_Environment` field by its C name: `background.*`, `ambient.*`, `ssao.*`, `ssil.*`, `ssgi.*`, `ssr.*`, `fog.*` (`fog.mode disabled|linear|exp2|exp`, `fog.color`, `fog.density`, …), `volumetricFog.*`, `dof.*` (`dof.mode disabled|enabled`), `bloom.*` (`bloom.mode disabled|mix|additive|screen`), `autoExposure.*`, `tonemap.*` (`tonemap.mode linear|reinhard|filmic|aces|agx`, `tonemap.exposure`), `color.*`. Booleans `on`/`off`, colours `R G B [A]`.
- **Sky:** `sky.mode procedural|color|panorama`, `sky.size`, `sky.panorama "textures/x.hdr"`, and the procedural sky's colours, curves and sun disc (`sky.skyTopColor`, `sky.skyHorizonColor`, `sky.groundBottomColor`, `sky.sunSizeDeg`, …; `sky.sunDirection` follows `sun.direction` unless set).
- **Ambient light (IBL):** `ambientSky.mode procedural|same|off`, `ambientSky.size`, and `ambientSky.<sky key>`: the ambient map comes from a copy of the visible sky with these keys changed. A brighter `ambientSky.groundBottomColor` is the fill light for undersides (see *Dark maps* below).
- **Sun** (the one shadowed directional light, with static shadow tiles): `sun.enabled`, `sun.direction x y z` (the direction the light travels), `sun.color`, `sun.energy`, `sun.specular`, `sun.fogEnergy`, `sun.range` (shadow distance), `sun.shadow`, `sun.softness`, `sun.depthBias`, `sun.slopeBias` (tuned for `shadow.size`; a quality preset or `--shadow` with a smaller map scales both up by the texel ratio, so they need no retuning per preset); `shadow.size` (px, read at start-up only).
- **Volumetric fog fade:** `volumetricFog.fadeStart`, `volumetricFog.fadeEnd` (m from the eye; default 0 0 = off; `fadeEnd` should be > `fadeStart`): the fog's density is full up to `fadeStart` and fades out smoothly by `fadeEnd`. Keeps close light shafts while buildings further away stay clear, and gives the fog a soft end instead of the hard stop at `volumetricFog.length` (`maps/monolith`: 10 50). R3D patch 13; ignored with upstream R3D. With a fade the fog's sun shadow also fades by each step's own distance from the eye (patch 14), so the haze has no hard line where the ground crosses 75% of `sun.range` (without a fade, fog in front of anything past that distance is unshadowed). `volumetricFog.fadeNear` (m, default 0 = off; larger than `fadeStart` is clamped to it): the density also fades *in*, from 0 at the eye to full at that distance (patch 15). Without it the fog around a sunlit eye veils the whole view, so the scene looks brighter when the player stands in sun than in shade (`maps/monolith`: 15 15 50).
- **Quality presets:** `bloom.offExposure` (default 1.19): the exposure factor that replaces bloom when the low preset (or `--bloom off`) turns it off. A map whose bloom is stronger than the default map's (intensity 0.04) sets its own value, measured so the mean brightness of its viewpoints stays the same (`maps/pine_forest`: bloom 0.06, `bloom.offExposure 1.3`).
- **Cache hints** (optional): `shadow.home x y z`, `fogCache.region x0 y0 z0 x1 y1 z1`. Without them both follow the map's bounds, which is what new maps want; the hints only exist to reproduce an older layout exactly.
- Out-of-range values (e.g. `volumetricFog.stepSize` < 0.05) and zero directions are rejected with `file:line: message`.
- **Engine pitfalls:** dense volumetric fog turns shadowed areas into a black curtain (keep `scatteringDensity` low, add a little `emissionEnergy`, raise shafts with `sun.fogEnergy`); keep `sun.range` ≥ `volumetricFog.length`; washed-out images want less `sun.energy` or `tonemap.exposure`, not brighter fog.

- **Dark maps** (dusk, night): keep `color.contrast` at **1**. R3D applies contrast in linear space after the tonemap, so any value > 1 clips the darkest tones to pure black (at 1.08 a quarter of a dusk view went pure black). Darken the mood with `tonemap.exposure` and `ambient.energy` instead, and keep the ambient sky (`ambientSky.*`) brighter and bluer than the visible sky, or shadows go black. Count pure-black pixels of a few shots: outside genuinely black objects there should be none.
- **Bloom and the low preset:** if a map's bloom is stronger than the default map's (`bloom.intensity` 0.04, threshold 0), set `bloom.offExposure` so `--bloom off` keeps the mean brightness of its viewpoints (Sun / quality keys above).

Two moods, side by side:
```
env sun.direction 0.55 -0.78 0.3        # monolith: high side sun through roof gaps
env fog.density 0.0085                  # monolith: megastructures fade into grey haze
env sun.direction 0.7 -0.32 0.64        # pine_forest: low, warm dusk sun between the trunks
env fog.color 62 70 102                 # pine_forest: blue-violet dusk fog
```

**Dusk**: copy the whole `env` block of `maps/pine_forest/pine_forest.map` (low warm sun, mauve horizon, blue-violet fog and ambient, exposure 0.95, contrast 1) and change colours from there. **Night** (moonlight + neon; the keys that differ from the `--new-map` daylight template, for a street with wet asphalt and neon signs: no pure black, mean brightness ~40/255):
```
env sky.skyTopColor 6 8 22               # near-black blue zenith
env sky.skyHorizonColor 40 34 60         # violet city glow on the horizon
env sky.groundHorizonColor 40 34 60      # = the horizon colour: no line where the ground ends
env sky.groundHorizonCurve 0.5
env sky.groundBottomColor 10 10 16
env sky.sunColor 190 205 255             # the "sun" is the moon
env sky.sunSizeDeg 1.5
env sky.sunEnergy 1.5
env ambientSky.skyTopColor 70 84 140     # the fill light: brighter than the visible night sky
env ambientSky.groundBottomColor 60 56 72
env ambient.energy 1.2
env sun.direction -0.35 -0.55 -0.76
env sun.color 160 180 255
env sun.energy 0.5
env sun.fogEnergy 0.8
env fog.color 22 24 42
env fog.density 0.012
env volumetricFog.emissionColor 22 24 42
env volumetricFog.emissionEnergy 0.06
env ssr.enabled on                       # wet, low-roughness ground reflects the signs
env bloom.intensity 0.08
env tonemap.exposure 1.6
env color.contrast 1
```
The light then comes from the map's lights: emissive sign materials with a weak point light in front of each (`energy 2 range 8`), lamps, windows (§2.6); props keep their own albedo (`inherit albedo`) with a low roughness for wet reflections.

### 2.6 Lights, spawn, viewpoints

```
light point x y z [key value ...]           point light
light spot  x y z dx dy dz [key value ...]  spot light shining along (dx, dy, dz)
light dir   dx dy dz [key value ...]        extra directional light (camera-fitted shadow, expensive: re-rendered every frame, and each
                                            shadowed one takes another directional shadow layer, 128 MB at 8192 px)
    keys: color R G B, energy f, range f, falloff f, specular f, fogEnergy f, inner deg, outer deg, shadow on|off,
          clip x0 y0 z0 x1 y1 z1
    defaults: white, energy 1, range 16 (dir: 50), falloff 1, specular 1, fogEnergy 1, cone 22.5/45, shadow off, no clip
spawn x y z yawDeg [pitchDeg]               player start (feet)
viewpoint "Name" ex ey ez tx ty tz          spectator viewpoint (eye, target), in order: --view 1..n, --bench, ← / →
```

- Lights belong to the current group and are shifted by `repeat` / `at`. A shadowed point or spot light renders its shadow map once per map load; use `shadow on` only for the few lights that matter (`maps/pine_forest`: the campfire, `light point 0 1.1 0 color 255 130 50 energy 5 range 18 falloff 1 fogEnergy 0.8 shadow on`; its lanterns are unshadowed).
- **Unshadowed lights shine through walls and floors.** A light's `range` is a sphere, so a lamp in a room also lights the room below, above and beside it. `clip x0 y0 z0 x1 y1 z1` (a world box, shifted by `repeat` / `at` like a box) limits the light to points inside the box: give every interior light the box of its own room or storey, from the middle of the slab below to the middle of the slab above, so the slab's top face is lit and the storey below is not (`maps/monolith` tower: `clip -31 15.85 -141 31 19.85 -99` on storey 1). It also clips the light's volumetric fog, and a tight box makes the light cheaper: the light is skipped when its box is out of view and drawn only on the pixels where the inside of its box can be seen (a room behind a wall costs almost nothing). R3D patches 16, 22 and 23; ignored with upstream R3D.
- Without `spawn` the player starts at feet (0, 0, 0), yaw 0; without viewpoints one is made from the spawn. Viewpoints are what ← / → fly between and what `--bench` measures, so add one for each area worth looking at.

### 2.7 Models

```
model "models/x.glb" x y z [rot yawDeg pitchDeg rollDeg] [scale s | scale sx sy sz] [origin model|bottom|center]
      [material "name"] [material.N "name"] [collide aabb|none|box x0 y0 z0 x1 y1 z1 [box ...]] [shadow on|off]
```
(one line; wrapped here)

- glTF/GLB preferred (also OBJ, FBX, DAE through R3D's importer). Each file is loaded once per map load and drawn instanced. Animations and skins are ignored (static pose).
- `rot`: yaw about +Y, pitch about +X, roll about +Z, applied roll, then pitch, then yaw (`R = Ry · Rx · Rz`); yaw 90 turns the model's +Z to +X. `scale` > 0 (files have no unit scale: a model in centimetres needs `scale 0.01`).
- `origin bottom` stands the model on y by its lowest vertex after scale and rotation, centred over x z by its file bounds: the safe way to stand a model on a surface. `center`: the same x z and the middle between its lowest and highest vertex; `model` (default): the file's own origin. World bounds, `collide aabb` and picking use the rotated vertices, not the rotated file box.
- Materials: `material "name"` overrides every slot, `material.N "name"` slot N (`--inspect-model` lists the slots). Precedence: `material.N`, then `material`, then the active `use` (when it is not `"default"`), else the file's own materials.
- `collide`: `aabb` (default: the world bounds block the player), `none`, or `box …` boxes in model space (before scale and rotation), each turned into a world AABB (a tree's trunk only). `shadow off` stops it casting the sun shadow (it still receives shadows).
- Every model file needs a recorded licence (CC0 / public domain, or CC-BY with the attribution): a row in the package's `ASSETS.md` and a `<file>.asset.md` sidecar. `tools/fetch_asset.py` downloads from licence-safe sources and `tools/record_asset.py` writes both records. Restyle a model through material overrides in the map, never by editing the file, so the original stays traceable.

```
model "models/tree_pineTallC.glb" -30.07 0 15.26 rot 35 0 0 scale 12.84 origin bottom material.0 "needles" material.1 "bark" collide box -0.025 -0.05 -0.025 0.025 0.4 0.025   # pine_forest
```

### 2.8 Limits and errors

**No limits on content:** any number of boxes, models, groups, materials, lights and viewpoints, and lines of any length (everything is allocated as needed). Sanity caps only: at most 1 000 000 objects (boxes + model instances + lights) per map, so a `repeat` grid that multiplies to millions fails with a clear message, and a 4 MB map file. Spawn, viewpoint and light positions are limited to ±1 000 000 m, and a spot light's `inner` angle must not exceed `outer`. **Errors** (unknown keyword, wrong argument count, bad number, a missing file, a path outside the package, an undefined material, too many objects, a map with no boxes and no models) are reported as `file:line: message`.

### 2.9 Entities and manual edits

Every box, model instance and light is an **entity**: an id plus its data (components) in the runtime's entity arrays (`MS_Scene.entityIds`, `entityObjects`, `entityTransforms`, `entitySources`, `entityModels`; `MS_FindEntity`, `MS_ObjectEntity` in `runtime/monolith_scene.h`). The id is the line's key plus the object's index within the line:

- a line with `id n` has the key *n*: stable as long as the line keeps its `id`, wherever it moves and however its numbers change;
- a line without one has a key derived from its text (shown with bit 31 set, e.g. `a278642c`): stable while that text stays the same, also when other lines are inserted, removed or moved; it changes when the line itself changes. Identical lines are numbered in map order, so removing the first of two identical lines gives the second one the first one's key.

`examples/05_entities` shows how a game uses them (its own components and systems, lookup by id). `--dump-entities <file>` lists them (`entity <e> id <key>:<index> explicit|implicit <kind> …`).

**Manual edits** (the edit mode, **M**, and `--apply-edits`) change the map file itself, with the smallest text edit, and the game reloads it: moving or turning an object rewrites the numbers on its line (and adds `id n` to it), removing it deletes the line, and one copy of a `repeat` line is split out onto its own line (the other copies keep their exact floats, with `from` where needed). Giving a line an id (**Ctrl+I**, the script's `id [n]`) changes only its `id n` prefix; removing one (**Delete** in the id panel, **F5**, the script's `id remove`) deletes only that prefix (with the blanks after it), and the line's objects get implicit keys again. Duplicated and added objects are new lines with a new `id`; an added model goes right after the line of the object it stands on (in its group), wrapped in `use "default"` … `use "<material>"` when a `use` is active there (a `use` would restyle it). Comments, spacing, groups and every other line stay as they were. Only boxes from `box` and `block` lines can turn: about the vertical in 90° steps, about their centre (the footprint's x and z extents swap) or, from the transform modal (**R**) and the script's `turn … pivot x y z`, orbiting a pivot with the footprint turned; they can be scaled (corners about a pivot; a block's centre and sizes) and resized (`size`). Stairs and waffles can only move. A model's turn about another axis than the vertical composes with its `rot` (R = Ry · Rx · Rz) and is written back as yaw, pitch and roll with 6 decimals; a turn about the vertical adds to the yaw and one about the model's own z to the roll, exactly. Scaling a model multiplies its `scale` (each model axis by the factor of the world axis nearest to it) and moves its position about the pivot. A turned or scaled whole `repeat` line with a pivot gets its steps (and `at`) turned or scaled too.

