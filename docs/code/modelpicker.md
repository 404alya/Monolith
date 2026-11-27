# `src/modelpicker.c`: the model picker

I in edit mode: choose a shape or one of the package's models and place it.

## The list

Its first `MS_SHAPE_KIND_COUNT` rows are the **Shapes** section ([editor-tools.md](../editor-tools.md) §2.8): one per kind (cube, sphere, cylinder, cone, capsule, plane), each with a thumbnail, "shape: <kind>", the size it gets and its material, and "built in". Then the package's model files that have a licence row in `ASSETS.md`: the edit mode's list (`EditModeScanModels`, scanned again on every open). Each model row has a thumbnail, the file, the triangles, the bounds' size in metres and the licence from the file's `ASSETS.md` row. The list widget's items are `0..5` the kinds (`PickerShape shapes[]`) and `6 + i` model `i` (`ItemCount`).

| Key | Action |
|---|---|
| typing | filters (file and licence; a shape row: its kind or the word "shape"; case-insensitive) |
| UP / DOWN / PGUP / PGDN / HOME / END | select (the shared list widget, `ui.h`) |
| ENTER on a shape | places the kind (`EditModeAddShape`): 1 × 1 × 1 m (a plane 2 × 0 × 2), the `"default"` material, at the point `DesignerPickWorldPoint` gives along the cursor's last pick: the surface hit (snapped as a volume corner, CTRL exact), else (ALT held, or nothing hit) the work plane (`--work-plane`, PgUp / PgDn in volume mode). On an upward face or the plane it stands on the point, under a downward face it hangs from it, against a wall it is pushed out by half its footprint and centred on the point's height. One `EDIT_SHAPE_ADD` after the hit box's / model's / shape's line (its group; `use "default"` … `use "<active>"` when another `use` is active there), at the end of the main file on the plane; selected, the picker closes. The log prints `model picker: added a <kind> after line <n> at <cx> <y0> <cz>` |
| ENTER | places the selected model at the point under the cursor (`EditModeAddModel`: snapped on the hit face's axes, CTRL exact; one live edit, undoable, saved with CTRL+S) and closes the picker, so G then moves it (the selection is the new model) |
| ESC | closes |
| I | closes too while the filter is empty (otherwise it types an i) |

Models of other packages are not listed: bringing one in is an asset import with a licence row (a request).

## Thumbnails

See [live-editing.md](../live-editing.md) 8.3. Thumbnails are `PICKER_THUMB_SIZE` square colour-only render targets, rendered lazily while the picker is open, a few per frame (`ModelPickerRenderThumbs`, before the frame's scene draw); rows without one show a placeholder.

A shape's thumbnail (`RenderShapeThumb`) is R3D's generated mesh of its kind (`R3D_GenMeshCube` / `Sphere` / `Cylinder` / `CylinderEx` for the cone / `Capsule` / `Plane`, 32 segments around) with R3D's base material made light grey, generated, rendered and unloaded at once; the runtime's own shape meshes are not used (they are built for instancing and the paint stage). Both kinds go through `StudioRender` (the bounds framed from the front right, 22° above).

Each is one R3D frame of the model file alone with its own materials, far beyond the map's bounds (so the AO volume, the fog cache and every map light are out of reach; none of the map's draws are submitted), in a neutral studio environment swapped in for the render. R3D's environment (global) is copied byte for byte before and restored byte for byte after, with the output mode and the depth output flag. One directional light without a shadow map, no fog, SSAO, SSIL, bloom: no shadow map, static cache or history buffer is drawn or read, so the next frame's image is the same as without the render (the `--model-picker` shots prove it).

Files the scene already has loaded are drawn from there; others are loaded for the render and unloaded right after (their references checked first, as the map loader does).

All thumbnails go when `scene->loadVersion` changes, on a renderer restart (`ModelPickerDropThumbs`) and at exit.

R3D's per-frame counter (`FRAME_INDEX` in surface shaders) advances once per thumbnail; no map shader reads it.
