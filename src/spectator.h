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

    MonoMsg message;        // short-lived HUD message (freed by SpectatorFree)
    float messageTime;
    bool holdMove;          // WASD don't move the camera (set per frame: edit mode with CTRL down,
                            // where CTRL+D duplicates)
    bool rmbFly;            // edit mode (set per frame): mouse look, WASD / E / Q / SPACE and the wheel
                            // only while RMB is held; G (move the player here) is the mode's grab
    bool mouseNav;          // draw mode (set per frame; docs/painting.md 18.7): no keyboard flight and
                            // no mouse look; the wheel moves toward / away from the point under the
                            // cursor (not with CTRL or SHIFT: the brush's), MMB drag orbits around the
                            // point under the cursor at the press, SPACE + LMB drag and SHIFT + MMB
                            // drag pan in the view plane. The arrows keep the viewpoints, P the pose
    // The navigation drag going on (mouseNav): 0 none, 1 orbit, 2 pan; its pivot (the scene point under
    // the cursor at the press, or a point at navDist along the ray when nothing is there)
    bool navBlocked;        // (set per frame) the cursor is on a panel: no wheel, no new drag (one
                            // going on goes on)
    int navDrag;
    Vector3 navPivot;
    float navDist;          // the last pivot's distance (the fallback depth over the sky)
} Spectator;

void LookAngles(Vector3 eye, Vector3 target, float *yaw, float *pitch);
Vector3 LookDir(float yaw, float pitch);
// The HUD message for 3 s: Static takes a literal only; f formats (text of any other lifetime:
// "%s", text)
void SpectatorMessageStatic(Spectator *s, const char *literal);
void SpectatorMessagef(Spectator *s, const char *fmt, ...) MSG_PRINTF(2, 3);
// Frees the HUD message (at exit)
void SpectatorFree(Spectator *s);
void SpectatorFlyTo(Spectator *s, int view);
// Smooth flight (as SpectatorFlyTo) to any eye position and view angles (radians)
void SpectatorFlyToPose(Spectator *s, Vector3 eye, float yaw, float pitch);
void SpectatorJumpTo(Spectator *s, int view);
// arrows: false while the arrow keys belong to something else (volume mode), so they
// don't fly to viewpoints. With mouseNav the camera moves only by the mouse (above)
void UpdateSpectator(Spectator *s, Player *player, float dt, bool arrows);
Camera3D SpectatorCamera(const Spectator *s);

#endif
