// viewer -- fly through a monolith map with raylib + R3D and the scene runtime
//
//   viewer [package] [switches]
//                        package: a map package folder or its compiled map, <name>.mapb
//                        (default: the package this folder was exported with, next to the
//                        program or in the current folder)
//
//   The window is single-sampled: a 4x multisampled window is presented black on some drivers
//   (Intel HD Graphics Gen11 on Windows, 2026-11-21), and R3D anti-aliases its own image (FXAA),
//   so MSAA on the window would only smooth the text drawn on top.
//
//   Switches for a window that misbehaves on some GPU or driver (they go into the log):
//     --msaa             a 4x multisampled window (black on the driver above; --no-msaa: the
//                        single-sampled one, the default)
//     --vsync            wait for the display at each swap
//     --fps <n>          frame cap (default 144; 0: none)
//     --finish           glFinish before each swap
//     --gl-debug         the driver's GL debug output in the log, and a GL error check with
//                        the frame count every 10 s
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
#include <rlgl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Declared by hand rather than via <GL/gl.h> / <windows.h> (which clash with raylib): glFinish
// is in every GL library, glfwGetProcAddress in raylib's window library (GLFW, desktop builds)
extern void glFinish(void);
extern void *glfwGetProcAddress(const char *name);

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

// The window's settings, from the command line
typedef struct {
    bool msaa, vsync, finish, glDebug;
    int fps;
} WindowOptions;

int main(int argc, char **argv)
{
    WindowOptions opt = { .msaa = false, .fps = 144 };    // single-sampled: see the top comment
    const char *packageArg = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--msaa") == 0) opt.msaa = true;
        else if (strcmp(argv[i], "--no-msaa") == 0) opt.msaa = false;
        else if (strcmp(argv[i], "--vsync") == 0) opt.vsync = true;
        else if (strcmp(argv[i], "--finish") == 0) opt.finish = true;
        else if (strcmp(argv[i], "--gl-debug") == 0) opt.glDebug = true;
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) opt.fps = atoi(argv[++i]);
        else if (argv[i][0] == '-' || packageArg) {
            fprintf(stderr, "usage: %s [package] [--msaa | --no-msaa] [--vsync] [--fps <n>] [--finish] [--gl-debug]\n",
                    argv[0]);
            return 1;
        }
        else packageArg = argv[i];
    }

    char package[1024];
    if (packageArg) snprintf(package, sizeof(package), "%s", packageArg);
    else {
        snprintf(package, sizeof(package), "%s%s", GetApplicationDirectory(), MAP_NAME);
        if (!DirectoryExists(package)) snprintf(package, sizeof(package), "%s", MAP_NAME);
    }

    // Read the package once without the GPU: errors show before a window opens, and the sun's
    // shadow map size (an R3D init hint) is set before R3D_Init
    MS_Scene *peek = MS_Load(package, &(MS_LoadOptions){ .noGpu = true });
    if (!peek) { fprintf(stderr, "%s\n", MS_LastError()); MS_ClearError(); return 1; }

    SetConfigFlags((opt.msaa ? FLAG_MSAA_4X_HINT : 0) | (opt.vsync ? FLAG_VSYNC_HINT : 0) | FLAG_WINDOW_RESIZABLE);
    InitWindow(1600, 900, MAP_NAME);
    SetTargetFPS(opt.fps > 0 ? opt.fps : 0);
    TraceLog(LOG_INFO, "viewer: window: MSAA %s, vsync %s, fps cap %d (0: none), glFinish before the swap %s, "
             "GL debug %s", opt.msaa ? "4x" : "off", opt.vsync ? "on" : "off", opt.fps > 0 ? opt.fps : 0,
             opt.finish ? "on" : "off", opt.glDebug ? "on" : "off");
    MS_LogGLErrors(LOG_INFO, "after InitWindow");
    if (opt.glDebug) MS_BeginGLDebugOutput(glfwGetProcAddress);
    MS_SetInitHints(peek);
    MS_Unload(peek);
    R3D_Init(GetScreenWidth(), GetScreenHeight());
    R3D_SetAntiAliasingMode(R3D_ANTI_ALIASING_MODE_FXAA);
    MS_InitRenderer();                  // depth clamp for the sun's shadow (see monolith_scene.h)

    MS_Scene *scene = MS_Load(package, NULL);
    if (!scene) {
        fprintf(stderr, "%s\n", MS_LastError());
        MS_ClearError();
        MS_EndGLDebugOutput();
        R3D_Close();
        CloseWindow();
        return 1;
    }
    MS_ApplyEnvironment(scene);         // sky, ambient light, fog, SSAO, bloom, tonemap ...

    int view = 0;                       // the map always has at least one viewpoint
    FlyCamera cam = FromViewpoint(&scene->viewpoints[view]);
    double checkAt = GetTime() + 10.0;  // --gl-debug: the next GL error check
    int frames = 0;
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
            if (opt.finish) {
                rlDrawRenderBatchActive();      // the text, then wait for the GPU before the swap
                glFinish();
            }
        EndDrawing();
        frames++;
        if (opt.glDebug && GetTime() >= checkAt) {
            TraceLog(LOG_INFO, "viewer: %d frames in the last 10 s", frames);
            MS_LogGLErrors(LOG_WARNING, "in the last 10 s");
            frames = 0;
            checkAt = GetTime() + 10.0;
        }
    }

    MS_Unload(scene);
    MS_ClearError();                    // frees the last error message, if a call failed
    MS_EndGLDebugOutput();
    R3D_Close();
    CloseWindow();
    return 0;
}
