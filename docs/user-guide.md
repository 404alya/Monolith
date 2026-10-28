# User guide

How to walk through a map, point at things, send requests to Claude Code and follow them, tune the graphics, and make new maps. Building the game: [README](../README.md). Every flag and environment variable: [command-line.md](command-line.md). Unfamiliar terms: [glossary.md](glossary.md); problems: [troubleshooting.md](troubleshooting.md).

## Getting around

The game starts in **walk mode** at the map's spawn point.

The HUD at the top shows what is going on (the mode, the effects, the selection, edit mode and its unsaved edits), not the keys: press **Esc** for the keybindings (the tables below have them too).

| Key | Walk mode |
|---|---|
| WASD + mouse | walk and look; Shift runs, Space jumps |
| F | fly (no collision): Space up, Ctrl down, Shift ×4 |
| TAB | switch to spectator mode and back |
| 1–6 | toggle volumetric fog, distance fog, SSAO, SSIL, bloom, sun (relative to what the map sets; they survive reloads). The HUD's top line shows each one green when on and dimmed when off |
| F1 | hide / show the HUD |
| F2 | graphics settings panel |
| F3 / Shift+F3 / F4 | render inspector (spectator mode): outlines, its panel, buffer views (*Render inspector* below) |
| F5 | id panel (spectator mode): every line with an `id` (*Id panel* below) |
| F9 | export the open map to `export/<map>-<stamp>/` ([export.md](export.md)) |
| F12 | save `screenshot.png` |
| Esc | close the prompt box, leave snip or volume mode or close the panel (it does not quit: close the window). With nothing to close it shows a **See keybindings** button: Enter or a click opens the keybindings panel, every key of the game in one section per mode (←/→ or a click on a tab changes the section, typing searches all of them, Esc closes) |

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
| Ctrl+S | save the manual edits to the map file (the HUD shows "N unsaved edits" until then) |
| Ctrl+Shift+Delete | clear the red list of edits that could not be applied again after the map changed on disk |
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

Small changes don't need Claude: select objects as usual (click, Shift+click, Ctrl+click for a group), press **M**, and use the keys below. In edit mode the mouse cursor is free: what is under it is outlined, and a click selects it (Shift+click adds or removes, Ctrl+click takes its group). **Hold the right mouse button to fly** (mouse look, WASD / E / Q / Space, the wheel sets the speed, Shift faster, Alt slower; the cursor hides meanwhile); a short right click (let go within a quarter of a second without moving) clears the selection. Each edit is the smallest possible text change to the map (the numbers on one line, a deleted line, a line split in two, a new line); comments, groups and every other line stay as they were. Edits are **live**: the scene changes at once without reloading the map (only the part that changed is rebuilt: a few milliseconds on small maps, about 50 ms on the largest one in `maps/`, against about half a second for a reload), and the map file is not written until you save. The selection follows the edited objects. The HUD shows the snap step, the first selected object's map line and entity id, the undo / redo counts and what the last edit did.

**Saving.** **Ctrl+S** writes your edits to the map file (in spectator mode, in edit mode or not). While there are unsaved edits the HUD shows **N unsaved edits** in every spectator mode; leaving edit mode with M or Esc first only reminds you (press again within 3 seconds to leave; the edits stay live and unsaved). The **Save edits** setting (F2, group *Editor*; `--edit-save` for one run) switches to **auto**: every edit, undo and redo is written to the file at once, so there is nothing to save. A request needs the file to show what you see: with unsaved edits, Enter in the prompt box sends nothing and says so; **Ctrl+S** in the prompt box saves and sends. If you close the game with unsaved edits, they are written to `unsaved/<map>-<date>-<time>.map` next to the settings file (`~/.config/monolith/` on Linux); the map file is not touched.

| In edit mode | Action |
|---|---|
| right mouse button held | fly the camera (mouse look, WASD / E / Q / Space, wheel speed, Shift ×4, Alt ×0.25) |
| click, Shift+click, Ctrl+click | select what is under the cursor (add / remove, its group); a short right click clears the selection |
| G / R / S | grab, rotate or scale the selection with the mouse (see *Grab, rotate, scale* below) |
| arrows, PgUp / PgDn | move the selection one step along the world axis nearest to where you look (up / down with PgUp / PgDn). The first selected object lands on the snap grid; the others keep their distance to it. With Ctrl: exactly one step, no grid |
| N | snap step 0.1, 0.25, 0.5, 1 or 2 m (shared with volume mode) |
| Alt+R / Alt+Shift+R | turn one step: models 15° of yaw (Ctrl: 1°), boxes 90° about their centre (from `box` and `block` lines; stairs and waffles can't turn, and boxes and models are turned separately) |
| Delete | remove |
| Ctrl+D | duplicate: the copies go one footprint to the right and become the selection |
| I | the **model picker**: add one of the package's models at the point under the cursor (see *Adding a model* below) |
| Ctrl+I | give the selection's line an id. With objects of one line selected, a small panel opens: type the number (it starts with the line's id, or the next free one), Enter sets it, Esc cancels, an empty field takes the next free id; the number must be unused in the map. With objects of several lines selected, each line without an id gets the next free one at once. Only the `id n` in front of the line changes (a `repeat` line keeps all its copies: the id names the line, its objects are `n:0`, `n:1`, …) |
| M or Esc | leave edit mode (V and C leave it too); the arrows fly to viewpoints again. With unsaved edits the first press only reminds you |
| Ctrl+S | save the edits to the map file (also outside edit mode, in spectator mode) |

**Grab, rotate, scale (G / R / S).** As in Blender: press **G** (grab), **R** (rotate) or **S** (scale) and move the mouse; the selection follows live and the camera stands still. **X**, **Y** or **Z** limits it to that axis (a coloured line through the pivot shows it: X red, Y green, Z blue), **Shift+X / Y / Z** to the plane of the other two axes; the same key again uses the first selected model's own axes, a third time removes the limit. Type a number instead of moving the mouse (metres along the axis, or along the view's right without one; degrees; the scale factor), with **.**, **-** (changes the sign) and Backspace. Hold **Ctrl** to snap (the pivot onto the N grid, 15°, steps of 0.1) and **Shift** to move ten times more finely. Pressing G, R or S again switches to that kind from where the objects started. **Click or Enter** applies it as one edit (one undo step); **right click or Esc** cancels and everything is exactly as before. The pivot is the centre of the selection (one model alone: its origin). The line at the top says what is happening: `GRAB  X 1.250 m  global | typed: 1.25_ | snap off …`. Rotate turns about the view axis, or about the axis you lock; boxes only turn about the vertical, in 90° steps (with boxes selected and no axis locked, R turns about the vertical). How the mouse turns it: about an axis you see from the side (the vertical from eye height, the usual case) it works like a turntable: drag right and the side facing you goes right, half a degree per pixel, wherever the cursor is; about an axis that points at you (the view axis, or looking down a locked axis) moving the mouse right turns it clockwise on screen when the cursor starts on the object, and started more than 80 px from the pivot it follows the angle the cursor sweeps around it, as in Blender. Boxes jump to the next 90° once you have dragged 30° past the last step (60 pixels). A box with a square footprint (a 2.2 × 2.2 m pillar) looks exactly the same turned by 90°, and any box turned by 180° about its own centre: the HUD says so in red and a click writes nothing (select it with its neighbours, Shift+click, to turn them around their common centre). Stairs and waffles can't turn or scale: they stay where they are and the HUD names them in red, while the rest of the selection follows. Lights move with G and ignore R and S. Numbers you type are written as you typed them (1.25 stays 1.25); a turn of a model about the vertical adds to its yaw exactly, other turns are written with 6 decimals; mouse values are rounded to what the HUD shows.

**Adding a model (I).** **I** opens the model picker on the right: every file in the package's `models/` folder that has a row in `ASSETS.md` (a model from elsewhere needs a request, since it needs a licence row first). Each row shows a picture of the model, its file, its triangles, its size in metres and the licence from its `ASSETS.md` row. Type to filter (by file or licence), ↑ / ↓ / PgUp / PgDn / Home / End select, **Enter** places the selected model at the point under the cursor, standing on the surface (it snaps on the surface's axes like a volume corner; hold Ctrl for the exact point) and joins the group of the object it stands on. It is one edit like the others (Ctrl+Z takes it back, Ctrl+S saves it), the picker closes and the new model is selected, so **G** moves it straight away. **Esc** closes the picker, and so does **I** while nothing is typed. While it is open it takes every key. The pictures are made the first time the picker opens after the map is loaded, two per frame (rows without one yet show "..."); the model is shown with its own materials in a neutral grey studio light, so it looks as it will when placed until the map gives it a material.

Editing one copy of a `repeat` row (a single pillar of a row of eight) splits that line: the moved copy gets its own line and the others stay exactly where they were. Lines you edit get an `id n` in front, so later edits and the selection keep track of them ([map format](map-format.md) §2.9); Ctrl+I gives a line one without changing anything else, for objects your own code looks up by id.

**Undo** (Ctrl+Z) puts the old text back, byte for byte; redo (Ctrl+Shift+Z or Ctrl+Y) applies the edit again; both are live like an edit, and saving doesn't end the history (undo past the save point and the count of unsaved edits goes up again). **When the map changes on disk** while you have unsaved edits (a request finishes, you save it in a text editor), the game loads the new file and applies your unsaved edits again on top of it, each to the same objects (found by their entity ids, so lines that moved don't matter). An edit whose objects are gone, or that no longer fits the line, is listed in red in the HUD ("could not re-apply: …") until you clear the list with **Ctrl+Shift+Delete**; the history before your last save is dropped. Without unsaved edits such a change just clears the history, so an undo never takes back a change you didn't make here. If a request of yours is queued or running on the same map, the first edit after your last save (and a save) only warns you (the request's session may be changing the same lines); press the key again within 3 seconds to go ahead.

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

## Render inspector (F3, Shift+F3, F4)

In spectator mode the inspector shows what the renderer actually draws. It is off by default and changes nothing while off.

| Key | Inspector |
|---|---|
| F3 | outlines: off → by material → by shader → by draw kind. Every object within 80 m in view is outlined (boxes their edges, models their rotated bounds; at most 20 000, nearest first), draws farther away their bounds. Labels name the draw under the crosshair and the 24 nearest ones: material, shader, model file, the kind of draw (`merged cell mesh`: one material's visible box faces in a 32 m cell, `instanced`: cubes of an `instanced on` material, `model`: instances of one model file), triangles, whether it casts a shadow |
| Shift+F3 | the inspector panel: every material, then every shader, with its draws and triangles. Type to filter, ↑ / ↓ select, **Space** hides / shows the selected one (hidden things also drop their shadows), **I** shows it alone (again: everything back; Ctrl+I while a filter is typed), Esc closes |
| F4 | buffer views (Shift+F4 back): scene, albedo, normal, ORM, radiance, specular, SSAO, SSIL, SSGI, SSR, bloom, DOF; the HUD names the view |

What is hidden is kept by name, so it stays hidden across reloads and edits. A model is hidden when one of the materials its `material` / `material.N` lines give it is; models drawn with their own materials can only be shown alone or hidden with the others by **I**.

## Id panel (F5)

In spectator mode (in the edit mode or not) **F5** lists every line of the map that has an `id n` prefix ([map-format.md](map-format.md) §2.9): the id, the map line, the kind (the box primitive, `model`, `light`), how many objects the line makes, its model file, material or light type, and its group. While it is open it takes every key and the cursor is free.

| Key | Id panel |
|---|---|
| typing, Backspace | filter by the id, the line, the kind, the file or material and the group (case-insensitive; Ctrl+Backspace clears it) |
| ↑ / ↓, Page Up / Page Down, Home / End | select a row |
| Enter | fly in front of the line's objects (far enough to see them all) and select them; the panel closes |
| Delete | remove the line's `id` prefix only: the objects stay, their entity ids become implicit (they then change when the line changes) |
| Shift+Delete | remove the line's objects (as Delete in the edit mode) |
| Ctrl+I | give the selection's line an id: the edit mode's number field (several lines: the next free ids at once) |
| Esc / F5 | close |

Removing and giving ids are manual edits like any other: live, undoable with Ctrl+Z, counted as unsaved until Ctrl+S, and the first one warns when a request is open on the map.

## Graphics settings (F2)

The panel has a preset selector, rows for display, shadows, ambient occlusion, fog, post-processing and performance, the editor's **Save edits** row (manual / auto, see *Editing by hand*; not a graphics setting and not part of a preset), and a live frame time readout. Use the arrow keys or the mouse; values that differ from the preset are blue. Heavy changes (shadow map size, AO mode, fog cache, preset) take a frame to apply; a new shadow map size reloads the map (about a second, your selection is kept). Esc or F2 closes and saves.

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
