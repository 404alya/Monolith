// Edit mode (M in spectator mode): manual object edits on the selection, without a request
// (editmode.c; the text edits themselves: mapedit.c)
#ifndef MONOLITH_EDITMODE_H
#define MONOLITH_EDITMODE_H

#include "common.h"
#include "designer.h"
#include "mapedit.h"
#include "transform.h"

typedef struct {
    bool active;            // M: edit mode on
    MapEditor ed;           // undo / redo history (kept when the mode is left)
    char **models;          // model files of the package listed in ASSETS.md (heap strings), sorted
    int modelCount, modelCap;
    char message[512];      // the last edit or why it was refused
    float messageTime;
    bool messageError;
    int warnKey;            // a request is open on this map: this key again within warnTime edits anyway
    float warnTime;
    int ackCount, ackNewest;    // the open requests the developer chose to edit beside
    float leaveWarnTime;    // M / ESC with unsaved edits warned: again within this leaves
    // The id panel (CTRL+I with objects of one line selected): the number being typed
    bool idOpen;
    char idText[12];
    int idLine, idHas, idNext; // the line, its id (0 = implicit), the next free id
    TransformModal modal;   // G / ALT+R / S and the rotate gizmo (transform.c)
} EditMode;

typedef enum {
    EDITMODE_NONE = 0,
    EDITMODE_RELOADED,      // an edit / undo / redo changed the scene (live): re-apply the
                            // environment like after a hot reload (the selection is already set)
} EditModeResult;

// Spectator-mode input, after DesignerUpdate and only when the designer doesn't capture the
// keys: M toggles the mode; in it G / ALT+R / S and LMB on the rotate gizmo start the transform
// modal (transform.c; while it runs it takes every key and the mouse: call only this then, with the
// frozen camera), arrows / PGUP / PGDN move the selection (snap: N; SHIFT+N the angle snap), R /
// SHIFT+R turn it 90 / -90 degrees, ALT+SHIFT+R resets its rotation, L switches the gizmo's axes
// (global / local), DELETE removes it, CTRL+D
// duplicates it, CTRL+I gives the selection's line an id (the id panel); plain I is the model
// picker's (modelpicker.c: main.c opens it). CTRL+Z / CTRL+SHIFT+Z
// (CTRL+Y) undo / redo, CTRL+S saves and CTRL+SHIFT+DELETE clears the list of edits a rebase
// could not apply, in spectator mode, in the mode or not. Every edit, undo and redo goes live
// (the working copy, mapedit.c); the map file is written by CTRL+S (or at once in auto save
// mode). While the id panel is open it takes every key (call it then even though the game is
// "typing").
EditModeResult EditModeUpdate(EditMode *m, Designer *d, Camera3D cam, float dt);
// The transform modal runs: the camera is frozen, the cursor hidden, every key and click is its
bool EditModeModalRunning(const EditMode *m);
// --edit-modal (docs/live-editing.md 11.10): "<g|r|s> [lock] [typed]" (r: the ALT+R modal), "ring
// <x|y|z|xx|yy|zz|view> <deg> ...", "ball <dx> <dy> ..." open the mode and the modal on the
// selection (HUD shots), EditModeFinishModal then commits it (one edit, as LMB) or cancels it
// (everything back); "r90", "r-90", "reset", "rot <y> <p> <r>" apply that edit now (end 2, cancel:
// nothing; *reloaded: it went live); "gizmo [hover <x|y|z|view|ball>]" only shows the gizmo.
// false: a bad spec, or the edit was refused (the log says why)
bool EditModeOpenModal(EditMode *m, Designer *d, Camera3D cam, const char *spec, int end, bool *reloaded);
// The rotate gizmo: what is under the cursor px (call every frame before DesignerUpdate; enabled
// false when the cursor isn't the mode's, a panel has it or RMB flies). True: an LMB press there is
// the gizmo's (don't let it select)
bool EditModeGizmoHover(EditMode *m, Designer *d, Camera3D cam, Vector2 px, bool enabled);
// The gizmo (2D, after R3D_End, on top of the scene)
void EditModeDrawGizmo(EditMode *m, Designer *d, Camera3D cam);
EditModeResult EditModeFinishModal(EditMode *m, Designer *d, bool commit);
// The modal's axis guides (3D, after R3D_End, like DesignerDrawOverlay)
void EditModeDrawGuides(const EditMode *m, Camera3D cam);
// The id panel is open: every key is its (digits, BACKSPACE, ENTER, ESC); freeze the game keys
bool EditModeCapturesInput(const EditMode *m);
// The id list (F5, idpanel.c) edits through these, as the mode's own keys do (the request warning
// with `key` as the key to press again, live, undoable, the selection becomes the edited objects):
// op on these objects (lights too, which the designer can't select); CTRL+I on the designer's
// selection (one line: the number field, several: the next free ids at once); the number
// field's keys while it is open (EditModeCapturesInput), in any spectator mode
EditModeResult EditModeApplyTo(EditMode *m, Designer *d, int key, const MS_ObjRef *sel, int selCount, const EditOp *op);
EditModeResult EditModeIdKey(EditMode *m, Designer *d);
EditModeResult EditModeIdFieldUpdate(EditMode *m, Designer *d);
// Opens the id panel for the selection with this text typed (--edit-id, for HUD shots)
void EditModeOpenIdPanel(EditMode *m, Designer *d, const char *text);
// The arrow keys belong to the mode (not to the spectator's viewpoints), and G / R too
bool EditModeTakesArrows(const EditMode *m);
// After any reload from disk (MS_Poll, a renderer restart: newScene): unsaved edits are applied
// again on top of the changed file (MapEditRebase; the selection follows them), otherwise the
// undo history is dropped. A renderer restart that loaded the same file again shows the working
// copy again.
void EditModeSync(EditMode *m, Designer *d, bool newScene);
// Unsaved edits (0 before the first edit, and in auto save mode unless a write failed)
int EditModeUnsaved(const EditMode *m);
// CTRL+S: writes the working copy to the map file; msg says what happened
bool EditModeSave(EditMode *m, char *msg, int msgSize);
// Opens the mode without a key (--edit-mode)
void EditModeOpen(EditMode *m, Designer *d);
// HUD lines at (x, y), returns the height used (nothing while the mode is off and quiet)
int EditModeDrawHud(const EditMode *m, const Designer *d, int x, int y);
void EditModeFree(EditMode *m);
// models[] again from the package's models/ and ASSETS.md (EditModeOpen does it; the picker on
// every open, so a file recorded meanwhile shows up)
void EditModeScanModels(EditMode *m);
// Adds the model file (package-relative, one of models[]) at the designer's hit: snapped on the
// hit face's axes (N step), CTRL held: exact; after the hit object's line, in its group. One
// live edit (undoable); the selection becomes the new model
EditModeResult EditModeAddModel(EditMode *m, Designer *d, const char *modelPath);

#endif
