// The editor's graphics settings panel (F2, both modes): raylib immediate-mode widgets over
// settings.c's rows. main.c applies what changes (live; a shadow map size restarts the renderer)
#ifndef MONOLITH_SETTINGSPANEL_H
#define MONOLITH_SETTINGSPANEL_H

#include "settings.h"

#define PANEL_RESET (-2)        // PanelUpdate: "Reset to preset" was pressed

typedef struct {
    bool open;
    bool interactive;           // a real window: the panel frees / captures the cursor
    int sel;                    // selected item: a row, then Reset, Close
    int freezeFrames;           // editor keys and camera stay frozen this many frames after closing
    bool ownProfiler;           // the panel switched R3D's GPU profiler on (and switches it off)
    float gpuMs;                // last GPU frame time from the profiler, < 0 = none yet
    float msAvg;                // smoothed CPU frame time
    MonoMsg status;             // "applying ..." and other notes, drawn in the footer (PanelFree)
    float statusTime;
} SettingsPanel;

void PanelOpen(SettingsPanel *p);
void PanelClose(SettingsPanel *p);
bool PanelCapturesInput(const SettingsPanel *p);    // open, or just closed
// The footer note for `seconds`: Static takes a literal only; f formats (text of any other
// lifetime: "%s", text)
void PanelStatusStatic(SettingsPanel *p, const char *literal, float seconds);
void PanelStatusf(SettingsPanel *p, float seconds, const char *fmt, ...) MSG_PRINTF(3, 4);
// Frees the footer note (at exit; PanelClose only closes the panel)
void PanelFree(SettingsPanel *p);

// Input while open (F2 toggling is main.c's): returns the row whose value changed in *s, or
// PANEL_RESET, or -1. ESC / F2 / Close close it. Also reads the profiler and frame time.
int PanelUpdate(SettingsPanel *p, GfxSettings *s, float dt, int screenW, int screenH);
void PanelDraw(const SettingsPanel *p, const GfxSettings *s, int screenW, int screenH, bool reused);

#endif
