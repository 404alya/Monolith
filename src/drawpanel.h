// The draw panel (draw mode, docked left; drawpanel.c; docs/painting.md 13.2, 18.9): target, tools
// (the tool grid and the tool's options), colour (saturation / value square, hue and alpha strips,
// fg / bg, hex, RGBA, recent colours, palette), density or grid (resize / fit to grid, pixel look,
// clear), box face groups. Mouse-driven, in the ui.h look.
#ifndef MONOLITH_DRAWPANEL_H
#define MONOLITH_DRAWPANEL_H

#include "common.h"
#include "drawmode.h"
#include "ui.h"

#define DRAWPANEL_X 6               // the panel's left edge (the HUD backdrop's)
#define DRAWPANEL_W 300
#define DRAWPANEL_RIGHT (DRAWPANEL_X + DRAWPANEL_W)

typedef enum {
    DRAWPANEL_ALL = 0, DRAWPANEL_TARGET, DRAWPANEL_TOOLS, DRAWPANEL_COLOR, DRAWPANEL_DENSITY, DRAWPANEL_FACES,
} DrawPanelSection;

// What the panel asks the draw mode to do this frame (the panel itself edits only dm's fields: the
// tool and its options, the colours and palette, density and switches; never a tool's behaviour); the values are DrawModePanelAction's DRAW_ACT_*
typedef enum {
    DRAWPANEL_ACT_NONE = 0,
    DRAWPANEL_ACT_RESIZE = DRAW_ACT_RESIZE,         // Resize to density; Fit to grid on a grid (docs/painting.md 17.7)
    DRAWPANEL_ACT_FACES = DRAW_ACT_FACES,           // apply faces[] to the target box's entries (5.3)
    DRAWPANEL_ACT_PIXEL_LOOK = DRAW_ACT_PIXEL_LOOK, // pixelLook changed: the target atlas's paint_filter
    DRAWPANEL_ACT_CLEAR = DRAW_ACT_CLEAR,           // Clear paint: the target's entry goes (5.3)
} DrawPanelAction;

typedef struct {
    UiDrag drag;
    UiEdit edit;
    char hex[12];                   // the hex field ("#RRGGBB" / "#RRGGBBAA"; follows dm->fg while closed)
    float hue, sat, val;            // the colour selector's HSV (hue 0..360): its own, so the hue stays
                                    // while saturation or value is 0 ...
    Color hsvOf;                    // ... and taken from fg again when fg's RGB is no longer this (fg
    bool hsvSet;                    // changed elsewhere: a key, the eyedropper, a field)
    unsigned char faces[6];         // the face-group buttons: the target box's groups, with the user's
                                    // unapplied changes (kept until the target is another box)
    unsigned char facesLoaded[6];   // ... the groups they were loaded from
    int facesKey;                   // ... the box and material (DrawTarget.faceKey; -1: none, -2: not loaded)
    Rectangle rect;                 // where it was drawn last (the mouse is the panel's inside it)
    DrawPanelSection only;          // --draw-panel <section>: draw that one alone (shots)
    bool interactive;               // the mouse is read (a window, or a --input-script shot)
    bool typing;                    // a field has the keyboard this frame (it was or is open): the
                                    // draw mode's keys (1..5, X, [ ], TAB, B / ESC) wait
    int top;                        // the y it was last drawn from (DrawPanelUpdate lays out there)
    int scroll, contentH;           // px scrolled (the panel is taller than the screen), its full height
} DrawPanel;

void DrawPanelInit(DrawPanel *p);
// --draw-panel <section>: the section named (all, target, tools (or pen), color / colour, density (or
// canvas), faces);
// false for an unknown name
bool DrawPanelSetOnly(DrawPanel *p, const char *section);
// The panel's mouse input (call before DrawModeUpdate); true while the cursor is over it (or a
// slider drag goes on outside it, or the click that closed a field landed outside it)
bool DrawPanelUpdate(DrawPanel *p, DrawMode *dm, DrawPanelAction *action);
// Draws it at the left edge from y down (2D, with the HUD)
void DrawPanelDraw(DrawPanel *p, const DrawMode *dm, int y);
bool DrawPanelContains(const DrawPanel *p, Vector2 px);

#endif
