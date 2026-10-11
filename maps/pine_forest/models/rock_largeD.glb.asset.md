# models/rock_largeD.glb

Provenance of `models/rock_largeD.glb` in this package (written by `tools/record_asset.py`).

| Field | Value |
|---|---|
| Name | kenney_nature-kit |
| Package path | `models/rock_largeD.glb` |
| Source | Kenney |
| Asset page | https://kenney.nl/assets/nature-kit |
| Download URL(s) | https://kenney.nl/media/pages/assets/nature-kit/37ac38a37b-1677698939/kenney_nature-kit.zip |
| Author(s) | Kenney (www.kenney.nl) |
| Licence | CC0 1.0 (https://creativecommons.org/publicdomain/zero/1.0/) |
| Attribution | not required (CC0); credit given in ASSETS.md anyway |
| Downloaded | 2026-10-11 (by tools/fetch_asset.py) |
| AI-generated | no |
| Archive sha256 | `fa7974a0d342bfe63c38664ba9f8ec1a4aab8ea25f099bdc56870e33588c4d9d` kenney_nature-kit.zip |
| Original sha256 | `e4fefd8d8d7deab42be61bd222bcd088330012f0025789146b81f25a46095ffd` extracted/Models/GLTF format/rock_largeD.glb |
| Format (this file) | glTF 2.0 binary (.glb) |
| Generator (this file) | UniGLTF-1.27 |
| ExtensionsUsed (this file) | KHR_materials_unlit |
| Triangles (this file) | 80 |
| Size (this file) | 1.0688 x 0.5683 x 1.0303 (x y z) |
| Up axis (this file) | +Y (glTF is Y-up by definition) |
| Units (this file) | metres (glTF is metres by definition; the size above is the truth) |
| Material slots | [0] "dirt": 1 mesh, 63 triangles, shading pbr; [1] "grass": 1 mesh, 9 triangles, shading pbr; [2] "_defaultMat": 1 mesh, 8 triangles, shading pbr |

## Changes to the file

- none: the file is byte-identical to the original

## --inspect-model warnings

- KHR_materials_unlit is listed in extensionsUsed but no material uses it: the materials are lit PBR
- material [0] "dirt" is fully metallic (metallic 1, no metalness map): unless it is metal it renders as dark tinted metal (common in kits exported with the glTF default); override it with a lit map material (metalness 0)
- material [1] "grass" is fully metallic (metallic 1, no metalness map): unless it is metal it renders as dark tinted metal (common in kits exported with the glTF default); override it with a lit map material (metalness 0)
- material [2] "_defaultMat" is fully metallic (metallic 1, no metalness map): unless it is metal it renders as dark tinted metal (common in kits exported with the glTF default); override it with a lit map material (metalness 0)

## Notes

- Kenney's licence file is models/kenney-License.txt. Restyled in pine_forest.map with lit palette materials per slot (material.N), which also replaces the kit's metallic 1.
