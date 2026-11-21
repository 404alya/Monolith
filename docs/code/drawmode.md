# `src/drawmode.c`: the draw mode

B in spectator mode: painting on boxes and models into the paint layer. The design is in [painting.md](../painting.md).

## Paint atlases

Paint lives in per-material atlases (`textures/paint/<material>.png`; a model slot drawn with its own material: `textures/paint/<file>_s<N>.png`), RGBA8 with straight alpha, transparent where unpainted.

Every painted surface has a rectangle there: a box face (or a group of a box's faces) or a model slot of one instance, written in the map as a `paint` statement. The runtime composites the atlas over what the material computed (its paint stage), so the object looks exactly as before wherever it isn't painted.

This file owns each atlas's pixels in memory while it paints: the runtime gets `MS_PaintAtlasProvide` (the image the next live edit loads) and `MS_TextureUpdate` (a frame's stamps).

## Allocation and undo

The first stroke on a face or slot without a rectangle is one undoable edit (`EDIT_PAINT_ADD`, `mapedit.c`): the object's line gets an id, the material its `paint` key, and the file's paint block a statement with the rectangle the packer chose (bottom-left on 4-texel cells with a 2-texel border, around the rectangles the text holds; the atlas grows by doubling up to 4096 x 4096). A stroke that goes on onto another face of the same box allocates there too, amended onto the same step (`MapEditAmendPaint`). Strokes on surfaces that have a rectangle are steps of paint alone.

Every step keeps the 64 x 64 tiles it touched as they were before it, and the atlas's size before and after; undo and redo swap them with the atlas (`mapedit.c` calls the hooks of this file).

## The paint grid

On a material with a paint grid (`MS_PaintGridOf`: its `paint_grid`, else the map's `env paint.grid`; [painting.md](../painting.md) §17) paint follows the map's own pixel grid:

- A new box face rectangle is the face's cells (`MS_BoxFaceCells`; a face group: per axis its largest face's), refused above 2048 cells. A model slot gets `MS_PaintSlotTexels` (its texture's texels) when its own UVs carry them, else the square of its unwrap made at the grid's texel size.
- A stroke's texel on a grid face is the hit's `paintTexel` (texels from the face's first cell), and its stamps clip to that face's cells; the brush cursor maps its footprint back through the same mapping (`DrawGeom.u0 .. u1`, `v0 .. v1`: where the face's 0 and 1 lie in the rectangle's texels), so its outline lies on the surface's texel lines.
- A new atlas on a grid gets `paint_filter nearest` from its first stroke (`NewAtlasNearest`: one texel is one cell of the surface's pixel art, so it must not be magnified bilinearly), unless *Pixel look* was turned off for grids this session (`DrawMode.gridFiltered`, not kept in `draw.ini`); without a grid the kept `pixel_look` decides as before. The panel shows *Pixel look* beside the brush on a grid target, and the target line names the face's cells when they differ from its rectangle ("concrete's atlas 1024²: 54×61 at 0 0 (24/m), face -x covers 37×61 cells of it").
- *Fit to grid* (the panel's resize on a grid) gives an entry its cells again: texel for texel when the old rectangle was within a quarter of them (made on the grid; the box moved or was resized since), else resampled nearest. It also takes entries a grid made inactive (rectangle smaller than the face's cells).

Without a grid everything is as before: the panel's density sizes rectangles and a face's 0..1 spans the whole rectangle.

## Unwrapping models

A model slot whose own UVs can't carry paint (tiled, overlapping, a palette, missing) gets an unwrap of its whole file on its first stroke (`PaintUnwrapModelFileAt` in `paintunwrap.c`, xatlas) at a fixed texel size: the grid's texels per metre (else the density) times the painted instance's largest scale, each slot packed into a square of its own whose side is the rectangle the first stroke allocates. It is provided to the runtime and written as the model's `.paintuv` sidecar on save. Interactively the note "unwrapping x.glb ..." shows for a frame first (a scanned rock takes ~2 s).

## A stroke

A stroke runs from LMB down to up.

- Every frame the segment from the last cursor position to the current one is cut in screen space until consecutive hits lie at most `spacing` texels apart on the same region (at most 10 levels deep, on a fixed stack).
- A stamp at each sample, clipped to the region (the entry's rectangle, or a model's UV island in it), is composited source-over in sRGB bytes from the pixels the stroke started from, with the highest coverage any stamp gave a texel (a mask per touched tile), so a stroke never darkens where it crosses itself.
- The eraser lowers alpha. Fill fills the region at LMB down.
- The stroke keeps its samples in atlas texels, so a rebase can replay it onto a PNG that changed on disk.

## Tools, selection, kept state (Aseprite round)

The tool on (`DrawMode.tool`, `drawkeys.h`) decides what LMB / RMB do ([painting.md](../painting.md) §18). The old pens are what a step's stamps do (`PenKind`; `DrawModeStampPen`): the pencil stamps its brush shape, the eraser the eraser, the bucket in whole-region mode the fill, all as before. Every other tool records its input in the step (tool, colours, options, seed, points in atlas texels, the selection it was clipped to) and `StepRender` draws the whole step from it: the shape preview redraws it each frame from the stroke's start tiles, and a rebase replays it. The rasterisers are pure (`drawtools.c`, unit-tested).

The selection (`DrawSelection`) is a mask over one region's rectangle, editor state only (not in the history, never in the map), dropped when its region disappears and on leaving the mode; every painting tool on its region is clipped to it.

The keys come from one table (`drawkeys.c`); the camera in draw mode is `Spectator.mouseNav` (`spectator.c`). The kept state goes through `DrawModeGetState` / `DrawModeSetState` / `DrawModeStateDiffers` to `drawsettings.c`'s `<package>/draw.ini`, which `main.c` loads with the map and writes a second after the last change. The screen eyedropper sets `screenWant`; `main.c` reads that one pixel of the scene image (before the HUD) on the next frame and calls `DrawModeScreenPicked`.

## Saving

CTRL+S writes every live atlas with unsaved pixels and the unwraps the text uses, then the map (`MapEditSave`), then the licence records (`ASSETS.md` rows and `.asset.md` sidecars of the files it wrote first), then `MS_EditMarkSaved`, so the hot reload doesn't reload the editor's own writes.
