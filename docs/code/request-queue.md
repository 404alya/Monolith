# `tools/request-queue.c`: the request queue on disk

`tools/request-queue.c` keeps the request queue on disk for the coordinator session (`.claude/skills/coordinator/SKILL.md`; the design is in [request-queue.md](../request-queue.md)). It is built by `./build.sh --tools request-queue` into `build/tools/request-queue`. It replaced `tools/request-queue.sh` (2026-11-13) and behaves exactly like it: the same subcommands, the same output byte for byte (the coordinator session and people read it), the same files written, the same exit codes. Overview of all the tools: [architecture.md §8](../architecture.md#8-tools).

The editor writes a bundle's `requests/<id>/state` = `queued` (and `requests/<id>/cancel` when the developer cancels); every later state change goes through this program, run by the coordinator only.

## Subcommands

All are `build/tools/request-queue <subcommand>`.

| Subcommand | What it does |
|---|---|
| `start` | Coordinator startup: empties the seen list, recovers bundles a dead coordinator had claimed (`recover` below), prints the queue. |
| `list` | The open bundles (state not `done` / `failed` / `cancelled`), oldest first, with their prompts; then `(N open)`. |
| `wait [--timeout s]` | Blocks (polling once a second) until a queued bundle or a cancel file appears that it has not reported before; prints `event: ...` lines, `--- queue` and the listing, and exits 0. The coordinator runs it in the background: its exit wakes the session. With `--timeout`: prints `event: none (timeout)` and exits 3 after `s` seconds without an event. |
| `claim <id>` | `queued` → `claimed`; writes `claim.txt` (the fingerprint). |
| `set <id> question\|claimed` | Sets the state. |
| `requeue <id>` | `claimed` / `question` → `queued`. |
| `changed <id>` | The files changed since the claim; exit 1 if any. |
| `finish <id> done\|failed\|cancelled [summary words]` | `done` needs `result.md`; `failed` / `cancelled` write one (summary + what changed) if there is none. |
| `recover` | (Part of `start`.) Claimed bundles without `result.md`: nothing changed since the claim → queued again; else failed and reported (with the usage-limit wording `Interrupted: the account's usage limit was reached; restarted on <MONOLITH_ACCOUNT>; …` when the newest `.coordinator/limit` line is newer than the claim). |
| `limit [--hook]` | The account's usage limit: appends `<time> account=<MONOLITH_ACCOUNT> reset=<time\|unknown> error=<word>` to `.coordinator/limit`, logs it, says what happens next. `--hook`: from Claude Code's `StopFailure` hook, its JSON on stdin (`error`, the error word, `[a-z_]` only, else `unknown`; the reset time from `resets …` in `last_assistant_message` or `error_details`). Without: the coordinator reports a subagent stopped at the limit (`error=subagent`). Without `MONOLITH_ACCOUNT` (no fallback configured): exit 1. |
| `switch` | The developer's manual switch: as `limit` with `error=manual`. Exit 1 without `MONOLITH_ACCOUNT`. |
| anything else (or nothing) | The usage text on stdout, exit 2. |

## Files

They live in the requests folder: `$MONOLITH_REQUESTS_DIR` (tests) or `<repo>/requests`.

| File | Content |
|---|---|
| `<id>/state` | One line, `<state> <YYYY-MM-DDTHH:MM:SS> <writer>`; readers use the first word. Bundles without a state file (older ones, per-request mode, one still being written) are never touched. |
| `<id>/claim.txt` | The fingerprint taken at the claim: `package <path>`, one `file <sha1>  ./<path>` line per file of the map package (`sha1sum`'s format, `sort -z` order), `code <sha1 of the code state>`. |
| `<id>/result.md` | Written here only by `finish failed` / `cancelled` when the subagent left none. |
| `.coordinator/` | The coordinator's own files: `lock` (held by the editor's wrapper, not touched here), `pid` (the wrapper's), `seen` (the events `wait` has reported in this coordinator's life), `log` (one line per event, with milliseconds), `limit` (the usage-limit marker, appended by `limit` / `switch` and the editor) and `account` (the editor's: the running coordinator's account). |

It never deletes anything (no `rm`): every file is written whole into `<file>.tmp` and renamed over the old one.

## Environment

| Variable | Use |
|---|---|
| `MONOLITH_REQUESTS_DIR` | The requests folder (tests); default `<repo>/requests`. |
| `MONOLITH_ACCOUNT` | The session's account label (set by the editor while `MONOLITH_CLAUDE_FALLBACK` is set): the account `limit` / `switch` mark, and the one `recover` names. |
| `LC_ALL` / `LC_CTYPE` / `LC_COLLATE` | As for the script's tools: the prompt is cut at 300 characters of the locale (bash's `${#prompt}`), bundles are listed in the locale's order (bash's glob), the package's files are sorted like `sort -z` and the changes like `sort -u`. |

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Done. |
| 1 | `changed` found changes (or there is no `claim.txt`); `limit` / `switch` without `MONOLITH_ACCOUNT`. |
| 2 | A bad id, a bundle in the wrong state, a bad argument (the script's `Die`), or the usage text. |
| 3 | `wait --timeout` ran out. |

## Callers

- The coordinator session (the `coordinator` skill); `src/launcher.c`'s start prompt names the skill.
- Claude Code's `StopFailure` hook (`limit --hook`), added by the editor with `--settings` while a fallback is configured; the developer (`! build/tools/request-queue switch`).
- `build/tools/stub-coordinator` (the coordinator's rules without Claude, for `build/tools/test-coordinator`).

## External programs

They are run exactly as the script ran them, so the fingerprint's bytes stay the same:

- `sha1sum`, for the package's files and the untracked code files: its output format, escapes included, is what `claim.txt` holds;
- `git` (`status`, `diff`, `ls-files`), for the code state.

The final hash of the code state is computed in the program (`Sha1`) from the same bytes the script piped into `sha1sum`.
