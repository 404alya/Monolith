# `runtime/monolith_scene.c` and `monolith_scene.h`

The scene runtime: it loads, draws, collides with and picks a monolith map package in any raylib + R3D program. It is C11 and depends only on raylib (raymath, rlgl) and R3D.

This page is the reference for the `.map` text format. Where [map-format.md](../map-format.md) and this page differ, this page is authoritative (it took over that role from the old top-of-file comment of `monolith_scene.c`); where this page and the code differ, the code decides. It is part of the monolith repository only: `--export` copies `monolith_scene.c` and `.h` into every export, but not this page.

**Editor-only parts.** The sections marked *(editor builds only)* below describe code inside `#ifdef MONOLITH_EDITOR` blocks: the map text's parser (the whole [Map format reference](#map-format-reference-map-editor-builds-only)), the compiler and the other editor sections. An exported game, and any program built without `-DMONOLITH_EDITOR`, has only the loader of the compiled form and what it needs to draw, collide and pick.

## Overview

monolith draws its maps with this same module, so a scene looks the same there and in your game.

A program reads a map's **compiled form**, `<name>.mapb`: the map's text compiled by monolith into fixed-size records ([compiled-map.md](../compiled-map.md); the region "Compiled maps" of `monolith_scene.c`). The runtime checks the records completely before it uses them. monolith writes the compiled form from the map's text ([map-format.md](../map-format.md)).

**Patched or upstream R3D.** Compile with `-DMS_PATCHED_R3D` when building against monolith's patched R3D (see its [patches.md](../../submodules/r3d/patches.md)). Without it the runtime uses upstream R3D APIs only: a camera-fitted sun shadow, no depth pre-pass toggle and no fog cache.

### Editor builds

Built with `-DMONOLITH_EDITOR` (the monolith editor, its tools and the examples), the module also has:

- the map text's parser (region "Parser" of `monolith_scene.c`; the format: [Map format reference](#map-format-reference-map-editor-builds-only) below),
- the compiler, which writes the compiled form,
- hot reload,
- the map's source lines,
- the package and environment writers.

These are the sections marked "editor" in the source. `--export` writes the module without them.

## Minimal use

```c
InitWindow(w, h, "game");
// optional: the map's shadow map size is an R3D init hint, so peek at it first
MS_Scene *peek = MS_Load("maps/forest", &(MS_LoadOptions){ .noGpu = true });
if (peek) { MS_SetInitHints(peek); MS_Unload(peek); }
R3D_Init(w, h);
MS_InitRenderer();                 // GL state the scene needs (depth clamp for the sun shadow)
MS_Scene *scene = MS_Load("maps/forest", NULL);
if (!scene) { puts(MS_LastError()); MS_ClearError(); return 1; }
MS_ApplyEnvironment(scene);
while (!WindowShouldClose()) {
    BeginDrawing();
        MS_Draw(scene, camera, (RenderTexture){ 0 });   // R3D_Begin .. R3D_End
    EndDrawing();
}
MS_Unload(scene);
MS_ClearError();                   // frees the last error message, if any
```

### Inside your own R3D frame

To draw your own objects and the scene together, in your own R3D frame instead:

```c
R3D_Begin(camera);
    MS_PushLights(scene, camera, aspect);
    MS_DrawGeometry(scene);
    ... your R3D_Draw* calls ...
R3D_End();
```

### GL errors and debug output

Neither the runtime nor R3D checks GL errors per call, so a call that a stricter driver rejects would leave nothing in the log. Three checks write the answer to the log (`MS_LogGLErrors`, a `glGetError` loop: `monolith_scene: GL errors <where>: none`, or one `GL error <where>: GL_INVALID_OPERATION (0x0502)` line per kind of error):

- `MS_InitRenderer` logs the window framebuffer's sample count (`window framebuffer: 4x multisampled` / `single-sampled`) and the errors left by opening the window and `R3D_Init`;
- `MS_PushLights` logs the errors of the load and the first two frames, once per load, after the `first frames after the load` line;
- a program can call `MS_LogGLErrors(level, where)` itself (the exported viewer does after `InitWindow` and, with `--gl-debug`, every 10 s).

`MS_BeginGLDebugOutput(getProc)` (opt-in) sends the driver's `GL_KHR_debug` (else `GL_ARB_debug_output`) messages to `TraceLog`, synchronously, without notifications, each message id at most 3 times (`monolith_scene: GL debug: error, high, api, id 1282: …`); `getProc` is the window library's GL loader (`glfwGetProcAddress`), because the runtime loads no GL functions of its own. It returns 0 (KHR), 1 (ARB) or 2 (neither, logged). raylib opens a non-debug context, where a driver may report less. `MS_EndGLDebugOutput` unhooks it and frees the list of seen ids (before `CloseWindow`).

What a clean run shows today: no GL errors, plus two `GL_INVALID_VALUE` from `glDeleteProgram` at `MS_Unload` with debug output on (R3D deletes a surface shader's program through every view of a union, `r3d_shader.c` `r3d_shader_custom_free`; harmless, in the AI's TODO list), and on NVIDIA performance notes and an "undefined behaviour" note about the spot / omni shadow samplers bound to texture 0 on maps without such lights.

### Ownership

- Scene contents are read-only for callers.
- Everything the runtime owns (GPU resources, parsed data, caches) is freed by `MS_Unload`.
- One scene can be drawn at a time per R3D context: R3D's environment and fog bake are global.

## Map format reference (.map)

*Editor builds only: the text parser is not part of a game build or an export.*

The parser reads one statement per line.

| Statement | Meaning |
|---|---|
| `# comment` | `#` starts a comment anywhere outside a `"string"`. |
| `monolith 3` | The format version: the first statement of the file, required. The parser reads exactly `MS_FORMAT_VERSION`; a file without this line is format 1. |
| `group "name"` | The following primitives (boxes, lights, models) belong to this group, until the next `group`. Names can be any length but can't contain `"`. |

### Primitives

Units are metres; +Y is up. Numbers are plain decimals, e.g. `12`, `-4.5`, `0.75`.

| Primitive | Arguments | Makes |
|---|---|---|
| `box` | `x0 y0 z0 x1 y1 z1 [rot y p r]` | A box between two opposite corners, given in any order. |
| `block` | `cx y0 cz sx sy sz [rot y p r]` | A box centred on (cx, cz), from y0 up to y0 + sy. |
| `stairs` | `cx baseY startZ width steps rise run` | `steps` (an integer) solid steps climbing toward −Z from startZ, each down to baseY. |
| `waffle` | `x0 x1 y0 y1 z depth bayW floorH` | A facade: a back wall from z−1 to z, vertical fins every bayW along x and ledges every floorH, both sticking out from z to z + depth. |
| `light` | see [Lights](#lights) | A point, spot or extra directional light. |
| `model` | see [Models](#models) | A model file placed at x y z. |

#### Turned boxes: `rot yawDeg pitchDeg rollDeg`

- Turns the box as written about its own centre, with the models' convention (R = Ry · Rx · Rz).
- `rot` is never shifted: every `repeat` copy turns about its own centre.
- `rot 0 0 0` is the box without `rot`.
- A turned box draws, collides and picks as its turned shape. Its `boxes[]` / `colliders[]` entry is its world AABB (`MS_Scene.turnedBoxes`, `boxTurned`).

#### Barriers: `barrier x0 y0 z0 x1 y1 z1 [rot y p r]`

An invisible wall (user-facing: `docs/map-format.md` §2.1, *Barriers*): it blocks the player exactly like a `box` of the same corners and turn, and nothing else sees it. It is a statement with its own object kind, not a material key on boxes, so that `boxes[]`, box indices, the draw ranges, `--dump-boxes` and every cache stay as they are (the developer's call, 2026-11-27).

- **Record.** `MS_Barrier` (`scene->barriers`, `barrierCount`): its world AABB `box`, `turned`, the exact shape `shape` (an `MS_TurnedBox`, filled for every barrier: an axis-aligned one has rotation 0 and the world's axes, so collision code needs no lookup table) and its `MS_Ref` (line, file, statement, group, repeat copy; material: the `use` active there, never used; part 0). Parsed by `ParseBarrier` (`id` / `repeat` / `group` / imports as for `box`; `rot` is never shifted), with the box's float math (`TurnedBoxMake`), so a barrier and a box written alike have bit-identical shapes. `SceneData.barrierWritten` keeps each one as written (corners ordered) for the compiled map, as `turnedWritten` does for boxes.
- **Colliders.** `colliders[] = boxes, model colliders, barriers`: box indices keep matching `boxes[]` and model colliders keep theirs, so `src/player.c`'s `colliders[hit]` keeps working. `scene->barrierColliderFirst` (= `colliderCount - barrierCount`) is barrier 0's collider; a collider index `c >= barrierColliderFirst` is barrier `c - barrierColliderFirst`. With barriers and no models, `colliders` is its own array (boxes copied, then the barriers), no longer `boxes` itself. The collider BVH covers them; `MS_FirstCollision` / `MS_Collides` test a barrier's exact shape (`OverlapsTurned` when turned), `MS_ColliderTop` / `MS_ColliderBottom` give a turned barrier's span (`TurnedSpanY`) and walkability, as for turned boxes. `collide off` doesn't apply (a barrier has no material).
- **Not drawn, not in any cache.** Barriers are never in `boxes[]`, the draw ranges, `UploadBoxes`, the shadow tiles, the light shadows, the fog cache, the AO volume (all of them read boxes and models only) or `scene->bounds` (`FinishGeometry` leaves them out, so the regions the caches cover don't move). A live edit (`MS_EditSetText`) that changes only barrier lines is no full upload (they are object statements, not in the look hash) and marks nothing dirty: the caches are restamped and kept. The edit preview moves a barrier's record, collider and entity transform and refits the collider BVH, with nothing dirty either.
- **Object kind** `MS_OBJ_BARRIER` (appended, so the other kinds keep their numbers). Everything that switches over `MS_ObjKind` learns it: in the runtime `BuildEntities`, `MS_ObjectRef` / `MS_ObjectBounds` / `MS_ObjectEntity`, `MS_EditPreviewBegin` / `Set` / `End`, `Publish` (`barrierEntities`); in the editor `src/mapedit.c` (`KindName`, the edit classes `EDIT_CLASS_BARRIER`, `TakeSnapshot`, the anchor of a selection), `src/designer.c` (`KindCount` / `KindRef`, the selection tables, pointing), `src/launcher.c` (`ObjKindName`, the bundle's `request.json` / `request.md`), `src/editmode.c` (the HUD's kind name), `src/idpanel.c`, `src/transform.c` (the gizmo's single-object frame) and `src/dumpscene.c` (`--dump-entities`). `src/drawmode.c` (painting) and `src/inspector.c` (draw ranges) never see one: their picks go through `MS_Raycast`.
- **Picking.** `MS_Raycast` skips barriers (a ray hits what is visible: pointing, painting, a game's line of sight); `MS_RaycastBarriers` tests the barriers alone (exact shape, a barrier containing the origin skipped), which the editor's overlay uses to pick them while it is on.
- **Entities.** `entityCount = boxCount + modelCount + lightCount + barrierCount`; `objEntities` holds boxes, models, lights, then barriers. A barrier's implicit id is its statement's key like any other line's (its text starts with `barrier`, so it never equals a box line's and existing ids don't shift); its transform is its shape's centre, rotation and size.
- **Compiled map.** Section 21 `barriers` (`CmBarrier`: the corners as written, the rotation, the `CmRef`), `CM_VERSION` 14 (`docs/compiled-map.md`).
- **Limits.** Barriers count toward `MS_MAX_OBJECTS` with boxes, models and lights.

### Lights

```
light point x y z [key value ...]            point light (R3D omni light)
light spot  x y z dx dy dz [key value ...]   spot light shining along (dx, dy, dz)
light dir   dx dy dz [key value ...]         extra directional light (no static tiles)
```

| Key | Meaning | Default |
|---|---|---|
| `color R G B` | colour | white |
| `energy f` | | 1 |
| `range f` | | 16 (dir: 50, its shadow distance) |
| `falloff f` | | 1 |
| `specular f` | | 1 |
| `fogEnergy f` | | 1 |
| `inner deg`, `outer deg` | the spot cone | 22.5 / 45 |
| `shadow on\|off` | | off |
| `depthBias f`, `slopeBias f` | The shadow map's constant and slope-scaled biases (`R3D_ShadowMap`; metres for a point light), >= 0; set on the map when `LoadLightShadow` loads it, part of `LightSame`, printed by `--dump-scene` only when set | R3D's default for the type |
| `clip x0 y0 z0 x1 y1 z1` | A world box, shifted like the position. The light reaches only points inside it, so an unshadowed light stays out of the room below or beside it. R3D patch 16; ignored with upstream R3D. | none |

### Models

```
model "models/x.glb" x y z [key value ...]
```

A model file placed at x y z. glTF/GLB works best; OBJ, FBX and DAE also load, through R3D's importer.

| Key | Meaning |
|---|---|
| `rot yawDeg pitchDeg rollDeg` | Yaw about +Y, pitch about +X, roll about +Z, applied roll, then pitch, then yaw (R = Ry · Rx · Rz). Yaw 90 turns the model's +Z to +X (like the camera's yaw). |
| `scale s` \| `scale sx sy sz` | Uniform or per-axis, > 0. Files have no unit scale: a model in cm needs `scale 0.01`. Lighting of per-axis scaled models is approximate (R3D rotates instance normals but does not re-scale them). |
| `origin model\|bottom\|center` | `model` (default): the file's origin goes to x y z. `bottom`: the model stands on y (its lowest vertex after scale and rotation), centred over x z (the centre of the file's bounds, turned with it); the safe way to stand a model on a surface. `center`: the same x z, with the middle between its lowest and highest vertex at y. |
| `material "name"` | Every material slot is drawn with this map material. |
| `material.N "name"` | Slot N only (0-based, in the file's material order). |
| `collide aabb\|none\|box x0 y0 z0 x1 y1 z1 [box ...]` | `aabb` (default): the world bounds (of the turned vertices) block the player. `none`. Or any number of boxes in model space (before scale and rotation), each turned into a world AABB (e.g. a tree's trunk only). |
| `shadow on\|off` | Casts the sun shadow (default on); a model always receives shadows. |

**Material precedence:** `material.N`, then `material`, then the active `use` (when it is not `"default"`), else the file's own. An override material with `inherit albedo|normal|orm|emission|all` keeps that texture of the slot's own material (`inherit emission` also keeps its colour and energy, `inherit normal` its scale); the override's colours and factors apply.

**Loading and drawing:**

- Each file is loaded once per map load (`R3D_LoadModel`) and drawn instanced per (file, materials, shadow).
- The file's own materials keep its choices, also `KHR_materials_unlit` (unlit: no light or shadow on it, e.g. Kenney's kits). Override it with a lit material to light it.
- Animations and skins are ignored (static pose).
- The files a `.gltf`/`.glb` names (buffers, images) and an `.obj`'s material libraries and textures must be inside the package.

### Placement prefixes

Written before a primitive on the same line.

| Prefix | Meaning |
|---|---|
| `id n` | A stable name for the line's objects: an integer 1 … 2147483647, unique in the file, written first (`id 12 repeat 4 0 0 3 box ...`). It changes nothing in the scene; it keys the entities (`MS_EntityId`), so they keep their identity when the line itself changes. |
| `repeat n dx dy dz [from k]` | n copies; copy i (0-based) is shifted by i · (dx, dy, dz). Up to 3 repeats on one line give a grid; the first is the outer loop, so it decides the box order. With `from k` the copies are i = k … k+n−1 instead: the same floats as copies k… of the longer row, so a row split in two keeps its exact positions. |

**What shifts.** Shifts apply to the position arguments only, never to sizes or counts:

| Statement | Shifted arguments |
|---|---|
| `box` | all six |
| `block` | `cx y0 cz` |
| `stairs` | `cx baseY startZ` |
| `waffle` | `x0 x1 y0 y1 z` |
| `light`, `model` | `x y z`, never a direction |

**Float order.** Each position is computed as `(i*d1 + j*d2 + k*d3) + value`, in float. With whole or half numbers the order doesn't matter; with values like 2.6 it can change the last bit of a float (`(-36 + 4.5) + 2.6` vs `-36 + (4.5 + 2.6)`). `from` gives the copies of a row split in two the same floats as the longer row's.

### Materials

```
material "name" key value [key value ...]
use "name"
```

- `material` defines a material, or adds keys to one: a long definition may span several lines with the same name.
- `"default"` always exists (plain PBR: albedo 180 180 180, roughness 0.9, metalness 0) and may be redefined.
- `use "name"`: the following boxes (and models) use this material, like `group`. `use "default"` goes back. The material must be defined before the `use`.
- Colours are 0–255 sRGB. Unset keys keep R3D's defaults (`R3D_MATERIAL_BASE`).

| Key | Meaning |
|---|---|
| `shader "shaders/x.glsl"` | An R3D surface shader, compiled once per file; each material draws with its own alias of it (its own uniforms and samplers). |
| `uniform <name> f [f f f]` | A float / vec2 / vec3 / vec4 uniform of that shader. |
| `sampler <name> "textures/x.png"` | A `sampler2D` uniform of that shader. |
| `data_sampler <name> "textures/x.png"` | The same for a texture of data, not colour (e.g. a baked tile read with `texelFetch`): linear, nearest, no mipmaps. |
| `albedo R G B [A]`, `albedo_map "p"`, `normal_map "p"`, `normal_scale f`, `orm_map "p"` | |
| `occlusion f`, `roughness f`, `metalness f`, `specular f` | |
| `emission R G B`, `emission_energy f`, `emission_map "p"` | |
| `uv_scale u v`, `uv_offset u v` | |
| `alpha_cutoff f` | 0 = never discards; allows the depth pre-pass. An opaque cull-back material without shader and albedo_map whose albedo alpha is 255 can't discard and is pre-passed whatever its value. |
| `cull back\|front\|none` | |
| `transparency off\|prepass\|alpha` | |
| `prepass on\|off` | Default on. `off`: this material is never drawn in the depth pre-pass, only once in the shading pass (for a cheap shader on very many triangles, where the pre-pass would only double the vertex cost). With R3D patch 28; upstream R3D always pre-passes, so the key changes nothing there. |
| `collide on\|off` | Default on. `off`: the material's boxes don't block the player (`MS_Collides` / `MS_FirstCollision` skip them: grass, foliage, anything to walk through); they are still drawn, picked, cast shadows and hide other boxes' faces, and stay in `boxes[]` / `colliders[]` (`MS_Material.noCollide`). Models ignore it (their own `collide`). |
| `shadow on\|off` | Default on. `off`: the material's boxes cast no shadow: `DrawBoxRange` draws them with `R3D_SHADOW_CAST_DISABLED`, so they stay out of the sun's tiles / camera-fitted map and every point / spot shadow, and a range past its draw distance is not drawn at all (its shadow-only draw would draw nothing). They are still drawn, lit, shadowed by other objects, picked and collide (`MS_Material.noShadow`; `MS_GetDrawRange`'s `castShadow`). Models ignore it (their own `shadow`). |
| `draw_distance <m>\|auto\|off` | Default auto. Beyond this distance from the camera (to the bounds of a map cell's boxes of the material) the camera skips its boxes; their shadows stay. Multiplied by the detail distance setting (0 there: always drawn). `auto`: 400 × the size of its largest box (the square root of its largest face). `off`: always drawn. With R3D patch 30. |
| `blend alpha\|additive\|add_alpha\|multiply\|premultiplied` | |
| `unlit on\|off` | |
| `priority n` | |
| `inherit albedo\|normal\|orm\|emission\|all` | Model overrides: keep the model's own map. |
| `wrap repeat\|clamp\|mirror` | Wrapping of this material's textures and samplers. Default repeat, so `uv_scale` > 1 tiles; trilinear + mipmaps. |
| `filter trilinear\|nearest` | Magnification of its albedo / normal / orm / emission maps: trilinear (default), or nearest (crisp texels up close, for pixel art; the nearest texels of the two nearest mipmaps are blended at a distance). A file used with both filters is two textures. |
| `uv_faces a b c d e f` | See [uv_faces](#uv_faces) below. |
| `instanced on\|off` | Default off. Keep this material's boxes as instanced unit cubes. Off: boxes are drawn as one mesh per cell of the faces no other box hides (see `UploadBoxes`). On is for shaders that read `INSTANCE_*` or the model-space `POSITION` in `vertex()`; a scan of the source turns it on, with a warning, when it finds those. |
| `paint "textures/paint/x.png"` | The material's paint atlas (RGBA8 PNG, straight alpha). The material draws with a paint stage after its own shader once a `paint` line below draws with it ([painting.md](../painting.md) §4); without one nothing changes. |
| `paint_filter trilinear\|nearest` | How the atlas is sampled (nearest: crisp paint texels). |
| `paint_grid <0 \| 1..1024>` | The grid (texels per metre, through the world origin) the material's paint texels lie on ([painting.md](../painting.md) §17): each box face's paint maps onto the grid cells it covers, so painted texels meet the surface's own texels and the paint of neighbouring boxes. `0`: none (the map's `env paint.grid` doesn't apply either). Without the key the map's `env paint.grid` applies. Changes nothing without `paint` lines. |

#### uv_faces

`uv_faces a b c d e f` puts the box faces +x −x +y −y +z −z (a turned box: its own axes) in cells of the texture:

- Faces with the same number (0 … 5; every number from 0 to the largest used) share a cell.
- k cells lie in a grid of ⌈√k⌉ × ⌈k / cols⌉; cell n is at column n % cols, row n / cols.
- A face keeps the cube's UV orientation inside its cell, inset by 1/64 cell: the first few mipmap levels don't bleed from the next cell; far smaller ones do.
- Needs `uv_scale 1 1` and `uv_offset 0 0`.
- Boxes only (merged, instanced and turned); models ignore it.
- Without the key every face shows the whole texture.

### Paint

([painting.md](../painting.md) §4.2.) A `paint` line may be anywhere after the material's line.

| Line | Meaning |
|---|---|
| `paint "material" N:i <faces> x y w h` | Faces of box i of the line with `id N` (its index within the line, map order) get the rectangle x y w h (texels) of the material's atlas. `<faces>` is a comma list of +x −x +y −y +z −z (a turned box: its own axes). Each face shows the rectangle in its own orientation (R3D's unit cube UVs). |
| `paint "material" N:i slot S x y w h` | Slot S of model instance i of the line `id N`, drawn with that material (an override), in the slot's paint UVs: its own UVs when they lie in 0..1 without overlap, else the file's unwrap sidecar, `models/<file>.paintuv`. |
| `paint own N:i slot S x y w h [paint_filter nearest]` | The same for a slot drawn with the model's own material. Its atlas is `textures/paint/<file>_s<S>.png`. |

**Rules (errors):**

- the material exists and has a `paint` key;
- line N exists and made such an object i;
- the faces are valid and not repeated;
- w h ≥ 1;
- no two rectangles of one atlas share a texel;
- no face or slot of an object is in two entries of one material.

An entry whose object no longer draws that face or slot with that material, or whose rectangle lies outside its atlas, draws nothing (logged). On a material with a paint grid (`paint_grid`, `env paint.grid`), a box face maps onto its own grid cells from the rectangle's lower-left texel, and an entry whose rectangle is smaller than one of its faces' cells draws nothing (logged: `paint: 120:0 +x covers 12x8 cells of the grid, more than its 10x8 rectangle; inactive`). `paint` lines are not statements: they take no statement key.

### Environment

```
env <key> <value...>
```

**R3D environment.** Every field of `R3D_Environment`, by its C name: `background.color/energy/skyBlur/rotation` (x y z w), `ambient.color/energy`, `ssao.*`, `ssil.*`, `ssgi.*`, `ssr.*`, `fog.*`, `volumetricFog.*`, `dof.*`, `bloom.*`, `autoExposure.*`, `tonemap.*`, `color.*` (the table `ENV_KEYS` in the source).

- Enums by lower-case name: `fog.mode disabled|linear|exp2|exp`; `bloom.mode disabled|mix|additive|screen`; `tonemap.mode linear|reinhard|filmic|aces|agx`; `dof.mode disabled|enabled`.
- Booleans `on|off`; colours `R G B [A]`.

**Sky.** `sky.mode procedural|color|panorama`, `sky.size n`, `sky.panorama "textures/x.hdr"`, and the `R3D_ProceduralSky` fields: `sky.skyTopColor`, `sky.skyHorizonColor`, `sky.skyHorizonCurve`, `sky.skyEnergy`, `sky.groundBottomColor`, `sky.groundHorizonColor`, `sky.groundHorizonCurve`, `sky.groundEnergy`, `sky.sunDirection` (default: `sun.direction`), `sky.sunColor`, `sky.sunSizeDeg`, `sky.sunCurve`, `sky.sunEnergy`.

**Ambient sky.** `ambientSky.mode procedural|same|off`, `ambientSky.size n`, and `ambientSky.<sky field>`. The ambient map (IBL) comes from a copy of the visible sky with these keys changed (`procedural`), from the visible sky itself (`same`), or there is none (`off`).

**Sun.** `sun.enabled on|off`, `sun.direction x y z` (the direction the light travels), `sun.color`, `sun.energy`, `sun.specular`, `sun.fogEnergy`, `sun.range` (shadow distance), `sun.shadow on|off`, `sun.softness`, `sun.depthBias`, `sun.slopeBias`; `shadow.size n` (px, read before `R3D_Init`: `MS_SetInitHints`). The biases are for `shadow.size`: with a smaller shadow map (a quality preset, `MS_LoadOptions.shadowSizeOverride`) they are scaled up by shadow.size / actual size (the texel ratio).

**Volumetric fog fade.**

- `volumetricFog.fadeStart f`, `volumetricFog.fadeEnd f`: the volumetric fog's density fades out by distance from the eye: 1 up to fadeStart, smoothstep to 0 at fadeEnd (fadeEnd > fadeStart). Default 0 0 = no fade: the fog stops hard at `volumetricFog.length`. Keeps the near light shafts while far buildings stay clear. R3D patch 13: ignored without `MS_PATCHED_R3D` (upstream R3D then renders the unfaded fog).
- `volumetricFog.fadeNear f`: the density also fades in, smoothstep from 0 at the eye to full at f m. Default 0 = off; at most fadeStart when a fade-out is set (larger values are clamped to it). Without it the sunlit fog around the eye veils the whole view, so the scene looks brighter when the player stands in sunlight than in shadow. R3D patch 15.

**Paint grid.** `paint.grid f`: the paint grid (texels per metre) of every material without `paint_grid` and of model slots drawn with their own material; `0` (the default) or 1..1024 ([painting.md](../painting.md) §17.1). Changes nothing without `paint` lines; a live edit of it rebuilds only the paint table.

**Bloom off.** `bloom.offExposure f`: the exposure factor applied when a quality setting turns bloom off, so the mean brightness stays (default 1.19, matched on the default map).

**Cache hints** (optional; derived from the map bounds without them, see the region "Caches" in the source): `shadow.home x y z`, `fogCache.region x0 y0 z0 x1 y1 z1`.

**Defaults and checks.** Every key has a built-in default (`BuiltinEnvironment()`: the values the default map was built with), so a map without `env` lines renders with those. Numeric values outside sane ranges (`ENV_RANGES`, e.g. `volumetricFog.stepSize` ≥ 0.05) and zero directions are rejected.

**Fog zones.** `fogzone x0 y0 z0 x1 y1 z1 fade <m> <key> <value...> ...` (main file only, like `env`): while the camera is inside the box the distance fog takes the line's values for `fog.density`, `fog.color`, `fog.start`, `fog.end`, `fog.skyAffect` (each at most once, checked like the `env` key); over `fade` m outside it they blend back (smoothstep of the camera's distance to the box). Stored in `SceneData.fogZones` (`MS_Scene.fogZones`, `MS_FogZone`), compiled as section 20. `ApplyFogZones`, called at the top of `MS_PushLights`, blends from the fog values R3D's environment held when something else last set them (it keeps the values it wrote, `MS_Private.fogWritten*`; when the environment holds others, `MS_ApplyEnvironment`, a quality setting or the program changed it, and those become the base), zones in map order, and writes the result for `R3D_End` to upload. A map without zones returns at once, so its image is unchanged. The line goes into the look hash, so a live edit of it is a full upload. User-facing description: `docs/map-format.md` §2.5.

### Player and cameras

| Statement | Meaning |
|---|---|
| `spawn x y z yawDeg [pitchDeg]` | The player start, feet position (default 0 0 0, yaw 0). |
| `viewpoint "Name" ex ey ez tx ty tz` | A camera viewpoint (eye, target), in order 1..n. Without any, one viewpoint is made from the spawn. |

### Paths, limits, entities and errors

**Paths** are relative to the package root (the folder of the map file). Absolute paths and `..` components are rejected, so a package can be copied anywhere. The files a model file itself refers to (glTF buffer / image URIs, OBJ `mtllib`, `.mtl` texture maps) must stay inside the package too; this is checked as assimp reads them (`MS_CheckModelReferences`).

**Limits:** none on content: any number of objects, groups, materials, lights and viewpoints; lines and names of any length. Sanity caps against runaway input:

| Cap | Value |
|---|---|
| `MS_MAX_OBJECTS` | 1 000 000 boxes + models + lights; a `repeat` grid multiplying to more fails |
| `MS_MAX_FILE_BYTES` | 4 MB per map-text file (the main file and each imported one) |
| positions | spawn / viewpoint / light positions within ±1 000 000 m |

**Source lines.** Every box remembers the map line of the statement that made it and that line's map-text file (`boxRefs[]`: `line`, `file`).

**Entities.** Every box, model, light and barrier is an entity (the `MS_Scene` `entity*` arrays) with a stable id: the line's `id` (or a key derived from the statement's text) and its index within the line.

**Errors** are reported as `file:line: message`. Besides syntax errors (unknown keyword, wrong argument count, bad number) these include a missing file, a path outside the package, an undefined material, too many objects, a spot light whose `inner` angle exceeds `outer` (`spot light: inner must be <= outer`), and a map with no boxes and no models (`map is empty (no boxes and no models)`; the compiled-map reader refuses it too). A line in an imported file is reported with that file's path (`maps/x/parts/lake.map:41: …`); the imports' own messages are in the next section.

## Imports: a map in several map-text files

The format is [map-format.md §2.10](../map-format.md#210-imports-one-map-in-several-files). This is how the runtime and its API carry it (design of 2026-11-21).

**Sources are per file.** The scene keeps a list of its map-text files (`MS_MapFile`: package-relative `path`, `fullPath`, `hash`, `size`, and the `import` line that reached it): `[0]` is the main map file, then each imported file in the order the parser first reached it. Every source line is a pair (file, line):

- `MS_Ref.file`, `MS_Source.file`, `MS_Material.file`, `MS_ModelFile.file` and `MS_PaintEntry.file` say which file their `line` is in (0 = the main file, so a map without imports has every `file` 0);
- the source-line API takes the file: `MS_LineText(scene, file, line, &len)`, `MS_GetLine(scene, file, line, …)`, `MS_MapText(scene, file, &size)`; `MS_MapFileCount` / `MS_MapFileAt` / `MS_MapFileFind` list the files;
- the live edit takes the text of every file: `MS_EditSetText(scene, files, count, stats)` with `MS_MapFileText { path, text, size }`, `files[0]` the main file; an `import` takes the text given for its path, else reads the file (a newly imported file). The new scene's file list is the parse's own, so the editor keys its working copies by path.

*Rejected:* one expanded text (every import pasted in) with a line map back to the files. The editor's working copy is whole text that it patches and hands to the live edit; with one expanded text it could not tell which file an inserted line (a split `repeat`, a duplicate, a new model or `paint` line) belongs to, and a line at a file boundary would be ambiguous. Per-file texts make every edit a patch of one file.

**Parsing.** An `import "<path>.map"` line (exactly one quoted path; no `id` / `repeat` prefix) parses that file's statements where it stands: `Parser.path` and the line number switch to the imported file while it is read, so every message is `<that file>:<line>: …`. The path goes through `InsidePackage` and `PackagePath` (so it is watched by hot reload even while missing, then must exist), is normalised (empty and `.` components dropped) and must end in `.map`. Each file is read with `ReadWholeFile(…, MS_FILE_MAX, …)`: the 4 MB cap is per file. A set of the files reached so far (the main file included; a hash set by path, also in the compiled reader, so many parts stay linear; files are told apart by their normalised path, so a symlink to a part counts as another file) refuses a second import (`"parts/a.map" is already imported (at x.map:12): a file is read once per map`) and a cycle (the chain of open imports is in the message). Before the import the parser saves `cur.group`, `cur.material` and its format-line state, clears them (group -1, material 0 = `"default"`, no format line seen), and restores them afterwards. A part must begin with `monolith N` equal to the main file's (in practice `MS_FORMAT_VERSION`: an older or missing one gives the usual migration message, naming the part's line). `env`, `spawn` and `viewpoint` in a part fail with `<file>:<line>: env lines belong in the main map file (<main>)`. The empty-map check runs once, for the whole map.

**Entity ids keep today's rule.** Implicit keys come from the statement's tokens (not its file), assigned in the order of the expanded text; explicit `id n` values are unique across the whole map (`StmtSlot` is per scene); the duplicate-id message names the other line as `<file>:<line>` when it is in another file (today's `also on line N` when both are in the same file). The next free id for an edit is the largest in any file + 1. A map split into parts (with `use` / `group` restated as §2.10 says) has the same ids as the single file, because every statement's tokens and their order are unchanged.

**Hash.**

- `scene->hash` without imports: FNV-1a 64 of the main file's bytes, as before.
- With imports: FNV-1a 64 over the concatenation, for every map-text file in file order (`[0]` first), of `<path>` `\0` `<size in decimal ASCII>` `\0` `<the file's bytes>`, where `<path>` is `MS_MapFile.path` (the main file: its file name). For example, in Python: `h=0xcbf29ce484222325; for p,b in files: for c in p.encode()+b"\0"+str(len(b)).encode()+b"\0"+b: h=((h^c)*0x100000001b3)&(2**64-1)`.
- Each file's own FNV-1a 64 is `MS_MapFileAt(scene, i)->hash` (bundles, and the editor's check that a file on disk is still the one loaded).

Hot reload is unchanged in kind: imported files are dependencies (`AddDep`, in `MS_Dependency` order), so `MS_Poll` watches them and its combined hash covers them.

**Compiled maps.** One `<name>.mapb` per map, never one per part: the compiler writes the whole parsed map, parts included, into that single file, and the reader needs nothing from the parts to load it (a game loads the one file as today). `CmRef` and every record field that holds a map line get the line's file; a new section `sources` lists every map-text file (`[0]` the main file, equal to the header's `sourceName` / `sourceHash` / `sourceSize`) with its package-relative name, size and FNV-1a 64. Only `SourceState` (`MS_CompiledSource`) reads the parts' text: stale when any file differs, missing when one is gone. `CM_VERSION` 9 → 10, so every existing `.mapb` must be compiled again.

**Dumps.** A line in the main file still prints `line <l>`, so the dumps of every map without imports stay byte-identical; a line in a part prints `line <path>:<l>` (package-relative). The dumps have no version line to bump, and no field is added that every dump shows.

**Defaults chosen** (the developer may change them): sources per file (above); each part has its own `monolith N`, equal to the main file's; nesting allowed, each file read once per map, cycles an error; `use` and `group` restored after an import; new lines go into the file of the line they come from (a split `repeat`, a duplicate, an `add`ed model next to an object) and a new `paint` line into the file of the painted object's line; import paths relative to the package; dumps and `scene->hash` unchanged without imports; the 4 MB cap per file.

**Messages** (`tests/unit/parse_error_test.c` has a case for each):

| Case | Message (at the line named) |
|---|---|
| a file imported a second time | `"parts/a.map" is already imported (at x.map:12): a file is read once per map` (the first import's file and line) |
| a cycle (a file importing itself, directly or not) | `import cycle: "parts/a.map" is already being read: parts/a.map -> parts/b.map -> parts/a.map` (the open files from the one imported again) |
| an `id` or `repeat` prefix on an import | `import takes no id or repeat prefix: import "parts/name.map"` |
| imports nested deeper than 256 files (`MS_IMPORT_DEPTH_MAX`: each level is a recursion of the parser) | `imports nested more than 256 files deep` |
| outside the package, missing, not quoted | the path checks every quoted path gets (`path must stay inside the package: …`, `file not found: … (paths are relative to the package folder)`, `a path must be in double quotes: …`) |
| not a `.map` file | `import: "textures/a.png" is not a map-text file (an imported file's name ends in .map)` |
| not one path | `import takes one path in double quotes: import "parts/name.map"` |
| `env` / `spawn` / `viewpoint` in a part | `env lines belong in the main map file (x.map)` (at the part's line) |
| a part without the current format line | the migration message at the part's line, naming the main map: `run /migrate-map <main map> …` |
| a duplicate `id` in another file | `duplicate id 5 (also at x.map:2)` (`also on line 2` when both lines are in one file) |

Other messages that name a second line (paint entries: `line 12 makes no boxes`, `overlaps line 12's`) write a line in an imported file as `line parts/a.map:12`, as the dumps do.

**Implementation** (`runtime/monolith_scene.c`). The file list is `SceneData.srcs[]` (`MapSrc`: `path`, `fullPath`, and in editor builds the public `MS_MapFile`, the text and its line index); `[0]` is made by `ParseScene` before the first line, each part by `ParseImport` (region "Parser") when its import line is read, and a compiled load fills it from the `sources` section, so both builds can name a line's file. Every later message that names a map line (textures, shaders, model files, paint entries) takes the line's file through `SrcName(d, file)`, so each object, material key (`MatDef.shaderFile`, `mapFile[]`, `paintFile`, `MatSampler.file`), model file, `paint` entry and statement (`stmtFiles`) records its file next to its line. `ParseImport` copies the path before it parses the part (the parser's line buffer and tokens are reused by the part's lines) and keeps the open files in `Parser.open` (the cycle check). An `import` line goes into the live edit's look hash like a `material` or `env` line: adding, removing or moving one makes the edit a full upload.

