// tests/unit/run_sh_test.c: the test of tools/run-sh.c (build/run-sh.o), run by `./build.sh test run_sh`
// (nob.c, region "Unit tests"), which builds run-sh.c twice into build/unit/run_sh/ (run-sh.o with the
// flags of `./build.sh --tools`, run-sh-san.o with ASan + UBSan) and calls
//
//     build/unit/run_sh/run_sh_test <run-sh.o> <run-sh-san.o> <out folder> <repo root>
//
// What it checks:
//   1. The rules table: every command line in CASES goes through `run-sh.o --check` (it never runs them)
//      with the repo root as the working directory, with both builds; the decision (allowed / refused, and
//      the rule's name in the refusal) must be the expected one. The table covers each rule's typical
//      refusals and allowed cases, and the commands sessions run every day (the must-pass list); it has no
//      rows for evasions (nested shells, interpreters, more wrappers): run-sh.o guards against ordinary
//      mistakes and doesn't look for them (tools/run-sh.c, "What it doesn't see"). The sanitizer build's runs fail on any
//      leak, UBSan or ASan report (LeakSanitizer's exit code differs from 0 / 126), so "zero leaks" covers
//      run-sh.o itself. <scratch> in a case is a folder under /tmp/claude-<uid>/ (a session scratchpad's
//      place), which holds a real symlink to the repo (link-to-repo) for the ways out of /tmp.
//   2. -c passes everything through: `-c 'exit 7'` exits 7, stdin reaches the command, the arguments
//      after the command are $0 $1, a refused command exits 126 with the refusal on stderr and runs
//      nothing, `-c 'sleep 30'` stops on SIGTERM and on SIGINT.
//   3. The benchmark lock: a --bench run (a fake game: <scratch>/fakebin/monolith, a link to `true`)
//      holds the lock until its shell ends; a second one at the same time is refused, and runs once the
//      first has ended.
//   4. The log: every decision above is in <out>/state/log (RUN_SH_DIR points run-sh.o there, so the real
//      build/run-sh/log is untouched).
//   5. The hook: JSON on stdin, the exact answer on stdout (plain, already wrapped, run_in_background and
//      timeout kept, single quotes, JSON escapes, Monitor, a WebSocket Monitor, a refused command, the
//      permission modes that only check, --rewrite all, input that isn't JSON).
// Exit 0 when everything passed, 1 otherwise (each failure printed with what came back).
//
// The scratch folder /tmp/claude-<uid>/run-sh-test/ is reused between runs and never deleted (nothing in
// this repo deletes with rm; it is under /tmp, which the system empties).

#ifndef _GNU_SOURCE  // (UNIT_FLAGS passes -D_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// #region Small helpers: heap strings, running a program

// A growing heap string.
typedef struct {
    char *s;
    size_t n, cap;
} Buf;

static void BufAdd(Buf *b, const char *p, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char *s = realloc(b->s, cap);
        if (!s) {
            fprintf(stderr, "run_sh_test: out of memory\n");
            exit(1);
        }
        b->s = s;
        b->cap = cap;
    }
    memcpy(b->s + b->n, p, n);
    b->n += n;
    b->s[b->n] = '\0';
}

static void BufAddC(Buf *b, const char *p)
{
    BufAdd(b, p, strlen(p));
}

// printf into a new heap string (free it).
__attribute__((format(printf, 1, 2))) static char *Sprintf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char *s = NULL;
    if (vasprintf(&s, fmt, args) < 0) s = NULL;
    va_end(args);
    if (!s) {
        fprintf(stderr, "run_sh_test: out of memory\n");
        exit(1);
    }
    return s;
}

// `s` with every `from` replaced by `to` (a new heap string).
static char *Replace(const char *s, const char *from, const char *to)
{
    Buf b = {0};
    size_t fl = strlen(from);
    for (const char *p = s;;) {
        const char *at = strstr(p, from);
        if (!at) {
            BufAddC(&b, p);
            break;
        }
        BufAdd(&b, p, (size_t)(at - p));
        BufAddC(&b, to);
        p = at + fl;
    }
    if (!b.s) BufAdd(&b, "", 0);
    return b.s;
}

// What a program run returned: its exit code (128 + n when killed by signal n), stdout and stderr.
typedef struct {
    int code;
    Buf out, err;
} Result;

static void ResultFree(Result *r)
{
    free(r->out.s);
    free(r->err.s);
}

// Reads a pipe to its end into a buffer.
static void Drain(int fd, Buf *b)
{
    char tmp[4096];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        BufAdd(b, tmp, (size_t)n);
    }
    if (!b->s) BufAdd(b, "", 0);
}

// Starts argv in `cwd` with `input` on stdin (NULL: /dev/null) and its stdout / stderr on pipes (when
// outFd / errFd are given). Signal dispositions are reset, so SIGINT reaches the child as in a terminal.
static pid_t Start(char *const *argv, const char *cwd, int *inFd, int *outFd, int *errFd)
{
    int in[2], out[2], err[2];
    if (pipe(in) || pipe(out) || pipe(err)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(err[1], 2);
        close(in[0]), close(in[1]), close(out[0]), close(out[1]), close(err[0]), close(err[1]);
        if (cwd && chdir(cwd) != 0) _exit(125);
        execv(argv[0], argv);
        _exit(127);
    }
    close(in[0]), close(out[1]), close(err[1]);
    *inFd = in[1];
    *outFd = out[0];
    *errFd = err[0];
    return pid;
}

static int StatusCode(int ws)
{
    return WIFEXITED(ws) ? WEXITSTATUS(ws) : WIFSIGNALED(ws) ? 128 + WTERMSIG(ws) : 1;
}

// Runs argv to its end with `input` on stdin and returns its code and output.
static Result Run(char *const *argv, const char *cwd, const char *input)
{
    Result r = {0};
    int in, out, err;
    pid_t pid = Start(argv, cwd, &in, &out, &err);
    if (pid < 0) {
        r.code = -1;
        return r;
    }
    if (input) {
        size_t n = strlen(input), done = 0;
        while (done < n) {
            ssize_t w = write(in, input + done, n - done);
            if (w <= 0) break;
            done += (size_t)w;
        }
    }
    close(in);
    // stderr is small (one line); stdout is read first, then stderr
    Drain(out, &r.out);
    Drain(err, &r.err);
    close(out);
    close(err);
    int ws;
    while (waitpid(pid, &ws, 0) < 0 && errno == EINTR) {}
    r.code = StatusCode(ws);
    return r;
}

// #endregion

// #region The test's state

static const char *gPlain, *gSan, *gOut, *gRoot;
static char *gScratch;  // /tmp/claude-<uid>/run-sh-test
static int gPassed, gFailed;

// Records one check; prints the failure with what came back.
__attribute__((format(printf, 2, 3))) static void Expect(bool ok, const char *fmt, ...)
{
    if (ok) {
        gPassed++;
        return;
    }
    gFailed++;
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "FAIL ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

// mkdir -p for the scratch folders.
static void MkdirP(const char *path)
{
    char *p = strdup(path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        mkdir(p, 0700);
        *s = '/';
    }
    mkdir(p, 0700);
    free(p);
}

// #endregion

// #region 1. The rules table

// One command line and its expected decision: `rule` is NULL for "allowed", else the rule's name the
// refusal must name. <scratch> and <root> are replaced before the check.
typedef struct {
    const char *cmd;
    const char *rule;
} Case;

#define DEL "no deletion outside /tmp"
#define LOST "no lost work"
#define WIN "no visible window"
#define SCREEN "never touch the screen"
#define BENCH "benchmarks"
#define STAY "stay in the repo"
#define OK NULL

static const Case CASES[] = {
    // --- no deletion outside /tmp: the commands, in the repo
    {"rm x", DEL},
    {"rm -rf src", DEL},
    {"rmdir build", DEL},
    {"unlink x", DEL},
    {"shred -u x", DEL},
    {"git clean -fd", DEL},
    {"git clean -fdx", DEL},
    {"git rm x", DEL},
    {"git rm -r src", DEL},
    // the command word: by basename, unquoted, after NAME=value, keywords and one wrapper
    {"/bin/rm x", DEL},
    {"\\rm x", DEL},
    {"'rm' x", DEL},
    {"FOO=1 rm x", DEL},
    {"command rm x", DEL},
    {"exec rm x", DEL},
    {"env rm x", DEL},
    {"env -i FOO=1 rm x", DEL},
    {"nice -n 5 rm x", DEL},
    {"nohup rm x", DEL},
    {"timeout 5 rm x", DEL},
    {"timeout -s KILL 5 rm x", DEL},
    {"time rm x", DEL},
    {"sudo rm x", DEL},
    {"sudo -u root rm x", DEL},
    {"xargs rm < list", DEL ": xargs rm; "},
    {"ls | xargs rm", DEL},
    {"ls | xargs -0 rm -f", DEL},
    {"echo /tmp/x | xargs rm", DEL},
    {"if true; then rm x; fi", DEL},
    {"for f in a; do rm x; done", DEL},
    {"f() { rm x; }; f", DEL},
    {"case a in a) rm x;; esac", DEL},
    // every separator starts a new command
    {"true && rm x", DEL},
    {"false || rm x", DEL},
    {"ls; rm x", DEL},
    {"echo a | rm x", DEL},
    {"sleep 1 & rm x", DEL},
    {"ls\nrm x", DEL},
    {"echo a \\\n; rm x", DEL},
    {"(rm x)", DEL},
    {"echo $(rm x)", DEL},
    {"echo `rm x`", DEL},
    {"x=$(rm y)", DEL},
    {"cat <(rm x)", DEL},
    // paths it can't check, and the ways out of /tmp
    {"rm -rf /tmp/../home/x", DEL},
    {"rm <scratch>/link-to-repo/file", DEL},
    {"rm -rf <scratch>/link-to-repo/", DEL},
    {"rm -rf <scratch>/link-to-repo/..", DEL},
    {"cd /tmp && rm -rf ../x", DEL},
    {"cd /tmp && rm -rf ../home/x", DEL},
    {"cd <scratch>/link-to-repo && rm -rf src", DEL},
    {"(cd /tmp && rm -rf y); rm -rf z", DEL ": rm z; "},
    {"echo $(cd /tmp); rm -rf z", DEL ": rm z; "},
    {"cd; rm -rf x", DEL},
    {"cd \"$D\" && rm -rf x", DEL},
    {"cd - && rm x", DEL},
    {"rm -rf /tmp", DEL},
    {"rm -rf /tmp/", DEL},
    {"rm -rf /tmp/.", DEL},
    {"rm -rf /tmp/a src", DEL},
    {"rm -rf /tmp/a \"$HOME/b\"", DEL},
    {"rm -rf \"$D\"", DEL ": rm $D; run-sh.o can't check"},
    {"rm -rf $D/x", DEL},
    {"rm -rf /tmp/$(echo x)", DEL},
    {"rm -rf /tmp/*", DEL ": rm /tmp/*; run-sh.o can't check"},
    {"rm -rf /tmp/vfy*", DEL},
    {"rm -f <scratch>/*.png", DEL},
    {"rm -rf /tmp/{a,../home}", DEL},
    {"rm -rf ~/x", DEL},
    {"find . -delete", DEL},
    {"find . -name '*.o' -delete", DEL},
    {"find src -exec rm {} \\;", DEL},
    {"find . -execdir rm {} +", DEL},
    {"find -L src -type f -delete", DEL},
    {"find <scratch>/link-to-repo -delete", DEL},
    {"git worktree remove ../wt", DEL},
    {"git worktree remove <scratch>/link-to-repo", DEL},
    {"git -C /tmp worktree remove ../home/x", DEL},
    // --- no lost work
    {"git reset --hard", LOST},
    {"git reset --hard HEAD~1", LOST},
    {"git checkout -- src/main.c", LOST},
    {"git checkout .", LOST},
    {"git checkout HEAD -- .", LOST},
    {"git checkout -f main", LOST},
    {"git restore src/main.c", LOST},
    {"git stash drop", LOST},
    {"git stash clear", LOST},
    {"git -C /tmp/x checkout -- y", LOST},
    {"git --no-pager reset --hard", LOST},
    {"cd src && git reset --hard", LOST},
    // --- no visible window
    {"./build/linux/monolith", WIN},
    {"build/linux/monolith --view 2", WIN},
    {"<root>/build/linux/monolith --map maps/x", WIN},
    {"./build.sh run", WIN},
    {"./build.sh run --view 2", WIN},
    {"./build.sh --win run", WIN},
    {"build/nob run", WIN},
    {"wine build/win/monolith.exe", WIN},
    {"/tmp/old-monolith-abc/build/linux/monolith --map maps/x", WIN},
    {"./build/linux/monolith --bench-window 4", WIN},
    {"nohup ./build/linux/monolith &", WIN},
    {"timeout 10 ./build/linux/monolith --view 1", WIN},
    {"MONOLITH_ALLOW_WINDOW=1 ./build/linux/monolith --shot x.png", WIN},
    {"MONOLITH_ALLOW_WINDOW=1", WIN},
    {"export MONOLITH_ALLOW_WINDOW=1", WIN},
    {"export MONOLITH_ALLOW_WINDOW", WIN},
    {"declare -x MONOLITH_ALLOW_WINDOW=1", WIN},
    {"env MONOLITH_ALLOW_WINDOW=1 ls", WIN},
    {"examples/build/linux/01_minimal/minimal", WIN},
    {"./examples/build/linux/01_minimal/minimal --shot x.png", WIN},
    {"cd examples && build/linux/01_minimal/minimal", WIN},
    {"wine examples/build/win/01_minimal/minimal.exe", WIN},
    // --- never touch the screen
    {"i3-msg workspace 2", SCREEN},
    {"i3-msg focus left", SCREEN},
    {"i3-msg -t run_command 'workspace 3'", SCREEN},
    {"i3-msg -t get_tree workspace 3", SCREEN},
    {"swaymsg -t command workspace 1", SCREEN},
    {"swaymsg 'move container to workspace 2'", SCREEN},
    {"xdotool key super+1", SCREEN},
    {"wmctrl -s 1", SCREEN},
    // --- benchmarks
    {"./build/linux/monolith --bench --headless --note x", BENCH},
    {"__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --note x", BENCH},
    {"__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless", BENCH},
    {"__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless --note", BENCH},
    {"__GL_SYNC_TO_VBLANK=1 ./build/linux/monolith --bench --headless --note x", BENCH},
    {"__GL_SYNC_TO_VBLANK=0 true; ./build/linux/monolith --bench --headless --note x", BENCH},  // only for true
    {"examples/build/linux-test/01_minimal/minimal --bench r.md maps/x", BENCH},
    // --- stay in the repo
    {"echo x > /etc/foo", STAY},
    {"echo x >/etc/foo", STAY},
    {"echo x >> ~/notes.txt", STAY},
    {"echo x 2> /etc/foo", STAY},
    {"echo x &> /etc/foo", STAY},
    {"echo x >& /etc/foo", STAY},
    {"cp a /usr/local/bin/", STAY},
    {"cp -r a b /opt", STAY},
    {"mv x /home/other", STAY},
    {"echo x | tee /etc/x", STAY},
    {"echo x | tee -a /etc/x", STAY},
    {"mkdir /opt/x", STAY},
    {"mkdir -p -m 700 /opt/x", STAY},
    {"touch /var/tmp/x", STAY},
    {"cp a <scratch>/link-to-repo/../outside", STAY},

    // --- allowed
    // the must-pass list
    {"gio trash x", OK},
    {"gio trash src/old.c build/x", OK},
    {"rm -rf /tmp/monolith-scratch-1", OK},
    {"rm <scratch>/out.png", OK},
    {"find /tmp/x -delete", OK},
    {"./build.sh lsan > /dev/null", OK},
    {"grep -rn rm src", OK},
    {"echo \"rm -rf /\"", OK},
    {"echo \"a; rm x\"", OK},
    {"git rm --cached x", OK},
    {"__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless --bench-frames 150 --note \"screen: what changed\"", OK},
    {"__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless --note \"what changed\"", OK},
    {"./build/linux/monolith --shot /tmp/x.png --view 2", OK},
    {"i3-msg -t get_tree", OK},
    // deletions under /tmp
    {"rm -rf <scratch>/a <scratch>/b", OK},
    {"rm -rf <scratch>/link-to-repo", OK},  // the link itself, not its target
    {"rm -rf /tmp/x", OK},
    {"rm -rf -- /tmp/x", OK},
    {"rmdir /tmp/empty-dir", OK},
    {"find <scratch> -name '*.png' -delete", OK},
    {"find /tmp/x -exec rm {} \\;", OK},
    {"find /tmp/x -type f -exec rm -f {} +", OK},
    {"git worktree remove /tmp/wt-1", OK},
    {"git worktree remove --force /tmp/wt-1", OK},
    {"cd /tmp && rm -rf ./monolith-x", OK},
    {"cd <scratch> && rm out.png", OK},
    {"mkdir -p /tmp/run-sh-test-new-zz && cd /tmp/run-sh-test-new-zz && rm -rf y", OK},
    {"sudo -u root ls", OK},
    {"ln -s a b", OK},
    // the build, git, reading
    {"./build.sh", OK},
    {"./build.sh --win", OK},
    {"./build.sh --tools run-sh", OK},
    {"./build.sh test run_sh", OK},
    {"grep -rn 'rm -rf' docs", OK},
    {"echo 'git reset --hard'", OK},
    {"echo 'rm -rf /' | grep rm", OK},
    {"cat <<EOF\nrm -rf /\nEOF", OK},
    {"cat <<'EOF' > <scratch>/x.txt\nrm -rf $(oops)\nEOF", OK},
    {"cat <<-EOF\n\trm -rf /\n\tEOF\necho done", OK},
    {"git rm -r --cached build", OK},
    {"git status", OK},
    {"git diff --stat", OK},
    {"git checkout main", OK},
    {"git checkout -b feature", OK},
    {"git restore --staged x", OK},
    {"git stash", OK},
    {"git stash pop", OK},
    {"git clean -n", OK},
    {"git clean -nd", OK},
    {"git reset --soft HEAD~1", OK},
    {"git log --oneline | head -5", OK},
    // benchmarks and windowless runs
    {"vblank_mode=0 ./build/linux/monolith --bench --headless --note x", OK},
    {"export __GL_SYNC_TO_VBLANK=0; ./build/linux/monolith --bench --headless --note x", OK},
    {"env __GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless --note=x", OK},
    {"__GL_SYNC_TO_VBLANK=0 examples/build/linux-test/01_minimal/minimal --bench r.md maps/x", OK},
    {"./build/linux/monolith --shot x.png --view 2 --off 16", OK},
    {"./build/linux/monolith --dump-boxes", OK},
    {"./build/linux/monolith --map maps/x --dump-scene-diff a.map", OK},
    {"./build/linux/monolith --map maps/x --apply-edits e.txt", OK},
    {"./build/linux/monolith --inspect-model m.glb", OK},
    {"./build/linux/monolith --new-map test", OK},
    {"./build/linux/monolith --export /tmp/e --map tests/models", OK},
    {"./build/linux/monolith --map maps/x --compile /tmp/x.mapb", OK},
    {"./build/linux/monolith --classify 'make it red'", OK},
    {"./build/linux/monolith --bake-surface maps/x", OK},
    {"./build.sh run --shot <scratch>/a.png", OK},
    {"timeout 60 ./build/linux/monolith --shot /tmp/x.png", OK},
    {"wine build/win/monolith.exe --shot win.png --view 2", OK},
    {"examples/build/linux-test/01_minimal/minimal --shot /tmp/x.png --view 2", OK},
    {"examples/build/linux-lsan/01_minimal/minimal --shot /tmp/x.png", OK},
    {"i3-msg -t get_workspaces", OK},
    {"swaymsg -t get_outputs", OK},
    {"i3-msg -t get_tree | head -c 100", OK},
    {"grep -rn MONOLITH_ALLOW_WINDOW src", OK},
    {"echo MONOLITH_ALLOW_WINDOW", OK},
    // writes in the repo, /tmp and the devices
    {"echo hi > /dev/null 2>&1", OK},
    {"ls >&2", OK},
    {"ls 2>&1 | tee build/log.txt", OK},
    {"echo x > build/x.txt", OK},
    {"echo x > <root>/build/x.txt", OK},
    {"echo x > <scratch>/notes.txt", OK},
    {"echo x > \"$OUT/x\"", OK},
    {"cp src/a.c /tmp/x/", OK},
    {"cp -r maps/x <scratch>/", OK},
    {"mv build/x <scratch>/", OK},
    {"touch build/stamp", OK},
    {"mkdir -p <scratch>/a/b", OK},
    // the rest
    {"python3 -c 'print(1)'", OK},
    {"bash -c 'echo hi'", OK},
    {"command -v rm", OK},
    {"which rm", OK},
    {"ls -la | grep -c rm", OK},
    {"find . -name '*.c' | head", OK},
    {"find src -name '*.c' -exec grep -l rm {} +", OK},
    {"xargs grep -l x < list", OK},
    {"sleep 1 &", OK},
    {"(cd /tmp && ls)", OK},
    {"f() { echo hi; }; f", OK},
    {"for f in a b; do echo $f; done", OK},
    {"case x in a) echo a;; *) echo b;; esac", OK},
    {"[[ -f x ]] && echo y", OK},
    {"echo {a,b} $((1 + 2)) ${HOME}", OK},
    {"<root>/build/run-sh.o -c 'ls'", OK},
    {"<root>/build/run-sh.o --check 'rm x'", OK},
    {"# rm -rf /", OK},
    {"echo a # rm -rf /", OK},
    {"echo 'unterminated", OK},
    {"", OK},
};

// Runs every case through `--check` with both builds and compares the decision.
static void TestRules(void)
{
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char *a = Replace(CASES[i].cmd, "<scratch>", gScratch);
        char *cmd = Replace(a, "<root>", gRoot);
        free(a);
        const char *bins[] = {gPlain, gSan};
        for (size_t b = 0; b < 2; b++) {
            char *argv[] = {(char *)bins[b], "--check", cmd, NULL};
            Result r = Run(argv, gRoot, NULL);
            bool refused = r.code == 126;
            bool ok;
            if (!CASES[i].rule) {
                ok = r.code == 0 && strcmp(r.out.s, "run-sh: allowed\n") == 0;
            } else {
                // a rule with ": " in it is a longer prefix of the message (the word and the hint too)
                char *want = Sprintf(strstr(CASES[i].rule, ": ") ? "run-sh: refused: %s" : "run-sh: refused: %s: ",
                                     CASES[i].rule);
                ok = refused && strncmp(r.out.s, want, strlen(want)) == 0;
                free(want);
            }
            Expect(ok, "[%s] %s\n     expected %s, got exit %d: %s%s", b ? "san" : "plain", cmd,
                   CASES[i].rule ? CASES[i].rule : "allowed", r.code, r.out.s, r.err.s);
            ResultFree(&r);
        }
        free(cmd);
    }
}

// #endregion

// #region 2. -c passes everything through

// Starts `run-sh.o -c 'sleep 30'`, sends `sig` after 300 ms and expects the process to end by it within
// five seconds (bash runs the single command in place, so the signal reaches sleep).
static void TestSignal(const char *bin, int sig, const char *name)
{
    char *argv[] = {(char *)bin, "-c", "sleep 30", NULL};
    int in, out, err;
    pid_t pid = Start(argv, gRoot, &in, &out, &err);
    close(in);
    struct timespec pause = {0, 300 * 1000 * 1000};
    nanosleep(&pause, NULL);
    kill(pid, sig);
    int ws = 0;
    bool ended = false;
    for (int i = 0; i < 500 && !ended; i++) {
        pid_t w = waitpid(pid, &ws, WNOHANG);
        if (w == pid) ended = true;
        else nanosleep(&(struct timespec){0, 10 * 1000 * 1000}, NULL);
    }
    if (!ended) {
        kill(pid, SIGKILL);
        waitpid(pid, &ws, 0);
    }
    close(out);
    close(err);
    Expect(ended && WIFSIGNALED(ws) && WTERMSIG(ws) == sig, "-c 'sleep 30' with %s: ended %d, status %d", name,
           ended, StatusCode(ws));
}

static void TestPassThrough(void)
{
    const char *bins[] = {gPlain, gSan};
    for (size_t b = 0; b < 2; b++) {
        const char *bin = bins[b];
        // the exit code
        char *exit7[] = {(char *)bin, "-c", "exit 7", NULL};
        Result r = Run(exit7, gRoot, NULL);
        Expect(r.code == 7, "-c 'exit 7' exited %d (%s)", r.code, r.err.s);
        ResultFree(&r);
        // stdin and stdout
        char *cat[] = {(char *)bin, "-c", "cat", NULL};
        r = Run(cat, gRoot, "hello stdin\nline 2\n");
        Expect(r.code == 0 && strcmp(r.out.s, "hello stdin\nline 2\n") == 0, "-c 'cat': exit %d, out '%s'", r.code,
               r.out.s);
        ResultFree(&r);
        // stderr, and the arguments after the command are $0 $1
        char *args[] = {(char *)bin, "-c", "echo \"$0 $1\"; echo err >&2", "a", "b", NULL};
        r = Run(args, gRoot, NULL);
        Expect(r.code == 0 && strcmp(r.out.s, "a b\n") == 0 && strcmp(r.err.s, "err\n") == 0,
               "-c with arguments: '%s' '%s'", r.out.s, r.err.s);
        ResultFree(&r);
        // a refused command runs nothing: 126 and the refusal on stderr (the marker's name is new on every
        // run, so nothing has to be deleted). This command really runs if run-sh.o wrongly lets it through,
        // so its deletion must be refused wherever the checkout is (a relative `rm -rf src` is allowed in a
        // checkout under /tmp, and it once deleted a scratch worktree's src/) and harmless if it ran: an
        // absolute path outside /tmp that doesn't exist.
        char *marker = Sprintf("%s/refused-ran-%ld-%ld-%zu", gScratch, (long)getpid(), (long)time(NULL), b);
        char *cmd = Sprintf("touch %s; rm -rf /nonexistent-run-sh-test/src", marker);
        char *refused[] = {(char *)bin, "-c", cmd, NULL};
        r = Run(refused, gRoot, NULL);
        struct stat st;
        Expect(r.code == 126 && strncmp(r.err.s, "run-sh: refused: no deletion outside /tmp: ", 43) == 0 &&
                   r.out.n == 0 && stat(marker, &st) != 0,
               "-c refused: exit %d, err '%s'", r.code, r.err.s);
        ResultFree(&r);
        free(cmd);
        free(marker);
        // the signals
        TestSignal(bin, SIGTERM, "SIGTERM");
        TestSignal(bin, SIGINT, "SIGINT");
    }
}

// #endregion

// #region 3. The benchmark lock

static void TestBenchLock(void)
{
    // a fake game: the lock is about the command line, not what runs
    char *fakebin = Sprintf("%s/fakebin", gScratch);
    char *fake = Sprintf("%s/monolith", fakebin);
    MkdirP(fakebin);
    struct stat st;
    if (lstat(fake, &st) != 0 && symlink("/bin/true", fake) != 0) {
        Expect(false, "can't make %s: %s", fake, strerror(errno));
    }
    char *first = Sprintf("__GL_SYNC_TO_VBLANK=0 %s --bench --headless --note first && sleep 2", fake);
    char *second = Sprintf("__GL_SYNC_TO_VBLANK=0 %s --bench --headless --note second", fake);

    // the first holds the lock while its shell runs (sleep 2)
    char *argv1[] = {(char *)gPlain, "-c", first, NULL};
    int in, out, err;
    pid_t pid = Start(argv1, gRoot, &in, &out, &err);
    close(in);
    nanosleep(&(struct timespec){0, 500 * 1000 * 1000}, NULL);
    // a second one meanwhile is refused (with either build)
    const char *bins[] = {gPlain, gSan};
    for (size_t b = 0; b < 2; b++) {
        char *argv2[] = {(char *)bins[b], "-c", second, NULL};
        Result r = Run(argv2, gRoot, NULL);
        Expect(r.code == 126 && strstr(r.err.s, "another benchmark is running"), "second bench while one runs: exit %d, '%s'",
               r.code, r.err.s);
        ResultFree(&r);
    }
    int ws;
    waitpid(pid, &ws, 0);
    close(out);
    close(err);
    Expect(StatusCode(ws) == 0, "the first bench exited %d", StatusCode(ws));
    // once it ended, the next one runs
    char *argv3[] = {(char *)gSan, "-c", second, NULL};
    Result r = Run(argv3, gRoot, NULL);
    Expect(r.code == 0, "bench after the first ended: exit %d, '%s'", r.code, r.err.s);
    ResultFree(&r);
    free(first);
    free(second);
    free(fake);
    free(fakebin);
}

// #endregion

// #region 4. The log

static void TestLog(void)
{
    char *path = Sprintf("%s/state/log", gOut);
    FILE *f = fopen(path, "r");
    Expect(f != NULL, "the log %s is missing", path);
    if (f) {
        Buf b = {0};
        char tmp[4096];
        size_t n;
        while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) BufAdd(&b, tmp, n);
        fclose(f);
        Expect(b.s && strstr(b.s, "\t-c\tallowed\t") && strstr(b.s, "\t--check\trun-sh: refused: ") &&
                   strstr(b.s, "\t--hook Bash\t") && strstr(b.s, "cat <<EOF\\nrm -rf /\\nEOF"),
               "the log lacks the expected lines");
        free(b.s);
    }
    free(path);
}

// #endregion

// #region 5. The hook

// A command quoted for the shell ('…', ' as '\'').
static char *ShellQuote(const char *s)
{
    Buf b = {0};
    BufAddC(&b, "'");
    for (; *s; s++) {
        if (*s == '\'') BufAddC(&b, "'\\''");
        else BufAdd(&b, s, 1);
    }
    BufAddC(&b, "'");
    return b.s;
}

// A string as JSON text (", \, newline, tab escaped).
static char *JsonQuote(const char *s)
{
    Buf b = {0};
    BufAddC(&b, "\"");
    for (; *s; s++) {
        if (*s == '"') BufAddC(&b, "\\\"");
        else if (*s == '\\') BufAddC(&b, "\\\\");
        else if (*s == '\n') BufAddC(&b, "\\n");
        else if (*s == '\t') BufAddC(&b, "\\t");
        else BufAdd(&b, s, 1);
    }
    BufAddC(&b, "\"");
    return b.s;
}

// The rewritten command for `command`, as a JSON string.
static char *Wrapped(const char *command)
{
    char *q = ShellQuote(command);
    char *c = Sprintf("%s/build/run-sh.o -c %s", gRoot, q);
    char *j = JsonQuote(c);
    free(q);
    free(c);
    return j;
}

// Feeds `input` to `run-sh.o --hook [extra]` (both builds) and compares stdout with `want` exactly.
static void Hook(const char *name, const char *input, const char *want, int wantCode, const char *extra)
{
    const char *bins[] = {gPlain, gSan};
    for (size_t b = 0; b < 2; b++) {
        char *argv[] = {(char *)bins[b], "--hook", extra ? "--rewrite" : NULL, (char *)extra, NULL};
        Result r = Run(argv, gRoot, input);
        Expect(r.code == wantCode && strcmp(r.out.s, want) == 0,
               "hook [%s] %s: exit %d\n     got:  %s\n     want: %s\n     err: %s", b ? "san" : "plain", name, r.code,
               r.out.s, want, r.err.s);
        ResultFree(&r);
    }
}

static void TestHook(void)
{
    const char *pre = "{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"updatedInput\":{";
    const char *deny = "{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"permissionDecision\":\"deny\","
                       "\"permissionDecisionReason\":";
    char *in, *want, *w;

    // plain: the command rewritten, the description kept
    in = Sprintf("{\"session_id\":\"s\",\"cwd\":\"%s\",\"permission_mode\":\"bypassPermissions\",\"hook_event_name\":"
                 "\"PreToolUse\",\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"ls -la src\",\"description\":"
                 "\"List src\"},\"tool_use_id\":\"t1\"}", gRoot);
    w = Wrapped("ls -la src");
    want = Sprintf("%s\"command\":%s,\"description\":\"List src\"}}}\n", pre, w);
    Hook("plain", in, want, 0, NULL);
    free(in), free(want), free(w);

    // run_in_background, timeout and a field the hook doesn't know: copied as written, in their order
    in = Sprintf("{\"cwd\":\"%s\",\"permission_mode\":\"bypassPermissions\",\"tool_name\":\"Bash\",\"tool_input\":"
                 "{\"description\":\"wait\",\"command\":\"sleep 5\",\"run_in_background\":true,\"timeout\": 600000,"
                 "\"future\":{\"a\":[1,2,{\"b\":null}]}}}", gRoot);
    w = Wrapped("sleep 5");
    want = Sprintf("%s\"description\":\"wait\",\"command\":%s,\"run_in_background\":true,\"timeout\":600000,"
                   "\"future\":{\"a\":[1,2,{\"b\":null}]}}}}\n", pre, w);
    Hook("run_in_background", in, want, 0, NULL);
    free(in), free(want), free(w);

    // single quotes, double quotes, a newline, a tab and \u escapes in the command
    in = Sprintf("{\"cwd\":\"%s\",\"permission_mode\":\"bypassPermissions\",\"tool_name\":\"Bash\",\"tool_input\":"
                 "{\"command\":\"echo 'it''s' \\\"q\\\"\\n\\tprintf '\\u00e9\\\\n'\"}}", gRoot);
    w = Wrapped("echo 'it''s' \"q\"\n\tprintf '\xc3\xa9\\n'");
    want = Sprintf("%s\"command\":%s}}}\n", pre, w);
    Hook("quotes and escapes", in, want, 0, NULL);
    free(in), free(want), free(w);

    // already wrapped (by this program's own path, or <repo>/build/run-sh.o): passed unchanged
    for (size_t b = 0; b < 2; b++) {
        const char *self = b == 0 ? gPlain : gSan;
        char *q = ShellQuote("ls");
        char *c = Sprintf("%s -c %s", self, q);
        char *j = JsonQuote(c);
        in = Sprintf("{\"cwd\":\"%s\",\"permission_mode\":\"bypassPermissions\",\"tool_name\":\"Bash\","
                     "\"tool_input\":{\"command\":%s}}", gRoot, j);
        char *argv[] = {(char *)self, "--hook", NULL};
        Result r = Run(argv, gRoot, in);
        Expect(r.code == 0 && r.out.n == 0, "hook already wrapped (%s): exit %d, '%s'", self, r.code, r.out.s);
        ResultFree(&r);
        free(in), free(q), free(c), free(j);
    }
    // wrapped, but more after it: wrapped again
    in = Sprintf("{\"cwd\":\"%s\",\"permission_mode\":\"bypassPermissions\",\"tool_name\":\"Bash\",\"tool_input\":"
                 "{\"command\":\"%s -c 'ls' && ls\"}}", gRoot, gPlain);
    char *more = Sprintf("%s -c 'ls' && ls", gPlain);
    w = Wrapped(more);
    want = Sprintf("%s\"command\":%s}}}\n", pre, w);
    Hook("wrapped and more", in, want, 0, NULL);
    free(in), free(want), free(w), free(more);

    // Monitor with a command: rewritten like Bash (Claude Code 2.1.323 applies it)
    in = Sprintf("{\"cwd\":\"%s\",\"permission_mode\":\"bypassPermissions\",\"tool_name\":\"Monitor\",\"tool_input\":"
                 "{\"description\":\"d\",\"timeout_ms\":10000,\"command\":\"tail -f build/x.log\"}}", gRoot);
    w = Wrapped("tail -f build/x.log");
    want = Sprintf("%s\"description\":\"d\",\"timeout_ms\":10000,\"command\":%s}}}\n", pre, w);
    Hook("Monitor", in, want, 0, NULL);
    free(in), free(want), free(w);

    // Monitor on a WebSocket, and another tool: passed
    Hook("Monitor ws", "{\"tool_name\":\"Monitor\",\"tool_input\":{\"ws\":{\"url\":\"wss://x\"},\"description\":\"d\","
                       "\"timeout_ms\":1000}}", "", 0, NULL);
    Hook("Read", "{\"tool_name\":\"Read\",\"tool_input\":{\"file_path\":\"/etc/passwd\"}}", "", 0, NULL);

    // refused: denied with the reason, in every mode
    const char *reason = "run-sh: refused: no deletion outside /tmp: rm src; move it to the trash with `gio trash "
                         "<path>` (rm and the other deletion commands only take literal paths under /tmp/, e.g. your "
                         "scratchpad)";
    char *jr = JsonQuote(reason);
    want = Sprintf("%s%s}}\n", deny, jr);
    const char *modes[] = {"bypassPermissions", "default", "acceptEdits", "auto"};
    for (size_t m = 0; m < 4; m++) {
        in = Sprintf("{\"cwd\":\"%s\",\"permission_mode\":\"%s\",\"tool_name\":\"Bash\",\"tool_input\":"
                     "{\"command\":\"rm -rf src\"}}", gRoot, modes[m]);
        Hook(modes[m], in, want, 0, NULL);
        free(in);
    }
    in = Sprintf("{\"cwd\":\"%s\",\"tool_name\":\"Monitor\",\"tool_input\":{\"command\":\"rm -rf src\"}}", gRoot);
    Hook("Monitor refused", in, want, 0, NULL);
    free(in), free(want), free(jr);

    // the hook's cwd is the session's: rm out.png is fine in the scratch folder
    in = Sprintf("{\"cwd\":\"%s\",\"permission_mode\":\"bypassPermissions\",\"tool_name\":\"Bash\",\"tool_input\":"
                 "{\"command\":\"rm out.png\"}}", gScratch);
    w = Wrapped("rm out.png");
    want = Sprintf("%s\"command\":%s}}}\n", pre, w);
    Hook("cwd from the JSON", in, want, 0, NULL);
    free(in), free(want), free(w);

    // the modes that ask: allowed commands pass unchanged, unless --rewrite all
    const char *asking[] = {"default", "acceptEdits", "plan"};
    for (size_t m = 0; m < 3; m++) {
        in = Sprintf("{\"cwd\":\"%s\",\"permission_mode\":\"%s\",\"tool_name\":\"Bash\",\"tool_input\":"
                     "{\"command\":\"git status\"}}", gRoot, asking[m]);
        Hook(asking[m], in, "", 0, NULL);
        w = Wrapped("git status");
        want = Sprintf("%s\"command\":%s}}}\n", pre, w);
        Hook("--rewrite all", in, want, 0, "all");
        free(in), free(want), free(w);
    }
    // the modes that never ask, and no mode at all: rewritten
    const char *unasked[] = {"auto", "dontAsk", NULL};
    for (size_t m = 0; m < 3; m++) {
        in = unasked[m] ? Sprintf("{\"cwd\":\"%s\",\"permission_mode\":\"%s\",\"tool_name\":\"Bash\",\"tool_input\":"
                                  "{\"command\":\"git status\"}}", gRoot, unasked[m])
                        : Sprintf("{\"cwd\":\"%s\",\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"git status\"}}",
                                  gRoot);
        w = Wrapped("git status");
        want = Sprintf("%s\"command\":%s}}}\n", pre, w);
        Hook(unasked[m] ? unasked[m] : "no mode", in, want, 0, NULL);
        free(in), free(want), free(w);
    }

    // input that isn't the tool call's JSON: exit 2 (the call is blocked)
    Hook("not JSON", "not json", "", 2, NULL);
    Hook("truncated", "{\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"ls", "", 2, NULL);
    Hook("NUL in a string", "{\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"ls\\u0000x\"}}", "", 2, NULL);
}

// #endregion

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: run_sh_test <run-sh.o> <run-sh-san.o> <out folder> <repo root>\n"
                        "(run it with ./build.sh test run_sh)\n");
        return 2;
    }
    gPlain = argv[1];
    gSan = argv[2];
    gOut = argv[3];
    gRoot = argv[4];
    // run-sh.o's log and lock go into the test's folder; the scratch folder under /tmp/claude-<uid>/
    char *state = Sprintf("%s/state", gOut);
    setenv("RUN_SH_DIR", state, 1);
    free(state);
    gScratch = Sprintf("/tmp/claude-%u/run-sh-test", (unsigned)getuid());
    MkdirP(gScratch);
    char *link = Sprintf("%s/link-to-repo", gScratch);
    struct stat st;
    if (lstat(link, &st) != 0 && symlink(gRoot, link) != 0) {
        fprintf(stderr, "run_sh_test: can't make %s: %s\n", link, strerror(errno));
        return 1;
    }
    free(link);

    TestRules();
    printf("run_sh_test: rules: %d passed, %d failed (%zu command lines x 2 builds)\n", gPassed, gFailed,
           sizeof CASES / sizeof CASES[0]);
    int passedBefore = gPassed, failedBefore = gFailed;
    TestPassThrough();
    TestBenchLock();
    TestHook();
    TestLog();
    printf("run_sh_test: -c, lock, hook, log: %d passed, %d failed\n", gPassed - passedBefore,
           gFailed - failedBefore);
    printf("run_sh_test: %s: %d passed, %d failed\n", gFailed ? "FAILED" : "OK", gPassed, gFailed);
    free(gScratch);
    return gFailed ? 1 : 0;
}
