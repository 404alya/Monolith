# `src/editmode.c`: the edit mode

M in spectator mode: move, turn, remove, duplicate and add objects by hand.

## How edits work

The edits act on the designer's selection (LMB, SHIFT+LMB, CTRL+LMB for a group, under the cursor: in the mode the camera flies only while RMB is held, `main.c`). Each one is a minimal text edit (`mapedit.c`, [mapedit.md](mapedit.md)) of the working copy, the map text the scene shows, and goes live at once (`MS_EditSetText`: only what changed is rebuilt, nothing is read from disk). The selection follows the edited objects through their entity ids.

CTRL+S writes the working copy to the map file; in auto save mode (settings: Editor, `edit_save = auto`) every edit, undo and redo is written at once. The HUD shows "N unsaved edits" in every spectator mode.

When the file changes on disk meanwhile (a request finished, another editor), the unsaved edits are applied again on top of it (`EditModeSync`); those that no longer apply are listed in red until CTRL+SHIFT+DELETE. No request is made.

## Keys in the mode

| Key | Action |
|---|---|
| arrows / PGUP / PGDN | move one snap step along the world axis nearest to the view (as volume mode); the first selected object lands on the snap grid, the others keep their offsets. CTRL: exactly one step, no grid |
| N / SHIFT+N | snap step 0.1 / 0.25 / 0.5 / 1 / 2 m (shared with volume mode) / the angle snap 1 / 5 / 15 / 45 / 90 degrees (CTRL in a rotate; 15 by default) |
| G / ALT+R / S | the transform modal (`transform.c`, [transform.md](transform.md)): grab, rotate, scale the selection with the mouse or a typed value, X / Y / Z (SHIFT: plane) locks, LMB / ENTER commit one edit, RMB / ESC cancel |
| R / SHIFT+R | turn 90 / -90 degrees about the vertical, each object about its own centre (boxes) / origin (models): one edit, no modal (stairs and waffles can't) |
| ALT+SHIFT+R | reset the rotation (`EDIT_SET_ROTATION 0 0 0`) |
| LMB on the gizmo | the rotate gizmo (`transform.c`: axis rings, view ring, trackball): drag to turn, the release commits; a click keeps it open as the modal |
| L | the gizmo's axes: global / the active object's own (local) |
| DELETE | remove |
| CTRL+D | duplicate, the copies one footprint further along the view's right |
| I | the model picker (`modelpicker.c`, [modelpicker.md](modelpicker.md), opened by `main.c`): the package's `models/` files listed in `ASSETS.md`; ENTER adds the chosen one at the point under the cursor (origin bottom, `EditModeAddModel`), after the line of the object there, in its group |
| CTRL+I | give the selection's line an id (see below) |
| M / ESC | leave the mode (V and C leave it too); with unsaved edits the first press only says so (again within 3 s leaves; the edits stay live) |

**CTRL+I:** with objects of one line selected, the id panel (a number field, prefilled with the line's id or the next free one; digits, BACKSPACE, ENTER sets it, ESC cancels; empty = the next free id). With objects of several lines, each line without an id gets the next free one at once. Only the `id n` prefix changes (`mapedit.c`).

## Undo, redo and open requests

CTRL+Z / CTRL+SHIFT+Z (or CTRL+Y) undo / redo manual edits anywhere in spectator mode; CTRL+S saves. When a request of this editor run is queued or running on the map, the first edit after the last save (and a save) first warns; the same key again within 3 s goes ahead (until another request is sent).
