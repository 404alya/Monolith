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

// Local patch 25: the sun (the first visible directional light, in uLight) is lit here, not
// in the lights pass, which saves its G-buffer read and its blend into both targets
uniform sampler2DArrayShadow uShadowDirTex;
uniform sampler2DArrayShadow uShadowSpotTex;    //< unused, for wrap/light.glsl
uniform samplerCubeArrayShadow uShadowOmniTex;  //< unused, for wrap/light.glsl
uniform sampler2DArray uShadowDirMinMaxTex;     //< RG = min/max depth per 16x16 texels
uniform bool uSunEnabled;
uniform bool uSunMinMax;                        //< uShadowDirMinMaxTex is valid

// Local patch 26: without SSR this pass also does compose.frag's work (scene = radiance +
// specular): it reads the two targets (emission and the lights pass) and writes the scene
uniform sampler2D uRadianceTex;
uniform sampler2D uSpecularTex;
uniform bool uCompose;

// ================================
// Helper Includes
// ================================

#include <wrap/view.glsl>
#include <wrap/env.glsl>
#include <wrap/light.glsl>

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
// Sun (local patch 25)
// ================================

// L_SampleShadowDir with the min/max shortcut of vfog_radiance.frag (VFog_SampleShadowDir):
// where the reference depth is in front of (or behind) every texel the 8 PCF taps can
// touch, each tap returns exactly 1 (or 0), so they are skipped. Same result.
float SunShadow(vec3 Pws, float Zvs, float NoL, mat2 diskRot)
{
    vec4 Pls = uLight.viewProj * vec4(Pws, 1.0);
    vec3 projCoords = Pls.xyz / Pls.w * 0.5 + 0.5;
    float bias = uLight.shadowDepthBias + uLight.shadowSlopeBias * (1.0 - NoL);
    float compareDepth = projCoords.z - bias;

    bool done = false;
    float shadow = 0.0;

    if (uSunMinMax)
    {
        const int TILE_SIZE = 16;   // must match R3D_SHADOW_MINMAX_TILE
        float mapSize = float(textureSize(uShadowDirTex, 0).x);
        float radius = uLight.shadowSoftness * mapSize + 2.0;
        vec2 center = projCoords.xy * mapSize - 0.5;
        ivec2 last = ivec2(textureSize(uShadowDirTex, 0).xy) - 1;
        ivec2 t0 = clamp(ivec2(floor(center - radius)), ivec2(0), last) / TILE_SIZE;
        ivec2 t1 = clamp(ivec2(floor(center + radius)) + 1, ivec2(0), last) / TILE_SIZE;

        vec2 a = texelFetch(uShadowDirMinMaxTex, ivec3(t0.x, t0.y, uLight.shadowLayer), 0).rg;
        vec2 b = texelFetch(uShadowDirMinMaxTex, ivec3(t1.x, t0.y, uLight.shadowLayer), 0).rg;
        vec2 c = texelFetch(uShadowDirMinMaxTex, ivec3(t0.x, t1.y, uLight.shadowLayer), 0).rg;
        vec2 d = texelFetch(uShadowDirMinMaxTex, ivec3(t1.x, t1.y, uLight.shadowLayer), 0).rg;
        float dMin = min(min(a.x, b.x), min(c.x, d.x));
        float dMax = max(max(a.y, b.y), max(c.y, d.y));

        const float EPS = 2.0 / 65535.0;
        float ref = clamp(compareDepth, 0.0, 1.0);
        bool covered = all(lessThanEqual(t1 - t0, ivec2(1)));

        if (covered && ref <= dMin - EPS) { shadow = 1.0; done = true; }
        else if (covered && ref > dMax + EPS) { shadow = 0.0; done = true; }
    }

    if (!done)
    {
        for (int i = 0; i < SHADOW_SAMPLES; ++i)
        {
            vec2 offset = diskRot * VOGEL_DISK[i] * uLight.shadowSoftness;
            shadow += texture(uShadowDirTex, vec4(projCoords.xy + offset, uLight.shadowLayer, compareDepth));
        }
        shadow /= float(SHADOW_SAMPLES);
    }

    vec3 distToBorder = min(projCoords, 1.0 - projCoords);
    float edgeFade = smoothstep(0.0, SHADOW_DIR_EDGE_FADE, min(distToBorder.x, min(distToBorder.y, distToBorder.z)));
    float distFade = smoothstep(uLight.range, uLight.range * 0.75, Zvs);

    return mix(1.0, shadow, edgeFade * distFade * uLight.shadowOpacity);
}

// The directional path of lighting.frag, unchanged (its own NoV, the unmodified ORM)
void SunLight(vec3 P, vec3 N, vec3 albedo, vec4 orm, out vec3 diffOut, out vec3 specOut)
{
    diffOut = vec3(0.0);
    specOut = vec3(0.0);

    if (any(lessThan(P, uLight.clipMin)) || any(greaterThan(P, uLight.clipMax))) return;

    vec3 L = -uLight.direction;
    float NoL = dot(N, L);
    if (NoL <= 0.0) return;

    vec3 V = normalize(uView.position - P);
    float NoV = max(dot(N, V), 1e-4);
    vec3 H = normalize(V + L);
    float LoH = max(dot(L, H), 0.0);
    float NoH = max(dot(N, H), 0.0);

    vec3 lightColE = uLight.color * uLight.energy;

    vec3 diff = L_Diffuse(orm.g, NoV, NoL, LoH);
    diff *= albedo * lightColE * (1.0 - orm.b);

    vec3 F0 = PBR_F0(orm.b, orm.w, albedo);
    vec3 spec = L_Specular(F0, orm.g, NoV, NoL, NoH, LoH);
    spec *= lightColE * uLight.specular;

    float shadow = 1.0;
    if (uLight.shadowLayer >= 0 && uLight.shadowOpacity != 0.0)
    {
        mat2 diskRot = L_ShadowDebandingMatrix(gl_FragCoord.xy);
        shadow *= SunShadow(P, length(P - uView.position), NoL, diskRot);
    }

    diffOut = diff * shadow;
    specOut = spec * shadow;
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

    vec3 sunDiff = vec3(0.0), sunSpec = vec3(0.0);
    if (uSunEnabled) SunLight(P, N, albedo, orm, sunDiff, sunSpec);   // before the AO changes orm

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

    vec3 outDiff = diff + io.rgb + sunDiff;
    vec3 outSpec = spec + sunSpec;

    if (uCompose)
    {
        // Local patch 26: compose.frag without SSR (its mix with a blank SSR is the specular)
        vec3 radiance = texelFetch(uRadianceTex, pixCoord, 0).rgb + outDiff;
        vec3 specular = texelFetch(uSpecularTex, pixCoord, 0).rgb + outSpec;
        FragRadiance = vec4(radiance + specular, 1.0);
        FragSpecular = vec4(0.0);   // no attachment
    }
    else
    {
        FragRadiance = vec4(outDiff, 1.0);
        FragSpecular = vec4(outSpec, 1.0);
    }
}
