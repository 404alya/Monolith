# User guide

How to walk through a map, point at things, send requests to Claude Code and follow them, tune the graphics, and make new maps. Building monolith: [README](../README.md). Every flag and environment variable: [command-line.md](command-line.md). Unfamiliar terms: [glossary.md](glossary.md); problems: [troubleshooting.md](troubleshooting.md).

## Getting around

The editor starts in **walk mode** at the map's spawn point.

The HUD at the top shows what is going on (the mode, the effects, the selection, edit mode and its unsaved edits), not the keys: press **Esc** for the keybindings (the tables below have them too).

| Key | Walk mode |
|---|---|
| WASD + mouse | walk and look; Shift runs, Space jumps |
| F | fly (no collision): Space up, Ctrl down, Shift ×4 |
| TAB | switch to spectator mode and back |
| 1–6 | toggle volumetric fog, distance fog, SSAO, SSIL, bloom, sun (relative to what the map sets; they survive reloads). The HUD's top line shows each one white when on and faint when off |
| F1 | hide / show the HUD |
| F2 | graphics settings panel |
| F3 / Shift+F3 / F4 | render inspector (spectator mode): outlines, its panel, buffer views (*Render inspector* below) |
| F5 | id panel (spectator mode): every line with an `id` (*Id panel* below) |
| F6 | groups panel (spectator mode): every group, by area (*Groups panel* below) |
| F9 | export the open map to `export/<map>-<stamp>/` ([export.md](export.md)) |
| F12 | save a screenshot in `screenshots/` (named by date and time) |
| Esc | close the prompt box, leave snip or volume mode or close the panel (it does not quit: close the window). With nothing to close it shows a **See keybindings** button: Enter or a click opens the keybindings panel, every key of the editor in one section per mode (←/→ or a click on a tab changes the section, typing searches all of them, Esc closes) |

In **spectator mode** you fly freely: WASD + mouse, E / Q (or Space) up and down, the mouse wheel sets the base speed (1–400 m/s), Shift ×4 and Alt ×0.25. ← / → fly to the previous / next viewpoint of the map. **P** prints the current view as `--pos … --look …` (the feet position) and copies it, handy for a `viewpoint` line or a command line. **G** moves the walking player to the camera. **O** shows or hides the map's barriers (invisible walls; *Barriers* below).

The map reloads by itself whenever one of its files changes, whether you edited it or Claude did. The bottom-left HUD line says `map reloaded (N boxes)`, or shows the parse error in red; the previous version stays loaded until the file is valid again.

## Pointing at things and sending a request

All of this happens in spectator mode.

| Input | Action |
|---|---|
| Crosshair | outlines the object under it; the HUD shows its group, material, map line, entity id and distance. The id is `id 12:0` when the line has `id 12` (stable whatever happens to the line), else `id a7d95ba7:0 (implicit)` (from the line's text: changes when that line changes); the number after the colon is the object's place among the objects of its line ([map-format.md](map-format.md) §2.9) |
| Left click / Shift+click | select the object / add or remove it |
| Ctrl+click / Ctrl+Shift+click | select / add the object's whole group |
| Right click | clear the selection |
| C | snip mode: the camera freezes and the cursor is free. Drag a rectangle over the screen (Shift+drag adds another), or press **D** for the pen and draw over the frame (*Sketches* below); Enter opens the prompt with the regions and strokes; right click, Esc or C leaves |
| V | volume mode: mark boxes in world space (below) |
| Shift+V | a volume from the selection's bounds (or the bounds of the object under the crosshair) |
| M | edit mode: move, turn, remove, duplicate or add objects yourself, without a request (below) |
| Ctrl+Z / Ctrl+Shift+Z | undo / redo the last manual edit (also Ctrl+Y) |
| Ctrl+S | save the manual edits to the map file (the HUD shows "N unsaved edits" until then) |
| Ctrl+Shift+Delete | clear the red list of edits that could not be applied again after the map changed on disk |
| Enter | open the prompt box |

Type what you want ("make this wall twice as tall", "plant three more pines along the path", "hang a neon sign over each of these stalls") and press **Enter** to send it; Shift+Enter starts a new line, Esc cancels. "This" and "here" mean what the crosshair points at, "these" the selection, the rectangles mean "this area" (useful for "fill this gap" or "make this part darker"), and the volumes mean "this space" (V1, V2, ...). Selections are highlighted in orange and survive map reloads.

### Sketches (the pen in snip mode)

Some requests are easier drawn than said: "a fence along here", "a path from here to there", "fill this area". In snip mode (**C**) press **D** to switch from rectangles to the pen (the help bar at the bottom says which is on; **D** again goes back). Then drag with the left button to draw a stroke over the frozen frame.

| With the pen | Action |
|---|---|
| drag (left button) | draw a stroke |
| 1 / 2 / 3 / 4 | colour: red, yellow, blue, white |
| F / L / A / O | kind: free hand (to start), straight line, arrow (its head where you let go), circle (from its centre out to the rim) |
| Backspace / Shift+Backspace | remove the last stroke / all strokes |
| D | back to rectangles (the strokes stay) |
| Enter | open the prompt box: strokes and rectangles go into the same request |

Strokes are numbered S1, S2, ... in their colour, so the prompt can name them ("S1 is where the fence goes, S2 the gate"). A free-hand stroke that ends near where it started is a closed outline: Claude reads it as an area, like a rectangle. Leaving snip mode drops the strokes (they belong to that frozen view), as it drops the rectangles. The request carries `sketch.png` (only the strokes), the strokes drawn on `view-marked.png`, and for each stroke what it passes over in the world.

### Volumes

A volume is a box in the world rather than on the screen: "this room", the space behind a wall, the gap between two buildings at a certain height, or an empty spot to fill. Press **V**; the camera keeps flying, so you can move between the clicks.

1. Click a surface for the first corner, or a point in the air (the *work plane*, below).
2. Click a second point for the opposite corner of the footprint.
3. Move the mouse up or down (or turn the wheel; Shift ×4) to set the height. The camera holds still meanwhile. Click or press Enter to add it.

Points snap to a grid (**N** cycles 0.1, 0.25, 0.5, 1 and 2 m; 0.5 m to start) along the face you clicked; hold **Ctrl** while clicking to place them exactly. Right click or Esc cancels the volume you are making.

**The work plane: corners in the air.** Where the crosshair points at the sky (or at nothing within reach), a click lands on the *work plane*, a level plane drawn as a small green grid around the point, with a thin line straight down to the ground under it. Hold **Alt** while clicking to use the plane even over a surface: a corner in the middle of a room or above water. Its height: for the first corner, the base of the last volume (or, with none yet, your feet: the camera height minus 1.62 m, on the snap grid); after the first corner, that corner's height, so the footprint stays level. Before the first click, while the next click would use the plane, **PgUp / PgDn** raise or lower it by one snap step (it keeps that height until you leave volume mode); the HUD's volume line says `work plane y 3.50 m`. Points on the plane snap to the grid in x and z (Ctrl: exact). The plane is only drawn on screen, never in the request images.

| In volume mode | Action |
|---|---|
| [ / ] | the previous / next volume becomes the active one |
| arrows, PgUp / PgDn | move the active volume one step (the arrows follow the world axis nearest to where you look; they don't fly to viewpoints in this mode). While the next click would land on the work plane, PgUp / PgDn move the plane instead |
| Alt + those keys | grow the volume on that side; Alt+Shift shrinks it (Alt+PgUp / PgDn too, even while Alt puts clicks on the plane) |
| Alt + click | the corner on the work plane even over a surface |
| Delete / Shift+Delete | remove the active volume / all volumes |
| H | write the active volume into the map as a barrier (an invisible wall, *Barriers* below); the volume stays |
| X | mark the active volume as a **3D snip**, or unmark it (below) |
| Enter | open the prompt box |
| V, Esc or right click | leave volume mode (the volumes stay) |

**3D snips.** A volume marked with **X** is drawn with dashed edges and labelled `V2 snip`. With the request go four pictures of only what is inside it: from the direction you are looking, from the front (+z), from the side (+x) and from the top, each framed to the box. Everything outside the box is cut away at its faces (what sticks out of the box is cut off, what lies outside still casts its shadows in), the sky is a flat grey and the distance and volumetric fog are off, so the contents read clearly. Use it for "this thing here", when it is hidden behind other things or hard to see from where you stand. The first snip after the map loads takes a moment longer (the cut versions of the map's shaders are compiled).

Volumes are drawn as green boxes, fainter where they are hidden, numbered V1, V2, ... They are sent with every request until you remove them, survive map reloads, and are never saved into the map.

### Editing by hand (M)

Small changes don't need Claude: select objects as usual (click, Shift+click, Ctrl+click for a group), press **M**, and use the keys below. In edit mode the mouse cursor is free: what is under it is outlined, and a click selects it (Shift+click adds or removes, Ctrl+click takes its group). **Hold the right mouse button to fly** (mouse look, WASD / E / Q / Space, the wheel sets the speed, Shift faster, Alt slower; the cursor hides meanwhile); a short right click (let go within a quarter of a second without moving) clears the selection. Each edit is the smallest possible text change to the map (the numbers on one line, a deleted line, a line split in two, a new line); comments, groups and every other line stay as they were. Edits are **live**: the scene changes at once without reloading the map (only the part that changed is rebuilt: a few milliseconds on small maps, about 50 ms on the largest one in `maps/`, against about half a second for a reload), and the map file is not written until you save. The selection follows the edited objects. The HUD shows the snap step, the first selected object's map line and entity id, the undo / redo counts and what the last edit did.

**Saving.** **Ctrl+S** writes your edits to the map file (in spectator mode, in edit mode or not). While there are unsaved edits the HUD shows **N unsaved edits** in every spectator mode; leaving edit mode with M or Esc first only reminds you (press again within 3 seconds to leave; the edits stay live and unsaved). The **Save edits** setting (F2, group *Editor*; `--edit-save` for one run) switches to **auto**: every edit, undo and redo is written to the file at once, so there is nothing to save. A request needs the file to show what you see: with unsaved edits, Enter in the prompt box sends nothing and says so; **Ctrl+S** in the prompt box saves and sends. If you close the editor with unsaved edits, they are written to `unsaved/<map>-<date>-<time>.map` next to the settings file (`~/.config/monolith/` on Linux); the map file is not touched.

| In edit mode | Action |
|---|---|
| right mouse button held | fly the camera (mouse look, WASD / E / Q / Space, wheel speed, Shift ×4, Alt ×0.25) |
| click, Shift+click, Ctrl+click | select what is under the cursor (add / remove, its group); a short right click clears the selection |
| G / S | grab or scale the selection with the mouse (see *Grab, rotate, scale* below) |
| R / Shift+R | turn the selection 90° / −90° about the vertical at once, each object about its own centre (a box stays a box with its corners rewritten). On **whole groups** (see *Groups as units* below) the groups turn as one about their centre, their lights with them |
| Alt+R | rotate with the mouse or a typed angle, the keyboard way (axis keys X / Y / Z; see *Grab, rotate, scale* below) |
| Alt+Shift+R | reset the rotation of the selection (boxes back to axis-aligned, models to no `rot`) |
| drag a ring of the rotate gizmo | turn the selection about that axis (see *The rotate gizmo* below) |
| L | the gizmo's axes: the world's or the first selected object's own |
| Shift+N | the angle snap step for Ctrl: 1, 5, 15 (default), 45 or 90° |
| arrows, PgUp / PgDn | move the selection one step along the world axis nearest to where you look (up / down with PgUp / PgDn). The first selected object lands on the snap grid; the others keep their distance to it. With Ctrl: exactly one step, no grid |
| N | snap step 0.1, 0.25, 0.5, 1 or 2 m (shared with volume mode) |
| Delete | remove |
| Ctrl+D | duplicate: the copies go one footprint to the right and become the selection. On **whole groups** each group is copied as a new group `"<name> 2"` (the next free number) |
| I | the **model picker**: add a shape (cube, sphere, cylinder, cone, capsule, plane) or one of the package's models at the point under the cursor (see *Adding a model* and *Adding a shape* below) |
| H | place a **barrier** (an invisible wall) with three clicks, as a volume (see *Barriers* below) |
| Ctrl+I | give the selection's line an id. With objects of one line selected, a small panel opens: type the number (it starts with the line's id, or the next free one), Enter sets it, Esc cancels, an empty field takes the next free id; the number must be unused in the map. With objects of several lines selected, each line without an id gets the next free one at once. Only the `id n` in front of the line changes (a `repeat` line keeps all its copies: the id names the line, its objects are `n:0`, `n:1`, …) |
| M or Esc | leave edit mode (V and C leave it too); the arrows fly to viewpoints again. With unsaved edits the first press only reminds you |
| Ctrl+S | save the edits to the map file (also outside edit mode, in spectator mode) |

**Grab, rotate, scale (G / Alt+R / S).** As in Blender: press **G** (grab), **Alt+R** (rotate) or **S** (scale) and move the mouse; the selection follows live and the camera stands still. **X**, **Y** or **Z** limits it to that axis (a coloured line through the pivot shows it: X red, Y green, Z blue), **Shift+X / Y / Z** to the plane of the other two axes; the same key again uses the first selected box's or model's own axes, a third time removes the limit. Type a number instead of moving the mouse (metres along the axis, or along the view's right without one; degrees; the scale factor), with **.**, **-** (changes the sign) and Backspace. Hold **Ctrl** to snap (the pivot onto the N grid, the angle step of Shift+N, 15° unless changed, steps of 0.1) and **Shift** to move ten times more finely. Pressing G, R or S again switches to that kind from where the objects started (inside it, plain R is rotate). **Click or Enter** applies it as one edit (one undo step); **right click or Esc** cancels and everything is exactly as before. The pivot is the centre of the selection (one model alone: its origin). The line at the top says what is happening: `GRAB  X 1.250 m  global | typed: 1.25_ | snap off …`. Rotate turns about the view axis, or about the axis you lock; boxes and blocks turn freely like models (a turned box gets `rot yaw pitch roll` on its line, and collides and is picked as its turned shape). How the mouse turns it: about an axis you see from the side (the vertical from eye height, the usual case) it works like a turntable: drag right and the side facing you goes right, half a degree per pixel, wherever the cursor is; about an axis that points at you (the view axis, or looking down a locked axis) moving the mouse right turns it clockwise on screen when the cursor starts on the object, and started more than 80 px from the pivot it follows the angle the cursor sweeps around it, as in Blender. A turn that leaves everything where it was (a square pillar turned 90° about its own centre) is named in red in the HUD and a click writes nothing. A box whose turn ends axis-aligned (any multiple of 90° about the world axes) is written back as plain corners without `rot`, and a turn back to 0 removes the `rot`. Stairs and waffles can't turn or scale: they stay where they are and the HUD names them in red, while the rest of the selection follows. Lights move with G and ignore R and S. Numbers you type are written as you typed them (1.25 stays 1.25); a turn of a model or turned box about the vertical adds to its yaw exactly (one about its own z to its roll), other turns are written with 6 decimals; mouse values are rounded to what the HUD shows.

**The rotate gizmo.** In edit mode, whenever the selection has a box, block or model, three coloured rings sit at the pivot (X red, Y green, Z blue; only the half toward you), a larger white ring around them and a faint grey disc inside: the same look as Blender's rotate gizmo, always 120 pixels across whatever the distance. Hover a ring and it thickens. **Drag a coloured ring** with the left button: the selection turns about that axis by the angle you sweep around the pivot, with an arc and the angle shown next to the cursor; release to apply. **The white ring** turns about the line of sight, **the disc** tumbles it like a trackball (half a degree per pixel). Hold **Ctrl** to snap to the angle step (**Shift+N** changes it), **Shift** for ten times finer control. Typing a number while dragging sets the angle exactly; then **Enter** or a click applies it and **Esc** or a right click cancels. A ring **clicked** without moving stays open the same way: move the mouse to turn, type, Enter. **L** switches the rings between the world's axes and the first selected object's own axes. Clicking the gizmo never changes the selection.

**Adding a model (I).** **I** opens the model picker on the right: every file in the package's `models/` folder that has a row in `ASSETS.md` (a model from elsewhere needs a request, since it needs a licence row first). Each row shows a picture of the model, its file, its triangles, its size in metres and the licence from its `ASSETS.md` row. Type to filter (by file or licence), ↑ / ↓ / PgUp / PgDn / Home / End select, **Enter** places the selected model at the point under the cursor, standing on the surface (it snaps on the surface's axes like a volume corner; hold Ctrl for the exact point) and joins the group of the object it stands on. It is one edit like the others (Ctrl+Z takes it back, Ctrl+S saves it), the picker closes and the new model is selected, so **G** moves it straight away. **Esc** closes the picker, and so does **I** while nothing is typed. While it is open it takes every key. The pictures are made the first time the picker opens after the map is loaded, two per frame (rows without one yet show "..."); the model is shown with its own materials in a neutral grey studio light, so it looks as it will when placed until the map gives it a material.

**Adding a shape (I).** The picker's first six rows are the **shapes**: cube, sphere, cylinder, cone, capsule and plane, built into monolith (no file, no licence row). Typing filters them too ("cyl", or "shape" for all six). **Enter** places the selected shape at the point under the cursor: 1 × 1 × 1 m (a plane 2 × 2 m, flat), plain grey (the `"default"` material), standing on the surface you point at (hanging under a ceiling, against a wall beside it), snapped like a model (Ctrl: exact). Hold **Alt** while pressing Enter, or point at the sky, and it stands on the **work plane** instead (the horizontal plane of volume mode, *Volumes* above: by default at your feet; `PgUp` / `PgDn` in volume mode raise and lower it), so a shape can float in the air. It is one `shape` line (`id N shape cylinder cx y0 cz 1 1 1`) after the line of the object it stands on (in its group; inside `use "default"` when that group uses another material), or at the end of the map on the work plane; one edit like the others (Ctrl+Z takes it back byte for byte, Ctrl+S saves it), and it is selected, so **G**, **R**, **S** and the gizmo move, turn and size it at once (a shape takes every edit a box does). Give it a material by editing its line or with a request.

Editing one copy of a `repeat` row (a single pillar of a row of eight) splits that line: the moved copy gets its own line and the others stay exactly where they were. Lines you edit get an `id n` in front, so later edits and the selection keep track of them ([map format](map-format.md) §2.9); Ctrl+I gives a line one without changing anything else, for objects your own code looks up by id.

**Undo** (Ctrl+Z) puts the old text back, byte for byte; redo (Ctrl+Shift+Z or Ctrl+Y) applies the edit again; both are live like an edit, and saving doesn't end the history (undo past the save point and the count of unsaved edits goes up again). **When the map changes on disk** while you have unsaved edits (a request finishes, you save it in a text editor), the editor loads the new file and applies your unsaved edits again on top of it, each to the same objects (found by their entity ids, so lines that moved don't matter). An edit whose objects are gone, or that no longer fits the line, is listed in red in the HUD ("could not re-apply: …") until you clear the list with **Ctrl+Shift+Delete**; the history before your last save is dropped. Without unsaved edits such a change just clears the history, so an undo never takes back a change you didn't make here. If a request of yours is queued or running on the same map, the first edit after your last save (and a save) only warns you (the request's session may be changing the same lines); press the key again within 3 seconds to go ahead.

**Picking the model.** Each request runs on a fast, default or heavy Claude model. A small separate Claude run (the AI classifier, on Fable; Opus when Fable is busy) reads the request and its context and picks the tier before the request is queued; that takes a few seconds and never holds up the editor. Start the prompt with `@fast`, `@default` or `@heavy` to choose yourself (no classifier then). If the classifier fails or takes longer than 30 seconds, the request runs on the default tier. `MONOLITH_CLASSIFIER=off` turns it off ([command-line.md](command-line.md)).

### Barriers (H, O)

A barrier is an invisible wall: it stops the player like a box but is never drawn, casts no shadow and changes nothing in the look of the map, in the editor, in exports and in games ([map-format.md](map-format.md) §2.1 *Barriers*). Use one where the player must not go but nothing should be seen: a map edge, a ledge nobody should walk off, a gap between two buildings. In spectator mode the barriers show as **red translucent boxes with red edges**, fainter where something hides them; **O** hides or shows them (the **Barrier overlay** setting, F2 group *Editor*, remembers it; on to start). Walk mode never shows them, so walking shows the map as a player sees it. While they are shown, a barrier in front of what you point at is pointed at instead: click it to select it, and in edit mode G / S / Alt+R / R, the arrows, Delete and Ctrl+D work on it as on a box (a barrier line is edited like a `box` line); when they are hidden clicks go through them.

To **place one by hand**, press **H** in edit mode and click as for a volume: a first corner on a surface (or on the work plane: in the air, or anywhere with Alt; *Volumes* above), the opposite corner of the footprint, then the height with the mouse or the wheel (the **N** snap grid applies, Ctrl places points exactly, a right click or Esc cancels). The third click or Enter writes one line `id N barrier x0 y0 z0 x1 y1 z1` into the map as one edit (Ctrl+Z takes it back, Ctrl+S or auto save writes it) and selects the new barrier, so G / S / Alt+R / the arrows / Delete / Ctrl+D work on it at once. With something selected the line goes right after the first selected object's line (in its file and group); with nothing selected it goes to the end of the main map file, under a `# barriers` comment in a `group "barriers"` (made the first time). In volume mode **H** turns the active volume into a barrier the same way (the volume stays). To ask Claude instead, say so in a request ("stop the player walking off this ledge"): it writes the same one line.

### Painting (B)

Marks, signs, decals and pixel art don't need a request either: press **B** in spectator mode and paint straight onto any box or model. The tools, their keys and the colour picker are Aseprite's, and the cursor stays free: the **mouse wheel** moves toward the point under the cursor, **middle-drag** orbits around it, **Space + drag** (or Shift + middle-drag) pans; the arrow keys still fly to the viewpoints. The **draw panel** on the left shows what the cursor last rested on (the object, the face or model part, where its paint goes, or why it can't be painted), the tools and their options, the colour picker and the density; a ring on the surface shows exactly which texels the brush will cover (at size 1, the one texel). Your tool, brush, colours, palette and the panel's switches are kept per map, in `draw.ini` in the map's folder (not part of the map: it isn't exported or reloaded, commit it only if you want to share your palette).

**Where the paint goes.** Paint is a layer over the material: the object keeps its look (its shader, textures, glow) everywhere you haven't painted. Each material that has paint gets an atlas, `textures/paint/<material>.png`, and the first stroke on a box face, a shape's part or a model part gives it a rectangle there: a `paint` line at the end of the map (under `# paint (draw mode)`), the material's `paint` key and an `id` on the object's line, one undoable edit. A stroke that runs over onto another face of the same box gives that face its rectangle too (still one undo). A model part drawn with its own material paints into `textures/paint/<file>_s<N>.png`. A model whose own texture coordinates repeat or overlap (most low-poly kits, scanned rocks) is **unwrapped** for paint on its first stroke (a `models/<file>.paintuv` file beside it; a large scan takes a few seconds, the HUD says so). Each face of a box has its own rectangle; the panel's **Faces** buttons group faces that should share paint (e.g. the four sides), then **Apply**.

**Painting a shape.** A shape is painted like a box: each of its **parts** has a rectangle of its own (a cube's six faces; a sphere's `surface`; a cylinder's and a capsule's `side`, `top` and `bottom`; a cone's `side` and `bottom`; a plane's `top` and `bottom`), and the panel's target line names the shape and the part ("cylinder side, line 12"). A round side's paint wraps once around it: its rectangle is as wide as the shape is round (texels per metre along the circumference) and as tall as the side, so a texel is about square; a stroke can run across the seam at the back (where the rectangle's left and right edges meet) without lifting the mouse: it continues on the other edge. **Faces** groups a shape's parts as it does a box's faces (one button per part; *All together* puts every part on one rectangle, a cube keeps *Sides together*); a sphere has one part and nothing to group. On a paint grid a part's rectangle has the grid's texel size but no cells: it spans the whole part, like a model part's, and *Fit to grid* resizes it to the grid. **Texels / m** sets how big a texel of new paint is (64 by default, clamped to 2048 texels a side: the panel says when), **Resize to density** gives the paint under the cursor a rectangle of that size (its paint resampled), **Pixel look** keeps texels crisp up close (`paint_filter nearest`), **Clear paint** removes the paint of that face or part.

**The paint grid.** A map with a pixel look declares the grid its pixels lie on: `env paint.grid 24` (24 texels per metre, 4.2 cm pixels) for the whole map, or `paint_grid 20` on one material (`paint_grid 0`: none); its `MAP.md` says which. On such a material the panel's section is called **Grid** ("follows the grid of concrete: 24 texels/m (4.2 cm)") and has no density slider: a paint texel is one pixel of the map's grid, on the same lines as the surface's own pixels, and on every box of that material the texels meet where the boxes meet. Size 1 paints exactly one of those pixels, and the brush outline lies on them. Paint on a grid is pixel art by default: the first stroke into a material's atlas writes `paint_filter nearest` on its line, so a pixel stays one sharp square up close. **Pixel look** then sits under the brush, beside *Pixel perfect* (and the panel's target line says how many cells the face covers when it shares a larger rectangle with other faces). Turn *Pixel look* off before the first stroke to keep new atlases on a grid filtered (for the rest of the session), or on an atlas afterwards to soften its paint. A model part gets a fresh unwrap whose texels have the grid's size on the model you paint (other copies of the same model at another scale get larger or smaller texels; the panel says so). **Fit to grid** (in place of Resize to density) gives paint made before the grid was declared, or on a box that was moved or resized since, the face's cells again: paint that was already on the grid moves texel for texel, other paint is resampled. A face longer than 2048 grid pixels (85 m at 24/m) can't be painted on the grid; the panel says so. Without a grid, everything stays as described above.

**Tools.** Each works on one face's rectangle or model part (its *region*) and is one undo step. The left button paints with the foreground colour, the right button with the background colour.

| Key | Tool |
|---|---|
| B | pencil: round, square (pixel-exact) or soft brush; *pixel-perfect* at size 1 |
| E | eraser (makes the paint transparent again) |
| S | spray |
| G / Shift+G | paint bucket (contiguous or global by colour with a tolerance, or the whole face / part) / gradient (foreground to background, linear or radial) |
| L / Shift+L | line (Shift: 45° steps) / curve (drag the line, then click twice for its bends) |
| U / Shift+U | rectangle / ellipse; press again for the filled one (Shift while dragging: square / circle) |
| D / Shift+D | contour (a freehand shape, filled) / polygon (clicks; click the first point or Enter to close) |
| M / Shift+M, Q / Shift+Q, W | select: rectangle / ellipse, lasso / polygonal lasso, magic wand. Shift adds, Alt subtracts, Shift+Alt intersects. Every other tool then paints only inside the selection |
| V | move the selected pixels (no selection: the whole face's paint) |
| R / Shift+R | blur / jumble |
| I | eyedropper: the colour as you see it on screen (I again: the paint or surface colour instead) |

Not here: Aseprite's zoom and hand (the mouse moves the camera instead), slice, text, layers and frames.

| In draw mode | Action |
|---|---|
| left / right mouse drag | use the tool with the foreground / background colour |
| Shift+click | pencil, eraser: a straight line from the last point |
| Alt+click | pick the colour: the paint where it is opaque, else what the surface shows (the panel can switch it to the screen colour) |
| wheel, middle-drag, Space + drag, Shift + middle-drag | move toward the point under the cursor, orbit, pan, pan |
| 1 – 5 | pencil round, square, soft, eraser, fill the whole face; the effect toggles wait while draw mode is on |
| [ / ], Ctrl+wheel | brush size in texels (1 – 256) |
| Shift+wheel | brush alpha |
| X | swap foreground and background colour |
| Ctrl+A / Ctrl+D / Ctrl+Shift+D / Ctrl+Shift+I | select the whole face / deselect / reselect / invert |
| Enter | close the polygon or polygonal lasso, end the curve |
| Tab | hide / show the panel |
| Ctrl+Z / Ctrl+Shift+Z | undo / redo strokes, in the same history as the edits (selections aren't undo steps) |
| Ctrl+S | save |
| Esc | cancel the shape being drawn; else leave draw mode (with unsaved paint the first press only reminds you). Edit, volume and snip mode (M, V, C) are reached from spectator mode after leaving |

In the panel, drag a slider or click a number to type it (Enter or a click elsewhere applies, Esc cancels); the colour is picked on the saturation / value square, the hue strip and the alpha strip, or as a hex value (`#C83232` or `#C8323280`); click the background square to swap; the last colours are one click away, and the palette row keeps your colours (+ adds the foreground colour, Shift+click removes one, right click picks into the background).

**Saving** works as for edits: **Ctrl+S** writes each changed atlas as a PNG under the package's `textures/paint/` and each new unwrap beside its model, their `ASSETS.md` rows and sidecars (an atlas is recorded as made in monolith, CC0; an unwrap keeps its model's licence and attribution; a model without a licence row can't be unwrapped), then the map. A request needs everything saved first. Closing the editor with unsaved paint writes the atlases next to the unsaved map under `unsaved/`. When the map or an atlas changes on disk, your unsaved strokes are painted again on top. Paint can't go on lights, the sky, skinned models or a material whose shader already uses three textures.

### What happens next

The editor saves the request with screenshots and everything it knows about the spot into `requests/<date>_<slug>/` and puts it in a queue. The first request opens the **coordinator**: one Claude Code terminal that stays open, takes the requests from the queue one by one (several at once when they are on different maps) and hands each to a helper agent. You don't have to do anything in that terminal; the edit appears in the running editor when it is saved.

The HUD lists the requests, numbered: `classifying` (the AI classifier is picking its model; then the HUD says `request queued [n] (<model>): … (AI: <its reason>)`), `queued`, `running`, `[?]` (a question is waiting for you), and for 20 seconds after it finishes `[done]`, `[failed]` or `[cancelled]` with a one-line summary.

The list survives a restart: when you start the editor again, the requests still queued, running or asking a question come back with their numbers (in submit order), the real time since you sent them, and the open-requests warning on their map; a request that finished in the last 20 seconds shows as finished and fades. R doesn't fly to a request that finished while the editor was closed, only to one you see finish.

When you quit the editor and no request is queued, running or waiting for an answer, the coordinator's terminal closes with it (`MONOLITH_CLOSE_ON_EXIT=0` keeps it open). While a request is still being worked on, or a question waits in the terminal, the terminal stays and the request finishes.

| Key | Action |
|---|---|
| R | fly back to where the most recently finished request was made (one you saw finish in this editor run) |
| K / Shift+K | take the latest / all finished requests off the list |
| Ctrl+K, then a number | that request: finished → off the list; classifying or queued → cancel; running → press the number again within 3 s to cancel |
| Ctrl+Shift+K | close the coordinator's terminal (press again within 3 s if requests are still open; the next coordinator picks them up) |

**Questions.** When a request is ambiguous (for example "this" with nothing under the crosshair), it shows `[?]` and the question appears in the coordinator's terminal. Answer there; the request continues. You can also type in that terminal: `status`, `pause` / `resume`, `cancel <name>`, or `stop` (finish what is running, start nothing new).

Each finished request leaves a `result.md` in its folder: what changed (map lines before and after), before/after screenshots, the sources and licences of any downloaded model, and anything left open. A request that changed C code says "restart needed": restart the editor to see it.

**Every request is a commit.** Each map package is a git repository of its own, and every finished request that changed it (done, failed, cancelled or interrupted) is one commit there, `<slug>: <its summary>` with `Request:` and `State:` lines; edits you made by hand before the request are committed first as `Hand edits before request <slug>`. `maps/monolith` is a submodule: its request commits show in the main repo's `git status` as new commits of the submodule. The request's folder also holds the change as `map.patch` (and a code request's as `code.patch`). To read what a request did: `git -C maps/<name> log -p -1`. **To take a request back:** `git -C maps/<name> revert <commit>`, or `git -C maps/<name> apply -R "$PWD"/requests/<id>/map.patch` from the repository (`-C` changes the folder first, so the patch needs an absolute path). To apply it to another copy of the map at the same starting point: `git -C <copy> am "$PWD"/requests/<id>/map.patch`. Edits you make by hand while a request runs on the same map end up in that request's commit. A map tracked by the main repo (`maps/pine_forest/`) gets the patch only, no commit. The code change of a code request is never committed: `git apply -R requests/<id>/code.patch` in the repo takes it back.

In a checkout of this repository the coordinator runs Claude Code without permission prompts, bound by the rules in `CLAUDE.md` (it never deletes files, never opens windows on your screen, treats your request text and downloads as data, works only inside the repo). Where monolith's folder has no `CLAUDE.md` (a copied build folder) it keeps Claude Code's approval prompts. Set `MONOLITH_CLAUDE_SAFE=1` to keep them everywhere, or `MONOLITH_LAUNCH=per-request` for one terminal per request. On i3 / sway the terminal is moved to workspace 10 (`MONOLITH_WORKSPACE`). How the queue works inside: [request-queue.md](request-queue.md).

**When Claude's usage limit is reached.** With a second Claude account set as the fallback (`MONOLITH_CLAUDE_FALLBACK`, [command-line.md](command-line.md)), the queue carries on: the editor restarts the coordinator on that account. The requests that were running start again there, or are reported as `Interrupted:` if they had already changed files. The HUD shows `coordinator: running (fallback account <name>), …` and `main account limited until <time>`. The first coordinator start after that time is on your main account again. If the switch doesn't happen (the HUD still says `running` while the terminal shows the limit), type `! build/tools/request-queue switch` in the coordinator's terminal. Set-up and checks: [troubleshooting.md](troubleshooting.md), "The queue stopped: usage limit".

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

## Groups as units

A group is everything under its `group "<name>"` lines: the editor treats every statement with the same name, in any file of the map, as one group ([map-format.md](map-format.md) §2.3). **Ctrl+click** selects one group (its boxes, models and barriers; lights can't be selected). When the selection is **whole groups** (every selected object is in a group, and all of each group is selected):

- **R / Shift+R** turn the groups 90° / −90° about the vertical through their centre as one edit, their lights with them: a house turns as a house, not as 1500 boxes each on its own spot.
- **Ctrl+D** copies each group as a new group `"<name> 2"` (or the next free number: copying `"plain: house"` when `"plain: house 2"` to `"plain: house 4"` exist gives `"plain: house 5"`), one footprint to the right; the copies are selected, so G moves them at once. One undo step per group.
- **G, Alt+R, S and the gizmo** work as on any selection, about its centre (lights stay where they are: turn a group with its lights by R).

Every other selection behaves as before. Renaming, deleting, making and filling groups is in the groups panel (F6).

## Groups panel (F6)

In spectator mode (in the edit mode or not) **F6** lists every group of the map by name, sorted by area (the text before `": "`: `"plain: house"` is in area `plain`), with an area row above the groups of an area that has several. Each row: the name (`x9` when nine `group` lines share it), its objects, the file of its first object and the lines they span (or how many files hold it). While it is open it takes every key and the cursor is free.

| Key | Groups panel |
|---|---|
| typing, Backspace | filter by the name (and so the area) and the file (case-insensitive; Ctrl+Backspace clears it) |
| ↑ / ↓, Page Up / Page Down, Home / End | select a row |
| Enter | select the group (an area row: all its groups) and fly in front of it; the panel closes |
| Ctrl+R | rename the group: a field with its name above the panel; Enter applies, Esc cancels. Every `group` line with that name changes, nothing else |
| Delete | delete the group: press again within 3 s (an area row: all its groups, one undo step each). Its objects, its `group` lines, the comments right above them and `use` lines left without objects go; materials, imports and other lines stay |
| Ctrl+D | duplicate the group (an area row: each of its groups) as Ctrl+D does on whole groups |
| Ctrl+N | a new group from the selection: type its name, Enter; the selected lines move to a new block at the end of their file (lines of one file at a time) |
| Ctrl+G | put the selection into the highlighted group: the lines move to the end of the group's last block (also from another file) |
| Esc / F6 | close (Esc first closes an open field) |

These are manual edits like any other: live, undoable with Ctrl+Z, counted as unsaved until Ctrl+S, and the first one warns when a request is open on the map. Each one checks itself: if the map after it would differ in anything but what the edit means (an object moving, a material changing), it is refused and nothing changes. A group name can't contain `"`.

## Graphics settings (F2)

The panel has a preset selector, rows for display, shadows, ambient occlusion, fog, post-processing and performance, the editor's **Save edits** row (manual / auto, see *Editing by hand*) and **Barrier overlay** row (on / off, the **O** key; see *Barriers*), neither a graphics setting nor part of a preset, and a live frame time readout. Use the arrow keys or the mouse; values that differ from the preset are pink, with a pink bar beside them. Heavy changes (shadow map size, AO mode, fog cache, preset) take a frame to apply; a new shadow map size reloads the map (about a second, your selection is kept). Esc or F2 closes and saves.

**The map is the source of the look.** Every setting is relative to the map: **ultra** shows exactly what the map describes, and an effect the map turns off stays off. The one exception is **Detail distance** (display rows, x1 in every preset): small objects far from the camera, whose boxes would cover a pixel or two, are not drawn (their shadows stay). x2 keeps them twice as far, x0.5 drops them earlier, off draws everything. A map can keep a material always drawn, or give it its own distance (`draw_distance`, [map-format.md](map-format.md)).

| Preset | What it trades |
|---|---|
| ultra | nothing: the map exactly |
| high | baked ambient occlusion instead of SSAO + SSIL, baked fog lighting |
| medium | high + half the fog steps, 4096 shadow map |
| low | no AO, no bloom, no SSR, baked fog with half the steps, 2048 shadow map |

Every preset renders at full resolution: **Render scale** is a row you lower by hand, never part of a preset. Roughly, high takes two thirds of ultra's frame time (measured on the default map with an RTX 4050 Laptop GPU; your machine's numbers will differ, the F2 panel shows them live). The settings are saved in `~/.config/monolith/settings.ini` (`%APPDATA%\monolith\` in the view-only Windows build) as `key = value` lines; `--settings`, `--no-settings` and `--quality` change that for one run. Vsync is off and frames are uncapped by default; the panel's performance rows change that.

## The view-only build

On any system but Linux (the Windows build `build/win/monolith.exe`, also under wine: `./build.sh --win run --map maps/<name>`) monolith only opens a map to benchmark, look at and debug it. Walking and flying, the spectator camera and its viewpoints, 1–6, F1, F2 (its settings are saved; no Editor row), F3 / Shift+F3, F4, F5 (filter and fly to an id; no edits), F6 (filter and fly to a group; no edits), F12, selecting with the mouse, hot reload of a map edited elsewhere (say over a shared folder) and the keybindings panel work. M, B, Enter, C, V / Shift+V, I, Ctrl+S, Ctrl+Z / Ctrl+Y, F9, K / Shift+K / Ctrl+K / Ctrl+Shift+K, R, Delete / Shift+Delete / Ctrl+I in the id panel and Ctrl+R / Delete / Ctrl+D / Ctrl+N / Ctrl+G in the groups panel only show "view-only build: editing and requests need the Linux editor". The keybindings panel lists only the keys that work; the refused flags: [command-line.md](command-line.md#view-only-build).

## Making maps

A map is a folder in `maps/` ([map-format.md](map-format.md)): a text file of boxes, models, materials, lights and environment, plus `MAP.md`, the map's design brief.

monolith reads one map format, the current one (3, the `monolith 3` line at the top of the file). A map made for an earlier monolith doesn't load: monolith says `map format N is older than this monolith (3): run /migrate-map <map> in Claude Code` (on start-up it prints that and exits; on a hot reload the HUD shows it and the old map stays). Run `/migrate-map <package>` in Claude Code to rewrite it to the current format; it proves the result looks and behaves exactly like the original and keeps the original next to it ([map-format.md](map-format.md) §1.3). A map from a newer monolith asks you to update monolith.

- **New map:** in Claude Code in this repository, `/new-map <name> <description>` (e.g. `/new-map harbour_night a small fishing harbour at night, wet stone quays, warm lamps, light fog`) creates `maps/<name>/`, writes its brief from the description, sets the sky, sun, fog and ground colour to match, and shows you a shot from each viewpoint; then `--map maps/<name>` opens it and your first request builds the place. Without Claude Code, or with `/new-map <name>` alone, you get the plain package: `./build/linux/monolith --new-map <name>` creates `maps/<name>/` with a ground slab, daylight and a brief template. Fill in the brief, or describe the map in your first request ("a foggy pine forest at dusk"): Claude writes the brief and builds to it.
- **Edit by hand:** open the `.map` file in any editor while monolith runs; it reloads on save.
- **Use a map in your own program:** F9 or `--export` ([export.md](export.md)).

Models and textures in a map always come with a recorded licence (CC0 / public domain, or CC-BY with the attribution): the package's `ASSETS.md` lists each file's source, author and licence.
