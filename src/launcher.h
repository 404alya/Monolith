// Launcher: request bundles, Claude Code in a new terminal, polling for result.md
#ifndef MONOLITH_LAUNCHER_H
#define MONOLITH_LAUNCHER_H

#include "common.h"
#include "designer.h"

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
