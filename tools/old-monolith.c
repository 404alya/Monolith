// tools/old-monolith.c: build monolith as it was at an older commit; print the binary's path. It replaced
// tools/old-monolith.sh (2026-11-13) with the same arguments, folders, log, messages and exit codes.
//
//   build/tools/old-monolith <commit>
//
// The migrate-map skill uses it to build a map format's last reader (the `last-reader:` lines of
// .claude/skills/migrate-map/versions.md) and prove a migration against it.
//
// - <commit> is resolved in the main repository (the checkout that owns this repo's git objects, also when this
//   program runs from a worktree). Folders are named after its first 12 hex digits.
// - Reuse: if /tmp/monolith-old-<hash12>-<n>/ holds .old-monolith-done with the full hash and its
//   build/linux/monolith exists, that binary is printed without building (the lowest such n wins).
// - Otherwise: a fresh worktree /tmp/monolith-old-<hash12>-<n>/ (the first n with nothing of that name, nor a
//   <n>.log), `git worktree add --detach`; submodules/r3d initialised at the commit's gitlink from the main
//   checkout's objects when the commit has it as a submodule (no network; commits that vendor R3D as a plain tree
//   need nothing); then the commit's own ./build.sh, with R3D_DIR unset (the commit's own R3D) and PATH led by
//   the folder <worktree>/.old-monolith-shim, whose rm, rmdir and unlink are symlinks to this program: called by
//   one of those names it only logs "<name> shim (not deleting): <arguments>" on stderr (which is the build
//   log) and exits 0 (DeleteStandIn, before anything else). Log: /tmp/monolith-old-<hash12>-<n>.log. On success
//   it writes .old-monolith-done.
// - stdout: exactly one line on success, the binary's absolute path. Progress and errors go to stderr
//   ("old-monolith: ..."); on failure: FAILED, the log, its last 20 lines, and always "left behind:".
// - It never deletes anything (no rm, no git worktree remove / prune); it lists what it leaves.
// - Old binaries may lack the agent-shell window guard: run them only with --dump-boxes, --dump-scene,
//   --dump-scene-diff or --shot. Build one old commit at a time (a full R3D build each).
//
// Environment: PATH (the shims go in front of it for the build), R3D_DIR (removed for the build). No variables of
// its own.
// Exit codes: 0 built or reused; 1 usage; 2 no git history or commit not found;
//             3 worktree or submodule setup failed; 4 the build failed (non-zero exit, or no binary).
//             As rm / rmdir / unlink: always 0.
// Called by: the migrate-map skill (and the developer); built by ./build.sh --tools.

#include "tool.h"

#include <glob.h>

// #region The rm / rmdir / unlink stand-ins

// Called through a symlink named rm, rmdir or unlink (the old commit's build runs with the shim folder first in
// PATH): logs what would have been deleted, exactly as the script's generated shims did
// (`echo "<name> shim (not deleting): $*" >&2; exit 0`), and deletes nothing. The build runs in a fresh folder,
// so a missing delete only leaves stale files that the build overwrites. Returns false when this program wasn't
// called by one of those names.
static bool DeleteStandIn(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (strcmp(base, "rm") != 0 && strcmp(base, "rmdir") != 0 && strcmp(base, "unlink") != 0) return false;
    // "$*": the arguments joined by single spaces (nothing after the colon's space when there are none)
    fprintf(stderr, "%s shim (not deleting): ", base);
    for (int i = 1; i < argc; i++) fprintf(stderr, "%s%s", i > 1 ? " " : "", argv[i]);
    fputc('\n', stderr);
    return true;
}

// #endregion

// #region Messages, failure, "left behind"

// Everything this run created, for "left behind:" (the log, the worktree, git's record of it).
static Nob_File_Paths gCreated;
// The build log once it exists (fail prints its last lines).
static const char *gLog = NULL;

// "left behind: <what this run created, space-separated>" or "left behind: nothing", on stderr.
static void LeftBehind(void)
{
    if (gCreated.count == 0) {
        ToolErr("left behind: nothing");
        return;
    }
    Nob_String_Builder sb = {0};
    for (size_t i = 0; i < gCreated.count; i++) nob_sb_appendf(&sb, "%s%s", i ? " " : "", gCreated.items[i]);
    nob_sb_append_null(&sb);
    ToolErr("left behind: %s", sb.items);
    nob_sb_free(sb);
}

// `tail -n 20 log >&2`: the log's last `lines` lines on stderr (a last line without a newline is printed as it is).
static void TailToStderr(const char *path, size_t lines)
{
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(path, &sb)) return;
    size_t start = 0;
    if (sb.count > 0) {
        size_t seen = 0;
        size_t i = sb.count;
        if (sb.items[i - 1] == '\n') i--;   // the last line's own newline doesn't start another line
        while (i > 0) {
            if (sb.items[i - 1] == '\n' && ++seen == lines) break;
            i--;
        }
        start = i;
    }
    fwrite(sb.items + start, 1, sb.count - start, stderr);
    fflush(stderr);
    nob_sb_free(sb);
}

// fail <exit code> <reason>: "FAILED: <reason>", the log and its last 20 lines when there is one, "left behind:",
// then exit with the code.
NOB_PRINTF_FORMAT(2, 3) static void Fail(int code, const char *fmt, ...)
{
    // the reason as one heap string (no length limit: it may hold paths)
    va_list args;
    va_start(args, fmt);
    char *reason = NULL;
    if (vasprintf(&reason, fmt, args) < 0) reason = NULL;
    va_end(args);
    ToolErr("FAILED: %s", reason ? reason : "(out of memory)");
    struct stat st;
    if (gLog && stat(gLog, &st) == 0 && S_ISREG(st.st_mode)) {
        ToolErr("log: %s", gLog);
        ToolErr("last log lines:");
        TailToStderr(gLog, 20);
    }
    LeftBehind();
    exit(code);
}

// #endregion

// #region git

// "$(git ...)": runs git with the arguments, its stdout captured (trailing newlines removed) and its stderr
// dropped (2>/dev/null) or appended to the log (`log`). Returns the exit status; *out is a heap string (free it).
static int Git(char **out, const char *log, Nob_Cmd *cmd)
{
    Nob_String_Builder sb = {0};
    int status = ToolCapture(cmd, &sb, log ? (ToolIo){.err = log, .append = true} : (ToolIo){.err = "/dev/null"});
    while (sb.count > 0 && sb.items[sb.count - 1] == '\n') sb.count--;
    nob_sb_append_null(&sb);
    *out = sb.items;
    return status;
}

// gitlink <commit-ish> <path> [repo]: the commit a submodule at <path> points to in that tree (the third field of
// `git ls-tree`'s line when its mode is 160000), else "" (a plain folder, or nothing there). Heap string.
static char *Gitlink(const char *repo, const char *treeish, const char *path)
{
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "git", "-C", repo, "ls-tree", treeish, "--", path);
    char *listing;
    Git(&listing, NULL, &cmd);
    nob_cmd_free(cmd);
    // awk '$1 == "160000" { print $3 }': the fields split at blanks and tabs
    Nob_String_Builder result = {0};
    for (char *line = listing; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *fields[3] = {0};
        int count = 0;
        for (char *p = line; *p && count < 3;) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            fields[count++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
        if (count >= 3 && strcmp(fields[0], "160000") == 0) {
            if (result.count) nob_da_append(&result, '\n');
            nob_sb_append_cstr(&result, fields[2]);
        }
        line = nl ? nl + 1 : NULL;
    }
    nob_sb_append_null(&result);
    free(listing);
    return result.items;
}

// init_gitlink <path in worktree> <commit> <candidate source repos...>: clones a submodule from the first local
// repository (of `sources`, a NULL-terminated list) that exists and has the commit (GIT_ALLOW_PROTOCOL=file:
// never the network), and checks the commit out detached; the clone's output goes to the log. Returns the source
// used (a heap string), or NULL when no candidate has the commit or the clone / checkout from the first one that
// has it failed (the next candidates aren't tried then, as in the script).
static char *InitGitlink(const char *dst, const char *sha, const char **sources)
{
    for (const char **src = sources; *src; src++) {
        struct stat st;
        if (stat(*src, &st) != 0) continue;
        Nob_Cmd cmd = {0};
        // git -C <src> cat-file -e <sha>^{commit} 2>/dev/null || continue
        nob_cmd_append(&cmd, "git", "-C", *src, "cat-file", "-e", nob_temp_sprintf("%s^{commit}", sha));
        if (ToolRun(&cmd, (ToolIo){.err = "/dev/null"}) != 0) {
            nob_cmd_free(cmd);
            continue;
        }
        // GIT_ALLOW_PROTOCOL=file git clone --quiet --no-checkout --no-hardlinks <src> <dst> >> log 2>&1, then
        // git -C <dst> checkout --quiet --detach <sha> >> log 2>&1
        nob_cmd_append(&cmd, "env", "GIT_ALLOW_PROTOCOL=file", "git", "clone", "--quiet", "--no-checkout",
                       "--no-hardlinks", *src, dst);
        bool ok = ToolRun(&cmd, (ToolIo){.out = gLog, .append = true, .errToOut = true}) == 0;
        if (ok) {
            nob_cmd_append(&cmd, "git", "-C", dst, "checkout", "--quiet", "--detach", sha);
            ok = ToolRun(&cmd, (ToolIo){.out = gLog, .append = true, .errToOut = true}) == 0;
        }
        nob_cmd_free(cmd);
        return ok ? strdup(*src) : NULL;
    }
    return NULL;
}

// #endregion

// #region The build

// Runs the commit's own ./build.sh in the worktree with R3D_DIR unset and the shim folder first in PATH, its
// stdout and stderr appended to the log (the script's
// `(cd "$WT" && env -u R3D_DIR PATH="$SHIM:$PATH" ./build.sh) >>"$LOG" 2>&1`). Returns its exit status (1 when
// the folder can't be entered, 126 / 127 when build.sh can't be started, as the shell reports them).
static int RunOldBuild(const char *wt, const char *shim)
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) return 126;
    if (pid == 0) {
        // the child: the log on fds 1 and 2, then the environment, the folder and build.sh
        int fd = open(gLog, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd < 0) _exit(1);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        const char *path = getenv("PATH");
        setenv("PATH", nob_temp_sprintf("%s:%s", shim, path ? path : ""), 1);
        unsetenv("R3D_DIR");
        if (chdir(wt) != 0) {
            fprintf(stderr, "cd: %s: %s\n", wt, strerror(errno));
            _exit(1);
        }
        char *args[] = {"./build.sh", NULL};
        execv(args[0], args);
        fprintf(stderr, "./build.sh: %s\n", strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }
    int wstatus;
    while (waitpid(pid, &wstatus, 0) < 0) {
        if (errno != EINTR) return 126;
    }
    return ToolStatus(wstatus);
}

// #endregion

int main(int argc, char **argv)
{
    // Called as rm / rmdir / unlink (a symlink in the shim folder): log and exit before anything else, even
    // before ToolInit (a stand-in must not rebuild itself or look for a repo: it runs inside the old build)
    if (DeleteStandIn(argc, argv)) return 0;

    ToolInit(argv, "old-monolith", "old-monolith");

    // One argument, the commit: not empty, not an option
    if (argc != 2 || !argv[1][0] || argv[1][0] == '-') {
        ToolErr("usage: build/tools/old-monolith <commit>   (build monolith as it was at <commit>; print the binary's "
                "path)");
        return 1;
    }
    const char *want = argv[1];

    // The repo this program belongs to, and the main checkout that owns its objects (the common git dir's
    // parent: a worktree's --git-common-dir is <main>/.git)
    const char *selfRoot = gToolRoot;
    Nob_Cmd cmd = {0};
    char *common;
    nob_cmd_append(&cmd, "git", "-C", selfRoot, "rev-parse", "--path-format=absolute", "--git-common-dir");
    if (Git(&common, NULL, &cmd) != 0) Fail(2, "no git history at %s", selfRoot);
    const char *commonBase = strrchr(common, '/');
    commonBase = commonBase ? commonBase + 1 : common;
    char *mainRepo = strcmp(commonBase, ".git") == 0 ? strndup(common, (size_t)(commonBase - common - 1))
                                                     : strdup(selfRoot);   // a bare or unusual layout

    // The commit's full hash, in the main checkout; its first 12 digits name the folders
    char *full;
    nob_cmd_append(&cmd, "git", "-C", mainRepo, "rev-parse", "--verify", "--quiet", nob_temp_sprintf("%s^{commit}", want));
    if (Git(&full, NULL, &cmd) != 0) Fail(2, "commit not found: %s (in %s)", want, mainRepo);
    char h12[13] = {0};
    strncpy(h12, full, 12);
    const char *base = nob_temp_sprintf("/tmp/monolith-old-%s", h12);

    // Reuse a finished build: the lowest n whose marker holds the full hash and whose binary exists
    glob_t g;
    long reuseN = -1;
    const char *reuse = NULL;
    if (glob(nob_temp_sprintf("%s-*", base), 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            const char *d = g.gl_pathv[i];
            struct stat st;
            if (stat(d, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            const char *n = strrchr(d, '-') + 1;
            bool digits = *n != '\0';
            for (const char *p = n; *p; p++) digits = digits && *p >= '0' && *p <= '9';
            if (!digits) continue;
            const char *marker = nob_temp_sprintf("%s/.old-monolith-done", d);
            if (stat(marker, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            if (access(nob_temp_sprintf("%s/build/linux/monolith", d), X_OK) != 0) continue;
            char *hash = ToolReadTrimmed(marker);
            bool same = hash && strcmp(hash, full) == 0;
            free(hash);
            if (!same) continue;
            long value = strtol(n, NULL, 10);
            if (reuseN < 0 || value < reuseN) {
                reuseN = value;
                reuse = nob_temp_strdup(d);
            }
        }
        globfree(&g);
    }
    if (reuse) {
        ToolErr("reusing the finished build of %s in %s", h12, reuse);
        printf("%s/build/linux/monolith\n", reuse);
        return 0;
    }

    // A fresh name: nothing called <base>-<n> (file, folder or dangling link) and no <base>-<n>.log
    long n = 1;
    for (;; n++) {
        struct stat st;
        if (lstat(nob_temp_sprintf("%s-%ld", base, n), &st) == 0) continue;
        if (lstat(nob_temp_sprintf("%s-%ld.log", base, n), &st) == 0) continue;
        break;
    }
    const char *wt = nob_temp_sprintf("%s-%ld", base, n);
    gLog = nob_temp_sprintf("%s-%ld.log", base, n);
    // `: > "$LOG"`: create (or empty) the log
    int fd = open(gLog, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) Fail(3, "cannot create the log %s", gLog);
    close(fd);
    nob_da_append(&gCreated, gLog);
    ToolErr("building %s (%s) in %s (log: %s)", h12, full, wt, gLog);

    // The worktree, detached at the commit (git's output into the log)
    nob_cmd_append(&cmd, "git", "-C", mainRepo, "worktree", "add", "--detach", wt, full);
    if (ToolRun(&cmd, (ToolIo){.out = gLog, .append = true, .errToOut = true}) != 0) {
        struct stat st;
        if (stat(wt, &st) == 0) nob_da_append(&gCreated, wt);
        Fail(3, "git worktree add failed");
    }
    nob_da_append(&gCreated, wt);
    nob_da_append(&gCreated, nob_temp_sprintf("worktree record %s/worktrees/%s", common, strrchr(wt, '/') + 1));

    // R3D: initialised from local objects when the commit has submodules/r3d as a submodule
    char *r3dSha = Gitlink(mainRepo, full, "submodules/r3d");
    if (*r3dSha) {
        ToolErr("submodules/r3d is a submodule at %s: initialising it from local objects", r3dSha);
        const char *dst = nob_temp_sprintf("%s/submodules/r3d", wt);
        const char *sources[] = {nob_temp_sprintf("%s/modules/submodules/r3d", common),
                                 nob_temp_sprintf("%s/submodules/r3d", mainRepo), NULL};
        char *r3dSrc = InitGitlink(dst, r3dSha, sources);
        if (!r3dSrc) Fail(3, "no local repository has submodules/r3d at %s (no network is used)", r3dSha);

        // R3D's own submodules (raylib, assimp, ... when they are submodules at that commit): every 160000 entry
        // of `git ls-tree -r HEAD` in the new clone
        nob_cmd_append(&cmd, "git", "-C", dst, "ls-tree", "-r", "HEAD");
        Nob_String_Builder tree = {0};
        ToolCapture(&cmd, &tree, (ToolIo){0});
        nob_sb_append_null(&tree);
        char *r3dGitDir;
        nob_cmd_append(&cmd, "git", "-C", r3dSrc, "rev-parse", "--absolute-git-dir");
        Git(&r3dGitDir, NULL, &cmd);
        for (char *line = tree.items; line && *line;) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            // `read -r mode _ sha path`: three blank-separated words, the rest of the line is the path
            char *words[3] = {0};
            char *p = line;
            int count = 0;
            for (; count < 3; count++) {
                while (*p == ' ' || *p == '\t') p++;
                if (!*p) break;
                words[count] = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                if (*p) *p++ = '\0';
            }
            while (*p == ' ' || *p == '\t') p++;
            char *path = p;
            size_t len = strlen(path);
            while (len > 0 && (path[len - 1] == ' ' || path[len - 1] == '\t')) path[--len] = '\0';
            if (count == 3 && strcmp(words[0], "160000") == 0) {
                const char *subSources[] = {nob_temp_sprintf("%s/modules/%s", r3dSrc, path),
                                            nob_temp_sprintf("%s/modules/%s", r3dGitDir, path),
                                            nob_temp_sprintf("%s/submodules/r3d/%s", mainRepo, path), NULL};
                char *used = InitGitlink(nob_temp_sprintf("%s/%s", dst, path), words[2], subSources);
                if (!used) {
                    Fail(3, "no local repository has submodules/r3d/%s at %s (no network is used)", path, words[2]);
                }
                free(used);
            }
            line = nl ? nl + 1 : NULL;
        }
        nob_sb_free(tree);
        free(r3dGitDir);
        free(r3dSrc);
    } else {
        ToolErr("submodules/r3d is a plain tree at %s: nothing to initialise", h12);
    }

    struct stat st;
    if (stat(nob_temp_sprintf("%s/build.sh", wt), &st) != 0 || !S_ISREG(st.st_mode)) {
        Fail(4, "the commit has no build.sh");
    }

    // The rm stand-ins: rm, rmdir and unlink in the shim folder are symlinks to this program, which only logs
    // when called by those names (DeleteStandIn): the old build.sh deletes nothing
    const char *shim = nob_temp_sprintf("%s/.old-monolith-shim", wt);
    if (!ToolMkdirP(shim)) Fail(3, "cannot create %s", shim);
    const char *standIns[] = {"rm", "rmdir", "unlink"};
    for (size_t i = 0; i < NOB_ARRAY_LEN(standIns); i++) {
        const char *link = nob_temp_sprintf("%s/%s", shim, standIns[i]);
        if (symlink(gToolSelf, link) != 0) Fail(3, "cannot create %s", link);
    }

    // The commit's own build, timed in whole seconds (as bash's $SECONDS)
    ToolErr("running the commit's ./build.sh (the first build of a commit builds R3D: a few minutes)");
    time_t start = time(NULL);
    int status = RunOldBuild(wt, shim);
    long elapsed = (long)(time(NULL) - start);
    FILE *log = fopen(gLog, "a");
    if (log) {
        fprintf(log, "old-monolith: build exit=%d secs=%ld\n", status, elapsed);
        fclose(log);
    }
    if (status != 0) Fail(4, "./build.sh exited with %d after %lds", status, elapsed);
    const char *bin = nob_temp_sprintf("%s/build/linux/monolith", wt);
    if (access(bin, X_OK) != 0) Fail(4, "./build.sh succeeded but %s does not exist", bin);

    // The marker that lets the next call reuse this build
    const char *marker = nob_temp_sprintf("%s/.old-monolith-done", wt);
    const char *line = nob_temp_sprintf("%s\n", full);
    if (!nob_write_entire_file(marker, line, strlen(line))) Fail(4, "cannot write %s", marker);
    ToolErr("built %s in %lds", h12, elapsed);
    LeftBehind();
    printf("%s\n", bin);
    return 0;
}
