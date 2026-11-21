# Troubleshooting

## Building

**The first `./build.sh` fails in R3D, raylib or assimp.** The full log is in `submodules/r3d/build-linux/monolith-build.log` (Windows: `build-mingw/`). Usually a missing package: see the install lines in the [README](../README.md). After installing it, run `./build.sh` again; to force a fresh configure, move `submodules/r3d/build-linux` out of the way.

**`./build.sh` fails before any `[INFO] CMD:` line.** It first compiles the build program `nob.c` into `build/nob` with `${CC:-clang}`; the compiler's message says why (usually no C compiler: install clang, or run `CC=gcc ./build.sh`). A `build/nob` from a broken `nob.c` is replaced on the next run once `nob.c` compiles again.

**`` gio not found in PATH: the repo deletes only with `gio trash` (CLAUDE.md); install it (Arch: glib2, Debian / Ubuntu: libglib2.0-bin) and run again ``.** Printed by `build/tools/leakcheck`, `build/tools/test-coordinator` or `./build.sh test export_strip` (after the program's name, e.g. `leakcheck: `), which stop before doing anything. They clean up after themselves by moving their scratch files to the trash with `gio trash`, never deleting them; without `gio` they would leave them behind. Install the package the message names and run again. Claude Code's refused deletions add `gio not found in PATH: install it (…)` for the same reason. The build itself doesn't need `gio`.

**The Linux binary doesn't start on another machine.** It needs glibc 2.34 or newer, `libGL.so.1`, `libgcc_s.so.1`, and the X11 client libraries (libX11, libXrandr, libXinerama, libXcursor, libXi), which GLFW loads at start-up; `readelf -d build/linux/monolith | grep NEEDED` lists only the first ones.

## Starting

**`cannot load the map`.** Without `--map` monolith opens `maps/monolith`, which is not in this repository. Start with `--map maps/pine_forest`, or create a map with `--new-map <name>`.

**`map format N is older than this monolith (3)`.** The map was written for an earlier monolith, which read another map format; this one reads only the current format. Run `/migrate-map <package>` in Claude Code in this repository (the message names the map): it rewrites the map to the current format, proves the result identical against a build of the last monolith that read the old format, and keeps the original as `<name>.map.v<N>`. If it refuses, its report says why, and the map is left as it was ([map-format.md](map-format.md) §1.3). A map without a `monolith N` line at the top counts as format 1.

**`map format N is newer than this monolith (3): update monolith`.** The map comes from a later monolith: update your checkout and build again.

**`compiled map version …` or `compiled map of map format …`.** A `.mapb` compiled by another monolith, or from an older map format. Compile it again from its `.map` (`--compile`, or `--export`), migrating the `.map` first if it is older ([compiled-map.md](compiled-map.md)). An exported game is not affected: it ships the runtime that reads its own compiled map ([export.md](export.md)).

**A red `file:line: message` in the HUD.** The map file has an error; the editor keeps showing the last version that loaded. Fix the line and save; to see the error without the editor, run `./build/linux/monolith --dump-boxes /tmp/x.txt --map <map>`.

**Black or empty window, or a crash at start.** The GPU driver must support OpenGL 3.3 core. On AMD and Intel drivers a surface shader can fail to compile where NVIDIA accepts it; the log names the shader. Check shaders with `glslangValidator` and avoid GLSL built-in names such as `noise1`–`noise4`, `length` or `distance` as identifiers.

**A window that is black on Windows (seen on Intel HD Graphics Gen11), but shows while it is dragged.** The whole window is black, the text overlay too, and the picture comes back while the window is moved (or by itself at times). The cause is a **4x multisampled window**: that driver (Intel HD Graphics Gen11, driver 31.0.101.2140, GL 3.3) presents a multisampled window black while it stands still, although the frame is rendered and no GL call fails. Found on 2026-11-21 with the exported viewer's switches: every run with a 4x multisampled window went black, every run with a single-sampled one stayed visible, and vsync, a 60 fps cap and `glFinish` before the swap changed nothing. GL reported no error in any run, and the driver's own debug output (`--gl-debug`) reported nothing during the frames. **The fix:** the exported viewer and the examples open a single-sampled window, as the editor does. R3D anti-aliases its own image (FXAA), so MSAA on the window only smoothed the edges of the text drawn on top. If your own program on the scene runtime asks for `FLAG_MSAA_4X_HINT`, drop it. The viewer keeps the multisampled window as an opt-in (`--msaa`, black on that driver) and its other window switches for the next driver report: `--vsync`, `--fps <n>`, `--finish` and `--gl-debug` (the driver's debug messages and a GL error check every 10 s). Run it as `viewer.exe > log.txt 2>&1`, leave the window still and compare. The log always says which window was opened (`viewer: window: …`, `monolith_scene: window framebuffer: …`) and whether GL reported errors in the load and first frames.

**`refusing to open the editor window from an AI agent's shell`.** You started the editor from a shell that Claude Code (or another agent) opened: `CLAUDECODE` or `AI_AGENT` is set there. Start it from your own terminal, or set `MONOLITH_ALLOW_WINDOW=1`. Agents themselves use `--shot` and the other windowless flags.

**It looks wrong or runs slowly after playing with F2.** The settings are saved in `~/.config/monolith/settings.ini`. Start with `--no-settings` to ignore it, choose a preset in F2, or move the file away.

## Requests

**`coordinator: not started: claude not found …`.** Install Claude Code and log in once (`claude` in a terminal). The editor looks in `PATH` and `~/.local/bin`, or set `MONOLITH_CLAUDE` to the program. Queued requests start with the next submit.

**`coordinator: not started: gio not found in PATH (the sessions delete only with gio trash): install glib2 / libglib2.0-bin (the requests stay queued)`.** The sessions never delete a file; they move it to the trash with `gio trash` (`CLAUDE.md`), so without `gio` the editor starts none (in per-request mode the request fails with the same reason, and the bundle is kept). Install `glib2` (Arch) or `libglib2.0-bin` (Debian / Ubuntu); the editor keeps running, and queued requests start with the next submit. The editor searches the `PATH` it was started with: from a desktop launcher that may differ from your shell's.

**No terminal opens.** The editor uses `MONOLITH_TERMINAL`, else `TERMINAL`, else the first of alacritty, kitty, foot, wezterm, xterm. kitty with `--single-instance` and wezterm's mux exit at once, so the editor can only close those windows on i3 / sway.

**A request stays `queued`.** The coordinator runs requests on the same map one at a time and code requests alone, at most three at once; a request on a map whose earlier request is asking a question (`[?]` in the HUD) waits until you answer it in the coordinator terminal. If the coordinator's terminal was closed, the next submit starts a new one, which picks up the queue. `requests/.coordinator/log` shows what the coordinator did (starts, claims, state changes, requeues); a request that never appears there was never claimed.

**The coordinator asks for approval of every command.** The working folder has no `CLAUDE.md` (a copied build folder), or `MONOLITH_CLAUDE_SAFE` is set. Run the editor from a repository checkout to work without prompts.

**A request finished as `failed` with "Interrupted".** The coordinator stopped while that request was changing files. Its `result.md` lists the changed files; check them, then send the request again if needed. `Interrupted: the account's usage limit was reached; restarted on <account>` means the stop was a switch to the fallback account (below).

**The queue stopped: usage limit.** The coordinator's terminal shows `You've hit your session limit · resets …` (or the weekly limit), and nothing runs until the reset. To continue on a second Claude account, set it as the fallback before starting the editor (fish: `set -gx MONOLITH_CLAUDE_FALLBACK "CLAUDE_CONFIG_DIR=$HOME/.claude-personal"`; see [command-line.md](command-line.md)). Then start that account once by hand in the repo root (`CLAUDE_CONFIG_DIR=~/.claude-personal claude`): log in if asked, accept "trust this folder?", and quit. From then on the editor restarts the coordinator on the fallback when the limit comes. The HUD shows `coordinator: running (fallback account .claude-personal), …` and `main account limited until 15:45`. Requests that were running go back to the queue, or are reported as `Interrupted:` if they had changed files. The editor goes back to main at the first coordinator start after main's reset.
- *The HUD still says `running` on main while the terminal shows the limit:* the limit wasn't detected (the coordinator's `StopFailure` hook didn't fire). Type `! build/tools/request-queue switch` in the coordinator terminal: the editor restarts it on the next account within a few seconds. Check `requests/.coordinator/limit` (one line per limit: time, account, reset, how it was reported) and the editor's log (`monolith: usage limit on …`).
- *`… uses the same config dir as main: it is the same account`:* the editor's shell already has that `CLAUDE_CONFIG_DIR`. Set the main account explicitly with `MONOLITH_CLAUDE_ENV="CLAUDE_CONFIG_DIR=$HOME/.claude"`, or start the editor from a shell without it.
- *`coordinator: usage limit on main; can't stop it from here`:* the coordinator wasn't started by a wrapper this editor can find (`requests/.coordinator/pid`). Close its terminal with CTRL+SHIFT+K or by hand; the next submit starts one on the fallback.
- *`all accounts limited; <account> resets <time>`:* every account is at its limit; the requests wait.
