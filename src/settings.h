// Graphics settings of the editor: the runtime's MS_Quality (presets, what the scene renders)
// plus the window-only parts (vsync, fps cap, frame reuse, window MSAA) and the editor's own
// settings (the edit save mode), the settings file and the row table the in-game panel
// (settingspanel.c) and the file share.
#ifndef MONOLITH_SETTINGS_H
#define MONOLITH_SETTINGS_H

#include "common.h"

typedef struct {
    MS_Quality q;               // what the scene renders (runtime/monolith_scene.h)
    MS_QualityLevel base;       // the preset "Reset to preset" returns to (the last one chosen)
    bool windowMsaa;            // 4x MSAA window; applies at the next start
    bool vsync;
    int fpsCap;                 // 0 = uncapped, -1 = the monitor's refresh rate, else frames per second
    bool frameReuse;            // interactive frame reuse (main.c "Frame reuse")
    // Editor (not graphics, but the same file and panel; never part of MS_Quality or the runtime)
    int editSave;               // 0 manual (CTRL+S writes the map), 1 auto (every edit is written at once)
} GfxSettings;

// Ultra (= the map exactly), no vsync, uncapped, frame reuse on, single-sampled window:
// exactly today's defaults
GfxSettings GfxDefaults(void);
void GfxSetPreset(GfxSettings *s, MS_QualityLevel level);   // q = the preset, base = level
MS_QualityLevel GfxPreset(const GfxSettings *s);            // the preset q equals, else custom

// The settings file: ~/.config/monolith/settings.ini ($XDG_CONFIG_HOME), on Windows
// %APPDATA%\monolith\settings.ini, else settings.ini next to the executable
const char *GfxDefaultPath(void);
// Missing file: false with err "" (not an error). Unknown keys / bad values: a warning each
// (logged), the rest still loads.
bool GfxLoad(GfxSettings *s, const char *path, char *err, int errSize);
bool GfxSave(const GfxSettings *s, const char *path);       // creates the folder
// Where the working copy of map `mapPath` goes when the game closes with unsaved edits: the
// settings file's folder (GfxDefaultPath), unsaved/<map name>-<YYYYMMDD-HHMMSS>.map (the folder is
// created). False when it can't be made.
bool GfxUnsavedPath(const char *mapPath, char *out, int outSize);
// One "key=value" (the file's keys), e.g. for --settings-set. False on unknown key / bad value
bool GfxSetKey(GfxSettings *s, const char *keyValue);
const char *GfxKeyHelp(void);

// ======================================================================================
// Rows: one per setting, shared by the panel and the file
// ======================================================================================

typedef enum { GFX_GROUP_DISPLAY, GFX_GROUP_SHADOWS, GFX_GROUP_AO, GFX_GROUP_FOG, GFX_GROUP_POST,
               GFX_GROUP_PERF, GFX_GROUP_EDITOR, GFX_GROUP_COUNT } GfxGroup;

typedef enum {
    GFX_ROW_VALUE = 0,          // a setting
    GFX_ROW_PRESET,             // the preset selector (key "preset")
} GfxRowKind;

int GfxRowCount(void);
GfxGroup GfxRowGroup(int row);
GfxRowKind GfxRowKindOf(int row);
const char *GfxRowLabel(int row);
const char *GfxRowKey(int row);
const char *GfxRowHint(int row);                // one line: what it does / costs
const char *GfxGroupName(GfxGroup g);
void GfxRowFormat(const GfxSettings *s, int row, char *out, int outSize);  // the current value
// Steps the value to the next / previous option (dir +1 / -1). False if it didn't change.
// The preset row sets the preset. Every value row makes the settings "custom" unless they
// happen to equal a preset.
bool GfxRowStep(GfxSettings *s, int row, int dir);
// Copies row's value from src to dst (a panel change goes to the run's settings and to the
// saved ones; CLI overrides of other rows stay out of the file)
void GfxRowCopy(GfxSettings *dst, const GfxSettings *src, int row);
bool GfxRowNeedsRestart(int row);               // window MSAA: next start
bool GfxRowIsHeavy(int row);                    // rebuilds something big (shadow map, bakes)

#endif
