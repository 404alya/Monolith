// surfacebake -- the draw mode's shader bake (docs/painting.md 3.6): a box drawn alone, the way
// the scene draws it, through R3D's G-buffer views into the cells of a canvas (surfacebake.c)
#ifndef MONOLITH_SURFACEBAKE_H
#define MONOLITH_SURFACEBAKE_H

#include "common.h"

// What a bake gives: three images of the canvas's size and layout (each face in its cell, the
// gutter filled from the face's edge texels)
typedef struct SurfaceBake {
    Image albedo;          // RGBA8, sRGB: the shader's ALBEDO; alpha 0 where it left a hole (discard)
    Image orm;              // RGBA8, linear: occlusion, roughness, metalness as the shader wrote them; alpha 255
    Image normal;           // RGBA8 tangent-space normal map of each face; empty when the shader doesn't change the normal
    bool holes;             // some texel of a face is uncovered
    double ms;              // how long it took
} SurfaceBake;

// Bakes box `box` (its material's shader, uniforms and samplers as the scene draws it) into a
// W x H canvas laid out as cells[] (uv_faces, docs/painting.md 4.1). One R3D frame per face and
// view (albedo over two backdrops, ORM, normal) into small colour targets; R3D's environment is
// swapped for a plain one and put back byte for byte, and the output mode and depth output are
// restored, so the scene's next frame is as it would have been. Call between frames, outside
// R3D_Begin / End. False with why (out is empty then).
bool SurfaceBakeBox(const MS_Scene *scene, int box, const unsigned char cells[6], int W, int H, SurfaceBake *out,
                    char *why, int whySize);
void SurfaceBakeFree(SurfaceBake *b);

#endif
