# User guide

How to walk through a map, point at things, send requests to Claude Code and follow them, tune the graphics, and make new maps. Building the game: [README](../README.md). Every flag and environment variable: [command-line.md](command-line.md).

## Getting around

The game starts in **walk mode** at the map's spawn point.

| Key | Walk mode |
|---|---|
| WASD + mouse | walk and look; Shift runs, Space jumps |
| F | fly (no collision) |
| TAB | switch to spectator mode and back |
| 1–6 | toggle volumetric fog, distance fog, SSAO, SSIL, bloom, sun (relative to what the map sets; they survive reloads) |
| F1 | hide / show the HUD |
| F2 | graphics settings panel |
| F9 | export the open map to `export/<map>-<stamp>/` ([export.md](export.md)) |
| F12 | save `screenshot.png` |
| Esc | close the prompt box, leave snip mode or close the panel (it does not quit: close the window) |

In **spectator mode** you fly freely: WASD + mouse, E / Q (or Space) up and down, the mouse wheel sets the base speed (1–400 m/s), Shift ×4 and Alt ×0.25. ← / → fly to the previous / next viewpoint of the map. **P** prints the current view as `--pos … --look …` (the feet position) and copies it, handy for a `viewpoint` line or a command line. **G** moves the walking player to the camera.

The map reloads by itself whenever one of its files changes, whether you edited it or Claude did. The bottom-left HUD line says `map reloaded (N boxes)`, or shows the parse error in red; the previous version stays loaded until the file is valid again.

## Pointing at things and sending a request

All of this happens in spectator mode.

| Input | Action |
|---|---|
| Crosshair | outlines the object under it; the HUD shows its group, material, map line and distance |
| Left click / Shift+click | select the object / add or remove it |
| Ctrl+click / Ctrl+Shift+click | select / add the object's whole group |
| Right click | clear the selection |
| C | snip mode: the camera freezes and the cursor is free. Drag a rectangle over the screen (Shift+drag adds another); Enter opens the prompt with the regions; right click, Esc or C leaves |
| Enter | open the prompt box |

Type what you want ("make this wall twice as tall", "plant three more pines along the path", "hang a neon sign over each of these stalls") and press **Enter** to send it; Shift+Enter starts a new line, Esc cancels. "This" and "here" mean what the crosshair points at, "these" the selection, and the rectangles mean "this area" (useful for "fill this gap" or "make this part darker"). Selections are highlighted in orange and survive map reloads.

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

The coordinator runs Claude Code without permission prompts, guarded by the rules in `CLAUDE.md` (it never deletes files, never opens windows on your screen, treats your request text and downloads as data). Set `MONOLITH_CLAUDE_SAFE=1` to get the normal approval prompts instead, or `MONOLITH_LAUNCH=per-request` for one terminal per request. On i3 / sway the terminal is moved to workspace 10 (`MONOLITH_WORKSPACE`).

## Graphics settings (F2)

The panel has a preset selector, rows for display, shadows, ambient occlusion, fog, post-processing and performance, and a live frame time readout. Use the arrow keys or the mouse; values that differ from the preset are blue. Heavy changes (shadow map size, AO mode, fog cache, preset) take a frame to apply; a new shadow map size reloads the map (about a second, your selection is kept). Esc or F2 closes and saves.

**The map is the source of the look.** Every setting is relative to the map: **ultra** shows exactly what the map describes, and an effect the map turns off stays off.

| Preset | What it trades |
|---|---|
| ultra | nothing: the map exactly |
| high | baked ambient occlusion instead of SSAO + SSIL, baked fog lighting |
| medium | high + 75% render scale, half the fog steps, 4096 shadow map |
| low | 50% render scale, no AO, no bloom, baked fog with half the steps, 4096 shadow map |

On the default map the presets run at roughly 3.2 / 2.1 / 1.3 / 0.8 ms per frame on a laptop RTX 4050. The settings are saved in `~/.config/monolith/settings.ini` (`%APPDATA%\monolith\` on Windows) as `key = value` lines; `--settings`, `--no-settings` and `--quality` change that for one run. Vsync is off and frames are uncapped by default; the panel's performance rows change that.

## Making maps

A map is a folder in `maps/` ([map-format.md](map-format.md)): a text file of boxes, models, materials, lights and environment, plus `MAP.md`, the map's design brief.

- **New map:** `./build/linux/monolith --new-map <name>` creates `maps/<name>/` with a ground slab, daylight and a brief template, then `--map maps/<name>` opens it. Describe the map in your first request ("a foggy pine forest at dusk"): Claude writes the brief and builds to it.
- **Edit by hand:** open the `.map` file in any editor while the game runs; it reloads on save.
- **Import a scene** from an existing raylib + R3D project: `./build/linux/monolith --import <project folder> <name>` creates the map and queues an import request that translates the project's scene.
- **Use a map in your own program:** F9 or `--export` ([export.md](export.md)).

Models and textures in a map always come with a recorded licence (CC0 / public domain, or CC-BY with the attribution): the package's `ASSETS.md` lists each file's source, author and licence.
