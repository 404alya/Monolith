// Map files: a plain-text list of box primitives (format reference at the top of map.c
// and maps/monolith.map), loaded into `world`, with hot reload
#ifndef MONOLITH_MAP_H
#define MONOLITH_MAP_H

#include "world.h"

#define MAP_LINE_MAX 1024       // longest accepted map line (characters, without newline)

// Parses the map file at `path` into a temporary world and swaps it into `world` only if
// the whole file is valid (world.version is incremented, path/mtime/hash updated, the
// file text kept for MapGetLine). On failure `world` is untouched, false is returned and
// `err` holds "file:line: message" (or "file: message"). Never crashes on bad input.
bool LoadMap(const char *path, char *err, int errSize);

typedef enum {
    MAP_UNCHANGED = 0,      // nothing to do
    MAP_RELOADED,           // `world` holds the new map: rebuild instances, drop caches
    MAP_RELOAD_FAILED,      // the file changed but is invalid; old map kept, see MapError()
} MapPollResult;

// Hot reload: call once per frame. About twice a second it checks world.path's mtime
// and, when it changed (or changed recently, see map.c), re-reads the file and reloads
// it if its contents differ from the last attempt. Reports each change only once.
MapPollResult MapPoll(float dt);

// The error of the last failed load ("" after a successful one)
const char *MapError(void);

// Copies line `line` (1-based) of the currently loaded map's text into `out` (without
// the newline, truncated to outSize - 1). False if there is no such line.
bool MapGetLine(int line, char *out, int outSize);

#endif
