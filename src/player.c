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

// Strict overlap: touching faces do not count (raylib's CheckCollisionBoxes is
// inclusive, which would make standing on the ground block all movement)
bool BoxesOverlap(BoundingBox a, BoundingBox b)
{
    return a.min.x < b.max.x && a.max.x > b.min.x &&
           a.min.y < b.max.y && a.max.y > b.min.y &&
           a.min.z < b.max.z && a.max.z > b.min.z;
}

// Returns the index of the first box overlapping the player at `feet`, or -1
static int Overlap(Vector3 feet)
{
    BoundingBox pb = PlayerBox(feet);
    for (int i = 0; i < scene->colliderCount; i++)
        if (BoxesOverlap(pb, scene->colliders[i])) return i;
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

    float stepUp = scene->colliders[hit].max.y - p->feet.y;
    if (p->grounded && stepUp > 0.0f && stepUp <= STEP_HEIGHT) {
        Vector3 up = next;
        up.y = scene->colliders[hit].max.y + 0.001f;
        if (Overlap(up) < 0) { p->feet = up; return; }
    }
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

    // Smoothly accelerate toward the wished velocity: heavy, slow, deliberate
    float speed = IsKeyDown(KEY_LEFT_SHIFT) ? RUN_SPEED : WALK_SPEED;
    float accel = p->grounded ? 10.0f : 2.0f;
    float t = 1.0f - expf(-accel * dt);
    p->vel.x = Lerp(p->vel.x, wish.x * speed, t);
    p->vel.z = Lerp(p->vel.z, wish.z * speed, t);
    // Stop the glide once it's invisible, so a standing player lets frame reuse kick in
    if (wish.x == 0.0f && wish.z == 0.0f && p->vel.x * p->vel.x + p->vel.z * p->vel.z < 1e-6f) p->vel.x = p->vel.z = 0.0f;

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
            next.y = scene->colliders[hit].max.y;
            p->grounded = true;
        } else {
            next.y = scene->colliders[hit].min.y - PLAYER_HEIGHT - 0.001f;
        }
        p->vel.y = 0.0f;
    }
    p->feet = next;

    // Head bob, only while walking on the ground
    float hspeed = sqrtf(p->vel.x * p->vel.x + p->vel.z * p->vel.z);
    if (p->grounded) p->bobPhase += hspeed * dt * 2.2f;
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
