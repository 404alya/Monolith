# `tools/todo.c`: the developer's task list

`tools/todo.c` manages the developer's task list, `TODO.md` at the repo root (`CONTRIBUTING.md`, "Bugs and tasks"). It replaces `tools/todo.sh` and is built by `./build.sh --tools todo` into `build/tools/todo`. Overview of all the tools: [architecture.md §8](../architecture.md#8-tools).

## Usage

```
build/tools/todo list
build/tools/todo add "<title>" "<description>"
build/tools/todo rm <id>...
```

## What it does

It runs the prebuilt Simple-Todo program (`submodules/simple-todo/todo.o`, x86-64 Linux) against the list file, after the one check this wrapper adds. Every mode and argument goes to `todo.o` unchanged (`list`, `add`, `rm`, and whatever else it understands, including a wrong argument count, which `todo.o` reports itself).

**The check:** `rm` first takes only task ids (digits only), because anything else could match a line the tool didn't write, such as the file's `#` header.

## The list file

The list file is fixed: `<repo root>/TODO.md`. `TODO_FILE` from the environment is ignored on purpose: the program overwrites it with the fixed path before `todo.o` starts, so no caller can point it elsewhere.

Claude sessions never write this list; theirs is `tools/ai-todo.c` (`.claude/TODO.md`, [ai-todo.md](ai-todo.md)).

## Environment

| Variable | Use |
|---|---|
| `TODO_FILE` | Ignored, then set for `todo.o`. |

Nothing else.

## Exit codes

| Code | Meaning |
|---|---|
| `todo.o`'s own | When it runs: this process is replaced by it, so its output, exit status and signals are the caller's. |
| 1 | `submodules/simple-todo` is not checked out, or `rm` is given something that is not a task id. |
| 126 / 127 | `todo.o` can't be started (as a shell reports it). |

## Messages

Messages keep the text of the shell script, with the program's name `todo:` in place of `todo.sh:`.

## Callers

The developer, by hand; documented in `CONTRIBUTING.md` and the `task-tracker` skill (sessions may read the list with `list`, never write it).
