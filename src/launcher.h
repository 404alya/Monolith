// Launcher: request bundles, the request queue + coordinator session (or one Claude Code terminal
// per request), polling for result.md
#ifndef MONOLITH_LAUNCHER_H
#define MONOLITH_LAUNCHER_H

#include "common.h"
#include "designer.h"

// Model choice per request (the "Model choice" comment in launcher.c; docs/request-queue.md "Models")
typedef enum { MODEL_TIER_FAST, MODEL_TIER_DEFAULT, MODEL_TIER_HEAVY } ModelTier;

// What the model choice knows about a request: the prompt for the @token, the rest for the AI
// classifier's input (src/classifier.h, ClassifierQuery)
typedef struct {
    const char *prompt;     // as typed; may start with an @tier token
    const char *package;    // the map package folder's name
    int boxes, models;      // objects in the map
    bool brief;             // the package has a MAP.md
    int selected;           // selected objects
    int regions;            // screen regions
    int volumes;            // volumes (world-space boxes)
    bool hit;               // the crosshair hit something
} ModelQuery;

// The model a request runs on, who chose it and why (request.md's Model line, request.json "model")
typedef struct {
    ModelTier tier;
    char name[128];         // value of --model ("" never: falls back to the tier's default)
    MonoMsg reason;         // one line, plain ASCII (freed with ModelChoiceFree)
    bool override;          // an @tier token in the prompt chose the tier
    const char *source;     // "classifier", "prompt", "MONOLITH_MODEL" or "MONOLITH_CLAUDE_ARGS"
    const char *classifier; // "ai", "failed", "off" or "override" (NULL while the classifier runs)
    char classifierModel[72];   // the model that answered (classifier "ai"), else ""
    double seconds;         // the classifier's time from start to answer; < 0: none
    bool addFlag;           // false: MONOLITH_CLAUDE_ARGS already has --model, don't add one
    int promptSkip;         // bytes at the start of the prompt taken by the @token (+ blanks)
    bool droppedModelArg;   // MONOLITH_CLAUDE_ARGS had a --model without a name (ignored)
} ModelChoice;

// The view-only build (MONOLITH_EDITING 0, common.h) has no launcher: launcher.c is POSIX and
// left out of that build (nob.c); the stubs at the end stand in for the calls its modules keep
#if MONOLITH_EDITING

const char *ModelTierName(ModelTier tier);
// The overrides: an @token in the prompt, MONOLITH_MODEL, a --model in MONOLITH_CLAUDE_ARGS
// (MONOLITH_MODEL_* name the tiers). *out is overwritten (not freed first): free it with
// ModelChoiceFree. Returns
//   0: an override decided (classifier "override"); *out is final
//   1: no override: the AI classifier decides (*out holds the default tier until then)
unsigned char LauncherChooseModel(const ModelQuery *q, ModelChoice *out);
// Frees what the model choice allocated in *c (its reason)
void ModelChoiceFree(ModelChoice *c);
// The whole choice, synchronously (--classify, no window, no bundle): the overrides, else the AI
// classifier run to its end in a temporary folder (removed afterwards), MONOLITH_CLASSIFIER=off
// or its failure giving the default tier. *out as for LauncherChooseModel.
void LauncherClassify(const ModelQuery *q, ModelChoice *out);

// The launcher's note: the text of the last LauncherSubmit / LauncherClose* outcome that is
// not a failure (failures are in MS_LastError()). "" before any. Valid until the next of those calls; freed by LauncherShutdown.
const char *LauncherNote(void);

// Writes requests/<stamp>_<slug>/ (view.png, view-marked.png, region-<n>.png) and computes the
// rest of the bundle. With an override (LauncherChooseModel) it then writes request.md and
// request.json and queues the bundle for the coordinator, starting one if none runs
// (MONOLITH_LAUNCH=per-request: launches Claude Code on it in a new terminal). Without one it
// starts the AI classifier instead and returns: LauncherUpdate writes and queues the bundle once
// the classifier has answered (or failed). Never blocks on a child. Returns
//   0: queued / sent / classifying (a queued bundle counts even if the coordinator couldn't
//      start); the one-line HUD text in LauncherNote() ("request queued [n] ...", "request sent
//      (...) ...", "request classifying [n] ...")
//   1: not sent; why in MS_LastError() (it names the bundle path if one was written, so the
//      developer can run it by hand)
unsigned char LauncherSubmit(const DesignRequest *req, Image view, Image marked);

// The HUD text of a classified request that LauncherUpdate has just queued or sent (or failed to:
// *warning true), once: NULL until the next one. Valid until the next launcher call.
const char *LauncherEventNote(bool *warning);

// Removes MONOLITH_ALLOW_WINDOW from this process's environment, so the Claude Code sessions
// the launcher starts (agents, whose shells set CLAUDECODE) never inherit the override of the
// editor's window guard. Call once, after main() has read it and before any launch.
void LauncherScrubEnv(void);

// Polls the running classifiers (every call, without waiting) and the open requests for
// result.md (cheap; call every frame, it rate-limits itself)
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
// at once, a running one on the second call. A request still classifying is cancelled at once
// in both modes (its classifier killed, the bundle written as cancelled). Returns
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
// Requests of this editor run that are queued or running on the map file mapPath (absolute or
// relative): how many, and the HUD number of the newest one (*newest, 0 = none)
int LauncherOpenOnMap(const char *mapPath, int *newest);
// A closed request's terminal process is still running (being shut down)
bool LauncherClosing(void);
// When quitting: kills the classifiers still running and queues (sends) their bundles on the
// default tier ("editor quit while classifying"); then MONOLITH_CLOSE_ON_EXIT=1 closes every
// finished request's terminal, or the coordinator if none of this run's requests is still open
void LauncherCloseOnExit(void);

// Camera of the most recently finished request, for "fly back" (false if none)
bool LauncherLastDone(Vector3 *eye, float *yaw, float *pitch);

// Queues any request still classifying (as LauncherCloseOnExit), then frees the request list and
// the launcher's texts (call once at exit; requests started later are tracked anew)
void LauncherShutdown(void);


#else   // !MONOLITH_EDITING: no requests; the callers refuse before they get here (main.c)
static inline const char *ModelTierName(ModelTier tier) { (void)tier; return "none"; }
static inline void LauncherClassify(const ModelQuery *q, ModelChoice *out)
{
    (void)q;
    *out = (ModelChoice){ .tier = MODEL_TIER_DEFAULT, .reason = { .text = MSG_VIEW_ONLY }, .source = "none",
                          .classifier = "off", .seconds = -1.0 };
}
static inline void ModelChoiceFree(ModelChoice *c) { MsgFree(&c->reason); }
static inline const char *LauncherNote(void) { return MSG_VIEW_ONLY; }
static inline unsigned char LauncherSubmit(const DesignRequest *req, Image view, Image marked)
{
    (void)req; (void)view; (void)marked;
    MS_SetError(MSG_VIEW_ONLY);
    return 1;
}
static inline const char *LauncherEventNote(bool *warning) { (void)warning; return NULL; }
static inline void LauncherScrubEnv(void) { }
static inline void LauncherUpdate(float dt) { (void)dt; }
static inline int LauncherDrawHud(int x, int y) { (void)x; (void)y; return 0; }
static inline unsigned char LauncherCloseLatestDone(void) { return 1; }
static inline int LauncherCloseAllDone(void) { return 0; }
static inline unsigned char LauncherCloseNumber(int number) { (void)number; MS_SetError(MSG_VIEW_ONLY); return 2; }
static inline unsigned char LauncherCloseCoordinator(void) { return 2; }
static inline int LauncherNewestNumber(void) { return 0; }
static inline int LauncherOpenOnMap(const char *mapPath, int *newest) { (void)mapPath; *newest = 0; return 0; }
static inline bool LauncherClosing(void) { return false; }
static inline void LauncherCloseOnExit(void) { }
static inline bool LauncherLastDone(Vector3 *eye, float *yaw, float *pitch) { (void)eye; (void)yaw; (void)pitch; return false; }
static inline void LauncherShutdown(void) { }
#endif

#endif
