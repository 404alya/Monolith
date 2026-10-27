# Command line

Every flag and environment variable of `monolith` (Linux: `./build/linux/monolith`, Windows: `build\win\monolith.exe`; the debug, LeakSanitizer and profiling builds of `./build.sh debug|lsan|profile` in `build/linux-<mode>/` take the same flags). The header comment of `src/main.c` is the source of truth. Timings quoted below were measured on an RTX 4050 Laptop GPU: a rough guide only, your machine's will differ. Keys and mouse: [user-guide.md](user-guide.md). "Patch N" means a local change to R3D ([glossary.md](glossary.md)).

Flags that never open a visible window, safe for scripts: `--shot` (renders in a hidden window), `--dump-boxes`, `--dump-scene`, `--dump-scene-diff`, `--dump-entities`, `--apply-edits`, `--inspect-model`, `--new-map`, `--export`, `--compile`, `--classify`, `--bake-surface`, and `--bench` together with `--headless`. `--import` opens no game window either, but it queues an import request, which can open the coordinator's terminal. Anything else opens the game window.

## Flags

An unknown flag, a missing or unparsable value, a value that is itself a `--flag`, or a non-finite / out-of-range number (positions, angles, regions, volumes: |v| ≤ 1e6) prints a message and exits 1 before any window opens.

| Flag | Meaning |
|---|---|
| `--pos x y z` | Player start (**feet**; default: the map's `spawn`) |
| `--eye x y z` | Start **eye** (= `--pos x y-1.62 z`); with `--view` it moves the spectator camera. Used by bundle reproduction commands. |
| `--look yawDeg pitchDeg` | Start angles; with `--view` they override the viewpoint's |
| `--view n` | Start in spectator mode at viewpoint *n* (1-based; a number beyond the map's viewpoints is ignored) |
| `--off <digits>` | Start with these effect toggles off (`--off 16`) |
| `--map <path>` | Package folder (loads `<folder>/<folder name>.map`, else its only `.map`, else a compiled `.mapb`) or a `.map` file instead of `maps/monolith/`; hot-reloaded. A compiled map (`.mapb`) loads too, for `--shot` and the `--dump-*` checks; it has no text, so edits and requests refuse it, and a warning says when its `.map` changed after it was compiled |
| `--ao full\|ssao\|lite\|off\|baked` | AO trade-off (`atmosphere.c`): `ssao` = SSIL off, `lite` = SSIL off + SSAO 8 samples, `off` = both off, `baked` = both off + the occlusion volume (`--ao-volume 0.5 0.6`) |
| `--ao-volume <voxel> <intensity>` | **Trade-off, off.** World-space occlusion volume (signed distance to boxes, 1 byte/voxel, ≤ 32 MB, ~60 ms CPU bake per load) darkening the ambient light (patch 11). View-independent, ~0.03 ms/frame. Boxes only. |
| `--render-scale <s>` | **Trade-off, default 1.** R3D renders at `s` (0.25–1) × the output size; `--upscale nearest\|linear\|bicubic\|lanczos` (default bicubic) |
| `--window-msaa` | 4× multisampled window (smoother overlay lines, ~0.1 ms); scene image unchanged. Default single-sampled. |
| `--bloom on\|off` | `off` skips bloom and multiplies exposure by the map's `env bloom.offExposure` (default 1.19) so the mean brightness stays |
| `--quality low\|medium\|high\|ultra` | Preset for this run (not saved). Default: the settings file in the interactive game, else ultra (= the map exactly) |
| `--settings <file>` / `--no-settings` | Use this settings file instead of `~/.config/monolith/settings.ini` (`$XDG_CONFIG_HOME`; Windows `%APPDATA%\monolith\`) / read and save none |
| `--settings-set key=value` | Change a setting as in the panel and **save** it (in `--shot`, `--dump-*`, `--bench`, `--export` and `--classify` runs only when `--settings <file>` names one) (repeatable, applied last; works with `--dump-boxes` without a window). Keys are the file's: `preset=medium`, `render_scale=0.75`, `ao=baked`, `shadow_size=4096`, `fps_cap=monitor`, … |
| `--settings-change key=value` | With `--shot`: apply after 10 frames through the panel's live path; must give the same image as a shot started with `--settings-set` |
| `--settings-panel` | With `--shot --shot-hud`: draw the open panel |
| `--fx key=value` | One effect override, after `--ao`/`--bloom`: `ssao.samples\|radius\|intensity\|power`, `ssil.samples\|radius\|gi\|ao`, `bloom.levels\|intensity`, `exposure`, `vfog.step\|length`. Range-checked like the `env` key. |
| `--new-map <name\|path>` | Create `maps/<name>/` (or the folder a path with `/` names; its parent must exist): map with ground slab, default material, built-in environment written out, spawn, two viewpoints; `MAP.md` template; empty `shaders/ models/ textures/`, `ASSETS.md`. Checks it loads, no window, refuses to overwrite. |
| `--export <dir>` | No window: `<dir>/<name>/` (the compiled `<name>.mapb`, the `<name>.map` it is compiled from, every referenced file + `MAP.md`, `ASSETS.md`, sidecars, files model files refer to), `monolith_scene.h/.c` (without the text parser), `viewer.c`, `README.md`. Refuses a non-empty dir; checks the compiled copy loads ([export.md](export.md)). A compiled `--map` exports from its `.map`, which must be next to it and unchanged |
| `--compile <out>` | No window: compile the map's text (`--map` or the default) into the compiled map `<out>` (`<name>.mapb`), the file a game build of the runtime loads ([compiled-map.md](compiled-map.md)). Checks the package like `--dump-boxes`, reads the result back, writes it atomically; the same text always gives the same bytes. Paths in it are package-relative, so write it into the package folder (or a copy of it) |
| `--import <project dir> <name\|path>` | Package as `--new-map` + an import bundle `requests/<stamp>_import-<name>/` (`kind: "import"`, heavy tier), queued for the coordinator like a submitted request; no game window |
| `--dump-boxes <file>` | No window: every box as hex floats (`%a`, `min max` per line). Exact-box-list check and full parse check (exit 1 + `file:line: message`); model files are imported on the CPU (a missing or broken one fails; no window) |
| `--dump-scene <file>` | Also model files, instances (transform, world bounds), colliders, materials, lights (`src/dumpscene.c`); hidden window when the map has models |
| `--dump-scene-diff <file>` | The same without object indices and map lines (names instead), so a diff shows only changed objects across inserted lines |
| `--dump-entities <file>` | No window: every entity (box, model instance, light) in map order with its stable id (`<key>:<index>`, explicit `id` or implicit), kind, object index, map line, `repeat` copy, transform and model file ([map-format.md](map-format.md) §2.9) |
| `--apply-edits <script>` | No window: run an edit script against the map file of `--map` (required: it **rewrites that file**), the same edits as the edit mode (**M**): `select line <n> [copy <c>]` / `select id <n> [index <i>]` / `select entity <hex> <index>` / `select none` (`also …` adds), `move dx dy dz`, `turn deg`, `remove`, `duplicate dx dy dz`, `add "models/x.glb" x y z` (after the first selected object's line), `id [n]` (the selection's line gets `id n`, one line only; without `n` each selected line without an id gets the next free one), `undo`, `redo`, `print`; `#` comments. Line numbers are those of the file as it is at that command. One `edits: …` line per command; exit 1 at the first failing one. Format: the header comment of `src/mapedit.c` |
| `--bake-surface <material> <out-prefix>` | Hidden window: bake a material of the map (`--map` or the default) into a tile, `<out-prefix>.png` (RGBA: the shader's `ALBEDO` in RGB, its `ROUGHNESS` in A, straight from R3D's 8-bit linear G-buffer). It renders one square quad of the material with an orthographic camera: `--bake-face top` (default; the quad lies at y = 0, tile u = x, v = z) or `side` (z = 0, u = x, v = y). `--bake-size <px>` (default 2048) and `--bake-texels <per m>` (default 24, the material's own pixel grid) give the tile's period, size / texels metres; texel (i, j), row j from the top of the file, is the pixel centred at ((i + 0.5) / texels, (j + 0.5) / texels). `--bake-levels <n>` (1–6, default 1) bakes levels 0..n-1 of the pixel grid into one atlas, (1.5 × size) × size: level 0 at (0, 0), level L ≥ 1 at x = size, y = size − size / 2^(L−1), each size / 2^L px. The material's shader is compiled with `#define MS_BAKE 1`, `MS_BAKE_FACE` (0 top, 1 side), `MS_BAKE_LEVEL`, `MS_BAKE_PERIOD` (metres) and `MS_BAKE_TEXELS` in front of its source, so it can force its grid level, make its noise periodic over the tile and write data instead of a colour (other shaders are loaded unchanged). The map reads such a tile with `data_sampler` ([map-format.md](map-format.md)). Every level reloads the map |
| `--inspect-model <file>` | No window: bounds, origin, meshes/triangles, material slots, textures, up axis / unit hints, glTF extensions, warnings, a placement line. Refuses files whose references leave their folder. |
| `--shot <file>` | Render 30 frames into a **hidden window** (never mapped), save, exit (1600×900 unless `--shot-size`). No hot reload or request polling. |
| `--shadow <size> <range>` | Override `env shadow.size` / `sun.range` (default `8192 100`; clamped 256–16384 px, 10–2000 m) |
| `--shadow-camera` / `--shadow-always` | Sun shadow from R3D's camera-fitted map re-rendered when needed / every frame, instead of the static shadow tiles ([glossary.md](glossary.md)). A/B and debugging. |
| `--fog-cache <voxel> <sub>` | **Trade-off, off.** Bake the fog's sun visibility into an 8-bit 3D texture per load (voxel 0.25–8 m, sub³ samples, sub 1–4; patch 8); region from `env fogCache.region` or the content bounds ([glossary.md](glossary.md), *fog cache*); outside it the march uses the shadow map |
| `--no-prepass` | No depth pre-pass (patch 6); same image, slower. A/B tests. |
| `--bench` | Per viewpoint: 60 warmup + 600 measured frames spinning 360°; table + Markdown report in `benchmark/<stamp>_shadow-<size>-<range>.md` (completed runs only). The report's Setup table names the machine (GPU, driver, CPU, system, resolution): frame times are only comparable between runs on the same machine |
| `--headless` | With `--bench`: hidden window, offscreen 1920×1006, `glFinish` per frame. **Required for automated runs.** ~10% slower than visible; compare headless only with headless. |
| `--bench-frames <n>` | Frames per viewpoint (≥ 10, default 600; ~150 is enough for a quick comparison). |
| `--bench-still` | Camera doesn't spin (suffix `_still`) |
| `--bench-window <samples>` | With `--bench --headless`: offscreen stand-in for the window (`windowemu.c`; 0–16 samples, 4 = `--window-msaa`, 0 = the default single-sampled window) with HUD and overlay, resolved like the swap |
| `--no-profile` | No per-pass GPU table (the profiler is on only under `--bench`) |
| `--note "text"` | Note at the top of the bench report |
| `--no-frame-reuse` / `--frame-reuse` | Turn the interactive frame reuse off ([architecture.md](architecture.md), "Frame reuse") / on where it is off by default (`--shot`, to prove a reused frame equals a fresh one; `--bench-still`, suffix `_reuse`) |

## Designer test flags

For testing the designer without a visible window:

| Flag | Meaning |
|---|---|
| `--pick` | With `--shot`: print the centre hit (object, group, map line + text, point, normal, distance, bounds; models: transform) |
| `--pick-at x y` | With `--shot`: the same for pixel (x, y), top-left origin (repeatable) |
| `--select <line>` | Select every object of this map line (repeatable) |
| `--region x y w h` | Attach a screen region (repeatable) |
| `--volume x0 y0 z0 x1 y1 z1` | Attach a volume: the axis-aligned box between two opposite corners in map coordinates, any order (repeatable). `--shot-hud` draws it, `--submit` writes it into the bundle |
| `--submit "text"` | With `--shot`: run the real submit path (bundle + queue + **real coordinator terminal** if none runs). For tests, point `MONOLITH_REQUESTS_DIR` at a scratch folder and `MONOLITH_TERMINAL` at `tools/stub-terminal.sh`, so no real Claude Code session starts |
| `--submit-wait <s>` / `--close-submitted` | Keep polling the request (state changes are logged) / then close it like Ctrl+K n (coordinator mode: cancel) and report when the terminal process is gone |
| `--type "text"` | Open the prompt box with this text |
| `--classify "text"` | No window: print the model tier the launcher would pick (context: map, `--select`, `--region`, `--volume`, hit; `--classify-kind edit\|import\|new-map`) |
| `--shot-hud` | Draw HUD, crosshair, overlay, regions, volumes, prompt box into the shot |
| `--edit-mode` | With `--shot --shot-hud`: open the edit mode (**M**), so the shot shows its HUD |
| `--edit-id <digits>` | With `--edit-mode` and `--select`: open its id panel (**Ctrl+I**) for the selection with these digits typed (`""`: empty), so the shot shows it |
| `--shot-size w h` | Shot size (64–8192) |
| `--reload-test <path>` | With `--shot`: load `<path>` through the hot-reload path after 10 frames; must equal a plain `--shot --map <path>` (proves every static cache is rebuilt) |

In `--submit`, `--type` and `--classify` text a literal `\n` becomes a new line.

## Environment variables

How the game starts Claude Code:

| Variable | Meaning |
|---|---|
| `MONOLITH_LAUNCH` | `coordinator` (default: one coordinator session takes the requests from a queue, [request-queue.md](request-queue.md)) or `per-request` (a terminal + session per request). Without `flock` (util-linux): per-request |
| `MONOLITH_COORDINATOR_MODEL` | The coordinator's model (default `sonnet`); each request's subagent runs on the request's tier model |
| `MONOLITH_TERMINAL` | Terminal program + optional args (`"kitty --single-instance"`); over `TERMINAL`; else the first of `alacritty`, `kitty`, `foot`, `wezterm`, `xterm` in `PATH` |
| `MONOLITH_CLAUDE_ARGS` | Extra Claude Code flags (whitespace-split), after the two permission flags and `--model <name>`, on the coordinator's (per-request: each request's) command line; a `--model` here replaces the chosen one (the coordinator's and every request's) |
| `MONOLITH_CLAUDE_SAFE` | Non-empty and not `0`: drop the permission flags (they are also dropped when the working folder has no `CLAUDE.md`) |
| `MONOLITH_ALLOW_WINDOW` | Non-empty and not `0`: open the game window even when `CLAUDECODE` or `AI_AGENT` is set. In such an AI agent's shell the game otherwise refuses, with exit status 1, every run that would show its window; the safe flags listed at the top of this page still work |
| `MONOLITH_MODEL` | One model for every request (turns the tier choice off) |
| `MONOLITH_MODEL_FAST` / `_DEFAULT` / `_HEAVY` | Model per tier (`haiku`, `sonnet`, `opus`). `ClassifyRequest` ("Model choice" in `launcher.c`): **heavy** for imports / new maps, an empty map or no `MAP.md`, asset, look/engine or large design words, > 280 bytes, 3+ sentences or > 2 regions and volumes together; **fast** for one short sentence (≤ 120 bytes) with a local edit word, no creation word and a target (crosshair hit, a selection of ≤ 100 objects, or exactly one region or volume); **default** otherwise. Written to `request.md` / `request.json`. |
| `@fast` / `@default` / `@heavy` (also `@haiku` / `@sonnet` / `@opus`) | Prompt prefix picking the tier, stripped from the bundle (`model.override: true`); `MONOLITH_MODEL` and a `--model` in the args still win |
| `MONOLITH_WORKSPACE` | i3 / sway workspace for the terminal (default `10`, empty = stay). `i3-msg` / `swaymsg` on `[title="^monolith-claude: coordinator$"]` (per-request: `^monolith-claude: <slug> <HHMMSS>$`), retried by a detached helper every 0.1 s for 10 s; also sets the `_monolith_coordinator` (`_monolith_<id>`) mark. An i3 `for_window [title="^monolith-claude: "] …` rule avoids the brief flash. |
| `MONOLITH_CLOSE_ON_EXIT` | Non-empty and not `0`: when the game quits, close the coordinator if none of this run's requests is open (per-request: finished requests' terminals) |
| `MONOLITH_CLAUDE` | The claude program (tests: `tools/stub-coordinator.sh`) |
| `MONOLITH_REQUESTS_DIR` | Requests folder instead of `<repo>/requests` (tests only: bundles, queue and lock in a scratch folder, away from a real coordinator) |
| `MONOLITH_CLOSE_METHOD` | `wm` or `signal` only (tests; default both, WM first) |

Other variables: `MONOLITH_CAPTURE` / `MONOLITH_CAPTURE_FRAME` (capture shim output folder / frame, default 3), `MONOLITH_DOWNLOADS` (`fetch_asset.py`, default `~/.cache/monolith/downloads`), `POLY_PIZZA_API_KEY` (or `~/.config/monolith/poly-pizza.key`; never printed), `MONOLITH_STUB_LOG` / `MONOLITH_STUB_OPENED` / `MONOLITH_STUB_RUN` (stub terminal: argv log, one line per "window", run the command; `tools/stub-terminal.sh`), `MONOLITH_STUB_WORK` (stub coordinator's seconds per request), `__GL_SYNC_TO_VBLANK=0` / `vblank_mode=0` (driver vsync off for benches), `__GL_YIELD=USLEEP` (NVIDIA: `glFinish` sleeps instead of spinning; the profiling tool `build/linux-profile/profile` sets it, so the wait doesn't look like work in a CPU profile).


## Settings file

Settings (file keys): Display — `render_scale` 0.25–1 (the panel offers 0.5–1), `upscale`, `anti_aliasing` none/fxaa/smaa, `window_msaa`, exposure / brightness / contrast / saturation trims. Shadows — `sun_shadow`, `shadow_size` (map's / 2048–16384), `shadow_range` (map's / 60–300 m), `shadow_softness`, `shadow_mode` tiles/camera/always. Below the map's `shadow.size`, the sun's depth and slope biases are scaled by the texel ratio (`SunShadowMap()`) so low suns get no acne. A range below the fog length fades the shafts early. AO — `ao` full/ssao/lite/baked/off, baked voxel/intensity, SSAO / SSIL parameters. Fog — volumetric fog, `fog_step` (× the map's step), length, fog cache voxel/samples, distance fog. Post — bloom, bloom intensity, SSR, SSGI (off / map's / on). Performance — frame reuse, fps cap (off/monitor/30–240), vsync, depth pre-pass. 

**The settings file** is `key = value`, one per line, `#` starts a comment. `preset = …` sets every scene setting and later keys override it; `base = …` picks the preset the panel compares with; booleans accept `on` / `off`, `true` / `false`, `yes` / `no`, `1` / `0`; `map` as a value means "use the map's value". Only the interactive game reads the default file; **`--shot`, `--bench`, `--dump-*`, `--export` and `--classify` never do** unless `--settings` names one, so verification stays deterministic. Command-line flags override for one run and are never saved (panel changes are written field by field). Unknown keys and bad values are logged and ignored.
