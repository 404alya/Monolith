// 01_minimal -- load a monolith map package and orbit around it (raylib + R3D)
//
// Also the viewer.c that --export puts next to an exported scene, so it depends on nothing
// but raylib, R3D and the scene runtime (monolith_scene.h/.c).
//
//   minimal [package]            open a window: the camera orbits the map's first viewpoint;
//                                mouse wheel zooms, LEFT/RIGHT switch viewpoints, ESC quits.
//                                package: a map package folder or .map file (default: scene/
//                                next to the executable)
//   --shot <file>                render 30 frames in a hidden window, save <file>, exit
//   --size <w> <h>               window / shot size (default 1600 900)
//   --view <n>                   start at the map's viewpoint n (1-based, default 1)
//   --eye x y z --look yaw pitch start at this eye position / view angles (degrees; yaw 0 looks
//                                toward +Z, yaw 180 toward -Z), like monolith's --eye / --look
//
// The shot renders exactly like `monolith --shot <file> --view n` (same camera math, FXAA,
// depth clamp, 30 frames into a RenderTexture), so the two images can be compared pixel by
// pixel.

#include "monolith_scene.h"
#include <raymath.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// monolith's camera convention: yaw about +Y (0 = toward +Z), pitch up, fovy 70
static Camera3D CameraFromAngles(Vector3 eye, float yaw, float pitch)
{
    Vector3 dir = { cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw) };
    return (Camera3D){ .position = eye, .target = Vector3Add(eye, dir), .up = { 0, 1, 0 },
                       .fovy = 70.0f, .projection = CAMERA_PERSPECTIVE };
}

static Camera3D CameraFromViewpoint(const MS_Viewpoint *v)
{
    Vector3 d = Vector3Normalize(Vector3Subtract(v->target, v->eye));
    return CameraFromAngles(v->eye, atan2f(d.x, d.z), asinf(Clamp(d.y, -1.0f, 1.0f)));
}

int main(int argc, char **argv)
{
    const char *package = NULL, *shotPath = NULL;
    int width = 1600, height = 900, view = 1;
    bool eyeSet = false, lookSet = false;
    Vector3 eye = { 0 };
    float yaw = 0.0f, pitch = 0.0f;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shotPath = argv[++i];
        else if (strcmp(argv[i], "--size") == 0 && i + 2 < argc) { width = atoi(argv[i + 1]); height = atoi(argv[i + 2]); i += 2; }
        else if (strcmp(argv[i], "--view") == 0 && i + 1 < argc) view = atoi(argv[++i]);
        else if (strcmp(argv[i], "--eye") == 0 && i + 3 < argc) {
            eye = (Vector3){ (float)atof(argv[i + 1]), (float)atof(argv[i + 2]), (float)atof(argv[i + 3]) };
            eyeSet = true;
            i += 3;
        } else if (strcmp(argv[i], "--look") == 0 && i + 2 < argc) {
            yaw = (float)atof(argv[i + 1]) * DEG2RAD;
            pitch = (float)atof(argv[i + 2]) * DEG2RAD;
            lookSet = true;
            i += 2;
        } else if (argv[i][0] != '-' && !package) package = argv[i];
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 1; }
    }
    char defaultPackage[1024];
    if (!package) {
        snprintf(defaultPackage, sizeof(defaultPackage), "%sscene", GetApplicationDirectory());
        package = defaultPackage;
    }

    // Parse the package once without the GPU: errors show before a window opens, and the
    // sun's shadow map size (an R3D init hint) is known before R3D_Init
    char err[512];
    MS_Scene *peek = MS_Load(package, &(MS_LoadOptions){ .noGpu = true }, err, sizeof(err));
    if (!peek) { fprintf(stderr, "%s\n", err); return 1; }

    // A shot opens a hidden window that is never mapped. The title "monolith-shot" lets a
    // window manager rule route it (i3: for_window [title="^monolith-(bench|shot)$"] ...).
    SetConfigFlags(FLAG_MSAA_4X_HINT | (shotPath ? FLAG_WINDOW_HIDDEN : FLAG_WINDOW_RESIZABLE));
    InitWindow(width, height, shotPath ? "monolith-shot" : "monolith scene");
    MS_SetInitHints(peek);
    MS_Unload(peek);
    R3D_Init(GetScreenWidth(), GetScreenHeight());
    R3D_SetAntiAliasingMode(R3D_ANTI_ALIASING_MODE_FXAA);
    MS_InitRenderer();                  // depth clamp for the sun's shadow (see monolith_scene.h)

    MS_Scene *scene = MS_Load(package, NULL, err, sizeof(err));
    if (!scene) { fprintf(stderr, "%s\n", err); R3D_Close(); CloseWindow(); return 1; }
    MS_ApplyEnvironment(scene);

    if (view < 1 || view > scene->viewpointCount) view = 1;
    Camera3D camera = CameraFromViewpoint(&scene->viewpoints[view - 1]);
    if (eyeSet || lookSet) {
        if (!lookSet) {     // keep the viewpoint's direction
            Vector3 d = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
            yaw = atan2f(d.x, d.z);
            pitch = asinf(Clamp(d.y, -1.0f, 1.0f));
        }
        camera = CameraFromAngles(eyeSet ? eye : camera.position, yaw, pitch);
    }

    // Shot: hidden windows have no visible pixels, so render into a texture and read that
    RenderTexture target = { 0 };
    if (shotPath) target = LoadRenderTexture(width, height);

    // Orbit around the point the view looks at (where the centre ray hits the scene)
    bool orbitReady = false;
    for (int frame = 0; !WindowShouldClose(); frame++) {
        if (!shotPath) {
            if (IsWindowResized()) R3D_SetResolution(GetScreenWidth(), GetScreenHeight());
            int step = IsKeyPressed(KEY_RIGHT) - IsKeyPressed(KEY_LEFT);
            if (step) {
                view = (view - 1 + step + scene->viewpointCount) % scene->viewpointCount + 1;
                camera = CameraFromViewpoint(&scene->viewpoints[view - 1]);
                orbitReady = false;
            }
            if (!orbitReady) {
                Vector3 dir = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
                MS_Hit hit = MS_Raycast(scene, camera.position, dir, 500.0f);
                camera.target = hit.obj.kind != MS_OBJ_NONE ? hit.point : Vector3Add(camera.position, Vector3Scale(dir, 30.0f));
                orbitReady = true;
            }
            UpdateCamera(&camera, CAMERA_ORBITAL);  // slow automatic orbit, wheel = zoom
        }

        BeginDrawing();
            ClearBackground(BLACK);
            MS_Draw(scene, camera, target);
            if (!shotPath) DrawText(TextFormat("%d fps  |  viewpoint %d/%d: %s  |  LEFT/RIGHT viewpoints, wheel zoom", GetFPS(),
                                               view, scene->viewpointCount, scene->viewpoints[view - 1].name),
                                    12, 10, 18, RAYWHITE);
        EndDrawing();

        if (shotPath && frame == 29) {
            Image img = LoadImageFromTexture(target.texture);
            ImageFlipVertical(&img);                            // GL textures are bottom-up
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8); // drop alpha, like monolith
            bool ok = ExportImage(img, shotPath);
            UnloadImage(img);
            printf("%s %s\n", ok ? "saved" : "failed to save", shotPath);
            break;
        }
    }

    if (shotPath) UnloadRenderTexture(target);
    MS_Unload(scene);
    R3D_Close();
    CloseWindow();
    return 0;
}
