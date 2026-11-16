# `src/launcher.c`: request bundles, the request queue and coordinator, polling for `result.md`

A submitted request becomes a self-contained folder `requests/<YYYY-MM-DD_HHMMSS>_<slug>/`: `view.png`, `view-marked.png`, `region-<n>.png` per screen region, `request.md`, `request.json` (bundle format 6, `.claude/skills/map-request/bundle-reference.md`).

The model is chosen per request (fast / default / heavy tier): by an override, else by the AI classifier, one `claude -p` per request run in the background (regions "Model choice" and "Classifying", `src/classifier.c`, [classifier.md](classifier.md)). `request.md`, `request.json` and `state` are written once it has answered.

The user's prompt never goes on a command line: it only lives in the bundle (and on the classifier's stdin), and every process is started from an argv array (no shell).

Linux only: the other platforms' build is view-only and leaves this file out (`nob.c`; the stubs in `launcher.h`; `MONOLITH_EDITING` in `common.h`).

## Two ways to hand the bundle to Claude Code

`MONOLITH_LAUNCH` picks one.

### Coordinator (default)

Region "Coordinator and queue"; [request-queue.md](../request-queue.md).

The editor writes `requests/<id>/state` = `queued` as the bundle's last file. Unless a coordinator holds `requests/.coordinator/lock`, one terminal titled "monolith-claude: coordinator" runs a small `/bin/sh` wrapper that holds the lock (`flock`) around:

```
claude <permission flags> --model $MONOLITH_COORDINATOR_MODEL (default sonnet) [MONOLITH_CLAUDE_ARGS]
  "You are the monolith request coordinator. Read .claude/skills/coordinator/SKILL.md ... Requests folder: <path>"
```

Later requests only go into the queue; they never open a window. The coordinator (`build/tools/request-queue`) claims bundles, hands each to a subagent on the request's model and writes every later state (claimed, question, done, failed, cancelled). The HUD follows `state` + `result.md`. Without `flock` (util-linux) the editor falls back to per-request mode.

### Per-request

A new terminal per request runs, in the repo root (no state file):

```
claude <permission flags> --model <name> [MONOLITH_CLAUDE_ARGS]
  "Map edit request: read requests/<id>/request.md and carry it out."
```

This is the behaviour before 2026-10-24.

## Where things go

### `requests/` and the working directory

`requests/` and the working directory of Claude Code are the repo root baked in by `build.sh` (`PROJECT_PATH`) while it exists, i.e. in a repo checkout; otherwise the directory of the executable (a copied `build/linux` folder). `MONOLITH_REQUESTS_DIR` (tests only) moves `requests/` (bundles, queue, lock) elsewhere.

### Terminal

`$MONOLITH_TERMINAL`, else `$TERMINAL`, else the first of alacritty, kitty, foot, wezterm, xterm found in `PATH`. The variables may hold a program name or path plus extra arguments (`"kitty --single-instance"`); the program's basename picks the syntax from `TERMINALS` in the source, and unknown terminals get `-e <command>`.

### claude

`$MONOLITH_CLAUDE` (tests: `build/tools/stub-coordinator`), else searched in `PATH`, then `~/.local/bin/claude` (where the native installer puts it, which monolith started from a desktop launcher may not have in `PATH`). It is passed to the terminal as an absolute path, so a terminal daemon with another environment (`kitty --single-instance`) still finds it.

### Permissions

Every session starts with `--allow-dangerously-skip-permissions --dangerously-skip-permissions`, so the developer isn't asked to approve each command; the guard rails live in `CLAUDE.md` and the coordinator / map-request skills (`.claude/skills/`) instead. `MONOLITH_CLAUDE_SAFE=1` (any value but empty or `0`) drops the two flags. So does a working directory without `CLAUDE.md` (a copied build folder): with no guard rails to read, the session keeps Claude Code's approval prompts.

### Model

Per request the AI classifier (`MONOLITH_CLASSIFIER*`, [request-queue.md](../request-queue.md) "Models"), or the overrides `MONOLITH_MODEL` and an `@tier` prompt prefix; `MONOLITH_MODEL_FAST` / `_DEFAULT` / `_HEAVY` name the tiers (region "Model choice"). The coordinator passes it to the request's subagent. The coordinator's own model: `MONOLITH_COORDINATOR_MODEL`.

### Extra Claude Code flags

`$MONOLITH_CLAUDE_ARGS`, split on whitespace (no quoting), after `--model`, on the coordinator's (or the request's) command line. Default none. A `--model` in them replaces the chosen one (and the coordinator's).

### Workspace (i3 / sway)

The new terminal is moved to workspace `$MONOLITH_WORKSPACE` (default 10; empty = leave it where it opens) with `i3-msg` (`swaymsg` when `$SWAYSOCK` is set) and the criteria `[title="^<its title>$"]`.

- A detached `/bin/sh` helper retries every 0.1 s for up to 10 s until the window exists, so the editor never waits.
- A criteria `move container to workspace` doesn't switch the visible workspace; focus returns to the previously focused window.
- The window may still flash up for a moment where it opened: an i3 `for_window` rule on the title (terminals set it before the window is mapped) avoids that, and the IPC move then finds it already there.
- Other window managers: `i3-msg` finds no i3 (exit 1) and the helper stops at once.
- The same command also gives the window an i3 mark, `_monolith_coordinator` or `_monolith_<id>` (also with `MONOLITH_WORKSPACE` empty), for closing it later.

### Closing

Region "Closing request windows". The HUD list numbers the requests.

- **Coordinator mode:** K / SHIFT+K take finished requests off the list, CTRL+K n cancels request n (a running one on a second press: the coordinator stops its subagent), CTRL+SHIFT+K closes the coordinator's terminal.
- **Per-request mode:** K / SHIFT+K / CTRL+K n close a request's terminal.

Closing goes through the mark (`i3-msg [con_mark=...] kill`), else SIGTERM / SIGKILL to the terminal's process group. `MONOLITH_CLOSE_ON_EXIT=1` closes the finished requests' terminals (coordinator mode: the coordinator, if none of this run's requests is still open) when the editor quits.
