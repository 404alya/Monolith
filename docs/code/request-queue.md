# `tools/request-queue.c`: the request queue on disk

`tools/request-queue.c` keeps the request queue on disk for the coordinator session (`.claude/skills/coordinator/SKILL.md`; the design is in [request-queue.md](../request-queue.md)). It is built by `./build.sh --tools request-queue` into `build/tools/request-queue`. It replaced `tools/request-queue.sh` (2026-11-13) and behaves exactly like it: the same subcommands, the same output byte for byte (the coordinator session and people read it), the same files written, the same exit codes. Overview of all the tools: [architecture.md §8](../architecture.md#8-tools).

The editor writes a bundle's `requests/<id>/state` = `queued` (and `requests/<id>/cancel` when the developer cancels); every later state change goes through this program, run by the coordinator only.

## Subcommands

All are `build/tools/request-queue <subcommand>`.

| Subcommand | What it does |
|---|---|
| `start` | Coordinator startup: empties the seen list, recovers bundles a dead coordinator had claimed (`recover` below), prints the queue. |
| `list [--all]` | The open bundles (state not `done` / `failed` / `cancelled`), oldest first, with their prompts; then `(N open)`. Each row ends `commit=<short sha>\|none\|failed patch=map\|map+code\|code\|none` (from `commit.txt`; `none` until the bundle finishes). `--all`: the finished bundles too, then `(N open, M finished)`. |
| `wait [--timeout s]` | Blocks (polling once a second) until a queued bundle or a cancel file appears that it has not reported before; prints `event: ...` lines, `--- queue` and the listing, and exits 0. The coordinator runs it in the background: its exit wakes the session. With `--timeout`: prints `event: none (timeout)` and exits 3 after `s` seconds without an event. |
| `claim <id>` | `queued` → `claimed`. First the package's repository ([request-queue.md §4a](../request-queue.md#4a-the-maps-history-one-commit-and-one-patch-per-request)): a plain folder gets `git init`, the package `.gitignore` and a first commit; uncommitted hand edits are committed as `Hand edits before request <slug>`; each step is a `<id>: map history: …` line (also in the log). Then `claim.txt`: the fingerprint and the baseline lines. |
| `set <id> question\|claimed` | Sets the state. |
| `requeue <id>` | `claimed` / `question` → `queued`. |
| `changed <id>` | The files changed since the claim; exit 1 if any. |
| `finish <id> done\|failed\|cancelled [summary words]` | `done` needs `result.md`; `failed` / `cancelled` write one (summary + what changed) if there is none. Then the map's history: the commit in the package's repository (`State:` the final state, `interrupted` from `recover`), `map.patch`, `code.patch`, `commit.txt`, and the line `<id>: map history: commit=… patch=…` before the state line. A bundle without a `map-repo` line in `claim.txt` (claimed by an older `request-queue`) gets no commit and no patch, and a line saying so. |
| `recover` | (Part of `start`.) Claimed bundles without `result.md`: nothing changed since the claim → queued again; else failed and reported (with the usage-limit wording `Interrupted: the account's usage limit was reached; restarted on <MONOLITH_ACCOUNT>; …` when the newest `.coordinator/limit` line is newer than the claim). |
| `limit [--hook]` | The account's usage limit: appends `<time> account=<MONOLITH_ACCOUNT> reset=<time\|unknown> error=<word>` to `.coordinator/limit`, logs it, says what happens next. `--hook`: from Claude Code's `StopFailure` hook, its JSON on stdin (`error`, the error word, `[a-z_]` only, else `unknown`; the reset time from `resets …` in `last_assistant_message` or `error_details`). Without: the coordinator reports a subagent stopped at the limit (`error=subagent`). Without `MONOLITH_ACCOUNT` (no fallback configured): exit 1. |
| `switch` | The developer's manual switch: as `limit` with `error=manual`. Exit 1 without `MONOLITH_ACCOUNT`. |
| anything else (or nothing) | The usage text on stdout, exit 2. |

## Files

They live in the requests folder: `$MONOLITH_REQUESTS_DIR` (tests) or `<repo>/requests`.

| File | Content |
|---|---|
| `<id>/state` | One line, `<state> <YYYY-MM-DDTHH:MM:SS> <writer>`; readers use the first word. Bundles without a state file (older ones, per-request mode, one still being written) are never touched. |
| `<id>/claim.txt` | The fingerprint taken at the claim: `package <path>`, one `file <sha1>  ./<path>` line per file of the map package (`sha1sum`'s format, `sort -z` order), `code <sha1 of the code state>`. After them the history's baseline: `map-repo own\|own-uncommitted <why>\|tracked <repo root>\|none: <why>`, `base-map <commit>` or `base-map-tree <tree>`, `base-code <tree>`. `changed` and `recover` read only the fingerprint lines. |
| `<id>/map.patch`, `<id>/code.patch` | The request's change: `git format-patch --binary` of its commit (a plain `diff-tree` diff without a commit; `no changes` when nothing changed), and the code change outside `maps/`, `requests/` and `.slop/` (only when there is one). |
| `<id>/commit.txt` | `commit <short sha>` \| `commit none` \| `commit failed: <why>`, then `patch map` \| `map+code` \| `code` \| `none`. Written at the end. |
| `<id>/commit-msg.txt`, `<id>/git.log` | The commit message (given to `git commit -F`), git's own output. |
| `<id>/patch-index`, `<id>/map-index` | The private git indexes the code tree and a patch-only package's tree are built in (`GIT_INDEX_FILE`); overwritten, never deleted. |
| `<id>/result.md` | Written here only by `finish failed` / `cancelled` when the subagent left none. |
| `.coordinator/` | The coordinator's own files: `lock` (held by the editor's wrapper, not touched here), `pid` (the wrapper's), `seen` (the events `wait` has reported in this coordinator's life), `log` (one line per event, with milliseconds), `limit` (the usage-limit marker, appended by `limit` / `switch` and the editor) and `account` (the editor's: the running coordinator's account). |

It never deletes anything (no `rm`): every file is written whole into `<file>.tmp` and renamed over the old one.

## Environment

| Variable | Use |
|---|---|
| `MONOLITH_REQUESTS_DIR` | The requests folder (tests); default `<repo>/requests`. |
| `MONOLITH_CODE_REPO` | Tests only: the repository `base-code` and `code.patch` are taken from; default the repo root. |
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

`git` also keeps the map's history ([request-queue.md §4a](../request-queue.md#4a-the-maps-history-one-commit-and-one-patch-per-request)), always as an argument list (`git -C <dir> …`, no shell; the commit message as a file, `-F`): `init`, `status --porcelain`, `add -A`, `config` (`user.name` / `user.email`, to word a failed commit), `commit` (with `-c user.useConfigOnly=true`), `reset -q` (only to unstage after a failed commit), `rev-parse`, `ls-files`, `check-ignore`, `format-patch`, `diff-tree`, and `read-tree` / `add` / `write-tree` with `GIT_INDEX_FILE` set to a private index in the bundle. It never commits, stages or resets in the main repo (a package tracked by it gets a patch only), and never pushes. Without git in `PATH` the history is skipped: `claim.txt` gets `map-repo none: git not found`, the claim prints `<id>: map history: none: git not found (no commit, no patch; install git)` and the finish `commit=none (none: git not found) patch=none`.

The final hash of the code state is computed in the program (`Sha1`) from the same bytes the script piped into `sha1sum`.
