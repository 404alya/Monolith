// Edit mode (M in spectator mode): manual object edits on the selection, without a request
// (editmode.c; the text edits themselves: mapedit.c)
#ifndef MONOLITH_EDITMODE_H
#define MONOLITH_EDITMODE_H

#include "common.h"
#include "designer.h"
#include "mapedit.h"

typedef struct {
    bool active;            // M: edit mode on
    MapEditor ed;           // undo / redo history (kept when the mode is left)
    char **models;          // model files of the package listed in ASSETS.md (heap strings), sorted
    int modelCount, modelCap;
    int modelPick;          // the one I adds
    char message[512];      // the last edit or why it was refused
    float messageTime;
    bool messageError;
    int warnKey;            // a request is open on this map: this key again within warnTime edits anyway
    float warnTime;
    int ackCount, ackNewest;    // the open requests the developer chose to edit beside
} EditMode;

typedef enum {
    EDITMODE_NONE = 0,
    EDITMODE_RELOADED,      // an edit / undo / redo rewrote and reloaded the map: re-apply the
                            // environment like after a hot reload (the selection is already set)
} EditModeResult;

// Spectator-mode input, after DesignerUpdate and only when the designer doesn't capture the
// keys: M toggles the mode; in it arrows / PGUP / PGDN move the selection (snap: N), R / SHIFT+R
// turn it, DELETE removes it, CTRL+D duplicates it, [ ] pick a model and I adds it at the
// crosshair. CTRL+Z / CTRL+SHIFT+Z (CTRL+Y) undo / redo in spectator mode, in the mode or not.
EditModeResult EditModeUpdate(EditMode *m, Designer *d, float dt);
// The arrow keys belong to the mode (not to the spectator's viewpoints), and R too
bool EditModeTakesArrows(const EditMode *m);
// After any reload (MS_Poll too): drops the undo history when the map changed elsewhere
void EditModeSync(EditMode *m);
// Opens the mode without a key (--edit-mode)
void EditModeOpen(EditMode *m, Designer *d);
// HUD lines at (x, y), returns the height used (nothing while the mode is off and quiet)
int EditModeDrawHud(const EditMode *m, const Designer *d, int x, int y);
void EditModeFree(EditMode *m);

#endif
