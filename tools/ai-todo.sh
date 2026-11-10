#!/usr/bin/env bash
# The AI's task list: .claude/TODO.md (bugs and deferred work found by Claude sessions).
#   tools/ai-todo.sh list | add "<title>" "<description>" | rm <id>...
# Runs the prebuilt Simple-Todo binary (submodules/simple-todo/todo.o, x86-64 Linux) against
# that file. The file is fixed here: TODO_FILE from the environment is ignored on purpose, so
# an AI entry can never land in the developer's TODO.md (tools/todo.sh).
# add keeps entries short: a title of at most 60 characters and a one-line description of at
# most 120 that points to the details (file:line, a doc section, a request id).
set -eu

TITLE_MAX=60
DESC_MAX=120

root="$(cd "$(dirname "$0")/.." && pwd)"
todo_file="$root/.claude/TODO.md"
todo_bin="$root/submodules/simple-todo/todo.o"

if [ ! -x "$todo_bin" ]; then
    echo "ai-todo.sh: submodules/simple-todo is not checked out; run: git submodule update --init submodules/simple-todo" >&2
    exit 1
fi

# Characters, not bytes, whatever the locale: UTF-8 continuation bytes (0x80-0xBF) are dropped
# before counting.
chars() { printf '%s' "$1" | LC_ALL=C tr -d '\200-\277' | wc -c | tr -d ' '; }

case "${1-}" in
add)
    if [ "$#" -eq 3 ]; then
        for field in "$2" "$3"; do
            case "$field" in
                *$'\n'* | *$'\r'*) echo "ai-todo.sh: add: title and description must be one line each" >&2; exit 1 ;;
            esac
        done
        n="$(chars "$2")"
        if [ "$n" -gt "$TITLE_MAX" ]; then
            echo "ai-todo.sh: add: title is $n characters, the limit is $TITLE_MAX; nothing written" >&2
            exit 1
        fi
        n="$(chars "$3")"
        if [ "$n" -gt "$DESC_MAX" ]; then
            echo "ai-todo.sh: add: description is $n characters, the limit is $DESC_MAX; point to the details instead; nothing written" >&2
            exit 1
        fi
    fi
    ;;  # a wrong argument count is reported by todo.o
rm)
    # Only task ids (digits): anything else could match a line the tool didn't write, such as
    # the file's '#' header.
    for id in "${@:2}"; do
        case "$id" in
            '' | *[!0-9]*) echo "ai-todo.sh: rm: '$id' is not a task id (digits only)" >&2; exit 1 ;;
        esac
    done
    ;;
esac

TODO_FILE="$todo_file" exec "$todo_bin" "$@"
