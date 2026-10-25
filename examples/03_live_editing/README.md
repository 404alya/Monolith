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

The same reload path is tested without a window by monolith itself: `monolith --shot x.png
--reload-test <package>` reloads after 10 frames and must render the same image as a plain shot
of that package. This example is built with the editor runtime (`-DMONOLITH_EDITOR`: `MS_Poll`
is an editor function); a shipped game doesn't need it.
