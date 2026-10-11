# 04_import_source: an existing R3D scene to import into monolith

`main.c` is a forest clearing at dusk written directly against R3D, the way many projects set
up a scene: a ground slab and a small cabin from `R3D_GenMeshCube`, a campfire with a ring of
rotated stones, Kenney Nature Kit trees and rocks (`assets/`, CC0) placed with position, yaw and
scale and switched from their unlit glTF materials to lit ones, a low shadowed sun, an orange
point light and a blue spot light, a dusk procedural sky with its ambient map, exp2 fog, bloom
and ACES tonemapping. Nothing in it knows about monolith.

```sh
examples/build/linux/04_import_source/import_source --shot /tmp/forest.png
```

## Route 1: an import request (Claude Code reads the source)

```sh
build/linux/monolith --import examples/04_import_source forest
```

creates `maps/forest/` and opens a Claude Code session (heavy model tier) on a bundle
`requests/<stamp>_import-forest/` that follows `AI-MAP-TOOL.md` "Import requests": it reads
`main.c` as data, copies the models into the package, records them in `ASSETS.md`, translates
the cubes, models, materials, lights, sky, fog and post-processing into the map, writes
`MAP.md`, and compares a `--shot` of this program with `monolith --shot` of the new map.

## Route 2: the capture shim (records what R3D draws)

`import_source_capture` is the same `main.c`, compiled with `runtime/monolith_capture.h`
put in front of it (`-include`, see `examples/build.sh`); the source is unchanged:

```sh
MONOLITH_CAPTURE=/tmp/cap/forest examples/build/linux/04_import_source/import_source_capture --shot /tmp/x.png
```

writes `/tmp/cap/forest/` (`forest.map`, `models/`, `MAP.md`, `ASSETS.md`, `CAPTURE.md`) from
frame 3: 11 boxes (the 4 rotated stones as their bounding boxes, listed as approximations), 8
model instances with their yaw and scale, 4 materials, the sun + 2 lights, the sky and every
environment setting. `CAPTURE.md` lists what it could not translate (here: the material
changes the program makes to the loaded models, the camera's 60 degree field of view).

## The package the import produced

Model lines need the models support of the scene runtime; the imported package is added
here once that is merged.

## Assets

See `assets/ASSETS.md`: Kenney Nature Kit 2.1, CC0, `assets/kenney-License.txt`.
