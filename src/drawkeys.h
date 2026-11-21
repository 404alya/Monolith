// The draw mode's tools and their keys (drawkeys.c; docs/painting.md 18): one table that the draw
// mode's input (drawmode.c), the panel's tool grid and hints (drawpanel.c) and the keys panel
// (keyspanel.c) read, so a key is changed in one place (rebinding from a file: a later round).
// Pure data: no window, no scene.
#ifndef MONOLITH_DRAWKEYS_H
#define MONOLITH_DRAWKEYS_H

#include <stdbool.h>

// The tools (Aseprite's, docs/painting.md 18.2). The order is the panel's tool grid, row by row
typedef enum {
    TOOL_PENCIL = 0,        // B: the brush (shape round / square / soft, size, hardness), pixel-perfect at size 1
    TOOL_ERASER,            // E: lowers alpha with the brush (docs/painting.md 7.2)
    TOOL_SPRAY,             // S: random dots in a disc of the spray width
    TOOL_BUCKET,            // G: fill by colour (contiguous / global, tolerance) or the whole region
    TOOL_GRADIENT,          // SHIFT+G: linear / radial, foreground to background
    TOOL_LINE,              // L
    TOOL_CURVE,             // SHIFT+L: a line, then two clicks for the control points
    TOOL_RECT,              // U (U again: filled)
    TOOL_RECT_FILL,
    TOOL_ELLIPSE,           // SHIFT+U (SHIFT+U again: filled)
    TOOL_ELLIPSE_FILL,
    TOOL_CONTOUR,           // D: a freehand outline, filled on release
    TOOL_POLYGON,           // SHIFT+D: clicks, filled when closed
    TOOL_MARQUEE,           // M: rectangular selection
    TOOL_MARQUEE_ELLIPSE,   // SHIFT+M
    TOOL_LASSO,             // Q
    TOOL_LASSO_POLY,        // SHIFT+Q
    TOOL_WAND,              // W: magic wand (the bucket's tolerance and contiguous switch)
    TOOL_MOVE,              // V: moves the selected pixels (no selection: the whole region's)
    TOOL_BLUR,              // R
    TOOL_JUMBLE,            // SHIFT+R
    TOOL_EYEDROPPER,        // I (ALT held with any painting tool: the eyedropper for one click)
    TOOL_COUNT
} DrawTool;

typedef enum {
    TOOLKIND_BRUSH = 0,     // paints along the drag with the brush (pencil, eraser, spray, blur, jumble)
    TOOLKIND_SHAPE,         // a shape previewed while the mouse is down, one stroke on release
    TOOLKIND_CLICK,         // acts at the click (bucket, eyedropper)
    TOOLKIND_SELECT,        // changes the selection (never the pixels)
    TOOLKIND_MOVE,          // moves pixels
} DrawToolKind;

// A tool key: `key` (raylib's KEY_*) with SHIFT or without selects `tool`; pressed again while that
// tool (or `again`) is on it switches to `again` (U: rectangle, filled rectangle; -1: no cycle)
typedef struct {
    int key;
    bool shift;
    DrawTool tool;
    int again;
} DrawToolKey;

const char *DrawToolName(DrawTool t);       // "pencil", "rectangle filled", ... (the panel, the HUD)
const char *DrawToolScriptName(DrawTool t); // --paint-script `tool <name>`: "pencil", "rectfill", ...
int DrawToolByScriptName(const char *name); // -1: none
DrawToolKind DrawToolKindOf(DrawTool t);
bool DrawToolPaints(DrawTool t);            // changes pixels (not a selection tool, not the eyedropper)
const char *DrawToolKeyName(DrawTool t);    // "B", "SHIFT+U", ... (hints); "" when it has no key

int DrawToolKeyCount(void);
const DrawToolKey *DrawToolKeyAt(int i);
// The tool a key press selects, given the tool on now (the table's cycle): -1 when the key with this
// SHIFT state is no tool key
int DrawToolForKey(int key, bool shift, DrawTool current);

#endif
