# Painting

Draw mode (**B**) lets the developer paint on any box or model in the editor without a request and
without leaving the game. This is the design the development rounds work from, written in the same
way as `docs/live-editing.md` (the working copy, live edits, undo, panels and verification flags that
painting builds on). Implementation notes follow at the end, one per stream as it merges.

Two rounds built it. The first (commit `3bc1bc4`) gave every painted object a copy of its material
with a canvas of its own and baked surface shaders into that canvas; its notes are at the end. The
second, the **paint layer** (§1–§9 below), replaces where paint lives and how it is drawn, and keeps
the first round's strokes, undo, saving, panel and verification flags (§7, §10–§14).

## 1. Why, and what stays the same

The developer wants to put marks, signs, decals and pixel art on the real map by hand, and the real
map is 97 % surface-shader boxes. In the first round a first stroke changed how the object looked
(the bake blurred it, froze its animation, lost its glow and specular; a copied flat colour shifted
the light nearby by 1–2 levels) and many surfaces were refused. Paint is now a **layer over the
material**: the material, its shader and its textures stay exactly as they are, and a last stage of
the material's fragment shader composites the paint over what the material computed.

What stays the same:

- **A map that nobody painted runs nothing new.** No new shader, texture, vertex data, instance
  attribute or draw. Every map in `maps/` gives pixel-identical `--shot`s, an identical
  `--dump-boxes` and the same bench results, in the editor and in the exported game.
- **A painted object looks pixel-identical wherever it isn't painted**, including before its first
  stroke: no resampling, no bake, no frozen animation, no lost emission, specular or view-dependent
  shading, no copied colour.
- **Painted objects stay in their material's draws.** No per-object materials, ranges or draw calls:
  merged box meshes and instance buffers carry which paint each face uses (§4.4).
- **Per-pixel work only where it is needed.** Materials without paint compile and draw as before.
  A painted material adds a vertex fetch and, per pixel, one test and one texture sample where the
  face has paint (§4.3).
- Model files on disk are never changed.
- Only **albedo** is painted (with the coat rule of §4.3). Roughness, metalness and emission
  painting is left for a later round.

## 2. Decisions

The developer decided these before the first round (they still hold):

1. **Pixel size means both controls.** The **brush size** in texels (1 = one texel, for pixel art)
   and the **density** in texels per metre, which sets how big a texel of new paint looks in the
   world (§5.2).
2. **Box faces: the developer says which faces share texture space.** Now a property of the paint
   itself: faces of one box in one group share one rectangle of the atlas (§3.2), set on the panel.

The developer decided these before the paint-layer round (2026-11-04):

3. The first round was committed as it stood (`3bc1bc4`, branch `paint-layer`); this round builds on
   it and history keeps the bake.
4. **Models whose UVs overlap or repeat** (Kenney kits with UVs up to ~30, mirrored halves) get a
   **generated paint unwrap** per model file when it is first painted, stored as a package sidecar
   (§4.5). The unwrap is made with xatlas (MIT), vendored with its licence, in the editor only.
5. **"No slower"** means: within measurement noise in the exported game, with one extra texture
   sample per pixel only on painted materials (§4.3, §15 phase 3).

Defaults chosen in phase 0 of the paint-layer round (each can change; the final summary lists them
again):

- **One paint atlas per material** that has paint: `textures/paint/<material>.png` (the material's
  name kept to letters, digits, `-`, `_`), RGBA8, straight alpha, transparent where unpainted, at most
  4096 × 4096 (§5).
- **Density** 64 texels/m (the panel's control, 8–512), now only the paint's own resolution; each
  side of a rectangle is clamped to 4..2048 texels (§5.2).
- **Pixel look** is per atlas: `paint_filter nearest` on the material (§4.1).
- **Compositing** (§4.3): the paint is a matte coat. Painted texels take the paint's colour over
  the material's albedo, and cover its emission and metalness in proportion to their alpha;
  roughness, normal, occlusion and specular stay the material's.
- **`transparency alpha`** materials: paint is opaque paint on the surface (alpha rises with the
  paint's alpha). **Cutout** materials (`alpha_cutoff`, `discard`): holes stay holes.
- **The eraser** is a true erase (alpha 0).
- **Shared mode goes** (and its key H): painting a material's own texture in place is no longer part
  of draw mode. Paint never changes a file the material reads.
- **`uv_faces`** stays a map key (first round) that the painter no longer writes; paint doesn't
  depend on it.
- Key **B**, the panel on the left, the pens, the eyedropper on **ALT+LMB**, one undo history with the
  map edits: as in the first round.

## 3. Where paint lands

### 3.1 The atlas of a material

Every material with paint names its **atlas**, a PNG in the package (`paint` key, §4.1). Every
painted surface drawn with the material has a **rectangle** in that atlas, in texels. Rectangles
never overlap, never move once allocated (the map text holds them), and keep a 2-texel transparent
border (§5.1).

A surface is painted **in the atlas of the material it is drawn with**: a box's material, or the
material a model slot is drawn with (its override `material.N` / `material` / `use`, or the model's
own material for that slot). A model's own material counts as a material of its own for this:
its atlas is named after the model file and slot (`textures/paint/<file>_s<N>.png`, §4.1).

### 3.2 Paint entries

A **paint entry** (one `paint` statement, §4.2) gives one object's surface a rectangle:

- **A box face, or a group of faces of one box.** Every box counts on its own: a `stairs` step, a
  `waffle` piece, one copy of a `repeat`. The entity is the box's `id N:i`. Faces in one entry
  share the rectangle and show the same paint, each in its own orientation (the face's u along its
  tangent, v up for side faces, as R3D's unit cube maps it). By default each face has an entry of its
  own; the panel's Faces section groups them (§13.2).
- **A model slot of one instance**: the slot's triangles in their **paint UV domain**, 0..1 (§4.5),
  mapped onto the rectangle.

An entry exists only for what was painted: the first stroke on a face (or a group, or a slot)
allocates its rectangle, nothing else of the object.

### 3.3 The first stroke

The first stroke on a surface without an entry is **one undoable edit** (`EDIT_PAINT_ADD`, §10.3):

1. The object's line gets an `id N` if it has none (as CTRL+I does), so the entry has a stable
   entity. Nothing is split out of a `repeat`, and the object's material is not touched: the entry
   names the box's index within the line (`N:i`).
2. The material gets its `paint "textures/paint/<name>.png"` key if it has none (and its atlas is
   made in memory, transparent).
3. A `paint` statement is appended to the file's paint block (§4.2) with the rectangle the packer
   chose (§5.1). If the atlas has no room it grows (§5.1).
4. The stroke paints into the atlas in memory. Nothing is written to disk before CTRL+S (§11).

A first stroke of alpha 0 therefore changes no pixel of the image (§15, the verification).

### 3.4 What can't be painted

- A model slot without texture coordinates **and** without an unwrap: an unwrap is made for it on the
  first stroke (§4.5), so this only refuses when the unwrap fails (degenerate geometry).
- A **skinned** model file (one with a skeleton): its vertices' bone channels carry the skinning,
  which the paint keys need (§4.4). No map in `maps/` has one.
- A material whose surface shader already declares **3 or more samplers**: R3D allows 4 per material
  and the paint stage needs 2 (§4.3). No shader in `maps/` uses more than 2.
- A surface shader whose comment-stripped source plus the paint stage exceeds R3D's 16 KB limit
  (§4.3). The largest in `maps/` is ~12 KB stripped.
- An atlas that is full at 4096 × 4096 (§5.1): "the paint atlas of x is full: lower the density".
- Lights, the sky, and textures of the environment.

Every refusal of the first round (surface-shader model slots, `transparency alpha` / `unlit` shader
materials, sampler textures without a licence row, face groups on a baked canvas, shared mode on a
flat colour) is gone.

## 4. Map format and drawing

All additions are opt-in. A map without them parses, meshes, compiles and draws exactly as now.

### 4.1 Material keys

```
material "concrete" shader "shaders/concrete.glsl" ... paint "textures/paint/concrete.png" paint_filter nearest
```

- `paint "<path>"`: the material's atlas, a package-relative PNG (RGBA8; any other format is converted
  on load). The material compiles with the paint stage (§4.3) only when at least one paint entry
  uses it; a `paint` key without entries changes nothing.
- `paint_filter trilinear | nearest`: how the atlas is sampled (`trilinear`, the default: mipmapped;
  `nearest`: nearest magnification and nearest-mipmap-linear minification, the panel's *Pixel look*).
- **A model's own material** has no line to put the key on. Its atlas is named by the entries
  themselves (§4.2, `own`) and is always `textures/paint/<model file name>_s<N>.png`, the file name kept
  to letters, digits, `-`, `_`. Its filter is `paint_filter` written on the entry.

### 4.2 The `paint` statement

```
# paint (draw mode)
paint "concrete" 120:0 +x 0 0 96 64
paint "concrete" 120:3 +y,-y 100 0 48 48
paint "crate" 40:0 slot 1 0 0 512 512
paint own 41:0 slot 0 0 0 256 256 paint_filter nearest
```

- `paint "<material>" <entity> <faces> <x> <y> <w> <h>` for a box: `<entity>` is `N:i` (the line's
  `id N`, the box's index within the line, as `select` writes it); `<faces>` a comma-separated list of
  `+x -x +y -y +z -z` (the box's own axes for a turned box), at least one; `x y w h` the rectangle
  in texels of the material's atlas.
- `paint "<material>" <entity> slot <N> <x> <y> <w> <h>` for a model instance: slot N of the model file
  drawn with `<material>` (its override). `paint own <entity> slot <N> …` when the slot is drawn with
  the model's own material, optionally followed by `paint_filter nearest`.
- **Rules** (parse errors with the line): the material exists and has a `paint` key; the line `N`
  exists; `i` is below its object count; the faces are valid and not repeated; no two entries of
  one material overlap (the 2-texel borders may touch); no face or slot of an entity is in two
  entries of the same material; `w`, `h` ≥ 1.
- **When the entity doesn't draw that face or slot with that material** (its line was edited, its
  material changed), the entry is kept but inactive: it draws nothing and the load logs one line
  ("paint: 120:0 +x is not drawn with concrete; inactive"). So editing geometry never breaks a map.
  A rectangle outside its atlas's size is the same (inactive, logged).
- The painter appends entries to a **paint block** at the end of the file, under the comment
  `# paint (draw mode)`, sorted by material, then entity, then face or slot. The developer may move
  them anywhere after the material's line.
- `paint` is a statement of the current map format (3).

### 4.3 The paint stage

Every material with at least one active entry draws with a **paint shader**, generated by the
runtime at load from the material's own surface shader (or, for a material without one, from an
empty one). The source R3D gets is:

1. the material's shader source with its comments stripped (so the paint stage fits R3D's 16 KB);
2. its `varying` declarations turned into `out` (under `STAGE_VERT`) / `in` declarations in place, so
   the user's functions can use them in global code;
3. its `void vertex()` and `void fragment()` renamed to `ms_userVertex()` / `ms_userFragment()`, each
   under `#ifdef STAGE_VERT` / `STAGE_FRAG` (R3D copies global code into both stages);
4. the paint stage, as below.

```glsl
uniform sampler2D ms_paint;        // the material's atlas (premultiplied alpha on the GPU)
uniform sampler2D ms_paintTable;   // RGBA32F, one texel per paint key: u0 v0 u1 v1 in the atlas
#ifdef STAGE_VERT
flat out vec4 ms_rect;             // the face's rectangle; all 0: no paint
out vec2 ms_uv;                    // 0..1 in the face or the slot's paint domain
void vertex() {
    ms_userVertex();
#if !defined(DEPTH) && !defined(DEPTH_CUBE)
    // which paint key and where in its domain (4.4); ms_rect = texelFetch(ms_paintTable, key)
#endif
}
#endif
#ifdef STAGE_FRAG
flat in vec4 ms_rect;
in vec2 ms_uv;
void fragment() {
    ms_userFragment();
#if !defined(DEPTH) && !defined(DEPTH_CUBE)
    if (ms_rect.z > ms_rect.x) {
        vec4 p = texture(ms_paint, mix(ms_rect.xy, ms_rect.zw, ms_uv));  // clamped half a texel inside
        ALBEDO = ALBEDO * (1.0 - p.a) + p.rgb;
        EMISSION *= 1.0 - p.a;
        METALNESS *= 1.0 - p.a;
        // transparency alpha: ALPHA += p.a * (1.0 - ALPHA); cutout materials: ALPHA unchanged
    }
#endif
}
#endif
```

- **Only painted materials** get it. A shader file used by painted and unpainted materials is compiled
  twice (the original and the paint version); the unpainted ones keep the original.
- **A material without a shader** gets the paint shader around an empty `vertex()` / `fragment()`. R3D
  runs the same template (`SceneFragment`'s automatic fetch, then `fragment()`), so outside the paint
  it computes the same values; phase 1 proves the pixels identical (alpha-0 entries on every kind of
  surface, §15). If they are not, the difference is reported (draw path, variant, sort order) before
  anything else.
- **Cost:** in the vertex stage, one `texelFetch` (not in the depth and shadow variants). In the
  fragment stage, one test of a flat varying, uniform over a face, and one bilinear or trilinear
  sample where the face has paint. Unpainted faces of a painted material pay the test only.
- **The depth pre-pass** is kept: the paint stage doesn't touch `ALPHA` on opaque materials and
  doesn't `discard`, and the pre-pass eligibility depends only on `alpha_cutoff`, depth and stencil
  state (R3D patch 6).
- **Sampling:** `ms_uv` is clamped half a texel inside the rectangle, so level 0 never reads the
  border. The atlas is uploaded premultiplied (filtering transparent texels adds no dark fringe); the
  PNG keeps straight alpha.

### 4.4 Paint keys in vertices and instances

A **paint key** is an index into the paint table (key 0: no paint). The runtime gives each active
entry's faces or slots keys at load (and on a live edit, §9) and writes them where each draw kind can
read them in its vertex stage, without new draws:

| Draw kind | Key | Domain coordinate `ms_uv` |
|---|---|---|
| **merged box meshes** (axis-aligned and turned) | per vertex: `boneIndices[0..3]` as a 32-bit little-endian key | per vertex: `boneWeights` as two unorm16 (u, v) = the face corner's 0 / 1 |
| **instanced boxes** (`instanced on`, a shader that needs it) | per instance: `INSTANCE_CUSTOM.x` = the first of 6 consecutive keys (faces +x −x +y −y +z −z; an unpainted face's key holds a zero rectangle), `INSTANCE_CUSTOM.y` = 1 | from the unit cube's model-space position and normal (the same mapping as R3D's cube UVs) |
| **model instances** | per instance: `INSTANCE_CUSTOM.x` = the first of the file's `materialCount` consecutive keys (one per slot), `.y` = 2; per vertex: `boneIndices[0]` = the slot | per vertex: `boneWeights` as two unorm16 = the paint UV (§4.5) |

- The vertex stage tells the kinds apart by `INSTANCE_CUSTOM.y` (0: a merged mesh, drawn with the
  runtime's one-instance buffer, which has no custom attribute: R3D sets it to 0).
- **Only what has paint carries keys:** the merged meshes of cells holding a painted box of a painted
  material (other boxes in them get key 0), the instance buffers of painted instanced materials (with
  `R3D_INSTANCE_CUSTOM`; other materials' buffers are unchanged), the instance buffers of model groups
  with a painted instance, and the meshes of painted model files (rebuilt in memory with slot and
  paint UV; the file on disk is untouched).
- The bone channels are free on every mesh the runtime makes and on every unskinned model (R3D
  binds them always and reads them only for skinned draws). Skinned files are refused (§3.4).
- **The paint table** is one RGBA32F texture per scene, 1024 texels wide, as many rows as needed,
  bound to every paint shader. Texel `key` holds the rectangle normalised to its atlas (u0 v0 u1 v1).
  It changes when entries or atlas sizes change (a small upload), never during a stroke.
- **Precision:** the face corners (0 and 1) and the keys are exact; a model's paint UV has 16 bits
  (1/16 texel of a 4096 atlas). The rectangle is float32. R3D's half-float texture coordinate is not
  used for paint.

### 4.5 Model paint UVs and the unwrap sidecar

A model slot's **paint domain** is 0..1 in each axis, mapped onto its rectangle:

- **Its own UVs**, when every texture coordinate of the slot's triangles is within 0..1 and no two of
  them overlap in UV space (a conservative raster test at 1024², §7.3). Most scanned and hand-unwrapped
  models.
- **Otherwise an unwrap**: tiled UVs (Kenney's 0..30), mirrored halves, overlapping trims, or no UVs.
  On the first stroke on such a slot the editor unwraps **the whole file** (each slot packed into its
  own 0..1 domain) with xatlas and stores it as `models/<file>.paintuv` beside the model. The runtime
  reads it in the editor and in the game; xatlas is in the editor only.

The sidecar is binary, little-endian:

```
"MSPAINTUV" 0x1A, uint32 version (1), uint32 meshCount, uint64 the model file's FNV-1a 64 (a stale
sidecar is ignored, logged), then per mesh: uint32 slot, uint32 vertexCount, uint32 indexCount,
vertexCount × { uint32 source vertex, uint16 u, uint16 v }, indexCount × uint32 index
```

The unwrap splits vertices at chart seams, so a painted file's mesh is rebuilt from it: each new
vertex is a copy of its source vertex (position, normal, tangent, colour, original UV) with the slot
and paint UV in its bone channels, and the triangles keep their order. The same triangles with the
same attributes rasterise to the same pixels. A file with an unwrap uses it for every slot; a file
without one uses its own UVs.

The sidecar is a package file: the export copies it, and it gets an `ASSETS.md` row and sidecar
derived from the model's ("paint unwrap of `models/x.glb`, generated in monolith", the model's licence
and attribution).

### 4.6 Compiled maps and the export

- `CmMaterial` gains `uint32_t paint` (the atlas path, `CM_NONE`) and `uint8_t paintFilter` (plus
  padding). A new section **paint** holds one `CmPaint` per entry, resolved at compile time from the
  entity to the object: `{ uint32 material (CM_NONE: a model's own), uint32 kind (0 box, 1 model),
  uint32 object (boxes[] or models[] index), uint32 faces (bits +x −x +y −y +z −z) or slot, uint32
  x, y, w, h, uint32 filter, int32 line }`. Inactive entries are written too (the runtime decides
  activity the same way). `CM_VERSION` 5; the reader then accepted 2–5 (since 2026-11-12 it reads only the current version: `docs/compiled-map.md`). Changes:
  `docs/compiled-map.md` and its version history.
- The export copies a package's dependencies as now; atlases and `.paintuv` sidecars are dependencies,
  so they go with it. The exported runtime generates the paint shaders and keys like the editor.
- The keys and the statement go into the header comment of `runtime/monolith_scene.c` and
  `docs/map-format.md` §2.

## 5. The atlas

The draw mode owns each atlas's pixels in memory (`drawmode.c`) while it paints; the runtime receives
them (§8.2).

### 5.1 Packing and growth

- A new atlas starts at the smallest power-of-two square, at least 256, that holds its first
  rectangle (the atlas's edges need no border: the sampler clamps there).
- Rectangles are placed **bottom-left on 4-texel cells** (so mip levels 1 and 2 stay inside each
  rectangle's border): the lowest row, then the leftmost cell, where the rectangle fits. Each
  rectangle owns the cells from its corner to 4 texels past its far edges (its 2-texel border and the
  next one's), so a placed rectangle starts on the grid and keeps a 2-texel transparent border on every
  side. The packer starts from every rectangle the map text holds in that atlas (active or not: they
  never move), whatever their position (a hand-written one off the grid is avoided conservatively).
- When a rectangle doesn't fit, the atlas **grows**: its width doubles when it is not wider than tall,
  else its height, up to 4096 × 4096. Growing keeps every texel where it is (the image is extended with
  transparent texels), so the text doesn't change; only the table (normalised rectangles) and the
  texture are rebuilt (the atlas is provided at its new size before the edit goes live).
- Full at 4096 × 4096: the stroke is refused (§3.4). Open question (§16): a second atlas per material.
- A new rectangle is cleared to transparent (with its border) where an earlier text left pixels; a
  rectangle no entry uses any more (clear, a face group that joined another) is cleared too.

### 5.2 Size of a rectangle

The **density** `ρ` (texels per metre, the panel's *Density*, default 64):

- **A box face group**: `w = ceil(ρ · a)`, `h = ceil(ρ · b)` for the largest face of the group (`a`
  along the face's u, `b` along its v), each clamped to 4..2048. A face smaller than the rectangle in
  one axis stretches over it. pine_forest's ground (a 572 m face) at 64 texels/m is clamped to 2048: 28
  cm texels, said on the panel; the ground itself is untouched (`uv_scale 286` keeps its look).
- **A model slot**: a square of `clamp(pow2ceil(ρ · sqrt(A)), 16, 2048)`, `A` the world surface area
  of the slot's triangles in this instance.
- The panel's *Resize to density* (§5.3) gives an entry a new rectangle.

### 5.3 Changing an entry

- **Faces** (the panel's face groups): one undoable step of `EDIT_PAINT_SET` / `EDIT_PAINT_ADD` ops
  (amended onto one history step) with the tiles they wrote. Each new group that has paint keeps the
  entry holding most of its faces (an entry kept by an earlier group doesn't count): `EDIT_PAINT_SET`
  gives it the group's faces, which leave the entries they were in (an entry left without faces goes,
  its rectangle is cleared). A group split from an entry another group kept gets a new rectangle of the
  same size with a copy of that entry's paint (`EDIT_PAINT_ADD`). A group whose faces have no paint
  stays without (its faces get rectangles with their first strokes).
- **Resize to density**: a new rectangle of the size §5.2 gives (packed without the entry's old one),
  the old paint resampled into it (nearest with `paint_filter nearest`, else bilinear with
  premultiplied alpha), the old rectangle cleared. One `EDIT_PAINT_SET` with the tiles.
- **Clear**: the panel's *Clear paint* removes the target's entry (`EDIT_PAINT_REMOVE`, all its faces)
  and clears its rectangle. A material's `paint` key and atlas file stay when its last entry goes (the
  atlas is transparent there).

## 6. Finding the texel under the cursor

`MS_RaycastSurface` (§8.1) gives the object, the material it is drawn with, the face or slot, and now
the **paint coordinate**:

- **A box (axis-aligned or turned)**: the face from the hit normal in the box's own frame; `paintUv`
  is the hit's position on the face, 0..1, oriented as R3D's unit cube maps that face (exactly the
  `ms_uv` the vertex stage gives the GPU, §4.4).
- **A model**: the triangle the BVH hit, its barycentric coordinates, and the triangle's paint UVs
  (own or unwrap, §4.5), interpolated. A slot that needs an unwrap that doesn't exist yet gives
  `paintUv` −1 −1 (the first stroke makes the unwrap, then casts again).
- `paintEntry`: the active entry of that face or slot in that material, or −1. The texel is the
  rectangle's `x + floor(u · w)`, `y + floor(v · h)` (row 0 at `v = 0`).

A model's own UVs can't overlap where they are used for paint (§4.5), so a stroke never shows up in
two places any more.

## 7. Strokes

### 7.1 The stroke model

As in the first round (its implementation notes): a stroke runs from LMB down to LMB up and is one
undo step; samples are subdivided in screen space; stamps are applied in texel space, clipped to the
hit's **region**. The region is now the entry's rectangle (a box face group), or the UV island of a
model triangle inside the slot's rectangle (§7.3). A stroke stays on the surface of its LMB down; on
another face of the same box it goes on in that face's entry (allocating it at once, which is the
first stroke of that face: its `EDIT_PAINT_ADD` is amended onto the stroke's step, so the stroke stays
one undo step, §10.2).

Colour: RGBA 0–255 from the panel, composited **source-over** in 8-bit sRGB bytes (as the PNG stores
them), alpha = colour alpha × the pen's coverage, with the per-stroke maximum-coverage mask of the
first round.

### 7.2 Pens

As in the first round, except:

| Pen | Change |
|---|---|
| eraser | **true erase**: lowers the texel's alpha by the pen's coverage (`a · (1 − coverage)`), colour kept for an anti-aliased edge; at full coverage the texel becomes 0 0 0 0 |
| fill | fills the region: the entry's rectangle, or the UV island |

### 7.3 UV islands (models)

As in the first round, in **paint UV** space: triangles joined over shared paint-UV edges, rasterised
into an island mask at the rectangle's size. With own UVs that don't overlap and with an unwrap, no
texel is shared between islands, so a texel belongs to at most one island; texels outside every island
(the slot's empty paint space) are never painted. The same raster at 1024² decides whether own UVs
can be used (§4.5).

### 7.4 The eyedropper

**ALT+LMB** samples what the surface shows under the paint: the paint texel when its alpha is 255;
otherwise the material's G-buffer albedo at the cursor (paint composited), so a procedural shader's
colour can be picked too: the view is rendered once more into a texture of the window's size in R3D's
`R3D_OUTPUT_ALBEDO` mode (the scene's own lights and geometry, `MS_PushLights` + `MS_DrawGeometry`), the
pixel read back and turned from linear to sRGB bytes. One extra scene render per click.

## 8. The runtime API (editor builds only)

Declared in `runtime/monolith_scene.h`, in the `MONOLITH_EDITOR` block "Painting". The game build and
`--export` don't have them (`./build.sh test export_strip`). The game build reads and draws paint
(§4) without any of these functions.

### 8.1 Surface picking

`MS_SurfaceHit` keeps the first round's fields and gains:

```c
Vector2 paintUv;        // 0..1 in the face (boxes) or the slot's paint domain (models, 4.5);
                        // -1 -1: a model slot that needs an unwrap that doesn't exist yet
int paintEntry;         // MS_PaintEntryAt index of the active entry for this face / slot and
                        // material, -1: none yet
```

`MS_ModelSlotPaintTriangle(scene, file, slot, k, Vector2 puv[3], Vector3 pos[3])` gives a slot
triangle's paint UVs, like `MS_ModelSlotTriangle` gives its texture coordinates.

### 8.2 Entries and atlases

```c
typedef struct {
    int material;           // materials[] index; -1: the model's own material (atlas named by file + slot)
    int line;               // map line of the paint statement
    MS_EntityId entity;
    MS_ObjRef obj;          // the box or model instance it resolved to (kind MS_OBJ_NONE: unresolved)
    unsigned char faces;    // boxes: bit f for each face +x -x +y -y +z -z; models: 0
    int slot;               // models: the slot; boxes: -1
    int x, y, w, h;         // texels of the atlas
    bool active;            // drawn (4.2: the entity draws that face / slot with that material and
                            // the rectangle is inside the atlas)
} MS_PaintEntry;

int MS_PaintEntryCount(const MS_Scene *scene);
const MS_PaintEntry *MS_PaintEntryAt(const MS_Scene *scene, int i);

typedef struct {
    const char *rel;        // package path of the atlas PNG (NULL: the material has no paint key)
    int width, height;      // of the loaded or provided image (0: none yet)
    bool nearest;           // paint_filter nearest
} MS_PaintAtlas;
// The atlas of material (-1 with file and slot: a model's own material)
bool MS_PaintAtlasOf(const MS_Scene *scene, int material, int file, int slot, MS_PaintAtlas *out);

// Straight-alpha RGBA8 pixels for an atlas path that the next MS_EditSetText (and any upload from
// memory) uses instead of the file, also when the file doesn't exist yet (a new or grown atlas).
// Same lifetime as the first round's MS_TextureProvide (kept across live edits, dropped by a load
// from disk and by MS_Unload). NULL forgets it.
bool MS_PaintAtlasProvide(MS_Scene *scene, const char *rel, const Image *img);

// A stroke: MS_TextureUpdate(scene, rel, img, rect) as in the first round, on the atlas path; for an
// atlas it premultiplies the rectangle on upload (the image stays straight alpha).

// Model paint UVs (4.5)
typedef enum {
    MS_PAINTUV_OWN,         // the slot's own UVs are used
    MS_PAINTUV_UNWRAP,      // the file's .paintuv sidecar (or a provided unwrap) is used
    MS_PAINTUV_NEEDS_UNWRAP,// own UVs overlap, repeat or are missing, and there is no unwrap yet
    MS_PAINTUV_SKINNED,     // refused: a skinned file
} MS_PaintUvKind;
MS_PaintUvKind MS_ModelPaintUv(const MS_Scene *scene, int file, int slot);

// An unwrap of a whole model file, the sidecar's contents (4.5): per mesh of the file in R3D's order
typedef struct {
    int slot, vertexCount, indexCount;
    unsigned int *source;   // per new vertex: its vertex in the mesh as loaded
    unsigned short *uv;     // per new vertex: u, v (unorm16)
    unsigned int *indices;
} MS_PaintUnwrapMesh;
typedef struct {
    int meshCount;
    MS_PaintUnwrapMesh *meshes;
    unsigned long long fileHash;    // FNV-1a 64 of the model file
} MS_PaintUnwrap;
// The editor's unwrap of model file `file`, used by the next MS_EditSetText as if read from the
// sidecar (provided like an atlas; NULL forgets it). Copied.
bool MS_PaintUnwrapProvide(MS_Scene *scene, int file, const MS_PaintUnwrap *u);
// The sidecar file: written by the editor on save, read by the runtime at load
bool MS_PaintUnwrapWrite(const char *path, const MS_PaintUnwrap *u);
bool MS_PaintUnwrapRead(const char *path, MS_PaintUnwrap *out);   // MS_PaintUnwrapFree it
void MS_PaintUnwrapFree(MS_PaintUnwrap *u);
// The mesh data of model file `file` as loaded (positions, UVs, indices; for the unwrap). False when
// out of range or not loaded.
bool MS_ModelMeshData(const MS_Scene *scene, int file, int mesh, R3D_MeshData *out);    // not owned
```

Phase 2 removed what only the first round's code used: `MS_TextureProvide` (now
`MS_PaintAtlasProvide` itself), `MS_TextureFind`, `MS_ModelSlotTexture`, `MS_TextureRead`,
`MS_BoxDrawOf`, `MS_MaterialFaceLayout` and `MS_FaceCellRect`.

### 8.3 Hot reload

As in the first round: a dependency that doesn't exist yet (a new atlas or sidecar before its first
save) is watched with mtime 0, and a save that writes PNGs and sidecars first, then the map, then calls
`MS_EditMarkSaved`, doesn't reload.

## 9. Live updates

### 9.1 A stroke

Only pixels change: the dirty rectangle of each atlas goes to `MS_TextureUpdate` once per frame
(premultiplied on upload, mipmaps regenerated). No text, no `scene->version` bump, no table or mesh
change. `MS_TextureGeneration` + 1, which frame reuse keys on (`FrameKey`).

### 9.2 A paint edit (first stroke, faces, resize, clear, undo of those)

A text edit whose only differences are `paint` statements and `paint` / `paint_filter` keys is a
**cheap live edit**: it must not re-upload the scene. Phase 1 adds it to `LiveMaterialPlan`:

- `paint` statements are not part of any material's look hash; the parser keeps a hash of them.
- A material whose only change is its `paint` / `paint_filter` keys, or whose set of active entries
  went from none to some (or back), is rebuilt as a "changed" material (kind 1 of the first round's
  plan): its shader alias becomes the paint shader (compiled once per shader file and load, then
  aliased) or the original again, its atlas loads (from the provided image).
- The paint table is rebuilt and uploaded. The merged meshes of the cells holding a box whose keys
  changed are rebuilt (through `LiveRetire`), the instance buffers of the materials and model groups
  whose keys changed are refilled (gaining or losing `R3D_INSTANCE_CUSTOM`), and a model file whose
  meshes gain or lose paint keys is rebuilt in memory. Nothing else changes.
- `MS_EditStats` reports what was rebuilt; a first stroke on a material that already has paint should
  cost one table upload and one cell mesh or instance buffer.

### 9.3 What a stroke must not do

As in the first round: no allocation per frame, scratch grown to the largest size seen, tiles
allocated on LMB down and when first touched.

## 10. Undo

### 10.1 Tiles

As in the first round, on the atlas: **64 × 64 texel tiles**, copied into the step the first time a
stroke touches them, swapped on undo and redo.

### 10.2 One history

The draw mode shares the edit mode's `MapEditor`: one history across text edits and strokes,
CTRL+Z / CTRL+SHIFT+Z (CTRL+Y) in both modes, `EditStep.paint` and `MapEditPaintHooks` as in the
first round.

- A stroke on surfaces that have entries: a step with no hunks and `paint` set.
- A stroke that allocates (the first stroke on a face, a group or a slot, §3.3; a stroke that goes on
  onto a new face, §7.1): **one** step with the text hunks (the `paint` statements, the material's
  `paint` key, the id) **and** `paint` (the tiles it touched, and the atlas's old size when it grew).
  Undo removes the text live; an atlas that the step created leaves `MS_PaintAtlasProvide` (it stays
  in memory for redo). **Undoing the first stroke on a map gives back its bytes exactly, and no atlas
  is written** (nothing is on disk before a save).
- Face groups, resize and clear (§5.3): one step with the hunks and the tiles of the rectangles they
  wrote.
- The 512 MiB cap on paint in the history, as in the first round.

### 10.3 Edit ops (`mapedit.c`)

`EDIT_PAINT_MATERIAL` (the first round's copy of a material) and its script syntax are gone. New:

```c
EDIT_PAINT_ADD,     // the selected object (one) gets a paint statement (4.2) in the file's paint block
                    // (appended at the end under `# paint (draw mode)` when there is none; sorted by
                    // material (named, then own), entity, face / slot): paint.material (NULL: the
                    // model's own), paint.faces or paint.slot, paint.rect; the material gets
                    // `paint "<paint.atlas>"` (with paint.nearest: and `paint_filter nearest`) when it
                    // has no paint key (the built-in "default" without a line of its own gets a
                    // `material "default" paint ...` line at the top of the block); the line gets an
                    // id when it has none (nothing is split out of a repeat)
EDIT_PAINT_SET,     // the selected object's entry in paint.material with the rectangle paint.rect, else
                    // the one holding the first face of paint.faces (or paint.slot), gets paint.faces
                    // and paint.rect (rect w <= 0: kept); an own entry takes paint.filter (-1 kept,
                    // 0 trilinear, 1 nearest). Several objects selected: each one's entry, the
                    // rectangle kept (the pixel look of an own atlas)
EDIT_PAINT_REMOVE,  // the selected object's entries in paint.material lose the faces of paint.faces
                    // (an entry left without faces goes) or the entry of paint.slot goes; faces 0 and
                    // slot -1: every entry of the object in that material
EDIT_MATERIAL_SET,  // as in the first round (paint_filter goes through it)
```

ADD and SET take their faces away from the object's other entries in the same material (an entry left
without faces goes), so each op keeps the rules of §4.2 by itself and a face that joins a group leaves
its old one. `EditOp.paint` is as specified plus `signed char filter`; `EditStep` keeps its own copies of
`paint.material` / `paint.atlas` (`paintMaterial`, `paintAtlas`) and a list of ops amended onto it
(`more[]`, `EditMore`): `MapEditAmendPaint(E, sel, n, op, msg)` applies one more paint op live onto the
top step (it must be an unsaved paint step the working copy shows), turns the step's hunks into the
one hunk from the text before it to the text after both (undo gives the bytes back exactly), and a
rebase applies the step's op and then each amended op on the objects it selected.

Edit-script syntax (`--apply-edits` and `--edit-script`), on the selection (one object):

```
paint-add "<material>"|own ["<atlas>"] faces <+x,-x,...>|slot <N> <x> <y> <w> <h> [nearest]
paint-set "<material>"|own faces <+x,...>|slot <N> [<x> <y> <w> <h>] [nearest|trilinear]
paint-remove "<material>"|own [faces <+x,...>|slot <N>]
material-set "<name>" <keys...>
```

(`own` takes no atlas: its path follows from the file.) A rebase re-applies them like any op; the
selection is resolved by entity id. An `EDIT_PAINT_ADD` whose rectangle now overlaps one that appeared
on disk fails with the parser's message ("paint: the rectangle ... overlaps line N's ... in the atlas
of concrete"); the stroke's tiles go with it to `failed[]`. With `--apply-edits` (no working copy, the
file is written and loaded from disk) the atlas a `paint` key names must exist as a file: the loader's
path check is strict for files loaded from disk (open question, §16).

## 11. Saving

### 11.1 CTRL+S and auto save

CTRL+S (in draw or edit mode) writes:

1. every atlas with unsaved paint to its PNG (RGBA8 straight alpha; `textures/paint/` made when
   missing), at the atlas's current size;
2. every unwrap made in this session to its `.paintuv` sidecar (§4.5);
3. the working text (`MapEditSave`);
4. the licence records of new files (§11.2);
5. then `MS_EditMarkSaved`.

Unsaved state, auto save (`edit_save = auto`) and the HUD's count as in the first round.

### 11.2 Licences (`ASSETS.md`, sidecars)

- **A paint atlas** is "made in monolith (painted by hand)", licence **CC0 1.0**, author the package
  (the developer can change it): one row and one `.asset.md` sidecar per atlas, written on the save
  that first writes it. The material's own assets are untouched: paint never derives from them.
- **A `.paintuv` sidecar** derives from its model: "paint unwrap of `models/x.glb`, generated in
  monolith", the model's row's author, licence and attribution. A model without an `ASSETS.md` row
  is refused at the first stroke that would make its unwrap ("models/x.glb has no licence row").

### 11.3 Closing the game with unsaved paint

As in the first round, with atlases and sidecars under `<settings folder>/unsaved/<map>-<time>/`.

## 12. Conflicts

As in the first round, per atlas instead of per canvas: on a reload with unsaved paint, an atlas whose
PNG is unchanged on disk is provided again; one that changed on disk gets its unsaved strokes replayed
(each step keeps its samples in paint-UV space and its entry); a step whose entry or material is gone
fails into `failed[]`. A request that edits a painted object's line keeps its `id`, so its entries
stay; a request that removes the object leaves its entries inactive (§4.2).

## 13. Draw mode

### 13.1 Keys

As in the first round, except: **H** (shared mode) is gone; ALT+LMB samples what the surface shows
(§7.4).

### 13.2 The panel (`src/drawpanel.c`)

As in the first round (its implementation notes give the layout), except:

1. **Target**: what the cursor rests on and where its paint goes: "box line 120 #3, face +z: concrete
   atlas 1024×512, 96×64 at 0 0", "model barrel.glb slot 1: crate atlas, a new 512² rectangle", "the
   first stroke adds 96×64 to concrete's atlas", or the refusal in `UI_ERROR` (§3.4). No shared switch.
2. **Pen**, 3. **Colour**: unchanged.
4. **Density** (was *Canvas*): texels / m, the texel size the target's paint has (or will have; "the
   rectangle is clamped to 2048" when it is), *Resize to density* (§5.3), *Pixel look* (the atlas's
   `paint_filter`: `EDIT_MATERIAL_SET` on the material, or every `paint own` entry of an own atlas), and
   *Clear paint* (§5.3).
5. **Faces** (boxes): the six faces, each with a group number button (0..5); faces with the same
   number share one rectangle. *Apply* rewrites the target box's entries (§5.3); *All separate* and
   *Sides together* set the buttons.

### 13.3 The brush cursor

As in the first round, in paint-UV space: the footprint mapped through the face (exact) or the
triangles of the island (affine). Fill outlines the entry's faces or the island.

## 14. Verification flags

As in the first round (`--paint-script`, `--draw-mode`, `--draw-panel`, `--input-script`), except:

- `shared` is gone; `faces <a b c d e f>` sets group numbers and applies them (§5.3); `clear`;
  `resize`; `pixel on|off` (these on the surface of the last stroke's LMB down);
  `stroke uv` / `dab uv` take **paint** coordinates (a box face 0..5 and its face uv, or a model slot
  and its paint uv); `print` lists the atlases (size, rectangles, unsaved state, pixel hashes) and the
  entries.
- Every flag stays windowless with `--shot` / `--dump-*` / `--apply-edits` and refused otherwise;
  scripted input never reaches the prompt box (`drawShotLocked`, the launcher guard).

The examples' test harness gains a **headless bench mode** for the exported runtime (§15, phase 3):

```
<example> --bench <report.md> [--bench-frames N] [--note "what changed"]
```

It opens the same hidden window as `--shot`, and for each viewpoint of the scene spins the camera as
`monolith --bench` does for `N` frames (default 600) with `glFinish` per frame, then writes a report
with the same statistics as `--bench` (per viewpoint and overall: mean, median, p95, p99 frame time,
frames) and the machine's *Setup* table (GPU, driver, CPU, system, resolution, the note). It refuses
to run without `--bench` or `--shot`, like the test build always does.

## 15. Phases and ownership (the paint-layer round)

| Phase | Who | Files | Done when |
|---|---|---|---|
| 0 | lead | this note §1–§16; headers and stubs (`monolith_scene.h` and its stubs, `mapedit.h`, `harness.h`'s interface) | both builds 0 warnings with the stubs |
| 1 | agent A | `runtime/monolith_scene.c/.h`, `docs/compiled-map.md`, `src/export.c` if needed | §4, §6, §8, §9; every map unchanged (shots, dumps); a scratch package with an alpha-0 entry on a box of each draw kind (merged, instanced, turned, a `stairs` step) and model slots (own material, shader override, tiled UVs with an unwrap, `transparency alpha`, `unlit`, emissive): **0 changed pixels** against the unpainted package at every view; a known rectangle painted: changes only inside its mask (light bounced by the paint reported separately); compiled round trip; `./build.sh examples test`; leak check |
| 3a | agent B (beside A) | `tests/examples/harness.h` bench mode; `src/paintunwrap.c/.h` (xatlas, vendored under `src/vendor/xatlas/` with its licence), `build.sh` | the harness bench on every exported map; base numbers for every map in `maps/` (benches through the lead); an unwrap of each Kenney model in `maps/` written and read back (`MS_PaintUnwrapRead`), no overlapping charts |
| 2 | agent C (after A) | `src/drawmode.c/.h`, `src/drawpanel.c/.h`, `src/mapedit.c/.h`, the draw-mode wiring in `src/main.c`; `src/surfacebake.c/.h` to the trash | §3, §5, §7, §10–§14; `--paint-script` shots of every pen on every kind of phase 1, including the monolith map's concrete, weeds, tower, porcelain, poster and ivy, and pine_forest's ground; alpha-0 first stroke: 0 pixels; undo restores the map bytes and leaves no atlas; minimal `--apply-edits` diff; save → reload identical; leak check, `--asan` |
| 3b | agent D (after C) | benches through the lead; a fix if needed | painted monolith (N = 10, 100, 1000 objects over its materials) against the same map unpainted, exported, interleaved pairs; draw counts; per-pass GPU table |
| 4 | verifier, then lead | the docs (§15 of the task prompt) | every claim re-checked |

`monolith_scene.c` belongs to A only; `drawmode.c` and `mapedit.c` to C only. Only one benchmark at a
time, through the lead.

## 16. Open questions (for the final summary)

- **A full atlas** (4096 × 4096) refuses; a second atlas per material (a `paint` key per page and a
  page per entry) would lift it.
- **Brushes across box edges**: a stamp is clipped to its face (first round).
- **Non-square faces** stretch over a rectangle sized by the group's largest face (§5.2).
- **The eyedropper's read-back** (§7.4) of the G-buffer albedo.
- **Paint on emissive and metal surfaces** covers emission and metalness (§4.3); keeping them is
  one line of the paint stage.
- **Mip bleeding** between rectangles from level 3 on (the 2-texel border on a 4-texel grid keeps
  levels 0–2 clean).
- **Licence of an atlas**: CC0 by default (§11.2).
- **`--apply-edits` and a new atlas**: a `paint` key loaded from disk must name an existing file, so an
  edit script's `paint-add` needs the atlas PNG in the package first (a lenient check would let the entry
  load inactive, "has no atlas image").
- **The unwrap of a large model** blocks the frame after its note (3.2 s for `rock_face_01.glb`): a
  worker thread would need copies of the mesh data.
- **The eyedropper** renders the view once more per click (R3D's albedo output mode).

## Implementation notes

(Appended per stream as it merges. The paint-layer round's notes come first as they merge; the first
round's notes below describe the canvas model and the bake that the paint layer replaces. Phase 4 moves
what is only history into the `project-history` skill.)

### Implementation notes: paint layer phase 1 (the runtime)

What phase 1 built in `runtime/monolith_scene.c/.h`, and where it differs from §1–§9.

- **Parsing** (§4.1, §4.2): `paint` and `paint_filter` in `ParseMaterial` (`MatDef.paint`, `paintFilter`); the
  `paint` statement in `ParsePaint` (stored with its entity, no statement key, hashed into
  `SceneData.paintHash` only); the entities are resolved at the end of the parse (`ResolvePaintEntities`:
  the line `id N` through the statement key set, its boxes / models by binary search on the statement
  index). The rules between entries are one function, `PaintConflict` (a sweep over the rectangles sorted
  by atlas and x, and over the entries sorted by material, kind and object), used by the parser and the
  compiled reader alike. **Interpretation**: "no two entries of one material overlap (the 2-texel borders
  may touch)" is checked as *no texel shared*; the border is the packer's rule, not the parser's.
- **Activity** (`PaintResolve`, after `BuildEntities` and the model files, in every load and edit): box
  material = entry material; for a model: slot < the file's `materialCount`, `SlotMaterial` = the entry's
  material (`-1` own), slot ≤ 255 (a byte in the paint mesh), the file not skinned, the slot's paint UVs
  usable (unwrap or own UVs that pass the test below), the rectangle inside the atlas (its size from the
  provided image or the PNG header, `ImageFileSize`, so `--compile` and `--dump-*` decide the same way
  without decoding). An inactive entry is logged once per load: `map:line: paint: 120:0 +x is not drawn with
  concrete; inactive` (also "lies outside its atlas", "has no atlas image", "is in a skinned model file",
  "needs a paint unwrap", "…'s shader can't take the paint stage"). A material whose shader can't take the
  stage (more than 2 samplers, too long, or a compile error) is demoted with its entries (`PaintDemote`).
- **Own UVs** (§4.5, `OwnUvKind`): from the file's CPU mesh data, so the game build decides like the editor:
  every coordinate in 0..1 and not all 0, then a 1024² raster in which a texel touched (conservatively,
  separating axes) by two UV islands (triangles joined over exactly equal UV edges), or whose centre lies
  strictly inside two triangles (folded or mirrored halves joined at a seam), fails. Found: every Kenney
  model in `maps/` and both scans (rock_face_01: folded triangles; stump_old) need an unwrap; only
  `sedan.glb` passes. Computed once per file and slot (`FilePaint.uvKind`), carried over by live edits.
- **CPU mesh data**: kept (not freed after the pick BVH) for every model file in editor builds
  (`MS_ModelMeshData`, unwraps, the own-UV test) and, in game builds, for files a paint entry names.
- **Unwrap sidecar** (§4.5): `models/<file>.paintuv`, the model path with its extension replaced (so
  `models/barrel.glb` → `models/barrel.paintuv`); read by `UnwrapRead` (game and editor), checked against
  the model file's FNV-1a 64 and its meshes (`UnwrapMatches`: a mesh per mesh, the mesh's slot, the same
  triangle count, sources inside the mesh); stale or not fitting: ignored, logged. A sidecar that exists is a
  dependency (watched, exported). `MS_PaintUnwrapProvide` keeps a copy per model *path* with a generation;
  dropped like provided textures (a load from disk, `MS_Unload`).
- **Own atlases** (§4.1): `textures/paint/<file>_s<N>.png`, `<file>` the model file's name without folder and
  extension, kept to letters, digits, `-`, `_` (`OwnAtlasPath`). `MS_PaintAtlasOf(-1, file, slot)` names it
  also before any entry. A missing own atlas makes its entries inactive (no error: there is no path token
  to cite); it is a dependency when it exists or is provided.
- **The paint shader** (§4.3, `PaintShaderSource`): comments stripped (a block comment becomes a space),
  leading blanks and empty lines dropped; `[flat|smooth|noperspective] varying T x;` becomes
  `#ifdef STAGE_VERT … out … #else … in … #endif` in place; `vertex()` / `fragment()` become
  `ms_userVertex()` / `ms_userFragment()` under their stage guard; a source with only one entry point has its
  whole code under that stage's guard (R3D gives a stage without its entry no user code, so its global code
  never compiled in the other stage before). **R3D's parser skips any token starting with `flat `,
  `smooth `, `noperspective `** (it copies global code with `hasVaryings` true), so interpolation qualifiers
  are written as `MS_INTERP(flat)` with `#define MS_INTERP(q) q`. The stage then follows as in §4.3 with two
  changes: the fragment test excludes only `DEPTH` / `DEPTH_CUBE` — **`GEOMETRY_PREPASS` is also the shading
  pass after the depth pre-pass** (R3D patch 6: `uDepthOnly` returns early in the depth-only run), so
  excluding it hid the paint of every `alpha_cutoff 0` material — and `ALPHA += …` runs under `FORWARD`
  (the blended program) only, which gives the transparency / cutout rules for every material of one shader
  file with one compile. The uv is clamped half a texel inside with `textureSize(ms_paint, 0)`.
  Compiled once per shader file and load (`ShaderFile.paint`, the empty shader is the entry with `rel` NULL);
  originals are now compiled lazily too (`GetShaderFile`), so a file only painted materials use is not
  compiled twice. Generated sizes: tower_ruin 14 118 bytes (raw 16 260), poster_var 12 178, concrete 9 429,
  the empty shader 1 719. All 19 shaders of `maps/` and `tests/models` pass `glslangValidator` in every
  variant (vertex, vertex DEPTH, fragment, DEPTH, DEPTH_CUBE, FORWARD, GEOMETRY + GEOMETRY_PREPASS, UNLIT)
  with STAGE_* and the vertex attributes defined (a scratch variant of `check_surface_shader.py`: the stock
  one defines neither, so it can't check generated sources).
- **Keys** (§4.4, `PaintKeys`): 6 per painted box, `materialCount` per painted model instance, from 1 up, in
  box then instance order; a face or slot without an entry keeps a zero rectangle. **A live edit keeps the
  keys of objects matched to old ones** (`boxFrom` / `modelFrom`) and appends new keys after old's, so a
  stroke on a new face rebuilds one cell, not every later one; a full load packs them again. Merged meshes:
  the key's 3 bytes in bone indices 0..2 (index 3 is 0), the corner UV as two unorm16 in the four weights
  (`PaintBones`, in `BuildBoxMesh` for axis-aligned and turned boxes). Instanced boxes:
  `R3D_INSTANCE_CUSTOM` (FLOAT32) `(first key, 1, 0, 0)`; the shader computes the face and UV from the unit
  cube's position and normal, checked once against `R3D_GenMeshDataCube` (`PaintCheckCube`: UVs and zero
  bone data). Model groups: the custom attribute `(first key or 0, 2, 0, 0)` on **every group that draws a
  slot with a paint shader** (also groups of unpainted files, whose bone bytes then don't matter); paint
  meshes per painted file (`BuildPaintMeshes`: copies of the CPU mesh data, or the unwrap's vertices, with
  slot and paint UV in the bone channels; groups draw them instead of the file's meshes).
- **Table** (§4.4): RGBA32F, 1024 keys per row, `rlLoadTexture` with nearest filtering, bound as
  `ms_paintTable` to every paint alias (`PaintBindTable`). A live edit takes old's texture when the table is
  byte-identical, else uploads a new one.
- **Atlas upload** (§4.3): `GetTextureEx(premul)`: RGBA8, colour premultiplied by alpha **in linear light and
  stored sRGB-encoded** (`PremulLut`, a 64 KB table: 255 keeps the byte, 0 gives 0), sRGB texture, clamp,
  trilinear (R3D's mipmaps) or `paint_filter nearest` (nearest / nearest-mipmap-linear). The GPU decodes
  sRGB before filtering, so the filtered value is colour × alpha in linear light, which the stage adds.
  `MS_TextureUpdate` premultiplies the uploaded rectangle the same way for atlas cache entries
  (`TexEntry.premul`); the provided image stays straight alpha.
- **Live edits** (§9.2): `matPaintlessHash` (the material's lines without the paint keys) next to the look
  and shape hashes: a change only in `paint` / `paint_filter` is no `PLAN_MAPS`. The plan is now bits
  (`PLAN_MAPS`, `PLAN_NEW`, `PLAN_PAINT`); `PLAN_PAINT` when `PaintSigSame` differs (painted or not, atlas
  path, size, filter): the material gets a new alias (`BuildMaterialShader`), old's is freed on success.
  **Draw order**: R3D sorts instanced opaque draws by the alias's creation serial (patch 17), so a re-made
  alias would draw after all others and coplanar faces of two materials could resolve differently than in a
  load (measured: 248 pixels at ground contact). Every later material with a shader is therefore re-aliased
  too (`materialsRebuilt` counts them). Merged cells are kept when their boxes' keys are the same
  (`BoxPaintKey`), instanced buffers when their boxes and keys are (`instanceBuffersRebuilt`), paint meshes
  when the file's unwrap generation is (`PaintMeshesLive`; replaced ones go to `LiveRetire`). A failed
  edit rebinds old's aliases to old's table (`LiveMaterialsBack`). New `MS_EditStats` fields:
  `instanceBuffersRebuilt`, `paintTables`, `paintMeshFiles`.
- **Picking** (§6): boxes: `paintUv` = the face UV (`CubeFaceUv`, the stage's mapping); models: the
  triangle's paint UVs (own UVs quantised to unorm16 as the paint mesh holds them, or the unwrap's)
  interpolated with the hit's barycentric coordinates; `paintEntry` by a linear search of the active entries.
- **Compiled maps** (§4.6): as specified (`CmMaterial` 164 bytes, `CmPaint` 40 bytes, section 18, version 5,
  versions 2–5 read). The reader re-checks the parser's rules (`PaintConflict`, an explicit `id` on the
  object's line, a material with an atlas). `tests/unit/compiled_test.c`: `MAT_SIZE` 164 and five new cases.
- **Not done / open**: the transform preview draws its moved boxes from a buffer without the custom attribute
  (their paint is hidden while the preview runs; merged ranges rebuilt for the preview keep theirs); a model
  file with more than 256 material slots can't paint slots past 255; `MS_EditStats.materialsRebuilt` now also
  counts re-aliased materials (see *Draw order*).
- **Verification** (scratch package: merged, turned, `stairs` step, instanced, stripes-shader, concrete and
  tower_ruin boxes; transparent, unlit and emissive boxes and model slots; own material with own UVs
  (sedan) and with an unwrap (rock scan); a Kenney tree and the rock scan with synthetic sidecars; 20
  entries): every map in `maps/`, `tests/models`, `tests/rotated`: identical dumps and 0 changed pixels in
  61 shots (viewpoints, yaw sweeps, heights, the steep view, `--reload-test`) against the build before.
  Zero-alpha atlases vs no paint: 0 pixels at 11 views. An opaque rectangle: with SSAO, SSIL and bloom off,
  879 254 changed pixels inside the paint footprint and 478 outside it (1-pixel dilation) over 10 views; with
  every effect on, the indirect light changes 1.96 M pixels outside it. Live edits (a first paint on a new
  material, an entry on an already keyed box, an entry removed, back to the start) vs a load of the same
  text: 0 pixels; a stroke (`MS_TextureUpdate` of every atlas) vs loading those atlases: 0; an atlas grown
  through `MS_PaintAtlasProvide`: 0. Compiled round trip, `./build.sh test compiled`, `./build.sh test parse_error`,
  `./build.sh test export_strip` pass; the export renders identically with the examples' game runtime.

### Implementation notes: paint layer phase 3a (harness bench, unwrap)

**The harness bench** (`tests/examples/harness.h`, §14). `<example> --bench <report.md>
[--bench-frames N] [--note …]` in any test build (`./build.sh examples test`) or an export's `viewer.c`
compiled with `-include <repo>/tests/examples/harness.h`. It mirrors `monolith --bench --headless`
(`src/main.c`'s bench loop, `src/bench.c`), whose code can't be included (it needs the editor's
`common.h`, the global `scene`, the spectator): the hidden window is always 1920 × 1006 (titled
`monolith-bench`; the program's own size request and its `MS_ApplyRenderResolution` calls are replaced
by that size), 60 warmup frames per viewpoint (monolith: 59 on the first one), then N timed frames
(default 600, 10 … 1 000 000) with the camera at the viewpoint's eye, monolith's yaw and pitch towards
its target and fovy 70, the yaw turning 2π · frame / N; `glFinish` after every `EndDrawing`, a frame's
time from one `glFinish` to the next. The camera is replaced in the first scene's `MS_Draw` calls, so
programs that draw with `R3D_Begin` + `MS_DrawGeometry` (`02_existing_project/after`, `05_entities`)
can't be benched: after 600 frames without an `MS_Draw` the run stops with exit 1. The report has
monolith's *Setup* rows (GPU, driver, CPU, system, resolution plus R3D's, map, sun shadow, effects,
vsync, window, method, command, the note; R3D build and quality become one *Program* row) and its
statistics per viewpoint and overall (avg ms, avg FPS, 1 % low FPS, worst ms, shadow updates) plus
the median, p95 and p99 the phase-0 interface asked for; the same table goes to stdout. No per-pass
GPU table: the harness never enables R3D's profiler (monolith's bench does, so absolute numbers of
the two tools differ; compare harness runs only with harness runs). `--shot` behaves as before: every
example's shots (`01_minimal` ×3 incl. `--quality medium`, `02` before / after, `03`, the import
example and its capture build (both removed since), `05`) are byte-identical to the phase-0 harness. `--shot` with `--bench`, and
`--bench-frames` / `--note` without `--bench`, are refused (exit 1).

**`build/tools/bench-export-ab`** `<base export> <new export> --note "…" [--map <name>] [--pairs N ≥ 3]
[--frames N] [--out <dir>] [--dry-run]`, `--summarize <out dir>`: compiles each export's `viewer.c`
with the harness (its own object; the runtime is compiled without it) against that export's
`monolith_scene.c/.h` and its libraries (`prebuilt/linux/lib`, else `build/r3d-linux/lib`), runs
A, B, A, B, … on each export's own package with `__GL_SYNC_TO_VBLANK=0 vblank_mode=0`, a note per run
naming its side and pair, and prints per pair A, B, B − A (ms, %), then the mean and median of the
paired differences overall and per viewpoint. Reports and logs in `benchmark/export-ab/<stamp>/`
(`A-<i>.md`, `B-<i>.md`, `runs.txt`, `summary.txt`). Linux only. Checked with `--dry-run` (both
binaries build with 0 warnings) and `--summarize` on copies of the one screening report (the parser);
it has not run a real A/B (the lead's benches).

The one screening run (exported `pine_forest`, `--bench-frames 150`, RTX 4050 Laptop GPU, driver
615.71.09, Ryzen 7 7435HS, Linux 7.2.5-cachyos; the GPU was shared with another agent's shots: not a
measurement): all views 3.267 ms avg (306 FPS), per view 2.83–4.11 ms, 0 shadow updates in timed frames.

**The paint unwrap** (`src/paintunwrap.c/.h`, §4.5). `PaintUnwrapMeshes(meshes, count, texelsHint,
out)` over plain arrays (positions float3, UVs float2 or NULL, indices or NULL for
unindexed, the slot) → `MS_PaintUnwrap`; `PaintUnwrapModelFile(scene, file, texelsHint, out)` (false: the reason in `MS_LastError()`, the
runtime's last error, which the editor's modules share; the signature of the task, without its error buffer; texelsHint `PAINTUNWRAP_TEXELS` = 1024) reads the meshes through
`MS_ModelMeshData` (as the header declares it: `R3D_MeshData *out`, not §8.2's `const R3D_MeshData
**`), converts R3D's float16 UVs, takes the slots from `model.meshMaterials`, refuses skinned files and
sets `fileHash` (FNV-1a 64 of the file). Per slot, in the order slots first appear, one xatlas atlas
holds all meshes of the slot: `ComputeCharts` with default options (the own UVs only as a seam hint),
`PackCharts` with padding 2 and bilinear borders into one texelsHint² domain (xatlas packs a fixed
resolution into as many atlases as needed, so the texels per unit start at xatlas's own estimate and
shrink by 0.9 until one atlas holds every chart). Paint UVs are the packed UVs / texelsHint as unorm16;
triangles keep their order and count (xatlas writes each output triangle at its source position; the
test proves `source[indices[3t + k]]` = the input's index `3t + k` for every corner of every model).
**Folds:** xatlas leaves a few overlapping triangles inside LSCM charts on dense meshes
(`rock_face_01.glb`: 43 texel centres of 1024² covered twice; `campfire.glb`: 2, from a triangle with
zero area in 3D). The unwrap checks its own result exactly (integer separating axes on the unorm16
triangles, a 128² grid) and gives every overlapping triangle an xatlas material group of its own (a
chart of its own) for another pass, up to 8 passes (both needed one more); triangles with zero area in
3D (they draw no pixel) and coincident duplicates (same three positions) are exempt. xatlas runs
single-threaded (`XA_MULTITHREADED=0`): two runs give the same bytes.

**xatlas** is vendored at `f700c779` (MIT, `src/vendor/xatlas/README.md`: URL, commit, sha256 of
`xatlas.cpp`, `xatlas.h`, `xatlas_c.h`, `LICENSE`); downloaded into an empty scratch folder, read
(includes, file I/O: only its debug writers, compiled out; no network, no process calls), copied
unchanged. The build (`nob.c`, run by `./build.sh`) compiles it once per build folder (`Xatlas`: C++14 — C++11 fails on its
single-threaded path — `-w -DNDEBUG -DXATLAS_C_API=1 -DXA_MULTITHREADED=0`, the build's own optimisation
and sanitizer flags, `-fno-sanitize=function` for UBSan) and links the object into the editor only;
the export and examples don't get it. Both targets 0 warnings; Linux NEEDED unchanged (libgcc_s, libGL,
libm, libc, ld-linux), newest GLIBC symbol 2.34; Windows imports unchanged (system DLLs and UCRT only).

**The test** (`tests/unit/paintunwrap_test.c`, ASan + UBSan; also in `build/tools/leakcheck` as
`paintunwrap`): synthetic meshes (a cube whose six faces share UVs 0..1, the same without UVs, a cube
of 8 shared vertices, a mirrored quad pair with overlapping UVs, five meshes over four slots with an
empty mesh, an unindexed one, tiled UVs and a degenerate triangle, three kinds of bad input) and every
model file in `maps/*/models/`, read with R3D's importer exactly as the runtime does
(`R3D_LoadModelEx(…, R3D_IMPORT_RETAIN_MESH_DATA)` in a hidden window). Per input: triangle order and
count, valid sources and indices, an exact texel-centre raster at 1024² (top-left rule) with 0 texels
covered twice (coincident faces counted apart), chart footprints (conservative raster at texelsHint) at
least 2 empty texels apart, and two runs byte-identical. The sidecar round trip (`MS_PaintUnwrapWrite`
→ `MS_PaintUnwrapRead`) and `PaintUnwrapModelFile` against the direct unwrap (`--scene` on the package
with the most models) run automatically once phase 1 replaces the stubs; until then they print
"skipped". Results (`-O2`, times of one unwrap; all: overlap 0, footprints 0, gaps > 2 texels,
deterministic):

| Input | Meshes / slots | Triangles | Vertices in → out | Charts | Time |
|---|---|---:|---:|---:|---:|
| `monolith/models/sedan.glb` | 5 / 1 | 2032 | 2718 → 2780 | 120 | 201 ms |
| `pine_forest/models/campfire.glb` | 4 / 3 | 641 | 1048 → 1098 | 192 | 130 ms |
| `lantern.glb` | 1 / 1 | 264 | 338 → 364 | 62 | 170 ms |
| `log.glb` | 2 / 2 | 200 | 312 → 312 | 64 | 61 ms |
| `rock_face_01.glb` (not Kenney; scanned) | 1 / 1 | 10000 | 5803 → 9578 | 777 | 3188 ms |
| `rock_largeA.glb` | 2 / 2 | 80 | 146 → 146 | 23 | 125 ms |
| `rock_largeD.glb` | 3 / 3 | 80 | 146 → 146 | 22 | 132 ms |
| `stump_old.glb` | 1 / 1 | 120 | 208 → 212 | 42 | 33 ms |
| `tent.glb` | 2 / 2 | 352 | 518 → 586 | 82 | 140 ms |
| `tree_pineDefaultA.glb` | 2 / 2 | 230 | 392 → 392 | 51 | 159 ms |
| `tree_pineRoundC.glb` | 2 / 2 | 206 | 342 → 342 | 64 | 156 ms |
| `tree_pineSmallA.glb` | 2 / 2 | 164 | 280 → 280 | 53 | 125 ms |
| `tree_pineTallA.glb` / `B` | 2 / 2 | 78 | 136 → 136 | 32 | 131 / 143 ms |
| `tree_pineTallC.glb` / `D` | 2 / 2 | 98 | 156 → 156 | 33 | 149 / 159 ms |

Open: most of a small model's time is the repacking (up to 80 fixed-resolution packs; a bisection
of the texels per unit would need ~8); `rock_face_01` takes 3 s, which a first stroke would wait for
(a worker thread, or a progress note, in phase 2). The 2-texel gap holds at texelsHint (1024); a
smaller paint rectangle scales it down (at 256 texels the charts are only ~0.5 texel apart), so draw
mode should pass the slot's real rectangle size, or the unwrap be made at the smallest size it will
be painted at.

### Implementation notes: paint layer phase 2 (draw mode)

What phase 2 built (`src/drawmode.c/.h`, `src/drawpanel.c/.h`, `src/mapedit.c/.h`, `src/paintunwrap.c/.h`,
the draw-mode wiring in `src/main.c`, `src/keyspanel.c`, `build/tools/leakcheck`, a removal in the runtime),
and where it differs from §1–§14 (which now say what was settled).

- **The bake and the canvases are gone**: `src/surfacebake.c/.h` (to the trash), shared mode and H, the
  per-object material copies, `EDIT_PAINT_MATERIAL` and `paint-material`. Removed from the runtime because
  nothing else used them (examples, tools and tests grepped): `MS_TextureProvide` (its body is now
  `MS_PaintAtlasProvide`), `MS_TextureFind`, `MS_ModelSlotTexture`, `MS_TextureRead`, `MS_BoxDrawOf` /
  `MS_BoxDraw`, `MS_MaterialFaceLayout`, `MS_FaceCellRect`. No other runtime change; every map in `maps/`,
  `tests/models` and `tests/rotated` stays pixel- and box-identical.
- **Atlases** (`Atlas` in `drawmode.h`): one per atlas path, opened from its PNG when it exists (straight
  alpha RGBA8, the pixels it was read with are its saved state), else empty until its first rectangle.
  Always provided (`MS_PaintAtlasProvide`) while it holds unsaved or session pixels; a frame's stamps go
  up with one `MS_TextureUpdate` per atlas (raylib's log quietened around it). A material's new atlas is
  `textures/paint/<material>.png` (letters, digits, `-`, `_`), `-2`, `-3`, ... when another material's key,
  another file's own atlas, a file on disk or another material's atlas of this session has the name (an
  atlas of the same material from an undone stroke is reused). Own atlases take the runtime's name
  (`MS_PaintAtlasOf(-1, file, slot)`).
- **Allocation** (`AddRect`): the size of §5.2 (`ceil(rho * edge)` with a 1e-4 slack, so 2 m at 64/m is
  128, not 129), the packer of §5.1 (an occupancy grid of 4-texel cells and its summed-area table, built
  on the click: at most 1024² cells), growth, then the op: `EDIT_PAINT_ADD` pushed with the stroke's step
  (`MapEditPushPaint`) or amended onto it (`MapEditAmendPaint`, a stroke that went on onto another face).
  The atlas is provided at its new size before the text goes live. A failed push puts the atlas back and
  releases the step, whose pixels go back too (`PaintStep.pending`). **Deviation**: a stroke onto a new
  face allocates at once (the text goes live mid-stroke, so the paint shows), and the step stays one undo.
- **Steps** (`PaintStep`): the atlas, the 64 × 64 tiles, the atlas's size before / after and whether the
  step gave the runtime the atlas (`created`). Undo swaps the tiles at the larger size, crops back to the
  size before, and takes a created atlas out (`MS_PaintAtlasProvide(NULL)`; it stays in memory for redo);
  redo extends, swaps and provides. Panel edits (faces, resize, clear) are `STEP_EDIT` steps: tiles, no
  samples.
- **Strokes** (§7): samples in atlas texels with a region (the entry's rectangle; a model: the island of
  the hit triangle, from `IslandsFor`: the slot's triangles joined over equal paint-UV edges, rasterised
  conservatively at the rectangle's size, cached per file, slot, size and scene version); stamps clip to
  the region. A stroke stays on the object, material (and slot) of its LMB down; on a box it goes on over
  any face. The eraser lowers alpha (`a · (1 − coverage)`, 0 0 0 0 at 0). Fill fills the region.
- **Unwraps** (§4.5): `EnsureUnwrap` before the first stroke on a slot whose `MS_ModelPaintUv` is
  `NEEDS_UNWRAP`: refused without the model's `ASSETS.md` row, then `PaintUnwrapModelFile(scene, file,
  texelsHint, …)` (the new parameter: the slot's rectangle side) and `MS_PaintUnwrapProvide`; the
  allocation's live edit makes the paint UVs exist, so the stroke's samples read them from the triangle
  (`MS_ModelSlotPaintTriangle` with the hit's barycentrics). Interactively the click only says
  "unwrapping x.glb for paint ...", the HUD shows it for a frame, and the next `DrawModeUpdate` unwraps
  (synchronously) and begins the stroke where LMB went down (a released button: one dab). No worker
  thread: `rock_face_01.glb` freezes the game for 3.2 s on that frame (machine A). A provided unwrap stays
  for the session (undo doesn't take it back: the file looks the same either way); its sidecar is written
  only while the text paints an instance of the file. `--paint-script`'s `uv` on such a slot unwraps first
  and makes a live edit of the same text so its paint UVs exist before the first hit.
- **Refusals** (`Resolve`, before the click; the panel and the HUD say them): lights and the sky; a
  skinned file; a slot above 255; a material whose shader declares 3 or more `uniform sampler…` or whose
  source without comments, leading blanks and empty lines plus 1 800 bytes passes 16 384 (a heuristic of
  the runtime's own test, read from the shader file once per material and scene version); a face or slot
  whose entry the text has but the load made inactive ("its paint entry (map line N) is inactive"); a
  model without a licence row when it needs an unwrap; a full atlas.
- **The eyedropper** (§7.4) as specified: the G-buffer albedo is feasible from the app side, through
  R3D's output mode (no R3D change). Measured: on `tests/models`' ground (`albedo 118 112 100`) it picks
  `#767063`.
- **Saving** (§11): atlases (live and unsaved), then the unwrap sidecars, then the map, then the atlas
  rows; `MS_EditMarkSaved` when files were written and the map on disk is the working copy. Rows: an atlas
  `` | `textures/paint/x.png` | made in monolith (painted by hand) | <package> | CC0 1.0, https://… | <date> | - | none | ``;
  a sidecar `` | `models/x.paintuv` | paint unwrap of `models/x.glb`, generated in monolith | <the model's
  author> | <the model's licence cell> | <date> | - | made from the model by monolith's draw mode (xatlas) | ``;
  each with an `.asset.md` sidecar.
- **Rebase** (§12): as the first round, per atlas; after the sync, when any atlas is still provided, one
  live edit of the same text loads the atlases from memory (the scene was loaded from disk before they were
  provided again, and a step of paint alone changes no text: an atlas replayed onto a PNG of another size
  stayed inactive without it), then their pixels go up. A panel edit can't be replayed onto an atlas that
  changed on disk: it fails into `failed[]`.
- **Panel** (§13.2): Target (no Shared switch), Pen, Colour, **Density** (texel size, *Resize to density*,
  *Pixel look*, *Clear paint*), Faces (the target box's groups from its entries, `DrawTarget.groups`;
  `facesKey` follows the box and material). `--draw-panel density` (and the old `canvas`). HUD: `DRAW
  round 4 px #C83232 a255 | ground's atlas 2048×2048, face +y | 2 unsaved edits`, before a first stroke
  `face +z: a new 96×64 in concrete's atlas` (`(unwrap first)`). The keys panel lost H.
- **`--paint-script`** (§14): `faces`, `resize`, `clear`, `pixel` act on the surface of the last stroke's
  LMB down (the panel's target in the game); `uv` coordinates are paint coordinates; `print` lists atlases
  (size, owner, unsaved, pixel hash, refs, provided), entries (line, material, entity, faces / slot,
  rectangle, active) and unwraps.
- **Leak check**: `paint-bake*`, `draw-panel-canvas` and shared mode are gone; new or rewritten:
  `paint-script` (every pen, first strokes on a textured, a plain and a shader box, a Kenney rock with its
  unwrap, faces, resize, pixel look, the eyedropper both ways, clear, save, poll; a stroke on the sky fails
  on purpose), `paint-rebase`, `paint-apply`, `paint-ops` (`paint-add` / `set` / `remove` in an edit script,
  undo; a bad face fails on purpose), `paint-rot` (turned merged and instanced boxes in paint coordinates),
  `draw-mode-input` (no H), `draw-unwrap-input` (a click on a Kenney tree: the note, then the unwrap and the
  stroke a frame later), `draw-panel-paint`.

**Verification** (machine A: RTX 4050 Laptop, driver 615.71.09, Ryzen 7 7435HS; scripts and images in the
session's scratch folder `agentC/`):

1. **Unpainted**: every map in `maps/` (monolith, pine_forest, test1), `tests/models`, `tests/rotated`
   against the base build (paint-layer + phases 1 and 3a): `--dump-boxes` identical (5 of 5), 0 changed
   pixels at every viewpoint and three yaws (34 of 34 shot pairs).
2. **Every pen on every kind** (a scratch package `kinds`: merged, instanced, turned boxes, a `stairs` step,
   a shader box, `transparency alpha`, `unlit`, emissive boxes; a Kenney tree with its own materials (unwrap),
   the Kenney rock with a shader override, `transparency alpha`, `unlit` and emissive overrides, the sedan
   with its own material and own UVs, `rock_face_01.glb` with its own material (unwrap, 3.2 s), the
   ground): fill, round, square, soft and eraser on each of the 17 surfaces (85 strokes); the live shot
   equals the saved package loaded fresh (0 pixels). On copies of `maps/monolith`: concrete (line 360,
   +x), a weed tuft (33551, top), the tower ruin ledge (488, a repeat copy), porcelain (668, top), the poster
   (378, +z, cutout) and the ivy (8000, instanced, cutout, `cull none`: paint shows only on the leaves); each
   look stays everywhere it isn't painted (before / after pairs in `agentC/mono/mono-before-after.png`), and
   pine_forest's ground (2048² clamped, 29.3 cm texels, the panel says so; `agentC/pine-before-after.png`).
3. **Alpha-0 first stroke**: on the 17 surfaces of `kinds` (one stroke each, every one allocating): 0 changed
   pixels against no stroke, live and after save + reload at 3 viewpoints; on the six monolith surfaces and
   pine_forest's ground: 0 pixels live and saved.
4. **Undo of first strokes + save** (`tests/models`: a box with a shader and a Kenney model slot, both
   allocating, both undone): the map's bytes identical, no `textures/paint/`, no sidecar, `ASSETS.md`
   identical; the shot equals the unpainted one (0 pixels). A stroke over the awning's edge (+y onto +z) is
   one step: one undo removes both entries.
5. **`--apply-edits`** of the seven `paint-add` ops a live script made (the atlases copied in first) gives
   the same bytes as the live `--paint-script` with `save`. The minimal diff (`tests/models`): each painted
   material gains ` paint "textures/paint/<material>.png"` at the end of its line, each painted object's
   line gains `id N `, and the file gains `# paint (draw mode)` and seven sorted `paint` lines.
6. **Save → reload**: live painted shots against the saved copies loaded fresh: 0 pixels (`tests/models`,
   `kinds` with every pen, the six monolith surfaces, pine_forest's ground).
7. `./build.sh`, `./build.sh --win`, `./build.sh examples` and `./build.sh examples test`: 0 warnings;
   `build/tools/leakcheck` and `build/tools/leakcheck --asan`: `leakcheck: OK: zero leaks of ours` (ASan: no
   sanitizer error); `./build.sh test compiled`, `./build.sh test parse_error`, `./build.sh test export_strip`,
   `./build.sh test paintunwrap` (with `PaintUnwrapModelFile`'s new parameter) pass.

**Open**: `--apply-edits` needs the atlas file of a new `paint` key (the loader's strict path check for a
load from disk); the unwrap of a large scan blocks a frame for seconds (a worker thread would need copies
of the mesh data); a material line for the built-in `"default"` goes into the paint block; the shader
refusal is a heuristic (the runtime demotes what it can't compile, logged); a split face group copies the
paint at its old size; a panel edit isn't replayed onto an atlas changed on disk.

### Implementation notes: the draw order fix (lead, after the phase 2 verification)

The phase 2 verifier found two image changes outside any paint, both at depth ties where two
surfaces touch: an alpha-0 entry on a Kenney tree's bark changed 157 pixels where trunks (painted
and unpainted) meet pine_forest's ground, and after four live strokes one monolith view differed by
11 pixels from a fresh load of the saved text (weed / ground). Cause: R3D sorts draws by the custom
shader's creation serial (patch 17; 0 without a shader) before textures, and the first draw wins at
equal depth. A material without a shader that draws with the paint version of the empty shader moved
behind every material without one; a live edit that makes an alias again gave it a later serial than
a fresh load. Fix: **R3D patch 27** (`R3D_SetSurfaceShaderSortKey`, `submodules/r3d/patches.md`) and
`ShaderSortKey` in the runtime: every scene material's alias gets the key `material index + 1`, the
paint alias of a material without a shader (and of a model slot's own material) the key 0, so the draw
order is the unpainted load's whatever is painted and however the aliases were made. With the patched
R3D a live paint edit no longer makes the later materials' aliases again (phase 1's workaround stays
for builds without `MS_PATCHED_R3D`). Verified: both repros 0 pixels; every map in `maps/`,
`tests/models`, `tests/rotated` against the round's base: dumps identical, 49 shot pairs 0 pixels;
alpha-0 entries on every kind (the verifiers' packages, monolith's six shader surfaces, pine_forest's
ground and a Kenney tree): 0 pixels; leak check and `--asan` clean.

## Implementation notes of the first round (canvases and the shader bake, replaced)

### Implementation notes: phase 1 (the runtime)

What phase 1 built, and where it differs from the sections above.

- **Keys** (§4): `uv_faces` and `filter` are parsed in `ParseMaterial`; the uv_scale / uv_offset
  rule is checked at the end of every `material` line, so it holds in whichever order and on
  whichever line the keys come ("uv_faces needs uv_scale 1 1 and uv_offset 0 0", at the line that
  breaks it). A layout with a gap (`0 0 3 3 3 3`) fails with "uv_faces: the cell numbers must use
  every number from 0 to the largest one". `MatDef` holds `hasFaces`, `uvFaces[6]`, `filter`.
- **Layouts** live in the region "Small helpers: box face layouts" (outside `MONOLITH_EDITOR`, the
  game build meshes with them): `FaceCellsValid`, `FaceGrid`, `FaceCellRect` (what
  `MS_FaceCellRect` returns), `FaceOfNormal`, `FaceLayoutData`. The face index comes from each cube
  face's normal, not from a table of R3D's face order (+Z −Z +X −X +Y −Y), so a change of that
  order can't swap cells.
- **Meshes** (§9.3): one `FaceCube` per distinct layout per load (`SceneData.faceCubes`, built by
  `BuildFaceCubes` after the materials, freed by `FreeGpu`): R3D's unit cube data with the UVs moved
  into the cells, and a mesh of it. `BuildBoxMesh` (and so `TurnedFaces` and the preview's
  `PreviewRebuildRanges`) takes the material's cube data through `MaterialCube`; instanced draws and
  `PreviewDraw` draw its mesh; the preview's turned-box meshes use it too. A material without the key
  gets R3D's cube data and `p->cube` exactly as before (one extra branch per instanced range draw).
  A live edit carries the face cubes over when the layouts are unchanged (see "Recycled vertex
  ranges" below).
- **UV precision**: R3D stores texture coordinates as half floats, so a cell corner is quantised
  (at most 1/4096 of the texture near u = 1, half a texel of a 2048 canvas). `MS_RaycastSurface`
  quantises the corners the same way and interpolates between them (`FaceCellUv`), so its `uv` is
  the one the GPU interpolates, not the unquantised rectangle.
- **`filter nearest`**: the texture is loaded trilinear (R3D makes the mipmaps), then
  `GL_TEXTURE_MAG_FILTER = NEAREST` and `GL_TEXTURE_MIN_FILTER = NEAREST_MIPMAP_LINEAR` through
  `rlTextureParameters`. Samplers (`sampler`, `data_sampler`) ignore the key. `TexEntry.filter` is
  part of the cache key.
- **Compiled maps** (§4.3): `CmMaterial` ends with `uint8_t uvFaces[6], filter, pad` (156 bytes),
  `CM_VERSION` 4. Versions 2 and 3 have 148-byte material records: the section table's record size
  is checked per version (`CmRecordSize`), `CmRecord` copies the file's record size and zeroes the
  rest, and an old material reads as no layout (255) and trilinear. The reader checks the layout
  with the parser's `FaceCellsValid` plus the uv_scale / uv_offset rule, `filter <= 1`, `pad == 0`.
  `tests/unit/compiled_test.c` had material offsets of 148 bytes hard-coded: they are `MAT_SIZE`
  now, with six new targeted cases (a gap, cell 6, a partial layout, filter 2, padding 1, a layout
  on `uv_scale 2 2`) and one that must load.
- **Picking** (§6, §8.1): `MS_RaycastModel` became a wrapper of `RaycastModelTri`, which also returns
  the triangle and the barycentric coordinates; `MS_RaycastSurface` calls `MS_Raycast` and then, for
  a model, `RaycastModelTri` on the instance it hit (the nearest triangle of that model). Deviation:
  `ModelPick` gains, in editor builds, `uv` (per vertex; NULL when every texture coordinate of the
  file is 0, which is what R3D's importer leaves without UVs), `triSrc` (per triangle in BVH order:
  its index in file order; `BvhBuild` swaps it with `tri`), `triAt` (the inverse, for
  `MS_ModelSlotTriangle`) and `meshTri` (the first triangle of each mesh), instead of a per-triangle
  `unsigned short *triMesh`: the mesh comes from a binary search in `meshTri`, and the triangle in
  the mesh from `triSrc`. The model's own uv_scale / uv_offset (`material` -1) are the file's
  `R3D_Material` of the slot. The game build's `ModelPick` and picking are unchanged.
- **`--pick` / `--pick-at`** print one more line, `pick: surface material <i> "<name>" face <f>
  slot <s> mesh <m> triangle <t> faceUv <u v> uv <u v> bary <a b c>` (`src/main.c`,
  `PrintSurfaceHit`). Note: picking hits both faces of a triangle (as before), so a slot drawn with
  back-face culling is also hit from behind, where nothing is drawn.
- **Provided textures** (§8.2): a registry in `MS_Private` (`ProvidedTex`: package path, RGBA8
  copy). `GetTexture` uses it in editor builds when the scene is being built by `MS_EditSetText`
  (`SceneData.live`), and the parser's `PackagePath` accepts a provided path that has no file yet
  (only then: a load from disk is strict). It is dropped by a successful load or reload from disk
  (`LoadBytes`) and by `MS_Unload`. `MS_TextureUpdate` also copies the rectangle into the provided
  image of that path, so a later upload from memory shows what the texture shows.
- **`MS_TextureUpdate`**: packs the rectangle's rows into a scratch buffer of `MS_Private` (grown to
  the largest rectangle; RGBA8, RGB8, grey and grey + alpha textures; others are skipped), uploads
  it with `rlUpdateTexture` into every cache entry of the path and regenerates their mipmaps
  (`rlGenTextureMipmaps`; not for `data_sampler` textures, which have none). It invalidates the sun
  tiles, the camera-fitted map and the point / spot shadows that the union of the bounds of each
  material's objects (boxes and model instances that draw a slot with it) reaches, for the materials
  whose `albedo_map` is the path and that have `alpha_cutoff > 0` or a transparency mode. Note:
  R3D's default `alpha_cutoff` is 0.5, so that is nearly every material: a stroke redraws the tile
  under the object on the next frame. Open question: `rlGenTextureMipmaps` logs one INFO line per
  call ("Mipmaps generated automatically"), so a stroke logs a line per frame; quieting it needs the
  editor to raise raylib's log level around strokes or a GL call the runtime doesn't have.
- **Live materials** (§9.4): the parser records `restLookHash` (format and env lines),
  `matLookHash[]` (every `material` line of a material, keyword and name included) and
  `matShapeHash[]` (the same without its `uv_faces`, `filter` and `albedo_map` keys).
  `LiveMaterialPlan` gives each material of the new text 0 (the same: old's GPU state moves over), 1
  (only those three keys changed: old's material and shader alias, its four textures again through
  `GetTexture`, its merged meshes rebuilt, its boxes and the models that use it dirty) or 2
  (appended: built with `BuildMaterial`, after old's, so the shader alias order is a load's); any
  other difference is a full upload as before. Both kinds can come in one edit. `MS_EditStats`
  gains `materialsRebuilt`. A failed edit gives back what it added to old's state
  (`LiveMaterialsBack`: the new shader aliases and shader files, the textures past old's count, and
  the texture list, which may have moved while it grew). Textures a changed material no longer uses
  stay in the cache until the next load or full upload.
- **The gutter** (§4.1) keeps samples inside the cell only for the first few mipmap levels: it is
  cell / 64 texels wide (2 texels at a 128 px cell, 4 at 256), which a level halves, so from about
  level log2(cell / 64) + 1 on the filter reaches into the next cell. The verifier measured it: at
  30–60 m a few edge pixels of a face pick up the neighbouring cell's colour. §4.1's "mipmaps don't
  bleed" holds for those first levels only. The gutter is unchanged (a wider one costs paint area).
- **Recycled vertex ranges** (fix after the verification). With several live edits before a frame
  (an edit script), a vertex range of R3D's shared buffer that one edit filled, the next freed and a
  third filled again was sometimes (30–70 % of runs) drawn with its first contents: a turned box
  drawn as a face cube (a unit cube at the origin), a duplicated instanced box missing. It needed
  `uv_faces` only because every live edit freed and re-allocated the face cubes; the box meshes do
  the same, and the older nondeterministic sun shadow after some live edits (about half the runs of
  the verifier's repro, shadow only) had the same cause. No CPU-side stale reference exists: the
  draw lists (offsets, buffers) were identical in good and bad runs, and a GPU read-back at the end
  of each edit made both go away, so the stale contents are on the GPU side (seen with NVIDIA
  615.71). Two changes:
  - `LiveRetire`: the box meshes and face cubes a live edit replaces are not freed at once; they go
    to `MS_Private.retired` and the next `MS_DrawGeometry` frees them, so a range is handed out
    again only after a frame has drawn what replaced it. (Out of memory: freed at once, as before.)
  - `LiveFaceCubes`: when the new text's materials have the same layouts in the same order, old's
    `faceCubes[]` move over (`LiveBorrow.faceCubes`, like the other material GPU state); only a
    change of the layouts builds new ones. Edits no longer churn them.
  The verifier's repro: 0 of 20; its scripts on every view: 0 pixels; the shadow repro: 0 of 20
  (from 4 of 8).
- **Hot reload** (§8.3): nothing in `MS_Poll` needed to change. A dependency that doesn't exist is
  watched with mtime 0 and stays unchanged; a PNG written and then `MS_EditMarkSaved` is unchanged
  too (both checked by the scratch test below). What did need a change was the parser's existence
  check for a provided path (above).
- **Tests** (scratch, not in the repo): a copy of `tests/models` with a 3 × 2 test-pattern texture,
  a merged, a turned and an instanced `uv_faces 0 1 2 3 4 5` box, a `0 0 1 1 2 2` box, a plain
  textured box, and `rock_face_01.glb` with an 8 × 8 checker override (`filter nearest`, and
  `uv_scale 2 2 uv_offset 0.25 0`): a grid of `--pick-at` points per viewpoint, each surface uv's
  texel against the shot's pixel; a harness program (`-include tests/examples/harness.h`) for the
  appended / changed / failed live paths, `MS_TextureProvide` / `MS_TextureUpdate` and `MS_Poll`,
  each shot against a fresh load of the same text and PNG (0 pixels), also under ASan + LSan.

### Phase 3a: the widgets and the panel's layout

- **Widgets** (`ui.c`): `UiSlider`, `UiNumberField`, `UiTextField`, `UiSwatch`, `UiButton`,
  `UiWidgetsBusy` as in §13.2, plus `UiRoundRect` (a rounded rectangle or disc of any radius from
  the atlas; the pen icons) and `UiWidgetsBegin(pass, mouse, clip)` / `UiWidgetsEnd`. The panel
  reads its input in `DrawPanelUpdate` (before the draw mode) and draws later with the HUD, so it
  lays itself out twice: an input pass (`UI_WIDGETS_INPUT`: nothing drawn) and a draw pass
  (`UI_WIDGETS_DRAW`: no input; it draws a copy of the `DrawMode`). Hover only counts inside the
  clip rectangle, so widgets scrolled out of the panel take nothing. Without `UiWidgetsBegin` a
  widget does both in one call (plain immediate mode).
- **`UiEdit`** gained `claim` (a field clicked while another is open: that one applies on the same
  click, this one opens the next frame, whatever order they are called in) and `all` (the whole
  text is selected when a field opens: typing replaces it). Fields take ASCII only; LEFT / RIGHT /
  HOME / END / BACKSPACE / DELETE / CTRL+A work. Clicks act on LMB press, as in the settings panel.
  Button and field text is `UI_M`, `UI_S` when it doesn't fit.
- **Layout** (300 px wide at x 6, from the HUD's last line + 6 px): title row, then Target (two
  lines, or the refusal wrapped in `UI_ERROR`; the **Shared** switch with its `{H}` cap on the
  header line), Pen (five 50 px buttons with shape icons and names; size: a log2 slider 1..256 with
  16 in the middle, plus the exact field; Hardness appears as a row only while the soft pen is
  chosen), Colour (R G B A slider + field rows, the swatch as an opaque half and a half over the
  checker, the hex field, 8 recent swatches), Canvas (texels / m: a log2 slider 8..512 with 64 in
  the middle, plus the field; the texel-size line; *Resize to density* and the *Pixel look* toggle)
  and Faces (six buttons "+x 0" .. "-z 5", a click counts the cell on 0..5; *Apply* closes up the
  numbers and returns `DRAWPANEL_ACT_FACES`; *All separate* / *Sides together* only set the
  buttons, Apply applies them). Full height ~636 px.
- **720p**: the panel is as tall as its content but at most the room down to 42 px above the
  bottom (the map message line); when that is less, it **scrolls**: the wheel over it scrolls
  (except over a slider or a field, which take the wheel), a thin scroll bar shows on the right,
  and the content is clipped with a scissor (only while it overflows). With three HUD lines above
  it at 1280×720, about 70 px of the Faces section are below the fold. At 1080p everything shows.
- **`DrawPanel`** gained `interactive`, `typing` (a field had or has the keyboard this frame: the
  draw mode's keys should wait, ESC included, which the field used to cancel), `top`, `scroll`,
  `contentH`, and `DrawPanelSetOnly` for `--draw-panel <section>` (`all`, `target`, `pen`,
  `color` / `colour`, `canvas`, `faces`; another name: exit 1). `DrawPanelUpdate` returns true
  while the cursor is over the panel, while a slider drag goes on outside it, and on the click that
  closed a field outside it. `DrawMode` is unchanged.
- **The target text** is built from `dm->hover` and the canvases: "box line N, face +z" /
  "model f.glb slot n", then the canvas ("paint_12.png 768×512"), "own texture 1024²", or
  "material x: a new canvas" / "its own texture" (shared). The texel line gives the face's
  mm × mm when the hit's material has a canvas, else 1000 / density. Phase 3b should check these
  against the real state.
- **`--draw-panel`** (main.c) draws the panel with `DrawModeInit`'s state, five recent colours and
  the surface at the middle of the view as the target (the phase-0 runtime gives no face: the
  normal picks it). With `--input-script` the cursor is free and the clicks go to the panel; the
  designer's input is skipped in such a run (otherwise an ENTER that applies a field also opens
  and submits the prompt box: a real request bundle and a coordinator start), and the game keys
  wait while a field is open. Phase 3b replaces this with the draw mode's own wiring and must keep
  both guards (`drawPanel.typing` for the keys, the panel's mouse for LMB / ENTER).

### Implementation notes: phase 2 (the painting core)

What phase 2 built (`src/drawmode.c`, `src/mapedit.c`, the draw-mode wiring in `src/main.c`), and
where it differs from the sections above.

- **The first stroke** (§3.1) is `EDIT_PAINT_MATERIAL` with the canvas's step, pushed at LMB down
  (the canvas is provided first, so the appended material loads it live). Name `paint_<id>` /
  `paint_<id>_s<N>`: the line's own id when the whole line is painted, else `MapEditNextId()` (what
  the split copy or the new id gets); `-2`, `-3`, … when the name, a canvas or a file of that name
  exists. The material line, the `use` wrap, `material.N`, the split and the id are as §3.1;
  `--apply-edits` with the same op gives the same bytes. Deviation: **a flat colour's copy** is
  `albedo 255 255 255` and the canvas holds the old colour *with its alpha* (not `255 255 255 A`
  over a canvas with alpha too, which would square it). The keys copied are the source's lines as
  written minus `albedo`, `albedo_map`, `uv_scale`, `uv_offset`, `uv_faces` (and `filter` when the
  pixel look is on); `inherit albedo` / `inherit all` of a model override make the slot's own
  texture the source (and become `inherit normal orm emission`). The built-in `"default"` starts
  with its `roughness 0.9 metalness 0`. Source pixels come from the GPU (`MS_TextureFind` /
  `MS_ModelSlotTexture` + `MS_TextureRead`), the file as a fallback. Box cells are sampled at the
  face uv of each texel's centre (clamped in the gutter), bilinear with the material's wrap, times
  the tint; a model's own texture with identity tiling is copied texel for texel (its size kept).
  Verified: flat boxes (axis-aligned and turned), a Kenney model with per-slot overrides and one
  with its own materials, and `rock_face_01.glb`'s embedded texture all give **0 differing pixels**
  before / after the copy; a textured box (`tiles.png`, `uv_scale 2 2`) differs by its resampling
  (§3.3).
- **Shared mode** (§3.2, §3.4): the hit material's `albedo_map` is opened in place (PNG only; a
  `data_sampler` texture and a file without an `ASSETS.md` row are refused). A model slot drawn
  with its own material is extracted to `textures/paint/<file>_s<N>.png` (the file name kept to
  letters, digits, `-`, `_`): one `EDIT_PAINT_MATERIAL` over every instance of the file that draws
  the slot with its own material (`PaintModelLines` in `mapedit.c`: whole model lines only; each
  gets `material.N` and an id). Deviation: shared mode on a material **without** an `albedo_map`
  is refused ("material x has no texture to paint"), rather than making one canvas for every box
  of the material (different sizes, no layout).
- **Canvases** open lazily: an earlier session's `textures/paint/` PNG when the cursor first goes
  over it (so the HUD and the panel show it), a shared texture at the first stroke. Its base and
  `savedHash` are the file's pixels. After a reload from disk a canvas no step holds is marked
  stale and read again when next used.
- **Sizes** (§5.1) as specified; `2048 / max(cols, rows)` makes a clamped 3 × 2 cell 682 px (not a
  power of two): a 60 m ground box gets 2046 × 1364.
- **UV islands** (§7.3): triangles joined over shared UV edges (exact float bits, a hash table of
  edges, union-find). Deviation: the raster is **conservative** (a texel counts when its centre is
  inside the triangle or within half a texel diagonal of an edge), so the hit texel of a thin or
  small triangle always belongs to its island; texels two islands touch are shared and both may
  paint them. A triangle that spans the whole texture or more (tiled UVs, e.g. Kenney's flat-colour
  models with UVs of 0..30) marks its island **everywhere**: painting it paints every tile, which is
  what such UVs mean.
- **Strokes** (§7.1): screen-space bisection on a fixed stack of `2 × 10 + 4` segments (no
  allocation per frame), `spacing = max(1, size / 4)`; consecutive hits must be on the same region
  (face cell / slot triangle) and that close. Deviation: **a stroke stays on the canvas of its LMB
  down**; samples on other objects or canvases are skipped (no first stroke in the middle of a
  drag). Stamps: deviation from §7.2, so a straight drag has straight edges and is s texels wide at
  every size (separate discs about integer texel centres left a 1-texel nub at ±s/2 every stamp):
  the round pens from size 3 (hard, soft, eraser) sweep a **capsule** from the stroke's previous
  sample to this one, at their sub-texel positions: the texels whose centre is closer than s / 2
  (strictly) to the segment (soft: coverage `1 − smoothstep(hardness, 1, d / (s / 2))` of that
  distance). The pixel-exact footprints are stamped at every texel the segment crosses: the square
  pen's s × s (sample texel at `floor(s / 2)`), round size 1 one texel, round size 2 the 2 × 2 texels
  around the sample's sub-texel position (was a plus of 5, 3 texels wide). A sample is joined to the
  one before when both are on the same region and at most 2 × spacing + 1 texels apart (`Sample.joined`,
  kept for replays). Verified on the saved canvas at sizes 1, 2, 3, 4, 7, 16, 64, horizontal and
  vertical drags: every column / row the same profile, width exactly s (the soft pen at 64: 63, its
  outermost row's coverage rounds to 0); diagonals vary by at most 1 texel per column. Without a
  layout a stamp wraps in repeat mode and ends at the edge in clamp / mirror. Each touched 64 × 64 tile is copied once (the undo copy and the
  compositing origin) with a byte mask of the stroke's highest coverage, freed at LMB up. One
  `MS_TextureUpdate` per canvas per frame (the union of the frame's stamps), with raylib's log level
  at WARNING around it (the INFO line per mipmap generation; it is set back to INFO).
- **Undo** (§10): `MapEditPaintHooks` gained `save` (CTRL+S anywhere and auto save write the
  canvases through it: `MapEditSaveAll`; `EditModeSave` calls it). Deviation in the hook order: both
  `undo` and `redo` run **before** the step's text goes live (the image the text's material loads is
  provided by then); when the text can't go live the other hook puts it back. `FreeStep` takes the
  editor and releases paint through the hooks; a step that fails a rebase releases its paint at once
  (the failed list keeps the text). Layout changes and resizes are `STEP_IMAGE` steps holding the
  whole old image and base. A **resize** has no text: the material loads its texture again through
  two live edits (the text without that `albedo_map`, then the text: `Rebind`). The cap:
  `MapEditPaintBytes` counts undo, redo and lost steps; `TrimPaintHistory` drops the oldest undo
  steps after a push (`droppedSteps`; `droppedUnsaved` keeps dropped unsaved ones counted as unsaved
  until a save).
- **Runtime fixes** (`runtime/monolith_scene.c/.h`): `MS_ModelSlotTriangleIndex(scene, file, mesh,
  triangle)` (the slot index of a hit triangle, for its island and the brush cursor); `GetTexture`
  (editor, live) skips a cached entry whose size differs from the provided image of its path (a
  canvas laid out anew or resized gets a texture of its new size; the old one stays in the cache
  until the next load); `MS_TextureUpdate` updates the cached textures of the path **of the image's
  size** and fails only when there is none (it failed on any mismatch). Neither path is taken
  without a provided image: every map in `maps/`, `tests/models` and `tests/rotated` stay
  pixel-identical and box-identical to the base binary.
- **Saving** (§11): PNGs (live canvases with unsaved pixels; `textures/paint/` made when missing),
  `MapEditSave`, the licence records, then `MS_EditMarkSaved` when only PNGs changed (and the map on
  disk is still the one read). A save refuses before writing anything when the map changed on disk.
  Rows: a new canvas `` | `<path>` | made in monolith (painted by hand) | <package> | CC0 1.0,
  https://creativecommons.org/publicdomain/zero/1.0/ | <date> | - | none | ``; a derived one copies
  the source row's author, licence and sha256 ("derived from `<source>` (<its source>)", Changes
  "painted in monolith from `<source>`"); a shared file's row gets "modified in monolith" once in its
  Changes cell. Sidecars (`<png>.asset.md`, the `record_asset.py` layout) for new and derived files.
  The hot reload doesn't reload after the editor's own writes (`poll` in the script: 0 reloads after
  `save`). Closing with unsaved paint writes the canvases under
  `<settings>/unsaved/<map>-<time>/<package path>` beside the map text.
- **Rebase** (§12): `MapEditRebase` calls the rebase hook before it applies a paint step again; a
  step of paint alone goes back on the stack as it is, a first stroke's op is applied again with its
  paint (`ApplyStep`). Per canvas, the first hook of a rebase reads the PNG on disk: unchanged (its
  hash is `savedHash`) → provided and uploaded again; changed → the canvas and its base become the
  new PNG (any size) and each unsaved step of it is replayed from its samples (tiles taken anew),
  reported as "re-applied N strokes onto …, which changed on disk". A step whose canvas no material
  of the new text shows fails ("no material shows … any more"). `DrawModeBeforeSync` (ends a stroke
  going on, provides the canvases) runs before `EditModeSync`, `DrawModeSync` after it.
- **Draw mode** (§13.1): B in spectator mode (not while the prompt box, snip mode, the id panel or
  the transform modal have the keys); B / ESC leave (the first press warns with unsaved edits); M,
  V and C leave into their mode. The designer never sees the mode's input (no selection, no prompt
  box, ENTER does nothing); the effect toggles 1–6 and TAB (spectator / walk) wait; the crosshair is
  hidden. A stroke while a request is open on the map uses the edit mode's warning
  (`EditModeConfirm`, LMB again within 3 s). The panel's actions go to `DrawModePanelAction`
  (resize, faces from `DrawPanel.faces`, pixel look) on the canvas under the cursor, else the one
  painted last. HUD: `DRAW  hard round 4 px  #C83232 a255  |  paint_42.png 384x256 face +z`, then
  the refusal (red) and the last message. The brush cursor (phase 3b refines it): the footprint's
  texel outline mapped through the face (exact) or the triangle (affine), a square at size 1, the
  face outline for fill, a circle of the density's texel size before a canvas exists.
- **`--paint-script`** (§14; the commands: the header comment of `DrawModeRunScript`): also
  `hardness`, `pixel on|off`, `resize`, `hover px`, `poll`, `put <file> <package path>`, `reload`.
  `stroke uv` takes a box face (0..5) and face coordinates, or a model slot and a texture
  coordinate (the first slot triangle that holds it). An object's entity changes when its first
  stroke gives its line an id (the script names it `id N:i` from then on). With `--apply-edits` it
  runs after the edit script in a hidden window (the player's camera at the spawn). `--draw-mode` and
  `--draw-panel` take `--cursor x y`. `--input-script` gained `down lmb|rmb` / `up lmb|rmb` and the
  keys `LBRACKET`, `RBRACKET` (`src/input.c`).
- **Open**: model picking still hits back faces (a back-culled slot is painted from behind);
  `alpha_cutoff` 0.5 (R3D's default) makes every stroke frame redraw the sun tiles under the object
  (accepted); the eyedropper on a tiled model texture reads the texel the wrap gives.

### Implementation notes: phase 3b (the panel on real state, the brush cursor, the HUD, the keys)

What phase 3b built (`src/drawmode.c/.h`, `src/drawpanel.c/.h`, `src/keyspanel.c`, the draw-mode
wiring in `src/main.c`, `build/tools/leakcheck`), and where it differs from the sections above.

- **The panel's target** (§13.2) is not the surface under the cursor of this frame but the one the
  cursor last **rested** on (`DrawMode.target`, a `DrawTarget`): the hover becomes the target after
  0.3 s on the same surface (object and face / slot), at once on an LMB press in the scene and in a
  shot (`DrawModeHover`). Otherwise moving the cursor from a box to the panel crossed the ground (or
  whatever lies between) and the panel showed and acted on that. The target keeps its ray and is cast
  again when `scene->version` changes (a panel action, undo, a first stroke), so its material, canvas
  and lines are always of the current scene. `DrawTarget` carries the panel's three lines (`what`,
  `into`, `texel`), built in `drawmode.c` each frame, so the panel reads no scene indices itself.
- **What paint goes into**, before it exists: `DrawGeom` is the texture space of a surface: its
  canvas, the texture shared mode would open, or the canvas the first stroke will make
  (`NewCanvasSize`, the size rules of `CreateOwnCanvas`, cached for the hover and the target). So the
  panel says "first stroke: a new 384×256 canvas, a copy of crate" / "tiles.png 128×128 in place:
  every object with tiles changes" / the refusal in `UI_ERROR` (shared mode on a material without an
  `albedo_map`, a surface shader, a light), and the texel size is that of the real (or coming) canvas:
  a box face exactly in mm × mm (cell, `uv_scale`), a model about (the hit triangle's world area over
  its texels), "will be" before the first stroke.
- **Panel ↔ state**: the face buttons load the target canvas's layout and keep the user's unapplied
  changes until the target is another canvas (an apply or an undo that changes the layout brings them
  in step); changed buttons and *Apply* are lit. Faces shows only for box targets (a box without a
  canvas or without a layout: a line saying so). *Pixel look* shows the target material's `filter`
  (new canvases: the switch). The actions act on the target's canvas; without one: "faces: box line
  29, face +z has no canvas yet: paint it first", "resize: point at a painted box or model first ...",
  pixel look on no canvas only sets new canvases. `DrawModePanelAction` no longer falls back to the
  canvas painted last (`--paint-script`'s `faces` / `resize` / `pixel` still use that one).
- **Layout**: the panel is ~45 px shorter (headers 29 px, rows 22, pen buttons 40), so at 1280×720 it
  fits without scrolling for every pen but the soft one (its hardness row scrolls ~20 px). Its top is
  fixed below the status line and the DRAW line: the refusal, the draw mode's message and the
  launcher's request lines go **to the right of the panel** (x = `DRAWPANEL_RIGHT` + 12) while it
  shows, so the panel never jumps when a message comes or goes and the request lines never overlap it.
  With the panel hidden (TAB) they go under the DRAW line as before.
- **HUD in draw mode**: one line, state only: `DRAW  hard round 4 px  #C83232 a255  |  paint_1.png
  384×256 face +z  |  2 unsaved edits` (fill: no size; no canvas yet: `face +z: a new 384×256
  canvas`; shared: `| shared`). The designer's HUD lines, hover outline and hit marker are not drawn
  in draw mode (the designer doesn't select there; the panel and the cursor say what is under the
  cursor); the edit mode's lines only when edits failed to re-apply (its unsaved line is in the DRAW
  line). The status line shows the effect toggles without their `{1}`..`{6}` caps (1..5 are pens).
- **The brush cursor** (§13.3): the pen's footprint as phase 2 stamps it, in texel space about the
  hit's sub-texel position: square the s × s texels at `floor(s / 2)`, round size 1 the texel, size 2
  the 2 × 2 texels, from 3 a ring of radius s / 2 (48 segments; the eraser's dashed, the soft pen's
  core a fainter ring at hardness × s / 2), fill the outline of what one click fills (a box: every
  face of the hit face's cell, all six without a layout; a model: the border of the hit triangle's
  UV island, its UV edges no other triangle of it shares). On a box face the outline is mapped exactly
  and cut at the face's edges; on a model through **every triangle of the hit island** that the
  footprint's UV box touches, each by its own affine map and cut at its edges (not only the hit
  triangle: on a dense scan a brush spans dozens of triangles). The slot's triangles, islands and an
  island's border are cached (`DrawSlotTris`, per file, slot and scene version). The cursor shows the
  footprint the first stroke will paint too (the coming canvas's geometry). Nothing is drawn while the
  cursor is on the panel, while RMB flies, or over something that can't be painted. **Z-fighting**:
  lines on the surface itself z-fought with only the overlay's depth bias (dashes on a face seen head
  on), so each point moves 0.3 % of its distance toward the eye, along its own view ray (same pixel).
  Verified: dabs of each pen at sizes 1, 2, 5, 16 and fill on a box, a turned box and a scanned model
  (`rock_face_01.glb`), each shot with the cursor at the same place: the outline encloses the painted
  texels on all of them.
- **Keybindings panel**: a *Draw mode* section (B, RMB, LMB, ALT+LMB, 1..5, `[` `]` / CTRL+wheel,
  SHIFT+wheel, X, H, TAB, the panel's fields and wheel, CTRL+S, CTRL+Z / CTRL+SHIFT+Z, CTRL+SHIFT+DELETE,
  ESC / M / V / C), between *Rotate gizmo* and *Panels and views*. The tabs still fit at 1280.
- **Request safety**: a scripted `--draw-panel` shot now runs the draw mode's own input (the panel,
  then `DrawModeUpdate`), like a scripted `--draw-mode` shot. In both, the designer **never** gets the
  script's input, even after B leaves the mode (`drawShotLocked` in `main.c`: only B, to open the
  mode again). The leak check's old panel clicks missed the hex field on the new layout, typed "C"
  into the draw mode (snip mode) and then ENTER: the lock kept that from reaching the prompt box (no
  request). A panel field that has the keyboard still blocks every draw-mode and game key.
- **Verification**: every map in `maps/` at every viewpoint pixel-identical to the base without
  `--shot-hud`; with it only the fps text of the status line differs; `--dump-boxes` identical.
  `build/tools/leakcheck` gained `draw-cursor-fill`, `draw-cursor-model` and `draw-panel-canvas`, and the
  panel-click run's coordinates follow the new layout.
- **Open**: the settle time (0.3 s) and the 0.3 % lift are by eye and need a hand test; a soft-pen
  panel scrolls a little at 720p; regrouping faces keeps each new cell's first face's paint (§5.2),
  so paint on a face that joins a group with an earlier face is dropped (as specified, but surprising
  in use).

### Phase 4: final verification and its fixes

An independent verifier re-checked every claim with its own scripts on scratch copies (machine A:
RTX 4050 Laptop, driver 615.71.09, Ryzen 7 7435HS): unpainted maps (62 shot pairs incl. yaw sweeps,
`--reload-test`, `--fog-cache`, `--ao-volume`: 0 changed pixels; dumps identical; edit scripts
unchanged), every pen on a plain, a turned, a `repeat` copy, a `stairs` statement and two models
(widths, alpha, eraser, fill measured on the saved PNGs), minimal map diffs equal to
`--apply-edits`, undo / redo interleaved with text edits, hot reload and rebase, licence records,
face layouts, panel / cursor / HUD shots, the input safety (no bundle, no terminal), the live-edit
mesh fix (12 runs, 0 differing), builds, unit tests, `build/tools/leakcheck` and `--asan`, and painted
packages exported and rendered by `01_minimal` (0 pixels on 13 views). Fixed by the lead:

- `RwBuild` (`mapedit.c`) called `qsort` with a NULL array (a first stroke on a line that already
  has an id: no splice); `EditModeScanModels` the same with no models (pre-existing, `--asan`).
- The panel drew "recent colours" over empty swatch outlines before the first colour.
- §3.3's claim about flat colours (above): measured, not always 0 pixels.

Left open (the final summary lists them): one stroke on a heavily tiled material bakes the tiling
into one canvas (pine_forest's ground, `uv_scale 286`: a 2046 × 1364 canvas, the whole floor
blurs: §3.3 as designed, but without a warning); a first stroke entirely outside the texture still
makes the material and an empty step; the script's `put` accepts `../` paths (a test command).

### Implementation notes: shader bake (§3.6, after the round)

What the shader bake built (`src/surfacebake.c/.h` new, `src/drawmode.c/.h`, one call in `src/main.c`,
`MS_BoxDrawOf` in the runtime, `build/tools/leakcheck`), and where it differs from §3.6.

- **Where the render lives**: `src/surfacebake.c`, not `bake.c`. `--bake-surface` is a tool of its own
  (its own window and R3D context, the map loaded again with `MS_BAKE` defines); this one renders the
  loaded scene's own material between two game frames. `SurfaceBakeBox(scene, box, cells, W, H, out)`
  gives three images of the canvas's size and layout.
- **Runtime** (`runtime/monolith_scene.c/.h`, editor block "Painting"): `MS_BoxDrawOf(scene, box,
  &MS_BoxDraw)`: the box's material, the unit cube its boxes are drawn with (R3D's or the face layout's
  cube), and either the material's instance buffer with the box's slot in it (instanced materials:
  `boxOrder`'s run of the material, the order `LoadBoxInstances` filled) or, for a merged material,
  the model matrix that puts the unit cube where `BuildBoxMesh` / `TurnedFaces` put its faces.
  Deviation: a merged box is drawn as that transformed cube (`R3D_DrawMeshPro`), not as a mesh of its
  world-space faces: no mesh is allocated in R3D's shared vertex buffer for the bake (the recycled-range
  fix of phase 1 above), and the fragment stage sees the same world positions, normals and UVs (merged
  materials never read `POSITION` in `vertex()` or `INSTANCE_*`: the loader instances those).
  Instanced boxes, turned ones included, are drawn from the scene's own buffer
  (`R3D_DrawMeshInstancedEx(cube, material, buffer, slot, 1)`), so `INSTANCE_*` and the model-space
  `POSITION` are the scene's; no instance buffer is created or freed (R3D caches the last bound buffer
  name, r3d-pitfalls §4).
- **The frames** (per face, four R3D frames: albedo over a black backdrop, albedo over a white one,
  ORM, normal) go into one RGBA8 colour-only target of **R3D's internal resolution**
  (`R3D_GetResolution`), so the visualizer copies the G-buffer one texel per pixel and coverage is
  decided at pixel centres. An orthographic camera 1 m outside the face looks along its normal (near
  0.5, far 2.5 m); its pixels are square, a third of a canvas texel along the face's finer axis
  (`SUPERSAMPLE` 3) where the face fits, else as small as the target allows, and the face's u = 0 /
  v = 0 edges lie on pixel edges. Each canvas texel takes the pixel at its centre: point sampling, so
  a pixel-art shader keeps crisp edges (a box filter of the texel's 3 × 3 pixels was tried: mean
  differences about the same, max lower, pixel-art edges visibly softer); the finer pixels give the
  shader the screen derivatives of a close view (its texture LOD and pixel-size levels at their
  finest; with pixels of texel size the weed and porcelain grain came out coarser: mean albedo
  difference 1.14 / 3.01 instead of 0.55 / 1.57). Gutter texels copy the nearest texel inside the
  face. Faces of zero area are filled opaque and flat.
- **Coverage**: R3D clears the G-buffer to 0 whatever the background colour, so the "two clear
  colours" are a **backdrop**: R3D's unit cube as a 1 mm slab just behind the face plane, inside the
  box (`min(1 mm, thickness / 4)` behind it), `cull none`, albedo 0 0 0 then 255 255 255, `alpha_cutoff
  0`. A pixel that shows exactly 0 0 0 over the black one and 255 255 255 over the white one is a hole
  (alpha 0); covered pixels give the same bytes in both frames. The slab also hides the rest of the
  box, which a `cull none` material would otherwise show through its holes. Uncovered texels take the
  colour of covered neighbours (4 rings, alpha stays 0), so filtering at a hole's edge pulls in no black.
- **Values**: albedo bytes (linear) → sRGB bytes (a table); ORM bytes copied as written (alpha 255);
  the world normal (8-bit view of R3D's octahedral RG16) turned into the face's tangent space with the
  unit cube's tangent, bitangent (`cross(N, T) · w`, as R3D's vertex stage) and normal for that face,
  turned with a turned box. The normal map is kept when some covered texel's normal is more than
  ~2.6° (dot < 0.999) from the face's own normal; else there is none.
- **The R3D state**: R3D's environment is `memcpy`'d out, replaced by `R3D_ENVIRONMENT_BASE` (no sky,
  ambient map, SSAO / SSIL / SSGI / SSR / fog / bloom / auto exposure) and `memcpy`'d back; the output
  mode and the depth-output flag are restored. No light is pushed (no shadow job runs). What still
  changes: R3D's frame counter (`FRAME_INDEX`) advances by 24 per bake (no shader in `maps/` reads it).
- **The flow** (`drawmode.c`): LMB on a box with a shader material asks for the bake (`RequestBake`:
  the refusals below, the sampler textures' licence rows, the canvas size) instead of beginning the
  stroke; `DrawModeRunBake`, called by `main.c` every frame right after the model picker's thumbnails
  (outside `R3D_Begin` / `End`, before the scene's draw), renders it; the next `DrawModeUpdate` begins
  the stroke where LMB went down (`PressStroke` with the stored hit and cursor position; a click whose
  button is up by then is one dab). The bake is dropped when `scene->version` changed meanwhile (said in
  the message line), while RMB flies, and when the mode is left. `--paint-script` bakes at once inside
  the script (`DrawMode.bakeNow`), so `--shot` and `--apply-edits` scripts give the same files.
- **The copy's keys**: the source's keys without `shader`, `uniform`, `sampler`, `data_sampler`,
  `instanced`, `orm_map`, `occlusion`, `roughness`, `metalness` (and `normal_map`, `normal_scale`
  when a normal map is baked; `alpha_cutoff` when it is 0 and the bake has holes), then `albedo 255 255
  255 albedo_map "…/paint_<id>.png" orm_map "…/paint_<id>_orm.png" occlusion 1 roughness 1 metalness
  1` [`normal_map "…/paint_<id>_normal.png" normal_scale 1`] [`alpha_cutoff 0.5`] `uv_faces 0 1 2 3 4
  5`. E.g. for a tower ruin spandrel: `material "paint_11" alpha_cutoff 0 albedo 255 255 255 albedo_map
  "textures/paint/paint_11.png" orm_map "textures/paint/paint_11_orm.png" occlusion 1 roughness 1
  metalness 1 normal_map "textures/paint/paint_11_normal.png" normal_scale 1 uv_faces 0 1 2 3 4 5`.
  A name is taken only when none of its three PNGs exists.
- **The ORM and normal maps** live in the `Canvas` (`orm`, `normal`, `ormRel`, `normalRel`): provided
  with the canvas (`Provide` / `Unprovide`, so a first stroke, an undo, a redo, a rebase and a renderer
  restart load them live), written once on the save that writes the canvas (`extrasWritten`; they are
  not painted), written with the canvas when the game closes with unsaved paint. Undo of the first
  stroke takes all three out of the scene; nothing is on disk before a save.
- **Licence records** (`RecordBaked`): a row and a sidecar for each of the three PNGs: Source "baked in
  monolith from `shaders/x.glsl` (made in monolith) and `textures/y.jpg`", Author the package plus each
  sampler texture's author, Licence CC0 1.0 for a shader without a row (as a new canvas) plus each
  texture's licence cell (each once), Changes "the shader's albedo (the canvas) / occlusion, roughness,
  metalness (orm_map) / tangent-space normals (normal_map) rendered in monolith's draw mode". A shader
  file with an `ASSETS.md` row gives its own author and licence.
- **Refused** (the panel's target line and the HUD say why before the click): a model slot with a shader
  material (§3.5), shared mode on a shader material, a shader material R3D draws in the forward pass
  (`transparency alpha`, `unlit on`: no G-buffer view), a sampler / data_sampler texture without an
  `ASSETS.md` row (refused at the click, before anything is rendered). **Face groups** (`faces`, §5.2)
  on a baked canvas are refused ("this canvas holds a baked shader, whose ORM and normal maps keep its
  layout"), in the session and for a canvas of an earlier session (its material has an `orm_map` or
  `normal_map` under `textures/paint/`); *Resize to density* works (the maps keep their size and the
  same layout).
- **Deviations from §3.6**: the backdrop instead of clear colours; a merged box drawn as the
  transformed unit cube; the target at R3D's internal resolution with point sampling; `alpha_cutoff 0.5`
  only when the material's was 0 (R3D's default without the key is 0.5 already); **a statement of
  several boxes** (`stairs`, `waffle`, a repeat that can't be split; they share one canvas, §3.1) is
  baked from the box that was clicked, so the others show that box's look; face groups refused.
- **Time**: 35–300 ms per bake on machine A (RTX 4050 Laptop, driver 615.71.09, Ryzen 7 7435HS): a weed
  tuft (192 × 128) ~0.1 s, a concrete pillar (1536 × 1024) ~0.2 s, the ivy sheet (2046 × 1364) ~0.24 s:
  a hitch on that one click.

**What a bake loses** (for the developer's decision; every shader in `maps/monolith` reads the eye
position, `v_eye`, and none reads `TIME` or `FRAME_INDEX`):

- **Detail finer than the canvas** blurs; pixel-art shaders (`u_px 24`, a 4.2 cm grid) baked at 64
  texels/m get edges that fall between texels (art pixels 2 or 3 texels wide); a density that is a
  multiple of 24 (72, 96) would keep them even. Up close the canvas is magnified bilinearly (or crisp
  with the panel's pixel look).
- **Distance levels** (`PxScale`: a coarser pixel grid farther away) freeze at the finest level, and
  textures read through `dFdx` / `textureGrad` (the grain photo) at a close view's LOD; the canvas's
  mipmaps do the minification instead.
- **`EMISSION`** is not baked (no G-buffer view): the tower ruin's window glow and the carpet's light
  are gone on a baked box; the copy keeps the material's own `emission` keys.
- **`SPECULAR`** is not baked either (R3D's ORM view shows only occlusion, roughness, metalness): the
  copy has the material's `specular` key or R3D's default 0.5 (concrete wrote 0.35, ivy 0.3, carpet 0.2).
- **View-dependent work**: ivy projects its leaves onto the sheet from the eye (`v_eye`); the bake shows
  it from 1 m straight in front, everywhere.
- **Per-box layout** (`INSTANCE_*`: tower ruin, dead wood, toy, ivy) is baked correctly for the box that
  was clicked; a multi-box statement shares it (above).
- **Holes** are kept (alpha 0 + `alpha_cutoff`), but the sun shadow samples the canvas's alpha at the
  shadow map's mip level, so small holes (the scratch crate's 5 cm dots) close in its shadow.
- **8-bit round trips**: linear → sRGB → linear albedo loses at most a level above ~0.2 linear; the
  normal goes through an 8-bit world normal and an 8-bit tangent-space map (~0.5° each).

**Verification** (machine A; scripts and images in the session's scratch folder `agentE/`, the larger
runs moved to the worktree's `build/agentE-scratch/` when `/tmp` filled up):

- **First stroke of alpha 0** on a copy of `maps/monolith` (`--apply-edits` + `--paint-script`, `dab uv`):
  a concrete pillar (line 360, face +x, 1536 × 1024), a weed olive tuft (line 33551, top), a tower ruin
  spandrel (line 488, a repeat copy, instanced, face +z, normal map), the porcelain bath rim (line 668,
  top), the missing poster (line 378, face +z, holes), the ivy climbing sheet (line 8000, instanced,
  `cull none`, holes, 2046 × 1364). Changed pixels head-on / oblique of 1 440 000: concrete 422 735 /
  199 233 (mean 1.4 / 1.7 levels), weed 43 317 / 17 841, tower 86 823 / 24 271, porcelain 181 168 /
  60 507, poster 119 919 / 28 768 (mean 4.6, max 84), ivy 174 939 / 38 773. At the seven viewpoints:
  0 pixels except concrete 1 881 (view 2), poster 561 (view 2), tower 348 / 176 (views 5, 6), ivy 5 206
  / 2 571 / 20 859 / 2 594 (views 3–6), mean 0.8–3.2 levels.
- **Baked albedo vs the live G-buffer albedo** of the same face head-on (`--output-mode albedo`, over
  the face's pixels, the mask from a magenta fill): mean / max |difference| in 8-bit levels: concrete
  1.12 / 47, weed 0.55 / 14, tower 1.85 / 17, porcelain 1.57 / 37, poster 3.67 / 128 (pixel-art
  edges), ivy 2.02 / 88 (leaf edges).
- **Real strokes** (red, round 24, a zig-zag in face space) show on the right face over the baked look
  on all six. The same script live in a `--shot` run with `save` writes a byte-identical map and PNGs
  (`ASSETS.md` differs only in the package folder's name as author); the live painted shot equals a
  shot of the saved copy loaded from disk (0 pixels); `dab` + `undo` + `save` leaves the map's bytes as
  they were, no `textures/paint/`, `ASSETS.md` unchanged.
- **Turned and instanced boxes** (a scratch copy of `tests/rotated` with a world-space checker shader
  with holes and a tilted normal on the yawed wall and the tumbled crate, both merged, and a model-space
  stripe shader on the `instanced on` boxes, axis-aligned and turned): the baked albedo, normals and
  holes match the live G-buffer views (`--output-mode albedo` / `normal` up close); the live script and
  the `--apply-edits` copy give the same image (0 pixels).
- **The frame of the bake and the next**: a click through the input shim on the concrete pillar; the
  frame the bake ran in is pixel-identical to the same run without the click, the next frame differs
  only on the pillar (412 728 pixels on its face, 9 999 elsewhere of at most 11 levels: its indirect
  light); the end shot equals the `--paint-script` dab at the same pixel (0 pixels). (The shot frames
  were picked with a temporary hook in a scratch build, not in the change.)
- **Unpainted**: every viewpoint of every map in `maps/` and `tests/models`, `tests/rotated` against the
  base build: 0 changed pixels, `--dump-boxes` identical.
- `build/tools/leakcheck` gained `paint-bake` (the awning: bake, undo, redo, save, a model stroke),
  `paint-bake-rot` (the scratch `tests/rotated` with shaders, `--apply-edits`) and `paint-bake-input`
  (a click and a drag through the input shim, undo, a second bake); `paint-script`'s last command is now
  shared mode on the awning (refused, exit 1).

**Open**: specular and emission of a shader (an R3D view of the ORM target's alpha and of the emission
would need an R3D patch); the density for pixel-art shaders (a multiple of their grid); a multi-box
statement baked from one box; face groups on a baked canvas (relaying out the ORM / normal maps too);
the ~0.1–0.3 s hitch on the click.

