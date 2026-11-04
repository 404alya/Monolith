// The draw panel (docs/painting.md 13.2): docked at the left edge below the HUD lines, 300 px
// wide, in the ui.h look. From top to bottom: Target (the surface the cursor rested on last, what
// paint there goes into or why it can't be painted; the shared switch), Pen (five pens, size,
// hardness), Colour (R G B A, a swatch, the hex field, the recent colours), Canvas (density, Resize
// to density, the texel size, pixel look) and Faces (boxes only: the face groups of a box canvas).
// The target, its canvas and its lines come from the draw mode (dm->target); the actions act on it.
//
// One layout function serves three passes: a measure pass (the height, no widget runs), the input
// pass (DrawPanelUpdate, before the draw mode reads the mouse: the widgets change the DrawMode,
// nothing is drawn) and the draw pass (DrawPanelDraw, with the HUD: the widgets draw a copy of the
// DrawMode and read no input). A panel taller than the room below the HUD (a 720p window) scrolls:
// the wheel over it scrolls, except over a slider or a field, which take the wheel themselves.
// Nothing here allocates.
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
#define PEN_H       40
#define BUTTON_H    28
#define BOTTOM_ROOM 42      // kept free under the panel (the map message line)
#define SCROLL_STEP 48

// Widget ids (unique in the panel, > 0)
enum {
    W_SIZE_S = 1, W_SIZE_F, W_HARD_S, W_HARD_F,
    W_R_S, W_G_S, W_B_S, W_A_S, W_R_F, W_G_F, W_B_F, W_A_F,
    W_HEX, W_DENS_S, W_DENS_F,
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
static const char *PEN_NAMES[PEN_COUNT] = { "round", "square", "soft", "eraser", "fill" };

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
        UiTextBold(name, c->x, ty, UI_S, UI_ACCENT);
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

static bool TargetBox(const DrawMode *dm)
{
    const MS_SurfaceHit *h = &dm->target.hit;
    return TargetSet(dm) && dm->target.ok && h->hit.obj.kind == MS_OBJ_BOX && h->face >= 0 && h->face < 6;
}

// The target's box canvas with a face layout (NULL: none)
static const Canvas *TargetLayoutCanvas(const DrawMode *dm)
{
    int ci = TargetBox(dm) ? dm->target.canvas : -1;
    if (ci < 0 || ci >= dm->canvasCount) return NULL;
    const Canvas *c = &dm->canvases[ci];
    return c->layout && c->file < 0 ? c : NULL;
}

// The face buttons follow the target canvas's layout; the user's unapplied changes stay until the
// target is another canvas (and an undo or an apply brings them in step again)
static void SyncFaces(DrawPanel *p, const DrawMode *dm)
{
    static const unsigned char separate[6] = { 0, 1, 2, 3, 4, 5 };
    const Canvas *c = TargetLayoutCanvas(dm);
    int key = c ? dm->target.canvas : -1;
    const unsigned char *now = c ? c->cells : separate;
    bool edited = memcmp(p->faces, p->facesLoaded, 6) != 0;
    if (key != p->facesCanvas || (!edited && memcmp(p->facesLoaded, now, 6) != 0)) {
        memcpy(p->faces, now, 6);
        memcpy(p->facesLoaded, now, 6);
        p->facesCanvas = key;
    } else if (memcmp(p->faces, now, 6) == 0) memcpy(p->facesLoaded, now, 6);
}

// The pixel look of the target (its material's filter), or of new canvases
static bool PixelLookShown(const DrawMode *dm)
{
    return TargetSet(dm) && dm->target.ok && dm->target.geom.ok && !dm->target.geom.fresh ? dm->target.geom.nearest : dm->pixelLook;
}

// #endregion

// #region Sections

static void Title(Ctx *c)
{
    if (Drawing(c)) {
        UiTextBold("Draw", c->x, c->y + (TITLE_H - UiLineHeight(UI_L)) / 2, UI_L, UI_ACCENT);
        const char *hints = "{TAB} hide|{B} leave";
        UiHints(hints, c->x + c->w - UiHintsWidth(hints, UI_XS), c->y + (TITLE_H - UiLineHeight(UI_XS)) / 2, UI_XS, UI_DIM);
    }
    c->y += TITLE_H;
}

static void SectionTarget(Ctx *c)
{
    DrawMode *dm = c->dm;
    Header(c, "Target", NULL);
    // The shared switch, on the header line (docs/painting.md 3.2)
    Rectangle sr = { (float)(c->x + c->w - 82), (float)(c->y - HEAD_H + 7), 82, 22 };
    if (Button(c, sr, "Shared", dm->shared)) dm->shared = !dm->shared;
    if (Drawing(c)) UiHints("{H}", (int)sr.x - UiHintsWidth("{H}", UI_XS) - 4, (int)sr.y + (22 - UiLineHeight(UI_XS)) / 2, UI_XS, UI_DIM);
    // What the cursor rested on last (dm->target), and what paint there goes into, or why it can't
    const DrawTarget *t = &dm->target;
    bool none = !TargetSet(dm) || t->hit.hit.obj.kind == MS_OBJ_NONE;
    c->y += Wrapped(c, none ? "point at a box or a model" : t->what, UI_S, none ? UI_DIM : UI_INK, 2);
    if (!none && !t->ok && t->refusal[0]) c->y += Wrapped(c, t->refusal, UI_S, UI_ERROR, 3);
    else if (!none && t->into[0]) c->y += Wrapped(c, t->into, UI_S, UI_DIM, 2);
    c->y += GAP;
}

// The pen's icon, centred at cx, cy (shapes from the atlas)
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
        UiRoundRect((Rectangle){ (float)(cx - 10), (float)(cy - 10), 20, 20 }, 10, Fade(ink, 0.22f), BLANK);
        UiRoundRect((Rectangle){ (float)(cx - 8), (float)(cy - 8), 16, 16 }, 8, Fade(ink, 0.35f), BLANK);
        UiRoundRect((Rectangle){ (float)(cx - 5), (float)(cy - 5), 10, 10 }, 5, ink, BLANK);
        break;
    case PEN_ERASER:
        UiRoundRect((Rectangle){ (float)(cx - 10), (float)(cy - 6), 20, 12 }, 3, BLANK, ink);
        UiRoundRect((Rectangle){ (float)(cx - 10), (float)(cy - 6), 9, 12 }, 3, ink, BLANK);
        DrawRectangle(cx - 10, cy + 8, 20, 1, Fade(ink, 0.5f));
        break;
    case PEN_FILL:
        UiRoundRect((Rectangle){ (float)(cx - 9), (float)(cy - 9), 18, 18 }, 3, BLANK, ink);
        DrawRectangle(cx - 6, cy - 6, 12, 12, Fade(ink, 0.75f));
        break;
    default: break;
    }
}

static void SectionPen(Ctx *c)
{
    DrawMode *dm = c->dm;
    Header(c, "Pen", "{1}..{5}|{[}{]} size");
    int bw = (c->w - (PEN_COUNT - 1) * GAP) / PEN_COUNT;
    int left = c->x + (c->w - (bw * PEN_COUNT + (PEN_COUNT - 1) * GAP)) / 2;
    for (int k = 0; k < PEN_COUNT; k++) {
        Rectangle r = { (float)(left + k * (bw + GAP)), (float)c->y, (float)bw, PEN_H };
        bool on = dm->pen == (PenKind)k;
        if (Button(c, r, NULL, on)) dm->pen = (PenKind)k;
        if (Drawing(c)) {
            Color ink = on ? UI_ACCENT : UI_INK;
            int cx = (int)(r.x + r.width / 2);
            PenIcon((PenKind)k, cx, (int)r.y + 13, ink);
            int tw = UiMeasure(PEN_NAMES[k], UI_XS);
            UiText(PEN_NAMES[k], cx - tw / 2, (int)r.y + PEN_H - UiLineHeight(UI_XS) - 3, UI_XS, on ? UI_ACCENT : UI_DIM);
        }
    }
    c->y += PEN_H + GAP;

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

    // Hardness: the soft pen's only (a row of its own while it is the pen)
    if (dm->pen == PEN_SOFT) {
        Row(c, "Hardness", LABEL_W, UI_TEXT, &sr, &fr);
        float hv = dm->hardness;
        if (Slider(c, W_HARD_S, sr, &hv, 0.0f, 1.0f, 0.01f)) dm->hardness = hv;
        hv = dm->hardness;
        if (Field(c, W_HARD_F, fr, &hv, 0.0f, 1.0f, 0.05f, 2)) dm->hardness = hv;
        c->y += ROW_H + ROW_GAP;
    }
    c->y += GAP - ROW_GAP;
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

static void SectionColor(Ctx *c)
{
    DrawMode *dm = c->dm;
    DrawPanel *p = c->p;
    Header(c, "Colour", "{ALT}+LMB picks|{X} swap");
    static const char *names[4] = { "R", "G", "B", "A" };
    static const Color inks[4] = { { 255, 140, 130, 255 }, { 140, 225, 140, 255 }, { 140, 180, 255, 255 }, { 238, 236, 230, 200 } };
    for (int k = 0; k < 4; k++) {
        unsigned char *ch = k == 0 ? &dm->color.r : k == 1 ? &dm->color.g : k == 2 ? &dm->color.b : &dm->color.a;
        Rectangle sr, fr;
        Row(c, names[k], CHANNEL_W, inks[k], &sr, &fr);
        float v = (float)*ch;
        if (Slider(c, W_R_S + k, sr, &v, 0.0f, 255.0f, 1.0f)) *ch = (unsigned char)ClampI((int)roundf(v), 0, 255);
        v = (float)*ch;
        if (Field(c, W_R_F + k, fr, &v, 0.0f, 255.0f, 1.0f, 0)) *ch = (unsigned char)ClampI((int)roundf(v), 0, 255);
        c->y += ROW_H + ROW_GAP;
    }
    c->y += 1;

    // The swatch (left half without alpha, right half over the checker) and the hex field
    Color opaque = dm->color;
    opaque.a = 255;
    Rectangle sw = { (float)c->x, (float)c->y, 34, BUTTON_H };
    Swatch(c, sw, opaque, false);
    Swatch(c, (Rectangle){ sw.x + sw.width, sw.y, 34, BUTTON_H }, dm->color, false);
    Rectangle hr = { sw.x + 2 * sw.width + GAP + 4, (float)c->y, 120, BUTTON_H };
    if (p->edit.active != W_HEX && p->edit.claim != W_HEX) FormatHex(p->hex, sizeof(p->hex), dm->color);
    if (c->pass != PASS_MEASURE && UiTextField(&p->edit, W_HEX, hr, p->hex, sizeof(p->hex)) && c->pass == PASS_INPUT) {
        Color col;
        if (ParseHex(p->hex, &col)) dm->color = col;
        FormatHex(p->hex, sizeof(p->hex), dm->color);      // (an unreadable one goes back)
    }
    Text(c, "hex", (int)(hr.x + hr.width) + 8, c->y + (BUTTON_H - UiLineHeight(UI_XS)) / 2, UI_XS, UI_DIM);
    c->y += BUTTON_H + GAP;

    // The recent colours (a click picks one)
    int n = DRAW_RECENT_COLORS, sz = (c->w - (n - 1) * 4) / n;
    for (int k = 0; k < n; k++) {
        Rectangle r = { (float)(c->x + k * (sz + 4)), (float)c->y, (float)sz, 22 };
        if (k < dm->recentCount) {
            if (Swatch(c, r, dm->recent[k], SameColor(dm->recent[k], dm->color))) dm->color = dm->recent[k];
        } else if (Drawing(c) && dm->recentCount > 0) DrawRectangleLinesEx(r, 1.0f, Fade(UI_LINE, 0.6f));
        // (with none yet the row shows its label alone, not the label over empty boxes)
    }
    if (dm->recentCount == 0) Text(c, "recent colours", c->x + 8, c->y + (22 - UiLineHeight(UI_XS)) / 2, UI_XS, UI_FAINT);
    c->y += 22 + GAP;
}

static void SectionCanvas(Ctx *c)
{
    DrawMode *dm = c->dm;
    Header(c, "Canvas", NULL);
    // Density in texels per metre: the slider over log2 (8 .. 512: 64 in the middle)
    Rectangle sr, fr;
    Row(c, "Texels / m", LABEL_W, UI_TEXT, &sr, &fr);
    float d = dm->density > 0.0f ? dm->density : DRAW_DENSITY_DEFAULT;
    float lv = log2f(d);
    if (Slider(c, W_DENS_S, sr, &lv, 3.0f, 9.0f, 0.0f)) dm->density = (float)ClampI((int)roundf(exp2f(lv)), 8, 512);
    float fv = d;
    if (Field(c, W_DENS_F, fr, &fv, 8.0f, 512.0f, 1.0f, 0)) dm->density = fv;
    c->y += ROW_H + ROW_GAP;
    // The texel size on the target (the canvas's, or the one its first stroke makes), else at the density
    const char *texel = TargetSet(dm) && dm->target.ok && dm->target.texel[0] ? dm->target.texel
                      : TextFormat("new canvases: a texel is about %.1f mm", 1000.0f / d);
    c->y += Wrapped(c, texel, UI_S, UI_DIM, 2) + GAP - 1;
    int bw = (c->w - GAP) / 2;
    if (Button(c, (Rectangle){ (float)c->x, (float)c->y, (float)bw, BUTTON_H }, "Resize to density", false)) c->act = DRAWPANEL_ACT_RESIZE;
    // (the target's own filter when it has a texture; else what new canvases get)
    bool pixel = PixelLookShown(dm);
    if (Button(c, (Rectangle){ (float)(c->x + bw + GAP), (float)c->y, (float)(c->w - bw - GAP), BUTTON_H }, "Pixel look", pixel)) {
        dm->pixelLook = !pixel;
        c->act = DRAWPANEL_ACT_PIXEL_LOOK;
    }
    c->y += BUTTON_H + GAP;
}

// faces[] with the numbers closed up in order (0 2 2 5 -> 0 1 1 2)
static void CloseUp(unsigned char faces[6])
{
    unsigned char map[6];
    int next = 0;
    for (int n = 0; n < 6; n++) {
        bool used = false;
        for (int f = 0; f < 6; f++) used |= faces[f] == n;
        map[n] = (unsigned char)(used ? next++ : 0);
    }
    for (int f = 0; f < 6; f++) faces[f] = map[faces[f] < 6 ? faces[f] : 0];
}

static void SectionFaces(Ctx *c)
{
    DrawPanel *p = c->p;
    const DrawMode *dm = c->dm;
    // (boxes only: a box canvas with a face layout has the buttons; the others say why not)
    if (!TargetBox(dm) && c->p->only != DRAWPANEL_FACES) return;
    Header(c, "Faces", "same number: one cell");
    const Canvas *cv = TargetLayoutCanvas(dm);
    if (!cv) {
        const char *why = !TargetBox(dm) ? "point at a box: its canvas's faces are grouped here"
                        : dm->target.canvas < 0 && dm->target.geom.fresh ? "the first stroke gives every face its own cell; then group them here"
                        : dm->target.geom.layout ? "paint it once, then its faces are grouped here"
                                                : "this texture has no face layout (uv_faces): every face shows all of it";
        c->y += Wrapped(c, why, UI_S, UI_DIM, 2) + GAP;
        return;
    }
    // A button per face: its name (dim) and its cell number; a click counts the number on (lit:
    // changed, not applied yet)
    int bw = (c->w - 5 * GAP) / 6;
    int left = c->x + (c->w - (6 * bw + 5 * GAP)) / 2;
    for (int f = 0; f < 6; f++) {
        Rectangle r = { (float)(left + f * (bw + GAP)), (float)c->y, (float)bw, BUTTON_H };
        if (Button(c, r, NULL, p->faces[f] != cv->cells[f])) p->faces[f] = (unsigned char)((p->faces[f] + 1) % 6);
        if (Drawing(c)) {
            const char *n = TextFormat("%d", p->faces[f] < 6 ? p->faces[f] : 0);
            int nw = UiMeasureBold(n, UI_M), fw = UiMeasure(FACE_NAMES[f], UI_XS);
            int x = (int)r.x + ((int)r.width - fw - 5 - nw) / 2;
            UiText(FACE_NAMES[f], x, (int)r.y + (BUTTON_H - UiLineHeight(UI_XS)) / 2 + 1, UI_XS, UI_DIM);
            UiTextBold(n, x + fw + 5, (int)r.y + (BUTTON_H - UiLineHeight(UI_M)) / 2, UI_M, UI_INK);
        }
    }
    c->y += BUTTON_H + GAP;
    // Apply, and the two presets (they set the buttons; Apply applies them)
    const char *labels[3] = { "Apply", "All separate", "Sides together" };
    int tw[3], total = 0;
    for (int k = 0; k < 3; k++) total += tw[k] = UiMeasure(labels[k], UI_M) + 16;
    int room = c->w - 2 * GAP, x = c->x;
    for (int k = 0; k < 3; k++) {
        int w = k < 2 ? tw[k] * room / total : c->x + c->w - x;
        Rectangle r = { (float)x, (float)c->y, (float)w, BUTTON_H };
        if (Button(c, r, labels[k], k == 0 && memcmp(p->faces, cv->cells, 6) != 0)) {
            if (k == 0) {
                CloseUp(p->faces);
                c->act = DRAWPANEL_ACT_FACES;
            } else {
                static const unsigned char presets[2][6] = { { 0, 1, 2, 3, 4, 5 }, { 0, 0, 1, 2, 0, 0 } };
                memcpy(p->faces, presets[k - 1], 6);
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
    if (only == DRAWPANEL_ALL || only == DRAWPANEL_PEN) SectionPen(c);
    if (only == DRAWPANEL_ALL || only == DRAWPANEL_COLOR) SectionColor(c);
    if (only == DRAWPANEL_ALL || only == DRAWPANEL_CANVAS) SectionCanvas(c);
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
    p->facesCanvas = -2;
    p->interactive = true;
}

bool DrawPanelSetOnly(DrawPanel *p, const char *section)
{
    static const struct { const char *name; DrawPanelSection s; } names[] = {
        { "all", DRAWPANEL_ALL }, { "target", DRAWPANEL_TARGET }, { "pen", DRAWPANEL_PEN }, { "color", DRAWPANEL_COLOR },
        { "colour", DRAWPANEL_COLOR }, { "canvas", DRAWPANEL_CANVAS }, { "faces", DRAWPANEL_FACES },
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
    UiPanel(p->rect, UI_PANEL, UI_LINE);
    if (scrolls) BeginScissorMode((int)p->rect.x + 1, (int)p->rect.y + 1, (int)p->rect.width - 2, (int)p->rect.height - 2);
    c = (Ctx){ .p = p, .dm = &view, .pass = PASS_DRAW, .y = y - p->scroll };
    UiWidgetsBegin(UI_WIDGETS_DRAW, p->interactive && !IsCursorHidden(), p->rect);
    Layout(&c);
    UiWidgetsEnd();
    if (scrolls) {
        EndScissorMode();
        // a thin track and a thumb of the visible share
        int trackH = (int)p->rect.height - 12, thumbH = trackH * (int)p->rect.height / p->contentH;
        if (thumbH < 16) thumbH = 16;
        int most = p->contentH - (int)p->rect.height;
        int thumbY = (int)p->rect.y + 6 + (most > 0 ? (trackH - thumbH) * p->scroll / most : 0);
        int tx = (int)(p->rect.x + p->rect.width) - 5;
        DrawRectangle(tx, (int)p->rect.y + 6, 2, trackH, UI_LINE);
        DrawRectangle(tx - 1, thumbY, 4, thumbH, UI_DIM);
    }
}

bool DrawPanelContains(const DrawPanel *p, Vector2 px)
{
    return p->rect.width > 0 && CheckCollisionPointRec(px, p->rect);
}
