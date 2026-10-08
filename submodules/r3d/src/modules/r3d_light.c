/* r3d_light.c -- Internal R3D light module.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#include "./r3d_light.h"

#include <r3d/r3d_frustum.h>
#include <r3d_config.h>
#include <raymath.h>
#include <stdint.h>
#include <string.h>
#include <float.h>

#include "../common/r3d_helper.h"
#include "../r3d_core_state.h"

// ========================================
// CONSTANTS
// ========================================

#define SHADOW_DIR_LAYER_GROWTH     2
#define SHADOW_SPOT_LAYER_GROWTH    4
#define SHADOW_OMNI_LAYER_GROWTH    4

// ========================================
// MODULE STATE
// ========================================

struct r3d_light R3D_MOD_LIGHT;

// ========================================
// SHADOW ARRAY FUNCTIONS
// ========================================

static void shadow_array_allocate_texture(GLuint texture, GLenum target, int size, uint32_t layers)
{
    int actualLayers = (target == GL_TEXTURE_CUBE_MAP_ARRAY) ? (int)layers * 6 : (int)layers;

    glBindTexture(target, texture);
    glTexImage3D(
        target, 0, GL_DEPTH_COMPONENT16, size, size, actualLayers,
        0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, NULL
    );

    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (target == GL_TEXTURE_CUBE_MAP_ARRAY)
    {
        glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }

    glTexParameteri(target, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(target, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    glBindTexture(target, 0);
}

static r3d_light_shadow_array_t shadow_array_create(GLenum target, int size, int growth)
{
    r3d_light_shadow_array_t arr = {0};

    arr.target   = target;
    arr.size     = size;
    arr.growth   = growth;
    arr.freeList = R3D_LIST_CREATE(int, 16);
    arr.cache    = R3D_LIST_CREATE(r3d_light_shadow_cache_t, 16);

    glGenFramebuffers(1, &arr.framebuffer);

    return arr;
}

static void shadow_array_destroy(r3d_light_shadow_array_t* arr)
{
    if (arr->texture != 0)     glDeleteTextures(1, &arr->texture);
    if (arr->framebuffer != 0) glDeleteFramebuffers(1, &arr->framebuffer);

    R3D_LIST_DESTROY(arr->cache);
    R3D_LIST_DESTROY(arr->freeList);

    *arr = (r3d_light_shadow_array_t){0};
}

static bool shadow_array_expand(r3d_light_shadow_array_t* arr, uint32_t growth)
{
    uint32_t newLayerCount = arr->layerCount + growth;

    GLuint newTexture;
    glGenTextures(1, &newTexture);
    shadow_array_allocate_texture(newTexture, arr->target, arr->size, newLayerCount);

    // Copy existing content into the new texture if any
    if (arr->layerCount > 0 && arr->texture != 0)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, arr->framebuffer);

        int facesPerLayer = (arr->target == GL_TEXTURE_CUBE_MAP_ARRAY) ? 6 : 1;
        for (uint32_t layer = 0; layer < arr->layerCount; layer++)
        {
            for (int face = 0; face < facesPerLayer; face++)
            {
                int layerFace = (int)layer * facesPerLayer + face;
                glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, arr->texture, 0, layerFace);
                glBindTexture(arr->target, newTexture);
                glCopyTexSubImage3D(arr->target, 0, 0, 0, layerFace, 0, 0, arr->size, arr->size);
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(arr->target, 0);
    }

    if (arr->texture != 0)
    {
        glDeleteTextures(1, &arr->texture);
    }
    arr->texture = newTexture;

    // Resize the cache (new entries zero-initialized => valid = false)
    R3D_LIST_RESIZE(arr->cache, newLayerCount);

    // Newly allocated layers become available
    for (uint32_t layer = arr->layerCount; layer < newLayerCount; layer++)
    {
        int l = (int)layer;
        R3D_LIST_PUSH(arr->freeList, l);
    }

    arr->layerCount = newLayerCount;

    return true;
}

static int shadow_array_acquire_layer(r3d_light_shadow_array_t* arr)
{
    if (R3D_LIST_EMPTY(arr->freeList))
    {
        if (!shadow_array_expand(arr, (uint32_t)arr->growth))
        {
            return -1;
        }
    }

    int layer = -1;
    R3D_LIST_POP(arr->freeList, &layer);

    r3d_light_shadow_cache_t* cache = &R3D_LIST_GET(arr->cache, r3d_light_shadow_cache_t, layer);
    cache->acquired = true;
    cache->rendered = false;

    return layer;
}

static void shadow_array_release_layer(r3d_light_shadow_array_t* arr, int layer)
{
    r3d_light_shadow_cache_t* cache = &R3D_LIST_GET(arr->cache, r3d_light_shadow_cache_t, layer);
    cache->acquired = false;
    cache->rendered = false;

    R3D_LIST_PUSH(arr->freeList, layer);
}

// ========================================
// LIGHT FUNCTIONS
// ========================================

static Matrix light_dir_view_proj(Vector3 dir, float range, R3D_Camera camera, double aspect)
{
    float camNear   = range / 1000.0f;
    float camFar    = range;
    float camFovy   = (float)camera.fovy;
    float camAspect = (float)aspect;

    float farH      = camFar * tanf(camFovy * (DEG2RAD * 0.5f));
    float halfDepth = (camFar - camNear) * 0.5f;
    float radius    = sqrtf(farH * farH * (1.0f + camAspect * camAspect) + halfDepth * halfDepth);

    Vector3 forward       = R3D_GetCameraForward(camera);
    Vector3 frustumCenter = Vector3Add(camera.position, Vector3Scale(forward, (camNear + camFar) * 0.5f));

    float ax = fabsf(dir.x);
    float ay = fabsf(dir.y);
    float az = fabsf(dir.z);

    Vector3 up         = (ax <= ay && ax <= az) ? (Vector3){1,0,0} : (ay <= az) ? (Vector3){0,1,0} : (Vector3){0,0,1};
    Vector3 lightRight = Vector3Normalize(Vector3CrossProduct(up, dir));
    Vector3 lightUp    = Vector3CrossProduct(dir, lightRight);

    float texelSize = (radius * 2.0f) / (float)R3D_HINT(R3D_HINT_SHADOW_DIR_SIZE);
    float cx = floorf(Vector3DotProduct(frustumCenter, lightRight) / texelSize) * texelSize;
    float cy = floorf(Vector3DotProduct(frustumCenter, lightUp) / texelSize) * texelSize;
    float cz = Vector3DotProduct(frustumCenter, dir);

    Vector3 snappedCenter = Vector3Add(
        Vector3Add(
            Vector3Scale(lightRight, cx),
            Vector3Scale(lightUp, cy)
        ),
        Vector3Scale(dir, cz)
    );

    const float zExtension = 100.0f; // Extent to capture objects behind the camera
    Vector3 eye = Vector3Subtract(snappedCenter, Vector3Scale(dir, radius + zExtension));
    Matrix view = MatrixLookAt(eye, snappedCenter, lightUp);

    float near = 0.0f;
    float far  = zExtension + radius * 2.0f;

    Matrix proj = MatrixOrtho(-radius, radius, -radius, radius, near, far);

    return MatrixMultiply(view, proj);
}

static Matrix light_spot_view_proj(Vector3 pos, Vector3 dir, float range)
{
    float near = 0.05f;
    float far  = range;

    Vector3 up  = {0, 1, 0};
    float upDot = fabsf(Vector3DotProduct(dir, up));
    if (upDot > 0.99f) up = (Vector3){1, 0, 0};

    Matrix view = MatrixLookAt(pos, Vector3Add(pos, dir), up);
    Matrix proj = MatrixPerspective(90 * DEG2RAD, 1.0, near, far);

    return MatrixMultiply(view, proj);
}

static void light_omni_view_proj(Vector3 pos, float range, Matrix* outMatrices, float* outFar)
{
    static const Vector3 DIRS[6] = {
        {  1.0,  0.0,  0.0 }, { -1.0,  0.0,  0.0 },
        {  0.0,  1.0,  0.0 }, {  0.0, -1.0,  0.0 },
        {  0.0,  0.0,  1.0 }, {  0.0,  0.0, -1.0 }
    };

    static const Vector3 UPS[6] = {
        {  0.0, -1.0,  0.0 }, {  0.0, -1.0,  0.0 },
        {  0.0,  0.0,  1.0 }, {  0.0,  0.0, -1.0 },
        {  0.0, -1.0,  0.0 }, {  0.0, -1.0,  0.0 }
    };

    float near = 0.05f;
    float far  = range;

    Matrix proj = MatrixPerspective(90 * DEG2RAD, 1.0, near, far);

    for (int face = 0; face < 6; face++)
    {
        Vector3 target = Vector3Add(pos, DIRS[face]);
        Matrix view = MatrixLookAt(pos, target, UPS[face]);
        outMatrices[face] = MatrixMultiply(view, proj);
    }

    *outFar = far;
}

static r3d_light_volume_t light_spot_volume(Vector3 pos, Vector3 dir, float range, float outerCos)
{
    r3d_light_volume_t volume = {0};

    float tanTheta2 = (1.0f - outerCos * outerCos) / (outerCos * outerCos);

    if (outerCos >= 0.70710678f)
    {
        volume.radius = range * (1.0f + tanTheta2) * 0.5f;
        volume.center = Vector3Add(pos, Vector3Scale(dir, volume.radius));
    }
    else
    {
        volume.radius = range * sqrtf(tanTheta2);
        volume.center = Vector3Add(pos, Vector3Scale(dir, range));
    }

    return volume;
}

static void light_volume_sphere_axis_extent(float centerAxis, float f, float radius, float projTerm, float* ndcMin, float* ndcMax)
{
    float L2 = centerAxis * centerAxis + f * f;
    float r2 = radius * radius;

    // Origin inside the circle -> spans the full angular range on this axis
    if (L2 <= r2)
    {
        *ndcMin = -1.0f;
        *ndcMax = +1.0f;
        return;
    }

    // Tangent length from origin to the circle
    float D = sqrtf(L2 - r2);

    // tan(theta +- alpha) via angle-sum identity, trig-free.
    // Denominator sign = forward/backward side; if non-positive, the tangent
    // points sideways/behind -> snap to the edge matching the numerator's sign.
    float numMin = centerAxis * D - f * radius;
    float denMin = f * D + centerAxis * radius;
    *ndcMin = (denMin > 0.0f)
        ? R3D_MAX(projTerm * (numMin / denMin), -1.0f)
        : ((numMin > 0.0f) ? +1.0f : -1.0f);

    float numMax = centerAxis * D + f * radius;
    float denMax = f * D - centerAxis * radius;
    *ndcMax = (denMax > 0.0f)
        ? R3D_MIN(projTerm * (numMax / denMax), +1.0f)
        : ((numMax > 0.0f) ? +1.0f : -1.0f);
}

static bool light_volume_screen_ndc(const r3d_light_volume_t* volume, Vector2* minNdc, Vector2* maxNdc)
{
    Vector3 camPos = R3D.viewState.camera.position;
    Vector3 center = volume->center;
    float radius = volume->radius;

    // Camera inside sphere -> covers the whole screen
    float distSq = Vector3DistanceSqr(camPos, center);
    if (distSq <= radius * radius)
    {
        *minNdc = (Vector2){-1, -1};
        *maxNdc = (Vector2){+1, +1};
        return true;
    }

    // Light volume center in view space
    Vector3 vsCenter = r3d_vector3_transform(center, &R3D.viewState.view);
    vsCenter.z = -vsCenter.z; // positive distance in front of camera

    // Sphere entirely behind the camera -> not visible
    if (vsCenter.z + radius <= 0.0f)
    {
        return false;
    }

    float P00 = R3D.viewState.proj.m0;
    float P11 = R3D.viewState.proj.m5;

    float minX, maxX, minY, maxY;
    light_volume_sphere_axis_extent(vsCenter.x, vsCenter.z, radius, P00, &minX, &maxX);
    light_volume_sphere_axis_extent(vsCenter.y, vsCenter.z, radius, P11, &minY, &maxY);

    // Fully off-screen on at least one axis
    if (minX > 1.0f || maxX < -1.0f || minY > 1.0f || maxY < -1.0f)
    {
        return false;
    }

    minNdc->x = minX;
    minNdc->y = minY;
    maxNdc->x = maxX;
    maxNdc->y = maxY;

    return true;
}

static void light_dir_push(const R3D_Light* light, const R3D_ShadowMap* map, R3D_Camera camera, double aspect, bool updateShadow)
{
    r3d_light_data_t data = {
        .volume      = {.center = {0}, .radius = FLT_MAX},
        .minNdc      = {-1, -1},
        .maxNdc      = {+1, +1},
        .color       = r3d_color_srgb_to_linear_vec3(light->color),
        .direction   = Vector3Normalize(light->direction),
        .energy      = light->energy,
        .specular    = light->specular,
        .range       = light->range,
        .fogEnergy   = light->fogEnergy,
        .type        = light->type,
    };

    if (map && light->range > 0.0f)
    {
        int mapLayer = (int)map->handle - 1;

        r3d_light_shadow_cache_t* cache = r3d_light_shadow_cache(map->type, mapLayer);

        if (!cache->rendered || updateShadow)
        {
            cache->viewProj = light_dir_view_proj(data.direction, data.range, camera, aspect);
            cache->rendered = true; // Assume it will be rendered in advance

            r3d_light_shadow_job_t job = {
                .frustum     = R3D_ComputeFrustum(cache->viewProj),
                .viewProj    = cache->viewProj,
                .cullMask    = map->cullMask,
                .type        = light->type,
                .shadowLayer = mapLayer,
            };

            R3D_LIST_PUSH(R3D_MOD_LIGHT.listShadowJobs, job);
        }

        data.viewProj        = cache->viewProj;
        data.shadowSoftness  = map->softness / (float)R3D_HINT(R3D_HINT_SHADOW_DIR_SIZE);
        data.shadowOpacity   = map->opacity;
        data.shadowDepthBias = map->depthBias;
        data.shadowSlopeBias = map->slopeBias;
        data.shadowFar       = cache->far;
        data.shadowLayer     = mapLayer;
    }
    else
    {
        data.shadowLayer = -1;
    }

    R3D_LIST_PUSH(R3D_MOD_LIGHT.listLightData, data);
}

static void light_spot_push(const R3D_Light* light, const R3D_ShadowMap* map, const R3D_Frustum* frustum, bool updateShadow)
{
    float range = light->range;
    if (range <= 0.0f) return;

    Vector3 position  = light->position;
    Vector3 direction = Vector3Normalize(light->direction);
    float outerCutOff = cosf(light->outerCutOff * DEG2RAD);
    r3d_light_volume_t volume = light_spot_volume(position, direction, range, outerCutOff);

    r3d_light_shadow_cache_t* cache = NULL;
    bool mustRenderShadow = false;
    int  mapLayer = -1;

    if (map)
    {
        mapLayer = (int)map->handle - 1;
        cache = r3d_light_shadow_cache(map->type, mapLayer);
        mustRenderShadow = !cache->rendered || updateShadow;
    }

    if (mustRenderShadow)
    {
        cache->viewProj = light_spot_view_proj(position, direction, range);
        cache->rendered = true; // Assume it will be rendered in advance

        r3d_light_shadow_job_t job = {
            .frustum     = R3D_ComputeFrustum(cache->viewProj),
            .viewProj    = cache->viewProj,
            .cullMask    = map->cullMask,
            .type        = light->type,
            .shadowLayer = mapLayer,
        };

        R3D_LIST_PUSH(R3D_MOD_LIGHT.listShadowJobs, job);
    }

    if (R3D_FrustumIntersectsSphere(frustum, volume.center, volume.radius))
    {
        Vector2 minNdc, maxNdc;

        if (light_volume_screen_ndc(&volume, &minNdc, &maxNdc))
        {
            r3d_light_data_t data = {
                .volume      = volume,
                .minNdc      = minNdc,
                .maxNdc      = maxNdc,
                .color       = r3d_color_srgb_to_linear_vec3(light->color),
                .position    = position,
                .direction   = direction,
                .energy      = light->energy,
                .specular    = light->specular,
                .range       = range,
                .falloff     = R3D_MAX(light->falloff, 1e-4f),
                .innerCutOff = cosf(light->innerCutOff * DEG2RAD),
                .outerCutOff = outerCutOff,
                .fogEnergy   = light->fogEnergy,
                .type        = light->type,
            };

            if (map)
            {
                data.viewProj        = cache->viewProj;
                data.shadowSoftness  = map->softness / (float)R3D_HINT(R3D_HINT_SHADOW_SPOT_SIZE);
                data.shadowOpacity   = map->opacity;
                data.shadowDepthBias = map->depthBias;
                data.shadowSlopeBias = map->slopeBias;
                data.shadowFar       = cache->far;
                data.shadowLayer     = mapLayer;
            }
            else
            {
                data.shadowLayer = -1;
            }

            R3D_LIST_PUSH(R3D_MOD_LIGHT.listLightData, data);
        }
    }
}

static void light_omni_push(const R3D_Light* light, const R3D_ShadowMap* map, const R3D_Frustum* frustum, bool updateShadow)
{
    if (light->range <= 0.0f) return;

    r3d_light_shadow_cache_t* cache = NULL;
    bool mustRenderShadow = false;
    int  mapLayer = -1;

    if (map)
    {
        mapLayer = (int)map->handle - 1;
        cache = r3d_light_shadow_cache(map->type, mapLayer);
        mustRenderShadow = !cache->rendered || updateShadow;
    }

    if (mustRenderShadow)
    {
        cache->rendered = true; // Assume it will be rendered in advance

        Matrix viewProjs[6];
        light_omni_view_proj(light->position, light->range, viewProjs, &cache->far);

        for (int i = 0; i < 6; i++)
        {
            r3d_light_shadow_job_t job = {
                .frustum     = R3D_ComputeFrustum(viewProjs[i]),
                .viewProj    = viewProjs[i],
                .position    = light->position,
                .far         = cache->far,
                .cullMask    = map->cullMask,
                .type        = light->type,
                .shadowLayer = mapLayer,
                .layerFace   = i,
            };

            R3D_LIST_PUSH(R3D_MOD_LIGHT.listShadowJobs, job);
        }
    }

    if (R3D_FrustumIntersectsSphere(frustum, light->position, light->range))
    {
        r3d_light_volume_t volume = {
            .center = light->position,
            .radius = light->range,
        };

        Vector2 minNdc, maxNdc;

        if (light_volume_screen_ndc(&volume, &minNdc, &maxNdc))
        {
            r3d_light_data_t data = {
                .volume      = volume,
                .minNdc      = minNdc,
                .maxNdc      = maxNdc,
                .color       = r3d_color_srgb_to_linear_vec3(light->color),
                .position    = light->position,
                .energy      = light->energy,
                .specular    = light->specular,
                .range       = light->range,
                .falloff     = R3D_MAX(light->falloff, 1e-4f),
                .fogEnergy   = light->fogEnergy,
                .type        = light->type,
            };

            if (map)
            {
                data.shadowSoftness  = map->softness / (float)R3D_HINT(R3D_HINT_SHADOW_OMNI_SIZE);
                data.shadowOpacity   = map->opacity;
                data.shadowDepthBias = map->depthBias;
                data.shadowSlopeBias = map->slopeBias;
                data.shadowFar       = cache->far;
                data.shadowLayer     = mapLayer;
            }
            else
            {
                data.shadowLayer = -1;
            }

            R3D_LIST_PUSH(R3D_MOD_LIGHT.listLightData, data);
        }
    }
}

static bool light_check_shadow_validity(const R3D_Light* light, const R3D_ShadowMap* map)
{
    bool valid = true;

    if (!r3d_light_shadow_layer_is_valid(map->type, (int)map->handle - 1))
    {
        const char* mType = r3d_light_type_name(map->type);
        R3D_TRACELOG(LOG_WARNING, "Invalid pushed shadow map (type: %s | handle: %d)", mType, map->handle);

        valid = false;
    }
    else if (map->type != light->type)
    {
        const char* mType = r3d_light_type_name(map->type);
        const char* lType = r3d_light_type_name(light->type);
        R3D_TRACELOG(LOG_WARNING, "Incompatible pushed shadow map (type: %s) with given light (type: %s)", mType, lType);

        valid = false;
    }

    return valid;
}

// ========================================
// MODULE FUNCTIONS
// ========================================

bool r3d_light_init(void)
{
    memset(&R3D_MOD_LIGHT, 0, sizeof(R3D_MOD_LIGHT));

    R3D_MOD_LIGHT.shadowArrays[R3D_LIGHT_DIR]  = shadow_array_create(GL_TEXTURE_2D_ARRAY, R3D_HINT(R3D_HINT_SHADOW_DIR_SIZE), SHADOW_DIR_LAYER_GROWTH);
    R3D_MOD_LIGHT.shadowArrays[R3D_LIGHT_SPOT] = shadow_array_create(GL_TEXTURE_2D_ARRAY, R3D_HINT(R3D_HINT_SHADOW_SPOT_SIZE), SHADOW_SPOT_LAYER_GROWTH);
    R3D_MOD_LIGHT.shadowArrays[R3D_LIGHT_OMNI] = shadow_array_create(GL_TEXTURE_CUBE_MAP_ARRAY, R3D_HINT(R3D_HINT_SHADOW_OMNI_SIZE), SHADOW_OMNI_LAYER_GROWTH);

    R3D_MOD_LIGHT.listShadowJobs = R3D_LIST_CREATE(r3d_light_shadow_job_t, 32);
    R3D_MOD_LIGHT.listLightData  = R3D_LIST_CREATE(r3d_light_data_t, 256);

    return true;
}

void r3d_light_quit(void)
{
    for (int i = 0; i < R3D_LIGHT_TYPE_COUNT; i++)
    {
        shadow_array_destroy(&R3D_MOD_LIGHT.shadowArrays[i]);
    }

    R3D_LIST_DESTROY(R3D_MOD_LIGHT.listShadowJobs);
    R3D_LIST_DESTROY(R3D_MOD_LIGHT.listLightData);
}

void r3d_light_push(const R3D_Light* light, const R3D_ShadowMap* map, bool updateShadow)
{
    if (map && !light_check_shadow_validity(light, map))
    {
        map = NULL;
    }

    switch (light->type)
    {
    case R3D_LIGHT_DIR:
        light_dir_push(light, map, R3D.viewState.camera, R3D.viewState.aspect, updateShadow);
        break;

    case R3D_LIGHT_SPOT:
        light_spot_push(light, map, &R3D.viewState.frustum, updateShadow);
        break;

    case R3D_LIGHT_OMNI:
        light_omni_push(light, map, &R3D.viewState.frustum, updateShadow);
        break;

    default:
        break;
    }
}

r3d_light_data_t* r3d_light_get(int lightIndex)
{
    return &R3D_LIST_GET(R3D_MOD_LIGHT.listLightData, r3d_light_data_t, lightIndex);
}

void r3d_light_clear(void)
{
    R3D_LIST_CLEAR(R3D_MOD_LIGHT.listShadowJobs);
    R3D_LIST_CLEAR(R3D_MOD_LIGHT.listLightData);
}

r3d_rect_t r3d_light_screen_rect(const r3d_light_data_t* light, int w, int h)
{
    int rectX = (int)((light->minNdc.x * 0.5f + 0.5f) * w);
    int rectY = (int)((light->minNdc.y * 0.5f + 0.5f) * h);
    int rectW = (int)((light->maxNdc.x * 0.5f + 0.5f) * w) - rectX;
    int rectH = (int)((light->maxNdc.y * 0.5f + 0.5f) * h) - rectY;

    return (r3d_rect_t) {rectX, rectY, rectW, rectH};
}

const char* r3d_light_type_name(R3D_LightType type)
{
    switch (type)
    {
    case R3D_LIGHT_DIR:  return "Directional";
    case R3D_LIGHT_SPOT: return "Spot";
    case R3D_LIGHT_OMNI: return "Omni";
    default: break;
    }
    return NULL;
}

int r3d_light_acquire_shadow_layer(R3D_LightType type)
{
    return shadow_array_acquire_layer(&R3D_MOD_LIGHT.shadowArrays[type]);
}

void r3d_light_release_shadow_layer(R3D_LightType type, int layer)
{
    if (r3d_light_shadow_layer_is_valid(type, layer))
    {
        shadow_array_release_layer(&R3D_MOD_LIGHT.shadowArrays[type], layer);
    }
}

void r3d_light_bind_shadow_fbo(R3D_LightType type, int layer, int face)
{
    R3D_ASSERT((type == R3D_LIGHT_OMNI && face >= 0 && face < 6) || (type != R3D_LIGHT_OMNI && face == 0));

    r3d_light_shadow_array_t* arr = &R3D_MOD_LIGHT.shadowArrays[type];
    int stride = (type == R3D_LIGHT_OMNI) ? 6 : 1;

    glBindFramebuffer(GL_FRAMEBUFFER, arr->framebuffer);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, arr->texture, 0, layer * stride + face);
    glViewport(0, 0, arr->size, arr->size);
}

bool r3d_light_shadow_layer_is_valid(R3D_LightType type, int layer)
{
    r3d_light_shadow_array_t* arr = &R3D_MOD_LIGHT.shadowArrays[type];

    if (layer < 0 || (uint32_t)layer >= arr->layerCount)
    {
        return false;
    }

    r3d_light_shadow_cache_t* cache = &R3D_LIST_GET(arr->cache, r3d_light_shadow_cache_t, layer);
    return cache->acquired;
}

int r3d_light_shadow_map_size(R3D_LightType type)
{
    return R3D_MOD_LIGHT.shadowArrays[type].size;
}

GLuint r3d_light_shadow_map(R3D_LightType type)
{
    return R3D_MOD_LIGHT.shadowArrays[type].texture;
}

r3d_light_shadow_cache_t* r3d_light_shadow_cache(R3D_LightType type, int layer)
{
    R3D_ASSERT(layer >= 0);
    return &R3D_LIST_GET(R3D_MOD_LIGHT.shadowArrays[type].cache, r3d_light_shadow_cache_t, layer);
}
