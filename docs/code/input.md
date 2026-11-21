# `src/input.c`: the editor's keyboard / mouse shim

The interface is `input.h`.

## How input is read

Every `src/` module reads input through these functions (`common.h` maps raylib's calls onto them). Without `--input-script` they only call raylib, so the interactive editor is unchanged.

With a script (a hidden `--shot` run) the keys, buttons, characters and the mouse come from it, frame by frame, and the cursor calls behave as raylib 5.5's GLFW platform does (`rcore_desktop_glfw.c`):

- `DisableCursor` and `EnableCursor` both put the cursor in the middle of the window;
- `SetMousePosition` sets the current and the previous position (no delta that frame);
- `GetMouseDelta` is the current minus the previous position (taken at the frame's start).

## The script

One step per line; `#` starts a comment. A step takes one frame unless it says how many.

| Step | Meaning |
|---|---|
| `wait <n>` | n frames without new input |
| `at <x> <y>` | the cursor jumps to (x, y) (window pixels; the delta is the jump) |
| `move <dx> <dy> [n]` | the mouse moves by (dx, dy) px, spread evenly over n frames (default 1) |
| `press <key>` | key down this frame, up the next one (a printable key types its char) |
| `down <key>` / `up <key>` | hold / release a key (SHIFT, CTRL, ALT, ...) |
| `click <left\|right\|middle>` | a button down this frame, up the next one |
| `down lmb\|rmb\|mmb` / `up lmb\|rmb\|mmb` | hold / release a mouse button (a drag: `down lmb`, `move ...`, `up lmb`) |
| `wheel <n>` | the wheel turns n notches this frame (`GetMouseWheelMove` returns n; may be negative or fractional, \|n\| ≤ 1000) |

**Keys:** A-Z, 0-9, ENTER, KP_ENTER, ESCAPE, BACKSPACE, TAB, DELETE, SPACE, MINUS, PERIOD, SHIFT, RSHIFT, CTRL, RCTRL, ALT, RALT, UP, DOWN, LEFT, RIGHT, LBRACKET, RBRACKET, F1 .. F12.
