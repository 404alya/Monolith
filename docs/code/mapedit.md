# `src/mapedit.c`: manual map edits as minimal text edits, with undo / redo

The map text stays the only source of truth. Every edit changes the numbers on a line, deletes a line, splits a `repeat` line or adds lines, and leaves every other byte as it was (comments, spacing, groups, line endings).

**A map in several files** ([map-format.md](../map-format.md) §2.10): every edit is made in the map-text file that holds the line (`MS_Ref.file`, `MS_Source.file`); see [Several files](#several-files).

In the editor (edit mode, `--edit-script`) the edits change the working copy (`MapEditor.files`: one `MapEditFile` per map-text file, keyed by its package path: the file's bytes plus the hunks of the unsaved steps), which goes live with `MS_EditSetText` (every file's working text; nothing written, no reload). `MapEditSave` writes the files that differ from disk (CTRL+S, or after every edit in auto save mode) and `MS_EditMarkSaved` tells the hot reload. Without a working copy (`--apply-edits`) every edit first checks that every map-text file on disk is still the loaded one, writes the files it changes and reloads the map (`MS_Reload`), as before.

## Edits

| Edit | What it writes |
|---|---|
| remove | deletes the object's line. One copy of a `repeat` line: the line is split so the other copies keep their exact positions, and the removed copy is left out |
| move | adds the shift to the line's position numbers (box: all six corners; block: `cx y0 cz`; stairs: `cx baseY startZ`; waffle: `x0 x1 y0 y1 z`; model: `x y z`; light: `x y z` and its clip box). One copy of a `repeat` line: split, and the moved copy gets its own plain line with its world coordinates |
| turn | any angle about any axis (see below) |
| rotation | sets the rotation (yaw pitch roll, absolute) about each object's own centre / origin: boxes by the same rule as turn (0 0 0 or an axis-aligned pose: plain corners, no `rot`), models get `rot` (0 0 0 removes it) |
| scale | factors per axis about a pivot: box corners, block centre and sizes, model position and `scale` (a turned box's extents, like a model's axes: each by the factor of the world axis nearest to it); stairs and waffles can't |
| size | a box's / block's size about its centre |
| duplicate | a new line after each source line: the copy, shifted |
| add | `id N model "models/x.glb" x y z origin bottom` after the line of the object it stands on (so it joins that object's group, in its file); wrapped in `use "default"` ... `use "<active>"` when a `use` is active there (that file's own `use` lines decide: a part starts with none), which would restyle the model |
| shape-add | `id N shape <kind> cx y0 cz sx sy sz` (`EDIT_SHAPE_ADD`: `op.shapeKind`, `op.pos` = cx y0 cz, `op.size` = sx sy sz, written with up to 4 decimals; sx, sz > 0, sy > 0 but a plane's 0): after the line of the first selected object (`op.anchor`: its file and group; wrapped in `use "default"` … `use "<active>"` when a `use` other than `"default"` is active there, as `add`), or with nothing selected at the end of the main map file (after its last non-empty line, with the same wrap rule). The new shape becomes the selection. A rebase finds the anchor by its entity id (`EditStep.anchorId`; stmt 0: no anchor) and refuses the step when it is gone |
| barrier-add | `id N barrier x0 y0 z0 x1 y1 z1 [rot y p r]` (`EDIT_BARRIER_ADD`: corners with up to 4 decimals, `rot` only when given and not 0 0 0): after the line of the first selected object (`op.anchor`: its file and group), or with nothing selected at the end of the main map file's `# barriers` block (the lines under that comment up to the first empty one), which is made at the file's end with `group "barriers"` when there is none. Barriers have no material, so no `use` wrap. The new barrier becomes the selection. A rebase finds the anchor by its entity id (`EditStep.anchorId`; stmt 0: no anchor) and refuses the step when it is gone |
| id | gives the objects' line an `id N` prefix (or changes it) and nothing else: a chosen N for one line (unique in the map, every file), or the next free one for each selected line without an id; a `repeat` line is not split (the id names the whole line) |
| move-to-file | the selection's whole lines into another map-text file (the edit script only): [move-to-file](#move-to-file) |
| id remove | the objects' lines lose their `id N` prefix (with the blanks after it) and nothing else; the objects stay, their entity ids become implicit (lines without an id are left alone; refused when no selected line has one) |
| group-rename, group-delete, group-new, group-move, group-duplicate | a whole group by name: [Groups](#groups) |

### Turn

Any angle about any axis (yaw / pitch / roll or an axis), global or local (the object's own axes), each object about its own centre / origin or orbiting a pivot.

- **Models:** `rot` composed with the turn (added when the line has none).
- **Box and block lines** ([live-editing.md](../live-editing.md) 11.9): the box's rotation R' = M R0 (local: R0 M). When R' is axis-aligned (a signed permutation) the box is written as plain corners again (box: the extents permuted; block: the sizes permuted and y0 from the centre) and a `rot` it had is removed; else its centre moves and `rot` is written / changed.
- A yaw (world Y) adds to the yaw and a local roll to the roll, exactly; any other turn is decomposed to yaw / pitch / roll with 6 decimals.
- **Lights** turn only about a pivot, globally (`turn … pivot x y z`, a whole group turned as one: R on a whole-group selection, [Groups](#groups)): the position orbits the pivot, a spot's or directional light's direction turns with 6 decimals, the `clip` box turns with it (only by a turn that is a signed permutation, i.e. 90 degree steps about the axes: an axis-aligned box stays one; otherwise refused), a whole line's `repeat` steps turn (`LightTurn`). Any other turn of a light (about its own position, `local`, `rotation`) is refused as before.
- Stairs and waffles can't turn.

### Splitting a `repeat` line

Splitting a `repeat` line keeps the copies in their map order: the rows before the changed copy, its row split around it, the rows after. Each untouched piece is written with plain `repeat` prefixes and adjusted start values when that gives exactly the same floats for every copy (whole and half numbers), else with `repeat n dx dy dz from k` (the same i * d products as before, so always exact).

## Entity ids

See `runtime/monolith_scene.h`, "Entities". A line edited by hand gets an `id N` prefix (N = the largest id in any of the map's files + 1) unless it has one, so its objects keep their identity although the line's text changed. New lines (moved copies, duplicates, added models) get a new id. The selection follows the edited objects through their ids.

## Undo / redo

Every edit is a list of hunks (file, offset, old bytes, new bytes; each file's hunks together, in ascending order, with the whole file's FNV-1a 64 before and after). Undo writes the old bytes back, so an edit followed by undo gives back every file byte for byte. Both work only while every file the step touched (working copy, or the files without one) is exactly the one the step produced (respectively started from): `scene->hash` (the combined hash) for the working copy, and each touched file's own hash.

`savedAt` is the undo depth the file and the working copy share. Steps undone past it stay on the redo stack, and when a new edit drops that stack they move to `lost[]` (the file still has them), so the unsaved count is |`undoCount` - `savedAt`| (every undo step when `savedAt` is -1) + `lostCount` + the unsaved steps dropped by an edit after undoing past the save (`droppedUnsaved`), and 1 when that sum is 0 but the working copy differs from the file (`MapEditUnsaved`).

When a map-text file changes on disk for any other reason (a request finished, another editor saved; any of the map's files), the unsaved steps keep their op and selection (entity ids): `MapEditRebase` applies them again, oldest first, to the reloaded file (each one live; the ids a step gives its lines anew are mapped for the steps after it). One whose objects are gone, or whose op fails, goes to `failed[]` (the HUD lists them); the history before the save point is dropped. Steps undone past the save point can't be replayed as ops (they are reported as failed: the file keeps them).

Without unsaved steps the history is dropped (`MapEditBegin`), so an undo never reverts a change the developer didn't make here. With `--apply-edits` (no working copy) every edit, undo and redo first checks that every map-text file on disk is still the loaded one.

## Several files

A map whose main file imports parts ([map-format.md](../map-format.md) §2.10, `MS_MapFileCount` / `MS_MapFileAt`):

- **The working copy** (`MapEditor.files[]`, `MapEditFile`) has one text per map-text file, keyed by its package path (`MS_MapFile.path`; `files[0]` the main file): the scene's file order is the parse's and can change after an edit that adds or removes an `import` line. Each has the hash of its text and of the file on disk as last read or written (`diskHash`, `onDisk`: false for a part `move-to-file` made and nobody saved yet). `workHash` is `scene->hash` (the combined hash) while the scene shows the working copy. `MapEditShowWork` puts the working copy live again (a renderer restart, provided textures); `MapEditSceneOnDisk` says whether the loaded scene is the files as last read or written.
- **Where an edit goes:** the patch (`Patch.files[]`, one `PFile` per scene file) is built over every file's text. Moves, turns, scale, size, remove, id, id remove, a split `repeat` line and a duplicate change the line's own file; `add` inserts after the anchor's line in its file (the `use` wrap from that file's own `use` lines: a part starts with none); `material-set` finds the material's lines in every file in the order of the map's text with every import pasted in (`ExpandedCompare`), replaces the first one's keys and adds missing keys to its first line (`MS_Material.file` / `line`); `paint-add` writes the `paint` line into the paint block of the painted object's file (made at its end when it has none) and the material's `paint` key onto the material's first line, in its file; `paint-set` / `paint-remove` change an entry's line wherever it is. Ids: `id n` is unique across the files (a duplicate is refused with `file:line`), the next free id is the largest in any file + 1 (`MaxId`, `MapEditNextId`).
- **Applying:** the texts form a file set (`FSet`: by path, borrowed from the working copy or read from disk and checked against the scene's per-file hashes); `FSetApply` applies a step's hunks file by file (each file's hash before and after checked, or filled for a new step); live, `FSetLive` hands every text to `MS_EditSetText` and `CommitWork` takes them over; without a working copy `FSetWriteAndReload` writes the files that changed and reloads (a failure writes them back).
- **Saving** writes only the files that differ from disk (a new part: its folder made first), after checking that none of the files changed on disk (and that a new part did not appear); the note names them: `saved 3 edits to maps/x/x.map + parts/lake.map`. `MapEditDiskUnchanged` and the rebase consider every file. Closing with unsaved edits (`MapEditWriteWork`) writes the main text to the given path and each changed part under `<path without .map>/<package path>`.
- **Lines are named** `line 41` in the main file (unchanged) and `line parts/lake.map:41` in a part: every message, `MapEditDescribe`, the script's `print`, and the HUDs (`MapEditFilePath`, `MAPEDIT_LINE_AT`).

## move-to-file

`move-to-file "<path>.map"` (an edit-script command only: no key, no panel, no menu; splitting a map is the AI's work) moves the selected objects' **whole lines** into map-text file `<path>` (package-relative, `/`-separated; empty and `.` components dropped). A selected `repeat` copy is split out first, as `move` does (a move by 0 0 0: the copy becomes a line of its own with a new id), and the step covers both (one undo).

- **The target** is a new file or a part of this map:
  - a **new file** gets the main file's format line (`monolith N`), the lines, and an `import "<path>"` line takes the place of the first moved line in the source file. Its folder is made on save;
  - an **existing part** gets the lines **at its end** (after its last line; not at the import's position: the part's own text stays in its order).
- **Scope:** before each moved line the target gets the `group` and the `use` that line had in the source (`use "default"` for none), wherever the target's own differ at that point (a new file starts with no group and `"default"`; an existing part with what its own lines leave at its end). A `group` or `use` line of the source whose scope (to the next line of its kind) held moved objects and holds no other object any more is removed too, so moving a whole group leaves no empty `group` line behind and the map's group list keeps its order. Comments, blank lines, `material` lines and every other line stay in the source.
- **Only object lines move** (boxes, blocks, stairs, waffles, models, lights): `env`, `spawn`, `viewpoint`, `material`, `import`, `paint`, `group` and `use` lines are never selected (they make no object), so they never move.
- **Refused**, with nothing written and a message: an empty selection; a target outside the package, absolute, with `..`, `\` or `"`, or not ending in `.map`; the main file; the file that holds the lines already; a file in the package this map doesn't import (`… exists but this map doesn't import it`); lines of **several files** in one call (move one file's lines at a time: the scope rules are per file); a line in no group moved to the end of a part that is in a group (a part's lines can't leave a group there); the result not loading (e.g. a moved `use` before the material's definition: `move-to-file refused: the map would not load: …`). A cycle can't arise: import lines never move, and a new file imports nothing.
- **Self-check:** the edit goes live (also with `--apply-edits`, which then writes and reloads), and the map before the move (after the split) and after it are compared entity by entity, sorted by id: the same set of ids, each with the same kind, shape and place (a box's bounds and turn, a model's file, transform, origin, collide, shadow and slot materials, a light's every field), the same material and group (by name), and the same paint entries. The order of the objects may change (lines appended to a part come earlier in the map's text). Any difference puts the texts back and refuses with the first one: `move-to-file refused (self-check: the map would not be the same): entity 84981c40:0 (box) would get another material or group` (two identical lines whose implicit keys would swap).
- **Undo** restores every file byte for byte. A new part the undone step made leaves the working copy, but a file already written to disk stays there (nothing is deleted; it is no longer imported). Redo makes it again (an identical file left on disk is taken as it is).

**Shape lines** (`shape <kind> cx y0 cz sx sy sz [rot y p r]`) are read like `block` lines shifted by one token: `Analyze` sets `Stmt.kw` to the kind word (so `kw + 1..6` are the numbers, `kw + 4` the first size and `kw + 7` the `rot` key, as after `block`) and takes the positions from `MS_PrimitiveArgs("block")`; `BlockPrim` treats `shape` as `block` where a line's layout matters. Every edit of a block works on them: move, turn, rotation, scale, size, remove, duplicate, id, move-to-file. Two differences: a turn keeps the `rot` unless the result is no rotation at all (`BoxTurn`'s `keepRot`: a quarter turn of a cylinder or a cone is not the same shape with swapped sizes, and a cube's parts would move to other faces), and a plane refuses a `size` with a height (`a plane has no height`; `scale` keeps its 0). `MapEditClassify` gives them `EDIT_CLASS_SHAPE` (`EditClassBoxLike`: the transform modal treats it as a block, and its preview keeps the rotation as the edit will); `TakeSnapshot` hashes a shape's kind, bounds and shape, and `print` names its kind `shape`.

## Groups

A group is its **name** ([editor-tools.md](../editor-tools.md) 3.1, [map-format.md](../map-format.md) §2.3): every `group "<name>"` statement with that name, in any file, is one group (a repeated name, as `"plain: grass"` in nine parts of a map, is edited as one). Five ops, each **one step** (one undo, byte for byte in every file it touched), live, rebased by op and its strings (`EditStep.groupName` / `groupNew`, `KeepOp`), all through `MoveStep` with move-to-file's self-check:

| Op (script) | What it writes |
|---|---|
| `EDIT_GROUP_RENAME` (`group-rename "<old>" "<new>"`) | the quoted name of every `group "<old>"` line in every file (the bytes of the name only: a comment on the line stays). The selection stays |
| `EDIT_GROUP_DELETE` (`group-delete "<name>"`) | every object line of the group (each entity whose group has the name: every block, every file), every `group "<name>"` line, the comment lines directly above a deleted line inside a block (not above its `group` line), the `group` / `use` lines whose scope (to the next line of its kind) held deleted objects and holds no other (`ScopeLinesGone`, move-to-file's rule), a block's blank lines when nothing else of it is left (so no empty `group` line or double blank line stays), and the `paint` lines of the deleted lines' ids in every file. `material`, `import`, `env`, `spawn`, `viewpoint`, `fogzone`, other `paint` lines and comments elsewhere stay. The selection becomes empty |
| `EDIT_GROUP_NEW` (`group-new "<name>"`) | the selection's whole lines (a `repeat` copy split out first, as move-to-file) into a new block `group "<name>"` at the end of the file that holds them, after a blank line, each with the `use` it had where the one in effect there differs; source `group` / `use` lines left without objects go. The selection stays the same objects |
| `EDIT_GROUP_MOVE` (`group-move "<name>"`) | the selection's whole lines (split as above; objects already in that group are left where they are) to the end of the group's **last block** (the last `group "<name>"` line in the map's text with every import pasted in, `ExpandedCompare`): after that block's last object line (or its `group` line when it has none), in that block's file, so also from another file; each with the `use` it had, and the `use` in effect there written back after them when it differs (the lines after keep their material) |
| `EDIT_GROUP_DUPLICATE` (`group-duplicate "<name>" "<new>" dx dy dz`) | a copy of every object line of the group in map order (each written as `duplicate` writes a whole line: shifted, a new id, without its comment) in a new block `group "<new>"` (after a blank line) right after the last object line of the group's last block, each with the `use` its source line had; the `use` in effect there is written back after the block when it differs. Paint entries are not copied. The copies become the selection |

**Refused** (nothing written, the reason in `MS_LastError()`): a name that isn't one (empty, with `"` or a control character: `GroupNameOk`), a group that doesn't exist (rename, delete, move, duplicate), a new name that exists already (rename: "group-move puts lines into a group"; new; duplicate), a rename to the same name, a group without objects (duplicate), lines of **several files** in one group-new / group-move (one file's lines at a time, as move-to-file), a selection already in the target group (move), an empty selection (new, move), the result not loading, and a failed self-check.

**Self-check** (`MoveStep`, `SnapExpect`): the map before the edit (after a split) is changed into what the map after must be, then compared entity by entity as move-to-file is (geometry, materials and the group by name, `SnapEntity.group`, and the paint entries): rename: the group's entities in the new group; new / move: the moved entities in the target group; delete: the group's entities dropped and their paint entries left out (`TakeSnapshot(…, dropGroup)`); duplicate: new entities allowed in the new group only. Every other entity must be exactly as before. The refusal names the op: `group-move refused (self-check: the map would not be the same): …`.

**The ops by name** (rename, delete, duplicate) take no lines of the selection: it only rides along as the step's selection (`selBefore`), and a rebase leaves out the objects of it that are gone instead of failing the step. Moving, turning and scaling a whole group are the plain `move`, `turn … pivot`, `scale … pivot` on `select group "<name>"` (with a pivot a turn takes the group's lights along: *Turn* above).

**Barrier lines** (`barrier x0 y0 z0 x1 y1 z1 [rot y p r]`) are read like `box` lines (`MS_PrimitiveArgs("barrier")` is box's layout; `CornerPrim`), so every edit of a box works on them: move, turn (any axis; `rot` written or removed by the same rule), rotation, scale, size, remove, duplicate, id, move-to-file. `MapEditClassify` gives them `EDIT_CLASS_BARRIER`, which the transform modal treats as a box (`EditClassBoxLike`); `TakeSnapshot` hashes a barrier's bounds and shape, and `print` names its kind `barrier`.

## Edit script

`--apply-edits <file>`: one command per line; `#` starts a comment.

| Command | Meaning |
|---|---|
| `select line [<file>:]<n> [copy <c>]` | the selection becomes every object of line n of the main map file, or of map-text file `<file>` (`select line parts/lake.map:41`; package-relative, as `MS_MapFile.path`) (with `copy`: only that `repeat` copy, all objects it makes) |
| `select group "<name>"` | every object of the group(s) with this name, in any file |
| `select id <n> [index <i>]` | the objects of the line with `id n` (`index`: only that one) |
| `select entity <hex> <index>` | the entity with this id (as `--dump-entities` prints it) |
| `select none` | empty the selection |
| `also ...` | like `select`, but adds to the selection |
| `move <dx> <dy> <dz>` | metres |
| `turn <degrees>` | yaw about each object's own centre / origin (any angle) |
| `turn <yaw> [<pitch> <roll>] [local] [pivot <x> <y> <z>]` | see below |
| `rotation <yaw> <pitch> <roll>` | the rotation itself, about each object's own centre / origin (0 0 0: a box's plain corners, a model without `rot`) |
| `scale <f> \| <fx> <fy> <fz> [local] [pivot <x> <y> <z>]` | factors (> 0) per world axis about the pivot (default: each object's own centre / origin): box corners, block centre and sizes, model position and `scale` (each model axis takes the factor of the world axis nearest to it; `local`: its own axes) |
| `size <sx> <sy> <sz>` | box / block size in metres about its centre (0 keeps that axis) |
| `remove` | remove the selection |
| `duplicate <dx> <dy> <dz>` | copies shifted by this |
| `add "<models/x.glb>" <x> <y> <z>` | after the line of the first selected object |
| `shape-add <kind> <cx> <y0> <cz> <sx> <sy> <sz>` | a new `shape` line (kind cube, sphere, cylinder, cone, capsule or plane): after the line of the first selected object, with nothing selected at the end of the main file (see *Edits*) |
| `barrier-add <x0> <y0> <z0> <x1> <y1> <z1> [rot <yaw> <pitch> <roll>]` | a new `barrier` line (an invisible wall): after the line of the first selected object, with nothing selected at the end of the main file's `# barriers` block (see *Edits*) |
| `id [<n>]` | the selection's line gets `id n` (one line), or without n each selected line without an id gets the next free one |
| `id remove` | the selection's lines lose their `id n` prefix (objects stay) |
| `paint-add ...` | see below |
| `paint-set ...` | see below |
| `paint-remove ...` | see below |
| `material-set "<name>" <keys...>` | the keys replace the same keys of the material (the first occurrence on its lines, in the map's text order across its files; later ones go) or are added to its first line (in that line's file); `"-key"` removes a key |
| `move-to-file "<path>.map"` | the selection's whole lines into that map-text file (a new part, or the end of one): [move-to-file](#move-to-file) |
| `group-rename "<old>" "<new>"` | every `group "<old>"` line becomes `group "<new>"`: [Groups](#groups) |
| `group-delete "<name>"` | the group's object lines, its `group` lines, their comments, the `use` lines left empty, its paint entries |
| `group-new "<name>"` | the selection's whole lines into a new group at the end of their file |
| `group-move "<name>"` | the selection's whole lines to the end of that group's last block |
| `group-duplicate "<name>" "<new>" <dx> <dy> <dz>` | a shifted copy of the group as a new group right after it |
| `undo`, `redo` | undo / redo one step |
| `print` | the selection (entity id, kind, line (`line parts/lake.map:41` in a part), copy); with `--edit-script` also the unsaved count and the steps a rebase could not apply |

**`turn <yaw> [<pitch> <roll>] [local] [pivot <x> <y> <z>]`:** R = Ry Rx Rz of the three angles composed on the left (global) or on the right (`local`: the object's own axes) of a model's or box's rotation. With a pivot the objects orbit it (a whole `repeat` line: its steps turn too; `local`: each about its own centre / origin). A yaw alone adds to the yaw and a local roll alone to the roll, exactly; other turns are written with 6 decimals. A box that ends up axis-aligned is written as corners, without `rot`.

**`paint-add "<material>"|own ["<atlas>"] faces <+x,-x,...>|slot <N> <x> <y> <w> <h> [nearest]`:** the draw mode's first stroke on a face / slot ([painting.md](../painting.md) 3.3, 10.3). It writes a `paint` statement for the selected object (one) in the paint block of the object's file (`# paint (draw mode)` at the end of that file, sorted by material, entity, face / slot). The material gets `paint "<atlas>"` (with `nearest`: also `paint_filter nearest`; with `own`, `nearest` goes on the statement) when it has no paint key; the line gets an id when it has none. Faces in other entries of the object in that material leave them (an entry left without faces goes).

**`paint-set "<material>"|own faces <+x,...>|slot <N> [<x> <y> <w> <h>] [nearest|trilinear]`:** the entry holding the first of the faces (or the slot) gets these faces (taken from the object's other entries) and rectangle. `nearest` / `trilinear`: an `own` entry's `paint_filter` (several objects selected: each one's entry, without a rectangle).

**`paint-remove "<material>"|own [faces <+x,...>|slot <N>]`:** the faces leave the object's entries (an entry left without faces goes), or the slot's entry goes; without faces / slot: every entry of the object in that material.

**Paint on shapes** (`EDIT_PAINT_ADD` / `SET` / `REMOVE` on a `shape` object, from the draw mode): the face bits are the shape's part bits and the statement names its parts (`PaintPartName`: `side,top`; a cube's `+x` … `-z`); `ReadPaintLine` reads a statement's names against its entity's kind (`PaintShapeKind`: `MS_FindEntity` of `id:index`; a statement whose entity is not a shape reads box faces only). The edit script's `faces <…>` words stay box faces (a cube's parts are the same).

### `--edit-script` only

`MapEditRunLiveScript`: every edit, undo and redo goes live and prints a `monolith: live edit <ms> (...)` line too.

| Command | Meaning |
|---|---|
| `save` | write the working copy's changed files (CTRL+S) |
| `disk <file> [<part>]` | the map file (or map-text file `<part>`, package-relative) becomes `<file>`'s bytes and is reloaded as the hot reload would; unsaved edits are rebased (printed: what applied, each failed step) |
| `preview move <dx> <dy> <dz>` | the selection follows a transform without a text (`MS_EditPreviewBegin` / `Set`: shifted from where it was when the preview began); the next edit ends it |
| `preview turn ...` / `preview scale ...` / `preview rotation ...` | the same with a turn / scale / rotation (their arguments), as the transform modal previews it |
| `preview end` | everything back as before the preview (`MS_EditPreviewEnd`) |

Each command prints one `edits: ...` line; the first failing one stops the script (exit 1).
