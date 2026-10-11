// Launcher: request bundles, Claude Code in a new terminal, polling for result.md
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
} ModelChoice;

const char *ModelTierName(ModelTier tier);
// Pure classifier (no environment): tier + reason; strips nothing
ModelTier ClassifyRequest(const ModelQuery *q, char *reason, int reasonSize);
// Classifier + @token + MONOLITH_MODEL* / MONOLITH_CLAUDE_ARGS: what the launcher runs
void LauncherChooseModel(const ModelQuery *q, ModelChoice *out);

// Writes requests/<stamp>_<slug>/ (view.png, view-marked.png, request.md, request.json)
// and launches Claude Code on it. Never blocks on the child. Returns false on failure;
// either way msg gets a one-line HUD text (on failure it names the bundle path if one
// was written, so the developer can run it by hand).
bool LauncherSubmit(const DesignRequest *req, Image view, Image marked, char *msg, int msgSize);

// Polls the open requests for result.md (cheap; call every frame, it rate-limits itself)
void LauncherUpdate(float dt);

// Draws the open/finished request list at (x, y); returns the height used in pixels
int LauncherDrawHud(int x, int y);

// Camera of the most recently finished request, for "fly back" (false if none)
bool LauncherLastDone(Vector3 *eye, float *yaw, float *pitch);

#endif
