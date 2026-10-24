# Design note: one coordinator session for all requests

Status: implemented (2026-10-24). Before this, every request opened its own terminal with its own `claude` process (`src/launcher.c`, region "Launching"). Several sessions could edit the same map at once; the only protection was the map hash check in `AI-MAP-TOOL.md` §6 step 1. Now one long-lived **coordinator** session takes requests from a queue on disk and hands each one to a **subagent**. The old mode stays behind `MONOLITH_LAUNCH=per-request`.

## 1. Parts

| Part | Where | Job |
|---|---|---|
| Game (submit, `--import`) | `src/launcher.c` | writes the bundle, then `state` = `queued` as its last file; starts the coordinator terminal if none holds the lock; shows the queue in the HUD |
| Coordinator lock wrapper | `src/launcher.c` (an `/bin/sh -c` script; Windows: a `9>` redirection) | holds `requests/.coordinator/lock` while the coordinator's `claude` runs |
| Queue tool | `tools/request-queue.sh` | every state change the coordinator makes (claim, finish, requeue, recover), the watcher (`wait`), the queue listing (`list`) |
| Coordinator | `claude` started by the game with `AI-COORDINATOR.md` | schedules, starts subagents, relays questions, finishes bundles. Reads only `request.json` metadata and the queue listing; never the images, never edits maps |
| Subagent | Agent tool inside the coordinator | does one request under `AI-MAP-TOOL.md`, returns a one-line status |
| Test doubles | `tools/stub-terminal.sh` (`MONOLITH_STUB_RUN`), `tools/stub-coordinator.sh`, `tools/test-coordinator.sh` | the same scheduling rules in shell, no Claude session |

## 2. Files on disk

**`requests/<id>/state`**: one line, `<state> <YYYY-MM-DDTHH:MM:SS> [detail]`. Readers use the first word. Written whole (temp file + rename where possible).

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

## 3. Handoff and waking (design point 1)

The game can't type into a running session, so the handoff is the file system:
1. The game writes the bundle, then `state` (`queued`).
2. If the lock is free (and no coordinator was started by this game in the last 20 s), it opens **one** terminal titled `monolith-claude: coordinator` running `claude [permission flags] --model <coordinator model> [MONOLITH_CLAUDE_ARGS] "<fixed prompt>"`, wrapped so the wrapper holds the lock; a second wrapper that finds the lock taken (two quick submits) waits 2 s for it, says so and exits. The fixed prompt names `AI-COORDINATOR.md` and the requests folder, never user text.
3. The coordinator keeps **`tools/request-queue.sh wait`** running in the background (Bash `run_in_background`). It polls the folder once a second in shell (no tokens) and **exits** when a `queued` bundle or a `cancel` file appears that it has not reported yet; it prints the events and the queue listing. The exit wakes the coordinator through the harness. Subagent completions wake it the same way. An idle queue costs nothing.
4. On every wake the coordinator first re-arms the watcher, then acts. Because `seen` records what was reported, a bundle queued between the watcher's exit and its restart still fires the new watcher.

## 4. Recovery (design point 2)

- A dead coordinator releases the lock; the next submit starts a new one.
- On startup the coordinator runs `request-queue.sh start`: empties `seen`, **recovers**, prints the queue. Only one coordinator can hold the lock, so every `claimed` / `question` bundle without `result.md` belongs to a dead one.
- **Fingerprint** (`claim.txt`, taken at the claim): `sha1sum` of every file in the request's map package, and one hash of the repo's code state (`git status --porcelain` + `git diff HEAD`, outside `maps/` and `requests/`).
  - Nothing changed since the claim: back to `queued` (it runs again from the start).
  - Something changed: `failed`, with `result.md` = `Interrupted: the coordinator stopped mid-run; files changed, check them` plus the list of changed files. It is **reported, not re-run**: a half-applied edit run a second time would apply twice. The coordinator also tells the developer in its terminal.
- Attribution is sound because of the scheduling rules: while a request is claimed nobody else may change its package, and the code hash only changes under a code request, which runs alone.

## 5. Scheduling rules (design point 3)

The coordinator starts a queued request (oldest first) only if all hold:
1. **No other claimed or question request uses the same map package** (`request.json` `map.package`; for imports the new package). An older queued request on the same package goes first (order per package is kept).
2. **No code request is running or waiting.** A code request (C, runtime, R3D, shaders outside a package, build) runs **alone**: it shares `build/`, the leak check and the GPU with everything else. It starts only when nothing else is claimed.
3. At most **3** subagents at once (one GPU renders every `--shot`).

Code is recognised two ways: the coordinator treats a prompt that plainly asks for engine work as code from the start; otherwise the subagent, as soon as it sees the request needs code, **stops before changing any code** and returns `NEEDS-CODE: <reason>`. The coordinator keeps it claimed (its package stays held), starts nothing new, waits until every other subagent has finished, then resumes it with `SendMessage` ("you are alone now; go ahead with the code change").

## 6. Models (design point 4)

- The game still chooses the tier per request (`LauncherChooseModel`, unchanged) and records it in `request.json` `model` (`tier`, `name`). The coordinator passes it as the subagent's `model`: a name containing `haiku` / `sonnet` / `opus` / `fable` maps to that alias; anything else means "omit `model`" (the subagent inherits the coordinator's). `request-queue.sh list` prints the mapped value (`agent-model=`).
- `@fast` / `@default` / `@heavy` (and `@haiku` / `@sonnet` / `@opus`), `MONOLITH_MODEL` and `MONOLITH_MODEL_FAST/_DEFAULT/_HEAVY` keep working unchanged: they decide `model.name`.
- The coordinator runs on one fixed model: **`MONOLITH_COORDINATOR_MODEL`**, default `sonnet` (scheduling and relaying need little). `MONOLITH_CLAUDE_ARGS` go on the coordinator's command line; a `--model` in them sets the coordinator's model and, as before, every request's `model.name`.

## 7. Ambiguous requests (design point 5)

Chosen: **ask through the coordinator.** The subagent writes the question to `requests/<id>/question.md` and returns `QUESTION`. The coordinator sets `state` to `question` (the HUD shows `[?] <slug>: <question>` so the developer in the game knows to look), asks in its own terminal (`[n] <slug>: <question>`), and when the developer answers resumes that subagent with `SendMessage`, the answer quoted as the developer's words. Other requests keep running meanwhile. The question text reaches no shell command (the coordinator reads the file; the answer goes through `SendMessage`). If the developer answers "skip" (or cancels in the game), the subagent writes `result.md` with `Not done: ...`.

Rejected alternative: `result.md` with `Not done: ambiguous` and a resubmit; the developer would lose the context and re-aim.

## 8. Isolation (design point 6)

Chosen: **no worktrees.** Map subagents edit the real repo in place, because hot reload reads the real package and the developer watches the change appear. Code subagents also work in place: they run alone (§5), the developer's `build/linux/monolith` is the binary to rebuild anyway, and a worktree costs an assimp rebuild of minutes (`working-method.md` §3).

## 9. HUD and keys (design point 7)

HUD rows: a coordinator line (`coordinator: running` / `starting` / `not running` / `not started: <error>`), then per request `[n] queued`, `[n] ... running`, `[n] [?] question`, `[n] [done]` / `[failed]` / `[cancelled]` with `result.md` line 1. **R** flies back to the last request that finished with a result, as before.

Keys in coordinator mode (per-request mode keeps the old meaning):

| Key | Coordinator mode |
|---|---|
| K | take the last finished request off the list |
| SHIFT+K | take every finished request off the list |
| CTRL+K, n | request [n]: finished → off the list; queued → cancel; running / question → press n again within 3 s → cancel (the coordinator stops its subagent and reports what changed) |
| CTRL+SHIFT+K | close the coordinator terminal (press again within 3 s while requests are queued or running; they are recovered by the next coordinator) |

The coordinator's window keeps the i3 / sway handling: moved to `MONOLITH_WORKSPACE`, mark `_monolith_coordinator` (fixed, so a restarted game can still close it through the WM). `MONOLITH_CLOSE_ON_EXIT=1` closes the coordinator when the game quits if none of the game's listed requests is still queued or running.

## 10. Fallback and Windows (design points 8, 9)

- `MONOLITH_LAUNCH=per-request` restores the old behaviour exactly (no `state` file, one terminal per request, the old K keys). Default `coordinator`. Without `flock` (util-linux) on Linux the game falls back to per-request and logs why.
- Windows: the same queue. The coordinator starts through the existing `cmd /c start ... cmd /k` path with ` 9>"<requests>\.coordinator\lock"` appended: cmd keeps the file open while `claude` runs; the game tests the lock by opening it with `_SH_DENYRW` (fails while cmd holds it). The scripts run in Claude Code's Git Bash. Compile-tested only; closing the window from the game is not possible there (as before).

## 11. Tests

- `MONOLITH_REQUESTS_DIR` (tests only) moves `requests/` (bundles, queue, lock) elsewhere, so tests never meet a real coordinator and the leak check no longer trashes real bundles queued during its run (the old hazard: its cleanup trashed every new folder in `requests/`).
- `MONOLITH_CLAUDE` replaces the `claude` program (tests: `tools/stub-coordinator.sh`).
- `tools/stub-terminal.sh` with `MONOLITH_STUB_RUN=<log>` runs the command after `-e` like a terminal would (no window) and appends one line per "window" to `$MONOLITH_STUB_OPENED`.
- `tools/test-coordinator.sh` proves: three submits open one terminal; same-package requests never overlap, different packages overlap; a code request runs alone; killing the coordinator mid-run requeues untouched requests and reports touched ones on the next start.
