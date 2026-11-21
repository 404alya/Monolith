# `tools/test-coordinator.c`: end-to-end test of the request queue

`tools/test-coordinator.c` is the end-to-end test of the request queue and the coordinator launch (`src/launcher.c`, `build/tools/request-queue`), with no Claude session and no window. It is built by `./build.sh --tools test-coordinator` into `build/tools/test-coordinator`. It replaced `tools/test-coordinator.sh` (2026-11-13) and runs the same cases with the same output. Overview of all the tools: [architecture.md §8](../architecture.md#8-tools).

## Usage

```
build/tools/test-coordinator [--keep]
```

`--keep` leaves the scratch folder; without it the folder goes to the trash. A run takes about a minute.

## How it stays away from everything real

| Setting | Effect |
|---|---|
| `MONOLITH_TERMINAL=build/tools/stub-terminal` | Runs the command it is given (`MONOLITH_STUB_RUN`) instead of opening a window, and counts "windows" (`MONOLITH_STUB_OPENED`). See [stub-terminal.md](stub-terminal.md). |
| `MONOLITH_CLAUDE=build/tools/stub-coordinator` | Stands in for `claude`: the coordinator's rules, no session. See [stub-coordinator.md](stub-coordinator.md). |
| `MONOLITH_REQUESTS_DIR=<scratch>/requests` | Bundles, queue and lock in a scratch folder (`build/coordinator-test/<stamp>/`), so the developer's own coordinator and `requests/` are never touched. |
| `MONOLITH_WORKSPACE=` (empty) | No i3 workspace switch. |
| `MONOLITH_CLASSIFIER=off` | No AI classifier: every request goes on the default tier at once (scheduling doesn't depend on the tier; the classifier's own cases run with `build/tools/stub-classifier` in the leak check). |

monolith runs only with `--shot` (a hidden window), `--new-map` and `--submit`.

It also sets `MONOLITH_LAUNCH=coordinator` and `MONOLITH_STUB_WORK=6` (each stub "subagent" works 6 s, long enough to see overlaps in the queue log), and unsets `MONOLITH_STUB_LOG`, `MONOLITH_CLAUDE_ARGS` and `MONOLITH_MODEL` so the developer's settings don't leak in.

## Checks

- Three submits open exactly one terminal.
- Two requests on the same map package never overlap.
- Requests on two packages run in parallel.
- A code request runs alone.
- The HUD side follows the state file.
- CTRL+K n (`--close-submitted`) cancels a running request.
- Killing the coordinator mid-run requeues untouched requests and reports touched ones on the next start.
- Without a coordinator, a queued request is cancelled by the editor itself.
- Without `gio` (case 5b: `PATH` is `<scratch>/no-gio-bin`, a symlink to every program of the test's own `PATH` except `gio`, plus `build/tools`), the editor starts no coordinator: its log says `monolith: coordinator not started: gio not found …`, no terminal opens (a separate `MONOLITH_STUB_OPENED` file stays empty) and the request stays `queued`.
- Per-request mode still opens one terminal per request, without a state file.
- 7, the usage limit (its own requests folder `requests-limit`, `MONOLITH_CLAUDE_FALLBACK=CLAUDE_CONFIG_DIR=<scratch>/fallback-config`, the stub coordinator limited on main 6 s after its start, `MONOLITH_STUB_WORK=12`, `MONOLITH_LIMIT_RETRY=14`, `CLAUDE_CONFIG_DIR` unset): a second coordinator starts, its log shows `account=fallback-config config=<that dir>` after `account=main config=-` and the `StopFailure` hook in `--settings`; the marker has the main line; the editor's log says it restarted the coordinator; the claimed-and-changed request is `failed` with `Interrupted: the account's usage limit was reached; restarted on fallback-config; …`; the claimed-and-unchanged one is requeued and done, the queued one done. After main's reset time the next start is on main. `request-queue limit --hook` reads `resets 3:45pm` into the marker's reset time.
- 8, the classifier (`--classify` with `build/tools/stub-classifier`, one requests folder per case): main limited → the fallback answers (`classifier account: fallback-config`, main marked); every account limited → the default tier with `the usage limit was reached on every account`; another failure → one run, no marker; no fallback → the old failure, no `MONOLITH_ACCOUNT`; two fallbacks → the second answers when main and the first are limited, the default tier when all three are; a submit → `request.json` `"classifierAccount": "fallback-config"` (format 7), the `Model:` line, its coordinator on the fallback.

Overlaps are read from the queue log's timestamps (the claim and the end of each request).

## Output and exit status

One line per step (`  ok: ...` / `  FAIL: ...`), ending with `test-coordinator: OK` (exit 0) or `test-coordinator: FAILED` (exit 1).

Exit 1 also when `build/linux/monolith` is missing (`./build.sh`), when `flock` (util-linux) or `gio` (Arch: `glib2`, Debian / Ubuntu: `libglib2.0-bin`; the test trashes its scratch folder) is not in `PATH` (`<name> not found in PATH: …`, checked before the scratch folder is made), or when `--new-map` fails. When a program of `build/tools/` is missing, the message says to run `./build.sh --tools <name>`.

## Called by

People and sessions after a change to the launcher, the queue or the coordinator's rules (the `engine-code` and `large-engine-work` skills, `CONTRIBUTING.md`).
