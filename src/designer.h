// Designer: crosshair picking, selection and the prompt box in spectator mode
#ifndef MONOLITH_DESIGNER_H
#define MONOLITH_DESIGNER_H

#include "world.h"

#define MAX_SELECTION   512
#define PROMPT_MAX      4096    // bytes, including the terminating 0
#define MAX_REGIONS     16      // screen regions per request (snip mode, --region)
#define REGION_MAX_SAMPLES 4096 // rays per region at most (a regular grid, see DesignerAnalyzeRegion)
#define REGION_MAX_OBJECTS 256  // objects listed per region (the rest is summed up as "omitted")

// Object references as in the scene runtime (runtime/monolith_scene.h, docs/design-v2.md
// section 5). Until the runtime is merged only boxes exist (index into world.items); once
// monolith_scene.h is included before this header, these stand-ins disappear.
#ifndef MONOLITH_SCENE_H
typedef enum { MS_OBJ_NONE = 0, MS_OBJ_BOX, MS_OBJ_MODEL } MS_ObjKind;
typedef struct { MS_ObjKind kind; int index; } MS_ObjRef;
#endif

// What the crosshair points at. box = index into world.items, -1 = nothing hit.
typedef struct {
    int box;
    Vector3 point;          // world-space hit point
    Vector3 normal;         // axis-aligned face normal
    float dist;             // from the eye, metres
} DesignerHit;

// A screen region (snip mode): pixels of the frame the request is made from (= view.png),
// top-left origin. Tied to the camera frozen while it was drawn.
typedef struct { Rectangle rect; } DesignRegion;

typedef struct {
    MS_ObjRef obj;
    int samples;            // rays of the region that hit this object first
    float share;            // samples / all samples of the region
} DesignRegionObject;

// What a region shows, computed at submit time by DesignerAnalyzeRegion (ray sampling)
typedef struct {
    Rectangle rect;         // clamped to the frame, whole pixels
    int gridW, gridH;       // sample grid (gridW * gridH <= REGION_MAX_SAMPLES)
    MS_ObjRef centerObj;    // under the rect's centre; kind MS_OBJ_NONE = sky
    Vector3 centerPoint, centerNormal;
    float centerDist;       // from the eye, metres
    bool hasDepth;          // any sample hit something
    float depthMin, depthMax;   // distance from the eye over the samples that hit, metres
    DesignRegionObject objects[REGION_MAX_OBJECTS];     // by samples, descending
    int objectCount;
    int omittedObjects;     // objects hit but not listed (more than REGION_MAX_OBJECTS)
    float omittedShare;
    float sky;              // share of samples that hit nothing
    double ms;              // time the analysis took
} DesignRegionInfo;

// One request as it is submitted: a snapshot of everything the launcher writes into the
// bundle. Object indices refer to `world` at submit time (world.version tells which load).
typedef struct {
    char prompt[PROMPT_MAX];
    Vector3 eye;
    float yaw, pitch;       // radians, project convention (yaw 0 looks toward +Z)
    float fovy;             // degrees
    int width, height;      // render size of view.png
    DesignerHit hit;
    MS_ObjRef selected[MAX_SELECTION];
    int selectedCount;
    DesignRegionInfo regions[MAX_REGIONS];
    int regionCount;
    unsigned int mapVersion;
} DesignRequest;

// ======================================================================================
// Designer state (designer.c): one instance, owned by main()
// ======================================================================================

// What a selected box was, so the selection survives a map hot reload (see DesignerSync)
typedef struct {
    BoundingBox box;
    int line;               // map line of the box
    int nth;                // index among the boxes of that line
    int group;              // group index at selection time (compared by name after a reload)
    char groupName[GROUP_NAME_MAX];
} DesignerSelKey;

typedef struct {
    bool interactive;       // a visible window: may show/hide the mouse cursor

    DesignerHit hit;        // under the crosshair (refreshed by DesignerPick / DesignerUpdate)

    int selected[MAX_SELECTION];    // indices into world.items, no duplicates
    DesignerSelKey keys[MAX_SELECTION];
    int selectedCount;
    unsigned int version;   // world.version the indices refer to

    // Snip mode (C): screen regions dragged out over a frozen camera. Regions are
    // screen-space, so a map reload leaves them alone (what they show is computed at submit).
    bool snipping;          // snip mode on: camera frozen, cursor free, LMB drags regions
    DesignRegion regions[MAX_REGIONS];
    int regionCount;
    bool dragging;
    bool dragAdd;           // Shift was down at the drag start: add instead of replace
    Vector2 dragStart, dragEnd;

    // Prompt box
    bool promptOpen;
    char prompt[PROMPT_MAX];
    int promptLen;          // bytes, excluding the terminating 0
    int cursor;             // byte offset, always on a UTF-8 codepoint boundary
    float wantX;            // Up/Down keep this x (pixels); < 0 = take it from the cursor
    int scroll;             // first visible wrapped line
    int freezeFrames;       // camera stays frozen this many more frames after the box closes

    char message[256];      // HUD message (submit result, selection notes)
    float messageTime;
    bool messageError;
} Designer;

typedef enum {
    DESIGNER_NONE = 0,
    DESIGNER_SUBMIT,        // Enter in the prompt box: main() renders the images and calls LauncherSubmit
} DesignerAction;

// Casts a ray against world.items (slab test); hit.box = -1 when nothing is hit.
// Boxes that contain the ray origin are ignored (you can pick out of a box you're inside).
DesignerHit DesignerPick(Vector3 origin, Vector3 dir);

// Call once per frame before anything reads selected[]: after a map reload it remaps the
// selection onto the new world (same geometry anywhere, else same map line + group).
void DesignerSync(Designer *d);

// d->hit = what the camera's centre ray hits. Call after the camera moved, before drawing.
void DesignerPickCamera(Designer *d, Camera3D cam);

// Spectator-mode input: clicks (selection, against d->hit, i.e. what the last frame
// showed under the crosshair), Enter (opens the prompt box) and all prompt editing.
// Call only in spectator mode, before the camera update.
DesignerAction DesignerUpdate(Designer *d, float dt);

// True while the prompt box is open, snip mode is on, or the camera is still frozen
// after either closed: the caller must skip game keys and camera movement then
bool DesignerCapturesInput(const Designer *d);

// Snip mode. Enter: freezes the camera, frees the cursor. Leave: drops the regions.
void DesignerSnipBegin(Designer *d);
void DesignerSnipEnd(Designer *d);
// Adds a region (pixels, clamped to a w x h frame; --region). False if full or empty.
bool DesignerAddRegion(Designer *d, Rectangle rect, int w, int h);

// What region `rect` (pixels of a w x h frame rendered with `cam`) shows: a regular grid of
// at most REGION_MAX_SAMPLES rays through pixel centres, plus one through the rect's centre
void DesignerAnalyzeRegion(Camera3D cam, int w, int h, Rectangle rect, DesignRegionInfo *out);

void DesignerOpenPrompt(Designer *d, const char *text);
void DesignerMessage(Designer *d, bool error, const char *text);

// Selection helpers (also used by --select)
void DesignerClearSelection(Designer *d);
void DesignerSelectLine(Designer *d, int line);

// After LauncherSubmit: closes the box (and clears the text) on success, keeps it on failure
void DesignerSubmitDone(Designer *d, bool ok, const char *msg);

// Fills the request from the designer and the camera (prompt, hit, selection, view,
// regions: analysed here, so this takes as long as DesignerAnalyzeRegion per region)
void DesignerBuildRequest(const Designer *d, Camera3D cam, float yaw, float pitch,
                          int width, int height, DesignRequest *req);

// 3D overlay on top of the R3D frame (call after R3D_End, with the frame's target bound,
// i.e. inside BeginDrawing or BeginTextureMode). Depth-tested against R3D's depth, which
// R3D_End copies into the target along with the colour. hover: also outline the box under
// the crosshair (the live view); false for view-marked.png (selection + hit point only).
void DesignerDrawOverlay(const Designer *d, Camera3D cam, bool hover);

// 2D: crosshair at the centre of a w x h target
void DesignerDrawCrosshair(int w, int h);

// HUD lines (selection, crosshair target) at (x, y); returns the height used
int DesignerDrawHud(const Designer *d, int x, int y);

// Screen regions on a w x h target. marked = false: the live look (rest of the screen
// dimmed while snipping or while regions are attached, outlines, numbers, pixel sizes,
// the snip-mode help line); true: outlines + numbers only (view-marked.png)
void DesignerDrawRegions(const Designer *d, int w, int h, bool marked);

// Prompt box and the designer message (bottom of a w x h screen)
void DesignerDrawPrompt(Designer *d, int w, int h);

// --pick: prints the hit to stdout
void DesignerPrintHit(DesignerHit hit);

#endif
