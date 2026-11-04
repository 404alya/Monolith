// The draw panel (draw mode, docked left; drawpanel.c; docs/painting.md 13.2): target, pen, colour
// picker, canvas density and pixel look, box face groups. Mouse-driven, in the ui.h look.
#ifndef MONOLITH_DRAWPANEL_H
#define MONOLITH_DRAWPANEL_H

#include "common.h"
#include "drawmode.h"
#include "ui.h"

#define DRAWPANEL_X 6               // the panel's left edge (the HUD backdrop's)
#define DRAWPANEL_W 300
#define DRAWPANEL_RIGHT (DRAWPANEL_X + DRAWPANEL_W)

typedef enum {
    DRAWPANEL_ALL = 0, DRAWPANEL_TARGET, DRAWPANEL_PEN, DRAWPANEL_COLOR, DRAWPANEL_CANVAS, DRAWPANEL_FACES,
} DrawPanelSection;

// What the panel asks the draw mode to do this frame (the panel itself edits only dm's pen,
// colour, density and switches)
typedef enum {
    DRAWPANEL_ACT_NONE = 0,
    DRAWPANEL_ACT_RESIZE,           // Resize to density (docs/painting.md 5.3)
    DRAWPANEL_ACT_FACES,            // apply faces[] to the canvas under the cursor (5.2)
    DRAWPANEL_ACT_PIXEL_LOOK,       // pixelLook changed: filter nearest / trilinear on the painted material
} DrawPanelAction;

typedef struct {
    UiDrag drag;
    UiEdit edit;
    char hex[12];                   // the hex field ("#RRGGBB" / "#RRGGBBAA"; follows dm->color while closed)
    unsigned char faces[6];         // the face-group buttons: the target canvas's layout, with the
                                    // user's unapplied changes (kept until the target canvas changes)
    unsigned char facesLoaded[6];   // ... the layout they were loaded from
    int facesCanvas;                // ... its canvas (-1: none, -2: not loaded yet)
    Rectangle rect;                 // where it was drawn last (the mouse is the panel's inside it)
    DrawPanelSection only;          // --draw-panel <section>: draw that one alone (shots)
    bool interactive;               // the mouse is read (a window, or a --input-script shot)
    bool typing;                    // a field has the keyboard this frame (it was or is open): the
                                    // draw mode's keys (1..5, X, H, [ ], TAB, B / ESC) wait
    int top;                        // the y it was last drawn from (DrawPanelUpdate lays out there)
    int scroll, contentH;           // px scrolled (the panel is taller than the screen), its full height
} DrawPanel;

void DrawPanelInit(DrawPanel *p);
// --draw-panel <section>: the section named (all, target, pen, color / colour, canvas, faces);
// false for an unknown name
bool DrawPanelSetOnly(DrawPanel *p, const char *section);
// The panel's mouse input (call before DrawModeUpdate); true while the cursor is over it (or a
// slider drag goes on outside it, or the click that closed a field landed outside it)
bool DrawPanelUpdate(DrawPanel *p, DrawMode *dm, DrawPanelAction *action);
// Draws it at the left edge from y down (2D, with the HUD)
void DrawPanelDraw(DrawPanel *p, const DrawMode *dm, int y);
bool DrawPanelContains(const DrawPanel *p, Vector2 px);

#endif
