// monolith -- AI-assisted map designer for brutalist concrete levels (raylib + R3D)
//
// The level is a map package (maps/<name>/: a plain-text <name>.map plus MAP.md, shaders,
// textures; format: the header comment of runtime/monolith_scene.c), loaded, drawn and
// hot-reloaded by the scene runtime (runtime/monolith_scene.h) while the game runs. In
// spectator mode you point at things, select boxes and type a request; the game writes
// requests/<id>/ and opens Claude Code in a new terminal to edit the map (AI-MAP-TOOL.md).
// Engine reference: PROMPT-INSTRUCTIONS.md.
//
// Controls:
//   WASD / mouse   walk & look          SHIFT  walk faster
//   SPACE          jump                 F      toggle fly (noclip)
//   1..6           toggle effects (volumetric fog, fog, SSAO, SSIL, bloom, sun): each key
//                  flips that effect relative to the map's setting; flips survive hot reloads
//   F1             toggle HUD (also hides the crosshair and selection overlay)
//   "(cached)" after the fps: nothing changed, the frame reused the last R3D image
//   F12            screenshot           TAB    toggle spectator camera
//   F9             export the map: package + scene runtime + README into
//                  export/<map>-<YYYY-MM-DD_HHMMSS>/ next to requests/ (export.c, like --export)
//   ESC            quit (closes the prompt box / leaves snip mode instead while open)
//   The HUD's bottom line shows "map reloaded (N boxes)" after an edit to any file of the
//   map package (map, shaders, textures), or the error (file:line: message) in red while
//   the package is broken (the old map stays loaded).
//
// Spectator:
//   WASD / mouse   move & look          E/Q or SPACE        up/down
//   mouse wheel    base speed           SHIFT x4, ALT x0.25
//   LEFT/RIGHT     fly to previous/next viewpoint
//   P              print + copy current view as --pos/--look arguments (--pos = feet)
//   G              move the player here
//
// Map designer (spectator mode, see designer.c / launcher.c):
//   crosshair      the box or model under it is outlined (models: their rotated bounds); the
//                  HUD shows its group and map line (models: file and transform too)
//   LMB            select the object under the crosshair (replaces the selection)
//   SHIFT+LMB      add/remove that object       CTRL+LMB   select its whole group
//   CTRL+SHIFT+LMB add the whole group          RMB        clear the selection
//   ENTER          open the request prompt box. In the box: type, BACKSPACE/DELETE
//                  (CTRL+BACKSPACE: word), LEFT/RIGHT/UP/DOWN, HOME/END (CTRL: whole
//                  text), CTRL+V paste, SHIFT+ENTER new line, ENTER send to Claude Code,
//                  ESC cancel (discards the text). While the box is open every game key
//                  and the camera are frozen and the mouse cursor is free. If sending
//                  fails, the box stays open with the text.
//   C              snip mode: the camera freezes and the cursor is free. Drag = a screen
//                  region (replaces the others), SHIFT+drag = add one, ENTER =
//                  prompt box with the regions (and the box selection made before C)
//                  attached, RMB / ESC / C = leave snip mode and drop the regions. ESC in the
//                  prompt box returns to snip mode with the regions; a successful submit
//                  ends it. The bundle lists what each region shows (designer.c)
//   R              fly back to the camera of the last finished request
//   The HUD lists open requests ("running", "waiting for result.md") and, for 20 s,
//   finished ones with the first line of their result.md (after that a dim line while
//   their terminal is still open). Each has a number [n], kept while it is listed.
//   K              close the terminal window of the last finished request
//   SHIFT+K        close the terminals of all finished requests
//   CTRL+K, n      close request [n] (1-9, 0 = 10), also a running one: then press n again
//                  within 3 s. ESC cancels; the digits don't toggle effects meanwhile.
//                  Closed requests leave the list, their bundles stay (launcher.c, region
//                  "Closing request windows": i3/sway mark + kill, else SIGTERM/SIGKILL)
//   The selection survives map hot reloads: objects are matched by identical geometry (models:
//   world bounds), else by the same map line + index within it + group; the HUD says if any
//   got lost.
//
// Command line:
//   --shot <file>  render 30 frames offscreen (1600x900) in a hidden window (never mapped,
//                  so the window manager doesn't show it), save the image to <file> and exit.
//                  No hot reload or request polling during shots
//   --shot-size <w> <h>   size of the --shot image (default 1600 900); request bundles pass
//                  their window size so the reproduction matches view.png exactly
//   --pos x y z    start position (feet; default: the map's spawn)
//   --look yawDeg pitchDeg   start view angles (default: the spawn's; with --view: override
//                  the viewpoint's)
//   --eye x y z    start position of the eye (= --pos x y-1.62 z); with --view it moves the
//                  spectator there instead (request bundles use it for their --shot command)
//   --off <keys>   start with these effects toggled off, e.g. --off 34 (SSAO + SSIL)
//   --view <n>     start in spectator mode at the map's viewpoint n (1-based)
//   --ao <mode>    screen-space AO / indirect light trade-off (atmosphere.c): full (default),
//                  ssao (SSIL off), lite (SSIL off, SSAO 8 samples), off (SSAO + SSIL off),
//                  baked (SSAO + SSIL off, the baked occlusion volume instead: --ao-volume 0.5 0.6)
//   --ao-volume <voxel> <intensity>   trade-off, off by default: bake a world-space occlusion
//                  volume of the boxes once per map load (voxel size in m, 0.25..4) and darken
//                  the ambient light with it (R3D patch 11; on top of SSAO/SSIL unless --ao baked)
//   --bloom off    bloom pass off, exposure x1.19 instead (same mean brightness, no faint
//                  halo at bright openings; atmosphere.c). Default: on
//   --fx key=value   override one effect setting, repeatable, e.g. --fx ssil.radius=2.5
//                  (keys: ssao./ssil.samples, .radius; ssao.intensity/.power; ssil.gi/.ao;
//                  bloom.levels/.intensity; exposure). Applied after --ao / --bloom
//   --shadow <size> <range>   sun shadow map size (px) and distance (m), overriding the map's
//                  env shadow.size / sun.range (the default map: 8192 100)
//   --shadow-camera   sun shadow from R3D's camera-fitted map, re-rendered only when the
//                  cached map no longer covers the view (the pre-tile path; default: static
//                  world-fixed tiles drawn once per map load, see monolith_scene.c; A/B tests)
//   --shadow-always   camera-fitted map re-rendered every frame (A/B tests and debugging)
//   --fog-cache <voxel> <sub>   trade-off, off by default: bake the sun visibility of the
//                  volumetric fog into a 3D texture once per map load (voxel size in m,
//                  sub^3 samples per voxel, e.g. 0.5 2) and let the fog march fetch it
//                  instead of the shadow map (R3D patch 8); changes the light shafts slightly.
//                  Region: the map's env fogCache.region, else derived from its bounds
//   --render-scale <s>   trade-off, default 1: R3D renders at s (0.25..1) times the window /
//                  shot size and upscales the result (--upscale nearest|linear|bicubic|lanczos,
//                  default bicubic). 0.75 renders 56% of the pixels
//   --window-msaa  4x multisampled window (the default before 2026-10-11): smoother designer
//                  overlay lines for ~0.1 ms GPU per frame; the scene image is the same
//   --no-prepass   no depth pre-pass before the G-buffer pass (R3D patch 6; same image,
//                  for A/B tests: the concrete shader then runs for every overdrawn fragment)
//   --bench        visit every viewpoint, spin the camera, print frame time stats, write a
//                  Markdown report to benchmark/<timestamp>_shadow-<size>-<range>.md and exit
//   --headless     with --bench: hidden window + offscreen target + glFinish per frame,
//                  so nothing appears on screen (compare headless runs only with each other)
//   --bench-frames <n>   timed frames per viewpoint (at least 10, default 600); fewer = quick
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
//   --bench-window <samples>   with --bench --headless: render into an offscreen stand-in for
//                  the visible window (a framebuffer with <samples> samples per pixel: 4 = the
//                  window's FLAG_MSAA_4X_HINT, 0 = single-sample), draw the spectator HUD and
//                  overlay on top and resolve it like the swap does (windowemu.c), to time the
//                  window-only costs without a visible window
//   --no-profile   with --bench: no per-pass GPU timings (R3D profiler, on by default when
//                  R3D is built with R3D_GPU_PROFILER, as build.sh does)
//   --note "text"  free-form note added to the benchmark report
//   --map <path>   load this map package (folder) or .map file instead of maps/monolith/
//                  (relative to the CWD; the default is the repo's maps/ while it exists,
//                  else maps/ next to the executable). A folder loads <folder>/<name>.map,
//                  else its only .map file. The package is reloaded whenever one of its
//                  files changes (format: runtime/monolith_scene.c)
//   --reload-test <path>   with --shot: after 10 frames, load this map (package or file)
//                  through the hot-reload path (as if the map had changed), then render the
//                  remaining frames. The shot must equal a plain --shot --map <path> (proves
//                  that every static cache is rebuilt on reload)
//   --dump-boxes <file>   load the map, write every box as hex floats (%a) to <file> and
//                  exit without opening a window (for checking that edits to the loader
//                  or the map keep the exact box list). Any map that fails to parse (with
//                  or without this flag) prints file:line: message and exits with status 1.
//                  Model files are only checked for existence here (no window, no GPU)
//   --dump-scene <file>   like --dump-boxes, plus model files, model instances (transform and
//                  world bounds as hex floats), model colliders, materials and lights
//                  (format: src/dumpscene.c). Maps with models load them in a hidden window
//   --inspect-model <file>   print what a model file contains and exit, without a window:
//                  bounds, size, origin, mesh/triangle/vertex counts, materials (= the slots
//                  for material.N) and their textures (embedded or external, sizes), up axis
//                  and unit hints, glTF extensions, warnings (unlit or fully metallic
//                  materials, > 50k triangles, textures > 2K, unsupported extensions) and a
//                  placement line (src/inspect.c)
//   --new-map <name>   create the map package maps/<name>/ (<name>.map with a ground slab,
//                  the default material and the default environment written out, a MAP.md
//                  template, empty shaders/ models/ textures/ and ASSETS.md) and exit without
//                  opening a window. Refuses to overwrite an existing folder
//   --export <dir>  write the map (--map or the default) as a folder for a plain raylib + R3D
//                  project and exit without opening a window: the package with every file it
//                  references, monolith_scene.h/.c, viewer.c and a README.md (export.c).
//                  Refuses a non-empty <dir>
//   --import <project dir> <name>   create maps/<name>/ (as --new-map) and open a Claude Code
//                  import session on a bundle requests/<stamp>_import-<name>/ that translates
//                  the raylib + R3D project's scene into it (import.c, AI-MAP-TOOL.md "Import
//                  requests"); no window. Same terminal / flags / workspace as edit requests
//
// Designer debug flags (for testing without a visible window):
//   --pick         with --shot: print what the crosshair (screen centre) hits on the shot
//                  frame: box or model index, group, map line + its text, point, normal,
//                  distance (models: file, position, rotation, scale, origin, bounds)
//   --select <line>  select every box / model made by this map line (repeatable)
//   --region x y w h   attach a screen region (pixels of the shot, top-left origin), as if
//                  dragged out in snip mode (repeatable); with --shot-hud the shot
//                  shows the snip-mode drawing, with --submit the bundle gets the regions
//   --submit "text"  with --shot: after saving the shot, run the real submit path (render
//                  view + view-marked offscreen, LauncherSubmit with the centre pick,
//                  the --select boxes and the --region regions) and print the launcher's message. This writes a real
//                  bundle into requests/ and launches a real terminal + Claude Code: for
//                  tests, set MONOLITH_TERMINAL to a stub script that only logs its arguments
//   --type "text"  open the prompt box with this text (cursor at the end)
//   --shot-hud     with --shot: draw the HUD, crosshair, selection overlay, regions and prompt box
//                  into the shot (to look at the UI; plain shots never contain it)
//   --submit-wait <s>   with --submit: keep polling the request (result.md, terminal process)
//                  for <s> seconds before exiting
//   --close-submitted   with --submit: then close the submitted request like CTRL+K n (twice
//                  if it is still running) and wait up to 10 s for its terminal process to go;
//                  prints "close [n]: ..." and "close: terminal process gone after X s"
//   --classify "text"   print the model the launcher would choose for this prompt (tier,
//                  model, reason; launcher.c "Model choice") and exit without opening a
//                  window. Context: the loaded map (empty? MAP.md?), the --select lines, the
//                  --region count and a crosshair hit. --classify-kind edit|import|new-map
//                  sets the request kind (default edit)
//   In --submit, --type and --classify text, a literal \n becomes a new line.
//
// Environment (launcher.c):
//   MONOLITH_TERMINAL   terminal to open for Claude Code: program name or path + optional
//                       args, e.g. "kitty --single-instance"
//   TERMINAL            used if MONOLITH_TERMINAL is unset; else the first of alacritty,
//                       kitty, foot, wezterm, xterm in PATH
//   MONOLITH_CLAUDE_ARGS   extra Claude Code flags, split on whitespace (default none), after
//                       --allow-dangerously-skip-permissions --dangerously-skip-permissions
//                       --model <name>; a --model in them replaces the chosen model
//   MONOLITH_MODEL      one model for every request (turns the per-request choice off)
//   MONOLITH_MODEL_FAST / _DEFAULT / _HEAVY   model of each tier (default haiku / sonnet /
//                       opus). The tier is chosen per request from the prompt and its
//                       context; a leading @fast / @default / @heavy (or @haiku / @sonnet /
//                       @opus) in the prompt picks it and is stripped from the bundle's prompt
//   MONOLITH_CLAUDE_SAFE=1   drop those two permission flags
//   MONOLITH_WORKSPACE  i3/sway workspace for the terminal (default 10, empty = don't move)
//   MONOLITH_CLOSE_ON_EXIT=1   close the terminals of finished requests when the game quits
//   MONOLITH_CLOSE_METHOD   wm / signal: close terminals only through i3/sway or only by
//                       signals (tests; default both, WM first)
//   __GL_SYNC_TO_VBLANK=0 (NVIDIA) / vblank_mode=0 (Mesa)   driver vsync off, for --bench

#include "common.h"
#include "player.h"
#include "spectator.h"
#include "atmosphere.h"
#include "bench.h"
#include "designer.h"
#include "launcher.h"
#include "newmap.h"
#include "export.h"
#include "import.h"
#include "inspect.h"
#include "dumpscene.h"
#include "windowemu.h"

// Only needed for glFinish() for --headless (depth clamp: MS_InitRenderer). Declared by hand
// rather than via <GL/gl.h> / <windows.h> (which clashes with raylib). On Windows x64 there
// is a single calling convention, so this matches opengl32.dll.
extern void glFinish(void);

#ifndef BENCHMARK_PATH
#   define BENCHMARK_PATH "./benchmark/"
#endif
#ifndef MAPS_PATH
#   define MAPS_PATH "./maps/"
#endif

// Data directories, with a trailing slash. Both builds are meant to be copied to other
// computers as a folder (build/linux/, build/win/), so they look next to the executable.
// On Linux, the repo directories baked in by build.sh win while they exist (i.e. on the
// dev machine), so bench reports land in the repo's benchmark/, and the map package that
// gets hot-reloaded (with its shaders) is the repo's maps/ one.
// Windows paths aren't baked in (wine would see Z:\...).
char benchmarkDir[1024];
static char mapsDir[1024];  // only main() needs it (default map, --new-map)
MS_Scene *scene;

static void InitDataPaths(void)
{
    const char *exeDir = GetApplicationDirectory();
#ifdef _WIN32
    snprintf(benchmarkDir, sizeof(benchmarkDir), "%sbenchmark/", exeDir);
    snprintf(mapsDir, sizeof(mapsDir), "%smaps/", exeDir);
#else
    if (DirectoryExists(BENCHMARK_PATH)) snprintf(benchmarkDir, sizeof(benchmarkDir), "%s", BENCHMARK_PATH);
    else snprintf(benchmarkDir, sizeof(benchmarkDir), "%sbenchmark/", exeDir);
    if (DirectoryExists(MAPS_PATH)) snprintf(mapsDir, sizeof(mapsDir), "%s", MAPS_PATH);
    else snprintf(mapsDir, sizeof(mapsDir), "%smaps/", exeDir);
#endif
}

static bool DumpBoxes(const MS_Scene *s, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    for (int i = 0; i < s->boxCount; i++) {
        BoundingBox b = s->boxes[i];
        fprintf(f, "%a %a %a %a %a %a\n", b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
    }
    return fclose(f) == 0;
}

// Renders the scene into `target` (zero = the window), so the main view and the request
// images (Submit) go through the same code. Returns whether a sun shadow map was
// re-rendered. Extra renders (Submit) go through the same cache check with their own
// target's aspect, so the cached map always matches what R3D last rendered.
static double lastR3DEndTime;   // seconds, set by DrawScene

static bool DrawScene(Camera3D camera, RenderTexture target)
{
    float aspect = target.id ? (float)target.texture.width / target.texture.height
                             : (float)GetRenderWidth() / GetRenderHeight();
    R3D_BeginPro((R3D_View){ .camera = R3D_CameraFromRL(camera), .target = target });
        bool update = MS_PushLights(scene, camera, aspect);
        MS_DrawGeometry(scene);     // (also bakes the --fog-cache volume once per map load)
    double r3dEndStart = GetTime();
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
    const char *aoMode, *bloomMode;
    const char **fx;            // the --fx arguments (argv strings)
    int fxCount;
    bool flip[6];               // keys 1-6: volumetric fog, fog, SSAO, SSIL, bloom, sun
} EffectState;

static void ApplyEnvironment(const EffectState *fx)
{
    MS_ApplyEnvironment(scene);
    R3D_Environment *env = R3D_GetEnvironment();
    ApplyAoMode(env, fx->aoMode);
    ApplyBloomMode(env, fx->bloomMode);
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
    MS_RenderSettings render;   // toggle 6 (sunOn), shadow mode, fog cache, pre-pass
    unsigned int sceneVersion;
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
static void MakeFrameKey(FrameKey *k, Camera3D camera)
{
    memset(k, 0, sizeof(*k));
    memcpy(&k->camera, &camera, sizeof(camera));
    memcpy(&k->env, R3D_GetEnvironment(), sizeof(k->env));
    MS_RenderSettings rs = MS_GetRenderSettings(scene);
    memcpy(&k->render, &rs, sizeof(rs));
    k->sceneVersion = scene->version;
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
static bool Submit(Designer *d, Camera3D cam, float yaw, float pitch, RenderTexture rt,
                   char *msg, int msgSize)
{
    // Window mode: an offscreen target at R3D's resolution (the window size), like --shot
    DesignRequest req;
    if (!DesignerBuildRequest(d, cam, yaw, pitch, rt.id ? rt.texture.width : GetScreenWidth(),
                              rt.id ? rt.texture.height : GetScreenHeight(), &req)) {
        snprintf(msg, msgSize, "out of memory: request not sent");
        DesignerSubmitDone(d, false, msg);
        return false;
    }
    bool own = rt.id == 0;
    if (own) rt = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
    DrawScene(cam, rt);
    Image view = ReadTarget(rt);
    BeginTextureMode(rt);
        DesignerDrawOverlay(d, cam, false);
        DesignerDrawCrosshair(rt.texture.width, rt.texture.height);
        DesignerDrawRegions(d, rt.texture.width, rt.texture.height, true);
    EndTextureMode();
    Image marked = ReadTarget(rt);

    bool ok = LauncherSubmit(&req, view, marked, msg, msgSize);
    TraceLog(ok ? LOG_INFO : LOG_WARNING, "monolith: submit: %s", msg);
    DesignerFreeRequest(&req);
    UnloadImage(view);
    UnloadImage(marked);
    if (own) UnloadRenderTexture(rt);
    DesignerSubmitDone(d, ok, msg);
    return ok;
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
    int *selectLines;           // --select <line>
    Rectangle *regions;         // --region x y w h
    double *benchTimes;         // [view * benchFrames + frame]
    int *benchShadowUpdates;    // per view
} mainHeap;

static void FreeMainHeap(void)
{
    free(mainHeap.fx);
    free(mainHeap.selectLines);
    free(mainHeap.regions);
    free(mainHeap.benchTimes);
    free(mainHeap.benchShadowUpdates);
    memset(&mainHeap, 0, sizeof(mainHeap));
}

static int Run(int argc, char **argv);

int main(int argc, char **argv)
{
    int status = Run(argc, argv);
    FreeMainHeap();
    LauncherShutdown();
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
    int shadowSize = 0;         // --shadow; 0 = the map's (env shadow.size / sun.range)
    float shadowRange = 0.0f;
    bool bench = false;
    bool headless = false;      // --bench without a visible window
    int benchWindowSamples = -1;    // --bench-window <samples>; -1 = off
    bool windowMsaa = false;    // --window-msaa
    bool depthPrepass = true;   // --no-prepass
    float renderScale = 1.0f;   // --render-scale
    float aoVolumeVoxel = 0.0f, aoVolumeIntensity = 1.0f;  // --ao-volume (--ao baked: 0.5 1)
    R3D_UpscaleMode upscale = R3D_UPSCALE_BICUBIC;  // --upscale
    MS_ShadowMode shadowMode = MS_SHADOW_TILES;     // --shadow-camera / --shadow-always
    const char *newMapName = NULL;
    const char *exportDir = NULL;               // --export
    const char *importDir = NULL, *importName = NULL;   // --import
    float fogCacheVoxel = 0.0f;
    int fogCacheSub = 2;
    const char *benchNote = NULL;
    const char *mapPath = NULL;
    const char *dumpPath = NULL;
    const char *dumpScenePath = NULL;   // --dump-scene
    const char *reloadTestPath = NULL;
    bool eyeSet = false, lookSet = false;   // --eye / --look given (they override --view)
    Vector3 eye = { 0 };
    bool pickFlag = false, shotHud = false;
    int frameReuseFlag = -1;    // --no-frame-reuse 0, --frame-reuse 1, default -1 (see below)
    const char *submitText = NULL, *typeText = NULL;
    float submitWait = -1.0f;       // --submit-wait: keep polling the request this long
    bool closeSubmitted = false;    // --close-submitted
    const char *classifyText = NULL, *classifyKind = "edit";
    const char *aoMode = "full", *bloomMode = "on";
    // Repeatable arguments: at most argc of each
    mainHeap.fx = malloc(sizeof(const char *) * argc);
    mainHeap.selectLines = malloc(sizeof(int) * argc);
    mainHeap.regions = malloc(sizeof(Rectangle) * argc);
    if (!mainHeap.fx || !mainHeap.selectLines || !mainHeap.regions) {
        fprintf(stderr, "monolith: out of memory\n");
        return 1;
    }
    const char **fxSettings = mainHeap.fx;
    int fxCount = 0;
    int *selectLines = mainHeap.selectLines, selectLineCount = 0;
    Rectangle *regionArgs = mainHeap.regions;
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
            fxSettings[fxCount++] = argv[i + 1];
            i++;
        }
        else if (strcmp(argv[i], "--bench") == 0) bench = true;
        else if (strcmp(argv[i], "--bench-still") == 0) bench = benchStill = true;
        else if (strcmp(argv[i], "--no-profile") == 0) benchProfile = false;
        else if (strcmp(argv[i], "--headless") == 0) headless = true;
        else if (strcmp(argv[i], "--window-msaa") == 0) windowMsaa = true;
        else if (strcmp(argv[i], "--bench-window") == 0 && i + 1 < argc)
            benchWindowSamples = (int)Clamp((float)atoi(argv[++i]), 0.0f, 16.0f);
        else if (strcmp(argv[i], "--shadow-always") == 0) shadowMode = MS_SHADOW_ALWAYS;
        else if (strcmp(argv[i], "--shadow-camera") == 0) { if (shadowMode != MS_SHADOW_ALWAYS) shadowMode = MS_SHADOW_CAMERA; }
        else if (strcmp(argv[i], "--new-map") == 0 && i + 1 < argc) newMapName = argv[++i];
        else if (strcmp(argv[i], "--export") == 0 && i + 1 < argc) exportDir = argv[++i];
        else if (strcmp(argv[i], "--import") == 0 && i + 2 < argc) { importDir = argv[i + 1]; importName = argv[i + 2]; i += 2; }
        else if (strcmp(argv[i], "--fog-cache") == 0 && i + 2 < argc) {
            // Clamped: 0.25 m is already 228 MB over the default region; 0 or less turns it off
            fogCacheVoxel = (float)atof(argv[i + 1]);
            fogCacheVoxel = fogCacheVoxel > 0.0f ? Clamp(fogCacheVoxel, 0.25f, 8.0f) : 0.0f;
            fogCacheSub = (int)Clamp((float)atoi(argv[i + 2]), 1.0f, 4.0f);
            i += 2;
        }
        else if (strcmp(argv[i], "--no-prepass") == 0) depthPrepass = false;
        else if (strcmp(argv[i], "--ao-volume") == 0 && i + 2 < argc) {
            aoVolumeVoxel = (float)atof(argv[i + 1]);
            aoVolumeVoxel = aoVolumeVoxel > 0.0f ? Clamp(aoVolumeVoxel, 0.25f, 4.0f) : 0.0f;
            aoVolumeIntensity = Clamp((float)atof(argv[i + 2]), 0.0f, 4.0f);
            i += 2;
        }
        else if (strcmp(argv[i], "--render-scale") == 0 && i + 1 < argc) renderScale = Clamp((float)atof(argv[++i]), 0.25f, 1.0f);
        else if (strcmp(argv[i], "--upscale") == 0 && i + 1 < argc) {
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
        else if (strcmp(argv[i], "--bench-frames") == 0 && i + 1 < argc)
            benchFrames = (int)Clamp((float)atoi(argv[++i]), 10.0f, (float)BENCH_FRAMES_LIMIT);
        else if (strcmp(argv[i], "--note") == 0 && i + 1 < argc) benchNote = argv[++i];
        else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) mapPath = argv[++i];
        else if (strcmp(argv[i], "--dump-boxes") == 0 && i + 1 < argc) dumpPath = argv[++i];
        else if (strcmp(argv[i], "--dump-scene") == 0 && i + 1 < argc) dumpScenePath = argv[++i];
        else if (strcmp(argv[i], "--inspect-model") == 0 && i + 1 < argc) return InspectModel(argv[++i]);
        else if (strcmp(argv[i], "--reload-test") == 0 && i + 1 < argc) reloadTestPath = argv[++i];
        else if (strcmp(argv[i], "--pick") == 0) pickFlag = true;
        else if (strcmp(argv[i], "--shot-hud") == 0) shotHud = true;
        else if (strcmp(argv[i], "--no-frame-reuse") == 0) frameReuseFlag = 0;
        else if (strcmp(argv[i], "--frame-reuse") == 0) frameReuseFlag = 1;
        else if (strcmp(argv[i], "--submit") == 0 && i + 1 < argc) submitText = argv[++i];
        else if (strcmp(argv[i], "--submit-wait") == 0 && i + 1 < argc) submitWait = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--close-submitted") == 0) closeSubmitted = true;
        else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) typeText = argv[++i];
        else if (strcmp(argv[i], "--classify") == 0 && i + 1 < argc) classifyText = argv[++i];
        else if (strcmp(argv[i], "--classify-kind") == 0 && i + 1 < argc) classifyKind = argv[++i];
        else if (strcmp(argv[i], "--select") == 0 && i + 1 < argc) {
            selectLines[selectLineCount++] = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--region") == 0 && i + 4 < argc) {
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
            posSet = true;
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

    if (newMapName) return NewMap(mapsDir, newMapName) ? 0 : 1;
    if (importDir) return ImportProject(mapsDir, importDir, importName) ? 0 : 1;

    // --- Map: parse it before opening a window, so a broken map just prints the error, and
    // the sun's shadow map size (an R3D init hint) is known before R3D_Init
    char defaultMap[1100];
    snprintf(defaultMap, sizeof(defaultMap), "%smonolith", mapsDir);
    if (!mapPath) mapPath = defaultMap;
    MS_LoadOptions loadOpt = { .noGpu = true, .shadowSizeOverride = shadowSize, .shadowRangeOverride = shadowRange };
    MS_Scene *peek;     // parsed only (no GPU); gives the R3D init hints once a GL context exists
    {
        char err[512];
        peek = MS_Load(mapPath, &loadOpt, err, sizeof(err));
        if (!peek) {
            fprintf(stderr, "monolith: cannot load the map\n%s\n", err);
            return 1;
        }
        TraceLog(LOG_INFO, "monolith: %s: %d boxes, %d groups (format %d)", peek->path, peek->boxCount,
                 peek->groupCount, peek->formatVersion);
        if (dumpPath) {
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
                for (int k = 0; k < selectLineCount; k++) if (peek->boxRefs[b].line == selectLines[k]) { selected++; break; }
            for (int m = 0; m < peek->modelCount; m++)
                for (int k = 0; k < selectLineCount; k++) if (peek->models[m].ref.line == selectLines[k]) { selected++; break; }
            char *text = Unescape(classifyText);
            if (!text) { MS_Unload(peek); return 1; }
            ModelQuery q = { .prompt = text, .kind = classifyKind, .selected = selected, .regions = regionArgCount,
                             .hit = true, .emptyMap = peek->boxCount + peek->modelCount <= 2, .noBrief = !peek->brief[0] };
            ModelChoice c;
            LauncherChooseModel(&q, &c);
            printf("tier: %s\nmodel: %s%s\nreason: %s\nsource: %s\nprompt: %s\n", ModelTierName(c.tier), c.name,
                   c.addFlag ? "" : " (from MONOLITH_CLAUDE_ARGS, no --model added)", c.reason, c.source, text + c.promptSkip);
            free(text);
            MS_Unload(peek);
            return 0;
        }
        if (exportDir) {
            char msg[1400];
            bool ok = ExportScene(peek, exportDir, msg, sizeof(msg));
            fprintf(ok ? stdout : stderr, "monolith: %s\n", msg);
            MS_Unload(peek);
            return ok ? 0 : 1;
        }
        if (dumpScenePath && peek->modelCount == 0) {
            // Without models nothing needs the GPU: no window at all
            bool ok = DumpScene(peek, dumpScenePath);
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
        shadowRange = peek->sun.light.range;
    }

    // No vsync, uncapped. The window is single-sampled unless --window-msaa: R3D anti-aliases
    // its own image (FXAA) and writes the same colour to every sample, so 4x MSAA on the
    // window only smoothed the designer's overlay lines, for ~0.1 ms GPU per frame at 1080p
    // (R3D's last pass writing 4 samples + the resolve; measured with --bench-window 4 vs 0).
    // Shots and request images were always single-sampled.
    unsigned int windowFlags = windowMsaa ? FLAG_MSAA_4X_HINT : 0;
    // --shot: hidden window, never mapped, so i3 & co. never show or focus it
    // --bench --headless too: then nothing ever appears on screen
    // --dump-scene of a map with models: model files load through R3D, which needs GL
    bool hidden = shotPath || (bench && headless) || dumpScenePath;
    windowFlags |= hidden ? FLAG_WINDOW_HIDDEN : FLAG_WINDOW_RESIZABLE;
    SetConfigFlags(windowFlags);
    // Distinct titles let the window manager route automated runs elsewhere, e.g. i3:
    //   for_window [title="^monolith-(bench|shot)$"] move container to workspace number 10
    // Headless benches use the size the tiled bench window gets on a 1080p screen, so
    // their numbers line up with the visible runs.
    bool headlessBench = bench && headless;
    InitWindow(headlessBench ? 1920 : shotPath ? shotW : 1600, headlessBench ? 1006 : shotPath ? shotH : 900,
               bench ? "monolith-bench" : (shotPath || dumpScenePath) ? "monolith-shot" : "monolith");
    SetTargetFPS(0);
    // Esc must close the prompt box without quitting, so quitting on Esc is done by hand
    // in the main loop (the window's close button still ends WindowShouldClose())
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
        char err[512];
        loadOpt.noGpu = false;
        scene = MS_Load(mapPath, &loadOpt, err, sizeof(err));
        if (!scene) {
            fprintf(stderr, "monolith: cannot load the map\n%s\n", err);
            R3D_Close();
            CloseWindow();
            return 1;
        }
        if (dumpScenePath) {
            bool ok = DumpScene(scene, dumpScenePath);
            fprintf(ok ? stdout : stderr, ok ? "monolith: wrote the scene to %s\n" : "monolith: cannot write %s\n", dumpScenePath);
            MS_Unload(scene);
            R3D_Close();
            CloseWindow();
            return ok ? 0 : 1;
        }
    }
    MS_RenderSettings renderSettings = MS_GetRenderSettings(scene);
    renderSettings.shadowMode = shadowMode;
    renderSettings.fogCacheVoxel = fogCacheVoxel;
    renderSettings.fogCacheSub = fogCacheSub;
    renderSettings.depthPrepass = depthPrepass;
    renderSettings.renderScale = renderScale;
    // --ao baked: the occlusion volume replaces SSAO + SSIL (an explicit --ao-volume wins)
    // (intensity 0.6: matched the mean brightness of full SSAO + SSIL in views 1-3 within 1.3/255)
    if (strcmp(aoMode, "baked") == 0 && aoVolumeVoxel == 0.0f) { aoVolumeVoxel = 0.5f; aoVolumeIntensity = 0.6f; }
    renderSettings.aoVolumeVoxel = aoVolumeVoxel;
    renderSettings.aoVolumeIntensity = aoVolumeIntensity;
    renderSettings.upscale = upscale;
    MS_SetRenderSettings(scene, renderSettings);
    MS_ApplyRenderResolution(scene, GetScreenWidth(), GetScreenHeight());

    // --- Environment: the map's, then --ao / --bloom / --fx, then the 1-6 toggles
    EffectState effects = { .aoMode = aoMode, .bloomMode = bloomMode, .fx = fxSettings, .fxCount = fxCount };
    ApplyEnvironment(&effects);

    bool hud = true;

    // Map hot reload message ("map reloaded (N boxes)" or the error), shown even with the
    // HUD off. Errors stay until the next successful load.
    char mapMessage[512] = "";
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
    if (bench) {
        benchTimes = mainHeap.benchTimes = calloc((size_t)scene->viewpointCount * benchFrames, sizeof(double));
        benchShadowUpdates = mainHeap.benchShadowUpdates = calloc(scene->viewpointCount, sizeof(int));
        if (!benchTimes || !benchShadowUpdates || !BenchInit(scene->viewpointCount)) {
            fprintf(stderr, "monolith: out of memory (bench tables)\n");
            bench = false;
        }
    }
    if (bench) {
        spec.active = true;
        SpectatorJumpTo(&spec, 0);
        hud = emulateWindow;
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
    float closePickTime = 0.0f; // CTRL+K: seconds left to press a request's number

    while (!WindowShouldClose())
    {
        float dt = fminf(GetFrameTime(), 1.0f / 20.0f);

        if (IsWindowResized()) MS_ApplyRenderResolution(scene, GetScreenWidth(), GetScreenHeight());

        // Esc quits, unless the prompt box is open or snip mode is on (the designer handles it)
        if (IsKeyPressed(KEY_ESCAPE) && !designer.promptOpen && !designer.snipping && closePickTime <= 0.0f) break;

        // While the prompt box is open (and a couple of frames after it closed) every
        // game key and the camera are frozen: the keys are text now
        bool typing = DesignerCapturesInput(&designer);

        // Closing request terminals (launcher.c): K = the last finished request, SHIFT+K =
        // all finished ones, CTRL+K then 1-9 / 0 (= 10) = request [n] of the HUD list, also
        // a running one (then its number twice). While CTRL+K waits for the number, the
        // digits don't toggle effects and ESC cancels instead of quitting.
        bool closeKeys = spec.active && !typing && !shotPath && !bench;
        bool picking = closeKeys && closePickTime > 0.0f;   // digits belong to the close
        closePickTime = picking ? closePickTime - GetFrameTime() : 0.0f;
        if (closeKeys) {
            char msg[256] = "";
            bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            int number = 0;
            for (int k = 0; k < 10; k++)
                if (IsKeyPressed(KEY_ZERO + k) || IsKeyPressed(KEY_KP_0 + k)) number = k ? k : 10;
            if (IsKeyPressed(KEY_K)) {
                closePickTime = 0.0f;
                if (ctrl) {
                    closePickTime = 3.0f;
                    snprintf(msg, sizeof(msg), "close which request? press its number (ESC cancels)");
                } else if (shift) LauncherCloseAllDone(msg, sizeof(msg));
                else LauncherCloseLatestDone(msg, sizeof(msg));
            } else if (picking && IsKeyPressed(KEY_ESCAPE)) {
                closePickTime = 0.0f;
                snprintf(msg, sizeof(msg), "nothing closed");
            } else if (picking && number) {
                // a running request asks for its number again: keep waiting for it
                closePickTime = LauncherCloseNumber(number, msg, sizeof(msg)) == 0 ? 3.0f : 0.0f;
            }
            if (msg[0]) SpectatorMessage(&spec, msg);
        }

        // Effect toggles, to see what each one contributes (1 volumetric fog, 2 fog, 3 SSAO,
        // 4 SSIL, 5 bloom, 6 sun; --off applies them on the first frame)
        bool first = !offApplied, toggled = false;
        for (int k = 0; k < 6; k++) {
            if ((!typing && !picking && IsKeyPressed(KEY_ONE + k)) || (first && strchr(offKeys, '1' + k))) {
                effects.flip[k] = !effects.flip[k];
                toggled = true;
            }
        }
        if (toggled) ApplyEnvironment(&effects);
        offApplied = true;
        R3D_Environment *env = R3D_GetEnvironment();
        if (!typing && IsKeyPressed(KEY_F1))    hud = !hud;
        if (!typing && IsKeyPressed(KEY_F12))   TakeScreenshot("screenshot.png");
        // (not while the package on disk is broken: the export copies the files on disk)
        if (!typing && !shotPath && !bench && !mapMessageError && IsKeyPressed(KEY_F9)) {
            // Export the current map (export.c); the result shows in the map message line
            char dir[1400];
            ExportDefaultDir(scene, dir, sizeof(dir));
            // (not shown as a map error: those stay until the next successful reload)
            bool exported = ExportScene(scene, dir, mapMessage, sizeof(mapMessage));
            mapMessageError = false;
            mapMessageTime = 6.0f;
            TraceLog(exported ? LOG_INFO : LOG_WARNING, "monolith: %s", mapMessage);
        }

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
            MS_ReloadResult r = MS_UNCHANGED;
            if (!shotPath) r = MS_Poll(scene, GetFrameTime());
            else if (frame == 10) {
                // --reload-test: the same reload handling as a changed map package
                char err[512];
                r = MS_Reload(scene, reloadTestPath, err, sizeof(err));
                if (r == MS_RELOAD_FAILED) TraceLog(LOG_WARNING, "monolith: --reload-test: %s", err);
            }
            if (r == MS_RELOADED) {
                // The runtime swapped in the new GPU resources (new before old: R3D caches
                // instance buffer names) and its caches key on scene->version. The map's
                // environment applies again, then the overrides and the current toggles.
                ApplyEnvironment(&effects);
                if (scene->modelCount > 0)
                    snprintf(mapMessage, sizeof(mapMessage), "map reloaded (%d boxes, %d models)", scene->boxCount, scene->modelCount);
                else snprintf(mapMessage, sizeof(mapMessage), "map reloaded (%d boxes)", scene->boxCount);
                mapMessageTime = 3.0f;
                mapMessageError = false;
                TraceLog(LOG_INFO, "monolith: %s", mapMessage);
            } else if (r == MS_RELOAD_FAILED) {
                snprintf(mapMessage, sizeof(mapMessage), "%s", MS_Error(scene));
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
            if (benchFrame >= 0 && benchFrame < benchFrames) benchTimes[(size_t)benchView * benchFrames + benchFrame] = now - benchLast;
            benchLast = now;
            if (++benchFrame == benchFrames) {
                if (++benchView == scene->viewpointCount) break;
                SpectatorJumpTo(&spec, benchView);
                benchFrame = -BENCH_WARMUP;
            }
            float baseYaw, basePitch;
            LookAngles(scene->viewpoints[benchView].eye, scene->viewpoints[benchView].target, &baseYaw, &basePitch);
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
                Submit(&designer, SpectatorCamera(&spec), spec.yaw, spec.pitch, (RenderTexture){ 0 }, msg, sizeof(msg));
            }
        }
        Camera3D camera = spec.active ? SpectatorCamera(&spec) : PlayerCamera(&player);
        if (spec.active || shotPath) DesignerPickCamera(&designer, camera);

        // Frame reuse (see its region): same key as the last frame -> composite the cache
        bool reused = false;
        bool unchanged = false;
        if (reuse.enabled) {
            FrameKey key;
            MakeFrameKey(&key, camera);
            unchanged = reuse.keyValid && memcmp(&key, &reuse.key, sizeof(key)) == 0;
            reuse.key = key;
            reuse.keyValid = true;
            if (!unchanged) reuse.cacheValid = false;
        }

        BeginDrawing();
            ClearBackground(BLACK);
            if (emulateWindow) WindowEmuClear(&windowEmu);

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
                    shadowUpdated = DrawScene(camera, reuse.rt);
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
                shadowUpdated = DrawScene(camera, shotTarget);
            }
            if (bench && shadowUpdated && benchFrame >= 0 && benchFrame < benchFrames) benchShadowUpdates[benchView]++;

            // HUD and overlays: on the window, or with --shot-hud into the shot
            bool drawUi = !shotPath || shotHud;
            bool uiToTarget = (drawUi && shotPath) || emulateWindow;
            if (uiToTarget) BeginTextureMode(shotTarget);
            // (--shot-hud shows the designer UI in walk mode too: shots of arbitrary --pos views)
            bool designerUi = (spec.active || shotPath) && (!bench || emulateWindow);
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
                         env->bloom.mode ? "on" : "off", MS_GetRenderSettings(scene).sunOn ? "on" : "off"),
                         12, 10, 16, (Color){ 230, 230, 230, 160 });

                if (spec.active) {
                    DrawText(TextFormat("speed %.1f m/s (wheel)  |  <- -> viewpoints  |  P copy view  |  G drop player  |  TAB exit  |  K / SHIFT+K / CTRL+K n close terminals",
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

        if (shotPath && ++frame == 30) {
            Image img = ReadTarget(shotTarget);
            if (!ExportImage(img, shotPath)) TraceLog(LOG_ERROR, "monolith: failed to save %s", shotPath);
            UnloadImage(img);
            if (pickFlag) DesignerPrintHit(designer.hit);
            if (reuse.enabled) TraceLog(LOG_INFO, "monolith: frame reuse: %d of %d frames composited from the cache", reusedFrames, frame);
            if (submitText) {
                char msg[512];
                float yaw = spec.active ? spec.yaw : player.yaw, pitch = spec.active ? spec.pitch : player.pitch;
                char *text = Unescape(submitText);
                if (text) DesignerOpenPrompt(&designer, text);
                free(text);
                bool ok = text && Submit(&designer, camera, yaw, pitch, shotTarget, msg, sizeof(msg));
                if (!text) snprintf(msg, sizeof(msg), "out of memory");
                printf("submit: %s: %s\n", ok ? "ok" : "failed", msg);
                fflush(stdout);
                // --submit-wait / --close-submitted: poll the request (result.md, the
                // terminal process) for a while, then close it the way CTRL+K n does
                if (ok && (submitWait > 0.0f || closeSubmitted)) {
                    for (double t0 = GetTime(); GetTime() - t0 < submitWait; ) { WaitTime(0.1); LauncherUpdate(0.1f); }
                    if (closeSubmitted) {
                        int n = LauncherNewestNumber();
                        for (int tries = 0; tries < 2; tries++) {   // a running request asks twice
                            int r = LauncherCloseNumber(n, msg, sizeof(msg));
                            printf("close [%d]: %s\n", n, msg);
                            if (r != 0) break;
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

    if (bench && benchView == scene->viewpointCount) {
        BenchCollectProfile(true);  // the last frames' results are still in flight
        WriteBenchReport(benchTimes, benchShadowUpdates, shadowSize, shadowRange, shadowMode != MS_SHADOW_ALWAYS, headless, benchNote, argc, argv);
    }

    LauncherCloseOnExit();          // MONOLITH_CLOSE_ON_EXIT=1: close finished requests' terminals
    R3D_SetProfilerEnabled(false);  // frees its GL queries
    BenchFree();
    DesignerFree(&designer);
    if (emulateWindow) WindowEmuUnload(&windowEmu);
    else if (hidden) UnloadRenderTexture(shotTarget);
    if (reuse.rt.id) UnloadRenderTexture(reuse.rt);
    if (reuse.enabled) UnloadShader(reuse.composite);
    MS_Unload(scene);
    R3D_Close();
    CloseWindow();
    return 0;
}

// #endregion
