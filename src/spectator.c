#include "spectator.h"

// ======================================================================================
// #region Spectator camera
// ======================================================================================

void LookAngles(Vector3 eye, Vector3 target, float *yaw, float *pitch)
{
    Vector3 d = Vector3Normalize(Vector3Subtract(target, eye));
    *yaw = atan2f(d.x, d.z);
    *pitch = asinf(Clamp(d.y, -1.0f, 1.0f));
}

Vector3 LookDir(float yaw, float pitch)
{
    return (Vector3){ cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw) };
}

void SpectatorMessageStatic(Spectator *s, const char *literal)
{
    MsgSetStatic(&s->message, literal);
    s->messageTime = 3.0f;
}

void SpectatorMessagef(Spectator *s, const char *fmt, ...)
{
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    MsgSetv(&s->message, fmt, arg_ptr);
    va_end(arg_ptr);
    s->messageTime = 3.0f;
}

void SpectatorFree(Spectator *s)
{
    MsgFree(&s->message);
}

void SpectatorFlyToPose(Spectator *s, Vector3 eye, float yaw, float pitch)
{
    s->flying = true;
    s->flyT = 0.0f;
    s->vel = Vector3Zero();
    s->fromPos = s->pos;
    s->fromYaw = s->yaw;
    s->fromPitch = s->pitch;
    s->toPos = eye;
    // Turn the short way around
    s->toYaw = s->fromYaw + Wrap(yaw - s->fromYaw, -PI, PI);
    s->toPitch = pitch;
}

void SpectatorFlyTo(Spectator *s, int view)
{
    int n = scene->viewpointCount;
    view = ((view % n) + n) % n;    // the map may have fewer viewpoints after a reload
    float yaw, pitch;
    LookAngles(scene->viewpoints[view].eye, scene->viewpoints[view].target, &yaw, &pitch);
    s->view = view;
    SpectatorFlyToPose(s, scene->viewpoints[view].eye, yaw, pitch);
    SpectatorMessagef(s, "[%d/%d] %s", view + 1, scene->viewpointCount, scene->viewpoints[view].name);
}

void SpectatorJumpTo(Spectator *s, int view)
{
    s->view = view;
    s->flying = false;
    s->vel = Vector3Zero();
    s->pos = scene->viewpoints[view].eye;
    LookAngles(scene->viewpoints[view].eye, scene->viewpoints[view].target, &s->yaw, &s->pitch);
}

// One frame of a viewpoint flight (s->flying)
static void FlightStep(Spectator *s, float dt)
{
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
}

// ======================================================================================
// #region Mouse navigation (draw mode, docs/painting.md 18.7)
// ======================================================================================

#define NAV_WHEEL_STEP  0.8f    // a notch toward the point: 80 % of its distance left (away: / 0.8)
#define NAV_MIN_DIST    0.05f
#define NAV_ORBIT_RATE  0.005f  // rad per pixel
#define NAV_SKY_DIST    10.0f   // the pivot's distance over the sky before any pivot was found

// The camera's basis (as SpectatorCamera: up is world up, right is screen right)
static void NavBasis(float yaw, float pitch, Vector3 *fwd, Vector3 *right, Vector3 *up)
{
    *fwd = LookDir(yaw, pitch);
    *right = (Vector3){ -cosf(yaw), 0.0f, sinf(yaw) };
    *up = Vector3CrossProduct(*right, *fwd);
}

// The scene point under the cursor (the ray from the eye through it), or over the sky the point at the
// last pivot's distance along that ray; *dist its distance along the ray (also the new last distance)
static Vector3 NavPoint(Spectator *s, Ray *ray, float *dist)
{
    *ray = GetScreenToWorldRayEx(GetMousePosition(), SpectatorCamera(s), GetScreenWidth(), GetScreenHeight());
    MS_Hit hit = MS_Raycast(scene, ray->position, ray->direction, 0.0f);
    if (s->navDist <= 0.0f) s->navDist = NAV_SKY_DIST;
    float d = hit.obj.kind != MS_OBJ_NONE ? hit.dist : s->navDist;
    s->navDist = d;
    *dist = d;
    return Vector3Add(ray->position, Vector3Scale(ray->direction, d));
}

// Wheel, MMB orbit, SPACE + LMB / SHIFT + MMB pan (no keyboard flight, no mouse look). True when the
// camera was moved by them this frame (that cancels a viewpoint flight)
static bool MouseNav(Spectator *s)
{
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool moved = false;

    // A drag ends with its button; a new one starts on a press off the panels
    if (s->navDrag == 1 && !IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) s->navDrag = 0;
    // (a SPACE + LMB pan ends when SPACE comes up too, as Aseprite's hand: the held LMB then does
    // nothing, a stroke starts only on a press)
    if (s->navDrag == 2 && !IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) && (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) || !IsKeyDown(KEY_SPACE)))
        s->navDrag = 0;
    if (s->navDrag == 0 && !s->navBlocked) {
        bool mmb = IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE);
        bool spaceLmb = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && IsKeyDown(KEY_SPACE);
        if (mmb || spaceLmb) {
            Ray ray;
            float d;
            s->navPivot = NavPoint(s, &ray, &d);
            s->navDrag = mmb && !shift ? 1 : 2;
            s->flying = false;
        }
    }

    Vector2 md = s->navDrag ? GetMouseDelta() : (Vector2){ 0, 0 };
    if (s->navDrag == 1 && (md.x != 0.0f || md.y != 0.0f)) {
        // Orbit: the pivot keeps its place in the camera's frame, so it stays where it was on screen
        Vector3 fwd, right, up;
        NavBasis(s->yaw, s->pitch, &fwd, &right, &up);
        Vector3 o = Vector3Subtract(s->navPivot, s->pos);
        float lx = Vector3DotProduct(o, right), ly = Vector3DotProduct(o, up), lz = Vector3DotProduct(o, fwd);
        s->yaw -= md.x * NAV_ORBIT_RATE;
        s->pitch = Clamp(s->pitch - md.y * NAV_ORBIT_RATE, -1.55f, 1.55f);
        NavBasis(s->yaw, s->pitch, &fwd, &right, &up);
        Vector3 local = Vector3Add(Vector3Add(Vector3Scale(right, lx), Vector3Scale(up, ly)), Vector3Scale(fwd, lz));
        s->pos = Vector3Subtract(s->navPivot, local);
        moved = true;
    } else if (s->navDrag == 2 && (md.x != 0.0f || md.y != 0.0f)) {
        // Pan in the view plane: the pivot follows the cursor (world per pixel at the pivot's depth)
        Vector3 fwd, right, up;
        NavBasis(s->yaw, s->pitch, &fwd, &right, &up);
        float depth = fmaxf(Vector3DotProduct(Vector3Subtract(s->navPivot, s->pos), fwd), NAV_MIN_DIST);
        float k = 2.0f * depth * tanf(SpectatorCamera(s).fovy * 0.5f * DEG2RAD) / (float)GetScreenHeight();
        Vector3 step = Vector3Subtract(Vector3Scale(up, md.y * k), Vector3Scale(right, md.x * k));
        s->pos = Vector3Add(s->pos, step);
        s->navPivot = Vector3Add(s->navPivot, step);    // (the same scene point, kept at the same depth)
        moved = true;
    }

    // Wheel: along the cursor's ray, toward the point under it by 20 % of its distance per notch, away
    // by 1 / 0.8 (CTRL / SHIFT + wheel are the brush's; over a panel the panel's)
    float wheel = !ctrl && !shift && !s->navBlocked ? GetMouseWheelMove() : 0.0f;
    if (wheel != 0.0f) {
        Ray ray;
        float d;
        NavPoint(s, &ray, &d);
        float nd = d * powf(NAV_WHEEL_STEP, wheel);
        if (wheel > 0.0f) nd = fmaxf(nd, fminf(d, NAV_MIN_DIST));     // (not closer than 5 cm)
        s->pos = Vector3Add(s->pos, Vector3Scale(ray.direction, d - nd));
        s->navDist = nd;
        moved = moved || nd != d;
    }
    return moved;
}

// #endregion

void UpdateSpectator(Spectator *s, Player *player, float dt, bool arrows)
{
    if (s->messageTime > 0.0f) s->messageTime -= dt;

    // Viewpoints: arrows cycle with a smooth flight
    if (arrows && IsKeyPressed(KEY_RIGHT)) SpectatorFlyTo(s, (s->view + 1) % scene->viewpointCount);
    if (arrows && IsKeyPressed(KEY_LEFT))  SpectatorFlyTo(s, (s->view <= 0 ? scene->viewpointCount : s->view) - 1);

    // Print the current view as command line arguments (for --shot and bookmarks)
    if (IsKeyPressed(KEY_P)) {
        const char *line = TextFormat("--pos %.2f %.2f %.2f --look %.1f %.1f",
                                      s->pos.x, s->pos.y - EYE_HEIGHT, s->pos.z,
                                      Wrap(s->yaw * RAD2DEG, 0.0f, 360.0f), s->pitch * RAD2DEG);
        printf("%s\n", line);
        fflush(stdout);
        SetClipboardText(line);
        SpectatorMessagef(s, "copied: %s", line);
    }

    // Draw mode: the mouse moves the camera (no keyboard flight, no mouse look, no G)
    if (s->mouseNav) {
        s->vel = Vector3Zero();
        if (MouseNav(s)) s->flying = false;
        else if (s->flying) FlightStep(s, dt);
        return;
    }
    s->navDrag = 0;

    // Edit mode: the camera flies only while RMB is held (the cursor is free otherwise)
    bool fly = !s->rmbFly || IsMouseButtonDown(MOUSE_BUTTON_RIGHT);

    // Speed: mouse wheel, exponential so it is equally quick at 2 m/s and 200 m/s
    float wheel = fly ? GetMouseWheelMove() : 0.0f;
    if (wheel != 0.0f) {
        s->speed = Clamp(s->speed * powf(1.25f, wheel), SPEC_MIN_SPEED, SPEC_MAX_SPEED);
        SpectatorMessagef(s, "speed %.1f m/s", s->speed);
    }

    // Drop the player at the current spot
    if (IsKeyPressed(KEY_G) && !s->rmbFly) {
        player->feet = (Vector3){ s->pos.x, s->pos.y - EYE_HEIGHT, s->pos.z };
        player->vel = Vector3Zero();
        player->yaw = s->yaw;
        player->pitch = s->pitch;
        SpectatorMessageStatic(s, "player moved here (TAB to walk)");
    }

    Vector2 md = fly ? GetMouseDelta() : (Vector2){ 0, 0 };
    bool input = fly && (md.x != 0.0f || md.y != 0.0f ||
                         IsKeyDown(KEY_W) || IsKeyDown(KEY_A) || IsKeyDown(KEY_S) || IsKeyDown(KEY_D) ||
                         IsKeyDown(KEY_E) || IsKeyDown(KEY_Q) || IsKeyDown(KEY_SPACE));

    if (s->flying) {
        // Any manual input cancels the flight
        if (input) s->flying = false;
        else {
            FlightStep(s, dt);
            return;
        }
    }

    s->yaw   -= md.x * 0.0022f;
    s->pitch -= md.y * 0.0022f;
    s->pitch  = Clamp(s->pitch, -1.55f, 1.55f);

    Vector3 fwd = LookDir(s->yaw, s->pitch);
    Vector3 right = { -cosf(s->yaw), 0, sinf(s->yaw) };

    Vector3 wish = Vector3Zero();
    if (!s->holdMove && fly) {
        if (IsKeyDown(KEY_W)) wish = Vector3Add(wish, fwd);
        if (IsKeyDown(KEY_S)) wish = Vector3Subtract(wish, fwd);
        if (IsKeyDown(KEY_D)) wish = Vector3Add(wish, right);
        if (IsKeyDown(KEY_A)) wish = Vector3Subtract(wish, right);
    }
    if (fly && (IsKeyDown(KEY_E) || IsKeyDown(KEY_SPACE))) wish.y += 1.0f;
    // Not Ctrl: Ctrl+click selects a whole group, and the camera must not sink meanwhile
    if (fly && IsKeyDown(KEY_Q)) wish.y -= 1.0f;
    wish = Vector3Normalize(wish);

    float speed = s->speed;
    if (IsKeyDown(KEY_LEFT_SHIFT)) speed *= 4.0f;
    if (IsKeyDown(KEY_LEFT_ALT))   speed *= 0.25f;

    // Damped velocity: glides to a stop instead of halting instantly
    float t = 1.0f - expf(-6.0f * dt);
    s->vel = Vector3Lerp(s->vel, Vector3Scale(wish, speed), t);
    // End the exponential glide once it's invisible (< 1 mm/s), so the camera really stops
    // and frame reuse can kick in instead of rendering sub-micron moves for seconds
    if (Vector3Length(wish) == 0.0f && Vector3Length(s->vel) < 1e-3f) s->vel = Vector3Zero();
    s->pos = Vector3Add(s->pos, Vector3Scale(s->vel, dt));
}

Camera3D SpectatorCamera(const Spectator *s)
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
