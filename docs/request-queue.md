# Request queue and coordinator

Every request goes through a queue on disk, worked through by one long-lived Claude Code session, the **coordinator**, which hands each request to a subagent. `MONOLITH_LAUNCH=per-request` switches to one terminal and session per request instead. The coordinator's own instructions are `.claude/skills/coordinator/SKILL.md`.

## 1. Parts

| Part | Where | Job |
|---|---|---|
| Game (submit, `--import`) | `src/launcher.c` | writes the bundle, then `state` = `queued` as its last file; starts the coordinator terminal if none holds the lock; shows the queue in the HUD |
| Coordinator lock wrapper | `src/launcher.c` (an `/bin/sh -c` script; Windows: a `9>` redirection) | holds `requests/.coordinator/lock` while the coordinator's `claude` runs |
| Queue tool | `tools/request-queue.sh` | every state change the coordinator makes (claim, finish, requeue, recover), the watcher (`wait`), the queue listing (`list`) |
| Coordinator | `claude` started by the game with `.claude/skills/coordinator/SKILL.md` | schedules, starts subagents, relays questions, finishes bundles. Reads only the queue listing (built from `request.json` metadata), line 1 of a `question.md` and the subagents' one-line replies; never `request.md`, the images or the maps, never edits anything |
| Subagent | Agent tool inside the coordinator | does one request under `.claude/skills/map-request/SKILL.md`, returns a one-line status |
| Test doubles | `tools/stub-terminal.sh` (`MONOLITH_STUB_RUN`), `tools/stub-coordinator.sh`, `tools/test-coordinator.sh` | the same scheduling rules in shell, no Claude session |

## 2. Files on disk

**`requests/<id>/state`**: one line, `<state> <YYYY-MM-DDTHH:MM:SS> <writer>` (`game` or `coordinator`). Readers use the first word. Written whole (temp file + rename where possible).

| State | Written by | Meaning |
|---|---|---|
| `queued` | game / `--import` | the bundle is complete and waits for the coordinator. **A bundle without `state` is never picked up** (older bundles, per-request mode, a bundle still being written) |
| `claimed` | `request-queue.sh claim` | a subagent has it. `claim.txt` next to it holds the fingerprint taken at the claim (§4) |
| `question` | `request-queue.sh set <id> question` | the subagent asked something (`question.md`, line 1 = the question); the coordinator asks the developer in its terminal. Back to `claimed` when answered |
| `done` | `request-queue.sh finish <id> done` | the subagent finished; `result.md` exists (required) |
| `failed` | `finish <id> failed` / `recover` | not done; `result.md` says why (written by the tool when the subagent left none) |
| `cancelled` | `finish <id> cancelled`; the game, only when no coordinator holds the lock | the developer cancelled it; `result.md` says whether anything had changed |

Transitions: `queued → claimed → (question ↔ claimed) → done | failed | cancelled`, and `claimed → queued` (requeue after a restart). After `queued` **only the coordinator writes `state`**: the game asks for a cancel by creating **`requests/<id>/cancel`** (one line, the time), so game and coordinator never race on the same file. The single exception: when no coordinator holds the lock, the game turns a `queued` bundle with a cancel straight into `cancelled` (+ `result.md`) itself, since nobody else can.

**`requests/.coordinator/`** (dot folder: invisible to `ls`-based tools such as the leak check's cleanup):
- `lock`: held (Linux: `flock(2)`) by the wrapper around the coordinator's `claude` for as long as it runs. "Is a coordinator running?" = "is the lock held?"; the kernel drops it when the process dies, however it dies.
- `seen`: the events the watcher has reported in this coordinator's life (`queued <id>`, `cancel <id>`); `start` empties it.
- `log`: append-only, one line per event: `<time with ms> <event> <id> [package]`. The tests read their timestamps from it; the developer can `tail -f` it.

## 3. Handoff and waking

The game can't type into a running session, so the handoff is the file system:
1. The game writes the bundle, then `state` (`queued`).
2. If the lock is free (and no coordinator was started by this game in the last 20 s), it opens **one** terminal titled `monolith-claude: coordinator` running `claude [permission flags] --model <coordinator model> [MONOLITH_CLAUDE_ARGS] "<fixed prompt>"`, wrapped so the wrapper holds the lock; a second wrapper that finds the lock taken (two quick submits) waits 2 s for it, says so and exits. The fixed prompt names `.claude/skills/coordinator/SKILL.md` and the requests folder, never user text.
3. The coordinator keeps **`tools/request-queue.sh wait`** running in the background (Bash `run_in_background`). It polls the folder once a second in shell (no tokens) and **exits** when a `queued` bundle or a `cancel` file appears that it has not reported yet; it prints the events and the queue listing. The exit wakes the coordinator through the harness. Subagent completions wake it the same way. An idle queue costs nothing.
4. On every wake the coordinator first re-arms the watcher, then acts. Because `seen` records what was reported, a bundle queued between the watcher's exit and its restart still fires the new watcher.

## 4. Recovery

- A dead coordinator releases the lock; the next submit starts a new one.
- On startup the coordinator runs `request-queue.sh start`: empties `seen`, **recovers**, prints the queue. Only one coordinator can hold the lock, so every `claimed` / `question` bundle belongs to a dead one. One that already has a `result.md` is finished as `done`.
- **Fingerprint** (`claim.txt`, taken at the claim): `sha1sum` of every file in the request's map package, and one hash of the repo's code state (`git status --porcelain`, `git diff HEAD` and the sha1 of untracked files, outside `maps/` and `requests/`).
  - Nothing changed since the claim: back to `queued` (it runs again from the start).
  - Something changed: `failed`, with `result.md` = `Interrupted: the coordinator stopped mid-run; files changed, check them` plus the list of changed files. It is **reported, not re-run**: a half-applied edit run a second time would apply twice. The coordinator also tells the developer in its terminal.
- Attribution is sound because of the scheduling rules: while a request is claimed nobody else may change its package, and the code hash only changes under a code request, which runs alone.

## 5. Scheduling

A queued request starts (oldest first) only when no other running request uses the same map package, no code request is running or waiting (code requests share `build/`, the leak check and the GPU, so they run alone), and fewer than 3 subagents run (one GPU renders every `--shot`). A request that turns out to need code stops before changing any, and resumes once it runs alone.

### Models

- The game chooses the tier per request (`LauncherChooseModel`) and records it in `request.json` `model` (`tier`, `name`). The coordinator passes it as the subagent's `model`: a name containing `haiku` / `sonnet` / `opus` / `fable` maps to that alias; anything else means "omit `model`" (the subagent inherits the coordinator's). `request-queue.sh list` prints the mapped value (`agent-model=`).
- `@fast` / `@default` / `@heavy` (and `@haiku` / `@sonnet` / `@opus`), `MONOLITH_MODEL` and `MONOLITH_MODEL_FAST/_DEFAULT/_HEAVY` decide `model.name`.
- The coordinator runs on one fixed model: **`MONOLITH_COORDINATOR_MODEL`**, default `sonnet` (scheduling and relaying need little). `MONOLITH_CLAUDE_ARGS` go on the coordinator's command line; a `--model` in them sets the coordinator's model and, every request's `model.name`.

## 6. Closing and cancelling

A cancel writes `requests/<id>/cancel`; the coordinator stops the subagent and the request's `result.md` lists what had changed. With no coordinator running the game cancels a queued request itself. On i3 / sway the game marks the coordinator's terminal `_monolith_coordinator` (per-request mode: each terminal `_monolith_<id>`) and closes it with `i3-msg '[con_mark=…] kill'` (that window only, no focus change); if the process is still alive 3 s later or there is no mark, it sends SIGTERM to the process group (started with `setsid`) and SIGKILL after 3 s, only while it is the game's own child. kitty `--single-instance` and wezterm mux exit at once, so only the i3 route reaches them. Windows: the entry only leaves the list. `MONOLITH_CLOSE_ON_EXIT=1` closes the coordinator when the game quits if none of its requests is open.

- `MONOLITH_LAUNCH=per-request` starts one terminal and session per request instead (no `state` file; K / Ctrl+K close those terminals). Default `coordinator`. Without `flock` (util-linux) on Linux the game falls back to per-request and logs why.
- Windows: the same queue. The coordinator starts through the `cmd /c start ... cmd /k` path with ` 9>"<requests>\.coordinator\lock"` appended: cmd keeps the file open while `claude` runs; the game tests the lock by opening it with `_SH_DENYRW` (fails while cmd holds it). The scripts run in Claude Code's Git Bash. Compile-tested only; closing the window from the game is not possible there (as before).

## 7. Tests

- `MONOLITH_REQUESTS_DIR` (tests only) moves `requests/` (bundles, queue, lock) elsewhere, so tests never meet a real coordinator and the leak check never touches real bundles.
- `MONOLITH_CLAUDE` replaces the `claude` program (tests: `tools/stub-coordinator.sh`).
- `tools/stub-terminal.sh` with `MONOLITH_STUB_RUN=<log>` runs the command after `-e` like a terminal would (no window) and appends one line per "window" to `$MONOLITH_STUB_OPENED`.
- `tools/test-coordinator.sh` proves: three submits open one terminal; same-package requests never overlap, different packages overlap; a code request runs alone; killing the coordinator mid-run requeues untouched requests and reports touched ones on the next start.
