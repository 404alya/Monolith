/* lighting.frag -- Fragment shader for applying direct lighting for deferred shading
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

#include <lib/math.glsl>
#include <lib/pbr.glsl>

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
// Uniforms
// ================================

uniform sampler2D uAlbedoTex;
uniform sampler2D uNormalTex;
uniform sampler2D uDepthTex;
uniform sampler2D uOrmTex;

uniform sampler2DArrayShadow uShadowDirTex;
uniform sampler2DArrayShadow uShadowSpotTex;
uniform samplerCubeArrayShadow uShadowOmniTex;

// ================================
// Helper Includes
// ================================

#include <wrap/light.glsl>
#include <wrap/view.glsl>

// ================================
// Main Function
// ================================

void main()
{
    ivec2 pixCoord = ivec2(gl_FragCoord.xy);

    /* Get position and normal in world space */

    float depth = texelFetch(uDepthTex, pixCoord, 0).r;
    vec3 N = V_GetWorldNormal(uNormalTex, pixCoord);
    vec3 P = V_GetWorldPosition(vTexCoord, depth);

    /* Compute light direction and the dot product of the normal and light direction */

    vec3 Ldelta = uLight.position - P;
    float Ldist = length(Ldelta);

    vec3 L = (uLight.type == LIGHT_DIR) ? -uLight.direction : Ldelta / max(Ldist, 1e-4);
    float NoL = dot(N, L);

    if (NoL <= 0.0)
    {
        FragRadiance = vec4(0.0);
        FragSpecular = vec4(0.0);
        return;
    }

    /* Sample albedo and ORM buffers */

    vec3 albedo = texelFetch(uAlbedoTex, pixCoord, 0).rgb;
    vec4 orm = texelFetch(uOrmTex, pixCoord, 0);

    /* Compute view direction and the dot product of the normal and view direction */

    vec3 V = normalize(uView.position - P);
    float NoV = max(dot(N, V), 1e-4);

    /* Compute the halfway vector between the view and light directions */

    vec3 H = normalize(V + L);

    float LoH = max(dot(L, H), 0.0);
    float NoH = max(dot(N, H), 0.0);

    /* Compute light color energy */

    vec3 lightColE = uLight.color * uLight.energy;

    /* Compute diffuse lighting */

    vec3 diff = L_Diffuse(orm.g, NoV, NoL, LoH);
    diff *= albedo * lightColE * (1.0 - orm.b);

    /* Compute specular lighting */

    vec3 F0 = PBR_F0(orm.b, orm.w, albedo);
    vec3 spec = L_Specular(F0, orm.g, NoV, NoL, NoH, LoH);
    spec *= lightColE * uLight.specular;

    /* Compute shadow factor */

    float shadow = 1.0;

    if (uLight.type != LIGHT_DIR)
    {
        float atten = pow(1.0 - clamp(Ldist / uLight.range, 0.0, 1.0), uLight.falloff);
        shadow *= atten;
    }

    if (uLight.type == LIGHT_SPOT)
    {
        float theta = dot(L, -uLight.direction);
        float epsilon = (uLight.innerCutOff - uLight.outerCutOff);
        shadow *= smoothstep(0.0, 1.0, (theta - uLight.outerCutOff) / epsilon);
    }

    if (uLight.shadowLayer >= 0 && uLight.shadowOpacity != 0.0 && shadow > 1e-4)
    {
        mat2 diskRot = L_ShadowDebandingMatrix(gl_FragCoord.xy);
        switch (uLight.type)
        {
        case LIGHT_DIR:  shadow *= L_SampleShadowDir(uLight, P, depth, NoL, diskRot); break;
        case LIGHT_SPOT: shadow *= L_SampleShadowSpot(uLight, P, NoL, diskRot); break;
        case LIGHT_OMNI: shadow *= L_SampleShadowOmni(uLight, P, NoL, diskRot); break;
        }
    }

    /* Compute final lighting contribution */

    FragRadiance = vec4(diff * shadow, 1.0);
    FragSpecular = vec4(spec * shadow, 1.0);
}
