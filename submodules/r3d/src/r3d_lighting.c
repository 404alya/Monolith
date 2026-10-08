/* r3d_lighting.c -- R3D Lighting Module.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#include <r3d/r3d_lighting.h>
#include <r3d_config.h>
#include <raymath.h>
#include <stddef.h>
#include <math.h>
#include <rlgl.h>

#include "./modules/r3d_light.h"

// ========================================
// PUBLIC API
// ========================================

// ----------------------------------------
// Lights Config Functions
// ----------------------------------------

R3D_Light R3D_CreateDirLight(Vector3 dir, Color color, float energy)
{
    R3D_Light light = {0};

    light.position    = (Vector3) {0};
    light.direction   = Vector3Normalize(dir);
    light.color       = color;
    light.energy      = energy;
    light.specular    = 1.0f;
    light.range       = 50.0f;
    light.falloff     = 1.0f;
    light.innerCutOff = 0.0f;
    light.outerCutOff = 180.0f;
    light.fogEnergy   = 1.0f;
    light.type        = R3D_LIGHT_DIR;

    return light;
}

R3D_Light R3D_CreateSpotLight(Vector3 pos, Vector3 dir, float range, Color color, float energy)
{
    R3D_Light light = {0};

    light.position    = pos;
    light.direction   = Vector3Normalize(dir);
    light.color       = color;
    light.energy      = energy;
    light.specular    = 1.0f;
    light.range       = range;
    light.falloff     = 1.0f;
    light.innerCutOff = 22.5f;
    light.outerCutOff = 45.0f;
    light.fogEnergy   = 1.0f;
    light.type        = R3D_LIGHT_SPOT;

    return light;
}

R3D_Light R3D_CreateOmniLight(Vector3 pos, float range, Color color, float energy)
{
    R3D_Light light = {0};

    light.position    = pos;
    light.direction   = (Vector3) {0};
    light.color       = color;
    light.energy      = energy;
    light.specular    = 1.0f;
    light.range       = range;
    light.falloff     = 1.0f;
    light.innerCutOff = 0.0f;
    light.outerCutOff = 180.0f;
    light.fogEnergy   = 1.0f;
    light.type        = R3D_LIGHT_OMNI;

    return light;
}

// ----------------------------------------
// Shadow Config Functions
// ----------------------------------------

R3D_ShadowMap R3D_LoadShadowMap(R3D_LightType type)
{
    R3D_ShadowMap shadowMap = {0};

    int layer = r3d_light_acquire_shadow_layer(type);
    if (!r3d_light_shadow_layer_is_valid(type, layer))
    {
        R3D_TRACELOG(LOG_WARNING, "Failed to load shadow map (type: %s)", r3d_light_type_name(type));
        return shadowMap;
    }

    shadowMap.handle   = (uint32_t)(1 + layer);
    shadowMap.softness = 1.0f;
    shadowMap.opacity  = 1.0f;
    shadowMap.cullMask = R3D_LAYER_ALL;
    shadowMap.type     = type;

    switch (type)
    {
    case R3D_LIGHT_DIR:
        shadowMap.depthBias = 0.0001f;
        shadowMap.slopeBias = 0.0015f;
        break;

    case R3D_LIGHT_SPOT:
        shadowMap.depthBias = 0.0001f;
        shadowMap.slopeBias = 0.0003f;
        break;

    case R3D_LIGHT_OMNI:
        shadowMap.depthBias = 0.025f;
        shadowMap.slopeBias = 0.1f;
        break;

    default:
        break;
    }

    R3D_TRACELOG(LOG_INFO, "Shadow map loaded successfully (type: %s)", r3d_light_type_name(type));

    return shadowMap;
}

void R3D_UnloadShadowMap(R3D_ShadowMap shadowMap)
{
    r3d_light_release_shadow_layer(shadowMap.type, (int)shadowMap.handle - 1);

    R3D_TRACELOG(LOG_INFO, "Shadow map unloaded successfully (type: %s)", r3d_light_type_name(shadowMap.type));
}

bool R3D_IsShadowMapValid(R3D_ShadowMap shadowMap)
{
    return r3d_light_shadow_layer_is_valid(shadowMap.type, (int)shadowMap.handle - 1);
}

// ----------------------------------------
// Light Helper Functions
// ----------------------------------------

static void r3d_draw_light_dir_debug(const R3D_Light* light)
{
    // Arrow parameters
    const float ARROW_LENGTH  = 1.5f;
    const float ARROW_HEAD    = 0.3f;
    const float ARROW_SPREAD  = 0.5f;
    const int   GRID_HALF     = 1;     // [-1, 0, 1] -> 3x3 grid

    Vector3 dir = Vector3Normalize(light->direction);

    // Build orthonormal basis around direction
    Vector3 ref = (fabsf(dir.y) < 0.999f) ? (Vector3){0,1,0} : (Vector3){1,0,0};
    Vector3 right = Vector3Normalize(Vector3CrossProduct(dir, ref));
    Vector3 up = Vector3CrossProduct(right, dir);

    Vector3 origin = light->position;

    for (int i = -GRID_HALF; i <= GRID_HALF; i++)
    {
        for (int j = -GRID_HALF; j <= GRID_HALF; j++)
        {
            // Offset arrow origin on the plane perpendicular to direction
            Vector3 offset = Vector3Add(
                Vector3Scale(right, (float)i * ARROW_SPREAD),
                Vector3Scale(up, (float)j * ARROW_SPREAD)
            );
            Vector3 from = Vector3Add(origin, offset);
            Vector3 to = Vector3Add(from, Vector3Scale(dir, ARROW_LENGTH));

            // Arrow shaft
            DrawLine3D(from, to, light->color);

            // Arrow head; 4 lines forming a cross-cap
            Vector3 head_base = Vector3Add(from, Vector3Scale(dir, ARROW_LENGTH - ARROW_HEAD));
            DrawLine3D(head_base, Vector3Add(head_base, Vector3Scale(right, ARROW_HEAD * 0.5f)), light->color);
            DrawLine3D(head_base, Vector3Add(head_base, Vector3Scale(right, -ARROW_HEAD * 0.5f)), light->color);
            DrawLine3D(head_base, Vector3Add(head_base, Vector3Scale(up, ARROW_HEAD * 0.5f)), light->color);
            DrawLine3D(head_base, Vector3Add(head_base, Vector3Scale(up, -ARROW_HEAD * 0.5f)), light->color);
        }
    }
}

static void r3d_draw_light_spot_debug(const R3D_Light* light)
{
    const int SEGMENTS = 32;

    Vector3 pos = light->position;
    Vector3 dir = Vector3Normalize(light->direction);

    // Build orthonormal basis
    Vector3 ref = (fabsf(dir.y) < 0.999f) ? (Vector3) {0,1,0} : (Vector3) {1,0,0};
    Vector3 right = Vector3Normalize(Vector3CrossProduct(dir, ref));
    Vector3 up = Vector3CrossProduct(right, dir);

    float anglesDeg[2] = { light->innerCutOff, light->outerCutOff };

    for (int c = 0; c < 2; c++)
    {
        float thetaRad = anglesDeg[c] * DEG2RAD;
        float radius = light->range * tanf(thetaRad);
        Vector3 base = Vector3Add(pos, Vector3Scale(dir, light->range));

        // Ring
        rlBegin(RL_LINES);
        rlColor4ub(light->color.r, light->color.g, light->color.b, 255);
        const float step = (2.0f * PI) / SEGMENTS;
        for (int i = 0; i < SEGMENTS; i++)
        {
            float a1 = i * step, a2 = (i + 1) * step;
            Vector3 p1 = Vector3Add(base, Vector3Add(
                Vector3Scale(right, cosf(a1) * radius),
                Vector3Scale(up,    sinf(a1) * radius)));
            Vector3 p2 = Vector3Add(base, Vector3Add(
                Vector3Scale(right, cosf(a2) * radius),
                Vector3Scale(up,    sinf(a2) * radius)));
            rlVertex3f(p1.x, p1.y, p1.z);
            rlVertex3f(p2.x, p2.y, p2.z);
        }
        rlEnd();

        // 4 lines from apex to ring (cardinal points only)
        float ringAngles[4] = {0, PI * 0.5f, PI, PI * 1.5f};
        for (int i = 0; i < 4; i++)
        {
            Vector3 rim = Vector3Add(base, Vector3Add(
                Vector3Scale(right, cosf(ringAngles[i]) * radius),
                Vector3Scale(up,    sinf(ringAngles[i]) * radius)));
            DrawLine3D(pos, rim, light->color);
        }
    }

    // Small sphere at apex
    DrawSphereEx(pos, 0.05f, 4, 8, light->color);
}

static void r3d_draw_light_omni_debug(const R3D_Light* light)
{
    const int   SEGMENTS = 32;
    const float STEP     = (2.0f * PI) / SEGMENTS;

    Vector3 pos = light->position;
    float range = light->range;

    // 3 orthogonal circles (XY, XZ, YZ planes)
    rlBegin(RL_LINES);
    rlColor4ub(light->color.r, light->color.g, light->color.b, 255);
    for (int i = 0; i < SEGMENTS; i++)
    {
        float a1 = i * STEP, a2 = (i + 1) * STEP;
        float c1 = cosf(a1) * range, s1 = sinf(a1) * range;
        float c2 = cosf(a2) * range, s2 = sinf(a2) * range;

        // XY plane
        rlVertex3f(pos.x + c1, pos.y + s1, pos.z);
        rlVertex3f(pos.x + c2, pos.y + s2, pos.z);
        // XZ plane
        rlVertex3f(pos.x + c1, pos.y, pos.z + s1);
        rlVertex3f(pos.x + c2, pos.y, pos.z + s2);
        // YZ plane
        rlVertex3f(pos.x, pos.y + c1, pos.z + s1);
        rlVertex3f(pos.x, pos.y + c2, pos.z + s2);
    }
    rlEnd();

    // Small sphere at center
    DrawSphereEx(pos, 0.05f, 4, 8, light->color);
}

void R3D_DrawLightDebug(R3D_Light light)
{
    switch (light.type)
    {
    case R3D_LIGHT_DIR:
        r3d_draw_light_dir_debug(&light);
        break;
    case R3D_LIGHT_SPOT:
        r3d_draw_light_spot_debug(&light);
        break;
    case R3D_LIGHT_OMNI:
        r3d_draw_light_omni_debug(&light);
        break;
    default:
        break;
    }
}
