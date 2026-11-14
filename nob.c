// nob.c: monolith's build program. ./build.sh compiles it into build/nob and runs it with its arguments;
// everything the build does is here, written on nob.h (src/vendor/nob/nob.h, vendored unmodified).
//
// Modes (the arguments of ./build.sh, unchanged from the shell script this replaces):
//   ./build.sh                -> Linux x86_64 build in build/linux/: monolith (R3D, raylib, assimp and
//                                libstdc++ linked statically) + maps/ (map packages); the folder can be
//                                copied as-is. Also writes compile_commands.json in the repo root for
//                                clangd (WriteCompileCommands below).
//   ./build.sh run [args]     -> build, then run the game with args (replaces this process)
//   ./build.sh debug          -> debug build (-g -O0, AddressSanitizer + UBSan, leak check at exit) in
//                                build/linux-debug/
//   ./build.sh lsan           -> LeakSanitizer-only build (-O1 -g, fast) in build/linux-lsan/, what
//                                build/tools/leakcheck runs
//   ./build.sh profile        -> CPU-profiling build in build/linux-profile/: -O2 -g -fno-omit-frame-pointer,
//                                with R3D/raylib/assimp as RelWithDebInfo + frame pointers in
//                                $R3D_DIR/build-linux-profile (so build-linux stays Release), plus the
//                                profiling tool build/linux-profile/profile (tools/profile.c, libc only)
//   ./build.sh --win          -> Windows x86_64 build in build/win/ (MinGW-w64): monolith.exe (statically
//                                linked) + maps/ (packages); the folder can be zipped and copied as-is.
//                                It is the view-only editor (benchmark, view, debug a map; no editing or
//                                requests: MONOLITH_EDITING in src/common.h, src/main.c "View-only build")
//   ./build.sh --win run [args]  -> Windows build, then run it under wine (replaces this process): the way to
//                                open the view-only build on Linux (from an agent's shell only with --shot or
//                                --bench --headless, as the Linux build)
//   ./build.sh --win debug|lsan  -> refused: the sanitizers don't work with MinGW
//   ./build.sh --tools [name...] -> the small programs that replaced the repo's shell scripts (TOOLS below;
//                                docs/architecture.md, "Tools"): tools/<name>.c into build/tools/<name>,
//                                tools/run-sh.c into build/run-sh.o. All of them, or only the ones named;
//                                only what is missing or older than its source, tools/tool.h or nob.h is
//                                compiled. The plain build doesn't build them. Each program then rebuilds
//                                itself when its source changes (tools/tool.h, ToolRebuildSelf).
//   ./build.sh examples [--win | test | lsan] -> the examples (examples/nob.c, compiled into examples/build/nob
//                                when missing or older, then run with these arguments; its header comment
//                                lists what each mode builds)
//   ./build.sh test [name...] [-- args...] -> the unit tests in tests/unit/ (UNIT_TESTS, region "Build steps"):
//                                all of them or the ones named, each built with ASan + UBSan into
//                                build/unit/<name>/ and run; exit 1 when any fails. Arguments after `--` are
//                                added to the run of every test that runs (use it with one named test:
//                                `./build.sh test paintunwrap -- <model files>`, `./build.sh test export_strip
//                                -- <export dir>`)
// Any other first argument builds like the plain Linux build but writes no compile_commands.json (as the
// shell script did: only "" and "run" write it).
//
// Environment:
//   R3D_DIR         another R3D tree (default: submodules/r3d, with local fixes, see its patches.md)
//   CC, CXX         the Linux compilers (default clang; CXX follows CC: clang -> clang++, gcc -> g++,
//                   otherwise c++). --win always uses x86_64-w64-mingw32-gcc/g++.
//   R3D_BUILD_TYPE  (Linux only) CMake build type of R3D/raylib/assimp, default Release.
//                   R3D_BUILD_TYPE=None ./build.sh -> the old unoptimised build (no CMAKE_BUILD_TYPE), only
//                   for comparisons with benchmark reports from before 2026-10-10. profile ignores it
//                   (always RelWithDebInfo, in its own build dir).
//
// R3D is built on first use as static libs: into $R3D_DIR/build-linux for Linux and into
// $R3D_DIR/build-mingw for --win (toolchain: cmake/mingw-w64-x86_64.cmake). Later runs only rebuild what
// changed; if the configure options changed (e.g. R3D_BUILD_TYPE), the build dir is re-configured in place
// (a full R3D rebuild). Delete those directories to force a fresh configure.
// The vendored xatlas (src/vendor/xatlas/, MIT; C++, the paint unwrap) is compiled once per build folder
// into <build folder>-obj/xatlas.o with its own flags (Xatlas below) and linked in.
// The runtime (runtime/monolith_scene.c) is built with -DMONOLITH_EDITOR: hot reload, source lines and the
// writers; --export writes it without them (src/export.c).
//
// Timing: every command runs through RunTimed(), which prints after it how long it took (wall time) and
// how much CPU time it and every process it waited for used (user + sys, from getrusage(RUSAGE_CHILDREN)),
// and at the end a summary table. nob.h measures neither: its nob_cmd_run() forks and waitpid()s, which
// returns no resource usage. The commands run one after another (never nob's .async), so the
// RUSAGE_CHILDREN difference around one command is that command's own: `cmake --build -j` counts all of its
// compiler processes, because make waits for them and cmake waits for make. No CPU cycle counts: the
// developer chose time only (2026-11-09).
//
// Output: nob's "[INFO] CMD: ..." echo of each command and the "[TIME]" lines go to stdout, like the
// script's progress lines ("configuring ...", "built: ...", whose text scripts and people read); warnings and
// errors go to stderr. So `./build.sh lsan > /dev/null` (build/tools/leakcheck) stays quiet unless something
// fails.
//
// Adding a mode or a flag: a flag goes into the argument list of the command it belongs to (CFLAGS-like
// lists are built in BuildLinux / BuildWindows, in the same order the compiler sees them; keep the
// compile_commands.json entries in step when a list they copy changes). A mode is a branch in main() or
// BuildLinux that picks its output folder and flags; run every command through RunTimed() (never
// nob_cmd_run directly, never .async) so it is timed and stays sequential.

// sched_getaffinity / CPU_COUNT (the -j count, like nproc), nftw. Before nob.h, which only sets
// _POSIX_C_SOURCE when nothing else has picked a feature level.
#define _GNU_SOURCE

// nob.h's command echo is replaced by RunTimed's own (the same "CMD: ..." text, but printed before RunTimed
// points stdout/stderr at a log file, so it reaches the terminal), and its file helpers' "[INFO] created
// directory ..." chatter isn't wanted: nob.c has its own file operations below.
#define NOB_NO_ECHO
// How nob rebuilds itself when nob.c or nob.h is newer than build/nob (NOB_GO_REBUILD_URSELF, e.g. when
// build/nob is run directly): the same compiler and flags ./build.sh uses for it (${CC:-clang}, C11, all
// warnings, -O1: the build program does little work of its own, so the cheap optimisation level is enough).
#define NOB_REBUILD_URSELF(binary_path, source_path) \
    (getenv("CC") && *getenv("CC") ? getenv("CC") : "clang"), \
    "-std=c11", "-Wall", "-Wextra", "-O1", "-o", (binary_path), (source_path)
#define NOB_IMPLEMENTATION
#include "src/vendor/nob/nob.h"

#include <fcntl.h>
#include <ftw.h>
#include <glob.h>
#include <limits.h>
#include <locale.h>
#include <sched.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

// #region Console, logging and timing

// The terminal's stdout and stderr, duplicated at start: RunTimed points fds 1 and 2 at a log file while a
// command whose output goes to a log runs, and nob's messages (and ours) must still reach the terminal.
static FILE *gOut;
static FILE *gErr;

// One timed command, for the summary table.
typedef struct {
    const char *label;
    double wall, user, sys;   // seconds
    bool ok;
} Timing;

typedef struct {
    Timing *items;
    size_t count, capacity;
} Timings;

static Timings gTimings;
static uint64_t gStartNanos;  // when main() started, for the whole run's wall time in the summary

// nob's log handler: "[INFO] ..." to the terminal's stdout, warnings and errors to its stderr.
static void LogHandler(Nob_Log_Level level, const char *fmt, va_list args)
{
    FILE *f = level == NOB_INFO ? gOut : gErr;
    switch (level) {
    case NOB_INFO:    fprintf(f, "[INFO] "); break;
    case NOB_WARNING: fprintf(f, "[WARNING] "); break;
    case NOB_ERROR:   fprintf(f, "[ERROR] "); break;
    case NOB_NO_LOGS: return;
    default:          NOB_UNREACHABLE("Nob_Log_Level");
    }
    vfprintf(f, fmt, args);
    fprintf(f, "\n");
    fflush(f);
}

// Our own progress lines (the shell script's `echo`s): to the terminal's stdout.
NOB_PRINTF_FORMAT(1, 2) static void Say(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(gOut, fmt, args);
    va_end(args);
    fputc('\n', gOut);
    fflush(gOut);
}

// Error lines (the script's `echo ... >&2`): to the terminal's stderr.
NOB_PRINTF_FORMAT(1, 2) static void SayErr(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(gErr, fmt, args);
    va_end(args);
    fputc('\n', gErr);
    fflush(gErr);
}

static double Seconds(struct timeval tv)
{
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

// A command's output: NULL for the terminal, or a log file (both stdout and stderr, like `> log 2>&1`;
// append = `>> log 2>&1`).
typedef struct {
    const char *log;
    bool append;
} Output;

// Runs one command (resetting it, as nob_cmd_run does) and records its time. The only place a command runs:
// prints nob's "[INFO] CMD: ..." echo, runs it synchronously, then prints
// "[TIME] <label>: <wall> s wall, <cpu> s CPU (<user> user + <sys> sys)".
// CPU time is the RUSAGE_CHILDREN difference: the command and every descendant it waited for. That is
// exact only because nothing else runs at the same time (no .async anywhere).
static bool RunTimed(Nob_Cmd *cmd, const char *label, Output out)
{
    Nob_String_Builder echo = {0};
    nob_cmd_render(*cmd, &echo);
    nob_sb_append_null(&echo);
    nob_log(NOB_INFO, "CMD: %s", echo.items);
    nob_sb_free(echo);

    // `> log 2>&1` / `>> log 2>&1`: point this process's fds 1 and 2 at the log for the duration of the
    // command, so the child inherits them. (nob's .stdout_path / .stderr_path can't do this: each opens the
    // file on its own with O_TRUNC, so the two streams would overwrite each other, and there's no append.)
    int savedOut = -1, savedErr = -1;
    fflush(stdout);
    fflush(stderr);
    if (out.log) {
        int fd = open(out.log, O_WRONLY | O_CREAT | (out.append ? O_APPEND : O_TRUNC), 0666);
        if (fd < 0) {
            nob_log(NOB_ERROR, "could not open %s: %s", out.log, strerror(errno));
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

    if (out.log) {
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
    fprintf(gOut, "[TIME] %s: %.2f s wall, %.2f s CPU (%.2f user + %.2f sys)%s\n", label, t.wall, t.user + t.sys,
            t.user, t.sys, ok ? "" : " (failed)");
    fflush(gOut);
    return ok;
}

// The summary table of every timed command and their total, plus the whole run's wall time (which also
// covers nob's own work: the maps/ copy, compile_commands.json). Printed at the end of a build, before
// `run` hands over to the game, and before a failed build exits.
static void PrintSummary(void)
{
    if (gTimings.count == 0) return;
    size_t width = strlen("total");
    for (size_t i = 0; i < gTimings.count; i++) {
        size_t n = strlen(gTimings.items[i].label);
        if (n > width) width = n;
    }
    double wall = 0, user = 0, sys = 0;
    fprintf(gOut, "[TIME] summary (wall = elapsed; CPU = user + sys of the command and every process it waited for)\n");
    fprintf(gOut, "  %-*s %9s %9s %9s %9s\n", (int)width, "command", "wall s", "CPU s", "user s", "sys s");
    for (size_t i = 0; i < gTimings.count; i++) {
        Timing *t = &gTimings.items[i];
        fprintf(gOut, "  %-*s %9.2f %9.2f %9.2f %9.2f%s\n", (int)width, t->label, t->wall, t->user + t->sys,
                t->user, t->sys, t->ok ? "" : "  (failed)");
        wall += t->wall;
        user += t->user;
        sys += t->sys;
    }
    fprintf(gOut, "  %-*s %9.2f %9.2f %9.2f %9.2f\n", (int)width, "total", wall, user + sys, user, sys);
    double whole = (double)(nob_nanos_since_unspecified_epoch() - gStartNanos) / 1e9;
    fprintf(gOut, "[TIME] whole run: %.2f s wall (commands %.2f s, nob's own file work and checks %.2f s)\n",
            whole, wall, whole - wall);
    fflush(gOut);
}

// Exit after a failure, with the summary of what ran so far.
static void Fail(int code)
{
    PrintSummary();
    exit(code);
}

// #endregion

// #region Paths and files (the shell script's mkdir -p, rm -rf, cp, cp -r, [[ -nt ]], $(cat))

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

// The current directory as the shell names it: $PWD when it is absolute and names this directory (so a path
// through a symlink stays as the user typed it, like bash's pwd), else getcwd().
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
        SayErr("build: can't read the current directory");
        exit(1);
    }
    return cwd;
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

// `mkdir -p path`: creates the directory and any missing parents (mode 0777 minus the umask, like mkdir).
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

static int RemoveEntry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st;
    (void)type;
    (void)ftw;
    if (remove(path) != 0) {
        SayErr("rm: cannot remove '%s': %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

// `rm -rf path` for one path of the build's own output (build/<target>/maps, resources, runtime): deletes
// it and everything under it, children first, without following symlinks; a missing path is fine.
static bool RemoveTree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT;
    return nftw(path, RemoveEntry, 64, FTW_DEPTH | FTW_PHYS) == 0;
}

// `cp src dst` for one regular file: dst is created with src's mode (minus the umask) or truncated and
// overwritten if it exists (keeping its mode), as cp does without -p. Timestamps are new, like cp's.
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

// `cp -r src dst` (dst doesn't exist yet): every entry, dot files included (maps/<name> may be a git checkout
// with its .git), directories with their source mode, symlinks copied as symlinks (not followed, like
// GNU cp -r below the top level). nob's nob_copy_directory_recursively skips symlinks, hence this one.
static bool CopyTree(const char *src, const char *dst)
{
    struct stat st;
    if (lstat(src, &st) != 0) {
        SayErr("cp: cannot stat '%s': %s", src, strerror(errno));
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t n = readlink(src, target, sizeof target - 1);
        if (n < 0) {
            SayErr("cp: cannot read symbolic link '%s': %s", src, strerror(errno));
            return false;
        }
        target[n] = '\0';
        if (symlink(target, dst) != 0) {
            SayErr("cp: cannot create symbolic link '%s': %s", dst, strerror(errno));
            return false;
        }
        return true;
    }
    if (S_ISREG(st.st_mode)) return CopyFile(src, dst);
    if (!S_ISDIR(st.st_mode)) {
        SayErr("cp: '%s': not a regular file, directory or symbolic link", src);
        return false;
    }
    if (mkdir(dst, st.st_mode & 07777) != 0) {
        SayErr("cp: cannot create directory '%s': %s", dst, strerror(errno));
        return false;
    }
    Nob_File_Paths children = {0};
    if (!nob_read_entire_dir(src, &children)) return false;
    bool ok = true;
    for (size_t i = 0; i < children.count && ok; i++) {
        const char *name = children.items[i];
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        size_t mark = nob_temp_save();
        ok = CopyTree(nob_temp_sprintf("%s/%s", src, name), nob_temp_sprintf("%s/%s", dst, name));
        nob_temp_rewind(mark);
    }
    nob_da_free(children);
    return ok;
}

// "$dir"/*.c: appends the matching paths to cmd in the order bash's glob gives them (sorted by the locale's
// collation, LC_COLLATE, which main() takes from the environment as bash does); no match leaves the pattern
// itself, like bash without nullglob.
static void AppendGlob(Nob_Cmd *cmd, const char *pattern)
{
    glob_t g;
    if (glob(pattern, GLOB_NOCHECK, NULL, &g) != 0) {
        nob_cmd_append(cmd, nob_temp_strdup(pattern));
        return;
    }
    for (size_t i = 0; i < g.gl_pathc; i++) nob_cmd_append(cmd, nob_temp_strdup(g.gl_pathv[i]));
    globfree(&g);
}

// ${NAME:-fallback}: the variable when it is set and not empty.
static const char *Env(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return v && *v ? v : fallback;
}

// ${s/from/to}: s with the first occurrence of from replaced by to (s itself when from doesn't occur).
static const char *ReplaceFirst(const char *s, const char *from, const char *to)
{
    const char *at = strstr(s, from);
    if (!at) return s;
    return nob_temp_sprintf("%.*s%s%s", (int)(at - s), s, to, at + strlen(from));
}

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

// `nproc`: the CPUs this process may run on (its affinity mask), overridden by OMP_NUM_THREADS and capped by
// OMP_THREAD_LIMIT as GNU nproc does. (nproc's cgroup v2 CPU-quota rule isn't mirrored; a build in a
// CPU-limited container then gets a higher -j than the script gave, which changes no output.)
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

// Prints a log file to stdout: all of it (`cat log`) or its last `lines` lines (`tail -<lines> log`).
static void PrintLog(const char *path, size_t lines)
{
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(path, &sb)) return;
    size_t start = 0;
    if (lines > 0 && sb.count > 0) {
        size_t seen = 0;
        size_t i = sb.count;
        if (sb.items[i - 1] == '\n') i--;   // the last line's own newline doesn't start another line
        while (i > 0) {
            if (sb.items[i - 1] == '\n' && ++seen == lines) break;
            i--;
        }
        start = i;
    }
    fwrite(sb.items + start, 1, sb.count - start, gOut);
    fflush(gOut);
    nob_sb_free(sb);
}

// #endregion

// #region compile_commands.json (a reader and writer for exactly what the old embedded Python did)

// A JSON value as read from R3D's compile_commands.json, in file order. Strings are decoded UTF-8 (len
// bytes, may contain NUL); numbers, true, false and null are kept as their source text (cmake writes none).
typedef enum { JSON_STRING, JSON_RAW, JSON_ARRAY, JSON_OBJECT } JsonKind;

typedef struct Json Json;
typedef struct {
    Json *items;
    size_t count, capacity;
} JsonList;

struct Json {
    JsonKind kind;
    const char *text; size_t len;    // JSON_STRING / JSON_RAW
    const char *key; size_t keyLen;  // set when this value is an object member
    JsonList items;                  // JSON_ARRAY elements or JSON_OBJECT members
};

typedef struct {
    const char *start, *p, *end;
    const char *path;  // for messages
} JsonReader;

static bool JsonFail(JsonReader *r, const char *what)
{
    SayErr("build: %s: %s at byte %zu", r->path, what, (size_t)(r->p - r->start));
    return false;
}

static void JsonSkipSpace(JsonReader *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) r->p++;
}

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void PutUtf8(Nob_String_Builder *sb, uint32_t c)
{
    if (c < 0x80) {
        nob_da_append(sb, (char)c);
    } else if (c < 0x800) {
        nob_da_append(sb, (char)(0xC0 | (c >> 6)));
        nob_da_append(sb, (char)(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        nob_da_append(sb, (char)(0xE0 | (c >> 12)));
        nob_da_append(sb, (char)(0x80 | ((c >> 6) & 0x3F)));
        nob_da_append(sb, (char)(0x80 | (c & 0x3F)));
    } else {
        nob_da_append(sb, (char)(0xF0 | (c >> 18)));
        nob_da_append(sb, (char)(0x80 | ((c >> 12) & 0x3F)));
        nob_da_append(sb, (char)(0x80 | ((c >> 6) & 0x3F)));
        nob_da_append(sb, (char)(0x80 | (c & 0x3F)));
    }
}

static bool JsonRead4Hex(JsonReader *r, uint32_t *out)
{
    if (r->end - r->p < 4) return JsonFail(r, "short \\u escape");
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int d = HexDigit(r->p[i]);
        if (d < 0) return JsonFail(r, "bad \\u escape");
        v = v * 16 + (uint32_t)d;
    }
    r->p += 4;
    *out = v;
    return true;
}

// A string literal (r->p at its opening quote), decoded into UTF-8 like Python's json module does.
static bool JsonReadString(JsonReader *r, const char **text, size_t *len)
{
    Nob_String_Builder sb = {0};
    r->p++;
    for (;;) {
        if (r->p >= r->end) {
            nob_sb_free(sb);
            return JsonFail(r, "unterminated string");
        }
        char c = *r->p++;
        if (c == '"') break;
        if (c != '\\') {
            nob_da_append(&sb, c);
            continue;
        }
        if (r->p >= r->end) break;
        char e = *r->p++;
        switch (e) {
        case '"': case '\\': case '/': nob_da_append(&sb, e); break;
        case 'b': nob_da_append(&sb, '\b'); break;
        case 'f': nob_da_append(&sb, '\f'); break;
        case 'n': nob_da_append(&sb, '\n'); break;
        case 'r': nob_da_append(&sb, '\r'); break;
        case 't': nob_da_append(&sb, '\t'); break;
        case 'u': {
            uint32_t c1;
            if (!JsonRead4Hex(r, &c1)) {
                nob_sb_free(sb);
                return false;
            }
            // a surrogate pair 😀 is one character, as Python decodes it
            if (c1 >= 0xD800 && c1 < 0xDC00 && r->end - r->p >= 6 && r->p[0] == '\\' && r->p[1] == 'u') {
                const char *save = r->p;
                r->p += 2;
                uint32_t c2;
                if (JsonRead4Hex(r, &c2) && c2 >= 0xDC00 && c2 < 0xE000) {
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                } else {
                    r->p = save;
                }
            }
            PutUtf8(&sb, c1);
        } break;
        default:
            nob_sb_free(sb);
            return JsonFail(r, "bad escape");
        }
    }
    *len = sb.count;
    nob_sb_append_null(&sb);
    *text = sb.items;
    return true;
}

static bool JsonReadValue(JsonReader *r, Json *v, int depth)
{
    memset(v, 0, sizeof *v);
    if (depth > 64) return JsonFail(r, "nested too deeply");
    JsonSkipSpace(r);
    if (r->p >= r->end) return JsonFail(r, "unexpected end");
    char c = *r->p;
    if (c == '"') {
        v->kind = JSON_STRING;
        return JsonReadString(r, &v->text, &v->len);
    }
    if (c == '[' || c == '{') {
        bool object = c == '{';
        v->kind = object ? JSON_OBJECT : JSON_ARRAY;
        r->p++;
        JsonSkipSpace(r);
        if (r->p < r->end && *r->p == (object ? '}' : ']')) {
            r->p++;
            return true;
        }
        for (;;) {
            Json item;
            const char *key = NULL;
            size_t keyLen = 0;
            if (object) {
                JsonSkipSpace(r);
                if (r->p >= r->end || *r->p != '"') return JsonFail(r, "expected a key");
                if (!JsonReadString(r, &key, &keyLen)) return false;
                JsonSkipSpace(r);
                if (r->p >= r->end || *r->p != ':') return JsonFail(r, "expected ':'");
                r->p++;
            }
            if (!JsonReadValue(r, &item, depth + 1)) return false;
            item.key = key;
            item.keyLen = keyLen;
            nob_da_append(&v->items, item);
            JsonSkipSpace(r);
            if (r->p >= r->end) return JsonFail(r, "unexpected end");
            if (*r->p == ',') {
                r->p++;
                continue;
            }
            if (*r->p == (object ? '}' : ']')) {
                r->p++;
                return true;
            }
            return JsonFail(r, "expected ',' or a closing bracket");
        }
    }
    // a number, true, false or null: kept as written
    const char *start = r->p;
    while (r->p < r->end && (isalnum((unsigned char)*r->p) || *r->p == '-' || *r->p == '+' || *r->p == '.')) r->p++;
    if (r->p == start) return JsonFail(r, "unexpected character");
    v->kind = JSON_RAW;
    v->text = start;
    v->len = (size_t)(r->p - start);
    return true;
}

// A string as Python's json.dump writes it with ensure_ascii (its default): ", \ and \b \f \n \r \t escaped
// by name, other characters outside ' '..'~' as \u00XX (lowercase hex), characters above U+FFFF as a
// surrogate pair. Invalid UTF-8 can't occur: Python's json.load would have refused the file, and so does
// this writer.
static bool JsonWriteString(Nob_String_Builder *sb, const char *s, size_t len)
{
    nob_da_append(sb, '"');
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else return false;
        if (i + n > len) return false;
        for (size_t k = 1; k < n; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        }
        i += n;
        switch (cp) {
        case '"':  nob_sb_append_cstr(sb, "\\\""); continue;
        case '\\': nob_sb_append_cstr(sb, "\\\\"); continue;
        case '\b': nob_sb_append_cstr(sb, "\\b"); continue;
        case '\f': nob_sb_append_cstr(sb, "\\f"); continue;
        case '\n': nob_sb_append_cstr(sb, "\\n"); continue;
        case '\r': nob_sb_append_cstr(sb, "\\r"); continue;
        case '\t': nob_sb_append_cstr(sb, "\\t"); continue;
        default: break;
        }
        if (cp >= ' ' && cp <= '~') {
            nob_da_append(sb, (char)cp);
        } else if (cp < 0x10000) {
            nob_sb_appendf(sb, "\\u%04x", (unsigned)cp);
        } else {
            uint32_t v = cp - 0x10000;
            nob_sb_appendf(sb, "\\u%04x\\u%04x", (unsigned)(0xD800 + (v >> 10)), (unsigned)(0xDC00 + (v & 0x3FF)));
        }
    }
    nob_da_append(sb, '"');
    return true;
}

static void JsonIndent(Nob_String_Builder *sb, int depth)
{
    for (int i = 0; i < depth; i++) nob_da_append(sb, ' ');
}

// A value as Python's json.dump(..., indent=1) writes it: one element or member per line, indented by one
// space per level, ", " never at a line end, "key": value, and [] / {} for empty containers.
static bool JsonWrite(Nob_String_Builder *sb, const Json *v, int depth)
{
    switch (v->kind) {
    case JSON_STRING: return JsonWriteString(sb, v->text, v->len);
    case JSON_RAW: nob_sb_append_buf(sb, v->text, v->len); return true;
    case JSON_ARRAY:
    case JSON_OBJECT: {
        bool object = v->kind == JSON_OBJECT;
        if (v->items.count == 0) {
            nob_sb_append_cstr(sb, object ? "{}" : "[]");
            return true;
        }
        nob_sb_append_cstr(sb, object ? "{\n" : "[\n");
        for (size_t i = 0; i < v->items.count; i++) {
            const Json *item = &v->items.items[i];
            JsonIndent(sb, depth + 1);
            if (object) {
                if (!JsonWriteString(sb, item->key, item->keyLen)) return false;
                nob_sb_append_cstr(sb, ": ");
            }
            if (!JsonWrite(sb, item, depth + 1)) return false;
            if (i + 1 < v->items.count) nob_da_append(sb, ',');
            nob_da_append(sb, '\n');
        }
        JsonIndent(sb, depth);
        nob_da_append(sb, object ? '}' : ']');
        return true;
    }
    }
    return false;
}

static Json JsonStr(const char *s)
{
    return (Json){.kind = JSON_STRING, .text = s, .len = strlen(s)};
}

static Json JsonMember(const char *key, Json v)
{
    v.key = key;
    v.keyLen = strlen(key);
    return v;
}

// Python's os.path.normpath of os.path.join(dir, file): file as is when absolute, else dir/file; then "."
// dropped, ".." resolved against the previous component (kept at the start of a relative path), repeated
// slashes collapsed (exactly two leading slashes kept, as POSIX allows).
static const char *PyNormJoin(const char *dir, const char *file)
{
    const char *path = file[0] == '/' ? file : nob_temp_sprintf("%s/%s", dir, file);
    if (path[0] == '\0') return ".";
    int lead = path[0] == '/' ? (path[1] == '/' && path[2] != '/' ? 2 : 1) : 0;
    Nob_File_Paths parts = {0};
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - start);
        if (n == 0 || (n == 1 && start[0] == '.')) continue;
        const char *part = nob_temp_strndup(start, n);
        if (strcmp(part, "..") == 0 &&
            ((lead == 0 && parts.count > 0 && strcmp(parts.items[parts.count - 1], "..") != 0) ||
             (lead > 0))) {
            if (parts.count > 0) parts.count--;
            continue;
        }
        nob_da_append(&parts, part);
    }
    Nob_String_Builder sb = {0};
    for (int i = 0; i < lead; i++) nob_da_append(&sb, '/');
    for (size_t i = 0; i < parts.count; i++) {
        if (i > 0) nob_da_append(&sb, '/');
        nob_sb_append_cstr(&sb, parts.items[i]);
    }
    if (sb.count == 0) nob_da_append(&sb, '.');
    nob_sb_append_null(&sb);
    const char *result = nob_temp_strdup(sb.items);
    nob_sb_free(sb);
    nob_da_free(parts);
    return result;
}

// `realpath -m path`: the canonical absolute path (symlinks resolved), also when the path doesn't exist
// (then the lexical form of cwd/path; realpath(3) needs it to exist).
static const char *RealpathM(const char *path)
{
    char buf[PATH_MAX];
    if (realpath(path, buf)) return nob_temp_strdup(buf);
    return NormalizeAbsolute(path[0] == '/' ? path : nob_temp_sprintf("%s/%s", LogicalCwd(), path));
}

typedef struct {
    Nob_Cmd *items;   // each: the compiler, its arguments, and the source file last
    size_t count, capacity;
} Entries;

// Writes compile_commands.json in the repo root, the compilation database clangd (and other tools) read for
// go-to-definition, references and diagnostics. Each entry is a compiler, its arguments and (last) the source
// file: exactly the arguments the file is compiled with, with no shell in between (the path defines' quotes
// stay as they are). R3D's own database (from CMAKE_EXPORT_COMPILE_COMMANDS) is merged in with assimp's
// entries dropped (C++, huge, not needed to follow calls into R3D and raylib). Machine-specific absolute
// paths: the file is gitignored. Written to a temporary file and renamed, so clangd never reads half a file.
// The bytes are what the old build.sh's embedded Python wrote (json.dump, indent=1), so switching between
// the two never shows a diff; python3 isn't needed any more.
static bool WriteCompileCommands(const char *root, const char *r3dDir, const char *r3dBuild, Entries *entries)
{
    const char *out = nob_temp_sprintf("%s/compile_commands.json", root);
    const char *r3dDb = nob_temp_sprintf("%s/compile_commands.json", r3dBuild);
    const char *assimp = nob_temp_sprintf("%s/", RealpathM(nob_temp_sprintf("%s/external/assimp", r3dDir)));

    Json db = {.kind = JSON_ARRAY};
    for (size_t i = 0; i < entries->count; i++) {
        Nob_Cmd *e = &entries->items[i];
        const char *file = e->items[e->count - 1];
        Json args = {.kind = JSON_ARRAY};
        for (size_t k = 0; k + 1 < e->count; k++) nob_da_append(&args.items, JsonStr(e->items[k]));
        nob_da_append(&args.items, JsonStr("-c"));
        nob_da_append(&args.items, JsonStr(file));
        Json entry = {.kind = JSON_OBJECT};
        nob_da_append(&entry.items, JsonMember("directory", JsonStr(root)));
        nob_da_append(&entry.items, JsonMember("file", JsonStr(file)));
        nob_da_append(&entry.items, JsonMember("arguments", args));
        nob_da_append(&db.items, entry);
    }

    if (IsRegularFile(r3dDb)) {
        Nob_String_Builder text = {0};
        if (!nob_read_entire_file(r3dDb, &text)) return false;
        JsonReader r = {.start = text.items, .p = text.items, .end = text.items + text.count, .path = r3dDb};
        Json theirs;
        if (!JsonReadValue(&r, &theirs, 0)) return false;
        if (theirs.kind != JSON_ARRAY) return JsonFail(&r, "not a JSON array");
        for (size_t i = 0; i < theirs.items.count; i++) {
            Json *e = &theirs.items.items[i];
            const char *dir = "", *file = "";
            for (size_t k = 0; e->kind == JSON_OBJECT && k < e->items.count; k++) {
                Json *m = &e->items.items[k];
                if (m->kind != JSON_STRING) continue;
                if (strcmp(m->key, "directory") == 0) dir = m->text;
                if (strcmp(m->key, "file") == 0) file = m->text;
            }
            if (strncmp(PyNormJoin(dir, file), assimp, strlen(assimp)) != 0) nob_da_append(&db.items, *e);
        }
        // `text` stays allocated: the kept entries' numbers point into it
    } else {
        SayErr("build.sh: %s not found: compile_commands.json has no R3D or raylib entries", r3dDb);
    }

    Nob_String_Builder sb = {0};
    if (!JsonWrite(&sb, &db, 0)) {
        SayErr("build: compile_commands.json: a string is not valid UTF-8");
        return false;
    }
    nob_da_append(&sb, '\n');
    const char *tmp = nob_temp_sprintf("%s.tmp", out);
    bool ok = nob_write_entire_file(tmp, sb.items, sb.count) && rename(tmp, out) == 0;
    if (!ok) SayErr("build: could not write %s: %s", out, strerror(errno));
    nob_sb_free(sb);
    return ok;
}

// #endregion

// #region Build steps

static const char *gRoot;    // the repo root (absolute, as `cd "$(dirname build.sh)" && pwd` names it)
static const char *gR3dDir;  // $R3D_DIR, default <root>/submodules/r3d, used as given (not normalised)

// The small programs that replaced the repo's shell scripts (docs/architecture.md, "Tools"). Each is one
// file, tools/<name>.c, written on nob.h with the shared tools/tool.h, and is built into build/tools/<name>,
// except run-sh, which goes to build/run-sh.o (the developer's name, like Simple-Todo's todo.o): every shell
// command a Claude session runs goes through it (.claude/settings.json's hook), and its short fixed path
// is what the hook names.
// The list is complete for the round that added it: a program is added here when its tools/<name>.c is
// written. The plain build doesn't build these (./build.sh is unchanged); after one ./build.sh --tools each
// program rebuilds itself when its source changed (ToolRebuildSelf in tools/tool.h).
static const char *TOOLS[] = {
    "run-sh",            // every shell command of a Claude session goes through it: guard rails (CLAUDE.md)
    "todo",              // the developer's task list, TODO.md
    "ai-todo",           // the AI's task list, .claude/TODO.md
    "stub-terminal",     // MONOLITH_TERMINAL stand-in for launcher tests: no window
    "request-queue",     // the request queue on disk, run by the coordinator session
    "stub-coordinator",  // MONOLITH_CLAUDE stand-in: the coordinator's scheduling rules without Claude
    "stub-classifier",   // MONOLITH_CLASSIFIER stand-in: the AI classifier's answers without Claude
    "test-coordinator",  // end-to-end test of the queue and the coordinator launch
    "leakcheck",         // every main code path under LeakSanitizer (CLAUDE.md: after every code change)
    "bench-export-ab",   // interleaved A/B benchmark pairs of two exports
    "old-monolith",      // builds monolith as it was at an older commit (the migrate-map skill)
};

// The unit tests in tests/unit/ (region "Unit tests" below runs them): one list for every test's sources and
// flags, here at the top because the compile_commands.json writer in BuildLinux builds its entries for the
// tests from the same list, so there is exactly one place for a test's compile flags. They replaced one
// shell script per test (then tests/unit/*_test.sh).
//
// Every test is compiled with UNIT_FLAGS (C11, every warning, ASan + UBSan with no recovery: a sanitizer
// report fails the test) and run with ASAN_OPTIONS=detect_leaks=1 (a leak fails it too). An entry adds:
//   name      what `./build.sh test <name>` takes; the binary is build/unit/<name>/<name>_test
//   sources   NULL-terminated; the first is the test's own file (the one compile_commands.json lists).
//             Relative to the repo root, or an absolute path ("{out}/xatlas.o": an object the prepare step made)
//   defines   NULL-terminated extra -D flags ("-DMONOLITH_EDITOR": the runtime as the editor has it)
//   includes  NULL-terminated -I folders, relative to the repo root ("runtime", "src")
//   r3d       links R3D, raylib and assimp from $R3D_DIR/build-linux (needs ./build.sh first) and adds
//             -DMS_PATCHED_R3D and their include folders, as the game does
//   game      needs build/linux/monolith (the prepare step runs it); both r3d and game print "run ./build.sh
//             first" when what they need is missing
//   env       NULL-terminated "NAME=value" set for the run (after ASAN_OPTIONS; "{root}" is replaced)
//   each      optional, NULL-terminated: the test runs once per item (printing "== <item>" before each),
//             "{each}" in args is replaced by the item; the first failing run ends the test
//   args      NULL-terminated arguments of the run ("{root}", "{out}" and "{each}" are replaced)
//   prepare   optional: runs before the compile (build helpers, copy data) and may append run arguments
//             (they come first, before args); returns false when it failed (it prints why)
// The flags are written in the order the old scripts gave them to the compiler: UNIT_FLAGS, then
// -DMS_PATCHED_R3D (r3d), defines, R3D's and raylib's -I (r3d), includes, sources, -o, libraries (r3d).
// To add a test: write tests/unit/<name>_test.c and add an entry below.
typedef struct UnitTest UnitTest;
struct UnitTest {
    const char *name;
    const char **sources;
    const char **defines;
    const char **includes;
    bool r3d;
    bool game;
    const char **env;
    const char **each;
    const char **args;
    bool (*prepare)(const UnitTest *t, const char *out, Nob_Cmd *run);
};

// The flags of every test (the tests' scripts all used this list): -g -O1 with frame pointers for readable
// sanitizer stacks, -fno-sanitize-recover=all so UBSan's first report ends the test with an error.
static const char *UNIT_FLAGS[] = {
    "-std=c11", "-Wall", "-Wextra", "-Wno-missing-field-initializers", "-g", "-O1", "-fno-omit-frame-pointer",
    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-D_GNU_SOURCE",
};

// The prepare steps (defined in region "Unit tests").
static bool PrepareRunSh(const UnitTest *t, const char *out, Nob_Cmd *run);
static bool PrepareCompiled(const UnitTest *t, const char *out, Nob_Cmd *run);
static bool PreparePaintunwrap(const UnitTest *t, const char *out, Nob_Cmd *run);

// The tests. All but run_sh are the old scripts' (collision, compiled, export_strip, msg, paintunwrap,
// parse_error), each with its script's flags, sources and run.
static const UnitTest UNIT_TESTS[] = {
    {
        // corruption test of the compiled-map reader (tests/unit/compiled_test.c): three packages, copied
        // into build/unit/compiled/ and compiled there with monolith --compile (PrepareCompiled), each run
        // through the test; the runtime as a game has it (no MONOLITH_EDITOR)
        .name = "compiled",
        .sources = (const char *[]){"tests/unit/compiled_test.c", "runtime/monolith_scene.c", NULL},
        .includes = (const char *[]){"runtime", NULL},
        .r3d = true,
        .game = true,
        .each = (const char *[]){"models", "rotated", "longname", NULL},
        .args = (const char *[]){"{out}/{each}", NULL},
        .prepare = PrepareCompiled,
    },
    {
        // error messages of the map-text parser (tests/unit/parse_error_test.c), against the runtime as the
        // editor has it (MONOLITH_EDITOR: the parser); the folder it may write in is build/unit/parse_error/
        .name = "parse_error",
        .sources = (const char *[]){"tests/unit/parse_error_test.c", "runtime/monolith_scene.c", NULL},
        .defines = (const char *[]){"-DMONOLITH_EDITOR", NULL},
        .includes = (const char *[]){"runtime", NULL},
        .r3d = true,
        .args = (const char *[]){"{out}", NULL},
    },
    {
        // the walking physics against turned boxes (tests/unit/collision_test.c): src/player.c's PlayerStep
        // on a noGpu load of tests/rotated, with the editor's runtime (it parses the map text)
        .name = "collision",
        .sources = (const char *[]){"tests/unit/collision_test.c", "src/player.c", "runtime/monolith_scene.c", NULL},
        .defines = (const char *[]){"-DMONOLITH_EDITOR", NULL},
        .includes = (const char *[]){"runtime", "src", NULL},
        .r3d = true,
        .args = (const char *[]){"{root}/tests/rotated", NULL},
    },
    {
        // the paint unwrap (tests/unit/paintunwrap_test.c: src/paintunwrap.c + the vendored xatlas, built with
        // the sanitizers by PreparePaintunwrap) on the synthetic meshes and every model of maps/*/models/;
        // extra arguments (other model files) go to the test. The GPU driver's own allocations at context
        // creation are suppressed by tools/lsan.supp (any-frame rule).
        .name = "paintunwrap",
        .sources = (const char *[]){"tests/unit/paintunwrap_test.c", "src/paintunwrap.c", "runtime/monolith_scene.c",
                                    "{out}/xatlas.o", NULL},
        .defines = (const char *[]){"-DMONOLITH_EDITOR", NULL},
        .includes = (const char *[]){"runtime", "src", NULL},
        .r3d = true,
        .env = (const char *[]){"LSAN_OPTIONS=suppressions={root}/tools/lsan.supp", NULL},
        .prepare = PreparePaintunwrap,
    },
    {
        // the editor's message text (src/msg.c, tests/unit/msg_test.c): static vs heap texts, replace,
        // append, long strings, out of memory. Plain C + libc: needs no ./build.sh first.
        .name = "msg",
        .sources = (const char *[]){"tests/unit/msg_test.c", NULL},
        .includes = (const char *[]){"src", NULL},
    },
    {
        // the runtime an export ships has no map-text parser (tests/unit/export_strip_test.c: a grep for
        // the parser's names in an export's monolith_scene.c / .h). Without an extra argument the test
        // exports tests/models with build/linux/monolith into build/unit/export_strip/ itself (and checks
        // that the game is built); with one (`./build.sh test export_strip -- <export dir>`) it checks
        // that export folder. Plain C + libc.
        .name = "export_strip",
        .sources = (const char *[]){"tests/unit/export_strip_test.c", NULL},
        .args = (const char *[]){"{root}", "{out}", NULL},
    },
    {
        // tools/run-sh.c: every rule of its table through --check, -c's exit codes / stdin / signals, the
        // benchmark lock, the hook's JSON answers
        .name = "run_sh",
        .sources = (const char *[]){"tests/unit/run_sh_test.c", NULL},
        .args = (const char *[]){"{out}", "{root}", NULL},
        .prepare = PrepareRunSh,
    },
};

// BuildR3D: configures R3D in <dir> once, then runs an incremental build; the output goes to
// <dir>/monolith-build.log. The configure args are remembered in <dir>/monolith-configure.args (one per
// line); when they differ (or a dir from before that file existed is found), cmake is re-run on the existing
// dir, which rebuilds what the new options affect (all of it for a new build type).
static void BuildR3D(const char *dir, const char *what, Nob_Cmd *configureArgs)
{
    const char *log = nob_temp_sprintf("%s/monolith-build.log", dir);
    const char *argsFile = nob_temp_sprintf("%s/monolith-configure.args", dir);
    Nob_String_Builder args = {0};
    for (size_t i = 0; i < configureArgs->count; i++) {
        if (i > 0) nob_da_append(&args, '\n');
        nob_sb_append_cstr(&args, configureArgs->items[i]);
    }
    while (args.count > 0 && args.items[args.count - 1] == '\n') args.count--;  // as $(printf ...) strips them
    nob_sb_append_null(&args);

    // mkdir -p <dir>: the R3D build dir (cmake would create it too, but the args file and log go there first)
    if (!MkdirP(dir)) Fail(1);
    const char *cache = nob_temp_sprintf("%s/CMakeCache.txt", dir);
    const char *previous = IsRegularFile(argsFile) ? ReadTrimmed(argsFile) : NULL;
    if (!IsRegularFile(cache) || !previous || strcmp(previous, args.items) != 0) {
        if (IsRegularFile(cache)) {
            Say("R3D configure options changed: re-configuring %s (a new build type rebuilds all of R3D, raylib "
                "and assimp: minutes) ...", dir);
        } else {
            Say("configuring R3D for %s in %s ...", what, dir);
        }
        // rm -f <args file>: forget the old options first, so a configure that fails half-way is re-run next
        // time instead of being taken for done
        if (remove(argsFile) != 0 && errno != ENOENT) {
            SayErr("rm: cannot remove '%s': %s", argsFile, strerror(errno));
            Fail(1);
        }
        // cmake configure: generate R3D's build system (Makefiles) for the source tree $R3D_DIR (-S) in <dir>
        // (-B), with the options the caller passes (see them at the BuildR3D calls). Output into the log,
        // which is printed in full when the configure fails.
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, "cmake", "-S", gR3dDir, "-B", dir);
        nob_da_append_many(&cmd, configureArgs->items, configureArgs->count);
        if (!RunTimed(&cmd, nob_temp_sprintf("cmake configure R3D (%s)", what), (Output){.log = log})) {
            PrintLog(log, 0);
            Fail(1);
        }
        nob_cmd_free(cmd);
        // the args file: written only after a successful configure, so its presence means "configured so"
        if (!WriteLine(argsFile, args.items)) Fail(1);
    }
    Say("building R3D for %s (log: %s) ...", what, log);
    // cmake build: the incremental build of R3D, raylib, assimp and zlib (~1 s when nothing changed), with as
    // many parallel jobs (-j) as nproc reports. Appended to the log; its last 50 lines are printed on failure.
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "cmake", "--build", dir, nob_temp_sprintf("-j%lu", Nproc()));
    if (!RunTimed(&cmd, nob_temp_sprintf("cmake build R3D (%s)", what), (Output){.log = log, .append = true})) {
        PrintLog(log, 50);
        Fail(1);
    }
    nob_cmd_free(cmd);
    nob_sb_free(args);
}

// Xatlas: compiles the vendored xatlas (src/vendor/xatlas/, MIT; the editor's paint unwrap, src/paintunwrap.c,
// through its C API) into <obj> when the object is missing, older than the source or nob.c, or was built with
// other flags (the compiler and flags are remembered in <obj>.flags). It is not our code: it gets -w (no
// warnings of its own), -DNDEBUG (no debug checks or debug files), XA_MULTITHREADED=0 (no threads: the same
// model always gives the same unwrap) and its C API (XATLAS_C_API=1). <extra> are the build's own
// optimisation and sanitizer flags, so the object matches the build it is linked into.
static void Xatlas(const char *cxx, const char *obj, Nob_Cmd *extra)
{
    const char *dir = nob_temp_sprintf("%s/src/vendor/xatlas", gRoot);
    Nob_Cmd flags = {0};
    // -std=c++14: the language level xatlas is written for
    nob_cmd_append(&flags, "-std=c++14", "-w", "-DNDEBUG", "-DXATLAS_C_API=1", "-DXA_MULTITHREADED=0");
    nob_da_append_many(&flags, extra->items, extra->count);
    const char *stamp = nob_temp_sprintf("%s.flags", obj);
    Nob_String_Builder stampText = {0};
    nob_sb_append_cstr(&stampText, cxx);
    for (size_t i = 0; i < flags.count; i++) {
        nob_da_append(&stampText, ' ');
        nob_sb_append_cstr(&stampText, flags.items[i]);
    }
    nob_sb_append_null(&stampText);

    // mkdir -p <build folder>-obj
    if (!MkdirP(nob_temp_dir_name(obj))) Fail(1);
    const char *previous = IsRegularFile(stamp) ? ReadTrimmed(stamp) : NULL;
    if (!IsRegularFile(obj) ||
        NewerThan(nob_temp_sprintf("%s/xatlas.cpp", dir), obj) ||
        NewerThan(nob_temp_sprintf("%s/xatlas.h", dir), obj) ||
        NewerThan(nob_temp_sprintf("%s/xatlas_c.h", dir), obj) ||
        NewerThan(nob_temp_sprintf("%s/nob.c", gRoot), obj) ||
        !previous || strcmp(previous, stampText.items) != 0) {
        Say("compiling xatlas (%s) ...", cxx);
        // the xatlas compile: C++ to an object file only (-c), linked into the game below
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, cxx);
        nob_da_append_many(&cmd, flags.items, flags.count);
        nob_cmd_append(&cmd, "-c", nob_temp_sprintf("%s/xatlas.cpp", dir), "-o", obj);
        if (!RunTimed(&cmd, nob_temp_sprintf("compile xatlas (%s)", cxx), (Output){0})) Fail(1);
        nob_cmd_free(cmd);
        // the stamp: which compiler and flags the object was built with
        if (!WriteLine(stamp, stampText.items)) Fail(1);
    }
    nob_sb_free(stampText);
    nob_cmd_free(flags);
}

// The packaging every target shares: map packages next to the binary, and the font licences.
static void CopyData(const char *out)
{
    // rm -rf <out>/resources <out>/maps <out>/runtime: map packages are found next to the binary (see
    // InitDataPaths in src/main.c), so the old copy goes first (a package deleted in the repo must not
    // linger in the build); resources/ is from builds before map packages (the shaders now live in the
    // packages), runtime/ from builds before --export needed the repo (src/export.c). Only these three
    // paths, all inside the build's own output folder.
    const char *stale[] = {"resources", "maps", "runtime"};
    for (size_t i = 0; i < NOB_ARRAY_LEN(stale); i++) {
        if (!RemoveTree(nob_temp_sprintf("%s/%s", out, stale[i]))) Fail(1);
    }
    // cp -r maps <out>/maps: every map package, so the folder is self-contained
    if (!CopyTree(nob_temp_sprintf("%s/maps", gRoot), nob_temp_sprintf("%s/maps", out))) Fail(1);
    // The editor's fonts are compiled in (src/font_outfit.h, src/font_pixelify.h); the OFL wants their
    // licences next to them
    if (!CopyFile(nob_temp_sprintf("%s/resources/fonts/outfit/OFL.txt", gRoot),
                  nob_temp_sprintf("%s/OFL-Outfit.txt", out)) ||
        !CopyFile(nob_temp_sprintf("%s/resources/fonts/pixelify/OFL.txt", gRoot),
                  nob_temp_sprintf("%s/OFL-PixelifySans.txt", out))) {
        Fail(1);
    }
}

// The binary's path for the "built: ..." line: relative to the repo root (${OUT#"$ROOT/"}).
static const char *RelativeToRoot(const char *path)
{
    size_t n = strlen(gRoot);
    if (strncmp(path, gRoot, n) == 0 && path[n] == '/') return path + n + 1;
    return path;
}

// Hands this process over to the program (`exec`): the summary first, since nothing runs after this.
static void Exec(const char *program, Nob_Cmd *args, bool searchPath)
{
    PrintSummary();
    nob_cmd_append(args, (const char *)NULL);
    fflush(NULL);
    if (searchPath) execvp(program, (char *const *)args->items);
    else execv(program, (char *const *)args->items);
    SayErr("build.sh: exec %s: %s", program, strerror(errno));
    exit(errno == ENOENT ? 127 : 126);
}

// ./build.sh --win [run [args]]
static int BuildWindows(int argc, char **argv)
{
    const char *next = argc > 0 ? argv[0] : "";
    if (strcmp(next, "debug") == 0 || strcmp(next, "lsan") == 0) {
        SayErr("build.sh: --win %s is not supported (the sanitizers don't work with MinGW).", next);
        SayErr("          Use ./build.sh %s for a sanitizer build on Linux.", next);
        return 1;
    }

    const char *winCc = "x86_64-w64-mingw32-gcc";
    const char *r3dWin = nob_temp_sprintf("%s/build-mingw", gR3dDir);
    const char *out = nob_temp_sprintf("%s/build/win", gRoot);

    // R3D, raylib and assimp as static libraries (BUILD_SHARED_LIBS=OFF), so the .exe needs no DLLs besides
    // the ones every Windows system has. The toolchain file selects the MinGW-w64 cross compilers. Release
    // always (R3D_BUILD_TYPE is Linux only). R3D_RAYLIB_VENDORED / R3D_ASSIMP_VENDORED: build the copies inside
    // submodules/r3d, never system packages; ASSIMP_BUILD_ZLIB: assimp's bundled zlib, linked in too;
    // R3D_BUILD_EXAMPLES=OFF: R3D's own examples aren't needed; R3D_GPU_PROFILER: per-pass GPU timer queries,
    // off at runtime unless --bench or the F2 panel turns them on (submodules/r3d/patches.md).
    Nob_Cmd cfg = {0};
    nob_cmd_append(&cfg, nob_temp_sprintf("-DCMAKE_TOOLCHAIN_FILE=%s/cmake/mingw-w64-x86_64.cmake", gRoot),
                   "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=OFF",
                   "-DR3D_RAYLIB_VENDORED=ON", "-DR3D_ASSIMP_VENDORED=ON", "-DASSIMP_BUILD_ZLIB=ON",
                   "-DR3D_BUILD_EXAMPLES=OFF", "-DR3D_GPU_PROFILER=ON");
    BuildR3D(r3dWin, "Windows", &cfg);

    // mkdir -p build/win
    if (!MkdirP(out)) Fail(1);
    const char *xatlasObj = nob_temp_sprintf("%s/build/win-obj/xatlas.o", gRoot);
    Nob_Cmd xatlasFlags = {0};
    nob_cmd_append(&xatlasFlags, "-O2");
    Xatlas("x86_64-w64-mingw32-g++", xatlasObj, &xatlasFlags);

    // The game: compile and link every C file in one command. -std=c11 -Wall -Wextra with zero warnings
    // (-Wno-missing-field-initializers: designated initialisers that leave fields zero are intended), -O2.
    // Include paths: R3D's headers, its generated config header (build dir), raylib's, assimp's (source and
    // generated config; --inspect-model reads models through assimp, src/inspect.c), assimp's zlib (the PNG
    // writer, src/png.c). R3D_BUILD_TYPE_NAME: shown in bench reports. -I runtime: the scene runtime's header;
    // MS_PATCHED_R3D: the runtime may use monolith's R3D patches; MONOLITH_EDITOR: the editor build of the
    // runtime. Then the sources (src/*.c but launcher.c and classifier.c, runtime/*.c in glob order) and the xatlas
    // object.
    // -s: strip the symbols (a smaller .exe). -static: libgcc, libstdc++ (assimp) and winpthread are linked
    // in as well, leaving only system DLLs (opengl32, gdi32, winmm, kernel32, ...) as imports. -lstdc++:
    // assimp is C++; opengl32/gdi32/winmm/shell32: what raylib's GLFW and the editor call.
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, winCc, "-std=c11", "-Wall", "-Wextra", "-Wno-missing-field-initializers", "-O2",
                   nob_temp_sprintf("-I%s/include", gR3dDir),
                   nob_temp_sprintf("-I%s/generated/include", r3dWin),
                   nob_temp_sprintf("-I%s/external/raylib/raylib/include", r3dWin),
                   nob_temp_sprintf("-I%s/external/assimp/include", gR3dDir),
                   nob_temp_sprintf("-I%s/external/assimp/include", r3dWin),
                   nob_temp_sprintf("-I%s/external/assimp/contrib/zlib", gR3dDir),
                   nob_temp_sprintf("-I%s/external/assimp/contrib/zlib", r3dWin),
                   "-DR3D_BUILD_TYPE_NAME=\"Release\"",
                   nob_temp_sprintf("-I%s/runtime", gRoot), "-DMS_PATCHED_R3D", "-DMONOLITH_EDITOR");
    // The Windows build is the view-only editor (MONOLITH_EDITING 0, src/common.h): the launcher and its AI
    // classifier (POSIX, Linux only) are left out; launcher.h gives the stubs it links against.
    size_t first = cmd.count, kept = first;
    AppendGlob(&cmd, nob_temp_sprintf("%s/src/*.c", gRoot));
    for (size_t k = first; k < cmd.count; k++) {
        const char *base = strrchr(cmd.items[k], '/');
        base = base ? base + 1 : cmd.items[k];
        if (strcmp(base, "launcher.c") != 0 && strcmp(base, "classifier.c") != 0) cmd.items[kept++] = cmd.items[k];
    }
    cmd.count = kept;
    AppendGlob(&cmd, nob_temp_sprintf("%s/runtime/*.c", gRoot));
    nob_cmd_append(&cmd, xatlasObj, "-o", nob_temp_sprintf("%s/monolith.exe", out),
                   "-s", "-static", nob_temp_sprintf("-L%s/lib", r3dWin), "-lr3d", "-lraylib", "-lassimp",
                   "-lzlibstatic", "-lstdc++", "-lopengl32", "-lgdi32", "-lwinmm", "-lshell32", "-lm");
    if (!RunTimed(&cmd, "compile+link monolith.exe (Windows)", (Output){0})) Fail(1);

    CopyData(out);
    Say("built: build/win/monolith.exe");

    if (strcmp(next, "run") == 0) {
        // wine: run the .exe with the remaining arguments, replacing this process (the shell script's
        // `exec wine ...`): not a build step, so not timed.
        Nob_Cmd run = {0};
        nob_cmd_append(&run, "wine", nob_temp_sprintf("%s/monolith.exe", out));
        nob_da_append_many(&run, (const char **)argv + 1, (size_t)(argc - 1));
        Exec("wine", &run, true);
    }
    PrintSummary();
    return 0;
}

// ./build.sh [run [args] | debug | lsan | profile]
static int BuildLinux(int argc, char **argv)
{
    const char *mode = argc > 0 ? argv[0] : "";
    bool profile = strcmp(mode, "profile") == 0;
    bool debug = strcmp(mode, "debug") == 0;
    bool lsan = strcmp(mode, "lsan") == 0;
    const char *cc = Env("CC", "clang");

    // R3D for Linux: static libs (BUILD_SHARED_LIBS=OFF), so the executable needs nothing beyond what every
    // desktop Linux has (glibc, libm, libGL from the GPU driver). Release since 2026-10-10; before that there
    // was no CMAKE_BUILD_TYPE (R3D unoptimised, like the original ../r3d/build), so frame times in older
    // benchmark reports aren't comparable (R3D_BUILD_TYPE=None rebuilds that: an empty CMAKE_BUILD_TYPE).
    // R3D_RAYLIB_VENDORED / R3D_ASSIMP_VENDORED: the copies inside submodules/r3d, never system packages.
    // ASSIMP_BUILD_ZLIB: bundled zlib, linked in too. ASSIMP_BUILD_MINIZIP: use assimp's bundled unzip; a system
    // minizip (Arch: /usr/include/minizip/) is found by pkg-config but its headers aren't where assimp's
    // #include <unzip.h> looks, so the build fails. R3D_BUILD_EXAMPLES=OFF: R3D's own examples aren't needed.
    // R3D_GPU_PROFILER: per-pass GPU timer queries, off at runtime unless --bench or the F2 panel turns them on
    // (see submodules/r3d/patches.md). CMAKE_EXPORT_COMPILE_COMMANDS: R3D's and raylib's entries for our
    // compile_commands.json (no change to how anything compiles).
    // The profile build has its own R3D dir: RelWithDebInfo (-O2 -g) with frame pointers
    // (-fno-omit-frame-pointer, and -mno-omit-leaf-frame-pointer for leaf functions too), so
    // perf --call-graph fp unwinds through R3D, raylib and assimp (tools/profile.c).
    const char *r3dLinux = nob_temp_sprintf("%s/build-linux", gR3dDir);
    const char *buildType = Env("R3D_BUILD_TYPE", "Release");
    Nob_Cmd extra = {0};
    if (profile) {
        r3dLinux = nob_temp_sprintf("%s/build-linux-profile", gR3dDir);
        buildType = "RelWithDebInfo";
        nob_cmd_append(&extra, "-DCMAKE_C_FLAGS=-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer",
                       "-DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer");
    }
    const char *cmakeType = strcmp(buildType, "None") == 0 ? "" : buildType;
    Nob_Cmd cfg = {0};
    nob_cmd_append(&cfg, nob_temp_sprintf("-DCMAKE_BUILD_TYPE=%s", cmakeType));
    nob_da_append_many(&cfg, extra.items, extra.count);
    nob_cmd_append(&cfg, "-DBUILD_SHARED_LIBS=OFF", "-DR3D_RAYLIB_VENDORED=ON", "-DR3D_ASSIMP_VENDORED=ON",
                   "-DASSIMP_BUILD_ZLIB=ON", "-DASSIMP_BUILD_MINIZIP=ON", "-DR3D_BUILD_EXAMPLES=OFF",
                   "-DR3D_GPU_PROFILER=ON", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON");
    BuildR3D(r3dLinux, nob_temp_sprintf("Linux (%s)", buildType), &cfg);

    const char *out = nob_temp_sprintf("%s/build/linux", gRoot);
    Nob_Cmd includes = {0};
    nob_cmd_append(&includes,
                   nob_temp_sprintf("-I%s/include", gR3dDir),                      // R3D's API
                   nob_temp_sprintf("-I%s/generated/include", r3dLinux),           // R3D's generated config header
                   nob_temp_sprintf("-I%s/external/raylib/raylib/include", r3dLinux),  // raylib's headers
                   // assimp's headers (source and generated config): --inspect-model (src/inspect.c) reads
                   // model files with assimp directly
                   nob_temp_sprintf("-I%s/external/assimp/include", gR3dDir),
                   nob_temp_sprintf("-I%s/external/assimp/include", r3dLinux),
                   // assimp's zlib: the PNG writer (src/png.c)
                   nob_temp_sprintf("-I%s/external/assimp/contrib/zlib", gR3dDir),
                   nob_temp_sprintf("-I%s/external/assimp/contrib/zlib", r3dLinux));

    // The game's C flags: C11, every warning (zero warnings is a rule; -Wno-missing-field-initializers:
    // designated initialisers that leave fields zero are intended), -O2.
    // The sanitizer builds go into their own folders, so they never replace the normal binary. R3D, raylib
    // and assimp stay the normal Release static libraries (not instrumented, but their mallocs are
    // intercepted, so leaks inside them are found too).
    Nob_Cmd cflags = {0};
    nob_cmd_append(&cflags, "-std=c11", "-Wall", "-Wextra", "-Wno-missing-field-initializers");
    if (debug) {
        // debug: debug info, no optimisation, frame pointers (complete sanitizer stacks), AddressSanitizer +
        // UndefinedBehaviorSanitizer (and LeakSanitizer at exit, part of ASan)
        nob_cmd_append(&cflags, "-g", "-O0", "-fno-omit-frame-pointer", "-fsanitize=address,undefined");
        out = nob_temp_sprintf("%s/build/linux-debug", gRoot);
    } else if (lsan) {
        // lsan: LeakSanitizer only, -O1 so the leak check's many runs stay fast; -g and frame pointers for
        // the leak reports' stacks
        nob_cmd_append(&cflags, "-g", "-O1", "-fno-omit-frame-pointer", "-fsanitize=leak");
        out = nob_temp_sprintf("%s/build/linux-lsan", gRoot);
    } else if (profile) {
        // profile: optimised like the release (-O2) with debug info and frame pointers (also in leaf
        // functions), so perf's frame-pointer unwinding gives whole call stacks
        nob_cmd_append(&cflags, "-O2", "-g", "-fno-omit-frame-pointer", "-mno-omit-leaf-frame-pointer");
        out = nob_temp_sprintf("%s/build/linux-profile", gRoot);
    } else {
        nob_cmd_append(&cflags, "-O2");
    }

    // mkdir -p <out>
    if (!MkdirP(out)) Fail(1);
    // xatlas (C++) with this build's optimisation and sanitizer flags, one object per build folder. The C++
    // compiler follows CC unless CXX is set: clang -> clang++, gcc -> g++ (the first occurrence is replaced,
    // so /usr/bin/clang-18 -> /usr/bin/clang++-18), anything else -> c++.
    const char *cxx;
    if (strstr(cc, "clang")) cxx = Env("CXX", ReplaceFirst(cc, "clang", "clang++"));
    else if (strstr(cc, "gcc")) cxx = Env("CXX", ReplaceFirst(cc, "gcc", "g++"));
    else cxx = Env("CXX", "c++");
    // xatlas gets the game's flags minus the language level and the warning flags (-std=*, -W*)
    Nob_Cmd xatlasFlags = {0};
    for (size_t i = 0; i < cflags.count; i++) {
        const char *f = cflags.items[i];
        if (strncmp(f, "-std=", 5) != 0 && strncmp(f, "-W", 2) != 0) nob_cmd_append(&xatlasFlags, f);
    }
    // (UBSan's -fsanitize=function would stop at xatlas's OpenNL part, which calls matrix destructors through
    // a pointer of the base struct type: UB by the letter, ABI-compatible; clang only)
    if (debug && strstr(cxx, "clang")) nob_cmd_append(&xatlasFlags, "-fno-sanitize=function");
    const char *xatlasObj = nob_temp_sprintf("%s-obj/xatlas.o", out);
    Xatlas(cxx, xatlasObj, &xatlasFlags);

    // Path defines baked into the binary (they are only used while those paths exist, see InitDataPaths in
    // src/main.c): where bench reports go, the maps, the repo (requests, the coordinator) and R3D's sources;
    // the R3D build type for bench reports. Their values keep their double quotes: C string literals.
    // -I runtime: the scene runtime's header; MS_PATCHED_R3D: the runtime may use monolith's R3D patches;
    // MONOLITH_EDITOR: the editor build of the runtime.
    Nob_Cmd defines = {0};
    nob_cmd_append(&defines,
                   nob_temp_sprintf("-DBENCHMARK_PATH=\"%s/benchmark/\"", gRoot),
                   nob_temp_sprintf("-DMAPS_PATH=\"%s/maps/\"", gRoot),
                   nob_temp_sprintf("-DPROJECT_PATH=\"%s/\"", gRoot),
                   nob_temp_sprintf("-DR3D_SOURCE_PATH=\"%s/\"", gR3dDir),
                   nob_temp_sprintf("-DR3D_BUILD_TYPE_NAME=\"%s\"", buildType),
                   nob_temp_sprintf("-I%s/runtime", gRoot), "-DMS_PATCHED_R3D", "-DMONOLITH_EDITOR");

    // The game: compile and link every C file (src/*.c, runtime/*.c in glob order) and the xatlas object in one
    // command. Link flags:
    // -L <R3D lib dir> -lr3d -lraylib -lassimp -lzlibstatic: the static libraries built above.
    // -l:libstdc++.a: libstdc++ (needed by assimp) linked statically as well: the host's libstdc++ is usually
    // newer than other distros' and would fail to load there. -lgcc_s stays shared (every distro has it; the
    // static unwinder would need glibc 2.35). -lGL (the GPU driver's), -lm, -lpthread, -ldl.
    // -Wl,--wrap=<symbol>,...: route symbols whose newest glibc version is newer than 2.34 to older versions,
    // see src/glibc_compat.c. The result runs on glibc >= 2.34. (examples/nob.c has the same list.)
    const char *glibcWrap[] = {"acosf", "asinf", "atan2f", "sqrtf", "fmod", "fmodf", "hypot", "hypotf",
                               "__isoc23_strtoul", "__isoc23_strtol", "arc4random"};
    Nob_String_Builder wrap = {0};
    nob_sb_append_cstr(&wrap, "-Wl");
    for (size_t i = 0; i < NOB_ARRAY_LEN(glibcWrap); i++) nob_sb_appendf(&wrap, ",--wrap=%s", glibcWrap[i]);
    nob_sb_append_null(&wrap);
    Nob_Cmd sources = {0};
    AppendGlob(&sources, nob_temp_sprintf("%s/src/*.c", gRoot));
    AppendGlob(&sources, nob_temp_sprintf("%s/runtime/*.c", gRoot));
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, cc);
    nob_da_append_many(&cmd, cflags.items, cflags.count);
    nob_da_append_many(&cmd, includes.items, includes.count);
    nob_da_append_many(&cmd, defines.items, defines.count);
    nob_da_append_many(&cmd, sources.items, sources.count);
    nob_cmd_append(&cmd, xatlasObj, "-o", nob_temp_sprintf("%s/monolith", out),
                   nob_temp_sprintf("-L%s/lib", r3dLinux), "-lr3d", "-lraylib", "-lassimp", "-lzlibstatic",
                   "-l:libstdc++.a", "-lgcc_s", "-lGL", "-lm", "-lpthread", "-ldl", wrap.items);
    if (!RunTimed(&cmd, nob_temp_sprintf("compile+link monolith (%s)", RelativeToRoot(out)), (Output){0})) Fail(1);

    CopyData(out);
    Say("built: %s/monolith", RelativeToRoot(out));

    // clangd's compilation database, from the lists above (the plain build only: the sanitizer and profile
    // builds and --win leave it alone). The game's files with the game's flags; tools/profile.c, xatlas, the
    // examples with the flags examples/nob.c uses (keep these lines in step when it changes), tests/unit
    // with the flags of UNIT_TESTS (generated from that list below).
    if (strcmp(mode, "") == 0 || strcmp(mode, "run") == 0) {
        Entries entries = {0};
        Nob_Cmd e = {0};
#define ENTRY_BEGIN() (e = (Nob_Cmd){0})
#define ENTRY_END() nob_da_append(&entries, e)
        for (size_t i = 0; i < sources.count; i++) {
            ENTRY_BEGIN();
            nob_cmd_append(&e, cc);
            nob_da_append_many(&e, cflags.items, cflags.count);
            nob_da_append_many(&e, includes.items, includes.count);
            nob_da_append_many(&e, defines.items, defines.count);
            nob_cmd_append(&e, sources.items[i]);
            ENTRY_END();
        }
        ENTRY_BEGIN();
        nob_cmd_append(&e, cxx, "-std=c++14", "-w", "-DNDEBUG", "-DXATLAS_C_API=1", "-DXA_MULTITHREADED=0");
        nob_da_append_many(&e, xatlasFlags.items, xatlasFlags.count);
        nob_cmd_append(&e, nob_temp_sprintf("%s/src/vendor/xatlas/xatlas.cpp", gRoot));
        ENTRY_END();
        ENTRY_BEGIN();
        nob_cmd_append(&e, cc, "-std=c11", "-Wall", "-Wextra", "-Wno-missing-field-initializers", "-O2", "-g",
                       nob_temp_sprintf("%s/tools/profile.c", gRoot));
        ENTRY_END();
        // the examples (examples/nob.c): the game's flags, R3D's and raylib's headers, the runtime
        Nob_Cmd ex = {0};
        nob_cmd_append(&ex, cc);
        nob_da_append_many(&ex, cflags.items, cflags.count);
        nob_cmd_append(&ex, nob_temp_sprintf("-I%s/include", gR3dDir),
                       nob_temp_sprintf("-I%s/external/raylib/raylib/include", r3dLinux),
                       nob_temp_sprintf("-I%s/runtime", gRoot), "-DMS_PATCHED_R3D");
        struct { const char *define, *file; } examples[] = {
            {NULL, "examples/01_minimal/main.c"},
            {"-DHARNESS_MAIN_NO_ARGS", "examples/02_existing_project/before.c"},
            {NULL, "examples/02_existing_project/after.c"},
            {"-DMONOLITH_EDITOR", "examples/03_live_editing/main.c"},
            {NULL, "examples/05_entities/main.c"},
        };
        for (size_t i = 0; i < NOB_ARRAY_LEN(examples); i++) {
            ENTRY_BEGIN();
            nob_da_append_many(&e, ex.items, ex.count);
            if (examples[i].define) nob_cmd_append(&e, examples[i].define);
            nob_cmd_append(&e, nob_temp_sprintf("%s/%s", gRoot, examples[i].file));
            ENTRY_END();
        }
        // tests/unit: one entry per test of UNIT_TESTS (the one list of the tests' flags), ASan + UBSan, stop
        // at the first error. clangd gets the editor-style view of every test: UNIT_FLAGS, R3D's and
        // raylib's headers (even for a test that links neither: harmless), the runtime, then the entry's
        // defines and its other -I folders, then the test's own file.
        for (size_t i = 0; i < NOB_ARRAY_LEN(UNIT_TESTS); i++) {
            const UnitTest *t = &UNIT_TESTS[i];
            ENTRY_BEGIN();
            nob_cmd_append(&e, cc);
            nob_da_append_many(&e, UNIT_FLAGS, NOB_ARRAY_LEN(UNIT_FLAGS));
            nob_cmd_append(&e, "-DMS_PATCHED_R3D", nob_temp_sprintf("-I%s/include", gR3dDir),
                           nob_temp_sprintf("-I%s/external/raylib/raylib/include", r3dLinux),
                           nob_temp_sprintf("-I%s/runtime", gRoot));
            for (const char **d = t->defines; d && *d; d++) nob_cmd_append(&e, *d);
            for (const char **inc = t->includes; inc && *inc; inc++) {
                if (strcmp(*inc, "runtime") != 0) nob_cmd_append(&e, nob_temp_sprintf("-I%s/%s", gRoot, *inc));
            }
            nob_cmd_append(&e, nob_temp_sprintf("%s/%s", gRoot, t->sources[0]));
            ENTRY_END();
        }
        // the tools (./build.sh --tools): their own flags (ToolCommand), one entry per tools/<name>.c that
        // exists
        for (size_t i = 0; i < NOB_ARRAY_LEN(TOOLS); i++) {
            const char *source = nob_temp_sprintf("%s/tools/%s.c", gRoot, TOOLS[i]);
            if (!IsRegularFile(source)) continue;
            ENTRY_BEGIN();
            nob_cmd_append(&e, Env("CC", "clang"), "-std=c11", "-Wall", "-Wextra",
                           "-Wno-missing-field-initializers", "-O1", source);
            ENTRY_END();
        }
#undef ENTRY_BEGIN
#undef ENTRY_END
        if (!WriteCompileCommands(gRoot, gR3dDir, r3dLinux, &entries)) Fail(1);
        Say("wrote: compile_commands.json");
    }

    // The profiling tool runs perf on the binary next to it. Not a game flag: the game stays as it is. Linux
    // only (perf is); plain C, links only libc. -O2 -g like the profile build.
    if (profile) {
        Nob_Cmd tool = {0};
        nob_cmd_append(&tool, cc, "-std=c11", "-Wall", "-Wextra", "-Wno-missing-field-initializers", "-O2", "-g",
                       nob_temp_sprintf("%s/tools/profile.c", gRoot), "-o", nob_temp_sprintf("%s/profile", out));
        if (!RunTimed(&tool, "compile profile tool (tools/profile.c)", (Output){0})) Fail(1);
        Say("built: %s/profile", RelativeToRoot(out));
    }

    if (strcmp(mode, "run") == 0) {
        // run: start the game with the remaining arguments, replacing this process (`exec`): not a build
        // step, so not timed
        Nob_Cmd run = {0};
        const char *game = nob_temp_sprintf("%s/monolith", out);
        nob_cmd_append(&run, game);
        nob_da_append_many(&run, (const char **)argv + 1, (size_t)(argc - 1));
        Exec(game, &run, false);
    }
    PrintSummary();
    return 0;
}

// #endregion

// #region Tools (./build.sh --tools [name...])

// (TOOLS, the list of programs, is at the top of region "Build steps": the compile_commands.json writer
// needs it too.)


// Where a tool's binary goes: build/run-sh.o for run-sh, else build/tools/<name>.
static const char *ToolBinary(const char *name)
{
    if (strcmp(name, "run-sh") == 0) return nob_temp_sprintf("%s/build/run-sh.o", gRoot);
    return nob_temp_sprintf("%s/build/tools/%s", gRoot, name);
}

// The compile of one tool: ${CC:-clang}, C11, every warning (zero warnings, as everywhere),
// -Wno-missing-field-initializers (designated initialisers that leave fields zero are intended, as in the
// game), -O1 (the programs do little work of their own). One source file, no libraries beyond libc.
// tools/tool.h's ToolCompileCommand has the same list for a program's rebuild of itself: keep the two in
// step, so both give the same binary.
static void ToolCommand(Nob_Cmd *cmd, const char *source, const char *binary)
{
    nob_cmd_append(cmd, Env("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Wno-missing-field-initializers",
                   "-O1", "-o", binary, source);
}

// ./build.sh --tools [name...]: builds every program in TOOLS (or only the ones named) that is missing or
// older than its source, tools/tool.h or nob.h; a second run compiles nothing. Each compile goes through
// RunTimed into a temporary file that is then renamed over the binary, so a program started at the same
// moment (the hook runs build/run-sh.o for every shell command) never sees a half-written file.
static int BuildTools(int argc, char **argv)
{
    // which programs: the named ones (each must be in TOOLS), or all of them
    Nob_File_Paths names = {0};
    for (int i = 0; i < argc; i++) {
        bool known = false;
        for (size_t k = 0; k < NOB_ARRAY_LEN(TOOLS); k++) known = known || strcmp(argv[i], TOOLS[k]) == 0;
        if (!known) {
            SayErr("build.sh: --tools: no tool named '%s'. The tools:", argv[i]);
            for (size_t k = 0; k < NOB_ARRAY_LEN(TOOLS); k++) SayErr("  %s", TOOLS[k]);
            return 1;
        }
        nob_da_append(&names, argv[i]);
    }
    if (names.count == 0) nob_da_append_many(&names, TOOLS, NOB_ARRAY_LEN(TOOLS));

    // mkdir -p build/tools: the binaries' folder (build/ is gitignored)
    if (!MkdirP(nob_temp_sprintf("%s/build/tools", gRoot))) return 1;
    const char *header = nob_temp_sprintf("%s/tools/tool.h", gRoot);
    const char *nobH = nob_temp_sprintf("%s/src/vendor/nob/nob.h", gRoot);
    size_t built = 0;
    for (size_t i = 0; i < names.count; i++) {
        const char *source = nob_temp_sprintf("%s/tools/%s.c", gRoot, names.items[i]);
        const char *binary = ToolBinary(names.items[i]);
        if (!IsRegularFile(source)) {
            SayErr("build.sh: --tools: %s is missing", RelativeToRoot(source));
            Fail(1);
        }
        // up to date: the binary exists and nothing it is built from is newer
        if (IsRegularFile(binary) && !NewerThan(source, binary) && !NewerThan(header, binary) &&
            !NewerThan(nobH, binary)) {
            continue;
        }
        // the compile, into <binary>.tmp.<pid>, then renamed into place
        const char *tmp = nob_temp_sprintf("%s.tmp.%ld", binary, (long)getpid());
        Nob_Cmd cmd = {0};
        ToolCommand(&cmd, source, tmp);
        if (!RunTimed(&cmd, nob_temp_sprintf("compile %s", RelativeToRoot(source)), (Output){0})) Fail(1);
        nob_cmd_free(cmd);
        if (rename(tmp, binary) != 0) {
            SayErr("build.sh: could not move %s into place: %s", RelativeToRoot(binary), strerror(errno));
            Fail(1);
        }
        Say("built: %s", RelativeToRoot(binary));
        built++;
    }
    if (built == 0) Say("tools: up to date");
    nob_da_free(names);
    PrintSummary();
    return 0;
}

// ./build.sh examples [--win | test | lsan]: the examples' build program, examples/nob.c (what
// examples/build.sh did, docs/architecture.md §4.3), compiled into examples/build/nob when that is missing or
// older than examples/nob.c or nob.h (with the flags build.sh compiles nob.c with), then run in place of
// this process with the remaining arguments, so its output and exit status are the caller's.
static int BuildExamples(int argc, char **argv)
{
    const char *source = nob_temp_sprintf("%s/examples/nob.c", gRoot);
    const char *binary = nob_temp_sprintf("%s/examples/build/nob", gRoot);
    const char *nobH = nob_temp_sprintf("%s/src/vendor/nob/nob.h", gRoot);
    if (!IsRegularFile(source)) {
        SayErr("build.sh: examples: %s is missing", RelativeToRoot(source));
        return 1;
    }
    if (!IsRegularFile(binary) || NewerThan(source, binary) || NewerThan(nobH, binary)) {
        // mkdir -p examples/build (gitignored by build/)
        if (!MkdirP(nob_temp_sprintf("%s/examples/build", gRoot))) return 1;
        // the compile, into a temporary file renamed into place (two builds at once never run half a file):
        // ${CC:-clang}, C11, every warning, -O1, as build.sh compiles nob.c
        const char *tmp = nob_temp_sprintf("%s.tmp.%ld", binary, (long)getpid());
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, Env("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-O1", "-o", tmp, source);
        if (!RunTimed(&cmd, "compile examples/nob.c", (Output){0})) Fail(1);
        nob_cmd_free(cmd);
        if (rename(tmp, binary) != 0) {
            SayErr("build.sh: could not move %s into place: %s", RelativeToRoot(binary), strerror(errno));
            Fail(1);
        }
    }
    Nob_Cmd run = {0};
    nob_cmd_append(&run, binary);
    nob_da_append_many(&run, (const char **)argv, (size_t)argc);
    Exec(binary, &run, false);
    return 1;
}

// #endregion

// #region Unit tests (./build.sh test [name...] [-- args...])

// What `./build.sh test` does with the list UNIT_TESTS (at the top of "Build steps": the tests' sources,
// flags and runs, with the meaning of every field). `./build.sh test` builds and runs all of them, in table
// order; `./build.sh test <name>...` the ones named; everything after a `--` is appended to the run's
// arguments of every test that runs (meant for one named test, e.g.
// `./build.sh test paintunwrap -- <model files>` or `./build.sh test export_strip -- <export dir>`).
// Each test builds into build/unit/<name>/ and runs there; the mode exits 1 when any test fails to build or
// run (after trying all of them) and prints which. A "run ./build.sh first" refusal counts as a failure.

// run_sh: builds tools/run-sh.c twice into the test's folder (the program run-sh.o with the flags
// `./build.sh --tools` uses, and run-sh-san.o with ASan + UBSan, so the test's runs of it are leak-checked
// too) and passes both to the test.
static bool PrepareRunSh(const UnitTest *t, const char *out, Nob_Cmd *run)
{
    (void)t;
    const char *source = nob_temp_sprintf("%s/tools/run-sh.c", gRoot);
    const char *plain = nob_temp_sprintf("%s/run-sh.o", out);
    const char *san = nob_temp_sprintf("%s/run-sh-san.o", out);
    Nob_Cmd cmd = {0};
    ToolCommand(&cmd, source, plain);
    bool ok = RunTimed(&cmd, "compile tools/run-sh.c (for run_sh_test)", (Output){0});
    if (ok) {
        nob_cmd_append(&cmd, Env("CC", "clang"));
        nob_da_append_many(&cmd, UNIT_FLAGS, NOB_ARRAY_LEN(UNIT_FLAGS));
        nob_cmd_append(&cmd, "-o", san, source);
        ok = RunTimed(&cmd, "compile tools/run-sh.c with ASan + UBSan (for run_sh_test)", (Output){0});
    }
    nob_cmd_free(cmd);
    nob_cmd_append(run, plain, san);
    return ok;
}

// `cp -r src/. dst/`: the contents of the folder src into dst (created when missing), overwriting files that
// are already there and removing nothing, so a second call refreshes a copy. Prints why and returns false on
// failure.
static bool CopyInto(const char *src, const char *dst)
{
    if (!MkdirP(dst)) return false;
    Nob_File_Paths children = {0};
    if (!nob_read_entire_dir(src, &children)) return false;
    bool ok = true;
    for (size_t i = 0; i < children.count && ok; i++) {
        const char *name = children.items[i];
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        size_t mark = nob_temp_save();
        const char *from = nob_temp_sprintf("%s/%s", src, name);
        const char *to = nob_temp_sprintf("%s/%s", dst, name);
        struct stat st;
        if (stat(from, &st) != 0) {
            SayErr("cp: cannot stat '%s': %s", from, strerror(errno));
            ok = false;
        } else if (S_ISDIR(st.st_mode)) {
            ok = CopyInto(from, to);
        } else {
            ok = CopyFile(from, to);
        }
        nob_temp_rewind(mark);
    }
    nob_da_free(children);
    return ok;
}

// Compiles the package at `dir` into dir/<name>.mapb with `monolith --compile` (hidden: no window). Its
// output goes to <out>/compile.log, and is copied to stderr when the compile fails.
static bool CompilePackage(const char *out, const char *dir, const char *name)
{
    const char *game = nob_temp_sprintf("%s/build/linux/monolith", gRoot);
    const char *log = nob_temp_sprintf("%s/compile.log", out);
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, game, "--map", dir, "--compile", nob_temp_sprintf("%s/%s.mapb", dir, name));
    bool ok = RunTimed(&cmd, nob_temp_sprintf("compile package %s (for compiled test)", name), (Output){.log = log});
    nob_cmd_free(cmd);
    if (!ok) {
        Nob_String_Builder text = {0};
        if (nob_read_entire_file(log, &text)) fwrite(text.items, 1, text.count, stderr);
        nob_sb_free(text);
    }
    return ok;
}

// compiled: the three packages the test reads, as compiled maps in build/unit/compiled/.
//   models, rotated  copies of tests/models and tests/rotated (turned boxes), compiled where they are
//   longname         made here: a texture three folders of 120 'e acute' deep, so the error-message cases
//                    have messages longer than any fixed buffer (compiled_test.c, MessageCases)
// All three are compiled with monolith --compile (build/linux/monolith, which RunUnitTest has already
// checked for).
static bool PrepareCompiled(const UnitTest *t, const char *out, Nob_Cmd *run)
{
    (void)t;
    (void)run;
    // the 120 'é' (UTF-8: 0xC3 0xA9, 240 bytes)
    Nob_String_Builder e = {0};
    for (int i = 0; i < 120; i++) nob_sb_append_cstr(&e, "\xc3\xa9");
    nob_sb_append_null(&e);

    // longname: the folders' path and the map that uses the texture (the strings are built first, so the
    // 'é' text can be freed before anything can fail)
    const char *longDir = nob_temp_sprintf("%s/longname", out);
    const char *deep = nob_temp_sprintf("%s/textures/%s/%s/%s", longDir, e.items, e.items, e.items);
    const char *mapText = nob_temp_sprintf(
        "monolith 3\nmaterial \"long\" albedo_map \"textures/%s/%s/%s/tile.png\"\nuse \"long\"\nbox 0 0 0 1 1 1\nspawn 0 0 3 0",
        e.items, e.items, e.items);
    nob_sb_free(e);

    // longname: the folders, the texture (a copy of tests/models' tiles.png) and the map
    if (!MkdirP(deep)) return false;
    if (!CopyFile(nob_temp_sprintf("%s/tests/models/textures/tiles.png", gRoot), nob_temp_sprintf("%s/tile.png", deep))) {
        return false;
    }
    if (!WriteLine(nob_temp_sprintf("%s/longname.map", longDir), mapText)) return false;

    // models and rotated: refresh the copies (nothing is removed from them)
    const char *copied[] = {"models", "rotated"};
    for (size_t i = 0; i < NOB_ARRAY_LEN(copied); i++) {
        const char *dir = nob_temp_sprintf("%s/%s", out, copied[i]);
        if (!CopyInto(nob_temp_sprintf("%s/tests/%s", gRoot, copied[i]), dir)) return false;
    }

    // the compile of all three, in the order the test runs them
    return CompilePackage(out, nob_temp_sprintf("%s/models", out), "models") &&
           CompilePackage(out, nob_temp_sprintf("%s/rotated", out), "rotated") &&
           CompilePackage(out, longDir, "longname");
}

// Appends the files `pattern` matches to `files`, in the order bash's glob gives them (sorted by the locale's
// collation); no match appends nothing (the script tested each name with [[ -f ]]).
static void GlobFiles(const char *pattern, Nob_File_Paths *files)
{
    glob_t g;
    if (glob(pattern, 0, NULL, &g) != 0) return;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        if (IsRegularFile(g.gl_pathv[i])) nob_da_append(files, nob_temp_strdup(g.gl_pathv[i]));
    }
    globfree(&g);
}

// paintunwrap, in three parts:
//   1. xatlas (the vendored paint-unwrap library) built with the sanitizers into <out>/xatlas.o, with the
//      script's flags: -fno-sanitize=function too, because its OpenNL part calls matrix destructors
//      through a pointer of the base struct type (UB by the letter, ABI-compatible). The compiler is
//      ${CXX:-clang++}; it is rebuilt when xatlas or nob.c changed or the flags did (Xatlas above).
//   2. the scratch folder the test writes in (--scratch <out>/scratch).
//   3. the models it unwraps: every .glb, .gltf and .obj in maps/*/models/ (read by R3D in a hidden
//      window), and --scene on the package with the most of them (the first one on a tie).
static bool PreparePaintunwrap(const UnitTest *t, const char *out, Nob_Cmd *run)
{
    (void)t;
    // 1. xatlas with the sanitizers
    Nob_Cmd san = {0};
    nob_cmd_append(&san, "-g", "-O1", "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
                   "-fno-sanitize-recover=all", "-fno-sanitize=function");
    Xatlas(Env("CXX", "clang++"), nob_temp_sprintf("%s/xatlas.o", out), &san);
    nob_cmd_free(san);

    // 2. the scratch folder
    const char *scratch = nob_temp_sprintf("%s/scratch", out);
    if (!MkdirP(scratch)) return false;
    nob_cmd_append(run, "--scratch", scratch);

    // 3. the models of every package, and the package with the most
    const char *mapsDir = nob_temp_sprintf("%s/maps", gRoot);
    glob_t packages;
    Nob_File_Paths models = {0};
    const char *scene = NULL;
    size_t most = 0;
    if (glob(nob_temp_sprintf("%s/*/", mapsDir), 0, NULL, &packages) == 0) {
        for (size_t i = 0; i < packages.gl_pathc; i++) {
            const char *pkg = packages.gl_pathv[i];  // ends with '/'
            size_t before = models.count;
            GlobFiles(nob_temp_sprintf("%smodels/*.glb", pkg), &models);
            GlobFiles(nob_temp_sprintf("%smodels/*.gltf", pkg), &models);
            GlobFiles(nob_temp_sprintf("%smodels/*.obj", pkg), &models);
            if (models.count - before > most) {
                most = models.count - before;
                scene = nob_temp_strndup(pkg, strlen(pkg) - 1);  // without the trailing '/'
            }
        }
        globfree(&packages);
    }
    if (scene) nob_cmd_append(run, "--scene", scene);
    for (size_t i = 0; i < models.count; i++) nob_cmd_append(run, models.items[i]);
    nob_da_free(models);
    return true;
}

// `s` with every "{root}", "{out}" and "{each}" replaced (a temp string); `each` may be NULL.
static const char *UnitSubst(const char *s, const char *out, const char *each)
{
    const char *r = s;
    while (strstr(r, "{root}")) r = ReplaceFirst(r, "{root}", gRoot);
    while (strstr(r, "{out}")) r = ReplaceFirst(r, "{out}", out);
    while (each && strstr(r, "{each}")) r = ReplaceFirst(r, "{each}", each);
    return r;
}

// The compile of one test: UNIT_FLAGS, -DMS_PATCHED_R3D, the entry's defines, R3D's and raylib's -I, the
// entry's -I folders, the sources, -o <binary>, R3D's libraries (the order the scripts gave them; r3d parts
// only when the entry links R3D). Returns the command (the caller runs and frees it).
static Nob_Cmd UnitCompileCommand(const UnitTest *t, const char *out, const char *binary)
{
    const char *libDir = nob_temp_sprintf("%s/build-linux", gR3dDir);
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, Env("CC", "clang"));
    nob_da_append_many(&cmd, UNIT_FLAGS, NOB_ARRAY_LEN(UNIT_FLAGS));
    if (t->r3d) nob_cmd_append(&cmd, "-DMS_PATCHED_R3D");
    for (const char **d = t->defines; d && *d; d++) nob_cmd_append(&cmd, *d);
    if (t->r3d) {
        nob_cmd_append(&cmd, nob_temp_sprintf("-I%s/include", gR3dDir),
                       nob_temp_sprintf("-I%s/external/raylib/raylib/include", libDir));
    }
    for (const char **i = t->includes; i && *i; i++) nob_cmd_append(&cmd, nob_temp_sprintf("-I%s/%s", gRoot, *i));
    for (const char **s = t->sources; *s; s++) {
        const char *path = UnitSubst(*s, out, NULL);
        nob_cmd_append(&cmd, path[0] == '/' ? path : nob_temp_sprintf("%s/%s", gRoot, path));
    }
    nob_cmd_append(&cmd, "-o", binary);
    if (t->r3d) {
        nob_cmd_append(&cmd, nob_temp_sprintf("-L%s/lib", libDir), "-lr3d", "-lraylib", "-lassimp",
                       "-lzlibstatic", "-lstdc++", "-lGL", "-lm", "-lpthread", "-ldl");
    }
    return cmd;
}

// Builds and runs one test; false when it failed (the reason is printed). `extra` are the arguments after
// `--` on the command line, appended to the run's arguments.
static bool RunUnitTest(const UnitTest *t, const Nob_File_Paths *extra)
{
    // Step 1: the test's folder, and what the test needs from an earlier ./build.sh. These are the scripts'
    // "run ./build.sh first" checks.
    const char *out = nob_temp_sprintf("%s/build/unit/%s", gRoot, t->name);
    if (!MkdirP(out)) return false;
    const char *libDir = nob_temp_sprintf("%s/build-linux", gR3dDir);
    if (t->r3d && !IsRegularFile(nob_temp_sprintf("%s/lib/libr3d.a", libDir))) {
        SayErr("build.sh: test %s: R3D's libraries are missing; run ./build.sh first", t->name);
        return false;
    }
    if (t->game && !IsRegularFile(nob_temp_sprintf("%s/build/linux/monolith", gRoot))) {
        SayErr("build.sh: test %s: build/linux/monolith is missing; run ./build.sh first", t->name);
        return false;
    }

    // Step 2: the prepare step (copies data, builds helpers); it appends the run's first arguments.
    const char *binary = nob_temp_sprintf("%s/%s_test", out, t->name);
    Nob_Cmd pre = {0};
    if (t->prepare && !t->prepare(t, out, &pre)) {
        nob_cmd_free(pre);
        return false;
    }

    // Step 3: the compile.
    Nob_Cmd cmd = UnitCompileCommand(t, out, binary);
    bool ok = RunTimed(&cmd, nob_temp_sprintf("compile %s test", t->name), (Output){0});
    nob_cmd_free(cmd);

    // Step 4: the run(s), with the sanitizers' leak check and the entry's environment (set here, inherited
    // by the test, put back afterwards).
    Nob_File_Paths names = {0}, saved = {0};
    if (ok) {
        Nob_File_Paths env = {0};
        nob_da_append(&env, "ASAN_OPTIONS=detect_leaks=1");
        for (const char **e = t->env; e && *e; e++) nob_da_append(&env, UnitSubst(*e, out, NULL));
        for (size_t i = 0; i < env.count; i++) {
            const char *eq = strchr(env.items[i], '=');
            if (!eq) continue;
            const char *name = nob_temp_strndup(env.items[i], (size_t)(eq - env.items[i]));
            const char *old = getenv(name);
            nob_da_append(&names, name);
            nob_da_append(&saved, old ? nob_temp_strdup(old) : NULL);
            setenv(name, eq + 1, 1);
        }
        nob_da_free(env);

        // one run, or one per `each` item ("== <item>" first; the first failure ends the test, as the
        // script's `set -e` did)
        size_t runs = 0;
        for (const char **e = t->each; e && *e; e++) runs++;
        if (runs == 0) runs = 1;
        for (size_t r = 0; r < runs && ok; r++) {
            const char *item = t->each ? t->each[r] : NULL;
            if (item) Say("== %s", item);
            Nob_Cmd run = {0};
            nob_cmd_append(&run, binary);
            nob_da_append_many(&run, pre.items, pre.count);
            for (const char **a = t->args; a && *a; a++) nob_cmd_append(&run, UnitSubst(*a, out, item));
            for (size_t i = 0; i < extra->count; i++) nob_cmd_append(&run, extra->items[i]);
            ok = RunTimed(&run, nob_temp_sprintf("run %s test%s%s", t->name, item ? " " : "", item ? item : ""),
                          (Output){0});
            nob_cmd_free(run);
        }
        for (size_t i = names.count; i-- > 0;) {
            if (saved.items[i]) setenv(names.items[i], saved.items[i], 1);
            else unsetenv(names.items[i]);
        }
    }
    nob_da_free(names);
    nob_da_free(saved);
    nob_cmd_free(pre);
    return ok;
}

// ./build.sh test [name...] [-- args...]: the named tests (each must be in UNIT_TESTS) or all, in table
// order; the arguments after `--` go to every test's run.
static int RunUnitTests(int argc, char **argv)
{
    // which tests: the names before `--`, each checked against the list; or all of them
    Nob_File_Paths names = {0}, extra = {0};
    bool afterDashes = false;
    for (int i = 0; i < argc; i++) {
        if (afterDashes) {
            nob_da_append(&extra, argv[i]);
            continue;
        }
        if (strcmp(argv[i], "--") == 0) {
            afterDashes = true;
            continue;
        }
        bool known = false;
        for (size_t k = 0; k < NOB_ARRAY_LEN(UNIT_TESTS); k++) known = known || strcmp(argv[i], UNIT_TESTS[k].name) == 0;
        if (!known) {
            SayErr("build.sh: test: no test named '%s'. The tests:", argv[i]);
            for (size_t k = 0; k < NOB_ARRAY_LEN(UNIT_TESTS); k++) SayErr("  %s", UNIT_TESTS[k].name);
            nob_da_free(names);
            nob_da_free(extra);
            return 1;
        }
        nob_da_append(&names, argv[i]);
    }

    // each test in table order; a failure doesn't stop the others
    Nob_File_Paths failed = {0};
    size_t ran = 0;
    for (size_t k = 0; k < NOB_ARRAY_LEN(UNIT_TESTS); k++) {
        bool wanted = names.count == 0;
        for (size_t i = 0; i < names.count; i++) wanted = wanted || strcmp(names.items[i], UNIT_TESTS[k].name) == 0;
        if (!wanted) continue;
        ran++;
        if (!RunUnitTest(&UNIT_TESTS[k], &extra)) nob_da_append(&failed, UNIT_TESTS[k].name);
    }
    PrintSummary();
    if (failed.count == 0) {
        Say("tests: %zu passed", ran);
    } else {
        SayErr("tests: %zu of %zu failed:", failed.count, ran);
        for (size_t i = 0; i < failed.count; i++) SayErr("  %s", failed.items[i]);
    }
    int code = failed.count == 0 ? 0 : 1;
    nob_da_free(names);
    nob_da_free(extra);
    nob_da_free(failed);
    return code;
}

// #endregion

int main(int argc, char **argv)
{
    gStartNanos = nob_nanos_since_unspecified_epoch();
    // The terminal's stdout / stderr, kept for our messages while a command's output goes to a log
    gOut = fdopen(dup(STDOUT_FILENO), "w");
    gErr = fdopen(dup(STDERR_FILENO), "w");
    if (!gOut || !gErr) {
        fprintf(stderr, "build: can't duplicate stdout/stderr: %s\n", strerror(errno));
        return 1;
    }
    nob_set_log_handler(LogHandler);
    // glob order as bash's: the environment's collation (only LC_COLLATE: number formatting stays "C")
    setlocale(LC_COLLATE, "");

    // The repo root from this program's path (<root>/build/nob): ./build.sh runs it by its absolute path; run
    // directly (build/nob, ../build/nob), the path is taken relative to the current directory. Lexically
    // normalised, without resolving symlinks, so the baked-in paths are those `cd "$(dirname build.sh)" && pwd`
    // gives.
    if (!strchr(argv[0], '/')) {
        SayErr("build: run me by my path (./build.sh, or build/nob from the repo root)");
        return 1;
    }
    const char *self = argv[0][0] == '/' ? argv[0] : nob_temp_sprintf("%s/%s", LogicalCwd(), argv[0]);
    self = NormalizeAbsolute(self);
    gRoot = nob_temp_dir_name(nob_temp_dir_name(self));

    // Rebuild build/nob first when nob.c or nob.h is newer than it (only when it is run directly: ./build.sh
    // already recompiled it). nob.h's NOB_GO_REBUILD_URSELF_PLUS(argc, argv, "src/vendor/nob/nob.h") would name
    // nob.c by __FILE__ and nob.h relative to the current directory; the same function with absolute paths
    // from the root works from any directory and after the repo moved. Its compile isn't timed.
    nob__go_rebuild_urself(argc, argv, nob_temp_sprintf("%s/nob.c", gRoot),
                           nob_temp_sprintf("%s/src/vendor/nob/nob.h", gRoot), NULL);

    gR3dDir = Env("R3D_DIR", nob_temp_sprintf("%s/submodules/r3d", gRoot));
    nob_shift(argv, argc);  // the program name
    if (argc > 0 && strcmp(argv[0], "--win") == 0) {
        nob_shift(argv, argc);
        return BuildWindows(argc, argv);
    }
    if (argc > 0 && strcmp(argv[0], "--tools") == 0) {
        nob_shift(argv, argc);
        return BuildTools(argc, argv);
    }
    if (argc > 0 && strcmp(argv[0], "examples") == 0) {
        nob_shift(argv, argc);
        return BuildExamples(argc, argv);
    }
    if (argc > 0 && strcmp(argv[0], "test") == 0) {
        nob_shift(argv, argc);
        return RunUnitTests(argc, argv);
    }
    return BuildLinux(argc, argv);
}
