/* pyramid.frag - GBuffer pyramid downsampling shader
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#version 330 core

// ================================
// Constants
// ================================

const ivec2 OFFSETS[4] = ivec2[4]
(
    ivec2(0, 0), ivec2(1, 0),
    ivec2(0, 1), ivec2(1, 1)
);

// ================================
// Depth + Selector
// ================================

#if defined(PYRAMID)

#include <ubo/view.glsl>

layout(location = 0) out float FragDepth;
layout(location = 1) out uint  FragIndex;

uniform sampler2D uDepthTex;

void main()
{
    ivec2 pixCoord = 2 * ivec2(gl_FragCoord.xy);

    ivec2 p0 = pixCoord + OFFSETS[0];
    ivec2 p1 = pixCoord + OFFSETS[1];
    ivec2 p2 = pixCoord + OFFSETS[2];
    ivec2 p3 = pixCoord + OFFSETS[3];

    float d0 = texelFetch(uDepthTex, p0, 0).r;
    float d1 = texelFetch(uDepthTex, p1, 0).r;
    float d2 = texelFetch(uDepthTex, p2, 0).r;
    float d3 = texelFetch(uDepthTex, p3, 0).r;

    FragDepth = d0;
    FragIndex = 0u;

    if (d1 < FragDepth) { FragDepth = d1; FragIndex = 1u; }
    if (d2 < FragDepth) { FragDepth = d2; FragIndex = 2u; }
    if (d3 < FragDepth) { FragDepth = d3; FragIndex = 3u; }

	gl_FragDepth = 1.0 - float(
		(d0 < uView.far) ||
		(d1 < uView.far) ||
		(d2 < uView.far) ||
		(d3 < uView.far)
	);
}

#endif // PYRAMID

// ================================
// Downsampling From Selector
// ================================

#if defined(DOWNSAMPLE)

layout(location = 0) out vec4 FragDown;

uniform usampler2D uSelectorTex;
uniform sampler2D  uSourceTex;

void main()
{
    ivec2 upCoord = 2 * ivec2(gl_FragCoord.xy);
    ivec2 pxCoord =     ivec2(gl_FragCoord.xy);

    uint index = texelFetch(uSelectorTex, pxCoord, 0).r;

    FragDown = texelFetch(uSourceTex, upCoord + OFFSETS[index], 0);
}

#endif // DOWNSAMPLE
