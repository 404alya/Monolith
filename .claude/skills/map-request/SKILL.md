---
name: map-request
description: Carry out one in-game map request from the monolith map designer — a bundle in requests/<id>/ (request.md, request.json, view.png, view-marked.png, region-<n>.png), usually as a subagent of the request coordinator. Use whenever a prompt mentions requests/<id>/request.md, a map edit request, a request bundle, result.md, question.md, DONE/FAILED/QUESTION/NEEDS-CODE replies, or an old prompt naming AI-MAP-TOOL.md. Import bundles (kind "import") also need the import-request skill.
---

# Map request

The workflow for the session that carries out one request (`requests/<id>/request.md`): normally a **subagent of the request coordinator** (the `coordinator` skill), or, in per-request mode, a Claude Code session in its own terminal. How the request reaches you, how to find what the developer means, and what to report. The guard rails in `CLAUDE.md` hold without exception: you run without approval prompts.

| Read | When |
|---|---|
| the map's `MAP.md` | every request: the map's own style, scale, materials, lighting, rules, groups |
| `docs/map-format.md` | every edit: package layout, the design brief's sections, the `.map` format |
| `.claude/skills/map-request/bundle-reference.md` | every request: what each file and field of the bundle means |
| the `map-editing` skill | every edit: coordinates and scale, editing rules, verification, hot reload |
| the `assets-and-licences` skill | the request needs a model or a texture |
| the `import-request` skill | `kind: "import"` bundles |
| the `engine-code` skill (+ `lighting-fog-shadows` for lighting, fog, shadows) | only for engine changes (C, R3D, shaders beyond the package), and under the coordinator only once you run alone (§2a) |

## 1. What this is

monolith is an **AI-assisted map designer** for 3D levels of any style, built on raylib + R3D: a brutalist concrete complex, a foggy low-poly pine forest at dusk, a neon market street, a medieval village. The look of a map is data: its boxes, models, materials, lights and environment, and its design brief.

- **The game** (`build/linux/monolith`) is the editor and viewer. The developer flies through the level in spectator mode, points the crosshair at a spot, optionally selects objects or drags out screen regions, types a request ("make this wall twice as tall", "plant three more pines along the path", "hang a neon sign over each of these stalls") and submits it.
- **The level** is a **map package** `maps/<name>/` (`docs/map-format.md` §1): a plain-text map file `<name>.map` (axis-aligned boxes, placed models, materials, lights, environment, spawn, viewpoints), the design brief `MAP.md`, and the package's shaders, models, textures and `ASSETS.md`. `--map <package or .map file>` picks the map (without it the game loads its default map, `docs/command-line.md`).
- **Claude Code** carries the requests out. The game queues each request; one long-lived **coordinator** session (started by the game in its own terminal; the `coordinator` skill) decides what runs when and hands each request to a **subagent**. The subagent edits the package (usually only the map file) in the real repo. The running game hot-reloads the package, so the change shows up in place without a restart. (`MONOLITH_LAUNCH=per-request` instead opens one terminal with its own session per request, the behaviour before 2026-10-24.)

You are the session doing one request. Your job is to change the map (rarely the engine) so it does what the user asked and fits the map's brief, check the result visually, and report back.

## 2. Request workflow

1. In spectator mode the developer aims, selects, optionally drags out screen regions (snip mode, **C**) and presses **Enter**, types the request and presses **Enter** again.
2. The game writes a bundle folder, **`requests/<YYYY-MM-DD_HHMMSS>_<slug>/`**, in the repo root (`requests/` is gitignored). The slug is made from the prompt's first words.
3. The game writes `state` (`queued`) as the bundle's last file and, if no coordinator is running, opens one terminal for it (moved to i3/sway workspace 10, `MONOLITH_WORKSPACE`). The coordinator claims the request when its rules allow (requests on the same map package never run at the same time; code requests run alone) and starts you as its subagent. The user's text is never put on a command line. It is only in the bundle. Everything runs **without approval prompts** (`--allow-dangerously-skip-permissions --dangerously-skip-permissions`, unless `MONOLITH_CLAUDE_SAFE=1`), so the guard rails in `CLAUDE.md` apply. The game also chose **your model** for this request (fast / default / heavy tier, from the prompt and its context; the `Model:` line near the top of `request.md` says which and why). If the task turns out much bigger than your tier suggests, still do it carefully and completely (you can't switch models), and say so in `result.md`. *Per-request mode:* the game opens a terminal running `claude "Map edit request: read requests/<id>/request.md and carry it out."` in the repo root instead.
4. You:
   1. read `request.md`, and `.claude/skills/map-request/bundle-reference.md` for what its sections and fields mean (object indices shift, the `Warning` line, copy indices, regions, format-1 bundles);
   2. **read the map's `MAP.md`** (the *Design brief* section of `request.md` names it; format-1 bundles don't: then it is `MAP.md` next to the map file). It decides what new content looks like and how it is built. If it is missing, empty or still the unfilled `--new-map` template (placeholders in parentheses), **write it first** (`docs/map-format.md` §1.2), then build;
   3. **read `docs/map-format.md`** (the map format: you need it for every edit; skip it only if you already read it in this session), and load the **`map-editing`** skill (`.claude/skills/map-editing/SKILL.md`: coordinates and scale, editing rules, verification); load **`assets-and-licences`** (`.claude/skills/assets-and-licences/SKILL.md`) too if the request needs a model or a texture;
   4. look at the images, locate the target (§3), edit the map and verify the edit (`map-editing`), and write **`requests/<id>/result.md`** (§4).
5. The game polls the bundle twice a second. Its HUD shows the request as `queued`, `running`, `[?]` (a question waits for the developer) and, when `result.md` appears, `[done] <slug>: <first line of result.md>` (or `[failed]` / `[cancelled]`). The map change is already visible through hot reload. **R** flies the developer's camera back to where the request was made.

Under the coordinator no other request edits your map package while you work, and code requests run alone. Still check the map hash first (`map-editing`, verification step 1): the developer may have edited the map by hand, an earlier request may have changed it since the submit, and in per-request mode several sessions can edit the same map at once.

### 2a. Working under the coordinator

- **You have no terminal**, and the developer doesn't see your messages; the coordinator relays. Your **final reply is one line**: `DONE: <line 1 of result.md>`, `FAILED: <line 1 of result.md>`, `QUESTION`, or `NEEDS-CODE: <why>`. Everything else goes into `result.md`.
- **Questions** (§3, step 6): write the question to `requests/<id>/question.md` (line 1: the question, under 150 characters, plain ASCII; details below it) and reply `QUESTION`. Change nothing before you ask if you can help it. You are resumed with the developer's answer; then carry on.
- **Code**: unless your prompt says you run alone, don't change code (C, the runtime, R3D, shaders outside the map package, build files, tools). If the request needs it, stop **before** changing any code and reply `NEEDS-CODE: <one line why>`. The coordinator resumes you once no other request runs; then code changes are allowed (the `engine-code` skill; afterwards the `leak-check` skill, `map-editing` verification step 7).
- **Don't touch the queue**: never write `state`, `claim.txt` or `cancel`, never run `tools/request-queue.sh`, and never touch other bundles. Write only into your own bundle (`result.md`, `question.md`, images, dumps).
- A cancel from the developer stops you mid-work; the coordinator then lists in `result.md` which files had changed. That is one more reason to write each map edit in one go (`map-editing`, verification step 3).

**Bundle files** (every field: `.claude/skills/map-request/bundle-reference.md`): `request.md` (read first), `request.json` (the same, machine-readable), `view.png` (the frame without the HUD), `view-marked.png` (selection in orange, crosshair hit as a cyan square, regions in magenta with their numbers), `region-<n>.png` (one crop per screen region), `result.md` (yours, last), `question.md` (yours, if you ask), and the queue's files `state`, `claim.txt` and `cancel`, which you never write. **Format-1 bundles** (no `Model:` line; `request.json` `"format": 1`) predate map packages: their line numbers and box indices are stale and their map path is old; `bundle-reference.md` (next to this file) says how to read them. **Bundles written before 2026-10-24** say to read `AI-MAP-TOOL.md`: that file is now this skill.

## 3. Locating what the user means

Work through these sources in this order:

1. **Crosshair hit and selection.** Together they are the strongest signal. "This", "it" and "here" usually mean the hit object or the hit point. When objects are selected, "these" or "this" means the selection. Many requests are about a **spot** rather than an object ("put a door here", "plant a tree here", "open a slit above this"): use the hit point and the face normal. The new content goes on that face, on the side the normal points to.
2. **Directions relative to the camera.** With yaw *y* and pitch *p*:
   - forward = `(cos p · sin y, sin p, cos p · cos y)`. Yaw 0 looks toward +Z, yaw 90 toward +X, yaw 180 toward −Z.
   - camera right = `(−cos y, 0, sin y)` and up = +Y.
   - "Left" is −right, "behind" is −forward (horizontally: the forward vector with its y set to 0), "in front of / beyond" is +forward. "Higher" is +Y. `request.md` gives both vectors already computed.
3. **Group names, landmarks and nearby context.** "The hall roof", "the canyon walls", "the camp" or "the ridge" refer to groups (`docs/map-format.md` §2.3); the map's `MAP.md` lists its groups and landmarks. The Nearby table tells you what surrounds the spot.
4. **Screen regions** (format-2 bundles; older bundles have none). The developer dragged a rectangle over the frame, usually to point at an *area* rather than one object: "this area", "everything here", "fill this gap", "make this part darker". Read the region's crop (`region-<n>.png`) and its numbered outline in `view-marked.png`. "Everything here" means the region's object list (read the *by map line* table first; ignore tiny shares, a few percent or less, unless they are what the request is about). "Fill this gap" / "put something here" is about the empty part: the sky share and the depth range tell you whether there is open space or distant geometry behind it. **A gap is bounded by the geometry at the region's edges, not by what is seen through it**: when the region looks through an opening (a canyon mouth, a doorway, the space between two buildings), its centre point and objects lie behind the opening, often far away; the **edge objects** (nearest first, with their sides) are the walls, floor and lintel that frame it, and the place to build is between them, at their distance. When the region shows a surface instead (a wall to "make darker", ground to "plant here"), the centre point + normal is the anchor. To probe more points of the frame, `--pick-at x y` (with the bundle's reproduction command) prints what the ray through any pixel of `view.png` hits. Combine with the selection and the crosshair hit when they are given too; when several regions are numbered in the prompt ("1", "2"), match them to the outlines.
5. **The images.** Look at `view-marked.png` (Read it as an image) to see what the user saw and what was marked. `view.png` shows the same frame without the marks.
6. **If it is still ambiguous**, for example "this" when there is no hit and no selection, or two plausible targets, **ask the developer**. Don't guess. Under the coordinator: `question.md` + reply `QUESTION` (§2a); the coordinator asks in its terminal, which the developer watches, and resumes you with the answer. In per-request mode: ask in your terminal.

## 4. result.md

Write it **last**, in one go. The game picks it up as soon as it has a complete first line.

- **Line 1: a one-line summary.** It appears in the game's HUD as `[done] <slug>: <line 1>`. Leading `#` and spaces are stripped and the line is cut at 159 characters. Keep it short (< 100 characters), plain ASCII (other characters show as `?`), and specific, e.g. `Pillar at (9, -5) raised from 20 m to 25 m (line 193 split into 3 lines)` or `3 pines added along the path, restyled needles/bark`.
- Then:
  - **what changed**: map lines before → after (quote them, with line numbers), and `MAP.md` / `ASSETS.md` changes;
  - the **paths of the before/after images**;
  - **assets**: every downloaded file with its source and licence (or why none was acceptable);
  - **anything left open**: assumptions, departures from the brief, things you couldn't do, side effects;
  - **"restart needed"** if you changed code.
- If you couldn't do the request, still write `result.md`, with a summary line that says so (e.g. `Not done: the developer chose to skip it` or `Not done: no model with an acceptable licence`). Otherwise the HUD shows the request as running until the coordinator gives up on it.

## 5. Import requests, models and textures

An import request (`monolith --import <project dir> <map name or path>`, bundle `kind: "import"`) brings the scene of an existing raylib + R3D project into a new map package: follow the **`import-request`** skill. Before placing any model or texture, load the **`assets-and-licences`** skill. Never place a file whose licence isn't recorded.
