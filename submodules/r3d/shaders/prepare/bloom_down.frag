/* bloom_down.frag -- Custom 36-tap bilinear downsampling shader for bloom generation
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
// Includes
// ================================

#include <lib/color.glsl>
#include <ubo/fx.glsl>

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
uniform vec2 uTexelSize;    //< Reciprocal of the resolution of the source being sampled
uniform bool uFirstPass;    //< We perform Karis average during first pass

// ================================
// Helper Functions
// ================================

float KarisAverage(vec3 col)
{
    float luma = C_Luminance(col);
    return 1.0 / (1.0 + luma);
}

vec3 Prefilter(vec3 col)
{
	float brightness = max(col.r, max(col.g, col.b));
	float soft = brightness - uBloom.prefilter.y;
	soft = clamp(soft, 0, uBloom.prefilter.z);
	soft = soft * soft * uBloom.prefilter.w;
	float contribution = max(soft, brightness - uBloom.prefilter.x);
	contribution /= max(brightness, 0.00001);
	return col * contribution;
}

// ================================
// Main Function
// ================================

void main()
{
    float x = uTexelSize.x;
    float y = uTexelSize.y;

    // Take 13 samples around current texel:
    // a - b - c
    // - j - k -
    // d - e - f
    // - l - m -
    // g - h - i

    vec3 a = texture(uTexture, vec2(vTexCoord.x - 2*x, vTexCoord.y + 2*y)).rgb;
    vec3 b = texture(uTexture, vec2(vTexCoord.x,       vTexCoord.y + 2*y)).rgb;
    vec3 c = texture(uTexture, vec2(vTexCoord.x + 2*x, vTexCoord.y + 2*y)).rgb;

    vec3 d = texture(uTexture, vec2(vTexCoord.x - 2*x, vTexCoord.y)).rgb;
    vec3 e = texture(uTexture, vec2(vTexCoord.x,       vTexCoord.y)).rgb;
    vec3 f = texture(uTexture, vec2(vTexCoord.x + 2*x, vTexCoord.y)).rgb;

    vec3 g = texture(uTexture, vec2(vTexCoord.x - 2*x, vTexCoord.y - 2*y)).rgb;
    vec3 h = texture(uTexture, vec2(vTexCoord.x,       vTexCoord.y - 2*y)).rgb;
    vec3 i = texture(uTexture, vec2(vTexCoord.x + 2*x, vTexCoord.y - 2*y)).rgb;

    vec3 j = texture(uTexture, vec2(vTexCoord.x - x, vTexCoord.y + y)).rgb;
    vec3 k = texture(uTexture, vec2(vTexCoord.x + x, vTexCoord.y + y)).rgb;
    vec3 l = texture(uTexture, vec2(vTexCoord.x - x, vTexCoord.y - y)).rgb;
    vec3 m = texture(uTexture, vec2(vTexCoord.x + x, vTexCoord.y - y)).rgb;

    if (uFirstPass)
    {
        vec3 g0 = (a+b+d+e) * (0.125/4.0);
        vec3 g1 = (b+c+e+f) * (0.125/4.0);
        vec3 g2 = (d+e+g+h) * (0.125/4.0);
        vec3 g3 = (e+f+h+i) * (0.125/4.0);
        vec3 g4 = (j+k+l+m) * (0.5/4.0);

        g0 *= KarisAverage(g0);
        g1 *= KarisAverage(g1);
        g2 *= KarisAverage(g2);
        g3 *= KarisAverage(g3);
        g4 *= KarisAverage(g4);

        FragColor = g0+g1+g2+g3+g4;
        FragColor = max(FragColor, 1e-4);
        FragColor = Prefilter(FragColor);
    }
    else
    {
        FragColor = e*0.125;
        FragColor += (a+c+g+i)*0.03125;
        FragColor += (b+d+f+h)*0.0625;
        FragColor += (j+k+l+m)*0.125;
    }
}
