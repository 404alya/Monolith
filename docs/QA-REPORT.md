# QA report: monolith v2 (2026-10-11)

A full QA pass after the v2 work (round 2, `docs/knowledge/history.md`), run while the developer was away.

**Process:**
1. Three independent testers worked on copies of the maps and never edited the repo.
2. Two fixers fixed every confirmed finding, each with a regression test.
3. The lead merged both fixers' work and re-verified it.

**Result:** 31 findings were fixed. 4 items remain open, each needing a decision from the developer (§4).

## 1. Final state of `main` (65e2680)

| Check | Result |
|---|---|
| `./build.sh`, `./build.sh --win`, `examples/build.sh`, `examples/build.sh --win` | 0 warnings |
| Default map: box dump vs. the pre-v2 baseline | byte-identical (290 boxes) |
| Default map: 27 verification shots (7 viewpoints; yaw sweeps at eye height y 1.6/60/100; the steep view; the walk start) vs. the pre-v2 baseline | 0 pixels differ |
| `tools/leakcheck.sh` (LeakSanitizer) | zero leaks of ours. The only suppressed leak is 366 B inside NVIDIA's driver. |
| `tools/leakcheck.sh --asan` (AddressSanitizer + UBSan) | zero leaks, no memory errors |
| Frame time before vs. after the QA fixes (3 interleaved headless pairs, 1920×1006, spinning camera) | 3.074 → 3.091 ms (+0.5%, within noise) |
| Unknown or malformed command-line flags | refused with exit 1 before any window opens |

## 2. What the testers did

| Tester | Scope | Volume |
|---|---|---|
| QA1, request flows | Played a Claude request session by hand, following `AI-MAP-TOOL.md` literally: split a `repeat` line, a screen-region request, model edits in pine_forest, a new "neon street" map built from `--new-map`, an import of `examples/04_import_source` (by hand and through the capture shim), and how format-1 bundles read | 6 scenarios |
| QA2, robustness | ASan/UBSan builds: map parser fuzzing, hot-reload chains, flag/settings fuzzing, broken model files, export/import edge cases, examples | 15,540 fuzzed maps, 3,000 mutated model files, 1,040 flag combinations, 200 settings files, 25 reload chains |
| QA3, visuals | Turn invariance (reprojected yaw sweeps), every quality preset on all maps, model placement, fog-cache seams, shadow-tile switching, frame reuse, the settings renderer restart, Windows under wine | about 600 shots |

What held up from the start:
- The parser: no sanitizer report in 15,540 fuzzed maps.
- Hot reload: every chain gave the same image as a direct shot.
- Turn invariance: the largest drift was 0.56/255.
- Frame reuse and the renderer restart: pixel-identical.
- Model placement: within 3 mm of the support.
- Shadow tiles: no pops.
- Fog cache: no seams.
- Wine: identical shots for the default map.
- The end-to-end request workflow, including splitting a 2-D `repeat` grid.

## 3. Findings and fixes

Severity: **S** = security, **C** = crash/hang, **W** = wrong result, **D** = docs/data, **c** = cosmetic.

| # | Sev | Finding | Fix |
|---|---|---|---|
| 1 | C | `--import`, `--export` and in-game F9 crashed. The cause was a stack overflow in raylib's recursive folder scan: a fixed 4096-byte buffer, overflowed by a long symlink path. | New `src/dirscan.c`: heap paths, symlinked folders not followed, capped at 1M entries / 64 levels (reported) |
| 2 | C | Symlink loops made the same scans grow exponentially, freezing the game on F9 | Same scanner |
| 3 | S | A model file could make the loader read files outside the package: glTF URIs ≥ 1024 characters or written with JSON `\u` escapes, or an OBJ `mtllib`. A line of `/etc/passwd` could reach the log. | References are decoded the way assimp reads them and validated, and over-long ones are rejected (`MS_CheckModelReferences`). `--inspect-model` refuses such files before reading them. |
| 4 | W | `--inspect-model` reported success for files with no meshes and for files referencing outside data | Warns "no meshes" and refuses outside references |
| 5 | C | Undefined behaviour (float → int) from huge or non-finite positions, e.g. `spawn 1e30 …` or `--eye nan …` | Positions are limited to ±1e6, the CLI rejects non-finite values, and the tile and volume keys are clamped |
| 6 | C | `--fx bloom.levels=16384` hung for over 60 s; `--fx` had no range checks | `--fx` now uses the map's env ranges |
| 7 | C | `--region nan …` was undefined behaviour | Rejected |
| 8 | C | The command line had no unknown-flag error. A flag that swallowed the next `--shot` (`--pos 1 --shot x.png`) started the visible game. | Strict parser: unknown flags, missing or unparsable values, and a value that is itself a flag all exit 1 before any window opens |
| 9 | W | Material lookup was quadratic: 80,000 materials took 18.2 s to parse | Hash table, now 0.12 s |
| 10 | D | The import inventory was silently capped at 8,192 files | True counts, with "at least" when the scan stopped |
| 11 | W | Settings-file lines over 511 characters were split into a second line | Lines of any length |
| 12 | c | `MONOLITH_CAPTURE_FRAME` past the last frame was silent; a UTF-8 BOM made a map fail; wording was inconsistent | Warning at exit; BOM skipped; messages unified |
| 13 | W | The capture shim left the last box material active over the model lines, so every captured model was recoloured | Writes `use "default"` before the models; spawn placed on the ground; `CAPTURE.md` names the changed fields |
| 14 | W | `--bloom off` used an exposure compensation (×1.19) tuned for the default map; pine_forest got 7–20% darker | New `env bloom.offExposure` (default 1.19, so the default map is unchanged); pine_forest uses 1.3, within 1% of bloom-on |
| 15 | W | Shadow stripes (acne) on pine_forest box tops with the 4096 shadow map (the low and medium presets) | Sun depth and slope biases scale with the texel size below the map's `shadow.size` |
| 16 | W | pine_forest rendered 25–36% pure black, against its own `MAP.md` | Map only: contrast 1.08 → 1.0, exposure, ambient and fog retuned. Pure-black pixels are now 0; the darkest tenth of the frame is about RGB (25, 25, 37). R3D is unchanged (see §4.1). |
| 17 | c | pine_forest showed a hard horizon line in view 5 | The ground below the horizon blends into the horizon colour |
| 18 | c | pine_forest's one-sided rock scan showed floating shards from behind | `cull none` on its override material |
| 19 | D | `--dump-scene` diffs were unreadable after an inserted line (1,504 changed lines for one new lantern) | New `--dump-scene-diff`: no indices or line numbers, so the same edit diffs as 6 lines |
| 20 | D | A region over a gap listed what is seen through the gap (140 m behind it), not the walls framing it | Regions list edge objects (just outside the border) and shares summed per map line; docs updated |
| 21 | D | The import inventory left out licence, README and other doc files, and headers included from outside the project | Listed |
| 22 | D | Import docs: no source for R3D defaults; `record_asset.py` needed a provenance JSON; the `inherit all` caveat was missing; example 04's README was stale | Docs fixed; `record_asset.py --source-note/--licence/--author` |
| 23 | D | `request.md`: the Nearby table said "boxes" for models; no copy index within a `repeat` line; fully overridden models were labelled "(own; …)" | "objects"; copy index such as `3 of 32 (i = 0, j = 3)`; "(all slots overridden: …)" |
| 24 | D | `--new-map` / `--import` could only write into the repo's `maps/`; picking worked only at the screen centre; no night example | Target paths accepted; `--pick-at x y`; a dusk/night `env` example in `AI-MAP-TOOL.md` |

Items 25–31 were small message and doc fixes that came with the items above. They are listed in the fixers' notes, along with the regression tests (each fails before and passes after).

## 4. Open: decisions for the developer

1. **R3D's contrast step clips dark values to black.** `color.contrast` is applied in linear space after tonemapping (`submodules/r3d/shaders/post/output.frag`, `Adjustments()`). Everything below 0.5·(1−1/c) becomes pure black.
   - The default map (contrast 1.05) has a few thousand such pixels at ultra, and 26–29k at the wall feet of view 1 with the baked-AO presets.
   - Fixing it in R3D (contrast around mid-grey in a perceptual space) would change the default map's look, so it was not done. pine_forest was fixed in its map instead.
2. **R3D texture path overflow.** `r3d_importer_texture.c` joins the model folder and the texture path into a 256-byte stack buffer without a bounds check. The map loader now rejects joins that would reach 256 bytes, so maps can't trigger it. Fixing R3D itself would be patch 13.
3. **An assimp leak** of about 1.4 KB when a glTF buffer file is shorter than declared. It is in assimp's own glTF code; not patched.
4. **Not checked yet:** FBX/DAE external references; symlinks inside a package that point outside it.

Decisions from the v2 summary still apply:
- the i3 `for_window` rule for `monolith-claude:` terminals;
- the Quaternius licence policy;
- the medium/low shadow-map size and the low preset's render scale;
- regenerating the Poly Pizza key.

## 5. Things only a person at the keyboard can test

- Snip mode: C, drag, Shift+drag, Esc / RMB / C to cancel, Enter.
- Closing request terminals: K, Shift+K, Ctrl+K then a number (press it twice for a running request).
- The F2 settings panel: presets, single rows, Reset, a restart to check the settings stick.
- Walking through pine_forest: trunk collision, passing under crowns, the ridge steps.
