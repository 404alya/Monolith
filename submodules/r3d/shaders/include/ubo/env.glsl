/* env.glsl -- Env data structures and uniform block.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

struct E_Probe {
    vec3  position;
    float falloff;
    float range;
    int   layer;
};

struct E_Ambient {
    vec4  rotation;
    vec4  color;
    float energy;
    int   irradiance;
    int   prefilter;
};

layout(std140) uniform EnvBlock {
    E_Probe   uIlluminationProbes[MAX_ILLUMINATION_PROBES];
    E_Probe   uReflectionProbes[MAX_REFLECTION_PROBES];
    E_Ambient uAmbient;

    int uNumIlluminationProbes;
    int uNumReflectionProbes;
    int uNumPrefilterLevels;
};
