// Editor-side effect overrides on top of the map's environment (the environment itself is
// map data: env statements, applied by MS_ApplyEnvironment in runtime/monolith_scene.c)
#ifndef MONOLITH_ATMOSPHERE_H
#define MONOLITH_ATMOSPHERE_H

#include "common.h"

// Trade-off presets for the screen-space AO / indirect passes (--ao <mode>) and bloom
// (--bloom on|off), and single effect settings (--fx key=value), applied to `env` after
// MS_ApplyEnvironment(). They return false (and change nothing) on an unknown name or bad
// value, so main() checks the arguments on a scratch environment before opening the window.
// The effect toggles 3/4 still work on top.
bool ApplyAoMode(R3D_Environment *env, const char *mode);
bool ApplyBloomMode(R3D_Environment *env, const char *mode);
bool ApplyFxSetting(R3D_Environment *env, const char *keyValue);
const char *AoModeHelp(void);
const char *FxSettingHelp(void);

#endif
