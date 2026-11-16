# `src/drawmode.c`: the draw mode

B in spectator mode: painting on boxes and models into the paint layer. The design is in [painting.md](../painting.md).

## Paint atlases

Paint lives in per-material atlases (`textures/paint/<material>.png`; a model slot drawn with its own material: `textures/paint/<file>_s<N>.png`), RGBA8 with straight alpha, transparent where unpainted.

Every painted surface has a rectangle there: a box face (or a group of a box's faces) or a model slot of one instance, written in the map as a `paint` statement. The runtime composites the atlas over what the material computed (its paint stage), so the object looks exactly as before wherever it isn't painted.

This file owns each atlas's pixels in memory while it paints: the runtime gets `MS_PaintAtlasProvide` (the image the next live edit loads) and `MS_TextureUpdate` (a frame's stamps).

## Allocation and undo

The first stroke on a face or slot without a rectangle is one undoable edit (`EDIT_PAINT_ADD`, `mapedit.c`): the object's line gets an id, the material its `paint` key, and the file's paint block a statement with the rectangle the packer chose (bottom-left on 4-texel cells with a 2-texel border, around the rectangles the text holds; the atlas grows by doubling up to 4096 x 4096). A stroke that goes on onto another face of the same box allocates there too, amended onto the same step (`MapEditAmendPaint`). Strokes on surfaces that have a rectangle are steps of paint alone.

Every step keeps the 64 x 64 tiles it touched as they were before it, and the atlas's size before and after; undo and redo swap them with the atlas (`mapedit.c` calls the hooks of this file).

## Unwrapping models

A model slot whose own UVs can't carry paint (tiled, overlapping, missing) gets an unwrap of its whole file on its first stroke (`paintunwrap.c`, xatlas), provided to the runtime and written as the model's `.paintuv` sidecar on save. Interactively the note "unwrapping x.glb ..." shows for a frame first (a scanned rock takes ~3 s).

## A stroke

A stroke runs from LMB down to up.

- Every frame the segment from the last cursor position to the current one is cut in screen space until consecutive hits lie at most `spacing` texels apart on the same region (at most 10 levels deep, on a fixed stack).
- A stamp at each sample, clipped to the region (the entry's rectangle, or a model's UV island in it), is composited source-over in sRGB bytes from the pixels the stroke started from, with the highest coverage any stamp gave a texel (a mask per touched tile), so a stroke never darkens where it crosses itself.
- The eraser lowers alpha. Fill fills the region at LMB down.
- The stroke keeps its samples in atlas texels, so a rebase can replay it onto a PNG that changed on disk.

## Saving

CTRL+S writes every live atlas with unsaved pixels and the unwraps the text uses, then the map (`MapEditSave`), then the licence records (`ASSETS.md` rows and `.asset.md` sidecars of the files it wrote first), then `MS_EditMarkSaved`, so the hot reload doesn't reload the editor's own writes.
