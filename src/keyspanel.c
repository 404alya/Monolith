// The keybindings panel and the ESC menu
//
// The HUD shows state, not keys: every key lives here, one section per mode. ESC with nothing
// else to close shows a single "See keybindings" button (ENTER or a click opens the panel, ESC
// dismisses it). In the panel LEFT / RIGHT (or a click on a section tab) change the section,
// typing searches every section at once, UP / DOWN / PGUP / PGDN / HOME / END scroll, ESC or F1
// closes. While the menu or the panel is open the camera and the game keys are frozen and the
// cursor is free (like the settings panel). Keep the table in step with src/main.c's header
// comment and docs/user-guide.md.
#include "keyspanel.h"
#include <ctype.h>

typedef struct { int section; const char *keys, *action; } KeyRow;

static const char *const SECTIONS[] = {
    "Walking", "Spectator", "Selecting and requests", "Snip and volumes", "Edit mode", "Grab, rotate, scale", "Rotate gizmo",
    "Panels and views",
};
#define SECTION_COUNT ((int)(sizeof(SECTIONS) / sizeof(SECTIONS[0])))

static const KeyRow ROWS[] = {
    { 0, "WASD + mouse", "walk and look" },
    { 0, "SHIFT", "run" },
    { 0, "SPACE", "jump" },
    { 0, "F", "fly (no collision): SPACE up, CTRL down, SHIFT x4" },
    { 0, "TAB", "spectator mode and back" },
    { 0, "1 .. 6", "toggle volumetric fog, distance fog, SSAO, SSIL, bloom, sun (relative to the map)" },
    { 0, "F1", "hide / show the HUD" },
    { 0, "F9", "export the open map to export/<map>-<stamp>/" },
    { 0, "F12", "save screenshot.png" },
    { 0, "ESC", "this menu (when there is nothing else to close)" },

    { 1, "WASD + mouse", "fly and look" },
    { 1, "E / Q, SPACE", "up / down" },
    { 1, "mouse wheel", "base speed (SHIFT x4, ALT x0.25)" },
    { 1, "LEFT / RIGHT", "fly to the previous / next viewpoint" },
    { 1, "P", "print and copy the view as --pos / --look" },
    { 1, "G", "move the player here" },
    { 1, "R", "fly back to where the last finished request was made" },
    { 1, "TAB", "back to walking" },

    { 2, "LMB", "select the object under the crosshair (edit mode: under the cursor)" },
    { 2, "SHIFT+LMB", "add / remove that object" },
    { 2, "CTRL+LMB", "select its whole group (CTRL+SHIFT+LMB: add it)" },
    { 2, "RMB", "clear the selection (edit mode: a short click)" },
    { 2, "ENTER", "the request prompt box" },
    { 2, "in the prompt box", "type; SHIFT+ENTER new line, CTRL+V paste, ENTER send, ESC cancel" },
    { 2, "K / SHIFT+K", "take the latest / all finished requests off the list" },
    { 2, "CTRL+K, then n", "request n: finished off the list, queued cancel, running: n again cancels" },
    { 2, "CTRL+SHIFT+K", "close the coordinator's terminal" },

    { 3, "C", "snip mode: drag screen regions (SHIFT+drag adds one); ENTER prompt; RMB / ESC / C leave" },
    { 3, "V", "volume mode: LMB first corner, LMB opposite corner, mouse / wheel height, LMB / ENTER add" },
    { 3, "SHIFT+V", "a volume from the selection's bounds" },
    { 3, "[ / ]", "volume mode: the previous / next volume" },
    { 3, "arrows, PGUP / PGDN", "volume mode: move the active volume one snap step" },
    { 3, "ALT (+SHIFT) + arrows", "volume mode: grow (shrink) that side" },
    { 3, "DELETE / SHIFT+DELETE", "volume mode: remove the active volume / all of them" },
    { 3, "N", "snap step 0.1 / 0.25 / 0.5 / 1 / 2 m (CTRL: no snap)" },

    { 4, "M", "enter / leave edit mode (ESC leaves too)" },
    { 4, "RMB held", "fly: mouse look, WASD / E / Q / SPACE, wheel speed" },
    { 4, "LMB, SHIFT+LMB, CTRL+LMB", "select under the cursor, add / remove, its group" },
    { 4, "G / ALT+R / S", "grab / rotate / scale the selection with the mouse" },
    { 4, "R / SHIFT+R", "turn 90 / -90 deg about the vertical, each object about its own centre" },
    { 4, "ALT+SHIFT+R", "reset the rotation" },
    { 4, "arrows, PGUP / PGDN", "move one snap step (CTRL: exactly one step, no grid)" },
    { 4, "N", "snap step 0.1 / 0.25 / 0.5 / 1 / 2 m" },
    { 4, "SHIFT+N", "angle snap 1 / 5 / 15 / 45 / 90 deg (CTRL in a rotate)" },
    { 4, "DELETE", "remove" },
    { 4, "CTRL+D", "duplicate" },
    { 4, "I", "the model picker: add a model at the cursor" },
    { 4, "CTRL+I", "give the selection's line an id" },
    { 4, "CTRL+S", "save the edits to the map file" },
    { 4, "CTRL+Z / CTRL+SHIFT+Z", "undo / redo (also CTRL+Y)" },
    { 4, "CTRL+SHIFT+DELETE", "clear the red list of edits that could not be applied again" },

    { 5, "move the mouse", "the value: grab follows the cursor, rotate turns, scale by distance" },
    { 5, "X / Y / Z", "lock to that axis; again: the active object's own axis; a third time: no lock" },
    { 5, "SHIFT+X / Y / Z", "lock to the plane of the other two axes" },
    { 5, "digits . - BACKSPACE", "type an exact value (metres, degrees, factor); - flips the sign" },
    { 5, "CTRL held", "snap: the N grid, the angle snap (SHIFT+N), steps of 0.1" },
    { 5, "SHIFT held", "ten times finer" },
    { 5, "G / R / S", "switch to that kind, from where the objects started" },
    { 5, "LMB / ENTER", "apply (one undo step)" },
    { 5, "RMB / ESC", "cancel: everything back as it was" },

    { 6, "LMB drag on a ring", "turn about that axis (X red, Y green, Z blue); the release applies" },
    { 6, "LMB drag on the white ring", "turn about the view axis (the dial)" },
    { 6, "LMB drag inside", "trackball: turn about the axis across the drag" },
    { 6, "LMB click on a part", "keep it open: the mouse turns, LMB / ENTER apply, RMB / ESC cancel" },
    { 6, "CTRL / SHIFT held", "snap to the angle snap / ten times finer" },
    { 6, "digits . - BACKSPACE", "type the angle (the trackball: about the view's up)" },
    { 6, "L", "the gizmo's axes: global / the active object's own" },

    { 7, "F2", "graphics settings: UP / DOWN select, LEFT / RIGHT change, ESC close" },
    { 7, "F3", "inspector outlines: by material, by shader, by draw kind, off" },
    { 7, "SHIFT+F3", "inspector panel: SPACE hide / show, I alone (CTRL+I while typing)" },
    { 7, "F4 / SHIFT+F4", "buffer view: albedo, normal, ORM, SSAO, SSIL, ... and back" },
    { 7, "F5", "id panel: ENTER fly there, DELETE remove the id, SHIFT+DELETE the objects" },
    { 7, "I (edit mode)", "model picker: type to filter, ENTER place at the cursor, ESC close" },
    { 7, "in a list panel", "type to filter, UP / DOWN / PGUP / PGDN / HOME / END select" },
};
#define ROW_COUNT ((int)(sizeof(ROWS) / sizeof(ROWS[0])))

#define KEYS_W      300     // the keys column (px)
#define HINTS       "{<-} {->} section|type to search every section|{UP} {DOWN} scroll|{ESC} close"

// Case-insensitive substring
static bool Contains(const char *s, const char *needle)
{
    for (; *s; s++) {
        int k = 0;
        while (needle[k] && s[k] && tolower((unsigned char)s[k]) == tolower((unsigned char)needle[k])) k++;
        if (!needle[k]) return true;
    }
    return false;
}

static bool Match(int item, const char *filter, void *user)
{
    (void)user;
    return Contains(ROWS[item].keys, filter) || Contains(ROWS[item].action, filter) || Contains(SECTIONS[ROWS[item].section], filter);
}

// Without a filter: only the rows of the current section
static bool MatchSection(int item, const char *filter, void *user)
{
    (void)filter;
    return ROWS[item].section == *(const int *)user;
}

static void Refilter(KeysPanel *k)
{
    if (k->list.filterLen > 0) UiListRefilter(&k->list, ROW_COUNT, Match, k);
    else {
        // UiListRefilter passes everything when the filter is empty: filter by section by hand
        k->list.rowCount = 0;
        for (int i = 0; i < ROW_COUNT; i++) {
            if (!MatchSection(i, "", &k->section)) continue;
            int *slot = MS_PUSH(k->list.rows, k->list.rowCount, k->list.rowCap);
            if (slot) *slot = i;
        }
        if (k->list.sel >= k->list.rowCount) k->list.sel = k->list.rowCount > 0 ? k->list.rowCount - 1 : 0;
        k->list.dirty = false;
    }
}

static void Close(KeysPanel *k)
{
    if (!k->open && !k->menu) return;
    k->open = k->menu = false;
    k->freezeFrames = 2;    // GLFW can report one big mouse jump when the cursor is captured again
    if (k->interactive) DisableCursor();
}

static void OpenPanel(KeysPanel *k)
{
    k->menu = false;
    k->open = true;
    UiListClear(&k->list);
    while (GetCharPressed()) {}     // (nothing typed before it opened goes into the search)
    Refilter(k);
    if (k->interactive) EnableCursor();
}

void KeysOpen(KeysPanel *k, const char *section)
{
    k->section = 0;
    for (int s = 0; section && s < SECTION_COUNT; s++)
        if (Contains(SECTIONS[s], section)) { k->section = s; break; }
    OpenPanel(k);
}

bool KeysCapturesInput(const KeysPanel *k) { return k->menu || k->open || k->freezeFrames > 0; }

void KeysUpdate(KeysPanel *k, bool escFree)
{
    if (!k->menu && !k->open) {
        if (k->freezeFrames > 0) k->freezeFrames--;
        else if (escFree && IsKeyPressed(KEY_ESCAPE)) {
            k->menu = true;
            if (k->interactive) EnableCursor();
        }
        return;
    }
    if (k->menu) {
        bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), k->button);
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || click) OpenPanel(k);
        else if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) Close(k);
        while (GetCharPressed()) {}
        return;
    }
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_F1)) { Close(k); return; }
    int step = IsKeyPressed(KEY_RIGHT) ? 1 : IsKeyPressed(KEY_LEFT) ? -1 : 0;
    if (step && k->list.filterLen == 0) {
        k->section = (k->section + step + SECTION_COUNT) % SECTION_COUNT;
        k->list.sel = k->list.scroll = 0;
        Refilter(k);
    }
    UiListKeys(&k->list, k->visibleRows);
    if (k->list.dirty) Refilter(k);
}

static void DrawRow(int item, Rectangle row, bool selected, void *user)
{
    const KeysPanel *k = user;
    const KeyRow *r = &ROWS[item];
    int ty = (int)row.y + ((int)row.height - UiLineHeight(UI_S)) / 2;
    int x = (int)row.x + 12;
    UiTextBold(r->keys, x, ty, UI_S, UI_ACCENT);
    int ax = x + KEYS_W;
    if (k->list.filterLen > 0) {
        // searching every section: say which one the row is from
        const char *sec = TextFormat("%s:", SECTIONS[r->section]);
        UiText(sec, ax, ty, UI_S, UI_DIM);
        ax += UiMeasure(sec, UI_S) + 8;
    }
    UiText(r->action, ax, ty, UI_S, selected ? UI_INK : UI_TEXT);
}

void KeysDraw(KeysPanel *k, int screenW, int screenH)
{
    if (k->menu) {
        // One button in the middle of the screen
        const char *label = "See keybindings";
        int bw = UiMeasureBold(label, UI_L) + 48, bh = 44;
        const char *hint = "{ENTER} open|{ESC} back";
        int pw = (bw > UiHintsWidth(hint, UI_S) ? bw : UiHintsWidth(hint, UI_S)) + 48, ph = bh + 70;
        Rectangle panel = { (float)((screenW - pw) / 2), (float)((screenH - ph) / 2), (float)pw, (float)ph };
        UiPanel(panel, UI_PANEL, UI_LINE);
        k->button = (Rectangle){ panel.x + (pw - bw) / 2.0f, panel.y + 20, (float)bw, (float)bh };
        bool hover = CheckCollisionPointRec(GetMousePosition(), k->button);
        UiPanel(k->button, hover ? UI_ACCENT : UI_RAISED, UI_ACCENT);
        UiTextBold(label, (int)k->button.x + 24, (int)k->button.y + (bh - UiLineHeight(UI_L)) / 2, UI_L,
                   hover ? (Color){ 16, 16, 18, 255 } : UI_INK);
        UiHints(hint, (int)panel.x + (pw - UiHintsWidth(hint, UI_S)) / 2, (int)(k->button.y + bh + 14), UI_S, UI_DIM);
        return;
    }
    if (!k->open) return;
    float w = fminf(1100.0f, (float)screenW - 40.0f), h = fminf(660.0f, (float)screenH - 120.0f);
    Rectangle r = { ((float)screenW - w) / 2.0f, ((float)screenH - h) / 2.0f + 18.0f, w, h };
    // Section tabs above the list (click one to show it)
    int tx = (int)r.x, ty = (int)r.y - 34;
    for (int s = 0; s < SECTION_COUNT; s++) {
        int tw = UiMeasure(SECTIONS[s], UI_S) + 20;
        Rectangle tab = { (float)tx, (float)ty, (float)tw, 28 };
        bool on = s == k->section && k->list.filterLen == 0;
        if (k->interactive && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), tab)) {
            k->section = s;
            UiListClear(&k->list);
            Refilter(k);
            on = true;
        }
        UiPanel(tab, on ? UI_RAISED : UI_PANEL, on ? UI_ACCENT : UI_LINE);
        UiText(SECTIONS[s], tx + 10, ty + (28 - UiLineHeight(UI_S)) / 2, UI_S, on ? UI_ACCENT : UI_DIM);
        tx += tw + 6;
    }
    const char *title = k->list.filterLen > 0 ? "Keybindings: every section" : TextFormat("Keybindings: %s", SECTIONS[k->section]);
    k->visibleRows = UiListDraw(&k->list, r, title, 26, DrawRow, k, HINTS);
}

void KeysFree(KeysPanel *k)
{
    UiListFree(&k->list);
    memset(k, 0, sizeof(*k));
}
