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
    bool hit;               // the crosshair hit something
    bool emptyMap;          // the map has (almost) nothing in it (<= 2 objects)
    bool noBrief;           // the map package has no MAP.md
} ModelQuery;

typedef struct {
    ModelTier tier;
    char name[128];         // value of --model ("" never: falls back to the tier's default)
    char reason[256];       // one line, plain ASCII
    bool override;          // an @tier token in the prompt chose the tier
    const char *source;     // "classifier", "prompt", "MONOLITH_MODEL" or "MONOLITH_CLAUDE_ARGS"
    bool addFlag;           // false: MONOLITH_CLAUDE_ARGS already has --model, don't add one
    int promptSkip;         // bytes at the start of the prompt taken by the @token (+ blanks)
    bool droppedModelArg;   // MONOLITH_CLAUDE_ARGS had a --model without a name (ignored)
} ModelChoice;

const char *ModelTierName(ModelTier tier);
// Pure classifier (no environment): tier + reason; strips nothing
ModelTier ClassifyRequest(const ModelQuery *q, char *reason, int reasonSize);
// Classifier + @token + MONOLITH_MODEL* / MONOLITH_CLAUDE_ARGS: what the launcher runs
void LauncherChooseModel(const ModelQuery *q, ModelChoice *out);

// Writes requests/<stamp>_<slug>/ (view.png, view-marked.png, request.md, request.json) and
// queues it for the coordinator, starting one if none runs (MONOLITH_LAUNCH=per-request:
// launches Claude Code on it in a new terminal). Never blocks on the child. Returns false on
// failure (a queued bundle counts as success even if the coordinator couldn't start); either
// way msg gets a one-line HUD text (on failure it names the bundle path if one was written,
// so the developer can run it by hand).
bool LauncherSubmit(const DesignRequest *req, Image view, Image marked, char *msg, int msgSize);

// Hands a bundle someone else wrote into requests/<id>/ (e.g. an import, src/import.c) on
// like LauncherSubmit: queued for the coordinator, or (per-request mode) Claude Code in a new
// terminal with the same flags and workspace handling, and tracks it in the HUD. claudePrompt
// is the one-line instruction on the command line in per-request mode (never user text).
// slug: [a-z0-9-] only (it goes into the window title). model: value of --model (NULL =
// none), e.g. from LauncherChooseModel; coordinator mode only names it in msg
bool LauncherStartBundle(const char *id, const char *slug, const char *claudePrompt, const char *model, char *msg, int msgSize);

// requests/ (the repo's while it exists, else next to the executable) and the folder Claude
// Code starts in; both with a trailing slash
const char *LauncherRequestsDir(void);
const char *LauncherRootDir(void);

// Polls the open requests for result.md (cheap; call every frame, it rate-limits itself)
void LauncherUpdate(float dt);

// Draws the open/finished request list at (x, y); returns the height used in pixels
int LauncherDrawHud(int x, int y);

// Closing request terminals (launcher.c, region "Closing request windows"). Each fills msg
// with a one-line HUD text ("closed <slug>", "<slug>: window already gone", ...). A closed
// request leaves the HUD list; its bundle stays on disk. Coordinator mode has no request
// terminals: closing a finished request only takes it off the list.
// The most recently finished request:
bool LauncherCloseLatestDone(char *msg, int msgSize);
// Every finished request; returns how many were taken off the list
int LauncherCloseAllDone(char *msg, int msgSize);
// Request [number] of the HUD list, running ones too: a request without result.md needs a
// second call within 3 s (the first returns 0 and asks for it). 1 = closed, -1 = no such number.
// Coordinator mode: cancels a queued request at once, a running one on the second call
int LauncherCloseNumber(int number, char *msg, int msgSize);
// Coordinator mode: closes the coordinator's terminal. With requests still queued or running
// it needs a second call within 3 s (the first returns 0). 1 = closed (or told how), -1 = none
int LauncherCloseCoordinator(char *msg, int msgSize);
// Number of the newest listed request (0 if none)
int LauncherNewestNumber(void);
// A closed request's terminal process is still running (being shut down)
bool LauncherClosing(void);
// MONOLITH_CLOSE_ON_EXIT=1: close every finished request's terminal, or the coordinator if
// none of this run's requests is still open (call when quitting)
void LauncherCloseOnExit(void);

// Camera of the most recently finished request, for "fly back" (false if none)
bool LauncherLastDone(Vector3 *eye, float *yaw, float *pitch);

// Frees the request list (call once at exit; requests started later are tracked anew)
void LauncherShutdown(void);

#endif
