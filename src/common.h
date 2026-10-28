// Shared includes, the current scene and process-wide data paths
#ifndef MONOLITH_COMMON_H
#define MONOLITH_COMMON_H

#include <r3d/r3d.h>
#include <raymath.h>
#include <rlgl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "monolith_scene.h"
// (raylib's keyboard / mouse calls go through the input shim: --input-script)
#include "input.h"

// The loaded map package (runtime/monolith_scene.h). Set by main() before the window
// opens; its contents change on hot reload (scene->version tells which load).
extern MS_Scene *scene;

// Data directory, with a trailing slash (see InitDataPaths in main.c)
extern char benchmarkDir[1024];

#endif
