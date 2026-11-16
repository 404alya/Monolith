# `src/modelpicker.c`: the model picker

I in edit mode: choose one of the package's models and place it.

## The list

It lists the package's model files that have a licence row in `ASSETS.md`: the edit mode's list (`EditModeScanModels`, scanned again on every open). Each row has a thumbnail, the file, the triangles, the bounds' size in metres and the licence from the file's `ASSETS.md` row.

| Key | Action |
|---|---|
| typing | filters (file and licence, case-insensitive) |
| UP / DOWN / PGUP / PGDN / HOME / END | select (the shared list widget, `ui.h`) |
| ENTER | places the selected model at the point under the cursor (`EditModeAddModel`: snapped on the hit face's axes, CTRL exact; one live edit, undoable, saved with CTRL+S) and closes the picker, so G then moves it (the selection is the new model) |
| ESC | closes |
| I | closes too while the filter is empty (otherwise it types an i) |

Models of other packages are not listed: bringing one in is an asset import with a licence row (a request).

## Thumbnails

See [live-editing.md](../live-editing.md) 8.3. Thumbnails are `PICKER_THUMB_SIZE` square colour-only render targets, rendered lazily while the picker is open, a few per frame (`ModelPickerRenderThumbs`, before the frame's scene draw); rows without one show a placeholder.

Each is one R3D frame of the model file alone with its own materials, far beyond the map's bounds (so the AO volume, the fog cache and every map light are out of reach; none of the map's draws are submitted), in a neutral studio environment swapped in for the render. R3D's environment (global) is copied byte for byte before and restored byte for byte after, with the output mode and the depth output flag. One directional light without a shadow map, no fog, SSAO, SSIL, bloom: no shadow map, static cache or history buffer is drawn or read, so the next frame's image is the same as without the render (the `--model-picker` shots prove it).

Files the scene already has loaded are drawn from there; others are loaded for the render and unloaded right after (their references checked first, as the map loader does).

All thumbnails go when `scene->loadVersion` changes, on a renderer restart (`ModelPickerDropThumbs`) and at exit.

R3D's per-frame counter (`FRAME_INDEX` in surface shaders) advances once per thumbnail; no map shader reads it.
