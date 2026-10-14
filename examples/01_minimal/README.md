# 01_minimal: load an exported scene and orbit around it

`main.c` is the smallest complete program that shows a monolith map package: parse it once
without the GPU (errors before a window opens, and the sun's shadow map size is an R3D init
hint), open the window, `R3D_Init`, `MS_InitRenderer`, `MS_Load`, `MS_ApplyEnvironment` (then
`MS_ApplyQuality` and `MS_ApplyRenderResolution`, for `--quality`), then `MS_Draw` every frame.
The camera starts at the map's first viewpoint and orbits the point it looks at.

```sh
examples/build/linux/01_minimal/minimal                      # scene/ next to the executable
examples/build/linux/01_minimal/minimal path/to/package      # any package folder or .map file
examples/build/linux/01_minimal/minimal --shot /tmp/a.png --view 2 [--size 1600 900]
examples/build/linux/01_minimal/minimal --shot /tmp/b.png --eye 0 1.62 16 --look 180 0
examples/build/linux/01_minimal/minimal --shot /tmp/c.png --view 2 --quality medium   # a quality preset
```

Keys: mouse wheel zooms, LEFT/RIGHT switch viewpoints, ESC quits.

## `scene/`

A small sample package (`courtyard.map`, 30 boxes): seven materials, one of them a texture
(`textures/tiles.png`) and one a surface shader (`shaders/stripes.glsl`, uniforms from the map),
four point lights, a late-afternoon environment and three viewpoints. It was made with
`monolith --new-map`, edited, and written here with `monolith --map <it> --export <dir>` (this is
the package folder of that export). `MAP.md` is its design brief, `ASSETS.md` says it has no
external assets.

## Same image as the editor

`minimal --shot x.png --view n` and `monolith --shot y.png --view n --map examples/01_minimal/scene`
are pixel-identical (same 1600 x 900 default, the same viewpoint-to-camera math as monolith's
spectator, FXAA, `MS_InitRenderer()` (depth clamp), 30 frames into a `RenderTexture`). The same holds
for an exported default map built in a folder outside the repo.

Keep it dependent on raylib, R3D and `monolith_scene.h` only. (An export's own `viewer.c` is
`runtime/export/viewer.c`: a fly camera and the map's viewpoints, without `--shot` or other
tool flags.)
