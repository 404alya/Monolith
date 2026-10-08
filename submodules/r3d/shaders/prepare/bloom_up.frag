/* bloom_up.frag -- Custom upsampling fragment shader used for bloom generation
 *
 * Original implementation by Jorge Jiménez, presented at SIGGRAPH 2014
 * (used in Call of Duty: Advanced Warfare)
 *
 * Copyright (c) 2014 Jorge Jiménez
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#version 330 core

// ================================
// In - Varyings
// ================================

noperspective in vec2 vTexCoord;

// ================================
// Out - Fragments
// ================================

layout (location = 0) out vec3 FragColor;

// ================================
// Samplers & Uniforms
// ================================

uniform sampler2D uTexture;
uniform vec2 uFilterRadius;

// ================================
// Main Function
// ================================

void main()
{
    // The filter kernel is applied with a radius, specified in texture
    // coordinates, so that the radius will vary across mip resolutions.
    float x = uFilterRadius.x;
    float y = uFilterRadius.y;

    // Take 9 samples around current texel:
    // a - b - c
    // d - e - f
    // g - h - i

    vec3 a = texture(uTexture, vec2(vTexCoord.x - x, vTexCoord.y + y)).rgb;
    vec3 b = texture(uTexture, vec2(vTexCoord.x,     vTexCoord.y + y)).rgb;
    vec3 c = texture(uTexture, vec2(vTexCoord.x + x, vTexCoord.y + y)).rgb;

    vec3 d = texture(uTexture, vec2(vTexCoord.x - x, vTexCoord.y)).rgb;
    vec3 e = texture(uTexture, vec2(vTexCoord.x,     vTexCoord.y)).rgb;
    vec3 f = texture(uTexture, vec2(vTexCoord.x + x, vTexCoord.y)).rgb;

    vec3 g = texture(uTexture, vec2(vTexCoord.x - x, vTexCoord.y - y)).rgb;
    vec3 h = texture(uTexture, vec2(vTexCoord.x,     vTexCoord.y - y)).rgb;
    vec3 i = texture(uTexture, vec2(vTexCoord.x + x, vTexCoord.y - y)).rgb;

    // Apply weighted distribution, by using a 3x3 tent filter:
    //  1   | 1 2 1 |
    // -- * | 2 4 2 |
    // 16   | 1 2 1 |

    FragColor = e*4.0;
    FragColor += (b+d+f+h)*2.0;
    FragColor += (a+c+g+i);
    FragColor *= 1.0 / 16.0;
}
