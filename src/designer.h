// Designer: crosshair picking, selection and the prompt box in spectator mode
#ifndef MONOLITH_DESIGNER_H
#define MONOLITH_DESIGNER_H

#include "common.h"

// The selection, the screen regions, the prompt text and what a region shows are heap arrays
// (MS_RESERVE / MS_PUSH from runtime/monolith_scene.h): no count limits. Only sanity caps:
#define PROMPT_LIMIT    (1024 * 1024)   // bytes of prompt text (a runaway paste stops here)
#define REGION_SAMPLES  4096    // rays per region at most (a regular grid, see DesignerAnalyzeRegion):
                                // the analysis cost, not a storage limit
#define VOLUME_SAMPLES  4096    // cells of a volume's empty-share grid at most (DesignerAnalyzeVolume)
#define VOLUME_MIN_SIZE 0.01f   // metres: a volume is at least this thick on every axis
#define VOLUME_SNAP_DEFAULT 0.5f    // metres: the volume snap step until N changes it

// Objects are MS_ObjRef (runtime/monolith_scene.h, .claude/skills/map-request/bundle-reference.md): a box
// (index into scene->boxes) or a model instance (index into scene->models).

// What the crosshair points at. obj.kind MS_OBJ_NONE = nothing hit (sky).
typedef struct {
    MS_ObjRef obj;
    Vector3 point;          // world-space hit point
    Vector3 normal;         // face normal (boxes: axis-aligned; models: the triangle's)
    float dist;             // from the eye, metres
} DesignerHit;

static inline bool ObjIsNone(MS_ObjRef o) { return o.kind == MS_OBJ_NONE; }
static inline bool ObjEqual(MS_ObjRef a, MS_ObjRef b) { return a.kind == b.kind && a.index == b.index; }

// A screen region (snip mode): pixels of the frame the request is made from (= view.png),
// top-left origin. Tied to the camera frozen while it was drawn.
typedef struct { Rectangle rect; } DesignRegion;

typedef struct {
    MS_ObjRef obj;
    int samples;            // rays of the region that hit this object first
    float share;            // samples / all samples of the region
} DesignRegionObject;

// Edge sides of a region (DesignRegionEdge.sides)
enum { REGION_EDGE_LEFT = 1, REGION_EDGE_RIGHT = 2, REGION_EDGE_TOP = 4, REGION_EDGE_BOTTOM = 8 };

// An object that rays just outside a region's border hit: what bounds an opening or a gap
// the region looks through (the region's own objects are what is seen through it)
typedef struct {
    MS_ObjRef obj;
    int samples;            // edge rays that hit this object first
    unsigned int sides;     // REGION_EDGE_* bits: on which sides of the rect
    float dist;             // nearest hit, metres from the eye
    Vector3 point;          // that nearest hit point
} DesignRegionEdge;

// A volume (V in spectator mode, --volume): an axis-aligned box in world space (map
// coordinates). World-space, so a map reload leaves it alone; an editor overlay, never
// written to the map.
typedef struct { BoundingBox box; } DesignVolume;

// An object whose world bounds overlap a volume (models count by their world AABB)
typedef struct {
    MS_ObjRef obj;
    float shareOfVolume;    // overlap of the object's bounds with the volume / the volume
    float shareOfObject;    // the same overlap / the object's bounds (flat axes count as 1)
    bool fullyInside;       // the object's bounds lie inside the volume
} DesignVolumeObject;

// What a volume holds, computed at submit time by DesignerAnalyzeVolume
typedef struct {
    BoundingBox box;
    float snap;             // the snap step when it was submitted (metres)
    DesignVolumeObject *objects;    // by shareOfVolume descending (heap; DesignerFreeRequest)
    int objectCount;
    float empty;            // share of the grid cells whose centre is in no object's bounds
    int grid[3];            // that grid (x, y, z cells; product <= VOLUME_SAMPLES)
    DesignerHit floor;      // first surface straight down from the volume's centre (none: obj.kind NONE)
    double ms;              // time the analysis took
} DesignVolumeInfo;

// What a region shows, computed at submit time by DesignerAnalyzeRegion (ray sampling)
typedef struct {
    Rectangle rect;         // clamped to the frame, whole pixels
    int gridW, gridH;       // sample grid (gridW * gridH <= REGION_SAMPLES)
    MS_ObjRef centerObj;    // under the rect's centre; kind MS_OBJ_NONE = sky
    Vector3 centerPoint, centerNormal;
    float centerDist;       // from the eye, metres
    bool hasDepth;          // any sample hit something
    float depthMin, depthMax;   // distance from the eye over the samples that hit, metres
    DesignRegionObject *objects;    // every object hit, by samples descending (heap; DesignerFreeRequest)
    int objectCount;
    int omittedObjects;     // objects hit but not listed: always 0 now (kept for the bundle format)
    float omittedShare;
    float sky;              // share of samples that hit nothing
    // Edge objects: rays on a ring edgeOffset px outside the rect (sides that leave the frame
    // are skipped), nearest first (heap; DesignerFreeRequest)
    DesignRegionEdge *edges;
    int edgeCount;
    int edgeSamples;        // rays on the ring
    float edgeSky;          // share of them that hit nothing
    float edgeOffset;       // px outside the rect
    double ms;              // time the analysis took
} DesignRegionInfo;

// One request as it is submitted: a snapshot of everything the launcher writes into the
// bundle. Object indices refer to the scene at submit time (scene->version tells which load).
// Its arrays are heap copies: DesignerFreeRequest releases them.
typedef struct {
    char *prompt;
    Vector3 eye;
    float yaw, pitch;       // radians, project convention (yaw 0 looks toward +Z)
    float fovy;             // degrees
    int width, height;      // render size of view.png
    DesignerHit hit;
    MS_ObjRef *selected;
    int selectedCount;
    DesignRegionInfo *regions;
    int regionCount;
    DesignVolumeInfo *volumes;
    int volumeCount;
    unsigned int mapVersion;
} DesignRequest;

// ======================================================================================
// Designer state (designer.c): one instance, owned by main()
// ======================================================================================

// What a selected box was, so the selection survives a map hot reload (see DesignerSync)
typedef struct {
    MS_ObjKind kind;
    BoundingBox box;        // world AABB (boxes: the box; models: their bounds)
    int line;               // map line of the object
    int nth;                // index among the objects of that kind on that line
    int group;              // group index at selection time (compared by name after a reload)
    char *groupName;        // heap copy, NULL = no group
} DesignerSelKey;

typedef struct {
    bool interactive;       // a visible window: may show/hide the mouse cursor

    DesignerHit hit;        // under the crosshair (refreshed by DesignerPick / DesignerUpdate)

    MS_ObjRef *selected;    // boxes and models, no duplicates (heap)
    DesignerSelKey *keys;   // parallel to selected[]
    int selectedCount, selectedCap, keyCap;
    int *selPos[3];         // per kind (MS_OBJ_BOX, MS_OBJ_MODEL): object index -> position in
    int selPosCap[3];       // selected[] + 1, 0 = not selected (O(1) membership for big selections)
    unsigned int version;   // scene->version the indices refer to

    // Snip mode (C): screen regions dragged out over a frozen camera. Regions are
    // screen-space, so a map reload leaves them alone (what they show is computed at submit).
    bool snipping;          // snip mode on: camera frozen, cursor free, LMB drags regions
    DesignRegion *regions;  // heap
    int regionCount, regionCap;
    bool dragging;
    bool dragAdd;           // Shift was down at the drag start: add instead of replace
    Vector2 dragStart, dragEnd;

    // Volume mode (V): world-space boxes made from crosshair clicks. The camera keeps
    // flying (except while the height is set); the arrows edit the active volume.
    bool volumeMode;
    int volumeStep;         // VOLUME_STEP_*: what the next click or the mouse does
    Vector3 volA, volB;     // snapped corner clicks of the volume being made
    float volBase;          // its bottom (y)
    float volHeight;        // its height before snapping (the mouse moves this)
    DesignVolume *volumes;  // heap
    int volumeCount, volumeCap;
    int activeVolume;       // index into volumes[] (edited by the arrows), -1 = none
    float volumeSnap;       // metres; 0 = VOLUME_SNAP_DEFAULT
    Vector3 eye, viewFwd;   // the camera of the last DesignerPickCamera (arrow directions)

    // Prompt box
    bool promptOpen;
    char *prompt;           // heap, NUL-terminated once anything was typed (NULL = empty)
    int promptLen;          // bytes, excluding the terminating 0
    int promptCap;
    int cursor;             // byte offset, always on a UTF-8 codepoint boundary
    float wantX;            // Up/Down keep this x (pixels); < 0 = take it from the cursor
    int scroll;             // first visible wrapped line
    int freezeFrames;       // camera stays frozen this many more frames after the box closes

    char message[256];      // HUD message (submit result, selection notes)
    float messageTime;
    bool messageError;
} Designer;

enum { VOLUME_STEP_FIRST = 0, VOLUME_STEP_SECOND, VOLUME_STEP_HEIGHT };

typedef enum {
    DESIGNER_NONE = 0,
    DESIGNER_SUBMIT,        // Enter in the prompt box: main() renders the images and calls LauncherSubmit
} DesignerAction;

// Casts a ray against scene->boxes (slab test) and models (MS_RaycastModel); obj.kind
// MS_OBJ_NONE when nothing is hit. Same result as MS_Raycast. Boxes that contain the ray
// origin are ignored (you can pick out of a box you're inside).
DesignerHit DesignerPick(Vector3 origin, Vector3 dir);

// Call once per frame before anything reads selected[]: after a map reload it remaps the
// selection onto the new world (same geometry anywhere, else same map line + group).
void DesignerSync(Designer *d);

// d->hit = what the camera's centre ray hits. Call after the camera moved, before drawing.
void DesignerPickCamera(Designer *d, Camera3D cam);
// What the ray through pixel (x, y) of a w x h frame of `cam` hits (top-left origin, pixel
// centres at i + 0.5; the frame centre is the crosshair ray): --pick-at
DesignerHit DesignerPickAt(Camera3D cam, int w, int h, float x, float y);

// Spectator-mode input: clicks (selection, against d->hit, i.e. what the last frame
// showed under the crosshair), Enter (opens the prompt box) and all prompt editing.
// Call only in spectator mode, before the camera update.
DesignerAction DesignerUpdate(Designer *d, float dt);

// True while the prompt box is open, snip mode is on, a volume's height is being set, or
// the camera is still frozen after one of them: the caller must skip game keys and camera
// movement then
bool DesignerCapturesInput(const Designer *d);
// True in volume mode: the arrow keys edit the active volume, so the spectator must not
// use them for viewpoints
bool DesignerTakesArrows(const Designer *d);

// Snip mode. Enter: freezes the camera, frees the cursor. Leave: drops the regions.
void DesignerSnipBegin(Designer *d);
void DesignerSnipEnd(Designer *d);
// Adds a region (pixels, clamped to a w x h frame; --region). False if empty (or out of memory).
bool DesignerAddRegion(Designer *d, Rectangle rect, int w, int h);

// Adds a volume (any two opposite corners; grown to VOLUME_MIN_SIZE per axis) and makes it
// the active one. False if a corner is not finite (or out of memory). --volume, Shift+V.
bool DesignerAddVolume(Designer *d, BoundingBox box);
// What volume `box` holds: overlapping objects, the empty share, the floor under it.
// out->objects is heap memory (DesignerFreeVolume, or DesignerFreeRequest).
void DesignerAnalyzeVolume(BoundingBox box, float snap, DesignVolumeInfo *out);
void DesignerFreeVolume(DesignVolumeInfo *v);

// What region `rect` (pixels of a w x h frame rendered with `cam`) shows: a regular grid of
// at most REGION_SAMPLES rays through pixel centres, plus one through the rect's centre.
// Also the edge objects: rays on a ring just outside the rect (out->edges).
// out->objects and out->edges are heap memory (DesignerFreeRegion, or DesignerFreeRequest).
void DesignerFreeRegion(DesignRegionInfo *r);
void DesignerAnalyzeRegion(Camera3D cam, int w, int h, Rectangle rect, DesignRegionInfo *out);

void DesignerOpenPrompt(Designer *d, const char *text);
void DesignerMessage(Designer *d, bool error, const char *text);

// Selection helpers (also used by --select)
void DesignerClearSelection(Designer *d);
void DesignerSelectLine(Designer *d, int line);
// Adds one box or model to the selection (the edit mode's selection after an edit)
bool DesignerSelectObject(Designer *d, MS_ObjRef o);

// The snap step shared by volume mode and the edit mode (metres), and N's cycle through
// 0.1 / 0.25 / 0.5 / 1 / 2 m (returns the new step)
float DesignerSnap(const Designer *d);
float DesignerCycleSnap(Designer *d);
// Leaves volume mode (cancels a volume being made)
void DesignerVolumeModeEnd(Designer *d);

// After LauncherSubmit: closes the box (and clears the text) on success, keeps it on failure
void DesignerSubmitDone(Designer *d, bool ok, const char *msg);

// Fills the request from the designer and the camera (prompt, hit, selection, view,
// regions: analysed here, so this takes as long as DesignerAnalyzeRegion per region).
// False when out of memory (req is then empty). Release it with DesignerFreeRequest.
bool DesignerBuildRequest(const Designer *d, Camera3D cam, float yaw, float pitch,
                          int width, int height, DesignRequest *req);
void DesignerFreeRequest(DesignRequest *req);

// Index of an object among the objects of its kind made by its map line (the copy number
// of a `repeat`); objects of one line are consecutive, so this walks back over that line only
int DesignerObjNth(MS_ObjRef o);

// Material label of an object: its material, or for models "(own)" / "(own; slot N = name, ...)"
const char *DesignerMaterialLabel(MS_ObjRef o);

// Frees everything the designer allocated (selection, regions, prompt, scratch buffers)
void DesignerFree(Designer *d);

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

// Volumes on a w x h target rendered with `cam` (the frame's target bound, after R3D_End,
// like DesignerDrawOverlay): wireframes (dim where hidden), numbers. marked = false: the
// live look (the volume being made, the active one brighter with its size, the volume-mode
// help line); true: wireframes + numbers only (view-marked.png). Nothing without volumes.
void DesignerDrawVolumes(const Designer *d, Camera3D cam, int w, int h, bool marked);

// Prompt box and the designer message (bottom of a w x h screen)
void DesignerDrawPrompt(Designer *d, int w, int h);

// --pick: prints the hit to stdout
void DesignerPrintHit(DesignerHit hit);

#endif
