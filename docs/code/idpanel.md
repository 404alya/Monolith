# `src/idpanel.c`: the id panel

F5 in spectator mode, in or out of the edit mode ([live-editing.md](../live-editing.md) 8.1).

## The list

It lists every explicit `id N` of the map (a line's prefix that names its objects for good: `runtime/monolith_scene.h`, "Entities"), one row per line in map order: the id, the line (`line 41`; a line of a part the map imports: `parts/lake.map:41`, in a wider column, cut to fit), the kind (the box primitive's name, model, light), how many objects the line makes, the model file or the material, and the group.

The list is the shared `UiList` widget (`ui.h`): UP / DOWN / PGUP / PGDN / HOME / END move, and typing filters (the id, the line, a part's path, the kind, the file or material and the group; case-insensitive).

## Keys of the panel

| Key | Action |
|---|---|
| ENTER | fly the spectator in front of the line's objects (their bounds fill the view) and select them; the panel closes (the flight runs with the camera keys) |
| DELETE | remove the line's `id N` prefix only: the objects stay, their entity ids become implicit (`EDIT_ID_REMOVE`) |
| SHIFT+DELETE | remove the line's objects (`EDIT_REMOVE`, as DELETE in the edit mode) |
| CTRL+I | give the designer's selection's line an id: the edit mode's number field (one line), or the next free ids at once (several lines) |
| ESC / F5 | close |

## Edits and memory

Edits go through the edit mode (`EditModeApplyTo`): live, undoable, saved with CTRL+S, with the warning when a request is open on the map. The panel takes every key while open and frees the cursor (as the settings panel does).

The rows are rebuilt from the scene's entities when `scene->version` changes (heap arrays that only grow, only while the panel is open); drawing allocates nothing.
