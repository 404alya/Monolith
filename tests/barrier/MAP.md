# barrier: test package

Not a level: a small package of `barrier` lines (invisible walls, `docs/map-format.md` §2.1, *Barriers*) for
the engine's tests. Shots of it must be identical with and without its `barrier` lines (they are
never drawn); `--show-barriers` draws the editor's overlay for a check; dumps from the text and from
`--compile` must match; edit scripts move, turn, resize, add and remove barriers. Keep it small and
self-contained (no external assets).

## Style and mood

Plain coloured blocks on the default daylight environment (the `--new-map` template).

## Scale and walkability

- Player: radius 0.3 m, height 1.75 m, eye at 1.62 m; steps up to 0.45 m; jumps ~0.9 m.
- The ledge (x 4 .. 10, z −6 .. 0, top y 2) has a barrier along its far edge (`id 7`).

## Materials and palette

`stone` (the ledge and a wall), `post` (green), `default` (the ground).

## Lighting and atmosphere

The built-in daylight environment, unchanged.

## Rules for new content

Boxes and barriers only; no models, textures or shaders. One barrier of each case: axis-aligned,
turned, `repeat`, `id`, imported.
