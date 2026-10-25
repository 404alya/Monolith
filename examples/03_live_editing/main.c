// 03_live_editing -- a game that hot-reloads the map package monolith is editing
//
// Run it next to monolith on the same package: every save in monolith's map designer (or
// every Claude Code request it launches) shows up here within a second, and a broken edit
// keeps the last good scene and shows the error. See README.md.
//
//   live_editing [package]   the package to watch (default: scene/ next to the executable),
//                            e.g. maps/pine_forest (run from the repo root)
//   Interactive: right mouse + WASD/QE fly (raylib's free camera), LEFT/RIGHT viewpoints.

#include "monolith_scene.h"
#include <raymath.h>
#include <math.h>
#include <stdlib.h>


static Camera3D ViewpointCamera(const MS_Scene *s, int i)
{
    return (Camera3D){ .position = s->viewpoints[i].eye, .target = s->viewpoints[i].target, .up = { 0, 1, 0 },
                       .fovy = 70.0f, .projection = CAMERA_PERSPECTIVE };
}

int main(int argc, char **argv)
{
    const char *package = argc > 1 ? argv[1] : TextFormat("%sscene", GetApplicationDirectory());

    char err[512];
    MS_Scene *peek = MS_Load(package, &(MS_LoadOptions){ .noGpu = true }, err, sizeof(err));
    if (!peek) { fprintf(stderr, "%s\n", err); return 1; }
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 720, "live editing");
    SetTargetFPS(60);
    MS_SetInitHints(peek);
    MS_Unload(peek);
    R3D_Init(GetScreenWidth(), GetScreenHeight());
    R3D_SetAntiAliasingMode(R3D_ANTI_ALIASING_MODE_FXAA);
    MS_InitRenderer();                  // depth clamp for the sun's shadow (see monolith_scene.h)

    MS_Scene *scene = MS_Load(package, NULL, err, sizeof(err));
    if (!scene) { fprintf(stderr, "%s\n", err); return 1; }
    MS_ApplyEnvironment(scene);
    int view = 0;
    Camera3D camera = ViewpointCamera(scene, view);

    char status[600] = "watching for changes";
    float statusTime = 0.0f;
    while (!WindowShouldClose()) {
        float dt = GetFrameTime();

        // --- The live part: poll every file of the package (map, shaders, textures, models).
        // A reload swaps in the new scene (version + 1); the environment is part of the map,
        // so it has to be applied again (then your own overrides, if any).
        MS_ReloadResult r = MS_Poll(scene, dt);
        if (r == MS_RELOADED) {
            MS_ApplyEnvironment(scene);
            snprintf(status, sizeof(status), "reloaded: %d boxes, %d models (version %u)", scene->boxCount,
                     scene->modelCount, scene->version);
            statusTime = 4.0f;
            printf("%s\n", status);
            if (view >= scene->viewpointCount) view = 0;
        } else if (r == MS_RELOAD_FAILED) {
            // The old scene stays loaded; the message is "file:line: message"
            snprintf(status, sizeof(status), "reload failed, keeping the last good scene: %s", MS_Error(scene));
            statusTime = 1e9f;
            printf("%s\n", status);
        }

        int step = IsKeyPressed(KEY_RIGHT) - IsKeyPressed(KEY_LEFT);
        if (step) {
            view = (view + step + scene->viewpointCount) % scene->viewpointCount;
            camera = ViewpointCamera(scene, view);
        }
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) UpdateCamera(&camera, CAMERA_FREE);
        if (IsWindowResized()) R3D_SetResolution(GetScreenWidth(), GetScreenHeight());

        BeginDrawing();
            ClearBackground(BLACK);
            MS_Draw(scene, camera, (RenderTexture){ 0 });
            DrawText(TextFormat("%s  |  RMB+WASD fly, LEFT/RIGHT viewpoints", package), 12, 10, 18, RAYWHITE);
            if (statusTime > 0.0f) DrawText(status, 12, GetScreenHeight() - 30, 18,
                                            MS_Error(scene)[0] ? (Color){ 255, 110, 90, 255 } : RAYWHITE);
        EndDrawing();
        statusTime -= dt;
    }

    MS_Unload(scene);
    R3D_Close();
    CloseWindow();
    return 0;
}
