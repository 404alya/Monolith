// Draw mode (B in spectator mode): painting on boxes and models into the paint layer (per-material
// atlases of the map's `paint` statements), strokes, undo tiles in the shared edit history, saving
// the atlases, unwrap sidecars and their licence records (drawmode.c; design: docs/painting.md)
#ifndef MONOLITH_DRAWMODE_H
#define MONOLITH_DRAWMODE_H

#include "common.h"
#include "designer.h"
#include "drawkeys.h"
#include "drawsettings.h"
#include "drawtools.h"
#include "editmode.h"

// How a stroke's stamps lay down texels (a step's pen; docs/painting.md 7.2, 18.1). The pencil's
// brush shape is one of the first three (DrawMode.shape); the eraser stamps PEN_ERASER, the bucket in
// "whole region" mode PEN_FILL (today's fill). --paint-script `pen <name>` and the keys 1 .. 5 still
// name all five: they set the tool and the shape (DrawModeSetPen)
typedef enum {
    PEN_ROUND = 0,          // hard round
    PEN_SQUARE,             // s x s texels, pixel-exact
    PEN_SOFT,               // round with a soft rim (hardness)
    PEN_ERASER,             // true erase: lowers the texels' alpha
    PEN_FILL,               // the whole rectangle (a box face group) / UV island (a model)
    PEN_COUNT
} PenKind;

#define DRAW_RECENT_COLORS 8
#define DRAW_SIZE_MAX 256
#define DRAW_DENSITY_DEFAULT 64.0f      // texels per metre of new paint (docs/painting.md 5.2)
#define DRAW_RECT_MIN 4                 // a box face group's rectangle: each side 4 .. 2048 texels (on a
                                        // paint grid: its cells, 1 .. 2048; a model slot's: 1 .. 2048)
#define DRAW_RECT_MAX 2048
#define DRAW_ATLAS_MIN 256              // an atlas: a power-of-two square from 256, growing to 4096 x 4096
#define DRAW_ATLAS_MAX 4096
#define DRAW_TILE 64                    // undo tile side (texels)

// One atlas: the pixels of one textures/paint/ PNG (straight alpha RGBA8), the material it belongs
// to, and the tiles of the stroke going on
typedef struct {
    char *rel;              // package path ("textures/paint/concrete.png")
    char *material;         // its material's name (heap); NULL: a model slot's own material
    char *modelPath;        // own atlases: the model file (heap) and the slot
    int slot;
    Image img;              // 0 x 0 (data NULL): nothing yet (a new atlas before its first rectangle)
    unsigned long long savedHash;   // FNV-1a 64 of the pixels as last written or read (0: never written)
    bool provided;          // given to the runtime (MS_PaintAtlasProvide): the scene shows these pixels
    bool recorded;          // its ASSETS.md row exists (or was written this session)
    bool written;           // its PNG was written this session
    bool stale;             // the scene was reloaded from disk and no step holds it: read it again
    int refs;               // paint steps that hold its tiles
    int *tileSlot;          // per 64 x 64 tile: its index in the stroke's tiles, -1 (the stroke going on)
    int tilesX, tilesY;
    bool dirty;             // this frame's stamps: [dx0, dx1) x [dy0, dy1) to upload
    int dx0, dy0, dx1, dy1;
    unsigned int rebaseGen; // the rebase that looked at it last (DrawMode.rebaseGen)
    bool replaying;         // that rebase found its PNG changed on disk: its strokes are replayed
    int replayed;           // ... how many
} Atlas;

// A paint unwrap made this session (docs/painting.md 4.5, 17.5): provided to the runtime, written as
// the model's .paintuv sidecar on the save that first needs it
typedef struct {
    char *modelPath;        // the model file (package path, heap)
    MS_PaintUnwrap u;
    bool written;
    float texelsPerUnit;    // what it was packed at (texels per model unit) ...
    float scale;            // ... for an instance of this scale (largest of its three)
    int *sides;             // per slot of the file: its square's side (-1: no mesh), the rectangle a
    int sideCount;          // first stroke allocates (heap)
} DrawUnwrap;

// Where paint at a surface goes (its entry's rectangle, or the one its first stroke will get): the
// brush cursor maps texels through it, the panel's texel size comes from it
typedef struct {
    bool ok;                // known (a surface that can be painted)
    bool own;               // a model slot drawn with its own material
    int entry;              // MS_PaintEntryAt index of its active entry, -1: none yet (fresh)
    int x, y, w, h;         // the entry's rectangle (fresh: x y -1, w h the size the first stroke takes)
    int atlasW, atlasH;     // its atlas's size now (0 x 0: none yet)
    unsigned char faces;    // boxes: the entry's faces (fresh: the hit face)
    bool nearest;           // the atlas's paint_filter nearest (fresh atlas: the pixel look switch)
    bool needsUnwrap;       // a model slot without paint UVs yet: the first stroke unwraps the file
    bool fresh;             // no entry yet
    bool estimate;          // fresh w h are an estimate (the unwrap that sets them isn't made yet)
    Vector2 uv;             // the hit in the entry's paint coordinates (0..1; -1: no paint UVs yet)
    float grid;             // the paint grid of the material (MS_PaintGridOf; docs/painting.md 17), 0: none
    // Where the surface's 0 and 1 lie in the rectangle's texels (texel = u0 + u (u1 - u0)): a box face
    // on a grid its first cell's offset and end (17.2), else 0 .. w and 0 .. h; and the texels the
    // hit face shows (a box face on a grid: its cells, at most the rectangle; else the rectangle)
    float u0, u1, v0, v1;
    int cellsW, cellsH;
} DrawGeom;

// What a per-frame text was built from (drawmode.c TextKeyOf): the texts hold names, so they are heap
// strings, rebuilt only when this changes (the hover and the target resolve every frame)
typedef struct {
    bool set;
    unsigned char code;
    int kind, index, face, slot, material;
    unsigned int version;
    bool ok, fresh, needsUnwrap, estimate;
    int atlasW, atlasH, w, h, x, y;
    float grid, density;    // the grid's texels per metre (17.7), the panel's density
} DrawTextKey;

// The surface the panel shows and acts on (docs/painting.md 13.2): the one the cursor last rested
// on in the scene (for 0.3 s, or clicked), so crossing other objects on the way to the panel
// doesn't change it. Found again (the same ray) when the scene changed
typedef struct {
    MS_SurfaceHit hit;
    bool ok;                // it can be painted
    unsigned char refusalCode;  // Resolve's code: 0 paintable, 1 nothing there, 2.. a refusal (its words:)
    MonoMsg refusal;        // why not (codes >= 2; rebuilt only when refusalKey changes: no per-frame allocation)
    DrawTextKey refusalKey;
    DrawGeom geom;
    Ray ray;                // the ray that found it
    unsigned int version;   // scene->version then
    unsigned int gen;       // +1 whenever it becomes another surface (object, face or slot)
    // The panel's lines (DrawModeUpdate / DrawModeHover): what it is, what paint there goes into (set
    // when ok), the texel size (texelSet); rebuilt only when textKey changes (they hold names)
    MonoMsg what, into, texel;
    bool intoSet, texelSet;
    float grid;             // the paint grid the target's paint follows (MS_PaintGridOf, texels per
                            // metre; docs/painting.md 17.7): the panel shows "Grid" and Fit to grid,
                            // no density slider; 0: none (the panel's density applies)
    MonoMsg gridLine;       // the panel's line about it: "follows the grid of concrete: 24 texels/m
    bool gridLineSet;       // (4.2 cm)" / "concrete has no paint grid: 64 texels/m from the panel"
    bool fitOk;             // Fit to grid can act on it (an entry on a grid, also one too small for
                            // its face's cells: Resolve's code 10)
    DrawTextKey textKey;
    bool facesOk;           // a box: the Faces section shows its groups
    int faceKey;            // the box and material the groups belong to (-1: none)
    unsigned char groups[6];// per face +x .. -z: its group (faces in one entry share a number; a face
                            // without paint has a number of its own)
} DrawTarget;

// Where a stroke's stamps go (docs/painting.md 7.1, 18.4): an entry's rectangle in atlas texels (a box
// face group, on a grid its face's cells), or a model's UV island inside it. A shape, a selection and
// a fill never leave their region
typedef struct {
    int x, y, w, h;
    int file, slot;         // a model slot (its islands); -1 for a box face group
    int island;             // -1: the whole rectangle
} DrawRegion;

// Line segments on a surface (world space, pairs of points: heap), built when what they show changed:
// the selection's outline, a selection shape's preview
typedef struct {
    Vector3 *p;
    int n, cap;
} DrawLines;

// The selection (docs/painting.md 18.5): editor state, never written into the map, not in the edit
// history. One region at a time: a selection made on another region replaces it. While it is on,
// every painting tool on its region paints only inside it (other regions are unaffected)
typedef struct {
    bool active;
    int atlas;              // DrawMode.atlases index
    DrawRegion region;
    MS_SurfaceHit hit;      // the surface it was made on (its outline is drawn through that surface)
    DtMask mask;            // the region's rectangle, 255 selected (heap)
    DtMask regionMask;      // the region's texels (255), the same rectangle (heap)
    DtMask last;            // what CTRL+D dropped (CTRL+SHIFT+D brings it back on the same region)
    bool lastSet;
    DrawRegion lastRegion;
    int lastAtlas;
    MS_SurfaceHit lastHit;
    unsigned int gen;       // +1 whenever the mask or its region changes
    DrawLines outline;      // the mask's border on its surface (built for gen at outlineVersion)
    unsigned int outlineGen, outlineVersion;
    bool outlineSet;
} DrawSelection;

// A shape or selection in progress (docs/painting.md 18.4): the points in atlas texels of one region,
// previewed live in the atlas from the stroke's start pixels while the mouse is down (or the clicks of
// a polygon / polygonal lasso / curve go on), one stroke on release. ESC drops it
typedef struct {
    bool on;
    DrawTool tool;
    bool rmb;               // started with RMB: the background colour
    int stage;              // curve: 0 the line's drag, 1 and 2 its control points; polygon / polygonal
                            // lasso: the clicks so far
    DtSelOp selOp;          // selection tools: from SHIFT / ALT at the press
    DtPoints pts;           // atlas texels (heap): 2 (line, rectangle, ellipse, gradient, marquee),
                            // 4 (curve: a, b, c1, c2), n (contour, polygon, lasso, polygonal lasso)
    MS_SurfaceHit hit;      // the surface of its first point
    DrawRegion region;
    int atlas;
    MS_SurfaceHit lastOn;   // the last hit on the region (a point off it maps through its plane)
    bool down;              // its button is held (a drag, or a click of the clicked tools)
    float clickTime;        // GetTime() of the last click (a double click closes a polygon)
    DtMask preview;         // a selection shape: its mask so far (heap), and its outline
    DrawLines outline;
} DrawShape;

// The draw mode's state (owned by main.c, beside the EditMode whose MapEditor it shares)
typedef struct {
    bool active;
    // Tool, brush and colours (the panel and the keys edit these; kept per map: docs/painting.md 18.6)
    DrawTool tool;
    PenKind shape;          // the pencil's brush: PEN_ROUND, PEN_SQUARE, PEN_SOFT
    int size;               // texels, 1..DRAW_SIZE_MAX
    float hardness;         // soft round: 0..1
    bool pixelPerfect;      // the pencil at size 1: Aseprite's pixel-perfect
    Color fg, bg;           // foreground (LMB) and background (RMB) colour; X swaps them. The brush
                            // alpha is fg's (SHIFT+wheel)
    Color recent[DRAW_RECENT_COLORS];
    int recentCount;
    Color *palette;         // the panel's palette row (heap; kept)
    int paletteCount, paletteCap;
    int sprayWidth, spraySpeed;     // the spray: disc diameter in texels, dots per 1/60 s
    DrawBucketMode bucket;  // contiguous, global, the whole region (today's fill)
    int tolerance;          // bucket and magic wand, 0..255
    bool gradientRadial;
    DrawPickMode pickAlt;   // what ALT+click samples (paint / albedo, or the screen)
    DrawPickMode pickTool;  // what the eyedropper tool samples
    float density;          // texels per metre of new rectangles (and Resize to density) on materials
                            // without a paint grid (docs/painting.md 17.7)
    bool pixelLook;         // new atlases of materials without a paint grid get paint_filter nearest
    bool gridFiltered;      // new atlases on a paint grid stay filtered: Pixel look turned off for them
                            // (session only, not kept); by default their first stroke writes
                            // paint_filter nearest (docs/painting.md 17.7)
    bool panelHidden;       // TAB
    DrawSelection sel;
    DrawShape shapeDrag;
    uint32_t seed;          // the spray's and jumble's next stroke (+1 per stroke; --paint-script spray sets it)
    // The screen eyedropper (docs/painting.md 18.8): a click asks, main.c reads the pixel of the scene's
    // image (before the HUD) on its next frame and hands it over (DrawModeScreenWanted / ..Picked)
    bool screenWant;
    int screenX, screenY;
    bool screenToFg;        // (RMB: the background colour)
    // --paint-script `pick screen`: renders and reads at once (main.c sets it; NULL: refused)
    bool (*screenRead)(void *user, int x, int y, Color *out);
    void *screenUser;
    // Atlases and unwraps (heap arrays)
    Atlas *atlases;
    int atlasCount, atlasCap;
    DrawUnwrap *unwraps;
    int unwrapCount, unwrapCap;
    // The stroke in progress (LMB held): its step, the surface it stays on, the last sample
    bool stroking;
    struct PaintStep *stroke;
    bool strokePushed;      // its step is in the history already (it allocated a rectangle)
    MS_ObjRef strokeObj;    // the object, material (index) and slot (models) of its LMB down
    int strokeMaterial, strokeSlot, strokeAtlas;
    Vector2 lastPx;
    bool lastOk;            // the last sample was on the stroke's surface
    float lastTx, lastTy;   // ... its texel (atlas pixels)
    int lastRegion;         // ... its region (the step's)
    bool strokeRmb;         // the stroke's button is RMB (the background colour)
    float sprayTime;        // the spray's time not yet sprayed (1/60 s ticks)
    bool ppDirty;           // a pixel-perfect stroke got samples: redrawn before the frame's upload
    // Where the pencil or eraser painted last (SHIFT+click draws a line from it on the same region)
    bool lastPaintSet;
    DrawRegion lastPaintRegion;
    int lastPaintAtlas;
    float lastPaintTx, lastPaintTy;
    MS_SurfaceHit lastHit;  // the surface of the last stroke's LMB down (--paint-script's faces,
    bool lastHitSet;        // resize, clear, pixel)
    // What is under the cursor (every frame) and why it can't be painted (refusalCode: Resolve's
    // code, 0 paintable, 1 nothing there, 2.. a refusal, its text in refusal: per-frame state, bounded)
    MS_SurfaceHit hover;
    unsigned char refusalCode;
    MonoMsg refusal;        // (rebuilt only when refusalKey changes)
    DrawTextKey refusalKey;
    // The last message (an event: SayStatic / Sayf set it), shown while messageTime runs; messageSaid:
    // one was said since --paint-script cleared it for its next command
    MonoMsg message;
    float messageTime;
    bool messageError;
    bool messageSaid;
    EditMode *em;           // the edit mode whose history it shares (the hooks' user is the DrawMode)
    Vector2 cursorPx;       // where the cursor is (window pixels)
    bool hoverOk;           // hover is a surface that can be painted
    float leaveWarnTime;    // ESC with unsaved paint warned: again within this leaves
    unsigned int rebaseGen;
    bool keysBlocked;       // set by the caller per frame: a panel field has the keyboard
    DrawGeom hoverGeom;     // where paint under the cursor goes
    Ray hoverRay;           // the ray that found dm->hover (cast again after the scene changed)
    unsigned int hoverVersion;
    bool onPanel;           // the cursor is on the panel this frame (no brush cursor)
    DrawTarget target;      // what the panel shows and acts on
    MS_SurfaceHit settling; // the surface under the cursor that isn't the target yet ...
    float settleTime;       // ... for this long
    // Caches (heap): per material whether its shader takes the paint stage, a model slot's paint
    // triangles and islands (the cursor), the UV islands of a slot at a rectangle's size (strokes)
    signed char *shaderOk;
    int shaderOkCount, shaderOkCap;
    unsigned int shaderOkVersion;
    struct DrawSlotTris *slotTris;
    struct DrawIslands *islands;
    // A first stroke on a model that needs a paint unwrap (docs/painting.md 4.5): the note shows for a
    // frame, then the next DrawModeUpdate unwraps the file and begins the stroke where LMB went down
    bool unwrapWant;
    MS_SurfaceHit unwrapHit;
    Vector2 unwrapPx;
    unsigned int unwrapVersion;
} DrawMode;

typedef enum {
    DRAWMODE_NONE = 0,
    DRAWMODE_RELOADED,      // the scene's text changed live (a first stroke, undo of one): as EditMode's
} DrawModeResult;

// The panel's requests (DrawModePanelAction)
enum { DRAW_ACT_RESIZE = 1, DRAW_ACT_FACES, DRAW_ACT_PIXEL_LOOK, DRAW_ACT_CLEAR };

// Sets the defaults and the edit history's paint hooks (call once, after EditMode is set up)
void DrawModeInit(DrawMode *dm, EditMode *em);
// The keys 1 .. 5 and --paint-script `pen`: round / square / soft set the pencil and its shape,
// eraser the eraser, fill the bucket in whole-region mode
void DrawModeSetPen(DrawMode *dm, PenKind pen);
// The pen a stroke of the current tool stamps with (pencil: its shape; eraser; bucket in region mode: fill)
PenKind DrawModeStampPen(const DrawMode *dm);
// The kept state (docs/painting.md 18.6): to / from the per-map file's record. Get copies the palette
// (false: out of memory); Set clamps every value and keeps dm's old palette when it can't copy it.
// Differs: whether s no longer holds dm's state (every frame: no allocation)
bool DrawModeGetState(const DrawMode *dm, DrawState *s);
bool DrawModeSetState(DrawMode *dm, const DrawState *s);
bool DrawModeStateDiffers(const DrawMode *dm, const DrawState *s);
// The screen eyedropper (docs/painting.md 18.8): a click asked for the pixel at (x, y) of the window
// (true); main.c reads it from the scene's image right after the scene is drawn, before any overlay or
// HUD, and calls Picked (ok false: it couldn't be read)
bool DrawModeScreenWanted(const DrawMode *dm, int *x, int *y);
void DrawModeScreenPicked(DrawMode *dm, bool ok, Color c);
// B in spectator mode: the mode on (edit, volume and snip mode end); off again
void DrawModeOpen(DrawMode *dm, EditMode *em, Designer *d);
void DrawModeClose(DrawMode *dm);
// Spectator-mode input while the mode is on (B in spectator mode opens it, ESC leaves): the tools of
// docs/painting.md 18.3 on LMB (and RMB: the background colour), ALT+LMB the eyedropper, [ ] /
// CTRL+wheel the size, 1..5 the pens, X, TAB, the selection keys; CTRL+Z / CTRL+SHIFT+Z and CTRL+S go
// through the edit mode (one history). Nothing is painted while SPACE or MMB is down (the camera's,
// spectator.c). cursorOnPanel: the panel has the mouse.
DrawModeResult DrawModeUpdate(DrawMode *dm, EditMode *em, Designer *d, Camera3D cam, bool cursorOnPanel, float dt);
// What the cursor at px is over (dm->hover, refusal, hoverGeom): DrawModeUpdate does it; a shot
// without input calls it for the HUD and the cursor
void DrawModeHover(DrawMode *dm, Camera3D cam, Vector2 px, int w, int h);
// The panel's request (DRAW_ACT_*: resize, faces, pixel look, clear) on the panel's target
// (dm->target; none: said in the message line); faces: the panel's face groups (DRAW_ACT_FACES)
DrawModeResult DrawModePanelAction(DrawMode *dm, EditMode *em, int action, const unsigned char faces[6]);
// The brush footprint on the surface under the cursor (3D, after R3D_End, like DesignerDrawOverlay;
// nothing while the cursor is on the panel or nothing paintable is under it)
void DrawModeDrawCursor(const DrawMode *dm, Camera3D cam);
// The HUD line (state only: pen, size, colour, the atlas and face, unsaved edits) at (x, y);
// returns the height used
int DrawModeDrawHud(const DrawMode *dm, const EditMode *em, int x, int y);
// The refusal under the cursor (red) and the last message, at (x, y); returns the height used
int DrawModeDrawMessages(const DrawMode *dm, int x, int y);
// After a reload from disk or a renderer restart, around EditModeSync: DrawModeBeforeSync first
// (the atlases and unwraps are provided again, so the working copy and a rebase's first strokes can
// go live), then EditModeSync (its rebase runs the paint hooks: atlases provided again, unsaved
// strokes replayed onto PNGs that changed on disk; docs/painting.md 12), then DrawModeSync: a
// restart uploads the atlases again; a reload reports the replays, and the atlases no step holds
// any more are read from disk again when next painted
void DrawModeBeforeSync(DrawMode *dm);
void DrawModeSync(DrawMode *dm, EditMode *em, bool newScene);
// CTRL+S: the unsaved atlases' PNGs and the new unwrap sidecars, then the map (MapEditSave), the
// licence records, then MS_EditMarkSaved (the edit history's save hook: EditModeSave and auto save
// call it too). Logs "monolith: <text>" itself (INFO for 0 / 1, WARNING for 2), except when the map
// changed on disk (nothing tried: the caller says that one). Returns:
//   0  saved: the text in the editor's note (MapEditNote(&em->ed)): "saved ...; %d paint file%s written ..."
//   1  nothing to save: the map was up to date, no paint file or licence record written (the note)
//   2  not saved, or saved only in part: the whole sentence in MS_LastError()
unsigned char DrawModeSave(DrawMode *dm, EditMode *em);
// Atlases with unsaved pixels
int DrawModeUnsavedAtlases(const DrawMode *dm);
// Closing with unsaved paint: the atlases (and unwraps) under dir (<settings>/unsaved/<map>-<time>/)
bool DrawModeWriteUnsaved(const DrawMode *dm, const char *dir);
// After EditModeFree (the history releases its paint through the hooks first)
void DrawModeFree(DrawMode *dm);

// --paint-script <file> (docs/painting.md 14): strokes applied live to the loaded GPU scene (after
// --edit-script; nothing written unless the script says `save`). cam: the shot's camera, w x h its
// size (for `px` coordinates). Returns the exit status.
int DrawModeRunScript(DrawMode *dm, EditMode *em, Designer *d, Camera3D cam, int w, int h, const char *path);

#endif
