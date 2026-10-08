// Procedural board-formed concrete. No textures.
// Everything is computed from world position, so boxes of any size
// get the same texel density (no UV stretching when scaling a cube).

#pragma usage opaque shadow

uniform float u_scale;   // global pattern scale, 1.0 = real-world meters

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float hash13(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

// Not "noise3": that is a (deprecated) GLSL built-in returning vec3. NVIDIA lets a
// float overload slide, but AMD/Intel/glslang reject it and the shader fails to compile.
float valueNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    float a = hash13(i + vec3(0, 0, 0)), b = hash13(i + vec3(1, 0, 0));
    float c = hash13(i + vec3(0, 1, 0)), d = hash13(i + vec3(1, 1, 0));
    float e = hash13(i + vec3(0, 0, 1)), g = hash13(i + vec3(1, 0, 1));
    float h = hash13(i + vec3(0, 1, 1)), k = hash13(i + vec3(1, 1, 1));

    return mix(mix(mix(a, b, f.x), mix(c, d, f.x), f.y),
               mix(mix(e, g, f.x), mix(h, k, f.x), f.y), f.z);
}

float fbm(vec3 p)
{
    float v = 0.0, amp = 0.5;
    for (int i = 0; i < 4; i++) {
        v += amp * valueNoise(p);
        p = p * 2.03 + vec3(17.1, 3.7, 9.2);
        amp *= 0.5;
    }
    return v;
}

// Distance (in meters) to the nearest seam of a grid with the given cell size.
float seamDist(vec2 uv, vec2 cell)
{
    vec2 f = fract(uv / cell);
    vec2 d = min(f, 1.0 - f) * cell;
    return min(d.x, d.y);
}

void fragment()
{
    float s = (u_scale > 0.0) ? u_scale : 1.0;
    vec3 p = POSITION / s;
    vec3 an = abs(NORMAL);

    // World boxes are axis-aligned: project onto the dominant axis plane.
    bool top = an.y > an.x && an.y > an.z;
    vec2 uv = top ? p.xz : (an.x > an.z ? p.zy : p.xy);

    // Large-scale mottling + fine grain
    float mott  = fbm(p * 0.35);
    float grain = valueNoise(p * 18.0);
    float c = 0.22 + 0.12 * mott + 0.04 * (grain - 0.5);

    float grime = 0.0;
    float wet = 0.0;

    if (!top)
    {
        // Formwork panels (2.4 x 1.2 m): each pour has a slightly different tone
        vec2 cell = vec2(2.4, 1.2);
        vec2 panelId = floor(uv / cell);
        c *= 0.92 + 0.12 * hash12(panelId);

        // Panel seams
        float seam = 1.0 - smoothstep(0.004, 0.018, seamDist(uv, cell));
        c *= 1.0 - 0.35 * seam;

        // Horizontal board marks inside the panel (15 cm boards)
        float board = fract(uv.y / 0.15);
        float boardLine = 1.0 - smoothstep(0.0, 0.06, min(board, 1.0 - board));
        c *= 1.0 - 0.10 * boardLine * (0.5 + valueNoise(vec3(uv * 3.0, 1.0)));

        // Tie holes: 2x2 per panel
        vec2 local = fract(uv / cell) * cell;
        vec2 holeGrid = vec2(0.6, 0.3);
        vec2 hl = mod(local - holeGrid, cell * 0.5) - cell * 0.25;
        float hole = 1.0 - smoothstep(0.012, 0.022, length(hl));
        c *= 1.0 - 0.55 * hole;

        // Rain streaks: sparse, grouped in patches, fading in and out along their length
        float streak = valueNoise(vec3(uv.x * 4.0, uv.y * 0.08, 3.1));
        streak = smoothstep(0.62, 0.9, streak);
        streak *= smoothstep(0.35, 0.75, fbm(vec3(uv.x * 0.25, uv.y * 0.05, 7.7)));
        streak *= 0.5 + 0.5 * valueNoise(vec3(uv.x * 1.5, uv.y * 0.4, 1.3));
        grime += 0.3 * streak;
    }
    else
    {
        // Slabs: 6 x 6 m expansion joints
        float joint = 1.0 - smoothstep(0.01, 0.03, seamDist(uv, vec2(6.0)));
        c *= 1.0 - 0.4 * joint;

        // Puddles / damp patches
        wet = smoothstep(0.55, 0.7, fbm(p * 0.15 + 4.0));
        c *= 1.0 - 0.35 * wet;
    }

    // Dirt accumulating near the ground
    grime += 0.45 * (1.0 - smoothstep(0.0, 2.2, p.y)) * (0.6 + 0.4 * mott);

    c *= 1.0 - clamp(grime, 0.0, 0.7);

    // Slight green-brown tint where it is dirty (moss/algae)
    vec3 tint = mix(vec3(1.00, 0.98, 0.95), vec3(0.82, 0.86, 0.72), clamp(grime * 1.4, 0.0, 1.0));

    ALBEDO = vec3(c) * tint;
    ROUGHNESS = mix(0.92, 0.25, wet);
    METALNESS = 0.0;
    SPECULAR = 0.35;
}
