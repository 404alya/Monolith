/* sampling.glsl -- Sampling helper functions.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

// ================================
// Structures
// ================================

struct S_UpsampleWeights {
    ivec2 p00, p10, p01, p11;
    float w00, w10, w01, w11;
    float invWSum;
};

// ================================
// Upsampling Functions
// ================================

S_UpsampleWeights S_ComputeUpsampleWeights(sampler2D depthTex, vec2 texCoord, float refDepth, float depthSharpness)
{
    ivec2 depthRes = textureSize(depthTex, 1);
    ivec2 maxCoord = depthRes - ivec2(1);

    vec2 pixLow = texCoord * vec2(depthRes) - 0.5;
    vec2 base = floor(pixLow);
    vec2 f = pixLow - base;

    S_UpsampleWeights r;
    r.p00 = clamp(ivec2(base),               ivec2(0), maxCoord);
    r.p10 = clamp(ivec2(base) + ivec2(1, 0), ivec2(0), maxCoord);
    r.p01 = clamp(ivec2(base) + ivec2(0, 1), ivec2(0), maxCoord);
    r.p11 = clamp(ivec2(base) + ivec2(1, 1), ivec2(0), maxCoord);

    vec4 d = vec4(
        texelFetch(depthTex, r.p00, 1).r,
        texelFetch(depthTex, r.p10, 1).r,
        texelFetch(depthTex, r.p01, 1).r,
        texelFetch(depthTex, r.p11, 1).r
    );

    vec4 w = vec4(
        (1.0 - f.x) * (1.0 - f.y),
        f.x * (1.0 - f.y),
        (1.0 - f.x) * f.y,
        f.x * f.y
    );

    w *= exp(-abs(d - vec4(refDepth)) * depthSharpness);

    r.w00 = w.x;
    r.w10 = w.y;
    r.w01 = w.z;
    r.w11 = w.w;

    r.invWSum = 1.0 / max(w.x + w.y + w.z + w.w, 1e-5);

    return r;
}

vec4 S_Upsample(sampler2D source, S_UpsampleWeights uw)
{
    vec4 c00 = texelFetch(source, uw.p00, 0);
    vec4 c10 = texelFetch(source, uw.p10, 0);
    vec4 c01 = texelFetch(source, uw.p01, 0);
    vec4 c11 = texelFetch(source, uw.p11, 0);

    return (c00 * uw.w00 + c10 * uw.w10 + c01 * uw.w01 + c11 * uw.w11) * uw.invWSum;
}

vec4 S_Upsample(sampler2D source, sampler2D depthTex, vec2 texCoord, float refDepth, float depthSharpness)
{
    S_UpsampleWeights uw = S_ComputeUpsampleWeights(depthTex, texCoord, refDepth, depthSharpness);

    return S_Upsample(source, uw);
}

vec4 S_UpsampleLod(sampler2D source, S_UpsampleWeights uw, float lod)
{
    vec2 invTexSize = 1.0 / vec2(textureSize(source, 0));

    vec4 c00 = textureLod(source, (vec2(uw.p00) + 0.5) * invTexSize, lod);
    vec4 c10 = textureLod(source, (vec2(uw.p10) + 0.5) * invTexSize, lod);
    vec4 c01 = textureLod(source, (vec2(uw.p01) + 0.5) * invTexSize, lod);
    vec4 c11 = textureLod(source, (vec2(uw.p11) + 0.5) * invTexSize, lod);

    return (c00 * uw.w00 + c10 * uw.w10 + c01 * uw.w01 + c11 * uw.w11) * uw.invWSum;
}

vec4 S_UpsampleLod(sampler2D source, sampler2D depthTex, vec2 texCoord, float refDepth, float depthSharpness, float lod)
{
    S_UpsampleWeights uw = S_ComputeUpsampleWeights(depthTex, texCoord, refDepth, depthSharpness);

    return S_UpsampleLod(source, uw, lod);
}
