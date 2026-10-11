// monolith_scene -- map packages: parse, hot-reload, draw, collide, pick (see monolith_scene.h)
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
//             inner deg, outer deg (spot cone), shadow on|off (default off)
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
//       material.N "name"             slot N only (0-based; --inspect-model lists the slots)
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
//     package; they are watched for hot reload like the model file.
//
// Placement prefixes, written before a primitive on the same line:
//   repeat n dx dy dz          [1] n copies, copy i (0-based) shifted by i * (dx, dy, dz); up to 3
//                                  repeats on one line give a grid (the first is the outer loop,
//                                  so it decides the box order)
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
//   keys: shader "shaders/x.glsl"  R3D surface shader (one instance per material)
//         uniform <name> f [f f f] float / vec2 / vec3 / vec4 uniform of that shader
//         sampler <name> "textures/x.png"   sampler2D uniform of that shader
//         albedo R G B [A]  albedo_map "p"  normal_map "p"  normal_scale f  orm_map "p"
//         occlusion f  roughness f  metalness f  specular f
//         emission R G B  emission_energy f  emission_map "p"
//         uv_scale u v  uv_offset u v  alpha_cutoff f (0 = never discards; allows the depth
//         pre-pass)  cull back|front|none  transparency off|prepass|alpha
//         blend alpha|additive|add_alpha|multiply|premultiplied  unlit on|off  priority n
//         inherit albedo|normal|orm|emission|all   (model overrides: keep the model's own map)
//         wrap repeat|clamp|mirror   wrapping of this material's textures and samplers
//                                    (default repeat, so uv_scale > 1 tiles; trilinear + mipmaps)
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
//   MS_SetInitHints).
//   Cache hints (optional; derived from the map bounds without them, see "Caches"):
//   shadow.home x y z, fogCache.region x0 y0 z0 x1 y1 z1.
//   Every key has a built-in default (BuiltinEnvironment(): the values the default map was
//   built with), so a v1 map renders as before apart from its material. Numeric values
//   outside sane ranges (ENV_RANGES, e.g. volumetricFog.stepSize >= 0.05) and zero
//   directions are rejected.
//
// Player and cameras (format 2):
//   spawn x y z yawDeg [pitchDeg]          player start, feet position (default 0 0 0, yaw 0)
//   viewpoint "Name" ex ey ez tx ty tz     spectator viewpoint (eye, target), in order 1..n;
//                                          without any, one viewpoint is made from the spawn
//
// Paths are relative to the package root (the folder of the map file). Absolute paths and
// ".." components are rejected, so a package can be copied anywhere.
// Limits: none on content (any number of objects, groups, materials, lights, viewpoints; lines
// and names of any length). Sanity caps against runaway input: MS_MAX_OBJECTS (1000000 boxes +
// models + lights; a `repeat` grid multiplying to more fails), MS_MAX_FILE_BYTES (4 MB file).
// Every box remembers the map line of the statement that made it (boxRefs[]).
// Errors are reported as "file:line: message" and the previous scene is kept.
//
// ======================================================================================

#include "monolith_scene.h"
#include <r3d/r3d_texture.h>
#include <raymath.h>
#include <rlgl.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MS_FILE_MAX         MS_MAX_FILE_BYTES
#define MS_REPEATS_MAX      3       // `repeat` prefixes per line (the grammar: a 3D grid at most)
#define MS_POLL_INTERVAL    0.5f
// After an mtime change, keep re-reading for this long: GetFileModTime has 1 s resolution,
// and editors that truncate + write (or rename) can leave an empty or partial file for a
// moment, then finish within the same second (same mtime).
#define MS_SETTLE_TIME      2.0f
#define SUN_TILE_COUNT      2       // = the 2 layers R3D allocates for directional shadows anyway
// --fog-cache without a region hint: at most this many voxels (bytes of VRAM)
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

// Memory rule (docs/design-v2.md 2.7): everything sized by the map is a heap array grown with
// MS_RESERVE / MS_PUSH and every name or path stored per element is a heap string; all of it
// belongs to the SceneData of one load and is freed with it (FreeData). Fixed char arrays are
// left only for one-off values bounded by the loader (EnvDef.skyPanorama: a package path,
// <= MS_PATH_MAX), the last error message and stack scratch for formatting one path or message.
// Allocation happens while parsing / uploading a load and in MS_Poll when a file changed;
// drawing, picking and collision never allocate.

typedef struct { char *name; int count; float v[4]; } MatUniform;
typedef struct { char *name; char *path; int line; } MatSampler;

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
} MatDef;

typedef struct {
    bool enabled, shadow;
    Vector3 direction;                  // as written; the light gets it normalized
    Color color;
    float energy, specular, fogEnergy, range, softness, depthBias, slopeBias;
    int shadowSize;
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
    bool homeSet, fogRegionSet;
    Vector3 home;
    BoundingBox fogRegion;
} EnvDef;

typedef struct { char *path; bool isColor; int wrap; Texture2D tex; } TexEntry;

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

// One parsed (and possibly uploaded) map. A reload builds a new one and swaps it in.
typedef struct {
    int formatVersion;
    char *path, *root, *brief;          // brief: "" when the package has no MAP.md
    unsigned long long hash;            // FNV-1a 64 of the map file
    long mtime;
    char *text;                         // the map file, for MS_GetLine
    int textSize;

    // Heap arrays: count = used, cap = allocated (MS_RESERVE / MS_PUSH)
    BoundingBox *boxes;
    MS_Ref *boxRefs;
    int boxCount, boxCap, boxRefCap;
    char **groups;
    int groupCount, groupCap;
    MS_Material *materials;
    MatDef *matDefs;                    // parallel to materials[]
    int materialCount, materialCap, matDefCap;
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

    char **deps;                        // files it depends on; [0] = the map file
    int depCount, depCap;

    // GPU (when uploaded)
    bool gpu;
    R3D_InstanceBuffer *inst;           // per material (materialCount)
    int *instCount;
    TexEntry *textures;
    int textureCount, textureCap;
    R3D_Cubemap sky;
    R3D_AmbientMap ambientMap;
    R3D_ShadowMap *lightShadow;         // per light (lightCount)
    bool *lightShadowDrawn;
    ModelGroup *modelGroups;
    int modelGroupCount;
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
    char error[512];

    // Hot reload: the files of the last load attempt and their mtimes, the combined hash
    // of the last attempt's contents
    char **watch;
    long *watchMtime;
    int watchCount, watchCap, watchMtimeCap;
    float pollTimer, settleTime;
    unsigned long long triedHash;

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

// Reads a whole file (malloc'd, NUL-terminated). NULL with a message in err on failure.
static char *ReadWholeFile(const char *path, int *size, char *err, int errSize)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errSize, "%s: cannot open file", path); return NULL; }
    char *buf = NULL;
    long len = -1;
    if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
    if (len < 0 || len > MS_FILE_MAX || fseek(f, 0, SEEK_SET) != 0)
        snprintf(err, errSize, "%s: cannot read file (or larger than 4 MB)", path);
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

// --map accepts a package folder or a map file (docs/design-v2.md section 1)
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
    char cand[MS_PATH_MAX + 80];
    snprintf(cand, sizeof(cand), "%s/%s.map", dir, base);
    if (FileExists(cand)) {
        if (snprintf(out, outSize, "%s", cand) < outSize) return true;
        snprintf(err, errSize, "%s: path too long", dir);
        return false;
    }
    FilePathList list = LoadDirectoryFilesEx(dir, ".map", false);
    bool ok = list.count == 1;
    if (ok) snprintf(out, outSize, "%s", list.paths[0]);
    else {
        int n = snprintf(err, errSize, "%s: no %s.map in the package folder and %s", dir, base,
                         list.count ? "more than one other .map file:" : "no other .map file");
        for (unsigned i = 0; i < list.count && n > 0 && n < errSize; i++)
            n += snprintf(err + n, errSize - n, " %s", GetFileName(list.paths[i]));
    }
    UnloadDirectoryFiles(list);
    return ok;
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

static const char *const FOG_NAMES[] = { "disabled", "linear", "exp2", "exp" };
static const char *const BLOOM_NAMES[] = { "disabled", "mix", "additive", "screen" };
static const char *const TONEMAP_NAMES[] = { "linear", "reinhard", "filmic", "aces", "agx" };
static const char *const DOF_NAMES[] = { "disabled", "enabled" };
static const char *const SKYMODE_NAMES[] = { "procedural", "color", "panorama" };
static const char *const AMBMODE_NAMES[] = { "procedural", "same", "off" };

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

// The built-in environment (docs/design-v2.md 2.6): the look the default map was built
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
    // (PROMPT-INSTRUCTIONS.md 7.1). The emission stands in for the missing sky light.
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
    // downward-facing surfaces got almost no ambient (PROMPT-INSTRUCTIONS.md 7.5).
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
}

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

// #endregion

// ======================================================================================
// #region Parser
// ======================================================================================

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
    bool bad = s[0] == '\0' || s[0] == '/' || s[0] == '\\' || (s[0] && s[1] == ':') || strchr(s, '\\');
    for (const char *c = s; *c && !bad; ) {
        const char *e = c;
        while (*e && *e != '/') e++;
        if (e - c == 2 && c[0] == '.' && c[1] == '.') bad = true;
        c = *e ? e + 1 : e;
    }
    if (bad) return Fail(P, "path must stay inside the package: \"%s\"", s);
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

// `repeat` / `at` prefixes of a line
typedef struct {
    float at[3];
    bool hasAt;
    int repN[MS_REPEATS_MAX];
    float repD[MS_REPEATS_MAX][3];
    int reps;
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
            pl->reps++;
            t += 5;
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

// Offset of copy (i0, i1, i2): (at + i*d1 + j*d2 + k*d3), in this order of float additions
static void CopyOffset(const Placement *pl, const int *idx, float off[3])
{
    for (int a = 0; a < 3; a++) {
        off[a] = pl->at[a];
        for (int r = 0; r < pl->reps; r++) off[a] += (float)idx[r] * pl->repD[r][a];
    }
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
        if (!NumF(P, args[1 + i], &v[i])) return false;
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
        } else {
            float *dst = strcmp(k, "energy") == 0 ? &L.energy : strcmp(k, "range") == 0 ? &L.range :
                         strcmp(k, "falloff") == 0 ? &L.falloff : strcmp(k, "specular") == 0 ? &L.specular :
                         strcmp(k, "fogEnergy") == 0 ? &L.fogEnergy : strcmp(k, "inner") == 0 ? &L.innerCutOff :
                         strcmp(k, "outer") == 0 ? &L.outerCutOff : NULL;
            if (!dst) return Fail(P, "unknown light key '%s' (color energy range falloff specular fogEnergy inner outer shadow)", k);
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
        R3D_Light c = L;
        if (shifted && nPos) c.position = (Vector3){ off[0] + pos.x, off[1] + pos.y, off[2] + pos.z };
        MS_LightDef *slot = MS_PUSH(d->lights, d->lightCount, d->lightCap);
        if (!slot) return Fail(P, "out of memory", NULL);
        *slot = (MS_LightDef){ c, shadow, P->cur };
    }
    return true;
}

static int FindMaterial(const SceneData *d, const char *name)
{
    for (int i = 0; i < d->materialCount; i++)
        if (strcmp(d->materials[i].name, name) == 0) return i;
    return -1;
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
    return mi;
}

// The built-in "default" material (docs/design-v2.md 2.2)
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
        } else if (strcmp(k, "sampler") == 0) {
            if (t + 2 >= n || v[0].quoted) return Fail(P, "sampler takes a name and a path: sampler u_tex \"textures/x.png\"", NULL);
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
};

static bool CheckEnvRange(Parser *P, const char *key, int type, const void *p)
{
    for (int i = 0; i < COUNT(ENV_RANGES); i++) {
        const EnvRange *r = &ENV_RANGES[i];
        if (strcmp(r->key, key) != 0) continue;
        float v = type == T_I ? (float)*(const int *)p : *(const float *)p;
        if (v > r->lo && v <= r->hi) return true;
        if (v == r->lo && !r->loOpen) return true;
        char m[160], lo[32], hi[32];
        FormatFloat(lo, sizeof(lo), r->lo);
        FormatFloat(hi, sizeof(hi), r->hi);
        snprintf(m, sizeof(m), "%s must be %s %s and <= %s", key, r->loOpen ? ">" : ">=", lo, hi);
        return Fail(P, "%s", m);
    }
    return true;
}

static bool ParseEnv(Parser *P, Token *tok, int n)
{
    if (n < 3 || tok[1].quoted) return Fail(P, "env takes a key and a value: env fog.density 0.01", NULL);
    const char *key = tok[1].s;
    EnvDef *e = &P->d->env;
    for (int i = 0; i < COUNT(ENV_KEYS); i++)
        if (strcmp(ENV_KEYS[i].key, key) == 0) {
            void *dst = (char *)&e->env + ENV_KEYS[i].off;
            return ParseValue(P, ENV_KEYS[i].type, &tok[2], n - 2, dst) && CheckEnvRange(P, key, ENV_KEYS[i].type, dst);
        }
    for (int i = 0; i < COUNT(DEF_KEYS); i++) {
        if (strcmp(DEF_KEYS[i].key, key) != 0) continue;
        if (!ParseValue(P, DEF_KEYS[i].type, &tok[2], n - 2, (char *)e + DEF_KEYS[i].off)) return false;
        if (strcmp(key, "shadow.home") == 0) e->homeSet = true;
        if (strcmp(key, "fogCache.region") == 0) e->fogRegionSet = true;
        if (strcmp(key, "shadow.size") == 0 && (e->sun.shadowSize < 256 || e->sun.shadowSize > 16384))
            return Fail(P, "shadow.size must be 256..16384", NULL);
        if ((strcmp(key, "sky.size") == 0 || strcmp(key, "ambientSky.size") == 0) &&
            (*(int *)((char *)e + DEF_KEYS[i].off) < 16 || *(int *)((char *)e + DEF_KEYS[i].off) > 4096))
            return Fail(P, "sky sizes must be 16..4096", NULL);
        if (strcmp(key, "sun.range") == 0 && !(e->sun.range > 0.0f)) return Fail(P, "sun.range must be > 0", NULL);
        if (strcmp(key, "sun.direction") == 0 && Vector3Length(e->sun.direction) == 0.0f)
            return Fail(P, "sun.direction must not be 0 0 0", NULL);
        return CheckEnvRange(P, key, DEF_KEYS[i].type, (char *)e + DEF_KEYS[i].off);
    }
    bool amb = strncmp(key, "ambientSky.", 11) == 0;
    if (amb || strncmp(key, "sky.", 4) == 0) {
        int k = SkyKeyIndex(key + (amb ? 11 : 4));
        if (k >= 0) {
            R3D_ProceduralSky *sky = amb ? &e->ambient : &e->sky;
            if (!ParseValue(P, SKY_KEYS[k].type, &tok[2], n - 2, (char *)sky + SKY_KEYS[k].off)) return false;
            if (SKY_KEYS[k].type == T_V3 && Vector3Length(*(Vector3 *)((char *)sky + SKY_KEYS[k].off)) == 0.0f)
                return Fail(P, "%s must not be 0 0 0", key);
            if (amb) e->ambientSet |= 1u << k;
            else if (strcmp(SKY_KEYS[k].key, "sunDirection") == 0) e->skySunDirSet = true;
            return true;
        }
    }
    return Fail(P, "unknown env key '%s' (see the header comment of monolith_scene.c)", key);
}

// --- Models (docs/design-v2.md 2.5)

// A file that a model file refers to (`ref`, as written in it, relative to the model's
// folder): must stay inside the package and exist; it becomes a dependency (hot reload)
static bool ModelRefDep(Parser *P, const char *modelRel, const char *ref, int refLen)
{
    char dec[MS_PATH_MAX];
    int n = 0;
    for (int i = 0; i < refLen && n < MS_PATH_MAX - 1; i++) {
        char c = ref[i];
        if (c == '\\' && i + 1 < refLen && ref[i + 1] == '/') continue;            // JSON "\/"
        if (c == '%' && i + 2 < refLen && isxdigit((unsigned char)ref[i + 1]) && isxdigit((unsigned char)ref[i + 2])) {
            char hex[3] = { ref[i + 1], ref[i + 2], 0 };
            c = (char)strtol(hex, NULL, 16);
            i += 2;
        }
        dec[n++] = c == '\\' ? '/' : c;
    }
    dec[n] = 0;
    if (n == 0 || strncmp(dec, "data:", 5) == 0) return true;                       // embedded
    char msg[MS_PATH_MAX + 200];
    if (strstr(dec, "://") || dec[0] == '/' || (dec[0] && dec[1] == ':')) {
        snprintf(msg, sizeof(msg), "\"%s\" refers to \"%s\", outside the package", modelRel, dec);
        return Fail(P, "%s", msg);
    }
    // modelRel's folder + dec, with "." and ".." resolved
    char joined[2 * MS_PATH_MAX], out[MS_PATH_MAX];
    const char *slash = strrchr(modelRel, '/');
    snprintf(joined, sizeof(joined), "%.*s%s", slash ? (int)(slash - modelRel + 1) : 0, modelRel, dec);
    int len = 0;
    for (char *c = joined; *c; ) {
        char *e = strchr(c, '/');
        int seg = e ? (int)(e - c) : (int)strlen(c);
        if (seg == 0 || (seg == 1 && c[0] == '.')) { /* skip */ }
        else if (seg == 2 && c[0] == '.' && c[1] == '.') {
            if (len == 0) {
                snprintf(msg, sizeof(msg), "\"%s\" refers to \"%s\", outside the package", modelRel, dec);
                return Fail(P, "%s", msg);
            }
            while (len > 0 && out[len - 1] != '/') len--;
            if (len > 0) len--;     // the '/' before the dropped segment
        } else {
            if (len + seg + 2 >= MS_PATH_MAX) return Fail(P, "path too long: \"%s\"", dec);
            if (len > 0) out[len++] = '/';
            memcpy(out + len, c, seg);
            len += seg;
        }
        c = e ? e + 1 : c + seg;
    }
    out[len] = 0;
    char full[MS_PATH_MAX];
    if (snprintf(full, sizeof(full), "%s%s", P->d->root, out) >= (int)sizeof(full)) return Fail(P, "path too long: \"%s\"", out);
    if (!AddDep(P, full)) return false;
    if (!FileExists(full)) {
        snprintf(msg, sizeof(msg), "file not found: \"%s\" (named by \"%s\")", out, modelRel);
        return Fail(P, "%s", msg);
    }
    return true;
}

// Every "uri": "..." in glTF JSON text
static bool GltfUriDeps(Parser *P, const char *modelRel, const char *json, size_t size)
{
    const char *end = json + size;
    for (const char *p = json; p + 5 < end; p++) {
        if (memcmp(p, "\"uri\"", 5) != 0) continue;
        const char *q = p + 5;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
        if (q >= end || *q != ':') continue;
        q++;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
        if (q >= end || *q != '"') continue;
        const char *s = ++q;
        while (q < end && *q != '"') q += (*q == '\\' && q + 1 < end) ? 2 : 1;
        if (q >= end) break;
        if (q - s < MS_PATH_MAX && !ModelRefDep(P, modelRel, s, (int)(q - s))) return false;
        p = q;
    }
    return true;
}

// The last whitespace-separated word of each line starting with one of `keys` (OBJ/MTL)
static bool ObjLineDeps(Parser *P, const char *modelRel, const char *text, const char *const *keys, int keyCount,
                        bool mtllib)
{
    for (const char *line = text; *line; ) {
        const char *eol = strchr(line, '\n');
        if (!eol) eol = line + strlen(line);
        const char *s = line;
        while (s < eol && (*s == ' ' || *s == '\t')) s++;
        for (int k = 0; k < keyCount; k++) {
            size_t kl = strlen(keys[k]);
            if ((size_t)(eol - s) <= kl || strncmp(s, keys[k], kl) != 0 || (s[kl] != ' ' && s[kl] != '\t')) continue;
            const char *a = s + kl, *b = eol;
            while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r')) b--;
            if (!mtllib) {          // options (-bm 1 ...) come first: the file name is the last word
                const char *w = b;
                while (w > a && w[-1] != ' ' && w[-1] != '\t') w--;
                a = w;
            } else while (a < b && (*a == ' ' || *a == '\t')) a++;
            if (b > a && b - a < MS_PATH_MAX) {
                if (!ModelRefDep(P, modelRel, a, (int)(b - a))) return false;
                if (mtllib) {
                    // the material library's textures, relative to the library's folder
                    char lib[MS_PATH_MAX], rel[2 * MS_PATH_MAX], full[3 * MS_PATH_MAX];
                    snprintf(lib, sizeof(lib), "%.*s", (int)(b - a), a);
                    const char *slash = strrchr(modelRel, '/');
                    snprintf(rel, sizeof(rel), "%.*s%s", slash ? (int)(slash - modelRel + 1) : 0, modelRel, lib);
                    snprintf(full, sizeof(full), "%s%s", P->d->root, rel);
                    int size;
                    char e[256];
                    char *mtl = ReadWholeFile(full, &size, e, sizeof(e));
                    if (mtl) {
                        static const char *const MAPS[] = { "map_Kd", "map_Ka", "map_Ks", "map_Ke", "map_Bump", "map_bump",
                                                            "bump", "map_d", "norm", "map_Pr", "map_Pm", "map_Ns", "disp" };
                        bool ok = ObjLineDeps(P, rel, mtl, MAPS, COUNT(MAPS), false);
                        free(mtl);
                        if (!ok) return false;
                    }
                }
            }
        }
        line = *eol ? eol + 1 : eol;
    }
    return true;
}

// The files a model file refers to: glTF buffers and images, OBJ material libraries and
// their textures (FBX/DAE: not followed)
static bool ModelDeps(Parser *P, const char *modelRel, const char *full)
{
    const char *ext = GetFileExtension(full);
    if (!ext) return true;
    if (TextIsEqual(TextToLower(ext), ".gltf") || TextIsEqual(TextToLower(ext), ".obj")) {
        bool gltf = TextIsEqual(TextToLower(ext), ".gltf");
        int size;
        char e[256];
        char *text = ReadWholeFile(full, &size, e, sizeof(e));
        if (!text) return Fail(P, "cannot read the model file \"%s\" (or larger than 4 MB)", modelRel);
        static const char *const OBJ_KEYS[] = { "mtllib" };
        bool ok = gltf ? GltfUriDeps(P, modelRel, text, (size_t)size) : ObjLineDeps(P, modelRel, text, OBJ_KEYS, 1, true);
        free(text);
        return ok;
    }
    if (TextIsEqual(TextToLower(ext), ".glb")) {
        // GLB: 12-byte header, then the JSON chunk (length, type "JSON", text)
        FILE *f = fopen(full, "rb");
        if (!f) return Fail(P, "cannot read the model file \"%s\"", modelRel);
        unsigned char h[20];
        bool ok = true;
        if (fread(h, 1, 20, f) == 20 && memcmp(h, "glTF", 4) == 0 && memcmp(h + 16, "JSON", 4) == 0) {
            uint32_t len = h[12] | (h[13] << 8) | (h[14] << 16) | ((uint32_t)h[15] << 24);
            char *json = len <= MS_FILE_MAX ? malloc(len) : NULL;
            if (json && fread(json, 1, len, f) == len) ok = GltfUriDeps(P, modelRel, json, len);
            free(json);
        }
        fclose(f);
        return ok;
    }
    return true;
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
        MS_ModelInstance c = M;
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
            if (!NumF(P, tok[i], &v[i - 1])) return false;
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
            if (!NumF(P, tok[2 + i], &v[i])) return false;
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
    P->cur = (MS_Ref){ 0, -1, -1, 0 };
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
        return Fail(P, P->d->formatVersion >= 2 ? "map is empty (no boxes and no models)" : "map has no boxes", NULL);
    return true;
}

// #endregion

// ======================================================================================
// #region Caches follow the map (docs/design-v2.md 4)
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

// The --fog-cache region: the hint, else the content bounds (+2 m), shrunk around the play
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
            lo[a] = (int)floorf((bmin[a] - AO_DIST_MAX - rmin[a]) / voxel - 0.5f);
            hi[a] = (int)ceilf((bmax[a] + AO_DIST_MAX - rmin[a]) / voxel - 0.5f);
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
        int key[3] = { (int)floorf((ls.x - origin.x) / cellXY), (int)floorf((ls.y - origin.y) / cellXY),
                       (int)floorf((ls.z - origin.z) / cellZ) };
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

static bool ParseScene(SceneData *d, const char *mapPath, const char *text, int size, const MS_LoadOptions *opt,
                       char *err, int errSize)
{
    char root[MS_PATH_MAX + 2], brief[MS_PATH_MAX + 16];
    RootOf(mapPath, root, sizeof(root));
    snprintf(brief, sizeof(brief), "%sMAP.md", root);
    d->path = MS_StrDup(mapPath);
    d->root = MS_StrDup(root);
    d->brief = MS_StrDup(FileExists(brief) ? brief : "");
    char **dep0 = MS_PUSH(d->deps, d->depCount, d->depCap);
    if (dep0) *dep0 = MS_StrDup(mapPath);
    d->formatVersion = 1;
    BuiltinEnvironment(&d->env);
    if (!d->path || !d->root || !d->brief || !dep0 || !*dep0 || !DefaultMaterial(d)) {
        snprintf(err, errSize, "%s: out of memory", mapPath);
        return false;
    }

    Parser P = { .d = d, .path = mapPath, .err = err, .errSize = errSize };
    bool ok = ParseText(&P, text, size);
    free(P.lineBuf);
    free(P.tok);
    if (ok && !FinishParse(d, opt)) {
        snprintf(err, errSize, "%s: out of memory", mapPath);
        ok = false;
    }
    return ok;
}

static Texture2D GetTexture(SceneData *d, const char *rel, bool isColor, int wrap, int line, char *err, int errSize)
{
    char full[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(full, sizeof(full), "%s%s", d->root, rel);
    for (int i = 0; i < d->textureCount; i++)
        if (strcmp(d->textures[i].path, full) == 0 && d->textures[i].isColor == isColor && d->textures[i].wrap == wrap) return d->textures[i].tex;
    char *path = MS_StrDup(full);
    if (!path || !MS_RESERVE(d->textures, d->textureCap, d->textureCount + 1)) {
        free(path);
        snprintf(err, errSize, "%s:%d: out of memory", d->path, line);
        return (Texture2D){ 0 };
    }
    // R3D's default wrap is clamp, which would make uv_scale > 1 useless; map materials repeat
    // unless `wrap clamp|mirror`. Filter: trilinear with generated mipmaps (R3D's default).
    static const TextureWrap WRAPS[] = { TEXTURE_WRAP_REPEAT, TEXTURE_WRAP_CLAMP, TEXTURE_WRAP_MIRROR_REPEAT };
    Texture2D tex = R3D_LoadTextureEx(full, WRAPS[wrap % 3], TEXTURE_FILTER_TRILINEAR, isColor);
    if (tex.id == 0) {
        free(path);
        snprintf(err, errSize, "%s:%d: cannot load the texture \"%s\"", d->path, line, rel);
        return tex;
    }
    TexEntry *e = &d->textures[d->textureCount++];
    e->path = path;
    e->isColor = isColor;
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

// Loads every model file of `d` (GPU). On failure: "map:line: message"
static bool LoadModelFiles(SceneData *d, char *err, int errSize)
{
    for (int i = 0; i < d->modelFileCount; i++) {
        MS_ModelFile *F = &d->modelFiles[i];
        char full[MS_PATH_MAX + MS_PATH_MAX];
        snprintf(full, sizeof(full), "%s%s", d->root, F->path);
        F->model = R3D_LoadModelEx(full, R3D_IMPORT_RETAIN_MESH_DATA);
        if (F->model.meshCount <= 0 || !F->model.meshes) {
            snprintf(err, errSize, "%s:%d: cannot load the model \"%s\" (unsupported or broken file; see the log; "
                     "--inspect-model shows why)", d->path, F->line, F->path);
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

// noGpu loads (--dump-boxes, --export, the peek before R3D_Init) don't upload models, but a
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
            snprintf(err, errSize, "%s:%d: cannot load the model \"%s\" (unsupported or broken file; see the log; "
                     "--inspect-model shows why)", d->path, F->line, F->path);
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
                         d->path, M->ref.line, d->modelSlots[M->slotFirst + k].slot, F->path, F->materialCount,
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

    bool ok = true;
    for (int g = 0; g < d->modelGroupCount && ok; g++) {
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
        Vector3 *pos = R3D_MapInstances(G->inst, R3D_INSTANCE_POSITION, false);
        Quaternion *rot = R3D_MapInstances(G->inst, R3D_INSTANCE_ROTATION, false);
        Vector3 *scl = R3D_MapInstances(G->inst, R3D_INSTANCE_SCALE, false);
        if (!pos || !rot || !scl) ok = false;
        for (int i = 0, k = 0; i < d->modelCount && ok; i++) {
            if (groupOf[i] != g) continue;
            pos[k] = d->models[i].translation;
            rot[k] = d->models[i].rotation;
            scl[k] = d->models[i].scale;
            k++;
        }
        R3D_UnmapInstances(G->inst, R3D_INSTANCE_POSITION | R3D_INSTANCE_ROTATION | R3D_INSTANCE_SCALE);
    }
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
        if (d->instCount && d->instCount[i] > 0) R3D_UnloadInstanceBuffer(d->inst[i]);
        if (d->materials[i].material.shader) R3D_UnloadSurfaceShader(d->materials[i].material.shader);
        d->materials[i].material.shader = NULL;
    }
    free(d->inst);
    free(d->instCount);
    d->inst = NULL;
    d->instCount = NULL;
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

// Creates every GPU resource of `d` (the old scene's are still alive: new before old, see
// the instance-buffer note in MS_Reload). On failure everything created so far is freed.
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
            char full[MS_PATH_MAX + MS_PATH_MAX];
            snprintf(full, sizeof(full), "%s%s", d->root, M->shader);
            m->shader = R3D_LoadSurfaceShader(full);
            if (!m->shader) {
                snprintf(err, errSize, "%s:%d: cannot load the surface shader \"%s\" (see the log)", d->path, D->shaderLine, M->shader);
                FreeGpu(d);
                return false;
            }
            for (int u = 0; u < D->uniformCount; u++)
                R3D_SetSurfaceShaderUniform(m->shader, D->uniforms[u].name, D->uniforms[u].v);
            for (int s = 0; s < D->samplerCount; s++) {
                Texture2D tex = GetTexture(d, D->samplers[s].path, true, D->wrap, D->samplers[s].line, err, errSize);
                if (!tex.id) { FreeGpu(d); return false; }
                R3D_SetSurfaceShaderSampler(m->shader, D->samplers[s].name, tex);
            }
        }
        static const bool IS_COLOR[MAP_COUNT] = { true, false, false, true };
        Texture2D *slot[MAP_COUNT] = { &m->albedo.texture, &m->normal.texture, &m->orm.texture, &m->emission.texture };
        for (int k = 0; k < MAP_COUNT; k++) {
            if (!D->maps[k]) continue;
            *slot[k] = GetTexture(d, D->maps[k], IS_COLOR[k], D->wrap, D->mapLine[k], err, errSize);
            if (!slot[k]->id) { FreeGpu(d); return false; }
        }

        // Box centres and sizes -> instance buffer of unit cubes, in box order
        int count = 0;
        for (int b = 0; b < d->boxCount; b++) count += d->boxRefs[b].material == i;
        d->instCount[i] = count;
        if (count == 0) continue;
        d->inst[i] = R3D_LoadInstanceBuffer(count, R3D_INSTANCE_POSITION | R3D_INSTANCE_SCALE);
        Vector3 *pos = R3D_MapInstances(d->inst[i], R3D_INSTANCE_POSITION, false);
        for (int b = 0, k = 0; b < d->boxCount; b++)
            if (d->boxRefs[b].material == i) pos[k++] = Vector3Scale(Vector3Add(d->boxes[b].min, d->boxes[b].max), 0.5f);
        R3D_UnmapInstances(d->inst[i], R3D_INSTANCE_POSITION);
        Vector3 *scl = R3D_MapInstances(d->inst[i], R3D_INSTANCE_SCALE, false);
        for (int b = 0, k = 0; b < d->boxCount; b++)
            if (d->boxRefs[b].material == i) scl[k++] = Vector3Subtract(d->boxes[b].max, d->boxes[b].min);
        R3D_UnmapInstances(d->inst[i], R3D_INSTANCE_SCALE);
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
    for (int i = 0; i < d->groupCount; i++) free(d->groups[i]);
    free(d->groups);
    for (int i = 0; i < d->viewpointCount; i++) free((char *)d->viewpoints[i].name);
    free(d->viewpoints);
    for (int i = 0; i < d->depCount; i++) free(d->deps[i]);
    free(d->deps);
    free(d->boxes);
    free(d->boxRefs);
    free(d->lights);
    free(d->path);
    free(d->root);
    free(d->brief);
    free(d->text);
    free(d);
}

// Points the public fields of the scene at d
static void Publish(MS_Scene *s, SceneData *d)
{
    s->formatVersion = d->formatVersion;
    s->path = d->path;
    s->root = d->root;
    s->brief = d->brief;
    s->hash = d->hash;
    s->mtime = d->mtime;
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
    s->colliders = d->modelCount > 0 ? d->colliders : d->boxes;
    s->colliderCount = d->modelCount > 0 ? d->colliderCount : d->boxCount;
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
}

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

// Parses `text` (takes ownership), uploads it and swaps it in on success
static bool LoadText(MS_Scene *s, const char *mapPath, char *text, int size, char *err, int errSize)
{
    MS_Private *p = s->priv;
    SceneData *d = calloc(1, sizeof(*d));
    if (!d) {
        snprintf(err, errSize, "%s: out of memory", mapPath);
        snprintf(p->error, sizeof(p->error), "%s", err);
        free(text);
        return false;
    }
    d->text = text;
    d->textSize = size;
    bool ok = ParseScene(d, mapPath, text, size, &p->opt, err, errSize);
    // Watch the files of this attempt (also after a failure: a shader that is still missing
    // reloads the package once it appears)
    SetWatch(p, d);
    p->triedHash = CombinedHash(text, size, p->watch, p->watchCount);
    p->settleTime = 0.0f;
    if (ok && p->gpu) {
        d->gpu = true;                  // (so FreeData unloads what LoadModelFiles loaded)
        ok = LoadModelFiles(d, err, errSize);
    }
    if (ok && !p->gpu) ok = CheckModelFiles(d, err, errSize);
    if (ok) ok = ResolveModels(d, err, errSize);
    if (ok) FinishGeometry(d);
    if (ok && p->gpu) {
        ok = UploadGpu(d, err, errSize);
        if (ok && !p->warnedShadowSize && R3D_GetHint(R3D_HINT_SHADOW_DIR_SIZE) != d->env.sun.shadowSize) {
            p->warnedShadowSize = true;
            TraceLog(LOG_WARNING, "monolith_scene: shadow.size %d is not the R3D hint %d (it applies at R3D_Init: "
                     "MS_SetInitHints before it, or restart)", d->env.sun.shadowSize, R3D_GetHint(R3D_HINT_SHADOW_DIR_SIZE));
        }
    }
    if (!ok) {
        snprintf(p->error, sizeof(p->error), "%s", err);
        FreeData(d);
        return false;
    }
    d->hash = Fnv1a64Update(FNV_OFFSET, text, (size_t)size);
    d->mtime = GetFileModTime(mapPath);

    // Swap: the new GPU resources exist already, the old ones are freed after. R3D caches
    // the instance buffer names it last bound (instances_enable in r3d_render.c) and skips
    // re-binding when they match; freeing first let GL hand out the same names again, so the
    // reloaded map was drawn with the old boxes.
    SceneData *old = p->d;
    p->d = d;
    s->version++;
    Publish(s, d);
    FreeData(old);
    p->error[0] = '\0';
    if (errSize > 0) err[0] = '\0';
    return true;
}

static bool LoadPath(MS_Scene *s, const char *path, char *err, int errSize)
{
    MS_Private *p = s->priv;
    char mapPath[MS_PATH_MAX];
    if (!ResolveMapPath(path, mapPath, sizeof(mapPath), err, errSize)) {
        snprintf(p->error, sizeof(p->error), "%s", err);
        return false;
    }
    int size;
    char *text = ReadWholeFile(mapPath, &size, err, errSize);
    if (!text) {
        snprintf(p->error, sizeof(p->error), "%s", err);
        return false;
    }
    return LoadText(s, mapPath, text, size, err, errSize);
}

// #endregion

// ======================================================================================
// #region Public API: loading and hot reload
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
    p->rs = (MS_RenderSettings){ .sunOn = true, .shadowMode = MS_SHADOW_TILES, .fogCacheSub = 2, .depthPrepass = true,
                                 .renderScale = 1.0f, .upscale = R3D_UPSCALE_BICUBIC, .aoVolumeIntensity = 1.0f,
                                 .sunShadow = true };
#else
    p->rs = (MS_RenderSettings){ .sunOn = true, .shadowMode = MS_SHADOW_CAMERA, .fogCacheSub = 2, .depthPrepass = true,
                                 .renderScale = 1.0f, .upscale = R3D_UPSCALE_BICUBIC, .aoVolumeIntensity = 1.0f,
                                 .sunShadow = true };
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
        TraceLog(LOG_INFO, "monolith_scene: %s: %d boxes, %d models (%d files), %d groups, %d materials, %d lights; sun shadow %d px, %.2f cm texels",
                 s->path, s->boxCount, s->modelCount, s->modelFileCount, s->groupCount, s->materialCount, s->lightCount, mapSize,
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
        FreeWatch(p);
        free(p->watch);
        free(p->watchMtime);
        free(p);
    }
    free(s);
}

MS_ReloadResult MS_Reload(MS_Scene *s, const char *path, char *err, int errSize)
{
    char *cur = MS_StrDup(path ? path : s->path);     // (s->path is freed by a successful load)
    if (!cur) {
        snprintf(err, errSize, "out of memory");
        return MS_RELOAD_FAILED;
    }
    bool ok = LoadPath(s, cur, err, errSize);
    free(cur);
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
    char *text = ReadWholeFile(mapPath, &size, err, sizeof(err));
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
    bool ok = LoadText(s, mapPath, text, size, err, sizeof(err));
    p->settleTime = settle;                     // keep watching while the editor may still write
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
    for (int l = 1; l < line; l++) {
        while (pos < d->textSize && d->text[pos] != '\n') pos++;
        if (pos >= d->textSize) return NULL;
        pos++;
    }
    if (pos >= d->textSize) return NULL;
    int len = 0;
    while (pos + len < d->textSize && d->text[pos + len] != '\n') len++;
    if (len > 0 && d->text[pos + len - 1] == '\r') len--;
    *length = len;
    return d->text + pos;
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
// Presets from the measured trade-off knobs (PROMPT-INSTRUCTIONS.md 7.3 "Trade-off knobs" and
// "Graphics settings"): ultra is the map exactly; high trades SSAO + SSIL for the baked
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
    // Bloom here is mostly a uniform ~20% lift (threshold 0, MIX 0.04): without it, exposure
    // x1.19 keeps the mean brightness (matched over the 26 verification shots)
    if (!q->bloom && env->bloom.mode != R3D_BLOOM_DISABLED) {
        env->bloom.mode = R3D_BLOOM_DISABLED;
        env->tonemap.exposure *= 1.19f;
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
    return m;
}

bool MS_PushLights(MS_Scene *s, Camera3D camera, float aspect)
{
    MS_Private *p = s->priv;
    SceneData *d = p->d;
    bool update = false;
    if (p->rs.sunOn && d->env.sun.enabled) {
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
        if (!L->shadow || !d->lightShadow[i].handle) { R3D_PushLight(L->light); continue; }
        // Static scene: point / spot shadows are drawn once; extra directional lights follow the camera
        bool draw = !d->lightShadowDrawn[i] || L->light.type == R3D_LIGHT_DIR;
        R3D_PushLightEx(L->light, d->lightShadow[i], draw);
        d->lightShadowDrawn[i] = true;
    }
    return update;
}

void MS_DrawGeometry(MS_Scene *s)
{
    MS_Private *p = s->priv;
    SceneData *d = p->d;
    for (int i = 0; i < d->materialCount; i++)
        if (d->instCount[i] > 0) R3D_DrawMeshInstanced(p->cube, d->materials[i].material, d->inst[i], d->instCount[i]);
    // Models: one instanced draw per (file, materials, shadow); they go into the sun's
    // shadow tiles too, which are drawn once per map load like for the boxes
    for (int g = 0; g < d->modelGroupCount; g++)
        R3D_DrawModelInstanced(d->modelGroups[g].model, d->modelGroups[g].inst, d->modelGroups[g].count);
#ifdef MS_PATCHED_R3D
    // Occlusion volume: once per map load (CPU bake + upload; not a pass of this frame)
    if (p->rs.aoVolumeVoxel > 0.0f && p->aoVersion != s->version) {
        if (!BakeOcclusionVolume(d, p->rs.aoVolumeVoxel, p->rs.aoVolumeIntensity)) R3D_ClearWorldOcclusion();
        p->aoVersion = s->version;
    }
    if (p->rs.sunOn && d->env.sun.enabled && d->env.sun.shadow && p->rs.sunShadow && p->rs.fogCacheVoxel > 0.0f && p->fogCacheVersion != s->version) {
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

MS_Hit MS_Raycast(const MS_Scene *s, Vector3 origin, Vector3 dir, float maxDist)
{
    MS_Hit best = { .obj = { MS_OBJ_NONE, -1 }, .dist = INFINITY };
    const float o[3] = { origin.x, origin.y, origin.z };
    const float d[3] = { dir.x, dir.y, dir.z };
    float limit = maxDist > 0.0f ? maxDist : INFINITY;

    for (int i = 0; i < s->boxCount; i++) {
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
        if (miss || axis < 0 || tNear < 0.0f || tNear >= best.dist || tNear > limit) continue;

        best.obj = (MS_ObjRef){ MS_OBJ_BOX, i };
        best.dist = tNear;
        best.point = Vector3Add(origin, Vector3Scale(dir, tNear));
        float n[3] = { 0, 0, 0 };
        n[axis] = d[axis] > 0.0f ? -1.0f : 1.0f;
        best.normal = (Vector3){ n[0], n[1], n[2] };
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

bool MS_Collides(const MS_Scene *s, BoundingBox a)
{
    for (int i = 0; i < s->colliderCount; i++) {
        const BoundingBox *b = &s->colliders[i];
        if (a.min.x < b->max.x && a.max.x > b->min.x && a.min.y < b->max.y && a.max.y > b->min.y &&
            a.min.z < b->max.z && a.max.z > b->min.z) return true;
    }
    return false;
}

const MS_Ref *MS_ObjectRef(const MS_Scene *s, MS_ObjRef obj)
{
    if (obj.kind == MS_OBJ_BOX && obj.index >= 0 && obj.index < s->boxCount) return &s->boxRefs[obj.index];
    if (obj.kind == MS_OBJ_MODEL && obj.index >= 0 && obj.index < s->modelCount) return &s->models[obj.index].ref;
    return NULL;
}

BoundingBox MS_ObjectBounds(const MS_Scene *s, MS_ObjRef obj)
{
    if (obj.kind == MS_OBJ_BOX && obj.index >= 0 && obj.index < s->boxCount) return s->boxes[obj.index];
    if (obj.kind == MS_OBJ_MODEL && obj.index >= 0 && obj.index < s->modelCount) return s->models[obj.index].bounds;
    return (BoundingBox){ 0 };
}

// #endregion

// ======================================================================================
// #region Public API: writing the environment
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
    return !ferror(f);
}

// #endregion

// ======================================================================================
// #region Public API: package files, R3D environment export (--export, capture shim)
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
