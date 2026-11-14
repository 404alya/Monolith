# Troubleshooting

## Building

**The first `./build.sh` fails in R3D, raylib or assimp.** The full log is in `submodules/r3d/build-linux/monolith-build.log` (Windows: `build-mingw/`). Usually a missing package: see the install lines in the [README](../README.md). After installing it, run `./build.sh` again; to force a fresh configure, move `submodules/r3d/build-linux` out of the way.

**`./build.sh` fails before any `[INFO] CMD:` line.** It first compiles the build program `nob.c` into `build/nob` with `${CC:-clang}`; the compiler's message says why (usually no C compiler: install clang, or run `CC=gcc ./build.sh`). A `build/nob` from a broken `nob.c` is replaced on the next run once `nob.c` compiles again.

**The Linux binary doesn't start on another machine.** It needs glibc 2.34 or newer, `libGL.so.1`, `libgcc_s.so.1`, and the X11 client libraries (libX11, libXrandr, libXinerama, libXcursor, libXi), which GLFW loads at start-up; `readelf -d build/linux/monolith | grep NEEDED` lists only the first ones.

## Starting

**`cannot load the map`.** Without `--map` monolith opens `maps/monolith`, which is not in this repository. Start with `--map maps/pine_forest`, or create a map with `--new-map <name>`.

**`map format N is older than this monolith (3)`.** The map was written for an earlier monolith, which read another map format; this one reads only the current format. Run `/migrate-map <package>` in Claude Code in this repository (the message names the map): it rewrites the map to the current format, proves the result identical against a build of the last monolith that read the old format, and keeps the original as `<name>.map.v<N>`. If it refuses, its report says why, and the map is left as it was ([map-format.md](map-format.md) §1.3). A map without a `monolith N` line at the top counts as format 1.

**`map format N is newer than this monolith (3): update monolith`.** The map comes from a later monolith: update your checkout and build again.

**`compiled map version …` or `compiled map of map format …`.** A `.mapb` compiled by another monolith, or from an older map format. Compile it again from its `.map` (`--compile`, or `--export`), migrating the `.map` first if it is older ([compiled-map.md](compiled-map.md)). An exported game is not affected: it ships the runtime that reads its own compiled map ([export.md](export.md)).

**A red `file:line: message` in the HUD.** The map file has an error; the editor keeps showing the last version that loaded. Fix the line and save; to see the error without the editor, run `./build/linux/monolith --dump-boxes /tmp/x.txt --map <map>`.

**Black or empty window, or a crash at start.** The GPU driver must support OpenGL 3.3 core. On AMD and Intel drivers a surface shader can fail to compile where NVIDIA accepts it; the log names the shader. Check shaders with `glslangValidator` and avoid GLSL built-in names such as `noise1`–`noise4`, `length` or `distance` as identifiers.

**`refusing to open the editor window from an AI agent's shell`.** You started the editor from a shell that Claude Code (or another agent) opened: `CLAUDECODE` or `AI_AGENT` is set there. Start it from your own terminal, or set `MONOLITH_ALLOW_WINDOW=1`. Agents themselves use `--shot` and the other windowless flags.

**It looks wrong or runs slowly after playing with F2.** The settings are saved in `~/.config/monolith/settings.ini`. Start with `--no-settings` to ignore it, choose a preset in F2, or move the file away.

## Requests

**`coordinator: not started: claude not found …`.** Install Claude Code and log in once (`claude` in a terminal). The editor looks in `PATH` and `~/.local/bin`, or set `MONOLITH_CLAUDE` to the program. Queued requests start with the next submit.

**No terminal opens.** The editor uses `MONOLITH_TERMINAL`, else `TERMINAL`, else the first of alacritty, kitty, foot, wezterm, xterm. kitty with `--single-instance` and wezterm's mux exit at once, so the editor can only close those windows on i3 / sway.

**A request stays `queued`.** The coordinator runs requests on the same map one at a time and code requests alone, at most three at once; a request on a map whose earlier request is asking a question (`[?]` in the HUD) waits until you answer it in the coordinator terminal. If the coordinator's terminal was closed, the next submit starts a new one, which picks up the queue. `requests/.coordinator/log` shows what the coordinator did (starts, claims, state changes, requeues); a request that never appears there was never claimed.

**The coordinator asks for approval of every command.** The working folder has no `CLAUDE.md` (a copied build folder), or `MONOLITH_CLAUDE_SAFE` is set. Run the editor from a repository checkout to work without prompts.

**A request finished as `failed` with "Interrupted".** The coordinator stopped while that request was changing files. Its `result.md` lists the changed files; check them, then send the request again if needed.
