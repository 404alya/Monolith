# Editor tools: world points, 3D snips, sketches, groups as units, shapes

The design note of the round of 2026-11-27 (as [request-queue.md](request-queue.md) is for the queue): the interfaces several parts of the code share, written before the code. Five features:

1. **World points in empty space**: volume corners, barrier clicks and shape placement can land in the air, on a *work plane* (§1).
2. **3D snips**: a volume marked as a snip sends images of only what is inside it (§5).
3. **Sketches**: freehand lines, arrows and circles over a frozen frame, numbered S1, S2, … and sent with the request (§4).
4. **Groups as units**: rename, delete, duplicate, move, turn and scale a whole group; put objects into a group or make a new one; the groups panel (F6) (§3).
5. **Shapes**: untextured primitives (cube, sphere, cylinder, cone, capsule, plane) as a new map statement, `shape`, paintable like boxes (§2).

Nothing here changes how a map in `maps/` looks or what its box list is: no map uses `shape`, and every overlay (the work plane, sketches, snip boxes, the groups panel) is editor drawing that never reaches `view.png`. Volumes, sketches and snips are never written into the map; only group edits and shapes are, each as one undoable edit with the smallest text change.

The developer's calls chosen as defaults (say so in the final summary): the work plane's rule (§1.2), the sketch keys (§4.1), the 3D snip key and its four images (§5.1), F6 and name identity for groups (§3.1), the shape collider (§2.5), the statement name and its argument order (§2.1).

## 1. World points

### 1.1 The function

```c
// src/designer.h
typedef enum {
    WORLD_POINT_SURFACE = 0,    // a surface hit as before (snapped on the hit face's axes); the work plane when nothing is hit
    WORLD_POINT_PLANE,          // the work plane even over a surface (Alt held while clicking)
} WorldPointMode;

typedef enum { WORLD_POINT_NONE = 0, WORLD_POINT_ON_SURFACE, WORLD_POINT_ON_PLANE } WorldPointKind;

// The world point under `cursor` (pixels of the frame `cam` renders; the crosshair: its centre):
// a surface hit snapped as VolumePoint did (Ctrl: exact), else the ray's crossing with the work
// plane (§1.2), snapped to the N grid in x and z (Ctrl: exact). *normal: the hit face's normal, or
// (0 1 0) on the plane. WORLD_POINT_NONE when nothing is hit and the ray doesn't reach the plane
// (looking up at a plane below the eye, or the crossing farther than WORK_PLANE_MAX_DIST).
WorldPointKind DesignerWorldPoint(const Designer *d, Camera3D cam, Vector2 cursor, WorldPointMode mode,
                                  Vector3 *out, Vector3 *normal);
#define WORK_PLANE_MAX_DIST 2000.0f     // metres along the ray
```

One function for every place that picks a point in the world: the volume corners (`VolumePoint`, `VolumeDraft`, `VolumeInput`), the barrier's three clicks (edit mode **H**, which runs the volume clicks) and the placement of a new shape (§2.8). A click on a surface gives exactly the point it gave before (same snap, same Ctrl), so every existing volume and barrier script gives the same result.

### 1.2 The work plane

- A horizontal plane `y = workPlaneY` (`Designer.workPlaneY`, `Designer.workPlaneSet`).
- **Its height:** for the first corner, the height of the last volume's base (`volumes[last].box.min.y`), else the camera's feet (eye − `MS_DEFAULT_EYE_HEIGHT`), snapped to the N step; after the first corner, the first corner's height (the footprint is flat, as on a surface). **PgUp / PgDn** before the first click move it by one snap step (in volume mode they move the active volume only once a corner is placed or when no plane is in use: the plane takes them while the next click would use it, i.e. while the crosshair is on the sky or Alt is held); the HUD says `work plane y 3.50 m`.
- **When it is used:** the ray hits nothing (sky, or a surface beyond the far limit), or **Alt** is held while clicking (the plane even over a surface: a corner in the middle of a room, above water).
- **How it is drawn** (live overlay only, never in `view-marked.png` or `view.png`): while the next click would land on it, a 9 × 9 grid patch of N-step cells centred on the snapped point, in the volume colour (`VOLUME_COLOR`, fainter toward the patch's edge), with a small cross at the point and a vertical tick down to the first surface below it (so a point in the air shows where it stands over the ground). The volume being made is drawn as today.
- **Scripted input:** `--input-script` clicks at sky pixels work as they are (the plane is the fallback); `--work-plane <y>` (with `--shot`) sets the plane's height for the run, and `down ALT` in the script uses it over a surface. `ALT` is already a script key.

### 1.3 What it gives the request

Nothing new in the bundle: a volume is its box, whatever its corners hit. `volumes[].floor` already tells the agent what is under a volume in the air.

## 2. The `shape` statement

### 2.1 Syntax

```
shape <kind> cx y0 cz sx sy sz [rot yawDeg pitchDeg rollDeg]
shape cylinder 4 0 -6 1.2 3 1.2                 # a 1.2 m wide round pillar, 3 m tall
shape sphere 0 1 0 2 2 2                        # a 2 m ball resting on y 1
shape cone 10 0 3 4 2.5 4 rot 45 0 0            # a roof cone
id 31 repeat 4 3 0 0 shape capsule -6 0 8 0.6 1.8 0.6
shape plane 0 0.01 0 8 0 8                      # a flat 8 x 8 m sheet at y 0.01
```

- Laid out like `block`: centre x z, from y0 up, full sizes (sx along x, sy up, sz along z), so `scale`, `size`, `turn` and `move` reuse the block math: the shape's box is exactly the box a `block` with the same six numbers makes, and the shape is drawn inside it (§2.2). `rot` turns it about the box's centre exactly as `block … rot` (R = Ry · Rx · Rz).
- Prefixes as for boxes: `id`, `repeat` (shifts `cx y0 cz` only), and the active `group` and `use` (its material; no `material` key on the line).
- Rules (parse errors with the line): a known kind; sx, sz > 0; sy > 0 except for `plane`, where sy must be 0 (`a plane has no height: write 0`); no `stairs`-like counts.
- A statement of the current map format (3): no version bump. Older runtimes refuse it with a line error. Every existing map reads the same.

### 2.2 Kinds, meshes and parts

Each kind is one mesh, generated once per load in a unit frame (centred in x z, from y 0 to 1, x and z from −0.5 to 0.5) and scaled by the size, turned and placed per shape. Segment counts are fixed, so the triangles are the same on every machine:

| Kind | In its box | Segments | Parts (paint regions, numbered like a box's faces) | Part UVs (0..1) |
|---|---|---|---|---|
| `cube` | the whole box | 6 quads (24 vertices, 12 triangles) | 0..5 = `+x -x +y -y +z -z` | as a box face (R3D's unit cube) |
| `sphere` | the ellipsoid inscribed in the box | 32 around × 16 rings (UV sphere, poles split per segment) | 0 = `surface` | u around from −z, counter-clockwise seen from above; v from the bottom pole (0) to the top (1) |
| `cylinder` | the elliptic cylinder inscribed in the box | 32 around; side + two caps (fans) | 0 = `side`, 1 = `top`, 2 = `bottom` | side: u around as the sphere, v up; caps: the disc's x z mapped onto 0..1 (top: seen from above, +x right, −z up; bottom: seen from below) |
| `cone` | apex at the top centre, base the inscribed ellipse at y0 | 32 around; side (one apex vertex per segment) + base fan | 0 = `side`, 1 = `bottom` | side: u around, v up (apex 1); bottom: as the cylinder's |
| `capsule` | a cylinder with a half-ellipsoid at each end, inscribed in the box (each cap's height min(sx, sz) / 2, at most sy / 2) | 32 around, 8 rings per half-sphere | 0 = `side`, 1 = `top`, 2 = `bottom` | side: u around, v up; caps: u around, v from the rim (0) to the pole (1) |
| `plane` | a rectangle at y0, sx × sz | 1 quad each way (two-sided: two quads, 4 triangles) | 0 = `top`, 1 = `bottom` | top: seen from above, +x right, −z up; bottom: seen from below |

- Normals are smooth around a round side, flat on caps and the cube; the seam of a round kind is at −z.
- The mesh carries, per vertex, its part (`boneIndices[0]`) and its part UV (`boneWeights` as two unorm16), exactly like a painted model's mesh ([painting.md](painting.md) §4.4), so the paint stage needs no new path; its texture coordinates (`texcoord`) are the part UV too, so a textured material shows its texture once per part.
- `uv_faces` does not apply (as for models); `uv_scale` / `uv_offset` do.

### 2.3 Runtime records (`runtime/monolith_scene.h`)

```c
typedef enum { MS_OBJ_NONE = 0, MS_OBJ_BOX, MS_OBJ_MODEL, MS_OBJ_LIGHT, MS_OBJ_BARRIER, MS_OBJ_SHAPE } MS_ObjKind;

typedef enum {
    MS_SHAPE_CUBE = 0, MS_SHAPE_SPHERE, MS_SHAPE_CYLINDER, MS_SHAPE_CONE, MS_SHAPE_CAPSULE, MS_SHAPE_PLANE,
    MS_SHAPE_KIND_COUNT
} MS_ShapeKind;

typedef struct {
    MS_ShapeKind kind;
    BoundingBox box;        // world AABB of its turned box (= its colliders[] entry)
    bool turned;            // written with a nonzero `rot`
    MS_TurnedBox shape;     // centre, size along its own axes, rotation as written, axes (always filled)
    MS_Ref ref;             // line, file, statement, group, material (the active `use`), copy; part 0
} MS_Shape;

// MS_Scene
const MS_Shape *shapes;     // `shape` lines, in map order
int shapeCount;
int shapeColliderFirst;     // colliders[shapeColliderFirst + i] is shape i
const int *shapeEntities;

// The part names of a kind ("+x" ... for a cube, "side" / "top" / "bottom" ...), its part count
int MS_ShapePartCount(MS_ShapeKind kind);
const char *MS_ShapePartName(MS_ShapeKind kind, int part);
const char *MS_ShapeKindName(MS_ShapeKind kind);   // "cube", "sphere", ...
```

- **colliders[]** become: boxes, model colliders, **shapes**, barriers. `barrierColliderFirst = colliderCount − barrierCount` stays true; for a map without shapes the list is unchanged.
- **Entities:** shapes are entities of kind `MS_OBJ_SHAPE` in map order among the others; `MS_Transform`: position = centre of the box, rotationDeg as written, scale = the size. For a map without shapes every entity id and index stays as it is.
- **bounds:** a shape counts in `scene->bounds` (it is drawn) and in the cells / clusters by its AABB.
- **MS_Raycast** hits shapes by their triangles (the turned mesh: transform the ray into the unit frame), so picking, painting and a game's line of sight see them; `MS_ObjectRef` / `MS_ObjectBounds` / `MS_ObjectEntity` know the kind. On equal distances a box wins over a shape, a shape over a model.
- **MS_SurfaceHit** on a shape: `face` = the part, `faceUv` / `paintUv` = the part UV, `slot` −1, `material` the shape's.

### 2.4 Drawing

The runtime's choice (stream A), with these constraints: one mesh per kind, generated in the runtime (shipped with an export: plain C, no file); shapes drawn instanced per (kind, material) with their transforms, through the same render path as model instances (R3D draws, frustum culling by cluster or per instance, shadows by the material's `shadow`, `draw_distance` like a box of that size); a material's `collide off` and `shadow off` apply to shapes as to boxes. No draw call, upload or cache changes for a map without shapes. Live edits: a changed shape marks a dirty region of its old and new AABB (as a box does, [live-editing.md](live-editing.md) §3.2); its instance buffers are rebuilt, nothing else.

### 2.5 Collider

Its turned box: the same OBB a `block … rot` with the same numbers gets (an axis-aligned one when `rot` is absent or axis-aligned), so the player collides with, stands on and steps up a shape exactly as on that block. A plane's box is 0 thick: its collider is the box 0.01 m thick **below** y0 (top at y0). **Open decision** for the summary: exact sphere / cylinder / cone / capsule colliders (a ball the player can't walk around the corner of; cost per test).

### 2.5a Paint

A shape's parts are painted like a box's faces: a `paint` entry names parts with their names in place of face names, comma-separated, sharing a rectangle:

```
paint "default" 31:0 side 0 0 128 64
paint "default" 31:0 top,bottom 130 0 32 32
paint "default" 40:0 +x,-x 0 70 64 64          # a cube: the box's face names
```

- The face bit mask (`MS_PaintEntry.faces`, `EditOp.paint.faces`) holds the part bits (bit p = part p); the names are checked against the entity's kind (a sphere's `top` is an error).
- On a paint grid the shape's parts map like model slots (no grid cells): its texel size is the grid's, and its rectangle is the part's length along its u and v at that density ([painting.md](painting.md) §5.2, the draw mode's `ShapeRectSize`). **A cube's parts too** (decided in stream D: one rule for every kind, as the runtime already maps them; a cube's part is sized like a box face, but without cells).
- The compiled `CmPaint` gets kind 2 = shape (`object` = shapes[] index, `faces` = the part bits).

### 2.6 Compiled map, dumps, export

- **Compiled record** (`docs/compiled-map.md`): section 22 `shapes`, one record per shape `{ u32 kind; f32 center[3], size[3], rotationDeg[3]; u32 turned; i32 line, file, prim, group, material, copy, part }` (64 bytes); `CM_VERSION` 15; the reader checks the kind, sizes and the plane rule like the parser.
- `--dump-scene`: `shape <kind> center … size … [rot …] line … group … material …` lines after the barriers; `--dump-entities`: kind `shape`; `--dump-boxes`: unchanged (shapes are not boxes); `--tri-report`: a shapes row per (kind, material) with its triangles.
- **Bundles:** an object's `kind` is `shape`; its `shape` object `{ kind, center, size, rotationDeg }`, its `material` as for a box.
- **Export:** the runtime generates the meshes; nothing else to copy.

### 2.7 Edits (`src/mapedit.c`)

`Analyze` reads `shape <kind>` and its six numbers like a block's (the kind word shifts the positions by one token; `rot` after the sixth number), so move, turn, rotation, scale, size, duplicate, remove, id and move-to-file work on a shape line. `EditObjClass` gives `EDIT_CLASS_SHAPE`, which the transform modal treats as a block (`EditClassBoxLike`). A plane refuses a `size` / `scale` that gives it a height. New op:

- `EDIT_SHAPE_ADD`: a new line `id N shape <kind> cx y0 cz sx sy sz` after the anchor's line (its group; the `use "default"` … `use "<active>"` wrap when a `use` is active there, as `add`), or at the end of the main map file with nothing under the cursor. Script: `shape-add <kind> <cx> <y0> <cz> <sx> <sy> <sz>`. The new shape is selected.

### 2.8 Placing (stream D)

The model picker (**I**) gets a *Shapes* section at its top: the six kinds with a small thumbnail each. Enter on a kind places it standing on the point `DesignerWorldPoint` gives (a surface or the work plane), sizes 1 × 1 × 1 m (plane 2 × 0 × 2), with the plain grey `"default"` material: one `EDIT_SHAPE_ADD`, selected, so **G** moves it at once.

## 3. Groups as units

### 3.1 What a group is

A group is identified by its **name**: every `group "<name>"` statement with that name, in any file, makes one group in the panel and in the group commands (the map-editing rule already says not to repeat names; a repeated name, as `"plain: grass"` in `maps/hysteria`'s parts, is shown and edited as one group). Ctrl+click is unchanged: it selects one statement's objects (an open point for the summary). A **whole-group selection**: every group statement that has a selected object has all its objects selected, and there is at least one such group (ungrouped objects make it not one).

An **area** is the text before `": "` in a group's name (`"plain: house"` → area `plain`); a name without `": "` has no area.

### 3.2 Edit ops and script commands

| Op | Script | Writes |
|---|---|---|
| `EDIT_GROUP_RENAME` | `group-rename "<old>" "<new>"` | the quoted name of every `group "<old>"` line in every file. Refused: `<new>` empty, with `"`, or already a group (use `group-move`) |
| `EDIT_GROUP_DELETE` | `group-delete "<name>"` | every object line of the group (every block of that name), every `group "<name>"` line, and comment lines directly above a deleted line inside the block; `use` lines whose scope holds no other object any more (the move-to-file rule); the paint entries of deleted entities. `material`, `env`, `import`, `paint` (of other entities), `spawn`, `viewpoint`, `fogzone` lines stay. No empty `group` line is left |
| `EDIT_GROUP_NEW` | `group-new "<name>"` | the selection's whole lines (a `repeat` copy split out first, as move-to-file) into a new block `group "<name>"` at the end of the file that holds them (with the `use` each line had, as move-to-file); `group` / `use` lines left without objects go. Refused: lines of several files, an existing name |
| `EDIT_GROUP_MOVE` | `group-move "<name>"` | the selection's whole lines to the end of that group's last block (in its file; with the `group` / `use` scope of move-to-file, also across files). Refused: lines of several files in one call |
| `EDIT_GROUP_DUPLICATE` | `group-duplicate "<name>" "<new>" <dx> <dy> <dz>` | a copy of every object line of the group, shifted, in a new block `group "<new>"` right after the group's last block (before the next `group` line, with the `use` each source line had); each copy a new id |

- Each is one step (one undo), live, its self-check as move-to-file's where lines move (the entity set the same except the groups: for rename the group name, for new / move the moved objects' group).
- `group-rename` and `group-delete` change nothing but those lines; the selection follows (delete: empty).
- Moving, turning and scaling a whole group are the existing `move`, `turn … pivot`, `scale … pivot` on `select group "<name>"`.

### 3.3 Keys on a whole-group selection (§3.1)

- **G / Alt+R / S / the gizmo**: already about the selection's centre; unchanged.
- **R / Shift+R**: turn ±90° about the vertical through the selection's centre (one `EDIT_ROTATE` with `hasPivot`), so the house turns as one; on any other selection as today (each object about its own centre).
- **Ctrl+D**: one `EDIT_GROUP_DUPLICATE` per selected group, shifted one footprint of the whole selection to the right; the new group is named `"<name> 2"` (the next free number); the copies become the selection. On any other selection as today.

### 3.4 The groups panel (F6, `src/groupspanel.c`)

In spectator mode, F6 opens the panel (the id panel is the model; `UiListDraw`, [ui-design.md](ui-design.md)). Rows: every group by name, sorted by area then name, under an area header row when the area has more than one group: name, object count, file(s), first–last line. Typing filters (name, area, file); ↑ / ↓ / PgUp / PgDn / Home / End select. Keys:

| Key | Action |
|---|---|
| Enter | select the group (an area row: all its groups), fly in front of it; the panel closes |
| Ctrl+R | rename: a text field with the name; Enter applies (`EDIT_GROUP_RENAME`) |
| Delete | delete the group (press again within 3 s; an area row: all its groups, one step each) |
| Ctrl+D | duplicate as Ctrl+D in the edit mode |
| Ctrl+N | a new group from the selection: a text field for the name (`EDIT_GROUP_NEW`) |
| Ctrl+G | put the selection into the highlighted group (`EDIT_GROUP_MOVE`) |
| Esc / F6 | close |

Edits from the panel are manual edits like any other (live, undoable, counted as unsaved, the first one warns when a request is open). The view-only build: filter and Enter only (the others refused, `MSG_VIEW_ONLY`). `--groups-panel [filter]` with `--shot --shot-hud` draws it.

## 4. Sketches

### 4.1 Keys

In snip mode (**C**), **D** switches between rectangles and the pen; the help bar says which. With the pen: drag with the left button to draw a stroke; **1–4** choose the colour (red, yellow, blue, white); **F** free, **L** line, **A** arrow, **O** circle choose the kind (free to start); **Backspace** removes the last stroke, **Shift+Backspace** all strokes. Strokes and regions go into the same request (Enter opens the prompt). Strokes are numbered S1, S2, … on screen and stay until sent or cleared, like regions; leaving snip mode drops them (they belong to the frozen camera), as it drops regions.

- **free**: the cursor's path, a point kept every 3 px; **closed** when it ends within 16 px of its start.
- **line** / **arrow**: from the press to the release (an arrow has its head at the release).
- **circle**: centre at the press, radius to the release; drawn and sent as a closed 64-point polygon.

### 4.2 Records (`src/designer.h`)

```c
typedef enum { SKETCH_FREE = 0, SKETCH_LINE, SKETCH_ARROW, SKETCH_CIRCLE } SketchKind;
#define SKETCH_COLOR_COUNT 4    // red, yellow, blue, white (scene-side constants, not UI tokens)

typedef struct {
    SketchKind kind;
    int color;              // 0..SKETCH_COLOR_COUNT-1
    Vector2 *points;        // pixels of the frame, top-left origin (heap)
    int pointCount, pointCap;
    bool closed;            // circles always; free strokes that end near their start
} DesignSketch;

// What a stroke touches, computed at submit time by DesignerAnalyzeSketch
typedef struct {
    DesignerHit start, end;             // what the first and last points hit
    Vector3 *worldPoints;               // the hit point of each sample along the stroke (heap)
    bool *worldHit;                     // per sample: something was hit
    int sampleCount;                    // at most SKETCH_SAMPLES, evenly spaced along the stroke
    DesignRegionObject *objects;        // objects hit along the stroke, by samples descending (heap)
    int objectCount;
    bool hasRegion;                     // closed: the inside analysed like a region
    DesignRegionInfo region;            // its rays inside the polygon only (rect = the polygon's bounds)
} DesignSketchInfo;
#define SKETCH_SAMPLES 256

// Designer: DesignSketch *sketches; int sketchCount, sketchCap; bool pen; int penColor; SketchKind penKind;
// DesignRequest: DesignSketch *sketches (copies); DesignSketchInfo *sketchInfos; int sketchCount;
bool DesignerAddSketch(Designer *d, SketchKind kind, int color, const Vector2 *pts, int n);   // --sketch
void DesignerAnalyzeSketch(Camera3D cam, int w, int h, const DesignSketch *s, DesignSketchInfo *out);
void DesignerFreeSketch(DesignSketch *s);
void DesignerFreeSketchInfo(DesignSketchInfo *i);
// Strokes on a w x h target; marked: as view-marked.png wants them (strokes + labels); alone: only the
// strokes on a transparent target (sketch.png)
void DesignerDrawSketches(const Designer *d, int w, int h, bool marked);
```

`--sketch <free|line|arrow|circle> <color 1-4> x0 y0 x1 y1 …` (repeatable; circle: centre then a point on the rim) attaches a stroke without a drag, like `--region`; `--input-script` drags (`down lmb`, `move`, `up lmb`) also work.

## 5. 3D snips

### 5.1 Keys and records

In volume mode, **X** marks or unmarks the active volume as a 3D snip; a snip volume is drawn with dashed edges and its label `V2 snip`. `--volume x0 y0 z0 x1 y1 z1 snip` marks one for tests. `DesignVolume` gets `bool snip`; `DesignVolumeInfo` gets `bool snip` and the images below.

Four images per snip volume, 1024 × 768 (or the box's aspect, longer side 1024), off-screen as the model picker renders its thumbnails, showing **only what is inside the volume** (how: Phase 6 decides; per object or per pixel, [§5.3](#53-open)):

| Side | Camera |
|---|---|
| `view` | the developer's camera direction, moved back so the box fills the frame |
| `front` | looking along −z at the box (from +z), framed to it |
| `side` | looking along −x (from +x) |
| `top` | looking down −y, the box's −z up in the image |

Perspective, fovy 40°, framed so the box's bounding sphere fits; the map's own lighting and environment, the sky replaced by a flat neutral grey so the cut-out reads.

### 5.2 Bundle

Files `snip-<n>-<side>.png` (n = the volume's number). `volumes[]` entries get `"snip": true` and `"images": [ { "file", "side", "camera": { "position", "target", "up", "fovyDeg", "width", "height" } } ]`; `"snip": false` and no `images` otherwise. The object list is the one volumes already have (`DesignerAnalyzeVolume`).

### 5.3 Open

Rendering only the contents: hiding everything outside the box per object (boxes, shapes and model instances whose AABB overlaps the box are drawn; others are not) is app-side; cutting at the box's faces per pixel likely needs an R3D patch (ask first). Phase 6 reports both before building.

## 6. Bundle format 9

One bump for the round: `"format": 9` in `request.json` and `request.md`'s first paragraph.

| File | Content |
|---|---|
| `sketch.png` | only the strokes, on a transparent background, at the frame's size (with their labels) |
| `view-marked.png` | also the strokes and their labels S1, S2, … (in their colours) |
| `snip-<n>-<side>.png` | per 3D snip volume, four images of only what is inside it (§5) |

**`request.json`:**

- `sketches[]`: `index`, `label` (`S1`), `color` (`red`, `yellow`, `blue`, `white`), `rgb`, `kind` (`free`, `line`, `arrow`, `circle`), `points` (`[[x, y], …]` pixels of `view.png`), `closed`, `start` and `end` (what the first and last points hit: `kind`, `index`, `point`, `normal`, `distance`, `line`, `group`, `material`, or `null` for sky), `samples` (`[[x, y, z] | null, …]`: the world point under each of up to 256 evenly spaced samples along the stroke), `objects` (hit along the stroke: kind, index, line, group, `samples`, `share`, largest first), `lines` (the same by map line) and, when `closed`, `region` (the analysis a region gets, over the rays inside the polygon: `center`, `depth`, `sky`, `objects`, `lines`, `edge`).
- `volumes[]`: `snip` and, for snips, `images` (§5.2).
- Objects of kind `shape` (§2.6) anywhere an object is named.

**`request.md`:** a *Sketches* section after *Screen regions*: per stroke `### S1 (red arrow)`, its points (first, last, count; closed or not), what its start and end hit, the world path (the hit samples' first, last and every 32nd point, with `request.json` holding them all), the objects along it by share, and for a closed stroke its region analysis as a region's. In *Volumes*, a snip volume says `3D snip: snip-2-view.png, snip-2-front.png, snip-2-side.png, snip-2-top.png (only what is inside V2)`.

**How the agent reads them** (`bundle-reference.md`, the `map-request` skill): a stroke is a gesture over `view.png`: a line or arrow along a surface ("a fence along here") follows its `samples`; an arrow points from its start to its end ("slopes down to there"); a closed stroke outlines an area (its `region`). Snip images show the target alone; the object list is the authority.

## 7. Streams

| Stream | Phases | Files |
|---|---|---|
| A: shapes runtime | 2 | `runtime/`, `src/export.c`, `src/dumpscene.c`, `src/trireport.c`, `Analyze` / classes / `EDIT_SHAPE_ADD` in `src/mapedit.c`, `tests/shapes/` |
| B: groups | 4 | `src/mapedit.c` (group ops), `src/editmode.c`, `src/transform.c`, `src/groupspanel.c/.h`, `src/main.c` (F6, `--groups-panel`), `src/keyspanel.c` |
| C: request tools | 1, 5, 6 | `src/designer.c/.h`, `src/launcher.c`, `src/classifier.c`, `src/main.c` (flags), stubs |
| D: shapes in the editor | 3 | `src/modelpicker.c`, `src/drawmode.c`, `src/drawpanel.c`, `src/paintunwrap.c`, `src/designer.c` (picking / outlining shapes), `src/launcher.c` (kind `shape`) |

Merge order: A, C, B, D. Implementation notes are appended below as streams merge.

## Implementation notes

### Stream A: shapes in the runtime (2026-11-27)

- **Meshes ("pieces").** Each kind is one unit mesh, except the capsule: its caps' height follows its size (min(sx, sz) / 2, at most sy / 2), which one scaled mesh can't do, so it is drawn as three pieces (side, top cap, bottom cap; the side left out when the caps meet), each instanced. The unit frame is centred on the origin (y −0.5..0.5; a cap 0..1 or −1..0; a plane at y 0), not "y 0 to 1" as §2.2 sketched: an instance is then placed at the box's centre and turned about it with the shape's rotation. The vertex data is built once per process into static arrays (a constant of the code, `pthread_once`), the GPU meshes once per scene when the first map with shapes loads (`MS_Private`), so a map without shapes uploads and draws nothing new.
- **Draws.** One instance buffer per (piece, material), clustered per 32 m cell like the boxes, drawn after the box and model ranges; a painted material's groups carry `INSTANCE_CUSTOM` = (first paint key, 2) on every instance, so the unchanged paint stage reads the part from the vertex. Shapes are not in the render inspector's draw ranges (`MS_DrawRangeCount`) nor in the occlusion volume (boxes only); every other cache sees them as any draw.
- **Records.** `MS_Shape.box` is the collider's AABB (a plane's reaches 0.01 m below it); the collider's exact shape is kept per shape (`shapeHull`). Interface addition: `int MS_ShapeTriangles(const MS_Scene *, int shape)` (for `--tri-report`).
- **Compiled record.** Section 22 stores the box as written (`u32 kind; f32 min[3], max[3], rotationDeg[3]; CmRef`, 68 bytes) instead of centre and size, so the reader's rebuilt floats are the parser's bit for bit (as barriers). `CM_VERSION` 15.
- **Paint.** Part names are read against one table (`PAINT_NAMES`: the box faces, then `surface side top bottom`) and resolved per entity once its kind is known. Shape parts get no paint-grid cells in the runtime (a cube's parts too: they map like model slots); §2.5a's "a cube's parts follow the box rules (cells)" is open for stream D.
- **Edits.** A turn of a shape line keeps its `rot` unless the result is no rotation (`BoxTurn`'s `keepRot`; the block rule of permuting sizes on quarter turns would change a cylinder's or cone's look); a plane refuses a `size` with a height. A map of only shapes is no longer "empty" (the message is now `map is empty (no boxes, models or shapes)`).
- **Tests.** `tests/shapes` (every kind plain and turned, four material kinds, paint on every kind), `./build.sh test compiled` runs it (section 22, paint kind 2, collision and ray hits in the game runtime), `test parse_error` has the shape and part-name errors, `build/tools/leakcheck` shoots, compiles and live-edits it.

### Stream B: groups as units (§3)

**Built:** the five ops and their script commands in `src/mapedit.c` (region *Building a patch*, section *Groups as units*: `GroupRename`, `GroupDelete`, `GroupDuplicate`, `RegroupLines` for new / move; reference [code/mapedit.md](code/mapedit.md#groups)); the whole-group R / Shift+R / Ctrl+D in `src/editmode.c` (`WholeGroups`, `GroupTurn`, `EditModeDuplicateGroups`); the groups panel `src/groupspanel.c/.h` ([code/groupspanel.md](code/groupspanel.md)), wired in `src/main.c` (F6, `--groups-panel [filter]`); rows in `src/keyspanel.c`; leak-check runs in `tools/leakcheck.c` (`edit-script-groups`, `groups-panel`). `Analyze` and `EditObjClass` are untouched (stream A's).

**Interface:** as the lead wrote it (`EDIT_GROUP_*`, `EditOp.groupName / groupNew`, `EditStep.groupName / groupNew`), plus one function in `editmode.h`, `EditModeDuplicateGroups(m, d, key, names, count)` (Ctrl+D on whole groups and the panel's Ctrl+D share it). No other header changed.

**How the ops run:** every group op goes through `MoveStep` (move-to-file's step with its self-check), which splits selected `repeat` copies out first for new / move, applies the edit live, and compares the map before and after entity by entity after `SnapExpect` has turned the map before into what the op means (rename: the group's entities in the new name; new / move: the moved entities in the target group; delete: the group's entities dropped, their paint left out; duplicate: new entities allowed only in the new group). The snapshot now hashes the group by name apart from the materials (`SnapEntity.group`), so move-to-file's check is the same as before. The ops by name ignore the selection (it rides along as the step's), and a rebase leaves out its gone objects instead of failing the step.

**Decisions (defaults, for the summary):**

- **A turn about a pivot takes lights along** (`LightTurn`): the existing `turn … pivot` refused any selection with a light, so `select group "plain: house"` + `turn 90 pivot …` (the house has 16 lights) could not work. A global turn with a pivot now orbits a light's position, turns a spot's / directional light's direction and turns its `clip` box (only in 90 degree steps about the axes, which keep it axis-aligned; other angles are refused for a light with a clip box). Every other turn of a light is refused as before. R on whole groups includes the groups' lights in the op (the designer can't select lights).
- **Whole-group selection** is per statement (§3.1); R turns exactly those statements. **Ctrl+D** copies by name, so it only takes the group path when every statement of each name is selected (a Ctrl+click on one of the nine `"plain: grass"` blocks duplicates that block's lines in place, as before); the copy of a group goes into one new block after the group's last block.
- **The new name** for Ctrl+D: `"<name> 2"`, or the next free number; a name that already ends in ` <number>` counts on from its base (`"plain: house 2"` → `"plain: house 5"` in hysteria, not `"plain: house 2 2"`).
- **Where lines go** in a group's last block (move, duplicate): after its last object line (not after trailing comments, `paint` blocks or `# barriers` lines that sit in the block), and the `use` in effect there is written back after the inserted lines when it differs, so the lines after keep their material. New groups (new, duplicate) start after a blank line.
- **group-delete** also removes a block's blank lines when nothing else of it is left (no double blank line, no empty `group` line), and the comment lines directly above any deleted line inside the block (so the header comments of a house go with it); comments above the `group` line itself stay.
- **The panel's keys in scripted shots:** unlike the id panel, `--input-script` drives the groups panel in the Linux editor too (`groupsKeys = !shotPath || scripted`), so its edits are tested end to end (with `CTRL+S` in the script they are saved to a scratch copy).
- **Not copied:** a duplicated group's paint entries (as `duplicate` never copied them).

**Measured** (this machine; `--edit-script` on a copy of `maps/hysteria`, 177 957 boxes; a timing of one run, not a benchmark): `select group "plain: house"` (1573 objects: 1557 boxes and 16 lights) + `move 10 0 -5`: `live edit 302.8 ms (parse 163.9, bvh 35.2, …, meshes 2.9 (38 of 519 rebuilt))`; `turn 90 pivot 27.375 8.375 151`: `273.3 ms (parse 146.7, bvh 33.8, …)`; rename 258.4 ms, duplicate 296.9 ms, delete 301.7 ms (each the whole map parsed again by `MS_EditSetText`; the self-check's two snapshots are not in the figure).

**Open:**

- A light group moved with **G** / scaled with **S** / turned with Alt+R or the gizmo leaves its lights behind (the designer can't select lights and the modal works on the selection); only R takes them along. Taking a whole group's lights into the modal is a small change in `transform.c` if wanted.
- Ctrl+click still selects one statement (§3.1's open point): for a repeated name the panel's Enter selects the whole name.
- `group-new` / `group-move` of a painted `repeat` copy is refused by the split (the existing move-to-file limit: the paint entry names a copy index the split renumbers).
- After group-move empties a block whose `group` line had a blank line after it, that blank line stays (only `group` / `use` lines go, move-to-file's rule).

### Stream C: world points, sketches (Phases 1 and 5)

- **World points** (`src/designer.c`, region "Volume mode"): `WorldPointOf` is the one rule (a surface hit snapped exactly as `VolumePoint` did, else the plane's crossing snapped in x and z, Ctrl exact); `VolumePoint` feeds it the last pick (`d->hit` with its ray, the new `Designer.pickDir`, set by `DesignerPickCamera` / `DesignerPickCursor`), so the edit mode's barrier clicks (cursor pick) and volume mode (crosshair) share it, and `DesignerWorldPoint` (for shape placement) casts its own ray through `cursor` of the window-sized frame (`PickShown`, so the barrier overlay counts as before). A surface click gives the bit-identical point: the same hit, the same snap code.
- **The plane's height** (`WorkPlaneY`): after the first corner that corner's; before it `workPlaneY` once set (PgUp / PgDn, `--work-plane`), else the last volume's base, else eye − `MS_DEFAULT_EYE_HEIGHT` on the snap grid. `Designer.workPlaneFixed` (new) keeps a `--work-plane` height when volume mode ends; a PgUp / PgDn height is forgotten then. Looking up at a plane below the eye (or a crossing past `WORK_PLANE_MAX_DIST`) gives no point: the click says *neither a surface nor the work plane under the crosshair*. So a sky click with the default plane (at the feet) only lands when the ray points down past the map's edge; with the plane raised above the eye (PgUp, `--work-plane`) sky clicks land on it.
- **PgUp / PgDn conflict** (a decision for the summary): the plane takes them while the next click would use it (sky under the crosshair, or Alt held), except Alt+PgUp / PgDn with an active volume, which keep growing its top / bottom as before. So with a volume active, Alt+click puts a corner on the plane but Alt+PgUp grows the volume; raise the plane with plain PgUp while looking at the sky, or select no volume.
- **Drawing**: `DrawWorkPlane`, live only (`DesignerDrawVolumes` with `marked` false): 9 × 9 grid lines of N steps around the point (segments fainter toward the edge; a faint pass through walls, a stronger depth-tested one), the corner cross, a tick down to `DesignerPick` straight below. The HUD's volume line adds `work plane y …` while the next click would use it; the help bars name Alt and PgUp / PgDn. `src/input.c` learned `PAGE_UP`, `PAGE_DOWN`, `HOME`, `END` for scripts.
- **Sketches** (`src/designer.c`, "Snip mode" / "Drawing"): the stroke being drawn is the last of `Designer.sketches[]` (no extra field); `PenInput` handles keys and drags; `SketchFinish` drops clicks (< 4 px), makes circles 64 rim points and sets `closed`. `DesignerAnalyzeSketch`: start / end picks, `count = clamp(ceil(len / 4 px) + 1, 2, 256)` samples at equal path lengths (a closed stroke's path returns to its start), each `DesignerPick`ed; objects tallied like a region's; a closed stroke runs the region analysis (`AnalyzeRegionIn`, `DesignerAnalyzeRegion` with a polygon: only rays inside it count; `DesignRegionInfo.samples` (new) holds that count, `gridW * gridH` for a region). Strokes are drawn with a 1 px dark rim, round joints, labels `S<n>` as `UiTag`s in the stroke colour; colours are module constants (`SKETCH_COLORS`; `DesignerSketchColor` / `DesignerSketchColorName` / `DesignerSketchKindName`, new in `designer.h`). `DesignerDrawSketchesAlone` blends alpha as `src + dst·(1 − src)` (`BLEND_CUSTOM_SEPARATE`) so `sketch.png` keeps the strokes' own alpha.
- **Bundle** (`src/launcher.c`): `LauncherSubmit` takes a fourth image, `sketch` (`Image{0}`: none). `MdRegionBody` and `JsonRegionAnalysis` are the region writers split out (a region's text and JSON are byte for byte what they were); `WriteSketchesMd` / `WriteSketchesJson` write §6; `images.sketch`; `volumes[].snip` (always false until Phase 6). The classifier's stdin gains `sketches: <n>` and `3D snips: <n>` (`ModelQuery` / `ClassifierQuery` `.sketches`, `.snips`); `tools/classifier-prompt.md` names them; the stubs and `request-queue` read none of those lines. `tools/test-coordinator.c` checks `"format": 9`.

### Stream C: 3D snips (Phase 6)

- **How the contents are cut** (the developer's choice: no R3D patch): per-pixel, app-side. `runtime/monolith_scene.c` region "3D snip clip" (editor only): `MS_SetClipBox(scene, box)` / `MS_ClearClipBox(scene)`. Each map material draws, while a box is set, with an alias of a *clip variant* of what it draws with: its surface shader, its paint version when painted, or the empty shader. The variant is `StageSource` (the paint stage's source rewriter, now parametrised by a `StageSpec`; `PaintShaderSource` is `StageSource(src, &PAINT_STAGE)` and gives the same bytes) with `CLIP_STAGE`: the source's `fragment()` renamed `ms_clipFragment` and called from a last `fragment()` that discards fragments whose world position (`POSITION`) is outside `ms_clipMin` / `ms_clipMax` (the box grown by 1 mm), except in the shadow variants (`DEPTH`, `DEPTH_CUBE`): what lies outside still casts its shadows in. The aliases get the material's uniforms, samplers, paint atlas and paint table, and its draw-sort key. Model groups get a copy of their slot materials with the clip aliases (map materials, painted own-material slots, and the empty shader's clip alias for slots drawn with the file's own material, so models without any surface shader are cut too). Built lazily on the first `MS_SetClipBox` of a scene version, freed with the scene (or rebuilt after an edit or reload). The depth pre-pass is off while a box is set (it writes depth before user code) and restored to `rs.depthPrepass` by Clear. Left over: a shader whose variant can't be made (more than 4 samplers, over R3D's 16 KB source limit, a compile error) draws its materials uncut, logged; stream A's shapes (drawn outside `DrawBoxRange` / `DrawModelRange`) need the same one-line swap; the edit preview (`PreviewDraw`) is not clipped (no preview runs during a submit).
- **Runtime lines touched** (all under `MONOLITH_EDITOR` except the `StageSource` refactor): `StageSpec` / `PAINT_STAGE` / `StageSource` replace the head of `PaintShaderSource` (paint region, ~25 lines); `MS_Private` gains `clip`, `clipOn`; `MS_Unload` calls `ClipFree`; the region "3D snip clip" (new, before `RangeFar`); `DrawBoxRange` (`mat` no longer const; one swap under `clipOn`) and `DrawModelRange` (one swap); `monolith_scene.h`: the two declarations after `MS_SetDrawFilter`.
- **The images** (`src/main.c` `RenderSnips`, called by `Submit` after `view-marked.png`): per snip volume the four cameras of `DesignerSnipViews` (fovy 40, 1024 × 768 for every box: the box's aspect is not used; distance = bounding-sphere radius / sin 20° × 1.02), rendered into one offscreen target with R3D's resolution set to 1024 × 768 and back. Sky: `background.sky` none, `background.color` 128 grey; **distance fog and volumetric fog off** (the lead's call, for clarity), set after `MS_PushLights` because a map's fog zones set the fog there; R3D's environment is restored byte for byte after each image (the model picker's pattern). Ambient light and the sun stay the map's.
- **Keys and flags**: X in volume mode (first step) toggles `DesignVolume.snip` of the active volume; snips draw dashed and are labelled `V<n> snip` (live and in `view-marked.png`); `--volume … snip`.
- **Bundle**: `snip-<n>-<side>.png`; `volumes[].snip`, `volumes[].images` (file, side, camera); a `3D snip` row in the volume's request.md table; the classifier's `3D snips: <n>`.
- **Cost**: the first snip after a load compiles one variant per distinct shader (monolith: 2.6 s with a cold driver cache in the first test, the rest of the submit unchanged); later snips about 15 ms per image (3 snips, 12 images in 183 ms on monolith).

### Stream D: shapes in the editor (Phase 3)

- **Objects** (`src/designer.c/.h`): shapes are selectable (`Selectable`, `SELECTABLE[4]`, `selPos[MS_OBJ_SHAPE + 1]`, `KindCount` / `KindRef`, `DesignerSync`'s geometry index per kind), outlined and tinted as their turned box (`ObjCorners`: always the `MS_TurnedBox`, so a plane is its flat rectangle, not the 1 cm collider), named in the HUD (`crosshair: … | shape cylinder | material … | shape N`, `selected: …, 2 shapes`) and by `--pick` (`pick: shape N kind cylinder …`, `pick: shape center … size … rot …`; the `pick: surface` line names the part). Ctrl+click, Shift+V and the edit mode's keys work through the existing paths (the classes are mapedit's `EDIT_CLASS_SHAPE`; the transform modal G / R / S on a shape was checked by an input script). The tie order of `DesignerPick` is `MS_Raycast`'s (box > shape > model).
- **Analyses:** a region's rays go through the region cull with a third candidate list (`RegionCull.shapes`, nearest first; `PickShapes`: a shape wins only strictly nearer than the best box, the lower index on a tie among shapes, models after: the rules of `MS_Raycast`), so a region over shapes counts them; a volume's object list and empty share take shapes by their AABB; sketches already pick through `MS_Raycast`. `request.md`'s *Nearby* counts shapes too.
- **Runtime (the one addition):** `MS_RaycastShape(scene, shape, origin, dir, maxDist, &hit)`, public (not editor-only): one shape by its triangles, the `ShapeRaycast` `MS_Raycast` uses. Needed by the region cull, which tests candidates one by one.
- **Bundle** (`src/launcher.c`): `ObjKindName` gives `shape`; `JsonShape` writes `"shape": { kind, center, size, rotationDeg }` wherever a model writes its `file` / `model` (hit, selection, region objects and edges, sketch objects, volume objects); `ObjTurn` gives a turned shape's `rotationDeg` like a turned box's. `request.md`: the hit's shape line (kind, centre, size, rotation), a shape line's selection table (kind, centre, size, rot, world bounds), the kind in backticks after `shape #n` in the region, sketch and volume tables, `N shapes` in the selection heading. No format bump (format 9 named the kind already). `.claude/skills/map-request/bundle-reference.md` describes the kind.
- **Placing** (`src/modelpicker.c`, `EditModeAddShape` in `src/editmode.c`): the picker's first six items are the kinds (thumbnails: **R3D's generated meshes** of each kind in the picker's studio, `RenderShapeThumb`, not the runtime's pieces); Enter places 1 × 1 × 1 m (plane 2 × 0 × 2) at `DesignerPickWorldPoint` (new in `designer.h`: `DesignerWorldPoint` along the last pick's ray and hit, no new cast; the edit mode picks the cursor every frame). **Decision:** on an upward face or the work plane the shape stands on the point; under a downward face (n.y < −0.5) it hangs from it; against a wall it is pushed out by half its footprint along the normal and centred on the point's height (the design said only "standing on"). It anchors after a box's / model's / shape's line; a barrier hit (overlay on) or the plane: the end of the main file (so after a paint block when the file ends with one: `ShapeAdd`'s rule). `ApplyTo` lets `EDIT_SHAPE_ADD` through without a selection. No work-plane grid is drawn while the picker is open (the plane's overlay stays volume mode's).
- **Paint** (`src/drawmode.c`, `src/drawpanel.c`, `src/mapedit.c`): see [code/drawmode.md](code/drawmode.md#shapes). mapedit's paint ops had box face names only: `PaintFacesText` / `ReadPaintLine` now write and read a shape's part names (`PaintShapeKind` by the statement's entity), `PaintObject` accepts shapes. Without that the first stroke on a sphere wrote `+x`, a parse error.
- **Not done / open:** the edit script's `paint-add … faces <…>` still reads box face names only (a cube works; the draw mode doesn't use the script path); a shape's brush cursor on a disc part draws past the disc's rim where the footprint does (the part UV square, not the disc); a stroke over a round side's seam is two runs (no join across the wrap).

