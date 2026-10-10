/* fog_sunvol.frag -- Bakes one slice of the volumetric fog's sun-visibility volume
 *
 * Local monolith patch (see patches.md). For a static directional light and static
 * geometry the shadow visibility the fog march samples is a fixed function of the world
 * position, so it is baked once into a 3D texture (one voxel per fragment, one draw per
 * z slice) and vfog_radiance.frag takes one trilinear fetch per step instead of the
 * shadow-map PCF. Each voxel stores the mean visibility over uSub^3 points spread
 * evenly through the voxel (a box prefilter, so slits thinner than a voxel come out
 * dimmer instead of aliasing), each a hardware 2x2 PCF tap of the shadow map.
 */

#version 330 core

uniform sampler2DArrayShadow uShadowTex;
uniform mat4 uLightViewProj;    // the matrix the shadow layer was rendered with
uniform int uLayer;
uniform vec3 uVolMin;           // world position of the volume's minimum corner
uniform vec3 uVoxel;            // voxel size per axis (m)
uniform int uSlice;             // z index of the slice being written
uniform int uSub;               // subsamples per axis
uniform float uBias;            // shadow depth bias (as the fog's lookups use it)

out float FragVis;

void main()
{
    vec3 corner = uVolMin + vec3(floor(gl_FragCoord.xy), float(uSlice)) * uVoxel;
    float invSub = 1.0 / float(uSub);
    float sum = 0.0;

    for (int z = 0; z < uSub; z++)
    for (int y = 0; y < uSub; y++)
    for (int x = 0; x < uSub; x++)
    {
        vec3 P = corner + (vec3(x, y, z) + 0.5) * invSub * uVoxel;
        vec4 Pls = uLightViewProj * vec4(P, 1.0);
        vec3 proj = Pls.xyz / Pls.w * 0.5 + 0.5;
        sum += texture(uShadowTex, vec4(proj.xy, float(uLayer), proj.z - uBias));
    }

    FragVis = sum * invSub * invSub * invSub;
}
