// --dump-scene: everything a map produces, one item per line, so map diffs stay checkable
// (diff two dumps). Floats that come from geometry are C hex floats (%a, exact); a few
// human-readable values follow in decimal after a '#'. Format (version line first):
//   monolith-scene-dump 1
//   map "<path>" format <n>
//   counts boxes <n> models <n> modelFiles <n> materials <n> lights <n> groups <n> colliders <n>
//   box <i> <minx miny minz maxx maxy maxz>  line <l> group <g> material <m>
//   modelfile <i> "<path>" loaded <yes|no> meshes <n> triangles <n> materials <n> bounds <6 hex>
//   model <i> file <f> line <l> group <g> material <m|-1> origin <o> collide <c> shadow <on|off>
//         position <3 hex> rotationDeg <3 hex> scale <3 hex> translation <3 hex> rotation <4 hex>
//         bounds <6 hex> colliders <first> <count> slots <N=m,...|->
//   collider <i> <6 hex>        (model colliders only; i >= boxes)
//   material <i> "<name>" line <l> shader "<path>" unlit <on|off>
//   light <i> <point|spot|dir> line <l> position <3 hex> direction <3 hex> color r g b energy <hex> range <hex> shadow <on|off>
//   sun direction <3 hex> energy <hex> range <hex> shadow <on|off>
#include "dumpscene.h"

static void Hex3(FILE *f, Vector3 v) { fprintf(f, " %a %a %a", v.x, v.y, v.z); }
static void HexBox(FILE *f, BoundingBox b) { Hex3(f, b.min); Hex3(f, b.max); }

bool DumpScene(const MS_Scene *s, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    static const char *const ORIGIN[] = { "model", "bottom", "center" };
    static const char *const COLLIDE[] = { "aabb", "none", "box" };
    static const char *const LIGHT[] = { "dir", "spot", "point" };
    fprintf(f, "monolith-scene-dump 1\nmap \"%s\" format %d\n", s->path, s->formatVersion);
    fprintf(f, "counts boxes %d models %d modelFiles %d materials %d lights %d groups %d colliders %d\n", s->boxCount,
            s->modelCount, s->modelFileCount, s->materialCount, s->lightCount, s->groupCount, s->colliderCount);
    for (int i = 0; i < s->groupCount; i++) fprintf(f, "group %d \"%s\"\n", i, s->groups[i]);
    for (int i = 0; i < s->boxCount; i++) {
        fprintf(f, "box %d", i);
        HexBox(f, s->boxes[i]);
        fprintf(f, " line %d group %d material %d\n", s->boxRefs[i].line, s->boxRefs[i].group, s->boxRefs[i].material);
    }
    for (int i = 0; i < s->modelFileCount; i++) {
        const MS_ModelFile *F = &s->modelFiles[i];
        fprintf(f, "modelfile %d \"%s\" loaded %s meshes %d triangles %d materials %d bounds", i, F->path,
                F->loaded ? "yes" : "no", F->meshCount, F->triangleCount, F->materialCount);
        HexBox(f, F->bounds);
        fprintf(f, "  # size %.4f %.4f %.4f\n", F->bounds.max.x - F->bounds.min.x, F->bounds.max.y - F->bounds.min.y,
                F->bounds.max.z - F->bounds.min.z);
    }
    for (int i = 0; i < s->modelCount; i++) {
        const MS_ModelInstance *M = &s->models[i];
        fprintf(f, "model %d file %d line %d group %d material %d origin %s collide %s shadow %s position", i, M->file,
                M->ref.line, M->ref.group, M->ref.material, ORIGIN[M->origin % 3], COLLIDE[M->collide % 3], M->castShadow ? "on" : "off");
        Hex3(f, M->position);
        fprintf(f, " rotationDeg"); Hex3(f, M->rotationDeg);
        fprintf(f, " scale"); Hex3(f, M->scale);
        fprintf(f, " translation"); Hex3(f, M->translation);
        fprintf(f, " rotation %a %a %a %a bounds", M->rotation.x, M->rotation.y, M->rotation.z, M->rotation.w);
        HexBox(f, M->bounds);
        fprintf(f, " colliders %d %d slots", M->colliderFirst, M->colliderCount);
        if (M->slotCount == 0) fprintf(f, " -");
        for (int k = 0; k < M->slotCount; k++)
            fprintf(f, "%s%d=%d", k ? "," : " ", s->modelSlots[M->slotFirst + k].slot, s->modelSlots[M->slotFirst + k].material);
        fprintf(f, "  # %s at %.3f %.3f %.3f, bounds %.3f %.3f %.3f .. %.3f %.3f %.3f\n", s->modelFiles[M->file].path,
                M->translation.x, M->translation.y, M->translation.z, M->bounds.min.x, M->bounds.min.y, M->bounds.min.z,
                M->bounds.max.x, M->bounds.max.y, M->bounds.max.z);
    }
    for (int i = s->boxCount; i < s->colliderCount; i++) {
        fprintf(f, "collider %d", i);
        HexBox(f, s->colliders[i]);
        fprintf(f, "\n");
    }
    for (int i = 0; i < s->materialCount; i++) {
        const MS_Material *m = &s->materials[i];
        fprintf(f, "material %d \"%s\" line %d shader \"%s\" unlit %s\n", i, m->name, m->line, m->shader,
                m->material.unlit ? "on" : "off");
    }
    for (int i = 0; i < s->lightCount; i++) {
        const R3D_Light *L = &s->lights[i].light;
        fprintf(f, "light %d %s line %d position", i, LIGHT[L->type % 3], s->lights[i].ref.line);
        Hex3(f, L->position);
        fprintf(f, " direction"); Hex3(f, L->direction);
        fprintf(f, " color %d %d %d energy %a range %a shadow %s\n", L->color.r, L->color.g, L->color.b, L->energy, L->range,
                s->lights[i].shadow ? "on" : "off");
    }
    fprintf(f, "sun direction");
    Hex3(f, s->sun.light.direction);
    fprintf(f, " energy %a range %a shadow %s enabled %s\n", s->sun.light.energy, s->sun.light.range,
            s->sun.shadow ? "on" : "off", s->sun.enabled ? "on" : "off");
    return fclose(f) == 0;
}
