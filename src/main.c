// monolith -- AI-assisted map designer for 3D levels of any style (raylib + R3D)
//
// The level is a map package (maps/<name>/: a plain-text <name>.map of boxes, models,
// materials, lights and environment, plus its design brief MAP.md, shaders, models, textures
// and ASSETS.md; format: the header comment of runtime/monolith_scene.c), loaded, drawn and
// hot-reloaded by the scene runtime (runtime/monolith_scene.h) while the game runs. The
// default map is maps/monolith/. In spectator mode you point at things, select boxes and
// models or drag out screen regions, and type a request; the game writes requests/<id>/ and
// queues it for the request coordinator, one Claude Code session in its own terminal that the
// game starts when none runs (.claude/skills/coordinator) and that hands each request to a subagent
// (.claude/skills/map-request). Engine reference: docs/architecture.md.
//
// Controls:
//   WASD / mouse   walk & look          SHIFT  walk faster
//   SPACE          jump                 F      toggle fly (noclip)
//   1..6           toggle effects (volumetric fog, fog, SSAO, SSIL, bloom, sun): each key
//                  flips that effect relative to the map's setting; flips survive hot reloads
//   F1             toggle HUD (also hides the crosshair and selection overlay)
//   F2             graphics settings panel (settingspanel.c): preset low/medium/high/ultra,
//                  then Display, Shadows, Ambient occlusion, Fog, Post, Performance rows.
//                  UP/DOWN select, LEFT/RIGHT (or the mouse: arrows, wheel) change, changes
//                  apply at once (a shadow map size restarts the renderer, ~1 s), ESC / F2
//                  close; saved to the settings file on close. Camera and game keys are frozen
//                  and the cursor is free while it is open
//   "(cached)" after the fps: nothing changed, the frame reused the last R3D image
//   F12            screenshot           TAB    toggle spectator camera
//   F9             export the open map as a self-contained folder (package, scene runtime,
//                  R3D, build.sh, README) into export/<map>-<YYYY-MM-DD_HHMMSS>/ next to
//                  requests/ (export.c, like --export)
//   ESC            closes the prompt box / leaves snip mode / closes the panel (no quit: close the window)
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
//                  prompt box with the regions (and the object selection made before C)
//                  attached, RMB / ESC / C = leave snip mode and drop the regions. ESC in the
//                  prompt box returns to snip mode with the regions; a successful submit
//                  ends it. The bundle lists what each region shows (designer.c)
//   R              fly back to the camera of the last finished request
//   The HUD lists the coordinator ("running", "starting", "not running") and the open
//   requests ("queued", "running", "[?]" = a question waits in the coordinator terminal)
//   and, for 20 s, finished ones ([done] / [failed] / [cancelled]) with the first line of
//   their result.md. Each has a number [n], kept while it is listed.
//   K              take the last finished request off the list
//   SHIFT+K        take all finished requests off the list
//   CTRL+K, n      request [n] (1-9, 0 = 10): finished = off the list; queued = cancel it;
//                  running = press n again within 3 s to cancel it (the coordinator stops
//                  its subagent and lists what had changed). ESC cancels the pick; the
//                  digits don't toggle effects meanwhile. Bundles always stay
//   CTRL+SHIFT+K   close the coordinator's terminal (press again within 3 s while requests
//                  are open; the next submit starts a new coordinator, which takes them up)
//   MONOLITH_LAUNCH=per-request (one terminal per request): K / SHIFT+K close the terminals
//                  of the last / all finished requests, CTRL+K n closes request [n]'s
//                  (running: press n twice); launcher.c, region "Closing request windows":
//                  i3/sway mark + kill, else SIGTERM/SIGKILL
//   The selection survives map hot reloads: objects are matched by identical geometry (models:
//   world bounds), else by the same map line + index within it + group; the HUD says if any
//   got lost.
//
// Command line (an unknown flag, a missing or unparsable value, a value that is itself a
// --flag, or a non-finite / out-of-range number prints a message and exits 1 before any
// window opens):
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
//   --bloom off    bloom pass off, exposure x the map's env bloom.offExposure instead
//                  (default 1.19: same mean brightness, no faint halo at bright openings;
//                  atmosphere.c). Default: on
//   --quality <low|medium|high|ultra>   graphics quality preset for this run (MS_Quality in
//                  runtime/monolith_scene.h; ultra = the map exactly, the default). The flags
//                  --ao, --ao-volume, --bloom, --shadow, --shadow-camera/-always, --fog-cache,
//                  --render-scale, --upscale, --window-msaa, --no-prepass and --(no-)frame-reuse
//                  are graphics settings too: given, each overrides that one setting for this run
//                  (after --quality). None of them is saved
//   --settings <file>   graphics settings file to read (and, from the panel, write) instead of
//                  ~/.config/monolith/settings.ini ($XDG_CONFIG_HOME; Windows:
//                  %APPDATA%\monolith\settings.ini; else next to the executable). Only the
//                  interactive game reads the default file: --shot, --bench, --dump-*, --export
//                  and --classify never read settings unless --settings names a file, so their
//                  images and numbers stay deterministic
//   --no-settings  neither read nor save a settings file
//   --settings-set key=value   change one setting as if in the panel (repeatable, after every
//                  other flag): applies to this run and is saved to the settings file. Keys = the
//                  file's (settings.c), e.g. preset=medium, render_scale=0.75, ao=baked
//   --settings-change key=value   with --shot: after 10 frames, apply this change through the
//                  panel's live path (repeatable; shadow_size restarts the renderer). The shot
//                  must equal a run started with --settings-set key=value
//   --settings-panel   with --shot --shot-hud: draw the open settings panel into the shot
//   --fx key=value   override one effect setting, repeatable, e.g. --fx ssil.radius=2.5
//                  (keys: ssao./ssil.samples, .radius; ssao.intensity/.power; ssil.gi/.ao;
//                  bloom.levels/.intensity; exposure; vfog.step/.length). Applied after the
//                  map's environment and the quality settings (--quality, --ao, --bloom, ...).
//                  Values must lie in the range of the matching env key (e.g. bloom.levels 0..1)
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
//                  for A/B tests: a material's surface shader then runs for every overdrawn
//                  fragment)
//   --bench        visit every viewpoint of the map, spin the camera, print frame time stats, write a
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
//   --dump-scene-diff <file>   the same without object indices and map lines, references
//                  by name (group, material, model file): diff two of these to see only the
//                  objects an edit changed, even after inserted lines (src/dumpscene.c)
//   --inspect-model <file>   print what a model file contains and exit, without a window:
//                  bounds, size, origin, mesh/triangle/vertex counts, materials (= the slots
//                  for material.N) and their textures (embedded or external, sizes), up axis
//                  and unit hints, glTF extensions, warnings (unlit or fully metallic
//                  materials, > 50k triangles, textures > 2K, unsupported extensions, no
//                  meshes) and a placement line (src/inspect.c). A model whose glTF / OBJ / MTL
//                  references leave its folder is refused (exit 1) before anything is read
//   --new-map <name|path>   create the map package maps/<name>/ (<name>.map with a ground
//                  slab, the default material and the default environment written out, a
//                  MAP.md template, empty shaders/ models/ textures/ and ASSETS.md) and exit
//                  without opening a window. A path (anything with a '/') creates the package
//                  in that folder instead (its parent must exist; the name is its last
//                  component), e.g. a scratch draft. Refuses to overwrite an existing folder
//   --export <dir>  write the map (--map or the default) as a self-contained folder that
//                  builds and runs on its own, and exit without opening a window: the package
//                  with every file it references, the scene runtime without its editor parts,
//                  viewer.c, build.sh, the bundled R3D (+ raylib, assimp), licences and a
//                  README.md (export.c). Refuses a non-empty <dir>; writes nothing when a
//                  referenced file is missing. Needs the repo (not a copied build folder)
//   --import <project dir> <name|path>   create maps/<name>/ or the package folder a path
//                  names (as --new-map) and queue a Claude Code import request (a bundle
//                  requests/<stamp>_import-<name>/, like a submit) that translates
//                  the raylib + R3D project's scene into it (import.c, the import-request
//                  skill); no window. Same queue / coordinator as edit requests
//
// Designer debug flags (for testing without a visible window):
//   --pick         with --shot: print what the crosshair (screen centre) hits on the shot
//                  frame: box or model index, group, map line + its text, point, normal,
//                  distance (models: file, position, rotation, scale, origin, bounds)
//   --pick-at x y  with --shot: the same for the ray through pixel (x, y) of the shot
//                  (top-left origin, as --region and view.png), e.g. the edges of a gap;
//                  repeatable, each printed after a "pick-at x y" line
//   --select <line>  select every box / model made by this map line (repeatable)
//   --region x y w h   attach a screen region (pixels of the shot, top-left origin), as if
//                  dragged out in snip mode (repeatable); with --shot-hud the shot
//                  shows the snip-mode drawing, with --submit the bundle gets the regions
//   --submit "text"  with --shot: after saving the shot, run the real submit path (render
//                  view + view-marked offscreen, LauncherSubmit with the centre pick, the
//                  --select objects and the --region regions) and print the launcher's
//                  message. This writes a real bundle into requests/ and queues it (and may
//                  start a real coordinator: terminal + Claude Code): for tests, set
//                  MONOLITH_REQUESTS_DIR to a scratch folder and MONOLITH_TERMINAL to a stub
//                  script that only logs its arguments (tools/stub-terminal.sh;
//                  tools/test-coordinator.sh runs the queue end to end with a stub coordinator)
//   --type "text"  open the prompt box with this text (cursor at the end)
//   --shot-hud     with --shot: draw the HUD, crosshair, selection overlay, regions and prompt box
//                  into the shot (to look at the UI; plain shots never contain it)
//   --submit-wait <s>   with --submit: keep polling the request (state, result.md, terminal
//                  process) for <s> seconds before exiting; state changes are logged
//   --close-submitted   with --submit: then close the submitted request like CTRL+K n (twice
//                  if it is still running; coordinator mode: cancels it) and wait up to 10 s
//                  for its terminal process to go; prints "close [n]: ..." and "close:
//                  terminal process gone after X s"
//   --classify "text"   print the model the launcher would choose for this prompt (tier,
//                  model, reason; launcher.c "Model choice") and exit without opening a
//                  window. Context: the loaded map (empty? MAP.md?), the --select lines, the
//                  --region count and a crosshair hit. --classify-kind edit|import|new-map
//                  sets the request kind (default edit)
//   In --submit, --type and --classify text, a literal \n becomes a new line.
//
// Environment (launcher.c):
//   MONOLITH_LAUNCH     coordinator (default: one coordinator session takes every request
//                       from a queue, docs/architecture.md section 5) or per-request (a new
//                       terminal with its own Claude Code session per request, as before)
//   MONOLITH_COORDINATOR_MODEL   the coordinator's model (default sonnet)
//   MONOLITH_TERMINAL   terminal to open for Claude Code: program name or path + optional
//                       args, e.g. "kitty --single-instance"
//   TERMINAL            used if MONOLITH_TERMINAL is unset; else the first of alacritty,
//                       kitty, foot, wezterm, xterm in PATH
//   MONOLITH_CLAUDE_ARGS   extra Claude Code flags, split on whitespace (default none), after
//                       --allow-dangerously-skip-permissions --dangerously-skip-permissions
//                       --model <name>, on the coordinator's (per-request: the request's)
//                       command line; a --model in them replaces the chosen model (both the
//                       coordinator's and the one recorded for each request)
//   MONOLITH_MODEL      one model for every request (turns the per-request choice off)
//   MONOLITH_MODEL_FAST / _DEFAULT / _HEAVY   model of each tier (default haiku / sonnet /
//                       opus). The tier is chosen per request from the prompt and its
//                       context; a leading @fast / @default / @heavy (or @haiku / @sonnet /
//                       @opus) in the prompt picks it and is stripped from the bundle's prompt
//   MONOLITH_CLAUDE_SAFE=1   drop those two permission flags
//   MONOLITH_WORKSPACE  i3/sway workspace for the terminal (default 10, empty = don't move)
//   MONOLITH_CLOSE_ON_EXIT=1   close the terminals of finished requests when the game quits
//                       (coordinator mode: the coordinator, if none of this run's requests
//                       is still queued or running)
//   MONOLITH_CLAUDE     the claude program to run (tests: tools/stub-coordinator.sh)
//   MONOLITH_REQUESTS_DIR   requests folder instead of <repo>/requests (tests only: bundles,
//                       queue and coordinator lock in a scratch folder)
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
#include "settings.h"
#include "settingspanel.h"
#include <errno.h>
#include <limits.h>
#include <math.h>

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
// map again (~1 s). False = nothing changed (the map on disk doesn't load: msg says why);
// *fatal = R3D was closed but the map then failed to load (the game has to quit).
static bool RestartRenderer(MS_LoadOptions *opt, int shadowSize, bool *fatal, char *msg, int msgSize)
{
    *fatal = false;
    char path[MS_PATH_MAX];
    snprintf(path, sizeof(path), "%s", scene->path);
    MS_LoadOptions peekOpt = *opt;
    peekOpt.noGpu = true;
    peekOpt.shadowSizeOverride = shadowSize;
    char err[512];
    MS_Scene *peek = MS_Load(path, &peekOpt, err, sizeof(err));
    if (!peek) { snprintf(msg, msgSize, "renderer not restarted: %.200s", err); return false; }
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
    scene = MS_Load(path, opt, err, sizeof(err));
    if (!scene) { *fatal = true; snprintf(msg, msgSize, "renderer restart failed: %.200s", err); return false; }
    snprintf(msg, msgSize, "renderer restarted: sun shadow map %d px", R3D_GetHint(R3D_HINT_SHADOW_DIR_SIZE));
    return true;
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
static void MakeFrameKey(FrameKey *k, Camera3D camera, const GfxSettings *gfx)
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
    const char **settingsSet;   // --settings-set key=value
    const char **settingsChange;    // --settings-change key=value
    int *selectLines;           // --select <line>
    Rectangle *regions;         // --region x y w h
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
    free(mainHeap.regions);
    free(mainHeap.pickAt);
    free(mainHeap.benchTimes);
    free(mainHeap.benchShadowUpdates);
    memset(&mainHeap, 0, sizeof(mainHeap));
}

static int Run(int argc, char **argv);

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
    const char *newMapName = NULL;
    const char *exportDir = NULL;               // --export
    const char *importDir = NULL, *importName = NULL;   // --import
    float fogCacheVoxel = -1.0f;    // --fog-cache
    int fogCacheSub = 2;
    const char *benchNote = NULL;
    const char *mapPath = NULL;
    const char *dumpPath = NULL;
    const char *dumpScenePath = NULL;   // --dump-scene, --dump-scene-diff
    bool dumpSceneDiff = false;         // --dump-scene-diff: no indices or lines
    const char *reloadTestPath = NULL;
    bool eyeSet = false, lookSet = false;   // --eye / --look given (they override --view)
    Vector3 eye = { 0 };
    bool pickFlag = false, shotHud = false;
    int frameReuseFlag = -1;    // --no-frame-reuse 0, --frame-reuse 1, default -1 (see below)
    const char *submitText = NULL, *typeText = NULL;
    float submitWait = -1.0f;       // --submit-wait: keep polling the request this long
    bool closeSubmitted = false;    // --close-submitted
    const char *classifyText = NULL, *classifyKind = "edit";
    const char *aoMode = NULL, *bloomMode = NULL;
    // Repeatable arguments: at most argc of each
    mainHeap.fx = malloc(sizeof(const char *) * argc);
    mainHeap.selectLines = malloc(sizeof(int) * argc);
    mainHeap.regions = malloc(sizeof(Rectangle) * argc);
    mainHeap.pickAt = malloc(sizeof(Vector2) * argc);
    mainHeap.settingsSet = malloc(sizeof(const char *) * argc);
    mainHeap.settingsChange = malloc(sizeof(const char *) * argc);
    if (!mainHeap.fx || !mainHeap.selectLines || !mainHeap.regions || !mainHeap.pickAt || !mainHeap.settingsSet ||
        !mainHeap.settingsChange) {
        fprintf(stderr, "monolith: out of memory\n");
        return 1;
    }
    const char **fxSettings = mainHeap.fx;
    int fxCount = 0;
    int *selectLines = mainHeap.selectLines, selectLineCount = 0;
    Rectangle *regionArgs = mainHeap.regions;
    Vector2 *pickAtArgs = mainHeap.pickAt;
    int pickAtCount = 0;
    int regionArgCount = 0;
    const char **settingsSets = mainHeap.settingsSet, **settingsChanges = mainHeap.settingsChange;
    int settingsSetCount = 0, settingsChangeCount = 0;
    // Every flag is checked here, before anything else happens: an unknown flag, a missing
    // value, a value that is another flag (`--pos 1 --shot x.png` must not eat --shot) or a
    // number that doesn't parse stops the program with a message and status 1. Safety rule:
    // a mistyped automated run must never fall through to the interactive (visible) game.
#define NEED(n) do { if (!ArgValues(argc, argv, i, (n))) return 1; } while (0)
#define NUM(k, lo, hi, dst) do { double v_; if (!ArgNumber(argv[i], argv[i + (k)], (lo), (hi), &v_)) return 1; (dst) = v_; } while (0)
#define INT(k, dst) do { long v_; if (!ArgInt(argv[i], argv[i + (k)], &v_)) return 1; (dst) = (int)v_; } while (0)
    const char *inspectPath = NULL;     // --inspect-model (runs after every flag is checked)
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
        else if (strcmp(a, "--import") == 0) { NEED(2); importDir = argv[i + 1]; importName = argv[i + 2]; i += 2; }
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
        else if (strcmp(a, "--map") == 0) { NEED(1); mapPath = argv[++i]; }
        else if (strcmp(a, "--dump-boxes") == 0) { NEED(1); dumpPath = argv[++i]; }
        else if (strcmp(a, "--dump-scene") == 0) { NEED(1); dumpScenePath = argv[++i]; dumpSceneDiff = false; }
        else if (strcmp(a, "--dump-scene-diff") == 0) { NEED(1); dumpScenePath = argv[++i]; dumpSceneDiff = true; }
        else if (strcmp(a, "--inspect-model") == 0) { NEED(1); inspectPath = argv[++i]; }
        else if (strcmp(a, "--reload-test") == 0) { NEED(1); reloadTestPath = argv[++i]; }
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
        else if (strcmp(a, "--no-frame-reuse") == 0) frameReuseFlag = 0;
        else if (strcmp(a, "--frame-reuse") == 0) frameReuseFlag = 1;
        else if (strcmp(a, "--submit") == 0) { NEED(1); submitText = argv[++i]; }
        else if (strcmp(a, "--submit-wait") == 0) { NEED(1); NUM(1, 0.0, 86400.0, submitWait); i++; }
        else if (strcmp(a, "--close-submitted") == 0) closeSubmitted = true;
        else if (strcmp(a, "--type") == 0) { NEED(1); typeText = argv[++i]; }
        else if (strcmp(a, "--classify") == 0) { NEED(1); classifyText = argv[++i]; }
        else if (strcmp(a, "--classify-kind") == 0) { NEED(1); classifyKind = argv[++i]; }
        else if (strcmp(a, "--select") == 0) { NEED(1); INT(1, selectLines[selectLineCount]); selectLineCount++; i++; }
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
            if (a[0] == '-') fprintf(stderr, "monolith: unknown flag '%s' (the flags are listed in the header comment of src/main.c)\n", a);
            else fprintf(stderr, "monolith: unexpected argument '%s' (no flag takes it as a value)\n", a);
            return 1;
        }
    }
#undef NEED
#undef NUM
#undef INT
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
    if (importDir) return ImportProject(mapsDir, importDir, importName) ? 0 : 1;

    // --- Graphics settings (settings.c). The interactive game reads the user's settings file
    // (and the panel saves it); automated runs (--shot, --bench, --dump-*, --export,
    // --classify) never read it unless --settings names a file, so their images and numbers
    // don't depend on what the developer last chose. --no-settings: no file at all. Then
    // --quality, then the single flags: they apply to this run and are never saved.
    // Last --settings-set: a change "as if made in the panel" (applies, and is saved).
    bool automated = shotPath || bench || dumpPath || dumpScenePath || exportDir || classifyText;
    const char *settingsFile = noSettings ? NULL : settingsPath ? settingsPath : automated ? NULL : GfxDefaultPath();
    GfxSettings gfx = GfxDefaults();
    if (settingsFile) {
        char err[512];
        if (GfxLoad(&gfx, settingsFile, err, sizeof(err)))
            TraceLog(LOG_INFO, "monolith: settings %s: quality %s%s%s", settingsFile, MS_QualityName(GfxPreset(&gfx)), err[0] ? "; " : "", err);
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
    if (!mapPath) mapPath = defaultMap;
    // (shadow size: an init hint, so a load option; the range is applied by MS_ApplyQuality)
    MS_LoadOptions loadOpt = { .noGpu = true, .shadowSizeOverride = gfx.q.shadowSize };
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
    bool hidden = shotPath || (bench && headless) || dumpScenePath;
    windowFlags |= hidden ? FLAG_WINDOW_HIDDEN : FLAG_WINDOW_RESIZABLE;
    if (!hidden && gfx.vsync) windowFlags |= FLAG_VSYNC_HINT;     // (settings: Performance)
    SetConfigFlags(windowFlags);
    // Distinct titles let the window manager route automated runs elsewhere, e.g. i3:
    //   for_window [title="^monolith-(bench|shot)$"] move container to workspace number 10
    // Headless benches use the size the tiled bench window gets on a 1080p screen, so
    // their numbers line up with the visible runs.
    bool headlessBench = bench && headless;
    InitWindow(headlessBench ? 1920 : shotPath ? shotW : 1600, headlessBench ? 1006 : shotPath ? shotH : 900,
               bench ? "monolith-bench" : (shotPath || dumpScenePath) ? "monolith-shot" : "monolith");
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
            bool ok = DumpScene(scene, dumpScenePath, dumpSceneDiff);
            fprintf(ok ? stdout : stderr, ok ? "monolith: wrote the scene to %s\n" : "monolith: cannot write %s\n", dumpScenePath);
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
    panel.interactive = !hidden;
    if (settingsPanelFlag && shotPath) PanelOpen(&panel);
    bool pendingApply = false;      // a heavy change waits one frame, so "applying ..." shows first
    const char *settingsSaveFile = automated ? (settingsPath && !noSettings ? settingsPath : NULL) : settingsFile;
    bool offApplied = false;  // --off toggles are applied on the first frame only
    float closePickTime = 0.0f; // CTRL+K: seconds left to press a request's number
    int exitStatus = 0;

    while (!WindowShouldClose())
    {
        float dt = fminf(GetFrameTime(), 1.0f / 20.0f);

        if (IsWindowResized()) MS_ApplyRenderResolution(scene, GetScreenWidth(), GetScreenHeight());

        // While the prompt box is open (and a couple of frames after it closed) every
        // game key and the camera are frozen: the keys are text now
        bool typing = DesignerCapturesInput(&designer);

        // Graphics settings panel (F2, both modes; settingspanel.c). A change goes to this
        // run's settings and to the saved ones (the command-line overrides of other settings
        // stay out of the file). Heavy changes (shadow map size, bakes) are applied one frame
        // later, so the panel shows "applying ..." first; the rest at once.
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
                    PanelStatus(&panel, "applying ...", 30.0f);
                } else applyNow = true;
                if (changed >= 0 && GfxRowNeedsRestart(changed)) PanelStatus(&panel, "window MSAA changes at the next start", 4.0f);
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
            char msg[256] = "";
            if (gfx.q.shadowSize != appliedShadowSize) {
                bool fatal;
                if (RestartRenderer(&loadOpt, gfx.q.shadowSize, &fatal, msg, sizeof(msg))) {
                    appliedShadowSize = gfx.q.shadowSize;
                    designer.version = (unsigned int)-1;   // remap the selection by its keys
                    reuse.keyValid = reuse.cacheValid = false;
                } else if (fatal) {
                    fprintf(stderr, "monolith: %s\n", msg);
                    exitStatus = 1;
                    break;
                } else gfx.q.shadowSize = appliedShadowSize;   // the map on disk is broken: keep the old one
                TraceLog(LOG_INFO, "monolith: %s", msg);
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
            if (msg[0]) PanelStatus(&panel, msg, 5.0f);
            else if (announced) PanelStatus(&panel, "applied", 1.5f);
        }
        typing = typing || PanelCapturesInput(&panel);

        // Closing requests (launcher.c): K = the last finished request, SHIFT+K = all finished
        // ones, CTRL+K then 1-9 / 0 (= 10) = request [n] of the HUD list, also a running one
        // (then its number twice; coordinator mode: cancels it), CTRL+SHIFT+K = close the
        // coordinator. While CTRL+K waits for the number, the digits don't toggle effects and
        // ESC cancels instead of quitting.
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
                if (ctrl && shift) LauncherCloseCoordinator(msg, sizeof(msg));
                else if (ctrl) {
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
            if (spec.active && !PanelCapturesInput(&panel)) {
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
            else if (!typing) UpdatePlayer(&player, dt);

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
            MakeFrameKey(&key, camera, &gfx);
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
                DrawText(TextFormat("%d fps%s  |  %s  |  1 volFog:%s  2 fog:%s  3 ssao:%s  4 ssil:%s  5 bloom:%s  6 sun:%s  |  F2 settings: %s",
                         GetFPS(), reused ? " (cached)" : "", spec.active ? "SPECTATOR" : player.fly ? "FLY" : "WALK",
                         env->volumetricFog.enabled ? "on" : "off", env->fog.mode ? "on" : "off",
                         env->ssao.enabled ? "on" : "off", env->ssil.enabled ? "on" : "off",
                         env->bloom.mode ? "on" : "off", MS_GetRenderSettings(scene).sunOn ? "on" : "off",
                         MS_QualityName(GfxPreset(&gfx))),
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
            // The settings panel shows even with the HUD off (F2 opened it)
            if (drawUi) PanelDraw(&panel, &gfx, GetScreenWidth(), GetScreenHeight(), reused);
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
            if (pickAtCount > 0) {
                // The same camera and ray frame as the shot (and as screen regions)
                Camera3D cam = spec.active ? SpectatorCamera(&spec) : PlayerCamera(&player);
                for (int k = 0; k < pickAtCount; k++) {
                    printf("pick-at %g %g (of %d x %d):\n", pickAtArgs[k].x, pickAtArgs[k].y, shotW, shotH);
                    DesignerPrintHit(DesignerPickAt(cam, shotW, shotH, pickAtArgs[k].x, pickAtArgs[k].y));
                }
                fflush(stdout);
            }
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
        WriteBenchReport(benchTimes, benchShadowUpdates, shadowSize, shadowRange, gfx.q.shadowMode != MS_SHADOW_ALWAYS, headless, benchNote, argc, argv);
    }

    if (gfxDirty && settingsSaveFile) {
        bool ok = GfxSave(&gfxSaved, settingsSaveFile);
        TraceLog(ok ? LOG_INFO : LOG_WARNING, "monolith: settings %s %s", ok ? "saved to" : "could not be saved to", settingsSaveFile);
    }
    PanelClose(&panel);             // (switches the GPU profiler off if the panel switched it on)
    LauncherCloseOnExit();          // MONOLITH_CLOSE_ON_EXIT=1: close finished requests' terminals
    R3D_SetProfilerEnabled(false);  // frees its GL queries
    BenchFree();
    DesignerFree(&designer);
    if (emulateWindow) WindowEmuUnload(&windowEmu);
    else if (hidden) UnloadRenderTexture(shotTarget);
    if (reuse.rt.id) UnloadRenderTexture(reuse.rt);
    if (reuse.composite.id) UnloadShader(reuse.composite);
    MS_Unload(scene);
    R3D_Close();
    CloseWindow();
    return exitStatus;
}

// #endregion
