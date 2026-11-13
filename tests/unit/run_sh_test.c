// tests/unit/run_sh_test.c: the test of tools/run-sh.c (build/run-sh.o), run by `./build.sh test run_sh`
// (nob.c, region "Unit tests"), which builds run-sh.c twice into build/unit/run_sh/ (run-sh.o with the
// flags of `./build.sh --tools`, run-sh-san.o with ASan + UBSan) and calls
//
//     build/unit/run_sh/run_sh_test <run-sh.o> <run-sh-san.o> <out folder> <repo root>
//
// What it checks (the Verify list of the task prompt, phase 1):
//   1. The rules table: every command line in CASES goes through `run-sh.o --check` (it never runs them)
//      with the repo root as the working directory, with both builds; the decision (allowed / refused, and
//      the rule's name in the refusal) must be the expected one. The sanitizer build's runs fail on any
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
#define READ "readable command"
#define OK NULL

static const Case CASES[] = {
    // --- no deletion outside /tmp: the commands, in the repo
    {"rm x", DEL},
    {"rm -rf src", DEL},
    {"rmdir build", DEL},
    {"unlink x", DEL},
    {"shred -u x", DEL},
    {"gio remove x", DEL},
    {"git clean -fd", DEL},
    {"git clean -fdx", DEL},
    {"git rm x", DEL},
    {"git rm -r src", DEL},
    // every way around it
    {"/bin/rm x", DEL},
    {"\\rm x", DEL},
    {"'rm' x", DEL},
    {"command rm x", DEL},
    {"builtin rm x", DEL},
    {"exec rm x", DEL},
    {"env rm x", DEL},
    {"env -i FOO=1 rm x", DEL},
    {"nice -n 5 rm x", DEL},
    {"nohup rm x", DEL},
    {"timeout 5 rm x", DEL},
    {"timeout -s KILL 5 rm x", DEL},
    {"time rm x", DEL},
    {"stdbuf -oL rm x", DEL},
    {"ionice -c 3 rm x", DEL},
    {"sudo rm x", DEL},
    {"xargs rm < list", DEL},
    {"ls | xargs rm", DEL},
    {"ls | xargs -0 rm -f", DEL},
    {"echo /tmp/x | xargs rm", DEL},
    {"find . -delete", DEL},
    {"find . -name '*.o' -delete", DEL},
    {"find src -exec rm {} \\;", DEL},
    {"find . -execdir rm {} +", DEL},
    {"find . -ok rm {} \\;", DEL},
    {"bash -c \"rm x\"", DEL},
    {"sh -c 'rm x'", DEL},
    {"zsh -c 'rm x'", DEL},
    {"dash -c 'rm x'", DEL},
    {"bash -lc 'rm x'", DEL},
    {"eval 'rm x'", DEL},
    {"eval rm x", DEL},
    {"sh -c 'sh -c \"rm x\"'", DEL},
    {"<root>/build/run-sh.o -c 'rm x'", DEL},
    {"python3 -c 'import os; os.remove(\"x\")'", DEL},
    {"python3 -c 'import shutil; shutil.rmtree(\"x\")'", DEL},
    {"python3 -c 'import os; os.system(\"rm -rf x\")'", DEL},
    {"python -Ic 'import os; os.unlink(\"x\")'", DEL},
    {"perl -e 'unlink \"x\"'", DEL},
    {"node -e 'require(\"fs\").rmSync(\"x\")'", DEL},
    {"ruby -e 'File.delete(\"x\")'", DEL},
    {"python3 - <<'EOF'\nimport os\nos.remove('x')\nEOF", DEL},
    {"{rm,-rf,src}", DEL},
    {"$'\\x72\\x6d' x", DEL},
    {"r''m x", DEL},
    {"echo $(rm x)", DEL},
    {"echo \"$(rm x)\"", DEL},
    {"echo `rm x`", DEL},
    {"cat <(rm x)", DEL},
    {"x=$(rm y)", DEL},
    {"echo ${X:-$(rm y)}", DEL},
    {"cat <<EOF\n$(rm x)\nEOF", DEL},
    {"bash <<'EOF'\nrm x\nEOF", DEL},
    {"bash <<< 'rm x'", DEL},
    {"env -S 'rm x'", DEL},
    {"git -c alias.x='!rm -rf src' x", DEL},
    {"if true; then rm x; fi", DEL},
    {"while false; do rm x; done", DEL},
    {"for f in a; do rm $f; done", DEL},
    {"case a in a) rm x;; esac", DEL},
    {"f() { rm x; }; f", DEL},
    {"true && rm x", DEL},
    {"false || rm x", DEL},
    {"echo a | rm x", DEL},
    {"sleep 1 & rm x", DEL},
    {"ls\nrm x", DEL},
    {"echo a \\\n; rm x", DEL},
    // every way out of /tmp
    {"rm -rf /tmp/../home/x", DEL},
    {"rm <scratch>/link-to-repo/file", DEL},
    {"rm -rf <scratch>/link-to-repo/", DEL},
    {"rm -rf <scratch>/link-to-repo/..", DEL},
    {"cd /tmp && rm -rf ../x", DEL},
    {"cd <scratch>/link-to-repo && rm -rf src", DEL},
    {"rm -rf /tmp", DEL},
    {"rm -rf /tmp/", DEL},
    {"rm -rf /tmp/.", DEL},
    {"rm -rf /tmp/a \"$HOME/b\"", DEL},
    {"rm -rf \"$D\"", DEL},
    {"rm -rf $D/x", DEL},
    {"rm -rf /tmp/a src", DEL},
    {"rm -rf /tmp/*", DEL ": rm /tmp/*; run-sh.o can't see"},
    {"rm -rf /tmp/vfy*", DEL ": rm /tmp/vfy*; run-sh.o can't see"},
    {"rm -rf /tmp/*/x", DEL},
    {"rm -rf <scratch>/.*", DEL},
    {"rm -rf ~/x", DEL},
    {"rm -rf /tmp/{a,../home}", DEL},
    {"(cd /tmp && rm -rf y); rm -rf z", DEL},
    {"cd /tmp | rm -rf x", DEL},
    {"cd /tmp/run-sh-test-missing-zz; rm -rf x", DEL},
    {"cd /tmp || rm -rf x", DEL},
    {"cd /tmp; cd -; rm a", DEL},
    {"cd \"$D\" && rm -rf x", DEL},
    {"find -L /tmp/x -delete", DEL},
    {"find /tmp/x -follow -delete", DEL},
    {"find <scratch>/link-to-repo -delete", DEL},
    {"git worktree remove ../wt", DEL},
    {"git worktree remove <scratch>/link-to-repo", DEL},
    {"git -C /tmp worktree remove ../home/x", DEL},
    {"env -C /tmp rm -rf ../x", DEL},
    // more wrappers: the command they run is checked
    {"setsid rm x", DEL},
    {"setsid -f rm -rf src", DEL},
    {"chronic rm x", DEL},
    {"strace -f rm x", DEL},
    {"strace -f -o /tmp/t rm x", DEL},
    {"ltrace rm x", DEL},
    {"gdb -batch -ex run --args rm x", DEL},
    {"flock /tmp/l rm x", DEL},
    {"flock -w 5 /tmp/l rm x", DEL},
    {"flock /tmp/l -c 'rm x'", DEL},
    {"flock -c 'rm x' /tmp/l", DEL},
    {"flock /tmp/l --command='rm x'", DEL},
    {"watch rm x", DEL},
    {"watch -n 1 'ls; rm x'", DEL},
    {"watch -x rm x", DEL},
    {"script -qc 'rm x' /dev/null", DEL},
    {"script --command 'rm x'", DEL},
    {"su -c 'rm x'", DEL},
    {"su -lc 'rm x' root", DEL},
    {"parallel rm ::: x", DEL ": parallel rm; "},
    {"parallel 'rm {}' ::: /tmp/a", DEL ": parallel rm; "},
    {"parallel -j 4 rm -f ::: /tmp/a", DEL},
    {"ls | xargs sh -c 'rm \"$@\"' _", DEL ": xargs rm; "},
    {"busybox rm x", DEL},
    {"toybox rm x", DEL},
    {"busybox sh -c 'rm x'", DEL},
    {"nice busybox rm x", DEL},
    // awk: the commands of its program
    {"awk 'BEGIN{system(\"rm x\")}'", DEL},
    {"gawk 'BEGIN { system( \"rm -rf src\" ) }'", DEL},
    {"mawk 'BEGIN{\"rm x\" | getline}'", DEL},
    {"nawk '{print | \"rm x\"}'", DEL},
    {"gawk -e 'BEGIN{system(\"rm x\")}'", DEL},
    {"awk 'BEGIN{system(\"rm \" f)}'", DEL ": awk system(…); "},
    {"awk 'BEGIN{system(cmd)}'", DEL ": awk system(…); "},
    {"awk 'BEGIN{cmd = \"ls\"; cmd | getline}'", DEL ": awk … | getline; "},
    {"awk '{print $1 | cmd}'", DEL ": awk print … | …; "},
    {"awk 'BEGIN{system(\"bash -c \\\"rm x\\\"\")}'", DEL},
    {"busybox awk 'BEGIN{system(\"rm x\")}'", DEL},
    // deletions in other tools
    {"tar --remove-files -cf /tmp/vfy.tar src", DEL ": tar --remove-files src; "},
    {"tar -cf /tmp/x.tar --remove-files /tmp/a src", DEL},
    {"tar czf /tmp/x.tgz --remove-files README.md", DEL},
    {"tar -C /tmp --remove-files -cf /tmp/x.tar ../home/x", DEL},
    {"tar --remove-files -cf /tmp/x.tar -T list", DEL},
    {"zip -m /tmp/vfy.zip README.md", DEL ": zip -m README.md; "},
    {"zip --move -r /tmp/a.zip src", DEL},
    {"zip -rm /tmp/a.zip src", DEL},
    {"zip -m -@ /tmp/a.zip", DEL},
    {"rsync -a --delete /tmp/vfyempty/ src/", DEL ": rsync --delete src/; "},
    {"rsync -a --delete-after /tmp/a/ maps/", DEL},
    {"rsync -a --remove-source-files src/ /tmp/x/", DEL ": rsync --remove-source-files src/; "},
    {"rsync --remove-source-files --files-from=list / /tmp/x/", DEL},
    // a link made in the same command line could redirect a deletion checked before the link exists
    {"ln -s /home /tmp/zz; rm -rf /tmp/zz/pc404", DEL},
    {"ln -sf ../.. /tmp/q && rm -rf /tmp/q/x", DEL},
    {"bash -c \"ln -s /home /tmp/zz; rm -r /tmp/zz/a\"", DEL},
    {"ln --symbolic /home /tmp/zz && eval 'rm -rf /tmp/zz/x'", DEL},
    {"mkdir -p /tmp/d && ln /home/x /tmp/d/y && rm /tmp/d/y", DEL},
    {"cp -s /home/x /tmp/y; rm /tmp/y", DEL},
    {"cp -rs /home /tmp/h && find /tmp/h -delete", DEL},
    {"rm -rf /tmp/zz/x; ln -s /home /tmp/zz", DEL},
    {"ln -s /home /tmp/zz; mv /tmp/zz /tmp/yy; rm -rf /tmp/yy/x", DEL},
    // --- no lost work
    {"git reset --hard", LOST},
    {"git reset --hard HEAD~1", LOST},
    {"git checkout -- src/main.c", LOST},
    {"git checkout .", LOST},
    {"git checkout HEAD -- .", LOST},
    {"git checkout -f main", LOST},
    {"git restore src/main.c", LOST},
    {"git restore --staged --worktree x", LOST},
    {"git restore -SW x", LOST},
    {"git stash drop", LOST},
    {"git stash clear", LOST},
    {"git switch -f main", LOST},
    {"git switch --discard-changes main", LOST},
    {"git -C /tmp/x checkout -- y", LOST},
    {"git --no-pager reset --hard", LOST},
    // --- no visible window
    {"./build/linux/monolith", WIN},
    {"build/linux/monolith --view 2", WIN},
    {"<root>/build/linux/monolith --map maps/x", WIN},
    {"./build.sh run", WIN},
    {"./build.sh run --view 2", WIN},
    {"./build.sh --win run", WIN},
    {"build/nob run", WIN},
    {"bash build.sh run", WIN},
    {"wine build/win/monolith.exe", WIN},
    {"/tmp/old-monolith-abc/build/linux/monolith --map maps/x", WIN},
    {"./build/linux/monolith --bench-window 4", WIN},
    {"nohup ./build/linux/monolith &", WIN},
    {"MONOLITH_ALLOW_WINDOW=1 ./build/linux/monolith --shot x.png", WIN},
    {"MONOLITH_ALLOW_WINDOW=1", WIN},
    {"export MONOLITH_ALLOW_WINDOW=1", WIN},
    {"export MONOLITH_ALLOW_WINDOW", WIN},
    {"declare -x MONOLITH_ALLOW_WINDOW=1", WIN},
    {"env MONOLITH_ALLOW_WINDOW=1 ls", WIN},
    {"bash -c 'MONOLITH_ALLOW_WINDOW=1 ls'", WIN},
    {"python3 -c 'import os; os.environ[\"MONOLITH_ALLOW_WINDOW\"]=\"1\"'", WIN},
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
    {"echo x >> ~/notes.txt", STAY},
    {"echo x 2> /etc/foo", STAY},
    {"echo x &> /etc/foo", STAY},
    {"echo x >& /etc/foo", STAY},
    {"cp a /usr/local/bin/", STAY},
    {"cp -t /opt a b", STAY},
    {"cp --target-directory=/opt a", STAY},
    {"mv x /home/other", STAY},
    {"echo x | tee /etc/x", STAY},
    {"mkdir /opt/x", STAY},
    {"mkdir -p ../outside", STAY},
    {"install -D README.md /home/pc404/vfyx", STAY},
    {"install -m 644 a /usr/local/bin/a", STAY},
    {"install -t /usr/local/bin a b", STAY},
    {"install -d build/x /opt/x", STAY},
    {"touch /var/tmp/x", STAY},
    {"cp a <scratch>/link-to-repo/../outside", STAY},
    {"/usr/bin/time -o /etc/t ls", STAY},
    // --- readable command
    {"echo 'unterminated", READ},
    {"echo \"unterminated", READ},
    {"echo $(ls", READ},
    {"echo `ls", READ},
    {"echo >", READ},

    // --- allowed, unchanged
    {"gio trash x", OK},
    {"gio trash src/old.c build/x", OK},
    {"rm -rf /tmp/monolith-scratch-1", OK},
    {"rm <scratch>/out.png", OK},
    {"rm -f <scratch>/*.png", OK},
    {"rm -rf <scratch>/a <scratch>/b", OK},
    {"rm -rf <scratch>/link-to-repo", OK},  // the link itself, not its target
    {"rm -rf /tmp/{a,b}", OK},
    {"rm -rf /tmp/x", OK},
    {"ln -s a b", OK},
    {"ln -sf ../.. /tmp/q", OK},
    {"cp -s /home/x /tmp/y", OK},
    // the wrappers and tools above, used harmlessly
    {"setsid -f ls", OK},
    {"chronic make", OK},
    {"strace -o /tmp/t ls", OK},
    {"strace -p 1234", OK},
    {"gdb ./build/linux-debug/monolith", OK},
    {"flock /tmp/l make", OK},
    {"flock -w 5 /tmp/l -c 'ls'", OK},
    {"watch -n 2 ls", OK},
    {"watch -n 1 'git status | head'", OK},
    {"script -qc 'ls' /dev/null", OK},
    {"su -c 'ls'", OK},
    {"parallel echo ::: a b", OK},
    {"parallel 'gzip -k {}' ::: /tmp/a", OK},
    {"busybox ls", OK},
    {"toybox echo rm", OK},
    {"awk '{n++} END {print n}' file", OK},
    {"awk -F: '$1 ~ /a|b/ {print $2 || 0}' f", OK},
    {"awk 'BEGIN{system(\"ls -la\")}'", OK},
    {"awk '{print $1 | \"sort -u\"}' f", OK},
    {"awk 'BEGIN{\"date\" | getline d; print d}'", OK},
    {"awk -f prog.awk data", OK},
    {"awk '/rm/ {print \"rm \" $0}' f", OK},
    {"tar -czf /tmp/x.tgz src", OK},
    {"tar --remove-files -cf /tmp/x.tar /tmp/a /tmp/b", OK},
    {"tar -C /tmp --remove-files -czf /tmp/x.tgz y", OK},
    {"tar czf /tmp/x.tgz --remove-files /tmp/y", OK},
    {"zip -r /tmp/a.zip src", OK},
    {"zip -m /tmp/vfy.zip /tmp/a /tmp/b", OK},
    {"zip -r -m /tmp/a.zip /tmp/d -x '*.o'", OK},
    {"rsync -a src/ /tmp/x/", OK},
    {"rsync -a --delete src/ /tmp/mirror/", OK},
    {"rsync -a --remove-source-files /tmp/a/ build/", OK},
    {"rsync -a --delete /tmp/a/ host:/srv/x", OK},
    {"install -m 644 README.md build/x", OK},
    {"install -d build/x /tmp/y", OK},
    {"rmdir /tmp/empty-dir", OK},
    {"find /tmp/x -delete", OK},
    {"find <scratch> -name '*.png' -delete", OK},
    {"find /tmp/x -exec rm {} \\;", OK},
    {"find /tmp/x -type f -exec rm -f {} +", OK},
    {"git worktree remove /tmp/wt-1", OK},
    {"git worktree remove --force /tmp/wt-1", OK},
    {"cd /tmp && rm -rf ./monolith-x", OK},
    {"cd <scratch> && rm out.png", OK},
    {"mkdir -p /tmp/run-sh-test-new-zz && cd /tmp/run-sh-test-new-zz && rm -rf y", OK},
    {"env -C /tmp rm -rf y", OK},
    {"./build.sh lsan > /dev/null", OK},
    {"./build.sh", OK},
    {"./build.sh --win", OK},
    {"./build.sh --tools run-sh", OK},
    {"./build.sh test run_sh", OK},
    {"grep -rn rm src", OK},
    {"grep -rn 'rm -rf' docs", OK},
    {"echo \"rm -rf /\"", OK},
    {"echo 'git reset --hard'", OK},
    {"cat <<EOF\nrm -rf /\nEOF", OK},
    {"cat <<'EOF' > <scratch>/x.txt\nrm -rf $(oops)\nEOF", OK},
    {"cat <<-EOF\n\trm -rf /\n\tEOF\necho done", OK},
    {"git rm --cached x", OK},
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
    {"git -c alias.x='!echo hi' x", OK},
    {"__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless --bench-frames 150 --note \"screen: what changed\"", OK},
    {"__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless --note \"what changed\"", OK},
    {"vblank_mode=0 ./build/linux/monolith --bench --headless --note x", OK},
    {"export __GL_SYNC_TO_VBLANK=0; ./build/linux/monolith --bench --headless --note x", OK},
    {"env __GL_SYNC_TO_VBLANK=0 ./build/linux/monolith --bench --headless --note=x", OK},
    {"__GL_SYNC_TO_VBLANK=0 examples/build/linux-test/01_minimal/minimal --bench r.md maps/x", OK},
    {"./build/linux/monolith --shot /tmp/x.png --view 2", OK},
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
    {"wine build/win/monolith.exe --shot win.png --view 2", OK},
    {"examples/build/linux-test/01_minimal/minimal --shot /tmp/x.png --view 2", OK},
    {"examples/build/linux-lsan/01_minimal/minimal --shot /tmp/x.png", OK},
    {"i3-msg -t get_tree", OK},
    {"i3-msg -t get_workspaces", OK},
    {"swaymsg -t get_outputs", OK},
    {"i3-msg -t get_tree | head -c 100", OK},
    {"grep -rn MONOLITH_ALLOW_WINDOW src", OK},
    {"echo MONOLITH_ALLOW_WINDOW", OK},
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
    {"python3 -c 'print(1)'", OK},
    {"python3 -I tools/record_asset.py --help", OK},
    {"bash -c 'echo hi'", OK},
    {"command -v rm", OK},
    {"type rm", OK},
    {"which rm", OK},
    {"ls -la | grep -c rm", OK},
    {"find . -name '*.c' | head", OK},
    {"find src -name '*.c' -exec grep -l rm {} +", OK},
    {"xargs grep -l x < list", OK},
    {"sleep 1 &", OK},
    {"(cd /tmp && ls)", OK},
    {"f() { echo hi; }; f", OK},
    {"for f in a b; do echo $f; done", OK},
    {"for ((i = 0; i < 3; i++)); do echo $i; done", OK},
    {"case x in a) echo a;; *) echo b;; esac", OK},
    {"echo $(case x in a) echo a;; esac)", OK},
    {"[[ -f x ]] && echo y", OK},
    {"(( 1 + 2 ))", OK},
    {"echo {a,b} $((1 + 2)) ${HOME}", OK},
    {"arr=(a b c); echo ${arr[@]}", OK},
    {"<root>/build/run-sh.o -c 'ls'", OK},
    {"<root>/build/run-sh.o --check 'rm x'", OK},
    {"# rm -rf /", OK},
    {"echo a # rm -rf /", OK},
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
