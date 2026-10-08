/* bloom.frag -- Fragment shader for applying bloom to the scene
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

#include <ubo/fx.glsl>

// ================================
// In - Varyings
// ================================

noperspective in vec2 vTexCoord;

// ================================
// Out - Fragments
// ================================

out vec4 FragColor;

// ================================
// Samplers & Uniforms
// ================================

uniform sampler2D uSceneTex;
uniform sampler2D uBloomTex;

// ================================
// Main Function
// ================================

void main()
{
    vec4 scene = texture(uSceneTex, vTexCoord);
    vec3 bloom = texture(uBloomTex, vTexCoord).rgb;

    vec3 color = scene.rgb;

    if (uBloom.mode == BLOOM_MIX)
    {
        color = mix(color, bloom, uBloom.intensity);
    }
    else if (uBloom.mode == BLOOM_ADDITIVE)
    {
        color += bloom * uBloom.intensity;
    }
    else if (uBloom.mode == BLOOM_SCREEN)
    {
        bloom = clamp(bloom * uBloom.intensity, vec3(0.0), vec3(1.0));
        color = max((color + bloom) - (color * bloom), vec3(0.0));
    }

    FragColor = vec4(color, scene.a);
}
