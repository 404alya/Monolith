/* r3d_shape.c -- R3D Shape Module.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#include <r3d/r3d_shape.h>
#include <raymath.h>
#include <stddef.h>
#include <float.h>

#include "./common/r3d_math.h"

// ========================================
// INLINE FUNCTIONS
// ========================================

static inline bool raycast_triangle(
    float* outT, Vector3* outEdge1,  Vector3* outEdge2,
    Vector3 localOrigin, Vector3 localDirection,
    Vector3 v0, Vector3 v1, Vector3 v2)
{
    // Möller-Trumbore ray triangle intersection algorithm
    // See: https://en.wikipedia.org/wiki/M%C3%B6ller%E2%80%93Trumbore_intersection_algorithm

    Vector3 edge1 = Vector3Subtract(v1, v0);
    Vector3 edge2 = Vector3Subtract(v2, v0);

    // If negative: it's a backface
    // If zero: ray parallel to triangle
    Vector3 h = Vector3CrossProduct(localDirection, edge2);
    float a = Vector3DotProduct(edge1, h);
    if (a < 1e-5f) return false;

    float f = 1.0f / a;

    Vector3 s = Vector3Subtract(localOrigin, v0);
    float u = f * Vector3DotProduct(s, h);
    if (u < 0.0f || u > 1.0f) return false;

    Vector3 q = Vector3CrossProduct(s, edge1);
    float v = f * Vector3DotProduct(localDirection, q);
    if (v < 0.0f || u + v > 1.0f) return false;

    float t = f * Vector3DotProduct(edge2, q);
    if (t < 1e-5f) return false;

    *outT = t;
    *outEdge1 = edge1;
    *outEdge2 = edge2;

    return true;
}

static inline void raycast_mesh_vertices(
    float* closestT, Vector3* closestEdge1,  Vector3* closestEdge2,
    const R3D_Vertex* vertices, int triangleCount,
    Vector3 localOrigin, Vector3 localDirection)
{
    for (int i = 0; i < triangleCount; i++)
    {
        int baseIdx = i * 3;
        Vector3 v0 = vertices[baseIdx].position;
        Vector3 v1 = vertices[baseIdx + 1].position;
        Vector3 v2 = vertices[baseIdx + 2].position;

        float t;
        Vector3 edge1, edge2;
        if (raycast_triangle(&t, &edge1, &edge2, localOrigin, localDirection, v0, v1, v2))
        {
            if (t < *closestT)
            {
                *closestT = t;
                *closestEdge1 = edge1;
                *closestEdge2 = edge2;
            }
        }
    }
}

static inline void raycast_mesh_indexed(
    float* closestT, Vector3* closestEdge1,  Vector3* closestEdge2,
    const R3D_Vertex* vertices, const uint32_t* indices, int triangleCount,
    Vector3 localOrigin, Vector3 localDirection)
{
    for (int i = 0; i < triangleCount; i++)
    {
        int baseIdx = i * 3;
        Vector3 v0 = vertices[indices[baseIdx]].position;
        Vector3 v1 = vertices[indices[baseIdx + 1]].position;
        Vector3 v2 = vertices[indices[baseIdx + 2]].position;

        float t;
        Vector3 edge1, edge2;
        if (raycast_triangle(&t, &edge1, &edge2, localOrigin, localDirection, v0, v1, v2))
        {
            if (t < *closestT)
            {
                *closestT = t;
                *closestEdge1 = edge1;
                *closestEdge2 = edge2;
            }
        }
    }
}

// ========================================
// PUBLIC API
// ========================================

R3D_BoundingBox R3D_GetBoundingBox(Vector3 center, Vector3 halfExtents)
{
    R3D_BoundingBox aabb;

    aabb.min = Vector3Subtract(center, halfExtents);
    aabb.max = Vector3Add(center, halfExtents);

    return aabb;
}

R3D_OrientedBox R3D_GetOrientedBox(BoundingBox aabb, Matrix transform)
{
    R3D_OrientedBox obb;

    obb.halfExtents.x = (aabb.max.x - aabb.min.x) * 0.5f;
    obb.halfExtents.y = (aabb.max.y - aabb.min.y) * 0.5f;
    obb.halfExtents.z = (aabb.max.z - aabb.min.z) * 0.5f;

    Vector3 localCenter = {
        (aabb.min.x + aabb.max.x) * 0.5f,
        (aabb.min.y + aabb.max.y) * 0.5f,
        (aabb.min.z + aabb.max.z) * 0.5f
    };

    obb.center.x = transform.m0*localCenter.x + transform.m4*localCenter.y + transform.m8*localCenter.z + transform.m12;
    obb.center.y = transform.m1*localCenter.x + transform.m5*localCenter.y + transform.m9*localCenter.z + transform.m13;
    obb.center.z = transform.m2*localCenter.x + transform.m6*localCenter.y + transform.m10*localCenter.z + transform.m14;

    obb.axisX = (Vector3){transform.m0, transform.m1, transform.m2};
    obb.axisY = (Vector3){transform.m4, transform.m5, transform.m6};
    obb.axisZ = (Vector3){transform.m8, transform.m9, transform.m10};

    return obb;
}

bool R3D_CheckCollisionBoundingBoxes(R3D_BoundingBox box1, R3D_BoundingBox box2)
{
    return (box1.min.x <= box2.max.x && box1.max.x >= box2.min.x)
        && (box1.min.y <= box2.max.y && box1.max.y >= box2.min.y)
        && (box1.min.z <= box2.max.z && box1.max.z >= box2.min.z);
}

bool R3D_CheckCollisionBoundingBoxSphere(R3D_BoundingBox box, Vector3 center, float radius)
{
    float cx = center.x < box.min.x ? box.min.x : (center.x > box.max.x ? box.max.x : center.x);
    float cy = center.y < box.min.y ? box.min.y : (center.y > box.max.y ? box.max.y : center.y);
    float cz = center.z < box.min.z ? box.min.z : (center.z > box.max.z ? box.max.z : center.z);
    float dx = center.x - cx;
    float dy = center.y - cy;
    float dz = center.z - cz;
    return (dx*dx + dy*dy + dz*dz) <= (radius * radius);
}

bool R3D_CheckCollisionOrientedBoxes(R3D_OrientedBox box1, R3D_OrientedBox box2)
{
    Vector3 axes[15] = {
        box1.axisX, box1.axisY, box1.axisZ,
        box2.axisX, box2.axisY, box2.axisZ,
        Vector3CrossProduct(box1.axisX, box2.axisX),
        Vector3CrossProduct(box1.axisX, box2.axisY),
        Vector3CrossProduct(box1.axisX, box2.axisZ),
        Vector3CrossProduct(box1.axisY, box2.axisX),
        Vector3CrossProduct(box1.axisY, box2.axisY),
        Vector3CrossProduct(box1.axisY, box2.axisZ),
        Vector3CrossProduct(box1.axisZ, box2.axisX),
        Vector3CrossProduct(box1.axisZ, box2.axisY),
        Vector3CrossProduct(box1.axisZ, box2.axisZ),
    };

    Vector3 d = Vector3Subtract(box2.center, box1.center);

    for (int i = 0; i < 15; i++)
    {
        if (Vector3LengthSqr(axes[i]) < 1e-10f)
        {
            continue;
        }

        Vector3 axis = Vector3Normalize(axes[i]);

        float r1 = fabsf(Vector3DotProduct(box1.axisX, axis)) * box1.halfExtents.x
                 + fabsf(Vector3DotProduct(box1.axisY, axis)) * box1.halfExtents.y
                 + fabsf(Vector3DotProduct(box1.axisZ, axis)) * box1.halfExtents.z;

        float r2 = fabsf(Vector3DotProduct(box2.axisX, axis)) * box2.halfExtents.x
                 + fabsf(Vector3DotProduct(box2.axisY, axis)) * box2.halfExtents.y
                 + fabsf(Vector3DotProduct(box2.axisZ, axis)) * box2.halfExtents.z;

        if (fabsf(Vector3DotProduct(d, axis)) > r1 + r2)
        {
            return false;
        }
    }

    return true;
}

bool R3D_CheckCollisionOrientedBoxSphere(R3D_OrientedBox box, Vector3 center, float radius)
{
    Vector3 d = Vector3Subtract(center, box.center);

    Vector3 local = {
        Vector3DotProduct(d, box.axisX),
        Vector3DotProduct(d, box.axisY),
        Vector3DotProduct(d, box.axisZ)
    };

    Vector3 closest = {
        Clamp(local.x, -box.halfExtents.x, box.halfExtents.x),
        Clamp(local.y, -box.halfExtents.y, box.halfExtents.y),
        Clamp(local.z, -box.halfExtents.z, box.halfExtents.z)
    };

    Vector3 delta = Vector3Subtract(local, closest);
    return Vector3LengthSqr(delta) <= radius * radius;
}

bool R3D_CheckCollisionSpheres(Vector3 center1, float radius1, Vector3 center2, float radius2)
{
    float dx = center2.x - center1.x;
    float dy = center2.y - center1.y;
    float dz = center2.z - center1.z;
    float r = radius1 + radius2;
    return (dx*dx + dy*dy + dz*dz) <= (r * r);
}

bool R3D_CheckCollisionCapsuleBoundingBox(R3D_Capsule capsule, BoundingBox box)
{
    Vector3 closestOnSegment = R3D_ClosestPointOnSegment(
        R3D_ClosestPointOnBox(capsule.start, box),
        capsule.start,
        capsule.end
    );

    Vector3 closestOnBox = R3D_ClosestPointOnBox(closestOnSegment, box);
    float distSq = Vector3DistanceSqr(closestOnBox, closestOnSegment);

    return distSq <= (capsule.radius * capsule.radius);
}

bool R3D_CheckCollisionCapsuleOrientedBox(R3D_Capsule capsule, R3D_OrientedBox box)
{
    Vector3 ds = Vector3Subtract(capsule.start, box.center);
    Vector3 de = Vector3Subtract(capsule.end, box.center);

    Vector3 localStart = {
        Vector3DotProduct(ds, box.axisX),
        Vector3DotProduct(ds, box.axisY),
        Vector3DotProduct(ds, box.axisZ)
    };
    Vector3 localEnd = {
        Vector3DotProduct(de, box.axisX),
        Vector3DotProduct(de, box.axisY),
        Vector3DotProduct(de, box.axisZ)
    };

    BoundingBox localBox = {
        Vector3Negate(box.halfExtents),
        box.halfExtents
    };

    float rSq = capsule.radius * capsule.radius;

    Vector3 seeds[3] = {
        localStart,
        localEnd,
        R3D_ClosestPointOnSegment((Vector3){0}, localStart, localEnd),
    };

    for (int i = 0; i < 3; i++)
    {
        Vector3 onSeg = seeds[i];
        for (int iter = 0; iter < 3; iter++)
        {
            Vector3 onBox = R3D_ClosestPointOnBox(onSeg, localBox);
            onSeg = R3D_ClosestPointOnSegment(onBox, localStart, localEnd);
        }
        Vector3 onBox = R3D_ClosestPointOnBox(onSeg, localBox);
        if (Vector3DistanceSqr(onSeg, onBox) <= rSq) return true;
    }

    return false;
}

bool R3D_CheckCollisionCapsuleSphere(R3D_Capsule capsule, Vector3 center, float radius)
{
    Vector3 closestPoint = R3D_ClosestPointOnSegment(center, capsule.start, capsule.end);
    float distSq = Vector3DistanceSqr(center, closestPoint);
    float radiusSum = capsule.radius + radius;

    return distSq <= (radiusSum * radiusSum);
}

bool R3D_CheckCollisionCapsules(R3D_Capsule a, R3D_Capsule b)
{
    Vector3 dirA = Vector3Subtract(a.end, a.start);
    Vector3 dirB = Vector3Subtract(b.end, b.start);
    Vector3 r = Vector3Subtract(a.start, b.start);

    float dotAA = Vector3DotProduct(dirA, dirA);
    float dotBB = Vector3DotProduct(dirB, dirB);
    float dotAB = Vector3DotProduct(dirA, dirB);
    float dotAR = Vector3DotProduct(dirA, r);
    float dotBR = Vector3DotProduct(dirB, r);

    float denom = dotAA * dotBB - dotAB * dotAB;
    float s = 0.0f, t = 0.0f;

    if (denom > 1e-6f)
    {
        s = (dotAB * dotBR - dotBB * dotAR) / denom;
        s = fmaxf(0.0f, fminf(1.0f, s));
        t = (dotAB * s + dotBR) / dotBB;

        if (t < 0.0f)
        {
            t = 0.0f;
            s = fmaxf(0.0f, fminf(1.0f, -dotAR / dotAA));
        }
        else if (t > 1.0f)
        {
            t = 1.0f;
            s = fmaxf(0.0f, fminf(1.0f, (dotAB - dotAR) / dotAA));
        }
    }
    else
    {
        s = 0.5f;
        t = fmaxf(0.0f, fminf(1.0f, dotBR / dotBB));
    }

    Vector3 closestA = Vector3Add(a.start, Vector3Scale(dirA, s));
    Vector3 closestB = Vector3Add(b.start, Vector3Scale(dirB, t));

    float distSq = Vector3DistanceSqr(closestA, closestB);
    float radiusSum = a.radius + b.radius;

    return distSq <= (radiusSum * radiusSum);
}

bool R3D_CheckCollisionCapsuleMesh(R3D_Capsule capsule, R3D_MeshData mesh, Matrix transform)
{
    Vector3 axis = Vector3Subtract(capsule.end, capsule.start);
    float radiusSq = capsule.radius * capsule.radius;

    bool useIndices = (mesh.indices != NULL);
    int triangleCount = useIndices ? (mesh.indexCount / 3) : (mesh.vertexCount / 3);

    for (int i = 0; i < triangleCount; i++)
    {
        Vector3 v0, v1, v2;

        if (useIndices)
        {
            v0 = mesh.vertices[mesh.indices[i*3    ]].position;
            v1 = mesh.vertices[mesh.indices[i*3 + 1]].position;
            v2 = mesh.vertices[mesh.indices[i*3 + 2]].position;
        }
        else
        {
            v0 = mesh.vertices[i*3    ].position;
            v1 = mesh.vertices[i*3 + 1].position;
            v2 = mesh.vertices[i*3 + 2].position;
        }

        v0 = r3d_vector3_transform(v0, &transform);
        v1 = r3d_vector3_transform(v1, &transform);
        v2 = r3d_vector3_transform(v2, &transform);

        const int samples = 5;
        for (int s = 0; s < samples; s++)
        {
            float t = (float)s / (samples - 1);
            Vector3 p = Vector3Add(capsule.start, Vector3Scale(axis, t));

            Vector3 closest = R3D_ClosestPointOnTriangle(p, v0, v1, v2);
            if (Vector3LengthSqr(Vector3Subtract(closest, p)) <= radiusSq)
            {
                return true;
            }
        }
    }

    return false;
}

R3D_Penetration R3D_CheckPenetrationBoundingBoxes(R3D_BoundingBox box1, R3D_BoundingBox box2)
{
    float ox = fminf(box1.max.x, box2.max.x) - fmaxf(box1.min.x, box2.min.x);
    if (ox <= 0) return (R3D_Penetration){0};

    float oy = fminf(box1.max.y, box2.max.y) - fmaxf(box1.min.y, box2.min.y);
    if (oy <= 0) return (R3D_Penetration){0};

    float oz = fminf(box1.max.z, box2.max.z) - fmaxf(box1.min.z, box2.min.z);
    if (oz <= 0) return (R3D_Penetration){0};

    float dx = (box2.min.x + box2.max.x) - (box1.min.x + box1.max.x);
    float dy = (box2.min.y + box2.max.y) - (box1.min.y + box1.max.y);
    float dz = (box2.min.z + box2.max.z) - (box1.min.z + box1.max.z);

    Vector3 normal;
    float depth;

    if (ox <= oy && ox <= oz)
    {
        depth = ox;
        normal = (Vector3) {dx >= 0 ? 1.0f : -1.0f, 0, 0};
    }
    else if (oy <= oz)
    {
        depth = oy;
        normal = (Vector3) {0, dy >= 0 ? 1.0f : -1.0f, 0};
    }
    else
    {
        depth = oz;
        normal = (Vector3) {0, 0, dz >= 0 ? 1.0f : -1.0f};
    }

    return (R3D_Penetration) {
        .collides = true, .depth = depth,
        .normal = normal, .mtv = Vector3Scale(normal, depth),
    };
}

R3D_Penetration R3D_CheckPenetrationBoundingBoxSphere(R3D_BoundingBox box, Vector3 center, float radius)
{
    float cx = center.x < box.min.x ? box.min.x : (center.x > box.max.x ? box.max.x : center.x);
    float cy = center.y < box.min.y ? box.min.y : (center.y > box.max.y ? box.max.y : center.y);
    float cz = center.z < box.min.z ? box.min.z : (center.z > box.max.z ? box.max.z : center.z);

    float dx = center.x - cx, dy = center.y - cy, dz = center.z - cz;
    float distSq = dx*dx + dy*dy + dz*dz;

    if (distSq > radius * radius)
    {
        return (R3D_Penetration){0};
    }

    Vector3 normal;
    float depth;

    if (distSq > 1e-10f)
    {
        float dist = sqrtf(distSq);
        normal = (Vector3){ dx/dist, dy/dist, dz/dist };
        depth  = radius - dist;
    }
    else
    {
        float d[6] = {
            center.x - box.min.x, box.max.x - center.x,
            center.y - box.min.y, box.max.y - center.y,
            center.z - box.min.z, box.max.z - center.z,
        };
        Vector3 normals[6] = {
            {-1,0,0}, {1,0,0}, {0,-1,0},
            {0,1,0}, {0,0,-1}, {0,0,1}
        };
        int best = 0;
        for (int i = 1; i < 6; i++)
        {
            if (d[i] < d[best]) best = i;
        }
        normal = normals[best];
        depth = radius + d[best];
    }

    return (R3D_Penetration) {
        .collides = true, .depth = depth,
        .normal = normal, .mtv = Vector3Scale(normal, depth),
    };
}

R3D_Penetration R3D_CheckPenetrationOrientedBoxes(R3D_OrientedBox box1, R3D_OrientedBox box2)
{
    Vector3 d = Vector3Subtract(box2.center, box1.center);

    Vector3 axes[15] = {
        box1.axisX, box1.axisY, box1.axisZ,
        box2.axisX, box2.axisY, box2.axisZ,
        Vector3CrossProduct(box1.axisX, box2.axisX),
        Vector3CrossProduct(box1.axisX, box2.axisY),
        Vector3CrossProduct(box1.axisX, box2.axisZ),
        Vector3CrossProduct(box1.axisY, box2.axisX),
        Vector3CrossProduct(box1.axisY, box2.axisY),
        Vector3CrossProduct(box1.axisY, box2.axisZ),
        Vector3CrossProduct(box1.axisZ, box2.axisX),
        Vector3CrossProduct(box1.axisZ, box2.axisY),
        Vector3CrossProduct(box1.axisZ, box2.axisZ),
    };

    float minDepth = FLT_MAX;
    Vector3 minAxis = {0, 1, 0};

    for (int i = 0; i < 15; i++)
    {
        if (Vector3LengthSqr(axes[i]) < 1e-10f) continue;
        Vector3 axis = Vector3Normalize(axes[i]);

        float r1 = fabsf(Vector3DotProduct(box1.axisX, axis)) * box1.halfExtents.x
                 + fabsf(Vector3DotProduct(box1.axisY, axis)) * box1.halfExtents.y
                 + fabsf(Vector3DotProduct(box1.axisZ, axis)) * box1.halfExtents.z;

        float r2 = fabsf(Vector3DotProduct(box2.axisX, axis)) * box2.halfExtents.x
                 + fabsf(Vector3DotProduct(box2.axisY, axis)) * box2.halfExtents.y
                 + fabsf(Vector3DotProduct(box2.axisZ, axis)) * box2.halfExtents.z;

        float overlap = r1 + r2 - fabsf(Vector3DotProduct(d, axis));
        if (overlap <= 0) return (R3D_Penetration){0};

        if (overlap < minDepth)
        {
            minDepth = overlap;
            minAxis = axis;
        }
    }

    // Ensure normal points from box1 toward box2
    if (Vector3DotProduct(minAxis, d) < 0)
    {
        minAxis = Vector3Negate(minAxis);
    }

    return (R3D_Penetration) {
        .collides = true, .depth = minDepth,
        .normal = minAxis, .mtv = Vector3Scale(minAxis, minDepth),
    };
}

R3D_Penetration R3D_CheckPenetrationOrientedBoxSphere(R3D_OrientedBox box, Vector3 center, float radius)
{
    Vector3 d = Vector3Subtract(center, box.center);
    Vector3 local = {
        Vector3DotProduct(d, box.axisX),
        Vector3DotProduct(d, box.axisY),
        Vector3DotProduct(d, box.axisZ),
    };

    R3D_BoundingBox localBox = {Vector3Negate(box.halfExtents), box.halfExtents};
    R3D_Penetration p = R3D_CheckPenetrationBoundingBoxSphere(localBox, local, radius);
    if (!p.collides) return (R3D_Penetration){0};

    Vector3 worldNormal = {
        box.axisX.x * p.normal.x + box.axisY.x * p.normal.y + box.axisZ.x * p.normal.z,
        box.axisX.y * p.normal.x + box.axisY.y * p.normal.y + box.axisZ.y * p.normal.z,
        box.axisX.z * p.normal.x + box.axisY.z * p.normal.y + box.axisZ.z * p.normal.z,
    };

    return (R3D_Penetration) {
        .collides = true, .depth = p.depth,
        .normal = worldNormal, .mtv = Vector3Scale(worldNormal, p.depth),
    };
}

R3D_Penetration R3D_CheckPenetrationSpheres(Vector3 c1, float r1, Vector3 c2, float r2)
{
    float dx = c2.x - c1.x, dy = c2.y - c1.y, dz = c2.z - c1.z;
    float distSq = dx*dx + dy*dy + dz*dz;
    float rSum = r1 + r2;

    if (distSq >= rSum * rSum)
    {
        return (R3D_Penetration){0};
    }

    float dist = sqrtf(distSq);
    Vector3 normal = dist > 1e-6f
        ? (Vector3) {dx/dist, dy/dist, dz/dist}
        : (Vector3) {0, 1, 0};

    float depth = rSum - dist;

    return (R3D_Penetration) {
        .collides = true, .depth = depth,
        .normal = normal, .mtv = Vector3Scale(normal, depth),
    };
}

R3D_Penetration R3D_CheckPenetrationCapsuleBoundingBox(R3D_Capsule capsule, BoundingBox box)
{
    R3D_Penetration result = {0};

    Vector3 closestOnSegment = R3D_ClosestPointOnSegment(
        R3D_ClosestPointOnBox(capsule.start, box),
        capsule.start,
        capsule.end
    );

    Vector3 closestOnBox = R3D_ClosestPointOnBox(closestOnSegment, box);
    Vector3 delta = Vector3Subtract(closestOnSegment, closestOnBox);

    float distSq = Vector3LengthSqr(delta);
    float radiusSq = capsule.radius * capsule.radius;

    if (distSq >= radiusSq) return result;

    float dist = sqrtf(distSq);
    result.collides = true;
    result.depth = capsule.radius - dist;

    if (dist > 1e-6f)
    {
        result.normal = Vector3Scale(delta, 1.0f / dist);
    }
    else
    {
        Vector3 boxCenter = {
            (box.min.x + box.max.x) * 0.5f,
            (box.min.y + box.max.y) * 0.5f,
            (box.min.z + box.max.z) * 0.5f
        };
        Vector3 toCenter = Vector3Subtract(closestOnSegment, boxCenter);

        float ax = fabsf(toCenter.x);
        float ay = fabsf(toCenter.y);
        float az = fabsf(toCenter.z);

        if (ax >= ay && ax >= az)
        {
            result.normal = (Vector3){toCenter.x > 0 ? 1.0f : -1.0f, 0, 0};
        }
        else if (ay >= az)
        {
            result.normal = (Vector3){0, toCenter.y > 0 ? 1.0f : -1.0f, 0};
        }
        else
        {
            result.normal = (Vector3){0, 0, toCenter.z > 0 ? 1.0f : -1.0f};
        }
    }
    
    result.mtv = Vector3Scale(result.normal, result.depth);
    return result;
}

R3D_Penetration R3D_CheckPenetrationCapsuleOrientedBox(R3D_Capsule capsule, R3D_OrientedBox box)
{
    Vector3 ds = Vector3Subtract(capsule.start, box.center);
    Vector3 de = Vector3Subtract(capsule.end,   box.center);

    Vector3 localStart = {
        Vector3DotProduct(ds, box.axisX),
        Vector3DotProduct(ds, box.axisY),
        Vector3DotProduct(ds, box.axisZ),
    };
    Vector3 localEnd = {
        Vector3DotProduct(de, box.axisX),
        Vector3DotProduct(de, box.axisY),
        Vector3DotProduct(de, box.axisZ),
    };

    BoundingBox localBox = {Vector3Negate(box.halfExtents), box.halfExtents};

    float rSq = capsule.radius * capsule.radius;

    Vector3 seeds[3] = {
        localStart,
        localEnd,
        R3D_ClosestPointOnSegment((Vector3){0}, localStart, localEnd),
    };

    float bestDistSq = FLT_MAX;
    Vector3 bestOnSeg = localStart;
    Vector3 bestOnBox = R3D_ClosestPointOnBox(localStart, localBox);

    for (int i = 0; i < 3; i++)
    {
        Vector3 onSeg = seeds[i];
        for (int iter = 0; iter < 3; iter++)
        {
            Vector3 onBox = R3D_ClosestPointOnBox(onSeg, localBox);
            onSeg = R3D_ClosestPointOnSegment(onBox, localStart, localEnd);
        }
        Vector3 onBox = R3D_ClosestPointOnBox(onSeg, localBox);
        float distSq = Vector3DistanceSqr(onSeg, onBox);

        if (distSq < bestDistSq)
        {
            bestDistSq = distSq;
            bestOnSeg = onSeg;
            bestOnBox = onBox;
        }
    }

    if (bestDistSq > rSq)
    {
        return (R3D_Penetration){0};
    }

    Vector3 localNormal;
    float depth;

    if (bestDistSq > 1e-10f)
    {
        float dist = sqrtf(bestDistSq);
        Vector3 d  = Vector3Subtract(bestOnSeg, bestOnBox);
        localNormal = (Vector3){ d.x/dist, d.y/dist, d.z/dist };
        depth = capsule.radius - dist;
    }
    else
    {
        float faces[6] = {
            bestOnSeg.x - localBox.min.x, localBox.max.x - bestOnSeg.x,
            bestOnSeg.y - localBox.min.y, localBox.max.y - bestOnSeg.y,
            bestOnSeg.z - localBox.min.z, localBox.max.z - bestOnSeg.z,
        };
        Vector3 faceNormals[6] = {
            {-1,0,0}, {1,0,0}, {0,-1,0},
            {0,1,0}, {0,0,-1}, {0,0,1}
        };
        int best = 0;
        for (int i = 1; i < 6; i++)
        {
            if (faces[i] < faces[best]) best = i;
        }
        localNormal = faceNormals[best];
        depth = capsule.radius + faces[best];
    }

    Vector3 worldNormal = {
        box.axisX.x * localNormal.x + box.axisY.x * localNormal.y + box.axisZ.x * localNormal.z,
        box.axisX.y * localNormal.x + box.axisY.y * localNormal.y + box.axisZ.y * localNormal.z,
        box.axisX.z * localNormal.x + box.axisY.z * localNormal.y + box.axisZ.z * localNormal.z,
    };

    return (R3D_Penetration) {
        .collides = true, .depth = depth, .normal = worldNormal,
        .mtv = Vector3Scale(worldNormal, depth),
    };
}

R3D_Penetration R3D_CheckPenetrationCapsuleSphere(R3D_Capsule capsule, Vector3 center, float radius)
{
    R3D_Penetration result = {0};

    Vector3 closestOnSegment = R3D_ClosestPointOnSegment(center, capsule.start, capsule.end);
    Vector3 delta = Vector3Subtract(center, closestOnSegment);

    float distSq = Vector3LengthSqr(delta);
    float combinedRadius = capsule.radius + radius;
    float combinedRadiusSq = combinedRadius * combinedRadius;

    if (distSq >= combinedRadiusSq) return result;

    float dist = sqrtf(distSq);
    result.collides = true;
    result.depth = combinedRadius - dist;

    if (dist > 1e-6f)
    {
        result.normal = Vector3Scale(delta, 1.0f / dist);
    }
    else
    {
        Vector3 capsuleDir = Vector3Subtract(capsule.end, capsule.start);
        float capsuleLengthSq = Vector3LengthSqr(capsuleDir);
        
        if (capsuleLengthSq > 1e-6f)
        {
            result.normal = (Vector3){capsuleDir.y, -capsuleDir.x, 0};
            float normalLengthSq = Vector3LengthSqr(result.normal);

            if (normalLengthSq < 1e-6f)
            {
                result.normal = (Vector3){0, capsuleDir.z, -capsuleDir.y};
                normalLengthSq = Vector3LengthSqr(result.normal);
            }

            if (normalLengthSq > 1e-6f)
            {
                result.normal = Vector3Normalize(result.normal);
            }
            else
            {
                result.normal = (Vector3){0, 1, 0};
            }
        }
        else
        {
            result.normal = (Vector3){0, 1, 0};
        }
    }

    result.mtv = Vector3Scale(result.normal, result.depth);
    return result;
}

R3D_Penetration R3D_CheckPenetrationCapsules(R3D_Capsule a, R3D_Capsule b)
{
    R3D_Penetration result = {0};

    Vector3 dirA = Vector3Subtract(a.end, a.start);
    Vector3 dirB = Vector3Subtract(b.end, b.start);
    Vector3 r = Vector3Subtract(a.start, b.start);

    float dotAA = Vector3DotProduct(dirA, dirA);
    float dotBB = Vector3DotProduct(dirB, dirB);
    float dotAB = Vector3DotProduct(dirA, dirB);
    float dotAR = Vector3DotProduct(dirA, r);
    float dotBR = Vector3DotProduct(dirB, r);

    float denom = dotAA * dotBB - dotAB * dotAB;

    float s = 0.0f, t = 0.0f;
    
    if (denom > 1e-6f)
    {
        s = (dotAB * dotBR - dotBB * dotAR) / denom;
        s = fmaxf(0.0f, fminf(1.0f, s));
        t = (dotAB * s + dotBR) / dotBB;

        if (t < 0.0f)
        {
            t = 0.0f;
            s = fmaxf(0.0f, fminf(1.0f, -dotAR / dotAA));
        }
        else if (t > 1.0f)
        {
            t = 1.0f;
            s = fmaxf(0.0f, fminf(1.0f, (dotAB - dotAR) / dotAA));
        }
    }
    else
    {
        s = 0.5f;
        t = fmaxf(0.0f, fminf(1.0f, dotBR / dotBB));
    }

    Vector3 closestA = Vector3Add(a.start, Vector3Scale(dirA, s));
    Vector3 closestB = Vector3Add(b.start, Vector3Scale(dirB, t));

    Vector3 delta = Vector3Subtract(closestA, closestB);
    float distSq = Vector3LengthSqr(delta);
    float combinedRadius = a.radius + b.radius;
    float combinedRadiusSq = combinedRadius * combinedRadius;

    if (distSq >= combinedRadiusSq) return result;

    float dist = sqrtf(distSq);
    result.collides = true;
    result.depth = combinedRadius - dist;

    if (dist > 1e-6f)
    {
        result.normal = Vector3Scale(delta, 1.0f / dist);
    }
    else
    {
        Vector3 cross = Vector3CrossProduct(dirA, dirB);
        float crossLengthSq = Vector3LengthSqr(cross);

        if (crossLengthSq > 1e-6f)
        {
            result.normal = Vector3Normalize(cross);
        }
        else
        {
            Vector3 perp = (Vector3){dirA.y, -dirA.x, 0};
            float perpLengthSq = Vector3LengthSqr(perp);

            if (perpLengthSq < 1e-6f)
            {
                perp = (Vector3){0, dirA.z, -dirA.y};
                perpLengthSq = Vector3LengthSqr(perp);
            }

            if (perpLengthSq > 1e-6f)
            {
                result.normal = Vector3Normalize(perp);
            }
            else
            {
                result.normal = (Vector3){0, 1, 0};
            }
        }
    }

    result.mtv = Vector3Scale(result.normal, result.depth);
    return result;
}

RayCollision R3D_RaycastTriangle(Ray ray, Vector3 p1, Vector3 p2, Vector3 p3)
{
    float t = 0.0f;
    Vector3 edge1 = {0};
    Vector3 edge2 = {0};

    if (!raycast_triangle(&t, &edge1, &edge2, ray.position, ray.direction, p1, p2, p3))
    {
        return (RayCollision){0};
    }

    Vector3 point = Vector3Add(ray.position, Vector3Scale(ray.direction, t));
    Vector3 normal = Vector3Normalize(Vector3CrossProduct(edge1, edge2));

    return (RayCollision) {
        .hit = true,
        .distance = t,
        .point = point,
        .normal = normal
    };
}

RayCollision R3D_RaycastQuad(Ray ray, Vector3 p1, Vector3 p2, Vector3 p3, Vector3 p4)
{
    RayCollision collision = R3D_RaycastTriangle(ray, p1, p2, p3);
    if (collision.hit) return collision;

    return R3D_RaycastTriangle(ray, p1, p3, p4);
}

RayCollision R3D_RaycastBoundingBox(Ray ray, R3D_BoundingBox box)
{
    float tmin = 0.0f;
    float tmax = FLT_MAX;
    int axis = -1;
    bool flip = false;

    float origin[3] = {ray.position.x, ray.position.y, ray.position.z};
    float dir[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
    float boxMin[3] = {box.min.x, box.min.y, box.min.z};
    float boxMax[3] = {box.max.x, box.max.y, box.max.z};

    for (int i = 0; i < 3; i++)
    {
        if (fabsf(dir[i]) < 1e-8f)
        {
            if (origin[i] < boxMin[i] || origin[i] > boxMax[i])
            {
                return (RayCollision){0};
            }
            continue;
        }

        float invD = 1.0f / dir[i];
        float t1 = (boxMin[i] - origin[i]) * invD;
        float t2 = (boxMax[i] - origin[i]) * invD;

        bool near = t1 <= t2;
        float tNear = near ? t1 : t2;
        float tFar  = near ? t2 : t1;

        if (tNear > tmin) tmin = tNear, axis = i, flip = !near;
        if (tFar < tmax) tmax = tFar;
        if (tmin > tmax) return (RayCollision){0};
    }

    if (tmin < 0) return (RayCollision){0};

    Vector3 normals[3] = {{1,0,0}, {0,1,0}, {0,0,1}};
    Vector3 normal = normals[axis];
    if (flip) normal = Vector3Negate(normal);

    Vector3 point = Vector3Add(ray.position, Vector3Scale(ray.direction, tmin));

    return (RayCollision) {
        .hit = true, .distance = tmin,
        .point = point, .normal = normal
    };
}

RayCollision R3D_RaycastOrientedBox(Ray ray, R3D_OrientedBox box)
{
    Vector3 d = Vector3Subtract(ray.position, box.center);

    Ray localRay = {
        .position = {
            Vector3DotProduct(d, box.axisX),
            Vector3DotProduct(d, box.axisY),
            Vector3DotProduct(d, box.axisZ),
        },
        .direction = {
            Vector3DotProduct(ray.direction, box.axisX),
            Vector3DotProduct(ray.direction, box.axisY),
            Vector3DotProduct(ray.direction, box.axisZ),
        },
    };

    R3D_BoundingBox localBox = {Vector3Negate(box.halfExtents), box.halfExtents};
    RayCollision hit = R3D_RaycastBoundingBox(localRay, localBox);
    if (!hit.hit) return (RayCollision){0};

    Vector3 worldNormal = {
        box.axisX.x * hit.normal.x + box.axisY.x * hit.normal.y + box.axisZ.x * hit.normal.z,
        box.axisX.y * hit.normal.x + box.axisY.y * hit.normal.y + box.axisZ.y * hit.normal.z,
        box.axisX.z * hit.normal.x + box.axisY.z * hit.normal.y + box.axisZ.z * hit.normal.z,
    };

    return (RayCollision) {
        .hit = true,
        .distance = hit.distance,
        .point = Vector3Add(ray.position, Vector3Scale(ray.direction, hit.distance)),
        .normal = worldNormal,
    };
}

RayCollision R3D_RaycastSphere(Ray ray, Vector3 center, float radius)
{
    Vector3 oc = Vector3Subtract(ray.position, center);
    float b = Vector3DotProduct(oc, ray.direction);
    float c = Vector3DotProduct(oc, oc) - radius * radius;

    if (b > 0 && c > 0) return (RayCollision){0};

    float disc = b*b - c;
    if (disc < 0) return (RayCollision){0};

    float sqrtDisc = sqrtf(disc);
    float t = -b - sqrtDisc;

    if (t < 0) t = -b + sqrtDisc;
    if (t < 0) return (RayCollision){0};

    Vector3 point  = Vector3Add(ray.position, Vector3Scale(ray.direction, t));
    Vector3 normal = Vector3Scale(Vector3Subtract(point, center), 1.0f / radius);

    return (RayCollision) {
        .hit = true, .distance = t,
        .point = point, .normal = normal
    };
}

RayCollision R3D_RaycastCapsule(Ray ray, R3D_Capsule capsule)
{
    Vector3 ab = Vector3Subtract(capsule.end, capsule.start);
    Vector3 ao = Vector3Subtract(ray.position, capsule.start);

    float abLen2 = Vector3DotProduct(ab, ab);
    float abDir = Vector3DotProduct(ray.direction, ab);
    float abOri = Vector3DotProduct(ao, ab);

    float a = abLen2 - abDir * abDir;
    float b = abLen2 * Vector3DotProduct(ao, ray.direction) - abOri * abDir;
    float c = abLen2 * Vector3DotProduct(ao, ao) - abOri * abOri - capsule.radius * capsule.radius * abLen2;

    RayCollision best = {0};
    best.distance = FLT_MAX;

    if (fabsf(a) > 1e-8f)
    {
        float disc = b*b - a*c;
        if (disc >= 0)
        {
            float sqrtDisc = sqrtf(disc);
            float t[2] = {(-b - sqrtDisc) / a, (-b + sqrtDisc) / a};

            for (int i = 0; i < 2; i++)
            {
                if (t[i] < 0) continue;

                float proj = abOri + t[i] * abDir;
                if (proj < 0 || proj > abLen2) continue;

                Vector3 point = Vector3Add(ray.position, Vector3Scale(ray.direction, t[i]));
                Vector3 axisPoint = Vector3Add(capsule.start, Vector3Scale(ab, proj / abLen2));
                Vector3 normal = Vector3Normalize(Vector3Subtract(point, axisPoint));

                if (t[i] < best.distance)
                {
                    best = (RayCollision) {
                        .hit = true, .distance = t[i],
                        .point = point, .normal = normal
                    };
                }
                break; // Nearest valid hit found
            }
        }
    }

    RayCollision capStart = R3D_RaycastSphere(ray, capsule.start, capsule.radius);
    RayCollision capEnd = R3D_RaycastSphere(ray, capsule.end,   capsule.radius);

    if (capStart.hit && capStart.distance < best.distance) best = capStart;
    if (capEnd.hit && capEnd.distance < best.distance) best = capEnd;

    return best.hit ? best : (RayCollision){0};
}

RayCollision R3D_RaycastMesh(Ray ray, R3D_MeshData mesh, Matrix transform)
{
    RayCollision collision = {0};
    collision.distance = FLT_MAX;

    if (mesh.vertices == NULL)
    {
        return collision;
    }

    Matrix invTransform = MatrixInvert(transform);
    Vector3 localOrigin = r3d_vector3_transform(ray.position, &invTransform);
    Vector3 localDirection = Vector3Normalize(r3d_vector3_transform_normal(ray.direction, &invTransform));

    int triangleCount = mesh.indices ? (mesh.indexCount / 3) : (mesh.vertexCount / 3);

    Vector3 closestEdge1 = {0};
    Vector3 closestEdge2 = {0};
    float closestT = FLT_MAX;

    if (mesh.indices)
    {
        raycast_mesh_indexed(
            &closestT, &closestEdge1, &closestEdge2,
            mesh.vertices, mesh.indices, triangleCount,
            localOrigin, localDirection
        );
    }
    else
    {
        raycast_mesh_vertices(
            &closestT, &closestEdge1, &closestEdge2,
            mesh.vertices, triangleCount,
            localOrigin, localDirection
        );
    }

    if (closestT < FLT_MAX)
    {
        Vector3 closestHitLocal = Vector3Add(localOrigin, Vector3Scale(localDirection, closestT));
        Vector3 normalLocal = Vector3Normalize(Vector3CrossProduct(closestEdge1, closestEdge2));
        Matrix normalMatrix = MatrixTranspose(invTransform);

        collision.hit = true;
        collision.point = r3d_vector3_transform(closestHitLocal, &transform);
        collision.distance = Vector3Distance(ray.position, collision.point);
        collision.normal = Vector3Normalize(r3d_vector3_transform_normal(normalLocal, &normalMatrix));
    }

    return collision;
}

RayCollision R3D_RaycastModel(Ray ray, R3D_Model model, Matrix transform)
{
    RayCollision collision = {0};
    collision.distance = FLT_MAX;

    if (model.meshData == NULL || model.meshCount <= 0)
    {
        return collision;
    }

    Matrix invTransform = MatrixInvert(transform);
    Vector3 localOrigin = r3d_vector3_transform(ray.position, &invTransform);
    Vector3 localDirection = Vector3Normalize(r3d_vector3_transform_normal(ray.direction, &invTransform));

    Vector3 closestEdge1 = {0};
    Vector3 closestEdge2 = {0};
    float closestT = FLT_MAX;

    // Test each mesh
    for (int meshIdx = 0; meshIdx < model.meshCount; meshIdx++)
    {
        R3D_MeshData mesh = model.meshData[meshIdx];
        if (mesh.vertices == NULL) continue;

        // Per-mesh AABB culling
        Ray localRay = {.position = localOrigin, .direction = localDirection};
        RayCollision meshBoxCol = GetRayCollisionBox(localRay, model.meshes[meshIdx].aabb);
        if (!meshBoxCol.hit) continue;

        int triangleCount = mesh.indices ? (mesh.indexCount / 3) : (mesh.vertexCount / 3);

        if (mesh.indices)
        {
            raycast_mesh_indexed(
                &closestT, &closestEdge1, &closestEdge2,
                mesh.vertices, mesh.indices, triangleCount,
                localOrigin, localDirection
            );
        }
        else
        {
            raycast_mesh_vertices(
                &closestT, &closestEdge1, &closestEdge2,
                mesh.vertices, triangleCount,
                localOrigin, localDirection
            );
        }
    }

    if (closestT < FLT_MAX)
    {
        Vector3 closestHitLocal = Vector3Add(localOrigin, Vector3Scale(localDirection, closestT));
        Vector3 normalLocal = Vector3Normalize(Vector3CrossProduct(closestEdge1, closestEdge2));
        Matrix normalMatrix = MatrixTranspose(invTransform);

        collision.hit = true;
        collision.point = r3d_vector3_transform(closestHitLocal, &transform);
        collision.distance = Vector3Distance(ray.position, collision.point);
        collision.normal = Vector3Normalize(r3d_vector3_transform_normal(normalLocal, &normalMatrix));
    }

    return collision;
}

Vector3 R3D_ClosestPointOnSegment(Vector3 point, Vector3 start, Vector3 end)
{
    Vector3 dir = Vector3Subtract(end, start);
    float lenSq = Vector3LengthSqr(dir);

    if (lenSq < 1e-10f) return start;

    float t = Vector3DotProduct(Vector3Subtract(point, start), dir) / lenSq;
    t = fmaxf(0.0f, fminf(t, 1.0f));

    return Vector3Add(start, Vector3Scale(dir, t));
}

Vector3 R3D_ClosestPointOnTriangle(Vector3 p, Vector3 a, Vector3 b, Vector3 c)
{
    Vector3 ab = Vector3Subtract(b, a);
    Vector3 ac = Vector3Subtract(c, a);
    Vector3 ap = Vector3Subtract(p, a);

    float d1 = Vector3DotProduct(ab, ap);
    float d2 = Vector3DotProduct(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    Vector3 bp = Vector3Subtract(p, b);
    float d3 = Vector3DotProduct(ab, bp);
    float d4 = Vector3DotProduct(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    Vector3 cp = Vector3Subtract(p, c);
    float d5 = Vector3DotProduct(ab, cp);
    float d6 = Vector3DotProduct(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
    {
        float v = d1 / (d1 - d3);
        return Vector3Add(a, Vector3Scale(ab, v));
    }
        
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
    {
        float v = d2 / (d2 - d6);
        return Vector3Add(a, Vector3Scale(ac, v));
    }
        
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
    {
        float v = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return Vector3Add(b, Vector3Scale(Vector3Subtract(c, b), v));
    }

    float denom = 1.f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;

    return Vector3Add(a, Vector3Add(Vector3Scale(ab, v), Vector3Scale(ac, w)));
}

Vector3 R3D_ClosestPointOnBox(Vector3 point, BoundingBox box)
{
    Vector3 closest;
    closest.x = fmaxf(box.min.x, fminf(point.x, box.max.x));
    closest.y = fmaxf(box.min.y, fminf(point.y, box.max.y));
    closest.z = fmaxf(box.min.z, fminf(point.z, box.max.z));
    return closest;
}
