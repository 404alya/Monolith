/* ssao.frag -- Scalable Ambient Occlusion fragment shader
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

// Adapted from the method proposed by Morgan McGuire et al. in "Scalable Ambient Obscurance"
// SEE: https://research.nvidia.com/publication/2012-06_scalable-ambient-obscurance

#version 330 core

// ================================
// Constants
// ================================

// Number of spiral turns for each sample count, ensuring coprime relationship.
// Each entry ROTATIONS[i] is chosen such that GCD(i+1, ROTATIONS[i]) = 1,
// preventing sample alignment artifacts in the spiral pattern.
// Indexed by (sampleCount - 1). Supports 1 to 98 samples.
const int ROTATIONS[98] = int[98](
    1, 1, 2, 3, 2, 5, 2, 3, 2,
    3, 3, 5, 5, 3, 4, 7, 5, 5, 7,
    9, 8, 5, 5, 7, 7, 7, 8, 5, 8,
    11, 12, 7, 10, 13, 8, 11, 8, 7, 14,
    11, 11, 13, 12, 13, 19, 17, 13, 11, 18,
    19, 11, 11, 14, 17, 21, 15, 16, 17, 18,
    13, 17, 11, 17, 19, 18, 25, 18, 19, 19,
    29, 21, 19, 27, 31, 29, 21, 18, 17, 29,
    31, 31, 23, 18, 25, 26, 25, 23, 19, 34,
    19, 27, 21, 25, 39, 29, 17, 21, 27
);

// ================================
// Includes
// ================================

#include <wrap/view.glsl>
#include <lib/math.glsl>
#include <ubo/fx.glsl>

// ================================
// In - Varyings
// ================================

noperspective in vec2 vTexCoord;

// ================================
// Out - Fragments
// ================================

out float FragOcclusion;

// ================================
// Samplers & Uniforms
// ================================

uniform sampler2D uNormalTex;
uniform sampler2D uDepthTex;

// ================================
// Helper Functions
// ================================

vec2 TapLocation(int i, float numSpiralTurns, float spin, out float rNorm)
{
    float alpha = (float(i) + 0.5) / float(uSsao.sampleCount);
    float angle = alpha * (numSpiralTurns * M_TAU) + spin;

    rNorm = alpha;
    return vec2(cos(angle), sin(angle));
}

// ================================
// Main Function
// ================================

void main()
{
    ivec2 depthTexSize = textureSize(uDepthTex, 0);
    vec2 depthTexInv = 1.0 / vec2(depthTexSize);

    ivec2 pixelCoord = ivec2(gl_FragCoord.xy);

    float depth = texelFetch(uDepthTex, pixelCoord, 0).r;
    vec3 position = V_GetViewPosition(vTexCoord, depth);
    vec3 normal = V_GetViewNormal(uNormalTex, pixelCoord);

    float projScale = abs(uView.proj[1][1]) * depthTexSize.y * 0.5;
    float ssRadius = projScale * uSsao.radius / max(depth, 1e-4);
    float radiusSq = uSsao.radius * uSsao.radius;

    float spin = M_TAU * M_HashR2(gl_FragCoord.xy);
    int numSpiralTurns = ROTATIONS[clamp(uSsao.sampleCount - 1, 0, 97)];

    float aoSum = 0.0;
    for (int i = 0; i < uSsao.sampleCount; i++)
    {
        float rNorm;
        vec2 unitDir = TapLocation(i, float(numSpiralTurns), spin, rNorm);
        ivec2 rawOffset = pixelCoord + ivec2(unitDir * ssRadius * rNorm);

        ivec2 pixelOffset = clamp(rawOffset, ivec2(0), depthTexSize - 1);
        bool inBounds = all(equal(pixelOffset, rawOffset));

        float sampleDepth = texelFetch(uDepthTex, pixelOffset, 0).r;
        vec3 samplePos = V_GetViewPosition((vec2(pixelOffset) + 0.5) * depthTexInv, sampleDepth);
        vec3 v = samplePos - position;

        float vv = dot(v, v);
        float vn = dot(v, normal);

        const float epsilon = 0.02;
        float f = max(radiusSq - vv, 0.0);
        float contribution = f * f * f * max((vn - uSsao.bias) / (epsilon + vv), 0.0);

        aoSum += contribution * float(inBounds);
    }

    float temp = radiusSq * uSsao.radius;
    aoSum /= (temp * temp);

    FragOcclusion = max(0.0, 1.0 - aoSum * uSsao.intensity / float(uSsao.sampleCount));
}
