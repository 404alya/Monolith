// monolith -- AI-assisted map designer with deterministic edits, for 3D levels of any style (raylib + R3D)
//
// The editor's entry point: parses the command line, runs the main loop (walking, spectator,
// designer, edit and draw modes, panels) over a map package that the scene runtime loads and
// hot-reloads. Every flag is checked before any window opens (a bad one exits 1); the flags
// that only edit or reach the AI are refused in the view-only build (MONOLITH_EDITING 0).
// The keys, every flag and the environment variables are listed in the reference below.
// Reference: docs/code/main.md

#include "common.h"
#include "player.h"
#include "spectator.h"
#include "atmosphere.h"
#include "bench.h"
#include "designer.h"
#include "launcher.h"
#include "newmap.h"
#include "export.h"
#include "inspect.h"
#include "bake.h"
#include "dumpscene.h"
#include "trireport.h"
#include "mapedit.h"
#include "editmode.h"
#include "windowemu.h"
#include "settings.h"
#include "settingspanel.h"
#include "inspector.h"
#include "idpanel.h"
#include "keyspanel.h"
#include "modelpicker.h"
#include "drawpanel.h"
#include "ui.h"
#include "png.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <time.h>

// Only needed for glFinish() for --headless (depth clamp: MS_InitRenderer). Declared by hand
// rather than via <GL/gl.h> / <windows.h> (which clashes with raylib). On Windows x64 there
// is a single calling convention, so this matches opengl32.dll.
extern void glFinish(void);
// (and the screen eyedropper's 1 x 1 read: GL 1.1 as well)
extern void glReadPixels(int x, int y, int width, int height, unsigned int format, unsigned int type, void *pixels);
extern unsigned int glGetError(void);

#ifndef BENCHMARK_PATH
#   define BENCHMARK_PATH "./benchmark/"
#endif
#ifndef MAPS_PATH
#   define MAPS_PATH "./maps/"
#endif
#ifndef SCREENSHOTS_PATH
#   define SCREENSHOTS_PATH "./screenshots/"
#endif
#ifndef PROJECT_PATH
#   define PROJECT_PATH ""
#endif

// Data directories, with a trailing slash. Both builds are meant to be copied to other
// computers as a folder (build/linux/, build/win/), so they look next to the executable.
// On Linux, the repo directories baked in by build.sh win while they exist (i.e. in a
// repo checkout), so bench reports land in the repo's benchmark/, and the map package that
// gets hot-reloaded (with its shaders) is the repo's maps/ one.
// Windows paths aren't baked in (wine would see Z:\...).
char benchmarkDir[1024];
static char mapsDir[1024];  // only main() needs it (default map, --new-map)
static char screenshotsDir[1024];   // F12's screenshots (created on the first one)
MS_Scene *scene;

static void InitDataPaths(void)
{
    const char *exeDir = GetApplicationDirectory();
#ifdef _WIN32
    snprintf(benchmarkDir, sizeof(benchmarkDir), "%sbenchmark/", exeDir);
    snprintf(mapsDir, sizeof(mapsDir), "%smaps/", exeDir);
    snprintf(screenshotsDir, sizeof(screenshotsDir), "%sscreenshots/", exeDir);
#else
    if (DirectoryExists(BENCHMARK_PATH)) snprintf(benchmarkDir, sizeof(benchmarkDir), "%s", BENCHMARK_PATH);
    else snprintf(benchmarkDir, sizeof(benchmarkDir), "%sbenchmark/", exeDir);
    if (DirectoryExists(MAPS_PATH)) snprintf(mapsDir, sizeof(mapsDir), "%s", MAPS_PATH);
    else snprintf(mapsDir, sizeof(mapsDir), "%smaps/", exeDir);
    // (screenshots/ appears on the first F12: the repo root existing is enough)
    if (DirectoryExists(SCREENSHOTS_PATH) || (PROJECT_PATH[0] && DirectoryExists(PROJECT_PATH)))
        snprintf(screenshotsDir, sizeof(screenshotsDir), "%s", SCREENSHOTS_PATH);
    else snprintf(screenshotsDir, sizeof(screenshotsDir), "%sscreenshots/", exeDir);
#endif
}

// F12: the last frame on screen as screenshots/<date>_<time>.png. raylib's TakeScreenshot
// drops any folder from the name and writes to the working directory, so the frame is read
// and written here. path receives the file written. Returns false (path empty) on failure.
static bool SaveScreenshot(char *path, size_t size)
{
    path[0] = '\0';
    if (!DirectoryExists(screenshotsDir) && MakeDirectory(screenshotsDir) != 0) return false;
    char stamp[32] = "screenshot";
    time_t now = time(NULL);
    const struct tm *tm = localtime(&now);
    if (tm) strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", tm);
    snprintf(path, size, "%s%s.png", screenshotsDir, stamp);
    for (int n = 2; FileExists(path); n++)     // (two in the same second)
        snprintf(path, size, "%s%s_%d.png", screenshotsDir, stamp, n);
    Image img = LoadImageFromScreen();
    bool ok = img.data && PngWrite(img, path);
    UnloadImage(img);
    if (!ok) path[0] = '\0';
    return ok;
}

static bool DumpBoxes(const MS_Scene *s, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    for (int i = 0; i < s->boxCount; i++) {
        BoundingBox b = s->boxes[i];
        fprintf(f, "%a %a %a %a %a %a", b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
        if (s->boxTurned && s->boxTurned[i] >= 0) {     // a turned box: its AABB, then its rotation and shape
            const MS_TurnedBox *t = &s->turnedBoxes[s->boxTurned[i]];
            fprintf(f, " rot %a %a %a center %a %a %a size %a %a %a", t->rotationDeg.x, t->rotationDeg.y, t->rotationDeg.z,
                    t->center.x, t->center.y, t->center.z, t->size.x, t->size.y, t->size.z);
        }
        fputc('\n', f);
    }
    return fclose(f) == 0;
}

// Renders the scene into `target` (zero = the window), so the main view and the request
// images (Submit) go through the same code. Returns whether a sun shadow map was
// re-rendered. Extra renders (Submit) go through the same cache check with their own
// target's aspect, so the cached map always matches what R3D last rendered. `depth`: the
// scene depth goes into the target too, for overlays depth-tested against it (R3D patch 24:
// the copy costs ~1.9 ms per frame on an integrated GPU).
static double lastR3DEndTime;   // seconds, set by DrawScene

static bool DrawScene(Camera3D camera, RenderTexture target, bool depth)
{
    float aspect = target.id ? (float)target.texture.width / target.texture.height
                             : (float)GetRenderWidth() / GetRenderHeight();
    R3D_BeginPro((R3D_View){ .camera = R3D_CameraFromRL(camera), .target = target });
        bool update = MS_PushLights(scene, camera, aspect);
        MS_DrawGeometry(scene);     // (also bakes the --fog-cache volume once per map load)
    double r3dEndStart = GetTime();
    R3D_SetOutputDepth(depth);
    R3D_End();
    lastR3DEndTime = GetTime() - r3dEndStart;   // CPU side of R3D_End, for the bench report
    return update;
}

// ======================================================================================
// #region Environment: the map's, then the command line overrides, then the 1-6 toggles
// ======================================================================================
// The toggles flip an effect relative to the map's setting (+ --ao/--bloom/--fx), and the
// flips are kept across hot reloads: a reload re-applies the map's environment and must not
// silently turn a toggled-off effect back on.

typedef struct {
    const GfxSettings *gfx;     // the graphics settings of this run (file + flags + panel)
    const char **fx;            // the --fx arguments (argv strings)
    int fxCount;
    bool flip[6];               // keys 1-6: volumetric fog, fog, SSAO, SSIL, bloom, sun
} EffectState;

// The map's environment, then the quality settings (runtime MS_ApplyQuality: --ao, --bloom,
// --render-scale, ... and the settings panel land there), then --fx, then the toggles; and
// R3D's resolution for the (possibly new) render scale
static void ApplyEnvironment(const EffectState *fx)
{
    MS_ApplyEnvironment(scene);
    MS_ApplyQuality(scene, &fx->gfx->q);
    R3D_Environment *env = R3D_GetEnvironment();
    for (int k = 0; k < fx->fxCount; k++) ApplyFxSetting(env, fx->fx[k]);

    // Flipping a mode that is off turns on the mode the default map uses
    if (fx->flip[0]) env->volumetricFog.enabled = !env->volumetricFog.enabled;
    if (fx->flip[1]) env->fog.mode = env->fog.mode ? R3D_FOG_DISABLED : R3D_FOG_EXP2;
    if (fx->flip[2]) env->ssao.enabled = !env->ssao.enabled;
    if (fx->flip[3]) env->ssil.enabled = !env->ssil.enabled;
    if (fx->flip[4]) env->bloom.mode = env->bloom.mode ? R3D_BLOOM_DISABLED : R3D_BLOOM_MIX;
    MS_RenderSettings rs = MS_GetRenderSettings(scene);
    rs.sunOn = !fx->flip[5];
    MS_SetRenderSettings(scene, rs);
    MS_ApplyRenderResolution(scene, GetScreenWidth(), GetScreenHeight());
}

// Shadow map size is an R3D init hint: a new size closes and re-initialises R3D and loads the
// map again (~1 s). Returns 0: restarted; 1: not restarted, nothing changed (the map on disk
// doesn't load: why in MS_LastError()); 2: R3D was closed but the map then failed to load (why in
// MS_LastError(); the editor has to quit). The caller reports it (its log and the settings panel).
static unsigned char RestartRenderer(MS_LoadOptions *opt, int shadowSize)
{
    char path[MS_PATH_MAX];
    snprintf(path, sizeof(path), "%s", scene->path);
    MS_LoadOptions peekOpt = *opt;
    peekOpt.noGpu = true;
    peekOpt.shadowSizeOverride = shadowSize;
    MS_Scene *peek = MS_Load(path, &peekOpt);
    if (!peek) return 1;
    MS_Unload(scene);
    scene = NULL;
    R3D_Close();
    MS_SetInitHints(peek);
    MS_Unload(peek);
    R3D_Init(GetScreenWidth(), GetScreenHeight());
    R3D_SetAntiAliasingMode(R3D_ANTI_ALIASING_MODE_FXAA);
    MS_InitRenderer();
    opt->shadowSizeOverride = shadowSize;
    opt->noGpu = false;
    scene = MS_Load(path, opt);
    if (!scene) return 2;
    return 0;
}

// fps cap of the settings: 0 = uncapped, -1 = the monitor's refresh rate
static int FpsTarget(int cap)
{
    if (cap >= 0) return cap;
    int hz = GetMonitorRefreshRate(GetCurrentMonitor());
    return hz > 0 ? hz : 60;
}

// #endregion

// ======================================================================================
// #region Frame reuse
// ======================================================================================
// A still camera over an unchanged scene produces the same R3D image every frame: R3D has
// no temporal effects in this setup (no TAA, no auto exposure, no history buffers; SSAO,
// SSIL and fog noise are keyed on screen position only, and the per-frame TIME /
// FRAME_INDEX only reach surface shaders, which the default map's concrete.glsl doesn't
// read; a map shader animated by TIME would freeze while the camera is still). So when nothing
// that feeds R3D changed since the last frame, the frame is composited from a cached copy
// of R3D's colour + depth instead of re-rendering it.
//   - While the key changes, R3D renders straight into the window as before (no extra
//     copy on moving frames). On the first unchanged frame R3D renders once more into the
//     cache (that frame is also the "settle" frame: the sun shadow map was already updated
//     for this camera, if at all, so nothing converges any further). Every later unchanged
//     frame only composites the cache and draws the HUD / overlay on top.
//   - The cache keeps R3D's depth in a depth *texture*, and the composite writes it back
//     with gl_FragDepth, so the designer overlay depth-tests exactly as over a fresh frame.
//     A shader pass rather than glBlitFramebuffer, because the window's framebuffer can be
//     multisampled (--window-msaa), and blitting into a multisampled framebuffer is an
//     error; R3D's own final pass is a fullscreen shader pass for the same reason.

// Everything that changes R3D's image. Compared bytewise, so floats compare exactly; a
// false "changed" (e.g. -0 vs 0) only costs one render.
// Everything else the image depends on (materials, lights, sun, sky, shaders, textures)
// comes from the map package, and any change there is a reload, i.e. a new scene version.
typedef struct {
    Camera3D camera;
    R3D_Environment env;    // every effect setting (the map's, --ao/--bloom/--fx, toggles 1-5)
    MS_RenderSettings render;   // toggle 6 (sunOn), shadow mode, fog cache, pre-pass, render scale
    GfxSettings gfx;            // every graphics setting (most also show up in env / render)
    int antiAliasing;           // R3D's AA mode (not part of the environment)
    unsigned int sceneVersion;
    int width, height;      // R3D resolution = window size
    int outputMode;             // the render inspector's buffer view (R3D_SetOutputMode, F4)
    unsigned int filterGeneration;  // and its draw filter (+1 on every hide / show / isolate)
    unsigned int textureGeneration; // painted texels (MS_TextureUpdate: no new scene version)
} FrameKey;

typedef struct {
    bool enabled;           // default: interactive window only; --no-frame-reuse / --frame-reuse
    bool keyValid, cacheValid;
    FrameKey key;
    RenderTexture rt;       // colour texture + depth texture (not a renderbuffer: it gets sampled)
    Shader composite;
    int depthLoc;
} FrameReuse;

// Fills every byte of the key (padding included), so memcmp is meaningful
static void MakeFrameKey(FrameKey *k, Camera3D camera, const GfxSettings *gfx, const Inspector *in)
{
    memset(k, 0, sizeof(*k));
    memcpy(&k->camera, &camera, sizeof(camera));
    memcpy(&k->env, R3D_GetEnvironment(), sizeof(k->env));
    MS_RenderSettings rs = MS_GetRenderSettings(scene);
    memcpy(&k->render, &rs, sizeof(rs));
    memcpy(&k->gfx, gfx, sizeof(*gfx));
    k->antiAliasing = (int)R3D_GetAntiAliasingMode();
    k->sceneVersion = scene->version;
    k->width = GetScreenWidth();
    k->height = GetScreenHeight();
    k->outputMode = (int)R3D_GetOutputMode();
    k->filterGeneration = in->filterGeneration;
    k->textureGeneration = MS_TextureGeneration(scene);
}

// --pick / --pick-at: where on the surface the ray hits (MS_RaycastSurface: the face or the model
// triangle, the texture coordinate; docs/painting.md 6), after DesignerPrintHit's lines
static void PrintSurfaceHit(Vector3 origin, Vector3 dir)
{
    MS_SurfaceHit h;
    if (!MS_RaycastSurface(scene, origin, dir, 0.0f, &h)) return;
    static const char *const FACE[] = { "+x", "-x", "+y", "-y", "+z", "-z" };
    printf("pick: surface material %d \"%s\"  face %s  slot %d  mesh %d  triangle %d  faceUv %.4f %.4f  uv %.4f %.4f"
           "  bary %.3f %.3f %.3f  paint grid %g texel %.3f %.3f cells %dx%d\n", h.material, h.material >= 0 &&
           h.material < scene->materialCount ? scene->materials[h.material].name : "(own)", h.face >= 0 && h.face < 6 ? FACE[h.face]
           : "-", h.slot, h.mesh, h.triangle, h.faceUv.x, h.faceUv.y, h.uv.x, h.uv.y, h.bary.x, h.bary.y, h.bary.z, h.paintGrid,
           h.paintTexel.x, h.paintTexel.y, h.paintCellsW, h.paintCellsH);
}

static RenderTexture LoadSceneCache(int w, int h)
{
    RenderTexture rt = { 0 };
    rt.id = rlLoadFramebuffer();
    rt.texture = (Texture2D){ rlLoadTexture(NULL, w, h, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1), w, h, 1,
                              PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    rt.depth = (Texture2D){ rlLoadTextureDepth(w, h, false), w, h, 1, 19 };
    rlFramebufferAttach(rt.id, rt.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    rlFramebufferAttach(rt.id, rt.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    if (!rlFramebufferComplete(rt.id)) TraceLog(LOG_WARNING, "monolith: frame reuse cache framebuffer incomplete");
    rlDisableFramebuffer();
    return rt;
}

// Copies colour and depth texel by texel (same size as the destination, so no filtering
// and no flip: gl_FragCoord addresses both)
static const char *COMPOSITE_FS =
    "#version 330\n"
    "uniform sampler2D texture0;\n"     // cached colour (bound by DrawTexture)
    "uniform sampler2D depthTex;\n"     // cached depth
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    finalColor = texelFetch(texture0, p, 0);\n"
    "    gl_FragDepth = texelFetch(depthTex, p, 0).r;\n"
    "}\n";

// Draws the cached frame into the current target (window or BeginTextureMode), after a
// clear of colour and depth. Blending off: R3D's own blit writes its alpha unblended too.
static void DrawSceneCache(FrameReuse *fr)
{
    BeginShaderMode(fr->composite);
        SetShaderValueTexture(fr->composite, fr->depthLoc, fr->rt.depth);
        rlDisableColorBlend();
        rlEnableDepthTest();    // depth func is GL_LEQUAL (R3D leaves it so) and the clear wrote 1
        DrawTexture(fr->rt.texture, 0, 0, WHITE);
        rlDrawRenderBatchActive();
        rlDisableDepthTest();
        rlEnableColorBlend();
    EndShaderMode();
}

// #endregion

// Render target -> top-down RGB image (GL textures are bottom-up)
static Image ReadTarget(RenderTexture rt)
{
    Image img = LoadImageFromTexture(rt.texture);
    ImageFlipVertical(&img);
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);     // drop alpha
    return img;
}

// The screen eyedropper (docs/painting.md 18.8): the pixel at window position (x, y) (top-down) of
// the scene just drawn into `fbo` (0: the window's back buffer, fbW x fbH: GetRenderWidth/Height, a
// HiDPI window's being larger than its screen size; a multisampled one is resolved by the read
// itself), as RGBA8 bytes: the same bytes ReadTarget and the --shot PNG get. One 1 x 1 read, only
// when a click asked for it. False: outside the window, or GL refused (a multisampled offscreen
// target: --bench-window, which never has the draw mode)
static bool ReadScenePixel(unsigned int fbo, int fbW, int fbH, int x, int y, Color *out)
{
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    if (x < 0 || y < 0 || x >= sw || y >= sh || fbW <= 0 || fbH <= 0) return false;
    int px = (int)((long long)x * fbW / sw), py = (int)((long long)y * fbH / sh);
    rlDrawRenderBatchActive();      // (anything raylib still holds is in the framebuffer first)
    for (int k = 0; k < 32 && glGetError() != 0; k++) {}   // (earlier errors aren't this read's)
    rlBindFramebuffer(RL_READ_FRAMEBUFFER, fbo);
    unsigned char rgba[4] = { 0 };
    glReadPixels(px, fbH - 1 - py, 1, 1, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, rgba);   // (GL: bottom-up)
    bool ok = glGetError() == 0;
    rlBindFramebuffer(RL_READ_FRAMEBUFFER, 0);
    *out = (Color){ rgba[0], rgba[1], rgba[2], rgba[3] };
    return ok;
}

// --paint-script `pick screen` (DrawMode.screenRead): the scene drawn with the shot's camera into
// the shot's target (no HUD), as the frame loop draws the shot's scene, and that pixel read. `fx`
// (the --apply-edits path, which runs before the main loop's environment): applied on the first read
typedef struct {
    Camera3D camera;
    RenderTexture target;       // (id 0: loaded at the window's size on the first read, then owned)
    bool own;
    const EffectState *fx;
} ScreenReadCtx;

static bool ScreenRead(void *user, int x, int y, Color *out)
{
    ScreenReadCtx *c = user;
    if (c->fx) {
        ApplyEnvironment(c->fx);
        c->fx = NULL;
    }
    if (!c->target.id) {
        c->target = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
        if (!c->target.id) return false;
        c->own = true;
    }
    DrawScene(c->camera, c->target, false);
    return ReadScenePixel(c->target.id, c->target.texture.width, c->target.texture.height, x, y, out);
}

// A request: view.png is the frame exactly as --shot renders it (offscreen, no HUD),
// view-marked.png the same pixels plus the selection, hit point and crosshair. One R3D
// render into `rt`, read back before and after drawing the overlay on top (R3D_End leaves
// its depth in rt, so the overlay is depth-tested like in the live view).
static unsigned char Submit(Designer *d, Camera3D cam, float yaw, float pitch, RenderTexture rt)
{
    // Window mode: an offscreen target at R3D's resolution (the window size), like --shot
    DesignRequest req;
    if (!DesignerBuildRequest(d, cam, yaw, pitch, rt.id ? rt.texture.width : GetScreenWidth(),
                              rt.id ? rt.texture.height : GetScreenHeight(), &req)) {
        MS_SetError("out of memory: request not sent");
        DesignerSubmitDone(d, false, MS_LastError());
        return 1;
    }
    bool own = rt.id == 0;
    if (own) rt = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
    DrawScene(cam, rt, true);
    Image view = ReadTarget(rt);
    BeginTextureMode(rt);
        DesignerDrawOverlay(d, cam, false);
        DesignerDrawCrosshair(rt.texture.width, rt.texture.height);
        DesignerDrawRegions(d, rt.texture.width, rt.texture.height, true);
        DesignerDrawVolumes(d, cam, rt.texture.width, rt.texture.height, true);
    EndTextureMode();
    Image marked = ReadTarget(rt);

    unsigned char r = LauncherSubmit(&req, view, marked);
    const char *text = r == 0 ? LauncherNote() : MS_LastError();   // 0: queued / sent
    TraceLog(r == 0 ? LOG_INFO : LOG_WARNING, "monolith: submit: %s", text);
    DesignerSubmitDone(d, r == 0, text);    // (copies it)
    DesignerFreeRequest(&req);
    UnloadImage(view);
    UnloadImage(marked);
    if (own) UnloadRenderTexture(rt);
    return r;
}

// Command line text for --submit / --type: a literal \n becomes a new line. A heap string
// (free it); on out of memory the text as given (a copy as well), or NULL
static char *Unescape(const char *in)
{
    char *out = malloc(strlen(in) + 1);
    if (!out) return NULL;
    int n = 0;
    for (const char *c = in; *c; c++) {
        if (c[0] == '\\' && c[1] == 'n') { out[n++] = '\n'; c++; }
        else out[n++] = *c;
    }
    out[n] = 0;
    return out;
}

// Heap memory of main() that lives until it returns (freed by main for every return path):
// the repeatable command-line arguments (sized by argc) and the --bench tables
static struct {
    const char **fx;            // --fx key=value
    const char **settingsSet;   // --settings-set key=value
    const char **settingsChange;    // --settings-change key=value
    int *selectLines;           // --select [<file>:]<line>: the line
    const char **selectFiles;   //   and the argument when it names a file (NULL: the main file)
    Rectangle *regions;         // --region x y w h
    BoundingBox *volumes;       // --volume x0 y0 z0 x1 y1 z1
    Vector2 *pickAt;            // --pick-at x y
    double *benchTimes;         // [view * benchFrames + frame]
    int *benchShadowUpdates;    // per view
} mainHeap;

static void FreeMainHeap(void)
{
    free(mainHeap.fx);
    free(mainHeap.settingsSet);
    free(mainHeap.settingsChange);
    free(mainHeap.selectLines);
    free(mainHeap.selectFiles);
    free(mainHeap.regions);
    free(mainHeap.volumes);
    free(mainHeap.pickAt);
    free(mainHeap.benchTimes);
    free(mainHeap.benchShadowUpdates);
    memset(&mainHeap, 0, sizeof(mainHeap));
}

// The draw mode's kept state (docs/painting.md 18.6): <package>/draw.ini in the editor, --draw-state's
// file in any run (a hidden run without it reads and writes none). Each frame compares the draw mode
// with what the file holds (DrawModeStateDiffers: no allocation); a change is copied (the palette's
// memory grows only when it does) and written 1 s after the last one, when draw mode is left and at
// exit. A failed write is logged by DrawStateSave, once per change (the next change tries again).
typedef struct {
    char *path;                 // heap; NULL: no file this run
    DrawState saved;            // what the file holds, or will once the change waiting is written
    bool pending;               // a change not written yet
    double changedAt;           // when the last change was seen (GetTime)
    bool wasActive;             // draw mode was on last frame
} DrawKeep;

static void DrawKeepLoad(DrawKeep *k, DrawMode *dm)
{
    if (!k->path) return;
    // (from the draw mode's state: a missing file or a key it lacks keeps what the draw mode has,
    // DrawModeInit's defaults, or a --draw-panel shot's recent colours)
    DrawModeGetState(dm, &k->saved);
    int ignored = 0;
    if (DrawStateLoad(&k->saved, k->path, &ignored) && ignored > 0)
        TraceLog(LOG_WARNING, "monolith: draw state %s: %d line%s ignored", k->path, ignored, ignored == 1 ? "" : "s");
    if (!DrawModeSetState(dm, &k->saved)) TraceLog(LOG_WARNING, "monolith: draw state: the palette could not be loaded (out of memory)");
    DrawModeGetState(dm, &k->saved);    // (as the draw mode clamped it: no write for that alone)
    k->wasActive = dm->active;
}

static void DrawKeepWrite(DrawKeep *k)
{
    k->pending = false;
    if (DrawStateSave(&k->saved, k->path)) TraceLog(LOG_INFO, "monolith: draw state saved to %s", k->path);
}

static void DrawKeepTick(DrawKeep *k, const DrawMode *dm, double now)
{
    if (!k->path) return;
    if (DrawModeStateDiffers(dm, &k->saved) && DrawModeGetState(dm, &k->saved)) {
        k->pending = true;
        k->changedAt = now;
    }
    bool left = k->wasActive && !dm->active;
    k->wasActive = dm->active;
    if (k->pending && (left || now - k->changedAt >= 1.0)) DrawKeepWrite(k);
}

static void DrawKeepFree(DrawKeep *k)
{
    if (k->path && k->pending) DrawKeepWrite(k);
    DrawStateFree(&k->saved);
    free(k->path);
    memset(k, 0, sizeof(*k));
}

static int Run(int argc, char **argv);

// True when this process runs in an AI agent's shell: Claude Code sets CLAUDECODE=1 in the
// shells it starts, other agents AI_AGENT. Such runs must never open the editor window on the
// developer's screen (CLAUDE.md guard rail); MONOLITH_ALLOW_WINDOW lets a person override it.
static bool EnvSet(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] && strcmp(v, "0") != 0;
}

static bool AgentShell(void)
{
    return (EnvSet("CLAUDECODE") || EnvSet("AI_AGENT")) && !EnvSet("MONOLITH_ALLOW_WINDOW");
}

// #region Command-line values
// Checks for Run()'s flag loop. Each prints why it failed (stderr) and returns false.

// Camera, look and region numbers must be finite and within this: the sun shadow tiles and
// the region frames convert them to int (QA2 F4/F5)
#define ARG_COORD_LIMIT 1e6

// argv[i] is a flag that takes n values: they must exist and must not be flags themselves
static bool ArgValues(int argc, char **argv, int i, int n)
{
    for (int k = 1; k <= n; k++) {
        if (i + k >= argc) {
            fprintf(stderr, "monolith: %s needs %d value%s\n", argv[i], n, n == 1 ? "" : "s");
            return false;
        }
        if (strncmp(argv[i + k], "--", 2) == 0) {
            fprintf(stderr, "monolith: %s needs %d value%s, got the flag '%s' instead\n", argv[i], n, n == 1 ? "" : "s", argv[i + k]);
            return false;
        }
    }
    return true;
}

static bool ArgNumber(const char *flag, const char *s, double lo, double hi, double *out)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end) {
        fprintf(stderr, "monolith: %s: '%s' is not a number\n", flag, s);
        return false;
    }
    if (!isfinite(v) || v < lo || v > hi) {
        fprintf(stderr, "monolith: %s: %s is out of range (%g .. %g)\n", flag, s, lo, hi);
        return false;
    }
    *out = v;
    return true;
}

// The map-text file a --select argument names before its ':' ("parts/lake.map:41"; package-relative, as
// MS_MapFile.path, a leading "./" allowed): its index in s, 0 for none (the main file), -1 if the map has none
static int SelectFile(const MS_Scene *s, const char *arg)
{
    if (!arg) return 0;
    const char *colon = strrchr(arg, ':');
    while (arg[0] == '.' && arg[1] == '/') arg += 2;
    char *path = MS_StrNDup(arg, (size_t)(colon > arg ? colon - arg : 0));
    int file = path ? MS_MapFileFind(s, path) : -1;
    free(path);
    return file;
}

static bool ArgInt(const char *flag, const char *s, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end) {
        fprintf(stderr, "monolith: %s: '%s' is not a whole number\n", flag, s);
        return false;
    }
    if (errno == ERANGE || v < INT_MIN || v > INT_MAX) {
        fprintf(stderr, "monolith: %s: %s is out of range\n", flag, s);
        return false;
    }
    *out = v;
    return true;
}

// #endregion

int main(int argc, char **argv)
{
    int status = Run(argc, argv);
    FreeMainHeap();
    LauncherShutdown();
    ExportShutdown();
    MS_ClearError();        // (the runtime's last error message, if any)
    return status;
}

// ======================================================================================
// #region Main
// ======================================================================================

static int Run(int argc, char **argv)
{
    InitDataPaths();

    // The player starts at the map's spawn (set after loading it) unless --pos/--eye/--look
    Player player = { 0 };
    bool posSet = false;

    const char *shotPath = NULL;
    int shotW = 1600, shotH = 900;  // --shot-size
    const char *offKeys = "";
    int startView = 0;
    int shadowSize = 0;         // --shadow; 0 = not given (the map's env shadow.size / sun.range)
    float shadowRange = 0.0f;
    bool bench = false;
    bool headless = false;      // --bench without a visible window
    int benchWindowSamples = -1;    // --bench-window <samples>; -1 = off
    // Graphics flags: each one, when given, overrides that graphics setting for this run (the
    // settings file, if read, or the defaults; -1 / NULL = not given)
    int windowMsaa = -1;        // --window-msaa
    bool noPrepass = false;     // --no-prepass
    float renderScale = -1.0f;  // --render-scale
    float aoVolumeVoxel = -1.0f, aoVolumeIntensity = 1.0f;  // --ao-volume
    int upscale = -1;           // --upscale
    int shadowMode = -1;        // --shadow-camera / --shadow-always
    const char *qualityName = NULL;     // --quality
    const char *settingsPath = NULL;    // --settings <file>
    bool noSettings = false;            // --no-settings
    bool settingsPanelFlag = false;     // --settings-panel
    bool idPanelFlag = false;           // --id-panel [filter]
    bool keysPanelFlag = false;         // --keys-panel [section]
    const char *keysPanelSection = NULL;
    const char *idPanelFilter = NULL;
    static Inspector inspector;         // render inspector (F3 / SHIFT+F3 / F4; --inspect, --output-mode, ...)
    const char *newMapName = NULL;
    const char *exportDir = NULL;               // --export
    const char *compilePath = NULL;             // --compile
    float fogCacheVoxel = -1.0f;    // --fog-cache
    int fogCacheSub = 2;
    const char *benchNote = NULL;
    const char *mapPath = NULL;
    const char *dumpPath = NULL;
    const char *dumpScenePath = NULL;   // --dump-scene, --dump-scene-diff
    const char *triReportPath = NULL;   // --tri-report
    const char *dumpEntitiesPath = NULL;    // --dump-entities
    const char *applyEditsPath = NULL;      // --apply-edits
    const char *editScriptPath = NULL;      // --edit-script
    const char *paintScriptPath = NULL;     // --paint-script (docs/painting.md 14)
    bool drawModeFlag = false;              // --draw-mode
    Vector2 cursorArg = { 0 };              // --cursor x y
    bool cursorSet = false;
    bool drawPanelFlag = false;             // --draw-panel [section]
    const char *drawPanelSection = NULL;
    const char *drawStateArg = NULL;        // --draw-state <file> (docs/painting.md 18.6)
    int editSaveArg = -1;                   // --edit-save: 0 manual, 1 auto, -1 the settings'
    bool dumpSceneDiff = false;         // --dump-scene-diff: no indices or lines
    const char *reloadTestPath = NULL;
    bool eyeSet = false, lookSet = false;   // --eye / --look given (they override --view)
    Vector3 eye = { 0 };
    bool pickFlag = false, shotHud = false;
    bool showBarriersFlag = false;          // --show-barriers: the barrier overlay in a --shot
    bool editModeFlag = false;          // --edit-mode
    const char *editIdText = NULL;      // --edit-id
    const char *editModalSpec = NULL;   // --edit-modal
    const char *inputScriptPath = NULL; // --input-script
    bool modelPickerFlag = false;       // --model-picker [filter]
    const char *modelPickerFilter = NULL;
    int modelPickerKey = 0;             // --model-picker-key: 1 enter, 2 close
    int frameReuseFlag = -1;    // --no-frame-reuse 0, --frame-reuse 1, default -1 (see below)
    const char *submitText = NULL, *typeText = NULL;
    float submitWait = -1.0f;       // --submit-wait: keep polling the request this long
    bool closeSubmitted = false;    // --close-submitted
    const char *classifyText = NULL;
    const char *aoMode = NULL, *bloomMode = NULL;
    // Repeatable arguments: at most argc of each
    mainHeap.fx = malloc(sizeof(const char *) * argc);
    mainHeap.selectLines = malloc(sizeof(int) * argc);
    mainHeap.selectFiles = malloc(sizeof(const char *) * argc);
    mainHeap.regions = malloc(sizeof(Rectangle) * argc);
    mainHeap.volumes = malloc(sizeof(BoundingBox) * argc);
    mainHeap.pickAt = malloc(sizeof(Vector2) * argc);
    mainHeap.settingsSet = malloc(sizeof(const char *) * argc);
    mainHeap.settingsChange = malloc(sizeof(const char *) * argc);
    if (!mainHeap.fx || !mainHeap.selectLines || !mainHeap.selectFiles || !mainHeap.regions || !mainHeap.volumes || !mainHeap.pickAt || !mainHeap.settingsSet ||
        !mainHeap.settingsChange) {
        fprintf(stderr, "monolith: out of memory\n");
        return 1;
    }
    const char **fxSettings = mainHeap.fx;
    int fxCount = 0;
    int *selectLines = mainHeap.selectLines, selectLineCount = 0;
    const char **selectFiles = mainHeap.selectFiles;
    Rectangle *regionArgs = mainHeap.regions;
    BoundingBox *volumeArgs = mainHeap.volumes;
    Vector2 *pickAtArgs = mainHeap.pickAt;
    int pickAtCount = 0;
    int regionArgCount = 0, volumeArgCount = 0;
    const char **settingsSets = mainHeap.settingsSet, **settingsChanges = mainHeap.settingsChange;
    int settingsSetCount = 0, settingsChangeCount = 0;
    // Every flag is checked here, before anything else happens: an unknown flag, a missing
    // value, a value that is another flag (`--pos 1 --shot x.png` must not eat --shot) or a
    // number that doesn't parse stops the program with a message and status 1. Safety rule:
    // a mistyped automated run must never fall through to the interactive (visible) editor.
#define NEED(n) do { if (!ArgValues(argc, argv, i, (n))) return 1; } while (0)
#define NUM(k, lo, hi, dst) do { double v_; if (!ArgNumber(argv[i], argv[i + (k)], (lo), (hi), &v_)) return 1; (dst) = v_; } while (0)
#define INT(k, dst) do { long v_; if (!ArgInt(argv[i], argv[i + (k)], &v_)) return 1; (dst) = (int)v_; } while (0)
    const char *inspectPath = NULL;     // --inspect-model (runs after every flag is checked)
    BakeArgs bake = { .size = 2048, .texels = 24.0f, .face = BAKE_FACE_TOP, .levels = 1 };   // --bake-surface (bake.c)
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--shot") == 0) { NEED(1); shotPath = argv[++i]; }
        else if (strcmp(a, "--shot-size") == 0) {
            NEED(2);
            INT(1, shotW);
            INT(2, shotH);
            shotW = shotW < 64 ? 64 : shotW > 8192 ? 8192 : shotW;
            shotH = shotH < 64 ? 64 : shotH > 8192 ? 8192 : shotH;
            i += 2;
        }
        else if (strcmp(a, "--off") == 0) { NEED(1); offKeys = argv[++i]; }
        else if (strcmp(a, "--view") == 0) { NEED(1); INT(1, startView); i++; }
        else if (strcmp(a, "--ao") == 0) { NEED(1); aoMode = argv[++i]; }
        else if (strcmp(a, "--bloom") == 0) { NEED(1); bloomMode = argv[++i]; }
        else if (strcmp(a, "--fx") == 0) { NEED(1); fxSettings[fxCount++] = argv[++i]; }
        else if (strcmp(a, "--bench") == 0) bench = true;
        else if (strcmp(a, "--bench-still") == 0) bench = benchStill = true;
        else if (strcmp(a, "--no-profile") == 0) benchProfile = false;
        else if (strcmp(a, "--headless") == 0) headless = true;
        else if (strcmp(a, "--window-msaa") == 0) windowMsaa = 1;
        else if (strcmp(a, "--quality") == 0) { NEED(1); qualityName = argv[++i]; }
        else if (strcmp(a, "--settings") == 0) { NEED(1); settingsPath = argv[++i]; }
        else if (strcmp(a, "--no-settings") == 0) noSettings = true;
        else if (strcmp(a, "--settings-set") == 0) { NEED(1); settingsSets[settingsSetCount++] = argv[++i]; }
        else if (strcmp(a, "--settings-change") == 0) { NEED(1); settingsChanges[settingsChangeCount++] = argv[++i]; }
        else if (strcmp(a, "--settings-panel") == 0) settingsPanelFlag = true;
        else if (strcmp(a, "--inspect") == 0) {
            NEED(1);
            if (!InspectorSetMode(&inspector, argv[++i]) || inspector.mode == INSPECT_OFF) {
                fprintf(stderr, "monolith: --inspect takes material, shader or draw, not '%s'\n", argv[i]);
                return 1;
            }
        }
        else if (strcmp(a, "--output-mode") == 0) {
            NEED(1);
            if (!InspectorSetOutput(&inspector, argv[++i])) {
                fprintf(stderr, "monolith: unknown --output-mode '%s' (scene, albedo, normal, orm, radiance, specular, ssao, ssil, "
                                "ssgi, ssr, bloom, dof)\n", argv[i]);
                return 1;
            }
        }
        else if (strcmp(a, "--inspector-panel") == 0) InspectorOpenPanel(&inspector);
        else if (strcmp(a, "--keys-panel") == 0) {
            keysPanelFlag = true;
            if (i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0) keysPanelSection = argv[++i];
        }
        else if (strcmp(a, "--id-panel") == 0) {
            idPanelFlag = true;
            if (i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0) idPanelFilter = argv[++i];
        }
        else if (strcmp(a, "--inspect-hide") == 0 || strcmp(a, "--inspect-isolate") == 0) {
            NEED(1);
            bool hide = a[10] == 'h';
            if (!argv[i + 1][0] || !(hide ? InspectorHide(&inspector, argv[i + 1]) : InspectorIsolate(&inspector, argv[i + 1]))) {
                fprintf(stderr, "monolith: %s needs a material name or a shader path\n", a);
                return 1;
            }
            i++;
        }
        else if (strcmp(a, "--bench-window") == 0) {
            NEED(1);
            INT(1, benchWindowSamples);
            benchWindowSamples = benchWindowSamples < 0 ? 0 : benchWindowSamples > 16 ? 16 : benchWindowSamples;
            i++;
        }
        else if (strcmp(a, "--shadow-always") == 0) shadowMode = MS_SHADOW_ALWAYS;
        else if (strcmp(a, "--shadow-camera") == 0) { if (shadowMode != (int)MS_SHADOW_ALWAYS) shadowMode = MS_SHADOW_CAMERA; }
        else if (strcmp(a, "--new-map") == 0) { NEED(1); newMapName = argv[++i]; }
        else if (strcmp(a, "--export") == 0) { NEED(1); exportDir = argv[++i]; }
        else if (strcmp(a, "--compile") == 0) { NEED(1); compilePath = argv[++i]; }
        else if (strcmp(a, "--fog-cache") == 0) {
            // Clamped: 0.25 m is already 228 MB over the default region; 0 or less turns it off
            NEED(2);
            NUM(1, -1e9, 1e9, fogCacheVoxel);
            INT(2, fogCacheSub);
            fogCacheVoxel = fogCacheVoxel > 0.0f ? Clamp(fogCacheVoxel, 0.25f, 8.0f) : 0.0f;
            fogCacheSub = fogCacheSub < 1 ? 1 : fogCacheSub > 4 ? 4 : fogCacheSub;
            i += 2;
        }
        else if (strcmp(a, "--no-prepass") == 0) noPrepass = true;
        else if (strcmp(a, "--ao-volume") == 0) {
            NEED(2);
            NUM(1, -1e9, 1e9, aoVolumeVoxel);
            NUM(2, -1e9, 1e9, aoVolumeIntensity);
            aoVolumeVoxel = aoVolumeVoxel > 0.0f ? Clamp(aoVolumeVoxel, 0.25f, 4.0f) : 0.0f;
            aoVolumeIntensity = Clamp(aoVolumeIntensity, 0.01f, 4.0f);
            i += 2;
        }
        else if (strcmp(a, "--render-scale") == 0) {
            NEED(1);
            NUM(1, -1e9, 1e9, renderScale);
            renderScale = Clamp(renderScale, 0.25f, 1.0f);
            i++;
        }
        else if (strcmp(a, "--upscale") == 0) {
            NEED(1);
            const char *m = argv[++i];
            if (strcmp(m, "nearest") == 0) upscale = R3D_UPSCALE_NEAREST;
            else if (strcmp(m, "linear") == 0) upscale = R3D_UPSCALE_LINEAR;
            else if (strcmp(m, "bicubic") == 0) upscale = R3D_UPSCALE_BICUBIC;
            else if (strcmp(m, "lanczos") == 0) upscale = R3D_UPSCALE_LANCZOS;
            else {
                fprintf(stderr, "monolith: unknown --upscale filter '%s' (nearest, linear, bicubic, lanczos)\n", m);
                return 1;
            }
        }
        else if (strcmp(a, "--bench-frames") == 0) {
            NEED(1);
            INT(1, benchFrames);
            benchFrames = benchFrames < 10 ? 10 : benchFrames > BENCH_FRAMES_LIMIT ? BENCH_FRAMES_LIMIT : benchFrames;
            i++;
        }
        else if (strcmp(a, "--note") == 0) { NEED(1); benchNote = argv[++i]; }
        else if (strcmp(a, "--bench-views") == 0) { NEED(1); benchViewsArg = argv[++i]; }
        else if (strcmp(a, "--map") == 0) { NEED(1); mapPath = argv[++i]; }
        else if (strcmp(a, "--dump-boxes") == 0) { NEED(1); dumpPath = argv[++i]; }
        else if (strcmp(a, "--dump-scene") == 0) { NEED(1); dumpScenePath = argv[++i]; dumpSceneDiff = false; }
        else if (strcmp(a, "--dump-scene-diff") == 0) { NEED(1); dumpScenePath = argv[++i]; dumpSceneDiff = true; }
        else if (strcmp(a, "--tri-report") == 0) { NEED(1); triReportPath = argv[++i]; }
        else if (strcmp(a, "--dump-entities") == 0) { NEED(1); dumpEntitiesPath = argv[++i]; }
        else if (strcmp(a, "--apply-edits") == 0) { NEED(1); applyEditsPath = argv[++i]; }
        else if (strcmp(a, "--inspect-model") == 0) { NEED(1); inspectPath = argv[++i]; }
        else if (strcmp(a, "--bake-surface") == 0) { NEED(2); bake.material = argv[i + 1]; bake.outPrefix = argv[i + 2]; i += 2; }
        else if (strcmp(a, "--bake-size") == 0 || strcmp(a, "--bake-levels") == 0) {
            NEED(1);
            bool size = a[7] == 's';
            int *dst = size ? &bake.size : &bake.levels, lo = size ? 16 : 1, hi = size ? 8192 : 6;
            INT(1, *dst);
            if (*dst < lo || *dst > hi) {
                fprintf(stderr, "monolith: %s %d is out of range (%d..%d)\n", a, *dst, lo, hi);
                return 1;
            }
            i++;
        }
        else if (strcmp(a, "--bake-texels") == 0) { NEED(1); NUM(1, 0.01, 10000, bake.texels); i++; }
        else if (strcmp(a, "--bake-face") == 0) {
            NEED(1);
            const char *f = argv[++i];
            if (strcmp(f, "top") != 0 && strcmp(f, "side") != 0) {
                fprintf(stderr, "monolith: unknown --bake-face '%s' (top, side)\n", f);
                return 1;
            }
            bake.face = f[0] == 't' ? BAKE_FACE_TOP : BAKE_FACE_SIDE;
        }
        else if (strcmp(a, "--reload-test") == 0) { NEED(1); reloadTestPath = argv[++i]; }
        else if (strcmp(a, "--edit-script") == 0) { NEED(1); editScriptPath = argv[++i]; }
        else if (strcmp(a, "--paint-script") == 0) { NEED(1); paintScriptPath = argv[++i]; }
        else if (strcmp(a, "--draw-mode") == 0) drawModeFlag = true;
        else if (strcmp(a, "--cursor") == 0) {
            NEED(2);
            NUM(1, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, cursorArg.x);
            NUM(2, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, cursorArg.y);
            cursorSet = true;
            i += 2;
        }
        else if (strcmp(a, "--draw-panel") == 0) {
            drawPanelFlag = true;       // (the section is optional)
            if (i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0) drawPanelSection = argv[++i];
        }
        else if (strcmp(a, "--draw-state") == 0) { NEED(1); drawStateArg = argv[++i]; }
        else if (strcmp(a, "--edit-save") == 0) {
            NEED(1);
            const char *v = argv[++i];
            if (strcmp(v, "manual") == 0) editSaveArg = 0;
            else if (strcmp(v, "auto") == 0) editSaveArg = 1;
            else { fprintf(stderr, "monolith: --edit-save takes manual or auto, not '%s'\n", v); return 1; }
        }
        else if (strcmp(a, "--pick") == 0) pickFlag = true;
        else if (strcmp(a, "--pick-at") == 0) {
            NEED(2);
            Vector2 v;
            NUM(1, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, v.x);
            NUM(2, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, v.y);
            pickAtArgs[pickAtCount++] = v;
            i += 2;
        }
        else if (strcmp(a, "--shot-hud") == 0) shotHud = true;
        else if (strcmp(a, "--show-barriers") == 0) showBarriersFlag = true;
        else if (strcmp(a, "--edit-mode") == 0) editModeFlag = true;
        else if (strcmp(a, "--edit-id") == 0) { NEED(1); editIdText = argv[++i]; }
        else if (strcmp(a, "--edit-modal") == 0) { NEED(1); editModalSpec = argv[++i]; }
        else if (strcmp(a, "--input-script") == 0) { NEED(1); inputScriptPath = argv[++i]; }
        else if (strcmp(a, "--model-picker") == 0) {
            modelPickerFlag = true;     // (the filter is optional)
            if (i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0) modelPickerFilter = argv[++i];
        }
        else if (strcmp(a, "--model-picker-key") == 0) {
            NEED(1);
            i++;
            modelPickerKey = strcmp(argv[i], "enter") == 0 ? 1 : strcmp(argv[i], "close") == 0 ? 2 : 0;
            if (!modelPickerKey) { fprintf(stderr, "monolith: --model-picker-key: enter or close, not '%s'\n", argv[i]); return 1; }
        }
        else if (strcmp(a, "--no-frame-reuse") == 0) frameReuseFlag = 0;
        else if (strcmp(a, "--frame-reuse") == 0) frameReuseFlag = 1;
        else if (strcmp(a, "--submit") == 0) { NEED(1); submitText = argv[++i]; }
        else if (strcmp(a, "--submit-wait") == 0) { NEED(1); NUM(1, 0.0, 86400.0, submitWait); i++; }
        else if (strcmp(a, "--close-submitted") == 0) closeSubmitted = true;
        else if (strcmp(a, "--type") == 0) { NEED(1); typeText = argv[++i]; }
        else if (strcmp(a, "--classify") == 0) { NEED(1); classifyText = argv[++i]; }
        else if (strcmp(a, "--select") == 0) {
            // [<file>:]<line>: a line of a part ("parts/lake.map:41"), else of the main file
            NEED(1);
            const char *colon = strrchr(argv[i + 1], ':');
            selectFiles[selectLineCount] = colon ? argv[i + 1] : NULL;
            long v;
            if (!ArgInt(a, colon ? colon + 1 : argv[i + 1], &v)) return 1;
            selectLines[selectLineCount++] = (int)v;
            i++;
        }
        else if (strcmp(a, "--region") == 0) {
            // (finite and bounded: the region frame is drawn with integer pixel coordinates)
            NEED(4);
            Rectangle r;
            NUM(1, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, r.x);
            NUM(2, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, r.y);
            NUM(3, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, r.width);
            NUM(4, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, r.height);
            regionArgs[regionArgCount++] = r;
            i += 4;
        }
        else if (strcmp(a, "--volume") == 0) {
            // Two opposite corners in map coordinates (any order)
            NEED(6);
            BoundingBox v;
            NUM(1, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, v.min.x);
            NUM(2, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, v.min.y);
            NUM(3, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, v.min.z);
            NUM(4, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, v.max.x);
            NUM(5, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, v.max.y);
            NUM(6, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, v.max.z);
            volumeArgs[volumeArgCount++] = v;
            i += 6;
        }
        else if (strcmp(a, "--shadow") == 0) {
            // Clamped: a zero range or size would divide by zero in the shadow tile layout
            NEED(2);
            INT(1, shadowSize);
            NUM(2, -1e9, 1e9, shadowRange);
            shadowSize = shadowSize < 256 ? 256 : shadowSize > 16384 ? 16384 : shadowSize;
            shadowRange = Clamp(shadowRange, 10.0f, 2000.0f);
            i += 2;
        }
        else if (strcmp(a, "--pos") == 0) {
            // (finite and bounded: the sun shadow tiles key on the camera position as integers)
            NEED(3);
            NUM(1, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, player.feet.x);
            NUM(2, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, player.feet.y);
            NUM(3, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, player.feet.z);
            posSet = true;
            i += 3;
        } else if (strcmp(a, "--eye") == 0) {
            NEED(3);
            double ey;
            NUM(1, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, eye.x);
            NUM(2, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, ey);
            NUM(3, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, eye.z);
            eye.y = (float)ey;
            // in double, so --eye x y z lands on the same float feet as --pos x y-1.62 z
            player.feet = (Vector3){ eye.x, (float)(ey - EYE_HEIGHT), eye.z };
            eyeSet = true;
            i += 3;
        } else if (strcmp(a, "--look") == 0) {
            NEED(2);
            double yaw, pitch;
            NUM(1, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, yaw);
            NUM(2, -ARG_COORD_LIMIT, ARG_COORD_LIMIT, pitch);
            player.yaw = yaw * DEG2RAD;
            player.pitch = pitch * DEG2RAD;
            lookSet = true;
            i += 2;
        }
        else {
            if (a[0] == '-') fprintf(stderr, "monolith: unknown flag '%s' (the flags are listed in docs/code/main.md)\n", a);
            else fprintf(stderr, "monolith: unexpected argument '%s' (no flag takes it as a value)\n", a);
            return 1;
        }
    }
#undef NEED
#undef NUM
#undef INT
    // The view-only build (common.h): the flags that edit, produce deliverables or reach the AI exit
    // here, before any file is written (no value starts with "--": NEED refuses that)
    if (!MONOLITH_EDITING) {
        static const char *const REFUSED[] = {
            "--apply-edits", "--edit-script", "--paint-script", "--new-map", "--submit", "--submit-wait",
            "--close-submitted", "--type", "--classify", "--edit-mode", "--edit-id", "--edit-modal",
            "--edit-save", "--model-picker", "--model-picker-key", "--draw-mode", "--draw-panel", "--draw-state", "--cursor", "--region",
            "--volume", "--bake-surface", "--bake-face", "--bake-size", "--bake-texels", "--bake-levels", "--export",
            "--compile",
        };
        for (int i = 1; i < argc; i++)
            for (size_t k = 0; k < sizeof(REFUSED) / sizeof(REFUSED[0]); k++)
                if (strcmp(argv[i], REFUSED[k]) == 0) {
                    fprintf(stderr, "monolith: %s: %s\n", argv[i], MSG_VIEW_ONLY);
                    return 1;
                }
    }
    if (inspectPath) return InspectModel(inspectPath);

    // --ao / --bloom / --fx / --quality / --settings-set: reject unknown names before a window opens
    MS_AoMode aoArg = MS_AO_FULL;
    MS_QualityLevel qualityArg = MS_QUALITY_ULTRA;
    {
        R3D_Environment scratch = { 0 };
        if (aoMode && !AoModeFromName(aoMode, &aoArg)) {
            fprintf(stderr, "monolith: unknown --ao mode '%s'. Modes:\n%s", aoMode, AoModeHelp());
            return 1;
        }
        if (bloomMode && strcmp(bloomMode, "on") != 0 && strcmp(bloomMode, "off") != 0) {
            fprintf(stderr, "monolith: unknown --bloom mode '%s' (on, off)\n", bloomMode);
            return 1;
        }
        if (qualityName && (!MS_QualityFromName(qualityName, &qualityArg) || qualityArg == MS_QUALITY_CUSTOM)) {
            fprintf(stderr, "monolith: unknown --quality '%s' (low, medium, high, ultra)\n", qualityName);
            return 1;
        }
        GfxSettings scratchGfx = GfxDefaults();
        for (int k = 0; k < settingsSetCount + settingsChangeCount; k++) {
            const char *kv = k < settingsSetCount ? settingsSets[k] : settingsChanges[k - settingsSetCount];
            if (!GfxSetKey(&scratchGfx, kv)) {
                fprintf(stderr, "monolith: bad setting '%s' (key=value). Keys:\n%s", kv, GfxKeyHelp());
                return 1;
            }
        }
        for (int k = 0; k < fxCount; k++) {
            if (!ApplyFxSetting(&scratch, fxSettings[k])) {
                fprintf(stderr, "monolith: bad --fx '%s' (key=number). Keys:\n%s", fxSettings[k], FxSettingHelp());
                return 1;
            }
        }
    }

    if (newMapName) return NewMap(mapsDir, newMapName) ? 0 : 1;

    // Every mode below that isn't one of these opens the visible editor window. An agent's shell
    // may not (a mistyped or forgotten flag would put the editor on the developer's screen).
    // (--edit-script is no mode of its own: it runs before --shot or a --dump-* flag, see below)
    bool windowless = shotPath || (bench && headless) || dumpPath || dumpScenePath || dumpEntitiesPath || applyEditsPath ||
                      exportDir || compilePath || classifyText || bake.material || triReportPath;
    bool editDump = editScriptPath && (dumpPath || dumpScenePath || dumpEntitiesPath);
    if (editScriptPath && !shotPath && !editDump) {
        fprintf(stderr, "monolith: --edit-script runs before --shot or --dump-boxes / --dump-scene(-diff) / --dump-entities: give one of them\n");
        return 1;
    }
    // --paint-script runs inside --shot (live, after --edit-script) or --apply-edits; --draw-mode and
    // --draw-panel are for --shot (--shot-hud) images (docs/painting.md 14)
    if (paintScriptPath && !shotPath && !applyEditsPath) {
        fprintf(stderr, "monolith: --paint-script runs inside --shot or --apply-edits: give one of them\n");
        return 1;
    }
    if ((drawModeFlag || drawPanelFlag) && !shotPath) {
        fprintf(stderr, "monolith: --draw-mode / --draw-panel need --shot\n");
        return 1;
    }
    // --draw-state: the editor (instead of <package>/draw.ini) or a --shot run (read before the first
    // frame, written as the editor does and at exit)
    if (drawStateArg && windowless && !shotPath) {
        fprintf(stderr, "monolith: --draw-state goes with --shot (or the editor)\n");
        return 1;
    }
    if (cursorSet && !drawModeFlag && !drawPanelFlag) {
        fprintf(stderr, "monolith: --cursor goes with --draw-mode or --draw-panel\n");
        return 1;
    }
    // --apply-edits with --paint-script: the edits on the text, then the strokes in a hidden window
    bool paintApply = applyEditsPath && paintScriptPath;
    // --draw-panel [section]: the draw panel over the shot (phase 3a: on DrawModeInit's state)
    static DrawPanel drawPanel;
    DrawPanelInit(&drawPanel);
    if (drawPanelFlag && !DrawPanelSetOnly(&drawPanel, drawPanelSection)) {
        fprintf(stderr, "monolith: --draw-panel %s: no such section (all, target, tools (or pen), color, density, faces)\n", drawPanelSection);
        return 1;
    }
    // --input-script: the input of a --shot run (input.c), from frame 10 on (every cache drawn)
    if (inputScriptPath && !shotPath) {
        fprintf(stderr, "monolith: --input-script needs --shot\n");
        return 1;
    }
    if (inputScriptPath && !InputScriptLoad(inputScriptPath, 10, shotW, shotH)) return 1;
    if (!windowless && AgentShell()) {
        fprintf(stderr, "monolith: refusing to open the editor window from an AI agent's shell (CLAUDECODE / AI_AGENT is set).\n"
                        "  Use --shot, --dump-boxes, --dump-scene(-diff), --dump-entities (each also with --edit-script), --apply-edits, --inspect-model, --new-map,\n"
                        "  --export, --compile, --classify, --bake-surface or --bench --headless; a person who wants the window from this shell sets\n"
                        "  MONOLITH_ALLOW_WINDOW=1.\n");
        return 1;
    }
    LauncherScrubEnv();     // the sessions this editor starts must not inherit the override

    // --- Graphics settings (settings.c). The interactive editor reads the user's settings file
    // (and the panel saves it); automated runs (--shot, --bench, --dump-*, --export,
    // --classify) never read it unless --settings names a file, so their images and numbers
    // don't depend on what the developer last chose. --no-settings: no file at all. Then
    // --quality, then the single flags: they apply to this run and are never saved.
    // Last --settings-set: a change "as if made in the panel" (applies, and is saved).
    bool automated = shotPath || bench || dumpPath || dumpScenePath || dumpEntitiesPath || applyEditsPath || exportDir ||
                     compilePath || classifyText || bake.material || triReportPath;
    const char *settingsFile = noSettings ? NULL : settingsPath ? settingsPath : automated ? NULL : GfxDefaultPath();
    GfxSettings gfx = GfxDefaults();
    if (settingsFile) {
        int ignored;
        if (GfxLoad(&gfx, settingsFile, &ignored)) {
            if (ignored)
                TraceLog(LOG_INFO, "monolith: settings %s: quality %s; %s: %d line%s ignored", settingsFile,
                         MS_QualityName(GfxPreset(&gfx)), settingsFile, ignored, ignored == 1 ? "" : "s");
            else TraceLog(LOG_INFO, "monolith: settings %s: quality %s", settingsFile, MS_QualityName(GfxPreset(&gfx)));
        }
        else TraceLog(LOG_INFO, "monolith: settings %s: no file yet (defaults; F2 changes them)", settingsFile);
    }
    GfxSettings gfxSaved = gfx;     // what the file holds: the panel's changes go here too, the flags don't
    bool gfxDirty = false;
    if (qualityName) GfxSetPreset(&gfx, qualityArg);
    if (aoMode) gfx.q.ao = aoArg;
    if (aoVolumeVoxel >= 0.0f) { gfx.q.aoVolumeVoxel = aoVolumeVoxel; gfx.q.aoVolumeIntensity = aoVolumeVoxel > 0.0f ? aoVolumeIntensity : 0.0f; }
    if (bloomMode) gfx.q.bloom = strcmp(bloomMode, "on") == 0;
    if (shadowSize > 0) { gfx.q.shadowSize = shadowSize; gfx.q.shadowRange = shadowRange; }
    if (shadowMode >= 0) gfx.q.shadowMode = (MS_ShadowMode)shadowMode;
    if (fogCacheVoxel >= 0.0f) { gfx.q.fogCacheVoxel = fogCacheVoxel; gfx.q.fogCacheSub = fogCacheSub; }
    if (noPrepass) gfx.q.depthPrepass = false;
    if (renderScale > 0.0f) gfx.q.renderScale = renderScale;
    if (upscale >= 0) gfx.q.upscale = (R3D_UpscaleMode)upscale;
    if (windowMsaa >= 0) gfx.windowMsaa = windowMsaa == 1;
    if (editSaveArg >= 0) gfx.editSave = editSaveArg;
    if (frameReuseFlag >= 0) gfx.frameReuse = frameReuseFlag == 1;
    for (int k = 0; k < settingsSetCount; k++) {
        GfxSetKey(&gfx, settingsSets[k]);
        GfxSetKey(&gfxSaved, settingsSets[k]);
        gfxDirty = true;
    }
    if (gfxDirty && settingsFile) {
        bool ok = GfxSave(&gfxSaved, settingsFile);
        TraceLog(ok ? LOG_INFO : LOG_WARNING, "monolith: settings %s %s", ok ? "saved to" : "could not be saved to", settingsFile);
        gfxDirty = false;
    }
    benchQuality = MS_QualityName(GfxPreset(&gfx));

    // --- Map: parse it before opening a window, so a broken map just prints the error, and
    // the sun's shadow map size (an R3D init hint) is known before R3D_Init
    char defaultMap[1100];
    snprintf(defaultMap, sizeof(defaultMap), "%smonolith", mapsDir);
    // (an edit script writes the map file: never the default one by accident)
    if (applyEditsPath && !mapPath) {
        fprintf(stderr, "monolith: --apply-edits needs --map <package or .map file> (it rewrites that map file)\n");
        return 1;
    }
    if (editScriptPath && !mapPath) {
        fprintf(stderr, "monolith: --edit-script needs --map <package or .map file> (its save and disk commands write that map file)\n");
        return 1;
    }
    if (!mapPath) mapPath = defaultMap;
    if (bake.material) {
        bake.mapPath = mapPath;
        return BakeSurface(&bake);
    }
    if (compilePath) {
        bool ok = MS_Compile(mapPath, compilePath);
        if (ok) printf("monolith: compiled %s into %s\n", mapPath, compilePath);
        else fprintf(stderr, "monolith: cannot compile the map\n%s\n", MS_LastError());
        return ok ? 0 : 1;
    }
    // (shadow size: an init hint, so a load option; the range is applied by MS_ApplyQuality)
    MS_LoadOptions loadOpt = { .noGpu = true, .shadowSizeOverride = gfx.q.shadowSize };
    MS_Scene *peek;     // parsed only (no GPU); gives the R3D init hints once a GL context exists
    {
        peek = MS_Load(mapPath, &loadOpt);
        if (!peek) {
            fprintf(stderr, "monolith: cannot load the map\n%s\n", MS_LastError());
            return 1;
        }
        TraceLog(LOG_INFO, "monolith: %s: %d boxes, %d groups (format %d)", peek->path, peek->boxCount,
                 peek->groupCount, peek->formatVersion);
        if (dumpEntitiesPath && !editScriptPath) {
            bool ok = DumpEntities(peek, dumpEntitiesPath);
            fprintf(ok ? stdout : stderr, ok ? "monolith: wrote %d entities to %s\n" : "monolith: cannot write %d entities to %s\n",
                    peek->entityCount, dumpEntitiesPath);
            MS_Unload(peek);
            return ok ? 0 : 1;
        }
        if (applyEditsPath && MS_CompiledSource(peek, NULL) != MS_SOURCE_TEXT) {
            fprintf(stderr, "monolith: %s is a compiled map: --apply-edits edits the map's text (its .map file)\n", peek->path);
            MS_Unload(peek);
            return 1;
        }
        if (applyEditsPath) {
            // The edits work on the text and the parsed lines only: a noGpu scene, reloaded
            // after every edit like the editor's
            scene = peek;
            signed char status = (signed char)MapEditRunScript(applyEditsPath);
            scene = NULL;
            if (status != 0 || !paintScriptPath) {
                MS_Unload(peek);
                return status;
            }
        }
        if (dumpPath && !editScriptPath) {
            bool ok = DumpBoxes(peek, dumpPath);
            fprintf(ok ? stdout : stderr, ok ? "monolith: wrote %d boxes to %s\n" : "monolith: cannot write %d boxes to %s\n",
                    peek->boxCount, dumpPath);
            MS_Unload(peek);
            return ok ? 0 : 1;
        }
        // --classify: the launcher's model choice for this prompt (no window). Context: the
        // --select lines and --region count against this map, plus a crosshair hit
        if (classifyText) {
            int selected = 0;
            for (int b = 0; b < peek->boxCount; b++)
                for (int k = 0; k < selectLineCount; k++)
                    if (peek->boxRefs[b].line == selectLines[k] && peek->boxRefs[b].file == SelectFile(peek, selectFiles[k])) {
                        selected++;
                        break;
                    }
            for (int m = 0; m < peek->modelCount; m++)
                for (int k = 0; k < selectLineCount; k++)
                    if (peek->models[m].ref.line == selectLines[k] && peek->models[m].ref.file == SelectFile(peek, selectFiles[k])) {
                        selected++;
                        break;
                    }
            char *text = Unescape(classifyText);
            if (!text) { MS_Unload(peek); return 1; }
            char *package = MonoStrf("%s", GetFileName(GetDirectoryPath(peek->path)));
            ModelQuery q = { .prompt = text, .package = package, .boxes = peek->boxCount, .models = peek->modelCount,
                             .brief = peek->brief[0] != 0, .selected = selected, .regions = regionArgCount, .volumes = volumeArgCount,
                             .hit = true };
            ModelChoice c;
            LauncherClassify(&q, &c);       // (the AI classifier runs to its end here: no window to keep alive)
            printf("tier: %s\nmodel: %s%s\nclassifier: %s\nclassifier model: %s\nreason: %s\n", ModelTierName(c.tier), c.name,
                   c.addFlag ? "" : " (from MONOLITH_CLAUDE_ARGS, no --model added)", c.classifier ? c.classifier : "?",
                   c.classifierModel[0] ? c.classifierModel : "-", MsgText(&c.reason));
            if (c.classifierAccount[0]) printf("classifier account: %s\n", c.classifierAccount);     // (only with MONOLITH_CLAUDE_FALLBACK)
            if (c.seconds >= 0.0) printf("seconds: %.2f\n", c.seconds);
            else printf("seconds: -\n");
            printf("source: %s\nprompt: %s\n", c.source, text + c.promptSkip);
            free(package);
            ModelChoiceFree(&c);
            free(text);
            MS_Unload(peek);
            return 0;
        }
        if (exportDir) {
            // (ExportScene prints its own result line, as this did)
            unsigned char r = ExportScene(peek, exportDir, true);
            MS_Unload(peek);
            return r == 0 ? 0 : 1;      // 0: exported
        }
        if (dumpScenePath && peek->modelCount == 0 && !editScriptPath) {
            // Without models nothing needs the GPU: no window at all
            bool ok = DumpScene(peek, dumpScenePath, dumpSceneDiff);
            fprintf(ok ? stdout : stderr, ok ? "monolith: wrote the scene to %s\n" : "monolith: cannot write %s\n", dumpScenePath);
            MS_Unload(peek);
            return ok ? 0 : 1;
        }
        // The spawn's angles in double, so 180 degrees lands exactly on the float PI
        if (!posSet && !eyeSet) player.feet = peek->spawnFeet;
        if (!lookSet) {
            player.yaw = (float)(peek->spawnYawDeg * (3.14159265358979323846 / 180.0));
            player.pitch = (float)(peek->spawnPitchDeg * (3.14159265358979323846 / 180.0));
        }
        shadowSize = peek->sun.shadowSize;          // (for the bench report)
        shadowRange = gfx.q.shadowRange > 0.0f ? gfx.q.shadowRange : peek->sun.light.range;
    }

    // No vsync, uncapped. The window is single-sampled unless --window-msaa: R3D anti-aliases
    // its own image (FXAA) and writes the same colour to every sample, so 4x MSAA on the
    // window only smoothed the designer's overlay lines, for ~0.1 ms GPU per frame at 1080p
    // (R3D's last pass writing 4 samples + the resolve; measured with --bench-window 4 vs 0).
    // Shots and request images were always single-sampled.
    unsigned int windowFlags = gfx.windowMsaa ? FLAG_MSAA_4X_HINT : 0;
    // --shot: hidden window, never mapped, so i3 & co. never show or focus it
    // --bench --headless too: then nothing ever appears on screen
    // --dump-scene of a map with models: model files load through R3D, which needs GL
    // --edit-script with a dump: the edits go live on the GPU scene first (a hidden window)
    // --tri-report: the hidden faces are found by the GPU upload
    bool hidden = shotPath || (bench && headless) || dumpScenePath || editDump || paintApply || triReportPath;
    windowFlags |= hidden ? FLAG_WINDOW_HIDDEN : FLAG_WINDOW_RESIZABLE;
    if (!hidden && gfx.vsync) windowFlags |= FLAG_VSYNC_HINT;     // (settings: Performance)
    SetConfigFlags(windowFlags);
    // Distinct titles let the window manager route automated runs elsewhere, e.g. i3:
    //   for_window [title="^monolith-(bench|shot)$"] move container to workspace number 10
    // Headless benches use the size the tiled bench window gets on a 1080p screen, so
    // their numbers line up with the visible runs.
    bool headlessBench = bench && headless;
    InitWindow(headlessBench ? 1920 : shotPath ? shotW : 1600, headlessBench ? 1006 : shotPath ? shotH : 900,
               bench ? "monolith-bench" : (shotPath || dumpScenePath || editDump || paintApply || triReportPath) ? "monolith-shot" : "monolith");
    SetTargetFPS(hidden || bench ? 0 : FpsTarget(gfx.fpsCap));
    // Esc closes the prompt box, snip mode, the panel and request-close prompts; it no longer
    // quits (the window's close button ends WindowShouldClose()). Raylib's default Esc exit
    // would quit even while those are open, so the exit key stays off.
    SetExitKey(KEY_NULL);

    // Sun shadow resolution (the map's shadow.size or --shadow; R3D_SetHint needs the GL
    // context). Static tiles and R3D's camera-fitted map both cover a sphere of radius
    // ~1.5x range (70 deg fov / 16:9; the fitted map grows with wider aspects), so texel
    // size = 2 * radius / shadow map size: range 100 m @ 8192 px -> ~3.7 cm texels. VRAM:
    // 16-bit depth, R3D allocates 2 layers -> 8192^2 * 2 B * 2 = 256 MB (the two tiles use
    // exactly these two layers), plus the min/max tiles (2 x 512^2 RG32F, 4 MB).
    MS_SetInitHints(peek);
    MS_Unload(peek);

    R3D_Init(GetScreenWidth(), GetScreenHeight());
    R3D_SetAntiAliasingMode(R3D_ANTI_ALIASING_MODE_FXAA);

    // Shadow "pancaking". R3D fits the sun's shadow map to the camera frustum, and
    // its near plane only reaches ~radius + 100 m toward the sun. Parts of tall
    // casters beyond that plane (e.g. the top of a 260 m tower ~330 m up the light
    // ray) get clipped, so their shadows pop in and out as the camera turns.
    // With depth clamp those parts are flattened onto the near plane instead of
    // being clipped, and still write depth. R3D never touches this state, so
    // enabling it once covers its shadow pass. Side effect on the main view:
    // geometry closer than the 1 cm near plane is clamped rather than cut away.
    // (glEnable(GL_DEPTH_CLAMP), in the runtime so every program that draws a scene gets it.)
    MS_InitRenderer();

    // --- The scene (runtime/monolith_scene.c): boxes as one instanced draw per material,
    // materials and their shaders, sky + ambient map, lights, sun shadow tiles
    {
        loadOpt.noGpu = false;
        scene = MS_Load(mapPath, &loadOpt);
        if (!scene) {
            fprintf(stderr, "monolith: cannot load the map\n%s\n", MS_LastError());
            R3D_Close();
            CloseWindow();
            return 1;
        }
        // (a mistyped name would hide nothing and the shot would prove nothing)
        const char *unknownName = InspectorUnknownName(&inspector, scene);
        if (unknownName) {
            fprintf(stderr, "monolith: --inspect-hide / --inspect-isolate: \"%s\" is no material name or shader path of %s\n",
                    unknownName, scene->path);
            InspectorFree(&inspector);
            MS_Unload(scene);
            R3D_Close();
            CloseWindow();
            return 1;
        }
        if (paintApply) {
            // --apply-edits ran; now --paint-script on the live scene (its `save` writes the package)
            static EditMode pe;
            static DrawMode pd;
            static Designer pdes;
            DrawModeInit(&pd, &pe);
            DesignerSync(&pdes);
            // (`pick screen`: the scene in the main loop's environment, which isn't applied yet)
            EffectState pfx = { .gfx = &gfx, .fx = fxSettings, .fxCount = fxCount };
            ScreenReadCtx psr = { .camera = PlayerCamera(&player), .fx = &pfx };
            pd.screenRead = ScreenRead;
            pd.screenUser = &psr;
            signed char status = (signed char)DrawModeRunScript(&pd, &pe, &pdes, psr.camera, GetScreenWidth(),
                                                                GetScreenHeight(), paintScriptPath);
            if (psr.own) UnloadRenderTexture(psr.target);
            int unsaved = EditModeUnsaved(&pe);
            if (unsaved > 0) TraceLog(LOG_INFO, "monolith: %d unsaved edit%s dropped (the paint script did not save)", unsaved, unsaved == 1 ? "" : "s");
            EditModeFree(&pe);
            DrawModeFree(&pd);
            DesignerFree(&pdes);
            MS_Unload(scene);
            R3D_Close();
            CloseWindow();
            return status;
        }
        if (editDump) {
            // --edit-script, then the dumps of the live scene
            MapEditor ed = { .autoSave = gfx.editSave == 1 };
            signed char status = (signed char)MapEditRunLiveScript(&ed, editScriptPath);
            MapEditFree(&ed);
            bool ok = true;
            if (dumpPath) {
                bool w = DumpBoxes(scene, dumpPath);
                fprintf(w ? stdout : stderr, w ? "monolith: wrote %d boxes to %s\n" : "monolith: cannot write %d boxes to %s\n",
                        scene->boxCount, dumpPath);
                ok = ok && w;
            }
            if (dumpEntitiesPath) {
                bool w = DumpEntities(scene, dumpEntitiesPath);
                fprintf(w ? stdout : stderr, w ? "monolith: wrote %d entities to %s\n" : "monolith: cannot write %d entities to %s\n",
                        scene->entityCount, dumpEntitiesPath);
                ok = ok && w;
            }
            if (dumpScenePath) {
                bool w = DumpScene(scene, dumpScenePath, dumpSceneDiff);
                fprintf(w ? stdout : stderr, w ? "monolith: wrote the scene to %s\n" : "monolith: cannot write %s\n", dumpScenePath);
                ok = ok && w;
            }
            MS_Unload(scene);
            R3D_Close();
            CloseWindow();
            return ok && status == 0 ? 0 : 1;
        }
        if (dumpScenePath) {
            bool ok = DumpScene(scene, dumpScenePath, dumpSceneDiff);
            fprintf(ok ? stdout : stderr, ok ? "monolith: wrote the scene to %s\n" : "monolith: cannot write %s\n", dumpScenePath);
            MS_Unload(scene);
            R3D_Close();
            CloseWindow();
            return ok ? 0 : 1;
        }
        if (triReportPath) {
            bool ok = WriteTriReport(scene, triReportPath, gfx.q.detailDistance);
            fprintf(ok ? stdout : stderr, ok ? "monolith: wrote the triangle report to %s\n" : "monolith: cannot write %s\n",
                    triReportPath);
            MS_Unload(scene);
            R3D_Close();
            CloseWindow();
            return ok ? 0 : 1;
        }
    }
    // --- Environment: the map's, then the graphics settings (render settings, AA, render
    // scale; --ao / --bloom / --render-scale / ... are part of them), --fx, the 1-6 toggles
    EffectState effects = { .gfx = &gfx, .fx = fxSettings, .fxCount = fxCount };
    ApplyEnvironment(&effects);
    int appliedShadowSize = gfx.q.shadowSize;   // the renderer restarts when the panel changes it
    bool appliedVsync = gfx.vsync;
    int appliedFps = gfx.fpsCap;

    bool hud = true;

    // Map hot reload message ("map reloaded (N boxes)" or the error), shown even with the
    // HUD off. Errors stay until the next successful load.
    MonoMsg mapMessage = { 0 };     // (shown while mapMessageTime runs; freed at the end)
    float mapMessageTime = 0.0f;
    bool mapMessageError = false;

    Spectator spec = { .speed = 12.0f, .view = -1 };
    if (startView >= 1 && startView <= scene->viewpointCount) {
        spec.active = true;
        SpectatorJumpTo(&spec, startView - 1);
        if (eyeSet) spec.pos = eye;
        if (lookSet) { spec.yaw = player.yaw; spec.pitch = player.pitch; }
    }

    // Designer: selection and prompt box (spectator mode)
    static Designer designer;
    // (a scripted run shows and hides the cursor as the window does: input.c emulates it)
    bool scripted = InputScripted();
    designer.interactive = !hidden || scripted;
    DesignerSync(&designer);
    for (int i = 0; i < selectLineCount; i++) {
        int file = SelectFile(scene, selectFiles[i]);
        if (file >= 0) DesignerSelectLine(&designer, file, selectLines[i]);
        else TraceLog(LOG_WARNING, "monolith: --select %s: the map has no such map-text file", selectFiles[i]);
    }
    // --region: as if dragged out in snip mode (pixels of the shot)
    if (regionArgCount > 0) DesignerSnipBegin(&designer);
    for (int i = 0; i < regionArgCount; i++)
        if (!DesignerAddRegion(&designer, regionArgs[i], GetScreenWidth(), GetScreenHeight()))
            TraceLog(LOG_WARNING, "monolith: --region %.0f %.0f %.0f %.0f is empty after clamping to %dx%d; ignored",
                     regionArgs[i].x, regionArgs[i].y, regionArgs[i].width, regionArgs[i].height, GetScreenWidth(), GetScreenHeight());
    // --volume: as if made in volume mode (world space)
    for (int i = 0; i < volumeArgCount; i++) DesignerAddVolume(&designer, volumeArgs[i]);
    // Edit mode (M): manual edits of the selection (editmode.c); --edit-mode opens it for --shot-hud
    static EditMode edit;
    edit.ed.autoSave = gfx.editSave == 1;
    if (editModeFlag) EditModeOpen(&edit, &designer);
    if (editModeFlag && editIdText) EditModeOpenIdPanel(&edit, &designer, editIdText);
    // The id panel (F5, idpanel.c); --id-panel opens it for a --shot-hud image
    static IdPanel idPanel;
    idPanel.interactive = !hidden || scripted;
    if (idPanelFlag && shotPath) IdPanelOpen(&idPanel, idPanelFilter);
    // The keybindings panel (ESC menu, keyspanel.c); --keys-panel opens it for a --shot-hud image
    static KeysPanel keys;
    keys.interactive = !hidden || scripted;
    if (keysPanelFlag && shotPath) KeysOpen(&keys, keysPanelSection);
    // The model picker (I in edit mode; modelpicker.c); --model-picker opens it (and the mode).
    // In a shot its thumbnails start late enough that the last ones render in the shot's own
    // frame (from the first frame with --reload-test or --settings-change, so the reload or the
    // restart at frame 10 frees them and they are rendered again; from frame 10 before an
    // ENTER); the --model-picker-key acts once they are all rendered
    static ModelPicker picker;
    int pickerThumbFrame = 0, pickerShotFrame = 30;
    bool pickerKeyDone = false;
    // The draw mode (B; drawmode.c, docs/painting.md) shares the edit mode's history. --draw-mode /
    // --draw-panel open it for --shot-hud images (the panel with a few recent colours; a scripted
    // shot frees the cursor and plays clicks on the panel)
    static DrawMode drawMode;
    DrawModeInit(&drawMode, &edit);
    drawPanel.interactive = !hidden || scripted;
    if (drawModeFlag || drawPanelFlag) DrawModeOpen(&drawMode, &edit, &designer);
    if (drawPanelFlag) {
        drawPanel.interactive = scripted;
        static const Color recent[] = { { 200, 50, 50, 255 }, { 40, 40, 44, 255 }, { 236, 220, 180, 255 }, { 90, 140, 70, 255 },
                                        { 60, 110, 200, 128 } };
        drawMode.recentCount = (int)(sizeof(recent) / sizeof(recent[0]));
        memcpy(drawMode.recent, recent, sizeof(recent));
    }
    // The draw mode's kept state (DrawKeep above): the package's in the editor, --draw-state's anywhere
    static DrawKeep drawKeep;
    if (MONOLITH_EDITING) drawKeep.path = drawStateArg ? MS_StrDup(drawStateArg) : !automated ? DrawStatePath(scene->root) : NULL;
    DrawKeepLoad(&drawKeep, &drawMode);
    if (modelPickerFlag && shotPath) {
        if (!edit.active) EditModeOpen(&edit, &designer);
        ModelPickerOpen(&picker, &edit, modelPickerFilter);
        int pending = ModelPickerPending(&picker);
        bool early = reloadTestPath || settingsChangeCount > 0;
        pickerThumbFrame = early ? 0 : modelPickerKey == 1 ? 10
                         : 29 - (pending > 0 ? (pending - 1) / PICKER_THUMBS_PER_FRAME : 0);
        if (!early && pickerThumbFrame < 10) pickerThumbFrame = 10;
    }

    if (!hidden || scripted) DisableCursor();
    if (drawPanelFlag && scripted) EnableCursor();     // (the draw panel takes clicks: a free cursor)
    // A scripted --draw-mode / --draw-panel shot is the draw mode's alone: its input never reaches the
    // designer, even after B leaves the mode (an ENTER there once opened the prompt box and sent a
    // real request)
    bool drawShotLocked = shotPath && scripted && (drawModeFlag || drawPanelFlag);
    // The edit mode's cursor (main loop): 0 not the mode's, 1 free, 2 hidden (RMB flying, the
    // modal); where it was when it was hidden; the RMB press a click may clear the selection with
    int editCursor = 0;
    Vector2 editCursorAt = { 0 };
    double rmbDownTime = 0.0;
    float rmbMoved = 0.0f;
    bool rmbClick = false;
    // --edit-modal "... commit" / "... cancel": how the scripted modal ends (1 commit, 2 cancel)
    int editModalEnd = 0;
    char *editModalText = NULL;
    if (editModalSpec) {
        editModalText = MS_StrDup(editModalSpec);
        char *last = editModalText ? strrchr(editModalText, ' ') : NULL;
        if (last && (strcmp(last + 1, "commit") == 0 || strcmp(last + 1, "cancel") == 0)) {
            editModalEnd = last[2] == 'o' ? 1 : 2;
            *last = '\0';
        }
        editModalSpec = editModalText;
    }
    if (typeText) {
        char *text = Unescape(typeText);
        if (text) DesignerOpenPrompt(&designer, text);
        free(text);
    }
    RenderTexture shotTarget = { 0 };   // zero = draw to the window
    if (hidden) shotTarget = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
    // --bench-window: the headless bench renders into a stand-in for the window instead,
    // with the spectator HUD and overlay drawn on top (windowemu.c)
    static WindowEmu windowEmu;
    bool emulateWindow = headlessBench && benchWindowSamples >= 0 &&
                         WindowEmuLoad(&windowEmu, GetScreenWidth(), GetScreenHeight(), benchWindowSamples);
    if (emulateWindow) {
        UnloadRenderTexture(shotTarget);
        shotTarget = windowEmu.target;
    }
    int frame = 0;
    int reusedFrames = 0;   // frames composited from the frame reuse cache (logged by --shot)

    // Benchmark: for each viewpoint, warm up, then spin the camera a full turn
    // (so the camera-fitted shadow map is re-fit every frame) while timing frames.
    double *benchTimes = NULL;          // [view * benchFrames + frame]
    int *benchShadowUpdates = NULL;     // per view
    int benchView = 0, benchFrame = -BENCH_WARMUP;
    double benchLast = 0.0;
    bool benchBadViews = false;         // a bad --bench-views list: exit 1 without a frame
    if (bench) {
        unsigned char st = BenchInit();
        if (st == 0) {
            benchTimes = mainHeap.benchTimes = calloc((size_t)benchViewCount * benchFrames, sizeof(double));
            benchShadowUpdates = mainHeap.benchShadowUpdates = calloc(benchViewCount, sizeof(int));
        }
        if (st == 2) {
            fprintf(stderr, "monolith: %s\n", MS_LastError());
            benchBadViews = true;
            bench = false;
        } else if (st != 0 || !benchTimes || !benchShadowUpdates) {
            fprintf(stderr, "monolith: out of memory (bench tables)\n");
            bench = false;
        }
    }
    if (bench) {
        spec.active = true;
        SpectatorJumpTo(&spec, benchViewList[0]);
        hud = emulateWindow;
        // GPU timer queries per R3D pass; results are read back a few frames later
        if (benchProfile) R3D_SetProfilerEnabled(true);
    }

    // Frame reuse: on by default only in the interactive window. --bench (spinning or
    // still) and --shot must measure / show real rendering, so there it is off unless
    // --frame-reuse asks for it (to time a reused frame, or to prove reused == fresh).
    static FrameReuse reuse;
    // (interactive: the settings' frame reuse, which the panel can switch)
    reuse.enabled = frameReuseFlag >= 0 ? frameReuseFlag == 1 : !hidden && !bench && gfx.frameReuse;
    if (reuse.enabled || (!hidden && !bench)) {
        reuse.composite = LoadShaderFromMemory(NULL, COMPOSITE_FS);
        reuse.depthLoc = GetShaderLocation(reuse.composite, "depthTex");
    }
    if (reuse.enabled) {
        if (bench) {
            benchFrameReuse = true;
            // timed frames are reused ones, so R3D records no passes
            benchProfile = false;
            R3D_SetProfilerEnabled(false);
        }
    }
    // Settings panel (F2): settingspanel.c. --settings-panel opens it for a --shot-hud image
    static SettingsPanel panel;
    panel.interactive = !hidden || scripted;
    if (settingsPanelFlag && shotPath) PanelOpen(&panel);
    bool pendingApply = false;      // a heavy change waits one frame, so "applying ..." shows first
    const char *settingsSaveFile = automated ? (settingsPath && !noSettings ? settingsPath : NULL) : settingsFile;
    bool offApplied = false;  // --off toggles are applied on the first frame only
    float closePickTime = 0.0f; // CTRL+K: seconds left to press a request's number
    int exitStatus = benchBadViews ? 1 : 0;

    // (a scripted run: the shot waits until 10 frames after the script's last step)
    if (scripted && pickerShotFrame < 10 + InputScriptFrames() + 10) pickerShotFrame = 10 + InputScriptFrames() + 10;
    while (!benchBadViews && !WindowShouldClose())
    {
        InputFrame(frame);      // (--input-script: this frame's keys and mouse)
        float dt = fminf(GetFrameTime(), 1.0f / 20.0f);

        if (IsWindowResized()) MS_ApplyRenderResolution(scene, GetScreenWidth(), GetScreenHeight());

        // While the prompt box is open (and a couple of frames after it closed) every
        // editor key and the camera are frozen: the keys are text now
        bool typing = DesignerCapturesInput(&designer) || EditModeCapturesInput(&edit) || EditModeModalRunning(&edit) ||
                      InspectorCapturesInput(&inspector) || IdPanelCapturesInput(&idPanel) || ModelPickerCapturesInput(&picker) ||
                      (drawMode.active && drawPanel.typing);     // (the draw panel: a field has the keys)
        // ESC with nothing else to close: the menu with the keybindings button (keyspanel.c);
        // while it or the panel is open every editor key and the camera are frozen
        if ((!shotPath || scripted) && !bench) {
            bool escFree = !typing && !PanelCapturesInput(&panel) && !(spec.active && (edit.active || drawMode.active)) && !designer.snipping &&
                           !designer.volumeMode && closePickTime <= 0.0f;
            KeysUpdate(&keys, escFree);
        }
        typing = typing || KeysCapturesInput(&keys);

        // Graphics settings panel (F2, both modes; settingspanel.c). A change goes to this
        // run's settings and to the saved ones (the command-line overrides of other settings
        // stay out of the file). Heavy changes (shadow map size, bakes) are applied one frame
        // later, so the panel shows "applying ..." first; the rest at once.
        // The edit save mode (settings: Editor): switching to auto writes what is unsaved
        if ((gfx.editSave == 1) != edit.ed.autoSave) {
            edit.ed.autoSave = gfx.editSave == 1;
            if (edit.ed.autoSave && EditModeUnsaved(&edit) > 0) EditModeSave(&edit);     // (it reports on its own)
        }
        bool applyNow = pendingApply;
        bool announced = pendingApply;
        pendingApply = false;
        if (!bench) {
            bool wasOpen = panel.open;
            int changed = PanelUpdate(&panel, &gfx, GetFrameTime(), GetScreenWidth(), GetScreenHeight());
            if (changed == PANEL_RESET) {
                GfxSetPreset(&gfx, gfx.base);
                GfxSetPreset(&gfxSaved, gfx.base);
            } else if (changed >= 0) GfxRowCopy(&gfxSaved, &gfx, changed);
            if (changed != -1) {
                gfxDirty = true;
                if (changed == PANEL_RESET || GfxRowIsHeavy(changed)) {
                    pendingApply = true;
                    PanelStatusStatic(&panel, "applying ...", 30.0f);
                } else applyNow = true;
                if (changed >= 0 && GfxRowNeedsRestart(changed)) PanelStatusStatic(&panel, "window MSAA changes at the next start", 4.0f);
            }
            if (wasOpen && !panel.open && gfxDirty && settingsSaveFile) {
                bool ok = GfxSave(&gfxSaved, settingsSaveFile);
                TraceLog(ok ? LOG_INFO : LOG_WARNING, "monolith: settings %s %s", ok ? "saved to" : "could not be saved to", settingsSaveFile);
                if (ok) gfxDirty = false;
            }
            if (!shotPath && !typing && !PanelCapturesInput(&panel) && IsKeyPressed(KEY_F2)) PanelOpen(&panel);
        }
        // --settings-change: after 10 frames, the same live path as a change in the panel
        if (shotPath && frame == 10 && settingsChangeCount > 0) {
            for (int k = 0; k < settingsChangeCount; k++) {
                GfxSetKey(&gfx, settingsChanges[k]);
                GfxSetKey(&gfxSaved, settingsChanges[k]);
            }
            gfxDirty = applyNow = true;
        }
        if (applyNow) {
            bool restartTried = false;      // (then the restart's result is the panel's status)
            if (gfx.q.shadowSize != appliedShadowSize) {
                restartTried = true;
                unsigned char r = RestartRenderer(&loadOpt, gfx.q.shadowSize);
                if (r == 2) {               // R3D closed, the map failed to load: quit
                    fprintf(stderr, "monolith: renderer restart failed: %s\n", MS_LastError());
                    exitStatus = 1;
                    break;
                }
                // (formatted now: the syncs below may replace the last error)
                char *text = r == 0 ? MonoStrf("renderer restarted: sun shadow map %d px", R3D_GetHint(R3D_HINT_SHADOW_DIR_SIZE))
                                    : MonoStrf("renderer not restarted: %s", MS_LastError());
                if (r == 0) {               // restarted
                    appliedShadowSize = gfx.q.shadowSize;
                    designer.version = (unsigned int)-1;   // remap the selection by its keys
                    inspector.scene = NULL;                 // a new scene: the inspector's filter again
                    idPanel.scene = NULL;                   // (and the id panel's rows)
                    ModelPickerDropThumbs(&picker);         // (rendered again while it is open)
                    reuse.keyValid = reuse.cacheValid = false;
                    DrawModeBeforeSync(&drawMode);          // (the paint atlases, for the working copy)
                    EditModeSync(&edit, &designer, true);  // (the working copy goes live again)
                    DrawModeSync(&drawMode, &edit, true);
                } else gfx.q.shadowSize = appliedShadowSize;   // 1: the map on disk is broken: keep the old one
                TraceLog(LOG_INFO, "monolith: %s", text ? text : "out of memory");
                PanelStatusf(&panel, 5.0f, "%s", text ? text : "out of memory");
                free(text);
            }
            ApplyEnvironment(&effects);
            if (!hidden && gfx.vsync != appliedVsync) {
                if (gfx.vsync) SetWindowState(FLAG_VSYNC_HINT);
                else ClearWindowState(FLAG_VSYNC_HINT);
                appliedVsync = gfx.vsync;
            }
            if (!hidden && gfx.fpsCap != appliedFps) {
                SetTargetFPS(FpsTarget(gfx.fpsCap));
                appliedFps = gfx.fpsCap;
            }
            if (!hidden && !bench) {
                reuse.enabled = gfx.frameReuse && reuse.composite.id;
                if (!reuse.enabled) reuse.keyValid = reuse.cacheValid = false;
            }
            if (!restartTried && announced) PanelStatusStatic(&panel, "applied", 1.5f);
        }
        typing = typing || PanelCapturesInput(&panel);
        // Render inspector (inspector.c): F3 outlines, SHIFT+F3 its panel (takes the keys while
        // open), F4 buffer views; spectator mode
        if (spec.active && !shotPath && !bench && !PanelCapturesInput(&panel) &&
            (InspectorCapturesInput(&inspector) || !typing)) InspectorUpdate(&inspector);
        typing = typing || InspectorCapturesInput(&inspector);
        // (the view-only build's refusals of F9, the K keys and the id panel's edits are tested by
        // --input-script shots: there these keys also act in a scripted shot; in the Linux editor
        // liveKeys is !shotPath, as it always was)
        bool liveKeys = !shotPath || (!MONOLITH_EDITING && scripted);
        // The id panel (idpanel.c): F5 in spectator mode; takes every key while open, its edits go
        // through the edit mode (live, undoable)
        if (spec.active && liveKeys && !bench && !typing && IsKeyPressed(KEY_F5)) IdPanelOpen(&idPanel, NULL);
        else if (liveKeys && !bench && IdPanelCapturesInput(&idPanel) &&
                 IdPanelUpdate(&idPanel, &edit, &designer, &spec) == EDITMODE_RELOADED) {
            ApplyEnvironment(&effects);
            mapMessageError = false;
        }
        typing = typing || IdPanelCapturesInput(&idPanel);

        // Closing requests (launcher.c): K = the last finished request, SHIFT+K = all finished
        // ones, CTRL+K then 1-9 / 0 (= 10) = request [n] of the HUD list, also a running one
        // (then its number twice; coordinator mode: cancels it), CTRL+SHIFT+K = close the
        // coordinator. While CTRL+K waits for the number, the digits don't toggle effects and
        // ESC cancels instead of quitting.
        bool closeKeys = spec.active && !typing && liveKeys && !bench;
        bool picking = closeKeys && closePickTime > 0.0f;   // digits belong to the close
        closePickTime = picking ? closePickTime - GetFrameTime() : 0.0f;
        if (closeKeys && !MONOLITH_EDITING) {
            if (IsKeyPressed(KEY_K)) DesignerMessageStatic(&designer, true, MSG_VIEW_ONLY);    // (no requests: view-only)
        } else if (closeKeys) {
            // (each branch posts its text; every outcome of LauncherCloseCoordinator /
            // LauncherCloseAllDone / LauncherCloseLatestDone is in the launcher's note)
            bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            int number = 0;
            for (int k = 0; k < 10; k++)
                if (IsKeyPressed(KEY_ZERO + k) || IsKeyPressed(KEY_KP_0 + k)) number = k ? k : 10;
            if (IsKeyPressed(KEY_K)) {
                closePickTime = 0.0f;
                if (ctrl && shift) {
                    LauncherCloseCoordinator();
                    SpectatorMessagef(&spec, "%s", LauncherNote());
                } else if (ctrl) {
                    closePickTime = 3.0f;
                    SpectatorMessageStatic(&spec, "close which request? press its number (ESC cancels)");
                } else if (shift) {
                    LauncherCloseAllDone();
                    SpectatorMessagef(&spec, "%s", LauncherNote());
                } else {
                    LauncherCloseLatestDone();
                    SpectatorMessagef(&spec, "%s", LauncherNote());
                }
            } else if (picking && IsKeyPressed(KEY_ESCAPE)) {
                closePickTime = 0.0f;
                SpectatorMessageStatic(&spec, "nothing closed");
            } else if (picking && number) {
                unsigned char r = LauncherCloseNumber(number);
                // a running request asks for its number again (1): keep waiting for it
                closePickTime = r == 1 ? 3.0f : 0.0f;
                SpectatorMessagef(&spec, "%s", r >= 2 ? MS_LastError() : LauncherNote());   // 2, 3: failed
            }
        }

        // Effect toggles, to see what each one contributes (1 volumetric fog, 2 fog, 3 SSAO,
        // 4 SSIL, 5 bloom, 6 sun; --off applies them on the first frame)
        bool first = !offApplied, toggled = false;
        for (int k = 0; k < 6; k++) {
            bool pens = spec.active && drawMode.active;     // (1..5: the draw mode's pens)
            if ((!typing && !picking && !pens && IsKeyPressed(KEY_ONE + k)) || (first && strchr(offKeys, '1' + k))) {
                effects.flip[k] = !effects.flip[k];
                toggled = true;
            }
        }
        if (toggled) ApplyEnvironment(&effects);
        offApplied = true;
        R3D_Environment *env = R3D_GetEnvironment();
        if (!typing && IsKeyPressed(KEY_F1))    hud = !hud;
        // O (spectator mode): the barrier overlay on / off, kept in the settings file (barrier_overlay)
        if (spec.active && !typing && IsKeyPressed(KEY_O)) {
            gfx.barrierOverlay = gfxSaved.barrierOverlay = !gfx.barrierOverlay;
            MsgSetStatic(&mapMessage, gfx.barrierOverlay ? "barriers shown (O hides them)" : "barriers hidden (O shows them)");
            mapMessageError = false;
            mapMessageTime = 3.0f;
            if (settingsSaveFile && !GfxSave(&gfxSaved, settingsSaveFile))
                TraceLog(LOG_WARNING, "monolith: settings could not be saved to %s", settingsSaveFile);
        }
        if (!typing && IsKeyPressed(KEY_F12)) {
            char shot[1100];
            if (SaveScreenshot(shot, sizeof(shot))) {
                TraceLog(LOG_INFO, "monolith: screenshot saved to %s", shot);
                MsgSetf(&mapMessage, "screenshot saved: %s", shot);
            } else {
                TraceLog(LOG_WARNING, "monolith: screenshot: could not write into %s", screenshotsDir);
                MsgSetf(&mapMessage, "screenshot: could not write into %s", screenshotsDir);
            }
            mapMessageError = false;
            mapMessageTime = 6.0f;
        }
        // (not while the package on disk is broken: the export copies the files on disk)
        if (!MONOLITH_EDITING && !typing && liveKeys && !bench && IsKeyPressed(KEY_F9)) {
            MsgSetStatic(&mapMessage, MSG_VIEW_ONLY);   // (where the export's result would show)
            mapMessageTime = 6.0f;
        } else if (!typing && !shotPath && !bench && !mapMessageError && IsKeyPressed(KEY_F9)) {
            // Export the current map (export.c); the result shows in the map message line
            char *dir = ExportDefaultDir(scene);
            // (ExportScene logs its result line; not shown as a map error: those stay until the
            // next successful reload)
            unsigned char r = 1;
            if (dir) r = ExportScene(scene, dir, false);
            else {
                MS_SetError("export: out of memory; nothing written");
                TraceLog(LOG_WARNING, "monolith: %s", MS_LastError());
            }
            free(dir);
            MsgSetf(&mapMessage, "%s", r == 0 ? ExportNote() : MS_LastError());   // 0: exported
            mapMessageError = false;
            mapMessageTime = 6.0f;
        }

        if (!typing && !(spec.active && drawMode.active) && IsKeyPressed(KEY_TAB)) {     // (draw mode: the panel)
            spec.active = !spec.active;
            if (spec.active) {
                // Start from the player's eyes
                Camera3D pc = PlayerCamera(&player);
                spec.pos = pc.position;
                spec.yaw = player.yaw;
                spec.pitch = player.pitch;
                spec.vel = Vector3Zero();
                spec.flying = false;
            }
        }

        // Map hot reload (not during --bench/--shot runs, which must stay deterministic)
        if (!bench && (!shotPath || reloadTestPath)) {
            MS_ReloadResult r = MS_UNCHANGED;
            if (!shotPath) r = MS_Poll(scene, GetFrameTime());
            else if (frame == 10) {
                // --reload-test: the same reload handling as a changed map package
                r = MS_Reload(scene, reloadTestPath);
                if (r == MS_RELOAD_FAILED) TraceLog(LOG_WARNING, "monolith: --reload-test: %s", MS_LastError());
            }
            if (r == MS_RELOADED) {
                // The runtime swapped in the new GPU resources (new before old: R3D caches
                // instance buffer names) and its caches key on scene->version. The map's
                // environment applies again, then the overrides and the current toggles.
                ApplyEnvironment(&effects);
                if (scene->modelCount > 0)
                    MsgSetf(&mapMessage, "map reloaded (%d boxes, %d models)", scene->boxCount, scene->modelCount);
                else MsgSetf(&mapMessage, "map reloaded (%d boxes)", scene->boxCount);
                mapMessageTime = 3.0f;
                mapMessageError = false;
                TraceLog(LOG_INFO, "monolith: %s", MsgText(&mapMessage));
                // A change made elsewhere: unsaved manual edits (and paint) are applied again on top
                // of it (else the undo history ends)
                DrawModeBeforeSync(&drawMode);
                EditModeSync(&edit, &designer, false);
                DrawModeSync(&drawMode, &edit, false);
            } else if (r == MS_RELOAD_FAILED) {
                mapMessageError = true;      // (the HUD shows MS_Error(scene): the scene keeps it, whole)
                TraceLog(LOG_WARNING, "monolith: map reload failed: %s", MS_Error(scene));
            }
            if (mapMessageTime > 0.0f) mapMessageTime -= GetFrameTime();
            if (!shotPath) {
                LauncherUpdate(GetFrameTime());     // unclamped: its HUD timers sum these
                // A classified request was just queued / sent (or failed to be): say so once
                const char *event = LauncherEventNote(NULL);
                if (event) SpectatorMessagef(&spec, "%s", event);
            }
        }
        // --edit-script with --shot: after 10 frames (every cache drawn), the edits go live like
        // the edit mode's, so the shot shows what the caches make of a live edit
        if (shotPath && editScriptPath && frame == 10) {
            if (MapEditRunLiveScript(&edit.ed, editScriptPath) != 0) exitStatus = 1;
            ApplyEnvironment(&effects);     // (a `disk` command may have changed it)
        }
        // --paint-script with --shot: then the strokes, live (docs/painting.md 14)
        if (shotPath && paintScriptPath && frame == 10) {
            DesignerSync(&designer);
            Camera3D cam = spec.active ? SpectatorCamera(&spec) : PlayerCamera(&player);
            // (`pick screen`: the shot's target; the frame below draws the shot's scene into it again)
            ScreenReadCtx sr = { .camera = cam, .target = shotTarget };
            drawMode.screenRead = ScreenRead;
            drawMode.screenUser = &sr;
            if (DrawModeRunScript(&drawMode, &edit, &designer, cam, GetScreenWidth(), GetScreenHeight(), paintScriptPath) != 0) exitStatus = 1;
            drawMode.screenRead = NULL;
            drawMode.screenUser = NULL;
            if (sr.own) UnloadRenderTexture(sr.target);
            if (!drawModeFlag && !drawPanelFlag) DrawModeClose(&drawMode);
            ApplyEnvironment(&effects);
        }
        // --edit-modal: the transform modal opens on the selection after 10 frames (every cache
        // drawn), as G / ALT+R / S or a gizmo drag would (r90, reset, rot: that edit now);
        // "... commit" / "... cancel" ends it after 20 frames
        if (shotPath && editModalSpec && frame == 10) {
            DesignerSync(&designer);
            Camera3D cam = spec.active ? SpectatorCamera(&spec) : PlayerCamera(&player);
            bool reloaded;
            if (!EditModeOpenModal(&edit, &designer, cam, editModalSpec, editModalEnd, &reloaded)) exitStatus = 1;
            if (reloaded) ApplyEnvironment(&effects);   // (r90, reset, rot: one edit, live)
        }
        if (shotPath && editModalEnd && frame == 20 && EditModeModalRunning(&edit)) {
            if (EditModeFinishModal(&edit, &designer, editModalEnd == 1) == EDITMODE_RELOADED) ApplyEnvironment(&effects);
            else if (editModalEnd == 1) exitStatus = 1;     // (the commit was refused: the log says why)
        }
        // --model-picker-key: ENTER / ESC once every listed thumbnail is rendered
        if (shotPath && modelPickerKey && !pickerKeyDone && picker.open && frame > pickerThumbFrame &&
            ModelPickerPending(&picker) == 0) {
            pickerKeyDone = true;
            if (modelPickerKey == 2) {
                ModelPickerClose(&picker);
                pickerShotFrame = frame + 1;
            } else {
                DesignerSync(&designer);
                if (ModelPickerEnter(&picker, &edit, &designer) == EDITMODE_RELOADED) ApplyEnvironment(&effects);
                else exitStatus = 1;
                pickerShotFrame = frame + 21;
            }
        }
        DesignerSync(&designer);    // remaps the selection after a reload

        double frameStart = GetTime();
        if (bench) {
            double now = frameStart;
            if (benchFrame >= 0 && benchFrame < benchFrames) benchTimes[(size_t)benchView * benchFrames + benchFrame] = now - benchLast;
            benchLast = now;
            if (++benchFrame == benchFrames) {
                if (++benchView == benchViewCount) break;
                SpectatorJumpTo(&spec, benchViewList[benchView]);
                benchFrame = -BENCH_WARMUP;
            }
            const MS_Viewpoint *vp = &scene->viewpoints[benchViewList[benchView]];
            float baseYaw, basePitch;
            LookAngles(vp->eye, vp->target, &baseYaw, &basePitch);
            spec.yaw = baseYaw;
            if (!benchStill) spec.yaw += 2.0f * PI * (float)(benchFrame > 0 ? benchFrame : 0) / benchFrames;
            R3D_SetProfilerTag(benchFrame >= 0 ? BENCH_TAG(benchView, benchFrame) : -1);
        }
        // (a scripted --draw-mode / --draw-panel shot goes through the draw mode's own input; its keys
        // and clicks never reach the designer, even once the mode is left: see drawShotLocked)
        else if (!shotPath || scripted) {
            DesignerAction action = DESIGNER_NONE;
            // Edit mode: the camera flies only while RMB is held; an RMB click (released within
            // 0.25 s, moved less than 4 px) clears the selection (DesignerUpdate doesn't then)
            bool editKeys = spec.active && edit.active;
            // Draw mode (B, drawmode.c): the cursor is free and the mouse moves the camera (wheel,
            // MMB orbit, SPACE + LMB / SHIFT + MMB pan: spectator.c, docs/painting.md 18.7), not over
            // the draw panel; the designer never sees its keys and clicks (no selection, no prompt box)
            bool drawKeys = spec.active && drawMode.active;
            designer.editMode = editKeys || drawKeys;
            spec.rmbFly = editKeys;
            spec.mouseNav = drawKeys;
            spec.navBlocked = false;
            if (editKeys && !EditModeModalRunning(&edit) && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                rmbDownTime = GetTime();
                rmbMoved = 0.0f;
                rmbClick = true;
            }
            if (rmbClick && IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) rmbMoved += Vector2Length(GetMouseDelta());
            if (rmbClick && IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
                rmbClick = false;
                if (editKeys && !typing && GetTime() - rmbDownTime < 0.25 && rmbMoved < 4.0f) DesignerClearSelection(&designer);
            }
            if (drawKeys && !PanelCapturesInput(&panel) && !InspectorCapturesInput(&inspector) && !IdPanelCapturesInput(&idPanel) &&
                !KeysCapturesInput(&keys)) {
                // The draw panel first: while the cursor is over it, LMB is its and nothing is painted
                DrawPanelAction act = DRAWPANEL_ACT_NONE;
                bool onPanel = !drawMode.panelHidden && editCursor == 1 && DrawPanelUpdate(&drawPanel, &drawMode, &act);
                spec.navBlocked = onPanel;
                drawMode.keysBlocked = typing || drawPanel.typing;
                DrawModeResult dr = DrawModeUpdate(&drawMode, &edit, &designer, SpectatorCamera(&spec), onPanel, dt);
                if (act != DRAWPANEL_ACT_NONE && DrawModePanelAction(&drawMode, &edit, (int)act, drawPanel.faces) == DRAWMODE_RELOADED)
                    dr = DRAWMODE_RELOADED;
                if (dr == DRAWMODE_RELOADED) {
                    ApplyEnvironment(&effects);
                    mapMessageError = false;
                }
                typing = typing || drawPanel.typing;
            } else if (drawShotLocked) {
                // A scripted --draw-mode / --draw-panel shot that left the mode: B opens it again, and
                // nothing else reaches the designer (no selection, no prompt box, no request)
                if (spec.active && !typing && !KeysCapturesInput(&keys) && !PanelCapturesInput(&panel) && IsKeyPressed(KEY_B))
                    DrawModeOpen(&drawMode, &edit, &designer);
            } else if (spec.active && !PanelCapturesInput(&panel) && !InspectorCapturesInput(&inspector) && !IdPanelCapturesInput(&idPanel) &&
                !KeysCapturesInput(&keys) && ModelPickerCapturesInput(&picker)) {
                // The model picker takes every key while open (ENTER places a model: a live edit)
                if (ModelPickerUpdate(&picker, &edit, &designer) == EDITMODE_RELOADED) {
                    ApplyEnvironment(&effects);
                    mapMessageError = false;
                }
                typing = true;
            } else if (spec.active && !PanelCapturesInput(&panel) && !InspectorCapturesInput(&inspector) && !IdPanelCapturesInput(&idPanel) &&
                       !KeysCapturesInput(&keys)) {
                // The edit mode's id panel and the transform modal take every key while they run
                bool idPanel = EditModeCapturesInput(&edit), modal = EditModeModalRunning(&edit);
                // The rotate gizmo under the free cursor: an LMB press there is its drag, not a
                // selection click (DesignerUpdate skips that frame)
                bool gizmoOk = editKeys && editCursor == 1 && !idPanel && !modal && !typing && !IsMouseButtonDown(MOUSE_BUTTON_RIGHT) &&
                               !designer.barrierPlace;     // (a barrier's corner clicks go to the designer)
                bool gizmoClick = EditModeGizmoHover(&edit, &designer, SpectatorCamera(&spec), GetMousePosition(), gizmoOk) && gizmoOk &&
                                  IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
                bool drawKey = !idPanel && !modal && !typing && !designer.promptOpen && !designer.snipping && IsKeyPressed(KEY_B) &&
                               !IsKeyDown(KEY_LEFT_CONTROL) && !IsKeyDown(KEY_RIGHT_CONTROL) && !IsKeyDown(KEY_LEFT_ALT) && !IsKeyDown(KEY_RIGHT_ALT);
                if (drawKey && !MONOLITH_EDITING) {
                    DesignerMessageStatic(&designer, true, MSG_VIEW_ONLY);
                } else if (drawKey) {
                    DrawModeOpen(&drawMode, &edit, &designer);      // (B: the draw mode, edit / volume mode end; nothing else this frame)
                } else if (!idPanel && !modal && !gizmoClick) {
                    action = DesignerUpdate(&designer, dt);
                    typing = DesignerCapturesInput(&designer);
                    if (designer.snipping || (designer.volumeMode && !designer.barrierPlace)) edit.active = false;   // C and V leave the edit mode
                }
                // Manual edits rewrite and reload the map: then the environment applies again,
                // as after a hot reload (the selection already follows the edited objects)
                if (!drawKey && (idPanel || modal || !typing) && action == DESIGNER_NONE &&
                    EditModeUpdate(&edit, &designer, SpectatorCamera(&spec), dt) == EDITMODE_RELOADED) {
                    ApplyEnvironment(&effects);
                    mapMessageError = false;
                }
                // I (without CTRL / ALT, not while RMB flies) opens the model picker
                bool modKeys = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_ALT) ||
                               IsKeyDown(KEY_RIGHT_ALT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
                if (edit.active && !typing && action == DESIGNER_NONE && !EditModeCapturesInput(&edit) &&
                    !EditModeModalRunning(&edit) && !modKeys && !designer.barrierPlace && IsKeyPressed(KEY_I))
                    ModelPickerOpen(&picker, &edit, NULL);
                // (view-only: no edit mode to open the picker in; I says why)
                if (!MONOLITH_EDITING && !drawKey && !typing && action == DESIGNER_NONE && !modKeys && IsKeyPressed(KEY_I))
                    DesignerMessageStatic(&designer, true, MSG_VIEW_ONLY);
                typing = typing || EditModeCapturesInput(&edit) || EditModeModalRunning(&edit) || ModelPickerCapturesInput(&picker);
            }
            spec.holdMove = (edit.active || drawMode.active) && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL));
            // The cursor: free in edit mode (hidden while RMB flies or the modal runs) and in draw mode
            // (always: the mouse navigates there), unless the prompt box, snip mode or the settings
            // panel own it (they show and hide it themselves)
            if (!hidden || scripted) {
                int want = !(spec.active && (edit.active || drawMode.active)) || designer.promptOpen || designer.snipping || panel.open || idPanel.open ||
                           keys.open || keys.menu ? 0
                         : EditModeModalRunning(&edit) || (edit.active && IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) ? 2 : 1;
                if (want != editCursor) {
                    if (want == 1 && IsCursorHidden()) {
                        EnableCursor();
                        if (editCursor == 2) SetMousePosition((int)editCursorAt.x, (int)editCursorAt.y);
                    } else if (want == 2 && !IsCursorHidden()) {
                        editCursorAt = GetMousePosition();
                        DisableCursor();
                    } else if (want == 0 && editCursor != 0 && !IsCursorHidden() && !designer.promptOpen && !designer.snipping && !panel.open &&
                               !idPanel.open)
                        DisableCursor();
                    editCursor = want;
                }
            }
            // (draw mode: R is the blur tool)
            if (!MONOLITH_EDITING && spec.active && !typing && IsKeyPressed(KEY_R)) {
                DesignerMessageStatic(&designer, true, MSG_VIEW_ONLY);     // (no requests: view-only)
            } else if (spec.active && !drawMode.active && !typing && !EditModeTakesArrows(&edit) && IsKeyPressed(KEY_R)) {
                Vector3 eye;
                float yaw, pitch;
                if (LauncherLastDone(&eye, &yaw, &pitch)) {
                    SpectatorFlyToPose(&spec, eye, yaw, pitch);
                    SpectatorMessageStatic(&spec, "back to the last finished request");
                } else SpectatorMessageStatic(&spec, "no finished request yet");
            }
            if (spec.active) { if (!typing) UpdateSpectator(&spec, &player, dt, !DesignerTakesArrows(&designer) && !EditModeTakesArrows(&edit)); }
            else if (!typing) UpdatePlayer(&player, dt);

            // A request needs the file to be what the scene shows (its line numbers, indices and
            // hash describe the loaded scene, its session reads the file): with unsaved edits
            // ENTER only says so, and CTRL+S in the box saves and sends
            int unsaved = EditModeUnsaved(&edit);
            bool saveAndSend = designer.promptOpen && unsaved > 0 && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) &&
                               IsKeyPressed(KEY_S);
            if (action == DESIGNER_SUBMIT && unsaved > 0)
                DesignerSubmitDone(&designer, false, TextFormat("%d unsaved edit%s: CTRL+S saves and sends, ESC back", unsaved,
                                                                unsaved == 1 ? "" : "s"));
            else if (action == DESIGNER_SUBMIT || saveAndSend) {
                // The camera is frozen while the box is open: this is the view the user typed at
                if (saveAndSend && EditModeSave(&edit) == 2) DesignerSubmitDone(&designer, false, MS_LastError());   // not saved
                else Submit(&designer, SpectatorCamera(&spec), spec.yaw, spec.pitch, (RenderTexture){ 0 });
            }
        }
        Camera3D camera = spec.active ? SpectatorCamera(&spec) : PlayerCamera(&player);
        // (edit mode with a free cursor: what is under the cursor, not the crosshair)
        bool cursorPick = editCursor == 1 && (!hidden || scripted);
        // The barrier overlay (O, the setting barrier_overlay): spectator mode only, never walking; a
        // --shot only with --show-barriers. While it is on the pick sees barriers (designer.c PickShown)
        bool barriersShown = shotPath ? showBarriersFlag : spec.active && gfx.barrierOverlay && (!bench || emulateWindow);
        DesignerShowBarriers(&designer, barriersShown);
        if (cursorPick) DesignerPickCursor(&designer, camera, GetScreenWidth(), GetScreenHeight(), GetMousePosition());
        else if (spec.active || shotPath) DesignerPickCamera(&designer, camera);
        InspectorApply(&inspector);     // buffer view and draw filter (nothing while it is off)
        DrawKeepTick(&drawKeep, &drawMode, GetTime());     // (the draw mode's kept state: 1 s after a change)
        if (shotPath && drawMode.active && !scripted && (drawPanelFlag || drawModeFlag)) {
            // --draw-mode / --draw-panel shots without a script: the cursor at --cursor (default: the
            // middle of the shot) is what the HUD, the brush cursor and the panel show (a scripted one
            // goes through the draw mode's own input above)
            Vector2 at = cursorSet ? cursorArg : (Vector2){ GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f };
            DrawModeHover(&drawMode, camera, at, GetScreenWidth(), GetScreenHeight());
        }
        // Model picker thumbnails: a few per frame, outside R3D_Begin/End (the render leaves the
        // environment and every cache as they were); dropped when the map is loaded again
        if (picker.open && !shotPath && !(spec.active && edit.active)) ModelPickerClose(&picker);   // (left the mode)
        if (!bench && (!shotPath || frame >= pickerThumbFrame)) ModelPickerRenderThumbs(&picker, PICKER_THUMBS_PER_FRAME);

        // Frame reuse (see its region): same key as the last frame -> composite the cache
        bool reused = false;
        bool unchanged = false;
        if (reuse.enabled) {
            FrameKey key;
            MakeFrameKey(&key, camera, &gfx, &inspector);
            unchanged = reuse.keyValid && memcmp(&key, &reuse.key, sizeof(key)) == 0;
            reuse.key = key;
            reuse.keyValid = true;
            if (!unchanged) reuse.cacheValid = false;
        }

        BeginDrawing();
            ClearBackground(BLACK);
            if (emulateWindow) WindowEmuClear(&windowEmu);

            // HUD and overlays: on the window, or with --shot-hud into the shot
            bool drawUi = !shotPath || shotHud;
            // (--shot-hud shows the designer UI in walk mode too: shots of arbitrary --pos views)
            bool designerUi = (spec.active || shotPath) && (!bench || emulateWindow);
            // Inspector outlines: with the HUD, and in every --inspect shot (labels: --shot-hud)
            // Another panel open (settings, id panel, model picker, keybindings): the inspector steps
            // aside (its panel closes, its outlines, labels and HUD line wait until that panel closes)
            bool otherPanel = panel.open || idPanel.open || picker.open || keys.open || keys.menu;
            if (otherPanel) inspector.panelOpen = false;
            bool inspectUi = inspector.mode != INSPECT_OFF && designerUi && (shotPath || (drawUi && hud)) && !otherPanel;

            // A hidden window has no visible pixels, so reading back the default
            // framebuffer is undefined: render --shot frames into a texture instead.
            bool shadowUpdated = false;
            if (unchanged) {
                if (!reuse.cacheValid) {
                    // first unchanged frame: render once more, into the cache
                    if (reuse.rt.texture.width != GetScreenWidth() || reuse.rt.texture.height != GetScreenHeight()) {
                        if (reuse.rt.id) UnloadRenderTexture(reuse.rt);
                        reuse.rt = LoadSceneCache(GetScreenWidth(), GetScreenHeight());
                    }
                    shadowUpdated = DrawScene(camera, reuse.rt, true);   // the cache keeps its depth for later frames
                    reuse.cacheValid = true;
                } else {
                    reused = true;
                    reusedFrames++;
                    lastR3DEndTime = 0.0;
                }
                if (shotTarget.id) { BeginTextureMode(shotTarget); ClearBackground(BLACK); }
                DrawSceneCache(&reuse);
                if (shotTarget.id) EndTextureMode();
            } else {
                shadowUpdated = DrawScene(camera, shotTarget, (drawUi && designerUi) || inspectUi || barriersShown);   // overlays depth-test
            }
            if (bench && shadowUpdated && benchFrame >= 0 && benchFrame < benchFrames) benchShadowUpdates[benchView]++;
            // The screen eyedropper (docs/painting.md 18.8): the scene is in its target and nothing is
            // drawn over it yet (overlays, the brush cursor, the HUD come next), so the pixel is the
            // scene's as seen. Only on the frame after a click, never per frame
            int screenX, screenY;
            if (DrawModeScreenWanted(&drawMode, &screenX, &screenY)) {
                Color picked = { 0 };
                bool ok = shotTarget.id ? ReadScenePixel(shotTarget.id, shotTarget.texture.width, shotTarget.texture.height,
                                                         screenX, screenY, &picked)
                                        : ReadScenePixel(0, GetRenderWidth(), GetRenderHeight(), screenX, screenY, &picked);
                DrawModeScreenPicked(&drawMode, ok, picked);
            }

            // The barrier overlay first: every other overlay (the selection's outlines) goes over it
            if (barriersShown && (shotPath || (drawUi && hud))) {
                if (shotTarget.id) BeginTextureMode(shotTarget);
                DesignerDrawBarriers(&designer, camera);
                if (shotTarget.id) EndTextureMode();
            }
            bool uiToTarget = (drawUi && shotPath) || emulateWindow;
            if (uiToTarget) BeginTextureMode(shotTarget);
            if (inspectUi) {
                if (!uiToTarget && shotTarget.id) BeginTextureMode(shotTarget);
                InspectorDrawOverlay(&inspector, camera, GetScreenWidth(), GetScreenHeight(), drawUi && hud);
                if (!uiToTarget && shotTarget.id) EndTextureMode();
            }
            // Draw mode: the brush cursor and the panel say what is under the cursor; the designer's
            // hover outline, hit marker and HUD lines (it doesn't select in the mode) stay away
            bool drawing = drawMode.active && (spec.active || drawPanelFlag);
            bool drawPanelShown = drawUi && hud && drawing && !drawMode.panelHidden && (drawPanelFlag || !shotPath || scripted);
            if (drawUi && hud && designerUi) {
                if (!drawing) DesignerDrawOverlay(&designer, camera, true);
                EditModeDrawGuides(&edit, camera);
                DesignerDrawVolumes(&designer, camera, GetScreenWidth(), GetScreenHeight(), false);
                if (spec.active) EditModeDrawGizmo(&edit, &designer, camera);     // (the rotate gizmo, 2D)
                if (spec.active || shotPath) DrawModeDrawCursor(&drawMode, camera);    // (the brush footprint, 3D)
                // (no crosshair while the edit mode's cursor is free or the modal runs)
                if (!cursorPick && !(spec.active && drawMode.active) && !(EditModeModalRunning(&edit) && (!hidden || scripted)))
                    DesignerDrawCrosshair(GetScreenWidth(), GetScreenHeight());
            }
            // Snip mode (screen regions) shows even with the HUD off, like the prompt box
            if (drawUi && designerUi) DesignerDrawRegions(&designer, GetScreenWidth(), GetScreenHeight(), false);
            int hudBottom = 10 + UI_HUD_LINE + 8;   // below the HUD lines (the draw panel's top)
            if (drawUi && hud) {
                // The status line: fps | mode | effect toggles (on / off by colour) | preset
#define TOGGLE_TEXT(on, name) ((on) ? "^" name "^" : "~" name "~")
                // (draw mode: 1..5 are the pens, so the toggles show without their keys)
                UiHudHints(TextFormat(drawing ? "%d fps%s|%s|%s  %s  %s  %s  %s  %s|{F2} settings: %s"
                                              : "%d fps%s|%s|{1} %s  {2} %s  {3} %s  {4} %s  {5} %s  {6} %s|{F2} settings: %s",
                           GetFPS(), reused ? " (cached)" : "",
                           spec.active ? TextFormat("SPECTATOR  %.1f m/s", spec.speed) : player.fly ? "FLY" : "WALK",
                           TOGGLE_TEXT(env->volumetricFog.enabled, "volFog"), TOGGLE_TEXT(env->fog.mode, "fog"),
                           TOGGLE_TEXT(env->ssao.enabled, "ssao"), TOGGLE_TEXT(env->ssil.enabled, "ssil"),
                           TOGGLE_TEXT(env->bloom.mode, "bloom"), TOGGLE_TEXT(MS_GetRenderSettings(scene).sunOn, "sun"),
                           MS_QualityName(GfxPreset(&gfx))),
                           12, 10, UI_HUD_LINE, UI_S, UI_TEXT);
#undef TOGGLE_TEXT

                if (spec.active) {
                    // (no key lists here: ESC opens the keybindings, keyspanel.c)
                    int y = 10 + UI_HUD_LINE + 8;
                    if (!drawing) y += DesignerDrawHud(&designer, 12, y);
                    y += DrawModeDrawHud(&drawMode, &edit, 12, y);
                    // (draw mode: the DRAW line has the unsaved count; the edit mode's lines only
                    // for the edits a rebase could not apply)
                    if (!drawing || edit.ed.failedCount > 0) y += EditModeDrawHud(&edit, &designer, 12, y);
                    if (!otherPanel) y += InspectorDrawHud(&inspector, 12, y);
                    hudBottom = y + 6;
                    if (drawPanelShown) {
                        // The panel's top stays put: the refusal, the draw mode's message and the
                        // requests go to the right of it
                        int my = hudBottom + DrawModeDrawMessages(&drawMode, DRAWPANEL_RIGHT + 12, hudBottom);
                        LauncherDrawHud(DRAWPANEL_RIGHT + 12, my + 4);
                    } else {
                        y += DrawModeDrawMessages(&drawMode, 12, y);
                        LauncherDrawHud(12, y + 4);
                    }
                    if (spec.messageTime > 0.0f && !designer.promptOpen)
                        UiToast(MsgText(&spec.message), GetScreenWidth() / 2, GetScreenHeight() - 76, UI_XL, UI_INK, fminf(spec.messageTime, 1.0f));
                } else if (!shotPath) {
                    LauncherDrawHud(12, 10 + UI_HUD_LINE + 4);  // finished requests also show while walking
                }
            }
            if (drawPanelShown) DrawPanelDraw(&drawPanel, &drawMode, hudBottom);
            if (drawUi && (mapMessageError || mapMessageTime > 0.0f) && !designer.promptOpen) {
                float a = mapMessageError ? 1.0f : fminf(mapMessageTime, 1.0f);
                int y = GetScreenHeight() - 36;
                if (mapMessageError) UiHudMark(MS_Error(scene), 12, y, 26, UI_M, UI_ERROR);   // (a broken map: stays)
                else {
                    // Fades as a whole (the alphas scaled, not replaced)
                    Color strip = UiAlpha(UI_BACKDROP, a), ink = UiAlpha(UI_INK, a);
                    DrawRectangle(6, y, UiMeasure(MsgText(&mapMessage), UI_M) + 12, 26, strip);
                    UiText(MsgText(&mapMessage), 12, y + (26 - UiLineHeight(UI_M)) / 2, UI_M, ink);
                }
            }
            // The prompt box shows even with the HUD off (Enter opened it, it must be visible)
            if (drawUi && designerUi) DesignerDrawPrompt(&designer, GetScreenWidth(), GetScreenHeight());
            // The settings panel shows even with the HUD off (F2 opened it)
            if (drawUi) PanelDraw(&panel, &gfx, GetScreenWidth(), GetScreenHeight(), reused);
            if (drawUi && designerUi) InspectorDrawPanel(&inspector, GetScreenWidth(), GetScreenHeight());
            if (drawUi && designerUi) IdPanelDraw(&idPanel, GetScreenWidth(), GetScreenHeight());
            if (drawUi && designerUi) ModelPickerDraw(&picker, GetScreenWidth(), GetScreenHeight());
            if (drawUi) KeysDraw(&keys, GetScreenWidth(), GetScreenHeight());     // (on top of everything)
            if (uiToTarget) EndTextureMode();
        EndDrawing();
        if (emulateWindow) WindowEmuPresent(&windowEmu);
        // A reused frame costs ~0.2 ms, so an uncapped loop would spin at thousands of fps
        // doing copies (fans, battery). Pace cached frames to the display instead; rendered
        // frames stay uncapped. Not in --bench/--shot, which time or count real frames.
        if (reused && !bench && !shotPath) {
            int hz = GetMonitorRefreshRate(GetCurrentMonitor());
            double budget = 1.0 / (hz > 0 ? hz : 60);
            double spent = GetTime() - frameStart;
            if (spent < budget) WaitTime(budget - spent);
        }
        if (bench) {
            if (benchFrame >= 0) BenchAddCpuTime(benchView, GetTime() - frameStart, lastR3DEndTime);
            BenchCollectProfile(false);
        }
        // A hidden window presents nothing, so swaps don't pace the frames; wait for the
        // GPU instead, or the bench would time how fast commands are queued.
        if (headlessBench) glFinish();

        // (--model-picker: the shot waits for every listed thumbnail and for --model-picker-key)
        if (shotPath && ++frame >= pickerShotFrame && !(picker.open && ModelPickerPending(&picker) > 0) &&
            !(modelPickerKey && !pickerKeyDone && picker.open)) {
            Image img = ReadTarget(shotTarget);
            if (!PngWrite(img, shotPath)) TraceLog(LOG_ERROR, "monolith: failed to save %s", shotPath);
            UnloadImage(img);
            if (pickFlag) {
                DesignerPrintHit(designer.hit);
                Camera3D cam = spec.active ? SpectatorCamera(&spec) : PlayerCamera(&player);
                PrintSurfaceHit(cam.position, Vector3Normalize(Vector3Subtract(cam.target, cam.position)));
                fflush(stdout);
            }
            if (pickAtCount > 0) {
                // The same camera and ray frame as the shot (and as screen regions)
                Camera3D cam = spec.active ? SpectatorCamera(&spec) : PlayerCamera(&player);
                for (int k = 0; k < pickAtCount; k++) {
                    printf("pick-at %g %g (of %d x %d):\n", pickAtArgs[k].x, pickAtArgs[k].y, shotW, shotH);
                    DesignerHit dh = DesignerPickAt(cam, shotW, shotH, pickAtArgs[k].x, pickAtArgs[k].y);
                    DesignerPrintHit(dh);
                    // (the ray through the point it hit: the same surface)
                    if (dh.obj.kind != MS_OBJ_NONE)
                        PrintSurfaceHit(cam.position, Vector3Normalize(Vector3Subtract(dh.point, cam.position)));
                }
                fflush(stdout);
            }
            if (reuse.enabled) TraceLog(LOG_INFO, "monolith: frame reuse: %d of %d frames composited from the cache", reusedFrames, frame);
            if (submitText) {
                float yaw = spec.active ? spec.yaw : player.yaw, pitch = spec.active ? spec.pitch : player.pitch;
                char *text = Unescape(submitText);
                if (text) DesignerOpenPrompt(&designer, text);
                free(text);
                // (as ENTER in the prompt box: a request needs the file to be what the scene shows)
                int unsaved = EditModeUnsaved(&edit);
                bool ok = false;
                if (!text) printf("submit: failed: out of memory\n");
                else if (unsaved > 0) printf("submit: failed: %d unsaved edit%s: CTRL+S saves and sends, ESC back\n", unsaved,
                                             unsaved == 1 ? "" : "s");
                else {
                    unsigned char r = Submit(&designer, camera, yaw, pitch, shotTarget);
                    ok = r == 0;            // 0: queued / sent; 1: not sent
                    printf("submit: %s: %s\n", ok ? "ok" : "failed", ok ? LauncherNote() : MS_LastError());
                }
                fflush(stdout);
                // --submit-wait / --close-submitted: poll the request (result.md, the
                // terminal process) for a while, then close it the way CTRL+K n does
                if (ok && (submitWait > 0.0f || closeSubmitted)) {
                    for (double t0 = GetTime(); GetTime() - t0 < submitWait; ) {
                        WaitTime(0.1);
                        LauncherUpdate(0.1f);
                        bool failed = false;
                        const char *event = LauncherEventNote(&failed);    // a classified request queued / sent
                        if (event) { printf("submit: classified: %s: %s\n", failed ? "failed" : "ok", event); fflush(stdout); }
                    }
                    if (closeSubmitted) {
                        int n = LauncherNewestNumber();
                        for (int tries = 0; tries < 2; tries++) {   // a running request asks twice
                            unsigned char r = LauncherCloseNumber(n);
                            printf("close [%d]: %s\n", n, r >= 2 ? MS_LastError() : LauncherNote());     // 2, 3: failed
                            if (r != 1) break;      // 1: asks for the number again
                        }
                        double t0 = GetTime();
                        while (LauncherClosing() && GetTime() - t0 < 10.0) { WaitTime(0.1); LauncherUpdate(0.1f); }
                        printf("close: terminal process %s after %.1f s\n", LauncherClosing() ? "STILL RUNNING" : "gone", GetTime() - t0);
                    }
                    fflush(stdout);
                }
            }
            break;
        }
    }

    if (bench && benchView == benchViewCount) {
        BenchCollectProfile(true);  // the last frames' results are still in flight
        WriteBenchReport(benchTimes, benchShadowUpdates, shadowSize, shadowRange, gfx.q.shadowMode != MS_SHADOW_ALWAYS, headless, benchNote, argc, argv);
    }

    if (gfxDirty && settingsSaveFile) {
        bool ok = GfxSave(&gfxSaved, settingsSaveFile);
        TraceLog(ok ? LOG_INFO : LOG_WARNING, "monolith: settings %s %s", ok ? "saved to" : "could not be saved to", settingsSaveFile);
    }
    PanelClose(&panel);             // (switches the GPU profiler off if the panel switched it on)
    PanelFree(&panel);
    LauncherCloseOnExit();          // MONOLITH_CLOSE_ON_EXIT=1: close finished requests' terminals
    R3D_SetProfilerEnabled(false);  // frees its GL queries
    BenchFree();
    DesignerFree(&designer);
    SpectatorFree(&spec);
    // Closing with unsaved manual edits (raylib can't cancel a close): the working copy goes to
    // the settings folder's unsaved/, the map file is not touched (interactive runs only)
    int unsavedAtExit = EditModeUnsaved(&edit);
    if (unsavedAtExit > 0 && !hidden) {
        char path[1400];
        bool ok = GfxUnsavedPath(scene->path, path, sizeof(path)) && MapEditWriteWork(&edit.ed, path);
        // (and each unsaved paint atlas under <map>-<time>/<package path>, docs/painting.md 11.3)
        if (ok && DrawModeUnsavedAtlases(&drawMode) > 0) {
            char dir[1400];
            snprintf(dir, sizeof(dir), "%s", path);
            char *dot = strrchr(dir, '.');
            if (dot && strcmp(dot, ".map") == 0) *dot = '\0';
            DrawModeWriteUnsaved(&drawMode, dir);
        }
        TraceLog(ok ? LOG_WARNING : LOG_ERROR, ok ? "monolith: %d unsaved edit%s written to %s (the map file is unchanged)"
                 : "monolith: %d unsaved edit%s could not be written (%s)", unsavedAtExit, unsavedAtExit == 1 ? "" : "s",
                 ok ? path : "no settings folder");
    } else if (unsavedAtExit > 0) TraceLog(LOG_INFO, "monolith: %d unsaved edit%s dropped (automated run)", unsavedAtExit,
                                           unsavedAtExit == 1 ? "" : "s");
    MsgFree(&mapMessage);
    ModelPickerFree(&picker);
    EditModeFree(&edit);            // (the history releases its paint through the draw mode's hooks)
    DrawKeepFree(&drawKeep);        // (a change still waiting is written)
    DrawModeFree(&drawMode);
    free(editModalText);
    InputScriptFree();
    InspectorFree(&inspector);
    IdPanelFree(&idPanel);
    KeysFree(&keys);
    if (emulateWindow) WindowEmuUnload(&windowEmu);
    else if (hidden) UnloadRenderTexture(shotTarget);
    if (reuse.rt.id) UnloadRenderTexture(reuse.rt);
    if (reuse.composite.id) UnloadShader(reuse.composite);
    MS_Unload(scene);
    R3D_Close();
    UiClose();
    CloseWindow();
    return exitStatus;
}

// #endregion
