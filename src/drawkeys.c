// drawkeys -- the draw mode's tools and their keys (drawkeys.h; docs/painting.md 18)
#include "drawkeys.h"
#include <raylib.h>
#include <string.h>

static const struct {
    const char *name, *script;
    DrawToolKind kind;
} TOOLS[TOOL_COUNT] = {
    [TOOL_PENCIL]           = { "pencil", "pencil", TOOLKIND_BRUSH },
    [TOOL_ERASER]           = { "eraser", "eraser", TOOLKIND_BRUSH },
    [TOOL_SPRAY]            = { "spray", "spray", TOOLKIND_BRUSH },
    [TOOL_BUCKET]           = { "paint bucket", "bucket", TOOLKIND_CLICK },
    [TOOL_GRADIENT]         = { "gradient", "gradient", TOOLKIND_SHAPE },
    [TOOL_LINE]             = { "line", "line", TOOLKIND_SHAPE },
    [TOOL_CURVE]            = { "curve", "curve", TOOLKIND_SHAPE },
    [TOOL_RECT]             = { "rectangle", "rect", TOOLKIND_SHAPE },
    [TOOL_RECT_FILL]        = { "filled rectangle", "rectfill", TOOLKIND_SHAPE },
    [TOOL_ELLIPSE]          = { "ellipse", "ellipse", TOOLKIND_SHAPE },
    [TOOL_ELLIPSE_FILL]     = { "filled ellipse", "ellipsefill", TOOLKIND_SHAPE },
    [TOOL_CONTOUR]          = { "contour", "contour", TOOLKIND_SHAPE },
    [TOOL_POLYGON]          = { "polygon", "polygon", TOOLKIND_SHAPE },
    [TOOL_MARQUEE]          = { "rectangular marquee", "marquee", TOOLKIND_SELECT },
    [TOOL_MARQUEE_ELLIPSE]  = { "elliptical marquee", "ellipsemarquee", TOOLKIND_SELECT },
    [TOOL_LASSO]            = { "lasso", "lasso", TOOLKIND_SELECT },
    [TOOL_LASSO_POLY]       = { "polygonal lasso", "polylasso", TOOLKIND_SELECT },
    [TOOL_WAND]             = { "magic wand", "wand", TOOLKIND_SELECT },
    [TOOL_MOVE]             = { "move", "move", TOOLKIND_MOVE },
    [TOOL_BLUR]             = { "blur", "blur", TOOLKIND_BRUSH },
    [TOOL_JUMBLE]           = { "jumble", "jumble", TOOLKIND_BRUSH },
    [TOOL_EYEDROPPER]       = { "eyedropper", "eyedropper", TOOLKIND_CLICK },
};

// Aseprite's default keys, docs/painting.md 18.3 (the developer's call 1: inside draw mode the letters are
// Aseprite's). One row per key and SHIFT state
static const DrawToolKey KEYS[] = {
    { KEY_B, false, TOOL_PENCIL, -1 },
    { KEY_E, false, TOOL_ERASER, -1 },
    { KEY_S, false, TOOL_SPRAY, -1 },
    { KEY_G, false, TOOL_BUCKET, -1 },
    { KEY_G, true, TOOL_GRADIENT, -1 },
    { KEY_L, false, TOOL_LINE, -1 },
    { KEY_L, true, TOOL_CURVE, -1 },
    { KEY_U, false, TOOL_RECT, TOOL_RECT_FILL },
    { KEY_U, true, TOOL_ELLIPSE, TOOL_ELLIPSE_FILL },
    { KEY_D, false, TOOL_CONTOUR, -1 },
    { KEY_D, true, TOOL_POLYGON, -1 },
    { KEY_M, false, TOOL_MARQUEE, -1 },
    { KEY_M, true, TOOL_MARQUEE_ELLIPSE, -1 },
    { KEY_Q, false, TOOL_LASSO, -1 },
    { KEY_Q, true, TOOL_LASSO_POLY, -1 },
    { KEY_W, false, TOOL_WAND, -1 },
    { KEY_V, false, TOOL_MOVE, -1 },
    { KEY_R, false, TOOL_BLUR, -1 },
    { KEY_R, true, TOOL_JUMBLE, -1 },
    { KEY_I, false, TOOL_EYEDROPPER, -1 },
};

#define KEY_ROWS ((int)(sizeof(KEYS) / sizeof(KEYS[0])))

static bool Valid(DrawTool t) { return (int)t >= 0 && t < TOOL_COUNT; }

const char *DrawToolName(DrawTool t) { return Valid(t) ? TOOLS[t].name : "?"; }
const char *DrawToolScriptName(DrawTool t) { return Valid(t) ? TOOLS[t].script : "?"; }
DrawToolKind DrawToolKindOf(DrawTool t) { return Valid(t) ? TOOLS[t].kind : TOOLKIND_BRUSH; }

int DrawToolByScriptName(const char *name)
{
    for (int t = 0; t < TOOL_COUNT; t++)
        if (strcmp(name, TOOLS[t].script) == 0) return t;
    return -1;
}

bool DrawToolPaints(DrawTool t)
{
    return Valid(t) && TOOLS[t].kind != TOOLKIND_SELECT && t != TOOL_EYEDROPPER;
}

const char *DrawToolKeyName(DrawTool t)
{
    static const char *const LETTERS[] = { "B", "E", "S", "G", "L", "U", "D", "M", "Q", "W", "V", "R", "I" };
    static const int CODES[] = { KEY_B, KEY_E, KEY_S, KEY_G, KEY_L, KEY_U, KEY_D, KEY_M, KEY_Q, KEY_W, KEY_V, KEY_R, KEY_I };
    static const char *const SHIFTED[] = { "SHIFT+B", "SHIFT+E", "SHIFT+S", "SHIFT+G", "SHIFT+L", "SHIFT+U", "SHIFT+D",
                                           "SHIFT+M", "SHIFT+Q", "SHIFT+W", "SHIFT+V", "SHIFT+R", "SHIFT+I" };
    for (int i = 0; i < KEY_ROWS; i++) {
        if (KEYS[i].tool != t && KEYS[i].again != (int)t) continue;
        for (int k = 0; k < (int)(sizeof(CODES) / sizeof(CODES[0])); k++)
            if (CODES[k] == KEYS[i].key) return KEYS[i].shift ? SHIFTED[k] : LETTERS[k];
    }
    return "";
}

int DrawToolKeyCount(void) { return KEY_ROWS; }
const DrawToolKey *DrawToolKeyAt(int i) { return i >= 0 && i < KEY_ROWS ? &KEYS[i] : NULL; }

int DrawToolForKey(int key, bool shift, DrawTool current)
{
    for (int i = 0; i < KEY_ROWS; i++) {
        const DrawToolKey *k = &KEYS[i];
        if (k->key != key || k->shift != shift) continue;
        if (k->again >= 0 && current == k->tool) return k->again;
        if (k->again >= 0 && (int)current == k->again) return k->tool;
        return k->tool;
    }
    return -1;
}
