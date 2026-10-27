/* vfog_compose.frag -- Fragment shader for volumetric fog radiance composition
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#version 330 core

// Local patch 24: one pass instead of three full-screen blends into the scene (distance fog,
// vfog transmittance, this composition), which only move pixels on bandwidth-bound GPUs.
// They were:
//   fog.frag:                scene = mix(scene, fogColor, f)            (alpha: f + a (1 - f))
//   vfog_transmittance.frag: scene = emission + scene T                 (alpha kept)
//   this pass:               scene = scene + radiance                   (alpha kept)
// Together: scene * (1 - f) T + (fogColor f T + emission + radiance), alpha f + a (1 - f),
// one dual-source blend (GL_ONE, GL_SRC1_COLOR / GL_ONE, GL_SRC1_ALPHA). The code of each
// term is unchanged from its shader; only the half-float roundings in between are gone.

// ================================
// Includes
// ================================

#include <lib/sampling.glsl>
#include <lib/color.glsl>
#include <ubo/view.glsl>
#include <wrap/fog.glsl>
#include <wrap/view.glsl>

// ================================
// In - Varyings
// ================================

noperspective in vec2 vTexCoord;

// ================================
// Out - Fragments
// ================================

layout(location = 0, index = 0) out vec4 FragColor;     // rgb = added light, a = fog factor
layout(location = 0, index = 1) out vec4 FragFactor;    // rgb = scene factor, a = 1 - fog factor

// ================================
// Samplers & Uniforms
// ================================

uniform sampler2D uRadianceTex;
uniform sampler2D uDepthTex;

// ================================
// Main Function
// ================================

void main()
{
    float refDepth = texelFetch(uDepthTex, ivec2(gl_FragCoord.xy), 0).r;
    bool sky = refDepth >= uView.far;

    // Distance from the eye, as in fog.frag and vfog_transmittance.frag (local patch 4)
    float dist = length(V_GetViewPosition(vTexCoord, refDepth));

    /* --- Distance fog (fog.frag) --- */

    float fogFactor = 0.0;
    if (uFog.mode != FOG_DISABLED)
    {
        fogFactor = sky ? uFog.skyAffect : FogFactor(dist);
    }

    /* --- Transmittance and emission (vfog_transmittance.frag) --- */

    float rayDist = min(dist, uVFog.maxDistance);

    float sigmaT = uVFog.scatteringDensity + uVFog.absortionDensity;
    float transmittance = exp(-sigmaT * rayDist);

    // Density fade (local patches 13, 15): see vfog_transmittance.frag
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

    float extinction = 1.0 - transmittance;
    vec3 emission = uVFog.emissionColor.rgb * (uVFog.emissionEnergy * extinction);

    if (sky)
    {
        transmittance = mix(1.0, transmittance, uVFog.skyAffect);
        emission *= uVFog.skyAffect;
    }

    /* --- In-scattered radiance (the original composition) --- */

    float depthSharpness = 1.0 / max(refDepth * 0.1, 0.05);
    vec3 radiance = C_UnTonemap(S_Upsample(uRadianceTex, uDepthTex, vTexCoord, refDepth, depthSharpness)).rgb;

    /* --- Combined blend --- */

    FragColor = vec4(uFog.color.rgb * (fogFactor * transmittance) + emission + radiance, fogFactor);
    FragFactor = vec4(vec3((1.0 - fogFactor) * transmittance), 1.0 - fogFactor);
}
