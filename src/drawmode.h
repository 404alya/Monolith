// Draw mode (B in spectator mode): painting on boxes and models, the canvases (in-memory RGBA8
// images of package PNGs), strokes, undo tiles in the shared edit history, saving the PNGs and
// their licence records (drawmode.c; design: docs/painting.md)
#ifndef MONOLITH_DRAWMODE_H
#define MONOLITH_DRAWMODE_H

#include "common.h"
#include "designer.h"
#include "editmode.h"

typedef enum {
    PEN_ROUND = 0,          // hard round
    PEN_SQUARE,             // s x s texels, pixel-exact
    PEN_SOFT,               // round with a soft rim (hardness)
    PEN_ERASER,             // puts back the canvas's base image
    PEN_FILL,               // the whole face cell / UV island
    PEN_COUNT
} PenKind;

#define DRAW_RECENT_COLORS 8
#define DRAW_SIZE_MAX 256
#define DRAW_DENSITY_DEFAULT 64.0f      // texels per metre of a new canvas
#define DRAW_CANVAS_MAX 2048            // px per side
#define DRAW_CANVAS_MIN 16
#define DRAW_TILE 64                    // undo tile side (texels)

// One canvas: the pixels of one package PNG, what shows it, and its base image (the eraser)
typedef struct {
    char *rel;              // package path ("textures/paint/paint_12.png")
    char *material;         // the material whose albedo_map it is (heap)
    Image img, base;        // RGBA8, the same size
    unsigned long long savedHash;   // FNV-1a 64 of the pixels as last written or read (0: never written)
    bool provided;          // given to the runtime (MS_TextureProvide): the scene shows it
    bool layout;            // a box canvas with uv_faces
    unsigned char cells[6]; // its layout (+x -x +y -y +z -z)
    int *island;            // models: island id per texel (-1: none; sharedTexel: more than one), NULL for boxes
    char *source;           // what it was made from, for the licence record (heap; NULL = a flat colour)
    // (phase 2)
    bool shared;            // a texture the map had, painted in place (docs/painting.md 3.2)
    bool recorded;          // its ASSETS.md row exists (or was written this session)
    bool written;           // its PNG was written this session
    bool stale;             // the scene was reloaded from disk and no step holds it: read it again
    int wrap;               // the material's wrap: 0 repeat, 1 clamp, 2 mirror
    int file, slot;         // a model slot's canvas: the model file and the slot, else -1
    int *triIsland;         // models: the island of each triangle of the slot (MS_ModelSlotTriangle's k)
    int triCount;
    unsigned char *sharedTexel; // models: texels several islands cover (1), NULL for boxes
    unsigned char *islandAll;   // models, per island: a triangle of it covers the whole texture (tiled UVs)
    int refs;               // paint steps that hold its tiles or images
    int *tileSlot;          // per 64 x 64 tile: its index in the stroke's tiles, -1 (the stroke going on)
    int tilesX, tilesY;
    bool dirty;             // this frame's stamps: [dx0, dx1) x [dy0, dy1) to upload
    int dx0, dy0, dx1, dy1;
    unsigned int rebaseGen; // the rebase that looked at it last (DrawMode.rebaseGen)
    bool replaying;         // that rebase found its PNG changed on disk: its strokes are replayed
    int replayed;           // ... how many
    // (a baked surface shader, docs/painting.md 3.6) the copy's ORM and normal maps: not painted,
    // provided and saved with the canvas; and what the three PNGs derive from (the licence records)
    Image orm, normal;      // RGBA8 (normal: empty when the shader doesn't change the normal)
    char *ormRel, *normalRel;       // their package paths (heap; NULL: none)
    bool extrasWritten;     // their PNGs were written this session
    char *bakeShader;       // the shader file (package path, heap); NULL: not a bake
    char *bakeTextures;     // its samplers' textures, one per line (heap; NULL: none)
} Canvas;

// The texture space of a surface: its canvas's, or the one its first stroke (or, in shared mode,
// opening its texture) will give it. The brush cursor maps texels through it; the panel's texel
// size comes from it
typedef struct {
    bool ok;                // known (a surface that can be painted)
    int w, h;               // texels
    bool layout;            // a box face layout (uv_faces)
    unsigned char cells[6];
    int wrap;               // 0 repeat, 1 clamp, 2 mirror
    Vector2 scale;          // texture space per unit of face / texture coordinate (the uv_scale)
    Vector2 uv;             // the hit in it (before the wrap)
    bool nearest;           // its material has `filter nearest`
    bool fresh;             // no canvas yet: the first stroke makes one this size
} DrawGeom;

// The surface the panel shows and acts on (docs/painting.md 13.2): the one the cursor last rested
// on in the scene (for 0.3 s, or clicked), so crossing other objects on the way to the panel
// doesn't change it. Found again (the same ray) when the scene changed
typedef struct {
    MS_SurfaceHit hit;
    int canvas;             // its canvas, -1: none yet
    bool ok;                // it can be painted
    char refusal[200];      // why not ("" when nothing is there)
    DrawGeom geom;
    Ray ray;                // the ray that found it
    unsigned int version;   // scene->version then
    unsigned int gen;       // +1 whenever it becomes another surface (object, face or slot)
    char what[200], into[240], texel[160];      // the panel's lines (DrawModeUpdate / DrawModeHover)
} DrawTarget;

// The draw mode's state (owned by main.c, beside the EditMode whose MapEditor it shares)
typedef struct {
    bool active;
    // Pen and colour (the panel edits these)
    PenKind pen;
    int size;               // texels, 1..DRAW_SIZE_MAX
    float hardness;         // soft round: 0..1
    Color color;
    Color recent[DRAW_RECENT_COLORS];
    int recentCount;
    float density;          // texels per metre for new canvases (and Resize to density)
    bool shared;            // paint the material's own texture (docs/painting.md 3.2)
    bool pixelLook;         // new canvases get `filter nearest`
    bool panelHidden;       // TAB
    // Canvases (heap array)
    Canvas *canvases;
    int canvasCount, canvasCap;
    // The stroke in progress (LMB held): its step, the last sample, the frame's dirty rectangle
    bool stroking;
    PaintStep *stroke;
    MS_SurfaceHit last;
    Vector2 lastPx;
    // What is under the cursor (every frame) and why it can't be painted ("" = it can)
    MS_SurfaceHit hover;
    char refusal[200];
    char message[256];
    float messageTime;
    bool messageError;
    // (phase 2)
    EditMode *em;           // the edit mode whose history it shares (the hooks' user is the DrawMode)
    Color prevColor;        // X swaps with it
    int strokeCanvas;       // the canvas of the stroke going on, -1
    bool strokePushed;      // its step is in the history already (a first stroke)
    bool lastOk;            // the last sample was on the stroke's canvas
    float lastTx, lastTy;   // ... its texel (canvas pixels)
    int lastRegion;         // ... its face (boxes) / island (models), -1
    int lastCanvas;         // the canvas painted last (--paint-script `faces`, `resize`, `pixel`)
    int hoverCanvas;        // the canvas of the surface under the cursor, -1
    Vector2 cursorPx;       // where the cursor is (window pixels)
    bool hoverOk;           // hover is a surface that can be painted
    float leaveWarnTime;    // B / ESC with unsaved paint warned: again within this leaves
    unsigned int rebaseGen;
    // Per material of the scene (rebuilt when scene->version changes): its albedo_map, wrap, the
    // canvas showing it (-1)
    struct DrawMat *mats;
    int matCount, matCap;
    unsigned int matVersion;
    bool matValid;
    // The eyedropper's CPU copy of the last texture it read (by package path, until the next load)
    char *pickRel;
    Image pickImg;
    unsigned int pickLoad;
    bool keysBlocked;       // set by the caller per frame: a panel field has the keyboard
    // (phase 3b) the brush cursor and the panel
    DrawGeom hoverGeom;     // the texture space of the surface under the cursor
    Ray hoverRay;           // the ray that found dm->hover (cast again after the scene changed)
    unsigned int hoverVersion;
    bool onPanel;          // the cursor is on the panel this frame (no brush cursor)
    DrawTarget target;      // what the panel shows and acts on
    MS_SurfaceHit settling; // the surface under the cursor that isn't the target yet ...
    float settleTime;       // ... for this long
    struct DrawSizeCache *sizes;    // a new canvas's size per surface (2 entries, heap)
    struct DrawSlotTris *slotTris;  // a model slot's triangles, UV islands and an island's outline (heap)
    // (the shader bake, docs/painting.md 3.6) LMB on a box with a surface-shader material asks for
    // a bake; DrawModeRunBake renders it before the next scene draw, and the stroke begins (the
    // first stroke's edit) in the frame after it. A script bakes at once (bakeNow)
    bool bakeWant, bakeReady, bakeNow;
    MS_SurfaceHit bakeHit;  // where LMB went down
    Vector2 bakePx;
    unsigned int bakeVersion;       // scene->version then (another one: the bake is dropped)
    int bakeW, bakeH;               // the canvas it was baked for
    struct SurfaceBake *bake;       // the result (heap)
} DrawMode;

typedef enum {
    DRAWMODE_NONE = 0,
    DRAWMODE_RELOADED,      // the scene's text changed live (a first stroke, undo of one): as EditMode's
} DrawModeResult;

// Sets the defaults and the edit history's paint hooks (call once, after EditMode is set up)
void DrawModeInit(DrawMode *dm, EditMode *em);
// B in spectator mode: the mode on (edit, volume and snip mode end); off again
void DrawModeOpen(DrawMode *dm, EditMode *em, Designer *d);
void DrawModeClose(DrawMode *dm);
// Spectator-mode input while the mode is on (B toggles it in main.c): LMB strokes at the cursor,
// ALT+LMB the eyedropper, [ ] / CTRL+wheel the size, 1..5 the pen, X, H, TAB; CTRL+Z / CTRL+SHIFT+Z
// and CTRL+S go through the edit mode (one history). cursorOnPanel: the panel has the mouse.
DrawModeResult DrawModeUpdate(DrawMode *dm, EditMode *em, Designer *d, Camera3D cam, bool cursorOnPanel, float dt);
// What the cursor at px is over (dm->hover, refusal, hoverCanvas): DrawModeUpdate does it; a shot
// without input calls it for the HUD and the cursor
void DrawModeHover(DrawMode *dm, Camera3D cam, Vector2 px, int w, int h);
// The panel's request (DrawPanelUpdate's action: 1 resize, 2 faces, 3 pixel look) on the panel's
// target (dm->target: its canvas; none: said in the message line); faces: the panel's face groups
// (action 2)
DrawModeResult DrawModePanelAction(DrawMode *dm, EditMode *em, int action, const unsigned char faces[6]);
// The brush footprint on the surface under the cursor (3D, after R3D_End, like DesignerDrawOverlay;
// nothing while the cursor is on the panel or nothing paintable is under it)
void DrawModeDrawCursor(const DrawMode *dm, Camera3D cam);
// The HUD line (state only: pen, size, colour, the canvas and face, unsaved edits) at (x, y);
// returns the height used
int DrawModeDrawHud(const DrawMode *dm, const EditMode *em, int x, int y);
// The refusal under the cursor (red) and the last message, at (x, y); returns the height used
int DrawModeDrawMessages(const DrawMode *dm, int x, int y);
// After a reload from disk or a renderer restart, around EditModeSync: DrawModeBeforeSync first
// (the canvases are provided again, so the working copy and a rebase's first strokes can go
// live), then EditModeSync (its rebase runs the paint hooks: canvases provided again, unsaved
// strokes replayed onto PNGs that changed on disk; docs/painting.md 12), then DrawModeSync: a
// restart uploads the canvases again; a reload reports the replays, and the canvases no step
// holds any more are read from disk again when next painted
void DrawModeBeforeSync(DrawMode *dm);
void DrawModeSync(DrawMode *dm, EditMode *em, bool newScene);
// CTRL+S: the unsaved canvases' PNGs, then the map (MapEditSave), the licence records, then
// MS_EditMarkSaved (the edit history's save hook: EditModeSave and auto save call it too)
bool DrawModeSave(DrawMode *dm, EditMode *em, char *msg, int msgSize);
// Canvases with unsaved pixels
int DrawModeUnsavedCanvases(const DrawMode *dm);
// Closing with unsaved paint: the canvases under dir (<settings>/unsaved/<map>-<time>/)
bool DrawModeWriteUnsaved(const DrawMode *dm, const char *dir);
// After EditModeFree (the history releases its paint through the hooks first)
void DrawModeFree(DrawMode *dm);
// The shader bake a first stroke asked for (docs/painting.md 3.6): call every frame between frames,
// outside R3D_Begin / End, before the scene's draw (beside the model picker's thumbnails). Nothing
// when none is wanted; the stroke begins in the next DrawModeUpdate
void DrawModeRunBake(DrawMode *dm);

// --paint-script <file> (docs/painting.md 14): strokes applied live to the loaded GPU scene (after
// --edit-script; nothing written unless the script says `save`). cam: the shot's camera, w x h its
// size (for `px` coordinates). Returns the exit status.
int DrawModeRunScript(DrawMode *dm, EditMode *em, Designer *d, Camera3D cam, int w, int h, const char *path);

#endif
