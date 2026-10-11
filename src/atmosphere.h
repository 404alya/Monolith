// Editor-side effect overrides on top of the map's environment (the environment itself is
// map data: env statements, applied by MS_ApplyEnvironment in runtime/monolith_scene.c)
#ifndef MONOLITH_ATMOSPHERE_H
#define MONOLITH_ATMOSPHERE_H

#include "common.h"

// --ao <mode>: the name of a runtime AO mode (MS_Quality.ao; the graphics settings apply it).
// Single effect settings (--fx key=value) are applied to `env` after the quality settings.
// Both return false (and change nothing) on an unknown name or bad value, so main() checks
// the arguments before opening the window. The effect toggles 3/4 still work on top.
bool AoModeFromName(const char *name, MS_AoMode *mode);
bool ApplyFxSetting(R3D_Environment *env, const char *keyValue);
const char *AoModeHelp(void);
const char *FxSettingHelp(void);

#endif
