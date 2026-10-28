# Live editing

How manual edits (edit mode, **M**) apply to the loaded scene without a reload, how they reach
the map file, and the editor tools built on them: the transform modal (**G** / **R** / **S**; rotate is **ALT+R** since §11),
the id panel, the render inspector and the model picker. This is the design the development
round worked from. Implementation notes follow at the end, one per stream as it merged.

## 1. Why

Before this round every edit key ran one `MapEditApply`: a text patch written to the map file,
then `MS_Reload`, which re-parses the map, reloads every model file and texture, compiles every
shader, uploads everything again and re-renders every static cache. On the largest map in
`maps/` (52 244 boxes) that is ~520 ms of load (textures 359, shaders 39, box meshes 37,
parse 24, hidden faces 12, collider BVH 8) plus a 40 ms first frame and the sun tile again. Fine
placement by arrow presses, let alone a drag that follows the mouse, is unusable at that cost.

Of those 520 ms only the parse, the collider BVH, the entities and the box cells are needed to
know what an edit changed (~40 ms on that map, a few ms on the others). Everything else can be
kept: no edit made in edit mode changes a material, a shader, a texture, the sky or the
environment.

## 2. The working copy

**The working copy is map text.** The editor keeps the map's text in memory (`MapEditor.work`).
Every edit is still made by `mapedit.c` exactly as before (the same minimal hunks: numbers on a
line, a deleted line, a split `repeat` line, inserted lines), but against the working text, not
the file. The new text then goes to the runtime with `MS_EditSetText`, which parses it on the
CPU and swaps in only what changed (§3). The file on disk is written only on save (§4).

This replaces the task prompt's sketch of "a list of edit ops applied over the loaded scene"
(the developer's brief left the representation open). Reasons:

- **Save + reload is identical to the live scene by construction.** The live scene *is* the
  parse of the exact bytes CTRL+S writes; the incremental GPU update only decides which
  resources can be kept, and anything it rebuilds is built by the same functions a load uses.
  An op list applied to scene arrays would have to re-implement the parser's semantics
  (`repeat` splits, `from k`, float rounding of written decimals, map order of new lines,
  `use` wrapping of added models) and prove it equal for every op.
- **Object order stays the map order.** Boxes are drawn and hidden-face-tested in box order,
  and `MS_Raycast` breaks ties by box index. A duplicate or an added model is a new line after
  its source line, so its objects sit in the middle of the arrays. A parse gets this right for
  free.
- **Requests, hot reload and the AI keep reading text.** The file stays the source of truth;
  the working copy is the file plus the hunks of the unsaved steps.

Each step still records its **op and its selection as entity ids** (`EditStep.op`,
`selBefore`), so the step can be applied again to a different text (rebase, §5).

### 2.1 Undo / redo

Unchanged in shape: an `EditStep` is a list of hunks (offset, old bytes, new bytes) between two
texts, now two *working* texts. Undo applies the inverse hunks to the working text and calls
`MS_EditSetText`; redo the hunks. Every step is live, so undo / redo cost the same as an edit.
The undo stack is no longer tied to the file on disk: it survives saves, and the HUD shows
where the save point is ("3 unsaved edits"; undo past the save point makes the working copy
differ from the file again: "1 unsaved edit").

A transform modal (§7) is **one step**: while dragging, the scene shows a preview (§3.4) and
nothing goes onto the stack; on confirm, one op with the final values goes through
`MapEditApply` like an arrow press.

### 2.2 The step model

```
MapEditor
  work, workSize            the working text (heap)
  diskHash                  FNV-1a 64 of the map file as last read or written by us
  undo[], redo[]            EditStep: hunks + hashes + op + selection ids + what
  savedAt                   undo depth that matches the file (-1: none does)
  failed[]                  steps that did not rebase (§5): shown in red, kept until dismissed
```

`unsaved = (savedAt == undoCount && workHash == diskHash) ? 0 : |undoCount - savedAt|`
(`-1`: every undo step counts). The HUD shows it whenever it is not 0, in every spectator mode.

## 3. The runtime's live-edit API (editor builds only)

Declared in `runtime/monolith_scene.h` in a `MONOLITH_EDITOR` block ("Live edits"). The game
build and `--export` don't have it; `tests/unit/export_strip_test.sh` checks that.

```c
MS_ReloadResult MS_EditSetText(MS_Scene *s, const char *text, int size, MS_EditStats *stats,
                               char *err, int errSize);
void MS_EditMarkSaved(MS_Scene *s);
bool MS_EditPreviewBegin(MS_Scene *s, const MS_ObjRef *objs, int count);
bool MS_EditPreviewSet(MS_Scene *s, const MS_Transform *transforms);
void MS_EditPreviewEnd(MS_Scene *s);
```

### 3.1 `MS_EditSetText`: parse, diff, keep, rebuild

1. **Parse** the text into a new `SceneData` (CPU only: `ParseScene`, `FinishParse`). Model
   files whose path the current load has are **moved over** (`R3D_Model`, `ModelPick`,
   bounds); a new path is loaded (an added model). `ResolveModels`, `FinishGeometry`,
   `BuildColliderBvh` and `BuildEntities` run as in a load.
2. **Same look?** The parser records a hash of every statement that is not an object, `group`,
   `use`, `viewpoint`, `spawn` or `id` line (materials, `env`, `sky`, `sun`, `shadow`, cache
   hints, `monolith`). When it differs from the current load's, the text changed something
   edit mode never changes: the new `SceneData` is uploaded in full (`UploadGpu`; still no
   read of the map file), i.e. a reload from memory. `stats->full` says so.
3. **Move over** the GPU resources that don't depend on objects: textures, shader files and
   aliases, materials, sky, ambient map. Light shadow maps go to the light with the same
   content (`R3D_Light` bytes, shadow, clip); a light without a match gets a new one.
4. **Boxes.** Build the new cell items and ranges (`MakeCellItem`, sort, `AddRanges`). Hidden
   faces: a box's flags depend only on the boxes that touch it, so they are computed for the
   boxes whose grown bounds overlap a dirty box (§3.2) and copied from the old load for the rest
   (matched by box content). A merged range whose key (material, big, cell) existed before with
   the same boxes in the same order and the same hidden flags **keeps its mesh**; every other
   merged range is built with `BuildBoxMesh`. Instanced materials whose box list changed get a
   new instance buffer. Counts and triangle totals as in a load.
5. **Models.** `BuildModelGroups` again (instance buffers only; cheap), with the moved-over files.
6. **Dirty regions** (§3.2) go to the caches (§3.3).
7. **Swap** like a load (new before old: R3D caches instance buffer names), `s->version + 1`,
   `Publish`, `s->hash` = the text's hash. `s->loadVersion` and the hot-reload state (`triedHash`,
   the watch list) are *not* touched: the file on disk didn't change.

On any failure (a parse error, out of memory) the scene stays as it was and `err` says why
(`file:line: message`, the line of the working text). `mapedit.c` already refuses an edit whose
text would not load; this is the same check, without the write / revert dance on disk.

`MS_EditStats` reports what happened (objects changed, ranges rebuilt / kept, tiles and caches
dirtied, `full`, and where the time went: parse, models, BVH, entities, hidden faces, meshes,
upload). `editmode.c` logs one line per edit:
`monolith: live edit 38.4 ms (parse 23.9, bvh 8.1, hidden 1.2, meshes 0.9 (3 of 324 rebuilt), ...)`.

### 3.2 What changed: dirty regions, not a version bump

The diff is **by content, not by entity id**: a box is unchanged when an identical box (bounds,
material) is in the old load; a model when an instance with the same file, transform, materials,
collide and shadow flag is; a light when an identical light is (multiset matches through a hash
of the content). Entity ids are no good here: the pieces of a split `repeat` line after the
first get new implicit ids although their boxes didn't move.

Each unmatched old object and each unmatched new object adds its world bounds to the dirty list
(`MS_Private.dirty[]`, editor-only). What each kind needs rebuilt:

| Object | Rebuilt |
|---|---|
| box | the merged meshes of its material's cells it leaves and enters, the cells of boxes whose hidden faces it changes (any material: occluders hide faces of other materials), its material's instance buffer if instanced, the collider BVH, the entity components; sun tiles, point / spot shadows, fog cache and AO volume where its bounds reach |
| model | its group's instance buffer (all model groups are rebuilt: cheap), world bounds, colliders and the collider BVH, entity components; sun tiles, light shadows and the fog cache where its bounds reach (the AO volume holds boxes only) |
| light | its `R3D_Light` (pushed every frame anyway) and its own shadow map (drawn again); nothing static |

### 3.3 The static caches

Today every cache keys on `s->version` and redraws everything when it changes. After a live
edit (which also bumps `s->version`, §3.5) an editor-only step **re-stamps** each cache with the
new version after invalidating only what the dirty regions reach, so the shared code in the
game build stays as it is:

- **Sun shadow tiles** (`SunShadowTiles`): a tile is invalidated when a dirty box, projected into
  light space, overlaps the tile's ortho square (centre ± radius in light-space x / y). Depth is
  ignored: depth clamp flattens casters toward the sun onto the near plane, so anything in the
  column counts. An invalid current tile is drawn again on the next frame by the existing LRU
  path; the others when the eye reaches them.
- **Camera-fitted sun map** (`SunShadowCache`, `MS_SHADOW_CAMERA`): invalid when a dirty box
  overlaps its light-space square (its `viewProj`).
- **Point / spot light shadows**: `lightShadowDrawn[i] = false` for each shadowed light whose
  range sphere overlaps a dirty box (and for every light that moved or changed).
- **Fog sun visibility** (`fogCacheVoxel > 0`): rebaked when a dirty box overlaps the bake's
  light-space box (`SunShadowBoxMatrix` of the fog region). R3D's bake is whole-region; phase 1
  measures it, and only if a rebake on the largest map costs more than ~20 ms does a minimal
  R3D patch add a sub-region bake (`patches.md`).
- **AO volume** (`aoVolumeVoxel > 0`): the voxels within `AO_DIST_MAX` of a dirty box are
  recomputed on the CPU from every box near them (collider BVH query; the same per-voxel
  minimum as the full bake, so identical bytes); the editor build keeps the byte grid for that.
  The grid is uploaded again whole (`R3D_SetWorldOcclusion`, a few MB; no R3D change).
- **Regions follow the content.** The fog region and the AO region come from `ContentBounds` /
  `PlayCenter`, the tile layout from `ShadowHome`. When an edit changes one of them, a reload
  would lay that cache out anew, so the live edit does too: that cache is redone in full.

Identity: after any sequence of live edits every cache holds what a fresh load of the same text
would draw into it, because a tile / voxel / map is either untouched by every dirty box (so its
content is a function of unchanged geometry) or drawn again. The `--edit-script` test (§9)
checks it with pixel diffs.

### 3.4 Preview (the transform modal)

While a G / R / S modal runs, the scene follows the mouse every frame without the text:

- `MS_EditPreviewBegin(objs)`: the selected boxes leave their merged ranges and instanced
  buffers (those ranges are rebuilt without them, with hidden faces recomputed nearby) and are
  drawn as instanced cubes of their materials from one small preview buffer. Models and lights
  keep their slots. Returns false (preview refused, the modal still works with outlines only)
  when out of memory.
- `MS_EditPreviewSet(transforms)`: absolute transforms for the objects of Begin (`MS_Transform`:
  boxes centre + size, models position / rotation / scale, lights position). Updates boxes,
  colliders (BVH refit), model matrices and bounds, light positions, entity transforms, the
  preview buffer; invalidates the sun tile(s) and light shadows that the old and new bounds
  reach, so shadows follow the drag (one tile render a frame, ~1 ms on the largest map). The fog
  cache and the AO volume are left alone during a drag and catch up on confirm.
- `MS_EditPreviewEnd()`: puts everything back as it was at Begin (cancel). On confirm the editor
  calls `MS_EditSetText` with the edited text instead, which replaces the preview state anyway.

Preview steps bump `s->version` (the frame changes), but the object indices stay, so the
designer keeps its selection by adopting the version (`DesignerAdoptVersion`) instead of
remapping by geometry.

### 3.5 What `scene->version` means now

- `s->version`: +1 whenever the published contents change: a load, a reload, a live edit, a
  preview step. `FrameKey`, frame reuse, the designer's selection indices and the bundle's
  `mapVersion` key on it, as before.
- `s->loadVersion` (new, editor-only): +1 on a load or reload from disk only. What must reset
  only when the map is read again keys on it: the model picker's thumbnails, the "first frames
  after the load" log, the undo history's rebase (§5).
- The caches key on `s->version` in shared code and are re-stamped by live edits (§3.3).

### 3.6 Draw ranges and the draw filter (the render inspector)

```c
int MS_DrawRangeCount(const MS_Scene *s);
bool MS_GetDrawRange(const MS_Scene *s, int r, MS_DrawRangeInfo *out);
void MS_SetDrawFilter(MS_Scene *s, const bool *hidden, int count);
```

Ranges are the runtime's real draws, box ranges first, then model ranges: a merged cell mesh
of one material, an instanced cube range of one material, or an instanced model group. The
info carries kind, material, shader, model file, objects (box or model instance indices),
bounds, triangles per pass and whether it casts shadows. The filter hides ranges from every
pass (camera and shadows). A filter change invalidates the sun tiles and the light shadows (so a
hidden object drops its shadow too); the fog cache and the AO volume keep the whole scene.
`MS_SetDrawFilter(s, NULL, 0)` turns it off; with it off `MS_DrawGeometry` costs one branch per
frame more. Range indices change with every live edit: the inspector rebuilds its filter when
`s->version` changes.

## 4. Saving

**CTRL+S** writes the working text to the map file (in place, as `mapedit.c` writes today), then
`MS_EditMarkSaved`: the runtime's hot-reload state takes the written bytes as loaded
(`triedHash`, watch mtimes, `s->loadVersion` unchanged), so `MS_Poll` doesn't reload the file it
just saw written. `diskHash` = the working text's hash, `savedAt = undoCount`.

The bytes written differ from the file before only by the hunks of the unsaved steps, i.e.
exactly the minimal hunks `MapEditApply` produced one by one (the working text started as the
file's bytes). Saving and then reloading gives the live scene again (§9 proves it per map).

### 4.1 Save mode (an editor setting)

The developer chose to have both:

- **Manual** (default, `edit_save = manual`): the working copy; CTRL+S saves; the HUD shows "N
  unsaved edits"; leaving edit mode with unsaved edits asks first (§6).
- **Auto** (`edit_save = auto`): every confirmed edit, undo and redo is applied live *and*
  written to the file at once (CTRL+S has nothing to do), followed by `MS_EditMarkSaved`, so the
  hot reload sees no change. The cost is the write; the scene never reloads for it.

The setting lives in the editor's settings (`settings.c`: file key `edit_save`, a row in a new
**Editor** group of the F2 panel, `GfxSettings.editSave`); `--edit-save <manual|auto>` overrides
it for one run. It is not part of `MS_Quality` or the runtime, so the export never sees it.

### 4.2 Closing the game with unsaved edits

raylib can't cancel a window close, so the working text is written to
`<settings folder>/unsaved/<map name>-<YYYYMMDD-HHMMSS>.map` (the settings file's folder:
`~/.config/monolith/` on Linux) and the log says where. The map file is not touched.

## 5. Conflicts

**The file changes on disk while edits are unsaved** (a request finished, an editor saved it,
a `git checkout`). `MS_Poll` sees it as today and reloads the package from disk (`loadVersion
+ 1`). Then `EditModeSync` **rebases**: the working text becomes the new file, and each unsaved
step, oldest first, is applied again: its op, to its selection resolved by entity id in the
current scene. A step applies when every entity of its selection still exists and the op
succeeds; then it gets new hunks and goes onto the new undo stack. A step that doesn't apply (an
entity is gone, a line is no longer the shape the op needs) goes to `failed[]` and is listed in
red in the HUD ("could not re-apply: moved 2 objects +0.5 m x (entity 7:0 is gone)"), until
dismissed (CTRL+SHIFT+DELETE clears the list). The rebased steps' undo history before the save
point is dropped (it described the old file). The result goes live with one `MS_EditSetText`.
Ids make this reliable: a line edited by hand already gets an `id N` prefix (`mapedit.c`), so
its objects keep their entity ids through any edit a request makes to other lines.

With no unsaved edits, an outside change only clears the undo history, as today.

**A request is submitted with unsaved edits.** A bundle's line numbers, object indices and map
hash describe the loaded scene, i.e. the working copy, while the request's session reads the
file. So a request needs the file to be the working copy: ENTER in the prompt box with unsaved
edits shows "N unsaved edits: CTRL+S saves and sends, ESC back" and sends nothing until saved.
In auto save mode there is never anything unsaved.

**An edit while a request runs on the map** keeps today's rule (the first key warns, the same
key again within 3 s edits anyway). It now only warns on the first edit after the last save,
since unsaved edits don't touch the file.

## 6. Edit mode keys (decided: camera flies only while RMB is held)

Edit mode (**M**) frees the letter keys by moving flight behind the right mouse button, as in
Blender's walk mode or Unreal: **RMB held** = mouse look + WASD / E / Q / SPACE flying, cursor
hidden. Without RMB the **cursor is free**: hovering outlines what is under the cursor (not the
crosshair), and LMB / SHIFT+LMB / CTRL+LMB select there (`DesignerPickAt`). An RMB **click**
(released within 0.25 s, the mouse moved less than 4 px) clears the selection as before. Outside
edit mode nothing changes.

| Key | In edit mode |
|---|---|
| RMB held | fly: mouse look, WASD / E / Q / SPACE, wheel speed, SHIFT ×4, ALT ×0.25 |
| LMB, SHIFT+LMB, CTRL+LMB | select under the cursor (add / remove, group) |
| RMB click | clear the selection |
| **G** / **ALT+R** / **S** | grab / rotate / scale the selection (the modal, §7; rotate moved from R to ALT+R in the free-rotation round, §11.10) |
| **R** / **SHIFT+R** | turn 90° / −90° about the vertical, each object about its own centre / origin (§11.10) |
| **ALT+SHIFT+R** | reset the rotation (`EDIT_SET_ROTATION` 0 0 0, §11.9) |
| LMB on the rotate gizmo, **L**, **SHIFT+N** | turn by a ring / the trackball; gizmo axes global / local; the angle snap step (§11.10) |
| arrows, PGUP / PGDN | move one snap step (as before; CTRL: exactly one step, no grid) |
| N | snap step 0.1 / 0.25 / 0.5 / 1 / 2 m |
| DELETE | remove |
| CTRL+D | duplicate |
| I | the model picker (§8.3); ENTER places the model at the cursor |
| CTRL+I | give the selection's line an id (as before) |
| CTRL+S | save (manual save mode) |
| CTRL+Z / CTRL+SHIFT+Z (CTRL+Y) | undo / redo (live) |
| F5 | the id panel (§8.1), in any spectator mode |
| M / ESC | leave edit mode; with unsaved edits the first press says "N unsaved edits: CTRL+S saves, M again leaves them unsaved (still live)" |

Spectator G (move the player here) and R (fly back to the last request) still work outside edit
mode. V and C still leave edit mode.

## 7. The transform modal (`src/transform.c`)

### 7.1 States

```
IDLE --G/ALT+R/S (selection not empty; a gizmo ring / the trackball: §11.10)--> RUNNING(kind)
RUNNING --mouse move--> RUNNING (value from the mouse, unless a number is typed)
RUNNING --X/Y/Z--> axis lock (again: local axes for models; a third time: no lock)
RUNNING --SHIFT+X/Y/Z--> plane lock (the two other axes; again: local; a third time: none)
RUNNING --digit . - BACKSPACE--> typed value (overrides the mouse; empty = the mouse again)
RUNNING --G/R/S--> switch kind (keeps nothing; starts from the original transforms)
RUNNING --LMB / ENTER--> commit: one op through MapEditApply, MS_EditSetText; IDLE
RUNNING --RMB / ESC--> cancel: MS_EditPreviewEnd; IDLE (everything back exactly)
```

While running, the camera is frozen, the cursor is hidden and the raw mouse delta drives the
value. Every frame the modal computes each selected object's transform from its transform at
the start, calls `MS_EditPreviewSet`, and draws the HUD line and the axis guide (a long line
through the pivot along each locked axis, in the axis colour: X red, Y green, Z blue).

### 7.2 Values

- **Grab:** the cursor's motion projected onto the plane through the pivot facing the camera
  (no lock), onto the locked axis (closest point between the mouse ray and the axis line) or
  onto the locked plane. Typed: metres along the locked axis (no lock: along the view's right; a plane lock: along the first axis of the plane, e.g. x for SHIFT+Y).
  Three typed numbers are not supported (Blender's TAB between components): lock an axis and
  type, then G again for the next axis.
- **Rotate:** about the view axis without a lock, about the locked axis with one (degrees
  typed). A plane lock means the
  axis normal to it. How the mouse reads depends on that axis: **across the view** (more than 30°
  off the line of sight, e.g. the vertical seen from eye height) the turn follows the motion along
  the screen direction the object's near side moves in, 0.5° per pixel, wherever the cursor
  starts (a turntable: drag right and the side facing you goes right); **pointing at the camera**
  (the view axis, or looking along a locked axis) the horizontal motion when the cursor starts
  within 80 px of the pivot's screen position (right = clockwise on screen), farther out the angle
  the cursor sweeps around it (Blender's dial). (Until the free-rotation round boxes turned only in
  90° steps about the vertical; §11 lifted that: boxes turn like models.) A value that leaves
  every object as it was (a box with a square footprint turned 90° about its centre) is named in
  red in the HUD and LMB / ENTER write nothing.
- **Scale:** the ratio of the cursor's distance to the pivot's screen position now and at the
  start; uniform without a lock, one axis with an axis lock, two with a plane lock. Typed: the
  factor.
- **Snap:** off by default (any value). **Holding CTRL** snaps: grab to the N step (the
  pivot lands on the grid, like the arrow keys), rotate to the angle step (SHIFT+N, 15° by
  default, §11.10), scale to 0.1. **SHIFT held**:
  the mouse moves the value ten times slower. Typed values are never snapped.
- **Pivot:** the centre of the selection's world bounds; with exactly one model selected, its
  origin (its position).

### 7.3 What each object can do

| | Grab | Rotate | Scale |
|---|---|---|---|
| box, block | any amount | any axis, global or local (its own axes), as models (§11.9): an axis-aligned result rewrites the corners, any other writes `rot`; about the pivot it also orbits | each axis about the pivot (uniform or locked axes): corners scale about the pivot |
| stairs, waffle | any amount | refused ("stairs / waffles can't turn") | refused |
| model | any amount | any axis; global axes compose on the left, local on the right (`R = Ry Rx Rz` is decomposed back to yaw / pitch / roll); about the pivot it also orbits | uniform or per axis (`scale` key; per-axis scale of a rotated model scales its local axes) |
| light | any amount (and its clip box) | ignored | ignored |

Exact values: a world-Y turn of a model or turned box adds to yaw, a local-Z turn adds to roll, so `R Y 37.5`
writes `yaw + 37.5` exactly; other turns go through the decomposition and are written with 6
decimals. Moves and box resizes are written with the fewest decimals (at most 6) that hold the
value rounded to 1e-6, so a typed 1.25 is written as 1.25.

### 7.4 HUD

One line above the edit-mode lines, as Blender's header, state only (the keys are in the
keybindings panel, ESC): `GRAB  X 1.250 m  global | typed: 1.25_ | snap off`. Rotate shows
degrees, scale the factor (per axis when
locked). Refusals (a waffle in a rotate) show in red and
the modal still runs for the rest.

### 7.5 New edit ops (`mapedit.c`)

- `EDIT_ROTATE` gains pitch and roll, the axis space, and the pivot: `degrees[3]` (yaw, pitch,
  roll for a global turn, or one value about `axis[3]`) + `pivot[3]` + `local`.
- `EDIT_SCALE`: `factor[3]` about `pivot[3]` (boxes: corners; block: centre and sizes; models:
  `scale` and position).
- `EDIT_SIZE`: set a box's size per axis about its centre (`size[3]`, 0 = keep).
- `EDIT_ID_REMOVE`: drop the `id N` prefix of the selection's lines (§8.1).

Edit script additions: `turn <yaw> [<pitch> <roll>] [local] [pivot x y z]`, `scale <f> | <fx>
<fy> <fz> [pivot x y z]`, `size <sx> <sy> <sz>` (0 keeps an axis), `id remove`, and for
`--edit-script` only: `save` (writes the --map file like CTRL+S) and `disk <file>` (replaces the
map file with that file's bytes and runs the outside-change path, §5: the conflict test).

## 8. Panels

### 8.1 The id panel (`src/idpanel.c`, F5)

Lists every explicit `id N` of the map: id, line, kind (box primitive name, `model`, `light`),
object count, model file or material, group. UP / DOWN / PGUP / PGDN / HOME / END move, typing
filters (matches the id, the line, the file, the material and the group, case-insensitive),
ENTER flies the spectator to the objects (`SpectatorFlyToPose`: in front of their bounds, at a
distance that fits them) and selects them, DELETE removes the **id prefix only** (the objects
stay; their entity ids become implicit; decided), SHIFT+DELETE removes the objects (the same as
DELETE in edit mode), CTRL+I gives the selection's line an id (the existing number field), ESC /
F5 close. Remove and give are edits: live, undoable, saved with CTRL+S. The panel takes every key
while open, like the prompt box.

### 8.2 The render inspector (`src/inspector.c`, F3 / SHIFT+F3 / F4)

Editor-only and off by default; with it off nothing is drawn and the runtime's filter is off,
so the image is the normal one.

- **F3** cycles the outline colour: off → by material → by shader → by draw kind (merged mesh,
  instanced boxes, model). Every object of every drawn range within 80 m and in the view frustum
  is outlined (boxes: their edges; models: their rotated bounds), up to 20 000 objects nearest
  first; farther ranges get their range bounds outlined. Colours: a hue per material / shader
  index (golden-ratio steps, fixed saturation and value), the same each run.
- **Labels**: the range under the cursor or crosshair always, plus the 24 nearest on-screen
  ranges, without overlapping (a label that would overlap an earlier one is skipped): material,
  shader, model file, `merged cell mesh` / `instanced` / `model`, triangles, `shadow` or `no
  shadow`.
- **SHIFT+F3** opens the inspector panel (the shared list widget): materials, then shaders, each
  with its range count and triangles; SPACE hides / shows the selected item, I isolates it
  (again: everything back), the filter as in the other panels. Hiding goes through
  `MS_SetDrawFilter`.
- **F4** cycles R3D's buffer views (`R3D_SetOutputMode`): scene, albedo, normal, ORM, radiance,
  specular, SSAO, SSIL (and whatever else `R3D_OutputMode` lists). The HUD names the view.
- `FrameKey` gains the inspector's output mode and a filter generation counter, so frame reuse
  sees every change.

### 8.3 The model picker (`src/modelpicker.c`, I in edit mode)

Lists the package's model files that have a licence row in `ASSETS.md` (the list `editmode.c`
scans today), filtered by typing. Each row: a 96 × 96 thumbnail, the file, triangles, bounds
(size in m), the licence (from its `ASSETS.md` row). ENTER places the model at the point under
the cursor (snapped on the hit face's axes like the old I; CTRL: exact) as a live edit; G then
moves it. ESC / I close (I only while nothing is typed, as the inspector's I; phase 3b). Models from other packages are not listed (decided default: bringing one
in is an asset import with a licence row, so it goes through a request).

Thumbnails: rendered when the picker is first opened after a load (a few per frame, so opening
doesn't stall; rows without one yet show a placeholder), with R3D into a 128 × 128 render
texture: the model alone at a far-away spot, a neutral studio environment swapped in for the
render and restored after, one directional light without shadow, no fog, no AO volume sampling.
The model's file is loaded for that and unloaded right after (files the scene already has are
reused). Freed when `s->loadVersion` changes and at exit. VRAM: 64 KB per thumbnail (RGBA8);
`docs/architecture.md` §7 records it.

### 8.4 The shared list widget (`ui.h`)

```c
typedef struct {
    int sel, scroll;                // selected row (index into rows[]), first visible row
    char *filter;                   // typed text (heap; NULL = empty)
    int filterLen, filterCap;
    int *rows;                      // the items that pass the filter, in item order (heap)
    int rowCount, rowCap;
    bool dirty;                     // filter changed: call UiListRefilter
} UiList;
```

`UiListKeys` handles UP / DOWN / PGUP / PGDN / HOME / END, typed characters and BACKSPACE
(CTRL+BACKSPACE: the whole filter) and leaves ENTER, ESC, DELETE and the rest to the panel.
`UiListRefilter` rebuilds `rows[]` with the panel's match callback. `UiListDraw` draws the panel
in the settings panel's look: `UI_PANEL` with a `UI_LINE` border, a `UI_L` bold title in
`UI_ACCENT`, the filter field (`UI_RAISED`, a cursor, a dim placeholder "type to filter"),
`UI_M` rows of 24 px (a row callback draws the columns), the selected row on `UI_RAISED` with an
accent bar, a thin scroll bar, and a `UiHints` footer. Rows taller than 24 px (the picker's
thumbnails) pass their height.

## 9. Verification

- `--edit-script <file>`: the `--apply-edits` language applied **live** to the loaded GPU scene
  (`MS_EditSetText`, no file write) before `--shot` or any `--dump-*` runs. For every map in
  `maps/` and `tests/models`, one script of moves, turns, resizes, scales, removes, duplicates,
  adds and id changes: the `--edit-script` shot vs. `--apply-edits` on a scratch copy + a shot of
  that copy: 0 changed pixels, at every viewpoint and with `--fog-cache` and `--ao-volume`;
  `--dump-boxes` and `--dump-entities` identical.
- Shots with no edits pixel-identical to the base build (viewpoints, yaw sweeps, `MAP.md` extra
  views, `--reload-test`), box dumps identical.
- Edit latency per map: the `live edit` log line vs. the `load` line.
- HUD shots: `--edit-modal "<g|r|s> [x|y|z|X|Y|Z] [typed]"` (opens the modal on the `--select`
  selection with that lock and text; capitals = plane locks), `--id-panel [filter]`,
  `--model-picker [filter]`, `--inspect <material|shader|draw>`, `--output-mode <name>`,
  `--inspector-panel`, with `--shot --shot-hud`.
- The conflict rule: `--edit-script` with unsaved steps, then `disk <file>` (a changed file),
  then `print`: the rebased selection and the failed list.
- Leak check with live edits, a preview, thumbnails and a reload (`tools/leakcheck.sh` gains
  those runs); both builds; `examples/build.sh`; `export_strip_test.sh`.

## 10. Decisions and defaults

Decided by the developer: RMB-held flying in edit mode (§6); both save modes as an editor
setting, manual by default (§4.1); rebase by entity id on outside changes, save first before a
request (§5); the id panel's remove drops only the prefix (§8.1).

Defaults chosen in phase 0 (each can change; say so in the final summary):

- Pivot: the centre of the selection's bounds; one model alone: its origin.
- Axes: global; the same axis key again switches models to local axes (boxes have none).
- Drag: grab follows the cursor exactly on the plane / axis; rotate by the swept screen angle;
  scale by the distance ratio; SHIFT ×0.1. Snap off; CTRL held snaps (N step, 15°, 0.1).
- Inspector: hue per material / shader index by golden-ratio steps; 24 labels + the hovered one;
  outlines within 80 m, at most 20 000 objects.
- Thumbnails: 128 × 128 rendered, shown at 96 × 96, lazily on first open after a load, a few per
  frame; 64 KB each.
- List widget: the settings panel's look (§8.4).
- Flags: `--edit-script`, `--edit-save`, `--edit-modal`, `--id-panel`, `--model-picker`,
  `--inspect`, `--inspector-panel`, `--output-mode`.
- Keys: F3 / SHIFT+F3 / F4 the inspector, F5 the id panel, I the picker, ALT+R the old step turn
  (the free-rotation round, §11.10: ALT+R is now the rotate modal, R / SHIFT+R the 90° step,
  ALT+SHIFT+R the reset).
- Picker: this package's models only.
- Unsaved edits on close: written to the settings folder's `unsaved/`.

## 11. Free rotation (round: every object turns, Blender-style rotate controls)

Decided by the developer: boxes and blocks turn freely like models (A), a turned box collides as
its turned shape (B), plain **R** is a 90° step with no free control (C), the keyboard rotate
modal moves to **ALT+R** (D), R's step is always about the vertical (E, the default: the
developer left it open), the rotate gizmo shows whenever edit mode has a selection that can turn
(F). Lights, stairs and waffles don't turn (as before). Maps without a turned box parse, mesh,
collide, pick, dump, compile and draw exactly as before: every new path is taken only for a box
that has a rotation.

### 11.1 The format

```
box   x0 y0 z0 x1 y1 z1 [rot yawDeg pitchDeg rollDeg]
block cx y0 cz sx sy sz [rot yawDeg pitchDeg rollDeg]
```

- The numbers before `rot` describe the box **before** it turns (two corners, or a block's centre,
  floor and sizes); `rot` turns it about **its own centre**, with the models' convention:
  `R = Ry · Rx · Rz` (roll about +Z, then pitch about +X, then yaw about +Y).
- `rot` is optional and needs `monolith 2` (a format-1 file with it is an error). It is never
  shifted: every `repeat` / `at` copy gets the same rotation about its own centre (so a turned
  `repeat` line is a row of identically turned boxes; turning the row as a whole about a pivot
  is a mapedit op that also turns the steps, 11.9). `stairs` and `waffle` don't take it (error).
- `rot 0 0 0` parses to an axis-aligned box (no turned-box record): exactly the box without
  `rot`. The editor never writes it (11.9).
- No `MS_FORMAT_VERSION` bump: an older runtime refuses the extra tokens with a line error
  instead of misreading them.

### 11.2 The runtime representation (`runtime/monolith_scene.h`)

```c
// A turned box (a box / block line with `rot`): the box before it turned, and its rotation
typedef struct {
    Vector3 center;         // world
    Vector3 size;           // along its own axes (the extents as written)
    Vector3 rotationDeg;    // yaw, pitch, roll as written (R = Ry * Rx * Rz, about center)
    Vector3 axis[3];        // its own x, y, z in the world: the columns of R (unit)
} MS_TurnedBox;

// in MS_Scene, after boxCount:
    const MS_TurnedBox *turnedBoxes;
    int turnedBoxCount;
    const int *boxTurned;   // per box: index into turnedBoxes[], -1 = axis-aligned; NULL when
                            // turnedBoxCount == 0 (then every box is boxes[i] exactly)
```

- `boxes[i]` of a turned box is its **world AABB** (the eight turned corners' min / max), so
  every consumer that treats `boxes[]` as bounds (clusters, culling, dirty regions, the designer's
  hover and volume tests, `MS_ObjectBounds`, scene bounds, fog / shadow regions) stays correct
  without a change. Only code that needs the exact shape looks at `boxTurned`.
- `colliders[i]` (i < boxCount) is the same AABB: the broad phase. Model colliders stay AABBs.
- The axes are computed once per box at parse / compiled read (floats, `Ry * Rx * Rz` composed
  with the same quaternion code as the models' rotation), so text and compiled loads give identical bits.
- `MS_Transform` of a turned box: `position` = `center`, `scale` = `size`, `rotationDeg` = its
  rotation. Axis-aligned boxes: as today (`rotationDeg` 0).
- `MS_EditPreviewSet` honours a box's `rotationDeg` (a box with rotation 0 previews exactly as
  today; a nonzero one previews as a turned box).

### 11.3 Meshing and drawing

- **Hidden faces:** a turned box never hides another box's face and none of its faces is hidden
  (it is left out of `FindHiddenFaces`' occluder grid and its `hidden` byte stays 0). The
  hidden faces of axis-aligned boxes are computed exactly as before.
- **Merged materials:** turned boxes sort after the axis-aligned ones of their material and cell
  (a `CellItem` key after `big`), so they form **their own per-cell ranges** and the axis-aligned
  ranges' meshes are byte-identical. Their mesh has all six faces of each box: the unit cube's
  vertices scaled by `size`, turned by R, moved to `center`; normals and tangents turned by R;
  UVs as the unit cube has them (so a turned box textures like an unturned one in its own frame).
- **Instanced materials** (`instanced on`): the instance buffer of a material that has a turned
  box gets `R3D_INSTANCE_ROTATION` too (identity for its axis-aligned boxes); every other
  material's buffer keeps today's flags (no change to their shader path).
- Sun shadow tiles, the fog cache and frame reuse need nothing new (they follow the meshes and
  `scene->version`).

### 11.4 Collision

- **The test:** `MS_FirstCollision` / `MS_Collides` walk the BVH by the AABBs as today; for a
  candidate collider that is a turned box (`i < boxCount && boxTurned && boxTurned[i] >= 0`) the
  strict overlap is a **separating-axis test** of the query AABB against the box (15 axes: the
  three world axes, the box's three, their nine cross products; touching is not a hit, as for
  AABBs; a cross product shorter than 1e-6 is skipped). Everything else takes the existing
  `Overlaps` line.
- **Landing and stepping** need the surface, not `max.y`. New queries (`monolith_scene.h`):

```c
// The top of collider `c` above the footprint (x, z rectangle) of `box`: the highest point of the
// collider over that rectangle (an axis-aligned collider: max.y). *walkable (optional): the
// collider's most upward face is at most MS_WALKABLE_SLOPE_DEG steep (axis-aligned: true)
float MS_ColliderTop(const MS_Scene *scene, int c, BoundingBox box, bool *walkable);
// The underside of collider `c` above that footprint: its lowest point there (min.y)
float MS_ColliderBottom(const MS_Scene *scene, int c, BoundingBox box);
#define MS_WALKABLE_SLOPE_DEG 46.0f
```

  The highest / lowest point of the box over the rectangle is the maximum / minimum of y over the
  polytope "box ∩ vertical prism of the rectangle": taken over its candidate vertices (the box's
  corners inside the prism, the box's 12 edges cut by the prism's 4 side planes, the prism's 4
  vertical edges through the box's faces). Exact, no allocation, ~100 flops.
- **`src/player.c`:** every `colliders[hit].max.y` / `min.y` goes through `MS_ColliderTop` /
  `MS_ColliderBottom` (for an axis-aligned collider they return exactly `max.y` / `min.y`, so the
  walk on every map is unchanged). Step up onto a turned box only when the rise is at most
  `STEP_HEIGHT` and the box is walkable, or its whole top (`boxes[i].max.y`) is within
  `STEP_HEIGHT` of the feet (rubble you step over); otherwise it is a wall. On a 30° ramp each
  frame's rise is a few cm, so the player walks up it and stands on the slope (feet at the
  surface's highest point under the footprint). **Walking down** a turned walkable box: when the
  player was grounded, isn't jumping and nothing is under the feet after the move, a probe down
  by `STEP_HEIGHT` that hits a **turned** walkable box snaps the feet onto it (so a ramp is walked
  down, not fallen down in hops); axis-aligned boxes keep today's fall (unchanged behaviour on
  stairs).
- The movement is split from the input: `PlayerStep(Player *, Vector3 wish, bool run, bool jump,
  float dt)` does the physics and `UpdatePlayer` reads the keys and calls it, so
  `tests/unit/collision_test.c` drives it against a `noGpu` scene of the test map (11.12).

### 11.5 Raycast (picking)

`MS_Raycast`: for a turned box the ray goes into the box's frame (origin − centre, dotted with
the three axes) and takes the slab test against ± size / 2; the hit normal is that axis in the
world, facing the ray. Same rules as before: an origin inside the box is skipped, the nearest hit
wins, ties go to the lower index. Axis-aligned boxes: the existing loop, unchanged. `MS_Hit.normal`'s
comment becomes "face normal (boxes: their face's, turned with a turned box)".

### 11.6 The AO volume and `--bake-surface`

- **AO volume: voxelised by the turned box** (it is cheap): `AoSplatBox` takes an optional
  `MS_TurnedBox`; the voxels within `AO_DIST_MAX` of its AABB are visited as today, and each
  voxel centre is moved into the box's frame (3 dot products) for the same box distance. An
  axis-aligned box splats exactly as now.
- `--bake-surface` bakes a material's surface shader into textures and doesn't read the boxes:
  nothing to do.

### 11.7 Compiled maps and the export

- `CM_VERSION` 3. New section `CM_BOX_TURNS` ("box turns"; after `CM_DEPS`), one 16-byte record
  per turned box, sorted by box, each box at most once:
  `typedef struct { uint32_t box; float rotationDeg[3]; } CmBoxTurn;`
  The `CM_BOXES` record keeps the box **as written** (the corners before the turn), not its AABB;
  the reader rebuilds the turned-box record (centre, size, axes, AABB) with the parser's code,
  so the bits match a text load. Checks: box < box count, strictly increasing, finite angles.
- The reader accepts **version 2** (no such section: no turned boxes) and 3; anything else is
  refused as today. The writer always writes 3.
- `docs/compiled-map.md`: the new section, the version history line.
- The export runtime is this file without the editor blocks: everything above is outside them.
  `runtime/export/viewer.c` and the examples need no change (`MS_Collides` stays exact).
  `src/export.c`'s README text: `colliders` are "world AABBs (a turned box: its bounds; the exact
  test is `MS_Collides`)", plus `turnedBoxes` / `boxTurned`.

### 11.8 Dumps and the request bundle

- `--dump-boxes`: an axis-aligned box prints exactly today's line; a turned box appends
  ` rot <yaw> <pitch> <roll> center <x> <y> <z> size <x> <y> <z>` (`%a`) after its AABB.
- `--dump-scene` / `--dump-scene-diff`: the same suffix on a turned box's `box` line; the
  `counts` line gains ` turned <n>` only when n > 0. `--dump-entities` prints the transform as it
  does (now with the box's rotation).
- The bundle (`src/launcher.c`): a turned box in the object tables shows its rotation
  (`rot y p r` in `request.md`, `"rotationDeg"` in `request.json` only for a turned box); the
  `"format"` number goes up by one and `.claude/skills/map-request/bundle-reference.md` describes
  it. Unturned boxes are written as before.

### 11.9 Edits (`src/mapedit.c`, `EditOp`)

- `EDIT_ROTATE` applies to `box` and `block` lines about any axis, global or **local** (now a
  box's own axes too), with or without a pivot. The written result of turning a box by `M`:
  - the box's total rotation `R' = M · R0` (global; local: `R0 · M`), its centre orbiting the
    pivot (local or no pivot: its own centre);
  - **when R' is axis-aligned** (a signed permutation: every entry exactly 0 or ±1, which the
    exact sin / cos of multiples of 90° give): the line stays an axis-aligned box. Its corners are
    rewritten (the extents permuted, a block's sizes permuted) and a `rot` it had is removed.
    So a 90° step of an unturned box rewrites the corners as today, and turning a turned box back
    to 0 0 0 (or any axis-aligned pose) drops its `rot`;
  - otherwise the extents stay as written, the centre moves (box: both corners; block: `cx cz`
    and `y0` = centre y − sy / 2), and `rot` is written / added as yaw, pitch, roll: **exact**
    for a world-Y turn (adds to yaw) and a local-Z turn (adds to roll), 6 decimals otherwise
    (as for models).
- A whole `repeat` line turned with a pivot: the steps and `at` turn too (as today); every copy
  gets the same new `rot`. One copy: split out as today.
- **New op `EDIT_SET_ROTATION`** (reset and typed yaw / pitch / roll): `ypr[3]` absolute, each
  object about its own centre (boxes) / origin (models). Boxes: the same written rule (0 0 0 or
  an axis-aligned pose → corners, no `rot`; else `rot ypr`); models: `rot` set (0 0 0 removes
  it). Lights, stairs, waffles: refused as for turns.

```c
EDIT_SET_ROTATION,      // ypr[] absolute about each object's own centre / origin (reset: 0 0 0)
...
double ypr[3];          // EDIT_SET_ROTATION: yaw, pitch, roll (degrees)
```

- Lifted: the 90° rule and the X/Z refusals (`RwTurn`, `RwTransform`, `MapEditPreviewTransform`,
  the selection checks near "turn boxes and models separately"). A plain `turn <deg>` (no pivot,
  no axis) now turns boxes by any yaw too (a non-multiple of 90 adds `rot`), and mixed boxes and
  models in one turn are fine.
- `MapEditPreviewTransform` gives a box's new `rotationDeg` (the decomposed R') and, for an
  axis-aligned R', the permuted size with rotation 0, exactly as the commit writes it.
- Edit script additions: `rotation <yaw> <pitch> <roll>` (`EDIT_SET_ROTATION`); `turn` as before
  (now on boxes with any angles).

### 11.10 Controls (`src/transform.c`, `src/editmode.c`)

| Key (edit mode) | Does |
|---|---|
| **R** / **SHIFT+R** | turn the selection 90° / −90° about the vertical, each object about its own centre / origin (one edit; no modal). Boxes, blocks, models (lights, stairs, waffles: named and skipped, as today) |
| **ALT+R** | the keyboard rotate modal (§7: X / Y / Z locks, typed angle, turntable / dial, CTRL snap, SHIFT slow), now for boxes about any axis too |
| **ALT+SHIFT+R** | reset rotation: `EDIT_SET_ROTATION` 0 0 0 on the selection |
| **LMB on a gizmo ring / the trackball** | drag to turn (below) |
| **L** | the gizmo's axes: global ↔ local (the active object's own axes: the first selected box or model) |
| **SHIFT+N** | the angle snap step: 1 → 5 → 15 → 45 → 90° (default 15; N still cycles the grid step) |
| G, S, the rest | as §6 |

Inside the ALT+R modal R still switches to rotate (G / R / S switch kind, §7.1).

**The gizmo** (shown in edit mode when the selection has a box, block or model that can turn,
no modal or drag runs and RMB isn't held; drawn in 2D after `R3D_End` with the guides, on top of
the scene, constant screen size):
- at the transform pivot (§7.2), three axis rings of radius **120 px** (X red, Y green, Z blue;
  the global axes or, with L, the active object's), only their **front half** (the half toward
  the camera) drawn and hit; a white **view ring** at 1.15 × the radius facing the camera; the
  **trackball**: a faint grey disc inside the radius. Line 2 px, 3 px and brighter when hovered.
- Hover (when the cursor isn't on a panel): the view ring if within 8 px of it, else the nearest
  axis ring within 8 px, else the trackball if inside the radius, else nothing (LMB selects as
  usual). An edge-on ring (seen as a line) is still hit along that line.
- **Drag a ring:** the angle swept around the pivot **in the ring's plane** (the cursor's rays hit
  the plane through the pivot, normal to the axis; the angle between the start and current hit
  about the axis, unwrapped). When the ring is edge-on (its plane seen at less than 15° from the line of sight) it falls
  back to the turntable (`TrackDirection`, 0.5°/px). The view ring turns about the view axis by the screen
  angle swept around the pivot (the dial).
- **Drag the trackball:** axis = normalise(camUp · dx + camRight · dy) (perpendicular to the drag
  on screen), angle = |drag| · 0.5°/px, from the drag since the start (one axis-angle turn, so one
  `EDIT_ROTATE` with `axis[]`).
- While dragging: an **arc** in the ring's plane from the start to the current angle (a filled
  translucent sector in the axis colour) and a **readout** next to the cursor (`37.5°`, with
  the snap step when CTRL is held); the HUD line as §7.4.
- CTRL snaps the angle to the snap step (SHIFT+N); SHIFT: ten times slower; typed values (`37.5`,
  `-90`; the trackball: degrees about the camera's up) override the mouse and are exact.
- **Commit / cancel:** a drag (the cursor moved more than 4 px with LMB held) commits on LMB
  release, unless something was typed; a ring **click** (released within 4 px), or a drag with
  typed text, stays open as a modal without the button: the mouse still turns, ENTER / LMB commit,
  ESC / RMB cancel. One undo step per commit. The no-change rule (§7.2: a value that leaves every
  object where it was is named in red and writes nothing) holds for all of them.
- The rotate drag is the transform modal with a source (`TRANSFORM_SRC_KEYS` for ALT+R / G / S,
  `TRANSFORM_SRC_RING` with its axis 0..2 or view, `TRANSFORM_SRC_BALL`); the preview and the op
  are built as for the keyboard modal.

`--edit-modal` gains (with `--select`, `--shot`, `--shot-hud`, `commit` / `cancel` as before):
`r90` / `r-90` (the step), `reset`, `rot <y> <p> <r>` (typed YPR: `EDIT_SET_ROTATION`),
`ring <x|y|z|xx|yy|zz|view> <deg>` (a ring drag of that many degrees; doubled = local),
`ball <dx> <dy>` (a trackball drag of that many pixels), `gizmo` (just draw the gizmo, with
`hover <x|y|z|view|ball>`), plus the existing `<g|r|s> …` forms (now the ALT+R modal for r).

### 11.11 Defaults chosen (say so in the final summary)

- Gizmo: X red, Y green, Z blue (the guide colours), view ring white at 1.15 × radius, trackball
  grey disc; radius 120 px, hit tolerance 8 px, constant on screen; front halves only.
- Gizmo axes: global by default, **L** toggles local.
- Snap: CTRL held; the step cycles 1 / 5 / 15 / 45 / 90° on **SHIFT+N**, 15° by default (an
  editor-session value in `designer.c` beside the grid step; not saved).
- Reset: **ALT+SHIFT+R** (Blender's ALT+R), replacing the old reverse step turn.
- Trackball 0.5°/px, as the turntable.
- AO volume: turned boxes voxelised by their shape.
- Walkable slope: 46° (`MS_WALKABLE_SLOPE_DEG`); step-down snap only onto turned walkable boxes.
- A turned box whose pose becomes axis-aligned is written as plain corners without `rot`.

### 11.12 Verification

As in the round's task prompt: every map's `--dump-boxes` and shots identical; the test map
`tests/rotated/` (a 30° ramp, a 45° yawed wall, a tumbled crate, a turned `repeat` row, a turned
block, a turned box under `instanced on`) with shots from three viewpoints, `--select` picks on
turned faces (the normal in the bundle), the collision unit test (`tests/unit/collision_test.sh`:
the player walks up the ramp and stands on it, walks down it without leaving the ground, and
stops at the yawed wall's true face where its AABB would have stopped it ≥ 0.3 m earlier), the
compile and export round trips, a `--reload-test`, and `--edit-modal` shots of every control on a
box and a model with the line before and after.

## Implementation notes

### Phase 1: the runtime, the working copy, saving (§2–§5, §9)

**Runtime** (`runtime/monolith_scene.c`, region "Live edits", editor builds only):

- **Parse.** The parser hashes the tokens of every `monolith`, `material` and `env` statement into
  `SceneData.lookHash` (`HashLook`). A different hash, a different material count or a compiled
  load means a full upload from memory (`UploadGpu`, `stats->full`); model files are then loaded
  anew too (`UploadGpu` frees everything on failure, so it must not own borrowed files).
- **Borrow, then take.** Everything the new load keeps from the old one (model files and their
  pick data, the materials' GPU state with the shader aliases, the texture list, the shader file
  list as a copy with `rel` pointed at the new materials' strings, sky, ambient map, the `unit`
  buffer, kept merged meshes, kept instance buffers, light shadow maps) is first *borrowed*:
  both loads hold the handle. On success `LiveRelease(old)` zeroes old's handles before
  `FreeData(old)`; on failure `LiveRelease(d)` zeroes the new load's before `FreeData(d)`, so a
  failed edit (a broken text, a missing new model file, out of memory) leaves the scene exactly
  as it was. New model files are loaded before anything is borrowed.
- **Matching** is by content (`LiveMatch`: a hash table of chains in map order, each new object
  takes the first unused old one with the same content): boxes by bounds + material, models by
  file, transform, bounds, materials, slots, colliders and shadow flag, lights by every field
  (field by field: `R3D_Light` padding is not set). Unmatched objects on both sides go into
  `MS_Private.dirty`, boxes first (the AO volume and the hidden faces look at boxes only).
- **Hidden faces** (`UploadBoxesLive`): copied from the old load for unchanged boxes that touch no
  changed box; searched again for the others with the same `HideRange` as a load, in an occluder
  grid that holds only the occluders touching an affected box (`HideGridBuild` got an optional
  skip set). A cell's list then has the same boxes in the same order as in the full grid for every
  box the search visits, so the neighbour order, which decides the piece-count limit, is the
  same; whether the full grid would exceed `HIDE_ENTRIES_MAX` (then nothing is hidden) is decided
  from the full count. More than 256 changed boxes: the full threaded search. A box whose hidden
  faces changed adds its bounds to the dirty list too (its cells' meshes are rebuilt anyway).
- **Ranges.** A merged range keeps its mesh when the old range with the same key (material,
  big, cell) has the same boxes (matched) in the same order with the same hidden faces; an
  instanced material keeps its instance buffer when its whole box list is the same. Model
  groups are always rebuilt (one small instance buffer per group).
- **Caches** (`Restamp`): every cache stamped with the old version is stamped with the new one
  unless a dirty box reaches it: sun tiles and the camera-fitted map by the NDC x / y of the
  dirty boxes' corners under their `viewProj` (±1.01), point / spot shadows by their range
  sphere (and a moved light gets a new map), the fog cache when the dirty boxes reach the bake's
  light-space box or its region changed, the AO volume voxel by voxel (`AoUpdate`: one window
  per changed box, the voxels within `AO_DIST_MAX` of it recomputed from every box with the
  same `AoSplatBox` as the full bake; more than 16 changed boxes: one window around all of them;
  a changed region or size: the full bake by the next `MS_DrawGeometry`). The first-frame log
  is re-stamped too (it is no load). `BakeOcclusionVolume` was split into `AoLayout`,
  `AoSplatBox`, `AoByte` and `AoBake` (shared, the same float arithmetic in the same order) with a
  game-build and an editor-build caller (the editor's keeps the bytes in `MS_Private.aoGrid`);
  `FindHiddenFaces` into `HideGridBuild` / `HideJobInit` / the threads (the game build passes no
  skip set); `LoadModelFiles` into `LoadModelFile` per file. The game build's behaviour is
  unchanged: every map's shots are pixel-identical to the build before the round (viewpoints, yaw
  sweeps, `--reload-test`) and the box dumps identical.
- **The fog cache** rebake is R3D's whole-region bake. Measured on the largest map in `maps/`
  (`--fog-cache 0.5 2`, 290 × 202 × 510 voxels): 19.0–20.5 ms per bake (R3D's own log line, `glFinish`
  around it; plus the bake's shadow map render) on the RTX 4050 Laptop machine, right at the
  ~20 ms threshold of §3.3. No R3D patch was made; whether a sub-region bake is worth a patch is
  the lead's decision.
- **Preview** (`MS_EditPreviewBegin / Set / End`): Begin records every selected object's state,
  rebuilds the merged ranges of the selected boxes without them (the boxes touching them get
  their faces searched again, the selected ones skipped as occluders), sets the selected boxes
  of instanced materials to scale 0 in their buffer, and draws the selected boxes as cubes from
  one preview buffer (one range per material; `PreviewDraw` in `MS_DrawGeometry`). Set writes
  boxes, colliders (BVH refit, `RefitColliderBvh`), models (`ResolveOneModel`, their instance
  slot through `modelSlotOf` / `modelRangeOf` kept by `BuildModelGroups`; the range's cluster
  bounds grow), lights (clip box shifted with them), entity transforms and the scene bounds;
  the old and new bounds invalidate tiles and light shadows (`Restamp` with the fog cache and
  the AO volume left as they are). End restores everything, frees the preview meshes and
  invalidates what the preview reached since Begin. `MS_EditSetText`, a reload and `MS_Unload`
  end a running preview first, so the diff of a confirming edit is against the original state.
- **`loadVersion`** goes up in `LoadBytes` only. `MS_EditMarkSaved` sets the watch list to the
  current text's dependencies (an added model file is watched from then on), their mtimes and
  `triedHash` as a load from disk would.

**Working copy** (`src/mapedit.c`): the working copy exists from `MapEditBegin` on (the first
edit / undo in the game, `--edit-script`); without it (`--apply-edits`) the old file path runs
unchanged (the same output and bytes). `MapEditor` gained `workHash` (so the per-frame unsaved
count never hashes the text), `loadVersion` (a reload from disk is seen without a hash) and
`lastStats`; `EditStep` gained `anchorId` (the entity an added model stands on, for a rebase).
`MapEditSave` re-reads the file and refuses to write when it changed since it was read (the hot
reload then rebases). Deviations from §5:

- **The rebase applies each step live** (one `MS_EditSetText` per re-applied step) instead of one
  `MS_EditSetText` for the result: a step's selection is resolved, and its patch built, against
  the entity sources of the text the steps before it made, which is the scene. Usually one or
  two steps, each ~an edit.
- **Ids a re-applied step gives anew** (a line's `id N` is the file's largest + 1, which differs
  when the file on disk has other ids) are mapped for the steps after it (`IdPair`), and the ids
  of a step that failed are dead for the later ones, so they fail too instead of picking an
  unrelated line with the same id on disk. A limit that stays: an explicit id a *saved* step gave
  a line identifies that line; if the file on disk gives the same id to another line, a later
  unsaved step selecting it applies there.
- **Steps undone past the save point** (the working copy lacks edits the file has) can't be
  replayed as ops; they are listed as failed ("undo of …: undone past the save point: the file
  keeps it"). `savedAt = -1` (the redo stack holding the save point was dropped): every undo step
  counts as unsaved and is re-applied.
- **The warning for a request on the map** only comes on the first edit after the last save (as
  designed) and on a save (CTRL+S writes the file the request reads).
- `EditModeSync(m, d, newScene)` also runs after a renderer restart (shadow size): the same file
  loaded again gets the working copy live again; a changed file rebases.
- `--edit-script` with `--shot` runs **after 10 frames** (every cache drawn once), so the shots test
  what the partial invalidation makes of a warm cache; with a `--dump-*` flag it runs right after
  the (hidden-window) load. Without `--shot` or a dump it is refused (it would need the window).
  The hidden test commands are `preview move dx dy dz` and `preview end`.
- Closing with unsaved edits writes `unsaved/` only in interactive runs (automated runs log that
  the edits were dropped, so tests never write into the settings folder).
- The **Editor** group of the F2 panel holds one row, *Save edits* (`edit_save`, `GfxSettings.editSave`);
  switching it to auto saves what is unsaved at once.

- `--submit` (the test hook of the request submit) is refused with the same message as ENTER while
  `--edit-script` left unsaved edits; after a `save` it writes the bundle, whose map hash is the saved file's.

**Verification** (re-run in full after the phase-1 agent's restart; scripts and outputs in the round's
scratch folder; machine A: RTX 4050 Laptop, driver 615.71.09, Ryzen 7 7435HS):

- **Live = file.** For every map in `maps/` and `tests/models` one script of moves (whole lines, single
  `repeat` copies, a 2-D `repeat` copy), turns, removes, duplicates, an add (`tests/models`,
  `pine_forest`; the largest map adds its `sedan.glb`, a model file new to the scene; `test1` has no
  models and only one square slab, which a 90° turn leaves the same, so no add and no turn), an id,
  undo / redo; on the largest map edits visible from five of its seven viewpoints (the cantilever block
  moved, a roof beam copy removed, boxes turned). `--edit-script` shots (the script runs after 10
  frames, every cache drawn) vs. `--apply-edits` on a copy + shots of it: 0 changed pixels at every
  viewpoint (7 + 5 + 2 + 2), also with `--fog-cache 0.5 2` and `--ao-volume 0.5 0.6` (the largest map,
  `pine_forest`, `tests/models`); `--dump-boxes` and `--dump-entities` (but the `map` line) identical.
  All edits of a script run in one frame, so the test covers partial invalidation of warm caches, not a
  cache redrawn between two edits (the same code path; by hand only).
- **No edits = before.** Every map's shots (every viewpoint, a yaw sweep 0/60/…/300 from viewpoint 1,
  `--reload-test`; the largest map, `pine_forest` and `tests/models` also with `--fog-cache 0.5 2` and
  `--ao-volume 0.5 0.6`) pixel-identical to the build before the round; `--dump-boxes` identical; the
  game build (the examples' test build and compiled copies of the maps through `01_minimal`)
  pixel-identical to the examples built from the commit before the round; `--apply-edits` gives the same
  files and output as before the round.
- **Save:** `save` writes the same bytes as `--apply-edits` of the script (all four maps); auto save mode
  (`--edit-save auto`) leaves the same file after every step, 0 unsaved.
- **Preview:** Begin / Set / End gives the unedited image, preview + confirming move the `--apply-edits`
  image (0 pixels, with the fog cache and the AO volume, `tests/models` and the largest map).
- **Conflicts** (`tests/models`): unsaved steps (a light moved, a rock turned, a tree copy moved, a model
  added), then `disk` with a changed file (lines inserted above, another light changed, `id 1` taken on
  disk): all four re-applied, new ids 2–5, the saved file equal to `--apply-edits` of the same ops on the
  changed file; with the turned rock's line removed on disk: three re-applied, "could not re-apply:
  turned 1 object by 15 deg (line 40) (entity c68c3e6d:0 is gone)" in red, the saved file equal to
  `--apply-edits` of the three other ops on that file.
- Edit latency: `.claude/skills/benchmarking/measurements.md` ("Live edits").
- Builds without warnings (`./build.sh`, `--win`, `examples/build.sh`, `test`, `--win`; the runtime with
  clang and gcc with and without `MONOLITH_EDITOR` and `MS_PATCHED_R3D`), `export_strip_test.sh`,
  `compiled_test.sh`, `tools/leakcheck.sh` (with the live-edit runs) and `--asan`: OK.

### Phase 2: the transform modal, the edit-mode keys, the new ops (§6, §7)

**Edit-mode keys** (`main.c`, `spectator.c`, `designer.c`): in edit mode `Spectator.rmbFly` lets the
camera look, fly (WASD / E / Q / SPACE) and change speed (wheel) only while RMB is held, and leaves G
to the mode (no "move the player here"). `main.c` keeps the cursor state: free in edit mode, hidden
while RMB flies or the modal runs (put back where it was when RMB is let go), never touched while the
prompt box, snip mode or the settings panel own it. With the cursor free the hit is
`DesignerPickCursor` (cached like the crosshair pick, keyed on the cursor too), so hover, LMB / SHIFT+LMB
/ CTRL+LMB and I work under the cursor and no crosshair is drawn; `Designer.editMode` stops
`DesignerUpdate` from clearing the selection on an RMB press, and `main.c` clears it on an RMB click
(released within 0.25 s, the mouse moved less than 4 px in all). ALT+R / ALT+SHIFT+R are the old step
turn. Outside edit mode nothing changed.

**The modal** (`src/transform.c`, `TransformModal` in `transform.h`, owned by `EditMode.modal`): Begin
copies the selection, its entity transforms and each object's class (`MapEditClassify`: box, block,
stairs / waffle, model, light, directional light; one `Analyze` per line), computes the pivot and the
first model's axes, and calls `MS_EditPreviewBegin`. Every frame it turns the input into the value,
builds the `EditOp` the commit would make, and gives each object that follows the transform
`MapEditPreviewTransform` computes from its start transform: the same matrices and the same exact
yaw / roll shortcuts that `mapedit.c` writes the line with, so the preview is the commit up to the
written decimals. `MS_EditPreviewSet` runs only when a transform changed; `DesignerAdoptVersion`
keeps the selection's indices through the version bumps. Commit: `MapEditApply` on the objects that
follow (one step, one undo), then `TransformEnd` (a refused commit ends the preview, so the scene is
back as before); a commit with nothing changed or nothing that follows is a cancel. Cancel:
`MS_EditPreviewEnd`. A reload from disk or a renderer restart cancels a running modal
(`EditModeSync`). The per-frame path allocates nothing (the arrays grow at Begin).

**Values as built** (defaults chosen where §7 left it open):

- Grab: no lock = the plane facing the camera through the pivot; axis = closest point to the
  cursor ray; plane = the ray meets the locked plane. A ray parallel to the axis / plane keeps the
  last value. Typed with a plane lock: along the plane's first axis (Y / Z for SHIFT+X, X / Z for
  SHIFT+Y, X / Y for SHIFT+Z). CTRL: the pivot lands on the N grid along the locked axes (no lock:
  the axes that lie in the view plane, i.e. not within ~8° of the view direction); with local axes
  the amount along each axis snaps to N.
- Rotate: the swept screen angle around the pivot's screen position, unwrapped frame by frame (more
  than one turn works); counter-clockwise on screen is counter-clockwise as seen from the camera.
  **Deviation:** with a box that can follow in the selection, no lock means the vertical (not the view
  axis) and the angle snaps to 90° for the whole selection (one op); an X / Z lock refuses the boxes
  (red) and turns the models freely.
- Scale: the distance ratio; when the cursor starts closer than 30 px to the pivot's screen position
  the ratio is eased ((d + k) / (d0 + k)) so it starts at 1 without a jump. Factors must be > 0
  (typed 0 or a '-': red, the commit is refused).
- Local (the same lock key twice) uses the **first selected model's** axes for the lock, the guides
  and grab (one `EDIT_MOVE` for all objects); rotate and scale with a local lock apply to each model's
  own axes, each **about its own origin** (no orbit); boxes have no local axes and act globally.
- '-' toggles the sign of the typed value (a lone '-' negates the mouse value); '.' once;
  BACKSPACE; at most 31 characters. Typed values are exact (to 1e-6); mouse values are rounded to
  what the HUD shows (1 mm, 0.01°, 0.001) before they go into the op; CTRL snaps (N, 15°, 0.1).
- G / R / S switch the kind: the original transforms, no lock, nothing typed; the cursor position at
  the switch is the new start. A new lock restarts the mouse value from the cursor's position then.
- The pivot is rounded to 0.1 mm (one model: `MapEditAnchor`, the position as written), so the
  written numbers of an orbit stay short.
- Lights follow grab (a directional light can't: red) and ignore rotate and scale (not red);
  stairs / waffles follow grab and are refused (red) for rotate and scale.
- HUD: one `UiHudHints` line above the edit-mode lines, `ROTATE  30.00 deg about X  global |
  typed: 30_ | snap off (hold CTRL) SHIFT precise | LMB / ENTER confirm RMB / ESC cancel | X Y Z
  axis, SHIFT plane, again: local | G R S switch`, the refusal on a `UiHudText` line under it in
  `UI_ERROR`. Guides: a 4 km line through the pivot along each locked axis (rotate: the axis turned
  about; X red, Y green, Z blue) and a small white pivot cross, drawn without depth test after the
  designer overlay. No crosshair while the modal runs in the window.

**The ops** (`src/mapedit.c`, region "Turns about any axis or a pivot, scales and sizes"): `EditOp`
gained `pitch` and `roll` (the script's `turn y p r`; the modal uses `axis` + `angle`). The old
`turn <yaw>` (no pivot, axis, pitch, roll or local) keeps the old code path (`RwTurn`), so old scripts
give the same bytes. The new path (`RwTransform`) maps the positional numbers (a whole line: its own
numbers, and with a pivot its `repeat` steps and `at` too; one copy: its world numbers, through the
existing split machinery) and splices the other numbers it changes: block sizes (swapped by an odd
quarter turn, scaled, set), a model's `rot` (a world-Y turn adds to the yaw, a local-Z turn to the
roll, exactly; anything else composed, global on the left, local on the right, and decomposed back
to yaw / pitch / roll with 6 decimals) and `scale` (each model axis times the factor of the world
axis nearest to it; written as one number when the three are written alike; inserted after `rot` or
the position when the line has none). Box corners are rotated with exact integer matrices and
written smaller first. Numbers the op leaves alone keep their text (compared as written). Box
turns about a pivot by 180° are allowed (the box moves); without a pivot they are still refused
("the same box"), and mixed box + model turns are allowed only on the new path (with a box, a yaw in
90° steps). Script: `turn <yaw> [<pitch> <roll>] [local] [pivot x y z]`, `scale <f> | <fx> <fy> <fz>
[local] [pivot x y z]` (`local` added to the planned syntax), `size <sx> <sy> <sz>`; `--edit-script`
also `preview turn …` / `preview scale …` (the modal's preview math, for the cancel test with dumps).
`EDIT_ID_REMOVE`, `save` / `disk` are phase 1's or later phases'.

**`--edit-modal "<g|r|s> [lock] [typed] [commit|cancel]"`** (`main.c`): with `--shot`, after 10 frames
(after `--edit-script`) the edit mode and the modal open on the `--select` selection (lock words
`x y z`, capitals for planes, doubled for local), values computed for the shot's camera; `commit` /
`cancel` end it after 20 frames through the same commit / cancel code. Exit 1 if it can't open or a
commit is refused.

**Verification** (scripts and outputs in the round's scratch folder `phase2/`; machine A):

- Unit `--apply-edits` scripts on copies of `tests/models` for size (box, a whole block `repeat`
  line, one block copy), scale (block copy, box about a pivot, rotated and plain models, a model
  `repeat` line about a pivot), turn (box and block line / copy about a pivot, a model line with a
  local roll, with yaw + pitch, one model copy pitched about a pivot, a model line orbiting a pivot,
  `rot` inserted, mixed box + model) and refusals: the written lines checked by hand (e.g. `box -6 2.5
  -9 6 2.7 -7` + `size 0 0.5 3` = `box -6 2.35 -9.5 6 2.85 -6.5`; `repeat 4 4 0 0 block -6 0 -8 3 2 0.4`
  + `turn -90 pivot 0 0 -8` = `repeat 4 0 0 4 block 0 0 -14 0.4 2 3`; the decomposed `rot 35.710735
  3.512998 60.38002` matches the composed matrix to 9e-9). Split exactness: on the largest map a
  script with 13 changed boxes (copies of a 1-D and a 2-D `repeat` line, a line with `at`, whole lines)
  leaves the other 52 231 boxes bit-identical (`--dump-boxes`, hex floats).
- `--edit-script` vs `--apply-edits` + shots of the same new-op scripts (with a `preview turn` /
  `preview end` in the live one): `tests/models` (2 viewpoints) and the largest map (7), each plain,
  `--fog-cache 0.5 2` and `--ao-volume 0.5 0.6`: 0 changed pixels in all 27 pairs; `--dump-boxes`
  and `--dump-entities` identical.
- Cancel: `--edit-modal "… cancel"` (grab with an axis lock, rotate 90 with boxes, rotate about Z,
  scale with a plane lock, uniform scale) vs no modal: 0 changed pixels at every viewpoint of
  `tests/models` and the largest map (45 pairs; the open modal itself changes 1.24 M pixels of one of
  those views); `preview turn` / `preview scale` + `preview end` then `--dump-boxes` /
  `--dump-entities`: identical to the unedited dumps on both maps.
- Commit: `--edit-modal "… commit"` vs `--apply-edits` of the op it reports (grab with an axis lock,
  a mixed box + model 90° turn about the bounds centre, a local-Z turn that writes `rot 30 0 35`,
  a plane-locked scale): 0 changed pixels (2 + 2 + 2 + 7 views).
- No edits: every map in `maps/` and `tests/models` (every viewpoint, yaw sweep 0/60/…/300 from
  viewpoint 1, `--reload-test`) pixel-identical to the build before the round (44 pairs), box dumps
  identical; phase 1's `--apply-edits` scripts give byte-identical files and output with the base
  binary (5 scripts on four maps).
- HUD shots (`--shot --shot-hud --edit-modal`): grab with a typed value and an axis lock, rotate
  (with a refused box), scale with a plane lock, on `tests/models` and the largest map.
- Builds without warnings (`./build.sh`, `--win`); `tools/leakcheck.sh` (with the new runs:
  `apply-xform`, `edit-script-xform`, `edit-modal-hud`, `-commit`, `-cancel`): OK. Mouse, keys,
  RMB flying, the cursor and the hover were not testable without a window: by hand only.
### Phase 3b: the model picker (§8.3)

**The picker** (`src/modelpicker.c`, `ModelPicker` in `modelpicker.h`, owned by `main.c`): the list is the
edit mode's (`EditModeScanModels`, the old `ScanModels` made public: `models/` files whose package path
`ASSETS.md` names), scanned again on every open; entries of files still listed keep their info and
thumbnail. The licence is the fourth cell of the file's `ASSETS.md` row (the row whose first cell is the
path in backticks), cut before `, http` and ` (` ("CC0 1.0, https://..." -> "CC0 1.0"); a file named in
`ASSETS.md` without such a row is listed with "no licence row in ASSETS.md" in `UI_WARN`. The filter
matches the file and the licence (case-insensitive). Rows are 104 px (a 96 × 96 thumbnail, the file in
`UI_M`, triangles and size in m, the licence in `UI_OK`); the panel sits where the inspector panel does.
ENTER is `EditModeAddModel` (the old `AddModel` with the path as a parameter: `EDIT_ADD` at the
designer's hit, snapped on the hit face's axes, CTRL exact, the confirmation key `KEY_ENTER`); placed, the
picker closes and the selection is the new model, so G moves it; refused (no surface under the cursor, a
request warning), it stays open with the reason in the edit mode's message line.

**Keys and routing** (`main.c`): plain I in edit mode (no CTRL / ALT, RMB not held, nothing else taking
the keys) opens it; `EditModeUpdate` no longer handles plain I. While open it takes every key
(`ModelPickerCapturesInput` joins `typing`; the designer and `EditModeUpdate` are not called), like the
inspector panel. Leaving spectator or edit mode closes it. **Deviations:** I closes only while the filter
is empty (otherwise it types an i: model names are full of them), as the inspector's I; **`[ ]` is
retired** (with I opening the picker they selected nothing anyone could add; `EditMode.modelPick` is gone,
and the HUD's "add: …" part with it).

**Thumbnails** (`ModelPickerRenderThumbs`, called every frame right before the scene's `DrawScene`,
outside any `R3D_Begin` / `End`): drops every thumbnail when `scene` or `scene->loadVersion` changed (a
live edit keeps them: they don't depend on the map), and while the picker is open renders up to
`PICKER_THUMBS_PER_FRAME` (2) missing ones of the rows that pass the filter, from the scroll position on.
Each is a 128 × 128 RGBA8 colour-only framebuffer (`LoadColorTarget`: no depth renderbuffer, so 64 KiB;
R3D's depth output is off for the render) and one R3D frame: the model file's `R3D_Model` with its own
materials (the scene's when a model of the map uses the file, else `R3D_LoadModel` after
`MS_CheckModelReferences`, unloaded right after), drawn at `scene->bounds.max + 1000 m + 2 r` on every
axis (beyond the AO volume, the fog cache and every light's reach; none of the map's draws or lights are
submitted), framed from yaw 35° / pitch 22° with a 30° square view fitted to the bounds' corners, lit by
one `R3D_PushLight` directional light (no shadow map) in a studio environment (`R3D_ENVIRONMENT_BASE`: no
sky, no ambient map, SSAO / SSIL / SSGI / SSR / fog / volumetric fog / bloom / auto exposure off; a flat
46 47 52 background, ambient 150 152 160 × 0.6, ACES). R3D's environment is `memcpy`'d before and
`memcpy`'d back after (every byte, padding included), and the output mode and depth-output flag are
restored. What else R3D keeps across frames was checked: the fog min/max tiles' "rebuild all" flag (the
thumbnail's frame has no directional shadow job, so it only defers a rebuild the next sun render does
anyway), the auto-exposure history (only touched with auto exposure on), the frame counter
`FRAME_INDEX` (advances per thumbnail; no shader in `maps/` or `tests/models` reads it). Triangles are
the file's `triangleCount` (the scene's) or the same count from the meshes (`indexCount / 3`, triangle
primitives); the size is the file's bounds. A file that does not load is listed with "no image" and not
tried again until the next load.

**Flags:** `--model-picker [filter]` (with `--shot`: opens the edit mode and the picker; the filter is an
optional value) and `--model-picker-key enter|close`. In a shot the thumbnails start at frame
`29 - (pending - 1) / 2` (at least 10), so the last ones render in the shot's own frame, before its scene
draw; from frame 0 with `--reload-test` or `--settings-change` (the reload / restart at frame 10 frees
them and they render again); from frame 10 before an ENTER. The shot waits until every listed row has its
thumbnail; `enter` acts on the frame after that and the shot follows 20 frames later; `close` closes and
shoots in the same frame. The log prints `model picker: added <file> after line <n> at <x y z>` (the
position as written), for the `--apply-edits` comparison.

**Verification** (machine A: RTX 4050 Laptop, driver 615.71.09, Ryzen 7 7435HS; scripts and images in the
round's scratch folder `phase3b/`):

- **A thumbnail render changes nothing.** Every viewpoint of `tests/models` (2), `pine_forest` (5) and the
  largest map (7, whose `sedan.glb` is not in the scene, so it is loaded for the thumbnail and unloaded):
  `--model-picker` (the last thumbnails in the shot's frame) and `--model-picker --model-picker-key close`
  vs. no flag: 0 changed pixels (28 pairs); `--reload-test` with the picker (thumbnails rendered, freed by
  the reload, rendered again) vs. the plain `--reload-test`: 0 changed pixels (3 maps).
- **No picker = before.** Every viewpoint of every map in `maps/` and `tests/models`, a yaw sweep 0/60/…/300
  from viewpoint 1 and a `--reload-test` vs. the base build: 0 changed pixels (44 pairs); box dumps identical.
- **ENTER = `add`.** `--model-picker <filter> --model-picker-key enter --edit-save auto` on a copy (rock on
  `tests/models`, tent on `pine_forest`, sedan on the largest map, which wraps the line in `use "default"` …)
  vs. `--apply-edits` of `select line <n>` + `add "<file>" <x y z>` on another copy: the map files
  byte-identical, `--dump-boxes`, `--dump-entities` and `--dump-scene-diff` identical, and the picker run's
  shot (20 frames after the live edit) = a shot of the applied copy: 0 changed pixels (3 maps).
- `--shot --shot-hud --model-picker [filter]` images of `pine_forest` and `tests/models` (unfiltered and
  filtered) and the largest map; builds without warnings (`./build.sh`, `--win`); `tools/leakcheck.sh` with
  the new runs (`picker-hud`, `-loaded`, `-reload`, `-restart`, `-enter`, `-close`, `-bad-key`): OK. The
  keys, the cursor pick and the look while typing are by hand only.

### Phase 3c: draw ranges, the draw filter, the render inspector, the list widget (§3.6, §8.2, §8.4)

**Runtime** (`runtime/monolith_scene.c`, region "Draw ranges and the draw filter", editor builds only):

- **Ranges** are `boxRanges[]` then `modelRanges[]`, as `MS_DrawGeometry` draws them. `MS_GetDrawRange` reads
  kind, material, shader, model file, slot materials, bounds and the shadow flag straight from the draw;
  triangles are a merged mesh's `indexCount / 3` (during a preview: without the selected boxes' faces, which
  the preview draws as cubes), 12 per instanced box, the file's triangles × instances for a model group.
  The object lists (`DrawRangeObjects`) are built on the first query after a version change from the draw
  order the live edits keep anyway (`boxOrder` + `instCount`: a range's boxes are items `first .. first +
  count` of its material's run; `modelSlotOf` / `modelRangeOf` for models), in `MS_Private` arrays that
  only grow: a preview step (a new version every frame, the same draws) allocates nothing, and nothing is
  built while nobody asks. `MS_DrawRangeCount` agrees with the load line: `maps/monolith` 351 box draws
  (324 merged meshes with faces, 27 instanced; 469 032 triangles), `pine_forest` 7 box draws (270 triangles)
  and 93 model draws, 744 objects (25 boxes + 719 models), as the inspector's log line shows.
- **The filter.** `MS_DrawGeometry` became `DrawBoxRange` / `DrawModelRange` (static inline, the same calls in
  the same order, shared with the game build) in its old loops, plus, in an editor block, one branch on
  `filterOn` to `DrawFiltered` (the same loops skipping hidden ranges; the transform preview's cubes are
  always drawn). With the filter off the image is the old one (below) and the cost one predictable branch.
- **What a filter change redraws.** `MS_SetDrawFilter` compares the set of hidden **objects** (a byte per box
  and model, from the range lists) with the last call's: only when it differs are the sun tiles, the
  camera-fitted map and every point / spot shadow drawn again. So the inspector setting the same names again
  after a live edit (other range indices, the same hidden objects) or a preview step costs no shadow redraw;
  objects that an edit changed were dirty anyway, and changed indices (lines inserted above) count as a
  change, which is only conservative. **Deviation:** the fog cache keeps the whole scene, so a fog bake that
  falls due (a load, a reload, an edit reaching it) while the filter hides something **waits** until the
  filter is off (R3D's bake renders the frame's draw list, which lacks the hidden ranges); meanwhile the fog
  keeps the last bake. The AO volume is baked on the CPU from every box and never sees the filter.

**Inspector** (`src/inspector.c`):

- What is hidden is kept **by name** (`m:<material>`, `s:<shader path>`; `*:` from the command line matches
  either), so it survives reloads, live edits and a renderer restart; `InspectorApply` (every frame, before
  the draw) rebuilds the per-version arrays when `scene` or `scene->version` changes (ranges, the distinct
  shaders, object → range, per item its ranges and triangles, and the overlay's scratch sized for every
  object and range, so drawing never allocates) and calls `MS_SetDrawFilter` when the hidden set or the
  version changed. Before it is first used it does nothing (`R3D_SetOutputMode` only when the view differs).
- A range is hidden when one of its materials (a model: any of its `material` / `material.N` overrides) or
  their shaders is hidden; while one item is isolated, a range is drawn only if it uses it. **Consequence:**
  a model draw is one draw of all its slots, so hiding or isolating one slot's material hides or shows the
  whole model; models drawn with their own materials are not listed and only go away with an isolate.
- **Keys:** the panel takes every key while open (`InspectorCapturesInput`: the designer, the edit mode and
  the camera wait, as for the settings panel). **Deviation:** SPACE never types into the panel's filter (it
  always hides / shows), I isolates while the filter is empty and types `i` otherwise, CTRL+I always
  isolates; material names are full of i's. Isolating clears the hidden list (it shows one item alone).
  SHIFT+F4 steps the buffer views back.
- **Outlines:** ranges in the frustum (Gribb-Hartmann planes of the overlay's own projection) within 80 m
  give their objects (each in the frustum and within 80 m), farther ones their bounds; more than 20 000
  objects are cut to the nearest by a quickselect. Depth-tested with the designer overlay's 4-step bias
  (`BeginOverlay` is a copy of `designer.c`'s, so `designer.c` is untouched). Model colours by material:
  the first overridden slot's material, else a hue per model file after the materials'; by shader: grey
  without one. Draw kind: merged blue, instanced orange, model green.
- **Labels:** the hovered range (a `MS_Raycast` through the crosshair, or the cursor while it is free) is
  always labelled and kept on screen; then the nearest on-screen ranges (bounds centre in view) in order,
  each skipped if it would overlap an earlier label or the open panel, until 24 are placed (the nearest 96
  are tried).
- **main.c:** `FrameKey` gained `outputMode` (`R3D_GetOutputMode`) and `filterGeneration`; the overlay
  needs R3D's depth, so `DrawScene`'s depth flag is also set while outlines are on (unchanged otherwise); in
  `--shot` runs the outlines are drawn without `--shot-hud` too (labels, the HUD line and the panel only
  with it). `RestartRenderer` resets `inspector.scene`, so the new scene gets the filter again. Flags:
  `--inspect`, `--output-mode`, `--inspector-panel`, and (new, for shots of the filter) `--inspect-hide
  <name>` (repeatable) and `--inspect-isolate <name>`.

**The list widget** (`src/ui.c`, region "The list widget"): `UiListKeys` (UP / DOWN / PGUP / PGDN with key
repeat, HOME / END, UTF-8 typed characters, BACKSPACE by code point, CTRL+BACKSPACE the whole filter),
`UiListRefilter` (keeps the selected item selected while it passes), `UiListDraw` (the settings panel's
look per §8.4: `UI_PANEL` + `UI_LINE`, a `UI_L` bold title in `UI_ACCENT` with the row count, the filter
field with a blinking cursor or "type to filter", rows of the caller's height with the selected one on
`UI_RAISED` with an accent bar, a thin scroll bar, a `UiHints` footer; returns the rows that fit, which the
panel passes to the next `UiListKeys` for PGUP / PGDN). Heap arrays that only grow; `UiListFree`.

**Verification** (machine A: RTX 4050 Laptop, driver 615.71.09, Ryzen 7 7435HS):

- **Off = before.** Every map in `maps/` and `tests/models` (every viewpoint, a yaw sweep 0/60/…/300 from
  viewpoint 1, a sweep at the last viewpoint looking down, `--reload-test`; the largest map, `pine_forest`
  and `tests/models` also with `--fog-cache 0.5 2` and `--ao-volume 0.5 0.6`): 62 shots pixel-identical to
  the build before the phase, box dumps identical; `--output-mode scene` = no flag; an `--inspect-hide` of a
  name nothing has = no flag; the game build (examples' test build on compiled copies of three maps through
  `01_minimal`) pixel-identical to the base editor binary.
- **The filter across live edits:** `tests/models` with moves, a duplicate, a turn and an added model, with
  a material hidden (also with the fog cache and the AO volume) or one isolated: `--edit-script` (warm
  caches) = `--apply-edits` on a copy + a shot with the same filter, 0 changed pixels at both viewpoints; a
  preview moved twice and ended with the filter on = no edit.
- Builds without warnings (`./build.sh`, `--win`, `examples/build.sh`, `test`, `--win`; the runtime with
  clang and gcc with and without `MONOLITH_EDITOR` and `MS_PATCHED_R3D`), `export_strip_test.sh`,
  `tools/leakcheck.sh` with new runs (each outline mode, labels, the panel, buffer views, a hide, an
  isolate, a hide with a live edit + preview + the fog cache, a hide across a reload, a bad `--inspect`).

### Phase 3a: the id panel and `EDIT_ID_REMOVE` (§8.1, §7.5)

**The op** (`src/mapedit.c`, `IdRemoveLines`): per selected statement the line is analysed as for
`EDIT_ID`, and a line with an `id n` prefix loses the two tokens and the blanks after them up to the next
token (`RwDropTokens`), nothing else; a `repeat` line is not split (the id names the whole line). Lines
without an id are left alone (the step's text says how many); with none at all the edit is refused ("line
204 has no id (its objects' ids are implicit already)"). `id` followed by `id remove` gives back the file
byte for byte (`RwId` inserts `id n ` before the first token). **The selection after it:** the new implicit
keys are the runtime's (a hash of the statement's tokens, identical lines taking the next free key), so
`mapedit.c` doesn't compute them: the patch keeps the selection as (statement index, object index)
(`Patch.selByStmt`; no statement is added or removed) and `ResolveSelByStmt` reads the entity ids from the
scene once the new text is loaded, live or by `--apply-edits`'s reload; `selAfter` gets them, so undo, redo
and a rebase (`MapId`) work as for every op. Script: `id remove` (both `--apply-edits` and
`--edit-script`).

**The panel** (`src/idpanel.c`, `IdPanel` in `idpanel.h`): the rows are rebuilt from the entity arrays
(contiguous runs of one statement whose key is explicit) when `scene` or `scene->version` changes, only
while the panel is open, into heap arrays that only grow; the kind is the line's keyword after its
prefixes (`MS_LineText`; `box` for a compiled map), "what" the model file, the box material or the light's
type. Edits go through the edit mode: `EditModeApplyTo` (the mode's `ApplyTo` on an explicit object list,
so a light's line works although the designer can't select lights; request warning, working copy, undo,
auto save, the HUD message), `EditModeIdKey` (CTRL+I as in the mode) and `EditModeIdFieldUpdate` (the
mode's number field takes the keys while it is open, drawn by `EditModeDrawHud` beside the panel). The
request warning's "press again" key is DELETE for the id removal and a separate code for SHIFT+DELETE, so
one doesn't confirm the other. **main.c:** F5 opens it (spectator mode, nothing else typing), it is in the
loop's `typing` from the first line on (so the inspector and the edit mode wait), `IdPanelUpdate` runs while
it captures input (open, or two frames after closing, like the settings panel: GLFW's mouse jump), the edit
mode's update is skipped meanwhile, the edit-mode cursor logic treats it like the settings panel (the panel
frees the cursor and captures it again on close), `RestartRenderer` resets `idPanel.scene`, it is drawn after
the inspector panel (also with the HUD off), `--id-panel [filter]` (with `--shot`).

**Deviations and choices** (where §8.1 left it open):

- **ENTER closes the panel.** The spectator's flight animates in `UpdateSpectator`, which doesn't run while
  a panel takes the keys; closing lets the flight run (F5 again reopens it with the filter cleared).
- The flight: from the side the camera is on (horizontally), looking down 20°, at the distance where a
  sphere around the bounds fills the vertical field of view (× 1.1, at least 2 m).
- The filter also matches the **kind** (`model`, `light`, `stairs` …), besides the id, line, file /
  material and group.
- The panel is placed like the inspector panel (right side, bottom above the toasts), wider (the hints
  footer): at 1600 × 900 its top edge overlaps the right end of long edit-mode HUD lines.

**Verification** (scripts and outputs in the round's scratch folder `phase3a/`; machine A: RTX 4050 Laptop,
driver 615.71.09, Ryzen 7 7435HS):

- Round trip `id` then `id remove` on lines without an id (a box line, one copy of a `repeat` block line
  and a 6-copy `repeat … at` line with `id 77` on the largest map; a model line, a `repeat` box line + a
  light line together on `pine_forest`): `--apply-edits` and `--edit-script` + `save` leave the file
  byte-identical (`cmp`), on both maps.
- `--edit-script` vs `--apply-edits` of `id remove` scripts (the largest map: `id 4`, then `id 5` (40
  stairs boxes) + `id 7`, undo, redo; a copy of `pine_forest` given ids first: a light's line, one copy of a
  `repeat` line): `--dump-boxes` and `--dump-entities` (but the `map` line) identical; the entity dumps vs
  the unedited map differ only in those lines' ids, now `implicit`. A line without an id: refused, exit 1,
  file unchanged.
- HUD shots (`--shot --shot-hud --id-panel [filter]`): the largest map unfiltered (9 ids), `roof`, `block`;
  the `pine_forest` copy with 5 ids (box, models, light, a `repeat` line) unfiltered and `tent`;
  `pine_forest` itself ("nothing to list"); after a live `id remove` (8 rows, "1 unsaved edit"); with the
  edit mode's number field open beside a filtered panel.
- Without the panel: every map in `maps/` (every viewpoint and a yaw sweep 0/60/…/300 from viewpoint 1:
  32 shots) pixel-identical to the base binary, `--dump-boxes` identical.
- Builds without warnings (`./build.sh`, `--win`); `tools/leakcheck.sh` with new runs (`apply-id-remove`
  with a failing `id remove`, `id-panel`, `id-panel-filter`, `id-panel-live`: live `id` / `id remove` with the
  panel open and the number field). Keys, the cursor, the flight and the edits from the panel's keys were
  not testable without a window: by hand only.

### Phase 4: independent verification and its fixes

An independent verifier re-ran every claim with its own scripts (different edits from the
authors': random scripts of 87–107 edits, CRLF line endings, 24 refused commands, shadow-camera
modes, edits that move the content bounds, the conflict cases) on every map in `maps/` and
`tests/models`: no live / saved mismatch anywhere (0 changed pixels in every pair, dumps
identical), save byte-identical, no edits pixel-identical to the base build, `--asan` clean. Its
four findings, fixed by the lead:

- **An undo past the save point was lost when a new edit followed it.** `savedAt` became -1 and
  the redo stack holding the undone steps was freed, so the unsaved count was one short and a
  rebase neither re-applied nor reported them (and with a save point deeper than the current
  depth, `savedAt = -1` would have re-applied steps the file already has). Now `savedAt` stays
  the depth both texts share and the undone steps move to `MapEditor.lost[]`: they count as
  unsaved, a save clears them, and a rebase reports them as failed ("undone past the save point:
  the file keeps it"), as in the case without the new edit (`tools/leakcheck.sh`:
  `edit-script-lost`).
- **A tiny `scale` / `size` wrote a zero-volume box** (6 written decimals). Refused now: a box or
  block side that would be written thinner than 0.000001 m, a size below it, a model scale that
  would be written as 0.
- **An unknown `--inspect-hide` / `--inspect-isolate` name was ignored silently** (a typo made a
  filter shot prove nothing). It now exits 1 with the name, after the load.
- **§7.2 did not say what a typed grab value does with a plane lock** (along the plane's first
  axis): added.

Not checkable without a window or a moving camera: sun tiles the eye reaches only after an edit
(a shot never moves after its edits), interactive mouse drags (snap / precision), Windows shots.

### After the round: the HUD without key lists, the keybindings panel, rotate by mouse

At the developer's request: the HUD shows state only (the spectator's two key lines, the edit
mode's key list, the modal's and the inspector's key hints are gone; the speed moved into the
status line; the unsaved count keeps `CTRL+S save` and the red list its clear key, the one action
each state needs). **ESC** with nothing to close shows one *See keybindings* button; the panel
(`src/keyspanel.c`, the `UiList` widget) has one section per mode and a search over all of them
(`--keys-panel [section]` for shots). While the settings panel, the id panel, the model picker
or the keybindings are open, the inspector steps aside: its panel closes and its outlines,
labels and HUD line wait until that panel closes (its draw filter stays). Rotate by mouse (§7.2)
follows the horizontal motion when the cursor starts on the object; before, a start on the
object gave almost no swept angle, so the mouse seemed to do nothing (and boxes need 45° for
their first 90° step).

### After the round: rotate "still doesn't work" (the cause, the fix, the input shim)

The developer reported that **R** with the mouse still did nothing after the change above. The
cause was not in the input path: the developer's own working copy of that session
(`unsaved/monolith-20261028-141909.map` in the settings folder) holds exactly one edit,
`repeat 3 0 0 9 block 9 0 13 2.2 20 2.2` split into three lines with the middle copy now
`id 10 block 9 0 22 2.2 20 2.2`, the same numbers as before. That is a committed rotate of a
2.2 × 2.2 m hypostyle pillar: boxes turn only in 90° steps about their own centre, and a square
footprint turned 90° is the same box, so the preview showed nothing, the commit wrote nothing
visible and still split the `repeat` line and added an id. A second effect made the mouse look
dead even on other boxes: the turn about the vertical was read as the angle swept around the
pivot's screen position, and seen from eye height a horizontal drag below a tall pillar's pivot
(at half its height) sweeps only a few degrees, short of the 45° the nearest-90 snap needed (a
150 px drag in `tests/models` gave 8°, so nothing). Also found: `--edit-modal "r … commit"` built
the committed op with a stand-in camera, so a model turned about the view axis was committed about
(0, 0, 1) instead of the axis the preview showed (4 042 pixels off).

Fix (`src/transform.c`): an axis across the view follows the motion along the near side's screen
direction (0.5°/px, the dial stays for an axis pointing at the camera); boxes take the next 90°
step at 30° past the last one; a value that changes nothing (each following object's transform
within 0.1 mm / 0.001° of its start) shows a red HUD line, and LMB / ENTER cancel with the reason
instead of writing a no-op edit; the modal keeps the camera of its last update for the commit.

Verification without a window: `--input-script` (`src/input.c`, `docs/command-line.md`): every
`src/` module reads keys and the mouse through a shim (common.h maps raylib's calls onto it; without
the flag they are raylib's own), and with a script a hidden `--shot` run gets scripted keys,
clicks and mouse motion through the interactive code path (DesignerUpdate selects by click, the
cursor is hidden / recentred / warped back as raylib's GLFW platform does, frame reuse on).
Before the fix, the pillar click + R + 200 px drag + LMB reproduced the developer's working copy
line for line, with a shot 0 pixels different from the unedited map. After it: the same input
writes nothing and the HUD says why. A box, a `block` repeat copy (selected by a click), a model
with no lock (view axis), with a Y lock, cursor on the object and 300+ px away, a mixed selection
with and without an X lock, plus a grab and a scale: each committed through the script, saved with
CTRL+S, and compared with `--apply-edits` of the op it logged: saved files byte-identical, shots 0
changed pixels (view-axis turns, which the edit language can't express: live vs a reload of the
saved file, 0); the previews (before LMB) equal the committed shots, 0 pixels. Without edits every
viewpoint of `maps/monolith` and `tests/models` is pixel-identical to the base build and the box
dump identical. What only a person can check: the feel of the drag with a real mouse (raw motion,
speed), and GLFW itself (the shim follows raylib 5.5's `rcore_desktop_glfw.c`: `DisableCursor` /
`EnableCursor` recentre through `SetMousePosition`, which also resets the previous position, so the
first frame's delta is 0, not a jump).

### Round: free rotation, phase 1 (controls)

The keys and the gizmo of §11.10 (`src/transform.c/.h`, `src/editmode.c/.h`, `src/designer.c/.h`,
`src/inspector.c`, `src/keyspanel.c`, `src/main.c`); `mapedit.c` and the runtime are phase 2's.

- **Keys** (`EditModeUpdate`): R / SHIFT+R is one `EDIT_ROTATE` with only `degrees` = ±90 (the plain
  turn); ALT+R starts the keyboard modal (`ModalStart`); ALT+SHIFT+R builds `EDIT_SET_ROTATION`
  0 0 0. Phase 1's `mapedit.c` has no case for that op and falls through to the duplicate path, so
  `SetRotation` doesn't apply it yet (`SET_ROTATION_READY = false`, a `MERGE` note beside it; the
  HUD and `--edit-modal reset` / `rot` say "not applied yet", exit 1). The merge sets it true.
  L flips `TransformModal.gizmoLocal`; SHIFT+N cycles `Designer.angleSnap` (`DesignerAngleSnap` /
  `DesignerCycleAngleSnap` / `DesignerSetAngleSnap`, 15 by default, not saved), the CTRL step of
  every rotate (keyboard and gizmo). The edit mode's HUD line shows both snaps and the gizmo space.
- **The modal has a source** (`TransformSource`): `TRANSFORM_SRC_KEYS` (G / ALT+R / S), `_RING`
  (`ring` 0..2 an axis ring, 3 the view ring) and `_BALL`. A ring sets `axisMask` / `local` as a
  lock would, so `BuildOp` and the HUD take the lock paths unchanged (a world-Y ring writes a yaw,
  exact; a local-Z ring a roll, exact); the view ring is the no-lock view axis with the dial forced;
  the trackball is one `EDIT_ROTATE` with `axis[]` (`ballAxis`, perpendicular to the drag on screen)
  + `angle`. `RingDegrees`: the start's and the cursor's rays through the ring's plane, the signed
  angle about the axis unwrapped frame by frame; a ring seen within 15° of edge-on at the start
  (`ringTrack`) uses `TrackDirection` (0.5°/px). SHIFT slows the accumulated mouse (so every source
  is 10× finer); a typed value overrides (the trackball: about the camera's up). X / Y / Z or
  G / R / S during a drag turn it into the keyboard modal. Commit: `dragging` until LMB is up; a
  release after more than 4 px of raw motion confirms (unless typed), otherwise the modal stays
  open without the button (LMB / ENTER confirm, RMB / ESC cancel).
- **Boxes' 90° rule removed** from transform.c (`boxes90`, `BoxSteps`, the default vertical axis,
  the refusals in `Classify`): boxes follow whatever `MapEditPreviewTransform` accepts; a refused
  one stays and `Compute` names it ("N boxes can't take this turn (they stay)"). With phase 1's
  mapedit.c only yaw multiples of 90° pass, so a 37° ring drag on a box is red and commits nothing.
  `hasModel` is now `hasLocal` (`ActiveAxes`: the first box, block or model's `rotationDeg`).
- **The gizmo** (`TransformGizmoHover`, `TransformGizmoBegin`, `TransformDrawGizmo`): the pivot
  (`SelectionPivot`, the modal's rule) and axes come from the designer's selection, its classes
  cached by a hash of the selection and `scene->version` / `loadVersion` (`GizmoSelection`: a heap
  array grown on change, nothing per frame). The world radius is 120 px at the pivot's depth; 72
  segments per ring, a segment drawn and hit when its middle faces the camera (`dir · toCam >
  -0.05 r`: a ring facing the camera shows whole). main.c calls `EditModeGizmoHover` before
  `DesignerUpdate` (only with the edit mode's free cursor, no panel / modal / RMB); an LMB press on
  a part skips `DesignerUpdate` that frame (no select / deselect) and `EditModeUpdate` starts the
  drag. It is drawn in 2D after `DesignerDrawVolumes`, only in the HUD layer: plain shots are
  unchanged.
- **Turned-box outlines and picks** (`scene->boxTurned`, NULL today): `DrawObjWires` / `DrawObjTint`
  (designer.c) and the inspector's outlines draw the turned box's oriented corners; `PickBoxes`
  does the slab test in the box's frame (normal = its axis), as §11.5's `MS_Raycast`.
- **`--edit-modal`**: `r90`, `r-90`, `reset`, `rot y p r` apply at frame 10 (nothing with `cancel`);
  `ring <x|y|z|xx|yy|zz|view> <deg>` and `ball <dx> <dy>` open a gizmo drag (a ring: `scriptDeg`
  stands for the dragged angle and the virtual cursor sits on the ring at the end of the arc),
  with optional `ctrl`, `step <deg>`, `type <v>` (`r …` takes `ctrl` / `step` too);
  `gizmo [hover <part>]` only draws it (`forceHover`).
- **Verified** (phase 1 alone): both builds 0 warnings; leak check OK; every viewpoint of
  `maps/pine_forest`, `maps/monolith`, `maps/test1`, `tests/models` 0 changed pixels against the
  base build, box dumps identical. Commits on `tests/models`' rock (`rot 30 0 10`): `ring y 37.5`
  → `rot 67.5 0 10`, `ring zz 20` → `rot 30 0 30`, `r-90` → `rot -60 0 10` (exact); `ring x 15` →
  `rot 30.867478 12.95254 17.63074` (= Rx(15)·R, 7e-9); `r90` on `box -6 2.5 -9 6 2.7 -7` → `box -1
  2.5 -14 1 2.7 -2`. Through `--input-script`: a click on the Y ring, 80 px, a click commits
  (`rot 61.14 0 10`, the rock stays selected); a trackball click + (40, 20) px + ENTER = 22.36°.
- **For the merge / phase 3**: `tools/leakcheck.sh`'s input scripts press R for the old modal (now a
  90° step: they still pass, but `input-noop` no longer exercises a no-op drag: use ALT+R there,
  and add gizmo runs); `docs/user-guide.md` / `command-line.md` and §6's table still list the old
  keys; `src/input.c` has no `down` / `up` for mouse buttons, so a scripted ring *drag* (button
  held) can't be played yet (clicks open the modal, which covers the same value path).


### Round: free rotation, phase 2 (turned boxes)

Implements §11.1–11.6, 11.9 and 11.12's runtime / edit parts (the compiled form, the export, the
dumps and the bundle, §11.7–11.8, are a later step; the controls, §11.10, another stream).

**Runtime** (`runtime/monolith_scene.c`):

- **Parse.** `ParsePrimitive` takes `rot y p r` after a `box` / `block`'s six numbers (format 2;
  an error on `stairs` / `waffle` and in format 1); `rot 0 0 0` is the plain box. `TurnedBox` makes
  the record with `TurnedBoxMake` (centre and size of the box as written, the axes from
  `RotationYpr`, the models' quaternion code, and the AABB of the 8 turned corners) and adds the
  AABB through `Box`. `SceneData.turned[]` / `boxTurned[]` (allocated with the first turned box,
  then grown by every `Box`) are published as `turnedBoxes` / `boxTurned` (NULL without turned
  boxes) and freed in `FreeData`. `TurnedBoxMake` sits in the parser's editor block: the
  compiled reader needs it outside (move it next to `RotationYpr` when the reader uses it).
- **Entities:** a turned box's `MS_Transform` is centre, size, rotation.
- **Meshing.** `HideRange` / `HideGridBuild` / `HideTotal` skip turned boxes (no occluder, every
  face kept). `CellItem.turned` sorts after `big`, and `AddRanges` starts a range when it changes,
  so turned boxes get their own per-cell ranges and the axis-aligned ranges' meshes stay as they
  were. `TurnedFaces` writes the six faces of a turned box (positions with the shader's
  `M_Rotate3D` arithmetic on the box's quaternion, so a merged face lies where an instanced cube's
  would; normals and tangents turned, UVs as the cube's). `LoadBoxInstances` (shared by
  `UploadBoxes` and `UploadBoxesLive`) gives a material's buffer `R3D_INSTANCE_ROTATION` only when
  one of its boxes is turned (identity for the others).
- **Collision.** `OverlapsTurned`: 15-axis SAT of the query AABB against the box (touching is no
  hit; cross products shorter than 1e-6 skipped), only for candidates whose AABB already overlaps.
  `MS_ColliderTop` / `MS_ColliderBottom`: `TurnedSpanY` takes the y range of the polytope box ∩
  prism over the footprint from its candidate vertices (corners inside, the 12 edges cut by the 4
  side planes, the 4 vertical prism edges clipped by the box's slabs); axis-aligned colliders return
  `max.y` / `min.y` exactly and walkable. Walkable: the most upward face normal's y ≥ cos 46°.
- **Raycast:** a turned box's slab test in its own frame; the normal is its axis in the world.
- **AO volume:** `AoSplat` dispatches; `AoSplatTurned` visits the same AABB window and measures each
  voxel centre in the box's frame (outside: the length of the positive part, inside: minus the depth).
- **Live edits.** `BoxKey` / `BoxSame` include the turned record, so a box that turns, un-turns or
  changes its rotation is a changed box (dirty regions, ranges rebuilt, buffers re-made); the
  ranges' old/new walk compares `turned` like the cell. Preview: `MS_EditPreviewSet` gives a box
  with a rotation its turned AABB (a turned box's record follows, restored at End), the preview
  buffer always has rotations (identity for the others), and turned boxes of a **merged** material
  are drawn from a per-range preview mesh built by `TurnedFaces` (their cubes get scale 0): an
  instanced cube's float normal shades 1–3 levels apart from the merged mesh's SNORM8 normal, so a
  cube preview differed from the commit in up to 1 257 pixels; with the mesh, 0.

**Player** (`src/player.c`): `PlayerStep(p, wish, run, jump, dt)` is the physics, `UpdatePlayer`
reads the input and calls it. Every `max.y` / `min.y` goes through `MS_ColliderTop` / `Bottom`; a
turned box is climbed only when walkable or entirely within `STEP_HEIGHT` of the feet; the feet
land `TURNED_LAND_GAP` (0.5 mm) above a turned box (an exact contact with a slope reads as a tiny
overlap through float rounding, which then blocked walking downhill); the step-down snap
(`SnapDown`, only with turned boxes in the map) probes a slab `STEP_HEIGHT` deep under the feet,
re-probing above an axis-aligned top it finds, and snaps onto the first turned walkable box
(its top may be up to 1 cm above the feet: the ramp's crest where it meets the platform).

**Edits** (`src/mapedit.c`): every turn (also the plain `turn <yaw>`) goes through `RwTransform`;
`RwTurn` is gone. Box and block lines: `BoxTurn` composes the rotation (`TurnYpr` = `ModelTurn`'s
exact yaw / local-roll rules, else decomposition at 6 decimals; `EDIT_SET_ROTATION`: the typed
angles), moves the centre (global: about the pivot; local and set: in place), and when
`MatYpr(new)` is a signed permutation (`SignedPermutation`, exact for multiples of 90° thanks to
`SinCosDeg`) writes plain corners (`box`: the smaller first; `block`: the size tokens' text
permuted, `y0` from the centre) and drops `rot` (`RwRot`), else keeps the written extents and
writes `rot`. Turned-box scale: the centre about the pivot, each own-axis extent by the factor of
the nearest world axis (`ModelAxisFactors`, as models). `size` needs nothing new (the corners are
the box before the turn). `rotation <yaw> <pitch> <roll>` in the script; `preview rotation` too.
The refusals for non-90° box turns, X / Z axes and mixed boxes and models are gone; "a box turned
by 180 degrees is the same box" stays for a plain turn of only unturned boxes.
`MapEditPreviewTransform` uses `BoxTurn` too (permuted size and rotation 0 for an axis-aligned
result).

**Verification** (base: the build before this phase): every viewpoint, a yaw sweep 0..300 and a
`--reload-test` of `maps/monolith`, `maps/pine_forest`, `maps/test1` and `tests/models`: 0 changed
pixels; their `--dump-boxes`, `--dump-scene`, `--dump-entities` and every viewpoint's `--pick`
identical; viewpoint 1 with `--ao baked` (the occlusion volume) 0 pixels. 20 edit scripts the old
editor accepted (90° box / block / pivot / split turns, model turns, scale, size, move, duplicate)
write byte-identical files with both builds; the two it refused (`turn 45`, `turn 0 90 0` of a box)
now work. `tests/rotated` (the test package): live (`--edit-script`) vs `--apply-edits` + reload
for a 37.5° turn (rot added), turned back (rot dropped), a 90° X turn of a plain box (corners), a
local turn of a turned box, a block turn, a whole `repeat` line about a pivot, `rotation 0 0 0` on
a box and a model, a split copy moved and turned, scale, size, duplicate: 0 pixels in all three
viewpoints (the block turn also with `--ao baked`, the occlusion volume's live update); previews vs commits 0 pixels except the pivot turn of a whole
`repeat` line (6 pixels: the commit writes the turned steps with 6 decimals, the preview places
each copy exactly). `tests/unit/collision_test.sh`: up the ramp onto the platform with the feet on
the slope (max error 0.5 mm, the gap), down without a frame off the ground, standing still on the
slope, stopped 1.67 m before the wall's AABB would have stopped it, at its true face; the raycast
normal of the yawed wall (0.707, 0, 0.707).

**Open / left for the next steps:** the compiled writer still writes a turned box's AABB as a plain
box (a `--compile` of `tests/rotated` loses the turns until §11.7 lands; `WriteCompiled` /
`ReadCompiled`, `CM_BOX_TURNS`); `--dump-boxes` / `--dump-scene` print turned boxes like plain ones
(§11.8, `main.c`'s `DumpBoxes`, `src/dumpscene.c`); `src/designer.c`'s `--pick` prints a box's
normal with `%.0f` (a turned face shows `1 0 1`; the bundle's `face normal` is exact).

### Round: free rotation, phase 2b (compiled, export, dumps)

Implements §11.7–11.8.

**Compiled maps** (`runtime/monolith_scene.c`, region "Compiled maps"; `docs/compiled-map.md`):
`CM_VERSION` 3, section 17 `CM_BOX_TURNS` ("box turns", `CmBoxTurn { u32 box; f32 rotationDeg[3]; }`,
16 bytes, `_Static_assert`ed), one record per turned box in box order. A turned box's `CM_BOXES`
record holds the box as written. That needs the written corners, which `SceneData` did not keep (a
record's `center ± size / 2` does not give back the corners bit for bit), so `TurnedBox` now also
stores them in `SceneData.turnedWritten[]` (parallel to `turned[]`, ordered corners, freed in
`FreeData`); only the writer reads it. `CmReadBoxTurns` (after the boxes) checks each record (box <
box count, strictly increasing, finite angles, not all 0 as the parser never makes `rot 0 0 0` a
turned box, and the parser's checks on the turned centre / size / AABB), calls `TurnedBoxMake` on the
stored corners and puts the AABB in `boxes[]`. `TurnedBoxMake` moved out of the parser's editor block
to "Small helpers: turned boxes". `CmReadHeader` accepts versions `CM_VERSION_OLDEST` (2) to 3; the
number of sections it expects follows the version (16 / 17), so a version 2 file has no turns
section and loads as before. The writer always writes 3.

**Dumps:** `--dump-boxes` appends ` rot … center … size …` (`%a`) to a turned box's line;
`--dump-scene` / `--dump-scene-diff` append the same to its `box` line (after `material`), and the
`counts` line gets ` turned <n>` only when n > 0. Maps without turned boxes dump byte for byte as
before.

**Bundle** (`src/launcher.c`, format 4, `bundle-reference.md`): the hit of a turned box gets a line
`- turned: rot y p r`; a selection table of a line with turned boxes gets a last column
`rot (yaw pitch roll)` and a sentence that min / max / centre / size are world bounds; `request.json`
gives a turned box `"rotationDeg"` in `hit` and in `selection[].objects[]`. Region and volume objects
are unchanged (bounds only).

**Export:** the README text on `colliders` (world AABBs; a turned box's entry is its bounds, the exact
test is `MS_Collides`), `turnedBoxes` / `boxTurned`, `MS_ColliderTop` / `Bottom`; `docs/export.md`
likewise. Nothing a game build needs for turned boxes is in an editor block.

**Verification** (base: `scratchpad/base-linux/monolith`, the build before the round):
- `maps/monolith`, `maps/pine_forest`, `tests/models`: `--dump-boxes`, `--dump-scene`,
  `--dump-scene-diff`, `--dump-entities` identical to the base build.
- Round trip (`maps/monolith`, `maps/pine_forest`, `tests/models`, `tests/rotated`): compiled twice
  into a copy of the package, byte-identical (2 728 392 / 108 112 / 8 112 / 7 272 bytes); the four dumps
  from the `.mapb` equal the text's (all but the `map` line; `tests/rotated`: `turned 9`, every
  `rot` / `center` / `size` equal); every viewpoint's shot 0 changed pixels.
- Version 2: the base build's `.mapb` of the three maps (version 2, 16 sections) loads in the new
  build: the four dumps equal the text's, every viewpoint 0 pixels.
- `tests/unit/compiled_test.sh` now runs on `tests/models` and `tests/rotated` (PASSED, 18 377 and
  16 703 loads); the turned cases (box out of range, 0xFFFFFFFF, unsorted, twice, NaN / ±inf angles,
  `0 0 0`, `-0 0 -0`, a written box whose turned centre overflows) are refused, a changed finite
  angle loads; versions 1 and "2 with a version 3 section table" are refused. The models-specific
  cases (uniforms, lights, model path) run only on a package without turns.
- `tests/unit/export_strip_test.sh`: OK. The collision test also passes built as a game (no
  `MONOLITH_EDITOR`) on a compiled `tests/rotated` with its `.map` moved away.
- Export of `tests/rotated` and `maps/pine_forest` into scratch, copied to fresh folders, `./build.sh`
  and `./build.sh --win` built; every viewpoint rendered by `examples/01_minimal` + the test harness
  linked against the export's `monolith_scene.c` and prebuilt libraries: 0 changed pixels against
  `monolith --shot` (3 + 5 viewpoints).
- `--submit` (stub terminal, scratch requests dir) on `tests/rotated` with lines 136 and 149 selected:
  the `rot` column and `"rotationDeg"`; line 137 (a plain box): no rotation anywhere. The same
  submit on `tests/models` with the base and the new build: only the format number (3 → 4) and the
  binary's path differ.
- Both targets, the examples and their test build without warnings; the runtime with and without
  `MONOLITH_EDITOR` / `MS_PATCHED_R3D`: 0 warnings; `tools/leakcheck.sh`: OK.

**Open:** `src/main.c`'s header comment and `docs/command-line.md` don't yet mention the turned-box
suffix of `--dump-boxes` / `--dump-scene` (both outside this step's files). The exported runtime has
older comments outside the editor blocks that mention "editor builds" (the compiled-map reader and
`InitData`, before this round).
