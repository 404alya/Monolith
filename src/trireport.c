// --tri-report: the triangles of a loaded map, counted as the runtime draws them (a merged
// material: only the faces no other box hides, MS_BoxTriangles; a model: its file's triangles per
// instance), so a map edit can be checked against a triangle budget. Format (version line first,
// sections most triangles first, distances in metres):
//   monolith-tri-report 1
//   map "<path>"
//   total boxes <n> box_triangles <t> models <n> model_triangles <t> triangles <t> ranges <n>
//   detail_distance <x> factor <MS_DETAIL_FACTOR>
//   viewpoint <i> "<name>" drawn <t> of <t> (<p>%) far_ranges <n>
//       (the triangles of the draw ranges within their draw distance of its eye; distance only,
//       no frustum: what the camera could draw from there)
//   group "<name>" boxes <n> models <n> triangles <t> (<p>%)
//   material "<name>" boxes <n> triangles <t> (<p>%) draw_distance <auto|off|m> ranges <n> distance <min>..<max>
//   line <l> boxes <n> models <n> triangles <t> group "<name>" material "<name>"     (the first 60)
//   range <r> <merged|instanced|models> "<material or model file>" objects <n> triangles <t>
//         distance <m|always> bounds <x0 y0 z0 x1 y1 z1>       (distance: at detail_distance 1)
#include "trireport.h"
#include <math.h>

// One row of a section: a name's counts
typedef struct { long long tris; int boxes, models, ranges; float dMin, dMax; } Count;

static const Count *sortRows;
static int ByTrianglesDesc(const void *a, const void *b)
{
    long long x = sortRows[*(const int *)a].tris, y = sortRows[*(const int *)b].tris;
    return x < y ? 1 : x > y ? -1 : (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b);
}

// The indices 0..n-1 of rows, most triangles first (ties: index order); NULL when out of memory
static int *SortedRows(const Count *rows, int n)
{
    int *order = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!order) return NULL;
    for (int i = 0; i < n; i++) order[i] = i;
    sortRows = rows;
    qsort(order, (size_t)n, sizeof(int), ByTrianglesDesc);
    return order;
}

static float Pct(long long part, long long whole) { return whole > 0 ? 100.0f * (float)part / (float)whole : 0.0f; }

// Distance from p to the nearest point of b
static float BoxDistance(Vector3 p, BoundingBox b)
{
    return Vector3Distance(p, Vector3Clamp(p, b.min, b.max));
}

bool WriteTriReport(const MS_Scene *s, const char *path, float detailDistance)
{
    int nr = MS_DrawRangeCount(s);
    int maxLine = 0;
    for (int i = 0; i < s->boxCount; i++) if (s->boxRefs[i].line > maxLine) maxLine = s->boxRefs[i].line;
    for (int i = 0; i < s->modelCount; i++) if (s->models[i].ref.line > maxLine) maxLine = s->models[i].ref.line;
    // groups: [groupCount] = no group
    Count *groups = calloc((size_t)s->groupCount + 1, sizeof(Count));
    Count *mats = calloc((size_t)s->materialCount + 1, sizeof(Count));
    Count *lines = calloc((size_t)maxLine + 1, sizeof(Count));
    Count *ranges = calloc((size_t)(nr > 0 ? nr : 1), sizeof(Count));
    int *lineGroup = calloc((size_t)maxLine + 1, sizeof(int)), *lineMat = calloc((size_t)maxLine + 1, sizeof(int));
    MS_DrawRangeInfo *info = calloc((size_t)(nr > 0 ? nr : 1), sizeof(MS_DrawRangeInfo));
    FILE *f = NULL;
    bool ok = groups && mats && lines && ranges && lineGroup && lineMat && info;
    for (int r = 0; ok && r < nr; r++) ok = MS_GetDrawRange(s, r, &info[r]);
    if (ok) f = fopen(path, "w");
    ok = ok && f;

    // Per object into its group, material and line; per range its triangles and distance
    long long boxTris = 0, modelTris = 0, rangeTris = 0;
    for (int i = 0; ok && i < s->boxCount; i++) {
        const MS_Ref *R = &s->boxRefs[i];
        long long t = MS_BoxTriangles(s, i);
        int g = R->group >= 0 && R->group < s->groupCount ? R->group : s->groupCount;
        groups[g].tris += t; groups[g].boxes++;
        if (R->material >= 0 && R->material < s->materialCount) { mats[R->material].tris += t; mats[R->material].boxes++; }
        if (R->line > 0) { lines[R->line].tris += t; lines[R->line].boxes++; lineGroup[R->line] = g; lineMat[R->line] = R->material; }
        boxTris += t;
    }
    for (int i = 0; ok && i < s->modelCount; i++) {
        const MS_ModelInstance *M = &s->models[i];
        long long t = s->modelFiles[M->file].triangleCount;
        int g = M->ref.group >= 0 && M->ref.group < s->groupCount ? M->ref.group : s->groupCount;
        groups[g].tris += t; groups[g].models++;
        if (M->ref.line > 0) { lines[M->ref.line].tris += t; lines[M->ref.line].models++; lineGroup[M->ref.line] = g; lineMat[M->ref.line] = -1; }
        modelTris += t;
    }
    for (int m = 0; m < s->materialCount && mats; m++) { mats[m].dMin = INFINITY; mats[m].dMax = 0.0f; }
    for (int r = 0; ok && r < nr; r++) {
        ranges[r].tris = info[r].triangles;
        rangeTris += info[r].triangles;
        int m = info[r].kind != MS_DRAW_MODELS ? info[r].material : -1;
        if (m >= 0 && m < s->materialCount) {
            mats[m].ranges++;
            mats[m].dMin = fminf(mats[m].dMin, info[r].drawDistance);
            mats[m].dMax = fmaxf(mats[m].dMax, info[r].drawDistance);
        }
    }
    long long total = boxTris + modelTris;

    if (ok) {
        fprintf(f, "monolith-tri-report 1\nmap \"%s\"\n", s->path);
        fprintf(f, "total boxes %d box_triangles %lld models %d model_triangles %lld triangles %lld ranges %d\n",
                s->boxCount, boxTris, s->modelCount, modelTris, total, nr);
        // (the ranges' sum is the same count: a mismatch means this report is wrong)
        if (rangeTris != total) fprintf(f, "warning: the draw ranges hold %lld triangles\n", rangeTris);
        fprintf(f, "detail_distance %g factor %g\n", detailDistance, MS_DETAIL_FACTOR);

        for (int v = 0; v < s->viewpointCount; v++) {
            const MS_Viewpoint *V = &s->viewpoints[v];
            long long drawn = 0;
            int far = 0;
            for (int r = 0; r < nr; r++) {
                float limit = info[r].drawDistance * detailDistance;
                bool near = !(detailDistance > 0.0f) || !isfinite(limit) || BoxDistance(V->eye, info[r].bounds) <= limit;
                if (near) drawn += info[r].triangles;
                else far++;
            }
            fprintf(f, "viewpoint %d \"%s\" drawn %lld of %lld (%.1f%%) far_ranges %d\n", v + 1, V->name, drawn, rangeTris,
                    Pct(drawn, rangeTris), far);
        }

        int *order = SortedRows(groups, s->groupCount + 1);
        ok = order != NULL;
        for (int k = 0; ok && k <= s->groupCount; k++) {
            int g = order[k];
            if (groups[g].boxes + groups[g].models == 0) continue;
            fprintf(f, "group \"%s\" boxes %d models %d triangles %lld (%.1f%%)\n", g < s->groupCount ? s->groups[g] : "",
                    groups[g].boxes, groups[g].models, groups[g].tris, Pct(groups[g].tris, total));
        }
        free(order);

        order = ok ? SortedRows(mats, s->materialCount) : NULL;
        ok = order != NULL;
        for (int k = 0; ok && k < s->materialCount; k++) {
            int m = order[k];
            const Count *C = &mats[m];
            if (C->boxes == 0) continue;
            float dd = s->materials[m].drawDistance;
            fprintf(f, "material \"%s\" boxes %d triangles %lld (%.1f%%) draw_distance ", s->materials[m].name, C->boxes,
                    C->tris, Pct(C->tris, total));
            if (dd < 0.0f) fprintf(f, "off");
            else if (dd > 0.0f) fprintf(f, "%g", dd);
            else fprintf(f, "auto");
            fprintf(f, " ranges %d distance %.0f..%.0f\n", C->ranges, C->ranges ? C->dMin : 0.0f, C->ranges ? C->dMax : 0.0f);
        }
        free(order);

        order = ok ? SortedRows(lines, maxLine + 1) : NULL;
        ok = order != NULL;
        for (int k = 0; ok && k < 60 && k <= maxLine; k++) {
            int l = order[k];
            if (lines[l].boxes + lines[l].models == 0) break;
            int g = lineGroup[l], m = lineMat[l];
            fprintf(f, "line %d boxes %d models %d triangles %lld group \"%s\" material \"%s\"\n", l, lines[l].boxes,
                    lines[l].models, lines[l].tris, g < s->groupCount ? s->groups[g] : "",
                    m >= 0 && m < s->materialCount ? s->materials[m].name : "");
        }
        free(order);

        order = ok ? SortedRows(ranges, nr) : NULL;
        ok = order != NULL;
        static const char *const KIND[] = { "merged", "instanced", "models" };
        for (int k = 0; ok && k < nr; k++) {
            const MS_DrawRangeInfo *I = &info[order[k]];
            const char *name = I->kind == MS_DRAW_MODELS ? s->modelFiles[I->modelFile].path : s->materials[I->material].name;
            BoundingBox b = I->bounds;
            fprintf(f, "range %d %s \"%s\" objects %d triangles %lld distance ", order[k], KIND[I->kind], name,
                    I->objectCount, I->triangles);
            if (isfinite(I->drawDistance)) fprintf(f, "%.0f", I->drawDistance);
            else fprintf(f, "always");
            fprintf(f, " bounds %.2f %.2f %.2f %.2f %.2f %.2f\n", b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
        }
        free(order);
    }

    bool written = f && fclose(f) == 0;
    free(groups); free(mats); free(lines); free(ranges); free(lineGroup); free(lineMat); free(info);
    return ok && written;
}
