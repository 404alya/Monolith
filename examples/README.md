# monolith examples

Small, complete raylib + R3D programs that show how a monolith map package is used in your
own project. Each one has its own README.

| Example | Shows |
|---|---|
| [`01_minimal`](01_minimal/) | Load an exported package and orbit around it (with `--shot` for comparisons; an export's own `viewer.c` is `runtime/export/viewer.c`). |
| [`02_existing_project`](02_existing_project/) | A small existing game (own camera, player, objects, lights) before and after integrating a scene, drawn inside the game's own R3D frame, with `MS_Collides` for collision. |
| [`03_live_editing`](03_live_editing/) | A game that hot-reloads the package monolith is editing, side by side with the editor. |
| [`05_entities`](05_entities/) | The scene's entities and components (stable ids, transforms, source lines), and a game's own components and systems on top of them; built like an exported game (no hot reload). |
| [`04_import_source`](04_import_source/) | A hand-written R3D scene (CC0 models, lights, fog, sky): the test input for `monolith --import` and for the capture shim (`runtime/monolith_capture.h`). |

## Build

```sh
./build.sh                  # once, in the repo root: builds R3D (submodules/r3d) and monolith
examples/build.sh           # -> examples/build/linux/<example>/
examples/build.sh --win     # -> examples/build/win/<example>/ (MinGW-w64; run ./build.sh --win first)
examples/build.sh test      # -> examples/build/linux-test/<example>/ (test build: --shot, no window)
examples/build.sh lsan      # -> examples/build/linux-lsan/<example>/ (test build + LeakSanitizer; tools/leakcheck.sh runs them)
```

The examples compile against monolith's R3D copy with `-DMS_PATCHED_R3D`, with the game's
flags (zero warnings) and static linking, and the runtime as an export has it (without
`-DMONOLITH_EDITOR`), except `03_live_editing` and the capture build, which need editor functions. Each output folder holds the program and the data it
loads (`scene/` or `assets/`), so it runs from any working directory. `examples/build/` is not
committed.

A game build of the runtime reads only compiled maps (`<name>.mapb`, [docs/compiled-map.md](../docs/compiled-map.md)):
it has no text parser. So `examples/build.sh` copies `01_minimal/scene/` next to each game-style
program and compiles its `courtyard.map` there with `build/linux/monolith --compile` (which is why
`./build.sh` comes first, also before `--win`). `03_live_editing` is an editor build and loads
the text, which it hot-reloads. To open another package with a game-style example, compile it
first: `build/linux/monolith --map <package> --compile <package>/<name>.mapb`.

## The test build: screenshots without a window

The examples are plain integration code: they open a window and run until it is closed. Tests
use a separate build of the same sources, `examples/build.sh test` (and `lsan` for
`tools/leakcheck.sh`), which compiles each example's main file with
`-include tests/examples/harness.h`. That build needs `--shot <file>`: it renders 30 frames
into a texture in a hidden window (never mapped, titled `monolith-shot`, so an i3 rule such as
`for_window [title="^monolith-(bench|shot)$"] move container to workspace number 10` routes it),
with a fixed 1/60 s frame time, saves the image and exits; without `--shot` it refuses to run.
The harness redirects the program's `InitWindow`, `R3D_Begin*`, `MS_Draw` and `EndDrawing` calls
(see its header comment); nothing in the examples refers to it.

```sh
examples/build/linux-test/01_minimal/minimal --shot /tmp/01.png --view 2
examples/build/linux-test/02_existing_project/after --shot /tmp/02-after.png
```

Rendering is deterministic: the same build gives the same image on every run. `01_minimal`
renders exactly like `monolith --shot --view <n>` of the same package (same camera math, FXAA,
depth clamp): the images are pixel-identical.

## Graphics quality presets

The runtime carries the editor's quality presets (`MS_Quality`, `runtime/monolith_scene.h`):
`MS_QualityPreset(MS_QUALITY_LOW | MEDIUM | HIGH | ULTRA)`, applied with
`MS_ApplyQuality(scene, &q)` right after every `MS_ApplyEnvironment(scene)`, then
`MS_ApplyRenderResolution(scene, w, h)` (render scale). The shadow map size `q.shadowSize` is an
R3D init hint: pass it as `MS_LoadOptions.shadowSizeOverride` to the peek load before
`R3D_Init` and to the real load. Ultra is the map exactly; lower presets trade SSAO/SSIL for the
baked occlusion volume, bake the fog's sun visibility, render at 75% / 50% and so on
(docs/user-guide.md "Graphics settings"). `01_minimal --quality medium` shows it:

```sh
examples/build/linux/01_minimal/minimal --shot /tmp/01-medium.png --view 2 --quality medium
```

`--quality ultra` (or no `--quality`) renders the same pixels as before. The examples' own
trade-off code paths are the editor's: `monolith --shot --view 2 --quality medium` of the same
package gives the same image.
