// The keybindings panel: every key of the game, one section per mode (walking, spectator and
// selection, requests, snip and volumes, edit mode, grab / rotate / scale, panels), reached
// through the ESC menu (keyspanel.c). The HUD lists no keys itself.
#ifndef MONOLITH_KEYSPANEL_H
#define MONOLITH_KEYSPANEL_H

#include "common.h"
#include "ui.h"

typedef struct {
    bool menu;                  // ESC pressed with nothing else to close: the "See keybindings" button
    bool open;                  // the panel
    bool interactive;           // a real window: frees / captures the cursor
    int section;                // shown while nothing is typed
    int freezeFrames;           // game keys and camera stay frozen this many frames after closing
    UiList list;
    int visibleRows;            // the panel's rows (UiListDraw), for PGUP / PGDN
    Rectangle button;           // the menu's button (last draw), for the mouse
} KeysPanel;

// escFree: ESC would do nothing else this frame (no panel, prompt box, snip / volume / edit mode,
// modal). ESC then opens the menu; in the menu ENTER or a click on its button opens the panel,
// ESC closes it; in the panel LEFT / RIGHT change the section, typing searches every section,
// ESC (or F1) closes it.
void KeysUpdate(KeysPanel *k, bool escFree);
bool KeysCapturesInput(const KeysPanel *k);     // the menu or the panel is open, or just closed
void KeysOpen(KeysPanel *k, const char *section);   // --keys-panel [section]: the panel at once
void KeysDraw(KeysPanel *k, int screenW, int screenH);
void KeysFree(KeysPanel *k);

#endif
