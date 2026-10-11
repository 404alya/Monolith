// Window emulation for headless benchmarks (--bench-window <samples>).
//
// A headless bench renders R3D into a plain single-sample texture and draws no HUD, while the
// visible window is a 4x multisampled default framebuffer (FLAG_MSAA_4X_HINT) that R3D's final
// pass writes into, the HUD / designer overlay draw on top of, and the swap (or, under a
// compositor, its copy of the window) resolves. This module builds that framebuffer offscreen,
// so the window-only costs can be timed without a visible window: --bench-window 4 = today's
// window, --bench-window 0 = the same without multisampling. The bench then also draws the
// spectator HUD and overlay as the interactive editor does.
//
// The multisample renderbuffer calls are not in rlgl, so they come from GLFW's loader (part of
// raylib; same calls on Windows, where opengl32.dll only exports GL 1.1).

#include "windowemu.h"

typedef void (*GLProc)(void);
extern GLProc glfwGetProcAddress(const char *name);

#define EMU_GL_FRAMEBUFFER 0x8D40
#define EMU_GL_RENDERBUFFER 0x8D41
#define EMU_GL_READ_FRAMEBUFFER 0x8CA8
#define EMU_GL_DRAW_FRAMEBUFFER 0x8CA9
#define EMU_GL_COLOR_ATTACHMENT0 0x8CE0
#define EMU_GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define EMU_GL_RGBA8 0x8058
#define EMU_GL_DEPTH24_STENCIL8 0x88F0
#define EMU_GL_COLOR_BUFFER_BIT 0x4000
#define EMU_GL_NEAREST 0x2600
#define EMU_GL_FRAMEBUFFER_COMPLETE 0x8CD5

static struct {
    void (*GenFramebuffers)(int, unsigned int *);
    void (*DeleteFramebuffers)(int, const unsigned int *);
    void (*BindFramebuffer)(unsigned int, unsigned int);
    void (*GenRenderbuffers)(int, unsigned int *);
    void (*DeleteRenderbuffers)(int, const unsigned int *);
    void (*BindRenderbuffer)(unsigned int, unsigned int);
    void (*RenderbufferStorageMultisample)(unsigned int, int, unsigned int, int, int);
    void (*FramebufferRenderbuffer)(unsigned int, unsigned int, unsigned int, unsigned int);
    unsigned int (*CheckFramebufferStatus)(unsigned int);
    void (*BlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned int, unsigned int);
} gl;

static bool LoadGl(void)
{
#define LOAD(name) do { GLProc f = glfwGetProcAddress("gl" #name); if (!f) return false; memcpy(&gl.name, &f, sizeof(f)); } while (0)
    LOAD(GenFramebuffers); LOAD(DeleteFramebuffers); LOAD(BindFramebuffer);
    LOAD(GenRenderbuffers); LOAD(DeleteRenderbuffers); LOAD(BindRenderbuffer);
    LOAD(RenderbufferStorageMultisample); LOAD(FramebufferRenderbuffer);
    LOAD(CheckFramebufferStatus); LOAD(BlitFramebuffer);
#undef LOAD
    return true;
}

static unsigned int Renderbuffer(unsigned int format, int samples, int w, int h)
{
    unsigned int rb = 0;
    gl.GenRenderbuffers(1, &rb);
    gl.BindRenderbuffer(EMU_GL_RENDERBUFFER, rb);
    gl.RenderbufferStorageMultisample(EMU_GL_RENDERBUFFER, samples, format, w, h);
    gl.BindRenderbuffer(EMU_GL_RENDERBUFFER, 0);
    return rb;
}

bool WindowEmuLoad(WindowEmu *w, int width, int height, int samples)
{
    memset(w, 0, sizeof(*w));
    if (!LoadGl()) {
        TraceLog(LOG_WARNING, "monolith: --bench-window: GL entry points missing");
        return false;
    }
    rlDrawRenderBatchActive();
    w->samples = samples;
    w->color = Renderbuffer(EMU_GL_RGBA8, samples, width, height);
    w->depth = Renderbuffer(EMU_GL_DEPTH24_STENCIL8, samples, width, height);
    unsigned int fbo = 0;
    gl.GenFramebuffers(1, &fbo);
    gl.BindFramebuffer(EMU_GL_FRAMEBUFFER, fbo);
    gl.FramebufferRenderbuffer(EMU_GL_FRAMEBUFFER, EMU_GL_COLOR_ATTACHMENT0, EMU_GL_RENDERBUFFER, w->color);
    gl.FramebufferRenderbuffer(EMU_GL_FRAMEBUFFER, EMU_GL_DEPTH_STENCIL_ATTACHMENT, EMU_GL_RENDERBUFFER, w->depth);
    bool ok = gl.CheckFramebufferStatus(EMU_GL_FRAMEBUFFER) == EMU_GL_FRAMEBUFFER_COMPLETE;

    w->resolveColor = Renderbuffer(EMU_GL_RGBA8, 0, width, height);
    gl.GenFramebuffers(1, &w->resolveFbo);
    gl.BindFramebuffer(EMU_GL_FRAMEBUFFER, w->resolveFbo);
    gl.FramebufferRenderbuffer(EMU_GL_FRAMEBUFFER, EMU_GL_COLOR_ATTACHMENT0, EMU_GL_RENDERBUFFER, w->resolveColor);
    ok = ok && gl.CheckFramebufferStatus(EMU_GL_FRAMEBUFFER) == EMU_GL_FRAMEBUFFER_COMPLETE;
    gl.BindFramebuffer(EMU_GL_FRAMEBUFFER, 0);

    // A RenderTexture only to carry the id and size: R3D and BeginTextureMode only use those
    w->target = (RenderTexture){ .id = fbo, .texture = { 0, width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 } };
    if (!ok) {
        TraceLog(LOG_WARNING, "monolith: --bench-window: framebuffer incomplete");
        WindowEmuUnload(w);
        return false;
    }
    TraceLog(LOG_INFO, "monolith: --bench-window: %dx%d, %d samples, HUD + overlay drawn", width, height, samples);
    return true;
}

void WindowEmuClear(WindowEmu *w)
{
    BeginTextureMode(w->target);
        ClearBackground(BLACK);
    EndTextureMode();
}

void WindowEmuPresent(WindowEmu *w)
{
    rlDrawRenderBatchActive();
    int width = w->target.texture.width, height = w->target.texture.height;
    gl.BindFramebuffer(EMU_GL_READ_FRAMEBUFFER, w->target.id);
    gl.BindFramebuffer(EMU_GL_DRAW_FRAMEBUFFER, w->resolveFbo);
    gl.BlitFramebuffer(0, 0, width, height, 0, 0, width, height, EMU_GL_COLOR_BUFFER_BIT, EMU_GL_NEAREST);
    gl.BindFramebuffer(EMU_GL_FRAMEBUFFER, 0);
}

void WindowEmuUnload(WindowEmu *w)
{
    if (!gl.DeleteFramebuffers) return;
    unsigned int fbos[2] = { w->target.id, w->resolveFbo };
    unsigned int rbs[3] = { w->color, w->depth, w->resolveColor };
    gl.DeleteFramebuffers(2, fbos);
    gl.DeleteRenderbuffers(3, rbs);
    memset(w, 0, sizeof(*w));
}
