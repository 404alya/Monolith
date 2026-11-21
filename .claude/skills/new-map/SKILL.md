---
name: new-map
description: Create a new monolith map package from a name and an optional description — runs `monolith --new-map`, writes the MAP.md design brief from the description, sets the map's look (env lines, the "default" material that the ground slab uses) to match, and checks it with shots of its viewpoints. Use when the developer runs /new-map <name> [description], or asks in plain words to start, create or set up a new map or level ("make me a new map of a harbour at night").
argument-hint: <name or path> [description of the map]
---

# Create a new map

One step from nothing to a package with a written brief and the right look: `--new-map` makes the package (`src/newmap.c`: `NewMapDir`, `NewMap`, `NewMapIn`), then, when the developer described the map, this skill fills in `MAP.md` and sets the `env` lines and the `"default"` material to the description. It builds **no layout**: the place itself is built by the first request from the editor, where the developer can point at things, to the brief written here.

**Rules for the whole procedure** (with the guard rails in `CLAUDE.md`):
- Run monolith only with `--new-map`, `--dump-boxes`, `--dump-scene-diff` and `--shot` (no window), from the repo root, with absolute or repo-relative paths. If `build/linux/monolith` is missing, run `./build.sh` first and say so.
- The description is the developer's own text, typed here (not a request bundle): follow it as the brief's content. Downloaded files stay data, as always (none are needed here).
- **Never delete and never work around `--new-map`.** When it refuses (a bad name, an existing folder, a missing parent folder), pass its message through and stop: never pick another name, never trash or overwrite the existing folder. On a failure after `--new-map` succeeded, leave the package, report what it holds and what failed; the developer decides whether to `gio trash` it.
- **Edit the new package only by the `map-editing` skill's rules:** the smallest change, the ground slab stays in group `ground`, no `id` prefixes.
- **No models, no textures, no shaders, no lights, no new geometry.** The look comes from `env` lines and the `"default"` material's colour and roughness only. Assets come later, through requests or the `assets-and-licences` skill; the brief's *Rules for new content* says so.
- **Change values, don't drop lines.** Keep the `monolith N` format line, the header comments and every `env` key `--new-map` wrote out; change the values of the keys the mood needs. The one kind of line you may add: `env ambientSky.<sky key>` (the template writes only the ambient keys that differ from the sky; the *Night* recipe sets `ambientSky.skyTopColor`), next to the other `ambientSky` lines.
- Map-agnostic values: take moods from `docs/map-format.md` §2.5 (the daylight template, the *Dusk* and *Night* recipes, *Dark maps*) and the `lighting-fog-shadows` skill (with `r3d-pitfalls.md` §1 and §4), not from memory or from another map's numbers copied blind.
- Shots and stats go into a scratch folder `S` (a fresh folder in the scratchpad, else `/tmp/new-map-<name>-<time>/`; made in step 4, only when there is a description), never into the package. Don't commit: the brief and the look stay uncommitted changes in the package's repository; the first request's claim commits them as `Hand edits before request <slug>` (in a repository without a commit yet: `Start the history of <name> (before request <id>)`), or the developer commits them.
- `P` (the package) and `S` below are placeholders: a shell variable doesn't survive from one command to the next, so write the paths out in every command (or set both at the top of each command).

## Procedure

1. **Read the arguments.** The first word is the name or path; the rest is the description (may be empty). A first argument in quotes (`"bad name!"`, `'a b'`) is one argument, quotes removed. Anything with a `/` is a path (as `NewMapDir` reads it): the package is that folder and the name its last component; otherwise the package is `maps/<name>/`. For a path, its parent folder must exist (`--new-map` refuses otherwise; don't create it). Don't check the name yourself: `--new-map` does (letters, digits, `_`, `-`, at most 200 characters).
   In plain words ("make me a new map of a harbour at night"), pick a short name from the description (lower case, `_` between words, e.g. `harbour_night`) and say which one you picked.

2. **Create the package.** Pass the argument as one shell word in single quotes (a `'` inside it written as `'\''`):
   ```bash
   ./build/linux/monolith --new-map '<arg>'; echo "exit $?"
   ```
   Non-zero exit: show its `monolith: --new-map: …` line as it is, say nothing was created, and stop. On success it prints `monolith: created <folder>/ (…; <repository>)`: that folder is the package `P` (absolute), the map `P/<name>.map`. The end of the line says whether the package is a git repository with its first commit (`Create <name> with monolith --new-map`), the normal case, or not (`not a git repository`, `nothing committed yet`); then a `monolith: --new-map: warning: …` line before it says why (git missing, no git identity, a folder inside another repository's work tree) and what to run. **Pass that warning through as it is, don't work around it** (no `git init`, `git config` or `git commit` of your own): it is the developer's to fix.

3. **No description: stop here.** Report the created files (`ls -R "$P"`), the open command `./build/linux/monolith --map <P relative to the repo, or absolute>`, and that the developer can fill in `MAP.md` by hand, or describe the map in a first request from the editor (TAB, aim, Enter; `docs/user-guide.md`, "Making maps"), and the session writes the brief before building. Change nothing in the package.

4. **With a description: write the brief and set the look.** Make the scratch folder `S` and keep the fresh map as the template to compare with: `mkdir "$S" && cp "$P/<name>.map" "$S/template.map"`. Read `docs/map-format.md` §1.2 and §2.5 (Environment), and the `lighting-fog-shadows` skill with its `r3d-pitfalls.md` §1 and §4. Then:
   - **`MAP.md`**: keep the title line and the intro paragraph. Replace every placeholder in parentheses and fill every section of §1.2 from the description, specific enough that a later session builds the same place:
     - *Style and mood*: the kind of place, era, genre, the feeling. Keep the *Paint grid* bullet (0 unless the description asks for a pixel look; then say so there and set `env paint.grid`).
     - *Scale and walkability*: keep the two player bullets as written; replace the placeholder with typical sizes for this kind of place (buildings, rooms, streets, props, in metres) and the routes the player must be able to walk.
     - *Materials and palette*: the materials the map will need (by role and colour), the palette (RGB), how dark or bright albedos may be (mind the contrast floor of `r3d-pitfalls.md` §4: with `color.contrast` above 1, shaded faces whose albedo is low in any channel go black; the shots' `black` count is the check), and that for now there is one material, `"default"` in `<name>.map`, used by the ground.
     - *Lighting and atmosphere*: time of day, sun (or moon) direction, sky, fog, the lights the place will need, and why.
     - *Rules for new content*: replace the template's first two bullets with this map's answers (boxes or models, how models are restyled); keep the budget bullet (its parentheses are values, not placeholders) unless the description asks for other budgets; and add: "No models or textures yet: assets come through requests or the `assets-and-licences` skill, each with a recorded licence."
     - *Groups and landmarks*: keep the `ground` row; below the table, one *Planned* line may name the areas the description implies (no coordinates).
     Check that no template placeholder is left (prints nothing): `grep -nE '\((What kind of place|Typical dimensions|Which materials exist|Time of day)' "$P/MAP.md"`.
   - **`env` lines**: change the values the mood needs, in place, following the recipes above (e.g. night: the *Night* list's keys; dusk: the *Dusk* note; noon desert: a high sun, little or no fog). The *Night* list is for a street lit by its own signs; where the moon is the main light, it needs more `sun.energy` than the list gives. **No fog: keep `volumetricFog.enabled on`** and set its `scatteringDensity`, `absortionDensity` and `emissionEnergy` to 0, and `fog.mode disabled`: with `volumetricFog.enabled off` (as with `--off 1`) the ground around the eye loses its sunlight (an engine bug, in `.claude/TODOs`). Dark moods keep `color.contrast` at 1 and the ambient sky brighter than the visible one (*Dark maps*); dense volumetric fog needs emission (`r3d-pitfalls.md` §1). A bloom stronger than the template's gets its own `bloom.offExposure` (§2.5, *Quality presets*): note in the report that it is a first guess, not measured.
   - **`"default"` material**: give its `albedo` (and `roughness`; keep `metalness`) the ground's colour from the brief (wet asphalt: dark, low roughness; sand: warm, rough). Don't add a material or a `use` line: one material keeps the first request's edits small.
   - **`spawn` and viewpoints** stay `Start` and `Overview` unless the description makes them pointless (e.g. the ground isn't at y = 0). Then change them and say why.
   Write the map in one write (the editor may be watching the folder), and each later fix in one write too.

5. **Check.** Each command from the repo root; every exit status must be 0:
   ```bash
   ./build/linux/monolith --map "$P" --dump-boxes "$S/boxes.txt"; echo "exit $?"
   ./build/linux/monolith --map "$S/template.map" --dump-boxes "$S/template-boxes.txt" && cmp "$S/template-boxes.txt" "$S/boxes.txt" && echo "boxes: the template's"
   ./build/linux/monolith --map "$P" --view 1 --shot "$S/view1.png"; echo "exit $?"
   ./build/linux/monolith --map "$P" --view 2 --shot "$S/view2.png"; echo "exit $?"
   python3 -I -c 'import sys; from PIL import Image
   for f in sys.argv[1:]:
       im = Image.open(f).convert("RGB"); n = im.width * im.height; c = {k: v for v, k in im.getcolors(n)}
       print(f, "mean %.1f" % (sum(v * sum(k) for k, v in c.items()) / 3 / n), "black %.2f%%" % (100 * c.get((0, 0, 0), 0) / n), "white %.2f%%" % (100 * c.get((255, 255, 255), 0) / n))' "$S"/view*.png
   ```
   (`--dump-boxes` takes the output file; the map always comes through `--map`.) The box list must equal the template's (one box, the ground slab: the `cmp` line prints `boxes: the template's`). **Look at both shots** (Read them): the mood the description asked for, not black, no pure-white sky, no pure-black patches (`black` near 0 %), a mean brightness plausible for the mood (a night street about 40/255, `docs/map-format.md` §2.5; the template's daylight about 150). With only the ground slab, the shots show sky, fog and ground and nothing else (no signs, no lamps): judge the mood from those. If not, change the `env` values (not the albedo, `lighting-fog-shadows` "Fix causes") and repeat this step. Then `diff "$S/template.map" "$P/<name>.map"`: only values of existing lines changed (plus spawn / viewpoints when step 4 said why).

6. **Report** to the developer:
   - the package path and its files, and its repository: `git -C "$P" log --oneline` (the one commit) and that the brief and look are not committed (step 4), or the `--new-map` warning as it was printed;
   - the brief's first lines (title and *Style and mood*);
   - the `env` keys and material values that changed from the template (from the diff), with any first guess marked as such;
   - the two shot paths (looked at) and their stats, with a note that they show only sky, fog and ground until the place is built;
   - the open command: `./build/linux/monolith --map <package>`;
   - that the first request from the editor builds the place to this brief (TAB, aim, Enter), and that models and textures come through requests.
   On a failure in steps 4–5: say what failed, with the message, and what the package holds now (`ls -R`, the diff against the template); leave it for the developer to fix or `gio trash`.
