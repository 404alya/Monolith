# Migration fixtures

Old-format maps and the text the `migrate-map` skill must turn them into. They are test input: never migrate these files themselves.

| Fixture | Format | What it covers |
|---|---|---|
| `format1/format1.map` | 1 | `maps/monolith.map` as of commit b70d5a2 (2026-10-10): no format line, a comment on line 1, 290 boxes, two real `at` lines (37: folds exactly; 51: needs a leading one-copy repeat) |
| `format2/format2.map` | 2 | a small package written for the 2 → 3 rules: viewpoints, a spawn, materials, and `at` on a line that folds exactly, a line whose fold is not exact (form (b)), a `block` with `rot`, a `light point`, and a line with `id` and a `repeat`; the word `at` in a comment and in a group name |

`<fixture>/expected.map` is the format-3 text that `.claude/skills/migrate-map/versions.md` produces from the fixture, rule by rule. It was checked against the last reader (commit 4f8e030): its format-2 rendition (`monolith 3` → `monolith 2`) gives a byte-identical `--dump-boxes`, an equal `--dump-scene` and `--dump-scene-diff` (apart from the `map` line) and 0 changed pixels at every view of the proof, against the fixture.

Used by:
- the `migrate-map` skill's tests: migrating a copy of a fixture must give `expected.map` byte for byte and pass the proof;
- the parser's rejection test: monolith refuses both fixtures (format 1 and 2 are older than the code's format) with the "older than this monolith ... /migrate-map" message, and loads `expected.map`.
