# `src/groupspanel.c`: the groups panel

F6 in spectator mode, in or out of the edit mode ([editor-tools.md](../editor-tools.md) 3.4). The id panel ([idpanel.md](idpanel.md)) is its model.

## The list

A group is its **name**: every `group "<name>"` statement with that name, in any file, is one row ([editor-tools.md](../editor-tools.md) 3.1). The rows are sorted by **area** (the text before the first `": "`: `"plain: house"` is in area `plain`; a name without one sorts by itself, before an area of the same text), then by name. An area with more than one group gets a **header row** (the area, its group count and objects) with its groups indented under it.

A group's row: its name (`x9` after it when nine statements share the name), its objects (boxes, models, lights, barriers), the map-text file of its first object (`parts/plain.map`, the main file by its own name) and the first–last line of its objects in that file, or `9 files` when its objects are in several.

The list is the shared `UiList` widget (`ui.h`): UP / DOWN / PGUP / PGDN / HOME / END move, typing filters (a group's name, which holds its area, and its file; case-insensitive). A header row stays while one of its groups passes.

## Keys of the panel

| Key | Action |
|---|---|
| ENTER | select the group (a header row: all its groups; lights can't be selected), fly in front of it (as the id panel flies: its bounds without lights fill the view); the panel closes |
| CTRL+R | rename: a text field above the panel with the group's name; ENTER applies (`EDIT_GROUP_RENAME`), ESC closes the field; a refusal (the name exists, a `"`) keeps it open with the reason in the edit mode's HUD line. Not on a header row |
| DELETE | delete the group: the first press arms it ("DELETE again to delete the group …" above the panel), a second press within 3 s deletes (`EDIT_GROUP_DELETE`); a header row: each of its groups, one step each |
| CTRL+D | duplicate as CTRL+D in the edit mode (`EditModeDuplicateGroups`): the row's group (a header row: each of its groups) copied as `"<name> 2"` (the next free number), shifted one footprint along the view's right; the copies become the selection |
| CTRL+N | a new group from the designer's selection: the text field (empty); ENTER applies (`EDIT_GROUP_NEW`). Needs a selection |
| CTRL+G | put the designer's selection into the highlighted group (`EDIT_GROUP_MOVE`). Not on a header row |
| ESC / F6 | close (ESC first closes an open text field) |

The text field takes printable characters but `"` (UTF-8 as typed), BACKSPACE (a whole character) and ENTER / ESC. The **view-only build** (`MONOLITH_EDITING` 0): filter and ENTER only; CTRL+R, DELETE, CTRL+D, CTRL+N and CTRL+G show `MSG_VIEW_ONLY`.

## Edits and memory

Edits go through the edit mode (`EditModeApplyTo`, `EditModeDuplicateGroups`): live, undoable, counted as unsaved until CTRL+S, and the first one warns when a request is open on the map. The panel takes every key while open (and two frames after closing) and frees the cursor (as the id panel does).

The rows are rebuilt from the scene's groups and entities when `scene->version` changes (or the scene: a renderer restart sets `scene` to NULL), only while the panel is open: `items[]` only grows; the rebuild's temporaries (the name order, the (row, file) pairs for the file count) are allocated and freed in it, an event, never a frame of its own. Drawing allocates nothing. A row's names point into the scene's strings; every edit copies them first (the edit gives the scene new strings).

## Flags

`--groups-panel [filter]` with `--shot --shot-hud` opens it (with `filter` typed: the next argument unless it starts with `--`). Unlike the id panel, a scripted shot (`--input-script`) drives its keys in the Linux editor too (`main.c`: `groupsKeys`), so F6, the filter, the fields and the edits are tested end to end; with `CTRL+S` in the script the edits are saved to the map's files.
