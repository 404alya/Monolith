// examples/nob.c: the build program of the examples, written on nob.h like the repo's nob.c. `./build.sh examples
// [mode]` (nob.c, BuildExamples) compiles it into examples/build/nob and runs it with the mode (none, --win, test,
// lsan); run directly it rebuilds itself the same way (NOB_GO_REBUILD_URSELF).
//
// The runtime is built like an exported game (no -DMONOLITH_EDITOR), except for 03_live_editing; the test builds
// add -include tests/examples/harness.h. The runtime is compiled once per flag set into an object; every
// out-of-date object compiles in one pool, the out-of-date programs link in a second (RunPool, a copy of
// nob.c's). Keep in step: the repo's nob.c mirrors these flags in compile_commands.json and has the same glibc
// --wrap list and pool.
// Reference: docs/code/nob-examples.md

// sched / glob / strdup declarations before any system header (nob.h only sets _POSIX_C_SOURCE when nothing
// else picked a feature level).
#define _GNU_SOURCE

// RunTimed prints its own "[INFO] CMD: ..." echo; nob's own echo (and its "[INFO] created directory ..."
// chatter from the file helpers) would print every command twice.
#define NOB_NO_ECHO
// How this program rebuilds itself when examples/nob.c or nob.h is newer than examples/build/nob (main(),
// nob__go_rebuild_urself): the same compiler and flags nob.c's BuildExamples compiles it with the first time
// (${CC:-clang}, C11, all warnings, -O1), so a rebuild gives the same binary.
#define NOB_REBUILD_URSELF(binary_path, source_path) \
    (getenv("CC") && *getenv("CC") ? getenv("CC") : "clang"), \
    "-std=c11", "-Wall", "-Wextra", "-O1", "-o", (binary_path), (source_path)
#define NOB_IMPLEMENTATION
#include "../src/vendor/nob/nob.h"

#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <sched.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

// #region Console, logging and timing (the same as nob.c's, trimmed to what the examples need)

// The terminal's stdout and stderr, duplicated at start: RunTimed points fds 1 and 2 at a log file while the
// scene compile runs (`> compile.log 2>&1`), and our own messages must still reach the terminal.
static FILE *gOut;
static FILE *gErr;

// One row of the summary table: a timed command, or a pool's own row (RunPool).
//   wall    seconds from the command's start to its end (a pool's row: from its first start to its last reap)
//   user, sys  CPU seconds of the command and every process it waited for (a pool's row: its commands' sum)
//   pool    0: a command run alone (RunTimed); n: the n-th pool's command, or (header) that pool's own row
typedef struct {
    const char *label;
    double wall, user, sys;
    bool ok;
    int pool;
    bool header;
} Timing;

typedef struct {
    Timing *items;
    size_t count, capacity;
} Timings;

static Timings gTimings;
static uint64_t gStartNanos;  // when main() started, for the whole run's wall time in the summary

// nob's log handler: "[INFO] ..." to the terminal's stdout, warnings and errors to its stderr.
static void LogHandler(Nob_Log_Level level, const char *fmt, va_list arg_ptr)
{
    FILE *f = level == NOB_INFO ? gOut : gErr;
    switch (level) {
    case NOB_INFO:    fprintf(f, "[INFO] "); break;
    case NOB_WARNING: fprintf(f, "[WARNING] "); break;
    case NOB_ERROR:   fprintf(f, "[ERROR] "); break;
    case NOB_NO_LOGS: return;
    default:          NOB_UNREACHABLE("Nob_Log_Level");
    }
    vfprintf(f, fmt, arg_ptr);
    fprintf(f, "\n");
    fflush(f);
}

// Our progress lines (the script's `echo`s: "built: ...", "compiled: ..."): to the terminal's stdout.
NOB_PRINTF_FORMAT(1, 2) static void Say(const char *fmt, ...)
{
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    vfprintf(gOut, fmt, arg_ptr);
    va_end(arg_ptr);
    fputc('\n', gOut);
    fflush(gOut);
}

// Error lines (the script's `echo ... >&2`): to the terminal's stderr, with the program's prefix.
NOB_PRINTF_FORMAT(1, 2) static void SayErr(const char *fmt, ...)
{
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    fprintf(gErr, "build.sh: examples: ");
    vfprintf(gErr, fmt, arg_ptr);
    va_end(arg_ptr);
    fputc('\n', gErr);
    fflush(gErr);
}

// A struct timeval in seconds.
static double Seconds(struct timeval tv)
{
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

// The "[TIME] <label>: <wall> s wall, <cpu> s CPU (<user> user + <sys> sys)" line after a command.
static void PrintTime(const Timing *t)
{
    fprintf(gOut, "[TIME] %s: %.2f s wall, %.2f s CPU (%.2f user + %.2f sys)%s\n", t->label, t->wall,
            t->user + t->sys, t->user, t->sys, t->ok ? "" : " (failed)");
    fflush(gOut);
}

// Runs one command on its own (resetting it, as nob_cmd_run does) and records its time; with RunPool the only
// places a command runs. Prints nob's "[INFO] CMD: ..." echo, runs it synchronously, then its "[TIME]" line.
// `log`: NULL leaves the command's output on the terminal; a path sends its stdout and stderr there
// (`> log 2>&1`, truncating it). CPU time is the RUSAGE_CHILDREN difference around the command: exact because
// nothing else runs meanwhile (RunPool reaps all its children before it returns).
static bool RunTimed(Nob_Cmd *cmd, const char *label, const char *log)
{
    Nob_String_Builder echo = {0};
    nob_cmd_render(*cmd, &echo);
    nob_sb_append_null(&echo);
    nob_log(NOB_INFO, "CMD: %s", echo.items);
    nob_sb_free(echo);

    // `> log 2>&1`: point this process's fds 1 and 2 at the log while the command runs, so the child
    // inherits both on one file description (nob's .stdout_path / .stderr_path would open it twice, and the
    // two streams would overwrite each other).
    int savedOut = -1, savedErr = -1;
    fflush(stdout);
    fflush(stderr);
    if (log) {
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            nob_log(NOB_ERROR, "could not open %s: %s", log, strerror(errno));
            cmd->count = 0;
            return false;
        }
        savedOut = dup(STDOUT_FILENO);
        savedErr = dup(STDERR_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }

    struct rusage before, after;
    getrusage(RUSAGE_CHILDREN, &before);
    uint64_t t0 = nob_nanos_since_unspecified_epoch();
    bool ok = nob_cmd_run_opt(cmd, (Nob_Cmd_Opt){0});
    uint64_t t1 = nob_nanos_since_unspecified_epoch();
    getrusage(RUSAGE_CHILDREN, &after);

    // the terminal back on fds 1 and 2
    if (log) {
        dup2(savedOut, STDOUT_FILENO);
        dup2(savedErr, STDERR_FILENO);
        close(savedOut);
        close(savedErr);
    }

    Timing t = {
        .label = label,
        .wall = (double)(t1 - t0) / 1e9,
        .user = Seconds(after.ru_utime) - Seconds(before.ru_utime),
        .sys = Seconds(after.ru_stime) - Seconds(before.ru_stime),
        .ok = ok,
    };
    nob_da_append(&gTimings, t);
    PrintTime(&t);
    return ok;
}

// The pool: the same code as the repo's nob.c (RunPool and its helpers; keep the two in step). Commands run
// up to N at once, each timed on its own through wait4(), its output captured and printed whole when it ends.

// One command of a pool: its argument list and its label in the summary.
typedef struct {
    Nob_Cmd cmd;
    const char *label;
} PoolJob;

typedef struct {
    PoolJob *items;
    size_t count, capacity;
} PoolJobs;

// A pool command while it runs: its job, process, start time and the two temporary files that take its
// stdout and stderr (unlinked at once by tmpfile(), so nothing is left behind).
typedef struct {
    PoolJob *job;
    pid_t pid;
    uint64_t start;
    FILE *out, *err;
} Running;

static int gPools;  // pools run so far: the next pool's number in the summary

// One "OMP_*" value as GNU nproc reads it: a positive number, optionally followed by blanks and a comma
// list; 0 if absent or malformed.
static unsigned long OmpThreads(const char *v)
{
    if (!v) return 0;
    while (*v == ' ' || *v == '\t') v++;
    if (*v < '0' || *v > '9') return 0;
    char *end;
    unsigned long n = strtoul(v, &end, 10);
    while (*end == ' ' || *end == '\t') end++;
    return *end == '\0' || *end == ',' ? n : 0;
}

// `nproc` (nob.c's Nproc): the CPUs this process may run on, overridden by OMP_NUM_THREADS and capped by
// OMP_THREAD_LIMIT as GNU nproc does.
static unsigned long Nproc(void)
{
    unsigned long limit = OmpThreads(getenv("OMP_THREAD_LIMIT"));
    if (limit == 0) limit = ULONG_MAX;
    unsigned long n = OmpThreads(getenv("OMP_NUM_THREADS"));
    if (n == 0) {
        cpu_set_t set;
        n = sched_getaffinity(0, sizeof set, &set) == 0 ? (unsigned long)CPU_COUNT(&set)
                                                        : (unsigned long)sysconf(_SC_NPROCESSORS_ONLN);
        if (n == 0) n = 1;
    }
    return n < limit ? n : limit;
}

// The pool's size: $JOBS when it is set (a positive number), else Nproc(), as in the repo's build.
static size_t PoolSize(void)
{
    const char *jobs = getenv("JOBS");
    if (!jobs || !*jobs) return (size_t)Nproc();
    char *end;
    unsigned long n = strtoul(jobs, &end, 10);
    if (*end != '\0' || n == 0 || jobs[0] == '-') {
        SayErr("JOBS must be a positive number (it is \"%s\")", jobs);
        exit(1);
    }
    return (size_t)n;
}

// Copies what a pool command wrote to one of its temporary files to `to` and closes the file.
static void DrainCapture(FILE *f, FILE *to)
{
    char buf[1 << 14];
    size_t n;
    rewind(f);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, to);
    fclose(f);
    fflush(to);
}

// Starts one pool command: fork + exec with its stdout and stderr on two temporary files. False (with a
// message) when the files or the process can't be made; a program that can't be executed is the child's
// failure (exit 127, its message in the captured stderr), as with nob_cmd_run.
static bool StartJob(PoolJob *job, Running *r)
{
    r->job = job;
    r->out = tmpfile();
    r->err = tmpfile();
    if (!r->out || !r->err) {
        SayErr("could not create a temporary file for %s: %s", job->label, strerror(errno));
        if (r->out) fclose(r->out);
        if (r->err) fclose(r->err);
        return false;
    }
    // the files must not leak into the other commands of the pool (dup2 makes fds 1 and 2 inheritable again)
    fcntl(fileno(r->out), F_SETFD, FD_CLOEXEC);
    fcntl(fileno(r->err), F_SETFD, FD_CLOEXEC);
    // the NULL-terminated argv, made before fork(): the child only calls async-signal-safe functions
    Nob_Cmd argv = {0};
    nob_da_append_many(&argv, job->cmd.items, job->cmd.count);
    nob_cmd_append(&argv, (const char *)NULL);
    fflush(NULL);
    r->start = nob_nanos_since_unspecified_epoch();
    r->pid = fork();
    if (r->pid < 0) {
        SayErr("could not start %s: %s", job->label, strerror(errno));
        nob_cmd_free(argv);
        fclose(r->out);
        fclose(r->err);
        return false;
    }
    if (r->pid == 0) {
        dup2(fileno(r->out), STDOUT_FILENO);
        dup2(fileno(r->err), STDERR_FILENO);
        execvp(argv.items[0], (char *const *)argv.items);
        dprintf(STDERR_FILENO, "[ERROR] could not exec %s: %s\n", argv.items[0], strerror(errno));
        _exit(127);
    }
    nob_cmd_free(argv);
    return true;
}

// Runs `jobs` in a pool of up to `size` processes at once, as one step `label` of the summary, and resets
// the list. For every command, when it ends: its "[INFO] CMD: ..." echo, its stdout and stderr (captured
// whole, so parallel compilers never interleave mid-line), why it failed if it did, and its "[TIME]" line.
// Wall from fork to reap, CPU from wait4() (that child's own rusage, exact while commands overlap). After the
// first failure no new command starts; the running ones are waited for. True when every command succeeded.
static bool RunPool(PoolJobs *jobs, const char *label, size_t size)
{
    if (jobs->count == 0) return true;
    if (size == 0) size = 1;
    int pool = ++gPools;
    // the pool's own row first, its commands' rows (in list order) after it; filled in at the end
    size_t headerAt = gTimings.count;
    nob_da_append(&gTimings, ((Timing){.label = label, .pool = pool, .header = true}));
    Timing *rows = calloc(jobs->count, sizeof *rows);
    bool *ran = calloc(jobs->count, sizeof *ran);
    Running *running = calloc(size, sizeof *running);
    NOB_ASSERT(rows && ran && running);

    uint64_t t0 = nob_nanos_since_unspecified_epoch();
    size_t next = 0, active = 0;
    bool ok = true;
    while (active > 0 || (ok && next < jobs->count)) {
        // fill free slots while nothing has failed
        while (ok && next < jobs->count && active < size) {
            Running *slot = NULL;
            for (size_t s = 0; s < size && !slot; s++) if (!running[s].job) slot = &running[s];
            if (!StartJob(&jobs->items[next], slot)) {
                slot->job = NULL;
                rows[next] = (Timing){.label = jobs->items[next].label, .pool = pool};
                ran[next] = true;
                ok = false;
                next++;
                break;
            }
            next++;
            active++;
        }
        if (active == 0) break;

        // reap the next command to end; its own rusage comes with it
        int status;
        struct rusage ru;
        pid_t pid = wait4(-1, &status, 0, &ru);
        if (pid < 0) {
            if (errno == EINTR) continue;
            SayErr("wait4: %s", strerror(errno));
            exit(1);
        }
        uint64_t t1 = nob_nanos_since_unspecified_epoch();
        Running *r = NULL;
        for (size_t s = 0; s < size && !r; s++) if (running[s].job && running[s].pid == pid) r = &running[s];
        if (!r) continue;  // not one of ours (nothing else is started meanwhile)
        size_t index = (size_t)(r->job - jobs->items);
        bool jobOk = WIFEXITED(status) && WEXITSTATUS(status) == 0;

        // the command's echo, output and time, in one piece
        Nob_String_Builder echo = {0};
        nob_cmd_render(r->job->cmd, &echo);
        nob_sb_append_null(&echo);
        nob_log(NOB_INFO, "CMD: %s", echo.items);
        nob_sb_free(echo);
        DrainCapture(r->out, gOut);
        DrainCapture(r->err, gErr);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            nob_log(NOB_ERROR, "command exited with exit code %d", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            nob_log(NOB_ERROR, "command process was terminated by signal %d", WTERMSIG(status));
        }
        rows[index] = (Timing){
            .label = r->job->label,
            .wall = (double)(t1 - r->start) / 1e9,
            .user = Seconds(ru.ru_utime),
            .sys = Seconds(ru.ru_stime),
            .ok = jobOk,
            .pool = pool,
        };
        ran[index] = true;
        PrintTime(&rows[index]);
        ok = ok && jobOk;
        r->job = NULL;
        active--;
    }
    uint64_t t1 = nob_nanos_since_unspecified_epoch();

    // the rows of the commands that ran, in list order, and the pool's own row
    Timing header = {.label = label, .wall = (double)(t1 - t0) / 1e9, .ok = ok, .pool = pool, .header = true};
    size_t count = 0;
    for (size_t i = 0; i < jobs->count; i++) {
        if (!ran[i]) continue;
        nob_da_append(&gTimings, rows[i]);
        header.user += rows[i].user;
        header.sys += rows[i].sys;
        count++;
    }
    header.label = nob_temp_sprintf("%s: %zu commands, up to %zu at once", label, count, size);
    gTimings.items[headerAt] = header;
    PrintTime(&header);

    for (size_t i = 0; i < jobs->count; i++) nob_cmd_free(jobs->items[i].cmd);
    jobs->count = 0;
    free(rows);
    free(ran);
    free(running);
    return ok;
}

// The summary table of every timed command and their total, plus the whole run's wall time (which also
// covers this program's own work: the data copies). Printed at the end, and before a failed run exits. A pool
// is one row (its elapsed time, its commands' CPU) with its commands indented below it: their wall times
// overlap, so only the pool's row counts towards the total wall time; CPU counts every command once.
static void PrintSummary(void)
{
    if (gTimings.count == 0) return;
    size_t width = strlen("total");
    for (size_t i = 0; i < gTimings.count; i++) {
        Timing *t = &gTimings.items[i];
        size_t n = strlen(t->label) + (t->pool && !t->header ? 2 : 0);
        if (n > width) width = n;
    }
    double wall = 0, user = 0, sys = 0;
    fprintf(gOut, "[TIME] summary (wall = elapsed; CPU = user + sys of the command and every process it waited for;\n"
                  "       indented commands ran in parallel in the pool above them: their wall times overlap, the\n"
                  "       pool's row is its elapsed time and the total counts that)\n");
    fprintf(gOut, "  %-*s %9s %9s %9s %9s\n", (int)width, "command", "wall s", "CPU s", "user s", "sys s");
    for (size_t i = 0; i < gTimings.count; i++) {
        Timing *t = &gTimings.items[i];
        bool inPool = t->pool && !t->header;
        fprintf(gOut, "  %s%-*s %9.2f %9.2f %9.2f %9.2f%s\n", inPool ? "  " : "", (int)width - (inPool ? 2 : 0),
                t->label, t->wall, t->user + t->sys, t->user, t->sys, t->ok ? "" : "  (failed)");
        if (!inPool) wall += t->wall;
        if (!t->header) {
            user += t->user;
            sys += t->sys;
        }
    }
    fprintf(gOut, "  %-*s %9.2f %9.2f %9.2f %9.2f\n", (int)width, "total", wall, user + sys, user, sys);
    double whole = (double)(nob_nanos_since_unspecified_epoch() - gStartNanos) / 1e9;
    fprintf(gOut, "[TIME] whole run: %.2f s wall (commands %.2f s, own file work and checks %.2f s)\n", whole, wall,
            whole - wall);
    fflush(gOut);
}

// Exit 1 after a failure (the script's `set -e` stop), with the summary of what ran so far.
static void Fail(void)
{
    PrintSummary();
    exit(1);
}

// #endregion

// #region Paths and files (the script's cd/pwd, mkdir -p, cp -r, cat)

// Lexical normalisation of an absolute path (what `cd dir && pwd` gives without following symlinks):
// collapses "//", drops ".", resolves ".." against the previous component. Returns a temp string.
static const char *NormalizeAbsolute(const char *path)
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
    const char *result = nob_temp_strdup(sb.items);
    nob_sb_free(sb);
    return result;
}

// The current directory as the shell names it: $PWD when it is absolute and names this directory (a path
// through a symlink stays as typed, like bash's pwd), else getcwd().
static const char *LogicalCwd(void)
{
    const char *pwd = getenv("PWD");
    struct stat a, b;
    if (pwd && pwd[0] == '/' && stat(pwd, &a) == 0 && stat(".", &b) == 0 && a.st_dev == b.st_dev &&
        a.st_ino == b.st_ino) {
        return pwd;
    }
    const char *cwd = nob_get_current_dir_temp();
    if (!cwd) {
        SayErr("can't read the current directory");
        exit(1);
    }
    return cwd;
}

// ${NAME:-fallback}: the variable when it is set and not empty.
static const char *Env(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return v && *v ? v : fallback;
}

// `mkdir -p path`: the directory and any missing parents (mode 0777 minus the umask, like mkdir).
static bool MkdirP(const char *path)
{
    Nob_String_Builder sb = {0};
    nob_sb_append_cstr(&sb, path);
    nob_sb_append_null(&sb);
    bool ok = true;
    for (size_t i = 1; i <= sb.count - 1 && ok; i++) {
        if (sb.items[i] != '/' && sb.items[i] != '\0') continue;
        char saved = sb.items[i];
        sb.items[i] = '\0';
        struct stat st;
        if (stat(sb.items, &st) != 0) {
            if (mkdir(sb.items, 0777) != 0 && errno != EEXIST) {
                SayErr("mkdir: cannot create directory '%s': %s", sb.items, strerror(errno));
                ok = false;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            SayErr("mkdir: '%s' exists but is not a directory", sb.items);
            ok = false;
        }
        sb.items[i] = saved;
    }
    nob_sb_free(sb);
    return ok;
}

// `cp src dst` for one regular file: dst is created with src's mode (minus the umask), or truncated and
// overwritten when it exists (keeping its mode), as cp does without -p. Timestamps are new, like cp's.
static bool CopyFile(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0) {
        SayErr("cp: cannot open '%s' for reading: %s", src, strerror(errno));
        return false;
    }
    struct stat st;
    fstat(in, &st);
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
    if (out < 0) {
        SayErr("cp: cannot create regular file '%s': %s", dst, strerror(errno));
        close(in);
        return false;
    }
    bool ok = true;
    static char buf[1 << 16];
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0) {
            SayErr("cp: error reading '%s': %s", src, strerror(errno));
            ok = false;
            break;
        }
        for (ssize_t done = 0; done < n;) {
            ssize_t m = write(out, buf + done, (size_t)(n - done));
            if (m < 0) {
                SayErr("cp: error writing '%s': %s", dst, strerror(errno));
                ok = false;
                break;
            }
            done += m;
        }
        if (!ok) break;
    }
    close(in);
    if (close(out) != 0 && ok) {
        SayErr("cp: error writing '%s': %s", dst, strerror(errno));
        ok = false;
    }
    return ok;
}

// `cp -r src/. dst/` (dst is an existing folder): every entry of src, dot files included, merged into dst.
// A folder that exists in dst is entered (its mode left alone, as cp does), a missing one is created with
// its source mode; a file is overwritten in place; a symlink is copied as a symlink (cp -r doesn't follow
// links below the top level). Files in dst that src doesn't have are kept, as with cp: an output folder
// only loses stale files when it is deleted. (nob's nob_copy_directory_recursively skips symlinks and
// expects a fresh destination, hence this one.)
static bool CopyInto(const char *src, const char *dst)
{
    Nob_File_Paths children = {0};
    if (!nob_read_entire_dir(src, &children)) return false;
    bool ok = true;
    for (size_t i = 0; i < children.count && ok; i++) {
        const char *name = children.items[i];
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        size_t mark = nob_temp_save();
        const char *from = nob_temp_sprintf("%s/%s", src, name);
        const char *to = nob_temp_sprintf("%s/%s", dst, name);
        struct stat st, dt;
        bool exists = lstat(to, &dt) == 0;
        if (lstat(from, &st) != 0) {
            SayErr("cp: cannot stat '%s': %s", from, strerror(errno));
            ok = false;
        } else if (S_ISDIR(st.st_mode)) {
            // a folder: create it unless it is there, then merge its entries
            if (exists && !S_ISDIR(dt.st_mode)) {
                SayErr("cp: cannot overwrite non-directory '%s' with directory '%s'", to, from);
                ok = false;
            } else if (!exists && mkdir(to, st.st_mode & 07777) != 0) {
                SayErr("cp: cannot create directory '%s': %s", to, strerror(errno));
                ok = false;
            } else {
                ok = CopyInto(from, to);
            }
        } else if (S_ISLNK(st.st_mode)) {
            // a symlink: copied as a link with the same target (an existing entry of that name is an error
            // here; the examples' data has no links)
            char target[PATH_MAX];
            ssize_t n = readlink(from, target, sizeof target - 1);
            if (n < 0 || (target[n] = '\0', symlink(target, to) != 0)) {
                SayErr("cp: cannot create symbolic link '%s': %s", to, strerror(errno));
                ok = false;
            }
        } else if (S_ISREG(st.st_mode)) {
            ok = CopyFile(from, to);
        } else {
            SayErr("cp: '%s': not a regular file, directory or symbolic link", from);
            ok = false;
        }
        nob_temp_rewind(mark);
    }
    nob_da_free(children);
    return ok;
}

static bool IsRegularFile(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// bash's [[ a -nt b ]]: a exists and is newer than b (nanosecond mtimes), or a exists and b doesn't.
static bool NewerThan(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) != 0) return false;
    if (stat(b, &sb) != 0) return true;
    if (sa.st_mtim.tv_sec != sb.st_mtim.tv_sec) return sa.st_mtim.tv_sec > sb.st_mtim.tv_sec;
    return sa.st_mtim.tv_nsec > sb.st_mtim.tv_nsec;
}

// "$(cat file)": the file's text with its trailing newlines removed; NULL if it can't be read.
static const char *ReadTrimmed(const char *path)
{
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(path, &sb)) return NULL;
    while (sb.count > 0 && sb.items[sb.count - 1] == '\n') sb.count--;
    nob_sb_append_null(&sb);
    const char *result = nob_temp_strdup(sb.items);
    nob_sb_free(sb);
    return result;
}

// `printf '%s\n' "$text" > path`.
static bool WriteLine(const char *path, const char *text)
{
    Nob_String_Builder sb = {0};
    nob_sb_append_cstr(&sb, text);
    nob_da_append(&sb, '\n');
    bool ok = nob_write_entire_file(path, sb.items, sb.count);
    nob_sb_free(sb);
    return ok;
}

// A command as one line, for a stamp file: arguments joined by spaces.
static const char *StampText(const Nob_Cmd *cmd)
{
    Nob_String_Builder sb = {0};
    for (size_t i = 0; i < cmd->count; i++) {
        if (i > 0) nob_da_append(&sb, ' ');
        nob_sb_append_cstr(&sb, cmd->items[i]);
    }
    nob_sb_append_null(&sb);
    const char *text = nob_temp_strdup(sb.items);
    nob_sb_free(sb);
    return text;
}

// rm -f <path>, for a stamp that is about to become wrong: forgotten before the work it vouches for, so work
// that fails half-way is redone next time instead of being taken for done.
static void ForgetStamp(const char *path)
{
    if (remove(path) != 0 && errno != ENOENT) {
        SayErr("rm: cannot remove '%s': %s", path, strerror(errno));
        Fail();
    }
}

// Whether any prerequisite in the compiler's dependency file <dep> (-MMD -MF, make syntax) is newer than <obj>
// or gone; true also when <dep> can't be read. The same reader as the repo's nob.c (DepsNewer).
static bool DepsNewer(const char *dep, const char *obj)
{
    Nob_String_Builder text = {0};
    if (!nob_read_entire_file(dep, &text)) return true;
    nob_sb_append_null(&text);
    // the rule's target ends at the first ':' followed by a blank or the line's end
    const char *p = text.items;
    while (*p && !(p[0] == ':' && (p[1] == ' ' || p[1] == '\t' || p[1] == '\n' || p[1] == '\\' || p[1] == '\0'))) p++;
    bool newer = *p == '\0';  // no rule: unreadable, compile
    if (*p) p++;
    Nob_String_Builder name = {0};
    while (!newer) {
        // skip blanks and backslash-newline continuations; an unescaped newline ends the rule
        while (*p == ' ' || *p == '\t' || (p[0] == '\\' && p[1] == '\n') || *p == '\r') p += p[0] == '\\' ? 2 : 1;
        if (*p == '\0' || *p == '\n') break;
        name.count = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && !(p[0] == '\\' && p[1] == '\n')) {
            if (p[0] == '\\' && (p[1] == ' ' || p[1] == '#' || p[1] == '\\')) p++;
            else if (p[0] == '$' && p[1] == '$') p++;
            nob_da_append(&name, *p);
            p++;
        }
        nob_sb_append_null(&name);
        struct stat st;
        newer = stat(name.items, &st) != 0 || NewerThan(name.items, obj);
    }
    nob_sb_free(name);
    nob_sb_free(text);
    return newer;
}

// Prints a file to stderr (`cat log >&2`): the scene compile's log when it failed.
static void PrintToStderr(const char *path)
{
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(path, &sb)) return;
    fwrite(sb.items, 1, sb.count, gErr);
    fflush(gErr);
    nob_sb_free(sb);
}

// #endregion

// #region The build: settings per mode, Example, Data, Compiled

// The repo root (absolute, no trailing slash) and examples/ in it, from this program's own path
// (<root>/examples/build/nob).
static const char *gRoot;
static const char *gHere;

// What the mode decides, set once in main() (the script's variables of the same names):
static bool gTest;               // test or lsan: the harness is force-included into every main file
static const char *gCc;          // the compiler
static const char *gOutDir;      // the output folder (examples/build/<linux|win|linux-test|linux-lsan>)
static const char *gExe;         // "" or ".exe"
static Nob_Cmd gCflags;          // compile flags, in compiler order
static Nob_Cmd gIncludes;        // -I R3D's headers, raylib's, the runtime
static Nob_Cmd gHarness;         // -include tests/examples/harness.h in the test builds, else empty
static Nob_Cmd gExtraSrc;        // sources every program links (Linux: src/glibc_compat.c), as objects
static const char *gLibDir;      // R3D's build folder for the target (its lib/ holds the static libraries)
static Nob_Cmd gLibs;            // the link flags after -o
static const char *gMonolith;    // build/linux/monolith, which compiles the scenes

// The path relative to the repo root (the script's ${path#"$ROOT/"}): unchanged when it isn't under it.
static const char *RelativeToRoot(const char *path)
{
    size_t n = strlen(gRoot);
    if (strncmp(path, gRoot, n) == 0 && path[n] == '/') return path + n + 1;
    return path;
}

// A stamp to write once the pool that makes its output has succeeded: <path> gets <text>.
typedef struct {
    const char *path, *text;
} Stamp;

typedef struct {
    Stamp *items;
    size_t count, capacity;
} Stamps;

// What the examples' registration collects, run by Build(): the out-of-date objects' compiles, the
// out-of-date programs' links, the stamps each pool writes when it succeeds, and every object registered so
// far (an object several programs link, the runtime, is compiled once).
static PoolJobs gCompiles, gLinks;
static Stamps gCompileStamps, gLinkStamps;
static Nob_Cmd gObjects;
static size_t gUpToDate;  // programs whose link was skipped: nothing they are made of changed

// Object <flags> <source> <obj>: registers the compile of <source> into <obj> with <flags> (the compiler and
// its flags in compiler order): `<flags> -MMD -MF <obj>.d -c <source> -o <obj>`, compiled by Build() only when
// out of date: the object is missing, older than its source, any header in its .d file (-MMD) or this
// program's source (examples/nob.c: a build change rebuilds everything), or was compiled with other flags
// (<obj>.flags, the stamp, written after the compile succeeded). Registering an object twice is a no-op.
static void Object(const Nob_Cmd *flags, const char *source, const char *obj)
{
    for (size_t i = 0; i < gObjects.count; i++) if (strcmp(gObjects.items[i], obj) == 0) return;
    nob_cmd_append(&gObjects, obj);
    if (!MkdirP(nob_temp_dir_name(obj))) Fail();
    const char *dep = nob_temp_sprintf("%s.d", obj);
    const char *stamp = nob_temp_sprintf("%s.flags", obj);
    const char *text = StampText(flags);
    const char *previous = IsRegularFile(stamp) ? ReadTrimmed(stamp) : NULL;
    if (previous && strcmp(previous, text) == 0 && IsRegularFile(obj) && !NewerThan(source, obj) &&
        !NewerThan(nob_temp_sprintf("%s/nob.c", gHere), obj) && !DepsNewer(dep, obj)) {
        return;
    }
    ForgetStamp(stamp);
    PoolJob job = {.label = nob_temp_sprintf("compile %s", RelativeToRoot(obj))};
    nob_da_append_many(&job.cmd, flags->items, flags->count);
    nob_cmd_append(&job.cmd, "-MMD", "-MF", dep, "-c", source, "-o", obj);
    nob_da_append(&gCompiles, job);
    nob_da_append(&gCompileStamps, ((Stamp){stamp, text}));
}

// The runtime's objects for one flag set (editor: -DMONOLITH_EDITOR, the runtime with hot reload): registers
// them and appends their paths to <objects>: runtime/monolith_scene.c and the extra sources (Linux:
// src/glibc_compat.c), compiled with $CC CFLAGS INCLUDES -DMS_PATCHED_R3D [-DMONOLITH_EDITOR], the flags the
// link command compiled them with when they were sources on it. <out>-obj/runtime/ or runtime-editor/.
static void RuntimeObjects(bool editor, bool runtime, Nob_Cmd *objects)
{
    Nob_Cmd flags = {0};
    nob_cmd_append(&flags, gCc);
    nob_da_append_many(&flags, gCflags.items, gCflags.count);
    nob_da_append_many(&flags, gIncludes.items, gIncludes.count);
    nob_cmd_append(&flags, "-DMS_PATCHED_R3D");
    if (editor) nob_cmd_append(&flags, "-DMONOLITH_EDITOR");
    const char *dir = nob_temp_sprintf("%s-obj/%s", gOutDir, editor ? "runtime-editor" : "runtime");
    Nob_Cmd sources = {0};
    if (runtime) nob_cmd_append(&sources, nob_temp_sprintf("%s/runtime/monolith_scene.c", gRoot));
    nob_da_append_many(&sources, gExtraSrc.items, gExtraSrc.count);
    for (size_t i = 0; i < sources.count; i++) {
        const char *base = strrchr(sources.items[i], '/') + 1;
        const char *obj = nob_temp_sprintf("%s/%.*s.o", dir, (int)(strlen(base) - 2), base);
        Object(&flags, sources.items[i], obj);
        nob_cmd_append(objects, obj);
    }
    nob_cmd_free(sources);
    nob_cmd_free(flags);
}

// Whether <obj> is queued for compiling (each compile command ends with `-o <obj>`): the program it goes into
// must then be linked again, though the object on disk is still the old one when Example() checks.
static bool WillCompile(const char *obj)
{
    for (size_t i = 0; i < gCompiles.count; i++) {
        const Nob_Cmd *c = &gCompiles.items[i].cmd;
        if (strcmp(c->items[c->count - 1], obj) == 0) return true;
    }
    return false;
}

// Example <dir> <program> <main file> <flags> [runtime]: registers the main file's compile on its own (in the
// test builds with the harness force-included, which must not reach the runtime) and the link with the
// runtime's objects (when `runtime`) and the extra sources' objects. `flags` is NULL-terminated:
// -DMONOLITH_EDITOR for the editor runtime (its objects are the editor set, and the link gets the flag too, as
// before), -DHARNESS_MAIN_NO_ARGS for a main(void), -include ... for a shim. The link is registered only when
// the program is out of date: missing, linked with another command (<obj dir>/<program>.link, the stamp),
// one of its objects about to be compiled (WillCompile), or older than one of its objects, R3D's libraries
// or this program's source. Returns the program's path, for
// the "built: <path>" line main() prints after Build().
static const char *Example(const char *dir, const char *prog, const char *mainFile, const char **flags, bool runtime)
{
    bool editor = false;
    for (const char **f = flags; *f; f++) editor = editor || strcmp(*f, "-DMONOLITH_EDITOR") == 0;

    // mkdir -p <out>/<dir>: the program's folder (its object goes to <out>-obj/<dir>/, outside the program
    // folders, which stay copyable)
    const char *progDir = nob_temp_sprintf("%s/%s", gOutDir, dir);
    if (!MkdirP(progDir)) Fail();
    const char *obj = nob_temp_sprintf("%s-obj/%s/%s.o", gOutDir, dir, prog);
    const char *exe = nob_temp_sprintf("%s/%s%s", progDir, prog, gExe);

    // compile: $CC CFLAGS INCLUDES -DMS_PATCHED_R3D <flags> HARNESS -c <main> -o <obj>
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, gCc);
    nob_da_append_many(&cmd, gCflags.items, gCflags.count);
    nob_da_append_many(&cmd, gIncludes.items, gIncludes.count);
    nob_cmd_append(&cmd, "-DMS_PATCHED_R3D");
    for (const char **f = flags; *f; f++) nob_cmd_append(&cmd, *f);
    nob_da_append_many(&cmd, gHarness.items, gHarness.count);
    Object(&cmd, mainFile, obj);
    Nob_Cmd objects = {0};
    nob_cmd_append(&objects, obj);
    RuntimeObjects(editor, runtime, &objects);

    // link: $CC CFLAGS INCLUDES -DMS_PATCHED_R3D [-DMONOLITH_EDITOR] <obj> [runtime] <extra> -o <exe> LIBS
    cmd.count = 0;
    nob_cmd_append(&cmd, gCc);
    nob_da_append_many(&cmd, gCflags.items, gCflags.count);
    nob_da_append_many(&cmd, gIncludes.items, gIncludes.count);
    nob_cmd_append(&cmd, "-DMS_PATCHED_R3D");
    if (editor) nob_cmd_append(&cmd, "-DMONOLITH_EDITOR");
    nob_da_append_many(&cmd, objects.items, objects.count);
    nob_cmd_append(&cmd, "-o", exe);
    nob_da_append_many(&cmd, gLibs.items, gLibs.count);

    const char *stamp = nob_temp_sprintf("%s-obj/%s/%s.link", gOutDir, dir, prog);
    const char *text = StampText(&cmd);
    const char *previous = IsRegularFile(stamp) ? ReadTrimmed(stamp) : NULL;
    bool stale = !IsRegularFile(exe) || !previous || strcmp(previous, text) != 0 ||
                 NewerThan(nob_temp_sprintf("%s/nob.c", gHere), exe);
    for (size_t i = 0; i < objects.count && !stale; i++) {
        stale = WillCompile(objects.items[i]) || NewerThan(objects.items[i], exe);
    }
    const char *libs[] = {"r3d", "raylib", "assimp", "zlibstatic"};
    for (size_t i = 0; i < NOB_ARRAY_LEN(libs) && !stale; i++) {
        stale = NewerThan(nob_temp_sprintf("%s/lib/lib%s.a", gLibDir, libs[i]), exe);
    }
    if (stale) {
        ForgetStamp(stamp);
        PoolJob job = {.cmd = cmd, .label = nob_temp_sprintf("link %s", RelativeToRoot(exe))};
        nob_da_append(&gLinks, job);
        nob_da_append(&gLinkStamps, ((Stamp){stamp, text}));
    } else {
        gUpToDate++;
        nob_cmd_free(cmd);
    }
    nob_cmd_free(objects);
    return exe;
}

// Writes the stamps of a pool that succeeded.
static void WriteStamps(Stamps *stamps)
{
    for (size_t i = 0; i < stamps->count; i++) {
        if (!WriteLine(stamps->items[i].path, stamps->items[i].text)) Fail();
    }
    stamps->count = 0;
}

// Runs what the examples' registration collected: the out-of-date objects in one pool (each runtime flag set
// compiled once, with every program's main file), then the out-of-date programs' links in a second pool.
static void Build(void)
{
    size_t size = PoolSize();
    if (gCompiles.count > 0) {
        // runtime/monolith_scene.c (by far the longest) first: the pool's wall time is bounded by it
        size_t front = 0;
        for (size_t i = 0; i < gCompiles.count; i++) {
            if (!strstr(gCompiles.items[i].label, "/monolith_scene.o")) continue;
            PoolJob job = gCompiles.items[i];
            memmove(&gCompiles.items[front + 1], &gCompiles.items[front], (i - front) * sizeof job);
            gCompiles.items[front++] = job;
        }
        if (!RunPool(&gCompiles, "compile the examples' objects", size)) Fail();
        WriteStamps(&gCompileStamps);
    }
    if (gLinks.count > 0) {
        if (!RunPool(&gLinks, "link the examples", size)) Fail();
        WriteStamps(&gLinkStamps);
    }
    if (gUpToDate > 0) Say("up to date: %zu of the programs (no object or library changed: not linked)", gUpToDate);
}

// Data <dir> <from> <to>: the data a program loads, next to it (<out>/<dir>/<to>/). `from` is relative to
// examples/. Copied into the folder like `cp -r <from>/. <to>/`: files of an older build that the source no
// longer has stay until the output folder is deleted.
static void Data(const char *dir, const char *from, const char *to)
{
    const char *dst = nob_temp_sprintf("%s/%s/%s", gOutDir, dir, to);
    if (!MkdirP(dst)) Fail();
    if (!CopyInto(nob_temp_sprintf("%s/%s", gHere, from), dst)) Fail();
}

// Compiled <dir> <package>: compiles the copied package's only .map into <name>.mapb next to it with
// build/linux/monolith --compile (a game build of the runtime reads only compiled maps). Its output goes to
// <out>-obj/compile.log and is printed only when the compile fails. Prints "compiled: <path>".
static void Compiled(const char *dir, const char *package)
{
    // the package's .map files (bash's "$pkg"/*.map: no dot files, sorted): there must be exactly one
    const char *pkg = nob_temp_sprintf("%s/%s/%s", gOutDir, dir, package);
    glob_t g;
    int found = glob(nob_temp_sprintf("%s/*.map", pkg), 0, NULL, &g);
    struct stat st;
    bool one = found == 0 && g.gl_pathc == 1 && stat(g.gl_pathv[0], &st) == 0 && S_ISREG(st.st_mode);
    const char *map = one ? nob_temp_strdup(g.gl_pathv[0]) : NULL;
    if (found == 0) globfree(&g);
    if (!one) {
        SayErr("%s must hold exactly one .map file", pkg);
        Fail();
    }

    // monolith --map <map> --compile <map>b > <out>-obj/compile.log 2>&1, the log on stderr if it fails
    const char *mapb = nob_temp_sprintf("%sb", map);
    const char *log = nob_temp_sprintf("%s-obj/compile.log", gOutDir);
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, gMonolith, "--map", map, "--compile", mapb);
    if (!RunTimed(&cmd, nob_temp_sprintf("compile scene %s", RelativeToRoot(mapb)), log)) {
        PrintToStderr(log);
        Fail();
    }
    nob_cmd_free(cmd);
    Say("compiled: %s", RelativeToRoot(mapb));
}

// #endregion

int main(int argc, char **argv)
{
    gStartNanos = nob_nanos_since_unspecified_epoch();
    // The terminal's stdout / stderr, kept for our messages while the scene compile's output goes to its log
    gOut = fdopen(dup(STDOUT_FILENO), "w");
    gErr = fdopen(dup(STDERR_FILENO), "w");
    if (!gOut || !gErr) {
        fprintf(stderr, "build.sh: examples: can't duplicate stdout/stderr: %s\n", strerror(errno));
        return 1;
    }
    nob_set_log_handler(LogHandler);

    // The repo root from this program's path (<root>/examples/build/nob): nob.c runs it by its absolute path;
    // run directly, the path is taken relative to the current directory. Lexically normalised without
    // resolving symlinks, so every path in the commands is the one `cd "$(dirname ...)" && pwd` gave.
    if (!strchr(argv[0], '/')) {
        SayErr("run me by my path (./build.sh examples, or examples/build/nob)");
        return 1;
    }
    const char *self = argv[0][0] == '/' ? argv[0] : nob_temp_sprintf("%s/%s", LogicalCwd(), argv[0]);
    self = NormalizeAbsolute(self);
    gHere = nob_temp_dir_name(nob_temp_dir_name(self));
    gRoot = nob_temp_dir_name(gHere);

    // Rebuild examples/build/nob first when examples/nob.c or nob.h is newer (only when run directly: nob.c
    // already recompiled it), with absolute paths so it works from any directory. Not timed.
    nob__go_rebuild_urself(argc, argv, nob_temp_sprintf("%s/nob.c", gHere),
                           nob_temp_sprintf("%s/src/vendor/nob/nob.h", gRoot), NULL);

    // The mode: only the first argument counts (as the script's $1); anything else is the plain build
    const char *mode = argc > 1 ? argv[1] : "";
    bool win = strcmp(mode, "--win") == 0;
    bool lsan = strcmp(mode, "lsan") == 0;
    gTest = lsan || strcmp(mode, "test") == 0;
    const char *r3d = Env("R3D_DIR", nob_temp_sprintf("%s/submodules/r3d", gRoot));

    // Compile flags: monolith's (C11, every warning, -O2); lsan: -g -O1 with frame pointers and
    // LeakSanitizer, so the leak check's stacks are readable
    if (lsan) {
        nob_cmd_append(&gCflags, "-std=c11", "-Wall", "-Wextra", "-Wno-missing-field-initializers", "-g", "-O1",
                       "-fno-omit-frame-pointer", "-fsanitize=leak");
    } else {
        nob_cmd_append(&gCflags, "-std=c11", "-Wall", "-Wextra", "-Wno-missing-field-initializers", "-O2");
    }

    // Per target: the compiler, R3D's library folder, the output folder and the link flags
    const char *libDir;  // (gLibDir, for the link's out-of-date check)
    if (win) {
        // MinGW-w64, fully static (-s -static): the .exe needs only system DLLs
        gCc = "x86_64-w64-mingw32-gcc";
        libDir = nob_temp_sprintf("%s/build-mingw", r3d);
        gOutDir = nob_temp_sprintf("%s/build/win", gHere);
        gExe = ".exe";
        nob_cmd_append(&gLibs, "-s", "-static", nob_temp_sprintf("-L%s/lib", libDir), "-lr3d", "-lraylib",
                       "-lassimp", "-lzlibstatic", "-lstdc++", "-lopengl32", "-lgdi32", "-lwinmm", "-lshell32", "-lm");
    } else {
        gCc = Env("CC", "clang");
        libDir = nob_temp_sprintf("%s/build-linux", r3d);
        gOutDir = nob_temp_sprintf("%s/build/%s", gHere, lsan ? "linux-lsan" : gTest ? "linux-test" : "linux");
        gExe = "";
        // Like the repo's build: libstdc++ static, symbols newer than glibc 2.34 routed to old versions
        // through -Wl,--wrap= and src/glibc_compat.c (nob.c's glibcWrap is the same list: keep them in step)
        const char *glibcWrap[] = {"acosf", "asinf", "atan2f", "sqrtf", "fmod", "fmodf", "hypot", "hypotf",
                                   "__isoc23_strtoul", "__isoc23_strtol", "arc4random"};
        Nob_String_Builder wrap = {0};
        nob_sb_append_cstr(&wrap, "-Wl");
        for (size_t i = 0; i < NOB_ARRAY_LEN(glibcWrap); i++) nob_sb_appendf(&wrap, ",--wrap=%s", glibcWrap[i]);
        nob_sb_append_null(&wrap);
        nob_cmd_append(&gExtraSrc, nob_temp_sprintf("%s/src/glibc_compat.c", gRoot));
        nob_cmd_append(&gLibs, nob_temp_sprintf("-L%s/lib", libDir), "-lr3d", "-lraylib", "-lassimp",
                       "-lzlibstatic", "-l:libstdc++.a", "-lgcc_s", "-lGL", "-lm", "-lpthread", "-ldl", wrap.items);
    }
    gLibDir = libDir;

    // What must exist first: monolith (it compiles the scenes, also for --win) and R3D's libraries for the
    // target, both made by the repo's ./build.sh (--win)
    gMonolith = nob_temp_sprintf("%s/build/linux/monolith", gRoot);
    if (access(gMonolith, X_OK) != 0) {
        SayErr("%s not found (it compiles the examples' scenes): run ./build.sh in the repo root first", gMonolith);
        return 1;
    }
    struct stat st;
    const char *libR3d = nob_temp_sprintf("%s/lib/libr3d.a", libDir);
    if (stat(libR3d, &st) != 0 || !S_ISREG(st.st_mode)) {
        SayErr("%s not found: run ./build.sh%s in the repo root first", libR3d, win ? " --win" : "");
        return 1;
    }
    nob_cmd_append(&gIncludes, nob_temp_sprintf("-I%s/include", r3d),
                   nob_temp_sprintf("-I%s/external/raylib/raylib/include", libDir),
                   nob_temp_sprintf("-I%s/runtime", gRoot));
    if (gTest) nob_cmd_append(&gHarness, "-include", nob_temp_sprintf("%s/tests/examples/harness.h", gRoot));

    // The examples, in the script's order (nob.c mirrors these programs and their flags in
    // compile_commands.json for clangd: keep it in step). Each flag list is NULL-terminated.
    const char *none[] = {NULL};
    const char *noArgs[] = {"-DHARNESS_MAIN_NO_ARGS", NULL};
    const char *editor[] = {"-DMONOLITH_EDITOR", NULL};

    // First every program is registered (its objects and its link), then Build() compiles all out-of-date
    // objects in one pool and links in a second; the data copies and scene compiles follow, per example in
    // the script's order.
    const char *minimal = Example("01_minimal", "minimal", nob_temp_sprintf("%s/01_minimal/main.c", gHere), none, true);
    const char *before = Example("02_existing_project", "before",
                                 nob_temp_sprintf("%s/02_existing_project/before.c", gHere), noArgs, false);
    const char *after = Example("02_existing_project", "after",
                                nob_temp_sprintf("%s/02_existing_project/after.c", gHere), none, true);
    const char *live = Example("03_live_editing", "live_editing", nob_temp_sprintf("%s/03_live_editing/main.c", gHere),
                               editor, true);
    const char *entities = Example("05_entities", "entities", nob_temp_sprintf("%s/05_entities/main.c", gHere), none,
                                   true);
    Build();

    Say("built: %s", RelativeToRoot(minimal));
    Data("01_minimal", "01_minimal/scene", "scene");
    Compiled("01_minimal", "scene");

    Say("built: %s", RelativeToRoot(before));
    Say("built: %s", RelativeToRoot(after));
    Data("02_existing_project", "01_minimal/scene", "scene");
    Compiled("02_existing_project", "scene");

    Say("built: %s", RelativeToRoot(live));
    Data("03_live_editing", "01_minimal/scene", "scene");

    Say("built: %s", RelativeToRoot(entities));
    Data("05_entities", "01_minimal/scene", "scene");
    Compiled("05_entities", "scene");

    PrintSummary();
    return 0;
}
