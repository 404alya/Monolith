// nob.c: the build program of this folder. ./build.sh compiles it into build/nob (when that is missing or older
// than nob.c or nob.h) and runs it with its own arguments; build/nob run directly rebuilds itself the same way.
// It is written on nob.h (next to it: a single-header C library by Alexey Kutepov, MIT licence or public
// domain, LICENSES/nob.txt), which runs the commands.
//
//   ./build.sh                 Linux x86_64 -> build/linux/viewer
//   ./build.sh --win           Windows x86_64 with MinGW-w64 (cross-compiler x86_64-w64-mingw32-gcc, e.g. from
//                              Linux) -> build/win/viewer.exe
//   ./build.sh [--win] --from-source   build R3D, raylib and assimp from r3d/ even when prebuilt/ has them
// Any other argument prints the usage and exits 2.
//
// R3D, raylib and assimp: the static libraries in prebuilt/<linux|win>/lib/ (built by monolith from r3d/) when
// this folder has them for the target, so only the viewer and the scene runtime are compiled. If linking
// against them fails (a compiler or C library older than the one they were built with), or with --from-source,
// they are built from r3d/ with CMake into build/r3d-linux/ (or build/r3d-mingw/): a few minutes the first
// time, later runs only rebuild what changed. Delete those folders to force a fresh configure.
//
// The map package is copied next to the program, so build/linux/ (or build/win/) can be moved anywhere as it is.
//
// Needs: a C compiler and the C++ runtime (assimp is C++); on Linux the OpenGL / X11 libraries. Building from
// source also needs cmake (3.22 or newer), python3 (R3D turns its shaders into C headers at build time), a C++
// compiler and on Linux the X11 / OpenGL development headers raylib needs (Debian / Ubuntu: libx11-dev
// libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev; Fedora: libX11-devel libXrandr-devel
// libXinerama-devel libXcursor-devel libXi-devel mesa-libGL-devel; Arch: libx11 libxrandr libxinerama
// libxcursor libxi mesa).
//
// Environment: CC, the Linux C compiler (default cc); it also compiles this program. --win always uses
// x86_64-w64-mingw32-gcc.
//
// Output: the progress lines below ("linking against ...", "configuring R3D ...", "building R3D ...",
// "built: ...") on stdout; the compiler's own messages as it prints them; a failed CMake step prints its log
// (the configure log whole, the last 50 lines of the build log). Exit codes: 0 built; 1 a step failed;
// 2 usage.

// sysconf, readlink, symlink and the other POSIX declarations, before any system header (nob.h only sets
// _POSIX_C_SOURCE when nothing else has picked a feature level).
#define _GNU_SOURCE

// nob.h would print "[INFO] CMD: ..." before every command; this program prints only its own progress lines.
#define NOB_NO_ECHO
// How build/nob rebuilds itself when nob.c or nob.h is newer than it (main(), nob__go_rebuild_urself): the
// compiler and flags ./build.sh compiles it with (${CC:-cc}, C11, all warnings, -O1), so both give the same
// program.
#define NOB_REBUILD_URSELF(binary_path, source_path) \
    (getenv("CC") && *getenv("CC") ? getenv("CC") : "cc"), \
    "-std=c11", "-Wall", "-Wextra", "-O1", "-o", (binary_path), (source_path)
#define NOB_IMPLEMENTATION
#include "nob.h"

#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

// The map package this folder was made for: the folder <MAP_NAME>/ next to this file, copied next to the
// program after the build.
#define MAP_NAME "@MAP@"

// #region Messages and commands

// nob's own messages: errors and warnings to stderr, as "[ERROR] ..." / "[WARNING] ...". Its "[INFO]" lines
// are dropped (this program prints its own progress), and so is "command exited with exit code N" after a
// command that failed: the command has printed why, and the step that ran it says what happens next.
static void LogHandler(Nob_Log_Level level, const char *fmt, va_list args)
{
    if (level == NOB_INFO || level == NOB_NO_LOGS) return;
    if (strcmp(fmt, "command exited with exit code %d") == 0) return;
    fprintf(stderr, level == NOB_WARNING ? "[WARNING] " : "[ERROR] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    fflush(stderr);
}

// A progress line on stdout.
NOB_PRINTF_FORMAT(1, 2) static void Say(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    putchar('\n');
    fflush(stdout);
}

// A message on stderr.
NOB_PRINTF_FORMAT(1, 2) static void SayErr(const char *fmt, ...)
{
    fflush(stdout);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

// Runs one command to its end (resetting it for the next one) and says whether it succeeded. `log`: NULL leaves
// its output on the terminal; a path sends both its stdout and stderr there (`> log 2>&1`): this process's fds
// 1 and 2 point at the file while the command runs, so the child inherits one shared file description and the
// two streams don't overwrite each other.
static bool Run(Nob_Cmd *cmd, const char *log)
{
    int savedOut = -1, savedErr = -1;
    fflush(stdout);
    fflush(stderr);
    if (log) {
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            SayErr("cannot write %s: %s", log, strerror(errno));
            cmd->count = 0;
            return false;
        }
        savedOut = dup(STDOUT_FILENO);
        savedErr = dup(STDERR_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    bool ok = nob_cmd_run_opt(cmd, (Nob_Cmd_Opt){0});
    if (log) {
        dup2(savedOut, STDOUT_FILENO);
        dup2(savedErr, STDERR_FILENO);
        close(savedOut);
        close(savedErr);
    }
    return ok;
}

// Prints a log file on stdout: all of it, or its last `lines` lines (0: all).
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
    fwrite(sb.items + start, 1, sb.count - start, stdout);
    fflush(stdout);
    nob_sb_free(sb);
}

// #endregion

// #region Paths and files

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
// through a symlink stays as typed), else getcwd().
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
        SayErr("cannot read the current directory");
        exit(1);
    }
    return cwd;
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

// A regular file (following symlinks), like the shell's [[ -f path ]].
static bool IsFile(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// `cp src dst` for one regular file: dst is created with src's mode (minus the umask), or truncated and
// overwritten when it exists (keeping its mode), as cp does without -p.
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

// `cp -R src/. dst/` (dst is an existing folder): every entry of src, dot files included, merged into dst. A
// folder that exists in dst is entered, a missing one is created with its source mode; a file is overwritten
// in place; a symlink is copied as a symlink. Files in dst that src doesn't have are kept.
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
            // a symlink: a link with the same target (an entry of that name already there is an error)
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

// #endregion

// #region The build

// This folder (absolute, no trailing slash), from this program's own path (<folder>/build/nob).
static const char *gHere;

// The path relative to this folder (for messages): unchanged when it isn't under it.
static const char *Relative(const char *path)
{
    size_t n = strlen(gHere);
    if (strncmp(path, gHere, n) == 0 && path[n] == '/') return path + n + 1;
    return path;
}

// What the target decides (set in main()): the compiler, the program's path and the link flags after -L<dir>.
static bool gWin;
static Nob_Cmd gCflags;
static Nob_Cmd gSources;
static const char *gExe;

// Compiles and links the viewer against the R3D, raylib and assimp libraries in `libDir`; true when the
// compiler succeeded (its own messages say why not).
static bool Link(const char *libDir)
{
    Nob_Cmd cmd = {0};
    if (gWin) {
        // -static: libgcc, libstdc++ (assimp) and winpthread go into the .exe too, leaving only system DLLs
        // (opengl32, gdi32, winmm, ...); -s strips it
        nob_cmd_append(&cmd, "x86_64-w64-mingw32-gcc");
        nob_da_append_many(&cmd, gCflags.items, gCflags.count);
        nob_da_append_many(&cmd, gSources.items, gSources.count);
        nob_cmd_append(&cmd, "-o", gExe, "-s", "-static", nob_temp_sprintf("-L%s", libDir), "-lr3d", "-lraylib",
                       "-lassimp", "-lzlibstatic", "-lstdc++", "-lopengl32", "-lgdi32", "-lwinmm", "-lshell32", "-lm");
    } else {
        const char *cc = getenv("CC") && *getenv("CC") ? getenv("CC") : "cc";
        nob_cmd_append(&cmd, cc);
        nob_da_append_many(&cmd, gCflags.items, gCflags.count);
        nob_da_append_many(&cmd, gSources.items, gSources.count);
        nob_cmd_append(&cmd, "-o", gExe, nob_temp_sprintf("-L%s", libDir), "-lr3d", "-lraylib", "-lassimp",
                       "-lzlibstatic", "-lstdc++", "-lGL", "-lm", "-lpthread", "-ldl");
    }
    bool ok = Run(&cmd, NULL);
    nob_cmd_free(cmd);
    return ok;
}

// R3D with its vendored raylib and assimp as static libraries, built with CMake in `dir` from r3d/: configured
// once (when dir has no CMakeCache.txt yet), then an incremental build on every call. `args` are the configure
// arguments. Exits 1 with the step's log when configuring or building fails.
static void BuildR3D(const char *dir, Nob_Cmd *args)
{
    if (!MkdirP(dir)) exit(1);
    const char *r3d = nob_temp_sprintf("%s/r3d", gHere);

    // configure: cmake -S r3d -B <dir> <args> > <dir>/configure.log 2>&1, the whole log when it fails
    if (!IsFile(nob_temp_sprintf("%s/CMakeCache.txt", dir))) {
        Say("configuring R3D in %s ...", Relative(dir));
        const char *log = nob_temp_sprintf("%s/configure.log", dir);
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, "cmake", "-S", r3d, "-B", dir);
        nob_da_append_many(&cmd, args->items, args->count);
        if (!Run(&cmd, log)) {
            PrintLog(log, 0);
            exit(1);
        }
        nob_cmd_free(cmd);
    }

    // build: cmake --build <dir> -j<online CPUs> > <dir>/build.log 2>&1, its last 50 lines when it fails
    const char *log = nob_temp_sprintf("%s/build.log", dir);
    Say("building R3D, raylib and assimp from source (log: %s) ...", Relative(log));
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "cmake", "--build", dir, nob_temp_sprintf("-j%ld", cpus > 0 ? cpus : 4));
    if (!Run(&cmd, log)) {
        PrintLog(log, 50);
        exit(1);
    }
    nob_cmd_free(cmd);
}

int main(int argc, char **argv)
{
    nob_set_log_handler(LogHandler);

    // This folder from the program's path (<folder>/build/nob): ./build.sh runs it by its absolute path; run
    // directly, the path is taken relative to the current directory. Lexically normalised, so a folder
    // reached through a symlink keeps the name it was reached by.
    if (!strchr(argv[0], '/')) {
        SayErr("run me by my path (./build.sh, or build/nob in this folder)");
        return 1;
    }
    const char *self = argv[0][0] == '/' ? argv[0] : nob_temp_sprintf("%s/%s", LogicalCwd(), argv[0]);
    self = NormalizeAbsolute(self);
    gHere = nob_temp_dir_name(nob_temp_dir_name(self));

    // Rebuild build/nob first when nob.c or nob.h is newer than it (only when it is run directly: ./build.sh
    // already compiled it), by absolute paths so it works from any directory
    nob__go_rebuild_urself(argc, argv, nob_temp_sprintf("%s/nob.c", gHere), nob_temp_sprintf("%s/nob.h", gHere),
                           NULL);

    // The options: --win and --from-source, in any order; anything else is a usage error
    bool fromSource = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--win") == 0) {
            gWin = true;
        } else if (strcmp(argv[i], "--from-source") == 0) {
            fromSource = true;
        } else {
            SayErr("usage: ./build.sh [--win] [--from-source]");
            return 2;
        }
    }

    // R3D with its vendored raylib and assimp, as static libraries (the CMake options of a source build).
    // MS_PATCHED_R3D (below) tells the scene runtime that this is the patched R3D (r3d/patches.md).
    Nob_Cmd cmakeArgs = {0};
    nob_cmd_append(&cmakeArgs, "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=OFF", "-DR3D_RAYLIB_VENDORED=ON",
                   "-DR3D_ASSIMP_VENDORED=ON", "-DASSIMP_BUILD_ZLIB=ON", "-DR3D_BUILD_EXAMPLES=OFF",
                   "-DR3D_WARNINGS_AS_ERRORS=OFF");

    // The public R3D headers and raylib's (the same files raylib installs), and the two sources
    const char *r3d = nob_temp_sprintf("%s/r3d", gHere);
    nob_cmd_append(&gCflags, "-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-missing-field-initializers",
                   "-DMS_PATCHED_R3D", nob_temp_sprintf("-I%s", gHere), nob_temp_sprintf("-I%s/include", r3d),
                   nob_temp_sprintf("-I%s/external/raylib/src", r3d));
    nob_cmd_append(&gSources, nob_temp_sprintf("%s/viewer.c", gHere), nob_temp_sprintf("%s/monolith_scene.c", gHere));

    // Per target: the output folder, the program, where a source build of the libraries goes and its extra
    // CMake options
    const char *target, *out, *sourceLibs;
    if (gWin) {
        target = "win";
        out = nob_temp_sprintf("%s/build/win", gHere);
        gExe = nob_temp_sprintf("%s/viewer.exe", out);
        sourceLibs = nob_temp_sprintf("%s/build/r3d-mingw", gHere);
        nob_cmd_append(&cmakeArgs, nob_temp_sprintf("-DCMAKE_TOOLCHAIN_FILE=%s/cmake/mingw-w64-x86_64.cmake", gHere));
    } else {
        target = "linux";
        out = nob_temp_sprintf("%s/build/linux", gHere);
        gExe = nob_temp_sprintf("%s/viewer", out);
        sourceLibs = nob_temp_sprintf("%s/build/r3d-linux", gHere);
        // ASSIMP_BUILD_MINIZIP: assimp's bundled unzip (a system minizip can be found whose headers aren't
        // where assimp looks)
        nob_cmd_append(&cmakeArgs, "-DASSIMP_BUILD_MINIZIP=ON");
    }
    if (!MkdirP(out)) return 1;

    // The prebuilt libraries first, when this folder has them for the target and --from-source wasn't given;
    // a failed link falls back to building them from r3d/
    const char *prebuilt = nob_temp_sprintf("%s/prebuilt/%s/lib", gHere, target);
    bool linked = false;
    if (!fromSource && IsFile(nob_temp_sprintf("%s/libr3d.a", prebuilt))) {
        Say("linking against the prebuilt libraries (prebuilt/%s/lib) ...", target);
        linked = Link(prebuilt);
        if (!linked) SayErr("linking against the prebuilt libraries failed (see above): building them from r3d/ instead");
    }
    if (!linked) {
        BuildR3D(sourceLibs, &cmakeArgs);
        if (!Link(nob_temp_sprintf("%s/lib", sourceLibs))) return 1;
    }

    // The map package next to the program (cp -R <map>/. <out>/<map>/), so the output folder runs anywhere
    const char *pkgOut = nob_temp_sprintf("%s/%s", out, MAP_NAME);
    if (!MkdirP(pkgOut) || !CopyInto(nob_temp_sprintf("%s/%s", gHere, MAP_NAME), pkgOut)) return 1;
    Say("built: %s (run it; the map package is next to it)", Relative(gExe));
    return 0;
}
