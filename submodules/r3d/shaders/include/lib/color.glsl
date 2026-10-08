/* color.glsl -- Color math functions.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

vec3 C_LinearToSrgb(vec3 color)
{
    // Approximation from http://chilliant.blogspot.com/2012/08/srgb-approximations-for-hlsl.html
    return max(vec3(1.055) * pow(color, vec3(0.416666667)) - vec3(0.055), vec3(0.0));
}

vec4 C_LinearToSrgb(vec4 color)
{
    return vec4(C_LinearToSrgb(color.rgb), color.a);
}

vec3 C_SrgbToLinear(vec3 color)
{
    // Approximation from http://chilliant.blogspot.com/2012/08/srgb-approximations-for-hlsl.html
    return color * (color * (color * 0.305306011 + 0.682171111) + 0.012522878);
}

vec4 C_SrgbToLinear(vec4 color)
{
    return vec4(C_SrgbToLinear(color.rgb), color.a);
}

float C_Luminance(vec3 color)
{
    // Rec. 709 (BT.709) https://en.wikipedia.org/wiki/Rec._709
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

float C_Luminance(vec4 color)
{
    return C_Luminance(color.rgb);
}

vec3 C_Tonemap(vec3 color)
{
    return color / (1.0 + max(max(color.r, color.g), color.b));
}

vec4 C_Tonemap(vec4 color)
{
    return vec4(C_Tonemap(color.rgb), color.a);
}

vec3 C_UnTonemap(vec3 color)
{
    return color / max(1.0 - max(max(color.r, color.g), color.b), 1e-4);
}

vec4 C_UnTonemap(vec4 color)
{
    return vec4(C_UnTonemap(color.rgb), color.a);
}
