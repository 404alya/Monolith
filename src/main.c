// monolith -- brutalist atmosphere test scene (raylib + R3D)
//
// Controls:
//   WASD / mouse   walk & look          SHIFT  walk faster
//   SPACE          jump                 F      toggle fly (noclip)
//   1..6           toggle effects (volumetric fog, fog, SSAO, SSIL, bloom, sun)
//   F1             toggle HUD           F12    screenshot
//   TAB            toggle spectator camera
//
// Spectator:
//   WASD / mouse   move & look          E/Q or SPACE/CTRL   up/down
//   mouse wheel    base speed           SHIFT x4, ALT x0.25
//   LEFT/RIGHT     fly to previous/next viewpoint
//   P              print + copy current view as --pos/--look arguments
//   G              move the player here
//
// Command line:
//   --shot <file>  render offscreen in a hidden window (never mapped, so the window
//                  manager doesn't show it), save the image to <file> and exit
//   --pos x y z    start position (feet)       --look yawDeg pitchDeg   start view angles
//   --off <keys>   start with these effects toggled off, e.g. --off 34 (SSAO + SSIL)
//   --view <n>     start in spectator mode at viewpoint n (1-based)
//   --shadow <size> <range>   sun shadow map size (px) and distance (m), default 8192 100
//   --shadow-always   re-render the sun shadow map every frame (default: only when the
//                  cached map no longer covers the view; for A/B tests and debugging)
//   --bench        visit every viewpoint, spin the camera, print frame time stats, write a
//                  Markdown report to benchmark/<timestamp>_shadow-<size>-<range>.md and exit
//   --headless     with --bench: hidden window + offscreen target + glFinish per frame,
//                  so nothing appears on screen (compare headless runs only with each other)
//   --bench-frames <n>   timed frames per viewpoint (10..600, default 600); fewer = quick
//                  screening runs (the camera spins faster, so the shadow map updates more)
//   --note "text"  free-form note added to the benchmark report

#include <r3d/r3d.h>
#include <raymath.h>
#include <rlgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#   include <sys/utsname.h>
#endif

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

// Data directories, with a trailing slash. Both builds are meant to be copied to other
// computers as a folder (build/linux/, build/win/), so they look next to the executable.
// On Linux, the repo directories baked in by build.sh win while they exist (i.e. on the
// dev machine), so concrete.glsl edits apply without rebuilding and bench reports land
// in the repo's benchmark/. Windows paths aren't baked in (wine would see Z:\...).
static char resourcesDir[1024], benchmarkDir[1024];

static void InitDataPaths(void)
{
    const char *exeDir = GetApplicationDirectory();
#ifdef _WIN32
    snprintf(resourcesDir, sizeof(resourcesDir), "%sresources/", exeDir);
    snprintf(benchmarkDir, sizeof(benchmarkDir), "%sbenchmark/", exeDir);
#else
    if (DirectoryExists(RESOURCES_PATH)) snprintf(resourcesDir, sizeof(resourcesDir), "%s", RESOURCES_PATH);
    else snprintf(resourcesDir, sizeof(resourcesDir), "%sresources/", exeDir);
    if (DirectoryExists(BENCHMARK_PATH)) snprintf(benchmarkDir, sizeof(benchmarkDir), "%s", BENCHMARK_PATH);
    else snprintf(benchmarkDir, sizeof(benchmarkDir), "%sbenchmark/", exeDir);
#endif
}

// ======================================================================================
// #region World (axis-aligned concrete boxes, used for both rendering and collision)
// ======================================================================================

#define MAX_BOXES 8192

typedef struct {
    BoundingBox items[MAX_BOXES];
    int count;
} World;

static World world;

static void Box(float x0, float y0, float z0, float x1, float y1, float z1)
{
    if (world.count >= MAX_BOXES) return;
    world.items[world.count++] = (BoundingBox){
        { fminf(x0, x1), fminf(y0, y1), fminf(z0, z1) },
        { fmaxf(x0, x1), fmaxf(y0, y1), fmaxf(z0, z1) },
    };
}

// Box from center (x, z), ground level y0 and size
static void Block(float cx, float y0, float cz, float sx, float sy, float sz)
{
    Box(cx - sx * 0.5f, y0, cz - sz * 0.5f, cx + sx * 0.5f, y0 + sy, cz + sz * 0.5f);
}

// Straight staircase climbing toward -Z. Each step is a solid block down to baseY.
static void Stairs(float cx, float baseY, float startZ, float width, int steps, float rise, float run)
{
    for (int i = 0; i < steps; i++) {
        float z0 = startZ - i * run;
        Box(cx - width * 0.5f, baseY, z0, cx + width * 0.5f, baseY + (i + 1) * rise, z0 - run);
    }
}

// Facade with a grid of deep window recesses ("waffle" facade):
// a back wall plus vertical fins and horizontal ledges in front of it.
static void WaffleFacade(float x0, float x1, float y0, float y1, float z, float depth, float bayW, float floorH)
{
    Box(x0, y0, z - 1.0f, x1, y1, z);                       // back wall
    for (float x = x0; x <= x1 + 0.01f; x += bayW)           // vertical fins
        Box(x - 0.3f, y0, z, x + 0.3f, y1, z + depth);
    for (float y = y0 + floorH; y <= y1 + 0.01f; y += floorH) // horizontal ledges
        Box(x0, y - 0.5f, z, x1, y, z + depth);
}

static void BuildWorld(void)
{
    world.count = 0;

    // Ground
    Box(-400, -1, -400, 400, 0, 400);

    // --- 1. Entry canyon: a narrow slot between two monumental walls -------------
    Box(-4.5f, 0, 40, -3.0f, 34, 95);
    Box( 3.0f, 0, 40,  4.5f, 34, 95);
    for (float z = 40; z < 95; z += 9)  // roof segments, thin light slits between them
        Box(-4.5f, 34, z, 4.5f, 36, fminf(z + 8.4f, 95));
    Box(-4.5f, 0, 95, 4.5f, 36, 97);    // back wall behind player

    // --- 2. Hypostyle hall: forest of pillars under a slatted roof ---------------
    const float hallY = 20.0f;
    for (float x = -36; x <= 36; x += 9)
        for (float z = -32; z <= 32; z += 9)
            if (fabsf(x) > 4.0f)   // keep the central axis free
                Block(x, 0, z, 2.2f, hallY, 2.2f);

    // Roof beams with gaps -> shafts of light through the volumetric fog
    for (float z = -36; z <= 36; z += 4.5f)
        Box(-40, hallY, z, 40, hallY + 2.0f, z + 2.6f);
    Box(-40, hallY + 2.0f, -36, 40, hallY + 3.0f, -30); // solid part of the roof
    Box(-40, hallY + 2.0f,  30, 40, hallY + 3.0f,  39);

    // Massive cantilevered block hanging over the hall
    Box(-14, hallY + 3.0f, -10, 30, hallY + 15.0f, 12);

    // --- 3. Monumental stairs up to the upper terrace --------------------------
    Stairs(0, 0, -40, 22, 40, 0.3f, 0.75f);           // rises 12 m
    Box(-11.5f, 0, -40, -11, 13, -70);                 // side walls
    Box( 11, 0, -40,  11.5f, 13, -70);
    Box(-60, 0, -70, 60, 12, -140);                    // terrace

    // --- 4. Tower block with a waffle facade on the terrace -------------------
    WaffleFacade(-30, 30, 12, 92, -100, 2.0f, 4.0f, 4.0f);
    Box(-30, 12, -140, 30, 92, -101);                  // body
    Box(-34, 92, -142, 34, 96, -96);                   // crown slab

    // Lone object in a big void: a single pillar on the terrace
    Block(18, 12, -82, 1.2f, 6.0f, 1.2f);

    // --- 5. Distant megastructures, only silhouettes in the fog ---------------
    struct { float x, z, w, h, d; } mega[] = {
        { -170, -120, 60, 180, 70 }, {  160, -180, 90, 240, 60 },
        { -120,  160, 80, 140, 80 }, {  190,   60, 50, 200, 120 },
        {    0, -300, 220, 120, 40 }, { -260,  -20, 40, 260, 40 },
    };
    for (size_t i = 0; i < sizeof(mega) / sizeof(mega[0]); i++) {
        Block(mega[i].x, 0, mega[i].z, mega[i].w, mega[i].h, mega[i].d);
        // repeating fins on each megastructure
        for (float x = -mega[i].w * 0.5f; x <= mega[i].w * 0.5f; x += 6)
            Block(mega[i].x + x, 0, mega[i].z + mega[i].d * 0.5f + 1.0f, 1.0f, mega[i].h, 2.0f);
    }

    // A bridge between two megastructures, very high up
    Box(-170, 120, -152, 160, 128, -144);
}

// #endregion
// ======================================================================================
// #region Player
// ======================================================================================

#define PLAYER_RADIUS   0.3f
#define PLAYER_HEIGHT   1.75f
#define EYE_HEIGHT      1.62f
#define STEP_HEIGHT     0.45f
#define WALK_SPEED      3.2f
#define RUN_SPEED       6.0f
#define FLY_SPEED       20.0f
#define GRAVITY         20.0f
#define JUMP_SPEED      6.0f

typedef struct {
    Vector3 feet;
    Vector3 vel;
    float yaw, pitch;
    bool grounded;
    bool fly;
    float bobPhase;
} Player;

static BoundingBox PlayerBox(Vector3 feet)
{
    return (BoundingBox){
        { feet.x - PLAYER_RADIUS, feet.y, feet.z - PLAYER_RADIUS },
        { feet.x + PLAYER_RADIUS, feet.y + PLAYER_HEIGHT, feet.z + PLAYER_RADIUS },
    };
}

// Strict overlap: touching faces do not count (raylib's CheckCollisionBoxes is
// inclusive, which would make standing on the ground block all movement)
static bool BoxesOverlap(BoundingBox a, BoundingBox b)
{
    return a.min.x < b.max.x && a.max.x > b.min.x &&
           a.min.y < b.max.y && a.max.y > b.min.y &&
           a.min.z < b.max.z && a.max.z > b.min.z;
}

// Returns the index of the first box overlapping the player at `feet`, or -1
static int Overlap(Vector3 feet)
{
    BoundingBox pb = PlayerBox(feet);
    for (int i = 0; i < world.count; i++)
        if (BoxesOverlap(pb, world.items[i])) return i;
    return -1;
}

// Moves along one horizontal axis, stepping up onto low obstacles (stairs)
static void MoveHorizontal(Player *p, int axis, float delta)
{
    if (delta == 0.0f) return;

    Vector3 next = p->feet;
    if (axis == 0) next.x += delta; else next.z += delta;

    int hit = Overlap(next);
    if (hit < 0) { p->feet = next; return; }

    float stepUp = world.items[hit].max.y - p->feet.y;
    if (p->grounded && stepUp > 0.0f && stepUp <= STEP_HEIGHT) {
        Vector3 up = next;
        up.y = world.items[hit].max.y + 0.001f;
        if (Overlap(up) < 0) { p->feet = up; return; }
    }
}

static void UpdatePlayer(Player *p, float dt)
{
    Vector2 md = GetMouseDelta();
    p->yaw   -= md.x * 0.0022f;
    p->pitch -= md.y * 0.0022f;
    p->pitch  = Clamp(p->pitch, -1.5f, 1.5f);

    if (IsKeyPressed(KEY_F)) { p->fly = !p->fly; p->vel = Vector3Zero(); }

    Vector3 fwd   = { sinf(p->yaw), 0, cosf(p->yaw) };
    Vector3 right = { -cosf(p->yaw), 0, sinf(p->yaw) };

    Vector3 wish = Vector3Zero();
    if (IsKeyDown(KEY_W)) wish = Vector3Add(wish, fwd);
    if (IsKeyDown(KEY_S)) wish = Vector3Subtract(wish, fwd);
    if (IsKeyDown(KEY_D)) wish = Vector3Add(wish, right);
    if (IsKeyDown(KEY_A)) wish = Vector3Subtract(wish, right);
    wish = Vector3Normalize(wish);

    if (p->fly) {
        Vector3 look = { cosf(p->pitch) * sinf(p->yaw), sinf(p->pitch), cosf(p->pitch) * cosf(p->yaw) };
        Vector3 dir = Vector3Zero();
        if (IsKeyDown(KEY_W)) dir = Vector3Add(dir, look);
        if (IsKeyDown(KEY_S)) dir = Vector3Subtract(dir, look);
        if (IsKeyDown(KEY_D)) dir = Vector3Add(dir, right);
        if (IsKeyDown(KEY_A)) dir = Vector3Subtract(dir, right);
        if (IsKeyDown(KEY_SPACE)) dir.y += 1.0f;
        if (IsKeyDown(KEY_LEFT_CONTROL)) dir.y -= 1.0f;
        float speed = FLY_SPEED * (IsKeyDown(KEY_LEFT_SHIFT) ? 4.0f : 1.0f);
        p->feet = Vector3Add(p->feet, Vector3Scale(Vector3Normalize(dir), speed * dt));
        return;
    }

    // Smoothly accelerate toward the wished velocity: heavy, slow, deliberate
    float speed = IsKeyDown(KEY_LEFT_SHIFT) ? RUN_SPEED : WALK_SPEED;
    float accel = p->grounded ? 10.0f : 2.0f;
    float t = 1.0f - expf(-accel * dt);
    p->vel.x = Lerp(p->vel.x, wish.x * speed, t);
    p->vel.z = Lerp(p->vel.z, wish.z * speed, t);

    if (p->grounded && IsKeyPressed(KEY_SPACE)) p->vel.y = JUMP_SPEED;
    p->vel.y -= GRAVITY * dt;

    MoveHorizontal(p, 0, p->vel.x * dt);
    MoveHorizontal(p, 2, p->vel.z * dt);

    // Vertical
    Vector3 next = p->feet;
    next.y += p->vel.y * dt;
    int hit = Overlap(next);
    p->grounded = false;
    if (hit >= 0) {
        if (p->vel.y < 0.0f) {
            next.y = world.items[hit].max.y;
            p->grounded = true;
        } else {
            next.y = world.items[hit].min.y - PLAYER_HEIGHT - 0.001f;
        }
        p->vel.y = 0.0f;
    }
    p->feet = next;

    // Head bob, only while walking on the ground
    float hspeed = sqrtf(p->vel.x * p->vel.x + p->vel.z * p->vel.z);
    if (p->grounded) p->bobPhase += hspeed * dt * 2.2f;
}

static Camera3D PlayerCamera(const Player *p)
{
    float bob = p->fly ? 0.0f : sinf(p->bobPhase * 2.0f) * 0.035f;
    Vector3 eye = { p->feet.x, p->feet.y + EYE_HEIGHT + bob, p->feet.z };
    Vector3 look = { cosf(p->pitch) * sinf(p->yaw), sinf(p->pitch), cosf(p->pitch) * cosf(p->yaw) };
    return (Camera3D){
        .position = eye,
        .target = Vector3Add(eye, look),
        .up = { 0, 1, 0 },
        .fovy = 70.0f,
        .projection = CAMERA_PERSPECTIVE,
    };
}

// #endregion
// ======================================================================================
// #region Spectator camera
// ======================================================================================

#define SPEC_MIN_SPEED  1.0f
#define SPEC_MAX_SPEED  400.0f
#define SPEC_FLY_TIME   1.6f    // seconds for a viewpoint transition

typedef struct {
    const char *name;
    Vector3 eye;
    Vector3 target;
} Viewpoint;

static const Viewpoint VIEWPOINTS[] = {
    { "Canyon",         {    0.0f,   1.6f,   88.0f }, {    0.0f,  4.0f,   40.0f } },
    { "Hypostyle hall", {   -6.0f,   1.6f,   34.0f }, {    6.0f,  6.0f,    0.0f } },
    { "Above the roof", {  -24.0f,  30.0f,   26.0f }, {    4.0f, 20.0f,    4.0f } },
    { "Stairs",         {    0.0f,   2.0f,  -25.0f }, {    0.0f, 10.0f,  -70.0f } },
    { "Terrace",        {    8.0f,  13.6f,  -70.0f }, {    0.0f, 30.0f, -100.0f } },
    { "Tower",          {   38.0f,  40.0f,  -55.0f }, {    0.0f, 55.0f, -100.0f } },
    { "Overview",       {   55.0f,  38.0f,   52.0f }, {    0.0f,  8.0f,  -10.0f } },
};
#define VIEWPOINT_COUNT ((int)(sizeof(VIEWPOINTS) / sizeof(VIEWPOINTS[0])))

typedef struct {
    bool active;
    Vector3 pos;
    Vector3 vel;
    float yaw, pitch;
    float speed;            // base speed, changed with the mouse wheel

    int view;               // last selected viewpoint, -1 if none
    bool flying;            // animating toward a viewpoint
    float flyT;
    Vector3 fromPos, toPos;
    float fromYaw, toYaw, fromPitch, toPitch;

    char message[128];      // short-lived HUD message
    float messageTime;
} Spectator;

static void LookAngles(Vector3 eye, Vector3 target, float *yaw, float *pitch)
{
    Vector3 d = Vector3Normalize(Vector3Subtract(target, eye));
    *yaw = atan2f(d.x, d.z);
    *pitch = asinf(Clamp(d.y, -1.0f, 1.0f));
}

static Vector3 LookDir(float yaw, float pitch)
{
    return (Vector3){ cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw) };
}

static void SpectatorMessage(Spectator *s, const char *text)
{
    snprintf(s->message, sizeof(s->message), "%s", text);
    s->messageTime = 3.0f;
}

static void SpectatorFlyTo(Spectator *s, int view)
{
    s->view = view;
    s->flying = true;
    s->flyT = 0.0f;
    s->vel = Vector3Zero();
    s->fromPos = s->pos;
    s->fromYaw = s->yaw;
    s->fromPitch = s->pitch;
    s->toPos = VIEWPOINTS[view].eye;
    LookAngles(VIEWPOINTS[view].eye, VIEWPOINTS[view].target, &s->toYaw, &s->toPitch);
    // Turn the short way around
    s->toYaw = s->fromYaw + Wrap(s->toYaw - s->fromYaw, -PI, PI);
    SpectatorMessage(s, TextFormat("[%d/%d] %s", view + 1, VIEWPOINT_COUNT, VIEWPOINTS[view].name));
}

static void SpectatorJumpTo(Spectator *s, int view)
{
    s->view = view;
    s->flying = false;
    s->vel = Vector3Zero();
    s->pos = VIEWPOINTS[view].eye;
    LookAngles(VIEWPOINTS[view].eye, VIEWPOINTS[view].target, &s->yaw, &s->pitch);
}

static void UpdateSpectator(Spectator *s, Player *player, float dt)
{
    if (s->messageTime > 0.0f) s->messageTime -= dt;

    // Viewpoints: arrows cycle with a smooth flight
    if (IsKeyPressed(KEY_RIGHT)) SpectatorFlyTo(s, (s->view + 1) % VIEWPOINT_COUNT);
    if (IsKeyPressed(KEY_LEFT))  SpectatorFlyTo(s, (s->view <= 0 ? VIEWPOINT_COUNT : s->view) - 1);

    // Speed: mouse wheel, exponential so it is equally quick at 2 m/s and 200 m/s
    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        s->speed = Clamp(s->speed * powf(1.25f, wheel), SPEC_MIN_SPEED, SPEC_MAX_SPEED);
        SpectatorMessage(s, TextFormat("speed %.1f m/s", s->speed));
    }

    // Print the current view as command line arguments (for --shot and bookmarks)
    if (IsKeyPressed(KEY_P)) {
        const char *line = TextFormat("--pos %.2f %.2f %.2f --look %.1f %.1f",
                                      s->pos.x, s->pos.y - EYE_HEIGHT, s->pos.z,
                                      Wrap(s->yaw * RAD2DEG, 0.0f, 360.0f), s->pitch * RAD2DEG);
        printf("%s\n", line);
        fflush(stdout);
        SetClipboardText(line);
        SpectatorMessage(s, TextFormat("copied: %s", line));
    }

    // Drop the player at the current spot
    if (IsKeyPressed(KEY_G)) {
        player->feet = (Vector3){ s->pos.x, s->pos.y - EYE_HEIGHT, s->pos.z };
        player->vel = Vector3Zero();
        player->yaw = s->yaw;
        player->pitch = s->pitch;
        SpectatorMessage(s, "player moved here (TAB to walk)");
    }

    Vector2 md = GetMouseDelta();
    bool input = md.x != 0.0f || md.y != 0.0f ||
                 IsKeyDown(KEY_W) || IsKeyDown(KEY_A) || IsKeyDown(KEY_S) || IsKeyDown(KEY_D) ||
                 IsKeyDown(KEY_E) || IsKeyDown(KEY_Q) || IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_LEFT_CONTROL);

    if (s->flying) {
        // Any manual input cancels the flight
        if (input) s->flying = false;
        else {
            s->flyT += dt / SPEC_FLY_TIME;
            float t = fminf(s->flyT, 1.0f);
            float e = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);   // smootherstep

            // Arc upward on long flights so we don't clip straight through buildings
            float dist = Vector3Distance(s->fromPos, s->toPos);
            float lift = sinf(e * PI) * fminf(dist * 0.25f, 60.0f);

            s->pos = Vector3Lerp(s->fromPos, s->toPos, e);
            s->pos.y += lift;
            s->yaw = Lerp(s->fromYaw, s->toYaw, e);
            s->pitch = Lerp(s->fromPitch, s->toPitch, e);
            if (t >= 1.0f) s->flying = false;
            return;
        }
    }

    s->yaw   -= md.x * 0.0022f;
    s->pitch -= md.y * 0.0022f;
    s->pitch  = Clamp(s->pitch, -1.55f, 1.55f);

    Vector3 fwd = LookDir(s->yaw, s->pitch);
    Vector3 right = { -cosf(s->yaw), 0, sinf(s->yaw) };

    Vector3 wish = Vector3Zero();
    if (IsKeyDown(KEY_W)) wish = Vector3Add(wish, fwd);
    if (IsKeyDown(KEY_S)) wish = Vector3Subtract(wish, fwd);
    if (IsKeyDown(KEY_D)) wish = Vector3Add(wish, right);
    if (IsKeyDown(KEY_A)) wish = Vector3Subtract(wish, right);
    if (IsKeyDown(KEY_E) || IsKeyDown(KEY_SPACE))        wish.y += 1.0f;
    if (IsKeyDown(KEY_Q) || IsKeyDown(KEY_LEFT_CONTROL)) wish.y -= 1.0f;
    wish = Vector3Normalize(wish);

    float speed = s->speed;
    if (IsKeyDown(KEY_LEFT_SHIFT)) speed *= 4.0f;
    if (IsKeyDown(KEY_LEFT_ALT))   speed *= 0.25f;

    // Damped velocity: glides to a stop instead of halting instantly
    float t = 1.0f - expf(-6.0f * dt);
    s->vel = Vector3Lerp(s->vel, Vector3Scale(wish, speed), t);
    s->pos = Vector3Add(s->pos, Vector3Scale(s->vel, dt));
}

static Camera3D SpectatorCamera(const Spectator *s)
{
    return (Camera3D){
        .position = s->pos,
        .target = Vector3Add(s->pos, LookDir(s->yaw, s->pitch)),
        .up = { 0, 1, 0 },
        .fovy = 70.0f,
        .projection = CAMERA_PERSPECTIVE,
    };
}

// #endregion
// ======================================================================================
// #region Atmosphere
// ======================================================================================

// Direction the sunlight travels: high enough to fall through the roof gaps,
// from the side so it rakes across the facades.
static const Vector3 SUN_DIR = { 0.55f, -0.78f, 0.30f };

static void SetupEnvironment(R3D_Cubemap *sky, R3D_AmbientMap *ambient)
{
    // Pale, overcast-ish sky with a hazy sun
    R3D_ProceduralSky sp = R3D_PROCEDURAL_SKY_BASE;
    sp.skyTopColor        = (Color){ 120, 132, 145, 255 };
    sp.skyHorizonColor    = (Color){ 200, 196, 186, 255 };
    sp.groundBottomColor  = (Color){ 48, 46, 44, 255 };
    sp.groundHorizonColor = (Color){ 190, 186, 178, 255 };
    sp.sunDirection       = SUN_DIR;
    sp.sunColor           = (Color){ 255, 236, 205, 255 };
    sp.sunSize            = 2.5f * DEG2RAD;
    sp.sunEnergy          = 2.0f;
    *sky = R3D_GenProceduralSky(1024, sp);
    *ambient = R3D_GenAmbientMap(*sky, R3D_AMBIENT_ILLUMINATION | R3D_AMBIENT_REFLECTION);

    R3D_Environment *env = R3D_GetEnvironment();
    env->background.sky = *sky;
    env->ambient.map = *ambient;
    env->ambient.energy = 0.9f;

    // Distance fog: swallows the megastructures
    env->fog.mode = R3D_FOG_EXP2;
    env->fog.color = (Color){ 150, 148, 143, 255 };
    env->fog.density = 0.0085f;
    env->fog.skyAffect = 0.7f;

    // Volumetric fog: god rays through the roof slits and between the pillars
    env->volumetricFog.enabled = true;
    // Keep the density low: R3D's volumetric fog is lit only by lights, not by
    // the sky, so dense fog in shadow turns into a black curtain that hides
    // everything behind it. The emission stands in for the missing sky light.
    env->volumetricFog.scatteringDensity = 0.012f;
    env->volumetricFog.absortionDensity = 0.001f;
    env->volumetricFog.scatteringColor = (Color){ 255, 244, 228, 255 };
    env->volumetricFog.emissionColor = (Color){ 150, 148, 143, 255 };
    env->volumetricFog.emissionEnergy = 0.12f;
    env->volumetricFog.anisotropy = 0.4f;
    env->volumetricFog.length = 90.0f;
    env->volumetricFog.stepSize = 0.75f;

    // Contact shadows in concrete corners + bounced light
    env->ssao.enabled = true;
    env->ssao.radius = 1.2f;
    env->ssao.intensity = 1.2f;
    env->ssil.enabled = true;
    env->ssil.radius = 5.0f;
    env->ssil.giIntensity = 1.2f;

    env->bloom.mode = R3D_BLOOM_MIX;
    env->bloom.intensity = 0.04f;

    env->tonemap.mode = R3D_TONEMAP_AGX;
    env->tonemap.exposure = 0.9f;
    env->color.saturation = 0.8f;
    env->color.contrast = 1.05f;
}

// #endregion
// ======================================================================================
// #region Sun shadow reuse
// ======================================================================================

// Re-rendering the 8192 px sun shadow map is a large share of the frame, and the
// scene is static, so the map only has to be redrawn when the area it covers no
// longer matches the view. R3D keeps the light-space matrix next to the cached
// map (R3D_PushLightEx(..., updateShadow = false)), so a reused map is always
// sampled consistently; the only risk is coverage.
//
// Why a reused map is pixel-identical where it covers: R3D's fit
// (light_dir_view_proj in r3d/src/modules/r3d_light.c) uses a radius that
// depends only on range, fov and aspect, and snaps the centre to whole texels,
// so every fit lays the same world-space texel grid. Shadow results can only
// differ through R3D's fades: edgeFade (last 5% of the map, in x, y and depth)
// times distFade (view depth 75%..100% of range). So the map is reused while,
// at every point of the view frustum out to range, the cached map's fade is
// within SHADOW_REUSE_EPS of what a freshly fitted map would give. That holds
// for the volumetric fog too: its march samples lie inside the same frustum
// and are faded by the depth of the pixel behind them, which is never less.

#define SHADOW_REUSE_EPS    (1.0f / 255.0f)   // below 8-bit visibility

typedef struct {
    bool valid;
    Matrix viewProj;        // our copy of the light-space matrix R3D cached with the map
    float aspect;           // fit inputs that change the radius, i.e. the texel grid
    float range;
} SunShadowCache;

// Replicates R3D's light_dir_view_proj(): ortho box around the bounding sphere
// of the camera frustum out to `range`, centre snapped to texels.
static Matrix SunShadowFit(Vector3 dir, float range, Camera3D cam, float aspect, int mapSize)
{
    float camNear = range / 1000.0f, camFar = range;
    float farH = camFar * tanf(cam.fovy * DEG2RAD * 0.5f);
    float halfDepth = (camFar - camNear) * 0.5f;
    float radius = sqrtf(farH * farH * (1.0f + aspect * aspect) + halfDepth * halfDepth);

    Vector3 forward = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 center = Vector3Add(cam.position, Vector3Scale(forward, (camNear + camFar) * 0.5f));

    float ax = fabsf(dir.x), ay = fabsf(dir.y), az = fabsf(dir.z);
    Vector3 up = (ax <= ay && ax <= az) ? (Vector3){ 1, 0, 0 } : (ay <= az) ? (Vector3){ 0, 1, 0 } : (Vector3){ 0, 0, 1 };
    Vector3 lightRight = Vector3Normalize(Vector3CrossProduct(up, dir));
    Vector3 lightUp = Vector3CrossProduct(dir, lightRight);

    float texel = radius * 2.0f / (float)mapSize;
    float cx = floorf(Vector3DotProduct(center, lightRight) / texel) * texel;
    float cy = floorf(Vector3DotProduct(center, lightUp) / texel) * texel;
    float cz = Vector3DotProduct(center, dir);
    Vector3 snapped = Vector3Add(Vector3Add(Vector3Scale(lightRight, cx), Vector3Scale(lightUp, cy)), Vector3Scale(dir, cz));

    const float zExtension = 100.0f;
    Vector3 eye = Vector3Subtract(snapped, Vector3Scale(dir, radius + zExtension));
    Matrix view = MatrixLookAt(eye, snapped, lightUp);
    Matrix proj = MatrixOrtho(-radius, radius, -radius, radius, 0.0f, zExtension + radius * 2.0f);
    return MatrixMultiply(view, proj);
}

// GLSL smoothstep, also valid for edge0 > edge1 as R3D uses it
static float SmoothStep(float e0, float e1, float x)
{
    float t = Clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// R3D's edgeFade (L_SampleShadowDir in r3d/shaders/include/wrap/light.glsl)
static float ShadowEdgeFade(Matrix viewProj, Vector3 p)
{
    Vector3 c = Vector3Transform(p, viewProj);   // ortho: w = 1
    Vector3 uv = { c.x * 0.5f + 0.5f, c.y * 0.5f + 0.5f, c.z * 0.5f + 0.5f };
    float d = fminf(fminf(fminf(uv.x, 1.0f - uv.x), fminf(uv.y, 1.0f - uv.y)), fminf(uv.z, 1.0f - uv.z));
    return SmoothStep(0.0f, 0.05f, d);
}

// Decides whether the sun shadow map must be re-rendered this frame
static bool SunShadowNeedsUpdate(SunShadowCache *c, Vector3 sunDir, float range, Camera3D cam, float aspect, int mapSize)
{
    Matrix fresh = SunShadowFit(sunDir, range, cam, aspect, mapSize);
    bool update = !c->valid || c->aspect != aspect || c->range != range;

    // Sample the view frustum out to range: depth slices x a grid across each slice
    Vector3 f = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 r = Vector3Normalize(Vector3CrossProduct(f, cam.up));
    Vector3 u = Vector3CrossProduct(r, f);
    float tanH = tanf(cam.fovy * DEG2RAD * 0.5f);
    enum { SLICES = 48, GX = 33, GY = 17 };
    for (int k = 0; k <= SLICES && !update; k++) {
        float camNear = (float)rlGetCullDistanceNear();   // the near plane R3D_CameraFromRL uses
        float z = camNear + (range - camNear) * k / SLICES;
        float distFade = SmoothStep(range, range * 0.75f, z);
        if (distFade <= 0.0f) continue;
        Vector3 sliceCenter = Vector3Add(cam.position, Vector3Scale(f, z));
        for (int j = 0; j < GY && !update; j++)
            for (int i = 0; i < GX; i++) {
                float x = (2.0f * i / (GX - 1) - 1.0f) * z * tanH * aspect;
                float y = (2.0f * j / (GY - 1) - 1.0f) * z * tanH;
                Vector3 p = Vector3Add(sliceCenter, Vector3Add(Vector3Scale(r, x), Vector3Scale(u, y)));
                float loss = ShadowEdgeFade(fresh, p) - ShadowEdgeFade(c->viewProj, p);
                if (distFade * loss > SHADOW_REUSE_EPS) { update = true; break; }
            }
    }

    if (update) {
        *c = (SunShadowCache){ true, fresh, aspect, range };
    }
    return update;
}

// #endregion
// ======================================================================================
// #region Benchmark report
// ======================================================================================

#define BENCH_WARMUP 60
#define BENCH_FRAMES 600     // timed frames per viewpoint (max); --bench-frames lowers it

static int benchFrames = BENCH_FRAMES;

// Only needed to put the GPU/driver into the benchmark report
#define GL_VENDOR   0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION  0x1F02
extern const unsigned char *glGetString(unsigned int name);

typedef struct {
    double avgMs, avgFps, lowFps, worstMs;
} BenchStats;

static BenchStats ComputeBenchStats(const double *times, int n)
{
    static double sorted[BENCH_FRAMES];
    double sum = 0.0;
    for (int i = 0; i < n; i++) { sum += times[i]; sorted[i] = times[i]; }

    // Partial selection sort: the slowest 1% of frames end up first
    int lowCount = n / 100 > 0 ? n / 100 : 1;
    for (int a = 0; a < lowCount; a++)
        for (int b = a + 1; b < n; b++)
            if (sorted[b] > sorted[a]) { double t = sorted[a]; sorted[a] = sorted[b]; sorted[b] = t; }
    double slow = 0.0;
    for (int a = 0; a < lowCount; a++) slow += sorted[a];
    slow /= lowCount;

    double avg = sum / n;
    return (BenchStats){ avg * 1000.0, 1.0 / avg, 1.0 / slow, sorted[0] * 1000.0 };
}

// Prints the results and writes them as Markdown to benchmarkDir/<timestamp>_shadow-<size>-<range>.md
static void WriteBenchReport(double times[][BENCH_FRAMES], const int *shadowUpdates, int shadowSize,
                             float shadowRange, bool shadowReuse, bool headless, const char *note, int argc, char **argv)
{
    BenchStats stats[VIEWPOINT_COUNT];
    double total = 0.0;
    for (int v = 0; v < VIEWPOINT_COUNT; v++) {
        stats[v] = ComputeBenchStats(times[v], benchFrames);
        for (int f = 0; f < benchFrames; f++) total += times[v][f];
    }
    int frames = VIEWPOINT_COUNT * benchFrames;
    double allMs = total / frames * 1000.0, allFps = frames / total;

    // stdout
    printf("\nbench: %dx%d, shadow %d px / %.0f m, %d frames per view\n",
           GetRenderWidth(), GetRenderHeight(), shadowSize, shadowRange, benchFrames);
    printf("%-16s %9s %9s %9s %9s\n", "view", "avg ms", "avg fps", "1% low", "sh.upd");
    for (int v = 0; v < VIEWPOINT_COUNT; v++)
        printf("%-16s %9.3f %9.1f %9.1f %9d\n", VIEWPOINTS[v].name, stats[v].avgMs, stats[v].avgFps, stats[v].lowFps, shadowUpdates[v]);
    printf("%-16s %9.3f %9.1f\n", "ALL", allMs, allFps);

    // Markdown report
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char stamp[32], when[32], path[1024];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", lt);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", lt);
    if (snprintf(path, sizeof(path), "%s%s_shadow-%d-%.0f.md", benchmarkDir, stamp, shadowSize, shadowRange) >= (int)sizeof(path)) {
        fprintf(stderr, "bench: report path too long: %s\n", benchmarkDir);
        return;
    }

    if (!DirectoryExists(benchmarkDir)) MakeDirectory(benchmarkDir);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }

    const char *vsyncEnv = getenv("__GL_SYNC_TO_VBLANK");
    const char *vblankEnv = getenv("vblank_mode");
#ifndef _WIN32
    struct utsname un;
    bool haveUname = uname(&un) == 0;
#endif
    R3D_Environment *env = R3D_GetEnvironment();

    fprintf(f, "# Benchmark %s\n\n", when);
    if (note && note[0]) fprintf(f, "> %s\n\n", note);

    fprintf(f, "## Setup\n\n");
    fprintf(f, "| | |\n|---|---|\n");
    fprintf(f, "| GPU | %s |\n", (const char *)glGetString(GL_RENDERER));
    fprintf(f, "| Driver | %s / %s |\n", (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_VERSION));
#ifdef _WIN32
    fprintf(f, "| System | Windows (x86_64, MinGW-w64 build) |\n");
#else
    if (haveUname) fprintf(f, "| System | %s %s (%s) |\n", un.sysname, un.release, un.machine);
#endif
    fprintf(f, "| Resolution | %d x %d |\n", GetRenderWidth(), GetRenderHeight());
    fprintf(f, "| Sun shadow | %d px, range %.0f m, %s |\n", shadowSize, shadowRange,
            shadowReuse ? "re-rendered only when the view leaves the cached map" : "re-rendered every frame (`--shadow-always`)");
    fprintf(f, "| Effects | volFog %s, fog %s, SSAO %s, SSIL %s, bloom %s |\n",
            env->volumetricFog.enabled ? "on" : "off", env->fog.mode ? "on" : "off",
            env->ssao.enabled ? "on" : "off", env->ssil.enabled ? "on" : "off", env->bloom.mode ? "on" : "off");
    fprintf(f, "| Vsync | app off; `__GL_SYNC_TO_VBLANK=%s`, `vblank_mode=%s` |\n",
            vsyncEnv ? vsyncEnv : "(unset)", vblankEnv ? vblankEnv : "(unset)");
    fprintf(f, "| Window | %s |\n", headless ? "headless: hidden window, offscreen target, glFinish per frame (no presentation)"
                                             : "visible (includes presentation)");
    fprintf(f, "| Method | per viewpoint: %d warmup frames, then %d timed frames while the camera spins 360 deg |\n",
            BENCH_WARMUP, benchFrames);
    fprintf(f, "| Command |");
    for (int i = 0; i < argc; i++) fprintf(f, " `%s`", argv[i]);
    fprintf(f, " |\n\n");

    fprintf(f, "## Results\n\n");
    fprintf(f, "| View | Avg ms | Avg FPS | 1%% low FPS | Worst ms | Shadow updates |\n|---|---:|---:|---:|---:|---:|\n");
    int allUpdates = 0;
    for (int v = 0; v < VIEWPOINT_COUNT; v++) {
        fprintf(f, "| %s | %.3f | %.1f | %.1f | %.2f | %d / %d |\n", VIEWPOINTS[v].name,
                stats[v].avgMs, stats[v].avgFps, stats[v].lowFps, stats[v].worstMs, shadowUpdates[v], benchFrames);
        allUpdates += shadowUpdates[v];
    }
    fprintf(f, "| **All** | **%.3f** | **%.1f** | | | %d / %d |\n", allMs, allFps, allUpdates, frames);
    fclose(f);

    printf("report: %s\n", path);
    fflush(stdout);
}

// #endregion
// ======================================================================================
// #region Main
// ======================================================================================

int main(int argc, char **argv)
{
    InitDataPaths();

    // Player starts at the dark end of the entry canyon, facing the hall
    Player player = { .feet = { 0, 0, 90 }, .yaw = PI };

    const char *shotPath = NULL;
    const char *offKeys = "";
    int startView = 0;
    int shadowSize = 8192;
    float shadowRange = 100.0f;
    bool bench = false;
    bool headless = false;      // --bench without a visible window
    bool shadowReuse = true;    // see SunShadowNeedsUpdate()
    const char *benchNote = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shotPath = argv[++i];
        else if (strcmp(argv[i], "--off") == 0 && i + 1 < argc) offKeys = argv[++i];
        else if (strcmp(argv[i], "--view") == 0 && i + 1 < argc) startView = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bench") == 0) bench = true;
        else if (strcmp(argv[i], "--headless") == 0) headless = true;
        else if (strcmp(argv[i], "--shadow-always") == 0) shadowReuse = false;
        else if (strcmp(argv[i], "--bench-frames") == 0 && i + 1 < argc)
            benchFrames = (int)Clamp((float)atoi(argv[++i]), 10.0f, (float)BENCH_FRAMES);
        else if (strcmp(argv[i], "--note") == 0 && i + 1 < argc) benchNote = argv[++i];
        else if (strcmp(argv[i], "--shadow") == 0 && i + 2 < argc) {
            shadowSize = atoi(argv[i + 1]);
            shadowRange = atof(argv[i + 2]);
            i += 2;
        }
        else if (strcmp(argv[i], "--pos") == 0 && i + 3 < argc) {
            player.feet = (Vector3){ atof(argv[i + 1]), atof(argv[i + 2]), atof(argv[i + 3]) };
            i += 3;
        } else if (strcmp(argv[i], "--look") == 0 && i + 2 < argc) {
            player.yaw = atof(argv[i + 1]) * DEG2RAD;
            player.pitch = atof(argv[i + 2]) * DEG2RAD;
            i += 2;
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
    InitWindow(headlessBench ? 1920 : 1600, headlessBench ? 1006 : 900,
               bench ? "monolith-bench" : shotPath ? "monolith-shot" : "monolith");
    SetTargetFPS(0);

    // Sun shadow resolution. R3D fits one directional shadow map around the whole
    // camera frustum out to sun.range (a bounding sphere, radius ~1.5x range at
    // 70 deg fov / 16:9), so texel size = 2 * radius / shadow map size.
    // range 100 m @ 8192 px -> ~3.7 cm texels (was 140 m @ 4096 -> ~10.7 cm).
    // VRAM: 16-bit depth, R3D allocates 2 layers -> 8192^2 * 2 B * 2 = 256 MB.
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
    BuildWorld();
    TraceLog(LOG_INFO, "monolith: %d boxes", world.count);

    R3D_Mesh cube = R3D_GenMeshCube(1.0f, 1.0f, 1.0f);
    R3D_InstanceBuffer instances = R3D_LoadInstanceBuffer(world.count, R3D_INSTANCE_POSITION | R3D_INSTANCE_SCALE);
    {
        Vector3 *pos = R3D_MapInstances(instances, R3D_INSTANCE_POSITION, false);
        for (int i = 0; i < world.count; i++)
            pos[i] = Vector3Scale(Vector3Add(world.items[i].min, world.items[i].max), 0.5f);
        R3D_UnmapInstances(instances, R3D_INSTANCE_POSITION);

        Vector3 *scl = R3D_MapInstances(instances, R3D_INSTANCE_SCALE, false);
        for (int i = 0; i < world.count; i++)
            scl[i] = Vector3Subtract(world.items[i].max, world.items[i].min);
        R3D_UnmapInstances(instances, R3D_INSTANCE_SCALE);
    }

    R3D_Material concrete = R3D_GetDefaultMaterial();
    concrete.shader = R3D_LoadSurfaceShader(TextFormat("%sshaders/concrete.glsl", resourcesDir));
    float patternScale = 1.0f;
    R3D_SetSurfaceShaderUniform(concrete.shader, "u_scale", &patternScale);

    // --- Atmosphere
    R3D_Cubemap sky;
    R3D_AmbientMap ambient;
    SetupEnvironment(&sky, &ambient);

    R3D_Light sun = R3D_CreateDirLight(SUN_DIR, (Color){ 255, 238, 214, 255 }, 2.0f);
    // Shadow distance. Shadows fade out between 75% and 100% of it (R3D), and
    // the distance fog already hides most of what lies beyond ~100 m. It also
    // must cover volumetricFog.length (90 m) or the light shafts fade early.
    sun.range = shadowRange;
    sun.fogEnergy = 4.0f;    // how strongly the sun lights the volumetric fog
    R3D_ShadowMap sunShadow = R3D_LoadShadowMap(R3D_LIGHT_DIR);
    sunShadow.softness = 2.0f;
    bool sunOn = true;
    SunShadowCache sunCache = { 0 };

    bool hud = true;

    Spectator spec = { .speed = 12.0f, .view = -1 };
    if (startView >= 1 && startView <= VIEWPOINT_COUNT) {
        spec.active = true;
        SpectatorJumpTo(&spec, startView - 1);
    }

    if (!hidden) DisableCursor();
    RenderTexture shotTarget = { 0 };   // zero = draw to the window
    if (hidden) shotTarget = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
    int frame = 0;

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
    }
    bool offApplied = false;  // --off toggles are applied on the first frame only

    while (!WindowShouldClose())
    {
        float dt = fminf(GetFrameTime(), 1.0f / 20.0f);

        if (IsWindowResized()) R3D_SetResolution(GetScreenWidth(), GetScreenHeight());

        // Effect toggles, to see what each one contributes
        R3D_Environment *env = R3D_GetEnvironment();
        bool first = !offApplied;
        #define TOGGLE(key, ch) (IsKeyPressed(key) || (first && strchr(offKeys, ch)))
        if (TOGGLE(KEY_ONE, '1'))   env->volumetricFog.enabled = !env->volumetricFog.enabled;
        if (TOGGLE(KEY_TWO, '2'))   env->fog.mode = env->fog.mode ? R3D_FOG_DISABLED : R3D_FOG_EXP2;
        if (TOGGLE(KEY_THREE, '3')) env->ssao.enabled = !env->ssao.enabled;
        if (TOGGLE(KEY_FOUR, '4'))  env->ssil.enabled = !env->ssil.enabled;
        if (TOGGLE(KEY_FIVE, '5'))  env->bloom.mode = env->bloom.mode ? R3D_BLOOM_DISABLED : R3D_BLOOM_MIX;
        if (TOGGLE(KEY_SIX, '6'))   sunOn = !sunOn;
        #undef TOGGLE
        offApplied = true;
        if (IsKeyPressed(KEY_F1))    hud = !hud;
        if (IsKeyPressed(KEY_F12))   TakeScreenshot("screenshot.png");

        if (IsKeyPressed(KEY_TAB)) {
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

        if (bench) {
            double now = GetTime();
            if (benchFrame >= 0 && benchFrame < benchFrames) benchTimes[benchView][benchFrame] = now - benchLast;
            benchLast = now;
            if (++benchFrame == benchFrames) {
                if (++benchView == VIEWPOINT_COUNT) break;
                SpectatorJumpTo(&spec, benchView);
                benchFrame = -BENCH_WARMUP;
            }
            float baseYaw, basePitch;
            LookAngles(VIEWPOINTS[benchView].eye, VIEWPOINTS[benchView].target, &baseYaw, &basePitch);
            spec.yaw = baseYaw + 2.0f * PI * (float)(benchFrame > 0 ? benchFrame : 0) / benchFrames;
        }
        else if (!shotPath) {
            if (spec.active) UpdateSpectator(&spec, &player, dt);
            else UpdatePlayer(&player, dt);
        }
        Camera3D camera = spec.active ? SpectatorCamera(&spec) : PlayerCamera(&player);

        BeginDrawing();
            ClearBackground(BLACK);

            // A hidden window has no visible pixels, so reading back the default
            // framebuffer is undefined: render --shot frames into a texture instead.
            R3D_BeginPro((R3D_View){ .camera = R3D_CameraFromRL(camera), .target = shotTarget });
                if (sunOn) {
                    // Only re-render the shadow map when the cached one no longer covers the view
                    float aspect = hidden ? (float)shotTarget.texture.width / shotTarget.texture.height
                                            : (float)GetRenderWidth() / GetRenderHeight();
                    bool update = !shadowReuse ||
                        SunShadowNeedsUpdate(&sunCache, sun.direction, shadowRange, camera, aspect, shadowSize);
                    if (bench && update && benchFrame >= 0 && benchFrame < benchFrames) benchShadowUpdates[benchView]++;
                    R3D_PushLightEx(sun, sunShadow, update);
                }
                R3D_DrawMeshInstanced(cube, concrete, instances, world.count);
            R3D_End();

            if (hud && !shotPath) {
                DrawText(TextFormat("%d fps  |  %s  |  1 volFog:%s  2 fog:%s  3 ssao:%s  4 ssil:%s  5 bloom:%s  6 sun:%s",
                         GetFPS(), spec.active ? "SPECTATOR" : player.fly ? "FLY" : "WALK",
                         env->volumetricFog.enabled ? "on" : "off", env->fog.mode ? "on" : "off",
                         env->ssao.enabled ? "on" : "off", env->ssil.enabled ? "on" : "off",
                         env->bloom.mode ? "on" : "off", sunOn ? "on" : "off"),
                         12, 10, 16, (Color){ 230, 230, 230, 160 });

                if (spec.active) {
                    DrawText(TextFormat("speed %.1f m/s (wheel)  |  <- -> viewpoints  |  P copy view  |  G drop player  |  TAB exit",
                             spec.speed), 12, 32, 16, (Color){ 230, 230, 230, 120 });
                    if (spec.messageTime > 0.0f) {
                        float a = fminf(spec.messageTime, 1.0f);
                        int w = MeasureText(spec.message, 24);
                        DrawText(spec.message, (GetScreenWidth() - w) / 2, GetScreenHeight() - 70, 24,
                                 Fade((Color){ 235, 232, 225, 255 }, a));
                    }
                }
            }
        EndDrawing();
        // A hidden window presents nothing, so swaps don't pace the frames; wait for the
        // GPU instead, or the bench would time how fast commands are queued.
        if (headlessBench) glFinish();

        if (shotPath && ++frame == 30) {
            Image img = LoadImageFromTexture(shotTarget.texture);
            ImageFlipVertical(&img);                                // GL textures are bottom-up
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);     // drop alpha
            if (!ExportImage(img, shotPath)) TraceLog(LOG_ERROR, "monolith: failed to save %s", shotPath);
            UnloadImage(img);
            break;
        }
    }

    if (bench && benchView == VIEWPOINT_COUNT)
        WriteBenchReport(benchTimes, benchShadowUpdates, shadowSize, shadowRange, shadowReuse, headless, benchNote, argc, argv);

    if (hidden) UnloadRenderTexture(shotTarget);
    R3D_UnloadSurfaceShader(concrete.shader);
    R3D_UnloadInstanceBuffer(instances);
    R3D_UnloadMesh(cube);
    R3D_UnloadShadowMap(sunShadow);
    R3D_UnloadAmbientMap(ambient);
    R3D_UnloadCubemap(sky);
    R3D_Close();
    CloseWindow();
    return 0;
}

// #endregion
