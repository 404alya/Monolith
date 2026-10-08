# monolith — Project Instructions

Context for anyone (human or AI assistant) modifying this project. Read this before changing code.

## 1. What this is

**monolith** is a real-time **3D scene project**: a first-person, explorable brutalist concrete environment rendered with raylib + R3D in plain C. You can walk through it or fly around it with a spectator camera.

The focus is on how the scene looks: light, shadow, fog, scale and composition.

Design principles:

- **No modelling pipeline.** All architecture is built **in code from axis-aligned boxes**, and the concrete look comes from a **procedural shader** with no textures.
- **Light and composition over detail.** Shadows, light shafts through volumetric fog, distance fog swallowing huge silhouettes, a human-scale viewer among enormous structures.
- **Plain C on a lightweight renderer.** raylib + R3D rather than a full engine.

Current state: a single hand-built scene with walk and spectator cameras, live effect toggles, offscreen screenshots and a benchmark mode. There is no level file format; the scene is defined in code.

## 2. Technology

| Item | Details |
|---|---|
| Language | C11 (`-std=c11 -Wall -Wextra`); the program is `src/main.c`, plus a small Linux build shim `src/glibc_compat.c` |
| Windowing / input | raylib 5.5 (bundled with R3D, linked statically) |
| Renderer | **R3D** 0.10.x (Bigfoot71/r3d): deferred PBR on top of raylib. Provides shadows, SSAO, SSIL, SSGI, SSR, fog, volumetric fog, bloom, tonemapping, procedural sky, IBL, instancing, surface shaders. |
| Assimp | Linked because R3D depends on it; not used directly |
| OpenGL | 3.3 core (through R3D/raylib). `libGL` (`opengl32` on Windows) is linked only for `glEnable`/`glFinish`/`glGetString`, declared by hand in `main.c` (see §7.2). |
| Platform | Linux (X11/GLFW), the main target. Windows x86_64 is cross-compiled with MinGW-w64 (`./build.sh --win`) and tested under wine. |
| Dev machine | CachyOS, NVIDIA RTX 4050 Laptop GPU, driver 615.71 |

**R3D location:** vendored as a plain copy (not a git submodule) in **`submodules/r3d`**, with its bundled raylib and assimp. `build.sh` builds it on first use as static libraries: `submodules/r3d/build-linux` for Linux and `submodules/r3d/build-mingw` for Windows. Headers are in `submodules/r3d/include/r3d/`, docs in `submodules/r3d/docs/shaders/*.md`, examples in `submodules/r3d/examples/`. The R3D source is the best reference for its behaviour; check it rather than guessing. Paths like `shaders/deferred/…` or `src/modules/…` elsewhere in this file are relative to `submodules/r3d`.

> **R3D changes are patches.** Work around R3D limitations from the app side where reasonable. When R3D itself has to change (e.g. a real bug), change `submodules/r3d` minimally and **record every change in `submodules/r3d/patches.md`** (symptom, cause, files, diff), so the copy can be refreshed from upstream later. Don't modify `~/Projects/r3d` (the original checkout) or other projects in `~/Projects` (e.g. `gameproject2`).

## 3. Layout

```
monolith/
├── build.sh                     # build script (see §4)
├── .clangd                      # include paths for clangd (absolute paths into submodules/r3d)
├── .gitignore                   # build/, build-mingw/, screenshot*.png
├── cmake/
│   └── mingw-w64-x86_64.cmake   # toolchain for cross-building R3D for Windows (--win)
├── PROMPT-INSTRUCTIONS.md       # this file
├── benchmark/                   # --bench reports: <timestamp>_shadow-<size>-<range>.md (kept in git)
├── src/
│   ├── main.c                   # the whole program (~1000 lines), split into #regions
│   └── glibc_compat.c           # Linux only: binds a few symbols to old glibc versions (§4)
├── resources/
│   └── shaders/
│       └── concrete.glsl        # procedural board-formed concrete (R3D surface shader)
└── submodules/
    └── r3d/                     # vendored R3D (+ raylib, assimp); local fixes in patches.md
```

`InitDataPaths()` sets the runtime `resourcesDir` / `benchmarkDir` (code uses those, not the macros). On Linux the repo directories baked in at compile time (`-DRESOURCES_PATH=".../resources/"`, `-DBENCHMARK_PATH`) win while they exist; otherwise, like on Windows, `resources/` and `benchmark/` next to the executable are used (`GetApplicationDirectory()`). So the binary runs from any working directory, and `build/linux/` and `build/win/` can be moved to other computers.

## 4. Build and run

```bash
./build.sh              # -> build/linux/monolith + build/linux/resources/  (clang, -O2)
./build.sh run [args]   # build, then run with args
./build.sh debug        # -g -O0 + AddressSanitizer/UBSan, same output folder
R3D_DIR=/other/r3d ./build.sh   # use a different R3D source tree (default submodules/r3d)
CC=gcc ./build.sh               # use another compiler

./build.sh --win                # -> build/win/monolith.exe + build/win/resources/ (MinGW-w64, -O2)
./build.sh --win run [args]     # build, then run under wine
```

Both output folders, `build/linux/` and `build/win/`, are **self-contained**: copy or zip the folder as-is to run it on another computer.

**R3D** is built by `build.sh` itself: on first use it configures `$R3D_DIR/build-linux` (Linux) or `$R3D_DIR/build-mingw` (Windows) and after that runs an incremental `cmake --build` each time (a ~1 s no-op unless R3D's sources changed; output in `<build dir>/monolith-build.log`). The first build compiles assimp and takes a few minutes. Delete the build directory to force a fresh configure.

**Linux:**
- R3D is configured in `$R3D_DIR/build-linux`: static libraries, vendored raylib + assimp + zlib (`ASSIMP_BUILD_ZLIB`), assimp's bundled unzip (`ASSIMP_BUILD_MINIZIP`; a system minizip breaks the build), no examples, and **no `CMAKE_BUILD_TYPE`, so R3D/raylib/assimp are compiled without optimisation**, like the original `~/Projects/r3d/build`. This keeps the numbers comparable with the existing reports in `benchmark/`; switching to `-DCMAKE_BUILD_TYPE=Release` would likely lower CPU-side frame time and should be benchmarked as a separate change.
- The executable links R3D, raylib, assimp, zlib and **libstdc++ statically** (`-l:libstdc++.a`; the host's libstdc++ is newer than most distros'). Its only runtime dependencies are `libc`, `libm`, `libgcc_s.so.1` and the GPU driver's `libGL.so.1`, all present on any desktop Linux. No rpath, no `LD_LIBRARY_PATH`. Check with `readelf -d build/linux/monolith | grep NEEDED`.
- **glibc ≥ 2.34** (Ubuntu 22.04+, Debian 12+, Fedora 35+, RHEL 9+). Linking against the host's glibc 2.44 would otherwise require e.g. `acosf@GLIBC_2.43`. `build.sh` passes `-Wl,--wrap=` for those symbols, and `src/glibc_compat.c` routes them to old versions (libm functions to `@GLIBC_2.2.5`, `arc4random` to `getrandom`, `__isoc23_strtoul` to `strtoul`). After changing compiler, glibc or dependencies, check `objdump -T build/linux/monolith | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1` still prints `GLIBC_2.34`, and add any newer symbol to `GLIBC_WRAP` in `build.sh` and to `glibc_compat.c`.
- Data paths: the repo's `resources/` and `benchmark/` (baked in with `-DRESOURCES_PATH`/`-DBENCHMARK_PATH`) are used while they exist, so on this machine shader edits apply without rebuilding and reports land in the repo. On any other computer the folders next to the executable are used (`resources/` is copied there by `build.sh`; `benchmark/` is created on the first `--bench`). Note that a portability test on this machine still uses the repo paths unless they're hidden, e.g. `unshare -rm sh -c 'mount -t tmpfs none ~/Projects/monolith && /path/to/copy/monolith --shot /tmp/x.png --view 2'`.
- x86-64 only (no ARM build).

**Windows (`--win`)** cross-compiles with `x86_64-w64-mingw32-gcc`:
- R3D is configured in `$R3D_DIR/build-mingw` with `cmake/mingw-w64-x86_64.cmake`: Release, static libraries, vendored raylib + assimp + zlib, no examples.
- The exe is linked **fully static** (`-static`: R3D, raylib, assimp, zlib, libstdc++, libgcc, winpthread), so `build/win/` holds just `monolith.exe` (~7 MB, stripped) and a copy of `resources/`, with no DLLs to ship. Its imports are only system DLLs: `opengl32`, `gdi32`, `user32`, `kernel32`, `shell32`, `winmm` and the UCRT `api-ms-win-crt-*` set (part of Windows 10+). Check with `x86_64-w64-mingw32-objdump -p build/win/monolith.exe | grep "DLL Name"`.
- It's a console-subsystem exe, so `--bench` tables and the **P** output are visible; started from Explorer it opens a console window next to the game.
- Bench reports from the Windows build go to `build/win/benchmark/`, not the repo's `benchmark/`.
- `--win debug` is refused: ASan/UBSan don't work with MinGW.
- Verify like Linux, under wine (it uses the host GL driver, so shots are pixel-identical to the Linux build): `wine build/win/monolith.exe --shot win.png --view 2`, or `__GL_SYNC_TO_VBLANK=0 wine build/win/monolith.exe --bench --headless --bench-frames 150`. The `MESA-EGL: warning` lines wine prints on NVIDIA are harmless.

A change is done only when it **builds with no warnings** on both targets (`./build.sh` and `./build.sh --win`) and has been **checked visually** (see §8).


## 5. Controls and command line

The header comment at the top of `src/main.c` is the source of truth; keep it updated when adding controls or flags.

**Walk mode (default)**

| Key | Action |
|---|---|
| WASD + mouse | Walk and look |
| Shift | Walk faster |
| Space | Jump |
| F | Toggle fly (noclip) |
| 1–6 | Toggle volumetric fog / distance fog / SSAO / SSIL / bloom / sun |
| F1 | Toggle HUD |
| F12 | Save `screenshot.png` |
| TAB | Toggle spectator mode |
| Esc | Quit |

**Spectator mode (TAB)**

| Key | Action |
|---|---|
| WASD + mouse | Move and look |
| E / Q (or Space / Ctrl) | Up / down |
| Mouse wheel | Base speed, 1–400 m/s, exponential steps |
| Shift / Alt | ×4 faster / ×0.25 slower |
| ← / → | Fly to the previous / next preset viewpoint (eased, arcs upward on long trips) |
| P | Print the current view as `--pos … --look …` and copy it to the clipboard |
| G | Move the player to the camera position |

**Command line**

| Flag | Meaning |
|---|---|
| `--pos x y z` | Player start position (feet) |
| `--look yawDeg pitchDeg` | Start view angles |
| `--view n` | Start in spectator mode at viewpoint *n* (1-based) |
| `--off <digits>` | Start with these effect toggles off, e.g. `--off 16` |
| `--shot <file>` | Render 30 frames offscreen, save the image to `<file>` (full path OK, 1600×900), exit. Uses a **hidden window** (`FLAG_WINDOW_HIDDEN`), so the window manager (i3) never maps, shows or focuses it. |
| `--shadow <size> <range>` | Sun shadow map size (px) and shadow distance (m). Default `8192 100`. |
| `--shadow-always` | Re-render the sun shadow map every frame (the old behaviour). By default it is re-rendered only when the cached map no longer covers the view (§7.3). For A/B benchmarks and debugging. |
| `--bench` | Benchmark: for each viewpoint, 60 warmup frames, then 600 measured frames while spinning the camera 360°. Prints a table and writes a **Markdown report** to `benchmark/<timestamp>_shadow-<size>-<range>.md` (GPU, driver, resolution, settings, command; avg ms, avg FPS, 1% low, worst frame and number of shadow-map re-renders per view). Reports are written only for completed runs. |
| `--headless` | With `--bench`: hidden window (never mapped), offscreen 1920×1006 target, `glFinish` after every frame. Doesn't touch the screen or need workspace 10 shown. Absolute times are ~10% higher than visible runs (no CPU/GPU overlap) and noisier if the machine is in use, so compare headless only with headless. |
| `--bench-frames <n>` | Timed frames per viewpoint for `--bench` (10–600, default 600). Use ~150 for quick screening runs (~10 s); keep 600 for numbers you report. Fewer frames = faster spin, so more shadow re-renders. |
| `--note "text"` | Free-form note placed at the top of the benchmark report (e.g. what changed). |

Vsync is **off** and the frame rate is uncapped. For benchmarks, also disable driver vsync:
`__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench` (NVIDIA) or `vblank_mode=0` (Mesa).

## 6. Code architecture (`src/main.c`)

The file is split into `// #region` blocks, in this order:

1. **World**: `World world` is a fixed array of `BoundingBox` (`MAX_BOXES` 8192). Every box is used **both for rendering and for collision**. Builder helpers:
   - `Box(x0,y0,z0, x1,y1,z1)`: box from two corners, in any order.
   - `Block(cx, y0, cz, sx, sy, sz)`: box from a ground-centre point and a size.
   - `Stairs(cx, baseY, startZ, width, steps, rise, run)`: solid steps climbing toward −Z.
   - `WaffleFacade(...)`: back wall plus vertical fins plus horizontal ledges (deep window grid).
   - `BuildWorld()`: the whole level, in numbered sections (see §6.1).
2. **Player**: feet-based AABB (r 0.3, h 1.75, eye 1.62). Movement is per axis with collision against all boxes using **`BoxesOverlap` (strict)**; see §7.4. Includes step-up for stairs (`STEP_HEIGHT` 0.45), gravity, jump, fly/noclip, smoothed acceleration and head bob. The camera is built by `PlayerCamera()`.
3. **Spectator camera**: `Spectator` state, the `VIEWPOINTS[]` table (name, eye, target), `UpdateSpectator()`, `SpectatorFlyTo()` / `SpectatorJumpTo()`, and `SpectatorCamera()`. Yaw/pitch convention: `LookDir(yaw, pitch) = (cos p · sin y, sin p, cos p · cos y)`, so yaw 0 looks toward +Z and yaw 180° toward −Z.
4. **Atmosphere**: `SUN_DIR` (direction light *travels*) and `SetupEnvironment()`, which sets the procedural sky, IBL ambient map, distance fog, volumetric fog, SSAO, SSIL, bloom, tonemap and colour grading.
5. **Main**: argument parsing, the shadow-size hint, `R3D_Init`, `glEnable(GL_DEPTH_CLAMP)`, world → **one instanced draw** (a unit cube; per-instance position = box centre, scale = box size), material + surface shader, sun light + shadow map, then the main loop (toggles, TAB, update, draw, HUD, `--shot` / `--bench` handling) and cleanup.

Between Atmosphere and Benchmark there is a **Sun shadow reuse** region (`SunShadowFit`, `SunShadowNeedsUpdate`, see §7.3).

Rendering per frame:
```c
R3D_BeginPro(view);                      // view.target = RenderTexture in --shot mode
    bool update = !shadowReuse || SunShadowNeedsUpdate(&sunCache, ...);
    R3D_PushLightEx(sun, sunShadow, update); // re-render the shadow map only when needed
    R3D_DrawMeshInstanced(cube, concrete, instances, world.count);
R3D_End();
```

### 6.1 Scene map (metres; Y up; the player walks toward −Z)

| # | Area | Location |
|---|---|---|
| 1 | Entry canyon: 34 m walls, roof segments with thin light slits; the player starts at `(0,0,90)` | x ±4.5, z 40…97 |
| 2 | Hypostyle hall: 2.2 m pillars on a 9 m grid, 20 m tall, slatted roof beams; a massive block sits on the roof | x ±40, z −36…39 |
| 3 | Monumental stairs: 40 steps, 12 m rise, with side walls | z −40…−70 |
| 4 | Upper terrace (y = 12) with a waffle-facade tower (to y = 96) and a lone pillar | z −70…−140 |
| 5 | Distant megastructures (up to 260 m tall) and a bridge at y 120; fog silhouettes | 120–300 m out |

The ground slab is 800 × 800 m with its top at y = 0.

### 6.2 Concrete shader (`resources/shaders/concrete.glsl`)

An R3D **surface shader** (`#pragma usage opaque shadow`, `void fragment()`). Everything is derived from the **world-space `POSITION`** and **`NORMAL`**, so boxes of any size keep a constant texel density without UVs. Because boxes are axis-aligned, it projects onto the dominant axis plane (not full triplanar blending).

- **Vertical faces:** 2.4 × 1.2 m formwork panels with per-panel tone, seams, 15 cm board marks, 2×2 tie holes per panel, and patchy rain streaks.
- **Top faces:** 6 m expansion joints and wet patches (low roughness).
- **All faces:** fbm mottling and grain; grime and a moss tint near the ground (y < 2.2).
- **Base albedo:** about `0.22 + 0.12·noise`, kept dark so sunlit concrete doesn't blow out to white.
- **Uniform:** `u_scale` (pattern scale, 1 = metres).

Fragment built-ins are listed in `submodules/r3d/docs/shaders/surface_shader.md` (`ALBEDO`, `ROUGHNESS`, `METALNESS`, `SPECULAR`, `EMISSION`, `NORMAL_MAP`, …).

## 7. R3D pitfalls already discovered (read before touching lighting)

These were found by debugging. They explain why some settings look the way they do.

### 7.1 Volumetric fog turns into a black curtain if dense
R3D's volumetric fog is **lit only by lights, not by the sky/IBL**. Fog in shadow therefore absorbs light without adding any, and with high density, buildings 30–60 m away go **pitch black depending on view direction**.

The fix is in place: low `scatteringDensity` (0.012), low absorption (0.001), and a small **`emissionEnergy`** (0.12, coloured like the distance fog) standing in for sky light. Raise the shafts with `sun.fogEnergy` rather than with density.

### 7.2 Directional shadow map is camera-fitted; casters get clipped
R3D fits the sun's ortho shadow map to a bounding sphere of the camera frustum out to `light.range`. Its near plane reaches only about `radius + 100 m` toward the sun, so tall casters (the top of a 260 m megastructure sits about 330 m up the light ray) were **clipped depending on where the camera looked, and their shadows popped**.

The fix is **shadow pancaking**: `glEnable(GL_DEPTH_CLAMP)` once after `R3D_Init`. R3D never touches that state, so it applies to R3D's shadow pass. The side effect is that geometry closer than the 1 cm camera near plane (raylib's `RL_CULL_DISTANCE_NEAR` 0.01) is clamped instead of clipped.

**Do not "fix" shadow problems by disabling shadow casting.** That hides the symptom; fix the cause.

### 7.3 Sun shadow: resolution and cost
- **Texel size:** about `2 · radius / size`, with `radius ≈ 1.5 · range` (70° fov, 16:9). At 1920×1006, range 100 m: radius ≈ 159 m.
- **Fade:** R3D fades directional shadows out between 75% and 100% of `range` (by *view depth*), and over the outer 5% of the map (x, y and depth).
- **Range lower bound:** `range` must stay at or above `volumetricFog.length` (90 m) or the light shafts fade early.
- **Current setting:** 8192 px / 100 m, about 3.7 cm texels. VRAM is 16-bit depth × 2 layers, about 256 MB.
- **Root limitation:** there are no cascaded shadow maps in R3D for directional lights. That is the real long-term fix (a library change).

**Where the shadow time goes** (8192/100, `--bench` averages of 3 runs, measured 2026-10-08 with temporary switches; reports in `benchmark/2026-10-08_1157*`–`1206*`):

| Part | Cost | Measured as |
|---|---:|---|
| Rendering the shadow map (every frame) | ~1.2 ms | every-frame vs. map frozen after the first frame |
| Sun's volumetric-fog march, mostly its shadow lookups | ~2.6 ms | default vs. `sun.fogEnergy = 0` |
| Everything shadow-related | ~3.5 ms | default vs. sun pushed without a shadow map |
| Size penalty 8192 vs 4096 | 1.7 ms (fog on), 1.0 ms (fog off) | so ~0.7 ms is the fog's lookups missing the texture cache on the bigger map |

The fog is the big one: `vfog_radiance.frag` runs at half resolution, up to `length / stepSize` = 120 steps per pixel, and **each step does the full 8-tap PCF** (`L_SampleShadowDir`), i.e. up to ~460 M shadow taps per frame. The lighting pass does only 8 taps per pixel (~0.2 ms).

**Shadow-map reuse (in place).** The scene is static, so the map is re-rendered only when needed: `R3D_PushLightEx(sun, sunShadow, update)`. R3D stores the light matrix with the cached map, so a reused map is always sampled consistently. And since R3D's fit radius depends only on range/fov/aspect and the centre is snapped to texels, every fit lays the **same world-space texel grid**, so the reused map has the same content where it covers. `SunShadowNeedsUpdate()` replicates R3D's fit (`light_dir_view_proj`) on the CPU and samples the view frustum out to `range` (48 depth slices × 33×17); it keeps the map only while R3D's fades (edge × distance) from the cached map are within 1/255 of a fresh fit's at every sample. The check costs ~0.1 ms CPU. **If R3D's fit or fade code changes, update `SunShadowFit()` / `ShadowEdgeFade()` to match.**
- Re-renders: walking forward ~1 per 65 m, sideways ~1 per 2.5–4.5 m, a still camera never; the bench spin (0.6°/frame) ~21% of frames, a 360°/s mouse flick ~50%.
- R3D's fit is very generous (the 159 m sphere sits 50 m ahead of the camera, so it covers everything within ~109 m in *any* direction). Even a stale map is almost always right; the check is deliberately strict anyway.
- Verified: static yaw sweeps are pixel-identical to the old build; after 90 frames of turning/walking/strafing/flying, reuse matches every-frame rendering (≤ 0.005% of pixels differ by > 2/255: 16-bit depth-quantisation noise that every-frame rendering has too), while a frozen map differs on up to 21% of pixels (180 m flight).

Measured with `--bench` (RTX 4050 Laptop, 1920×1006, vsync off, averaged over runs):

| Shadow setting | Texel | Frame time | FPS |
|---|---|---|---|
| 4096 / 140 m (old) | ~10.7 cm | 6.00 ms | 167 |
| 4096 / 100 m, re-rendered every frame | ~7.4 cm | 5.85 ms* | 171 |
| 6144 / 100 m, re-rendered every frame | ~4.9 cm | 7.27 ms | 138 |
| 8192 / 100 m, re-rendered every frame (`--shadow-always`) | ~3.7 cm | 7.55 ms* | 132 |
| **8192 / 100 m, reused (current)** | ~3.7 cm | **6.62 ms*** | **151** |

\* 2026-10-08 runs (`benchmark/2026-10-08_1224*`–`1226*` for the last two). The older rows were measured on an earlier day; the GPU ran ~5% faster today, so compare starred rows with each other. The bench spins the camera non-stop, which is the worst case for reuse; with a still camera the frame cost is ~6.3 ms (the shadow-render share disappears).

The cost tracks map size, not range. 6144/100 is the recommended fallback for weaker GPUs.

**Not applied: larger fog `stepSize`** (quality trade-off, decide by eye). Screening (`--bench-frames 150`): 0.75 → 1.0 saves ~0.38 ms, → 1.25 ~0.69 ms. Stills look the same, but 2–5% of hall pixels change by up to ~25/255 (the march noise pattern), and thin shafts may alias differently in motion.

**R3D-side proposals** (library changes, not made here):
1. *Fog shadow lookups* (`shaders/deferred/vfog_radiance.frag`, the `LIGHT_DIR` case in the march loop): use one hardware-PCF tap per step instead of `L_SampleShadowDir`'s 8-tap Vogel disk; the per-pixel jitter plus the bilateral upsample already blur it. Up to ~8× fewer taps, i.e. most of the ~2 ms. Also skip the taps when `edgeFade * distFade * shadowOpacity == 0` (sky pixels pass `depth = far`, so their fog shadow weight is already 0).
2. *Cascaded shadow maps* for directional lights (`light_dir_view_proj` / `light_dir_push` in `src/modules/r3d_light.c`, `L_SampleShadowDir` in `shaders/include/wrap/light.glsl`): e.g. 2–3 × 2048–4096 cascades give finer texels near the camera than today's single 8192 map at a fraction of the fill and cache cost.
3. *Rotation-stable fit:* centre the fit on the camera position (radius = distance to the far corners) instead of 50 m ahead, so turning on the spot never invalidates a cached map. App-side reuse would then only re-render on movement.

### 7.4 Other gotchas
- **`CheckCollisionBoxes` is inclusive:** touching counts as a hit, so a player standing on the ground could never move. Use the project's strict `BoxesOverlap()`.
- **Brightness:** the sun at energy 3.5 with exposure 1.1 made the buildings look white. The current values are sun 2.0, exposure 0.9 (AgX), ambient 0.9 and fog colour grey 150. If things look washed out, lower the **sun energy** or **exposure**. Don't brighten the fog colour.
- **Sun angle:** a low sun from behind the player left every visible face in shadow and produced no shafts. `SUN_DIR = (0.55, −0.78, 0.30)` (high, from the side) is chosen so light falls through the roof gaps and rakes across facades.
- **Distance fog:** EXP2 at density 0.0085 makes aerial and long views (>150 m) very milky. Spectator viewpoints are kept fairly close for that reason.
- **Instance attributes:** the buffer holds only `POSITION | SCALE`, with no rotation. All world boxes are axis-aligned, and both collision and the shader rely on that.
- **Surface-shader uniforms:** they are uploaded at `R3D_End()`, so the last value set in a frame wins. They can't vary per draw within a frame.
- **Hidden-window screenshots:** in `--shot` mode the window is never mapped, so reading the default framebuffer (`TakeScreenshot`) would be undefined: no pixels are owned. Shots are rendered with `R3D_BeginPro` into a `RenderTexture` and read back with `LoadImageFromTexture`, flipped vertically. Don't switch back to `TakeScreenshot`.
- **NVIDIA accepts GLSL that AMD/Intel reject.** Wine uses the host driver, so on this machine neither Linux nor wine shows these errors. Two found so far (shaders failed to compile on a friend's Windows PC with an AMD RX 9060 XT): a function named `noise3` in `concrete.glsl` (clashes with the deprecated built-in `noise3()`, which returns `vec3`; now `valueNoise`), and a uniform-block member named `length` in R3D (`uVFog.length`, patched; see `submodules/r3d/patches.md`). Avoid GLSL built-in names (`noise1`–`noise4`, `length`, `distance`, …) for functions and members, and check shader changes with **`glslangValidator`** (installed), which rejects the same things. For `concrete.glsl`, wrap it in a stub that declares the built-ins it uses and calls `fragment()` from `main()`.
- **Ambient colour vs. ambient map:** setting `ambient.map = {0}` and using a plain `ambient.color` produced no ambient light in testing. Keep the IBL ambient map generated from the sky.

## 8. How to verify changes

There's no test suite; verification is **visual and numeric**:

1. **Build cleanly:** `./build.sh` and `./build.sh --win` with no warnings.
2. **Screenshots from fixed views:**
   ```bash
   ./build/linux/monolith --shot a.png --view 2                       # preset viewpoint
   ./build/linux/monolith --shot b.png --pos 0 0 -5 --look 120 5      # exact view
   ./build/linux/monolith --shot c.png --view 2 --off 16              # with effects off
   ```
   For anything lighting- or shadow-related, take a **sweep of yaw angles from the same position** (e.g. 0/60/…/300) and compare. Both the black-curtain and shadow-popping bugs only showed up when rotating.
3. **Isolate effects** with `--off` (1 volumetric fog, 2 fog, 3 SSAO, 4 SSIL, 5 bloom, 6 sun) to find which one causes a problem.
4. **Performance:** `__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench [--shadow S R] --note "what changed"`. Each run writes a report into `benchmark/`. Laptop GPUs vary, so alternate A/B runs several times and compare averages (run-to-run noise is ~1–2%). Screen ideas with `--bench-frames 150` (~10 s a run); use full runs ×3 only for numbers you report. Unlike `--shot`, `--bench` opens a **visible** window on purpose (it measures real presentation).
   **Automated / AI-driven runs: use `--bench --headless`** so the developer's screen is never taken over. A *visible* bench window must actually be shown: on a workspace that isn't displayed (i3 unmaps it) the run stalls at 100% CPU with the GPU idle and never finishes. Use visible runs only when the developer asks for presentation-inclusive numbers.
5. **Window titles and i3:** automated runs use their own titles (`monolith-bench`, `monolith-shot`; normal play is `monolith`). The developer's i3 config (`~/.config/i3/config`) has
   `for_window [title="^monolith-(bench|shot)$"] move container to workspace number $ws10`,
   so automated runs open on workspace 10 and never take over the focused workspace. Keep these titles if you change window creation.
6. **Movement:** collision and controls can't be checked from screenshots; test them by playing.

## 9. Common modifications

**Add architecture.** Add `Box`/`Block`/`Stairs`/`WaffleFacade` calls in `BuildWorld()`. Rendering, shadows and collision pick them up automatically. Keep boxes axis-aligned and keep the total under `MAX_BOXES`. Think in real scale: player 1.8 m, pillars 20 m, buildings 50–260 m.

**Add a spectator viewpoint.** Fly there in spectator mode, press **P**, and add an entry `{ "Name", {eye}, {target} }` to `VIEWPOINTS[]`. Note that P prints *feet* position (eye − 1.62) for `--pos`, while `VIEWPOINTS` stores the *eye*. `--bench` automatically covers new viewpoints.

**Tune the look.** Everything is in `SetupEnvironment()`, plus the sun setup in `main()` (energy, `fogEnergy`, `range`, shadow `softness`). Respect §7.1–7.3. To see what one effect contributes, toggle it live with keys 1–6.

**Change the concrete.** Edit `concrete.glsl`; no recompile is needed, since it loads at start-up. Keep the albedo dark (concrete is about 0.2–0.4 linear) and derive everything from world position.

**Add a new effect toggle.** Extend the `TOGGLE(KEY_x, 'x')` block in the main loop, the HUD string, and the header comment.

**Add a CLI flag.** Parse it in the loop at the top of `main()` and document it in the header comment and in §5 here.

## 10. Conventions

- **English** for all code, identifiers, comments and project names.
- **Match the existing style:** 4-space indent, `static` functions, PascalCase for functions, `UPPER_CASE` macros, `// #region` sections, and short comments explaining *why* rather than what.
- **Single file** until it really hurts. If you split, keep `build.sh` compiling `src/*.c`. (`src/glibc_compat.c` is build plumbing, not program code; it compiles to nothing outside Linux.)
- **No textures or models unless deliberately introduced.** The procedural, model-free approach is a core constraint.
- **Comment non-obvious engine workarounds** in the code with their reason (as for §7.1 and §7.2), so nobody "cleans them up".
- **Don't commit `build/` or screenshots.**

## 11. Known limitations and next steps

- **Level format:** the level is hard-coded. Next step: load boxes from a file (cJSON or a simple text format) with hot-reload.
- **Decals:** none yet. R3D supports them (`r3d_decal.h`); water stains, rust streaks and graffiti would add a lot.
- **Outdoor haze:** high and outdoor views are very hazy because sunlit volumetric fog fills the whole view. Height-dependent fog would help; R3D's volumetric fog is uniform.
- **Shadow cascades / cheaper fog shadow lookups:** see the R3D proposals in §7.3.
- **Vsync:** off with no toggle; screen tearing is possible.
- **Platform:** Linux, plus a Windows x86_64 cross-build (`./build.sh --win`). Tested under wine (NVIDIA driver). The first run on a real Windows PC (AMD Radeon RX 9060 XT) exposed the strict-GLSL issues in §7.4; the fixes are verified with `glslangValidator` but not yet on that PC. There's no native Windows build script (MSVC / CMake for monolith itself) and no macOS support. The Windows build has no sanitizer mode.
