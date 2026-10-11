// Window emulation for headless benchmarks (--bench-window <samples>)
#ifndef MONOLITH_WINDOWEMU_H
#define MONOLITH_WINDOWEMU_H

#include "common.h"

// An offscreen stand-in for the visible window's default framebuffer: a colour + depth/stencil
// framebuffer with `samples` samples per pixel (4 = what FLAG_MSAA_4X_HINT gives the window,
// 0 = a single-sample one), plus a single-sample copy that WindowEmuPresent() resolves into,
// as the swap / the compositor's copy does with the window's back buffer.
typedef struct {
    RenderTexture target;       // R3D_BeginPro / BeginTextureMode target (texture.id is 0)
    unsigned int color, depth;  // renderbuffers
    unsigned int resolveFbo, resolveColor;
    int samples;
} WindowEmu;

// false if the GL entry points or the framebuffer are missing (then the run uses the plain
// headless target)
bool WindowEmuLoad(WindowEmu *w, int width, int height, int samples);
// Clears colour and depth, like BeginDrawing + ClearBackground does for the window
void WindowEmuClear(WindowEmu *w);
// Resolves / copies the frame into the single-sample buffer (the "swap")
void WindowEmuPresent(WindowEmu *w);
void WindowEmuUnload(WindowEmu *w);

#endif
