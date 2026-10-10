// Sun shadow: static world-fixed tiles (default), or R3D's camera-fitted map re-rendered
// only when the cached one no longer covers the view (--shadow-camera, for A/B tests)
#ifndef MONOLITH_SHADOW_H
#define MONOLITH_SHADOW_H

#include "common.h"

// --- Static tiles (default, see shadow.c)

#define SUN_TILE_COUNT 2    // = the 2 layers R3D allocates for directional shadows anyway

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
    unsigned int version;   // world.version the tiles were rendered for
    float range;            // settings the tiles were laid out for
    int mapSize;
    int renders;            // tile renders so far (log / bench)
} SunShadowTiles;

// Picks the tile that covers everything within `range` of `eye` and returns its index
// (= which of the SUN_TILE_COUNT shadow maps to push). *render is set when that tile
// must be (re)drawn this frame, with tiles->tile[index].viewProj. Depends only on the
// eye position, never on the view direction; tiles are dropped when `version` changes.
int SunShadowTileSelect(SunShadowTiles *t, Vector3 sunDir, float range, int mapSize, Vector3 eye,
                        unsigned int version, bool *render);

// World-space texel size of the tiles (for the log)
float SunShadowTileTexel(float range, int mapSize);

// Forget what tile slot `slot` holds (its shadow layer was used for something else)
void SunShadowTileForget(SunShadowTiles *t, int slot);

// Orthographic light-space matrix whose box covers `region` (plus `margin` m on every
// side; casters toward the sun are pancaked onto its near plane), for baking
Matrix SunShadowBoxMatrix(Vector3 sunDir, BoundingBox region, float margin);

// --- Camera-fitted map, reused while it still covers the view (--shadow-camera)

typedef struct {
    bool valid;
    Matrix viewProj;        // our copy of the light-space matrix R3D cached with the map
    float aspect;           // fit inputs that change the radius, i.e. the texel grid
    float range;
} SunShadowCache;

bool SunShadowNeedsUpdate(SunShadowCache *c, Vector3 sunDir, float range, Camera3D cam, float aspect, int mapSize);

#endif
