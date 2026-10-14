// Export: one map as a self-contained folder (package, scene runtime, R3D, build script, README)
#ifndef MONOLITH_EXPORT_H
#define MONOLITH_EXPORT_H

#include "common.h"

// Writes <outDir>/ (see the header comment of export.c): the package with every file the
// scene references, the scene runtime without its editor parts, viewer.c, build.sh, the
// bundled R3D, licences and README.md. Refuses a non-empty outDir
// and writes nothing when a source is missing. Works with a noGpu scene (no window needed).
// Needs the monolith repo (the runtime and R3D sources). msg gets a one-line summary (or the
// error); details are printed to stdout/stderr.
bool ExportScene(const MS_Scene *scene, const char *outDir, char *msg, int msgSize);

// <repo root or exe dir>/export/<map name>-<YYYY-MM-DD_HHMMSS>/ (the in-game export key)
void ExportDefaultDir(const MS_Scene *scene, char *out, int outSize);

#endif
