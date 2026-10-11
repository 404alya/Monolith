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

// Local patch 11: baked world-space occlusion (R3D_SetWorldOcclusion)
uniform sampler3D uWorldAoTex;      //< signed distance to the nearest surface, encoded in [0, 1]
uniform bool uWorldAoEnabled;
uniform vec3 uWorldAoMin;           //< volume corner (world)
uniform vec3 uWorldAoInvExtent;     //< 1 / volume size
uniform vec2 uWorldAoDecode;        //< distance (m) = value * x + y
uniform float uWorldAoIntensity;

// ================================
// Helper Includes
// ================================

#include <wrap/view.glsl>
#include <wrap/env.glsl>

// ================================
// World-space occlusion (local patch 11)
// ================================

// Distance-field ambient occlusion: how far the surface is from other surfaces at a few
// heights above it along its normal. A point h metres out from a surface with nothing else
// near is h from every surface; any nearer surface makes it less, and the shortfall is the
// occlusion. Static (a function of the world position and normal only), so it never
// changes when the camera turns, and it sees occluders that are off screen.
float WorldOcclusion(vec3 P, vec3 N)
{
    const float H[5] = float[5](0.15, 0.4, 0.9, 1.8, 3.2);

    vec3 uvw = (P - uWorldAoMin) * uWorldAoInvExtent;
    vec3 edge = min(uvw, 1.0 - uvw);
    float inside = clamp(min(edge.x, min(edge.y, edge.z)) * 32.0, 0.0, 1.0);
    if (inside <= 0.0) return 1.0;

    float occ = 0.0, wsum = 0.0, w = 1.0;
    for (int i = 0; i < 5; i++)
    {
        vec3 q = (P + N * H[i] - uWorldAoMin) * uWorldAoInvExtent;
        float d = texture(uWorldAoTex, q).r * uWorldAoDecode.x + uWorldAoDecode.y;
        occ += w * clamp((H[i] - d) / H[i], 0.0, 1.0);
        wsum += w;
        w *= 0.75;
    }

    float ao = clamp(1.0 - uWorldAoIntensity * occ / wsum, 0.0, 1.0);
    return mix(1.0, ao, inside);
}

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

    if (uWorldAoEnabled)
    {
        orm.x *= WorldOcclusion(P, N);
    }

    vec3 diff = vec3(0.0);
    vec3 spec = vec3(0.0);
    E_ComputeAmbientAndProbes(diff, spec, kD, orm.rgb, F0, P, N, V, NoV);

    FragRadiance = vec4(diff + io.rgb, 1.0);
    FragSpecular = vec4(spec, 1.0);
}
