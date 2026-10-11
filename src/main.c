// monolith -- AI-assisted map designer for brutalist concrete levels (raylib + R3D)
//
// The level is a plain-text map file (maps/monolith.map, format: see map.c), hot-reloaded
// while the game runs. In spectator mode you point at things, select boxes and type a
// request; the game writes requests/<id>/ and opens Claude Code in a new terminal to edit
// the map (AI-MAP-TOOL.md). Engine reference: PROMPT-INSTRUCTIONS.md.
//
// Controls:
//   WASD / mouse   walk & look          SHIFT  walk faster
//   SPACE          jump                 F      toggle fly (noclip)
//   1..6           toggle effects (volumetric fog, fog, SSAO, SSIL, bloom, sun)
//   F1             toggle HUD (also hides the crosshair and selection overlay)
//   "(cached)" after the fps: nothing changed, the frame reused the last R3D image
//   F12            screenshot           TAB    toggle spectator camera
//   ESC            quit (closes the prompt box / leaves snip mode instead while open)
//   The HUD's bottom line shows "map reloaded (N boxes)" after a map edit, or the parse
//   error (file:line: message) in red while the file is broken (the old map stays loaded).
//
// Spectator:
//   WASD / mouse   move & look          E/Q or SPACE        up/down
//   mouse wheel    base speed           SHIFT x4, ALT x0.25
//   LEFT/RIGHT     fly to previous/next viewpoint
//   P              print + copy current view as --pos/--look arguments (--pos = feet)
//   G              move the player here
//
// Map designer (spectator mode, see designer.c / launcher.c):
//   crosshair      the box under it is outlined; the HUD shows its group and map line
//   LMB            select the box under the crosshair (replaces the selection)
//   SHIFT+LMB      add/remove that box          CTRL+LMB   select the box's whole group
//   CTRL+SHIFT+LMB add the whole group          RMB        clear the selection
//   ENTER          open the request prompt box. In the box: type, BACKSPACE/DELETE
//                  (CTRL+BACKSPACE: word), LEFT/RIGHT/UP/DOWN, HOME/END (CTRL: whole
//                  text), CTRL+V paste, SHIFT+ENTER new line, ENTER send to Claude Code,
//                  ESC cancel (discards the text). While the box is open every game key
//                  and the camera are frozen and the mouse cursor is free. If sending
//                  fails, the box stays open with the text.
//   C              snip mode: the camera freezes and the cursor is free. Drag = a screen
//                  region (replaces the others), SHIFT+drag = add one (up to 16), ENTER =
//                  prompt box with the regions (and the box selection made before C)
//                  attached, RMB / ESC / C = leave snip mode and drop the regions. ESC in the
//                  prompt box returns to snip mode with the regions; a successful submit
//                  ends it. The bundle lists what each region shows (designer.c)
//   R              fly back to the camera of the last finished request
//   The HUD lists open requests ("running", "waiting for result.md") and, for 20 s,
//   finished ones with the first line of their result.md.
//   The selection survives map hot reloads: boxes are matched by identical geometry,
//   else by the same map line + index within it + group; the HUD says if any got lost.
//
// Command line:
//   --shot <file>  render 30 frames offscreen (1600x900) in a hidden window (never mapped,
//                  so the window manager doesn't show it), save the image to <file> and exit.
//                  No hot reload or request polling during shots
//   --shot-size <w> <h>   size of the --shot image (default 1600 900); request bundles pass
//                  their window size so the reproduction matches view.png exactly
//   --pos x y z    start position (feet)
//   --look yawDeg pitchDeg   start view angles (with --view: override the viewpoint's)
//   --eye x y z    start position of the eye (= --pos x y-1.62 z); with --view it moves the
//                  spectator there instead (request bundles use it for their --shot command)
//   --off <keys>   start with these effects toggled off, e.g. --off 34 (SSAO + SSIL)
//   --view <n>     start in spectator mode at viewpoint n (1-based)
//   --ao <mode>    screen-space AO / indirect light trade-off (atmosphere.c): full (default),
//                  ssao (SSIL off), lite (SSIL off, SSAO 8 samples), off (SSAO + SSIL off)
//   --bloom off    bloom pass off, exposure x1.19 instead (same mean brightness, no faint
//                  halo at bright openings; atmosphere.c). Default: on
//   --fx key=value   override one effect setting, repeatable, e.g. --fx ssil.radius=2.5
//                  (keys: ssao./ssil.samples, .radius; ssao.intensity/.power; ssil.gi/.ao;
//                  bloom.levels/.intensity; exposure). Applied after --ao / --bloom
//   --shadow <size> <range>   sun shadow map size (px) and distance (m), default 8192 100
//   --shadow-camera   sun shadow from R3D's camera-fitted map, re-rendered only when the
//                  cached map no longer covers the view (the pre-tile path; default: static
//                  world-fixed tiles drawn once per map load, see shadow.c; for A/B tests)
//   --shadow-always   camera-fitted map re-rendered every frame (A/B tests and debugging)
//   --fog-cache <voxel> <sub>   trade-off, off by default: bake the sun visibility of the
//                  volumetric fog into a 3D texture once per map load (voxel size in m,
//                  sub^3 samples per voxel, e.g. 0.5 2) and let the fog march fetch it
//                  instead of the shadow map (R3D patch 8); changes the light shafts slightly
//   --no-prepass   no depth pre-pass before the G-buffer pass (R3D patch 6; same image,
//                  for A/B tests: the concrete shader then runs for every overdrawn fragment)
//   --bench        visit every viewpoint, spin the camera, print frame time stats, write a
//                  Markdown report to benchmark/<timestamp>_shadow-<size>-<range>.md and exit
//   --headless     with --bench: hidden window + offscreen target + glFinish per frame,
//                  so nothing appears on screen (compare headless runs only with each other)
//   --bench-frames <n>   timed frames per viewpoint (10..600, default 600); fewer = quick
//                  screening runs (the camera spins faster, so the shadow map updates more)
//   --bench-still  like --bench, but the camera doesn't spin (the still-camera ideal: no
//                  shadow-map re-renders); the report gets a _still suffix
//   --no-frame-reuse   always render with R3D. By default the interactive window reuses the
//                  last R3D image while nothing that affects it changed (camera, effect
//                  toggles, sun, map version, window size): the frame is then only the cached
//                  image + HUD/overlay, and the HUD fps line says "(cached)". See "Frame reuse"
//   --frame-reuse  turn frame reuse on where it is off by default: --shot (to check that a
//                  reused frame equals a fresh one; logs how many frames were reused) and
//                  --bench-still (times a reused frame; report suffix _reuse, no GPU profile)
//   --no-profile   with --bench: no per-pass GPU timings (R3D profiler, on by default when
//                  R3D is built with R3D_GPU_PROFILER, as build.sh does)
//   --note "text"  free-form note added to the benchmark report
//   --map <file>   load this map instead of maps/monolith.map (path relative to the CWD;
//                  the default is the repo's maps/ while it exists, else maps/ next to the
//                  executable). The map is reloaded whenever the file changes (format: map.c)
//   --reload-test <file>   with --shot: after 10 frames, load this map through the hot-reload
//                  path (as if the map file had changed), then render the remaining frames.
//                  The shot must equal a plain --shot --map <file> (proves that every
//                  static cache is rebuilt on reload)
//   --dump-boxes <file>   load the map, write every box as hex floats (%a) to <file> and
//                  exit without opening a window (for checking that edits to the loader
//                  or the map keep the exact box list). Any map that fails to parse (with
//                  or without this flag) prints file:line: message and exits with status 1
//
// Designer debug flags (for testing without a visible window):
//   --pick         with --shot: print what the crosshair (screen centre) hits on the shot
//                  frame: box index, group, map line + its text, point, normal, distance
//   --select <line>  select every box made by this map line (repeatable, up to 64)
//   --region x y w h   attach a screen region (pixels of the shot, top-left origin), as if
//                  dragged out in snip mode (repeatable, up to 16); with --shot-hud the shot
//                  shows the snip-mode drawing, with --submit the bundle gets the regions
//   --submit "text"  with --shot: after saving the shot, run the real submit path (render
//                  view + view-marked offscreen, LauncherSubmit with the centre pick,
//                  the --select boxes and the --region regions) and print the launcher's message. This writes a real
//                  bundle into requests/ and launches a real terminal + Claude Code: for
//                  tests, set MONOLITH_TERMINAL to a stub script that only logs its arguments
//   --type "text"  open the prompt box with this text (cursor at the end)
//   --shot-hud     with --shot: draw the HUD, crosshair, selection overlay, regions and prompt box
//                  into the shot (to look at the UI; plain shots never contain it)
//   In --submit and --type text, a literal \n becomes a new line.
//
// Environment (launcher.c):
//   MONOLITH_TERMINAL   terminal to open for Claude Code: program name or path + optional
//                       args, e.g. "kitty --single-instance"
//   TERMINAL            used if MONOLITH_TERMINAL is unset; else the first of alacritty,
//                       kitty, foot, wezterm, xterm in PATH
//   MONOLITH_CLAUDE_ARGS   extra Claude Code flags, split on whitespace (default none)
//   __GL_SYNC_TO_VBLANK=0 (NVIDIA) / vblank_mode=0 (Mesa)   driver vsync off, for --bench

#include "common.h"
#include "world.h"
#include "map.h"
#include "player.h"
#include "spectator.h"
#include "atmosphere.h"
#include "shadow.h"
#include "bench.h"
#include "designer.h"
#include "launcher.h"

// Only needed for glEnable(GL_DEPTH_CLAMP), see main(), and glFinish() for --headless.
// Declared by hand rather than via <GL/gl.h> / <windows.h> (which clashes with raylib).
// On Windows x64 there is a single calling convention, so these match opengl32.dll.
#define GL_DEPTH_CLAMP 0x864F
extern void glEnable(unsigned int cap);
extern void glFinish(void);

#ifndef RESOURCES_PATH
#   define RESOURCES_PATH "./resources/"
#endif
#ifndef BENCHMARK_PATH
#   define BENCHMARK_PATH "./benchmark/"
#endif
#ifndef MAPS_PATH
#   define MAPS_PATH "./maps/"
#endif

// Data directories, with a trailing slash. Both builds are meant to be copied to other
// computers as a folder (build/linux/, build/win/), so they look next to the executable.
// On Linux, the repo directories baked in by build.sh win while they exist (i.e. on the
// dev machine), so concrete.glsl edits apply without rebuilding and bench reports land
// in the repo's benchmark/, and the map that gets hot-reloaded is the repo's maps/ one.
// Windows paths aren't baked in (wine would see Z:\...).
char resourcesDir[1024], benchmarkDir[1024];
static char mapsDir[1024];  // only main() needs it (default map)

static void InitDataPaths(void)
{
    const char *exeDir = GetApplicationDirectory();
#ifdef _WIN32
    snprintf(resourcesDir, sizeof(resourcesDir), "%sresources/", exeDir);
    snprintf(benchmarkDir, sizeof(benchmarkDir), "%sbenchmark/", exeDir);
    snprintf(mapsDir, sizeof(mapsDir), "%smaps/", exeDir);
#else
    if (DirectoryExists(RESOURCES_PATH)) snprintf(resourcesDir, sizeof(resourcesDir), "%s", RESOURCES_PATH);
    else snprintf(resourcesDir, sizeof(resourcesDir), "%sresources/", exeDir);
    if (DirectoryExists(BENCHMARK_PATH)) snprintf(benchmarkDir, sizeof(benchmarkDir), "%s", BENCHMARK_PATH);
    else snprintf(benchmarkDir, sizeof(benchmarkDir), "%sbenchmark/", exeDir);
    if (DirectoryExists(MAPS_PATH)) snprintf(mapsDir, sizeof(mapsDir), "%s", MAPS_PATH);
    else snprintf(mapsDir, sizeof(mapsDir), "%smaps/", exeDir);
#endif
}

// Box centers and sizes -> instance buffer of unit cubes (again after every map reload)
static R3D_InstanceBuffer LoadWorldInstances(void)
{
    R3D_InstanceBuffer instances = R3D_LoadInstanceBuffer(world.count, R3D_INSTANCE_POSITION | R3D_INSTANCE_SCALE);
    Vector3 *pos = R3D_MapInstances(instances, R3D_INSTANCE_POSITION, false);
    for (int i = 0; i < world.count; i++)
        pos[i] = Vector3Scale(Vector3Add(world.items[i].min, world.items[i].max), 0.5f);
    R3D_UnmapInstances(instances, R3D_INSTANCE_POSITION);

    Vector3 *scl = R3D_MapInstances(instances, R3D_INSTANCE_SCALE, false);
    for (int i = 0; i < world.count; i++)
        scl[i] = Vector3Subtract(world.items[i].max, world.items[i].min);
    R3D_UnmapInstances(instances, R3D_INSTANCE_SCALE);
    return instances;
}

static bool DumpBoxes(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    for (int i = 0; i < world.count; i++) {
        BoundingBox b = world.items[i];
        fprintf(f, "%a %a %a %a %a %a\n", b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
    }
    return fclose(f) == 0;
}

// Everything R3D needs to draw the world, so the main view and the request images
// (Submit) render through the same code
typedef struct {
    R3D_Mesh cube;
    R3D_Material concrete;
    R3D_InstanceBuffer instances;
    R3D_Light sun;
    R3D_ShadowMap sunShadow[SUN_TILE_COUNT];    // one per cached tile ([0] for the camera-fitted map)
    bool sunOn;
    bool shadowTiles;       // static tiles (default), see SunShadowTileSelect()
    SunShadowTiles sunTiles;
    SunShadowCache sunCache;    // camera-fitted map (--shadow-camera / --shadow-always)
    bool shadowReuse;       // see SunShadowNeedsUpdate()
    float fogCacheVoxel;    // --fog-cache: voxel size (m), 0 = off
    int fogCacheSub;
    unsigned int fogCacheVersion;   // world.version the fog volume was baked for, 0 = none
    float shadowRange;
    int shadowSize;
} Scene;

// Renders the world into `target` (zero = the window). Returns whether the sun shadow
// map was re-rendered. Extra renders (Submit) go through the same cache check with their
// own target's aspect, so the cached map always matches what R3D last rendered.
static double lastR3DEndTime;   // seconds, set by DrawScene

// Where the fog's baked sun visibility applies (--fog-cache): the default map's play
// area with its light shafts (hall, canyon, stairs, terrace, tower) up to 100 m high.
// The fog samples the shadow map outside it.
#define FOG_CACHE_REGION (BoundingBox){ { -70.0f, -1.0f, -150.0f }, { 75.0f, 100.0f, 105.0f } }

static bool DrawScene(Scene *sc, Camera3D camera, RenderTexture target)
{
    bool update = false;
    R3D_BeginPro((R3D_View){ .camera = R3D_CameraFromRL(camera), .target = target });
        if (sc->sunOn && sc->shadowTiles) {
            // Static tiles: drawn once per map load (per tile), chosen by eye position only
            int tile = SunShadowTileSelect(&sc->sunTiles, sc->sun.direction, sc->shadowRange, sc->shadowSize,
                                           camera.position, world.version, &update);
            if (update) TraceLog(LOG_INFO, "monolith: sun shadow tile %d drawn (%d so far)", tile, sc->sunTiles.renders);
            R3D_PushLightFixedShadow(sc->sun, sc->sunShadow[tile], sc->sunTiles.tile[tile].viewProj, update);
        } else if (sc->sunOn) {
            // Only re-render the shadow map when the cached one no longer covers the view
            float aspect = target.id ? (float)target.texture.width / target.texture.height
                                     : (float)GetRenderWidth() / GetRenderHeight();
            update = !sc->shadowReuse ||
                SunShadowNeedsUpdate(&sc->sunCache, sc->sun.direction, sc->shadowRange, camera, aspect, sc->shadowSize);
            R3D_PushLightEx(sc->sun, sc->sunShadow[0], update);
        }
        R3D_DrawMeshInstanced(sc->cube, sc->concrete, sc->instances, world.count);
        if (sc->sunOn && sc->fogCacheVoxel > 0.0f && sc->fogCacheVersion != world.version) {
            // Fog sun visibility, baked once per map load into the shadow layer this frame
            // doesn't sample (that tile slot is forgotten: it holds the bake map now)
            int slot = (sc->shadowTiles && sc->sunTiles.current == 1) ? 0 : 1;
            if (sc->shadowTiles) SunShadowTileForget(&sc->sunTiles, slot);
            BoundingBox region = FOG_CACHE_REGION;
            R3D_BakeFogSunVisibility(sc->sunShadow[slot], SunShadowBoxMatrix(sc->sun.direction, region, 2.0f),
                                     region, sc->fogCacheVoxel, sc->fogCacheSub);
            sc->fogCacheVersion = world.version;
        }
    double r3dEndStart = GetTime();
    R3D_End();
    lastR3DEndTime = GetTime() - r3dEndStart;   // CPU side of R3D_End, for the bench report
    return update;
}

// ======================================================================================
// #region Frame reuse
// ======================================================================================
// A still camera over an unchanged scene produces the same R3D image every frame: R3D has
// no temporal effects in this setup (no TAA, no auto exposure, no history buffers; SSAO,
// SSIL and fog noise are keyed on screen position only, and the per-frame TIME /
// FRAME_INDEX only reach user shaders, which concrete.glsl doesn't read). So when nothing
// that feeds R3D changed since the last frame, the frame is composited from a cached copy
// of R3D's colour + depth instead of re-rendering it.
//   - While the key changes, R3D renders straight into the window as before (no extra
//     copy on moving frames). On the first unchanged frame R3D renders once more into the
//     cache (that frame is also the "settle" frame: the sun shadow map was already updated
//     for this camera, if at all, so nothing converges any further). Every later unchanged
//     frame only composites the cache and draws the HUD / overlay on top.
//   - The cache keeps R3D's depth in a depth *texture*, and the composite writes it back
//     with gl_FragDepth, so the designer overlay depth-tests exactly as over a fresh frame.
//     A shader pass rather than glBlitFramebuffer, because the window's framebuffer is
//     multisampled (FLAG_MSAA_4X_HINT), and blitting into a multisampled framebuffer is
//     an error; R3D's own final blit is a fullscreen shader pass for the same reason.

// Everything that changes R3D's image. Compared bytewise, so floats compare exactly; a
// false "changed" (e.g. -0 vs 0) only costs one render.
typedef struct {
    Camera3D camera;
    R3D_Environment env;    // every effect setting (toggles 1-5 and anything else)
    R3D_Light sun;
    R3D_ShadowMap sunShadow;
    bool sunOn;             // toggle 6
    bool shadowReuse;
    unsigned int mapVersion;
    int width, height;      // R3D resolution = window size
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
static void MakeFrameKey(FrameKey *k, const Scene *sc, Camera3D camera)
{
    memset(k, 0, sizeof(*k));
    memcpy(&k->camera, &camera, sizeof(camera));
    memcpy(&k->env, R3D_GetEnvironment(), sizeof(k->env));
    memcpy(&k->sun, &sc->sun, sizeof(k->sun));
    memcpy(&k->sunShadow, &sc->sunShadow, sizeof(k->sunShadow));
    k->sunOn = sc->sunOn;
    k->shadowReuse = sc->shadowReuse;
    k->mapVersion = world.version;
    k->width = GetScreenWidth();
    k->height = GetScreenHeight();
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

// A request: view.png is the frame exactly as --shot renders it (offscreen, no HUD),
// view-marked.png the same pixels plus the selection, hit point and crosshair. One R3D
// render into `rt`, read back before and after drawing the overlay on top (R3D_End leaves
// its depth in rt, so the overlay is depth-tested like in the live view).
static bool Submit(Scene *sc, Designer *d, Camera3D cam, float yaw, float pitch, RenderTexture rt,
                   char *msg, int msgSize)
{
    // Window mode: an offscreen target at R3D's resolution (the window size), like --shot
    bool own = rt.id == 0;
    if (own) rt = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());

    static DesignRequest req;
    DesignerBuildRequest(d, cam, yaw, pitch, rt.texture.width, rt.texture.height, &req);
    DrawScene(sc, cam, rt);
    Image view = ReadTarget(rt);
    BeginTextureMode(rt);
        DesignerDrawOverlay(d, cam, false);
        DesignerDrawCrosshair(rt.texture.width, rt.texture.height);
        DesignerDrawRegions(d, rt.texture.width, rt.texture.height, true);
    EndTextureMode();
    Image marked = ReadTarget(rt);

    bool ok = LauncherSubmit(&req, view, marked, msg, msgSize);
    TraceLog(ok ? LOG_INFO : LOG_WARNING, "monolith: submit: %s", msg);
    UnloadImage(view);
    UnloadImage(marked);
    if (own) UnloadRenderTexture(rt);
    DesignerSubmitDone(d, ok, msg);
    return ok;
}

// Command line text for --submit / --type: a literal \n becomes a new line
static const char *Unescape(const char *in)
{
    static char out[PROMPT_MAX];
    int n = 0;
    for (const char *c = in; *c && n < PROMPT_MAX - 1; c++) {
        if (c[0] == '\\' && c[1] == 'n') { out[n++] = '\n'; c++; }
        else out[n++] = *c;
    }
    out[n] = 0;
    return out;
}

// ======================================================================================
// #region Main
// ======================================================================================

int main(int argc, char **argv)
{
    InitDataPaths();

    // Player starts at the dark end of the entry canyon, facing the hall
    Player player = { .feet = { 0, 0, 90 }, .yaw = PI };

    const char *shotPath = NULL;
    int shotW = 1600, shotH = 900;  // --shot-size
    const char *offKeys = "";
    int startView = 0;
    int shadowSize = 8192;
    float shadowRange = 100.0f;
    bool bench = false;
    bool headless = false;      // --bench without a visible window
    bool shadowReuse = true;    // see SunShadowNeedsUpdate()
    bool depthPrepass = true;   // --no-prepass
    bool shadowTiles = true;    // see SunShadowTileSelect()
    float fogCacheVoxel = 0.0f;
    int fogCacheSub = 2;
    const char *benchNote = NULL;
    const char *mapPath = NULL;
    const char *dumpPath = NULL;
    const char *reloadTestPath = NULL;
    bool eyeSet = false, lookSet = false;   // --eye / --look given (they override --view)
    Vector3 eye = { 0 };
    bool pickFlag = false, shotHud = false;
    int frameReuseFlag = -1;    // --no-frame-reuse 0, --frame-reuse 1, default -1 (see below)
    const char *submitText = NULL, *typeText = NULL;
    const char *aoMode = "full", *bloomMode = "on";
    const char *fxSettings[16];
    int fxCount = 0;
    int selectLines[64], selectLineCount = 0;
    Rectangle regionArgs[MAX_REGIONS];
    int regionArgCount = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shotPath = argv[++i];
        else if (strcmp(argv[i], "--shot-size") == 0 && i + 2 < argc) {
            shotW = (int)Clamp((float)atoi(argv[i + 1]), 64.0f, 8192.0f);
            shotH = (int)Clamp((float)atoi(argv[i + 2]), 64.0f, 8192.0f);
            i += 2;
        }
        else if (strcmp(argv[i], "--off") == 0 && i + 1 < argc) offKeys = argv[++i];
        else if (strcmp(argv[i], "--view") == 0 && i + 1 < argc) startView = atoi(argv[++i]);
        else if (strcmp(argv[i], "--ao") == 0 && i + 1 < argc) aoMode = argv[++i];
        else if (strcmp(argv[i], "--bloom") == 0 && i + 1 < argc) bloomMode = argv[++i];
        else if (strcmp(argv[i], "--fx") == 0 && i + 1 < argc) {
            if (fxCount < 16) fxSettings[fxCount++] = argv[i + 1];
            i++;
        }
        else if (strcmp(argv[i], "--bench") == 0) bench = true;
        else if (strcmp(argv[i], "--bench-still") == 0) bench = benchStill = true;
        else if (strcmp(argv[i], "--no-profile") == 0) benchProfile = false;
        else if (strcmp(argv[i], "--headless") == 0) headless = true;
        else if (strcmp(argv[i], "--shadow-always") == 0) shadowReuse = shadowTiles = false;
        else if (strcmp(argv[i], "--shadow-camera") == 0) shadowTiles = false;
        else if (strcmp(argv[i], "--fog-cache") == 0 && i + 2 < argc) {
            // Clamped: 0.25 m is already 228 MB over the default region; 0 or less turns it off
            fogCacheVoxel = (float)atof(argv[i + 1]);
            fogCacheVoxel = fogCacheVoxel > 0.0f ? Clamp(fogCacheVoxel, 0.25f, 8.0f) : 0.0f;
            fogCacheSub = (int)Clamp((float)atoi(argv[i + 2]), 1.0f, 4.0f);
            i += 2;
        }
        else if (strcmp(argv[i], "--no-prepass") == 0) depthPrepass = false;
        else if (strcmp(argv[i], "--bench-frames") == 0 && i + 1 < argc)
            benchFrames = (int)Clamp((float)atoi(argv[++i]), 10.0f, (float)BENCH_FRAMES);
        else if (strcmp(argv[i], "--note") == 0 && i + 1 < argc) benchNote = argv[++i];
        else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) mapPath = argv[++i];
        else if (strcmp(argv[i], "--dump-boxes") == 0 && i + 1 < argc) dumpPath = argv[++i];
        else if (strcmp(argv[i], "--reload-test") == 0 && i + 1 < argc) reloadTestPath = argv[++i];
        else if (strcmp(argv[i], "--pick") == 0) pickFlag = true;
        else if (strcmp(argv[i], "--shot-hud") == 0) shotHud = true;
        else if (strcmp(argv[i], "--no-frame-reuse") == 0) frameReuseFlag = 0;
        else if (strcmp(argv[i], "--frame-reuse") == 0) frameReuseFlag = 1;
        else if (strcmp(argv[i], "--submit") == 0 && i + 1 < argc) submitText = argv[++i];
        else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) typeText = argv[++i];
        else if (strcmp(argv[i], "--select") == 0 && i + 1 < argc) {
            int line = atoi(argv[++i]);
            if (selectLineCount < 64) selectLines[selectLineCount++] = line;
        }
        else if (strcmp(argv[i], "--region") == 0 && i + 4 < argc) {
            if (regionArgCount < MAX_REGIONS)
                regionArgs[regionArgCount++] = (Rectangle){ atof(argv[i + 1]), atof(argv[i + 2]), atof(argv[i + 3]), atof(argv[i + 4]) };
            i += 4;
        }
        else if (strcmp(argv[i], "--shadow") == 0 && i + 2 < argc) {
            // Clamped: a zero range or size would divide by zero in the shadow tile layout
            shadowSize = (int)Clamp((float)atoi(argv[i + 1]), 256.0f, 16384.0f);
            shadowRange = Clamp((float)atof(argv[i + 2]), 10.0f, 2000.0f);
            i += 2;
        }
        else if (strcmp(argv[i], "--pos") == 0 && i + 3 < argc) {
            player.feet = (Vector3){ atof(argv[i + 1]), atof(argv[i + 2]), atof(argv[i + 3]) };
            i += 3;
        } else if (strcmp(argv[i], "--eye") == 0 && i + 3 < argc) {
            eye = (Vector3){ atof(argv[i + 1]), atof(argv[i + 2]), atof(argv[i + 3]) };
            // in double, so --eye x y z lands on the same float feet as --pos x y-1.62 z
            player.feet = (Vector3){ eye.x, (float)(atof(argv[i + 2]) - EYE_HEIGHT), eye.z };
            eyeSet = true;
            i += 3;
        } else if (strcmp(argv[i], "--look") == 0 && i + 2 < argc) {
            player.yaw = atof(argv[i + 1]) * DEG2RAD;
            player.pitch = atof(argv[i + 2]) * DEG2RAD;
            lookSet = true;
            i += 2;
        }
    }

    // --ao / --bloom / --fx: reject unknown names before a window opens
    {
        R3D_Environment scratch = { 0 };
        if (!ApplyAoMode(&scratch, aoMode)) {
            fprintf(stderr, "monolith: unknown --ao mode '%s'. Modes:\n%s", aoMode, AoModeHelp());
            return 1;
        }
        if (!ApplyBloomMode(&scratch, bloomMode)) {
            fprintf(stderr, "monolith: unknown --bloom mode '%s' (on, off)\n", bloomMode);
            return 1;
        }
        for (int k = 0; k < fxCount; k++) {
            if (!ApplyFxSetting(&scratch, fxSettings[k])) {
                fprintf(stderr, "monolith: bad --fx '%s' (key=number). Keys:\n%s", fxSettings[k], FxSettingHelp());
                return 1;
            }
        }
    }

    // --- World: load the map before opening a window, so a broken map just prints the error
    {
        char defaultMap[1100], err[512];
        snprintf(defaultMap, sizeof(defaultMap), "%smonolith.map", mapsDir);
        if (!mapPath) mapPath = defaultMap;
        if (!LoadMap(mapPath, err, sizeof(err))) {
            fprintf(stderr, "monolith: cannot load the map\n%s\n", err);
            return 1;
        }
        TraceLog(LOG_INFO, "monolith: %s: %d boxes, %d groups", world.path, world.count, world.groupCount);
        if (dumpPath) {
            bool ok = DumpBoxes(dumpPath);
            fprintf(ok ? stdout : stderr, ok ? "monolith: wrote %d boxes to %s\n" : "monolith: cannot write %d boxes to %s\n",
                    world.count, dumpPath);
            return ok ? 0 : 1;
        }
    }

    unsigned int windowFlags = FLAG_MSAA_4X_HINT;   // no vsync, uncapped
    // --shot: hidden window, never mapped, so i3 & co. never show or focus it
    // --bench --headless too: then nothing ever appears on screen
    bool hidden = shotPath || (bench && headless);
    windowFlags |= hidden ? FLAG_WINDOW_HIDDEN : FLAG_WINDOW_RESIZABLE;
    SetConfigFlags(windowFlags);
    // Distinct titles let the window manager route automated runs elsewhere, e.g. i3:
    //   for_window [title="^monolith-(bench|shot)$"] move container to workspace number 10
    // Headless benches use the size the tiled bench window gets on a 1080p screen, so
    // their numbers line up with the visible runs.
    bool headlessBench = bench && headless;
    InitWindow(headlessBench ? 1920 : shotPath ? shotW : 1600, headlessBench ? 1006 : shotPath ? shotH : 900,
               bench ? "monolith-bench" : shotPath ? "monolith-shot" : "monolith");
    SetTargetFPS(0);
    // Esc must close the prompt box without quitting, so quitting on Esc is done by hand
    // in the main loop (the window's close button still ends WindowShouldClose())
    SetExitKey(KEY_NULL);

    // Sun shadow resolution. Static tiles (shadow.c) and R3D's camera-fitted map
    // both cover a sphere of radius ~1.5x range (70 deg fov / 16:9; the fitted map
    // grows with wider aspects), so texel size = 2 * radius / shadow map size.
    // range 100 m @ 8192 px -> ~3.7 cm texels (was 140 m @ 4096 -> ~10.7 cm).
    // VRAM: 16-bit depth, R3D allocates 2 layers -> 8192^2 * 2 B * 2 = 256 MB (the two
    // tiles use exactly these two layers), plus the min/max tiles (2 x 512^2 RG32F, 4 MB).
    R3D_SetHint(R3D_HINT_SHADOW_DIR_SIZE, shadowSize);

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
    glEnable(GL_DEPTH_CLAMP);

    // --- World -> instanced unit cubes
    Scene scene = { .shadowReuse = shadowReuse, .shadowTiles = shadowTiles,
                    .fogCacheVoxel = fogCacheVoxel, .fogCacheSub = fogCacheSub,
                    .shadowRange = shadowRange, .shadowSize = shadowSize };
    scene.cube = R3D_GenMeshCube(1.0f, 1.0f, 1.0f);
    scene.instances = LoadWorldInstances();

    scene.concrete = R3D_GetDefaultMaterial();
    scene.concrete.shader = R3D_LoadSurfaceShader(TextFormat("%sshaders/concrete.glsl", resourcesDir));
    float patternScale = 1.0f;
    R3D_SetSurfaceShaderUniform(scene.concrete.shader, "u_scale", &patternScale);
    // The concrete never discards (alpha is always 1), so turning the alpha test off
    // changes nothing on screen but lets R3D pre-pass it: depth first, then the
    // expensive procedural shader runs once per visible pixel instead of once per
    // overdrawn fragment (pillar forests, fins: ~1.2-2.4x overdraw). R3D patch 6.
    scene.concrete.alphaCutoff = 0.0f;
    R3D_SetDepthPrepass(depthPrepass);

    // --- Atmosphere
    R3D_Cubemap sky;
    R3D_AmbientMap ambient;
    SetupEnvironment(&sky, &ambient);
    ApplyAoMode(R3D_GetEnvironment(), aoMode);
    ApplyBloomMode(R3D_GetEnvironment(), bloomMode);
    for (int k = 0; k < fxCount; k++) ApplyFxSetting(R3D_GetEnvironment(), fxSettings[k]);

    R3D_Light sun = R3D_CreateDirLight(SUN_DIR, (Color){ 255, 238, 214, 255 }, 2.0f);
    // Shadow distance. Shadows fade out between 75% and 100% of it (R3D), and
    // the distance fog already hides most of what lies beyond ~100 m. It also
    // must cover volumetricFog.length (90 m) or the light shafts fade early.
    sun.range = shadowRange;
    sun.fogEnergy = 4.0f;    // how strongly the sun lights the volumetric fog
    for (int i = 0; i < SUN_TILE_COUNT; i++) {
        scene.sunShadow[i] = R3D_LoadShadowMap(R3D_LIGHT_DIR);
        scene.sunShadow[i].softness = 2.0f;
    }
    scene.sun = sun;
    scene.sunOn = true;
    if (shadowTiles)
        TraceLog(LOG_INFO, "monolith: sun shadow: static tiles, %d px, %.2f cm texels", shadowSize,
                 100.0f * SunShadowTileTexel(shadowRange, shadowSize));

    bool hud = true;

    // Map hot reload message ("map reloaded (N boxes)" or the error), shown even with the
    // HUD off. Errors stay until the next successful load.
    char mapMessage[512] = "";
    float mapMessageTime = 0.0f;
    bool mapMessageError = false;

    Spectator spec = { .speed = 12.0f, .view = -1 };
    if (startView >= 1 && startView <= VIEWPOINT_COUNT) {
        spec.active = true;
        SpectatorJumpTo(&spec, startView - 1);
        if (eyeSet) spec.pos = eye;
        if (lookSet) { spec.yaw = player.yaw; spec.pitch = player.pitch; }
    }

    // Designer: selection and prompt box (spectator mode)
    static Designer designer;
    designer.interactive = !hidden;
    DesignerSync(&designer);
    for (int i = 0; i < selectLineCount; i++) DesignerSelectLine(&designer, selectLines[i]);
    // --region: as if dragged out in snip mode (pixels of the shot)
    if (regionArgCount > 0) DesignerSnipBegin(&designer);
    for (int i = 0; i < regionArgCount; i++)
        if (!DesignerAddRegion(&designer, regionArgs[i], GetScreenWidth(), GetScreenHeight()))
            TraceLog(LOG_WARNING, "monolith: --region %.0f %.0f %.0f %.0f is empty after clamping to %dx%d; ignored",
                     regionArgs[i].x, regionArgs[i].y, regionArgs[i].width, regionArgs[i].height, GetScreenWidth(), GetScreenHeight());

    if (!hidden) DisableCursor();
    if (typeText) DesignerOpenPrompt(&designer, Unescape(typeText));
    RenderTexture shotTarget = { 0 };   // zero = draw to the window
    if (hidden) shotTarget = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
    int frame = 0;
    int reusedFrames = 0;   // frames composited from the frame reuse cache (logged by --shot)

    // Benchmark: for each viewpoint, warm up, then spin the camera a full turn
    // (so the camera-fitted shadow map is re-fit every frame) while timing frames.
    static double benchTimes[VIEWPOINT_COUNT][BENCH_FRAMES];
    static int benchShadowUpdates[VIEWPOINT_COUNT];
    int benchView = 0, benchFrame = -BENCH_WARMUP;
    double benchLast = 0.0;
    if (bench) {
        spec.active = true;
        SpectatorJumpTo(&spec, 0);
        hud = false;
        // GPU timer queries per R3D pass; results are read back a few frames later
        if (benchProfile) R3D_SetProfilerEnabled(true);
    }

    // Frame reuse: on by default only in the interactive window. --bench (spinning or
    // still) and --shot must measure / show real rendering, so there it is off unless
    // --frame-reuse asks for it (to time a reused frame, or to prove reused == fresh).
    static FrameReuse reuse;
    reuse.enabled = frameReuseFlag >= 0 ? frameReuseFlag == 1 : !hidden && !bench;
    if (reuse.enabled) {
        reuse.composite = LoadShaderFromMemory(NULL, COMPOSITE_FS);
        reuse.depthLoc = GetShaderLocation(reuse.composite, "depthTex");
        if (bench) {
            benchFrameReuse = true;
            // timed frames are reused ones, so R3D records no passes
            benchProfile = false;
            R3D_SetProfilerEnabled(false);
        }
    }
    bool offApplied = false;  // --off toggles are applied on the first frame only

    while (!WindowShouldClose())
    {
        float dt = fminf(GetFrameTime(), 1.0f / 20.0f);

        if (IsWindowResized()) R3D_SetResolution(GetScreenWidth(), GetScreenHeight());

        // Esc quits, unless the prompt box is open or snip mode is on (the designer handles it)
        if (IsKeyPressed(KEY_ESCAPE) && !designer.promptOpen && !designer.snipping) break;

        // While the prompt box is open (and a couple of frames after it closed) every
        // game key and the camera are frozen: the keys are text now
        bool typing = DesignerCapturesInput(&designer);

        // Effect toggles, to see what each one contributes
        R3D_Environment *env = R3D_GetEnvironment();
        bool first = !offApplied;
        #define TOGGLE(key, ch) ((!typing && IsKeyPressed(key)) || (first && strchr(offKeys, ch)))
        if (TOGGLE(KEY_ONE, '1'))   env->volumetricFog.enabled = !env->volumetricFog.enabled;
        if (TOGGLE(KEY_TWO, '2'))   env->fog.mode = env->fog.mode ? R3D_FOG_DISABLED : R3D_FOG_EXP2;
        if (TOGGLE(KEY_THREE, '3')) env->ssao.enabled = !env->ssao.enabled;
        if (TOGGLE(KEY_FOUR, '4'))  env->ssil.enabled = !env->ssil.enabled;
        if (TOGGLE(KEY_FIVE, '5'))  env->bloom.mode = env->bloom.mode ? R3D_BLOOM_DISABLED : R3D_BLOOM_MIX;
        if (TOGGLE(KEY_SIX, '6'))   scene.sunOn = !scene.sunOn;
        #undef TOGGLE
        offApplied = true;
        if (!typing && IsKeyPressed(KEY_F1))    hud = !hud;
        if (!typing && IsKeyPressed(KEY_F12))   TakeScreenshot("screenshot.png");

        if (!typing && IsKeyPressed(KEY_TAB)) {
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
            MapPollResult r = MAP_UNCHANGED;
            if (!shotPath) r = MapPoll(GetFrameTime());
            else if (frame == 10) {
                // --reload-test: the same reload handling as a changed map file
                char err[512];
                r = LoadMap(reloadTestPath, err, sizeof(err)) ? MAP_RELOADED : MAP_RELOAD_FAILED;
                if (r == MAP_RELOAD_FAILED) TraceLog(LOG_WARNING, "monolith: --reload-test: %s", err);
            }
            if (r == MAP_RELOADED) {
                // New buffer first, then free the old one. R3D caches the bound instance
                // buffer names (instances_enable in r3d_render.c) and skips re-binding when
                // they match; freeing first let GL hand out the same names again, so the
                // global VAO kept pointing at the deleted buffers and the reloaded map was
                // drawn with the old boxes (found by --reload-test).
                R3D_InstanceBuffer old = scene.instances;
                scene.instances = LoadWorldInstances();
                R3D_UnloadInstanceBuffer(old);
                // Boxes moved: the cached sun shadow maps are stale (the tiles also key on
                // world.version themselves)
                scene.sunCache.valid = false;
                snprintf(mapMessage, sizeof(mapMessage), "map reloaded (%d boxes)", world.count);
                mapMessageTime = 3.0f;
                mapMessageError = false;
                TraceLog(LOG_INFO, "monolith: %s", mapMessage);
            } else if (r == MAP_RELOAD_FAILED) {
                snprintf(mapMessage, sizeof(mapMessage), "%s", MapError());
                mapMessageError = true;
                TraceLog(LOG_WARNING, "monolith: map reload failed: %s", mapMessage);
            }
            if (mapMessageTime > 0.0f) mapMessageTime -= GetFrameTime();
            if (!shotPath) LauncherUpdate(GetFrameTime());   // unclamped: its HUD timers sum these
        }
        DesignerSync(&designer);    // remaps the selection after a reload

        double frameStart = GetTime();
        if (bench) {
            double now = frameStart;
            if (benchFrame >= 0 && benchFrame < benchFrames) benchTimes[benchView][benchFrame] = now - benchLast;
            benchLast = now;
            if (++benchFrame == benchFrames) {
                if (++benchView == VIEWPOINT_COUNT) break;
                SpectatorJumpTo(&spec, benchView);
                benchFrame = -BENCH_WARMUP;
            }
            float baseYaw, basePitch;
            LookAngles(VIEWPOINTS[benchView].eye, VIEWPOINTS[benchView].target, &baseYaw, &basePitch);
            spec.yaw = baseYaw;
            if (!benchStill) spec.yaw += 2.0f * PI * (float)(benchFrame > 0 ? benchFrame : 0) / benchFrames;
            R3D_SetProfilerTag(benchFrame >= 0 ? BENCH_TAG(benchView, benchFrame) : -1);
        }
        else if (!shotPath) {
            DesignerAction action = DESIGNER_NONE;
            if (spec.active) {
                action = DesignerUpdate(&designer, dt);
                typing = DesignerCapturesInput(&designer);
            }
            if (spec.active && !typing && IsKeyPressed(KEY_R)) {
                Vector3 eye;
                float yaw, pitch;
                if (LauncherLastDone(&eye, &yaw, &pitch)) {
                    SpectatorFlyToPose(&spec, eye, yaw, pitch);
                    SpectatorMessage(&spec, "back to the last finished request");
                } else SpectatorMessage(&spec, "no finished request yet");
            }
            if (spec.active) { if (!typing) UpdateSpectator(&spec, &player, dt); }
            else UpdatePlayer(&player, dt);

            if (action == DESIGNER_SUBMIT) {
                // The camera is frozen while the box is open: this is the view the user typed at
                char msg[512];
                Submit(&scene, &designer, SpectatorCamera(&spec), spec.yaw, spec.pitch, (RenderTexture){ 0 }, msg, sizeof(msg));
            }
        }
        Camera3D camera = spec.active ? SpectatorCamera(&spec) : PlayerCamera(&player);
        if (spec.active || shotPath) DesignerPickCamera(&designer, camera);

        // Frame reuse (see its region): same key as the last frame -> composite the cache
        bool reused = false;
        bool unchanged = false;
        if (reuse.enabled) {
            FrameKey key;
            MakeFrameKey(&key, &scene, camera);
            unchanged = reuse.keyValid && memcmp(&key, &reuse.key, sizeof(key)) == 0;
            reuse.key = key;
            reuse.keyValid = true;
            if (!unchanged) reuse.cacheValid = false;
        }

        BeginDrawing();
            ClearBackground(BLACK);

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
                    shadowUpdated = DrawScene(&scene, camera, reuse.rt);
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
                shadowUpdated = DrawScene(&scene, camera, shotTarget);
            }
            if (bench && shadowUpdated && benchFrame >= 0 && benchFrame < benchFrames) benchShadowUpdates[benchView]++;

            // HUD and overlays: on the window, or with --shot-hud into the shot
            bool drawUi = !shotPath || shotHud;
            if (drawUi && shotPath) BeginTextureMode(shotTarget);
            // (--shot-hud shows the designer UI in walk mode too: shots of arbitrary --pos views)
            bool designerUi = (spec.active || shotPath) && !bench;
            if (drawUi && hud && designerUi) {
                DesignerDrawOverlay(&designer, camera, true);
                DesignerDrawCrosshair(GetScreenWidth(), GetScreenHeight());
            }
            // Snip mode (screen regions) shows even with the HUD off, like the prompt box
            if (drawUi && designerUi) DesignerDrawRegions(&designer, GetScreenWidth(), GetScreenHeight(), false);
            if (drawUi && hud) {
                DrawText(TextFormat("%d fps%s  |  %s  |  1 volFog:%s  2 fog:%s  3 ssao:%s  4 ssil:%s  5 bloom:%s  6 sun:%s",
                         GetFPS(), reused ? " (cached)" : "", spec.active ? "SPECTATOR" : player.fly ? "FLY" : "WALK",
                         env->volumetricFog.enabled ? "on" : "off", env->fog.mode ? "on" : "off",
                         env->ssao.enabled ? "on" : "off", env->ssil.enabled ? "on" : "off",
                         env->bloom.mode ? "on" : "off", scene.sunOn ? "on" : "off"),
                         12, 10, 16, (Color){ 230, 230, 230, 160 });

                if (spec.active) {
                    DrawText(TextFormat("speed %.1f m/s (wheel)  |  <- -> viewpoints  |  P copy view  |  G drop player  |  TAB exit",
                             spec.speed), 12, 32, 16, (Color){ 230, 230, 230, 120 });
                    DrawText("LMB select  |  SHIFT+LMB add/remove  |  CTRL+LMB group  |  RMB clear  |  C snip regions  |  ENTER request  |  R fly to last result",
                             12, 52, 16, (Color){ 230, 230, 230, 120 });
                    int y = 80;
                    y += DesignerDrawHud(&designer, 12, y);
                    LauncherDrawHud(12, y + 4);
                    if (spec.messageTime > 0.0f && !designer.promptOpen) {
                        float a = fminf(spec.messageTime, 1.0f);
                        int w = MeasureText(spec.message, 24);
                        DrawText(spec.message, (GetScreenWidth() - w) / 2, GetScreenHeight() - 70, 24,
                                 Fade((Color){ 235, 232, 225, 255 }, a));
                    }
                } else if (!shotPath) {
                    LauncherDrawHud(12, 32);    // finished requests also show while walking
                }
            }
            if (drawUi && (mapMessageError || mapMessageTime > 0.0f) && !designer.promptOpen) {
                Color c = mapMessageError ? (Color){ 255, 110, 90, 255 }
                                          : Fade((Color){ 235, 232, 225, 255 }, fminf(mapMessageTime, 1.0f));
                DrawText(mapMessage, 12, GetScreenHeight() - 30, 18, c);
            }
            // The prompt box shows even with the HUD off (Enter opened it, it must be visible)
            if (drawUi && designerUi) DesignerDrawPrompt(&designer, GetScreenWidth(), GetScreenHeight());
            if (drawUi && shotPath) EndTextureMode();
        EndDrawing();
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

        if (shotPath && ++frame == 30) {
            Image img = ReadTarget(shotTarget);
            if (!ExportImage(img, shotPath)) TraceLog(LOG_ERROR, "monolith: failed to save %s", shotPath);
            UnloadImage(img);
            if (pickFlag) DesignerPrintHit(designer.hit);
            if (reuse.enabled) TraceLog(LOG_INFO, "monolith: frame reuse: %d of %d frames composited from the cache", reusedFrames, frame);
            if (submitText) {
                char msg[512];
                float yaw = spec.active ? spec.yaw : player.yaw, pitch = spec.active ? spec.pitch : player.pitch;
                DesignerOpenPrompt(&designer, Unescape(submitText));
                bool ok = Submit(&scene, &designer, camera, yaw, pitch, shotTarget, msg, sizeof(msg));
                printf("submit: %s: %s\n", ok ? "ok" : "failed", msg);
                fflush(stdout);
            }
            break;
        }
    }

    if (bench && benchView == VIEWPOINT_COUNT) {
        BenchCollectProfile(true);  // the last frames' results are still in flight
        WriteBenchReport(benchTimes, benchShadowUpdates, shadowSize, shadowRange, shadowReuse, headless, benchNote, argc, argv);
    }

    R3D_SetProfilerEnabled(false);  // frees its GL queries
    if (hidden) UnloadRenderTexture(shotTarget);
    if (reuse.rt.id) UnloadRenderTexture(reuse.rt);
    if (reuse.enabled) UnloadShader(reuse.composite);
    R3D_UnloadSurfaceShader(scene.concrete.shader);
    R3D_UnloadInstanceBuffer(scene.instances);
    R3D_UnloadMesh(scene.cube);
    for (int i = 0; i < SUN_TILE_COUNT; i++) R3D_UnloadShadowMap(scene.sunShadow[i]);
    R3D_UnloadAmbientMap(ambient);
    R3D_UnloadCubemap(sky);
    R3D_Close();
    CloseWindow();
    return 0;
}

// #endregion
