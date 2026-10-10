/* shadow_minmax.frag -- Min/max depth per tile of a directional shadow map layer
 *
 * Local monolith patch (see patches.md). Used by vfog_radiance.frag to skip the
 * PCF taps of march steps that are fully lit or fully shadowed.
 * Two passes (one thread per 256 texels is too serial to keep the GPU busy):
 *   uPass 0: shadow layer -> min/max of 8x8 texels   (uShadowTex -> RG32F, 1/8 size)
 *   uPass 1: 2x2 of those -> 16x16 tiles            (uSourceTex -> RG32F, 1/16 size)
 */

#version 330 core

// Pass 0 is bound by the texel fetch rate: gathering 2x2 texels per fetch makes it
// ~4x faster (8192^2 map: ~0.35 ms instead of ~1.5 ms). Same result either way.
#extension GL_ARB_texture_gather : enable

uniform sampler2DArray uShadowTex;  // depth values: compare mode is off during this pass
uniform sampler2D uSourceTex;
uniform int uLayer;
uniform int uPass;

out vec2 FragMinMax;

void main()
{
    float dMin = 1.0;
    float dMax = 0.0;

    if (uPass == 0)
    {
#ifdef GL_ARB_texture_gather
        // A sample exactly on the corner between 4 texels gathers those 4 texels
        vec2 base = floor(gl_FragCoord.xy) * 8.0;
        vec2 invSize = 1.0 / vec2(textureSize(uShadowTex, 0).xy);
        for (int y = 0; y < 4; y++)
        {
            for (int x = 0; x < 4; x++)
            {
                vec2 uv = (base + vec2(2 * x + 1, 2 * y + 1)) * invSize;
                vec4 d = textureGather(uShadowTex, vec3(uv, float(uLayer)));
                dMin = min(dMin, min(min(d.x, d.y), min(d.z, d.w)));
                dMax = max(dMax, max(max(d.x, d.y), max(d.z, d.w)));
            }
        }
#else
        ivec2 base = ivec2(gl_FragCoord.xy) * 8;
        ivec2 last = textureSize(uShadowTex, 0).xy - 1;
        for (int y = 0; y < 8; y++)
        {
            for (int x = 0; x < 8; x++)
            {
                float d = texelFetch(uShadowTex, ivec3(min(base + ivec2(x, y), last), uLayer), 0).r;
                dMin = min(dMin, d);
                dMax = max(dMax, d);
            }
        }
#endif
    }
    else
    {
        ivec2 base = ivec2(gl_FragCoord.xy) * 2;
        ivec2 last = textureSize(uSourceTex, 0) - 1;
        for (int y = 0; y < 2; y++)
        {
            for (int x = 0; x < 2; x++)
            {
                vec2 mm = texelFetch(uSourceTex, min(base + ivec2(x, y), last), 0).rg;
                dMin = min(dMin, mm.x);
                dMax = max(dMax, mm.y);
            }
        }
    }

    FragMinMax = vec2(dMin, dMax);
}
