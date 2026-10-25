# 05_entities: entities, components and your own systems

Every box, model instance and light of a map is an **entity**. The scene runtime gives each one
an id and its data as **components**: parallel arrays in `MS_Scene`, indexed by the entity
number (`0 .. entityCount-1`, map order).

| Component | Array | What it holds |
|---|---|---|
| id | `entityIds` | `MS_EntityId { stmt, index }`: stable across map versions (the line's `id n`, or a key from the line's text, plus the object's index in the line) |
| object | `entityObjects` | the box, model instance or light it is (`MS_ObjRef`: kind + index into `boxes[]`, `models[]`, `lights[]`) |
| transform | `entityTransforms` | position, rotation (models), scale (boxes: their size) |
| source | `entitySources` | map line, statement, `repeat` copy, part (stairs step, waffle piece) |
| model | `entityModels` | the model file, -1 for boxes and lights |

Lookups: `MS_ObjectEntity(scene, obj)` (a box / model / light → its entity) and
`MS_FindEntity(scene, id)` (a stable id → the entity in the loaded map; a hash lookup).

The scene's components are read-only (the level is static). This example shows how a game adds
its own on top:

1. **Components** are the game's arrays, indexed by the same entity number, with a "has it"
   flag (`World` in `main.c`: a `Beacon` per light and model). New component = new array.
2. **Systems** are loops over the entities that have the components they need:
   `BeaconSystem` advances each beacon's phase, `BeaconDrawSystem` draws a glowing cube above
   the entity's position, inside the game's own R3D frame after `MS_PushLights` +
   `MS_DrawGeometry`.
3. **Ids name objects across map versions.** The entity number is just a position in this
   load; `MS_FindEntity(scene, id)` finds the object a stable id names (`--find`), e.g. for game
   code or save files that refer to "the door with id 12".

It is built like an exported game: the runtime without its editor parts (no `-DMONOLITH_EDITOR`,
so no hot reload).

```sh
examples/build/linux/05_entities/entities                    # the courtyard sample scene
examples/build/linux/05_entities/entities maps/pine_forest   # from the repo root: 719 models get beacons
examples/build/linux/05_entities/entities --find a7d95ba7:0   # look an entity up by its id
```

It prints a summary (entities per kind, the first of each kind with all its components).
Ids are the ones `monolith --dump-entities <file>` writes. Keys: right mouse + WASD / Q / E fly,
LEFT/RIGHT viewpoints, TAB the next entity with a beacon (its components at the bottom).

Ids made by hand edits in monolith (`id n` on the line) stay the same whatever happens to the
line; implicit ones stay the same while the line's text does ([map format](../../docs/map-format.md) §2.9).
