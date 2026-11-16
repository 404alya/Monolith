# `tools/stub-coordinator.c`: a fake request coordinator

`tools/stub-coordinator.c` is a fake request coordinator for tests: no Claude, no window. It is built by `./build.sh --tools stub-coordinator` into `build/tools/stub-coordinator`. It replaced `tools/stub-coordinator.sh` (2026-11-13) and behaves exactly like it. Overview of all the tools: [architecture.md §8](../architecture.md#8-tools).

## How it is used

The editor starts it in place of `claude`, with

```
MONOLITH_CLAUDE=build/tools/stub-coordinator
MONOLITH_TERMINAL=build/tools/stub-terminal
MONOLITH_STUB_RUN=<log>
```

(see `build/tools/test-coordinator`, [test-coordinator.md](test-coordinator.md)). It ignores its arguments.

## The scheduling rules

It applies the scheduling rules of the coordinator skill (`.claude/skills/coordinator/SKILL.md`, §4) through `build/tools/request-queue` ([request-queue.md](request-queue.md)), so the tests exercise the real queue:

- A queued request starts only if no claimed request uses the same map package, and an older queued one on that package goes first.
- A code request runs alone: it starts only when nothing else runs, and while it waits or runs nothing newer starts.
- At most 3 run at once.

These rules are kept in step with the skill (the `large-engine-work` skill, §5): when the skill's rules change, this program changes with them.

## Workers

A "subagent" is a child process that sleeps `$MONOLITH_STUB_WORK` seconds and writes `result.md`. Markers in the prompt:

| Marker | Effect |
|---|---|
| `[code]` | A code request. |
| `[edit]` | The worker appends to `<package>/stub-edit.txt` right after the claim (for the recovery test). |

A cancel file stops the worker and finishes the bundle as cancelled.

## Output

On stdout, which the stub terminal appends to its `MONOLITH_STUB_RUN` file: one line per event, with the time of day in milliseconds.

```
HH:MM:SS.mmm stub-coordinator: started (pid <pid>), args: <count>
(request-queue start's own output)
HH:MM:SS.mmm start <id> <package>[ [code]]
HH:MM:SS.mmm end <id>
HH:MM:SS.mmm cancelled <id>[ (queued)]
HH:MM:SS.mmm wait <id> (code request: waits until it is alone)
```

`request-queue`'s error messages (stderr) land there too.

## Environment

| Variable | Use |
|---|---|
| `MONOLITH_STUB_WORK` | Seconds a worker "works" (default 3; fractions allowed, like `sleep`'s). |
| `MONOLITH_REQUESTS_DIR` | The requests folder (also read by `request-queue`); default `<repo>/requests`. |

## Running and exit status

It runs until it is killed: the tests send SIGTERM to its process group, which ends its workers too. Exit status 1 when `build/tools/request-queue` is not built (with a message saying how to build it).
