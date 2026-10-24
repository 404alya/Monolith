// shot.h -- hidden-window screenshots for the examples (`--shot <file>`), header-only
//
// Every example accepts `--shot <file>`: it then opens a hidden window that is never mapped
// (titled "monolith-shot", so a window manager rule can route it, e.g. i3:
// for_window [title="^monolith-(bench|shot)$"] move container to workspace number 10),
// renders SHOT_FRAMES frames into a RenderTexture, saves the last one and exits. A hidden
// window has no visible pixels, so reading the window back (TakeScreenshot) would be
// undefined: R3D renders into the texture instead (R3D_BeginPro with a target).
//
//   Shot shot = ShotParse(argc, argv);
//   ShotInitWindow(&shot, 1280, 720, "my game");          // instead of InitWindow
//   ... R3D_Init ...
//   ShotBegin(&shot);                                     // after R3D_Init: the target
//   while (!WindowShouldClose()) {
//       ... R3D_BeginPro((R3D_View){ R3D_CameraFromRL(cam), shot.target }) ... R3D_End();
//       if (ShotFrameDone(&shot)) break;                  // after EndDrawing
//   }
//   ShotEnd(&shot);
#ifndef EXAMPLES_SHOT_H
#define EXAMPLES_SHOT_H

#include <raylib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHOT_FRAMES 30

typedef struct {
    const char *path;       // NULL = normal interactive run
    int frame;
    RenderTexture target;   // zero = the window
} Shot;

// True in an AI agent's shell (Claude Code sets CLAUDECODE=1, other agents AI_AGENT), where an
// example may only run with --shot: a visible window would land on the developer's screen.
// MONOLITH_ALLOW_WINDOW=1 lets a person open it anyway.
static inline bool ShotAgentShell(void)
{
    const char *c = getenv("CLAUDECODE"), *a = getenv("AI_AGENT"), *allow = getenv("MONOLITH_ALLOW_WINDOW");
    bool agent = (c && c[0] && strcmp(c, "0") != 0) || (a && a[0] && strcmp(a, "0") != 0);
    return agent && !(allow && allow[0] && strcmp(allow, "0") != 0);
}

// Parses --shot, and refuses (exit 1) an interactive run in an AI agent's shell. Call it
// first, before anything is loaded.
static inline Shot ShotParse(int argc, char **argv)
{
    Shot s = { 0 };
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--shot") == 0) s.path = argv[i + 1];
    if (!s.path && ShotAgentShell()) {
        fprintf(stderr, "refusing to open a window from an AI agent's shell: use --shot <file> "
                        "(or set MONOLITH_ALLOW_WINDOW=1)\n");
        exit(1);
    }
    return s;
}

static inline void ShotInitWindow(const Shot *s, int width, int height, const char *title)
{
    SetConfigFlags(FLAG_MSAA_4X_HINT | (s->path ? FLAG_WINDOW_HIDDEN : FLAG_WINDOW_RESIZABLE));
    InitWindow(width, height, s->path ? "monolith-shot" : title);
    if (!s->path) SetTargetFPS(60);
}

static inline void ShotBegin(Shot *s)
{
    if (s->path) s->target = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
}

// Call once per frame after EndDrawing; true when the shot is saved and the program should exit
static inline bool ShotFrameDone(Shot *s)
{
    if (!s->path || ++s->frame < SHOT_FRAMES) return false;
    Image img = LoadImageFromTexture(s->target.texture);
    ImageFlipVertical(&img);                                // GL textures are bottom-up
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
    bool ok = ExportImage(img, s->path);
    UnloadImage(img);
    printf("%s %s\n", ok ? "saved" : "failed to save", s->path);
    return true;
}

static inline void ShotEnd(Shot *s)
{
    if (s->target.id) UnloadRenderTexture(s->target);
}

#endif
