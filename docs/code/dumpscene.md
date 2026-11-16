# `src/dumpscene.c`

`--dump-scene`, `--dump-scene-diff` and `--dump-entities`: everything a map produces, one item per line, so map diffs stay checkable (diff two dumps).

Floats that come from geometry are C hex floats (`%a`, exact); a few human-readable values follow in decimal after a `#`.

## --dump-scene

The version line comes first:

```
monolith-scene-dump 1
map "<path>" format <n>
counts boxes <n> models <n> modelFiles <n> materials <n> lights <n> groups <n> colliders <n> [turned <n>]
box <i> <minx miny minz maxx maxy maxz>  line <l> group <g> material <m>
    [rot <3 hex> center <3 hex> size <3 hex>]
modelfile <i> "<path>" loaded <yes|no> meshes <n> triangles <n> materials <n> bounds <6 hex>
model <i> file <f> line <l> group <g> material <m|-1> origin <o> collide <c> shadow <on|off>
      position <3 hex> rotationDeg <3 hex> scale <3 hex> translation <3 hex> rotation <4 hex>
      bounds <6 hex> colliders <first> <count> slots <N=m,...|->
collider <i> <6 hex>
material <i> "<name>" line <l> shader "<path>" unlit <on|off> [draw_distance <off|hex>]
light <i> <point|spot|dir> line <l> position <3 hex> direction <3 hex> color r g b energy <hex> range <hex> shadow <on|off> [clip <3 hex> <3 hex>]
sun direction <3 hex> energy <hex> range <hex> shadow <on|off>
```

| Item | Notes |
|---|---|
| `box` | A turned box adds `rot`, `center` and `size` (its rotation and shape); its six numbers are then its world AABB. |
| `collider` | Model colliders only; i ≥ boxes. |

## --dump-entities

Writes the entities (`runtime/monolith_scene.h`, "Entities"), in map order:

```
monolith-entity-dump 1
map "<path>" entities <n>
entity <e> id <stmt as 8 hex digits>:<index> <explicit|implicit> <box|model|light> <object index>
       line <l> copy <c> part <p> position <3 hex> rotationDeg <3 hex> scale <3 hex> model <file|-1>
```

## --dump-scene-diff

Writes the same items as `--dump-scene` without anything that shifts when lines are inserted or removed above them: no object, group, material, file or collider indices and no map lines.

- References are by name instead: `group "<name>"`, `material "<name>"` or `-`, `file "<path>"`, `slots N="<name>"`.
- A model lists only its collider count.
- The first line is `monolith-scene-dump 1 diff`.

So the diff of two such dumps shows only the objects that really changed: an inserted model is one added line, not a renumbering of every later row.
