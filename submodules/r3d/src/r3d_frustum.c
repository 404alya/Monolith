/* r3d_frustum.c -- Common R3D Frustum Functions
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#include <r3d/r3d_frustum.h>
#include <raymath.h>
#include <float.h>

#include "./common/r3d_math.h"
#include "./r3d_core_state.h"

// ========================================
// INLINE FUNCTIONS
// ========================================

static inline Vector4 normalize_plane(Vector4 plane)
{
    float len = sqrtf(plane.x*plane.x + plane.y*plane.y + plane.z*plane.z);
    if (len <= 1e-6f) return (Vector4) {0};

    float invLen = 1.0f / len;
    plane.x *= invLen;
    plane.y *= invLen;
    plane.z *= invLen;
    plane.w *= invLen;

    return plane;
}

static inline float distance_to_plane(Vector4 plane, Vector3 position)
{
    return plane.x*position.x + plane.y*position.y + plane.z*position.z + plane.w;
}

// ========================================
// PUBLIC API
// ========================================

R3D_Frustum R3D_GetFrustum(void)
{
    return R3D.viewState.frustum;
}

R3D_Frustum R3D_ComputeFrustum(Matrix viewProj)
{
    R3D_Frustum frustum = {0};

    frustum.planes[R3D_PLANE_RIGHT] = normalize_plane((Vector4) {
        viewProj.m3 - viewProj.m0,
        viewProj.m7 - viewProj.m4,
        viewProj.m11 - viewProj.m8,
        viewProj.m15 - viewProj.m12
    });

    frustum.planes[R3D_PLANE_LEFT] = normalize_plane((Vector4) {
        viewProj.m3 + viewProj.m0,
        viewProj.m7 + viewProj.m4,
        viewProj.m11 + viewProj.m8,
        viewProj.m15 + viewProj.m12
    });

    frustum.planes[R3D_PLANE_TOP] = normalize_plane((Vector4) {
        viewProj.m3 - viewProj.m1,
        viewProj.m7 - viewProj.m5,
        viewProj.m11 - viewProj.m9,
        viewProj.m15 - viewProj.m13
    });

    frustum.planes[R3D_PLANE_BOTTOM] = normalize_plane((Vector4) {
        viewProj.m3 + viewProj.m1,
        viewProj.m7 + viewProj.m5,
        viewProj.m11 + viewProj.m9,
        viewProj.m15 + viewProj.m13
    });

    frustum.planes[R3D_PLANE_BACK] = normalize_plane((Vector4) {
        viewProj.m3 - viewProj.m2,
        viewProj.m7 - viewProj.m6,
        viewProj.m11 - viewProj.m10,
        viewProj.m15 - viewProj.m14
    });

    frustum.planes[R3D_PLANE_FRONT] = normalize_plane((Vector4) {
        viewProj.m3 + viewProj.m2,
        viewProj.m7 + viewProj.m6,
        viewProj.m11 + viewProj.m10,
        viewProj.m15 + viewProj.m14
    });

    return frustum;
}

BoundingBox R3D_ComputeFrustumBoundingBox(Matrix invViewProj)
{
    Vector4 clipCorners[8] = {
        {-1, -1, -1, 1}, {1, -1, -1, 1}, {1, 1, -1, 1}, {-1, 1, -1, 1}, // Near
        {-1, -1,  1, 1}, {1, -1,  1, 1}, {1, 1,  1, 1}, {-1, 1,  1, 1}  // Far
    };

    BoundingBox bbox = {
        .min = (Vector3){ FLT_MAX,  FLT_MAX,  FLT_MAX},
        .max = (Vector3){-FLT_MAX, -FLT_MAX, -FLT_MAX}
    };

    for (int i = 0; i < 8; i++)
    {
        Vector4 p = r3d_vector4_transform(clipCorners[i], &invViewProj);
        if (fabsf(p.w) > 1e-6f)
        {
            float invW = 1.0f / p.w;
            p.x *= invW;
            p.y *= invW;
            p.z *= invW;
        }
        bbox.min = Vector3Min(bbox.min, (Vector3) {p.x, p.y, p.z});
        bbox.max = Vector3Max(bbox.max, (Vector3) {p.x, p.y, p.z});
    }

    return bbox;
}

void R3D_ComputeFrustumCorners(Matrix invViewProj, Vector3 corners[8])
{
    Vector4 clipCorners[8] = {
        {-1, -1, -1, 1}, {1, -1, -1, 1}, {1, 1, -1, 1}, {-1, 1, -1, 1}, // Near
        {-1, -1,  1, 1}, {1, -1,  1, 1}, {1, 1,  1, 1}, {-1, 1,  1, 1}  // Far
    };

    for (int i = 0; i < 8; i++)
    {
        Vector4 p = r3d_vector4_transform(clipCorners[i], &invViewProj);
        if (fabsf(p.w) > 1e-6f)
        {
            float invW = 1.0f / p.w;
            p.x *= invW;
            p.y *= invW;
            p.z *= invW;
        }
        corners[i] = (Vector3) {p.x, p.y, p.z};
    }
}

bool R3D_FrustumContainsPoint(const R3D_Frustum* frustum, Vector3 position)
{
    for (int i = 0; i < R3D_PLANE_COUNT; i++)
    {
        if (distance_to_plane(frustum->planes[i], position) <= 0)
        {
            return false;
        }
    }

    return true;
}

bool R3D_FrustumContainsAnyPoint(const R3D_Frustum* frustum, const Vector3* positions, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (R3D_FrustumContainsPoint(frustum, positions[i]))
        {
            return true;
        }
    }

    return false;
}

bool R3D_FrustumIntersectsSphere(const R3D_Frustum* frustum, Vector3 position, float radius)
{
    for (int i = 0; i < R3D_PLANE_COUNT; i++)
    {
        if (distance_to_plane(frustum->planes[i], position) < -radius)
        {
            return false;
        }
    }

    return true;
}

bool R3D_FrustumIntersectsBoundingBox(const R3D_Frustum* frustum, BoundingBox aabb)
{
    float xMin = aabb.min.x, yMin = aabb.min.y, zMin = aabb.min.z;
    float xMax = aabb.max.x, yMax = aabb.max.y, zMax = aabb.max.z;

    for (int i = 0; i < R3D_PLANE_COUNT; i++)
    {
        const Vector4* plane = &frustum->planes[i];

        // Choose the optimal coordinates according to the sign of the normal
        float distance = distance_to_plane(*plane, (Vector3) {
            .x = (plane->x >= 0.0f) ? xMax : xMin,
            .y = (plane->y >= 0.0f) ? yMax : yMin,
            .z = (plane->z >= 0.0f) ? zMax : zMin
        });

        if (distance < -EPSILON) return false;
    }

    return true;
}

bool R3D_FrustumIntersectsOrientedBox(const R3D_Frustum* frustum, R3D_OrientedBox obb)
{
    for (int i = 0; i < R3D_PLANE_COUNT; i++)
    {
        const Vector4* plane = &frustum->planes[i];

        float centerDistance =
            plane->x * obb.center.x +
            plane->y * obb.center.y +
            plane->z * obb.center.z +
            plane->w;

        float projectedRadius =
            fabsf(plane->x*obb.axisX.x + plane->y*obb.axisX.y + plane->z*obb.axisX.z) * obb.halfExtents.x +
            fabsf(plane->x*obb.axisY.x + plane->y*obb.axisY.y + plane->z*obb.axisY.z) * obb.halfExtents.y +
            fabsf(plane->x*obb.axisZ.x + plane->y*obb.axisZ.y + plane->z*obb.axisZ.z) * obb.halfExtents.z;

        if (centerDistance + projectedRadius < -EPSILON) return false;
    }

    return true;
}
