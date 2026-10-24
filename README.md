# monolith

An **AI-assisted map designer** for 3D levels of any style. You fly through a level, point at a spot, select objects or drag a rectangle over the screen, and type what should change: "make this wall twice as tall", "plant three more pines along the path", "hang a neon sign over each of these stalls". Claude Code carries the request out in the background, and the running game reloads the map so the change appears in place.

The game is the editor and the viewer. It is written in plain C11 on [raylib](https://www.raylib.com/) 5.5 and [R3D](https://github.com/Bigfoot71/r3d) 0.10 (deferred PBR: shadows, SSAO, SSIL, fog, volumetric fog, bloom, procedural sky). A level is a **map package** in `maps/<name>/`: a plain-text `.map` file of boxes, models, materials, lights and environment, a `MAP.md` design brief that keeps every edit in the map's style, and the map's shaders, models and textures. The scene runtime (`runtime/monolith_scene.h/.c`) also loads a package in any other raylib + R3D program, so a map can ship in your own game.

## Build

Linux x86_64 needs clang (or `CC=gcc`), cmake ≥ 3.22, python3, a C++ compiler and the X11 / OpenGL development headers. R3D, raylib and assimp are vendored in `submodules/r3d` and built on the first run (a few minutes).

```bash
./build.sh              # -> build/linux/monolith + build/linux/maps/
./build.sh run          # build and start the game
./build.sh --win        # Windows x86_64 with MinGW-w64 -> build/win/monolith.exe (runs under wine too)
examples/build.sh       # the examples of using a map in your own program
```

Both output folders are self-contained: copy or zip them as they are.

## First steps

```bash
./build/linux/monolith                          # the default map
./build/linux/monolith --map maps/pine_forest   # another map package
./build/linux/monolith --new-map my_level       # create maps/my_level/, then open it with --map
```

Press **TAB** for spectator mode, aim, and press **Enter** to type a request. Requests need [Claude Code](https://claude.com/claude-code) installed (`claude` in `PATH` or `~/.local/bin`): the first request opens one terminal, the coordinator, that works through every request you send. The [user guide](docs/user-guide.md) explains the keys, the request list and what to expect.

Note: `maps/` in this repository only contains `pine_forest`; the default map package (`maps/monolith`) is kept locally. Without it, start with `--map maps/pine_forest` or create one with `--new-map`.

## Documentation

| Page | For |
|---|---|
| [docs/user-guide.md](docs/user-guide.md) | using the game: moving, selecting, requests, settings, making maps |
| [docs/map-format.md](docs/map-format.md) | writing or reading a `.map` file and its design brief |
| [docs/command-line.md](docs/command-line.md) | every flag and environment variable |
| [docs/export.md](docs/export.md) | using a map in your own raylib + R3D program |
| [docs/architecture.md](docs/architecture.md) | how the code is organised, the scene runtime, the request queue, known limitations |

## Working on monolith with Claude Code

The repository carries everything Claude Code needs: `CLAUDE.md` (guard rails and hard rules, loaded in every session) and project skills in `.claude/skills/` (the workflows for map requests, map editing, assets and licences, engine code, lighting, benchmarking, the leak check, larger engine work, the request coordinator, and the project's history), plus the `/task-prompt` command. Claude loads the matching skill for the task by itself. Machine-specific settings go in your own `.claude/settings.local.json` (gitignored), e.g. `{"env": {"BLENDER": "/path/to/blender"}}` when Blender is not in `PATH`.

Local changes to R3D are recorded in `submodules/r3d/patches.md`. Every external asset in a map package has its source and licence recorded in the package's `ASSETS.md`.
