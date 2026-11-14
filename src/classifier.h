// The AI classifier: one `claude -p` run per request that picks its tier (fast / default / heavy),
// started and polled through nob.h without ever waiting in the frame loop (classifier.c;
// docs/request-queue.md, "Models"). The launcher (launcher.c) decides whether to run it at all
// (overrides, MONOLITH_CLASSIFIER=off, which program) and turns the result into its ModelChoice.
#ifndef MONOLITH_CLASSIFIER_H
#define MONOLITH_CLASSIFIER_H

#include "common.h"     // MONOLITH_EDITING, msg.h

// What the classifier is told about one request: the lines of classifier-input.txt (its stdin).
// The prompt is data and only ever goes to stdin, never on a command line.
typedef struct {
    const char *prompt;         // the request text, without an @token
    const char *package;        // the map package folder's name
    int boxes, models;          // objects in the map
    bool brief;                 // the package has a MAP.md
    int selected, regions, volumes;
    bool hit;                   // the crosshair is on an object
} ClassifierQuery;

// One classifier run. Zero-initialised it is idle. While it runs, pid is the Nob_Proc (a pid on
// POSIX); once ClassifierPoll returned 1 (or ClassifierKill ran) the result fields are final.
typedef struct {
    int pid;                    // the running process; 0 = none (finished, reaped or never started)
    double start;               // monotonic seconds when it started
    double termAt, killAt;      // after a timeout: when SIGTERM went out, when SIGKILL follows (0 = not yet)
    double timeout;             // seconds (MONOLITH_CLASSIFIER_TIMEOUT, default 30)
    char *dir;                  // folder of classifier-input.txt / classifier.json / classifier.log (heap)
    bool timedOut;              // SIGTERM was sent because of the timeout
    // the result
    bool done;
    int tier;                   // 0 fast, 1 default, 2 heavy (the launcher's ModelTier order)
    bool ok;                    // true: the AI answered within the schema; false: failed (default tier)
    char *model;                // the model that answered (from modelUsage), heap; NULL if unknown
    MonoMsg reason;             // the AI's one-line reason (plain ASCII, <= 200 bytes), or why it failed
    double seconds;             // from the start to the answer (or the failure)
} Classifier;

#if MONOLITH_EDITING

// Writes <dir>classifier-input.txt from q (dir ends in '/') and starts `program` on it with the
// fixed arguments (--model $MONOLITH_CLASSIFIER_MODEL / fable, --fallback-model, --safe-mode, no
// tools, JSON schema, --system-prompt-file promptFile), stdout to <dir>classifier.json, stderr to
// <dir>classifier.log. Never blocks on the child. Returns
//   0: started (poll it with ClassifierPoll)
//   1: not started: c is done already, failed (c->reason says why: the input file, the prompt
//      file, the start)
unsigned char ClassifierStart(Classifier *c, const ClassifierQuery *q, const char *dir, const char *program,
                              const char *promptFile);

// Checks the run once, without waiting (waitpid WNOHANG); sends SIGTERM after the timeout and
// SIGKILL 2 s later. Returns 0: still running; 1: done (the result fields are set: an answer
// within the schema, or a failure with its reason).
unsigned char ClassifierPoll(Classifier *c);

// Stops a running classifier (SIGTERM, up to 1 s for it to go, then SIGKILL; reaped) and marks
// the run failed with `why` as its reason. Blocks for at most about a second: only for cancel and
// quit. Nothing happens to a run that is done already.
void ClassifierKill(Classifier *c, const char *why);

// ClassifierPoll until done, sleeping 20 ms between checks (the command line's --classify only)
void ClassifierWait(Classifier *c);

// Frees the heap fields (kills a run that still runs, as ClassifierKill); c is idle again
void ClassifierFree(Classifier *c);

#endif

#endif
