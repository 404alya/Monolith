// input -- the editor's keyboard / mouse shim (input.h)
//
// Every src/ module reads input through these functions (common.h maps raylib's calls onto
// them). Without --input-script they only call raylib, so the interactive game is unchanged.
// With a script (a hidden --shot run) the keys, buttons, characters and the mouse come from it,
// frame by frame, and the cursor calls behave as raylib 5.5's GLFW platform does
// (rcore_desktop_glfw.c): DisableCursor and EnableCursor both put the cursor in the middle of
// the window, SetMousePosition sets the current and the previous position (no delta that
// frame), GetMouseDelta is the current minus the previous position (taken at the frame's start).
//
// The script: one step per line, '#' starts a comment. A step takes one frame unless it says
// how many:
//   wait <n>              n frames without new input
//   at <x> <y>            the cursor jumps to (x, y) (window pixels; the delta is the jump)
//   move <dx> <dy> [n]    the mouse moves by (dx, dy) px, spread evenly over n frames (default 1)
//   press <key>           key down this frame, up the next one (a printable key types its char)
//   down <key> / up <key> hold / release a key (SHIFT, CTRL, ALT, ...)
//   click <left|right>    a button down this frame, up the next one
// Keys: A-Z, 0-9, ENTER, KP_ENTER, ESCAPE, BACKSPACE, TAB, DELETE, SPACE, MINUS, PERIOD,
// SHIFT, RSHIFT, CTRL, RCTRL, ALT, RALT, UP, DOWN, LEFT, RIGHT.
#define MONOLITH_INPUT_IMPL
#include "input.h"
#include "monolith_scene.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_KEYS 512

typedef enum { STEP_WAIT, STEP_AT, STEP_MOVE, STEP_PRESS, STEP_DOWN, STEP_UP, STEP_CLICK } StepKind;

typedef struct {
    StepKind kind;
    int key, frames;
    float x, y;
} Step;

static struct {
    bool on;
    Step *steps;
    int count, cap, total, start;
    int at, done;                   // the current step, frames of it played
    bool down[INPUT_KEYS], prevDown[INPUT_KEYS], release[INPUT_KEYS];
    bool button[3], prevButton[3], releaseButton[3];
    int ch;                         // the character typed this frame (0: none)
    Vector2 pos, prevPos;
    bool hidden;
    int w, h;
} S;

static const struct { const char *name; int key; } KEY_NAMES[] = {
    { "ENTER", KEY_ENTER }, { "KP_ENTER", KEY_KP_ENTER }, { "ESCAPE", KEY_ESCAPE }, { "ESC", KEY_ESCAPE },
    { "BACKSPACE", KEY_BACKSPACE }, { "TAB", KEY_TAB }, { "DELETE", KEY_DELETE }, { "SPACE", KEY_SPACE },
    { "MINUS", KEY_MINUS }, { "PERIOD", KEY_PERIOD }, { "SHIFT", KEY_LEFT_SHIFT }, { "RSHIFT", KEY_RIGHT_SHIFT },
    { "CTRL", KEY_LEFT_CONTROL }, { "RCTRL", KEY_RIGHT_CONTROL }, { "ALT", KEY_LEFT_ALT }, { "RALT", KEY_RIGHT_ALT },
    { "UP", KEY_UP }, { "DOWN", KEY_DOWN }, { "LEFT", KEY_LEFT }, { "RIGHT", KEY_RIGHT },
};

static int KeyByName(const char *w)
{
    if (w[0] && !w[1]) {
        int c = toupper((unsigned char)w[0]);
        if (c >= 'A' && c <= 'Z') return KEY_A + (c - 'A');
        if (c >= '0' && c <= '9') return KEY_ZERO + (c - '0');
    }
    for (size_t i = 0; i < sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]); i++)
        if (strcmp(w, KEY_NAMES[i].name) == 0) return KEY_NAMES[i].key;
    return -1;
}

// The character a key types (GLFW's char callback), 0: none
static int KeyChar(int key, bool shift)
{
    if (key >= KEY_A && key <= KEY_Z) return (shift ? 'A' : 'a') + (key - KEY_A);
    if (key >= KEY_ZERO && key <= KEY_NINE) return '0' + (key - KEY_ZERO);
    if (key == KEY_PERIOD) return '.';
    if (key == KEY_MINUS) return '-';
    if (key == KEY_SPACE) return ' ';
    return 0;
}

bool InputScriptLoad(const char *path, int startFrame, int screenW, int screenH)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "monolith: --input-script: cannot read %s\n", path); return false; }
    char *line = NULL;
    size_t lineCap = 0;
    int lineNo = 0;
    bool ok = true;
    while (ok) {
        // (a line of any length: getline-like growth with fgets)
        size_t len = 0;
        int c;
        while ((c = fgetc(f)) != EOF && c != '\n') {
            if (len + 2 > lineCap) {
                size_t cap = lineCap ? lineCap * 2 : 128;
                char *nl = realloc(line, cap);
                if (!nl) { ok = false; break; }
                line = nl;
                lineCap = cap;
            }
            line[len++] = (char)c;
        }
        if (!ok) { fprintf(stderr, "monolith: --input-script: out of memory\n"); break; }
        if (c == EOF && len == 0) break;
        lineNo++;
        if (!line) continue;
        line[len] = '\0';
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char w[4][32] = { { 0 } };
        int n = sscanf(line, "%31s %31s %31s %31s", w[0], w[1], w[2], w[3]);
        if (n <= 0) continue;
        Step st = { .frames = 1 };
        char *end;
        bool bad = false;
        if (strcmp(w[0], "wait") == 0 && n == 2) {
            st.kind = STEP_WAIT;
            st.frames = (int)strtol(w[1], &end, 10);
            bad = *end || st.frames < 1 || st.frames > 100000;
        } else if ((strcmp(w[0], "at") == 0 && n == 3) || (strcmp(w[0], "move") == 0 && (n == 3 || n == 4))) {
            st.kind = w[0][0] == 'a' ? STEP_AT : STEP_MOVE;
            st.x = strtof(w[1], &end);
            bad = *end != '\0';
            st.y = strtof(w[2], &end);
            bad = bad || *end;
            if (n == 4) {
                st.frames = (int)strtol(w[3], &end, 10);
                bad = bad || *end || st.frames < 1 || st.frames > 100000;
            }
        } else if ((strcmp(w[0], "press") == 0 || strcmp(w[0], "down") == 0 || strcmp(w[0], "up") == 0) && n == 2) {
            st.kind = w[0][0] == 'p' ? STEP_PRESS : w[0][0] == 'd' ? STEP_DOWN : STEP_UP;
            st.key = KeyByName(w[1]);
            bad = st.key < 0;
        } else if (strcmp(w[0], "click") == 0 && n == 2) {
            st.kind = STEP_CLICK;
            st.key = strcmp(w[1], "left") == 0 ? MOUSE_BUTTON_LEFT : strcmp(w[1], "right") == 0 ? MOUSE_BUTTON_RIGHT : -1;
            bad = st.key < 0;
        } else bad = true;
        if (bad) {
            fprintf(stderr, "monolith: --input-script %s:%d: not a step: %s\n", path, lineNo, line);
            ok = false;
            break;
        }
        Step *slot = MS_PUSH(S.steps, S.count, S.cap);
        if (!slot) { fprintf(stderr, "monolith: --input-script: out of memory\n"); ok = false; break; }
        *slot = st;
        S.total += st.frames;
    }
    free(line);
    fclose(f);
    if (!ok) { InputScriptFree(); return false; }
    S.on = true;
    S.start = startFrame;
    S.w = screenW;
    S.h = screenH;
    S.pos = S.prevPos = (Vector2){ screenW * 0.5f, screenH * 0.5f };
    return true;
}

bool InputScripted(void) { return S.on; }
int InputScriptFrames(void) { return S.total; }
bool InputScriptDone(void) { return !S.on || S.at >= S.count; }

void InputScriptFree(void)
{
    free(S.steps);
    memset(&S, 0, sizeof(S));
}

void InputFrame(int frame)
{
    if (!S.on) return;
    // What raylib's PollInputEvents does before the new events: the previous states
    memcpy(S.prevDown, S.down, sizeof(S.down));
    memcpy(S.prevButton, S.button, sizeof(S.button));
    S.prevPos = S.pos;
    S.ch = 0;
    for (int k = 0; k < INPUT_KEYS; k++)
        if (S.release[k]) { S.down[k] = false; S.release[k] = false; }
    for (int b = 0; b < 3; b++)
        if (S.releaseButton[b]) { S.button[b] = false; S.releaseButton[b] = false; }
    if (frame < S.start || S.at >= S.count) return;
    // The new events: the current step's share of this frame
    const Step *st = &S.steps[S.at];
    switch (st->kind) {
    case STEP_WAIT: break;
    case STEP_AT: S.pos = (Vector2){ st->x, st->y }; break;
    case STEP_MOVE: {
        // (frame i of n moves to round((i + 1) / n) of the way: the steps sum exactly)
        float x0 = roundf(st->x * S.done / st->frames), x1 = roundf(st->x * (S.done + 1) / st->frames);
        float y0 = roundf(st->y * S.done / st->frames), y1 = roundf(st->y * (S.done + 1) / st->frames);
        S.pos.x += x1 - x0;
        S.pos.y += y1 - y0;
        break;
    }
    case STEP_PRESS:
        S.down[st->key] = true;
        S.release[st->key] = true;
        S.ch = KeyChar(st->key, S.down[KEY_LEFT_SHIFT] || S.down[KEY_RIGHT_SHIFT]);
        break;
    case STEP_DOWN: S.down[st->key] = true; break;
    case STEP_UP: S.down[st->key] = false; break;
    case STEP_CLICK:
        S.button[st->key] = true;
        S.releaseButton[st->key] = true;
        break;
    }
    if (++S.done >= st->frames) { S.at++; S.done = 0; }
}

static bool KeyOk(int key) { return key > 0 && key < INPUT_KEYS; }

bool InputIsKeyPressed(int key)
{
    if (!S.on) return IsKeyPressed(key);
    return KeyOk(key) && S.down[key] && !S.prevDown[key];
}

bool InputIsKeyPressedRepeat(int key) { return S.on ? false : IsKeyPressedRepeat(key); }

bool InputIsKeyDown(int key)
{
    if (!S.on) return IsKeyDown(key);
    return KeyOk(key) && S.down[key];
}

int InputGetCharPressed(void)
{
    if (!S.on) return GetCharPressed();
    int c = S.ch;
    S.ch = 0;
    return c;
}

static bool ButtonOk(int b) { return b >= 0 && b < 3; }

bool InputIsMouseButtonPressed(int b)
{
    if (!S.on) return IsMouseButtonPressed(b);
    return ButtonOk(b) && S.button[b] && !S.prevButton[b];
}

bool InputIsMouseButtonDown(int b)
{
    if (!S.on) return IsMouseButtonDown(b);
    return ButtonOk(b) && S.button[b];
}

bool InputIsMouseButtonReleased(int b)
{
    if (!S.on) return IsMouseButtonReleased(b);
    return ButtonOk(b) && !S.button[b] && S.prevButton[b];
}

Vector2 InputGetMousePosition(void) { return S.on ? S.pos : GetMousePosition(); }

Vector2 InputGetMouseDelta(void)
{
    if (!S.on) return GetMouseDelta();
    return (Vector2){ S.pos.x - S.prevPos.x, S.pos.y - S.prevPos.y };
}

float InputGetMouseWheelMove(void) { return S.on ? 0.0f : GetMouseWheelMove(); }

void InputSetMousePosition(int x, int y)
{
    if (!S.on) { SetMousePosition(x, y); return; }
    S.pos = S.prevPos = (Vector2){ (float)x, (float)y };
}

// (raylib 5.5, GLFW: both put the cursor in the middle of the window through SetMousePosition)
void InputEnableCursor(void)
{
    if (!S.on) { EnableCursor(); return; }
    InputSetMousePosition(S.w / 2, S.h / 2);
    S.hidden = false;
}

void InputDisableCursor(void)
{
    if (!S.on) { DisableCursor(); return; }
    InputSetMousePosition(S.w / 2, S.h / 2);
    S.hidden = true;
}

bool InputIsCursorHidden(void) { return S.on ? S.hidden : IsCursorHidden(); }
