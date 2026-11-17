// tools/ai-todo.c -> build/tools/ai-todo list | add "<title>" "<description>" | rm <id>...: the AI's task
// list, .claude/TODO.md, through the prebuilt Simple-Todo program (submodules/simple-todo/todo.o). The list
// file is fixed (TODO_FILE is ignored), so an AI entry never lands in the developer's TODOs; `add` keeps
// entries short (60 / 120 characters, one line each), `rm` takes only task ids.
// Reference: docs/code/ai-todo.md

#include "tool.h"

// The limits of `add`, in characters.
#define TITLE_MAX 60
#define DESC_MAX 120

// The length of `s` in characters: its bytes minus the UTF-8 continuation bytes (0x80-0xBF). The same count
// the script made with `LC_ALL=C tr -d '\200-\277' | wc -c`, so it doesn't depend on the locale.
static long CountChars(const char *s)
{
    long n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) != 0x80) n++;
    }
    return n;
}

// Whether `s` has a line break in it (newline or carriage return): an entry is one line per field.
static bool HasLineBreak(const char *s)
{
    return strchr(s, '\n') != NULL || strchr(s, '\r') != NULL;
}

// Whether `s` is a task id: at least one character, every one a digit 0-9 (the script's case pattern
// `'' | *[!0-9]*` refused exactly the empty string and any string with a non-digit in it).
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
    // Step 1: the program's name for messages ("ai-todo"), the repo root from this program's own path, and
    // a rebuild of this program first when tools/ai-todo.c or tools/tool.h changed since it was built.
    ToolInit(argv, "ai-todo", "ai-todo");

    // Step 2: the two fixed paths. The list file is never taken from the environment
    // (docs/code/ai-todo.md#the-list-file).
    const char *todoFile = nob_temp_sprintf("%s/.claude/TODO.md", gToolRoot);
    const char *todoBin = nob_temp_sprintf("%s/submodules/simple-todo/todo.o", gToolRoot);

    // Step 3: the submodule must be checked out (the script's `-x` test).
    if (access(todoBin, X_OK) != 0) {
        ToolDie(1, "submodules/simple-todo is not checked out; run: git submodule update --init submodules/simple-todo");
    }

    const char *mode = argc > 1 ? argv[1] : "";
    if (strcmp(mode, "add") == 0 && argc == 4) {
        // Step 4a: add with a title and a description. One line each first (the title is looked at
        // first, then the description), then the lengths in characters (title first). A wrong argument
        // count skips all of this: todo.o reports it.
        for (int i = 2; i <= 3; i++) {
            if (HasLineBreak(argv[i])) ToolDie(1, "add: title and description must be one line each");
        }
        long n = CountChars(argv[2]);
        if (n > TITLE_MAX) ToolDie(1, "add: title is %ld characters, the limit is %d; nothing written", n, TITLE_MAX);
        n = CountChars(argv[3]);
        if (n > DESC_MAX) {
            ToolDie(1, "add: description is %ld characters, the limit is %d; point to the details instead; nothing written",
                    n, DESC_MAX);
        }
    } else if (strcmp(mode, "rm") == 0) {
        // Step 4b: rm takes only task ids. Every argument is checked before todo.o sees any of them, so a
        // bad id never removes the good ones that came before it.
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
