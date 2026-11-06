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
//   box    x0 y0 z0 x1 y1 z1 [rot y p r]        [1] two opposite corners, any order
//   block  cx y0 cz sx sy sz [rot y p r]        [1] centered on (cx, cz), from y0 up to y0 + sy
//       rot yawDeg pitchDeg rollDeg   (format 2) turns the box as written about its own centre,
//                                     with the models' convention (R = Ry * Rx * Rz). Never
//                                     shifted: every repeat / at copy turns about its own centre.
//                                     rot 0 0 0 is the box without rot. A turned box draws, collides
//                                     and picks as its turned shape; its boxes[] / colliders[] entry
//                                     is its world AABB (MS_Scene.turnedBoxes, boxTurned)
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
//         prepass on|off   [on] off: this material is never drawn in the depth pre-pass, only once in
//                                    the shading pass (for a cheap shader on very many triangles, where
//                                    the pre-pass would only double the vertex cost). With R3D patch 28;
//                                    upstream R3D always pre-passes, so the key changes nothing there
//         blend alpha|additive|add_alpha|multiply|premultiplied  unlit on|off  priority n
//         inherit albedo|normal|orm|emission|all   (model overrides: keep the model's own map)
//         wrap repeat|clamp|mirror   wrapping of this material's textures and samplers
//                                    (default repeat, so uv_scale > 1 tiles; trilinear + mipmaps)
//         filter trilinear|nearest   magnification of its albedo / normal / orm / emission maps:
//                                    trilinear (default), or nearest (crisp texels up close, for
//                                    pixel art; nearest texels of the two nearest mipmaps blended
//                                    at a distance). A file used with both filters is two textures
//         uv_faces a b c d e f       box faces +x -x +y -y +z -z (a turned box: its own axes) in
//                                    cells of the texture: faces with the same number (0..5; every
//                                    number from 0 to the largest used) share a cell. k cells lie in
//                                    a grid of ceil(sqrt(k)) x ceil(k / cols), cell n at column
//                                    n % cols, row n / cols; a face keeps the cube's UV orientation
//                                    inside its cell, inset by 1/64 cell (the first few mipmap
//                                    levels don't bleed from the next cell; far smaller ones do).
//                                    Needs uv_scale 1 1 and uv_offset 0 0. Boxes only (merged,
//                                    instanced and turned); models ignore it. Without the key every
//                                    face shows the whole texture
//         instanced on|off   [off] keep this material's boxes as instanced unit cubes. Off: boxes
//                                    are drawn as one mesh per cell of the faces no other box hides
//                                    (see UploadBoxes). On is for shaders that read INSTANCE_* or the
//                                    model-space POSITION in vertex(); a scan of the source turns it
//                                    on with a warning when it finds those
//         paint "textures/paint/x.png"   the material's paint atlas (RGBA8 PNG, straight alpha): the
//                                    material draws with a paint stage after its own shader once a
//                                    `paint` line below draws with it (docs/painting.md 4); none: nothing
//                                    changes
//         paint_filter trilinear|nearest   how the atlas is sampled (nearest: crisp paint texels)
//   Colours are 0-255 sRGB. Unset keys keep R3D's defaults (R3D_MATERIAL_BASE).
//
// Paint (format 2; docs/painting.md 4.2), anywhere after the material's line:
//   paint "material" N:i <faces> x y w h   faces of box i of the line with `id N` (its index within the
//                                  line, map order), `<faces>` a comma list of +x -x +y -y +z -z (a
//                                  turned box: its own axes), get the rectangle x y w h (texels) of the
//                                  material's atlas; each face shows it in its own orientation (R3D's
//                                  unit cube UVs)
//   paint "material" N:i slot S x y w h   slot S of model instance i of the line `id N`, drawn with
//                                  that material (an override), in the slot's paint UVs (its own UVs
//                                  when they lie in 0..1 without overlap, else the file's unwrap
//                                  sidecar, models/<file>.paintuv)
//   paint own N:i slot S x y w h [paint_filter nearest]   the same for a slot drawn with the model's
//                                  own material: its atlas is textures/paint/<file>_s<S>.png
//   Rules (errors): the material exists and has a `paint` key, line N exists and made such an
//   object i, the faces are valid and not repeated, w h >= 1, no two rectangles of one atlas share a
//   texel, no face or slot of an object is in two entries of one material. An entry whose object no
//   longer draws that face or slot with that material, or whose rectangle lies outside its atlas,
//   draws nothing (logged). `paint` lines are no statements: they take no statement key.
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
    bool noPrepass;                     // `prepass off`: no depth pre-pass draw (R3D patch 28, MaterialDrawMode)
    bool hasFaces;                      // `uv_faces`: uvFaces[f] is the texture cell of box face f (+x -x +y -y +z -z)
    unsigned char uvFaces[6];
    unsigned char filter;               // `filter`: TEX_TRILINEAR (default) or TEX_NEAREST, for its four maps
    bool merge, occluder;               // set at upload: boxes become a visible-face mesh / may hide other boxes' faces
    int faceCube;                       // set at upload: 1 + its entry in faceCubes[] (hasFaces), 0 = R3D's cube
    // The paint layer: `paint "path"` (its atlas, NULL = none) and `paint_filter` (TEX_*); painted:
    // it has an active paint entry, so it draws with the paint version of its shader (PaintResolve);
    // paintW x paintH: the atlas image's size (0: unreadable); paintTex: the atlas on the GPU
    char *paint;
    int paintLine;
    unsigned char paintFilter;
    bool painted;
    int paintW, paintH;
    Texture2D paintTex;
} MatDef;

enum { TEX_TRILINEAR, TEX_NEAREST };

// A `paint` statement: a rectangle of an atlas for faces of one box or one slot of one model
// instance. material -1: the model slot's own material (its atlas is named by file and slot:
// ownAtlas). The object is resolved from the entity (`id N`, index i) when the map is parsed; the
// entity id itself is filled in after BuildEntities. active: drawn (PaintResolve); why: the reason
// it isn't (PAINT_*), for the log
typedef struct {
    int material;
    int kind;                           // PAINT_BOX or PAINT_MODEL
    int object;                         // boxes[] or models[] index
    unsigned char faces;                // boxes: bit f for face f (+x -x +y -y +z -z)
    int slot;                           // models: the file's material slot; boxes: -1
    int x, y, w, h;                     // texels of the atlas
    bool nearest;                       // `paint own ... paint_filter nearest`
    int line;
    MS_EntityId entity;
    int ownAtlas;                       // own material: index into ownAtlases[]; else -1
    bool active;
    signed char why;
} PaintDef;

enum { PAINT_BOX = 0, PAINT_MODEL = 1 };
enum { PAINT_OK = 0, PAINT_NOT_DRAWN, PAINT_OUTSIDE, PAINT_NO_ATLAS, PAINT_SKINNED, PAINT_NEEDS_UNWRAP, PAINT_NO_SHADER };

// The atlas of a model slot drawn with the file's own material: textures/paint/<file>_s<slot>.png
typedef struct {
    int file, slot;
    char *rel;
    bool nearest;                       // an entry asked for paint_filter nearest
    bool painted;                       // it has an active entry
    int w, h;                           // the image's size (0: none)
    Texture2D tex;
    R3D_SurfaceShader *shader;          // the own material's paint shader alias (GPU)
} OwnAtlas;

// A model file's paint UVs from an unwrap (its .paintuv sidecar, or one provided in memory): per
// mesh of the file, the unwrap's vertices (a source vertex each, with a unorm16 paint UV) and indices
typedef struct {
    int slot, vertexCount, indexCount;
    unsigned int *source;
    unsigned short *uv;
    unsigned int *indices;
} UnwrapMesh;

typedef struct {
    int meshCount;
    UnwrapMesh *meshes;
    unsigned long long fileHash;
} Unwrap;

// The paint state of a model file (parallel to modelFiles[]). painted: an active entry draws on
// one of its instances, so its meshes are drawn from `meshes` (rebuilt in memory with the slot and
// the paint UV in the bone channels; the file's own meshes are untouched). uvKind: per slot, an
// MS_PAINTUV_* value once computed (-1: not yet)
typedef struct {
    bool painted, keepData;
    bool hasUnwrap;                     // `unwrap` holds a valid unwrap of the file as loaded
    Unwrap unwrap;
    unsigned int unwrapGen;             // 0: from the sidecar; else the provided unwrap's generation
    R3D_Mesh *meshes;                   // GPU: the paint meshes, NULL = none
    int meshCount;
    unsigned int meshesGen;             //   built from the unwrap of this generation (or own UVs: ~0u)
    signed char *uvKind;
    int uvKindCount;
} FilePaint;

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

// A loaded texture. premul: a paint atlas, uploaded with its colour premultiplied by its alpha (in
// linear light, stored sRGB-encoded) so filtering transparent texels adds no dark fringe
typedef struct { char *path; bool isColor, data, premul; int wrap, filter; Texture2D tex; } TexEntry;

// The unit cube of one face layout (`uv_faces`): R3D's cube with each face's UVs moved into its
// cell (FaceLayoutData), as mesh data for the merged meshes and as a mesh for instanced draws
typedef struct {
    unsigned char cells[6];
    R3D_MeshData data;
    R3D_Mesh mesh;
} FaceCube;

// A surface shader file of the map, compiled once: every material that uses it draws with an
// alias of it (its own uniforms and samplers, the shared program and shader variants)
// rel NULL: the empty shader (only its paint version, for painted materials without a shader).
// Each version is compiled when a material first draws with it.
typedef struct {
    const char *rel;                    // the materials' shader path (not owned)
    R3D_SurfaceShader *shader;          // never drawn with: aliases copy its default uniform values
    bool scanned, needsCube, cutout;    // ScanShader of its source (unreadable: both true)
    R3D_SurfaceShader *paint;           // its paint version (PaintShaderSource), NULL = not compiled
    bool paintTried;                    //   a compile was tried (paint NULL then: it failed)
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
#ifdef MONOLITH_EDITOR
    Vector2 *uv;                // per vertex: its texture coordinate (NULL: the file has none)
    unsigned int *triSrc;       // per triangle (BVH order): its index in file order (the meshes' triangles in turn)
    unsigned int *triAt;        //   and back: per triangle in file order, its place in tri[]
    int *meshTri;               // per mesh + 1: the first triangle of each mesh in file order (meshCount + 1)
    int meshCount;
#endif
} ModelPick;

// One draw: instances [first, first + count) of a buffer (group = the material for boxes, the
// ModelGroup for models), bounds = the union of their boxes. Clustered and instanced, except a
// merged box range (MatDef.merge): its draw is `mesh`, the visible faces of its boxes (empty: none)
typedef struct { int group, first, count; BoundingBox bounds; R3D_Mesh mesh; } DrawRange;

// An object to sort into cells: group, then big (wider than a cell in x or z: one cluster per
// group), then turned (a turned box: ranges of their own after the axis-aligned ones), then cell,
// then index (keeps map order within a cluster)
typedef struct { int group, big, turned, cx, cz, index; BoundingBox bounds; } CellItem;

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
    MS_TurnedBox *turned;               // the boxes with a rotation (boxes[] holds their world AABBs)
    int turnedCount, turnedCap;
    int *boxTurned;                     // per box: index into turned[], -1 = none; NULL while there are none
    int boxTurnedCap;
    BoundingBox *turnedWritten;         // parallel to turned[]: each box as written (corners ordered), for
    int turnedWrittenCap;               // compiled maps, which rebuild the record from it bit for bit
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

    // The paint layer (docs/painting.md): the `paint` entries, the atlases of model slots drawn
    // with their own material, each model file's paint state (parallel to modelFiles[], NULL with
    // no model files); per box / model instance the first of its paint keys (0 = none; NULL while
    // nothing is painted) and the paint table: 4 floats per key (the rectangle, normalised to its
    // atlas), PAINT_TABLE_W keys per row, its texture (GPU); anyPaint: an active entry exists
    PaintDef *paints;
    int paintCount, paintCap;
    OwnAtlas *ownAtlases;
    int ownAtlasCount, ownAtlasCap;
    FilePaint *filePaint;
    int *boxPaintKey, *modelPaintKey;
    int paintKeyCount;
    float *paintTable;
    int paintTableRows;
    Texture2D paintTableTex;
    bool anyPaint;
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
    FaceCube *faceCubes;                // one per distinct `uv_faces` layout of the materials (BuildFaceCubes)
    int faceCubeCount, faceCubeCap;
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
    // Live edits (MS_EditSetText): what the next edit compares with and keeps
    unsigned long long lookHash;        // the statements that are no objects (materials, env, format)
    unsigned long long restLookHash;    // the same without the materials (env, format)
    unsigned long long *matLookHash;    // per material: its `material` lines (parallel to materials[])
    unsigned long long *matShapeHash;   //   and the same without its uv_faces, filter, albedo_map, paint
                                        //   and paint_filter keys
    unsigned long long *matPaintlessHash;   //   and its lines without the paint and paint_filter keys
    int matLookHashCap, matShapeHashCap, matPaintlessHashCap;
    unsigned long long paintHash;       // the `paint` statements (no look hash has them)
    MS_PaintEntry *paintPub;            // MS_PaintEntryAt's view of paints[] (filled by PaintResolve)
    struct MS_Private *live;            // MS_EditSetText: the scene whose provided textures it may use
    unsigned char *hidden;              // per box: its hidden faces (UploadBoxes), NULL = none merged
    int *boxOrder;                      // the boxes in draw order (material, cell; UploadBoxes)
    int boxOrderCount;
    int *boxRangeOf;                    // per box: its range in boxRanges[], -1 = none
    int *modelSlotOf, *modelRangeOf;    // per model instance: its slot in its group's buffer, its range
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

#ifdef MONOLITH_EDITOR
// An image provided for a package path (MS_PaintAtlasProvide): what an upload from memory uses
// instead of the file (RGBA8, owned)
typedef struct ProvidedTex {
    char *rel;
    Image img;
} ProvidedTex;

// An unwrap provided for a model file (MS_PaintUnwrapProvide): what the next live edit uses instead
// of the file's sidecar (owned)
typedef struct ProvidedUnwrap {
    char *rel;
    Unwrap u;
    unsigned int gen;
} ProvidedUnwrap;

// A transform preview (MS_EditPreviewBegin .. End): the objects, what they were at Begin, and how
// the selected boxes are drawn meanwhile (out of their merged meshes and instanced buffers, as
// instanced cubes from buf)
struct EditPreview {
    MS_ObjRef *objs;
    int count;
    BoundingBox *boxOrig;               // per object: boxes' bounds at Begin
    MS_TurnedBox *turnOrig;             //   and turned boxes' records
    MS_ModelInstance *modelOrig;        // models
    int *localFirst;                    // models: first of their `collide box` boxes in localColliders
    BoundingBox *colliderOrig;          // models' world colliders, objs[i]'s at colliderAt[i]
    int *colliderAt;
    MS_LightDef *lightOrig;             // lights
    MS_Transform *entityOrig;
    BoundingBox *reach;                 // per object: everything it covered since Begin
    unsigned char *boxSel;              // per box: selected
    unsigned char *hiddenOrig;          // d->hidden at Begin (NULL: none)
    int *ranges, rangeCount;            // merged ranges drawn without the selected boxes
    R3D_Mesh *meshOrig;                 //   and their meshes at Begin
    int *instMat, *instSlot, instCount; // selected boxes of instanced materials: material, slot
    Vector3 *instScaleOrig;
    BoundingBox *modelRangeOrig;        // model range bounds at Begin (grown while models move)
    R3D_InstanceBuffer buf;             // the selected boxes as cubes, by material
    int *bufBox, bufCount;              //   box of each slot
    R3D_Mesh *turnMesh;                 // per draw range of a merged material: its boxes that have a
    BoundingBox *turnBounds;            //   rotation as one mesh (as their merged range will draw
    R3D_MeshData cube;                  //   them; their cubes get a zero scale), R3D's cube data
    int *drawMat, *drawFirst, *drawCount, drawRanges;
    BoundingBox *drawBounds;
    long long trisDrawn;                // d->boxTrisDrawn, d->boxMeshCount at Begin
    int meshCount;
};
#endif

struct MS_Private {
    SceneData *d;
    MS_LoadOptions opt;
    MS_RenderSettings rs;
#ifdef MONOLITH_EDITOR
    char *error;                        // MS_Error: the last failed (re)load's message (heap), NULL = ""
    bool errorOom;                      //   ... and building that copy ran out of memory

    // Hot reload: the files of the last load attempt and their mtimes, the combined hash
    // of the last attempt's contents
    char **watch;
    long *watchMtime;
    int watchCount, watchCap, watchMtimeCap;
    float pollTimer, settleTime;
    unsigned long long triedHash;

    // Live edits (MS_EditSetText): the world bounds of what the last edit changed, the occlusion
    // volume's bytes as last uploaded (an edit recomputes only the voxels near a change), the
    // preview of a transform (MS_EditPreviewBegin .. End; NULL = none)
    BoundingBox *dirty;
    int dirtyCount, dirtyCap;
    unsigned char *aoGrid;
    int aoDim[3];
    BoundingBox aoRegion;
    float aoVoxel, aoIntensity;
    struct EditPreview *pv;

    // Painting (region "Painting"): images provided for package paths (MS_PaintAtlasProvide), the
    // texture generation (+1 on every MS_TextureUpdate) and the scratch a sub-rectangle upload
    // packs its rows into (grown to the largest rectangle seen)
    struct ProvidedTex *provided;
    int providedCount, providedCap;
    unsigned int textureGeneration;
    unsigned char *texScratch;
    int texScratchCap;
    // Unwraps provided for model files (MS_PaintUnwrapProvide), by the file's package path; each
    // provide gets the next generation (a file's paint meshes are rebuilt when it changes)
    struct ProvidedUnwrap *unwraps;
    int unwrapCount, unwrapCap;
    unsigned int unwrapGeneration;

    // Meshes a live edit replaced, freed by the next MS_DrawGeometry (after a frame drew the new ones):
    // see LiveRetire
    R3D_Mesh *retired;
    int retiredCount, retiredCap;

    // The render inspector (region "Draw ranges"): per draw range the objects it draws, built on
    // the first query after a version change (rangeObj: box indices of the box ranges, then model
    // instance indices; range r's start at rangeAt[r]), and the draw filter (drawHidden per range,
    // filterOn: any range hidden) with the objects it hides (boxes, then models)
    int *rangeObj, *rangeAt, *rangeMatAt;
    int rangeObjCap, rangeAtCap, rangeMatAtCap;
    unsigned int rangeVersion;
    const SceneData *rangeData;
    bool *drawHidden;
    int drawHiddenCount, drawHiddenCap;
    bool filterOn;
    unsigned char *filterObj, *filterObjNew;
    int filterObjCount, filterObjCap, filterObjNewCap;
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

// The calling thread's last error (MS_LastError): a heap string with no length limit. NULL
// with errOom false is "" (nothing to free); errOom: building the last message ran out of
// memory, and MS_LastError() is the static "out of memory" instead. Per thread, so a program's
// own threads never see another thread's message.
static _Thread_local char *errText;
static _Thread_local size_t errLen;
static _Thread_local bool errOom;

// The last error becomes (append: the last error +) "<path>: " (line > 0: "<path>:<line>: ";
// path NULL: no prefix) + the printf-formatted message. The new text is built in a new string
// before the old one is freed, so arguments may point into the last error itself. Out of memory:
// the last error becomes "out of memory" (never half a message).
static void ErrPutV(bool append, const char *path, int line, const char *fmt, va_list ap)
{
    size_t keep = append && !errOom ? errLen : 0;
    int pre = !path ? 0 : line > 0 ? snprintf(NULL, 0, "%s:%d: ", path, line) : snprintf(NULL, 0, "%s: ", path);
    va_list measure;
    va_copy(measure, ap);
    int body = vsnprintf(NULL, 0, fmt, measure);
    va_end(measure);
    if (pre < 0) pre = 0;       // (an encoding error: nothing to show; %ls is never used)
    if (body < 0) body = 0;

    size_t total = keep + (size_t)pre + (size_t)body;
    char *s = total < SIZE_MAX ? malloc(total + 1) : NULL;
    if (!s) {
        free(errText);
        errText = NULL;
        errLen = 0;
        errOom = true;
        return;
    }
    if (keep) memcpy(s, errText, keep);
    s[keep] = '\0';
    if (pre && line > 0) snprintf(s + keep, (size_t)pre + 1, "%s:%d: ", path, line);
    else if (pre) snprintf(s + keep, (size_t)pre + 1, "%s: ", path);
    if (body) vsnprintf(s + keep + pre, (size_t)body + 1, fmt, ap);
    s[total] = '\0';
    free(errText);
    errText = s;
    errLen = total;
    errOom = false;
}

// Sets the last error to a printf-formatted message; returns false, so a check can
// `return ErrSet(…)`. The content (paths, names, tokens) goes in as arguments: no stack buffer.
static bool ErrSet(const char *fmt, ...) MS_PRINTF_LIKE(1, 2);
static bool ErrSet(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ErrPutV(false, NULL, 0, fmt, ap);
    va_end(ap);
    return false;
}

// Appends a printf-formatted piece to the last error (lists of names); returns false
static bool ErrAppendF(const char *fmt, ...) MS_PRINTF_LIKE(1, 2);
static bool ErrAppendF(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ErrPutV(true, NULL, 0, fmt, ap);
    va_end(ap);
    return false;
}

// A successful call: the last error becomes "" (its memory is kept for the next message)
static void ErrClear(void)
{
    if (errText) errText[0] = '\0';
    errLen = 0;
    errOom = false;
}

const char *MS_LastError(void)
{
    return errOom ? "out of memory" : errText ? errText : "";
}

void MS_ClearError(void)
{
    free(errText);
    errText = NULL;
    errLen = 0;
    errOom = false;
}

#ifdef MONOLITH_EDITOR
bool MS_SetError(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ErrPutV(false, NULL, 0, fmt, ap);
    va_end(ap);
    return false;
}

bool MS_AppendError(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ErrPutV(true, NULL, 0, fmt, ap);
    va_end(ap);
    return false;
}
#endif

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

// Reads a whole file of at most maxBytes (malloc'd, NUL-terminated). NULL on failure, with the
// reason in the last error when `report` (else the last error is left alone: a caller that
// only tries a file, or will say why itself).
static char *ReadWholeFile(const char *path, long maxBytes, int *size, bool report)
{
#define READ_FAIL(...) do { if (report) ErrSet(__VA_ARGS__); } while (0)
    FILE *f = fopen(path, "rb");
    if (!f) { READ_FAIL("%s: cannot open file", path); return NULL; }
    char *buf = NULL;
    long len = -1;
    if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
    else {
        READ_FAIL("%s: cannot seek to the end: %s", path, strerror(errno));
        fclose(f);
        return NULL;
    }

    signed char start = (signed char)fseek(f, 0, SEEK_SET);
    if (start != 0) {
        READ_FAIL("%s: cannot seek to the start: %s", path, strerror(errno));
        fclose(f);
        return NULL;
    }

    if (len < 0 || len > maxBytes)
        READ_FAIL("%s: cannot read file (or larger than %ld MB)", path, maxBytes / (1024 * 1024));
    else if (!(buf = malloc(len + 1)))
        READ_FAIL("%s: out of memory", path);
    else if ((long)fread(buf, 1, len, f) != len) {
        READ_FAIL("%s: read error", path);
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) { buf[len] = '\0'; *size = (int)len; }
    return buf;
#undef READ_FAIL
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
// file with that extension. 1 = found (in out), 0 = none or several (*count of them; several: the
// last error becomes their names, each after a space, for the caller to put into its message),
// -1 = path too long.
static int PackageFile(const char *dir, const char *base, const char *ext, char *out, int outSize, int *count)
{
    char cand[MS_PATH_MAX + 80];
    *count = 0;
    if (snprintf(cand, sizeof(cand), "%s/%s%s", dir, base, ext) >= (int)sizeof(cand)) return -1;
    if (FileExists(cand)) return snprintf(out, outSize, "%s", cand) < outSize ? 1 : -1;
    FilePathList list = LoadDirectoryFilesEx(dir, ext, false);
    *count = (int)list.count;
    signed char found = 0;
    if (list.count == 1) found = (signed char)(snprintf(out, outSize, "%s", list.paths[0]) < outSize ? 1 : -1);
    if (list.count > 1) {
        ErrSet("%s", "");
        for (unsigned i = 0; i < list.count; i++) ErrAppendF(" %s", GetFileName(list.paths[i]));
    }
    UnloadDirectoryFiles(list);
    return found;
}

// A package folder or a map file. A folder's map: editor builds read its text, <name>.map (else
// its only .map file), and a compiled map when there is no text; game builds read only compiled
// maps, <name>.mapb (else the folder's only .mapb file)
static bool ResolveMapPath(const char *in, char *out, int outSize)
{
    if (strlen(in) >= (size_t)outSize) return ErrSet("%s: path too long", in);
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
    int count, found;
    // (several files: PackageFile left their names in the last error, which the message quotes)
#ifdef MONOLITH_EDITOR
    found = PackageFile(dir, base, ".map", out, outSize, &count);
    if (found == 1) return true;
    if (found == 0 && count == 0) {
        int compiledCount;
        found = PackageFile(dir, base, ".mapb", out, outSize, &compiledCount);
        if (found == 1) return true;
    }
    if (found < 0) ErrSet("%s: path too long", dir);
    else ErrSet("%s: no %s.map in the package folder and %s%s", dir, base,
                count ? "more than one other .map file:" : "no other .map file", count ? MS_LastError() : "");
#else
    found = PackageFile(dir, base, ".mapb", out, outSize, &count);
    if (found == 1) return true;
    if (found < 0) ErrSet("%s: path too long", dir);
    else if (count == 0)
        ErrSet("%s: no %s.mapb in the package folder (this program reads compiled maps: compile the "
               "map's text with monolith)", dir, base);
    else ErrSet("%s: no %s.mapb in the package folder and more than one other .mapb file:%s", dir, base, MS_LastError());
#endif
    return false;
}

// A package-relative path that can't leave the package: not empty, not absolute, no drive letter,
// no '\\', no ".." component
static bool InsidePackage(const char *s)
{
    if (s[0] == '\0') return false;
    if (s[0] == '/' || s[0] == '\\') return false;   // absolute
    if (s[1] == ':') return false;                    // drive letter (s[0] is not the terminator)
    if (strchr(s, '\\')) return false;

    // Walk the '/'-separated components
    const char *part = s;
    for (;;) {
        size_t len = strcspn(part, "/");
        if (len == 2 && part[0] == '.' && part[1] == '.') return false;
        if (part[len] == '\0') return true;
        part += len + 1;
    }
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

// Compiled maps store these values (CmEnv.type): new types only at the end, CM_VERSION bumped
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

// "<path>: <message>" (line > 0: "<path>:<line>: <message>") as the last error; returns false
static bool ErrAt(const char *path, int line, const char *fmt, ...) MS_PRINTF_LIKE(3, 4);
static bool ErrAt(const char *path, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ErrPutV(false, path, line, fmt, ap);
    va_end(ap);
    return false;
}

// The checks an env value must pass once read (`key` = its full name, p = the value of `type`):
// false with "<path>[:<line>]: <reason>" in the last error. One place for both the text parser
// and compiled maps.
static bool CheckEnvValue(const char *key, int type, const void *p, const char *path, int line)
{
    int i = type == T_I ? *(const int *)p : 0;
    if (strcmp(key, "shadow.size") == 0 && (i < 256 || i > 16384)) {
        ErrAt(path, line, "shadow.size must be 256..16384");
        return false;
    }
    if ((strcmp(key, "sky.size") == 0 || strcmp(key, "ambientSky.size") == 0) && (i < 16 || i > 4096)) {
        ErrAt(path, line, "sky sizes must be 16..4096");
        return false;
    }
    if (strcmp(key, "sun.range") == 0 && !(*(const float *)p > 0.0f)) {
        ErrAt(path, line, "sun.range must be > 0");
        return false;
    }
    // Directions (sun.direction, sky.sunDirection, ambientSky.sunDirection); shadow.home is a point
    if (type == T_V3 && strcmp(key, "shadow.home") != 0 && Vector3Length(*(const Vector3 *)p) == 0.0f) {
        ErrAt(path, line, "%s must not be 0 0 0", key);
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
        ErrAt(path, line, "%s must be %s %s and <= %s", key, r->loOpen ? ">" : ">=", lo, hi);
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
// #region Small helpers: turned boxes (a box or block with a rotation)
// ======================================================================================

// R = Ry * Rx * Rz of yaw, pitch, roll in degrees (the same code as the models' rotation)
static Quaternion RotationYpr(Vector3 deg)
{
    Quaternion qy = QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, deg.x * DEG2RAD);
    Quaternion qx = QuaternionFromAxisAngle((Vector3){ 1, 0, 0 }, deg.y * DEG2RAD);
    Quaternion qz = QuaternionFromAxisAngle((Vector3){ 0, 0, 1 }, deg.z * DEG2RAD);
    return QuaternionNormalize(QuaternionMultiply(QuaternionMultiply(qy, qx), qz));
}

// Box b's turned-box record, NULL for an axis-aligned box
static inline const MS_TurnedBox *TurnedOf(const SceneData *d, int b)
{
    return d->boxTurned && d->boxTurned[b] >= 0 ? &d->turned[d->boxTurned[b]] : NULL;
}

// A direction given in a turned box's own frame, in the world
static inline Vector3 TurnedDir(const MS_TurnedBox *t, float x, float y, float z)
{
    return (Vector3){ t->axis[0].x * x + t->axis[1].x * y + t->axis[2].x * z,
                      t->axis[0].y * x + t->axis[1].y * y + t->axis[2].y * z,
                      t->axis[0].z * x + t->axis[1].z * y + t->axis[2].z * z };
}

// A point given in a turned box's own frame (offsets from its centre along its axes)
static inline Vector3 TurnedPoint(const MS_TurnedBox *t, float x, float y, float z)
{
    return Vector3Add(t->center, TurnedDir(t, x, y, z));
}

// A turned box from the box as written (its corners before the turn, any order) and its
// rotation about its centre: the record (centre, size, axes) and its world AABB (the min / max of
// its eight turned corners)
static void TurnedBoxMake(BoundingBox written, Vector3 rotationDeg, MS_TurnedBox *t, BoundingBox *aabb)
{
    BoundingBox w = { Vector3Min(written.min, written.max), Vector3Max(written.min, written.max) };
    t->center = Vector3Scale(Vector3Add(w.min, w.max), 0.5f);
    t->size = Vector3Subtract(w.max, w.min);
    t->rotationDeg = rotationDeg;
    Matrix m = QuaternionToMatrix(RotationYpr(rotationDeg));
    t->axis[0] = (Vector3){ m.m0, m.m1, m.m2 };
    t->axis[1] = (Vector3){ m.m4, m.m5, m.m6 };
    t->axis[2] = (Vector3){ m.m8, m.m9, m.m10 };
    Vector3 h = Vector3Scale(t->size, 0.5f);
    *aabb = (BoundingBox){ { INFINITY, INFINITY, INFINITY }, { -INFINITY, -INFINITY, -INFINITY } };
    for (int c = 0; c < 8; c++) {
        Vector3 q = TurnedPoint(t, c & 1 ? h.x : -h.x, c & 2 ? h.y : -h.y, c & 4 ? h.z : -h.z);
        aabb->min = Vector3Min(aabb->min, q);
        aabb->max = Vector3Max(aabb->max, q);
    }
}

// #endregion

// ======================================================================================
// #region Small helpers: box face layouts (a material's `uv_faces`)
// ======================================================================================
// A layout gives each face of a box (+x -x +y -y +z -z, the box's own axes when it is turned) a
// cell number 0..5; faces with the same number share a cell of the texture. k cells (the largest
// number + 1) lie in a grid of ceil(sqrt(k)) columns and ceil(k / cols) rows, cell n at column
// n % cols, row n / cols. A face keeps the orientation R3D's unit cube gives it, inset by a gutter
// of 1/64 cell on each side, so the first few mipmap levels don't bleed from the next cell (a gutter
// of g texels covers about log2(g) + 1 levels; at a distance the smaller levels do mix in the next
// cell at the face's edges).

#define FACE_GUTTER (1.0f / 64.0f)

// A valid layout: every number 0..5, and every number from 0 to the largest one used
static bool FaceCellsValid(const unsigned char cells[6])
{
    int top = 0;
    for (int f = 0; f < 6; f++) {
        if (cells[f] > 5) return false;
        if (cells[f] > top) top = cells[f];
    }
    for (int n = 0; n <= top; n++) {
        bool used = false;
        for (int f = 0; f < 6; f++) used = used || cells[f] == n;
        if (!used) return false;
    }
    return true;
}

// The grid of a layout: cols x rows cells
static void FaceGrid(const unsigned char cells[6], int *cols, int *rows)
{
    int k = 1;
    for (int f = 0; f < 6; f++)
        if (cells[f] + 1 > k) k = cells[f] + 1;
    int c = 1;
    while (c * c < k) c++;
    *cols = c;
    *rows = (k + c - 1) / c;
}

// The rectangle of face f's cell in 0..1 texture space (inset: with the gutter)
static Rectangle FaceCellRect(const unsigned char cells[6], int face, bool inset)
{
    int cols, rows;
    FaceGrid(cells, &cols, &rows);
    int n = face >= 0 && face < 6 ? cells[face] : 0;
    float g = inset ? FACE_GUTTER : 0.0f;
    float cw = 1.0f / (float)cols, ch = 1.0f / (float)rows;
    return (Rectangle){ ((float)(n % cols) + g) * cw, ((float)(n / cols) + g) * ch,
                        (1.0f - 2.0f * g) * cw, (1.0f - 2.0f * g) * ch };
}

// The layout face (+x -x +y -y +z -z = 0..5) of a unit normal along one axis
static int FaceOfNormal(Vector3 n)
{
    int axis = fabsf(n.x) > 0.5f ? 0 : fabsf(n.y) > 0.5f ? 1 : 2;
    float v = axis == 0 ? n.x : axis == 1 ? n.y : n.z;
    return 2 * axis + (v < 0.0f);
}

// R3D's unit cube (24 vertices, 4 per face, in its own face order) with each face's UVs moved into
// its cell of the layout: u' = x + u * w, v' = y + v * h of the face's inset cell rectangle
static void FaceLayoutData(R3D_MeshData *md, const unsigned char cells[6])
{
    for (int f = 0; f < 6; f++) {
        Rectangle r = FaceCellRect(cells, FaceOfNormal(R3D_UnpackNormal(md->vertices[f * 4].normal)), true);
        for (int c = 0; c < 4; c++) {
            R3D_Vertex *vx = &md->vertices[f * 4 + c];
            Vector2 uv = R3D_UnpackTexCoord(vx->texcoord);
            R3D_PackTexCoord(vx->texcoord, (Vector2){ r.x + uv.x * r.width, r.y + uv.y * r.height });
        }
    }
}

// Where on face f (+x -x +y -y +z -z) of a unit cube a point in its local -0.5..0.5 coordinates
// lies, as R3D_GenMeshDataCube maps the face's corners (linear in the two face axes, so exact):
// u along the face's tangent, v up for the side faces. The paint stage's instanced boxes compute
// the same in GLSL (PAINT_STAGE_VERT); PaintCheckCube proves it against R3D's cube at load.
static Vector2 CubeFaceUv(int f, Vector3 l)
{
    switch (f) {
    case 0:  return (Vector2){ 0.5f - l.z, l.y + 0.5f };    // +x
    case 1:  return (Vector2){ l.z + 0.5f, l.y + 0.5f };    // -x
    case 2:  return (Vector2){ l.x + 0.5f, 0.5f - l.z };    // +y
    case 3:  return (Vector2){ l.x + 0.5f, l.z + 0.5f };    // -y
    case 4:  return (Vector2){ l.x + 0.5f, l.y + 0.5f };    // +z
    default: return (Vector2){ 0.5f - l.x, l.y + 0.5f };    // -z
    }
}

// #endregion

// ======================================================================================
// #region Small helpers: the paint layer (docs/painting.md 4)
// ======================================================================================
// What paint is drawn with, apart from the map's statements: atlas files, unwrap sidecars, the
// paint shader's source, and the test whether a model slot's own UVs can carry paint.

#define PAINT_TABLE_W       1024        // keys per row of the paint table (RGBA32F)
#define PAINT_RASTER        1024        // the own-UV test's raster (texels per side)
#define PAINT_SHADER_MAX    16384       // R3D_MAX_SHADER_CODE_LENGTH (R3D's CMakeLists.txt): a surface shader's source
static const char *const PAINT_FACES[6] = { "+x", "-x", "+y", "-y", "+z", "-z" };

// The size of a PNG from its header (IHDR), without decoding it; else of any image raylib reads.
// False when the file can't be read.
static bool ImageFileSize(const char *full, int *w, int *h)
{
    *w = *h = 0;
    FILE *f = fopen(full, "rb");
    if (!f) return false;
    unsigned char b[24];
    size_t n = fread(b, 1, sizeof(b), f);
    fclose(f);
    static const unsigned char SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (n == sizeof(b) && memcmp(b, SIG, 8) == 0 && memcmp(b + 12, "IHDR", 4) == 0) {
        *w = (int)((unsigned)b[16] << 24 | (unsigned)b[17] << 16 | (unsigned)b[18] << 8 | b[19]);
        *h = (int)((unsigned)b[20] << 24 | (unsigned)b[21] << 16 | (unsigned)b[22] << 8 | b[23]);
        return *w > 0 && *h > 0;
    }
    Image img = LoadImage(full);
    *w = img.width;
    *h = img.height;
    UnloadImage(img);
    return *w > 0 && *h > 0;
}

// Premultiplying an sRGB colour byte c by an alpha byte a in linear light, stored sRGB-encoded again:
// PremulLut()[a * 256 + c]. A texture of such texels decodes (sRGB -> linear, before filtering) to
// colour x alpha, which the paint stage adds over the material. a = 255 gives c back, a = 0 gives 0.
// A constant table of the arithmetic, not content: built once, never freed (64 KB).
static unsigned char g_premul[256 * 256];
static atomic_bool g_premulReady;

static const unsigned char *PremulLut(void)
{
    if (atomic_load(&g_premulReady)) return g_premul;
    for (int c = 0; c < 256; c++) {
        float s = (float)c / 255.0f;
        float lin = s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
        for (int a = 0; a < 256; a++) {
            float v = lin * (float)a / 255.0f;
            float e = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
            int q = (int)floorf(e * 255.0f + 0.5f);
            g_premul[a * 256 + c] = (unsigned char)(a == 255 ? c : q < 0 ? 0 : q > 255 ? 255 : q);
        }
    }
    atomic_store(&g_premulReady, true);
    return g_premul;
}

// RGBA8 texels premultiplied in place (PremulLut)
static void PremultiplyRgba8(unsigned char *px, size_t texels)
{
    const unsigned char *lut = PremulLut();
    for (size_t i = 0; i < texels; i++, px += 4) {
        const unsigned char *row = lut + (size_t)px[3] * 256;
        px[0] = row[px[0]];
        px[1] = row[px[1]];
        px[2] = row[px[2]];
    }
}

// A key and a face corner's (or a paint UV's) u, v in a vertex's bone channels: the key's three bytes
// in the bone indices, u and v as unorm16 in the four weights (docs/painting.md 4.4)
static void PaintBones(R3D_Vertex *v, unsigned int key, Vector2 uv)
{
    unsigned int u = (unsigned int)(fminf(fmaxf(uv.x, 0.0f), 1.0f) * 65535.0f + 0.5f);
    unsigned int w = (unsigned int)(fminf(fmaxf(uv.y, 0.0f), 1.0f) * 65535.0f + 0.5f);
    v->boneIndices[0] = (uint8_t)(key & 0xFF);
    v->boneIndices[1] = (uint8_t)(key >> 8 & 0xFF);
    v->boneIndices[2] = (uint8_t)(key >> 16 & 0xFF);
    v->boneIndices[3] = 0;
    v->boneWeights[0] = (uint8_t)(u & 0xFF);
    v->boneWeights[1] = (uint8_t)(u >> 8);
    v->boneWeights[2] = (uint8_t)(w & 0xFF);
    v->boneWeights[3] = (uint8_t)(w >> 8);
}

// The atlas of a model slot drawn with the file's own material: textures/paint/<name>_s<slot>.png,
// <name> the model file's name without its folder and extension, kept to letters, digits, '-' and
// '_' (anything else becomes '_'). False when it doesn't fit.
static bool OwnAtlasPath(const char *modelRel, int slot, char *out, int size)
{
    const char *base = strrchr(modelRel, '/');
    base = base ? base + 1 : modelRel;
    const char *dot = strrchr(base, '.');
    int len = dot && dot > base ? (int)(dot - base) : (int)strlen(base);
    int n = snprintf(out, (size_t)size, "textures/paint/%.*s_s%d.png", len, base, slot);
    if (n < 0 || n >= size) return false;
    for (char *c = out + 15; c < out + 15 + len; c++)
        if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') *c = '_';
    return true;
}

// A model file's unwrap sidecar: its path with the extension replaced by .paintuv
// (models/barrel.glb -> models/barrel.paintuv). False when it doesn't fit.
static bool UnwrapSidecarPath(const char *modelRel, char *out, int size)
{
    const char *slash = strrchr(modelRel, '/');
    const char *dot = strrchr(modelRel, '.');
    int len = dot && (!slash || dot > slash + 1) ? (int)(dot - modelRel) : (int)strlen(modelRel);
    int n = snprintf(out, (size_t)size, "%.*s.paintuv", len, modelRel);
    return n >= 0 && n < size;
}

static void UnwrapFree(Unwrap *u)
{
    for (int m = 0; u->meshes && m < u->meshCount; m++) {
        free(u->meshes[m].source);
        free(u->meshes[m].uv);
        free(u->meshes[m].indices);
    }
    free(u->meshes);
    memset(u, 0, sizeof(*u));
}

// Little-endian reads from a checked buffer
static uint32_t Le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t Le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

static const unsigned char UNWRAP_MAGIC[10] = { 'M', 'S', 'P', 'A', 'I', 'N', 'T', 'U', 'V', 0x1A };

// Reads a .paintuv sidecar (docs/painting.md 4.5): "MSPAINTUV" 0x1A, uint32 version 1, uint32
// meshCount, uint64 the model file's FNV-1a 64, then per mesh uint32 slot, vertexCount, indexCount,
// vertexCount x { uint32 source, uint16 u, uint16 v }, indexCount x uint32 index; little-endian.
// Checked: the sizes, whole triangles, every index below the mesh's vertex count. False (nothing
// allocated) when it isn't such a file or out of memory.
static bool UnwrapRead(const char *path, Unwrap *out)
{
    memset(out, 0, sizeof(*out));
    int size = 0;
    unsigned char *b = (unsigned char *)ReadWholeFile(path, MS_MAX_COMPILED_BYTES, &size, false);
    if (!b) return false;
    bool ok = size >= 30 && memcmp(b, UNWRAP_MAGIC, 10) == 0 && Le32(b + 10) == 1;
    uint32_t meshes = ok ? Le32(b + 14) : 0;
    ok = ok && meshes <= MS_MAX_OBJECTS;
    if (ok) {
        out->fileHash = (unsigned long long)Le32(b + 18) | (unsigned long long)Le32(b + 22) << 32;
        out->meshes = calloc(meshes > 0 ? meshes : 1, sizeof(UnwrapMesh));
        ok = out->meshes != NULL;
    }
    size_t at = 26;
    for (uint32_t m = 0; ok && m < meshes; m++) {
        if ((size_t)size - at < 12) { ok = false; break; }
        uint32_t slot = Le32(b + at), vc = Le32(b + at + 4), ic = Le32(b + at + 8);
        at += 12;
        if (slot > MS_MAX_OBJECTS || ic % 3 != 0 ||
            (uint64_t)vc * 8 + (uint64_t)ic * 4 > (uint64_t)size - at) { ok = false; break; }
        UnwrapMesh *M = &out->meshes[m];
        out->meshCount = (int)m + 1;
        M->slot = (int)slot;
        M->vertexCount = (int)vc;
        M->indexCount = (int)ic;
        M->source = malloc(sizeof(unsigned int) * (vc > 0 ? vc : 1));
        M->uv = malloc(sizeof(unsigned short) * 2 * (vc > 0 ? vc : 1));
        M->indices = malloc(sizeof(unsigned int) * (ic > 0 ? ic : 1));
        if (!M->source || !M->uv || !M->indices) { ok = false; break; }
        for (uint32_t k = 0; k < vc; k++, at += 8) {
            M->source[k] = Le32(b + at);
            M->uv[2 * k] = Le16(b + at + 4);
            M->uv[2 * k + 1] = Le16(b + at + 6);
        }
        for (uint32_t k = 0; k < ic && ok; k++, at += 4) {
            M->indices[k] = Le32(b + at);
            ok = M->indices[k] < vc;
        }
    }
    ok = ok && at == (size_t)size;
    free(b);
    if (!ok) UnwrapFree(out);
    return ok;
}

// FNV-1a 64 of a file's bytes; false when it can't be read
static bool FileHash(const char *path, unsigned long long *hash)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned long long h = FNV_OFFSET;
    unsigned char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) h = Fnv1a64Update(h, buf, got);
    fclose(f);
    *hash = h;
    return true;
}

// Whether an unwrap fits a model as loaded: a mesh per mesh, each with the mesh's slot, its
// triangles in the same number (the order is the mesh's), every source vertex inside the mesh
static bool UnwrapMatches(const Unwrap *u, const R3D_Model *m)
{
    if (!m->meshData || !m->meshMaterials || u->meshCount != m->meshCount) return false;
    for (int i = 0; i < u->meshCount; i++) {
        const UnwrapMesh *M = &u->meshes[i];
        const R3D_MeshData *md = &m->meshData[i];
        int tris = md->indexCount > 0 ? md->indexCount / 3 : md->vertexCount / 3;
        if (M->slot != m->meshMaterials[i] || M->indexCount != 3 * tris) return false;
        for (int k = 0; k < M->vertexCount; k++)
            if (M->source[k] >= (unsigned int)md->vertexCount) return false;
    }
    return true;
}

// --- The paint stage (docs/painting.md 4.3). A painted material's shader is its own surface shader
// (or an empty one) rewritten so that a last stage composites the material's paint atlas over what
// the material computed: comments stripped, `varying`s declared in place per stage, vertex() and
// fragment() renamed and kept to their stage, then the stage below. R3D's parser copies global code
// into both stages and skips any line starting with flat / smooth / noperspective, so interpolation
// qualifiers are written through MS_INTERP().

static const char PAINT_STAGE_HEAD[] =
    "uniform sampler2D ms_paint;\n"
    "uniform sampler2D ms_paintTable;\n";

// The vertex stage: which paint key and where in its domain. INSTANCE_CUSTOM.y tells the draw kinds
// apart: 1 an instanced box (key x + face, uv from the unit cube), 2 a model instance (key x + the
// slot in bone index 0, uv in the bone weights as two unorm16), 0 a merged box mesh (the key in the
// four bone indices, uv in the bone weights). Key 0 holds a zero rectangle: no paint.
static const char PAINT_STAGE_VERT[] =
    "#ifdef STAGE_VERT\n"
    "MS_INTERP(flat) out vec4 ms_rect;\n"
    "out vec2 ms_uv;\n"
    "void vertex() {\n"
    "%s"
    "#if !defined(DEPTH) && !defined(DEPTH_CUBE)\n"
    "int ms_key = 0;\n"
    "vec2 ms_p;\n"
    "float ms_kind = INSTANCE_CUSTOM.y;\n"
    "if (ms_kind > 0.5 && ms_kind < 1.5) {\n"
    "vec3 ms_n = aNormal, ms_q = aPosition;\n"
    "int ms_f;\n"
    "if (abs(ms_n.x) > 0.5) { ms_f = ms_n.x > 0.0 ? 0 : 1; ms_p = vec2(ms_n.x > 0.0 ? 0.5 - ms_q.z : ms_q.z + 0.5, ms_q.y + 0.5); }\n"
    "else if (abs(ms_n.y) > 0.5) { ms_f = ms_n.y > 0.0 ? 2 : 3; ms_p = vec2(ms_q.x + 0.5, ms_n.y > 0.0 ? 0.5 - ms_q.z : ms_q.z + 0.5); }\n"
    "else { ms_f = ms_n.z > 0.0 ? 4 : 5; ms_p = vec2(ms_n.z > 0.0 ? ms_q.x + 0.5 : 0.5 - ms_q.x, ms_q.y + 0.5); }\n"
    "if (INSTANCE_CUSTOM.x > 0.5) ms_key = int(INSTANCE_CUSTOM.x + 0.5) + ms_f;\n"
    "} else {\n"
    "vec4 ms_w = floor(aBoneWeights * 255.0 + 0.5);\n"
    "ms_p = vec2(ms_w.x + ms_w.y * 256.0, ms_w.z + ms_w.w * 256.0) / 65535.0;\n"
    "if (ms_kind > 1.5) { if (INSTANCE_CUSTOM.x > 0.5) ms_key = int(INSTANCE_CUSTOM.x + 0.5) + aBoneIndices.x; }\n"
    "else ms_key = aBoneIndices.x + aBoneIndices.y * 256 + aBoneIndices.z * 65536;\n"
    "}\n"
    "ms_rect = texelFetch(ms_paintTable, ivec2(ms_key % 1024, ms_key / 1024), 0);\n"
    "ms_uv = ms_p;\n"
    "#endif\n"
    "}\n"
    "#endif\n";

// The fragment stage: paint is a matte coat over the material's albedo, covering its emission and
// metalness by its alpha; on a blended (forward) material it adds to the alpha. The atlas is
// premultiplied; the uv stays half a texel inside the rectangle. (GEOMETRY_PREPASS is not a depth
// variant: with the depth pre-pass it is also the shading pass, R3D patch 6.)
static const char PAINT_STAGE_FRAG[] =
    "#ifdef STAGE_FRAG\n"
    "MS_INTERP(flat) in vec4 ms_rect;\n"
    "in vec2 ms_uv;\n"
    "void fragment() {\n"
    "%s"
    "#if !defined(DEPTH) && !defined(DEPTH_CUBE)\n"
    "if (ms_rect.z > ms_rect.x) {\n"
    "vec2 ms_h = 0.5 / vec2(textureSize(ms_paint, 0));\n"
    "vec4 ms_c = texture(ms_paint, clamp(mix(ms_rect.xy, ms_rect.zw, ms_uv), ms_rect.xy + ms_h, ms_rect.zw - ms_h));\n"
    "ALBEDO = ALBEDO * (1.0 - ms_c.a) + ms_c.rgb;\n"
    "EMISSION *= 1.0 - ms_c.a;\n"
    "METALNESS *= 1.0 - ms_c.a;\n"
    "#ifdef FORWARD\n"
    "ALPHA += ms_c.a * (1.0 - ALPHA);\n"
    "#endif\n"
    "}\n"
    "#endif\n"
    "}\n"
    "#endif\n";

// A growing text (the generated source)
typedef struct { char *s; int n, cap; bool oom; } Text;

static void TextPut(Text *t, const char *s, int len)
{
    if (t->oom || !MS_RESERVE(t->s, t->cap, t->n + len + 1)) { t->oom = true; return; }
    memcpy(t->s + t->n, s, (size_t)len);
    t->n += len;
    t->s[t->n] = '\0';
}

static void TextStr(Text *t, const char *s) { TextPut(t, s, (int)strlen(s)); }

static bool IsIdent(char c) { return isalnum((unsigned char)c) || c == '_'; }

// The word w at s[i] (not inside a longer identifier), followed by whitespace when `space`
static bool WordAt(const char *s, int i, const char *w, bool space)
{
    size_t n = strlen(w);
    if ((i > 0 && IsIdent(s[i - 1])) || strncmp(s + i, w, n) != 0) return false;
    return space ? isspace((unsigned char)s[i + (int)n]) : !IsIdent(s[i + (int)n]);
}

// The source without comments (a block comment becomes one space, as the preprocessor sees it),
// each line without its leading blanks, and without empty lines: smaller, the same GLSL
static bool StripComments(const char *src, Text *out)
{
    bool lineStart = true;
    for (int i = 0; src[i]; ) {
        if (src[i] == '/' && src[i + 1] == '/') {
            while (src[i] && src[i] != '\n') i++;
            continue;
        }
        if (src[i] == '/' && src[i + 1] == '*') {
            const char *e = strstr(src + i + 2, "*/");
            i = e ? (int)(e - src) + 2 : (int)strlen(src);
            if (!lineStart) TextPut(out, " ", 1);
            continue;
        }
        if (lineStart && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r' || src[i] == '\n')) { i++; continue; }
        lineStart = src[i] == '\n';
        TextPut(out, &src[i++], 1);
    }
    return !out->oom;
}

// How many sampler uniforms a source declares (R3D allows R3D_MAX_SHADER_SAMPLERS per material)
static int CountSamplers(const char *s)
{
    int n = 0;
    for (int i = 0; s[i]; i++) {
        if (!WordAt(s, i, "uniform", true)) continue;
        int k = i + 7;
        while (isspace((unsigned char)s[k])) k++;
        n += strncmp(s + k, "sampler", 7) == 0;
    }
    return n;
}

// The paint version of a surface shader's source (src NULL: of an empty shader), heap; NULL when out
// of memory or when the source declares more than 2 samplers (the stage needs 2 of R3D's 4).
static char *PaintShaderSource(const char *src)
{
    Text in = { 0 }, out = { 0 };
    if (src && !StripComments(src, &in)) { free(in.s); return NULL; }
    const char *s = in.s ? in.s : "";
    if (CountSamplers(s) > 2) { free(in.s); return NULL; }

    // Which entry points the source has: without vertex() its code belongs to the fragment stage
    // only, without fragment() to the vertex stage (R3D gives a stage without its entry no user code)
    bool hasV = false, hasF = false;
    for (int i = 0; s[i]; i++)
        if (WordAt(s, i, "void", true)) {
            int k = i + 4;
            while (isspace((unsigned char)s[k])) k++;
            hasV = hasV || WordAt(s, k, "vertex", false);
            hasF = hasF || WordAt(s, k, "fragment", false);
        }
    TextStr(&out, "#define MS_INTERP(q) q\n");
    if (src && (hasV != hasF)) TextStr(&out, hasV ? "#ifdef STAGE_VERT\n" : "#ifdef STAGE_FRAG\n");

    for (int i = 0; s[i]; ) {
        // vertex() / fragment(): renamed, under their stage
        if (WordAt(s, i, "void", true)) {
            int k = i + 4;
            while (isspace((unsigned char)s[k])) k++;
            bool v = WordAt(s, k, "vertex", false), f = WordAt(s, k, "fragment", false);
            const char *open = (v || f) ? strchr(s + k, '{') : NULL;
            if (open) {
                const char *e = open;
                for (int depth = 0; *e; e++) {
                    if (*e == '{') depth++;
                    else if (*e == '}' && --depth == 0) { e++; break; }
                }
                TextStr(&out, v ? "\n#ifdef STAGE_VERT\nvoid ms_userVertex() " : "\n#ifdef STAGE_FRAG\nvoid ms_userFragment() ");
                TextPut(&out, open, (int)(e - open));
                TextStr(&out, "\n#endif\n");
                i = (int)(e - s);
                continue;
            }
        }
        // [flat | smooth | noperspective] varying T name; -> out (vertex stage) / in (fragment stage)
        const char *q = WordAt(s, i, "flat", true) ? "flat" : WordAt(s, i, "smooth", true) ? "smooth"
                      : WordAt(s, i, "noperspective", true) ? "noperspective" : NULL;
        int k = i + (q ? (int)strlen(q) : 0);
        while (q && isspace((unsigned char)s[k])) k++;
        if (WordAt(s, k, "varying", true)) {
            const char *rest = s + k + 7, *semi = strchr(rest, ';');
            if (semi) {
                int len = (int)(semi - rest) + 1;
                for (int st = 0; st < 2; st++) {
                    TextStr(&out, st == 0 ? "\n#ifdef STAGE_VERT\n" : "\n#else\n");
                    if (q) { TextStr(&out, "MS_INTERP("); TextStr(&out, q); TextStr(&out, ") "); }
                    TextStr(&out, st == 0 ? "out" : "in");
                    TextPut(&out, rest, len);
                }
                TextStr(&out, "\n#endif\n");
                i = (int)(semi - s) + 1;
                continue;
            }
        }
        TextPut(&out, &s[i++], 1);
    }
    if (src && (hasV != hasF)) TextStr(&out, "\n#endif\n");

    // The stage (its entry points call the source's, renamed)
    TextStr(&out, "\n");
    TextStr(&out, PAINT_STAGE_HEAD);
    char head[64];
    for (int st = 0; st < 2; st++) {
        const char *fmt = st == 0 ? PAINT_STAGE_VERT : PAINT_STAGE_FRAG;
        const char *at = strstr(fmt, "%s");
        const char *call = st == 0 ? "ms_userVertex();\n" : "ms_userFragment();\n";
        snprintf(head, sizeof(head), "%s", (st == 0 ? hasV : hasF) ? call : "");
        TextPut(&out, fmt, (int)(at - fmt));
        TextStr(&out, head);
        TextStr(&out, at + 2);
    }
    free(in.s);
    if (out.oom) { free(out.s); return NULL; }
    return out.s;
}

// --- Whether a model slot's own UVs can carry paint (docs/painting.md 4.5, 7.3): every texture
// coordinate of its triangles inside 0..1, and no texel of a PAINT_RASTER^2 raster claimed twice:
// conservatively by two UV islands (triangles joined over shared UV edges), or at its centre by
// two triangles (which catches mirrored halves joined at their seam)

typedef struct { float u0, v0, u1, v1; int tri; } UvEdge;

static int CompareUvEdges(const void *pa, const void *pb)
{
    const UvEdge *a = pa, *b = pb;
    int c = memcmp(a, b, 4 * sizeof(float));
    return c ? c : (a->tri > b->tri) - (a->tri < b->tri);
}

static int UfFind(int *parent, int i)
{
    while (parent[i] != i) i = parent[i] = parent[parent[i]];
    return i;
}

// Does the triangle (a, b, c) overlap the square [x0, x1] x [y0, y1]? (separating axes: the
// square's two and the triangle's three edge normals; its bounds were tested already)
static bool TriOverlapsSquare(const Vector2 *t, float x0, float y0, float x1, float y1)
{
    for (int e = 0; e < 3; e++) {
        Vector2 a = t[e], b = t[(e + 1) % 3], c = t[(e + 2) % 3];
        float nx = a.y - b.y, ny = b.x - a.x;               // a normal of edge a-b
        float side = nx * (c.x - a.x) + ny * (c.y - a.y);   // the third vertex's side
        if (side == 0.0f) return false;                     // degenerate
        float s = side > 0.0f ? 1.0f : -1.0f, best = -INFINITY;
        for (int k = 0; k < 4; k++) {
            float x = k & 1 ? x1 : x0, y = k & 2 ? y1 : y0;
            float v = s * (nx * (x - a.x) + ny * (y - a.y));
            if (v > best) best = v;
        }
        if (best < 0.0f) return false;                      // the square lies wholly outside this edge
    }
    return true;
}

// How a slot maps onto paint (MS_PaintUvKind's values, for the game build too)
enum { PUV_OWN = 0, PUV_UNWRAP, PUV_NEEDS_UNWRAP, PUV_SKINNED };

// PUV_OWN or PUV_NEEDS_UNWRAP for slot `slot` of a loaded model with its CPU mesh data: the
// triangles of the meshes drawn with that slot. No texture coordinates (all 0, what R3D's importer
// leaves), or out of memory: needs an unwrap (the safe answer).
static int OwnUvKind(const R3D_Model *m, int slot)
{
    if (!m->meshData || !m->meshMaterials) return PUV_NEEDS_UNWRAP;
    int n = 0;
    for (int i = 0; i < m->meshCount; i++) {
        const R3D_MeshData *md = &m->meshData[i];
        if (m->meshMaterials[i] == slot && m->meshes[i].primitiveType == R3D_PRIMITIVE_TRIANGLES)
            n += md->indexCount > 0 ? md->indexCount / 3 : md->vertexCount / 3;
    }
    if (n == 0) return PUV_NEEDS_UNWRAP;
    Vector2 *tri = malloc(sizeof(Vector2) * 3 * (size_t)n);
    UvEdge *edges = malloc(sizeof(UvEdge) * 3 * (size_t)n);
    int *parent = malloc(sizeof(int) * (size_t)n);
    int *owner = calloc((size_t)PAINT_RASTER * PAINT_RASTER, sizeof(int));
    unsigned char *centre = calloc((size_t)PAINT_RASTER * PAINT_RASTER, 1);
    bool own = tri && edges && parent && owner && centre, any = false;

    // The slot's triangles in file order, every coordinate inside 0..1 (and not all of them 0)
    int t = 0;
    for (int i = 0; own && i < m->meshCount; i++) {
        const R3D_MeshData *md = &m->meshData[i];
        if (m->meshMaterials[i] != slot || m->meshes[i].primitiveType != R3D_PRIMITIVE_TRIANGLES) continue;
        int tris = md->indexCount > 0 ? md->indexCount / 3 : md->vertexCount / 3;
        for (int k = 0; k < tris && own; k++, t++)
            for (int c = 0; c < 3; c++) {
                unsigned int v = md->indexCount > 0 ? md->indices[3 * k + c] : (unsigned int)(3 * k + c);
                Vector2 uv = v < (unsigned int)md->vertexCount ? R3D_UnpackTexCoord(md->vertices[v].texcoord) : (Vector2){ -1, -1 };
                own = own && uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f;
                any = any || uv.x != 0.0f || uv.y != 0.0f;
                tri[3 * t + c] = uv;
            }
    }
    own = own && any;

    // Islands: triangles that share a UV edge (both ends exactly equal) are one island
    for (int i = 0; own && i < n; i++) {
        parent[i] = i;
        for (int e = 0; e < 3; e++) {
            Vector2 a = tri[3 * i + e], b = tri[3 * i + (e + 1) % 3];
            bool swap = a.x > b.x || (a.x == b.x && a.y > b.y);
            edges[3 * i + e] = swap ? (UvEdge){ b.x, b.y, a.x, a.y, i } : (UvEdge){ a.x, a.y, b.x, b.y, i };
        }
    }
    if (own) qsort(edges, 3 * (size_t)n, sizeof(UvEdge), CompareUvEdges);
    for (int e = 1; own && e < 3 * n; e++)
        if (memcmp(&edges[e], &edges[e - 1], 4 * sizeof(float)) == 0) {
            int a = UfFind(parent, edges[e].tri), b = UfFind(parent, edges[e - 1].tri);
            if (a != b) parent[a] = b;
        }

    // The raster: a texel touched by two islands, or whose centre two triangles cover, is shared
    const float R = (float)PAINT_RASTER;
    for (int i = 0; own && i < n; i++) {
        Vector2 p[3];
        float mnx = INFINITY, mny = INFINITY, mxx = -INFINITY, mxy = -INFINITY;
        for (int c = 0; c < 3; c++) {
            p[c] = (Vector2){ tri[3 * i + c].x * R, tri[3 * i + c].y * R };
            mnx = fminf(mnx, p[c].x); mxx = fmaxf(mxx, p[c].x);
            mny = fminf(mny, p[c].y); mxy = fmaxf(mxy, p[c].y);
        }
        float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
        if (area == 0.0f) continue;
        int island = UfFind(parent, i) + 1;
        int x0 = (int)floorf(mnx), x1 = (int)floorf(mxx), y0 = (int)floorf(mny), y1 = (int)floorf(mxy);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > PAINT_RASTER - 1) x1 = PAINT_RASTER - 1;
        if (y1 > PAINT_RASTER - 1) y1 = PAINT_RASTER - 1;
        for (int y = y0; own && y <= y1; y++)
            for (int x = x0; own && x <= x1; x++) {
                if (!TriOverlapsSquare(p, (float)x, (float)y, (float)x + 1.0f, (float)y + 1.0f)) continue;
                int *o = &owner[(size_t)y * PAINT_RASTER + x];
                if (*o && *o != island) own = false;
                *o = island;
                // the centre strictly inside (a centre on a shared edge belongs to neither)
                float cx = (float)x + 0.5f, cy = (float)y + 0.5f, prev = 0.0f;
                bool inside = true;
                for (int e = 0; e < 3 && inside; e++) {
                    Vector2 a = p[e], b = p[(e + 1) % 3];
                    float v = (b.x - a.x) * (cy - a.y) - (b.y - a.y) * (cx - a.x);
                    inside = v != 0.0f && (prev == 0.0f || (v > 0.0f) == (prev > 0.0f));
                    prev = v;
                }
                if (inside && centre[(size_t)y * PAINT_RASTER + x]++) own = false;
            }
    }
    free(tri); free(edges); free(parent); free(owner); free(centre);
    return own ? PUV_OWN : PUV_NEEDS_UNWRAP;
}

// --- The rules between paint entries (docs/painting.md 4.2), one check for the text parser and
// compiled maps: no two rectangles of one atlas share a texel, and no face or slot of an object is in
// two entries of one material. (A material's atlas is its own; an own material's atlas is per model
// file and slot: ownAtlases[].)

typedef struct { long long a, b; int idx; } PaintSortItem;

static int ComparePaintItems(const void *pa, const void *pb)
{
    const PaintSortItem *x = pa, *y = pb;
    if (x->a != y->a) return x->a < y->a ? -1 : 1;
    if (x->b != y->b) return x->b < y->b ? -1 : 1;
    return (x->idx > y->idx) - (x->idx < y->idx);
}

// 0: the entries keep the rules; 1: entries *ea and *eb overlap in their atlas; 2: they name the
// same face or slot of one object in one material; -1: out of memory
static int PaintConflict(const SceneData *d, int *ea, int *eb)
{
    int n = d->paintCount;
    if (n < 2) return 0;
    PaintSortItem *it = malloc(sizeof(PaintSortItem) * (size_t)n);
    if (!it) return -1;
    int result = 0;

    // Rectangles: by atlas, then x; each against the following ones that start left of its end
    for (int i = 0; i < n; i++) {
        const PaintDef *e = &d->paints[i];
        it[i] = (PaintSortItem){ e->material >= 0 ? e->material : (long long)d->materialCount + e->ownAtlas, e->x, i };
    }
    qsort(it, (size_t)n, sizeof(PaintSortItem), ComparePaintItems);
    for (int i = 0; i < n && !result; i++) {
        const PaintDef *e = &d->paints[it[i].idx];
        for (int j = i + 1; j < n && it[j].a == it[i].a && it[j].b < (long long)e->x + e->w && !result; j++) {
            const PaintDef *f = &d->paints[it[j].idx];
            if (f->y < e->y + e->h && e->y < f->y + f->h) {
                result = 1;
                *ea = it[i].idx < it[j].idx ? it[i].idx : it[j].idx;
                *eb = it[i].idx < it[j].idx ? it[j].idx : it[i].idx;
            }
        }
    }

    // Faces and slots: by (material, kind, object); neighbours in a run share no face or slot
    for (int i = 0; i < n && !result; i++) {
        const PaintDef *e = &d->paints[i];
        it[i] = (PaintSortItem){ ((long long)(e->material + 1) << 1) | e->kind, e->object, i };
    }
    if (!result) qsort(it, (size_t)n, sizeof(PaintSortItem), ComparePaintItems);
    for (int i = 0; i < n && !result; i++)
        for (int j = i + 1; j < n && it[j].a == it[i].a && it[j].b == it[i].b && !result; j++) {
            const PaintDef *e = &d->paints[it[i].idx], *f = &d->paints[it[j].idx];
            if (e->kind == PAINT_BOX ? (e->faces & f->faces) != 0 : e->slot == f->slot) {
                result = 2;
                *ea = it[i].idx < it[j].idx ? it[i].idx : it[j].idx;
                *eb = it[i].idx < it[j].idx ? it[j].idx : it[i].idx;
            }
        }
    free(it);
    return result;
}

// The own atlas of model file `file`'s slot `slot` in d->ownAtlases, -1 when there is none
static int OwnAtlasFind(const SceneData *d, int file, int slot)
{
    for (int i = 0; i < d->ownAtlasCount; i++)
        if (d->ownAtlases[i].file == file && d->ownAtlases[i].slot == slot) return i;
    return -1;
}

// The own atlas of model file `file`'s slot `slot` in d->ownAtlases (added when missing: its path
// from the file's name, OwnAtlasPath); -1 when out of memory or the path doesn't fit
static int OwnAtlasOf(SceneData *d, int file, int slot)
{
    int found = OwnAtlasFind(d, file, slot);
    if (found >= 0) return found;
    char rel[MS_PATH_MAX];
    if (!OwnAtlasPath(d->modelFiles[file].path, slot, rel, sizeof(rel))) return -1;
    char *copy = MS_StrDup(rel);
    OwnAtlas *a = copy ? MS_PUSH(d->ownAtlases, d->ownAtlasCount, d->ownAtlasCap) : NULL;
    if (!a) { free(copy); return -1; }
    *a = (OwnAtlas){ .file = file, .slot = slot, .rel = copy };
    return d->ownAtlasCount - 1;
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

// Writes "<file>:<line>: <message>" ("<file>: <message>" outside a line) into the parser's
// last error (ErrPutV); returns false, so a check can `return FailF(…)`
static bool FailF(Parser *P, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ErrPutV(false, P->path, P->line, fmt, ap);
    va_end(ap);
    return false;
}

// FailF with at most one string argument (NULL: none)
static bool Fail(Parser *P, const char *fmt, const char *arg)
{
    return FailF(P, fmt, arg ? arg : "");
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

// Tokens into a running hash (each with its kind, quoted or not)
static unsigned long long HashTokens(unsigned long long h, const Token *tok, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned char q = tok[i].quoted ? 1 : 2;
        h = Fnv1a64Update(h, &q, 1);
        h = Fnv1a64Update(h, tok[i].s, strlen(tok[i].s) + 1);
    }
    return h;
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

// The image provided for package path `rel` (MS_PaintAtlasProvide), NULL if none (or no scene)
static const Image *ProvidedImage(const MS_Private *p, const char *rel)
{
    for (int i = 0; p && i < p->providedCount; i++)
        if (strcmp(p->provided[i].rel, rel) == 0) return &p->provided[i].img;
    return NULL;
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
    // (a live edit may name a texture that exists only in memory so far: MS_PaintAtlasProvide)
    if (!FileExists(full) && !ProvidedImage(P->d->live, s))
        return Fail(P, "file not found: \"%s\" (paths are relative to the package folder)", s);
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
    char list[256];     // the table's names (a few short keywords); the token goes in whole
    int n = 0;
    list[0] = '\0';
    for (int i = 0; i < count && n < (int)sizeof(list); i++) n += snprintf(list + n, sizeof(list) - n, " %s", names[i]);
    FailF(P, "bad %s '%s' (expected%s)", what, t.s, list);
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
    if (!MS_RESERVE(d->boxes, d->boxCap, d->boxCount + 1) || !MS_RESERVE(d->boxRefs, d->boxRefCap, d->boxCount + 1) ||
        (d->boxTurned && !MS_RESERVE(d->boxTurned, d->boxTurnedCap, d->boxCount + 1))) {
        P->outOfMemory = true;
        return false;
    }
    if (d->boxTurned) d->boxTurned[d->boxCount] = -1;
    d->boxes[d->boxCount] = b;
    d->boxRefs[d->boxCount++] = P->cur;
    P->cur.part++;
    return true;
}

// A box with a nonzero rotation: its AABB as the box (Box), plus its turned-box record
static bool TurnedBox(Parser *P, BoundingBox written, Vector3 rotationDeg)
{
    SceneData *d = P->d;
    MS_TurnedBox t;
    BoundingBox a;
    TurnedBoxMake(written, rotationDeg, &t, &a);
    if (!isfinite(t.size.x) || !isfinite(t.size.y) || !isfinite(t.size.z) || !isfinite(t.center.x) ||
        !isfinite(t.center.y) || !isfinite(t.center.z)) {
        P->badNumber = true;
        return false;
    }
    if (!d->boxTurned) {        // the first one: every box before it is axis-aligned
        if (!MS_RESERVE(d->boxTurned, d->boxTurnedCap, d->boxCount + 1)) { P->outOfMemory = true; return false; }
        for (int i = 0; i < d->boxCount; i++) d->boxTurned[i] = -1;
    }
    if (!MS_RESERVE(d->turned, d->turnedCap, d->turnedCount + 1) ||
        !MS_RESERVE(d->turnedWritten, d->turnedWrittenCap, d->turnedCount + 1)) { P->outOfMemory = true; return false; }
    if (!Box(P, a.min.x, a.min.y, a.min.z, a.max.x, a.max.y, a.max.z)) return false;
    d->boxTurned[d->boxCount - 1] = d->turnedCount;
    d->turnedWritten[d->turnedCount] = (BoundingBox){ Vector3Min(written.min, written.max), Vector3Max(written.min, written.max) };
    d->turned[d->turnedCount++] = t;
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
    // `rot yaw pitch roll` after a box's or block's numbers (format 2): turned about its centre
    Vector3 rot = { 0 };
    if (argc > def->argc && strcmp(args[def->argc].s, "rot") == 0) {
        if (def->argc != 6) return Fail(P, "%s can't turn (rot is for box and block lines)", def->name);
        if (P->d->formatVersion < 2) return Fail(P, "rot on a %s needs map format 2: add 'monolith 2' as the first line", def->name);
        if (argc != def->argc + 4) return Fail(P, "rot takes 3 numbers: rot yawDeg pitchDeg rollDeg", NULL);
        for (int i = 0; i < 3; i++)
            if (!NumF(P, args[def->argc + 1 + i], &((float *)&rot)[i])) return false;
        argc = def->argc;
    }
    bool turned = rot.x != 0.0f || rot.y != 0.0f || rot.z != 0.0f;     // rot 0 0 0: the box without rot
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
            default:
                if (turned) {
                    // the box as written: Box's corners, or Block's (the same float math)
                    BoundingBox w = def == &PRIMS[0]
                        ? (BoundingBox){ { p[0], p[1], p[2] }, { p[3], p[4], p[5] } }
                        : (BoundingBox){ { p[0] - p[3] * 0.5f, p[1], p[2] - p[5] * 0.5f }, { p[0] + p[3] * 0.5f, p[1] + p[4], p[2] + p[5] * 0.5f } };
                    ok = TurnedBox(P, w, rot);
                } else ok = (def == &PRIMS[0]) ? Box(P, p[0], p[1], p[2], p[3], p[4], p[5])
                                               : Block(P, p[0], p[1], p[2], p[3], p[4], p[5]);
                break;
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
    if (!MS_RESERVE(d->matLookHash, d->matLookHashCap, d->materialCount + 1) ||
        !MS_RESERVE(d->matShapeHash, d->matShapeHashCap, d->materialCount + 1) ||
        !MS_RESERVE(d->matPaintlessHash, d->matPaintlessHashCap, d->materialCount + 1)) {
        free(copy);
        return -1;
    }
    int mi = d->materialCount++;
    d->materials[mi] = (MS_Material){ .name = copy, .material = R3D_MATERIAL_BASE };
    d->matDefs[mi] = (MatDef){ 0 };
    d->matLookHash[mi] = d->matShapeHash[mi] = d->matPaintlessHash[mi] = FNV_OFFSET;
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
    // (each line starts with its keyword and name in both hashes, so two lines never hash as one)
    d->matLookHash[mi] = HashTokens(d->matLookHash[mi], tok, 2);
    d->matShapeHash[mi] = HashTokens(d->matShapeHash[mi], tok, 2);
    d->matPaintlessHash[mi] = HashTokens(d->matPaintlessHash[mi], tok, 2);

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
        } else if (strcmp(k, "prepass") == 0) {
            bool on;
            ONE_WORD();
            if (!OnOff(P, v[0], &on)) return false;
            D->noPrepass = !on;
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
        } else if (strcmp(k, "uv_faces") == 0) {
            NEED(6);
            unsigned char cells[6];
            for (int f = 0; f < 6; f++) {
                int c;
                if (!NumI(P, v[f], 0, 5, &c)) return false;
                cells[f] = (unsigned char)c;
            }
            if (!FaceCellsValid(cells))
                return Fail(P, "uv_faces: the cell numbers must use every number from 0 to the largest one (e.g. 0 1 2 3 4 5, "
                               "0 0 1 1 2 2)", NULL);
            memcpy(D->uvFaces, cells, sizeof(cells));
            D->hasFaces = true;
        } else if (strcmp(k, "filter") == 0) {
            static const char *const FI[] = { "trilinear", "nearest" };   // = TEX_*
            int e;
            ONE_WORD();
            if (!Keyword(P, v[0], FI, 2, "texture filter", &e)) return false;
            D->filter = (unsigned char)e;
        } else if (strcmp(k, "paint") == 0) {
            // The paint layer's atlas (docs/painting.md 4.1): used once a `paint` entry draws with it
            ONE_WORD();
            if (!PackagePath(P, v[0], full, sizeof(full))) return false;
            char *copy;
            if (!Dup(P, v[0].s, &copy)) return false;
            free(D->paint);
            D->paint = copy;
            D->paintLine = P->line;
        } else if (strcmp(k, "paint_filter") == 0) {
            static const char *const PF[] = { "trilinear", "nearest" };   // = TEX_*
            int e;
            ONE_WORD();
            if (!Keyword(P, v[0], PF, 2, "paint filter", &e)) return false;
            D->paintFilter = (unsigned char)e;
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
        // The key into the material's hashes (the shape hash leaves out what a live edit can change
        // without a full upload: the face layout, the filter, the albedo map and the paint keys; the
        // paintless hash leaves out only the paint keys, whose change rebuilds no mesh)
        bool paintKey = strcmp(k, "paint") == 0 || strcmp(k, "paint_filter") == 0;
        d->matLookHash[mi] = HashTokens(d->matLookHash[mi], &tok[t], 1 + used);
        if (strcmp(k, "uv_faces") != 0 && strcmp(k, "filter") != 0 && strcmp(k, "albedo_map") != 0 && !paintKey)
            d->matShapeHash[mi] = HashTokens(d->matShapeHash[mi], &tok[t], 1 + used);
        if (!paintKey) d->matPaintlessHash[mi] = HashTokens(d->matPaintlessHash[mi], &tok[t], 1 + used);
        t += 1 + used;
    }
    // A face layout puts each face in a cell of the texture: tiling would cross the cells
    if (D->hasFaces && (m->uvScale.x != 1.0f || m->uvScale.y != 1.0f || m->uvOffset.x != 0.0f || m->uvOffset.y != 0.0f))
        return Fail(P, "uv_faces needs uv_scale 1 1 and uv_offset 0 0", NULL);
    return true;
}

// `paint "<material>"|own <id>:<i> <faces>|slot <N> <x> <y> <w> <h> [paint_filter nearest|trilinear]`
// (docs/painting.md 4.2): stored with its entity; the entity is resolved at the end of the parse
// (ResolvePaintEntities), when every line is known. Not a statement of objects: it takes no
// statement key and no part of the look hashes (d->paintHash has it).
static bool ParsePaint(Parser *P, Token *tok, int n)
{
    SceneData *d = P->d;
    d->paintHash = HashTokens(d->paintHash, tok, n);
    static const char USAGE[] = "paint takes a material (or own), an entity id:index, faces (+x,-y,...) or slot N, "
                                "and a rectangle x y w h: paint \"name\" 12:0 +x 0 0 64 64";
    if (n < 8) return Fail(P, "%s", USAGE);
    PaintDef e = { .slot = -1, .ownAtlas = -1, .line = P->line };

    // The material (`own`: the model slot's own)
    if (tok[1].quoted) {
        if ((e.material = FindMaterial(d, tok[1].s)) < 0)
            return Fail(P, "unknown material \"%s\" (define it with 'material' before its paint)", tok[1].s);
    } else if (strcmp(tok[1].s, "own") == 0) e.material = -1;
    else return Fail(P, "paint: expected a material name in double quotes or own, got '%s'", tok[1].s);

    // The entity: the line's id and the object's index within the line
    char *colon = tok[2].quoted ? NULL : strchr(tok[2].s, ':');
    if (!colon) return Fail(P, "paint: expected an entity id:index (e.g. 12:0), got '%s'", tok[2].s);
    *colon = '\0';
    int id, index;
    bool entOk = ParseInt(P, tok[2].s, 1, 2147483647, &id) && ParseInt(P, colon + 1, 0, MS_MAX_OBJECTS, &index);
    *colon = ':';
    if (!entOk) return false;
    e.entity = (MS_EntityId){ (unsigned int)id, index };

    // Faces (a box) or a slot (a model)
    int t;
    if (!tok[3].quoted && strcmp(tok[3].s, "slot") == 0) {
        e.kind = PAINT_MODEL;
        if (!NumI(P, tok[4], 0, MS_MAX_OBJECTS, &e.slot)) return false;
        t = 5;
    } else {
        if (tok[3].quoted) return Fail(P, "paint: expected faces (+x,-y,...) or slot N, got the string \"%s\"", tok[3].s);
        e.kind = PAINT_BOX;
        for (const char *f = tok[3].s; ; ) {
            size_t len = strcspn(f, ",");
            int face = -1;
            for (int k = 0; k < 6; k++)
                if (len == 2 && strncmp(f, PAINT_FACES[k], 2) == 0) face = k;
            if (face < 0) return Fail(P, "paint: bad face list '%s' (faces +x -x +y -y +z -z, separated by commas)", tok[3].s);
            if (e.faces & (1u << face)) return Fail(P, "paint: face %s twice", PAINT_FACES[face]);
            e.faces |= (unsigned char)(1u << face);
            if (f[len] == '\0') break;
            f += len + 1;
        }
        t = 4;
    }
    if (e.material < 0 && e.kind == PAINT_BOX)
        return Fail(P, "paint own is for a model slot drawn with its own material: paint own N:i slot S x y w h", NULL);

    // The rectangle, then paint_filter (own materials only: a material line has its own key)
    if (n < t + 4) return Fail(P, "%s", USAGE);
    if (!NumI(P, tok[t], 0, 1000000, &e.x) || !NumI(P, tok[t + 1], 0, 1000000, &e.y) ||
        !NumI(P, tok[t + 2], 1, 1000000, &e.w) || !NumI(P, tok[t + 3], 1, 1000000, &e.h))
        return false;
    t += 4;
    if (t < n && !tok[t].quoted && strcmp(tok[t].s, "paint_filter") == 0) {
        if (e.material >= 0)
            return Fail(P, "paint: paint_filter goes on the material's line (only `paint own` entries take it)", NULL);
        static const char *const PF[] = { "trilinear", "nearest" };
        int f;
        if (t + 1 >= n) return Fail(P, "paint_filter takes trilinear or nearest", NULL);
        if (!Keyword(P, tok[t + 1], PF, 2, "paint filter", &f)) return false;
        e.nearest = f == 1;
        t += 2;
    }
    if (t < n) return Fail(P, "paint: unexpected '%s' after the rectangle", tok[t].s);

    PaintDef *slot = MS_PUSH(d->paints, d->paintCount, d->paintCap);
    if (!slot) return Fail(P, "out of memory", NULL);
    *slot = e;
    return true;
}

// The first object of statement `prim` in a list sorted by statement (boxes by boxRefs, models by
// ref): its index and how many the statement made
static int StatementObjects(const SceneData *d, bool models, int prim, int *count)
{
    int n = models ? d->modelCount : d->boxCount, lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int p = models ? d->models[mid].ref.prim : d->boxRefs[mid].prim;
        if (p < prim) lo = mid + 1;
        else hi = mid;
    }
    int k = lo;
    while (k < n && (models ? d->models[k].ref.prim : d->boxRefs[k].prim) == prim) k++;
    *count = k - lo;
    return lo;
}

// After the last line: each paint entry's entity becomes its object (the line `id N` exists and
// made a box, or a model for a slot, with index i), its material has a `paint` key, the own
// materials' atlases are named (and watched), and the entries keep the rules between them
// (PaintConflict). The messages cite the paint line.
static bool ResolvePaintEntities(Parser *P)
{
    SceneData *d = P->d;
    for (int i = 0; i < d->paintCount; i++) {
        PaintDef *e = &d->paints[i];
        P->line = e->line;
        if (e->material >= 0 && !d->matDefs[e->material].paint)
            return Fail(P, "paint: material \"%s\" has no paint key (add paint \"textures/paint/<name>.png\" to it)",
                        d->materials[e->material].name);
        int *s = d->stmtSetCap ? StmtSlot(d, e->entity.stmt) : NULL;
        if (!s || !*s) return FailF(P, "paint: no line has id %u", e->entity.stmt);
        int prim = *s - 1, boxes, models;
        int firstBox = StatementObjects(d, false, prim, &boxes), firstModel = StatementObjects(d, true, prim, &models);
        int count = e->kind == PAINT_BOX ? boxes : models;
        if (count == 0)
            return FailF(P, e->kind == PAINT_BOX ? "paint: line %d (id %u) makes no boxes (a model is painted by slot)"
                                                 : "paint: line %d (id %u) makes no models (a box is painted by faces)",
                         d->stmtLines[prim], e->entity.stmt);
        if (e->entity.index >= count)
            return FailF(P, "paint: %u:%d: line %d makes %d %s", e->entity.stmt, e->entity.index, d->stmtLines[prim], count,
                         e->kind == PAINT_BOX ? (count == 1 ? "box" : "boxes") : (count == 1 ? "model" : "models"));
        e->object = (e->kind == PAINT_BOX ? firstBox : firstModel) + e->entity.index;

        // An own material's atlas: named after the file, watched like any file the map names
        if (e->material < 0) {
            int a = OwnAtlasOf(d, d->models[e->object].file, e->slot);
            if (a < 0) return Fail(P, "out of memory", NULL);
            e->ownAtlas = a;
            OwnAtlas *A = &d->ownAtlases[a];
            A->nearest = A->nearest || e->nearest;
            char full[MS_PATH_MAX + MS_PATH_MAX];
            snprintf(full, sizeof(full), "%s%s", d->root, A->rel);
            if ((FileExists(full) || ProvidedImage(d->live, A->rel)) && !AddDep(P, full)) return false;
        }
    }
    int a = 0, b = 0, c = PaintConflict(d, &a, &b);
    if (c < 0) return Fail(P, "out of memory", NULL);
    if (c > 0) {
        const PaintDef *e = &d->paints[a], *f = &d->paints[b];
        P->line = f->line;
        const char *mat = f->material >= 0 ? d->materials[f->material].name : d->ownAtlases[f->ownAtlas].rel;
        if (c == 1)
            return FailF(P, "paint: the rectangle %d %d %d %d overlaps line %d's (%d %d %d %d) in the atlas of %s",
                         f->x, f->y, f->w, f->h, e->line, e->x, e->y, e->w, e->h, mat);
        if (f->kind == PAINT_BOX)
            return FailF(P, "paint: %u:%d: a face of it is already in line %d's entry of %s", f->entity.stmt,
                         f->entity.index, e->line, mat);
        return FailF(P, "paint: %u:%d slot %d is already in line %d's entry of %s", f->entity.stmt, f->entity.index,
                     f->slot, e->line, mat);
    }
    P->line = 0;
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
            if (!ParseValue(P, ENV_KEYS[i].type, &tok[2], n - 2, dst)) return false;
            return CheckEnvValue(key, ENV_KEYS[i].type, dst, P->path, P->line);
        }
    for (int i = 0; i < COUNT(DEF_KEYS); i++) {
        if (strcmp(DEF_KEYS[i].key, key) != 0) continue;
        void *dst = (char *)e + DEF_KEYS[i].off;
        if (!ParseValue(P, DEF_KEYS[i].type, &tok[2], n - 2, dst)) return false;
        if (strcmp(key, "shadow.home") == 0) e->homeSet = true;
        if (strcmp(key, "fogCache.region") == 0) e->fogRegionSet = true;
        return CheckEnvValue(key, DEF_KEYS[i].type, dst, P->path, P->line);
    }
    bool amb = strncmp(key, "ambientSky.", 11) == 0;
    if (amb || strncmp(key, "sky.", 4) == 0) {
        int k = SkyKeyIndex(key + (amb ? 11 : 4));
        if (k >= 0) {
            R3D_ProceduralSky *sky = amb ? &e->ambient : &e->sky;
            void *dst = (char *)sky + SKY_KEYS[k].off;
            if (!ParseValue(P, SKY_KEYS[k].type, &tok[2], n - 2, dst)) return false;
            if (!CheckEnvValue(key, SKY_KEYS[k].type, dst, P->path, P->line)) return false;
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
    if (strstr(ref, "://") || ref[0] == '/' || ref[0] == '\\' || (ref[0] && ref[1] == ':')) {
        return FailF(P, "\"%s\" refers to \"%s\", outside the package", modelRel, ref);
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
                return FailF(P, "\"%s\" refers to \"%s\", outside the package", modelRel, ref);
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
    char full[MS_PATH_MAX];
    if (snprintf(full, sizeof(full), "%s%s", P->d->root, out) >= (int)sizeof(full)) return Fail(P, "path too long: \"%s\"", out);
    if (!AddDep(P, full)) return false;     // (a dependency: hot reload watches it)
    if (!FileExists(full)) return FailF(P, "file not found: \"%s\" (named by \"%s\")", out, modelRel);
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
    char full[MS_PATH_MAX];
    if (snprintf(full, sizeof(full), "%s%s", P->d->root, mtlRel) >= (int)sizeof(full)) return Fail(P, "path too long: \"%s\"", mtlRel);
    int size;
    char *mtl = ReadWholeFile(full, MS_FILE_MAX, &size, false);
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
    char *text = ReadWholeFile(full, MS_FILE_MAX, &size, false);
    if (!text) return Fail(P, "cannot read the model file \"%s\" (or larger than 4 MB)", modelRel);
    bool ok = gltf ? GltfUriDeps(P, modelRel, text, (size_t)size) : ObjDeps(P, modelRel, text, (size_t)size);
    free(text);
    return ok;
}

bool MS_CheckModelReferences(const char *root, const char *modelRel)
{
    ErrClear();
    SceneData *d = calloc(1, sizeof(SceneData));
    char *full = d ? MS_StrDup(TextFormat("%s%s", root, modelRel)) : NULL;
    char *rootCopy = MS_StrDup(root);
    bool ok = false;
    if (d && full && rootCopy) {
        d->root = rootCopy;
        Parser P = { .d = d, .path = modelRel };
        ok = ModelDeps(&P, modelRel, full);
        for (int i = 0; i < d->depCount; i++) free(d->deps[i]);
        free(d->deps);
    } else ErrSet("out of memory");
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
        // Its paint unwrap (docs/painting.md 4.5), when there is one: a file of the package too
        char side[MS_PATH_MAX], sideFull[MS_PATH_MAX + MS_PATH_MAX];
        if (UnwrapSidecarPath(copy, side, sizeof(side))) {
            snprintf(sideFull, sizeof(sideFull), "%s%s", d->root, side);
            if (FileExists(sideFull) && !AddDep(P, sideFull)) return false;
        }
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
        if (*slot) return FailF(P, "duplicate id %d (also on line %d)", pl->id, d->stmtLines[*slot - 1]);
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

// A statement that is not an object, group, use, spawn or viewpoint (format, material, env): its
// tokens go into d->lookHash, so a live edit knows whether the text changed anything but objects.
// The format and env lines also go into restLookHash (a material line into its own hashes:
// ParseMaterial), so it can tell materials appended or changed from anything else.
static void HashLook(SceneData *d, const Token *tok, int n)
{
    d->lookHash = HashTokens(d->lookHash, tok, n);
    if (strcmp(tok[0].s, "material") != 0) d->restLookHash = HashTokens(d->restLookHash, tok, n);
}

// One statement (already tokenized, at least one token)
static bool ParseStatement(Parser *P, Token *tok, int n)
{
    SceneData *d = P->d;
    bool v2 = d->formatVersion >= 2;
    const char *kw = tok[0].quoted ? "" : tok[0].s;
    if (strcmp(kw, "monolith") == 0 || strcmp(kw, "material") == 0 || strcmp(kw, "env") == 0) HashLook(d, tok, n);

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

    static const char *const V2_STATEMENTS[] = { "material", "use", "env", "spawn", "viewpoint", "light", "model", "paint" };
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
    if (strcmp(kw, "paint") == 0) return ParsePaint(P, tok, n);
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
                           "paint, spawn, viewpoint, repeat or at)", prim);
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
    return ResolvePaintEntities(P);
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
//
// The version is the guard: a file names its CM_VERSION and each section its record size, and the
// reader refuses a version it doesn't know or a record size that differs. So bump CM_VERSION on any
// change to a record (a field added, removed, resized or moved), to a section or to a stored T_*
// value, even one that keeps the size. A new record: a section id at the end of the enum with its
// CM_SECTION entry, CM_VERSION bumped (CmReadHeader: how many sections each version has), a section
// reader (CmRead*) in ReadCompiled's list after the sections its records point into, and the
// matching section writer (CmPut*). The layout goes into docs/compiled-map.md.

#define CM_VERSION          6           // MS_COMPILED_VERSION: bump on any change to a record or section
#define CM_VERSION_OLDEST   2           // the oldest version still read (2: no box turns section; 2 and 3:
                                        // materials without uvFaces and filter; 2 to 4: materials without
                                        // paint, no paint section; 2 to 5: materials without noPrepass)
#define CM_BYTE_ORDER       0x01020304u
#define CM_NONE             0xFFFFFFFFu // a string field without a string
#define CM_MAX_ENV          4096        // env records (the key tables have ~120 keys)
static const unsigned char CM_MAGIC[8] = { 0x89, 'M', 'A', 'P', 'B', '\r', '\n', 0x1A };

// --- Records. The field order, types and array sizes are the file's layout: any change here
// bumps CM_VERSION. A string field (`uint32_t name`, `path`, …) is an offset into the string table,
// CM_NONE for none. A `line` is the map line a value came from (for messages); never negative.

// The file header (64 bytes, at offset 0). sectionTable: the section table's offset;
// sourceName: the source file's name (a string); sourceHash / sourceSize: FNV-1a 64 and size of the
// source text, to tell a stale compiled file; flags: 0.
typedef struct {
    uint8_t magic[8];
    uint32_t version, headerSize, byteOrder, formatVersion, sectionCount, sectionTable;
    uint64_t fileSize, sourceHash, sourceSize;
    uint32_t sourceName, flags;
} CmHeader;

// One section table entry: where section `id` lies in the file, how many records it has and
// their size (which must equal the reader's: CM_SECTION[id].size)
typedef struct {
    uint32_t id, offset, count, recordSize;
} CmSection;

// The source reference of a box, model or light: MS_Ref in its field order (map line, statement
// index, group, material, repeat copy, part of a primitive)
typedef struct {
    int32_t line, prim, group, material, copy, part;
} CmRef;

// A box (section "boxes"), already expanded from repeat / at / stairs / waffle. A turned box holds
// the box as written (before the turn); its CmBoxTurn holds the rotation.
typedef struct {
    float min[3], max[3];
    CmRef ref;
} CmBox;

// A `material` line. maps: texture paths (CM_NONE for none); mapLine: the line of each map key;
// cull, transparency, blend: R3D's enums; inherit: INHERIT_* bits (`inherit`, model overrides);
// unlit / instanced: 0 or 1; uniformFirst / samplerFirst: the material's first record in the
// uniforms / samplers sections (each material's ranges follow the previous material's).
// Version 4: uvFaces: the box face layout (`uv_faces`, a cell per face +x -x +y -y +z -z; all
// 255 = none); filter: TEX_* (`filter`); pad: 0. A version 2 or 3 record ends before uvFaces.
// Version 5: paint: the paint atlas (`paint`, CM_NONE for none); paintFilter: TEX_* (`paint_filter`);
// pad2: 0. A version 4 record ends before paint.
// Version 6: noPrepass: 0 or 1 (`prepass off`). A version 5 record ends before noPrepass.
typedef struct {
    uint32_t name, shader, maps[MAP_COUNT];     // maps: albedo, normal, orm, emission
    int32_t line, shaderLine, mapLine[MAP_COUNT];
    uint8_t albedo[4], emission[4];
    float emissionEnergy, normalScale, occlusion, roughness, metalness, specular,
          uvScale[2], uvOffset[2], alphaCutoff;
    int32_t cull, transparency, blend, priority, wrap;
    uint32_t inherit, unlit, instanced;
    uint32_t uniformFirst, uniformCount, samplerFirst, samplerCount;
    uint8_t uvFaces[6], filter, pad;            // version 4
    uint32_t paint;                             // version 5
    uint8_t paintFilter, pad2[3];
    uint32_t noPrepass;                         // version 6
} CmMaterial;

#define CM_MATERIAL_V3_SIZE ((uint32_t)offsetof(CmMaterial, uvFaces))  // a version 2 / 3 material record
#define CM_MATERIAL_V4_SIZE ((uint32_t)offsetof(CmMaterial, paint))    // a version 4 material record
#define CM_MATERIAL_V5_SIZE ((uint32_t)offsetof(CmMaterial, noPrepass)) // a version 5 material record

// A `paint` statement (version 5), its entity resolved to the object: material (CM_NONE: the model
// slot's own material), kind (0 a box, 1 a model instance), object (index into the boxes or models
// section), faces (a box: bit f for face f, +x -x +y -y +z -z) or the slot (a model), the rectangle
// in texels of the atlas, filter (TEX_*: `paint own … paint_filter`; 0 for a material's entry), the
// paint line. Every entry is stored, also one that doesn't draw (the load decides that).
typedef struct {
    uint32_t material, kind, object, facesOrSlot;
    uint32_t x, y, w, h;
    uint32_t filter;
    int32_t line;
} CmPaint;

// A material's shader uniform (`uniform <name> f [f f f]`): count: how many of v are used (1 to 4); the
// others are 0
typedef struct {
    uint32_t name, count;
    float v[4];
} CmUniform;

// A material's texture sampler (`sampler <name> "path"` or `data_sampler <name> "path"`)
typedef struct {
    uint32_t name, path;
    int32_t line;
    uint32_t data;                              // data: 1 = data_sampler
} CmSampler;

// A `light` line as the parser leaves it (direction normalised, defaults filled in, clip box
// shifted). type: R3D_LightType; inner / outer: a spot's cut-off angles; shadow / clip: 0 or 1;
// clipMin / clipMax: the clip box (zeros without one).
typedef struct {
    uint32_t type;
    float position[3], direction[3];
    uint8_t color[4];
    float energy, specular, range, falloff, inner, outer, fogEnergy;
    uint32_t shadow, clip;
    float clipMin[3], clipMax[3];
    CmRef ref;
} CmLight;

// A `viewpoint` line: its name, the eye and the point it looks at
typedef struct {
    uint32_t name;
    float eye[3], target[3];
} CmViewpoint;

// A model file the map's `model` lines use: its path and the line that first names it
typedef struct {
    uint32_t path;
    int32_t line;
} CmModelFile;

// A `model` line (an instance) as written. file: index into the model files section; origin,
// collide: MS_ModelOrigin, MS_ModelCollide; castShadow: 0 or 1.
typedef struct {
    int32_t file;
    float position[3], rotationDeg[3], scale[3];
    uint32_t origin, collide, castShadow;
    int32_t colliderFirst, colliderCount, slotFirst, slotCount;     // in the model colliders / slots sections
    CmRef ref;
} CmModel;

// A model's collision box in model space (section "model colliders"; `collide box …`)
typedef struct {
    float min[3], max[3];
} CmCollider;

// A model's slot material (section "model slots"; `material.N "name"`): the slot number N and
// the material's index
typedef struct {
    int32_t slot, material;
} CmSlot;

// An `env` key with its final value. value: the type's words (a float, an int, 0 / 1, four colour
// components 0-255, 3 / 4 / 6 floats, an enum index or a path's string offset); the unused words
// are 0.
typedef struct {
    uint32_t key, type, value[6];               // key: the env key's name; type: T_*
} CmEnv;

// The `spawn` line (always one record; hasSpawn 0 without a spawn line)
typedef struct {
    uint32_t hasSpawn;
    float feet[3], yawDeg, pitchDeg;
} CmSpawn;

// A box's turn (`box … rot`, version 3): the box's index and its rotation as written (yaw, pitch,
// roll). The turned box's CmBox holds the box as written.
typedef struct {
    uint32_t box;
    float rotationDeg[3];
} CmBoxTurn;

// The section ids, in file order (an id is also the section's place in the section table)
enum {
    CM_STRINGS = 1, CM_BOXES, CM_GROUPS, CM_MATERIALS, CM_UNIFORMS, CM_SAMPLERS, CM_LIGHTS, CM_VIEWPOINTS,
    CM_MODEL_FILES, CM_MODELS, CM_COLLIDERS, CM_SLOTS, CM_STATEMENTS, CM_ENV, CM_SPAWN, CM_DEPS,
    CM_BOX_TURNS,                       // version 3
    CM_PAINT,                           // version 5
    CM_SECTIONS = CM_PAINT,
    CM_SECTIONS_V2 = CM_DEPS,           // the sections of a version 2 file
    CM_SECTIONS_V4 = CM_BOX_TURNS       //   and of a version 3 or 4 file
};

// What the code knows about one section: its name (for messages), its record size and the most
// records it may have
typedef struct {
    const char *name;
    uint32_t size;
    uint32_t max;
} CmSectionInfo;

// Per section id: name (messages), record size, most records
static const CmSectionInfo CM_SECTION[CM_SECTIONS + 1] = {
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
    [CM_BOX_TURNS]   = { "box turns", sizeof(CmBoxTurn), MS_MAX_OBJECTS },
    [CM_PAINT]       = { "paint", sizeof(CmPaint), MS_MAX_OBJECTS },
};

// --- Reader

// Whether the bytes start with the compiled-map magic (how a load tells the two inputs apart)
static bool IsCompiledMap(const char *bytes, int size)
{
    return size >= (int)sizeof(CM_MAGIC) && memcmp(bytes, CM_MAGIC, sizeof(CM_MAGIC)) == 0;
}

// A reader over the checked file: the bytes, its sections, and where messages go
typedef struct {
    SceneData *d;                   // the scene being filled
    const unsigned char *bytes;
    uint32_t size;
    CmSection sec[CM_SECTIONS + 1]; // by section id, from the checked section table (empty: not in the file)
    uint32_t version;               // the file's CM_VERSION
    const char *str;                // the string table (NUL-terminated: checked)
    uint32_t strSize;
} CmReader;

// Writes "<file>: <message>" into the last error (ErrPutV); returns false, so a check can
// `return CmFail(…)`
static bool CmFail(CmReader *R, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ErrPutV(false, R->d->path, 0, fmt, ap);
    va_end(ap);
    return false;
}

// Record i of section `id` (in range: checked by the caller's loop over the section's count),
// copied out (sections are aligned, but a copy keeps the reads free of aliasing questions). A record
// of an older version that is shorter (CmRecordSize) leaves the newer fields zero.
static void CmRecord(const CmReader *R, int id, uint32_t i, void *out)
{
    uint32_t size = R->sec[id].recordSize;
    memset(out, 0, CM_SECTION[id].size);
    memcpy(out, R->bytes + R->sec[id].offset + (size_t)i * size, size);
}

// The record size of section `id` in a file of `version` (a material had no uvFaces before 4, no
// paint before 5, no noPrepass before 6)
static uint32_t CmRecordSize(uint32_t version, uint32_t id)
{
    if (id == CM_MATERIALS && version < 4) return CM_MATERIAL_V3_SIZE;
    if (id == CM_MATERIALS && version < 5) return CM_MATERIAL_V4_SIZE;
    if (id == CM_MATERIALS && version < 6) return CM_MATERIAL_V5_SIZE;
    return CM_SECTION[id].size;
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

    // The parser's checks: inside the package, and the file is there
    if (!InsidePackage(s)) return CmFail(R, "%s: path must stay inside the package: \"%s\"", what, s);
    char full[MS_PATH_MAX];
    if (snprintf(full, sizeof(full), "%s%s", R->d->root, s) >= (int)sizeof(full))
        return CmFail(R, "path too long: \"%s\"", s);
    if (!FileExists(full))
        return CmFail(R, "file not found: \"%s\" (paths are relative to the package folder)", s);

    if (!(*out = MS_StrDup(s))) return CmFail(R, "out of memory");
    return true;
}

// The number checks the reader repeats. Each copies a guarantee of the text parser, so a file
// that passes them holds only values a parse could have produced.

// n finite floats
static bool FiniteN(const float *v, int n)
{
    for (int k = 0; k < n; k++)
        if (!isfinite(v[k])) return false;
    return true;
}

// A finite float[3]; a finite Vector3
static bool Finite3(const float *v) { return FiniteN(v, 3); }
static bool FiniteV3(Vector3 v) { return isfinite(v.x) && isfinite(v.y) && isfinite(v.z); }

// A position the parser reads as written (never shifted): finite and within ±MS_COORD_LIMIT
static bool InCoordLimit(const float *v)
{
    return Finite3(v) &&
           fabsf(v[0]) <= MS_COORD_LIMIT &&
           fabsf(v[1]) <= MS_COORD_LIMIT &&
           fabsf(v[2]) <= MS_COORD_LIMIT;
}

// Finite corners with min <= max on every axis (a NaN fails the comparison too)
static bool FiniteOrdered(const float *min, const float *max)
{
    return Finite3(min) && Finite3(max) &&
           min[0] <= max[0] && min[1] <= max[1] && min[2] <= max[2];
}

// Box()'s check: shifts and sizes can overflow even when every number written is finite, so the
// corners and the size must both be finite
static bool FiniteBox(BoundingBox b)
{
    return FiniteV3(b.min) && FiniteV3(b.max) &&
           isfinite(b.max.x - b.min.x) && isfinite(b.max.y - b.min.y) && isfinite(b.max.z - b.min.z);
}

// A record's float[3] as a Vector3
static Vector3 V3(const float *v) { return (Vector3){ v[0], v[1], v[2] }; }

// An MS_Ref; boxes and lights always use a material (the active `use`), models may have none (-1)
static bool CmReadRef(CmReader *R, const CmRef *c, bool modelMaterial, const char *what, uint32_t i,
                      MS_Ref *out)
{
    const SceneData *d = R->d;
    if (c->line < 0 ||
        c->prim < 0 || c->prim >= d->stmtCount ||
        c->group < -1 || c->group >= d->groupCount ||
        c->material < (modelMaterial ? -1 : 0) || c->material >= d->materialCount ||
        c->copy < 0 ||
        c->part < 0)
        return CmFail(R, "%s %u: source reference out of range", what, i);
    *out = (MS_Ref){ c->line, c->prim, c->group, c->material, c->copy, c->part };
    return true;
}

// The header and the section table: sizes, versions, bounds. Nothing else is read before this.
static bool CmReadHeader(CmReader *R, CmHeader *h)
{
    // The header: magic, byte order, versions, sizes
    if (R->size < sizeof(CmHeader)) return CmFail(R, "compiled map: file too short (%u bytes)", R->size);
    memcpy(h, R->bytes, sizeof(*h));
    if (memcmp(h->magic, CM_MAGIC, sizeof(CM_MAGIC)) != 0) return CmFail(R, "not a compiled map");
    if (h->byteOrder != CM_BYTE_ORDER)
        return CmFail(R, "compiled map: unsupported byte order (compiled maps are little-endian; big-endian hosts are not supported)");
    if (h->version < CM_VERSION_OLDEST || h->version > CM_VERSION)
        return CmFail(R, "compiled map version %u; this runtime reads compiled map versions %d to %d: compile the map again",
                      h->version, CM_VERSION_OLDEST, CM_VERSION);
    // The sections of this version (a section's id is its place in this list)
    uint32_t sections = h->version == 2 ? CM_SECTIONS_V2 : h->version < 5 ? CM_SECTIONS_V4 : CM_SECTIONS;
    R->version = h->version;
    if (h->headerSize != sizeof(CmHeader) || h->flags != 0) return CmFail(R, "compiled map: bad header");
    if (h->fileSize != R->size)
        return CmFail(R, "compiled map: the header says %llu bytes, the file has %u (truncated or damaged)",
                      (unsigned long long)h->fileSize, R->size);
    if (h->formatVersion < 1 || h->formatVersion > MS_FORMAT_VERSION)
        return CmFail(R, "map format %u is newer than this runtime supports (%d); update monolith_scene",
                      h->formatVersion, MS_FORMAT_VERSION);

    // The section table: one entry per section of this version, inside the file
    if (h->sectionCount != sections ||
        h->sectionTable < sizeof(CmHeader) ||
        h->sectionTable % 4 ||
        (uint64_t)h->sectionTable + (uint64_t)h->sectionCount * sizeof(CmSection) > R->size)
        return CmFail(R, "compiled map: bad section table");

    // Each section: a known id, once, the reader's record size, at most its records, inside the file
    memset(R->sec, 0, sizeof(R->sec));
    for (uint32_t i = 0; i < h->sectionCount; i++) {
        CmSection s;
        memcpy(&s, R->bytes + h->sectionTable + i * sizeof(CmSection), sizeof(s));
        if (s.id < 1 || s.id > sections) return CmFail(R, "compiled map: unknown section %u", s.id);
        const char *name = CM_SECTION[s.id].name;
        if (R->sec[s.id].id) return CmFail(R, "compiled map: section '%s' appears twice", name);
        if (s.recordSize != CmRecordSize(h->version, s.id))
            return CmFail(R, "compiled map: section '%s' has %u-byte records, expected %u",
                          name, s.recordSize, CmRecordSize(h->version, s.id));
        if (s.count > CM_SECTION[s.id].max)
            return CmFail(R, "compiled map: section '%s' has too many records (%u)", name, s.count);
        if (s.offset < sizeof(CmHeader) ||
            s.offset % 4 ||
            (uint64_t)s.offset + (uint64_t)s.count * s.recordSize > R->size)
            return CmFail(R, "compiled map: section '%s' lies outside the file", name);
        R->sec[s.id] = s;
    }
    // (all ids of the version present: sectionCount of them, none twice, none unknown; a version 2
    // file leaves the newer sections empty)
    if (R->sec[CM_SPAWN].count != 1) return CmFail(R, "compiled map: section 'spawn' must have one record");

    // The string table: starts and ends with a NUL (so every offset inside it ends in one), no
    // control characters
    uint32_t n = R->sec[CM_STRINGS].count;
    R->str = (const char *)R->bytes + R->sec[CM_STRINGS].offset;
    R->strSize = n;
    if (n == 0 || R->str[0] != '\0' || R->str[n - 1] != '\0')
        return CmFail(R, "compiled map: bad string table");
    for (uint32_t i = 0; i < n; i++)
        if ((unsigned char)R->str[i] < 0x20 && R->str[i] != '\0')
            return CmFail(R, "compiled map: control character in the string table");

    // The parser's cap on objects
    if ((uint64_t)R->sec[CM_BOXES].count + R->sec[CM_MODELS].count + R->sec[CM_LIGHTS].count > MS_MAX_OBJECTS)
        return CmFail(R, "too many objects (a map makes at most 1000000 boxes, models and lights)");
    return true;
}

// The section readers. Two rules hold in each:
// - Partial loads: a record that owns heap memory (a name, a path) is counted (d->xCount = i + 1)
//   as soon as it owns it, before its other checks, so FreeData frees it when a later check fails.
//   Records without heap memory are counted after their checks.
// - Each check copies a guarantee of the text parser (named in the comment), never more: a file
//   the parser could have produced always loads, any other file fails with a message.

// The source file's name, for messages that cite a map line ("forest.map:12: ...")
static bool CmReadSourceName(CmReader *R, const CmHeader *h)
{
    SceneData *d = R->d;

    // A plain file name next to the compiled file
    const char *src;
    if (!CmString(R, h->sourceName, false, "source name", &src)) return false;
    if (!src[0] || strchr(src, '/') || strchr(src, '\\')) return CmFail(R, "compiled map: bad source name");

    // Its path in the package replaces the compiled file's path in messages
    char full[MS_PATH_MAX];
    if (snprintf(full, sizeof(full), "%s%s", d->root, src) >= (int)sizeof(full))
        return CmFail(R, "path too long: \"%s\"", src);
    char *srcPath = MS_StrDup(full);
    if (!srcPath) return CmFail(R, "out of memory");
    free(d->srcPath);
    d->srcPath = srcPath;
#ifdef MONOLITH_EDITOR
    d->compiled = true;
    d->srcHash = h->sourceHash;
    d->srcSize = h->sourceSize;
#endif
    return true;
}

// Statement keys: nonzero and unique (MS_FindEntity relies on it)
static bool CmReadStatements(CmReader *R)
{
    SceneData *d = R->d;
    uint32_t n = R->sec[CM_STATEMENTS].count;
    if (!MS_RESERVE(d->stmtKeys, d->stmtCap, (int)n)) return CmFail(R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t key;
        CmRecord(R, CM_STATEMENTS, i, &key);
        if (key == 0) return CmFail(R, "compiled map: statement %u has key 0", i);

        // The statement key set (open addressing) finds a duplicate
        if (2 * (d->stmtCount + 1) > d->stmtSetCap && !GrowStmtSet(d)) return CmFail(R, "out of memory");
        int *slot = StmtSlot(d, key);
        if (*slot) return CmFail(R, "compiled map: duplicate statement key %08x", key);
        d->stmtKeys[d->stmtCount] = key;
        *slot = ++d->stmtCount;
    }
    return true;
}

// Group names (`group` lines): each a non-empty name, counted as soon as its copy is made
static bool CmReadGroups(CmReader *R)
{
    SceneData *d = R->d;
    uint32_t n = R->sec[CM_GROUPS].count;
    if (!MS_RESERVE(d->groups, d->groupCap, (int)n)) return CmFail(R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t name;
        CmRecord(R, CM_GROUPS, i, &name);
        if (!CmName(R, name, "group", &d->groups[i])) return false;
        d->groupCount = (int)i + 1;
    }
    return true;
}

// A material's lines and enum fields: what the parser accepts in a `material` line (lines are never
// negative, each enum one of its key's names, the priority within the parser's range)
static bool CmMaterialInRange(const CmMaterial *c)
{
    bool lines = c->line >= 0 && c->shaderLine >= 0;
    for (int k = 0; k < MAP_COUNT; k++) lines = lines && c->mapLine[k] >= 0;
    return lines &&
           c->cull >= 0 && c->cull <= 2 &&
           c->transparency >= 0 && c->transparency <= 2 &&
           c->blend >= 0 && c->blend <= 4 &&
           c->priority >= -1000000 && c->priority <= 1000000 &&
           c->wrap >= 0 && c->wrap <= 2 &&
           c->inherit <= 15 &&
           c->unlit <= 1 &&
           c->instanced <= 1 &&
           c->noPrepass <= 1 &&
           c->filter <= TEX_NEAREST &&
           c->pad == 0 &&
           c->paintFilter <= TEX_NEAREST &&
           c->pad2[0] == 0 && c->pad2[1] == 0 && c->pad2[2] == 0;
}

// A material's face layout: none (all 255), or a layout the parser accepts (FaceCellsValid) on a
// material with uv_scale 1 1 and uv_offset 0 0
static bool CmMaterialFaces(const CmMaterial *c)
{
    static const uint8_t NONE[6] = { 255, 255, 255, 255, 255, 255 };
    if (memcmp(c->uvFaces, NONE, sizeof(NONE)) == 0) return true;
    return FaceCellsValid(c->uvFaces) &&
           c->uvScale[0] == 1.0f && c->uvScale[1] == 1.0f &&
           c->uvOffset[0] == 0.0f && c->uvOffset[1] == 0.0f;
}

// A checked material record into its MS_Material and MatDef (names and paths are already there)
static void CmFillMaterial(const CmMaterial *c, MS_Material *M, MatDef *D)
{
    // What R3D draws with
    R3D_Material *m = &M->material;
    M->line = c->line;
    m->albedo.color = (Color){ c->albedo[0], c->albedo[1], c->albedo[2], c->albedo[3] };
    m->emission.color = (Color){ c->emission[0], c->emission[1], c->emission[2], c->emission[3] };
    m->emission.energy = c->emissionEnergy;
    m->normal.scale = c->normalScale;
    m->orm.occlusion = c->occlusion;
    m->orm.roughness = c->roughness;
    m->orm.metalness = c->metalness;
    m->orm.specular = c->specular;
    m->uvScale = (Vector2){ c->uvScale[0], c->uvScale[1] };
    m->uvOffset = (Vector2){ c->uvOffset[0], c->uvOffset[1] };
    m->alphaCutoff = c->alphaCutoff;
    m->cullMode = (R3D_CullMode)c->cull;
    m->transparencyMode = (R3D_TransparencyMode)c->transparency;
    m->blendMode = (R3D_BlendMode)c->blend;
    m->priority = c->priority;
    m->unlit = c->unlit == 1;

    // What the load needs besides (texture lines, wrap, inheritance, instancing)
    D->shaderLine = c->shaderLine;
    for (int k = 0; k < MAP_COUNT; k++) D->mapLine[k] = c->mapLine[k];
    D->wrap = c->wrap;
    D->inherit = c->inherit;
    D->instanced = c->instanced == 1;
    D->noPrepass = c->noPrepass == 1;
    D->hasFaces = c->uvFaces[0] != 255;
    if (D->hasFaces) memcpy(D->uvFaces, c->uvFaces, sizeof(D->uvFaces));
    D->filter = c->filter;
    D->paintFilter = c->paintFilter;
}

// A material's uniforms and samplers. Each material's ranges start where the previous material's
// ended (the writer's order), so no two materials share a record: shared ranges would let a small
// file allocate a lot of memory. *uniformsUsed / *samplersUsed: where the next ranges must start.
static bool CmReadMaterialParams(CmReader *R, const CmMaterial *c, MatDef *D, const char *name,
                                 uint32_t *uniformsUsed, uint32_t *samplersUsed)
{
    // The ranges: right after the previous material's, inside their sections
    if (c->uniformFirst != *uniformsUsed ||
        c->samplerFirst != *samplersUsed ||
        (uint64_t)c->uniformFirst + c->uniformCount > R->sec[CM_UNIFORMS].count ||
        (uint64_t)c->samplerFirst + c->samplerCount > R->sec[CM_SAMPLERS].count)
        return CmFail(R, "material \"%s\": uniform or sampler range out of range", name);
    *uniformsUsed += c->uniformCount;
    *samplersUsed += c->samplerCount;
    if (!MS_RESERVE(D->uniforms, D->uniformCap, (int)c->uniformCount) ||
        !MS_RESERVE(D->samplers, D->samplerCap, (int)c->samplerCount))
        return CmFail(R, "out of memory");

    // Uniforms: counted before the name is copied, so FreeData frees it on a later failure
    for (uint32_t k = 0; k < c->uniformCount; k++) {
        CmUniform u;
        CmRecord(R, CM_UNIFORMS, c->uniformFirst + k, &u);
        MatUniform *U = &D->uniforms[D->uniformCount++];
        if (!CmName(R, u.name, "uniform", &U->name)) return false;
        // The parser: 1 to 4 finite numbers; the unused ones are 0
        if (u.count < 1 || u.count > 4 || !FiniteN(u.v, 4))
            return CmFail(R, "material \"%s\": uniform '%s' out of range", name, U->name);
        U->count = (int)u.count;
        memcpy(U->v, u.v, sizeof(U->v));
    }

    // Samplers: the same, with a path that must exist
    for (uint32_t k = 0; k < c->samplerCount; k++) {
        CmSampler sm;
        CmRecord(R, CM_SAMPLERS, c->samplerFirst + k, &sm);
        MatSampler *S = &D->samplers[D->samplerCount++];
        if (!CmName(R, sm.name, "sampler", &S->name) || !CmPath(R, sm.path, false, "sampler", &S->path))
            return false;
        if (sm.line < 0 || sm.data > 1)
            return CmFail(R, "material \"%s\": sampler line or kind out of range", name);
        S->line = sm.line;
        S->data = sm.data == 1;
    }
    return true;
}

// Material i: its strings (name, shader, texture maps: present, inside the package), its numbers
// (finite, as every number the parser reads), its lines and enums, then its uniforms and samplers
static bool CmReadMaterial(CmReader *R, uint32_t i, uint32_t *uniformsUsed, uint32_t *samplersUsed)
{
    SceneData *d = R->d;
    CmMaterial c;
    CmRecord(R, CM_MATERIALS, i, &c);
    if (R->version < 4) memset(c.uvFaces, 255, sizeof(c.uvFaces));     // (no layout, filter 0: trilinear)
    if (R->version < 5) c.paint = CM_NONE;                              // (no paint, paint_filter trilinear)

    // Counted first: the strings below are heap copies FreeData must find
    MS_Material *M = &d->materials[i];
    MatDef *D = &d->matDefs[i];
    d->materialCount = (int)i + 1;
    *M = (MS_Material){ .material = R3D_MATERIAL_BASE };
    *D = (MatDef){ 0 };

    // Strings
    char *name, *shader;
    if (!CmName(R, c.name, "material", &name)) return false;
    M->name = name;
    if (i == 0 && strcmp(name, "default") != 0)
        return CmFail(R, "compiled map: material 0 must be \"default\"");
    if (!CmPath(R, c.shader, true, "material shader", &shader)) return false;
    M->shader = shader;
    for (int k = 0; k < MAP_COUNT; k++)
        if (!CmPath(R, c.maps[k], true, "material texture", &D->maps[k])) return false;
    if (!CmPath(R, c.paint, true, "material paint atlas", &D->paint)) return false;
    D->paintLine = c.line;

    // Numbers, lines and enums
    float f[] = {
        c.emissionEnergy, c.normalScale, c.occlusion, c.roughness, c.metalness, c.specular,
        c.uvScale[0], c.uvScale[1], c.uvOffset[0], c.uvOffset[1], c.alphaCutoff
    };
    if (!FiniteN(f, COUNT(f))) return CmFail(R, "material \"%s\": number out of range", name);
    if (!CmMaterialInRange(&c)) return CmFail(R, "material \"%s\": value out of range", name);
    if (!CmMaterialFaces(&c)) return CmFail(R, "material \"%s\": bad uv_faces", name);

    CmFillMaterial(&c, M, D);
    return CmReadMaterialParams(R, &c, D, name, uniformsUsed, samplersUsed);
}

// Material 0 is always "default" (InitData makes it before the first line is parsed). Every
// uniform and sampler record belongs to exactly one material.
static bool CmReadMaterials(CmReader *R)
{
    SceneData *d = R->d;
    uint32_t n = R->sec[CM_MATERIALS].count;
    if (n == 0) return CmFail(R, "compiled map: no materials (material 0 is \"default\")");
    if (!MS_RESERVE(d->materials, d->materialCap, (int)n) || !MS_RESERVE(d->matDefs, d->matDefCap, (int)n))
        return CmFail(R, "out of memory");

    uint32_t uniformsUsed = 0, samplersUsed = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!CmReadMaterial(R, i, &uniformsUsed, &samplersUsed)) return false;

    // No record left over that no material's range covers
    if (uniformsUsed != R->sec[CM_UNIFORMS].count || samplersUsed != R->sec[CM_SAMPLERS].count)
        return CmFail(R, "compiled map: uniforms or samplers no material uses");
    return true;
}

// Boxes as Box() leaves them: corners ordered (its fminf / fmaxf), corners and size finite, and
// in map order (a statement's boxes follow the previous statement's)
static bool CmReadBoxes(CmReader *R)
{
    SceneData *d = R->d;
    uint32_t n = R->sec[CM_BOXES].count;
    if (!MS_RESERVE(d->boxes, d->boxCap, (int)n) || !MS_RESERVE(d->boxRefs, d->boxRefCap, (int)n))
        return CmFail(R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        CmBox c;
        CmRecord(R, CM_BOXES, i, &c);
        BoundingBox b = { V3(c.min), V3(c.max) };
        if (!FiniteOrdered(c.min, c.max) || !FiniteBox(b))
            return CmFail(R, "box %u: coordinate or size out of range", i);
        if (!CmReadRef(R, &c.ref, false, "box", i, &d->boxRefs[i])) return false;
        if (i > 0 && d->boxRefs[i].prim < d->boxRefs[i - 1].prim)
            return CmFail(R, "box %u: out of map order", i);
        d->boxes[i] = b;
        d->boxCount = (int)i + 1;
    }
    return true;
}

// The box turns (version 3), after the boxes they turn: each turned box's CmBox holds the box as
// written; its record and world AABB are rebuilt with the parser's own code (TurnedBoxMake), so
// the bits match a text load. TurnedBox's guarantees: one record per turned box, in box order, a
// finite nonzero rotation, then TurnedBox's and Box()'s checks on the result.
static bool CmReadBoxTurns(CmReader *R)
{
    SceneData *d = R->d;
    uint32_t n = R->sec[CM_BOX_TURNS].count;
    if (n == 0) return true;
    if (n > (uint32_t)d->boxCount)
        return CmFail(R, "compiled map: more box turns (%u) than boxes (%d)", n, d->boxCount);
    if (!MS_RESERVE(d->turned, d->turnedCap, (int)n) ||
        !MS_RESERVE(d->turnedWritten, d->turnedWrittenCap, (int)n) ||
        !MS_RESERVE(d->boxTurned, d->boxTurnedCap, d->boxCount))
        return CmFail(R, "out of memory");
    for (int i = 0; i < d->boxCount; i++) d->boxTurned[i] = -1;

    uint32_t prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        // Check: an existing box, after the previous turn's, turned by a finite nonzero rotation
        CmBoxTurn c;
        CmRecord(R, CM_BOX_TURNS, i, &c);
        if (c.box >= (uint32_t)d->boxCount) return CmFail(R, "box turn %u: box %u out of range", i, c.box);
        if (i > 0 && c.box <= prev) return CmFail(R, "box turn %u: not in box order", i);
        prev = c.box;
        if (!Finite3(c.rotationDeg)) return CmFail(R, "box turn %u: rotation not finite", i);
        if (c.rotationDeg[0] == 0.0f && c.rotationDeg[1] == 0.0f && c.rotationDeg[2] == 0.0f)
            return CmFail(R, "box turn %u: rotation 0 0 0 (that box is not turned)", i);

        // Rebuild: the turned box and its world AABB from the box as written
        BoundingBox w = d->boxes[c.box], a;
        MS_TurnedBox t;
        TurnedBoxMake(w, V3(c.rotationDeg), &t, &a);
        if (!FiniteV3(t.size) || !FiniteV3(t.center) || !FiniteBox(a))
            return CmFail(R, "box turn %u: coordinate or size out of range", i);

        // Store: the box list holds the AABB, the written box is kept beside it
        d->boxes[c.box] = a;
        d->boxTurned[c.box] = (int)i;
        d->turnedWritten[i] = w;
        d->turned[i] = t;
        d->turnedCount = (int)i + 1;
    }
    return true;
}

// Model files: a path inside the package that exists, counted as soon as its copy is made, then
// its line checked
static bool CmReadModelFiles(CmReader *R)
{
    SceneData *d = R->d;
    uint32_t n = R->sec[CM_MODEL_FILES].count;
    if (!MS_RESERVE(d->modelFiles, d->modelFileCap, (int)n) ||
        !MS_RESERVE(d->modelPick, d->modelPickCap, (int)n))
        return CmFail(R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        CmModelFile c;
        CmRecord(R, CM_MODEL_FILES, i, &c);
        char *path;
        if (!CmPath(R, c.path, false, "model file", &path)) return false;
        d->modelFiles[i] = (MS_ModelFile){ .path = path, .line = c.line };
        d->modelFileCount = (int)i + 1;
        if (c.line < 0) return CmFail(R, "model file \"%s\": line out of range", path);
    }
    return true;
}

// A model's collision boxes in its own space (the parser's: finite, ordered corners)
static bool CmReadColliders(CmReader *R)
{
    SceneData *d = R->d;
    uint32_t n = R->sec[CM_COLLIDERS].count;
    if (!MS_RESERVE(d->localColliders, d->localColliderCap, (int)n)) return CmFail(R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        CmCollider c;
        CmRecord(R, CM_COLLIDERS, i, &c);
        if (!FiniteOrdered(c.min, c.max)) return CmFail(R, "model collider %u: coordinate out of range", i);
        d->localColliders[d->localColliderCount++] = (BoundingBox){ V3(c.min), V3(c.max) };
    }
    return true;
}

// A model's slot materials (`material.N "name"`): a slot number and a material read above
static bool CmReadSlots(CmReader *R)
{
    SceneData *d = R->d;
    uint32_t n = R->sec[CM_SLOTS].count;
    if (!MS_RESERVE(d->modelSlots, d->modelSlotCap, (int)n)) return CmFail(R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        CmSlot c;
        CmRecord(R, CM_SLOTS, i, &c);
        if (c.slot < 0 || c.slot > MS_MAX_OBJECTS ||
            c.material < 0 || c.material >= d->materialCount)
            return CmFail(R, "model slot %u: out of range", i);
        d->modelSlots[d->modelSlotCount++] = (MS_SlotMaterial){ c.slot, c.material };
    }
    return true;
}

// Model instances, after the files, colliders and slots they index. The parser's guarantees for a
// `model` line: finite transform, positive scale, enums in range, collider ranges only for
// `collide box …`, map order, and its cap on collision boxes (an `aabb` model makes one per copy)
static bool CmReadModels(CmReader *R)
{
    SceneData *d = R->d;
    uint32_t n = R->sec[CM_MODELS].count;
    if (!MS_RESERVE(d->models, d->modelCap, (int)n)) return CmFail(R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        CmModel c;
        CmRecord(R, CM_MODELS, i, &c);
        MS_ModelInstance M = { 0 };

        // The file, the transform and the enums
        if (c.file < 0 || c.file >= d->modelFileCount ||
            !Finite3(c.position) ||
            !Finite3(c.rotationDeg) ||
            !Finite3(c.scale) ||
            !(c.scale[0] > 0.0f && c.scale[1] > 0.0f && c.scale[2] > 0.0f) ||
            c.origin > 2 ||
            c.collide > 2 ||
            c.castShadow > 1)
            return CmFail(R, "model %u: value out of range", i);

        // The collider and slot ranges (colliders only for `collide box …`)
        if (c.colliderFirst < 0 || c.colliderCount < 0 ||
            (int64_t)c.colliderFirst + c.colliderCount > d->localColliderCount ||
            (c.collide != MS_COLLIDE_BOXES && c.colliderCount != 0) ||
            c.slotFirst < 0 || c.slotCount < 0 ||
            (int64_t)c.slotFirst + c.slotCount > d->modelSlotCount)
            return CmFail(R, "model %u: collider or slot range out of range", i);

        // The source reference, map order, and the cap on collision boxes
        if (!CmReadRef(R, &c.ref, true, "model", i, &M.ref)) return false;
        if (i > 0 && M.ref.prim < d->models[i - 1].ref.prim)
            return CmFail(R, "model %u: out of map order", i);
        int perCopy = c.collide == MS_COLLIDE_AABB ? 1 : c.colliderCount;
        if (d->modelColliderCount + perCopy > MS_MAX_OBJECTS)
            return CmFail(R, "too many model collision boxes (max 1000000)");
        d->modelColliderCount += perCopy;

        // Fill and store
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
    return true;
}

// The paint entries (version 5), after the materials, boxes and models they point into. The
// parser's guarantees for a `paint` line: a material with a paint key (or a model slot's own), a
// box with faces or a model with a slot, made by a line with an explicit `id`, a rectangle of at
// least one texel, paint_filter only on own entries, and the rules between the entries
// (PaintConflict). The entity id is the object's (BuildEntities fills it in).
static bool CmReadPaint(CmReader *R)
{
    SceneData *d = R->d;
    uint32_t n = R->sec[CM_PAINT].count;
    if (n == 0) return true;
    if (!MS_RESERVE(d->paints, d->paintCap, (int)n)) return CmFail(R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        CmPaint c;
        CmRecord(R, CM_PAINT, i, &c);
        bool own = c.material == CM_NONE;
        bool ok = (own || (c.material < (uint32_t)d->materialCount && d->matDefs[c.material].paint)) &&
                  c.kind <= PAINT_MODEL &&
                  (!own || c.kind == PAINT_MODEL) &&
                  c.object < (uint32_t)(c.kind == PAINT_BOX ? d->boxCount : d->modelCount) &&
                  (c.kind == PAINT_BOX ? c.facesOrSlot >= 1 && c.facesOrSlot <= 0x3F : c.facesOrSlot <= MS_MAX_OBJECTS) &&
                  c.x <= 1000000 && c.y <= 1000000 && c.w >= 1 && c.w <= 1000000 && c.h >= 1 && c.h <= 1000000 &&
                  c.filter <= TEX_NEAREST && (own || c.filter == 0) &&
                  c.line >= 0;
        if (!ok) return CmFail(R, "paint %u: value out of range", i);
        int prim = c.kind == PAINT_BOX ? d->boxRefs[c.object].prim : d->models[c.object].ref.prim;
        if (d->stmtKeys[prim] & 0x80000000u) return CmFail(R, "paint %u: its line has no id", i);

        PaintDef e = {
            .material = own ? -1 : (int)c.material, .kind = (int)c.kind, .object = (int)c.object,
            .faces = c.kind == PAINT_BOX ? (unsigned char)c.facesOrSlot : 0,
            .slot = c.kind == PAINT_BOX ? -1 : (int)c.facesOrSlot,
            .x = (int)c.x, .y = (int)c.y, .w = (int)c.w, .h = (int)c.h,
            .nearest = c.filter == TEX_NEAREST, .line = c.line, .ownAtlas = -1,
        };
        if (own) {
            if ((e.ownAtlas = OwnAtlasOf(d, d->models[e.object].file, e.slot)) < 0) return CmFail(R, "out of memory");
            d->ownAtlases[e.ownAtlas].nearest = d->ownAtlases[e.ownAtlas].nearest || e.nearest;
        }
        d->paints[d->paintCount++] = e;
    }
    int a = 0, b = 0, conflict = PaintConflict(d, &a, &b);
    if (conflict < 0) return CmFail(R, "out of memory");
    if (conflict == 1) return CmFail(R, "paint %d: its rectangle overlaps paint %d's in one atlas", b, a);
    if (conflict == 2) return CmFail(R, "paint %d: a face or slot of its object is in paint %d too", b, a);
    return true;
}

// The parser's guarantees for a `light` line: the numbers as written are finite (positions within
// the coordinate limit, but `repeat` / `at` may shift them past it), a point light has no
// direction and the others a normalised one, a spot's inner angle is at most its outer one, the
// clip box is ordered and not flat. (Numbers so large or small that normalising or shifting them
// breaks this fail when the map is compiled, which reads the result back.)
static bool CmLightInRange(const CmLight *c)
{
    // Every number finite, the enums and switches in range
    float f[] = { c->energy, c->specular, c->range, c->falloff, c->inner, c->outer, c->fogEnergy };
    bool ok = c->type <= 2 &&
              Finite3(c->position) &&
              Finite3(c->direction) &&
              c->shadow <= 1 &&
              c->clip <= 1 &&
              FiniteN(f, COUNT(f));

    // The direction by type: none for a point light, unit length otherwise
    float len = ok ? Vector3Length(V3(c->direction)) : 0.0f;
    if (c->type == R3D_LIGHT_OMNI)
        ok = ok && c->direction[0] == 0.0f && c->direction[1] == 0.0f && c->direction[2] == 0.0f;
    else
        ok = ok && fabsf(len - 1.0f) <= 1e-3f;

    // A spot's cone, the clip box
    if (c->type == R3D_LIGHT_SPOT) ok = ok && c->inner <= c->outer;
    if (c->clip)
        ok = ok &&
             Finite3(c->clipMin) && Finite3(c->clipMax) &&
             c->clipMin[0] < c->clipMax[0] &&
             c->clipMin[1] < c->clipMax[1] &&
             c->clipMin[2] < c->clipMax[2];
    return ok;
}

// Lights: checked (CmLightInRange, the source reference, map order), then into R3D_Light fields
static bool CmReadLights(CmReader *R)
{
    SceneData *d = R->d;
    uint32_t n = R->sec[CM_LIGHTS].count;
    if (!MS_RESERVE(d->lights, d->lightCap, (int)n)) return CmFail(R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        CmLight c;
        CmRecord(R, CM_LIGHTS, i, &c);
        if (!CmLightInRange(&c)) return CmFail(R, "light %u: value out of range", i);
        MS_LightDef L = { 0 };
        if (!CmReadRef(R, &c.ref, false, "light", i, &L.ref)) return false;
        if (i > 0 && L.ref.prim < d->lights[i - 1].ref.prim)
            return CmFail(R, "light %u: out of map order", i);

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
    return true;
}

// Viewpoints are written as numbers, never shifted: within the coordinate limit, eye != target
static bool CmReadViewpoints(CmReader *R)
{
    SceneData *d = R->d;
    uint32_t n = R->sec[CM_VIEWPOINTS].count;
    if (!MS_RESERVE(d->viewpoints, d->viewpointCap, (int)n)) return CmFail(R, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        CmViewpoint c;
        CmRecord(R, CM_VIEWPOINTS, i, &c);
        MS_Viewpoint *v = &d->viewpoints[i];

        // Counted as soon as the name is copied, then checked
        char *name;
        if (!CmName(R, c.name, "viewpoint", &name)) return false;
        *v = (MS_Viewpoint){ name, V3(c.eye), V3(c.target) };
        d->viewpointCount = (int)i + 1;
        if (!InCoordLimit(c.eye) || !InCoordLimit(c.target) || Vector3Equals(v->eye, v->target))
            return CmFail(R, "viewpoint \"%s\": position out of range", name);
    }
    return true;
}

// The one spawn record (CmReadHeader checked there is exactly one), present or not
static bool CmReadSpawn(CmReader *R)
{
    SceneData *d = R->d;
    CmSpawn sp;
    CmRecord(R, CM_SPAWN, 0, &sp);
    if (sp.hasSpawn > 1 || !InCoordLimit(sp.feet) || !isfinite(sp.yawDeg) || !isfinite(sp.pitchDeg))
        return CmFail(R, "spawn: value out of range");
    d->hasSpawn = sp.hasSpawn == 1;
    d->spawnFeet = V3(sp.feet);
    d->spawnYawDeg = sp.yawDeg;
    d->spawnPitchDeg = sp.pitchDeg;
    return true;
}

// One env record: the key's table entry by name, the type it must have, the value checked as
// the text parser checks it, then stored where the text parser stores it
static bool CmReadEnvRecord(CmReader *R, const CmEnv *c)
{
    EnvDef *e = &R->d->env;
    const char *key;
    if (!CmString(R, c->key, false, "env key", &key)) return false;

    // The key's table entry: ENV_KEYS (R3D_Environment fields), DEF_KEYS, then the sky tables
    // (`sky.*` and `ambientSky.*` share SKY_KEYS)
    int type = -1;
    void *dst = NULL;
    int sky = -1;
    bool amb = false;
    for (int i = 0; i < COUNT(ENV_KEYS) && !dst; i++)
        if (strcmp(ENV_KEYS[i].key, key) == 0) {
            type = ENV_KEYS[i].type;
            dst = (char *)&e->env + ENV_KEYS[i].off;
        }
    for (int i = 0; i < COUNT(DEF_KEYS) && !dst; i++)
        if (strcmp(DEF_KEYS[i].key, key) == 0) { type = DEF_KEYS[i].type; dst = (char *)e + DEF_KEYS[i].off; }
    if (!dst) {
        amb = strncmp(key, "ambientSky.", 11) == 0;
        if (amb || strncmp(key, "sky.", 4) == 0) sky = SkyKeyIndex(key + (amb ? 11 : 4));
        if (sky >= 0) {
            type = SKY_KEYS[sky].type;
            dst = (char *)(amb ? &e->ambient : &e->sky) + SKY_KEYS[sky].off;
        }
    }
    if (!dst) return CmFail(R, "unknown env key '%s'", key);
    if (c->type != (uint32_t)type) return CmFail(R, "env key '%s': wrong value type", key);

    // The parser's guarantees per value type; the words a type doesn't use are 0
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
        case T_QUAT: words = 4; ok = FiniteN(f, 4); break;
        case T_BOX: words = 6; ok = FiniteOrdered(f, f + 3); break;
        case T_PATH: break;
        default: ok = w[0] < (uint32_t)ENUM_COUNT[type]; break;
    }
    for (int k = words; k < 6; k++) ok = ok && w[k] == 0;
    if (!ok) return CmFail(R, "env key '%s': value out of range", key);

    // Store the value in the field's own type
    switch (type) {
        case T_B: *(bool *)dst = w[0] == 1; break;
        case T_COLOR:
            *(Color *)dst = (Color){
                (unsigned char)w[0], (unsigned char)w[1], (unsigned char)w[2], (unsigned char)w[3]
            };
            break;
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

    // The checks across keys and the "set by the map" flags, exactly as the parser leaves them
    if (!CheckEnvValue(key, type, dst, R->d->path, 0)) return false;
    if (strcmp(key, "shadow.home") == 0) e->homeSet = true;
    if (strcmp(key, "fogCache.region") == 0) e->fogRegionSet = true;
    if (sky >= 0 && amb) e->ambientSet |= 1u << sky;
    if (sky >= 0 && !amb && strcmp(SKY_KEYS[sky].key, "sunDirection") == 0) e->skySunDirSet = true;
    return true;
}

// Env records override the built-in environment InitData set, key by key, in file order
static bool CmReadEnv(CmReader *R)
{
    uint32_t n = R->sec[CM_ENV].count;
    for (uint32_t i = 0; i < n; i++) {
        CmEnv c;
        CmRecord(R, CM_ENV, i, &c);
        if (!CmReadEnvRecord(R, &c)) return false;
    }
    return true;
}

// The package files the map was made from, package-relative. Every build checks them; only a
// build that watches files keeps them ([0], the compiled file itself, is there from InitData).
static bool CmReadDeps(CmReader *R)
{
    SceneData *d = R->d;
    uint32_t n = R->sec[CM_DEPS].count;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off;
        const char *rel;
        CmRecord(R, CM_DEPS, i, &off);
        if (!CmString(R, off, false, "dependency", &rel)) return false;
        if (!InsidePackage(rel))
            return CmFail(R, "dependency: path must stay inside the package: \"%s\"", rel);
#ifdef MONOLITH_EDITOR
        char full[MS_PATH_MAX];
        if (snprintf(full, sizeof(full), "%s%s", d->root, rel) >= (int)sizeof(full))
            return CmFail(R, "path too long: \"%s\"", rel);
        char *dep = MS_StrDup(full);
        char **slot = dep ? MS_PUSH(d->deps, d->depCount, d->depCap) : NULL;
        if (!slot) { free(dep); return CmFail(R, "out of memory"); }
        *slot = dep;
#else
        (void)d;
#endif
    }
    return true;
}

// The parser refuses a map without boxes and models (format 1 had no models)
static bool CmCheckNotEmpty(CmReader *R)
{
    const SceneData *d = R->d;
    if (d->boxCount == 0 && d->modelCount == 0)
        return CmFail(R, d->formatVersion >= 2 ? "map is empty (no boxes and no models)"
                                               : "map is empty (no boxes)");
    return true;
}

// Fills d (after InitData) from a compiled map: the same SceneData the text parser would have made.
// The header and the section table are checked before anything else is read. Then the sections,
// in an order the data fixes: a record is checked against the records it points into, so those
// are read first. The order is also the order of the messages when a file has several faults.
static bool ReadCompiled(SceneData *d, const char *bytes, int size)
{
    CmReader R = {
        .d = d,
        .bytes = (const unsigned char *)bytes,
        .size = (uint32_t)size
    };

    // The header and the section table first
    CmHeader h;
    if (size < 0 || !CmReadHeader(&R, &h)) return false;
    d->formatVersion = (int)h.formatVersion;
    if (!CmReadSourceName(&R, &h)) return false;

    // Then the sections, each after the ones it points into
    return CmReadStatements(&R) &&      // every MS_Ref points at a statement,
           CmReadGroups(&R) &&          // a group (or none)
           CmReadMaterials(&R) &&       // and a material; slots name materials too
           CmReadBoxes(&R) &&
           CmReadBoxTurns(&R) &&        // a turn rebuilds a box read above
           CmReadModelFiles(&R) &&      // a model indexes a file,
           CmReadColliders(&R) &&       // a range of colliders
           CmReadSlots(&R) &&           // and a range of slots
           CmReadModels(&R) &&
           CmReadPaint(&R) &&           // a paint entry names a material and a box or model
           CmReadLights(&R) &&
           CmReadViewpoints(&R) &&
           CmReadSpawn(&R) &&
           CmReadEnv(&R) &&
           CmReadDeps(&R) &&
           CmCheckNotEmpty(&R);
}

#ifdef MONOLITH_EDITOR
// The source text of a compiled map (next to it) against the hash and size it was compiled from
static MS_SourceState SourceState(const SceneData *d)
{
    if (!d->compiled) return MS_SOURCE_TEXT;
    FILE *f = fopen(d->srcPath, "rb");
    if (!f) return MS_SOURCE_MISSING;

    // FNV-1a 64 and size of the whole file, as the writer computed them
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

// A warning in the log when the compiled map is older than its source text
static void WarnStale(const SceneData *d)
{
    if (SourceState(d) == MS_SOURCE_STALE)
        TraceLog(LOG_WARNING, "monolith_scene: %s was compiled from an older %s: compile it again",
                 d->path, d->srcPath);
}

// --- Writer (editor): a parsed SceneData (before FinishParse) as a compiled map

// A growing byte buffer: one section's contents
typedef struct {
    unsigned char *b;
    int n, cap;
} CmBuf;

// The writer's state: each section's bytes and record count, the string table's index, and one
// out-of-memory flag that every later call checks (so the writers need no error paths)
typedef struct {
    CmBuf sec[CM_SECTIONS + 1];     // section contents
    uint32_t count[CM_SECTIONS + 1];
    uint32_t *strSlot;              // string table index: offset + 1 per slot, 0 = empty (open addressing)
    int strSlotCap, strCount;
    bool oom;
} CmWriter;

// Appends one record (CM_SECTION[id].size bytes from rec) to section id
static void CmPut(CmWriter *W, int id, const void *rec)
{
    CmBuf *B = &W->sec[id];
    uint32_t size = CM_SECTION[id].size;
    if (W->oom || !MS_RESERVE(B->b, B->cap, B->n + (int)size)) { W->oom = true; return; }
    memcpy(B->b + B->n, rec, size);
    B->n += (int)size;
    W->count[id]++;
}

// The string table index's hash of a string
static unsigned long long CmHash(const char *s) { return Fnv1a64Update(FNV_OFFSET, s, strlen(s)); }

// The string's offset in the string table (each string stored once, in first-use order); NULL = CM_NONE
static uint32_t CmStr(CmWriter *W, const char *s)
{
    if (!s) return CM_NONE;
    CmBuf *T = &W->sec[CM_STRINGS];
    if (W->oom) return 0;

    // Grow the index at half full: rehash every stored string into a table twice the size
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

    // Already stored: its offset
    unsigned h = (unsigned)CmHash(s) & (unsigned)(W->strSlotCap - 1);
    for (; W->strSlot[h]; h = (h + 1) & (unsigned)(W->strSlotCap - 1))
        if (strcmp((const char *)T->b + W->strSlot[h] - 1, s) == 0) return W->strSlot[h] - 1;

    // New: appended to the table with its NUL, and indexed in the free slot the probe ended on
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

// An MS_Ref as its record
static CmRef CmRefOf(MS_Ref r) { return (CmRef){ r.line, r.prim, r.group, r.material, r.copy, r.part }; }

// A vector or colour as the float / byte array of a record field (in an initializer)
#define CM_XY(v)    { (v).x, (v).y }
#define CM_XYZ(v)   { (v).x, (v).y, (v).z }
#define CM_RGBA(c)  { (c).r, (c).g, (c).b, (c).a }

// One env record of `type` from the value at p
static void CmPutEnv(CmWriter *W, const char *key, int type, const void *p)
{
    CmEnv c = { .key = CmStr(W, key), .type = (uint32_t)type };
    switch (type) {
        case T_B: c.value[0] = *(const bool *)p ? 1 : 0; break;
        case T_COLOR: {
            const Color *col = p;
            c.value[0] = col->r;
            c.value[1] = col->g;
            c.value[2] = col->b;
            c.value[3] = col->a;
            break;
        }
        case T_V3: memcpy(c.value, p, 3 * sizeof(float)); break;
        case T_QUAT: memcpy(c.value, p, 4 * sizeof(float)); break;
        case T_BOX: memcpy(c.value, p, 6 * sizeof(float)); break;
        case T_PATH: c.value[0] = CmStr(W, p); break;
        default: memcpy(c.value, p, 4); break;
    }
    CmPut(W, CM_ENV, &c);
}

// Every env key with its final value, in table order: ENV_KEYS, DEF_KEYS (without the optional
// keys the map didn't set), then per sky key `sky.*` and the `ambientSky.*` the map set
static void CmPutEnvironment(CmWriter *W, const EnvDef *e)
{
    for (int i = 0; i < COUNT(ENV_KEYS); i++)
        CmPutEnv(W, ENV_KEYS[i].key, ENV_KEYS[i].type, (const char *)&e->env + ENV_KEYS[i].off);

    for (int i = 0; i < COUNT(DEF_KEYS); i++) {
        const char *k = DEF_KEYS[i].key;
        if ((strcmp(k, "shadow.home") == 0 && !e->homeSet) ||
            (strcmp(k, "fogCache.region") == 0 && !e->fogRegionSet) ||
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

// Whether this host stores integers little-endian (records are copied out of memory as they are)
static bool IsLittleEndianHost(void)
{
    uint32_t v = CM_BYTE_ORDER;
    unsigned char b[4];
    memcpy(b, &v, 4);
    return b[0] == 0x04;
}

// The section writers. Each appends its records in the order the reader expects (map order, the
// order the parser made them). Sections land in the file by id, whatever order they are written
// in, but strings don't: the string table holds each string once, in the order CmStr first sees
// it. The order of the CmStr calls (these writers in WriteCompiled's order, and the calls inside
// each) is therefore part of the output: keep it, or the same map compiles to other bytes.

// One key per statement, by statement index
static void CmPutStatements(CmWriter *W, const SceneData *d)
{
    for (int i = 0; i < d->stmtCount; i++) CmPut(W, CM_STATEMENTS, &d->stmtKeys[i]);
}

// The group names, by group index
static void CmPutGroups(CmWriter *W, const SceneData *d)
{
    for (int i = 0; i < d->groupCount; i++) {
        uint32_t s = CmStr(W, d->groups[i]);
        CmPut(W, CM_GROUPS, &s);
    }
}

// Each material, then its uniforms and samplers: its ranges start where the previous material's
// ended (CmReadMaterialParams relies on it)
static void CmPutMaterials(CmWriter *W, const SceneData *d)
{
    for (int i = 0; i < d->materialCount; i++) {
        const MS_Material *M = &d->materials[i];
        const MatDef *D = &d->matDefs[i];
        const R3D_Material *m = &M->material;

        // Strings in this order: name, shader, then the maps (in the loop below)
        uint32_t name = CmStr(W, M->name), shader = CmStr(W, M->shader);
        CmMaterial c = {
            .name = name, .shader = shader,
            .line = M->line, .shaderLine = D->shaderLine,
            .albedo = CM_RGBA(m->albedo.color), .emission = CM_RGBA(m->emission.color),
            .emissionEnergy = m->emission.energy, .normalScale = m->normal.scale,
            .occlusion = m->orm.occlusion, .roughness = m->orm.roughness,
            .metalness = m->orm.metalness, .specular = m->orm.specular,
            .uvScale = CM_XY(m->uvScale), .uvOffset = CM_XY(m->uvOffset), .alphaCutoff = m->alphaCutoff,
            .cull = m->cullMode, .transparency = m->transparencyMode, .blend = m->blendMode,
            .priority = m->priority,
            .wrap = D->wrap, .inherit = D->inherit, .unlit = m->unlit, .instanced = D->instanced,
            .uniformFirst = W->count[CM_UNIFORMS], .uniformCount = (uint32_t)D->uniformCount,
            .samplerFirst = W->count[CM_SAMPLERS], .samplerCount = (uint32_t)D->samplerCount,
            .uvFaces = { 255, 255, 255, 255, 255, 255 }, .filter = D->filter,
            .paintFilter = D->paintFilter, .noPrepass = D->noPrepass,
        };
        if (D->hasFaces) memcpy(c.uvFaces, D->uvFaces, sizeof(c.uvFaces));
        for (int k = 0; k < MAP_COUNT; k++) {
            c.maps[k] = CmStr(W, D->maps[k]);
            c.mapLine[k] = D->mapLine[k];
        }
        c.paint = CmStr(W, D->paint);
        CmPut(W, CM_MATERIALS, &c);

        // Its uniforms and samplers, right after the previous material's
        for (int k = 0; k < D->uniformCount; k++) {
            CmUniform u = { CmStr(W, D->uniforms[k].name), (uint32_t)D->uniforms[k].count, { 0 } };
            memcpy(u.v, D->uniforms[k].v, sizeof(u.v));
            CmPut(W, CM_UNIFORMS, &u);
        }
        for (int k = 0; k < D->samplerCount; k++) {
            const MatSampler *S = &D->samplers[k];
            uint32_t sname = CmStr(W, S->name), path = CmStr(W, S->path);
            CmSampler sm = { sname, path, S->line, S->data };
            CmPut(W, CM_SAMPLERS, &sm);
        }
    }
}

// A turned box is stored as written (the reader rebuilds its AABB) plus a box turn record
static void CmPutBoxes(CmWriter *W, const SceneData *d)
{
    for (int i = 0; i < d->boxCount; i++) {
        const MS_TurnedBox *t = TurnedOf(d, i);
        const BoundingBox *b = t ? &d->turnedWritten[d->boxTurned[i]] : &d->boxes[i];
        CmBox c = { CM_XYZ(b->min), CM_XYZ(b->max), CmRefOf(d->boxRefs[i]) };
        CmPut(W, CM_BOXES, &c);
        if (t) {
            CmBoxTurn r = { (uint32_t)i, CM_XYZ(t->rotationDeg) };
            CmPut(W, CM_BOX_TURNS, &r);
        }
    }
}

// Model files, their colliders and slot materials, then the instances that index them
static void CmPutModels(CmWriter *W, const SceneData *d)
{
    for (int i = 0; i < d->modelFileCount; i++) {
        CmModelFile c = { CmStr(W, d->modelFiles[i].path), d->modelFiles[i].line };
        CmPut(W, CM_MODEL_FILES, &c);
    }
    for (int i = 0; i < d->localColliderCount; i++) {
        const BoundingBox *b = &d->localColliders[i];
        CmCollider c = { CM_XYZ(b->min), CM_XYZ(b->max) };
        CmPut(W, CM_COLLIDERS, &c);
    }
    for (int i = 0; i < d->modelSlotCount; i++) {
        CmSlot c = { d->modelSlots[i].slot, d->modelSlots[i].material };
        CmPut(W, CM_SLOTS, &c);
    }
    for (int i = 0; i < d->modelCount; i++) {
        const MS_ModelInstance *M = &d->models[i];
        CmModel c = {
            .file = M->file,
            .position = CM_XYZ(M->position), .rotationDeg = CM_XYZ(M->rotationDeg), .scale = CM_XYZ(M->scale),
            .origin = M->origin, .collide = M->collide, .castShadow = M->castShadow,
            .colliderFirst = M->colliderFirst, .colliderCount = M->colliderCount,
            .slotFirst = M->slotFirst, .slotCount = M->slotCount,
            .ref = CmRefOf(M->ref),
        };
        CmPut(W, CM_MODELS, &c);
    }
}

// The paint entries in map order, each resolved to its object (ResolvePaintEntities)
static void CmPutPaint(CmWriter *W, const SceneData *d)
{
    for (int i = 0; i < d->paintCount; i++) {
        const PaintDef *e = &d->paints[i];
        CmPaint c = {
            .material = e->material >= 0 ? (uint32_t)e->material : CM_NONE,
            .kind = (uint32_t)e->kind, .object = (uint32_t)e->object,
            .facesOrSlot = e->kind == PAINT_BOX ? e->faces : (uint32_t)e->slot,
            .x = (uint32_t)e->x, .y = (uint32_t)e->y, .w = (uint32_t)e->w, .h = (uint32_t)e->h,
            .filter = e->nearest ? TEX_NEAREST : TEX_TRILINEAR, .line = e->line,
        };
        CmPut(W, CM_PAINT, &c);
    }
}

// A light without a clip box stores zeros there (the reader ignores them)
static void CmPutLights(CmWriter *W, const SceneData *d)
{
    for (int i = 0; i < d->lightCount; i++) {
        const MS_LightDef *L = &d->lights[i];
        const R3D_Light *l = &L->light;
        CmLight c = {
            .type = l->type,
            .position = CM_XYZ(l->position), .direction = CM_XYZ(l->direction),
            .color = CM_RGBA(l->color),
            .energy = l->energy, .specular = l->specular, .range = l->range, .falloff = l->falloff,
            .inner = l->innerCutOff, .outer = l->outerCutOff, .fogEnergy = l->fogEnergy,
            .shadow = L->shadow, .clip = L->clip,
            .ref = CmRefOf(L->ref),
        };
        if (L->clip) {
            memcpy(c.clipMin, &L->clipBox.min, sizeof(c.clipMin));
            memcpy(c.clipMax, &L->clipBox.max, sizeof(c.clipMax));
        }
        CmPut(W, CM_LIGHTS, &c);
    }
}

// The viewpoints in map order: name, eye, target
static void CmPutViewpoints(CmWriter *W, const SceneData *d)
{
    for (int i = 0; i < d->viewpointCount; i++) {
        const MS_Viewpoint *v = &d->viewpoints[i];
        CmViewpoint c = { CmStr(W, v->name), CM_XYZ(v->eye), CM_XYZ(v->target) };
        CmPut(W, CM_VIEWPOINTS, &c);
    }
}

// Always one record, also without a `spawn` line (hasSpawn 0)
static void CmPutSpawn(CmWriter *W, const SceneData *d)
{
    CmSpawn sp = { d->hasSpawn, CM_XYZ(d->spawnFeet), d->spawnYawDeg, d->spawnPitchDeg };
    CmPut(W, CM_SPAWN, &sp);
}

// The dependencies inside the package, package-relative ([0] is the map file itself; a file
// outside the package is not part of what the compiled map can name)
static void CmPutDeps(CmWriter *W, const SceneData *d)
{
    size_t rootLen = strlen(d->root);
    for (int i = 1; i < d->depCount; i++) {
        if (strncmp(d->deps[i], d->root, rootLen) != 0) continue;
        uint32_t s = CmStr(W, d->deps[i] + rootLen);
        CmPut(W, CM_DEPS, &s);
    }
}

// The file from the written sections: the header, the section table (every section, in id
// order), then the sections in id order, each at a multiple of 8 with zero padding. *outSize and
// the malloc'd bytes, or NULL with a message (out of memory, too large).
static unsigned char *CmAssemble(const CmWriter *W, const SceneData *d, uint32_t srcNameOff,
                                 unsigned long long srcHash, long long srcSize, int *outSize)
{
    // The layout: each section's offset, and the file's size
    uint64_t total = sizeof(CmHeader) + CM_SECTIONS * sizeof(CmSection);
    uint32_t offset[CM_SECTIONS + 1];
    for (int id = 1; id <= CM_SECTIONS; id++) {
        total = (total + 7) & ~(uint64_t)7;
        offset[id] = (uint32_t)total;
        total += (uint64_t)W->sec[id].n;
    }

    // The buffer, zeroed (the padding), unless a writer ran out of memory or the file is too large
    unsigned char *out = NULL;
    if (W->oom) ErrSet("%s: out of memory", d->path);
    else if (total > MS_MAX_COMPILED_BYTES)
        ErrSet("%s: the compiled map would be larger than 256 MB", d->path);
    else if (!(out = calloc(1, (size_t)total))) ErrSet("%s: out of memory", d->path);
    if (!out) return NULL;

    // The header
    CmHeader h = {
        .version = CM_VERSION,
        .headerSize = sizeof(CmHeader),
        .byteOrder = CM_BYTE_ORDER,
        .formatVersion = (uint32_t)d->formatVersion,
        .sectionCount = CM_SECTIONS,
        .sectionTable = sizeof(CmHeader),
        .fileSize = total,
        .sourceHash = srcHash,
        .sourceSize = (uint64_t)srcSize,
        .sourceName = srcNameOff
    };
    memcpy(h.magic, CM_MAGIC, sizeof(CM_MAGIC));
    memcpy(out, &h, sizeof(h));

    // The section table entries and the sections
    for (int id = 1; id <= CM_SECTIONS; id++) {
        CmSection s = { (uint32_t)id, offset[id], W->count[id], CM_SECTION[id].size };
        memcpy(out + sizeof(CmHeader) + (id - 1) * sizeof(CmSection), &s, sizeof(s));
        if (W->sec[id].n) memcpy(out + offset[id], W->sec[id].b, (size_t)W->sec[id].n);
    }
    *outSize = (int)total;
    return out;
}

// The compiled file of d (parsed, before FinishParse) in a malloc'd buffer; srcName, srcHash,
// srcSize describe the map text it came from. NULL with a message on failure.
static unsigned char *WriteCompiled(const SceneData *d, const char *srcName, unsigned long long srcHash,
                                    long long srcSize, int *outSize)
{
    if (!IsLittleEndianHost()) {
        ErrSet("%s: compiled maps are little-endian; this host is not", d->path);
        return NULL;
    }

    // The sections. This order is the order of the strings in the string table: keep it.
    CmWriter W = { 0 };
    CmPut(&W, CM_STRINGS, "");      // offset 0: the empty string (the table starts with a NUL)
    uint32_t srcNameOff = CmStr(&W, srcName);
    CmPutStatements(&W, d);
    CmPutGroups(&W, d);
    CmPutMaterials(&W, d);
    CmPutBoxes(&W, d);
    CmPutModels(&W, d);
    CmPutPaint(&W, d);
    CmPutLights(&W, d);
    CmPutViewpoints(&W, d);
    CmPutSpawn(&W, d);
    CmPutEnvironment(&W, &d->env);
    CmPutDeps(&W, d);

    // The file, then the writer's buffers freed
    unsigned char *out = CmAssemble(&W, d, srcNameOff, srcHash, srcSize, outSize);
    for (int id = 1; id <= CM_SECTIONS; id++) free(W.sec[id].b);
    free(W.strSlot);
    return out;
}
#undef CM_XY
#undef CM_XYZ
#undef CM_RGBA
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

// The volume's region and size in voxels
static BoundingBox AoLayout(const SceneData *d, float voxel, int dim[3])
{
    BoundingBox region = BudgetRegion(d, voxel, AO_VOLUME_BUDGET, "occlusion volume");
    int max3D = Max3DTextureSize();
    for (int a = 0; a < 3; a++) {
        float size = ((const float *)&region.max)[a] - ((const float *)&region.min)[a];
        dim[a] = (int)ceilf(size / voxel);
        if (dim[a] < 2) dim[a] = 2;
        if (max3D > 2 && dim[a] > max3D) dim[a] = max3D;    // (float rounding; the region is cut already)
    }
    return region;
}

// One box into dist[], the voxels [wlo, wlo + wdim) of the volume (x fastest): each of them within
// AO_DIST_MAX of the box keeps the minimum of its value and its distance to the box (outside:
// the distance, inside: minus the depth). axis: scratch of wdim[0] + wdim[1] + wdim[2] floats.
// A voxel's value is the minimum over the boxes, whatever their order or the window.
static void AoSplatBox(const BoundingBox *box, const float *rmin, float voxel, const int wlo[3], const int wdim[3],
                       float *dist, float *axis)
{
    const float *bmin = (const float *)&box->min, *bmax = (const float *)&box->max;
    int lo[3], hi[3];
    float *ax[3] = { axis, axis + wdim[0], axis + wdim[0] + wdim[1] };
    for (int a = 0; a < 3; a++) {
        // voxel centres c = rmin + (i + 0.5) * voxel within AO_DIST_MAX of the box
        lo[a] = CellKey((bmin[a] - AO_DIST_MAX - rmin[a]) / voxel - 0.5f);
        hi[a] = -CellKey(-((bmax[a] + AO_DIST_MAX - rmin[a]) / voxel - 0.5f));  // = ceil
        if (lo[a] < wlo[a]) lo[a] = wlo[a];
        if (hi[a] > wlo[a] + wdim[a] - 1) hi[a] = wlo[a] + wdim[a] - 1;
        if (lo[a] > hi[a]) return;
        // per axis: outside distance (> 0), or minus the distance to the nearer face (<= 0)
        for (int i = lo[a]; i <= hi[a]; i++) {
            float c = rmin[a] + ((float)i + 0.5f) * voxel;
            ax[a][i - wlo[a]] = c < bmin[a] ? bmin[a] - c : c > bmax[a] ? c - bmax[a] : -fminf(c - bmin[a], bmax[a] - c);
        }
    }
    for (int z = lo[2]; z <= hi[2]; z++) {
        float dz = ax[2][z - wlo[2]];
        for (int y = lo[1]; y <= hi[1]; y++) {
            float dy = ax[1][y - wlo[1]];
            float oz = fmaxf(dz, 0.0f), oy = fmaxf(dy, 0.0f), syz = oy * oy + oz * oz;
            if (syz >= AO_DIST_MAX * AO_DIST_MAX) continue;
            float iyz = fmaxf(dy, dz);     // <= 0 only when inside on both axes
            float *row = dist + ((size_t)(z - wlo[2]) * wdim[1] + (y - wlo[1])) * wdim[0];
            for (int x = lo[0]; x <= hi[0]; x++) {
                float dx = ax[0][x - wlo[0]], e;
                if (dx <= 0.0f && iyz <= 0.0f) e = fmaxf(dx, iyz);     // inside: minus the depth
                else {
                    float ox = fmaxf(dx, 0.0f);
                    e = sqrtf(ox * ox + syz);
                }
                // union of boxes: the minimum (outside: the nearest box, inside: the deepest)
                if (e < row[x - wlo[0]]) row[x - wlo[0]] = e;
            }
        }
    }
}

// A turned box into dist[] the same way: the voxels within AO_DIST_MAX of its AABB `box`, each
// centre taken into the box's own frame for its distance to the box (outside: the distance,
// inside: minus the depth)
static void AoSplatTurned(const MS_TurnedBox *t, const BoundingBox *box, const float *rmin, float voxel, const int wlo[3],
                          const int wdim[3], float *dist)
{
    const float *bmin = (const float *)&box->min, *bmax = (const float *)&box->max;
    const float h[3] = { t->size.x * 0.5f, t->size.y * 0.5f, t->size.z * 0.5f };
    int lo[3], hi[3];
    for (int a = 0; a < 3; a++) {
        lo[a] = CellKey((bmin[a] - AO_DIST_MAX - rmin[a]) / voxel - 0.5f);
        hi[a] = -CellKey(-((bmax[a] + AO_DIST_MAX - rmin[a]) / voxel - 0.5f));
        if (lo[a] < wlo[a]) lo[a] = wlo[a];
        if (hi[a] > wlo[a] + wdim[a] - 1) hi[a] = wlo[a] + wdim[a] - 1;
        if (lo[a] > hi[a]) return;
    }
    for (int z = lo[2]; z <= hi[2]; z++)
        for (int y = lo[1]; y <= hi[1]; y++) {
            float *row = dist + ((size_t)(z - wlo[2]) * wdim[1] + (y - wlo[1])) * wdim[0];
            for (int x = lo[0]; x <= hi[0]; x++) {
                Vector3 c = { rmin[0] + ((float)x + 0.5f) * voxel, rmin[1] + ((float)y + 0.5f) * voxel, rmin[2] + ((float)z + 0.5f) * voxel };
                Vector3 r = Vector3Subtract(c, t->center);
                float q[3], out = 0.0f, in = -INFINITY;
                for (int a = 0; a < 3; a++) {
                    q[a] = fabsf(Vector3DotProduct(r, t->axis[a])) - h[a];
                    out += q[a] > 0.0f ? q[a] * q[a] : 0.0f;
                    in = fmaxf(in, q[a]);
                }
                float e = in <= 0.0f ? in : sqrtf(out);
                if (e < row[x - wlo[0]]) row[x - wlo[0]] = e;
            }
        }
}

// Box b of d into dist[] (AoSplatBox, or AoSplatTurned for a turned box)
static void AoSplat(const SceneData *d, int b, const float *rmin, float voxel, const int wlo[3], const int wdim[3],
                    float *dist, float *axis)
{
    const MS_TurnedBox *t = TurnedOf(d, b);
    if (t) AoSplatTurned(t, &d->boxes[b], rmin, voxel, wlo, wdim, dist);
    else AoSplatBox(&d->boxes[b], rmin, voxel, wlo, wdim, dist, axis);
}

static unsigned char AoByte(float dist)
{
    float v = (Clamp(dist, AO_DIST_MIN, AO_DIST_MAX) - AO_DIST_MIN) / (AO_DIST_MAX - AO_DIST_MIN);
    return (unsigned char)(v * 255.0f + 0.5f);
}

// The volume's bytes (malloc'd; NULL when out of memory, logged), its region and size
static unsigned char *AoBake(const SceneData *d, float voxel, BoundingBox *regionOut, int dim[3])
{
    BoundingBox region = AoLayout(d, voxel, dim);
    size_t count = (size_t)dim[0] * dim[1] * dim[2];
    unsigned char *grid = malloc(count);
    float *dist = malloc(count * sizeof(float));
    float *axis = malloc(sizeof(float) * (dim[0] + dim[1] + dim[2]));
    if (!grid || !dist || !axis) {
        free(grid); free(dist); free(axis);
        TraceLog(LOG_WARNING, "monolith_scene: occlusion volume: out of memory (%d x %d x %d)", dim[0], dim[1], dim[2]);
        return NULL;
    }
    // dist > 0: distance to the nearest box outside all boxes; <= 0: minus the depth inside
    for (size_t i = 0; i < count; i++) dist[i] = AO_DIST_MAX;
    static const int ZERO[3] = { 0, 0, 0 };
    for (int b = 0; b < d->boxCount; b++) AoSplat(d, b, (const float *)&region.min, voxel, ZERO, dim, dist, axis);
    for (size_t i = 0; i < count; i++) grid[i] = AoByte(dist[i]);
    free(dist); free(axis);
    *regionOut = region;
    return grid;
}

#ifndef MONOLITH_EDITOR
static bool BakeOcclusionVolume(const SceneData *d, float voxel, float intensity)
{
    double t0 = GetTime();
    BoundingBox region;
    int dim[3];
    unsigned char *grid = AoBake(d, voxel, &region, dim);
    if (!grid) return false;
    R3D_SetWorldOcclusion(grid, dim[0], dim[1], dim[2], region.min, voxel, AO_DIST_MIN, AO_DIST_MAX, intensity);
    TraceLog(LOG_INFO, "monolith_scene: occlusion volume %d x %d x %d (%.1f MB, %.2f m voxels) baked in %.0f ms",
             dim[0], dim[1], dim[2], (size_t)dim[0] * dim[1] * dim[2] / (1024.0 * 1024.0), voxel, (GetTime() - t0) * 1000.0);
    free(grid);
    return true;
}
#else
// The same, and the editor keeps the bytes (MS_EditSetText recomputes the voxels near a change)
static bool BakeOcclusionVolume(MS_Private *p, const SceneData *d, float voxel, float intensity)
{
    double t0 = GetTime();
    BoundingBox region;
    int dim[3];
    unsigned char *grid = AoBake(d, voxel, &region, dim);
    free(p->aoGrid);
    p->aoGrid = grid;
    if (!grid) return false;
    R3D_SetWorldOcclusion(grid, dim[0], dim[1], dim[2], region.min, voxel, AO_DIST_MIN, AO_DIST_MAX, intensity);
    TraceLog(LOG_INFO, "monolith_scene: occlusion volume %d x %d x %d (%.1f MB, %.2f m voxels) baked in %.0f ms",
             dim[0], dim[1], dim[2], (size_t)dim[0] * dim[1] * dim[2] / (1024.0 * 1024.0), voxel, (GetTime() - t0) * 1000.0);
    memcpy(p->aoDim, dim, sizeof(p->aoDim));
    p->aoRegion = region;
    p->aoVoxel = voxel;
    p->aoIntensity = intensity;
    return true;
}
#endif
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
            const MS_TurnedBox *t = TurnedOf(d, ib);
            r = &d->boxRefs[ib];
            if (t) tr = (MS_Transform){ t->center, t->rotationDeg, t->size };
            else {
                tr.position = Vector3Scale(Vector3Add(b.min, b.max), 0.5f);
                tr.scale = Vector3Subtract(b.max, b.min);
            }
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
static bool BuildColliderBvh(SceneData *d)
{
    int n;
    const BoundingBox *c = PublishedColliders(d, &n);
    if (n == 0) return true;
    d->colliderOrder = malloc(sizeof(int) * (size_t)n);
    d->colliderNodes = malloc(sizeof(BvhNode) * 2 * (size_t)n);
    if (!d->colliderOrder || !d->colliderNodes) {
        ErrSet("%s: out of memory (collider BVH)", d->path);
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
static bool ParseScene(SceneData *d, const char *text, int size)
{
    if (!DefaultMaterial(d)) {
        ErrSet("%s: out of memory", d->path);
        return false;
    }
    Parser P = { .d = d, .path = d->path };
    bool ok = ParseText(&P, text, size);
    free(P.lineBuf);
    free(P.tok);
    return ok;
}
#endif

// A material's texture (or a sampler's: filter TEX_TRILINEAR; a paint atlas: premul), loaded once per
// path, colour space, kind, wrap and filter
static Texture2D GetTextureEx(SceneData *d, const char *rel, bool isColor, bool data, bool premul, int wrap, int filter,
                              int line)
{
    char full[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(full, sizeof(full), "%s%s", d->root, rel);
#ifdef MONOLITH_EDITOR
    // (a canvas provided at another size than the cached texture, docs/painting.md 5.2 / 5.3: a
    // texture of the new size; the old one stays in the cache until the next load)
    const Image *prov = ProvidedImage(d->live, rel);
#endif
    for (int i = 0; i < d->textureCount; i++)
        if (strcmp(d->textures[i].path, full) == 0 && d->textures[i].isColor == isColor && d->textures[i].data == data &&
            d->textures[i].premul == premul && d->textures[i].wrap == wrap && d->textures[i].filter == filter) {
#ifdef MONOLITH_EDITOR
            if (prov && (d->textures[i].tex.width != prov->width || d->textures[i].tex.height != prov->height)) continue;
#endif
            return d->textures[i].tex;
        }
    char *path = MS_StrDup(full);
    if (!path || !MS_RESERVE(d->textures, d->textureCap, d->textureCount + 1)) {
        free(path);
        ErrSet("%s:%d: out of memory", d->srcPath, line);
        return (Texture2D){ 0 };
    }
    // R3D's default wrap is clamp, which would make uv_scale > 1 useless; map materials repeat
    // unless `wrap clamp|mirror`. Filter: trilinear with generated mipmaps (R3D's default).
    // A data_sampler's texture holds data, not colour (e.g. a tile baked by the editor's
    // --bake-surface, read with texelFetch): linear (no sRGB decode), nearest, no mipmaps.
    static const TextureWrap WRAPS[] = { TEXTURE_WRAP_REPEAT, TEXTURE_WRAP_CLAMP, TEXTURE_WRAP_MIRROR_REPEAT };
    double t0 = GetTime();
    TextureFilter tf = data ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_TRILINEAR;
    Texture2D tex = { 0 };
    if (premul) {
        // A paint atlas: RGBA8, premultiplied in linear light (PremulLut), from the provided pixels
        // or the file; the image keeps straight alpha
        Image img = { 0 };
#ifdef MONOLITH_EDITOR
        if (prov) img = ImageCopy(*prov);
        else
#endif
        img = LoadImage(full);
        if (img.data && img.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        if (img.data && img.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) {
            PremultiplyRgba8(img.data, (size_t)img.width * (size_t)img.height);
            tex = R3D_LoadTextureFromImageEx(img, WRAPS[wrap % 3], tf, true);
        }
        UnloadImage(img);
    } else {
#ifdef MONOLITH_EDITOR
        // A live edit uses the pixels provided for the path (a canvas in memory) instead of the file
        if (prov) tex = R3D_LoadTextureFromImageEx(*prov, WRAPS[wrap % 3], tf, isColor && !data);
        else
#endif
        tex = R3D_LoadTextureEx(full, WRAPS[wrap % 3], tf, isColor && !data);
    }
    // `filter nearest`: crisp texels up close (nearest magnification), and nearest texels of the
    // two nearest mipmaps blended at a distance (no shimmer); the mipmaps are the trilinear load's
    if (tex.id && filter == TEX_NEAREST && !data) {
        rlTextureParameters(tex.id, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_NEAREST);
        rlTextureParameters(tex.id, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_NEAREST_MIP_LINEAR);
    }
    d->times.textures += (GetTime() - t0) * 1000.0;
    if (tex.id == 0) {
        free(path);
        ErrSet("%s:%d: cannot load the texture \"%s\"", d->srcPath, line, rel);
        return tex;
    }
    TexEntry *e = &d->textures[d->textureCount++];
    e->path = path;
    e->isColor = isColor;
    e->data = data;
    e->premul = premul;
    e->wrap = wrap;
    e->filter = filter;
    e->tex = tex;
    return tex;
}

static Texture2D GetTexture(SceneData *d, const char *rel, bool isColor, bool data, int wrap, int filter, int line)
{
    return GetTextureEx(d, rel, isColor, data, false, wrap, filter, line);
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
#ifdef MONOLITH_EDITOR
            if (mp->triSrc) {
                unsigned int s = mp->triSrc[i];
                mp->triSrc[i] = mp->triSrc[j];
                mp->triSrc[j] = s;
            }
#endif
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
#ifdef MONOLITH_EDITOR
    free(mp->uv);
    free(mp->triSrc);
    free(mp->triAt);
    free(mp->meshTri);
#endif
    memset(mp, 0, sizeof(*mp));
}

#ifdef MONOLITH_EDITOR
// What surface picking needs besides (MS_RaycastSurface): each vertex's texture coordinate (none
// when no triangle mesh of the file has any: R3D's importer leaves them 0 then), each triangle's
// index in file order, and where each mesh's triangles start. Out of memory: none of it (the
// model still picks, without texture coordinates).
static void BuildModelPickUv(const R3D_Model *m, ModelPick *mp)
{
    mp->uv = malloc(sizeof(Vector2) * (size_t)mp->vertCount);
    mp->triSrc = malloc(sizeof(unsigned int) * (size_t)mp->triCount);
    mp->triAt = malloc(sizeof(unsigned int) * (size_t)mp->triCount);
    mp->meshTri = malloc(sizeof(int) * (size_t)(m->meshCount + 1));
    if (!mp->uv || !mp->triSrc || !mp->triAt || !mp->meshTri) {
        free(mp->uv); free(mp->triSrc); free(mp->triAt); free(mp->meshTri);
        mp->uv = NULL; mp->triSrc = mp->triAt = NULL; mp->meshTri = NULL;
        return;
    }
    mp->meshCount = m->meshCount;
    bool any = false;
    int v0 = 0, t = 0;
    for (int i = 0; i < m->meshCount; i++) {
        mp->meshTri[i] = t;
        if (m->meshes[i].primitiveType != R3D_PRIMITIVE_TRIANGLES) continue;
        const R3D_MeshData *md = &m->meshData[i];
        for (int k = 0; k < md->vertexCount; k++) {
            mp->uv[v0 + k] = R3D_UnpackTexCoord(md->vertices[k].texcoord);
            any = any || mp->uv[v0 + k].x != 0.0f || mp->uv[v0 + k].y != 0.0f;
        }
        t += md->indexCount > 0 ? md->indexCount / 3 : md->vertexCount / 3;
        v0 += md->vertexCount;
    }
    mp->meshTri[m->meshCount] = t;
    for (int k = 0; k < mp->triCount; k++) mp->triSrc[k] = (unsigned int)k;
    if (!any) {
        free(mp->uv);
        mp->uv = NULL;
    }
}
#endif

// Copies the triangles of a loaded model (R3D_IMPORT_RETAIN_MESH_DATA) into a ModelPick
// with a BVH, then frees R3D's CPU mesh data unless `keepData` (paint rebuilds the meshes from it)
static void BuildModelPick(MS_ModelFile *F, ModelPick *mp, bool keepData)
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
#ifdef MONOLITH_EDITOR
        BuildModelPickUv(m, mp);        // (before the BVH, which reorders triSrc with tri)
#endif
        BvhBuild(mp, 0, 0, tris, 0);
#ifdef MONOLITH_EDITOR
        for (int k = 0; mp->triSrc && k < tris; k++) mp->triAt[mp->triSrc[k]] = (unsigned int)k;
#endif
    } else if (tris > 0) FreeModelPick(mp);
    if (m->meshData && !keepData) {
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

static void PaintLoadUnwrap(SceneData *d, int file);

// Loads model file i of `d` (GPU). On failure: "map:line: message"
static bool LoadModelFile(SceneData *d, int i)
{
    MS_ModelFile *F = &d->modelFiles[i];
    char full[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(full, sizeof(full), "%s%s", d->root, F->path);
    F->model = R3D_LoadModelEx(full, R3D_IMPORT_RETAIN_MESH_DATA);
    if (F->model.meshCount <= 0 || !F->model.meshes) {
        ErrSet(MODEL_LOAD_ERROR, d->srcPath, F->line, F->path);
        return false;
    }
    F->loaded = true;
    F->bounds = F->model.aabb;
    F->meshCount = F->model.meshCount;
    F->materialCount = F->model.materialCount;
    BuildModelPick(F, &d->modelPick[i], d->filePaint && d->filePaint[i].keepData);
    if (d->filePaint) PaintLoadUnwrap(d, i);
    TraceLog(LOG_INFO, "monolith_scene: model %s: %d meshes, %d triangles, %d materials, size %.3f x %.3f x %.3f",
             F->path, F->meshCount, F->triangleCount, F->materialCount, F->bounds.max.x - F->bounds.min.x,
             F->bounds.max.y - F->bounds.min.y, F->bounds.max.z - F->bounds.min.z);
    return true;
}

// Loads every model file of `d` (GPU). On failure: "map:line: message"
static bool LoadModelFiles(SceneData *d)
{
    for (int i = 0; i < d->modelFileCount; i++)
        if (!LoadModelFile(d, i)) return false;
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
static bool CheckModelFiles(SceneData *d)
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
            ErrSet(MODEL_LOAD_ERROR, d->srcPath, F->line, F->path);
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
static bool ResolveModels(SceneData *d)
{
    RotBoundsCache cache = { 0 };
    int nc = d->boxCount;
    if (d->modelCount > 0) {
        d->colliders = malloc(sizeof(BoundingBox) * ((size_t)d->boxCount + (size_t)d->modelColliderCount));
        if (!d->colliders) {
            ErrSet("%s: out of memory", d->path);
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
    if (a->turned != b->turned) return a->turned < b->turned ? -1 : 1;
    if (a->cx != b->cx) return a->cx < b->cx ? -1 : 1;
    if (a->cz != b->cz) return a->cz < b->cz ? -1 : 1;
    return a->index < b->index ? -1 : a->index > b->index;
}

// Appends the ranges of items[0..count) (one group, sorted): a new one wherever the cell changes
static bool AddRanges(DrawRange **ranges, int *n, int *cap, int group, const CellItem *items, int count)
{
    for (int k = 0; k < count; k++) {
        const CellItem *it = &items[k];
        if (k == 0 || it->big != items[k - 1].big || it->turned != items[k - 1].turned || it->cx != items[k - 1].cx ||
            it->cz != items[k - 1].cz) {
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
#ifdef MONOLITH_EDITOR
    free(d->modelSlotOf);
    free(d->modelRangeOf);
    d->modelSlotOf = d->modelRangeOf = NULL;
#endif
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
static bool BuildModelGroups(SceneData *d)
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
        ErrSet("%s: out of memory", d->path);
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
        ErrSet("%s: out of memory", d->path);
        return false;
    }
    for (int i = 0; i < d->modelCount; i++) items[i] = MakeCellItem(groupOf[i], i, d->models[i].bounds, MS_MODEL_CELL_SIZE);
    qsort(items, d->modelCount, sizeof(CellItem), CompareCellItems);
#ifdef MONOLITH_EDITOR
    // (the preview moves single instances: where each one sits; out of memory: no preview)
    d->modelSlotOf = malloc(sizeof(int) * d->modelCount);
    d->modelRangeOf = malloc(sizeof(int) * d->modelCount);
#endif

    bool ok = true;
    for (int g = 0, at = 0; g < d->modelGroupCount && ok; at += d->modelGroups[g++].count) {
        ModelGroup *G = &d->modelGroups[g];
        const MS_ModelFile *F = &d->modelFiles[G->file];
        G->model = F->model;
        G->model.materials = malloc(sizeof(R3D_Material) * (F->materialCount > 0 ? F->materialCount : 1));
        if (!G->slotMat || !G->model.materials) { ok = false; break; }
        // Paint (docs/painting.md 4.4): a group with a slot drawn with a paint shader carries its
        // instances' first paint key (x; 0: none) and the draw kind 2 (y); a painted file is drawn
        // from its paint meshes (slot and paint UV in the bone channels)
        const FilePaint *FP = d->filePaint ? &d->filePaint[G->file] : NULL;
        if (FP && FP->meshes) G->model.meshes = FP->meshes;
        bool painted = false;
        for (int s = 0; s < F->materialCount; s++) {
            const R3D_Material *own = &F->model.materials[s];
            int mi = G->slotMat[s];
            if (mi >= 0) painted = painted || d->matDefs[mi].painted;
            if (mi < 0) {
                G->model.materials[s] = *own;
                int a = OwnAtlasFind(d, G->file, s);
                if (a >= 0 && d->ownAtlases[a].painted && d->ownAtlases[a].shader) {
                    G->model.materials[s].shader = d->ownAtlases[a].shader;
                    painted = true;
                }
                continue;
            }
            R3D_Material m = d->materials[mi].material;
            unsigned inh = d->matDefs[mi].inherit;
            if (inh & INHERIT_ALBEDO) m.albedo.texture = own->albedo.texture;
            if (inh & INHERIT_NORMAL) m.normal = own->normal;
            if (inh & INHERIT_ORM) m.orm.texture = own->orm.texture;
            if (inh & INHERIT_EMISSION) m.emission = own->emission;
            G->model.materials[s] = m;
        }
        if (!G->castShadow) {
            const R3D_Mesh *src = G->model.meshes;
            G->model.meshes = malloc(sizeof(R3D_Mesh) * F->model.meshCount);
            if (!G->model.meshes) { G->model.meshes = F->model.meshes; ok = false; break; }
            G->ownMeshes = true;
            for (int k = 0; k < F->model.meshCount; k++) {
                G->model.meshes[k] = src[k];
                G->model.meshes[k].shadowCastMode = R3D_SHADOW_CAST_DISABLED;
            }
        }
        int count = G->count;
        G->inst = R3D_LoadInstanceBuffer(count, R3D_INSTANCE_POSITION | R3D_INSTANCE_ROTATION | R3D_INSTANCE_SCALE |
                                                (painted ? R3D_INSTANCE_CUSTOM : 0));
        G->instLoaded = true;
        if (G->inst.capacity < count) { ok = false; break; }
        if (painted) {
            Vector4 *cus = R3D_MapInstances(G->inst, R3D_INSTANCE_CUSTOM, false);
            for (int k = 0; k < count && cus; k++) {
                int i = items[at + k].index;
                cus[k] = (Vector4){ d->modelPaintKey ? (float)d->modelPaintKey[i] : 0.0f, 2.0f, 0.0f, 0.0f };
            }
            if (cus) R3D_UnmapInstances(G->inst, R3D_INSTANCE_CUSTOM);
            else ok = false;
        }
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
#ifdef MONOLITH_EDITOR
        int rangesBefore = d->modelRangeCount;
#endif
        if (ok && !AddRanges(&d->modelRanges, &d->modelRangeCount, &d->modelRangeCap, g, items + at, count)) ok = false;
#ifdef MONOLITH_EDITOR
        for (int r = rangesBefore; ok && d->modelSlotOf && d->modelRangeOf && r < d->modelRangeCount; r++)
            for (int k = 0; k < d->modelRanges[r].count; k++) {
                int i = items[at + d->modelRanges[r].first + k].index;
                d->modelSlotOf[i] = d->modelRanges[r].first + k;
                d->modelRangeOf[i] = r;
            }
#endif
    }
    free(items);
    free(groupOf);
    if (!ok) ErrSet("%s: out of memory (model instances)", d->path);
    return ok;
}

// The unit cubes of the materials' face layouts (`uv_faces`): one per distinct layout, in material
// order; each material with a layout gets its cube (MatDef.faceCube). Materials without one draw
// R3D's cube as before. False when out of memory.
static bool BuildFaceCubes(SceneData *d)
{
    for (int i = 0; i < d->materialCount; i++) {
        MatDef *D = &d->matDefs[i];
        D->faceCube = 0;
        if (!D->hasFaces) continue;
        int c = 0;
        while (c < d->faceCubeCount && memcmp(d->faceCubes[c].cells, D->uvFaces, 6) != 0) c++;
        if (c == d->faceCubeCount) {
            FaceCube *F = MS_PUSH(d->faceCubes, d->faceCubeCount, d->faceCubeCap);
            if (!F) return false;
            *F = (FaceCube){ .data = R3D_GenMeshDataCube(1.0f, 1.0f, 1.0f) };
            memcpy(F->cells, D->uvFaces, 6);
            if (F->data.vertexCount != 24 || F->data.indexCount != 36) return false;
            FaceLayoutData(&F->data, F->cells);
            BoundingBox aabb = { { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f } };   // (R3D_GenMeshCube's)
            F->mesh = R3D_LoadMesh(R3D_PRIMITIVE_TRIANGLES, F->data, &aabb);
            if (F->mesh.vertexCount == 0) return false;
        }
        D->faceCube = c + 1;
    }
    return true;
}

static void FreeFaceCubes(SceneData *d)
{
    for (int c = 0; c < d->faceCubeCount; c++) {
        R3D_UnloadMeshData(d->faceCubes[c].data);
        R3D_UnloadMesh(d->faceCubes[c].mesh);
    }
    free(d->faceCubes);
    d->faceCubes = NULL;
    d->faceCubeCount = d->faceCubeCap = 0;
}

// The unit cube data material m's boxes are meshed from: its face layout's, else R3D's (`cube`)
static inline const R3D_MeshData *MaterialCube(const SceneData *d, int m, const R3D_MeshData *cube)
{
    int c = d->matDefs[m].faceCube;
    return c > 0 ? &d->faceCubes[c - 1].data : cube;
}

static void FreePaintMeshes(SceneData *d);

static void FreeGpu(SceneData *d)
{
    if (!d->gpu) return;
    FreeModelGroups(d);
    FreeFaceCubes(d);
    FreePaintMeshes(d);
    for (int a = 0; a < d->ownAtlasCount; a++) {
        if (d->ownAtlases[a].shader) R3D_UnloadSurfaceShader(d->ownAtlases[a].shader);
        d->ownAtlases[a].shader = NULL;
        d->ownAtlases[a].tex = (Texture2D){ 0 };       // (in the texture cache: freed below)
    }
    if (d->paintTableTex.id) rlUnloadTexture(d->paintTableTex.id);
    d->paintTableTex = (Texture2D){ 0 };
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
    for (int i = 0; i < d->shaderFileCount; i++) {
        if (d->shaderFiles[i].shader) R3D_UnloadSurfaceShader(d->shaderFiles[i].shader);
        if (d->shaderFiles[i].paint) R3D_UnloadSurfaceShader(d->shaderFiles[i].paint);
    }
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
#ifdef MONOLITH_EDITOR
    free(d->hidden);
    free(d->boxOrder);
    free(d->boxRangeOf);
    d->hidden = NULL;
    d->boxOrder = d->boxRangeOf = NULL;
    d->boxOrderCount = 0;
#endif
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
        if (m < 0 || m >= d->materialCount || !d->matDefs[m].merge || TurnedOf(d, i)) continue;   // (a turned box shows every face)
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

// The occluder grid of FindHiddenFaces: the boxes as Box6, the big ones, and per occupied cell its
// occluders. slots is NULL when there are more than HIDE_ENTRIES_MAX entries (then nothing is hidden)
typedef struct {
    Box6 *bx;
    int *big, bigCount;
    HideSlot *slots;
    unsigned int slotMask;
    int *ids;
    Box6 *gbox;
} HideGrid;

static void HideGridFree(HideGrid *g)
{
    free(g->gbox);
    free(g->ids);
    free(g->slots);
    free(g->big);
    free(g->bx);
    memset(g, 0, sizeof(*g));
}

// Builds the grid of d's occluders (skip, may be NULL: boxes left out as occluders). False when
// out of memory (g freed)
static bool HideGridBuild(const SceneData *d, const unsigned char *skip, HideGrid *g)
{
    memset(g, 0, sizeof(*g));
    int count = d->boxCount;
    size_t cap = count > 0 ? (size_t)count : 1;
    g->bx = malloc(sizeof(Box6) * cap);
    g->big = malloc(sizeof(int) * cap);
    bool ok = g->bx && g->big;
    long long total = 0;
    for (int i = 0; i < count && ok; i++) {
        const BoundingBox *B = &d->boxes[i];
        g->bx[i] = (Box6){ { B->min.x, B->min.y, B->min.z }, { B->max.x, B->max.y, B->max.z } };
        int m = d->boxRefs[i].material, lo[3], hi[3];
        // (a turned box hides nothing: its AABB is not its shape)
        if (m < 0 || m >= d->materialCount || !d->matDefs[m].occluder || (skip && skip[i]) || TurnedOf(d, i)) continue;
        if (!HideCells(&g->bx[i], lo, hi)) g->big[g->bigCount++] = i;
        else total += (long long)(hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1);
    }
    if (ok && total <= HIDE_ENTRIES_MAX) {
        // Two passes over the occluders' cells: count per cell, then place each box after the
        // cell's earlier ones (ascending box order within a cell)
        unsigned int nslots = 2;
        while (nslots < 2 * (unsigned long long)total) nslots <<= 1;
        g->slotMask = nslots - 1;
        g->slots = calloc(nslots, sizeof(HideSlot));
        g->ids = malloc(sizeof(int) * (total > 0 ? (size_t)total : 1));
        g->gbox = malloc(sizeof(Box6) * (total > 0 ? (size_t)total : 1));
        ok = g->slots && g->ids && g->gbox;
        HideSlot *slots = g->slots;
        for (int pass = 0; pass < 2 && ok; pass++) {
            for (unsigned int h = 0, at = 0; pass == 1 && h < nslots; h++) {
                slots[h].first = (int)at;           // (advanced while filling, set back below)
                at += (unsigned int)slots[h].count;
            }
            for (int i = 0; i < count; i++) {
                int m = d->boxRefs[i].material, lo[3], hi[3], cell[3];
                if (m < 0 || m >= d->materialCount || !d->matDefs[m].occluder || (skip && skip[i]) || TurnedOf(d, i) ||
                    !HideCells(&g->bx[i], lo, hi))
                    continue;
                for (cell[0] = lo[0]; cell[0] <= hi[0]; cell[0]++)
                    for (cell[1] = lo[1]; cell[1] <= hi[1]; cell[1]++)
                        for (cell[2] = lo[2]; cell[2] <= hi[2]; cell[2]++) {
                            unsigned long long key = HideKey(cell);
                            unsigned int h = HideHash(key, g->slotMask);
                            while (slots[h].count && slots[h].key != key) h = (h + 1) & g->slotMask;
                            if (pass == 0) {
                                slots[h].key = key;
                                slots[h].count++;
                            } else {
                                int e = slots[h].first++;
                                g->ids[e] = i;
                                g->gbox[e] = g->bx[i];
                            }
                        }
            }
        }
        for (unsigned int h = 0; ok && h < nslots; h++) slots[h].first -= slots[h].count;
    }
    if (!ok) HideGridFree(g);
    return ok;
}

// The job over grid g: the face axes and directions from R3D's cube (its face order)
static void HideJobInit(HideJob *J, const SceneData *d, const HideGrid *g, const R3D_MeshData *cube, unsigned char *hidden)
{
    memset(J, 0, sizeof(*J));
    J->d = d;
    J->bx = g->bx;
    J->big = g->big;
    J->bigCount = g->bigCount;
    J->slots = g->slots;
    J->slotMask = g->slotMask;
    J->ids = g->ids;
    J->gbox = g->gbox;
    J->hidden = hidden;
    atomic_init(&J->next, 0);
    for (int f = 0; f < 6; f++) {
        Vector3 nrm = R3D_UnpackNormal(cube->vertices[f * 4].normal);
        J->axis[f] = fabsf(nrm.x) > 0.5f ? 0 : fabsf(nrm.y) > 0.5f ? 1 : 2;
        J->dir[f] = (J->axis[f] == 0 ? nrm.x : J->axis[f] == 1 ? nrm.y : nrm.z) > 0.0f ? 1 : -1;
    }
}

// hidden[b] bit f: face f of box b (R3D's cube face order) is hidden; only for boxes of merged
// materials. Each box's faces depend only on the boxes, so threads take boxes in chunks and the
// result is the same as one thread's. False when out of memory.
static bool FindHiddenFaces(const SceneData *d, const R3D_MeshData *cube, unsigned char *hidden)
{
    int count = d->boxCount;
    HideGrid g;
    bool ok = HideGridBuild(d, NULL, &g);
    if (ok && g.slots) {
        HideJob J;
        HideJobInit(&J, d, &g, cube, hidden);
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
    HideGridFree(&g);
    return ok;
}

// The six faces of turned box t appended to md (room for 24 vertices, 36 indices): the cube scaled
// by its size, turned, moved to its centre (the arithmetic of an instanced cube's rotation, so it
// lies where its instance would); normals and tangents turned with it (UVs as the cube's: the box
// is textured in its own frame)
static void TurnedFaces(R3D_MeshData *md, const MS_TurnedBox *t, const R3D_MeshData *cube)
{
    Quaternion q = RotationYpr(t->rotationDeg);
    Vector3 qv = { q.x, q.y, q.z };
    for (int f = 0; f < 6; f++) {
        unsigned int base = (unsigned int)md->vertexCount;
        for (int c = 0; c < 4; c++) {
            R3D_Vertex vx = cube->vertices[f * 4 + c];
            Vector3 v = { vx.position.x * t->size.x, vx.position.y * t->size.y, vx.position.z * t->size.z };
            Vector3 tv = Vector3Scale(Vector3CrossProduct(qv, v), 2.0f);
            vx.position = Vector3Add(Vector3Add(Vector3Add(v, Vector3Scale(tv, q.w)), Vector3CrossProduct(qv, tv)), t->center);
            Vector3 n = R3D_UnpackNormal(vx.normal);
            Vector4 tg = R3D_UnpackTangent(vx.tangent);
            Vector3 tn = TurnedDir(t, tg.x, tg.y, tg.z);
            R3D_PackNormal(vx.normal, TurnedDir(t, n.x, n.y, n.z));
            R3D_PackTangent(vx.tangent, (Vector4){ tn.x, tn.y, tn.z, tg.w });
            md->vertices[md->vertexCount++] = vx;
        }
        for (int j = 0; j < 6; j++) md->indices[md->indexCount++] = base + cube->indices[f * 6 + j] - (unsigned int)(f * 4);
    }
}

// One mesh of the visible faces of the boxes items[0..R->count): R3D's own unit-cube faces (vertex
// format, UVs, normals, winding), each corner placed the way the GPU placed an instanced cube's,
// local * scale + position with the same position and scale floats. A material with a face layout
// uses its own cube (the same, UVs in their cells: MaterialCube).
static bool BuildBoxMesh(SceneData *d, DrawRange *R, const CellItem *items, const unsigned char *hidden, const R3D_MeshData *cube)
{
    cube = MaterialCube(d, R->group, cube);
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
    // A painted material's painted boxes: each face's paint key and corner UV in the bone channels
    // (docs/painting.md 4.4); every other vertex keeps the cube's (zero: key 0, no paint)
    const int *paintKey = d->matDefs[R->group].painted ? d->boxPaintKey : NULL;
    for (int k = 0; k < R->count; k++) {
        const BoundingBox *B = &d->boxes[items[k].index];
        const MS_TurnedBox *t = TurnedOf(d, items[k].index);
        int key = paintKey ? paintKey[items[k].index] : 0;
        if (t) {
            int first = md.vertexCount;
            TurnedFaces(&md, t, cube);
            for (int v = 0; key > 0 && v < 24; v++) {
                const R3D_Vertex *cv = &cube->vertices[v];
                int face = FaceOfNormal(R3D_UnpackNormal(cv->normal));
                PaintBones(&md.vertices[first + v], (unsigned int)(key + face), CubeFaceUv(face, cv->position));
            }
            continue;
        }
        Vector3 pos = Vector3Scale(Vector3Add(B->min, B->max), 0.5f), scl = Vector3Subtract(B->max, B->min);
        for (int f = 0; f < 6; f++) {
            if (hidden[items[k].index] >> f & 1) continue;
            unsigned int base = (unsigned int)md.vertexCount;
            for (int c = 0; c < 4; c++) {
                R3D_Vertex vx = cube->vertices[f * 4 + c];
                if (key > 0) {
                    int face = FaceOfNormal(R3D_UnpackNormal(vx.normal));
                    PaintBones(&vx, (unsigned int)(key + face), CubeFaceUv(face, vx.position));
                }
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

// The instance buffer of material m's boxes items[0..count): unit cubes at the boxes' centres,
// scaled by their sizes; a material with a turned box also gets the rotations (identity for its
// axis-aligned boxes), the others keep a buffer without them. False when out of memory.
static bool LoadBoxInstances(SceneData *d, int m, const CellItem *items, int count)
{
    bool turned = false;
    for (int k = 0; k < count && !turned; k++) turned = items[k].turned;
    // A painted material's boxes carry their first paint key (x; 0: none) and the draw kind 1 (y) in
    // the custom attribute (docs/painting.md 4.4); other buffers have none
    bool painted = d->matDefs[m].painted && d->boxPaintKey;
    R3D_InstanceFlags flags = R3D_INSTANCE_POSITION | R3D_INSTANCE_SCALE | (turned ? R3D_INSTANCE_ROTATION : 0) |
                              (painted ? R3D_INSTANCE_CUSTOM : 0);
    d->inst[m] = R3D_LoadInstanceBuffer(count, flags);
    if (d->inst[m].capacity < count) return false;
    Vector3 *pos = R3D_MapInstances(d->inst[m], R3D_INSTANCE_POSITION, false);
    Vector3 *scl = R3D_MapInstances(d->inst[m], R3D_INSTANCE_SCALE, false);
    Quaternion *rot = turned ? R3D_MapInstances(d->inst[m], R3D_INSTANCE_ROTATION, false) : NULL;
    Vector4 *cus = painted ? R3D_MapInstances(d->inst[m], R3D_INSTANCE_CUSTOM, false) : NULL;
    bool ok = pos && scl && (!turned || rot) && (!painted || cus);
    for (int k = 0; k < count && ok && cus; k++) cus[k] = (Vector4){ (float)d->boxPaintKey[items[k].index], 1.0f, 0.0f, 0.0f };
    if (cus) R3D_UnmapInstances(d->inst[m], R3D_INSTANCE_CUSTOM);
    for (int k = 0; k < count && ok; k++) {
        const MS_TurnedBox *t = TurnedOf(d, items[k].index);
        if (t) {
            pos[k] = t->center;
            scl[k] = t->size;
            rot[k] = RotationYpr(t->rotationDeg);
            continue;
        }
        const BoundingBox *B = &d->boxes[items[k].index];
        pos[k] = Vector3Scale(Vector3Add(B->min, B->max), 0.5f);
        scl[k] = Vector3Subtract(B->max, B->min);
        if (rot) rot[k] = QuaternionIdentity();
    }
    R3D_UnmapInstances(d->inst[m], (pos ? R3D_INSTANCE_POSITION : 0) | (scl ? R3D_INSTANCE_SCALE : 0) | (rot ? R3D_INSTANCE_ROTATION : 0));
    return ok;
}

#ifdef MONOLITH_EDITOR
// What the next live edit compares with: the hidden faces (taken over from *hidden), the boxes in
// draw order (items[0..n), sorted) and each box's range. Out of memory: left NULL (the edit then
// rebuilds every box draw)
static void KeepBoxIndex(SceneData *d, const CellItem *items, int n, unsigned char **hidden)
{
    d->hidden = *hidden;
    *hidden = NULL;
    d->boxOrder = malloc(sizeof(int) * (n > 0 ? (size_t)n : 1));
    d->boxRangeOf = malloc(sizeof(int) * (d->boxCount > 0 ? (size_t)d->boxCount : 1));
    if (!d->boxOrder || !d->boxRangeOf) {
        free(d->boxOrder);
        free(d->boxRangeOf);
        d->boxOrder = d->boxRangeOf = NULL;
        return;
    }
    d->boxOrderCount = n;
    for (int k = 0; k < n; k++) d->boxOrder[k] = items[k].index;
    for (int b = 0; b < d->boxCount; b++) d->boxRangeOf[b] = -1;
    int at = 0, m = 0;          // items of material m start at the sum of the counts before it
    for (int r = 0; r < d->boxRangeCount; r++) {
        const DrawRange *R = &d->boxRanges[r];
        while (m < R->group) at += d->instCount[m++];
        for (int k = 0; k < R->count; k++) d->boxRangeOf[items[at + R->first + k].index] = r;
    }
}
#endif

// Boxes -> draws per material and 32 m cell (boxRanges: one cluster each). A material that is
// `merge` gets, per range, one mesh of its boxes' visible faces (BuildBoxMesh): fewer triangles,
// and it casts the same shadow (a hidden face lies inside an opaque box, so it was never the
// nearest depth of any shadow texel). The others get an instance buffer of unit cubes (centre and
// size per box) shared by their ranges. `unit` draws a mesh as an instance (see MS_DrawGeometry).
static bool UploadBoxes(SceneData *d)
{
    CellItem *items = malloc(sizeof(CellItem) * (d->boxCount > 0 ? d->boxCount : 1));
    if (!items) {
        ErrSet("%s: out of memory", d->path);
        return false;
    }
    bool anyMerge = false;
    int n = 0;
    for (int b = 0; b < d->boxCount; b++) {
        int m = d->boxRefs[b].material;
        if (m < 0 || m >= d->materialCount) continue;
        items[n] = MakeCellItem(m, b, d->boxes[b], MS_CELL_SIZE);
        items[n++].turned = TurnedOf(d, b) != NULL;
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
            if (!LoadBoxInstances(d, m, items + at, count)) {
                ok = false;
                if (d->inst[m].capacity < count) break;
            }
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
#ifdef MONOLITH_EDITOR
    if (ok) KeepBoxIndex(d, items, n, &hidden);
#endif
    free(hidden);
    free(items);
    if (!ok) ErrSet("%s: out of memory (box meshes)", d->path);
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

// The source of a shader file as R3D gets it (heap; NULL when unreadable): the load's shader
// defines (when it has any) go first, so the source's #ifdef blocks see them (R3D copies a
// #define like any other global code)
static char *ShaderSourceText(const SceneData *d, const char *rel)
{
    char full[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(full, sizeof(full), "%s%s", d->root, rel);
    char *text = LoadFileText(full);
    if (!text) return NULL;
    const char *defs = "";
#ifdef MONOLITH_EDITOR
    if (d->shaderDefines && (!d->shaderDefinesFile || strcmp(d->shaderDefinesFile, rel) == 0)) defs = d->shaderDefines;
#endif
    size_t a = strlen(defs), b = strlen(text);
    char *code = malloc(a + b + 2);
    if (code) {
        memcpy(code, defs, a);
        code[a] = '\n';
        memcpy(code + a + 1, text, b + 1);
        if (a == 0) memmove(code, code + 1, b + 1);     // (no defines: the file as it is)
    }
    UnloadFileText(text);
    return code;
}

// A material's shader file (rel NULL: the empty shader of painted materials without one): found or
// added with its source scanned. NULL when out of memory (the last error says so).
static ShaderFile *ShaderFileOf(SceneData *d, const char *rel, int line)
{
    for (int i = 0; i < d->shaderFileCount; i++) {
        const char *r = d->shaderFiles[i].rel;
        if (rel ? r && strcmp(r, rel) == 0 : !r) return &d->shaderFiles[i];
    }
    ShaderFile *f = MS_PUSH(d->shaderFiles, d->shaderFileCount, d->shaderFileCap);
    if (!f) {
        ErrSet("%s:%d: out of memory", d->srcPath, line);
        return NULL;
    }
    *f = (ShaderFile){ .rel = rel, .scanned = true };
    if (!rel) return f;
    char full[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(full, sizeof(full), "%s%s", d->root, rel);
    char *src = LoadFileText(full);
    f->scanned = src != NULL;
    if (src) ScanShader(src, &f->needsCube, &f->cutout);
    else f->needsCube = f->cutout = true;
    UnloadFileText(src);
    return f;
}

// The compiled shader of a material's shader file: compiled by the first material that draws with
// it. NULL when it fails to load or out of memory (the last error says which).
static ShaderFile *GetShaderFile(SceneData *d, const MS_Material *M, const MatDef *D)
{
    ShaderFile *f = ShaderFileOf(d, M->shader, D->shaderLine);
    if (!f || f->shader) return f;
    double t0 = GetTime();
#ifdef MONOLITH_EDITOR
    if (d->shaderDefines && (!d->shaderDefinesFile || strcmp(d->shaderDefinesFile, M->shader) == 0)) {
        char *code = ShaderSourceText(d, M->shader);
        f->shader = code ? R3D_LoadSurfaceShaderFromMemory(code) : NULL;
        free(code);
    } else
#endif
    {
        char full[MS_PATH_MAX + MS_PATH_MAX];
        snprintf(full, sizeof(full), "%s%s", d->root, M->shader);
        f->shader = R3D_LoadSurfaceShader(full);
    }
    d->times.shaderLoads++;
    d->times.shaders += (GetTime() - t0) * 1000.0;
    if (!f->shader) {
        ErrSet("%s:%d: cannot load the surface shader \"%s\" (see the log)", d->srcPath, D->shaderLine, M->shader);
        return NULL;
    }
    return f;
}

// The paint version of a shader file (docs/painting.md 4.3), compiled the first time a painted
// material needs it. NULL when the source can't take the paint stage (more than 2 samplers, too
// long for R3D) or doesn't compile: its materials then draw without paint (logged).
static R3D_SurfaceShader *PaintShaderOf(SceneData *d, ShaderFile *f)
{
    if (f->paintTried) return f->paint;
    f->paintTried = true;
    double t0 = GetTime();
    char *src = f->rel ? ShaderSourceText(d, f->rel) : NULL;
    char *code = (!f->rel || src) ? PaintShaderSource(src) : NULL;
    if (code && strlen(code) <= PAINT_SHADER_MAX) f->paint = R3D_LoadSurfaceShaderFromMemory(code);
    if (!f->paint)
        TraceLog(LOG_WARNING, "monolith_scene: %s: the surface shader \"%s\" can't take the paint stage (%s); its paint is "
                 "not drawn", d->srcPath, f->rel ? f->rel : "(none)", !code ? "more than 2 samplers, or unreadable"
                 : strlen(code) > PAINT_SHADER_MAX ? "longer than R3D's limit with it" : "see the log");
    free(code);
    free(src);
    d->times.shaderLoads++;
    d->times.shaders += (GetTime() - t0) * 1000.0;
    return f->paint;
}

// --- The paint layer at load and upload (docs/painting.md 4). Nothing here runs for a map without
// an active paint entry: no shader, texture, vertex data or instance attribute is added.

// The size of a paint atlas image: the pixels provided in memory, else the file's header
static void PaintAtlasSize(const SceneData *d, const char *rel, int *w, int *h)
{
    *w = *h = 0;
#ifdef MONOLITH_EDITOR
    const Image *prov = ProvidedImage(d->live, rel);
    if (prov) {
        *w = prov->width;
        *h = prov->height;
        return;
    }
#endif
    char full[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(full, sizeof(full), "%s%s", d->root, rel);
    ImageFileSize(full, w, h);
}

// Before the model files load: their paint state, and which keep their CPU mesh data (builds with
// the text parser: every file, for unwraps and picking; others: the files a paint entry names)
static void PaintPrepare(SceneData *d)
{
    if (d->modelFileCount == 0 || !(d->filePaint = calloc((size_t)d->modelFileCount, sizeof(FilePaint)))) return;
    for (int f = 0; f < d->modelFileCount; f++) {
#ifdef MONOLITH_EDITOR
        d->filePaint[f].keepData = true;
#endif
    }
    for (int i = 0; i < d->paintCount; i++)
        if (d->paints[i].kind == PAINT_MODEL) d->filePaint[d->models[d->paints[i].object].file].keepData = true;
}

static void FreeFilePaint(FilePaint *FP)
{
    UnwrapFree(&FP->unwrap);
    free(FP->uvKind);
    free(FP->meshes);       // (the meshes themselves: FreePaintMeshes, GPU)
    memset(FP, 0, sizeof(*FP));
}

#ifdef MONOLITH_EDITOR
// A deep copy (the editor's provided unwraps, a live edit's files)
static bool UnwrapCopy(Unwrap *dst, const Unwrap *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->fileHash = src->fileHash;
    if (!(dst->meshes = calloc(src->meshCount > 0 ? (size_t)src->meshCount : 1, sizeof(UnwrapMesh)))) return false;
    for (int m = 0; m < src->meshCount; m++) {
        const UnwrapMesh *S = &src->meshes[m];
        UnwrapMesh *D = &dst->meshes[m];
        dst->meshCount = m + 1;
        *D = (UnwrapMesh){ S->slot, S->vertexCount, S->indexCount, NULL, NULL, NULL };
        D->source = malloc(sizeof(unsigned int) * (S->vertexCount > 0 ? (size_t)S->vertexCount : 1));
        D->uv = malloc(sizeof(unsigned short) * 2 * (S->vertexCount > 0 ? (size_t)S->vertexCount : 1));
        D->indices = malloc(sizeof(unsigned int) * (S->indexCount > 0 ? (size_t)S->indexCount : 1));
        if (!D->source || !D->uv || !D->indices) { UnwrapFree(dst); return false; }
        memcpy(D->source, S->source, sizeof(unsigned int) * (size_t)S->vertexCount);
        memcpy(D->uv, S->uv, sizeof(unsigned short) * 2 * (size_t)S->vertexCount);
        memcpy(D->indices, S->indices, sizeof(unsigned int) * (size_t)S->indexCount);
    }
    return true;
}

// The unwrap provided for a model file (MS_PaintUnwrapProvide), NULL if none
static const ProvidedUnwrap *ProvidedUnwrapOf(const MS_Private *p, const char *rel)
{
    for (int i = 0; p && i < p->unwrapCount; i++)
        if (strcmp(p->unwraps[i].rel, rel) == 0) return &p->unwraps[i];
    return NULL;
}
#endif

// After model file `file` loaded (with its CPU mesh data): its unwrap, provided in memory or read
// from its sidecar, kept when it fits the file as loaded (the model's hash and its meshes); a stale
// or broken sidecar is ignored (logged)
static void PaintLoadUnwrap(SceneData *d, int file)
{
    FilePaint *FP = &d->filePaint[file];
    const MS_ModelFile *F = &d->modelFiles[file];
    UnwrapFree(&FP->unwrap);
    FP->hasUnwrap = false;
    FP->unwrapGen = 0;
    if (!FP->keepData || !F->loaded || !F->model.meshData) return;
#ifdef MONOLITH_EDITOR
    const ProvidedUnwrap *pu = ProvidedUnwrapOf(d->live, F->path);
    if (pu) {
        FP->hasUnwrap = UnwrapMatches(&pu->u, &F->model) && UnwrapCopy(&FP->unwrap, &pu->u);
        FP->unwrapGen = pu->gen;
        if (!FP->hasUnwrap) TraceLog(LOG_WARNING, "monolith_scene: %s: the provided paint unwrap doesn't fit it; ignored", F->path);
        return;
    }
#endif
    char side[MS_PATH_MAX], full[MS_PATH_MAX + MS_PATH_MAX], model[MS_PATH_MAX + MS_PATH_MAX];
    if (!UnwrapSidecarPath(F->path, side, sizeof(side))) return;
    snprintf(full, sizeof(full), "%s%s", d->root, side);
    if (!FileExists(full)) return;
    snprintf(model, sizeof(model), "%s%s", d->root, F->path);
    unsigned long long hash = 0;
    bool read = UnwrapRead(full, &FP->unwrap);
    bool fresh = read && FileHash(model, &hash) && hash == FP->unwrap.fileHash;
    FP->hasUnwrap = fresh && UnwrapMatches(&FP->unwrap, &F->model);
    if (!FP->hasUnwrap) {
        TraceLog(LOG_WARNING, "monolith_scene: %s: %s; ignored", side, !read ? "not a paint unwrap file"
                 : !fresh ? "made for another version of the model file" : "doesn't fit the model's meshes");
        UnwrapFree(&FP->unwrap);
    }
}

// How slot `slot` of model file `file` maps onto paint (PUV_*): the file's unwrap for every slot when
// it has one, else its own UVs if they can carry paint (OwnUvKind, computed once per slot)
static int FileUvKind(SceneData *d, int file, int slot)
{
    const MS_ModelFile *F = &d->modelFiles[file];
    FilePaint *FP = d->filePaint ? &d->filePaint[file] : NULL;
    if (!FP || !F->loaded) return PUV_NEEDS_UNWRAP;
    if (F->model.skeleton.boneCount > 0) return PUV_SKINNED;
    if (FP->hasUnwrap) return PUV_UNWRAP;
    if (slot < 0 || slot >= F->materialCount || !F->model.meshData) return PUV_NEEDS_UNWRAP;
    if (!FP->uvKind) {
        if (!(FP->uvKind = malloc((size_t)F->materialCount))) return PUV_NEEDS_UNWRAP;
        memset(FP->uvKind, -1, (size_t)F->materialCount);
        FP->uvKindCount = F->materialCount;
    }
    if (FP->uvKind[slot] < 0) FP->uvKind[slot] = (signed char)OwnUvKind(&F->model, slot);
    return FP->uvKind[slot];
}

// The faces of an entry as written ("+x,-y") or its slot ("slot 2"), for messages
static void PaintTargetText(const PaintDef *e, char *out, int size)
{
    if (e->kind == PAINT_MODEL) { snprintf(out, (size_t)size, "slot %d", e->slot); return; }
    int n = 0;
    out[0] = '\0';
    for (int f = 0; f < 6 && n < size; f++)
        if (e->faces & (1u << f)) n += snprintf(out + n, (size_t)(size - n), "%s%s", n ? "," : "", PAINT_FACES[f]);
}

// What is painted, from the entries that are active: the materials, own atlases and model files that
// draw paint, and whether anything does
static void PaintFlags(SceneData *d)
{
    d->anyPaint = false;
    for (int m = 0; m < d->materialCount; m++) d->matDefs[m].painted = false;
    for (int a = 0; a < d->ownAtlasCount; a++) d->ownAtlases[a].painted = false;
    for (int f = 0; d->filePaint && f < d->modelFileCount; f++) d->filePaint[f].painted = false;
    for (int i = 0; i < d->paintCount; i++) {
        const PaintDef *e = &d->paints[i];
        if (!e->active) continue;
        d->anyPaint = true;
        if (e->material >= 0) d->matDefs[e->material].painted = true;
        else d->ownAtlases[e->ownAtlas].painted = true;
        if (e->kind == PAINT_MODEL && d->filePaint) d->filePaint[d->models[e->object].file].painted = true;
    }
}

// An entry that doesn't draw: logged once per load, with the reason
static void PaintLogInactive(const SceneData *d, const PaintDef *e)
{
    char what[64], why[MS_PATH_MAX + 64];
    PaintTargetText(e, what, sizeof(what));
    const char *mat = e->material >= 0 ? d->materials[e->material].name : "its own material";
    const char *atlas = e->material >= 0 ? d->matDefs[e->material].paint : d->ownAtlases[e->ownAtlas].rel;
    switch (e->why) {
    case PAINT_NOT_DRAWN:    snprintf(why, sizeof(why), "is not drawn with %s", mat); break;
    case PAINT_OUTSIDE:      snprintf(why, sizeof(why), "lies outside its atlas %s", atlas); break;
    case PAINT_NO_ATLAS:     snprintf(why, sizeof(why), "has no atlas image %s", atlas); break;
    case PAINT_SKINNED:      snprintf(why, sizeof(why), "is in a skinned model file (its bone channels are in use)"); break;
    case PAINT_NEEDS_UNWRAP: snprintf(why, sizeof(why), "needs a paint unwrap (its own UVs overlap, repeat or are missing)"); break;
    default:                 snprintf(why, sizeof(why), "can't be drawn: %s's shader can't take the paint stage", mat); break;
    }
    TraceLog(LOG_WARNING, "monolith_scene: %s:%d: paint: %u:%d %s %s; inactive", d->srcPath, e->line, e->entity.stmt,
             e->entity.index, what, why);
}

// Which entries draw (docs/painting.md 4.2): the object draws those faces / that slot with the
// entry's material, the slot's UVs can carry paint, and the rectangle lies inside its atlas. After
// BuildEntities (a compiled map's entries get their entity ids here) and the model files.
static void PaintResolve(SceneData *d)
{
    for (int m = 0; m < d->materialCount; m++) {
        MatDef *D = &d->matDefs[m];
        if (D->paint) PaintAtlasSize(d, D->paint, &D->paintW, &D->paintH);
    }
    for (int a = 0; a < d->ownAtlasCount; a++) PaintAtlasSize(d, d->ownAtlases[a].rel, &d->ownAtlases[a].w, &d->ownAtlases[a].h);
    for (int i = 0; i < d->paintCount; i++) {
        PaintDef *e = &d->paints[i];
        int ent = d->objEntities ? d->objEntities[(e->kind == PAINT_BOX ? 0 : d->boxCount) + e->object] : -1;
        if (ent >= 0) e->entity = d->entityIds[ent];
        int w = e->material >= 0 ? d->matDefs[e->material].paintW : d->ownAtlases[e->ownAtlas].w;
        int h = e->material >= 0 ? d->matDefs[e->material].paintH : d->ownAtlases[e->ownAtlas].h;
        int why = PAINT_OK;
        if (e->kind == PAINT_BOX) {
            if (d->boxRefs[e->object].material != e->material) why = PAINT_NOT_DRAWN;
        } else {
            const MS_ModelInstance *M = &d->models[e->object];
            const MS_ModelFile *F = &d->modelFiles[M->file];
            int uv = F->loaded ? FileUvKind(d, M->file, e->slot) : PUV_OWN;
            if ((F->loaded && e->slot >= F->materialCount) || e->slot > 255 || SlotMaterial(d, M, e->slot) != e->material)
                why = PAINT_NOT_DRAWN;
            else if (uv == PUV_SKINNED) why = PAINT_SKINNED;
            else if (uv == PUV_NEEDS_UNWRAP) why = PAINT_NEEDS_UNWRAP;
        }
        if (why == PAINT_OK && (w <= 0 || h <= 0)) why = PAINT_NO_ATLAS;
        else if (why == PAINT_OK && ((long long)e->x + e->w > w || (long long)e->y + e->h > h)) why = PAINT_OUTSIDE;
        e->active = why == PAINT_OK;
        e->why = (signed char)why;
        if (!e->active) PaintLogInactive(d, e);
    }
    PaintFlags(d);
}

#ifdef MONOLITH_EDITOR
// MS_PaintEntryAt's view of the entries, once the load or edit knows which draw. Out of memory: none
// (MS_PaintEntryCount 0).
static void PaintPublish(SceneData *d)
{
    free(d->paintPub);
    d->paintPub = NULL;
    if (d->paintCount == 0 || !(d->paintPub = malloc(sizeof(MS_PaintEntry) * (size_t)d->paintCount))) return;
    for (int i = 0; i < d->paintCount; i++) {
        const PaintDef *e = &d->paints[i];
        d->paintPub[i] = (MS_PaintEntry){
            .material = e->material, .line = e->line, .entity = e->entity,
            .obj = { e->kind == PAINT_BOX ? MS_OBJ_BOX : MS_OBJ_MODEL, e->object },
            .faces = e->faces, .slot = e->slot,
            .x = e->x, .y = e->y, .w = e->w, .h = e->h,
            .nearest = e->nearest, .active = e->active,
        };
    }
}
#endif

// A material whose shader can't take the paint stage (PaintShaderOf failed): its entries don't draw
static void PaintDemote(SceneData *d, int material, int ownAtlas)
{
    for (int i = 0; i < d->paintCount; i++) {
        PaintDef *e = &d->paints[i];
        if (!e->active || (material >= 0 ? e->material != material : e->ownAtlas != ownAtlas)) continue;
        e->active = false;
        e->why = PAINT_NO_SHADER;
        PaintLogInactive(d, e);
    }
    PaintFlags(d);
}

// Where a scene material's draws go in R3D's draw sort: material i with its own shader keeps rank
// i + 1 whether it draws with that shader or with its paint version, and a material without one
// keeps 0 when it draws with the paint version of the empty shader. So the draw order (and which of
// two coplanar faces shows) doesn't depend on paint or on the order live edits made the aliases.
// Without the patched R3D the order is the aliases' creation order (a load's: material order).
static void ShaderSortKey(R3D_SurfaceShader *shader, unsigned int key)
{
#ifdef MS_PATCHED_R3D
    R3D_SetSurfaceShaderSortKey(shader, key);   // (R3D patch 27)
#else
    (void)shader; (void)key;
#endif
}

// The paint alias of painted material i: the paint version of its shader (or of the empty one),
// its uniforms and samplers, and its atlas as ms_paint (ms_paintTable: PaintBindTable). A shader
// that can't take the paint stage demotes the material (it draws as unpainted). On failure (out of
// memory, an unreadable texture) the last error says why.
static bool BuildPaintedShader(SceneData *d, int i)
{
    MS_Material *M = &d->materials[i];
    MatDef *D = &d->matDefs[i];
    R3D_Material *m = &M->material;
    ShaderFile *f = ShaderFileOf(d, M->shader, D->shaderLine);
    if (!f) return false;
    R3D_SurfaceShader *ps = PaintShaderOf(d, f);
    if (!ps) {
        PaintDemote(d, i, -1);
        return true;
    }
    if (!(m->shader = R3D_LoadSurfaceShaderAlias(ps))) {
        ErrSet("%s:%d: out of memory", d->srcPath, D->shaderLine);
        return false;
    }
    ShaderSortKey(m->shader, M->shader ? (unsigned int)i + 1 : 0);
    for (int u = 0; u < D->uniformCount; u++) R3D_SetSurfaceShaderUniform(m->shader, D->uniforms[u].name, D->uniforms[u].v);
    for (int s = 0; s < D->samplerCount; s++) {
        Texture2D tex = GetTexture(d, D->samplers[s].path, true, D->samplers[s].data, D->wrap, TEX_TRILINEAR,
                                   D->samplers[s].line);
        if (!tex.id) return false;
        R3D_SetSurfaceShaderSampler(m->shader, D->samplers[s].name, tex);
    }
    D->paintTex = GetTextureEx(d, D->paint, true, false, true, 1, D->paintFilter, D->paintLine);
    if (!D->paintTex.id) return false;
    R3D_SetSurfaceShaderSampler(m->shader, "ms_paint", D->paintTex);
    return true;
}

// The atlases and paint aliases of the model slots painted with their own material (an alias of
// the empty shader's paint version each)
static bool BuildOwnAtlases(SceneData *d)
{
    for (int a = 0; a < d->ownAtlasCount; a++) {
        OwnAtlas *A = &d->ownAtlases[a];
        if (!A->painted || A->shader) continue;
        ShaderFile *f = ShaderFileOf(d, NULL, d->modelFiles[A->file].line);
        if (!f) return false;
        R3D_SurfaceShader *ps = PaintShaderOf(d, f);
        if (!ps) {
            PaintDemote(d, -1, a);
            continue;
        }
        A->tex = GetTextureEx(d, A->rel, true, false, true, 1, A->nearest ? TEX_NEAREST : TEX_TRILINEAR,
                              d->modelFiles[A->file].line);
        if (!A->tex.id) return false;
        if (!(A->shader = R3D_LoadSurfaceShaderAlias(ps))) {
            ErrSet("%s: out of memory", d->path);
            return false;
        }
        ShaderSortKey(A->shader, 0);        // (sorts as the model's own material: no shader)
        R3D_SetSurfaceShaderSampler(A->shader, "ms_paint", A->tex);
    }
    return true;
}

// The paint keys (docs/painting.md 4.4): 6 per painted box (its faces +x -x +y -y +z -z), one per slot
// of a painted model instance, key 0 for "no paint"; and the table of rectangles. A scene built
// to replace another passes it (old, with boxFrom / modelFrom: d's object -> old's) so unchanged
// objects keep their keys and their meshes and buffers can stay; new keys go after old's. False when
// out of memory.
static bool PaintKeys(SceneData *d, const SceneData *old, const int *boxFrom, const int *modelFrom)
{
    free(d->boxPaintKey);
    free(d->modelPaintKey);
    free(d->paintTable);
    d->boxPaintKey = d->modelPaintKey = NULL;
    d->paintTable = NULL;
    d->paintKeyCount = d->paintTableRows = 0;
    if (!d->anyPaint) return true;
    d->boxPaintKey = calloc(d->boxCount > 0 ? (size_t)d->boxCount : 1, sizeof(int));
    d->modelPaintKey = calloc(d->modelCount > 0 ? (size_t)d->modelCount : 1, sizeof(int));
    if (!d->boxPaintKey || !d->modelPaintKey) return false;

    // Which objects need keys, then their keys (old's where an object is the same)
    for (int i = 0; i < d->paintCount; i++) {
        const PaintDef *e = &d->paints[i];
        if (e->active) (e->kind == PAINT_BOX ? d->boxPaintKey : d->modelPaintKey)[e->object] = -1;
    }
    int next = old && old->paintKeyCount > 1 ? old->paintKeyCount : 1;
    for (int b = 0; b < d->boxCount; b++) {
        if (d->boxPaintKey[b] >= 0) continue;
        int o = old && boxFrom && old->boxPaintKey ? boxFrom[b] : -1;
        if (o >= 0 && old->boxPaintKey[o] > 0) d->boxPaintKey[b] = old->boxPaintKey[o];
        else { d->boxPaintKey[b] = next; next += 6; }
    }
    for (int i = 0; i < d->modelCount; i++) {
        if (d->modelPaintKey[i] >= 0) continue;
        int o = old && modelFrom && old->modelPaintKey ? modelFrom[i] : -1;
        int slots = d->modelFiles[d->models[i].file].materialCount;
        if (o >= 0 && old->modelPaintKey[o] > 0) d->modelPaintKey[i] = old->modelPaintKey[o];
        else { d->modelPaintKey[i] = next; next += slots > 0 ? slots : 1; }
    }
    if (next > 16777215) return false;      // (keys are 3 bytes in a merged mesh's vertices)

    // The table: each active entry's rectangle, normalised to its atlas, at its faces' / slot's keys
    d->paintKeyCount = next;
    d->paintTableRows = (next + PAINT_TABLE_W - 1) / PAINT_TABLE_W;
    if (!(d->paintTable = calloc((size_t)d->paintTableRows * PAINT_TABLE_W * 4, sizeof(float)))) return false;
    for (int i = 0; i < d->paintCount; i++) {
        const PaintDef *e = &d->paints[i];
        if (!e->active) continue;
        float w = (float)(e->material >= 0 ? d->matDefs[e->material].paintW : d->ownAtlases[e->ownAtlas].w);
        float h = (float)(e->material >= 0 ? d->matDefs[e->material].paintH : d->ownAtlases[e->ownAtlas].h);
        float r[4] = { (float)e->x / w, (float)e->y / h, (float)(e->x + e->w) / w, (float)(e->y + e->h) / h };
        if (e->kind == PAINT_BOX) {
            for (int f = 0; f < 6; f++)
                if (e->faces & (1u << f)) memcpy(&d->paintTable[4 * (size_t)(d->boxPaintKey[e->object] + f)], r, sizeof(r));
        } else memcpy(&d->paintTable[4 * (size_t)(d->modelPaintKey[e->object] + e->slot)], r, sizeof(r));
    }
    return true;
}

// The paint table on the GPU: old's texture when the table is the same (*took), else a new one
static bool PaintTableUpload(SceneData *d, const SceneData *old, bool *took)
{
    *took = false;
    if (!d->anyPaint) return true;
    size_t bytes = (size_t)d->paintTableRows * PAINT_TABLE_W * 4 * sizeof(float);
    if (old && old->paintTableTex.id && old->paintTable && old->paintTableRows == d->paintTableRows &&
        memcmp(old->paintTable, d->paintTable, bytes) == 0) {
        d->paintTableTex = old->paintTableTex;
        *took = true;
        return true;
    }
    unsigned int id = rlLoadTexture(d->paintTable, PAINT_TABLE_W, d->paintTableRows, PIXELFORMAT_UNCOMPRESSED_R32G32B32A32, 1);
    if (!id) return false;
    rlTextureParameters(id, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_NEAREST);
    rlTextureParameters(id, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_NEAREST);
    d->paintTableTex = (Texture2D){ id, PAINT_TABLE_W, d->paintTableRows, 1, PIXELFORMAT_UNCOMPRESSED_R32G32B32A32 };
    return true;
}

// Every paint alias of x reads x's table
static void PaintBindTable(const SceneData *x)
{
    if (!x->paintTableTex.id) return;
    for (int m = 0; m < x->materialCount; m++)
        if (x->matDefs[m].painted && x->materials[m].material.shader)
            R3D_SetSurfaceShaderSampler(x->materials[m].material.shader, "ms_paintTable", x->paintTableTex);
    for (int a = 0; a < x->ownAtlasCount; a++)
        if (x->ownAtlases[a].shader) R3D_SetSurfaceShaderSampler(x->ownAtlases[a].shader, "ms_paintTable", x->paintTableTex);
}

static void PaintCheckCube(void);

// The paint layer's GPU state after the materials (their paint aliases and atlases): the own
// materials' atlases and aliases, the keys (old's for the same objects, when d replaces old), the table
// (old's texture when it is the same: *tookTable) bound to every paint alias. On failure the last error says why.
static bool PaintUpload(SceneData *d, const SceneData *old, const int *boxFrom, const int *modelFrom, bool *tookTable)
{
    bool took = false;
    if (!BuildOwnAtlases(d)) return false;
    if (!PaintKeys(d, old, boxFrom, modelFrom) || !PaintTableUpload(d, old, &took)) {
        ErrSet("%s: out of memory (paint keys)", d->path);
        return false;
    }
    if (tookTable) *tookTable = took;
    PaintBindTable(d);
    if (d->anyPaint) PaintCheckCube();
    return true;
}

// R3D's unit cube as the paint stage assumes it: each face's UVs what CubeFaceUv gives for its
// corners (the instanced boxes' face UVs are computed from the position in the shader), and no bone
// data (a box without paint reads key 0). Checked once per process; a mismatch is logged (paint on
// instanced boxes would then be misplaced).
static void PaintCheckCube(void)
{
    static atomic_bool checked;     // (a fact of the R3D build, not of a scene)
    if (atomic_exchange(&checked, true)) return;
    R3D_MeshData c = R3D_GenMeshDataCube(1.0f, 1.0f, 1.0f);
    bool ok = c.vertexCount == 24;
    for (int k = 0; ok && k < c.vertexCount; k++) {
        const R3D_Vertex *v = &c.vertices[k];
        Vector2 uv = R3D_UnpackTexCoord(v->texcoord), cu = CubeFaceUv(FaceOfNormal(R3D_UnpackNormal(v->normal)), v->position);
        ok = uv.x == cu.x && uv.y == cu.y && !v->boneIndices[0] && !v->boneIndices[1] && !v->boneIndices[2] &&
             !v->boneIndices[3] && !v->boneWeights[0] && !v->boneWeights[1] && !v->boneWeights[2] && !v->boneWeights[3];
    }
    R3D_UnloadMeshData(c);
    if (!ok) TraceLog(LOG_ERROR, "monolith_scene: R3D's unit cube is not the one the paint stage assumes (UVs or bone data): "
                      "paint on boxes is misplaced");
}

// The paint meshes of painted model file `file` (docs/painting.md 4.5): each mesh rebuilt from the
// file's CPU mesh data with the slot (bone index 0) and the paint UV (bone weights): the unwrap's
// vertices and triangles when the file has one, else its own vertices with their UVs. The file's
// meshes and the file on disk are not changed. False when out of memory.
static bool BuildPaintMeshes(SceneData *d, int file)
{
    FilePaint *FP = &d->filePaint[file];
    const R3D_Model *m = &d->modelFiles[file].model;
    if (!m->meshData || !m->meshMaterials) return false;
    if (!(FP->meshes = calloc((size_t)m->meshCount, sizeof(R3D_Mesh)))) return false;
    FP->meshCount = m->meshCount;
    for (int i = 0; i < m->meshCount; i++) {
        const R3D_MeshData *md = &m->meshData[i];
        const R3D_Mesh *orig = &m->meshes[i];
        unsigned int slot = (unsigned int)(m->meshMaterials[i] < 255 ? m->meshMaterials[i] : 255);
        R3D_MeshData nd;
        // (a mesh without vertices stays an empty mesh: nothing to draw)
        if (md->vertexCount <= 0 || !md->vertices || (FP->hasUnwrap && FP->unwrap.meshes[i].vertexCount <= 0)) continue;
        if (FP->hasUnwrap) {
            const UnwrapMesh *U = &FP->unwrap.meshes[i];
            nd = R3D_LoadMeshData(U->vertexCount > 0 ? U->vertexCount : 1, U->indexCount);
            if (!nd.vertices || (U->indexCount > 0 && !nd.indices)) { R3D_UnloadMeshData(nd); return false; }
            for (int k = 0; k < U->vertexCount; k++) {
                nd.vertices[k] = md->vertices[U->source[k]];
                PaintBones(&nd.vertices[k], slot, (Vector2){ U->uv[2 * k] / 65535.0f, U->uv[2 * k + 1] / 65535.0f });
            }
            if (U->indexCount > 0) memcpy(nd.indices, U->indices, sizeof(uint32_t) * (size_t)U->indexCount);
            nd.vertexCount = U->vertexCount;
            nd.indexCount = U->indexCount;
        } else {
            nd = R3D_CopyMeshData(*md);
            if (!nd.vertices) return false;
            for (int k = 0; k < nd.vertexCount; k++)
                PaintBones(&nd.vertices[k], slot, R3D_UnpackTexCoord(nd.vertices[k].texcoord));
        }
        R3D_Mesh mesh = R3D_LoadMesh(orig->primitiveType, nd, &orig->aabb);
        R3D_UnloadMeshData(nd);
        if (mesh.vertexCount <= 0) return false;
        mesh.shadowCastMode = orig->shadowCastMode;
        mesh.layerMask = orig->layerMask;
        FP->meshes[i] = mesh;
    }
    FP->meshesGen = FP->hasUnwrap ? FP->unwrapGen : ~0u;
    return true;
}

static void FreePaintMeshes(SceneData *d)
{
    for (int f = 0; d->filePaint && f < d->modelFileCount; f++) {
        FilePaint *FP = &d->filePaint[f];
        for (int i = 0; FP->meshes && i < FP->meshCount; i++)
            if (FP->meshes[i].vertexCapacity > 0) R3D_UnloadMesh(FP->meshes[i]);
        free(FP->meshes);
        FP->meshes = NULL;
        FP->meshCount = 0;
    }
}

// Material i's textures (its four maps, with the material's wrap and filter); an unset map keeps
// R3D's default. On failure the last error says why.
static bool BuildMaterialMaps(SceneData *d, int i)
{
    MatDef *D = &d->matDefs[i];
    R3D_Material *m = &d->materials[i].material;
    static const bool IS_COLOR[MAP_COUNT] = { true, false, false, true };
    Texture2D *slot[MAP_COUNT] = { &m->albedo.texture, &m->normal.texture, &m->orm.texture, &m->emission.texture };
    for (int k = 0; k < MAP_COUNT; k++) {
        if (!D->maps[k]) continue;
        *slot[k] = GetTexture(d, D->maps[k], IS_COLOR[k], false, D->wrap, D->filter, D->mapLine[k]);
        if (!slot[k]->id) return false;
    }
    return true;
}

// Material i's GPU state: its shader alias (with its uniforms and samplers) and its textures. On
// failure the last error says why (what it made stays in d: FreeGpu frees it).
static bool BuildMaterialShader(SceneData *d, int i)
{
    MS_Material *M = &d->materials[i];
    MatDef *D = &d->matDefs[i];
    R3D_Material *m = &M->material;
    m->shader = NULL;
    // A painted material draws with the paint version of its shader (or of the empty one)
    if (D->painted && !BuildPaintedShader(d, i)) return false;
    if (D->painted) return true;
    if (M->shader) {
        // One compiled shader per file (a compile is ~2 ms, and the first frame compiles each
        // shader's variants again); the materials' aliases are created in material order, so
        // R3D's draw sort, which orders shaders by creation, sees the same order as with one
        // compiled shader per material
        ShaderFile *f = GetShaderFile(d, M, D);
        if (f) m->shader = R3D_LoadSurfaceShaderAlias(f->shader);
        if (f && !m->shader) ErrSet("%s:%d: out of memory", d->srcPath, D->shaderLine);
        if (!m->shader) return false;
        ShaderSortKey(m->shader, (unsigned int)i + 1);
        for (int u = 0; u < D->uniformCount; u++)
            R3D_SetSurfaceShaderUniform(m->shader, D->uniforms[u].name, D->uniforms[u].v);
        for (int s = 0; s < D->samplerCount; s++) {
            Texture2D tex = GetTexture(d, D->samplers[s].path, true, D->samplers[s].data, D->wrap, TEX_TRILINEAR,
                                       D->samplers[s].line);
            if (!tex.id) return false;
            R3D_SetSurfaceShaderSampler(m->shader, D->samplers[s].name, tex);
        }
    }
    return true;
}

static bool BuildMaterial(SceneData *d, int i)
{
    return BuildMaterialShader(d, i) && BuildMaterialMaps(d, i);
}

// How material i's boxes are drawn (after BuildMaterial loaded its shader)
static void MaterialDrawMode(SceneData *d, int i)
{
    MS_Material *M = &d->materials[i];
    MatDef *D = &d->matDefs[i];
    R3D_Material *m = &M->material;
    bool needsCube = false, cutout = false;
    if (M->shader) {
        const ShaderFile *f = ShaderFileOf(d, M->shader, D->shaderLine);    // (scanned)
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
#ifdef MS_PATCHED_R3D   // R3D patch 28; upstream R3D always pre-passes
    m->noDepthPrepass = D->noPrepass;
#endif
    D->occluder = D->merge && !cutout && m->billboardMode == R3D_BILLBOARD_DISABLED && m->albedo.color.a == 255 &&
                  !D->maps[MAP_ALBEDO] && m->alphaCutoff <= 1.0f;
}

// Creates every GPU resource of `d` (a previous load's are still alive: new before old, see
// the instance-buffer note in LoadBytes). On failure everything created so far is freed.
static bool UploadGpu(SceneData *d)
{
    d->gpu = true;
    d->inst = calloc(d->materialCount, sizeof(R3D_InstanceBuffer));
    d->instCount = calloc(d->materialCount, sizeof(int));
    d->lightShadow = calloc(d->lightCount > 0 ? d->lightCount : 1, sizeof(R3D_ShadowMap));
    d->lightShadowDrawn = calloc(d->lightCount > 0 ? d->lightCount : 1, sizeof(bool));
    if (!d->inst || !d->instCount || !d->lightShadow || !d->lightShadowDrawn) {
        ErrSet("%s: out of memory", d->path);
        FreeGpu(d);
        return false;
    }
    for (int i = 0; i < d->materialCount; i++)
        if (!BuildMaterial(d, i)) {
            FreeGpu(d);
            return false;
        }

    // The paint layer (only with an active entry): the own materials' paint, the keys and their table
    if (d->anyPaint && !PaintUpload(d, NULL, NULL, NULL, NULL)) {
        FreeGpu(d);
        return false;
    }

    // How the boxes of each material are drawn, and the unit cubes of the face layouts
    for (int i = 0; i < d->materialCount; i++) MaterialDrawMode(d, i);
    if (!BuildFaceCubes(d)) {
        ErrSet("%s: out of memory", d->path);
        FreeGpu(d);
        return false;
    }

    double tBoxes = GetTime();
    bool boxesOk = UploadBoxes(d);
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
            ErrSet("%s: cannot load the sky panorama \"%s\"", d->path, e->skyPanorama);
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
    for (int f = 0; d->filePaint && f < d->modelFileCount; f++)
        if (d->filePaint[f].painted && !BuildPaintMeshes(d, f)) {
            ErrSet("%s: out of memory (paint meshes)", d->path);
            FreeGpu(d);
            return false;
        }
    if (!BuildModelGroups(d)) {
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
        free(D->paint);
    }
    free(d->materials);
    free(d->matDefs);
    free(d->matHash);
    free(d->paints);
    for (int a = 0; a < d->ownAtlasCount; a++) free(d->ownAtlases[a].rel);
    free(d->ownAtlases);
    for (int f = 0; d->filePaint && f < d->modelFileCount; f++) FreeFilePaint(&d->filePaint[f]);
    free(d->filePaint);
    free(d->boxPaintKey);
    free(d->modelPaintKey);
    free(d->paintTable);
    for (int i = 0; i < d->groupCount; i++) free(d->groups[i]);
    free(d->groups);
    for (int i = 0; i < d->viewpointCount; i++) free((char *)d->viewpoints[i].name);
    free(d->viewpoints);
    free(d->boxes);
    free(d->boxRefs);
    free(d->turned);
    free(d->boxTurned);
    free(d->turnedWritten);
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
    free(d->matLookHash);
    free(d->matShapeHash);
    free(d->matPaintlessHash);
    free(d->paintPub);
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
    s->turnedBoxes = d->turned;
    s->turnedBoxCount = d->turnedCount;
    s->boxTurned = d->turnedCount > 0 ? d->boxTurned : NULL;
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
static bool FinishLoad(SceneData *d, const MS_LoadOptions *opt, bool gpu)
{
    if (!FinishParse(d, opt)) {
        ErrSet("%s: out of memory", d->path);
        return false;
    }
    bool ok;
#ifdef MONOLITH_EDITOR
    d->shaderDefines = opt->shaderDefines;
    d->shaderDefinesFile = opt->shaderDefinesFile;
#endif
    double t0 = GetTime();
    PaintPrepare(d);
    if (gpu) {
        d->gpu = true;                  // (so FreeData unloads what LoadModelFiles loaded)
        ok = LoadModelFiles(d);
    } else ok = CheckModelFiles(d);
    if (ok) ok = ResolveModels(d);
    d->times.models = (GetTime() - t0) * 1000.0;
    if (ok) {
        FinishGeometry(d);
        t0 = GetTime();
        ok = BuildColliderBvh(d);
        d->times.bvh = (GetTime() - t0) * 1000.0;
    }
    if (ok && !BuildEntities(d)) {
        ErrSet("%s: out of memory", d->path);
        ok = false;
    }
    if (ok && d->paintCount > 0) PaintResolve(d);
    if (ok && gpu) ok = UploadGpu(d);
#ifdef MONOLITH_EDITOR
    if (ok) PaintPublish(d);
#endif
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

#ifdef MONOLITH_EDITOR
static void FreeProvided(MS_Private *p);
#endif

// Loads a map from its file's bytes (takes ownership): fills a SceneData from them, finishes the
// load and swaps it in on success
static bool LoadBytes(MS_Scene *s, const char *mapPath, char *bytes, int size)
{
    MS_Private *p = s->priv;
    SceneData *d = calloc(1, sizeof(*d));
    if (!d) {
        ErrSet("%s: out of memory", mapPath);
        free(bytes);
        return false;
    }
    double tLoad = GetTime();
    bool ok = InitData(d, mapPath);
    if (!ok) ErrSet("%s: out of memory", mapPath);
    bool compiled = IsCompiledMap(bytes, size);
    if (ok && compiled) ok = ReadCompiled(d, bytes, size);
#ifdef MONOLITH_EDITOR
    if (ok && !compiled && size > MS_FILE_MAX) {
        ErrSet("%s: cannot read file (or larger than 4 MB)", mapPath);
        ok = false;
    }
    if (ok && !compiled) ok = ParseScene(d, bytes, size);
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
        ErrSet("%s: not a compiled map (this program reads compiled maps, <name>.mapb: compile the "
                 "map's text with monolith)", mapPath);
        ok = false;
    }
    free(bytes);
#endif
    if (ok) ok = FinishLoad(d, &p->opt, p->gpu);
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
#ifdef MONOLITH_EDITOR
    if (p->pv) MS_EditPreviewEnd(s);    // (puts the old scene back together before it goes)
    FreeProvided(p);                    // (the files are the truth again; the editor provides anew)
#endif
    SceneData *old = p->d;
    p->d = d;
    s->version++;
#ifdef MONOLITH_EDITOR
    s->loadVersion++;
#endif
    Publish(s, d);
    FreeData(old);
    if (p->gpu) LogLoadTimes(d, old != NULL, (GetTime() - tLoad) * 1000.0);
    ErrClear();
    return true;
}

static bool LoadPath(MS_Scene *s, const char *path)
{
    char mapPath[MS_PATH_MAX];
    if (!ResolveMapPath(path, mapPath, sizeof(mapPath))) return false;
    int size;
    char *bytes = ReadWholeFile(mapPath, MS_MAX_COMPILED_BYTES, &size, true);
    if (!bytes) return false;
    return LoadBytes(s, mapPath, bytes, size);
}

// #endregion

// ======================================================================================
// #region Public API: loading
// ======================================================================================

MS_Scene *MS_Load(const char *path, const MS_LoadOptions *opt)
{
    MS_Scene *s = calloc(1, sizeof(*s));
    MS_Private *p = s ? calloc(1, sizeof(*p)) : NULL;
    if (!p) {
        free(s);
        ErrSet("%s: out of memory", path);
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
    if (!LoadPath(s, path)) {
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
#ifdef MONOLITH_EDITOR
        if (p->pv) MS_EditPreviewEnd(s);
        free(p->dirty);
        free(p->aoGrid);
        free(p->rangeObj);
        free(p->rangeAt);
        free(p->rangeMatAt);
        free(p->drawHidden);
        free(p->filterObj);
        free(p->filterObjNew);
        FreeProvided(p);
        free(p->provided);
        free(p->unwraps);
        free(p->texScratch);
        for (int i = 0; p->gpu && i < p->retiredCount; i++) R3D_UnloadMesh(p->retired[i]);
        free(p->retired);
#endif
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
        free(p->error);
#endif
        free(p);
    }
    free(s);
}

#ifdef MONOLITH_EDITOR
// MS_Error's copy of a (re)load's outcome: the last error after a failure, nothing after a
// success. Out of memory: MS_Error says so.
static void KeepReloadError(MS_Private *p, bool ok)
{
    free(p->error);
    p->error = ok ? NULL : MS_StrDup(MS_LastError());
    p->errorOom = !ok && !p->error;
}

MS_ReloadResult MS_Reload(MS_Scene *s, const char *path)
{
    char *cur = MS_StrDup(path ? path : s->path);     // (s->path is freed by a successful load)
    bool ok = cur ? LoadPath(s, cur) : ErrSet("out of memory");
    free(cur);
    KeepReloadError(s->priv, ok);
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

    char mapPath[MS_PATH_MAX];
    snprintf(mapPath, sizeof(mapPath), "%s", p->watch[0]);
    int size;
    char *text = ReadWholeFile(mapPath, MS_MAX_COMPILED_BYTES, &size, false);
    if (!text) {
        // Usually transient (the next poll retries), but a file that stays unreadable or
        // too big must not fail silently: report it once the settle window has run out
        // (read again for the reason: a poll that reloads nothing leaves the last error alone)
        if (p->settleTime > 0.0f) return MS_UNCHANGED;
        text = ReadWholeFile(mapPath, MS_MAX_COMPILED_BYTES, &size, true);
    }
    if (!text) {
        KeepReloadError(p, false);
        p->triedHash = 0;
        return MS_RELOAD_FAILED;
    }
    unsigned long long hash = CombinedHash(text, size, p->watch, p->watchCount);
    if (hash == p->triedHash) {                 // touched, or the same broken package again
        free(text);
        return MS_UNCHANGED;
    }
    float settle = p->settleTime;
    bool ok = LoadBytes(s, mapPath, text, size);
    p->settleTime = settle;                     // keep watching while the editor may still write
    KeepReloadError(p, ok);
    return ok ? MS_RELOADED : MS_RELOAD_FAILED;
}

const char *MS_Error(const MS_Scene *s)
{
    const MS_Private *p = s->priv;
    return p->error ? p->error : p->errorOom ? "out of memory" : "";
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

bool MS_Compile(const char *mapPath, const char *outPath)
{
    char path[MS_PATH_MAX];
    if (!ResolveMapPath(mapPath, path, sizeof(path))) return false;
    int size;
    char *text = ReadWholeFile(path, MS_FILE_MAX, &size, true);
    if (!text) return false;
    if (IsCompiledMap(text, size)) {
        ErrSet("%s: already a compiled map (compile the map's text, its .map file)", path);
        free(text);
        return false;
    }
    SceneData *d = calloc(1, sizeof(*d));
    bool ok = d && InitData(d, path);
    if (!ok) ErrSet("%s: out of memory", path);
    if (ok) ok = ParseScene(d, text, size);
    unsigned char *out = NULL;
    int outSize = 0;
    if (ok) ok = (out = WriteCompiled(d, GetFileName(path), Fnv1a64Update(FNV_OFFSET, text, (size_t)size), size,
                                      &outSize)) != NULL;
    // The package check a noGpu load makes (model files included), then the compiled bytes read
    // back as a game would (relative to the source's package folder): what compiles, loads
    if (ok) ok = FinishLoad(d, &(MS_LoadOptions){ .noGpu = true }, false);
    FreeData(d);
    free(text);
    if (ok) {
        SceneData *r = calloc(1, sizeof(*r));
        ok = r && InitData(r, path);
        if (!ok) ErrSet("%s: out of memory", path);
        if (ok) ok = ReadCompiled(r, (const char *)out, outSize);
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
            ErrSet("%s: cannot write the compiled map", outPath);
            remove(partial);
        }
    }
    free(out);
    if (ok) ErrClear();
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

#ifdef MONOLITH_EDITOR
// ======================================================================================
// #region Live edits (editor; docs/live-editing.md)
// ======================================================================================
// MS_EditSetText parses a new map text on the CPU and swaps it in like a load, but keeps every GPU
// resource whose input did not change: model files, textures, shaders, materials, sky, ambient
// map, the merged box meshes and instance buffers whose boxes are the same, the light shadow maps
// of identical lights. Objects are matched by content (not by entity id: the pieces of a split
// `repeat` line get new ids although their boxes stay). What changed (the bounds of every object
// without an identical one on the other side) goes into p->dirty, and the static caches redraw
// only where it reaches: sun shadow tiles and the camera-fitted map by their light-space square,
// light shadows by their range, the fog cache by its bake box, the occlusion volume voxel by voxel.
// Everything kept or redrawn is what a load of the same text would make (the --edit-script tests).

#define LIVE_HIDE_PARTIAL_MAX   256     // more changed boxes: the hidden faces are searched again in full
#define LIVE_AO_WINDOWS_MAX     16      // more changed boxes: one window around all of them

// --- Matching objects by content: old objects in a hash table of chains (map order), each new
// object takes the first unused old one with the same content

typedef bool (*LiveSame)(const SceneData *a, int i, const SceneData *b, int j);

typedef struct {
    unsigned long long *key;    // per slot: content hash
    int *head, *tail;           // per slot: first / last old object + 1 (0 = empty slot)
    int *next;                  // per old object: the next one with the same hash, -1
    bool *used;
    unsigned int mask;
} LiveMatch;

static void LiveMatchFree(LiveMatch *m)
{
    free(m->key); free(m->head); free(m->tail); free(m->next); free(m->used);
    memset(m, 0, sizeof(*m));
}

static bool LiveMatchInit(LiveMatch *m, int count)
{
    memset(m, 0, sizeof(*m));
    unsigned int cap = 16;
    while (cap < 2u * (unsigned int)(count > 0 ? count : 1)) cap *= 2;
    m->mask = cap - 1;
    m->key = calloc(cap, sizeof(*m->key));
    m->head = calloc(cap, sizeof(int));
    m->tail = calloc(cap, sizeof(int));
    m->next = malloc(sizeof(int) * (count > 0 ? (size_t)count : 1));
    m->used = calloc(count > 0 ? (size_t)count : 1, sizeof(bool));
    if (m->key && m->head && m->tail && m->next && m->used) return true;
    LiveMatchFree(m);
    return false;
}

static unsigned int LiveSlot(const LiveMatch *m, unsigned long long key)
{
    unsigned int h = (unsigned int)((key * 0x9E3779B97F4A7C15ull) >> 32) & m->mask;
    while (m->head[h] && m->key[h] != key) h = (h + 1) & m->mask;
    return h;
}

static void LiveMatchAdd(LiveMatch *m, int i, unsigned long long key)
{
    unsigned int h = LiveSlot(m, key);
    m->next[i] = -1;
    if (!m->head[h]) { m->key[h] = key; m->head[h] = i + 1; }
    else m->next[m->tail[h] - 1] = i;
    m->tail[h] = i + 1;
}

// The first unused old object with this content, -1 = none
static int LiveMatchTake(LiveMatch *m, unsigned long long key, LiveSame same, const SceneData *old, const SceneData *d, int j)
{
    unsigned int h = LiveSlot(m, key);
    for (int i = m->head[h] - 1; i >= 0 && m->head[h]; i = m->next[i])
        if (!m->used[i] && same(old, i, d, j)) { m->used[i] = true; return i; }
    return -1;
}

static unsigned long long BoxKey(const SceneData *d, int i)
{
    unsigned long long h = Fnv1a64Update(FNV_OFFSET, &d->boxes[i], sizeof(BoundingBox));
    const MS_TurnedBox *t = TurnedOf(d, i);
    if (t) h = Fnv1a64Update(h, t, sizeof(MS_TurnedBox));
    return Fnv1a64Update(h, &d->boxRefs[i].material, sizeof(int));
}

static bool BoxSame(const SceneData *a, int i, const SceneData *b, int j)
{
    const MS_TurnedBox *ta = TurnedOf(a, i), *tb = TurnedOf(b, j);
    return memcmp(&a->boxes[i], &b->boxes[j], sizeof(BoundingBox)) == 0 && a->boxRefs[i].material == b->boxRefs[j].material &&
           !ta == !tb && (!ta || memcmp(ta, tb, sizeof(MS_TurnedBox)) == 0);
}

// A model instance: file, placement, materials, shadow, colliders (what its draw and its cache
// contributions depend on)
static unsigned long long ModelKey(const SceneData *d, int i)
{
    const MS_ModelInstance *M = &d->models[i];
    unsigned long long h = Fnv1a64Update(FNV_OFFSET, d->modelFiles[M->file].path, strlen(d->modelFiles[M->file].path));
    h = Fnv1a64Update(h, &M->transform, sizeof(Matrix));
    h = Fnv1a64Update(h, &M->bounds, sizeof(BoundingBox));
    return Fnv1a64Update(h, &M->castShadow, sizeof(bool));
}

static bool ModelSame(const SceneData *a, int i, const SceneData *b, int j)
{
    const MS_ModelInstance *A = &a->models[i], *B = &b->models[j];
    if (strcmp(a->modelFiles[A->file].path, b->modelFiles[B->file].path) != 0 || A->castShadow != B->castShadow ||
        A->ref.material != B->ref.material || A->collide != B->collide || A->slotCount != B->slotCount ||
        A->colliderCount != B->colliderCount || A->origin != B->origin ||
        memcmp(&A->transform, &B->transform, sizeof(Matrix)) != 0 || memcmp(&A->bounds, &B->bounds, sizeof(BoundingBox)) != 0 ||
        memcmp(&A->position, &B->position, sizeof(Vector3)) != 0 || memcmp(&A->rotationDeg, &B->rotationDeg, sizeof(Vector3)) != 0 ||
        memcmp(&A->scale, &B->scale, sizeof(Vector3)) != 0)
        return false;
    for (int k = 0; k < A->slotCount; k++)
        if (a->modelSlots[A->slotFirst + k].slot != b->modelSlots[B->slotFirst + k].slot ||
            a->modelSlots[A->slotFirst + k].material != b->modelSlots[B->slotFirst + k].material) return false;
    for (int k = 0; k < A->colliderCount; k++)
        if (memcmp(&a->colliders[A->colliderFirst + k], &b->colliders[B->colliderFirst + k], sizeof(BoundingBox)) != 0) return false;
    return true;
}

// A light by the fields that make it (R3D_Light field by field: its padding is not set)
static unsigned long long LightKey(const SceneData *d, int i)
{
    const R3D_Light *L = &d->lights[i].light;
    unsigned long long h = Fnv1a64Update(FNV_OFFSET, &L->position, sizeof(Vector3));
    h = Fnv1a64Update(h, &L->direction, sizeof(Vector3));
    h = Fnv1a64Update(h, &L->range, sizeof(float));
    return Fnv1a64Update(h, &L->type, sizeof(L->type));
}

static bool LightSame(const SceneData *a, int i, const SceneData *b, int j)
{
    const MS_LightDef *A = &a->lights[i], *B = &b->lights[j];
    const R3D_Light *x = &A->light, *y = &B->light;
    return memcmp(&x->position, &y->position, sizeof(Vector3)) == 0 && memcmp(&x->direction, &y->direction, sizeof(Vector3)) == 0 &&
           memcmp(&x->color, &y->color, sizeof(Color)) == 0 && x->energy == y->energy && x->specular == y->specular &&
           x->range == y->range && x->falloff == y->falloff && x->innerCutOff == y->innerCutOff &&
           x->outerCutOff == y->outerCutOff && x->fogEnergy == y->fogEnergy && x->type == y->type &&
           A->shadow == B->shadow && A->clip == B->clip && (!A->clip || memcmp(&A->clipBox, &B->clipBox, sizeof(BoundingBox)) == 0);
}

// Matches every new object of one kind to an old one: from[j] = old index or -1; *changed counts
// the objects on both sides without a partner. False when out of memory.
static bool LiveMatchAll(const SceneData *old, int oldCount, const SceneData *d, int count,
                         unsigned long long (*key)(const SceneData *, int), LiveSame same, int *from, bool **oldUsed, int *changed)
{
    LiveMatch m;
    if (!LiveMatchInit(&m, oldCount)) return false;
    for (int i = 0; i < oldCount; i++) LiveMatchAdd(&m, i, key(old, i));
    int matched = 0;
    for (int j = 0; j < count; j++) {
        from[j] = LiveMatchTake(&m, key(d, j), same, old, d, j);
        matched += from[j] >= 0;
    }
    *changed = (count - matched) + (oldCount - matched);
    *oldUsed = m.used;
    m.used = NULL;
    LiveMatchFree(&m);
    return true;
}

// --- Dirty regions

static void DirtyAdd(MS_Private *p, BoundingBox b)
{
    if (p->dirtyCount < 0) return;
    BoundingBox *slot = MS_PUSH(p->dirty, p->dirtyCount, p->dirtyCap);
    if (slot) *slot = b;
    else p->dirtyCount = -1;    // out of memory: everything is dirty (see DirtyAll)
}

static bool DirtyAll(const MS_Private *p) { return p->dirtyCount < 0; }

static inline bool BoxesTouch(BoundingBox a, BoundingBox b)
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y && a.min.z <= b.max.z &&
           a.max.z >= b.min.z;
}

// Does a dirty box reach the light-space square of an orthographic shadow map (its viewProj; depth
// ignored: casters toward the light are pancaked onto the near plane)?
static bool DirtyHitsMap(const MS_Private *p, int from, int to, Matrix viewProj)
{
    if (DirtyAll(p)) return true;
    const float m = 1.01f;      // a little margin for float rounding
    for (int i = from; i < to; i++) {
        const BoundingBox *b = &p->dirty[i];
        float x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY;
        for (int c = 0; c < 8; c++) {
            Vector3 q = { (c & 1) ? b->max.x : b->min.x, (c & 2) ? b->max.y : b->min.y, (c & 4) ? b->max.z : b->min.z };
            Vector3 n = Vector3Transform(q, viewProj);
            x0 = fminf(x0, n.x); x1 = fmaxf(x1, n.x);
            y0 = fminf(y0, n.y); y1 = fmaxf(y1, n.y);
        }
        if (x1 >= -m && x0 <= m && y1 >= -m && y0 <= m) return true;
    }
    return false;
}

static bool DirtyHitsSphere(const MS_Private *p, int from, int to, Vector3 c, float r)
{
    if (DirtyAll(p)) return true;
    for (int i = from; i < to; i++) {
        const BoundingBox *b = &p->dirty[i];
        float dx = fmaxf(fmaxf(b->min.x - c.x, 0.0f), c.x - b->max.x);
        float dy = fmaxf(fmaxf(b->min.y - c.y, 0.0f), c.y - b->max.y);
        float dz = fmaxf(fmaxf(b->min.z - c.z, 0.0f), c.z - b->max.z);
        if (dx * dx + dy * dy + dz * dz <= r * r * 1.0001f + 1e-4f) return true;
    }
    return false;
}

// What a live edit does with a material (bits of the plan): its textures and meshes again (changed
// only in uv_faces, filter or albedo_map), built (appended), its shader alias again (its paint changed)
enum { PLAN_MAPS = 1, PLAN_NEW = 2, PLAN_MATERIAL = 3, PLAN_PAINT = 4 };

// --- What d borrowed from old: given back to old when the edit fails, taken from old (its handles
// zeroed, so freeing old leaves them) when it succeeds

typedef struct {
    int *fileFrom;              // per model file of d: the old file it took, -1 = loaded anew
    int *lightFrom;             // per light of d: the old light whose shadow map it took, -1
    int *meshFrom;              // per box range of d: the old range whose mesh it took, -1
    int meshFromCap;
    bool *instFrom;             // per material: d took old's instance buffer
    bool materials, unit;       // d took old's material GPU state (shaders, textures, sky) / unit buffer
    bool faceCubes;             // d took old's face layout cubes (the same layouts)
    int textureCount;           // with materials: old's textures (d's list may have grown past them)
    const unsigned char *matRebuild;    // LiveMaterialPlan's (not owned): the materials d built again
    bool paintTable;            // d took old's paint table texture (the same table)
    int *paintMeshFrom;         // per model file of d: the old file whose paint meshes it took, -1
} LiveBorrow;

static void LiveBorrowFree(LiveBorrow *B)
{
    free(B->fileFrom); free(B->lightFrom); free(B->meshFrom); free(B->instFrom); free(B->paintMeshFrom);
    memset(B, 0, sizeof(*B));
}

// Zeroes in x what it shares with the other side (x = d after a failure, x = old after a success)
static void LiveRelease(SceneData *x, bool isNew, const SceneData *d, const LiveBorrow *B)
{
    for (int i = 0; B->fileFrom && i < d->modelFileCount; i++) {
        if (B->fileFrom[i] < 0) continue;
        int k = isNew ? i : B->fileFrom[i];
        memset(&x->modelFiles[k].model, 0, sizeof(R3D_Model));
        x->modelFiles[k].loaded = false;
        memset(&x->modelPick[k], 0, sizeof(ModelPick));
    }
    for (int j = 0; B->lightFrom && j < d->lightCount; j++)
        if (B->lightFrom[j] >= 0 && x->lightShadow) x->lightShadow[isNew ? j : B->lightFrom[j]] = (R3D_ShadowMap){ 0 };
    for (int r = 0; B->meshFrom && r < d->boxRangeCount && r < B->meshFromCap; r++)
        if (B->meshFrom[r] >= 0) x->boxRanges[isNew ? r : B->meshFrom[r]].mesh = (R3D_Mesh){ 0 };
    for (int m = 0; B->instFrom && x->inst && m < x->materialCount; m++)
        if (B->instFrom[m]) x->inst[m] = (R3D_InstanceBuffer){ 0 };
    if (B->materials) {
        for (int i = 0; i < x->materialCount; i++) x->materials[i].material.shader = NULL;
        // (the shader file list: d has its own copy of the array, the shaders are old's)
        free(x->shaderFiles);
        x->shaderFiles = NULL;
        x->shaderFileCount = x->shaderFileCap = 0;
        x->textures = NULL;             // (one array: the TexEntry list moved, not copied)
        x->textureCount = x->textureCap = 0;
        x->sky = (R3D_Cubemap){ 0 };
        x->ambientMap = (R3D_AmbientMap){ 0 };
    }
    if (B->unit) x->unit = (R3D_InstanceBuffer){ 0 };
    if (B->faceCubes) {
        x->faceCubes = NULL;            // (one array: moved, not copied)
        x->faceCubeCount = x->faceCubeCap = 0;
    }
    if (B->paintTable) x->paintTableTex = (Texture2D){ 0 };
    for (int i = 0; B->paintMeshFrom && x->filePaint && i < d->modelFileCount; i++) {
        if (B->paintMeshFrom[i] < 0) continue;
        FilePaint *FP = &x->filePaint[isNew ? i : B->paintMeshFrom[i]];
        free(FP->meshes);               // (the array: each side has its own; the meshes are shared)
        FP->meshes = NULL;
        FP->meshCount = 0;
    }
}

// The box meshes and face cubes old still owns after an edit are not freed yet: they go to
// p->retired, freed by the next MS_DrawGeometry. Freed at once, their vertex ranges in R3D's shared
// buffer were handed out again to the next edit; with several edits before a frame (an edit script)
// a range filled by one edit, freed by the next and filled again by a third was sometimes drawn
// with its first contents on the GPU (a box drawn as another mesh, a shadow drawn from old
// geometry). After a frame has drawn the new meshes, their ranges are reused safely. On out of
// memory they are freed at once (as before).
static void LiveRetire(MS_Private *p, SceneData *old)
{
    for (int r = 0; r < old->boxRangeCount; r++) {
        if (old->boxRanges[r].mesh.vertexCapacity <= 0) continue;
        R3D_Mesh *slot = MS_PUSH(p->retired, p->retiredCount, p->retiredCap);
        if (!slot) return;
        *slot = old->boxRanges[r].mesh;
        old->boxRanges[r].mesh = (R3D_Mesh){ 0 };
    }
    for (int c = 0; c < old->faceCubeCount; c++) {
        R3D_Mesh *slot = MS_PUSH(p->retired, p->retiredCount, p->retiredCap);
        if (!slot) return;
        *slot = old->faceCubes[c].mesh;
        old->faceCubes[c].mesh = (R3D_Mesh){ 0 };
    }
    for (int f = 0; old->filePaint && f < old->modelFileCount; f++)
        for (int i = 0; old->filePaint[f].meshes && i < old->filePaint[f].meshCount; i++) {
            R3D_Mesh *m = &old->filePaint[f].meshes[i];
            if (m->vertexCapacity <= 0) continue;
            R3D_Mesh *slot = MS_PUSH(p->retired, p->retiredCount, p->retiredCap);
            if (!slot) return;
            *slot = *m;
            *m = (R3D_Mesh){ 0 };
        }
}

// The paint meshes of d's painted model files: old's when d took the file over and they were built
// from the same unwrap (or both from own UVs), else built (stats: paintMeshFiles). False when out of
// memory (the last error says so).
static bool PaintMeshesLive(SceneData *old, SceneData *d, LiveBorrow *B, MS_EditStats *st)
{
    if (!d->filePaint) return true;
    if (!(B->paintMeshFrom = malloc(sizeof(int) * (size_t)d->modelFileCount))) {
        ErrSet("%s: out of memory", d->path);
        return false;
    }
    for (int f = 0; f < d->modelFileCount; f++) {
        FilePaint *FP = &d->filePaint[f];
        int o = B->fileFrom ? B->fileFrom[f] : -1;
        const FilePaint *OP = o >= 0 && old->filePaint ? &old->filePaint[o] : NULL;
        B->paintMeshFrom[f] = -1;
        if (!FP->painted) continue;
        unsigned int gen = FP->hasUnwrap ? FP->unwrapGen : ~0u;
        if (OP && OP->meshes && OP->meshesGen == gen && OP->meshCount == d->modelFiles[f].model.meshCount &&
            (FP->meshes = malloc(sizeof(R3D_Mesh) * (size_t)OP->meshCount))) {
            memcpy(FP->meshes, OP->meshes, sizeof(R3D_Mesh) * (size_t)OP->meshCount);
            FP->meshCount = OP->meshCount;
            FP->meshesGen = gen;
            B->paintMeshFrom[f] = o;
            continue;
        }
        if (!BuildPaintMeshes(d, f)) {
            ErrSet("%s: out of memory (paint meshes)", d->path);
            return false;
        }
        st->paintMeshFiles++;
    }
    return true;
}

// The face layout cubes of d: old's when d's materials have the same layouts in the same order (the
// list BuildFaceCubes would make is old's), else built. An edit then doesn't free and allocate them
// again (see LiveRetire). False when out of memory.
static bool LiveFaceCubes(SceneData *old, SceneData *d, LiveBorrow *B)
{
    int n = 0;
    bool same = true;
    for (int i = 0; i < d->materialCount && same; i++) {
        const MatDef *D = &d->matDefs[i];
        if (!D->hasFaces) continue;
        int c = 0;
        while (c < n && memcmp(old->faceCubes[c].cells, D->uvFaces, 6) != 0) c++;
        if (c < n) continue;
        same = n < old->faceCubeCount && memcmp(old->faceCubes[n].cells, D->uvFaces, 6) == 0;
        n++;
    }
    if (!same || n != old->faceCubeCount) return BuildFaceCubes(d);
    d->faceCubes = old->faceCubes;
    d->faceCubeCount = old->faceCubeCount;
    d->faceCubeCap = old->faceCubeCap;
    B->faceCubes = true;
    for (int i = 0; i < d->materialCount; i++) {
        MatDef *D = &d->matDefs[i];
        D->faceCube = 0;
        for (int c = 0; D->hasFaces && c < d->faceCubeCount && !D->faceCube; c++)
            if (memcmp(d->faceCubes[c].cells, D->uvFaces, 6) == 0) D->faceCube = c + 1;
    }
    return true;
}

// After a failed edit that took old's material GPU state: what d added to it is freed (the
// shader aliases of its new materials, the shader files and textures it loaded), and the texture
// list, which may have moved while it grew, goes back to old
static void LiveMaterialsBack(SceneData *old, SceneData *d, const LiveBorrow *B)
{
    for (int i = 0; B->matRebuild && i < d->materialCount; i++) {
        R3D_SurfaceShader *sh = d->materials[i].material.shader;
        bool fresh = (B->matRebuild[i] & PLAN_MATERIAL) == PLAN_NEW ||
                     ((B->matRebuild[i] & PLAN_PAINT) && i < old->materialCount && sh != old->materials[i].material.shader);
        if (fresh && sh) {
            R3D_UnloadSurfaceShader(sh);
            d->materials[i].material.shader = NULL;
        }
    }
    // Shaders d compiled: whole files it added, and versions of old's files old hadn't compiled
    for (int f = 0; f < d->shaderFileCount; f++) {
        const ShaderFile *o = f < old->shaderFileCount ? &old->shaderFiles[f] : NULL;
        if (d->shaderFiles[f].shader && (!o || !o->shader)) R3D_UnloadSurfaceShader(d->shaderFiles[f].shader);
        if (d->shaderFiles[f].paint && (!o || !o->paint)) R3D_UnloadSurfaceShader(d->shaderFiles[f].paint);
    }
    PaintBindTable(old);        // (old's aliases d shared read d's table meanwhile)
    for (int i = B->textureCount; i < d->textureCount; i++) {
        R3D_UnloadTexture(d->textures[i].tex);
        free(d->textures[i].path);
    }
    old->textures = d->textures;
    old->textureCap = d->textureCap;
}

// Whether old's materials can stay (docs/painting.md 9.2, 9.4): the rest of the look the same, each of
// old's materials the same (0), or changed only in uv_faces, filter or albedo_map (PLAN_MAPS) or only
// in its paint keys (0 here: PaintSigSame decides), and the others appended at the end (PLAN_NEW).
// *plan: per material of d (heap, the caller frees it). False: a full upload.
static bool LiveMaterialPlan(const SceneData *old, const SceneData *d, unsigned char **plan)
{
    if (old->restLookHash != d->restLookHash || d->materialCount < old->materialCount || !old->matLookHash ||
        !d->matLookHash) return false;
    unsigned char *w = calloc((size_t)d->materialCount, 1);
    if (!w) return false;
    for (int i = 0; i < old->materialCount; i++) {
        if (old->matLookHash[i] == d->matLookHash[i]) continue;
        if (old->matShapeHash[i] != d->matShapeHash[i]) {
            free(w);
            return false;
        }
        if (old->matPaintlessHash[i] != d->matPaintlessHash[i]) w[i] = PLAN_MAPS;
    }
    for (int i = old->materialCount; i < d->materialCount; i++) w[i] = PLAN_NEW;
    *plan = w;
    return true;
}

// Whether material i draws the same paint in old and d: both without, or both with the same atlas
// (path, size) and filter
static bool PaintSigSame(const SceneData *old, const SceneData *d, int i)
{
    const MatDef *a = &old->matDefs[i], *b = &d->matDefs[i];
    if (a->painted != b->painted) return false;
    return !a->painted || (strcmp(a->paint, b->paint) == 0 && a->paintFilter == b->paintFilter &&
                           a->paintW == b->paintW && a->paintH == b->paintH);
}

// A model file d took over from old (index `from` there): its unwrap, the one provided for it now or
// else old's, and old's own-UV results (they depend on the file only)
static void PaintLiveUnwrap(const MS_Private *p, SceneData *d, int i, const SceneData *old, int from)
{
    FilePaint *FP = &d->filePaint[i];
    const FilePaint *OP = old->filePaint ? &old->filePaint[from] : NULL;
    const MS_ModelFile *F = &d->modelFiles[i];
    const ProvidedUnwrap *pu = ProvidedUnwrapOf(p, F->path);
    if (pu) {
        FP->hasUnwrap = F->model.meshData && UnwrapMatches(&pu->u, &F->model) && UnwrapCopy(&FP->unwrap, &pu->u);
        FP->unwrapGen = pu->gen;
    } else if (OP && OP->hasUnwrap) {
        FP->hasUnwrap = UnwrapCopy(&FP->unwrap, &OP->unwrap);
        FP->unwrapGen = OP->unwrapGen;
    }
    if (OP && OP->uvKind && F->materialCount > 0 && (FP->uvKind = malloc((size_t)F->materialCount))) {
        memcpy(FP->uvKind, OP->uvKind, (size_t)F->materialCount);
        FP->uvKindCount = F->materialCount;
    }
}

// --- Boxes: the draws of d, keeping old's meshes and instance buffers where nothing changed

// The entries FindHiddenFaces' occluder grid would have (HideGridBuild's count)
static long long HideTotal(const SceneData *d)
{
    long long total = 0;
    for (int b = 0; b < d->boxCount; b++) {
        int m = d->boxRefs[b].material, lo[3], hi[3];
        if (m < 0 || m >= d->materialCount || !d->matDefs[m].occluder || TurnedOf(d, b)) continue;
        Box6 bx = { { d->boxes[b].min.x, d->boxes[b].min.y, d->boxes[b].min.z }, { d->boxes[b].max.x, d->boxes[b].max.y, d->boxes[b].max.z } };
        if (HideCells(&bx, lo, hi)) total += (long long)(hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1);
    }
    return total;
}

// The paint key a box of material m is drawn with in x (0: none; a material without paint draws none)
static inline int BoxPaintKey(const SceneData *x, int m, int b)
{
    return x->matDefs[m].painted && x->boxPaintKey ? x->boxPaintKey[b] : 0;
}

// Old ranges of material m: [first[m], end[m]) in old->boxRanges; items of m start at at[m]
typedef struct { int *first, *end, *at, *oldAt; } RangeIndex;

// rebuild: per material of d, nonzero when it was built again (its boxes' meshes are not kept:
// their UVs or textures changed) or is new (beyond old's materials); NULL: none
static bool UploadBoxesLive(MS_Private *p, SceneData *old, SceneData *d, const int *boxFrom, int dirtyBoxes, LiveBorrow *B,
                            const unsigned char *rebuild, MS_EditStats *st)
{
    int mc = d->materialCount, oldMc = old->materialCount;
    CellItem *items = malloc(sizeof(CellItem) * (d->boxCount > 0 ? d->boxCount : 1));
    RangeIndex ri = { calloc(mc + 1, sizeof(int)), calloc(mc + 1, sizeof(int)), calloc(mc + 1, sizeof(int)),
                      calloc(mc + 1, sizeof(int)) };
    B->instFrom = calloc(mc > 0 ? mc : 1, sizeof(bool));
    if (!items || !ri.first || !ri.end || !ri.at || !ri.oldAt || !B->instFrom) {
        free(items); free(ri.first); free(ri.end); free(ri.at); free(ri.oldAt);
        ErrSet("%s: out of memory", d->path);
        return false;
    }
    bool anyMerge = false;
    int n = 0;
    for (int b = 0; b < d->boxCount; b++) {
        int m = d->boxRefs[b].material;
        if (m < 0 || m >= mc) continue;
        items[n] = MakeCellItem(m, b, d->boxes[b], MS_CELL_SIZE);
        items[n++].turned = TurnedOf(d, b) != NULL;
        d->instCount[m]++;
        anyMerge |= d->matDefs[m].merge;
    }
    qsort(items, n, sizeof(CellItem), CompareCellItems);
    // what old drew: per material its ranges and where its items start
    bool canKeep = old->boxOrder && old->instCount && old->boxOrderCount > 0;
    for (int m = 0, at = 0, oat = 0; m < mc; m++) {
        ri.at[m] = at;
        ri.oldAt[m] = oat;
        at += d->instCount[m];
        oat += old->instCount && m < oldMc ? old->instCount[m] : 0;
        ri.first[m] = ri.end[m] = 0;
    }
    for (int r = old->boxRangeCount - 1; r >= 0; r--) ri.first[old->boxRanges[r].group] = r;
    for (int r = 0; r < old->boxRangeCount; r++) ri.end[old->boxRanges[r].group] = r + 1;

    double t0 = GetTime();
    R3D_MeshData cube = { 0 };
    unsigned char *hidden = NULL;
    bool ok = true;
    if (anyMerge) {
        cube = R3D_GenMeshDataCube(1.0f, 1.0f, 1.0f);
        hidden = calloc(d->boxCount > 0 ? d->boxCount : 1, 1);
        ok = cube.vertexCount == 24 && cube.indexCount == 36 && hidden;
        // (over HIDE_ENTRIES_MAX a full search hides nothing: as it, or the copied faces would not be a search's)
        long long total = ok ? HideTotal(d) : 0, oldTotal = ok && old->hidden ? HideTotal(old) : 0;
        bool partial = ok && old->hidden && dirtyBoxes <= LIVE_HIDE_PARTIAL_MAX && !DirtyAll(p) && oldTotal <= HIDE_ENTRIES_MAX;
        if (ok && total > HIDE_ENTRIES_MAX) memset(hidden, 0, (size_t)d->boxCount);
        else if (partial) {
            // A box's faces depend only on the boxes that touch it: copied for an unchanged box
            // that touches no changed one, searched again for the rest (in the same grid, so with
            // the same neighbour order as a full search)
            int *affected = NULL, affectedCount = 0, affectedCap = 0;
            for (int b = 0; b < d->boxCount && ok; b++) {
                int m = d->boxRefs[b].material;
                if (m < 0 || m >= mc || !d->matDefs[m].merge) continue;
                bool redo = boxFrom[b] < 0;
                for (int k = 0; k < dirtyBoxes && !redo; k++) redo = BoxesTouch(d->boxes[b], p->dirty[k]);
                if (!redo) hidden[b] = old->hidden[boxFrom[b]];
                else {
                    int *slot = MS_PUSH(affected, affectedCount, affectedCap);
                    if (slot) *slot = b;
                    else ok = false;
                }
            }
            // The grid holds only the occluders that touch an affected box: a cell's list keeps the
            // same boxes in the same order for every box the search visits (the others are never
            // its neighbours)
            unsigned char *far = ok && affectedCount > 0 ? malloc((size_t)d->boxCount) : NULL;
            BoundingBox reach = { { INFINITY, INFINITY, INFINITY }, { -INFINITY, -INFINITY, -INFINITY } };
            for (int k = 0; k < affectedCount; k++) reach = (BoundingBox){ Vector3Min(reach.min, d->boxes[affected[k]].min),
                                                                          Vector3Max(reach.max, d->boxes[affected[k]].max) };
            for (int b = 0; far && b < d->boxCount; b++) {
                int m = d->boxRefs[b].material;
                far[b] = 1;
                if (m < 0 || m >= mc || !d->matDefs[m].occluder || !BoxesTouch(d->boxes[b], reach)) continue;
                for (int k = 0; k < affectedCount && far[b]; k++) far[b] = !BoxesTouch(d->boxes[b], d->boxes[affected[k]]);
            }
            if (ok && affectedCount > 0 && !far) ok = false;
            HideGrid g;
            if (ok && affectedCount > 0 && (ok = HideGridBuild(d, far, &g))) {
                if (!g.slots) memset(hidden, 0, (size_t)d->boxCount);
                else {
                    HideJob J;
                    HideJobInit(&J, d, &g, &cube, hidden);
                    int *nb = malloc(sizeof(int) * (size_t)d->boxCount), *stamp = malloc(sizeof(int) * (size_t)d->boxCount);
                    Rect *piece = malloc(sizeof(Rect) * HIDE_PIECES);
                    ok = nb && stamp && piece;
                    for (int i = 0; ok && i < d->boxCount; i++) stamp[i] = -1;
                    for (int k = 0; ok && k < affectedCount; k++) {
                        hidden[affected[k]] = 0;
                        HideRange(&J, affected[k], affected[k] + 1, nb, stamp, piece);
                    }
                    free(nb); free(stamp); free(piece);
                }
                HideGridFree(&g);
            }
            free(far);
            free(affected);
        } else if (ok) ok = FindHiddenFaces(d, &cube, hidden);
        st->hiddenMs = (GetTime() - t0) * 1000.0;
        // A box whose faces changed is part of the change too (its cells' meshes are rebuilt below anyway)
        for (int b = 0; ok && b < d->boxCount; b++)
            if (boxFrom[b] >= 0 && old->hidden && hidden[b] != old->hidden[boxFrom[b]]) DirtyAdd(p, d->boxes[b]);
        if (ok && old->unit.capacity > 0) {
            d->unit = old->unit;
            B->unit = true;
        } else if (ok) {
            d->unit = R3D_LoadInstanceBuffer(1, R3D_INSTANCE_POSITION | R3D_INSTANCE_SCALE);
            Vector3 *pos = R3D_MapInstances(d->unit, R3D_INSTANCE_POSITION, false);
            Vector3 *scl = R3D_MapInstances(d->unit, R3D_INSTANCE_SCALE, false);
            if (pos && scl) {
                pos[0] = Vector3Zero();
                scl[0] = Vector3One();
            } else ok = false;
            R3D_UnmapInstances(d->unit, (pos ? R3D_INSTANCE_POSITION : 0) | (scl ? R3D_INSTANCE_SCALE : 0));
        }
    }
    double tMeshes = GetTime();
    for (int m = 0; m < mc && ok; m++) {
        int count = d->instCount[m], at = ri.at[m], oat = ri.oldAt[m];
        if (count == 0) continue;
        // The same boxes in the same order as old's draws of m (and, merged, the same faces)
        bool sameList = canKeep && m < oldMc && old->instCount[m] == count;
        for (int k = 0; sameList && k < count; k++) sameList = boxFrom[items[at + k].index] == old->boxOrder[oat + k];
        int first = d->boxRangeCount;
        if (!d->matDefs[m].merge) {
            // (an instanced buffer also carries its boxes' paint keys: the same ones, or none on both sides)
            bool samePaint = sameList && old->matDefs[m].painted == d->matDefs[m].painted;
            for (int k = 0; samePaint && d->matDefs[m].painted && k < count; k++)
                samePaint = BoxPaintKey(d, m, items[at + k].index) == BoxPaintKey(old, m, old->boxOrder[oat + k]);
            if (samePaint && old->inst[m].capacity >= count) {
                d->inst[m] = old->inst[m];
                B->instFrom[m] = true;
            } else if (!LoadBoxInstances(d, m, items + at, count)) {
                ok = false;
                if (d->inst[m].capacity < count) break;
            } else st->instanceBuffersRebuilt++;
            d->boxTrisDrawn += 12LL * count;
        }
        if (ok && !AddRanges(&d->boxRanges, &d->boxRangeCount, &d->boxRangeCap, m, items + at, count)) ok = false;
        if (ok && !MS_RESERVE(B->meshFrom, B->meshFromCap, d->boxRangeCount)) ok = false;
        for (int r = first; ok && r < d->boxRangeCount; r++) B->meshFrom[r] = -1;
        if (!ok || !d->matDefs[m].merge) continue;
        // Merged: walk old's ranges of m alongside (both sorted by big, cell)
        bool keepable = canKeep && m < oldMc && !(rebuild && (rebuild[m] & PLAN_MATERIAL));
        int o = keepable ? ri.first[m] : 0, oEnd = keepable ? ri.end[m] : 0;
        for (int r = first; ok && r < d->boxRangeCount; r++) {
            DrawRange *R = &d->boxRanges[r];
            const CellItem *it = &items[at + R->first];
            int keep = -1;
            while (o < oEnd) {
                const DrawRange *O = &old->boxRanges[o];
                int oi = old->boxOrder[oat + O->first];
                CellItem oc = MakeCellItem(m, oi, old->boxes[oi], MS_CELL_SIZE);
                oc.turned = TurnedOf(old, oi) != NULL;
                int c = oc.big != it->big ? (oc.big < it->big ? -1 : 1) : oc.turned != it->turned ? (oc.turned < it->turned ? -1 : 1)
                      : oc.cx != it->cx ? (oc.cx < it->cx ? -1 : 1) : oc.cz != it->cz ? (oc.cz < it->cz ? -1 : 1) : 0;
                if (c < 0) { o++; continue; }
                if (c == 0 && O->count == R->count) {
                    bool same = true;
                    for (int k = 0; same && k < R->count; k++) {
                        int nb = items[at + R->first + k].index, ob = old->boxOrder[oat + O->first + k];
                        same = boxFrom[nb] == ob && old->hidden && hidden[nb] == old->hidden[ob] &&
                               BoxPaintKey(d, m, nb) == BoxPaintKey(old, m, ob);
                    }
                    if (same) keep = o;
                }
                break;
            }
            if (keep >= 0) {
                R->mesh = old->boxRanges[keep].mesh;
                B->meshFrom[r] = keep;
                int visible = 0;
                for (int k = 0; k < R->count; k++)
                    for (int f = 0; f < 6; f++) visible += !(hidden[items[at + R->first + k].index] >> f & 1);
                d->boxTrisDrawn += 2LL * visible;
                d->boxMeshCount += R->mesh.vertexCount > 0;
                st->rangesKept++;
            } else {
                ok = BuildBoxMesh(d, R, items + at + R->first, hidden, &cube);
                st->rangesRebuilt++;
            }
        }
    }
    st->meshesMs = (GetTime() - tMeshes) * 1000.0;
    R3D_UnloadMeshData(cube);
    if (ok) KeepBoxIndex(d, items, n, &hidden);
    free(hidden);
    free(items);
    free(ri.first); free(ri.end); free(ri.at); free(ri.oldAt);
    if (!ok) ErrSet("%s: out of memory (box meshes)", d->path);
    return ok;
}

// --- The occlusion volume: the voxels near the changed boxes again
#ifdef MS_PATCHED_R3D

// The voxels within AO_DIST_MAX of box b (AoSplatBox's range), clipped to the volume; false if none
static bool AoWindow(const MS_Private *p, BoundingBox b, int lo[3], int hi[3])
{
    const float *rmin = (const float *)&p->aoRegion.min, *bmin = (const float *)&b.min, *bmax = (const float *)&b.max;
    for (int a = 0; a < 3; a++) {
        lo[a] = CellKey((bmin[a] - AO_DIST_MAX - rmin[a]) / p->aoVoxel - 0.5f);
        hi[a] = -CellKey(-((bmax[a] + AO_DIST_MAX - rmin[a]) / p->aoVoxel - 0.5f));
        if (lo[a] < 0) lo[a] = 0;
        if (hi[a] > p->aoDim[a] - 1) hi[a] = p->aoDim[a] - 1;
        if (lo[a] > hi[a]) return false;
    }
    return true;
}

// Recomputes the voxels near p->dirty[0..count) from every box of d (each voxel the same minimum
// over the same boxes as a full bake: identical bytes) and uploads the volume. False when out of memory.
static bool AoUpdate(MS_Private *p, const SceneData *d, int count)
{
    int wins = count <= LIVE_AO_WINDOWS_MAX ? count : 1;
    for (int w = 0; w < wins; w++) {
        int lo[3], hi[3], wdim[3];
        bool any = false;
        if (wins == count) any = AoWindow(p, p->dirty[w], lo, hi);
        else {
            // one window around every changed box
            for (int k = 0; k < count; k++) {
                int l[3], h[3];
                if (!AoWindow(p, p->dirty[k], l, h)) continue;
                for (int a = 0; a < 3; a++) {
                    lo[a] = any && lo[a] < l[a] ? lo[a] : l[a];
                    hi[a] = any && hi[a] > h[a] ? hi[a] : h[a];
                }
                any = true;
            }
        }
        if (!any) continue;
        for (int a = 0; a < 3; a++) wdim[a] = hi[a] - lo[a] + 1;
        size_t vol = (size_t)wdim[0] * wdim[1] * wdim[2];
        float *dist = malloc(vol * sizeof(float)), *axis = malloc(sizeof(float) * (wdim[0] + wdim[1] + wdim[2]));
        if (!dist || !axis) { free(dist); free(axis); return false; }
        for (size_t i = 0; i < vol; i++) dist[i] = AO_DIST_MAX;
        for (int b = 0; b < d->boxCount; b++)
            AoSplat(d, b, (const float *)&p->aoRegion.min, p->aoVoxel, lo, wdim, dist, axis);
        for (int z = 0; z < wdim[2]; z++)
            for (int y = 0; y < wdim[1]; y++) {
                unsigned char *row = p->aoGrid + ((size_t)(z + lo[2]) * p->aoDim[1] + (y + lo[1])) * p->aoDim[0] + lo[0];
                const float *src = dist + ((size_t)z * wdim[1] + y) * wdim[0];
                for (int x = 0; x < wdim[0]; x++) row[x] = AoByte(src[x]);
            }
        free(dist);
        free(axis);
    }
    R3D_SetWorldOcclusion(p->aoGrid, p->aoDim[0], p->aoDim[1], p->aoDim[2], p->aoRegion.min, p->aoVoxel, AO_DIST_MIN,
                          AO_DIST_MAX, p->aoIntensity);
    return true;
}
#endif

// --- The caches after a change: what the dirty regions reach is drawn again, the rest is stamped
// with the new version (as if drawn for it). dirtyBoxes: p->dirty[0..dirtyBoxes) are boxes (the
// occlusion volume holds boxes only). fogAo false: the fog cache and the occlusion volume keep what
// they have (a preview step: they catch up when the edit is confirmed)
static void Restamp(MS_Scene *s, unsigned int oldVersion, const SceneData *old, int dirtyBoxes, bool fogAo, MS_EditStats *st)
{
    MS_Private *p = s->priv;
    SceneData *d = p->d;
    int nd = DirtyAll(p) ? 0 : p->dirtyCount;
    if (p->frameVersion == oldVersion) p->frameVersion = s->version;   // (not a load: no first-frame log)
#ifdef MS_PATCHED_R3D
    if (p->tiles.version == oldVersion) {
        for (int i = 0; i < SUN_TILE_COUNT; i++)
            if (p->tiles.tile[i].valid && DirtyHitsMap(p, 0, nd, p->tiles.tile[i].viewProj)) {
                p->tiles.tile[i].valid = false;
                st->tilesDirty++;
            }
        p->tiles.version = s->version;
    }
#endif
    if (p->camCache.valid && p->camCache.version == oldVersion) {
        if (DirtyHitsMap(p, 0, nd, p->camCache.viewProj)) { p->camCache.valid = false; st->tilesDirty++; }
        else p->camCache.version = s->version;
    }
    for (int i = 0; d->lightShadowDrawn && i < d->lightCount; i++) {
        const R3D_Light *L = &d->lights[i].light;
        if (!d->lights[i].shadow || !d->lightShadowDrawn[i] || L->type == R3D_LIGHT_DIR) continue;
        if (DirtyHitsSphere(p, 0, nd, L->position, L->range)) {
            d->lightShadowDrawn[i] = false;
            st->lightShadowsDirty++;
        }
    }
#ifdef MS_PATCHED_R3D
    if (p->fogCacheVersion && p->fogCacheVersion == oldVersion) {
        bool keep = !fogAo;
        if (fogAo) {
            BoundingBox ro = FogCacheRegion(old, p->rs.fogCacheVoxel), rn = FogCacheRegion(d, p->rs.fogCacheVoxel);
            keep = memcmp(&ro, &rn, sizeof(BoundingBox)) == 0 &&
                   !DirtyHitsMap(p, 0, nd, SunShadowBoxMatrix(d->sunLight.direction, rn, 2.0f));
        }
        if (keep) p->fogCacheVersion = s->version;
        else st->fogDirty = true;           // baked again by the next MS_DrawGeometry
    }
    if (p->aoVersion && p->aoVersion == oldVersion) {
        bool keep = !fogAo || (!DirtyAll(p) && dirtyBoxes == 0);
        if (!keep && p->aoGrid && p->rs.aoVolumeVoxel == p->aoVoxel && p->rs.aoVolumeIntensity == p->aoIntensity && !DirtyAll(p)) {
            int dim[3];
            BoundingBox region = AoLayout(d, p->aoVoxel, dim);
            if (memcmp(&region, &p->aoRegion, sizeof(BoundingBox)) == 0 && memcmp(dim, p->aoDim, sizeof(dim)) == 0) {
                double t0 = GetTime();
                keep = AoUpdate(p, d, dirtyBoxes);
                st->aoMs = (GetTime() - t0) * 1000.0;
            }
        }
        if (!keep || dirtyBoxes > 0 || DirtyAll(p)) st->aoDirty = true;
        if (keep) p->aoVersion = s->version;     // else baked in full by the next MS_DrawGeometry
    }
#else
    (void)old;
    (void)dirtyBoxes;
    (void)fogAo;
#endif
}

// --- MS_EditSetText

MS_ReloadResult MS_EditSetText(MS_Scene *s, const char *text, int size, MS_EditStats *stats)
{
    MS_Private *p = s->priv;
    SceneData *old = p->d;
    MS_EditStats st = { 0 };
    double t0 = GetTime();
    ErrClear();
    if (p->pv) MS_EditPreviewEnd(s);
    if (!old || size < 0 || size > MS_FILE_MAX) {
        ErrSet("%s: no map loaded, or the text is larger than 4 MB", old ? old->path : "map");
        return MS_RELOAD_FAILED;
    }
    char *bytes = malloc((size_t)size + 1);
    SceneData *d = bytes ? calloc(1, sizeof(*d)) : NULL;
    LiveBorrow B = { 0 };
    int *boxFrom = NULL, *modelFrom = NULL;
    bool *oldBoxUsed = NULL, *oldModelUsed = NULL, *oldLightUsed = NULL;
    bool ok = d && InitData(d, old->path);
    if (!ok) ErrSet("%s: out of memory", old->path);
    if (ok) {
        d->live = p;                    // (the provided textures count as files: MS_PaintAtlasProvide)
        memcpy(bytes, text, (size_t)size);
        bytes[size] = '\0';
        ok = ParseScene(d, bytes, size);
        d->text = bytes;
        d->textSize = size;
        bytes = NULL;
        IndexLines(d);
        d->hash = Fnv1a64Update(FNV_OFFSET, d->text, (size_t)size);
        d->mtime = old->mtime;
        d->shaderDefines = p->opt.shaderDefines;
        d->shaderDefinesFile = p->opt.shaderDefinesFile;
        st.parseMs = d->times.parse = (GetTime() - t0) * 1000.0;
    }
    if (ok && !FinishParse(d, &p->opt)) {
        ErrSet("%s: out of memory", d->path);
        ok = false;
    }
    // Anything but objects changed (a material, env, the format): uploaded in full, from memory.
    // Except materials appended at the end, or changed only in uv_faces, filter or albedo_map: those
    // are built again and everything else is kept (LiveMaterialPlan)
    unsigned char *matRebuild = NULL;
    bool full = !old->gpu || old->compiled;
    if (ok && !full && (old->lookHash != d->lookHash || old->materialCount != d->materialCount))
        full = !LiveMaterialPlan(old, d, &matRebuild);
    st.full = full;
    if (ok) PaintPrepare(d);
    if (ok && !p->gpu) ok = CheckModelFiles(d);
    else if (ok) {
        // Model files: the ones old has are taken over, new paths are loaded
        double tm = GetTime();
        d->gpu = true;
        B.fileFrom = malloc(sizeof(int) * (d->modelFileCount > 0 ? (size_t)d->modelFileCount : 1));
        ok = B.fileFrom != NULL;
        if (!ok) ErrSet("%s: out of memory", d->path);
        for (int i = 0; ok && i < d->modelFileCount; i++) B.fileFrom[i] = -1;
        for (int i = 0; ok && i < d->modelFileCount; i++) {
            for (int k = 0; !full && k < old->modelFileCount && B.fileFrom[i] < 0; k++)
                if (old->modelFiles[k].loaded && strcmp(old->modelFiles[k].path, d->modelFiles[i].path) == 0) B.fileFrom[i] = k;
            if (B.fileFrom[i] >= 0) {
                MS_ModelFile *F = &d->modelFiles[i];
                const MS_ModelFile *O = &old->modelFiles[B.fileFrom[i]];
                F->model = O->model;
                F->loaded = true;
                F->bounds = O->bounds;
                F->meshCount = O->meshCount;
                F->triangleCount = O->triangleCount;
                F->materialCount = O->materialCount;
                d->modelPick[i] = old->modelPick[B.fileFrom[i]];
                if (d->filePaint) PaintLiveUnwrap(p, d, i, old, B.fileFrom[i]);
            } else ok = LoadModelFile(d, i);
        }
        st.modelsMs = (GetTime() - tm) * 1000.0;
    }
    if (ok) ok = ResolveModels(d);
    if (ok) {
        FinishGeometry(d);
        double tb = GetTime();
        ok = BuildColliderBvh(d);
        st.bvhMs = (GetTime() - tb) * 1000.0;
    }
    if (ok) {
        double te = GetTime();
        if (!BuildEntities(d)) {
            ErrSet("%s: out of memory", d->path);
            ok = false;
        }
        if (ok && d->paintCount > 0) PaintResolve(d);
        st.entitiesMs = (GetTime() - te) * 1000.0;
    }

    // What changed: objects without an identical partner, boxes first (the occlusion volume and
    // the hidden faces look at boxes only), then models
    int dirtyBoxes = 0;
    p->dirtyCount = 0;
    if (ok) {
        int lightsChanged = 0;
        int *lightFrom = malloc(sizeof(int) * (d->lightCount > 0 ? (size_t)d->lightCount : 1));
        for (int j = 0; lightFrom && j < d->lightCount; j++) lightFrom[j] = -1;
        boxFrom = malloc(sizeof(int) * (d->boxCount > 0 ? (size_t)d->boxCount : 1));
        modelFrom = malloc(sizeof(int) * (d->modelCount > 0 ? (size_t)d->modelCount : 1));
        ok = lightFrom && boxFrom && modelFrom &&
             LiveMatchAll(old, old->boxCount, d, d->boxCount, BoxKey, BoxSame, boxFrom, &oldBoxUsed, &st.boxesChanged) &&
             LiveMatchAll(old, old->modelCount, d, d->modelCount, ModelKey, ModelSame, modelFrom, &oldModelUsed, &st.modelsChanged) &&
             LiveMatchAll(old, old->lightCount, d, d->lightCount, LightKey, LightSame, lightFrom, &oldLightUsed, &lightsChanged);
        st.lightsChanged = lightsChanged;
        B.lightFrom = lightFrom;
        if (!ok) ErrSet("%s: out of memory", d->path);
        for (int i = 0; ok && i < old->boxCount; i++) if (!oldBoxUsed[i]) DirtyAdd(p, old->boxes[i]);
        for (int j = 0; ok && j < d->boxCount; j++) if (boxFrom[j] < 0) DirtyAdd(p, d->boxes[j]);
        // A material built again (a new texture or filter: its alpha may cut other shadows; an
        // albedo map also makes it no occluder) changes what its unmoved objects do too
        for (int j = 0; ok && matRebuild && j < d->boxCount; j++)
            if (boxFrom[j] >= 0 && (matRebuild[d->boxRefs[j].material] & PLAN_MATERIAL)) DirtyAdd(p, d->boxes[j]);
        dirtyBoxes = p->dirtyCount;
        for (int i = 0; ok && i < old->modelCount; i++) if (!oldModelUsed[i]) DirtyAdd(p, old->models[i].bounds);
        for (int j = 0; ok && j < d->modelCount; j++) if (modelFrom[j] < 0) DirtyAdd(p, d->models[j].bounds);
        for (int j = 0; ok && matRebuild && j < d->modelCount; j++) {
            bool uses = false;
            for (int sl = 0; modelFrom[j] >= 0 && sl < d->modelFiles[d->models[j].file].materialCount && !uses; sl++) {
                int m = SlotMaterial(d, &d->models[j], sl);
                uses = m >= 0 && (matRebuild[m] & PLAN_MATERIAL);
            }
            if (uses) DirtyAdd(p, d->models[j].bounds);
        }
        if (DirtyAll(p)) dirtyBoxes = 0;
    }

    // The paint layer: a material whose paint changed (it started or stopped drawing paint, or its
    // atlas, filter or atlas size changed) gets its shader alias again (PLAN_PAINT). With the
    // patched R3D every alias carries its material's sort key (ShaderSortKey), so the draw order
    // stays a load's. Without it R3D draws in the order the aliases were made (their serial), which
    // must stay a load's (material order), or coplanar faces of two materials could swap: so the
    // materials after it that have a shader get their alias again too.
    if (ok && p->gpu && !full) {
        bool after = false;
        for (int i = 0; i < old->materialCount; i++) {
#ifdef MS_PATCHED_R3D
            bool again = !PaintSigSame(old, d, i);
            (void)after;
#else
            bool again = !PaintSigSame(old, d, i) || (after && (d->materials[i].shader || d->matDefs[i].painted));
#endif
            if (!again) continue;
            after = true;
            if (!matRebuild && !(matRebuild = calloc((size_t)d->materialCount, 1))) {
                ErrSet("%s: out of memory", d->path);
                ok = false;
                break;
            }
            matRebuild[i] |= PLAN_PAINT;
        }
    }

    double tu = GetTime();
    if (ok && p->gpu && full) {
        ok = UploadGpu(d);        // (frees what it made on failure)
        if (ok) for (int j = 0; j < d->lightCount; j++) B.lightFrom[j] = -1;
        if (ok && d->anyPaint) st.paintTables++;
    } else if (ok && p->gpu) {
        d->gpu = true;
        d->inst = calloc(d->materialCount, sizeof(R3D_InstanceBuffer));
        d->instCount = calloc(d->materialCount, sizeof(int));
        d->lightShadow = calloc(d->lightCount > 0 ? d->lightCount : 1, sizeof(R3D_ShadowMap));
        d->lightShadowDrawn = calloc(d->lightCount > 0 ? d->lightCount : 1, sizeof(bool));
        d->shaderFiles = malloc(sizeof(ShaderFile) * (old->shaderFileCount > 0 ? (size_t)old->shaderFileCount : 1));
        ok = d->inst && d->instCount && d->lightShadow && d->lightShadowDrawn && d->shaderFiles;
        if (!ok) ErrSet("%s: out of memory", d->path);
        if (ok) {
            // Shaders, textures, sky: the same statements, so old's GPU state as it is
            d->shaderFileCount = d->shaderFileCap = old->shaderFileCount;
            for (int f = 0; f < old->shaderFileCount; f++) {
                ShaderFile *F = &d->shaderFiles[f];
                *F = old->shaderFiles[f];
                for (int i = 0; F->rel && i < d->materialCount; i++)   // (rel points into the materials' strings)
                    if (d->materials[i].shader && strcmp(d->materials[i].shader, F->rel) == 0) { F->rel = d->materials[i].shader; break; }
            }
            d->textures = old->textures;
            d->textureCount = old->textureCount;
            d->textureCap = old->textureCap;
            d->sky = old->sky;
            d->ambientMap = old->ambientMap;
            B.materials = true;
            B.textureCount = old->textureCount;
            B.matRebuild = matRebuild;
            // Materials: old's as they are; one changed in uv_faces, filter or albedo_map keeps old's
            // shader and gets its textures again; one whose paint changed gets its shader alias again
            // (PLAN_PAINT: the paint version or the original); an appended one is built (in material
            // order, so its shader alias comes after old's, as in a load)
            for (int i = 0; ok && i < d->materialCount; i++) {
                int how = matRebuild ? matRebuild[i] : 0;
                if (how == 0) {
                    d->materials[i].material = old->materials[i].material;
                    d->matDefs[i].merge = old->matDefs[i].merge;
                    d->matDefs[i].occluder = old->matDefs[i].occluder;
                    d->matDefs[i].paintTex = old->matDefs[i].paintTex;
                    continue;
                }
                if ((how & PLAN_MATERIAL) == PLAN_NEW) ok = BuildMaterial(d, i);
                else {
                    R3D_Material m = old->materials[i].material, fresh = d->materials[i].material;
                    d->matDefs[i].paintTex = old->matDefs[i].paintTex;
                    if (how & PLAN_MAPS) {
                        m.albedo.texture = fresh.albedo.texture;
                        m.normal.texture = fresh.normal.texture;
                        m.orm.texture = fresh.orm.texture;
                        m.emission.texture = fresh.emission.texture;
                    }
                    d->materials[i].material = m;
                    if (how & PLAN_MAPS) ok = BuildMaterialMaps(d, i);
                    if (ok && (how & PLAN_PAINT)) ok = BuildMaterialShader(d, i);
                }
                if (ok) MaterialDrawMode(d, i);
                st.materialsRebuilt++;
            }
            if (ok && !LiveFaceCubes(old, d, &B)) {
                ErrSet("%s: out of memory", d->path);
                ok = false;
            }
            // The paint keys (the same objects keep theirs) and table, before the box draws use them
            bool tookTable = false;
            if (ok) ok = PaintUpload(d, old, boxFrom, modelFrom, &tookTable);
            B.paintTable = tookTable;
            if (ok && d->anyPaint && !tookTable) st.paintTables++;
            if (ok) ok = UploadBoxesLive(p, old, d, boxFrom, dirtyBoxes, &B, matRebuild, &st);
        }
        // Lights: an identical light keeps its shadow map (drawn again below if a change reaches it)
        for (int j = 0; ok && j < d->lightCount; j++) {
            int i = B.lightFrom[j];
            d->lightShadowDrawn[j] = false;
            if (i >= 0 && (!old->lights[i].shadow || !old->lightShadow[i].handle)) i = B.lightFrom[j] = -1;
            if (i >= 0) {
                d->lightShadow[j] = old->lightShadow[i];
                d->lightShadowDrawn[j] = old->lightShadowDrawn[i];
            } else if (d->lights[j].shadow) {
                d->lightShadow[j] = R3D_LoadShadowMap(d->lights[j].light.type);
                st.lightShadowsDirty++;
            }
        }
        // Paint meshes of the painted model files: old's when built from the same unwrap
        if (ok) ok = PaintMeshesLive(old, d, &B, &st);
        if (ok) ok = BuildModelGroups(d);
    }
    st.uploadMs = (GetTime() - tu) * 1000.0 - st.hiddenMs - st.meshesMs;
    free(boxFrom);
    free(modelFrom);
    free(oldBoxUsed);
    free(oldModelUsed);
    free(oldLightUsed);
    free(bytes);
    if (!ok) {
        if (d && B.materials) LiveMaterialsBack(old, d, &B);
        if (d) LiveRelease(d, true, d, &B);
        FreeData(d);
        free(matRebuild);
        LiveBorrowFree(&B);
        p->dirtyCount = 0;
        return MS_RELOAD_FAILED;
    }

    // Swap (new before old, as a load), then the caches
    unsigned int oldVersion = s->version;
    p->d = d;
    s->version++;
    Publish(s, d);
    PaintPublish(d);
    if (p->gpu) {
        // (an alias a material replaced because its paint changed: old's, not shared)
        for (int i = 0; matRebuild && B.materials && i < old->materialCount; i++) {
            R3D_SurfaceShader *sh = old->materials[i].material.shader;
            if ((matRebuild[i] & PLAN_PAINT) && sh && sh != d->materials[i].material.shader) R3D_UnloadSurfaceShader(sh);
        }
        LiveRelease(old, false, d, &B);
        LiveRetire(p, old);
        if (full) {
            // a reload from memory: every cache is drawn again (they key on the version)
        } else Restamp(s, oldVersion, old, dirtyBoxes, true, &st);
    }
    FreeData(old);
    free(matRebuild);
    LiveBorrowFree(&B);
    st.ms = (GetTime() - t0) * 1000.0;
    if (stats) *stats = st;
    return MS_RELOADED;
}

void MS_EditMarkSaved(MS_Scene *s)
{
    MS_Private *p = s->priv;
    SceneData *d = p->d;
    if (!d || !d->text) return;
    // As if the package had been loaded from disk now: watched files, their mtimes, the hash
    // MS_Poll compares with (the written bytes are the text the scene shows)
    SetWatch(p, d);
    p->triedHash = CombinedHash(d->text, d->textSize, p->watch, p->watchCount);
    p->settleTime = 0.0f;
    d->mtime = GetFileModTime(d->path);
    s->mtime = d->mtime;
}

// --- Preview: objects follow a transform without a new text

static void PreviewFree(struct EditPreview *v)
{
    if (!v) return;
    if (v->buf.capacity > 0) R3D_UnloadInstanceBuffer(v->buf);
    for (int r = 0; v->turnMesh && r < v->drawRanges; r++) R3D_UnloadMesh(v->turnMesh[r]);
    R3D_UnloadMeshData(v->cube);
    free(v->objs); free(v->boxOrig); free(v->turnOrig); free(v->modelOrig); free(v->localFirst); free(v->colliderOrig); free(v->colliderAt);
    free(v->lightOrig); free(v->entityOrig); free(v->reach); free(v->boxSel); free(v->hiddenOrig); free(v->ranges);
    free(v->meshOrig); free(v->instMat); free(v->instSlot); free(v->instScaleOrig); free(v->modelRangeOrig);
    free(v->bufBox); free(v->turnMesh); free(v->turnBounds); free(v->drawMat); free(v->drawFirst); free(v->drawCount); free(v->drawBounds);
    free(v);
}

static BoundingBox BoundsUnion(BoundingBox a, BoundingBox b)
{
    return (BoundingBox){ Vector3Min(a.min, b.min), Vector3Max(a.max, b.max) };
}

// The collider BVH's node bounds after colliders moved (nodes come after their parent)
static void RefitColliderBvh(SceneData *d)
{
    int n;
    const BoundingBox *c = PublishedColliders(d, &n);
    for (int ni = d->colliderNodeCount - 1; ni >= 0; ni--) {
        BvhNode *nd = &d->colliderNodes[ni];
        if (nd->left < 0) {
            nd->mn = (Vector3){ INFINITY, INFINITY, INFINITY };
            nd->mx = (Vector3){ -INFINITY, -INFINITY, -INFINITY };
            for (int k = nd->first; k < nd->first + nd->count; k++) {
                nd->mn = Vector3Min(nd->mn, c[d->colliderOrder[k]].min);
                nd->mx = Vector3Max(nd->mx, c[d->colliderOrder[k]].max);
            }
        } else {
            nd->mn = Vector3Min(d->colliderNodes[nd->left].mn, d->colliderNodes[nd->left + 1].mn);
            nd->mx = Vector3Max(d->colliderNodes[nd->left].mx, d->colliderNodes[nd->left + 1].mx);
        }
    }
}

// Model instance i placed again (ResolveModels for one instance) with its colliders
static void ResolveOneModel(SceneData *d, int i, int localFirst)
{
    MS_ModelInstance *M = &d->models[i];
    RotBoundsCache cache = { 0 };
    const MS_ModelFile *F = &d->modelFiles[M->file];
    BoundingBox fb = F->loaded ? F->bounds : (BoundingBox){ 0 };
    Quaternion qy = QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, M->rotationDeg.x * DEG2RAD);
    Quaternion qx = QuaternionFromAxisAngle((Vector3){ 1, 0, 0 }, M->rotationDeg.y * DEG2RAD);
    Quaternion qz = QuaternionFromAxisAngle((Vector3){ 0, 0, 1 }, M->rotationDeg.z * DEG2RAD);
    M->rotation = QuaternionNormalize(QuaternionMultiply(QuaternionMultiply(qy, qx), qz));
    Matrix m = QuaternionToMatrix(M->rotation);
    m.m0 *= M->scale.x; m.m1 *= M->scale.x; m.m2 *= M->scale.x;
    m.m4 *= M->scale.y; m.m5 *= M->scale.y; m.m6 *= M->scale.y;
    m.m8 *= M->scale.z; m.m9 *= M->scale.z; m.m10 *= M->scale.z;
    BoundingBox lb = RotatedFileBounds(d, M, m, &cache);
    BoundingBox ob = TransformBox(fb, m);
    Vector3 shift = { 0 };
    if (M->origin == MS_ORIGIN_BOTTOM)
        shift = (Vector3){ -(ob.min.x + ob.max.x) * 0.5f, -lb.min.y, -(ob.min.z + ob.max.z) * 0.5f };
    else if (M->origin == MS_ORIGIN_CENTER)
        shift = (Vector3){ -(ob.min.x + ob.max.x) * 0.5f, -(lb.min.y + lb.max.y) * 0.5f, -(ob.min.z + ob.max.z) * 0.5f };
    M->translation = Vector3Add(M->position, shift);
    m.m12 = M->translation.x; m.m13 = M->translation.y; m.m14 = M->translation.z;
    M->transform = m;
    M->bounds = (BoundingBox){ Vector3Add(lb.min, M->translation), Vector3Add(lb.max, M->translation) };
    if (M->collide == MS_COLLIDE_AABB && M->colliderCount > 0) d->colliders[M->colliderFirst] = M->bounds;
    else if (M->collide == MS_COLLIDE_BOXES)
        for (int k = 0; k < M->colliderCount; k++) d->colliders[M->colliderFirst + k] = TransformBox(d->localColliders[localFirst + k], m);
    free(cache.e);
}

// Writes model instance i into its group's instance buffer
static void PreviewModelInstance(SceneData *d, int i)
{
    if (!d->modelSlotOf || !d->modelRangeOf) return;
    const MS_ModelInstance *M = &d->models[i];
    DrawRange *R = &d->modelRanges[d->modelRangeOf[i]];
    ModelGroup *G = &d->modelGroups[R->group];
    int k = d->modelSlotOf[i];
    Vector3 *pos = R3D_MapInstances(G->inst, R3D_INSTANCE_POSITION, false);
    Quaternion *rot = R3D_MapInstances(G->inst, R3D_INSTANCE_ROTATION, false);
    Vector3 *scl = R3D_MapInstances(G->inst, R3D_INSTANCE_SCALE, false);
    if (pos) pos[k] = M->translation;
    if (rot) rot[k] = M->rotation;
    if (scl) scl[k] = M->scale;
    R3D_UnmapInstances(G->inst, (pos ? R3D_INSTANCE_POSITION : 0) | (rot ? R3D_INSTANCE_ROTATION : 0) | (scl ? R3D_INSTANCE_SCALE : 0));
    R->bounds = BoundsUnion(R->bounds, M->bounds);
}

// The preview buffer slots of the selected boxes, and the draw ranges' bounds
static void PreviewBoxBuffer(SceneData *d, struct EditPreview *v)
{
    if (v->bufCount == 0) return;
    Vector3 *pos = R3D_MapInstances(v->buf, R3D_INSTANCE_POSITION, false);
    Vector3 *scl = R3D_MapInstances(v->buf, R3D_INSTANCE_SCALE, false);
    Quaternion *rot = R3D_MapInstances(v->buf, R3D_INSTANCE_ROTATION, false);
    for (int k = 0; pos && scl && rot && k < v->bufCount; k++) {
        // a box with a rotation: its transform (centre, size, rotation); else its bounds as today.
        // (A turned box of a merged material is drawn by turnMesh instead, as its range will.)
        const MS_Transform *et = &d->entityTransforms[d->objEntities[v->bufBox[k]]];
        if (et->rotationDeg.x != 0.0f || et->rotationDeg.y != 0.0f || et->rotationDeg.z != 0.0f) {
            pos[k] = et->position;
            scl[k] = d->matDefs[d->boxRefs[v->bufBox[k]].material].merge ? Vector3Zero() : et->scale;
            rot[k] = RotationYpr(et->rotationDeg);
            continue;
        }
        const BoundingBox *B = &d->boxes[v->bufBox[k]];
        pos[k] = Vector3Scale(Vector3Add(B->min, B->max), 0.5f);
        scl[k] = Vector3Subtract(B->max, B->min);
        rot[k] = QuaternionIdentity();
    }
    R3D_UnmapInstances(v->buf, (pos ? R3D_INSTANCE_POSITION : 0) | (scl ? R3D_INSTANCE_SCALE : 0) | (rot ? R3D_INSTANCE_ROTATION : 0));
    for (int r = 0; r < v->drawRanges; r++) {
        BoundingBox b = d->boxes[v->bufBox[v->drawFirst[r]]];
        for (int k = 1; k < v->drawCount[r]; k++) b = BoundsUnion(b, d->boxes[v->bufBox[v->drawFirst[r] + k]]);
        v->drawBounds[r] = b;
        // the turned boxes of a merged material: one mesh of their faces, built as UploadBoxes does
        R3D_UnloadMesh(v->turnMesh[r]);
        v->turnMesh[r] = (R3D_Mesh){ 0 };
        if (!d->matDefs[v->drawMat[r]].merge || v->cube.vertexCount != 24) continue;
        int n = 0;
        for (int k = 0; k < v->drawCount[r]; k++) {
            Vector3 deg = d->entityTransforms[d->objEntities[v->bufBox[v->drawFirst[r] + k]]].rotationDeg;
            n += deg.x != 0.0f || deg.y != 0.0f || deg.z != 0.0f;
        }
        if (n == 0) continue;
        R3D_MeshData md = R3D_LoadMeshData(24 * n, 36 * n);
        BoundingBox tb = { { INFINITY, INFINITY, INFINITY }, { -INFINITY, -INFINITY, -INFINITY } };
        for (int k = 0; md.vertices && md.indices && k < v->drawCount[r]; k++) {
            int box = v->bufBox[v->drawFirst[r] + k];
            const MS_Transform *et = &d->entityTransforms[d->objEntities[box]];
            if (et->rotationDeg.x == 0.0f && et->rotationDeg.y == 0.0f && et->rotationDeg.z == 0.0f) continue;
            Vector3 h = Vector3Scale(et->scale, 0.5f);
            MS_TurnedBox t;
            BoundingBox a;
            TurnedBoxMake((BoundingBox){ Vector3Subtract(et->position, h), Vector3Add(et->position, h) }, et->rotationDeg, &t, &a);
            TurnedFaces(&md, &t, MaterialCube(d, v->drawMat[r], &v->cube));
            tb = BoundsUnion(tb, d->boxes[box]);
        }
        if (md.vertices && md.indices) {
            v->turnBounds[r] = tb;
            v->turnMesh[r] = R3D_LoadMesh(R3D_PRIMITIVE_TRIANGLES, md, &v->turnBounds[r]);
        }
        R3D_UnloadMeshData(md);
    }
}

// The merged meshes of the ranges in v->ranges built again from their boxes minus the selected
// ones (hidden faces as they are in d->hidden); the meshes they had stay in v->meshOrig
static bool PreviewRebuildRanges(SceneData *d, struct EditPreview *v, const R3D_MeshData *cube)
{
    int at = 0, m = 0;
    bool ok = true;
    CellItem *tmp = malloc(sizeof(CellItem) * (d->boxCount > 0 ? (size_t)d->boxCount : 1));
    if (!tmp) return false;
    for (int k = 0; k < v->rangeCount && ok; k++) {
        DrawRange *R = &d->boxRanges[v->ranges[k]];
        at = 0;
        for (m = 0; m < R->group; m++) at += d->instCount[m];
        DrawRange t = *R;
        t.mesh = (R3D_Mesh){ 0 };
        t.count = 0;
        for (int j = 0; j < R->count; j++) {
            int b = d->boxOrder[at + R->first + j];
            if (!v->boxSel[b]) tmp[t.count++] = (CellItem){ .group = R->group, .index = b, .bounds = d->boxes[b] };
        }
        ok = t.count == 0 || BuildBoxMesh(d, &t, tmp, d->hidden, cube);
        R->mesh = t.mesh;
    }
    free(tmp);
    return ok;
}

// The box draws as they were at Begin: merged meshes, hidden faces, instanced slots
static void PreviewUndoDraws(SceneData *d, struct EditPreview *v)
{
    for (int k = 0; v->ranges && v->meshOrig && k < v->rangeCount; k++) {
        R3D_Mesh *cur = &d->boxRanges[v->ranges[k]].mesh;
        if (memcmp(cur, &v->meshOrig[k], sizeof(R3D_Mesh)) != 0) R3D_UnloadMesh(*cur);     // (not rebuilt: it is the original)
        *cur = v->meshOrig[k];
    }
    if (v->hiddenOrig && d->hidden) memcpy(d->hidden, v->hiddenOrig, (size_t)d->boxCount);
    for (int k = 0; k < v->instCount; k++) {
        Vector3 *scl = R3D_MapInstances(d->inst[v->instMat[k]], R3D_INSTANCE_SCALE, false);
        if (scl) {
            scl[v->instSlot[k]] = v->instScaleOrig[k];
            R3D_UnmapInstances(d->inst[v->instMat[k]], R3D_INSTANCE_SCALE);
        }
    }
    d->boxTrisDrawn = v->trisDrawn;
    d->boxMeshCount = v->meshCount;
}

bool MS_EditPreviewBegin(MS_Scene *s, const MS_ObjRef *objs, int count)
{
    MS_Private *p = s->priv;
    SceneData *d = p->d;
    if (p->pv || !p->gpu || !d || !d->gpu || count <= 0) return false;
    for (int i = 0; i < count; i++) {
        int n = objs[i].kind == MS_OBJ_BOX ? d->boxCount : objs[i].kind == MS_OBJ_MODEL ? d->modelCount
              : objs[i].kind == MS_OBJ_LIGHT ? d->lightCount : 0;
        if (objs[i].index < 0 || objs[i].index >= n) return false;
        if (objs[i].kind == MS_OBJ_BOX && (!d->boxOrder || !d->boxRangeOf)) return false;
        if (objs[i].kind == MS_OBJ_MODEL && (!d->modelSlotOf || !d->modelRangeOf)) return false;
    }
    struct EditPreview *v = calloc(1, sizeof(*v));
    if (!v) return false;
    size_t nc = (size_t)count;
    v->count = count;
    v->objs = malloc(sizeof(MS_ObjRef) * nc);
    v->boxOrig = calloc(nc, sizeof(BoundingBox));
    v->turnOrig = calloc(nc, sizeof(MS_TurnedBox));
    v->modelOrig = calloc(nc, sizeof(MS_ModelInstance));
    v->localFirst = calloc(nc, sizeof(int));
    v->colliderAt = calloc(nc, sizeof(int));
    v->lightOrig = calloc(nc, sizeof(MS_LightDef));
    v->entityOrig = calloc(nc, sizeof(MS_Transform));
    v->reach = calloc(nc, sizeof(BoundingBox));
    v->boxSel = calloc(d->boxCount > 0 ? (size_t)d->boxCount : 1, 1);
    v->modelRangeOrig = malloc(sizeof(BoundingBox) * (d->modelRangeCount > 0 ? (size_t)d->modelRangeCount : 1));
    v->bufBox = malloc(sizeof(int) * nc);
    v->turnMesh = calloc(nc, sizeof(R3D_Mesh));
    v->turnBounds = calloc(nc, sizeof(BoundingBox));
    v->drawMat = malloc(sizeof(int) * nc);
    v->drawFirst = malloc(sizeof(int) * nc);
    v->drawCount = malloc(sizeof(int) * nc);
    v->drawBounds = malloc(sizeof(BoundingBox) * nc);
    v->instMat = malloc(sizeof(int) * nc);
    v->instSlot = malloc(sizeof(int) * nc);
    v->instScaleOrig = malloc(sizeof(Vector3) * nc);
    v->trisDrawn = d->boxTrisDrawn;
    v->meshCount = d->boxMeshCount;
    bool ok = v->objs && v->boxOrig && v->turnOrig && v->modelOrig && v->localFirst && v->colliderAt && v->lightOrig && v->entityOrig && v->reach &&
              v->boxSel && v->modelRangeOrig && v->bufBox && v->turnMesh && v->turnBounds && v->drawMat && v->drawFirst && v->drawCount && v->drawBounds &&
              v->instMat && v->instSlot && v->instScaleOrig;
    // What everything was at Begin
    int colliders = 0;
    for (int i = 0; ok && i < count; i++) {
        MS_ObjRef o = objs[i];
        v->objs[i] = o;
        int e = d->objEntities[o.kind == MS_OBJ_BOX ? o.index : o.kind == MS_OBJ_MODEL ? d->boxCount + o.index
                               : d->boxCount + d->modelCount + o.index];
        v->entityOrig[i] = d->entityTransforms[e];
        if (o.kind == MS_OBJ_BOX) {
            v->boxOrig[i] = v->reach[i] = d->boxes[o.index];
            if (TurnedOf(d, o.index)) v->turnOrig[i] = *TurnedOf(d, o.index);
            v->boxSel[o.index] = 1;
        } else if (o.kind == MS_OBJ_MODEL) {
            v->modelOrig[i] = d->models[o.index];
            v->reach[i] = d->models[o.index].bounds;
            v->colliderAt[i] = colliders;
            colliders += d->models[o.index].colliderCount;
            int lf = 0;     // its `collide box` boxes: after those of the instances before it
            for (int k = 0; k < o.index; k++) if (d->models[k].collide == MS_COLLIDE_BOXES) lf += d->models[k].colliderCount;
            v->localFirst[i] = lf;
        } else {
            v->lightOrig[i] = d->lights[o.index];
            v->reach[i] = (BoundingBox){ d->lights[o.index].light.position, d->lights[o.index].light.position };
        }
    }
    v->colliderOrig = malloc(sizeof(BoundingBox) * (colliders > 0 ? (size_t)colliders : 1));
    ok = ok && v->colliderOrig;
    for (int i = 0; ok && i < count; i++)
        if (objs[i].kind == MS_OBJ_MODEL && d->models[objs[i].index].colliderCount > 0)
            memcpy(&v->colliderOrig[v->colliderAt[i]], &d->colliders[d->models[objs[i].index].colliderFirst],
                   sizeof(BoundingBox) * d->models[objs[i].index].colliderCount);
    for (int r = 0; ok && r < d->modelRangeCount; r++) v->modelRangeOrig[r] = d->modelRanges[r].bounds;

    // The selected boxes leave their draws: merged ranges are built again without them (with the
    // faces they hid shown again), instanced ones get a zero scale in their buffer
    R3D_MeshData cube = { 0 };
    if (ok) {
        int at = 0;
        int *matAt = calloc(d->materialCount + 1, sizeof(int));
        ok = matAt != NULL;
        for (int m = 0; ok && m < d->materialCount; m++) { matAt[m] = at; at += d->instCount[m]; }
        // boxes that touch a selected merged box: their faces are searched again without it
        int *touch = NULL, touchCount = 0, touchCap = 0;
        bool anyMerged = false;
        for (int i = 0; ok && i < count; i++) {
            if (objs[i].kind != MS_OBJ_BOX) continue;
            int b = objs[i].index, m = d->boxRefs[b].material;
            if (m < 0 || m >= d->materialCount) continue;
            if (d->matDefs[m].merge) { anyMerged = true; continue; }
            // instanced: its slot in the material's buffer
            int slot = -1;
            for (int k = 0; k < d->instCount[m] && slot < 0; k++) if (d->boxOrder[matAt[m] + k] == b) slot = k;
            if (slot < 0) continue;
            Vector3 *scl = R3D_MapInstances(d->inst[m], R3D_INSTANCE_SCALE, false);
            if (scl) {
                v->instMat[v->instCount] = m;
                v->instSlot[v->instCount] = slot;
                v->instScaleOrig[v->instCount++] = scl[slot];
                scl[slot] = Vector3Zero();
                R3D_UnmapInstances(d->inst[m], R3D_INSTANCE_SCALE);
            }
        }
        if (ok && anyMerged && d->hidden) {
            cube = R3D_GenMeshDataCube(1.0f, 1.0f, 1.0f);
            v->hiddenOrig = malloc((size_t)d->boxCount);
            ok = v->hiddenOrig && cube.vertexCount == 24;
            if (ok) memcpy(v->hiddenOrig, d->hidden, (size_t)d->boxCount);
            for (int b = 0; ok && b < d->boxCount; b++) {
                int m = d->boxRefs[b].material;
                if (v->boxSel[b] || m < 0 || m >= d->materialCount || !d->matDefs[m].merge) continue;
                bool near = false;
                for (int i = 0; i < count && !near; i++)
                    near = objs[i].kind == MS_OBJ_BOX && BoxesTouch(d->boxes[b], d->boxes[objs[i].index]);
                if (!near) continue;
                int *slot = MS_PUSH(touch, touchCount, touchCap);
                if (slot) *slot = b;
                else ok = false;
            }
            HideGrid g;
            if (ok && touchCount > 0 && (ok = HideGridBuild(d, v->boxSel, &g))) {
                if (g.slots) {
                    HideJob J;
                    HideJobInit(&J, d, &g, &cube, d->hidden);
                    int *nb = malloc(sizeof(int) * (size_t)d->boxCount), *stamp = malloc(sizeof(int) * (size_t)d->boxCount);
                    Rect *piece = malloc(sizeof(Rect) * HIDE_PIECES);
                    ok = nb && stamp && piece;
                    for (int i = 0; ok && i < d->boxCount; i++) stamp[i] = -1;
                    for (int k = 0; ok && k < touchCount; k++) {
                        d->hidden[touch[k]] = 0;
                        HideRange(&J, touch[k], touch[k] + 1, nb, stamp, piece);
                    }
                    free(nb); free(stamp); free(piece);
                }
                HideGridFree(&g);
            }
            // the ranges of the selected merged boxes and of every box whose faces changed
            v->ranges = malloc(sizeof(int) * (size_t)(count + touchCount));
            v->meshOrig = malloc(sizeof(R3D_Mesh) * (size_t)(count + touchCount));
            ok = ok && v->ranges && v->meshOrig;
            for (int k = 0; ok && k < count + touchCount; k++) {
                int b = k < count ? (objs[k].kind == MS_OBJ_BOX ? objs[k].index : -1) : touch[k - count];
                if (b < 0 || d->boxRangeOf[b] < 0 || !d->matDefs[d->boxRefs[b].material].merge) continue;
                if (k >= count && d->hidden[b] == v->hiddenOrig[b]) continue;
                int r = d->boxRangeOf[b];
                bool seen = false;
                for (int j = 0; j < v->rangeCount && !seen; j++) seen = v->ranges[j] == r;
                if (seen) continue;
                v->meshOrig[v->rangeCount] = d->boxRanges[r].mesh;
                v->ranges[v->rangeCount++] = r;
            }
            if (ok) ok = PreviewRebuildRanges(d, v, &cube);
        }
        free(touch);
        free(matAt);
    }
    R3D_UnloadMeshData(cube);

    // The preview draw: the selected boxes as cubes, one range per material
    for (int i = 0; ok && i < count; i++)
        if (objs[i].kind == MS_OBJ_BOX && d->boxRefs[objs[i].index].material >= 0) v->bufBox[v->bufCount++] = objs[i].index;
    if (ok && v->bufCount > 0) {
        // (insertion sort by material, then box: a selection is small)
        for (int a = 1; a < v->bufCount; a++)
            for (int b = a; b > 0; b--) {
                int x = v->bufBox[b - 1], y = v->bufBox[b];
                int mx = d->boxRefs[x].material, my = d->boxRefs[y].material;
                if (mx < my || (mx == my && x < y)) break;
                v->bufBox[b - 1] = y;
                v->bufBox[b] = x;
            }
        for (int k = 0; k < v->bufCount; k++) {
            int m = d->boxRefs[v->bufBox[k]].material;
            if (v->drawRanges == 0 || v->drawMat[v->drawRanges - 1] != m) {
                v->drawMat[v->drawRanges] = m;
                v->drawFirst[v->drawRanges] = k;
                v->drawCount[v->drawRanges++] = 0;
            }
            v->drawCount[v->drawRanges - 1]++;
        }
        // (with rotations: a box may turn during the preview; identity draws as without them)
        v->buf = R3D_LoadInstanceBuffer(v->bufCount, R3D_INSTANCE_POSITION | R3D_INSTANCE_ROTATION | R3D_INSTANCE_SCALE);
        v->cube = R3D_GenMeshDataCube(1.0f, 1.0f, 1.0f);
        ok = v->buf.capacity >= v->bufCount;
        if (ok) PreviewBoxBuffer(d, v);
    }
    if (!ok) {
        PreviewUndoDraws(d, v);
        PreviewFree(v);
        return false;
    }
    p->pv = v;
    unsigned int oldVersion = s->version++;
    p->dirtyCount = 0;
    MS_EditStats st = { 0 };
    Restamp(s, oldVersion, d, 0, false, &st);
    return true;
}

bool MS_EditPreviewSet(MS_Scene *s, const MS_Transform *tr)
{
    MS_Private *p = s->priv;
    SceneData *d = p->d;
    struct EditPreview *v = p->pv;
    if (!v) return false;
    p->dirtyCount = 0;
    bool boxes = false, colliders = false;
    for (int i = 0; i < v->count; i++) {
        MS_ObjRef o = v->objs[i];
        BoundingBox before, after;
        int e;
        if (o.kind == MS_OBJ_BOX) {
            e = d->objEntities[o.index];
            before = d->boxes[o.index];
            Vector3 h = Vector3Scale(tr[i].scale, 0.5f);
            after = (BoundingBox){ Vector3Subtract(tr[i].position, h), Vector3Add(tr[i].position, h) };
            Vector3 r = tr[i].rotationDeg;
            if (r.x != 0.0f || r.y != 0.0f || r.z != 0.0f || TurnedOf(d, o.index)) {
                // turned: its AABB as the box (a turned box's record follows; an axis-aligned one
                // collides and picks as that AABB until the edit is made)
                MS_TurnedBox t;
                TurnedBoxMake(after, r, &t, &after);
                if (TurnedOf(d, o.index)) d->turned[d->boxTurned[o.index]] = t;
            }
            d->boxes[o.index] = after;
            if (d->modelCount > 0) d->colliders[o.index] = after;
            d->entityTransforms[e].position = tr[i].position;
            d->entityTransforms[e].rotationDeg = r;
            d->entityTransforms[e].scale = tr[i].scale;
            boxes = colliders = true;
        } else if (o.kind == MS_OBJ_MODEL) {
            MS_ModelInstance *M = &d->models[o.index];
            e = d->objEntities[d->boxCount + o.index];
            before = M->bounds;
            M->position = tr[i].position;
            M->rotationDeg = tr[i].rotationDeg;
            M->scale = tr[i].scale;
            ResolveOneModel(d, o.index, v->localFirst[i]);
            PreviewModelInstance(d, o.index);
            after = M->bounds;
            d->entityTransforms[e] = (MS_Transform){ M->position, M->rotationDeg, M->scale };
            colliders = colliders || M->colliderCount > 0;
        } else {
            MS_LightDef *L = &d->lights[o.index];
            e = d->objEntities[d->boxCount + d->modelCount + o.index];
            Vector3 delta = Vector3Subtract(tr[i].position, L->light.position);
            before = (BoundingBox){ L->light.position, L->light.position };
            L->light.position = tr[i].position;
            if (L->clip) L->clipBox = (BoundingBox){ Vector3Add(L->clipBox.min, delta), Vector3Add(L->clipBox.max, delta) };
            after = (BoundingBox){ L->light.position, L->light.position };
            if (d->lightShadowDrawn) d->lightShadowDrawn[o.index] = false;
            d->entityTransforms[e].position = tr[i].position;
            v->reach[i] = BoundsUnion(v->reach[i], after);
            continue;       // (a light is in no static cache but its own shadow)
        }
        DirtyAdd(p, before);
        DirtyAdd(p, after);
        v->reach[i] = BoundsUnion(v->reach[i], after);
    }
    if (boxes) PreviewBoxBuffer(d, v);
    if (colliders) RefitColliderBvh(d);
    FinishGeometry(d);
    s->bounds = d->bounds;
    unsigned int oldVersion = s->version++;
    MS_EditStats st = { 0 };
    Restamp(s, oldVersion, d, 0, false, &st);
    return true;
}

void MS_EditPreviewEnd(MS_Scene *s)
{
    MS_Private *p = s->priv;
    SceneData *d = p->d;
    struct EditPreview *v = p->pv;
    if (!v) return;
    p->pv = NULL;
    p->dirtyCount = 0;
    bool colliders = false;
    for (int i = 0; i < v->count; i++) {
        MS_ObjRef o = v->objs[i];
        if (o.kind == MS_OBJ_BOX) {
            d->boxes[o.index] = v->boxOrig[i];
            if (TurnedOf(d, o.index)) d->turned[d->boxTurned[o.index]] = v->turnOrig[i];
            if (d->modelCount > 0) d->colliders[o.index] = v->boxOrig[i];
            d->entityTransforms[d->objEntities[o.index]] = v->entityOrig[i];
            colliders = true;
        } else if (o.kind == MS_OBJ_MODEL) {
            MS_ModelInstance *M = &d->models[o.index];
            *M = v->modelOrig[i];
            if (M->colliderCount > 0 && v->colliderOrig)
                memcpy(&d->colliders[M->colliderFirst], &v->colliderOrig[v->colliderAt[i]], sizeof(BoundingBox) * M->colliderCount);
            colliders = colliders || M->colliderCount > 0;
            PreviewModelInstance(d, o.index);
            d->entityTransforms[d->objEntities[d->boxCount + o.index]] = v->entityOrig[i];
        } else if (o.kind == MS_OBJ_LIGHT) {
            d->lights[o.index] = v->lightOrig[i];
            if (d->lightShadowDrawn) d->lightShadowDrawn[o.index] = false;
            d->entityTransforms[d->objEntities[d->boxCount + d->modelCount + o.index]] = v->entityOrig[i];
            continue;
        }
        DirtyAdd(p, v->reach[i]);
    }
    for (int r = 0; v->modelRangeOrig && r < d->modelRangeCount; r++) d->modelRanges[r].bounds = v->modelRangeOrig[r];
    PreviewUndoDraws(d, v);
    if (colliders) RefitColliderBvh(d);
    FinishGeometry(d);
    s->bounds = d->bounds;
    PreviewFree(v);
    // The caches drawn during the preview showed the objects elsewhere: drawn again there
    unsigned int oldVersion = s->version++;
    MS_EditStats st = { 0 };
    Restamp(s, oldVersion, d, 0, false, &st);
}

// The preview's boxes (MS_DrawGeometry)
static void PreviewDraw(MS_Private *p)
{
    const struct EditPreview *v = p->pv;
    const SceneData *d = p->d;
    for (int r = 0; v && r < v->drawRanges; r++) {
        int c = d->matDefs[v->drawMat[r]].faceCube;     // (a face layout's cube, else R3D's)
        R3D_BeginCluster(v->drawBounds[r]);
        R3D_DrawMeshInstancedEx(c > 0 ? d->faceCubes[c - 1].mesh : p->cube, d->materials[v->drawMat[r]].material, v->buf,
                                v->drawFirst[r], v->drawCount[r]);
        R3D_EndCluster();
        if (v->turnMesh[r].vertexCount > 0) {
            R3D_BeginCluster(v->turnBounds[r]);
            R3D_DrawMeshInstancedEx(v->turnMesh[r], d->materials[v->drawMat[r]].material, d->unit, 0, 1);
            R3D_EndCluster();
        }
    }
}

// #endregion

// ======================================================================================
// #region Draw ranges and the draw filter (editor: the render inspector; docs/live-editing.md 3.6)
// ======================================================================================
// Ranges are MS_DrawGeometry's draws: d->boxRanges[], then d->modelRanges[]. Their object lists
// come from the draw order the live edits keep anyway (d->boxOrder: the boxes sorted by material
// and cell; d->modelSlotOf / modelRangeOf: each model instance's place), built on the first query
// after a version change into arrays that only grow, so a preview step (a new version every
// frame, the same draws) allocates nothing.

// Per range its objects (p->rangeObj from p->rangeAt[r]); false when out of memory or the draw
// order is unknown (an earlier out of memory)
static bool DrawRangeObjects(const MS_Scene *s)
{
    MS_Private *p = s->priv;
    const SceneData *d = p->d;
    if (p->rangeData == d && p->rangeVersion == s->version) return true;
    p->rangeData = NULL;
    int nr = d->boxRangeCount + d->modelRangeCount;
    int no = d->boxOrderCount + d->modelCount;
    if (!MS_RESERVE(p->rangeAt, p->rangeAtCap, nr + 1) || !MS_RESERVE(p->rangeObj, p->rangeObjCap, no > 0 ? no : 1) ||
        !MS_RESERVE(p->rangeMatAt, p->rangeMatAtCap, d->materialCount + 1)) return false;
    if (d->boxRangeCount > 0 && (!d->boxOrder || !d->instCount)) return false;
    if (d->modelRangeCount > 0 && (!d->modelSlotOf || !d->modelRangeOf)) return false;
    // Boxes: a range's boxes are items [first, first + count) of its material's run in boxOrder
    for (int m = 0, at = 0; m < d->materialCount; m++) { p->rangeMatAt[m] = at; at += d->instCount ? d->instCount[m] : 0; }
    int at = 0;
    for (int r = 0; r < d->boxRangeCount; r++) {
        const DrawRange *R = &d->boxRanges[r];
        p->rangeAt[r] = at;
        memcpy(p->rangeObj + at, d->boxOrder + p->rangeMatAt[R->group] + R->first, sizeof(int) * (size_t)R->count);
        at += R->count;
    }
    // Models: slot k of a range is instance slot first + k of its group's buffer
    for (int r = 0; r < d->modelRangeCount; r++) {
        p->rangeAt[d->boxRangeCount + r] = at;
        at += d->modelRanges[r].count;
    }
    p->rangeAt[nr] = at;
    for (int i = 0; i < d->modelCount; i++) {
        int r = d->modelRangeOf[i];
        p->rangeObj[p->rangeAt[d->boxRangeCount + r] + d->modelSlotOf[i] - d->modelRanges[r].first] = i;
    }
    p->rangeData = d;
    p->rangeVersion = s->version;
    return true;
}

int MS_DrawRangeCount(const MS_Scene *s)
{
    const SceneData *d = s->priv->d;
    return d->gpu ? d->boxRangeCount + d->modelRangeCount : 0;
}

bool MS_GetDrawRange(const MS_Scene *s, int range, MS_DrawRangeInfo *out)
{
    MS_Private *p = s->priv;
    const SceneData *d = p->d;
    if (range < 0 || range >= MS_DrawRangeCount(s) || !DrawRangeObjects(s)) return false;
    *out = (MS_DrawRangeInfo){ .material = -1, .modelFile = -1, .castShadow = true };
    out->objects = p->rangeObj + p->rangeAt[range];
    out->objectCount = p->rangeAt[range + 1] - p->rangeAt[range];
    if (range < d->boxRangeCount) {
        const DrawRange *R = &d->boxRanges[range];
        bool merged = d->matDefs[R->group].merge;
        out->kind = merged ? MS_DRAW_BOXES_MERGED : MS_DRAW_BOXES_INSTANCED;
        out->material = R->group;
        out->shader = d->materials[R->group].shader;
        out->bounds = R->bounds;
        // (a merged range drawn by a transform preview lacks the selected boxes' faces)
        out->triangles = merged ? R->mesh.indexCount / 3 : 12LL * R->count;
        return true;
    }
    const DrawRange *R = &d->modelRanges[range - d->boxRangeCount];
    const ModelGroup *G = &d->modelGroups[R->group];
    const MS_ModelFile *F = &d->modelFiles[G->file];
    out->kind = MS_DRAW_MODELS;
    out->modelFile = G->file;
    out->slotMaterials = G->slotMat;
    out->slotCount = F->materialCount;
    out->bounds = R->bounds;
    out->triangles = (long long)F->triangleCount * R->count;
    out->castShadow = G->castShadow;
    return true;
}

// Everything a shadow map holds is drawn again: the sun's tiles or camera-fitted map, every
// point / spot shadow (the fog cache and the occlusion volume keep the whole scene)
static void FilterInvalidate(MS_Private *p)
{
    for (int i = 0; i < SUN_TILE_COUNT; i++) p->tiles.tile[i].valid = false;
    p->tiles.current = -1;
    p->camCache.valid = false;
    for (int i = 0; p->d->lightShadowDrawn && i < p->d->lightCount; i++) p->d->lightShadowDrawn[i] = false;
}

void MS_SetDrawFilter(MS_Scene *s, const bool *hidden, int count)
{
    MS_Private *p = s->priv;
    const SceneData *d = p->d;
    int nr = MS_DrawRangeCount(s);
    if (!hidden || count < 0) count = 0;
    if (count > nr) count = nr;
    bool any = false;
    for (int r = 0; r < count && !any; r++) any = hidden[r];
    if (any && !MS_RESERVE(p->drawHidden, p->drawHiddenCap, count)) any = false;   // (out of memory: off)
    // The objects it hides decide whether the shadows change: the same objects after a live
    // edit (other range indices) or a preview step (a new version) keep every shadow map.
    // filterObjCount: the objects filterObj describes, 0 = none hidden, -1 = unknown
    int no = d->boxCount + d->modelCount;
    bool changed;
    if (!any) changed = p->filterOn;
    else if (!DrawRangeObjects(s) || !MS_RESERVE(p->filterObjNew, p->filterObjNewCap, no > 0 ? no : 1)) {
        changed = true;
        p->filterObjCount = -1;
    } else {
        memset(p->filterObjNew, 0, (size_t)no);
        for (int r = 0; r < count; r++) {
            if (!hidden[r]) continue;
            int base = r < d->boxRangeCount ? 0 : d->boxCount;
            for (int k = p->rangeAt[r]; k < p->rangeAt[r + 1]; k++) p->filterObjNew[base + p->rangeObj[k]] = 1;
        }
        changed = !p->filterOn || p->filterObjCount != no || memcmp(p->filterObj, p->filterObjNew, (size_t)no) != 0;
        // keep what it hides for the next call (swap the buffers)
        unsigned char *t = p->filterObj;
        int tc = p->filterObjCap;
        p->filterObj = p->filterObjNew;
        p->filterObjCap = p->filterObjNewCap;
        p->filterObjNew = t;
        p->filterObjNewCap = tc;
        p->filterObjCount = no;
    }
    if (!any) p->filterObjCount = 0;
    if (any) memcpy(p->drawHidden, hidden, sizeof(bool) * (size_t)count);
    p->drawHiddenCount = any ? count : 0;
    if (changed) FilterInvalidate(p);
    p->filterOn = any;
}

// #endregion

// ======================================================================================
// #region Painting (docs/painting.md): surface picking, face layouts, textures
// ======================================================================================

static bool RaycastModelTri(const MS_Scene *s, int mi, Vector3 origin, Vector3 dir, float maxDist, MS_Hit *hit, int *tri,
                            Vector3 *bary);

// A face UV (0..1) in its cell of the layout, as the GPU interpolates it between the cube's corners:
// their UVs as FaceLayoutData writes them (half floats, R3D's vertex format), then linear
static Vector2 FaceCellUv(const unsigned char cells[6], int f, Vector2 fu)
{
    Rectangle r = FaceCellRect(cells, f, true);
    uint16_t h0[2], h1[2];
    R3D_PackTexCoord(h0, (Vector2){ r.x + 0.0f * r.width, r.y + 0.0f * r.height });
    R3D_PackTexCoord(h1, (Vector2){ r.x + 1.0f * r.width, r.y + 1.0f * r.height });
    Vector2 c0 = R3D_UnpackTexCoord(h0), c1 = R3D_UnpackTexCoord(h1);
    return (Vector2){ c0.x + fu.x * (c1.x - c0.x), c0.y + fu.y * (c1.y - c0.y) };
}

// The mesh of the file whose triangles (in file order) hold triangle `src`
static int MeshOfTriangle(const ModelPick *mp, int src)
{
    int lo = 0, hi = mp->meshCount - 1;
    while (lo < hi) {               // the last mesh whose first triangle is <= src
        int mid = (lo + hi + 1) / 2;
        if (mp->meshTri[mid] <= src) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

// A model's own UV as its paint mesh stores it (unorm16, PaintBones), so picking and drawing agree
static Vector2 Unorm16Uv(Vector2 t)
{
    float u = fminf(fmaxf(t.x, 0.0f), 1.0f), v = fminf(fmaxf(t.y, 0.0f), 1.0f);
    return (Vector2){ floorf(u * 65535.0f + 0.5f) / 65535.0f, floorf(v * 65535.0f + 0.5f) / 65535.0f };
}

// The active paint entry of a box face (face >= 0) or a model slot in `material`'s atlas (-1: the
// slot's own material), -1 for none (a linear search: entries are few)
static int PaintEntryOf(const SceneData *d, int kind, int object, int material, int face, int slot)
{
    for (int i = 0; i < d->paintCount; i++) {
        const PaintDef *e = &d->paints[i];
        if (e->active && e->kind == kind && e->object == object && e->material == material &&
            (kind == PAINT_BOX ? face >= 0 && (e->faces >> face & 1) : e->slot == slot)) return i;
    }
    return -1;
}

// A box hit: its face in its own frame, where on the face, the cell (uv_faces), the material's
// uv_scale / uv_offset
static void SurfaceOfBox(const SceneData *d, MS_SurfaceHit *out)
{
    int b = out->hit.obj.index;
    const MS_TurnedBox *T = TurnedOf(d, b);
    Vector3 local, nrm;
    if (T) {
        Vector3 rel = Vector3Subtract(out->hit.point, T->center);
        float l[3], n[3];
        for (int a = 0; a < 3; a++) {
            float size = ((const float *)&T->size)[a];
            l[a] = size > 0.0f ? Vector3DotProduct(rel, T->axis[a]) / size : 0.0f;
            n[a] = Vector3DotProduct(out->hit.normal, T->axis[a]);
        }
        local = (Vector3){ l[0], l[1], l[2] };
        nrm = (Vector3){ n[0], n[1], n[2] };
    } else {
        const BoundingBox *B = &d->boxes[b];
        Vector3 c = Vector3Scale(Vector3Add(B->min, B->max), 0.5f), size = Vector3Subtract(B->max, B->min);
        Vector3 rel = Vector3Subtract(out->hit.point, c);
        local = (Vector3){ size.x > 0.0f ? rel.x / size.x : 0.0f, size.y > 0.0f ? rel.y / size.y : 0.0f,
                           size.z > 0.0f ? rel.z / size.z : 0.0f };
        nrm = out->hit.normal;
    }
    int m = d->boxRefs[b].material;
    out->material = m;
    out->face = FaceOfNormal(nrm);
    Vector2 fu = CubeFaceUv(out->face, local);
    fu.x = fu.x < 0.0f ? 0.0f : fu.x > 1.0f ? 1.0f : fu.x;
    fu.y = fu.y < 0.0f ? 0.0f : fu.y > 1.0f ? 1.0f : fu.y;
    out->faceUv = fu;
    out->paintUv = fu;          // (the face corner UVs of the paint keys: CubeFaceUv's mapping)
    out->paintEntry = PaintEntryOf(d, PAINT_BOX, b, m, out->face, -1);
    Vector2 uv = fu;
    if (m >= 0 && m < d->materialCount) {
        const MatDef *D = &d->matDefs[m];
        const R3D_Material *M = &d->materials[m].material;
        if (D->hasFaces) uv = FaceCellUv(D->uvFaces, out->face, fu);
        uv = (Vector2){ M->uvOffset.x + uv.x * M->uvScale.x, M->uvOffset.y + uv.y * M->uvScale.y };
    }
    out->uv = uv;
}

// A model hit: the triangle the BVH hit, its mesh, slot and texture coordinates, the slot's material
static void SurfaceOfModel(const MS_Scene *s, Vector3 origin, Vector3 dir, float maxDist, MS_SurfaceHit *out)
{
    const SceneData *d = s->priv->d;
    int mi = out->hit.obj.index;
    const MS_ModelInstance *M = &d->models[mi];
    const ModelPick *mp = &d->modelPick[M->file];
    const R3D_Model *model = &d->modelFiles[M->file].model;
    MS_Hit h;
    int tri = -1;
    Vector3 bary = { 0 };
    // (the same ray and the model's nearest triangle: MS_Raycast took this model's)
    if (!RaycastModelTri(s, mi, origin, dir, maxDist, &h, &tri, &bary) || tri < 0 || !mp->triSrc) return;
    int src = (int)mp->triSrc[tri];
    out->mesh = MeshOfTriangle(mp, src);
    out->triangle = src - mp->meshTri[out->mesh];
    out->bary = bary;
    out->slot = model->meshMaterials && out->mesh < model->meshCount ? model->meshMaterials[out->mesh] : 0;
    Vector2 tc = { 0 };
    if (mp->uv) {
        const unsigned int *v = &mp->tri[3 * tri];
        Vector2 a = mp->uv[v[0]], b = mp->uv[v[1]], c = mp->uv[v[2]];
        tc = (Vector2){ bary.x * a.x + bary.y * b.x + bary.z * c.x, bary.x * a.y + bary.y * b.y + bary.z * c.y };
    }
    out->faceUv = tc;
    int m = out->slot >= 0 ? SlotMaterial(d, M, out->slot) : -1;
    out->material = m;
    // The paint coordinate: the triangle's paint UVs (own, as the paint mesh stores them, or the
    // unwrap's) at the hit; -1 -1 while the slot needs an unwrap
    int kind = d->filePaint ? FileUvKind(s->priv->d, M->file, out->slot) : PUV_NEEDS_UNWRAP;
    Vector2 q[3] = { { 0 } };
    bool any = false;
    if (kind == PUV_OWN && mp->uv) {
        const unsigned int *v = &mp->tri[3 * tri];
        for (int c = 0; c < 3; c++) q[c] = Unorm16Uv(mp->uv[v[c]]);
        any = true;
    } else if (kind == PUV_UNWRAP && out->mesh < d->filePaint[M->file].unwrap.meshCount) {
        const UnwrapMesh *U = &d->filePaint[M->file].unwrap.meshes[out->mesh];
        for (int c = 0; c < 3 && 3 * out->triangle + c < U->indexCount; c++) {
            unsigned int u = U->indices[3 * out->triangle + c];
            q[c] = (Vector2){ U->uv[2 * u] / 65535.0f, U->uv[2 * u + 1] / 65535.0f };
        }
        any = true;
    }
    if (any)
        out->paintUv = (Vector2){ bary.x * q[0].x + bary.y * q[1].x + bary.z * q[2].x,
                                  bary.x * q[0].y + bary.y * q[1].y + bary.z * q[2].y };
    out->paintEntry = PaintEntryOf(d, PAINT_MODEL, mi, m, -1, out->slot);
    // The uv_scale / uv_offset the slot is drawn with: the override's, else the file's own material's
    const R3D_Material *R = m >= 0 ? &d->materials[m].material
                          : out->slot >= 0 && out->slot < model->materialCount && model->materials ? &model->materials[out->slot]
                          : NULL;
    out->uv = R ? (Vector2){ R->uvOffset.x + tc.x * R->uvScale.x, R->uvOffset.y + tc.y * R->uvScale.y } : tc;
}

bool MS_RaycastSurface(const MS_Scene *s, Vector3 origin, Vector3 dir, float maxDist, MS_SurfaceHit *out)
{
    memset(out, 0, sizeof(*out));
    out->hit = MS_Raycast(s, origin, dir, maxDist);
    out->material = out->face = out->slot = out->mesh = out->triangle = -1;
    out->paintUv = (Vector2){ -1.0f, -1.0f };     // (no paint coordinate: SurfaceOf* fill it)
    out->paintEntry = -1;
    const SceneData *d = s->priv->d;
    if (out->hit.obj.kind == MS_OBJ_BOX) SurfaceOfBox(d, out);
    else if (out->hit.obj.kind == MS_OBJ_MODEL) SurfaceOfModel(s, origin, dir, maxDist, out);
    return out->hit.obj.kind != MS_OBJ_NONE;
}

int MS_ModelSlotTriangleCount(const MS_Scene *s, int file, int slot)
{
    if (!s || file < 0 || file >= s->modelFileCount) return 0;
    const SceneData *d = s->priv->d;
    const ModelPick *mp = &d->modelPick[file];
    const R3D_Model *model = &d->modelFiles[file].model;
    if (!mp->meshTri || !model->meshMaterials) return 0;
    int n = 0;
    for (int i = 0; i < mp->meshCount && i < model->meshCount; i++)
        if (model->meshMaterials[i] == slot) n += mp->meshTri[i + 1] - mp->meshTri[i];
    return n;
}

bool MS_ModelSlotTriangle(const MS_Scene *s, int file, int slot, int k, Vector2 uv[3], Vector3 pos[3])
{
    if (!s || file < 0 || file >= s->modelFileCount || k < 0) return false;
    const SceneData *d = s->priv->d;
    const ModelPick *mp = &d->modelPick[file];
    const R3D_Model *model = &d->modelFiles[file].model;
    if (!mp->uv || !mp->meshTri || !mp->triAt || !model->meshMaterials) return false;
    for (int i = 0; i < mp->meshCount && i < model->meshCount; i++) {
        if (model->meshMaterials[i] != slot) continue;
        int n = mp->meshTri[i + 1] - mp->meshTri[i];
        if (k >= n) {
            k -= n;
            continue;
        }
        const unsigned int *v = &mp->tri[3 * mp->triAt[mp->meshTri[i] + k]];
        for (int c = 0; c < 3; c++) {
            uv[c] = mp->uv[v[c]];
            pos[c] = mp->pos[v[c]];
        }
        return true;
    }
    return false;
}

int MS_ModelSlotTriangleIndex(const MS_Scene *s, int file, int mesh, int triangle)
{
    if (!s || file < 0 || file >= s->modelFileCount || mesh < 0 || triangle < 0) return -1;
    const SceneData *d = s->priv->d;
    const ModelPick *mp = &d->modelPick[file];
    const R3D_Model *model = &d->modelFiles[file].model;
    if (!mp->meshTri || !model->meshMaterials || mesh >= mp->meshCount || mesh >= model->meshCount) return -1;
    if (triangle >= mp->meshTri[mesh + 1] - mp->meshTri[mesh]) return -1;
    int slot = model->meshMaterials[mesh], k = triangle;
    for (int i = 0; i < mesh; i++)
        if (model->meshMaterials[i] == slot) k += mp->meshTri[i + 1] - mp->meshTri[i];
    return k;
}

// Forgets every provided image and unwrap (a load or reload from disk, MS_Unload)
static void FreeProvided(MS_Private *p)
{
    for (int i = 0; i < p->providedCount; i++) {
        free(p->provided[i].rel);
        UnloadImage(p->provided[i].img);
    }
    p->providedCount = 0;
    for (int i = 0; i < p->unwrapCount; i++) {
        free(p->unwraps[i].rel);
        UnwrapFree(&p->unwraps[i].u);
    }
    p->unwrapCount = 0;
}

bool MS_PaintAtlasProvide(MS_Scene *s, const char *rel, const Image *img)
{
    MS_Private *p = s->priv;
    if (!rel) return false;
    int i = 0;
    while (i < p->providedCount && strcmp(p->provided[i].rel, rel) != 0) i++;
    if (!img || !img->data || img->width <= 0 || img->height <= 0) {
        // forget it (the last entry takes its place)
        if (i < p->providedCount) {
            free(p->provided[i].rel);
            UnloadImage(p->provided[i].img);
            p->provided[i] = p->provided[--p->providedCount];
        }
        return true;
    }
    Image copy = ImageCopy(*img);
    if (copy.data && copy.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) ImageFormat(&copy, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    if (!copy.data || copy.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) {
        UnloadImage(copy);
        return false;
    }
    if (i < p->providedCount) {
        UnloadImage(p->provided[i].img);
        p->provided[i].img = copy;
        return true;
    }
    char *name = MS_StrDup(rel);
    ProvidedTex *e = name ? MS_PUSH(p->provided, p->providedCount, p->providedCap) : NULL;
    if (!e) {
        free(name);
        UnloadImage(copy);
        return false;
    }
    *e = (ProvidedTex){ name, copy };
    return true;
}

// Rows y0..y0+h of columns x0..x0+w of the RGBA8 image, in pixel format `format` (RGBA8, RGB8,
// grey, grey + alpha), into the scratch buffer; NULL for another format or out of memory
static const void *PackRect(MS_Private *p, const Image *img, int x0, int y0, int w, int h, int format)
{
    int bpp = format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 ? 4 : format == PIXELFORMAT_UNCOMPRESSED_R8G8B8 ? 3
            : format == PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA ? 2 : format == PIXELFORMAT_UNCOMPRESSED_GRAYSCALE ? 1 : 0;
    if (bpp == 0 || !MS_RESERVE(p->texScratch, p->texScratchCap, w * h * bpp)) return NULL;
    const unsigned char *src = img->data;
    unsigned char *dst = p->texScratch;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = src + ((size_t)(y0 + y) * (size_t)img->width + (size_t)x0) * 4;
        if (bpp == 4) {
            memcpy(dst, row, (size_t)w * 4);
            dst += (size_t)w * 4;
            continue;
        }
        for (int x = 0; x < w; x++, row += 4) {
            if (bpp >= 3) { *dst++ = row[0]; *dst++ = row[1]; *dst++ = row[2]; }
            else *dst++ = row[0];
            if (bpp == 2) *dst++ = row[3];
        }
    }
    return p->texScratch;
}

// After an update of the texture at full path `full`: the shadows of the objects of every material
// whose albedo map it is and that may discard or blend (alpha_cutoff > 0, transparency) can change,
// so the sun tiles and light shadows their bounds reach are drawn again (nothing else is re-stamped)
static void TextureShadowsInvalidate(MS_Private *p, const char *full)
{
    SceneData *d = p->d;
    size_t rootLen = strlen(d->root);
    if (strncmp(full, d->root, rootLen) != 0) return;
    const char *rel = full + rootLen;
    p->dirtyCount = 0;
    for (int m = 0; m < d->materialCount; m++) {
        const MatDef *D = &d->matDefs[m];
        const R3D_Material *M = &d->materials[m].material;
        if (!D->maps[MAP_ALBEDO] || strcmp(D->maps[MAP_ALBEDO], rel) != 0 ||
            (M->alphaCutoff <= 0.0f && M->transparencyMode == R3D_TRANSPARENCY_OPAQUE)) continue;
        BoundingBox u = { { INFINITY, INFINITY, INFINITY }, { -INFINITY, -INFINITY, -INFINITY } };
        for (int b = 0; b < d->boxCount; b++)
            if (d->boxRefs[b].material == m) u = BoundsUnion(u, d->boxes[b]);
        for (int i = 0; i < d->modelCount; i++) {
            const MS_ModelInstance *I = &d->models[i];
            bool uses = false;
            for (int sl = 0; sl < d->modelFiles[I->file].materialCount && !uses; sl++) uses = SlotMaterial(d, I, sl) == m;
            if (uses) u = BoundsUnion(u, I->bounds);
        }
        if (u.min.x <= u.max.x) DirtyAdd(p, u);
    }
    int nd = DirtyAll(p) ? 0 : p->dirtyCount;
    if (nd == 0 && !DirtyAll(p)) return;
#ifdef MS_PATCHED_R3D
    for (int i = 0; i < SUN_TILE_COUNT; i++)
        if (p->tiles.tile[i].valid && (DirtyAll(p) || DirtyHitsMap(p, 0, nd, p->tiles.tile[i].viewProj))) p->tiles.tile[i].valid = false;
#endif
    if (p->camCache.valid && (DirtyAll(p) || DirtyHitsMap(p, 0, nd, p->camCache.viewProj))) p->camCache.valid = false;
    for (int i = 0; d->lightShadowDrawn && i < d->lightCount; i++) {
        const R3D_Light *L = &d->lights[i].light;
        if (!d->lights[i].shadow || !d->lightShadowDrawn[i] || L->type == R3D_LIGHT_DIR) continue;
        if (DirtyAll(p) || DirtyHitsSphere(p, 0, nd, L->position, L->range)) d->lightShadowDrawn[i] = false;
    }
    p->dirtyCount = 0;
}

bool MS_TextureUpdate(MS_Scene *s, const char *rel, const Image *img, Rectangle rect)
{
    MS_Private *p = s->priv;
    SceneData *d = p->d;
    if (!d || !d->gpu || !rel || !img || !img->data || img->format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) return false;
    char full[MS_PATH_MAX + MS_PATH_MAX];
    snprintf(full, sizeof(full), "%s%s", d->root, rel);

    // The rectangle in whole texels, inside the image
    int x0 = (int)floorf(rect.x), y0 = (int)floorf(rect.y);
    int x1 = (int)ceilf(rect.x + rect.width), y1 = (int)ceilf(rect.y + rect.height);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > img->width) x1 = img->width;
    if (y1 > img->height) y1 = img->height;

    // Every cached texture of the path (one per wrap, filter, ...) of the image's size (a canvas
    // resized live leaves its old texture in the cache, unused), then the rows
    bool any = false;
    for (int i = 0; i < d->textureCount; i++) {
        const TexEntry *e = &d->textures[i];
        if (strcmp(e->path, full) == 0 && e->tex.width == img->width && e->tex.height == img->height) any = true;
    }
    if (!any) return false;
    for (int i = 0; x1 > x0 && y1 > y0 && i < d->textureCount; i++) {
        const TexEntry *e = &d->textures[i];
        if (strcmp(e->path, full) != 0 || e->tex.width != img->width || e->tex.height != img->height) continue;
        const void *rows = PackRect(p, img, x0, y0, x1 - x0, y1 - y0, e->tex.format);
        if (!rows) continue;
        // (a paint atlas is premultiplied on the GPU; the image stays straight alpha)
        if (e->premul && e->tex.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
            PremultiplyRgba8(p->texScratch, (size_t)(x1 - x0) * (size_t)(y1 - y0));
        rlUpdateTexture(e->tex.id, x0, y0, x1 - x0, y1 - y0, e->tex.format, rows);
        if (!e->data) {
            int mipmaps;
            rlGenTextureMipmaps(e->tex.id, e->tex.width, e->tex.height, e->tex.format, &mipmaps);
        }
    }

    // The provided copy follows, so a later upload from memory shows the same pixels
    for (int i = 0; i < p->providedCount; i++) {
        Image *P = &p->provided[i].img;
        if (strcmp(p->provided[i].rel, rel) != 0 || P->width != img->width || P->height != img->height) continue;
        for (int y = y0; y < y1; y++)
            memcpy((unsigned char *)P->data + ((size_t)y * (size_t)P->width + (size_t)x0) * 4,
                   (const unsigned char *)img->data + ((size_t)y * (size_t)img->width + (size_t)x0) * 4, (size_t)(x1 - x0) * 4);
    }
    p->textureGeneration++;
    TextureShadowsInvalidate(p, full);
    return true;
}

unsigned int MS_TextureGeneration(const MS_Scene *s)
{
    return s->priv->textureGeneration;
}

// --- The paint layer (docs/painting.md 3-9, 8): entries, atlases, model paint UVs, unwraps

int MS_PaintEntryCount(const MS_Scene *s)
{
    const SceneData *d = s && s->priv ? s->priv->d : NULL;
    return d && d->paintPub ? d->paintCount : 0;
}

const MS_PaintEntry *MS_PaintEntryAt(const MS_Scene *s, int i)
{
    const SceneData *d = s && s->priv ? s->priv->d : NULL;
    return d && d->paintPub && i >= 0 && i < d->paintCount ? &d->paintPub[i] : NULL;
}

bool MS_PaintAtlasOf(const MS_Scene *s, int material, int file, int slot, MS_PaintAtlas *out)
{
    memset(out, 0, sizeof(*out));
    MS_Private *p = s ? s->priv : NULL;
    SceneData *d = p ? p->d : NULL;
    if (!d) return false;
    const char *rel = NULL;
    if (material >= 0) {
        if (material >= d->materialCount) return false;
        rel = d->matDefs[material].paint;
        out->nearest = d->matDefs[material].paintFilter == TEX_NEAREST;
    } else {
        // A model slot's own material: its atlas's path is named after the file (also before any entry)
        if (file < 0 || file >= d->modelFileCount || slot < 0 || slot > MS_MAX_OBJECTS) return false;
        int a = OwnAtlasOf(d, file, slot);
        if (a < 0) return false;
        rel = d->ownAtlases[a].rel;
        out->nearest = d->ownAtlases[a].nearest;
    }
    out->rel = rel;
    if (!rel) return true;
    // The image's size now: the provided pixels (a new or grown atlas), else the file's
    const Image *prov = ProvidedImage(p, rel);
    if (prov) {
        out->width = prov->width;
        out->height = prov->height;
    } else {
        char full[MS_PATH_MAX + MS_PATH_MAX];
        snprintf(full, sizeof(full), "%s%s", d->root, rel);
        ImageFileSize(full, &out->width, &out->height);
    }
    return true;
}

MS_PaintUvKind MS_ModelPaintUv(const MS_Scene *s, int file, int slot)
{
    SceneData *d = s && s->priv ? s->priv->d : NULL;
    if (!d || file < 0 || file >= d->modelFileCount || !d->modelFiles[file].loaded || !d->filePaint) return MS_PAINTUV_INVALID;
    return (MS_PaintUvKind)FileUvKind(d, file, slot);
}

bool MS_ModelSlotPaintTriangle(const MS_Scene *s, int file, int slot, int k, Vector2 puv[3], Vector3 pos[3])
{
    SceneData *d = s && s->priv ? s->priv->d : NULL;
    if (!d || file < 0 || file >= d->modelFileCount || k < 0 || !d->filePaint) return false;
    const ModelPick *mp = &d->modelPick[file];
    const R3D_Model *model = &d->modelFiles[file].model;
    int kind = FileUvKind(d, file, slot);
    if (kind != PUV_OWN && kind != PUV_UNWRAP) return false;
    if (!mp->meshTri || !mp->triAt || !model->meshMaterials || (kind == PUV_OWN && !mp->uv)) return false;
    const FilePaint *FP = &d->filePaint[file];
    for (int i = 0; i < mp->meshCount && i < model->meshCount; i++) {
        if (model->meshMaterials[i] != slot) continue;
        int n = mp->meshTri[i + 1] - mp->meshTri[i];
        if (k >= n) {
            k -= n;
            continue;
        }
        const unsigned int *v = &mp->tri[3 * mp->triAt[mp->meshTri[i] + k]];
        for (int c = 0; c < 3; c++) {
            pos[c] = mp->pos[v[c]];
            if (kind == PUV_UNWRAP) {
                const UnwrapMesh *U = &FP->unwrap.meshes[i];
                unsigned int u = U->indices[3 * k + c];
                puv[c] = (Vector2){ U->uv[2 * u] / 65535.0f, U->uv[2 * u + 1] / 65535.0f };
            } else {
                puv[c] = Unorm16Uv(mp->uv[v[c]]);
            }
        }
        return true;
    }
    return false;
}

bool MS_ModelMeshData(const MS_Scene *s, int file, int mesh, R3D_MeshData *out)
{
    memset(out, 0, sizeof(*out));
    const SceneData *d = s && s->priv ? s->priv->d : NULL;
    if (!d || file < 0 || file >= d->modelFileCount || !d->modelFiles[file].loaded) return false;
    const R3D_Model *m = &d->modelFiles[file].model;
    if (!m->meshData || mesh < 0 || mesh >= m->meshCount || !m->meshData[mesh].vertices) return false;
    *out = m->meshData[mesh];
    return true;
}

// MS_PaintUnwrap and the runtime's Unwrap have the same fields: a copy either way (deep)
static bool UnwrapFromPublic(Unwrap *dst, const MS_PaintUnwrap *src)
{
    Unwrap view = { src->meshCount, NULL, src->fileHash };
    UnwrapMesh *meshes = calloc(src->meshCount > 0 ? (size_t)src->meshCount : 1, sizeof(UnwrapMesh));
    if (!meshes) return false;
    for (int m = 0; m < src->meshCount; m++) {
        const MS_PaintUnwrapMesh *M = &src->meshes[m];
        meshes[m] = (UnwrapMesh){ M->slot, M->vertexCount, M->indexCount, M->source, M->uv, M->indices };
    }
    view.meshes = meshes;
    bool ok = UnwrapCopy(dst, &view);
    free(meshes);
    return ok;
}

bool MS_PaintUnwrapProvide(MS_Scene *s, int file, const MS_PaintUnwrap *u)
{
    MS_Private *p = s->priv;
    const SceneData *d = p->d;
    if (!d || file < 0 || file >= d->modelFileCount) return false;
    const char *rel = d->modelFiles[file].path;
    int i = 0;
    while (i < p->unwrapCount && strcmp(p->unwraps[i].rel, rel) != 0) i++;
    if (!u) {
        // forget it (the last entry takes its place)
        if (i < p->unwrapCount) {
            free(p->unwraps[i].rel);
            UnwrapFree(&p->unwraps[i].u);
            p->unwraps[i] = p->unwraps[--p->unwrapCount];
        }
        return true;
    }
    Unwrap copy;
    if (!UnwrapFromPublic(&copy, u)) return false;
    if (i == p->unwrapCount) {
        char *name = MS_StrDup(rel);
        ProvidedUnwrap *e = name ? MS_PUSH(p->unwraps, p->unwrapCount, p->unwrapCap) : NULL;
        if (!e) {
            free(name);
            UnwrapFree(&copy);
            return false;
        }
        e->rel = name;
    } else UnwrapFree(&p->unwraps[i].u);
    p->unwraps[i].u = copy;
    p->unwraps[i].gen = ++p->unwrapGeneration;
    return true;
}

// Little-endian writes of the sidecar's fields
static bool PutLe(FILE *f, uint64_t v, int bytes)
{
    unsigned char b[8];
    for (int i = 0; i < bytes; i++) b[i] = (unsigned char)(v >> (8 * i));
    return fwrite(b, 1, (size_t)bytes, f) == (size_t)bytes;
}

bool MS_PaintUnwrapWrite(const char *path, const MS_PaintUnwrap *u)
{
    if (!path || !u || u->meshCount < 0) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(UNWRAP_MAGIC, 1, sizeof(UNWRAP_MAGIC), f) == sizeof(UNWRAP_MAGIC) &&
              PutLe(f, 1, 4) && PutLe(f, (uint64_t)u->meshCount, 4) && PutLe(f, u->fileHash, 8);
    for (int m = 0; ok && m < u->meshCount; m++) {
        const MS_PaintUnwrapMesh *M = &u->meshes[m];
        ok = M->vertexCount >= 0 && M->indexCount >= 0 &&
             PutLe(f, (uint64_t)M->slot, 4) && PutLe(f, (uint64_t)M->vertexCount, 4) && PutLe(f, (uint64_t)M->indexCount, 4);
        for (int k = 0; ok && k < M->vertexCount; k++)
            ok = PutLe(f, M->source[k], 4) && PutLe(f, M->uv[2 * k], 2) && PutLe(f, M->uv[2 * k + 1], 2);
        for (int k = 0; ok && k < M->indexCount; k++) ok = PutLe(f, M->indices[k], 4);
    }
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool MS_PaintUnwrapRead(const char *path, MS_PaintUnwrap *out)
{
    memset(out, 0, sizeof(*out));
    Unwrap u;
    if (!path || !UnwrapRead(path, &u)) return false;
    MS_PaintUnwrapMesh *meshes = calloc(u.meshCount > 0 ? (size_t)u.meshCount : 1, sizeof(MS_PaintUnwrapMesh));
    if (!meshes) {
        UnwrapFree(&u);
        return false;
    }
    // (the arrays move over)
    for (int m = 0; m < u.meshCount; m++) {
        const UnwrapMesh *M = &u.meshes[m];
        meshes[m] = (MS_PaintUnwrapMesh){ M->slot, M->vertexCount, M->indexCount, M->source, M->uv, M->indices };
    }
    *out = (MS_PaintUnwrap){ u.meshCount, meshes, u.fileHash };
    free(u.meshes);
    return true;
}

void MS_PaintUnwrapFree(MS_PaintUnwrap *u)
{
    if (!u) return;
    for (int m = 0; u->meshes && m < u->meshCount; m++) {
        free(u->meshes[m].source);
        free(u->meshes[m].uv);
        free(u->meshes[m].indices);
    }
    free(u->meshes);
    memset(u, 0, sizeof(*u));
}

// #endregion
#endif


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

// One draw per cell and material. Instanced ones are clusters: R3D tests a cluster against
// each view's frustum (the camera, the shadow maps) and skips its draw when outside
static inline void DrawBoxRange(const MS_Private *p, const SceneData *d, const DrawRange *R)
{
    const R3D_Material mat = d->materials[R->group].material;
    if (!d->matDefs[R->group].merge) {
        int c = d->matDefs[R->group].faceCube;     // (a face layout's cube, else R3D's)
        R3D_BeginCluster(R->bounds);
        R3D_DrawMeshInstancedEx(c > 0 ? d->faceCubes[c - 1].mesh : p->cube, mat, d->inst[R->group], R->first, R->count);
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

// Models: the same per (file, materials, shadow) group; they go into the sun's shadow tiles
// too, which are drawn once per map load like for the boxes
static inline void DrawModelRange(const SceneData *d, const DrawRange *R)
{
    const ModelGroup *G = &d->modelGroups[R->group];
    R3D_BeginCluster(R->bounds);
    R3D_DrawModelInstancedEx(G->model, G->inst, R->first, R->count);
    R3D_EndCluster();
}

#ifdef MONOLITH_EDITOR
// MS_DrawGeometry's draws without the ranges the draw filter hides (MS_SetDrawFilter)
static void DrawFiltered(MS_Private *p, const SceneData *d)
{
    int n = p->drawHiddenCount;
    for (int r = 0; r < d->boxRangeCount; r++)
        if (r >= n || !p->drawHidden[r]) DrawBoxRange(p, d, &d->boxRanges[r]);
    if (p->pv) PreviewDraw(p);
    for (int r = 0; r < d->modelRangeCount; r++)
        if (d->boxRangeCount + r >= n || !p->drawHidden[d->boxRangeCount + r]) DrawModelRange(d, &d->modelRanges[r]);
}
#endif

void MS_DrawGeometry(MS_Scene *s)
{
    MS_Private *p = s->priv;
    SceneData *d = p->d;
#ifdef MONOLITH_EDITOR
    // (the meshes the live edits since the last frame replaced: this frame draws their successors)
    for (int i = 0; i < p->retiredCount; i++) R3D_UnloadMesh(p->retired[i]);
    p->retiredCount = 0;
    if (p->filterOn) DrawFiltered(p, d);    // the render inspector's draw filter
    else {
#endif
    for (int r = 0; r < d->boxRangeCount; r++) DrawBoxRange(p, d, &d->boxRanges[r]);
#ifdef MONOLITH_EDITOR
    if (p->pv) PreviewDraw(p);      // the boxes of a transform preview
#endif
    for (int r = 0; r < d->modelRangeCount; r++) DrawModelRange(d, &d->modelRanges[r]);
#ifdef MONOLITH_EDITOR
    }
#endif
#ifdef MS_PATCHED_R3D
    // Occlusion volume: once per map load (CPU bake + upload; not a pass of this frame)
    if (p->rs.aoVolumeVoxel > 0.0f && p->aoVersion != s->version) {
#ifdef MONOLITH_EDITOR
        if (!BakeOcclusionVolume(p, d, p->rs.aoVolumeVoxel, p->rs.aoVolumeIntensity)) R3D_ClearWorldOcclusion();
#else
        if (!BakeOcclusionVolume(d, p->rs.aoVolumeVoxel, p->rs.aoVolumeIntensity)) R3D_ClearWorldOcclusion();
#endif
        p->aoVersion = s->version;
    }
    bool fogBake = SunOn(p) && d->env.sun.shadow && p->rs.sunShadow && p->rs.fogCacheVoxel > 0.0f && p->fogCacheVersion != s->version;
#ifdef MONOLITH_EDITOR
    // (the fog cache keeps every object: its bake waits while the draw filter hides some)
    if (p->filterOn) fogBake = false;
#endif
    if (fogBake) {
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
static bool RaycastModelTri(const MS_Scene *s, int mi, Vector3 origin, Vector3 dir, float maxDist, MS_Hit *hit, int *tri,
                            Vector3 *bary);

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
            float mn[3] = { b->min.x, b->min.y, b->min.z };
            float mx[3] = { b->max.x, b->max.y, b->max.z };
            const float *O = o, *D = d;
            // A turned box: the same slab test in its own frame (the ray's origin and direction
            // along its axes, the box +-size / 2 about its centre)
            const MS_TurnedBox *T = TurnedOf(sd, i);
            float to[3], td[3];
            if (T) {
                Vector3 rel = Vector3Subtract(origin, T->center);
                for (int a = 0; a < 3; a++) {
                    to[a] = Vector3DotProduct(rel, T->axis[a]);
                    td[a] = Vector3DotProduct(dir, T->axis[a]);
                    mx[a] = ((const float *)&T->size)[a] * 0.5f;
                    mn[a] = -mx[a];
                }
                O = to;
                D = td;
            }
            float tNear = -INFINITY, tFar = INFINITY;
            int axis = -1;
            bool miss = false;
            for (int a = 0; a < 3 && !miss; a++) {
                if (fabsf(D[a]) < 1e-12f) {
                    // Parallel to this slab: inside it or never
                    if (O[a] < mn[a] || O[a] > mx[a]) miss = true;
                    continue;
                }
                float t1 = (mn[a] - O[a]) / D[a], t2 = (mx[a] - O[a]) / D[a];
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
            n[axis] = D[axis] > 0.0f ? -1.0f : 1.0f;
            best.normal = T ? Vector3Scale(T->axis[axis], n[axis]) : (Vector3){ n[0], n[1], n[2] };
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
    return RaycastModelTri(s, mi, origin, dir, maxDist, hit, NULL, NULL);
}

// MS_RaycastModel, plus the triangle hit (*tri: its place in the file's ModelPick.tri[], -1 when
// the model has no triangles) and the barycentric coordinates of the hit in it (of its vertices
// 0, 1, 2); tri and bary may be NULL
static bool RaycastModelTri(const MS_Scene *s, int mi, Vector3 origin, Vector3 dir, float maxDist, MS_Hit *hit, int *tri,
                            Vector3 *bary)
{
    if (tri) *tri = -1;
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
    float best = limit, bestU = 0.0f, bestV = 0.0f;
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
            if (tt > 1e-5f && tt < best) { best = tt; bestTri = t; bestU = u; bestV = v; }
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
    if (tri) *tri = bestTri;
    if (bary) *bary = (Vector3){ 1.0f - bestU - bestV, bestU, bestV };
    return true;
}

static inline bool Overlaps(BoundingBox a, Vector3 mn, Vector3 mx)
{
    return a.min.x < mx.x && a.max.x > mn.x && a.min.y < mx.y && a.max.y > mn.y && a.min.z < mx.z && a.max.z > mn.z;
}

// Strict overlap of the AABB a with turned box t (separating axes: the world's three, the box's
// three and their nine cross products; touching is not an overlap)
static bool OverlapsTurned(BoundingBox a, const MS_TurnedBox *t)
{
    Vector3 ca = Vector3Scale(Vector3Add(a.min, a.max), 0.5f), ha = Vector3Scale(Vector3Subtract(a.max, a.min), 0.5f);
    Vector3 T = Vector3Subtract(t->center, ca), hb = Vector3Scale(t->size, 0.5f);
    const Vector3 E[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int k = 0; k < 15; k++) {
        Vector3 L = k < 3 ? E[k] : k < 6 ? t->axis[k - 3] : Vector3CrossProduct(E[(k - 6) / 3], t->axis[(k - 6) % 3]);
        if (k >= 6 && Vector3LengthSqr(L) < 1e-12f) continue;     // (parallel axes: no separating axis there)
        float ra = ha.x * fabsf(L.x) + ha.y * fabsf(L.y) + ha.z * fabsf(L.z);
        float rb = hb.x * fabsf(Vector3DotProduct(L, t->axis[0])) + hb.y * fabsf(Vector3DotProduct(L, t->axis[1])) +
                   hb.z * fabsf(Vector3DotProduct(L, t->axis[2]));
        if (fabsf(Vector3DotProduct(T, L)) >= ra + rb) return false;
    }
    return true;
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
            if ((first < 0 || i < first) && Overlaps(a, s->colliders[i].min, s->colliders[i].max) &&
                (i >= d->boxCount || !TurnedOf(d, i) || OverlapsTurned(a, TurnedOf(d, i)))) first = i;
        }
    }
    return first;
}

bool MS_Collides(const MS_Scene *s, BoundingBox a)
{
    return MS_FirstCollision(s, a) >= 0;
}

// The lowest and highest y of turned box t over the rectangle [x0, x1] x [z0, z1]: the extremes of
// the polytope "box inside the vertical prism over the rectangle", taken over its candidate vertices
// (the box's corners inside the prism, its 12 edges cut by the prism's 4 side planes, the prism's 4
// vertical edges through the box). False when the box isn't over the rectangle.
static bool TurnedSpanY(const MS_TurnedBox *t, float x0, float x1, float z0, float z1, float *lo, float *hi)
{
    Vector3 h = Vector3Scale(t->size, 0.5f), q[8];
    *lo = INFINITY;
    *hi = -INFINITY;
    for (int c = 0; c < 8; c++) {
        q[c] = TurnedPoint(t, c & 1 ? h.x : -h.x, c & 2 ? h.y : -h.y, c & 4 ? h.z : -h.z);
        if (q[c].x >= x0 && q[c].x <= x1 && q[c].z >= z0 && q[c].z <= z1) { *lo = fminf(*lo, q[c].y); *hi = fmaxf(*hi, q[c].y); }
    }
    // the edges (corners that differ in one bit) cut by the planes x = x0, x1 and z = z0, z1
    for (int c = 0; c < 8; c++)
        for (int bit = 1; bit < 8; bit <<= 1) {
            if (c & bit) continue;
            Vector3 a = q[c], b = q[c | bit];
            for (int pl = 0; pl < 4; pl++) {
                bool px = pl < 2;
                float v = px ? (pl == 0 ? x0 : x1) : (pl == 2 ? z0 : z1);
                float av = px ? a.x : a.z, bv = px ? b.x : b.z;
                if (av == bv || (av - v) * (bv - v) > 0.0f) continue;
                float f = (v - av) / (bv - av);
                float other = px ? a.z + (b.z - a.z) * f : a.x + (b.x - a.x) * f;
                if (px ? (other < z0 || other > z1) : (other < x0 || other > x1)) continue;
                float y = a.y + (b.y - a.y) * f;
                *lo = fminf(*lo, y);
                *hi = fmaxf(*hi, y);
            }
        }
    // the prism's vertical edges: the y interval where (X, y, Z) is inside the box on every axis
    for (int e = 0; e < 4; e++) {
        Vector3 r = { (e & 1 ? x1 : x0) - t->center.x, -t->center.y, (e & 2 ? z1 : z0) - t->center.z };
        float ylo = -INFINITY, yhi = INFINITY;
        for (int k = 0; k < 3 && ylo <= yhi; k++) {
            float q0 = Vector3DotProduct(r, t->axis[k]), ay = t->axis[k].y, hk = ((const float *)&h)[k];
            if (fabsf(ay) < 1e-12f) {
                if (fabsf(q0) > hk) ylo = INFINITY;
                continue;
            }
            float u = (-hk - q0) / ay, w = (hk - q0) / ay;
            ylo = fmaxf(ylo, fminf(u, w));
            yhi = fminf(yhi, fmaxf(u, w));
        }
        if (ylo <= yhi) { *lo = fminf(*lo, ylo); *hi = fmaxf(*hi, yhi); }
    }
    return *lo <= *hi;
}

float MS_ColliderTop(const MS_Scene *s, int c, BoundingBox box, bool *walkable)
{
    const SceneData *d = s->priv->d;
    if (walkable) *walkable = true;
    if (c < 0 || c >= s->colliderCount) return 0.0f;
    const MS_TurnedBox *t = c < d->boxCount ? TurnedOf(d, c) : NULL;
    if (!t) return s->colliders[c].max.y;
    if (walkable) {
        float up = fmaxf(fabsf(t->axis[0].y), fmaxf(fabsf(t->axis[1].y), fabsf(t->axis[2].y)));
        *walkable = up >= cosf(MS_WALKABLE_SLOPE_DEG * DEG2RAD);
    }
    float lo, hi;
    return TurnedSpanY(t, box.min.x, box.max.x, box.min.z, box.max.z, &lo, &hi) ? hi : s->colliders[c].max.y;
}

float MS_ColliderBottom(const MS_Scene *s, int c, BoundingBox box)
{
    const SceneData *d = s->priv->d;
    if (c < 0 || c >= s->colliderCount) return 0.0f;
    const MS_TurnedBox *t = c < d->boxCount ? TurnedOf(d, c) : NULL;
    float lo, hi;
    if (!t || !TurnedSpanY(t, box.min.x, box.max.x, box.min.z, box.max.z, &lo, &hi)) return s->colliders[c].min.y;
    return lo;
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
