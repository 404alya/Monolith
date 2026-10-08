/* vfog_compose.frag -- Fragment shader for volumetric fog radiance composition
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

#include <lib/sampling.glsl>
#include <lib/color.glsl>

// ================================
// In - Varyings
// ================================

noperspective in vec2 vTexCoord;

// ================================
// Out - Fragments
// ================================

out vec4 FragRadiance;

// ================================
// Samplers & Uniforms
// ================================

uniform sampler2D uRadianceTex;
uniform sampler2D uDepthTex;

// ================================
// Main Function
// ================================

void main()
{
    float refDepth = texelFetch(uDepthTex, ivec2(gl_FragCoord.xy), 0).r;
    float depthSharpness = 1.0 / max(refDepth * 0.1, 0.05);

    FragRadiance = S_Upsample(uRadianceTex, uDepthTex, vTexCoord, refDepth, depthSharpness);
    FragRadiance = C_UnTonemap(FragRadiance);
}
