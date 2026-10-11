# models/rock_face_01.glb

Provenance of `models/rock_face_01.glb` in this package (written by `tools/record_asset.py`).

| Field | Value |
|---|---|
| Name | Rock Face 01 |
| Package path | `models/rock_face_01.glb` |
| Source | Poly Haven |
| Asset page | https://polyhaven.com/a/rock_face_01 |
| Download URL(s) | https://dl.polyhaven.org/file/ph-assets/Models/gltf/1k/rock_face_01/rock_face_01_1k.gltf<br>https://dl.polyhaven.org/file/ph-assets/Models/gltf/8k/rock_face_01/rock_face_01.bin<br>https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/rock_face_01/rock_face_01_arm_1k.jpg<br>https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/rock_face_01/rock_face_01_diff_1k.jpg<br>https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/rock_face_01/rock_face_01_nor_gl_1k.jpg |
| Author(s) | Dario Barresi |
| Licence | CC0 1.0 (https://polyhaven.com/license) |
| Attribution | not required (CC0); credit given in ASSETS.md anyway |
| Downloaded | 2026-10-11 (by tools/fetch_asset.py) |
| AI-generated | no |
| Original sha256 | `96c332c01d173a2431115f3bac29be101fba341e5500e2a08e384f4edd17299e` rock_face_01_1k.gltf (md5 verified against the API) |
| Original sha256 | `873eac0319333e92bc96ddeedf8ebf69dc3c68ff1dfe32d8c14ddc165cdb20ac` rock_face_01.bin (md5 verified against the API) |
| Original sha256 | `59128c79d207a50a69a38002986135ade3b19642d26158cec141eca41578c9ca` textures/rock_face_01_arm_1k.jpg (md5 verified against the API) |
| Original sha256 | `41bd26ed2db1332b1ebb4b1326fd8af4d21f0f0eb57b8ea385033115c722cfdb` textures/rock_face_01_diff_1k.jpg (md5 verified against the API) |
| Original sha256 | `9a9d85b9a01f4e24fc9ceec8baf96c019f552bca69591ff544c7a725e28b1d97` textures/rock_face_01_nor_gl_1k.jpg (md5 verified against the API) |
| Triangles (source) | 20174 |
| Format (this file) | glTF 2.0 binary (.glb) |
| Generator (this file) | Khronos glTF Blender I/O v5.2.40 |
| ExtensionsUsed (this file) | (none) |
| Triangles (this file) | 10000 |
| Size (this file) | 4.9535 x 3.5651 x 3.8259 (x y z) |
| Up axis (this file) | +Y (glTF is Y-up by definition) |
| Units (this file) | metres (glTF is metres by definition; the size above is the truth) |
| Material slots | [0] "rock_face_01": 1 mesh, 10000 triangles, shading pbr |
| Texture maps | albedo map: embedded *0 (jpg, 51008 bytes), 512 x 512; normal map: embedded *2 (jpg, 56485 bytes), 512 x 512; roughness map: embedded *1 (jpg, 36905 bytes), 512 x 512; metalness map: embedded *1 (jpg, 36905 bytes), 512 x 512 |

## Changes to the file

- tools/normalize_model.py (Blender 5.2.2 LTS): applied node transforms (identity transforms in the output)
- tools/normalize_model.py (Blender 5.2.2 LTS): decimated (collapse, ratio 0.4957): 20174 -> 10000 triangles
- tools/normalize_model.py (Blender 5.2.2 LTS): moved the origin to the bottom centre of the bounds (shift -0.3432 0.0358 1.5531, Y-up)
- tools/normalize_model.py (Blender 5.2.2 LTS): image 'rock_face_01_diff': 1024x1024 -> 512x512
- tools/normalize_model.py (Blender 5.2.2 LTS): image 'rock_face_01_nor_gl': 1024x1024 -> 512x512
- tools/normalize_model.py (Blender 5.2.2 LTS): image 'rock_face_01_rough': 1024x1024 -> 512x512
- converted glTF + .bin + 3 JPGs into one GLB (Blender glTF exporter; textures embedded)

## Notes

- Restyled in pine_forest.map with material "rock scan" (palette albedo, inherit normal): only the relief of the scan is kept.
