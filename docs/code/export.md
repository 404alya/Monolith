# `src/export.c`

Exports one map as a self-contained folder that builds and runs on its own. For the user's side see [export.md](../export.md); for where it fits in the program, [architecture.md](../architecture.md) "Export, examples".

Two ways in, both writing the same folder:

- `--export <dir> [--map <path>]` (no window);
- the editor key **F9**: the open map, into `export/<map>-<stamp>/`.

## What it writes

| Path | Contents |
|---|---|
| `<dir>/README.md` | Build steps, how to use the scene in another raylib + R3D program (bundled or own R3D), quality presets, licences. |
| `<dir>/build.sh` | The one-step build of the viewer: Linux, `--win` for MinGW-w64. Only a launcher (= `runtime/export/build.sh`): it compiles `nob.c` into `build/nob` and runs it. |
| `<dir>/nob.c` | The viewer's build program (= `runtime/export/nob.c` with the map's name filled in), written on nob.h ([nob-export.md](nob-export.md)). |
| `<dir>/nob.h` | nob.h (= `src/vendor/nob/nob.h`); its licence in `LICENSES/nob.txt`. |
| `<dir>/viewer.c` | A plain example program: window, fly camera, the map's viewpoints (= `runtime/export/viewer.c` with the map's name). |
| `<dir>/monolith_scene.h/.c` | The scene runtime without the editor parts (see [The runtime without its editor parts](#the-runtime-without-its-editor-parts)). |
| `<dir>/<name>/` | The map package: `<name>.mapb` (the compiled map the program loads, [compiled-map.md](../compiled-map.md)), `<name>.map` (its source text, byte for byte; the program never reads it), `MAP.md`, `ASSETS.md` and every file the map references (shaders, textures, panoramas, models), plus provenance sidecars (`<file>.asset.md`), licence files in the same folders and the files a model file refers to (`.gltf`: buffers and images; `.obj`: `.mtl` and its texture maps). |
| `<dir>/r3d/` | The patched R3D monolith builds with (`submodules/r3d`, or the `R3D_DIR` it was built with), with `patches.md` and its vendored raylib, assimp, glad, tinycthread and uthash: only what a build needs (`R3D_KEEP` / `R3D_SKIP` in `export.c`: no `.git*`, build folders, examples, docs or test data). |
| `<dir>/prebuilt/<linux\|win>/lib/` | `libr3d.a`, `libraylib.a`, `libassimp.a`, `libzlibstatic.a` as monolith's `build.sh` made them from that tree (`AddPrebuilt`: only current static Release builds; a target without them builds from source). |
| `<dir>/cmake/mingw-w64-x86_64.cmake` | The toolchain file `build.sh --win` uses. |
| `<dir>/LICENSES/` | The licence texts of monolith (MIT), R3D, raylib, assimp, uthash and nob.h (`nob.txt`: the licence block at the end of nob.h, `NobLicence`). |

## The runtime without its editor parts

The runtime is copied without its editor parts (`StripEditor`):

- every `#ifdef MONOLITH_EDITOR` block is dropped;
- every `#ifndef MONOLITH_EDITOR` block is kept, without its directives.

So the exported files have no `MONOLITH_EDITOR` left in them; anything else fails the export.

R3D and the runtime load nothing from disk at run time besides the map package: R3D's shaders and lookup tables are compiled into the library, and raylib's default font too.

## Paths

The loader only accepts package-relative paths (no absolute paths, no `..`), so the map file is copied byte for byte and every path in it stays valid. The map file is renamed to `<name>.map` only so that the folder loads by its name. Files of the source package that nothing refers to are not copied; the export lists them.

## Nothing partial

1. Every source is checked before anything is written: a referenced file that is missing fails the export with the list of missing files.
2. The folder is written as `<dir>.partial`.
3. Its map is compiled there (`MS_Compile`).
4. The compiled map is loaded again (no GPU).
5. The folder is renamed to `<dir>`.

If writing, compiling or the check fails, the files written into `<dir>.partial` are removed again.

A compiled map exports from its source text, which must be next to it and unchanged since it was compiled.

## Sources

The export reads from the repo: `PROJECT_PATH` (`runtime/`, `cmake/`) and `R3D_SOURCE_PATH`, the R3D tree `build.sh` built against. Without the repo (a copied build folder) the export fails with a message.
