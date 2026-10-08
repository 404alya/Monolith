/* compose.frag -- Deferred pipeline composition fragment shader
 *
 * Composes the final scene by combining the outputs from
 * the deferred rendering pipeline.
 *
 * Copyright (c) 2025-2026 Victor Le Juez
 *
 * This software is distributed under the terms of the accompanying LICENSE file.
 * It is provided "as-is", without any express or implied warranty.
 */

#version 330 core

// ================================
// Includes
// ================================

#include <lib/color.glsl>
#include <lib/pbr.glsl>

// ================================
// In - Varyings
// ================================

noperspective in vec2 vTexCoord;

// ================================
// Out - Fragments
// ================================

layout(location = 0) out vec4 FragColor;

// ================================
// Samplers & Uniforms
// ================================

uniform sampler2D uAlbedoTex;
uniform sampler2D uRadianceTex;
uniform sampler2D uSpecularTex;
uniform sampler2D uOrmTex;
uniform sampler2D uSsrTex;

uniform float uSsrNumLevels;

// ================================
// Main Function
// ================================

void main()
{
    vec3 albedo = texelFetch(uAlbedoTex, ivec2(gl_FragCoord.xy), 0).rgb;
    vec3 radiance = texelFetch(uRadianceTex, ivec2(gl_FragCoord.xy), 0).rgb;
    vec3 specular = texelFetch(uSpecularTex, ivec2(gl_FragCoord.xy), 0).rgb;

    vec4 orm = texelFetch(uOrmTex, ivec2(gl_FragCoord).xy, 0);
    vec4 ssr = C_UnTonemap(textureLod(uSsrTex, vTexCoord, orm.y * uSsrNumLevels));

    vec3 F0 = PBR_F0(orm.z, orm.w, albedo);
    vec3 kS_approx = F0 * (1.0 - orm.y * 0.5);

    specular = mix(specular, kS_approx * ssr.rgb, ssr.a);

    FragColor = vec4(radiance + specular, 1.0);
}
