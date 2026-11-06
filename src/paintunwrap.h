// Paint unwrap: a generated paint UV layout of a model file (docs/painting.md 4.5)
//
// A model slot whose own UVs repeat, overlap or are missing gets its paint domain from an unwrap
// of the whole file: xatlas (src/vendor/xatlas/, MIT, editor build only) cuts each slot's triangles
// into charts and packs them into that slot's own 0..1 domain. The result is the MS_PaintUnwrap of
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

// Unwraps `meshCount` meshes: every slot is packed into its own 0..1 domain (all meshes of the
// slot together, charts not overlapping, at least 2 texels apart at a texelsHint x texelsHint
// paint rectangle; texelsHint is clamped to 16..4096). out->meshes[i] belongs to meshes[i]:
// its slot, a source vertex per new vertex (< the input's vertexCount) and indices whose
// triangle t is input triangle t (source[indices[3t + k]] == the input's index 3t + k).
// No two triangles' paint UVs overlap within a slot, except triangles of zero area in 3D and
// coincident duplicates (they draw no pixel, or the other's). Triangles of zero area that xatlas
// leaves out get paint UV 0, 0. The
// result is deterministic (single-threaded xatlas, fixed options). out->fileHash is 0 (the
// caller sets it). Free with MS_PaintUnwrapFree. False with a message in MS_LastError() (out is
// then empty) on bad input or when out of memory.
bool PaintUnwrapMeshes(const PaintUnwrapInput *meshes, int meshCount, int texelsHint, MS_PaintUnwrap *out);

// The unwrap of model file `file` of a loaded scene: its meshes from MS_ModelMeshData, the slots
// from the model, fileHash = FNV-1a 64 of the file's bytes, at a texelsHint x texelsHint paint
// rectangle (the draw mode passes the size of the slot's rectangle; PAINTUNWRAP_TEXELS: the tests'
// size). False with a message when the file isn't loaded, is skinned, or the unwrap fails.
#define PAINTUNWRAP_TEXELS 1024
bool PaintUnwrapModelFile(const MS_Scene *scene, int file, int texelsHint, MS_PaintUnwrap *out);

#endif
