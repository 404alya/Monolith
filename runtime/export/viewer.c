// viewer -- fly through a monolith map with raylib + R3D and the scene runtime
//
//   viewer [package]     package: a map package folder or its compiled map, <name>.mapb
//                        (default: the package this folder was exported with, next to the
//                        program or in the current folder)
//
//   mouse (right button held)   look around        W A S D   move
//   E / Q                       up / down          SHIFT     faster
//   LEFT / RIGHT                previous / next viewpoint of the map
//
// The scene runtime (monolith_scene.h/.c) owns everything about the map: loading, the
// environment, lights, shadows and drawing. Everything else here (window, camera, input)
// is this program's and would be your game's.

#include "monolith_scene.h"
#include <raymath.h>
#include <math.h>
#include <stdio.h>

static const char *const MAP_NAME = "@MAP@";

typedef struct {
    Vector3 eye;
    float yaw, pitch;   // radians; yaw 0 looks toward +Z, pitch up
} FlyCamera;

static Camera3D ToCamera3D(FlyCamera c)
{
    Vector3 dir = { cosf(c.pitch) * sinf(c.yaw), sinf(c.pitch), cosf(c.pitch) * cosf(c.yaw) };
    return (Camera3D){ .position = c.eye, .target = Vector3Add(c.eye, dir), .up = { 0, 1, 0 },
                       .fovy = 70.0f, .projection = CAMERA_PERSPECTIVE };
}

static FlyCamera FromViewpoint(const MS_Viewpoint *v)
{
    Vector3 d = Vector3Normalize(Vector3Subtract(v->target, v->eye));
    return (FlyCamera){ v->eye, atan2f(d.x, d.z), asinf(Clamp(d.y, -1.0f, 1.0f)) };
}

static void Fly(FlyCamera *c, float dt)
{
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        Vector2 md = GetMouseDelta();
        c->yaw -= md.x * 0.003f;
        c->pitch = Clamp(c->pitch - md.y * 0.003f, -1.55f, 1.55f);
    }
    Vector3 forward = { cosf(c->pitch) * sinf(c->yaw), sinf(c->pitch), cosf(c->pitch) * cosf(c->yaw) };
    Vector3 right = { -cosf(c->yaw), 0.0f, sinf(c->yaw) };
    Vector3 move = { 0 };
    if (IsKeyDown(KEY_W)) move = Vector3Add(move, forward);
    if (IsKeyDown(KEY_S)) move = Vector3Subtract(move, forward);
    if (IsKeyDown(KEY_D)) move = Vector3Add(move, right);
    if (IsKeyDown(KEY_A)) move = Vector3Subtract(move, right);
    if (IsKeyDown(KEY_E)) move.y += 1.0f;
    if (IsKeyDown(KEY_Q)) move.y -= 1.0f;
    float speed = IsKeyDown(KEY_LEFT_SHIFT) ? 24.0f : 6.0f;     // m/s
    if (Vector3Length(move) > 0.0f) c->eye = Vector3Add(c->eye, Vector3Scale(Vector3Normalize(move), speed * dt));
}

int main(int argc, char **argv)
{
    char package[1024];
    if (argc > 1) snprintf(package, sizeof(package), "%s", argv[1]);
    else {
        snprintf(package, sizeof(package), "%s%s", GetApplicationDirectory(), MAP_NAME);
        if (!DirectoryExists(package)) snprintf(package, sizeof(package), "%s", MAP_NAME);
    }

    // Read the package once without the GPU: errors show before a window opens, and the sun's
    // shadow map size (an R3D init hint) is set before R3D_Init
    MS_Scene *peek = MS_Load(package, &(MS_LoadOptions){ .noGpu = true });
    if (!peek) { fprintf(stderr, "%s\n", MS_LastError()); MS_ClearError(); return 1; }

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(1600, 900, MAP_NAME);
    SetTargetFPS(144);
    MS_SetInitHints(peek);
    MS_Unload(peek);
    R3D_Init(GetScreenWidth(), GetScreenHeight());
    R3D_SetAntiAliasingMode(R3D_ANTI_ALIASING_MODE_FXAA);
    MS_InitRenderer();                  // depth clamp for the sun's shadow (see monolith_scene.h)

    MS_Scene *scene = MS_Load(package, NULL);
    if (!scene) { fprintf(stderr, "%s\n", MS_LastError()); MS_ClearError(); R3D_Close(); CloseWindow(); return 1; }
    MS_ApplyEnvironment(scene);         // sky, ambient light, fog, SSAO, bloom, tonemap ...

    int view = 0;                       // the map always has at least one viewpoint
    FlyCamera cam = FromViewpoint(&scene->viewpoints[view]);
    while (!WindowShouldClose()) {
        if (IsWindowResized()) MS_ApplyRenderResolution(scene, GetScreenWidth(), GetScreenHeight());
        int step = IsKeyPressed(KEY_RIGHT) - IsKeyPressed(KEY_LEFT);
        if (step) {
            view = (view + step + scene->viewpointCount) % scene->viewpointCount;
            cam = FromViewpoint(&scene->viewpoints[view]);
        }
        Fly(&cam, GetFrameTime());

        BeginDrawing();
            ClearBackground(BLACK);
            MS_Draw(scene, ToCamera3D(cam), (RenderTexture){ 0 });   // one whole R3D frame
            DrawText(TextFormat("viewpoint %d/%d: %s", view + 1, scene->viewpointCount, scene->viewpoints[view].name),
                     12, 10, 20, RAYWHITE);
            DrawText(TextFormat("%d fps  |  LEFT/RIGHT viewpoints, right mouse look, WASD + E/Q move, SHIFT fast",
                                GetFPS()), 12, 36, 16, LIGHTGRAY);
        EndDrawing();
    }

    MS_Unload(scene);
    MS_ClearError();                    // frees the last error message, if a call failed
    R3D_Close();
    CloseWindow();
    return 0;
}
