#include "designer.h"
#include "mapedit.h"
#include "ui.h"
#include <math.h>
#include <limits.h>

// Memory: the selection, regions and prompt live in the Designer (heap arrays, freed by
// DesignerFree); per-frame drawing and picking reuse file-level scratch arrays that only grow
// (to the scene's box / group count, the prompt's line count), so a steady frame allocates
// nothing. Region analysis and the request snapshot allocate at submit time only.

// ======================================================================================
// #region Picking
// ======================================================================================

// Slab test against the boxes list[0..n) (NULL: all boxes, n = scene->boxCount). With
// minDist (the list's boxes' distances from the origin, ascending) the loop stops at the
// first box that can't be nearer than the best hit. Equal distances go to the lowest box
// index, as in a plain loop over all boxes, so every list gives the same hit.
static DesignerHit PickBoxes(Vector3 origin, Vector3 dir, const int *list, const float *minDist, int n)
{
    DesignerHit best = { .obj = { MS_OBJ_NONE, -1 }, .dist = INFINITY };
    const float o[3] = { origin.x, origin.y, origin.z };
    const float d[3] = { dir.x, dir.y, dir.z };

    for (int k = 0; k < n; k++) {
        if (minDist && minDist[k] > best.dist) break;
        int i = list ? list[k] : k;
        const BoundingBox *b = &scene->boxes[i];
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
        // tNear < 0: the eye is inside this box. Skip it, so flying through a wall
        // still lets you pick what is beyond.
        if (miss || axis < 0 || tNear < 0.0f || tNear > best.dist) continue;
        if (tNear == best.dist && i > best.obj.index) continue;

        best.obj = (MS_ObjRef){ MS_OBJ_BOX, i };
        best.dist = tNear;
        best.point = Vector3Add(origin, Vector3Scale(dir, tNear));
        float n[3] = { 0, 0, 0 };
        n[axis] = d[axis] > 0.0f ? -1.0f : 1.0f;
        best.normal = (Vector3){ n[0], n[1], n[2] };
    }
    if (best.obj.kind == MS_OBJ_NONE) best.dist = 0.0f;
    return best;
}

// Models list[0..n) (NULL: all models) after the boxes: a model only wins when strictly
// nearer (as in MS_Raycast). With minDist (ascending distances to the models' bounds) the
// loop stops at the first model that can't be nearer.
static void PickModels(Vector3 origin, Vector3 dir, const int *list, const float *minDist, int n, DesignerHit *best)
{
    float bestDist = ObjIsNone(best->obj) ? INFINITY : best->dist;
    for (int k = 0; k < n; k++) {
        if (minDist && minDist[k] > bestDist) break;
        int i = list ? list[k] : k;
        MS_Hit h;
        if (!MS_RaycastModel(scene, i, origin, dir, isfinite(bestDist) ? bestDist : 0.0f, &h) || h.dist >= bestDist) continue;
        *best = (DesignerHit){ h.obj, h.point, h.normal, h.dist };
        bestDist = h.dist;
    }
}

// PickBoxes + PickModels over everything are MS_Raycast's rules; it goes through the scene's BVH
DesignerHit DesignerPick(Vector3 origin, Vector3 dir)
{
    MS_Hit h = MS_Raycast(scene, origin, dir, 0.0f);
    return (DesignerHit){ h.obj, h.point, h.normal, h.dist };
}

// Name of an object's material (the HUD, --pick and request bundles show it). Models
// without a whole-model override use the file's own materials ("(own)"), possibly with
// per-slot material.N overrides, which are listed: "(own; slot 0 = bark, slot 2 = needles)",
// or "(all slots overridden: slot 0 = wood, slot 1 = canvas)" when no slot of the file keeps
// its own material. The label lives in a buffer grown as needed and reused by the next call.
const char *DesignerMaterialLabel(MS_ObjRef o)
{
    const MS_Ref *r = MS_ObjectRef(scene, o);
    int m = r ? r->material : -1;
    if (m >= 0 && m < scene->materialCount) return scene->materials[m].name;
    if (o.kind != MS_OBJ_MODEL) return "default";
    const MS_ModelInstance *mi = &scene->models[o.index];
    if (mi->slotCount <= 0) return "(own)";
    const MS_SlotMaterial *slots = &scene->modelSlots[mi->slotFirst];
    int fileSlots = scene->modelFiles[mi->file].materialCount;
    bool all = fileSlots > 0;
    for (int s = 0; s < fileSlots && all; s++) {
        bool found = false;
        for (int i = 0; i < mi->slotCount && !found; i++) found = slots[i].slot == s;
        all = found;
    }
    static char *buf;
    static int cap;
    const char *head = all ? "(all slots overridden:" : "(own;";
    size_t need = strlen(head) + 2;
    for (int i = 0; i < mi->slotCount; i++) {
        int sm = slots[i].material;
        need += 32 + strlen(sm >= 0 && sm < scene->materialCount ? scene->materials[sm].name : "?");
    }
    if (need > (size_t)INT_MAX || !MS_RESERVE(buf, cap, (int)need)) return all ? "(all slots overridden)" : "(own; slot overrides)";
    int n = snprintf(buf, cap, "%s", head);
    for (int i = 0; i < mi->slotCount; i++) {
        int sm = slots[i].material;
        const char *name = sm >= 0 && sm < scene->materialCount ? scene->materials[sm].name : "?";
        n += snprintf(buf + n, cap - n, "%s slot %d = %s", i ? "," : "", slots[i].slot, name);
    }
    snprintf(buf + n, cap - n, ")");
    return buf;
}

void DesignerPrintHit(DesignerHit hit)
{
    const MS_Ref *rp = MS_ObjectRef(scene, hit.obj);
    if (!rp) {
        printf("pick: no hit\n");
        fflush(stdout);
        return;
    }
    MS_Ref r = *rp;
    BoundingBox b = MS_ObjectBounds(scene, hit.obj);
    int textLen = 0;
    const char *text = r.line > 0 ? MS_LineText(scene, r.line, &textLen) : NULL;
    if (!text) text = "";
    if (hit.obj.kind == MS_OBJ_MODEL) {
        const MS_ModelInstance *m = &scene->models[hit.obj.index];
        static const char *const ORIGIN[] = { "model", "bottom", "center" };
        printf("pick: model %d  file \"%s\"  group \"%s\"  material \"%s\"  line %d: %.*s\n"
               "pick: point %.3f %.3f %.3f  normal %.3f %.3f %.3f  dist %.3f\n"
               "pick: model position %.3f %.3f %.3f  rot %.3f %.3f %.3f  scale %.3f %.3f %.3f  origin %s  -> translation %.3f %.3f %.3f\n"
               "pick: model bounds min %.3f %.3f %.3f  max %.3f %.3f %.3f\n",
               hit.obj.index, scene->modelFiles[m->file].path, r.group >= 0 ? scene->groups[r.group] : "",
               DesignerMaterialLabel(hit.obj), r.line, textLen, text, hit.point.x, hit.point.y, hit.point.z, hit.normal.x, hit.normal.y,
               hit.normal.z, hit.dist, m->position.x, m->position.y, m->position.z, m->rotationDeg.x, m->rotationDeg.y,
               m->rotationDeg.z, m->scale.x, m->scale.y, m->scale.z, ORIGIN[m->origin % 3], m->translation.x,
               m->translation.y, m->translation.z, b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
    } else {
        printf("pick: box %d  group \"%s\"  material \"%s\"  line %d: %.*s\n"
               "pick: point %.3f %.3f %.3f  normal %.0f %.0f %.0f  dist %.3f\n"
               "pick: box min %.3f %.3f %.3f  max %.3f %.3f %.3f\n",
               hit.obj.index, r.group >= 0 ? scene->groups[r.group] : "", DesignerMaterialLabel(hit.obj), r.line, textLen, text,
               hit.point.x, hit.point.y, hit.point.z, hit.normal.x, hit.normal.y, hit.normal.z, hit.dist,
               b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
    }
    fflush(stdout);
}

// #endregion

// ======================================================================================
// #region Screen regions: what a region shows (ray sampling)
// ======================================================================================

static float BoxDistance(BoundingBox b, Vector3 p);     // drawing region

// The shape of the scene runtime's MS_Hit
typedef struct {
    MS_ObjRef obj;          // kind MS_OBJ_NONE = nothing hit
    Vector3 point, normal;
    float dist;
} RegionHit;

// The objects a region's rays can hit: the boxes that touch the region's frustum, nearest
// first (see PickBoxes). Without it a region costs 4096 rays x all boxes (~300 ms at 8192
// boxes); with it, rays stop at the first hit's distance and boxes outside are never tested.
typedef struct {
    int *list;
    float *minDist;         // distance from the eye to the box, slightly rounded down
    int count, listCap, minDistCap;
    int *models;            // the same for models (by their world bounds)
    float *modelDist;
    int modelCount, modelsCap, modelDistCap;
} RegionCull;

// Room for n boxes and m models
static bool CullReserve(RegionCull *c, int n, int m)
{
    return MS_RESERVE(c->list, c->listCap, n) && MS_RESERVE(c->minDist, c->minDistCap, n) &&
           MS_RESERVE(c->models, c->modelsCap, m) && MS_RESERVE(c->modelDist, c->modelDistCap, m);
}

static void CullFree(RegionCull *c)
{
    free(c->list);
    free(c->minDist);
    free(c->models);
    free(c->modelDist);
    memset(c, 0, sizeof(*c));
}

// The one place region sampling asks the scene what a ray hits: the same hit as
// DesignerPick (the crosshair), from the region's candidates only (boxes, then models).
static RegionHit RegionPick(const RegionCull *c, Vector3 origin, Vector3 dir)
{
    DesignerHit h = PickBoxes(origin, dir, c->list, c->minDist, c->count);
    PickModels(origin, dir, c->models, c->modelDist, c->modelCount, &h);
    RegionHit r = { .obj = h.obj, .point = h.point, .normal = h.normal, .dist = h.dist };
    return r;
}

// Per box / model index, for CompareCull (qsort has no context); grown to the scene's counts
static float *boxDist, *modelDist;
static int boxDistCap, modelDistCap;
static int CompareCull(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    if (boxDist[ia] != boxDist[ib]) return boxDist[ia] < boxDist[ib] ? -1 : 1;
    return ia - ib;
}
static int CompareModelCull(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    if (modelDist[ia] != modelDist[ib]) return modelDist[ia] < modelDist[ib] ? -1 : 1;
    return ia - ib;
}

// Rays through a w x h frame of camera `cam`, built like raylib's perspective projection
// (MatrixLookAt + fovy, aspect w / h), so the ray through (x, y) (continuous pixel
// coordinates, top-left origin, pixel centres at i + 0.5) passes through what that pixel
// shows. The frame centre gives exactly the crosshair ray of DesignerPickCamera.
typedef struct { Vector3 eye, fwd, right, up; float tanX, tanY; int w, h; } RayFrame;

static RayFrame MakeRayFrame(Camera3D cam, int w, int h)
{
    RayFrame f = { .eye = cam.position, .w = w, .h = h };
    f.fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    f.right = Vector3Normalize(Vector3CrossProduct(f.fwd, cam.up));
    f.up = Vector3CrossProduct(f.right, f.fwd);
    f.tanY = tanf(cam.fovy * DEG2RAD * 0.5f);
    f.tanX = f.tanY * (float)w / (float)h;
    return f;
}

static Vector3 RayThrough(const RayFrame *f, float x, float y)
{
    if (x * 2.0f == (float)f->w && y * 2.0f == (float)f->h) return f->fwd;    // the crosshair ray, bit for bit
    float nx = 2.0f * x / f->w - 1.0f, ny = 1.0f - 2.0f * y / f->h;
    Vector3 d = Vector3Add(f->fwd, Vector3Add(Vector3Scale(f->right, nx * f->tanX), Vector3Scale(f->up, ny * f->tanY)));
    return Vector3Normalize(d);
}

// True if the box lies entirely outside one of the 4 planes (normals n, through the eye)
static bool OutsidePlanes(const Vector3 *n, Vector3 eye, BoundingBox bb)
{
    for (int i = 0; i < 4; i++) {
        // The box corner furthest along the plane normal
        Vector3 p = { n[i].x > 0 ? bb.max.x : bb.min.x, n[i].y > 0 ? bb.max.y : bb.min.y, n[i].z > 0 ? bb.max.z : bb.min.z };
        if (Vector3DotProduct(n[i], Vector3Subtract(p, eye)) < -1e-3f) return true;     // 1 mm of slack
    }
    return false;
}

// Candidates of a rect (pixels): boxes (and models) not entirely outside one of the four planes through
// the eye and the rect's edges (every ray through the rect lies inside them), nearest first.
// in = NULL: from all boxes; else filtered from in's list (already sorted). False when out of memory.
static bool BuildRegionCull(const RayFrame *f, Rectangle rect, const RegionCull *in, RegionCull *c)
{
    c->count = c->modelCount = 0;
    if (!CullReserve(c, in ? in->count : scene->boxCount, in ? in->modelCount : scene->modelCount)) return false;
    if (!in && (!MS_RESERVE(boxDist, boxDistCap, scene->boxCount) || !MS_RESERVE(modelDist, modelDistCap, scene->modelCount)))
        return false;
    float x0 = rect.x, y0 = rect.y, x1 = rect.x + rect.width, y1 = rect.y + rect.height;
    Vector3 corner[4] = { RayThrough(f, x0, y0), RayThrough(f, x1, y0), RayThrough(f, x1, y1), RayThrough(f, x0, y1) };
    Vector3 mid = RayThrough(f, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
    Vector3 n[4];
    for (int i = 0; i < 4; i++) {
        n[i] = Vector3Normalize(Vector3CrossProduct(corner[i], corner[(i + 1) % 4]));
        if (Vector3DotProduct(n[i], mid) < 0.0f) n[i] = Vector3Negate(n[i]);    // pointing inside
    }
    // Models first (by their world bounds; the same test as for boxes)
    c->modelCount = 0;
    int mcount = in ? in->modelCount : scene->modelCount;
    for (int k = 0; k < mcount; k++) {
        int m = in ? in->models[k] : k;
        if (OutsidePlanes(n, f->eye, scene->models[m].bounds)) continue;
        if (in) c->modelDist[c->modelCount] = in->modelDist[k];
        else modelDist[m] = BoxDistance(scene->models[m].bounds, f->eye) * (1.0f - 1e-5f) - 1e-4f;
        c->models[c->modelCount++] = m;
    }
    if (!in) {
        if (c->modelCount > 1) qsort(c->models, c->modelCount, sizeof(c->models[0]), CompareModelCull);
        for (int k = 0; k < c->modelCount; k++) c->modelDist[k] = modelDist[c->models[k]];
    }

    c->count = 0;
    int count = in ? in->count : scene->boxCount;
    for (int k = 0; k < count; k++) {
        int b = in ? in->list[k] : k;
        BoundingBox bb = scene->boxes[b];
        if (OutsidePlanes(n, f->eye, bb)) continue;
        if (in) {
            c->minDist[c->count] = in->minDist[k];
            c->list[c->count++] = b;
            continue;
        }
        // Distance to the box, rounded down a little so float noise can't stop a ray early
        boxDist[b] = BoxDistance(bb, f->eye) * (1.0f - 1e-5f) - 1e-4f;
        c->list[c->count++] = b;
    }
    if (in) return true;
    if (c->count > 1) qsort(c->list, c->count, sizeof(c->list[0]), CompareCull);
    for (int k = 0; k < c->count; k++) c->minDist[k] = boxDist[c->list[k]];
    return true;
}

#define REGION_TILE 8   // rays per tile side in DesignerAnalyzeRegion

// Samples per object: open addressing over (kind, index), size = 2 x the region's samples
// (so at most half full: a region can't hit more distinct objects than it has samples)
typedef struct { MS_ObjRef obj; int samples; } TallySlot;

static void TallyAdd(TallySlot *tally, unsigned int size, MS_ObjRef obj)
{
    unsigned int h = ((unsigned int)obj.index * 2654435761u) ^ (unsigned int)obj.kind;
    for (unsigned int i = h % size; ; i = (i + 1) % size) {
        if (tally[i].samples == 0) { tally[i].obj = obj; tally[i].samples = 1; return; }
        if (tally[i].obj.kind == obj.kind && tally[i].obj.index == obj.index) { tally[i].samples++; return; }
    }
}

static int CompareRegionObjects(const void *pa, const void *pb)
{
    const DesignRegionObject *a = pa, *b = pb;
    if (a->samples != b->samples) return b->samples - a->samples;
    if (a->obj.kind != b->obj.kind) return (int)a->obj.kind - (int)b->obj.kind;
    return a->obj.index - b->obj.index;
}

static int CompareRegionEdges(const void *pa, const void *pb)
{
    const DesignRegionEdge *a = pa, *b = pb;
    if (a->dist != b->dist) return a->dist < b->dist ? -1 : 1;
    if (a->obj.kind != b->obj.kind) return (int)a->obj.kind - (int)b->obj.kind;
    return a->obj.index - b->obj.index;
}

#define REGION_EDGE_STEP 8.0f   // px between edge rays along a side
#define REGION_EDGE_MAX 64      // edge rays per side at most

// The ring of rays just outside the rect: which objects bound it, nearest first. A region
// dragged over an opening ("fill this gap") shows what lies behind the opening; the opening
// itself is bounded by these. False when out of memory.
static bool AnalyzeRegionEdge(const RayFrame *f, Rectangle rect, DesignRegionInfo *out)
{
    float off = fmaxf(4.0f, 0.03f * fminf(rect.width, rect.height));
    out->edgeOffset = off;
    float x0 = rect.x - off, y0 = rect.y - off, x1 = rect.x + rect.width + off, y1 = rect.y + rect.height + off;
    RegionCull cull = { 0 };
    DesignRegionEdge *e = NULL;
    int n = 0, cap = 0, sky = 0, total = 0;
    // The ring's bounding rect, clamped to the frame (rays outside the frame are not cast)
    Rectangle ring = { fmaxf(x0, 0.0f), fmaxf(y0, 0.0f), 0, 0 };
    ring.width = fminf(x1, (float)f->w) - ring.x;
    ring.height = fminf(y1, (float)f->h) - ring.y;
    if (ring.width <= 0 || ring.height <= 0) return true;
    if (!BuildRegionCull(f, ring, NULL, &cull)) return false;
    for (int side = 0; side < 4; side++) {
        bool vertical = side < 2;   // left, right: a column of rays; top, bottom: a row
        float fixed = side == 0 ? x0 : side == 1 ? x1 : side == 2 ? y0 : y1;
        if (fixed < 0.0f || fixed >= (vertical ? (float)f->w : (float)f->h)) continue;   // outside the frame
        float a = vertical ? y0 : x0, b = vertical ? y1 : x1;
        int count = (int)fminf((float)REGION_EDGE_MAX, fmaxf(2.0f, ceilf((b - a) / REGION_EDGE_STEP)));
        for (int k = 0; k < count; k++) {
            float t = a + (b - a) * (k + 0.5f) / count;
            float x = vertical ? fixed : t, y = vertical ? t : fixed;
            if (x < 0.0f || y < 0.0f || x >= (float)f->w || y >= (float)f->h) continue;
            total++;
            RegionHit r = RegionPick(&cull, f->eye, RayThrough(f, x, y));
            if (r.obj.kind == MS_OBJ_NONE) { sky++; continue; }
            int j = 0;
            while (j < n && !ObjEqual(e[j].obj, r.obj)) j++;
            if (j == n) {
                if (!MS_RESERVE(e, cap, n + 1)) { free(e); CullFree(&cull); return false; }
                e[n++] = (DesignRegionEdge){ r.obj, 0, 0, INFINITY, r.point };
            }
            e[j].samples++;
            e[j].sides |= 1u << side;
            if (r.dist < e[j].dist) { e[j].dist = r.dist; e[j].point = r.point; }
        }
    }
    CullFree(&cull);
    if (n > 1) qsort(e, n, sizeof(e[0]), CompareRegionEdges);
    out->edges = e;
    out->edgeCount = n;
    out->edgeSamples = total;
    out->edgeSky = total > 0 ? (float)sky / total : 0.0f;
    return true;
}

void DesignerFreeRegion(DesignRegionInfo *r)
{
    free(r->objects);
    free(r->edges);
    r->objects = NULL;
    r->edges = NULL;
    r->objectCount = r->edgeCount = 0;
}

void DesignerAnalyzeRegion(Camera3D cam, int w, int h, Rectangle rect, DesignRegionInfo *out)
{
    double t0 = GetTime();
    memset(out, 0, sizeof(*out));
    out->rect = rect;
    out->centerObj = (MS_ObjRef){ MS_OBJ_NONE, -1 };
    int rw = (int)rect.width, rh = (int)rect.height;
    if (rw <= 0 || rh <= 0 || w <= 0 || h <= 0) return;
    RayFrame f = MakeRayFrame(cam, w, h);
    RegionCull cull = { 0 }, tileCull = { 0 };
    TallySlot *tally = NULL;
    if (!BuildRegionCull(&f, rect, NULL, &cull)) goto oom;

    RegionHit c = RegionPick(&cull, f.eye, RayThrough(&f, rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f));
    out->centerObj = c.obj;
    out->centerPoint = c.point;
    out->centerNormal = c.normal;
    out->centerDist = c.dist;

    // Grid: every pixel centre while that is at most REGION_SAMPLES rays, else a
    // regular grid of about square cells, sampled at the cell centres
    int gx = rw, gy = rh;
    if ((long long)rw * rh > REGION_SAMPLES) {
        float cell = sqrtf((float)rw * rh / REGION_SAMPLES);
        gx = (int)fmaxf(1.0f, floorf(rw / cell));
        gy = (int)fmaxf(1.0f, floorf(rh / cell));
        while (gx * gy > REGION_SAMPLES) { if (gx > gy) gx--; else gy--; }
    }
    out->gridW = gx;
    out->gridH = gy;

    int sky = 0, total = gx * gy;
    unsigned int tallySize = 2u * (unsigned int)total;
    if (!(tally = calloc(tallySize, sizeof(TallySlot)))) goto oom;
    out->depthMin = INFINITY;
    out->depthMax = 0.0f;
    // Tiles of REGION_TILE x REGION_TILE rays, each with the region's candidates that touch
    // it: rays that hit nothing (sky) otherwise test every box in the region's frustum
    const int T = REGION_TILE;
    float cellW = rect.width / gx, cellH = rect.height / gy;
    for (int tj = 0; tj < gy; tj += T) {
        for (int ti = 0; ti < gx; ti += T) {
            int i1 = ti + T < gx ? ti + T : gx, j1 = tj + T < gy ? tj + T : gy;
            if (!BuildRegionCull(&f, (Rectangle){ rect.x + ti * cellW, rect.y + tj * cellH, (i1 - ti) * cellW, (j1 - tj) * cellH },
                                 &cull, &tileCull)) goto oom;
            for (int j = tj; j < j1; j++) {
                float y = rect.y + (j + 0.5f) * cellH;
                for (int i = ti; i < i1; i++) {
                    float x = rect.x + (i + 0.5f) * cellW;
                    RegionHit r = RegionPick(&tileCull, f.eye, RayThrough(&f, x, y));
                    if (r.obj.kind == MS_OBJ_NONE) { sky++; continue; }
                    TallyAdd(tally, tallySize, r.obj);
                    out->depthMin = fminf(out->depthMin, r.dist);
                    out->depthMax = fmaxf(out->depthMax, r.dist);
                }
            }
        }
    }
    out->hasDepth = sky < total;
    if (!out->hasDepth) out->depthMin = out->depthMax = 0.0f;
    out->sky = (float)sky / total;

    int n = 0;
    for (unsigned int i = 0; i < tallySize; i++) n += tally[i].samples > 0;
    if (n > 0 && !(out->objects = malloc(sizeof(DesignRegionObject) * n))) goto oom;
    n = 0;
    for (unsigned int i = 0; i < tallySize; i++)
        if (tally[i].samples > 0) out->objects[n++] = (DesignRegionObject){ tally[i].obj, tally[i].samples, (float)tally[i].samples / total };
    if (n > 1) qsort(out->objects, n, sizeof(out->objects[0]), CompareRegionObjects);
    out->objectCount = n;
    if (!AnalyzeRegionEdge(&f, rect, out)) goto oom;
    out->ms = (GetTime() - t0) * 1000.0;
    free(tally);
    CullFree(&cull);
    CullFree(&tileCull);
    return;
oom:
    TraceLog(LOG_WARNING, "monolith: region analysis: out of memory");
    free(tally);
    CullFree(&cull);
    CullFree(&tileCull);
    out->objectCount = 0;
}

// #endregion

// ======================================================================================
// #region Volumes: what a volume holds
// ======================================================================================

void DesignerFreeVolume(DesignVolumeInfo *v)
{
    free(v->objects);
    v->objects = NULL;
    v->objectCount = 0;
}

static int CompareVolumeObject(const void *pa, const void *pb)
{
    const DesignVolumeObject *a = pa, *b = pb;
    if (a->shareOfVolume != b->shareOfVolume) return a->shareOfVolume > b->shareOfVolume ? -1 : 1;
    if (a->obj.kind != b->obj.kind) return (int)a->obj.kind - (int)b->obj.kind;
    return a->obj.index - b->obj.index;
}

// Grid cells [*lo, *hi] of `n` cells over [min, min + n * step] that [a, b] overlaps
static void CellRange(float a, float b, float min, float step, int n, int *lo, int *hi)
{
    *lo = (int)floorf((a - min) / step);
    *hi = (int)ceilf((b - min) / step) - 1;
    if (*lo < 0) *lo = 0;
    if (*hi > n - 1) *hi = n - 1;
}

// Share of cell i (of `step` from `min`) that [a, b] covers; a flat [a, b] inside covers it all
static float CellCover(float a, float b, float min, float step, int i)
{
    float c0 = min + i * step, c1 = c0 + step;
    if (b <= a) return a >= c0 && a <= c1 ? 1.0f : 0.0f;
    return fmaxf(fminf(b, c1) - fmaxf(a, c0), 0.0f) / step;
}

// Objects: every box and model whose world bounds overlap the volume (a positive overlap on
// every axis; a flat object only needs to lie within the volume on its flat axis). Empty
// share: a grid of at most VOLUME_SAMPLES cells; each cell sums the exact overlap of every
// object's bounds with it, capped at the whole cell (models count by their AABB and objects
// that overlap each other twice, so it errs toward solid; testing only cell centres was off
// by 5 points for 2.2 m pillars in 2.3 m cells). Floor: a ray straight down
// from the centre (DesignerPick: boxes that contain the centre are skipped).
void DesignerAnalyzeVolume(BoundingBox v, float snap, DesignVolumeInfo *out)
{
    double t0 = GetTime();
    memset(out, 0, sizeof(*out));
    out->box = v;
    out->snap = snap;
    out->floor.obj = (MS_ObjRef){ MS_OBJ_NONE, -1 };
    const float vmn[3] = { v.min.x, v.min.y, v.min.z }, vmx[3] = { v.max.x, v.max.y, v.max.z };
    double vol = (double)(vmx[0] - vmn[0]) * (vmx[1] - vmn[1]) * (vmx[2] - vmn[2]);

    int cap = 0;
    for (int i = 0; i < scene->boxCount + scene->modelCount; i++) {
        MS_ObjRef o = i < scene->boxCount ? (MS_ObjRef){ MS_OBJ_BOX, i } : (MS_ObjRef){ MS_OBJ_MODEL, i - scene->boxCount };
        BoundingBox b = MS_ObjectBounds(scene, o);
        const float bmn[3] = { b.min.x, b.min.y, b.min.z }, bmx[3] = { b.max.x, b.max.y, b.max.z };
        double inVol = 1.0, ofObj = 1.0;
        bool overlap = true, inside = true;
        for (int a = 0; a < 3 && overlap; a++) {
            float lo = fmaxf(vmn[a], bmn[a]), hi = fminf(vmx[a], bmx[a]);
            float size = bmx[a] - bmn[a];
            overlap = size > 0.0f ? hi > lo : hi >= lo;
            inVol *= hi - lo;
            if (size > 0.0f) ofObj *= (hi - lo) / size;
            inside = inside && bmn[a] >= vmn[a] - 1e-4f && bmx[a] <= vmx[a] + 1e-4f;
        }
        if (!overlap) continue;
        DesignVolumeObject *slot = MS_PUSH(out->objects, out->objectCount, cap);
        if (!slot) break;   // out of memory: the list ends here
        *slot = (DesignVolumeObject){ o, vol > 0.0 ? (float)(inVol / vol) : 0.0f, (float)ofObj, inside };
    }
    if (out->objectCount > 1) qsort(out->objects, out->objectCount, sizeof(out->objects[0]), CompareVolumeObject);

    // Grid: cubic cells of the size that gives VOLUME_SAMPLES cells, fewer when an axis is thin
    float cell = cbrtf((float)(vol / VOLUME_SAMPLES));
    int total = 1;
    for (int a = 0; a < 3; a++) {
        float n = cell > 0.0f ? floorf((vmx[a] - vmn[a]) / cell) : 1.0f;
        out->grid[a] = n < 1.0f ? 1 : n > VOLUME_SAMPLES ? VOLUME_SAMPLES : (int)n;
        total *= out->grid[a];
    }
    while (total > VOLUME_SAMPLES) {     // rounding: drop a layer off the axis with the most cells
        int a = out->grid[0] >= out->grid[1] && out->grid[0] >= out->grid[2] ? 0 : out->grid[1] >= out->grid[2] ? 1 : 2;
        total = total / out->grid[a] * (out->grid[a] - 1);
        out->grid[a]--;
    }
    float *solid = calloc(total, sizeof(float));
    if (solid) {
        float step[3];
        for (int a = 0; a < 3; a++) step[a] = (vmx[a] - vmn[a]) / out->grid[a];
        for (int k = 0; k < out->objectCount; k++) {
            BoundingBox b = MS_ObjectBounds(scene, out->objects[k].obj);
            const float bmn[3] = { b.min.x, b.min.y, b.min.z }, bmx[3] = { b.max.x, b.max.y, b.max.z };
            int lo[3], hi[3];
            for (int a = 0; a < 3; a++) CellRange(bmn[a], bmx[a], vmn[a], step[a], out->grid[a], &lo[a], &hi[a]);
            for (int z = lo[2]; z <= hi[2]; z++) {
                float cz = CellCover(bmn[2], bmx[2], vmn[2], step[2], z);
                for (int y = lo[1]; y <= hi[1]; y++) {
                    float cyz = cz * CellCover(bmn[1], bmx[1], vmn[1], step[1], y);
                    for (int x = lo[0]; x <= hi[0]; x++)
                        solid[(z * out->grid[1] + y) * out->grid[0] + x] += cyz * CellCover(bmn[0], bmx[0], vmn[0], step[0], x);
                }
            }
        }
        double filled = 0.0;
        for (int i = 0; i < total; i++) filled += fminf(solid[i], 1.0f);
        out->empty = (float)(1.0 - filled / total);
        free(solid);
    } else out->empty = NAN;    // out of memory: unknown ("null" in the bundle)

    Vector3 c = Vector3Scale(Vector3Add(v.min, v.max), 0.5f);
    out->floor = DesignerPick(c, (Vector3){ 0.0f, -1.0f, 0.0f });
    out->ms = (GetTime() - t0) * 1000.0;
}

// #endregion

// ======================================================================================
// #region Selection
// ======================================================================================

void DesignerMessage(Designer *d, bool error, const char *text)
{
    snprintf(d->message, sizeof(d->message), "%s", text);
    d->messageTime = error ? 8.0f : 4.0f;
    d->messageError = error;
}

// selected[] position + 1 of an object, 0 = not selected (d->selPos, grown on demand)
static int SelectionIndexOf(const Designer *d, MS_ObjRef o)
{
    if ((o.kind != MS_OBJ_BOX && o.kind != MS_OBJ_MODEL) || o.index < 0 || o.index >= d->selPosCap[o.kind]) return -1;
    return d->selPos[o.kind][o.index] - 1;
}

static bool SetSelPos(Designer *d, MS_ObjRef o, int pos)
{
    if (o.kind != MS_OBJ_BOX && o.kind != MS_OBJ_MODEL) return false;
    if (pos == 0 && o.index >= d->selPosCap[o.kind]) return true;
    if (!MS_RESERVE(d->selPos[o.kind], d->selPosCap[o.kind], o.index + 1)) return false;
    d->selPos[o.kind][o.index] = pos;
    return true;
}

// Objects of one kind: count, map line, group
static int KindCount(MS_ObjKind k) { return k == MS_OBJ_MODEL ? scene->modelCount : scene->boxCount; }
static const MS_Ref *KindRef(MS_ObjKind k, int i) { return k == MS_OBJ_MODEL ? &scene->models[i].ref : &scene->boxRefs[i]; }

int DesignerObjNth(MS_ObjRef o)
{
    const MS_Ref *r = MS_ObjectRef(scene, o);
    if (!r) return 0;
    // Consecutive calls along one line (selecting a whole `repeat` line) count on from the last
    static struct { unsigned int version; MS_ObjKind kind; int line, index, nth; } last;
    int nth = 0;
    if (last.version == scene->version && last.kind == o.kind && last.line == r->line && last.index == o.index - 1)
        nth = last.nth + 1;
    else
        for (int i = o.index - 1; i >= 0 && KindRef(o.kind, i)->line == r->line; i--) nth++;
    last.version = scene->version;
    last.kind = o.kind;
    last.line = r->line;
    last.index = o.index;
    last.nth = nth;
    return nth;
}

static DesignerSelKey MakeKey(MS_ObjRef o)
{
    const MS_Ref *r = KindRef(o.kind, o.index);
    DesignerSelKey k = { .kind = o.kind, .box = MS_ObjectBounds(scene, o), .line = r->line, .group = r->group };
    k.nth = DesignerObjNth(o);
    if (k.group >= 0) k.groupName = MS_StrDup(scene->groups[k.group]);
    return k;
}

static bool SelectionAdd(Designer *d, MS_ObjRef o)
{
    if (SelectionIndexOf(d, o) >= 0) return true;
    int n = d->selectedCount;
    if (!MS_RESERVE(d->selected, d->selectedCap, n + 1) || !MS_RESERVE(d->keys, d->keyCap, n + 1) || !SetSelPos(d, o, n + 1)) {
        DesignerMessage(d, true, TextFormat("out of memory: selection stopped at %d objects", n));
        return false;
    }
    d->keys[n] = MakeKey(o);
    d->selected[n] = o;
    d->selectedCount = n + 1;
    return true;
}

static void SelectionRemoveAt(Designer *d, int at)
{
    SetSelPos(d, d->selected[at], 0);
    free(d->keys[at].groupName);
    d->selectedCount--;
    memmove(&d->selected[at], &d->selected[at + 1], (d->selectedCount - at) * sizeof(d->selected[0]));
    memmove(&d->keys[at], &d->keys[at + 1], (d->selectedCount - at) * sizeof(d->keys[0]));
    for (int i = at; i < d->selectedCount; i++) SetSelPos(d, d->selected[i], i + 1);
}

void DesignerClearSelection(Designer *d)
{
    for (int i = 0; i < d->selectedCount; i++) {
        SetSelPos(d, d->selected[i], 0);
        free(d->keys[i].groupName);
    }
    d->selectedCount = 0;
}

bool DesignerSelectObject(Designer *d, MS_ObjRef o)
{
    if ((o.kind != MS_OBJ_BOX && o.kind != MS_OBJ_MODEL) || o.index < 0 || o.index >= KindCount(o.kind)) return false;
    return SelectionAdd(d, o);
}

void DesignerSelectLine(Designer *d, int line)
{
    for (int k = MS_OBJ_BOX; k <= MS_OBJ_MODEL; k++)
        for (int i = 0; i < KindCount(k); i++)
            if (KindRef(k, i)->line == line && !SelectionAdd(d, (MS_ObjRef){ k, i })) return;
}

// The object's whole group, boxes and models (an object without a group stands alone)
static void SelectGroupOf(Designer *d, MS_ObjRef o)
{
    int g = KindRef(o.kind, o.index)->group;
    if (g < 0) { SelectionAdd(d, o); return; }
    for (int k = MS_OBJ_BOX; k <= MS_OBJ_MODEL; k++)
        for (int i = 0; i < KindCount(k); i++)
            if (KindRef(k, i)->group == g && !SelectionAdd(d, (MS_ObjRef){ k, i })) return;
}

static bool SameBox(BoundingBox a, BoundingBox b)
{
    return a.min.x == b.min.x && a.min.y == b.min.y && a.min.z == b.min.z &&
           a.max.x == b.max.x && a.max.y == b.max.y && a.max.z == b.max.z;
}

// Objects of one kind bucketed by their exact bounds (for DesignerSync): chains in index order
typedef struct { int *head, *next; unsigned int mask; } GeoIndex;

static unsigned int GeoHash(BoundingBox b)
{
    const float *f = (const float *)&b;
    unsigned int h = 2166136261u;
    for (int i = 0; i < 6; i++) {
        float v = f[i] == 0.0f ? 0.0f : f[i];       // -0 and 0 compare equal
        unsigned int u;
        memcpy(&u, &v, sizeof(u));
        h = (h ^ u) * 16777619u;
    }
    return h;
}

static bool GeoIndexBuild(GeoIndex *g, MS_ObjKind kind)
{
    int n = KindCount(kind);
    unsigned int size = 16;
    while (size < 2u * (unsigned int)n) size *= 2;
    g->head = malloc(sizeof(int) * size);
    g->next = malloc(sizeof(int) * (n > 0 ? n : 1));
    if (!g->head || !g->next) return false;
    g->mask = size - 1;
    for (unsigned int i = 0; i < size; i++) g->head[i] = -1;
    for (int i = n - 1; i >= 0; i--) {      // backwards, so every chain ends up ascending
        unsigned int b = GeoHash(MS_ObjectBounds(scene, (MS_ObjRef){ kind, i })) & g->mask;
        g->next[i] = g->head[b];
        g->head[b] = i;
    }
    return true;
}

// First object of `kind` made by map line `line` (objects are in map-line order), or -1
static int FirstOfLine(MS_ObjKind kind, int line)
{
    int lo = 0, hi = KindCount(kind);
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (KindRef(kind, mid)->line < line) lo = mid + 1; else hi = mid;
    }
    return lo < KindCount(kind) && KindRef(kind, lo)->line == line ? lo : -1;
}

// After a map reload the box indices mean nothing. Remap each selected box:
//  1. a box with exactly the same min/max (unchanged geometry, even if lines moved),
//     preferring one from the same line;
//  2. else the box with the same index within the same map line, if that line still
//     belongs to the same group (the usual "this pillar got taller" edit).
// Anything else is dropped and the HUD says how many were lost.
void DesignerSync(Designer *d)
{
    if (d->version == scene->version) return;
    bool first = d->version == 0;
    d->version = scene->version;
    d->hit.obj = (MS_ObjRef){ MS_OBJ_NONE, -1 };    // re-picked on the next DesignerUpdate
    d->pickValid = false;
    if (first || d->selectedCount == 0) { DesignerClearSelection(d); return; }

    // The old indices mean nothing now: take the keys out and start over
    DesignerSelKey *old = d->keys;
    int oldCount = d->selectedCount;
    for (int i = 0; i < oldCount; i++) SetSelPos(d, d->selected[i], 0);
    d->keys = NULL;
    d->keyCap = 0;
    d->selectedCount = 0;

    GeoIndex geo[3] = { 0 };
    bool geoOk[3] = { false };
    for (int s = 0; s < oldCount; s++) {
        const DesignerSelKey *k = &old[s];
        MS_ObjKind kind = k->kind;
        int found = -1;
        if (!geoOk[kind] && !geo[kind].head) geoOk[kind] = GeoIndexBuild(&geo[kind], kind);
        if (geoOk[kind]) {
            for (int i = geo[kind].head[GeoHash(k->box) & geo[kind].mask]; i >= 0; i = geo[kind].next[i]) {
                const MS_Ref *r = KindRef(kind, i);
                if (!SameBox(MS_ObjectBounds(scene, (MS_ObjRef){ kind, i }), k->box) || SelectionIndexOf(d, (MS_ObjRef){ kind, i }) >= 0) continue;
                if (found < 0 || r->line == k->line) found = i;
                if (r->line == k->line) break;
            }
        }
        if (found < 0) {
            int i = FirstOfLine(kind, k->line);
            if (i >= 0) i += k->nth;
            if (i >= 0 && i < KindCount(kind) && KindRef(kind, i)->line == k->line) {
                const MS_Ref *r = KindRef(kind, i);
                const char *name = r->group >= 0 ? scene->groups[r->group] : "";
                if (strcmp(name, k->groupName ? k->groupName : "") == 0 && SelectionIndexOf(d, (MS_ObjRef){ kind, i }) < 0) found = i;
            }
        }
        if (found >= 0) SelectionAdd(d, (MS_ObjRef){ kind, found });
    }
    for (int i = 0; i < 3; i++) { free(geo[i].head); free(geo[i].next); }
    for (int s = 0; s < oldCount; s++) free(old[s].groupName);
    free(old);

    int lost = oldCount - d->selectedCount;
    if (lost > 0)
        DesignerMessage(d, false, TextFormat("map reloaded: %d of %d selected objects no longer exist", lost, oldCount));
}

// #endregion

// ======================================================================================
// #region Prompt text: editing and word wrap
// ======================================================================================

#define PROMPT_FONT     20
#define PROMPT_MARGIN   12      // box to screen edge
#define PROMPT_PAD      10      // text to box edge
#define PROMPT_LINE_H   26
#define PROMPT_HEADER_H 32     // title line + rule
#define PROMPT_MIN_LINES 3
#define PROMPT_MAX_LINES 10

typedef struct { int start, end; } PromptLine;     // byte range, end excludes the '\n'
static PromptLine *promptLines;     // the last layout (grows to the most lines the prompt had)
static int promptLineCap;

static bool IsCont(unsigned char c) { return (c & 0xC0) == 0x80; }

static int PrevBoundary(const char *s, int pos)
{
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && IsCont((unsigned char)s[pos])) pos--;
    return pos;
}

static int NextBoundary(const char *s, int len, int pos)
{
    if (pos >= len) return len;
    pos++;
    while (pos < len && IsCont((unsigned char)s[pos])) pos++;
    return pos;
}

// Width of one codepoint as the prompt text advances it (the same metrics UiText draws with,
// so wrapping and the cursor match the drawn text)
static float GlyphAdvance(int cp)
{
    return UiAdvance(cp, PROMPT_FONT);
}

// Width of s[from, to)
static float TextSpanWidth(const char *s, int from, int to)
{
    float w = 0.0f;
    for (int p = from; p < to; ) {
        int n = 1;
        int cp = GetCodepointNext(s + p, &n);
        w += GlyphAdvance(cp);
        p += n > 0 ? n : 1;
    }
    return w;
}

// Word wrap into promptLines; returns the line count (at least 1; out of memory: the text
// is laid out as far as it fits, the rest is not shown)
static int PromptLayout(const Designer *d, float maxW)
{
    const char *s = d->prompt ? d->prompt : "";
    int len = d->promptLen, count = 0, pos = 0;
    if (!MS_RESERVE(promptLines, promptLineCap, 1)) return 0;
    for (;;) {
        if (!MS_RESERVE(promptLines, promptLineCap, count + 1)) return count;
        int start = pos, p = pos, lastBreak = -1;
        float w = 0.0f;
        for (;;) {
            if (p >= len) {
                promptLines[count++] = (PromptLine){ start, len };
                return count;
            }
            if (s[p] == '\n') {
                promptLines[count++] = (PromptLine){ start, p };
                pos = p + 1;
                break;
            }
            int n = 1;
            int cp = GetCodepointNext(s + p, &n);
            if (n <= 0) n = 1;
            float a = GlyphAdvance(cp);
            if (w + a > maxW && p > start && cp != ' ') {
                // Break after the last space on this line, or mid-word if there is none
                int end = lastBreak > start ? lastBreak : p;
                promptLines[count++] = (PromptLine){ start, end };
                pos = end;
                break;
            }
            w += a;
            p += n;
            if (cp == ' ') lastBreak = p;
        }
    }
}

// Wrapped line that holds the cursor: the last one starting at or before it, so a
// cursor at a soft wrap sits at the start of the next line
static int CursorLine(int lineCount, int cursor)
{
    int line = 0;
    for (int i = 0; i < lineCount; i++)
        if (promptLines[i].start <= cursor) line = i;
    return line;
}

// Last cursor position on wrapped line `line`. A soft-wrapped line ends where the next
// one begins, and that offset displays on the next line, so stop one codepoint earlier.
static int LineEnd(const Designer *d, int line)
{
    PromptLine l = promptLines[line];
    if (l.end > l.start && l.end < d->promptLen && d->prompt[l.end] != '\n')
        return PrevBoundary(d->prompt, l.end);
    return l.end;
}

// Byte offset within line `line` closest to x pixels from the line start
static int OffsetAtX(const Designer *d, int line, float x)
{
    PromptLine l = promptLines[line];
    int best = l.start, last = LineEnd(d, line);
    float w = 0.0f, bestErr = fabsf(x);
    for (int p = l.start; p < last; ) {
        int n = 1;
        int cp = GetCodepointNext(d->prompt + p, &n);
        w += GlyphAdvance(cp);
        p += n > 0 ? n : 1;
        if (fabsf(w - x) < bestErr) { bestErr = fabsf(w - x); best = p; }
    }
    return best;
}

static float PromptTextWidth(int screenW)
{
    return (float)(screenW - 2 * PROMPT_MARGIN - 2 * PROMPT_PAD);
}

// Inserts n bytes at the cursor, truncated at a codepoint boundary to PROMPT_LIMIT (or to
// what memory allows)
static void PromptInsert(Designer *d, const char *bytes, int n)
{
    int room = PROMPT_LIMIT - 1 - d->promptLen;
    if (n > room) {
        n = room;
        while (n > 0 && IsCont((unsigned char)bytes[n])) n--;
    }
    if (n <= 0 || !MS_RESERVE(d->prompt, d->promptCap, d->promptLen + n + 1)) return;
    memmove(d->prompt + d->cursor + n, d->prompt + d->cursor, d->promptLen - d->cursor + 1);
    memcpy(d->prompt + d->cursor, bytes, n);
    d->promptLen += n;
    d->cursor += n;
}

static void PromptDeleteRange(Designer *d, int from, int to)
{
    if (to <= from) return;
    memmove(d->prompt + from, d->prompt + to, d->promptLen - to + 1);
    d->promptLen -= to - from;
    d->cursor = from;
}

// Clipboard text: CR dropped, tabs -> space, other control characters dropped
static void PromptPaste(Designer *d)
{
    const char *clip = GetClipboardText();
    if (!clip) return;
    size_t clipLen = strlen(clip);
    if (clipLen > PROMPT_LIMIT) clipLen = PROMPT_LIMIT;
    char *buf = malloc(clipLen + 1);
    if (!buf) return;
    int n = 0;
    for (const char *c = clip; *c && n < (int)clipLen; c++) {
        unsigned char ch = (unsigned char)*c;
        if (ch == '\r') continue;
        if (ch == '\t') ch = ' ';
        if (ch < 32 && ch != '\n') continue;
        if (ch == 127) continue;
        buf[n++] = (char)ch;
    }
    int before = d->promptLen;
    PromptInsert(d, buf, n);
    free(buf);
    if (d->promptLen - before < n) DesignerMessage(d, true, TextFormat("prompt limit: %d bytes", PROMPT_LIMIT - 1));
}

static bool PromptBlank(const Designer *d)
{
    for (int i = 0; i < d->promptLen; i++)
        if (d->prompt[i] != ' ' && d->prompt[i] != '\n') return false;
    return true;
}

void DesignerOpenPrompt(Designer *d, const char *text)
{
    if (text) {
        // Cut to the limit without splitting a codepoint (same rule as PromptInsert)
        size_t len = strlen(text);
        int n = len > PROMPT_LIMIT - 1 ? PROMPT_LIMIT - 1 : (int)len;
        while (n > 0 && (size_t)n < len && IsCont((unsigned char)text[n])) n--;
        if (!MS_RESERVE(d->prompt, d->promptCap, n + 1)) n = 0;
        if (d->prompt) {
            memcpy(d->prompt, text, n);
            d->prompt[n] = 0;
        }
        d->promptLen = d->cursor = n;
        d->scroll = 0;
    }
    d->promptOpen = true;
    d->wantX = -1.0f;
    if (d->interactive) EnableCursor();
}

static void ClosePrompt(Designer *d, bool clear)
{
    d->promptOpen = false;
    if (clear) {
        if (d->prompt) d->prompt[0] = 0;
        d->promptLen = d->cursor = d->scroll = 0;
    }
    // GLFW can report one big mouse jump when the cursor is captured again
    d->freezeFrames = 2;
    // Opened from snip mode: back to it (camera still frozen, cursor still free)
    if (d->interactive && !d->snipping) DisableCursor();
}

// Success: the box closes and the text and the regions are gone (snip mode ends).
// Failure: the box stays open with the text and the regions, to try again.
void DesignerSubmitDone(Designer *d, bool ok, const char *msg)
{
    DesignerMessage(d, !ok, msg);
    if (ok) {
        DesignerSnipEnd(d);
        ClosePrompt(d, true);
    }
}

static bool KeyRepeat(int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); }

static DesignerAction PromptInput(Designer *d)
{
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    if (IsKeyPressed(KEY_ESCAPE)) {
        ClosePrompt(d, true);
        return DESIGNER_NONE;
    }
    // Plain Enter only on the press, never on key repeat, so a held Enter can't submit
    if (shift && (KeyRepeat(KEY_ENTER) || KeyRepeat(KEY_KP_ENTER))) {
        PromptInsert(d, "\n", 1);
        d->wantX = -1.0f;
    } else if (!shift && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))) {
        if (PromptBlank(d)) DesignerMessage(d, true, "type a request first (Esc closes the box)");
        else return DESIGNER_SUBMIT;
    }

    // Typed text (Ctrl+letter produces no characters on GLFW, so Ctrl+V doesn't type a 'v')
    int before = d->promptLen, typed = 0;
    for (int cp = GetCharPressed(); cp > 0; cp = GetCharPressed()) {
        if (cp < 32 || cp == 127) continue;
        int n = 0;
        const char *utf8 = CodepointToUTF8(cp, &n);
        PromptInsert(d, utf8, n);
        typed += n;
        d->wantX = -1.0f;
    }
    if (typed > d->promptLen - before) DesignerMessage(d, true, TextFormat("prompt limit: %d bytes", PROMPT_LIMIT - 1));

    if (ctrl && KeyRepeat(KEY_V)) { PromptPaste(d); d->wantX = -1.0f; }

    if (KeyRepeat(KEY_BACKSPACE) && d->cursor > 0) {
        int from = PrevBoundary(d->prompt, d->cursor);
        if (ctrl) {
            // Whole word: spaces, then the word before them
            from = d->cursor;
            while (from > 0 && (d->prompt[from - 1] == ' ' || d->prompt[from - 1] == '\n')) from--;
            while (from > 0 && d->prompt[from - 1] != ' ' && d->prompt[from - 1] != '\n') from--;
        }
        PromptDeleteRange(d, from, d->cursor);
        d->wantX = -1.0f;
    }
    if (KeyRepeat(KEY_DELETE) && d->cursor < d->promptLen) {
        int at = d->cursor;
        PromptDeleteRange(d, at, NextBoundary(d->prompt, d->promptLen, at));
        d->wantX = -1.0f;
    }
    if (KeyRepeat(KEY_LEFT))  { d->cursor = PrevBoundary(d->prompt, d->cursor); d->wantX = -1.0f; }
    if (KeyRepeat(KEY_RIGHT)) { d->cursor = NextBoundary(d->prompt, d->promptLen, d->cursor); d->wantX = -1.0f; }

    bool home = KeyRepeat(KEY_HOME), end = KeyRepeat(KEY_END);
    bool up = KeyRepeat(KEY_UP), down = KeyRepeat(KEY_DOWN);
    if (home || end || up || down) {
        if (ctrl && home) d->cursor = 0;
        else if (ctrl && end) d->cursor = d->promptLen;
        else {
            int lines = PromptLayout(d, PromptTextWidth(GetScreenWidth()));
            int line = CursorLine(lines, d->cursor);
            if (home) d->cursor = promptLines[line].start;
            if (end) d->cursor = LineEnd(d, line);
            if (up || down) {
                if (d->wantX < 0.0f) d->wantX = TextSpanWidth(d->prompt, promptLines[line].start, d->cursor);
                int target = line + (down ? 1 : -1);
                if (target < 0) d->cursor = 0;
                else if (target >= lines) d->cursor = d->promptLen;
                else d->cursor = OffsetAtX(d, target, d->wantX);
            }
        }
        if (!(up || down)) d->wantX = -1.0f;
    }
    return DESIGNER_NONE;
}

// #endregion

// ======================================================================================
// #region Update
// ======================================================================================

bool DesignerCapturesInput(const Designer *d)
{
    return d->promptOpen || d->snipping || (d->volumeMode && d->volumeStep == VOLUME_STEP_HEIGHT) || d->freezeFrames > 0;
}

bool DesignerTakesArrows(const Designer *d)
{
    return d->volumeMode;
}

// #endregion

// ======================================================================================
// #region Volume mode
// ======================================================================================
// V in spectator mode. Click a surface for the first corner, a second one for the opposite
// footprint corner, then set the height (mouse up / down or the wheel; the camera is
// frozen meanwhile) and click again. Points snap to a grid of d->volumeSnap metres on the
// axes along the clicked face (the face's own plane stays exact); Ctrl turns snapping off.
// Arrows / PgUp / PgDn move the active volume by one step, with Alt they grow the face on
// that side, with Alt+Shift they shrink it. Volumes stay until deleted (Delete, Shift+Delete).

static const float VOLUME_SNAPS[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f };
#define VOLUME_DEFAULT_HEIGHT 2.0f  // metres, when both corner clicks are at the same height

static float VolumeSnap(const Designer *d) { return d->volumeSnap > 0.0f ? d->volumeSnap : VOLUME_SNAP_DEFAULT; }

float DesignerSnap(const Designer *d) { return VolumeSnap(d); }

float DesignerCycleSnap(Designer *d)
{
    float s = VolumeSnap(d);
    int n = (int)(sizeof(VOLUME_SNAPS) / sizeof(VOLUME_SNAPS[0])), k = 0;
    while (k < n && VOLUME_SNAPS[k] != s) k++;
    return d->volumeSnap = VOLUME_SNAPS[(k + 1) % n];
}

static float SnapTo(float x, float step) { return roundf(x / step) * step; }

// The crosshair hit point, snapped on the axes along the hit face (none with Ctrl)
static Vector3 VolumePoint(const Designer *d)
{
    Vector3 p = d->hit.point, n = d->hit.normal;
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) return p;
    float s = VolumeSnap(d);
    if (fabsf(n.x) < 0.9f) p.x = SnapTo(p.x, s);
    if (fabsf(n.y) < 0.9f) p.y = SnapTo(p.y, s);
    if (fabsf(n.z) < 0.9f) p.z = SnapTo(p.z, s);
    return p;
}

// The volume being made, from the clicks so far: after the first click a flat footprint up
// to the crosshair, after the second one the footprint with its (snapped) height
static BoundingBox VolumeDraft(const Designer *d)
{
    bool second = d->volumeStep == VOLUME_STEP_SECOND;
    Vector3 a = d->volA, b = second ? VolumePoint(d) : d->volB;
    float base = second ? fminf(a.y, b.y) : d->volBase;
    float h = 0.0f;
    if (!second) {
        float s = (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) ? VOLUME_MIN_SIZE : VolumeSnap(d);
        h = fmaxf(s == VOLUME_MIN_SIZE ? d->volHeight : SnapTo(d->volHeight, s), s);
    }
    return (BoundingBox){ { fminf(a.x, b.x), base, fminf(a.z, b.z) }, { fmaxf(a.x, b.x), base + h, fmaxf(a.z, b.z) } };
}

bool DesignerAddVolume(Designer *d, BoundingBox box)
{
    Vector3 a = box.min, b = box.max;
    if (!isfinite(a.x) || !isfinite(a.y) || !isfinite(a.z) || !isfinite(b.x) || !isfinite(b.y) || !isfinite(b.z)) return false;
    BoundingBox v = { Vector3Min(a, b), Vector3Max(a, b) };
    if (v.max.x - v.min.x < VOLUME_MIN_SIZE) v.max.x = v.min.x + VOLUME_MIN_SIZE;
    if (v.max.y - v.min.y < VOLUME_MIN_SIZE) v.max.y = v.min.y + VOLUME_MIN_SIZE;
    if (v.max.z - v.min.z < VOLUME_MIN_SIZE) v.max.z = v.min.z + VOLUME_MIN_SIZE;
    DesignVolume *slot = MS_PUSH(d->volumes, d->volumeCount, d->volumeCap);
    if (!slot) {
        DesignerMessage(d, true, "out of memory: volume not added");
        return false;
    }
    slot->box = v;
    d->activeVolume = d->volumeCount - 1;
    return true;
}

// Shift+V: the selection's bounds (or the crosshair object's) as a new volume
static void VolumeFromSelection(Designer *d)
{
    BoundingBox b;
    if (d->selectedCount > 0) {
        b = MS_ObjectBounds(scene, d->selected[0]);
        for (int i = 1; i < d->selectedCount; i++) {
            BoundingBox o = MS_ObjectBounds(scene, d->selected[i]);
            b.min = Vector3Min(b.min, o.min);
            b.max = Vector3Max(b.max, o.max);
        }
    } else if (!ObjIsNone(d->hit.obj)) b = MS_ObjectBounds(scene, d->hit.obj);
    else {
        DesignerMessage(d, true, "nothing selected and nothing under the crosshair: no volume");
        return;
    }
    if (DesignerAddVolume(d, b))
        DesignerMessage(d, false, TextFormat("V%d from the %s bounds", d->volumeCount,
                        d->selectedCount > 0 ? "selection's" : "crosshair object's"));
}

static void VolumeCancel(Designer *d)
{
    // The height step froze the camera: let the mouse settle before it looks around again
    if (d->volumeStep == VOLUME_STEP_HEIGHT) d->freezeFrames = 2;
    d->volumeStep = VOLUME_STEP_FIRST;
}

static void VolumeModeEnd(Designer *d)
{
    VolumeCancel(d);
    d->volumeMode = false;
}

void DesignerVolumeModeEnd(Designer *d) { VolumeModeEnd(d); }

// Arrows / PgUp / PgDn on the active volume. The horizontal arrows follow the world axis
// nearest to the camera's forward / right, so "up" pushes the volume away from the eye.
static void VolumeEdit(Designer *d)
{
    if (d->activeVolume < 0 || d->activeVolume >= d->volumeCount) return;
    static const int KEYS[6] = { KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_PAGE_UP, KEY_PAGE_DOWN };
    Vector3 f = fabsf(d->viewFwd.x) > fabsf(d->viewFwd.z) ? (Vector3){ d->viewFwd.x > 0 ? 1.0f : -1.0f, 0, 0 }
                                                         : (Vector3){ 0, 0, d->viewFwd.z >= 0 ? 1.0f : -1.0f };
    Vector3 right = { -f.z, 0.0f, f.x };    // as the spectator's right = (-cos yaw, 0, sin yaw)
    const Vector3 dirs[6] = { f, Vector3Negate(f), right, Vector3Negate(right), { 0, 1, 0 }, { 0, -1, 0 } };
    bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    float s = VolumeSnap(d);
    BoundingBox *v = &d->volumes[d->activeVolume].box;
    for (int k = 0; k < 6; k++) {
        if (!KeyRepeat(KEYS[k])) continue;
        Vector3 dir = dirs[k];
        float *mn[3] = { &v->min.x, &v->min.y, &v->min.z }, *mx[3] = { &v->max.x, &v->max.y, &v->max.z };
        int a = dir.x != 0.0f ? 0 : dir.y != 0.0f ? 1 : 2;
        float sign = dir.x + dir.y + dir.z;
        if (!alt) {
            *mn[a] += sign * s;
            *mx[a] += sign * s;
        } else if (!shift) {
            if (sign > 0) *mx[a] += s;
            else *mn[a] -= s;
        } else {
            // Shrink, but never thinner than one step (or VOLUME_MIN_SIZE when already thinner)
            float keep = fminf(s, *mx[a] - *mn[a]);
            if (sign > 0) *mx[a] = fmaxf(*mx[a] - s, *mn[a] + keep);
            else *mn[a] = fminf(*mn[a] + s, *mx[a] - keep);
        }
    }
}

static DesignerAction VolumeInput(Designer *d)
{
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool enter = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);

    if (d->volumeStep != VOLUME_STEP_FIRST) {
        if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            VolumeCancel(d);
            return DESIGNER_NONE;
        }
        if (d->volumeStep == VOLUME_STEP_SECOND) {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (ObjIsNone(d->hit.obj)) { DesignerMessage(d, true, "no surface under the crosshair"); return DESIGNER_NONE; }
                d->volB = VolumePoint(d);
                d->volBase = fminf(d->volA.y, d->volB.y);
                float dy = fabsf(d->volA.y - d->volB.y);
                d->volHeight = dy >= VolumeSnap(d) ? dy : VOLUME_DEFAULT_HEIGHT;
                d->volumeStep = VOLUME_STEP_HEIGHT;
            }
            return DESIGNER_NONE;
        }
        // Height: the top face follows the mouse at the angle the look would turn, so it
        // moves about as far on screen as the mouse did
        BoundingBox draft = VolumeDraft(d);
        Vector3 top = { (draft.min.x + draft.max.x) * 0.5f, draft.max.y, (draft.min.z + draft.max.z) * 0.5f };
        float dist = fmaxf(Vector3Distance(d->eye, top), 1.0f);
        d->volHeight -= GetMouseDelta().y * 0.0022f * dist;
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) d->volHeight = SnapTo(d->volHeight, VolumeSnap(d)) + wheel * VolumeSnap(d) * (shift ? 4.0f : 1.0f);
        d->volHeight = fmaxf(d->volHeight, VOLUME_MIN_SIZE);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || enter) {
            if (DesignerAddVolume(d, VolumeDraft(d))) {
                BoundingBox v = d->volumes[d->activeVolume].box;
                DesignerMessage(d, false, TextFormat("V%d: %.2f x %.2f x %.2f m", d->volumeCount,
                                v.max.x - v.min.x, v.max.y - v.min.y, v.max.z - v.min.z));
            }
            VolumeCancel(d);
        }
        return DESIGNER_NONE;
    }

    if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        VolumeModeEnd(d);
        return DESIGNER_NONE;
    }
    if (enter) {
        DesignerOpenPrompt(d, NULL);
        return DESIGNER_NONE;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (ObjIsNone(d->hit.obj)) DesignerMessage(d, true, "no surface under the crosshair");
        else {
            d->volA = VolumePoint(d);
            d->volumeStep = VOLUME_STEP_SECOND;
        }
        return DESIGNER_NONE;
    }
    if (IsKeyPressed(KEY_N)) DesignerMessage(d, false, TextFormat("volume snap %g m", DesignerCycleSnap(d)));
    if (d->volumeCount > 0 && (IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressed(KEY_RIGHT_BRACKET))) {
        int step = IsKeyPressed(KEY_RIGHT_BRACKET) ? 1 : -1;
        d->activeVolume = ((d->activeVolume < 0 ? 0 : d->activeVolume) + step + d->volumeCount) % d->volumeCount;
    }
    if (IsKeyPressed(KEY_DELETE) && d->volumeCount > 0) {
        if (shift) {
            DesignerMessage(d, false, TextFormat("%d volume%s cleared", d->volumeCount, d->volumeCount == 1 ? "" : "s"));
            d->volumeCount = 0;
            d->activeVolume = -1;
        } else if (d->activeVolume >= 0 && d->activeVolume < d->volumeCount) {
            DesignerMessage(d, false, TextFormat("V%d removed", d->activeVolume + 1));
            memmove(&d->volumes[d->activeVolume], &d->volumes[d->activeVolume + 1],
                    sizeof(DesignVolume) * (d->volumeCount - d->activeVolume - 1));
            d->volumeCount--;
            if (d->activeVolume >= d->volumeCount) d->activeVolume = d->volumeCount - 1;
        }
    }
    VolumeEdit(d);
    return DESIGNER_NONE;
}

// #endregion

// ======================================================================================
// #region Snip mode
// ======================================================================================
// C in spectator mode: the camera freezes and the cursor is free. Drag = a new region
// (replaces the others), Shift+drag = add one, Enter = the prompt box
// with the regions attached (plus the box selection, made before C). RMB, Esc or C again
// leave snip mode and drop its regions: they only mean something for this frozen camera.
// Esc in the prompt box closes the box (discarding the text, as always) and returns to
// snip mode with the regions intact; a successful submit ends snip mode.

void DesignerSnipBegin(Designer *d)
{
    d->snipping = true;
    d->dragging = false;
    d->regionCount = 0;
    if (d->interactive) EnableCursor();
}

void DesignerSnipEnd(Designer *d)
{
    if (!d->snipping) return;
    d->snipping = false;
    d->dragging = false;
    d->regionCount = 0;
    d->freezeFrames = 2;    // GLFW can report one big mouse jump when the cursor is captured again
    if (d->interactive) DisableCursor();
}

// Whole pixels, clamped to the frame
static Rectangle ClampRect(Rectangle r, int w, int h)
{
    float x0 = Clamp(roundf(fminf(r.x, r.x + r.width)), 0.0f, (float)w);
    float y0 = Clamp(roundf(fminf(r.y, r.y + r.height)), 0.0f, (float)h);
    float x1 = Clamp(roundf(fmaxf(r.x, r.x + r.width)), 0.0f, (float)w);
    float y1 = Clamp(roundf(fmaxf(r.y, r.y + r.height)), 0.0f, (float)h);
    return (Rectangle){ x0, y0, x1 - x0, y1 - y0 };
}

bool DesignerAddRegion(Designer *d, Rectangle rect, int w, int h)
{
    Rectangle r = ClampRect(rect, w, h);
    if (r.width < 1.0f || r.height < 1.0f) return false;
    DesignRegion *slot = MS_PUSH(d->regions, d->regionCount, d->regionCap);
    if (!slot) {
        DesignerMessage(d, true, "out of memory: region not added");
        return false;
    }
    slot->rect = r;
    return true;
}

static Rectangle DragRect(const Designer *d)
{
    return ClampRect((Rectangle){ d->dragStart.x, d->dragStart.y, d->dragEnd.x - d->dragStart.x,
                                  d->dragEnd.y - d->dragStart.y }, GetScreenWidth(), GetScreenHeight());
}

static DesignerAction SnipInput(Designer *d)
{
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_C) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        DesignerSnipEnd(d);
        return DESIGNER_NONE;
    }
    if (!d->dragging && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))) {
        DesignerOpenPrompt(d, NULL);
        return DESIGNER_NONE;
    }
    Vector2 m = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        d->dragging = true;
        d->dragAdd = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        d->dragStart = d->dragEnd = m;
    }
    if (d->dragging) {
        d->dragEnd = m;
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            d->dragging = false;
            Rectangle r = DragRect(d);
            // A click without a drag (under 4 px) changes nothing
            if (r.width >= 4.0f && r.height >= 4.0f) {
                if (!d->dragAdd) d->regionCount = 0;
                DesignerAddRegion(d, r, GetScreenWidth(), GetScreenHeight());
            }
        }
    }
    return DESIGNER_NONE;
}

void DesignerPickCamera(Designer *d, Camera3D cam)
{
    // A still camera over the same scene hits the same thing (the ray cast is most of a reused
    // frame's CPU time on a map with many model triangles). Bitwise compares: -0 and +0 give
    // different ray slopes. DesignerSync invalidates it whenever the scene changes.
    if (d->pickValid && d->pickVersion == scene->version && memcmp(&d->pickPos, &cam.position, sizeof(Vector3)) == 0 &&
        memcmp(&d->pickTarget, &cam.target, sizeof(Vector3)) == 0) return;
    d->pickValid = true;
    d->pickVersion = scene->version;
    d->pickPos = cam.position;
    d->pickTarget = cam.target;
    d->eye = cam.position;
    d->viewFwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    d->hit = DesignerPick(cam.position, d->viewFwd);
}

DesignerHit DesignerPickAt(Camera3D cam, int w, int h, float x, float y)
{
    RayFrame f = MakeRayFrame(cam, w > 0 ? w : 1, h > 0 ? h : 1);
    return DesignerPick(cam.position, RayThrough(&f, x, y));
}

DesignerAction DesignerUpdate(Designer *d, float dt)
{
    if (d->messageTime > 0.0f) d->messageTime -= dt;
    bool frozen = d->freezeFrames > 0;
    if (d->freezeFrames > 0) d->freezeFrames--;

    if (d->promptOpen) return PromptInput(d);
    if (d->snipping) return SnipInput(d);
    if (frozen) return DESIGNER_NONE;   // the frame(s) right after the box closed

    if (IsKeyPressed(KEY_V)) {
        if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) VolumeFromSelection(d);
        else if (d->volumeMode) VolumeModeEnd(d);
        else {
            d->volumeMode = true;
            d->volumeStep = VOLUME_STEP_FIRST;
        }
        return DESIGNER_NONE;
    }
    if (d->volumeMode) {
        if (d->volumeStep == VOLUME_STEP_FIRST && IsKeyPressed(KEY_C)) {
            VolumeModeEnd(d);
            DesignerSnipBegin(d);
            return DESIGNER_NONE;
        }
        return VolumeInput(d);
    }

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        DesignerOpenPrompt(d, NULL);
        return DESIGNER_NONE;
    }
    if (IsKeyPressed(KEY_C)) {
        DesignerSnipBegin(d);
        return DESIGNER_NONE;
    }

    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) DesignerClearSelection(d);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !ObjIsNone(d->hit.obj)) {
        MS_ObjRef box = d->hit.obj;
        if (ctrl) {
            // Ctrl: the whole group (Ctrl+Shift adds it to the selection)
            if (!shift) DesignerClearSelection(d);
            SelectGroupOf(d, box);
        } else if (shift) {
            int at = SelectionIndexOf(d, box);
            if (at >= 0) SelectionRemoveAt(d, at);
            else SelectionAdd(d, box);
        } else {
            DesignerClearSelection(d);
            SelectionAdd(d, box);
        }
    }
    return DESIGNER_NONE;
}

void DesignerFreeRequest(DesignRequest *req)
{
    free(req->prompt);
    free(req->selected);
    for (int i = 0; i < req->regionCount; i++) DesignerFreeRegion(&req->regions[i]);
    free(req->regions);
    for (int i = 0; i < req->volumeCount; i++) DesignerFreeVolume(&req->volumes[i]);
    free(req->volumes);
    memset(req, 0, sizeof(*req));
}

bool DesignerBuildRequest(const Designer *d, Camera3D cam, float yaw, float pitch,
                          int width, int height, DesignRequest *req)
{
    memset(req, 0, sizeof(*req));
    req->prompt = MS_StrNDup(d->prompt ? d->prompt : "", d->promptLen);
    req->selected = malloc(sizeof(MS_ObjRef) * (d->selectedCount > 0 ? d->selectedCount : 1));
    req->regions = calloc(d->regionCount > 0 ? d->regionCount : 1, sizeof(DesignRegionInfo));
    req->volumes = calloc(d->volumeCount > 0 ? d->volumeCount : 1, sizeof(DesignVolumeInfo));
    if (!req->prompt || !req->selected || !req->regions || !req->volumes) {
        DesignerFreeRequest(req);
        return false;
    }
    req->eye = cam.position;
    req->yaw = yaw;
    req->pitch = pitch;
    req->fovy = cam.fovy;
    req->width = width;
    req->height = height;
    req->hit = d->hit;
    req->selectedCount = d->selectedCount;
    for (int i = 0; i < d->selectedCount; i++) req->selected[i] = d->selected[i];
    req->mapVersion = scene->version;

    // Regions: what each one shows is computed now, against the map as it is at submit
    req->regionCount = d->regionCount;
    for (int i = 0; i < d->regionCount; i++) {
        DesignRegionInfo *r = &req->regions[i];
        DesignerAnalyzeRegion(cam, width, height, d->regions[i].rect, r);
        int objects = r->objectCount + r->omittedObjects;
        TraceLog(LOG_INFO, "monolith: region %d: %.0fx%.0f px, %dx%d rays, %d object%s, sky %.0f%%, %.1f ms (%d boxes, %d models)",
                 i + 1, r->rect.width, r->rect.height, r->gridW, r->gridH, objects, objects == 1 ? "" : "s",
                 100.0f * r->sky, r->ms, scene->boxCount, scene->modelCount);
    }
    // Volumes: likewise
    req->volumeCount = d->volumeCount;
    for (int i = 0; i < d->volumeCount; i++) {
        DesignVolumeInfo *v = &req->volumes[i];
        DesignerAnalyzeVolume(d->volumes[i].box, VolumeSnap(d), v);
        TraceLog(LOG_INFO, "monolith: volume %d: %.2f x %.2f x %.2f m, %d object%s, empty %.0f%%, %d cells, %.1f ms",
                 i + 1, v->box.max.x - v->box.min.x, v->box.max.y - v->box.min.y, v->box.max.z - v->box.min.z,
                 v->objectCount, v->objectCount == 1 ? "" : "s", 100.0f * v->empty, v->grid[0] * v->grid[1] * v->grid[2], v->ms);
    }
    return true;
}

// #endregion

// ======================================================================================
// #region Drawing
// ======================================================================================

// Scratch arrays of the per-frame drawing code: grown to what the scene / regions / prompt
// need, never shrunk, so steady frames allocate nothing; freed by DesignerFree
static struct {
    unsigned char *seen; int seenCap;   // DesignerDrawHud: groups already named
    float *ys; int ysCap;               // DimOutside: band edges
    Vector2 *xs; int xsCap;             // DimOutside: rectangles in a band
    Rectangle *rects; int rectsCap;     // DesignerDrawRegions
    char *line; int lineCap;            // DesignerDrawPrompt: one wrapped line
} scratch;

#define SEL_COLOR   (Color){ 255, 160, 40, 255 }
#define HIT_COLOR   (Color){ 90, 220, 255, 255 }
#define REGION_COLOR (Color){ 240, 90, 220, 255 }
#define VOLUME_COLOR (Color){ 70, 235, 170, 255 }

// Distance from the eye to the nearest point of a box (0 inside)
static float BoxDistance(BoundingBox b, Vector3 p)
{
    Vector3 c = { Clamp(p.x, b.min.x, b.max.x), Clamp(p.y, b.min.y, b.max.y), Clamp(p.z, b.min.z, b.max.z) };
    return Vector3Distance(c, p);
}

// The box grown a little, so its outline sits just in front of the faces it outlines.
// Near the camera that is what keeps the lines from z-fighting; beyond ~60 m the depth
// bias in DesignerDrawOverlay is larger and does it (so the inflation stops growing).
static void InflatedBox(BoundingBox b, Vector3 eye, Vector3 *center, Vector3 *size)
{
    float e = fminf(0.004f + 0.0015f * BoxDistance(b, eye), 0.1f);
    *center = Vector3Scale(Vector3Add(b.min, b.max), 0.5f);
    *size = Vector3Add(Vector3Subtract(b.max, b.min), (Vector3){ 2 * e, 2 * e, 2 * e });
}

// A small square on the hit face around the hit point, with its normal as a short tick
static void DrawHitMarker(DesignerHit hit, Vector3 eye, Color c)
{
    float dist = Vector3Distance(eye, hit.point);
    float s = 0.012f * dist + 0.02f;
    Vector3 n = hit.normal;
    // Two axes spanning the face
    Vector3 u = fabsf(n.x) > 0.5f ? (Vector3){ 0, 1, 0 } : (Vector3){ 1, 0, 0 };
    // made perpendicular to n for model faces (no change for axis-aligned box normals)
    if (Vector3DotProduct(u, n) != 0.0f) u = Vector3Normalize(Vector3Subtract(u, Vector3Scale(n, Vector3DotProduct(u, n))));
    Vector3 v = Vector3CrossProduct(n, u);
    Vector3 p = Vector3Add(hit.point, Vector3Scale(n, 0.004f + 0.0015f * dist));
    Vector3 q[4];
    for (int i = 0; i < 4; i++) {
        float a = (i == 0 || i == 3) ? -s : s, b = (i < 2) ? -s : s;
        q[i] = Vector3Add(p, Vector3Add(Vector3Scale(u, a), Vector3Scale(v, b)));
    }
    for (int i = 0; i < 4; i++) DrawLine3D(q[i], q[(i + 1) % 4], c);
    DrawLine3D(Vector3Add(p, Vector3Scale(u, -2 * s)), Vector3Add(p, Vector3Scale(u, 2 * s)), c);
    DrawLine3D(Vector3Add(p, Vector3Scale(v, -2 * s)), Vector3Add(p, Vector3Scale(v, 2 * s)), c);
    DrawLine3D(p, Vector3Add(p, Vector3Scale(n, 4 * s)), c);
}

// The 8 corners of a model instance's oriented box (its model-space bounds under its
// transform), pushed out a little like InflatedBox; corner i: bit 0 x, bit 1 y, bit 2 z
static void ModelCorners(const MS_ModelInstance *m, Vector3 eye, Vector3 *out)
{
    BoundingBox lb = scene->modelFiles[m->file].bounds;
    Vector3 c = Vector3Scale(Vector3Add(m->bounds.min, m->bounds.max), 0.5f);
    float e = fminf(0.004f + 0.0015f * BoxDistance(m->bounds, eye), 0.1f);
    for (int i = 0; i < 8; i++) {
        Vector3 p = { (i & 1) ? lb.max.x : lb.min.x, (i & 2) ? lb.max.y : lb.min.y, (i & 4) ? lb.max.z : lb.min.z };
        p = Vector3Transform(p, m->transform);
        Vector3 dir = Vector3Subtract(p, c);
        float len = Vector3Length(dir);
        out[i] = len > 0.0f ? Vector3Add(p, Vector3Scale(dir, e / len)) : p;
    }
}

// Outline of an object: a box's (inflated) AABB, a model's oriented box
static void DrawObjWires(MS_ObjRef o, Vector3 eye, Color col)
{
    if (o.kind == MS_OBJ_MODEL) {
        Vector3 q[8];
        ModelCorners(&scene->models[o.index], eye, q);
        for (int i = 0; i < 8; i++)
            for (int b = 1; b < 8; b <<= 1)
                if (!(i & b)) DrawLine3D(q[i], q[i | b], col);
        return;
    }
    Vector3 c, s;
    InflatedBox(scene->boxes[o.index], eye, &c, &s);
    DrawCubeWiresV(c, s, col);
}

// Translucent faces of an object
static void DrawObjTint(MS_ObjRef o, Vector3 eye, Color col)
{
    if (o.kind == MS_OBJ_MODEL) {
        static const int FACES[6][4] = { { 0, 2, 6, 4 }, { 1, 5, 7, 3 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 4, 6, 7, 5 } };
        Vector3 q[8];
        ModelCorners(&scene->models[o.index], eye, q);
        rlDisableBackfaceCulling();
        for (int f = 0; f < 6; f++) {
            DrawTriangle3D(q[FACES[f][0]], q[FACES[f][1]], q[FACES[f][2]], col);
            DrawTriangle3D(q[FACES[f][0]], q[FACES[f][2]], q[FACES[f][3]], col);
        }
        rlDrawRenderBatchActive();
        rlEnableBackfaceCulling();
        return;
    }
    Vector3 c, s;
    InflatedBox(scene->boxes[o.index], eye, &c, &s);
    DrawCubeV(c, s, col);
}

// BeginMode3D for overlays drawn on top of an R3D frame, depth-tested against its depth.
// R3D_End copies its depth buffer into the target, and BeginMode3D builds the same
// projection (same fovy, aspect and rlGetCullDistanceNear/Far), so the overlay can be
// depth-tested against the scene. Shift the overlay's depth 4 steps of the 24-bit depth
// buffer toward the camera (z_ndc -= bias): a constant step in depth-buffer units is
// the precision the scene's depth has at every distance, so lines on faces don't
// z-fight. With the 1 cm near plane one step is z^2 / 168 km, i.e. 0.6 mm at 10 m,
// 13 cm at 150 m; more bias would let the overlay show through nearby walls far away.
static void BeginOverlay3D(Camera3D cam)
{
    BeginMode3D(cam);
    Matrix proj = rlGetMatrixProjection();
    const float bias = 4.0f / 16777216.0f * 2.0f;
    proj.m2 -= bias * proj.m3;
    proj.m6 -= bias * proj.m7;
    proj.m10 -= bias * proj.m11;
    proj.m14 -= bias * proj.m15;
    rlSetMatrixProjection(proj);
}

// Back to what R3D_End leaves and raylib's 2D drawing expects
static void EndOverlay3D(void)
{
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
    rlDisableDepthTest();
    EndMode3D();
}

void DesignerDrawOverlay(const Designer *d, Camera3D cam, bool hover)
{
    if (d->selectedCount == 0 && ObjIsNone(d->hit.obj)) return;
    Vector3 eye = cam.position;

    BeginOverlay3D(cam);

    // Selected objects: a faint always-on-top outline, so selected objects behind walls
    // stay findable...
    rlDisableDepthTest();
    for (int i = 0; i < d->selectedCount; i++) DrawObjWires(d->selected[i], eye, Fade(SEL_COLOR, 0.22f));
    rlDrawRenderBatchActive();

    // ...then, depth-tested, a translucent tint (no depth writes, so it can't hide the
    // outlines) and a strong outline. Models get their oriented box (their bounds under
    // their transform).
    rlEnableDepthTest();
    rlDisableDepthMask();
    for (int i = 0; i < d->selectedCount; i++) DrawObjTint(d->selected[i], eye, Fade(SEL_COLOR, 0.16f));
    rlDrawRenderBatchActive();
    for (int i = 0; i < d->selectedCount; i++) DrawObjWires(d->selected[i], eye, Fade(SEL_COLOR, 0.95f));

    if (!ObjIsNone(d->hit.obj)) {
        // Hover: a subtle white outline (not for view-marked.png, where only the selection
        // and the hit point should stand out)
        if (hover && SelectionIndexOf(d, d->hit.obj) < 0) DrawObjWires(d->hit.obj, eye, (Color){ 255, 255, 255, 110 });
        DrawHitMarker(d->hit, eye, HIT_COLOR);
    }
    EndOverlay3D();
}

// Help bar of a mode at the bottom centre: the mode's pill, then its key hints (UiHints markup)
static void DrawHelpBar(const char *mode, Color modeColor, const char *hints, int w, int h)
{
    int pw = UiPillWidth(mode), tw = pw + 10 + UiHintsWidth(hints, UI_S);
    int x = (w - tw) / 2, y = h - 66;
    UiPanel((Rectangle){ (float)(x - 10), (float)y, (float)(tw + 20), 30 }, UI_PANEL, UI_LINE);
    UiPill(mode, x, y + 5, modeColor, (Color){ 16, 16, 18, 255 });
    UiHints(hints, x + pw + 10, y + (30 - UiLineHeight(UI_S)) / 2, UI_S, UI_INK);
}

// Volume label: a pill with the text, centred above screen point p (kept on screen)
static void DrawVolumeTab(Vector2 p, const char *text, int w, int h)
{
    int tw = UiPillWidth(text), th = UI_PILL_H;
    int lx = (int)p.x - tw / 2, ly = (int)p.y - th - 4;
    if (lx + tw > w - 1) lx = w - 1 - tw;
    if (lx < 1) lx = 1;
    if (ly + th > h - 1) ly = h - 1 - th;
    if (ly < 1) ly = 1;
    UiPill(text, lx, ly, VOLUME_COLOR, (Color){ 10, 24, 18, 255 });
}

// The tab of volume `v` at the middle of its top face, if that is in front of the camera
static void VolumeTab(BoundingBox v, const char *text, Camera3D cam, int w, int h)
{
    Vector3 top = { (v.min.x + v.max.x) * 0.5f, v.max.y, (v.min.z + v.max.z) * 0.5f };
    Vector3 fwd = Vector3Subtract(cam.target, cam.position);
    if (Vector3DotProduct(Vector3Subtract(top, cam.position), fwd) <= 0.0f) return;
    DrawVolumeTab(GetWorldToScreenEx(top, cam, w, h), text, w, h);
}

void DesignerDrawVolumes(const Designer *d, Camera3D cam, int w, int h, bool marked)
{
    bool live = !marked && d->volumeMode;
    bool making = live && d->volumeStep != VOLUME_STEP_FIRST;
    bool aim = live && d->volumeStep != VOLUME_STEP_HEIGHT && !ObjIsNone(d->hit.obj);
    if (d->volumeCount == 0 && !live) return;
    BoundingBox draft = making ? VolumeDraft(d) : (BoundingBox){ 0 };
    // The active volume stands out only while it can be edited
    int active = live ? d->activeVolume : -1;

    BeginOverlay3D(cam);
    // Volumes are often behind walls (that is what they are for): a faint outline on top of
    // everything first, then, depth-tested, translucent faces and the strong outline
    rlDisableDepthTest();
    for (int i = 0; i < d->volumeCount; i++) DrawBoundingBox(d->volumes[i].box, Fade(VOLUME_COLOR, i == active ? 0.5f : 0.3f));
    if (making) DrawBoundingBox(draft, Fade(VOLUME_COLOR, 0.5f));
    rlDrawRenderBatchActive();
    rlEnableDepthTest();
    rlDisableDepthMask();
    for (int i = 0; i < d->volumeCount; i++) {
        BoundingBox v = d->volumes[i].box;
        DrawCubeV(Vector3Scale(Vector3Add(v.min, v.max), 0.5f), Vector3Subtract(v.max, v.min), Fade(VOLUME_COLOR, i == active ? 0.12f : 0.07f));
    }
    if (making) DrawCubeV(Vector3Scale(Vector3Add(draft.min, draft.max), 0.5f), Vector3Subtract(draft.max, draft.min), Fade(VOLUME_COLOR, 0.12f));
    rlDrawRenderBatchActive();
    for (int i = 0; i < d->volumeCount; i++)
        DrawBoundingBox(d->volumes[i].box, Fade(VOLUME_COLOR, active < 0 || i == active ? 1.0f : 0.6f));
    if (making) DrawBoundingBox(draft, VOLUME_COLOR);
    if (aim) {
        // Where a click would put the corner (snapped)
        Vector3 p = VolumePoint(d);
        float s = 0.012f * Vector3Distance(cam.position, p) + 0.03f;
        DrawLine3D((Vector3){ p.x - s, p.y, p.z }, (Vector3){ p.x + s, p.y, p.z }, VOLUME_COLOR);
        DrawLine3D((Vector3){ p.x, p.y - s, p.z }, (Vector3){ p.x, p.y + s, p.z }, VOLUME_COLOR);
        DrawLine3D((Vector3){ p.x, p.y, p.z - s }, (Vector3){ p.x, p.y, p.z + s }, VOLUME_COLOR);
    }
    EndOverlay3D();

    char text[96];
    for (int i = 0; i < d->volumeCount; i++) {
        BoundingBox v = d->volumes[i].box;
        if (i == active) snprintf(text, sizeof(text), "V%d   %.2f x %.2f x %.2f m", i + 1, v.max.x - v.min.x, v.max.y - v.min.y, v.max.z - v.min.z);
        else snprintf(text, sizeof(text), "V%d", i + 1);
        VolumeTab(v, text, cam, w, h);
    }
    if (making) {
        snprintf(text, sizeof(text), "V%d   %.2f x %.2f x %.2f m", d->volumeCount + 1, draft.max.x - draft.min.x,
                 draft.max.y - draft.min.y, draft.max.z - draft.min.z);
        VolumeTab(draft, text, cam, w, h);
    }

    if (live && !d->promptOpen) {
        const char *help;
        if (d->volumeStep == VOLUME_STEP_SECOND)
            help = "{LMB} opposite footprint corner|{CTRL} no snap|{RMB} / {ESC} cancel";
        else if (d->volumeStep == VOLUME_STEP_HEIGHT)
            help = "mouse up / down or {wheel} height ({SHIFT} x4)|{CTRL} no snap|{LMB} / {ENTER} done|{RMB} / {ESC} cancel";
        else
            help = TextFormat("{LMB} first corner|{[} {]} active|arrows, {PGUP} / {PGDN} move ({ALT} grow, {ALT+SHIFT} shrink)|"
                              "{N} snap %g m|{DEL} / {SHIFT+DEL} remove / clear|{ENTER} request|{V} / {ESC} leave   (%d volume%s)",
                              VolumeSnap(d), d->volumeCount, d->volumeCount == 1 ? "" : "s");
        DrawHelpBar("VOLUME", VOLUME_COLOR, help, w, h);
    }
}

void DesignerDrawCrosshair(int w, int h)
{
    int cx = w / 2, cy = h / 2;
    Color shade = { 0, 0, 0, 120 }, ink = { 240, 240, 235, 220 };
    // Dark rim first so it reads on bright concrete and on dark fog alike
    DrawRectangle(cx - 9, cy - 2, 7, 4, shade);
    DrawRectangle(cx + 3, cy - 2, 7, 4, shade);
    DrawRectangle(cx - 2, cy - 9, 4, 7, shade);
    DrawRectangle(cx - 2, cy + 3, 4, 7, shade);
    DrawRectangle(cx - 8, cy - 1, 5, 2, ink);
    DrawRectangle(cx + 4, cy - 1, 5, 2, ink);
    DrawRectangle(cx - 1, cy - 8, 2, 5, ink);
    DrawRectangle(cx - 1, cy + 4, 2, 5, ink);
    DrawRectangle(cx - 1, cy - 1, 2, 2, ink);
}

// HUD line on a dark backdrop: these lines are often over bright fog
static void HudText(const char *text, int x, int y, Color c)
{
    UiHudText(text, x, y, UI_HUD_LINE, UI_S, c);
}

int DesignerDrawHud(const Designer *d, int x, int y)
{
    Color main = UI_TEXT, dim = UI_DIM;
    int y0 = y;

    if (d->selectedCount == 0) {
        HudText("selected: none", x, y, dim);
    } else {
        // Distinct group names, in selection order
        static char text[512];
        int models = 0;
        for (int i = 0; i < d->selectedCount; i++) models += d->selected[i].kind == MS_OBJ_MODEL;
        int boxes = d->selectedCount - models, n;
        if (models == 0) n = snprintf(text, sizeof(text), "selected: %d box%s  [", boxes, boxes == 1 ? "" : "es");
        else n = snprintf(text, sizeof(text), "selected: %d box%s, %d model%s  [", boxes, boxes == 1 ? "" : "es",
                          models, models == 1 ? "" : "s");
        unsigned char *seen = MS_RESERVE(scratch.seen, scratch.seenCap, scene->groupCount + 1) ? scratch.seen : NULL;
        if (!seen) return 0;
        if (scene->groupCount > 0) memset(seen, 0, scene->groupCount);
        int shown = 0, more = 0;
        bool ungrouped = false;
        for (int i = 0; i < d->selectedCount; i++) {
            int g = MS_ObjectRef(scene, d->selected[i])->group;
            if (g < 0) { ungrouped = true; continue; }
            if (seen[g]) continue;
            seen[g] = true;
            if (shown < 5 && n < (int)sizeof(text) - 80) {
                n += snprintf(text + n, sizeof(text) - n, "%s%s", shown ? ", " : "", scene->groups[g]);
                shown++;
            } else more++;
        }
        if (ungrouped && n < (int)sizeof(text) - 40) n += snprintf(text + n, sizeof(text) - n, "%s(no group)", shown ? ", " : "");
        if (more && n < (int)sizeof(text) - 40) n += snprintf(text + n, sizeof(text) - n, " +%d more", more);
        snprintf(text + n, sizeof(text) - n, "]");
        HudText(text, x, y, Fade(SEL_COLOR, 0.95f));
    }
    y += 20;

    if (!ObjIsNone(d->hit.obj)) {
        MS_Ref r = *MS_ObjectRef(scene, d->hit.obj);
        const char *group = r.group >= 0 ? scene->groups[r.group] : "(no group)";
        char idText[48];
        MapEditIdText(d->hit.obj, idText, sizeof(idText));
        if (d->hit.obj.kind == MS_OBJ_MODEL) {
            const MS_ModelInstance *m = &scene->models[d->hit.obj.index];
            HudText(TextFormat("crosshair: %s  |  model %s  |  material %s  |  line %d  |  %s  |  model %d  |  %.1f m", group,
                    scene->modelFiles[m->file].path, DesignerMaterialLabel(d->hit.obj), r.line, idText, d->hit.obj.index,
                    d->hit.dist), x, y, main);
            y += 20;
            HudText(TextFormat("  at %.2f %.2f %.2f  rot %.1f %.1f %.1f  scale %.2f %.2f %.2f", m->position.x, m->position.y,
                    m->position.z, m->rotationDeg.x, m->rotationDeg.y, m->rotationDeg.z, m->scale.x, m->scale.y, m->scale.z),
                    x, y, dim);
        } else
            HudText(TextFormat("crosshair: %s  |  material %s  |  line %d  |  %s  |  box %d  |  %.1f m",
                    group, DesignerMaterialLabel(d->hit.obj), r.line, idText, d->hit.obj.index, d->hit.dist),
                    x, y, main);
        y += 20;
        char line[128];     // (cut below anyway)
        if (r.line > 0 && MS_GetLine(scene, r.line, line, sizeof(line))) {
            // Long lines are cut to keep the HUD on screen
            if (strlen(line) > 110) strcpy(line + 107, "...");
            HudText(TextFormat("  %d: %s", r.line, line), x, y, dim);
            y += 20;
        }
    } else {
        HudText("crosshair: sky", x, y, dim);
        y += 20;
    }

    if (d->volumeCount > 0 || d->volumeMode) {
        char text[256];
        int n = snprintf(text, sizeof(text), "volumes: %d", d->volumeCount);
        if (d->activeVolume >= 0 && d->activeVolume < d->volumeCount) {
            BoundingBox v = d->volumes[d->activeVolume].box;
            snprintf(text + n, sizeof(text) - n, "  |  V%d min %.2f %.2f %.2f  max %.2f %.2f %.2f%s", d->activeVolume + 1,
                     v.min.x, v.min.y, v.min.z, v.max.x, v.max.y, v.max.z, d->volumeMode ? "  (arrows edit it)" : "");
        }
        HudText(text, x, y, Fade(VOLUME_COLOR, 0.95f));
        y += 20;
    }
    return y - y0;
}

// Darkens everything outside the rectangles (whole pixels): horizontal bands between the
// rectangles' y edges, and in each band the gaps between the rectangles spanning it
static void DimOutside(const Rectangle *rects, int n, int w, int h, Color c)
{
    if (!MS_RESERVE(scratch.ys, scratch.ysCap, 2 * n + 2) || !MS_RESERVE(scratch.xs, scratch.xsCap, n)) return;
    float *ys = scratch.ys;
    Vector2 *xs = scratch.xs;       // x = left, y = right edge of each rectangle in a band
    int ny = 0;
    ys[ny++] = 0.0f;
    ys[ny++] = (float)h;
    for (int i = 0; i < n; i++) { ys[ny++] = rects[i].y; ys[ny++] = rects[i].y + rects[i].height; }
    for (int i = 1; i < ny; i++)    // insertion sort, a few dozen values at most
        for (int j = i; j > 0 && ys[j] < ys[j - 1]; j--) { float t = ys[j]; ys[j] = ys[j - 1]; ys[j - 1] = t; }

    for (int b = 0; b + 1 < ny; b++) {
        float y0 = ys[b], y1 = ys[b + 1];
        if (y1 <= y0) continue;
        int nx = 0;
        for (int i = 0; i < n; i++) {
            if (rects[i].y > y0 || rects[i].y + rects[i].height < y1) continue;
            xs[nx] = (Vector2){ rects[i].x, rects[i].x + rects[i].width };
            for (int j = nx++; j > 0 && xs[j].x < xs[j - 1].x; j--) { Vector2 t = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = t; }
        }
        float x = 0.0f;
        for (int i = 0; i < nx; i++) {
            if (xs[i].x > x) DrawRectangleRec((Rectangle){ x, y0, xs[i].x - x, y1 - y0 }, c);
            x = fmaxf(x, xs[i].y);
        }
        if (x < w) DrawRectangleRec((Rectangle){ x, y0, w - x, y1 - y0 }, c);
    }
}

// Outline with a dark rim (reads on bright fog and in shade alike) and a label tab with
// the number (0 = none) and, if asked, the pixel size
static void DrawRegionFrame(Rectangle r, int number, bool size, int w)
{
    DrawRectangleLinesEx((Rectangle){ r.x - 1, r.y - 1, r.width + 2, r.height + 2 }, 1.0f, (Color){ 0, 0, 0, 170 });
    DrawRectangleLinesEx((Rectangle){ r.x + 2, r.y + 2, r.width - 4, r.height - 4 }, 1.0f, (Color){ 0, 0, 0, 90 });
    DrawRectangleLinesEx(r, 2.0f, REGION_COLOR);
    char text[64];
    if (number > 0 && size) snprintf(text, sizeof(text), "%d   %.0f x %.0f", number, r.width, r.height);
    else if (number > 0) snprintf(text, sizeof(text), "%d", number);
    else snprintf(text, sizeof(text), "%.0f x %.0f", r.width, r.height);
    int tw = UiPillWidth(text), th = UI_PILL_H;
    // Above the top-left corner, or inside the rect at the top of the screen; kept on screen
    int lx = (int)r.x, ly = r.y >= th + 4 ? (int)r.y - th - 4 : (int)r.y + 4;
    if (lx + tw > w - 1) lx = w - 1 - tw;
    if (lx < 1) lx = 1;
    UiPill(text, lx, ly, REGION_COLOR, (Color){ 20, 10, 20, 255 });
}

void DesignerDrawRegions(const Designer *d, int w, int h, bool marked)
{
    if (marked) {
        for (int i = 0; i < d->regionCount; i++) DrawRegionFrame(d->regions[i].rect, i + 1, false, w);
        return;
    }
    if (!d->snipping) return;

    // A plain drag replaces the regions, so while it runs only the new one shows
    if (!MS_RESERVE(scratch.rects, scratch.rectsCap, d->regionCount + 1)) return;
    Rectangle *rects = scratch.rects;
    int n = 0;
    bool replacing = d->dragging && !d->dragAdd;
    if (!replacing) for (int i = 0; i < d->regionCount; i++) rects[n++] = d->regions[i].rect;
    Rectangle drag = d->dragging ? DragRect(d) : (Rectangle){ 0 };
    if (d->dragging) rects[n++] = drag;

    DimOutside(rects, n, w, h, (Color){ 0, 0, 0, 120 });
    if (!replacing)
        for (int i = 0; i < d->regionCount; i++) DrawRegionFrame(d->regions[i].rect, i + 1, true, w);
    if (d->dragging && drag.width > 0.0f && drag.height > 0.0f) {
        int number = replacing ? 1 : d->regionCount + 1;
        DrawRegionFrame(drag, number, true, w);
    }

    if (!d->promptOpen) {
        const char *help = TextFormat("drag new region|{SHIFT}+drag add|{ENTER} request|{RMB} / {ESC} / {C} cancel"
                                      "   (%d region%s)", d->regionCount, d->regionCount == 1 ? "" : "s");
        DrawHelpBar("SNIP", REGION_COLOR, help, w, h);
    }
}

void DesignerDrawPrompt(Designer *d, int w, int h)
{
    int msgY = h - 100;
    if (d->promptOpen) {
        float maxW = PromptTextWidth(w);
        int lines = PromptLayout(d, maxW);
        int cursorLine = CursorLine(lines, d->cursor);
        int visible = lines < PROMPT_MIN_LINES ? PROMPT_MIN_LINES : lines > PROMPT_MAX_LINES ? PROMPT_MAX_LINES : lines;
        if (cursorLine < d->scroll) d->scroll = cursorLine;
        if (cursorLine >= d->scroll + visible) d->scroll = cursorLine - visible + 1;
        if (d->scroll > lines - visible) d->scroll = lines - visible > 0 ? lines - visible : 0;

        int boxH = PROMPT_HEADER_H + visible * PROMPT_LINE_H + 2 * PROMPT_PAD;
        Rectangle box = { PROMPT_MARGIN, (float)(h - PROMPT_MARGIN - boxH), (float)(w - 2 * PROMPT_MARGIN), (float)boxH };
        UiPanel(box, UI_PANEL, UI_LINE);

        int tx = (int)box.x + PROMPT_PAD, ty = (int)box.y + PROMPT_PAD;
        char regions[64] = "";
        int rn = 0;
        if (d->regionCount > 0) rn = snprintf(regions, sizeof(regions), " + %d region%s", d->regionCount, d->regionCount == 1 ? "" : "s");
        if (d->volumeCount > 0) snprintf(regions + rn, sizeof(regions) - rn, " + %d volume%s", d->volumeCount, d->volumeCount == 1 ? "" : "s");
        int hy = ty + (PROMPT_HEADER_H - 8 - UiLineHeight(UI_S)) / 2;
        UiTextBold("Request for Claude", tx, hy, UI_S, UI_INK);
        UiHints(TextFormat("(%d selected%s%s)|{Enter} send|{Shift+Enter} new line|{Ctrl+V} paste|{Esc} %s",
                           d->selectedCount, !ObjIsNone(d->hit.obj) ? " + crosshair point" : "", regions,
                           d->snipping ? "back to snip" : "cancel"),
                tx + UiMeasureBold("Request for Claude", UI_S) + 12, hy, UI_S, UI_DIM);
        const char *counter = TextFormat("%d bytes", d->promptLen);
        Color counterColor = d->promptLen >= PROMPT_LIMIT - 1 ? UI_ERROR : UI_DIM;
        UiText(counter, (int)(box.x + box.width) - PROMPT_PAD - UiMeasure(counter, UI_S), hy, UI_S, counterColor);
        DrawRectangle(tx, ty + PROMPT_HEADER_H - 5, (int)box.width - 2 * PROMPT_PAD, 1, UI_LINE);
        ty += PROMPT_HEADER_H;

        Color ink = UI_INK;
        if (d->promptLen == 0) {
            UiText(d->regionCount > 0 || d->volumeCount > 0
                   ? "Describe the change. The crosshair point, the selected boxes, the screen regions and the volumes are sent along."
                   : "Describe the change. The crosshair point and the selected boxes are sent along.",
                   tx, ty, PROMPT_FONT, UI_FAINT);
        }
        for (int i = d->scroll; i < lines && i < d->scroll + visible; i++) {
            PromptLine l = promptLines[i];
            int n = l.end - l.start;
            if (!MS_RESERVE(scratch.line, scratch.lineCap, n + 1)) break;
            char *buf = scratch.line;
            if (n > 0) memcpy(buf, d->prompt + l.start, n);
            buf[n] = 0;
            int ly = ty + (i - d->scroll) * PROMPT_LINE_H;
            UiText(buf, tx, ly, PROMPT_FONT, ink);
            if (i == cursorLine && fmod(GetTime(), 1.0) < 0.6) {
                float cx = tx + TextSpanWidth(d->prompt, l.start, d->cursor);
                DrawRectangle((int)cx, ly, 2, UiLineHeight(PROMPT_FONT), UI_CURSOR);
            }
        }
        // Scroll hints
        if (d->scroll > 0) UiText("^", (int)(box.x + box.width) - 18, ty, UI_S, ink);
        if (d->scroll + visible < lines) UiText("v", (int)(box.x + box.width) - 18, ty + (visible - 1) * PROMPT_LINE_H, UI_S, ink);
        msgY = (int)box.y - 34;
    }

    if (d->messageTime > 0.0f) {
        UiToast(d->message, w / 2, msgY, UI_L, d->messageError ? UI_ERROR : UI_INK, fminf(d->messageTime, 1.0f));
    }
}

// #endregion

void DesignerFree(Designer *d)
{
    DesignerClearSelection(d);
    free(d->selected);
    free(d->keys);
    for (int k = 0; k < 3; k++) free(d->selPos[k]);
    free(d->regions);
    free(d->volumes);
    free(d->prompt);
    d->selected = NULL;
    d->keys = NULL;
    d->regions = NULL;
    d->volumes = NULL;
    d->volumeCount = d->volumeCap = 0;
    d->prompt = NULL;
    d->selectedCap = d->keyCap = d->regionCount = d->regionCap = d->promptLen = d->promptCap = d->cursor = 0;
    for (int k = 0; k < 3; k++) { d->selPos[k] = NULL; d->selPosCap[k] = 0; }
    // The file-level scratch (one designer per process)
    free(scratch.seen);
    free(scratch.ys);
    free(scratch.xs);
    free(scratch.rects);
    free(scratch.line);
    memset(&scratch, 0, sizeof(scratch));
    free(promptLines);
    promptLines = NULL;
    promptLineCap = 0;
    free(boxDist);
    free(modelDist);
    boxDist = modelDist = NULL;
    boxDistCap = modelDistCap = 0;
}
