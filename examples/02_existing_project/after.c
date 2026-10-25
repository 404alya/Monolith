// 02_existing_project / after.c -- the game of before.c, AFTER integrating a monolith scene
//
// The level (floor, walls, sky, sun, fog, post-processing, the map's lamps) now comes from a
// monolith map package, drawn inside the game's own R3D frame next to its own objects; the
// player collides with the scene through MS_Collides. Every change is marked "monolith:";
// README.md shows the diff.
//
//   after [package]            package: default scene/ next to the executable

#include <r3d/r3d.h>
#include <raymath.h>
#include <math.h>
#include "monolith_scene.h"                         // monolith: the scene runtime

static MS_Scene *scene;                             // monolith: the level

// #region Game objects
typedef struct {
    Vector3 feet;
    float yaw, vy;
    bool onGround;
} Player;

#define CRATE_COUNT 4
static const Vector3 CRATES[CRATE_COUNT] = { { 3, 0, 10 }, { 4.2f, 0, 10.6f }, { 3.6f, 1, 10.3f }, { -4, 0, 7 } };

static BoundingBox PlayerBox(Vector3 feet)
{
    return (BoundingBox){ { feet.x - 0.3f, feet.y, feet.z - 0.3f }, { feet.x + 0.3f, feet.y + 1.8f, feet.z + 0.3f } };
}

static bool Overlap(BoundingBox a, BoundingBox b)      // strict: touching is not a hit
{
    return a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y && a.max.y > b.min.y &&
           a.min.z < b.max.z && a.max.z > b.min.z;
}

// What the player can't walk into: the scene and the game's crates (1 m cubes)
static bool Blocked(BoundingBox b)
{
    if (MS_Collides(scene, b)) return true;         // monolith: was the flat floor (y < 0)
    for (int i = 0; i < CRATE_COUNT; i++) {
        Vector3 c = CRATES[i];
        if (Overlap(b, (BoundingBox){ { c.x - 0.5f, c.y, c.z - 0.5f }, { c.x + 0.5f, c.y + 1, c.z + 0.5f } })) return true;
    }
    return false;
}

// Moves one axis at a time and undoes the move on a collision
static void MovePlayer(Player *p, Vector3 delta)
{
    for (int axis = 0; axis < 3; axis++) {
        Vector3 next = p->feet;
        float d = axis == 0 ? delta.x : axis == 1 ? delta.y : delta.z;
        if (axis == 0) next.x += d; else if (axis == 1) next.y += d; else next.z += d;
        if (!Blocked(PlayerBox(next))) p->feet = next;
        else if (axis == 1) { if (d < 0) p->onGround = true; p->vy = 0; }
    }
}

static void UpdatePlayer(Player *p, float dt)
{
    p->yaw += ((float)IsKeyDown(KEY_LEFT) - (float)IsKeyDown(KEY_RIGHT)) * 2.0f * dt - GetMouseDelta().x * 0.003f;
    Vector3 fwd = { sinf(p->yaw), 0, cosf(p->yaw) }, right = { -cosf(p->yaw), 0, sinf(p->yaw) };
    Vector3 wish = Vector3Add(Vector3Scale(fwd, (float)IsKeyDown(KEY_W) - (float)IsKeyDown(KEY_S)),
                              Vector3Scale(right, (float)IsKeyDown(KEY_D) - (float)IsKeyDown(KEY_A)));
    if (p->onGround && IsKeyPressed(KEY_SPACE)) p->vy = 5.0f;
    p->vy -= 15.0f * dt;
    p->onGround = false;
    MovePlayer(p, (Vector3){ wish.x * 5.0f * dt, p->vy * dt, wish.z * 5.0f * dt });
}

static Camera3D FollowCamera(const Player *p)
{
    Vector3 head = { p->feet.x, p->feet.y + 1.6f, p->feet.z };
    Vector3 back = { -sinf(p->yaw) * 5.0f, 1.8f, -cosf(p->yaw) * 5.0f };
    return (Camera3D){ .position = Vector3Add(head, back), .target = head, .up = { 0, 1, 0 }, .fovy = 60.0f,
                       .projection = CAMERA_PERSPECTIVE };
}
// #endregion

int main(int argc, char **argv)
{
    // monolith: parse the package before R3D_Init (errors before a window; the sun's shadow map size is an init hint)
    const char *package = argc > 1 && argv[1][0] != '-' ? argv[1] : TextFormat("%sscene", GetApplicationDirectory());
    char err[512];
    MS_Scene *peek = MS_Load(package, &(MS_LoadOptions){ .noGpu = true }, err, sizeof(err));
    if (!peek) { puts(err); return 1; }
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 720, "my game");
    SetTargetFPS(60);
    MS_SetInitHints(peek);                          // monolith
    MS_Unload(peek);                                // monolith
    R3D_Init(GetScreenWidth(), GetScreenHeight());
    R3D_SetAntiAliasingMode(R3D_ANTI_ALIASING_MODE_FXAA);
    MS_InitRenderer();                              // monolith: GL state for the sun's shadow (depth clamp)

    scene = MS_Load(package, NULL, err, sizeof(err));   // monolith: load the level
    if (!scene) { puts(err); return 1; }
    MS_ApplyEnvironment(scene);                         // monolith: sky, ambient, fog, post-processing

    // Meshes and materials of the game's own objects (monolith: the floor is gone)
    R3D_Mesh crateMesh = R3D_GenMeshCube(1, 1, 1);
    R3D_Mesh playerMesh = R3D_GenMeshCapsule(0.3f, 1.2f, 8, 16);
    R3D_Material crateMat = R3D_GetDefaultMaterial();
    crateMat.albedo.color = (Color){ 150, 105, 60, 255 };
    crateMat.orm.roughness = 0.8f;
    R3D_Material playerMat = R3D_GetDefaultMaterial();
    playerMat.albedo.color = (Color){ 40, 110, 200, 255 };
    playerMat.orm.roughness = 0.4f;

    // The game's own lights (monolith: sky, ambient and sun come from the scene now)
    R3D_Light lamp = R3D_CreateOmniLight((Vector3){ 2, 2.5f, 12 }, 10.0f, (Color){ 120, 200, 255, 255 }, 3.0f);

    Player player = { .feet = scene->spawnFeet, .yaw = scene->spawnYawDeg * DEG2RAD };   // monolith: the map's spawn
    DisableCursor();

    while (!WindowShouldClose()) {
        UpdatePlayer(&player, fminf(GetFrameTime(), 1.0f / 20.0f));
        Camera3D camera = FollowCamera(&player);

        BeginDrawing();
            ClearBackground(BLACK);
            R3D_Begin(camera);
                MS_PushLights(scene, camera, (float)GetScreenWidth() / GetScreenHeight());   // monolith: sun + map lights
                MS_DrawGeometry(scene);                                                     // monolith: the level
                R3D_PushLight(lamp);
                for (int i = 0; i < CRATE_COUNT; i++)
                    R3D_DrawMesh(crateMesh, crateMat, Vector3Add(CRATES[i], (Vector3){ 0, 0.5f, 0 }), 1.0f);
                R3D_DrawMesh(playerMesh, playerMat, Vector3Add(player.feet, (Vector3){ 0, 0.9f, 0 }), 1.0f);
            R3D_End();
            DrawText("WASD move, LEFT/RIGHT or mouse turn, SPACE jump", 12, 10, 18, RAYWHITE);
        EndDrawing();
    }

    MS_Unload(scene);                               // monolith
    R3D_UnloadMesh(playerMesh);
    R3D_UnloadMesh(crateMesh);
    R3D_Close();
    CloseWindow();
    return 0;
}
