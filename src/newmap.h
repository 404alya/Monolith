// --new-map: creates a new, empty map package (docs/design-v2.md section 1)
#ifndef MONOLITH_NEWMAP_H
#define MONOLITH_NEWMAP_H

#include "common.h"

// Creates <mapsDir><name>/ with <name>.map (format line, the default material, the
// built-in environment written out, a spawn, two viewpoints and a ground slab), a MAP.md
// template, empty shaders/ models/ textures/ and an empty ASSETS.md, then checks that the
// package loads. Refuses to overwrite anything. Prints what it did; false on any error.
bool NewMap(const char *mapsDir, const char *name);

#endif
