// The draw panel (docs/painting.md 13.2): docked at the left edge below the HUD lines, 300 px
// wide, in the ui.h look. From top to bottom: Target (the surface the cursor rested on last, what
// paint there goes into or why it can't be painted), Tools (the 22 tools' grid, the tool's name and
// key, its options: brush, size, hardness, pixel perfect, spray, bucket, gradient, eyedropper;
// docs/painting.md 18.9; on a paint grid also Pixel look, 17.7), Colour (saturation / value square,
// hue and alpha strips, fg / bg, R G B A, the hex field, the recent colours, the palette), Density
// (texels per metre, the texel size there, Resize to density, Pixel look, Clear paint; "Grid" with Fit
// to grid and no slider when the target's material has a paint grid) and Faces (boxes only: the face
// groups of the box's paint).
// The target, its rectangle and its lines come from the draw mode (dm->target); the actions act on it.
//
// One layout function serves three passes: a measure pass (the height, no widget runs), the input
// pass (DrawPanelUpdate, before the draw mode reads the mouse: the widgets change the DrawMode,
// nothing is drawn) and the draw pass (DrawPanelDraw, with the HUD: the widgets draw a copy of the
// DrawMode and read no input). A panel taller than the room below the HUD (a 720p window) scrolls:
// the wheel over it scrolls, except over a slider or a field, which take the wheel themselves.
// Nothing here allocates, except the palette's `+` (dm->palette is a heap array).
#include "drawpanel.h"

#include <math.h>
#include <string.h>

#define PANEL_X     DRAWPANEL_X
#define PANEL_W     DRAWPANEL_W
#define PAD         12      // panel edge to content
#define INNER       (PANEL_W - 2 * PAD)
#define TITLE_H     26
#define HEAD_H      29      // a section's header line (with the rule above it)
#define ROW_H       22      // a slider row
#define ROW_GAP     2
#define FIELD_W     54
#define GAP         6
#define LABEL_W     74      // "Size", "Hardness", "Density"
#define CHANNEL_W   20      // "R", "G", "B", "A"
#define BUTTON_H    28
#define BOTTOM_ROOM 42      // kept free under the panel (the map message line)
#define SCROLL_STEP 48

// Widget ids (unique in the panel, > 0)
enum {
    W_SIZE_S = 1, W_SIZE_F, W_HARD_S, W_HARD_F,
    W_R_S, W_G_S, W_B_S, W_A_S, W_R_F, W_G_F, W_B_F, W_A_F,
    W_HEX, W_DENS_S, W_DENS_F,
    W_SV, W_HUE, W_ALPHA, W_SPRAYW_S, W_SPRAYW_F, W_SPEED_S, W_SPEED_F, W_TOL_S, W_TOL_F,
};

typedef enum { PASS_MEASURE, PASS_INPUT, PASS_DRAW } Pass;

typedef struct {
    DrawPanel *p;
    DrawMode *dm;           // the input pass: the draw mode's; the draw pass: a copy
    Pass pass;
    int x, y, w;            // content left, the next row's top, content width
    DrawPanelAction act;
    bool wheelTaken;        // the mouse is over a widget that takes the wheel
} Ctx;

static const char *FACE_NAMES[6] = { "+x", "-x", "+y", "-y", "+z", "-z" };

static bool Drawing(const Ctx *c) { return c->pass == PASS_DRAW; }
static int ClampI(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// #region Widget calls (skipped by the measure pass)

static void TakeWheel(Ctx *c, Rectangle r)
{
    if (c->pass != PASS_MEASURE && c->p->interactive && CheckCollisionPointRec(GetMousePosition(), r)) c->wheelTaken = true;
}

static bool Slider(Ctx *c, int id, Rectangle r, float *v, float lo, float hi, float step)
{
    TakeWheel(c, r);
    return c->pass != PASS_MEASURE && UiSlider(&c->p->drag, id, r, v, lo, hi, step) && c->pass == PASS_INPUT;
}

static bool Field(Ctx *c, int id, Rectangle r, float *v, float lo, float hi, float step, int decimals)
{
    TakeWheel(c, r);
    return c->pass != PASS_MEASURE && UiNumberField(&c->p->edit, id, r, v, lo, hi, step, decimals) && c->pass == PASS_INPUT;
}

static bool Button(Ctx *c, Rectangle r, const char *label, bool active)
{
    return c->pass != PASS_MEASURE && UiButton(r, label, active) && c->pass == PASS_INPUT;
}

static bool Swatch(Ctx *c, Rectangle r, Color col, bool selected)
{
    return c->pass != PASS_MEASURE && UiSwatch(r, col, selected) && c->pass == PASS_INPUT;
}

// #endregion

// #region Text

static void Text(Ctx *c, const char *s, int x, int y, int size, Color ink)
{
    if (Drawing(c)) UiText(s, x, y, size, ink);
}

// text wrapped at words into lines of the content width (at most maxLines; the last one is cut);
// returns the height used
static int Wrapped(Ctx *c, const char *text, int size, Color ink, int maxLines)
{
    int lh = UiLineHeight(size) + 1, lines = 0;
    char line[256];
    const char *s = text;
    while (*s && lines < maxLines) {
        while (*s == ' ') s++;
        int n = 0, fit = 0;         // bytes in the line, bytes up to the last word that fits
        while (s[n] && n < (int)sizeof(line) - 1) {
            int e = n;
            while (s[e] && s[e] != ' ' && e < (int)sizeof(line) - 1) e++;
            snprintf(line, sizeof(line), "%.*s", e, s);
            if (UiMeasure(line, size) > c->w && fit > 0) break;
            fit = n = e;
            while (s[n] == ' ') n++;
        }
        if (fit == 0) fit = n > 0 ? n : (int)strlen(s);
        snprintf(line, sizeof(line), "%.*s", fit, s);
        Text(c, line, c->x, c->y + lines * lh, size, ink);
        s += fit;
        lines++;
    }
    if (lines == 0) lines = 1;
    return lines * lh;
}

// A section's header: a rule, the name, hints (UiHints markup) at the right
static void Header(Ctx *c, const char *name, const char *hints)
{
    if (Drawing(c)) {
        DrawRectangle(c->x, c->y + 4, c->w, 1, UI_LINE);
        int ty = c->y + 8;
        UiTextBold(name, c->x, ty - 1, UI_M, UI_INK);
        if (hints) UiHints(hints, c->x + c->w - UiHintsWidth(hints, UI_XS), ty + 1, UI_XS, UI_DIM);
    }
    c->y += HEAD_H;
}

// A labelled value row: the label, then where the slider and the number field go
static void Row(Ctx *c, const char *label, int labelW, Color ink, Rectangle *slider, Rectangle *field)
{
    Text(c, label, c->x, c->y + (ROW_H - UiLineHeight(UI_S)) / 2, UI_S, ink);
    *field = (Rectangle){ (float)(c->x + c->w - FIELD_W), (float)(c->y + 1), FIELD_W, ROW_H - 2 };
    float sx = (float)(c->x + labelW);
    *slider = (Rectangle){ sx, (float)c->y, field->x - 2 * GAP - sx, ROW_H };
}

// #endregion

// #region What the panel acts on (dm->target: the surface the cursor rested on last)

static bool TargetSet(const DrawMode *dm) { return dm->target.version != 0; }

// The target is a box's face or a shape's part (its faces / parts are grouped in the Faces section)
static bool TargetBox(const DrawMode *dm)
{
    const MS_SurfaceHit *h = &dm->target.hit;
    return TargetSet(dm) && dm->target.ok && (h->hit.obj.kind == MS_OBJ_BOX || h->hit.obj.kind == MS_OBJ_SHAPE) && h->face >= 0 &&
           h->face < 6;
}

// The target is a shape (its parts' names on the buttons)
static const MS_Shape *TargetShape(const DrawMode *dm)
{
    const MS_SurfaceHit *h = &dm->target.hit;
    return TargetBox(dm) && h->hit.obj.kind == MS_OBJ_SHAPE && h->hit.obj.index < scene->shapeCount ? &scene->shapes[h->hit.obj.index]
                                                                                                    : NULL;
}

// The name on face button f: a box's face, a shape's part
static const char *FaceButtonName(const DrawMode *dm, int f)
{
    const MS_Shape *S = TargetShape(dm);
    const char *n = S ? MS_ShapePartName(S->kind, f) : f < 6 ? FACE_NAMES[f] : NULL;
    return n ? n : "";
}

// The face buttons follow the target box's (shape's) groups; the user's unapplied changes stay until the
// target is another box or shape (and an undo or an apply brings them in step again)
static void SyncFaces(DrawPanel *p, const DrawMode *dm)
{
    static const unsigned char separate[6] = { 0, 1, 2, 3, 4, 5 };
    bool box = TargetBox(dm) && dm->target.facesOk;
    int key = box ? dm->target.faceKey : -1;
    const unsigned char *now = box ? dm->target.groups : separate;
    bool edited = memcmp(p->faces, p->facesLoaded, 6) != 0;
    if (key != p->facesKey || (!edited && memcmp(p->facesLoaded, now, 6) != 0)) {
        memcpy(p->faces, now, 6);
        memcpy(p->facesLoaded, now, 6);
        p->facesKey = key;
    } else if (memcmp(p->faces, now, 6) == 0) memcpy(p->facesLoaded, now, 6);
}

// The pixel look of the target (its atlas's paint_filter, or what its first stroke gives a new one:
// nearest on a paint grid, docs/painting.md 17.7), or of new atlases
static bool PixelLookShown(const DrawMode *dm)
{
    return TargetSet(dm) && dm->target.ok && dm->target.geom.ok ? dm->target.geom.nearest : dm->pixelLook;
}

// The target's paint follows a paint grid (docs/painting.md 17.7): the section is "Grid", no density,
// and Pixel look sits with the brush (one texel is one cell of the surface's pixel art)
static bool TargetGrid(const DrawMode *dm) { return TargetSet(dm) && dm->target.grid > 0.0f && dm->target.gridLineSet; }

// The Pixel look toggle at r: the draw mode toggles what it shows (DrawModePanelAction)
static void PixelLookButton(Ctx *c, Rectangle r)
{
    if (Button(c, r, "Pixel look", PixelLookShown(c->dm))) c->act = DRAWPANEL_ACT_PIXEL_LOOK;
}

// #endregion

// #region Sections

static void Title(Ctx *c)
{
    if (Drawing(c)) {
        // (the pixel title's capitals, ~21 px from ~6 px below the y given, start where the old
        // title's did: 3 px into the title line; the hints sit on their middle)
        UiTitle("Draw", c->x, c->y - 3, UI_INK);
        const char *hints = "{TAB} hide|{ESC} leave";
        UiHints(hints, c->x + c->w - UiHintsWidth(hints, UI_XS), c->y + 14 - UiLineHeight(UI_XS) / 2, UI_XS, UI_DIM);
    }
    c->y += TITLE_H;
}

static void SectionTarget(Ctx *c)
{
    DrawMode *dm = c->dm;
    Header(c, "Target", NULL);
    // What the cursor rested on last (dm->target), and what paint there goes into, or why it can't
    const DrawTarget *t = &dm->target;
    bool none = !TargetSet(dm) || t->hit.hit.obj.kind == MS_OBJ_NONE;
    c->y += Wrapped(c, none ? "point at a box or a model" : MsgText(&t->what), UI_S, none ? UI_DIM : UI_INK, 2);
    if (!none && !t->ok && t->refusalCode >= 2) c->y += Wrapped(c, MsgText(&t->refusal), UI_S, UI_ERROR, 3);    // (a refusal)
    else if (!none && t->intoSet) c->y += Wrapped(c, MsgText(&t->into), UI_S, UI_DIM, 3);
    c->y += GAP;
}

// The brush shape's icon, centred at cx, cy (shapes from the atlas)
static void PenIcon(PenKind k, int cx, int cy, Color ink)
{
    switch (k) {
    case PEN_ROUND:
        UiRoundRect((Rectangle){ (float)(cx - 8), (float)(cy - 8), 16, 16 }, 8, ink, BLANK);
        break;
    case PEN_SQUARE:
        DrawRectangle(cx - 7, cy - 7, 14, 14, ink);
        break;
    case PEN_SOFT:
        UiRoundRect((Rectangle){ (float)(cx - 10), (float)(cy - 10), 20, 20 }, 10, UiAlpha(ink, 0.22f), BLANK);
        UiRoundRect((Rectangle){ (float)(cx - 8), (float)(cy - 8), 16, 16 }, 8, UiAlpha(ink, 0.35f), BLANK);
        UiRoundRect((Rectangle){ (float)(cx - 5), (float)(cy - 5), 10, 10 }, 5, ink, BLANK);
        break;
    case PEN_ERASER:
        UiRoundRect((Rectangle){ (float)(cx - 10), (float)(cy - 6), 20, 12 }, 3, BLANK, ink);
        UiRoundRect((Rectangle){ (float)(cx - 10), (float)(cy - 6), 9, 12 }, 3, ink, BLANK);
        DrawRectangle(cx - 10, cy + 8, 20, 1, UiAlpha(ink, 0.5f));
        break;
    case PEN_FILL:
        UiRoundRect((Rectangle){ (float)(cx - 9), (float)(cy - 9), 18, 18 }, 3, BLANK, ink);
        DrawRectangle(cx - 6, cy - 6, 12, 12, UiAlpha(ink, 0.75f));
        break;
    default: break;
    }
}

// #region Tool icons (monochrome, about 18 x 18 px, in the style of PenIcon)

static void Seg(float cx, float cy, float x0, float y0, float x1, float y1, float th, Color c)
{
    DrawLineEx((Vector2){ cx + x0, cy + y0 }, (Vector2){ cx + x1, cy + y1 }, th, c);
}

// A closed outline through n points (offsets from cx, cy): solid, or dashed (3 px on, 2 off, the
// pattern running on around the corners)
static void Outline(float cx, float cy, const Vector2 *pts, int n, bool dashed, Color c)
{
    float at = 0.0f;        // (dashed: where in the 5 px pattern the segment starts)
    for (int i = 0; i < n; i++) {
        Vector2 a = { cx + pts[i].x, cy + pts[i].y }, b = { cx + pts[(i + 1) % n].x, cy + pts[(i + 1) % n].y };
        if (!dashed) { DrawLineEx(a, b, 2.0f, c); continue; }
        float len = Vector2Distance(a, b);
        if (len <= 0.0f) continue;
        for (float t = -at; t < len; t += 5.0f) {
            float t0 = fmaxf(t, 0.0f), t1 = fminf(t + 3.0f, len);
            if (t1 > t0) DrawLineEx(Vector2Lerp(a, b, t0 / len), Vector2Lerp(a, b, t1 / len), 2.0f, c);
        }
        at = fmodf(at + len, 5.0f);
    }
}

// A filled polygon (points counter-clockwise on screen, convex enough for a fan from its centre)
static void FillFan(float cx, float cy, const Vector2 *pts, int n, Color c)
{
    for (int i = 0; i < n; i++) {
        Vector2 a = { cx + pts[i].x, cy + pts[i].y }, b = { cx + pts[(i + 1) % n].x, cy + pts[(i + 1) % n].y };
        DrawTriangle((Vector2){ cx, cy }, a, b, c);
    }
}

// n points on an ellipse of radii rx, ry (counter-clockwise on screen); n <= 32
static int EllipsePts(Vector2 *pts, int n, float rx, float ry)
{
    for (int i = 0; i < n; i++) {
        float a = -2.0f * PI * (float)i / (float)n;
        pts[i] = (Vector2){ rx * cosf(a), ry * sinf(a) };
    }
    return n;
}

static void ToolIcon(DrawTool t, int icx, int icy, Color ink)
{
    float cx = (float)icx, cy = (float)icy;
    // (a freehand blob and a straight polygon, counter-clockwise on screen)
    static const Vector2 BLOB[] = { { 1, -8 }, { -5, -7 }, { -8, -2 }, { -7, 4 }, { -2, 7 }, { 4, 6 }, { 8, 2 }, { 7, -4 } };
    static const Vector2 POLY[] = { { -1, -8 }, { -8, -1 }, { -5, 7 }, { 6, 6 }, { 8, -4 } };
    Vector2 e[32];
    switch (t) {
    case TOOL_PENCIL:
        Seg(cx, cy, -4, 4, 7, -7, 5.0f, ink);
        Seg(cx, cy, -8, 8, -4, 4, 2.0f, ink);
        break;
    case TOOL_ERASER: PenIcon(PEN_ERASER, icx, icy, ink); break;
    case TOOL_SPRAY: {
        static const int D[5][2] = { { 4, -8 }, { 7, -5 }, { 3, -3 }, { 8, -1 }, { 5, 1 } };
        UiRoundRect((Rectangle){ cx - 8, cy - 3, 9, 12 }, 2, BLANK, ink);
        DrawRectangle(icx - 6, icy - 7, 5, 4, ink);
        for (int k = 0; k < 5; k++) DrawRectangle(icx + D[k][0], icy + D[k][1], 2, 2, ink);
        break;
    }
    case TOOL_BUCKET: {
        static const Vector2 B[] = { { -2, -8 }, { -9, -1 }, { -2, 6 }, { 5, -1 } };
        Outline(cx, cy, B, 4, false, ink);
        Seg(cx, cy, -5, -4, 1, 2, 2.0f, UiAlpha(ink, 0.5f));
        UiRoundRect((Rectangle){ cx + 4, cy + 3, 6, 6 }, 3, ink, BLANK);
        DrawTriangle((Vector2){ cx + 7, cy - 1 }, (Vector2){ cx + 4.3f, cy + 5 }, (Vector2){ cx + 9.7f, cy + 5 }, ink);
        break;
    }
    case TOOL_GRADIENT:
        for (int k = 0; k < 6; k++) DrawRectangle(icx - 9 + 3 * k, icy - 6, 3, 12, UiAlpha(ink, 1.0f - (float)k * 0.17f));
        DrawRectangleLinesEx((Rectangle){ cx - 9, cy - 6, 18, 12 }, 1.0f, UiAlpha(ink, 0.6f));
        break;
    case TOOL_LINE: Seg(cx, cy, -8, 7, 8, -7, 2.0f, ink); break;
    case TOOL_CURVE: {
        Vector2 p0 = { -9, 6 }, c1 = { -3, -16 }, c2 = { 3, 16 }, p3 = { 9, -6 }, prev = p0;
        for (int k = 1; k <= 12; k++) {
            float s = (float)k / 12.0f, u = 1.0f - s;
            Vector2 q = { u * u * u * p0.x + 3 * u * u * s * c1.x + 3 * u * s * s * c2.x + s * s * s * p3.x,
                          u * u * u * p0.y + 3 * u * u * s * c1.y + 3 * u * s * s * c2.y + s * s * s * p3.y };
            Seg(cx, cy, prev.x, prev.y, q.x, q.y, 2.0f, ink);
            prev = q;
        }
        break;
    }
    case TOOL_RECT: DrawRectangleLinesEx((Rectangle){ cx - 8, cy - 6, 16, 12 }, 2.0f, ink); break;
    case TOOL_RECT_FILL: DrawRectangle(icx - 8, icy - 6, 16, 12, ink); break;
    case TOOL_ELLIPSE: Outline(cx, cy, e, EllipsePts(e, 24, 8.5f, 6.0f), false, ink); break;
    case TOOL_ELLIPSE_FILL: FillFan(cx, cy, e, EllipsePts(e, 24, 9.0f, 6.5f), ink); break;
    case TOOL_CONTOUR:
        FillFan(cx, cy, BLOB, 8, UiAlpha(ink, 0.35f));
        Outline(cx, cy, BLOB, 8, false, ink);
        break;
    case TOOL_POLYGON:
        FillFan(cx, cy, POLY, 5, UiAlpha(ink, 0.35f));
        Outline(cx, cy, POLY, 5, false, ink);
        break;
    case TOOL_MARQUEE: {
        static const Vector2 R[] = { { -8, -6 }, { -8, 6 }, { 8, 6 }, { 8, -6 } };
        Outline(cx, cy, R, 4, true, ink);
        break;
    }
    case TOOL_MARQUEE_ELLIPSE: Outline(cx, cy, e, EllipsePts(e, 16, 8.5f, 6.5f), true, ink); break;
    case TOOL_LASSO:
        Outline(cx, cy - 2, BLOB, 8, true, ink);
        Seg(cx, cy, -5, 4, -7, 9, 2.0f, ink);
        break;
    case TOOL_LASSO_POLY: Outline(cx, cy, POLY, 5, true, ink); break;
    case TOOL_WAND:
        Seg(cx, cy, -8, 8, 1, -1, 3.0f, ink);
        Seg(cx, cy, 5, -10, 5, -2, 2.0f, ink);
        Seg(cx, cy, 1, -6, 9, -6, 2.0f, ink);
        DrawRectangle(icx + 7, icy + 1, 2, 2, ink);
        DrawRectangle(icx - 1, icy - 10, 2, 2, ink);
        break;
    case TOOL_MOVE:
        Seg(cx, cy, -7, 0, 7, 0, 2.0f, ink);
        Seg(cx, cy, 0, -7, 0, 7, 2.0f, ink);
        DrawTriangle((Vector2){ cx, cy - 10 }, (Vector2){ cx - 4, cy - 5 }, (Vector2){ cx + 4, cy - 5 }, ink);
        DrawTriangle((Vector2){ cx, cy + 10 }, (Vector2){ cx + 4, cy + 5 }, (Vector2){ cx - 4, cy + 5 }, ink);
        DrawTriangle((Vector2){ cx - 10, cy }, (Vector2){ cx - 5, cy + 4 }, (Vector2){ cx - 5, cy - 4 }, ink);
        DrawTriangle((Vector2){ cx + 10, cy }, (Vector2){ cx + 5, cy - 4 }, (Vector2){ cx + 5, cy + 4 }, ink);
        break;
    case TOOL_BLUR:
        UiRoundRect((Rectangle){ cx - 6, cy - 3, 12, 12 }, 6, ink, BLANK);
        DrawTriangle((Vector2){ cx, cy - 10 }, (Vector2){ cx - 5.5f, cy }, (Vector2){ cx + 5.5f, cy }, ink);
        break;
    case TOOL_JUMBLE: {
        static const int J[9][2] = { { -8, -8 }, { -1, -7 }, { 5, -8 }, { -7, -1 }, { 0, 0 }, { 6, -2 }, { -8, 5 }, { -2, 6 }, { 5, 5 } };
        for (int k = 0; k < 9; k++)
            DrawRectangle(icx + J[k][0], icy + J[k][1], 4, 4, UiAlpha(ink, k % 3 == 0 ? 1.0f : k % 3 == 1 ? 0.6f : 0.35f));
        break;
    }
    case TOOL_EYEDROPPER:
        Seg(cx, cy, -6, 6, 3, -3, 3.0f, ink);
        Seg(cx, cy, -9, 9, -6, 6, 1.5f, ink);
        UiRoundRect((Rectangle){ cx + 1, cy - 9, 8, 8 }, 4, ink, BLANK);
        break;
    default: break;
    }
}

// #endregion

static bool Hovered(const Ctx *c, Rectangle r)
{
    if (c->pass == PASS_MEASURE || !c->p->interactive || IsCursorHidden()) return false;
    Vector2 m = GetMousePosition();
    return CheckCollisionPointRec(m, r) && CheckCollisionPointRec(m, c->p->rect);
}

static bool RightClicked(const Ctx *c, Rectangle r)
{
    return c->pass == PASS_INPUT && Hovered(c, r) && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
}

static bool ShiftDown(void) { return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }

// A row of n buttons (a choice; `current` is on): after a label (LABEL_W), or across the row without
// one. The index clicked, else -1
static int Choice(Ctx *c, const char *label, const char *const *names, int n, int current)
{
    int x0 = c->x;
    if (label) {
        Text(c, label, c->x, c->y + (BUTTON_H - UiLineHeight(UI_S)) / 2, UI_S, UI_TEXT);
        x0 += LABEL_W;
    }
    int room = c->x + c->w - x0, clicked = -1;
    for (int k = 0; k < n; k++) {
        int a = x0 + k * (room + GAP) / n, b = x0 + (k + 1) * (room + GAP) / n - GAP;
        if (Button(c, (Rectangle){ (float)a, (float)c->y, (float)(b - a), BUTTON_H }, names[k], k == current)) clicked = k;
    }
    c->y += BUTTON_H + GAP;
    return clicked;
}

// A slider + number field row of an integer in lo..hi
static void IntRow(Ctx *c, const char *label, int sliderId, int fieldId, int *value, int lo, int hi)
{
    Rectangle sr, fr;
    Row(c, label, LABEL_W, UI_TEXT, &sr, &fr);
    float v = (float)*value;
    if (Slider(c, sliderId, sr, &v, (float)lo, (float)hi, 1.0f)) *value = ClampI((int)roundf(v), lo, hi);
    v = (float)*value;
    if (Field(c, fieldId, fr, &v, (float)lo, (float)hi, 1.0f, 0)) *value = ClampI((int)roundf(v), lo, hi);
    c->y += ROW_H + ROW_GAP;
}

// Brush shape (round / square / soft), size, hardness (soft)
static void BrushRows(Ctx *c)
{
    DrawMode *dm = c->dm;
    Text(c, "Brush", c->x, c->y + (BUTTON_H - UiLineHeight(UI_S)) / 2, UI_S, UI_TEXT);
    int x0 = c->x + LABEL_W, room = c->w - LABEL_W;
    for (int k = 0; k < 3; k++) {
        int a = x0 + k * (room + GAP) / 3, b = x0 + (k + 1) * (room + GAP) / 3 - GAP;
        Rectangle r = { (float)a, (float)c->y, (float)(b - a), BUTTON_H };
        bool on = dm->shape == (PenKind)k;
        if (Button(c, r, NULL, on)) dm->shape = (PenKind)k;
        if (Drawing(c)) PenIcon((PenKind)k, (int)(r.x + r.width / 2), (int)(r.y + r.height / 2), on ? UI_ON_INK : UI_INK);
    }
    c->y += BUTTON_H + GAP;

    // Size: the slider runs over log2 (1 .. 256: 16 in the middle), the field is exact
    Rectangle sr, fr;
    Row(c, "Size", LABEL_W, UI_TEXT, &sr, &fr);
    float lv = log2f((float)(dm->size > 0 ? dm->size : 1));
    float before = lv;
    if (Slider(c, W_SIZE_S, sr, &lv, 0.0f, log2f((float)DRAW_SIZE_MAX), 0.0f)) {
        int n = ClampI((int)roundf(exp2f(lv)), 1, DRAW_SIZE_MAX);
        // (a wheel step that rounds back to the same size goes one texel on)
        if (n == dm->size && GetMouseWheelMove() != 0.0f) n = ClampI(n + (lv > before ? 1 : -1), 1, DRAW_SIZE_MAX);
        dm->size = n;
    }
    float fv = (float)dm->size;
    if (Field(c, W_SIZE_F, fr, &fv, 1.0f, (float)DRAW_SIZE_MAX, 1.0f, 0)) dm->size = ClampI((int)fv, 1, DRAW_SIZE_MAX);
    c->y += ROW_H + ROW_GAP;

    // Hardness: the soft brush's only
    if (dm->shape == PEN_SOFT) {
        Row(c, "Hardness", LABEL_W, UI_TEXT, &sr, &fr);
        float hv = dm->hardness;
        if (Slider(c, W_HARD_S, sr, &hv, 0.0f, 1.0f, 0.01f)) dm->hardness = hv;
        hv = dm->hardness;
        if (Field(c, W_HARD_F, fr, &hv, 0.0f, 1.0f, 0.05f, 2)) dm->hardness = hv;
        c->y += ROW_H + ROW_GAP;
    }
    c->y += GAP - ROW_GAP;
}

// The options of the tool on (docs/painting.md 18.9): only DrawMode fields, nothing of the tools'
// behaviour
static void ToolOptions(Ctx *c)
{
    DrawMode *dm = c->dm;
    DrawTool t = dm->tool;
    bool brush = t == TOOL_PENCIL || t == TOOL_ERASER || t == TOOL_SPRAY || t == TOOL_LINE || t == TOOL_CURVE ||
                 t == TOOL_RECT || t == TOOL_ELLIPSE || t == TOOL_BLUR || t == TOOL_JUMBLE;
    if (brush) BrushRows(c);
    // On a paint grid Pixel look sits here, with the brush (docs/painting.md 17.7): beside Pixel perfect,
    // else on a row of its own under the tool's options
    bool grid = TargetGrid(dm);
    if (t == TOOL_PENCIL) {
        int bw = (c->w - GAP) / 2;
        Rectangle r = grid ? (Rectangle){ (float)c->x, (float)c->y, (float)bw, BUTTON_H }
                           : (Rectangle){ (float)(c->x + LABEL_W), (float)c->y, (float)(c->w - LABEL_W), BUTTON_H };
        if (Button(c, r, "Pixel perfect", dm->pixelPerfect)) dm->pixelPerfect = !dm->pixelPerfect;
        if (grid) PixelLookButton(c, (Rectangle){ (float)(c->x + bw + GAP), (float)c->y, (float)(c->w - bw - GAP), BUTTON_H });
        c->y += BUTTON_H + GAP;
    } else if (t == TOOL_SPRAY) {
        IntRow(c, "Width", W_SPRAYW_S, W_SPRAYW_F, &dm->sprayWidth, 1, 256);
        IntRow(c, "Speed", W_SPEED_S, W_SPEED_F, &dm->spraySpeed, 1, 256);
        c->y += GAP - ROW_GAP;
    } else if (t == TOOL_BUCKET || t == TOOL_WAND) {
        // (the wand: contiguous / global; the bucket also the whole region, today's fill)
        static const char *const MODES[3] = { "Contiguous", "Global", "Region" };
        int k = Choice(c, NULL, MODES, t == TOOL_BUCKET ? 3 : 2, (int)dm->bucket);
        if (k >= 0) dm->bucket = (DrawBucketMode)k;
        if (!(t == TOOL_BUCKET && dm->bucket == DRAW_BUCKET_REGION)) {
            IntRow(c, "Tolerance", W_TOL_S, W_TOL_F, &dm->tolerance, 0, 255);
            c->y += GAP - ROW_GAP;
        }
    } else if (t == TOOL_GRADIENT) {
        static const char *const KINDS[2] = { "Linear", "Radial" };
        int k = Choice(c, "Gradient", KINDS, 2, dm->gradientRadial ? 1 : 0);
        if (k >= 0) dm->gradientRadial = k == 1;
    } else if (t == TOOL_EYEDROPPER) {
        static const char *const PICKS[2] = { "Screen", "Paint" };
        int k = Choice(c, "Samples", PICKS, 2, dm->pickTool == DRAW_PICK_SCREEN ? 0 : 1);
        if (k >= 0) dm->pickTool = k == 0 ? DRAW_PICK_SCREEN : DRAW_PICK_PAINT;
    }
    if (grid && t != TOOL_PENCIL) {
        PixelLookButton(c, (Rectangle){ (float)(c->x + LABEL_W), (float)c->y, (float)(c->w - LABEL_W), BUTTON_H });
        c->y += BUTTON_H + GAP;
    }
    // (what the tool does when it has no options; the selection tools' modifiers)
    if (DrawToolKindOf(t) == TOOLKIND_SELECT) {
        const char *hints = "{SHIFT} adds|{ALT} subtracts|{SHIFT}{ALT} both";
        if (Drawing(c)) UiHints(hints, c->x, c->y, UI_XS, UI_DIM);
        c->y += UiLineHeight(UI_XS) + GAP;
    } else if (DrawToolKindOf(t) == TOOLKIND_MOVE) {
        c->y += Wrapped(c, "drags the selected pixels (no selection: the region's)", UI_XS, UI_DIM, 2) + GAP;
    } else if (DrawToolKindOf(t) == TOOLKIND_SHAPE && !brush && t != TOOL_GRADIENT) {
        c->y += Wrapped(c, "filled with fg (RMB: bg)", UI_XS, UI_DIM, 2) + GAP;
    }
}

#define TOOL_COLS 8
#define TOOL_H 30
#define TOOL_GAP 4

static void SectionTools(Ctx *c)
{
    DrawMode *dm = c->dm;
    Header(c, "Tools", "{1}..{5} pens|{[}{]} size");
    // The grid, DrawTool order row by row; the tool on is white
    int bw = (c->w - (TOOL_COLS - 1) * TOOL_GAP) / TOOL_COLS;
    int left = c->x + (c->w - (bw * TOOL_COLS + (TOOL_COLS - 1) * TOOL_GAP)) / 2;
    int hover = -1;
    for (int t = 0; t < TOOL_COUNT; t++) {
        Rectangle r = { (float)(left + (t % TOOL_COLS) * (bw + TOOL_GAP)), (float)(c->y + (t / TOOL_COLS) * (TOOL_H + TOOL_GAP)),
                        (float)bw, TOOL_H };
        bool on = dm->tool == (DrawTool)t;
        if (Button(c, r, NULL, on)) dm->tool = (DrawTool)t;
        if (Hovered(c, r)) hover = t;
        if (Drawing(c)) ToolIcon((DrawTool)t, (int)(r.x + r.width / 2), (int)(r.y + r.height / 2), on ? UI_ON_INK : UI_INK);
    }
    c->y += ((TOOL_COUNT + TOOL_COLS - 1) / TOOL_COLS) * (TOOL_H + TOOL_GAP) - TOOL_GAP + GAP;

    // The hint line: the tool's name and its key (the one under the cursor, else the one on)
    DrawTool shown = hover >= 0 ? (DrawTool)hover : dm->tool;
    if (Drawing(c)) {
        UiTextBold(DrawToolName(shown), c->x, c->y, UI_S, shown == dm->tool ? UI_INK : UI_DIM);
        const char *key = DrawToolKeyName(shown);
        const char *plus = strchr(key, '+');
        const char *markup = !key[0] ? "~no key~"
                           : plus ? TextFormat("{%.*s}+{%s}", (int)(plus - key), key, plus + 1) : TextFormat("{%s}", key);
        UiHints(markup, c->x + c->w - UiHintsWidth(markup, UI_XS), c->y + (UiLineHeight(UI_S) - UiLineHeight(UI_XS)) / 2, UI_XS, UI_DIM);
    }
    c->y += UiLineHeight(UI_S) + GAP;

    ToolOptions(c);
    c->y += Wrapped(c, "not here: zoom, hand (the camera), slice, text, layers, frames", UI_XS, UI_FAINT, 2) + GAP;
}

static void FormatHex(char *out, int size, Color c)
{
    if (c.a == 255) snprintf(out, (size_t)size, "#%02X%02X%02X", c.r, c.g, c.b);
    else snprintf(out, (size_t)size, "#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
}

// "#RRGGBB" or "#RRGGBBAA" (the # and surrounding spaces optional); false for anything else
static bool ParseHex(const char *s, Color *out)
{
    while (*s == ' ') s++;
    if (*s == '#') s++;
    unsigned v[4] = { 0, 0, 0, 255 };
    int n = 0;
    while (s[n] && s[n] != ' ') {
        char ch = s[n];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
        n++;
    }
    for (int k = n; s[k]; k++) if (s[k] != ' ') return false;
    if (n != 6 && n != 8) return false;
    for (int k = 0; k < n / 2; k++) {
        char pair[3] = { s[2 * k], s[2 * k + 1], 0 };
        v[k] = (unsigned)strtoul(pair, NULL, 16);
    }
    *out = (Color){ (unsigned char)v[0], (unsigned char)v[1], (unsigned char)v[2], (unsigned char)v[3] };
    return true;
}

static bool SameColor(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

// The selector's HSV from fg when fg's RGB changed elsewhere (a key, the eyedropper, a field): the
// hue stays while the saturation or the value is 0, the saturation while the value is 0
static void SyncHsv(DrawPanel *p, Color fg)
{
    if (p->hsvSet && fg.r == p->hsvOf.r && fg.g == p->hsvOf.g && fg.b == p->hsvOf.b) return;
    Vector3 hsv = ColorToHSV(fg);
    if (hsv.z > 0.0f) {
        if (hsv.y > 0.0f && isfinite(hsv.x)) p->hue = hsv.x;
        p->sat = hsv.y;
    }
    p->val = hsv.z;
    p->hsvOf = fg;
    p->hsvSet = true;
}

#define SV_W        188     // the saturation / value square
#define SV_H        128
#define HUE_W       16
#define ALPHA_H     12
#define FGBG        38      // the fg / bg squares (bg offset by FGBG_OFF down and right)
#define FGBG_OFF    18
#define PAL_CELL    20      // a palette swatch
#define PAL_GAP     3

static void SectionColor(Ctx *c)
{
    DrawMode *dm = c->dm;
    DrawPanel *p = c->p;
    Header(c, "Colour", "{ALT}+LMB picks|{X} swap");
    SyncHsv(p, dm->fg);

    // The selector: saturation / value square, hue strip beside it, alpha strip under it
    Rectangle sv = { (float)c->x, (float)c->y, SV_W, SV_H };
    Rectangle hue = { sv.x + SV_W + 8, sv.y, HUE_W, SV_H };
    Rectangle alpha = { sv.x, sv.y + SV_H + 8, SV_W, ALPHA_H };
    if (c->pass != PASS_MEASURE) {
        float s = p->sat, v = p->val, h = p->hue, a = (float)dm->fg.a;
        bool hsvChanged = false;
        if (UiSatValSquare(&p->drag, W_SV, sv, h, &s, &v) && c->pass == PASS_INPUT) {
            p->sat = s;
            p->val = v;
            hsvChanged = true;
        }
        if (UiHueStrip(&p->drag, W_HUE, hue, &h) && c->pass == PASS_INPUT) {
            p->hue = h;
            hsvChanged = true;
        }
        if (hsvChanged) {
            Color n = ColorFromHSV(p->hue, p->sat, p->val);
            n.a = dm->fg.a;
            dm->fg = n;
            p->hsvOf = n;
        }
        if (UiAlphaStrip(&p->drag, W_ALPHA, alpha, dm->fg, &a) && c->pass == PASS_INPUT) dm->fg.a = (unsigned char)ClampI((int)a, 0, 255);
    }

    // fg / bg: overlapping squares (Aseprite's); a click on bg makes it fg (a swap, as X)
    float fx = hue.x + HUE_W + 10;
    Rectangle fgR = { fx, sv.y, FGBG, FGBG }, bgR = { fx + FGBG_OFF, sv.y + FGBG_OFF, FGBG, FGBG };
    Swatch(c, bgR, dm->bg, false);
    Swatch(c, fgR, dm->fg, false);
    if (c->pass == PASS_INPUT && Hovered(c, bgR) && !Hovered(c, fgR) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        Color t = dm->fg;
        dm->fg = dm->bg;
        dm->bg = t;
    }
    Text(c, "fg", (int)fgR.x, (int)(bgR.y + FGBG + 4), UI_XS, UI_DIM);
    Text(c, "bg", (int)(bgR.x + FGBG) - UiMeasure("bg", UI_XS), (int)(bgR.y + FGBG + 4), UI_XS, UI_DIM);
    c->y += SV_H + 8 + ALPHA_H + GAP + 4;

    // R G B A
    static const char *names[4] = { "R", "G", "B", "A" };
    for (int k = 0; k < 4; k++) {
        unsigned char *ch = k == 0 ? &dm->fg.r : k == 1 ? &dm->fg.g : k == 2 ? &dm->fg.b : &dm->fg.a;
        Rectangle sr, fr;
        Row(c, names[k], CHANNEL_W, UI_TEXT, &sr, &fr);
        float v = (float)*ch;
        if (Slider(c, W_R_S + k, sr, &v, 0.0f, 255.0f, 1.0f)) *ch = (unsigned char)ClampI((int)roundf(v), 0, 255);
        v = (float)*ch;
        if (Field(c, W_R_F + k, fr, &v, 0.0f, 255.0f, 1.0f, 0)) *ch = (unsigned char)ClampI((int)roundf(v), 0, 255);
        c->y += ROW_H + ROW_GAP;
    }
    c->y += 1;

    // The hex field
    Rectangle hr = { (float)c->x, (float)c->y, 120, BUTTON_H };
    if (p->edit.active != W_HEX && p->edit.claim != W_HEX) FormatHex(p->hex, sizeof(p->hex), dm->fg);
    if (c->pass != PASS_MEASURE && UiTextField(&p->edit, W_HEX, hr, p->hex, sizeof(p->hex)) && c->pass == PASS_INPUT) {
        Color col;
        if (ParseHex(p->hex, &col)) dm->fg = col;
        FormatHex(p->hex, sizeof(p->hex), dm->fg);      // (an unreadable one goes back)
    }
    Text(c, "hex", (int)(hr.x + hr.width) + 8, c->y + (BUTTON_H - UiLineHeight(UI_XS)) / 2, UI_XS, UI_DIM);
    c->y += BUTTON_H + GAP;

    // The recent colours (a click picks one into fg, RMB into bg)
    int n = DRAW_RECENT_COLORS, sz = (c->w - (n - 1) * 4) / n;
    for (int k = 0; k < n; k++) {
        Rectangle r = { (float)(c->x + k * (sz + 4)), (float)c->y, (float)sz, 22 };
        if (k < dm->recentCount) {
            if (Swatch(c, r, dm->recent[k], SameColor(dm->recent[k], dm->fg))) dm->fg = dm->recent[k];
            else if (RightClicked(c, r)) dm->bg = dm->recent[k];
        } else if (Drawing(c) && dm->recentCount > 0) DrawRectangleLinesEx(r, 1.0f, UiAlpha(UI_LINE, 0.6f));
        // (with none yet the row shows its label alone, not the label over empty boxes)
    }
    if (dm->recentCount == 0) Text(c, "recent colours", c->x + 8, c->y + (22 - UiLineHeight(UI_XS)) / 2, UI_XS, UI_FAINT);
    c->y += 22 + GAP;

    // The palette: + adds fg; a click picks into fg, RMB into bg, SHIFT+click removes
    if (Drawing(c)) {
        UiText("Palette", c->x, c->y, UI_XS, UI_DIM);
        const char *hints = "RMB bg|{SHIFT}+click removes";
        UiHints(hints, c->x + c->w - UiHintsWidth(hints, UI_XS), c->y, UI_XS, UI_DIM);
    }
    c->y += UiLineHeight(UI_XS) + 4;
    int cols = (c->w + PAL_GAP) / (PAL_CELL + PAL_GAP);
    int count = dm->paletteCount;
    for (int k = 0; k <= count; k++) {
        Rectangle r = { (float)(c->x + (k % cols) * (PAL_CELL + PAL_GAP)), (float)(c->y + (k / cols) * (PAL_CELL + PAL_GAP)), PAL_CELL, PAL_CELL };
        if (k == count) {
            // (+ at the end; it stops at DRAWSTATE_PALETTE_MAX, the state file's bound)
            bool room = count < DRAWSTATE_PALETTE_MAX;
            if (Button(c, r, room ? "+" : "", false) && room) {
                Color *slot = MS_PUSH(dm->palette, dm->paletteCount, dm->paletteCap);
                if (slot) *slot = dm->fg;
            }
            continue;
        }
        Color col = dm->palette[k];
        if (Swatch(c, r, col, SameColor(col, dm->fg))) {
            if (ShiftDown()) {
                memmove(&dm->palette[k], &dm->palette[k + 1], sizeof(Color) * (size_t)(dm->paletteCount - k - 1));
                dm->paletteCount--;
                break;      // (the rest moved: drawn from the next frame on)
            }
            dm->fg = col;
        } else if (RightClicked(c, r)) dm->bg = col;
    }
    c->y += (count / cols + 1) * (PAL_CELL + PAL_GAP) - PAL_GAP + GAP;
}

// The Density / Grid section's buttons: resize (Resize to density, or Fit to grid on a grid), Pixel
// look (on a grid it sits with the brush instead, ToolOptions: then Clear paint joins the first row),
// Clear paint
static void DensityButtons(Ctx *c, const char *resize, bool resizeShown, bool pixelHere)
{
    int bw = (c->w - GAP) / 2;
    Rectangle left = { (float)c->x, (float)c->y, (float)bw, BUTTON_H };
    Rectangle right = { (float)(c->x + bw + GAP), (float)c->y, (float)(c->w - bw - GAP), BUTTON_H };
    // (Fit to grid only on a target with an entry, DrawTarget.fitOk; its place stays, so the panel
    // doesn't move under the cursor)
    if (resizeShown && Button(c, left, resize, false)) c->act = DRAWPANEL_ACT_RESIZE;
    if (!pixelHere) {
        if (Button(c, right, "Clear paint", false)) c->act = DRAWPANEL_ACT_CLEAR;
        c->y += BUTTON_H + GAP;
        return;
    }
    // (the target atlas's filter when it has one; else what new atlases get)
    PixelLookButton(c, right);
    c->y += BUTTON_H + GAP;
    left.y = (float)c->y;
    if (Button(c, left, "Clear paint", false)) c->act = DRAWPANEL_ACT_CLEAR;
    c->y += BUTTON_H + GAP;
}

static void SectionDensity(Ctx *c)
{
    DrawMode *dm = c->dm;
    bool grid = TargetGrid(dm);
    Header(c, grid ? "Grid" : "Density", NULL);
    // What the target's texels follow: "follows the grid of concrete: 24 texels/m (4.2 cm)", or "concrete
    // has no paint grid: 64 texels/m from the panel" over the slider
    if (TargetSet(dm) && dm->target.gridLineSet)
        c->y += Wrapped(c, MsgText(&dm->target.gridLine), UI_S, grid ? UI_TEXT : UI_DIM, 2) + GAP - 1;
    if (grid) {
        // The texel size there (its rectangle's or the one its first stroke gets) and Fit to grid
        if (dm->target.texelSet) c->y += Wrapped(c, MsgText(&dm->target.texel), UI_S, UI_DIM, 2) + GAP - 1;
        DensityButtons(c, "Fit to grid", dm->target.fitOk, false);
        return;
    }
    // Texels per metre of new paint: the slider over log2 (8 .. 512: 64 in the middle)
    Rectangle sr, fr;
    Row(c, "Texels / m", LABEL_W, UI_TEXT, &sr, &fr);
    float d = dm->density > 0.0f ? dm->density : DRAW_DENSITY_DEFAULT;
    float lv = log2f(d);
    if (Slider(c, W_DENS_S, sr, &lv, 3.0f, 9.0f, 0.0f)) dm->density = (float)ClampI((int)roundf(exp2f(lv)), 8, 512);
    float fv = d;
    if (Field(c, W_DENS_F, fr, &fv, 8.0f, 512.0f, 1.0f, 0)) dm->density = fv;
    c->y += ROW_H + ROW_GAP;
    // The texel size on the target (its rectangle's, or the one its first stroke gets), else at the density
    const char *texel = TargetSet(dm) && dm->target.ok && dm->target.texelSet ? MsgText(&dm->target.texel)
                      : TextFormat("new paint: a texel is about %.1f mm", 1000.0f / d);
    c->y += Wrapped(c, texel, UI_S, UI_DIM, 2) + GAP - 1;
    DensityButtons(c, "Resize to density", true, true);
}

// faces[0..count) with the numbers closed up in order (0 2 2 5 -> 0 1 1 2); faces past count stay
static void CloseUp(unsigned char faces[6], int count)
{
    unsigned char map[6];
    int next = 0;
    for (int n = 0; n < 6; n++) {
        bool used = false;
        for (int f = 0; f < count; f++) used |= faces[f] == n;
        map[n] = (unsigned char)(used ? next++ : 0);
    }
    for (int f = 0; f < count; f++) faces[f] = map[faces[f] < 6 ? faces[f] : 0];
}

static void SectionFaces(Ctx *c)
{
    DrawPanel *p = c->p;
    const DrawMode *dm = c->dm;
    // (boxes and shapes of 2 or more parts; the others say why not)
    if (!TargetBox(dm) && c->p->only != DRAWPANEL_FACES) return;
    Header(c, "Faces", "same number: one rectangle");
    if (!TargetBox(dm) || !dm->target.facesOk) {
        c->y += Wrapped(c, TargetShape(dm) ? "a sphere has one part: nothing to group"
                                           : "point at a box or a shape: its faces' (parts') paint is grouped here", UI_S, UI_DIM, 2) + GAP;
        return;
    }
    const unsigned char *cells = dm->target.groups;
    const MS_Shape *S = TargetShape(dm);
    int count = dm->target.faceCount > 0 && dm->target.faceCount <= 6 ? dm->target.faceCount : 6;
    // A button per face (a shape's part): its name (dim) and its cell number; a click counts the number
    // on (lit: changed, not applied yet)
    int bw = (c->w - (count - 1) * GAP) / count;
    int left = c->x + (c->w - (count * bw + (count - 1) * GAP)) / 2;
    for (int f = 0; f < count; f++) {
        Rectangle r = { (float)(left + f * (bw + GAP)), (float)c->y, (float)bw, BUTTON_H };
        bool lit = p->faces[f] != cells[f];
        if (Button(c, r, NULL, lit)) p->faces[f] = (unsigned char)((p->faces[f] + 1) % count);
        if (Drawing(c)) {
            const char *name = FaceButtonName(dm, f);
            const char *n = TextFormat("%d", p->faces[f] < 6 ? p->faces[f] : 0);
            int nw = UiMeasureBold(n, UI_M), fw = UiMeasure(name, UI_XS);
            int x = (int)r.x + ((int)r.width - fw - 5 - nw) / 2;
            UiText(name, x, (int)r.y + (BUTTON_H - UiLineHeight(UI_XS)) / 2 + 1, UI_XS, lit ? UiAlpha(UI_ON_INK, 0.6f) : UI_DIM);
            UiTextBold(n, x + fw + 5, (int)r.y + (BUTTON_H - UiLineHeight(UI_M)) / 2, UI_M, lit ? UI_ON_INK : UI_INK);
        }
    }
    c->y += BUTTON_H + GAP;
    // Apply, and the two presets (they set the buttons; Apply applies them). A round shape's second
    // preset puts every part together (a cube's: the sides, as a box's)
    bool round = S && S->kind != MS_SHAPE_CUBE;
    const char *labels[3] = { "Apply", "All separate", round ? "All together" : "Sides together" };
    int tw[3], total = 0;
    for (int k = 0; k < 3; k++) total += tw[k] = UiMeasure(labels[k], UI_M) + 16;
    int room = c->w - 2 * GAP, x = c->x;
    for (int k = 0; k < 3; k++) {
        int w = k < 2 ? tw[k] * room / total : c->x + c->w - x;
        Rectangle r = { (float)x, (float)c->y, (float)w, BUTTON_H };
        if (Button(c, r, labels[k], k == 0 && memcmp(p->faces, cells, (size_t)count) != 0)) {
            if (k == 0) {
                CloseUp(p->faces, count);
                c->act = DRAWPANEL_ACT_FACES;
            } else {
                static const unsigned char presets[3][6] = { { 0, 1, 2, 3, 4, 5 }, { 0, 0, 1, 2, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
                memcpy(p->faces, presets[k == 2 && round ? 2 : k - 1], (size_t)count);
            }
        }
        x += w + GAP;
    }
    c->y += BUTTON_H + GAP;
}

// #endregion

// Every section from the top (c->y: the panel's top, scrolled); c->y ends at the content's end
static void Layout(Ctx *c)
{
    DrawPanelSection only = c->p->only;
    int top = c->y;
    c->x = PANEL_X + PAD;
    c->w = INNER;
    c->y += 4;
    Title(c);
    if (only == DRAWPANEL_ALL || only == DRAWPANEL_TARGET) SectionTarget(c);
    if (only == DRAWPANEL_ALL || only == DRAWPANEL_TOOLS) SectionTools(c);
    if (only == DRAWPANEL_ALL || only == DRAWPANEL_COLOR) SectionColor(c);
    if (only == DRAWPANEL_ALL || only == DRAWPANEL_DENSITY) SectionDensity(c);
    if (only == DRAWPANEL_ALL || only == DRAWPANEL_FACES) SectionFaces(c);
    c->y += PAD - GAP;
    c->p->contentH = c->y - top;
}

// The panel's rectangle when drawn from y (its height: the content's, at most the room left)
static Rectangle PanelRect(const DrawPanel *p, int y)
{
    int room = GetScreenHeight() - y - BOTTOM_ROOM;
    if (room < 120) room = 120;
    int h = p->contentH < room ? p->contentH : room;
    return (Rectangle){ PANEL_X, (float)y, PANEL_W, (float)h };
}

static void ClampScroll(DrawPanel *p)
{
    int most = p->contentH - (int)p->rect.height;
    if (p->scroll > most) p->scroll = most;
    if (p->scroll < 0) p->scroll = 0;
}

void DrawPanelInit(DrawPanel *p)
{
    memset(p, 0, sizeof(*p));
    for (int f = 0; f < 6; f++) p->faces[f] = p->facesLoaded[f] = (unsigned char)f;
    p->facesKey = -2;
    p->interactive = true;
}

bool DrawPanelSetOnly(DrawPanel *p, const char *section)
{
    static const struct { const char *name; DrawPanelSection s; } names[] = {
        { "all", DRAWPANEL_ALL }, { "target", DRAWPANEL_TARGET }, { "tools", DRAWPANEL_TOOLS }, { "pen", DRAWPANEL_TOOLS },
        { "color", DRAWPANEL_COLOR },
        { "colour", DRAWPANEL_COLOR }, { "density", DRAWPANEL_DENSITY }, { "canvas", DRAWPANEL_DENSITY },
        { "faces", DRAWPANEL_FACES },
    };
    p->only = DRAWPANEL_ALL;
    if (!section) return true;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(section, names[i].name) == 0) { p->only = names[i].s; return true; }
    return false;
}

bool DrawPanelUpdate(DrawPanel *p, DrawMode *dm, DrawPanelAction *action)
{
    *action = DRAWPANEL_ACT_NONE;
    bool wasTyping = p->edit.active || p->edit.claim;
    if (dm->panelHidden || !p->interactive || p->rect.width <= 0.0f) {
        // (hidden: a drag or an open field ends without applying)
        p->drag.active = p->edit.active = p->edit.claim = 0;
        p->typing = wasTyping;
        return false;
    }
    bool mouse = !IsCursorHidden();
    Vector2 m = GetMousePosition();
    bool over = mouse && CheckCollisionPointRec(m, p->rect);
    bool pressed = mouse && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    bool dragging = p->drag.active != 0;
    SyncFaces(p, dm);

    Ctx c = { .p = p, .dm = dm, .pass = PASS_INPUT, .y = p->top - p->scroll };
    UiWidgetsBegin(UI_WIDGETS_INPUT, mouse, p->rect);
    Layout(&c);
    UiWidgetsEnd();
    // The wheel scrolls a panel taller than its room (over anything but a slider or a field)
    float wheel = over && !c.wheelTaken ? GetMouseWheelMove() : 0.0f;
    if (wheel != 0.0f && p->contentH > (int)p->rect.height) {
        p->scroll -= (int)(wheel * SCROLL_STEP);
        ClampScroll(p);
    }
    p->typing = wasTyping || p->edit.active || p->edit.claim;
    *action = c.act;
    return over || dragging || p->drag.active || (wasTyping && pressed);
}

void DrawPanelDraw(DrawPanel *p, const DrawMode *dm, int y)
{
    p->top = y;
    if (dm->panelHidden) {
        p->rect = (Rectangle){ 0 };
        return;
    }
    SyncFaces(p, dm);
    // The widgets draw a copy: the draw pass changes nothing
    DrawMode view = *dm;
    Ctx c = { .p = p, .dm = &view, .pass = PASS_MEASURE, .y = 0 };
    Layout(&c);
    p->rect = PanelRect(p, y);
    ClampScroll(p);
    bool scrolls = p->contentH > (int)p->rect.height;
    UiSheet(p->rect);
    if (scrolls) BeginScissorMode((int)p->rect.x + 1, (int)p->rect.y + 1, (int)p->rect.width - 2, (int)p->rect.height - 2);
    c = (Ctx){ .p = p, .dm = &view, .pass = PASS_DRAW, .y = y - p->scroll };
    UiWidgetsBegin(UI_WIDGETS_DRAW, p->interactive && !IsCursorHidden(), p->rect);
    Layout(&c);
    UiWidgetsEnd();
    if (scrolls) {
        EndScissorMode();
        // a 2 px track and a thumb of the visible share
        int trackH = (int)p->rect.height - 12, thumbH = trackH * (int)p->rect.height / p->contentH;
        if (thumbH < 16) thumbH = 16;
        int most = p->contentH - (int)p->rect.height;
        int thumbY = (int)p->rect.y + 6 + (most > 0 ? (trackH - thumbH) * p->scroll / most : 0);
        int tx = (int)(p->rect.x + p->rect.width) - 5;
        DrawRectangle(tx, (int)p->rect.y + 6, 2, trackH, UI_LINE);
        DrawRectangle(tx, thumbY, 2, thumbH, UI_DIM);
    }
}

bool DrawPanelContains(const DrawPanel *p, Vector2 px)
{
    return p->rect.width > 0 && CheckCollisionPointRec(px, p->rect);
}
