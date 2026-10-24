---
name: map-editing
description: Edit a monolith map package (maps/<name>/ — the .map file, MAP.md, the package's shaders, models and textures), by hand or as part of a request. Coordinates, scale and walkability; editing rules (smallest change, splitting repeat lines, groups, coplanar faces, resting on supports); verification with --shot, --dump-boxes, --dump-scene-diff and the map hash; hot reload vs. rebuild. Use for any change to a .map file or a map package.
---

# Map editing

Rules and verification for every change to a map package. Read the map's `MAP.md` and `docs/knowledge/map-format.md` first. For a request bundle, the `map-request` skill is the workflow around this (locating the target, `result.md`). When you edit **by hand** (no bundle), skip what needs one (the hash from `request.md`, the reproduction command): take the before/after images from the map's viewpoints (`--view n`) or an `--eye` / `--look` near the change, into a scratch folder, and still diff the dumps. Flags: `docs/knowledge/cli.md`.

## 1. Coordinates and scale

- Units are **metres**. **+Y is up**. The world is right-handed. Boxes are axis-aligned; only models can be rotated.
- **The player** (engine facts, the same in every map): an AABB of radius 0.3 m and height 1.75 m (~1.8 m), with the **eye at 1.62 m** above the feet. It starts at the map's `spawn` (feet).
- **Walkability.** Walkways need **≥ 2.2 m headroom**. Stairs can rise **≤ 0.45 m per step** (`STEP_HEIGHT`: higher steps can't be climbed). A jump reaches about 0.9 m. Openings must be at least 0.6 m wide for the player to fit through; for comfort make them ≥ 1 m.
- **The map's scale** (ground size, reference sizes of buildings, trees, props, the route that must stay walkable) is in its `MAP.md`, *Scale and walkability*. Examples: in a monumental concrete map pillars are 20 m and buildings 50–260 m; in a forest map pines are 11–25 m and a tent 2.7 m wide.
- **Visibility.** Sun shadows reach `sun.range` from the eye (100 m unless the map sets it), fading out over the last quarter. How far you can see depends on the map's distance fog (`env fog.*`); `MAP.md` says what that means for its views.
- **Feet vs. eye.** This is a common source of off-by-1.62 m errors.
  - `--pos x y z` takes the **feet** (eye − 1.62). The **P** key in the game prints `--pos` (feet), and `spawn` takes the feet.
  - `--eye x y z` takes the **eye**. `request.md`, `request.json`, the map's `viewpoint` lines and the reproduction command all use the eye.
  - Without `--view`, `--pos`/`--eye` place the walking player (the shot renders that player's camera). With `--view n`, `--eye` moves the spectator camera instead and `--look` overrides the viewpoint's angles.

## 2. Editing rules

- **Follow the map's `MAP.md`**: its materials, palette, scale, budgets and rules for new content.
- **Change the smallest set of lines** that does what was asked. Don't reformat, renumber, reorder or "tidy" other lines, and don't move unrelated geometry.
- **Changing one copy of a `repeat` line:** split the line so that the changed copy gets its own line. Example: a selected pillar is copy 3 of an 8-copy z-row. Write the row as copies 0–2, then the changed one, then copies 4–7, with adjusted start values and counts. `request.md` gives the copy indices (hit and selection: `3 of 32 (i = 0, j = 3)`, `i` = the first, outer `repeat`). In a 2-D grid (`repeat 4 … repeat 8 …`) keep the object order: the outer rows before the changed one as one line, then that row split into its copies before / the changed one / after, then the remaining rows. Check that the other copies keep exactly their old coordinates (`--dump-boxes` / `--dump-scene-diff`, §3).
- **New content goes into a well-named group.** Either extend the matching existing group block or add a new `group "area: what"` in the same style (`group "hypostyle hall: north door"`, `group "clearing: woodpile"`, `group "market: stalls west"`). Keep or add short `#` comments that say what the lines are. Names are how the developer and later requests refer to things; add new landmarks to `MAP.md`.
- **Boxes stay axis-aligned.** Diagonal or curved shapes are steps of boxes, or a model if the brief allows models.
- **Solid steps in another direction** (stairs only climb toward −Z): repeat a box whose bottom stays buried in the floor below. For example, `repeat 10 0.75 0.3 0 box 0 -3 0 0.75 0.3 4` makes 10 steps climbing toward +X, each 0.3 m higher.
- **Faces:** exactly coplanar overlapping faces are harmless only when both shade identically (same material, and the material is computed from world position, like a concrete shader computed from world position). With textured or different materials they flicker: don't overlap them. Faces that are *nearly* coplanar (a few mm to cm apart) always flicker, so either align them exactly or separate them clearly.
- **Models and textures** (finding, licensing, downloading, normalising, recording and restyling them): the `assets-and-licences` skill (`.claude/skills/assets-and-licences/SKILL.md`). Never place a file whose licence isn't recorded.
- **Lights and environment** are content too: a request for "warmer light", "denser fog" or "a lamp here" is an `env` or `light` edit. Change the look of a whole map only when the request asks for it.
- **Watch what a `repeat` grid multiplies to**: the loader accepts up to 1 000 000 objects, but every object costs drawing, picking and collision time (tens of thousands are fine, a million is slow).
- **Real scale.** Think in metres (§1, the brief's reference sizes). Keep walkways ≥ 2.2 m high and steps ≤ 0.45 m. New boxes and models rest on something (the ground, a terrace, a roof) unless the request wants them floating.
- **Engine changes are allowed when a request needs them**: a new primitive or statement in `runtime/monolith_scene.c`, a new environment key, shader work in the package's `shaders/`. They must follow the `engine-code` skill (and `lighting-fog-shadows` with `docs/knowledge/r3d-pitfalls.md` for lighting, fog and shadows; R3D changes recorded in `submodules/r3d/patches.md`), keep the look of every map unless the request is about the look (every map in `maps/` stays pixel-identical), build with **zero warnings on both targets** (`./build.sh` and `./build.sh --win`) and pass the leak check (§3, step 7). Under the coordinator, code changes need its go-ahead first (`map-request` §2a). If you add a primitive or statement, document it in the header comment of `runtime/monolith_scene.c` and in `docs/knowledge/map-format.md` §2.

## 3. Verification

Run every command from the repo root. The binary is `./build/linux/monolith`, and the reproduction command in `request.md` uses its absolute path. **Never open a visible window on the developer's screen.** Use only `--shot` (it renders in a hidden window that is never mapped), `--dump-boxes` / `--inspect-model` (no window) and `--dump-scene` (a hidden window when the map has models). Below, `<map>` is the map file (or package) named in `request.md`, e.g. `maps/<name>/<name>.map`.

1. **Check the map hash** before editing. Compare with the hash in `request.md` ("at submit: FNV-1a 64 hash …"), using the command printed there:
   ```sh
   python3 -c "import sys,functools as f;print('%016x'%f.reduce(lambda h,b:((h^b)*0x100000001b3)&(2**64-1),open(sys.argv[1],'rb').read(),0xcbf29ce484222325))" <map>
   ```
   If the hash differs, the file changed after the submit, maybe because of another request. Re-read it and re-locate your targets by coordinates and line text, because the line numbers and object indices in `request.md` may be stale.
2. **Before image.** Run the bundle's reproduction command, with the output put into the bundle folder:
   ```sh
   ./build/linux/monolith --shot requests/<id>/before.png --shot-size W H --eye X Y Z --look YAW PITCH --map <map>
   ```
   (`--shot` with a relative path writes into the current directory, so always give the bundle path.) `--shot` renders 30 frames and exits; the bundle's command passes `--shot-size W H` with the size of `view.png`, so the image matches `view.png` (pixel-identical in tests; without `--shot-size` shots are 1600 × 900). Add `--pick` to print what the screen centre hits (box or model, group, material, map line, point, normal, distance).
   Also save the scene list: `./build/linux/monolith --dump-boxes requests/<id>/boxes-before.txt --map <map>` (one box per line as hex floats), or `--dump-scene-diff requests/<id>/scene-before.txt` when the edit touches models, materials or lights (`--dump-scene` without `-diff` also writes object indices and map lines, so every row after an inserted line changes; `-diff` refers to groups, materials and model files by name, so its diff shows only the changed objects).
3. **Edit the map.** For multi-line edits, prefer one write: the game reloads on every save, so intermediate states flash on the developer's screen. A broken save keeps the old map loaded and shows a red `file:line: message` in the HUD.
4. **Check that it parses.** A broken map never crashes the game, but it also doesn't load, so the developer keeps seeing the old one. To check the file, load it explicitly:
   ```sh
   ./build/linux/monolith --dump-boxes requests/<id>/boxes-after.txt --map <map>   # no window
   ```
   A parse error prints `file:line: message` and exits with status 1 (`--dump-boxes` opens no window but also imports every model file on the CPU, so a missing, truncated or unreadable model fails here too; `--export` fails the same way). `--map` is also how to try a draft copy before writing it over the real map. `diff` the two dumps (`--dump-boxes` or `--dump-scene-diff`, the same kind before and after) to confirm that only the intended objects changed (with inserted lines, the unchanged ones move as a block).
5. **After image** from the same camera: `--shot requests/<id>/after.png` with the same `--eye`/`--look`. Then **look at both images** (Read them). Add extra views where the reproduction view can't show the change, e.g. from the other side, from above, or walking height `--eye x <ground + 1.62> z`. The map's viewpoints are `--view 1..n`. Use `--off <digits>` to isolate effects (1 volumetric fog, 2 distance fog, 3 SSAO, 4 SSIL, 5 bloom, 6 sun).
6. **Check:**
   - the request is visibly done, at the right spot, at a sensible scale, **in the style of `MAP.md`** (materials, palette, budgets);
   - no **floating** objects: each new box or model rests on or is attached to something, unless intended. Compare y0 with the top of the support (`--pick` on it);
   - no **z-fighting** or flickering near-coplanar faces;
   - walkways not blocked: **headroom ≥ 2.2 m**, stairs **rise ≤ 0.45 m** per step, openings ≥ 0.6 m (better ≥ 1 m), and the routes `MAP.md` names still exist;
   - no unrelated objects moved (the dump diff), and nothing multiplied out of scale (the loader's sanity cap is 1 000 000 objects);
   - for models: scale next to the player, support, collision, shadows and lit materials, licence recorded, budgets (the `assets-and-licences` skill, "Verification additions for models");
   - `MAP.md` updated if you added a landmark, group or material.
7. **Code changes only (C, shaders, build): leak check** (the `leak-check` skill). Run `tools/leakcheck.sh` (it builds the LeakSanitizer binaries and runs the main code paths in hidden windows, about 1.5 minutes; its test bundles go to its own requests folder, so it doesn't touch the queue). It must end with `leakcheck: OK: zero leaks of ours`; otherwise fix the leak it prints before writing `result.md`. Map-only edits (including the package's own shaders) skip this step; check a changed shader with `glslangValidator` instead (`docs/knowledge/r3d-pitfalls.md` §4).
8. **Write `requests/<id>/result.md`** (the `map-request` skill, §4). For a hand edit, report the same to the developer: lines before → after, images, what is left open.

**Hot reload vs. rebuild.** The game polls the map package (map file, shaders, textures, models) about twice a second and reloads it when a file's content changes. The HUD then shows `map reloaded (N boxes)`. **A package-only edit (map, `MAP.md`, the package's shaders, models, textures) needs no rebuild and no restart.** A **code edit** (C or R3D) needs `./build.sh` (and `./build.sh --win` for the warning check), and **the developer restarts the game**. Say so in `result.md` (or to the developer). A changed `shadow.size` also needs a restart (or the F2 panel's shadow map size).
