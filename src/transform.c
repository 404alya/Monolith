// transform -- the edit mode's transform modal: G / ALT+R / S grab, rotate and scale the selection,
// and the rotate gizmo (docs/live-editing.md 7, 11.10)
//
// G / ALT+R / S in edit mode start it on the selection; the camera is frozen and the cursor hidden,
// and the raw mouse motion (SHIFT: ten times slower) drives the value every frame:
//   grab     the cursor's motion on the plane through the pivot facing the camera, on the locked
//            axis (closest point between the cursor's ray and the axis line) or on the locked plane
//   rotate   about the view axis, or the locked axis (a plane lock: the axis normal to it). An
//            axis across the view (a vertical one seen from eye height) follows the motion along
//            the screen direction the near side moves in (0.5 deg / px: drag right, the side facing
//            you goes right); an axis pointing at the camera takes the angle the cursor sweeps
//            around the pivot's screen position (or the horizontal motion when the cursor starts
//            near the pivot). Boxes turn about any axis as far as MapEditPreviewTransform accepts
//            the turn (what it refuses is named in red and stays). A value that changes nothing (a
//            square box turned 90 degrees about its centre, any box turned 180) is named in red and
//            LMB / ENTER write nothing
//   scale    the ratio of the cursor's distance to the pivot's screen position now and at the
//            start: uniform, one axis (axis lock) or two (plane lock)
// X / Y / Z lock an axis, SHIFT+X / Y / Z a plane (the other two axes); the same key again uses
// the active object's own axes (local: the first selected box, block or model), a third time
// unlocks. Digits, '.', '-' (toggles the sign) and BACKSPACE type the value (metres along the
// locked axis or the view's right, degrees, the factor); typed values are exact. CTRL held snaps:
// the pivot onto the N grid, the angle step (SHIFT+N: 1 / 5 / 15 / 45 / 90 degrees), factors to
// 0.1. G / R / S switch kind (from the original transforms, the lock and the typed value
// cleared). LMB / ENTER commit one edit (one EditOp through MapEditApply: one undo step); RMB /
// ESC cancel (MS_EditPreviewEnd: everything back exactly).
//
// The rotate gizmo (shown while no modal runs and the selection has a box, block or model; 2D,
// a constant 120 px): three axis rings (their front halves; global axes or, with L, the active
// object's), a white view ring at 1.15 x and a grey trackball disc. LMB on a part starts the same
// modal with a source: a ring turns by the angle the cursor sweeps in its plane (edge-on, under 15
// degrees: the turntable), the view ring is the dial, the trackball turns about the axis
// perpendicular to the drag on screen (0.5 deg / px, one axis-angle EDIT_ROTATE). A drag (moved
// more than 4 px) commits on release unless a value was typed; a click, or a drag with typed
// text, stays open as the modal (the mouse still turns; LMB / ENTER commit, RMB / ESC cancel).
//
// The pivot is the centre of the selection's world bounds (rounded to 0.1 mm), or one model's
// origin when it is the whole selection. Each frame the op is built as for the commit and every
// object's transform comes from MapEditPreviewTransform (the same math mapedit.c writes with), so
// the preview shows what the commit writes. Objects that can't follow (stairs and waffles turned
// or scaled, turns MapEditPreviewTransform refuses, directional lights moved) keep their transform
// and are named in red in the HUD; lights ignore rotate and scale. Mouse values are rounded to
// what the HUD shows (mm, 0.01 degree, 0.001) before they are written.
#include "transform.h"
#include "ui.h"
#include <math.h>

#define COLOR_X     (Color){ 235, 80, 80, 255 }
#define COLOR_Y     (Color){ 120, 215, 90, 255 }
#define COLOR_Z     (Color){ 80, 145, 255, 255 }
#define MODAL_COLOR (Color){ 255, 214, 90, 255 }
#define SCALE_NEAR_PX   30.0f   // closer to the pivot than this at the start: the ratio is eased
// Rotate, two ways to read the mouse:
//   - the axis lies across the view (more than ROT_TRACK_MIN_SIN off the line of sight: a
//     vertical axis seen from eye height): the turn follows the motion along the direction the
//     object's near side moves on screen (a turntable: drag right, the side facing you goes
//     right), ROT_DEG_PER_PX, wherever the cursor starts. The screen angle around the pivot says
//     little here (a horizontal drag below a tall pillar's pivot sweeps a few degrees);
//   - the axis points at the camera (the view axis, or looking along a locked axis): a start this
//     far from the pivot's screen position turns by the angle the cursor sweeps around it
//     (Blender's dial); nearer, the sweep is tiny, so the horizontal motion turns it (right =
//     clockwise on screen)
#define ROT_DIAL_PX     80.0f
#define ROT_DEG_PER_PX  0.5
#define ROT_TRACK_MIN_SIN 0.5f      // sin of 30 degrees between the axis and the line of sight
// The rotate gizmo (docs/live-editing.md 11.10, 11.11)
#define GIZMO_RADIUS_PX     120.0f  // the axis rings and the trackball, constant on screen
#define GIZMO_VIEW_SCALE    1.15f   // the view ring's radius / GIZMO_RADIUS_PX
#define GIZMO_HIT_PX        8.0f    // hover tolerance of a ring
#define GIZMO_SEGMENTS      72      // per ring (drawing and hit test)
#define GIZMO_FRONT_MIN     -0.05f  // a ring segment is drawn when its direction . toCam is above this
                                    // (the front half; a ring facing the camera: all of it)
#define GIZMO_EDGE_ON_SIN   0.258819f   // sin 15 deg: a ring seen flatter than this is edge-on (turntable)
#define DRAG_MIN_PX         4.0f    // an LMB press on the gizmo moved further than this is a drag
#define GIZMO_BALL_COLOR    (Color){ 170, 170, 176, 255 }

static const char AXIS_NAME[3] = { 'X', 'Y', 'Z' };

static bool Ctrl(void) { return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL); }
static bool Shift(void) { return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }

static Vector3 Unit(int a) { return a == 0 ? (Vector3){ 1, 0, 0 } : a == 1 ? (Vector3){ 0, 1, 0 } : (Vector3){ 0, 0, 1 }; }

static int BitCount(int m) { return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1); }

// The single locked axis (an axis lock, or the axis normal to a locked plane); -1 = none
static int LockedAxis(const TransformModal *t)
{
    if (BitCount(t->axisMask) == 1) return t->axisMask == 1 ? 0 : t->axisMask == 2 ? 1 : 2;
    if (BitCount(t->axisMask) == 2) return (t->axisMask & 1) == 0 ? 0 : (t->axisMask & 2) == 0 ? 1 : 2;
    return -1;
}

// Axis a of the lock in the world: the active object's own (local) or the world's
static Vector3 LockDir(const TransformModal *t, int a)
{
    return t->local && t->hasLocal ? t->localAxes[a] : Unit(a);
}

static double Round(double v, double step) { return round(v / step) * step; }

bool TransformRunning(const TransformModal *t) { return t->kind != TRANSFORM_NONE; }

const char *TransformCancelNote(const TransformModal *t) { return t->cancelNote[0] ? t->cancelNote : NULL; }

int TransformCommitObjects(const TransformModal *t, const MS_ObjRef **objs)
{
    *objs = t->commit;
    return t->commitCount;
}

void TransformFree(TransformModal *t)
{
    free(t->objs);
    free(t->start);
    free(t->now);
    free(t->cls);
    free(t->follows);
    free(t->commit);
    free(t->gcls);
    memset(t, 0, sizeof(*t));
}

// ======================================================================================
// #region The selection: pivot, local axes
// ======================================================================================

// The pivot of objs[] (cls[]: their EditObjClass): one model alone: its origin (rounded to 0.1
// mm, the written decimal); else the centre of the world bounds of everything but directional
// lights, rounded to 0.1 mm
static Vector3 SelectionPivot(const MS_ObjRef *objs, const unsigned char *cls, int n)
{
    if (n == 1 && objs[0].kind == MS_OBJ_MODEL) {
        double p[3];
        MapEditAnchor(objs[0], p);
        return (Vector3){ (float)p[0], (float)p[1], (float)p[2] };
    }
    BoundingBox bb = { { INFINITY, INFINITY, INFINITY }, { -INFINITY, -INFINITY, -INFINITY } };
    for (int i = 0; i < n; i++) {
        if (cls[i] == EDIT_CLASS_DIR_LIGHT || MS_ObjectEntity(scene, objs[i]) < 0) continue;
        BoundingBox b = MS_ObjectBounds(scene, objs[i]);
        bb.min = Vector3Min(bb.min, b.min);
        bb.max = Vector3Max(bb.max, b.max);
    }
    if (bb.min.x <= bb.max.x) {
        Vector3 c = Vector3Scale(Vector3Add(bb.min, bb.max), 0.5f);
        return (Vector3){ (float)Round(c.x, 1e-4), (float)Round(c.y, 1e-4), (float)Round(c.z, 1e-4) };
    }
    int e = n > 0 ? MS_ObjectEntity(scene, objs[0]) : -1;
    return e >= 0 ? scene->entityTransforms[e].position : (Vector3){ 0 };
}

// The active object's axes (R = Ry Rx Rz of its rotationDeg, as the runtime turns it): the first
// box, block or model of objs[]; false (and the world axes) when there is none
static bool ActiveAxes(const MS_ObjRef *objs, const unsigned char *cls, int n, Vector3 axes[3])
{
    for (int a = 0; a < 3; a++) axes[a] = Unit(a);
    for (int i = 0; i < n; i++) {
        int c = cls[i], e = MS_ObjectEntity(scene, objs[i]);
        if ((c != EDIT_CLASS_BOX && c != EDIT_CLASS_BLOCK && c != EDIT_CLASS_MODEL) || e < 0) continue;
        Vector3 r = scene->entityTransforms[e].rotationDeg;
        Quaternion q = QuaternionMultiply(QuaternionMultiply(QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, r.x * DEG2RAD),
                                                             QuaternionFromAxisAngle((Vector3){ 1, 0, 0 }, r.y * DEG2RAD)),
                                          QuaternionFromAxisAngle((Vector3){ 0, 0, 1 }, r.z * DEG2RAD));
        for (int a = 0; a < 3; a++) axes[a] = Vector3RotateByQuaternion(Unit(a), q);
        return true;
    }
    return false;
}

static bool CanTurn(int c) { return c == EDIT_CLASS_BOX || c == EDIT_CLASS_BLOCK || c == EDIT_CLASS_MODEL; }

// The designer's selection classified (cached until the selection or the scene changes): true
// when it has a box, block or model (the gizmo shows)
static bool GizmoSelection(TransformModal *t, const Designer *d)
{
    int n = d->selectedCount;
    if (n == 0) return false;
    unsigned long long h = 1469598103934665603ULL;
    const unsigned long long words[4] = { (unsigned long long)(uintptr_t)scene, scene->version, scene->loadVersion, (unsigned long long)n };
    for (int k = 0; k < 4; k++) h = (h ^ words[k]) * 1099511628211ULL;
    for (int i = 0; i < n; i++) h = (h ^ ((unsigned long long)d->selected[i].kind << 32 ^ (unsigned int)d->selected[i].index)) * 1099511628211ULL;
    if (h == t->gkey && t->gcap >= n) return t->gcan;
    if (n > t->gcap) {
        unsigned char *c = realloc(t->gcls, (size_t)n);
        if (!c) { t->gkey = 0; return false; }
        t->gcls = c;
        t->gcap = n;
    }
    MapEditClassify(d->selected, n, t->gcls);
    t->gcan = false;
    for (int i = 0; i < n; i++) {
        if (MS_ObjectEntity(scene, d->selected[i]) < 0) t->gcls[i] = EDIT_CLASS_NONE;
        t->gcan = t->gcan || CanTurn(t->gcls[i]);
    }
    t->gkey = h;
    return t->gcan;
}

// #endregion

// ======================================================================================
// #region Values
// ======================================================================================

// The typed number (false: nothing typed that is a number; *neg: a lone '-')
static bool TypedValue(const TransformModal *t, double *v, bool *neg)
{
    *neg = t->typed[0] == '-';
    char *end;
    double x = strtod(t->typed, &end);
    if (end == t->typed || !isfinite(x)) return false;
    *v = x;
    return true;
}

static Vector2 Cursor(const TransformModal *t) { return Vector2Add(t->mouseStart, t->mouse); }

static Ray CursorRay(Vector2 px, Camera3D cam)
{
    return GetScreenToWorldRayEx(px, cam, GetScreenWidth(), GetScreenHeight());
}

// Where the ray meets the plane through p with normal n (false: parallel, or behind)
static bool RayPlane(Ray r, Vector3 p, Vector3 n, Vector3 *hit)
{
    float dn = Vector3DotProduct(r.direction, n);
    if (fabsf(dn) < 1e-5f) return false;
    float u = Vector3DotProduct(Vector3Subtract(p, r.position), n) / dn;
    if (u < 0.0f) return false;
    *hit = Vector3Add(r.position, Vector3Scale(r.direction, u));
    return true;
}

// The parameter s of the point p + s a closest to the ray (false: the ray runs along the axis)
static bool RayAxis(Ray r, Vector3 p, Vector3 a, double *s)
{
    double b = Vector3DotProduct(a, r.direction);
    double denom = 1.0 - b * b;
    if (denom < 1e-6) return false;
    Vector3 w0 = Vector3Subtract(p, r.position);
    double dd = Vector3DotProduct(a, w0), e = Vector3DotProduct(r.direction, w0);
    *s = (b * e - dd) / denom;
    return true;
}

static Vector3 ViewForward(Camera3D cam) { return Vector3Normalize(Vector3Subtract(cam.target, cam.position)); }

// The camera's right and up (unit; up perpendicular to the view)
static void CamBasis(Camera3D cam, Vector3 *right, Vector3 *up)
{
    Vector3 fwd = ViewForward(cam);
    *right = Vector3Normalize(Vector3CrossProduct(fwd, cam.up));
    *up = Vector3CrossProduct(*right, fwd);
}

static void GrabValue(TransformModal *t, Camera3D cam, float snap, bool ctrl)
{
    Vector3 P = t->pivot, fwd = ViewForward(cam);
    double v;
    bool neg;
    int n = BitCount(t->axisMask);
    bool local = t->local && t->hasLocal;
    double delta[3] = { t->value[0], t->value[1], t->value[2] };    // (kept when the ray can't tell)
    if (TypedValue(t, &v, &neg)) {
        // Metres along the locked axis (a plane: its first axis), no lock: the view's right
        Vector3 dir = Vector3Normalize(Vector3CrossProduct(fwd, (Vector3){ 0, 1, 0 }));
        for (int a = 0; a < 3 && n > 0; a++)
            if (t->axisMask & (1 << a)) { dir = LockDir(t, a); break; }
        for (int a = 0; a < 3; a++) t->value[a] = round(((const float *)&dir)[a] * v * 1e6) / 1e6;
        return;
    }
    Ray r0 = CursorRay(t->mouseStart, cam), r1 = CursorRay(Cursor(t), cam);
    if (n == 1) {
        int a = LockedAxis(t);
        Vector3 A = LockDir(t, a);
        double s0, s1;
        if (RayAxis(r0, P, A, &s0) && RayAxis(r1, P, A, &s1)) {
            double s = s1 - s0;
            if (neg) s = -s;
            if (ctrl) {
                if (local) s = Round(s, snap);
                else s = Round(((const float *)&P)[a] + s, snap) - ((const float *)&P)[a];
            } else s = Round(s, 1e-3);
            for (int k = 0; k < 3; k++) delta[k] = ((const float *)&A)[k] * s;
        }
    } else {
        Vector3 N = n == 2 ? LockDir(t, LockedAxis(t)) : fwd, h0, h1;
        if (RayPlane(r0, P, N, &h0) && RayPlane(r1, P, N, &h1)) {
            Vector3 d = Vector3Subtract(h1, h0);
            if (neg) d = Vector3Negate(d);
            for (int k = 0; k < 3; k++) delta[k] = Round(((const float *)&d)[k], 1e-3);
            if (ctrl && n == 2 && local) {
                // the amounts along the two local axes, each snapped
                Vector3 sum = { 0 };
                for (int a = 0; a < 3; a++)
                    if (t->axisMask & (1 << a)) {
                        Vector3 A = t->localAxes[a];
                        sum = Vector3Add(sum, Vector3Scale(A, (float)Round(Vector3DotProduct(d, A), snap)));
                    }
                for (int k = 0; k < 3; k++) delta[k] = ((const float *)&sum)[k];
            } else if (ctrl) {
                // the pivot onto the grid along the axes the motion lies in
                for (int k = 0; k < 3; k++) {
                    bool in = n == 2 ? (t->axisMask & (1 << k)) != 0 : fabsf(((const float *)&N)[k]) < 0.99f;
                    float pk = ((const float *)&P)[k];
                    delta[k] = in ? Round(pk + ((const float *)&d)[k], snap) - pk : 0.0;
                }
            }
        }
    }
    for (int k = 0; k < 3; k++) t->value[k] = round(delta[k] * 1e6) / 1e6;
}

// Screen angle of px around the pivot's screen position (counter-clockwise positive), false
// when px is too close to it to tell
static bool ScreenAngle(Vector2 px, Vector2 ps, double *a)
{
    double dx = px.x - ps.x, dy = px.y - ps.y;
    if (dx * dx + dy * dy < 16.0) return false;
    *a = atan2(-dy, dx);
    return true;
}

static Vector2 PivotScreen(const TransformModal *t, Camera3D cam)
{
    return GetWorldToScreenEx(t->pivot, cam, GetScreenWidth(), GetScreenHeight());
}

// The rotation axis in the world (for the view sign, the guide and the op): the trackball's,
// the locked axis, else the view axis (toward the camera)
static Vector3 RotWorldAxis(const TransformModal *t, Camera3D cam)
{
    if (t->source == TRANSFORM_SRC_BALL) return t->ballAxis;
    int a = LockedAxis(t);
    if (a >= 0) return LockDir(t, a);
    return Vector3Negate(ViewForward(cam));
}

// The screen direction (unit, px) the near side of the turned object moves in for a positive
// turn about A: false when A points (within 30 degrees) at the camera, where the dial is used
static bool TrackDirection(const TransformModal *t, Camera3D cam, Vector3 A, Vector2 ps, Vector2 *dir)
{
    Vector3 toCam = Vector3Subtract(cam.position, t->pivot);
    float dist = Vector3Length(toCam);
    if (dist < 1e-4f) return false;
    Vector3 tangent = Vector3CrossProduct(A, Vector3Scale(toCam, 1.0f / dist));     // (|A| = 1)
    float len = Vector3Length(tangent);
    if (len < ROT_TRACK_MIN_SIN) return false;
    // a short step along the tangent, projected (5 % of the distance: well in front of the camera)
    Vector3 q = Vector3Add(t->pivot, Vector3Scale(tangent, 0.05f * dist / len));
    Vector2 d = Vector2Subtract(GetWorldToScreenEx(q, cam, GetScreenWidth(), GetScreenHeight()), ps);
    float dl = Vector2Length(d);
    if (!(dl > 1e-3f)) return false;
    *dir = Vector2Scale(d, 1.0f / dl);
    return true;
}

// A ring drag (an axis ring): the angle the cursor swept about the ring's axis in its plane (the
// start's and the cursor's rays through the plane, unwrapped frame by frame); edge-on at the
// start: the turntable. Degrees
static double RingDegrees(TransformModal *t, Camera3D cam)
{
    Vector3 A = LockDir(t, t->ring);
    if (t->ringTrack) {
        Vector2 track;
        return TrackDirection(t, cam, A, PivotScreen(t, cam), &track) ? Vector2DotProduct(t->mouse, track) * ROT_DEG_PER_PX : 0.0;
    }
    Vector3 h0, h1, P = t->pivot;
    if (RayPlane(CursorRay(t->mouseStart, cam), P, A, &h0) && RayPlane(CursorRay(Cursor(t), cam), P, A, &h1)) {
        Vector3 v0 = Vector3Subtract(h0, P), v1 = Vector3Subtract(h1, P);
        if (Vector3Length(v0) > 1e-6f && Vector3Length(v1) > 1e-6f) {
            double a = atan2(Vector3DotProduct(A, Vector3CrossProduct(v0, v1)), Vector3DotProduct(v0, v1));
            double da = a - t->lastAngle;
            while (da > PI) da -= 2.0 * PI;
            while (da < -PI) da += 2.0 * PI;
            t->sweep += da;
            t->lastAngle = a;
        }
    }
    return t->sweep * (180.0 / PI);     // (a ray past the horizon keeps the last angle)
}

static void RotateValue(TransformModal *t, Camera3D cam, bool ctrl)
{
    double v;
    bool neg;
    if (TypedValue(t, &v, &neg)) {
        // (the trackball: degrees about the camera's up)
        if (t->source == TRANSFORM_SRC_BALL) { Vector3 right; CamBasis(cam, &right, &t->ballAxis); }
        t->value[0] = round(v * 1e6) / 1e6;
        return;
    }
    double deg, a;
    if (t->source == TRANSFORM_SRC_BALL) {
        // axis perpendicular to the drag on screen (right: about up, down: about right), the
        // angle by the drag's length
        float len = Vector2Length(t->mouse);
        Vector3 right, up;
        CamBasis(cam, &right, &up);
        if (len > 1e-3f) t->ballAxis = Vector3Normalize(Vector3Add(Vector3Scale(up, t->mouse.x), Vector3Scale(right, t->mouse.y)));
        else t->ballAxis = up;
        deg = len * ROT_DEG_PER_PX;
    } else if (t->scripted) deg = t->scriptDeg;
    else if (t->source == TRANSFORM_SRC_RING && t->ring < 3) deg = RingDegrees(t, cam);
    else {
        Vector2 ps = PivotScreen(t, cam), track;
        Vector3 A = RotWorldAxis(t, cam);
        bool dial = t->source == TRANSFORM_SRC_RING;    // (the view ring: always the dial)
        if (!dial && TrackDirection(t, cam, A, ps, &track)) {
            // the axis across the view: the motion along the near side's screen direction
            deg = Vector2DotProduct(t->mouse, track) * ROT_DEG_PER_PX;
            t->haveAngle = false;
        } else {
            if (!dial && Vector2Distance(t->mouseStart, ps) < ROT_DIAL_PX) {
                t->sweep = -t->mouse.x * ROT_DEG_PER_PX * (PI / 180.0);
                t->haveAngle = false;
            } else if (ScreenAngle(Cursor(t), ps, &a)) {
                if (!t->haveAngle) {
                    double a0;
                    t->lastAngle = ScreenAngle(t->mouseStart, ps, &a0) ? a0 : a;
                    t->sweep = 0.0;
                    t->haveAngle = true;
                }
                double da = a - t->lastAngle;
                while (da > PI) da -= 2.0 * PI;
                while (da < -PI) da += 2.0 * PI;
                t->sweep += da;
                t->lastAngle = a;
            }
            // Counter-clockwise on screen turns counter-clockwise as seen from the camera: positive
            // about an axis that points toward the camera
            double sign = Vector3DotProduct(A, Vector3Subtract(cam.position, t->pivot)) >= 0.0f ? 1.0 : -1.0;
            deg = t->sweep * sign * (180.0 / PI);
        }
    }
    if (neg) deg = -deg;
    t->value[0] = ctrl ? Round(deg, t->angleStep) : Round(deg, 0.01);
    if (t->value[0] == 0.0) t->value[0] = 0.0;     // (no -0)
}

static void ScaleValue(TransformModal *t, Camera3D cam, bool ctrl)
{
    double v, f;
    bool neg;
    if (TypedValue(t, &v, &neg)) f = round(v * 1e6) / 1e6;
    else {
        Vector2 ps = PivotScreen(t, cam);
        double d0 = Vector2Distance(t->mouseStart, ps), d1 = Vector2Distance(Cursor(t), ps);
        double k = d0 < SCALE_NEAR_PX ? SCALE_NEAR_PX - d0 : 0.0;     // (a start on the pivot: no jump)
        f = (d1 + k) / (d0 + k);
        if (neg) f = -f;
        f = ctrl ? Round(f, 0.1) : Round(f, 1e-3);
    }
    int n = BitCount(t->axisMask);
    for (int a = 0; a < 3; a++) t->value[a] = n == 0 || (t->axisMask & (1 << a)) ? f : 1.0;
}

// The op the commit would apply now
static void BuildOp(const TransformModal *t, Camera3D cam, EditOp *op)
{
    memset(op, 0, sizeof(*op));
    op->hasPivot = true;
    op->pivot[0] = t->pivot.x;
    op->pivot[1] = t->pivot.y;
    op->pivot[2] = t->pivot.z;
    if (t->kind == TRANSFORM_GRAB) {
        op->kind = EDIT_MOVE;
        op->hasPivot = false;
        memcpy(op->delta, t->value, sizeof(op->delta));
    } else if (t->kind == TRANSFORM_ROTATE) {
        op->kind = EDIT_ROTATE;
        int a = t->source == TRANSFORM_SRC_BALL ? -1 : LockedAxis(t);
        bool local = t->local && t->hasLocal;
        if (a == 1 && !local) op->degrees = t->value[0];    // a yaw (exact)
        else if (a >= 0) {
            op->axis[a] = 1.0;
            op->angle = t->value[0];
            op->local = local;
        } else {
            // the view axis, or the trackball's axis-angle turn
            Vector3 A = RotWorldAxis(t, cam);
            op->axis[0] = A.x;
            op->axis[1] = A.y;
            op->axis[2] = A.z;
            op->angle = t->value[0];
        }
    } else {
        op->kind = EDIT_SCALE;
        op->local = t->local && t->hasLocal;
        memcpy(op->factor, t->value, sizeof(op->factor));
    }
}

static bool Identity(const TransformModal *t)
{
    if (t->kind == TRANSFORM_GRAB) return t->value[0] == 0.0 && t->value[1] == 0.0 && t->value[2] == 0.0;
    if (t->kind == TRANSFORM_ROTATE) return t->value[0] == 0.0;
    return t->value[0] == 1.0 && t->value[1] == 1.0 && t->value[2] == 1.0;
}

// Appends "; text" (or text) to the refusal line
static void Refuse(TransformModal *t, const char *text)
{
    size_t l = strlen(t->refusal);
    snprintf(t->refusal + l, sizeof(t->refusal) - l, "%s%s", l ? "; " : "", text);
}

// Which objects follow the kind, the refusal text (boxes follow whatever MapEditPreviewTransform
// accepts: Compute names the turns it refuses)
static void Classify(TransformModal *t)
{
    int refusedOther = 0, refusedDir = 0;
    for (int i = 0; i < t->count; i++) {
        int c = t->cls[i];
        bool f = false;
        if (t->kind == TRANSFORM_GRAB) {
            f = c != EDIT_CLASS_DIR_LIGHT && c != EDIT_CLASS_NONE;
            refusedDir += c == EDIT_CLASS_DIR_LIGHT;
        } else {
            f = CanTurn(c);
            refusedOther += c == EDIT_CLASS_OTHER_BOX;
        }
        t->follows[i] = f;
    }
    t->refusal[0] = '\0';
    const char *verb = t->kind == TRANSFORM_ROTATE ? "turn" : "scale";
    if (refusedOther)
        Refuse(t, TextFormat("%d box%s from stairs / waffle lines can't %s (they stay)", refusedOther, refusedOther == 1 ? "" : "es", verb));
    if (refusedDir)
        Refuse(t, TextFormat("%d directional light%s %s no position (%s)", refusedDir, refusedDir == 1 ? "" : "s",
                             refusedDir == 1 ? "has" : "have", refusedDir == 1 ? "it stays" : "they stay"));
}

// The value from the mouse / typed text, every object's transform, the preview
// The same placement to within 0.1 mm and 0.001 degree (a box's size comes from float corners:
// a square footprint turned 90 degrees swaps 2.20000029 and 2.20000076)
static bool SameTransform(const MS_Transform *a, const MS_Transform *b)
{
    const float *x = (const float *)a, *y = (const float *)b;
    for (int k = 0; k < 9; k++)
        if (fabsf(x[k] - y[k]) > (k >= 3 && k < 6 ? 1e-3f : 1e-4f)) return false;
    return true;
}

// The red HUD line when the value changes nothing: a box turned about its own centre is the same
// box when its footprint is square (or turned 180 degrees)
static void NoChangeNote(TransformModal *t)
{
    int box = -1, boxes = 0;
    for (int i = 0; i < t->count; i++)
        if (t->follows[i] && (t->cls[i] == EDIT_CLASS_BOX || t->cls[i] == EDIT_CLASS_BLOCK)) { boxes++; if (box < 0) box = i; }
    if (t->kind == TRANSFORM_ROTATE && boxes == 1 && t->commitCount == 1) {
        Vector3 sz = t->start[box].scale;
        Refuse(t, TextFormat("this box looks the same turned %g deg (footprint %g x %g m%s): nothing to turn. SHIFT+LMB adds "
                             "neighbours to turn them around their common centre", t->value[0], Round(sz.x, 1e-3), Round(sz.z, 1e-3),
                             fabsf(sz.x - sz.z) < 1e-4f ? ", square" : ""));
    } else
        Refuse(t, TextFormat("everything that follows ends where it started: nothing to commit%s", t->kind == TRANSFORM_ROTATE &&
                             boxes > 0 ? " (a square footprint turned 90 deg or any box turned 180 is the same box)" : ""));
}

static void Compute(TransformModal *t, Designer *d, Camera3D cam, bool ctrl)
{
    t->cam = cam;
    t->snapping = ctrl;
    t->snapStep = DesignerSnap(d);
    t->angleStep = DesignerAngleSnap(d);
    Classify(t);
    if (t->kind == TRANSFORM_GRAB) GrabValue(t, cam, t->snapStep, ctrl);
    else if (t->kind == TRANSFORM_ROTATE) RotateValue(t, cam, ctrl);
    else {
        ScaleValue(t, cam, ctrl);
        if (t->value[0] <= 0.0 || t->value[1] <= 0.0 || t->value[2] <= 0.0) Refuse(t, "a scale factor must be > 0");
    }
    EditOp op;
    BuildOp(t, cam, &op);
    bool changed = false;
    int refusedTurn = 0;
    t->commitCount = 0;
    for (int i = 0; i < t->count; i++) {
        MS_Transform tr = t->start[i];
        if (t->follows[i]) {
            if (MapEditPreviewTransform(&op, t->cls[i], &t->start[i], &tr)) t->commit[t->commitCount++] = t->objs[i];
            else {
                t->follows[i] = 0;
                refusedTurn += t->kind == TRANSFORM_ROTATE && (t->cls[i] == EDIT_CLASS_BOX || t->cls[i] == EDIT_CLASS_BLOCK);
            }
        }
        if (memcmp(&tr, &t->now[i], sizeof(tr)) != 0) changed = true;
        t->now[i] = tr;
    }
    // (an object the edits can't turn this way: MapEditPreviewTransform refused it; it stays, named here)
    if (refusedTurn && !Identity(t))
        Refuse(t, TextFormat("%d box%s can't take this turn (%s)", refusedTurn, refusedTurn == 1 ? "" : "es", refusedTurn == 1 ? "it stays" : "they stay"));
    bool moved = false;
    for (int i = 0; i < t->count && !moved; i++) moved = t->follows[i] && !SameTransform(&t->now[i], &t->start[i]);
    t->noChange = !Identity(t) && t->commitCount > 0 && !moved;
    if (t->noChange) NoChangeNote(t);
    if (t->previewing && changed) {
        MS_EditPreviewSet(scene, t->now);
        DesignerAdoptVersion(d);    // (the same objects at the same indices)
    }
}

// #endregion

// ======================================================================================
// #region Begin, update, end
// ======================================================================================

static void ResetValues(TransformModal *t)
{
    t->source = TRANSFORM_SRC_KEYS;
    t->dragging = t->dragMoved = t->scripted = t->scriptCtrl = false;
    t->rawMouse = (Vector2){ 0, 0 };
    t->axisMask = 0;
    t->local = false;
    t->typed[0] = '\0';
    t->cancelNote[0] = '\0';
    t->noChange = false;
    t->haveAngle = false;
    t->sweep = t->lastAngle = 0.0;
    t->value[0] = t->value[1] = t->value[2] = t->kind == TRANSFORM_SCALE ? 1.0 : 0.0;
}

static bool Grow(TransformModal *t, int n)
{
    if (n <= t->cap) return true;
    MS_ObjRef *objs = realloc(t->objs, sizeof(MS_ObjRef) * (size_t)n);
    if (objs) t->objs = objs;
    MS_Transform *start = realloc(t->start, sizeof(MS_Transform) * (size_t)n);
    if (start) t->start = start;
    MS_Transform *now = realloc(t->now, sizeof(MS_Transform) * (size_t)n);
    if (now) t->now = now;
    unsigned char *cls = realloc(t->cls, (size_t)n);
    if (cls) t->cls = cls;
    unsigned char *follows = realloc(t->follows, (size_t)n);
    if (follows) t->follows = follows;
    MS_ObjRef *commit = realloc(t->commit, sizeof(MS_ObjRef) * (size_t)n);
    if (commit) t->commit = commit;
    if (!objs || !start || !now || !cls || !follows || !commit) return false;
    t->cap = n;
    return true;
}

bool TransformBegin(TransformModal *t, Designer *d, TransformKind kind, char *msg, int msgSize)
{
    int n = d->selectedCount;
    if (n == 0) { snprintf(msg, msgSize, "nothing selected (LMB selects, SHIFT+LMB adds, CTRL+LMB a group)"); return false; }
    if (!Grow(t, n)) { snprintf(msg, msgSize, "out of memory"); return false; }
    t->count = n;
    memcpy(t->objs, d->selected, sizeof(MS_ObjRef) * (size_t)n);
    MapEditClassify(t->objs, n, t->cls);
    for (int i = 0; i < n; i++) {
        int e = MS_ObjectEntity(scene, t->objs[i]);
        t->start[i] = e >= 0 ? scene->entityTransforms[e] : (MS_Transform){ 0 };
        t->now[i] = t->start[i];
        if (e < 0) t->cls[i] = EDIT_CLASS_NONE;
    }
    t->hasLocal = ActiveAxes(t->objs, t->cls, n, t->localAxes);
    t->pivot = SelectionPivot(t->objs, t->cls, n);
    t->kind = kind;
    ResetValues(t);
    t->mouse = (Vector2){ 0, 0 };
    t->mouseStart = d->interactive ? GetMousePosition() : (Vector2){ GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f };
    t->previewing = MS_EditPreviewBegin(scene, t->objs, n);
    DesignerAdoptVersion(d);
    if (d->interactive) while (GetCharPressed()) {}     // (nothing typed before it opened goes into it)
    msg[0] = '\0';
    return true;
}

void TransformEnd(TransformModal *t, Designer *d)
{
    if (t->kind == TRANSFORM_NONE) return;
    // (MS_EditPreviewEnd is a no-op once the commit's MS_EditSetText ended the preview; after
    // a refused commit it puts everything back)
    if (t->previewing) {
        unsigned int v = scene->version;
        MS_EditPreviewEnd(scene);
        if (scene->version != v) DesignerAdoptVersion(d);
    }
    t->previewing = false;
    t->kind = TRANSFORM_NONE;
    t->source = TRANSFORM_SRC_KEYS;
    t->dragging = false;
}

static void Cancel(TransformModal *t, Designer *d)
{
    if (t->previewing) {
        MS_EditPreviewEnd(scene);
        DesignerAdoptVersion(d);
    }
    t->previewing = false;
    t->kind = TRANSFORM_NONE;
    t->source = TRANSFORM_SRC_KEYS;
    t->dragging = false;
}

// LMB / ENTER with nothing to write (no value, nothing follows, or a value that changes
// nothing): everything back, cancelNote says why when there was a value
static void CancelNothing(TransformModal *t, Designer *d)
{
    t->cancelNote[0] = '\0';
    if (t->noChange)
        snprintf(t->cancelNote, sizeof(t->cancelNote), "nothing written: %s", t->kind == TRANSFORM_ROTATE
                 ? "turned, the objects are where they were (a square footprint turned 90 deg, or any box turned 180)"
                 : "every object would end where it started");
    else if (!Identity(t) && t->commitCount == 0) snprintf(t->cancelNote, sizeof(t->cancelNote), "nothing written: nothing selected can follow");
    Cancel(t, d);
}

// X / Y / Z (plane: SHIFT): lock, the same again local (the selection has local axes), a third time none
static void LockKey(TransformModal *t, int axis, bool plane)
{
    int mask = plane ? (7 & ~(1 << axis)) : 1 << axis;
    if (t->axisMask == mask) {
        if (!t->local && t->hasLocal) t->local = true;
        else { t->axisMask = 0; t->local = false; }
    } else {
        t->axisMask = mask;
        t->local = false;
    }
}

static void TypeChar(TransformModal *t, int c)
{
    size_t n = strlen(t->typed);
    if (c == '-') {
        if (t->typed[0] == '-') memmove(t->typed, t->typed + 1, n);
        else if (n + 1 < sizeof(t->typed)) { memmove(t->typed + 1, t->typed, n + 1); t->typed[0] = '-'; }
        return;
    }
    if (c == '.' && strchr(t->typed, '.')) return;
    if ((c >= '0' && c <= '9') || c == '.')
        if (n + 1 < sizeof(t->typed)) { t->typed[n] = (char)c; t->typed[n + 1] = '\0'; }
}

// LMB / ENTER (or the release of a drag): the op, or everything back when it writes nothing
static TransformResult Confirm(TransformModal *t, Designer *d, Camera3D cam, EditOp *op)
{
    BuildOp(t, cam, op);
    if (!Identity(t) && t->commitCount > 0 && !t->noChange) return TRANSFORM_COMMIT;
    CancelNothing(t, d);
    return TRANSFORM_CANCELLED;
}

TransformResult TransformUpdate(TransformModal *t, Designer *d, Camera3D cam, EditOp *op)
{
    if (t->kind == TRANSFORM_NONE) return TRANSFORM_RUNNING;
    if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        Cancel(t, d);
        return TRANSFORM_CANCELLED;
    }
    bool shift = Shift(), ctrl = Ctrl();
    Vector2 md = GetMouseDelta();
    t->mouse = Vector2Add(t->mouse, Vector2Scale(md, shift ? 0.1f : 1.0f));
    t->rawMouse = Vector2Add(t->rawMouse, md);
    if (t->dragging && Vector2Length(t->rawMouse) > DRAG_MIN_PX) t->dragMoved = true;
    static const int KIND_KEYS[3] = { KEY_G, KEY_R, KEY_S };
    for (int k = 0; k < 3; k++)
        if (IsKeyPressed(KIND_KEYS[k]) && !ctrl) {
            // A new kind from the original transforms: the cursor where it is now is the new start
            // (a gizmo drag becomes the keyboard modal)
            t->kind = (TransformKind)(TRANSFORM_GRAB + k);
            ResetValues(t);
            t->mouseStart = Cursor(t);
            t->mouse = (Vector2){ 0, 0 };
        }
    static const int AXIS_KEYS[3] = { KEY_X, KEY_Y, KEY_Z };
    for (int a = 0; a < 3; a++)
        if (IsKeyPressed(AXIS_KEYS[a])) {
            if (t->source != TRANSFORM_SRC_KEYS) {
                // (a lock key in a gizmo drag: the keyboard modal from here, the lock its own)
                t->source = TRANSFORM_SRC_KEYS;
                t->dragging = t->scripted = false;
                t->axisMask = 0;
                t->local = false;
            }
            LockKey(t, a, shift);
            t->haveAngle = false;   // (the sweep starts again for the new axis)
            t->mouseStart = Cursor(t);
            t->mouse = (Vector2){ 0, 0 };
        }
    if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && t->typed[0]) t->typed[strlen(t->typed) - 1] = '\0';
    for (int c = GetCharPressed(); c; c = GetCharPressed()) TypeChar(t, c);
    Compute(t, d, cam, ctrl);
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) return Confirm(t, d, cam, op);
    if (t->dragging) {
        // The gizmo's button: released after a drag commits (unless a value was typed); a click
        // (released within DRAG_MIN_PX), or typed text, leaves the modal open without the button
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            t->dragging = false;
            if (t->dragMoved && !t->typed[0]) return Confirm(t, d, cam, op);
        }
        return TRANSFORM_RUNNING;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return Confirm(t, d, cam, op);
    return TRANSFORM_RUNNING;
}

// Words of an --edit-modal spec (at most 8, each at most 31 bytes)
static int SpecWords(const char *spec, char words[8][32])
{
    int n = 0;
    for (const char *p = spec; *p && n < 8; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        int l = 0;
        while (*p && *p != ' ') { if (l < 31) words[n][l++] = *p; p++; }
        words[n++][l] = '\0';
    }
    return n;
}

bool TransformOpenScripted(TransformModal *t, Designer *d, Camera3D cam, const char *spec, char *msg, int msgSize)
{
    char words[8][32] = { { 0 } };
    int n = SpecWords(spec, words);
    TransformKind kind = n > 0 && !strcmp(words[0], "g") ? TRANSFORM_GRAB : n > 0 && !strcmp(words[0], "r") ? TRANSFORM_ROTATE
                       : n > 0 && !strcmp(words[0], "s") ? TRANSFORM_SCALE : TRANSFORM_NONE;
    if (kind == TRANSFORM_NONE) { snprintf(msg, msgSize, "--edit-modal \"<g|r|s> [x|y|z|X|Y|Z] [typed]\": not '%s'", spec); return false; }
    if (!TransformBegin(t, d, kind, msg, msgSize)) return false;
    for (int k = 1; k < n; k++) {
        const char *w = words[k];
        char c = w[0];
        bool lock = (c == 'x' || c == 'y' || c == 'z' || c == 'X' || c == 'Y' || c == 'Z') && (w[1] == '\0' || (w[1] == c && w[2] == '\0'));
        if (lock) {
            int a = (c | 0x20) - 'x';
            bool plane = c < 'a';
            LockKey(t, a, plane);
            if (w[1]) LockKey(t, a, plane);     // doubled: local
        } else if (!strcmp(w, "ctrl")) t->scriptCtrl = true;
        else if (!strcmp(w, "step") && k + 1 < n) DesignerSetAngleSnap(d, (float)atof(words[++k]));
        else
            for (const char *q = w; *q; q++) TypeChar(t, *q);
    }
    Compute(t, d, cam, t->scriptCtrl);
    return true;
}

TransformResult TransformFinish(TransformModal *t, Designer *d, bool commit, EditOp *op)
{
    if (t->kind == TRANSFORM_NONE) return TRANSFORM_CANCELLED;
    if (!commit || Identity(t) || t->commitCount == 0 || t->noChange) {
        if (commit) CancelNothing(t, d);
        else Cancel(t, d);
        return TRANSFORM_CANCELLED;
    }
    // (the value and the camera of the last Compute: a turn about the view axis is about the
    // axis the preview showed)
    BuildOp(t, t->cam, op);
    return TRANSFORM_COMMIT;
}

// #endregion

// ======================================================================================
// #region The rotate gizmo
// ======================================================================================

// Where the gizmo is on screen this frame
typedef struct {
    Vector3 P;              // the pivot
    Vector2 ps;             // its screen position
    float r;                // the world radius that is GIZMO_RADIUS_PX at the pivot's depth
    Vector3 axes[3];        // the rings' axes (world, unit)
    Vector3 toCam;          // unit, from the pivot to the camera
} GizmoFrame;

static bool MakeFrame(Camera3D cam, Vector3 P, const Vector3 axes[3], GizmoFrame *g)
{
    Vector3 fwd = ViewForward(cam), toCam = Vector3Subtract(cam.position, P);
    float depth = -Vector3DotProduct(toCam, fwd), dist = Vector3Length(toCam);
    if (!(depth > 0.05f) || dist < 1e-4f || cam.projection != CAMERA_PERSPECTIVE) return false;
    int w = GetScreenWidth(), h = GetScreenHeight();
    g->P = P;
    g->ps = GetWorldToScreenEx(P, cam, w, h);
    g->r = GIZMO_RADIUS_PX * depth * 2.0f * tanf(cam.fovy * 0.5f * DEG2RAD) / (float)h;
    for (int a = 0; a < 3; a++) g->axes[a] = axes[a];
    g->toCam = Vector3Scale(toCam, 1.0f / dist);
    return true;
}

// A unit vector in the plane normal to A: v's part in it, else any
static Vector3 InPlane(Vector3 A, Vector3 v)
{
    Vector3 p = Vector3Subtract(v, Vector3Scale(A, Vector3DotProduct(A, v)));
    float l = Vector3Length(p);
    if (l > 1e-3f) return Vector3Scale(p, 1.0f / l);
    p = Vector3CrossProduct(A, fabsf(A.y) < 0.9f ? (Vector3){ 0, 1, 0 } : (Vector3){ 1, 0, 0 });
    return Vector3Normalize(p);
}

// The point of the ring about A (radius scale * r) at angle th from u (right-handed about A)
static Vector3 RingPoint(const GizmoFrame *g, Vector3 A, Vector3 u, float scale, float th)
{
    Vector3 v = Vector3CrossProduct(A, u);
    Vector3 dir = Vector3Add(Vector3Scale(u, cosf(th)), Vector3Scale(v, sinf(th)));
    return Vector3Add(g->P, Vector3Scale(dir, g->r * scale));
}

static Vector2 Project(Vector3 p, Camera3D cam) { return GetWorldToScreenEx(p, cam, GetScreenWidth(), GetScreenHeight()); }

static float SegmentDistance(Vector2 p, Vector2 a, Vector2 b)
{
    Vector2 ab = Vector2Subtract(b, a);
    float l2 = Vector2DotProduct(ab, ab), s = l2 > 0.0f ? Vector2DotProduct(Vector2Subtract(p, a), ab) / l2 : 0.0f;
    s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
    return Vector2Distance(p, Vector2Add(a, Vector2Scale(ab, s)));
}

// Axis ring a's front half: draws it (thick > 0) and / or returns the screen distance of px to it
static float AxisRing(const GizmoFrame *g, Camera3D cam, int a, Vector2 px, float thick, Color c)
{
    Vector3 A = g->axes[a], u = InPlane(A, g->toCam);
    float best = INFINITY, step = 2.0f * PI / GIZMO_SEGMENTS;
    Vector2 prev = Project(RingPoint(g, A, u, 1.0f, 0.0f), cam);
    for (int k = 1; k <= GIZMO_SEGMENTS; k++) {
        Vector2 cur = Project(RingPoint(g, A, u, 1.0f, k * step), cam);
        Vector3 mid = Vector3Subtract(RingPoint(g, A, u, 1.0f, (k - 0.5f) * step), g->P);
        if (Vector3DotProduct(mid, g->toCam) > GIZMO_FRONT_MIN * g->r) {
            if (thick > 0.0f) DrawLineEx(prev, cur, thick, c);
            float dd = SegmentDistance(px, prev, cur);
            if (dd < best) best = dd;
        }
        prev = cur;
    }
    return best;
}

// A circle on screen (the view ring)
static void ScreenCircle(Vector2 c, float radius, float thick, Color col)
{
    float step = 2.0f * PI / GIZMO_SEGMENTS;
    for (int k = 0; k < GIZMO_SEGMENTS; k++) {
        Vector2 a = { c.x + radius * cosf(k * step), c.y + radius * sinf(k * step) };
        Vector2 b = { c.x + radius * cosf((k + 1) * step), c.y + radius * sinf((k + 1) * step) };
        DrawLineEx(a, b, thick, col);
    }
}

// The idle gizmo's frame (no modal): the designer's selection, its pivot and the ring axes
static bool IdleFrame(TransformModal *t, Designer *d, Camera3D cam, GizmoFrame *g)
{
    if (!GizmoSelection(t, d)) return false;
    Vector3 axes[3];
    bool local = ActiveAxes(d->selected, t->gcls, d->selectedCount, axes);
    if (!(t->gizmoLocal && local)) for (int a = 0; a < 3; a++) axes[a] = Unit(a);
    return MakeFrame(cam, SelectionPivot(d->selected, t->gcls, d->selectedCount), axes, g);
}

GizmoPart TransformGizmoHover(TransformModal *t, Designer *d, Camera3D cam, Vector2 px, bool enabled)
{
    t->hover = GIZMO_NONE;
    GizmoFrame g;
    if (t->kind != TRANSFORM_NONE || !IdleFrame(t, d, cam, &g)) return GIZMO_NONE;
    if (!enabled) return t->hover = t->forceHover;
    // the view ring, else the nearest axis ring, else the trackball
    float dc = Vector2Distance(px, g.ps);
    if (fabsf(dc - GIZMO_RADIUS_PX * GIZMO_VIEW_SCALE) <= GIZMO_HIT_PX) return t->hover = GIZMO_VIEW;
    float best = GIZMO_HIT_PX;
    for (int a = 0; a < 3; a++) {
        float dd = AxisRing(&g, cam, a, px, 0.0f, BLANK);
        if (dd <= best) { best = dd; t->hover = (GizmoPart)(GIZMO_X + a); }
    }
    if (t->hover == GIZMO_NONE && dc < GIZMO_RADIUS_PX) t->hover = GIZMO_BALL;
    return t->hover;
}

// A begun rotate modal becomes a drag of `part`: its source, its lock and where the arc starts
// (fromCursor: the start cursor's ray through the ring's plane; else the ring's front)
static void SetupDrag(TransformModal *t, Camera3D cam, GizmoPart part, bool local, bool fromCursor)
{
    t->cam = cam;
    t->source = part == GIZMO_BALL ? TRANSFORM_SRC_BALL : TRANSFORM_SRC_RING;
    t->ring = part == GIZMO_VIEW ? 3 : part - GIZMO_X;
    if (t->source == TRANSFORM_SRC_RING && t->ring < 3) {
        t->axisMask = 1 << t->ring;
        t->local = local && t->hasLocal;
    }
    Vector3 right, up;
    CamBasis(cam, &right, &up);
    t->ballAxis = up;
    Vector3 A = t->source == TRANSFORM_SRC_RING ? RotWorldAxis(t, cam) : up;
    Vector3 toCam = Vector3Normalize(Vector3Subtract(cam.position, t->pivot)), h;
    t->ringTrack = t->source == TRANSFORM_SRC_RING && t->ring < 3 && fabsf(Vector3DotProduct(A, toCam)) < GIZMO_EDGE_ON_SIN;
    if (fromCursor && !t->ringTrack && RayPlane(CursorRay(t->mouseStart, cam), t->pivot, A, &h) &&
        Vector3Length(Vector3Subtract(h, t->pivot)) > 1e-5f)
        t->ringU = InPlane(A, Vector3Subtract(h, t->pivot));
    else t->ringU = t->ring == 3 ? InPlane(A, right) : InPlane(A, toCam);
    t->sweep = t->lastAngle = 0.0;
    t->haveAngle = false;
    t->rawMouse = (Vector2){ 0, 0 };
    t->dragMoved = false;
}

bool TransformGizmoBegin(TransformModal *t, Designer *d, Camera3D cam, GizmoPart part, char *msg, int msgSize)
{
    if (part == GIZMO_NONE) { snprintf(msg, msgSize, "nothing of the gizmo there"); return false; }
    bool local = t->gizmoLocal;
    if (!TransformBegin(t, d, TRANSFORM_ROTATE, msg, msgSize)) return false;
    SetupDrag(t, cam, part, local, true);
    t->dragging = true;
    t->hover = GIZMO_NONE;
    Compute(t, d, cam, Ctrl());
    return true;
}

bool TransformGizmoScripted(TransformModal *t, Designer *d, Camera3D cam, const char *spec, char *msg, int msgSize)
{
    char words[8][32] = { { 0 } };
    int n = SpecWords(spec, words);
    bool ball = n >= 3 && !strcmp(words[0], "ball"), ring = n >= 3 && !strcmp(words[0], "ring");
    GizmoPart part = GIZMO_NONE;
    bool local = false;
    if (ring) {
        const char *w = words[1];
        if (!strcmp(w, "view")) part = GIZMO_VIEW;
        else if ((w[0] == 'x' || w[0] == 'y' || w[0] == 'z') && (w[1] == '\0' || (w[1] == w[0] && w[2] == '\0'))) {
            part = (GizmoPart)(GIZMO_X + (w[0] - 'x'));
            local = w[1] != '\0';
        }
    } else if (ball) part = GIZMO_BALL;
    char *end1, *end2;
    double a = strtod(words[1], &end1), b = strtod(words[2], &end2);
    if (part == GIZMO_NONE || (ring && (*end2 || !isfinite(b))) || (ball && (*end1 || *end2 || !isfinite(a) || !isfinite(b)))) {
        snprintf(msg, msgSize, "--edit-modal \"ring <x|y|z|xx|yy|zz|view> <deg> | ball <dx> <dy> [ctrl] [step <deg>] [type <v>]\": not '%s'", spec);
        return false;
    }
    if (!TransformBegin(t, d, TRANSFORM_ROTATE, msg, msgSize)) return false;
    for (int k = 3; k < n; k++) {
        if (!strcmp(words[k], "ctrl")) t->scriptCtrl = true;
        else if (!strcmp(words[k], "step") && k + 1 < n) DesignerSetAngleSnap(d, (float)atof(words[++k]));
        else if (!strcmp(words[k], "type") && k + 1 < n)
            for (const char *q = words[++k]; *q; q++) TypeChar(t, *q);
    }
    SetupDrag(t, cam, part, local, false);
    if (ball) {
        t->mouseStart = PivotScreen(t, cam);
        t->mouse = (Vector2){ (float)a, (float)b };
    } else {
        // the cursor on the ring where the arc starts and where it ends (the readout's place)
        t->scripted = true;
        t->scriptDeg = b;
        GizmoFrame g;
        Vector3 axes[3] = { Unit(0), Unit(1), Unit(2) };
        if (MakeFrame(cam, t->pivot, axes, &g)) {
            Vector3 A = RotWorldAxis(t, cam);
            float s = t->ring == 3 ? GIZMO_VIEW_SCALE : 1.0f;
            t->mouseStart = Project(RingPoint(&g, A, t->ringU, s, 0.0f), cam);
            t->mouse = Vector2Subtract(Project(RingPoint(&g, A, t->ringU, s, (float)(b * DEG2RAD)), cam), t->mouseStart);
        }
    }
    Compute(t, d, cam, t->scriptCtrl);
    return true;
}

// "37.5" (at most two decimals, no trailing zeros)
static const char *Degrees(double v)
{
    static char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", v);
    char *p = buf + strlen(buf) - 1;
    while (p > buf && *p == '0') *p-- = '\0';
    if (*p == '.') *p = '\0';
    if (!strcmp(buf, "-0")) snprintf(buf, sizeof(buf), "0");
    return buf;
}

static Color AxisColor(int a) { return a == 0 ? COLOR_X : a == 1 ? COLOR_Y : COLOR_Z; }

// The filled sector of a ring drag from ringU by the value (at most one turn), with its two edges
static void DrawArc(const TransformModal *t, const GizmoFrame *g, Camera3D cam, Vector3 A, float scale, Color c)
{
    double deg = fmax(-360.0, fmin(360.0, t->value[0]));
    int n = (int)ceil(fabs(deg) / 4.0);
    n = n < 1 ? 1 : n > 90 ? 90 : n;
    Vector2 first = Project(RingPoint(g, A, t->ringU, scale, 0.0f), cam), prev = first;
    for (int k = 1; k <= n; k++) {
        Vector2 cur = Project(RingPoint(g, A, t->ringU, scale, (float)(deg * DEG2RAD * k / n)), cam);
        // (raylib fills counter-clockwise triangles: on a y-down screen a negative cross product)
        float cross = (prev.x - g->ps.x) * (cur.y - g->ps.y) - (prev.y - g->ps.y) * (cur.x - g->ps.x);
        if (cross < 0.0f) DrawTriangle(g->ps, prev, cur, Fade(c, 0.25f));
        else DrawTriangle(g->ps, cur, prev, Fade(c, 0.25f));
        prev = cur;
    }
    DrawLineEx(g->ps, first, 2.0f, Fade(c, 0.9f));
    DrawLineEx(g->ps, prev, 2.0f, c);
}

void TransformDrawGizmo(TransformModal *t, Designer *d, Camera3D cam)
{
    bool drag = t->kind == TRANSFORM_ROTATE && t->source != TRANSFORM_SRC_KEYS;
    if (t->kind != TRANSFORM_NONE && !drag) return;
    GizmoFrame g;
    if (drag) {
        Vector3 axes[3];
        for (int a = 0; a < 3; a++) axes[a] = t->local && t->hasLocal ? t->localAxes[a] : Unit(a);
        if (!MakeFrame(t->cam, t->pivot, axes, &g)) return;
        cam = t->cam;
    } else if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || !IdleFrame(t, d, cam, &g)) return;

    GizmoPart hot = drag ? (t->source == TRANSFORM_SRC_BALL ? GIZMO_BALL : (GizmoPart)(t->ring == 3 ? GIZMO_VIEW : GIZMO_X + t->ring))
                         : t->hover;
    float dim = drag ? 0.3f : 0.85f;     // (while dragging the other parts step back)
    // the trackball disc, the axis rings, the view ring, the pivot
    DrawCircleV(g.ps, GIZMO_RADIUS_PX, Fade(GIZMO_BALL_COLOR, hot == GIZMO_BALL ? 0.16f : 0.07f));
    for (int a = 0; a < 3; a++) {
        bool on = (int)hot == GIZMO_X + a;
        AxisRing(&g, cam, a, g.ps, on ? 3.0f : 2.0f, on ? ColorBrightness(AxisColor(a), 0.35f) : Fade(AxisColor(a), dim));
    }
    ScreenCircle(g.ps, GIZMO_RADIUS_PX * GIZMO_VIEW_SCALE, hot == GIZMO_VIEW ? 3.0f : 2.0f, Fade(WHITE, hot == GIZMO_VIEW ? 1.0f : dim * 0.8f));
    DrawCircleV(g.ps, 3.0f, Fade(WHITE, 0.9f));
    if (!drag) return;

    // The drag: its arc (a ring) or its vector (the trackball), the cursor and the readout
    Vector2 cur = Cursor(t);
    Color c = t->source == TRANSFORM_SRC_BALL ? GIZMO_BALL_COLOR : t->ring == 3 ? WHITE : AxisColor(t->ring);
    if (t->source == TRANSFORM_SRC_RING)
        DrawArc(t, &g, cam, RotWorldAxis(t, cam), t->ring == 3 ? GIZMO_VIEW_SCALE : 1.0f, c);
    else DrawLineEx(g.ps, cur, 2.0f, Fade(WHITE, 0.8f));
    DrawCircleLinesV(cur, 5.0f, WHITE);
    char text[96];
    int l = snprintf(text, sizeof(text), "%s\xC2\xB0", Degrees(t->value[0]));
    if (t->typed[0]) snprintf(text + l, sizeof(text) - (size_t)l, "  typed");
    else if (t->snapping) snprintf(text + l, sizeof(text) - (size_t)l, "  snap %s\xC2\xB0", Degrees(t->angleStep));
    UiHudText(text, (int)cur.x + 14, (int)cur.y + 10, UI_HUD_LINE, UI_S, t->noChange || t->commitCount == 0 ? UI_ERROR : UI_INK);
}

// #endregion

// ======================================================================================
// #region Drawing
// ======================================================================================

void TransformDrawGuides(const TransformModal *t, Camera3D cam)
{
    if (t->kind == TRANSFORM_NONE || t->source != TRANSFORM_SRC_KEYS) return;     // (a gizmo drag draws the gizmo)
    BeginMode3D(cam);
    rlDisableDepthTest();
    Vector3 P = t->pivot;
    // a long line through the pivot along each locked axis (rotate: the axis turned about)
    for (int a = 0; a < 3; a++) {
        bool on = t->kind == TRANSFORM_ROTATE ? LockedAxis(t) == a : (t->axisMask & (1 << a)) != 0;
        if (!on) continue;
        Vector3 A = Vector3Scale(LockDir(t, a), 2000.0f);
        DrawLine3D(Vector3Subtract(P, A), Vector3Add(P, A), AxisColor(a));
    }
    // the pivot: a small cross
    float s = 0.02f * Vector3Distance(cam.position, P);
    DrawLine3D((Vector3){ P.x - s, P.y, P.z }, (Vector3){ P.x + s, P.y, P.z }, WHITE);
    DrawLine3D((Vector3){ P.x, P.y - s, P.z }, (Vector3){ P.x, P.y + s, P.z }, WHITE);
    DrawLine3D((Vector3){ P.x, P.y, P.z - s }, (Vector3){ P.x, P.y, P.z + s }, WHITE);
    rlDrawRenderBatchActive();
    rlDisableDepthTest();
    EndMode3D();
}

// "X 1.250 m", "local Y -0.500 m", "X 0.250  Z 1.000 m", ...
static void ValueText(const TransformModal *t, char *out, int size)
{
    bool local = t->local && t->hasLocal;
    int n = BitCount(t->axisMask), a = LockedAxis(t);
    if (t->kind == TRANSFORM_ROTATE) {
        const char *about = t->source == TRANSFORM_SRC_BALL
                          ? TextFormat("the trackball axis (%.2f %.2f %.2f)", t->ballAxis.x, t->ballAxis.y, t->ballAxis.z)
                          : a >= 0 ? TextFormat("%s%c", local ? "local " : "", AXIS_NAME[a]) : "the view axis";
        snprintf(out, size, "%.2f deg about %s", t->value[0], about);
        return;
    }
    char part[3][48];
    int parts = 0;
    for (int k = 0; k < 3; k++) {
        if (n > 0 && !(t->axisMask & (1 << k))) continue;
        double v = t->value[k];
        if (t->kind == TRANSFORM_GRAB && local)     // the amount along the active object's axis
            v = t->value[0] * t->localAxes[k].x + t->value[1] * t->localAxes[k].y + t->value[2] * t->localAxes[k].z;
        snprintf(part[parts++], sizeof(part[0]), "%s%c %.3f", local ? "local " : "", AXIS_NAME[k], v);
    }
    if (t->kind == TRANSFORM_SCALE && n == 0) {
        snprintf(out, size, "x %.3f (uniform)", t->value[0]);
        return;
    }
    snprintf(out, size, "%s%s%s%s%s%s", part[0], parts > 1 ? "  " : "", parts > 1 ? part[1] : "", parts > 2 ? "  " : "",
             parts > 2 ? part[2] : "", t->kind == TRANSFORM_GRAB ? " m" : "");
}

int TransformDrawHud(const TransformModal *t, int x, int y)
{
    if (t->kind == TRANSFORM_NONE) return 0;
    int y0 = y;
    const char *kind = t->kind == TRANSFORM_GRAB ? "GRAB" : t->kind == TRANSFORM_ROTATE ? "ROTATE" : "SCALE";
    char value[160];
    ValueText(t, value, sizeof(value));
    int n = BitCount(t->axisMask);
    const char *space = t->source == TRANSFORM_SRC_BALL ? "trackball"
                      : n == 0 ? (t->kind != TRANSFORM_ROTATE ? "free" : t->source == TRANSFORM_SRC_RING ? "view ring" : "view")
                      : t->local && t->hasLocal ? "local" : "global";
    const char *snap;
    if (t->snapping)
        snap = t->kind == TRANSFORM_GRAB ? TextFormat("^snap %g m^", t->snapStep)
             : t->kind == TRANSFORM_ROTATE ? TextFormat("^snap %g deg^", t->angleStep) : "^snap 0.1^";
    else snap = t->kind == TRANSFORM_ROTATE ? TextFormat("snap off (%g deg)", t->angleStep) : "snap off";
    const char *typed = t->typed[0] ? TextFormat("typed: %s_", t->typed) : "type a value";
    // (state only: the modal's keys are in the keybindings panel, ESC)
    UiHudHints(TextFormat("%s  %s  %s|%s|%s", kind, value, space, typed, snap),
               x, y, UI_HUD_LINE, UI_S, Fade(MODAL_COLOR, 0.95f));
    y += UI_HUD_LINE;
    if (t->refusal[0]) {
        UiHudText(t->refusal, x, y, UI_HUD_LINE, UI_S, UI_ERROR);
        y += UI_HUD_LINE;
    }
    return y - y0;
}

// #endregion
