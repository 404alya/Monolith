# Code reference

One page per source file whose reference text is too long for a comment at the top of the file: key and flag lists, file formats, rules, environment variables, exit codes. Each of these source files opens with a short comment (what the module is, its key invariants) that ends with `Reference: docs/code/<name>.md`. Where a page here and a user page (such as [command-line.md](../command-line.md) or [map-format.md](../map-format.md)) cover the same thing, the page here is the complete reference and the user page links to it. Where either disagrees with the code, the code decides.

These pages are not part of an export: the exported `monolith_scene.c` / `.h`, `nob.c` and `build.sh` name them as pages of the monolith repository.

## Editor (`src/`)

| Page | Documents |
|---|---|
| [main.md](main.md) | `src/main.c`: every key (walking, spectator, map designer, modes), every command-line flag, the environment variables, the view-only build |
| [launcher.md](launcher.md) | `src/launcher.c`: request bundles, the coordinator and per-request modes, terminals, permissions, models, workspaces |
| [mapedit.md](mapedit.md) | `src/mapedit.c`: manual edits as minimal text edits, entity ids, undo / redo and rebase, the edit-script language |
| [editmode.md](editmode.md) | `src/editmode.c`: the edit mode (**M**): how edits work, its keys, undo with open requests |
| [transform.md](transform.md) | `src/transform.c`: the transform modal (locks, typed values, snapping) and the rotate gizmo |
| [drawmode.md](drawmode.md) | `src/drawmode.c`: the draw mode: paint atlases, allocation and undo, unwrapping, strokes, saving |
| [modelpicker.md](modelpicker.md) | `src/modelpicker.c`: the model picker's list and thumbnails |
| [idpanel.md](idpanel.md) | `src/idpanel.c`: the id panel (F5) |
| [classifier.md](classifier.md) | `src/classifier.c`: the AI classifier that picks a request's model |
| [input.md](input.md) | `src/input.c`: the keyboard / mouse shim and input scripts |
| [ui.md](ui.md) | `src/ui.c`: fonts, the atlas, shapes and text layout of the editor UI |
| [export.md](export.md) | `src/export.c`: what `--export` writes, the runtime without its editor parts |
| [bake.md](bake.md) | `src/bake.c`: `--bake-surface`, baking a surface shader into a tile |
| [dumpscene.md](dumpscene.md) | `src/dumpscene.c`: `--dump-scene`, `--dump-entities`, `--dump-scene-diff` |

## Runtime and builds

| Page | Documents |
|---|---|
| [monolith_scene.md](monolith_scene.md) | `runtime/monolith_scene.c` and `.h`: overview, minimal use, and the complete `.map` format reference (authoritative) |
| [nob-export.md](nob-export.md) | `runtime/export/nob.c`: the build program shipped in every export |
| [nob.md](nob.md) | `nob.c` (repo root): monolith's build program: every `./build.sh` mode, its environment, timing and output |
| [nob-examples.md](nob-examples.md) | `examples/nob.c`: the examples' build program and its modes |

## Tools (`tools/`)

| Page | Documents |
|---|---|
| [run-sh.md](run-sh.md) | `tools/run-sh.c`: the guarded shell: every rule, the hook, the log, its limits |
| [leakcheck.md](leakcheck.md) | `tools/leakcheck.c`: the leak check: what it runs, how leaks are classified |
| [tool.md](tool.md) | `tools/tool.h`: what every tool shares |
| [request-queue.md](request-queue.md) | `tools/request-queue.c`: the request queue on disk |
| [test-coordinator.md](test-coordinator.md) | `tools/test-coordinator.c`: end-to-end test of the request queue |
| [stub-coordinator.md](stub-coordinator.md) | `tools/stub-coordinator.c`: a fake request coordinator |
| [stub-classifier.md](stub-classifier.md) | `tools/stub-classifier.c`: a fake AI classifier |
| [stub-terminal.md](stub-terminal.md) | `tools/stub-terminal.c`: a fake terminal |
| [profile.md](profile.md) | `tools/profile.c`: CPU profile of a monolith run |
| [bench-export-ab.md](bench-export-ab.md) | `tools/bench-export-ab.c`: A/B benchmark of two exports |
| [old-monolith.md](old-monolith.md) | `tools/old-monolith.c`: build an older monolith |
| [todo.md](todo.md) | `tools/todo.c`: the developer's task list |
| [ai-todo.md](ai-todo.md) | `tools/ai-todo.c`: the AI's task list |
| [fetch_asset.md](fetch_asset.md) | `tools/fetch_asset.py`: licence-safe asset downloads |
| [record_asset.md](record_asset.md) | `tools/record_asset.py`: recording an asset's provenance |
| [normalize_model.md](normalize_model.md) | `tools/normalize_model.py`: normalising a model in Blender |

## Tests (`tests/`)

| Page | Documents |
|---|---|
| [harness.md](harness.md) | `tests/examples/harness.h`: the examples' test build: `--shot` and `--bench` without changing their source |
| [export_strip_test.md](export_strip_test.md) | `tests/unit/export_strip_test.c`: an export's runtime has no map-text parser |
| [run_sh_test.md](run_sh_test.md) | `tests/unit/run_sh_test.c`: the test of run-sh: rules, `-c`, lock, log, hook |
