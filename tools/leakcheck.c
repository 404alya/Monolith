// tools/leakcheck.c: runs monolith's main code paths under LeakSanitizer (or ASan + UBSan) and exits 1 on any
// leak of ours. Built by `./build.sh --tools leakcheck` into build/tools/leakcheck; it replaced
// tools/leakcheck.sh (2026-11-13) with the same runs, arguments, environment, expected exit codes, output
// lines and exit codes.
//
//   build/tools/leakcheck              build the LSan binaries if a source is newer, run everything
//   build/tools/leakcheck --no-build   use the existing build/linux-lsan + examples/build/linux-lsan (and the
//                                      sanitizer builds of the tools made by an earlier run)
//   build/tools/leakcheck --bench      also a short headless bench (--bench --headless --bench-frames 10
//                                      --bench-window 4): only when no other benchmark runs (one GPU)
//   build/tools/leakcheck --asan       the same paths with the ASan + UBSan build (./build.sh debug,
//                                      build/linux-debug): use-after-free, overflows, UB; leaks too
// The flags combine; anything else prints the usage line and exits 2.
//
// Standing rule (CLAUDE.md): after any change to code (C, shaders, build), this must report zero leaks before
// the change counts as done. Map-only edits don't need it.
//
// Sections, in the order they run (each is marked "Section" in main() below):
//   1. Build (incremental): monolith's sanitizer build (./build.sh lsan or debug) when a source is newer than
//      the binary, the examples' (./build.sh examples lsan, LSan mode only), the profiling tool
//      (tools/profile.c), run-sh.o and every program of build/tools/ with the same sanitizer into the build
//      folder, the paint unwrap test against the build's own xatlas object, and the real
//      build/tools/stub-terminal (./build.sh --tools stub-terminal) when it is missing.
//   2. Output folder: build/leakcheck/<YYYYmmddHHMMSS>/ with logs/ (kept; build/ is not committed) and tmp/
//      (shots, exports, scratch copies: trashed at the end); build/leakcheck/latest points at it.
//   3. Each run (Run below): one command with the sanitizer's options in its environment, its stdout and
//      stderr in logs/<name>.out, its sanitizer report in logs/<name>.<pid>, "<name> <status>" appended to
//      logs/runs.txt; a run whose exit status is not the expected one fails the check.
//   4. Clean-up: the files the runs created (tmp/, the new map packages in maps/) go to the trash (gio trash),
//      never rm.
//   5. Report: tools/lsan_report.py (Python, kept) classifies the reports with tools/lsan.supp and prints one
//      line per run and the stack of every leak of ours; then the final line.
//
// Every run uses a hidden window (--shot, --dump-*, ...) or none: nothing appears on screen.
// What it runs: --dump-boxes, --dump-scene (with and without models, and -diff), --shot of the default map
// (two views, camera-fitted shadow and fog cache variants; every quality preset, settings files, live settings
// changes incl. the renderer restart, the settings panel), of tests/models (models, textured / shader
// materials, shadowed lights), of examples/01_minimal/scene and maps/pine_forest (textured ground, many
// restyled models), --reload-test both ways and into a broken map, a broken map on the command line, failing
// loads with long messages (a package with many .map files: on the command line and as a reload),
// --bake-surface (and a data_sampler reading its tile), --shot-hud with --select / --region / --volume, the
// render inspector (outline modes, labels, its panel, buffer views, the draw filter with a live edit, a
// preview and a reload), manual edits (--apply-edits on a copy of tests/models: move, turn, split, duplicate,
// add, id, undo, redo and a failing command; `id` / `id remove` with a failing one; --dump-entities; the
// edit-mode HUD), the id panel (--id-panel unfiltered, filtered, and with live id edits and the number field),
// the draw panel (--draw-panel whole, one section, clicks on its widgets through --input-script, a bad
// section), painting (--paint-script into the paint layer: every pen, first strokes on plain, textured,
// shader, turned and instanced boxes and model slots (a Kenney model unwrapped), a stroke onto a second face,
// face groups, resize, pixel look, clear, undo / redo, the eyedropper (paint and G-buffer albedo), save, a
// rebase onto an atlas changed on disk; --apply-edits with a paint script and with paint-add / set / remove;
// the draw mode's HUD and strokes through the input shim, a click that unwraps a model), live edits
// (--edit-script with --shot and --dump-entities: moves, undo / redo, duplicate, add, a preview, save, and a
// rebase after `disk` with a failing step; with the fog cache and the AO volume; refused without --shot / a
// dump), the transform ops (turn with pitch / roll / local / pivot, scale, size: --apply-edits with a failing
// command, --edit-script with turn / scale previews) and the transform modal (--edit-modal: open with the HUD,
// committed, cancelled; --input-script: mouse and keys through the input shim, a no-op turn), the rotate
// gizmo, the R step and reset, turned boxes (shots with the caches, a reload, live edits), the model picker
// (--model-picker: thumbnails of the scene's files and of one loaded for the render, a filter, freed and
// rendered again across a reload and a renderer restart, ENTER and ESC), --submit (build/tools/stub-terminal
// instead of a terminal, MONOLITH_WORKSPACE=) on both maps, in coordinator mode (queue + coordinator start, a
// cancel with no coordinator) and per-request mode, with the AI classifier (MONOLITH_CLASSIFIER = the sanitizer
// copy of build/tools/stub-classifier for every run, never a real `claude -p`: an answer, a quit and a cancel
// while classifying, a failure), --export, --compile and compiled maps (dumps, a shot with a reload, a damaged
// file, a stale one and its refused export), --inspect-model, --new-map, --classify (an answer, garbage, a
// timeout, off, an override), and every example's --shot (the game-style ones load compiled maps), the
// profiling tool (tools/profile.c) on the fixtures in tests/profile/ (--report-only, no perf run),
// the paint unwrap test (tests/unit/paintunwrap_test.c: xatlas on synthetic meshes and every model file in
// maps/*/models/), run-sh.o (tools/run-sh.c, built with the sanitizer into the build folder: --check on
// allowed, refused and unterminated lines, -c, --hook on Bash / Monitor / bad JSON) and its unit test
// (./build.sh test run_sh: ASan + UBSan, with its own sanitizer build of run-sh.c on every case), and the other
// programs of build/tools/ (each built with the sanitizer into <build folder>/tools/), on harmless inputs
// only: stub-terminal with a log, request-queue on a scratch requests folder (usage, list, wait, a bad id),
// todo / ai-todo `list` and refused arguments in a scratch copy of the repo root (their lists are copies; only
// where submodules/simple-todo is checked out),
// stub-coordinator and test-coordinator in that scratch root (they stop at their first check: no queue
// program, no monolith), bench-export-ab --summarize on hand-made reports, old-monolith with bad arguments and as
// its rm stand-in. Never a real benchmark (beside --bench), never a real old build, never a real coordinator.
// Bundles go to its own requests folder (MONOLITH_REQUESTS_DIR=<tmp>/requests), never to requests/: a
// coordinator running meanwhile never sees them, and bundles queued meanwhile are left alone.
//
// Leaks are classified by tools/lsan_report.py with tools/lsan.supp: only memory allocated by GPU driver /
// system library code is ignored (see the comment at the top of lsan.supp); every leak allocated by our code,
// raylib, R3D or assimp counts. Logs: build/leakcheck/latest/logs/.
//
// Environment:
//   CC      the compiler of the sanitizer builds made here (profile, run-sh.o, the tools, the paint unwrap
//           test); default clang. ./build.sh reads it too.
//   R3D_DIR another R3D tree (default submodules/r3d): its sources count for "a source is newer", and the
//           paint unwrap test links its libraries from $R3D_DIR/build-linux.
//   Set for the runs (and put back after each): LSAN_OPTIONS (LSan mode), or ASAN_OPTIONS, UBSAN_OPTIONS and
//   LSAN_OPTIONS (--asan); RUN_SH_DIR for run-sh.o's runs (unset afterwards, as the script did).
//
// Exit codes: 0 zero leaks of ours and every run exited as expected ("leakcheck: OK: zero leaks of ours");
// 1 a leak of ours, an unexpected exit status or a sanitizer error ("leakcheck: FAILED: ..."), or a build
// failed / a binary is missing; 2 a bad argument.
//
// Called by: every session after a code change (CLAUDE.md, the leak-check skill), and the developer.

#include "tool.h"

#include <fnmatch.h>
#include <ftw.h>
#include <glob.h>
#include <locale.h>

// #region State

// The mode: "lsan" (the LeakSanitizer build) or "debug" (--asan: the ASan + UBSan build). It is also the
// argument of ./build.sh that builds the binary, and the word in "building (<mode>) ...".
static const char *gMode = "lsan";
// The logs folder (build/leakcheck/<stamp>/logs) and the scratch folder (build/leakcheck/<stamp>/tmp).
static const char *gLog;
static const char *gTmp;
// Set when a run exited with a status it shouldn't have, or hit an ASan / UBSan error.
static bool gFailed = false;

// The sanitizer options every run shares (LeakSanitizer reads them; ASan's leak check too):
//   exitcode=0                 the program's own exit status is kept (the leaks are judged from the report)
//   fast_unwind_on_malloc=0    R3D / raylib / the driver have no frame pointers, so unwind with DWARF to get
//                              the whole stack
//   malloc_context_size=40     enough frames to reach our code from deep inside a library
// No suppressions here: lsan_report.py classifies by the allocating frame.
#define SAN_COMMON "exitcode=0:fast_unwind_on_malloc=0:malloc_context_size=40"

// "<tmp>/<name>": a path in the scratch folder (a temp string).
static const char *T(const char *name)
{
    return nob_temp_sprintf("%s/%s", gTmp, name);
}

// #endregion

// #region Files (the script's printf > file, cat >> file, cp -r, grep -v, head)

// `printf '...' > path` (append = false) or `>> path` (append = true): writes text as it is. Prints why and
// returns false when the file can't be written (the script's redirection failed and the run after it failed
// in turn, which the check then reports).
static bool WriteText(const char *path, const char *text, bool append)
{
    FILE *f = fopen(path, append ? "ab" : "wb");
    if (!f) {
        ToolErr("%s: %s", path, strerror(errno));
        return false;
    }
    fputs(text, f);
    fclose(f);
    return true;
}

// `cat src >> dst` (append) or `cat src > dst`: the whole file. False (with the reason) when src can't be read.
static bool CatInto(const char *src, const char *dst, bool append)
{
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(src, &sb)) {
        nob_sb_free(sb);
        return false;
    }
    FILE *f = fopen(dst, append ? "ab" : "wb");
    if (!f) {
        ToolErr("%s: %s", dst, strerror(errno));
        nob_sb_free(sb);
        return false;
    }
    fwrite(sb.items, 1, sb.count, f);
    fclose(f);
    nob_sb_free(sb);
    return true;
}

// `head -c bytes src > dst`: the first `bytes` bytes of src (all of it when shorter).
static bool HeadBytes(const char *src, const char *dst, size_t bytes)
{
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(src, &sb)) {
        nob_sb_free(sb);
        return false;
    }
    bool ok = nob_write_entire_file(dst, sb.items, sb.count < bytes ? sb.count : bytes);
    nob_sb_free(sb);
    return ok;
}

// `head -n lines src >> dst`: the first `lines` lines of src (each with its newline), appended.
static bool HeadLinesAppend(const char *src, const char *dst, size_t lines)
{
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(src, &sb)) {
        nob_sb_free(sb);
        return false;
    }
    size_t end = 0, seen = 0;
    while (end < sb.count && seen < lines) {
        if (sb.items[end++] == '\n') seen++;
    }
    FILE *f = fopen(dst, "ab");
    if (!f) {
        ToolErr("%s: %s", dst, strerror(errno));
        nob_sb_free(sb);
        return false;
    }
    fwrite(sb.items, 1, end, f);
    fclose(f);
    nob_sb_free(sb);
    return true;
}

// `grep -v '^<prefix>' src > dst`: every line of src that doesn't start with `prefix` (a fixed string: the
// script's pattern had no regex characters besides ^ and the dots of ".glb", which match themselves there).
static bool GrepOutPrefix(const char *src, const char *dst, const char *prefix)
{
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(src, &sb)) {
        nob_sb_free(sb);
        return false;
    }
    FILE *f = fopen(dst, "wb");
    if (!f) {
        ToolErr("%s: %s", dst, strerror(errno));
        nob_sb_free(sb);
        return false;
    }
    size_t n = strlen(prefix);
    for (size_t i = 0; i < sb.count;) {
        size_t end = i;
        while (end < sb.count && sb.items[end] != '\n') end++;
        bool hasNl = end < sb.count;
        if (!(end - i >= n && memcmp(sb.items + i, prefix, n) == 0)) {
            fwrite(sb.items + i, 1, end - i, f);
            // grep ends every line it prints with a newline, also a last line that had none
            fputc('\n', f);
        }
        i = end + (hasNl ? 1 : 0);
    }
    fclose(f);
    nob_sb_free(sb);
    return true;
}

// `cp -r src dst` (dst not there yet: it becomes the copy) and `cp -r src/. dst/` (dst there: the contents
// go into it): nob's recursive copy does both. Its own error messages say what failed.
static bool CopyTree(const char *src, const char *dst)
{
    return nob_copy_directory_recursively(src, dst);
}

// `mkdir -p path`, quietly for the callers that ignore a failure (the next run then fails and says so).
static void MkdirP(const char *path)
{
    (void)ToolMkdirP(path);
}

// #endregion

// #region "Is a source newer than the binary?" (the script's find ... -newer ... -print -quit)

// The walk's state (nftw passes no user pointer): the reference binary, whether only source-like names count
// and paths under a build/ folder are skipped, and the answer.
static const char *gWalkRef;
static bool gWalkFiltered;
static bool gWalkFound;

// One entry of the walk: newer than the reference (modification time, nanoseconds, like find -newer), and,
// for the filtered walk, a source-like name (*.c *.cpp *.h *.sh *.glsl *.vert *.frag) not under a build/
// folder (find's -not -path '*/build/*'). Stops the walk at the first hit (find's -print -quit).
static int WalkEntry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)type;
    if (gWalkFiltered) {
        static const char *patterns[] = {"*.c", "*.cpp", "*.h", "*.sh", "*.glsl", "*.vert", "*.frag"};
        const char *base = path + ftw->base;
        bool named = false;
        for (size_t i = 0; i < NOB_ARRAY_LEN(patterns) && !named; i++) named = fnmatch(patterns[i], base, 0) == 0;
        if (!named || strstr(path, "/build/")) return FTW_CONTINUE;
    }
    struct stat ref;
    if (stat(gWalkRef, &ref) != 0) {
        gWalkFound = true;
        return FTW_STOP;
    }
    bool newer = st->st_mtim.tv_sec != ref.st_mtim.tv_sec ? st->st_mtim.tv_sec > ref.st_mtim.tv_sec
                                                          : st->st_mtim.tv_nsec > ref.st_mtim.tv_nsec;
    if (newer) {
        gWalkFound = true;
        return FTW_STOP;
    }
    return FTW_CONTINUE;
}

// Whether anything under the `starts` (folders walked recursively, files tested themselves; missing ones are
// skipped, as find's errors went to /dev/null) is newer than `ref`. `filtered`: only source-like names, not
// under build/ (see WalkEntry). Symlinks are not followed (find's default -P).
static bool AnyNewer(const char *ref, const char **starts, size_t count, bool filtered)
{
    gWalkRef = ref;
    gWalkFiltered = filtered;
    gWalkFound = false;
    for (size_t i = 0; i < count && !gWalkFound; i++) {
        struct stat st;
        if (lstat(starts[i], &st) != 0) continue;
        nftw(starts[i], WalkEntry, 32, FTW_PHYS | FTW_ACTIONRETVAL);
    }
    return gWalkFound;
}

// The script's Newer(): the binary is missing (or not executable), or a source of monolith or the examples is
// newer than it: src/, runtime/, build.sh, nob.c, examples/, and R3D's src/, include/ and shaders/ (R3D's
// sources count too: build.sh then rebuilds the static library incrementally; so do the build program nob.c
// and its header, src/vendor/nob/nob.h, which is under src/).
static bool SourcesNewer(const char *root, const char *r3d, const char *binary)
{
    if (access(binary, X_OK) != 0) return true;
    const char *starts[] = {
        nob_temp_sprintf("%s/src", root),     nob_temp_sprintf("%s/runtime", root),
        nob_temp_sprintf("%s/build.sh", root), nob_temp_sprintf("%s/nob.c", root),
        nob_temp_sprintf("%s/examples", root), nob_temp_sprintf("%s/src", r3d),
        nob_temp_sprintf("%s/include", r3d),   nob_temp_sprintf("%s/shaders", r3d),
    };
    return AnyNewer(binary, starts, NOB_ARRAY_LEN(starts), true);
}

// #endregion

// #region Building with a sanitizer

// The compile of one small C program (the profiling tool, run-sh.o, a program of build/tools/) with the
// build's sanitizer: the flags of nob.c's ToolCommand plus -g, frame pointers and -fsanitize=<san> (leak in
// LSan mode, address,undefined with --asan). Its messages go to our stdout / stderr. False when it failed.
static bool CompileSanitized(const char *source, const char *binary, const char *san)
{
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, ToolEnv("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Wno-missing-field-initializers",
                   "-g", "-O1", "-fno-omit-frame-pointer", nob_temp_sprintf("-fsanitize=%s", san), source, "-o",
                   binary);
    int status = ToolRun(&cmd, (ToolIo){0});
    nob_cmd_free(cmd);
    return status == 0;
}

// The programs of build/tools/ that get a sanitizer copy in <build folder>/tools/ (run-sh.o is built beside the
// monolith binary as before, leakcheck itself is the one running). The real ones in build/tools/ are never touched:
// ToolRebuildSelf only rebuilds a binary at its own place, so these copies stay sanitizer builds.
static const char *SAN_TOOLS[] = {
    "stub-terminal", "request-queue", "todo", "ai-todo", "stub-coordinator", "test-coordinator",
    "bench-export-ab", "old-monolith", "stub-classifier",
};

// #endregion

// #region Running one command (the script's Run)

// One environment variable to set for a run and its value before, so it can be put back afterwards.
typedef struct {
    const char *name;
    char *old;     // heap copy of the value before, or NULL when it wasn't set
} SavedEnv;

// setenv(name, value), remembering the old value in *saved.
static void SetFor(SavedEnv *saved, const char *name, const char *value)
{
    const char *old = getenv(name);
    saved->name = name;
    saved->old = old ? strdup(old) : NULL;
    setenv(name, value, 1);
}

// Puts a variable back as it was before SetFor.
static void Restore(SavedEnv *saved)
{
    if (saved->old) setenv(saved->name, saved->old, 1);
    else unsetenv(saved->name);
    free(saved->old);
    saved->old = NULL;
}

// Whether any file logs/<name>.* (the run's output and its sanitizer reports) holds "ERROR: AddressSanitizer"
// or "runtime error:" (the script's grep -lqE over "$LOG/$name".*): ASan / UBSan errors that aren't leaks.
static bool HasSanitizerError(const char *name)
{
    Nob_File_Paths files = {0};
    bool found = false;
    if (nob_read_entire_dir(gLog, &files)) {
        size_t n = strlen(name);
        for (size_t i = 0; i < files.count && !found; i++) {
            const char *f = files.items[i];
            if (strncmp(f, name, n) != 0 || f[n] != '.') continue;
            Nob_String_Builder sb = {0};
            if (nob_read_entire_file(nob_temp_sprintf("%s/%s", gLog, f), &sb)) {
                nob_sb_append_null(&sb);
                found = strstr(sb.items, "ERROR: AddressSanitizer") || strstr(sb.items, "runtime error:");
            }
            nob_sb_free(sb);
        }
    }
    nob_da_free(files);
    return found;
}

// Run <name> <expected exit status> <command>: runs the command with the sanitizer's options (log_path =
// logs/<name>, so its reports land in logs/<name>.<pid>), stdin from `in` (NULL: ours), stdout and stderr into
// logs/<name>.out; appends "<name> <status>" to logs/runs.txt (lsan_report.py lists the runs from it); a status
// other than `want`, or (--asan) an ASan / UBSan error in its logs, fails the check with a message on stderr.
// The command is freed.
static void RunIn(const char *name, int want, const char *in, Nob_Cmd *cmd)
{
    const char *logPath = nob_temp_sprintf("%s/%s", gLog, name);
    SavedEnv saved[3] = {0};
    size_t nSaved = 0;
    if (strcmp(gMode, "lsan") == 0) {
        SetFor(&saved[nSaved++], "LSAN_OPTIONS", nob_temp_sprintf("%s:log_path=%s", SAN_COMMON, logPath));
    } else {
        // ASan mode: ASan's leak check on, the same options, and UBSan's reports with stacks into the same
        // log files (ASan stops at its first error by default)
        SetFor(&saved[nSaved++], "ASAN_OPTIONS",
               nob_temp_sprintf("detect_leaks=1:%s:log_path=%s", SAN_COMMON, logPath));
        SetFor(&saved[nSaved++], "UBSAN_OPTIONS", nob_temp_sprintf("print_stacktrace=1:log_path=%s", logPath));
        SetFor(&saved[nSaved++], "LSAN_OPTIONS", SAN_COMMON);
    }
    int status = ToolRun(cmd, (ToolIo){.in = in, .out = nob_temp_sprintf("%s.out", logPath), .errToOut = true});
    for (size_t i = nSaved; i-- > 0;) Restore(&saved[i]);
    free(cmd->items);
    *cmd = (Nob_Cmd){0};

    WriteText(nob_temp_sprintf("%s/runs.txt", gLog), nob_temp_sprintf("%s %d\n", name, status), true);
    if (status != want) {
        ToolErr("%s exited with %d (expected %d); see %s.out", name, status, want, logPath);
        gFailed = true;
    }
    if (strcmp(gMode, "debug") == 0 && HasSanitizerError(name)) {
        ToolErr("%s: AddressSanitizer / UBSan error, see %s.*", name, logPath);
        gFailed = true;
    }
}

// RUN(name, want, program, args...): one run with our stdin (the script's `Run name want cmd...`).
#define RUN(name, want, ...)                     \
    do {                                         \
        Nob_Cmd run_ = {0};                      \
        nob_cmd_append(&run_, __VA_ARGS__);      \
        RunIn((name), (want), NULL, &run_);      \
    } while (0)

// RUN_STDIN(name, want, file, program, args...): the same with stdin from a file (the script's `<<< text`,
// written to that file first).
#define RUN_STDIN(name, want, in, ...)           \
    do {                                         \
        Nob_Cmd run_ = {0};                      \
        nob_cmd_append(&run_, __VA_ARGS__);      \
        RunIn((name), (want), (in), &run_);      \
    } while (0)

// #endregion

// #region Clean-up and usage

// Moves what the runs created to the trash (never rm); a path that isn't there is skipped (the script's
// [[ -e ]] follows symlinks: a dangling one counts as not there). gio's own error text is dropped and ours
// printed instead.
static void Trash(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (ToolTrash(path, true) != 0) ToolErr("could not move %s to the trash; remove it by hand", path);
}

// The usage line on stderr, exit 2.
static void Usage(void)
{
    ToolDie(2, "usage: build/tools/leakcheck [--no-build] [--asan] [--bench]");
}

// #endregion

int main(int argc, char **argv)
{
    // The name for messages ("leakcheck: ..."), the repo root from this program's path, and a rebuild of this
    // program first when tools/leakcheck.c, tools/tool.h or nob.h changed.
    ToolInit(argv, "leakcheck", "leakcheck");
    const char *root = gToolRoot;
    // cd "$ROOT": the runs name maps and tests relative to the repo root, as the script's did
    if (chdir(root) != 0) ToolDie(1, "cd %s: %s", root, strerror(errno));
    // glob order as bash's (maps/*/models/*.glb): the environment's collation
    setlocale(LC_COLLATE, "");

    // The flags, in any order and combination
    bool build = true, bench = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-build") == 0) build = false;
        else if (strcmp(argv[i], "--asan") == 0) gMode = "debug";
        else if (strcmp(argv[i], "--bench") == 0) bench = true;
        else Usage();
    }
    bool lsan = strcmp(gMode, "lsan") == 0;
    // The sanitizer build's folder (./build.sh lsan / debug) and, in LSan mode, the examples' (./build.sh
    // examples lsan); --asan runs no examples (they have no ASan build)
    const char *binDir = strdup(nob_temp_sprintf("%s/build/%s", root, lsan ? "linux-lsan" : "linux-debug"));
    const char *exDir = strdup(nob_temp_sprintf("%s/examples/build/linux-lsan", root));
    const char *M = strdup(nob_temp_sprintf("%s/monolith", binDir));
    const char *san = lsan ? "leak" : "address,undefined";
    const char *r3d = strdup(ToolEnv("R3D_DIR", nob_temp_sprintf("%s/submodules/r3d", root)));
    const char *toolDir = strdup(nob_temp_sprintf("%s/tools", binDir));

    // ---------------------------------------------------------------------------------------------------------
    // Section 1: build (incremental: only when a source is newer than the binary)
    if (build) {
        // monolith's sanitizer build; its progress output is dropped, its errors stay on stderr
        if (SourcesNewer(root, r3d, M)) {
            ToolSay("building (%s) ...", gMode);
            Nob_Cmd cmd = {0};
            nob_cmd_append(&cmd, "./build.sh", gMode);
            int status = ToolRun(&cmd, (ToolIo){.out = "/dev/null"});
            nob_cmd_free(cmd);
            if (status != 0) ToolDie(1, "./build.sh %s failed", gMode);
        }
        // the examples with LSan (examples/build/linux-lsan/), when an example's or the runtime's source changed
        if (lsan && SourcesNewer(root, r3d, nob_temp_sprintf("%s/01_minimal/minimal", exDir))) {
            Nob_Cmd cmd = {0};
            nob_cmd_append(&cmd, "./build.sh", "examples", "lsan");
            int status = ToolRun(&cmd, (ToolIo){.out = "/dev/null"});
            nob_cmd_free(cmd);
            if (status != 0) ToolDie(1, "./build.sh examples lsan failed");
        }
        // the profiling tool (./build.sh profile builds the real one): plain C, libc only, rebuilt when its
        // source is newer
        const char *profileSrc = nob_temp_sprintf("%s/tools/profile.c", root);
        const char *profileBin = nob_temp_sprintf("%s/profile", binDir);
        if (access(profileBin, X_OK) != 0 || ToolNewer(profileSrc, profileBin)) {
            if (!CompileSanitized(profileSrc, profileBin, san)) ToolDie(1, "building tools/profile.c failed");
        }
        // run-sh.o (tools/run-sh.c; ./build.sh --tools builds the real one) with the sanitizer, built every
        // time (a second or two): cheaper than tracking its dependencies, and the copy can never be stale
        if (!CompileSanitized(nob_temp_sprintf("%s/tools/run-sh.c", root), nob_temp_sprintf("%s/run-sh.o", binDir),
                              san)) {
            ToolDie(1, "building tools/run-sh.c failed");
        }
        // the other programs of build/tools/, the same way, into <build folder>/tools/ (about half a second
        // each). These copies don't rebuild themselves (ToolRebuildSelf: not at their own place).
        if (!ToolMkdirP(toolDir)) return 1;
        for (size_t i = 0; i < NOB_ARRAY_LEN(SAN_TOOLS); i++) {
            const char *src = nob_temp_sprintf("%s/tools/%s.c", root, SAN_TOOLS[i]);
            if (!CompileSanitized(src, nob_temp_sprintf("%s/%s", toolDir, SAN_TOOLS[i]), san)) {
                ToolDie(1, "building tools/%s.c failed", SAN_TOOLS[i]);
            }
        }
        // the paint unwrap test (tests/unit/paintunwrap_test.c) against the build's own xatlas object (the
        // build, nob.c, compiles it into <build folder>-obj/ with the same sanitizer), rebuilt when the test,
        // the unwrap, anything in runtime/ (folders included, as find listed them) or that object is newer
        const char *unwrapBin = nob_temp_sprintf("%s/paintunwrap_test", binDir);
        const char *xatlas = nob_temp_sprintf("%s-obj/xatlas.o", binDir);
        const char *unwrapDeps[] = {
            nob_temp_sprintf("%s/tests/unit/paintunwrap_test.c", root), nob_temp_sprintf("%s/src/paintunwrap.c", root),
            nob_temp_sprintf("%s/src/paintunwrap.h", root), nob_temp_sprintf("%s/runtime", root), xatlas,
        };
        if (access(unwrapBin, X_OK) != 0 || AnyNewer(unwrapBin, unwrapDeps, NOB_ARRAY_LEN(unwrapDeps), false)) {
            const char *libDir = nob_temp_sprintf("%s/build-linux", r3d);
            Nob_Cmd cmd = {0};
            nob_cmd_append(&cmd, ToolEnv("CC", "clang"), "-std=c11", "-Wall", "-Wextra",
                           "-Wno-missing-field-initializers", "-g", "-O1", "-fno-omit-frame-pointer",
                           nob_temp_sprintf("-fsanitize=%s", san), "-DMS_PATCHED_R3D", "-DMONOLITH_EDITOR",
                           nob_temp_sprintf("-I%s/include", r3d),
                           nob_temp_sprintf("-I%s/external/raylib/raylib/include", libDir),
                           nob_temp_sprintf("-I%s/runtime", root), nob_temp_sprintf("-I%s/src", root),
                           nob_temp_sprintf("%s/tests/unit/paintunwrap_test.c", root),
                           nob_temp_sprintf("%s/src/paintunwrap.c", root),
                           nob_temp_sprintf("%s/runtime/monolith_scene.c", root), xatlas, "-o", unwrapBin,
                           nob_temp_sprintf("-L%s/lib", libDir), "-lr3d", "-lraylib", "-lassimp", "-lzlibstatic",
                           "-l:libstdc++.a", "-lgcc_s", "-lGL", "-lm", "-lpthread", "-ldl");
            int status = ToolRun(&cmd, (ToolIo){0});
            nob_cmd_free(cmd);
            if (status != 0) ToolDie(1, "building tests/unit/paintunwrap_test.c failed");
        }
        // the stub terminal the --submit runs start instead of a terminal: the real program
        // (it keeps itself current once built), built when missing
        if (access(nob_temp_sprintf("%s/build/tools/stub-terminal", root), X_OK) != 0) {
            Nob_Cmd cmd = {0};
            nob_cmd_append(&cmd, "./build.sh", "--tools", "stub-terminal");
            int status = ToolRun(&cmd, (ToolIo){.out = "/dev/null"});
            nob_cmd_free(cmd);
            if (status != 0) ToolDie(1, "./build.sh --tools stub-terminal failed");
        }
    }
    // What the runs need, built now or by an earlier run (--no-build)
    if (access(M, X_OK) != 0) ToolDie(1, "%s missing (run without --no-build)", M);
    const char *PROFILE = strdup(nob_temp_sprintf("%s/profile", binDir));
    if (access(PROFILE, X_OK) != 0) ToolDie(1, "%s missing (run without --no-build)", PROFILE);
    const char *RUNSH = strdup(nob_temp_sprintf("%s/run-sh.o", binDir));
    if (access(RUNSH, X_OK) != 0) ToolDie(1, "%s missing (run without --no-build)", RUNSH);
    for (size_t i = 0; i < NOB_ARRAY_LEN(SAN_TOOLS); i++) {
        const char *bin = nob_temp_sprintf("%s/%s", toolDir, SAN_TOOLS[i]);
        if (access(bin, X_OK) != 0) ToolDie(1, "%s missing (run without --no-build)", bin);
    }
    const char *STUB = strdup(nob_temp_sprintf("%s/build/tools/stub-terminal", root));
    if (access(STUB, X_OK) != 0) {
        ToolDie(1, "build/tools/stub-terminal missing (run without --no-build, or ./build.sh --tools stub-terminal)");
    }

    // ---------------------------------------------------------------------------------------------------------
    // Section 2: the output folder build/leakcheck/<stamp>/ (logs stay; build/ is not committed), `latest`
    // pointing at it; tmp/ (shots, exports, copies) goes to the trash at the end
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%Y%m%d%H%M%S", &tm);
    const char *outDir = strdup(nob_temp_sprintf("%s/build/leakcheck/%s", root, stamp));
    gLog = strdup(nob_temp_sprintf("%s/logs", outDir));
    gTmp = strdup(nob_temp_sprintf("%s/tmp", outDir));
    if (!ToolMkdirP(gLog) || !ToolMkdirP(gTmp)) return 1;
    // ln -sfn <stamp> build/leakcheck/latest: a new link under a temporary name renamed over the old one (the
    // swap is atomic and needs no delete)
    {
        const char *latest = nob_temp_sprintf("%s/build/leakcheck/latest", root);
        const char *tmpLink = nob_temp_sprintf("%s.tmp.%ld", latest, (long)getpid());
        if (symlink(stamp, tmpLink) != 0 || rename(tmpLink, latest) != 0) {
            ToolErr("could not point build/leakcheck/latest at %s: %s", stamp, strerror(errno));
        }
    }
    // runs.txt starts empty: one "<name> <status>" line per run
    WriteText(nob_temp_sprintf("%s/runs.txt", gLog), "", false);

    // ---------------------------------------------------------------------------------------------------------
    // Section 3: the runs
    const char *REQ = strdup(T("requests"));   // the bundles' folder (inside tmp/: trashed with it)
    MkdirP(REQ);
    // Every run's AI classifier is the sanitizer copy of the stub (never a real `claude -p`; its own leaks are
    // reported with the run that started it), logging into tmp/; the developer's classifier settings stay out
    setenv("MONOLITH_CLASSIFIER", nob_temp_sprintf("%s/stub-classifier", toolDir), 1);
    setenv("MONOLITH_STUB_CLASSIFIER_LOG", T("stub-classifier.log"), 1);
    unsetenv("MONOLITH_STUB_CLASSIFIER");
    unsetenv("MONOLITH_CLASSIFIER_MODEL");
    unsetenv("MONOLITH_CLASSIFIER_FALLBACK");
    unsetenv("MONOLITH_CLASSIFIER_TIMEOUT");

    ToolSay("running monolith (%s build) ...", gMode);
    RUN("dump-boxes", 0, M, "--dump-boxes", T("boxes.txt"));
    RUN("dump-scene", 0, M, "--dump-scene", T("scene.txt"));
    RUN("dump-scene-models", 0, M, "--dump-scene", T("scene-models.txt"), "--map", "tests/models");
    RUN("dump-scene-diff", 0, M, "--dump-scene-diff", T("scene-diff.txt"), "--map", "tests/models");
    RUN("shot-view2", 0, M, "--shot", T("v2.png"), "--view", "2");
    RUN("shot-view5-camera", 0, M, "--shot", T("v5.png"), "--view", "5", "--shadow-camera", "--fog-cache", "1", "2");
    RUN("shot-models", 0, M, "--shot", T("models.png"), "--view", "1", "--map", "tests/models");
    RUN("shot-models-close", 0, M, "--shot", T("models2.png"), "--view", "2", "--map", "tests/models",
        "--shadow-always", "--no-prepass");
    RUN("shot-courtyard", 0, M, "--shot", T("courtyard.png"), "--view", "1", "--map", "examples/01_minimal/scene");
    // (maps/pine_forest is a separate package: shot only when it is there)
    {
        struct stat st;
        if (stat("maps/pine_forest", &st) == 0 && S_ISDIR(st.st_mode)) {
            RUN("shot-pine-forest", 0, M, "--shot", T("pine.png"), "--view", "1", "--map", "maps/pine_forest");
        }
    }
    RUN("shot-render-scale", 0, M, "--shot", T("scale.png"), "--view", "2", "--render-scale", "0.5", "--upscale",
        "lanczos");
    RUN("shot-ao-baked", 0, M, "--shot", T("aobaked.png"), "--view", "1", "--ao", "baked");
    RUN("shot-ao-volume", 0, M, "--shot", T("aovol.png"), "--view", "2", "--map", "tests/models", "--ao-volume", "0.5",
        "0.6", "--render-scale", "0.75");
    RUN("reload-ao-volume", 0, M, "--shot", T("aovol2.png"), "--view", "1", "--ao", "baked", "--reload-test",
        "tests/models");

    // Graphics settings (settings.c, settingspanel.c, MS_Quality): every preset, a settings file written / read /
    // rejected, live changes incl. the renderer restart (shadow map size), the panel
    static const char *QUALITIES[] = {"low", "medium", "high", "ultra"};
    for (size_t i = 0; i < NOB_ARRAY_LEN(QUALITIES); i++) {
        const char *q = QUALITIES[i];
        RUN(nob_temp_sprintf("shot-quality-%s", q), 0, M, "--shot", T(nob_temp_sprintf("q-%s.png", q)), "--view", "1",
            "--quality", q);
    }
    RUN("settings-write", 0, M, "--settings", T("settings.ini"), "--settings-set", "preset=medium", "--settings-set",
        "bloom=off", "--settings-set", "fps_cap=monitor", "--dump-boxes", T("sb.txt"));
    RUN("settings-read", 0, M, "--shot", T("sr.png"), "--view", "2", "--settings", T("settings.ini"), "--quality",
        "low", "--render-scale", "0.6");
    WriteText(T("bad-settings.ini"), "preset = high\nbogus = 1\nrender_scale = abc\n", false);
    RUN("settings-bad-lines", 0, M, "--shot", T("sbad.png"), "--view", "2", "--settings", T("bad-settings.ini"));
    RUN("settings-bad-flag", 1, M, "--dump-boxes", T("sb2.txt"), "--settings-set", "nonsense=1");
    RUN("settings-restart", 0, M, "--shot", T("srs.png"), "--view", "2", "--quality", "low", "--settings-change",
        "preset=ultra");
    RUN("settings-restart-models", 0, M, "--shot", T("srm.png"), "--view", "1", "--map", "tests/models",
        "--settings-change", "shadow_size=4096", "--settings-change", "ao=baked", "--settings-change", "ssr=on",
        "--settings-change", "ssgi=on");
    RUN("settings-panel", 0, M, "--shot", T("panel.png"), "--view", "2", "--shot-hud", "--settings-panel", "--quality",
        "high");
    // --bench: a 10-frame headless bench, only when asked (one benchmark at a time on the machine)
    if (bench) {
        RUN("bench-window", 0, M, "--bench", "--headless", "--bench-frames", "10", "--bench-window", "4",
            "--no-profile", "--note", "leakcheck (not a measurement)");
    }
    RUN("reload-to-models", 0, M, "--shot", T("r1.png"), "--view", "2", "--reload-test", "tests/models");
    RUN("reload-to-default", 0, M, "--shot", T("r2.png"), "--view", "1", "--map", "tests/models", "--reload-test",
        "maps/monolith", "--frame-reuse");
    WriteText(T("broken.map"), "monolith 3\nbox 0 0 0 1 1 1\nbogus statement here\n", false);
    RUN("reload-to-broken", 0, M, "--shot", T("r3.png"), "--view", "1", "--map", "tests/models", "--reload-test",
        T("broken.map"));
    RUN("broken-map", 1, M, "--dump-boxes", T("broken.txt"), "--map", T("broken.map"));

    // A failing load whose message grows past any fixed size (the runtime's last error, MS_LastError): a package
    // folder with 60 long-named .map files and no <name>.map lists every one of them
    MkdirP(T("several"));
    for (int i = 1; i <= 60; i++) {
        WriteText(T(nob_temp_sprintf("several/candidate-%d-with-a-long-name-so-the-list-outgrows-old-buffers.map", i)),
                  "monolith 3\nbox 0 0 0 1 1 1\n", false);
    }
    RUN("several-maps", 1, M, "--dump-boxes", T("several.txt"), "--map", T("several"));
    RUN("reload-to-several", 0, M, "--shot", T("r4.png"), "--view", "1", "--map", "tests/models", "--reload-test",
        T("several"));

    // --bake-surface (bake.c): a shader with an MS_BAKE path, 3 levels into an atlas, then a map that reads the
    // tile through a data_sampler; and a material that doesn't exist (exit 1)
    MkdirP(T("bake/shaders"));
    WriteText(T("bake/shaders/s.glsl"),
              "#pragma usage opaque\nvoid fragment()\n{\n#ifdef MS_BAKE\n"
              "    ALBEDO = vec3(fract(POSITION.xy), float(MS_BAKE_LEVEL) / 8.0);\n#else\n    ALBEDO = vec3(0.5);\n"
              "#endif\n    ROUGHNESS = 0.5;\n}\n",
              false);
    WriteText(T("bake/bake.map"), "monolith 3\nmaterial \"s\" shader \"shaders/s.glsl\" alpha_cutoff 0\nbox 0 0 0 1 1 1\n",
              false);
    RUN("bake-surface", 0, M, "--map", T("bake"), "--bake-surface", "s", T("bake/tile"), "--bake-size", "64",
        "--bake-levels", "3", "--bake-face", "side");
    WriteText(T("bake/shaders/t.glsl"),
              "#pragma usage opaque\nuniform sampler2D u_tile;\nvoid fragment()\n{\n"
              "    ALBEDO = texelFetch(u_tile, ivec2(fract(POSITION.xz) * 64.0), 0).rgb;\n}\n",
              false);
    WriteText(T("bake/bake.map"),
              "material \"t\" shader \"shaders/t.glsl\" data_sampler u_tile \"tile.png\" alpha_cutoff 0\nuse \"t\"\n"
              "box 2 0 0 3 1 1\n",
              true);
    RUN("shot-data-sampler", 0, M, "--shot", T("bake.png"), "--map", T("bake"));
    RUN("bake-no-material", 1, M, "--map", T("bake"), "--bake-surface", "nope", T("bake/x"));

    // Manual edits (mapedit.c) on a copy; the last command fails on purpose (exit 1)
    CopyTree("tests/models", T("edit-models"));
    WriteText(T("edits.txt"),
              "select line 47\nmove 0 1 0\nselect line 36 copy 1\nmove 0.5 0 0\nturn 15\nselect line 32\nturn 90\n"
              "duplicate 0 0 3\nselect line 30 copy 2\nremove\nselect line 26\nadd \"models/tree_oak.glb\" 8 0 8\n"
              "undo\nundo\nredo\nprint\nselect id 1\nid 40\nselect line 26\nalso id 40\nid\nundo\n"
              "select line 9999\nremove\n",
              false);
    RUN("apply-edits", 1, M, "--apply-edits", T("edits.txt"), "--map", T("edit-models"));

    // Live edits (--edit-script: the working copy, MS_EditSetText, the caches redrawn in part, the preview,
    // save, and the rebase after an outside change with a step that no longer applies)
    CopyTree("tests/models", T("live-models"));
    GrepOutPrefix("tests/models/models.map", T("live-changed.map"), "repeat 3 4 0 0 model \"models/tree_oak.glb\"");
    WriteText(T("live-changed.map"), "group \"added on disk\"\nid 1 box 8 0 8 9 1 9\n", true);
    WriteText(T("live.txt"),
              nob_temp_sprintf("select line 30 copy 1\nalso line 36 copy 1\nalso line 45\npreview move 0 1 0\n"
                               "preview move 1 1 0\npreview end\npreview move 0.5 0 0\nmove 0.5 0 0\nundo\nredo\n"
                               "select line 34\nduplicate 0 0 3\nturn 90\nselect line 26\n"
                               "add \"models/rock_largeA.glb\" 2 0 5\nsave\nselect line 44\nmove 0 0 1\nselect id 1\n"
                               "move 0 1 0\nselect line 49\nremove\nprint\ndisk %s/live-changed.map\nprint\n"
                               "select line 30\n",
                               gTmp),
              false);
    RUN("edit-script-shot", 0, M, "--shot", T("live.png"), "--view", "1", "--map", T("live-models"), "--edit-script",
        T("live.txt"), "--fog-cache", "1", "2", "--ao-volume", "0.5", "0.6", "--shot-hud", "--edit-mode");
    CopyTree("tests/models", T("live-models2"));
    RUN("edit-script-dump", 0, M, "--dump-entities", T("live-entities.txt"), "--map", T("live-models2"),
        "--edit-script", T("live.txt"));
    RUN("edit-script-alone", 1, M, "--map", T("live-models2"), "--edit-script", T("live.txt"));

    // A step undone past the save point, then a new edit (the undone step moves to lost[]), then a change on
    // disk: the lost step is reported failed
    CopyTree("tests/models", T("lost-models"));
    CatInto("tests/models/models.map", T("lost-changed.map"), false);
    WriteText(T("lost-changed.map"), "# changed on disk\n", true);
    WriteText(T("lost.txt"),
              nob_temp_sprintf("select line 40\nturn 0 25 15\nsave\nundo\nselect line 30\nduplicate 0 0 -3\nprint\n"
                               "disk %s\nprint\n",
                               T("lost-changed.map")),
              false);
    RUN("edit-script-lost", 0, M, "--dump-boxes", T("lost-boxes.txt"), "--map", T("lost-models"), "--edit-script",
        T("lost.txt"));
    RUN("dump-entities", 0, M, "--dump-entities", T("entities.txt"), "--map", "tests/models");
    RUN("edit-mode-hud", 0, M, "--shot", T("edit.png"), "--view", "1", "--map", "tests/models", "--shot-hud",
        "--edit-mode", "--select", "36");
    RUN("edit-id-panel", 0, M, "--shot", T("edit-id.png"), "--view", "1", "--map", "tests/models", "--shot-hud",
        "--edit-mode", "--select", "30", "--edit-id", "57");

    // `id` and `id remove` (the last command fails on purpose: line 30 has no id any more); the id panel (F5)
    // open on the default map, filtered, and after live edits that give and remove ids (its rows rebuilt) with
    // the edit mode's number field open beside it
    CopyTree("tests/models", T("id-models"));
    WriteText(T("id.txt"), "select line 30\nid\nalso line 36\nid\nid remove\nundo\nredo\nselect line 30\nid remove\n",
              false);
    RUN("apply-id-remove", 1, M, "--apply-edits", T("id.txt"), "--map", T("id-models"));
    RUN("id-panel", 0, M, "--shot", T("idpanel.png"), "--view", "1", "--shot-hud", "--id-panel");
    RUN("keys-panel", 0, M, "--shot", T("keys.png"), "--view", "1", "--shot-hud", "--keys-panel", "grab");

    // The draw panel (docs/painting.md 13.2): drawn whole and one section; clicks through the input shim (the
    // hex field typed into, a number field applied by a click on the next one, a slider, buttons)
    RUN("draw-panel", 0, M, "--shot", T("drawpanel.png"), "--view", "1", "--shot-hud", "--draw-panel");
    RUN("draw-panel-color", 0, M, "--shot", T("drawpanel2.png"), "--view", "1", "--shot-hud", "--draw-panel", "color",
        "--shot-size", "1280", "720");
    // (the panel's layout at 1280 x 720 with maps/monolith's concrete wall as its target (a new rectangle): the
    // hex field, the R and G fields, the soft and square pens; a scripted --draw-panel shot goes through the
    // draw mode's own input, and its keys never reach the designer)
    WriteText(T("drawpanel.txt"),
              "at 155 435\nclick left\nwait 2\npress 3\npress C\npress 8\npress 0\npress F\npress F\npress ENTER\n"
              "wait 2\nat 266 335\nclick left\nwait 2\npress 7\nat 266 359\nclick left\nwait 3\npress ESCAPE\n"
              "at 155 241\nclick left\nwait 2\nat 100 241\nclick left\nwait 2\n",
              false);
    RUN("draw-panel-input", 0, M, "--shot", T("drawpanel3.png"), "--view", "1", "--shot-hud", "--draw-panel",
        "--shot-size", "1280", "720", "--input-script", T("drawpanel.txt"));
    RUN("draw-panel-bad", 1, M, "--shot", T("drawpanel4.png"), "--draw-panel", "nosuch");

    // Painting (docs/painting.md: the paint layer): --paint-script on a copy of tests/models with every pen on
    // the tile wall, the ground and the awning (a surface shader; first strokes allocate rectangles in their
    // materials' atlases), a stroke that goes on onto another face of the same box (one step), face groups, a
    // resize, the pixel look, models (the Kenney rock's "rock" override: its file is unwrapped first), undo /
    // redo, the eyedropper (an opaque paint texel, then the G-buffer albedo of a render), clear, save
    // (atlases, unwrap sidecars, the map, licence rows and sidecars), the hot reload's poll, and a stroke on
    // the sky, refused (the last command: exit 1); then a rebase on the saved copy (an atlas and the map
    // changed on disk, the stroke replayed); --apply-edits with a paint script (a hidden window) and with the
    // paint ops of the edit script; tests/rotated (a turned merged box, an instanced one) in paint
    // coordinates; the draw mode's HUD and brush cursor; strokes, undo, the eyedropper and leaving through the
    // input shim; a click on a model that needs an unwrap (the note, then the unwrap and the stroke a frame
    // later)
    CopyTree("tests/models", T("paint-models"));
    WriteText(T("paint.txt"),
              "pen square\nsize 16\ncolor 30 60 230 128\nstroke px 720 380 880 380\nstroke px 990 380 1000 400\n"
              "pen round\nsize 6\nstroke px 700 330 700 355\nfaces 0 0 1 1 2 2\ndensity 32\nresize\npixel on\n"
              "pen round\nsize 1\ncolor 200 30 30\nstroke px 600 600 1000 640\nsize 16\ncolor 200 30 30 128\n"
              "stroke px 600 620 1000 660 800 560\npen soft\nhardness 0.3\nstroke px 640 470 760 480\npen eraser\n"
              "stroke px 700 600 900 600\npen fill\ndab px 1000 520\ndab px 720 350\npick px 1000 520\n"
              "pick px 400 500\nundo\nundo\nredo\nprint\nsave\npoll\nprint\npen round\nsize 8\ncolor 20 200 20\n"
              "dab px 580 565\nclear\nsave\nprint\nstroke px 850 150 860 150\n",
              false);
    RUN("paint-script", 1, M, "--shot", T("paint.png"), "--view", "1", "--map", T("paint-models"), "--paint-script",
        T("paint.txt"));
    CatInto(T("paint-models/models.map"), T("paint-changed.map"), false);
    WriteText(T("paint-changed.map"), "# changed on disk\n", true);
    WriteText(T("paint-rebase.txt"),
              nob_temp_sprintf("pen round\nsize 8\nstroke px 600 470 700 480\n"
                               "put tests/models/textures/tiles.png textures/paint/ground.png\ndisk %s/paint-changed.map\n"
                               "print\n",
                               gTmp),
              false);
    RUN("paint-rebase", 0, M, "--shot", T("paint2.png"), "--view", "1", "--map", T("paint-models"), "--paint-script",
        T("paint-rebase.txt"));
    CopyTree("tests/models", T("paint-apply"));
    WriteText(T("paint-apply-edits.txt"), "select line 30 copy 1\nmove 0 0 1\n", false);
    WriteText(T("paint-apply.txt"), "pen round\nsize 4\nstroke px 700 700 900 720\nsave\nprint\n", false);
    RUN("paint-apply", 0, M, "--map", T("paint-apply"), "--apply-edits", T("paint-apply-edits.txt"), "--paint-script",
        T("paint-apply.txt"));
    // (the paint ops in an edit script: the atlases exist on disk, as an --apply-edits map needs them)
    CopyTree("tests/models", T("paint-ops"));
    MkdirP(T("paint-ops/textures/paint"));
    nob_copy_file(T("paint-models/textures/paint/ground.png"), T("paint-ops/textures/paint/ground.png"));
    WriteText(T("paint-ops.txt"),
              "select line 26\n"
              "paint-add \"ground\" \"textures/paint/ground.png\" faces +y,+z 0 0 64 64 nearest\n"
              "paint-add \"ground\" \"textures/paint/ground.png\" faces +z 68 0 32 32\n"
              "paint-set \"ground\" faces +y,+z,-x 0 0 64 64\npaint-remove \"ground\" faces -x\nselect line 40\n"
              "paint-add own slot 1 0 0 64 64 nearest\npaint-set own slot 1 trilinear\npaint-remove own\nundo\n"
              "select line 26\npaint-remove \"ground\"\n"
              "paint-add \"ground\" \"textures/paint/ground.png\" faces +q 0 0 4 4\n",
              false);
    RUN("paint-ops", 1, M, "--map", T("paint-ops"), "--apply-edits", T("paint-ops.txt"));
    CopyTree("tests/rotated", T("paint-rot"));
    WriteText(T("paint-rot.txt"),
              "color 200 30 30 160\nsize 12\nstroke uv entity d21aa771:0 4 0.2 0.5 0.8 0.5\n"
              "dab uv entity bf614502:0 2 0.5 0.5\ndab uv entity f0ae616b:0 4 0.5 0.5\npen fill\n"
              "dab uv id 3:0 0 0.5 0.5\nundo\nredo\nsave\nprint\n",
              false);
    RUN("paint-rot", 0, M, "--map", T("paint-rot"), "--apply-edits", "/dev/null", "--paint-script", T("paint-rot.txt"));
    RUN("draw-mode-hud", 0, M, "--shot", T("drawmode.png"), "--view", "1", "--map", T("paint-models"), "--shot-hud",
        "--draw-mode", "--cursor", "800", "600");
    WriteText(T("paint-input.txt"),
              "at 600 600\nwait 2\ndown lmb\nmove 200 20 10\nup lmb\nwait 2\ndown CTRL\npress Z\nup CTRL\nwait 2\n"
              "press 4\nat 650 620\ndown lmb\nmove 150 0 5\nup lmb\nwait 2\ndown ALT\nclick left\nup ALT\nwait 2\n"
              "press X\npress B\nwait 2\npress B\nwait 2\n",
              false);
    CopyTree("tests/models", T("paint-input-models"));
    RUN("draw-mode-input", 0, M, "--shot", T("drawmode2.png"), "--view", "1", "--map", T("paint-input-models"),
        "--shot-hud", "--draw-mode", "--input-script", T("paint-input.txt"));
    CopyTree("tests/models", T("paint-unwrap-input"));
    WriteText(T("paint-unwrap-input.txt"), "at 800 370\nwait 2\nclick left\nwait 4\ndown CTRL\npress Z\nup CTRL\nwait 2\n",
              false);
    RUN("draw-unwrap-input", 0, M, "--shot", T("drawmode3.png"), "--view", "1", "--map", T("paint-unwrap-input"),
        "--shot-hud", "--draw-mode", "--input-script", T("paint-unwrap-input.txt"));

    // The brush cursor and the panel on real paint: the fill outline of a model's UV island (the slot's
    // triangles and islands cached), a ring across a model's triangles after its first stroke, and the panel on
    // a painted box whose faces were regrouped (live only: nothing is saved)
    CopyTree("tests/models", T("paint-cursor-models"));
    WriteText(T("paint-cursor1.txt"), "pen fill\n", false);
    RUN("draw-cursor-fill", 0, M, "--shot", T("drawcursor1.png"), "--view", "2", "--map", T("paint-cursor-models"),
        "--shot-hud", "--draw-mode", "--paint-script", T("paint-cursor1.txt"), "--cursor", "850", "150");
    WriteText(T("paint-cursor2.txt"), "pen round\nsize 16\ndab px 850 150\n", false);
    RUN("draw-cursor-model", 0, M, "--shot", T("drawcursor2.png"), "--view", "2", "--map", T("paint-cursor-models"),
        "--shot-hud", "--draw-mode", "--paint-script", T("paint-cursor2.txt"), "--cursor", "850", "150");
    WriteText(T("paint-cursor3.txt"), "pen square\nsize 8\ndab px 800 700\nfaces 0 0 0 1 0 0\n", false);
    RUN("draw-panel-paint", 0, M, "--shot", T("drawcursor3.png"), "--view", "2", "--map", T("paint-cursor-models"),
        "--shot-hud", "--draw-panel", "--paint-script", T("paint-cursor3.txt"), "--cursor", "800", "700");
    RUN("id-panel-filter", 0, M, "--shot", T("idpanel2.png"), "--view", "1", "--shot-hud", "--id-panel", "block");
    CopyTree("tests/models", T("id-models2"));
    WriteText(T("id-live.txt"), "select line 30\nid 5\nselect line 36\nalso line 40\nid\nselect id 5\nid remove\n", false);
    RUN("id-panel-live", 0, M, "--shot", T("idpanel3.png"), "--view", "1", "--map", T("id-models2"), "--shot-hud",
        "--id-panel", "tree", "--edit-script", T("id-live.txt"), "--edit-mode", "--select", "45", "--edit-id", "9");

    // The transform ops (mapedit.c) on a copy, the last command fails on purpose (a light can't be scaled);
    // live with turn / scale previews; the transform modal (transform.c) open, committed, cancelled
    CopyTree("tests/models", T("xform-models"));
    WriteText(T("xform.txt"),
              "select line 41\nturn 0 30 0\nscale 1.25\nselect line 40\nturn 0 0 15 local\nturn 10 20 30 pivot 1 0 1\n"
              "scale 1 2 1\nselect line 36 copy 1\nturn 37.5 pivot 0 0 -2\nselect line 32\nsize 0 0.5 3\n"
              "turn 90 pivot 0 0 -8\nselect line 30 copy 2\nscale 1.25 1 2\nselect line 30\nturn -90 pivot 0 0 -8\n"
              "undo\nredo\nselect line 49\nscale 2\n",
              false);
    RUN("apply-xform", 1, M, "--apply-edits", T("xform.txt"), "--map", T("xform-models"));
    CopyTree("tests/models", T("xform-live"));
    WriteText(T("xform-live.txt"),
              "select line 30\nalso line 36\nalso line 40\nalso line 45\npreview turn 90 pivot 0 0 0\n"
              "preview scale 1.5 pivot 0 0 0\npreview end\n",
              false);
    HeadLinesAppend(T("xform.txt"), T("xform-live.txt"), 18);
    RUN("edit-script-xform", 0, M, "--shot", T("xform.png"), "--view", "1", "--map", T("xform-live"), "--edit-script",
        T("xform-live.txt"), "--fog-cache", "1", "2", "--ao-volume", "0.5", "0.6");
    RUN("edit-modal-hud", 0, M, "--shot", T("modal.png"), "--view", "1", "--map", "tests/models", "--shot-hud",
        "--select", "30", "--select", "36", "--select", "40", "--edit-modal", "r xx 30");
    CopyTree("tests/models", T("modal-models"));
    RUN("edit-modal-commit", 0, M, "--shot", T("modal-commit.png"), "--view", "1", "--map", T("modal-models"),
        "--select", "30", "--select", "40", "--edit-modal", "s Y 1.5 commit");
    RUN("edit-modal-cancel", 0, M, "--shot", T("modal-cancel.png"), "--view", "1", "--map", "tests/models", "--select",
        "30", "--select", "40", "--select", "45", "--edit-modal", "g x 1.25 cancel");

    // The rotate gizmo and the R step / reset (docs/live-editing.md 11.10), on a model and on turned boxes
    RUN("gizmo-hover", 0, M, "--shot", T("gizmo.png"), "--view", "1", "--map", "tests/models", "--shot-hud", "--select",
        "40", "--edit-modal", "gizmo hover y");
    CopyTree("tests/models", T("gizmo-models"));
    RUN("gizmo-ring", 0, M, "--shot", T("gizmo-ring.png"), "--view", "1", "--map", T("gizmo-models"), "--shot-hud",
        "--select", "40", "--edit-modal", "ring y 37.5 commit");
    RUN("gizmo-ball", 0, M, "--shot", T("gizmo-ball.png"), "--view", "1", "--map", T("gizmo-models"), "--select", "40",
        "--edit-modal", "ball 30 -10 commit");
    RUN("gizmo-view", 0, M, "--shot", T("gizmo-view.png"), "--view", "1", "--map", T("gizmo-models"), "--select", "40",
        "--edit-modal", "ring view 20 cancel");
    CopyTree("tests/rotated", T("gizmo-rotated"));
    RUN("step-r90", 0, M, "--shot", T("step.png"), "--view", "1", "--map", T("gizmo-rotated"), "--select", "155",
        "--edit-modal", "r90");
    RUN("reset-rotation", 0, M, "--shot", T("reset.png"), "--view", "1", "--map", T("gizmo-rotated"), "--select", "147",
        "--edit-modal", "reset");
    RUN("gizmo-ring-box", 0, M, "--shot", T("gizmo-box.png"), "--view", "1", "--map", T("gizmo-rotated"), "--select",
        "142", "--edit-modal", "ring x 15 commit");

    // Turned boxes (box / block lines with `rot`): merged and instanced meshes, the caches, a reload, live edits
    // that add, change and drop a rotation, previews
    RUN("rotated-shot", 0, M, "--shot", T("rotated.png"), "--view", "2", "--map", "tests/rotated", "--fog-cache", "1",
        "2", "--ao-volume", "0.5", "0.6");
    RUN("rotated-reload", 0, M, "--shot", T("rotated-reload.png"), "--view", "1", "--map", "tests/rotated",
        "--reload-test", "tests/models");
    CopyTree("tests/rotated", T("rotated-live"));
    WriteText(T("rotated-live.txt"),
              "select line 147\npreview turn 0 30 0\npreview end\nturn 0 30 0 local\nrotation 0 0 0\nselect line 142\n"
              "turn 37.5\nturn -37.5\nundo\nselect line 149\nturn 15 pivot 0 0 0\n",
              false);
    RUN("rotated-edits", 0, M, "--shot", T("rotated-live.png"), "--view", "1", "--map", T("rotated-live"),
        "--edit-script", T("rotated-live.txt"), "--ao-volume", "0.5", "0.6");

    // The modal through the input shim (--input-script): a click selects a repeat copy, R / G / S with the
    // mouse, a lock, typed digits, LMB / ENTER / RMB, the cursor hidden and shown again, CTRL+S
    CopyTree("tests/models", T("input-models"));
    WriteText(T("input.txt"),
              "at 1180 440\nclick left\nwait 2\ndown ALT\npress R\nup ALT\nmove 140 0 10\nclick left\nwait 2\n"
              "press G\npress X\nmove 60 0 5\npress ENTER\npress S\nmove 80 0 5\nclick right\ndown ALT\npress R\n"
              "up ALT\npress Y\npress 4\npress 5\npress ENTER\nwait 2\ndown CTRL\npress S\nup CTRL\n",
              false);
    RUN("input-script", 0, M, "--shot", T("input.png"), "--view", "2", "--map", T("input-models"), "--shot-hud",
        "--edit-mode", "--frame-reuse", "--input-script", T("input.txt"));
    WriteText(T("input-square.txt"), "select line 32\nsize 2 0 2\n", false);
    CopyTree("tests/models", T("input-square"));
    RUN("input-square-prep", 0, M, "--apply-edits", T("input-square.txt"), "--map", T("input-square"));
    WriteText(T("input-noop.txt"), "at 860 352\ndown ALT\npress R\nup ALT\nclick left\n", false);
    RUN("input-noop", 0, M, "--shot", T("input-noop.png"), "--view", "1", "--map", T("input-square"), "--shot-hud",
        "--select", "32", "--edit-mode", "--input-script", T("input-noop.txt"));
    WriteText(T("input-bad.txt"), "press NOKEY\n", false);
    RUN("input-bad", 1, M, "--shot", T("input-bad.png"), "--input-script", T("input-bad.txt"));

    // The render inspector: outline modes, labels, its panel, buffer views, the draw filter (also with a live
    // edit, a preview and a reload while it hides something)
    RUN("inspect-material", 0, M, "--shot", T("insp1.png"), "--view", "2", "--shot-hud", "--inspect", "material",
        "--output-mode", "normal");
    RUN("inspect-shader-panel", 0, M, "--shot", T("insp2.png"), "--view", "1", "--map", "tests/models", "--shot-hud",
        "--inspect", "shader", "--inspector-panel", "--inspect-hide", "leaves", "--output-mode", "ssao");
    RUN("inspect-draw-isolate", 0, M, "--shot", T("insp3.png"), "--view", "1", "--map", "maps/pine_forest", "--inspect",
        "draw", "--inspect-isolate", "needles");
    CopyTree("tests/models", T("insp-models"));
    RUN("inspect-filter-edit", 0, M, "--shot", T("insp4.png"), "--view", "1", "--map", T("insp-models"),
        "--edit-script", T("live.txt"), "--inspect", "material", "--inspect-hide", "bark", "--fog-cache", "1", "2");
    RUN("inspect-filter-reload", 0, M, "--shot", T("insp6.png"), "--view", "1", "--map", "tests/models",
        "--inspect-hide", "rock", "--reload-test", "maps/pine_forest");
    RUN("inspect-bad-mode", 1, M, "--shot", T("insp5.png"), "--inspect", "nothing");

    // The model picker: thumbnails (the scene's files and one loaded for the render), filtered, freed and
    // rendered again by a reload, a renderer restart, ENTER (a live edit) and ESC
    RUN("picker-hud", 0, M, "--shot", T("pick1.png"), "--view", "1", "--map", "maps/pine_forest", "--shot-hud",
        "--model-picker");
    RUN("picker-loaded", 0, M, "--shot", T("pick2.png"), "--view", "2", "--shot-hud", "--model-picker", "sedan");
    RUN("picker-reload", 0, M, "--shot", T("pick3.png"), "--view", "1", "--map", "tests/models", "--model-picker",
        "--reload-test", "tests/models");
    RUN("picker-restart", 0, M, "--shot", T("pick4.png"), "--view", "1", "--map", "tests/models", "--model-picker",
        "rock", "--settings-change", "shadow_size=2048");
    CopyTree("tests/models", T("pick-models"));
    RUN("picker-enter", 0, M, "--shot", T("pick5.png"), "--view", "1", "--map", T("pick-models"), "--model-picker",
        "oak", "--model-picker-key", "enter");
    RUN("picker-close", 0, M, "--shot", T("pick6.png"), "--view", "1", "--map", "tests/models", "--model-picker",
        "--model-picker-key", "close");
    RUN("picker-bad-key", 1, M, "--shot", T("pick7.png"), "--model-picker", "--model-picker-key", "maybe");
    RUN("shot-hud", 0, M, "--shot", T("hud.png"), "--view", "2", "--shot-hud", "--select", "192", "--select", "198",
        "--region", "100", "100", "300", "200", "--region", "800", "300", "200", "200", "--volume", "-11", "0", "-34",
        "-7", "20", "34", "--volume", "3", "4", "12", "-4", "0", "4", "--type", "make it\\ntaller", "--pick",
        "--pick-at", "10", "10", "--pick-at", "1590", "890");

    // --submit with the stub terminal (no window; MONOLITH_WORKSPACE empty: no workspace switch) into our own
    // requests folder: coordinator mode on the default map (its classifier still running when the editor quits:
    // queued on the default tier), per-request mode on tests/models (@heavy: no classifier), a cancel with no
    // coordinator (MONOLITH_CLAUDE that doesn't exist), a classified one queued, a cancel while classifying and
    // a classifier that fails before a per-request launch. `env` sets the variables for monolith only, as the
    // script did.
    const char *reqEnv = nob_temp_sprintf("MONOLITH_REQUESTS_DIR=%s", REQ);
    const char *termEnv = nob_temp_sprintf("MONOLITH_TERMINAL=%s", STUB);
    RUN("submit-default", 0, "env", reqEnv, termEnv, nob_temp_sprintf("MONOLITH_STUB_LOG=%s", T("stub1.txt")),
        "MONOLITH_WORKSPACE=", "MONOLITH_STUB_CLASSIFIER=slow", M, "--shot", T("s1.png"), "--view", "2", "--select", "192", "--select", "193", "--region",
        "100", "100", "300", "200", "--region", "800", "300", "200", "200", "--volume", "-11", "0", "-34", "-7", "20",
        "34", "--volume", "-4", "0", "4", "3", "4", "12", "--submit", "leakcheck: make these taller");
    RUN("submit-models", 0, "env", reqEnv, termEnv, nob_temp_sprintf("MONOLITH_STUB_LOG=%s", T("stub2.txt")),
        "MONOLITH_WORKSPACE=", "MONOLITH_LAUNCH=per-request", M, "--shot", T("s2.png"), "--view", "1", "--map",
        "tests/models", "--select", "36", "--region", "0", "0", "1600", "900", "--volume", "-50", "-1", "-50", "50", "30",
        "50", "--submit", "@heavy leakcheck: restyle the trees", "--submit-wait", "1", "--close-submitted");
    RUN("submit-cancel", 0, "env", reqEnv, termEnv, "MONOLITH_CLAUDE=/nonexistent/claude", "MONOLITH_WORKSPACE=", M,
        "--shot", T("s3.png"), "--view", "2", "--submit", "leakcheck: cancel me", "--submit-wait", "1",
        "--close-submitted");
    RUN("submit-classified", 0, "env", reqEnv, termEnv, "MONOLITH_STUB_CLASSIFIER=heavy", "MONOLITH_WORKSPACE=", M,
        "--shot", T("s4.png"), "--view", "2", "--select", "192", "--submit", "leakcheck: classified", "--submit-wait", "1");
    RUN("submit-cancel-classifying", 0, "env", reqEnv, termEnv, "MONOLITH_STUB_CLASSIFIER=slow", "MONOLITH_WORKSPACE=",
        M, "--shot", T("s5.png"), "--view", "2", "--submit", "leakcheck: cancel while classifying", "--submit-wait",
        "0.5", "--close-submitted");
    RUN("submit-classifier-fail", 0, "env", reqEnv, termEnv, nob_temp_sprintf("MONOLITH_STUB_LOG=%s", T("stub3.txt")),
        "MONOLITH_STUB_CLASSIFIER=fail", "MONOLITH_LAUNCH=per-request", "MONOLITH_WORKSPACE=", M, "--shot", T("s6.png"),
        "--view", "2", "--submit", "leakcheck: classifier fails", "--submit-wait", "1");
    RUN("export-default", 0, M, "--export", T("export-default"));
    RUN("export-models", 0, M, "--export", T("export-models"), "--map", "tests/models");

    // Compiled maps: a copy of tests/models compiled, loaded (dumps, a shot with a reload), a damaged file, then
    // its source changed (stale: a warning when loaded, the export refuses it)
    const char *CM = strdup(T("compiled-models"));
    MkdirP(CM);
    CopyTree("tests/models", CM);
    const char *cmMapb = strdup(nob_temp_sprintf("%s/models.mapb", CM));
    RUN("compile", 0, M, "--compile", cmMapb, "--map", CM);
    RUN("compile-broken", 1, M, "--compile", T("broken.mapb"), "--map", T("missing-package"));
    RUN("dump-scene-compiled", 0, M, "--dump-scene", T("scene-compiled.txt"), "--map", cmMapb);
    RUN("shot-compiled", 0, M, "--shot", T("compiled.png"), "--view", "1", "--map", cmMapb, "--reload-test", cmMapb);
    HeadBytes(cmMapb, nob_temp_sprintf("%s/damaged.mapb", CM), 4000);
    RUN("compiled-damaged", 1, M, "--dump-boxes", T("damaged.txt"), "--map", nob_temp_sprintf("%s/damaged.mapb", CM));
    WriteText(nob_temp_sprintf("%s/models.map", CM), "# changed\n", true);
    RUN("compiled-stale", 0, M, "--dump-entities", T("stale.txt"), "--map", cmMapb);
    RUN("export-stale", 1, M, "--export", T("export-stale"), "--map", cmMapb);
    RUN("inspect-model", 0, M, "--inspect-model", "tests/models/models/tree_oak.glb");
    RUN("inspect-missing", 1, M, "--inspect-model", T("missing.glb"));
    // --new-map by name (a package in maps/, trashed at the end) and by path (in tmp/)
    const char *newMap = strdup(nob_temp_sprintf("_leakcheck_%s", stamp));
    RUN("new-map", 0, M, "--new-map", newMap);
    RUN("new-map-path", 0, M, "--new-map", T("newmap_path"));
    // --classify: the AI path with the stub (an answer, garbage, a timeout), off, and an override
    RUN("classify", 0, M, "--classify", "make the pillar taller", "--select", "192");
    RUN("classify-garbage", 0, "env", "MONOLITH_STUB_CLASSIFIER=garbage", M, "--classify", "leakcheck: garbage");
    RUN("classify-timeout", 0, "env", "MONOLITH_STUB_CLASSIFIER=slow", "MONOLITH_CLASSIFIER_TIMEOUT=1", M, "--classify",
        "leakcheck: too slow");
    RUN("classify-off", 0, "env", "MONOLITH_CLASSIFIER=off", M, "--classify", "leakcheck: off");
    RUN("classify-override", 0, M, "--classify", "@fast leakcheck: override");

    // The profiling tool: its report on a cut-down real profile and bench report (tests/profile/), a missing
    // file, and a run it must refuse (it would open a window) before perf starts
    ToolSay("running the profiling tool ...");
    RUN("profile-report", 0, PROFILE, "--report-only", "tests/profile/perf.txt");
    RUN("profile-report-all", 0, PROFILE, "--report-only", "tests/profile/perf.txt", "--all", "--top", "5");
    RUN("profile-report-gpu", 0, PROFILE, "--report-only", "tests/profile/perf.txt", "--bench", "tests/profile/bench.md",
        "--finish-site", "Run+0x5c02");
    RUN("profile-report-gpu-all", 0, PROFILE, "--report-only", "tests/profile/perf.txt", "--bench",
        "tests/profile/bench.md", "--all");
    RUN("profile-missing", 1, PROFILE, "--report-only", T("missing-perf.txt"));
    RUN("profile-refuse", 2, PROFILE, "--bin", M, "refused", "--", "--view", "2");

    // The paint unwrap (src/paintunwrap.c + xatlas): the synthetic meshes, bad input and the model files of the
    // packages (R3D's importer in a hidden window); the sidecar round trip in tmp/. The models: every
    // maps/*/models/*.glb, then every *.gltf (two globs, each sorted, as bash expanded them)
    ToolSay("running the paint unwrap test ...");
    {
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, nob_temp_sprintf("%s/paintunwrap_test", binDir), "--scratch", gTmp);
        static const char *patterns[] = {"maps/*/models/*.glb", "maps/*/models/*.gltf"};
        for (size_t p = 0; p < NOB_ARRAY_LEN(patterns); p++) {
            glob_t g;
            if (glob(patterns[p], 0, NULL, &g) == 0) {
                for (size_t i = 0; i < g.gl_pathc; i++) {
                    struct stat st;
                    // [[ -f "$m" ]]: regular files only
                    if (stat(g.gl_pathv[i], &st) == 0 && S_ISREG(st.st_mode)) {
                        nob_cmd_append(&cmd, nob_temp_strdup(g.gl_pathv[i]));
                    }
                }
                globfree(&g);
            }
        }
        RunIn("paintunwrap", 0, NULL, &cmd);
    }

    // run-sh.o: every mode on allowed, refused and unterminated lines (--check and --hook run nothing; -c runs
    // harmless commands). Its log and benchmark lock go to tmp/run-sh, not build/run-sh. Then its unit test
    // (ASan + UBSan, with its own sanitizer build of run-sh.c on every case).
    ToolSay("running run-sh.o and its test ...");
    setenv("RUN_SH_DIR", T("run-sh"), 1);
    // an allowed line with a here-document, a pipe, redirections, every kind of expansion, loops and a case,
    // and a benchmark with an environment assignment
    const char *rsAllowed = "cat <<EOF | grep -c x > build/x.txt\n"
                            "$(echo {a,b} ${HOME:-/} $((1 + 2)))\n"
                            "EOF\n"
                            "for f in a b; do case $f in a) echo \"$f\";; esac; done; __GL_SYNC_TO_VBLANK=0 "
                            "build/linux/monolith --bench --headless --note x";
    // a find deletion whose root leaves /tmp after a cd
    const char *rsRefused = "cd /tmp && find ../home -exec rm {} +";
    // deletes under /tmp only: allowed (the developer's decision)
    const char *rsTmp = "rm -rf /tmp/a/x.png '/tmp/b' && find /tmp/c -delete";
    // the hook's JSON (each line ends with the newline bash's <<< adds): a Bash call, a Monitor call, bad JSON
    WriteText(T("run-sh-hook.json"),
              nob_temp_sprintf("{\"cwd\":\"%s\",\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"echo 'a b'\","
                               "\"description\":\"d\",\"run_in_background\":true}}\n",
                               root),
              false);
    WriteText(T("run-sh-monitor.json"), "{\"tool_name\":\"Monitor\",\"tool_input\":{\"command\":\"rm -rf src\"}}\n", false);
    WriteText(T("run-sh-bad.json"), "{\"tool_name\":\n", false);
    RUN("run-sh-allowed", 0, RUNSH, "--check", rsAllowed);
    RUN("run-sh-refused", 126, RUNSH, "--check", rsRefused);
    RUN("run-sh-tmp", 0, RUNSH, "--check", rsTmp);
    RUN("run-sh-unterminated", 0, RUNSH, "--check", "echo \"$(ls");
    RUN("run-sh-c", 7, RUNSH, "-c", "exit 7");
    RUN("run-sh-c-refused", 126, RUNSH, "-c", "git reset --hard");
    RUN_STDIN("run-sh-hook", 0, T("run-sh-hook.json"), RUNSH, "--hook");
    RUN_STDIN("run-sh-hook-monitor", 0, T("run-sh-monitor.json"), RUNSH, "--hook");
    RUN_STDIN("run-sh-hook-bad", 2, T("run-sh-bad.json"), RUNSH, "--hook");
    unsetenv("RUN_SH_DIR");
    RUN("run-sh-test", 0, "./build.sh", "test", "run_sh");

    // The other programs of build/tools/ (their sanitizer copies in <build folder>/tools/), on harmless inputs
    // only: nothing here writes outside tmp/, opens a window, benchmarks, builds an old commit or starts a
    // coordinator.
    ToolSay("running the tools ...");
    {
        const char *td = strdup(T("tools"));
        MkdirP(td);
        const char *tool[NOB_ARRAY_LEN(SAN_TOOLS)];
        for (size_t i = 0; i < NOB_ARRAY_LEN(SAN_TOOLS); i++) {
            tool[i] = strdup(nob_temp_sprintf("%s/%s", toolDir, SAN_TOOLS[i]));
        }
        enum { STUBT, RQ, TODO, AITODO, STUBC, TESTC, BENCHAB, OLDM, STUBCL };

        // stub-terminal: its argv into a log and a "window" counted; then MONOLITH_STUB_RUN, which hands the
        // process over to the command after -e (a plain echo)
        RUN("tool-stub-terminal", 0, "env", nob_temp_sprintf("MONOLITH_STUB_LOG=%s/stub-log.txt", td),
            nob_temp_sprintf("MONOLITH_STUB_OPENED=%s/stub-opened.txt", td), tool[STUBT], "--title", "leakcheck",
            "-e", "claude", "a b");
        RUN("tool-stub-terminal-run", 0, "env", nob_temp_sprintf("MONOLITH_STUB_RUN=%s/stub-run.txt", td), tool[STUBT],
            "-e", "echo", "leakcheck");

        // request-queue on a scratch requests folder: the usage text, an empty folder, two hand-made bundles (one
        // queued, one done: only the queued one is open), `wait` that reports the queued one, a second `wait`
        // that has nothing new (exit 3 after its timeout), and a bad id
        const char *rqEmpty = nob_temp_sprintf("MONOLITH_REQUESTS_DIR=%s/requests-empty", td);
        const char *rqDir = strdup(nob_temp_sprintf("%s/requests", td));
        const char *rqEnv = nob_temp_sprintf("MONOLITH_REQUESTS_DIR=%s", rqDir);
        MkdirP(nob_temp_sprintf("%s/requests-empty", td));
        MkdirP(nob_temp_sprintf("%s/20261113-120000-queued", rqDir));
        MkdirP(nob_temp_sprintf("%s/20261113-120100-done", rqDir));
        WriteText(nob_temp_sprintf("%s/20261113-120000-queued/state", rqDir), "queued 2026-11-13T12:00:00 game\n", false);
        WriteText(nob_temp_sprintf("%s/20261113-120000-queued/request.json", rqDir),
                  "{\n  \"format\": 6,\n  \"package\": \"maps/nonexistent\",\n"
                  "  \"prompt\": \"leakcheck: make it \\\"taller\\\"\\nand wider\",\n"
                  "  \"model\": { \"tier\": \"heavy\", \"name\": \"opus\", \"reason\": \"a \\\"tier\\\": \\\"x\\\" in it\", "
                  "\"override\": false, \"source\": \"classifier\", \"classifier\": \"ai\", \"classifierModel\": "
                  "\"claude-fable-5-2\", \"seconds\": 2.50 }\n}\n",
                  false);
        WriteText(nob_temp_sprintf("%s/20261113-120100-done/state", rqDir), "done 2026-11-13T12:01:00 coordinator\n",
                  false);
        WriteText(nob_temp_sprintf("%s/20261113-120100-done/request.json", rqDir),
                  "{\n  \"format\": 6,\n  \"package\": \"maps/nonexistent\",\n  \"prompt\": \"finished\"\n}\n",
                  false);
        RUN("tool-request-queue-usage", 2, "env", rqEnv, tool[RQ]);
        RUN("tool-request-queue-list-empty", 0, "env", rqEmpty, tool[RQ], "list");
        RUN("tool-request-queue-list", 0, "env", rqEnv, tool[RQ], "list");
        RUN("tool-request-queue-wait", 0, "env", rqEnv, tool[RQ], "wait", "--timeout", "5");
        RUN("tool-request-queue-wait-timeout", 3, "env", rqEnv, tool[RQ], "wait", "--timeout", "1");
        RUN("tool-request-queue-bad-id", 2, "env", rqEnv, tool[RQ], "claim", "../x");

        // A scratch repo root for the programs that act on the repo itself (todo / ai-todo write the lists in the
        // root; stub-coordinator and test-coordinator start the queue and the editor from it): a folder with
        // build.sh and nob.c (what ToolInit looks for; placeholders, never run), copies of the two lists, the
        // Simple-Todo program linked in, and links to the sanitizer copies, run by the links' paths so their
        // root is this folder. todo / ai-todo run only `list` and arguments they refuse before todo.o starts.
        const char *sr = strdup(nob_temp_sprintf("%s/root", td));
        MkdirP(nob_temp_sprintf("%s/bin", sr));
        MkdirP(nob_temp_sprintf("%s/.claude", sr));
        MkdirP(nob_temp_sprintf("%s/submodules/simple-todo", sr));
        WriteText(nob_temp_sprintf("%s/build.sh", sr), "# placeholder: the leak check's scratch repo root\n", false);
        WriteText(nob_temp_sprintf("%s/nob.c", sr), "// placeholder: the leak check's scratch repo root\n", false);
        if (!CatInto("TODO.md", nob_temp_sprintf("%s/TODO.md", sr), false)) {
            WriteText(nob_temp_sprintf("%s/TODO.md", sr), "", false);
        }
        if (!CatInto(".claude/TODO.md", nob_temp_sprintf("%s/.claude/TODO.md", sr), false)) {
            WriteText(nob_temp_sprintf("%s/.claude/TODO.md", sr), "", false);
        }
        if (symlink(nob_temp_sprintf("%s/submodules/simple-todo/todo.o", root),
                    nob_temp_sprintf("%s/submodules/simple-todo/todo.o", sr)) != 0) {
            ToolErr("could not link todo.o into %s: %s", sr, strerror(errno));
        }
        const char *link[NOB_ARRAY_LEN(SAN_TOOLS)] = {0};
        for (size_t i = 0; i < NOB_ARRAY_LEN(SAN_TOOLS); i++) {
            link[i] = strdup(nob_temp_sprintf("%s/bin/%s", sr, SAN_TOOLS[i]));
            if (symlink(tool[i], link[i]) != 0) ToolErr("could not link %s: %s", link[i], strerror(errno));
        }
        // (only where submodules/simple-todo is checked out: a git worktree usually has it empty, and then both
        // programs stop with "not checked out" before their checks, exit 1)
        if (access(nob_temp_sprintf("%s/submodules/simple-todo/todo.o", root), X_OK) == 0) {
            RUN("tool-todo-list", 0, link[TODO], "list");
            RUN("tool-todo-rm-bad", 1, link[TODO], "rm", "1", "x1");
            RUN("tool-ai-todo-list", 0, link[AITODO], "list");
            RUN("tool-ai-todo-add-long", 1, link[AITODO], "add",
                "bug: a title that is far longer than the sixty characters an entry may have",
                "leakcheck: refused before anything is written");
            RUN("tool-ai-todo-add-lines", 1, link[AITODO], "add", "bug: two\nlines", "leakcheck");
        }
        // (stub-coordinator stops at its first step: no build/tools/request-queue in the scratch root; test-
        // coordinator at its first check: no build/linux/monolith there)
        RUN("tool-stub-coordinator-no-queue", 1, link[STUBC]);
        RUN("tool-test-coordinator-no-game", 1, link[TESTC]);

        // bench-export-ab: the usage text, --summarize on three hand-made report pairs (two views and the
        // overall row in each "## Results" table), and on a folder with none
        const char *ab = strdup(nob_temp_sprintf("%s/export-ab", td));
        MkdirP(ab);
        MkdirP(nob_temp_sprintf("%s/export-ab-empty", td));
        for (int pair = 1; pair <= 3; pair++) {
            for (int side = 0; side < 2; side++) {
                double base = 4.0 + 0.1 * pair + 0.25 * side;
                WriteText(nob_temp_sprintf("%s/%c-%d.md", ab, side ? 'B' : 'A', pair),
                          nob_temp_sprintf("# Bench (leakcheck fixture)\n\n## Results\n\n"
                                           "| View | Avg ms | p99 ms |\n|---|---|---|\n"
                                           "| 1 hall | %.3f | 9.0 |\n| 2 yard | %.3f | 9.5 |\n"
                                           "| **All** | **%.3f** | 9.5 |\n\n## Notes\n\n| x | 1 |\n",
                                           base, base + 1.0, base + 0.5),
                          false);
            }
        }
        RUN("tool-bench-export-ab-usage", 2, tool[BENCHAB]);
        RUN("tool-bench-export-ab-summarize", 0, tool[BENCHAB], "--summarize", ab);
        RUN("tool-bench-export-ab-summarize-empty", 1, tool[BENCHAB], "--summarize",
            nob_temp_sprintf("%s/export-ab-empty", td));

        // old-monolith: no argument, an option instead of a commit, a commit that doesn't exist (git rev-parse
        // only: nothing is created), and its rm stand-in (a link named rm: it only logs what it was asked to
        // delete)
        RUN("tool-old-monolith-usage", 1, tool[OLDM]);
        RUN("tool-old-monolith-option", 1, tool[OLDM], "--help");
        RUN("tool-old-monolith-no-commit", 2, tool[OLDM], "leakcheck-no-such-commit");
        MkdirP(nob_temp_sprintf("%s/shim", td));
        const char *rmShim = strdup(nob_temp_sprintf("%s/shim/rm", td));
        if (symlink(tool[OLDM], rmShim) != 0) ToolErr("could not link %s: %s", rmShim, strerror(errno));
        RUN("tool-old-monolith-rm-shim", 0, rmShim, "-rf", nob_temp_sprintf("%s/nothing-here", td));
    }

    // The examples (LSan mode only: they have no ASan build), each with --shot; 05 finds an entity by id
    if (lsan) {
        ToolSay("running the examples ...");
        RUN("ex-01-minimal", 0, nob_temp_sprintf("%s/01_minimal/minimal", exDir), "--shot", T("e1.png"));
        RUN("ex-01-minimal-v2", 0, nob_temp_sprintf("%s/01_minimal/minimal", exDir), "--shot", T("e1b.png"), "--view",
            "2");
        RUN("ex-01-minimal-quality", 0, nob_temp_sprintf("%s/01_minimal/minimal", exDir), "--shot", T("e1q.png"),
            "--view", "2", "--quality", "medium");
        RUN("ex-02-before", 0, nob_temp_sprintf("%s/02_existing_project/before", exDir), "--shot", T("e2a.png"));
        RUN("ex-02-after", 0, nob_temp_sprintf("%s/02_existing_project/after", exDir), "--shot", T("e2b.png"));
        RUN("ex-03-live-editing", 0, nob_temp_sprintf("%s/03_live_editing/live_editing", exDir), "--shot", T("e3.png"));
        RUN("ex-05-entities", 0, nob_temp_sprintf("%s/05_entities/entities", exDir), "--shot", T("e5.png"), "--find",
            "a7d95ba7:0");
    }

    // ---------------------------------------------------------------------------------------------------------
    // Section 4: clean up what the runs created (to the trash, never rm): the package made in maps/, and
    // tmp/ with everything else
    Trash(nob_temp_sprintf("%s/maps/%s", root, newMap));
    Trash(gTmp);

    // ---------------------------------------------------------------------------------------------------------
    // Section 5: the report. lsan_report.py prints one line per run (from runs.txt) and the stack of every leak
    // of ours, and exits 1 when there is one.
    {
        size_t runs = 0;
        Nob_String_Builder sb = {0};
        if (nob_read_entire_file(nob_temp_sprintf("%s/runs.txt", gLog), &sb)) {
            for (size_t i = 0; i < sb.count; i++) runs += sb.items[i] == '\n';
        }
        nob_sb_free(sb);
        ToolSay("results (%zu runs; logs in %s/)", runs, gLog + strlen(root) + 1);
    }
    Nob_Cmd report = {0};
    nob_cmd_append(&report, "python3", nob_temp_sprintf("%s/tools/lsan_report.py", root),
                   nob_temp_sprintf("%s/tools/lsan.supp", root), gLog);
    int leaks = ToolRun(&report, (ToolIo){0});
    nob_cmd_free(report);
    if (leaks == 0 && !gFailed) {
        ToolSay("OK: zero leaks of ours");
        return 0;
    }
    if (leaks != 0) ToolSay("FAILED: leaks of ours (stacks above)");
    if (gFailed) ToolSay("FAILED: a run exited unexpectedly or hit a sanitizer error (see above)");
    return 1;
}
