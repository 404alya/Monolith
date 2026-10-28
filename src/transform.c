// transform -- the edit mode's transform modal: G / R / S grab, rotate and scale the selection
// (docs/live-editing.md 7)
//
// G / R / S in edit mode start it on the selection; the camera is frozen and the cursor hidden,
// and the raw mouse motion (SHIFT: ten times slower) drives the value every frame:
//   grab     the cursor's motion on the plane through the pivot facing the camera, on the locked
//            axis (closest point between the cursor's ray and the axis line) or on the locked plane
//   rotate   about the view axis, or the locked axis (a plane lock: the axis normal to it); with
//            boxes that follow, no lock means the vertical. An axis across the view (a vertical
//            one seen from eye height) follows the motion along the screen direction the near
//            side moves in (0.5 deg / px: drag right, the side facing you goes right); an axis
//            pointing at the camera takes the angle the cursor sweeps around the pivot's screen
//            position (or the horizontal motion when the cursor starts near the pivot). Boxes
//            turn only in 90 degree steps: the next step comes 30 degrees past the last one. A
//            value that changes nothing (a square box turned 90 degrees about its centre, any
//            box turned 180) is named in red and LMB / ENTER write nothing
//   scale    the ratio of the cursor's distance to the pivot's screen position now and at the
//            start: uniform, one axis (axis lock) or two (plane lock)
// X / Y / Z lock an axis, SHIFT+X / Y / Z a plane (the other two axes); the same key again uses
// the first selected model's own axes (local; boxes have none), a third time unlocks. Digits, '.',
// '-' (toggles the sign) and BACKSPACE type the value (metres along the locked axis or the view's
// right, degrees, the factor); typed values are exact. CTRL held snaps: the pivot onto the N grid,
// 15 degrees, factors to 0.1. G / R / S switch kind (from the original transforms, the lock and
// the typed value cleared). LMB / ENTER commit one edit (one EditOp through MapEditApply: one undo
// step); RMB / ESC cancel (MS_EditPreviewEnd: everything back exactly).
//
// The pivot is the centre of the selection's world bounds (rounded to 0.1 mm), or one model's
// origin when it is the whole selection. Each frame the op is built as for the commit and every
// object's transform comes from MapEditPreviewTransform (the same math mapedit.c writes with), so
// the preview shows what the commit writes. Objects that can't follow (stairs and waffles turned
// or scaled, boxes turned about another axis than the vertical, directional lights moved) keep
// their transform and are named in red in the HUD; lights ignore rotate and scale. Mouse values
// are rounded to what the HUD shows (mm, 0.01 degree, 0.001) before they are written.
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
//     vertical axis seen from eye height, the usual case for boxes): the turn follows the motion
//     along the direction the object's near side moves on screen (a turntable: drag right, the
//     side facing you goes right), ROT_DEG_PER_PX, wherever the cursor starts. The screen angle
//     around the pivot says little here (a horizontal drag below a tall pillar's pivot sweeps a
//     few degrees);
//   - the axis points at the camera (the view axis, or looking along a locked axis): a start this
//     far from the pivot's screen position turns by the angle the cursor sweeps around it
//     (Blender's dial); nearer, the sweep is tiny, so the horizontal motion turns it (right =
//     clockwise on screen)
#define ROT_DIAL_PX     80.0f
#define ROT_DEG_PER_PX  0.5
#define ROT_TRACK_MIN_SIN 0.5f      // sin of 30 degrees between the axis and the line of sight
// Boxes turn only in 90 degree steps: the mouse's angle takes the next step once it is this far
// past the last one (not halfway, 45 degrees: a short drag already shows the step)
#define BOX_STEP_AT_DEG 30.0

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

// Axis a of the lock in the world: the model's own (local) or the world's
static Vector3 LockDir(const TransformModal *t, int a)
{
    return t->local && t->hasModel ? t->localAxes[a] : Unit(a);
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
    memset(t, 0, sizeof(*t));
}

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

static void GrabValue(TransformModal *t, Camera3D cam, float snap, bool ctrl)
{
    Vector3 P = t->pivot, fwd = ViewForward(cam);
    double v;
    bool neg;
    int n = BitCount(t->axisMask);
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
                if (t->local && t->hasModel) s = Round(s, snap);
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
            if (ctrl && n == 2 && t->local && t->hasModel) {
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

// The rotation axis in the world (for the view sign and the guide)
static Vector3 RotWorldAxis(const TransformModal *t, Camera3D cam)
{
    int a = LockedAxis(t);
    if (a >= 0) return LockDir(t, a);
    if (t->boxes90) return (Vector3){ 0, 1, 0 };
    return Vector3Negate(ViewForward(cam));
}

// The boxes' angle for a dragged one: a 90 degree step once the drag is BOX_STEP_AT_DEG past
// the last step (30 -> 90, 120 -> 180, ...; the sign of the drag)
static double BoxSteps(double deg)
{
    double q = floor((fabs(deg) + 90.0 - BOX_STEP_AT_DEG) / 90.0);
    return deg < 0.0 ? -90.0 * q : 90.0 * q;
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

static void RotateValue(TransformModal *t, Camera3D cam, bool ctrl)
{
    double v;
    bool neg;
    if (TypedValue(t, &v, &neg)) {
        t->value[0] = t->boxes90 ? Round(v, 90.0) : round(v * 1e6) / 1e6;
        return;
    }
    Vector2 ps = PivotScreen(t, cam), track;
    Vector3 A = RotWorldAxis(t, cam);
    double deg, a;
    if (TrackDirection(t, cam, A, ps, &track)) {
        // the axis across the view: the motion along the near side's screen direction
        deg = Vector2DotProduct(t->mouse, track) * ROT_DEG_PER_PX;
        t->haveAngle = false;
    } else {
        if (Vector2Distance(t->mouseStart, ps) < ROT_DIAL_PX) {
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
    if (neg) deg = -deg;
    t->rawDeg = Round(deg, 0.01);
    t->value[0] = t->boxes90 ? BoxSteps(deg) : ctrl ? Round(deg, 15.0) : Round(deg, 0.01);
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
        int a = LockedAxis(t);
        bool local = t->local && t->hasModel;
        if ((a == 1 && !local) || (a < 0 && t->boxes90)) op->degrees = t->value[0];    // a yaw (exact for models)
        else if (a >= 0) {
            op->axis[a] = 1.0;
            op->angle = t->value[0];
            op->local = local;
        } else {
            Vector3 A = RotWorldAxis(t, cam);
            op->axis[0] = A.x;
            op->axis[1] = A.y;
            op->axis[2] = A.z;
            op->angle = t->value[0];
        }
    } else {
        op->kind = EDIT_SCALE;
        op->local = t->local && t->hasModel;
        memcpy(op->factor, t->value, sizeof(op->factor));
    }
}

static bool Identity(const TransformModal *t)
{
    if (t->kind == TRANSFORM_GRAB) return t->value[0] == 0.0 && t->value[1] == 0.0 && t->value[2] == 0.0;
    if (t->kind == TRANSFORM_ROTATE) return t->value[0] == 0.0;
    return t->value[0] == 1.0 && t->value[1] == 1.0 && t->value[2] == 1.0;
}

// Which objects follow the kind and lock, the refusal text, boxes90
static void Classify(TransformModal *t)
{
    int a = LockedAxis(t);
    bool boxAxis = a < 0 || a == 1;         // boxes turn about the vertical only
    int boxesIn = 0, refusedOther = 0, refusedBoxes = 0, refusedDir = 0;
    for (int i = 0; i < t->count; i++) {
        int c = t->cls[i];
        bool box = c == EDIT_CLASS_BOX || c == EDIT_CLASS_BLOCK, f = false;
        if (t->kind == TRANSFORM_GRAB) {
            f = c != EDIT_CLASS_DIR_LIGHT && c != EDIT_CLASS_NONE;
            refusedDir += c == EDIT_CLASS_DIR_LIGHT;
        } else if (t->kind == TRANSFORM_ROTATE) {
            f = c == EDIT_CLASS_MODEL || (box && boxAxis);
            refusedBoxes += box && !boxAxis;
            refusedOther += c == EDIT_CLASS_OTHER_BOX;
            boxesIn += box && boxAxis;
        } else {
            f = c == EDIT_CLASS_MODEL || box;
            refusedOther += c == EDIT_CLASS_OTHER_BOX;
        }
        t->follows[i] = f;
    }
    t->boxes90 = t->kind == TRANSFORM_ROTATE && boxesIn > 0;
    t->refusal[0] = '\0';
    const char *verb = t->kind == TRANSFORM_ROTATE ? "turn" : "scale";
    if (refusedOther)
        snprintf(t->refusal, sizeof(t->refusal), "%d box%s from stairs / waffle lines can't %s (they stay)", refusedOther,
                 refusedOther == 1 ? "" : "es", verb);
    if (refusedBoxes) {
        size_t l = strlen(t->refusal);
        snprintf(t->refusal + l, sizeof(t->refusal) - l, "%s%d box%s can't turn about %s: boxes turn only about Y, in 90 deg steps",
                 l ? "; " : "", refusedBoxes, refusedBoxes == 1 ? "" : "es", t->local && t->hasModel ? "a model axis" : "this axis");
    }
    if (refusedDir) {
        size_t l = strlen(t->refusal);
        snprintf(t->refusal + l, sizeof(t->refusal) - l, "%s%d directional light%s %s no position (%s)", l ? "; " : "", refusedDir,
                 refusedDir == 1 ? "" : "s", refusedDir == 1 ? "has" : "have", refusedDir == 1 ? "it stays" : "they stay");
    }
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
    size_t l = strlen(t->refusal);
    int box = -1, boxes = 0;
    for (int i = 0; i < t->count; i++)
        if (t->follows[i] && (t->cls[i] == EDIT_CLASS_BOX || t->cls[i] == EDIT_CLASS_BLOCK)) { boxes++; if (box < 0) box = i; }
    if (t->kind == TRANSFORM_ROTATE && boxes == 1 && t->commitCount == 1) {
        Vector3 sz = t->start[box].scale;
        snprintf(t->refusal + l, sizeof(t->refusal) - l, "%sthis box looks the same turned %.0f deg (footprint %g x %g m%s): nothing to "
                 "turn; boxes stay axis-aligned. SHIFT+LMB adds neighbours to turn them around their common centre", l ? "; " : "",
                 t->value[0], Round(sz.x, 1e-3), Round(sz.z, 1e-3), fabsf(sz.x - sz.z) < 1e-4f ? ", square" : "");
    } else
        snprintf(t->refusal + l, sizeof(t->refusal) - l, "%severything that follows ends where it started: nothing to commit%s",
                 l ? "; " : "", t->kind == TRANSFORM_ROTATE && boxes > 0 ? " (boxes stay axis-aligned: a square footprint turned 90 "
                 "deg or any box turned 180 is the same box)" : "");
}

static void Compute(TransformModal *t, Designer *d, Camera3D cam, bool ctrl)
{
    t->cam = cam;
    t->snapping = ctrl;
    t->snapStep = DesignerSnap(d);
    Classify(t);
    if (t->kind == TRANSFORM_GRAB) GrabValue(t, cam, t->snapStep, ctrl);
    else if (t->kind == TRANSFORM_ROTATE) RotateValue(t, cam, ctrl);
    else {
        ScaleValue(t, cam, ctrl);
        if (t->value[0] <= 0.0 || t->value[1] <= 0.0 || t->value[2] <= 0.0) {
            size_t l = strlen(t->refusal);
            snprintf(t->refusal + l, sizeof(t->refusal) - l, "%sa scale factor must be > 0", l ? "; " : "");
        }
    }
    EditOp op;
    BuildOp(t, cam, &op);
    bool changed = false;
    t->commitCount = 0;
    for (int i = 0; i < t->count; i++) {
        MS_Transform tr = t->start[i];
        if (t->follows[i]) {
            if (MapEditPreviewTransform(&op, t->cls[i], &t->start[i], &tr)) t->commit[t->commitCount++] = t->objs[i];
            else t->follows[i] = 0;
        }
        if (memcmp(&tr, &t->now[i], sizeof(tr)) != 0) changed = true;
        t->now[i] = tr;
    }
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
    t->axisMask = 0;
    t->local = false;
    t->typed[0] = '\0';
    t->cancelNote[0] = '\0';
    t->noChange = false;
    t->haveAngle = false;
    t->sweep = t->rawDeg = 0.0;
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
    BoundingBox bb = { { INFINITY, INFINITY, INFINITY }, { -INFINITY, -INFINITY, -INFINITY } };
    t->hasModel = false;
    for (int i = 0; i < n; i++) {
        int e = MS_ObjectEntity(scene, t->objs[i]);
        t->start[i] = e >= 0 ? scene->entityTransforms[e] : (MS_Transform){ 0 };
        t->now[i] = t->start[i];
        if (e < 0) t->cls[i] = EDIT_CLASS_NONE;
        if (t->cls[i] != EDIT_CLASS_DIR_LIGHT && e >= 0) {
            BoundingBox b = MS_ObjectBounds(scene, t->objs[i]);
            bb.min = Vector3Min(bb.min, b.min);
            bb.max = Vector3Max(bb.max, b.max);
        }
        if (t->cls[i] == EDIT_CLASS_MODEL && !t->hasModel) {
            // the first model's axes (R = Ry Rx Rz, as the runtime turns it): local locks
            Vector3 r = t->start[i].rotationDeg;
            Quaternion q = QuaternionMultiply(QuaternionMultiply(QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, r.x * DEG2RAD),
                                                                 QuaternionFromAxisAngle((Vector3){ 1, 0, 0 }, r.y * DEG2RAD)),
                                              QuaternionFromAxisAngle((Vector3){ 0, 0, 1 }, r.z * DEG2RAD));
            for (int a = 0; a < 3; a++) t->localAxes[a] = Vector3RotateByQuaternion(Unit(a), q);
            t->hasModel = true;
        }
    }
    if (n == 1 && t->objs[0].kind == MS_OBJ_MODEL) {
        double p[3];
        MapEditAnchor(t->objs[0], p);       // (its position, rounded to 0.1 mm: the written decimal)
        t->pivot = (Vector3){ (float)p[0], (float)p[1], (float)p[2] };
    } else if (bb.min.x <= bb.max.x) {
        Vector3 c = Vector3Scale(Vector3Add(bb.min, bb.max), 0.5f);
        t->pivot = (Vector3){ (float)Round(c.x, 1e-4), (float)Round(c.y, 1e-4), (float)Round(c.z, 1e-4) };
    } else t->pivot = n > 0 ? t->start[0].position : (Vector3){ 0 };
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
}

static void Cancel(TransformModal *t, Designer *d)
{
    if (t->previewing) {
        MS_EditPreviewEnd(scene);
        DesignerAdoptVersion(d);
    }
    t->previewing = false;
    t->kind = TRANSFORM_NONE;
}

// LMB / ENTER with nothing to write (no value, nothing follows, or a value that changes
// nothing): everything back, cancelNote says why when there was a value
static void CancelNothing(TransformModal *t, Designer *d)
{
    t->cancelNote[0] = '\0';
    if (t->noChange)
        snprintf(t->cancelNote, sizeof(t->cancelNote), "nothing written: %s", t->kind == TRANSFORM_ROTATE && t->boxes90
                 ? "turned, the boxes are the same boxes (a square footprint turned 90 deg, or any box turned 180)"
                 : "every object would end where it started");
    else if (!Identity(t) && t->commitCount == 0) snprintf(t->cancelNote, sizeof(t->cancelNote), "nothing written: nothing selected can follow");
    Cancel(t, d);
}

// X / Y / Z (plane: SHIFT): lock, the same again local (a model is selected), a third time none
static void LockKey(TransformModal *t, int axis, bool plane)
{
    int mask = plane ? (7 & ~(1 << axis)) : 1 << axis;
    if (t->axisMask == mask) {
        if (!t->local && t->hasModel) t->local = true;
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
    static const int KIND_KEYS[3] = { KEY_G, KEY_R, KEY_S };
    for (int k = 0; k < 3; k++)
        if (IsKeyPressed(KIND_KEYS[k]) && !ctrl) {
            // A new kind from the original transforms: the cursor where it is now is the new start
            t->kind = (TransformKind)(TRANSFORM_GRAB + k);
            ResetValues(t);
            t->mouseStart = Cursor(t);
            t->mouse = (Vector2){ 0, 0 };
        }
    static const int AXIS_KEYS[3] = { KEY_X, KEY_Y, KEY_Z };
    for (int a = 0; a < 3; a++)
        if (IsKeyPressed(AXIS_KEYS[a])) {
            LockKey(t, a, shift);
            t->haveAngle = false;   // (the sweep starts again for the new axis)
            t->mouseStart = Cursor(t);
            t->mouse = (Vector2){ 0, 0 };
        }
    if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && t->typed[0]) t->typed[strlen(t->typed) - 1] = '\0';
    for (int c = GetCharPressed(); c; c = GetCharPressed()) TypeChar(t, c);
    Compute(t, d, cam, ctrl);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        BuildOp(t, cam, op);
        if (!Identity(t) && t->commitCount > 0 && !t->noChange) return TRANSFORM_COMMIT;
        CancelNothing(t, d);
        return TRANSFORM_CANCELLED;
    }
    return TRANSFORM_RUNNING;
}

bool TransformOpenScripted(TransformModal *t, Designer *d, Camera3D cam, const char *spec, char *msg, int msgSize)
{
    char words[4][32] = { { 0 } };
    int n = 0;
    for (const char *p = spec; *p && n < 4; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        int l = 0;
        while (*p && *p != ' ') { if (l < 31) words[n][l++] = *p; p++; }
        words[n++][l] = '\0';
    }
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
        } else
            for (const char *q = w; *q; q++) TypeChar(t, *q);
    }
    Compute(t, d, cam, false);
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
// #region Drawing
// ======================================================================================

static Color AxisColor(int a) { return a == 0 ? COLOR_X : a == 1 ? COLOR_Y : COLOR_Z; }

void TransformDrawGuides(const TransformModal *t, Camera3D cam)
{
    if (t->kind == TRANSFORM_NONE) return;
    BeginMode3D(cam);
    rlDisableDepthTest();
    Vector3 P = t->pivot;
    // a long line through the pivot along each locked axis (rotate: the axis turned about)
    for (int a = 0; a < 3; a++) {
        bool on = t->kind == TRANSFORM_ROTATE ? LockedAxis(t) == a : (t->axisMask & (1 << a)) != 0;
        if (t->kind == TRANSFORM_ROTATE && LockedAxis(t) < 0 && t->boxes90 && a == 1) on = true;
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
    bool local = t->local && t->hasModel;
    int n = BitCount(t->axisMask), a = LockedAxis(t);
    if (t->kind == TRANSFORM_ROTATE) {
        const char *about = a >= 0 ? TextFormat("%s%c", local ? "local " : "", AXIS_NAME[a]) : t->boxes90 ? "Y" : "the view axis";
        if (t->boxes90 && !t->typed[0] && t->rawDeg != t->value[0])
            snprintf(out, size, "%.0f deg about %s (dragged %.1f; boxes: the next 90 at %.0f past one)", t->value[0], about, t->rawDeg,
                     BOX_STEP_AT_DEG);
        else snprintf(out, size, "%.2f deg about %s", t->value[0], about);
        return;
    }
    char part[3][48];
    int parts = 0;
    for (int k = 0; k < 3; k++) {
        if (n > 0 && !(t->axisMask & (1 << k))) continue;
        double v = t->value[k];
        if (t->kind == TRANSFORM_GRAB && local)     // the amount along the model's axis
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
    const char *space = n == 0 ? (t->kind != TRANSFORM_ROTATE ? "free" : t->boxes90 ? "global" : "view")
                      : t->local && t->hasModel ? "local" : "global";
    const char *snap;
    if (t->boxes90) snap = "boxes turn in 90 deg steps";
    else if (t->snapping)
        snap = t->kind == TRANSFORM_GRAB ? TextFormat("^snap %g m^", t->snapStep) : t->kind == TRANSFORM_ROTATE ? "^snap 15 deg^" : "^snap 0.1^";
    else snap = "snap off";
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
