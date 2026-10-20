// Player: feet-based AABB, walking with collision, fly mode
#ifndef MONOLITH_PLAYER_H
#define MONOLITH_PLAYER_H

#include "common.h"

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

void UpdatePlayer(Player *p, float dt);
Camera3D PlayerCamera(const Player *p);

#endif
