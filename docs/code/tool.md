# `tools/tool.h`: what the tools share

`tools/tool.h` is what every small program in `tools/` shares. It is header-only, on `nob.h` (`src/vendor/nob/nob.h`, vendored, only its `va_list` names changed). Overview of all the tools: [architecture.md §8](../architecture.md#8-tools).

## A program

Each program is one file, `tools/<name>.c`, that includes this header first:

```c
#include "tool.h"

int main(int argc, char **argv)
{
    ToolInit(argv, "todo", "todo");  // name, repo root, rebuild itself when its source changed
    ...
}
```

`./build.sh --tools [name...]` (`nob.c`, region "Tools") compiles `tools/<name>.c` into `build/tools/<name>` (`tools/run-sh.c` into `build/run-sh.o`). After that each program keeps itself current (`ToolInit`), so the docs and skills call `build/tools/<name>` directly.

## What is here, and why

| Piece | What it is for |
|---|---|
| `ToolInit` | The program's name for messages; the repo root, found from the program's own path (as `nob.c` finds it from `build/nob`); and the rebuild of the program when `tools/<name>.c`, this header or `nob.h` is newer than the binary. |
| `ToolInitBuilt` | `ToolInit` for a program that links more than its own file (`build/tools/library`: vendored SQLite, sqlite-vec, llama.cpp): its rebuild is `./build.sh --tools <name>` (output on stderr), which builds the stale vendored objects too, instead of a one-file compile; then the new binary runs in place of the old one as with `ToolInit`. If the build fails or leaves the binary older than its sources, the old binary keeps running with a warning. |
| `ToolSay` / `ToolErr` / `ToolDie` | Messages in the shell scripts' `<name>: ...` style, on stdout or stderr. |
| `ToolRun` / `ToolCapture` / `ToolExec` | Running a command given as an argument list (nob's `Nob_Cmd`: no shell, so no quoting or word-splitting surprises) and getting its exit status, its output, or handing the process over to it. nob's own `nob_cmd_run` only answers true / false, and the scripts these programs replace read exit codes (monolith's exit 1 for a broken map, `request-queue`'s 3 for a timeout). |
| `ToolTrash` | `gio trash <path>`, the repo's only way to delete (`CLAUDE.md`). Returns gio's exit status. As a safeguard for a caller that skipped `ToolNeedGio`, a missing `gio` is caught here too: it prints `<program>: gio not found in PATH: …` and returns 127 without exiting, so the caller's clean-up and report still run. |
| `ToolInPath` / `ToolNeed` / `ToolNeedGio` | `command -v name` (an empty `PATH` entry is the current folder); `ToolNeed(name, why)` exits 1 with `<program>: <name> not found in PATH: <why>` when it is missing; `ToolNeedGio()` is `ToolNeed` for `gio` with the install hint (`TOOL_GIO_PACKAGES`: Arch `glib2`, Debian / Ubuntu `libglib2.0-bin`). Every program that calls `ToolTrash` calls `ToolNeedGio` before doing any work, so a missing `gio` stops it at once instead of after minutes, with scratch files left behind. run-sh's refusal uses `ToolInPath` and `TOOL_GIO_PACKAGES` too. |
| `ToolProgram` | The path of another program in `build/tools/`, with the message that says how to build it. |
| `ToolEnv`, `ToolNewer`, `ToolMkdirP`, `ToolReadTrimmed` | Small equivalents of what the shell scripts used everywhere: `${VAR:-x}`, `[[ a -nt b ]]`, `mkdir -p`, `$(cat f)`. |

## Conventions

- Everything is `static inline`, so a program that doesn't use a function gets no unused-function warning.
- Every program is a single translation unit, so nothing here needs a separate implementation switch.
- Linux only, like the editor these tools serve (`fork` / `exec`, `/proc/self/exe`).
