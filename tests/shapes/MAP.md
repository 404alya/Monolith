# shapes: test package

Not a level: a small package of `shape` lines (generated primitives, `docs/map-format.md` §2.1, *Shapes*;
the design: `docs/editor-tools.md` §2) for the engine's tests. Every kind (cube, sphere, cylinder,
cone, capsule, plane) appears axis-aligned and turned, with flat, metallic, textured (a CC0 texture)
and surface-shader materials; plus `repeat`, `id`, a capsule whose caps meet (no side), `collide off`
/ `shadow off` shapes and a paint entry on every kind. Used by: dumps from the text and from `--compile`
(they must match), shots of every viewpoint (text, compiled and the exported viewer pixel-identical),
`./build.sh test compiled` (section 22 and paint kind 2), edit scripts (`--edit-script` against
`--apply-edits` on a copy). Keep it small and self-contained.

## Style and mood

Plain primitives on the default daylight environment (the `--new-map` template).

## Scale and walkability

- Player: radius 0.3 m, height 1.75 m, eye at 1.62 m; steps up to 0.45 m; jumps ~0.9 m.
- Shapes are 1–3 m; a shape collides as its box (a plane: 0.01 m thick below it). The green `ghost`
  cylinder and plane (x 15..20, z −1) are walk-through and cast no shadow.

## Materials and palette

`clay` (terracotta), `metal` (polished), `ground037` (the ambientCG Ground037 texture, once per part),
`tiled` (the same texture, `uv_scale 3 3`), `parts` (`shaders/parts.glsl`: an 8 × 8 checker of the
part UV, tinted red along u and blue along v, so a shot shows each part's UV layout), `ghost` (`collide
off shadow off`), `paintable` (grey, with the paint atlas `textures/paint/paintable.png`: per rectangle a
red band along u = 0, a blue band along v = 0, a yellow dot). `default` is the ground.

## Groups and landmarks

| Group | Where | What |
|---|---|---|
| plain row | z −6, x −10..10 | every kind, axis-aligned: cube, sphere, cylinder, cone, capsule, plane |
| turned row | z 1, x −10..10 | every kind with `rot` (a lying cylinder, a pitched cone, a rolled sphere …) |
| capsules and planes | x 15..20, z −7..−1 | `id 20 repeat 3` capsules, `id 21` capsule whose caps meet, the `ghost` shapes |
| painted | z 8, x −8..12 | `id 30`–`35`: one shape of each kind with paint entries (`# paint (draw mode)` at the end) |

## Viewpoints

1 Overview, 2 Plain row, 3 Turned row, 4 Capsules and planes, 5 From below (low, under the turned
sphere), 6 Top down, 7 Painted.

## Lighting and atmosphere

The built-in daylight environment, unchanged.

## Rules for new content

Shapes (and the ground box) only; no models. Every kind keeps one plain and one turned example.
