/* light.glsl -- Light helpers wrapping UBO and LIB for direct use in shaders.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#include "../ubo/light.glsl"
#include "../lib/pbr.glsl"

/* === Lighting === */

vec3 L_Diffuse(float roughness, float NoV, float NoL, float LoH)
{
    return vec3(PBR_Fd_Burley(roughness, NoV, NoL, LoH) * NoL);
}

vec3 L_Specular(vec3 F0, float roughness, float NoV, float NoL, float NoH, float LoH)
{
    roughness = max(roughness, 0.045);
    float alpha = roughness * roughness;

	float D = PBR_D_GGX(NoH, alpha);
	float V = PBR_V_SmithGGXCorrelated(NoV, NoL, alpha);
	vec3  F = PBR_F_Schlick(F0, LoH);

	return (D * V) * F * NoL;
}

/* === Shadows === */

#define SHADOW_SAMPLES 8

const vec2 VOGEL_DISK[8] = vec2[8](
    vec2(0.250000, 0.000000),
    vec2(-0.319290, 0.292496),
    vec2(0.048872, -0.556877),
    vec2(0.402444, 0.524918),
    vec2(-0.738535, -0.130636),
    vec2(0.699605, -0.445031),
    vec2(-0.234004, 0.870484),
    vec2(-0.446271, -0.859268)
);

mat2 L_ShadowDebandingMatrix(vec2 fragCoord)
{
    float r = M_TAU * M_HashIGN(fragCoord);
    float sr = sin(r), cr = cos(r);
    return mat2(vec2(cr, -sr), vec2(sr, cr));
}

float L_SampleShadowDir(Light light, vec3 Pws, float Zvs, float NoL, mat2 diskRot)
{
    vec4 Pls = light.viewProj * vec4(Pws, 1.0);
    vec3 projCoords = Pls.xyz / Pls.w * 0.5 + 0.5;
    float bias = light.shadowDepthBias + light.shadowSlopeBias * (1.0 - NoL);
    float compareDepth = projCoords.z - bias;

    float shadow = 0.0;
    for (int i = 0; i < SHADOW_SAMPLES; ++i)
    {
        vec2 offset = diskRot * VOGEL_DISK[i] * light.shadowSoftness;
        shadow += texture(uShadowDirTex, vec4(projCoords.xy + offset, light.shadowLayer, compareDepth));
    }
    shadow /= float(SHADOW_SAMPLES);

    vec3 distToBorder = min(projCoords, 1.0 - projCoords);
    float edgeFade = smoothstep(0.0, 0.05, min(distToBorder.x, min(distToBorder.y, distToBorder.z)));
    float distFade = smoothstep(light.range, light.range * 0.75, Zvs);

    return mix(1.0, shadow, edgeFade * distFade * light.shadowOpacity);
}

float L_SampleShadowSpot(Light light, vec3 Pws, float NoL, mat2 diskRot)
{
    vec4 Pls = light.viewProj * vec4(Pws, 1.0);
    vec3 projCoords = Pls.xyz / Pls.w * 0.5 + 0.5;
    float bias = light.shadowDepthBias + light.shadowSlopeBias * (1.0 - NoL);
    float compareDepth = projCoords.z - bias;

    float shadow = 0.0;
    for (int i = 0; i < SHADOW_SAMPLES; ++i)
    {
        vec2 offset = diskRot * VOGEL_DISK[i] * light.shadowSoftness;
        shadow += texture(uShadowSpotTex, vec4(projCoords.xy + offset, light.shadowLayer, compareDepth));
    }
    shadow /= float(SHADOW_SAMPLES);

    return mix(1.0, shadow, light.shadowOpacity);
}

float L_SampleShadowOmni(Light light, vec3 Pws, float NoL, mat2 diskRot)
{
    vec3 lightToFrag = Pws - light.position;
    float currentDepth = length(lightToFrag);

    float bias = light.shadowDepthBias + light.shadowSlopeBias * (1.0 - NoL);
    float compareDepth = (currentDepth - bias) / light.shadowFar;

    mat3 OBN = M_OrthonormalBasis(lightToFrag / currentDepth);

    float shadow = 0.0;
    for (int i = 0; i < SHADOW_SAMPLES; ++i)
    {
        vec2 diskOffset = diskRot * VOGEL_DISK[i] * light.shadowSoftness;
        shadow += texture(uShadowOmniTex, vec4(OBN * vec3(diskOffset.xy, 1.0), light.shadowLayer), compareDepth);
    }
    shadow /= float(SHADOW_SAMPLES);

    return mix(1.0, shadow, light.shadowOpacity);
}
