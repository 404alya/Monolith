# `src/mapedit.c`: manual map edits as minimal text edits, with undo / redo

The map text stays the only source of truth. Every edit changes the numbers on a line, deletes a line, splits a `repeat` line or adds lines, and leaves every other byte as it was (comments, spacing, groups, line endings).

In the editor (edit mode, `--edit-script`) the edits change the working copy (`MapEditor.work`: the file's bytes plus the hunks of the unsaved steps), which goes live with `MS_EditSetText` (nothing written, no reload). `MapEditSave` writes it to the file (CTRL+S, or after every edit in auto save mode) and `MS_EditMarkSaved` tells the hot reload. Without a working copy (`--apply-edits`) every edit writes the file and reloads it (`MS_Reload`), as before.

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
| add | `id N model "models/x.glb" x y z origin bottom` after the line of the object it stands on (so it joins that object's group); wrapped in `use "default"` ... `use "<active>"` when a `use` is active there, which would restyle the model |
| id | gives the objects' line an `id N` prefix (or changes it) and nothing else: a chosen N for one line (unique in the file), or the next free one for each selected line without an id; a `repeat` line is not split (the id names the whole line) |
| id remove | the objects' lines lose their `id N` prefix (with the blanks after it) and nothing else; the objects stay, their entity ids become implicit (lines without an id are left alone; refused when no selected line has one) |

### Turn

Any angle about any axis (yaw / pitch / roll or an axis), global or local (the object's own axes), each object about its own centre / origin or orbiting a pivot.

- **Models:** `rot` composed with the turn (added when the line has none).
- **Box and block lines** ([live-editing.md](../live-editing.md) 11.9): the box's rotation R' = M R0 (local: R0 M). When R' is axis-aligned (a signed permutation) the box is written as plain corners again (box: the extents permuted; block: the sizes permuted and y0 from the centre) and a `rot` it had is removed; else its centre moves and `rot` is written / changed.
- A yaw (world Y) adds to the yaw and a local roll to the roll, exactly; any other turn is decomposed to yaw / pitch / roll with 6 decimals.
- Stairs, waffles and lights can't turn.

### Splitting a `repeat` line

Splitting a `repeat` line keeps the copies in their map order: the rows before the changed copy, its row split around it, the rows after. Each untouched piece is written with plain `repeat` prefixes and adjusted start values when that gives exactly the same floats for every copy (whole and half numbers), else with `repeat n dx dy dz from k` (the same i * d products as before, so always exact).

## Entity ids

See `runtime/monolith_scene.h`, "Entities". A line edited by hand gets an `id N` prefix (N = the largest id in the file + 1) unless it has one, so its objects keep their identity although the line's text changed. New lines (moved copies, duplicates, added models) get a new id. The selection follows the edited objects through their ids.

## Undo / redo

Every edit is a list of hunks (offset, old bytes, new bytes). Undo writes the old bytes back, so an edit followed by undo gives back the text byte for byte. Both work only while the text (working copy, or the file without one) is exactly the one the step produced (respectively started from).

`savedAt` is the undo depth the file and the working copy share. Steps undone past it stay on the redo stack, and when a new edit drops that stack they move to `lost[]` (the file still has them), so the unsaved count is |`undoCount` - `savedAt`| (every undo step when `savedAt` is -1) + `lostCount` + the unsaved steps dropped by an edit after undoing past the save (`droppedUnsaved`), and 1 when that sum is 0 but the working copy differs from the file (`MapEditUnsaved`).

When the file changes on disk for any other reason (a request finished, another editor saved), the unsaved steps keep their op and selection (entity ids): `MapEditRebase` applies them again, oldest first, to the reloaded file (each one live; the ids a step gives its lines anew are mapped for the steps after it). One whose objects are gone, or whose op fails, goes to `failed[]` (the HUD lists them); the history before the save point is dropped. Steps undone past the save point can't be replayed as ops (they are reported as failed: the file keeps them).

Without unsaved steps the history is dropped (`MapEditBegin`), so an undo never reverts a change the developer didn't make here. With `--apply-edits` (no working copy) every edit, undo and redo first checks that the file on disk is still the loaded map.

## Edit script

`--apply-edits <file>`: one command per line; `#` starts a comment.

| Command | Meaning |
|---|---|
| `select line <n> [copy <c>]` | the selection becomes every object of map line n (with `copy`: only that `repeat` copy, all objects it makes) |
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
| `id [<n>]` | the selection's line gets `id n` (one line), or without n each selected line without an id gets the next free one |
| `id remove` | the selection's lines lose their `id n` prefix (objects stay) |
| `paint-add ...` | see below |
| `paint-set ...` | see below |
| `paint-remove ...` | see below |
| `material-set "<name>" <keys...>` | the keys replace the same keys of the material (the first occurrence on its lines; later ones go) or are added to its first line; `"-key"` removes a key |
| `undo`, `redo` | undo / redo one step |
| `print` | the selection (entity id, kind, line, copy); with `--edit-script` also the unsaved count and the steps a rebase could not apply |

**`turn <yaw> [<pitch> <roll>] [local] [pivot <x> <y> <z>]`:** R = Ry Rx Rz of the three angles composed on the left (global) or on the right (`local`: the object's own axes) of a model's or box's rotation. With a pivot the objects orbit it (a whole `repeat` line: its steps turn too; `local`: each about its own centre / origin). A yaw alone adds to the yaw and a local roll alone to the roll, exactly; other turns are written with 6 decimals. A box that ends up axis-aligned is written as corners, without `rot`.

**`paint-add "<material>"|own ["<atlas>"] faces <+x,-x,...>|slot <N> <x> <y> <w> <h> [nearest]`:** the draw mode's first stroke on a face / slot ([painting.md](../painting.md) 3.3, 10.3). It writes a `paint` statement for the selected object (one) in the file's paint block (`# paint (draw mode)` at the end of the file, sorted by material, entity, face / slot). The material gets `paint "<atlas>"` (with `nearest`: also `paint_filter nearest`; with `own`, `nearest` goes on the statement) when it has no paint key; the line gets an id when it has none. Faces in other entries of the object in that material leave them (an entry left without faces goes).

**`paint-set "<material>"|own faces <+x,...>|slot <N> [<x> <y> <w> <h>] [nearest|trilinear]`:** the entry holding the first of the faces (or the slot) gets these faces (taken from the object's other entries) and rectangle. `nearest` / `trilinear`: an `own` entry's `paint_filter` (several objects selected: each one's entry, without a rectangle).

**`paint-remove "<material>"|own [faces <+x,...>|slot <N>]`:** the faces leave the object's entries (an entry left without faces goes), or the slot's entry goes; without faces / slot: every entry of the object in that material.

### `--edit-script` only

`MapEditRunLiveScript`: every edit, undo and redo goes live and prints a `monolith: live edit <ms> (...)` line too.

| Command | Meaning |
|---|---|
| `save` | write the working copy to the map file (CTRL+S) |
| `disk <file>` | the map file becomes `<file>`'s bytes and is reloaded as the hot reload would; unsaved edits are rebased (printed: what applied, each failed step) |
| `preview move <dx> <dy> <dz>` | the selection follows a transform without a text (`MS_EditPreviewBegin` / `Set`: shifted from where it was when the preview began); the next edit ends it |
| `preview turn ...` / `preview scale ...` / `preview rotation ...` | the same with a turn / scale / rotation (their arguments), as the transform modal previews it |
| `preview end` | everything back as before the preview (`MS_EditPreviewEnd`) |

Each command prints one `edits: ...` line; the first failing one stops the script (exit 1).
