# Painting

Draw mode (**B**) lets the developer paint on any box or model in the editor and edit the
textures the scene uses, without a request and without leaving the game. This is the design the
development round works from, written in the same way as `docs/live-editing.md` (the working
copy, live edits, undo, panels and verification flags that this round builds on). Implementation
notes follow at the end, one per stream as it merges.

## 1. Why, and what stays the same

The developer wants to put marks, signs, decals and pixel art on objects by hand instead of
describing them in a request. Painting writes **package files** (PNGs under `textures/paint/`)
plus the **smallest change to the map text** (a material and where it is used), so requests, hot
reload, the export and the AI keep reading the package as they do now.

What stays the same:

- **A map that nobody painted looks and behaves exactly as before.** Every new path in the
  runtime is taken only for a material with one of the new keys (§4). Every map in `maps/` gives
  pixel-identical `--shot`s and an identical `--dump-boxes`.
- Model files on disk are never changed. A model is restyled through material overrides only
  (`material.N`).
- Boxes of materials without the new keys merge into meshes exactly as now.
- Only **albedo** is painted this round. Roughness, metalness and emission painting is left for a
  later round.

## 2. Decisions

The developer decided these before phase 0:

1. **Pixel size means both controls.** One is the **brush size** in texels (1 = one texel, for
   pixel art). The other is the **canvas density** in texels per metre, which sets how big a
   texel looks in the world. New canvases use the density. An existing canvas can be resized to
   it (§5.3).
2. **Each object gets a texture of its own.** The first stroke on an object gives it its own
   material and canvas (§3). A **shared** switch on the panel paints into the material's existing
   `albedo_map` instead, so every object using it changes.
3. **Box faces: the developer says which faces share texture space.** This is a per-material
   face-group key, `uv_faces` (§4.1): faces with the same number share one cell of the canvas.
   Leaving the key out keeps today's UVs, where all six faces show the whole texture.

Defaults chosen in phase 0 (each can change; the final summary lists them again):

- Key **B** in spectator mode, next to M / V / C.
- The panel is docked on the **left**, so it doesn't collide with the inspector, id and picker
  panels on the right. It is mouse-driven (§13.2).
- Pens: hard round, square (pixel-exact), soft round, eraser, fill (face / UV island).
- Eyedropper on **ALT+LMB**.
- Canvas density 64 texels/m, at most 2048 px per side, at least 16.
- Folder `textures/paint/`, PNG only (RGBA8).
- One shared undo history with the map edits.
- New box canvases get `uv_faces 0 1 2 3 4 5`: every face has its own cell.
- The eraser restores the canvas's **base image**: what the canvas held when it was made or
  first opened in this session (§7.4).
- Paint on a material with a surface `shader` is refused this round (§3.5). Changed after the round:
  a box's first stroke bakes its shader into the canvas (§3.6); model slots are still refused.

## 3. Where paint lands

### 3.1 An object's own material (the default)

The first stroke on an object that has no canvas of its own is **one undoable edit** that:

1. Makes a **copy of the object's current material** under a new name, `paint_<id>` (a model
   slot: `paint_<id>_s<N>`). `<id>` is the object's line id. A line without one gets the next
   free id, as CTRL+I does (`mapedit.c`). If the name already exists, `-2`, `-3`, … is appended.
2. Gives the copy `albedo_map "textures/paint/paint_<id>.png"` (a model slot:
   `…/paint_<id>_s<N>.png`), plus the keys the canvas needs (§3.3).
3. Points the object at the copy:
   - **A box line**: the line is split out of its `repeat` as the other edits do (one copy
     painted). It is then wrapped as `use "paint_<id>"` … `use "<the material it had>"`, so the
     lines after it keep their material.
   - **A model**: `material.N "paint_<id>_s<N>"` is added to its line (or replaced), also split
     out of a `repeat`.
   - **Several boxes from one statement** (`stairs`, `waffle`, a `block` with a repeat that can't
     be split): every box of the statement uses the material. They share one canvas, because they
     share one layout (§4.1).
4. Makes the canvas (§5) in memory. Nothing is written to disk before CTRL+S (§11).

The **material line** goes after the last `material` statement of the file when that comes
before the object's line. Then the new material is appended to `materials[]`, which the live
path keeps cheap (§9.4). Otherwise it goes right before the `use` line, and that edit is a full
upload from memory (§9.4; it happens once per object).

### 3.2 The shared texture

With the panel's **shared** switch on, a stroke paints into the texture the surface already shows
(the material's `albedo_map`, or a model slot's own albedo, §3.4), in place, so every object that
uses it changes. The map text changes only when the source must be extracted first (§3.4). Saving
writes the PNG over the package file it came from. If that file is a recorded asset, its
`ASSETS.md` row gets "modified in monolith" (§11.2). A texture that lives outside the package
can't be painted.

### 3.3 What the copy holds

| The object's material has | Copy and canvas |
|---|---|
| a flat colour, no `albedo_map` | albedo `255 255 255 A`; the canvas is filled with the old colour (sRGB bytes as written) |
| an `albedo_map`, `uv_scale 1 1`, `uv_offset 0 0` | the canvas is the texture's pixels (a box: resampled into each face's cell, §5.1), times the albedo colour (the tint is baked in); albedo `255 255 255 A` |
| an `albedo_map` with any other `uv_scale` / `uv_offset` | the tiling is baked: the canvas samples the texture as the surface showed it (bilinear, wrap as the material's), then `uv_scale 1 1`, `uv_offset 0 0` |
| a model slot drawn with its own material | the slot's albedo texture read back from the GPU (§3.4) times its colour; the copy gets `inherit normal orm emission` and writes the scalar values of the slot's `R3D_Material` that the map format can express (roughness, metalness, occlusion, normal scale, emission colour and energy, cull, transparency, blend, alpha cutoff, unlit) |

Every other key of the source material is copied as written (`normal_map`, `orm_map`,
`roughness`, `wrap`, `priority`, …). So the object looks the same before the first stroke, up to
the resampling. A flat colour is the same up to the 8-bit sRGB round trip: in phase 2's shots
often 0 changed pixels, but the final verification measured a texture-sampled colour lighting its
surroundings slightly differently from the constant one (up to 1–2 levels on ~45–57 k pixels of
one view, mostly on a neighbouring box, through indirect light), deterministic and the same after a
save and reload.

Baking the tint means the colour you pick is the colour you see (lighting aside), and the
eyedropper reads what is shown.

### 3.4 Textures embedded in a model

A model slot's albedo can be embedded in a GLB, or be an external file the `.gltf` names. In both
cases it is read back from its GPU texture (`MS_TextureRead` with a model slot, §8.2; mip level
0, RGBA8). The model file is not touched. In shared mode the readback is extracted to
`textures/paint/<model file name>_s<N>.png`. Every instance of that file that draws the slot with
its own material gets `material.N` of a new material `paint_<file>_s<N>` (`inherit normal orm
emission`, the slot's albedo map extracted). That is one edit and changes those lines only.

### 3.5 What can't be painted (this round)

- A **model slot** drawn with a surface-shader material (a bake of a model would have to render
  in UV space; no map in `maps/` has one). Boxes with a shader material are baked (§3.6).
- Lights, the sky, and textures of the environment.
- A model slot without UVs: refused ("this model has no texture coordinates").
- A `data_sampler` texture: it holds data, not colour.

### 3.6 Surface-shader materials on boxes: baked into the canvas (decided after the round)

On the default map 97 % of the boxes (50 688 of 52 290) use a surface-shader material (concrete,
the weeds, the tower, porcelain …), so refusing them made draw mode useless there. The developer
decided: **bake the shader into the canvas** on the first stroke and drop the shader from the copy.
The object can look different after its first stroke (that is accepted): detail finer than the
canvas density blurs, animation (`TIME`, vertex sway) freezes at the frame of the bake, and light
the shader computes per view is lost.

- **What is baked**, per face of the box (each into its cell of the layout, §4.1), all from R3D's
  G-buffer views of one offscreen frame per output (no light, fog or post: the bytes the shader
  wrote):
  - `R3D_OUTPUT_ALBEDO` → the canvas (`albedo_map`, the painted image). The G-buffer is linear,
    the canvas sRGB: converted. The copy's `albedo` becomes `255 255 255`.
  - `R3D_OUTPUT_ORM` → `textures/paint/paint_<id>_orm.png` (`orm_map`; occlusion, roughness,
    metalness as the shader wrote them; not painted). The copy's `roughness` / `metalness` /
    `occlusion` become 1 so the map's values pass through as written.
  - `R3D_OUTPUT_NORMAL` → `textures/paint/paint_<id>_normal.png` (`normal_map`), converted from
    world space into each face's tangent space (the unit cube's tangent and normal for that face,
    turned with a turned box). Only when the shader changes the normal (else no normal map).
  - **Coverage**: a shader that discards (cutout weeds) leaves holes. The albedo pass is drawn
    twice over two different backdrops (black, then white); a texel that shows the backdrop in both
    is uncovered and gets alpha 0, and the copy keeps the material's `alpha_cutoff` (0.5 when it was
    0). (Implemented with a backdrop slab, not clear colours: R3D clears the G-buffer to 0 whatever
    the background colour.)
  - The shader's `EMISSION` is not baked (no G-buffer view of it); the copy keeps the material's
    own `emission` keys.
- **How**: the box is drawn alone, the way the scene draws it (a merged material: R3D's unit cube
  with the transform that puts it where its merged faces lie, so the fragment stage sees the same
  world positions, normals and UVs; an instanced one, `instanced on` or a shader that needs it: the
  material's own instance buffer at the box's slot, so a shader that reads model-space `POSITION`
  or `INSTANCE_*` sees what it sees in the scene), with the material's own shader alias, uniforms
  and samplers. For each face an orthographic camera looks at it from outside along its normal and
  covers the face (a convex box: the other faces are edge-on), with pixels no larger than the
  cell's texels (the target has R3D's internal resolution, one pixel per G-buffer texel; implemented
  notes below). It runs between two game frames, outside `R3D_Begin` / `End`, with R3D's
  environment, output mode and depth-output flag saved and restored byte for byte (the model
  picker's thumbnail pattern), so the scene's frame and caches are unchanged.
- **Where it lands**: the copy (§3.1) without `shader`, `uniform`, `sampler`, `data_sampler` and
  `instanced` keys (and without `orm_map`, `occlusion`, `roughness`, `metalness`; with a baked
  normal map also without `normal_map` and `normal_scale`), with the three maps above (`normal_scale
  1`), `uv_faces 0 1 2 3 4 5`. One undoable edit with the first stroke, as for any first stroke; the
  stroke itself is applied in the frame after the bake.
- **Licence**: the baked PNGs derive from the shader file (made in the package: its `ASSETS.md`
  row if it has one, else "made in monolith") and from any texture its samplers read (their rows'
  licences and attributions); a sampler texture without a licence row refuses the bake.
- **Shared mode** on a shader material is refused (there is no texture to share). So is a shader
  material that R3D draws in the forward pass (`transparency alpha`, `unlit on`): the G-buffer has no
  view of it.
- **Verification**: before/after shots of a first stroke of alpha 0 on boxes of each kind of
  shader material in `maps/monolith` (concrete, a weed, the tower, porcelain, a poster), on a
  scratch copy: the difference is reported as a trade-off (pixel counts and images), not
  required to be 0; the baked albedo vs the live shader's G-buffer albedo of the same face seen
  head-on (`--output-mode albedo`), which should agree up to the texel resampling.

## 4. Map format additions

Both keys are material keys of format 2, opt-in. A map without them parses, meshes, compiles and
draws exactly as now.

### 4.1 `uv_faces`: which box faces share texture space

```
material "paint_12" albedo 255 255 255 albedo_map "textures/paint/paint_12.png" uv_faces 0 1 2 3 4 5
```

- `uv_faces a b c d e f`: one cell number per face, in the order **+x −x +y −y +z −z** (the box's
  own axes for a turned box). Faces with the same number share one cell of the texture. The numbers
  are 0..5, and every number from 0 to the largest one is used (so `0 0 1 1 2 2` and `0 0 0 0 1 2`
  are fine, `0 0 3 3 3 3` is an error).
- `k` cells (the largest number + 1) are laid out in a grid of `cols = ceil(sqrt(k))` ×
  `rows = ceil(k / cols)` cells: 1 → 1×1, 2 → 2×1, 3 → 2×2, 4 → 2×2, 5 and 6 → 3×2. Cell `n` is
  at column `n % cols`, row `n / cols`.
- Within its cell, a face keeps the orientation the unit cube gives it today (R3D's
  `R3D_GenMeshDataCube`: u along the face's tangent, v up for the side faces), inset by a
  **gutter** of 1/64 of the cell on each side, so mipmaps don't bleed from the next cell (§5.1).
- A face of a box is `u, v` in 0..1. It is sampled at
  `((col + g + u (1 − 2g)) / cols, (row + g + v (1 − 2g)) / rows)` with `g = 1/64`.
- **Where it applies:** every box drawn with the material: merged meshes (`BuildBoxMesh`,
  `TurnedFaces`), instanced cubes (one unit cube mesh per distinct layout, §9.3), the transform
  preview's cubes. Models ignore it.
- It needs `uv_scale 1 1` and `uv_offset 0 0` (the defaults). Any other value is a parse error
  ("uv_faces needs uv_scale 1 1 and uv_offset 0 0"), since tiling would cross the cells.
- The texture keeps its wrap (`wrap`); the gutter keeps samples inside the cell.
- **All faces alike** (today's mapping, no gutter) is the material without the key. `uv_faces 0 0
  0 0 0 0` is the same layout with a gutter.

Every face of one material uses the same layout. So all boxes of one material share it, and
painted boxes (one material each, §3.1) choose freely. Changing the layout of a painted material
from the panel moves its paint (§5.2).

### 4.2 `filter`: how a texture is magnified

```
filter trilinear | nearest
```

- `trilinear` (default, today): linear magnification, trilinear minification with mipmaps.
- `nearest`: nearest magnification (crisp texels up close, for pixel art) and
  nearest-mipmap-linear minification (no shimmer at a distance). It applies to the material's
  `albedo_map`, `normal_map`, `orm_map` and `emission_map`.
- The texture cache (`TexEntry`) keys on the filter as well as the path, colour, data and wrap,
  so one file used with both filters is two textures.
- The draw panel's **pixel look** switch writes `filter nearest` on the painted material.

### 4.3 Compiled maps, the export, the docs

- `CmMaterial` gains `uint8_t uvFaces[6]` (255 = no key) and `uint8_t filter` (0 trilinear, 1
  nearest), plus padding to keep 4-byte alignment. `CM_VERSION` 4. The reader checks the parser's
  rules and accepts versions 2–4: an older file has neither key. The writer always writes 4.
  Changes: `docs/compiled-map.md` and its version history.
- The export copies a package's files as now, so `textures/paint/*.png` go with it (they are
  dependencies). Nothing else changes.
- The keys go into the header comment of `runtime/monolith_scene.c` ("Materials") and
  `docs/map-format.md` §2.

## 5. The canvas

A canvas is an RGBA8 image in memory with the package path of its PNG, the GPU texture(s) that
show it, its layout and its base image (§7.4). The draw mode owns it (`drawmode.c`). The runtime
only receives pixels (§8.2).

### 5.1 Size

The **density** `ρ` is in texels per metre (the panel's *Canvas* control, default 64).

- **A box** (a layout of `cols × rows` cells): the cell side is
  `c = clamp(pow2ceil(ρ · L), 16, 2048 / max(cols, rows))`. `L` is the longest edge of any face
  in any cell of the material's boxes (all boxes of a multi-box statement share it). The canvas is
  `cols · c × rows · c` (e.g. 3 × 2 cells of 256 = 768 × 512).
  - Cells are square. A face that isn't square stretches its texels along its longer side, as
    textures on boxes do now. The panel shows the face's texel size in mm on both axes (§13.2).
    Open question (§16).
- **A model slot**: the side is `clamp(pow2ceil(ρ · sqrt(A)), 16, 2048)`, square. `A` is the world
  surface area of the slot's triangles in this instance (scale applied). For a slot whose own
  texture is copied (§3.3), that texture's size is kept, so its pixels stay as they are.
- **An existing texture** (shared mode, §3.2) keeps its size.

### 5.2 Changing the layout of a painted box

The panel's face-group control rewrites `uv_faces` on the painted material: an undoable edit,
`EDIT_MATERIAL_SET` (§10.3). The canvas is laid out anew, and each new cell takes its content from
the old cell of its **first** face (in +x −x +y −y +z −z order), resampled nearest when the cell
size changed. Faces that become one cell therefore show the paint of the first of them. Undo
restores both the key and the old canvas.

### 5.3 Changing the density of a canvas

*Resize to density* on the panel resamples the canvas to the size §5.1 gives for the current
density: nearest when `filter nearest` is set, bilinear otherwise. This is one undoable step that
keeps the whole image from before.

## 6. Finding the texel under the cursor

`MS_RaycastSurface` (§8.1) gives the object, the material it is drawn with, and the texture
coordinate:

- **An axis-aligned box**: the face from the hit normal's axis and sign. `u, v` come from the hit
  point's position on the face: the box's local coordinates in −0.5..0.5, mapped as R3D's cube
  vertices map them (linear in the two face axes, so this is exact). Then the cell transform of
  §4.1 when the material has `uv_faces`.
- **A turned box**: the same in the box's own frame (the hit point minus the centre, dotted with
  its three axes; the face from the local normal).
- **A model**: the triangle the BVH hit, its barycentric coordinates, and the triangle's three
  texture coordinates. `ModelPick` keeps those, plus each triangle's mesh (§8.1). The slot is
  `R3D_Model.meshMaterials[mesh]`. The material is the slot's override (`material.N`, `material`,
  `use`), else the model's own (-1).
- Then the material's `uv_scale` / `uv_offset` (models: whatever their material says), wrapped
  as the material wraps (repeat: `fract`, clamp: clamp to 0..1, mirror). The texel is
  `floor(u · W), floor(v · H)` of the canvas (row 0 is `v = 0`, as raylib uploads an image; a PNG
  of a wall therefore looks upside down in an image viewer, which doesn't matter here).

A model's UVs can overlap (mirrored halves, tiled trims). Painting one place then paints every
place that shares those texels, which is what the texture really is. The HUD says "shared UVs"
when the hit triangle's UV area overlaps another triangle of the slot (known from the island
mask, §7.3).

## 7. Strokes

### 7.1 The stroke model

- A stroke runs from LMB down to LMB up. It is one undo step (§10).
- **Samples**: every frame, the segment from the last cursor position to the current one is
  subdivided in screen space until consecutive surface hits lie at most `spacing` texels apart on
  the same canvas (`spacing = max(1, size / 4)` texels; hard round and square at size 1–2:
  every texel). The bisection depth is at most 10 per frame and reuses the frame's scratch, so it
  allocates nothing. Where the cursor crosses onto another face, cell, triangle or object, the
  stroke goes on there. A canvas jump (another object) never interpolates across.
- **Stamps** are applied in texel space at each sample, clipped to the hit's **region**: the cell
  of a box face (§4.1), or the UV island of a model triangle (§7.3). So a brush never leaks into
  another face's cell or another island. A stroke along a box edge stops at the edge and goes on
  on the next face as the cursor moves over. Open question (§16): a brush projected across edges.
- Colour: RGBA 0–255 from the panel, composited **source-over** in 8-bit sRGB bytes (as the
  PNG stores them), with alpha = colour alpha × the pen's coverage. Within one stroke a texel takes
  the **maximum coverage** any stamp gave it (a stroke mask), not the sum, so an alpha-128 stroke
  that crosses itself stays alpha 128. The mask is a byte per texel of the canvas's dirty tiles
  (§10.1).

### 7.2 Pens

| Pen | Footprint at size s (texels) | Coverage |
|---|---|---|
| hard round | texels whose centre is within s / 2 of the sample's texel centre (s = 1: one texel) | 1 |
| square | the s × s texels with the sample's texel at `floor(s / 2)`, axis-aligned in texture space (pixel-exact) | 1 |
| soft round | within s / 2 | `1 − smoothstep(0.5, 1, d / (s / 2))` (a solid core, a soft rim) |
| eraser | the hard-round footprint | puts back the base image's texel (§7.4) with the pen's coverage; ignores the colour |
| fill | the whole region (the box face's cell, or the UV island) on LMB down; no drag | 1, flat colour (the colour's alpha) |

Size 1..256 texels: `[` / `]` step by 1 below 8, then ×1.25, and CTRL+wheel does the same.

### 7.3 UV islands (models)

When a model slot's canvas opens, its triangles (`MS_ModelSlotTriangles`, §8.1) are rasterised
into an **island mask** at the canvas size: an `int` per texel, with triangles joined into an
island when they share an edge with the same UVs (a union-find on vertex UV pairs) and each texel
labelled with the island of the triangle covering it. A texel covered by several islands is marked
**shared**. A stamp only touches texels of the hit triangle's island, so a big brush doesn't paint
the unrelated island next to it in the texture. Fill fills one island. Texels outside every
island (the empty space of the atlas) are never painted.

The mask is built on the CPU once per canvas open (a few ms for 20 k triangles at 1024²) and
rebuilt when the canvas is resized.

### 7.4 The base image and the eraser

The **base image** is a copy of the canvas when it was made (§3.3) or first opened in this
session (the PNG as read). The eraser restores it. Erasing paint from an earlier session isn't
possible, because the saved PNG is the new base. Open question (§16): keep a
`textures/paint/<name>.base.png` instead.

### 7.5 The eyedropper

**ALT+LMB** samples the **texture**, not the lit screen: the texel `MS_RaycastSurface` names, from
the canvas when the surface has one, else from a CPU copy of the material's albedo texture
(`MS_TextureRead`, cached by path until the next load) times the albedo colour, else the albedo
colour alone. The colour goes into the picker and the recent colours.

## 8. The runtime API (editor builds only)

These are declared in `runtime/monolith_scene.h`, in a new `MONOLITH_EDITOR` block ("Painting").
The game build and `--export` don't have them, and `tests/unit/export_strip_test.sh` checks that.

### 8.1 Surface picking

```c
typedef struct {
    MS_Hit hit;             // as MS_Raycast (the same object, point, normal and distance)
    int material;           // materials[] index the surface is drawn with; -1: a model slot's own material
    int face;               // boxes: 0..5 = +x -x +y -y +z -z (a turned box: its own axes); models: -1
    int slot;               // models: the file's material slot of the triangle; boxes: -1
    int mesh, triangle;     // models: the mesh of the file and the triangle in it; boxes: -1
    Vector2 faceUv;         // boxes: where on the face, 0..1 (before uv_faces); models: the texcoord
    Vector2 uv;             // the texture coordinate the albedo map is sampled at: after uv_faces,
                            // uv_scale and uv_offset, before the wrap
    Vector3 bary;           // models: barycentric coordinates of the hit in the triangle
} MS_SurfaceHit;

// MS_Raycast, plus where on the surface it hit. False when nothing is hit (out->hit.obj.kind
// MS_OBJ_NONE). A model without texture coordinates is still hit, with uv 0 0 and slot set.
bool MS_RaycastSurface(const MS_Scene *scene, Vector3 origin, Vector3 dir, float maxDist,
                       MS_SurfaceHit *out);

// The triangles of model file `file` drawn with its material slot `slot`: count, then each
// one's three texture coordinates and model-space positions. False when out of range or the file
// has no texture coordinates.
int MS_ModelSlotTriangleCount(const MS_Scene *scene, int file, int slot);
bool MS_ModelSlotTriangle(const MS_Scene *scene, int file, int slot, int k, Vector2 uv[3], Vector3 pos[3]);

// A material's face layout (uv_faces): false when it has none. cells[f] for the faces +x -x +y
// -y +z -z, the grid size, and the cell rectangle of face f in 0..1 texture space with the gutter
// inset (MS_FaceCellRect, shared by the meshes and the editor so they agree bit for bit)
bool MS_MaterialFaceLayout(const MS_Scene *scene, int material, unsigned char cells[6], int *cols, int *rows);
Rectangle MS_FaceCellRect(const unsigned char cells[6], int face, bool inset);
```

`ModelPick` gains texture coordinates per vertex (`Vector2 *uv`, NULL when the file has none) and
the mesh per triangle (`unsigned short *triMesh`, in BVH order). These live **in editor builds
only** (`#ifdef MONOLITH_EDITOR` around the fields and their copies in `BuildModelPick`), so the
game build's memory and picking stay as they are. `MS_Raycast` itself doesn't change.
`MS_RaycastModel` gets a static helper that also returns the triangle and the barycentric
coordinates. `MS_RaycastSurface` calls it.

### 8.2 Textures

```c
// The GPU texture the scene loaded for this package path (the first cache entry with it;
// id 0 = none). Model slots: MS_ModelSlotTexture.
Texture2D MS_TextureFind(const MS_Scene *scene, const char *rel);
// The albedo texture of model file `file`'s material slot `slot` as the file has it (id 0: none)
Texture2D MS_ModelSlotTexture(const MS_Scene *scene, int file, int slot);
// Reads a GPU texture's level 0 back as an RGBA8 image (UnloadImage it). False if id is 0.
bool MS_TextureRead(Texture2D tex, Image *out);

// Pixels for a package path that the next MS_EditSetText (and any upload from memory) uses
// instead of reading the file, also when the file doesn't exist yet (a new canvas). Copied
// (RGBA8). NULL / 0 x 0 forgets it. Kept across live edits, dropped by a load or reload from disk
// and by MS_Unload (the editor provides again after its rebase).
bool MS_TextureProvide(MS_Scene *scene, const char *rel, const Image *img);

// Uploads the rectangle `rect` (texels) of `img` into every cached texture with this package
// path, then regenerates their mipmaps. The image must have the texture's size and be RGBA8.
// MS_TextureGeneration + 1. A material of alpha_cutoff > 0 or transparency that uses the texture
// invalidates the sun shadow tiles and light shadows its objects reach (their shadow can change);
// nothing else is re-stamped. False if no texture has that path or the size differs.
bool MS_TextureUpdate(MS_Scene *scene, const char *rel, const Image *img, Rectangle rect);
// +1 on every MS_TextureUpdate (frame reuse keys on it, so a still camera sees the stroke)
unsigned int MS_TextureGeneration(const MS_Scene *scene);
```

### 8.3 Hot reload

`MS_EditMarkSaved` already sets the watch list to the current text's dependencies and their
mtimes, so a save that writes the PNGs **first**, then the map, then calls `MS_EditMarkSaved`
doesn't trigger a reload. Phase 1 checks two cases and fixes them if needed:

- a dependency that doesn't exist yet (a new canvas before its first save) must not make `MS_Poll`
  reload every frame or fail;
- a write of a PNG alone (auto save of a stroke, §11.1, with the map unchanged) must not reload.
  The editor calls `MS_EditMarkSaved` after it.

## 9. Live updates

### 9.1 A stroke

A stroke changes only pixels: dirty rectangle → `MS_TextureUpdate` once per frame per canvas
(the union of the frame's stamps, so one sub-image upload and one mipmap regeneration). No text,
no `s->version` bump: the selection indices, the caches and the merged meshes stay as they are.

### 9.2 Frame reuse

`FrameKey` (main.c) gains `textureGeneration` (`MS_TextureGeneration`), so a still camera with
frame reuse on shows the stroke on the next frame.

### 9.3 `uv_faces` on instanced cubes and the preview

Instanced materials draw R3D's unit cube. A material with `uv_faces` and `instanced on` draws a
unit cube of its own, made by `FaceLayoutCube` (the cube's vertices with the cell transform of
§4.1 applied to the UVs), one per distinct layout, freed with the scene. The transform preview's
cubes (`MS_EditPreviewBegin`) use the same per-layout cube for those materials. Materials without
the key draw `p->cube` exactly as now.

### 9.4 Adding a material live

A new material changes the look hash (`lookHash`), so `MS_EditSetText` would upload everything
from memory (~520 ms on the largest map in `maps/`). Phase 1 adds one cheap case. When the new
text's materials are the old ones, **in the same order with the same definitions**, plus
materials **appended** at the end, and nothing else in the look hash changed:

- the old materials' GPU state is moved over as now;
- each new material is built (`BuildMaterial`, its textures through `GetTexture`, which consults
  the provided images, §8.2);
- `materials[]`, `matDefs[]`, `inst[]` and `instCount[]` grow;
- box and model ranges are rebuilt as an object edit does (the painted object's boxes moved to the
  new material: changed boxes, dirty regions).

For this, the parser records a look hash **per material** (`matLookHash[]`) and a look hash of the
rest. A text where a material was inserted, removed or changed is a full upload as now. The same
cheap path covers `EDIT_MATERIAL_SET` on a material that only changes `uv_faces`, `filter` or
`albedo_map`: that material's ranges are rebuilt (their UVs or textures change), and every other
GPU resource is kept.

### 9.5 What a stroke must not do

A stroke must not allocate per frame. The scratch (subdivision points, the dirty rectangle, the
stroke mask's tiles) grows to the largest size seen. The canvas and its undo tiles are allocated
on LMB down and when a tile is first touched (an input event). This follows the memory rules of
the `engine-code` skill.

## 10. Undo

### 10.1 Tiles

A canvas is divided into **64 × 64 texel tiles**. The first time a stroke touches a tile, the
tile's pixels are copied into the step (`PaintTile { canvas, tx, ty, pixels }`). Undo **swaps**
each saved tile with the canvas's current pixels and uploads it. Redo swaps again with the same
buffer. One copy per touched tile serves both directions.

### 10.2 One history

The draw mode shares the edit mode's `MapEditor`: one history, ordered across text edits and
strokes, CTRL+Z / CTRL+SHIFT+Z (CTRL+Y) in both modes. `EditStep` gains:

```c
struct PaintStep *paint;    // NULL for text edits; the strokes' tiles and their canvas (drawmode.c)
```

- A stroke on an existing canvas: a step with no hunks (`hashBefore == hashAfter`) and `paint`
  set. Undo / redo swap its tiles and call no `MS_EditSetText`.
- The first stroke on an object (§3.1): **one** step with the text hunks (material, `use` /
  `material.N`, id, split) **and** `paint` (canvas creation plus the stroke's tiles). Undo removes
  the text live, and the canvas leaves `MS_TextureProvide` but stays in memory for redo.
- Layout changes and resizes (§5.2, §5.3): one step with the hunks and the whole old image.
- `mapedit.c` calls back into the draw mode for `paint` steps (`MapEditPaintHooks`: undo, redo,
  free, rebase), so `mapedit.c` doesn't depend on canvases.
- **Memory cap:** paint tiles in the history are capped at 512 MiB. Beyond that the oldest steps
  are dropped from the bottom of the undo stack (text steps with them, so the order stays right),
  and the HUD says "oldest N undo steps dropped (paint history over 512 MiB)". The save point
  goes with them: those steps count as saved or not as before.

### 10.3 New edit ops (`mapedit.c`)

```c
EDIT_PAINT_MATERIAL,    // one object (box, or model + slot) gets a new material: matName, matKeys
                        // (the copy's keys, written as given); boxes: split out of a repeat, wrapped
                        // in use "matName" ... use "<its material>"; models: material.N; id added
EDIT_MATERIAL_SET,      // material matName: set the keys in matKeys (replacing the same keys,
                        // adding the others) on its first line; "-key" removes a key
...
const char *matName;    // EDIT_PAINT_MATERIAL, EDIT_MATERIAL_SET
const char *matKeys;    // e.g. "albedo 255 255 255 albedo_map \"textures/paint/paint_12.png\" uv_faces 0 1 2 3 4 5"
int slot;               // EDIT_PAINT_MATERIAL on a model: the slot (-1: every slot, `material`)
```

Edit-script syntax (`--apply-edits` and `--edit-script`), so phase 2 can test the text without
the painting:

```
paint-material "<name>" <keys...>         on the selection (one object)
paint-material "<name>" slot <N> <keys...>
material-set "<name>" <keys...>
```

A rebase re-applies them like any op. Its selection is resolved by entity id. The material name is
kept; if a material of that name exists on disk with other keys, the step fails ("material
paint_12 exists on disk").

## 11. Saving

### 11.1 CTRL+S and auto save

CTRL+S (in draw or edit mode) writes:

1. every canvas with unsaved paint to its PNG (`PngWrite`, RGBA8; folder `textures/paint/`
   created when missing);
2. the working text, as now (`MapEditSave`);
3. the licence records of new or derived PNGs (§11.2);
4. then `MS_EditMarkSaved`.

A canvas is unsaved when its pixels differ from what its PNG has (a hash of the pixels as last
written or read). The HUD's "N unsaved edits" counts paint steps like text steps.

In **auto save** mode (`edit_save = auto`) every stroke writes its canvas at LMB up, and every
first stroke also writes the map. Each is followed by `MS_EditMarkSaved`.

### 11.2 Licences (`ASSETS.md`, sidecars)

Every PNG under `textures/paint/` gets an `ASSETS.md` row and a sidecar
(`textures/paint/<name>.png.asset.md`), written on the save that first writes it, in the format
the `assets-and-licences` skill describes (`tools/record_asset.py`'s columns; the draw mode writes
the same text from C):

- **A new canvas from a flat colour**: "made in monolith (painted by hand)", licence **CC0 1.0**,
  author the package (the developer can change it).
- **Derived** from a texture or a model's embedded texture: the source's licence and attribution
  are kept ("derived from `textures/x.png` (CC-BY 4.0, …), painted in monolith"). A source without
  an `ASSETS.md` row is refused at the first stroke ("textures/x.png has no licence row in
  ASSETS.md"), as the picker refuses unrecorded models.
- **Shared mode** on a recorded file: its row gains "modified in monolith" once.

### 11.3 Closing the game with unsaved paint

As for unsaved edits (`docs/live-editing.md` §4.2), the working text goes to
`<settings folder>/unsaved/<map>-<YYYYMMDD-HHMMSS>.map`, and each unsaved canvas to
`<settings folder>/unsaved/<map>-<YYYYMMDD-HHMMSS>/<package path>`. The package isn't touched.
Automated runs only log that the paint was dropped.

## 12. Conflicts

- **The map file or a painted PNG changes on disk** while paint is unsaved. `MS_Poll` reloads,
  and `EditModeSync` rebases the text steps as now. Each canvas then goes through its paint rebase
  hook:
  - Its material still names the same PNG, and the PNG on disk is unchanged: the canvas's pixels
    are provided again (`MS_TextureProvide`) and go live with the rebase's `MS_EditSetText`.
  - The PNG changed on disk (a request repainted it): its unsaved **strokes are replayed** onto the
    new image. Each paint step keeps its stroke as input (pen, size, colour, the samples in UV
    space), so the replay works like a rebase of ops. They are reported ("re-applied 3 strokes onto
    textures/paint/paint_12.png, which changed on disk").
  - The material or the object is gone: the steps go to `failed[]` in red, as text steps do.
- **A request is submitted with unsaved paint**: refused until saved, as with unsaved edits
  ("N unsaved edits: CTRL+S saves and sends"). A request session never sees in-memory paint.
- **A stroke while a request runs on the map**: today's warning (the first stroke after the last
  save warns, the same key again within 3 s paints anyway).
- **A request edits a painted object's line**: the line's id keeps its entity. The canvas follows
  the material, not the line, so it stays.

## 13. Draw mode

### 13.1 Keys

Draw mode (**B** in spectator mode) works like edit mode (`docs/live-editing.md` §6): the cursor is
free, **RMB held** flies (mouse look, WASD / E / Q / SPACE, wheel speed), and an RMB click does
nothing. B, M, V and C switch between the modes. Leaving draw mode keeps unsaved paint (it is
live), with the same first-press warning as edit mode.

| Key | In draw mode |
|---|---|
| LMB (drag) | paint with the pen; fill: fill the face / island |
| ALT+LMB | eyedropper (the texture) |
| `[` / `]`, CTRL+wheel | brush size (§7.2) |
| SHIFT+wheel | brush alpha ±8 |
| 1 … 5 | pen: hard round, square, soft round, eraser, fill (the effect toggles 1–6 wait while draw mode is on) |
| X | swap the colour with the last one |
| H | shared switch (paint the material's own texture, §3.2) |
| CTRL+Z / CTRL+SHIFT+Z (CTRL+Y) | undo / redo (shared with edit mode) |
| CTRL+S | save |
| TAB | hide / show the panel |
| B / ESC | leave draw mode |

The keybindings panel (`keyspanel.c`) gets a *Draw mode* section.

### 13.2 The panel (`src/drawpanel.c`)

The panel is docked on the left edge, below the HUD lines, 300 px wide, and uses the `ui.h` look
(`UI_PANEL`, `UI_LINE` border, `UI_L` bold title in `UI_ACCENT`). It is mouse-driven. While the
cursor is over it, LMB is the panel's and the scene isn't painted. From top to bottom:

1. **Target**: what the cursor is over ("box line 120, face +z, paint_12.png 768×512", "model
   barrel.glb slot 1, own texture 1024²", or the refusal in `UI_ERROR`) and the shared switch.
2. **Pen**: five buttons (icons drawn with shapes), the size slider + number field (1–256
   texels), and for soft round the hardness.
3. **Colour**: R, G, B and A sliders (0–255), each with a number field; a swatch (on a checker
   for alpha); a hex field (`#RRGGBB` or `#RRGGBBAA`); 8 recent colours (a click picks); the
   eyedropper hint.
4. **Canvas**: the density slider + field (texels / m, 8–512, default 64), *Resize to density*
   (§5.3), the texel size of the face under the cursor (mm × mm), and the **pixel look** switch
   (`filter nearest`).
5. **Faces** (boxes): the six faces +x −x +y −y +z −z, each with a cell number button (a click
   cycles 0..5). An unused number in between is closed up on apply. Below it: *Apply* (§5.2),
   *All separate* (0 1 2 3 4 5) and *Sides together* (0 0 1 2 0 0).

New `ui.h` widgets (`src/ui.c`), all immediate-mode, one state struct per drag or edit:

```c
// A horizontal slider of value in lo..hi at r; true while it changed this frame
bool UiSlider(UiDrag *drag, int id, Rectangle r, float *value, float lo, float hi, float step);
// A number field: click to type, ENTER / clicking elsewhere applies, ESC cancels, the wheel steps
bool UiNumberField(UiEdit *edit, int id, Rectangle r, float *value, float lo, float hi, float step, int decimals);
// A text field (the hex input): the same editing rules, true when ENTER applied it
bool UiTextField(UiEdit *edit, int id, Rectangle r, char *text, int textSize);
// A colour swatch (alpha on a checker); true when clicked
bool UiSwatch(Rectangle r, Color c, bool selected);
// A button and a toggle in the panel's look; true when clicked
bool UiButton(Rectangle r, const char *label, bool active);
// typedef struct { int active; float grab; } UiDrag;  typedef struct { int active; char buf[32]; int len, cursor; } UiEdit;
```

### 13.3 The brush cursor

Under the cursor, the pen's **texel footprint** is drawn on the surface: the outline of the
footprint's texels around the hit texel, mapped back to the world through the hit face's (or
triangle's) UV → position mapping (a box face: exact; a model: the triangle's affine map, clipped
to the triangle's plane). It is drawn as a ring for round pens and a quad for square, depth-tested
with the designer overlay's bias. At size 1 it is the texel's own square, so pixel art shows
which texel is next. Fill outlines the face. The HUD line:
`DRAW  hard round 4 px  #C83232 a255  | paint_12.png 768×512 face +z  | 2 unsaved edits`.

## 14. Verification flags

- `--paint-script <file>`: strokes applied **live**, after `--edit-script` and before `--shot` or
  any `--dump-*` (after 10 frames with `--shot`, as `--edit-script`), or written to the package
  with `--apply-edits` semantics (`save` in the script). One command per line:

  ```
  pen <round|square|soft|eraser|fill>      size <n>     color <r> <g> <b> [<a>]     density <n>
  shared <on|off>                          faces <a b c d e f>   (EDIT_MATERIAL_SET, §5.2)
  stroke px <x0> <y0> <x1> <y1> [...]      screen pixels of the shot's camera, interpolated as LMB drags
  stroke uv <entity> <face|slot> <u0> <v0> [<u1> <v1> ...]   texture space of that object's surface
  dab px <x> <y>   /   dab uv <entity> <face|slot> <u> <v>     one stamp (fill: fills there)
  pick px <x> <y>                          the eyedropper (prints the colour)
  undo / redo / save / print               (print: the canvases, their sizes, unsaved state, pixel hashes)
  ```

  Entities are written as `select` writes them (`id 12:0`, `entity a7d95ba7:0`).
- `--draw-mode`: open draw mode (with `--shot --shot-hud`: the HUD line and the brush cursor at
  `--cursor <x> <y>`).
- `--draw-panel [section]`: draw mode with the panel, for HUD shots.
- `--input-script` gains `b` and mouse `down` / `up` on LMB (for drags through the real input
  path).

All of these are windowless with `--shot` / `--dump-*` / `--apply-edits`, and refused otherwise.

## 15. Phases and ownership

| Phase | Who | Files | Done when |
|---|---|---|---|
| 0 | lead | this note; headers and stubs (`monolith_scene.h`, `drawmode.h/.c`, `drawpanel.h/.c`, `mapedit.h`, `ui.h`, flags in `main.c`) | both builds 0 warnings with the stubs |
| 1 | agent A | `runtime/monolith_scene.c/.h`, `docs/compiled-map.md`, `src/export.c` if needed, `FrameKey` in `main.c` | §4, §8, §9.2–9.4; every map unchanged; a scratch map with an atlas box, a turned atlas box and a model: hits give the expected uv / face, known texels in shots; compiled round trip; `examples/build.sh test`; leak check |
| 3a | agent B (beside A) | `src/ui.c/.h` widgets, `src/drawpanel.c` laid out on dummy state | `--draw-panel` shots at 1280×720 and 1920×1080 |
| 2 | agent C (after A) | `src/drawmode.c`, `src/mapedit.c/.h`, the draw-mode wiring in `main.c`, `designer.c`, `editmode.c` | §3, §5–7, §10–12; `--paint-script` shots of each pen at size 1 / 16, alpha 255 / 128, eraser, fill; `--apply-edits` diff minimal; undo byte-identical PNG; hot-reload round trip; leak check |
| 3b | agent D (after B, C) | `drawpanel.c` wiring, the brush cursor, HUD, `keyspanel.c` | panel shots with real state; hand-test steps |
| 4 | verifier, then lead | docs | every claim re-checked; docs updated |

`monolith_scene.c` belongs to A only, and `mapedit.c` to C only. Nothing here needs a benchmark
unless phase 1 changes how boxes merge for existing materials. If it does, one A/B check goes
through the lead.

## 16. Open questions (for the final summary)

- **Non-square faces** stretch their texels (§5.1). An alternative is cells sized per face
  (a packed atlas that depends on the box's size, re-laid out when it is resized).
- **Brushes across box edges** (§7.1): a stamp is clipped to its face. Projecting it across the
  edge is possible later (stamp in 3D and splat into every face it reaches).
- **Materials with a surface shader**: boxes are baked (§3.6, with the losses its implementation
  notes list); model slots are refused (§3.5).
- **The eraser** restores only this session's base (§7.4). Keeping a `.base.png` beside each canvas
  would allow erasing older paint.
- **Licence of a new canvas**: CC0 by default (§11.2).

## Implementation notes

(Appended per stream as it merges.)

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
wiring in `src/main.c`, `tools/leakcheck.sh`), and where it differs from the sections above.

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
  `tools/leakcheck.sh` gained `draw-cursor-fill`, `draw-cursor-model` and `draw-panel-canvas`, and the
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
mesh fix (12 runs, 0 differing), builds, unit tests, `tools/leakcheck.sh` and `--asan`, and painted
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
`MS_BoxDrawOf` in the runtime, `tools/leakcheck.sh`), and where it differs from §3.6.

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
- `tools/leakcheck.sh` gained `paint-bake` (the awning: bake, undo, redo, save, a model stroke),
  `paint-bake-rot` (the scratch `tests/rotated` with shaders, `--apply-edits`) and `paint-bake-input`
  (a click and a drag through the input shim, undo, a second bake); `paint-script`'s last command is now
  shared mode on the awning (refused, exit 1).

**Open**: specular and emission of a shader (an R3D view of the ORM target's alpha and of the emission
would need an R3D patch); the density for pixel-art shaders (a multiple of their grid); a multi-box
statement baked from one box; face groups on a baked canvas (relaying out the ORM / normal maps too);
the ~0.1–0.3 s hitch on the click.

