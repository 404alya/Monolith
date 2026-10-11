# monolith examples

Small, complete raylib + R3D programs that show how a monolith map package is used in your
own project. Each one has its own README.

| Example | Shows |
|---|---|
| [`01_minimal`](01_minimal/) | Load an exported package and orbit around it. The same file is the `viewer.c` of every export. |
| [`02_existing_project`](02_existing_project/) | A small existing game (own camera, player, objects, lights) before and after integrating a scene, drawn inside the game's own R3D frame, with `MS_Collides` for collision. |
| [`03_live_editing`](03_live_editing/) | A game that hot-reloads the package monolith is editing, side by side with the editor. |
| [`04_import_source`](04_import_source/) | A hand-written R3D scene (CC0 models, lights, fog, sky): the test input for `monolith --import` and for the capture shim (`runtime/monolith_capture.h`). |

## Build

```sh
./build.sh                  # once, in the repo root: builds R3D (submodules/r3d) and monolith
examples/build.sh           # -> examples/build/linux/<example>/
examples/build.sh --win     # -> examples/build/win/<example>/ (MinGW-w64; run ./build.sh --win first)
```

The examples compile against monolith's R3D copy with `-DMS_PATCHED_R3D`, with the game's
flags (zero warnings) and static linking. Each output folder holds the program and the data it
loads (`scene/` or `assets/`), so it runs from any working directory. `examples/build/` is not
committed.

## Screenshots without a window

Every example takes `--shot <file>`: it renders 30 frames into a texture in a hidden window
(never mapped, titled `monolith-shot`, so an i3 rule such as
`for_window [title="^monolith-(bench|shot)$"] move container to workspace number 10` routes it),
saves the image and exits. The shared code is [`common/shot.h`](common/shot.h).

```sh
examples/build/linux/01_minimal/minimal --shot /tmp/01.png --view 2
examples/build/linux/02_existing_project/after --shot /tmp/02-after.png
```

`01_minimal` renders exactly like `monolith --shot --view <n>` of the same package (same camera
math, FXAA, depth clamp): the images are pixel-identical.
