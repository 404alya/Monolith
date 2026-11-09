// The transform modal of edit mode: G / ALT+R / S grab, rotate and scale the selection with the
// mouse or a typed value, and the rotate gizmo's drags; live (MS_EditPreview*), committed as one
// edit (transform.c; docs/live-editing.md 7, 11.10)
#ifndef MONOLITH_TRANSFORM_H
#define MONOLITH_TRANSFORM_H

#include "common.h"
#include "designer.h"
#include "mapedit.h"

typedef enum { TRANSFORM_NONE = 0, TRANSFORM_GRAB, TRANSFORM_ROTATE, TRANSFORM_SCALE } TransformKind;

// What drives a running modal (docs/live-editing.md 11.10): the keys (G / ALT+R / S, the mouse
// anywhere), a gizmo ring (the angle swept in its plane; the view ring: the dial) or the trackball
typedef enum { TRANSFORM_SRC_KEYS = 0, TRANSFORM_SRC_RING, TRANSFORM_SRC_BALL } TransformSource;

// A part of the rotate gizmo
typedef enum { GIZMO_NONE = 0, GIZMO_X, GIZMO_Y, GIZMO_Z, GIZMO_VIEW, GIZMO_BALL } GizmoPart;

typedef struct {
    TransformKind kind;             // NONE = not running
    TransformSource source;
    int ring;                       // SRC_RING: 0..2 the axis, 3 the view ring
    bool ringTrack;                 // SRC_RING: the ring was edge-on at the start: the turntable
    Vector3 ringU;                  // SRC_RING: where the arc starts (world, unit, in the ring's plane)
    Vector3 ballAxis;               // SRC_BALL: the axis of the last update (world, unit)
    bool dragging;                  // started by an LMB press on the gizmo, the button still held
    bool dragMoved;                 // ... and the cursor moved more than 4 px: the release commits
    Vector2 rawMouse;               // mouse delta since the start without the SHIFT slow-down
    bool scripted;                  // --edit-modal ring: scriptDeg stands for the dragged angle
    double scriptDeg;
    bool scriptCtrl;                // --edit-modal ... ctrl: CTRL held
    int axisMask;                   // locked axes: bit 0 x, 1 y, 2 z (0 = none; two bits = a plane lock)
    bool local;                     // locks along the active object's own axes (the same lock key twice)
    char typed[32];                 // the typed value ("" = the mouse decides); bounded input
    Vector2 mouse;                  // accumulated mouse delta since the start (px; SHIFT: x 0.1)
    Vector2 mouseStart;             // the cursor when the modal started (screen px)
    Vector3 pivot;                  // world
    Vector3 localAxes[3];           // the active object's axes (world; local locks use them)
    bool hasLocal;                  // a box, block or model is selected: the first one's axes are the local axes
    MS_ObjRef *objs;                // the selection at the start (heap)
    MS_Transform *start, *now;      // per object (heap)
    unsigned char *cls;             // per object: EditObjClass (heap)
    unsigned char *follows;         // per object: follows the current kind (1), refused / ignored (0) (heap)
    MS_ObjRef *commit;              // the objects that follow, for the commit (heap)
    int count, cap, commitCount;
    bool previewing;                // MS_EditPreviewBegin succeeded
    bool snapping;                  // CTRL was held at the last update
    float snapStep;                 // the N step then (metres)
    float angleStep;                // the angle snap step then (degrees; SHIFT+N)
    double value[3];                // grab: metres per world axis; rotate: [0] degrees; scale: factors
    Vector3 rotAxis;                // rotate: the axis (world; local: the model axis in model space)
    double sweep, lastAngle;        // rotate: the angle the cursor swept (radians), its last angle
    bool haveAngle;
    // What can't follow or why nothing changes (Classify, Compute: every frame), shown in red; its
    // words (refusal) are rebuilt only when these parts change (refusalShown)
    struct TransformRefusal {
        int kind;                       // the modal's kind (the verb)
        int other, dir, turn;           // boxes from stairs / waffle lines, directional lights, boxes refused a turn
        bool scaleBad;                  // a scale factor <= 0
        unsigned char note;             // nothing changes: 0 no, 1 one box looks the same turned, 2 the rest
        bool noteBoxes;                 // 2: boxes among them (rotate)
        double deg, sx, sz;             // 1: the turn and the box's footprint
        bool square;
    } refusalParts, refusalShown;
    MonoMsg refusal;
    bool refused;                   // refusal has words
    bool noChange;                  // a value, but every object that follows ends as it started
                                    // (a square box turned 90 deg): LMB / ENTER write nothing
    unsigned char cancelWhy;        // why the last commit attempt wrote nothing (TransformCancelNote):
                                    // 0 a plain cancel, 1 turned with no change, 2 every object would
                                    // end where it started, 3 nothing selected can follow
    Camera3D cam;                   // the camera of the last update (the commit's view axis)
    // The rotate gizmo while no modal runs (kept across modals)
    bool gizmoLocal;                // L: the rings follow the active object's own axes
    GizmoPart hover, forceHover;    // under the cursor; forced by --edit-modal "gizmo hover ..."
    unsigned char *gcls;            // the selection's classes (heap), recomputed when it changes
    int gcap;
    unsigned long long gkey;        // the selection and scene->version they belong to
    bool gcan;                      // the selection has a box, block or model
} TransformModal;

typedef enum {
    TRANSFORM_RUNNING = 0,          // nothing to do for the caller (or not running)
    TRANSFORM_COMMIT,               // confirmed: *op is the edit to apply (MapEditApply on TransformCommitObjects)
    TRANSFORM_CANCELLED,            // everything is back as it was
} TransformResult;

// Starts a modal of `kind` on the designer's selection. Returns 0 started, 1 nothing selected,
// 2 out of memory (the caller has the texts for the codes)
unsigned char TransformBegin(TransformModal *t, Designer *d, TransformKind kind);
// Every frame while running (takes every key and the mouse; the camera is frozen): on
// TRANSFORM_COMMIT *op holds the edit; the caller applies it to TransformCommitObjects and then
// calls TransformEnd (the commit's MS_EditSetText has ended the preview; a refused one is undone)
TransformResult TransformUpdate(TransformModal *t, Designer *d, Camera3D cam, EditOp *op);
bool TransformRunning(const TransformModal *t);
// After TRANSFORM_CANCELLED: why nothing was written when LMB / ENTER asked for a commit (a
// value that changes nothing: a static sentence for cancelWhy), NULL when it was a plain cancel
// (RMB / ESC, no value)
const char *TransformCancelNote(const TransformModal *t);
// The objects the commit's op applies to (those that follow the current kind)
int TransformCommitObjects(const TransformModal *t, const MS_ObjRef **objs);
// After a commit (applied or refused) or to abort: the preview ends if it still runs
void TransformEnd(TransformModal *t, Designer *d);
// --edit-modal "g x 1.25": opens the modal with this lock and typed text, values computed for
// cam (for --shot-hud). Lock words: x y z (axis), X Y Z (plane), doubled (xx, XX) = local axes.
// Returns TransformBegin's values, or 3: the spec is not `<g|r|s> ...` (the caller quotes it)
unsigned char TransformOpenScripted(TransformModal *t, Designer *d, Camera3D cam, const char *spec);
// The scripted modal's end (--edit-modal ... commit / cancel): commit fills *op (TRANSFORM_COMMIT,
// then as after TransformUpdate), cancel puts everything back (TRANSFORM_CANCELLED)
TransformResult TransformFinish(TransformModal *t, Designer *d, bool commit, EditOp *op);
// The rotate gizmo (docs/live-editing.md 11.10). It shows while no modal runs (or a gizmo drag
// does) and the selection has a box, block or model; drawn in 2D after R3D_End at a constant size
// TransformGizmoHover: what is under the cursor px (enabled false: nothing, or the forced part)
GizmoPart TransformGizmoHover(TransformModal *t, Designer *d, Camera3D cam, Vector2 px, bool enabled);
// LMB on a gizmo part: a rotate modal driven by it (the drag). Returns TransformBegin's values, or 3:
// nothing of the gizmo there (part GIZMO_NONE)
unsigned char TransformGizmoBegin(TransformModal *t, Designer *d, Camera3D cam, GizmoPart part);
// --edit-modal "ring <x|y|z|xx|yy|zz|view> <deg> ..." / "ball <dx> <dy> ...": a gizmo drag of that
// much, values computed for cam; extra words: ctrl (CTRL held), step <deg> (the angle snap), type <v>.
// Returns TransformBegin's values, or 3: the spec is not a ring / ball spec (the caller quotes it)
unsigned char TransformGizmoScripted(TransformModal *t, Designer *d, Camera3D cam, const char *spec);
void TransformDrawGizmo(TransformModal *t, Designer *d, Camera3D cam);
// The axis guides (3D, after R3D_End like the designer overlay) and the HUD line; returns its height
void TransformDrawGuides(const TransformModal *t, Camera3D cam);
int TransformDrawHud(const TransformModal *t, int x, int y);
void TransformFree(TransformModal *t);

#endif
