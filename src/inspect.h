// --inspect-model: prints what a model file contains (no window; inspect.c)
#ifndef MONOLITH_INSPECT_H
#define MONOLITH_INSPECT_H

#include "common.h"

// Prints bounds, size, mesh/triangle/material counts, materials and their textures, up axis
// and unit hints, warnings and a placement line to stdout. Returns the process exit status.
int InspectModel(const char *path);

#endif
