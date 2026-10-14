/* vfog_transmittance.frag -- Fragment shader of volumetric fog transmittance
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

#include <lib/math.glsl>
#include <ubo/view.glsl>
#include <ubo/fx.glsl>
#include <wrap/view.glsl>

// ================================
// In - Varyings
// ================================

noperspective in vec2 vTexCoord;

// ================================
// Out - Fragments
// ================================

out vec4 FragColor; // rgb = emission, a = transmittance (composed via blend mode)

// ================================
// Samplers & Uniforms
// ================================

uniform sampler2D uDepthTex;

// ================================
// Main Function
// ================================

void main()
{
    float depth = texelFetch(uDepthTex, ivec2(gl_FragCoord.xy), 0).r;
    // Distance along the view ray, not view depth: view depth changes when the camera
    // only turns, and the radiance march integrates over distance (local patch 4)
    float rayDist = min(length(V_GetViewPosition(vTexCoord, depth)), uVFog.maxDistance);

    // Analytic transmittance over the full ray distance
    float sigmaT = uVFog.scatteringDensity + uVFog.absortionDensity;
    float transmittance = exp(-sigmaT * rayDist);

    // Density fade (local patch 13): the density is 1 - smoothstep(fadeStart, fadeEnd, t),
    // so the optical depth is sigmaT times its integral over [0, rayDist]. With
    // s(u) = 3u^2 - 2u^3 and w = fadeEnd - fadeStart: int_0^u s = u^3 - u^4 / 2.
    // Near fade-in (local patch 15): the density is also multiplied by s(t / fadeNear), which
    // removes n (v - v^3 + v^4 / 2) of it (v = clamped d / n). Exact while fadeNear <=
    // fadeStart (the two ramps don't overlap); monolith clamps it to that.
    if (uVFog.fadeEnd > 0.0 || uVFog.fadeNear > 0.0)
    {
        float depthInt = rayDist;
        if (uVFog.fadeEnd > 0.0)
        {
            float a = min(uVFog.fadeStart, rayDist);
            float w = max(uVFog.fadeEnd - uVFog.fadeStart, 1e-3);
            float u = clamp((rayDist - uVFog.fadeStart) / w, 0.0, 1.0);
            depthInt = a + w * (u - (u * u * u - 0.5 * u * u * u * u));
        }
        if (uVFog.fadeNear > 0.0)
        {
            float n = uVFog.fadeNear;
            float v = clamp(rayDist / n, 0.0, 1.0);
            depthInt = max(depthInt - n * (v - (v * v * v - 0.5 * v * v * v * v)), 0.0);
        }
        transmittance = exp(-sigmaT * depthInt);
    }

    // Emission fills the volume proportionally to extinction
    float extinction = 1.0 - transmittance;
    vec3 emission = uVFog.emissionColor.rgb * (uVFog.emissionEnergy * extinction);

    // Scale effect on sky pixels
    if (depth >= uView.far)
    {
        transmittance = mix(1.0, transmittance, uVFog.skyAffect);  // 0 = no effect, 1 = full effect
        emission *= uVFog.skyAffect;                               // mix(vec3(0), emission, t) == emission * t
    }

    FragColor = vec4(emission, transmittance);
}
