// examples/nob.c: the build program of the examples. `./build.sh examples [mode]` (nob.c, BuildExamples)
// compiles this file into examples/build/nob when that is missing or older than this file or nob.h, then runs
// it in its place with the mode; run directly (examples/build/nob [mode]) it rebuilds itself the same way
// (NOB_GO_REBUILD_URSELF). Written on nob.h (src/vendor/nob/nob.h, vendored unmodified), like the repo's own
// build program nob.c, whose structure and helpers it follows. It replaced examples/build.sh (2026-11-13):
// the same commands with the same arguments in the same order, the same output folders and data copies.
//
// Modes (the first argument; any other argument, or none, is the plain build, as the script read only $1):
//   ./build.sh examples          -> examples/build/linux/<example>/       (clang, -O2)
//   ./build.sh examples --win    -> examples/build/win/<example>/         (MinGW-w64: x86_64-w64-mingw32-gcc)
//   ./build.sh examples test     -> examples/build/linux-test/<example>/  (the test build, below)
//   ./build.sh examples lsan     -> examples/build/linux-lsan/<example>/  (the test build with LeakSanitizer,
//                                   -O1 -g; the leak check runs these)
// Each output folder holds the program(s) and the data they load (scene/ or assets/), so it runs from anywhere.
// The object files go next to it, into <output folder>-obj/<example>/ (outside the program folders, which stay
// copyable), as does the log of the last scene compile (<output folder>-obj/compile.log).
//
// The examples are plain integration code. The test builds compile each example's main file with
// -include tests/examples/harness.h: the same program then needs --shot <file>, renders 30 frames in a hidden
// window, saves the image and exits (it never opens a visible window), e.g.
//   examples/build/linux-test/01_minimal/minimal --shot /tmp/x.png --view 2
// or, as a benchmark of the runtime as a game builds it (the harness's bench mode; one at a time,
// __GL_SYNC_TO_VBLANK=0): examples/build/linux-test/01_minimal/minimal --bench report.md <package>
//
// Builds against monolith's R3D copy (submodules/r3d, with -DMS_PATCHED_R3D) and its static libraries, which
// the repo's ./build.sh builds (run it once first, or ./build.sh --win for --win). Same compiler flags as the
// game (zero warnings), same static linking and glibc compatibility shim (src/glibc_compat.c) on Linux. The
// runtime is built like an exported game (no -DMONOLITH_EDITOR), except for 03_live_editing (MS_Poll). A game
// build reads only compiled maps, so the game-style examples get their scene compiled next to the copied text
// (<name>.mapb, through build/linux/monolith --compile, also for --win); 03_live_editing loads the text.
//
// Environment:
//   R3D_DIR   another R3D tree (default: submodules/r3d); its build-linux/ (or build-mingw/ for --win) must
//             hold the static libraries, include/ and raylib's headers
//   CC        the Linux compiler (default clang); --win always uses x86_64-w64-mingw32-gcc
//
// Exit codes: 0 every example built; 1 something is missing (build/linux/monolith, the R3D libraries), a
// command failed (its output above says why) or a scene did not compile (its log is printed).
//
// Output: like nob.c. Every command runs through RunTimed(), which echoes it ("[INFO] CMD: ...") and prints
// how long it took ("[TIME] ..."); a summary table ends the run. Those lines and the progress lines
// ("built: <path>", "compiled: <path>", the text the script printed) go to stdout, errors to stderr, so
// `./build.sh examples lsan > /dev/null` (the leak check) stays quiet unless something fails.
//
// Keep in step: the repo's nob.c mirrors these programs and their flags in compile_commands.json for clangd
// (WriteCompileCommands, "the examples"), and has the same glibc --wrap list (glibcWrap). A flag change here
// changes those lines there.

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

// Our progress lines (the script's `echo`s: "built: ...", "compiled: ..."): to the terminal's stdout.
NOB_PRINTF_FORMAT(1, 2) static void Say(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(gOut, fmt, args);
    va_end(args);
    fputc('\n', gOut);
    fflush(gOut);
}

// Error lines (the script's `echo ... >&2`): to the terminal's stderr, with the program's prefix.
NOB_PRINTF_FORMAT(1, 2) static void SayErr(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(gErr, "build.sh: examples: ");
    vfprintf(gErr, fmt, args);
    va_end(args);
    fputc('\n', gErr);
    fflush(gErr);
}

// A struct timeval in seconds.
static double Seconds(struct timeval tv)
{
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

// Runs one command (resetting it, as nob_cmd_run does) and records its time; the only place a command runs.
// Prints nob's "[INFO] CMD: ..." echo, runs it synchronously, then
// "[TIME] <label>: <wall> s wall, <cpu> s CPU (<user> user + <sys> sys)". `log`: NULL leaves the command's
// output on the terminal; a path sends its stdout and stderr there (`> log 2>&1`, truncating it).
// CPU time is the RUSAGE_CHILDREN difference around the command: exact because nothing else runs meanwhile.
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
    fprintf(gOut, "[TIME] %s: %.2f s wall, %.2f s CPU (%.2f user + %.2f sys)%s\n", label, t.wall, t.user + t.sys,
            t.user, t.sys, ok ? "" : " (failed)");
    fflush(gOut);
    return ok;
}

// The summary table of every timed command and their total, plus the whole run's wall time (which also
// covers this program's own work: the data copies). Printed at the end, and before a failed run exits.
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
static Nob_Cmd gExtraSrc;        // sources every program links (Linux: src/glibc_compat.c)
static Nob_Cmd gLibs;            // the link flags after -o
static const char *gMonolith;    // build/linux/monolith, which compiles the scenes

// The path relative to the repo root (the script's ${path#"$ROOT/"}): unchanged when it isn't under it.
static const char *RelativeToRoot(const char *path)
{
    size_t n = strlen(gRoot);
    if (strncmp(path, gRoot, n) == 0 && path[n] == '/') return path + n + 1;
    return path;
}

// Example <dir> <program> <main file> <flags> [runtime]: the main file is compiled on its own (in the test
// builds with the harness force-included, which must not reach the runtime), then linked with the other
// sources (the runtime when `runtime`, then the extra sources). `flags` is NULL-terminated: -DMONOLITH_EDITOR
// for the editor runtime (then also passed to the link, which compiles the runtime), -DHARNESS_MAIN_NO_ARGS
// for a main(void), -include ... for a shim. Prints "built: <output path>" like the script.
static void Example(const char *dir, const char *prog, const char *mainFile, const char **flags, bool runtime)
{
    // -DMONOLITH_EDITOR among the flags is passed to the link command too (the runtime is compiled there)
    bool editor = false;
    for (const char **f = flags; *f; f++) editor = editor || strcmp(*f, "-DMONOLITH_EDITOR") == 0;

    // mkdir -p <out>/<dir> <out>-obj/<dir>: the program's folder and its object's (outside the program
    // folders, which stay copyable)
    const char *progDir = nob_temp_sprintf("%s/%s", gOutDir, dir);
    const char *objDir = nob_temp_sprintf("%s-obj/%s", gOutDir, dir);
    if (!MkdirP(progDir) || !MkdirP(objDir)) Fail();
    const char *obj = nob_temp_sprintf("%s/%s.o", objDir, prog);
    const char *exe = nob_temp_sprintf("%s/%s%s", progDir, prog, gExe);

    // compile: $CC CFLAGS INCLUDES -DMS_PATCHED_R3D <flags> HARNESS -c <main> -o <obj>
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, gCc);
    nob_da_append_many(&cmd, gCflags.items, gCflags.count);
    nob_da_append_many(&cmd, gIncludes.items, gIncludes.count);
    nob_cmd_append(&cmd, "-DMS_PATCHED_R3D");
    for (const char **f = flags; *f; f++) nob_cmd_append(&cmd, *f);
    nob_da_append_many(&cmd, gHarness.items, gHarness.count);
    nob_cmd_append(&cmd, "-c", mainFile, "-o", obj);
    if (!RunTimed(&cmd, nob_temp_sprintf("compile %s", RelativeToRoot(obj)), NULL)) Fail();

    // link: $CC CFLAGS INCLUDES -DMS_PATCHED_R3D [-DMONOLITH_EDITOR] <obj> [runtime] EXTRA_SRC -o <exe> LIBS
    nob_cmd_append(&cmd, gCc);
    nob_da_append_many(&cmd, gCflags.items, gCflags.count);
    nob_da_append_many(&cmd, gIncludes.items, gIncludes.count);
    nob_cmd_append(&cmd, "-DMS_PATCHED_R3D");
    if (editor) nob_cmd_append(&cmd, "-DMONOLITH_EDITOR");
    nob_cmd_append(&cmd, obj);
    if (runtime) nob_cmd_append(&cmd, nob_temp_sprintf("%s/runtime/monolith_scene.c", gRoot));
    nob_da_append_many(&cmd, gExtraSrc.items, gExtraSrc.count);
    nob_cmd_append(&cmd, "-o", exe);
    nob_da_append_many(&cmd, gLibs.items, gLibs.count);
    if (!RunTimed(&cmd, nob_temp_sprintf("link %s", RelativeToRoot(exe)), NULL)) Fail();
    nob_cmd_free(cmd);

    Say("built: %s", RelativeToRoot(exe));
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

    // Compile flags: the game's (C11, every warning, -O2); lsan: -g -O1 with frame pointers and
    // LeakSanitizer, so the leak check's stacks are readable
    if (lsan) {
        nob_cmd_append(&gCflags, "-std=c11", "-Wall", "-Wextra", "-Wno-missing-field-initializers", "-g", "-O1",
                       "-fno-omit-frame-pointer", "-fsanitize=leak");
    } else {
        nob_cmd_append(&gCflags, "-std=c11", "-Wall", "-Wextra", "-Wno-missing-field-initializers", "-O2");
    }

    // Per target: the compiler, R3D's library folder, the output folder and the link flags
    const char *libDir;
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

    // What must exist first: the game (it compiles the scenes, also for --win) and R3D's libraries for the
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

    Example("01_minimal", "minimal", nob_temp_sprintf("%s/01_minimal/main.c", gHere), none, true);
    Data("01_minimal", "01_minimal/scene", "scene");
    Compiled("01_minimal", "scene");

    Example("02_existing_project", "before", nob_temp_sprintf("%s/02_existing_project/before.c", gHere), noArgs, false);
    Example("02_existing_project", "after", nob_temp_sprintf("%s/02_existing_project/after.c", gHere), none, true);
    Data("02_existing_project", "01_minimal/scene", "scene");
    Compiled("02_existing_project", "scene");

    Example("03_live_editing", "live_editing", nob_temp_sprintf("%s/03_live_editing/main.c", gHere), editor, true);
    Data("03_live_editing", "01_minimal/scene", "scene");

    Example("05_entities", "entities", nob_temp_sprintf("%s/05_entities/main.c", gHere), none, true);
    Data("05_entities", "01_minimal/scene", "scene");
    Compiled("05_entities", "scene");

    PrintSummary();
    return 0;
}
