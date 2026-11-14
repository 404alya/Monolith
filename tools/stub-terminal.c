// tools/stub-terminal.c: a fake terminal for tests of the request launcher (src/launcher.c). Built by
// `./build.sh --tools stub-terminal` into build/tools/stub-terminal; it replaced tools/stub-terminal.sh
// (2026-11-13) and does exactly what the script did.
//
// Use it as MONOLITH_TERMINAL=build/tools/stub-terminal: instead of opening a window and starting Claude Code
// it only records how it was called, and nothing appears on screen. The launcher treats it as an unknown
// terminal, so it passes the command to run after "-e".
//
// Environment (each one is optional; unset or empty = off):
//   MONOLITH_STUB_LOG=<file>      writes the argv it was given to <file>, one argument per line (the file is
//                                 replaced; no arguments = one empty line, as the script's printf did)
//   MONOLITH_STUB_OPENED=<file>   appends one line per "window" to <file>: "HH:MM:SS.<nanoseconds> <pid>", to
//                                 count them; the pid is the process group the launcher started (setsid), which
//                                 the tests kill to stop a coordinator
//   MONOLITH_STUB_RUN=<file>      runs the command after -e like a terminal would, in place of this process (same
//                                 pid), stdout and stderr appended to <file>, stdin from /dev/null, and stays until
//                                 it exits. With MONOLITH_CLAUDE=build/tools/stub-coordinator this is a coordinator
//                                 without a window and without Claude (build/tools/test-coordinator)
//
// Exit status: 0, or the command's own with MONOLITH_STUB_RUN (127 / 126 when it can't be started; the message
// then goes into <file>, as the script's `exec` printed it there).
//
// Called by: the editor (src/launcher.c) in launcher tests and build/tools/test-coordinator; the leak check.

#include "tool.h"

// The env variable's value when it is set and not empty, else NULL (the script's `[ -n "$VAR" ]`).
static const char *Set(const char *name)
{
    const char *v = getenv(name);
    return v && *v ? v : NULL;
}

// Opens `path` for appending (creating it) and puts it on fd `target`; the script's `>> file` on an exec. On
// failure prints why and exits 1, as the shell does for a failed redirection.
static void AppendTo(const char *path, int target)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) ToolDie(1, "%s: %s", path, strerror(errno));
    if (fd != target) {
        dup2(fd, target);
        close(fd);
    }
}

int main(int argc, char **argv)
{
    ToolInit(argv, "stub-terminal", "stub-terminal");

    // MONOLITH_STUB_LOG: printf '%s\n' "$@" > file (with no arguments printf still prints its format once)
    const char *log = Set("MONOLITH_STUB_LOG");
    if (log) {
        Nob_String_Builder sb = {0};
        for (int i = 1; i < argc; i++) {
            nob_sb_append_cstr(&sb, argv[i]);
            nob_da_append(&sb, '\n');
        }
        if (argc == 1) nob_da_append(&sb, '\n');
        FILE *f = fopen(log, "wb");
        if (f) {
            fwrite(sb.items, 1, sb.count, f);
            fclose(f);
        } else {
            ToolErr("%s: %s", log, strerror(errno));
        }
        nob_sb_free(sb);
    }

    // MONOLITH_STUB_OPENED: echo "$(date '+%H:%M:%S.%N') $$" >> file: one line per "window"
    const char *opened = Set("MONOLITH_STUB_OPENED");
    if (opened) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        localtime_r(&ts.tv_sec, &tm);
        char stamp[32];
        strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
        FILE *f = fopen(opened, "ab");
        if (f) {
            fprintf(f, "%s.%09ld %ld\n", stamp, ts.tv_nsec, (long)getpid());
            fclose(f);
        } else {
            ToolErr("%s: %s", opened, strerror(errno));
        }
    }

    // MONOLITH_STUB_RUN: skip the terminal's own arguments up to -e, then exec the rest with the streams of
    // a terminal that writes into the file. With nothing after -e (or no -e) the shell's `exec` with only
    // redirections ran nothing: the file is still created, and the script went on to exit 0.
    const char *run = Set("MONOLITH_STUB_RUN");
    if (run) {
        int i = 1;
        while (i < argc && strcmp(argv[i], "-e") != 0) i++;
        if (i < argc) i++;
        int in = open("/dev/null", O_RDONLY);
        if (in >= 0 && in != STDIN_FILENO) {
            dup2(in, STDIN_FILENO);
            close(in);
        }
        AppendTo(run, STDOUT_FILENO);
        dup2(STDOUT_FILENO, STDERR_FILENO);
        if (i < argc) {
            Nob_Cmd cmd = {0};
            nob_da_append_many(&cmd, (const char **)argv + i, (size_t)(argc - i));
            ToolExec(&cmd);  // returns only on failure, and then exits 127 / 126 with the reason
        }
    }
    return 0;
}
