// --bake-surface: renders one material of a map onto a flat tile and writes it as a PNG (bake.c)
#ifndef MONOLITH_BAKE_H
#define MONOLITH_BAKE_H

#include "common.h"

typedef enum { BAKE_FACE_TOP = 0, BAKE_FACE_SIDE } BakeFace;

typedef struct {
    const char *mapPath;    // package folder or .map file
    const char *material;   // material name in that map
    const char *outPrefix;  // writes <outPrefix>.png
    int size;               // level-0 tile size in px (square)
    float texels;           // level-0 texels per metre: the tile covers size / texels metres
    BakeFace face;
    int levels;             // 1 = level 0 only; n > 1: levels 0..n-1 in one atlas (bake.c)
    const char *shaderFile; // (set by BakeSurface: the material's shader, package-relative; NULL = none)
} BakeArgs;

// Opens its own hidden window and R3D context, writes the PNG, closes both. Returns the
// process exit status (messages on stdout / stderr).
int BakeSurface(const BakeArgs *a);

#endif
