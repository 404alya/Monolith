#!/usr/bin/env bash
# The developer's task list: TODO.md at the repo root (CONTRIBUTING.md, "Bugs and tasks").
#   tools/todo.sh list | add "<title>" "<description>" | rm <id>...
# Runs the prebuilt Simple-Todo binary (submodules/simple-todo/todo.o, x86-64 Linux) against
# that file. The file is fixed here: TODO_FILE from the environment is ignored on purpose.
# Claude sessions never write this list; theirs is tools/ai-todo.sh (.claude/TODO.md).
set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"
todo_file="$root/TODO.md"
todo_bin="$root/submodules/simple-todo/todo.o"

if [ ! -x "$todo_bin" ]; then
    echo "todo.sh: submodules/simple-todo is not checked out; run: git submodule update --init submodules/simple-todo" >&2
    exit 1
fi

# rm takes only task ids (digits): anything else could match a line the tool didn't write,
# such as the file's '#' header.
if [ "${1-}" = rm ]; then
    for id in "${@:2}"; do
        case "$id" in
            '' | *[!0-9]*) echo "todo.sh: rm: '$id' is not a task id (digits only)" >&2; exit 1 ;;
        esac
    done
fi

TODO_FILE="$todo_file" exec "$todo_bin" "$@"
