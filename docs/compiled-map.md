# Compiled maps

A map's source is its text file, `<name>.map` ([map-format.md](map-format.md)). The editor, hot reload, requests and manual edits all work on that text. Programs that only play a map (an export, the game-style examples) load a **compiled map** instead: `<name>.mapb`, a binary file with the scene already parsed. monolith writes it (`monolith --compile <out> --map <path>`, and `--export` does it for you). The scene runtime of a game build has no text parser at all: it reads fixed-size records into the scene's arrays, checks them, and continues with the same load steps as a text load (models, colliders, entities, GPU upload), so the scene renders the same pixels.

Code: `runtime/monolith_scene.c`, region "Compiled maps" (records, `ReadCompiled`, `WriteCompiled`) and `MS_Compile`; `--compile` in `src/main.c`. Tests: `tests/unit/compiled_test.sh`, `tests/unit/export_strip_test.sh`. Sizes for scale: the default map's 2.0 MB of text compiles to 2.7 MB (52,244 boxes once its `repeat` lines are expanded, 291 lights), `pine_forest`'s 133 kB to 108 kB.

## 1. What is stored, what is computed at load

The compiled file holds what the text parser produces, before anything that depends on the GPU, the model files or load options:

| Stored | Computed at load (both inputs, same code) |
|---|---|
| boxes, already expanded from `repeat` / `at` / `stairs` / `waffle`, each with its `MS_Ref` (line, statement, group, material, copy, part); a turned box (`rot`) as written, its corners before the turn | the default viewpoint made from the spawn (when the map has none), the sun light, `shadowSizeOverride` / `shadowRangeOverride` |
| box turns: the rotation of each turned box | a turned box's record (centre, size, axes) and its world AABB, which becomes its entry in the box list (the parser's own code, `TurnedBoxMake`) |
| materials (every key the map can set, shader and texture paths, uniforms, samplers, `wrap`, `inherit`, `instanced`, the map lines of the keys) | model bounds, `origin` shifts, transforms and colliders (they need the model file) |
| groups, model files, model instances (as written: position, rotation, scale, origin, collide boxes in model space, slot materials, shadow) | the collider list and its BVH, the scene bounds |
| lights (as the parser leaves them: direction normalised, defaults filled in, clip box shifted) | entities (ids come from the stored statement keys) |
| the environment: every `env` key with its final value | hidden box faces, the per-cell meshes, every GPU object |
| spawn, viewpoints, the map format version (1 or 2) | |
| one key per statement (its `id`, or the implicit key from its text) | |
| the dependency list (every file the map references, also the files model files name) | |
| the source file's name, size and FNV-1a 64 hash | |

Values are copied bit for bit (floats as their IEEE bits, `-0` stays `-0`), so a scene loaded from the compiled file is identical to the scene loaded from the text: the same box list, the same entity ids, the same image. The derived data in the right column is cheap or depends on the GPU load; storing it is a possible follow-up once the load time has been measured.

## 2. File layout

Little-endian only, every field a fixed-width integer or an IEEE-754 `float`, every record a multiple of 4 bytes, every section starting at a multiple of 8. All offsets are from the start of the file.

**Header** (64 bytes, at offset 0):

| Offset | Type | Field |
|---|---|---|
| 0 | `u8[8]` | magic `89 4D 41 50 42 0D 0A 1A` (`\x89MAPB\r\n\x1a`: not text, and catches a file mangled by newline conversion) |
| 8 | `u32` | binary-format version, `MS_COMPILED_VERSION` (3; see *Versions* below). Separate from the map format version `MS_FORMAT_VERSION`; bumped whenever a record or section changes |
| 12 | `u32` | header size (64) |
| 16 | `u32` | byte-order tag `0x01020304` (a big-endian reader sees `0x04030201` and refuses the file) |
| 20 | `u32` | map format version of the source (1 or 2) |
| 24 | `u32` | section count (17; 16 in version 2) |
| 28 | `u32` | offset of the section table |
| 32 | `u64` | file size in bytes (a truncated file fails here) |
| 40 | `u64` | FNV-1a 64 of the source `.map` bytes |
| 48 | `u64` | size of the source `.map` in bytes |
| 56 | `u32` | the source's file name (string offset, e.g. `forest.map`) |
| 60 | `u32` | flags (0) |

**Section table**: one 16-byte entry per section, `{ u32 id; u32 offset; u32 count; u32 recordSize; }`. Each id appears once. The record size must equal the reader's: a different size is a different format version. An unknown id is an error (an older runtime can't know what it would have meant).

**Sections** (id, record, size in bytes):

| Id | Section | Record | Bytes |
|---|---|---|---|
| 1 | strings | the string table: NUL-terminated UTF-8, `count` bytes, first byte and last byte `0` | 1 |
| 2 | boxes | `f32 min[3], max[3]; i32 line, prim, group, material, copy, part` | 48 |
| 3 | groups | `u32 name` | 4 |
| 4 | materials | names and paths (`name`, `shader`, `maps[4]`: albedo, normal, orm, emission), `i32 line, shaderLine, mapLine[4]`, `u8 albedo[4], emission[4]` (RGBA), `f32 emissionEnergy, normalScale, occlusion, roughness, metalness, specular, uvScale[2], uvOffset[2], alphaCutoff`, `i32 cull, transparency, blend, priority, wrap`, `u32 inherit, unlit, instanced`, `u32 uniformFirst, uniformCount, samplerFirst, samplerCount` | 148 |
| 5 | uniforms | `u32 name, count; f32 v[4]` | 24 |
| 6 | samplers | `u32 name, path; i32 line; u32 data` (1 = `data_sampler`) | 16 |
| 7 | lights | `u32 type; f32 position[3], direction[3]; u8 color[4]; f32 energy, specular, range, falloff, inner, outer, fogEnergy; u32 shadow, clip; f32 clipMin[3], clipMax[3]; i32 ref[6]` | 116 |
| 8 | viewpoints | `u32 name; f32 eye[3], target[3]` | 28 |
| 9 | model files | `u32 path; i32 line` | 8 |
| 10 | models | `i32 file; f32 position[3], rotationDeg[3], scale[3]; u32 origin, collide, castShadow; i32 colliderFirst, colliderCount, slotFirst, slotCount; i32 ref[6]` | 92 |
| 11 | model colliders | `f32 min[3], max[3]` (model space, `collide box`) | 24 |
| 12 | model slots | `i32 slot, material` (`material.N`) | 8 |
| 13 | statements | `u32 key` (by statement index, `MS_Ref.prim`) | 4 |
| 14 | environment | `u32 key` (string: the `env` key, e.g. `fog.density`), `u32 type`, `u32 value[6]` | 32 |
| 15 | spawn | one record: `u32 hasSpawn; f32 feet[3], yawDeg, pitchDeg` | 24 |
| 16 | dependencies | `u32 path` (package-relative) | 4 |
| 17 | box turns (version 3) | `u32 box; f32 rotationDeg[3]` (yaw, pitch, roll as written): one record per turned box, sorted by box, each box at most once | 16 |

**Turned boxes:** for a box with a box turn record, its `boxes` record holds the box **as written** (the two corners before the turn, ordered), not its world AABB; the reader rebuilds the turned-box record (centre, size, axes) and the AABB with the parser's own code, so the bits match a text load. Every other box is stored as its corners, as in version 2.

**Versions:** 1 had no sampler kind (`CmSampler.data`); 2 added it; 3 added the box turns section (17), and a turned box's `boxes` record became the box as written. The reader reads versions 2 and 3 (a version 2 file has 16 sections and no turned boxes); the writer always writes 3. Older or newer files are refused with a message to compile the map again.

`ref[6]` is `MS_Ref` in its field order (line, prim, group, material, copy, part). A string field holds an offset into the string table; `0xFFFFFFFF` means none (a material without a shader). Every name and path is stored once. Unused bytes are zero.

**Environment values** are stored by key name rather than by position, so the key tables can grow or be reordered without breaking compiled files. The value words by type: float and degree keys one `f32` (degree keys hold the radians the parser computed), integers one `i32`, switches 0 / 1, colours four words 0–255, vectors 3, quaternions 4, boxes 6 (`min`, then `max`), enums their index, paths a string offset. The compiler writes every key with its final value (built-in defaults included), except the optional ones the map didn't set (`shadow.home`, `fogCache.region`, `sky.sunDirection`, unset `ambientSky.*`, an empty `sky.panorama`). The reader starts from the built-in environment and applies the records, which gives the same environment as the text parser.

The records are defined once, in `runtime/monolith_scene.c` (region "Compiled maps"), with `_Static_assert`s on every record's size and on the field offsets.

**Deterministic output:** the same map gives byte-identical compiled files: records in map order, strings in first-use order, no timestamps, padding zeroed.

## 3. The reader checks everything first

A compiled file is input like any other. Before it trusts a byte, the reader checks:
- the magic, the format version, the byte-order tag, the header size, the file size against the real size, and a size cap (`MS_MAX_COMPILED_BYTES`, 256 MB);
- the section table: every section inside the file, aligned, known, once, with the expected record size; counts against `MS_MAX_OBJECTS` (boxes + models + lights, model collision boxes, and every other list);
- every string offset inside the table, the table NUL-terminated, no control characters in a name or path;
- every index in range: `MS_Ref` group / material / statement, a model's file, slot and collider ranges, slot materials; each material's uniforms and samplers follow the previous material's (no shared ranges, which would let a small file make the reader allocate a lot);
- every float finite, and the checks the parser makes on the same values (exactly those: a file the parser could have produced always loads): box corners ordered with finite sizes, box turns in range (box < box count), strictly increasing, with finite angles that are not all 0 and a turned box whose centre, size and AABB stay finite, spawn and viewpoint positions within ±1 000 000 m (light positions only finite: `repeat` may shift them past the limit), model scale > 0, a point light without a direction and the others with a unit one, spot `inner <= outer`, a light clip box ordered and not flat, viewpoint eye ≠ target, the `env` checks (`CheckEnvValue`, the parser's own function: ranges, `shadow.size`, sky sizes, non-zero sun and sky directions), enum values, integer ranges;
- every path package-relative (no absolute path, drive letter, backslash or `..`; the parser's own check), and the file present;
- material 0 named `default`, statement keys unique and not 0, boxes, models and lights in map order, a map that makes at least one box or model.

A file that fails any check is rejected with `<file>: <message>` (for example `forest.mapb: compiled map: section 'boxes' lies outside the file`, or `forest.mapb: compiled map format 4, this runtime reads formats 2 to 3: compile the map again`) and nothing is loaded; a bad file never crashes the reader. Errors found later in the load (a texture that won't load) name the source map and line, as for a text load: `forest.map:41: cannot load the texture "textures/bark.png"`.

## 4. API and detection

- **`MS_Load`** (unchanged signature) tells the two inputs apart by the file's **magic**, not by its extension:
  - a file: compiled if it starts with the magic; otherwise a text map (editor builds only: a game build reports that it reads only compiled maps);
  - a package folder, game build: `<name>/<name>.mapb`, else the folder's only `.mapb`; a folder that has only a `.map` gets a clear error saying it needs compiling;
  - a package folder, editor build: `<name>.map` first (the text is the source), then the same rules for `.mapb`.
- **`bool MS_Compile(const char *mapPath, const char *outPath, char *err, int errSize)`** (editor builds; `monolith --compile <out> --map <path>`): parses the text, writes the compiled bytes, runs the rest of a `noGpu` load as the package check (model files included), reads the bytes back as a game would, then writes `<out>.partial` and renames it, so a failed compile never leaves half a file. Paths inside stay package-relative, so `<out>` belongs in the package folder or a copy of it.
- **`MS_SourceState MS_CompiledSource(const MS_Scene *scene, const char **sourcePath)`** (editor builds): `MS_SOURCE_TEXT` (loaded from text), or for a compiled scene `MS_SOURCE_CURRENT` / `MS_SOURCE_STALE` / `MS_SOURCE_MISSING`, comparing the stored hash and size with the source file next to it (`*sourcePath`, when given). A stale compiled file loads with a warning in the editor and the dumps; `--export` refuses it, and exports a current one from its text.
- In editor builds a compiled scene has no map text (`MS_GetLine` / `MS_LineText` return nothing), its `hash` is the compiled file's, and its dependencies are the compiled file plus the stored list, so hot reload watches the right files. Manual edits and requests need the text and refuse a compiled map.
- `MS_SetInitHints` and `noGpu` peek loads work on compiled files as on text.

`--export` writes `<name>/<name>.mapb` next to the copied `<name>.map` and checks that it loads; the exported `viewer.c` opens the package folder and so the compiled map. `examples/build.sh` compiles the scene of each game-style example (`01_minimal`, `02_existing_project`, `05_entities`) with `build/linux/monolith`.

## 5. The two builds

| | editor build (`-DMONOLITH_EDITOR`: monolith, `03_live_editing`, the capture shim) | game build (exports, `01_minimal`, `02_existing_project`, `05_entities`) |
|---|---|---|
| reads | text and compiled | compiled only |
| text parser (tokenizer, statements, `repeat`, model-file reference scan) | yes | **not compiled in** (`--export` removes it) |
| compiled-file writer | yes (`MS_Compile`) | no |
| compiled-file reader | yes | yes |

A new statement that only makes boxes needs no format change (boxes are stored expanded), and neither does an `env` key of an existing type (keys are stored by name). Anything else the parser stores (a new field or kind of object, a new value type) needs a compiled record or field and a bump of `MS_COMPILED_VERSION` (`CM_VERSION` in the code); older compiled files are then rejected with a message to recompile, unless the reader keeps reading them (`CM_VERSION_OLDEST`: version 2 files have no box turns section and still load).

The model-file reference check (glTF URIs, OBJ material libraries and their textures must stay inside the package and fit R3D's importer path buffer) is part of the text parser, so it runs when a map is compiled and exported, not when a game loads the compiled file. A game trusts the model files it ships with, as it trusts its own executable.

## 6. Tests

- Round trip: every map in `maps/`, `tests/models` and `tests/rotated` (turned boxes) compiled into a copy of its package (twice: byte-identical; the Windows build writes the same bytes), then `--dump-boxes`, `--dump-scene`, `--dump-entities` (all but the `map "<path>"` line) and every viewpoint's `--shot` from the `.mapb` equal the text's, 0 pixels different; a `--reload-test` of a compiled map equals its plain shot.
- `tests/unit/compiled_test.sh`: builds the reader as a game has it with ASan + UBSan and loads ~18,000 damaged copies of the compiled `tests/models` and ~17,000 of `tests/rotated` (box turns out of range, unsorted, twice, not finite, 0 0 0, a centre that overflows): every truncation, every single-byte flip, 2000 random damages, and targeted header, section-table, index, float, string and path damage, each refused with a message or loaded as a valid scene; no crash, no leak.
- `tests/unit/export_strip_test.sh`: the exported `monolith_scene.c/.h` contain no tokenizer, statement parser, model-file scan, compiler, number parsing (`strto*`, `ato*`, `sscanf`), `".map"` lookup or `MONOLITH_EDITOR`.
- `tools/leakcheck.sh` compiles a map and loads it (dumps, a shot with a reload, a damaged and a stale file).

## 7. Defaults chosen

- Extension `.mapb`, flag `--compile <out> [--map <path>]`.
- Compiled files are build output (exports, `examples/build.sh`), not committed: `.gitignore` lists `*.mapb`.
- A game build's `MS_Load` on a package folder looks for `<name>.mapb`, with a clear error when only the `.map` is there.
- No derived data (hidden faces, cell meshes, colliders) in this round.
- Little-endian only; a big-endian host is refused.
- Map line numbers (`MS_Ref.line`, `MS_Source`) are kept: they are public API and cost 4 bytes per object.
