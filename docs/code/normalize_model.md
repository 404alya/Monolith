# `tools/normalize_model.py`: normalise a model in Blender

`tools/normalize_model.py` is a Blender script that normalises a downloaded model into a clean GLB. The workflow around it is the `assets-and-licences` skill.

## Running it

Run it headless and locked down. Never open a downloaded `.blend`: this script only imports glTF / GLB / OBJ / FBX through Blender's own importers.

```
BLENDER=${BLENDER:-blender}   # set BLENDER when Blender is not in PATH (e.g. a Steam install)
"$BLENDER" -b --factory-startup --disable-autoexec --python-exit-code 1 \
    --python tools/normalize_model.py -- <in.glb|.gltf|.obj|.fbx> <out.glb> [options]
```

Run it from the repo root, never from a download directory.

## Options

| Option | Effect |
|---|---|
| `--origin bottom\|center\|keep` | Moves the origin: `bottom` is the bottom centre of the bounds (default `keep`). |
| `--height H` | Uniform scale so the model is H metres tall. |
| `--scale S` | Uniform scale factor (e.g. 0.01 for an FBX in centimetres). |
| `--decimate N` | Collapse-decimates meshes to about N triangles in total. |
| `--max-texture PX` | Downscales every image larger than PX on its longer side. |
| `--strip-unlit` | Rebuilds `KHR_materials_unlit` (emission / background) materials as lit Principled BSDF with the same base colour / texture. |
| `--metallic F` | Sets the metallic factor of every material without a metalness map (e.g. 0 for Kenney kits exported with metallic 1). |
| `--report FILE` | Also writes the change list as JSON (for `tools/record_asset.py`, [record_asset.md](record_asset.md)). |

## The output

- glTF is Y-up; Blender is Z-up internally and its glTF importer / exporter convert both ways, so the output is Y-up metres.
- Transforms are applied (the output has identity node transforms); animations and cameras / lights are dropped.
- It prints every change it made.
