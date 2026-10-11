// Export: a map package + the scene runtime + a README, ready to drop into a raylib + R3D project
#ifndef MONOLITH_EXPORT_H
#define MONOLITH_EXPORT_H

#include "common.h"

// Writes <outDir>/ (see the header comment of export.c): the package with every file the
// scene references, the scene runtime (monolith_scene.h/.c), viewer.c and README.md.
// Refuses a non-empty outDir. Works with a noGpu scene (no window needed). msg gets a
// one-line summary (or the error); details are printed to stdout/stderr.
bool ExportScene(const MS_Scene *scene, const char *outDir, char *msg, int msgSize);

// <repo root or exe dir>/export/<map name>-<YYYY-MM-DD_HHMMSS>/ (the in-game export key)
void ExportDefaultDir(const MS_Scene *scene, char *out, int outSize);

#endif
