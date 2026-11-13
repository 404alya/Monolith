// tools/todo.c: the developer's task list, TODO.md at the repo root (CONTRIBUTING.md, "Bugs and tasks").
// Replaces tools/todo.sh; built by `./build.sh --tools todo` into build/tools/todo.
//
//   build/tools/todo list
//   build/tools/todo add "<title>" "<description>"
//   build/tools/todo rm <id>...
//
// What it does: runs the prebuilt Simple-Todo program (submodules/simple-todo/todo.o, x86-64 Linux) against
// the list file, after the one check this wrapper adds. Every mode and argument goes to todo.o unchanged
// (list, add, rm, and whatever else it understands, including a wrong argument count, which todo.o reports
// itself); `rm` first takes only task ids (digits only), because anything else could match a line the tool
// didn't write, such as the file's '#' header.
//
// The list file is fixed here (<repo root>/TODO.md). TODO_FILE from the environment is ignored on purpose:
// the program overwrites it with the fixed path before todo.o starts, so no caller can point it elsewhere.
// Claude sessions never write this list; theirs is tools/ai-todo.c (.claude/TODO.md).
//
// Environment: TODO_FILE (ignored, then set for todo.o); nothing else.
//
// Exit codes: todo.o's own when it runs (this process is replaced by it, so its output, exit status and
// signals are the caller's); 1 when submodules/simple-todo is not checked out, or when `rm` is given
// something that is not a task id. 126 / 127 when todo.o can't be started (as a shell reports it).
//
// Messages keep the text of the shell script, with the program's name "todo:" in place of "todo.sh:".
//
// Callers: the developer, by hand; documented in CONTRIBUTING.md and the task-tracker skill (sessions may
// read the list with `list`, never write it).

#include "tool.h"

// Whether `s` is a task id: at least one character, every one a digit 0-9. The script's case pattern
// `'' | *[!0-9]*` refused exactly the empty string and any string with a non-digit in it.
static bool IsTaskId(const char *s)
{
    if (*s == '\0') return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    // Step 1: the program's name for messages ("todo"), the repo root from this program's own path
    // (build/tools/todo -> the folder holding build.sh and nob.c), and a rebuild of this program first when
    // tools/todo.c or tools/tool.h changed since it was built.
    ToolInit(argv, "todo", "todo");

    // Step 2: the two fixed paths. The list file is never taken from the environment (see the header), so
    // the developer's list can only be written through this wrapper's own checks.
    const char *todoFile = nob_temp_sprintf("%s/TODO.md", gToolRoot);
    const char *todoBin = nob_temp_sprintf("%s/submodules/simple-todo/todo.o", gToolRoot);

    // Step 3: the submodule must be checked out (the script's `-x` test: the file exists and may be run).
    if (access(todoBin, X_OK) != 0) {
        ToolDie(1, "submodules/simple-todo is not checked out; run: git submodule update --init submodules/simple-todo");
    }

    // Step 4: `rm` takes only task ids. Every argument after "rm" is checked before todo.o sees any of
    // them, so a bad id never removes the good ones that came before it.
    if (argc > 1 && strcmp(argv[1], "rm") == 0) {
        for (int i = 2; i < argc; i++) {
            if (!IsTaskId(argv[i])) ToolDie(1, "rm: '%s' is not a task id (digits only)", argv[i]);
        }
    }

    // Step 5: hand the process over to todo.o with the fixed list file in its environment and the caller's
    // arguments as they came (the script's `TODO_FILE=... exec todo.o "$@"`).
    setenv("TODO_FILE", todoFile, 1);
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, todoBin);
    for (int i = 1; i < argc; i++) nob_cmd_append(&cmd, argv[i]);
    ToolExec(&cmd);
    return 1;  // ToolExec returns only after printing why it failed and exiting; not reached
}
