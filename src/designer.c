#include "designer.h"
#include <math.h>

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

DesignerHit DesignerPick(Vector3 origin, Vector3 dir)
{
    DesignerHit h = PickBoxes(origin, dir, NULL, NULL, scene->boxCount);
    PickModels(origin, dir, NULL, NULL, scene->modelCount, &h);
    return h;
}

// Name of an object's material (the HUD, --pick and request bundles show it). Models
// without a whole-model override use the file's own materials ("(own)"), possibly with
// per-slot material.N overrides, which are listed: "(own; slot 0 = bark, slot 2 = needles)"
const char *DesignerMaterialLabel(MS_ObjRef o)
{
    const MS_Ref *r = MS_ObjectRef(scene, o);
    int m = r ? r->material : -1;
    if (m >= 0 && m < scene->materialCount) return scene->materials[m].name;
    if (o.kind != MS_OBJ_MODEL) return "default";
    const MS_ModelInstance *mi = &scene->models[o.index];
    if (mi->slotCount <= 0) return "(own)";
    static char buf[512];   // one label at a time; long lists are cut with "..."
    int n = snprintf(buf, sizeof(buf), "(own;");
    for (int i = 0; i < mi->slotCount && n < (int)sizeof(buf) - 8; i++) {
        const MS_SlotMaterial *sm = &scene->modelSlots[mi->slotFirst + i];
        const char *name = sm->material >= 0 && sm->material < scene->materialCount ? scene->materials[sm->material].name : "?";
        n += snprintf(buf + n, sizeof(buf) - n, "%s slot %d = %s", i ? "," : "", sm->slot, name);
    }
    if (n >= (int)sizeof(buf) - 8) snprintf(buf + sizeof(buf) - 8, 8, "...)");
    else snprintf(buf + n, sizeof(buf) - n, ")");
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
#define PROMPT_SPACING  2.0f
#define PROMPT_MARGIN   12      // box to screen edge
#define PROMPT_PAD      10      // text to box edge
#define PROMPT_LINE_H   24
#define PROMPT_HEADER_H 24
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

// Width of one codepoint as DrawTextEx advances it (glyph + spacing)
static float GlyphAdvance(int cp)
{
    Font f = GetFontDefault();
    float scale = (float)PROMPT_FONT / (float)f.baseSize;
    int idx = GetGlyphIndex(f, cp);
    float adv = f.glyphs[idx].advanceX ? (float)f.glyphs[idx].advanceX : f.recs[idx].width;
    return adv * scale + PROMPT_SPACING;
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
    return d->promptOpen || d->snipping || d->freezeFrames > 0;
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
    d->hit = DesignerPick(cam.position, Vector3Normalize(Vector3Subtract(cam.target, cam.position)));
}

DesignerAction DesignerUpdate(Designer *d, float dt)
{
    if (d->messageTime > 0.0f) d->messageTime -= dt;
    bool frozen = d->freezeFrames > 0;
    if (d->freezeFrames > 0) d->freezeFrames--;

    if (d->promptOpen) return PromptInput(d);
    if (d->snipping) return SnipInput(d);
    if (frozen) return DESIGNER_NONE;   // the frame(s) right after the box closed

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
    for (int i = 0; i < req->regionCount; i++) free(req->regions[i].objects);
    free(req->regions);
    memset(req, 0, sizeof(*req));
}

bool DesignerBuildRequest(const Designer *d, Camera3D cam, float yaw, float pitch,
                          int width, int height, DesignRequest *req)
{
    memset(req, 0, sizeof(*req));
    req->prompt = MS_StrNDup(d->prompt ? d->prompt : "", d->promptLen);
    req->selected = malloc(sizeof(MS_ObjRef) * (d->selectedCount > 0 ? d->selectedCount : 1));
    req->regions = calloc(d->regionCount > 0 ? d->regionCount : 1, sizeof(DesignRegionInfo));
    if (!req->prompt || !req->selected || !req->regions) {
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

void DesignerDrawOverlay(const Designer *d, Camera3D cam, bool hover)
{
    if (d->selectedCount == 0 && ObjIsNone(d->hit.obj)) return;
    Vector3 eye = cam.position;

    BeginMode3D(cam);
    // R3D_End copies its depth buffer into the target, and BeginMode3D builds the same
    // projection (same fovy, aspect and rlGetCullDistanceNear/Far), so the overlay can be
    // depth-tested against the scene. Shift the overlay's depth 4 steps of the 24-bit depth
    // buffer toward the camera (z_ndc -= bias): a constant step in depth-buffer units is
    // the precision the scene's depth has at every distance, so lines on faces don't
    // z-fight. With the 1 cm near plane one step is z^2 / 168 km, i.e. 0.6 mm at 10 m,
    // 13 cm at 150 m; more bias would let the overlay show through nearby walls far away.
    Matrix proj = rlGetMatrixProjection();
    const float bias = 4.0f / 16777216.0f * 2.0f;
    proj.m2 -= bias * proj.m3;
    proj.m6 -= bias * proj.m7;
    proj.m10 -= bias * proj.m11;
    proj.m14 -= bias * proj.m15;
    rlSetMatrixProjection(proj);

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
    rlDrawRenderBatchActive();

    // Back to what R3D_End leaves and raylib's 2D drawing expects
    rlEnableDepthMask();
    rlDisableDepthTest();
    EndMode3D();
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

// HUD text with a dark drop shadow: these lines are often over bright fog
static void HudText(const char *text, int x, int y, Color c)
{
    DrawText(text, x + 1, y + 1, 16, (Color){ 0, 0, 0, (unsigned char)(c.a / 2) });
    DrawText(text, x, y, 16, c);
}

int DesignerDrawHud(const Designer *d, int x, int y)
{
    Color main = { 230, 230, 230, 170 }, dim = { 230, 230, 230, 115 };
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
        if (d->hit.obj.kind == MS_OBJ_MODEL) {
            const MS_ModelInstance *m = &scene->models[d->hit.obj.index];
            HudText(TextFormat("crosshair: %s  |  model %s  |  material %s  |  line %d  |  model %d  |  %.1f m", group,
                    scene->modelFiles[m->file].path, DesignerMaterialLabel(d->hit.obj), r.line, d->hit.obj.index, d->hit.dist),
                    x, y, main);
            y += 20;
            HudText(TextFormat("  at %.2f %.2f %.2f  rot %.1f %.1f %.1f  scale %.2f %.2f %.2f", m->position.x, m->position.y,
                    m->position.z, m->rotationDeg.x, m->rotationDeg.y, m->rotationDeg.z, m->scale.x, m->scale.y, m->scale.z),
                    x, y, dim);
        } else
            HudText(TextFormat("crosshair: %s  |  material %s  |  line %d  |  box %d  |  %.1f m",
                    group, DesignerMaterialLabel(d->hit.obj), r.line, d->hit.obj.index, d->hit.dist),
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
    int tw = MeasureText(text, 16) + 8, th = 20;
    // Above the top-left corner, or inside the rect at the top of the screen; kept on screen
    int lx = (int)r.x, ly = r.y >= th ? (int)r.y - th : (int)r.y + 2;
    if (lx + tw > w) lx = w - tw;
    if (lx < 0) lx = 0;
    DrawRectangle(lx, ly, tw, th, REGION_COLOR);
    DrawText(text, lx + 4, ly + 2, 16, (Color){ 20, 10, 20, 255 });
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
        const char *help = TextFormat("SNIP   drag: new region  |  SHIFT+drag: add  |  ENTER: request  |  RMB / ESC / C: cancel"
                                      "   (%d region%s)", d->regionCount, d->regionCount == 1 ? "" : "s");
        int tw = MeasureText(help, 18);
        DrawRectangle((w - tw) / 2 - 10, h - 64, tw + 20, 28, (Color){ 16, 16, 18, 200 });
        DrawText(help, (w - tw) / 2, h - 59, 18, (Color){ 240, 238, 232, 230 });
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
        DrawRectangleRec(box, (Color){ 16, 16, 18, 225 });
        DrawRectangleLinesEx(box, 1.0f, (Color){ 230, 230, 230, 90 });

        int tx = (int)box.x + PROMPT_PAD, ty = (int)box.y + PROMPT_PAD;
        char regions[32] = "";
        if (d->regionCount > 0) snprintf(regions, sizeof(regions), " + %d region%s", d->regionCount, d->regionCount == 1 ? "" : "s");
        DrawText(TextFormat("Request for Claude   (%d selected%s%s)   Enter send  |  Shift+Enter new line  |  Ctrl+V paste  |  Esc %s",
                 d->selectedCount, !ObjIsNone(d->hit.obj) ? " + crosshair point" : "", regions,
                 d->snipping ? "back to snip" : "cancel"), tx, ty, 16, (Color){ 230, 230, 230, 140 });
        const char *counter = TextFormat("%d bytes", d->promptLen);
        Color counterColor = d->promptLen >= PROMPT_LIMIT - 1 ? (Color){ 255, 110, 90, 230 } : (Color){ 230, 230, 230, 140 };
        DrawText(counter, (int)(box.x + box.width) - PROMPT_PAD - MeasureText(counter, 16), ty, 16, counterColor);
        ty += PROMPT_HEADER_H;

        Font font = GetFontDefault();
        Color ink = { 240, 238, 232, 255 };
        if (d->promptLen == 0) {
            DrawTextEx(font, d->regionCount > 0
                       ? "Describe the change. The crosshair point, the selected boxes and the screen regions are sent along."
                       : "Describe the change. The crosshair point and the selected boxes are sent along.",
                       (Vector2){ (float)tx, (float)ty }, PROMPT_FONT, PROMPT_SPACING, (Color){ 240, 238, 232, 90 });
        }
        for (int i = d->scroll; i < lines && i < d->scroll + visible; i++) {
            PromptLine l = promptLines[i];
            int n = l.end - l.start;
            if (!MS_RESERVE(scratch.line, scratch.lineCap, n + 1)) break;
            char *buf = scratch.line;
            if (n > 0) memcpy(buf, d->prompt + l.start, n);
            buf[n] = 0;
            int ly = ty + (i - d->scroll) * PROMPT_LINE_H;
            DrawTextEx(font, buf, (Vector2){ (float)tx, (float)ly }, PROMPT_FONT, PROMPT_SPACING, ink);
            if (i == cursorLine && fmod(GetTime(), 1.0) < 0.6) {
                float cx = tx + TextSpanWidth(d->prompt, l.start, d->cursor);
                DrawRectangle((int)cx, ly - 1, 2, PROMPT_FONT + 2, (Color){ 255, 200, 120, 255 });
            }
        }
        // Scroll hints
        if (d->scroll > 0) DrawText("^", (int)(box.x + box.width) - 18, ty, 16, ink);
        if (d->scroll + visible < lines) DrawText("v", (int)(box.x + box.width) - 18, ty + (visible - 1) * PROMPT_LINE_H, 16, ink);
        msgY = (int)box.y - 28;
    }

    if (d->messageTime > 0.0f) {
        float a = fminf(d->messageTime, 1.0f);
        Color c = d->messageError ? (Color){ 255, 110, 90, 255 } : (Color){ 235, 232, 225, 255 };
        int mw = MeasureText(d->message, 20);
        DrawText(d->message, (w - mw) / 2, msgY, 20, Fade(c, a));
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
    free(d->prompt);
    d->selected = NULL;
    d->keys = NULL;
    d->regions = NULL;
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
