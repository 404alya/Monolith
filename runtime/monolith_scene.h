// monolith_scene -- load, hot-reload, draw, collide with and pick a monolith map package
// in any raylib + R3D program. C11; depends only on raylib (raymath, rlgl) and R3D.
//
// The monolith editor uses exactly this module, so a scene shows the same in the editor
// and in your game. Map format: docs/design-v2.md section 2 (and the header comment of
// monolith_scene.c). Compile with -DMS_PATCHED_R3D when building against monolith's R3D
// copy (submodules/r3d, see its patches.md); without it the runtime uses upstream R3D APIs
// only (camera-fitted sun shadow, no depth pre-pass toggle, no fog cache).
//
// Minimal use:
//     char err[512];
//     InitWindow(w, h, "game");
//     // optional: the map's shadow map size is an R3D init hint, so peek at it first
//     MS_Scene *peek = MS_Load("maps/forest", &(MS_LoadOptions){ .noGpu = true }, err, sizeof(err));
//     if (peek) { MS_SetInitHints(peek); MS_Unload(peek); }
//     R3D_Init(w, h);
//     MS_Scene *scene = MS_Load("maps/forest", NULL, err, sizeof(err));
//     if (!scene) { puts(err); return 1; }
//     MS_ApplyEnvironment(scene);
//     while (!WindowShouldClose()) {
//         if (MS_Poll(scene, GetFrameTime()) == MS_RELOADED) MS_ApplyEnvironment(scene);  // optional
//         BeginDrawing();
//             MS_Draw(scene, camera, (RenderTexture){ 0 });   // R3D_Begin .. R3D_End
//         EndDrawing();
//     }
//     MS_Unload(scene);
//
// Inside your own R3D frame instead (your objects and the scene together):
//     R3D_Begin(camera);
//         MS_PushLights(scene, camera, aspect);
//         MS_DrawGeometry(scene);
//         ... your R3D_Draw* calls ...
//     R3D_End();
//
// The map format reference is the header comment of monolith_scene.c (and docs/design-v2.md
// section 2). Scene contents are read-only for callers; everything the runtime owns (GPU
// resources, file text, caches) is freed by MS_Unload. One scene can be drawn at a time
// per R3D context (R3D's environment and fog bake are global).
#ifndef MONOLITH_SCENE_H
#define MONOLITH_SCENE_H

#include <r3d/r3d.h>
#include <raylib.h>
#include <stdbool.h>
#include <stdio.h>

#define MS_FORMAT_VERSION       2       // newest map format this runtime reads

#define MS_MAX_BOXES            8192
#define MS_MAX_GROUPS           256
#define MS_MAX_MATERIALS        64
#define MS_MAX_MODEL_FILES      64
#define MS_MAX_MODELS           4096    // model instances
#define MS_MAX_LIGHTS           64      // besides the sun
#define MS_MAX_VIEWPOINTS       32
#define MS_NAME_MAX             64      // group / material / viewpoint names, incl. the 0
#define MS_PATH_MAX             1024
#define MS_LINE_MAX             1024    // longest accepted map line (characters)
#define MS_DEFAULT_EYE_HEIGHT   1.62f   // eye above the spawn's feet (for the viewpoint made from the spawn)

// ======================================================================================
// Scene contents (read-only for callers; valid until the next MS_Poll / MS_Reload)
// ======================================================================================

typedef enum { MS_OBJ_NONE = 0, MS_OBJ_BOX, MS_OBJ_MODEL } MS_ObjKind;

// An object of the scene: a box (index into boxes[]) or a model instance (into models[])
typedef struct { MS_ObjKind kind; int index; } MS_ObjRef;

// Where an object came from, so tools can point at the map file
typedef struct {
    int line;               // 1-based source line in the map file, 0 = not from a file
    int prim;               // index of the statement (map line) that generated it, -1 = none
    int group;              // index into groups[], -1 = no group
    int material;           // index into materials[] (boxes: the material; models: the override, -1 = own)
} MS_Ref;

typedef struct {
    char name[MS_NAME_MAX];
    int line;               // first map line that defines it, 0 = built-in
    R3D_Material material;  // ready to draw (shader, textures loaded)
    char shader[MS_PATH_MAX];   // package-relative shader path, "" = none
} MS_Material;

typedef struct {
    char path[MS_PATH_MAX];     // package-relative
    R3D_Model model;
    BoundingBox bounds;         // model space
    int meshCount, triangleCount, materialCount;
} MS_ModelFile;

typedef struct {
    int file;                   // index into modelFiles[]
    Vector3 position;           // as written (after repeat/at), before `origin` adjustment
    Vector3 rotationDeg;        // yaw, pitch, roll
    Vector3 scale;
    Matrix transform;           // model -> world
    BoundingBox bounds;         // world AABB of the transformed model bounds
    int colliderFirst, colliderCount;   // range in colliders[] (0 count = collide none)
    bool castShadow;
    MS_Ref ref;
} MS_ModelInstance;

typedef struct {
    R3D_Light light;            // type, position/direction, colour, energy, range, ...
    bool shadow;
    MS_Ref ref;
} MS_LightDef;

typedef struct {
    char name[MS_NAME_MAX];
    Vector3 eye, target;
} MS_Viewpoint;

// The sun: the one directional light with the static shadow tiles
typedef struct {
    bool enabled, shadow;
    R3D_Light light;            // light.direction = direction the light travels; light.range = shadow distance
    float softness, depthBias, slopeBias;
    int shadowSize;             // px (env shadow.size, or --shadow)
} MS_Sun;

typedef struct MS_Private MS_Private;

typedef struct {
    int formatVersion;              // 1 or 2 (of the loaded file)
    char path[MS_PATH_MAX];         // the map file
    char root[MS_PATH_MAX];         // package root (folder of the map file), trailing '/'
    char brief[MS_PATH_MAX];        // root + "MAP.md" if it exists, else ""
    unsigned int version;           // +1 on every successful (re)load
    unsigned long long hash;        // FNV-1a 64 of the map file bytes
    long mtime;

    const BoundingBox *boxes;       // axis-aligned boxes
    const MS_Ref *boxRefs;
    int boxCount;

    const MS_ModelInstance *models;
    int modelCount;
    const MS_ModelFile *modelFiles;
    int modelFileCount;

    const BoundingBox *colliders;   // boxes first (same indices as boxes[]), then model colliders
    int colliderCount;

    const MS_Material *materials;   // [0] = "default"
    int materialCount;

    char (*groups)[MS_NAME_MAX];
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

    MS_Private *priv;               // GPU resources, caches, file text, hot-reload state
} MS_Scene;

// ======================================================================================
// Loading and hot reload
// ======================================================================================

typedef struct {
    bool noGpu;             // parse + geometry only, no R3D calls (e.g. a box dump without a window).
                            // Models are still read for their bounds where possible.
    int shadowSizeOverride; // > 0: overrides env shadow.size
    float shadowRangeOverride;  // > 0: overrides sun.range
} MS_LoadOptions;

// Loads a package folder or a .map file (docs/design-v2.md section 1). Needs R3D_Init first
// unless opt->noGpu. Returns NULL and "file:line: message" in err on failure.
MS_Scene *MS_Load(const char *path, const MS_LoadOptions *opt, char *err, int errSize);
void MS_Unload(MS_Scene *scene);

typedef enum {
    MS_UNCHANGED = 0,
    MS_RELOADED,            // new contents swapped in (version + 1): re-apply environment overrides
    MS_RELOAD_FAILED,       // a file changed but the package is invalid; old scene kept, see MS_Error
} MS_ReloadResult;

// Hot reload: call once per frame. About twice a second it checks every file the scene
// depends on (map, shaders, models, textures) and reloads the package when one changed.
MS_ReloadResult MS_Poll(MS_Scene *scene, float dt);

// Reloads now, from `path` (NULL = the current one). Same swap rules as MS_Poll.
MS_ReloadResult MS_Reload(MS_Scene *scene, const char *path, char *err, int errSize);

const char *MS_Error(const MS_Scene *scene);    // last failed (re)load, "" after a success

// R3D hints that only take effect before R3D_Init (today: the sun's shadow map size, from
// env shadow.size or MS_LoadOptions.shadowSizeOverride). Call it after InitWindow (R3D_SetHint
// queries GL) and before R3D_Init, e.g. with a noGpu scene. A reload that changes shadow.size
// keeps the old size until a restart.
void MS_SetInitHints(const MS_Scene *scene);

// Line `line` (1-based) of the loaded map's text, without the newline
bool MS_GetLine(const MS_Scene *scene, int line, char *out, int outSize);

// ======================================================================================
// Environment and drawing
// ======================================================================================

// Writes the scene's sky, ambient map, fog, volumetric fog, SSAO/SSIL/..., bloom, tonemap
// and colour settings into R3D_GetEnvironment(). Call after MS_Load and after a reload;
// apply your own overrides afterwards.
void MS_ApplyEnvironment(const MS_Scene *scene);

typedef enum {
    MS_SHADOW_TILES = 0,    // static world-fixed tiles, drawn once per map load (needs MS_PATCHED_R3D)
    MS_SHADOW_CAMERA,       // R3D's camera-fitted map, re-rendered when it no longer covers the view
    MS_SHADOW_ALWAYS,       // camera-fitted, re-rendered every frame
} MS_ShadowMode;

// Runtime switches that are not part of the map (the editor's toggles and A/B flags)
typedef struct {
    bool sunOn;             // default true
    MS_ShadowMode shadowMode;   // default MS_SHADOW_TILES (without MS_PATCHED_R3D: MS_SHADOW_CAMERA)
    float fogCacheVoxel;    // > 0: bake the fog's sun visibility (needs MS_PATCHED_R3D), 0 = off (default)
    int fogCacheSub;        // subsamples^3 per voxel, default 2
    bool depthPrepass;      // default true: R3D_SetDepthPrepass (needs MS_PATCHED_R3D; R3D-global)
} MS_RenderSettings;

MS_RenderSettings MS_GetRenderSettings(const MS_Scene *scene);
void MS_SetRenderSettings(MS_Scene *scene, MS_RenderSettings settings);

// A whole frame into `target` (zero = the current framebuffer): R3D_BeginPro, lights,
// geometry, R3D_End. Returns true when a sun shadow map was (re)rendered this frame.
bool MS_Draw(MS_Scene *scene, Camera3D camera, RenderTexture target);

// The parts, for use inside your own R3D_Begin*/R3D_End. aspect = target width / height.
bool MS_PushLights(MS_Scene *scene, Camera3D camera, float aspect);
void MS_DrawGeometry(MS_Scene *scene);

// Writes the scene's environment as format-2 `env` lines: every R3D_Environment field,
// sky.*, the ambientSky.* keys that are set, sun.*, shadow.size and the cache hints that are
// set. scene NULL = the built-in defaults (docs/design-v2.md section 2.6). For --new-map
// and export. Values are written so that they read back as the same floats.
bool MS_WriteEnvironment(const MS_Scene *scene, FILE *f);

// ======================================================================================
// Queries
// ======================================================================================

typedef struct {
    MS_ObjRef obj;          // kind MS_OBJ_NONE = nothing hit
    Vector3 point, normal;  // normal: face normal (boxes: axis-aligned)
    float dist;
} MS_Hit;

// First object along the ray within maxDist (<= 0: unlimited). Boxes that contain the
// origin are skipped (so you can pick out of a box you are inside); models are tested
// against their bounds, then their triangles where available.
MS_Hit MS_Raycast(const MS_Scene *scene, Vector3 origin, Vector3 dir, float maxDist);

// Strict overlap (touching is not a hit) of `box` with any collider
bool MS_Collides(const MS_Scene *scene, BoundingBox box);

const MS_Ref *MS_ObjectRef(const MS_Scene *scene, MS_ObjRef obj);     // NULL for MS_OBJ_NONE
BoundingBox MS_ObjectBounds(const MS_Scene *scene, MS_ObjRef obj);    // world AABB

#endif
