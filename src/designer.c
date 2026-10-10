#include "designer.h"
#include "map.h"
#include <math.h>

// ======================================================================================
// #region Picking
// ======================================================================================

DesignerHit DesignerPick(Vector3 origin, Vector3 dir)
{
    DesignerHit best = { .box = -1, .dist = INFINITY };
    const float o[3] = { origin.x, origin.y, origin.z };
    const float d[3] = { dir.x, dir.y, dir.z };

    for (int i = 0; i < world.count; i++) {
        const BoundingBox *b = &world.items[i];
        const float mn[3] = { b->min.x, b->min.y, b->min.z };
        const float mx[3] = { b->max.x, b->max.y, b->max.z };
        float tNear = -INFINITY, tFar = INFINITY;
        int axis = -1;
        bool miss = false;
        for (int a = 0; a < 3 && !miss; a++) {
            if (fabsf(d[a]) < 1e-12f) {
                // Parallel to this slab: inside it or never
                if (o[a] < mn[a] || o[a] > mx[a]) miss = true;
                continue;
            }
            float t1 = (mn[a] - o[a]) / d[a], t2 = (mx[a] - o[a]) / d[a];
            if (t1 > t2) { float t = t1; t1 = t2; t2 = t; }
            if (t1 > tNear) { tNear = t1; axis = a; }
            if (t2 < tFar) tFar = t2;
            if (tNear > tFar) miss = true;
        }
        // tNear < 0: the eye is inside this box. Skip it, so flying through a wall
        // still lets you pick what is beyond.
        if (miss || axis < 0 || tNear < 0.0f || tNear >= best.dist) continue;

        best.box = i;
        best.dist = tNear;
        best.point = Vector3Add(origin, Vector3Scale(dir, tNear));
        float n[3] = { 0, 0, 0 };
        n[axis] = d[axis] > 0.0f ? -1.0f : 1.0f;
        best.normal = (Vector3){ n[0], n[1], n[2] };
    }
    if (best.box < 0) best.dist = 0.0f;
    return best;
}

void DesignerPrintHit(DesignerHit hit)
{
    if (hit.box < 0) {
        printf("pick: no hit\n");
        fflush(stdout);
        return;
    }
    BoxRef r = world.refs[hit.box];
    BoundingBox b = world.items[hit.box];
    char text[MAP_LINE_MAX + 1] = "";
    if (r.line > 0) MapGetLine(r.line, text, sizeof(text));
    printf("pick: box %d  group \"%s\"  line %d: %s\n"
           "pick: point %.3f %.3f %.3f  normal %.0f %.0f %.0f  dist %.3f\n"
           "pick: box min %.3f %.3f %.3f  max %.3f %.3f %.3f\n",
           hit.box, r.group >= 0 ? world.groups[r.group] : "", r.line, text,
           hit.point.x, hit.point.y, hit.point.z, hit.normal.x, hit.normal.y, hit.normal.z, hit.dist,
           b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
    fflush(stdout);
}

// #endregion

// ======================================================================================
// #region Selection
// ======================================================================================

void DesignerMessage(Designer *d, bool error, const char *text)
{
    snprintf(d->message, sizeof(d->message), "%s", text);
    d->messageTime = error ? 8.0f : 4.0f;
    d->messageError = error;
}

static int SelectionIndexOf(const Designer *d, int box)
{
    for (int i = 0; i < d->selectedCount; i++)
        if (d->selected[i] == box) return i;
    return -1;
}

static DesignerSelKey MakeKey(int box)
{
    DesignerSelKey k = { .box = world.items[box], .line = world.refs[box].line, .group = world.refs[box].group };
    for (int j = 0; j < box; j++)
        if (world.refs[j].line == k.line) k.nth++;
    if (k.group >= 0) snprintf(k.groupName, sizeof(k.groupName), "%s", world.groups[k.group]);
    return k;
}

static bool SelectionAdd(Designer *d, int box)
{
    if (SelectionIndexOf(d, box) >= 0) return true;
    if (d->selectedCount >= MAX_SELECTION) {
        DesignerMessage(d, true, TextFormat("selection full (%d boxes)", MAX_SELECTION));
        return false;
    }
    d->keys[d->selectedCount] = MakeKey(box);
    d->selected[d->selectedCount++] = box;
    return true;
}

static void SelectionRemoveAt(Designer *d, int at)
{
    d->selectedCount--;
    memmove(&d->selected[at], &d->selected[at + 1], (d->selectedCount - at) * sizeof(d->selected[0]));
    memmove(&d->keys[at], &d->keys[at + 1], (d->selectedCount - at) * sizeof(d->keys[0]));
}

void DesignerClearSelection(Designer *d)
{
    d->selectedCount = 0;
}

void DesignerSelectLine(Designer *d, int line)
{
    for (int i = 0; i < world.count; i++)
        if (world.refs[i].line == line && !SelectionAdd(d, i)) break;
}

// The box's whole group (a box without a group stands alone)
static void SelectGroupOf(Designer *d, int box)
{
    int g = world.refs[box].group;
    if (g < 0) { SelectionAdd(d, box); return; }
    for (int i = 0; i < world.count; i++)
        if (world.refs[i].group == g && !SelectionAdd(d, i)) break;
}

static bool SameBox(BoundingBox a, BoundingBox b)
{
    return a.min.x == b.min.x && a.min.y == b.min.y && a.min.z == b.min.z &&
           a.max.x == b.max.x && a.max.y == b.max.y && a.max.z == b.max.z;
}

// After a map reload the box indices mean nothing. Remap each selected box:
//  1. a box with exactly the same min/max (unchanged geometry, even if lines moved),
//     preferring one from the same line;
//  2. else the box with the same index within the same map line, if that line still
//     belongs to the same group (the usual "this pillar got taller" edit).
// Anything else is dropped and the HUD says how many were lost.
void DesignerSync(Designer *d)
{
    if (d->version == world.version) return;
    bool first = d->version == 0;
    d->version = world.version;
    d->hit.box = -1;                // re-picked on the next DesignerUpdate
    if (first || d->selectedCount == 0) { d->selectedCount = 0; return; }

    static DesignerSelKey old[MAX_SELECTION];
    int oldCount = d->selectedCount;
    memcpy(old, d->keys, oldCount * sizeof(old[0]));
    d->selectedCount = 0;

    for (int s = 0; s < oldCount; s++) {
        const DesignerSelKey *k = &old[s];
        int found = -1;
        for (int i = 0; i < world.count; i++) {
            if (!SameBox(world.items[i], k->box) || SelectionIndexOf(d, i) >= 0) continue;
            if (found < 0 || world.refs[i].line == k->line) found = i;
            if (world.refs[i].line == k->line) break;
        }
        if (found < 0) {
            int nth = 0;
            for (int i = 0; i < world.count; i++) {
                if (world.refs[i].line != k->line) continue;
                if (nth++ != k->nth) continue;
                int g = world.refs[i].group;
                const char *name = g >= 0 ? world.groups[g] : "";
                if (strcmp(name, k->groupName) == 0 && SelectionIndexOf(d, i) < 0) found = i;
                break;
            }
        }
        if (found >= 0) SelectionAdd(d, found);
    }

    int lost = oldCount - d->selectedCount;
    if (lost > 0)
        DesignerMessage(d, false, TextFormat("map reloaded: %d of %d selected boxes no longer exist", lost, oldCount));
}

// #endregion

// ======================================================================================
// #region Prompt text: editing and word wrap
// ======================================================================================

#define PROMPT_FONT     20
#define PROMPT_SPACING  2.0f
#define PROMPT_MARGIN   12      // box to screen edge
#define PROMPT_PAD      10      // text to box edge
#define PROMPT_LINE_H   24
#define PROMPT_HEADER_H 24
#define PROMPT_MIN_LINES 3
#define PROMPT_MAX_LINES 10

typedef struct { int start, end; } PromptLine;     // byte range, end excludes the '\n'
static PromptLine promptLines[PROMPT_MAX + 1];

static bool IsCont(unsigned char c) { return (c & 0xC0) == 0x80; }

static int PrevBoundary(const char *s, int pos)
{
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && IsCont((unsigned char)s[pos])) pos--;
    return pos;
}

static int NextBoundary(const char *s, int len, int pos)
{
    if (pos >= len) return len;
    pos++;
    while (pos < len && IsCont((unsigned char)s[pos])) pos++;
    return pos;
}

// Width of one codepoint as DrawTextEx advances it (glyph + spacing)
static float GlyphAdvance(int cp)
{
    Font f = GetFontDefault();
    float scale = (float)PROMPT_FONT / (float)f.baseSize;
    int idx = GetGlyphIndex(f, cp);
    float adv = f.glyphs[idx].advanceX ? (float)f.glyphs[idx].advanceX : f.recs[idx].width;
    return adv * scale + PROMPT_SPACING;
}

// Width of s[from, to)
static float TextSpanWidth(const char *s, int from, int to)
{
    float w = 0.0f;
    for (int p = from; p < to; ) {
        int n = 1;
        int cp = GetCodepointNext(s + p, &n);
        w += GlyphAdvance(cp);
        p += n > 0 ? n : 1;
    }
    return w;
}

// Word wrap into promptLines; returns the line count (at least 1)
static int PromptLayout(const Designer *d, float maxW)
{
    const char *s = d->prompt;
    int len = d->promptLen, count = 0, pos = 0;
    for (;;) {
        int start = pos, p = pos, lastBreak = -1;
        float w = 0.0f;
        for (;;) {
            if (p >= len) {
                promptLines[count++] = (PromptLine){ start, len };
                return count;
            }
            if (s[p] == '\n') {
                promptLines[count++] = (PromptLine){ start, p };
                pos = p + 1;
                break;
            }
            int n = 1;
            int cp = GetCodepointNext(s + p, &n);
            if (n <= 0) n = 1;
            float a = GlyphAdvance(cp);
            if (w + a > maxW && p > start && cp != ' ') {
                // Break after the last space on this line, or mid-word if there is none
                int end = lastBreak > start ? lastBreak : p;
                promptLines[count++] = (PromptLine){ start, end };
                pos = end;
                break;
            }
            w += a;
            p += n;
            if (cp == ' ') lastBreak = p;
        }
    }
}

// Wrapped line that holds the cursor: the last one starting at or before it, so a
// cursor at a soft wrap sits at the start of the next line
static int CursorLine(int lineCount, int cursor)
{
    int line = 0;
    for (int i = 0; i < lineCount; i++)
        if (promptLines[i].start <= cursor) line = i;
    return line;
}

// Last cursor position on wrapped line `line`. A soft-wrapped line ends where the next
// one begins, and that offset displays on the next line, so stop one codepoint earlier.
static int LineEnd(const Designer *d, int line)
{
    PromptLine l = promptLines[line];
    if (l.end > l.start && l.end < d->promptLen && d->prompt[l.end] != '\n')
        return PrevBoundary(d->prompt, l.end);
    return l.end;
}

// Byte offset within line `line` closest to x pixels from the line start
static int OffsetAtX(const Designer *d, int line, float x)
{
    PromptLine l = promptLines[line];
    int best = l.start, last = LineEnd(d, line);
    float w = 0.0f, bestErr = fabsf(x);
    for (int p = l.start; p < last; ) {
        int n = 1;
        int cp = GetCodepointNext(d->prompt + p, &n);
        w += GlyphAdvance(cp);
        p += n > 0 ? n : 1;
        if (fabsf(w - x) < bestErr) { bestErr = fabsf(w - x); best = p; }
    }
    return best;
}

static float PromptTextWidth(int screenW)
{
    return (float)(screenW - 2 * PROMPT_MARGIN - 2 * PROMPT_PAD);
}

// Inserts n bytes at the cursor, truncated to the free space at a codepoint boundary
static void PromptInsert(Designer *d, const char *bytes, int n)
{
    int room = PROMPT_MAX - 1 - d->promptLen;
    if (n > room) {
        n = room;
        while (n > 0 && IsCont((unsigned char)bytes[n])) n--;
    }
    if (n <= 0) return;
    memmove(d->prompt + d->cursor + n, d->prompt + d->cursor, d->promptLen - d->cursor + 1);
    memcpy(d->prompt + d->cursor, bytes, n);
    d->promptLen += n;
    d->cursor += n;
}

static void PromptDeleteRange(Designer *d, int from, int to)
{
    if (to <= from) return;
    memmove(d->prompt + from, d->prompt + to, d->promptLen - to + 1);
    d->promptLen -= to - from;
    d->cursor = from;
}

// Clipboard text: CR dropped, tabs -> space, other control characters dropped
static void PromptPaste(Designer *d)
{
    const char *clip = GetClipboardText();
    if (!clip) return;
    static char buf[PROMPT_MAX];
    int n = 0;
    for (const char *c = clip; *c && n < PROMPT_MAX - 1; c++) {
        unsigned char ch = (unsigned char)*c;
        if (ch == '\r') continue;
        if (ch == '\t') ch = ' ';
        if (ch < 32 && ch != '\n') continue;
        if (ch == 127) continue;
        buf[n++] = (char)ch;
    }
    int before = d->promptLen;
    PromptInsert(d, buf, n);
    if (d->promptLen - before < n) DesignerMessage(d, true, TextFormat("prompt limit: %d characters", PROMPT_MAX - 1));
}

static bool PromptBlank(const Designer *d)
{
    for (int i = 0; i < d->promptLen; i++)
        if (d->prompt[i] != ' ' && d->prompt[i] != '\n') return false;
    return true;
}

void DesignerOpenPrompt(Designer *d, const char *text)
{
    if (text) {
        // Cut to the limit without splitting a codepoint (same rule as PromptInsert)
        int n = (int)strlen(text);
        if (n > PROMPT_MAX - 1) {
            n = PROMPT_MAX - 1;
            while (n > 0 && IsCont((unsigned char)text[n])) n--;
        }
        memcpy(d->prompt, text, n);
        d->prompt[n] = 0;
        d->promptLen = d->cursor = n;
        d->scroll = 0;
    }
    d->promptOpen = true;
    d->wantX = -1.0f;
    if (d->interactive) EnableCursor();
}

static void ClosePrompt(Designer *d, bool clear)
{
    d->promptOpen = false;
    if (clear) {
        d->prompt[0] = 0;
        d->promptLen = d->cursor = d->scroll = 0;
    }
    // GLFW can report one big mouse jump when the cursor is captured again
    d->freezeFrames = 2;
    if (d->interactive) DisableCursor();
}

void DesignerSubmitDone(Designer *d, bool ok, const char *msg)
{
    DesignerMessage(d, !ok, msg);
    if (ok) ClosePrompt(d, true);
}

static bool KeyRepeat(int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); }

static DesignerAction PromptInput(Designer *d)
{
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    if (IsKeyPressed(KEY_ESCAPE)) {
        ClosePrompt(d, true);
        return DESIGNER_NONE;
    }
    // Plain Enter only on the press, never on key repeat, so a held Enter can't submit
    if (shift && (KeyRepeat(KEY_ENTER) || KeyRepeat(KEY_KP_ENTER))) {
        PromptInsert(d, "\n", 1);
        d->wantX = -1.0f;
    } else if (!shift && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))) {
        if (PromptBlank(d)) DesignerMessage(d, true, "type a request first (Esc closes the box)");
        else return DESIGNER_SUBMIT;
    }

    // Typed text (Ctrl+letter produces no characters on GLFW, so Ctrl+V doesn't type a 'v')
    int before = d->promptLen, typed = 0;
    for (int cp = GetCharPressed(); cp > 0; cp = GetCharPressed()) {
        if (cp < 32 || cp == 127) continue;
        int n = 0;
        const char *utf8 = CodepointToUTF8(cp, &n);
        PromptInsert(d, utf8, n);
        typed += n;
        d->wantX = -1.0f;
    }
    if (typed > d->promptLen - before) DesignerMessage(d, true, TextFormat("prompt limit: %d characters", PROMPT_MAX - 1));

    if (ctrl && KeyRepeat(KEY_V)) { PromptPaste(d); d->wantX = -1.0f; }

    if (KeyRepeat(KEY_BACKSPACE) && d->cursor > 0) {
        int from = PrevBoundary(d->prompt, d->cursor);
        if (ctrl) {
            // Whole word: spaces, then the word before them
            from = d->cursor;
            while (from > 0 && (d->prompt[from - 1] == ' ' || d->prompt[from - 1] == '\n')) from--;
            while (from > 0 && d->prompt[from - 1] != ' ' && d->prompt[from - 1] != '\n') from--;
        }
        PromptDeleteRange(d, from, d->cursor);
        d->wantX = -1.0f;
    }
    if (KeyRepeat(KEY_DELETE) && d->cursor < d->promptLen) {
        int at = d->cursor;
        PromptDeleteRange(d, at, NextBoundary(d->prompt, d->promptLen, at));
        d->wantX = -1.0f;
    }
    if (KeyRepeat(KEY_LEFT))  { d->cursor = PrevBoundary(d->prompt, d->cursor); d->wantX = -1.0f; }
    if (KeyRepeat(KEY_RIGHT)) { d->cursor = NextBoundary(d->prompt, d->promptLen, d->cursor); d->wantX = -1.0f; }

    bool home = KeyRepeat(KEY_HOME), end = KeyRepeat(KEY_END);
    bool up = KeyRepeat(KEY_UP), down = KeyRepeat(KEY_DOWN);
    if (home || end || up || down) {
        if (ctrl && home) d->cursor = 0;
        else if (ctrl && end) d->cursor = d->promptLen;
        else {
            int lines = PromptLayout(d, PromptTextWidth(GetScreenWidth()));
            int line = CursorLine(lines, d->cursor);
            if (home) d->cursor = promptLines[line].start;
            if (end) d->cursor = LineEnd(d, line);
            if (up || down) {
                if (d->wantX < 0.0f) d->wantX = TextSpanWidth(d->prompt, promptLines[line].start, d->cursor);
                int target = line + (down ? 1 : -1);
                if (target < 0) d->cursor = 0;
                else if (target >= lines) d->cursor = d->promptLen;
                else d->cursor = OffsetAtX(d, target, d->wantX);
            }
        }
        if (!(up || down)) d->wantX = -1.0f;
    }
    return DESIGNER_NONE;
}

// #endregion

// ======================================================================================
// #region Update
// ======================================================================================

bool DesignerCapturesInput(const Designer *d)
{
    return d->promptOpen || d->freezeFrames > 0;
}

void DesignerPickCamera(Designer *d, Camera3D cam)
{
    d->hit = DesignerPick(cam.position, Vector3Normalize(Vector3Subtract(cam.target, cam.position)));
}

DesignerAction DesignerUpdate(Designer *d, float dt)
{
    if (d->messageTime > 0.0f) d->messageTime -= dt;
    bool frozen = d->freezeFrames > 0;
    if (d->freezeFrames > 0) d->freezeFrames--;

    if (d->promptOpen) return PromptInput(d);
    if (frozen) return DESIGNER_NONE;   // the frame(s) right after the box closed

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        DesignerOpenPrompt(d, NULL);
        return DESIGNER_NONE;
    }

    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) DesignerClearSelection(d);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && d->hit.box >= 0) {
        int box = d->hit.box;
        if (ctrl) {
            // Ctrl: the whole group (Ctrl+Shift adds it to the selection)
            if (!shift) DesignerClearSelection(d);
            SelectGroupOf(d, box);
        } else if (shift) {
            int at = SelectionIndexOf(d, box);
            if (at >= 0) SelectionRemoveAt(d, at);
            else SelectionAdd(d, box);
        } else {
            DesignerClearSelection(d);
            SelectionAdd(d, box);
        }
    }
    return DESIGNER_NONE;
}

void DesignerBuildRequest(const Designer *d, Camera3D cam, float yaw, float pitch,
                          int width, int height, DesignRequest *req)
{
    memset(req, 0, sizeof(*req));
    snprintf(req->prompt, sizeof(req->prompt), "%s", d->prompt);
    req->eye = cam.position;
    req->yaw = yaw;
    req->pitch = pitch;
    req->fovy = cam.fovy;
    req->width = width;
    req->height = height;
    req->hit = d->hit;
    req->selectedCount = d->selectedCount;
    memcpy(req->selected, d->selected, d->selectedCount * sizeof(req->selected[0]));
    req->mapVersion = world.version;
}

// #endregion

// ======================================================================================
// #region Drawing
// ======================================================================================

#define SEL_COLOR   (Color){ 255, 160, 40, 255 }
#define HIT_COLOR   (Color){ 90, 220, 255, 255 }

// Distance from the eye to the nearest point of a box (0 inside)
static float BoxDistance(BoundingBox b, Vector3 p)
{
    Vector3 c = { Clamp(p.x, b.min.x, b.max.x), Clamp(p.y, b.min.y, b.max.y), Clamp(p.z, b.min.z, b.max.z) };
    return Vector3Distance(c, p);
}

// The box grown a little, so its outline sits just in front of the faces it outlines.
// Near the camera that is what keeps the lines from z-fighting; beyond ~60 m the depth
// bias in DesignerDrawOverlay is larger and does it (so the inflation stops growing).
static void InflatedBox(BoundingBox b, Vector3 eye, Vector3 *center, Vector3 *size)
{
    float e = fminf(0.004f + 0.0015f * BoxDistance(b, eye), 0.1f);
    *center = Vector3Scale(Vector3Add(b.min, b.max), 0.5f);
    *size = Vector3Add(Vector3Subtract(b.max, b.min), (Vector3){ 2 * e, 2 * e, 2 * e });
}

// A small square on the hit face around the hit point, with its normal as a short tick
static void DrawHitMarker(DesignerHit hit, Vector3 eye, Color c)
{
    float dist = Vector3Distance(eye, hit.point);
    float s = 0.012f * dist + 0.02f;
    Vector3 n = hit.normal;
    // Two axes spanning the face
    Vector3 u = fabsf(n.x) > 0.5f ? (Vector3){ 0, 1, 0 } : (Vector3){ 1, 0, 0 };
    Vector3 v = Vector3CrossProduct(n, u);
    Vector3 p = Vector3Add(hit.point, Vector3Scale(n, 0.004f + 0.0015f * dist));
    Vector3 q[4];
    for (int i = 0; i < 4; i++) {
        float a = (i == 0 || i == 3) ? -s : s, b = (i < 2) ? -s : s;
        q[i] = Vector3Add(p, Vector3Add(Vector3Scale(u, a), Vector3Scale(v, b)));
    }
    for (int i = 0; i < 4; i++) DrawLine3D(q[i], q[(i + 1) % 4], c);
    DrawLine3D(Vector3Add(p, Vector3Scale(u, -2 * s)), Vector3Add(p, Vector3Scale(u, 2 * s)), c);
    DrawLine3D(Vector3Add(p, Vector3Scale(v, -2 * s)), Vector3Add(p, Vector3Scale(v, 2 * s)), c);
    DrawLine3D(p, Vector3Add(p, Vector3Scale(n, 4 * s)), c);
}

void DesignerDrawOverlay(const Designer *d, Camera3D cam, bool hover)
{
    if (d->selectedCount == 0 && d->hit.box < 0) return;
    Vector3 eye = cam.position;

    BeginMode3D(cam);
    // R3D_End copies its depth buffer into the target, and BeginMode3D builds the same
    // projection (same fovy, aspect and rlGetCullDistanceNear/Far), so the overlay can be
    // depth-tested against the scene. Shift the overlay's depth 4 steps of the 24-bit depth
    // buffer toward the camera (z_ndc -= bias): a constant step in depth-buffer units is
    // the precision the scene's depth has at every distance, so lines on faces don't
    // z-fight. With the 1 cm near plane one step is z^2 / 168 km, i.e. 0.6 mm at 10 m,
    // 13 cm at 150 m; more bias would let the overlay show through nearby walls far away.
    Matrix proj = rlGetMatrixProjection();
    const float bias = 4.0f / 16777216.0f * 2.0f;
    proj.m2 -= bias * proj.m3;
    proj.m6 -= bias * proj.m7;
    proj.m10 -= bias * proj.m11;
    proj.m14 -= bias * proj.m15;
    rlSetMatrixProjection(proj);

    // Selected boxes: a faint always-on-top outline, so selected boxes behind walls
    // stay findable...
    rlDisableDepthTest();
    for (int i = 0; i < d->selectedCount; i++) {
        Vector3 c, s;
        InflatedBox(world.items[d->selected[i]], eye, &c, &s);
        DrawCubeWiresV(c, s, Fade(SEL_COLOR, 0.22f));
    }
    rlDrawRenderBatchActive();

    // ...then, depth-tested, a translucent tint (no depth writes, so it can't hide the
    // outlines) and a strong outline
    rlEnableDepthTest();
    rlDisableDepthMask();
    for (int i = 0; i < d->selectedCount; i++) {
        Vector3 c, s;
        InflatedBox(world.items[d->selected[i]], eye, &c, &s);
        DrawCubeV(c, s, Fade(SEL_COLOR, 0.16f));
    }
    rlDrawRenderBatchActive();
    for (int i = 0; i < d->selectedCount; i++) {
        Vector3 c, s;
        InflatedBox(world.items[d->selected[i]], eye, &c, &s);
        DrawCubeWiresV(c, s, Fade(SEL_COLOR, 0.95f));
    }

    if (d->hit.box >= 0) {
        // Hover: a subtle white outline (not for view-marked.png, where only the selection
        // and the hit point should stand out)
        if (hover && SelectionIndexOf(d, d->hit.box) < 0) {
            Vector3 c, s;
            InflatedBox(world.items[d->hit.box], eye, &c, &s);
            DrawCubeWiresV(c, s, (Color){ 255, 255, 255, 110 });
        }
        DrawHitMarker(d->hit, eye, HIT_COLOR);
    }
    rlDrawRenderBatchActive();

    // Back to what R3D_End leaves and raylib's 2D drawing expects
    rlEnableDepthMask();
    rlDisableDepthTest();
    EndMode3D();
}

void DesignerDrawCrosshair(int w, int h)
{
    int cx = w / 2, cy = h / 2;
    Color shade = { 0, 0, 0, 120 }, ink = { 240, 240, 235, 220 };
    // Dark rim first so it reads on bright concrete and on dark fog alike
    DrawRectangle(cx - 9, cy - 2, 7, 4, shade);
    DrawRectangle(cx + 3, cy - 2, 7, 4, shade);
    DrawRectangle(cx - 2, cy - 9, 4, 7, shade);
    DrawRectangle(cx - 2, cy + 3, 4, 7, shade);
    DrawRectangle(cx - 8, cy - 1, 5, 2, ink);
    DrawRectangle(cx + 4, cy - 1, 5, 2, ink);
    DrawRectangle(cx - 1, cy - 8, 2, 5, ink);
    DrawRectangle(cx - 1, cy + 4, 2, 5, ink);
    DrawRectangle(cx - 1, cy - 1, 2, 2, ink);
}

// HUD text with a dark drop shadow: these lines are often over bright fog
static void HudText(const char *text, int x, int y, Color c)
{
    DrawText(text, x + 1, y + 1, 16, (Color){ 0, 0, 0, (unsigned char)(c.a / 2) });
    DrawText(text, x, y, 16, c);
}

int DesignerDrawHud(const Designer *d, int x, int y)
{
    Color main = { 230, 230, 230, 170 }, dim = { 230, 230, 230, 115 };
    int y0 = y;

    if (d->selectedCount == 0) {
        HudText("selected: none", x, y, dim);
    } else {
        // Distinct group names, in selection order
        static char text[512];
        int n = snprintf(text, sizeof(text), "selected: %d box%s  [", d->selectedCount, d->selectedCount == 1 ? "" : "es");
        bool seen[MAX_GROUPS] = { false };
        int shown = 0, more = 0;
        bool ungrouped = false;
        for (int i = 0; i < d->selectedCount; i++) {
            int g = world.refs[d->selected[i]].group;
            if (g < 0) { ungrouped = true; continue; }
            if (seen[g]) continue;
            seen[g] = true;
            if (shown < 5 && n < (int)sizeof(text) - 80) {
                n += snprintf(text + n, sizeof(text) - n, "%s%s", shown ? ", " : "", world.groups[g]);
                shown++;
            } else more++;
        }
        if (ungrouped && n < (int)sizeof(text) - 40) n += snprintf(text + n, sizeof(text) - n, "%s(no group)", shown ? ", " : "");
        if (more && n < (int)sizeof(text) - 40) n += snprintf(text + n, sizeof(text) - n, " +%d more", more);
        snprintf(text + n, sizeof(text) - n, "]");
        HudText(text, x, y, Fade(SEL_COLOR, 0.95f));
    }
    y += 20;

    if (d->hit.box >= 0) {
        BoxRef r = world.refs[d->hit.box];
        HudText(TextFormat("crosshair: %s  |  line %d  |  box %d  |  %.1f m",
                r.group >= 0 ? world.groups[r.group] : "(no group)", r.line, d->hit.box, d->hit.dist),
                x, y, main);
        y += 20;
        char line[MAP_LINE_MAX + 1];
        if (r.line > 0 && MapGetLine(r.line, line, sizeof(line))) {
            // Long lines are cut to keep the HUD on screen
            if (strlen(line) > 110) strcpy(line + 107, "...");
            HudText(TextFormat("  %d: %s", r.line, line), x, y, dim);
            y += 20;
        }
    } else {
        HudText("crosshair: sky", x, y, dim);
        y += 20;
    }
    return y - y0;
}

void DesignerDrawPrompt(Designer *d, int w, int h)
{
    int msgY = h - 100;
    if (d->promptOpen) {
        float maxW = PromptTextWidth(w);
        int lines = PromptLayout(d, maxW);
        int cursorLine = CursorLine(lines, d->cursor);
        int visible = lines < PROMPT_MIN_LINES ? PROMPT_MIN_LINES : lines > PROMPT_MAX_LINES ? PROMPT_MAX_LINES : lines;
        if (cursorLine < d->scroll) d->scroll = cursorLine;
        if (cursorLine >= d->scroll + visible) d->scroll = cursorLine - visible + 1;
        if (d->scroll > lines - visible) d->scroll = lines - visible > 0 ? lines - visible : 0;

        int boxH = PROMPT_HEADER_H + visible * PROMPT_LINE_H + 2 * PROMPT_PAD;
        Rectangle box = { PROMPT_MARGIN, (float)(h - PROMPT_MARGIN - boxH), (float)(w - 2 * PROMPT_MARGIN), (float)boxH };
        DrawRectangleRec(box, (Color){ 16, 16, 18, 225 });
        DrawRectangleLinesEx(box, 1.0f, (Color){ 230, 230, 230, 90 });

        int tx = (int)box.x + PROMPT_PAD, ty = (int)box.y + PROMPT_PAD;
        DrawText(TextFormat("Request for Claude   (%d selected%s)   Enter send  |  Shift+Enter new line  |  Ctrl+V paste  |  Esc cancel",
                 d->selectedCount, d->hit.box >= 0 ? " + crosshair point" : ""), tx, ty, 16, (Color){ 230, 230, 230, 140 });
        const char *counter = TextFormat("%d / %d", d->promptLen, PROMPT_MAX - 1);
        Color counterColor = d->promptLen >= PROMPT_MAX - 1 ? (Color){ 255, 110, 90, 230 } : (Color){ 230, 230, 230, 140 };
        DrawText(counter, (int)(box.x + box.width) - PROMPT_PAD - MeasureText(counter, 16), ty, 16, counterColor);
        ty += PROMPT_HEADER_H;

        Font font = GetFontDefault();
        Color ink = { 240, 238, 232, 255 };
        if (d->promptLen == 0) {
            DrawTextEx(font, "Describe the change. The crosshair point and the selected boxes are sent along.",
                       (Vector2){ (float)tx, (float)ty }, PROMPT_FONT, PROMPT_SPACING, (Color){ 240, 238, 232, 90 });
        }
        static char buf[PROMPT_MAX];
        for (int i = d->scroll; i < lines && i < d->scroll + visible; i++) {
            PromptLine l = promptLines[i];
            int n = l.end - l.start;
            memcpy(buf, d->prompt + l.start, n);
            buf[n] = 0;
            int ly = ty + (i - d->scroll) * PROMPT_LINE_H;
            DrawTextEx(font, buf, (Vector2){ (float)tx, (float)ly }, PROMPT_FONT, PROMPT_SPACING, ink);
            if (i == cursorLine && fmod(GetTime(), 1.0) < 0.6) {
                float cx = tx + TextSpanWidth(d->prompt, l.start, d->cursor);
                DrawRectangle((int)cx, ly - 1, 2, PROMPT_FONT + 2, (Color){ 255, 200, 120, 255 });
            }
        }
        // Scroll hints
        if (d->scroll > 0) DrawText("^", (int)(box.x + box.width) - 18, ty, 16, ink);
        if (d->scroll + visible < lines) DrawText("v", (int)(box.x + box.width) - 18, ty + (visible - 1) * PROMPT_LINE_H, 16, ink);
        msgY = (int)box.y - 28;
    }

    if (d->messageTime > 0.0f) {
        float a = fminf(d->messageTime, 1.0f);
        Color c = d->messageError ? (Color){ 255, 110, 90, 255 } : (Color){ 235, 232, 225, 255 };
        int mw = MeasureText(d->message, 20);
        DrawText(d->message, (w - mw) / 2, msgY, 20, Fade(c, a));
    }
}

// #endregion
