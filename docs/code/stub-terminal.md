# `tools/stub-terminal.c`: a fake terminal

`tools/stub-terminal.c` is a fake terminal for tests of the request launcher (`src/launcher.c`). It is built by `./build.sh --tools stub-terminal` into `build/tools/stub-terminal`. It replaced `tools/stub-terminal.sh` (2026-11-13) and does exactly what the script did. Overview of all the tools: [architecture.md §8](../architecture.md#8-tools).

## How it is used

Set `MONOLITH_TERMINAL=build/tools/stub-terminal`. Instead of opening a window and starting Claude Code, it only records how it was called, and nothing appears on screen. The launcher treats it as an unknown terminal, so it passes the command to run after `-e`.

## Environment

Each variable is optional; unset or empty means off.

| Variable | Effect |
|---|---|
| `MONOLITH_STUB_LOG=<file>` | Writes the argv it was given to `<file>`, one argument per line. The file is replaced; no arguments gives one empty line, as the script's `printf` did. |
| `MONOLITH_STUB_OPENED=<file>` | Appends one line per "window" to `<file>`, `HH:MM:SS.<nanoseconds> <pid>`, to count them. The pid is the process group the launcher started (`setsid`), which the tests kill to stop a coordinator. |
| `MONOLITH_STUB_RUN=<file>` | Runs the command after `-e` like a terminal would, in place of this process (same pid), with stdout and stderr appended to `<file>` and stdin from `/dev/null`, and stays until it exits. With `MONOLITH_CLAUDE=build/tools/stub-coordinator` this is a coordinator without a window and without Claude (`build/tools/test-coordinator`). |

## Exit status

0, or the command's own with `MONOLITH_STUB_RUN` (127 / 126 when it can't be started; the message then goes into `<file>`, as the script's `exec` printed it there).

## Called by

The editor (`src/launcher.c`) in launcher tests and `build/tools/test-coordinator`; the leak check.
