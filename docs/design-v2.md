# monolith v2: design record (interfaces for the parallel work streams)

> **Design record.** This note was written before the v2 work fanned out
> (the round-2 task prompt, now retired: its reasons are in `docs/knowledge/history.md`, its
> working method in `docs/knowledge/working-method.md`) and fixed the shared interfaces; the streams appended
> implementation notes (§8–§10) as they merged. It is kept as the record of *why* things are
> the way they are. It is not the reference: where it and the code differ, the code wins, and
> the current, authoritative descriptions are
> - the map format: the header comment of `runtime/monolith_scene.c` and `docs/knowledge/map-format.md`;
> - the runtime API: `runtime/monolith_scene.h` and `PROMPT-INSTRUCTIONS.md` §6.1;
> - request bundles: `AI-MAP-TOOL.md` §2 (and `src/launcher.c`);
> - flags, keys and environment variables: the header comment of `src/main.c` and
>   `PROMPT-INSTRUCTIONS.md` §5;
> - a map's look and rules: its `MAP.md` (the default map: `maps/monolith/MAP.md`).
>
> Later additions are summarised in §11.

Goals in one line each: any style of map; a per-map design brief; models; export/import
to plain raylib + R3D projects; `examples/`; Claude terminals on i3 workspace 10 with
permission flags; screen-region selection.

---

## 1. Map package

```
maps/<name>/
├── <name>.map        geometry, materials, models, lights, environment (format v2)
├── MAP.md            design brief, read by the AI first on every request
├── shaders/          surface shaders used by this map's materials
├── models/           model files, each with a provenance sidecar <file>.asset.md
├── textures/
└── ASSETS.md         every external asset: source URL, author, licence (+URL), date, sha256, changes
```

- Every path inside a map file is **relative to the folder that contains the map file**
  (the package root). Absolute paths and `..` components are rejected by the loader
  (`file:line: path must stay inside the package`), so a package can be copied anywhere.
- `--map <path>` accepts a package folder or a `.map` file. For a folder the loader uses
  `<folder>/<basename of folder>.map`, else the only `*.map` inside it, else it fails with
  a message listing what it found.
- A single `.map` file outside a package (v1 or v2) loads too; its folder is its root.
- The default map is `maps/monolith/` (`maps/monolith/monolith.map`). `concrete.glsl` moves
  to `maps/monolith/shaders/concrete.glsl`; nothing else in the engine refers to it.
- `--new-map <name>` creates `maps/<name>/` with `<name>.map` (format line, a ground slab,
  a default material, the default environment written out), a `MAP.md` template with the
  recommended sections (§1.1), empty `shaders/ models/ textures/` and an empty `ASSETS.md`.
  It refuses to overwrite an existing folder.

### 1.1 `MAP.md` (free-form Markdown; recommended sections)

*Style and mood*, *Scale and walkability* (player size, step height, headroom, typical
dimensions), *Materials and palette*, *Lighting and atmosphere*, *Rules for new content*
(when to use boxes, when models are allowed, how models are restyled, polygon budget,
texture size limit), *Groups and landmarks*. The default map's `MAP.md` takes over
everything brutalist from the project docs.

---

## 2. Map format v2

Format v1 (today's grammar: `group`, `box`, `block`, `stairs`, `waffle`, `repeat`, `at`)
stays valid unchanged. v2 adds statements; still one statement per line, `#` comments,
`"strings"` without escapes, numbers as plain decimals.

### 2.1 Version line

```
monolith 2          first statement of the file (comments/blank lines may precede it)
```

- No version line: format 1. Only v1 statements are accepted; everything else uses the
  built-in defaults (§2.6).
- `monolith 2`: all statements below are accepted.
- `monolith N` with N > the runtime's `MS_FORMAT_VERSION`:
  `file:line: map format N is newer than this runtime supports (2); update monolith_scene`.

### 2.2 Materials

```
material "name" key value [key value ...]
use "name"                 following primitives (boxes and models) use this material,
                           like `group`; `use "default"` goes back to the default
```

- Several `material` lines with the same name add to / override keys of one material, so
  long definitions can span lines. A material must be defined before a `use` names it.
- The material `"default"` always exists (built-in: plain PBR, albedo 180 180 180,
  roughness 0.9, metalness 0). A map may redefine it with `material "default" ...`.
- Keys (all optional):

| Key | Values | R3D field |
|---|---|---|
| `shader` | `"shaders/x.glsl"` | `R3D_LoadSurfaceShader`; one shader instance per material (uniforms are global per shader, §7.4) |
| `uniform` | `<name> f [f f f]` (repeatable) | `R3D_SetSurfaceShaderUniform`, float / vec2 / vec3 / vec4 |
| `albedo` | `R G B [A]` (0–255 sRGB) | `albedo.color` |
| `albedo_map` | `"textures/x.png"` | `albedo.texture` |
| `normal_map`, `normal_scale` | path; float | `normal.texture`, `normal.scale` |
| `orm_map` | path (R occlusion, G roughness, B metalness) | `orm.texture` |
| `occlusion`, `roughness`, `metalness` | float | `orm.*` |
| `emission`, `emission_energy`, `emission_map` | `R G B`; float; path | `emission.*` |
| `uv_scale`, `uv_offset` | `u v` | `uvScale`, `uvOffset` |
| `alpha_cutoff` | float (0 = never discards; enables the depth pre-pass for it) | `alphaCutoff` |
| `cull` | `back` / `front` / `none` | `cullMode` |
| `transparency` | `off` / `prepass` / `alpha` | `transparencyMode` |
| `inherit` | `albedo` / `normal` / `orm` / `emission` / `all` (repeatable) | model overrides only: keep that map from the model's own material (§2.5) |
| `wrap` | `repeat` / `clamp` / `mirror` (stream B) | wrapping of the material's textures and samplers; default `repeat` (R3D's own default is clamp, which made `uv_scale` > 1 useless); trilinear + mipmaps |

- Rendering: **one instanced draw per material that has boxes** (the default map: one).
- Picking, the HUD and request bundles name a box's or model's material.

### 2.3 Environment

```
env <key> <value...>
```

Keys are dotted paths. Every key has a built-in default equal to today's hard-coded value
(§2.6), so a v1 map renders as before apart from its material.

- **R3D environment**: the `R3D_Environment` field names, e.g. `fog.mode exp2`,
  `fog.color 150 148 143`, `fog.density 0.0085`, `fog.skyAffect 0.7`,
  `volumetricFog.enabled on`, `volumetricFog.scatteringDensity 0.012`, `ssao.radius 1.2`,
  `ssil.giIntensity 1.2`, `bloom.mode mix`, `bloom.intensity 0.04`, `tonemap.mode agx`,
  `tonemap.exposure 0.9`, `color.saturation 0.8`, `ambient.energy 0.9`,
  `background.color ...`, `ssr.*`, `ssgi.*`, `dof.*`, `autoExposure.*`.
  Enums by lower-case name (`exp2`, `linear`, `exp`, `disabled`; `mix`, `additive`, `screen`;
  `linear`, `reinhard`, `filmic`, `aces`, `agx`). Booleans `on`/`off`. Colours `R G B [A]`.
  The table lives in one place in `monolith_scene.c` and covers every field, so imports
  can express any R3D setup.
- **Sky** (`sky.*`): `sky.mode procedural|color|panorama`, `sky.size 1024`,
  `sky.panorama "textures/x.hdr"`, and the `R3D_ProceduralSky` fields
  (`sky.skyTopColor`, `sky.skyHorizonColor`, `sky.skyHorizonCurve`, `sky.skyEnergy`,
  `sky.groundBottomColor`, `sky.groundHorizonColor`, `sky.groundHorizonCurve`,
  `sky.groundEnergy`, `sky.sunDirection`, `sky.sunColor`, `sky.sunSizeDeg`, `sky.sunCurve`,
  `sky.sunEnergy`). `sky.sunDirection` defaults to `sun.direction`.
- **Ambient-only sky** (`ambientSky.*`, §7.5 of PROMPT-INSTRUCTIONS): the same keys as
  `sky.*`, applied to a copy of the visible sky that only feeds `R3D_GenAmbientMap`;
  `ambientSky.size 256`; `ambientSky.mode same|procedural|off` (`same` = use the visible sky).
- **Sun** (`sun.*`, the one shadowed directional light with the static shadow tiles):
  `sun.enabled on`, `sun.direction x y z` (the direction light travels), `sun.color`,
  `sun.energy`, `sun.specular`, `sun.fogEnergy`, `sun.range`, `sun.shadow on|off`,
  `sun.softness`, `sun.depthBias`, `sun.slopeBias` (scaled by the texel ratio when the shadow map is smaller than `shadow.size`), `bloom.offExposure` (exposure factor when a quality setting turns bloom off). `--shadow <size> <range>` overrides
  `shadow.size` / `sun.range`.
- **Caches**: `shadow.size 8192`, and optional placement hints `shadow.home x y z` and
  `fogCache.region x0 y0 z0 x1 y1 z1`. Without hints both are derived from the map bounds
  (§4); a hint is only needed to reproduce an existing layout bit for bit.

### 2.4 Lights, spawn, viewpoints

```
light point x y z [color R G B] [energy f] [range f] [falloff f] [specular f] [fogEnergy f] [shadow on|off]
light spot  x y z dx dy dz [inner deg] [outer deg] [same keys as point]
light dir   dx dy dz [color ...] [energy ...] [shadow on|off]     extra directional lights (no static tiles)
spawn x y z yawDeg [pitchDeg]          player start, feet position
viewpoint "Name" ex ey ez tx ty tz     spectator viewpoint (eye, target); order = 1..n for --view and --bench
```

`repeat` / `at` shift light positions (not directions). Lights belong to the current group.
Without `spawn`: feet (0, 0, 0), yaw 0. Without viewpoints: one viewpoint from the spawn.
The default map's player start `(0,0,90)` yaw 180 and the 7 `VIEWPOINTS[]` move into its
map file (exact same floats).

### 2.5 Models (Phase 2)

```
model "models/x.glb" x y z [rot yawDeg pitchDeg rollDeg] [scale s | scale sx sy sz]
      [origin model|bottom|center] [material "name"] [material.N "name"]
      [collide aabb|none|box x0 y0 z0 x1 y1 z1 [box ...]] [shadow on|off]
```
(one line; wrapped here for reading)

- `x y z` are shifted by `repeat` / `at`. Rotation: yaw about +Y, pitch about +X, roll about
  +Z, applied roll, then pitch, then yaw (`R = Ry · Rx · Rz`), degrees.
- `origin`: `model` (default) puts the file's origin at x y z; `bottom` puts the bottom
  centre of the model's (scaled, rotated) bounds there; `center` the bounds centre.
  `bottom` is the safe way to stand a model on a surface.
- `material "name"` overrides every material slot; `material.N "name"` slot N only. Without
  either, the model's own materials are used, unless a `use` is active: then `use` applies
  as the override (so `use "concrete"` restyles following models too). Overrides can keep the
  model's own maps with `inherit` (§2.2).
- `collide`: `aabb` (default) = the transformed bounds; `none`; or `box ...` = one or more
  boxes in model-local space (before rotation/scale), each turned into a world AABB.
- Each file is loaded once per map load and drawn instanced per (file, override set).
- Picking: ray vs transformed bounds, then triangles where cheap (`MS_Raycast`).
- Models cast and receive the sun shadow, count towards the map bounds, invalidate caches.

### 2.6 Built-in defaults

The defaults are today's values from `SetupEnvironment()` and `main()` (sun direction
`0.55 -0.78 0.30`, colour 255 238 214, energy 2, fogEnergy 4, range 100, softness 2;
procedural sky and ambient-only sky as in `atmosphere.c`; fog, volumetric fog, SSAO, SSIL,
bloom, tonemap, colour as there), except the material: the built-in `"default"` is plain
PBR (§2.2), not concrete. The default map writes **all** of its settings out explicitly,
so later changes to the defaults never change it.

### 2.7 Limits

**No content limits** (stream M): boxes, groups, materials, model files, model instances,
model collision boxes, `material.N` overrides, `collide box` boxes per line, lights,
viewpoints, uniforms / samplers per material, textures, dependencies, names (any length)
and map lines (any length, any number of words) are heap arrays and heap strings sized to
the map. Only sanity caps against runaway input remain, each with a `file:line: message`:

| Cap | Value | Why |
|---|---|---|
| `MS_MAX_OBJECTS` | 1 000 000 boxes + model instances + lights together (and, separately, 1 000 000 model collision boxes) | a `repeat` grid (or `stairs` count) that multiplies to 100 M boxes; also the largest `repeat` / `stairs` count |
| `MS_MAX_FILE_BYTES` | 4 MB map file (and a `.gltf` / `.obj` read for its references) | a wrong file passed as a map |
| `MS_PATH_MAX` | 1024 bytes for a path the loader builds (package root + relative path) | stack scratch, `path too long` |
| `repeat` per line | 3 | the grammar (a 3D grid) |

**Memory rule (editor and runtime):** anything whose size depends on the content (the map,
the selection, the regions, the prompt, open requests, child processes, bench frames, export
file lists, ...) is a heap array grown with `MS_RESERVE` / `MS_PUSH` (declared in
`runtime/monolith_scene.h`, used by `src/` too) or a heap string. Fixed `char` arrays remain
only for stack scratch that formats one message or one path, and for single values bounded
by the loader (the environment's sky panorama path, the last error message). The per-frame
path (drawing, picking, HUD, collision) never allocates: its scratch arrays grow to the
largest size seen and are reused; allocation happens on (re)load, on input events (selection,
paste, regions), at submit / export / bench report time.

---

## 3. Scene runtime (`runtime/monolith_scene.h` + `runtime/monolith_scene.c`)

A separately compilable C11 module, depending only on raylib, raymath, rlgl and R3D. It is
what the game uses to load, hot-reload, draw, collide and pick, and what `--export` ships.
Editor-only code (designer, launcher, bench, prompt box, frame reuse, player, spectator)
stays in `src/`.

- The header (`runtime/monolith_scene.h`) is the API contract; see its comments.
- **Patched vs. upstream R3D.** monolith's R3D copy has local patches (static shadow tiles
  `R3D_PushLightFixedShadow`, depth pre-pass, fog bake, ...). The runtime compiles against
  both: with `-DMS_PATCHED_R3D` (set by `build.sh` and `examples/build.sh` when they build
  against `submodules/r3d`) it uses the patched APIs; without it, it falls back to
  `R3D_PushLightEx` with the camera-fitted map re-rendered when needed, no pre-pass toggle
  and no fog cache. The export README says which image differences that implies.
- It owns everything that was in `map.c`, `world.c`, `shadow.c` and the scene part of
  `atmosphere.c` / `main.c` (materials, instances, sun + shadow maps, sky, ambient map,
  lights, fog cache). `--ao` / `--bloom` / `--fx` stay in the game as overrides applied
  after `MS_ApplyEnvironment` (as implemented later: `--ao` and `--bloom` became fields of
  the runtime's `MS_Quality`, applied by `MS_ApplyQuality`; only `--fx` stays game-side, §11).
- Hot reload watches every file the scene depends on (map, shaders, model files,
  textures). Any change reloads the whole package and bumps `version`; a failed reload
  keeps the old scene. New GPU resources are created before old ones are freed (§7.4
  instance-buffer name caching).
- `version` changes on every successful (re)load; everything that changes the image
  without a reload is in `R3D_GetEnvironment()` or in `MS_RenderSettings`, so the game's
  `FrameKey` = camera + environment bytes + render settings + graphics settings (`GfxSettings`, F2) + R3D's AA mode + `version` + window size.

---

## 4. Caches follow the map

- **Shadow tile home** (`TILE_HOME`, today `(10, 38, 2)`) and the **fog-cache region**
  (today `FOG_CACHE_REGION`) are derived from the loaded map's bounds, ignoring huge
  ground-like boxes as the stream that implements it decides and documents. Explicit hints
  (`env shadow.home`, `env fogCache.region`) override. The default map must give
  pixel-identical images; if the derived values can't reproduce today's layout exactly,
  the default map carries the hints.
- **As implemented (stream A):** a box is *ground-like* when its smaller horizontal extent
  is >= 100 m and its height <= 5% of that; the *content bounds* are all other boxes (all
  boxes if only ground-like ones exist). The shadow tile home is the centre of the AABB of
  the viewpoint eyes (a map without viewpoints gets one from its spawn), else of the content
  bounds. The fog-cache region is the content bounds + 2 m, shrunk around the play centre
  (keeping its proportions, staying inside the bounds) until it has at most 64 M voxels at
  the requested voxel size. Derivation does not reproduce `(10, 38, 2)` / `FOG_CACHE_REGION`,
  so `maps/monolith/monolith.map` carries `env shadow.home 10 38 2` and
  `env fogCache.region -70 -1 -150 75 100 105`.

---

## 5. Selection record and bundle `format: 2`

Object references are `{kind, index}`: `kind` is `box` or `model`, `index` is into
`scene->boxes` or `scene->models` at submit time.

```c
typedef struct { MS_ObjKind kind; int index; } MS_ObjRef;     // runtime header
typedef struct { Rectangle rect; } DesignRegion;               // pixels of view.png, top-left origin
// DesignRequest gains: MS_ObjRef *selected; DesignRegionInfo *regions; int regionCount;  (heap, stream M)
```

`request.json`, `"format": 2` (format 1 bundles stay readable; `AI-MAP-TOOL.md` describes both):

```jsonc
{
  "format": 2,
  "kind": "edit",                       // "edit" | "import" | "new-map"
  "prompt": "...",
  "camera": { /* as format 1 */ },
  "hit": { "kind": "box"|"model", "index": 12, "point": [..], "normal": [..], "distance": 4.2,
           "line": 47, "lineText": "...", "group": "...", "material": "concrete",
           "min": [..], "max": [..],                       // world AABB (both kinds)
           "model": { "file": "models/x.glb", "position": [..], "rotationDeg": [..], "scale": [..] } // models only
         } | null,
  "selection": [ { "line": 47, "lineText": "...", "group": "...",
                   "objects": [ { "kind": "box", "index": 46, "nth": 3, "min": [..], "max": [..],
                                  "center": [..], "size": [..], "material": "..." /*, "model": {...} */ } ] } ],
  "regions": [ { "index": 1, "rect": [x, y, w, h], "rectNorm": [x, y, w, h], "crop": "region-1.png",
                 "center": { "point": [..], "normal": [..], "kind": "box", "index": 3, "distance": 12.5 } | null,
                 "depth": { "min": 3.1, "max": 48.0 } | null,
                 "objects": [ { "kind": "box", "index": 3, "line": 47, "group": "...", "material": "...",
                                "share": 0.42 } ],          // share of the region's pixels, sorted descending
                 "sky": 0.10 } ],                           // share of region pixels with no object
  "nearby": [ /* as format 1 */ ],
  "reproduce": { /* as format 1 */ },
  "map": { "path": "maps/monolith/monolith.map", "package": "maps/monolith", "brief": "maps/monolith/MAP.md" | null,
           "formatVersion": 2, "hash": "…", "mtime": 0, "version": 3,
           "boxes": 279, "models": 0, "groups": 13, "materials": 1 },
  "mapReloadedSinceSnapshot": false,
  "import": { "projectDir": "...", "mapName": "...", "package": "maps/<name>" }   // kind "import" only
}
```

`request.md` gets matching sections: *Design brief* (path of `MAP.md`, to be read first),
*Selection* (boxes and models), *Screen regions* (one table per region), and for imports an
*Import* section. Region crops are `region-<n>.png`; `view-marked.png` outlines each region
with its number.

Region object visibility: dense ray sampling with `MS_Raycast` over the region (at most
~4096 rays per region, a regular grid), or an object-ID render; either way the result
lists every object that covers at least one sample.

As implemented (stream E): ray sampling through one function, `RegionPick` in `designer.c`,
which tests per-region candidate lists (boxes culled to the region's and then each 8x8-ray
tile's frustum, nearest first) and returns exactly the crosshair pick's hit; ~6 ms per
full-screen region at 8192 boxes. Extra keys beyond the schema above: `images.regions`;
`hit.box` and selection `objects[].box` (format 1 names, = `index`); `regions[].center.line`
/ `.group`; `regions[].samples`, `.grid` `[w, h]`, `.omittedObjects` / `.omittedShare`
(objects beyond the 256 listed per region; since stream M every object is listed and both are 0);
`regions[].objects[].min` / `.max`. `depth` is the
distance from the eye.

---

## 6. Controls and defaults chosen for the developer's calls

| Decision | Default chosen |
|---|---|
| Snip-mode key | **C** in spectator mode (free today). Drag = new region, Shift+drag = add, RMB/Esc = cancel, Enter = prompt with the regions |
| Workspace for Claude terminals | `MONOLITH_WORKSPACE` (default `10`, empty = don't move) |
| Permission flags | on by default; `MONOLITH_CLAUDE_SAFE=1` drops them |
| Licence policy | CC0 / public domain by default; CC-BY allowed with the attribution recorded in `ASSETS.md` and the export README; nothing else |
| Polygon / texture budget | project default ≤ 50k triangles per model, textures ≤ 2K; a `MAP.md` may set its own |
| Package layout | §1 |

---

## 7. Streams and merge order

| Stream | Content | Depends on |
|---|---|---|
| A | runtime extraction + packages + materials + environment/lights/spawn/viewpoints as data + caches from bounds + `--new-map` + format versioning (Phases 1, 3.1, 3.3) | — |
| F | launcher: workspace 10 + permission flags (Phase 6) | — |
| E | snip-mode regions + bundle format 2 (Phase 5) | rebased on A after it merges |
| C1 | research: model/texture sources, licences, safe download + normalisation tooling | — (read-only) |
| B | models in maps, `--inspect-model`, `--dump-scene` (Phase 2.1) | A |
| C2 | asset workflow docs + the second-style test map with a restyled CC0 model (Phases 2.2–2.3) | B, C1 |
| D | `--export`, `examples/` 01–04, `--import`, capture-shim evaluation (Phases 3.2, 4) | A (B for models) |
| G | style-neutral docs, `maps/monolith/MAP.md` (Phase 7) | last |
| V | independent verification after each merge | each merge |

Merges go into `main` one at a time; after each: both targets build with zero warnings, the
default map's box dump and the verification shots are unchanged.

---

## 8. Stream A implementation notes (differences from the draft above)

- **API additions** (`runtime/monolith_scene.h`): `MS_SetInitHints(scene)` (R3D hints that
  only apply before `R3D_Init`: today the sun's shadow map size; call it after `InitWindow`,
  because `R3D_SetHint` queries GL, e.g. with a `noGpu` scene); `MS_WriteEnvironment(scene
  or NULL, FILE *)` (writes every `env` key; NULL = built-in defaults; used by `--new-map`,
  meant for export); `MS_RenderSettings.depthPrepass` (default true; `R3D_SetDepthPrepass`,
  patched R3D only); `MS_DEFAULT_EYE_HEIGHT` (1.62, for the viewpoint made from the spawn).
  `<stdio.h>` is included by the header. No other signature changed.
- **Parsing:** every referenced file (shader, texture, panorama) must exist at parse time
  (`file:line: file not found: "..."`), also for `noGpu` loads, so `--dump-boxes` is a full
  package check. A v2 map without boxes is rejected (`map is empty`); the models stream
  relaxes that to "no boxes and no models" (done, §9; `model` lines are parsed since stream B). A `monolith N` line after
  another statement is an error.
- **Material keys beyond §2.2:** `sampler <name> "path"` (sampler2D uniform of the material's
  shader), `specular f` (`orm.specular`), `blend alpha|additive|add_alpha|multiply|premultiplied`,
  `unlit on|off`, `priority n`. Unset keys keep `R3D_MATERIAL_BASE` (so `alpha_cutoff` is
  0.5 unless set; set it to 0 for opaque shader materials to get the depth pre-pass).
- **Lights:** defaults white, energy 1, range 16 (point/spot), falloff 1, specular 1,
  fogEnergy 1, cone 22.5/45 deg, `shadow off`. Shadowed point/spot lights render their map
  once per load; shadowed extra `dir` lights are camera-fitted every frame (expensive; they
  also take extra directional shadow layers, 128 MB each at 8192 px).
- **Environment:** `sky.sunSizeDeg` is degrees (converted with `DEG2RAD` in float, so 2.5
  reproduces the old value bit for bit). `ambientSky.<key>` overrides only the keys given;
  the others follow `sky.*`. `sky.mode panorama` loads `sky.panorama` with
  `R3D_CUBEMAP_LAYOUT_PANORAMA`; `ambientSky.mode same` is the right choice with panoramas.
  `shadow.size` is read at start-up only (a reload that changes it logs a warning).
- **Hot reload:** the watch list is the files of the last load *attempt* (so a shader that is
  still missing after a failed reload is picked up when it appears). A reload happens when
  an mtime changes and the combined content hash (map bytes + every watched file) differs
  from the last attempt; the 2 s settle window of v1 is kept.
- **Game side:** the 1-6 toggles are *flips* relative to the map's (+ `--ao/--bloom/--fx`)
  setting; after a reload the game re-applies map environment, overrides and flips, so a
  toggled-off effect stays off. Flipping a mode that the map has off turns on `exp2` fog /
  `mix` bloom (as before). The player starts at the map's `spawn` unless `--pos/--eye/--look`.
  `resources/` is gone (the only resource was the shader); `build.sh` copies `maps/`.
- **Upstream R3D:** without `MS_PATCHED_R3D` the runtime uses `R3D_PushLightEx` (camera-fitted,
  re-rendered when needed), never the fixed-shadow / fog-bake / pre-pass / profiler APIs;
  `MS_SHADOW_TILES` and `fogCacheVoxel` fall back to camera-fitted / off. Checked with
  `-fsyntax-only` and a `gcc -O2 -c` against `~/Projects/r3d/include` (no patched symbols
  referenced); not linked or run against an upstream build.

---

## 9. Stream D implementation notes (export, import, examples, capture)

- **API additions** (`runtime/monolith_scene.h`, a separate region at the end of
  `monolith_scene.c`): `MS_DependencyCount(scene)` / `MS_Dependency(scene, i)` (the watched
  files: [0] the map, then every referenced shader / texture / panorama / model, as root +
  package-relative path; files a model pulls in itself are not listed) and
  `MS_WriteR3DEnvironment(const R3D_Environment *, FILE *)` (every `ENV_KEYS` field, for the
  capture shim). No signature changed.
- **Export layout:** `<dir>/README.md`, `<dir>/monolith_scene.h/.c`, `<dir>/viewer.c`,
  `<dir>/<name>/` (the package; the map file renamed `<name>.map`). Paths need no rewriting:
  the loader rejects anything not package-relative. Model parts (`.gltf` URIs, `.obj`/`.mtl`
  maps) and `<file>.asset.md` sidecars are found by `src/export.c`, not by the runtime.
- **Launcher addition:** `LauncherStartBundle(id, slug, claudePrompt, model, msg)` +
  `LauncherRequestsDir()` / `LauncherRootDir()`, used by `--import`.
- **Import bundle:** `request.json` `format: 2`, `kind: "import"`, `camera`/`hit` `null`,
  empty `selection`/`regions`/`nearby`, `model`, `map` (the new package), `import`
  `{projectDir, mapName, package}` (absolute paths); `request.md` sections *Prompt*, *Import*
  (with an inventory of the project's files), *Verify*, *Map at creation*.
- **Capture shim:** `runtime/monolith_capture.h` (header-only, `MONOLITH_CAPTURE_IMPLEMENTATION`
  in one file, needs `monolith_scene.c` linked). Not used by the game.


## 10. Stream B implementation notes (models, Phase 2.1)

- **Statement** as in §2.5; details (the reference is the header comment of
  `monolith_scene.c`): `scale` values must be > 0 (mirroring would flip the winding);
  `collide box` takes any number of boxes (8 before stream M); a `use` overrides models only when it is not
  `"default"` (`use "default"` means "own materials" for models); precedence `material.N` >
  `material` > `use` > the file's own. `material.N` beyond the file's slot count is a logged
  warning (ignored), not an error, because noGpu loads can't know the slot count.
  `inherit` copies the slot's own texture (`albedo`, `orm`), its whole normal map incl. scale
  (`normal`) or its whole emission incl. colour and energy (`emission`); the override's
  colours and factors apply otherwise. Model lines need format 2.
- **Runtime API additions:** `MS_RaycastModel(scene, model, origin, dir, maxDist, &hit)`;
  `MS_ModelFile.loaded`, `.line`; `MS_ModelInstance.origin`, `.rotation` (quaternion),
  `.translation` (position + origin shift), `.collide`, `.slotFirst/.slotCount`; scene
  `modelSlots[]` / `modelSlotCount` (`MS_SlotMaterial { slot, material }`); enums
  `MS_ModelOrigin`, `MS_ModelCollide`; limits `MS_MAX_MODEL_COLLIDERS`, `MS_MAX_MODEL_SLOTS`,
  `MS_MAX_COLLIDE_BOXES` (removed by stream M, §2.7). `ref.material` of a model = the whole-model override, -1 = own.
- **Loading:** `R3D_LoadModelEx(path, R3D_IMPORT_RETAIN_MESH_DATA)` once per file per load (new
  scene's models load while the old scene is still alive). The CPU positions/indices are
  copied into a BVH for picking, then R3D's mesh data is freed. A noGpu load (main.c's
  peek, `--dump-boxes`) does not read model files: bounds are a zero box (`loaded = false`),
  so origin shifts, model bounds and colliders are only final in GPU loads; `--dump-scene`
  therefore opens a hidden window when the map has models.
- **Drawing:** one `R3D_DrawModelInstanced` per (file, per-slot material set, shadow flag), with
  POSITION | ROTATION | SCALE instance buffers. The group's `R3D_Model` is a shallow copy with
  its own `materials[]` (and `meshes[]` with `R3D_SHADOW_CAST_DISABLED` for `shadow off`).
  R3D does not frustum-cull instanced groups (neither boxes nor models). R3D rotates instance
  normals but does not inverse-scale them, so per-axis scaled models light approximately.
- **Unlit:** the file's material choices are kept, including KHR_materials_unlit (R3D maps it to
  `unlit`); an override material (lit unless `unlit on`) relights it. Finding: the Kenney
  Nature Kit GLBs list `KHR_materials_unlit` in `extensionsUsed` but no material uses it; they
  are lit PBR with `metallicFactor 1` (dark tinted metal). `--inspect-model` warns about both.
- **Picking:** boxes first (lowest index wins ties), then models, which win only when strictly
  nearer; `DesignerPick`, `RegionPick` (models culled per region/tile frustum by their world
  bounds, nearest first) and `MS_Raycast` give the same hit. Model hits report the triangle's
  normal (facing the ray). The overlay outlines models with their oriented box.
- **Collision:** `colliders[]` = boxes, then per instance: `aabb` its world bounds, `box ...` each
  model-space box's transformed AABB, `none` nothing. Without models `colliders` is still the
  box array itself.
- **Caches:** model world bounds count towards `bounds` and the cache content bounds; the sun
  shadow tiles include models (drawn in the same pass, once per map load).
- **Hot reload deps:** the model file plus every `uri` of a `.gltf` / `.glb` JSON (buffers,
  images; `data:` URIs skipped) and an `.obj`'s `mtllib` files and their `map_*` textures; they
  must stay inside the package (`file:line: "models/x.gltf" refers to "...", outside the
  package`) and exist. FBX/DAE references are not followed.
- **Bundles:** `hit.model` and `selection[].objects[].model` = `{ file, position, rotationDeg,
  scale, origin, translation, collide, shadow, slotMaterials }`; region objects of kind model
  carry `file`; `"box"` keys only on boxes. request.md: a model table per selected model line.
- **Fixes from the verifier of stream A (in this stream):** (1) a missing referenced file is
  watched (added to the dependencies before the existence check), so the package reloads when
  it appears; (2) numeric `env` values are range-checked and rejected with `file:line: key
  must be ...` (`ENV_RANGES` in `monolith_scene.c`; e.g. `volumetricFog.stepSize >= 0.05`, which
  used to hang the GPU at 0); (3) zero `sun.direction` / `sky.sunDirection` /
  `ambientSky.sunDirection` are rejected; (4) a spot light with `inner > outer` is rejected,
  the header comment states the `dir` light's default range (50), and shader / texture load
  errors cite the map line of the key that names the file.
- **After the merge of D (stream D's requests):** `MS_Dependency` lists model files and the
  files they name, so `--export` copies them (tested: the exported models package renders
  pixel-identical in monolith and in the exported `viewer.c`). Material textures load with
  `wrap repeat` by default (material key `wrap repeat|clamp|mirror`; model-own textures keep
  the importer's settings): examples 01-03 change by up to 26/255 on the edges of the `tiles`
  faces only. `MS_InitRenderer()` (after `R3D_Init`) does `glEnable(GL_DEPTH_CLAMP)`; main.c,
  examples 01-03 and the export README use it (02 newly gets depth clamp).

---

## 11. Later additions (after the streams above; pointers only)

- **Memory rules and leak check** (stream M): no content limits, heap arrays via `MS_RESERVE` /
  `MS_PUSH`, sanity caps (§2.7); `tools/leakcheck.sh` with `tools/lsan.supp` /
  `tools/lsan_report.py` (`PROMPT-INSTRUCTIONS.md` §4).
- **Pass trimming** (stream P): R3D patch 10, single-sampled window by default (`--window-msaa`),
  `--bench-window`; trade-off knobs `--render-scale` / `--upscale` and the baked occlusion volume
  `--ao-volume` / `--ao baked` (R3D patch 11) (`PROMPT-INSTRUCTIONS.md` §7.3).
- **Graphics settings**: `MS_Quality`, `MS_QualityPreset`, `MS_ApplyQuality`,
  `MS_ApplyRenderResolution` in the runtime; the F2 panel and the settings file in the game
  (`settings.c`, `settingspanel.c`; `PROMPT-INSTRUCTIONS.md` §5 "Graphics settings").
- **Model tiers**: every request gets a fast / default / heavy model (`request.md` `Model:` line,
  `request.json` `model`; `launcher.c` "Model choice"; `MONOLITH_MODEL*`).
- **Closing request terminals**: K, Shift+K, Ctrl+K n; i3 / sway marks `_monolith_<id>`
  (`launcher.c` "Closing request windows").
- **Request coordinator** (2026-10-24): one coordinator session takes every request from a queue
  and hands it to a subagent (`docs/design-coordinator.md`, `AI-COORDINATOR.md`). The bundle
  format is unchanged (`request.json` stays format 2); new files next to it: `state` (queued,
  claimed, question, done, failed, cancelled), `claim.txt` (fingerprint at the claim), `cancel`
  (from the game), `question.md` (from the subagent). The K keys take finished requests off the
  list and cancel; Ctrl+Shift+K closes the coordinator. `MONOLITH_LAUNCH=per-request` keeps the
  old terminal per request.
- **Style-neutral docs** (stream G): the default map's brief, look and reasons moved from the
  project docs into `maps/monolith/MAP.md`.
