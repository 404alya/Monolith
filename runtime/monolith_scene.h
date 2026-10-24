// monolith_scene -- load, draw, collide with and pick a monolith map package in any
// raylib + R3D program. C11; depends only on raylib (raymath, rlgl) and R3D.
//
// monolith draws its maps with this same module, so a scene looks the same there and in
// your game. Map format: the header comment of monolith_scene.c. Compile with
// -DMS_PATCHED_R3D when building against monolith's patched R3D (see its patches.md);
// without it the runtime uses upstream R3D APIs only (camera-fitted sun shadow, no depth
// pre-pass toggle, no fog cache).
#ifdef MONOLITH_EDITOR
//
// Built with -DMONOLITH_EDITOR (the monolith editor, its tools and the examples), the module
// also has hot reload, the map's source lines and the package / environment writers (the
// sections marked "editor"). --export writes the module without them.
#endif
//
// Minimal use:
//     char err[512];
//     InitWindow(w, h, "game");
//     // optional: the map's shadow map size is an R3D init hint, so peek at it first
//     MS_Scene *peek = MS_Load("maps/forest", &(MS_LoadOptions){ .noGpu = true }, err, sizeof(err));
//     if (peek) { MS_SetInitHints(peek); MS_Unload(peek); }
//     R3D_Init(w, h);
//     MS_InitRenderer();                 // GL state the scene needs (depth clamp for the sun shadow)
//     MS_Scene *scene = MS_Load("maps/forest", NULL, err, sizeof(err));
//     if (!scene) { puts(err); return 1; }
//     MS_ApplyEnvironment(scene);
//     while (!WindowShouldClose()) {
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
// Scene contents are read-only for callers; everything the runtime owns (GPU resources,
// parsed data, caches) is freed by MS_Unload. One scene can be drawn at a time per R3D
// context (R3D's environment and fog bake are global).
#ifndef MONOLITH_SCENE_H
#define MONOLITH_SCENE_H

#include <r3d/r3d.h>
#include <raylib.h>
#include <stdbool.h>
#include <stdio.h>

#define MS_FORMAT_VERSION       2       // newest map format this runtime reads

// No content limits: boxes, models, groups, materials, lights, viewpoints, dependencies,
// names and map lines live in heap arrays sized to the map. The remaining caps only stop
// runaway input (a `repeat` grid of 100 M boxes, a 1 GB file) with a clear error:
#define MS_MAX_OBJECTS          1000000 // boxes + model instances + lights a map may make (and
                                        // model collision boxes, counted separately)
#define MS_MAX_FILE_BYTES       (4 * 1024 * 1024)   // map file (and a .gltf/.obj read for its references)
#define MS_PATH_MAX             1024    // longest path the loader builds (package root + relative path)
#define MS_DEFAULT_EYE_HEIGHT   1.62f   // eye above the spawn's feet (for the viewpoint made from the spawn)

// ======================================================================================
// Scene contents (read-only for callers; valid until MS_Unload)
// ======================================================================================

typedef enum { MS_OBJ_NONE = 0, MS_OBJ_BOX, MS_OBJ_MODEL } MS_ObjKind;

// An object of the scene: a box (index into boxes[]) or a model instance (into models[])
typedef struct { MS_ObjKind kind; int index; } MS_ObjRef;

// Where an object came from in the map file
typedef struct {
    int line;               // 1-based source line in the map file, 0 = not from a file
    int prim;               // index of the statement (map line) that generated it, -1 = none
    int group;              // index into groups[], -1 = no group
    int material;           // index into materials[] (boxes: the material; models: the override, -1 = own)
} MS_Ref;

// Strings in scene contents (names, paths) are heap strings owned by the scene
typedef struct {
    const char *name;
    int line;               // first map line that defines it, 0 = built-in
    R3D_Material material;  // ready to draw (shader, textures loaded)
    const char *shader;     // package-relative shader path, NULL = none
} MS_Material;

// A model file (glTF/GLB, OBJ, FBX, DAE through R3D's importer), loaded once per map load
typedef struct {
    const char *path;           // package-relative
    R3D_Model model;            // GPU loads only (zero in noGpu scenes)
    bool loaded;                // false in noGpu scenes: the file was not read, bounds are a zero box
    BoundingBox bounds;         // model space (R3D's aabb: node transforms applied, no unit scale)
    int meshCount, triangleCount, materialCount;
    int line;                   // first map line that uses it
} MS_ModelFile;

typedef enum { MS_ORIGIN_MODEL = 0, MS_ORIGIN_BOTTOM, MS_ORIGIN_CENTER } MS_ModelOrigin;
typedef enum { MS_COLLIDE_AABB = 0, MS_COLLIDE_NONE, MS_COLLIDE_BOXES } MS_ModelCollide;

// `material.N "name"`: slot N of the model's materials is drawn with materials[material]
typedef struct { int slot, material; } MS_SlotMaterial;

typedef struct {
    int file;                   // index into modelFiles[]
    Vector3 position;           // as written (after repeat/at), before `origin` adjustment
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
    int formatVersion;              // 1 or 2 (of the loaded file)
    const char *path;               // the map file
    const char *root;               // package root (folder of the map file), trailing '/'
    unsigned int version;           // 1 after MS_Load (the runtime's caches key on it)
#ifdef MONOLITH_EDITOR
    // editor: version is +1 on every successful reload
    const char *brief;              // root + "MAP.md" if it exists, else "" (never NULL)
    unsigned long long hash;        // FNV-1a 64 of the map file bytes
    long mtime;
#endif

    const BoundingBox *boxes;       // axis-aligned boxes
    const MS_Ref *boxRefs;
    int boxCount;

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

    MS_Private *priv;               // GPU resources, caches, parsed data
} MS_Scene;

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
} MS_LoadOptions;

// Loads a package folder (<name>/<name>.map, or its only .map file) or a .map file. Needs
// R3D_Init first unless opt->noGpu. Returns NULL and "file:line: message" in err on failure.
MS_Scene *MS_Load(const char *path, const MS_LoadOptions *opt, char *err, int errSize);
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
MS_ReloadResult MS_Poll(MS_Scene *scene, float dt);

// Reloads now, from `path` (NULL = the current one). Same swap rules as MS_Poll.
MS_ReloadResult MS_Reload(MS_Scene *scene, const char *path, char *err, int errSize);

const char *MS_Error(const MS_Scene *scene);    // last failed (re)load, "" after a success

// Line `line` (1-based) of the loaded map's text, without the newline (cut to outSize - 1 bytes)
bool MS_GetLine(const MS_Scene *scene, int line, char *out, int outSize);

// The same line without copying: a pointer into the scene's copy of the map file, *length
// bytes, not NUL-terminated (print it with "%.*s"); NULL if there is no such line. Valid
// until the next reload. Lines have no length limit.
const char *MS_LineText(const MS_Scene *scene, int line, int *length);
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
} MS_RenderSettings;

MS_RenderSettings MS_GetRenderSettings(const MS_Scene *scene);
void MS_SetRenderSettings(MS_Scene *scene, MS_RenderSettings settings);

// ======================================================================================
// Graphics quality presets
// ======================================================================================
// The map stays the source of the look: every setting is relative to the map's environment
// (0 / 1 / "the map's" = unchanged), so MS_QUALITY_ULTRA renders exactly what the map
// describes, and an effect the map turns off stays off. Lower presets scale the cost down:
//     MS_Quality q = MS_QualityPreset(MS_QUALITY_MEDIUM);
//     MS_LoadOptions opt = { .shadowSizeOverride = q.shadowSize };  // the shadow map size is an
//     ... peek with opt + .noGpu, MS_SetInitHints(peek), R3D_Init, MS_InitRenderer ...  // init hint
//     MS_Scene *scene = MS_Load(path, &opt, err, sizeof(err));
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
} MS_Quality;

// Low / medium / high / ultra (= the map exactly); MS_QUALITY_CUSTOM returns ultra
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
    Vector3 point, normal;  // normal: face normal (boxes: axis-aligned)
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

// Strict overlap (touching is not a hit) of `box` with any collider
bool MS_Collides(const MS_Scene *scene, BoundingBox box);
// The lowest index into scene->colliders that `box` strictly overlaps, -1 for none
int MS_FirstCollision(const MS_Scene *scene, BoundingBox box);

const MS_Ref *MS_ObjectRef(const MS_Scene *scene, MS_ObjRef obj);     // NULL for MS_OBJ_NONE
BoundingBox MS_ObjectBounds(const MS_Scene *scene, MS_ObjRef obj);    // world AABB

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
// the reason in err. The map loader runs this check for every model file of a map;
// --inspect-model runs it with the model's own folder as root before assimp reads anything.
bool MS_CheckModelReferences(const char *root, const char *modelRel, char *err, int errSize);

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
