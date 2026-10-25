// harness.h -- test build of the examples: headless screenshots, no change to their source
//
// Not part of the examples: they are plain integration code. examples/build.sh test (and lsan,
// for tools/leakcheck.sh) compiles each example's main file with `-include harness.h`, which
// turns the unchanged program into a test run:
//
//   <example> --shot <file> [the example's own arguments]
//
// It opens a hidden window that is never mapped (title "monolith-shot", so a window manager
// rule can route it), renders HARNESS_FRAMES frames into a texture, saves the last one to
// <file> and lets the program end normally (WindowShouldClose turns true), so its cleanup
// runs (the leak check needs that). Without --shot it refuses to run (exit 1): a test binary
// never opens a visible window. Frame time is a fixed 1/60 s, so an image never depends on
// the machine's speed. The example's own window title, flags, FPS cap and cursor calls are
// ignored.
//
// How: the program's raylib / R3D / scene calls that decide where a frame goes are macros to
// the Harness* functions below (main included, which becomes HarnessExampleMain), and every
// frame that would reach the window goes into the harness's texture instead: R3D_Begin /
// R3D_BeginEx / R3D_BeginPro / MS_Draw without a target get it. R3D itself always writes framebuffer 0 when
// it has no target, which a hidden window can't read back. With the capture shim in front
// (`-include monolith_capture.h -include harness.h`), its recorders are wrapped the same way.
#ifndef MONOLITH_TEST_HARNESS_H
#define MONOLITH_TEST_HARNESS_H

#include <raylib.h>
#include <r3d/r3d.h>
#include "monolith_scene.h"     // (declarations only: programs without the runtime never call it)
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HARNESS_FRAMES 30

static struct {
    const char *path;       // --shot <file>
    RenderTexture target;   // where the frames go (made right after InitWindow)
    int frame;
    bool done;              // the shot is saved: WindowShouldClose returns true
} harness;

// The calls below use the real functions (or, with the capture shim, its recorders): the
// macros that route the program's calls here are only defined at the end of this file
static inline void HarnessSetConfigFlags(unsigned int flags)
{
    SetConfigFlags((flags & ~(unsigned int)(FLAG_WINDOW_RESIZABLE | FLAG_FULLSCREEN_MODE | FLAG_VSYNC_HINT)) | FLAG_WINDOW_HIDDEN);
}

static inline void HarnessInitWindow(int width, int height, const char *title)
{
    (void)title;
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(width, height, "monolith-shot");
    harness.target = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
}

static inline void HarnessCloseWindow(void)
{
    if (harness.target.id) UnloadRenderTexture(harness.target);
    harness.target = (RenderTexture){ 0 };
    CloseWindow();
}

static inline void HarnessEndDrawing(void)
{
    EndDrawing();
    if (harness.done || ++harness.frame < HARNESS_FRAMES) return;
    Image img = LoadImageFromTexture(harness.target.texture);
    ImageFlipVertical(&img);                                // GL textures are bottom-up
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);     // no alpha, like monolith --shot
    bool ok = ExportImage(img, harness.path);
    UnloadImage(img);
    printf("%s %s\n", ok ? "saved" : "failed to save", harness.path);
    fflush(stdout);
    harness.done = true;
}

static inline bool HarnessWindowShouldClose(void) { return harness.done || WindowShouldClose(); }
static inline float HarnessGetFrameTime(void) { return 1.0f / 60.0f; }
static inline void HarnessNoInt(int v) { (void)v; }
static inline void HarnessNoArgs(void) { }

static inline void HarnessBeginPro(R3D_View view)
{
    if (view.target.id == 0) view.target = harness.target;
    R3D_BeginPro(view);
}

static inline void HarnessBeginEx(R3D_Camera camera)
{
    HarnessBeginPro((R3D_View){ .camera = camera, .target = harness.target });
}

static inline void HarnessBegin(Camera3D camera)
{
    HarnessBeginPro((R3D_View){ .camera = R3D_CameraFromRL(camera), .target = harness.target });
}

static inline bool HarnessDraw(MS_Scene *scene, Camera3D camera, RenderTexture target)
{
    return MS_Draw(scene, camera, target.id ? target : harness.target);
}

// The example's main, renamed (HARNESS_MAIN_NO_ARGS: it is `int main(void)`)
#ifdef HARNESS_MAIN_NO_ARGS
int HarnessExampleMain(void);
#define HARNESS_CALL_MAIN(argc, argv) ((void)(argc), (void)(argv), HarnessExampleMain())
#else
int HarnessExampleMain(int argc, char **argv);
#define HARNESS_CALL_MAIN(argc, argv) HarnessExampleMain((argc), (argv))
#endif

// --shot <file> is the harness's; every other argument goes to the example
int main(int argc, char **argv)
{
    char **args = malloc(sizeof(char *) * (size_t)(argc + 1));
    if (!args) return 1;
    int n = 0;
    for (int i = 0; i < argc; i++) {
        if (i > 0 && strcmp(argv[i], "--shot") == 0 && i + 1 < argc) harness.path = argv[++i];
        else args[n++] = argv[i];
    }
    args[n] = NULL;
    if (!harness.path) {
        fprintf(stderr, "%s: test build: needs --shot <file> (it never opens a visible window; the plain example is "
                        "examples/build/<platform>/)\n", argv[0]);
        free(args);
        return 1;
    }
    int status = HARNESS_CALL_MAIN(n, args);
    free(args);
    return status;
}

#define main                HarnessExampleMain
#define SetConfigFlags      HarnessSetConfigFlags
#define InitWindow          HarnessInitWindow
#define CloseWindow         HarnessCloseWindow
#define EndDrawing          HarnessEndDrawing
#define WindowShouldClose   HarnessWindowShouldClose
#define GetFrameTime        HarnessGetFrameTime
#define SetTargetFPS        HarnessNoInt
#define DisableCursor       HarnessNoArgs
#define EnableCursor        HarnessNoArgs
#undef R3D_Begin
#undef R3D_BeginEx
#undef R3D_BeginPro
#define R3D_Begin           HarnessBegin
#define R3D_BeginEx         HarnessBeginEx
#define R3D_BeginPro        HarnessBeginPro
#define MS_Draw             HarnessDraw

#endif
