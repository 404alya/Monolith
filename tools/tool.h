// tools/tool.h: what every small program in tools/ shares, header-only, on nob.h (src/vendor/nob/nob.h,
// vendored unmodified). Each program is one file, tools/<name>.c, that includes this header first:
//
//     #include "tool.h"
//
//     int main(int argc, char **argv)
//     {
//         ToolInit(argv, "todo", "todo");  // name, repo root, rebuild itself when its source changed
//         ...
//     }
//
// `./build.sh --tools [name...]` (nob.c, region "Tools") compiles tools/<name>.c into build/tools/<name>
// (tools/run-sh.c into build/run-sh.o); after that each program keeps itself current (ToolInit below), so
// the docs and skills call build/tools/<name> directly.
//
// What is here, and why each piece exists:
//   - ToolInit: the program's name for messages, the repo root found from the program's own path (as nob.c
//     finds it from build/nob), and the rebuild of the program when tools/<name>.c, this header or nob.h is
//     newer than the binary.
//   - ToolSay / ToolErr / ToolDie: messages in the shell scripts' "<name>: ..." style, on stdout or stderr.
//   - ToolRun / ToolCapture / ToolExec: running a command given as an argument list (nob's Nob_Cmd: no shell,
//     so no quoting or word-splitting surprises) and getting its exit status, its output, or handing the
//     process over to it. nob's own nob_cmd_run only answers true / false, and the scripts these programs
//     replace read exit codes (monolith's exit 1 for a broken map, request-queue's 3 for a timeout).
//   - ToolTrash: `gio trash <path>`, the repo's only way to delete (CLAUDE.md).
//   - ToolProgram: the path of another program in build/tools/, with the message that says how to build it.
//   - small shell equivalents the scripts used everywhere: ${VAR:-x} (ToolEnv), [[ a -nt b ]] (ToolNewer),
//     mkdir -p (ToolMkdirP), $(cat f) (ToolReadTrimmed).
//
// Everything is `static inline` so a program that doesn't use a function gets no unused-function warning;
// every program is a single translation unit, so nothing here needs a separate implementation switch.
// Linux only, like the editor these tools serve (fork / exec, /proc/self/exe).

#ifndef MONOLITH_TOOL_H
#define MONOLITH_TOOL_H

// fork/exec/waitpid, setenv, realpath, strdup, ... : the POSIX and GNU declarations, before any system header
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

// nob prints "[INFO] CMD: ..." for every command it runs and "[INFO] created directory ..." for its file
// helpers; the shell scripts these programs replace printed neither, and their output text must not change.
#define NOB_NO_ECHO
#define NOB_IMPLEMENTATION
#include "../src/vendor/nob/nob.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// #region Program identity and messages

// The program's name as its messages print it ("todo", "request-queue", "run-sh"), set by ToolInit.
static const char *gToolName = "tool";
// The repo root (absolute, no trailing slash), set by ToolInit.
static const char *gToolRoot = NULL;
// The program's own path (absolute), set by ToolInit; what ToolInit re-executes after a rebuild.
static const char *gToolSelf = NULL;

// "<name>: <text>\n" on stdout (the scripts' plain `echo "name: ..."`).
NOB_PRINTF_FORMAT(1, 2) static inline void ToolSay(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stdout, "%s: ", gToolName);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fputc('\n', stdout);
    fflush(stdout);
}

// "<name>: <text>\n" on stderr (the scripts' `echo "name: ..." >&2`).
NOB_PRINTF_FORMAT(1, 2) static inline void ToolErr(const char *fmt, ...)
{
    fflush(stdout);  // keep the order the two streams were written in when both go to one terminal or file
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "%s: ", gToolName);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

// ToolErr, then exit with `code` (the scripts' `echo ... >&2; exit N`).
NOB_PRINTF_FORMAT(2, 3) static inline void ToolDie(int code, const char *fmt, ...)
{
    fflush(stdout);
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "%s: ", gToolName);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    exit(code);
}

// #endregion

// #region Shell equivalents

// ${NAME:-fallback}: the variable when it is set and not empty, else the fallback.
static inline const char *ToolEnv(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return v && *v ? v : fallback;
}

// bash's [[ a -nt b ]]: a exists and is newer than b (nanosecond modification times), or a exists and b
// doesn't. (nob's nob_needs_rebuild compares whole seconds, so a source saved in the same second as the
// binary was built would not count as newer.)
static inline bool ToolNewer(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) != 0) return false;
    if (stat(b, &sb) != 0) return true;
    if (sa.st_mtim.tv_sec != sb.st_mtim.tv_sec) return sa.st_mtim.tv_sec > sb.st_mtim.tv_sec;
    return sa.st_mtim.tv_nsec > sb.st_mtim.tv_nsec;
}

// `mkdir -p path`: the directory and any missing parents (mode 0777 minus the umask). Prints the reason
// and returns false on failure.
static inline bool ToolMkdirP(const char *path)
{
    size_t n = strlen(path);
    char *p = strdup(path);
    if (!p) return false;
    bool ok = true;
    for (size_t i = 1; i <= n && ok; i++) {
        if (p[i] != '/' && p[i] != '\0') continue;
        char saved = p[i];
        p[i] = '\0';
        struct stat st;
        if (stat(p, &st) != 0) {
            if (mkdir(p, 0777) != 0 && errno != EEXIST) {
                ToolErr("mkdir: cannot create directory '%s': %s", p, strerror(errno));
                ok = false;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            ToolErr("mkdir: '%s' exists but is not a directory", p);
            ok = false;
        }
        p[i] = saved;
    }
    free(p);
    return ok;
}

// "$(cat file)": the file's text with its trailing newlines removed, as a heap string (free it); NULL if
// the file can't be read.
static inline char *ToolReadTrimmed(const char *path)
{
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(path, &sb)) return NULL;
    while (sb.count > 0 && sb.items[sb.count - 1] == '\n') sb.count--;
    nob_sb_append_null(&sb);
    return sb.items;
}

// #endregion

// #region Running commands

// Where a command's standard streams go. NULL paths keep this process's own stream (the terminal, a pipe,
// whatever the caller was given). `append` opens out / err with O_APPEND instead of truncating them
// (`>>`); `errToOut` sends stderr wherever stdout goes (`2>&1`, also when stdout is a captured pipe);
// `outToErr` sends stdout to this process's stderr (`>&2`).
typedef struct {
    const char *in;      // `< path`
    const char *out;     // `> path` (or `>> path` with append)
    const char *err;     // `2> path` (or `2>> path` with append)
    bool append;
    bool errToOut;
    bool outToErr;
} ToolIo;

// The exit status the shell would report for a waitpid() status: the exit code, or 128 + the signal
// number when the process was killed.
static inline int ToolStatus(int wstatus)
{
    if (WIFEXITED(wstatus)) return WEXITSTATUS(wstatus);
    if (WIFSIGNALED(wstatus)) return 128 + WTERMSIG(wstatus);
    return 1;
}

// In the child: open `path` and put it on fd `target`; on failure the child exits 1 with the reason, as a
// shell does for a failed redirection.
static inline void ToolRedirect(const char *path, int target, int flags)
{
    int fd = open(path, flags, 0666);
    if (fd < 0) {
        fprintf(stderr, "%s: %s: %s\n", gToolName, path, strerror(errno));
        _exit(1);
    }
    if (fd != target) {
        dup2(fd, target);
        close(fd);
    }
}

// Starts cmd (searched in PATH like a shell when its first word has no '/') with the given streams;
// `captureFd` >= 0 becomes the child's stdout (ToolCapture's pipe). Returns the pid, or -1 when fork fails.
static inline pid_t ToolSpawn(Nob_Cmd *cmd, ToolIo io, int captureFd, int closeFd)
{
    if (cmd->count == 0) return -1;
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid != 0) return pid;

    // the child: set up the streams in the shell's order (stdin, stdout, stderr), then exec
    if (closeFd >= 0) close(closeFd);
    int outFlags = O_WRONLY | O_CREAT | (io.append ? O_APPEND : O_TRUNC);
    if (io.in) ToolRedirect(io.in, STDIN_FILENO, O_RDONLY);
    if (captureFd >= 0) {
        dup2(captureFd, STDOUT_FILENO);
        close(captureFd);
    } else if (io.outToErr) {
        dup2(STDERR_FILENO, STDOUT_FILENO);
    } else if (io.out) {
        ToolRedirect(io.out, STDOUT_FILENO, outFlags);
    }
    if (io.errToOut) dup2(STDOUT_FILENO, STDERR_FILENO);
    else if (io.err) ToolRedirect(io.err, STDERR_FILENO, outFlags);
    // execvp wants a NULL-terminated array; cmd->items is a heap array with room to add one
    nob_da_append(cmd, (const char *)NULL);
    execvp(cmd->items[0], (char *const *)cmd->items);
    fprintf(stderr, "%s: %s: %s\n", gToolName, cmd->items[0], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

// Runs cmd to its end and returns its exit status (0 = success; 128 + n when killed by signal n; 127 when
// the program wasn't found, like a shell). The command is reset afterwards (count = 0, memory kept), as
// nob_cmd_run does, so it can be filled again.
static inline int ToolRun(Nob_Cmd *cmd, ToolIo io)
{
    pid_t pid = ToolSpawn(cmd, io, -1, -1);
    cmd->count = 0;
    if (pid < 0) {
        ToolErr("could not start a process: %s", strerror(errno));
        return 126;
    }
    int wstatus;
    while (waitpid(pid, &wstatus, 0) < 0) {
        if (errno != EINTR) return 126;
    }
    return ToolStatus(wstatus);
}

// "$(command)": runs cmd, appends everything it writes to stdout to `out` (not NUL-terminated; the caller
// adds the terminator or trims), and returns its exit status like ToolRun. io.out is ignored (stdout is the
// pipe); io.errToOut captures stderr too.
static inline int ToolCapture(Nob_Cmd *cmd, Nob_String_Builder *out, ToolIo io)
{
    int fds[2];
    if (pipe(fds) != 0) {
        ToolErr("pipe: %s", strerror(errno));
        cmd->count = 0;
        return 126;
    }
    pid_t pid = ToolSpawn(cmd, io, fds[1], fds[0]);
    cmd->count = 0;
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        ToolErr("could not start a process: %s", strerror(errno));
        return 126;
    }
    char buf[4096];
    for (;;) {
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        nob_sb_append_buf(out, buf, (size_t)n);
    }
    close(fds[0]);
    int wstatus;
    while (waitpid(pid, &wstatus, 0) < 0) {
        if (errno != EINTR) return 126;
    }
    return ToolStatus(wstatus);
}

// `exec command ...`: replaces this process with cmd (searched in PATH), so its exit status, signals and
// stdin / stdout / stderr are the caller's directly. Returns only on failure: then it prints why and exits
// 127 (not found) or 126, as a shell does.
static inline void ToolExec(Nob_Cmd *cmd)
{
    fflush(stdout);
    fflush(stderr);
    nob_da_append(cmd, (const char *)NULL);
    execvp(cmd->items[0], (char *const *)cmd->items);
    ToolDie(errno == ENOENT ? 127 : 126, "%s: %s", cmd->items[0], strerror(errno));
}

// `gio trash <path>`: moves the path to the desktop trash (the developer can restore it). The repo's only
// way to delete anything (CLAUDE.md: never rm). Returns gio's exit status; `quiet` sends gio's own error
// text to /dev/null (callers that print their own message, like `gio trash ... 2>/dev/null || echo ...`).
static inline int ToolTrash(const char *path, bool quiet)
{
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "gio", "trash", path);
    int status = ToolRun(&cmd, (ToolIo){.err = quiet ? "/dev/null" : NULL});
    nob_cmd_free(cmd);
    return status;
}

// #endregion

// #region Repo root, other programs, rebuilding itself

// The C compiler and flags of every program in tools/: ${CC:-clang}, C11, every warning (zero warnings, as
// everywhere in the repo), -O1 (they do little work of their own). Keep in step with ToolCommand in nob.c,
// which builds them the first time: a rebuild here must give the same binary.
static inline void ToolCompileCommand(Nob_Cmd *cmd, const char *source, const char *binary)
{
    nob_cmd_append(cmd, ToolEnv("CC", "clang"), "-std=c11", "-Wall", "-Wextra",
                   "-Wno-missing-field-initializers", "-O1", "-o", binary, source);
}

// Lexical normalisation of an absolute path (collapses "//", drops ".", resolves ".." against the previous
// component), without following symlinks: the repo root keeps the name the caller used, as nob.c's
// NormalizeAbsolute does for build/nob. Returns a heap string.
static inline char *ToolNormalize(const char *path)
{
    Nob_String_Builder sb = {0};
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - start);
        if (n == 0 || (n == 1 && start[0] == '.')) continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            while (sb.count > 0 && sb.items[sb.count - 1] != '/') sb.count--;
            if (sb.count > 0) sb.count--;
            continue;
        }
        nob_da_append(&sb, '/');
        nob_sb_append_buf(&sb, start, n);
    }
    if (sb.count == 0) nob_da_append(&sb, '/');
    nob_sb_append_null(&sb);
    return sb.items;
}

// The current directory as the shell names it: $PWD when it is absolute and is this directory (a path
// through a symlink stays as typed, like bash's pwd), else getcwd(). Returns a heap string.
static inline char *ToolLogicalCwd(void)
{
    const char *pwd = getenv("PWD");
    struct stat a, b;
    if (pwd && pwd[0] == '/' && stat(pwd, &a) == 0 && stat(".", &b) == 0 && a.st_dev == b.st_dev &&
        a.st_ino == b.st_ino) {
        return strdup(pwd);
    }
    return getcwd(NULL, 0);
}

// This program's absolute path: argv[0] when it names a path (made absolute against the logical current
// directory and normalised, so a repo reached through a symlink keeps that name, as `cd "$(dirname "$0")"
// && pwd` did in the scripts), else /proc/self/exe (found through PATH). Returns a heap string.
static inline char *ToolSelfPath(const char *argv0)
{
    if (argv0 && strchr(argv0, '/')) {
        if (argv0[0] == '/') return ToolNormalize(argv0);
        char *cwd = ToolLogicalCwd();
        char *joined = cwd ? strdup(nob_temp_sprintf("%s/%s", cwd, argv0)) : NULL;
        free(cwd);
        if (joined) {
            char *result = ToolNormalize(joined);
            free(joined);
            return result;
        }
    }
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n < 0) return NULL;
    buf[n] = '\0';
    return strdup(buf);
}

// The repo root above `path`: the nearest folder holding both build.sh and nob.c (a program lives in
// <root>/build/tools/ or, for run-sh.o, <root>/build/; a git worktree is its own root). Heap string or NULL.
static inline char *ToolFindRoot(const char *path)
{
    char *dir = strdup(path);
    if (!dir) return NULL;
    for (;;) {
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) break;
        *slash = '\0';
        struct stat a, b;
        if (stat(nob_temp_sprintf("%s/build.sh", dir), &a) == 0 && S_ISREG(a.st_mode) &&
            stat(nob_temp_sprintf("%s/nob.c", dir), &b) == 0 && S_ISREG(b.st_mode)) {
            return dir;
        }
    }
    free(dir);
    return NULL;
}

// The path of another program built by `./build.sh --tools` ("request-queue" -> <root>/build/tools/
// request-queue; "run-sh" -> <root>/build/run-sh.o). When it isn't built, says how to build it and exits 1.
// Returns a temp string (nob's temp buffer: copy it to keep it past nob_temp_reset).
static inline const char *ToolProgram(const char *name)
{
    const char *path = strcmp(name, "run-sh") == 0 ? nob_temp_sprintf("%s/build/run-sh.o", gToolRoot)
                                                   : nob_temp_sprintf("%s/build/tools/%s", gToolRoot, name);
    if (access(path, X_OK) != 0) {
        ToolDie(1, "%s is not built; run: ./build.sh --tools %s", path + strlen(gToolRoot) + 1, name);
    }
    return path;
}

// Rebuilds this program when its source (tools/<source>.c), tools/tool.h or nob.h is newer than the binary,
// then runs the new binary in place of this process (execv: same pid, arguments, streams; its exit status
// is the caller's). The compile goes into a temporary file that is renamed over the binary, so a program
// started at the same moment never runs a half-written file, and two rebuilding at once both end with a
// whole one. Does nothing when the source isn't there, or when this binary is not the one at the program's
// own place (a sanitizer copy, a binary copied elsewhere). If the compile fails
// the old binary stays and keeps running, with a warning, so a broken edit doesn't lock anyone out.
// (nob.h's NOB_GO_REBUILD_URSELF does the same check but runs the new binary as a child and exits 0 or 1,
// losing the exit status these programs report, and it keeps a <binary>.old next to it.)
static inline void ToolRebuildSelf(char **argv, const char *source)
{
    const char *src = nob_temp_sprintf("%s/tools/%s.c", gToolRoot, source);
    const char *deps[] = {
        src,
        nob_temp_sprintf("%s/tools/tool.h", gToolRoot),
        nob_temp_sprintf("%s/src/vendor/nob/nob.h", gToolRoot),
    };
    struct stat st;
    if (stat(src, &st) != 0) return;
    // Only the program at its own place (build/tools/<name>, build/run-sh.o) rebuilds itself: a copy built
    // elsewhere on purpose (a sanitizer build in build/unit/ or by the leak check) must stay what it is.
    const char *home = strcmp(source, "run-sh") == 0 ? nob_temp_sprintf("%s/build/run-sh.o", gToolRoot)
                                                     : nob_temp_sprintf("%s/build/tools/%s", gToolRoot, source);
    if (strcmp(gToolSelf, home) != 0) return;
    bool stale = false;
    for (size_t i = 0; i < NOB_ARRAY_LEN(deps); i++) stale = stale || ToolNewer(deps[i], gToolSelf);
    if (!stale) return;

    const char *tmp = nob_temp_sprintf("%s.tmp.%ld", gToolSelf, (long)getpid());
    Nob_Cmd cmd = {0};
    ToolCompileCommand(&cmd, src, tmp);
    // the compiler's messages go to stderr, so they never mix into a program's stdout (which callers parse)
    int status = ToolRun(&cmd, (ToolIo){.outToErr = true});
    nob_cmd_free(cmd);
    if (status != 0 || rename(tmp, gToolSelf) != 0) {
        ToolErr("warning: tools/%s.c changed but did not compile; running the old build", source);
        return;
    }
    fflush(NULL);
    execv(gToolSelf, argv);
    ToolDie(126, "exec %s after its rebuild: %s", gToolSelf, strerror(errno));
}

// Sets the program's name for messages, finds the repo root from the program's own path and rebuilds the
// program when its source changed (ToolRebuildSelf). `source` is the file name under tools/ without ".c"
// (usually the same as `name`). Exits 1 when the root can't be found (the binary was moved out of a
// checkout).
static inline void ToolInit(char **argv, const char *name, const char *source)
{
    gToolName = name;
    char *self = ToolSelfPath(argv[0]);
    if (!self) ToolDie(1, "can't find my own path");
    gToolSelf = self;
    gToolRoot = ToolFindRoot(self);
    if (!gToolRoot) ToolDie(1, "can't find the repo root (no build.sh and nob.c above %s)", self);
    ToolRebuildSelf(argv, source);
}

// #endregion

#endif // MONOLITH_TOOL_H
