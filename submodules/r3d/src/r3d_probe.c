/* r3d_probe.c -- R3D Probe Module.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#include <r3d/r3d_probe.h>
#include <r3d_config.h>
#include <raymath.h>
#include <stddef.h>
#include <stdint.h>

#include "./modules/r3d_env.h"

// ========================================
// PUBLIC API
// ========================================

R3D_Probe R3D_LoadProbe(R3D_ProbeType type, bool interior, bool shadow)
{
    R3D_Probe probe = {0};

    int layer = -1;
    switch (type)
    {
    case R3D_PROBE_ILLUMINATION:
        layer = r3d_env_irradiance_acquire_layer();
        break;
    case R3D_PROBE_REFLECTION:
        layer = r3d_env_prefilter_acquire_layer();
        break;
    }

    if (!r3d_env_probe_layer_is_valid(type, layer))
    {
        R3D_TRACELOG(LOG_WARNING, "Failed to load probe (type: %s)", r3d_env_probe_type_name(type));
        return probe;
    }

    probe.type     = type;
    probe.handle   = (uint32_t)(1 + layer);
    probe.position = (Vector3) {0};
    probe.falloff  = 1.0f;
    probe.range    = 10.0f;
    probe.interior = interior;
    probe.shadows  = shadow;

    R3D_TRACELOG(LOG_INFO, "Probe loaded successfully (type: %s)", r3d_env_probe_type_name(type));

    return probe;
}

void R3D_UnloadProbe(R3D_Probe probe)
{
    switch (probe.type)
    {
    case R3D_PROBE_ILLUMINATION:
        r3d_env_irradiance_release_layer((int)probe.handle - 1);
        break;
    case R3D_PROBE_REFLECTION:
        r3d_env_prefilter_release_layer((int)probe.handle - 1);
        break;
    }

    R3D_TRACELOG(LOG_INFO, "Probe unloaded successfully (type: %s)", r3d_env_probe_type_name(probe.type));
}

bool R3D_IsProbeValid(R3D_Probe probe)
{
    return r3d_env_probe_layer_is_valid(probe.type, (int)probe.handle - 1);
}
