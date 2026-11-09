// Launcher: request bundles, the request queue + coordinator session (or one Claude Code terminal
// per request), polling for result.md
#ifndef MONOLITH_LAUNCHER_H
#define MONOLITH_LAUNCHER_H

#include "common.h"
#include "designer.h"

// Model choice per request (rules: the "Model choice" comment in launcher.c)
typedef enum { MODEL_TIER_FAST, MODEL_TIER_DEFAULT, MODEL_TIER_HEAVY } ModelTier;

typedef struct {
    const char *prompt;     // as typed; may start with an @tier token
    const char *kind;       // "edit", "import" or "new-map"
    int selected;           // selected objects
    int regions;            // screen regions
    int volumes;            // volumes (world-space boxes)
    bool hit;               // the crosshair hit something
    bool emptyMap;          // the map has (almost) nothing in it (<= 2 objects)
    bool noBrief;           // the map package has no MAP.md
} ModelQuery;

typedef struct {
    ModelTier tier;
    char name[128];         // value of --model ("" never: falls back to the tier's default)
    MonoMsg reason;         // one line, plain ASCII (freed with ModelChoiceFree)
    bool override;          // an @tier token in the prompt chose the tier
    const char *source;     // "classifier", "prompt", "MONOLITH_MODEL" or "MONOLITH_CLAUDE_ARGS"
    bool addFlag;           // false: MONOLITH_CLAUDE_ARGS already has --model, don't add one
    int promptSkip;         // bytes at the start of the prompt taken by the @token (+ blanks)
    bool droppedModelArg;   // MONOLITH_CLAUDE_ARGS had a --model without a name (ignored)
} ModelChoice;

const char *ModelTierName(ModelTier tier);
// Pure classifier (no environment): tier + reason (set in *reason: a literal for fixed
// reasons, a heap text otherwise; MsgFree it); strips nothing
ModelTier ClassifyRequest(const ModelQuery *q, MonoMsg *reason);
// Classifier + @token + MONOLITH_MODEL* / MONOLITH_CLAUDE_ARGS: what the launcher runs.
// *out is overwritten (not freed first): free it with ModelChoiceFree
void LauncherChooseModel(const ModelQuery *q, ModelChoice *out);
// Frees what LauncherChooseModel allocated in *c (its reason)
void ModelChoiceFree(ModelChoice *c);

// The launcher's note: the text of the last LauncherSubmit / LauncherStartBundle /
// LauncherClose* outcome that is not a failure (failures are in MS_LastError()). "" before
// any. Valid until the next of those calls; freed by LauncherShutdown.
const char *LauncherNote(void);

// Writes requests/<stamp>_<slug>/ (view.png, view-marked.png, request.md, request.json) and
// queues it for the coordinator, starting one if none runs (MONOLITH_LAUNCH=per-request:
// launches Claude Code on it in a new terminal). Never blocks on the child. Returns
//   0: queued / sent (a queued bundle counts even if the coordinator couldn't start); the
//      one-line HUD text in LauncherNote() ("request queued [n] ..." / "request sent (...) ...")
//   1: not sent; why in MS_LastError() (it names the bundle path if one was written, so the
//      developer can run it by hand)
unsigned char LauncherSubmit(const DesignRequest *req, Image view, Image marked);

// Hands a bundle someone else wrote into requests/<id>/ (e.g. an import, src/import.c) on
// like LauncherSubmit: queued for the coordinator, or (per-request mode) Claude Code in a new
// terminal with the same flags and workspace handling, and tracks it in the HUD. claudePrompt
// is the one-line instruction on the command line in per-request mode (never user text).
// slug: [a-z0-9-] only (it goes into the window title). model: value of --model (NULL =
// none), e.g. from LauncherChooseModel; coordinator mode only names it in the note.
// Returns 0: queued / sent (the text in LauncherNote()); 1: not sent (MS_LastError())
unsigned char LauncherStartBundle(const char *id, const char *slug, const char *claudePrompt, const char *model);

// requests/ (the repo's while it exists, else next to the executable) and the folder Claude
// Code starts in; both with a trailing slash
const char *LauncherRequestsDir(void);
const char *LauncherRootDir(void);

// Removes MONOLITH_ALLOW_WINDOW from this process's environment, so the Claude Code sessions
// the launcher starts (agents, whose shells set CLAUDECODE) never inherit the override of the
// game's window guard. Call once, after main() has read it and before any launch.
void LauncherScrubEnv(void);

// Polls the open requests for result.md (cheap; call every frame, it rate-limits itself)
void LauncherUpdate(float dt);

// Draws the open/finished request list at (x, y); returns the height used in pixels
int LauncherDrawHud(int x, int y);

// Closing request terminals (launcher.c, region "Closing request windows"). Each sets a
// one-line HUD text ("closed <slug>", "<slug>: window already gone", ...) in LauncherNote(),
// or for the failures of LauncherCloseNumber in MS_LastError(). A closed request leaves the
// HUD list; its bundle stays on disk. Coordinator mode has no request terminals: closing a
// finished request only takes it off the list.
// The most recently finished request. 0: closed (note; logged); 1: no finished request (note)
unsigned char LauncherCloseLatestDone(void);
// Every finished request; returns how many were taken off the list (the note says it; 0:
// "no finished request to close")
int LauncherCloseAllDone(void);
// Request [number] of the HUD list, running ones too: a request without result.md needs a
// second call within 3 s (the first asks for it). Coordinator mode: cancels a queued request
// at once, a running one on the second call. Returns
//   0: closed / cancelled / cancel already asked (note)
//   1: waiting for the number again (note: "press %d again ...")
//   2: no request [number] in the list (MS_LastError())
//   3: the cancel file could not be written (MS_LastError())
unsigned char LauncherCloseNumber(int number);
// Coordinator mode: closes the coordinator's terminal. With requests still queued or running
// it needs a second call within 3 s. Returns (the text always in LauncherNote())
//   0: closed, or told how (logged)
//   1: requests still open: press again
//   2: no coordinator
unsigned char LauncherCloseCoordinator(void);
// Number of the newest listed request (0 if none)
int LauncherNewestNumber(void);
// Requests of this game run that are queued or running on the map file mapPath (absolute or
// relative): how many, and the HUD number of the newest one (*newest, 0 = none)
int LauncherOpenOnMap(const char *mapPath, int *newest);
// A closed request's terminal process is still running (being shut down)
bool LauncherClosing(void);
// MONOLITH_CLOSE_ON_EXIT=1: close every finished request's terminal, or the coordinator if
// none of this run's requests is still open (call when quitting)
void LauncherCloseOnExit(void);

// Camera of the most recently finished request, for "fly back" (false if none)
bool LauncherLastDone(Vector3 *eye, float *yaw, float *pitch);

// Frees the request list and the launcher's texts (call once at exit; requests started later
// are tracked anew)
void LauncherShutdown(void);

#endif
