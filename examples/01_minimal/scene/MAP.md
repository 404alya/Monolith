# courtyard: design brief

A small sample scene for `examples/01_minimal` and a test map for `--export`. Read this file first on every request for this map.

## Style and mood

A quiet Mediterranean courtyard in the late afternoon: warm plaster walls, a terracotta floor, a stone arcade, a fountain in the middle, striped awnings. Calm and simple; few objects, clear shapes.

## Scale and walkability

- Player: radius 0.3 m, height 1.75 m, eye at 1.62 m; steps up to 0.45 m.
- The courtyard is 40 x 35 m; walls 7-9 m; the arcade 5 m high with pillars every 5 m.

## Materials and palette

| Material | Use |
|---|---|
| `terracotta` | the floor (plain colour) |
| `plaster` | walls, warm off-white |
| `stone` | arcade, fountain column |
| `tiles` | fountain rim (`textures/tiles.png`, one repeat per face) |
| `water`, `iron`, `glow` | fountain water, lamp posts, lamp heads (emissive) |
| `awning` | `shaders/stripes.glsl`: red/cream stripes in world space |

## Lighting and atmosphere

Low warm sun from the north-east (`sun.direction 0.62 -0.42 -0.66`), a warm horizon, light haze (fog density 0.002). Four warm point lights on the lamp posts.

## Rules for new content

Boxes only, in the palette above. No models.

## Groups and landmarks

| Group | What it is |
|---|---|
| `ground`, `walls`, `arcade`, `fountain`, `awnings`, `lamps` | as named |
