// The draw mode's kept state (drawsettings.c; docs/painting.md 18.6): tool, brush, colours, palette,
// the tool options, density, pixel look and the panel, in a `key=value` file per map package
// (<package>/draw.ini), read when the editor opens the map and written (debounced) when it changes.
// Pure: no window, no scene (tests/unit/drawsettings_test.c); drawmode.c copies it to and from a
// DrawMode (DrawModeGetState / DrawModeSetState).
#ifndef MONOLITH_DRAWSETTINGS_H
#define MONOLITH_DRAWSETTINGS_H

#include <raylib.h>     // (Color only)
#include <stdbool.h>

#define DRAWSTATE_FILE "draw.ini"       // in the map package's folder (the developer's call 3: per map)
#define DRAWSTATE_RECENT 8              // the recent colours (DRAW_RECENT_COLORS)
#define DRAWSTATE_PALETTE_MAX 4096      // a sanity bound on palette= (a longer list: the rest ignored, warned)

typedef enum { DRAW_BUCKET_CONTIGUOUS = 0, DRAW_BUCKET_GLOBAL, DRAW_BUCKET_REGION } DrawBucketMode;
typedef enum { DRAW_PICK_PAINT = 0, DRAW_PICK_SCREEN } DrawPickMode;

typedef struct {
    int tool;               // DrawTool (drawkeys.h)
    int shape;              // the pencil's brush: PEN_ROUND / PEN_SQUARE / PEN_SOFT (drawmode.h)
    int size;               // 1 .. DRAW_SIZE_MAX texels
    float hardness;         // 0 .. 1
    bool pixelPerfect;
    Color fg, bg;           // the brush alpha is fg's (SHIFT+wheel)
    Color recent[DRAWSTATE_RECENT];
    int recentCount;
    Color *palette;         // heap (palette=), any length up to DRAWSTATE_PALETTE_MAX
    int paletteCount, paletteCap;
    int sprayWidth;         // texels (the disc's diameter), 1 .. 256
    int spraySpeed;         // dots per 1/60 s, 1 .. 256
    DrawBucketMode bucket;
    int tolerance;          // 0 .. 255 (bucket and magic wand)
    bool gradientRadial;
    DrawPickMode pickAlt;   // ALT+click
    DrawPickMode pickTool;  // the eyedropper tool (I; I again toggles)
    float density;          // texels per metre, 8 .. 512
    bool pixelLook;
    bool panelHidden;
} DrawState;

// Today's defaults (DrawModeInit's) and Aseprite's for the new options (docs/painting.md 18.6)
void DrawStateDefaults(DrawState *s);
void DrawStateFree(DrawState *s);                           // the palette; s keeps its other values
bool DrawStateCopy(DrawState *dst, const DrawState *src);   // deep (dst freed first); false: out of memory
bool DrawStateEqual(const DrawState *a, const DrawState *b);

// <root>/draw.ini as a heap string (root: the package folder, scene->root, with or without its
// trailing '/'); NULL: out of memory
char *DrawStatePath(const char *root);
// A missing file: true, s untouched, *ignored 0 (not an error). Unreadable: false (logged).
// A bad value or an unknown key: a warning each (logged with the line), that key keeps its value,
// the rest loads; *ignored = how many lines were ignored (GfxLoad's rules)
bool DrawStateLoad(DrawState *s, const char *path, int *ignored);
// Written whole (a temporary file renamed over it); false: not written (logged)
bool DrawStateSave(const DrawState *s, const char *path);
// One "key=value" line of the file's keys into s (the loader's parser): false on an unknown key or a
// bad value, s unchanged
bool DrawStateSetKey(DrawState *s, const char *keyValue);

#endif
