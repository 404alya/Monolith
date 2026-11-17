// tools/todo.c -> build/tools/todo list | add "<title>" "<description>" | rm <id>...: the developer's task
// list, TODOs at the repo root, through the prebuilt Simple-Todo program (submodules/simple-todo/todo.o).
// The list file is fixed (TODO_FILE is ignored); `rm` takes only task ids. Claude sessions never write
// this list (theirs is tools/ai-todo.c).
// Reference: docs/code/todo.md

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

    // Step 2: the two fixed paths. The list file is never taken from the environment
    // (docs/code/todo.md#the-list-file), so the developer's list can only be written through this wrapper's own
    // checks.
    const char *todoFile = nob_temp_sprintf("%s/TODOs", gToolRoot);
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
