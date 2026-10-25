// In-game graphics settings panel: two columns of grouped rows (Display, Shadows, Ambient
// occlusion | Fog, Post, Performance), each "label   < value >". Keyboard: UP/DOWN select,
// LEFT/RIGHT change, ENTER presses a button, ESC / F2 close. Mouse: hover selects, click the
// arrows (or the left / right half of the value), wheel changes. While open the camera and the
// game keys are frozen and the cursor is free (like the prompt box).
#include "settingspanel.h"
#include "ui.h"
#include <r3d/r3d_profiler.h>

#define ROW_H       24
#define HEADER_H    30
#define TITLE_H     64
#define FOOTER_H    92
#define PAD         20
#define COL_GAP     36
#define FONT        UI_M
#define VALUE_W     210     // value column incl. arrows
#define ARROW_W     22

typedef struct {
    Rectangle panel;
    Rectangle row[64];          // per settings row (GfxRowCount() <= 64)
    int header[GFX_GROUP_COUNT];    // y of each group header
    int headerX[GFX_GROUP_COUNT];
    Rectangle reset, close;
    int colW;
} Layout;

static int ItemCount(void) { return GfxRowCount() + 2; }    // rows, Reset, Close
static int ResetItem(void) { return GfxRowCount(); }
static int CloseItem(void) { return GfxRowCount() + 1; }

// Groups 0-2 in the left column, 3-5 in the right; rows in table order
static Layout MakeLayout(int w, int h)
{
    Layout L;
    memset(&L, 0, sizeof(L));
    int colH[2] = { 0, 0 };
    GfxGroup last = (GfxGroup)-1;
    for (int r = 0; r < GfxRowCount(); r++) {
        GfxGroup g = GfxRowGroup(r);
        int c = g >= GFX_GROUP_FOG;
        if (g != last) { colH[c] += HEADER_H; last = g; }
        colH[c] += ROW_H;
    }
    int bodyH = colH[0] > colH[1] ? colH[0] : colH[1];
    int pw = w - 40 < 1080 ? w - 40 : 1080;
    int ph = TITLE_H + bodyH + FOOTER_H;
    if (ph > h - 20) ph = h - 20;
    L.panel = (Rectangle){ (float)((w - pw) / 2), (float)((h - ph) / 2), (float)pw, (float)ph };
    if (L.panel.x < 0) L.panel.x = 0;
    if (L.panel.y < 0) L.panel.y = 0;
    L.colW = (pw - 2 * PAD - COL_GAP) / 2;
    int y[2] = { (int)L.panel.y + TITLE_H, (int)L.panel.y + TITLE_H };
    last = (GfxGroup)-1;
    for (int r = 0; r < GfxRowCount() && r < 64; r++) {
        GfxGroup g = GfxRowGroup(r);
        int c = g >= GFX_GROUP_FOG;
        int x = (int)L.panel.x + PAD + c * (L.colW + COL_GAP);
        if (g != last) {
            L.header[g] = y[c] + 8;
            L.headerX[g] = x;
            y[c] += HEADER_H;
            last = g;
        }
        L.row[r] = (Rectangle){ (float)x, (float)y[c], (float)L.colW, (float)ROW_H };
        y[c] += ROW_H;
    }
    float by = L.panel.y + L.panel.height - 40;
    L.close = (Rectangle){ L.panel.x + L.panel.width - PAD - 110, by, 110, 28 };
    L.reset = (Rectangle){ L.close.x - 16 - 170, by, 170, 28 };
    return L;
}

static Rectangle ValueRect(Rectangle row) { return (Rectangle){ row.x + row.width - VALUE_W, row.y, VALUE_W, row.height }; }

void PanelOpen(SettingsPanel *p)
{
    p->open = true;
    if (p->sel < 0 || p->sel >= ItemCount()) p->sel = 0;
    p->gpuMs = -1.0f;
    if (R3D_IsProfilerAvailable() && !R3D_IsProfilerEnabled()) {
        R3D_SetProfilerEnabled(true);   // GPU timer queries per pass, read a few frames later
        p->ownProfiler = true;
    }
    if (p->interactive) EnableCursor();
}

void PanelClose(SettingsPanel *p)
{
    if (!p->open) return;
    p->open = false;
    p->freezeFrames = 2;    // GLFW can report one big mouse jump when the cursor is captured again
    if (p->ownProfiler) { R3D_SetProfilerEnabled(false); p->ownProfiler = false; }
    if (p->interactive) DisableCursor();
}

bool PanelCapturesInput(const SettingsPanel *p)
{
    return p->open || p->freezeFrames > 0;
}

void PanelStatus(SettingsPanel *p, const char *text, float seconds)
{
    snprintf(p->status, sizeof(p->status), "%s", text);
    p->statusTime = seconds;
}

int PanelUpdate(SettingsPanel *p, GfxSettings *s, float dt, int w, int h)
{
    if (p->statusTime > 0.0f) p->statusTime -= dt;
    if (!p->open) {
        if (p->freezeFrames > 0) p->freezeFrames--;
        return -1;
    }
    p->msAvg = p->msAvg > 0.0f ? p->msAvg * 0.9f + dt * 1000.0f * 0.1f : dt * 1000.0f;
    R3D_ProfilerFrame f;
    while (R3D_PopProfilerFrame(&f)) p->gpuMs = f.totalMs;

    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_F2)) { PanelClose(p); return -1; }
    int n = ItemCount();
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) p->sel = (p->sel + 1) % n;
    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) p->sel = (p->sel + n - 1) % n;
    int dir = 0;
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) dir = 1;
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) dir = -1;
    bool press = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);

    // Mouse (only with a free cursor: a real window)
    if (p->interactive) {
        Layout L = MakeLayout(w, h);
        Vector2 m = GetMousePosition();
        static Vector2 lastMouse;
        bool moved = m.x != lastMouse.x || m.y != lastMouse.y;
        lastMouse = m;
        int hover = -1;
        for (int r = 0; r < GfxRowCount() && r < 64; r++) if (CheckCollisionPointRec(m, L.row[r])) hover = r;
        if (CheckCollisionPointRec(m, L.reset)) hover = ResetItem();
        if (CheckCollisionPointRec(m, L.close)) hover = CloseItem();
        if (hover >= 0 && moved) p->sel = hover;
        if (hover >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            p->sel = hover;
            if (hover >= GfxRowCount()) press = true;
            else {
                Rectangle v = ValueRect(L.row[hover]);
                if (CheckCollisionPointRec(m, v)) dir = m.x < v.x + v.width / 2 ? -1 : 1;
                else dir = 1;
            }
        }
        float wheel = GetMouseWheelMove();
        if (hover >= 0 && hover < GfxRowCount() && wheel != 0.0f) { p->sel = hover; dir = wheel > 0 ? 1 : -1; }
    }

    if (p->sel == CloseItem() && press) { PanelClose(p); return -1; }
    if (p->sel == ResetItem() && press) return PANEL_RESET;
    if (p->sel < GfxRowCount()) {
        if (press && !dir) dir = 1;
        if (dir && GfxRowStep(s, p->sel, dir)) return p->sel;
    }
    return -1;
}

static void Button(Rectangle r, const char *text, bool selected)
{
    UiPanel(r, selected ? (Color){ 70, 64, 56, 255 } : UI_RAISED, selected ? UI_ACCENT : UI_LINE);
    int tw = UiMeasure(text, FONT);
    UiText(text, (int)(r.x + (r.width - tw) / 2), (int)(r.y + (r.height - UiLineHeight(FONT)) / 2), FONT, UI_INK);
}

void PanelDraw(const SettingsPanel *p, const GfxSettings *s, int w, int h, bool reused)
{
    if (!p->open) return;
    Layout L = MakeLayout(w, h);
    DrawRectangle(0, 0, w, h, (Color){ 0, 0, 0, 90 });
    UiPanel(L.panel, (Color){ 18, 18, 20, 236 }, UI_LINE);

    // Title + frame time
    int tx = (int)L.panel.x + PAD, ty = (int)L.panel.y + 18;
    UiTextBold("Graphics settings", tx, ty, UI_XL, UI_INK);
    const char *gpu = p->gpuMs >= 0.0f ? TextFormat("GPU %.2f ms", p->gpuMs)
                    : R3D_IsProfilerAvailable() ? "GPU -" : "GPU: profiler not built in";
    const char *timing = TextFormat("%d fps   %.2f ms   %s%s", GetFPS(), p->msAvg, gpu, reused ? "   (cached frame)" : "");
    UiText(timing, (int)(L.panel.x + L.panel.width) - PAD - UiMeasure(timing, FONT), ty + 5, FONT, UI_DIM);
    DrawRectangle((int)L.panel.x + PAD, (int)L.panel.y + TITLE_H - 10, (int)L.panel.width - 2 * PAD, 1, UI_LINE);

    // Values that differ from the preset the settings came from are drawn in blue
    GfxSettings ref = *s;
    ref.q = MS_QualityPreset(s->base);
    GfxSettings defaults = GfxDefaults();
    ref.windowMsaa = defaults.windowMsaa;
    ref.vsync = defaults.vsync;
    ref.fpsCap = defaults.fpsCap;
    ref.frameReuse = defaults.frameReuse;

    for (int g = 0; g < GFX_GROUP_COUNT; g++)
        if (L.header[g]) UiTextBold(GfxGroupName((GfxGroup)g), L.headerX[g], L.header[g], FONT, UI_ACCENT);
    for (int r = 0; r < GfxRowCount() && r < 64; r++) {
        Rectangle row = L.row[r];
        bool sel = p->sel == r;
        if (sel) UiPanel((Rectangle){ row.x - 6, row.y, row.width + 12, row.height }, (Color){ 255, 255, 255, 22 }, BLANK);
        int ty2 = (int)(row.y + (row.height - UiLineHeight(FONT)) / 2);
        UiText(GfxRowLabel(r), (int)row.x + 10, ty2, FONT, sel ? UI_INK : (Color){ 220, 218, 212, 220 });
        if (GfxRowNeedsRestart(r))
            UiPill("restart", (int)row.x + 10 + UiMeasure(GfxRowLabel(r), FONT) + 10, (int)row.y + (ROW_H - UI_PILL_H) / 2,
                   (Color){ 238, 236, 230, 40 }, UI_DIM);
        char v[64], rv[64];
        GfxRowFormat(s, r, v, sizeof(v));
        GfxRowFormat(&ref, r, rv, sizeof(rv));
        bool differs = GfxRowKindOf(r) == GFX_ROW_VALUE && strcmp(v, rv) != 0;
        Rectangle vr = ValueRect(row);
        Color arrow = sel ? UI_ACCENT : (Color){ 238, 236, 230, 70 };
        UiText("<", (int)vr.x + 4, ty2, FONT, arrow);
        UiText(">", (int)(vr.x + vr.width) - ARROW_W + 6, ty2, FONT, arrow);
        int vw = UiMeasure(v, FONT);
        UiText(v, (int)(vr.x + (vr.width - vw) / 2), ty2, FONT,
                 GfxRowKindOf(r) == GFX_ROW_PRESET ? UI_ACCENT : differs ? UI_CHANGED : UI_INK);
    }

    // Footer: the selected row's hint, status, buttons
    int fy = (int)(L.panel.y + L.panel.height) - FOOTER_H + 6;
    DrawRectangle((int)L.panel.x + PAD, fy, (int)L.panel.width - 2 * PAD, 1, UI_LINE);
    const char *hint = p->sel < GfxRowCount() ? GfxRowHint(p->sel)
                     : p->sel == ResetItem() ? TextFormat("every setting back to the %s preset", MS_QualityName(s->base))
                     : "close the panel (F2 / ESC)";
    UiText(hint, tx, fy + 10, UI_S, UI_INK);
    if (p->statusTime > 0.0f && p->status[0])
        UiText(p->status, tx, fy + 34, UI_S, Fade(UI_ACCENT, fminf(p->statusTime, 1.0f)));
    UiHints("{UP}/{DOWN} select|{LEFT}/{RIGHT} change|mouse: arrows / wheel|blue = changed from the preset",
            tx, (int)L.close.y + (28 - UiLineHeight(UI_XS)) / 2, UI_XS, UI_DIM);
    Button(L.reset, "Reset to preset", p->sel == ResetItem());
    Button(L.close, "Close", p->sel == CloseItem());
}
