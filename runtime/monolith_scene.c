// monolith_scene -- map packages: load, draw, collide, pick (see monolith_scene.h)
//
// A program reads a map's compiled form, <name>.mapb: the map's text compiled by monolith into
// fixed-size records (region "Compiled maps"; monolith's docs/compiled-map.md), which this file
// checks completely before it uses them.
#ifdef MONOLITH_EDITOR
//
// Editor builds also parse the text itself (region "Parser") and write the compiled form.
//
// ======================================================================================
// Map format reference (.map), one statement per line
// ======================================================================================
//
//   # comment                      '#' starts a comment anywhere outside a "string"
//   monolith 2                     format version: the first statement of the file. Without it
//                                  the file is format 1 (only the v1 statements marked [1]
//                                  below; everything else uses the built-in defaults)
//   group "name"               [1] following primitives (boxes, lights, models) belong to this
//                                  group, until the next `group` (names: any length, no '"')
//
// Primitives (meters; +Y is up; numbers are plain decimals, e.g. 12, -4.5, 0.75):
//   box    x0 y0 z0 x1 y1 z1                    [1] two opposite corners, any order
//   block  cx y0 cz sx sy sz                    [1] centered on (cx, cz), from y0 up to y0 + sy
//   stairs cx baseY startZ width steps rise run [1] `steps` (integer) solid steps climbing
//                                                   toward -Z from startZ, each down to baseY
//   waffle x0 x1 y0 y1 z depth bayW floorH      [1] facade: back wall z-1..z, vertical fins
//                                                   every bayW along x, ledges every floorH,
//                                                   both sticking out from z to z + depth
//   light point x y z [key value ...]               point light (R3D omni light)
//   light spot  x y z dx dy dz [key value ...]      spot light shining along (dx, dy, dz)
//   light dir   dx dy dz [key value ...]            extra directional light (no static tiles)
//       keys: color R G B, energy f, range f, falloff f, specular f, fogEnergy f,
//             inner deg, outer deg (spot cone), shadow on|off (default off),
//             clip x0 y0 z0 x1 y1 z1 (world box, shifted like the position: the light reaches
//             only points inside it, so an unshadowed light stays out of the room below or
//             beside it; R3D patch 16, ignored with upstream R3D; default none)
//       defaults: white, energy 1, range 16 (dir: 50, its shadow distance), falloff 1, specular 1, fogEnergy 1, cone 22.5/45
//   model "models/x.glb" x y z [key value ...]      a model file placed at x y z (glTF/GLB best;
//                                                   also OBJ, FBX, DAE through R3D's importer)
//       rot yawDeg pitchDeg rollDeg   yaw about +Y, pitch about +X, roll about +Z, applied roll,
//                                     then pitch, then yaw (R = Ry * Rx * Rz); yaw 90 turns the
//                                     model's +Z to +X (like the camera's yaw)
//       scale s | scale sx sy sz      uniform or per-axis, > 0 (files have no unit scale: a model
//                                     in cm needs scale 0.01). Lighting of per-axis scaled models
//                                     is approximate (R3D rotates instance normals but does not
//                                     re-scale them)
//       origin model|bottom|center    model (default): the file's origin goes to x y z; bottom:
//                                     the model stands on y (its lowest vertex after scale and
//                                     rotation), centred over x z (the centre of the file's
//                                     bounds, turned with it); the safe way to stand a model on
//                                     a surface; center: the same x z, the middle between its
//                                     lowest and highest vertex at y
//       material "name"               every material slot drawn with this map material
//       material.N "name"             slot N only (0-based, in the file's material order)
//                                     Precedence: material.N, then material, then the active
//                                     `use` (when it is not "default"), else the file's own.
//                                     An override material with `inherit albedo|normal|orm|
//                                     emission|all` keeps that texture of the slot's own material
//                                     (inherit emission also keeps its colour and energy, inherit
//                                     normal its scale); the override's colours and factors apply.
//       collide aabb|none|box x0 y0 z0 x1 y1 z1 [box ...]   aabb (default): the world bounds
//                                     (of the turned vertices) block the player; none; or any number of boxes in model space
//                                     (before scale/rotation), each turned into a world AABB
//                                     (e.g. a tree's trunk only)
//       shadow on|off                 casts the sun shadow (default on); always receives shadows
//     Each file is loaded once per map load (R3D_LoadModel) and drawn instanced per (file,
//     materials, shadow). The file's own materials keep its choices, also KHR_materials_unlit
//     (unlit: no light or shadow on it, e.g. Kenney's kits): override with a lit material to
//     light it. Animations and skins are ignored (static pose). Files a .gltf/.glb names
//     (buffers, images) and an .obj's material libraries and textures must be inside the
//     package.
//
// Placement prefixes, written before a primitive on the same line:
//   id n                           a stable name for the line's objects (an integer 1..2147483647,
//                                  unique in the file), written first: id 12 repeat 4 0 0 3 box ...
//                                  Changes nothing in the scene; it keys the entities (MS_EntityId)
//                                  so they keep their identity when the line itself changes
//   repeat n dx dy dz [from k] [1] n copies, copy i (0-based) shifted by i * (dx, dy, dz); up to 3
//                                  repeats on one line give a grid (the first is the outer loop,
//                                  so it decides the box order). `from k`: the copies are
//                                  i = k .. k+n-1 instead (the same floats as copies k.. of the
//                                  longer row, so a row split in two keeps its exact positions)
//   at x y z                   [1] shift by (x, y, z): the primitive's own coordinates are
//                                  relative to this point
// Shifts apply to the position arguments only (box: all six; block: cx y0 cz; stairs:
// cx baseY startZ; waffle: x0 x1 y0 y1 z; light, model: x y z, never a direction), never to sizes
// or counts. Each position is computed as (at + i*d1 + j*d2 + k*d3) + value, in float. With
// whole or half numbers the order doesn't matter; with values like 2.6 it can change the last
// bit of a float ((-36 + 4.5) + 2.6 vs -33.4 + 4.5). `at` exists to reproduce the original C
// loops, which computed (start + i*step) + 2.6. Example, 17 roof beams 4.5 m apart, each
// 2.6 m deep, the first one at z = -36:
//   repeat 17 0 0 4.5 at 0 0 -36 box -40 20 0 40 22 2.6
//
// Materials (format 2):
//   material "name" key value [key value ...]   define a material, or add keys to one (a long
//                                  definition may span several lines with the same name).
//                                  "default" always exists (plain PBR: albedo 180 180 180,
//                                  roughness 0.9, metalness 0) and may be redefined.
//   use "name"                     following boxes (and models) use this material, like `group`;
//                                  `use "default"` goes back. Must be defined before the `use`.
//   keys: shader "shaders/x.glsl"  R3D surface shader (compiled once per file; each material draws
//                                    with its own alias of it: its own uniforms and samplers)
//         uniform <name> f [f f f] float / vec2 / vec3 / vec4 uniform of that shader
//         sampler <name> "textures/x.png"   sampler2D uniform of that shader
//         data_sampler <name> "textures/x.png"   the same for a texture of data, not colour (e.g.
//                                    a baked tile read with texelFetch): linear, nearest, no mipmaps
//         albedo R G B [A]  albedo_map "p"  normal_map "p"  normal_scale f  orm_map "p"
//         occlusion f  roughness f  metalness f  specular f
//         emission R G B  emission_energy f  emission_map "p"
//         uv_scale u v  uv_offset u v  alpha_cutoff f (0 = never discards; allows the depth
//         pre-pass)  cull back|front|none  transparency off|prepass|alpha
//         blend alpha|additive|add_alpha|multiply|premultiplied  unlit on|off  priority n
//         inherit albedo|normal|orm|emission|all   (model overrides: keep the model's own map)
//         wrap repeat|clamp|mirror   wrapping of this material's textures and samplers
//                                    (default repeat, so uv_scale > 1 tiles; trilinear + mipmaps)
//         instanced on|off   [off] keep this material's boxes as instanced unit cubes. Off: boxes
//                                    are drawn as one mesh per cell of the faces no other box hides
//                                    (see UploadBoxes). On is for shaders that read INSTANCE_* or the
//                                    model-space POSITION in vertex(); a scan of the source turns it
//                                    on with a warning when it finds those
//   Colours are 0-255 sRGB. Unset keys keep R3D's defaults (R3D_MATERIAL_BASE).
//
// Environment (format 2):  env <key> <value...>
//   Every field of R3D_Environment, by its C name: background.color/energy/skyBlur/rotation
//   (x y z w), ambient.color/energy, ssao.*, ssil.*, ssgi.*, ssr.*, fog.*, volumetricFog.*,
//   dof.*, bloom.*, autoExposure.*, tonemap.*, color.* (table ENV_KEYS below). Enums by
//   lower-case name (fog.mode disabled|linear|exp2|exp; bloom.mode disabled|mix|additive|
//   screen; tonemap.mode linear|reinhard|filmic|aces|agx; dof.mode disabled|enabled),
//   booleans on|off, colours R G B [A].
//   sky.mode procedural|color|panorama, sky.size n, sky.panorama "textures/x.hdr", and the
//   R3D_ProceduralSky fields: sky.skyTopColor, sky.skyHorizonColor, sky.skyHorizonCurve,
//   sky.skyEnergy, sky.groundBottomColor, sky.groundHorizonColor, sky.groundHorizonCurve,
//   sky.groundEnergy, sky.sunDirection (default: sun.direction), sky.sunColor,
//   sky.sunSizeDeg, sky.sunCurve, sky.sunEnergy.
//   ambientSky.mode procedural|same|off, ambientSky.size n, and ambientSky.<sky field>: the
//   ambient map (IBL) comes from a copy of the visible sky with these keys changed
//   (procedural), from the visible sky itself (same), or there is none (off).
//   sun.enabled on|off, sun.direction x y z (the direction the light travels), sun.color,
//   sun.energy, sun.specular, sun.fogEnergy, sun.range (shadow distance), sun.shadow on|off,
//   sun.softness, sun.depthBias, sun.slopeBias; shadow.size n (px, read before R3D_Init:
//   MS_SetInitHints). The biases are for shadow.size: with a smaller shadow map (a quality
//   preset, MS_LoadOptions.shadowSizeOverride) they are scaled up by shadow.size / actual
//   size (the texel ratio).
//   volumetricFog.fadeStart f, volumetricFog.fadeEnd f: the volumetric fog's density fades
//   out by distance from the eye, 1 up to fadeStart, smoothstep to 0 at fadeEnd (fadeEnd >
//   fadeStart; default 0 0 = no fade, the fog stops hard at volumetricFog.length). Keeps the
//   near light shafts while far buildings stay clear. R3D patch 13: ignored without
//   MS_PATCHED_R3D (upstream R3D then renders the unfaded fog).
//   volumetricFog.fadeNear f: the density also fades in, smoothstep from 0 at the eye to full
//   at f m (default 0 = off; at most fadeStart when a fade-out is set, larger values are
//   clamped to it). Without it the sunlit fog around the eye veils the whole view, so the
//   scene looks brighter when the player stands in sunlight than in shadow. R3D patch 15.
//   bloom.offExposure f: exposure factor applied when a quality setting turns bloom off, so
//   the mean brightness stays (default 1.19, matched on the default map).
//   Cache hints (optional; derived from the map bounds without them, see "Caches"):
//   shadow.home x y z, fogCache.region x0 y0 z0 x1 y1 z1.
//   Every key has a built-in default (BuiltinEnvironment(): the values the default map was
//   built with), so a v1 map renders as before apart from its material. Numeric values
//   outside sane ranges (ENV_RANGES, e.g. volumetricFog.stepSize >= 0.05) and zero
//   directions are rejected.
//
// Player and cameras (format 2):
//   spawn x y z yawDeg [pitchDeg]          player start, feet position (default 0 0 0, yaw 0)
//   viewpoint "Name" ex ey ez tx ty tz     camera viewpoint (eye, target), in order 1..n;
//                                          without any, one viewpoint is made from the spawn
//
// Paths are relative to the package root (the folder of the map file). Absolute paths and
// ".." components are rejected, so a package can be copied anywhere. The files a model file
// itself refers to (glTF buffer / image URIs, OBJ mtllib, .mtl texture maps) must stay inside
// the package too (checked as assimp reads them: MS_CheckModelReferences).
// Limits: none on content (any number of objects, groups, materials, lights, viewpoints; lines
// and names of any length). Sanity caps against runaway input: MS_MAX_OBJECTS (1000000 boxes +
// models + lights; a `repeat` grid multiplying to more fails), MS_MAX_FILE_BYTES (4 MB file),
// spawn / viewpoint / light positions within +-1000000 m.
// Every box remembers the map line of the statement that made it (boxRefs[]).
// Every box, model and light is an entity (MS_Scene entity* arrays) with a stable id: the
// line's `id` (or a key derived from the statement's text) and its index within the line.
// Errors are reported as "file:line: message".
//
// ======================================================================================
#endif

#include "monolith_scene.h"
#include <r3d/r3d_texture.h>
#include <raymath.h>
#include <rlgl.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MS_FILE_MAX         MS_MAX_FILE_BYTES
#define MS_REPEATS_MAX      3       // `repeat` prefixes per line (the grammar: a 3D grid at most)
// Camera and light positions (spawn, viewpoint, light) stay within +-MS_COORD_LIMIT, so the sun
// shadow tiles' integer cell keys never overflow (QA2 F4)
#define MS_COORD_LIMIT      1e6f
// Boxes and model instances are drawn in clusters (R3D frustum-culls a cluster as a whole, and never
// culls an instanced draw outside one): one per grid cell of this size on the XZ plane, by centre
#define MS_CELL_SIZE        32.0f
// Models get coarser cells: their instances are few and cheap (a tree is ~100-200 triangles),
// so a cluster per 32 m cell cost more in draw calls than the culling saved (pine_forest)
#define MS_MODEL_CELL_SIZE  128.0f
#ifdef MONOLITH_EDITOR
#define MS_POLL_INTERVAL    0.5f
// After an mtime change, keep re-reading for this long: GetFileModTime has 1 s resolution,
// and editors that truncate + write (or rename) can leave an empty or partial file for a
// moment, then finish within the same second (same mtime).
#define MS_SETTLE_TIME      2.0f
#endif
#define SUN_TILE_COUNT      2       // = the 2 layers R3D allocates for directional shadows anyway
// The fog cache without a region hint: at most this many voxels (bytes of VRAM)
#define FOG_CACHE_BUDGET    (64.0 * 1024.0 * 1024.0)
// The occlusion volume (aoVolumeVoxel): at most this many voxels (bytes of VRAM), and the
// distance range it stores (the AO probes reach 3.2 m out; inside geometry only the first
// voxel matters, for the trilinear filter)
#define AO_VOLUME_BUDGET    (32.0 * 1024.0 * 1024.0)
#define AO_DIST_MIN         (-1.0f)
#define AO_DIST_MAX         4.0f

// ======================================================================================
// #region Types
// ======================================================================================

// Memory rule (.claude/skills/engine-code, "Memory rules"): everything sized by the map is a heap array grown with
// MS_RESERVE / MS_PUSH and every name or path stored per element is a heap string; all of it
// belongs to the SceneData of one load and is freed with it (FreeData). Fixed char arrays are
// left only for one-off values bounded by the loader (EnvDef.skyPanorama: a package path,
// <= MS_PATH_MAX) and stack scratch for formatting one path or message. Allocation happens
// only while a map loads; drawing, picking and collision never allocate.

typedef struct { char *name; int count; float v[4]; } MatUniform;
typedef struct { char *name; char *path; int line; bool data; } MatSampler;   // data: data_sampler (linear, nearest, no mipmaps)

enum { MAP_ALBEDO, MAP_NORMAL, MAP_ORM, MAP_EMISSION, MAP_COUNT };
enum { INHERIT_ALBEDO = 1, INHERIT_NORMAL = 2, INHERIT_ORM = 4, INHERIT_EMISSION = 8 };

// What a material line says beyond the plain R3D_Material values (files, uniforms)
typedef struct {
    char *maps[MAP_COUNT];              // package-relative, NULL = none
    MatUniform *uniforms;
    int uniformCount, uniformCap;
    MatSampler *samplers;
    int samplerCount, samplerCap;
    unsigned inherit;                   // INHERIT_* (for model overrides)
    int shaderLine, mapLine[MAP_COUNT];  // map lines of the keys, for load errors
    int wrap;                           // texture wrapping: 0 repeat (default), 1 clamp, 2 mirror
    bool instanced;                     // `instanced on`: boxes stay instanced cubes
    bool merge, occluder;               // set at upload: boxes become a visible-face mesh / may hide other boxes' faces
} MatDef;

typedef struct {
    bool enabled, shadow;
    Vector3 direction;                  // as written; the light gets it normalized
    Color color;
    float energy, specular, fogEnergy, range, softness, depthBias, slopeBias;
    int shadowSize;
    int fileShadowSize;                 // shadow.size as the map gives it (before any override)
} SunDef;

enum { SKY_PROCEDURAL, SKY_COLOR, SKY_PANORAMA };
enum { AMB_PROCEDURAL, AMB_SAME, AMB_OFF };

typedef struct {
    R3D_Environment env;                // sky / ambient map handles are set at upload
    int skyMode, skySize;
    char skyPanorama[MS_PATH_MAX];
    R3D_ProceduralSky sky;
    bool skySunDirSet;                  // else sky.sunDirection = sun.direction
    int ambientMode, ambientSize;
    R3D_ProceduralSky ambient;          // ambientSky.* values, only the fields in ambientSet
    unsigned ambientSet;                // bit i = SKY_KEYS[i] was set
    SunDef sun;
    float bloomOffExposure;             // exposure factor when a quality setting turns bloom off
    float vfogFadeStart, vfogFadeEnd;   // volumetric fog density fade (R3D patch 13); end <= 0: none
    float vfogFadeNear;                 // and fade-in from the eye (R3D patch 15); <= 0: none
    bool homeSet, fogRegionSet;
    Vector3 home;
    BoundingBox fogRegion;
} EnvDef;

typedef struct { char *path; bool isColor, data; int wrap; Texture2D tex; } TexEntry;

// A surface shader file of the map, compiled once: every material that uses it draws with an
// alias of it (its own uniforms and samplers, the shared program and shader variants)
typedef struct {
    const char *rel;                    // the materials' shader path (not owned)
    R3D_SurfaceShader *shader;          // never drawn with: aliases copy its default uniform values
    bool scanned, needsCube, cutout;    // ScanShader of its source (unreadable: both true)
} ShaderFile;

// Where a load's time went (ms), for the load log line (LogLoadTimes)
typedef struct {
    double parse, models, bvh, shaders, textures, hidden, meshes;
    int shaderLoads;                    // R3D_LoadSurfaceShader calls (compiled shaders)
} LoadTimes;

// Picking data of a model file: a CPU copy of its triangles (model space) and a BVH over them
typedef struct { Vector3 mn, mx; int left, first, count; } BvhNode;    // leaf: count > 0 (left = -1)
typedef struct {
    Vector3 *pos;
    int vertCount;              // pos[] entries (the vertices of the file's triangle meshes)
    unsigned int *tri;          // 3 vertex indices per triangle, in BVH order
    int triCount;
    BvhNode *nodes;
    int nodeCount;
} ModelPick;

// One draw: instances [first, first + count) of a buffer (group = the material for boxes, the
// ModelGroup for models), bounds = the union of their boxes. Clustered and instanced, except a
// merged box range (MatDef.merge): its draw is `mesh`, the visible faces of its boxes (empty: none)
typedef struct { int group, first, count; BoundingBox bounds; R3D_Mesh mesh; } DrawRange;

// An object to sort into cells: group, then big (wider than a cell in x or z: one cluster per
// group), then cell, then index (keeps map order within a cluster)
typedef struct { int group, big, cx, cz, index; BoundingBox bounds; } CellItem;

// One instanced draw: the model instances of one file with the same material set and shadow flag
typedef struct {
    int file;
    bool castShadow;
    int *slotMat;               // per material slot of the file: materials[] index, -1 = the file's own
    R3D_Model model;            // shallow copy of the file's model with its own materials[] (and meshes[] if shadow off)
    bool ownMeshes;
    R3D_InstanceBuffer inst;
    bool instLoaded;            // inst exists (a failed BuildModelGroups may stop before it)
    int count;
} ModelGroup;

// One parsed (and possibly uploaded) map
typedef struct {
    int formatVersion;
    char *path, *root;
    char *srcPath;                      // the map text the lines in messages refer to ("file:line: ...")
#ifdef MONOLITH_EDITOR
    char *brief;                        // "" when the package has no MAP.md
    unsigned long long hash;            // FNV-1a 64 of the map file
    long mtime;
    char *text;                         // the map file, for MS_GetLine (NULL for a compiled map)
    int textSize;
    int *lineStarts;                    // offset of each line in text (NULL: out of memory, MS_LineText walks the text)
    int lineCount;
    bool compiled;                      // loaded from a compiled map: its source's hash and size
    unsigned long long srcHash;
    long long srcSize;
#endif

    // Heap arrays: count = used, cap = allocated (MS_RESERVE / MS_PUSH)
    BoundingBox *boxes;
    MS_Ref *boxRefs;
    int boxCount, boxCap, boxRefCap;
    char **groups;
    int groupCount, groupCap;
    MS_Material *materials;
    MatDef *matDefs;                    // parallel to materials[]
    int materialCount, materialCap, matDefCap;
    int *matHash, matHashCap;           // material name -> index + 1 (open addressing, 0 = empty)
    MS_LightDef *lights;
    int lightCount, lightCap;
    MS_Viewpoint *viewpoints;
    int viewpointCount, viewpointCap;
    bool hasSpawn;
    Vector3 spawnFeet;
    float spawnYawDeg, spawnPitchDeg;
    EnvDef env;
    R3D_Light sunLight;
    BoundingBox bounds;

    // Models. While parsing, an instance's colliderFirst/Count is its range in localColliders
    // (collide box ...); ResolveModels turns it into its range in colliders[].
    MS_ModelFile *modelFiles;
    ModelPick *modelPick;               // parallel to modelFiles[]
    int modelFileCount, modelFileCap, modelPickCap;
    MS_ModelInstance *models;
    int modelCount, modelCap;
    BoundingBox *localColliders;        // model space
    int localColliderCount, localColliderCap;
    int modelColliderCount;             // world colliders the instances will need
    MS_SlotMaterial *modelSlots;
    int modelSlotCount, modelSlotCap;
    BoundingBox *colliders;             // boxes, then model colliders (only with models)
    int colliderCount;
    BvhNode *colliderNodes;             // BVH over the published colliders (MS_Raycast, MS_FirstCollision)
    int *colliderOrder;                 // collider indices in leaf order
    int colliderNodeCount;

    // Statements (lines that make objects), by MS_Ref.prim: their entity key, and a set of the
    // keys in use (open addressing: statement index + 1, 0 = empty) for ids and implicit keys
    unsigned int *stmtKeys;
    int *stmtLines;                     // map line of each (for the duplicate id message)
    int stmtCount, stmtCap, stmtLineCap;
    int *stmtSet;
    int stmtSetCap;                     // a power of two, at most half full
    // Entities (BuildEntities): components by entity, object -> entity per kind, and an id ->
    // entity hash (entity + 1, 0 = empty)
    MS_EntityId *entityIds;
    MS_ObjRef *entityObjects;
    MS_Transform *entityTransforms;
    MS_Source *entitySources;
    int *entityModels;
    int entityCount;
    int *objEntities;                   // boxes, then models, then lights
    int *entityHash;
    int entityHashCap;

#ifdef MONOLITH_EDITOR
    char **deps;                        // files it depends on; [0] = the map file
    int depCount, depCap;
#endif

    // GPU (when uploaded)
    bool gpu;
    R3D_InstanceBuffer *inst;           // per material (materialCount)
    int *instCount;
    DrawRange *boxRanges;               // the box draws: one cluster each
    int boxRangeCount, boxRangeCap;
    R3D_InstanceBuffer unit;            // one instance at the origin, scale 1: how merged meshes are drawn
    int boxMeshCount;                   // merged ranges that have a mesh
    long long boxTrisDrawn;             // triangles the boxes draw per pass (12 per instanced box)
    float mergeMs;                      // time the hidden-face search and the meshes took
    LoadTimes times;
    TexEntry *textures;
    int textureCount, textureCap;
    ShaderFile *shaderFiles;
    int shaderFileCount, shaderFileCap;
    R3D_Cubemap sky;
    R3D_AmbientMap ambientMap;
    R3D_ShadowMap *lightShadow;         // per light (lightCount)
    bool *lightShadowDrawn;
    ModelGroup *modelGroups;
    int modelGroupCount;
    DrawRange *modelRanges;
    int modelRangeCount, modelRangeCap;
#ifdef MONOLITH_EDITOR
    const char *shaderDefines, *shaderDefinesFile;  // MS_LoadOptions (not owned)
#endif
} SceneData;

typedef struct {
    bool valid;             // rendered for the current map version and settings
    int key[3];             // tile cell in light space
    Matrix viewProj;        // light-space matrix the tile is rendered (and sampled) with
    unsigned int lastUsed;  // for least-recently-used replacement
} SunShadowTile;

typedef struct {
    SunShadowTile tile[SUN_TILE_COUNT];
    int current;            // tile used last frame, -1 = none yet
    unsigned int useCount;
    unsigned int version;   // scene version the tiles were rendered for
    float range;            // settings the tiles were laid out for
    int mapSize;
    Vector3 home;
    int renders;            // tile renders so far (log)
} SunShadowTiles;

typedef struct {
    bool valid;
    Matrix viewProj;        // our copy of the light-space matrix R3D cached with the map
    float aspect;           // fit inputs that change the radius, i.e. the texel grid
    float range;
    unsigned int version;
} SunShadowCache;

struct MS_Private {
    SceneData *d;
    MS_LoadOptions opt;
    MS_RenderSettings rs;
#ifdef MONOLITH_EDITOR
    char error[512];

    // Hot reload: the files of the last load attempt and their mtimes, the combined hash
    // of the last attempt's contents
    char **watch;
    long *watchMtime;
    int watchCount, watchCap, watchMtimeCap;
    float pollTimer, settleTime;
    unsigned long long triedHash;
#endif

    // GPU state that lives as long as the scene (not per load)
    bool gpu;
    R3D_Mesh cube;
    bool sunShadowLoaded;
    R3D_ShadowMap sunShadow[SUN_TILE_COUNT];    // one per cached tile ([0] for the camera-fitted map)
    SunShadowTiles tiles;
    SunShadowCache camCache;
    unsigned int fogCacheVersion;               // scene version the fog volume was baked for, 0 = none
    unsigned int aoVersion;                     // scene version the occlusion volume was baked for, 0 = none
    bool warnedShadowSize;                      // the shadow.size vs. R3D hint warning, once
    unsigned int frameVersion;                  // scene version MS_PushLights last saw
    double frameStart;                          // time of its last call
    int firstFrames;                            // frames timed since the last load (first two)
    double firstFrameMs[2];
};

// #endregion

// ======================================================================================
// #region Small helpers: growable arrays, strings, hashing, files, paths
// ======================================================================================

bool MS_ArrayReserve(void *arrayPtr, int *cap, int need, size_t elemSize)
{
    if (need <= *cap) return true;
    if (need < 0 || elemSize == 0) return false;
    long long n = *cap > 0 ? *cap : 16;
    while (n < need) n *= 2;
    if (n > 0x7fffffff) n = 0x7fffffff;
    if ((unsigned long long)n > (unsigned long long)SIZE_MAX / elemSize) return false;
    void *old;
    memcpy(&old, arrayPtr, sizeof(old));        // (the T * as a void *, without type punning)
    void *p = realloc(old, (size_t)n * elemSize);
    if (!p) return false;
    memset((char *)p + (size_t)*cap * elemSize, 0, (size_t)(n - *cap) * elemSize);
    memcpy(arrayPtr, &p, sizeof(p));
    *cap = (int)n;
    return true;
}

char *MS_StrNDup(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

char *MS_StrDup(const char *s)
{
    return MS_StrNDup(s, strlen(s));
}

#ifdef MONOLITH_EDITOR
static unsigned long long Fnv1a64Update(unsigned long long h, const void *data, size_t size)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < size; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}
#define FNV_OFFSET 14695981039346656037ULL
#endif

// Reads a whole file of at most maxBytes (malloc'd, NUL-terminated). NULL with a message in err on failure.
static char *ReadWholeFile(const char *path, long maxBytes, int *size, char *err, int errSize)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errSize, "%s: cannot open file", path); return NULL; }
    char *buf = NULL;
    long len = -1;
    if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
    if (len < 0 || len > maxBytes || fseek(f, 0, SEEK_SET) != 0)
        snprintf(err, errSize, "%s: cannot read file (or larger than %ld MB)", path, maxBytes / (1024 * 1024));
    else if (!(buf = malloc(len + 1)))
        snprintf(err, errSize, "%s: out of memory", path);
    else if ((long)fread(buf, 1, len, f) != len) {
        snprintf(err, errSize, "%s: read error", path);
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) { buf[len] = '\0'; *size = (int)len; }
    return buf;
}

#ifdef MONOLITH_EDITOR
// Map text + the bytes of every other watched file: changes when any file of the package does
static unsigned long long CombinedHash(const char *text, int size, char *const *files, int count)
{
    unsigned long long h = Fnv1a64Update(FNV_OFFSET, text, (size_t)size);
    for (int i = 1; i < count; i++) {
        h = Fnv1a64Update(h, files[i], strlen(files[i]) + 1);
        FILE *f = fopen(files[i], "rb");
        if (!f) { h = Fnv1a64Update(h, "\1missing", 8); continue; }
        unsigned char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) h = Fnv1a64Update(h, buf, n);
        fclose(f);
    }
    return h;
}
#endif

// The map file of a package folder with extension `ext`: <dir>/<base><ext>, else the folder's only
// file with that extension. 1 = found (in out), 0 = none or several (*count of them, their names
// in `names`), -1 = path too long.
static int PackageFile(const char *dir, const char *base, const char *ext, char *out, int outSize, int *count,
                       char *names, int namesSize)
{
    char cand[MS_PATH_MAX + 80];
    snprintf(cand, sizeof(cand), "%s/%s%s", dir, base, ext);
    *count = 0;
    if (FileExists(cand)) return snprintf(out, outSize, "%s", cand) < outSize ? 1 : -1;
    FilePathList list = LoadDirectoryFilesEx(dir, ext, false);
    *count = (int)list.count;
    int found = list.count == 1 && snprintf(out, outSize, "%s", list.paths[0]) < outSize;
    int n = 0;
    if (namesSize > 0) names[0] = '\0';
    for (unsigned i = 0; i < list.count && n >= 0 && n < namesSize; i++)
        n += snprintf(names + n, namesSize - n, " %s", GetFileName(list.paths[i]));
    UnloadDirectoryFiles(list);
    return found;
}

// A package folder or a map file. A folder's map: editor builds read its text, <name>.map (else
// its only .map file), and a compiled map when there is no text; game builds read only compiled
// maps, <name>.mapb (else the folder's only .mapb file)
static bool ResolveMapPath(const char *in, char *out, int outSize, char *err, int errSize)
{
    if (!DirectoryExists(in)) {
        snprintf(out, outSize, "%s", in);
        return true;
    }
    char dir[MS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", in);
    size_t len = strlen(dir);
    while (len > 1 && (dir[len - 1] == '/' || dir[len - 1] == '\\')) dir[--len] = '\0';
    const char *base = dir;
    for (const char *c = dir; *c; c++)
        if (*c == '/' || *c == '\\') base = c + 1;
    char names[512];
    int count, found;
#ifdef MONOLITH_EDITOR
    found = PackageFile(dir, base, ".map", out, outSize, &count, names, sizeof(names));
    if (found == 1) return true;
    if (found == 0 && count == 0) {
        int compiledCount;
        found = PackageFile(dir, base, ".mapb", out, outSize, &compiledCount, names, sizeof(names));
        if (found == 1) return true;
    }
    if (found < 0) snprintf(err, errSize, "%s: path too long", dir);
    else snprintf(err, errSize, "%s: no %s.map in the package folder and %s%s", dir, base,
                  count ? "more than one other .map file:" : "no other .map file", count ? names : "");
#else
    found = PackageFile(dir, base, ".mapb", out, outSize, &count, names, sizeof(names));
    if (found == 1) return true;
    if (found < 0) snprintf(err, errSize, "%s: path too long", dir);
    else if (count == 0)
        snprintf(err, errSize, "%s: no %s.mapb in the package folder (this program reads compiled maps: compile the "
                 "map's text with monolith)", dir, base);
    else snprintf(err, errSize, "%s: no %s.mapb in the package folder and more than one other .mapb file:%s", dir, base, names);
#endif
    return false;
}

// A package-relative path that can't leave the package: not empty, not absolute, no drive letter,
// no '\\', no ".." component
static bool InsidePackage(const char *s)
{
    if (s[0] == '\0' || s[0] == '/' || s[0] == '\\' || (s[0] && s[1] == ':') || strchr(s, '\\')) return false;
    for (const char *c = s; *c; ) {
        const char *e = c;
        while (*e && *e != '/') e++;
        if (e - c == 2 && c[0] == '.' && c[1] == '.') return false;
        c = *e ? e + 1 : e;
    }
    return true;
}

// Package root: the folder of the map file, with a trailing '/'
static void RootOf(const char *mapPath, char *root, int size)
{
    int cut = -1;
    for (int i = 0; mapPath[i]; i++)
        if (mapPath[i] == '/' || mapPath[i] == '\\') cut = i;
    if (cut < 0) snprintf(root, size, "./");
    else snprintf(root, size, "%.*s/", cut, mapPath);
}

// #endregion

// ======================================================================================
// #region Key tables: env, sky, sun (one place for every key; used to parse and to write)
// ======================================================================================

enum {
    T_F, T_I, T_B, T_COLOR, T_V3, T_QUAT, T_DEG, T_BOX, T_PATH,
    T_FOG, T_BLOOM, T_TONEMAP, T_DOF, T_SKYMODE, T_AMBMODE,
};
enum { B_ENV, B_DEF };  // offset base: R3D_Environment or EnvDef

typedef struct { const char *key; unsigned char type, base; size_t off; } KeyDef;

#define KE(k, t, f) { k, t, B_ENV, offsetof(R3D_Environment, f) }
#define KD(k, t, f) { k, t, B_DEF, offsetof(EnvDef, f) }

// Every R3D_Environment field except the sky / ambient map handles (sky.*, ambientSky.*)
static const KeyDef ENV_KEYS[] = {
    KE("background.color", T_COLOR, background.color),
    KE("background.energy", T_F, background.energy),
    KE("background.skyBlur", T_F, background.skyBlur),
    KE("background.rotation", T_QUAT, background.rotation),
    KE("ambient.color", T_COLOR, ambient.color),
    KE("ambient.energy", T_F, ambient.energy),
    KE("ssao.enabled", T_B, ssao.enabled),
    KE("ssao.sampleCount", T_I, ssao.sampleCount),
    KE("ssao.intensity", T_F, ssao.intensity),
    KE("ssao.power", T_F, ssao.power),
    KE("ssao.radius", T_F, ssao.radius),
    KE("ssao.bias", T_F, ssao.bias),
    KE("ssil.enabled", T_B, ssil.enabled),
    KE("ssil.sampleCount", T_I, ssil.sampleCount),
    KE("ssil.giIntensity", T_F, ssil.giIntensity),
    KE("ssil.aoIntensity", T_F, ssil.aoIntensity),
    KE("ssil.aoPower", T_F, ssil.aoPower),
    KE("ssil.radius", T_F, ssil.radius),
    KE("ssil.bias", T_F, ssil.bias),
    KE("ssgi.enabled", T_B, ssgi.enabled),
    KE("ssgi.sliceCount", T_I, ssgi.sliceCount),
    KE("ssgi.edgeFade", T_F, ssgi.edgeFade),
    KE("ssgi.distanceFalloff", T_F, ssgi.distanceFalloff),
    KE("ssgi.normalRejection", T_F, ssgi.normalRejection),
    KE("ssgi.intensity", T_F, ssgi.intensity),
    KE("ssgi.denoiseSteps", T_I, ssgi.denoiseSteps),
    KE("ssr.enabled", T_B, ssr.enabled),
    KE("ssr.maxIterations", T_I, ssr.maxIterations),
    KE("ssr.thickness", T_F, ssr.thickness),
    KE("ssr.edgeFade", T_F, ssr.edgeFade),
    KE("fog.mode", T_FOG, fog.mode),
    KE("fog.color", T_COLOR, fog.color),
    KE("fog.start", T_F, fog.start),
    KE("fog.end", T_F, fog.end),
    KE("fog.density", T_F, fog.density),
    KE("fog.skyAffect", T_F, fog.skyAffect),
    KE("volumetricFog.enabled", T_B, volumetricFog.enabled),
    KE("volumetricFog.scatteringDensity", T_F, volumetricFog.scatteringDensity),
    KE("volumetricFog.absortionDensity", T_F, volumetricFog.absortionDensity),   // R3D's spelling
    KE("volumetricFog.scatteringColor", T_COLOR, volumetricFog.scatteringColor),
    KE("volumetricFog.anisotropy", T_F, volumetricFog.anisotropy),
    KE("volumetricFog.emissionColor", T_COLOR, volumetricFog.emissionColor),
    KE("volumetricFog.emissionEnergy", T_F, volumetricFog.emissionEnergy),
    KE("volumetricFog.skyAffect", T_F, volumetricFog.skyAffect),
    KE("volumetricFog.length", T_F, volumetricFog.length),
    KE("volumetricFog.stepSize", T_F, volumetricFog.stepSize),
    KE("dof.mode", T_DOF, dof.mode),
    KE("dof.focusPoint", T_F, dof.focusPoint),
    KE("dof.focusScale", T_F, dof.focusScale),
    KE("dof.nearScale", T_F, dof.nearScale),
    KE("dof.maxBlurSize", T_F, dof.maxBlurSize),
    KE("bloom.mode", T_BLOOM, bloom.mode),
    KE("bloom.levels", T_F, bloom.levels),
    KE("bloom.intensity", T_F, bloom.intensity),
    KE("bloom.threshold", T_F, bloom.threshold),
    KE("bloom.softThreshold", T_F, bloom.softThreshold),
    KE("bloom.filterRadius", T_F, bloom.filterRadius),
    KE("autoExposure.enabled", T_B, autoExposure.enabled),
    KE("autoExposure.minEV", T_F, autoExposure.minEV),
    KE("autoExposure.maxEV", T_F, autoExposure.maxEV),
    KE("autoExposure.exposureCompensation", T_F, autoExposure.exposureCompensation),
    KE("autoExposure.adaptationToBright", T_F, autoExposure.adaptationToBright),
    KE("autoExposure.adaptationToDark", T_F, autoExposure.adaptationToDark),
    KE("tonemap.mode", T_TONEMAP, tonemap.mode),
    KE("tonemap.exposure", T_F, tonemap.exposure),
    KE("tonemap.white", T_F, tonemap.white),
    KE("color.brightness", T_F, color.brightness),
    KE("color.contrast", T_F, color.contrast),
    KE("color.saturation", T_F, color.saturation),
};

// Sky, sun, shadow and cache keys (the procedural sky fields are in SKY_KEYS)
static const KeyDef DEF_KEYS[] = {
    KD("sky.mode", T_SKYMODE, skyMode),
    KD("sky.size", T_I, skySize),
    KD("sky.panorama", T_PATH, skyPanorama),
    KD("ambientSky.mode", T_AMBMODE, ambientMode),
    KD("ambientSky.size", T_I, ambientSize),
    KD("sun.enabled", T_B, sun.enabled),
    KD("sun.direction", T_V3, sun.direction),
    KD("sun.color", T_COLOR, sun.color),
    KD("sun.energy", T_F, sun.energy),
    KD("sun.specular", T_F, sun.specular),
    KD("sun.fogEnergy", T_F, sun.fogEnergy),
    KD("sun.range", T_F, sun.range),
    KD("sun.shadow", T_B, sun.shadow),
    KD("sun.softness", T_F, sun.softness),
    KD("sun.depthBias", T_F, sun.depthBias),
    KD("sun.slopeBias", T_F, sun.slopeBias),
    KD("shadow.size", T_I, sun.shadowSize),
    KD("bloom.offExposure", T_F, bloomOffExposure),
    KD("volumetricFog.fadeStart", T_F, vfogFadeStart),
    KD("volumetricFog.fadeEnd", T_F, vfogFadeEnd),
    KD("volumetricFog.fadeNear", T_F, vfogFadeNear),
    KD("shadow.home", T_V3, home),
    KD("fogCache.region", T_BOX, fogRegion),
};

// R3D_ProceduralSky fields, as sky.<name> and ambientSky.<name>
typedef struct { const char *key; unsigned char type; size_t off; } SkyKeyDef;
#define KS(k, t, f) { k, t, offsetof(R3D_ProceduralSky, f) }
static const SkyKeyDef SKY_KEYS[] = {
    KS("skyTopColor", T_COLOR, skyTopColor),
    KS("skyHorizonColor", T_COLOR, skyHorizonColor),
    KS("skyHorizonCurve", T_F, skyHorizonCurve),
    KS("skyEnergy", T_F, skyEnergy),
    KS("groundBottomColor", T_COLOR, groundBottomColor),
    KS("groundHorizonColor", T_COLOR, groundHorizonColor),
    KS("groundHorizonCurve", T_F, groundHorizonCurve),
    KS("groundEnergy", T_F, groundEnergy),
    KS("sunDirection", T_V3, sunDirection),
    KS("sunColor", T_COLOR, sunColor),
    KS("sunSizeDeg", T_DEG, sunSize),
    KS("sunCurve", T_F, sunCurve),
    KS("sunEnergy", T_F, sunEnergy),
};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

#ifdef MS_PATCHED_R3D   // the shadow tiles and the occlusion volume
// floorf(v) as an int, clamped (NaN -> 0): a camera or box far outside any sane map (e.g. a
// player falling for hours) must not overflow a cell index (float -> int out of range is UB)
static int CellKey(float v)
{
    float f = floorf(v);
    if (f != f) return 0;
    return f < -1e8f ? -100000000 : f > 1e8f ? 100000000 : (int)f;
}
#endif

// How many values each enum key type has (compiled maps store the index)
static const int ENUM_COUNT[] = { [T_FOG] = 4, [T_BLOOM] = 4, [T_TONEMAP] = 5, [T_DOF] = 2, [T_SKYMODE] = 3, [T_AMBMODE] = 3 };
#ifdef MONOLITH_EDITOR
// Their names in the map text
static const char *const FOG_NAMES[] = { "disabled", "linear", "exp2", "exp" };
static const char *const BLOOM_NAMES[] = { "disabled", "mix", "additive", "screen" };
static const char *const TONEMAP_NAMES[] = { "linear", "reinhard", "filmic", "aces", "agx" };
static const char *const DOF_NAMES[] = { "disabled", "enabled" };
static const char *const SKYMODE_NAMES[] = { "procedural", "color", "panorama" };
static const char *const AMBMODE_NAMES[] = { "procedural", "same", "off" };
_Static_assert(sizeof(FOG_NAMES) / sizeof(FOG_NAMES[0]) == 4 && sizeof(BLOOM_NAMES) / sizeof(BLOOM_NAMES[0]) == 4 &&
               sizeof(TONEMAP_NAMES) / sizeof(TONEMAP_NAMES[0]) == 5 && sizeof(DOF_NAMES) / sizeof(DOF_NAMES[0]) == 2 &&
               sizeof(SKYMODE_NAMES) / sizeof(SKYMODE_NAMES[0]) == 3 && sizeof(AMBMODE_NAMES) / sizeof(AMBMODE_NAMES[0]) == 3,
               "ENUM_COUNT must match the enum names");
#endif

static int SkyKeyIndex(const char *name)
{
    for (int i = 0; i < COUNT(SKY_KEYS); i++)
        if (strcmp(SKY_KEYS[i].key, name) == 0) return i;
    return -1;
}

static int TypeSize(int type)
{
    switch (type) {
        case T_COLOR: return sizeof(Color);
        case T_V3:    return sizeof(Vector3);
        case T_QUAT:  return sizeof(Quaternion);
        case T_BOX:   return sizeof(BoundingBox);
        case T_B:     return sizeof(bool);
        default:      return 4;     // float, int, enums
    }
}

// The built-in environment (docs/map-format.md 2.5): the look the default map was built
// with, which used to be hard-coded in SetupEnvironment() and main(). A map overrides
// any key with `env`; a v1 map gets exactly these.
static void BuiltinEnvironment(EnvDef *e)
{
    memset(e, 0, sizeof(*e));
    e->env = R3D_ENVIRONMENT_BASE;
    R3D_Environment *env = &e->env;
    env->ambient.energy = 0.9f;

    env->fog.mode = R3D_FOG_EXP2;
    env->fog.color = (Color){ 150, 148, 143, 255 };
    env->fog.density = 0.0085f;
    env->fog.skyAffect = 0.7f;

    env->volumetricFog.enabled = true;
    // Keep the density low: R3D's volumetric fog is lit only by lights, not by the sky, so
    // dense fog in shadow turns into a black curtain that hides everything behind it
    // (.claude/skills/lighting-fog-shadows/r3d-pitfalls.md 1). The emission stands in for the missing sky light.
    env->volumetricFog.scatteringDensity = 0.012f;
    env->volumetricFog.absortionDensity = 0.001f;
    env->volumetricFog.scatteringColor = (Color){ 255, 244, 228, 255 };
    env->volumetricFog.emissionColor = (Color){ 150, 148, 143, 255 };
    env->volumetricFog.emissionEnergy = 0.12f;
    env->volumetricFog.anisotropy = 0.4f;
    env->volumetricFog.length = 90.0f;
    env->volumetricFog.stepSize = 0.75f;

    env->ssao.enabled = true;
    env->ssao.radius = 1.2f;
    env->ssao.intensity = 1.2f;
    env->ssil.enabled = true;
    env->ssil.radius = 5.0f;
    env->ssil.giIntensity = 1.2f;

    env->bloom.mode = R3D_BLOOM_MIX;
    env->bloom.intensity = 0.04f;

    env->tonemap.mode = R3D_TONEMAP_AGX;
    env->tonemap.exposure = 0.9f;
    env->color.saturation = 0.8f;
    env->color.contrast = 1.05f;

    e->skyMode = SKY_PROCEDURAL;
    e->skySize = 1024;
    e->sky = R3D_PROCEDURAL_SKY_BASE;
    e->sky.skyTopColor        = (Color){ 120, 132, 145, 255 };
    e->sky.skyHorizonColor    = (Color){ 200, 196, 186, 255 };
    e->sky.groundBottomColor  = (Color){ 48, 46, 44, 255 };
    e->sky.groundHorizonColor = (Color){ 190, 186, 178, 255 };
    e->sky.sunColor           = (Color){ 255, 236, 205, 255 };
    e->sky.sunSize            = 2.5f * DEG2RAD;
    e->sky.sunEnergy          = 2.0f;

    // The ambient map comes from an ambient-only copy of the sky whose lower hemisphere
    // stands in for light bounced off the ground; with the visible sky's dark ground,
    // downward-facing surfaces got almost no ambient (.claude/skills/lighting-fog-shadows/r3d-pitfalls.md 5).
    e->ambientMode = AMB_PROCEDURAL;
    e->ambientSize = 256;
    e->ambient.groundBottomColor = (Color){ 120, 116, 108, 255 };
    e->ambient.groundHorizonCurve = 0.1f;
    e->ambientSet = (1u << SkyKeyIndex("groundBottomColor")) | (1u << SkyKeyIndex("groundHorizonCurve"));

    e->sun = (SunDef){
        .enabled = true, .shadow = true,
        .direction = { 0.55f, -0.78f, 0.30f },
        .color = { 255, 238, 214, 255 },
        .energy = 2.0f, .specular = 1.0f, .fogEnergy = 4.0f, .range = 100.0f,
        .softness = 2.0f, .depthBias = 0.0001f, .slopeBias = 0.0015f,   // biases: R3D's defaults
        .shadowSize = 8192,
    };
    // Bloom here is mostly a uniform ~20% lift (threshold 0, MIX 0.04): without it, exposure
    // x1.19 keeps the mean brightness (matched over the default map's 26 verification shots)
    e->bloomOffExposure = 1.19f;
}

#ifdef MONOLITH_EDITOR
// Shortest decimal that reads back as the same float
static void FormatFloat(char *buf, int size, float v)
{
    // without an exponent if possible (100, not 1e+02)
    for (int pass = 0; pass < 2; pass++)
        for (int p = 1; p <= 9; p++) {
            snprintf(buf, size, "%.*g", p, v);
            if (strtof(buf, NULL) == v && (pass == 1 || !strchr(buf, 'e'))) return;
        }
}

// A value as the map file writes it (MS_WriteEnvironment)
static int FormatValue(char *buf, int size, int type, const void *p)
{
    char a[32], b[32], c[32], d[32];
    switch (type) {
        case T_F: FormatFloat(buf, size, *(const float *)p); return 1;
        case T_DEG: {
            // shortest degrees value that reads back (deg * DEG2RAD, as ParseValue does) to the same radians
            float rad = *(const float *)p;
            for (int prec = 1; prec <= 9; prec++) {
                snprintf(buf, size, "%.*g", prec, rad * RAD2DEG);
                if (strtof(buf, NULL) * DEG2RAD == rad) break;
            }
            return 1;
        }
        case T_I: snprintf(buf, size, "%d", *(const int *)p); return 1;
        case T_B: snprintf(buf, size, "%s", *(const bool *)p ? "on" : "off"); return 1;
        case T_COLOR: {
            const Color *col = p;
            if (col->a == 255) snprintf(buf, size, "%d %d %d", col->r, col->g, col->b);
            else snprintf(buf, size, "%d %d %d %d", col->r, col->g, col->b, col->a);
            return 1;
        }
        case T_V3: {
            const Vector3 *v = p;
            FormatFloat(a, 32, v->x); FormatFloat(b, 32, v->y); FormatFloat(c, 32, v->z);
            snprintf(buf, size, "%s %s %s", a, b, c);
            return 1;
        }
        case T_QUAT: {
            const Quaternion *q = p;
            FormatFloat(a, 32, q->x); FormatFloat(b, 32, q->y); FormatFloat(c, 32, q->z); FormatFloat(d, 32, q->w);
            snprintf(buf, size, "%s %s %s %s", a, b, c, d);
            return 1;
        }
        case T_BOX: {
            const BoundingBox *bb = p;
            char e[32], f[32];
            FormatFloat(a, 32, bb->min.x); FormatFloat(b, 32, bb->min.y); FormatFloat(c, 32, bb->min.z);
            FormatFloat(d, 32, bb->max.x); FormatFloat(e, 32, bb->max.y); FormatFloat(f, 32, bb->max.z);
            snprintf(buf, size, "%s %s %s %s %s %s", a, b, c, d, e, f);
            return 1;
        }
        case T_PATH: snprintf(buf, size, "\"%s\"", (const char *)p); return 1;
        case T_FOG: snprintf(buf, size, "%s", FOG_NAMES[*(const int *)p & 3]); return 1;
        case T_BLOOM: snprintf(buf, size, "%s", BLOOM_NAMES[*(const int *)p & 3]); return 1;
        case T_TONEMAP: { int v = *(const int *)p; snprintf(buf, size, "%s", TONEMAP_NAMES[v >= 0 && v < 5 ? v : 0]); return 1; }
        case T_DOF: snprintf(buf, size, "%s", DOF_NAMES[*(const int *)p & 1]); return 1;
        case T_SKYMODE: snprintf(buf, size, "%s", SKYMODE_NAMES[*(const int *)p % 3]); return 1;
        case T_AMBMODE: snprintf(buf, size, "%s", AMBMODE_NAMES[*(const int *)p % 3]); return 1;
    }
    return 0;
}
#endif

// Allowed ranges of numeric env values. Values outside are rejected (file:line: message)
// rather than clamped: some make R3D's shaders loop forever or divide by zero (e.g.
// volumetricFog.stepSize 0 hangs the GPU in vfog_radiance.frag), the rest are typos.
typedef struct { const char *key; float lo, hi; bool loOpen; } EnvRange;
static const EnvRange ENV_RANGES[] = {
    { "ssao.sampleCount", 1, 128, false }, { "ssao.radius", 0, 1000, true }, { "ssao.intensity", 0, 100, false },
    { "ssao.power", 0, 100, false },
    { "ssil.sampleCount", 1, 128, false }, { "ssil.radius", 0, 1000, true }, { "ssil.giIntensity", 0, 100, false },
    { "ssil.aoIntensity", 0, 100, false }, { "ssil.aoPower", 0, 100, false },
    { "ssgi.sliceCount", 1, 64, false }, { "ssgi.denoiseSteps", 0, 64, false },
    { "ssr.maxIterations", 1, 4096, false },
    { "fog.start", 0, 1e9f, false }, { "fog.end", 0, 1e9f, false }, { "fog.density", 0, 1e6f, false },
    { "volumetricFog.scatteringDensity", 0, 1e6f, false }, { "volumetricFog.absortionDensity", 0, 1e6f, false },
    { "volumetricFog.anisotropy", -1, 1, false }, { "volumetricFog.emissionEnergy", 0, 1e6f, false },
    { "volumetricFog.length", 0, 10000, false }, { "volumetricFog.stepSize", 0.05f, 1000, false },
    { "dof.maxBlurSize", 0, 1000, false },
    { "bloom.levels", 0, 1, false }, { "bloom.intensity", 0, 1e6f, false }, { "bloom.filterRadius", 0, 100, false },
    { "tonemap.exposure", 0, 1e6f, false }, { "tonemap.white", 0, 1e6f, true },
    { "background.energy", 0, 1e6f, false }, { "ambient.energy", 0, 1e6f, false },
    { "sun.energy", 0, 1e6f, false }, { "sun.fogEnergy", 0, 1e6f, false }, { "sun.softness", 0, 1e6f, false },
    { "bloom.offExposure", 0, 100, true },
    { "volumetricFog.fadeStart", 0, 10000, false }, { "volumetricFog.fadeEnd", 0, 10000, false },
    { "volumetricFog.fadeNear", 0, 10000, false },
};

#ifdef MONOLITH_EDITOR
bool MS_EnvKeyRange(const char *key, float *lo, float *hi, bool *loOpen)
{
    for (int i = 0; i < COUNT(ENV_RANGES); i++) {
        if (strcmp(ENV_RANGES[i].key, key) != 0) continue;
        *lo = ENV_RANGES[i].lo;
        *hi = ENV_RANGES[i].hi;
        *loOpen = ENV_RANGES[i].loOpen;
        return true;
    }
    return false;
}
#endif

// The checks an env value must pass once read (`key` = its full name, p = the value of `type`):
// false with the reason in msg. One place for both the text parser and compiled maps.
static bool CheckEnvValue(const char *key, int type, const void *p, char *msg, int msgSize)
{
    int i = type == T_I ? *(const int *)p : 0;
    if (strcmp(key, "shadow.size") == 0 && (i < 256 || i > 16384)) {
        snprintf(msg, msgSize, "shadow.size must be 256..16384");
        return false;
    }
    if ((strcmp(key, "sky.size") == 0 || strcmp(key, "ambientSky.size") == 0) && (i < 16 || i > 4096)) {
        snprintf(msg, msgSize, "sky sizes must be 16..4096");
        return false;
    }
    if (strcmp(key, "sun.range") == 0 && !(*(const float *)p > 0.0f)) {
        snprintf(msg, msgSize, "sun.range must be > 0");
        return false;
    }
    // Directions (sun.direction, sky.sunDirection, ambientSky.sunDirection); shadow.home is a point
    if (type == T_V3 && strcmp(key, "shadow.home") != 0 && Vector3Length(*(const Vector3 *)p) == 0.0f) {
        snprintf(msg, msgSize, "%s must not be 0 0 0", key);
        return false;
    }
    for (int k = 0; k < COUNT(ENV_RANGES); k++) {
        const EnvRange *r = &ENV_RANGES[k];
        if (strcmp(r->key, key) != 0) continue;
        float v = type == T_I ? (float)*(const int *)p : *(const float *)p;
        if (v > r->lo && v <= r->hi) return true;
        if (v == r->lo && !r->loOpen) return true;
        char lo[32], hi[32];
#ifdef MONOLITH_EDITOR
        FormatFloat(lo, sizeof(lo), r->lo);
        FormatFloat(hi, sizeof(hi), r->hi);
#else
        snprintf(lo, sizeof(lo), "%g", r->lo);
        snprintf(hi, sizeof(hi), "%g", r->hi);
#endif
        snprintf(msg, msgSize, "%s must be %s %s and <= %s", key, r->loOpen ? ">" : ">=", lo, hi);
        return false;
    }
    return true;
}

// #endregion

// ======================================================================================
// #region Small helpers: statement keys
// ======================================================================================

// Statement keys (MS_EntityId.stmt): a set of the keys in use, by MS_Ref.prim (the parser and
// compiled maps keep them unique)
static unsigned int KeySlot(unsigned int key, int cap) { return (key * 2654435761u) & (unsigned int)(cap - 1); }

// The set slot holding `key`, or the empty slot where it would go
static int *StmtSlot(const SceneData *d, unsigned int key)
{
    unsigned int i = KeySlot(key, d->stmtSetCap);
    while (d->stmtSet[i] && d->stmtKeys[d->stmtSet[i] - 1] != key) i = (i + 1) & (unsigned int)(d->stmtSetCap - 1);
    return &d->stmtSet[i];
}

static bool GrowStmtSet(SceneData *d)
{
    int cap = d->stmtSetCap ? d->stmtSetCap * 2 : 64;
    int *set = calloc((size_t)cap, sizeof(int));
    if (!set) return false;
    free(d->stmtSet);
    d->stmtSet = set;
    d->stmtSetCap = cap;
    for (int s = 0; s < d->stmtCount; s++) *StmtSlot(d, d->stmtKeys[s]) = s + 1;
    return true;
}

// #endregion

// ======================================================================================
// #region Parser
// ======================================================================================
// The text parser exists only in editor builds: games read compiled maps (see "Compiled maps").
#ifdef MONOLITH_EDITOR

typedef struct {
    char *s;
    bool quoted;
} Token;

typedef struct {
    SceneData *d;
    const char *path;           // for messages
    int line;                   // 0 = no line in messages
    char *err;
    int errSize;
    MS_Ref cur;                 // stamped on every object (line, statement, group, material)
    int prim;                   // statement counter
    bool badNumber;             // the last failed Box() overflowed float, rather than the scene being full
    bool outOfMemory;           // the last failed Box() could not grow the arrays
    bool sawStatement;          // the version line must come first
    char *lineBuf;              // the current line (tokenized in place), grown to the longest line
    int lineCap;
    Token *tok;                 // its tokens, grown to the most words on a line
    int tokCap;
} Parser;

static bool Fail(Parser *P, const char *fmt, const char *arg)
{
    char msg[512];
    snprintf(msg, sizeof(msg), fmt, arg ? arg : "");
    if (P->line > 0) snprintf(P->err, P->errSize, "%s:%d: %s", P->path, P->line, msg);
    else snprintf(P->err, P->errSize, "%s: %s", P->path, msg);
    return false;
}

static bool ParseFloat(Parser *P, const char *s, float *out)
{
    char *end;
    errno = 0;
    float v = strtof(s, &end);
    if (end == s || *end != '\0' || !isfinite(v)) return Fail(P, "bad number '%s'", s);
    *out = v;
    return true;
}

static bool ParseInt(Parser *P, const char *s, int lo, int hi, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno) return Fail(P, "bad integer '%s'", s);
    if (v < lo || v > hi) {
        char range[96];
        snprintf(range, sizeof(range), "%s (allowed %d..%d)", s, lo, hi);
        return Fail(P, "integer out of range: %s", range);
    }
    *out = (int)v;
    return true;
}

// A number token (strings are rejected)
static bool NumF(Parser *P, Token t, float *out)
{
    if (t.quoted) return Fail(P, "expected a number, got the string \"%s\"", t.s);
    return ParseFloat(P, t.s, out);
}

// A camera or light position number (spawn, viewpoint, light): |v| <= MS_COORD_LIMIT
static bool NumPos(Parser *P, Token t, float *out)
{
    if (!NumF(P, t, out)) return false;
    if (fabsf(*out) > MS_COORD_LIMIT) return Fail(P, "position out of range: %s (allowed -1000000..1000000)", t.s);
    return true;
}

static bool NumI(Parser *P, Token t, int lo, int hi, int *out)
{
    if (t.quoted) return Fail(P, "expected an integer, got the string \"%s\"", t.s);
    return ParseInt(P, t.s, lo, hi, out);
}

static bool IsNumber(Token t)
{
    if (t.quoted) return false;
    char *end;
    strtof(t.s, &end);
    return end != t.s && *end == '\0';
}

// Splits a line into whitespace-separated tokens (into P->tok), in place. "..." is one
// token (no escapes); '#' outside quotes ends the line.
static bool Tokenize(Parser *P, char *line, int *count)
{
    int n = 0;
    char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') break;
        if (!MS_RESERVE(P->tok, P->tokCap, n + 1)) return Fail(P, "out of memory", NULL);
        Token *tok = P->tok;
        if (*p == '"') {
            char *close = strchr(p + 1, '"');
            if (!close) return Fail(P, "unterminated string", NULL);
            *close = '\0';
            tok[n++] = (Token){ p + 1, true };
            p = close + 1;
            if (*p != '\0' && *p != ' ' && *p != '\t' && *p != '#') return Fail(P, "missing space after string", NULL);
            continue;
        }
        tok[n++] = (Token){ p, false };
        while (*p && *p != ' ' && *p != '\t' && *p != '#' && *p != '"') p++;
        if (*p == '"') return Fail(P, "unexpected '\"' inside a word", NULL);
        if (*p == '#') { *p = '\0'; break; }
        if (*p) *p++ = '\0';
    }
    *count = n;
    return true;
}

// Adds a file to the scene's dependencies (hot reload watches them)
static bool AddDep(Parser *P, const char *full)
{
    SceneData *d = P->d;
    for (int i = 0; i < d->depCount; i++)
        if (strcmp(d->deps[i], full) == 0) return true;
    char *copy = MS_StrDup(full);
    char **slot = copy ? MS_PUSH(d->deps, d->depCount, d->depCap) : NULL;
    if (!slot) { free(copy); return Fail(P, "out of memory", NULL); }
    *slot = copy;
    return true;
}

// A heap copy of s for the scene (names, paths); "out of memory" on failure
static bool Dup(Parser *P, const char *s, char **out)
{
    if (!(*out = MS_StrDup(s))) return Fail(P, "out of memory", NULL);
    return true;
}

// Package-relative path -> full path; rejects anything that could leave the package
static bool PackagePath(Parser *P, Token t, char *full, int fullSize)
{
    if (!t.quoted) return Fail(P, "a path must be in double quotes: \"%s\"", t.s);
    const char *s = t.s;
    if (!InsidePackage(s)) return Fail(P, "path must stay inside the package: \"%s\"", s);
    if (snprintf(full, fullSize, "%s%s", P->d->root, s) >= fullSize) return Fail(P, "path too long: \"%s\"", s);
    // Watched before the existence check: after a failed load the watch list is this attempt's
    // dependencies, so a file that is still missing reloads the package once it appears
    if (!AddDep(P, full)) return false;
    if (!FileExists(full)) return Fail(P, "file not found: \"%s\" (paths are relative to the package folder)", s);
    return true;
}

// The name token of a `group` / `material` / `use` / `viewpoint` line (points into the line)
static bool Name(Parser *P, Token *tok, int n, const char *what, const char **out)
{
    if (n < 2 || !tok[1].quoted) {
        char m[160];
        snprintf(m, sizeof(m), "%s takes a name in double quotes: %s \"name\"", what, what);
        return Fail(P, "%s", m);
    }
    if (tok[1].s[0] == '\0') return Fail(P, "empty name", NULL);
    *out = tok[1].s;
    return true;
}

static bool NoStrings(Parser *P, Token *tok, int from, int n)
{
    for (int i = from; i < n; i++)
        if (tok[i].quoted) return Fail(P, "unexpected string \"%s\"", tok[i].s);
    return true;
}

static int Keyword(Parser *P, Token t, const char *const *names, int count, const char *what, int *out)
{
    for (int i = 0; i < count; i++)
        if (!t.quoted && strcmp(t.s, names[i]) == 0) { *out = i; return 1; }
    char m[256];
    int n = snprintf(m, sizeof(m), "bad %s '%s' (expected", what, t.s);
    for (int i = 0; i < count && n < (int)sizeof(m); i++) n += snprintf(m + n, sizeof(m) - n, " %s", names[i]);
    if (n < (int)sizeof(m)) snprintf(m + n, sizeof(m) - n, ")");
    Fail(P, "%s", m);
    return 0;
}

static bool OnOff(Parser *P, Token t, bool *out)
{
    static const char *const NAMES[] = { "off", "on" };
    int v;
    if (!Keyword(P, t, NAMES, 2, "switch", &v)) return false;
    *out = v == 1;
    return true;
}

static bool ColorArgs(Parser *P, Token *v, int nv, Color *out)
{
    if (nv != 3 && nv != 4) return Fail(P, "a colour takes 3 or 4 numbers (R G B [A], 0-255)", NULL);
    int c[4] = { 0, 0, 0, 255 };
    for (int i = 0; i < nv; i++)
        if (!NumI(P, v[i], 0, 255, &c[i])) return false;
    *out = (Color){ (unsigned char)c[0], (unsigned char)c[1], (unsigned char)c[2], (unsigned char)c[3] };
    return true;
}

// One value of `type` from tokens v[0..nv-1] into dst
static bool ParseValue(Parser *P, int type, Token *v, int nv, void *dst)
{
    int need = 1;
    switch (type) {
        case T_COLOR: return ColorArgs(P, v, nv, dst);
        case T_V3: need = 3; break;
        case T_QUAT: need = 4; break;
        case T_BOX: need = 6; break;
        default: break;
    }
    if (nv != need) {
        char m[64];
        snprintf(m, sizeof(m), "expected %d value%s, got %d", need, need == 1 ? "" : "s", nv);
        return Fail(P, "%s", m);
    }
    switch (type) {
        case T_F: return NumF(P, v[0], dst);
        case T_DEG: {
            float deg;
            if (!NumF(P, v[0], &deg)) return false;
            *(float *)dst = deg * DEG2RAD;
            return true;
        }
        case T_I: return NumI(P, v[0], -1000000000, 1000000000, dst);
        case T_B: return OnOff(P, v[0], dst);
        case T_V3: case T_QUAT: case T_BOX: {
            float f[6];
            for (int i = 0; i < need; i++)
                if (!NumF(P, v[i], &f[i])) return false;
            if (type == T_BOX) {
                BoundingBox *b = dst;
                *b = (BoundingBox){ { fminf(f[0], f[3]), fminf(f[1], f[4]), fminf(f[2], f[5]) },
                                    { fmaxf(f[0], f[3]), fmaxf(f[1], f[4]), fmaxf(f[2], f[5]) } };
            } else memcpy(dst, f, need * sizeof(float));
            return true;
        }
        case T_PATH: {
            char full[MS_PATH_MAX];
            if (!PackagePath(P, v[0], full, sizeof(full))) return false;
            snprintf(dst, MS_PATH_MAX, "%s", v[0].s);
            return true;
        }
        case T_FOG:     { int e; if (!Keyword(P, v[0], FOG_NAMES, 4, "fog mode", &e)) return false; *(R3D_Fog *)dst = (R3D_Fog)e; return true; }
        case T_BLOOM:   { int e; if (!Keyword(P, v[0], BLOOM_NAMES, 4, "bloom mode", &e)) return false; *(R3D_Bloom *)dst = (R3D_Bloom)e; return true; }
        case T_TONEMAP: { int e; if (!Keyword(P, v[0], TONEMAP_NAMES, 5, "tonemap mode", &e)) return false; *(R3D_Tonemap *)dst = (R3D_Tonemap)e; return true; }
        case T_DOF:     { int e; if (!Keyword(P, v[0], DOF_NAMES, 2, "dof mode", &e)) return false; *(R3D_DoF *)dst = (R3D_DoF)e; return true; }
        case T_SKYMODE: return Keyword(P, v[0], SKYMODE_NAMES, 3, "sky mode", dst) == 1;
        case T_AMBMODE: return Keyword(P, v[0], AMBMODE_NAMES, 3, "ambientSky mode", dst) == 1;
    }
    return Fail(P, "internal: unknown value type", NULL);
}

// --- Primitives (the former BuildWorld() builders; float math kept exactly as it was)

static bool Box(Parser *P, float x0, float y0, float z0, float x1, float y1, float z1)
{
    SceneData *d = P->d;
    if (d->boxCount + d->modelCount + d->lightCount >= MS_MAX_OBJECTS) return false;
    BoundingBox b = {
        { fminf(x0, x1), fminf(y0, y1), fminf(z0, z1) },
        { fmaxf(x0, x1), fmaxf(y0, y1), fmaxf(z0, z1) },
    };
    // Each number is finite, but shifts (repeat * d, at) or the size can still overflow;
    // inf/NaN would reach the instance scale, collision and picking
    if (!isfinite(b.min.x) || !isfinite(b.min.y) || !isfinite(b.min.z) ||
        !isfinite(b.max.x) || !isfinite(b.max.y) || !isfinite(b.max.z) ||
        !isfinite(b.max.x - b.min.x) || !isfinite(b.max.y - b.min.y) || !isfinite(b.max.z - b.min.z)) {
        P->badNumber = true;
        return false;
    }
    if (!MS_RESERVE(d->boxes, d->boxCap, d->boxCount + 1) || !MS_RESERVE(d->boxRefs, d->boxRefCap, d->boxCount + 1)) {
        P->outOfMemory = true;
        return false;
    }
    d->boxes[d->boxCount] = b;
    d->boxRefs[d->boxCount++] = P->cur;
    P->cur.part++;
    return true;
}

// Box from center (x, z), ground level y0 and size
static bool Block(Parser *P, float cx, float y0, float cz, float sx, float sy, float sz)
{
    return Box(P, cx - sx * 0.5f, y0, cz - sz * 0.5f, cx + sx * 0.5f, y0 + sy, cz + sz * 0.5f);
}

// Straight staircase climbing toward -Z. Each step is a solid block down to baseY.
static bool Stairs(Parser *P, float cx, float baseY, float startZ, float width, int steps, float rise, float run)
{
    for (int i = 0; i < steps; i++) {
        float z0 = startZ - i * run;
        if (!Box(P, cx - width * 0.5f, baseY, z0, cx + width * 0.5f, baseY + (i + 1) * rise, z0 - run)) return false;
    }
    return true;
}

// Facade with a grid of deep window recesses ("waffle" facade): a back wall plus vertical
// fins and horizontal ledges in front of it. Every loop iteration adds a box, so
// MS_MAX_OBJECTS bounds the loops even for silly input.
static bool WaffleFacade(Parser *P, float x0, float x1, float y0, float y1, float z, float depth, float bayW, float floorH)
{
    if (!Box(P, x0, y0, z - 1.0f, x1, y1, z)) return false;                // back wall
    for (float x = x0; x <= x1 + 0.01f; x += bayW)                          // vertical fins
        if (!Box(P, x - 0.3f, y0, z, x + 0.3f, y1, z + depth)) return false;
    for (float y = y0 + floorH; y <= y1 + 0.01f; y += floorH)               // horizontal ledges
        if (!Box(P, x0, y - 0.5f, z, x1, y, z + depth)) return false;
    return true;
}

enum { AX_NONE, AX_X, AX_Y, AX_Z, AX_INT };   // what a primitive argument is

typedef struct {
    const char *name;
    int argc;
    unsigned char kind[8];     // per argument: axis it is shifted along, or AX_INT
} PrimDef;

static const PrimDef PRIMS[] = {
    { "box",    6, { AX_X, AX_Y, AX_Z, AX_X, AX_Y, AX_Z } },
    { "block",  6, { AX_X, AX_Y, AX_Z, AX_NONE, AX_NONE, AX_NONE } },
    { "stairs", 7, { AX_X, AX_Y, AX_Z, AX_NONE, AX_INT, AX_NONE, AX_NONE } },
    { "waffle", 8, { AX_X, AX_X, AX_Y, AX_Y, AX_Z, AX_NONE, AX_NONE, AX_NONE } },
};

// `id` / `repeat` / `at` prefixes of a line
typedef struct {
    float at[3];
    bool hasAt;
    int repN[MS_REPEATS_MAX];
    int repFrom[MS_REPEATS_MAX];        // `from k`: the first copy index of that repeat (default 0)
    float repD[MS_REPEATS_MAX][3];
    int reps;
    bool hasId;
    int id;                             // `id n`
} Placement;

static bool ParsePlacement(Parser *P, Token *tok, int n, int *tIn, Placement *pl)
{
    memset(pl, 0, sizeof(*pl));
    long total = 1;
    int t = 0;
    for (;;) {
        if (t >= n) return Fail(P, "missing primitive after repeat/at", NULL);
        if (strcmp(tok[t].s, "repeat") == 0 && !tok[t].quoted) {
            if (pl->reps == MS_REPEATS_MAX) return Fail(P, "at most 3 repeats per line", NULL);
            if (t + 4 >= n) return Fail(P, "repeat takes 4 numbers: repeat n dx dy dz", NULL);
            if (!NumI(P, tok[t + 1], 0, MS_MAX_OBJECTS, &pl->repN[pl->reps])) return false;
            for (int a = 0; a < 3; a++)
                if (!NumF(P, tok[t + 2 + a], &pl->repD[pl->reps][a])) return false;
            total *= pl->repN[pl->reps] > 0 ? pl->repN[pl->reps] : 1;
            // Bounds the work even for primitives that make no boxes (stairs with 0 steps)
            if (total > MS_MAX_OBJECTS) return Fail(P, "too many copies (a map makes at most 1000000 objects)", NULL);
            t += 5;
            if (t < n && !tok[t].quoted && strcmp(tok[t].s, "from") == 0) {
                if (t + 1 >= n) return Fail(P, "from takes the first copy index: repeat n dx dy dz from k", NULL);
                if (!NumI(P, tok[t + 1], 0, MS_MAX_OBJECTS, &pl->repFrom[pl->reps])) return false;
                t += 2;
            }
            pl->reps++;
        } else if (strcmp(tok[t].s, "id") == 0 && !tok[t].quoted) {
            if (pl->hasId) return Fail(P, "only one 'id' per line", NULL);
            if (t + 1 >= n) return Fail(P, "id takes a number: id n (1..2147483647)", NULL);
            if (!NumI(P, tok[t + 1], 1, 2147483647, &pl->id)) return false;
            pl->hasId = true;
            t += 2;
        } else if (strcmp(tok[t].s, "at") == 0 && !tok[t].quoted) {
            if (pl->hasAt) return Fail(P, "only one 'at' per line", NULL);
            if (t + 3 >= n) return Fail(P, "at takes 3 numbers: at x y z", NULL);
            for (int a = 0; a < 3; a++)
                if (!NumF(P, tok[t + 1 + a], &pl->at[a])) return false;
            pl->hasAt = true;
            t += 4;
        } else break;
    }
    *tIn = t;
    return true;
}

// Offset of copy (i0, i1, i2): (at + i*d1 + j*d2 + k*d3), in this order of float additions;
// with `from`, i counts from there (the same floats as that copy of the longer row)
static void CopyOffset(const Placement *pl, const int *idx, float off[3])
{
    for (int a = 0; a < 3; a++) {
        off[a] = pl->at[a];
        for (int r = 0; r < pl->reps; r++) off[a] += (float)(idx[r] + pl->repFrom[r]) * pl->repD[r][a];
    }
}

// Stamps the copy (i0, i1, i2) of a cnt[] grid on the objects that follow (MS_Ref.copy, .part)
static void BeginCopy(Parser *P, const int *cnt, int i0, int i1, int i2)
{
    P->cur.copy = (i0 * cnt[1] + i1) * cnt[2] + i2;
    P->cur.part = 0;
}

static bool ParsePrimitive(Parser *P, const PrimDef *def, Token *args, int argc, const Placement *pl)
{
    if (argc != def->argc) {
        char what[64];
        snprintf(what, sizeof(what), "%s takes %d numbers, got %d", def->name, def->argc, argc);
        return Fail(P, "%s", what);
    }
    float v[8];
    int steps = 0;
    for (int a = 0; a < argc; a++) {
        if (def->kind[a] == AX_INT) {
            if (!NumI(P, args[a], 0, MS_MAX_OBJECTS, &steps)) return false;
            v[a] = 0.0f;
        } else if (!NumF(P, args[a], &v[a])) return false;
    }
    if (def->argc == 8 && (v[6] <= 0.0f || v[7] <= 0.0f)) return Fail(P, "waffle bayW and floorH must be > 0", NULL);

    bool shifted = pl->hasAt || pl->reps > 0;
    int cnt[MS_REPEATS_MAX] = { 1, 1, 1 };
    for (int r = 0; r < pl->reps; r++) cnt[r] = pl->repN[r];

    for (int i0 = 0; i0 < cnt[0]; i0++)
    for (int i1 = 0; i1 < cnt[1]; i1++)
    for (int i2 = 0; i2 < cnt[2]; i2++) {
        int idx[MS_REPEATS_MAX] = { i0, i1, i2 };
        float off[3];
        CopyOffset(pl, idx, off);
        BeginCopy(P, cnt, i0, i1, i2);
        float p[8];
        for (int a = 0; a < argc; a++) {
            int k = def->kind[a];
            // Unshifted lines use the numbers as written (no "+ 0", which would turn -0 into 0)
            p[a] = (shifted && k >= AX_X && k <= AX_Z) ? off[k - AX_X] + v[a] : v[a];
        }
        bool ok;
        switch (def->argc) {
            case 7:  ok = Stairs(P, p[0], p[1], p[2], p[3], steps, p[5], p[6]); break;
            case 8:  ok = WaffleFacade(P, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]); break;
            default: ok = (def == &PRIMS[0]) ? Box(P, p[0], p[1], p[2], p[3], p[4], p[5])
                                             : Block(P, p[0], p[1], p[2], p[3], p[4], p[5]); break;
        }
        if (!ok) return Fail(P, P->badNumber ? "coordinate or size out of range" : P->outOfMemory ? "out of memory"
                                : "too many objects (a map makes at most 1000000 boxes, models and lights)", NULL);
    }
    return true;
}

// light point|spot|dir ... (args = the tokens after "light")
static bool ParseLight(Parser *P, Token *args, int n, const Placement *pl)
{
    static const char *const TYPES[] = { "dir", "spot", "point" };   // = R3D_LightType order
    int type;
    if (n < 1 || !Keyword(P, args[0], TYPES, 3, "light type", &type)) return n < 1 ? Fail(P, "light takes a type: light point|spot|dir ...", NULL) : false;
    int nPos = type == R3D_LIGHT_DIR ? 0 : 3, nDir = type == R3D_LIGHT_OMNI ? 0 : 3;
    if (n < 1 + nPos + nDir) return Fail(P, "light: missing coordinates (point x y z, spot x y z dx dy dz, dir dx dy dz)", NULL);
    float v[6];
    for (int i = 0; i < nPos + nDir; i++)
        if (!(i < nPos ? NumPos(P, args[1 + i], &v[i]) : NumF(P, args[1 + i], &v[i]))) return false;
    Vector3 pos = { 0 }, dir = { 0, -1, 0 };
    if (nPos) pos = (Vector3){ v[0], v[1], v[2] };
    if (nDir) dir = (Vector3){ v[nPos], v[nPos + 1], v[nPos + 2] };
    if (nDir && Vector3Length(dir) == 0.0f) return Fail(P, "light direction must not be 0 0 0", NULL);

    // Defaults (R3D_Create*Light, with range 16 m for point / spot lights)
    R3D_Light L = { 0 };
    L.position = pos;
    L.direction = nDir ? Vector3Normalize(dir) : (Vector3){ 0 };
    L.color = WHITE;
    L.energy = 1.0f;
    L.specular = 1.0f;
    L.range = type == R3D_LIGHT_DIR ? 50.0f : 16.0f;
    L.falloff = 1.0f;
    L.innerCutOff = type == R3D_LIGHT_SPOT ? 22.5f : 0.0f;
    L.outerCutOff = type == R3D_LIGHT_SPOT ? 45.0f : 180.0f;
    L.fogEnergy = 1.0f;
    L.type = (R3D_LightType)type;
    bool shadow = false;
    bool clip = false;
    float cb[6] = { 0 };

    for (int t = 1 + nPos + nDir; t < n; ) {
        const char *k = args[t].s;
        if (args[t].quoted) return Fail(P, "unexpected string \"%s\"", k);
        int nv = 0;
        while (t + 1 + nv < n && IsNumber(args[t + 1 + nv])) nv++;
        Token *val = &args[t + 1];
        float f;
        if (strcmp(k, "color") == 0) { if (!ColorArgs(P, val, nv, &L.color)) return false; }
        else if (strcmp(k, "shadow") == 0) {
            if (t + 1 >= n || !OnOff(P, args[t + 1], &shadow)) return t + 1 >= n ? Fail(P, "shadow takes on or off", NULL) : false;
            nv = 1;
        } else if (strcmp(k, "clip") == 0) {
            if (nv != 6) return Fail(P, "light key 'clip' takes six numbers: x0 y0 z0 x1 y1 z1", NULL);
            for (int i = 0; i < 6; i++) if (!NumPos(P, val[i], &cb[i])) return false;
            for (int i = 0; i < 3; i++) if (cb[i] == cb[i + 3]) return Fail(P, "light clip box must not be flat", NULL);
            clip = true;
        } else {
            float *dst = strcmp(k, "energy") == 0 ? &L.energy : strcmp(k, "range") == 0 ? &L.range :
                         strcmp(k, "falloff") == 0 ? &L.falloff : strcmp(k, "specular") == 0 ? &L.specular :
                         strcmp(k, "fogEnergy") == 0 ? &L.fogEnergy : strcmp(k, "inner") == 0 ? &L.innerCutOff :
                         strcmp(k, "outer") == 0 ? &L.outerCutOff : NULL;
            if (!dst) return Fail(P, "unknown light key '%s' (color energy range falloff specular fogEnergy inner outer shadow clip)", k);
            if (nv != 1) return Fail(P, "light key '%s' takes one number", k);
            if (!NumF(P, val[0], &f)) return false;
            *dst = f;
        }
        t += 1 + nv;
    }
    if (type == R3D_LIGHT_SPOT && L.innerCutOff > L.outerCutOff) return Fail(P, "spot light: inner must be <= outer", NULL);

    bool shifted = pl->hasAt || pl->reps > 0;
    int cnt[MS_REPEATS_MAX] = { 1, 1, 1 };
    for (int r = 0; r < pl->reps; r++) cnt[r] = pl->repN[r];
    for (int i0 = 0; i0 < cnt[0]; i0++)
    for (int i1 = 0; i1 < cnt[1]; i1++)
    for (int i2 = 0; i2 < cnt[2]; i2++) {
        SceneData *d = P->d;
        if (d->boxCount + d->modelCount + d->lightCount >= MS_MAX_OBJECTS)
            return Fail(P, "too many objects (a map makes at most 1000000 boxes, models and lights)", NULL);
        int idx[MS_REPEATS_MAX] = { i0, i1, i2 };
        float off[3];
        CopyOffset(pl, idx, off);
        BeginCopy(P, cnt, i0, i1, i2);
        R3D_Light c = L;
        if (shifted && nPos) c.position = (Vector3){ off[0] + pos.x, off[1] + pos.y, off[2] + pos.z };
        BoundingBox cbox = { 0 };
        if (clip) {
            // shifted like box corners: (at + i * d) + value
            float a[6];
            for (int k = 0; k < 6; k++) a[k] = shifted ? off[k % 3] + cb[k] : cb[k];
            cbox.min = (Vector3){ fminf(a[0], a[3]), fminf(a[1], a[4]), fminf(a[2], a[5]) };
            cbox.max = (Vector3){ fmaxf(a[0], a[3]), fmaxf(a[1], a[4]), fmaxf(a[2], a[5]) };
        }
        MS_LightDef *slot = MS_PUSH(d->lights, d->lightCount, d->lightCap);
        if (!slot) return Fail(P, "out of memory", NULL);
        *slot = (MS_LightDef){ c, shadow, P->cur, clip, cbox };
    }
    return true;
}

// Material names are looked up through a hash table: a linear search made a map with many
// materials quadratic to parse (QA2 F6: 80000 materials took 17 s)
static unsigned MatHashSlot(const char *name, int cap)
{
    return (unsigned)(Fnv1a64Update(FNV_OFFSET, name, strlen(name)) & (unsigned long long)(cap - 1));
}

static int FindMaterial(const SceneData *d, const char *name)
{
    if (!d->matHashCap) return -1;
    for (unsigned h = MatHashSlot(name, d->matHashCap); d->matHash[h]; h = (h + 1) & (unsigned)(d->matHashCap - 1))
        if (strcmp(d->materials[d->matHash[h] - 1].name, name) == 0) return d->matHash[h] - 1;
    return -1;
}

// Enters material `mi` into the table (grown to keep it at most half full)
static bool MatHashInsert(SceneData *d, int mi)
{
    if (2 * (d->materialCount + 1) > d->matHashCap) {
        int cap = d->matHashCap ? d->matHashCap : 64;
        while (2 * (d->materialCount + 1) > cap) {
            if (cap > (1 << 29)) return false;
            cap *= 2;
        }
        int *t = calloc((size_t)cap, sizeof(int));
        if (!t) return false;
        free(d->matHash);
        d->matHash = t;
        d->matHashCap = cap;
        for (int i = 0; i < d->materialCount; i++)
            if (i != mi) {
                unsigned h = MatHashSlot(d->materials[i].name, cap);
                while (t[h]) h = (h + 1) & (unsigned)(cap - 1);
                t[h] = i + 1;
            }
    }
    unsigned h = MatHashSlot(d->materials[mi].name, d->matHashCap);
    while (d->matHash[h]) h = (h + 1) & (unsigned)(d->matHashCap - 1);
    d->matHash[h] = mi + 1;
    return true;
}

// A new material `name` with R3D's defaults: its index, or -1 when out of memory
static int AddMaterial(SceneData *d, const char *name)
{
    char *copy = MS_StrDup(name);
    if (!copy || !MS_RESERVE(d->matDefs, d->matDefCap, d->materialCount + 1) ||
        !MS_RESERVE(d->materials, d->materialCap, d->materialCount + 1)) {
        free(copy);
        return -1;
    }
    int mi = d->materialCount++;
    d->materials[mi] = (MS_Material){ .name = copy, .material = R3D_MATERIAL_BASE };
    d->matDefs[mi] = (MatDef){ 0 };
    if (!MatHashInsert(d, mi)) return -1;   // (the entry stays; FreeData frees its name)
    return mi;
}

// The built-in "default" material (docs/map-format.md 2.4)
static bool DefaultMaterial(SceneData *d)
{
    if (AddMaterial(d, "default") != 0) return false;
    MS_Material *m = &d->materials[0];
    m->material.albedo.color = (Color){ 180, 180, 180, 255 };
    m->material.orm.roughness = 0.9f;
    m->material.orm.metalness = 0.0f;
    return true;
}

static bool ParseMaterial(Parser *P, Token *tok, int n)
{
    SceneData *d = P->d;
    const char *name;
    if (!Name(P, tok, n, "material", &name)) return false;
    int mi = FindMaterial(d, name);
    if (mi < 0 && (mi = AddMaterial(d, name)) < 0) return Fail(P, "out of memory", NULL);
    MS_Material *M = &d->materials[mi];
    MatDef *D = &d->matDefs[mi];
    R3D_Material *m = &M->material;
    if (M->line == 0) M->line = P->line;

    for (int t = 2; t < n; ) {
        const char *k = tok[t].s;
        if (tok[t].quoted) return Fail(P, "expected a material key, got the string \"%s\"", k);
        int nv = 0;
        while (t + 1 + nv < n && IsNumber(tok[t + 1 + nv])) nv++;
        Token *v = &tok[t + 1];
        int used = nv;
        char full[MS_PATH_MAX];
        float f;
        #define NEED(c) do { if (nv != (c)) { char m_[96]; snprintf(m_, sizeof(m_), "material key '%s' takes %d number%s", k, (c), (c) == 1 ? "" : "s"); return Fail(P, "%s", m_); } } while (0)
        #define ONE_WORD() do { if (t + 1 >= n) return Fail(P, "material key '%s' needs a value", k); used = 1; } while (0)
        if (strcmp(k, "shader") == 0) {
            ONE_WORD();
            if (!PackagePath(P, v[0], full, sizeof(full))) return false;
            char *copy;
            if (!Dup(P, v[0].s, &copy)) return false;
            free((char *)M->shader);
            M->shader = copy;
            D->shaderLine = P->line;
        } else if (strcmp(k, "uniform") == 0) {
            if (t + 1 >= n || v[0].quoted) return Fail(P, "uniform takes a name and 1-4 numbers: uniform u_name 1.0", NULL);
            nv = 0;
            while (t + 2 + nv < n && IsNumber(tok[t + 2 + nv])) nv++;
            if (nv < 1 || nv > 4) return Fail(P, "uniform '%s' takes 1 to 4 numbers", v[0].s);
            MatUniform *u = NULL;
            for (int i = 0; i < D->uniformCount; i++)
                if (strcmp(D->uniforms[i].name, v[0].s) == 0) u = &D->uniforms[i];
            if (!u) {
                if (!(u = MS_PUSH(D->uniforms, D->uniformCount, D->uniformCap))) return Fail(P, "out of memory", NULL);
                if (!Dup(P, v[0].s, &u->name)) { D->uniformCount--; return false; }
            }
            u->count = nv;
            for (int i = 0; i < nv; i++)
                if (!NumF(P, tok[t + 2 + i], &u->v[i])) return false;
            used = 1 + nv;
        } else if (strcmp(k, "sampler") == 0 || strcmp(k, "data_sampler") == 0) {
            if (t + 2 >= n || v[0].quoted) return Fail(P, "sampler / data_sampler take a name and a path: sampler u_tex \"textures/x.png\"", NULL);
            if (!PackagePath(P, v[1], full, sizeof(full))) return false;
            MatSampler *s = NULL;
            for (int i = 0; i < D->samplerCount; i++)
                if (strcmp(D->samplers[i].name, v[0].s) == 0) s = &D->samplers[i];
            if (!s) {
                if (!(s = MS_PUSH(D->samplers, D->samplerCount, D->samplerCap))) return Fail(P, "out of memory", NULL);
                if (!Dup(P, v[0].s, &s->name)) { D->samplerCount--; return false; }
            }
            char *copy;
            if (!Dup(P, v[1].s, &copy)) return false;
            free(s->path);
            s->path = copy;
            s->line = P->line;
            s->data = k[0] == 'd';
            used = 2;
        } else if (strcmp(k, "albedo") == 0) {
            if (!ColorArgs(P, v, nv, &m->albedo.color)) return false;
        } else if (strcmp(k, "emission") == 0) {
            if (nv != 3) return Fail(P, "emission takes 3 numbers (R G B)", NULL);
            if (!ColorArgs(P, v, nv, &m->emission.color)) return false;
        } else if (strcmp(k, "albedo_map") == 0 || strcmp(k, "normal_map") == 0 ||
                   strcmp(k, "orm_map") == 0 || strcmp(k, "emission_map") == 0) {
            ONE_WORD();
            if (!PackagePath(P, v[0], full, sizeof(full))) return false;
            int slot = k[0] == 'a' ? MAP_ALBEDO : k[0] == 'n' ? MAP_NORMAL : k[0] == 'o' ? MAP_ORM : MAP_EMISSION;
            char *copy;
            if (!Dup(P, v[0].s, &copy)) return false;
            free(D->maps[slot]);
            D->maps[slot] = copy;
            D->mapLine[slot] = P->line;
        } else if (strcmp(k, "uv_scale") == 0 || strcmp(k, "uv_offset") == 0) {
            NEED(2);
            Vector2 uv;
            if (!NumF(P, v[0], &uv.x) || !NumF(P, v[1], &uv.y)) return false;
            if (k[3] == 's') m->uvScale = uv; else m->uvOffset = uv;
        } else if (strcmp(k, "cull") == 0) {
            static const char *const CULL[] = { "none", "back", "front" };   // = R3D_CullMode
            int e;
            ONE_WORD();
            if (!Keyword(P, v[0], CULL, 3, "cull mode", &e)) return false;
            m->cullMode = (R3D_CullMode)e;
        } else if (strcmp(k, "transparency") == 0) {
            static const char *const TR[] = { "off", "prepass", "alpha" };    // = R3D_TransparencyMode
            int e;
            ONE_WORD();
            if (!Keyword(P, v[0], TR, 3, "transparency mode", &e)) return false;
            m->transparencyMode = (R3D_TransparencyMode)e;
        } else if (strcmp(k, "blend") == 0) {
            static const char *const BL[] = { "alpha", "additive", "add_alpha", "multiply", "premultiplied" };
            int e;
            ONE_WORD();
            if (!Keyword(P, v[0], BL, 5, "blend mode", &e)) return false;
            m->blendMode = (R3D_BlendMode)e;
        } else if (strcmp(k, "wrap") == 0) {
            static const char *const WR[] = { "repeat", "clamp", "mirror" };
            ONE_WORD();
            if (!Keyword(P, v[0], WR, 3, "wrap mode", &D->wrap)) return false;
        } else if (strcmp(k, "unlit") == 0) {
            ONE_WORD();
            if (!OnOff(P, v[0], &m->unlit)) return false;
        } else if (strcmp(k, "instanced") == 0) {
            ONE_WORD();
            if (!OnOff(P, v[0], &D->instanced)) return false;
        } else if (strcmp(k, "priority") == 0) {
            NEED(1);
            if (!NumI(P, v[0], -1000000, 1000000, &m->priority)) return false;
        } else if (strcmp(k, "inherit") == 0) {
            static const char *const IN[] = { "albedo", "normal", "orm", "emission", "all" };
            int e;
            ONE_WORD();
            if (!Keyword(P, v[0], IN, 5, "inherit map", &e)) return false;
            D->inherit |= e == 4 ? 15u : (1u << e);
        } else {
            float *dst = strcmp(k, "normal_scale") == 0 ? &m->normal.scale :
                         strcmp(k, "occlusion") == 0 ? &m->orm.occlusion :
                         strcmp(k, "roughness") == 0 ? &m->orm.roughness :
                         strcmp(k, "metalness") == 0 ? &m->orm.metalness :
                         strcmp(k, "specular") == 0 ? &m->orm.specular :
                         strcmp(k, "emission_energy") == 0 ? &m->emission.energy :
                         strcmp(k, "alpha_cutoff") == 0 ? &m->alphaCutoff : NULL;
            if (!dst) return Fail(P, "unknown material key '%s'", k);
            NEED(1);
            if (!NumF(P, v[0], &f)) return false;
            *dst = f;
        }
        #undef NEED
        #undef ONE_WORD
        t += 1 + used;
    }
    return true;
}

static bool ParseEnv(Parser *P, Token *tok, int n)
{
    if (n < 3 || tok[1].quoted) return Fail(P, "env takes a key and a value: env fog.density 0.01", NULL);
    const char *key = tok[1].s;
    EnvDef *e = &P->d->env;
    char m[160];
    for (int i = 0; i < COUNT(ENV_KEYS); i++)
        if (strcmp(ENV_KEYS[i].key, key) == 0) {
            void *dst = (char *)&e->env + ENV_KEYS[i].off;
            if (!ParseValue(P, ENV_KEYS[i].type, &tok[2], n - 2, dst)) return false;
            return CheckEnvValue(key, ENV_KEYS[i].type, dst, m, sizeof(m)) || Fail(P, "%s", m);
        }
    for (int i = 0; i < COUNT(DEF_KEYS); i++) {
        if (strcmp(DEF_KEYS[i].key, key) != 0) continue;
        void *dst = (char *)e + DEF_KEYS[i].off;
        if (!ParseValue(P, DEF_KEYS[i].type, &tok[2], n - 2, dst)) return false;
        if (strcmp(key, "shadow.home") == 0) e->homeSet = true;
        if (strcmp(key, "fogCache.region") == 0) e->fogRegionSet = true;
        return CheckEnvValue(key, DEF_KEYS[i].type, dst, m, sizeof(m)) || Fail(P, "%s", m);
    }
    bool amb = strncmp(key, "ambientSky.", 11) == 0;
    if (amb || strncmp(key, "sky.", 4) == 0) {
        int k = SkyKeyIndex(key + (amb ? 11 : 4));
        if (k >= 0) {
            R3D_ProceduralSky *sky = amb ? &e->ambient : &e->sky;
            void *dst = (char *)sky + SKY_KEYS[k].off;
            if (!ParseValue(P, SKY_KEYS[k].type, &tok[2], n - 2, dst)) return false;
            if (!CheckEnvValue(key, SKY_KEYS[k].type, dst, m, sizeof(m))) return Fail(P, "%s", m);
            if (amb) e->ambientSet |= 1u << k;
            else if (strcmp(SKY_KEYS[k].key, "sunDirection") == 0) e->skySunDirSet = true;
            return true;
        }
    }
    return Fail(P, "unknown env key '%s' (see the header comment of monolith_scene.c)", key);
}

// --- Models (docs/map-format.md 2.7)

// A model file's references (glTF uris, OBJ mtllib, MTL texture maps) must stay inside the
// package. The checks read the files the way assimp / R3D's importer do (QA2 F1/F2): JSON
// strings fully decoded (\uXXXX too) and keys matched after decoding, OBJ/MTL lines split on
// \r \n \0 \f over the whole buffer, MTL map lines matched by prefix and every possible file
// name on the line checked (assimp takes the rest of the line after the options, spaces
// included), references of 1024 bytes or more rejected (never skipped), and both the raw and
// the %-decoded form of each path must stay inside.

// R3D's importer joins a model's folder and a texture path in a 256-byte buffer
// (r3d_importer_texture.c, MAX_PATH_LENGTH); a longer join would overflow it
#define MS_IMPORTER_PATH_MAX 256

enum { REF_DEP = 1, REF_TEXTURE = 2 };

// `ref` resolved against the folder of `base` (a package-relative file): "." and ".."
// removed, '\' read as '/'. Fails if it is absolute, a URL, or leaves the package.
static bool RefResolve(Parser *P, const char *modelRel, const char *base, const char *ref, char *out, int outSize)
{
    char msg[2 * MS_PATH_MAX + 200];
    if (strstr(ref, "://") || ref[0] == '/' || ref[0] == '\\' || (ref[0] && ref[1] == ':')) {
        snprintf(msg, sizeof(msg), "\"%s\" refers to \"%s\", outside the package", modelRel, ref);
        return Fail(P, "%s", msg);
    }
    char joined[2 * MS_PATH_MAX];
    const char *slash = strrchr(base, '/');
    snprintf(joined, sizeof(joined), "%.*s%s", slash ? (int)(slash - base + 1) : 0, base, ref);
    for (char *c = joined; *c; c++) if (*c == '\\') *c = '/';
    int len = 0;
    for (char *c = joined; *c; ) {
        char *e = strchr(c, '/');
        int seg = e ? (int)(e - c) : (int)strlen(c);
        if (seg == 0 || (seg == 1 && c[0] == '.')) { /* skip */ }
        else if (seg == 2 && c[0] == '.' && c[1] == '.') {
            if (len == 0) {
                snprintf(msg, sizeof(msg), "\"%s\" refers to \"%s\", outside the package", modelRel, ref);
                return Fail(P, "%s", msg);
            }
            while (len > 0 && out[len - 1] != '/') len--;
            if (len > 0) len--;     // the '/' before the dropped segment
        } else {
            if (len + seg + 2 >= outSize) return Fail(P, "path too long: \"%s\"", ref);
            if (len > 0) out[len++] = '/';
            memcpy(out + len, c, seg);
            len += seg;
        }
        c = e ? e + 1 : c + seg;
    }
    out[len] = 0;
    return true;
}

// One file reference in a model file (`ref`, refLen bytes, JSON escapes already decoded),
// relative to the folder of `base`. REF_DEP: it must exist (and outRel, if given, gets its
// package path); REF_TEXTURE: R3D's importer opens it relative to the model's folder
// (length guard).
static bool ModelRef(Parser *P, const char *modelRel, const char *base, const char *ref, int refLen, int flags, char *outRel)
{
    if (refLen == 0 || (refLen >= 5 && strncmp(ref, "data:", 5) == 0)) return true;    // embedded
    if (refLen >= MS_PATH_MAX) return Fail(P, "\"%s\": a file reference in it is too long (1024 bytes or more)", modelRel);
    for (int i = 0; i < refLen; i++)
        if ((unsigned char)ref[i] < 0x20) return Fail(P, "\"%s\": a file reference in it contains a control character", modelRel);
    if (flags & REF_TEXTURE) {
        const char *slash = strrchr(modelRel, '/');
        size_t dir = strlen(P->d->root) + (slash ? (size_t)(slash - modelRel) : 0);
        if (dir + 1 + (size_t)refLen + 1 > MS_IMPORTER_PATH_MAX)
            return Fail(P, "\"%s\": a file reference in it is too long for R3D's importer (its folder + the reference "
                           "must stay under 256 bytes)", modelRel);
    }
    char raw[MS_PATH_MAX], dec[MS_PATH_MAX], out[MS_PATH_MAX];
    memcpy(raw, ref, refLen);
    raw[refLen] = 0;
    int n = 0;
    for (int i = 0; i < refLen; i++) {
        char c = raw[i];
        if (c == '%' && i + 2 < refLen && isxdigit((unsigned char)raw[i + 1]) && isxdigit((unsigned char)raw[i + 2])) {
            char hex[3] = { raw[i + 1], raw[i + 2], 0 };
            c = (char)strtol(hex, NULL, 16);
            i += 2;
            if ((unsigned char)c < 0x20) return Fail(P, "\"%s\": a file reference in it contains a control character", modelRel);
        }
        dec[n++] = c;
    }
    dec[n] = 0;
    // both forms (assimp and R3D may or may not %-decode): each must stay inside
    if (!RefResolve(P, modelRel, base, raw, out, sizeof(out)) || !RefResolve(P, modelRel, base, dec, out, sizeof(out))) return false;
    if (!(flags & REF_DEP)) return true;
    char full[MS_PATH_MAX], msg[MS_PATH_MAX + 200];
    if (snprintf(full, sizeof(full), "%s%s", P->d->root, out) >= (int)sizeof(full)) return Fail(P, "path too long: \"%s\"", out);
    if (!AddDep(P, full)) return false;     // (a dependency: hot reload watches it)
    if (!FileExists(full)) {
        snprintf(msg, sizeof(msg), "file not found: \"%s\" (named by \"%s\")", out, modelRel);
        return Fail(P, "%s", msg);
    }
    if (outRel) memcpy(outRel, out, strlen(out) + 1);
    return true;
}

static void PutUtf8(char *o, int *n, unsigned cp)
{
    if (cp < 0x80) o[(*n)++] = (char)cp;
    else if (cp < 0x800) { o[(*n)++] = (char)(0xC0 | (cp >> 6)); o[(*n)++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        o[(*n)++] = (char)(0xE0 | (cp >> 12)); o[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        o[(*n)++] = (char)(0xF0 | (cp >> 18)); o[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        o[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static bool Hex4(const char *s, const char *end, unsigned *out)
{
    if (end - s < 4) return false;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int c = (unsigned char)s[i];
        if (!isxdigit(c)) return false;
        v = v * 16 + (unsigned)(isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
    }
    *out = v;
    return true;
}

// The JSON string starting after its opening quote at s, decoded (UTF-8) into a heap string
// of *outLen bytes; *next = just after the closing quote. NULL (with Fail) on a bad escape,
// an unterminated string or out of memory.
static char *JsonString(Parser *P, const char *modelRel, const char *s, const char *end, const char **next, int *outLen)
{
    const char *q = s;
    while (q < end && *q != '"') q += (*q == '\\' && q + 1 < end) ? 2 : 1;
    if (q >= end) { Fail(P, "\"%s\": malformed JSON (unterminated string)", modelRel); return NULL; }
    char *o = malloc((size_t)(q - s) + 1);
    if (!o) { Fail(P, "out of memory", NULL); return NULL; }
    int n = 0;
    for (const char *p = s; p < q; p++) {
        if (*p != '\\') { o[n++] = *p; continue; }
        char e = *++p;
        unsigned cp;
        switch (e) {
        case '"': case '\\': case '/': o[n++] = e; break;
        case 'b': o[n++] = '\b'; break;
        case 'f': o[n++] = '\f'; break;
        case 'n': o[n++] = '\n'; break;
        case 'r': o[n++] = '\r'; break;
        case 't': o[n++] = '\t'; break;
        case 'u':
            if (!Hex4(p + 1, q, &cp)) goto bad;
            p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {     // surrogate pair
                unsigned lo;
                if (q - p < 7 || p[1] != '\\' || p[2] != 'u' || !Hex4(p + 3, q, &lo) || lo < 0xDC00 || lo > 0xDFFF) goto bad;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            }
            PutUtf8(o, &n, cp);
            break;
        default: goto bad;
        }
    }
    o[n] = 0;
    *outLen = n;
    *next = q + 1;
    return o;
bad:
    free(o);
    Fail(P, "\"%s\": malformed JSON (bad string escape)", modelRel);
    return NULL;
}

// glTF JSON: the value of every "uri" key (keys compared after decoding)
static bool GltfUriDeps(Parser *P, const char *modelRel, const char *json, size_t size)
{
    const char *end = json + size;
    bool uriValueNext = false;      // just read the key "uri" and its ':'
    for (const char *p = json; p < end; ) {
        if (*p != '"') {
            if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') uriValueNext = false;
            p++;
            continue;
        }
        const char *next;
        int len;
        char *str = JsonString(P, modelRel, p + 1, end, &next, &len);
        if (!str) return false;
        const char *q = next;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
        bool ok = true;
        if (q < end && *q == ':') {         // a key
            uriValueNext = len == 3 && memcmp(str, "uri", 3) == 0;
            p = q + 1;
        } else {
            if (uriValueNext) ok = ModelRef(P, modelRel, modelRel, str, len, REF_DEP | REF_TEXTURE, NULL);
            uriValueNext = false;
            p = next;
        }
        free(str);
        if (!ok) return false;
    }
    return true;
}

// OBJ / MTL lines as assimp splits them: [*s, *e) is the next line (without its end)
static bool NextLine(const char **cur, const char *end, const char **s, const char **e)
{
    if (*cur >= end) return false;
    const char *p = *cur;
    while (p < end && *p != '\r' && *p != '\n' && *p != '\0' && *p != '\f') p++;
    *s = *cur;
    *e = p;
    *cur = p < end ? p + 1 : p;
    return true;
}

static bool IsBlank(char c) { return c == ' ' || c == '\t'; }

// An .mtl file's texture maps (map_*, bump, norm, disp, refl, any case). assimp takes the
// rest of the line after the options as the name, R3D opens it relative to the model's
// folder: every word start on the line must give a path inside the package from the
// model's and from the library's folder; the last word must exist (relative to the library,
// lastMustExist) and becomes a dependency.
static bool MtlDeps(Parser *P, const char *modelRel, const char *mtlRel, const char *text, size_t size, bool lastMustExist)
{
    const char *cur = text, *end = text + size, *s, *e;
    while (NextLine(&cur, end, &s, &e)) {
        while (s < e && IsBlank(*s)) s++;
        const char *k = s;
        while (k < e && !IsBlank(*k)) k++;
        char key[8] = { 0 };
        for (int i = 0; i < 4 && s + i < k; i++) key[i] = (char)tolower((unsigned char)s[i]);
        if (strncmp(key, "map", 3) != 0 && strcmp(key, "bump") != 0 && strcmp(key, "norm") != 0 &&
            strcmp(key, "disp") != 0 && strcmp(key, "refl") != 0) continue;
        const char *b = e;
        while (b > k && (IsBlank(b[-1]) || b[-1] == '\r')) b--;
        const char *first = k;
        while (first < b && IsBlank(*first)) first++;
        if (first >= b) continue;
        if (b - first >= MS_PATH_MAX) return Fail(P, "\"%s\": a texture line in it is too long (1024 bytes or more)", mtlRel);
        const char *last = first;
        for (const char *w = first; w < b; w++) {
            if (w > first && !(IsBlank(w[-1]) && !IsBlank(*w))) continue;
            last = w;
            if (!ModelRef(P, modelRel, modelRel, w, (int)(b - w), w == first ? REF_TEXTURE : 0, NULL) ||
                !ModelRef(P, modelRel, mtlRel, w, (int)(b - w), 0, NULL)) return false;
        }
        if (lastMustExist && !ModelRef(P, modelRel, mtlRel, last, (int)(b - last), REF_DEP, NULL)) return false;
    }
    return true;
}

// An .mtl file the OBJ importer reads (package-relative): its texture maps
static bool MtlFileDeps(Parser *P, const char *modelRel, const char *mtlRel, bool lastMustExist)
{
    char full[MS_PATH_MAX], e[256];
    if (snprintf(full, sizeof(full), "%s%s", P->d->root, mtlRel) >= (int)sizeof(full)) return Fail(P, "path too long: \"%s\"", mtlRel);
    int size;
    char *mtl = ReadWholeFile(full, MS_FILE_MAX, &size, e, sizeof(e));
    if (!mtl) return Fail(P, "cannot read the material library \"%s\" (or larger than 4 MB)", mtlRel);
    bool ok = MtlDeps(P, modelRel, mtlRel, mtl, (size_t)size, lastMustExist);
    free(mtl);
    return ok;
}

// OBJ: every mtllib line (the name is the rest of the line) and its library's textures;
// also the library assimp falls back to when a mtllib can't be opened (<model>.mtl)
static bool ObjDeps(Parser *P, const char *modelRel, const char *text, size_t size)
{
    const char *cur = text, *end = text + size, *s, *e;
    while (NextLine(&cur, end, &s, &e)) {
        while (s < e && IsBlank(*s)) s++;
        if (e - s <= 6 || strncmp(s, "mtllib", 6) != 0 || !IsBlank(s[6])) continue;
        const char *a = s + 6, *b = e;
        while (a < b && IsBlank(*a)) a++;
        while (b > a && IsBlank(b[-1])) b--;
        if (b <= a) continue;
        char lib[MS_PATH_MAX];
        if (!ModelRef(P, modelRel, modelRel, a, (int)(b - a), REF_DEP, lib) || !MtlFileDeps(P, modelRel, lib, true)) return false;
    }
    size_t l = strlen(modelRel);
    char fallback[MS_PATH_MAX], full[MS_PATH_MAX];
    if (l >= 3 && l < sizeof(fallback)) {
        snprintf(fallback, sizeof(fallback), "%.*smtl", (int)(l - 3), modelRel);
        if (snprintf(full, sizeof(full), "%s%s", P->d->root, fallback) < (int)sizeof(full) && FileExists(full) &&
            !MtlFileDeps(P, modelRel, fallback, false)) return false;
    }
    return true;
}

// The files a model file refers to: glTF buffers and images (.gltf / .glb, either layout:
// assimp tells binary from text by the magic, not the extension), OBJ material libraries
// and their textures (FBX/DAE: not followed)
static bool ModelDeps(Parser *P, const char *modelRel, const char *full)
{
    const char *ext = GetFileExtension(full);
    if (!ext) return true;
    bool gltf = TextIsEqual(TextToLower(ext), ".gltf") || TextIsEqual(TextToLower(ext), ".glb");
    if (!gltf && !TextIsEqual(TextToLower(ext), ".obj")) return true;
    if (gltf) {
        // GLB: 12-byte header, then the JSON chunk (length, type, text); read only that
        // part (the binary chunk can be large)
        FILE *f = fopen(full, "rb");
        if (!f) return Fail(P, "cannot read the model file \"%s\"", modelRel);
        unsigned char h[20];
        bool glb = fread(h, 1, 20, f) == 20 && memcmp(h, "glTF", 4) == 0;
        bool ok = true;
        if (glb) {
            uint32_t len = h[12] | (h[13] << 8) | (h[14] << 16) | ((uint32_t)h[15] << 24);
            char *json = memcmp(h + 16, "JSON", 4) == 0 && len <= MS_FILE_MAX ? malloc(len ? len : 1) : NULL;
            if (json && fread(json, 1, len, f) == len) ok = GltfUriDeps(P, modelRel, json, len);
            else ok = Fail(P, "\"%s\": not a GLB file assimp can read (no complete JSON chunk of at most 4 MB)", modelRel);
            free(json);
        }
        fclose(f);
        if (glb) return ok;
    }
    // .gltf (or a text glTF named .glb) and .obj: the whole text
    int size;
    char e[256];
    char *text = ReadWholeFile(full, MS_FILE_MAX, &size, e, sizeof(e));
    if (!text) return Fail(P, "cannot read the model file \"%s\" (or larger than 4 MB)", modelRel);
    bool ok = gltf ? GltfUriDeps(P, modelRel, text, (size_t)size) : ObjDeps(P, modelRel, text, (size_t)size);
    free(text);
    return ok;
}

bool MS_CheckModelReferences(const char *root, const char *modelRel, char *err, int errSize)
{
    if (errSize > 0) err[0] = 0;
    SceneData *d = calloc(1, sizeof(SceneData));
    char *full = d ? MS_StrDup(TextFormat("%s%s", root, modelRel)) : NULL;
    char *rootCopy = MS_StrDup(root);
    bool ok = false;
    if (d && full && rootCopy) {
        d->root = rootCopy;
        Parser P = { .d = d, .path = modelRel, .err = err, .errSize = errSize };
        ok = ModelDeps(&P, modelRel, full);
        for (int i = 0; i < d->depCount; i++) free(d->deps[i]);
        free(d->deps);
    } else snprintf(err, errSize, "out of memory");
    free(rootCopy);
    free(full);
    free(d);
    return ok;
}

// model "models/x.glb" x y z [rot y p r] [scale s | sx sy sz] [origin model|bottom|center]
//       [material "m"] [material.N "m"] [collide aabb|none|box ...] [shadow on|off]
static bool ParseModel(Parser *P, Token *args, int n, const Placement *pl)
{
    SceneData *d = P->d;
    if (n < 4) return Fail(P, "model takes a file and a position: model \"models/x.glb\" x y z [rot ...] [scale ...] ...", NULL);
    char full[MS_PATH_MAX];
    if (!PackagePath(P, args[0], full, sizeof(full))) return false;
    int file = -1;
    for (int i = 0; i < d->modelFileCount; i++)
        if (strcmp(d->modelFiles[i].path, args[0].s) == 0) file = i;
    if (file < 0) {
        if (!ModelDeps(P, args[0].s, full)) return false;
        char *copy;
        if (!Dup(P, args[0].s, &copy)) return false;
        if (!MS_RESERVE(d->modelPick, d->modelPickCap, d->modelFileCount + 1) ||
            !MS_RESERVE(d->modelFiles, d->modelFileCap, d->modelFileCount + 1)) {
            free(copy);
            return Fail(P, "out of memory", NULL);
        }
        file = d->modelFileCount++;
        MS_ModelFile *F = &d->modelFiles[file];
        F->path = copy;
        F->line = P->line;
    }
    float pos[3];
    for (int i = 0; i < 3; i++)
        if (!NumF(P, args[1 + i], &pos[i])) return false;

    MS_ModelInstance M = { .file = file, .scale = { 1, 1, 1 }, .castShadow = true, .ref = P->cur };
    M.ref.material = P->cur.material > 0 ? P->cur.material : -1;   // `use` (not "default") restyles models
    M.slotFirst = d->modelSlotCount;
    M.colliderFirst = d->localColliderCount;
    for (int t = 4; t < n; ) {
        const char *k = args[t].s;
        if (args[t].quoted) return Fail(P, "expected a model key, got the string \"%s\"", k);
        int nv = 0;
        while (t + 1 + nv < n && IsNumber(args[t + 1 + nv])) nv++;
        Token *v = &args[t + 1];
        int used = nv;
        if (strcmp(k, "rot") == 0) {
            if (nv != 3) return Fail(P, "rot takes 3 numbers: rot yawDeg pitchDeg rollDeg", NULL);
            for (int i = 0; i < 3; i++)
                if (!NumF(P, v[i], &((float *)&M.rotationDeg)[i])) return false;
        } else if (strcmp(k, "scale") == 0) {
            if (nv != 1 && nv != 3) return Fail(P, "scale takes 1 or 3 numbers: scale s | scale sx sy sz", NULL);
            for (int i = 0; i < 3; i++)
                if (!NumF(P, v[nv == 1 ? 0 : i], &((float *)&M.scale)[i])) return false;
            if (!(M.scale.x > 0.0f && M.scale.y > 0.0f && M.scale.z > 0.0f)) return Fail(P, "scale must be > 0", NULL);
        } else if (strcmp(k, "origin") == 0) {
            static const char *const ORIGINS[] = { "model", "bottom", "center" };
            int e;
            if (t + 1 >= n) return Fail(P, "origin takes model, bottom or center", NULL);
            if (!Keyword(P, v[0], ORIGINS, 3, "origin", &e)) return false;
            M.origin = (MS_ModelOrigin)e;
            used = 1;
        } else if (strcmp(k, "shadow") == 0) {
            if (t + 1 >= n) return Fail(P, "shadow takes on or off", NULL);
            if (!OnOff(P, v[0], &M.castShadow)) return false;
            used = 1;
        } else if (strcmp(k, "material") == 0 || strncmp(k, "material.", 9) == 0) {
            if (t + 1 >= n || !v[0].quoted) return Fail(P, "%s takes a material name in double quotes", k);
            int mi = FindMaterial(d, v[0].s);
            if (mi < 0) return Fail(P, "unknown material \"%s\" (define it with 'material' before the model)", v[0].s);
            if (k[8] == '\0') M.ref.material = mi;
            else {
                int slot;
                if (!ParseInt(P, k + 9, 0, MS_MAX_OBJECTS, &slot)) return false;
                MS_SlotMaterial *sm = MS_PUSH(d->modelSlots, d->modelSlotCount, d->modelSlotCap);
                if (!sm) return Fail(P, "out of memory", NULL);
                *sm = (MS_SlotMaterial){ slot, mi };
            }
            used = 1;
        } else if (strcmp(k, "collide") == 0) {
            if (t + 1 >= n) return Fail(P, "collide takes aabb, none or box x0 y0 z0 x1 y1 z1 [box ...]", NULL);
            if (!v[0].quoted && strcmp(v[0].s, "aabb") == 0) { M.collide = MS_COLLIDE_AABB; used = 1; }
            else if (!v[0].quoted && strcmp(v[0].s, "none") == 0) { M.collide = MS_COLLIDE_NONE; used = 1; }
            else if (!v[0].quoted && strcmp(v[0].s, "box") == 0) {
                M.collide = MS_COLLIDE_BOXES;
                d->localColliderCount = M.colliderFirst;    // a repeated `collide` replaces the boxes
                used = 0;
                while (t + 1 + used < n && !v[used].quoted && strcmp(v[used].s, "box") == 0) {
                    float b[6];
                    for (int i = 0; i < 6; i++) {
                        if (t + 2 + used + i >= n) return Fail(P, "collide box takes 6 numbers: box x0 y0 z0 x1 y1 z1", NULL);
                        if (!NumF(P, v[used + 1 + i], &b[i])) return false;
                    }
                    BoundingBox *lc = MS_PUSH(d->localColliders, d->localColliderCount, d->localColliderCap);
                    if (!lc) return Fail(P, "out of memory", NULL);
                    *lc = (BoundingBox){
                        { fminf(b[0], b[3]), fminf(b[1], b[4]), fminf(b[2], b[5]) },
                        { fmaxf(b[0], b[3]), fmaxf(b[1], b[4]), fmaxf(b[2], b[5]) } };
                    used += 7;
                }
            } else return Fail(P, "collide takes aabb, none or box x0 y0 z0 x1 y1 z1 [box ...], not '%s'", v[0].s);
        } else return Fail(P, "unknown model key '%s' (rot scale origin material material.N collide shadow)", k);
        t += 1 + used;
    }
    M.slotCount = d->modelSlotCount - M.slotFirst;
    M.colliderCount = M.collide == MS_COLLIDE_BOXES ? d->localColliderCount - M.colliderFirst : 0;
    int perCopy = M.collide == MS_COLLIDE_AABB ? 1 : M.colliderCount;

    bool shifted = pl->hasAt || pl->reps > 0;
    int cnt[MS_REPEATS_MAX] = { 1, 1, 1 };
    for (int r = 0; r < pl->reps; r++) cnt[r] = pl->repN[r];
    for (int i0 = 0; i0 < cnt[0]; i0++)
    for (int i1 = 0; i1 < cnt[1]; i1++)
    for (int i2 = 0; i2 < cnt[2]; i2++) {
        if (d->boxCount + d->modelCount + d->lightCount >= MS_MAX_OBJECTS)
            return Fail(P, "too many objects (a map makes at most 1000000 boxes, models and lights)", NULL);
        if (d->modelColliderCount + perCopy > MS_MAX_OBJECTS) return Fail(P, "too many model collision boxes (max 1000000)", NULL);
        int idx[MS_REPEATS_MAX] = { i0, i1, i2 };
        float off[3];
        CopyOffset(pl, idx, off);
        BeginCopy(P, cnt, i0, i1, i2);
        MS_ModelInstance c = M;
        c.ref.copy = P->cur.copy;
        c.position = shifted ? (Vector3){ off[0] + pos[0], off[1] + pos[1], off[2] + pos[2] } : (Vector3){ pos[0], pos[1], pos[2] };
        if (!isfinite(c.position.x) || !isfinite(c.position.y) || !isfinite(c.position.z))
            return Fail(P, "coordinate out of range", NULL);
        MS_ModelInstance *slot = MS_PUSH(d->models, d->modelCount, d->modelCap);
        if (!slot) return Fail(P, "out of memory", NULL);
        *slot = c;
        d->modelColliderCount += perCopy;
    }
    return true;
}

// --- Statement keys (MS_EntityId.stmt): `id n`, else derived from the statement's tokens

// Registers the statement P->cur.prim: its explicit id (unique in the file), or an implicit key
// from its text. Identical statements (and the rare hash collision) take the next free key,
// in map order, so the n-th copy of a line keeps its key while the lines before it stay.
static bool AddStatement(Parser *P, const Placement *pl, const Token *tok, int n)
{
    SceneData *d = P->d;
    if (!MS_RESERVE(d->stmtKeys, d->stmtCap, d->stmtCount + 1) || !MS_RESERVE(d->stmtLines, d->stmtLineCap, d->stmtCount + 1) ||
        (2 * (d->stmtCount + 1) > d->stmtSetCap && !GrowStmtSet(d)))
        return Fail(P, "out of memory", NULL);
    unsigned int key;
    int *slot;
    if (pl->hasId) {
        key = (unsigned int)pl->id;
        slot = StmtSlot(d, key);
        if (*slot) {
            char msg[96];
            snprintf(msg, sizeof(msg), "duplicate id %d (also on line %d)", pl->id, d->stmtLines[*slot - 1]);
            return Fail(P, "%s", msg);
        }
    } else {
        unsigned long long h = FNV_OFFSET;
        for (int i = 0; i < n; i++) {
            unsigned char q = tok[i].quoted ? 1 : 2;
            h = Fnv1a64Update(h, &q, 1);
            h = Fnv1a64Update(h, tok[i].s, strlen(tok[i].s));
        }
        key = 0x80000000u | ((unsigned int)(h ^ (h >> 32)) & 0x7fffffffu);
        while (*(slot = StmtSlot(d, key))) key = 0x80000000u | ((key + 1) & 0x7fffffffu);
    }
    d->stmtKeys[d->stmtCount] = key;
    d->stmtLines[d->stmtCount] = P->line;
    *slot = ++d->stmtCount;
    return true;
}

// One statement (already tokenized, at least one token)
static bool ParseStatement(Parser *P, Token *tok, int n)
{
    SceneData *d = P->d;
    bool v2 = d->formatVersion >= 2;
    const char *kw = tok[0].quoted ? "" : tok[0].s;

    if (strcmp(kw, "monolith") == 0) {
        if (P->sawStatement) return Fail(P, "the format line 'monolith N' must be the first statement", NULL);
        int v;
        if (n != 2 || !NumI(P, tok[1], 1, 1000000, &v)) return n != 2 ? Fail(P, "format line: monolith <version>", NULL) : false;
        if (v > MS_FORMAT_VERSION) {
            char m[160];
            snprintf(m, sizeof(m), "map format %d is newer than this runtime supports (%d); update monolith_scene",
                     v, MS_FORMAT_VERSION);
            return Fail(P, "%s", m);
        }
        d->formatVersion = v;
        P->sawStatement = true;
        return true;
    }
    P->sawStatement = true;

    if (strcmp(kw, "group") == 0) {
        const char *name;
        if (n != 2) return Fail(P, "group takes one name in double quotes: group \"name\"", NULL);
        if (!tok[1].quoted) return Fail(P, "group name must be in double quotes: group \"%s\"", tok[1].s);
        if (!Name(P, tok, n, "group", &name)) return false;
        char *copy;
        if (!Dup(P, name, &copy)) return false;
        char **slot = MS_PUSH(d->groups, d->groupCount, d->groupCap);
        if (!slot) { free(copy); return Fail(P, "out of memory", NULL); }
        *slot = copy;
        P->cur.group = d->groupCount - 1;
        return true;
    }

    static const char *const V2_STATEMENTS[] = { "material", "use", "env", "spawn", "viewpoint", "light", "model" };
    for (int i = 0; i < COUNT(V2_STATEMENTS); i++)
        if (!v2 && strcmp(kw, V2_STATEMENTS[i]) == 0)
            return Fail(P, "'%s' needs map format 2: add 'monolith 2' as the first line", kw);

    if (strcmp(kw, "material") == 0) return ParseMaterial(P, tok, n);
    if (strcmp(kw, "use") == 0) {
        const char *name;
        if (n != 2) return Fail(P, "use takes one material name: use \"name\"", NULL);
        if (!Name(P, tok, n, "use", &name)) return false;
        int mi = FindMaterial(d, name);
        if (mi < 0) return Fail(P, "unknown material \"%s\" (define it with 'material' before using it)", name);
        P->cur.material = mi;
        return true;
    }
    if (strcmp(kw, "env") == 0) return ParseEnv(P, tok, n);
    if (strcmp(kw, "spawn") == 0) {
        float v[5] = { 0 };
        if (n != 5 && n != 6) return Fail(P, "spawn takes x y z yawDeg [pitchDeg] (feet position)", NULL);
        for (int i = 1; i < n; i++)
            if (!(i <= 3 ? NumPos(P, tok[i], &v[i - 1]) : NumF(P, tok[i], &v[i - 1]))) return false;
        d->hasSpawn = true;
        d->spawnFeet = (Vector3){ v[0], v[1], v[2] };
        d->spawnYawDeg = v[3];
        d->spawnPitchDeg = v[4];
        return true;
    }
    if (strcmp(kw, "viewpoint") == 0) {
        MS_Viewpoint vp = { 0 };
        const char *name;
        if (n != 8) return Fail(P, "viewpoint takes a name and 6 numbers: viewpoint \"Name\" ex ey ez tx ty tz", NULL);
        if (!Name(P, tok, n, "viewpoint", &name)) return false;
        float v[6];
        for (int i = 0; i < 6; i++)
            if (!NumPos(P, tok[2 + i], &v[i])) return false;
        vp.eye = (Vector3){ v[0], v[1], v[2] };
        vp.target = (Vector3){ v[3], v[4], v[5] };
        if (Vector3Equals(vp.eye, vp.target)) return Fail(P, "viewpoint eye and target must differ", NULL);
        char *copy;
        if (!Dup(P, name, &copy)) return false;
        vp.name = copy;
        MS_Viewpoint *slot = MS_PUSH(d->viewpoints, d->viewpointCount, d->viewpointCap);
        if (!slot) { free(copy); return Fail(P, "out of memory", NULL); }
        *slot = vp;
        return true;
    }

    // Placement prefixes, then a primitive, a light (or a model)
    Placement pl;
    int t;
    if (!ParsePlacement(P, tok, n, &t, &pl)) return false;
    const char *prim = tok[t].quoted ? "" : tok[t].s;
    P->cur.prim = P->prim++;
    if (!AddStatement(P, &pl, tok, n)) return false;
    P->cur.copy = P->cur.part = 0;
    if (strcmp(prim, "light") == 0) {
        if (!v2) return Fail(P, "'light' needs map format 2: add 'monolith 2' as the first line", NULL);
        if (!NoStrings(P, tok, t + 1, n)) return false;
        return ParseLight(P, &tok[t + 1], n - t - 1, &pl);
    }
    if (strcmp(prim, "model") == 0) {
        if (!v2) return Fail(P, "'model' needs map format 2: add 'monolith 2' as the first line", NULL);
        return ParseModel(P, &tok[t + 1], n - t - 1, &pl);
    }
    if (!NoStrings(P, tok, 0, n)) return false;
    for (int i = 0; i < COUNT(PRIMS); i++)
        if (strcmp(prim, PRIMS[i].name) == 0) return ParsePrimitive(P, &PRIMS[i], &tok[t + 1], n - t - 1, &pl);
    if (v2) return Fail(P, "unknown keyword '%s' (expected group, box, block, stairs, waffle, model, light, material, use, env, "
                           "spawn, viewpoint, repeat or at)", prim);
    return Fail(P, "unknown keyword '%s' (expected group, box, block, stairs, waffle, repeat or at)", prim);
}

static bool ParseText(Parser *P, const char *text, int size)
{
    int lineNo = 0, pos = 0;
    P->cur = (MS_Ref){ 0, -1, -1, 0, 0, 0 };
    if (size >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0) pos = 3;     // a UTF-8 byte order mark: skipped
    while (pos < size) {
        int start = pos;
        while (pos < size && text[pos] != '\n') pos++;
        int len = pos - start;
        pos++;                              // skip '\n'
        P->line = ++lineNo;
        if (len > 0 && text[start + len - 1] == '\r') len--;
        if (!MS_RESERVE(P->lineBuf, P->lineCap, len + 1)) return Fail(P, "out of memory", NULL);
        char *line = P->lineBuf;
        memcpy(line, text + start, len);
        line[len] = '\0';
        if ((int)strlen(line) != len) return Fail(P, "unexpected NUL byte", NULL);
        for (int i = 0; i < len; i++)
            if ((unsigned char)line[i] < 32 && line[i] != '\t') return Fail(P, "unexpected control character", NULL);

        int n;
        if (!Tokenize(P, line, &n)) return false;
        if (n == 0) continue;
        P->cur.line = lineNo;
        if (!ParseStatement(P, P->tok, n)) return false;
    }
    P->line = 0;
    if (P->d->boxCount == 0 && P->d->modelCount == 0)
        return Fail(P, P->d->formatVersion >= 2 ? "map is empty (no boxes and no models)" : "map is empty (no boxes)", NULL);
    return true;
}

#endif

// #endregion

// ======================================================================================
// #region Compiled maps (docs/compiled-map.md): fixed-size records, read without parsing
// ======================================================================================
// A compiled map holds what the text parser produces (before FinishParse): every value bit for
// bit, so the rest of the load and the image are the same as from the text. Little-endian,
// fixed-width fields, every record a multiple of 4 bytes, sections at multiples of 8. The
// reader checks the whole file before it uses any of it.

#define CM_VERSION          2           // MS_COMPILED_VERSION: bump on any change to a record or section
#define CM_BYTE_ORDER       0x01020304u
#define CM_NONE             0xFFFFFFFFu // a string field without a string
#define CM_MAX_ENV          4096        // env records (the key tables have ~120 keys)
static const unsigned char CM_MAGIC[8] = { 0x89, 'M', 'A', 'P', 'B', '\r', '\n', 0x1A };

typedef struct {
    uint8_t magic[8];
    uint32_t version, headerSize, byteOrder, formatVersion, sectionCount, sectionTable;
    uint64_t fileSize, sourceHash, sourceSize;
    uint32_t sourceName, flags;
} CmHeader;
typedef struct { uint32_t id, offset, count, recordSize; } CmSection;
typedef struct { int32_t line, prim, group, material, copy, part; } CmRef;      // = MS_Ref
typedef struct { float min[3], max[3]; CmRef ref; } CmBox;
typedef struct {
    uint32_t name, shader, maps[MAP_COUNT];     // maps: albedo, normal, orm, emission
    int32_t line, shaderLine, mapLine[MAP_COUNT];
    uint8_t albedo[4], emission[4];
    float emissionEnergy, normalScale, occlusion, roughness, metalness, specular, uvScale[2], uvOffset[2], alphaCutoff;
    int32_t cull, transparency, blend, priority, wrap;
    uint32_t inherit, unlit, instanced;
    uint32_t uniformFirst, uniformCount, samplerFirst, samplerCount;
} CmMaterial;
typedef struct { uint32_t name, count; float v[4]; } CmUniform;
typedef struct { uint32_t name, path; int32_t line; uint32_t data; } CmSampler;   // data: 1 = data_sampler
typedef struct {
    uint32_t type;
    float position[3], direction[3];
    uint8_t color[4];
    float energy, specular, range, falloff, inner, outer, fogEnergy;
    uint32_t shadow, clip;
    float clipMin[3], clipMax[3];
    CmRef ref;
} CmLight;
typedef struct { uint32_t name; float eye[3], target[3]; } CmViewpoint;
typedef struct { uint32_t path; int32_t line; } CmModelFile;
typedef struct {
    int32_t file;
    float position[3], rotationDeg[3], scale[3];
    uint32_t origin, collide, castShadow;
    int32_t colliderFirst, colliderCount, slotFirst, slotCount;     // in the model colliders / slots sections
    CmRef ref;
} CmModel;
typedef struct { float min[3], max[3]; } CmCollider;
typedef struct { int32_t slot, material; } CmSlot;
typedef struct { uint32_t key, type, value[6]; } CmEnv;         // key: the env key's name; type: T_*
typedef struct { uint32_t hasSpawn; float feet[3], yawDeg, pitchDeg; } CmSpawn;

// The layout is the file format: sizes and offsets must not move (docs/compiled-map.md)
_Static_assert(sizeof(float) == 4, "compiled maps store IEEE-754 binary32 floats");
_Static_assert(sizeof(CmHeader) == 64, "CmHeader");
_Static_assert(offsetof(CmHeader, version) == 8 && offsetof(CmHeader, sectionTable) == 28 &&
               offsetof(CmHeader, fileSize) == 32 && offsetof(CmHeader, sourceHash) == 40 &&
               offsetof(CmHeader, sourceSize) == 48 && offsetof(CmHeader, sourceName) == 56 &&
               offsetof(CmHeader, flags) == 60, "CmHeader offsets");
_Static_assert(sizeof(CmSection) == 16, "CmSection");
_Static_assert(sizeof(CmRef) == 24, "CmRef");
_Static_assert(sizeof(CmBox) == 48 && offsetof(CmBox, ref) == 24, "CmBox");
_Static_assert(sizeof(CmMaterial) == 148 && offsetof(CmMaterial, line) == 24 && offsetof(CmMaterial, albedo) == 48 &&
               offsetof(CmMaterial, emissionEnergy) == 56 && offsetof(CmMaterial, cull) == 100 &&
               offsetof(CmMaterial, inherit) == 120 && offsetof(CmMaterial, uniformFirst) == 132, "CmMaterial");
_Static_assert(sizeof(CmUniform) == 24 && offsetof(CmUniform, v) == 8, "CmUniform");
_Static_assert(sizeof(CmSampler) == 16, "CmSampler");
_Static_assert(sizeof(CmLight) == 116 && offsetof(CmLight, color) == 28 && offsetof(CmLight, energy) == 32 &&
               offsetof(CmLight, shadow) == 60 && offsetof(CmLight, clipMin) == 68 && offsetof(CmLight, ref) == 92, "CmLight");
_Static_assert(sizeof(CmViewpoint) == 28, "CmViewpoint");
_Static_assert(sizeof(CmModelFile) == 8, "CmModelFile");
_Static_assert(sizeof(CmModel) == 92 && offsetof(CmModel, origin) == 40 && offsetof(CmModel, colliderFirst) == 52 &&
               offsetof(CmModel, ref) == 68, "CmModel");
_Static_assert(sizeof(CmCollider) == 24 && sizeof(CmSlot) == 8, "CmCollider, CmSlot");
_Static_assert(sizeof(CmEnv) == 32 && offsetof(CmEnv, value) == 8, "CmEnv");
_Static_assert(sizeof(CmSpawn) == 24, "CmSpawn");
// T_* values are stored in CmEnv.type: the key types only ever get new values at the end
_Static_assert(T_F == 0 && T_PATH == 8 && T_AMBMODE == 14, "T_* values are part of the compiled format");

enum {
    CM_STRINGS = 1, CM_BOXES, CM_GROUPS, CM_MATERIALS, CM_UNIFORMS, CM_SAMPLERS, CM_LIGHTS, CM_VIEWPOINTS,
    CM_MODEL_FILES, CM_MODELS, CM_COLLIDERS, CM_SLOTS, CM_STATEMENTS, CM_ENV, CM_SPAWN, CM_DEPS,
    CM_SECTIONS = CM_DEPS
};
// Per section id: name (messages), record size, most records
static const struct { const char *name; uint32_t size; uint32_t max; } CM_SECTION[CM_SECTIONS + 1] = {
    [CM_STRINGS]     = { "strings", 1, MS_MAX_COMPILED_BYTES },
    [CM_BOXES]       = { "boxes", sizeof(CmBox), MS_MAX_OBJECTS },
    [CM_GROUPS]      = { "groups", 4, MS_MAX_OBJECTS },
    [CM_MATERIALS]   = { "materials", sizeof(CmMaterial), MS_MAX_OBJECTS },
    [CM_UNIFORMS]    = { "uniforms", sizeof(CmUniform), MS_MAX_OBJECTS },
    [CM_SAMPLERS]    = { "samplers", sizeof(CmSampler), MS_MAX_OBJECTS },
    [CM_LIGHTS]      = { "lights", sizeof(CmLight), MS_MAX_OBJECTS },
    [CM_VIEWPOINTS]  = { "viewpoints", sizeof(CmViewpoint), MS_MAX_OBJECTS },
    [CM_MODEL_FILES] = { "model files", sizeof(CmModelFile), MS_MAX_OBJECTS },
    [CM_MODELS]      = { "models", sizeof(CmModel), MS_MAX_OBJECTS },
    [CM_COLLIDERS]   = { "model colliders", sizeof(CmCollider), MS_MAX_OBJECTS },
    [CM_SLOTS]       = { "model slots", sizeof(CmSlot), MS_MAX_OBJECTS },
    [CM_STATEMENTS]  = { "statements", 4, MS_MAX_OBJECTS },
    [CM_ENV]         = { "environment", sizeof(CmEnv), CM_MAX_ENV },
    [CM_SPAWN]       = { "spawn", sizeof(CmSpawn), 1 },
    [CM_DEPS]        = { "dependencies", 4, MS_MAX_OBJECTS },
};

static bool IsCompiledMap(const char *bytes, int size)
{
    return size >= (int)sizeof(CM_MAGIC) && memcmp(bytes, CM_MAGIC, sizeof(CM_MAGIC)) == 0;
}

// A reader over the checked file: the bytes, its sections, and where messages go
typedef struct {
    SceneData *d;
    const unsigned char *bytes;
    uint32_t size;
    CmSection sec[CM_SECTIONS + 1];
    const char *str;                // the string table (NUL-terminated: checked)
    uint32_t strSize;
    char *err;
    int errSize;
} CmReader;

static bool CmFail(CmReader *R, const char *fmt, ...)
{
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    snprintf(R->err, R->errSize, "%s: %s", R->d->path, msg);
    return false;
}

// Record i of section `id` (in range: checked by the caller's loop over the section's count),
// copied out (sections are aligned, but a copy keeps the reads free of aliasing questions)
static void CmRecord(const CmReader *R, int id, uint32_t i, void *out)
{
    memcpy(out, R->bytes + R->sec[id].offset + (size_t)i * CM_SECTION[id].size, CM_SECTION[id].size);
}

// A string field: *out = the string (NULL for CM_NONE when `optional`), false with a message otherwise
static bool CmString(CmReader *R, uint32_t off, bool optional, const char *what, const char **out)
{
    if (off == CM_NONE && optional) { *out = NULL; return true; }
    if (off >= R->strSize) return CmFail(R, "%s: string offset %u outside the string table", what, off);
    *out = R->str + off;
    return true;
}

// A heap copy of a string field (a name: must not be empty)
static bool CmName(CmReader *R, uint32_t off, const char *what, char **out)
{
    const char *s;
    if (!CmString(R, off, false, what, &s)) return false;
    if (!s[0]) return CmFail(R, "%s: empty name", what);
    if (!(*out = MS_StrDup(s))) return CmFail(R, "out of memory");
    return true;
}

// A package-relative path field: inside the package and present, as the parser requires;
// *out = a heap copy (NULL for CM_NONE when `optional`)
static bool CmPath(CmReader *R, uint32_t off, bool optional, const char *what, char **out)
{
    const char *s;
    *out = NULL;
    if (!CmString(R, off, optional, what, &s)) return false;
    if (!s) return true;
    if (!InsidePackage(s)) return CmFail(R, "%s: path must stay inside the package: \"%s\"", what, s);
    char full[MS_PATH_MAX];
    if (snprintf(full, sizeof(full), "%s%s", R->d->root, s) >= (int)sizeof(full)) return CmFail(R, "path too long: \"%s\"", s);
    if (!FileExists(full)) return CmFail(R, "file not found: \"%s\" (paths are relative to the package folder)", s);
    if (!(*out = MS_StrDup(s))) return CmFail(R, "out of memory");
    return true;
}

static bool Finite3(const float *v) { return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]); }
static bool InCoordLimit(const float *v) { return Finite3(v) && fabsf(v[0]) <= MS_COORD_LIMIT && fabsf(v[1]) <= MS_COORD_LIMIT && fabsf(v[2]) <= MS_COORD_LIMIT; }
static Vector3 V3(const float *v) { return (Vector3){ v[0], v[1], v[2] }; }

// An MS_Ref; boxes and lights always use a material (the active `use`), models may have none (-1)
static bool CmReadRef(CmReader *R, const CmRef *c, bool modelMaterial, const char *what, uint32_t i, MS_Ref *out)
{
    const SceneData *d = R->d;
    if (c->line < 0 || c->prim < 0 || c->prim >= d->stmtCount || c->group < -1 || c->group >= d->groupCount ||
        c->material < (modelMaterial ? -1 : 0) || c->material >= d->materialCount || c->copy < 0 || c->part < 0)
        return CmFail(R, "%s %u: source reference out of range", what, i);
    *out = (MS_Ref){ c->line, c->prim, c->group, c->material, c->copy, c->part };
    return true;
}

// The header and the section table: sizes, versions, bounds. Nothing else is read before this.
static bool CmReadHeader(CmReader *R, CmHeader *h)
{
    if (R->size < sizeof(CmHeader)) return CmFail(R, "compiled map: file too short (%u bytes)", R->size);
    memcpy(h, R->bytes, sizeof(*h));
    if (memcmp(h->magic, CM_MAGIC, sizeof(CM_MAGIC)) != 0) return CmFail(R, "not a compiled map");
    if (h->byteOrder != CM_BYTE_ORDER)
        return CmFail(R, "compiled map: unsupported byte order (compiled maps are little-endian; big-endian hosts are not supported)");
    if (h->version != CM_VERSION)
        return CmFail(R, "compiled map format %u, this runtime reads format %d: compile the map again", h->version, CM_VERSION);
    if (h->headerSize != sizeof(CmHeader) || h->flags != 0) return CmFail(R, "compiled map: bad header");
    if (h->fileSize != R->size)
        return CmFail(R, "compiled map: the header says %llu bytes, the file has %u (truncated or damaged)",
                      (unsigned long long)h->fileSize, R->size);
    if (h->formatVersion < 1 || h->formatVersion > MS_FORMAT_VERSION)
        return CmFail(R, "map format %u is newer than this runtime supports (%d); update monolith_scene", h->formatVersion, MS_FORMAT_VERSION);
    if (h->sectionCount != CM_SECTIONS || h->sectionTable < sizeof(CmHeader) || h->sectionTable % 4 ||
        (uint64_t)h->sectionTable + (uint64_t)h->sectionCount * sizeof(CmSection) > R->size)
        return CmFail(R, "compiled map: bad section table");
    memset(R->sec, 0, sizeof(R->sec));
    for (uint32_t i = 0; i < h->sectionCount; i++) {
        CmSection s;
        memcpy(&s, R->bytes + h->sectionTable + i * sizeof(CmSection), sizeof(s));
        if (s.id < 1 || s.id > CM_SECTIONS) return CmFail(R, "compiled map: unknown section %u", s.id);
        const char *name = CM_SECTION[s.id].name;
        if (R->sec[s.id].id) return CmFail(R, "compiled map: section '%s' appears twice", name);
        if (s.recordSize != CM_SECTION[s.id].size)
            return CmFail(R, "compiled map: section '%s' has %u-byte records, expected %u", name, s.recordSize, CM_SECTION[s.id].size);
        if (s.count > CM_SECTION[s.id].max) return CmFail(R, "compiled map: section '%s' has too many records (%u)", name, s.count);
        if (s.offset < sizeof(CmHeader) || s.offset % 4 || (uint64_t)s.offset + (uint64_t)s.count * s.recordSize > R->size)
            return CmFail(R, "compiled map: section '%s' lies outside the file", name);
        R->sec[s.id] = s;
    }
    // (all CM_SECTIONS ids present: sectionCount of them, none twice, none unknown)
    if (R->sec[CM_SPAWN].count != 1) return CmFail(R, "compiled map: section 'spawn' must have one record");
    uint32_t n = R->sec[CM_STRINGS].count;
    R->str = (const char *)R->bytes + R->sec[CM_STRINGS].offset;
    R->strSize = n;
    if (n == 0 || R->str[0] != '\0' || R->str[n - 1] != '\0') return CmFail(R, "compiled map: bad string table");
    for (uint32_t i = 0; i < n; i++)
        if ((unsigned char)R->str[i] < 0x20 && R->str[i] != '\0') return CmFail(R, "compiled map: control character in the string table");
    if ((uint64_t)R->sec[CM_BOXES].count + R->sec[CM_MODELS].count + R->sec[CM_LIGHTS].count > MS_MAX_OBJECTS)
        return CmFail(R, "too many objects (a map makes at most 1000000 boxes, models and lights)");
    return true;
}

static bool CmReadMaterials(CmReader *R)
{
    SceneData *d = R->d;
    uint32_t n = R->sec[CM_MATERIALS].count;
    if (n == 0) return CmFail(R, "compiled map: no materials (material 0 is \"default\")");
    if (!MS_RESERVE(d->materials, d->materialCap, (int)n) || !MS_RESERVE(d->matDefs, d->matDefCap, (int)n)) return CmFail(R, "out of memory");
    uint32_t uniformsUsed = 0, samplersUsed = 0;
    for (uint32_t i = 0; i < n; i++) {
        CmMaterial c;
        CmRecord(R, CM_MATERIALS, i, &c);
        MS_Material *M = &d->materials[i];
        MatDef *D = &d->matDefs[i];
        d->materialCount = (int)i + 1;          // (from here FreeData frees what this record got)
        *M = (MS_Material){ .material = R3D_MATERIAL_BASE };
        *D = (MatDef){ 0 };
        char *name, *shader;
        if (!CmName(R, c.name, "material", &name)) return false;
        M->name = name;
        if (i == 0 && strcmp(name, "default") != 0) return CmFail(R, "compiled map: material 0 must be \"default\"");
        if (!CmPath(R, c.shader, true, "material shader", &shader)) return false;
        M->shader = shader;
        for (int k = 0; k < MAP_COUNT; k++)
            if (!CmPath(R, c.maps[k], true, "material texture", &D->maps[k])) return false;
        float f[] = { c.emissionEnergy, c.normalScale, c.occlusion, c.roughness, c.metalness, c.specular,
                      c.uvScale[0], c.uvScale[1], c.uvOffset[0], c.uvOffset[1], c.alphaCutoff };
        for (int k = 0; k < COUNT(f); k++)
            if (!isfinite(f[k])) return CmFail(R, "material \"%s\": number out of range", name);
        bool lines = c.line >= 0 && c.shaderLine >= 0;
        for (int k = 0; k < MAP_COUNT; k++) lines = lines && c.mapLine[k] >= 0;
        if (!lines || c.cull < 0 || c.cull > 2 || c.transparency < 0 || c.transparency > 2 || c.blend < 0 || c.blend > 4 ||
            c.priority < -1000000 || c.priority > 1000000 || c.wrap < 0 || c.wrap > 2 || c.inherit > 15 || c.unlit > 1 ||
            c.instanced > 1)
            return CmFail(R, "material \"%s\": value out of range", name);
        R3D_Material *m = &M->material;
        M->line = c.line;
        m->albedo.color = (Color){ c.albedo[0], c.albedo[1], c.albedo[2], c.albedo[3] };
        m->emission.color = (Color){ c.emission[0], c.emission[1], c.emission[2], c.emission[3] };
        m->emission.energy = c.emissionEnergy;
        m->normal.scale = c.normalScale;
        m->orm.occlusion = c.occlusion;
        m->orm.roughness = c.roughness;
        m->orm.metalness = c.metalness;
        m->orm.specular = c.specular;
        m->uvScale = (Vector2){ c.uvScale[0], c.uvScale[1] };
        m->uvOffset = (Vector2){ c.uvOffset[0], c.uvOffset[1] };
        m->alphaCutoff = c.alphaCutoff;
        m->cullMode = (R3D_CullMode)c.cull;
        m->transparencyMode = (R3D_TransparencyMode)c.transparency;
        m->blendMode = (R3D_BlendMode)c.blend;
        m->priority = c.priority;
        m->unlit = c.unlit == 1;
        D->shaderLine = c.shaderLine;
        for (int k = 0; k < MAP_COUNT; k++) D->mapLine[k] = c.mapLine[k];
        D->wrap = c.wrap;
        D->inherit = c.inherit;
        D->instanced = c.instanced == 1;

        // Each material's uniforms and samplers follow the previous material's (as written): ranges
        // can't overlap, so the records are read once (shared ranges would multiply the memory)
        if (c.uniformFirst != uniformsUsed || c.samplerFirst != samplersUsed ||
            (uint64_t)c.uniformFirst + c.uniformCount > R->sec[CM_UNIFORMS].count ||
            (uint64_t)c.samplerFirst + c.samplerCount > R->sec[CM_SAMPLERS].count)
            return CmFail(R, "material \"%s\": uniform or sampler range out of range", name);
        uniformsUsed += c.uniformCount;
        samplersUsed += c.samplerCount;
        if (!MS_RESERVE(D->uniforms, D->uniformCap, (int)c.uniformCount) || !MS_RESERVE(D->samplers, D->samplerCap, (int)c.samplerCount))
            return CmFail(R, "out of memory");
        for (uint32_t k = 0; k < c.uniformCount; k++) {
            CmUniform u;
            CmRecord(R, CM_UNIFORMS, c.uniformFirst + k, &u);
            MatUniform *U = &D->uniforms[D->uniformCount++];
            if (!CmName(R, u.name, "uniform", &U->name)) return false;
            if (u.count < 1 || u.count > 4 || !isfinite(u.v[0]) || !isfinite(u.v[1]) || !isfinite(u.v[2]) || !isfinite(u.v[3]))
                return CmFail(R, "material \"%s\": uniform '%s' out of range", name, U->name);
            U->count = (int)u.count;
            memcpy(U->v, u.v, sizeof(U->v));
        }
        for (uint32_t k = 0; k < c.samplerCount; k++) {
            CmSampler sm;
            CmRecord(R, CM_SAMPLERS, c.samplerFirst + k, &sm);
            MatSampler *S = &D->samplers[D->samplerCount++];
            if (!CmName(R, sm.name, "sampler", &S->name) || !CmPath(R, sm.path, false, "sampler", &S->path)) return false;
            if (sm.line < 0 || sm.data > 1) return CmFail(R, "material \"%s\": sampler line or kind out of range", name);
            S->line = sm.line;
            S->data = sm.data == 1;
        }
    }
    if (uniformsUsed != R->sec[CM_UNIFORMS].count || samplersUsed != R->sec[CM_SAMPLERS].count)
        return CmFail(R, "compiled map: uniforms or samplers no material uses");
    return true;
}

// One env record: the key's table entry by name, the type it must have, the value checked as
// the text parser checks it, then stored where the text parser stores it
static bool CmReadEnvRecord(CmReader *R, const CmEnv *c)
{
    EnvDef *e = &R->d->env;
    const char *key;
    if (!CmString(R, c->key, false, "env key", &key)) return false;
    int type = -1;
    void *dst = NULL;
    int sky = -1;
    bool amb = false;
    for (int i = 0; i < COUNT(ENV_KEYS) && !dst; i++)
        if (strcmp(ENV_KEYS[i].key, key) == 0) { type = ENV_KEYS[i].type; dst = (char *)&e->env + ENV_KEYS[i].off; }
    for (int i = 0; i < COUNT(DEF_KEYS) && !dst; i++)
        if (strcmp(DEF_KEYS[i].key, key) == 0) { type = DEF_KEYS[i].type; dst = (char *)e + DEF_KEYS[i].off; }
    if (!dst) {
        amb = strncmp(key, "ambientSky.", 11) == 0;
        if (amb || strncmp(key, "sky.", 4) == 0) sky = SkyKeyIndex(key + (amb ? 11 : 4));
        if (sky >= 0) { type = SKY_KEYS[sky].type; dst = (char *)(amb ? &e->ambient : &e->sky) + SKY_KEYS[sky].off; }
    }
    if (!dst) return CmFail(R, "unknown env key '%s'", key);
    if (c->type != (uint32_t)type) return CmFail(R, "env key '%s': wrong value type", key);

    const uint32_t *w = c->value;
    float f[6];
    memcpy(f, w, sizeof(f));
    int words = 1;
    bool ok = true;
    switch (type) {
        case T_F: case T_DEG: ok = isfinite(f[0]); break;
        case T_I: ok = (int32_t)w[0] >= -1000000000 && (int32_t)w[0] <= 1000000000; break;
        case T_B: ok = w[0] <= 1; break;
        case T_COLOR: words = 4; ok = w[0] <= 255 && w[1] <= 255 && w[2] <= 255 && w[3] <= 255; break;
        case T_V3: words = 3; ok = Finite3(f); break;
        case T_QUAT: words = 4; ok = Finite3(f) && isfinite(f[3]); break;
        case T_BOX: words = 6; ok = Finite3(f) && Finite3(f + 3) && f[0] <= f[3] && f[1] <= f[4] && f[2] <= f[5]; break;
        case T_PATH: break;
        default: ok = w[0] < (uint32_t)ENUM_COUNT[type]; break;
    }
    for (int k = words; k < 6; k++) ok = ok && w[k] == 0;
    if (!ok) return CmFail(R, "env key '%s': value out of range", key);
    switch (type) {
        case T_B: *(bool *)dst = w[0] == 1; break;
        case T_COLOR: *(Color *)dst = (Color){ (unsigned char)w[0], (unsigned char)w[1], (unsigned char)w[2], (unsigned char)w[3] }; break;
        case T_V3: case T_QUAT: case T_BOX: memcpy(dst, f, (size_t)words * sizeof(float)); break;
        case T_PATH: {
            char *path;
            if (!CmPath(R, w[0], false, key, &path)) return false;
            bool fits = strlen(path) < MS_PATH_MAX;
            if (fits) snprintf(dst, MS_PATH_MAX, "%s", path);
            free(path);
            if (!fits) return CmFail(R, "env key '%s': path too long", key);
            break;
        }
        default: memcpy(dst, w, 4); break;     // float, int, enums (all 4 bytes)
    }
    char msg[160];
    if (!CheckEnvValue(key, type, dst, msg, sizeof(msg))) return CmFail(R, "%s", msg);
    if (strcmp(key, "shadow.home") == 0) e->homeSet = true;
    if (strcmp(key, "fogCache.region") == 0) e->fogRegionSet = true;
    if (sky >= 0 && amb) e->ambientSet |= 1u << sky;
    if (sky >= 0 && !amb && strcmp(SKY_KEYS[sky].key, "sunDirection") == 0) e->skySunDirSet = true;
    return true;
}

// Fills d (after InitData) from a compiled map: the same SceneData the text parser would have made
static bool ReadCompiled(SceneData *d, const char *bytes, int size, char *err, int errSize)
{
    CmReader R = { .d = d, .bytes = (const unsigned char *)bytes, .size = (uint32_t)size, .err = err, .errSize = errSize };
    CmHeader h;
    if (size < 0 || !CmReadHeader(&R, &h)) return false;
    d->formatVersion = (int)h.formatVersion;

    // The source file's name, for messages that cite a map line ("forest.map:12: ...")
    const char *src;
    if (!CmString(&R, h.sourceName, false, "source name", &src)) return false;
    if (!src[0] || strchr(src, '/') || strchr(src, '\\')) return CmFail(&R, "compiled map: bad source name");
    char full[MS_PATH_MAX];
    if (snprintf(full, sizeof(full), "%s%s", d->root, src) >= (int)sizeof(full)) return CmFail(&R, "path too long: \"%s\"", src);
    char *srcPath = MS_StrDup(full);
    if (!srcPath) return CmFail(&R, "out of memory");
    free(d->srcPath);
    d->srcPath = srcPath;
#ifdef MONOLITH_EDITOR
    d->compiled = true;
    d->srcHash = h.sourceHash;
    d->srcSize = h.sourceSize;
#endif

    // Statements first: every MS_Ref points at one. Keys unique (MS_FindEntity relies on it)
    uint32_t n = R.sec[CM_STATEMENTS].count;
    if (!MS_RESERVE(d->stmtKeys, d->stmtCap, (int)n)) return CmFail(&R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t key;
        CmRecord(&R, CM_STATEMENTS, i, &key);
        if (key == 0) return CmFail(&R, "compiled map: statement %u has key 0", i);
        if (2 * (d->stmtCount + 1) > d->stmtSetCap && !GrowStmtSet(d)) return CmFail(&R, "out of memory");
        int *slot = StmtSlot(d, key);
        if (*slot) return CmFail(&R, "compiled map: duplicate statement key %08x", key);
        d->stmtKeys[d->stmtCount] = key;
        *slot = ++d->stmtCount;
    }

    n = R.sec[CM_GROUPS].count;
    if (!MS_RESERVE(d->groups, d->groupCap, (int)n)) return CmFail(&R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t name;
        CmRecord(&R, CM_GROUPS, i, &name);
        if (!CmName(&R, name, "group", &d->groups[i])) return false;
        d->groupCount = (int)i + 1;
    }

    if (!CmReadMaterials(&R)) return false;

    n = R.sec[CM_BOXES].count;
    if (!MS_RESERVE(d->boxes, d->boxCap, (int)n) || !MS_RESERVE(d->boxRefs, d->boxRefCap, (int)n)) return CmFail(&R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        CmBox c;
        CmRecord(&R, CM_BOXES, i, &c);
        BoundingBox b = { V3(c.min), V3(c.max) };
        // The parser's guarantees: corners ordered, coordinates and sizes finite, map order
        if (!Finite3(c.min) || !Finite3(c.max) || !(b.min.x <= b.max.x && b.min.y <= b.max.y && b.min.z <= b.max.z) ||
            !isfinite(b.max.x - b.min.x) || !isfinite(b.max.y - b.min.y) || !isfinite(b.max.z - b.min.z))
            return CmFail(&R, "box %u: coordinate or size out of range", i);
        if (!CmReadRef(&R, &c.ref, false, "box", i, &d->boxRefs[i])) return false;
        if (i > 0 && d->boxRefs[i].prim < d->boxRefs[i - 1].prim) return CmFail(&R, "box %u: out of map order", i);
        d->boxes[i] = b;
        d->boxCount = (int)i + 1;
    }

    n = R.sec[CM_MODEL_FILES].count;
    if (!MS_RESERVE(d->modelFiles, d->modelFileCap, (int)n) || !MS_RESERVE(d->modelPick, d->modelPickCap, (int)n))
        return CmFail(&R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        CmModelFile c;
        CmRecord(&R, CM_MODEL_FILES, i, &c);
        char *path;
        if (!CmPath(&R, c.path, false, "model file", &path)) return false;
        d->modelFiles[i] = (MS_ModelFile){ .path = path, .line = c.line };
        d->modelFileCount = (int)i + 1;
        if (c.line < 0) return CmFail(&R, "model file \"%s\": line out of range", path);
    }

    n = R.sec[CM_COLLIDERS].count;
    if (!MS_RESERVE(d->localColliders, d->localColliderCap, (int)n)) return CmFail(&R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        CmCollider c;
        CmRecord(&R, CM_COLLIDERS, i, &c);
        if (!Finite3(c.min) || !Finite3(c.max) || !(c.min[0] <= c.max[0] && c.min[1] <= c.max[1] && c.min[2] <= c.max[2]))
            return CmFail(&R, "model collider %u: coordinate out of range", i);
        d->localColliders[d->localColliderCount++] = (BoundingBox){ V3(c.min), V3(c.max) };
    }

    n = R.sec[CM_SLOTS].count;
    if (!MS_RESERVE(d->modelSlots, d->modelSlotCap, (int)n)) return CmFail(&R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        CmSlot c;
        CmRecord(&R, CM_SLOTS, i, &c);
        if (c.slot < 0 || c.slot > MS_MAX_OBJECTS || c.material < 0 || c.material >= d->materialCount)
            return CmFail(&R, "model slot %u: out of range", i);
        d->modelSlots[d->modelSlotCount++] = (MS_SlotMaterial){ c.slot, c.material };
    }

    n = R.sec[CM_MODELS].count;
    if (!MS_RESERVE(d->models, d->modelCap, (int)n)) return CmFail(&R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        CmModel c;
        CmRecord(&R, CM_MODELS, i, &c);
        MS_ModelInstance M = { 0 };
        if (c.file < 0 || c.file >= d->modelFileCount || !Finite3(c.position) || !Finite3(c.rotationDeg) ||
            !Finite3(c.scale) || !(c.scale[0] > 0.0f && c.scale[1] > 0.0f && c.scale[2] > 0.0f) || c.origin > 2 ||
            c.collide > 2 || c.castShadow > 1)
            return CmFail(&R, "model %u: value out of range", i);
        if (c.colliderFirst < 0 || c.colliderCount < 0 || (int64_t)c.colliderFirst + c.colliderCount > d->localColliderCount ||
            (c.collide != MS_COLLIDE_BOXES && c.colliderCount != 0) ||
            c.slotFirst < 0 || c.slotCount < 0 || (int64_t)c.slotFirst + c.slotCount > d->modelSlotCount)
            return CmFail(&R, "model %u: collider or slot range out of range", i);
        if (!CmReadRef(&R, &c.ref, true, "model", i, &M.ref)) return false;
        if (i > 0 && M.ref.prim < d->models[i - 1].ref.prim) return CmFail(&R, "model %u: out of map order", i);
        int perCopy = c.collide == MS_COLLIDE_AABB ? 1 : c.colliderCount;
        if (d->modelColliderCount + perCopy > MS_MAX_OBJECTS) return CmFail(&R, "too many model collision boxes (max 1000000)");
        d->modelColliderCount += perCopy;
        M.file = c.file;
        M.position = V3(c.position);
        M.rotationDeg = V3(c.rotationDeg);
        M.scale = V3(c.scale);
        M.origin = (MS_ModelOrigin)c.origin;
        M.collide = (MS_ModelCollide)c.collide;
        M.castShadow = c.castShadow == 1;
        M.colliderFirst = c.colliderFirst;
        M.colliderCount = c.colliderCount;
        M.slotFirst = c.slotFirst;
        M.slotCount = c.slotCount;
        d->models[d->modelCount++] = M;
    }

    n = R.sec[CM_LIGHTS].count;
    if (!MS_RESERVE(d->lights, d->lightCap, (int)n)) return CmFail(&R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        CmLight c;
        CmRecord(&R, CM_LIGHTS, i, &c);
        // What the parser guarantees: the numbers as written are finite (positions within the
        // coordinate limit, but `repeat` / `at` may shift them past it), a point light has no
        // direction and the others a normalised one, a spot's inner angle is at most its outer
        // one, the clip box is ordered and not flat. (Numbers so large or small that normalising
        // or shifting them breaks this fail when the map is compiled, which reads the result back.)
        float f[] = { c.energy, c.specular, c.range, c.falloff, c.inner, c.outer, c.fogEnergy };
        bool ok = c.type <= 2 && Finite3(c.position) && Finite3(c.direction) && c.shadow <= 1 && c.clip <= 1;
        for (int k = 0; k < COUNT(f); k++) ok = ok && isfinite(f[k]);
        float len = ok ? Vector3Length(V3(c.direction)) : 0.0f;
        if (c.type == R3D_LIGHT_OMNI) ok = ok && c.direction[0] == 0.0f && c.direction[1] == 0.0f && c.direction[2] == 0.0f;
        else ok = ok && fabsf(len - 1.0f) <= 1e-3f;
        if (c.type == R3D_LIGHT_SPOT) ok = ok && c.inner <= c.outer;
        if (c.clip) ok = ok && Finite3(c.clipMin) && Finite3(c.clipMax) && c.clipMin[0] < c.clipMax[0] &&
                         c.clipMin[1] < c.clipMax[1] && c.clipMin[2] < c.clipMax[2];
        if (!ok) return CmFail(&R, "light %u: value out of range", i);
        MS_LightDef L = { 0 };
        if (!CmReadRef(&R, &c.ref, false, "light", i, &L.ref)) return false;
        if (i > 0 && L.ref.prim < d->lights[i - 1].ref.prim) return CmFail(&R, "light %u: out of map order", i);
        L.light.type = (R3D_LightType)c.type;
        L.light.position = V3(c.position);
        L.light.direction = V3(c.direction);
        L.light.color = (Color){ c.color[0], c.color[1], c.color[2], c.color[3] };
        L.light.energy = c.energy;
        L.light.specular = c.specular;
        L.light.range = c.range;
        L.light.falloff = c.falloff;
        L.light.innerCutOff = c.inner;
        L.light.outerCutOff = c.outer;
        L.light.fogEnergy = c.fogEnergy;
        L.shadow = c.shadow == 1;
        L.clip = c.clip == 1;
        if (L.clip) L.clipBox = (BoundingBox){ V3(c.clipMin), V3(c.clipMax) };
        d->lights[d->lightCount++] = L;
    }

    n = R.sec[CM_VIEWPOINTS].count;
    if (!MS_RESERVE(d->viewpoints, d->viewpointCap, (int)n)) return CmFail(&R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        CmViewpoint c;
        CmRecord(&R, CM_VIEWPOINTS, i, &c);
        MS_Viewpoint *v = &d->viewpoints[i];
        char *name;
        if (!CmName(&R, c.name, "viewpoint", &name)) return false;
        *v = (MS_Viewpoint){ name, V3(c.eye), V3(c.target) };
        d->viewpointCount = (int)i + 1;
        if (!InCoordLimit(c.eye) || !InCoordLimit(c.target) || Vector3Equals(v->eye, v->target))
            return CmFail(&R, "viewpoint \"%s\": position out of range", name);
    }

    CmSpawn sp;
    CmRecord(&R, CM_SPAWN, 0, &sp);
    if (sp.hasSpawn > 1 || !InCoordLimit(sp.feet) || !isfinite(sp.yawDeg) || !isfinite(sp.pitchDeg))
        return CmFail(&R, "spawn: value out of range");
    d->hasSpawn = sp.hasSpawn == 1;
    d->spawnFeet = V3(sp.feet);
    d->spawnYawDeg = sp.yawDeg;
    d->spawnPitchDeg = sp.pitchDeg;

    n = R.sec[CM_ENV].count;
    for (uint32_t i = 0; i < n; i++) {
        CmEnv c;
        CmRecord(&R, CM_ENV, i, &c);
        if (!CmReadEnvRecord(&R, &c)) return false;
    }

    // The files hot reload watches (editor builds; [0], the compiled file, is there from InitData)
    n = R.sec[CM_DEPS].count;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off;
        const char *rel;
        CmRecord(&R, CM_DEPS, i, &off);
        if (!CmString(&R, off, false, "dependency", &rel)) return false;
        if (!InsidePackage(rel)) return CmFail(&R, "dependency: path must stay inside the package: \"%s\"", rel);
#ifdef MONOLITH_EDITOR
        if (snprintf(full, sizeof(full), "%s%s", d->root, rel) >= (int)sizeof(full)) return CmFail(&R, "path too long: \"%s\"", rel);
        char *dep = MS_StrDup(full);
        char **slot = dep ? MS_PUSH(d->deps, d->depCount, d->depCap) : NULL;
        if (!slot) { free(dep); return CmFail(&R, "out of memory"); }
        *slot = dep;
#endif
    }

    if (d->boxCount == 0 && d->modelCount == 0)
        return CmFail(&R, d->formatVersion >= 2 ? "map is empty (no boxes and no models)" : "map is empty (no boxes)");
    return true;
}

#ifdef MONOLITH_EDITOR
// The source text of a compiled map (next to it) against the hash and size it was compiled from
static MS_SourceState SourceState(const SceneData *d)
{
    if (!d->compiled) return MS_SOURCE_TEXT;
    FILE *f = fopen(d->srcPath, "rb");
    if (!f) return MS_SOURCE_MISSING;
    unsigned long long h = FNV_OFFSET;
    long long n = 0;
    unsigned char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        h = Fnv1a64Update(h, buf, got);
        n += (long long)got;
    }
    fclose(f);
    return h == d->srcHash && n == d->srcSize ? MS_SOURCE_CURRENT : MS_SOURCE_STALE;
}

static void WarnStale(const SceneData *d)
{
    if (SourceState(d) == MS_SOURCE_STALE)
        TraceLog(LOG_WARNING, "monolith_scene: %s was compiled from an older %s: compile it again", d->path, d->srcPath);
}

// --- Writer (editor): a parsed SceneData (before FinishParse) as a compiled map

typedef struct {
    unsigned char *b;
    int n, cap;
} CmBuf;

typedef struct {
    CmBuf sec[CM_SECTIONS + 1];     // section contents
    uint32_t count[CM_SECTIONS + 1];
    uint32_t *strSlot;              // string table index: offset + 1 per slot, 0 = empty (open addressing)
    int strSlotCap, strCount;
    bool oom;
} CmWriter;

static void CmPut(CmWriter *W, int id, const void *rec)
{
    CmBuf *B = &W->sec[id];
    uint32_t size = CM_SECTION[id].size;
    if (W->oom || !MS_RESERVE(B->b, B->cap, B->n + (int)size)) { W->oom = true; return; }
    memcpy(B->b + B->n, rec, size);
    B->n += (int)size;
    W->count[id]++;
}

static unsigned long long CmHash(const char *s) { return Fnv1a64Update(FNV_OFFSET, s, strlen(s)); }

// The string's offset in the string table (each string stored once, in first-use order); NULL = CM_NONE
static uint32_t CmStr(CmWriter *W, const char *s)
{
    if (!s) return CM_NONE;
    CmBuf *T = &W->sec[CM_STRINGS];
    if (W->oom) return 0;
    if (2 * (W->strCount + 1) > W->strSlotCap) {
        int cap = W->strSlotCap ? W->strSlotCap * 2 : 1024;
        uint32_t *t = calloc((size_t)cap, sizeof(uint32_t));
        if (!t) { W->oom = true; return 0; }
        for (int i = 0; i < W->strSlotCap; i++) {
            if (!W->strSlot[i]) continue;
            unsigned h = (unsigned)CmHash((const char *)T->b + W->strSlot[i] - 1) & (unsigned)(cap - 1);
            while (t[h]) h = (h + 1) & (unsigned)(cap - 1);
            t[h] = W->strSlot[i];
        }
        free(W->strSlot);
        W->strSlot = t;
        W->strSlotCap = cap;
    }
    unsigned h = (unsigned)CmHash(s) & (unsigned)(W->strSlotCap - 1);
    for (; W->strSlot[h]; h = (h + 1) & (unsigned)(W->strSlotCap - 1))
        if (strcmp((const char *)T->b + W->strSlot[h] - 1, s) == 0) return W->strSlot[h] - 1;
    int len = (int)strlen(s) + 1;
    if (!MS_RESERVE(T->b, T->cap, T->n + len)) { W->oom = true; return 0; }
    uint32_t off = (uint32_t)T->n;
    memcpy(T->b + T->n, s, len);
    T->n += len;
    W->count[CM_STRINGS] += (uint32_t)len;
    W->strSlot[h] = off + 1;
    W->strCount++;
    return off;
}

static CmRef CmRefOf(MS_Ref r) { return (CmRef){ r.line, r.prim, r.group, r.material, r.copy, r.part }; }

// One env record of `type` from the value at p
static void CmPutEnv(CmWriter *W, const char *key, int type, const void *p)
{
    CmEnv c = { .key = CmStr(W, key), .type = (uint32_t)type };
    switch (type) {
        case T_B: c.value[0] = *(const bool *)p ? 1 : 0; break;
        case T_COLOR: { const Color *col = p; c.value[0] = col->r; c.value[1] = col->g; c.value[2] = col->b; c.value[3] = col->a; break; }
        case T_V3: memcpy(c.value, p, 3 * sizeof(float)); break;
        case T_QUAT: memcpy(c.value, p, 4 * sizeof(float)); break;
        case T_BOX: memcpy(c.value, p, 6 * sizeof(float)); break;
        case T_PATH: c.value[0] = CmStr(W, p); break;
        default: memcpy(c.value, p, 4); break;
    }
    CmPut(W, CM_ENV, &c);
}

static void CmPutEnvironment(CmWriter *W, const EnvDef *e)
{
    for (int i = 0; i < COUNT(ENV_KEYS); i++)
        CmPutEnv(W, ENV_KEYS[i].key, ENV_KEYS[i].type, (const char *)&e->env + ENV_KEYS[i].off);
    for (int i = 0; i < COUNT(DEF_KEYS); i++) {
        const char *k = DEF_KEYS[i].key;
        if ((strcmp(k, "shadow.home") == 0 && !e->homeSet) || (strcmp(k, "fogCache.region") == 0 && !e->fogRegionSet) ||
            (DEF_KEYS[i].type == T_PATH && !((const char *)e + DEF_KEYS[i].off)[0]))
            continue;       // optional and not set by the map (the reader keeps the built-in value)
        CmPutEnv(W, k, DEF_KEYS[i].type, (const char *)e + DEF_KEYS[i].off);
    }
    for (int i = 0; i < COUNT(SKY_KEYS); i++) {
        char key[64];
        if (strcmp(SKY_KEYS[i].key, "sunDirection") != 0 || e->skySunDirSet) {
            snprintf(key, sizeof(key), "sky.%s", SKY_KEYS[i].key);
            CmPutEnv(W, key, SKY_KEYS[i].type, (const char *)&e->sky + SKY_KEYS[i].off);
        }
        if (e->ambientSet & (1u << i)) {
            snprintf(key, sizeof(key), "ambientSky.%s", SKY_KEYS[i].key);
            CmPutEnv(W, key, SKY_KEYS[i].type, (const char *)&e->ambient + SKY_KEYS[i].off);
        }
    }
}

static bool IsLittleEndianHost(void)
{
    uint32_t v = CM_BYTE_ORDER;
    unsigned char b[4];
    memcpy(b, &v, 4);
    return b[0] == 0x04;
}

// The compiled file of d (parsed, before FinishParse) in a malloc'd buffer; srcName, srcHash,
// srcSize describe the map text it came from. NULL with a message on failure.
static unsigned char *WriteCompiled(const SceneData *d, const char *srcName, unsigned long long srcHash, long long srcSize,
                                    int *outSize, char *err, int errSize)
{
    if (!IsLittleEndianHost()) {
        snprintf(err, errSize, "%s: compiled maps are little-endian; this host is not", d->path);
        return NULL;
    }
    CmWriter W = { 0 };
    CmPut(&W, CM_STRINGS, "");      // offset 0: the empty string (the table starts with a NUL)
    uint32_t srcNameOff = CmStr(&W, srcName);

    for (int i = 0; i < d->stmtCount; i++) CmPut(&W, CM_STATEMENTS, &d->stmtKeys[i]);
    for (int i = 0; i < d->groupCount; i++) { uint32_t s = CmStr(&W, d->groups[i]); CmPut(&W, CM_GROUPS, &s); }
    for (int i = 0; i < d->materialCount; i++) {
        const MS_Material *M = &d->materials[i];
        const MatDef *D = &d->matDefs[i];
        const R3D_Material *m = &M->material;
        CmMaterial c = {
            .name = CmStr(&W, M->name), .shader = CmStr(&W, M->shader), .line = M->line, .shaderLine = D->shaderLine,
            .albedo = { m->albedo.color.r, m->albedo.color.g, m->albedo.color.b, m->albedo.color.a },
            .emission = { m->emission.color.r, m->emission.color.g, m->emission.color.b, m->emission.color.a },
            .emissionEnergy = m->emission.energy, .normalScale = m->normal.scale, .occlusion = m->orm.occlusion,
            .roughness = m->orm.roughness, .metalness = m->orm.metalness, .specular = m->orm.specular,
            .uvScale = { m->uvScale.x, m->uvScale.y }, .uvOffset = { m->uvOffset.x, m->uvOffset.y }, .alphaCutoff = m->alphaCutoff,
            .cull = m->cullMode, .transparency = m->transparencyMode, .blend = m->blendMode, .priority = m->priority,
            .wrap = D->wrap, .inherit = D->inherit, .unlit = m->unlit, .instanced = D->instanced,
            .uniformFirst = W.count[CM_UNIFORMS], .uniformCount = (uint32_t)D->uniformCount,
            .samplerFirst = W.count[CM_SAMPLERS], .samplerCount = (uint32_t)D->samplerCount,
        };
        for (int k = 0; k < MAP_COUNT; k++) {
            c.maps[k] = CmStr(&W, D->maps[k]);
            c.mapLine[k] = D->mapLine[k];
        }
        CmPut(&W, CM_MATERIALS, &c);
        for (int k = 0; k < D->uniformCount; k++) {
            CmUniform u = { CmStr(&W, D->uniforms[k].name), (uint32_t)D->uniforms[k].count, { 0 } };
            memcpy(u.v, D->uniforms[k].v, sizeof(u.v));
            CmPut(&W, CM_UNIFORMS, &u);
        }
        for (int k = 0; k < D->samplerCount; k++) {
            CmSampler sm = { CmStr(&W, D->samplers[k].name), CmStr(&W, D->samplers[k].path), D->samplers[k].line, D->samplers[k].data };
            CmPut(&W, CM_SAMPLERS, &sm);
        }
    }
    for (int i = 0; i < d->boxCount; i++) {
        const BoundingBox *b = &d->boxes[i];
        CmBox c = { { b->min.x, b->min.y, b->min.z }, { b->max.x, b->max.y, b->max.z }, CmRefOf(d->boxRefs[i]) };
        CmPut(&W, CM_BOXES, &c);
    }
    for (int i = 0; i < d->modelFileCount; i++) {
        CmModelFile c = { CmStr(&W, d->modelFiles[i].path), d->modelFiles[i].line };
        CmPut(&W, CM_MODEL_FILES, &c);
    }
    for (int i = 0; i < d->localColliderCount; i++) {
        const BoundingBox *b = &d->localColliders[i];
        CmCollider c = { { b->min.x, b->min.y, b->min.z }, { b->max.x, b->max.y, b->max.z } };
        CmPut(&W, CM_COLLIDERS, &c);
    }
    for (int i = 0; i < d->modelSlotCount; i++) {
        CmSlot c = { d->modelSlots[i].slot, d->modelSlots[i].material };
        CmPut(&W, CM_SLOTS, &c);
    }
    for (int i = 0; i < d->modelCount; i++) {
        const MS_ModelInstance *M = &d->models[i];
        CmModel c = {
            .file = M->file, .position = { M->position.x, M->position.y, M->position.z },
            .rotationDeg = { M->rotationDeg.x, M->rotationDeg.y, M->rotationDeg.z }, .scale = { M->scale.x, M->scale.y, M->scale.z },
            .origin = M->origin, .collide = M->collide, .castShadow = M->castShadow,
            .colliderFirst = M->colliderFirst, .colliderCount = M->colliderCount, .slotFirst = M->slotFirst, .slotCount = M->slotCount,
            .ref = CmRefOf(M->ref),
        };
        CmPut(&W, CM_MODELS, &c);
    }
    for (int i = 0; i < d->lightCount; i++) {
        const MS_LightDef *L = &d->lights[i];
        const R3D_Light *l = &L->light;
        CmLight c = {
            .type = l->type, .position = { l->position.x, l->position.y, l->position.z },
            .direction = { l->direction.x, l->direction.y, l->direction.z }, .color = { l->color.r, l->color.g, l->color.b, l->color.a },
            .energy = l->energy, .specular = l->specular, .range = l->range, .falloff = l->falloff, .inner = l->innerCutOff,
            .outer = l->outerCutOff, .fogEnergy = l->fogEnergy, .shadow = L->shadow, .clip = L->clip,
            .ref = CmRefOf(L->ref),
        };
        if (L->clip) {
            memcpy(c.clipMin, &L->clipBox.min, sizeof(c.clipMin));
            memcpy(c.clipMax, &L->clipBox.max, sizeof(c.clipMax));
        }
        CmPut(&W, CM_LIGHTS, &c);
    }
    for (int i = 0; i < d->viewpointCount; i++) {
        const MS_Viewpoint *v = &d->viewpoints[i];
        CmViewpoint c = { CmStr(&W, v->name), { v->eye.x, v->eye.y, v->eye.z }, { v->target.x, v->target.y, v->target.z } };
        CmPut(&W, CM_VIEWPOINTS, &c);
    }
    CmSpawn sp = { d->hasSpawn, { d->spawnFeet.x, d->spawnFeet.y, d->spawnFeet.z }, d->spawnYawDeg, d->spawnPitchDeg };
    CmPut(&W, CM_SPAWN, &sp);
    CmPutEnvironment(&W, &d->env);
    size_t rootLen = strlen(d->root);
    for (int i = 1; i < d->depCount; i++) {         // [0] is the map file itself
        if (strncmp(d->deps[i], d->root, rootLen) != 0) continue;
        uint32_t s = CmStr(&W, d->deps[i] + rootLen);
        CmPut(&W, CM_DEPS, &s);
    }

    // Header, section table, sections (each at a multiple of 8, zero padding)
    uint64_t total = sizeof(CmHeader) + CM_SECTIONS * sizeof(CmSection);
    uint32_t offset[CM_SECTIONS + 1];
    for (int id = 1; id <= CM_SECTIONS; id++) {
        total = (total + 7) & ~(uint64_t)7;
        offset[id] = (uint32_t)total;
        total += (uint64_t)W.sec[id].n;
    }
    unsigned char *out = NULL;
    if (W.oom) snprintf(err, errSize, "%s: out of memory", d->path);
    else if (total > MS_MAX_COMPILED_BYTES) snprintf(err, errSize, "%s: the compiled map would be larger than 256 MB", d->path);
    else if (!(out = calloc(1, (size_t)total))) snprintf(err, errSize, "%s: out of memory", d->path);
    if (out) {
        CmHeader h = { .version = CM_VERSION, .headerSize = sizeof(CmHeader), .byteOrder = CM_BYTE_ORDER,
                       .formatVersion = (uint32_t)d->formatVersion, .sectionCount = CM_SECTIONS, .sectionTable = sizeof(CmHeader),
                       .fileSize = total, .sourceHash = srcHash, .sourceSize = (uint64_t)srcSize, .sourceName = srcNameOff };
        memcpy(h.magic, CM_MAGIC, sizeof(CM_MAGIC));
        memcpy(out, &h, sizeof(h));
        for (int id = 1; id <= CM_SECTIONS; id++) {
            CmSection s = { (uint32_t)id, offset[id], W.count[id], CM_SECTION[id].size };
            memcpy(out + sizeof(CmHeader) + (id - 1) * sizeof(CmSection), &s, sizeof(s));
            if (W.sec[id].n) memcpy(out + offset[id], W.sec[id].b, (size_t)W.sec[id].n);
        }
        *outSize = (int)total;
    }
    for (int id = 1; id <= CM_SECTIONS; id++) free(W.sec[id].b);
    free(W.strSlot);
    return out;
}
#endif

// #endregion

// ======================================================================================
// #region Caches follow the map (.claude/skills/lighting-fog-shadows/r3d-pitfalls.md 3)
// ======================================================================================
#ifdef MS_PATCHED_R3D   // only the static tiles and the fog bake use these
// "Ground-like" boxes (a slab much wider than it is tall, >= 100 m across) are left out of
// the bounds the caches are placed by: a big ground plane would put the play area's centre
// and the fog cache region in the middle of nowhere.

static bool GroundLike(BoundingBox b)
{
    float w = fminf(b.max.x - b.min.x, b.max.z - b.min.z), h = b.max.y - b.min.y;
    return w >= 100.0f && h <= 0.05f * w;
}

static BoundingBox ContentBounds(const SceneData *d)
{
    BoundingBox r = { { INFINITY, INFINITY, INFINITY }, { -INFINITY, -INFINITY, -INFINITY } };
    for (int i = 0; i < d->boxCount; i++) {
        if (GroundLike(d->boxes[i])) continue;
        r.min = Vector3Min(r.min, d->boxes[i].min);
        r.max = Vector3Max(r.max, d->boxes[i].max);
    }
    for (int i = 0; i < d->modelCount; i++) {
        r.min = Vector3Min(r.min, d->models[i].bounds.min);
        r.max = Vector3Max(r.max, d->models[i].bounds.max);
    }
    if (r.min.x > r.max.x) r = d->bounds;   // only ground: use everything
    return r;
}

// Where the player and the cameras are: the shadow tiles are laid out around it
static Vector3 PlayCenter(const SceneData *d)
{
    if (d->viewpointCount > 0) {
        Vector3 lo = d->viewpoints[0].eye, hi = lo;
        for (int i = 1; i < d->viewpointCount; i++) {
            lo = Vector3Min(lo, d->viewpoints[i].eye);
            hi = Vector3Max(hi, d->viewpoints[i].eye);
        }
        return Vector3Scale(Vector3Add(lo, hi), 0.5f);
    }
    BoundingBox b = ContentBounds(d);
    return Vector3Scale(Vector3Add(b.min, b.max), 0.5f);
}

static Vector3 ShadowHome(const SceneData *d)
{
    return d->env.homeSet ? d->env.home : PlayCenter(d);
}

#define MS_GL_MAX_3D_TEXTURE_SIZE 0x8073
extern void glGetIntegerv(unsigned int pname, int *data);     // (declared by hand, see glEnable below)

// GL_MAX_3D_TEXTURE_SIZE of the current context (0 = unknown)
static int Max3DTextureSize(void)
{
    int v = 0;
    glGetIntegerv(MS_GL_MAX_3D_TEXTURE_SIZE, &v);
    return v;
}

// Shrinks r on every axis that would need more than the GPU's 3D texture size in voxels
// (a long thin map: an object 20 km out made the occlusion volume 23153 voxels long, the
// upload failed with GL_INVALID_VALUE and AO silently went off). The kept part is centred
// on the play centre and stays inside r. Logs what it cut.
static BoundingBox ClampRegionTo3DLimit(const SceneData *d, BoundingBox r, float voxel, const char *what)
{
    int max3D = Max3DTextureSize();
    if (max3D <= 2) return r;
    float maxSize = (float)(max3D - 1) * voxel;    // ceil(size / voxel) <= max3D with room for rounding
    Vector3 c = Vector3Clamp(PlayCenter(d), r.min, r.max);
    for (int a = 0; a < 3; a++) {
        float *l = &((float *)&r.min)[a], *h = &((float *)&r.max)[a];
        if (*h - *l <= maxSize) continue;
        float rmin = *l, rmax = *h, cc = ((const float *)&c)[a];
        float lo = cc - maxSize * 0.5f, hi = cc + maxSize * 0.5f;
        if (lo < rmin) { hi += rmin - lo; lo = rmin; }
        if (hi > rmax) { lo -= hi - rmax; hi = rmax; }
        TraceLog(LOG_WARNING, "monolith_scene: %s: %c extent %.0f m is %d voxels of %.2f m, over the GPU's 3D texture limit %d: "
                 "only %.0f..%.0f is covered", what, "xyz"[a], rmax - rmin, (int)ceilf((rmax - rmin) / voxel), voxel, max3D, lo, hi);
        *l = lo;
        *h = hi;
    }
    return r;
}

// The content bounds (+2 m), cut to the GPU's 3D texture size per axis, then shrunk around
// the play centre until they fit `budget` voxels
static BoundingBox BudgetRegion(const SceneData *d, float voxel, double budget, const char *what)
{
    BoundingBox r = ContentBounds(d);
    r.min = Vector3SubtractValue(r.min, 2.0f);
    r.max = Vector3AddValue(r.max, 2.0f);
    r = ClampRegionTo3DLimit(d, r, voxel, what);
    Vector3 size = Vector3Subtract(r.max, r.min);
    double voxels = (double)(size.x / voxel) * (size.y / voxel) * (size.z / voxel);
    if (voxels > budget) {
        float s = (float)cbrt(budget / voxels);
        Vector3 c = Vector3Clamp(PlayCenter(d), r.min, r.max), half = Vector3Scale(size, 0.5f * s);
        Vector3 lo = Vector3Subtract(c, half), hi = Vector3Add(c, half);
        // keep the shrunk box inside the content bounds where it fits
        for (int a = 0; a < 3; a++) {
            float *l = &((float *)&lo)[a], *h = &((float *)&hi)[a];
            float rmin = ((const float *)&r.min)[a], rmax = ((const float *)&r.max)[a];
            if (*l < rmin) { *h += rmin - *l; *l = rmin; }
            if (*h > rmax) { *l -= *h - rmax; *h = rmax; }
        }
        r = (BoundingBox){ lo, hi };
    }
    return r;
}

// The fog cache region: the hint, else the content bounds (+2 m), shrunk around the play
// centre until it fits FOG_CACHE_BUDGET voxels
static BoundingBox FogCacheRegion(const SceneData *d, float voxel)
{
    return d->env.fogRegionSet ? ClampRegionTo3DLimit(d, d->env.fogRegion, voxel, "fog cache")
                               : BudgetRegion(d, voxel, FOG_CACHE_BUDGET, "fog cache");
}

// Occlusion volume (aoVolumeVoxel, R3D patch 11): the signed distance from each voxel centre
// to the nearest box surface (negative inside), clamped to [AO_DIST_MIN, AO_DIST_MAX] and
// stored in a byte. Built on the CPU: every box only visits the voxels within AO_DIST_MAX of
// it (its grown bounds), so the cost follows the boxes' volume, not boxes x voxels (default
// map, 0.5 m: ~0.1 s). Models are not in it (their bounds are no stand-in for their shape).
static bool BakeOcclusionVolume(const SceneData *d, float voxel, float intensity)
{
    BoundingBox region = BudgetRegion(d, voxel, AO_VOLUME_BUDGET, "occlusion volume");
    int dim[3], max3D = Max3DTextureSize();
    for (int a = 0; a < 3; a++) {
        float size = ((const float *)&region.max)[a] - ((const float *)&region.min)[a];
        dim[a] = (int)ceilf(size / voxel);
        if (dim[a] < 2) dim[a] = 2;
        if (max3D > 2 && dim[a] > max3D) dim[a] = max3D;    // (float rounding; the region is cut already)
    }
    size_t count = (size_t)dim[0] * dim[1] * dim[2];
    unsigned char *grid = malloc(count);
    float *dist = malloc(count * sizeof(float));
    float *axis = malloc(sizeof(float) * (dim[0] + dim[1] + dim[2]));
    if (!grid || !dist || !axis) {
        free(grid); free(dist); free(axis);
        TraceLog(LOG_WARNING, "monolith_scene: occlusion volume: out of memory (%d x %d x %d)", dim[0], dim[1], dim[2]);
        return false;
    }
    double t0 = GetTime();
    // dist > 0: distance to the nearest box outside all boxes; <= 0: minus the depth inside
    for (size_t i = 0; i < count; i++) dist[i] = AO_DIST_MAX;
    const float *rmin = (const float *)&region.min;
    for (int b = 0; b < d->boxCount; b++) {
        const float *bmin = (const float *)&d->boxes[b].min, *bmax = (const float *)&d->boxes[b].max;
        int lo[3], hi[3];
        bool empty = false;
        float *ax[3] = { axis, axis + dim[0], axis + dim[0] + dim[1] };
        for (int a = 0; a < 3 && !empty; a++) {
            // voxel centres c = rmin + (i + 0.5) * voxel within AO_DIST_MAX of the box
            lo[a] = CellKey((bmin[a] - AO_DIST_MAX - rmin[a]) / voxel - 0.5f);
            hi[a] = -CellKey(-((bmax[a] + AO_DIST_MAX - rmin[a]) / voxel - 0.5f));  // = ceil
            if (lo[a] < 0) lo[a] = 0;
            if (hi[a] > dim[a] - 1) hi[a] = dim[a] - 1;
            empty = lo[a] > hi[a];
            // per axis: outside distance (> 0), or minus the distance to the nearer face (<= 0)
            for (int i = lo[a]; i <= hi[a]; i++) {
                float c = rmin[a] + ((float)i + 0.5f) * voxel;
                ax[a][i] = c < bmin[a] ? bmin[a] - c : c > bmax[a] ? c - bmax[a] : -fminf(c - bmin[a], bmax[a] - c);
            }
        }
        if (empty) continue;
        for (int z = lo[2]; z <= hi[2]; z++) {
            float dz = ax[2][z];
            for (int y = lo[1]; y <= hi[1]; y++) {
                float dy = ax[1][y];
                float oz = fmaxf(dz, 0.0f), oy = fmaxf(dy, 0.0f), syz = oy * oy + oz * oz;
                if (syz >= AO_DIST_MAX * AO_DIST_MAX) continue;
                float iyz = fmaxf(dy, dz);     // <= 0 only when inside on both axes
                float *row = dist + ((size_t)z * dim[1] + y) * dim[0];
                for (int x = lo[0]; x <= hi[0]; x++) {
                    float dx = ax[0][x], e;
                    if (dx <= 0.0f && iyz <= 0.0f) e = fmaxf(dx, iyz);     // inside: minus the depth
                    else {
                        float ox = fmaxf(dx, 0.0f);
                        e = sqrtf(ox * ox + syz);
                    }
                    // union of boxes: the minimum (outside: the nearest box, inside: the deepest)
                    if (e < row[x]) row[x] = e;
                }
            }
        }
    }
    for (size_t i = 0; i < count; i++) {
        float v = (Clamp(dist[i], AO_DIST_MIN, AO_DIST_MAX) - AO_DIST_MIN) / (AO_DIST_MAX - AO_DIST_MIN);
        grid[i] = (unsigned char)(v * 255.0f + 0.5f);
    }
    R3D_SetWorldOcclusion(grid, dim[0], dim[1], dim[2], region.min, voxel, AO_DIST_MIN, AO_DIST_MAX, intensity);
    TraceLog(LOG_INFO, "monolith_scene: occlusion volume %d x %d x %d (%.1f MB, %.2f m voxels) baked in %.0f ms",
             dim[0], dim[1], dim[2], count / (1024.0 * 1024.0), voxel, (GetTime() - t0) * 1000.0);
    free(grid); free(dist); free(axis);
    return true;
}
#endif

// #endregion

// ======================================================================================
// #region Sun shadow: static world-fixed tiles, camera-fitted fallback (the former shadow.c)
// ======================================================================================

// R3D's light-space basis for a directional light (light_dir_view_proj in
// r3d/src/modules/r3d_light.c): x = lightRight, y = lightUp, z = dir
static void LightBasis(Vector3 dir, Vector3 *right, Vector3 *up)
{
    float ax = fabsf(dir.x), ay = fabsf(dir.y), az = fabsf(dir.z);
    Vector3 ref = (ax <= ay && ax <= az) ? (Vector3){ 1, 0, 0 } : (ay <= az) ? (Vector3){ 0, 1, 0 } : (Vector3){ 0, 0, 1 };
    *right = Vector3Normalize(Vector3CrossProduct(ref, dir));
    *up = Vector3CrossProduct(dir, *right);
}

// R3D's fit radius: bounding sphere of a camera frustum out to `range`
static float FitRadius(float range, float fovy, float aspect)
{
    float camNear = range / 1000.0f, camFar = range;
    float farH = camFar * tanf(fovy * DEG2RAD * 0.5f);
    float halfDepth = (camFar - camNear) * 0.5f;
    return sqrtf(farH * farH * (1.0f + aspect * aspect) + halfDepth * halfDepth);
}

// Sun and boxes are static, so the shadow depth is a pure function of the map. Instead
// of R3D's camera-fitted map (which moves with every step and turn and had to be
// re-rendered for ~21% of the frames while turning, 1.2 ms + 0.37 ms min/max each),
// the sun's shadow comes from world-fixed tiles: orthographic maps on a fixed grid in
// light space, rendered once and then only sampled (R3D_PushLightFixedShadow, R3D
// patch 7). Which tile is used depends only on the eye position, so turning never
// re-renders anything, and the two tiles cached in R3D's two directional layers cover
// the play area around the tile home (for the default map: every viewpoint and
// verification shot), so there a tile is drawn once per map load. Elsewhere a new tile is
// drawn when the eye enters a new cell, i.e. every ~90 m of travel across the sun direction.
//
// Each tile has exactly the texel size and depth range of R3D's fit for a 70 deg,
// 16:9 view (8192 px / 100 m: 3.69 cm texels, 403 m deep), and its centre is snapped
// to the same world-space texel lattice. So at 16:9 the texel grid, depth precision,
// biases and PCF footprint are the ones the camera-fitted map had (wider screens used
// a larger radius there: 1920x1006 had 3.88 cm texels, so tiles are sharper).
//
// Coverage: shadows are weighted only within `range` of the eye (R3D fades them by
// distance from the eye, patch 4; the fog's march samples lie within that too), and
// the map's own edge fade is the outer SHADOW_EDGE_FADE. A tile is used only while
// every point within `range` of the eye stays inside that, so the fades are exactly 1
// wherever shadows show: same result as the camera-fitted map, minus 16-bit depth
// rounding. Casters toward the sun beyond the near plane are flattened onto it by
// GL_DEPTH_CLAMP (pancaking, MS_InitRenderer after R3D_Init), so tall far-away
// casters still shadow.

#define SHADOW_EDGE_FADE    0.01f   // SHADOW_DIR_EDGE_FADE in r3d/shaders/include/wrap/light.glsl
#define TILE_FIT_ASPECT     (16.0f / 9.0f)
#define TILE_FIT_FOVY       70.0f   // the cameras' fovy
#define TILE_Z_EXTENSION    100.0f  // R3D's zExtension: depth window = 2 * radius + this
#define TILE_HYSTERESIS     3.0f    // m: cells are this much smaller than what a tile covers

#ifdef MS_PATCHED_R3D   // the static tiles and the fog bake need the patched R3D APIs
// Half-extents (light-space x/y and z) of the eye positions a tile can serve
static void TileReach(float range, float *hxy, float *hz, float *radius, float *halfDepth)
{
    *radius = FitRadius(range, TILE_FIT_FOVY, TILE_FIT_ASPECT);
    *halfDepth = (2.0f * *radius + TILE_Z_EXTENSION) * 0.5f;
    *hxy = *radius * (1.0f - 2.0f * SHADOW_EDGE_FADE) - range;
    *hz = *halfDepth * (1.0f - 2.0f * SHADOW_EDGE_FADE) - range;
}

// The grid is laid out so `home` sits on a cell boundary in light-space y and in the
// middle of a cell in x and z (for the default map, home (10, 38, 2) puts the eye positions
// of all 7 viewpoints and all verification shots into two cells, i.e. the two cached tiles).
// Picks the tile that covers everything within `range` of `eye` and returns its index
// (= which of the SUN_TILE_COUNT shadow maps to push). *render is set when that tile must be
// (re)drawn this frame, with t->tile[index].viewProj. Depends only on the eye position,
// never on the view direction; tiles are dropped when `version` (or the layout) changes.
static int SunShadowTileSelect(SunShadowTiles *t, Vector3 sunDir, float range, int mapSize, Vector3 home,
                               Vector3 eye, unsigned int version, bool *render)
{
    Vector3 dir = Vector3Normalize(sunDir), right, up;
    LightBasis(dir, &right, &up);
    float hxy, hz, radius, halfDepth;
    TileReach(range, &hxy, &hz, &radius, &halfDepth);
    float cellXY = 2.0f * (hxy - TILE_HYSTERESIS), cellZ = 2.0f * (hz - TILE_HYSTERESIS);

    // New map or settings: every tile is stale (keyed on the scene version)
    if (t->version != version || t->range != range || t->mapSize != mapSize || !Vector3Equals(t->home, home)) {
        for (int i = 0; i < SUN_TILE_COUNT; i++) t->tile[i].valid = false;
        t->version = version;
        t->range = range;
        t->mapSize = mapSize;
        t->home = home;
        t->current = -1;
    }

    Vector3 ls = { Vector3DotProduct(eye, right), Vector3DotProduct(eye, up), Vector3DotProduct(eye, dir) };
    Vector3 origin = { Vector3DotProduct(home, right) - 0.5f * cellXY, Vector3DotProduct(home, up) - cellXY,
                       Vector3DotProduct(home, dir) - 0.5f * cellZ };
    float texel = 2.0f * radius / (float)mapSize;

    // Keep a cached tile while the eye is within its reach (the current one first, so
    // the eye doesn't flip between overlapping tiles)
    int pick = -1;
    for (int k = -1; k < SUN_TILE_COUNT && pick < 0; k++) {
        int i = (k < 0) ? t->current : k;
        if (i < 0 || !t->tile[i].valid) continue;
        const int *key = t->tile[i].key;
        float cx = origin.x + (key[0] + 0.5f) * cellXY, cy = origin.y + (key[1] + 0.5f) * cellXY;
        float cz = origin.z + (key[2] + 0.5f) * cellZ;
        if (fabsf(ls.x - cx) <= hxy && fabsf(ls.y - cy) <= hxy && fabsf(ls.z - cz) <= hz) pick = i;
    }

    *render = false;
    if (pick < 0) {
        // The eye's cell, into the least recently used slot
        int key[3] = { CellKey((ls.x - origin.x) / cellXY), CellKey((ls.y - origin.y) / cellXY),
                       CellKey((ls.z - origin.z) / cellZ) };
        pick = 0;
        for (int i = 1; i < SUN_TILE_COUNT; i++) {
            if (!t->tile[i].valid && t->tile[pick].valid) pick = i;
            else if (t->tile[i].valid == t->tile[pick].valid && t->tile[i].lastUsed < t->tile[pick].lastUsed) pick = i;
        }

        // R3D's matrix layout: ortho box around the centre, snapped to whole texels in
        // x/y (the same lattice R3D's fit snaps to), depth window centred on the cell
        float cx = floorf((origin.x + (key[0] + 0.5f) * cellXY) / texel) * texel;
        float cy = floorf((origin.y + (key[1] + 0.5f) * cellXY) / texel) * texel;
        float cz = origin.z + (key[2] + 0.5f) * cellZ;
        Vector3 center = Vector3Add(Vector3Add(Vector3Scale(right, cx), Vector3Scale(up, cy)), Vector3Scale(dir, cz));
        Vector3 lightEye = Vector3Subtract(center, Vector3Scale(dir, halfDepth));
        Matrix view = MatrixLookAt(lightEye, center, up);
        Matrix proj = MatrixOrtho(-radius, radius, -radius, radius, 0.0f, 2.0f * halfDepth);

        SunShadowTile *tile = &t->tile[pick];
        tile->valid = true;
        memcpy(tile->key, key, sizeof(key));
        tile->viewProj = MatrixMultiply(view, proj);
        t->renders++;
        *render = true;
    }

    t->tile[pick].lastUsed = ++t->useCount;
    t->current = pick;
    return pick;
}

// Orthographic light-space matrix whose box covers `region` (plus `margin` m on every
// side; casters toward the sun are pancaked onto its near plane), for the fog bake
static Matrix SunShadowBoxMatrix(Vector3 sunDir, BoundingBox region, float margin)
{
    Vector3 dir = Vector3Normalize(sunDir), right, up;
    LightBasis(dir, &right, &up);
    Vector3 lo = { 1e30f, 1e30f, 1e30f }, hi = { -1e30f, -1e30f, -1e30f };
    for (int i = 0; i < 8; i++) {
        Vector3 p = { (i & 1) ? region.max.x : region.min.x, (i & 2) ? region.max.y : region.min.y,
                      (i & 4) ? region.max.z : region.min.z };
        Vector3 l = { Vector3DotProduct(p, right), Vector3DotProduct(p, up), Vector3DotProduct(p, dir) };
        lo = Vector3Min(lo, l);
        hi = Vector3Max(hi, l);
    }
    Vector3 half = Vector3Add(Vector3Scale(Vector3Subtract(hi, lo), 0.5f), (Vector3){ margin, margin, margin });
    Vector3 mid = Vector3Scale(Vector3Add(hi, lo), 0.5f);
    Vector3 center = Vector3Add(Vector3Add(Vector3Scale(right, mid.x), Vector3Scale(up, mid.y)), Vector3Scale(dir, mid.z));
    Matrix view = MatrixLookAt(Vector3Subtract(center, Vector3Scale(dir, half.z)), center, up);
    Matrix proj = MatrixOrtho(-half.x, half.x, -half.y, half.y, 0.0f, 2.0f * half.z);
    return MatrixMultiply(view, proj);
}
#endif

// Camera-fitted map, re-rendered only when the cached one no longer covers the view
// (MS_SHADOW_CAMERA; the path before the static tiles, and the only one with upstream R3D).
// R3D keeps the light-space matrix next to the cached map (R3D_PushLightEx(...,
// updateShadow = false)), so a reused map is always sampled consistently; the only risk
// is coverage. R3D's fit (light_dir_view_proj) uses a radius that depends only on range,
// fov and aspect and snaps the centre to whole texels, so every fit lays the same
// world-space texel grid. Shadow results can only differ through R3D's fades: edgeFade
// (outer SHADOW_EDGE_FADE of the map; 5% upstream, before R3D patch 7) times distFade
// (distance from the eye 75%..100% of range; R3D patch 4, view depth upstream). So the
// map is reused while, at every point of the view frustum within range, the cached map's
// fade is within SHADOW_REUSE_EPS of what a freshly fitted map would give. With upstream
// R3D the checks are slightly off (edge fade and depth-based distFade), which only costs
// image accuracy at the map edge, never correctness of the cached matrix.

#define SHADOW_REUSE_EPS    (1.0f / 255.0f)   // below 8-bit visibility

// Replicates R3D's light_dir_view_proj(): ortho box around the bounding sphere
// of the camera frustum out to `range`, centre snapped to texels.
static Matrix SunShadowFit(Vector3 dir, float range, Camera3D cam, float aspect, int mapSize)
{
    float camNear = range / 1000.0f, camFar = range;
    float radius = FitRadius(range, cam.fovy, aspect);

    Vector3 forward = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 center = Vector3Add(cam.position, Vector3Scale(forward, (camNear + camFar) * 0.5f));

    Vector3 lightRight, lightUp;
    LightBasis(dir, &lightRight, &lightUp);

    float texel = radius * 2.0f / (float)mapSize;
    float cx = floorf(Vector3DotProduct(center, lightRight) / texel) * texel;
    float cy = floorf(Vector3DotProduct(center, lightUp) / texel) * texel;
    float cz = Vector3DotProduct(center, dir);
    Vector3 snapped = Vector3Add(Vector3Add(Vector3Scale(lightRight, cx), Vector3Scale(lightUp, cy)), Vector3Scale(dir, cz));

    const float zExtension = 100.0f;
    Vector3 eye = Vector3Subtract(snapped, Vector3Scale(dir, radius + zExtension));
    Matrix view = MatrixLookAt(eye, snapped, lightUp);
    Matrix proj = MatrixOrtho(-radius, radius, -radius, radius, 0.0f, zExtension + radius * 2.0f);
    return MatrixMultiply(view, proj);
}

// GLSL smoothstep, also valid for edge0 > edge1 as R3D uses it
static float SmoothStep(float e0, float e1, float x)
{
    float t = Clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// R3D's edgeFade (L_SampleShadowDir in r3d/shaders/include/wrap/light.glsl)
static float ShadowEdgeFade(Matrix viewProj, Vector3 p)
{
    Vector3 c = Vector3Transform(p, viewProj);   // ortho: w = 1
    Vector3 uv = { c.x * 0.5f + 0.5f, c.y * 0.5f + 0.5f, c.z * 0.5f + 0.5f };
    float d = fminf(fminf(fminf(uv.x, 1.0f - uv.x), fminf(uv.y, 1.0f - uv.y)), fminf(uv.z, 1.0f - uv.z));
    return SmoothStep(0.0f, SHADOW_EDGE_FADE, d);
}

// Decides whether the camera-fitted sun shadow map must be re-rendered this frame
static bool SunShadowNeedsUpdate(SunShadowCache *c, Vector3 sunDir, float range, Camera3D cam, float aspect,
                                 int mapSize, unsigned int version)
{
    Matrix fresh = SunShadowFit(sunDir, range, cam, aspect, mapSize);
    bool update = !c->valid || c->aspect != aspect || c->range != range || c->version != version;

    // Sample the view frustum out to range: depth slices x a grid across each slice
    // (every point within `range` of the eye has a view depth below `range`)
    Vector3 f = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 r = Vector3Normalize(Vector3CrossProduct(f, cam.up));
    Vector3 u = Vector3CrossProduct(r, f);
    float tanH = tanf(cam.fovy * DEG2RAD * 0.5f);
    enum { SLICES = 48, GX = 33, GY = 17 };
    for (int k = 0; k <= SLICES && !update; k++) {
        float camNear = (float)rlGetCullDistanceNear();   // the near plane R3D_CameraFromRL uses
        float z = camNear + (range - camNear) * k / SLICES;
        if (z >= range) continue;
        Vector3 sliceCenter = Vector3Add(cam.position, Vector3Scale(f, z));
        for (int j = 0; j < GY && !update; j++)
            for (int i = 0; i < GX; i++) {
                float x = (2.0f * i / (GX - 1) - 1.0f) * z * tanH * aspect;
                float y = (2.0f * j / (GY - 1) - 1.0f) * z * tanH;
                Vector3 p = Vector3Add(sliceCenter, Vector3Add(Vector3Scale(r, x), Vector3Scale(u, y)));
                // R3D's distFade (by distance from the eye, see above)
                float distFade = SmoothStep(range, range * 0.75f, Vector3Distance(p, cam.position));
                if (distFade <= 0.0f) continue;
                float loss = ShadowEdgeFade(fresh, p) - ShadowEdgeFade(c->viewProj, p);
                if (distFade * loss > SHADOW_REUSE_EPS) { update = true; break; }
            }
    }

    if (update) *c = (SunShadowCache){ true, fresh, aspect, range, version };
    return update;
}

// #endregion

// ======================================================================================
// #region Parse + upload (GPU) + swap
// ======================================================================================

static bool FinishParse(SceneData *d, const MS_LoadOptions *opt)
{
    SunDef *sun = &d->env.sun;
    sun->fileShadowSize = sun->shadowSize;
    if (opt->shadowSizeOverride > 0) sun->shadowSize = opt->shadowSizeOverride;
    if (opt->shadowRangeOverride > 0.0f) sun->range = opt->shadowRangeOverride;
    d->sunLight = R3D_CreateDirLight(sun->direction, sun->color, sun->energy);
    d->sunLight.specular = sun->specular;
    // Shadow distance. Shadows fade out between 75% and 100% of it (R3D); it must cover
    // volumetricFog.length or the light shafts fade early.
    d->sunLight.range = sun->range;
    d->sunLight.fogEnergy = sun->fogEnergy;

    if (!d->hasSpawn) {
        d->spawnFeet = (Vector3){ 0 };
        d->spawnYawDeg = d->spawnPitchDeg = 0.0f;
    }
    if (d->viewpointCount == 0) {
        double yaw = d->spawnYawDeg * (3.14159265358979323846 / 180.0), pitch = d->spawnPitchDeg * (3.14159265358979323846 / 180.0);
        char *name = MS_StrDup("Spawn");
        MS_Viewpoint *v = name ? MS_PUSH(d->viewpoints, d->viewpointCount, d->viewpointCap) : NULL;
        if (!v) { free(name); return false; }
        v->name = name;
        v->eye = (Vector3){ d->spawnFeet.x, d->spawnFeet.y + MS_DEFAULT_EYE_HEIGHT, d->spawnFeet.z };
        v->target = Vector3Add(v->eye, (Vector3){ (float)(cos(pitch) * sin(yaw)), (float)sin(pitch), (float)(cos(pitch) * cos(yaw)) });
    }
    return true;
}

// Scene bounds (boxes, models, point/spot lights) and the collider list; after ResolveModels
static void FinishGeometry(SceneData *d)
{
    BoundingBox b = { { INFINITY, INFINITY, INFINITY }, { -INFINITY, -INFINITY, -INFINITY } };
    for (int i = 0; i < d->boxCount; i++) {
        b.min = Vector3Min(b.min, d->boxes[i].min);
        b.max = Vector3Max(b.max, d->boxes[i].max);
    }
    for (int i = 0; i < d->modelCount; i++) {
        b.min = Vector3Min(b.min, d->models[i].bounds.min);
        b.max = Vector3Max(b.max, d->models[i].bounds.max);
    }
    for (int i = 0; i < d->lightCount; i++)
        if (d->lights[i].light.type != R3D_LIGHT_DIR) {
            b.min = Vector3Min(b.min, d->lights[i].light.position);
            b.max = Vector3Max(b.max, d->lights[i].light.position);
        }
    d->bounds = b;
}

static unsigned int EntityHash(MS_EntityId id, int cap)
{
    unsigned int h = id.stmt * 0x9E3779B1u ^ (unsigned int)id.index * 0x85EBCA77u;
    return (h ^ (h >> 15)) & (unsigned int)(cap - 1);
}

// The entities (after ResolveModels): every box, model and light in map order (the three
// arrays merged by statement), each with its components and its id: the statement's key and
// the object's index within the statement
static bool BuildEntities(SceneData *d)
{
    int n = d->boxCount + d->modelCount + d->lightCount, m = n > 0 ? n : 1;
    int cap = 16;
    while (cap < 2 * n) cap *= 2;
    d->entityIds = malloc(sizeof(MS_EntityId) * m);
    d->entityObjects = malloc(sizeof(MS_ObjRef) * m);
    d->entityTransforms = malloc(sizeof(MS_Transform) * m);
    d->entitySources = malloc(sizeof(MS_Source) * m);
    d->entityModels = malloc(sizeof(int) * m);
    d->objEntities = malloc(sizeof(int) * m);
    d->entityHash = calloc((size_t)cap, sizeof(int));
    d->entityHashCap = cap;
    if (!d->entityIds || !d->entityObjects || !d->entityTransforms || !d->entitySources || !d->entityModels ||
        !d->objEntities || !d->entityHash)
        return false;
    int ib = 0, im = 0, il = 0, lastPrim = -1, index = 0;
    for (int e = 0; e < n; e++) {
        int pb = ib < d->boxCount ? d->boxRefs[ib].prim : INT_MAX;
        int pm = im < d->modelCount ? d->models[im].ref.prim : INT_MAX;
        int pli = il < d->lightCount ? d->lights[il].ref.prim : INT_MAX;
        const MS_Ref *r;
        MS_Transform tr = { .scale = { 1, 1, 1 } };
        int model = -1;
        if (pb <= pm && pb <= pli) {
            BoundingBox b = d->boxes[ib];
            r = &d->boxRefs[ib];
            tr.position = Vector3Scale(Vector3Add(b.min, b.max), 0.5f);
            tr.scale = Vector3Subtract(b.max, b.min);
            d->entityObjects[e] = (MS_ObjRef){ MS_OBJ_BOX, ib };
            d->objEntities[ib++] = e;
        } else if (pm <= pli) {
            const MS_ModelInstance *M = &d->models[im];
            r = &M->ref;
            tr = (MS_Transform){ M->position, M->rotationDeg, M->scale };
            model = M->file;
            d->entityObjects[e] = (MS_ObjRef){ MS_OBJ_MODEL, im };
            d->objEntities[d->boxCount + im++] = e;
        } else {
            r = &d->lights[il].ref;
            tr.position = d->lights[il].light.position;
            d->entityObjects[e] = (MS_ObjRef){ MS_OBJ_LIGHT, il };
            d->objEntities[d->boxCount + d->modelCount + il++] = e;
        }
        index = r->prim == lastPrim ? index + 1 : 0;
        lastPrim = r->prim;
        MS_EntityId id = { r->prim >= 0 && r->prim < d->stmtCount ? d->stmtKeys[r->prim] : 0, index };
        d->entityIds[e] = id;
        d->entityTransforms[e] = tr;
        d->entitySources[e] = (MS_Source){ r->line, r->prim, r->copy, r->part };
        d->entityModels[e] = model;
        unsigned int h = EntityHash(id, cap);
        while (d->entityHash[h]) h = (h + 1) & (unsigned int)(cap - 1);
        d->entityHash[h] = e + 1;
    }
    d->entityCount = n;
    return true;
}

// The colliders MS_Scene publishes: boxes, then model colliders (without models: the box array itself)
static const BoundingBox *PublishedColliders(const SceneData *d, int *count)
{
    *count = d->modelCount > 0 ? d->colliderCount : d->boxCount;
    return d->modelCount > 0 ? d->colliders : d->boxes;
}

// Node `ni` over colliderOrder[first, first + count): leaves of <= 4 colliders, split at the
// middle of the longest axis of the centres' bounds (in place, like BvhBuild)
static void ColliderBvhBuild(SceneData *d, const BoundingBox *c, int ni, int first, int count, int depth)
{
    BvhNode *nd = &d->colliderNodes[ni];
    nd->mn = (Vector3){ INFINITY, INFINITY, INFINITY };
    nd->mx = (Vector3){ -INFINITY, -INFINITY, -INFINITY };
    Vector3 cmn = nd->mn, cmx = nd->mx;
    for (int k = first; k < first + count; k++) {
        const BoundingBox *b = &c[d->colliderOrder[k]];
        nd->mn = Vector3Min(nd->mn, b->min);
        nd->mx = Vector3Max(nd->mx, b->max);
        Vector3 m = Vector3Scale(Vector3Add(b->min, b->max), 0.5f);
        cmn = Vector3Min(cmn, m);
        cmx = Vector3Max(cmx, m);
    }
    nd->left = -1;
    nd->first = first;
    nd->count = count;
    if (count <= 4 || depth >= 60) return;
    Vector3 ext = Vector3Subtract(cmx, cmn);
    int axis = (ext.x >= ext.y && ext.x >= ext.z) ? 0 : (ext.y >= ext.z) ? 1 : 2;
    float split = (((float *)&cmn)[axis] + ((float *)&cmx)[axis]) * 0.5f;
    int i = first, j = first + count - 1;
    while (i <= j) {
        const BoundingBox *b = &c[d->colliderOrder[i]];
        Vector3 m = Vector3Scale(Vector3Add(b->min, b->max), 0.5f);
        if (((float *)&m)[axis] < split) i++;
        else {
            int s = d->colliderOrder[i];
            d->colliderOrder[i] = d->colliderOrder[j];
            d->colliderOrder[j--] = s;
        }
    }
    int leftCount = i - first;
    if (leftCount == 0 || leftCount == count) leftCount = count / 2;    // all centres equal
    int l = d->colliderNodeCount;
    d->colliderNodeCount += 2;
    nd->left = l;
    nd->count = 0;
    ColliderBvhBuild(d, c, l, first, leftCount, depth + 1);
    ColliderBvhBuild(d, c, l + 1, first + leftCount, count - leftCount, depth + 1);
}

// The queries' BVH over the colliders (noGpu loads too: tools pick and collide as well)
static bool BuildColliderBvh(SceneData *d, char *err, int errSize)
{
    int n;
    const BoundingBox *c = PublishedColliders(d, &n);
    if (n == 0) return true;
    d->colliderOrder = malloc(sizeof(int) * (size_t)n);
    d->colliderNodes = malloc(sizeof(BvhNode) * 2 * (size_t)n);
    if (!d->colliderOrder || !d->colliderNodes) {
        snprintf(err, errSize, "%s: out of memory (collider BVH)", d->path);
        return false;
    }
    for (int i = 0; i < n; i++) d->colliderOrder[i] = i;
    d->colliderNodeCount = 1;
    ColliderBvhBuild(d, c, 0, 0, n, 0);
    return true;
}

// What every load starts with, whatever its input: the paths, the built-in environment and (editor)
// the brief and the first dependency, the map file itself. False when out of memory.
static bool InitData(SceneData *d, const char *mapPath)
{
    char root[MS_PATH_MAX + 2];
    RootOf(mapPath, root, sizeof(root));
    d->path = MS_StrDup(mapPath);
    d->srcPath = MS_StrDup(mapPath);
    d->root = MS_StrDup(root);
    d->formatVersion = 1;
    BuiltinEnvironment(&d->env);
    bool mem = d->path && d->srcPath && d->root;
#ifdef MONOLITH_EDITOR
    char brief[MS_PATH_MAX + 16];
    snprintf(brief, sizeof(brief), "%sMAP.md", root);
    d->brief = MS_StrDup(FileExists(brief) ? brief : "");
    char **dep0 = MS_PUSH(d->deps, d->depCount, d->depCap);
    if (dep0) *dep0 = MS_StrDup(mapPath);
    mem = mem && d->brief && dep0 && *dep0;
#endif
    return mem;
}

#ifdef MONOLITH_EDITOR
// Fills d (after InitData) from the map text: the parsed scene, before FinishParse
static bool ParseScene(SceneData *d, const char *text, int size, char *err, int errSize)
{
    if (!DefaultMaterial(d)) {
        snprintf(err, errSize, "%s: out of memory", d->path);
        return false;
    }
    Parser P = { .d = d, .path = d->path, .err = err, .errSize = errSize };
    bool ok = ParseText(&P, text, size);
    free(P.lineBuf);
    free(P.tok);
    return ok;
}
#endif

static Texture2D GetTexture(SceneData *d, const char *rel, bool isColor, bool data, int wrap, int line, char *err, int errSize)
{
    char full[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(full, sizeof(full), "%s%s", d->root, rel);
    for (int i = 0; i < d->textureCount; i++)
        if (strcmp(d->textures[i].path, full) == 0 && d->textures[i].isColor == isColor && d->textures[i].data == data &&
            d->textures[i].wrap == wrap) return d->textures[i].tex;
    char *path = MS_StrDup(full);
    if (!path || !MS_RESERVE(d->textures, d->textureCap, d->textureCount + 1)) {
        free(path);
        snprintf(err, errSize, "%s:%d: out of memory", d->srcPath, line);
        return (Texture2D){ 0 };
    }
    // R3D's default wrap is clamp, which would make uv_scale > 1 useless; map materials repeat
    // unless `wrap clamp|mirror`. Filter: trilinear with generated mipmaps (R3D's default).
    // A data_sampler's texture holds data, not colour (e.g. a tile baked by the editor's
    // --bake-surface, read with texelFetch): linear (no sRGB decode), nearest, no mipmaps.
    static const TextureWrap WRAPS[] = { TEXTURE_WRAP_REPEAT, TEXTURE_WRAP_CLAMP, TEXTURE_WRAP_MIRROR_REPEAT };
    double t0 = GetTime();
    Texture2D tex = R3D_LoadTextureEx(full, WRAPS[wrap % 3], data ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_TRILINEAR, isColor && !data);
    d->times.textures += (GetTime() - t0) * 1000.0;
    if (tex.id == 0) {
        free(path);
        snprintf(err, errSize, "%s:%d: cannot load the texture \"%s\"", d->srcPath, line, rel);
        return tex;
    }
    TexEntry *e = &d->textures[d->textureCount++];
    e->path = path;
    e->isColor = isColor;
    e->data = data;
    e->wrap = wrap;
    e->tex = tex;
    return tex;
}

// --- Models: load, pick data (BVH), transforms, draw groups

static Vector3 TriCentroid(const ModelPick *mp, int t)
{
    const unsigned int *v = &mp->tri[3 * t];
    return Vector3Scale(Vector3Add(Vector3Add(mp->pos[v[0]], mp->pos[v[1]]), mp->pos[v[2]]), 1.0f / 3.0f);
}

// Node `ni` over triangles [first, first + count): leaves of <= 4 triangles, split at the
// middle of the longest axis of the centroids' bounds (in place: tri[] gets reordered)
static void BvhBuild(ModelPick *mp, int ni, int first, int count, int depth)
{
    BvhNode *nd = &mp->nodes[ni];
    nd->mn = (Vector3){ INFINITY, INFINITY, INFINITY };
    nd->mx = (Vector3){ -INFINITY, -INFINITY, -INFINITY };
    Vector3 cmn = nd->mn, cmx = nd->mx;
    for (int t = first; t < first + count; t++) {
        for (int k = 0; k < 3; k++) {
            Vector3 p = mp->pos[mp->tri[3 * t + k]];
            nd->mn = Vector3Min(nd->mn, p);
            nd->mx = Vector3Max(nd->mx, p);
        }
        Vector3 c = TriCentroid(mp, t);
        cmn = Vector3Min(cmn, c);
        cmx = Vector3Max(cmx, c);
    }
    nd->left = -1;
    nd->first = first;
    nd->count = count;
    if (count <= 4 || depth >= 60) return;
    Vector3 ext = Vector3Subtract(cmx, cmn);
    int axis = (ext.x >= ext.y && ext.x >= ext.z) ? 0 : (ext.y >= ext.z) ? 1 : 2;
    float split = (((float *)&cmn)[axis] + ((float *)&cmx)[axis]) * 0.5f;
    int i = first, j = first + count - 1;
    while (i <= j) {
        Vector3 c = TriCentroid(mp, i);
        if (((float *)&c)[axis] < split) i++;
        else {
            for (int k = 0; k < 3; k++) {
                unsigned int s = mp->tri[3 * i + k];
                mp->tri[3 * i + k] = mp->tri[3 * j + k];
                mp->tri[3 * j + k] = s;
            }
            j--;
        }
    }
    int leftCount = i - first;
    if (leftCount == 0 || leftCount == count) leftCount = count / 2;    // all centroids equal
    int l = mp->nodeCount;
    mp->nodeCount += 2;
    nd->left = l;
    nd->count = 0;
    BvhBuild(mp, l, first, leftCount, depth + 1);
    BvhBuild(mp, l + 1, first + leftCount, count - leftCount, depth + 1);
}

static void FreeModelPick(ModelPick *mp)
{
    free(mp->pos);
    free(mp->tri);
    free(mp->nodes);
    memset(mp, 0, sizeof(*mp));
}

// Copies the triangles of a loaded model (R3D_IMPORT_RETAIN_MESH_DATA) into a ModelPick
// with a BVH, then frees R3D's CPU mesh data
static void BuildModelPick(MS_ModelFile *F, ModelPick *mp)
{
    R3D_Model *m = &F->model;
    int verts = 0, tris = 0;
    for (int i = 0; i < m->meshCount; i++) {
        if (!m->meshData || m->meshes[i].primitiveType != R3D_PRIMITIVE_TRIANGLES) continue;
        const R3D_MeshData *md = &m->meshData[i];
        verts += md->vertexCount;
        tris += md->indexCount > 0 ? md->indexCount / 3 : md->vertexCount / 3;
    }
    F->triangleCount = tris;
    if (tris > 0 && (mp->pos = malloc(sizeof(Vector3) * verts)) && (mp->tri = malloc(sizeof(unsigned int) * 3 * tris)) &&
        (mp->nodes = malloc(sizeof(BvhNode) * 2 * tris))) {
        int v0 = 0, t = 0;
        for (int i = 0; i < m->meshCount; i++) {
            if (m->meshes[i].primitiveType != R3D_PRIMITIVE_TRIANGLES) continue;
            const R3D_MeshData *md = &m->meshData[i];
            for (int k = 0; k < md->vertexCount; k++) mp->pos[v0 + k] = md->vertices[k].position;
            int n = md->indexCount > 0 ? md->indexCount / 3 * 3 : md->vertexCount / 3 * 3;
            for (int k = 0; k < n; k++) mp->tri[3 * t + k] = v0 + (md->indexCount > 0 ? md->indices[k] : (unsigned int)k);
            t += n / 3;
            v0 += md->vertexCount;
        }
        mp->triCount = tris;
        mp->vertCount = verts;
        mp->nodeCount = 1;
        BvhBuild(mp, 0, 0, tris, 0);
    } else if (tris > 0) FreeModelPick(mp);
    if (m->meshData) {
        for (int i = 0; i < m->meshCount; i++) {
            R3D_UnloadMeshData(m->meshData[i]);
            memset(&m->meshData[i], 0, sizeof(m->meshData[i]));
        }
    }
}

// A model file R3D / assimp cannot read: "map:line: message"
#ifdef MONOLITH_EDITOR
#define MODEL_LOAD_ERROR "%s:%d: cannot load the model \"%s\" (unsupported or broken file; see the log; " \
                         "--inspect-model shows why)"
#else
#define MODEL_LOAD_ERROR "%s:%d: cannot load the model \"%s\" (unsupported or broken file; see the log)"
#endif

// Loads every model file of `d` (GPU). On failure: "map:line: message"
static bool LoadModelFiles(SceneData *d, char *err, int errSize)
{
    for (int i = 0; i < d->modelFileCount; i++) {
        MS_ModelFile *F = &d->modelFiles[i];
        char full[MS_PATH_MAX + MS_PATH_MAX];
        snprintf(full, sizeof(full), "%s%s", d->root, F->path);
        F->model = R3D_LoadModelEx(full, R3D_IMPORT_RETAIN_MESH_DATA);
        if (F->model.meshCount <= 0 || !F->model.meshes) {
            snprintf(err, errSize, MODEL_LOAD_ERROR, d->srcPath, F->line, F->path);
            return false;
        }
        F->loaded = true;
        F->bounds = F->model.aabb;
        F->meshCount = F->model.meshCount;
        F->materialCount = F->model.materialCount;
        BuildModelPick(F, &d->modelPick[i]);
        TraceLog(LOG_INFO, "monolith_scene: model %s: %d meshes, %d triangles, %d materials, size %.3f x %.3f x %.3f",
                 F->path, F->meshCount, F->triangleCount, F->materialCount, F->bounds.max.x - F->bounds.min.x,
                 F->bounds.max.y - F->bounds.min.y, F->bounds.max.z - F->bounds.min.z);
    }
    return true;
}

// noGpu loads (the peek before R3D_Init, reading a map without a window) don't upload models, but a
// broken model file must still fail the load the way the GPU load would. So every model
// file is imported here with assimp on the CPU (no GL) and dropped: the same call and
// post-processing flags as R3D_LoadImporter (r3d_importer.c: POST_PROCESS_BASELINE +
// aiProcess_GenNormals, the flags R3D_LoadModelEx(.., R3D_IMPORT_RETAIN_MESH_DATA) uses), the
// same failure test, plus "no meshes" (R3D's model takes scene->mNumMeshes meshes; the GPU
// load fails on 0, e.g. a text file named .obj). assimp is declared by hand: its headers
// aren't on every include path the runtime is built with, but it is always linked (R3D
// needs it). Only the first three members of the C struct aiScene are read (unchanged since
// assimp 3).
struct MS_AiScenePrefix { unsigned int mFlags; void *mRootNode; unsigned int mNumMeshes; };
extern const struct MS_AiScenePrefix *aiImportFile(const char *file, unsigned int flags);
extern void aiReleaseImport(const struct MS_AiScenePrefix *scene);
extern const char *aiGetErrorString(void);
#define MS_AI_IMPORT_FLAGS (0x1u | 0x2u | 0x8u | 0x20u | 0x1000u | 0x8000u | 0x40000u | 0x800000u)
#define MS_AI_SCENE_FLAGS_INCOMPLETE 0x1u

// On failure: "map:line: message" (the GPU load's wording)
static bool CheckModelFiles(SceneData *d, char *err, int errSize)
{
    for (int i = 0; i < d->modelFileCount; i++) {
        const MS_ModelFile *F = &d->modelFiles[i];
        char full[MS_PATH_MAX + MS_PATH_MAX];
        snprintf(full, sizeof(full), "%s%s", d->root, F->path);
        const struct MS_AiScenePrefix *sc = aiImportFile(full, MS_AI_IMPORT_FLAGS);
        bool ok = sc && sc->mRootNode && !(sc->mFlags & MS_AI_SCENE_FLAGS_INCOMPLETE) && sc->mNumMeshes > 0;
        if (!ok) TraceLog(LOG_WARNING, "monolith_scene: model %s: %s", full, !sc || !sc->mRootNode || (sc->mFlags & MS_AI_SCENE_FLAGS_INCOMPLETE)
                                                                         ? aiGetErrorString() : "no meshes in the file");
        if (sc) aiReleaseImport(sc);
        if (!ok) {
            snprintf(err, errSize, MODEL_LOAD_ERROR, d->srcPath, F->line, F->path);
            return false;
        }
    }
    return true;
}

// World AABB of the model-space box b under the linear part of m plus `t`
static BoundingBox TransformBox(BoundingBox b, Matrix m)
{
    BoundingBox r = { { INFINITY, INFINITY, INFINITY }, { -INFINITY, -INFINITY, -INFINITY } };
    for (int i = 0; i < 8; i++) {
        Vector3 p = { (i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z };
        p = Vector3Transform(p, m);
        r.min = Vector3Min(r.min, p);
        r.max = Vector3Max(r.max, p);
    }
    return r;
}

// Model-space bounds of a file under the linear matrix m (rotation x scale), cached per
// (file, rotation, scale) for one ResolveModels call
typedef struct { int file; Vector3 rot, scale; BoundingBox box; } RotBoundsEntry;
typedef struct { RotBoundsEntry *e; int count, cap; } RotBoundsCache;

// The tight bounds of the file's vertices under m: the file's AABB turned and re-boxed is
// too big for a pitched or rolled model (a rock at rot 0 30 20 floated 0.89 m with origin
// bottom, and its collider was far too wide). Without CPU vertex data (noGpu loads, a file
// without triangles) or when out of memory: the file's AABB corners, transformed and re-boxed.
static BoundingBox RotatedFileBounds(const SceneData *d, const MS_ModelInstance *M, Matrix m, RotBoundsCache *cache)
{
    const MS_ModelFile *F = &d->modelFiles[M->file];
    BoundingBox fb = F->loaded ? F->bounds : (BoundingBox){ 0 };
    const ModelPick *mp = d->modelPick ? &d->modelPick[M->file] : NULL;
    if (!F->loaded || !mp || !mp->pos || mp->vertCount <= 0) return TransformBox(fb, m);
    for (int i = 0; i < cache->count; i++) {
        const RotBoundsEntry *e = &cache->e[i];
        if (e->file == M->file && memcmp(&e->rot, &M->rotationDeg, sizeof(Vector3)) == 0
            && memcmp(&e->scale, &M->scale, sizeof(Vector3)) == 0)
            return e->box;
    }
    BoundingBox r = { { INFINITY, INFINITY, INFINITY }, { -INFINITY, -INFINITY, -INFINITY } };
    for (int k = 0; k < mp->vertCount; k++) {
        Vector3 p = Vector3Transform(mp->pos[k], m);
        r.min = Vector3Min(r.min, p);
        r.max = Vector3Max(r.max, p);
    }
    RotBoundsEntry *e = MS_PUSH(cache->e, cache->count, cache->cap);
    if (e) *e = (RotBoundsEntry){ M->file, M->rotationDeg, M->scale, r };
    return r;
}

// Transforms, world bounds and colliders of every instance (file bounds: zero box when the
// files were not loaded); then the collider list (boxes first)
static bool ResolveModels(SceneData *d, char *err, int errSize)
{
    RotBoundsCache cache = { 0 };
    int nc = d->boxCount;
    if (d->modelCount > 0) {
        d->colliders = malloc(sizeof(BoundingBox) * ((size_t)d->boxCount + (size_t)d->modelColliderCount));
        if (!d->colliders) {
            snprintf(err, errSize, "%s: out of memory", d->path);
            return false;
        }
        if (d->boxCount > 0) memcpy(d->colliders, d->boxes, sizeof(BoundingBox) * d->boxCount);
    }
    for (int i = 0; i < d->modelCount; i++) {
        MS_ModelInstance *M = &d->models[i];
        const MS_ModelFile *F = &d->modelFiles[M->file];
        BoundingBox fb = F->loaded ? F->bounds : (BoundingBox){ 0 };
        Quaternion qy = QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, M->rotationDeg.x * DEG2RAD);
        Quaternion qx = QuaternionFromAxisAngle((Vector3){ 1, 0, 0 }, M->rotationDeg.y * DEG2RAD);
        Quaternion qz = QuaternionFromAxisAngle((Vector3){ 0, 0, 1 }, M->rotationDeg.z * DEG2RAD);
        M->rotation = QuaternionNormalize(QuaternionMultiply(QuaternionMultiply(qy, qx), qz));
        // R * S, columns scaled (raylib matrices are column-major: m0..m2 = column 0)
        Matrix m = QuaternionToMatrix(M->rotation);
        m.m0 *= M->scale.x; m.m1 *= M->scale.x; m.m2 *= M->scale.x;
        m.m4 *= M->scale.y; m.m5 *= M->scale.y; m.m6 *= M->scale.y;
        m.m8 *= M->scale.z; m.m9 *= M->scale.z; m.m10 *= M->scale.z;
        // Bounds around the origin: the vertices' (tight). Horizontally, origin bottom/center
        // put the centre of the file's AABB (turned with the model, so it does not wander as
        // the model yaws) at x z; vertically the lowest vertex (bottom) or the middle of the
        // lowest and highest vertex (center) at y, so a pitched or rolled model stands on y
        // instead of floating above it. (Yaw only: the same y as the re-boxed AABB.)
        BoundingBox lb = RotatedFileBounds(d, M, m, &cache);
        BoundingBox ob = TransformBox(fb, m);      // the file's AABB, turned and re-boxed
        Vector3 shift = { 0 };
        if (M->origin == MS_ORIGIN_BOTTOM)
            shift = (Vector3){ -(ob.min.x + ob.max.x) * 0.5f, -lb.min.y, -(ob.min.z + ob.max.z) * 0.5f };
        else if (M->origin == MS_ORIGIN_CENTER)
            shift = (Vector3){ -(ob.min.x + ob.max.x) * 0.5f, -(lb.min.y + lb.max.y) * 0.5f, -(ob.min.z + ob.max.z) * 0.5f };
        M->translation = Vector3Add(M->position, shift);
        m.m12 = M->translation.x; m.m13 = M->translation.y; m.m14 = M->translation.z;
        M->transform = m;
        M->bounds = (BoundingBox){ Vector3Add(lb.min, M->translation), Vector3Add(lb.max, M->translation) };

        int localFirst = M->colliderFirst, localCount = M->colliderCount;
        M->colliderFirst = nc;
        if (M->collide == MS_COLLIDE_AABB) d->colliders[nc++] = M->bounds;
        else if (M->collide == MS_COLLIDE_BOXES)
            for (int k = 0; k < localCount; k++) d->colliders[nc++] = TransformBox(d->localColliders[localFirst + k], m);
        M->colliderCount = nc - M->colliderFirst;
    }
    d->colliderCount = nc;
    free(cache.e);
    return true;
}

static CellItem MakeCellItem(int group, int index, BoundingBox b, float cell)
{
    CellItem it = { .group = group, .index = index, .bounds = b };
    it.big = b.max.x - b.min.x > cell || b.max.z - b.min.z > cell;
    if (!it.big) {
        it.cx = (int)floorf((b.min.x + b.max.x) * 0.5f / cell);
        it.cz = (int)floorf((b.min.z + b.max.z) * 0.5f / cell);
    }
    return it;
}

static int CompareCellItems(const void *pa, const void *pb)
{
    const CellItem *a = pa, *b = pb;
    if (a->group != b->group) return a->group < b->group ? -1 : 1;
    if (a->big != b->big) return a->big < b->big ? -1 : 1;
    if (a->cx != b->cx) return a->cx < b->cx ? -1 : 1;
    if (a->cz != b->cz) return a->cz < b->cz ? -1 : 1;
    return a->index < b->index ? -1 : a->index > b->index;
}

// Appends the ranges of items[0..count) (one group, sorted): a new one wherever the cell changes
static bool AddRanges(DrawRange **ranges, int *n, int *cap, int group, const CellItem *items, int count)
{
    for (int k = 0; k < count; k++) {
        const CellItem *it = &items[k];
        if (k == 0 || it->big != items[k - 1].big || it->cx != items[k - 1].cx || it->cz != items[k - 1].cz) {
            DrawRange *r = MS_PUSH(*ranges, *n, *cap);
            if (!r) return false;
            *r = (DrawRange){ .group = group, .first = k, .bounds = it->bounds };
        }
        DrawRange *r = &(*ranges)[*n - 1];
        r->count++;
        r->bounds.min = Vector3Min(r->bounds.min, it->bounds.min);
        r->bounds.max = Vector3Max(r->bounds.max, it->bounds.max);
    }
    return true;
}

static void FreeModelGroups(SceneData *d)
{
    for (int g = 0; g < d->modelGroupCount; g++) {
        ModelGroup *G = &d->modelGroups[g];
        if (G->instLoaded) R3D_UnloadInstanceBuffer(G->inst);
        free(G->slotMat);
        free(G->model.materials);
        if (G->ownMeshes) free(G->model.meshes);
    }
    free(d->modelGroups);
    d->modelGroups = NULL;
    d->modelGroupCount = 0;
    free(d->modelRanges);
    d->modelRanges = NULL;
    d->modelRangeCount = d->modelRangeCap = 0;
}

// The material of slot s for instance M: material.N, then the instance's override, else -1
static int SlotMaterial(const SceneData *d, const MS_ModelInstance *M, int s)
{
    int m = M->ref.material;
    for (int k = 0; k < M->slotCount; k++)
        if (d->modelSlots[M->slotFirst + k].slot == s) m = d->modelSlots[M->slotFirst + k].material;
    return m;
}

// One draw group per (file, materials, shadow): instance buffers with position, rotation
// and scale, and the R3D_Model copy it is drawn with. GPU; after ResolveModels.
static bool BuildModelGroups(SceneData *d, char *err, int errSize)
{
    if (d->modelCount == 0) return true;
    int maxSlots = 1;
    for (int f = 0; f < d->modelFileCount; f++)
        if (d->modelFiles[f].materialCount > maxSlots) maxSlots = d->modelFiles[f].materialCount;
    d->modelGroups = calloc(d->modelCount, sizeof(ModelGroup));
    int *groupOf = malloc(sizeof(int) * d->modelCount);
    int *tmp = malloc(sizeof(int) * maxSlots);
    if (!d->modelGroups || !groupOf || !tmp) {
        free(groupOf); free(tmp);
        snprintf(err, errSize, "%s: out of memory", d->path);
        return false;
    }
    for (int i = 0; i < d->modelCount; i++) {
        const MS_ModelInstance *M = &d->models[i];
        const MS_ModelFile *F = &d->modelFiles[M->file];
        int slots = F->materialCount;
        for (int k = 0; k < M->slotCount; k++)
            if (d->modelSlots[M->slotFirst + k].slot >= F->materialCount && (i == 0 || d->models[i - 1].ref.line != M->ref.line))
                TraceLog(LOG_WARNING, "monolith_scene: %s:%d: material.%d: \"%s\" has %d material slot%s (0..%d); ignored",
                         d->srcPath, M->ref.line, d->modelSlots[M->slotFirst + k].slot, F->path, F->materialCount,
                         F->materialCount == 1 ? "" : "s", F->materialCount - 1);
        for (int s = 0; s < slots; s++) tmp[s] = SlotMaterial(d, M, s);
        int g = 0;
        for (; g < d->modelGroupCount; g++) {
            const ModelGroup *G = &d->modelGroups[g];
            if (G->file == M->file && G->castShadow == M->castShadow && memcmp(G->slotMat, tmp, sizeof(int) * slots) == 0) break;
        }
        if (g == d->modelGroupCount) {
            ModelGroup *G = &d->modelGroups[d->modelGroupCount++];
            G->file = M->file;
            G->castShadow = M->castShadow;
            G->slotMat = malloc(sizeof(int) * (slots > 0 ? slots : 1));
            if (G->slotMat) memcpy(G->slotMat, tmp, sizeof(int) * slots);
        }
        d->modelGroups[g].count++;
        groupOf[i] = g;
    }
    free(tmp);

    // Instances sorted by (group, cell): each group's buffer holds its cells one after the other
    CellItem *items = malloc(sizeof(CellItem) * (d->modelCount > 0 ? d->modelCount : 1));
    if (!items) {
        free(groupOf);
        snprintf(err, errSize, "%s: out of memory", d->path);
        return false;
    }
    for (int i = 0; i < d->modelCount; i++) items[i] = MakeCellItem(groupOf[i], i, d->models[i].bounds, MS_MODEL_CELL_SIZE);
    qsort(items, d->modelCount, sizeof(CellItem), CompareCellItems);

    bool ok = true;
    for (int g = 0, at = 0; g < d->modelGroupCount && ok; at += d->modelGroups[g++].count) {
        ModelGroup *G = &d->modelGroups[g];
        const MS_ModelFile *F = &d->modelFiles[G->file];
        G->model = F->model;
        G->model.materials = malloc(sizeof(R3D_Material) * (F->materialCount > 0 ? F->materialCount : 1));
        if (!G->slotMat || !G->model.materials) { ok = false; break; }
        for (int s = 0; s < F->materialCount; s++) {
            const R3D_Material *own = &F->model.materials[s];
            int mi = G->slotMat[s];
            if (mi < 0) { G->model.materials[s] = *own; continue; }
            R3D_Material m = d->materials[mi].material;
            unsigned inh = d->matDefs[mi].inherit;
            if (inh & INHERIT_ALBEDO) m.albedo.texture = own->albedo.texture;
            if (inh & INHERIT_NORMAL) m.normal = own->normal;
            if (inh & INHERIT_ORM) m.orm.texture = own->orm.texture;
            if (inh & INHERIT_EMISSION) m.emission = own->emission;
            G->model.materials[s] = m;
        }
        if (!G->castShadow) {
            G->model.meshes = malloc(sizeof(R3D_Mesh) * F->model.meshCount);
            if (!G->model.meshes) { G->model.meshes = F->model.meshes; ok = false; break; }
            G->ownMeshes = true;
            for (int k = 0; k < F->model.meshCount; k++) {
                G->model.meshes[k] = F->model.meshes[k];
                G->model.meshes[k].shadowCastMode = R3D_SHADOW_CAST_DISABLED;
            }
        }
        int count = G->count;
        G->inst = R3D_LoadInstanceBuffer(count, R3D_INSTANCE_POSITION | R3D_INSTANCE_ROTATION | R3D_INSTANCE_SCALE);
        G->instLoaded = true;
        if (G->inst.capacity < count) { ok = false; break; }
        Vector3 *pos = R3D_MapInstances(G->inst, R3D_INSTANCE_POSITION, false);
        Quaternion *rot = R3D_MapInstances(G->inst, R3D_INSTANCE_ROTATION, false);
        Vector3 *scl = R3D_MapInstances(G->inst, R3D_INSTANCE_SCALE, false);
        if (!pos || !rot || !scl) ok = false;
        for (int k = 0; k < count && ok; k++) {
            const MS_ModelInstance *M = &d->models[items[at + k].index];
            pos[k] = M->translation;
            rot[k] = M->rotation;
            scl[k] = M->scale;
        }
        R3D_UnmapInstances(G->inst, (pos ? R3D_INSTANCE_POSITION : 0) | (rot ? R3D_INSTANCE_ROTATION : 0) | (scl ? R3D_INSTANCE_SCALE : 0));
        if (ok && !AddRanges(&d->modelRanges, &d->modelRangeCount, &d->modelRangeCap, g, items + at, count)) ok = false;
    }
    free(items);
    free(groupOf);
    if (!ok) snprintf(err, errSize, "%s: out of memory (model instances)", d->path);
    return ok;
}

static void FreeGpu(SceneData *d)
{
    if (!d->gpu) return;
    FreeModelGroups(d);
    for (int i = 0; i < d->modelFileCount; i++)
        if (d->modelFiles[i].model.meshes) {
            R3D_UnloadModel(d->modelFiles[i].model, true);
            memset(&d->modelFiles[i].model, 0, sizeof(R3D_Model));
        }
    for (int i = 0; i < d->materialCount; i++) {
        if (d->inst && d->inst[i].capacity > 0) R3D_UnloadInstanceBuffer(d->inst[i]);
        if (d->materials[i].material.shader) R3D_UnloadSurfaceShader(d->materials[i].material.shader);
        d->materials[i].material.shader = NULL;
    }
    // (after their aliases, which share their programs)
    for (int i = 0; i < d->shaderFileCount; i++) R3D_UnloadSurfaceShader(d->shaderFiles[i].shader);
    free(d->shaderFiles);
    d->shaderFiles = NULL;
    d->shaderFileCount = d->shaderFileCap = 0;
    if (d->unit.capacity > 0) R3D_UnloadInstanceBuffer(d->unit);
    d->unit = (R3D_InstanceBuffer){ 0 };
    free(d->inst);
    free(d->instCount);
    d->inst = NULL;
    d->instCount = NULL;
    for (int r = 0; r < d->boxRangeCount; r++) R3D_UnloadMesh(d->boxRanges[r].mesh);
    free(d->boxRanges);
    d->boxRanges = NULL;
    d->boxRangeCount = d->boxRangeCap = 0;
    for (int i = 0; i < d->textureCount; i++) {
        R3D_UnloadTexture(d->textures[i].tex);
        free(d->textures[i].path);
    }
    free(d->textures);
    d->textures = NULL;
    d->textureCount = d->textureCap = 0;
    if (d->sky.texture) R3D_UnloadCubemap(d->sky);
    if (d->ambientMap.irradiance || d->ambientMap.prefilter) R3D_UnloadAmbientMap(d->ambientMap);
    d->sky = (R3D_Cubemap){ 0 };
    d->ambientMap = (R3D_AmbientMap){ 0 };
    for (int i = 0; d->lightShadow && i < d->lightCount; i++)
        if (d->lights[i].shadow && d->lightShadow[i].handle) R3D_UnloadShadowMap(d->lightShadow[i]);
    free(d->lightShadow);
    free(d->lightShadowDrawn);
    d->lightShadow = NULL;
    d->lightShadowDrawn = NULL;
    d->gpu = false;
}

// --- Hidden box faces
// A face of box i is hidden when its rectangle lies inside the union of the rectangles (on its
// plane) of the occluders: opaque boxes B != i that occupy the space right outside it (a face
// toward +a at plane p: B.min[a] <= p < B.max[a]; toward -a: B.min[a] < p <= B.max[a]). Exact
// float compares, no epsilon (touching boxes come from identical arithmetic), and a face stays
// whenever it isn't proven covered. The occluders are looked up in a grid of HIDE_CELL cells
// (a hash of the occupied cells); boxes in too many cells are tested against every face.
#define HIDE_CELL           4.0f
#define HIDE_CELLS_MAX      1024        // a box in more cells goes to the "big" list; a face over more cells stays
#define HIDE_ENTRIES_MAX    (1 << 22)   // grid entries (~60 B each with the hash): more and nothing is hidden (a map of huge overlapping boxes)
#define HIDE_PIECES         64          // a face the occluders leave in more pieces stays
// An occluder must reach this far (m) beyond the face's plane. 0: every covered face goes (25 % of
// maps/monolith's box triangles), but a face lying only a few cm behind its covering surface shows
// through the 24-bit depth buffer at some pixels past ~30 m (z-fighting), so the image changes
// there (0.03 % of the pixels). 0.25 keeps such faces: 9 % of the triangles go, and the image of
// maps/monolith stays the same within its render noise (0.05: 13 %, a few pixels differ)
#define HIDE_MIN_COVER      0.0f

typedef struct { float mn[3], mx[3]; } Box6;
typedef struct { float x0, y0, x1, y1; } Rect;                  // on the two axes other than the face's

static int HideCell(float v)
{
    float c = floorf(v / HIDE_CELL);
    return c < -1e9f ? -1000000000 : c > 1e9f ? 1000000000 : (int)c;
}

// Cell range of a box; false when it covers more than HIDE_CELLS_MAX cells
static bool HideCells(const Box6 *b, int lo[3], int hi[3])
{
    double n = 1.0;
    for (int a = 0; a < 3; a++) {
        lo[a] = HideCell(b->mn[a]);
        hi[a] = HideCell(b->mx[a]);
        n *= (double)hi[a] - lo[a] + 1.0;
    }
    return n <= HIDE_CELLS_MAX;
}

static unsigned long long HideKey(const int cell[3])
{
    unsigned long long k = 0;
    for (int a = 0; a < 3; a++) {
        long long c = (long long)cell[a] + (1 << 20);           // (clamped: far cells share a key, the exact test follows)
        k = k << 21 | (unsigned long long)(c < 0 ? 0 : c > 0x1FFFFF ? 0x1FFFFF : c);
    }
    return k;
}

// Does B occupy the space right outside the face (axis a, plane p, toward dir) and overlap its rect?
static inline bool Covers(const Box6 *B, int a, int dir, float p, int u, int v, const Rect *face)
{
    return (dir > 0 ? B->mn[a] <= p && p + HIDE_MIN_COVER < B->mx[a] : B->mn[a] + HIDE_MIN_COVER < p && p <= B->mx[a]) &&
           B->mn[u] < face->x1 && B->mx[u] > face->x0 && B->mn[v] < face->y1 && B->mx[v] > face->y0;
}

// Takes B's rectangle out of the pieces; false when that leaves too many pieces
static bool CoverPieces(Rect *piece, int *n, const Box6 *B, int u, int v)
{
    for (int k = 0; k < *n; ) {
        Rect P = piece[k];
        float x0 = P.x0 > B->mn[u] ? P.x0 : B->mn[u], y0 = P.y0 > B->mn[v] ? P.y0 : B->mn[v];
        float x1 = P.x1 < B->mx[u] ? P.x1 : B->mx[u], y1 = P.y1 < B->mx[v] ? P.y1 : B->mx[v];
        if (x0 >= x1 || y0 >= y1) { k++; continue; }
        if (*n + 3 >= HIDE_PIECES) return false;
        piece[k] = piece[--*n];                                 // P is replaced by what B leaves of it
        if (P.x0 < x0) piece[(*n)++] = (Rect){ P.x0, P.y0, x0, P.y1 };
        if (x1 < P.x1) piece[(*n)++] = (Rect){ x1, P.y0, P.x1, P.y1 };
        if (P.y0 < y0) piece[(*n)++] = (Rect){ x0, P.y0, x1, y0 };
        if (y1 < P.y1) piece[(*n)++] = (Rect){ x0, y1, x1, P.y1 };
    }
    return true;
}

// nb: the boxes that touch or overlap box i (the only ones that can cover its faces)
static bool FaceHidden(const Box6 *bx, const int *nb, int nbCount, int i, int a, int dir, Rect *piece)
{
    const Box6 *A = &bx[i];
    int u = a == 0 ? 1 : 0, v = a == 2 ? 1 : 2;
    float p = dir > 0 ? A->mx[a] : A->mn[a];
    Rect face = { A->mn[u], A->mn[v], A->mx[u], A->mx[v] };
    if (face.x0 >= face.x1 || face.y0 >= face.y1) return true;     // no area: nothing to see
    int n = 1;
    piece[0] = face;
    for (int k = 0; k < nbCount; k++) {
        if (!Covers(&bx[nb[k]], a, dir, p, u, v, &face)) continue;
        if (!CoverPieces(piece, &n, &bx[nb[k]], u, v)) return false;
        if (n == 0) return true;
    }
    return false;
}

// The occluder grid: per occupied cell (open-addressed hash of HideKey), its occluders in
// ascending box order (as sorted entries were), so neighbours come in the same order as before
typedef struct { unsigned long long key; int first, count; } HideSlot;

typedef struct {
    const SceneData *d;
    const Box6 *bx;
    const int *big;
    int bigCount;
    const HideSlot *slots;
    unsigned int slotMask;
    const int *ids;                     // the cells' occluders, slot by slot
    const Box6 *gbox;                   // their boxes, in the same order
    int axis[6], dir[6];
    unsigned char *hidden;
    atomic_int next;                    // first box of the next chunk to take
} HideJob;

static unsigned int HideHash(unsigned long long key, unsigned int mask)
{
    return (unsigned int)((key * 0x9E3779B97F4A7C15ull) >> 32) & mask;
}

static const HideSlot *HideFind(const HideJob *J, unsigned long long key)
{
    for (unsigned int h = HideHash(key, J->slotMask); ; h = (h + 1) & J->slotMask) {
        const HideSlot *S = &J->slots[h];
        if (S->count == 0 || S->key == key) return S->count ? S : NULL;
    }
}

// The faces of boxes [from, to) (scratch: nb and stamp per box, piece HIDE_PIECES)
static void HideRange(HideJob *J, int from, int to, int *nb, int *stamp, Rect *piece)
{
    const SceneData *d = J->d;
    for (int i = from; i < to; i++) {
        int m = d->boxRefs[i].material, nbCount = 0, lo[3], hi[3], cell[3];
        if (m < 0 || m >= d->materialCount || !d->matDefs[m].merge) continue;
        const Box6 *A = &J->bx[i];
        // Neighbours: the occluders in A's cells (a touching box shares a cell with A), once
        // each, and the big ones. A box in too many cells only has the big ones.
        if (HideCells(A, lo, hi)) {
            for (cell[0] = lo[0]; cell[0] <= hi[0]; cell[0]++)
                for (cell[1] = lo[1]; cell[1] <= hi[1]; cell[1]++)
                    for (cell[2] = lo[2]; cell[2] <= hi[2]; cell[2]++) {
                        const HideSlot *S = HideFind(J, HideKey(cell));
                        for (int e = S ? S->first : 0, end = S ? S->first + S->count : 0; e < end; e++) {
                            const Box6 *B = &J->gbox[e];
                            if (!((B->mn[0] <= A->mx[0]) & (B->mx[0] >= A->mn[0]) & (B->mn[1] <= A->mx[1]) &
                                  (B->mx[1] >= A->mn[1]) & (B->mn[2] <= A->mx[2]) & (B->mx[2] >= A->mn[2]))) continue;
                            int id = J->ids[e];
                            if (id == i || stamp[id] == i) continue;
                            stamp[id] = i;
                            nb[nbCount++] = id;
                        }
                    }
        }
        for (int k = 0; k < J->bigCount; k++) {
            const Box6 *B = &J->bx[J->big[k]];
            if (J->big[k] == i || B->mn[0] > A->mx[0] || B->mx[0] < A->mn[0] || B->mn[1] > A->mx[1] || B->mx[1] < A->mn[1] ||
                B->mn[2] > A->mx[2] || B->mx[2] < A->mn[2]) continue;
            nb[nbCount++] = J->big[k];
        }
        for (int f = 0; f < 6; f++)
            if (FaceHidden(J->bx, nb, nbCount, i, J->axis[f], J->dir[f], piece)) J->hidden[i] |= (unsigned char)(1u << f);
    }
}

#define HIDE_CHUNK          512         // boxes a thread takes at a time
#define HIDE_THREADS        8           // at most (with the calling thread)

// A worker: chunks of boxes until none are left (out of memory: it takes none)
static void HideWork(HideJob *J)
{
    int count = J->d->boxCount;
    int *nb = malloc(sizeof(int) * (size_t)count), *stamp = malloc(sizeof(int) * (size_t)count);
    Rect *piece = malloc(sizeof(Rect) * HIDE_PIECES);
    bool ok = nb && stamp && piece;
    for (int i = 0; ok && i < count; i++) stamp[i] = -1;
    for (int from; ok && (from = atomic_fetch_add(&J->next, HIDE_CHUNK)) < count; )
        HideRange(J, from, from + HIDE_CHUNK < count ? from + HIDE_CHUNK : count, nb, stamp, piece);
    free(piece);
    free(stamp);
    free(nb);
}

static void *HideThread(void *job)
{
    HideWork(job);
    return NULL;
}

// hidden[b] bit f: face f of box b (R3D's cube face order) is hidden; only for boxes of merged
// materials. Each box's faces depend only on the boxes, so threads take boxes in chunks and the
// result is the same as one thread's. False when out of memory.
static bool FindHiddenFaces(const SceneData *d, const R3D_MeshData *cube, unsigned char *hidden)
{
    int count = d->boxCount;
    size_t cap = count > 0 ? (size_t)count : 1;
    Box6 *bx = malloc(sizeof(Box6) * cap);
    int *big = malloc(sizeof(int) * cap), *ids = NULL;
    HideSlot *slots = NULL;
    Box6 *gbox = NULL;
    int bigCount = 0;
    unsigned int slotMask = 0;
    bool ok = bx && big;
    long long total = 0;
    for (int i = 0; i < count && ok; i++) {
        const BoundingBox *B = &d->boxes[i];
        bx[i] = (Box6){ { B->min.x, B->min.y, B->min.z }, { B->max.x, B->max.y, B->max.z } };
        int m = d->boxRefs[i].material, lo[3], hi[3];
        if (m < 0 || m >= d->materialCount || !d->matDefs[m].occluder) continue;
        if (!HideCells(&bx[i], lo, hi)) big[bigCount++] = i;
        else total += (long long)(hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1);
    }
    if (ok && total <= HIDE_ENTRIES_MAX) {
        // Two passes over the occluders' cells: count per cell, then place each box after the
        // cell's earlier ones (ascending box order within a cell)
        unsigned int nslots = 2;
        while (nslots < 2 * (unsigned long long)total) nslots <<= 1;
        slotMask = nslots - 1;
        slots = calloc(nslots, sizeof(HideSlot));
        ids = malloc(sizeof(int) * (total > 0 ? (size_t)total : 1));
        gbox = malloc(sizeof(Box6) * (total > 0 ? (size_t)total : 1));
        ok = slots && ids && gbox;
        for (int pass = 0; pass < 2 && ok; pass++) {
            for (unsigned int h = 0, at = 0; pass == 1 && h < nslots; h++) {
                slots[h].first = (int)at;           // (advanced while filling, set back below)
                at += (unsigned int)slots[h].count;
            }
            for (int i = 0; i < count; i++) {
                int m = d->boxRefs[i].material, lo[3], hi[3], cell[3];
                if (m < 0 || m >= d->materialCount || !d->matDefs[m].occluder || !HideCells(&bx[i], lo, hi)) continue;
                for (cell[0] = lo[0]; cell[0] <= hi[0]; cell[0]++)
                    for (cell[1] = lo[1]; cell[1] <= hi[1]; cell[1]++)
                        for (cell[2] = lo[2]; cell[2] <= hi[2]; cell[2]++) {
                            unsigned long long key = HideKey(cell);
                            unsigned int h = HideHash(key, slotMask);
                            while (slots[h].count && slots[h].key != key) h = (h + 1) & slotMask;
                            if (pass == 0) {
                                slots[h].key = key;
                                slots[h].count++;
                            } else {
                                int e = slots[h].first++;
                                ids[e] = i;
                                gbox[e] = bx[i];
                            }
                        }
            }
        }
        for (unsigned int h = 0; ok && h < nslots; h++) slots[h].first -= slots[h].count;
    }
    if (ok && slots) {
        HideJob J = { .d = d, .bx = bx, .big = big, .bigCount = bigCount, .slots = slots, .slotMask = slotMask,
                      .ids = ids, .gbox = gbox, .hidden = hidden };
        atomic_init(&J.next, 0);
        for (int f = 0; f < 6; f++) {
            Vector3 nrm = R3D_UnpackNormal(cube->vertices[f * 4].normal);
            J.axis[f] = fabsf(nrm.x) > 0.5f ? 0 : fabsf(nrm.y) > 0.5f ? 1 : 2;
            J.dir[f] = (J.axis[f] == 0 ? nrm.x : J.axis[f] == 1 ? nrm.y : nrm.z) > 0.0f ? 1 : -1;
        }
        pthread_t thread[HIDE_THREADS - 1];
        int threads = 0;
        while (threads < HIDE_THREADS - 1 && count > (threads + 1) * HIDE_CHUNK &&
               pthread_create(&thread[threads], NULL, HideThread, &J) == 0) threads++;
        HideWork(&J);
        for (int t = 0; t < threads; t++) pthread_join(thread[t], NULL);
        // Done when every chunk was taken: a worker does each chunk it takes, and one that is out
        // of memory takes none
        ok = atomic_load(&J.next) >= count;
    }
    free(gbox);
    free(ids);
    free(slots);
    free(big);
    free(bx);
    return ok;
}

// One mesh of the visible faces of the boxes items[0..R->count): R3D's own unit-cube faces (vertex
// format, UVs, normals, winding), each corner placed the way the GPU placed an instanced cube's,
// local * scale + position with the same position and scale floats
static bool BuildBoxMesh(SceneData *d, DrawRange *R, const CellItem *items, const unsigned char *hidden, const R3D_MeshData *cube)
{
    int visible = 0;
    for (int k = 0; k < R->count; k++)
        for (int f = 0; f < 6; f++) visible += !(hidden[items[k].index] >> f & 1);
    d->boxTrisDrawn += 2LL * visible;
    if (visible == 0) return true;
    R3D_MeshData md = R3D_LoadMeshData(4 * visible, 6 * visible);
    if (!md.vertices || !md.indices) {
        R3D_UnloadMeshData(md);
        return false;
    }
    for (int k = 0; k < R->count; k++) {
        const BoundingBox *B = &d->boxes[items[k].index];
        Vector3 pos = Vector3Scale(Vector3Add(B->min, B->max), 0.5f), scl = Vector3Subtract(B->max, B->min);
        for (int f = 0; f < 6; f++) {
            if (hidden[items[k].index] >> f & 1) continue;
            unsigned int base = (unsigned int)md.vertexCount;
            for (int c = 0; c < 4; c++) {
                R3D_Vertex vx = cube->vertices[f * 4 + c];
                vx.position = (Vector3){ pos.x + vx.position.x * scl.x, pos.y + vx.position.y * scl.y, pos.z + vx.position.z * scl.z };
                md.vertices[md.vertexCount++] = vx;
            }
            for (int j = 0; j < 6; j++) md.indices[md.indexCount++] = base + cube->indices[f * 6 + j] - (unsigned int)(f * 4);
        }
    }
    R->mesh = R3D_LoadMesh(R3D_PRIMITIVE_TRIANGLES, md, &R->bounds);
    R3D_UnloadMeshData(md);
    d->boxMeshCount += R->mesh.vertexCount > 0;
    return R->mesh.vertexCount > 0;
}

// Boxes -> draws per material and 32 m cell (boxRanges: one cluster each). A material that is
// `merge` gets, per range, one mesh of its boxes' visible faces (BuildBoxMesh): fewer triangles,
// and it casts the same shadow (a hidden face lies inside an opaque box, so it was never the
// nearest depth of any shadow texel). The others get an instance buffer of unit cubes (centre and
// size per box) shared by their ranges. `unit` draws a mesh as an instance (see MS_DrawGeometry).
static bool UploadBoxes(SceneData *d, char *err, int errSize)
{
    CellItem *items = malloc(sizeof(CellItem) * (d->boxCount > 0 ? d->boxCount : 1));
    if (!items) {
        snprintf(err, errSize, "%s: out of memory", d->path);
        return false;
    }
    bool anyMerge = false;
    int n = 0;
    for (int b = 0; b < d->boxCount; b++) {
        int m = d->boxRefs[b].material;
        if (m < 0 || m >= d->materialCount) continue;
        items[n++] = MakeCellItem(m, b, d->boxes[b], MS_CELL_SIZE);
        d->instCount[m]++;
        anyMerge |= d->matDefs[m].merge;
    }
    qsort(items, n, sizeof(CellItem), CompareCellItems);

    double t0 = GetTime();
    R3D_MeshData cube = { 0 };
    unsigned char *hidden = NULL;
    bool ok = true;
    if (anyMerge) {
        cube = R3D_GenMeshDataCube(1.0f, 1.0f, 1.0f);
        hidden = calloc(d->boxCount, 1);
        double th = GetTime();
        ok = cube.vertexCount == 24 && cube.indexCount == 36 && hidden && FindHiddenFaces(d, &cube, hidden);
        d->times.hidden = (GetTime() - th) * 1000.0;
        d->unit = R3D_LoadInstanceBuffer(1, R3D_INSTANCE_POSITION | R3D_INSTANCE_SCALE);
        Vector3 *pos = R3D_MapInstances(d->unit, R3D_INSTANCE_POSITION, false);
        Vector3 *scl = R3D_MapInstances(d->unit, R3D_INSTANCE_SCALE, false);
        if (pos && scl) {
            pos[0] = Vector3Zero();
            scl[0] = Vector3One();
        } else {
            ok = false;
        }
        R3D_UnmapInstances(d->unit, (pos ? R3D_INSTANCE_POSITION : 0) | (scl ? R3D_INSTANCE_SCALE : 0));
    }
    d->mergeMs = (float)((GetTime() - t0) * 1000.0);
    for (int m = 0, at = 0; m < d->materialCount && ok; at += d->instCount[m++]) {
        int count = d->instCount[m];
        if (count == 0) continue;
        int first = d->boxRangeCount;
        if (!d->matDefs[m].merge) {
            d->inst[m] = R3D_LoadInstanceBuffer(count, R3D_INSTANCE_POSITION | R3D_INSTANCE_SCALE);
            if (d->inst[m].capacity < count) { ok = false; break; }
            Vector3 *pos = R3D_MapInstances(d->inst[m], R3D_INSTANCE_POSITION, false);
            Vector3 *scl = R3D_MapInstances(d->inst[m], R3D_INSTANCE_SCALE, false);
            if (!pos || !scl) ok = false;
            for (int k = 0; k < count && ok; k++) {
                const BoundingBox *B = &d->boxes[items[at + k].index];
                pos[k] = Vector3Scale(Vector3Add(B->min, B->max), 0.5f);
                scl[k] = Vector3Subtract(B->max, B->min);
            }
            R3D_UnmapInstances(d->inst[m], (pos ? R3D_INSTANCE_POSITION : 0) | (scl ? R3D_INSTANCE_SCALE : 0));
            d->boxTrisDrawn += 12LL * count;
        }
        if (ok && !AddRanges(&d->boxRanges, &d->boxRangeCount, &d->boxRangeCap, m, items + at, count)) ok = false;
        if (d->matDefs[m].merge) {
            t0 = GetTime();
            for (int r = first; ok && r < d->boxRangeCount; r++)
                ok = BuildBoxMesh(d, &d->boxRanges[r], items + at + d->boxRanges[r].first, hidden, &cube);
            d->mergeMs += (float)((GetTime() - t0) * 1000.0);
        }
    }
    R3D_UnloadMeshData(cube);
    free(hidden);
    free(items);
    if (!ok) snprintf(err, errSize, "%s: out of memory (box meshes)", d->path);
    return ok;
}

// Whole-identifier search for `word` in s[0..end)
static bool HasWord(const char *s, const char *end, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = s; p + n <= end; p++) {
        if (memcmp(p, word, n) != 0) continue;
        if ((p > s && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) || (p + n < end && (isalnum((unsigned char)p[n]) || p[n] == '_'))) continue;
        return true;
    }
    return false;
}

// What a surface shader's source says (a text scan, conservative: comments count too).
// needsCube: it reads the box instance (INSTANCE_*) or the model-space POSITION in vertex(), so
// it only works on instanced unit cubes. cutout: it may discard or write ALPHA.
static void ScanShader(const char *src, bool *needsCube, bool *cutout)
{
    *needsCube = strstr(src, "INSTANCE_") != NULL;
    *cutout = strstr(src, "ALPHA") != NULL || strstr(src, "discard") != NULL;
    const char *fn = strstr(src, "void vertex()");
    const char *open = fn ? strchr(fn, '{') : NULL;
    if (!open || *needsCube) return;
    const char *end = open;
    for (int depth = 0; *end; end++) {
        if (*end == '{') depth++;
        else if (*end == '}' && --depth == 0) break;
    }
    *needsCube = HasWord(open, end, "POSITION");
}

// The compiled shader of a material's shader file: loaded (and its source scanned) by the first
// material that uses the file. NULL when it fails to load or out of memory (err says which).
static ShaderFile *GetShaderFile(SceneData *d, const MS_Material *M, const MatDef *D, char *err, int errSize)
{
    for (int i = 0; i < d->shaderFileCount; i++)
        if (strcmp(d->shaderFiles[i].rel, M->shader) == 0) return &d->shaderFiles[i];
    ShaderFile *f = MS_PUSH(d->shaderFiles, d->shaderFileCount, d->shaderFileCap);
    if (!f) {
        snprintf(err, errSize, "%s:%d: out of memory", d->srcPath, D->shaderLine);
        return NULL;
    }
    char full[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(full, sizeof(full), "%s%s", d->root, M->shader);
    double t0 = GetTime();
#ifdef MONOLITH_EDITOR
    if (d->shaderDefines && (!d->shaderDefinesFile || strcmp(d->shaderDefinesFile, M->shader) == 0)) {
        // The defines go first, so the source's #ifdef blocks see them (R3D copies a
        // #define like any other global code)
        char *text = LoadFileText(full);
        size_t a = strlen(d->shaderDefines), b = text ? strlen(text) : 0;
        char *code = text ? malloc(a + b + 2) : NULL;
        if (code) {
            memcpy(code, d->shaderDefines, a);
            code[a] = '\n';
            memcpy(code + a + 1, text, b + 1);
        }
        *f = (ShaderFile){ .rel = M->shader, .shader = code ? R3D_LoadSurfaceShaderFromMemory(code) : NULL };
        free(code);
        UnloadFileText(text);
    } else
#endif
    *f = (ShaderFile){ .rel = M->shader, .shader = R3D_LoadSurfaceShader(full) };
    d->times.shaderLoads++;
    if (!f->shader) {
        d->shaderFileCount--;
        snprintf(err, errSize, "%s:%d: cannot load the surface shader \"%s\" (see the log)", d->srcPath, D->shaderLine, M->shader);
        return NULL;
    }
    char *src = LoadFileText(full);
    f->scanned = src != NULL;
    if (src) ScanShader(src, &f->needsCube, &f->cutout);
    else f->needsCube = f->cutout = true;
    UnloadFileText(src);
    d->times.shaders += (GetTime() - t0) * 1000.0;
    return f;
}

// Creates every GPU resource of `d` (a previous load's are still alive: new before old, see
// the instance-buffer note in LoadBytes). On failure everything created so far is freed.
static bool UploadGpu(SceneData *d, char *err, int errSize)
{
    d->gpu = true;
    d->inst = calloc(d->materialCount, sizeof(R3D_InstanceBuffer));
    d->instCount = calloc(d->materialCount, sizeof(int));
    d->lightShadow = calloc(d->lightCount > 0 ? d->lightCount : 1, sizeof(R3D_ShadowMap));
    d->lightShadowDrawn = calloc(d->lightCount > 0 ? d->lightCount : 1, sizeof(bool));
    if (!d->inst || !d->instCount || !d->lightShadow || !d->lightShadowDrawn) {
        snprintf(err, errSize, "%s: out of memory", d->path);
        FreeGpu(d);
        return false;
    }
    for (int i = 0; i < d->materialCount; i++) {
        MS_Material *M = &d->materials[i];
        MatDef *D = &d->matDefs[i];
        R3D_Material *m = &M->material;
        m->shader = NULL;
        if (M->shader) {
            // One compiled shader per file (a compile is ~2 ms, and the first frame compiles each
            // shader's variants again); the materials' aliases are created in material order, so
            // R3D's draw sort, which orders shaders by creation, sees the same order as with one
            // compiled shader per material
            ShaderFile *f = GetShaderFile(d, M, D, err, errSize);
            if (f) m->shader = R3D_LoadSurfaceShaderAlias(f->shader);
            if (f && !m->shader) snprintf(err, errSize, "%s:%d: out of memory", d->srcPath, D->shaderLine);
            if (!m->shader) {
                FreeGpu(d);
                return false;
            }
            for (int u = 0; u < D->uniformCount; u++)
                R3D_SetSurfaceShaderUniform(m->shader, D->uniforms[u].name, D->uniforms[u].v);
            for (int s = 0; s < D->samplerCount; s++) {
                Texture2D tex = GetTexture(d, D->samplers[s].path, true, D->samplers[s].data, D->wrap, D->samplers[s].line, err, errSize);
                if (!tex.id) { FreeGpu(d); return false; }
                R3D_SetSurfaceShaderSampler(m->shader, D->samplers[s].name, tex);
            }
        }
        static const bool IS_COLOR[MAP_COUNT] = { true, false, false, true };
        Texture2D *slot[MAP_COUNT] = { &m->albedo.texture, &m->normal.texture, &m->orm.texture, &m->emission.texture };
        for (int k = 0; k < MAP_COUNT; k++) {
            if (!D->maps[k]) continue;
            *slot[k] = GetTexture(d, D->maps[k], IS_COLOR[k], false, D->wrap, D->mapLine[k], err, errSize);
            if (!slot[k]->id) { FreeGpu(d); return false; }
        }
    }

    // How the boxes of each material are drawn
    for (int i = 0; i < d->materialCount; i++) {
        MS_Material *M = &d->materials[i];
        MatDef *D = &d->matDefs[i];
        R3D_Material *m = &M->material;
        bool needsCube = false, cutout = false;
        if (M->shader) {
            const ShaderFile *f = GetShaderFile(d, M, D, err, errSize);     // (loaded above)
            needsCube = !f || f->needsCube;
            cutout = !f || f->cutout;
            if (f && f->scanned && f->needsCube && !D->instanced)
                TraceLog(LOG_WARNING, "monolith_scene: %s:%d: material \"%s\": shader \"%s\" reads the box instance (INSTANCE_*, "
                         "or POSITION in vertex()): add `instanced on` to the material", d->srcPath, D->shaderLine, M->name, M->shader);
        }
        // Boxes are merged into a visible-face mesh unless the shader needs the instanced cubes. Only
        // opaque materials: R3D picks the lights of a blended (forward) draw by the mesh's own bounds,
        // which for the instanced unit cube are the cube at the origin, so a mesh would be lit differently.
        // Those that also never discard can hide another box's face (see FindHiddenFaces)
        D->merge = !D->instanced && !needsCube && m->cullMode == R3D_CULL_BACK && m->transparencyMode == R3D_TRANSPARENCY_OPAQUE;
        D->occluder = D->merge && !cutout && m->billboardMode == R3D_BILLBOARD_DISABLED && m->albedo.color.a == 255 &&
                      !D->maps[MAP_ALBEDO] && m->alphaCutoff <= 1.0f;
    }

    double tBoxes = GetTime();
    bool boxesOk = UploadBoxes(d, err, errSize);
    d->times.meshes = (GetTime() - tBoxes) * 1000.0 - d->times.hidden;
    if (!boxesOk) {
        FreeGpu(d);
        return false;
    }

    // Sky, and the ambient map from the ambient-only copy of it
    EnvDef *e = &d->env;
    R3D_ProceduralSky sp = e->sky;
    if (!e->skySunDirSet) sp.sunDirection = e->sun.direction;
    if (e->skyMode == SKY_PROCEDURAL) d->sky = R3D_GenProceduralSky(e->skySize, sp);
    else if (e->skyMode == SKY_PANORAMA) {
        char full[MS_PATH_MAX + MS_PATH_MAX];
        snprintf(full, sizeof(full), "%s%s", d->root, e->skyPanorama);
        d->sky = R3D_LoadCubemap(full, R3D_CUBEMAP_LAYOUT_PANORAMA);
        if (!d->sky.texture) {
            snprintf(err, errSize, "%s: cannot load the sky panorama \"%s\"", d->path, e->skyPanorama);
            FreeGpu(d);
            return false;
        }
    }
    if (e->ambientMode == AMB_PROCEDURAL) {
        R3D_ProceduralSky ap = sp;
        for (int k = 0; k < COUNT(SKY_KEYS); k++)
            if (e->ambientSet & (1u << k))
                memcpy((char *)&ap + SKY_KEYS[k].off, (const char *)&e->ambient + SKY_KEYS[k].off, TypeSize(SKY_KEYS[k].type));
        R3D_Cubemap ambientSky = R3D_GenProceduralSky(e->ambientSize, ap);
        d->ambientMap = R3D_GenAmbientMap(ambientSky, R3D_AMBIENT_ILLUMINATION | R3D_AMBIENT_REFLECTION);
        R3D_UnloadCubemap(ambientSky);
    } else if (e->ambientMode == AMB_SAME && d->sky.texture) {
        d->ambientMap = R3D_GenAmbientMap(d->sky, R3D_AMBIENT_ILLUMINATION | R3D_AMBIENT_REFLECTION);
    }

    for (int i = 0; i < d->lightCount; i++) {
        d->lightShadowDrawn[i] = false;
        if (d->lights[i].shadow) d->lightShadow[i] = R3D_LoadShadowMap(d->lights[i].light.type);
    }
    if (!BuildModelGroups(d, err, errSize)) {
        FreeGpu(d);
        return false;
    }
    return true;
}

static void FreeData(SceneData *d)
{
    if (!d) return;
    FreeGpu(d);
    for (int i = 0; i < d->modelFileCount; i++) {
        FreeModelPick(&d->modelPick[i]);
        free((char *)d->modelFiles[i].path);
    }
    free(d->modelPick);
    free(d->modelFiles);
    free(d->models);
    free(d->localColliders);
    free(d->modelSlots);
    free(d->colliders);
    free(d->colliderNodes);
    free(d->colliderOrder);
    for (int i = 0; i < d->materialCount; i++) {
        free((char *)d->materials[i].name);
        free((char *)d->materials[i].shader);
        MatDef *D = &d->matDefs[i];
        for (int k = 0; k < MAP_COUNT; k++) free(D->maps[k]);
        for (int k = 0; k < D->uniformCount; k++) free(D->uniforms[k].name);
        for (int k = 0; k < D->samplerCount; k++) { free(D->samplers[k].name); free(D->samplers[k].path); }
        free(D->uniforms);
        free(D->samplers);
    }
    free(d->materials);
    free(d->matDefs);
    free(d->matHash);
    for (int i = 0; i < d->groupCount; i++) free(d->groups[i]);
    free(d->groups);
    for (int i = 0; i < d->viewpointCount; i++) free((char *)d->viewpoints[i].name);
    free(d->viewpoints);
    free(d->boxes);
    free(d->boxRefs);
    free(d->lights);
    free(d->stmtKeys);
    free(d->stmtLines);
    free(d->stmtSet);
    free(d->entityIds);
    free(d->entityObjects);
    free(d->entityTransforms);
    free(d->entitySources);
    free(d->entityModels);
    free(d->objEntities);
    free(d->entityHash);
    free(d->path);
    free(d->srcPath);
    free(d->root);
#ifdef MONOLITH_EDITOR
    for (int i = 0; i < d->depCount; i++) free(d->deps[i]);
    free(d->deps);
    free(d->brief);
    free(d->text);
    free(d->lineStarts);
#endif
    free(d);
}

// Points the public fields of the scene at d
static void Publish(MS_Scene *s, SceneData *d)
{
    s->formatVersion = d->formatVersion;
    s->path = d->path;
    s->root = d->root;
#ifdef MONOLITH_EDITOR
    s->brief = d->brief;
    s->hash = d->hash;
    s->mtime = d->mtime;
#endif
    s->boxes = d->boxes;
    s->boxRefs = d->boxRefs;
    s->boxCount = d->boxCount;
    s->models = d->models;
    s->modelCount = d->modelCount;
    s->modelFiles = d->modelFiles;
    s->modelFileCount = d->modelFileCount;
    s->modelSlots = d->modelSlots;
    s->modelSlotCount = d->modelSlotCount;
    // Boxes first, then the model colliders (without models: the box array itself)
    s->colliders = PublishedColliders(d, &s->colliderCount);
    s->materials = d->materials;
    s->materialCount = d->materialCount;
    s->groups = (const char *const *)d->groups;
    s->groupCount = d->groupCount;
    s->lights = d->lights;
    s->lightCount = d->lightCount;
    const SunDef *sun = &d->env.sun;
    s->sun = (MS_Sun){ sun->enabled, sun->shadow, d->sunLight, sun->softness, sun->depthBias, sun->slopeBias, sun->shadowSize };
    s->hasSpawn = d->hasSpawn;
    s->spawnFeet = d->spawnFeet;
    s->spawnYawDeg = d->spawnYawDeg;
    s->spawnPitchDeg = d->spawnPitchDeg;
    s->viewpoints = d->viewpoints;
    s->viewpointCount = d->viewpointCount;
    s->bounds = d->bounds;
    s->entityCount = d->entityCount;
    s->entityIds = d->entityIds;
    s->entityObjects = d->entityObjects;
    s->entityTransforms = d->entityTransforms;
    s->entitySources = d->entitySources;
    s->entityModels = d->entityModels;
    s->boxEntities = d->objEntities;
    s->modelEntities = d->objEntities + d->boxCount;
    s->lightEntities = d->objEntities + d->boxCount + d->modelCount;
}

#ifdef MONOLITH_EDITOR
static void FreeWatch(MS_Private *p)
{
    for (int i = 0; i < p->watchCount; i++) free(p->watch[i]);
    p->watchCount = 0;
}

// The watch list becomes the files of this attempt (on out of memory: as many as fit)
static void SetWatch(MS_Private *p, const SceneData *d)
{
    FreeWatch(p);
    if (!MS_RESERVE(p->watch, p->watchCap, d->depCount) || !MS_RESERVE(p->watchMtime, p->watchMtimeCap, d->depCount)) return;
    for (int i = 0; i < d->depCount; i++) {
        if (!(p->watch[i] = MS_StrDup(d->deps[i]))) break;
        p->watchMtime[i] = GetFileModTime(d->deps[i]);
        p->watchCount = i + 1;
    }
}
#endif

// The rest of a load, the same for every input: what follows from the parsed scene (FinishParse
// on), then with `gpu` the model files and every GPU resource; else the model files are only checked
static bool FinishLoad(SceneData *d, const MS_LoadOptions *opt, bool gpu, char *err, int errSize)
{
    if (!FinishParse(d, opt)) {
        snprintf(err, errSize, "%s: out of memory", d->path);
        return false;
    }
    bool ok;
#ifdef MONOLITH_EDITOR
    d->shaderDefines = opt->shaderDefines;
    d->shaderDefinesFile = opt->shaderDefinesFile;
#endif
    double t0 = GetTime();
    if (gpu) {
        d->gpu = true;                  // (so FreeData unloads what LoadModelFiles loaded)
        ok = LoadModelFiles(d, err, errSize);
    } else ok = CheckModelFiles(d, err, errSize);
    if (ok) ok = ResolveModels(d, err, errSize);
    d->times.models = (GetTime() - t0) * 1000.0;
    if (ok) {
        FinishGeometry(d);
        t0 = GetTime();
        ok = BuildColliderBvh(d, err, errSize);
        d->times.bvh = (GetTime() - t0) * 1000.0;
    }
    if (ok && !BuildEntities(d)) {
        snprintf(err, errSize, "%s: out of memory", d->path);
        ok = false;
    }
    if (ok && gpu) ok = UploadGpu(d, err, errSize);
    return ok;
}

#ifdef MONOLITH_EDITOR
// Where each line of the map text starts (MS_LineText is called every frame for the hovered line;
// walking a 4 MB text to it took ~0.05 ms): a line starts at 0 and after each newline, inside the text
static void IndexLines(SceneData *d)
{
    int n = d->textSize > 0;
    for (int i = 0; i + 1 < d->textSize; i++) n += d->text[i] == '\n';
    d->lineStarts = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!d->lineStarts) return;
    d->lineCount = 0;
    if (d->textSize > 0) d->lineStarts[d->lineCount++] = 0;
    for (int i = 0; i + 1 < d->textSize; i++)
        if (d->text[i] == '\n') d->lineStarts[d->lineCount++] = i + 1;
}
#endif

// One line per load with GPU upload: where the time went (ms). The first frame after it, which
// compiles the shader variants it draws with, is logged by MS_PushLights.
static void LogLoadTimes(const SceneData *d, bool again, double total)
{
    const LoadTimes *t = &d->times;
    const char *what = "load";
#ifdef MONOLITH_EDITOR
    if (again) what = "reload";
#else
    (void)again;
#endif
    double rest = total - t->parse - t->models - t->bvh - t->shaders - t->textures - t->hidden - t->meshes;
    TraceLog(LOG_INFO, "monolith_scene: %s %.1f ms: parse %.1f, models %.1f, collider BVH %.1f, surface shaders %.1f "
             "(%d compiled), textures %.1f (%d), hidden faces %.1f, box meshes %.1f, rest %.1f",
             what, total, t->parse, t->models, t->bvh, t->shaders, t->shaderLoads, t->textures, d->textureCount,
             t->hidden, t->meshes, rest);
}

// Loads a map from its file's bytes (takes ownership): fills a SceneData from them, finishes the
// load and swaps it in on success
static bool LoadBytes(MS_Scene *s, const char *mapPath, char *bytes, int size, char *err, int errSize)
{
    MS_Private *p = s->priv;
    SceneData *d = calloc(1, sizeof(*d));
    if (!d) {
        snprintf(err, errSize, "%s: out of memory", mapPath);
        free(bytes);
        return false;
    }
    double tLoad = GetTime();
    bool ok = InitData(d, mapPath);
    if (!ok) snprintf(err, errSize, "%s: out of memory", mapPath);
    bool compiled = IsCompiledMap(bytes, size);
    if (ok && compiled) ok = ReadCompiled(d, bytes, size, err, errSize);
#ifdef MONOLITH_EDITOR
    if (ok && !compiled && size > MS_FILE_MAX) {
        snprintf(err, errSize, "%s: cannot read file (or larger than 4 MB)", mapPath);
        ok = false;
    }
    if (ok && !compiled) ok = ParseScene(d, bytes, size, err, errSize);
    d->times.parse = (GetTime() - tLoad) * 1000.0;
    if (ok && compiled) WarnStale(d);
    d->text = compiled ? NULL : bytes;
    d->textSize = compiled ? 0 : size;
    if (!compiled) IndexLines(d);
    d->hash = Fnv1a64Update(FNV_OFFSET, bytes, (size_t)size);
    d->mtime = GetFileModTime(mapPath);
    // Watch the files of this attempt (also after a failure: a shader that is still missing
    // reloads the package once it appears)
    SetWatch(p, d);
    p->triedHash = CombinedHash(bytes, size, p->watch, p->watchCount);
    p->settleTime = 0.0f;
    if (compiled) free(bytes);
#else
    d->times.parse = (GetTime() - tLoad) * 1000.0;
    if (ok && !compiled) {
        snprintf(err, errSize, "%s: not a compiled map (this program reads compiled maps, <name>.mapb: compile the "
                 "map's text with monolith)", mapPath);
        ok = false;
    }
    free(bytes);
#endif
    if (ok) ok = FinishLoad(d, &p->opt, p->gpu, err, errSize);
    if (ok && p->gpu && !p->warnedShadowSize && R3D_GetHint(R3D_HINT_SHADOW_DIR_SIZE) != d->env.sun.shadowSize) {
        p->warnedShadowSize = true;
        TraceLog(LOG_WARNING, "monolith_scene: shadow.size %d is not the R3D hint %d (it applies at R3D_Init: "
                 "MS_SetInitHints before it, or restart)", d->env.sun.shadowSize, R3D_GetHint(R3D_HINT_SHADOW_DIR_SIZE));
    }
    if (!ok) {
        FreeData(d);
        return false;
    }

    // Swap: the new GPU resources exist already, a previous load's are freed after. R3D
    // caches the instance buffer names it last bound (instances_enable in r3d_render.c) and
    // skips re-binding when they match; freeing first let GL hand out the same names again,
    // so the new map was drawn with the old boxes.
    SceneData *old = p->d;
    p->d = d;
    s->version++;
    Publish(s, d);
    FreeData(old);
    if (p->gpu) LogLoadTimes(d, old != NULL, (GetTime() - tLoad) * 1000.0);
    if (errSize > 0) err[0] = '\0';
    return true;
}

static bool LoadPath(MS_Scene *s, const char *path, char *err, int errSize)
{
    char mapPath[MS_PATH_MAX];
    if (!ResolveMapPath(path, mapPath, sizeof(mapPath), err, errSize)) return false;
    int size;
    char *bytes = ReadWholeFile(mapPath, MS_MAX_COMPILED_BYTES, &size, err, errSize);
    if (!bytes) return false;
    return LoadBytes(s, mapPath, bytes, size, err, errSize);
}

// #endregion

// ======================================================================================
// #region Public API: loading
// ======================================================================================

MS_Scene *MS_Load(const char *path, const MS_LoadOptions *opt, char *err, int errSize)
{
    MS_Scene *s = calloc(1, sizeof(*s));
    MS_Private *p = s ? calloc(1, sizeof(*p)) : NULL;
    if (!p) {
        free(s);
        snprintf(err, errSize, "%s: out of memory", path);
        return NULL;
    }
    s->priv = p;
    if (opt) p->opt = *opt;
    p->gpu = !p->opt.noGpu;
#ifdef MS_PATCHED_R3D
    p->rs = (MS_RenderSettings){ .shadowMode = MS_SHADOW_TILES, .fogCacheSub = 2, .depthPrepass = true,
                                 .renderScale = 1.0f, .upscale = R3D_UPSCALE_BICUBIC, .aoVolumeIntensity = 1.0f,
                                 .sunShadow = true };
#else
    p->rs = (MS_RenderSettings){ .shadowMode = MS_SHADOW_CAMERA, .fogCacheSub = 2, .depthPrepass = true,
                                 .renderScale = 1.0f, .upscale = R3D_UPSCALE_BICUBIC, .aoVolumeIntensity = 1.0f,
                                 .sunShadow = true };
#endif
#ifdef MONOLITH_EDITOR
    p->rs.sunOn = true;
#endif
    p->tiles.current = -1;
    if (p->gpu) {
        p->cube = R3D_GenMeshCube(1.0f, 1.0f, 1.0f);
#ifdef MS_PATCHED_R3D
        R3D_SetDepthPrepass(p->rs.depthPrepass);
#endif
    }
    if (!LoadPath(s, path, err, errSize)) {
        MS_Unload(s);
        return NULL;
    }
    if (p->gpu) {
        for (int i = 0; i < SUN_TILE_COUNT; i++) p->sunShadow[i] = R3D_LoadShadowMap(R3D_LIGHT_DIR);
        p->sunShadowLoaded = true;
        int mapSize = R3D_GetHint(R3D_HINT_SHADOW_DIR_SIZE);
        TraceLog(LOG_INFO, "monolith_scene: %s: %d boxes (%d draws, %d merged meshes: %lld of %lld triangles drawn, %.0f ms), %d models (%d files, %d draws), %d groups, %d materials, %d lights; sun shadow %d px, %.2f cm texels",
                 s->path, s->boxCount, p->d->boxRangeCount, p->d->boxMeshCount, p->d->boxTrisDrawn, 12LL * s->boxCount, p->d->mergeMs,
                 s->modelCount, s->modelFileCount, p->d->modelRangeCount, s->groupCount, s->materialCount, s->lightCount, mapSize,
                 100.0f * 2.0f * FitRadius(s->sun.light.range, TILE_FIT_FOVY, TILE_FIT_ASPECT) / (float)mapSize);
    }
    return s;
}

void MS_Unload(MS_Scene *s)
{
    if (!s) return;
    MS_Private *p = s->priv;
    if (p) {
        FreeData(p->d);
#ifdef MS_PATCHED_R3D
        // The fog bake is R3D-global: without this, the next scene drawn in this R3D context
        // would sample this scene's sun visibility (R3D frees the volume itself at R3D_Close)
        if (p->gpu && p->fogCacheVersion) R3D_ClearFogSunVisibility();
#endif
        if (p->gpu) {
#ifdef MS_PATCHED_R3D
            if (p->aoVersion) R3D_ClearWorldOcclusion();
#endif
            if (p->sunShadowLoaded)
                for (int i = 0; i < SUN_TILE_COUNT; i++) R3D_UnloadShadowMap(p->sunShadow[i]);
            R3D_UnloadMesh(p->cube);
        }
#ifdef MONOLITH_EDITOR
        FreeWatch(p);
        free(p->watch);
        free(p->watchMtime);
#endif
        free(p);
    }
    free(s);
}

#ifdef MONOLITH_EDITOR
MS_ReloadResult MS_Reload(MS_Scene *s, const char *path, char *err, int errSize)
{
    char *cur = MS_StrDup(path ? path : s->path);     // (s->path is freed by a successful load)
    if (!cur) {
        snprintf(err, errSize, "out of memory");
        return MS_RELOAD_FAILED;
    }
    bool ok = LoadPath(s, cur, err, errSize);
    free(cur);
    snprintf(s->priv->error, sizeof(s->priv->error), "%s", ok ? "" : err);
    return ok ? MS_RELOADED : MS_RELOAD_FAILED;
}

MS_ReloadResult MS_Poll(MS_Scene *s, float dt)
{
    MS_Private *p = s->priv;
    p->pollTimer += dt;
    if (p->pollTimer < MS_POLL_INTERVAL || p->watchCount == 0) return MS_UNCHANGED;
    p->pollTimer = 0.0f;

    if (GetFileModTime(p->watch[0]) == 0) return MS_UNCHANGED;   // missing for a moment (editor rename): wait
    for (int i = 0; i < p->watchCount; i++) {
        long m = GetFileModTime(p->watch[i]);
        if (m != p->watchMtime[i]) {
            p->watchMtime[i] = m;
            p->settleTime = MS_SETTLE_TIME;
        }
    }
    if (p->settleTime <= 0.0f) return MS_UNCHANGED;
    p->settleTime -= MS_POLL_INTERVAL;

    char err[512], mapPath[MS_PATH_MAX];
    snprintf(mapPath, sizeof(mapPath), "%s", p->watch[0]);
    int size;
    char *text = ReadWholeFile(mapPath, MS_MAX_COMPILED_BYTES, &size, err, sizeof(err));
    if (!text) {
        // Usually transient (the next poll retries), but a file that stays unreadable or
        // too big must not fail silently: report it once the settle window has run out
        if (p->settleTime > 0.0f) return MS_UNCHANGED;
        snprintf(p->error, sizeof(p->error), "%s", err);
        p->triedHash = 0;
        return MS_RELOAD_FAILED;
    }
    unsigned long long hash = CombinedHash(text, size, p->watch, p->watchCount);
    if (hash == p->triedHash) {                 // touched, or the same broken package again
        free(text);
        return MS_UNCHANGED;
    }
    float settle = p->settleTime;
    bool ok = LoadBytes(s, mapPath, text, size, err, sizeof(err));
    p->settleTime = settle;                     // keep watching while the editor may still write
    snprintf(p->error, sizeof(p->error), "%s", ok ? "" : err);
    return ok ? MS_RELOADED : MS_RELOAD_FAILED;
}

const char *MS_Error(const MS_Scene *s)
{
    return s->priv->error;
}

const char *MS_LineText(const MS_Scene *s, int line, int *length)
{
    const SceneData *d = s->priv->d;
    if (!d || !d->text || line < 1) return NULL;
    int pos = 0;
    if (d->lineStarts) {
        if (line > d->lineCount) return NULL;
        pos = d->lineStarts[line - 1];
    } else {
        for (int l = 1; l < line; l++) {
            while (pos < d->textSize && d->text[pos] != '\n') pos++;
            if (pos >= d->textSize) return NULL;
            pos++;
        }
    }
    if (pos >= d->textSize) return NULL;
    int len = 0;
    while (pos + len < d->textSize && d->text[pos + len] != '\n') len++;
    if (len > 0 && d->text[pos + len - 1] == '\r') len--;
    *length = len;
    return d->text + pos;
}

const char *MS_MapText(const MS_Scene *s, int *size)
{
    const SceneData *d = s->priv->d;
    *size = d && d->text ? d->textSize : 0;
    return d && d->text ? d->text : "";
}

int MS_PrimitiveArgs(const char *name, unsigned char axes[8])
{
    for (int i = 0; i < COUNT(PRIMS); i++)
        if (strcmp(name, PRIMS[i].name) == 0) {
            memcpy(axes, PRIMS[i].kind, sizeof(PRIMS[i].kind));
            return PRIMS[i].argc;
        }
    return -1;
}

bool MS_GetLine(const MS_Scene *s, int line, char *out, int outSize)
{
    int len;
    const char *text = outSize > 0 ? MS_LineText(s, line, &len) : NULL;
    if (!text) return false;
    if (len > outSize - 1) len = outSize - 1;
    memcpy(out, text, len);
    out[len] = '\0';
    return true;
}

MS_SourceState MS_CompiledSource(const MS_Scene *s, const char **sourcePath)
{
    const SceneData *d = s->priv->d;
    if (sourcePath) *sourcePath = d ? d->srcPath : s->path;
    return d ? SourceState(d) : MS_SOURCE_TEXT;
}

bool MS_Compile(const char *mapPath, const char *outPath, char *err, int errSize)
{
    char path[MS_PATH_MAX];
    if (!ResolveMapPath(mapPath, path, sizeof(path), err, errSize)) return false;
    int size;
    char *text = ReadWholeFile(path, MS_FILE_MAX, &size, err, errSize);
    if (!text) return false;
    if (IsCompiledMap(text, size)) {
        snprintf(err, errSize, "%s: already a compiled map (compile the map's text, its .map file)", path);
        free(text);
        return false;
    }
    SceneData *d = calloc(1, sizeof(*d));
    bool ok = d && InitData(d, path);
    if (!ok) snprintf(err, errSize, "%s: out of memory", path);
    if (ok) ok = ParseScene(d, text, size, err, errSize);
    unsigned char *out = NULL;
    int outSize = 0;
    if (ok) ok = (out = WriteCompiled(d, GetFileName(path), Fnv1a64Update(FNV_OFFSET, text, (size_t)size), size,
                                      &outSize, err, errSize)) != NULL;
    // The package check a noGpu load makes (model files included), then the compiled bytes read
    // back as a game would (relative to the source's package folder): what compiles, loads
    if (ok) ok = FinishLoad(d, &(MS_LoadOptions){ .noGpu = true }, false, err, errSize);
    FreeData(d);
    free(text);
    if (ok) {
        SceneData *r = calloc(1, sizeof(*r));
        ok = r && InitData(r, path);
        if (!ok) snprintf(err, errSize, "%s: out of memory", path);
        if (ok) ok = ReadCompiled(r, (const char *)out, outSize, err, errSize);
        FreeData(r);
    }
    if (ok) {
        char partial[MS_PATH_MAX + 16];
        snprintf(partial, sizeof(partial), "%s.partial", outPath);
        FILE *f = fopen(partial, "wb");
        ok = f && fwrite(out, 1, (size_t)outSize, f) == (size_t)outSize;
        if (f && fclose(f) != 0) ok = false;
#ifdef _WIN32
        if (ok) remove(outPath);                // (rename does not replace a file on Windows)
#endif
        if (ok) ok = rename(partial, outPath) == 0;
        if (!ok) {
            snprintf(err, errSize, "%s: cannot write the compiled map", outPath);
            remove(partial);
        }
    }
    free(out);
    return ok;
}
#endif

// Declared by hand rather than via <GL/gl.h> / <windows.h> (which clash with raylib). On
// Windows x64 there is a single calling convention, so this matches opengl32.dll.
#define MS_GL_DEPTH_CLAMP 0x864F
extern void glEnable(unsigned int cap);

void MS_InitRenderer(void)
{
    glEnable(MS_GL_DEPTH_CLAMP);
}

void MS_SetInitHints(const MS_Scene *s)
{
    R3D_SetHint(R3D_HINT_SHADOW_DIR_SIZE, s->sun.shadowSize);
}

// #endregion

// ======================================================================================
// #region Public API: environment and drawing
// ======================================================================================

void MS_ApplyEnvironment(const MS_Scene *s)
{
    const SceneData *d = s->priv->d;
    R3D_Environment *env = R3D_GetEnvironment();
    *env = d->env.env;
    env->background.sky = d->sky;
    env->ambient.map = d->ambientMap;
#ifdef MS_PATCHED_R3D   // R3D patch 13; upstream R3D has no density fade
    env->volumetricFog.fadeStart = d->env.vfogFadeStart;
    env->volumetricFog.fadeEnd = d->env.vfogFadeEnd;
    // patch 15's transmittance integral assumes the fade-in ends before the fade-out starts
    env->volumetricFog.fadeNear = d->env.vfogFadeEnd > 0.0f && d->env.vfogFadeNear > d->env.vfogFadeStart
                                ? d->env.vfogFadeStart : d->env.vfogFadeNear;
#endif
}

MS_RenderSettings MS_GetRenderSettings(const MS_Scene *s)
{
    return s->priv->rs;
}

void MS_SetRenderSettings(MS_Scene *s, MS_RenderSettings rs)
{
    MS_Private *p = s->priv;
#ifndef MS_PATCHED_R3D
    if (rs.shadowMode == MS_SHADOW_TILES) rs.shadowMode = MS_SHADOW_CAMERA;
    rs.fogCacheVoxel = 0.0f;
    rs.aoVolumeVoxel = 0.0f;
#endif
    if (rs.fogCacheVoxel > 0.0f) {
        rs.fogCacheVoxel = Clamp(rs.fogCacheVoxel, 0.25f, 8.0f);
        rs.fogCacheSub = (int)Clamp((float)rs.fogCacheSub, 1.0f, 4.0f);
    } else rs.fogCacheVoxel = 0.0f;
    rs.renderScale = (rs.renderScale > 0.0f) ? Clamp(rs.renderScale, 0.25f, 1.0f) : 1.0f;
    rs.aoVolumeVoxel = rs.aoVolumeVoxel > 0.0f ? Clamp(rs.aoVolumeVoxel, 0.25f, 4.0f) : 0.0f;
    if (!(rs.aoVolumeIntensity >= 0.0f)) rs.aoVolumeIntensity = 1.0f;
    rs.shadowRange = rs.shadowRange > 0.0f ? Clamp(rs.shadowRange, 10.0f, 2000.0f) : 0.0f;
    rs.shadowSoftness = rs.shadowSoftness > 0.0f ? rs.shadowSoftness : 0.0f;
    if ((int)rs.upscale < (int)R3D_UPSCALE_NEAREST || (int)rs.upscale > (int)R3D_UPSCALE_LANCZOS) rs.upscale = R3D_UPSCALE_BICUBIC;
    MS_RenderSettings old = p->rs;
    p->rs = rs;
    if (rs.shadowMode != old.shadowMode) {
        // the other path rendered something else into the layers
        for (int i = 0; i < SUN_TILE_COUNT; i++) p->tiles.tile[i].valid = false;
        p->tiles.current = -1;
        p->camCache.valid = false;
    }
#ifdef MS_PATCHED_R3D
    if (p->gpu && rs.depthPrepass != old.depthPrepass) R3D_SetDepthPrepass(rs.depthPrepass);
    if (p->gpu && (rs.fogCacheVoxel != old.fogCacheVoxel || rs.fogCacheSub != old.fogCacheSub)) {
        if (p->fogCacheVersion) R3D_ClearFogSunVisibility();
        p->fogCacheVersion = 0;
    }
    if (p->gpu && (rs.aoVolumeVoxel != old.aoVolumeVoxel || rs.aoVolumeIntensity != old.aoVolumeIntensity)) {
        if (p->aoVersion) R3D_ClearWorldOcclusion();
        p->aoVersion = 0;   // re-baked by the next MS_DrawGeometry
    }
#endif
}

void MS_ApplyRenderResolution(const MS_Scene *s, int outW, int outH)
{
    const MS_RenderSettings *rs = &s->priv->rs;
    // Rounded to the nearest pixel; renderScale 1 gives exactly the output size
    int w = (int)((float)outW * rs->renderScale + 0.5f), h = (int)((float)outH * rs->renderScale + 0.5f);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int cw = 0, ch = 0;
    R3D_GetResolution(&cw, &ch);
    if (cw != w || ch != h) R3D_SetResolution(w, h);
    if (R3D_GetUpscaleMode() != rs->upscale) R3D_SetUpscaleMode(rs->upscale);
}

// --- Graphics quality (MS_Quality, monolith_scene.h)
// Presets from the measured trade-off knobs (.claude/skills/benchmarking/measurements.md "Trade-off knobs" and
// docs/user-guide.md "Graphics settings"): ultra is the map exactly; high trades SSAO + SSIL for the baked
// occlusion volume and bakes the fog's sun visibility; medium also renders at 75% with half
// the fog march steps and a 4096 shadow map; low renders at 50% without AO and bloom.

MS_Quality MS_QualityPreset(MS_QualityLevel level)
{
    MS_Quality q = {
        .renderScale = 1.0f, .upscale = R3D_UPSCALE_BICUBIC, .antiAliasing = R3D_ANTI_ALIASING_MODE_FXAA,
        .sunShadow = true, .shadowMode = MS_SHADOW_TILES,
        .ao = MS_AO_FULL,
        .volumetricFog = true, .fogStepScale = 1.0f, .fogLengthScale = 1.0f, .fogCacheSub = 2, .distanceFog = true,
        .bloom = true, .bloomIntensityScale = 1.0f,
        .exposure = 1.0f, .brightness = 1.0f, .contrast = 1.0f, .saturation = 1.0f,
        .depthPrepass = true,
    };
    switch (level) {
    case MS_QUALITY_LOW:
        q.renderScale = 0.5f;
        q.ao = MS_AO_OFF;
        q.bloom = false;
        q.shadowSize = 4096;
        q.fogCacheVoxel = 0.5f;
        q.fogStepScale = 2.0f;
        break;
    case MS_QUALITY_MEDIUM:
        q.renderScale = 0.75f;
        q.ao = MS_AO_BAKED;
        q.aoVolumeVoxel = 0.5f;
        q.aoVolumeIntensity = 0.6f;
        q.shadowSize = 4096;
        q.fogCacheVoxel = 0.5f;
        q.fogStepScale = 2.0f;
        break;
    case MS_QUALITY_HIGH:
        q.ao = MS_AO_BAKED;
        q.aoVolumeVoxel = 0.5f;
        q.aoVolumeIntensity = 0.6f;
        q.fogCacheVoxel = 0.5f;
        break;
    default: break;     // ultra, custom
    }
    return q;
}

static const char *const QUALITY_NAMES[] = { "low", "medium", "high", "ultra", "custom" };

const char *MS_QualityName(MS_QualityLevel level)
{
    return (level >= MS_QUALITY_LOW && level <= MS_QUALITY_CUSTOM) ? QUALITY_NAMES[level] : "custom";
}

bool MS_QualityFromName(const char *name, MS_QualityLevel *level)
{
    for (int i = 0; i <= MS_QUALITY_CUSTOM; i++)
        if (strcmp(name, QUALITY_NAMES[i]) == 0) { *level = (MS_QualityLevel)i; return true; }
    return false;
}

bool MS_QualityEqual(const MS_Quality *a, const MS_Quality *b)
{
#define EQ(f) (a->f == b->f)
    return EQ(renderScale) && EQ(upscale) && EQ(antiAliasing) && EQ(sunShadow) && EQ(shadowSize) &&
           EQ(shadowRange) && EQ(shadowSoftness) && EQ(shadowMode) && EQ(ao) && EQ(aoVolumeVoxel) &&
           EQ(aoVolumeIntensity) && EQ(ssaoSamples) && EQ(ssilSamples) && EQ(ssaoRadius) && EQ(ssaoIntensity) &&
           EQ(ssilRadius) && EQ(volumetricFog) && EQ(fogStepScale) && EQ(fogLengthScale) && EQ(fogCacheVoxel) &&
           EQ(fogCacheSub) && EQ(distanceFog) && EQ(bloom) && EQ(bloomIntensityScale) && EQ(ssr) && EQ(ssgi) &&
           EQ(exposure) && EQ(brightness) && EQ(contrast) && EQ(saturation) && EQ(depthPrepass);
#undef EQ
}

MS_QualityLevel MS_QualityMatch(const MS_Quality *q)
{
    for (int i = MS_QUALITY_LOW; i < MS_QUALITY_CUSTOM; i++) {
        MS_Quality p = MS_QualityPreset((MS_QualityLevel)i);
        if (MS_QualityEqual(q, &p)) return (MS_QualityLevel)i;
    }
    return MS_QUALITY_CUSTOM;
}

// A non-finite float in MS_Quality (a NaN from a bad settings file or an app bug) would
// pass every clamp and black out the frame: such a field takes ultra's value (the map's
// own) instead, with a warning. Returns how many fields it replaced.
static int QualitySanitize(MS_Quality *q)
{
    const MS_Quality ultra = MS_QualityPreset(MS_QUALITY_ULTRA);
    struct { const char *name; float *f; float def; } fields[] = {
        { "renderScale", &q->renderScale, ultra.renderScale },
        { "shadowRange", &q->shadowRange, ultra.shadowRange },
        { "shadowSoftness", &q->shadowSoftness, ultra.shadowSoftness },
        { "aoVolumeVoxel", &q->aoVolumeVoxel, ultra.aoVolumeVoxel },
        { "aoVolumeIntensity", &q->aoVolumeIntensity, ultra.aoVolumeIntensity },
        { "ssaoRadius", &q->ssaoRadius, ultra.ssaoRadius },
        { "ssaoIntensity", &q->ssaoIntensity, ultra.ssaoIntensity },
        { "ssilRadius", &q->ssilRadius, ultra.ssilRadius },
        { "fogStepScale", &q->fogStepScale, ultra.fogStepScale },
        { "fogLengthScale", &q->fogLengthScale, ultra.fogLengthScale },
        { "fogCacheVoxel", &q->fogCacheVoxel, ultra.fogCacheVoxel },
        { "bloomIntensityScale", &q->bloomIntensityScale, ultra.bloomIntensityScale },
        { "exposure", &q->exposure, ultra.exposure },
        { "brightness", &q->brightness, ultra.brightness },
        { "contrast", &q->contrast, ultra.contrast },
        { "saturation", &q->saturation, ultra.saturation },
    };
    int n = 0;
    for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
        if (isfinite(*fields[i].f)) continue;
        TraceLog(LOG_WARNING, "monolith_scene: quality %s is not a finite number, using %g", fields[i].name, fields[i].def);
        *fields[i].f = fields[i].def;
        n++;
    }
    return n;
}

void MS_ApplyQuality(MS_Scene *s, const MS_Quality *qIn)
{
    MS_Quality qs = *qIn;
    QualitySanitize(&qs);
    const MS_Quality *q = &qs;
    MS_RenderSettings rs = MS_GetRenderSettings(s);
    rs.renderScale = q->renderScale;
    rs.upscale = q->upscale;
    rs.sunShadow = q->sunShadow;
    rs.shadowRange = q->shadowRange;
    rs.shadowSoftness = q->shadowSoftness;
    rs.shadowMode = q->shadowMode;
    rs.fogCacheVoxel = q->fogCacheVoxel;
    rs.fogCacheSub = q->fogCacheSub;
    rs.depthPrepass = q->depthPrepass;
    if (q->ao == MS_AO_BAKED) {
        // 0.6 matched the mean brightness of full SSAO + SSIL in views 1-3 within 1.3/255
        rs.aoVolumeVoxel = q->aoVolumeVoxel > 0.0f ? q->aoVolumeVoxel : 0.5f;
        rs.aoVolumeIntensity = q->aoVolumeIntensity > 0.0f ? q->aoVolumeIntensity : 0.6f;
    } else {
        rs.aoVolumeVoxel = q->aoVolumeVoxel;
        rs.aoVolumeIntensity = q->aoVolumeIntensity > 0.0f ? q->aoVolumeIntensity : 1.0f;
    }
    MS_SetRenderSettings(s, rs);
    if (s->priv->gpu && R3D_GetAntiAliasingMode() != q->antiAliasing) R3D_SetAntiAliasingMode(q->antiAliasing);

    // The environment: relative to the map's, which MS_ApplyEnvironment just wrote
    R3D_Environment *env = R3D_GetEnvironment();
    switch (q->ao) {
    case MS_AO_SSAO: env->ssil.enabled = false; break;
    case MS_AO_LITE: env->ssil.enabled = false; env->ssao.sampleCount = 8; break;
    case MS_AO_BAKED:
    case MS_AO_OFF: env->ssil.enabled = false; env->ssao.enabled = false; break;
    default: break;
    }
    if (q->ssaoSamples > 0) env->ssao.sampleCount = q->ssaoSamples;
    if (q->ssaoRadius > 0.0f) env->ssao.radius = q->ssaoRadius;
    if (q->ssaoIntensity > 0.0f) env->ssao.intensity = q->ssaoIntensity;
    if (q->ssilSamples > 0) env->ssil.sampleCount = q->ssilSamples;
    if (q->ssilRadius > 0.0f) env->ssil.radius = q->ssilRadius;
    if (!q->volumetricFog) env->volumetricFog.enabled = false;
    if (q->fogStepScale > 0.0f && q->fogStepScale != 1.0f) env->volumetricFog.stepSize *= q->fogStepScale;
    if (q->fogLengthScale > 0.0f && q->fogLengthScale != 1.0f) env->volumetricFog.length *= q->fogLengthScale;
    if (!q->distanceFog) env->fog.mode = R3D_FOG_DISABLED;
    // Without bloom the map's env bloom.offExposure keeps the mean brightness (bloom with
    // threshold 0 is mostly a uniform lift; default 1.19, matched on the default map)
    if (!q->bloom && env->bloom.mode != R3D_BLOOM_DISABLED) {
        env->bloom.mode = R3D_BLOOM_DISABLED;
        env->tonemap.exposure *= s->priv->d ? s->priv->d->env.bloomOffExposure : 1.19f;
    }
    if (q->bloomIntensityScale != 1.0f) env->bloom.intensity *= q->bloomIntensityScale;
    if (q->ssr) env->ssr.enabled = q->ssr > 0;
    if (q->ssgi) env->ssgi.enabled = q->ssgi > 0;
    if (q->exposure != 1.0f) env->tonemap.exposure *= q->exposure;
    if (q->brightness != 1.0f) env->color.brightness *= q->brightness;
    if (q->contrast != 1.0f) env->color.contrast *= q->contrast;
    if (q->saturation != 1.0f) env->color.saturation *= q->saturation;
}

static R3D_ShadowMap SunShadowMap(const MS_Private *p, int slot)
{
    R3D_ShadowMap m = p->sunShadow[slot];
    m.softness = p->rs.shadowSoftness > 0.0f ? p->rs.shadowSoftness : p->d->env.sun.softness;
    m.depthBias = p->d->env.sun.depthBias;
    m.slopeBias = p->d->env.sun.slopeBias;
    // The map's biases are tuned for its shadow.size. A smaller map (a quality preset, an override)
    // has larger texels and so larger depth steps across a texel on sloped receivers: scale
    // both biases with the texel size, or low sun angles show acne stripes on flat tops.
    int size = R3D_GetHint(R3D_HINT_SHADOW_DIR_SIZE);
    if (size > 0 && size < p->d->env.sun.fileShadowSize) {
        float k = (float)p->d->env.sun.fileShadowSize / (float)size;
        m.depthBias *= k;
        m.slopeBias *= k;
    }
    return m;
}

// The sun is pushed (and its fog visibility baked)
static bool SunOn(const MS_Private *p)
{
#ifdef MONOLITH_EDITOR
    if (!p->rs.sunOn) return false;     // the editor's sun toggle
#endif
    return p->d->env.sun.enabled;
}

bool MS_PushLights(MS_Scene *s, Camera3D camera, float aspect)
{
    MS_Private *p = s->priv;
    SceneData *d = p->d;
    bool update = false;
    // The first frames after a load compile the shader variants they draw with (some only in the
    // second frame): the first two are logged, each from this call to the next one (the whole
    // frame, its present included)
    double now = GetTime();
    if (p->frameVersion != s->version) {
        p->frameVersion = s->version;
        p->firstFrames = 0;
    } else if (p->firstFrames < 2) {
        p->firstFrameMs[p->firstFrames++] = (now - p->frameStart) * 1000.0;
        if (p->firstFrames == 2)
            TraceLog(LOG_INFO, "monolith_scene: first frames after the load %.1f + %.1f ms", p->firstFrameMs[0], p->firstFrameMs[1]);
    }
    p->frameStart = now;
    if (SunOn(p)) {
        R3D_Light sun = d->sunLight;
        // (the tiles and the camera-fitted cache key on the range, so a new one redraws them)
        if (p->rs.shadowRange > 0.0f) sun.range = p->rs.shadowRange;
        int mapSize = R3D_GetHint(R3D_HINT_SHADOW_DIR_SIZE);
        if (!d->env.sun.shadow || !p->rs.sunShadow) R3D_PushLight(sun);
#ifdef MS_PATCHED_R3D
        else if (p->rs.shadowMode == MS_SHADOW_TILES) {
            // Static tiles: drawn once per map load (per tile), chosen by eye position only
            int tile = SunShadowTileSelect(&p->tiles, sun.direction, sun.range, mapSize, ShadowHome(d),
                                           camera.position, s->version, &update);
            if (update) TraceLog(LOG_INFO, "monolith_scene: sun shadow tile %d drawn (%d so far)", tile, p->tiles.renders);
            R3D_PushLightFixedShadow(sun, SunShadowMap(p, tile), p->tiles.tile[tile].viewProj, update);
        }
#endif
        else {
            // Only re-render the shadow map when the cached one no longer covers the view
            update = p->rs.shadowMode == MS_SHADOW_ALWAYS ||
                SunShadowNeedsUpdate(&p->camCache, sun.direction, sun.range, camera, aspect, mapSize, s->version);
            R3D_PushLightEx(sun, SunShadowMap(p, 0), update);
        }
    }
    for (int i = 0; i < d->lightCount; i++) {
        const MS_LightDef *L = &d->lights[i];
        R3D_Light light = L->light;
#ifdef MS_PATCHED_R3D
        // Clip box (patch 16): keeps an unshadowed light out of the room below / beside it
        light.clip = L->clip;
        light.clipMin = L->clipBox.min;
        light.clipMax = L->clipBox.max;
#endif
        if (!L->shadow || !d->lightShadow[i].handle) { R3D_PushLight(light); continue; }
        // Static scene: point / spot shadows are drawn once; extra directional lights follow the camera
        bool draw = !d->lightShadowDrawn[i] || L->light.type == R3D_LIGHT_DIR;
        R3D_PushLightEx(light, d->lightShadow[i], draw);
        d->lightShadowDrawn[i] = true;
    }
    return update;
}

void MS_DrawGeometry(MS_Scene *s)
{
    MS_Private *p = s->priv;
    SceneData *d = p->d;
    // One draw per cell and material. Instanced ones are clusters: R3D tests a cluster against
    // each view's frustum (the camera, the shadow maps) and skips its draw when outside
    for (int r = 0; r < d->boxRangeCount; r++) {
        const DrawRange *R = &d->boxRanges[r];
        const R3D_Material mat = d->materials[R->group].material;
        if (!d->matDefs[R->group].merge) {
            R3D_BeginCluster(R->bounds);
            R3D_DrawMeshInstancedEx(p->cube, mat, d->inst[R->group], R->first, R->count);
            R3D_EndCluster();
        } else if (R->mesh.vertexCount > 0) {
            // A merged range is its mesh drawn as one instance at the origin: it stays in R3D's
            // instanced draw list, sorted like the cubes were (where boxes overlap exactly, the
            // draw order decides which one shows), and the cluster culls it the same way
            R3D_BeginCluster(R->bounds);
            R3D_DrawMeshInstancedEx(R->mesh, mat, d->unit, 0, 1);
            R3D_EndCluster();
        }
    }
    // Models: the same per (file, materials, shadow) group; they go into the sun's
    // shadow tiles too, which are drawn once per map load like for the boxes
    for (int r = 0; r < d->modelRangeCount; r++) {
        const DrawRange *R = &d->modelRanges[r];
        const ModelGroup *G = &d->modelGroups[R->group];
        R3D_BeginCluster(R->bounds);
        R3D_DrawModelInstancedEx(G->model, G->inst, R->first, R->count);
        R3D_EndCluster();
    }
#ifdef MS_PATCHED_R3D
    // Occlusion volume: once per map load (CPU bake + upload; not a pass of this frame)
    if (p->rs.aoVolumeVoxel > 0.0f && p->aoVersion != s->version) {
        if (!BakeOcclusionVolume(d, p->rs.aoVolumeVoxel, p->rs.aoVolumeIntensity)) R3D_ClearWorldOcclusion();
        p->aoVersion = s->version;
    }
    if (SunOn(p) && d->env.sun.shadow && p->rs.sunShadow && p->rs.fogCacheVoxel > 0.0f && p->fogCacheVersion != s->version) {
        // Fog sun visibility, baked once per map load into the shadow layer this frame
        // doesn't sample (that tile slot is forgotten: it holds the bake map now)
        bool tiles = p->rs.shadowMode == MS_SHADOW_TILES;
        int slot = (tiles && p->tiles.current == 1) ? 0 : 1;
        if (tiles) {
            p->tiles.tile[slot].valid = false;
            if (p->tiles.current == slot) p->tiles.current = -1;
        }
        BoundingBox region = FogCacheRegion(d, p->rs.fogCacheVoxel);
        R3D_BakeFogSunVisibility(SunShadowMap(p, slot), SunShadowBoxMatrix(d->sunLight.direction, region, 2.0f),
                                 region, p->rs.fogCacheVoxel, p->rs.fogCacheSub);
        p->fogCacheVersion = s->version;
    }
#endif
}

bool MS_Draw(MS_Scene *s, Camera3D camera, RenderTexture target)
{
    float aspect = target.id ? (float)target.texture.width / target.texture.height
                             : (float)GetRenderWidth() / GetRenderHeight();
    R3D_BeginPro((R3D_View){ .camera = R3D_CameraFromRL(camera), .target = target });
        bool update = MS_PushLights(s, camera, aspect);
        MS_DrawGeometry(s);
    R3D_End();
    return update;
}

// #endregion

// ======================================================================================
// #region Public API: queries
// ======================================================================================

static bool RayBox(Vector3 o, Vector3 d, Vector3 mn, Vector3 mx, float *tNear, float *tFar, int *axis);

MS_Hit MS_Raycast(const MS_Scene *s, Vector3 origin, Vector3 dir, float maxDist)
{
    MS_Hit best = { .obj = { MS_OBJ_NONE, -1 }, .dist = INFINITY };
    const float o[3] = { origin.x, origin.y, origin.z };
    const float d[3] = { dir.x, dir.y, dir.z };
    float limit = maxDist > 0.0f ? maxDist : INFINITY;

    // The boxes, through the colliders' BVH: a node's slab interval (the same arithmetic) holds
    // those of its boxes, so a node is skipped only when none of its boxes can be the hit. The
    // order of the visits doesn't matter: the nearest box wins, on equal distances the lower index.
    const SceneData *sd = s->priv->d;
    int stack[128], sp = 0;
    if (sd->colliderNodeCount > 0) stack[sp++] = 0;
    while (sp > 0) {
        const BvhNode *nd = &sd->colliderNodes[stack[--sp]];
        float nodeNear, nodeFar;
        if (!RayBox(origin, dir, nd->mn, nd->mx, &nodeNear, &nodeFar, NULL) || nodeFar < 0.0f ||
            nodeNear > best.dist || nodeNear > limit) continue;
        if (nd->count == 0) {
            if (sp + 2 <= 128) { stack[sp++] = nd->left; stack[sp++] = nd->left + 1; }
            continue;
        }
        for (int k = nd->first; k < nd->first + nd->count; k++) {
            int i = sd->colliderOrder[k];
            if (i >= s->boxCount) continue;     // a model collider: models are hit by their triangles below
            const BoundingBox *b = &s->boxes[i];
            const float mn[3] = { b->min.x, b->min.y, b->min.z };
            const float mx[3] = { b->max.x, b->max.y, b->max.z };
            float tNear = -INFINITY, tFar = INFINITY;
            int axis = -1;
            bool miss = false;
            for (int a = 0; a < 3 && !miss; a++) {
                if (fabsf(d[a]) < 1e-12f) {
                    // Parallel to this slab: inside it or never
                    if (o[a] < mn[a] || o[a] > mx[a]) miss = true;
                    continue;
                }
                float t1 = (mn[a] - o[a]) / d[a], t2 = (mx[a] - o[a]) / d[a];
                if (t1 > t2) { float t = t1; t1 = t2; t2 = t; }
                if (t1 > tNear) { tNear = t1; axis = a; }
                if (t2 < tFar) tFar = t2;
                if (tNear > tFar) miss = true;
            }
            // tNear < 0: the origin is inside this box. Skip it, so flying through a wall
            // still lets you pick what is beyond.
            if (miss || axis < 0 || tNear < 0.0f || tNear > best.dist || tNear > limit) continue;
            if (tNear == best.dist && i > best.obj.index) continue;

            best.obj = (MS_ObjRef){ MS_OBJ_BOX, i };
            best.dist = tNear;
            best.point = Vector3Add(origin, Vector3Scale(dir, tNear));
            float n[3] = { 0, 0, 0 };
            n[axis] = d[axis] > 0.0f ? -1.0f : 1.0f;
            best.normal = (Vector3){ n[0], n[1], n[2] };
        }
    }
    // Models: only strictly nearer than the best box (a box wins a tie)
    for (int i = 0; i < s->modelCount; i++) {
        MS_Hit h;
        float lim = best.obj.kind != MS_OBJ_NONE ? best.dist : (maxDist > 0.0f ? maxDist : 0.0f);
        if (MS_RaycastModel(s, i, origin, dir, lim, &h) && h.dist < best.dist) best = h;
    }
    if (best.obj.kind == MS_OBJ_NONE) best.dist = 0.0f;
    return best;
}

// Slab test; tNear < 0 when the origin is inside. False on a miss.
static bool RayBox(Vector3 o, Vector3 d, Vector3 mn, Vector3 mx, float *tNear, float *tFar, int *axis)
{
    const float O[3] = { o.x, o.y, o.z }, D[3] = { d.x, d.y, d.z };
    const float MN[3] = { mn.x, mn.y, mn.z }, MX[3] = { mx.x, mx.y, mx.z };
    float t0 = -INFINITY, t1 = INFINITY;
    int ax = -1;
    for (int a = 0; a < 3; a++) {
        if (fabsf(D[a]) < 1e-12f) {
            if (O[a] < MN[a] || O[a] > MX[a]) return false;
            continue;
        }
        float a1 = (MN[a] - O[a]) / D[a], a2 = (MX[a] - O[a]) / D[a];
        if (a1 > a2) { float t = a1; a1 = a2; a2 = t; }
        if (a1 > t0) { t0 = a1; ax = a; }
        if (a2 < t1) t1 = a2;
        if (t0 > t1) return false;
    }
    *tNear = t0;
    *tFar = t1;
    if (axis) *axis = ax;
    return ax >= 0 || t0 <= t1;
}

bool MS_RaycastModel(const MS_Scene *s, int mi, Vector3 origin, Vector3 dir, float maxDist, MS_Hit *hit)
{
    if (mi < 0 || mi >= s->modelCount) return false;
    const SceneData *d = s->priv->d;
    const MS_ModelInstance *M = &s->models[mi];
    float limit = maxDist > 0.0f ? maxDist : INFINITY;
    float tn, tf;
    int axis;
    if (!RayBox(origin, dir, M->bounds.min, M->bounds.max, &tn, &tf, &axis) || tf < 0.0f || tn > limit) return false;

    const ModelPick *mp = &d->modelPick[M->file];
    if (mp->triCount == 0) {
        // No triangles (noGpu scene, or a file without any): its bounds, like a box
        if (axis < 0 || tn < 0.0f) return false;
        float n[3] = { 0, 0, 0 };
        n[axis] = ((const float *)&dir)[axis] > 0.0f ? -1.0f : 1.0f;
        *hit = (MS_Hit){ { MS_OBJ_MODEL, mi }, Vector3Add(origin, Vector3Scale(dir, tn)), { n[0], n[1], n[2] }, tn };
        return true;
    }

    // Into model space: the same ray parameter t (affine map), so distances stay world distances
    Matrix inv = MatrixInvert(M->transform);
    Vector3 o = Vector3Transform(origin, inv);
    Vector3 dd = { inv.m0 * dir.x + inv.m4 * dir.y + inv.m8 * dir.z, inv.m1 * dir.x + inv.m5 * dir.y + inv.m9 * dir.z,
                   inv.m2 * dir.x + inv.m6 * dir.y + inv.m10 * dir.z };
    float best = limit;
    int bestTri = -1;
    int stack[128], sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const BvhNode *nd = &mp->nodes[stack[--sp]];
        float a, b;
        if (!RayBox(o, dd, nd->mn, nd->mx, &a, &b, NULL) || b < 0.0f || a > best) continue;
        if (nd->count == 0) {
            if (sp + 2 <= 128) { stack[sp++] = nd->left; stack[sp++] = nd->left + 1; }
            continue;
        }
        for (int t = nd->first; t < nd->first + nd->count; t++) {
            // Moeller-Trumbore, both faces
            Vector3 v0 = mp->pos[mp->tri[3 * t]], v1 = mp->pos[mp->tri[3 * t + 1]], v2 = mp->pos[mp->tri[3 * t + 2]];
            Vector3 e1 = Vector3Subtract(v1, v0), e2 = Vector3Subtract(v2, v0);
            Vector3 pv = Vector3CrossProduct(dd, e2);
            float det = Vector3DotProduct(e1, pv);
            if (fabsf(det) < 1e-20f) continue;
            float inv_det = 1.0f / det;
            Vector3 tv = Vector3Subtract(o, v0);
            float u = Vector3DotProduct(tv, pv) * inv_det;
            if (u < 0.0f || u > 1.0f) continue;
            Vector3 qv = Vector3CrossProduct(tv, e1);
            float v = Vector3DotProduct(dd, qv) * inv_det;
            if (v < 0.0f || u + v > 1.0f) continue;
            float tt = Vector3DotProduct(e2, qv) * inv_det;
            if (tt > 1e-5f && tt < best) { best = tt; bestTri = t; }
        }
    }
    if (bestTri < 0) return false;
    Vector3 v0 = mp->pos[mp->tri[3 * bestTri]], v1 = mp->pos[mp->tri[3 * bestTri + 1]], v2 = mp->pos[mp->tri[3 * bestTri + 2]];
    Vector3 nl = Vector3CrossProduct(Vector3Subtract(v1, v0), Vector3Subtract(v2, v0));
    // Normals transform with the inverse transpose
    Vector3 n = { inv.m0 * nl.x + inv.m1 * nl.y + inv.m2 * nl.z, inv.m4 * nl.x + inv.m5 * nl.y + inv.m6 * nl.z,
                  inv.m8 * nl.x + inv.m9 * nl.y + inv.m10 * nl.z };
    n = Vector3Normalize(n);
    if (Vector3DotProduct(n, dir) > 0.0f) n = Vector3Negate(n);
    *hit = (MS_Hit){ { MS_OBJ_MODEL, mi }, Vector3Add(origin, Vector3Scale(dir, best)), n, best };
    return true;
}

static inline bool Overlaps(BoundingBox a, Vector3 mn, Vector3 mx)
{
    return a.min.x < mx.x && a.max.x > mn.x && a.min.y < mx.y && a.max.y > mn.y && a.min.z < mx.z && a.max.z > mn.z;
}

int MS_FirstCollision(const MS_Scene *s, BoundingBox a)
{
    // Through the colliders' BVH (a node strictly overlaps `a` whenever one of its colliders does)
    const SceneData *d = s->priv->d;
    int first = -1;
    int stack[128], sp = 0;
    if (d->colliderNodeCount > 0) stack[sp++] = 0;
    while (sp > 0) {
        const BvhNode *nd = &d->colliderNodes[stack[--sp]];
        if (!Overlaps(a, nd->mn, nd->mx)) continue;
        if (nd->count == 0) {
            if (sp + 2 <= 128) { stack[sp++] = nd->left; stack[sp++] = nd->left + 1; }
            continue;
        }
        for (int k = nd->first; k < nd->first + nd->count; k++) {
            int i = d->colliderOrder[k];
            if ((first < 0 || i < first) && Overlaps(a, s->colliders[i].min, s->colliders[i].max)) first = i;
        }
    }
    return first;
}

bool MS_Collides(const MS_Scene *s, BoundingBox a)
{
    return MS_FirstCollision(s, a) >= 0;
}

const MS_Ref *MS_ObjectRef(const MS_Scene *s, MS_ObjRef obj)
{
    if (obj.kind == MS_OBJ_BOX && obj.index >= 0 && obj.index < s->boxCount) return &s->boxRefs[obj.index];
    if (obj.kind == MS_OBJ_MODEL && obj.index >= 0 && obj.index < s->modelCount) return &s->models[obj.index].ref;
    if (obj.kind == MS_OBJ_LIGHT && obj.index >= 0 && obj.index < s->lightCount) return &s->lights[obj.index].ref;
    return NULL;
}

BoundingBox MS_ObjectBounds(const MS_Scene *s, MS_ObjRef obj)
{
    if (obj.kind == MS_OBJ_BOX && obj.index >= 0 && obj.index < s->boxCount) return s->boxes[obj.index];
    if (obj.kind == MS_OBJ_MODEL && obj.index >= 0 && obj.index < s->modelCount) return s->models[obj.index].bounds;
    if (obj.kind == MS_OBJ_LIGHT && obj.index >= 0 && obj.index < s->lightCount)
        return (BoundingBox){ s->lights[obj.index].light.position, s->lights[obj.index].light.position };
    return (BoundingBox){ 0 };
}

int MS_ObjectEntity(const MS_Scene *s, MS_ObjRef obj)
{
    if (obj.kind == MS_OBJ_BOX && obj.index >= 0 && obj.index < s->boxCount) return s->boxEntities[obj.index];
    if (obj.kind == MS_OBJ_MODEL && obj.index >= 0 && obj.index < s->modelCount) return s->modelEntities[obj.index];
    if (obj.kind == MS_OBJ_LIGHT && obj.index >= 0 && obj.index < s->lightCount) return s->lightEntities[obj.index];
    return -1;
}

int MS_FindEntity(const MS_Scene *s, MS_EntityId id)
{
    const SceneData *d = s->priv->d;
    if (!d || !d->entityHashCap) return -1;
    for (unsigned int h = EntityHash(id, d->entityHashCap); d->entityHash[h]; h = (h + 1) & (unsigned int)(d->entityHashCap - 1)) {
        int e = d->entityHash[h] - 1;
        if (d->entityIds[e].stmt == id.stmt && d->entityIds[e].index == id.index) return e;
    }
    return -1;
}

// #endregion

#ifdef MONOLITH_EDITOR
// ======================================================================================
// #region Public API: writing the environment (editor)
// ======================================================================================

static void WriteKey(FILE *f, const char *prefix, const char *key, int type, const void *p)
{
    char v[256];
    FormatValue(v, sizeof(v), type, p);
    fprintf(f, "env %s%s %s\n", prefix, key, v);
}

// DEF_KEYS starting with `prefix` (optional ones only when set)
static void WriteDefKeys(FILE *f, const EnvDef *e, const char *prefix)
{
    for (int i = 0; i < COUNT(DEF_KEYS); i++) {
        const KeyDef *k = &DEF_KEYS[i];
        if (strncmp(k->key, prefix, strlen(prefix)) != 0) continue;
        if (strcmp(k->key, "sky.panorama") == 0 && e->skyMode != SKY_PANORAMA) continue;
        if (strcmp(k->key, "shadow.home") == 0 && !e->homeSet) continue;
        if (strcmp(k->key, "fogCache.region") == 0 && !e->fogRegionSet) continue;
        if (strcmp(k->key, "volumetricFog.fadeNear") == 0) { if (!(e->vfogFadeNear > 0.0f)) continue; }
        else if (strncmp(k->key, "volumetricFog.fade", 18) == 0 && !(e->vfogFadeEnd > 0.0f)) continue;
        WriteKey(f, "", k->key, k->type, (const char *)e + k->off);
    }
}

bool MS_WriteEnvironment(const MS_Scene *s, FILE *f)
{
    static EnvDef builtin;
    const EnvDef *e;
    if (s) e = &s->priv->d->env;
    else {
        BuiltinEnvironment(&builtin);
        e = &builtin;
    }
    fprintf(f, "# Sky (sky.sunDirection follows sun.direction unless it is set)\n");
    WriteDefKeys(f, e, "sky.");
    for (int j = 0; j < COUNT(SKY_KEYS); j++)
        if (strcmp(SKY_KEYS[j].key, "sunDirection") != 0 || e->skySunDirSet)
            WriteKey(f, "sky.", SKY_KEYS[j].key, SKY_KEYS[j].type, (const char *)&e->sky + SKY_KEYS[j].off);
    fprintf(f, "# Ambient light (IBL): a copy of the sky with these keys changed\n");
    WriteDefKeys(f, e, "ambientSky.");
    for (int j = 0; j < COUNT(SKY_KEYS); j++)
        if (e->ambientSet & (1u << j))
            WriteKey(f, "ambientSky.", SKY_KEYS[j].key, SKY_KEYS[j].type, (const char *)&e->ambient + SKY_KEYS[j].off);
    fprintf(f, "# Sun: the shadowed directional light (static shadow tiles)\n");
    WriteDefKeys(f, e, "sun.");
    WriteDefKeys(f, e, "shadow.");
    WriteDefKeys(f, e, "fogCache.");
    fprintf(f, "# R3D environment\n");
    for (int i = 0; i < COUNT(ENV_KEYS); i++)
        WriteKey(f, "", ENV_KEYS[i].key, ENV_KEYS[i].type, (const char *)&e->env + ENV_KEYS[i].off);
    WriteDefKeys(f, e, "volumetricFog.");   // the density fade, only when the map sets one
    fprintf(f, "# Quality presets: exposure factor when a preset turns bloom off (keeps the mean brightness)\n");
    WriteDefKeys(f, e, "bloom.");
    return !ferror(f);
}

// #endregion

// ======================================================================================
// #region Public API: package files, R3D environment export (editor: --export, capture shim)
// ======================================================================================

int MS_DependencyCount(const MS_Scene *s)
{
    return s && s->priv && s->priv->d ? s->priv->d->depCount : 0;
}

const char *MS_Dependency(const MS_Scene *s, int index)
{
    if (index < 0 || index >= MS_DependencyCount(s)) return NULL;
    return s->priv->d->deps[index];
}

bool MS_WriteR3DEnvironment(const R3D_Environment *env, FILE *f)
{
    for (int i = 0; i < COUNT(ENV_KEYS); i++)
        WriteKey(f, "", ENV_KEYS[i].key, ENV_KEYS[i].type, (const char *)env + ENV_KEYS[i].off);
    return !ferror(f);
}

// #endregion
#endif
