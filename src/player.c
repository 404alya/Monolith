#include "player.h"

// ======================================================================================
// #region Player
// ======================================================================================

static BoundingBox PlayerBox(Vector3 feet)
{
    return (BoundingBox){
        { feet.x - PLAYER_RADIUS, feet.y, feet.z - PLAYER_RADIUS },
        { feet.x + PLAYER_RADIUS, feet.y + PLAYER_HEIGHT, feet.z + PLAYER_RADIUS },
    };
}

// Returns the index of the first box overlapping the player at `feet`, or -1. The overlap is
// strict: touching faces do not count (raylib's CheckCollisionBoxes is inclusive, which would
// make standing on the ground block all movement)
static int Overlap(Vector3 feet)
{
    return MS_FirstCollision(scene, PlayerBox(feet));
}

// A turned box (a box / block line with `rot`) collides as its shape: the player stands on it a
// hair above its surface (exact float contact with a slope can read as a tiny overlap)
#define TURNED_LAND_GAP 0.0005f

static bool IsTurned(int c)
{
    return c >= 0 && c < scene->boxCount && scene->boxTurned && scene->boxTurned[c] >= 0;
}

// Moves along one horizontal axis, stepping up onto low obstacles (stairs, a walkable slope)
static void MoveHorizontal(Player *p, int axis, float delta)
{
    if (delta == 0.0f) return;

    Vector3 next = p->feet;
    if (axis == 0) next.x += delta; else next.z += delta;

    int hit = Overlap(next);
    if (hit < 0) { p->feet = next; return; }

    // The top of what was hit over the player's footprint (an axis-aligned box: its max.y). A
    // turned box is climbed only when walkable, or when all of it is a step high (rubble)
    bool walkable;
    float top = MS_ColliderTop(scene, hit, PlayerBox(next), &walkable);
    float stepUp = top - p->feet.y;
    bool climb = walkable || scene->colliders[hit].max.y - p->feet.y <= STEP_HEIGHT;
    if (p->grounded && climb && stepUp > 0.0f && stepUp <= STEP_HEIGHT) {
        Vector3 up = next;
        up.y = top + 0.001f;
        if (Overlap(up) < 0) { p->feet = up; return; }
    }
}

// Walking down a turned walkable box (a ramp): when the player was on the ground and nothing is
// under the feet after the move, the highest turned walkable box within STEP_HEIGHT below the
// feet takes them, so a slope is walked down instead of fallen down in hops. Axis-aligned boxes
// keep the fall (stairs as they always were).
static bool SnapDown(Player *p)
{
    BoundingBox probe = PlayerBox(p->feet);
    probe.max.y = p->feet.y;
    probe.min.y = p->feet.y - STEP_HEIGHT;
    for (int tries = 0; tries < 8; tries++) {
        int hit = MS_FirstCollision(scene, probe);
        if (hit < 0) return false;
        bool walkable;
        float top = MS_ColliderTop(scene, hit, probe, &walkable);
        // (a slope's top may lie a hair above the feet where it meets a ledge they leave)
        if (IsTurned(hit) && walkable && top <= p->feet.y + 0.01f) {
            Vector3 feet = { p->feet.x, top + TURNED_LAND_GAP, p->feet.z };
            if (Overlap(feet) >= 0) return false;
            p->feet = feet;
            return true;
        }
        // something else below: look again only above its top (a turned ramp may lie higher)
        if (!(top > probe.min.y) || top >= probe.max.y) return false;
        probe.min.y = top;
    }
    return false;
}

void PlayerStep(Player *p, Vector3 wish, bool run, bool jump, float dt)
{
    // Smoothly accelerate toward the wished velocity: heavy, slow, deliberate
    float speed = run ? RUN_SPEED : WALK_SPEED;
    float accel = p->grounded ? 10.0f : 2.0f;
    float t = 1.0f - expf(-accel * dt);
    p->vel.x = Lerp(p->vel.x, wish.x * speed, t);
    p->vel.z = Lerp(p->vel.z, wish.z * speed, t);
    // Stop the glide once it's invisible, so a standing player lets frame reuse kick in
    if (wish.x == 0.0f && wish.z == 0.0f && p->vel.x * p->vel.x + p->vel.z * p->vel.z < 1e-6f) p->vel.x = p->vel.z = 0.0f;

    bool wasGrounded = p->grounded, jumped = p->grounded && jump;
    if (jumped) p->vel.y = JUMP_SPEED;
    p->vel.y -= GRAVITY * dt;

    MoveHorizontal(p, 0, p->vel.x * dt);
    MoveHorizontal(p, 2, p->vel.z * dt);

    // Vertical: land on the top of what is under the footprint, or bump the head on its underside
    Vector3 next = p->feet;
    next.y += p->vel.y * dt;
    int hit = Overlap(next);
    p->grounded = false;
    if (hit >= 0) {
        if (p->vel.y < 0.0f) {
            next.y = MS_ColliderTop(scene, hit, PlayerBox(next), NULL);
            if (IsTurned(hit)) next.y += TURNED_LAND_GAP;
            p->grounded = true;
        } else {
            next.y = MS_ColliderBottom(scene, hit, PlayerBox(next)) - PLAYER_HEIGHT - 0.001f;
        }
        p->vel.y = 0.0f;
    }
    p->feet = next;
    if (!p->grounded && wasGrounded && !jumped && scene->turnedBoxCount > 0 && SnapDown(p)) {
        p->grounded = true;
        p->vel.y = 0.0f;
    }

    // Head bob, only while walking on the ground
    float hspeed = sqrtf(p->vel.x * p->vel.x + p->vel.z * p->vel.z);
    if (p->grounded) p->bobPhase += hspeed * dt * 2.2f;
}

void UpdatePlayer(Player *p, float dt)
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

    PlayerStep(p, wish, IsKeyDown(KEY_LEFT_SHIFT), p->grounded && IsKeyPressed(KEY_SPACE), dt);
}

Camera3D PlayerCamera(const Player *p)
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
