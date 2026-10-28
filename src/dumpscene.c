// --dump-scene: everything a map produces, one item per line, so map diffs stay checkable
// (diff two dumps). Floats that come from geometry are C hex floats (%a, exact); a few
// human-readable values follow in decimal after a '#'. Format (version line first):
//   monolith-scene-dump 1
//   map "<path>" format <n>
//   counts boxes <n> models <n> modelFiles <n> materials <n> lights <n> groups <n> colliders <n> [turned <n>]
//   box <i> <minx miny minz maxx maxy maxz>  line <l> group <g> material <m>
//       [rot <3 hex> center <3 hex> size <3 hex>]     (a turned box: its rotation and shape; the
//                                                      six numbers are then its world AABB)
//   modelfile <i> "<path>" loaded <yes|no> meshes <n> triangles <n> materials <n> bounds <6 hex>
//   model <i> file <f> line <l> group <g> material <m|-1> origin <o> collide <c> shadow <on|off>
//         position <3 hex> rotationDeg <3 hex> scale <3 hex> translation <3 hex> rotation <4 hex>
//         bounds <6 hex> colliders <first> <count> slots <N=m,...|->
//   collider <i> <6 hex>        (model colliders only; i >= boxes)
//   material <i> "<name>" line <l> shader "<path>" unlit <on|off>
//   light <i> <point|spot|dir> line <l> position <3 hex> direction <3 hex> color r g b energy <hex> range <hex> shadow <on|off> [clip <3 hex> <3 hex>]
//   sun direction <3 hex> energy <hex> range <hex> shadow <on|off>
//
// --dump-entities writes the entities (runtime/monolith_scene.h, "Entities"), map order:
//   monolith-entity-dump 1
//   map "<path>" entities <n>
//   entity <e> id <stmt as 8 hex digits>:<index> <explicit|implicit> <box|model|light> <object index>
//          line <l> copy <c> part <p> position <3 hex> rotationDeg <3 hex> scale <3 hex> model <file|-1>
//
// --dump-scene-diff writes the same items without anything that shifts when lines are
// inserted or removed above them: no object, group, material, file or collider indices and
// no map lines. References are by name instead (group "<name>", material "<name>" or -,
// file "<path>", slots N="<name>"), and a model lists only its collider count. So the diff
// of two such dumps shows only the objects that really changed (an inserted model is one
// added line, not a renumbering of every later row). First line: monolith-scene-dump 1 diff.
#include "dumpscene.h"

static void Hex3(FILE *f, Vector3 v) { fprintf(f, " %a %a %a", v.x, v.y, v.z); }
static void HexBox(FILE *f, BoundingBox b) { Hex3(f, b.min); Hex3(f, b.max); }

// Index prefix (" <i>") and "line <l> " only in the plain dump
static void Index(FILE *f, bool diff, int i) { if (!diff) fprintf(f, " %d", i); }
static void Line(FILE *f, bool diff, int line) { if (!diff) fprintf(f, " line %d", line); }

static void GroupRef(FILE *f, const MS_Scene *s, bool diff, int g)
{
    if (!diff) fprintf(f, " group %d", g);
    else fprintf(f, " group \"%s\"", g >= 0 && g < s->groupCount ? s->groups[g] : "");
}

static void MaterialRef(FILE *f, const MS_Scene *s, bool diff, const char *key, int m)
{
    if (!diff) fprintf(f, " %s %d", key, m);
    else if (m >= 0 && m < s->materialCount) fprintf(f, " %s \"%s\"", key, s->materials[m].name);
    else fprintf(f, " %s -", key);
}

bool DumpScene(const MS_Scene *s, const char *path, bool diff)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    static const char *const ORIGIN[] = { "model", "bottom", "center" };
    static const char *const COLLIDE[] = { "aabb", "none", "box" };
    static const char *const LIGHT[] = { "dir", "spot", "point" };
    fprintf(f, "monolith-scene-dump 1%s\nmap \"%s\" format %d\n", diff ? " diff" : "", s->path, s->formatVersion);
    fprintf(f, "counts boxes %d models %d modelFiles %d materials %d lights %d groups %d colliders %d", s->boxCount,
            s->modelCount, s->modelFileCount, s->materialCount, s->lightCount, s->groupCount, s->colliderCount);
    if (s->turnedBoxCount > 0) fprintf(f, " turned %d", s->turnedBoxCount);    // (only then: older dumps stay equal)
    fprintf(f, "\n");
    for (int i = 0; i < s->groupCount; i++) {
        fprintf(f, "group");
        Index(f, diff, i);
        fprintf(f, " \"%s\"\n", s->groups[i]);
    }
    for (int i = 0; i < s->boxCount; i++) {
        fprintf(f, "box");
        Index(f, diff, i);
        HexBox(f, s->boxes[i]);
        Line(f, diff, s->boxRefs[i].line);
        GroupRef(f, s, diff, s->boxRefs[i].group);
        MaterialRef(f, s, diff, "material", s->boxRefs[i].material);
        if (s->boxTurned && s->boxTurned[i] >= 0) {
            const MS_TurnedBox *t = &s->turnedBoxes[s->boxTurned[i]];
            fprintf(f, " rot");
            Hex3(f, t->rotationDeg);
            fprintf(f, " center");
            Hex3(f, t->center);
            fprintf(f, " size");
            Hex3(f, t->size);
        }
        fprintf(f, "\n");
    }
    for (int i = 0; i < s->modelFileCount; i++) {
        const MS_ModelFile *F = &s->modelFiles[i];
        fprintf(f, "modelfile");
        Index(f, diff, i);
        fprintf(f, " \"%s\" loaded %s meshes %d triangles %d materials %d bounds", F->path,
                F->loaded ? "yes" : "no", F->meshCount, F->triangleCount, F->materialCount);
        HexBox(f, F->bounds);
        fprintf(f, "  # size %.4f %.4f %.4f\n", F->bounds.max.x - F->bounds.min.x, F->bounds.max.y - F->bounds.min.y,
                F->bounds.max.z - F->bounds.min.z);
    }
    for (int i = 0; i < s->modelCount; i++) {
        const MS_ModelInstance *M = &s->models[i];
        fprintf(f, "model");
        Index(f, diff, i);
        if (diff) fprintf(f, " file \"%s\"", s->modelFiles[M->file].path);
        else fprintf(f, " file %d", M->file);
        Line(f, diff, M->ref.line);
        GroupRef(f, s, diff, M->ref.group);
        MaterialRef(f, s, diff, "material", M->ref.material);
        fprintf(f, " origin %s collide %s shadow %s position", ORIGIN[M->origin % 3], COLLIDE[M->collide % 3],
                M->castShadow ? "on" : "off");
        Hex3(f, M->position);
        fprintf(f, " rotationDeg"); Hex3(f, M->rotationDeg);
        fprintf(f, " scale"); Hex3(f, M->scale);
        fprintf(f, " translation"); Hex3(f, M->translation);
        fprintf(f, " rotation %a %a %a %a bounds", M->rotation.x, M->rotation.y, M->rotation.z, M->rotation.w);
        HexBox(f, M->bounds);
        if (diff) fprintf(f, " colliders %d slots", M->colliderCount);
        else fprintf(f, " colliders %d %d slots", M->colliderFirst, M->colliderCount);
        if (M->slotCount == 0) fprintf(f, " -");
        for (int k = 0; k < M->slotCount; k++) {
            const MS_SlotMaterial *sl = &s->modelSlots[M->slotFirst + k];
            if (diff && sl->material >= 0 && sl->material < s->materialCount)
                fprintf(f, "%s%d=\"%s\"", k ? "," : " ", sl->slot, s->materials[sl->material].name);
            else fprintf(f, "%s%d=%d", k ? "," : " ", sl->slot, sl->material);
        }
        fprintf(f, "  # %s at %.3f %.3f %.3f, bounds %.3f %.3f %.3f .. %.3f %.3f %.3f\n", s->modelFiles[M->file].path,
                M->translation.x, M->translation.y, M->translation.z, M->bounds.min.x, M->bounds.min.y, M->bounds.min.z,
                M->bounds.max.x, M->bounds.max.y, M->bounds.max.z);
    }
    for (int i = s->boxCount; i < s->colliderCount; i++) {
        fprintf(f, "collider");
        Index(f, diff, i);
        HexBox(f, s->colliders[i]);
        fprintf(f, "\n");
    }
    for (int i = 0; i < s->materialCount; i++) {
        const MS_Material *m = &s->materials[i];
        fprintf(f, "material");
        Index(f, diff, i);
        fprintf(f, " \"%s\"", m->name);
        Line(f, diff, m->line);
        fprintf(f, " shader \"%s\" unlit %s\n", m->shader ? m->shader : "", m->material.unlit ? "on" : "off");
    }
    for (int i = 0; i < s->lightCount; i++) {
        const R3D_Light *L = &s->lights[i].light;
        fprintf(f, "light");
        Index(f, diff, i);
        fprintf(f, " %s", LIGHT[L->type % 3]);
        Line(f, diff, s->lights[i].ref.line);
        fprintf(f, " position");
        Hex3(f, L->position);
        fprintf(f, " direction"); Hex3(f, L->direction);
        fprintf(f, " color %d %d %d energy %a range %a shadow %s", L->color.r, L->color.g, L->color.b, L->energy, L->range,
                s->lights[i].shadow ? "on" : "off");
        if (s->lights[i].clip) { fprintf(f, " clip"); Hex3(f, s->lights[i].clipBox.min); Hex3(f, s->lights[i].clipBox.max); }
        fprintf(f, "\n");
    }
    fprintf(f, "sun direction");
    Hex3(f, s->sun.light.direction);
    fprintf(f, " energy %a range %a shadow %s enabled %s\n", s->sun.light.energy, s->sun.light.range,
            s->sun.shadow ? "on" : "off", s->sun.enabled ? "on" : "off");
    return fclose(f) == 0;
}

bool DumpEntities(const MS_Scene *s, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    static const char *const KIND[] = { "none", "box", "model", "light" };
    fprintf(f, "monolith-entity-dump 1\nmap \"%s\" entities %d\n", s->path, s->entityCount);
    for (int e = 0; e < s->entityCount; e++) {
        MS_EntityId id = s->entityIds[e];
        const MS_Source *src = &s->entitySources[e];
        const MS_Transform *t = &s->entityTransforms[e];
        fprintf(f, "entity %d id %08x:%d %s %s %d line %d copy %d part %d position", e, id.stmt, id.index,
                MS_ENTITY_EXPLICIT(id) ? "explicit" : "implicit", KIND[s->entityObjects[e].kind % 4], s->entityObjects[e].index,
                src->line, src->copy, src->part);
        Hex3(f, t->position);
        fprintf(f, " rotationDeg"); Hex3(f, t->rotationDeg);
        fprintf(f, " scale"); Hex3(f, t->scale);
        fprintf(f, " model %d\n", s->entityModels[e]);
    }
    return fclose(f) == 0;
}
