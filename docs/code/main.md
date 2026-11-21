# `src/main.c`: the editor's entry point, keys and command line

monolith is an AI-assisted map designer with deterministic edits, for 3D levels of any style (raylib + R3D).

The level is a map package (`maps/<name>/`): a plain-text `<name>.map` of boxes, models, materials, lights and environment, plus its design brief `MAP.md`, shaders, models, textures and `ASSETS.md`. The format is described in [monolith_scene.md](monolith_scene.md) (the scene runtime). The scene runtime (`runtime/monolith_scene.h`) loads, draws and hot-reloads the package while the editor runs. The default map is `maps/monolith/`.

In spectator mode you point at things, select boxes and models or drag out screen regions, and type a request. The editor writes `requests/<id>/` and queues it for the request coordinator: one Claude Code session in its own terminal, which the editor starts when none runs (`.claude/skills/coordinator`) and which hands each request to a subagent (`.claude/skills/map-request`).

Engine reference: [architecture.md](../architecture.md).

## View-only build

The editor edits and reaches the AI only on Linux (+ i3 / sway). Built for any other platform (today `build/win/monolith.exe`, run on Windows or under wine: `./build.sh --win run`) it is a viewer, to benchmark, view and debug a map (`MONOLITH_EDITING 0`, `src/common.h`).

**Works:**

- walking / flying, the spectator camera (TAB, viewpoints, P, G);
- 1..6, F1, F2 (without the Editor row; the settings file is saved), F3 / SHIFT+F3, F4 / SHIFT+F4;
- F5 read-only (filter; ENTER flies there and selects);
- F12;
- selecting with the mouse (the HUD shows the object's line);
- the ESC menu and keybindings panel (it lists only these keys);
- hot reload of a map edited elsewhere;
- the flags not listed below (`--shot`, `--bench [--headless]`, `--dump-*`, `--tri-report`, `--inspect-model`, `--input-script`, the graphics and debug flags).

**Refused:**

- keys: M (the edit mode and everything in it: the transform modal, the rotate gizmo, I, CTRL+D, DELETE, CTRL+I), B, ENTER, C, V / SHIFT+V, CTRL+S, CTRL+Z / CTRL+Y, F9, K / SHIFT+K / CTRL+K / CTRL+SHIFT+K, R, and DELETE / SHIFT+DELETE / CTRL+I in the id panel. The HUD says "view-only build: editing and requests need the Linux editor" (`MSG_VIEW_ONLY`, `src/msg.h`).
- flags: `--apply-edits`, `--edit-script`, `--paint-script`, `--new-map`, `--submit`, `--submit-wait`, `--close-submitted`, `--type`, `--classify`, `--edit-mode`, `--edit-id`, `--edit-modal`, `--edit-save`, `--model-picker`, `--model-picker-key`, `--draw-mode`, `--draw-panel`, `--draw-state`, `--cursor`, `--region`, `--volume`, `--bake-surface` (and its `--bake-*` options), `--export` and `--compile` print that message and exit 1 before anything is written.

The launcher (`launcher.c`) is not in that build.

## Controls

| Key | Action |
|---|---|
| WASD / mouse | walk & look |
| SHIFT | walk faster |
| SPACE | jump |
| F | toggle fly (noclip) |
| 1..6 | toggle effects (volumetric fog, fog, SSAO, SSIL, bloom, sun): each key flips that effect relative to the map's setting; flips survive hot reloads |
| F1 | toggle HUD (also hides the crosshair and selection overlay) |
| F2 | graphics settings panel (see below) |
| F3 | render inspector (see below) |
| SHIFT+F3 | the inspector panel (see below) |
| F4 / SHIFT+F4 | R3D buffer view: scene, albedo, normal, orm, radiance, specular, ssao, ssil, ssgi, ssr, bloom, dof (named in the HUD) |
| F5 | the id panel (see below) |
| F12 | screenshot |
| TAB | toggle spectator camera |
| F9 | export the open map as a self-contained folder (package, scene runtime, R3D, `build.sh`, README) into `export/<map>-<YYYY-MM-DD_HHMMSS>/` next to `requests/` (`export.c`, like `--export`) |
| ESC | closes the prompt box / leaves snip or volume mode / closes the panel (no quit: close the window); with nothing to close: the menu (see below) |

**F2, the graphics settings panel** (`settingspanel.c`): preset low/medium/high/ultra, then the Display, Shadows, Ambient occlusion, Fog, Post and Performance rows. UP/DOWN select, LEFT/RIGHT (or the mouse: arrows, wheel) change. Changes apply at once (a shadow map size restarts the renderer, ~1 s). ESC / F2 close; the settings are saved to the settings file on close. Camera and editor keys are frozen and the cursor is free while it is open.

**"(cached)" after the fps:** nothing changed, and the frame reused the last R3D image.

**F3, the render inspector** (spectator mode; `inspector.c`): outlines of what is drawn, cycling off → by material → by shader → by draw kind (merged cell mesh / instanced / model), with labels of the draw under the crosshair and the 24 nearest ones.

**SHIFT+F3, the inspector panel:** materials, then shaders (draws, triangles). Type to filter, UP/DOWN select, SPACE hide / show (shadows too), I isolate (again: all back; CTRL+I while a filter is typed), ESC close. It takes the keys while open.

**F5, the id panel** (spectator mode; `idpanel.c`): every `id N` line of the map (id, line, kind, objects, file or material, group). Type to filter, UP/DOWN select, ENTER fly there and select, DELETE remove the id prefix (the objects stay), SHIFT+DELETE remove the objects, CTRL+I id for the selection's line, ESC / F5 close. Edits are live and undoable (CTRL+S saves). It takes the keys while open.

**ESC menu:** with nothing to close, ESC shows the menu with one "See keybindings" button. ENTER or a click opens the keybindings panel (`keyspanel.c`): one section per mode, LEFT / RIGHT or a tab change the section, typing searches all of them, ESC / F1 close. The HUD shows state only, no key lists.

**Reload line:** the HUD's bottom line shows "map reloaded (N boxes)" after an edit to any file of the map package (map, shaders, textures), or the error (`file:line: message`) in red while the package is broken (the old map stays loaded).

## Spectator

| Key | Action |
|---|---|
| WASD / mouse | move & look |
| E/Q or SPACE | up/down |
| mouse wheel | base speed |
| SHIFT / ALT | speed x4 / x0.25 |
| LEFT/RIGHT | fly to previous/next viewpoint |
| P | print + copy current view as `--pos`/`--look` arguments (`--pos` = feet) |
| G | move the player here |

## Map designer

Spectator mode; see `designer.c` / `launcher.c`.

### Selecting

| Key | Action |
|---|---|
| crosshair | the box or model under it is outlined (models: their rotated bounds); the HUD shows its group and map line (models: file and transform too) |
| LMB | select the object under the crosshair (replaces the selection) |
| SHIFT+LMB | add/remove that object |
| CTRL+LMB | select its whole group |
| CTRL+SHIFT+LMB | add the whole group |
| RMB | clear the selection |

The selection survives map hot reloads: objects are matched by identical geometry (models: world bounds), else by the same map line + index within it + group; the HUD says if any got lost.

### The prompt box (ENTER)

ENTER opens the request prompt box. In the box: type, BACKSPACE/DELETE (CTRL+BACKSPACE: a word), LEFT/RIGHT/UP/DOWN, HOME/END (with CTRL: the whole text), CTRL+V paste, SHIFT+ENTER new line, ENTER send to Claude Code, ESC cancel (discards the text). While the box is open every editor key and the camera are frozen and the mouse cursor is free. If sending fails, the box stays open with the text.

### Snip mode (C)

The camera freezes and the cursor is free.

- Drag: a screen region (replaces the others); SHIFT+drag: add one.
- ENTER: the prompt box with the regions (and the object selection made before C) attached.
- RMB / ESC / C: leave snip mode and drop the regions.
- ESC in the prompt box returns to snip mode with the regions; a successful submit ends it.

The bundle lists what each region shows (`designer.c`).

### Volume mode (V)

World-space boxes (the camera keeps flying).

- LMB on a surface: the first corner; LMB again: the opposite footprint corner; then mouse up / down or the wheel sets the height (camera frozen; SHIFT x4), and LMB / ENTER adds the volume.
- Points snap to N's step (0.1 / 0.25 / 0.5 / 1 / 2 m, default 0.5) on the axes along the clicked face; CTRL = no snap.
- RMB / ESC cancel the volume being made, else leave volume mode (as V does).
- In the mode: `[` `]` = the active volume; arrows / PGUP / PGDN move it one step (arrows along the world axes nearest to the view; they don't fly to viewpoints then); ALT + them grows that side, ALT+SHIFT shrinks it; DELETE removes it, SHIFT+DELETE clears all; ENTER = prompt box.

Volumes stay (through map reloads and submits too) until removed; they are never written to the map. The bundle lists what each volume holds (`designer.c`, `volumes[]`).

SHIFT+V makes a volume from the selection's bounds (or the crosshair object's).

### Edit mode (M)

The edit mode (`editmode.c`, [editmode.md](editmode.md)) makes manual edits of the selection as minimal text edits (`mapedit.c`, [mapedit.md](mapedit.md)) of the working copy, the map text the scene shows. Each one goes live at once (`MS_EditSetText`: only what changed is rebuilt, nothing is read from disk); no request is made.

**Camera and cursor.** In the mode the camera flies only while RMB is held (mouse look, WASD / E / Q / SPACE, wheel speed, SHIFT x4, ALT x0.25; the cursor is hidden then). Otherwise the cursor is free: what is under it is outlined, LMB / SHIFT+LMB / CTRL+LMB select there, and an RMB click (released within 0.25 s, moved < 4 px) clears the selection.

**The transform modal (G / ALT+R / S)** grabs / rotates / scales the selection (`transform.c`, [transform.md](transform.md)):

- the mouse or a typed number drives it;
- X / Y / Z lock an axis, SHIFT+X / Y / Z a plane; the same key again uses the active object's own axes (the first selected box or model), a third time none;
- CTRL held snaps (N step, the angle snap, 0.1), SHIFT held x0.1;
- LMB / ENTER commit one edit, RMB / ESC cancel;
- ALT+R about a vertical axis seen from the side: drag right and the near side goes right, 0.5 deg / px;
- boxes follow the turns the edits accept, the others are named in red; a turn that changes nothing (a square box by 90 deg) is named in red and writes nothing;
- stairs / waffles can't turn or scale.

**R / SHIFT+R** turn the selection 90 / -90 deg about the vertical, each object about its own centre / origin (one edit, no modal). **ALT+SHIFT+R** resets its rotation.

**The rotate gizmo** (with a box, block or model selected, no modal, RMB not held): X / Y / Z rings (red / green / blue, their front halves), a white view ring and a grey trackball disc at the pivot, 120 px.

- LMB drags a ring (the angle swept in its plane; edge-on: the turntable), the view ring (the dial) or the trackball (about the axis across the drag, 0.5 deg / px).
- CTRL snaps to the angle snap, SHIFT x0.1, digits type the angle.
- The release commits; a click keeps it open as the modal.
- L: the gizmo's axes global / local. SHIFT+N: the angle snap 1 / 5 / 15 / 45 / 90 deg (default 15).

**Other keys in the mode:**

| Key | Action |
|---|---|
| arrows / PGUP / PGDN | move the selection one snap step (N: 0.1 .. 2 m, shared with volume mode; the arrows follow the world axes nearest to the view and don't fly to viewpoints then; CTRL: no grid) |
| DELETE | remove it |
| CTRL+D | duplicate it |
| I | the model picker (`modelpicker.c`, [modelpicker.md](modelpicker.md)): the package's models listed in `ASSETS.md` with thumbnails, triangles, size and licence; type to filter, UP / DOWN select, ENTER places the model at the point under the cursor as one live edit and selects it, so G moves it; ESC, or I with nothing typed, closes; it takes every key while open |
| CTRL+I | give the selection's line an id (one line: a panel to type it, ENTER sets, ESC cancels; several lines: the next free ids) |
| G | the grab (in the mode, G is not "move the player here") |
| M / ESC (and V, C) | leave; with unsaved edits the first M / ESC only says so |

The HUD shows the snap step, the first selected object's map line and entity id, and the last edit. With a request of this run queued or running on the map, the first edit after the last save (and a save) only warns (again within 3 s: go ahead).

### Draw mode (B)

The draw mode (`drawmode.c`, `drawpanel.c`; [drawmode.md](drawmode.md), [painting.md](../painting.md)) paints on boxes and models, a layer over their material (its look stays wherever it isn't painted). The keys are Aseprite's (painting.md 18.3; the tool keys are one table, `src/drawkeys.c`). The cursor stays free and the mouse moves the camera (`Spectator.mouseNav`, painting.md 18.7): no keyboard flight, no mouse look; RMB no longer flies here (edit mode keeps RMB flight).

| Key | Action |
|---|---|
| B, E, S, G / SHIFT+G, L / SHIFT+L, U / SHIFT+U, D / SHIFT+D, M / SHIFT+M, Q / SHIFT+Q, W, V, R / SHIFT+R, I | the tool: pencil, eraser, spray, bucket / gradient, line / curve, rectangle / ellipse (again: filled), contour / polygon, marquee / elliptical marquee, lasso / polygonal lasso, magic wand, move, blur / jumble, eyedropper (again: screen ↔ paint) |
| LMB drag | the tool (pencil, eraser, bucket in whole-region mode paint; the others come with the tools' round) |
| ALT+LMB | pick the colour (an opaque paint texel, else the surface's albedo) |
| 1..5 | pencil round / square / soft, eraser, bucket in whole-region mode (the effect toggles wait) |
| `[` / `]` or CTRL+wheel | the size in texels (1..256) |
| SHIFT+wheel | the foreground alpha |
| wheel | toward / away from the point under the cursor (20 % of its distance per notch) |
| MMB drag | orbit around the point under the cursor at the press |
| SPACE + LMB drag, SHIFT + MMB drag | pan (nothing is painted while SPACE or MMB is down) |
| LEFT / RIGHT, P | fly to a viewpoint; print the view |
| X | swap foreground and background |
| CTRL+A / CTRL+D / CTRL+SHIFT+D / CTRL+SHIFT+I, ENTER | the selection and the shapes' ENTER (the tools' round) |
| TAB | hide the panel |
| ESC | cancel the shape in progress; else leave (with unsaved edits the first ESC only says so). M, V and C don't leave: leave first |

The wheel and new drags don't act while the cursor is on the draw panel (`Spectator.navBlocked`); B enters draw mode from spectator mode only.

The panel is docked left: target, pen, colour sliders / hex / recent colours, texels per metre + Resize to density + Pixel look + Clear paint (on a material with a paint grid: the grid line + Fit to grid, no slider), the box's face groups + Apply.

The first stroke on a face or model slot gives it a rectangle in its material's atlas (`textures/paint/<material>.png`): a `paint` statement, the material's `paint` key, the line's id, one live edit. A model whose UVs can't carry paint is unwrapped first (a `.paintuv` sidecar). Strokes are undone with CTRL+Z in the same history as the edits. CTRL+S writes the atlases and unwraps (and their `ASSETS.md` rows and sidecars), then the map.

### Saving, undo and redo

**CTRL+S** writes the working copy to the map file (spectator mode). With the settings' Editor row "Save edits" = auto, every edit, undo and redo is written at once instead. "N unsaved edits" shows in every spectator mode while there are any; ENTER in the prompt box then sends nothing (CTRL+S in the box saves and sends). Closing the editor with unsaved edits writes the working copy to the settings folder's `unsaved/<map>-<YYYYMMDD-HHMMSS>.map` (the map file is not touched).

**CTRL+Z / CTRL+SHIFT+Z (CTRL+Y)** undo / redo manual edits (spectator mode; live, like edits). When the map changes on disk (a request, another editor), unsaved edits are applied again on top of it by entity id; the ones that no longer apply are listed in red until CTRL+SHIFT+DELETE. Without unsaved edits the history is dropped.

### Requests in the HUD

**R** flies back to the camera of the last finished request.

The HUD lists the coordinator ("running", "starting", "not running") and the open requests ("classifying": the AI classifier is picking its model; "queued", "running", "[?]" = a question waits in the coordinator terminal) and, for 20 s, finished ones ([done] / [failed] / [cancelled]) with the first line of their `result.md`. Each has a number [n], kept while it is listed.

| Key | Action |
|---|---|
| K | take the last finished request off the list |
| SHIFT+K | take all finished requests off the list |
| CTRL+K, n | request [n] (1-9, 0 = 10): finished = off the list; classifying or queued = cancel it; running = press n again within 3 s to cancel it (the coordinator stops its subagent and lists what had changed). ESC cancels the pick; the digits don't toggle effects meanwhile. Bundles always stay |
| CTRL+SHIFT+K | close the coordinator's terminal (press again within 3 s while requests are open; the next submit starts a new coordinator, which takes them up) |

With `MONOLITH_LAUNCH=per-request` (one terminal per request): K / SHIFT+K close the terminals of the last / all finished requests, CTRL+K n closes request [n]'s (running: press n twice); `launcher.c`, region "Closing request windows" ([launcher.md](launcher.md)): i3/sway mark + kill, else SIGTERM/SIGKILL.

## Command line

An unknown flag, a missing or unparsable value, a value that is itself a `--flag`, or a non-finite / out-of-range number prints a message and exits 1 before any window opens. The user-facing reference is [command-line.md](../command-line.md).

### Start position and view

| Flag | Meaning |
|---|---|
| `--pos x y z` | start position (feet; default: the map's spawn) |
| `--look yawDeg pitchDeg` | start view angles (default: the spawn's; with `--view`: override the viewpoint's) |
| `--eye x y z` | start position of the eye (= `--pos x y-1.62 z`); with `--view` it moves the spectator there instead (request bundles use it for their `--shot` command) |
| `--off <keys>` | start with these effects toggled off, e.g. `--off 34` (SSAO + SSIL) |
| `--view <n>` | start in spectator mode at the map's viewpoint n (1-based; a number beyond the map's viewpoints is ignored) |

### Shots

| Flag | Meaning |
|---|---|
| `--shot <file>` | render 30 frames offscreen (1600x900) in a hidden window (never mapped, so the window manager doesn't show it), save the image to `<file>` and exit. No hot reload or request polling during shots |
| `--shot-size <w> <h>` | size of the `--shot` image (default 1600 900); request bundles pass their window size so the reproduction matches `view.png` exactly |

### Graphics settings

| Flag | Meaning |
|---|---|
| `--ao <mode>` | screen-space AO / indirect light trade-off (`atmosphere.c`): full (default), ssao (SSIL off), lite (SSIL off, SSAO 8 samples), off (SSAO + SSIL off), baked (SSAO + SSIL off, the baked occlusion volume instead: `--ao-volume 0.5 0.6`) |
| `--ao-volume <voxel> <intensity>` | trade-off, off by default: bake a world-space occlusion volume of the boxes once per map load (voxel size in m, 0.25..4) and darken the ambient light with it (R3D patch 11; on top of SSAO/SSIL unless `--ao baked`) |
| `--bloom on\|off` | `off`: bloom pass off, exposure x the map's `env bloom.offExposure` instead (default 1.19: same mean brightness, no faint halo at bright openings; `atmosphere.c`); `on`: bloom on whatever the settings say. Default: the settings (on) |
| `--quality <low\|medium\|high\|ultra>` | graphics quality preset for this run (`MS_Quality` in `runtime/monolith_scene.h`; ultra = the map exactly). Default: the settings file's preset in the interactive editor, else ultra |
| `--settings <file>` | graphics settings file to read (and, from the panel, write) instead of `~/.config/monolith/settings.ini` (`$XDG_CONFIG_HOME`; Windows: `%APPDATA%\monolith\settings.ini`; else next to the executable) |
| `--no-settings` | neither read nor save a settings file |
| `--settings-set key=value` | change one setting as if in the panel (repeatable, after every other flag): applies to this run and is saved to the settings file (in the automated runs listed below only with `--settings <file>`). Keys = the file's (`settings.c`), e.g. `preset=medium`, `render_scale=0.75`, `ao=baked` |
| `--settings-change key=value` | with `--shot`: after 10 frames, apply this change through the panel's live path (repeatable; `shadow_size` restarts the renderer). The shot must equal a run started with `--settings-set key=value` |
| `--fx key=value` | override one effect setting, repeatable, e.g. `--fx ssil.radius=2.5` (keys: `ssao.`/`ssil.samples`, `.radius`; `ssao.intensity`/`.power`; `ssil.gi`/`.ao`; `bloom.levels`/`.intensity`; `exposure`; `vfog.step`/`.length`). Applied after the map's environment and the quality settings (`--quality`, `--ao`, `--bloom`, ...). Values must lie in the range of the matching env key (e.g. `bloom.levels` 0..1) |
| `--shadow <size> <range>` | sun shadow map size (px) and distance (m), overriding the map's `env shadow.size` / `sun.range` (the default map: 8192 100) |
| `--shadow-camera` | sun shadow from R3D's camera-fitted map, re-rendered only when the cached map no longer covers the view (the pre-tile path; default: static world-fixed tiles drawn once per map load, see `monolith_scene.c`; A/B tests) |
| `--shadow-always` | camera-fitted map re-rendered every frame (A/B tests and debugging) |
| `--fog-cache <voxel> <sub>` | trade-off, off by default: bake the sun visibility of the volumetric fog into a 3D texture once per map load (voxel size in m, sub^3 samples per voxel, e.g. 0.5 2) and let the fog march fetch it instead of the shadow map (R3D patch 8); changes the light shafts slightly. Region: the map's `env fogCache.region`, else derived from its bounds |
| `--render-scale <s>` | trade-off, default 1: R3D renders at s (0.25..1) times the window / shot size and upscales the result (`--upscale nearest\|linear\|bicubic\|lanczos`, default bicubic). 0.75 renders 56% of the pixels |
| `--window-msaa` | 4x multisampled window (the default before 2026-10-11): smoother designer overlay lines for ~0.1 ms GPU per frame; the scene image is the same |
| `--no-prepass` | no depth pre-pass before the G-buffer pass (R3D patch 6; same image, for A/B tests: a material's surface shader then runs for every overdrawn fragment) |
| `--no-frame-reuse` | always render with R3D. By default the interactive window reuses the last R3D image while nothing that affects it changed (camera, effect toggles, sun, map version, window size): the frame is then only the cached image + HUD/overlay, and the HUD fps line says "(cached)". See the region "Frame reuse" in `src/main.c` |
| `--frame-reuse` | turn frame reuse on where it is off by default: `--shot` (to check that a reused frame equals a fresh one; logs how many frames were reused) and `--bench-still` (times a reused frame; report suffix `_reuse`, no GPU profile) |

The flags `--ao`, `--ao-volume`, `--bloom`, `--shadow`, `--shadow-camera`/`-always`, `--fog-cache`, `--render-scale`, `--upscale`, `--window-msaa`, `--no-prepass` and `--(no-)frame-reuse` are graphics settings too: given, each overrides that one setting for this run (after `--quality`). None of them is saved.

Only the interactive editor reads the default settings file: `--shot`, `--bench`, `--dump-*`, `--apply-edits`, `--export`, `--compile`, `--classify`, `--bake-surface` and `--tri-report` (the `automated` runs in `main()`) never read or save settings unless `--settings` names a file, so their images and numbers stay deterministic.

### Panels and views in shots

| Flag | Meaning |
|---|---|
| `--settings-panel` | with `--shot --shot-hud`: draw the open settings panel into the shot |
| `--inspect <material\|shader\|draw>` | with `--shot`: the render inspector's outlines (F3) in the shot; its labels and HUD line with `--shot-hud` |
| `--output-mode <view>` | R3D buffer view (F4): scene (default), albedo, normal, orm, radiance, specular, ssao, ssil, ssgi, ssr, bloom, dof |
| `--inspector-panel` | with `--shot --shot-hud`: draw the open inspector panel (SHIFT+F3) |
| `--keys-panel [section]` | with `--shot --shot-hud`: draw the open keybindings panel at the first section whose name contains `section` (e.g. edit) |
| `--id-panel [filter]` | with `--shot --shot-hud`: draw the open id panel (F5), with this filter typed |
| `--inspect-hide <name>` | hide a material (name) or shader (path) from every pass, as SPACE in the inspector panel (repeatable). A name the map has no material or shader for exits 1 |
| `--inspect-isolate <name>` | draw only what uses it (I). A name the map has no material or shader for exits 1 |

### Benchmarks

| Flag | Meaning |
|---|---|
| `--bench` | visit every viewpoint of the map, spin the camera, print frame time stats, write a Markdown report to `benchmark/<timestamp>_shadow-<size>-<range>.md` and exit |
| `--headless` | with `--bench`: hidden window + offscreen target + `glFinish` per frame, so nothing appears on screen (compare headless runs only with each other) |
| `--bench-frames <n>` | timed frames per viewpoint (at least 10, default 600); fewer = quick screening runs (the camera spins faster, so the shadow map updates more) |
| `--bench-views <list>` | with `--bench`: only these viewpoints, 1-based, comma-separated, in this order (e.g. `1,3,4`; default every viewpoint); exit 1 on a bad list |
| `--bench-still` | like `--bench`, but the camera doesn't spin (the still-camera ideal: no shadow-map re-renders); the report gets a `_still` suffix |
| `--bench-window <samples>` | with `--bench --headless`: render into an offscreen stand-in for the visible window (a framebuffer with `<samples>` samples per pixel: 4 = the window's `FLAG_MSAA_4X_HINT`, 0 = single-sample), draw the spectator HUD and overlay on top and resolve it like the swap does (`windowemu.c`), to time the window-only costs without a visible window |
| `--no-profile` | with `--bench`: no per-pass GPU timings (R3D profiler, on by default when R3D is built with `R3D_GPU_PROFILER`, as `build.sh` does) |
| `--note "text"` | free-form note added to the benchmark report |

### Maps, dumps and tools

| Flag | Meaning |
|---|---|
| `--map <path>` | load this map package (folder) or `.map` file instead of `maps/monolith/` (see below) |
| `--reload-test <path>` | with `--shot`: after 10 frames, load this map (package or file) through the hot-reload path (as if the map had changed), then render the remaining frames. The shot must equal a plain `--shot --map <path>` (proves that every static cache is rebuilt on reload) |
| `--dump-boxes <file>` | load the map, write every box as hex floats (`%a`) to `<file>` and exit without opening a window (see below) |
| `--dump-scene <file>` | like `--dump-boxes`, plus model files, model instances (transform and world bounds as hex floats), model colliders, materials and lights (format: `src/dumpscene.c`). Maps with models load them in a hidden window |
| `--dump-scene-diff <file>` | the same without object indices and map lines, references by name (group, material, model file): diff two of these to see only the objects an edit changed, even after inserted lines (`src/dumpscene.c`) |
| `--dump-entities <file>` | load the map (no window), write every entity (box, model, light) with its stable id, source line, copy and transform (`src/dumpscene.c`) and exit |
| `--tri-report <file>` | the map's triangles as the runtime draws them (hidden faces left out), per viewpoint what the detail distance leaves out, then per group, material, map line and draw range (format: `src/trireport.c`); loads in a hidden window |
| `--inspect-model <file>` | print what a model file contains and exit, without a window (see below) |
| `--new-map <name\|path>` | create a map package and exit without opening a window (see below) |
| `--export <dir>` | write the map as a self-contained folder and exit without opening a window (see below) |
| `--bake-surface <material> <out-prefix>` | bake a material of the map into a tile and exit (see below) |
| `--compile <out>` | compile the map's text (`--map` or the default) into the compiled map `<out>` (`<name>.mapb`, [compiled-map.md](../compiled-map.md)), the file games load, and exit without a window. Checks the package like `--dump-boxes`; the same text always gives the same bytes. Paths in it are package-relative: write it into the package folder |

**`--map <path>`.** The path is relative to the CWD; the default is the repo's `maps/` while it exists, else `maps/` next to the executable. A folder loads `<folder>/<name>.map`, else its only `.map` file (else a compiled `<name>.mapb`). The package is reloaded whenever one of its files changes (format: [monolith_scene.md](monolith_scene.md)). A compiled map (`.mapb`, `--compile`) loads too, for the `--shot` and `--dump-*` checks; it has no text, so `--apply-edits`, manual edits and requests refuse it, and a warning says when its `.map` has changed since it was compiled.

**`--dump-boxes <file>`.** For checking that edits to the loader or the map keep the exact box list. A turned box is written as its world AABB, then `" rot y p r center x y z size x y z"`. Any map that fails to parse (with or without this flag) prints `file:line: message` and exits with status 1. Model files are imported on the CPU (a missing, truncated or unreadable one fails), but nothing reaches the GPU (no window).

**`--inspect-model <file>`.** Prints bounds, size, origin, mesh/triangle/vertex counts, materials (= the slots for `material.N`) and their textures (embedded or external, sizes), up axis and unit hints, glTF extensions, warnings (unlit or fully metallic materials, > 50k triangles, textures > 2K, unsupported extensions, no meshes) and a placement line (`src/inspect.c`). A model whose glTF / OBJ / MTL references leave its folder is refused (exit 1) before anything is read.

**`--new-map <name|path>`.** Creates the map package `maps/<name>/`: `<name>.map` with a ground slab, the default material and the default environment written out, a `MAP.md` template, empty `shaders/`, `models/`, `textures/` and `ASSETS.md`. A path (anything with a `/`) creates the package in that folder instead (its parent must exist; the name is its last component), e.g. a scratch draft. It refuses to overwrite an existing folder.

**`--export <dir>`.** Writes the map (`--map` or the default) as a self-contained folder that builds and runs on its own: the package with every file it references, the scene runtime without its editor parts, `viewer.c`, `build.sh`, the bundled R3D (+ raylib, assimp), licences and a `README.md` (`export.c`). It refuses a non-empty `<dir>` and writes nothing when a referenced file is missing. It needs the repo (not a copied build folder).

**`--bake-surface <material> <out-prefix>`.** Bakes a material of the map (`--map` or the default) into a tile and exits (hidden window; `src/bake.c`): one square quad of it rendered with an orthographic camera through R3D's albedo and ORM outputs into `<out-prefix>.png` (RGBA: `ALBEDO`, `ROUGHNESS` in A). Options:

| Option | Meaning |
|---|---|
| `--bake-face top\|side` | default top: the quad at y 0, u = x, v = z; side: z 0, u = x, v = y |
| `--bake-size <px>` | default 2048 |
| `--bake-texels <per m>` | default 24: the material's pixel grid; the tile repeats every size / texels m |
| `--bake-levels <n>` | 1..6, default 1: levels 0..n-1 of the pixel grid in one atlas (layout in `bake.c`) |

The material's shader gets `#define MS_BAKE 1`, `MS_BAKE_FACE`, `MS_BAKE_LEVEL`, `MS_BAKE_PERIOD`, `MS_BAKE_TEXELS` in front of its source; a map reads the tile with `data_sampler`.

### Edit and paint scripts

| Flag | Meaning |
|---|---|
| `--apply-edits <script>` | run an edit script (select / also / move / turn / rotation / scale / size / remove / duplicate / add / id / paint-add / paint-set / paint-remove / material-set / undo / redo / print; format: [mapedit.md](mapedit.md#edit-script)) against the map file of `--map`, which is required (the script rewrites that file), and exit without a window; exit 1 at the first failing command |
| `--edit-script <script>` | the same language applied live, as the edit mode does (see below) |
| `--edit-save <manual\|auto>` | the edit save mode for this run (settings: `edit_save`) |
| `--paint-script <script>` | strokes of the draw mode (see below) |
| `--draw-mode` | with `--shot`: open the draw mode (B); with `--shot-hud` its HUD line and the brush cursor at `--cursor` (default: the middle of the shot) |
| `--draw-panel [all\|target\|tools\|color\|density\|faces]` | with `--shot --shot-hud`: the draw mode with the draw panel (or one section of it; `pen` is an alias of `tools`), for HUD images; with `--input-script` the clicks go to the panel |
| `--draw-state <file>` | the draw mode's kept state from and to this file (docs/painting.md 18.6) instead of `<package>/draw.ini`; with `--shot` (which otherwise reads and writes none) or the editor |
| `--cursor x y` | with `--draw-mode` / `--draw-panel`: where the cursor is (shot pixels) |

**`--edit-script <script>`.** The edit script language applied live, as the edit mode does (the working copy, `MS_EditSetText`; the map file is not written), plus:

- `save`: writes the `--map` file like CTRL+S;
- `disk <file>`: the map file becomes that file's bytes, reloaded as by the hot reload, unsaved edits rebased;
- `preview move dx dy dz` / `preview turn ...` / `preview scale ...` / `preview rotation ...` / `preview end` (`MS_EditPreview*`; turn, scale and rotation as the transform modal previews them).

It needs `--map` and `--shot` or a `--dump-boxes` / `--dump-scene(-diff)` / `--dump-entities` flag. With `--shot` it runs after 10 frames (every cache drawn once), so the shot shows what the caches make of the edits; with a dump it runs right after the load (hidden window), then the dump. The result must equal `--apply-edits` on a copy of the map. Exit 1 if a command failed.

**`--paint-script <script>`.** Strokes of the draw mode ([painting.md](../painting.md) 14; the commands: the header comment of `DrawModeRunScript` in `src/drawmode.c`). With `--shot` it runs after 10 frames, after `--edit-script`, live (nothing written unless it says `save`); with `--apply-edits` after that script, in a hidden window (its `save` writes the PNGs and the map). Exit 1 if a command failed.

## Designer debug flags

For testing without a visible window.

| Flag | Meaning |
|---|---|
| `--pick` | with `--shot`: print what the crosshair (screen centre) hits on the shot frame: box or model index, group, map line + its text, point, normal, distance (models: file, position, rotation, scale, origin, bounds); then a `pick: surface` line: the material it is drawn with, the box face or the model's slot, mesh and triangle, the position on the face / texture coordinate, the sampled `uv` and barycentric coordinates ([painting.md](../painting.md) §6) |
| `--pick-at x y` | with `--shot`: the same for the ray through pixel (x, y) of the shot (top-left origin, as `--region` and `view.png`), e.g. the edges of a gap; repeatable, each printed after a `pick-at x y` line |
| `--select <line>` | select every box / model made by this map line (repeatable) |
| `--region x y w h` | attach a screen region (pixels of the shot, top-left origin), as if dragged out in snip mode (repeatable); with `--shot-hud` the shot shows the snip-mode drawing, with `--submit` the bundle gets the regions |
| `--volume x0 y0 z0 x1 y1 z1` | attach a volume: an axis-aligned box between two opposite corners in map coordinates, as if made in volume mode (repeatable); `--shot-hud` draws it, `--submit` puts it into the bundle (`volumes[]`) |
| `--submit "text"` | with `--shot`: run the real submit path (see below) |
| `--type "text"` | open the prompt box with this text (cursor at the end) |
| `--shot-hud` | with `--shot`: draw the HUD, crosshair, selection overlay, regions, volumes and prompt box into the shot (to look at the UI; plain shots never contain it) |
| `--edit-mode` | with `--shot --shot-hud`: open the edit mode (M), to see its HUD |
| `--edit-id <digits>` | with `--edit-mode` and `--select`: open its id panel (CTRL+I) for the selection with these digits typed (`""` for an empty field), to see it |
| `--edit-modal "<g\|r\|s> [x\|y\|z\|X\|Y\|Z] [typed] [commit\|cancel]"` | with `--shot` and `--select`: drive the transform modal (see below) |
| `--input-script <file>` | with `--shot`: the run's keyboard and mouse come from this script (see below) |
| `--model-picker [filter]` | with `--shot`: open the edit mode and its model picker (see below) |
| `--model-picker-key <enter\|close>` | with `--model-picker`: once the thumbnails are rendered, press ENTER or ESC (see below) |
| `--submit-wait <s>` | with `--submit`: keep polling the request (state, `result.md`, terminal process) for `<s>` seconds before exiting; state changes are logged |
| `--close-submitted` | with `--submit`: then close the submitted request like CTRL+K n (twice if it is still running; coordinator mode: cancels it) and wait up to 10 s for its terminal process to go; prints `close [n]: ...` and `close: terminal process gone after X s` |
| `--classify "text"` | run the launcher's model choice for this prompt and print it, then exit without opening a window (see below) |

In `--submit`, `--type` and `--classify` text, a literal `\n` becomes a new line.

**`--submit "text"`.** After saving the shot, runs the real submit path: render view + view-marked offscreen, `LauncherSubmit` with the centre pick, the `--select` objects, the `--region` regions and the `--volume` volumes, and print the launcher's message. This writes a real bundle into `requests/` and queues it (and may start a real coordinator: terminal + Claude Code). For tests, set `MONOLITH_REQUESTS_DIR` to a scratch folder and `MONOLITH_TERMINAL` to a stub script that only logs its arguments (`build/tools/stub-terminal`; `build/tools/test-coordinator` runs the queue end to end with a stub coordinator). It is refused (nothing written) while `--edit-script` left unsaved edits, as ENTER is.

**`--edit-modal`.** After 10 frames, opens the edit mode and the transform modal (G / ALT+R / S) on the selection with that lock (capitals: plane locks; doubled, e.g. `xx`: local axes) and typed value, as with `--shot-hud`, to see its HUD. `commit` / `cancel` end it after 20 frames (one edit, as LMB; or everything back, as RMB). Exit 1 if it can't open or the commit is refused (or writes nothing: a value that changes nothing). Other forms:

| Form | Meaning |
|---|---|
| `ring <x\|y\|z\|xx\|yy\|zz\|view> <deg>` | a gizmo ring drag of that many degrees (doubled: local axes) |
| `ball <dx> <dy>` | a trackball drag of that many px |
| `ctrl` (optional, after `ring` / `ball`) | CTRL held |
| `step <deg>` | the angle snap |
| `type <v>` | a typed value |
| `r90` / `r-90` | R / SHIFT+R |
| `reset` | ALT+SHIFT+R |
| `rot <y> <p> <r>` | the rotation set |
| `gizmo [hover <x\|y\|z\|view\|ball>]` | only draw the gizmo, that part hovered |

`r90`, `r-90`, `reset` and `rot` are applied at once (nothing with `cancel`).

**`--input-script <file>`.** With `--shot`, the run's keyboard and mouse come from this script from frame 10 on, through the same code paths as in the window (`src/input.c`: every module reads input through it; cursor show / hide / warp behave as raylib's GLFW platform). One step per line: `wait n`, `at x y`, `move dx dy [n]`, `press <key>`, `down <key>`, `up <key>`, `click left|right` (format: [input.md](input.md#the-script)). The interactive input handling runs as in the window (edit mode clicks, G / ALT+R / S with the mouse, CTRL+S); the shot is taken 10 frames after the last step. Exit 1 on a bad line.

**`--model-picker [filter]`.** With `--shot`, opens the edit mode and its model picker (I) with this filter typed (an optional value: the next argument unless it starts with `--`); `--shot-hud` draws it. The thumbnails render a few per frame, timed so that the last ones render in the shot's own frame (before its scene draw; from the first frame with `--reload-test` or `--settings-change`, so the reload or the renderer restart frees them and they render again); the shot waits until every listed row has one. Without `--shot-hud` the shot must equal a plain one (a thumbnail render changes nothing).

**`--model-picker-key <enter|close>`.** Once the thumbnails are rendered, presses ENTER (place the selected model at the crosshair's point, as at the cursor; the shot follows 20 frames later, so it must equal `--apply-edits` of the `add` the log prints) or ESC (closed; the shot is that frame). Exit 1 if ENTER is refused.

**`--classify "text"`.** Runs the launcher's model choice for this prompt and prints it (tier, model, classifier, classifier model, `classifier account` only with `MONOLITH_CLAUDE_FALLBACK`, reason, seconds, source), then exits without opening a window: the overrides, else the AI classifier (one `claude -p` on Fable, Opus as fallback; `launcher.c` "Model choice", `classifier.c`, [classifier.md](classifier.md)), run to its end. Context: the loaded map (its objects, `MAP.md`), the `--select` lines, the `--region` and `--volume` counts and a crosshair hit.

## Environment

Read by `launcher.c` ([launcher.md](launcher.md)), except the driver variables at the end; `MONOLITH_ALLOW_WINDOW` is also read by `src/main.c`.

| Variable | Meaning |
|---|---|
| `MONOLITH_LAUNCH` | `coordinator` (default: one coordinator session takes every request from a queue, [request-queue.md](../request-queue.md)) or `per-request` (a new terminal with its own Claude Code session per request, as before) |
| `MONOLITH_COORDINATOR_MODEL` | the coordinator's model (default sonnet) |
| `MONOLITH_CLAUDE_FALLBACK` / `MONOLITH_CLAUDE_ENV` / `MONOLITH_LIMIT_RETRY` | the usage-limit fallback accounts, the main account's assignments, the seconds an unknown reset counts ([command-line.md](../command-line.md), [request-queue.md](../request-queue.md) "Usage limit") |
| `MONOLITH_TERMINAL` | terminal to open for Claude Code: program name or path + optional args, e.g. `"kitty --single-instance"` |
| `TERMINAL` | used if `MONOLITH_TERMINAL` is unset; else the first of alacritty, kitty, foot, wezterm, xterm in `PATH`. In an AI agent's shell (`CLAUDECODE` / `AI_AGENT`) no terminal opens unless `MONOLITH_TERMINAL` is set (a stub) or `MONOLITH_ALLOW_WINDOW`: the request stays queued |
| `MONOLITH_CLAUDE_ARGS` | extra Claude Code flags, split on whitespace (default none), after `--allow-dangerously-skip-permissions --dangerously-skip-permissions --model <name>`, on the coordinator's (per-request: the request's) command line; a `--model` in them replaces the chosen model (both the coordinator's and the one recorded for each request) |
| `MONOLITH_MODEL` | one model for every request (an override: no classifier runs) |
| `MONOLITH_MODEL_FAST` / `_DEFAULT` / `_HEAVY` | model of each tier (default haiku / sonnet / opus). Without an override the AI classifier picks the tier per request (one `claude -p`, [request-queue.md](../request-queue.md) "Models"); a leading `@fast` / `@default` / `@heavy` (or `@haiku` / `@sonnet` / `@opus`) in the prompt picks it instead and is stripped from the bundle's prompt |
| `MONOLITH_CLASSIFIER` | the classifier program (default: the claude program below), or `off` (every request without an override runs on the default tier). In an AI agent's shell it is off unless this names a program (tests: `build/tools/stub-classifier`) |
| `MONOLITH_CLASSIFIER_MODEL` / `_FALLBACK` | its model and the fallback when that one is overloaded or not available (default fable / opus) |
| `MONOLITH_CLASSIFIER_TIMEOUT` | seconds before the classifier is stopped (default 30); its failure or timeout gives the default tier |
| `MONOLITH_CLAUDE_SAFE=1` | drop those two permission flags (also dropped when the working folder has no `CLAUDE.md`) |
| `MONOLITH_ALLOW_WINDOW=1` | open the editor window even in an AI agent's shell (`CLAUDECODE` or `AI_AGENT` set), where it otherwise refuses every mode that would show it |
| `MONOLITH_WORKSPACE` | i3/sway workspace for the terminal (default 10, empty = don't move) |
| `MONOLITH_CLOSE_ON_EXIT=1` | close the terminals of finished requests when the editor quits (coordinator mode: the coordinator, if none of this run's requests is still queued or running) |
| `MONOLITH_CLAUDE` | the claude program to run (tests: `build/tools/stub-coordinator`) |
| `MONOLITH_REQUESTS_DIR` | requests folder instead of `<repo>/requests` (tests only: bundles, queue and coordinator lock in a scratch folder) |
| `MONOLITH_CLOSE_METHOD` | `wm` / `signal`: close terminals only through i3/sway or only by signals (tests; default both, WM first) |
| `__GL_SYNC_TO_VBLANK=0` (NVIDIA) / `vblank_mode=0` (Mesa) | driver vsync off, for `--bench` |
