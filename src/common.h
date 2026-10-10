// Shared includes and process-wide data paths
#ifndef MONOLITH_COMMON_H
#define MONOLITH_COMMON_H

#include <r3d/r3d.h>
#include <raymath.h>
#include <rlgl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Data directories, with a trailing slash (see InitDataPaths in main.c)
extern char resourcesDir[1024], benchmarkDir[1024];

#endif
