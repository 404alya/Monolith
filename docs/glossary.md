# Glossary

Terms used across these pages, in alphabetical order.

**AO volume, baked AO** (`--ao baked`): ambient occlusion computed once per map load into a 3D texture around the boxes, instead of per frame (SSAO / SSIL). Cheaper and stable when the camera turns; a quality trade-off used by the high and medium presets.

**Barrier:** an invisible wall, a `barrier x0 y0 z0 x1 y1 z1 [rot y p r]` line: it stops the player exactly like a box of that shape but is never drawn, casts no shadow and changes nothing in the look. Spectator mode shows the barriers as a red overlay (**O**); **H** in edit mode places one ([map-format.md](map-format.md) §2.1, [user-guide.md](user-guide.md#barriers-h-o)). The opposite of a `collide off` material (drawn, but not blocking).

**Brief, design brief:** a map's `MAP.md`: its style, scale, materials, lighting, rules for new content and named groups. Every request on the map is carried out in its style ([map-format.md](map-format.md) §1.2).

**Bundle:** the folder the editor writes for one request, `requests/<date>_<slug>/`: the prompt, screenshots, the camera, what the crosshair hit, the selection, the regions and the volumes, and later the result.

**Catalog, model catalog** (`library/library.db`): a SQLite database in the repo with one row per model from Poly Haven, Kenney, Polyfork and Poly Pizza: its links, licence, file hash, measured facts and three short descriptions (shape, style, use) with their embeddings. Request sessions search it with `build/tools/library search` instead of searching the web; the model files stay at their sources until a map uses one ([model-library.md](model-library.md)).

**Classifier, AI classifier:** a small one-shot Claude run (`claude -p` on Fable, Opus as fallback, no tools) the editor starts for each request without an override, before it queues the bundle: it reads the request and its context and picks the tier ([request-queue.md](request-queue.md) "Models"). Not the coordinator and not one of its subagents.

**Compiled map** (`<name>.mapb`): a map's text compiled by `monolith --compile` (and by `--export`) into fixed-size binary records. Games and the game-style examples load it; their runtime has no text parser. The `.map` text stays the source: edit it and compile again ([compiled-map.md](compiled-map.md)).

**Coordinator:** the one Claude Code session the editor opens in its own terminal. It takes requests from the queue and hands each to a subagent ([request-queue.md](request-queue.md)).

**Draw mode:** key B in spectator mode: paint on boxes and models with a brush, the colour picker and the draw panel. The first stroke on a face or model part gives it a rectangle in its material's paint atlas; strokes are undoable with the edits and saved with Ctrl+S ([painting.md](painting.md), [user-guide.md](user-guide.md)).

**Edit mode, manual edit:** key M in spectator mode: move, turn, remove, duplicate or add the selected objects yourself. Each edit is a minimal text edit to the map file (with undo / redo); no request is made ([user-guide.md](user-guide.md)).

**Entity, entity id:** every box, model instance, light and barrier of a loaded map, with its data (transform, source line, model file) attached as components. Its id stays the same across reloads: the line's `id n` (or a key from the line's text) plus the object's index within the line ([map-format.md](map-format.md) §2.9).

**Face groups** (`uv_faces`): which of a box's six faces share texture space. Faces with the same number share one cell of the material's texture; without the key every face shows the whole texture ([map-format.md](map-format.md)). In draw mode the faces of one `paint` entry share one rectangle of the paint atlas (the draw panel's Faces section; [painting.md](painting.md)).

**Fog cache** (`--fog-cache`): the volumetric fog's sun visibility baked once per load into a 3D texture, so the fog doesn't sample the shadow map every frame. A quality trade-off used by the high, medium and low presets.

**Frame reuse:** when the camera and everything else that affects the image stand still, the editor shows the last rendered frame again instead of rendering a new one.

**Group:** a named set of boxes, models, lights and barriers in a map (`group "clearing: camp"`). Requests and the HUD refer to things by group; Ctrl+click selects one.

**Group (in the editor):** the group tools (the groups panel, **F6**, the `group-*` edit-script commands, whole-group **R** / **Ctrl+D**) take a group by its **name**: every `group "<name>"` statement with that name, in any file of the map, is one group. An **area** is the part of a name before `": "` (`"plain: house"` is in area `plain`) ([user-guide.md](user-guide.md), [editor-tools.md](editor-tools.md) §3).

**Hot reload:** the editor watches the map package's files and reloads the map when one changes, keeping the old version loaded while a new one has errors.

**IBL, ambient map:** image-based lighting: the soft light from the sky that reaches every surface, computed from a (procedural) sky image.

**Import, part:** a map's text can be split over several files: the main `<name>.map` names the others with `import "parts/<name>.map"`, and each is read in place of its import line. An imported file is a *part*; it starts with its own `monolith N` line, with no `use` and no `group`, and may not hold `env`, `spawn` or `viewpoint` lines. Edits land in the file that holds the line, and the compiled map is still one `<name>.mapb`. Only the AI splits a map, with the edit script's `move-to-file` ([map-format.md](map-format.md) §2.10)

**Map package:** a folder `maps/<name>/` with the `.map` file, `MAP.md`, `ASSETS.md` and the map's shaders, models and textures. Paths inside it are relative, so it can be copied or exported as a whole.

**Paint atlas:** the image a material's paint lives in (`textures/paint/<material>.png`; a model part's own material: `textures/paint/<file>_s<N>.png`): each painted box face or model part has a rectangle there, named by a `paint` line of the map; drawn over the material by its paint stage ([painting.md](painting.md)).

**Patch N:** a numbered local change to R3D. Each is described, with the reason and the diff, in `submodules/r3d/patches.md`; features marked "patch N" don't work with an unpatched R3D.

**R3D:** the renderer monolith uses, a deferred PBR renderer on top of raylib, vendored in `submodules/r3d`.

**R3D pitfall N:** a numbered R3D behaviour found by debugging (fog in shadow turning black, clipped shadow casters, …), in `.claude/skills/lighting-fog-shadows/r3d-pitfalls.md`. Read it before changing lighting, fog or shadows.

**Regions, snip mode:** rectangles dragged over the screen (key C in spectator mode) to point at an area rather than an object.

**Rotate gizmo:** in edit mode, the rings drawn at the selection's pivot (X red, Y green, Z blue, a white view ring, a grey trackball disc inside), as in Blender: drag one to turn the selection about that axis ([user-guide.md](user-guide.md)).

**RRF, reciprocal rank fusion:** how the catalog search merges its rankings (one per description vector, one by keywords): each model scores the sum of 1 / (60 + its rank) over the lists it appears in, so a model ranked well by several lists comes first ([model-library.md](model-library.md) §6).

**Shadow tiles:** the sun's shadow map rendered for fixed tiles of the world and then only sampled, instead of every frame. The two tiles around the map's play area are drawn once per load; moving far away draws a new one about every 90 m. Which tile is used depends only on the camera position, so turning never re-renders them (`--shadow-camera` switches back to R3D's per-view shadow map).

**Shape:** an untextured primitive, a `shape <kind> cx y0 cz sx sy sz [rot y p r]` line: a cube, sphere, cylinder, cone, capsule or plane drawn inside the box a `block` with the same numbers makes, with its material, colliding as that (turned) box. Its paint regions are its *parts* (a cylinder's `side`, `top`, `bottom`). Added in the editor from the model picker (**I**) ([map-format.md](map-format.md) §2.1, [editor-tools.md](editor-tools.md) §2).

**Sketch:** freehand lines, arrows and circles the developer draws over the frozen frame in snip mode (**C**, then **D** for the pen), numbered S1, S2, …; sent with the request as `sketch.png`, drawn into `view-marked.png`, and in `request.json` with the world points each stroke passes over ([user-guide.md](user-guide.md)).

**Skill:** a Claude Code instruction file in `.claude/skills/<name>/SKILL.md`. Claude loads the matching one for its task (map requests, engine code, benchmarking, …).

**3D snip:** a volume marked with **X** in volume mode: the request gets four images (view, front, side, top) of only what is inside it, every surface cut at the volume's faces, besides the object list every volume has ([editor-tools.md](editor-tools.md) §5).

**Spawn, viewpoint:** where the walking player starts (feet position), and the named camera positions you fly between with ← / → and that `--view n` and `--bench` use.

**SSAO, SSIL, SSGI, SSR:** screen-space ambient occlusion (contact shadows in corners), indirect lighting (light bouncing off nearby surfaces), global illumination and reflections. Computed per frame from what is on screen.

**Subagent:** a helper Claude Code agent the coordinator starts for one request. It edits the map, checks the result with screenshots and writes `result.md`.

**Task lists:** the repo's two lists of open bugs and future work, one entry per line, titles starting `bug:`, `task:` or `idea:`: the maintainer's `TODOs` (`build/tools/todo`) and the AI's `.claude/TODOs` (`build/tools/ai-todo`), where Claude Code sessions note what they found or deferred ([CONTRIBUTING.md](../CONTRIBUTING.md#bugs-and-tasks)).

**Tier:** the size of Claude model a request gets (fast, default, heavy), chosen by the AI classifier; `@fast` / `@default` / `@heavy` at the start of a prompt overrides it.

**Turned box:** a `box` or `block` line with `rot yaw pitch roll`: the box turned about its own centre. It draws, collides and is picked as its turned shape; the runtime keeps its world AABB in `boxes[]` and the shape in `turnedBoxes` ([map-format.md](map-format.md) §2).

**Volumes, volume mode:** axis-aligned boxes marked in world space (key V in spectator mode, or Shift+V from the selection) to point at a space rather than at what the screen shows: a room behind a wall, an empty spot to fill. Sent with requests as V1, V2, ...; never saved into the map.

**Volumetric fog:** fog that is lit by the sun and lights, so light shafts show through gaps. Distance fog is the plain haze that fades far geometry.

**Work plane:** the horizontal plane a volume corner, barrier click or new shape lands on when nothing is under the cursor (the sky) or **Alt** is held: at the first corner's height, else the last volume's base or the camera's feet; **PgUp / PgDn** move it by the snap step ([user-guide.md](user-guide.md), [editor-tools.md](editor-tools.md) §1).
