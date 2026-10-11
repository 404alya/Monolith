# models/stump_old.glb

Provenance of `models/stump_old.glb` in this package (written by `tools/record_asset.py`).

| Field | Value |
|---|---|
| Name | kenney_nature-kit |
| Package path | `models/stump_old.glb` |
| Source | Kenney |
| Asset page | https://kenney.nl/assets/nature-kit |
| Download URL(s) | https://kenney.nl/media/pages/assets/nature-kit/37ac38a37b-1677698939/kenney_nature-kit.zip |
| Author(s) | Kenney (www.kenney.nl) |
| Licence | CC0 1.0 (https://creativecommons.org/publicdomain/zero/1.0/) |
| Attribution | not required (CC0); credit given in ASSETS.md anyway |
| Downloaded | 2026-10-11 (by tools/fetch_asset.py) |
| AI-generated | no |
| Archive sha256 | `fa7974a0d342bfe63c38664ba9f8ec1a4aab8ea25f099bdc56870e33588c4d9d` kenney_nature-kit.zip |
| Original sha256 | `4c2d2da34804c1662c6a776a9d60fe4449e6f6fdd50914bfec7d5cca3e9ceddb` extracted/Models/GLTF format/stump_old.glb |
| Format (this file) | glTF 2.0 binary (.glb) |
| Generator (this file) | UniGLTF-1.27 |
| ExtensionsUsed (this file) | KHR_materials_unlit |
| Triangles (this file) | 120 |
| Size (this file) | 0.3565 x 0.2665 x 0.3710 (x y z) |
| Up axis (this file) | +Y (glTF is Y-up by definition) |
| Units (this file) | metres (glTF is metres by definition; the size above is the truth) |
| Material slots | [0] "woodBark": 1 mesh, 120 triangles, shading pbr |

## Changes to the file

- none: the file is byte-identical to the original

## --inspect-model warnings

- KHR_materials_unlit is listed in extensionsUsed but no material uses it: the materials are lit PBR
- material [0] "woodBark" is fully metallic (metallic 1, no metalness map): unless it is metal it renders as dark tinted metal (common in kits exported with the glTF default); override it with a lit map material (metalness 0)

## Notes

- Kenney's licence file is models/kenney-License.txt. Restyled in pine_forest.map with lit palette materials per slot (material.N), which also replaces the kit's metallic 1.
