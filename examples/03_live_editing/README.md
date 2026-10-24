# 03_live_editing: your game hot-reloads what monolith is editing

The game polls every file of a map package (`MS_Poll`, about twice a second: the map, its
shaders, textures and models). When one changes, the whole package is reloaded and swapped in
(`MS_RELOADED`; apply the environment again, then your own overrides). A broken edit keeps the
last good scene (`MS_RELOAD_FAILED`, the reason in `MS_Error(scene)` as `file:line: message`).

## Side by side with monolith

1. From the repo root, start monolith on a package, e.g.
   `build/linux/monolith --map maps/pine_forest`.
2. Start this example on the **same folder**:
   `examples/build/linux/03_live_editing/live_editing maps/pine_forest`.
3. Edit: submit a request in monolith (spectator mode, Enter), or save the `.map` file in an
   editor. Both windows show the change within a second; this one prints
   `reloaded: N boxes ...` or the error.

Keys: right mouse + WASD / Q / E fly, LEFT/RIGHT viewpoints. Both programs only read the
package, so any number of them can watch it.

## Testing it without a window

`--reload <package>` reloads from another package after 10 frames through the same code path
as a file change (`MS_Reload`). Its `--shot` must equal a plain `--shot` of that package, which
proves that everything (instances, materials, shadow tiles, environment) is rebuilt:

```sh
live_editing scene --reload /tmp/sceneB --shot /tmp/reloaded.png
live_editing /tmp/sceneB --shot /tmp/direct.png        # identical image
```
