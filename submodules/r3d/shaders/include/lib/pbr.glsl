/* pbr.glsl -- PBR functions with explicit parameters, no UBO dependency.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#include "./math.glsl"

float PBR_D_GGX(float NoH, float roughness)
{
	float a2 = roughness * roughness;
	float f  = (NoH * a2 - NoH) * NoH + 1.0;
	return a2 / (M_PI * f * f);
}

float PBR_V_SmithGGXCorrelated(float NoV, float NoL, float roughness)
{
	float a2   = roughness * roughness;
	float GGXV = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
	float GGXL = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
	return 0.5 / max(GGXV + GGXL, 1e-5);
}

vec3 PBR_F_Schlick(const vec3 f0, float f90, float VoH)
{
	// Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"
	return f0 + (f90 - f0) * M_Pow5(1.0 - VoH);
}

vec3 PBR_F_Schlick(const vec3 f0, float VoH)
{
	float f = M_Pow5(1.0 - VoH);
	return f + f0 * (1.0 - f);
}

float PBR_F_Schlick(float f0, float f90, float VoH)
{
	return f0 + (f90 - f0) * M_Pow5(1.0 - VoH);
}

float PBR_Fd_Burley(float roughness, float NoV, float NoL, float LoH)
{
	// Burley 2012, "Physically-Based Shading at Disney"
	float f90 = 0.5 + 2.0 * roughness * LoH * LoH;
	float lightScatter = PBR_F_Schlick(1.0, f90, NoL);
	float viewScatter  = PBR_F_Schlick(1.0, f90, NoV);
	return lightScatter * viewScatter * (1.0 / M_PI);
}

vec3 PBR_F0(float metallic, float specular, vec3 albedo)
{
    // use (albedo * metallic) as colored specular reflectance at 0 angle for metallic materials
    // SEE: https://google.github.io/filament/Filament.md.html

    float dielectric = 0.16 * specular * specular;
    return mix(vec3(dielectric), albedo, vec3(metallic));
}
