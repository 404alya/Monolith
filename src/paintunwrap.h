// Paint unwrap: a generated paint UV layout of a model file (docs/painting.md 4.5, 17.5)
//
// A model slot whose own UVs repeat, overlap, are a palette or are missing gets its paint domain
// from an unwrap of the whole file: xatlas (src/vendor/xatlas/, MIT, editor build only) cuts each
// slot's triangles into charts and packs them at a fixed texel size into that slot's own square
// (its 0..1 domain). The result is the MS_PaintUnwrap of
// the runtime (the contents of the file's .paintuv sidecar): per mesh, new vertices that copy a
// source vertex, a unorm16 paint UV each, and the mesh's triangles in their original order and
// count. The meshes go in as plain arrays, so the unwrap doesn't depend on how a model is loaded.
#ifndef MONOLITH_PAINTUNWRAP_H
#define MONOLITH_PAINTUNWRAP_H

#include "monolith_scene.h"
#include <stdbool.h>

// One mesh of the file, as loaded (R3D's order)
typedef struct {
    const float *positions;         // vertexCount x 3 (model space)
    const float *texcoords;         // vertexCount x 2, or NULL: used only as a hint (seams)
    const unsigned int *indices;    // indexCount (triangles), or NULL: vertices 3k, 3k+1, 3k+2
    int vertexCount, indexCount;    // (indexCount is ignored when indices is NULL)
    int slot;                       // the file's material slot the mesh is drawn with
} PaintUnwrapInput;

// The largest square a slot's unwrap may take (texels a side): the atlas's limit (docs/painting.md 5.1)
#define PAINTUNWRAP_MAX_SIDE 4096

// Unwraps `meshCount` meshes at a fixed texel size (docs/painting.md 17.5): every slot is packed
// into a square of its own, at texelsPerUnit paint texels per model unit, with charts on whole
// texels at least 2 texels apart; sides[slot] (sideCount entries, -1 for a slot no mesh uses, 0 for
// one with only degenerate triangles) gets the square's side, so a paint rectangle of that side has
// one texel per texel of the pack. Paint UVs are pack texels / side (0..1). out->meshes[i] belongs
// to meshes[i]: its slot, a source vertex per new vertex (< the input's vertexCount) and indices
// whose triangle t is input triangle t (source[indices[3t + k]] == the input's index 3t + k).
// No two triangles' paint UVs overlap within a slot, except triangles of zero area in 3D and
// coincident duplicates (they draw no pixel, or the other's). Triangles of zero area that xatlas
// leaves out get paint UV 0, 0. The result is deterministic (single-threaded xatlas, fixed
// options). out->fileHash is 0 (the caller sets it). Free with MS_PaintUnwrapFree. False with a
// message in MS_LastError() (out is then empty) on bad input, a slot that needs more than
// PAINTUNWRAP_MAX_SIDE texels a side, or when out of memory.
bool PaintUnwrapMeshes(const PaintUnwrapInput *meshes, int meshCount, float texelsPerUnit, MS_PaintUnwrap *out, int *sides,
                       int sideCount);

// The unwrap of model file `file` of a loaded scene (docs/painting.md 17.5): its meshes from
// MS_ModelMeshData, the slots from the model, fileHash = FNV-1a 64 of the file's bytes, at
// texelsPerUnit paint texels per model unit (the draw mode: the grid's texels per metre, or its
// density, times the painted instance's largest scale), each slot's square side in sides[slot] as
// PaintUnwrapMeshes gives it: the rectangle a first stroke allocates. False with a message when the
// file isn't loaded, is skinned, or the unwrap fails.
bool PaintUnwrapModelFileAt(const MS_Scene *scene, int file, float texelsPerUnit, MS_PaintUnwrap *out, int *sides,
                            int sideCount);

#endif
