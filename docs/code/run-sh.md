# `tools/run-sh.c`: the guarded shell

`tools/run-sh.c` builds `build/run-sh.o`, the shell that every Claude session's commands run through. It behaves like `bash -c` (same stdin, stdout, stderr, exit status, signals and background jobs), but first looks at the command line and refuses the ordinary mistakes that break the guard rails in `CLAUDE.md`, with a message that says why and what to use instead.

It is a guard against mistakes, not a sandbox: the checks are simple on purpose (the developer's call; see [What it doesn't see](#what-it-doesnt-see)). It is written on `nob.h` with `tools/tool.h` ([tool.md](tool.md)). Overview of all the tools: [architecture.md §8](../architecture.md#8-tools).

## Modes

| Command | What it does |
|---|---|
| `run-sh.o -c '<command line>' [arg0 [args...]]` | Checks the line. When it passes, `exec`s `bash -c '<command line>' [arg0 [args...]]` (always bash; a `cd` in it doesn't carry over to the next command). A benchmark takes the benchmark lock first. |
| `run-sh.o --check '<command line>'` | Prints the decision (`run-sh: allowed` or the refusal line) and exits 0 or 126. Runs nothing and takes no lock. |
| `run-sh.o --hook [--rewrite unprompted\|all]` | The `PreToolUse` hook: the tool call's JSON on stdin, the answer on stdout (see [The hook](#the-hook)). |
| `run-sh.o --help` | The usage text. |

A refused line prints, on stderr,

```
run-sh: refused: <rule>: <the word that broke it>; <what to do instead>
```

and `-c` exits 126 without running anything.

## The log

Every decision, in all three modes, is appended to a log, one line each, tab-separated:

```
<local time> TAB <mode> TAB <decision> TAB <working directory> TAB <command>
```

In the command, `\`, tab and newline are escaped (`\\`, `\t`, `\n`). The log is `<RUN_SH_DIR>/log`, by default `build/run-sh/log`. It grows without limit.

## Environment

| Variable | Use |
|---|---|
| `RUN_SH_DIR` | The folder of the log and the benchmark lock (`<dir>/log`, `<dir>/bench.lock`). Default `<repo>/build/run-sh`; the unit test points it at its own folder. |
| `HOME` | For `~` and a bare `cd`. |
| `__GL_SYNC_TO_VBLANK` / `vblank_mode` | A benchmark needs one of them set to 0: in front of the command (also after `env`), exported earlier in the line, or in `run-sh.o`'s own environment. |

## Exit codes

| Mode | Exit codes |
|---|---|
| `-c` | the command's own status; 126 refused; 127 bash can't start |
| `--check` | 0 allowed; 126 refused |
| `--hook` | 0 with the answer; 2 when its input isn't the expected JSON (the call is then blocked) |
| any | 2 for a usage error |

## Callers and building

- The `PreToolUse` hook in `.claude/settings.json` runs `run-sh.o --hook` for every `Bash` and `Monitor` call.
- Built by `./build.sh --tools run-sh`; after that it rebuilds itself when `tools/run-sh.c`, `tools/tool.h` or `nob.h` is newer (`ToolInit`).
- `tests/unit/run_sh_test.c` (`./build.sh test run_sh`) checks every rule.

## How it reads a command

The code is in the regions "Splitting" and "Checking a segment".

1. **Segments and words.** The line is cut into segments at `;` `&` `|` `&&` `||`, newlines, `(` `)` `$(` and backticks, but not inside `'…'` or `"…"`. Each segment's words are split at blanks, with the quotes removed; a backslash keeps the next character; `# …` comments are dropped. `>` and `>>` (also `>file`, `2>`, `&>`) take the next word as a file the segment writes; `<`, `<<<` and `2>&1` are dropped; a here-document's body is skipped unread.
2. **The command word** is the first word after:
   - a keyword in front (`if` `then` `do` `else` `elif` `while` `until` `!` `{`),
   - any `NAME=value` words,
   - and one wrapper with its options: `sudo`, `env` (and its `NAME=value` words), `nice`, `nohup`, `timeout` (and its duration), `time`, `command`, `exec`, `xargs`.

   Names are compared by their basename.
3. **`cd <dir>`** sets the working directory of the segments after it (`cd` alone: `$HOME`; `cd -` or a directory with `$` etc.: unknown). The end of a `( … )` or `$( … )` restores the directory it started in.

## The rules

Each rule enforces a line of `CLAUDE.md`, and the refusal names the rule.

| Rule (as the refusal prints it) | `CLAUDE.md` line it enforces | What it refuses |
|---|---|---|
| `no deletion outside /tmp` | "Never delete with rm ... gio trash"; "temporary files you created under /tmp" | `rm` / `rmdir` / `unlink` / `shred` with an operand that doesn't resolve under `/tmp/` (never `/tmp` itself) or can't be checked (a word with `$` `` ` `` `*` `?` `[` `{`, or a relative path in an unknown directory); `xargs` feeding one of them; `find -delete` / `-exec rm…` with a root outside `/tmp/`; `git clean` without `-n` / `--dry-run`; `git rm` without `--cached`; `git worktree remove` outside `/tmp/`. `gio trash` is always allowed. |
| `no lost work` | "Never delete"; the work in the tree is the developer's | `git reset --hard`; `git checkout` with `--`, `.`, `-f` or `--force`; `git restore` without `--staged`; `git stash drop` or `clear`. |
| `no visible window` | "Never open a visible window" | monolith (`monolith`, `monolith.exe`, also as wine's program; `build.sh [--win] run`; `nob run`) without `--shot`, `--dump-boxes`, `--dump-scene`, `--dump-scene-diff`, `--dump-entities`, `--tri-report`, `--apply-edits`, `--inspect-model`, `--new-map`, `--export`, `--compile`, `--classify`, `--bake-surface` or `--bench --headless` (old-monolith builds lack monolith's own guard); the plain examples (`examples/build/linux/`, `win/`; their test builds `linux-test/` and `linux-lsan/` are fine); a word starting `MONOLITH_ALLOW_WINDOW=`, or `export` / `declare` / `typeset` / `readonly` / `local MONOLITH_ALLOW_WINDOW…`. |
| `never touch the screen` | "Never switch the developer's workspace, desktop or window focus" | `i3-msg` / `swaymsg` other than `-t get_*` without a payload; `xdotool`; `wmctrl`. |
| `benchmarks` | "only --bench --headless, one at a time ... __GL_SYNC_TO_VBLANK=0 and --note" | monolith's `--bench` without `--headless`, without vsync off (see [Environment](#environment)) or without `--note "<text>"`; an example test build's `--bench` without vsync off; a second benchmark while one runs (see below). |
| `stay in the repo` | "Work only inside the repo, plus your own scratch folders under `/tmp`" (a download folder is written only by `tools/fetch_asset.py`, which run-sh doesn't see) | an absolute path (or `~`) outside the repo and `/tmp` as a `>` / `>>` target, as the last operand of `cp` / `mv`, or as an operand of `tee` / `mkdir` / `touch`. `/dev/null`, `/dev/std*`, `/dev/tty` and `/dev/fd/N` are fine. |
| `leave .slop alone` | "`.slop/` is not project content" | any word or `>` / `>>` target naming a path with a `.slop` component (`.slop`, `.slop/x`, `<root>/.slop/x`). `git add .slop`, `--exclude-dir=.slop` and `rg -g '!.slop'` are fine; a recursive search of the repo that doesn't exclude it isn't seen. |
| `the guard's source` | reading the guard's source needs the developer's approval (`.claude/settings.json` asks for the Read tool) | any word or `>` / `>>` target whose last component is `run-sh.c`. Building (`./build.sh --tools run-sh`), the test and `git add` are fine. |

**The benchmark lock.** `-c` takes an exclusive `flock` on `<RUN_SH_DIR>/bench.lock` through a file descriptor without `O_CLOEXEC` before it `exec`s bash, so bash and every child hold the lock until the run ends (this also covers `bench-export-ab`). A second benchmark started meanwhile is refused. `--check` doesn't look at the lock.

## How under /tmp is decided

This is `Resolve` in the code. The path is made absolute against the working directory, then walked: each existing component is replaced by its realpath (links followed, `..` taken physically), and the rest is handled lexically. The last component isn't followed (`rm` removes a link, not its target) unless the path ends in `/`, or for `find` roots and `git worktree remove`. The result must start with `/tmp/`.

## What it doesn't see

These pass unchecked:

- commands inside nested `bash -c '…'` / `eval` strings, interpreters (`python3 -c`, `awk`), scripts and compiled programs; a command word that only exists after expansion (`$X`, `$(…)`);
- wrappers other than the listed ones, or a second wrapper (`sudo env rm`);
- here-documents and here-strings (`bash <<EOF`: the body is skipped), and text inside quotes (`"$(rm x)"`);
- destroying a file's content: `> file`, `truncate`, `cp /dev/null file`, `sed -i`;
- two-step deletions: `mv <repo file> /tmp/x` then `rm /tmp/x`, or a link made and deleted through;
- a `cd` that fails, or runs in a pipeline, after `||` or in the background (it still counts);
- windows of other programs (`xdg-open`, image viewers), or monolith copied and run under another name.

## The hook

`--hook` reads the tool call's JSON: `{"tool_name", "tool_input", "cwd", "permission_mode", ...}`. A `Bash` or `Monitor` call with a `"command"` is checked against the JSON's `"cwd"`; the answer depends on the result:

| Input | Answer |
|---|---|
| `Bash` or `Monitor` with a `"command"`, refused | In every permission mode: `{"hookSpecificOutput": {"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"run-sh: refused: ..."}}` |
| `Bash` or `Monitor` with a `"command"`, allowed | In the modes that never ask, `"updatedInput"` (see below). In the modes that ask, an empty answer: the command passes unchanged. |
| `Monitor` with `"ws"` and no command, and every other tool | Passed (empty answer). |
| Input that isn't the expected JSON | Exit 2 with the reason on stderr (this blocks the call). |

**The rewrite.** For an allowed command, `"updatedInput"` is `tool_input` with `"command"` replaced by `<repo>/build/run-sh.o -c '<command>'` (each `'` written as `'\''`), every other field copied as written. There is no `permissionDecision`, so the normal permission flow continues on the rewritten command. Claude Code 2.1.323 applies a bare `updatedInput` in every mode, also for `Monitor`, and matches allow rules against the rewritten command.

**When it rewrites.** Only in the modes that never ask: `bypassPermissions`, `dontAsk`, `auto`, or no or an unknown mode. In `default`, `acceptEdits` and `plan` an allowed command passes unchanged (an empty answer), because a rewritten one would miss the allow rules and every command would ask; `--rewrite all` rewrites in those modes too. A command that already is exactly `<this run-sh.o> -c '…'` passes unchanged.

**Which `run-sh.o`.** `<repo>` is this program's own repo: the hook runs `$CLAUDE_PROJECT_DIR/build/run-sh.o`, the main checkout's, even in a worktree agent.

## Other limits

- In the modes that only check, allowed commands run in the session's own shell: no benchmark lock, a `cd` carries over, and the command is logged once, as `--hook`.
- When `tools/run-sh.c` changed but doesn't compile, every call tries again (a second or two) and then runs the old build with a warning.
- The log grows without limit.
