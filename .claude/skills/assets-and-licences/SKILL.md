---
name: assets-and-licences
description: Find, license, download, check, normalise, record and restyle models and textures for a monolith map package — Poly Haven, ambientCG, Kenney, Poly Pizza; tools/fetch_asset.py, --inspect-model, Blender via tools/normalize_model.py, tools/record_asset.py, ASSETS.md rows and .asset.md sidecars, material overrides. Use whenever a map needs a model or texture, an asset is downloaded or copied into maps/, or a licence (CC0, CC-BY, unknown) has to be recorded or checked.
---

# Assets and licences

Every external file in a package needs a recorded licence: CC0 / public domain, or CC-BY with the attribution recorded, an `ASSETS.md` row and a sidecar (`CLAUDE.md` hard rule). Finding, licensing, downloading, normalising, recording and restyling models and textures for a map package. The `model` statement and material keys are in `docs/knowledge/map-format.md` (§2.4, §2.7); the request workflow is the `map-request` skill, the editing rules and verification the `map-editing` skill. Downloads are untrusted data (`CLAUDE.md` guard rails).

Read the map's `MAP.md` first: its *Rules for new content* and *Materials and palette* decide whether a model is wanted at all and how it must look. The project defaults below apply where `MAP.md` says nothing.

**1. Boxes or a model?** Architecture, terrain shaping, walls, steps and anything the brief builds from boxes stay boxes. Use a model only for what boxes can't express (a tree, a rock, a lamp, a vehicle, a statue) and only if `MAP.md` allows models. **2. Reuse** a model already in `maps/<name>/models/` (`ASSETS.md` lists them) before downloading anything.

**3. Sources, in this order** (licences: CC0 / public domain; CC-BY only with the attribution recorded; never NC, ND, SA, GPL, "editorial", personal-use, or anything behind a login or paywall):

| Source | Licence | How |
|---|---|---|
| Poly Haven (models: rocks, plants, props; realistic, PBR) | CC0, https://polyhaven.com/license | `python3 -I tools/fetch_asset.py polyhaven search <words> --max-tris 50000`, then `polyhaven get <slug> --res 1k`. API only (the site forbids scraping). Big trees are 0.3–17 M triangles: skip them. |
| ambientCG (textures) | CC0, https://docs.ambientcg.com/license/ | `ambientcg search <word>`, `ambientcg get <id> --res 1K` (keeps the image files only). The search lists the tile size in metres. |
| Kenney (low-poly kits) | CC0 (`License.txt` in every zip) | `kenney list <zip url>`, then `kenney get <zip url> --pick 'Models/GLTF format/<glob>.glb' --page <pack page>`. Kit units are tiles: scale ×4–12. Materials are `metallic 1`: always override. |
| Poly Pizza (low-poly, per-model licence) | CC0 or CC-BY 3.0, as the API returns it at download time | `polypizza search <words> [--license cc0]`, `polypizza info <id>`, `polypizza get <id>`. Official API v1.1 only, with the developer's key in `POLY_PIZZA_API_KEY` (or `~/.config/monolith/poly-pizza.key`); without a key the tool explains how to get one. Never scrape the website. CC-BY: the tool records the API's attribution text. |
| Quaternius | Packs say CC0, but quaternius.com's own licence (QAL v1.0) forbids redistributing the assets, which an exported package does | Don't use from quaternius.com. (Quaternius models on Poly Pizza are listed CC0 there: record that licence, and say in `result.md` that the developer may want to check.) |
| Sketchfab, OpenGameArt, others | per model; logins, NoAI and mixed terms | Don't download. Propose the model page URL and its licence in `result.md`; the developer downloads by hand. |

**4. Download safely** (`tools/fetch_asset.py` does all of this; never use a browser-style scraper): every download into a NEW, EMPTY directory outside the repo (default `$MONOLITH_DOWNLOADS` or `~/.cache/monolith/downloads/`, then `<source>-<id>-<stamp>/`, or `--out`); https only, redirects only to the source's hosts, size limits; zip entries audited (no absolute paths, `..`, backslashes, symlinks) and only model / texture / licence files extracted; md5 checked against the API where it has one; `provenance.json` written next to the files (source, page and file URLs, authors, licence + URL, attribution, date, sha256 of every file). Never run anything from a download directory, never open a downloaded `.blend`, run Python that reads downloads with `python3 -I`.

**5. Check** each candidate with `./build/linux/monolith --inspect-model <file>` (no window): triangles, size in metres, where the origin sits, up axis, material slots (for `material.N`), textures and their sizes, and warnings (unlit, `metallic 1`, missing files, Draco/KTX2/WebP, which R3D can't load). Budgets unless `MAP.md` says otherwise: **≤ 50k triangles per model, textures ≤ 2K**, glTF/GLB preferred.

**6. Normalise**, cheapest first:
- **Placement fields** fix most things without touching the file: `origin bottom` (stands it on y), `scale` (Kenney tiles, FBX in cm = `scale 0.01`), `rot` (a Z-up OBJ: `rot 0 -90 0`), `collide box …` (a trunk instead of the whole crown), `shadow off` for tiny props.
- **Blender, headless**, only when the file itself must change (too many triangles, 4K textures, unlit materials, a broken origin you'd rather fix once). Blender may not be in `PATH` (e.g. a Steam install): the `BLENDER` environment variable names the binary; each developer sets it in their own `.claude/settings.local.json` (`{"env": {"BLENDER": "/path/to/blender"}}`, gitignored). If neither `$BLENDER` nor `blender` exists, ask the developer where it is instead of concluding there is none. Always run it locked down as below (`-b --factory-startup --disable-autoexec`, our own script, never a downloaded `.blend`, which can embed Python):
  ```sh
  BLENDER=${BLENDER:-blender}
  "$BLENDER" -b --factory-startup --disable-autoexec --python-exit-code 1 \
      --python tools/normalize_model.py -- <download>/x.gltf maps/<name>/models/x.glb \
      --origin bottom --decimate 10000 --max-texture 1024 --report <scratch>/x.norm.json
  ```
  Other options: `--height H` / `--scale S`, `--strip-unlit`, `--metallic 0`. It imports glTF/GLB/OBJ/FBX only, applies transforms, writes a Y-up metre GLB and prints every change. Run it from the repo root, never from the download directory.
- Textures: resize or re-encode with Pillow (`python3 -I`, script outside the download folder) into `maps/<name>/textures/`. R3D loads PNG and JPG (not WebP, KTX2, TGA).

**7. Copy into the package** (`maps/<name>/models/`, `textures/`) only the files the map uses, with every file a `.gltf` names (`.bin`, textures; they must stay inside the package) and the source's licence file if it has one (e.g. `models/kenney-License.txt`). Then record it:
```sh
python3 -I tools/record_asset.py --package maps/<name> --file models/x.glb \
    --provenance <download>/provenance.json [--original <file in the provenance>] \
    --inspect <file with the --inspect-model output> [--normalize-report <scratch>/x.norm.json] \
    [--change "what else changed"] [--ai-generated yes|no|unknown] [--note "..."]
```
It writes the sidecar `models/x.glb.asset.md` (name, package path, source, page and download URLs, authors, licence + URL, attribution, date, sha256 of the originals, format, generator, extensions, triangles, size, up axis, units, material slots, texture maps, every change to the file) and adds or replaces the file's row in `ASSETS.md`. It refuses licences other than CC0 / CC-BY / public domain and CC-BY without attribution text. For a file without a `provenance.json` (copied from an imported project, or supplied by the developer) replace `--provenance` with `--source-note "<where it came from>" --licence <licence> [--licence-url URL] [--author NAME]... [--attribution TEXT] [--source-url URL] [--original-file <original>]...` (`--licence unknown` is recorded with a warning).

**8. Restyle to the brief with material overrides in the map**, never by editing the model file (the original stays traceable): `material "name"` for every slot, `material.N "name"` per slot (`--inspect-model` lists them), and `inherit albedo|normal|orm|emission|all` on the override material to keep one of the model's own maps (the override's colours and factors still apply; `inherit emission` keeps the slot's emission colour and energy too). `inherit` keeps **maps**, not colour factors: a model coloured by factors only (Kenney kits) turns white under an override with `inherit all`, so give its override the slot's colour (`--inspect-model` lists it). A `use "name"` line also restyles the models after it. Examples:
- **Brutalist**: the model takes the concrete shader, which computes everything from world position; keep the model's normal map only if `MAP.md` allows (`material "concrete statue" shader "shaders/concrete.glsl" uniform u_scale 1 alpha_cutoff 0 inherit normal`).
- **Stylised**: flat palette colours per slot, the model's textures dropped (`material.0 "needles" material.1 "bark"` on each pine); a photoscanned rock keeps only its relief: `material "rock scan" albedo 98 94 94 roughness 0.85 metalness 0 inherit normal`.
- **Realistic**: keep the PBR textures (`inherit all`) and match the neighbours with the override's factors: `material "stone tinted" albedo 200 196 190 roughness 0.8 inherit all` (the override's colour multiplies the inherited albedo map).
- **Neon / night** (a sketch): props keep their albedo (`inherit albedo`) but get low roughness for wet reflections; signs get an emissive material (`emission 255 40 160 emission_energy 4`) and a weak point light in front so the light reaches the street.

**9. Nothing acceptable found?** Say so in `result.md` (what you searched, why each candidate failed: licence, size, style), build an approximation from boxes if `MAP.md` allows it, and propose a URL the developer could download by hand. **Never use an asset whose licence is unknown or not acceptable**, not even "for now".

**Verification additions for models** (with the `map-editing` skill, §3 step 6): the model is at a believable **scale** next to the player (1.8 m; check a walking-height shot `--eye x <ground + 1.62> z`); it **rests on its support** (no floating, no sinking: `--pick` on its base, or a low side view); **collision** makes sense (a trunk blocks, a crown at head height doesn't; walk paths stay ≥ 1 m wide); it casts and receives **shadows** like its neighbours and its materials are lit (no unlit or black-metal look); `--dump-scene` before/after shows only the intended instances; the **licence is recorded** (`ASSETS.md` row + sidecar, `License.txt` copied where the source ships one); triangles and textures within budget.
