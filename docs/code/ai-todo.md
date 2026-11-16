# `tools/ai-todo.c`: the AI's task list

`tools/ai-todo.c` manages the AI's task list, `.claude/TODO.md`: bugs and deferred work found by Claude sessions. It replaces `tools/ai-todo.sh` and is built by `./build.sh --tools ai-todo` into `build/tools/ai-todo`. The developer's own list is `tools/todo.c` ([todo.md](todo.md)). Overview of all the tools: [architecture.md §8](../architecture.md#8-tools).

## Usage

```
build/tools/ai-todo list
build/tools/ai-todo add "<title>" "<description>"
build/tools/ai-todo rm <id>...
```

## What it does

It runs the prebuilt Simple-Todo program (`submodules/simple-todo/todo.o`, x86-64 Linux) against the list file, after the checks this wrapper adds. Every mode and argument goes to `todo.o` unchanged (including a wrong argument count, which `todo.o` reports itself).

## The checks

| Mode | Check |
|---|---|
| `add` | Keeps entries short: a title of at most 60 characters and a one-line description of at most 120 that points to the details (file:line, a doc section, a request id). Title and description must each be one line (no newline, no carriage return). Characters are counted, not bytes, whatever the locale: UTF-8 continuation bytes (0x80-0xBF) are dropped before counting. Nothing is written when a check fails. The checks apply only to `add` with exactly a title and a description; any other argument count passes through and `todo.o` reports it. |
| `rm` | Takes only task ids (digits only): anything else could match a line the tool didn't write, such as the file's `#` header. |

## The list file

The list file is fixed: `<repo root>/.claude/TODO.md`. `TODO_FILE` from the environment is ignored on purpose: the program overwrites it with the fixed path before `todo.o` starts, so an AI entry can never land in the developer's `TODO.md` (`tools/todo.c`).

## Environment

| Variable | Use |
|---|---|
| `TODO_FILE` | Ignored, then set for `todo.o`. |

Nothing else.

## Exit codes

| Code | Meaning |
|---|---|
| `todo.o`'s own | When it runs: this process is replaced by it, so its output, exit status and signals are the caller's. |
| 1 | `submodules/simple-todo` is not checked out, or a check above fails. |
| 126 / 127 | `todo.o` can't be started (as a shell reports it). |

## Messages

Messages keep the text of the shell script, with the program's name `ai-todo:` in place of `ai-todo.sh:`.

## Callers

Claude sessions (the `task-tracker` skill), and by hand.
