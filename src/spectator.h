// Spectator camera: free flight, preset viewpoints
#ifndef MONOLITH_SPECTATOR_H
#define MONOLITH_SPECTATOR_H

#include "common.h"
#include "player.h"

#define SPEC_MIN_SPEED  1.0f
#define SPEC_MAX_SPEED  400.0f
#define SPEC_FLY_TIME   1.6f    // seconds for a viewpoint transition

// The viewpoints are map data: scene->viewpoints[0 .. scene->viewpointCount - 1]

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
    bool holdMove;          // WASD don't move the camera (set per frame: edit mode with CTRL down,
                            // where CTRL+D duplicates)
} Spectator;

void LookAngles(Vector3 eye, Vector3 target, float *yaw, float *pitch);
Vector3 LookDir(float yaw, float pitch);
void SpectatorMessage(Spectator *s, const char *text);
void SpectatorFlyTo(Spectator *s, int view);
// Smooth flight (as SpectatorFlyTo) to any eye position and view angles (radians)
void SpectatorFlyToPose(Spectator *s, Vector3 eye, float yaw, float pitch);
void SpectatorJumpTo(Spectator *s, int view);
// arrows: false while the arrow keys belong to something else (volume mode), so they
// don't fly to viewpoints
void UpdateSpectator(Spectator *s, Player *player, float dt, bool arrows);
Camera3D SpectatorCamera(const Spectator *s);

#endif
