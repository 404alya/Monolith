// The keyboard / mouse shim of the editor (input.c): every src/ module reads input through it
// (common.h maps raylib's input calls here). Without --input-script each call is raylib's own;
// with it the input of a hidden --shot run comes from a script, frame by frame, through the same
// code paths the interactive game uses (docs/command-line.md, --input-script)
#ifndef MONOLITH_INPUT_H
#define MONOLITH_INPUT_H

#include <raylib.h>
#include <stdbool.h>

// Reads the script (false: unreadable or a bad line, message on stderr). The script starts at
// frame `startFrame` of the run; InputScriptFrames() is its length in frames
bool InputScriptLoad(const char *path, int startFrame, int screenW, int screenH);
bool InputScripted(void);           // a script was loaded (the run's input is the script's)
int InputScriptFrames(void);
bool InputScriptDone(void);         // every line was played
// Once per frame, at the top of the main loop (where raylib's PollInputEvents took the events)
void InputFrame(int frame);
void InputScriptFree(void);

bool InputIsKeyPressed(int key);
bool InputIsKeyPressedRepeat(int key);
bool InputIsKeyDown(int key);
int InputGetCharPressed(void);
bool InputIsMouseButtonPressed(int button);
bool InputIsMouseButtonDown(int button);
bool InputIsMouseButtonReleased(int button);
Vector2 InputGetMousePosition(void);
Vector2 InputGetMouseDelta(void);
float InputGetMouseWheelMove(void);
void InputSetMousePosition(int x, int y);
void InputEnableCursor(void);
void InputDisableCursor(void);
bool InputIsCursorHidden(void);

#ifndef MONOLITH_INPUT_IMPL
#define IsKeyPressed(k)             InputIsKeyPressed(k)
#define IsKeyPressedRepeat(k)       InputIsKeyPressedRepeat(k)
#define IsKeyDown(k)                InputIsKeyDown(k)
#define GetCharPressed()            InputGetCharPressed()
#define IsMouseButtonPressed(b)     InputIsMouseButtonPressed(b)
#define IsMouseButtonDown(b)        InputIsMouseButtonDown(b)
#define IsMouseButtonReleased(b)    InputIsMouseButtonReleased(b)
#define GetMousePosition()          InputGetMousePosition()
#define GetMouseDelta()             InputGetMouseDelta()
#define GetMouseWheelMove()         InputGetMouseWheelMove()
#define SetMousePosition(x, y)      InputSetMousePosition(x, y)
#define EnableCursor()              InputEnableCursor()
#define DisableCursor()             InputDisableCursor()
#define IsCursorHidden()            InputIsCursorHidden()
#endif

#endif
