# Glossary

Terms used across these pages, in alphabetical order.

**AO volume, baked AO** (`--ao baked`): ambient occlusion computed once per map load into a 3D texture around the boxes, instead of per frame (SSAO / SSIL). Cheaper and stable when the camera turns; a quality trade-off used by the high and medium presets.

**Brief, design brief:** a map's `MAP.md`: its style, scale, materials, lighting, rules for new content and named groups. Every request on the map is carried out in its style ([map-format.md](map-format.md) §1.2).

**Bundle:** the folder the game writes for one request, `requests/<date>_<slug>/`: the prompt, screenshots, the camera, what the crosshair hit, the selection, the regions and the volumes, and later the result.

**Compiled map** (`<name>.mapb`): a map's text compiled by `monolith --compile` (and by `--export`) into fixed-size binary records. Games and the game-style examples load it; their runtime has no text parser. The `.map` text stays the source: edit it and compile again ([compiled-map.md](compiled-map.md)).

**Coordinator:** the one Claude Code session the game opens in its own terminal. It takes requests from the queue and hands each to a subagent ([request-queue.md](request-queue.md)).

**Edit mode, manual edit:** key M in spectator mode: move, turn, remove, duplicate or add the selected objects yourself. Each edit is a minimal text edit to the map file (with undo / redo); no request is made ([user-guide.md](user-guide.md)).

**Entity, entity id:** every box, model instance and light of a loaded map, with its data (transform, source line, model file) attached as components. Its id stays the same across reloads: the line's `id n` (or a key from the line's text) plus the object's index within the line ([map-format.md](map-format.md) §2.9).

**Fog cache** (`--fog-cache`): the volumetric fog's sun visibility baked once per load into a 3D texture, so the fog doesn't sample the shadow map every frame. A quality trade-off used by the high, medium and low presets.

**Frame reuse:** when the camera and everything else that affects the image stand still, the game shows the last rendered frame again instead of rendering a new one.

**Group:** a named set of boxes, models and lights in a map (`group "clearing: camp"`). Requests and the HUD refer to things by group; Ctrl+click selects one.

**Hot reload:** the game watches the map package's files and reloads the map when one changes, keeping the old version loaded while a new one has errors.

**IBL, ambient map:** image-based lighting: the soft light from the sky that reaches every surface, computed from a (procedural) sky image.

**Map package:** a folder `maps/<name>/` with the `.map` file, `MAP.md`, `ASSETS.md` and the map's shaders, models and textures. Paths inside it are relative, so it can be copied or exported as a whole.

**Patch N:** a numbered local change to R3D. Each is described, with the reason and the diff, in `submodules/r3d/patches.md`; features marked "patch N" don't work with an unpatched R3D.

**R3D:** the renderer monolith uses, a deferred PBR renderer on top of raylib, vendored in `submodules/r3d`.

**R3D pitfall N:** a numbered R3D behaviour found by debugging (fog in shadow turning black, clipped shadow casters, …), in `.claude/skills/lighting-fog-shadows/r3d-pitfalls.md`. Read it before changing lighting, fog or shadows.

**Regions, snip mode:** rectangles dragged over the screen (key C in spectator mode) to point at an area rather than an object.

**Volumes, volume mode:** axis-aligned boxes marked in world space (key V in spectator mode, or Shift+V from the selection) to point at a space rather than at what the screen shows: a room behind a wall, an empty spot to fill. Sent with requests as V1, V2, ...; never saved into the map.

**Rotate gizmo:** in edit mode, the rings drawn at the selection's pivot (X red, Y green, Z blue, a white view ring, a grey trackball disc inside), as in Blender: drag one to turn the selection about that axis ([user-guide.md](user-guide.md)).

**Shadow tiles:** the sun's shadow map rendered for fixed tiles of the world and then only sampled, instead of every frame. The two tiles around the map's play area are drawn once per load; moving far away draws a new one about every 90 m. Which tile is used depends only on the camera position, so turning never re-renders them (`--shadow-camera` switches back to R3D's per-view shadow map).

**Skill:** a Claude Code instruction file in `.claude/skills/<name>/SKILL.md`. Claude loads the matching one for its task (map requests, engine code, benchmarking, …).

**Spawn, viewpoint:** where the walking player starts (feet position), and the named camera positions you fly between with ← / → and that `--view n` and `--bench` use.

**SSAO, SSIL, SSGI, SSR:** screen-space ambient occlusion (contact shadows in corners), indirect lighting (light bouncing off nearby surfaces), global illumination and reflections. Computed per frame from what is on screen.

**Subagent:** a helper Claude Code agent the coordinator starts for one request. It edits the map, checks the result with screenshots and writes `result.md`.

**Tier:** the size of Claude model a request gets (fast, default, heavy), chosen from its wording; `@fast` / `@default` / `@heavy` at the start of a prompt overrides it.

**Turned box:** a `box` or `block` line with `rot yaw pitch roll`: the box turned about its own centre. It draws, collides and is picked as its turned shape; the runtime keeps its world AABB in `boxes[]` and the shape in `turnedBoxes` ([map-format.md](map-format.md) §2).

**Volumetric fog:** fog that is lit by the sun and lights, so light shafts show through gaps. Distance fog is the plain haze that fades far geometry.
