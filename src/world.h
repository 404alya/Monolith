// World: axis-aligned concrete boxes, used for both rendering and collision.
// The boxes come from a map file, see map.h.
#ifndef MONOLITH_WORLD_H
#define MONOLITH_WORLD_H

#include "common.h"

#define MAX_BOXES   8192
#define MAX_GROUPS  256
#define GROUP_NAME_MAX 64

// Where a box came from, so selection and request bundles can point at the map file
typedef struct {
    int line;               // 1-based source line in the map file, 0 = not from a file
    int prim;               // index of the primitive (map line) that generated it, -1 = none
    int group;              // index into World.groups, -1 = no group
} BoxRef;

typedef struct {
    BoundingBox items[MAX_BOXES];
    BoxRef refs[MAX_BOXES];
    int count;

    char groups[MAX_GROUPS][GROUP_NAME_MAX];
    int groupCount;

    // Incremented on every successful (re)build. Static caches (shadow map, fog
    // volume, frame reuse, ...) key on it and rebuild when it changes.
    unsigned int version;

    // The map file the boxes were loaded from (see map.c), for hot reload and for
    // tools that quote map lines
    char path[1024];
    long mtime;                 // GetFileModTime(path) when it was loaded
    unsigned long long hash;    // FNV-1a 64 of the file bytes
} World;

// The current map. Filled only by LoadMap() (map.c), which swaps a fully parsed map in
// at once, so rendering and collision never see a half-loaded world.
extern World world;

#endif
