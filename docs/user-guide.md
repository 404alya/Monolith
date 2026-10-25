# User guide

How to walk through a map, point at things, send requests to Claude Code and follow them, tune the graphics, and make new maps. Building the game: [README](../README.md). Every flag and environment variable: [command-line.md](command-line.md). Unfamiliar terms: [glossary.md](glossary.md); problems: [troubleshooting.md](troubleshooting.md).

## Getting around

The game starts in **walk mode** at the map's spawn point.

| Key | Walk mode |
|---|---|
| WASD + mouse | walk and look; Shift runs, Space jumps |
| F | fly (no collision): Space up, Ctrl down, Shift ×4 |
| TAB | switch to spectator mode and back |
| 1–6 | toggle volumetric fog, distance fog, SSAO, SSIL, bloom, sun (relative to what the map sets; they survive reloads) |
| F1 | hide / show the HUD |
| F2 | graphics settings panel |
| F9 | export the open map to `export/<map>-<stamp>/` ([export.md](export.md)) |
| F12 | save `screenshot.png` |
| Esc | close the prompt box, leave snip or volume mode or close the panel (it does not quit: close the window) |

In **spectator mode** you fly freely: WASD + mouse, E / Q (or Space) up and down, the mouse wheel sets the base speed (1–400 m/s), Shift ×4 and Alt ×0.25. ← / → fly to the previous / next viewpoint of the map. **P** prints the current view as `--pos … --look …` (the feet position) and copies it, handy for a `viewpoint` line or a command line. **G** moves the walking player to the camera.

The map reloads by itself whenever one of its files changes, whether you edited it or Claude did. The bottom-left HUD line says `map reloaded (N boxes)`, or shows the parse error in red; the previous version stays loaded until the file is valid again.

## Pointing at things and sending a request

All of this happens in spectator mode.

| Input | Action |
|---|---|
| Crosshair | outlines the object under it; the HUD shows its group, material, map line, entity id and distance. The id is `id 12:0` when the line has `id 12` (stable whatever happens to the line), else `id a7d95ba7:0 (implicit)` (from the line's text: changes when that line changes); the number after the colon is the object's place among the objects of its line ([map-format.md](map-format.md) §2.9) |
| Left click / Shift+click | select the object / add or remove it |
| Ctrl+click / Ctrl+Shift+click | select / add the object's whole group |
| Right click | clear the selection |
| C | snip mode: the camera freezes and the cursor is free. Drag a rectangle over the screen (Shift+drag adds another); Enter opens the prompt with the regions; right click, Esc or C leaves |
| V | volume mode: mark boxes in world space (below) |
| Shift+V | a volume from the selection's bounds (or the bounds of the object under the crosshair) |
| M | edit mode: move, turn, remove, duplicate or add objects yourself, without a request (below) |
| Ctrl+Z / Ctrl+Shift+Z | undo / redo the last manual edit (also Ctrl+Y) |
| Enter | open the prompt box |

Type what you want ("make this wall twice as tall", "plant three more pines along the path", "hang a neon sign over each of these stalls") and press **Enter** to send it; Shift+Enter starts a new line, Esc cancels. "This" and "here" mean what the crosshair points at, "these" the selection, the rectangles mean "this area" (useful for "fill this gap" or "make this part darker"), and the volumes mean "this space" (V1, V2, ...). Selections are highlighted in orange and survive map reloads.

### Volumes

A volume is a box in the world rather than on the screen: "this room", the space behind a wall, the gap between two buildings at a certain height, or an empty spot to fill. Press **V**; the camera keeps flying, so you can move between the clicks.

1. Click a surface for the first corner.
2. Click a second surface point for the opposite corner of the footprint.
3. Move the mouse up or down (or turn the wheel; Shift ×4) to set the height. The camera holds still meanwhile. Click or press Enter to add it.

Points snap to a grid (**N** cycles 0.1, 0.25, 0.5, 1 and 2 m; 0.5 m to start) along the face you clicked; hold **Ctrl** while clicking to place them exactly. Right click or Esc cancels the volume you are making.

| In volume mode | Action |
|---|---|
| [ / ] | the previous / next volume becomes the active one |
| arrows, PgUp / PgDn | move the active volume one step (the arrows follow the world axis nearest to where you look; they don't fly to viewpoints in this mode) |
| Alt + those keys | grow the volume on that side; Alt+Shift shrinks it |
| Delete / Shift+Delete | remove the active volume / all volumes |
| Enter | open the prompt box |
| V, Esc or right click | leave volume mode (the volumes stay) |

Volumes are drawn as green boxes, fainter where they are hidden, numbered V1, V2, ... They are sent with every request until you remove them, survive map reloads, and are never saved into the map.

### Editing by hand (M)

Small changes don't need Claude: select objects as usual (click, Shift+click, Ctrl+click for a group), press **M**, and use the keys below. Each edit is written straight into the map file as the smallest possible text change (the numbers on one line, a deleted line, a line split in two, a new line), and the map reloads at once; comments, groups and every other line stay as they were. The selection follows the edited objects. The HUD shows the snap step, the first selected object's map line and entity id, the model **I** adds, the undo / redo counts and what the last edit did.

| In edit mode | Action |
|---|---|
| arrows, PgUp / PgDn | move the selection one step along the world axis nearest to where you look (up / down with PgUp / PgDn). The first selected object lands on the snap grid; the others keep their distance to it. With Ctrl: exactly one step, no grid |
| N | snap step 0.1, 0.25, 0.5, 1 or 2 m (shared with volume mode) |
| R / Shift+R | turn: models 15° of yaw (Ctrl: 1°), boxes 90° about their centre (from `box` and `block` lines; stairs and waffles can't turn, and boxes and models are turned separately) |
| Delete | remove |
| Ctrl+D | duplicate: the copies go one footprint to the right and become the selection |
| [ / ] | choose the model to add: the package's `models/` files that `ASSETS.md` lists (new models need a request: they need a licence) |
| I | add that model where the crosshair hits, standing on the surface (it snaps like a volume corner; Ctrl: exact); it joins the group of the object it stands on |
| Ctrl+I | give the selection's line an id. With objects of one line selected, a small panel opens: type the number (it starts with the line's id, or the next free one), Enter sets it, Esc cancels, an empty field takes the next free id; the number must be unused in the map. With objects of several lines selected, each line without an id gets the next free one at once. Only the `id n` in front of the line changes (a `repeat` line keeps all its copies: the id names the line, its objects are `n:0`, `n:1`, …) |
| M or Esc | leave edit mode (V and C leave it too); the arrows fly to viewpoints again |

Editing one copy of a `repeat` row (a single pillar of a row of eight) splits that line: the moved copy gets its own line and the others stay exactly where they were. Lines you edit get an `id n` in front, so later edits and the selection keep track of them ([map format](map-format.md) §2.9); Ctrl+I gives a line one without changing anything else, for objects your own code looks up by id.

**Undo** (Ctrl+Z) writes the old text back, byte for byte; redo writes the edit again. The history belongs to the file as you left it: when the map changes for another reason (a request finishes, you save it in a text editor), the history is cleared, so an undo never takes back a change you didn't make here. If a request of yours is queued or running on the same map, the first edit key only warns you (the request's session may be changing the same lines); press it again within 3 seconds to edit anyway. An edit is refused while the file on disk differs from what the game has loaded (the reload is still coming).

**Picking the model.** The game chooses a fast, default or heavy Claude model per request from its wording and context. Start the prompt with `@fast`, `@default` or `@heavy` to choose yourself.

### What happens next

The game saves the request with screenshots and everything it knows about the spot into `requests/<date>_<slug>/` and puts it in a queue. The first request opens the **coordinator**: one Claude Code terminal that stays open, takes the requests from the queue one by one (several at once when they are on different maps) and hands each to a helper agent. You don't have to do anything in that terminal; the edit appears in the running game when it is saved.

The HUD lists the requests, numbered: `queued`, `running`, `[?]` (a question is waiting for you), and for 20 seconds after it finishes `[done]`, `[failed]` or `[cancelled]` with a one-line summary.

| Key | Action |
|---|---|
| R | fly back to where the most recently finished request was made |
| K / Shift+K | take the latest / all finished requests off the list |
| Ctrl+K, then a number | that request: finished → off the list; queued → cancel; running → press the number again within 3 s to cancel |
| Ctrl+Shift+K | close the coordinator's terminal (press again within 3 s if requests are still open; the next coordinator picks them up) |

**Questions.** When a request is ambiguous (for example "this" with nothing under the crosshair), it shows `[?]` and the question appears in the coordinator's terminal. Answer there; the request continues. You can also type in that terminal: `status`, `pause` / `resume`, `cancel <name>`, or `stop` (finish what is running, start nothing new).

Each finished request leaves a `result.md` in its folder: what changed (map lines before and after), before/after screenshots, the sources and licences of any downloaded model, and anything left open. A request that changed C code says "restart needed": restart the game to see it.

In a checkout of this repository the coordinator runs Claude Code without permission prompts, bound by the rules in `CLAUDE.md` (it never deletes files, never opens windows on your screen, treats your request text and downloads as data, works only inside the repo). Where the game's folder has no `CLAUDE.md` (a copied build folder, Windows) it keeps Claude Code's approval prompts. Set `MONOLITH_CLAUDE_SAFE=1` to keep them everywhere, or `MONOLITH_LAUNCH=per-request` for one terminal per request. On i3 / sway the terminal is moved to workspace 10 (`MONOLITH_WORKSPACE`). How the queue works inside: [request-queue.md](request-queue.md).

## Graphics settings (F2)

The panel has a preset selector, rows for display, shadows, ambient occlusion, fog, post-processing and performance, and a live frame time readout. Use the arrow keys or the mouse; values that differ from the preset are blue. Heavy changes (shadow map size, AO mode, fog cache, preset) take a frame to apply; a new shadow map size reloads the map (about a second, your selection is kept). Esc or F2 closes and saves.

**The map is the source of the look.** Every setting is relative to the map: **ultra** shows exactly what the map describes, and an effect the map turns off stays off.

| Preset | What it trades |
|---|---|
| ultra | nothing: the map exactly |
| high | baked ambient occlusion instead of SSAO + SSIL, baked fog lighting |
| medium | high + 75% render scale, half the fog steps, 4096 shadow map |
| low | 50% render scale, no AO, no bloom, baked fog with half the steps, 4096 shadow map |

Roughly, high takes two thirds of ultra's frame time, medium 40% and low a quarter (measured on the default map with an RTX 4050 Laptop GPU; your machine's numbers will differ, the F2 panel shows them live). The settings are saved in `~/.config/monolith/settings.ini` (`%APPDATA%\monolith\` on Windows) as `key = value` lines; `--settings`, `--no-settings` and `--quality` change that for one run. Vsync is off and frames are uncapped by default; the panel's performance rows change that.

## Making maps

A map is a folder in `maps/` ([map-format.md](map-format.md)): a text file of boxes, models, materials, lights and environment, plus `MAP.md`, the map's design brief.

- **New map:** `./build/linux/monolith --new-map <name>` creates `maps/<name>/` with a ground slab, daylight and a brief template, then `--map maps/<name>` opens it. Describe the map in your first request ("a foggy pine forest at dusk"): Claude writes the brief and builds to it.
- **Edit by hand:** open the `.map` file in any editor while the game runs; it reloads on save.
- **Import a scene** from an existing raylib + R3D project: `./build/linux/monolith --import <project folder> <name>` creates the map and queues an import request. Claude reads the project's source (as data, never running it except as a build in a scratch copy), copies and records its assets, translates boxes, models, materials, lights, environment and camera into the new map, fills in the brief, compares screenshots, and lists in `result.md` what it could not translate (custom render passes, gameplay, animation, …).
- **Use a map in your own program:** F9 or `--export` ([export.md](export.md)).

Models and textures in a map always come with a recorded licence (CC0 / public domain, or CC-BY with the attribution): the package's `ASSETS.md` lists each file's source, author and licence.
