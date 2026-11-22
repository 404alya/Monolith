# imports: test package

Not a level: a small package that tests `import` (one map in several map-text files,
docs/map-format.md §2.10). `imports.map` imports `parts/a.map` (which imports
`parts/nested/c.map`) and `parts/b.map`, with `use`, `group` and `material` lines on both sides of
each import, a material defined in the main file and extended in a part, a `repeat` line, a model,
a `paint` line with its atlas, lights, the spawn and three viewpoints.

`imports-flat.map` is its **single-file twin**: the same lines in one file, with the scope rules
written out (`use "default"` where a part starts, the importing file's `use` again where an import
ends; every import is followed by a `group` line, so no `group` has to be restated). Load it with
`--map tests/imports/imports-flat.map` (the package folder loads `imports.map`). The two must give the
same `--dump-boxes`, the same `--dump-scene` and `--dump-entities` without the `line` fields and the
`map` line, and pixel-identical shots from every viewpoint. Keep the two in step when either changes.

## Groups and landmarks

| File | Groups |
|---|---|
| `imports.map` | `ground`, `main: before the imports`, `main: between the imports`, `main: after the imports` |
| `parts/a.map` | `part a`, `part a: after c` |
| `parts/nested/c.map` | `part c (nested)` |
| `parts/b.map` | `part b` |
