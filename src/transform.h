// The transform modal of edit mode: G / R / S grab, rotate and scale the selection with the
// mouse or a typed value, live (MS_EditPreview*), committed as one edit (transform.c;
// docs/live-editing.md 7)
#ifndef MONOLITH_TRANSFORM_H
#define MONOLITH_TRANSFORM_H

#include "common.h"
#include "designer.h"
#include "mapedit.h"

typedef enum { TRANSFORM_NONE = 0, TRANSFORM_GRAB, TRANSFORM_ROTATE, TRANSFORM_SCALE } TransformKind;

typedef struct {
    TransformKind kind;             // NONE = not running
    int axisMask;                   // locked axes: bit 0 x, 1 y, 2 z (0 = none; two bits = a plane lock)
    bool local;                     // locks along the models' own axes (the same lock key twice)
    char typed[32];                 // the typed value ("" = the mouse decides); bounded input
    Vector2 mouse;                  // accumulated mouse delta since the start (px; SHIFT: x 0.1)
    Vector2 mouseStart;             // the cursor when the modal started (screen px)
    Vector3 pivot;                  // world
    Vector3 localAxes[3];           // the first selected model's axes (world; local locks use them)
    bool hasModel;                  // a model is selected (local locks exist)
    MS_ObjRef *objs;                // the selection at the start (heap)
    MS_Transform *start, *now;      // per object (heap)
    unsigned char *cls;             // per object: EditObjClass (heap)
    unsigned char *follows;         // per object: follows the current kind (1), refused / ignored (0) (heap)
    MS_ObjRef *commit;              // the objects that follow, for the commit (heap)
    int count, cap, commitCount;
    bool previewing;                // MS_EditPreviewBegin succeeded
    bool boxes90;                   // rotate: boxes follow, so the angle snaps to 90 degrees
    bool snapping;                  // CTRL was held at the last update
    float snapStep;                 // the N step then (metres)
    double value[3];                // grab: metres per world axis; rotate: [0] degrees; scale: factors
    Vector3 rotAxis;                // rotate: the axis (world; local: the model axis in model space)
    double sweep, lastAngle;        // rotate: the angle the cursor swept (radians), its last screen angle
    bool haveAngle;
    double rawDeg;                  // rotate: the angle before the boxes' 90 degree snap (HUD)
    char refusal[320];              // what can't follow (e.g. "stairs can't turn"), shown in red
    bool noChange;                  // a value, but every object that follows ends as it started
                                    // (a square box turned 90 deg): LMB / ENTER write nothing
    char cancelNote[200];           // why the last commit attempt wrote nothing ("" = cancelled)
    Camera3D cam;                   // the camera of the last update (the commit's view axis)
} TransformModal;

typedef enum {
    TRANSFORM_RUNNING = 0,          // nothing to do for the caller (or not running)
    TRANSFORM_COMMIT,               // confirmed: *op is the edit to apply (MapEditApply on TransformCommitObjects)
    TRANSFORM_CANCELLED,            // everything is back as it was
} TransformResult;

// Starts a modal of `kind` on the designer's selection (false: nothing selected / out of memory, msg)
bool TransformBegin(TransformModal *t, Designer *d, TransformKind kind, char *msg, int msgSize);
// Every frame while running (takes every key and the mouse; the camera is frozen): on
// TRANSFORM_COMMIT *op holds the edit; the caller applies it to TransformCommitObjects and then
// calls TransformEnd (the commit's MS_EditSetText has ended the preview; a refused one is undone)
TransformResult TransformUpdate(TransformModal *t, Designer *d, Camera3D cam, EditOp *op);
bool TransformRunning(const TransformModal *t);
// After TRANSFORM_CANCELLED: why nothing was written when LMB / ENTER asked for a commit (a
// value that changes nothing), NULL when it was a plain cancel (RMB / ESC, no value)
const char *TransformCancelNote(const TransformModal *t);
// The objects the commit's op applies to (those that follow the current kind)
int TransformCommitObjects(const TransformModal *t, const MS_ObjRef **objs);
// After a commit (applied or refused) or to abort: the preview ends if it still runs
void TransformEnd(TransformModal *t, Designer *d);
// --edit-modal "g x 1.25": opens the modal with this lock and typed text, values computed for
// cam (for --shot-hud). Lock words: x y z (axis), X Y Z (plane), doubled (xx, XX) = local axes
bool TransformOpenScripted(TransformModal *t, Designer *d, Camera3D cam, const char *spec, char *msg, int msgSize);
// The scripted modal's end (--edit-modal ... commit / cancel): commit fills *op (TRANSFORM_COMMIT,
// then as after TransformUpdate), cancel puts everything back (TRANSFORM_CANCELLED)
TransformResult TransformFinish(TransformModal *t, Designer *d, bool commit, EditOp *op);
// The axis guides (3D, after R3D_End like the designer overlay) and the HUD line; returns its height
void TransformDrawGuides(const TransformModal *t, Camera3D cam);
int TransformDrawHud(const TransformModal *t, int x, int y);
void TransformFree(TransformModal *t);

#endif
