// 05_entities -- the scene's entities and components, and a game's own components and
// systems on top of them
//
// Every box, model instance and light of a map is an entity: a number (0..entityCount-1,
// map order) with components in parallel arrays of MS_Scene (entityIds, entityObjects,
// entityTransforms, entitySources, entityModels). The scene's components are read-only.
// A game adds its own components the same way: one array per component, indexed by the
// entity number, plus a "has it" flag; a system is a loop over the entities that have the
// components it needs. The MS_EntityId names an object independently of its number, e.g.
// for game code that refers to "the door with id 12". Built like an exported game: the
// runtime without its editor parts (no hot reload). See README.md.
//
//   entities [package] [--find <key>:<index>]
//     package             default: scene/ next to the executable (or a package with its compiled
//                         map, e.g. a copy of maps/pine_forest after monolith --compile)
//     --find <key>:<index>   print the entity with this id (as monolith --dump-entities writes
//                         it, e.g. 0000000c:0 or a278642c:3)
//   Interactive: right mouse + WASD/QE fly, LEFT/RIGHT viewpoints, TAB next beacon entity.

#include "monolith_scene.h"
#include <raymath.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ======================================================================================
// The game's own component and its storage
// ======================================================================================

// A beacon: a small glowing cube that floats above a light or a model and bobs (phase:
// per-entity state that BeaconSystem advances)
typedef struct {
    float phase;            // radians, advanced by BeaconSystem
    float speed;            // radians per second
    Color color;
} Beacon;

// The game's world: its components for the scene's entities, indexed like them
typedef struct {
    int count;              // = scene->entityCount
    bool *hasBeacon;
    Beacon *beacon;
} World;

static void WorldFree(World *w)
{
    free(w->hasBeacon);
    free(w->beacon);
    *w = (World){ 0 };
}

// Attaches the game's components by a rule on the scene's components: every light and every
// model instance gets a beacon (a light's in its own colour)
static bool WorldBuild(World *w, const MS_Scene *s)
{
    int n = s->entityCount > 0 ? s->entityCount : 1;
    *w = (World){ .count = s->entityCount, .hasBeacon = calloc(n, sizeof(bool)), .beacon = calloc(n, sizeof(Beacon)) };
    if (!w->hasBeacon || !w->beacon) { WorldFree(w); return false; }
    for (int e = 0; e < s->entityCount; e++) {
        MS_ObjRef o = s->entityObjects[e];
        if (o.kind != MS_OBJ_LIGHT && o.kind != MS_OBJ_MODEL) continue;
        w->hasBeacon[e] = true;
        w->beacon[e] = (Beacon){
            .phase = 0.0f,
            .speed = 1.5f + 0.5f * (float)(e % 3),
            .color = o.kind == MS_OBJ_LIGHT ? s->lights[o.index].light.color : (Color){ 120, 200, 255, 255 },
        };
    }
    return true;
}

// ======================================================================================
// Systems: loops over the entities with the components they need
// ======================================================================================

static void BeaconSystem(World *w, float dt)
{
    for (int e = 0; e < w->count; e++)
        if (w->hasBeacon[e]) w->beacon[e].phase = fmodf(w->beacon[e].phase + w->beacon[e].speed * dt, 2.0f * PI);
}

// Where a beacon floats: above the entity's position (Transform component); models: above
// their world bounds
static Vector3 BeaconPosition(const World *w, const MS_Scene *s, int e)
{
    Vector3 p = s->entityTransforms[e].position;
    if (s->entityObjects[e].kind == MS_OBJ_MODEL) p.y = MS_ObjectBounds(s, s->entityObjects[e]).max.y;
    p.y += 0.6f + 0.15f * sinf(w->beacon[e].phase);
    return p;
}

// Inside the game's own R3D frame, after the scene
static void BeaconDrawSystem(const World *w, const MS_Scene *s, R3D_Mesh cube, R3D_Material mat)
{
    for (int e = 0; e < w->count; e++) {
        if (!w->hasBeacon[e]) continue;
        mat.albedo.color = w->beacon[e].color;
        mat.emission.color = w->beacon[e].color;
        R3D_DrawMesh(cube, mat, BeaconPosition(w, s, e), 0.2f);
    }
}

// ======================================================================================
// Reading the scene's components
// ======================================================================================

static const char *KindName(MS_ObjKind k)
{
    return k == MS_OBJ_BOX ? "box" : k == MS_OBJ_MODEL ? "model" : k == MS_OBJ_LIGHT ? "light" : "none";
}

// One entity with all its components, as a line
static const char *Describe(const MS_Scene *s, int e)
{
    MS_EntityId id = s->entityIds[e];
    MS_ObjRef o = s->entityObjects[e];
    const MS_Source *src = &s->entitySources[e];
    const MS_Transform *t = &s->entityTransforms[e];
    int g = MS_ObjectRef(s, o)->group;
    return TextFormat("entity %d  id %08x:%d (%s)  %s %d  line %d copy %d  group \"%s\"  at %.2f %.2f %.2f%s%s",
                      e, id.stmt, id.index, MS_ENTITY_EXPLICIT(id) ? "id" : "implicit", KindName(o.kind), o.index,
                      src->line, src->copy, g >= 0 ? s->groups[g] : "", t->position.x, t->position.y, t->position.z,
                      s->entityModels[e] >= 0 ? "  model " : "",
                      s->entityModels[e] >= 0 ? s->modelFiles[s->entityModels[e]].path : "");
}

static void PrintSummary(const MS_Scene *s)
{
    int kinds[4] = { 0 }, explicitIds = 0;
    for (int e = 0; e < s->entityCount; e++) {
        kinds[s->entityObjects[e].kind]++;
        explicitIds += MS_ENTITY_EXPLICIT(s->entityIds[e]);
    }
    printf("%d entities: %d boxes, %d models, %d lights; %d with an `id` line\n", s->entityCount,
           kinds[MS_OBJ_BOX], kinds[MS_OBJ_MODEL], kinds[MS_OBJ_LIGHT], explicitIds);
    // The first entity of each kind
    for (int k = MS_OBJ_BOX; k <= MS_OBJ_LIGHT; k++)
        for (int e = 0; e < s->entityCount; e++)
            if (s->entityObjects[e].kind == (MS_ObjKind)k) { printf("  %s\n", Describe(s, e)); break; }
}

static Camera3D ViewpointCamera(const MS_Scene *s, int i)
{
    return (Camera3D){ .position = s->viewpoints[i].eye, .target = s->viewpoints[i].target, .up = { 0, 1, 0 },
                       .fovy = 70.0f, .projection = CAMERA_PERSPECTIVE };
}

int main(int argc, char **argv)
{
    const char *package = NULL, *find = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--find") == 0 && i + 1 < argc) find = argv[++i];
        else if (argv[i][0] != '-') package = argv[i];
    }
    if (!package) package = TextFormat("%sscene", GetApplicationDirectory());

    char err[512];
    MS_Scene *peek = MS_Load(package, &(MS_LoadOptions){ .noGpu = true }, err, sizeof(err));
    if (!peek) { fprintf(stderr, "%s\n", err); return 1; }
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 720, "entities");
    SetTargetFPS(60);
    MS_SetInitHints(peek);
    MS_Unload(peek);
    R3D_Init(GetScreenWidth(), GetScreenHeight());
    R3D_SetAntiAliasingMode(R3D_ANTI_ALIASING_MODE_FXAA);
    MS_InitRenderer();

    MS_Scene *scene = MS_Load(package, NULL, err, sizeof(err));
    if (!scene) { fprintf(stderr, "%s\n", err); return 1; }
    MS_ApplyEnvironment(scene);
    PrintSummary(scene);

    // Lookup by stable id: the same id finds the same object, whatever its entity number
    if (find) {
        unsigned int key;
        int index;
        int e = sscanf(find, "%x:%d", &key, &index) == 2 ? MS_FindEntity(scene, (MS_EntityId){ key, index }) : -1;
        if (e >= 0) printf("found: %s\n", Describe(scene, e));
        else printf("no entity %s\n", find);
    }

    World world;
    if (!WorldBuild(&world, scene)) { fprintf(stderr, "out of memory\n"); return 1; }
    R3D_Mesh cube = R3D_GenMeshCube(1, 1, 1);
    R3D_Material glow = R3D_GetDefaultMaterial();
    glow.emission.energy = 4.0f;

    int view = 0, picked = -1;
    Camera3D camera = ViewpointCamera(scene, view);
    while (!WindowShouldClose()) {
        float dt = GetFrameTime();

        BeaconSystem(&world, dt);

        int step = IsKeyPressed(KEY_RIGHT) - IsKeyPressed(KEY_LEFT);
        if (step) {
            view = (view + step + scene->viewpointCount) % scene->viewpointCount;
            camera = ViewpointCamera(scene, view);
        }
        if (IsKeyPressed(KEY_TAB))      // the next entity with a beacon
            for (int k = 1; k <= world.count; k++) {
                int e = (picked + k) % world.count;
                if (world.hasBeacon[e]) { picked = e; break; }
            }
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) UpdateCamera(&camera, CAMERA_FREE);
        if (IsWindowResized()) R3D_SetResolution(GetScreenWidth(), GetScreenHeight());

        BeginDrawing();
            ClearBackground(BLACK);
            // The game's own R3D frame: the scene's lights and geometry, then the beacons
            R3D_Begin(camera);
                MS_PushLights(scene, camera, (float)GetScreenWidth() / GetScreenHeight());
                MS_DrawGeometry(scene);
                BeaconDrawSystem(&world, scene, cube, glow);
            R3D_End();
            DrawText("RMB+WASD fly  |  LEFT/RIGHT viewpoints  |  TAB next beacon", 12, 10, 18, RAYWHITE);
            if (picked >= 0) DrawText(Describe(scene, picked), 12, GetScreenHeight() - 30, 18, RAYWHITE);
        EndDrawing();
    }

    WorldFree(&world);
    R3D_UnloadMesh(cube);
    MS_Unload(scene);
    R3D_Close();
    CloseWindow();
    return 0;
}
