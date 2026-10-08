/* env.glsl -- Env helpers wrapping UBO and LIB for direct use in shaders.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#include "../ubo/env.glsl"
#include "../lib/ibl.glsl"

vec4 E_SampleIlluminationProbe(E_Probe probe, vec3 P, vec3 N)
{
    float dist   = length(P - probe.position);
    float weight = pow(clamp(1.0 - dist / probe.range, 0.0, 1.0), probe.falloff);

    if (weight < 1e-4) return vec4(0.0);

    vec3 irradiance = IBL_SampleIrradiance(uIrradianceTex, probe.layer, N);

    return vec4(irradiance * weight, weight);
}

vec4 E_SampleReflectionProbe(E_Probe probe, float roughness, vec3 P, vec3 N, vec3 V)
{
    float dist = length(P - probe.position);
    float weight = pow(clamp(1.0 - dist / probe.range, 0.0, 1.0), probe.falloff);

    if (weight < 1e-4) return vec4(0.0);

    vec3 prefilter = IBL_SamplePrefilter(uPrefilterTex, probe.layer, V, N, roughness, uNumPrefilterLevels);

    return vec4(prefilter * weight, weight);
}

void E_ComputeAmbientAndProbes(inout vec3 outDiff, inout vec3 outSpec, vec3 kD, vec3 orm, vec3 F0, vec3 P, vec3 N, vec3 V, float NoV)
{
    float occlusion = orm.x;
    float roughness = orm.y;
    float metalness = orm.z;

    vec4 diff = vec4(0.0);
    vec4 spec = vec4(0.0);

    for (int i = 0; i < uNumIlluminationProbes; ++i)
    {
        diff += E_SampleIlluminationProbe(uIlluminationProbes[i], P, N);
    }

    if (diff.w < 1.0)
    {
        float weight = 1.0 - diff.w;

        if (uAmbient.irradiance >= 0)
        {
            diff.rgb += weight * IBL_SampleIrradiance(uIrradianceTex, uAmbient.irradiance, N, uAmbient.rotation);
        }
        else
        {
            diff.rgb += weight * uAmbient.color.rgb;
        }
    }
    else
    {
        diff.rgb /= diff.w;
    }

    for (int i = 0; i < uNumReflectionProbes; ++i)
    {
        spec += E_SampleReflectionProbe(uReflectionProbes[i], roughness, P, N, V);
    }

    if (spec.w < 1.0)
    {
        if (uAmbient.irradiance >= 0)
        {
            spec.rgb += (1.0 - spec.w) * IBL_SamplePrefilter(uPrefilterTex, uAmbient.prefilter, V, N, uAmbient.rotation, roughness, uNumPrefilterLevels);
        }
    }
    else
    {
        spec.rgb /= spec.w;
    }

    diff.rgb *= occlusion * uAmbient.energy;
    spec.rgb *= IBL_GetSpecularOcclusion(NoV, occlusion, roughness);

    vec2 brdf = texture(uBrdfLutTex, vec2(NoV, roughness)).xy;
    IBL_MultiScattering(diff.rgb, spec.rgb, kD, F0, brdf, NoV);

    outDiff += diff.rgb;
    outSpec += spec.rgb;
}

void E_ComputeAmbientOnly(inout vec3 outDiff, inout vec3 outSpec, vec3 kD, vec3 orm, vec3 F0, vec3 P, vec3 N, vec3 V, float NoV)
{
    float occlusion = orm.x;
    float roughness = orm.y;
    float metalness = orm.z;

    vec3 diff = vec3(0.0);
    vec3 spec = vec3(0.0);

    if (uAmbient.irradiance >= 0)
    {
        diff = IBL_SampleIrradiance(uIrradianceTex, uAmbient.irradiance, N, uAmbient.rotation).rgb;
        diff *= occlusion * uAmbient.energy;
    }
    else
    {
        diff = uAmbient.color.rgb * uAmbient.energy * occlusion;
    }

    if (uAmbient.prefilter >= 0)
    {
        spec  = IBL_SamplePrefilter(uPrefilterTex, uAmbient.prefilter, V, N, uAmbient.rotation, roughness, uNumPrefilterLevels).rgb;
        spec *= IBL_GetSpecularOcclusion(NoV, occlusion, roughness);
    }

    vec2 brdf = texture(uBrdfLutTex, vec2(NoV, roughness)).xy;
    IBL_MultiScattering(diff, spec, kD, F0, brdf, NoV);

    outDiff += diff;
    outSpec += spec;
}

void E_ComputeAmbientColor(inout vec3 outDiff, vec3 kD, float occlusion)
{
    outDiff += kD * uAmbient.color.rgb * uAmbient.energy * occlusion;
}
