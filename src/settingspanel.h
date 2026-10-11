// The in-game graphics settings panel (F2, both modes): raylib immediate-mode widgets over
// settings.c's rows. main.c applies what changes (live; a shadow map size restarts the renderer)
#ifndef MONOLITH_SETTINGSPANEL_H
#define MONOLITH_SETTINGSPANEL_H

#include "settings.h"

#define PANEL_RESET (-2)        // PanelUpdate: "Reset to preset" was pressed

typedef struct {
    bool open;
    bool interactive;           // a real window: the panel frees / captures the cursor
    int sel;                    // selected item: a row, then Reset, Close
    int freezeFrames;           // game keys and camera stay frozen this many frames after closing
    bool ownProfiler;           // the panel switched R3D's GPU profiler on (and switches it off)
    float gpuMs;                // last GPU frame time from the profiler, < 0 = none yet
    float msAvg;                // smoothed CPU frame time
    char status[160];           // "applying ..." and other notes, drawn in the footer
    float statusTime;
} SettingsPanel;

void PanelOpen(SettingsPanel *p);
void PanelClose(SettingsPanel *p);
bool PanelCapturesInput(const SettingsPanel *p);    // open, or just closed
void PanelStatus(SettingsPanel *p, const char *text, float seconds);

// Input while open (F2 toggling is main.c's): returns the row whose value changed in *s, or
// PANEL_RESET, or -1. ESC / F2 / Close close it. Also reads the profiler and frame time.
int PanelUpdate(SettingsPanel *p, GfxSettings *s, float dt, int screenW, int screenH);
void PanelDraw(const SettingsPanel *p, const GfxSettings *s, int screenW, int screenH, bool reused);

#endif
