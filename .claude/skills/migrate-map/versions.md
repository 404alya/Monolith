# Map format migrations

One entry per step `N → N+1` of the `.map` text format, oldest first. The `migrate-map` skill (`SKILL.md`, next to this file) applies them in order and proves each one against a build of the entry's `last-reader:` commit. A format change in the code ships its entry here in the same change (the `engine-code` skill, "Change the map format").

## Entry format

Every entry has these parts, in this order:

- **Heading:** `## N → N+1 (YYYY-MM-DD)`, the date the code stopped reading N.
- **Changes:** what format N+1 changed, in a sentence or two.
- **Rewrite rules:** numbered, applied in order, one per kind of affected line. Each one has a *before* and an *after* example line. A line no rule names is copied unchanged, byte for byte (comments, blank lines and trailing comments included). On a line a rule changes, everything after the last number it rewrites stays as it was, including the spaces before a trailing `#` comment. If a line is affected but no rule covers its case, the migration is refused (the skill's refusal report: `rule not covered`).
- **Lossless:** `yes` (every dump and shot of the result equals the original's), or `no` with exactly what differs and how the proof allows for it.
- **last-reader:** the full commit hash of the newest commit that still reads format N. `tools/old-monolith.sh` must be able to build it.
- **Proof:** what the three checks (box dump, scene dump, shots) must show for this step, including any allowed difference.

Number formats in the *after* lines: write a computed number as the shortest decimal that parses back to the same `float`. For example, `40 + 8.4` is written `48.4`, never `48.400001`. Python gives the float sum and its shortest decimal (pass the two numbers):

```
python3 -I -c "import struct,sys; f=lambda x: struct.unpack('f',struct.pack('f',x))[0]; s=f(f(float(sys.argv[1]))+f(float(sys.argv[2]))); import decimal; t=next(t for t in ('%.*g'%(p,s) for p in range(1,10)) if f(float(t))==s); print(format(decimal.Decimal(t),'f'))" 40 8.4
```

It prints `48.4`. The proof is what decides, not this check.

## 1 → 2 (2026-11-12)

**Changes:** format 2 added the format line `monolith 2` and the statements `material`, `use`, `env`, `spawn`, `viewpoint`, `light`, `model`, `paint`, and `rot` on box and block lines. Format 1 is a file without a format line. It has only `group`, `box`, `block`, `stairs`, `waffle`, `repeat` and `at`, and everything else takes the built-in defaults. Format 2 keeps exactly those defaults for whatever a file doesn't set, so adding the format line changes nothing else.

**Rewrite rules:**

1. **The format line.** If line 1 is blank or only a comment, write `monolith 2` at its start, followed by three spaces and its comment if it has one. That keeps every later line number:
   - before (line 1): `# monolith.map -- the brutalist test level. Edit while the game runs: it reloads on save.`
   - after (line 1): `monolith 2   # monolith.map -- the brutalist test level. Edit while the game runs: it reloads on save.`

   If line 1 holds a statement, insert a new line 1, `monolith 2`, and every later line moves down by one:
   - before (line 1): `box 0 0 0 1 1 1`
   - after (lines 1–2): `monolith 2` / `box 0 0 0 1 1 1`

   A UTF-8 byte-order mark at the start of the file stays in front of everything.
2. Nothing else changes. Leave `at` alone: step 2 → 3 rewrites it.

**Lossless:** yes. Only line 1 changes. With the inserted-line case of rule 1, the line numbers in the scene dump move down by one.

**last-reader:** `4f8e030d948fca26b2df64d1e0e65e77c7009c58`. It reads formats 1 and 2. Its build of `tests/migrate/format1/format1.map` gives 290 boxes, the same box dump as b70d5a2 (2026-10-10, the build that first read this map).

**Proof:**
- `--dump-boxes`: the last-reader build on the original equals the last-reader build on the result, byte for byte. Format 2 is not the current format, so the current build can't load the intermediate file. The skill proves the whole chain once more at the end (`SKILL.md`, "Proof").
- `--dump-scene`: equal apart from the `map "<path>" format N` line. If rule 1 inserted a line, every `line L` with L ≥ 1 in the original's dump is `line L+1` in the result's. Built-in objects (the `"default"` material) keep `line 0`. Nothing else differs. `--dump-scene-diff`, which has no line numbers, is equal apart from the `map` line in both cases.
- `--shot` at every viewpoint, at the default start view, and on the yaw sweep: 0 changed pixels each.

## 2 → 3 (2026-11-12)

**Changes:** format 3 removed the placement prefix `at x y z` and made the format line mandatory (a file without one is format 1). Everything else in format 2 is unchanged.

**Rewrite rules:**

1. **The format line.** Replace `monolith 2` with `monolith 3` and keep the rest of that line (spacing, trailing comment):
   - before: `monolith 2   # the format line, with a comment`
   - after: `monolith 3   # the format line, with a comment`
2. **`at x y z` with at most 2 `repeat`s on the line.** The parser computes each shifted number as `(at + i·d1 + j·d2 + k·d3) + value`, in `float`, in that order. Shifted numbers are the position arguments: box, all six; block, `cx y0 cz`; stairs, `cx baseY startZ`; waffle, `x0 x1 y0 y1 z`; light and model, `x y z` and a light's `clip` box. Sizes, counts, directions, `rot` and `scale` are never shifted. Use form (a) if it gives the same floats, otherwise form (b):
   - **(a) Fold** `at` into the line's numbers: delete `at x y z` and add the axis's `at` value to each shifted number (`x` to x-axis numbers, and so on), written as rule "Number formats" above says. The fold is exact when every `at` value, every shifted number and every `repeat` step on that axis is a multiple of 1/16 and no sum reaches 2^19 in magnitude. When that doesn't hold, the fold may still be exact (`40 + 8.4`). Try it, and keep it only if the step's proof shows it identical: the box dump for box primitives, the scene dump for `light` and `model` lines (they aren't in the box dump). If it isn't identical, use (b) for that line and prove again.
     - before: `repeat 6 0 0 9 at 0 0 40 box -4.5 34 0 4.5 36 8.4`
     - after: `repeat 6 0 0 9 box -4.5 34 40 4.5 36 48.4` (exact: z values 40 + 0 and 40 + 8.4 checked by the box dump)
   - **(b) A leading one-copy repeat.** Delete `at x y z` and write `repeat 1 x y z from 1` as the first `repeat` of the line, after `id n` if there is one, before the other repeats. Its single copy is `0 + (0 + 1)·(x, y, z)` = `(x, y, z)`, so the parser adds the same floats in the same order as with `at`. The one exception is a component written `-0`: `0 + -0` is `+0`, so a value that also ends at `-0` comes out `+0`. The proof catches that, and such a line is then `rule not covered`. The line's numbers stay as they were. The copy indices don't change either (a one-copy outer repeat leaves the copy number `i` as it was), but a bundle lists the line's repeat counts as `[1, n]`.
     - before: `repeat 17 0 0 4.5 at 0 0 -36 box -40 20 0 40 22 2.6`
     - after: `repeat 1 0 0 -36 from 1 repeat 17 0 0 4.5 box -40 20 0 40 22 2.6`
     - before: `id 7 repeat 5 0 0 2.6 at 0 0 -3.3 box 0 0 0 1 1 1`
     - after: `id 7 repeat 1 0 0 -3.3 from 1 repeat 5 0 0 2.6 box 0 0 0 1 1 1`
3. **`at` with 3 `repeat`s on the line:** not covered. A line holds at most 3 repeats, so (b) can't be used, and unrolling a repeat would change the line count and the copy indices. The migration is refused (`rule not covered: line L`), and the developer rewrites that line by hand.
4. A word `at` inside a comment or a `"string"` is not a prefix: leave it.

**Lossless:** yes. The boxes, lights, models and every other value are the same floats. The source references stay the same (same line, copy and part). What changes is the text of the lines with `at`, and with form (b) the repeat counts that a request bundle lists for such a line.

**last-reader:** `4f8e030d948fca26b2df64d1e0e65e77c7009c58` (reads formats 1 and 2; the first commit that refuses 2 is the one that adds this entry).

**Proof:**
- `--dump-boxes`: the last-reader build on the original equals the current build on the result, byte for byte.
- `--dump-scene`: equal apart from the `map "<path>" format N` line, line numbers included (this step never adds or removes a line).
- `--shot` at every viewpoint, at the default start view, and on the yaw sweep: 0 changed pixels each (last-reader build on the original, current build on the result).
