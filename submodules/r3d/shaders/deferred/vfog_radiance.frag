/* vfog_radiance.frag -- Fragment shader of volumetric fog radiance
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

out vec3 FragRadiance;

// ================================
// Samplers & Uniforms
// ================================

uniform sampler2D uDepthTex;

uniform sampler2DArrayShadow uShadowDirTex;
uniform sampler2DArrayShadow uShadowSpotTex;
uniform samplerCubeArrayShadow uShadowOmniTex;

uniform sampler2DArray uShadowDirMinMaxTex;     // RG = min/max depth per 16x16 texels (local patch)

// Baked sun visibility of directional lights (local patch, see patches.md): R = mean
// shadow visibility per voxel over the box [uFogSunVolMin, uFogSunVolMin + extent]
uniform sampler3D uFogSunVolTex;
uniform bool uFogSunVolEnabled;
uniform vec3 uFogSunVolMin;
uniform vec3 uFogSunVolInvExtent;   // 1 / extent
uniform vec3 uFogSunVolHalfTexel;   // half a voxel in texture coordinates

// ================================
// Helper Includes
// ================================

#include <wrap/light.glsl>
#include <wrap/view.glsl>

// ================================
// Helper Functions
// ================================

float PhaseFunction_Schlick(vec3 w0, vec3 w1)
{
    // Both vectors assumed normalized; skip length division
    float cosTheta = dot(w0, w1);
    float nom = 1.0 - uVFog.anisotropy * uVFog.anisotropy;
    float denom = 4.0 * M_PI * (1.0 + uVFog.anisotropy * cosTheta) * (1.0 + uVFog.anisotropy * cosTheta);
    return nom / denom;
}

// L_SampleShadowDir (NoL = 1) with an exact shortcut (local patch, see patches.md):
// when the reference depth is in front of (or behind) every texel the 8 PCF taps can
// touch, each tap returns exactly 1 (or 0), so the taps are skipped. The texels come
// from the min/max depth tiles of uShadowDirMinMaxTex, 2x2 tiles cover the footprint.
// fadeDist replaces L_SampleShadowDir's view depth (local patch 4): the distance from
// the eye to the surface behind the march, so the fade doesn't change when the camera
// only turns. Within that distance (< range) every step on screen lies well inside the
// camera-fitted map, and inside monolith's static tiles (patch 7) as well, so edgeFade
// is 1 there and doesn't depend on the camera either.
float VFog_SampleShadowDir(vec3 Pws, float fadeDist, mat2 diskRot)
{
    vec4 Pls = uLight.viewProj * vec4(Pws, 1.0);
    vec3 projCoords = Pls.xyz / Pls.w * 0.5 + 0.5;
    float bias = uLight.shadowDepthBias + uLight.shadowSlopeBias * (1.0 - 1.0);
    float compareDepth = projCoords.z - bias;

    // Footprint in texels: largest Vogel offset (< 1.0 * softness) plus the bilinear
    // neighbour, with margin. Clamped like CLAMP_TO_EDGE clamps the taps.
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

    // The hardware compares clamp(ref, 0, 1) <= stored; keep 2 units of the 16-bit
    // depth format as margin so rounding of the reference can't flip a tap
    const float EPS = 2.0 / 65535.0;
    float ref = clamp(compareDepth, 0.0, 1.0);

    // 2x2 tiles only cover footprints narrower than a tile (softness up to ~5 texels)
    bool covered = all(lessThanEqual(t1 - t0, ivec2(1)));

    float shadow;
    if (covered && ref <= dMin - EPS) shadow = 1.0;
    else if (covered && ref > dMax + EPS) shadow = 0.0;
    else
    {
        shadow = 0.0;
        for (int i = 0; i < SHADOW_SAMPLES; ++i)
        {
            vec2 offset = diskRot * VOGEL_DISK[i] * uLight.shadowSoftness;
            shadow += texture(uShadowDirTex, vec4(projCoords.xy + offset, uLight.shadowLayer, compareDepth));
        }
        shadow /= float(SHADOW_SAMPLES);
    }

    vec3 distToBorder = min(projCoords, 1.0 - projCoords);
    float edgeFade = smoothstep(0.0, SHADOW_DIR_EDGE_FADE, min(distToBorder.x, min(distToBorder.y, distToBorder.z)));
    float distFade = smoothstep(uLight.range, uLight.range * 0.75, fadeDist);

    return mix(1.0, shadow, edgeFade * distFade * uLight.shadowOpacity);
}

// Directional shadow visibility at Pws: one trilinear fetch of the baked volume inside
// it (local patch, see patches.md), else VFog_SampleShadowDir. Same fades as there:
// distFade by fadeDist; edgeFade is 1 wherever the shadow is weighted (see above).
float VFog_SunVisibility(vec3 Pws, float fadeDist, mat2 diskRot)
{
    vec3 uvw = (Pws - uFogSunVolMin) * uFogSunVolInvExtent;
    if (uFogSunVolEnabled && all(greaterThanEqual(uvw, uFogSunVolHalfTexel)) && all(lessThanEqual(uvw, 1.0 - uFogSunVolHalfTexel)))
    {
        float shadow = texture(uFogSunVolTex, uvw).r;
        float distFade = smoothstep(uLight.range, uLight.range * 0.75, fadeDist);
        return mix(1.0, shadow, distFade * uLight.shadowOpacity);
    }
    return VFog_SampleShadowDir(Pws, fadeDist, diskRot);
}

// Distance fade of the fog density (local patch 13, see patches.md): 1 up to
// uVFog.fadeStart from the eye, smoothstep down to 0 at uVFog.fadeEnd. Local patch 15:
// with uVFog.fadeNear > 0 it also fades in, smoothstep from 0 at the eye to 1 at fadeNear,
// so the fog around the eye (sunlit or not) doesn't veil the whole view. Only called when
// a fade is set; without one the march below runs the unpatched arithmetic.
float VFog_DensityFade(float t)
{
    float f = 1.0;
    if (uVFog.fadeEnd > 0.0)
    {
        float u = clamp((t - uVFog.fadeStart) / max(uVFog.fadeEnd - uVFog.fadeStart, 1e-3), 0.0, 1.0);
        f = 1.0 - u * u * (3.0 - 2.0 * u);
    }
    if (uVFog.fadeNear > 0.0)
    {
        float v = clamp(t / uVFog.fadeNear, 0.0, 1.0);
        f *= v * v * (3.0 - 2.0 * v);
    }
    return f;
}

// Computes the march range [tEnter, tExit] along rayDir (from rayOrigin),
// clamped to uVFog.maxDistance and to the light's range sphere when applicable.
// tEnter/tExit are both 0.0 when the ray never enters the light's range
void VFog_GetMarchRange(vec3 rayOrigin, vec3 rayDir, float maxDist, out float tEnter, out float tExit)
{
    if (uLight.type == LIGHT_DIR)
    {
        tEnter = 0.0;
        tExit = maxDist;
        return;
    }

    vec3 toLight = uLight.position - rayOrigin;
    float tCenter = dot(toLight, rayDir);
    float distToRaySq = dot(toLight, toLight) - tCenter * tCenter;
    float rangeSq = uLight.range * uLight.range;

    if (distToRaySq >= rangeSq)
    {
        tEnter = 0.0;
        tExit = 0.0;
        return;
    }

    float halfChord = sqrt(rangeSq - distToRaySq);

    tEnter = max(tCenter - halfChord, 0.0);
    tExit = min(tCenter + halfChord, maxDist);

    // Clip box (local patch 16): march only where the ray is inside it. Lights without one
    // have +-1e30 bounds; the slab test is skipped for them, so their march is unchanged.
    if (uLight.clipMin.x > -1e29)
    {
        vec3 invDir = 1.0 / rayDir;   // +-inf on axis-parallel rays: the slab test still holds
        vec3 t0 = (uLight.clipMin - rayOrigin) * invDir;
        vec3 t1 = (uLight.clipMax - rayOrigin) * invDir;
        vec3 tMin = min(t0, t1), tMax = max(t0, t1);
        tEnter = max(tEnter, max(max(tMin.x, tMin.y), tMin.z));
        tExit = min(tExit, min(min(tMax.x, tMax.y), tMax.z));
        if (tExit <= tEnter) { tEnter = 0.0; tExit = 0.0; }
    }
}

// ================================
// Main Function
// ================================

void main()
{
    ivec2 pixCoord = ivec2(gl_FragCoord.xy);

    float depth = texelFetch(uDepthTex, pixCoord, 0).r;
    vec3 P = V_GetWorldPosition(vTexCoord, depth);

    vec3 toFrag = P - uView.position;
    float fragDist = length(toFrag);
    vec3 rayDir = toFrag / max(fragDist, 1e-6);

    float tEnter, tExit;
    VFog_GetMarchRange(uView.position, rayDir, uVFog.maxDistance, tEnter, tExit);

    tExit = min(fragDist, tExit);
    float rayDist = tExit - tEnter;

    // Early-out: this light's range sphere doesn't intersect the view ray
    if (rayDist <= 0.0)
    {
        FragRadiance = vec3(0.0);
        return;
    }

    vec3 marchPos = uView.position + rayDir * tEnter;
    vec3 deltaStep = rayDir * uVFog.stepSize;
    vec3 fragToCameraNorm = -rayDir; // view direction along the ray

    // Jitter starting position to break up banding, adjust ray length to avoid overshooting
    float jitter = M_HashIGN(gl_FragCoord.xy);
    marchPos += deltaStep * jitter;
    rayDist -= uVFog.stepSize * jitter;

    vec3 radiance = vec3(0.0);
    float transmittance = 1.0;

    // Density fade (local patch 13): t = distance of the march position from the eye.
    // Past fadeEnd the density is 0, so those steps add nothing: stop the march there.
    // A near fade-in alone (local patch 15) takes the same per-step path.
    bool fadeOn = uVFog.fadeEnd > 0.0 || uVFog.fadeNear > 0.0;
    float tStart = tEnter + uVFog.stepSize * jitter;
    if (uVFog.fadeEnd > 0.0) rayDist = min(rayDist, uVFog.fadeEnd - tStart);

    // Extinction coefficient and per-step transmittance factor (constant along the ray)
    float sigmaT = uVFog.scatteringDensity + uVFog.absortionDensity;
    float transmittanceStep = exp(-sigmaT * uVFog.stepSize);

    // Precalculate constant terms
    vec3 lightColor = uLight.color * uLight.energy * uLight.fogEnergy;
    mat2 diskRot = L_ShadowDebandingMatrix(gl_FragCoord.xy);
    bool hasShadow = uLight.shadowLayer >= 0 && uLight.shadowOpacity != 0.0;

    // VFog_SampleShadowDir fades the shadow out by the distance to the surface behind the
    // march (constant along the ray) and weights it by exactly 0 at fragDist >= range,
    // e.g. on sky pixels. Skip its taps there: same result, no wasted PCF lookups.
    // With a density fade (local patch 14) the shadow fades by each step's own distance
    // from the eye instead, so the per-step branch below decides.
    if (uLight.type == LIGHT_DIR && fragDist >= uLight.range && !fadeOn) hasShadow = false;

    // Precompute inverses used inside the loop
    float invRange = 1.0 / max(uLight.range, 1e-4);
    float invEpsilon = 1.0 / max(uLight.innerCutOff - uLight.outerCutOff, 1e-4);

    // Directional light (local patch 9, see patches.md): L, and so the phase function and
    // the whole in-scatter factor, are constant along the ray; only the visibility varies.
    // Same arithmetic in the same order as the generic loop below, just hoisted.
    if (uLight.type == LIGHT_DIR)
    {
        float phase = PhaseFunction_Schlick(-uLight.direction, fragToCameraNorm);
        vec3 LiDir = uVFog.scatteringDensity * uVFog.scatteringColor.rgb * phase * lightColor;

        // With a density fade (local patch 13) the per-step factors vary: plain loop.
        if (fadeOn)
        {
            // Shadow fade by the step's distance from the eye (local patch 14), not by
            // fragDist: fragDist is constant along the ray, so all fog in front of a surface
            // past 0.75 range went fully sunlit at once, a hard line across the haze where
            // the ground crosses that distance. Steps closer than range lie inside the shadow
            // map's fade-free area (static tiles and camera fit alike), so edgeFade stays 1.
            for (float l = 0; l < rayDist; l += uVFog.stepSize)
            {
                float t = tStart + l;
                float visibility = (hasShadow && t < uLight.range) ? VFog_SunVisibility(marchPos, t, diskRot) : 1.0;
                float fade = VFog_DensityFade(t);

                transmittance *= exp(-sigmaT * uVFog.stepSize * fade);
                radiance += LiDir * (visibility * fade) * transmittance * uVFog.stepSize;

                marchPos += deltaStep;
            }
            rayDist = 0.0;
        }
        // Unshadowed rays (sky, surfaces >= range away): every step adds LiDir times the
        // transmittance so far, a geometric series with a closed form. n = the loop's
        // step count (equal up to float rounding of the accumulated l).
        else if (!hasShadow)
        {
            float n = ceil(rayDist / uVFog.stepSize);
            float series = (transmittanceStep < 0.99999)
                ? transmittanceStep * (1.0 - pow(transmittanceStep, n)) / (1.0 - transmittanceStep)
                : n;    // no extinction: n steps of 1
            radiance = LiDir * series * uVFog.stepSize;
            rayDist = 0.0;
        }

        for (float l = 0; l < rayDist; l += uVFog.stepSize)
        {
            float visibility = VFog_SunVisibility(marchPos, fragDist, diskRot);
            vec3 Li = LiDir * visibility;

            transmittance *= transmittanceStep;
            radiance += Li * transmittance * uVFog.stepSize;

            marchPos += deltaStep;
        }

        rayDist = 0.0;  // skip the generic loop
    }

    for (float l = 0; l < rayDist; l += uVFog.stepSize)
    {
        // Compute light direction and distance at each march step
        vec3 Ldelta = uLight.position - marchPos;
        float Ldist = length(Ldelta);
        float invLdist = 1.0 / max(Ldist, 1e-4);
        vec3 L = (uLight.type == LIGHT_DIR) ? -uLight.direction : Ldelta * invLdist;

        // Shadow visibility at current march position
        float visibility = 1.0;
        if (hasShadow)
        {
            switch (uLight.type)
            {
            case LIGHT_DIR:  visibility *= VFog_SunVisibility(marchPos, fragDist, diskRot); break;
            case LIGHT_SPOT: visibility *= L_SampleShadowSpot(uLight, marchPos, 1.0, diskRot); break;
            case LIGHT_OMNI: visibility *= L_SampleShadowOmni(uLight, marchPos, 1.0, diskRot); break;
            }
        }

        // Range attenuation (omni and spot only)
        if (uLight.type != LIGHT_DIR)
        {
            float atten = pow(1.0 - clamp(Ldist * invRange, 0.0, 1.0), uLight.falloff);
            visibility *= atten;
        }

        // Angular attenuation (spot only)
        if (uLight.type == LIGHT_SPOT)
        {
            float theta = dot(L, -uLight.direction);
            visibility *= smoothstep(0.0, 1.0, (theta - uLight.outerCutOff) * invEpsilon);
        }

        // In-scattering: phase function * scattering coefficient * incident radiance
        float phase = PhaseFunction_Schlick(L, fragToCameraNorm);
        vec3 Li = uVFog.scatteringDensity * uVFog.scatteringColor.rgb * phase * lightColor * visibility;

        // Accumulate transmittance then integrate radiance (order matters)
        if (fadeOn)
        {
            float fade = VFog_DensityFade(tStart + l);
            Li *= fade;
            transmittance *= exp(-sigmaT * uVFog.stepSize * fade);
        }
        else transmittance *= transmittanceStep;
        radiance += Li * transmittance * uVFog.stepSize;

        marchPos += deltaStep;
    }

    // Attenuate contribution on sky pixels
    FragRadiance = (depth >= uView.far) ? radiance * uVFog.skyAffect : radiance;
    FragRadiance = C_Tonemap(FragRadiance);
}
