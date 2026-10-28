// Collision with turned boxes (docs/live-editing.md 11.4, 11.12): the walking physics of
// src/player.c (PlayerStep) driven without input against a noGpu load of tests/rotated. The
// player walks up the 30 degree ramp onto the platform and stands on the slope, walks down it
// without leaving the ground, and stops at the 45 degree yawed wall's true face (its AABB would
// stop it more than 0.3 m earlier). Also MS_ColliderTop / MS_ColliderBottom on axis-aligned
// colliders (exactly max.y / min.y) and MS_Raycast's turned face normal.
//   tests/unit/collision_test.sh           (builds it with ASan + UBSan and runs it)
//   collision_test <tests/rotated>
#include "player.h"
#include <math.h>

MS_Scene *scene;
char benchmarkDir[1024];

// player.c reads the keys through the input shim: nothing is pressed here
bool InputIsKeyPressed(int key) { (void)key; return false; }
bool InputIsKeyDown(int key) { (void)key; return false; }
Vector2 InputGetMouseDelta(void) { return (Vector2){ 0 }; }

static int fails;

static void Check(bool ok, const char *what, double got, double want)
{
    printf("%s %-58s got %.5f, want %.5f\n", ok ? "ok  " : "FAIL", what, got, want);
    if (!ok) fails++;
}

// The ramp (tests/rotated: block 10 2.1268 -5 10 0.4 3 rot 0 0 30): its top surface's height at x
static double RampTop(double x)
{
    double c = 30.0 * 3.14159265358979323846 / 180.0, cy = 2.1268 + 0.2;
    // the top face: centre + 0.2 * (-sin, cos); along it (cos, sin)
    double x0 = 10.0 - 0.2 * sin(c), y0 = cy + 0.2 * cos(c);
    return y0 + (x - x0) * tan(c);
}

static void Steps(Player *p, Vector3 wish, int frames)
{
    for (int i = 0; i < frames; i++) PlayerStep(p, wish, false, false, 1.0f / 60.0f);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: collision_test <tests/rotated>\n"); return 2; }
    SetTraceLogLevel(LOG_WARNING);
    char err[512];
    scene = MS_Load(argv[1], &(MS_LoadOptions){ .noGpu = true }, err, sizeof(err));
    if (!scene) { fprintf(stderr, "collision_test: %s\n", err); return 1; }
    Check(scene->turnedBoxCount == 9, "turned boxes in tests/rotated", scene->turnedBoxCount, 9);

    // Axis-aligned colliders: exactly max.y / min.y, walkable
    int aaChecked = 0;
    for (int c = 0; c < scene->boxCount; c++) {
        if (scene->boxTurned[c] >= 0) continue;
        bool walk = false;
        BoundingBox b = scene->colliders[c];
        float top = MS_ColliderTop(scene, c, b, &walk), bottom = MS_ColliderBottom(scene, c, b);
        if (top != b.max.y || bottom != b.min.y || !walk) { fails++; printf("FAIL axis-aligned collider %d: %g %g\n", c, top, bottom); }
        aaChecked++;
    }
    Check(aaChecked == 3, "axis-aligned colliders: top = max.y, bottom = min.y", aaChecked, 3);

    // Up the ramp: from the spawn (feet 2 0 -5) toward +X onto the platform (top y 5 from x 14.3)
    Player p = { .feet = { 2.0f, 0.0f, -5.0f }, .grounded = true };
    Steps(&p, (Vector3){ 0 }, 10);
    double maxErr = 0.0;
    int airborne = 0;
    for (int i = 0; i < 600 && p.feet.x < 17.0f; i++) {
        PlayerStep(&p, (Vector3){ 1, 0, 0 }, false, false, 1.0f / 60.0f);
        double front = p.feet.x + PLAYER_RADIUS;      // the uphill edge of the footprint
        if (front > 5.8 && front < 14.2) {
            maxErr = fmax(maxErr, fabs(p.feet.y - RampTop(front)));
            airborne += !p.grounded;
        }
    }
    Check(p.feet.x >= 17.0f, "walked up the ramp onto the platform (feet x)", p.feet.x, 17.0);
    Check(fabsf(p.feet.y - 5.0f) < 1e-4f, "standing on the platform (feet y)", p.feet.y, 5.0);
    Check(maxErr < 0.003, "on the ramp: feet at the slope under the footprint (max error)", maxErr, 0.0);
    Check(airborne == 0, "on the ramp going up: frames off the ground", airborne, 0);

    // Down the ramp without leaving the ground
    airborne = 0;
    maxErr = 0.0;
    for (int i = 0; i < 600 && p.feet.x > 3.0f; i++) {
        PlayerStep(&p, (Vector3){ -1, 0, 0 }, false, false, 1.0f / 60.0f);
        double front = p.feet.x + PLAYER_RADIUS;
        if (front > 5.8 && front < 14.2) {
            maxErr = fmax(maxErr, fabs(p.feet.y - RampTop(front)));
            airborne += !p.grounded;
        }
    }
    Check(p.feet.x <= 3.0f && fabsf(p.feet.y) < 1e-4f, "walked down onto the ground (feet y)", p.feet.y, 0.0);
    Check(airborne == 0, "on the ramp going down: frames off the ground", airborne, 0);
    Check(maxErr < 0.003, "on the ramp going down: feet at the slope (max error)", maxErr, 0.0);

    // Stand on the slope: halfway up, then no input for two seconds
    p = (Player){ .feet = { 2.0f, 0.0f, -5.0f }, .grounded = true };
    for (int i = 0; i < 600 && p.feet.x < 9.0f; i++) PlayerStep(&p, (Vector3){ 1, 0, 0 }, false, false, 1.0f / 60.0f);
    Steps(&p, (Vector3){ 0 }, 60);       // (the glide stops)
    Vector3 still = p.feet;
    Steps(&p, (Vector3){ 0 }, 120);
    Check(p.grounded && Vector3Distance(still, p.feet) < 1e-4f, "standing still on the slope (drift)", Vector3Distance(still, p.feet), 0.0);
    Check(fabs(p.feet.y - RampTop(p.feet.x + PLAYER_RADIUS)) < 0.002, "standing on the slope (feet y)", p.feet.y, RampTop(p.feet.x + PLAYER_RADIUS));

    // The yawed wall (block 0 0 -15 6 3 0.4 rot 45 0 0): walking toward -Z at x = 0 the player's
    // corner (-0.3, zmin) meets its face where (p - c) . n = 0.2, n = (sin 45, 0, cos 45)
    p = (Player){ .feet = { 0.0f, 0.0f, -8.0f }, .grounded = true };
    Steps(&p, (Vector3){ 0, 0, -1 }, 400);
    double s45 = sqrt(0.5), contact = -15.0 + 0.2 / s45 + PLAYER_RADIUS + PLAYER_RADIUS;    // feet z at contact
    int wall = -1;
    for (int c = 0; c < scene->boxCount; c++)
        if (scene->boxTurned[c] >= 0 && fabsf(scene->turnedBoxes[scene->boxTurned[c]].rotationDeg.x - 45.0f) < 1e-6f) wall = c;
    double aabbStop = wall >= 0 ? scene->boxes[wall].max.z + PLAYER_RADIUS : 0.0;
    Check(p.feet.z >= contact - 1e-4 && p.feet.z <= contact + 0.06, "stopped at the yawed wall's true face (feet z)", p.feet.z, contact);
    Check(aabbStop - p.feet.z >= 0.3, "its AABB would have stopped it this much earlier (m)", aabbStop - p.feet.z, 0.3);
    Check(MS_FirstCollision(scene, (BoundingBox){ { p.feet.x - 0.3f, 0.01f, p.feet.z - 0.3f }, { p.feet.x + 0.3f, 1.75f, p.feet.z + 0.3f } }) < 0,
          "no overlap where it stopped", 0, 0);

    // Picking: the wall's face toward the player, normal (sin 45, 0, cos 45)
    MS_Hit h = MS_Raycast(scene, (Vector3){ 0, 1.5f, -8 }, (Vector3){ 0, 0, -1 }, 0.0f);
    Check(h.obj.kind == MS_OBJ_BOX && h.obj.index == wall, "raycast hits the yawed wall", h.obj.index, wall);
    Check(fabsf(h.normal.x - (float)s45) < 1e-5f && fabsf(h.normal.z - (float)s45) < 1e-5f && fabsf(h.normal.y) < 1e-6f,
          "raycast normal: the wall's turned face (x)", h.normal.x, s45);
    Check(fabsf(h.dist - (float)(7.0 - 0.2 / s45)) < 1e-4f, "raycast distance to the face", h.dist, 7.0 - 0.2 / s45);

    MS_Unload(scene);
    printf("collision_test: %s (%d failed)\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
