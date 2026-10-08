/* ssr.frag -- Screen Space Reflections fragment shader (Hi-Z tracing)
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#version 330 core

// ================================
// Includes
// ================================

#include <wrap/view.glsl>
#include <lib/color.glsl>
#include <lib/math.glsl>
#include <ubo/fx.glsl>

// ================================
// In - Varyings
// ================================

noperspective in vec2 vTexCoord;

// ================================
// Out - Fragments
// ================================

out vec4 FragColor;

// ================================
// Samplers & Uniforms
// ================================

uniform sampler2D uRadianceTex;
uniform sampler2D uSpecularTex;
uniform sampler2D uNormalTex;
uniform sampler2D uDepthTex;

// ================================
// Helper Functions
// ================================

vec2 CellCount(int level)
{
    return vec2(textureSize(uDepthTex, level));
}

float FetchCellK(ivec2 cellIndex, int level)
{
    float linearDepth = texelFetch(uDepthTex, cellIndex, level).r;
    return 1.0 / linearDepth;
}

float NextCellEdgeT(vec2 posXY, vec2 cellCount, vec2 cellStep, vec3 screenPos, vec3 screenRayDir)
{
    vec2 cellIdx = floor(posXY * cellCount);
    vec2 nextIdx = cellIdx + clamp(cellStep, vec2(0.0), vec2(1.0));
    vec2 nextUV = (nextIdx / cellCount) + cellStep * 1e-6;
    vec2 edgeT = (nextUV - screenPos.xy) / screenRayDir.xy;
    return min(edgeT.x, edgeT.y);
}

vec3 ClipToNearPlane(vec3 pos, vec3 dir)
{
    if (pos.z > 0.0)
    {
        pos -= dir / dir.z * (pos.z + 1e-5);
    }
    return pos;
}

// ================================
// Hi-Z Tracing
// ================================

struct HitResult { vec2 uv; float z; float t; bool hit; };

HitResult TraceHiZ(vec3 startViewPos, vec3 reflectionDir)
{
    HitResult result;
    result.hit = false;
    result.uv = vec2(0.0);
    result.t = 0.0;

    vec3 endViewPos = ClipToNearPlane(startViewPos + reflectionDir, reflectionDir);
    vec2 uv1 = V_ViewToScreen(endViewPos);
    float k0 = -1.0 / startViewPos.z;
    float k1 = -1.0 / endViewPos.z;

    vec3 screenPos = vec3(vTexCoord, k0);
    vec3 screenEndPos = vec3(uv1, k1);

    vec3 screenRayDir = screenEndPos - screenPos;
    screenRayDir /= abs(screenRayDir.z);

    bool movingAway = screenRayDir.z >= 0.0;

    vec2 tNear = (vec2(0.0) - screenPos.xy) / screenRayDir.xy;
    vec2 tFar  = (vec2(1.0) - screenPos.xy) / screenRayDir.xy;
    vec2 tExit = max(tNear, tFar);
    float tMax = min(tExit.x, tExit.y);

    vec2 cellStep = vec2(screenRayDir.x < 0.0 ? -1.0 : 1.0, screenRayDir.y < 0.0 ? -1.0 : 1.0);

    int level = 0;
    int iterationsLeft = uSsr.maxIterations;

    // Push the start position to the boundary of its level-0 cell to avoid
    // immediately self-intersecting the current pixel.
    float t = NextCellEdgeT(screenPos.xy, CellCount(0), cellStep, screenPos, screenRayDir);

    while (level >= 0 && iterationsLeft > 0 && t < tMax)
    {
        vec3 curPos = screenPos + screenRayDir * t;

        vec2 cellCount = CellCount(level);
        vec2 cellIdxF = floor(curPos.xy * cellCount);
        ivec2 cellIdx = ivec2(cellIdxF);

        float cellK = FetchCellK(cellIdx, level);
        float depthT = (cellK - screenPos.z) * screenRayDir.z;
        float edgeT = NextCellEdgeT(curPos.xy, cellCount, cellStep, screenPos, screenRayDir);

        bool cellHit = movingAway ? (t <= depthT) : (depthT <= edgeT);
        int mipOffset = cellHit ? -1 : 1;

        if (level == 0)
        {
            float cellLinearDepth = 1.0 / cellK;
            float rayLinearDepth = 1.0 / curPos.z;
            if ((cellLinearDepth - rayLinearDepth) > uSsr.thickness)
            {
                cellHit = false;
                mipOffset = 0;
            }
        }

        if (cellHit)
        {
            if (!movingAway) t = max(t, depthT);
        }
        else
        {
            t = edgeT;
        }

        level = min(level + mipOffset, uSsr.maxLevel);
        --iterationsLeft;
    }

    if (level < 0 && t < tMax)
    {
        vec3 hitRay = screenPos + screenRayDir * t;
        result.hit = true;
        result.uv = hitRay.xy;
        result.z = hitRay.z;
        result.t = clamp(t / tMax, 0.0, 1.0);
    }

    return result;
}

// ================================
// Main Function
// ================================

void main()
{
    float linearDepth = texelFetch(uDepthTex, ivec2(gl_FragCoord.xy), 0).r;
    vec3 viewNormal = V_GetViewNormal(uNormalTex, ivec2(gl_FragCoord.xy));
    vec3 viewPos = V_GetViewPosition(vTexCoord, linearDepth);
    vec3 viewDir = normalize(viewPos);

    vec3 reflectionDir = normalize(reflect(viewDir, viewNormal));
    HitResult result = TraceHiZ(viewPos, reflectionDir);
    if (!result.hit)
    {
        FragColor = vec4(0.0);
        return;
    }

    ivec2 hitPixCoord = ivec2(result.uv * vec2(textureSize(uDepthTex, 0)));
    float hitLinearDepth = texelFetch(uDepthTex, hitPixCoord, 0).r;
    if (hitLinearDepth >= uView.far)
    {
        FragColor = vec4(0.0);
        return;
    }

    vec3 hitNormal = V_GetViewNormal(uNormalTex, result.uv);
    if (dot(reflectionDir, hitNormal) > 0.0)
    {
        FragColor = vec4(0.0);
        return;
    }

    vec3 hitDiff = texelFetch(uRadianceTex, hitPixCoord, 0).rgb;
    vec3 hitSpec = texelFetch(uSpecularTex, hitPixCoord, 0).rgb;

    vec3 rayPos = V_GetViewPosition(result.uv, 1.0 / result.z);
    vec3 hitPos = V_GetViewPosition(result.uv, hitLinearDepth);

    float confidence = 1.0 - smoothstep(0.0, uSsr.thickness, length(rayPos - hitPos));
    float validity = clamp(confidence * confidence, 0.0, 1.0);

    vec2 distToBorder = min(result.uv, 1.0 - result.uv);
    float edgeFade = smoothstep(0.0, uSsr.edgeFade, min(distToBorder.x, distToBorder.y));

    FragColor = vec4(C_Tonemap(hitDiff + hitSpec), validity * edgeFade);
}
