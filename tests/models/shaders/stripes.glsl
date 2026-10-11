// Awning cloth: two colours in stripes along x, computed from the world position
// (so every box shows the same stripe width, whatever its size)

#pragma usage opaque shadow

uniform vec3 u_colorA;      // linear RGB
uniform vec3 u_colorB;
uniform float u_width;      // stripe width in metres

void fragment()
{
    float s = step(0.5, fract(POSITION.x / (2.0 * u_width)));
    ALBEDO = mix(u_colorA, u_colorB, s);
    ROUGHNESS = 0.85;
}
