# 02_existing_project: adding a monolith scene to an existing game

`before.c` is a typical small raylib + R3D game: a third-person camera, a player capsule with
box collision, gravity and jumping, a few crates, its own shadowed sun, a lamp and a sky, on a
flat floor. `after.c` is the same game with the level coming from a monolith map package (the
courtyard of `01_minimal`). Every changed line is marked `monolith:`.

```sh
examples/build/linux/02_existing_project/before --shot /tmp/before.png
examples/build/linux/02_existing_project/after  --shot /tmp/after.png     # [package] optional
```

Keys: WASD move, LEFT/RIGHT or the mouse turn, SPACE jumps.

## What had to change (about 25 lines)

1. **Include and load.** `#include "monolith_scene.h"`, add `monolith_scene.c` to the build.
   Parse the package once with `.noGpu = true` before `R3D_Init` and call `MS_SetInitHints`
   (the sun's shadow map size is an R3D init hint), then `MS_Load` + `MS_ApplyEnvironment`.
2. **Remove what the scene now provides:** the floor mesh, the game's own sky, ambient map,
   tonemap setting and sun (with its shadow map). Keep your own lights for your own things (the
   lamp stays). R3D has two directional shadow layers and the scene's sun uses them, so a
   second shadowed sun of your own would compete for them.
3. **Draw inside your own frame.** Between your `R3D_Begin*` and `R3D_End`:
   `MS_PushLights(scene, camera, aspect)` and `MS_DrawGeometry(scene)`, next to your draws.
4. **Collision.** `MS_Collides(scene, box)` (strict AABB overlap with every box and model
   collider of the map) replaces the floor test; the crates are still tested by the game.
5. **Start at the map's spawn** (`scene->spawnFeet`, `spawnYawDeg`), optional.
6. **Hot reload** (optional): `if (MS_Poll(scene, dt) == MS_RELOADED) MS_ApplyEnvironment(scene);`
   (see `03_live_editing`).
7. **Unload**: `MS_Unload(scene)` before `R3D_Close`.

Not done here, see the export README: `glEnable(GL_DEPTH_CLAMP)` (matters for tall shadow
casters far away; this scene has none).

## The diff

```diff
@@ includes
+#include "monolith_scene.h"                         // monolith: the scene runtime
+
+static MS_Scene *scene;                             // monolith: the level
@@ Blocked()
-    if (b.min.y < 0.0f) return true;
+    if (MS_Collides(scene, b)) return true;         // monolith: was the flat floor (y < 0)
@@ main(): before R3D_Init
+    const char *package = argc > 1 && argv[1][0] != '-' ? argv[1] : TextFormat("%sscene", GetApplicationDirectory());
+    char err[512];
+    MS_Scene *peek = MS_Load(package, &(MS_LoadOptions){ .noGpu = true }, err, sizeof(err));
+    if (!peek) { puts(err); return 1; }
     ShotInitWindow(&shot, 1280, 720, "my game");
+    MS_SetInitHints(peek);                          // monolith
+    MS_Unload(peek);                                // monolith
     R3D_Init(GetScreenWidth(), GetScreenHeight());
@@ main(): setup
-    R3D_Mesh floorMesh = R3D_GenMeshPlane(60, 60, 1, 1);
+    scene = MS_Load(package, NULL, err, sizeof(err));   // monolith: load the level
+    if (!scene) { puts(err); return 1; }
+    MS_ApplyEnvironment(scene);                         // monolith: sky, ambient, fog, post-processing
-    R3D_Material floorMat = ...;                        (2 lines)
-    R3D_Cubemap sky = R3D_GenProceduralSky(...);        (sky, ambient map, tonemap, sun + shadow map: 9 lines)
-    Player player = { .feet = { 0, 0, 16 }, .yaw = PI };
+    Player player = { .feet = scene->spawnFeet, .yaw = scene->spawnYawDeg * DEG2RAD };   // monolith: the map's spawn
@@ main loop
+        if (MS_Poll(scene, GetFrameTime()) == MS_RELOADED) MS_ApplyEnvironment(scene);  // monolith: hot reload (optional)
-                R3D_PushLightEx(sun, sunShadow, true);
+                MS_PushLights(scene, camera, (float)GetScreenWidth() / GetScreenHeight());   // monolith: sun + map lights
+                MS_DrawGeometry(scene);                                                     // monolith: the level
                 R3D_PushLight(lamp);
-                R3D_DrawMesh(floorMesh, floorMat, Vector3Zero(), 1.0f);
@@ cleanup
-    R3D_UnloadShadowMap(sunShadow); R3D_UnloadAmbientMap(ambient); R3D_UnloadCubemap(sky); R3D_UnloadMesh(floorMesh);
+    MS_Unload(scene);                               // monolith
```

(`diff -u before.c after.c` gives the full diff; the comment header differs as well.)
