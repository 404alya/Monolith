#include "shadow.h"

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

// ======================================================================================
// #region Static sun shadow tiles
// ======================================================================================

// Sun and boxes are static, so the shadow depth is a pure function of the map. Instead
// of R3D's camera-fitted map (which moves with every step and turn and had to be
// re-rendered for ~21% of the frames while turning, 1.2 ms + 0.37 ms min/max each),
// the sun's shadow comes from world-fixed tiles: orthographic maps on a fixed grid in
// light space, rendered once and then only sampled (R3D_PushLightFixedShadow, R3D
// patch 7). Which tile is used depends only on the eye position, so turning never
// re-renders anything, and the two tiles cached in R3D's two directional layers cover
// the whole default map's play area (every viewpoint and verification shot), so there
// a tile is drawn once per map load. Elsewhere (spectator far out) a new tile is drawn
// when the eye enters a new cell, i.e. every ~90 m of travel across the sun direction.
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
// GL_DEPTH_CLAMP (pancaking, see main()), so tall far-away casters still shadow.

#define SHADOW_EDGE_FADE    0.01f   // SHADOW_DIR_EDGE_FADE in r3d/shaders/include/wrap/light.glsl
#define TILE_FIT_ASPECT     (16.0f / 9.0f)
#define TILE_FIT_FOVY       70.0f   // the camera's fovy (PlayerCamera / SpectatorCamera)
#define TILE_Z_EXTENSION    100.0f  // R3D's zExtension: depth window = 2 * radius + this
#define TILE_HYSTERESIS     3.0f    // m: cells are this much smaller than what a tile covers

// The grid is laid out so this point sits on a cell boundary in light-space y and in
// the middle of a cell in x and z: then the eye positions of all 7 viewpoints and all
// verification shots (default map, SUN_DIR) fall into two cells, i.e. the two cached tiles.
#define TILE_HOME           (Vector3){ 10.0f, 38.0f, 2.0f }

// Half-extents (light-space x/y and z) of the eye positions a tile can serve
static void TileReach(float range, float *hxy, float *hz, float *radius, float *halfDepth)
{
    *radius = FitRadius(range, TILE_FIT_FOVY, TILE_FIT_ASPECT);
    *halfDepth = (2.0f * *radius + TILE_Z_EXTENSION) * 0.5f;
    *hxy = *radius * (1.0f - 2.0f * SHADOW_EDGE_FADE) - range;
    *hz = *halfDepth * (1.0f - 2.0f * SHADOW_EDGE_FADE) - range;
}

float SunShadowTileTexel(float range, int mapSize)
{
    return 2.0f * FitRadius(range, TILE_FIT_FOVY, TILE_FIT_ASPECT) / (float)mapSize;
}

int SunShadowTileSelect(SunShadowTiles *t, Vector3 sunDir, float range, int mapSize, Vector3 eye,
                        unsigned int version, bool *render)
{
    Vector3 dir = Vector3Normalize(sunDir), right, up;
    LightBasis(dir, &right, &up);
    float hxy, hz, radius, halfDepth;
    TileReach(range, &hxy, &hz, &radius, &halfDepth);
    float cellXY = 2.0f * (hxy - TILE_HYSTERESIS), cellZ = 2.0f * (hz - TILE_HYSTERESIS);

    // New map or settings: every tile is stale (keyed on world.version)
    if (t->version != version || t->range != range || t->mapSize != mapSize) {
        for (int i = 0; i < SUN_TILE_COUNT; i++) t->tile[i].valid = false;
        t->version = version;
        t->range = range;
        t->mapSize = mapSize;
        t->current = -1;
    }

    Vector3 ls = { Vector3DotProduct(eye, right), Vector3DotProduct(eye, up), Vector3DotProduct(eye, dir) };
    Vector3 home = TILE_HOME;
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

void SunShadowTileForget(SunShadowTiles *t, int slot)
{
    t->tile[slot].valid = false;
    if (t->current == slot) t->current = -1;
}

Matrix SunShadowBoxMatrix(Vector3 sunDir, BoundingBox region, float margin)
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

// #endregion

// ======================================================================================
// #region Camera-fitted sun shadow reuse (--shadow-camera)
// ======================================================================================

// The path before the static tiles, kept for A/B tests (--shadow-camera). Re-rendering
// the 8192 px sun shadow map is a large share of the frame, and the scene is static, so
// the map only has to be redrawn when the area it covers no longer matches the view. R3D keeps the light-space matrix next to the cached
// map (R3D_PushLightEx(..., updateShadow = false)), so a reused map is always
// sampled consistently; the only risk is coverage.
//
// Why a reused map is pixel-identical where it covers: R3D's fit
// (light_dir_view_proj in r3d/src/modules/r3d_light.c) uses a radius that
// depends only on range, fov and aspect, and snaps the centre to whole texels,
// so every fit lays the same world-space texel grid. Shadow results can only
// differ through R3D's fades: edgeFade (outer SHADOW_EDGE_FADE of the map, in x, y
// and depth; 5% before R3D patch 7) times distFade (distance from the eye 75%..100% of
// range; R3D patch 4, it used to be view depth). So the map is reused while, at every
// point of the view frustum within range, the cached map's fade is within
// SHADOW_REUSE_EPS of what a freshly fitted map would give (a fresh fit always has edgeFade = 1 there).
// That holds for the volumetric fog too: its march samples lie inside the same
// frustum and are faded by the distance of the surface behind them, never less.

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

// Decides whether the sun shadow map must be re-rendered this frame
bool SunShadowNeedsUpdate(SunShadowCache *c, Vector3 sunDir, float range, Camera3D cam, float aspect, int mapSize)
{
    Matrix fresh = SunShadowFit(sunDir, range, cam, aspect, mapSize);
    bool update = !c->valid || c->aspect != aspect || c->range != range;

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
                // R3D's distFade (by distance from the eye, see the region comment)
                float distFade = SmoothStep(range, range * 0.75f, Vector3Distance(p, cam.position));
                if (distFade <= 0.0f) continue;
                float loss = ShadowEdgeFade(fresh, p) - ShadowEdgeFade(c->viewProj, p);
                if (distFade * loss > SHADOW_REUSE_EPS) { update = true; break; }
            }
    }

    if (update) {
        *c = (SunShadowCache){ true, fresh, aspect, range };
    }
    return update;
}

// #endregion
