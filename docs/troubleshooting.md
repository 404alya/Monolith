# Troubleshooting

## Building

**The first `./build.sh` fails in R3D, raylib or assimp.** The full log is in `submodules/r3d/build-linux/monolith-build.log` (Windows: `build-mingw/`). Usually a missing package: see the install lines in the [README](../README.md). After installing it, run `./build.sh` again; to force a fresh configure, move `submodules/r3d/build-linux` out of the way.

**The Linux binary doesn't start on another machine.** It needs glibc 2.34 or newer and `libGL.so.1`; everything else is linked in. Check with `readelf -d build/linux/monolith | grep NEEDED`.

## Starting

**`cannot load the map`.** Without `--map` the game opens `maps/monolith`, which is not in this repository. Start with `--map maps/pine_forest`, or create a map with `--new-map <name>`.

**A red `file:line: message` in the HUD.** The map file has an error; the game keeps showing the last version that loaded. Fix the line and save; to see the error without the game, run `./build/linux/monolith --dump-boxes /tmp/x.txt --map <map>`.

**Black or empty window, or a crash at start.** The GPU driver must support OpenGL 3.3 core. On AMD and Intel drivers a surface shader can fail to compile where NVIDIA accepts it; the log names the shader. Check shaders with `glslangValidator` and avoid GLSL built-in names such as `noise1`–`noise4`, `length` or `distance` as identifiers.

**`refusing to open the game window from an AI agent's shell`.** You started the game from a shell that Claude Code (or another agent) opened: `CLAUDECODE` or `AI_AGENT` is set there. Start it from your own terminal, or set `MONOLITH_ALLOW_WINDOW=1`. Agents themselves use `--shot` and the other windowless flags.

**It looks wrong or runs slowly after playing with F2.** The settings are saved in `~/.config/monolith/settings.ini`. Start with `--no-settings` to ignore it, choose a preset in F2, or move the file away.

## Requests

**`coordinator: not started: claude not found …`.** Install Claude Code and log in once (`claude` in a terminal). The game looks in `PATH` and `~/.local/bin`, or set `MONOLITH_CLAUDE` to the program. Queued requests start with the next submit.

**No terminal opens.** The game uses `MONOLITH_TERMINAL`, else `TERMINAL`, else the first of alacritty, kitty, foot, wezterm, xterm. kitty with `--single-instance` and wezterm's mux exit at once, so the game can only close those windows on i3 / sway.

**A request stays `queued`.** The coordinator runs requests on the same map one at a time and code requests alone, at most three at once; a request on a map whose earlier request is asking a question (`[?]` in the HUD) waits until you answer it in the coordinator terminal. If the coordinator's terminal was closed, the next submit starts a new one, which picks up the queue. `requests/.coordinator/log` shows every queue event.

**The coordinator asks for approval of every command.** The working folder has no `CLAUDE.md` (a copied build folder, Windows), or `MONOLITH_CLAUDE_SAFE` is set. Run the game from a repository checkout to work without prompts.

**A request finished as `failed` with "Interrupted".** The coordinator stopped while that request was changing files. Its `result.md` lists the changed files; check them, then send the request again if needed.
