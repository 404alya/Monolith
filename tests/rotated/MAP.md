# rotated: test package

Not a level: a small package of turned boxes (`box` / `block` lines with `rot`, format 2) for the
engine's tests (docs/live-editing.md 11.12). `tests/unit/collision_test.sh` walks the player
over it (noGpu); shots, picks and edit scripts check drawing, picking and manual edits of turned
boxes. Keep it small and self-contained (no external assets); the collision test reads the
coordinates below, so change them only together with `tests/unit/collision_test.c`.

## Style and mood

Plain coloured blocks on the default daylight environment (the `--new-map` template).

## Scale and walkability

- Player: radius 0.3 m, height 1.75 m, eye at 1.62 m; steps up to 0.45 m; jumps ~0.9 m.
- The ramp is 30 degrees (walkable: `MS_WALKABLE_SLOPE_DEG` is 46), 3 m wide, from the ground at
  x 5.57 up to y 5 at x 14.23, onto the platform (x 14.3 .. 20, top y 5).

## Materials and palette

`ramp` (sand), `wall` (blue grey), `crate` (brown), `post` (green), `inst` (red, `instanced on`:
its boxes stay instanced cubes, one of them turned), `default` (the ground).

## Lighting and atmosphere

The built-in daylight environment, unchanged.

## Rules for new content

Boxes only. Each case of a turned box once; no models, textures or shaders.

## Groups and landmarks

| Group | What it is | Where |
|---|---|---|
| `ground` | Ground slab, 200 x 200 m, top at y = 0 | x, z +-100 |
| `ramp` | 30 degree ramp (a block rolled about +Z) and the platform at its top | x 5.6 .. 20, z -7 .. -3 |
| `yawed wall` | 6 x 3 x 0.4 m wall yawed 45 degrees | centre 0 -15 |
| `props` | a crate turned about all three axes; a `repeat` row of 5 posts yawed 30 degrees | x -14 .. -6, z -9 .. -4 |
| `instanced` | an axis-aligned and a turned box of the `instanced on` material | x 4 .. 7, z -12 |
