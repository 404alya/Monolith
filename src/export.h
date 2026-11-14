// Export: one map as a self-contained folder (package, scene runtime, R3D, build script, README)
#ifndef MONOLITH_EXPORT_H
#define MONOLITH_EXPORT_H

#include "common.h"

// Writes <outDir>/ (see the header comment of export.c): the package with every file the
// scene references, the scene runtime without its editor parts, viewer.c, build.sh, the
// bundled R3D, licences and README.md. Refuses a non-empty outDir
// and writes nothing when a source is missing. Works with a noGpu scene (no window needed).
// Needs the monolith repo (the runtime and R3D sources). Details are printed to stdout/stderr;
// the one-line result is printed too: console = true as "monolith: <text>" on stdout (exported)
// or stderr (not), as --export does; false as a TraceLog (INFO / WARNING) for the editor key.
//   0: exported; the text in ExportNote() ("exported <dir> (...): build it with ./build.sh ...")
//   1: nothing written; the whole "export: ..." sentence in MS_LastError()
unsigned char ExportScene(const MS_Scene *scene, const char *outDir, bool console);
// The success sentence of the last export ("" before any); freed by ExportShutdown
const char *ExportNote(void);
// Frees the export's note (call once at exit)
void ExportShutdown(void);

// <repo root or exe dir>/export/<map name>-<YYYY-MM-DD_HHMMSS> (the editor's export key): a heap
// path (free it), NULL when out of memory
char *ExportDefaultDir(const MS_Scene *scene);

#endif
