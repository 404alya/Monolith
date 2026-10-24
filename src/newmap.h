// --new-map: creates a new, empty map package (docs/map-format.md section 1)
#ifndef MONOLITH_NEWMAP_H
#define MONOLITH_NEWMAP_H

#include "common.h"

// Creates a map package with <name>.map (format line, the default material, the built-in
// environment written out, a spawn, two viewpoints and a ground slab), a MAP.md template,
// empty shaders/ models/ textures/ and an empty ASSETS.md, then checks that the package
// loads. `target` is a bare name (the package goes to <mapsDir><name>/) or a path with a
// '/' (the package is that folder; its parent must exist; the name is its last component).
// Refuses to overwrite anything. Prints what it did; false on any error.
bool NewMap(const char *mapsDir, const char *target);

// The package folder for `target` as NewMap reads it (heap; NULL when out of memory), and
// the map name in *name (points into the result or into target)
char *NewMapDir(const char *mapsDir, const char *target, const char **name);

// NewMap with the folder and name already resolved (NewMapDir)
bool NewMapIn(const char *dir, const char *name, const char *mapsDir);

#endif
