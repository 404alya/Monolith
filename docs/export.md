# Export

How to use a monolith map in your own raylib + R3D program. (The other direction, bringing a project's scene into monolith, is `--import`: [user-guide.md](user-guide.md).) Code: `src/export.c`, `runtime/export/`; [architecture.md](architecture.md) §4.3.

`monolith --export <dir> [--map <path>]`, or **F9** in the game (the open map, into `export/<map>-<stamp>/`), writes one map as a self-contained folder: someone extracts it on a clean machine, runs `./build.sh` (Linux) or `./build.sh --win` (MinGW-w64), and gets a viewer that draws the map exactly as monolith does, with the map's viewpoints. Layout:

| Path | What |
|---|---|
| `<name>/` | the map package: `<name>.map` (byte for byte), `MAP.md`, `ASSETS.md`, every file the map references, sidecars, licence files next to them, the files model files refer to (`.gltf` URIs, `.obj` → `.mtl` → maps). Unreferenced files are listed, not copied. |
| `monolith_scene.h/.c` | the scene runtime **without its `#ifdef MONOLITH_EDITOR` blocks**: no hot reload, source lines, writers or editor toggles; loading, drawing, quality presets, spawn / viewpoints / colliders and the queries stay |
| `viewer.c`, `build.sh` | from `runtime/export/` with `@MAP@` = the map's name: a plain example program (window, fly camera, viewpoint names on screen) and the one-step build (R3D + raylib + assimp as static libraries via CMake, `-DMS_PATCHED_R3D`) |
| `r3d/` | the patched R3D monolith builds with (`submodules/r3d` or its `R3D_DIR`), with `patches.md` and the vendored raylib, assimp, glad, tinycthread, uthash; only what a build reads (`R3D_KEEP` / `R3D_SKIP` in `src/export.c`; no `.git*`, build folders, examples, docs, tests): ~33 MB |
| `prebuilt/<linux\|win>/lib/` | `libr3d.a`, `libraylib.a`, `libassimp.a`, `libzlibstatic.a` exactly as monolith's `build.sh` built them from that tree (R3D's GPU profiler compiled in, off at run time): `build.sh` only compiles the viewer and runtime and links (~2 s); if that link fails (an older toolchain or glibc than this machine's gcc 16 / glibc 2.44) it builds from `r3d/`, as `--from-source` always does. A target is left out, with the reason in the log, when its libraries are missing, not a static Release build, or its last build (`monolith-build.log`) failed or ran before the newest R3D / raylib / assimp source changed: run `./build.sh` / `./build.sh --win` before exporting. ~26 MB for both targets |
| `cmake/`, `LICENSES/`, `README.md` | the MinGW toolchain file; R3D, raylib, assimp and uthash licence texts; the README (bundled build first, using the scene in another program, own R3D as the alternative, patched vs. upstream R3D, quality presets, depth clamp, licences + the `ASSETS.md` table) |

Nothing partial: every source is checked first (a referenced file that is missing fails the export and nothing is written), then the folder is written as `<dir>.partial`, its package loaded again and renamed. R3D and the runtime read nothing else at run time (R3D's shaders and lookup tables are compiled in). The export needs the repo (a copied build folder can't export). Keep packages exportable: package-relative paths only, every file recorded in `ASSETS.md`.

Build needs on the target machine (not bundled): with the prebuilt libraries a C compiler, the C++ runtime and on Linux libGL / X11; from source also cmake ≥ 3.22, python3 (R3D's shader embedding), a C++ compiler and on Linux the X11 / GL development headers (listed in the README); `--win` the MinGW-w64 cross-compilers.

## Using the scene in your own program

The exported `viewer.c` is the complete example. The call order: `InitWindow`; `R3D_Init`; `MS_InitRenderer`; `MS_Load`; `MS_ApplyEnvironment` (+ `MS_ApplyQuality` for a quality preset) + `MS_ApplyRenderResolution`; per frame `MS_Draw` (or `MS_PushLights` + `MS_DrawGeometry` inside your own `R3D_Begin` … `R3D_End`); `MS_Unload` before `R3D_Close`. Collision and picking: `MS_Collides`, `MS_Raycast`. The API is documented in `monolith_scene.h`; the examples in `examples/` (in this repo) show a minimal program, adding a scene to an existing project, and live editing.
