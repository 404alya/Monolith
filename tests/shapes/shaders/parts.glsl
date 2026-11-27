// A checker of the part UV: 8 x 8 squares over each part of a shape (or each face of a box), so a
// shot shows where every part's 0..1 lies and which way it runs (u: the red tint grows, v: the blue)

#pragma usage opaque shadow

uniform vec3 u_colorA;      // linear RGB
uniform vec3 u_colorB;

void fragment()
{
    vec2 c = floor(TEXCOORD * 8.0);
    vec3 base = mix(u_colorA, u_colorB, mod(c.x + c.y, 2.0));
    ALBEDO = base * vec3(0.6 + 0.4 * TEXCOORD.x, 0.8, 0.6 + 0.4 * TEXCOORD.y);
    ROUGHNESS = 0.8;
}
