// monolith_scene -- load, draw, collide with and pick a monolith map package in any
// raylib + R3D program. C11; depends only on raylib (raymath, rlgl) and R3D.
//
// monolith draws its maps with this same module, so a scene looks the same there and in
// your game. It reads a map's compiled form, <name>.mapb (monolith's docs/compiled-map.md),
// which monolith writes from the map's text (docs/map-format.md). Compile with
// -DMS_PATCHED_R3D when building against monolith's patched R3D (see its patches.md);
// without it the runtime uses upstream R3D APIs only (camera-fitted sun shadow, no depth
// pre-pass toggle, no fog cache).
#ifdef MONOLITH_EDITOR
//
// Built with -DMONOLITH_EDITOR (the monolith editor, its tools and the examples), the module
// also has the map text's parser (the format: the header comment of monolith_scene.c), the
// compiler, hot reload, the map's source lines and the package / environment writers (the
// sections marked "editor"). --export writes the module without them.
#endif
//
// Minimal use:
//     InitWindow(w, h, "game");
//     // optional: the map's shadow map size is an R3D init hint, so peek at it first
//     MS_Scene *peek = MS_Load("maps/forest", &(MS_LoadOptions){ .noGpu = true });
//     if (peek) { MS_SetInitHints(peek); MS_Unload(peek); }
//     R3D_Init(w, h);
//     MS_InitRenderer();                 // GL state the scene needs (depth clamp for the sun shadow)
//     MS_Scene *scene = MS_Load("maps/forest", NULL);
//     if (!scene) { puts(MS_LastError()); MS_ClearError(); return 1; }
//     MS_ApplyEnvironment(scene);
//     while (!WindowShouldClose()) {
//         BeginDrawing();
//             MS_Draw(scene, camera, (RenderTexture){ 0 });   // R3D_Begin .. R3D_End
//         EndDrawing();
//     }
//     MS_Unload(scene);
//     MS_ClearError();                   // frees the last error message, if any
//
// Inside your own R3D frame instead (your objects and the scene together):
//     R3D_Begin(camera);
//         MS_PushLights(scene, camera, aspect);
//         MS_DrawGeometry(scene);
//         ... your R3D_Draw* calls ...
//     R3D_End();
//
// Scene contents are read-only for callers; everything the runtime owns (GPU resources,
// parsed data, caches) is freed by MS_Unload. One scene can be drawn at a time per R3D
// context (R3D's environment and fog bake are global).
#ifndef MONOLITH_SCENE_H
#define MONOLITH_SCENE_H

#include <r3d/r3d.h>
#include <raylib.h>
#include <stdbool.h>
#include <stdio.h>

#define MS_FORMAT_VERSION       3       // the map format this runtime reads (the only one)

// No content limits: boxes, models, groups, materials, lights, viewpoints, dependencies,
// names and map lines live in heap arrays sized to the map. The remaining caps only stop
// runaway input (a `repeat` grid of 100 M boxes, a 1 GB file) with a clear error:
#define MS_MAX_OBJECTS          1000000 // boxes + model instances + lights a map may make (and
                                        // model collision boxes, counted separately)
#define MS_MAX_FILE_BYTES       (4 * 1024 * 1024)   // map file (and a .gltf/.obj read for its references)
#define MS_PATH_MAX             1024    // longest path the loader builds (package root + relative path)
#define MS_MAX_COMPILED_BYTES   (256 * 1024 * 1024) // compiled map file (<name>.mapb)
#define MS_DEFAULT_EYE_HEIGHT   1.62f   // eye above the spawn's feet (for the viewpoint made from the spawn)

// ======================================================================================
// Scene contents (read-only for callers; valid until MS_Unload)
// ======================================================================================

typedef enum { MS_OBJ_NONE = 0, MS_OBJ_BOX, MS_OBJ_MODEL, MS_OBJ_LIGHT } MS_ObjKind;

// An object of the scene: a box (index into boxes[]), a model instance (into models[]) or a
// light (into lights[]; MS_Raycast never returns lights)
typedef struct { MS_ObjKind kind; int index; } MS_ObjRef;

// Where an object came from in the map file
typedef struct {
    int line;               // 1-based source line in the map file, 0 = not from a file
    int prim;               // index of the statement (map line) that generated it, -1 = none
    int group;              // index into groups[], -1 = no group
    int material;           // index into materials[] (boxes: the material; models: the override, -1 = own)
    int copy;               // which copy of the statement's `repeat` grid made it (0-based, first repeat
                            // outermost, counted from the line's first copy)
    int part;               // index among the objects that copy made (stairs steps, waffle pieces; else 0)
} MS_Ref;

// ======================================================================================
// Entities: every box, model instance and light, with its data attached as components
// ======================================================================================
// Entities are numbered 0..entityCount-1 in map order (one load; a reload renumbers them).
// What stays the same across loads is the MS_EntityId: the statement's key plus the object's
// index within that statement. The key is the line's `id N` when it has one (N, 1..2^31-1,
// bit 31 clear: stable while the line keeps its id), else an implicit key derived from the
// statement's text (bit 31 set: stable while that text, and the order of identical
// statements, stays the same; lines inserted, removed or moved elsewhere don't change it).
// Components are parallel arrays indexed by entity; new components are new arrays.

typedef struct {
    unsigned int stmt;      // `id N`, or an implicit key with bit 31 set
    int index;              // the object's index among the objects of its statement (map order)
} MS_EntityId;

#define MS_ENTITY_EXPLICIT(eid) (((eid).stmt & 0x80000000u) == 0)

typedef struct {
    Vector3 position;       // boxes: the centre; models: the position as written; lights: the position
    Vector3 rotationDeg;    // models and turned boxes: yaw, pitch, roll; other boxes and lights: 0
    Vector3 scale;          // boxes: the size (a turned box: along its own axes); models: the scale; lights: 1
} MS_Transform;

// A turned box (a `box` / `block` line with `rot`): the box before it turned, and its rotation
// about its centre. Its boxes[] entry (and colliders[] entry) is its world AABB.
typedef struct {
    Vector3 center;         // world
    Vector3 size;           // along its own axes (the extents as written)
    Vector3 rotationDeg;    // yaw, pitch, roll as written (R = Ry * Rx * Rz, about center)
    Vector3 axis[3];        // its own x, y, z axes in the world: the columns of R (unit)
} MS_TurnedBox;

typedef struct {
    int line, prim;         // map line and statement index (= MS_Ref)
    int copy, part;         // `repeat` copy and the object within it (= MS_Ref)
} MS_Source;

// Strings in scene contents (names, paths) are heap strings owned by the scene
typedef struct {
    const char *name;
    int line;               // first map line that defines it, 0 = built-in
    R3D_Material material;  // ready to draw (shader, textures loaded)
    const char *shader;     // package-relative shader path, NULL = none
    float drawDistance;     // m (`draw_distance`): 0 = automatic (from the size of its boxes),
                            // < 0 = `off` (always drawn); see MS_RenderSettings.detailDistance
} MS_Material;

// A model file (glTF/GLB, OBJ, FBX, DAE through R3D's importer), loaded once per map load
typedef struct {
    const char *path;           // package-relative
    R3D_Model model;            // GPU loads only (zero in noGpu scenes); its meshCount and materialCount
                                // are the file's mesh and material (slot) counts, so 0 in noGpu scenes
    bool loaded;                // false in noGpu scenes: the file was not read, bounds are a zero box
    BoundingBox bounds;         // model space (R3D's aabb: node transforms applied, no unit scale)
    int triangleCount;          // summed over the meshes (R3D doesn't store it), 0 in noGpu scenes
    int line;                   // first map line that uses it
} MS_ModelFile;

typedef enum { MS_ORIGIN_MODEL = 0, MS_ORIGIN_BOTTOM, MS_ORIGIN_CENTER } MS_ModelOrigin;
typedef enum { MS_COLLIDE_AABB = 0, MS_COLLIDE_NONE, MS_COLLIDE_BOXES } MS_ModelCollide;

// `material.N "name"`: slot N of the model's materials is drawn with materials[material]
typedef struct { int slot, material; } MS_SlotMaterial;

typedef struct {
    int file;                   // index into modelFiles[]
    Vector3 position;           // as written (after repeat), before `origin` adjustment
    Vector3 rotationDeg;        // yaw, pitch, roll (R = Ry * Rx * Rz: roll first, then pitch, then yaw)
    Vector3 scale;
    MS_ModelOrigin origin;
    Matrix transform;           // model -> world: scale, rotate, translate (includes the origin shift)
    Quaternion rotation;        // the rotation of transform
    Vector3 translation;        // the translation of transform (= position + origin shift)
    BoundingBox bounds;         // world AABB of the transformed vertices (noGpu: of the file's bounds)
    MS_ModelCollide collide;
    int colliderFirst, colliderCount;   // range in colliders[] (0 count = collide none)
    int slotFirst, slotCount;   // range in modelSlots[] (material.N overrides)
    bool castShadow;
    MS_Ref ref;                 // ref.material: `material "x"`, else the active `use`; -1 = the model's own
} MS_ModelInstance;

typedef struct {
    R3D_Light light;            // type, position/direction, colour, energy, range, ...
    bool shadow;
    MS_Ref ref;
    bool clip;                  // `clip x0 y0 z0 x1 y1 z1`: the light only reaches points in clipBox
    BoundingBox clipBox;        //   (world space; needs the patched R3D, ignored with upstream R3D)
} MS_LightDef;

typedef struct {
    const char *name;
    Vector3 eye, target;
} MS_Viewpoint;

// The sun: the one directional light with the static shadow tiles
typedef struct {
    bool enabled, shadow;
    R3D_Light light;            // light.direction = direction the light travels; light.range = shadow distance
    float softness, depthBias, slopeBias;
    int shadowSize;             // px (env shadow.size, or MS_LoadOptions.shadowSizeOverride)
} MS_Sun;

typedef struct MS_Private MS_Private;

typedef struct {
    int formatVersion;              // MS_FORMAT_VERSION (the only map format a scene loads from)
    const char *path;               // the map file
    const char *root;               // package root (folder of the map file), trailing '/'
    unsigned int version;           // 1 after MS_Load (the runtime's caches key on it)
#ifdef MONOLITH_EDITOR
    // editor: version is +1 on every successful reload, live edit (MS_EditSetText) and preview
    // step (MS_EditPreviewSet); loadVersion only on a load or reload from disk
    unsigned int loadVersion;
    const char *brief;              // root + "MAP.md" if it exists, else "" (never NULL)
    unsigned long long hash;        // FNV-1a 64 of the map file bytes
    long mtime;
#endif

    const BoundingBox *boxes;       // axis-aligned boxes (a turned box: its world AABB)
    const MS_Ref *boxRefs;
    int boxCount;
    const MS_TurnedBox *turnedBoxes;    // the boxes with a rotation
    int turnedBoxCount;
    const int *boxTurned;           // per box: index into turnedBoxes[], -1 = axis-aligned; NULL when
                                    // turnedBoxCount == 0 (every box is then exactly boxes[i])

    const MS_ModelInstance *models;
    int modelCount;
    const MS_ModelFile *modelFiles;
    int modelFileCount;
    const MS_SlotMaterial *modelSlots;
    int modelSlotCount;

    const BoundingBox *colliders;   // boxes first (same indices as boxes[]), then model colliders
    int colliderCount;

    const MS_Material *materials;   // [0] = "default"
    int materialCount;

    const char *const *groups;      // group names
    int groupCount;

    const MS_LightDef *lights;
    int lightCount;
    MS_Sun sun;

    bool hasSpawn;
    Vector3 spawnFeet;
    float spawnYawDeg, spawnPitchDeg;
    const MS_Viewpoint *viewpoints;
    int viewpointCount;

    BoundingBox bounds;             // everything (boxes, models, lights)

    // Entities (see "Entities" above): components indexed by entity, in map order
    int entityCount;                // = boxCount + modelCount + lightCount
    const MS_EntityId *entityIds;   // stable id
    const MS_ObjRef *entityObjects; // the box, model instance or light it is
    const MS_Transform *entityTransforms;
    const MS_Source *entitySources;
    const int *entityModels;        // model file (index into modelFiles[]), -1 for boxes and lights
    const int *boxEntities;         // object index -> entity, per kind
    const int *modelEntities;
    const int *lightEntities;

    MS_Private *priv;               // GPU resources, caches, parsed data
} MS_Scene;

// ======================================================================================
// Errors
// ======================================================================================
// A function that can fail says so by its return value (NULL, false, MS_RELOAD_FAILED) and
// leaves the reason in the last error: "file: message", or "file:line: message" when a map
// line is to blame. A message has no length limit: one that quotes a long path or lists many
// files is never cut.
//  - Each thread has its own last error.
//  - MS_Load sets it when it fails and clears it ("") when it succeeds.
#ifdef MONOLITH_EDITOR
//  - Editor builds: MS_Reload, MS_Compile, MS_EditSetText and MS_CheckModelReferences do the
//    same; MS_Poll sets it when a reload fails, clears it when one succeeds and leaves it alone
//    when it reloads nothing. The editor's own functions report through it too (MS_SetError)
//    and set it only when they fail.
#endif
//  - No other runtime function changes it.
//  - The pointer MS_LastError returns stays valid until the next MS_* call on the same thread:
//    copy the text (MS_StrDup) to keep it longer.
//  - Out of memory while a message is built: the message is "out of memory" (a static
//    string), never NULL and never half a message.

// The calling thread's last error; "" when there is none (never NULL)
const char *MS_LastError(void);

// Frees the calling thread's last error (MS_LastError() is "" after it). Call it at exit, and
// in a thread of your own before it ends if it called the runtime, so the message's memory is
// returned.
void MS_ClearError(void);

// printf-style argument checks where the compiler has them (MinGW: the archetype its own stdio
// declares, so %zu is checked like snprintf's)
#if defined(__MINGW32__) && defined(__MINGW_PRINTF_FORMAT)
#define MS_PRINTF_LIKE(f, a) __attribute__((format(__MINGW_PRINTF_FORMAT, f, a)))
#elif defined(__GNUC__) || defined(__clang__)
#define MS_PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#else
#define MS_PRINTF_LIKE(f, a)
#endif

#ifdef MONOLITH_EDITOR
// Editor: sets the calling thread's last error to a printf-style message, or appends one to
// it, so the editor's own functions report failures the same way as the runtime's. The
// arguments may point into MS_LastError() (e.g. to prefix it). Both return false, so a
// failing function can `return MS_SetError(...)`.
bool MS_SetError(const char *fmt, ...) MS_PRINTF_LIKE(1, 2);
bool MS_AppendError(const char *fmt, ...) MS_PRINTF_LIKE(1, 2);
#endif

// ======================================================================================
// Loading
// ======================================================================================

typedef struct {
    bool noGpu;             // parse + geometry only, no R3D calls (e.g. a peek before R3D_Init).
                            // Model files are parsed on the CPU (assimp, no GL) so a broken one
                            // fails the load like a GPU load would, then dropped; their bounds are
                            // unknown (zero box, loaded = false), so model bounds, colliders and
                            // origin bottom/center shifts are not final.
    int shadowSizeOverride; // > 0: overrides env shadow.size
    float shadowRangeOverride;  // > 0: overrides sun.range
#ifdef MONOLITH_EDITOR
    const char *shaderDefines;  // editor: put before the source of a surface shader (e.g. the
                                // "#define MS_BAKE ..." lines of --bake-surface); NULL = none
    const char *shaderDefinesFile;  // only this shader file (package-relative, as the map names
                                // it); NULL = every surface shader
#endif
} MS_LoadOptions;

// Loads a compiled map (<name>.mapb: the map's text compiled by monolith, read without parsing) or
// a package folder holding one (<name>/<name>.mapb, or its only .mapb file). Needs R3D_Init first
// unless opt->noGpu. Returns NULL on failure, the reason in MS_LastError(): "file: message" (a map
// line, when there is one, refers to the map's text: "file:line: message").
#ifdef MONOLITH_EDITOR
// Editor builds also read the map's text: a folder's <name>.map (else its only .map file) comes
// before a compiled map, and a file's first bytes tell the two apart.
#endif
MS_Scene *MS_Load(const char *path, const MS_LoadOptions *opt);
void MS_Unload(MS_Scene *scene);

// R3D hints that only take effect before R3D_Init (today: the sun's shadow map size, from
// env shadow.size or MS_LoadOptions.shadowSizeOverride). Call it after InitWindow (R3D_SetHint
// queries GL) and before R3D_Init, e.g. with a noGpu scene.
void MS_SetInitHints(const MS_Scene *scene);

// GL state the scene needs that R3D doesn't set. Call once right after R3D_Init (before or
// after MS_Load). Today: glEnable(GL_DEPTH_CLAMP), "shadow pancaking": the sun's shadow map
// only reaches a limited distance toward the sun, so parts of tall casters beyond its near
// plane would be clipped and their shadows pop as the camera turns; with depth clamp they
// are flattened onto the near plane instead (R3D never touches this state). Side effect:
// geometry closer than the camera's near plane (1 cm) is clamped rather than cut away.
void MS_InitRenderer(void);

#ifdef MONOLITH_EDITOR
// ======================================================================================
// Hot reload and source lines (editor)
// ======================================================================================
// Scene contents stay valid until the next MS_Poll / MS_Reload that swaps a new load in.

typedef enum {
    MS_UNCHANGED = 0,
    MS_RELOADED,            // new contents swapped in (version + 1): re-apply environment overrides
    MS_RELOAD_FAILED,       // a file changed but the package is invalid; old scene kept, see MS_Error
} MS_ReloadResult;

// Hot reload: call once per frame. About twice a second it checks every file the scene
// depends on (map, shaders, models, textures) and reloads the package when one changed.
// A reload that changes shadow.size keeps the old shadow map size until a restart.
// MS_RELOAD_FAILED: the reason in MS_LastError() and MS_Error(scene).
MS_ReloadResult MS_Poll(MS_Scene *scene, float dt);

// Reloads now, from `path` (NULL = the current one). Same swap rules as MS_Poll; the reason
// of a failure in MS_LastError().
MS_ReloadResult MS_Reload(MS_Scene *scene, const char *path);

// The scene's own copy of its last failed (re)load's message (MS_Poll, MS_Reload), "" after a
// successful one: unlike MS_LastError(), it stays until the scene reloads again. No length limit.
const char *MS_Error(const MS_Scene *scene);

// Line `line` (1-based) of the loaded map's text, without the newline (cut to outSize - 1 bytes)
bool MS_GetLine(const MS_Scene *scene, int line, char *out, int outSize);

// The same line without copying: a pointer into the scene's copy of the map file, *length
// bytes, not NUL-terminated (print it with "%.*s"); NULL if there is no such line. Valid
// until the next reload. Lines have no length limit.
const char *MS_LineText(const MS_Scene *scene, int line, int *length);

// The whole loaded map file (*size bytes, not NUL-terminated; its FNV-1a 64 is scene->hash).
// Valid until the next reload.
const char *MS_MapText(const MS_Scene *scene, int *size);

// The argument layout of a box primitive (box, block, stairs, waffle): returns the argument
// count and fills axes[0..count-1] with what `repeat` shifts: MS_AXIS_X/Y/Z, MS_AXIS_NONE
// (a size), MS_AXIS_INT (a count). -1 if `name` is not a box primitive. For tools that edit
// map lines.
enum { MS_AXIS_NONE = 0, MS_AXIS_X, MS_AXIS_Y, MS_AXIS_Z, MS_AXIS_INT };
int MS_PrimitiveArgs(const char *name, unsigned char axes[8]);

// ======================================================================================
// Compiled maps (editor; docs/compiled-map.md)
// ======================================================================================
// A compiled map has no map text: MS_GetLine, MS_LineText and MS_MapText return nothing for it.

// Compiles the map text at mapPath (a package folder or a .map file) into a compiled map at
// outPath: parsed, checked like a noGpu load (model files included), read back, then written
// (to outPath + ".partial", renamed). The same text always gives the same bytes. Paths in it
// stay relative to the package, so outPath belongs in the package folder (or in a copy of it).
// False with "file:line: message" in MS_LastError().
bool MS_Compile(const char *mapPath, const char *outPath);

typedef enum {
    MS_SOURCE_TEXT = 0,     // the scene was loaded from its map text
    MS_SOURCE_CURRENT,      // from a compiled map; the .map it was compiled from is next to it, unchanged
    MS_SOURCE_STALE,        // ... and has changed since (compile it again)
    MS_SOURCE_MISSING,      // ... and is not there
} MS_SourceState;
// sourcePath (may be NULL): the map text's path (for a text load, scene->path)
MS_SourceState MS_CompiledSource(const MS_Scene *scene, const char **sourcePath);

// ======================================================================================
// Live edits (editor; docs/live-editing.md)
// ======================================================================================
// The editor's working copy is map text. MS_EditSetText swaps in a new text without reading
// the package again: model files, textures, shaders, materials and the sky carry over, only
// the draws whose objects changed are rebuilt, and the static caches (sun shadow tiles, light
// shadows, fog cache, AO volume) redraw only where the changed objects reach. The result is
// what a load of the same text would give (pixel- and box-identical). Nothing is written.

typedef struct {
    bool full;                  // non-object statements changed: uploaded in full (a reload from memory)
    int boxesChanged, modelsChanged, lightsChanged;     // objects without an identical one before / after
    int rangesRebuilt, rangesKept;                      // merged box meshes
    int materialsRebuilt;       // materials appended, or changed only in uv_faces / filter / albedo_map
                                // or in their paint (built again while the others were kept;
                                // docs/painting.md 9.2)
    int instanceBuffersRebuilt; // instanced box buffers filled again (their boxes or paint keys changed)
    int paintTables;            // paint table uploads (0: the table is unchanged)
    int paintMeshFiles;         // model files whose paint meshes were built (docs/painting.md 4.5)
    int tilesDirty, lightShadowsDirty;                  // sun shadow tiles / light shadow maps to draw again
    bool fogDirty, aoDirty;                             // fog cache / AO volume to bake again (part or all)
    double ms;                  // the whole call
    double parseMs, modelsMs, bvhMs, entitiesMs, hiddenMs, meshesMs, uploadMs, aoMs;
} MS_EditStats;

// The scene becomes the parse of text[0..size) (copied). scene->hash = its hash, version + 1;
// loadVersion and the hot-reload state are unchanged (the files on disk are). MS_RELOADED on
// success (stats filled when not NULL), MS_RELOAD_FAILED with "file:line: message" in
// MS_LastError() and the scene unchanged otherwise. Ends a preview (MS_EditPreviewBegin) if one is running.
MS_ReloadResult MS_EditSetText(MS_Scene *scene, const char *text, int size, MS_EditStats *stats);

// The editor wrote the scene's current text (MS_MapText) to the map file: hot reload takes the
// files on disk as loaded, so MS_Poll does not reload them for that write
void MS_EditMarkSaved(MS_Scene *scene);

// Preview of a transform that follows the mouse (no text): Begin takes the objects out of the
// merged draws (they are drawn as instanced cubes / their own instances meanwhile), Set gives
// each one an absolute transform (MS_Transform as the entity components describe it: boxes
// centre + size + rotation (nonzero: drawn turned), models position / rotation / scale, lights
// position; version + 1, sun tiles
// and light shadows follow, fog cache and AO volume wait), End puts everything back as it was
// at Begin. Begin returns false when out of memory or a preview runs already.
bool MS_EditPreviewBegin(MS_Scene *scene, const MS_ObjRef *objs, int count);
bool MS_EditPreviewSet(MS_Scene *scene, const MS_Transform *transforms);
void MS_EditPreviewEnd(MS_Scene *scene);

// ======================================================================================
// Draw ranges and the draw filter (editor: the render inspector)
// ======================================================================================

typedef enum { MS_DRAW_BOXES_MERGED = 0, MS_DRAW_BOXES_INSTANCED, MS_DRAW_MODELS } MS_DrawKind;

typedef struct {
    MS_DrawKind kind;
    int material;               // boxes: index into materials[]; models: -1 (see slotMaterials)
    const char *shader;         // boxes: the material's shader path, NULL = none
    int modelFile;              // models: index into modelFiles[], else -1
    const int *slotMaterials;   // models: per material slot of the file, materials[] index or -1 (own)
    int slotCount;
    const int *objects;         // box indices (boxes) or model instance indices (models), draw order
    int objectCount;
    BoundingBox bounds;
    long long triangles;        // per pass (a merged mesh: its visible faces)
    bool castShadow;
    float drawDistance;         // m at detailDistance 1: farther from the camera than this (to its
                                // bounds) the camera skips it, its shadows stay; INFINITY = always
} MS_DrawRangeInfo;

// The runtime's draws in MS_DrawGeometry order: box ranges, then model ranges. Valid until the
// next version change.
int MS_DrawRangeCount(const MS_Scene *scene);
bool MS_GetDrawRange(const MS_Scene *scene, int range, MS_DrawRangeInfo *out);
// Triangles box `box` draws per pass: 2 per face no other box hides (a merged material), else 12;
// 0 without the GPU upload
int MS_BoxTriangles(const MS_Scene *scene, int box);
// hidden[r] true: range r is not drawn (camera and shadow passes). Copied; NULL / count 0 = off.
// A change redraws the sun shadow tiles and light shadows; fog cache and AO volume keep everything.
void MS_SetDrawFilter(MS_Scene *scene, const bool *hidden, int count);
#endif

// ======================================================================================
// Environment and drawing
// ======================================================================================

// Writes the scene's sky, ambient map, fog, volumetric fog, SSAO/SSIL/..., bloom, tonemap
// and colour settings into R3D_GetEnvironment(). Call after MS_Load; apply your own
// overrides afterwards.
void MS_ApplyEnvironment(const MS_Scene *scene);

typedef enum {
    MS_SHADOW_TILES = 0,    // static world-fixed tiles, drawn once per map load (needs MS_PATCHED_R3D)
    MS_SHADOW_CAMERA,       // R3D's camera-fitted map, re-rendered when it no longer covers the view
    MS_SHADOW_ALWAYS,       // camera-fitted, re-rendered every frame
} MS_ShadowMode;

// Render switches that are not part of the map. MS_ApplyQuality sets them from a quality
// preset; set them directly only for finer control.
typedef struct {
#ifdef MONOLITH_EDITOR
    bool sunOn;             // default true (editor: the sun toggle)
#endif
    MS_ShadowMode shadowMode;   // default MS_SHADOW_TILES (without MS_PATCHED_R3D: MS_SHADOW_CAMERA)
    float fogCacheVoxel;    // > 0: bake the fog's sun visibility (needs MS_PATCHED_R3D), 0 = off (default)
    int fogCacheSub;        // subsamples^3 per voxel, default 2
    bool depthPrepass;      // default true: R3D_SetDepthPrepass (needs MS_PATCHED_R3D; R3D-global)
    // Trade-offs (they change the image; the defaults keep the full-quality look):
    float renderScale;      // 1 (default): R3D renders at the output size. 0.25..1: at that fraction
                            // of it, upscaled with `upscale` (see MS_ApplyRenderResolution)
    R3D_UpscaleMode upscale;    // filter for renderScale < 1, default R3D_UPSCALE_BICUBIC
    float aoVolumeVoxel;    // > 0: bake a world-space occlusion volume of the boxes (voxel size in m,
                            // 0.25..4) once per map load and darken the ambient light with it
                            // (needs MS_PATCHED_R3D, patch 11); usually with SSAO + SSIL off,
                            // which it replaces. 0 = off (default)
    float aoVolumeIntensity;    // strength of that occlusion, default 1
    // Sun shadow overrides (quality settings; the defaults keep the map's):
    bool sunShadow;         // default true; false = the sun is pushed without a shadow map
    float shadowRange;      // > 0: shadow distance in m (10..2000) instead of the map's sun.range; 0 = the map's
    float shadowSoftness;   // > 0: PCF softness instead of the map's sun.softness; 0 = the map's
    // Draw distance of small objects (a trade-off, on by default): a draw (a material's boxes in
    // one map cell, a model file's instances in one cell) that is farther from the camera than
    // its draw distance x detailDistance is skipped by the camera and still casts its shadows.
    // The draw distance is the material's `draw_distance`, or automatic: MS_DETAIL_FACTOR x the
    // size of its largest object (the square root of the largest face of its bounds). Uses the
    // camera of the last MS_PushLights. 1 = default, 0 = draw everything.
    float detailDistance;
} MS_RenderSettings;

// The automatic draw distance per metre of object size (MS_RenderSettings.detailDistance)
#define MS_DETAIL_FACTOR 400.0f

MS_RenderSettings MS_GetRenderSettings(const MS_Scene *scene);
void MS_SetRenderSettings(MS_Scene *scene, MS_RenderSettings settings);

// ======================================================================================
// Graphics quality presets
// ======================================================================================
// The map stays the source of the look: every setting is relative to the map's environment
// (0 / 1 / "the map's" = unchanged), so MS_QUALITY_ULTRA renders exactly what the map
// describes (only small objects beyond their draw distance are skipped in every preset:
// detailDistance 1; 0 draws them), and an effect the map turns off stays off. Lower presets
// scale the cost down:
//     MS_Quality q = MS_QualityPreset(MS_QUALITY_MEDIUM);
//     MS_LoadOptions opt = { .shadowSizeOverride = q.shadowSize };  // the shadow map size is an
//     ... peek with opt + .noGpu, MS_SetInitHints(peek), R3D_Init, MS_InitRenderer ...  // init hint
//     MS_Scene *scene = MS_Load(path, &opt);
//     MS_ApplyEnvironment(scene);
//     MS_ApplyQuality(scene, &q);                  // right after every MS_ApplyEnvironment
//     MS_ApplyRenderResolution(scene, w, h);
// Changing shadowSize later needs R3D_Close / R3D_Init (and a new MS_Load); everything else
// applies live with MS_ApplyEnvironment + MS_ApplyQuality + MS_ApplyRenderResolution.

typedef enum { MS_QUALITY_LOW = 0, MS_QUALITY_MEDIUM, MS_QUALITY_HIGH, MS_QUALITY_ULTRA, MS_QUALITY_CUSTOM } MS_QualityLevel;

// Screen-space AO / indirect light: what the map sets up (full), less of it, or the baked volume
typedef enum {
    MS_AO_FULL = 0,         // the map's SSAO + SSIL
    MS_AO_SSAO,             // SSIL off
    MS_AO_LITE,             // SSIL off, SSAO 8 samples
    MS_AO_BAKED,            // SSAO + SSIL off, the baked occlusion volume instead (needs MS_PATCHED_R3D)
    MS_AO_OFF,              // SSAO + SSIL off
} MS_AoMode;

typedef struct {
    // Resolution and anti-aliasing
    float renderScale;              // 0.25..1 (MS_RenderSettings.renderScale)
    R3D_UpscaleMode upscale;
    R3D_AntiAliasingMode antiAliasing;  // default FXAA
    // Sun shadow
    bool sunShadow;
    int shadowSize;                 // px, 0 = the map's shadow.size (an R3D init hint, see above)
    float shadowRange;              // m, 0 = the map's sun.range
    float shadowSoftness;           // 0 = the map's sun.softness
    MS_ShadowMode shadowMode;
    // Ambient occlusion / indirect light
    MS_AoMode ao;
    float aoVolumeVoxel;            // m; with MS_AO_BAKED 0 = 0.5. > 0 with another mode: the volume on top
    float aoVolumeIntensity;        // with MS_AO_BAKED 0 = 0.6, else 0 = 1
    int ssaoSamples, ssilSamples;   // 0 = the map's
    float ssaoRadius, ssaoIntensity, ssilRadius;    // 0 = the map's
    // Fog
    bool volumetricFog;             // false turns the map's volumetric fog off
    float fogStepScale;             // x the map's march step (1 = the map's; 2 = half the steps)
    float fogLengthScale;           // x the map's march length (1 = the map's)
    float fogCacheVoxel;            // > 0: bake the fog's sun visibility (m), 0 = off
    int fogCacheSub;                // sub^3 samples per voxel (1..4)
    bool distanceFog;               // false turns the map's distance fog off
    // Post
    bool bloom;                     // false: bloom off, exposure x env bloom.offExposure (default 1.19)
    float bloomIntensityScale;      // x the map's bloom intensity
    int ssr, ssgi;                  // -1 off, 0 the map's, 1 on (with the map's parameters)
    // Display trims on top of the map's grading (multipliers, 1 = the map's)
    float exposure, brightness, contrast, saturation;
    // Performance (same image)
    bool depthPrepass;
    // Detail
    float detailDistance;           // x the draw distance of small objects (MS_RenderSettings), 0 = off
} MS_Quality;

// Low / medium / high / ultra (= the map exactly, but for detailDistance 1 in every preset);
// MS_QUALITY_CUSTOM returns ultra
MS_Quality MS_QualityPreset(MS_QualityLevel level);
const char *MS_QualityName(MS_QualityLevel level);  // "low" .. "ultra", "custom"
bool MS_QualityFromName(const char *name, MS_QualityLevel *level);
// The preset q equals field by field, else MS_QUALITY_CUSTOM
MS_QualityLevel MS_QualityMatch(const MS_Quality *q);
bool MS_QualityEqual(const MS_Quality *a, const MS_Quality *b);

// Applies q: the render settings (renderScale, upscale, sun shadow, shadow mode, fog cache,
// AO volume, pre-pass), R3D's anti-aliasing mode, and the environment part on
// top of R3D_GetEnvironment(). Call it right after MS_ApplyEnvironment (it scales the map's
// values, so applying it twice would scale twice), then MS_ApplyRenderResolution.
// q->shadowSize is not applied here (init hint: MS_LoadOptions.shadowSizeOverride).
void MS_ApplyQuality(MS_Scene *scene, const MS_Quality *q);

// R3D's internal resolution for an output of outW x outH pixels (the window, or the target
// passed to MS_Draw): renderScale x output (at least 1 px), plus the upscale filter. Only calls
// R3D when something changed. Call it after R3D_Init, when the output size changes and after
// changing renderScale / upscale (in place of R3D_SetResolution).
void MS_ApplyRenderResolution(const MS_Scene *scene, int outW, int outH);

// A whole frame into `target` (zero = the current framebuffer): R3D_BeginPro, lights,
// geometry, R3D_End. Returns true when a sun shadow map was (re)rendered this frame.
bool MS_Draw(MS_Scene *scene, Camera3D camera, RenderTexture target);

// The parts, for use inside your own R3D_Begin*/R3D_End. aspect = target width / height.
// MS_DrawGeometry submits R3D clusters (R3D_BeginCluster / R3D_EndCluster, one per map cell,
// so R3D frustum-culls the level): don't call it between your own BeginCluster and EndCluster.
bool MS_PushLights(MS_Scene *scene, Camera3D camera, float aspect);
void MS_DrawGeometry(MS_Scene *scene);

// ======================================================================================
// Queries
// ======================================================================================

typedef struct {
    MS_ObjRef obj;          // kind MS_OBJ_NONE = nothing hit
    Vector3 point, normal;  // normal: face normal (boxes: their face's, turned with a turned box)
    float dist;
} MS_Hit;

// First object along the ray within maxDist (<= 0: unlimited). Boxes that contain the
// origin are skipped (so you can pick out of a box you are inside); models are tested
// against their bounds, then their triangles where available (MS_RaycastModel). On equal
// distances the lower box index wins, and a box wins over a model.
MS_Hit MS_Raycast(const MS_Scene *scene, Vector3 origin, Vector3 dir, float maxDist);

// One model instance: its world bounds first, then its triangles (a BVH over a CPU copy of
// the mesh). Without triangle data (noGpu scenes) the bounds are hit like a box (an origin
// inside them is skipped). The normal faces the ray. False if missed or beyond maxDist
// (<= 0: unlimited).
bool MS_RaycastModel(const MS_Scene *scene, int model, Vector3 origin, Vector3 dir, float maxDist, MS_Hit *hit);

// Strict overlap (touching is not a hit) of `box` with any collider (a turned box: its turned
// shape, not its AABB)
bool MS_Collides(const MS_Scene *scene, BoundingBox box);
// The lowest index into scene->colliders that `box` strictly overlaps, -1 for none
int MS_FirstCollision(const MS_Scene *scene, BoundingBox box);

// Steepest face a player may stand and walk on (MS_ColliderTop's *walkable)
#define MS_WALKABLE_SLOPE_DEG   46.0f
// The top of collider c above the footprint (the x, z rectangle) of `box`: the collider's highest
// point over that rectangle (an axis-aligned collider: its max.y). *walkable (may be NULL): its
// most upward face is at most MS_WALKABLE_SLOPE_DEG steep (axis-aligned: true). For landing and
// stepping up after MS_FirstCollision.
float MS_ColliderTop(const MS_Scene *scene, int c, BoundingBox box, bool *walkable);
// The underside of collider c above that footprint: its lowest point there (axis-aligned: min.y)
float MS_ColliderBottom(const MS_Scene *scene, int c, BoundingBox box);

const MS_Ref *MS_ObjectRef(const MS_Scene *scene, MS_ObjRef obj);     // NULL for MS_OBJ_NONE
BoundingBox MS_ObjectBounds(const MS_Scene *scene, MS_ObjRef obj);    // world AABB (lights: their position)

// The entity of an object, -1 for none / out of range
int MS_ObjectEntity(const MS_Scene *scene, MS_ObjRef obj);
// The entity with this id in the loaded map, -1 if there is none (a hash lookup)
int MS_FindEntity(const MS_Scene *scene, MS_EntityId id);

#ifdef MONOLITH_EDITOR
// ======================================================================================
// Painting (editor: draw mode; docs/painting.md)
// ======================================================================================
// Where on a surface a ray hits (the texture coordinate the albedo map is sampled at, and the
// paint layer's coordinate), the triangles of a model's material slot, textures updated in place
// without a new scene version, and the paint layer: entries, atlases (provided from memory for
// paths that have no file yet) and model paint UVs.

typedef struct {
    MS_Hit hit;             // as MS_Raycast (the same object, point, normal and distance)
    int material;           // materials[] index the surface is drawn with; -1: a model slot's own material
    int face;               // boxes: 0..5 = +x -x +y -y +z -z (a turned box: its own axes); models: -1
    int slot;               // models: the file's material slot of the triangle; boxes: -1
    int mesh, triangle;     // models: the file's mesh and the triangle in it; boxes: -1
    Vector2 faceUv;         // boxes: where on the face, 0..1 (before uv_faces); models: the texcoord
    Vector2 uv;             // where the albedo map is sampled: after uv_faces, uv_scale and
                            // uv_offset, before the wrap
    Vector3 bary;           // models: barycentric coordinates of the hit in the triangle
    Vector2 paintUv;        // the paint layer's coordinate (docs/painting.md 4.4, 6): 0..1 in the face
                            // (boxes, oriented as R3D's unit cube) or in the slot's paint domain
                            // (models: own UVs or the unwrap); -1 -1: a slot that needs an unwrap
    int paintEntry;         // MS_PaintEntryAt index of the active entry of this face / slot in
                            // `material`'s atlas; -1: none yet
} MS_SurfaceHit;

// MS_Raycast, plus where on the surface it hit. False when nothing is hit (out->hit.obj.kind
// MS_OBJ_NONE). A model without texture coordinates is still hit, with uv 0 0 and slot set.
bool MS_RaycastSurface(const MS_Scene *scene, Vector3 origin, Vector3 dir, float maxDist, MS_SurfaceHit *out);

// The triangles of model file `file` drawn with its material slot `slot`: how many, then each
// one's three texture coordinates and model-space positions. False when out of range or the
// file has no texture coordinates.
int MS_ModelSlotTriangleCount(const MS_Scene *scene, int file, int slot);
bool MS_ModelSlotTriangle(const MS_Scene *scene, int file, int slot, int k, Vector2 uv[3], Vector3 pos[3]);
// k of MS_ModelSlotTriangle for triangle `triangle` of mesh `mesh` (MS_SurfaceHit's), in its slot
// (the mesh's material slot); -1 when out of range
int MS_ModelSlotTriangleIndex(const MS_Scene *scene, int file, int mesh, int triangle);

// Uploads rect (texels) of img (RGBA8, the texture's size) into every cached texture with this
// package path and regenerates their mipmaps; MS_TextureGeneration + 1 (scene->version is not
// changed). A material with alpha_cutoff > 0 or transparency that uses it also redraws the sun
// tiles and light shadows its objects reach. False if no texture of img's size has the path.
// (A provided image of another size than the cached texture makes the next live edit that loads
// the path load a texture of its size: an atlas that grew.)
bool MS_TextureUpdate(MS_Scene *scene, const char *rel, const Image *img, Rectangle rect);
// +1 on every MS_TextureUpdate (frame reuse keys on it)
unsigned int MS_TextureGeneration(const MS_Scene *scene);

// --- The paint layer (docs/painting.md 3-9). A material with paint keeps its shader, textures and
// draws; a last stage of its fragment shader composites its atlas over what it computed. Entries
// are the map's `paint` statements; atlases are package PNGs (straight alpha, RGBA8). A stroke is
// MS_TextureUpdate on the atlas path (premultiplied on upload).

// A paint entry (one `paint` statement), resolved against this load
typedef struct {
    int material;           // materials[] index; -1: a model slot's own material (atlas by file + slot)
    int line;               // map line of the statement
    MS_EntityId entity;
    MS_ObjRef obj;          // the box or model instance it resolved to (kind MS_OBJ_NONE: unresolved)
    unsigned char faces;    // boxes: bit f for each face +x -x +y -y +z -z sharing the rectangle; models: 0
    int slot;               // models: the slot; boxes: -1
    int x, y, w, h;         // the rectangle, texels of the atlas
    bool nearest;           // `paint own ... paint_filter nearest` (own materials only)
    bool active;            // drawn: the object draws those faces / that slot with the material, and
                            // the rectangle lies inside the atlas
} MS_PaintEntry;

int MS_PaintEntryCount(const MS_Scene *scene);
const MS_PaintEntry *MS_PaintEntryAt(const MS_Scene *scene, int i);    // NULL when out of range

// The atlas of a material (material >= 0), or of a model file's slot drawn with its own material
// (material -1, file and slot)
typedef struct {
    const char *rel;        // package path of the atlas PNG; NULL: the material has no `paint` key
    int width, height;      // of the loaded or provided image (0 x 0: none yet)
    bool nearest;           // paint_filter nearest
} MS_PaintAtlas;
bool MS_PaintAtlasOf(const MS_Scene *scene, int material, int file, int slot, MS_PaintAtlas *out);

// Straight-alpha pixels for an atlas path that the next MS_EditSetText (and any upload from
// memory) uses instead of the file, also while the file doesn't exist (a new or grown atlas).
// Copied (as RGBA8); img NULL forgets the path. Kept across live edits; dropped by a load or
// reload from disk and by MS_Unload. False when out of memory.
bool MS_PaintAtlasProvide(MS_Scene *scene, const char *rel, const Image *img);

// How model file `file`'s slot `slot` maps onto paint (docs/painting.md 4.5)
typedef enum {
    MS_PAINTUV_OWN = 0,         // its own UVs (all in 0..1, no overlap)
    MS_PAINTUV_UNWRAP,          // the file's unwrap (its .paintuv sidecar, or a provided one)
    MS_PAINTUV_NEEDS_UNWRAP,    // own UVs overlap, repeat or are missing, and there is no unwrap yet
    MS_PAINTUV_SKINNED,         // refused: the file is skinned (its bone channels are in use)
    MS_PAINTUV_INVALID,         // out of range or not loaded
} MS_PaintUvKind;
MS_PaintUvKind MS_ModelPaintUv(const MS_Scene *scene, int file, int slot);

// Triangle k of MS_ModelSlotTriangle with its paint UVs (own or unwrap) instead of its texture
// coordinates. False when out of range or the slot needs an unwrap.
bool MS_ModelSlotPaintTriangle(const MS_Scene *scene, int file, int slot, int k, Vector2 puv[3], Vector3 pos[3]);

// The mesh data of model file `file`'s mesh `mesh` as loaded (R3D's order; positions, texture
// coordinates, indices), for the editor's unwrap. False when out of range or not loaded.
bool MS_ModelMeshData(const MS_Scene *scene, int file, int mesh, R3D_MeshData *out);    // not owned

// An unwrap of a whole model file: the contents of its .paintuv sidecar (docs/painting.md 4.5), one
// entry per mesh of the file in R3D's order. A vertex of the unwrap is a copy of vertex source[k] of
// the mesh as loaded, with paint UV uv[2k], uv[2k + 1] (unorm16); triangles keep the mesh's order.
typedef struct {
    int slot, vertexCount, indexCount;
    unsigned int *source;
    unsigned short *uv;
    unsigned int *indices;
} MS_PaintUnwrapMesh;
typedef struct {
    int meshCount;
    MS_PaintUnwrapMesh *meshes;
    unsigned long long fileHash;    // FNV-1a 64 of the model file's bytes (a stale sidecar is ignored)
} MS_PaintUnwrap;
// The editor's unwrap of model file `file`, used by the next MS_EditSetText as if it were read
// from the sidecar. Copied; NULL forgets it. Same lifetime as MS_PaintAtlasProvide.
bool MS_PaintUnwrapProvide(MS_Scene *scene, int file, const MS_PaintUnwrap *u);
// The sidecar: written by the editor on save, read by the runtime at load (MS_PaintUnwrapFree it)
bool MS_PaintUnwrapWrite(const char *path, const MS_PaintUnwrap *u);
bool MS_PaintUnwrapRead(const char *path, MS_PaintUnwrap *out);
void MS_PaintUnwrapFree(MS_PaintUnwrap *u);
#endif

#ifdef MONOLITH_EDITOR
// ======================================================================================
// Package files and environment writers (editor: --export, --new-map, the capture shim)
// ======================================================================================

// Writes the scene's environment as format-2 `env` lines: every R3D_Environment field,
// sky.*, the ambientSky.* keys that are set, sun.*, shadow.size and the cache hints that are
// set. scene NULL = the built-in defaults (BuiltinEnvironment(), docs/map-format.md 2.5). For --new-map
// and export. Values are written so that they read back as the same floats.
bool MS_WriteEnvironment(const MS_Scene *scene, FILE *f);

// Files the loaded scene depends on, each once: [0] is the map file, then every shader,
// texture, panorama and model file a statement references, as scene->root + the
// package-relative path written in the map. These are the files hot reload watches.
// Files a model file pulls in by itself (a .gltf's .bin and textures) are not listed.
int MS_DependencyCount(const MS_Scene *scene);
const char *MS_Dependency(const MS_Scene *scene, int index);   // NULL when out of range

// Writes every R3D_Environment field of `env` as a format-2 `env` line (the same table and
// number formatting as MS_WriteEnvironment; no sky.*, ambientSky.*, sun.* or cache keys,
// which are not part of R3D_Environment). For tools that translate a running R3D program.
bool MS_WriteR3DEnvironment(const R3D_Environment *env, FILE *f);

// Checks the files a model file refers to (glTF buffers and images, OBJ material libraries
// and their texture maps) without importing it: each must stay inside `root` (a folder path
// ending in '/') and exist; references of 1024 bytes or more, control characters and
// absolute or URL references fail. modelRel: the model's path relative to root. False with
// the reason in MS_LastError(). The map loader runs this check for every model file of a map;
// --inspect-model runs it with the model's own folder as root before assimp reads anything.
bool MS_CheckModelReferences(const char *root, const char *modelRel);

// The range the map parser enforces for an `env` key (e.g. "volumetricFog.stepSize": >= 0.05):
// lo..hi, lo excluded when *loOpen. False if the key has no range. For tools that set
// environment values from elsewhere (monolith's --fx), so they can't do what a map can't.
bool MS_EnvKeyRange(const char *key, float *lo, float *hi, bool *loOpen);
#endif

// ======================================================================================
// Growable arrays and strings (plain heap memory: the runtime's own helpers, free to use;
// content-sized data is never a fixed array)
// ======================================================================================

// `arrayPtr` is the address of a `T *` with room for *cap elements of elemSize bytes. Makes
// room for at least `need` elements (the capacity doubles, at least 16; the new room is
// zeroed). False when out of memory or the size would overflow: the array is unchanged then.
bool MS_ArrayReserve(void *arrayPtr, int *cap, int need, size_t elemSize);
#define MS_RESERVE(arr, cap, need) ((need) <= (cap) || MS_ArrayReserve(&(arr), &(cap), (need), sizeof(*(arr))))
// Appends one zeroed element: a pointer to it (count + 1), or NULL when out of memory
#define MS_PUSH(arr, count, cap) (MS_RESERVE(arr, cap, (count) + 1) ? &(arr)[(count)++] : (void *)0)

// malloc'd copy of s (of its first n bytes for MS_StrNDup); NULL when out of memory
char *MS_StrDup(const char *s);
char *MS_StrNDup(const char *s, size_t n);

#endif
