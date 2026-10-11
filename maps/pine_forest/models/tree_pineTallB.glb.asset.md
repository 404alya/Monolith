# models/tree_pineTallB.glb

Provenance of `models/tree_pineTallB.glb` in this package (written by `tools/record_asset.py`).

| Field | Value |
|---|---|
| Name | kenney_nature-kit |
| Package path | `models/tree_pineTallB.glb` |
| Source | Kenney |
| Asset page | https://kenney.nl/assets/nature-kit |
| Download URL(s) | https://kenney.nl/media/pages/assets/nature-kit/37ac38a37b-1677698939/kenney_nature-kit.zip |
| Author(s) | Kenney (www.kenney.nl) |
| Licence | CC0 1.0 (https://creativecommons.org/publicdomain/zero/1.0/) |
| Attribution | not required (CC0); credit given in ASSETS.md anyway |
| Downloaded | 2026-10-11 (by tools/fetch_asset.py) |
| AI-generated | no |
| Archive sha256 | `fa7974a0d342bfe63c38664ba9f8ec1a4aab8ea25f099bdc56870e33588c4d9d` kenney_nature-kit.zip |
| Original sha256 | `49b54146351e1e009e97ee592f997a90a16b10c5467baf734d352df1cddec5d1` extracted/Models/GLTF format/tree_pineTallB.glb |
| Format (this file) | glTF 2.0 binary (.glb) |
| Generator (this file) | UniGLTF-1.27 |
| ExtensionsUsed (this file) | KHR_materials_unlit |
| Triangles (this file) | 78 |
| Size (this file) | 0.3863 x 1.9346 x 0.3920 (x y z) |
| Up axis (this file) | +Y (glTF is Y-up by definition) |
| Units (this file) | metres (glTF is metres by definition; the size above is the truth) |
| Material slots | [0] "leafsDark": 1 mesh, 68 triangles, shading pbr; [1] "woodBarkDark": 1 mesh, 10 triangles, shading pbr |

## Changes to the file

- none: the file is byte-identical to the original

## --inspect-model warnings

- KHR_materials_unlit is listed in extensionsUsed but no material uses it: the materials are lit PBR
- material [0] "leafsDark" is fully metallic (metallic 1, no metalness map): unless it is metal it renders as dark tinted metal (common in kits exported with the glTF default); override it with a lit map material (metalness 0)
- material [1] "woodBarkDark" is fully metallic (metallic 1, no metalness map): unless it is metal it renders as dark tinted metal (common in kits exported with the glTF default); override it with a lit map material (metalness 0)

## Notes

- Kenney's licence file is models/kenney-License.txt. Restyled in pine_forest.map with lit palette materials per slot (material.N), which also replaces the kit's metallic 1.
