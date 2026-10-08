/* ambient.frag -- Fragment shader for applying ambient lighting for deferred shading
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#version 330 core

// ================================
// Extensions
// ================================

#extension GL_ARB_texture_cube_map_array : enable

// ================================
// Includes
// ================================

#include <lib/sampling.glsl>
#include <lib/color.glsl>
#include <lib/math.glsl>
#include <lib/pbr.glsl>
#include <ubo/fx.glsl>

// ================================
// In - Varyings
// ================================

noperspective in vec2 vTexCoord;

// ================================
// Out - Fragments
// ================================

layout(location = 0) out vec4 FragRadiance;
layout(location = 1) out vec4 FragSpecular;

// ================================
// Samplers & Uniforms
// ================================

uniform sampler2D uAlbedoTex;
uniform sampler2D uNormalTex;
uniform sampler2D uDepthTex;
uniform sampler2D uSsaoTex;
uniform sampler2D uSsgiTex;
uniform sampler2D uSsilTex;
uniform sampler2D uOrmTex;

uniform samplerCubeArray uIrradianceTex;
uniform samplerCubeArray uPrefilterTex;
uniform sampler2D uBrdfLutTex;

// ================================
// Helper Includes
// ================================

#include <wrap/view.glsl>
#include <wrap/env.glsl>

// ================================
// Main Function
// ================================

void main()
{
    ivec2 pixCoord = ivec2(gl_FragCoord.xy);

    vec3 albedo = texelFetch(uAlbedoTex, pixCoord, 0).rgb;
    float depth = texelFetch(uDepthTex, pixCoord, 0).r;
    vec4 orm = texelFetch(uOrmTex, pixCoord, 0);

    vec3 P = V_GetWorldPosition(vTexCoord, depth);
    vec3 N = V_GetWorldNormal(uNormalTex, pixCoord);
    vec3 V = normalize(uView.position - P);
    float NoV = max(dot(N, V), 0.0);

    vec3 F0 = PBR_F0(orm.z, orm.w, albedo);
    vec3 kD = albedo * (1.0 - orm.z);

    vec4 io = vec4(0.0, 0.0, 0.0, 1.0);
    if (uSsil.enabled || uSsao.enabled || uSsgi.enabled)
    {
        const float kMinNdotV = 0.05;
        const float kDepthEdgeToleranceMeters = 0.05;
        float depthSharpness = max(NoV, kMinNdotV) / kDepthEdgeToleranceMeters;
        S_UpsampleWeights uw = S_ComputeUpsampleWeights(uDepthTex, vTexCoord, depth, depthSharpness);

        if (uSsil.enabled)
        {
            io = S_Upsample(uSsilTex, uw);
            io.rgb = C_UnTonemap(io.rgb);
            io.rgb *= uSsil.giIntensity;
            io.a = pow(io.a, uSsil.aoPower);
        }

        if (uSsao.enabled)
        {
            float ao = S_Upsample(uSsaoTex, uw).r;
            io.a *= pow(ao, uSsao.power);
        }

        if (uSsgi.enabled)
        {
            vec3 gi = S_Upsample(uSsgiTex, uw).rgb;
            gi = C_UnTonemap(gi) * uSsgi.intensity;
            io.rgb += gi;
        }

        orm.x *= io.a;
        io.rgb *= kD;
    }

    vec3 diff = vec3(0.0);
    vec3 spec = vec3(0.0);
    E_ComputeAmbientAndProbes(diff, spec, kD, orm.rgb, F0, P, N, V, NoV);

    FragRadiance = vec4(diff + io.rgb, 1.0);
    FragSpecular = vec4(spec, 1.0);
}
