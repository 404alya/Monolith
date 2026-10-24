// --import: bring an existing raylib + R3D project's scene into a new map package
#ifndef MONOLITH_IMPORT_H
#define MONOLITH_IMPORT_H

#include "common.h"

// Creates the package (the --new-map path: mapTarget is a name for maps/<name>/ or a path
// to the package folder), writes an import bundle requests/<stamp>_import-<name>/
// (request.md with an "Import" section, request.json with "kind": "import") and opens a
// Claude Code import session on it through the launcher. The session follows
// the import-request skill (.claude/skills/import-request/SKILL.md). Prints what it did; false on error.
bool ImportProject(const char *mapsDir, const char *projectDir, const char *mapTarget);

#endif
