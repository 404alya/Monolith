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

creates `maps/forest/` (or, given a path such as `/tmp/drafts/forest`, the package in that
folder, which keeps a trial import out of the repo) and opens a Claude Code session (heavy
model tier) on a bundle
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

## Checking an import

No imported package is checked in here: an import is a request, so its result is a new map
package in `maps/` (or wherever `--import` was pointed). Compare it with the original:

```sh
examples/build/linux/04_import_source/import_source --shot /tmp/original.png
build/linux/monolith --map maps/forest --shot /tmp/imported.png --shot-size 1280 720 --view 1
```

Expected differences, none of them errors: the field of view (this program uses 60 degrees,
monolith viewpoints 70), the rotated stones (boxes are axis-aligned: approximated or left out),
and the Kenney models' colours if their materials were overridden with `inherit all` (an
override's `albedo` defaults to white and replaces the file's colour factor: copy the colours
`--inspect-model` lists into the override, or override only roughness / metalness per slot).

The program builds only through `examples/build.sh` (it links monolith's R3D copy and the static
libraries `../build.sh` builds, and includes `../common/shot.h` from outside this folder): to
build a scratch copy, copy `examples/04_import_source/` and `examples/common/` together.

## Assets

See `assets/ASSETS.md`: Kenney Nature Kit 2.1, CC0, `assets/kenney-License.txt`.
