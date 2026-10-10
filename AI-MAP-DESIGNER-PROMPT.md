# Task: turn monolith into an AI-driven map designer, and make it fast by caching static work

You are working in `~/Projects/monolith`, a real-time 3D scene written in plain C11 on raylib 5.5 + R3D 0.10.x (deferred PBR). Today it is one hand-built brutalist concrete level that you can walk through or fly around in a spectator camera. The developer wants two things:

1. **An AI map designer.** In spectator mode the developer looks at part of the map, optionally selects objects, types what should change ("make this wall twice as tall", "add a row of pillars here", "this corner is too dark, open a slit in the roof above it"), and submits it. The game launches **Claude Code in a new terminal window**, and that session gets the request with everything it needs to find the right spot: camera position, yaw/pitch, fov, the point under the crosshair, the selected objects and a screenshot. Claude Code then edits the map, and the running game shows the result without a restart.
2. **Much better frame rate**, mostly by rendering static things once and caching them instead of recomputing them every frame. The scene geometry and the sun never move, but the renderer currently treats everything as dynamic.
3. **View-independent effects.** Some effects, most visibly the **fog**, change when the camera only *rotates* in place. For a static scene seen from a fixed point, turning the head must never change how a given world spot looks. Today it does. This must be fixed, and fixed before Phase 3 caches bake the error in.

Read everything in "Before you start" and "How to work: multiple agents" first. Then do the work in the phases below. Each phase must build, run and be verified before its results are merged.

---

## Before you start (required reading)

- **`PROMPT-INSTRUCTIONS.md`**: the full technical reference for this codebase: build, controls, code architecture, the scene map, and the R3D pitfalls already found and fixed (§7). Read all of it, especially §7 (volumetric fog black curtain, shadow pancaking with `GL_DEPTH_CLAMP`, the shadow-map reuse logic and its cost breakdown, strict-GLSL issues on AMD/Intel, ambient fill from the ambient-only sky) and §8 (how to verify).
- **`src/main.c`**: the whole program (~1100 lines, `// #region` blocks: World, Player, Spectator camera, Atmosphere, Sun shadow reuse, Benchmark report, Main). The header comment is the source of truth for controls and flags.
- **`submodules/r3d/patches.md`**: the vendored R3D copy and the 3 local patches (vfog member rename, skipping wasted fog PCF taps, the shadow min/max tile map). **Every change to `submodules/r3d` must be recorded there** (symptom, cause, files, diff, verification), in the same style.
- **`benchmark/`**: past `--bench` reports. Current baseline (headless, 1920×1006, RTX 4050 Laptop, 2026-10-10): **~5.87 ms average, ~170 FPS**, 1% lows ~110–135 FPS.
- R3D's source (`submodules/r3d/src/r3d_draw.c` for the frame's pass order, `src/modules/r3d_light.c`, `shaders/`, `include/r3d/*.h`, `docs/`) is the reference for how the renderer behaves. Read it instead of guessing.

### Hard rules (from the developer, non-negotiable)

- **Never take over the developer's screen.** For automated runs use `--shot` (hidden window) and `--bench --headless`, keep runs short, and never switch the developer's i3 workspace. Visible bench windows on a workspace that isn't shown stall forever.
- **Benchmarks are noisy.** Headless runs swing ~10% between GPU clock states. Verify any claimed gain or regression with **interleaved A/B pairs** (A, B, A, B, …, at least 3 pairs, same flags, two binaries side by side, e.g. `build/linux/monolith` copied to `monolith-A`). Screen with `--bench-frames 150`; report numbers only from full runs. Write `--note "what changed"` on every run.
- **Your Bash tool runs zsh.** An unquoted `$VAR` does not word-split, so pass env overrides as arrays or inline (`__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith ...`).
- **Both targets must build with zero warnings:** `./build.sh` and `./build.sh --win`.
- **Don't change how the scene looks unless it's an explicit, approved trade-off.** Every optimisation is either pixel-identical (prove it with `--shot` comparisons from fixed views and yaw sweeps, with a pixel-diff count) or listed as a trade-off with before/after images for the developer to decide on. Don't fix shadow problems by turning off shadow casting.
- **Keep the project's character:** plain C, no textures or models authored by hand, axis-aligned boxes, the procedural concrete shader, the existing code style (4-space indent, `static` PascalCase functions, `// #region`, comments that say *why*). Textures **generated at runtime** as caches are fine.
- Don't touch `~/Projects/r3d` or any other project in `~/Projects`.
- Don't commit unless asked. Never commit `build/`, screenshots or request bundles.

---

## How to work: multiple agents

The developer explicitly wants **several agents working on these tasks**, not one agent doing everything in sequence. This counts as opt-in to multi-agent orchestration: use subagents (the Agent tool), or a workflow if one is available. You are the **lead agent**. You own the plan and the merge order, and you integrate and verify the final result. Hand the actual work to agents with clear, self-contained briefs. Each brief names the files the agent may touch, the rules above, what "done" means, and what to report back: a diff summary, verification evidence such as pixel-diff counts and bench reports, and open questions.

**Suggested split** (adjust it once you have read the code):

| Stream | Agent(s) | Can start | Touches |
|---|---|---|---|
| 0. Module split | lead, alone, first | immediately | `src/main.c` → e.g. `world.c/.h`, `map.c`, `spectator.c`, `designer.c`, `launcher.c`, `render.c`; `build.sh` keeps compiling `src/*.c`. A pure move, pixel-identical. It exists so parallel streams don't all edit one 1100-line file. |
| A. Map format + loader + hot reload (Phase 1) | 1 agent | after 0 | `map.c`, `maps/`, `InitDataPaths`, `build.sh` |
| B. Fog / effect rotation investigation (Phase 2.5) | 1 research agent (read-only R3D shaders + `--shot` sweeps), then 1 fix agent | immediately | `submodules/r3d/shaders/…`, `patches.md` |
| C. GPU profiler + Release R3D build + baseline profile (Phase 3, step 0–1) | 1 agent | immediately | `r3d_draw.c` (timer queries), `build.sh`, bench report |
| D. Selection, crosshair picking, prompt input UI (2.1–2.2) | 1 agent | after A's box back-references exist | `designer.c`, HUD |
| E. Request bundle + terminal/Claude launcher + result polling (2.3–2.5) | 1 agent | after A; in parallel with D, against an agreed struct for "selection + camera state" | `launcher.c`, `.gitignore` |
| F. Docs: `AI-MAP-TOOL.md`, `CLAUDE.md`, `PROMPT-INSTRUCTIONS.md`, `main.c` header (2.6) | 1 agent | drafts early, finalised last | docs only |
| G. Caching work (Phase 3, steps 2–7) | 1 agent per independent cache (frame reuse, static shadow, fog cache, baked AO) | after B's fix and C's profile | mostly R3D, each recorded in `patches.md` |
| V. Verifier | 1 independent agent per merged stream | after each stream | read-only + `--shot` / bench; it re-checks the claims instead of trusting them |

Coordination rules:

- **Isolation:** give every code-writing agent its own **git worktree** (Agent tool `isolation: "worktree"`). Merge one stream at a time, rebuild, and re-verify after each merge. Streams that touch the same R3D shader (B's fog fix, patches 2/3, G's fog cache all live in `vfog_radiance.frag` / `light.glsl`) must be **sequential**, or handled by one agent.
- **Build cost in worktrees:** `submodules/r3d/build-linux` is untracked, so a fresh worktree rebuilds assimp (several minutes). Agents that don't change R3D can point at the main checkout's build with `R3D_DIR=~/Projects/monolith/submodules/r3d ./build.sh`. Agents that change R3D build their own copy. Note that `RESOURCES_PATH`/`BENCHMARK_PATH` get baked to the worktree's paths.
- **One GPU, one benchmark at a time.** Benchmarks must never run concurrently with each other or with heavy shot sweeps. A parallel GPU load ruins the numbers, and they are noisy enough already. Serialise all `--bench` runs through the lead, or through a single "bench runner" agent that takes A/B binary pairs and runs the interleaved pairs. `--shot` runs for correctness checks may run in parallel.
- **Agree on interfaces before fanning out:** the box record with its back-reference (line, primitive, group), the map version counter that caches key on, and the "designer state" struct the launcher serialises. The lead writes these headers first.
- Research agents (e.g. the fog investigation, or reading R3D's pass order for the profiler) stay read-only and report their findings with file:line evidence. Don't let several agents re-read the same large files for the same question.
- Every agent follows the hard rules above, especially: never a visible window, headless benches only, no commits.
- Keep the fan-out proportionate: roughly 6–10 agents over the whole job. Don't spawn agents for trivial edits.

---

## Phase 1: the map becomes data (prerequisite for everything else)

The level is currently hard-coded in `BuildWorld()`. An AI that edits C and then needs a rebuild and a restart is a poor loop, and caches need a clear "the map changed" event. So:

1. **Define a plain-text map format** and move the current level into `maps/monolith.map` (create `maps/`; ship it next to the binary like `resources/`, through `InitDataPaths()` and `build.sh`). Requirements:
   - One primitive per line, so a line number identifies an object and diffs stay readable. Keep the existing builders as primitives: `box x0 y0 z0 x1 y1 z1`, `block cx y0 cz sx sy sz`, `stairs cx baseY startZ width steps rise run`, `waffle x0 x1 y0 y1 z depth bayW floorH`. Add more only if a request actually needs them.
   - `# comments` and **named groups** (e.g. `group "hypostyle hall: pillars"`) that every following primitive belongs to until the next `group`. Names are how a human or an AI refers to parts of the map, so keep the current section names (canyon, hall, stairs, terrace, tower, megastructures, bridge).
   - Loops (pillar grids, roof beams, megastructure fins) can either be expanded into explicit lines or expressed as a small `repeat` / `grid` construct. Choose whichever keeps the file short, editable by an AI without mistakes, and easy to parse in C. Document the choice.
   - Errors (unknown keyword, wrong argument count, too many boxes) are reported with file:line and **never crash the game**. On a bad reload, keep the previous map and show the error in the HUD.
2. Every generated box keeps a back-reference: **source line, primitive index and group**. Selection and request bundles (Phase 2) depend on it.
3. **Hot reload:** poll the file's mtime about twice a second (`GetFileModTime`). On change, re-parse, rebuild the instance buffer, invalidate every static cache (Phase 3), and show "map reloaded (N boxes)" in the HUD. Collision uses the new boxes immediately. If the player ends up inside a box, leave them there; spectator mode doesn't collide.
4. **Verify:** the loaded map must produce **exactly the same box list** as the old `BuildWorld()` (compare them once, e.g. with a temporary `--dump-boxes` flag or a one-off check), and `--shot` images of all 7 viewpoints must be pixel-identical to the pre-change build.
5. Add `--map <file>` to load another map. Document the format in the new instruction file (Phase 2, step 6).

## Phase 2: in-game prompting → Claude Code

### 2.1 Selection in spectator mode
- Show a **crosshair** in spectator mode. Each frame, cast a ray from the camera centre against the world boxes (axis-aligned, so a slab test per box is enough; there are only a few hundred). The hit box gets a subtle highlight. Draw the overlay with raylib after `R3D_End()` in a `BeginMode3D` using the same camera, wireframe or a translucent face tint, no depth artefacts. Check the R3D docs/source for how to depth-test against R3D's depth buffer, or accept an always-on-top outline.
- **Left click**: select the box under the crosshair (clears the old selection). **Shift + left click**: add or remove it. **Ctrl + left click**: select the box's whole group. **Right click**: clear the selection. Also keep the **hit point and face normal** of the crosshair; many requests are about a spot ("put a door here") rather than an object.
- Selected boxes get a distinct, clearly visible highlight. The HUD shows how many are selected and their group names.

### 2.2 Prompt input
- **Enter** (in spectator mode) opens a text box at the bottom of the screen. It needs multi-line text, backspace, cursor keys, paste (Ctrl+V via `GetClipboardText`), Shift+Enter for a newline, Enter to submit and Esc to cancel. While the box is open, **all game keys are suppressed** (WASD, 1–6, P, G, TAB, F, arrows, Esc-quit), the camera freezes and the mouse cursor is shown. Remember that `WindowShouldClose()` triggers on Esc by default: use `SetExitKey(KEY_NULL)` and handle quitting yourself.
- Use raylib's font, word-wrapped, with a visible character limit (a few thousand chars).

### 2.3 Request bundle
On submit, before launching anything, write a self-contained folder `requests/<YYYY-MM-DD_HHMMSS>_<slug>/` (repo root on the dev machine, next to the binary elsewhere; add `requests/` to `.gitignore`):

- `view.png`: the current frame **without HUD or overlays**, rendered offscreen at the window size the way `--shot` does it (R3D into a `RenderTexture`, `LoadImageFromTexture`, flip vertically). Don't use `TakeScreenshot`.
- `view-marked.png`: the same frame with the selection highlight and the crosshair hit point drawn on it.
- `request.md`: what the AI reads first. It contains:
  - the user's prompt, verbatim;
  - camera: eye position, yaw/pitch in degrees (with the project's convention: yaw 0 looks toward +Z, yaw 180 toward −Z; `LookDir = (cos p·sin y, sin p, cos p·cos y)`), forward vector, fovy 70°, aspect, resolution;
  - the crosshair hit: point, face normal, box, group, map-file line, and distance from the camera;
  - every selected box: min/max, centre, size, group, and the source line **with the line text quoted** from the map file;
  - nearby context: the groups within ~30 m of the hit point, so the AI knows what surrounds it;
  - **exact reproduction commands**: a `--shot` command that renders this exact view, so the AI can take before/after images itself. `--pos` takes the *feet* (eye − 1.62) today, which is error-prone, so add an `--eye x y z` flag and use it here;
  - the map file path and the map's content hash/mtime at submit time, so the AI can tell if the file changed in between;
  - an instruction to read `CLAUDE.md` / `AI-MAP-TOOL.md` first and to write `result.md` into this folder when done.
- `request.json`: the same data, machine-readable.

### 2.4 Launching Claude Code
- Launch **a new terminal window** running Claude Code in the repo root, with a short initial prompt that points to the bundle, e.g. `claude "Map edit request: read requests/<id>/request.md and carry it out."`. Don't paste the user's text into a shell command line: build an argv array and use `fork` + `execvp` (or `posix_spawnp`) so no quoting or injection is possible. Don't wait for the child; reap it so no zombies are left.
- Terminal selection: env `MONOLITH_TERMINAL` if set, else `$TERMINAL`, else the first of `alacritty`, `kitty`, `foot`, `wezterm`, `xterm` found in `PATH`. The developer's machine uses i3 with `alacritty`. Each terminal has its own "run this command" syntax (`alacritty -e`, `kitty`, `foot`, `wezterm start --`, `xterm -e`), so use a small table. Set the working directory to the repo root (alacritty `--working-directory`, or `chdir` in the child before exec). Give the window a recognisable title (e.g. `monolith-claude: <slug>`) so i3 rules can place it.
- Extra Claude Code flags come from env `MONOLITH_CLAUDE_ARGS` (e.g. a permission mode or a model). Default: none. Don't bypass permissions by default.
- Windows build: open `cmd /c start "" cmd /k claude ...` via `CreateProcess`, or at least compile cleanly and show "launching is Linux-only for now" in the HUD. Pick one and document it.
- If no terminal or no `claude` binary is found, keep the bundle and show the error in the HUD, with the bundle path so the developer can run it by hand.

### 2.5 Closing the loop
- The game keeps a small list of open requests. It polls each bundle folder for `result.md`, then shows "✓ <slug> done" (with the first line of the result) in the HUD. Combined with map hot reload, the developer sees the change appear in place.
- Optional, if cheap: a key (e.g. **R**) that flies the spectator back to the camera of the last finished request, so before/after can be compared from the same spot.
- Several requests can run at once. When two touch the same map, the second Claude session must re-read the file before editing (the map-hash check in `request.md` covers this).

### 2.6 The new instruction files for the AI side
Claude Code automatically reads `CLAUDE.md` in the repo root, not `PROMPT-INSTRUCTIONS.md`. So:

- **Create `AI-MAP-TOOL.md`**: the product-level instructions. It should say clearly that this project is an **AI-assisted map designer**: monolith is the editor/viewer, `maps/*.map` is the level, and Claude Code sessions launched from inside the game carry out map edits. Its content:
  - the request workflow end to end, and what each file in a bundle means;
  - **how to locate what the user means**: the crosshair hit and selection come first. Next, read "here", "this", "left" and "behind" relative to the camera (camera right = `(−cos yaw, 0, sin yaw)`, forward as above). Then use group names and nearby context. Look at `view-marked.png`. If it is genuinely ambiguous, ask in the terminal rather than guess.
  - coordinate conventions and scale: metres, Y up, ground top at y = 0, player 1.8 m, eye 1.62, pillars 20 m, buildings 50–260 m; feet vs eye;
  - the map format reference, with an example of each primitive and of groups;
  - **editing rules**: change the smallest set of lines that satisfies the request; keep groups and comments meaningful (new geometry goes into a well-named group); stay axis-aligned; keep the box count under `MAX_BOXES`; don't move unrelated geometry; edits that need engine changes (new primitive types, lighting, atmosphere in `SetupEnvironment()`, shader work) are allowed but must follow `PROMPT-INSTRUCTIONS.md` and build cleanly on both targets;
  - **verification**: render the request's reproduction `--shot` before and after the edit (hidden window, never the developer's screen), look at both images, check for z-fighting, floating boxes, blocked walkways (walkways need ≥ 2.2 m headroom; stairs rise ≤ 0.45 m per step, `STEP_HEIGHT`); then write `result.md`: what changed (map lines before → after), the paths of the after-image(s), and anything left open;
  - the game hot-reloads the map, so a map-only edit needs no rebuild. Code edits need `./build.sh`, and the developer restarts the game.
- **Create `CLAUDE.md`**: short. One paragraph on what the project is, then `@AI-MAP-TOOL.md` and `@PROMPT-INSTRUCTIONS.md` imports, plus the hard rules above (screen, benchmarks, zsh, both builds).
- **Update `PROMPT-INSTRUCTIONS.md`** so it is the engine/technical reference of the map tool, not of "a single hand-built scene". Update §1 (what this is), §3 (layout: `maps/`, `requests/`, the new docs), §5 (new controls and flags), §6 (World region: loader and hot reload instead of `BuildWorld()`, selection, prompt UI, launcher), §6.1 (the scene map becomes "the default map"), §9 (common modifications) and §11 (the level-format limitation is gone). Keep every pitfall in §7. Also update the header comment in `main.c`.

### 2.7 Verify Phase 2
- Unit-level: parser round-trip on the default map; ray-pick on a few known views (crosshair at the centre of `--view 2` must hit a known pillar). Add a `--pick` style debug flag or test the pick in a `--shot` run with a printed result.
- End to end, by hand: tell the developer exactly what to try (select a pillar, submit "make this pillar 5 m taller", watch a terminal open, see the pillar grow after Claude edits the map). Don't run a visible game window yourself.

## Phase 3: performance, render static work once

**Start by measuring, not guessing.** Add a `--profile` mode (or extend `--bench`) with **per-pass GPU timings** using `GL_TIME_ELAPSED` queries (double- or triple-buffered so nothing stalls) around R3D's passes in `r3d_draw.c` (shadows, shadow min/max, geometry, SSAO, SSIL, lights, ambient, fog, volumetric fog, bloom, post, FXAA), plus CPU frame time. Wire it in as a recorded, `#ifdef`-guarded R3D patch, and print/write a table per viewpoint into the bench report. Put the breakdown in your summary before optimising anything. Known facts: the scene is only a few hundred boxes in **one instanced draw** (vertex work is tiny); the frame is fill-bound; volumetric fog was the largest single cost (~2.6 ms of it was the sun's march with shadow lookups before patches 2–3); the 8192² shadow map costs ~1.2 ms per re-render; SSAO and SSIL run every frame at full or half resolution.

Then work down this list. **Measure each step separately with interleaved A/B pairs**, and keep it only if it helps. Order by expected gain ÷ risk once you have the profile; roughly:

1. **Optimised R3D build.** `build.sh` builds R3D/raylib/assimp on Linux **without `CMAKE_BUILD_TYPE`, i.e. unoptimised** (kept so far for comparability with old reports). Switch to Release (`-DCMAKE_BUILD_TYPE=Release`; the build dir needs a re-configure) and measure. CPU time per frame matters at these frame rates. Note this break in comparability in `PROMPT-INSTRUCTIONS.md` and in the bench report.
2. **Don't re-render an unchanged frame.** In the designer workflow the camera often stands still (selecting, typing a prompt, waiting for Claude). If the camera, toggles, map version and window size are unchanged since the last frame, skip `R3D_Begin…R3D_End` and only redraw the cached scene image plus the HUD/overlay. Render R3D into a `RenderTexture` and blit it, or find R3D's equivalent. Noise-based effects (fog jitter, SSAO noise) may be temporally animated; check that a frozen frame looks right. `--bench` must keep measuring real rendering: it spins the camera, so this never triggers there, but make sure of it.
3. **Static sun shadow map.** Sun and geometry are static, so the shadow depth content is a pure function of the map. Replace the camera-fitted, re-rendered map with **shadow data rendered once per map load**, then only sampled. Options, in rising complexity: (a) a fixed world-space ortho fit over the playable area (check the texel size: e.g. ~250 m across at 8192² ≈ 3 cm, which beats today's ~3.7 cm), plus a coarse second map for the megastructures; (b) a few static cascades or tiles chosen around the camera position and cached by tile key, re-rendered only when a new tile comes into range. This is an R3D patch (fixed light matrix / an "external" shadow map, and the `L_SampleShadowDir` fades must still make sense). Keep `GL_DEPTH_CLAMP` pancaking (§7.2). The min/max tile map (patch 3) then also gets built only once. Remove or simplify `SunShadowNeedsUpdate()` if it becomes dead code, but keep `--shadow-always`-style A/B switches until the result is proven.
4. **Cached volumetric-fog lighting.** With a static sun and static geometry, the sun-visibility term the fog march samples is static in world space. Bake it once per map load into a **3D texture** (e.g. 0.5 m voxels over the area where fog shafts matter, trilinear; size it to VRAM) and let `vfog_radiance.frag` take one texture fetch per step instead of shadow-map PCF. Optionally also cache the integrated in-scatter. Compare the shafts carefully against the current build (hall shafts, canyon slits, yaw sweeps); aliasing of thin slits is the risk. Also re-test larger `stepSize` and quarter-resolution fog on top of the cache; `PROMPT-INSTRUCTIONS.md` §7.3 has earlier numbers.
5. **Static ambient occlusion / bounce.** SSAO and SSIL recompute static information every frame. Consider baking per-map, world-space AO (and optionally a low-frequency bounce/irradiance volume, e.g. R3D probes in `include/r3d/r3d_probe.h`, or a 3D texture sampled in `concrete.glsl`) once per map load. Then turn the screen-space passes down (smaller radius, lower resolution) or off. This one changes the look by definition: show the developer before/after shots and the measured gain, and let them choose.
6. **Concrete shader cost.** If the profile shows the geometry pass is expensive (procedural fbm per pixel), consider caching: e.g. bake the low-frequency terms per box face into runtime-generated textures/atlas at map load. Only if it measurably matters.
7. **Culling** is likely irrelevant at a few hundred boxes. Only pursue it if the profile says so (e.g. splitting the instance buffer by region so the main and shadow passes can skip off-screen groups).

Every cache is keyed on the **map version** (and the shadow/fog settings it depends on) and rebuilt on hot reload (Phase 1). Show the rebuild time in the HUD and log, and keep it short enough (target < 1–2 s for the default map) that live editing stays pleasant. Also track VRAM: the 8192² shadow array is already ~256 MB.

**Target:** report the before/after frame time per viewpoint and in total from full headless runs. Aim for at least **2× the current ~170 FPS** with an unchanged look, plus an "ideal" still-camera number. If a step changes the image, keep it behind a switch and present it as a trade-off instead of shipping it silently.

### Verify Phase 3
- `./build.sh` and `./build.sh --win`, no warnings; all shaders pass `glslangValidator` (strict front end, catches AMD/Intel-only errors).
- Pixel diffs vs. the pre-change build: all 7 viewpoints, yaw sweeps (0/60/…/300) at eye y 1.6, 60 and 100, the steep downward view `--pos -52.22 61.62 39.53 --look 140.2 -39.6`, and shots after a map hot reload (to prove cache invalidation works).
- Full headless benches, interleaved pairs, with notes; reports stay in `benchmark/`.
- Record every R3D change in `submodules/r3d/patches.md`, and update `PROMPT-INSTRUCTIONS.md` §7.3 (and anything else that changed) with the new numbers and mechanisms, including the "if R3D changes X, update Y" warnings.

---

## Deliverables

- Map file format + loader + hot reload, the default map in `maps/monolith.map`, identical output to the old code.
- Spectator selection, crosshair picking, prompt input UI, request bundles, a terminal + Claude Code launcher, result polling.
- `AI-MAP-TOOL.md`, `CLAUDE.md`, an updated `PROMPT-INSTRUCTIONS.md`, and an updated header comment in `main.c`.
- A per-pass GPU profiler, then the caching work from Phase 3 with measured results, and the trade-offs that need a decision.
- A final summary for the developer: what was built, how to try it (exact keys and steps), the benchmark table before/after, every R3D patch added, and the open decisions (each with before/after screenshots where the look changes).

When something is genuinely the developer's call (a visual trade-off, a big refactor such as splitting `main.c`, which is allowed if it really helps, e.g. `map.c`, `designer.c`, `launcher.c`, with `build.sh` still compiling `src/*.c`), decide with a sensible default, say so, and keep going. Ask only when you are actually blocked.
