#include "spectator.h"

// ======================================================================================
// #region Spectator camera
// ======================================================================================

const Viewpoint VIEWPOINTS[VIEWPOINT_COUNT] = {
    { "Canyon",         {    0.0f,   1.6f,   88.0f }, {    0.0f,  4.0f,   40.0f } },
    { "Hypostyle hall", {   -6.0f,   1.6f,   34.0f }, {    6.0f,  6.0f,    0.0f } },
    { "Above the roof", {  -24.0f,  30.0f,   26.0f }, {    4.0f, 20.0f,    4.0f } },
    { "Stairs",         {    0.0f,   2.0f,  -25.0f }, {    0.0f, 10.0f,  -70.0f } },
    { "Terrace",        {    8.0f,  13.6f,  -70.0f }, {    0.0f, 30.0f, -100.0f } },
    { "Tower",          {   38.0f,  40.0f,  -55.0f }, {    0.0f, 55.0f, -100.0f } },
    { "Overview",       {   55.0f,  38.0f,   52.0f }, {    0.0f,  8.0f,  -10.0f } },
};
_Static_assert(sizeof(VIEWPOINTS) / sizeof(VIEWPOINTS[0]) == VIEWPOINT_COUNT, "update VIEWPOINT_COUNT");

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

void SpectatorMessage(Spectator *s, const char *text)
{
    snprintf(s->message, sizeof(s->message), "%s", text);
    s->messageTime = 3.0f;
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
    float yaw, pitch;
    LookAngles(VIEWPOINTS[view].eye, VIEWPOINTS[view].target, &yaw, &pitch);
    s->view = view;
    SpectatorFlyToPose(s, VIEWPOINTS[view].eye, yaw, pitch);
    SpectatorMessage(s, TextFormat("[%d/%d] %s", view + 1, VIEWPOINT_COUNT, VIEWPOINTS[view].name));
}

void SpectatorJumpTo(Spectator *s, int view)
{
    s->view = view;
    s->flying = false;
    s->vel = Vector3Zero();
    s->pos = VIEWPOINTS[view].eye;
    LookAngles(VIEWPOINTS[view].eye, VIEWPOINTS[view].target, &s->yaw, &s->pitch);
}

void UpdateSpectator(Spectator *s, Player *player, float dt)
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
                 IsKeyDown(KEY_E) || IsKeyDown(KEY_Q) || IsKeyDown(KEY_SPACE);

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
    // Not Ctrl: Ctrl+click selects a whole group, and the camera must not sink meanwhile
    if (IsKeyDown(KEY_Q)) wish.y -= 1.0f;
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
