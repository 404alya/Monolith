// profile.c -- CPU profile of one headless monolith run with perf (no sudo, user space only)
//
//   profile [--period N] [--spin] [--keep-data] [--out DIR] [--bin PATH] <name> -- <monolith args...>
//   profile [--bin PATH] --report DIR      write DIR's report.txt again from its perf.txt
//   profile --report-only <perf.txt> [--bench REPORT.md] [--finish-site SYM+0xOFF] [--all] [--top N]
//                                          print a report of any perf.txt (no binary needed)
//
//   build/linux-profile/profile spin -- --bench --headless --note "profile: spinning"
//   build/linux-profile/profile still -- --bench --headless --bench-still --frame-reuse --note "profile: still"
//   build/linux-profile/profile load -- --shot /tmp/x.png --view 1 --reload-test maps/<name>
//
// Built by ./build.sh profile into build/linux-profile/profile, next to the binary it profiles:
// build/linux-profile/monolith (-O2 -g and frame pointers, R3D/raylib/assimp RelWithDebInfo).
// Linux only (perf is); links only libc. A profile run, step by step (main() at the bottom reads
// the same way):
//   1. Refuse runs that would open a window: only --bench --headless and --shot runs.
//   2. Pick the sampling period: a sample every N user-mode cycles (cycles:u), N chosen so that at
//      the CPU's top clock the rate is 40% of the kernel's perf_event_max_sample_rate. The kernel
//      lowers that limit by itself after slow sampling interrupts (dwarf stacks are slow: e.g. to
//      2000/s); above about half of it perf is throttled, drops samples in bursts and biases every
//      share (measured on machine A: 80% of it throttles, 45% doesn't). --period overrides.
//   3. Record: `perf record -e cycles:u -c <period> --call-graph dwarf,16384` of the game, run in the
//      repo root (it resolves relative paths such as --shot files against the repo, like a plain
//      run) with __GL_SYNC_TO_VBLANK=0 and __GL_YIELD=USLEEP (--spin: the driver's default
//      spin-wait in glFinish). The game's output goes to run.log. perf.data (~16 KB per sample)
//      goes into a temporary folder under /tmp, deleted at the end (--keep-data moves it to --out).
//   4. Convert: `perf script` writes perf.txt, the text the report reads.
//   5. perf-stats.txt: the period and perf's sample, throttle and lost counts; a warning when perf
//      was throttled (such a run doesn't count: rerun with a larger --period).
//   6. Find the headless bench's glFinish() call in Run() (nm + objdump), so the report can leave
//      out the driver polling there (waiting for the GPU isn't CPU work).
//   7. Find the bench report from the `report: ` line in run.log (bench runs only).
//   8. Write report.txt (see "The report" below).
// Output folder: --out, default benchmark/profile/<stamp>_<name>/ (gitignored).
//
// Every command is printed before it runs, and runs without a shell: fork, then execvp with an
// argv array; its output goes to a file (dup2) or comes back through a pipe, and its exit status
// is checked (waitpid). perf counts as a benchmark: one run at a time on the GPU. The
// `benchmarking` skill, "CPU profiling", has the method.
//
// The report:
// CPU: every sample of perf.txt (weighted by its period, i.e. cycles) is charged to the first
// function of our own binary on its stack, so a function's share includes the driver and library
// work it calls directly (the driver has no symbols). Waiting for the GPU isn't CPU work and is
// left out: sleeps and yields, and the driver polling at the glFinish() call of step 6. One-time
// work (init, map loads and reloads, shader compiles, cache bakes, exit) is left out too, so the
// table is the per-frame work; --all keeps it (bench runs leave it out, --shot runs keep it: there
// the load is the point).
// GPU: the GPU work is per R3D pass, not per function: the bench report's "GPU passes" table
// (GL_TIME_ELAPSED queries), ranked by its All column.
// --report needs the binary that recorded DIR (the glFinish site is an offset into it).
#define _DEFAULT_SOURCE      // mkdtemp, setenv, realpath, strdup (glibc)
#define _XOPEN_SOURCE 700    // nftw with FTW_DEPTH / FTW_PHYS

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// #region Small helpers

// Prints "profile: <message>" to stderr and exits 1 (the temporary folder is removed by atexit)
static void Fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("profile: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void *Alloc(size_t bytes)
{
    void *p = malloc(bytes);
    if (!p) Fail("out of memory");
    return p;
}

// Grows a heap array to hold at least `need` elements (doubling, like MS_RESERVE in the runtime)
#define GROW(arr, cap, need) \
    do { \
        if ((size_t)(need) > (cap)) { \
            size_t n_ = (cap) ? (cap) : 16; \
            while (n_ < (size_t)(need)) n_ *= 2; \
            void *p_ = realloc((arr), n_ * sizeof(*(arr))); \
            if (!p_) Fail("out of memory"); \
            (arr) = p_; \
            (cap) = n_; \
        } \
    } while (0)

// A heap string formatted like printf
static char *Format(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *s = Alloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

static char *Dup(const char *s)
{
    char *d = strdup(s);
    if (!d) Fail("out of memory");
    return d;
}

// A whole file as a heap string; NULL if it can't be read
static char *ReadFile(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *text = NULL;
    size_t len = 0, cap = 0, got;
    do {
        GROW(text, cap, len + 65536 + 1);
        got = fread(text + len, 1, cap - len - 1, f);
        len += got;
    } while (got > 0);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) { free(text); return NULL; }
    text[len] = '\0';
    return text;
}

// The next line of `text` (cut at '\n', which is replaced by '\0'); NULL at the end. `*pos` is
// where the next call continues.
static char *NextLine(char *text, size_t *pos)
{
    char *line = text + *pos;
    if (*line == '\0') return NULL;
    char *nl = strchr(line, '\n');
    if (nl) { *nl = '\0'; *pos = (size_t)(nl + 1 - text); }
    else *pos += strlen(line);
    return line;
}

static bool IsFile(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// mkdir -p
static void MakeDirs(const char *path)
{
    char *p = Dup(path);
    for (char *s = p + 1; ; s++) {
        if (*s == '/' || *s == '\0') {
            char c = *s;
            *s = '\0';
            if (mkdir(p, 0777) != 0 && errno != EEXIST) Fail("can't create %s: %s", p, strerror(errno));
            *s = c;
            if (c == '\0') break;
        }
    }
    free(p);
}

// A POSIX extended regular expression; the patterns are fixed, so a bad one is a bug here
static regex_t Regex(const char *pattern)
{
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) Fail("bad regex %s", pattern);
    return re;
}

// #endregion

// #region Running commands (fork + execvp, no shell)

// How a command's process is set up; a zeroed Child is like ours
typedef struct {
    const char *dir;                 // working directory
    const char *const *env;          // NULL-terminated "NAME=value" list, set in the child only
    const char *stdinPath;           // files for stdin / stdout / stderr; stderrPath may be
    const char *stdoutPath;          // STDOUT_TOO (2>&1)
    const char *stderrPath;
} Child;
#define STDOUT_TOO "&1"

// Prints a command the way it would be typed into a shell, so it can be copied and run again
static void PrintCommand(char *const argv[], Child c, bool captured)
{
    printf("$");
    if (c.dir) printf(" cd %s &&", c.dir);
    for (const char *const *e = c.env; e && *e; e++) printf(" %s", *e);
    for (int i = 0; argv[i]; i++) {
        const char *a = argv[i];
        bool plain = *a != '\0' && strspn(a, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                             "0123456789_-+=.,:/@%") == strlen(a);
        if (plain) { printf(" %s", a); continue; }
        printf(" '");
        for (; *a; a++) {
            if (*a == '\'') printf("'\\''");
            else putchar(*a);
        }
        putchar('\'');
    }
    if (c.stdinPath) printf(" < %s", c.stdinPath);
    if (c.stdoutPath) printf(" > %s", c.stdoutPath);
    if (c.stderrPath) {
        if (strcmp(c.stderrPath, STDOUT_TOO) == 0) printf(" 2>&1");
        else printf(" 2> %s", c.stderrPath);
    }
    if (captured) printf(" | (read by profile)");
    putchar('\n');
    fflush(stdout);
}

// Opens `path` as file descriptor `fd` in the child (an error ends the child)
static void Redirect(int fd, const char *path, int flags)
{
    int f = open(path, flags, 0666);
    if (f < 0 || dup2(f, fd) < 0) {
        fprintf(stderr, "profile: can't open %s: %s\n", path, strerror(errno));
        _exit(127);
    }
    close(f);
}

// Starts a command: fork, then in the child the redirections, the working directory and the
// environment, then execvp. pipeOut >= 0: the child's stdout goes into that pipe instead.
static pid_t Start(char *const argv[], Child c, int pipeOut)
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) Fail("fork failed: %s", strerror(errno));
    if (pid > 0) return pid;

    // the child (_exit, never exit: exit would run our atexit cleanup in here)
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    if (c.stdinPath) Redirect(STDIN_FILENO, c.stdinPath, O_RDONLY);
    if (pipeOut >= 0) dup2(pipeOut, STDOUT_FILENO);
    if (c.stdoutPath) Redirect(STDOUT_FILENO, c.stdoutPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (c.stderrPath) {
        if (strcmp(c.stderrPath, STDOUT_TOO) == 0) dup2(STDOUT_FILENO, STDERR_FILENO);
        else Redirect(STDERR_FILENO, c.stderrPath, O_WRONLY | O_CREAT | O_TRUNC);
    }
    // (after the redirections: relative output paths stay relative to where profile was started)
    if (c.dir && chdir(c.dir) != 0) {
        fprintf(stderr, "profile: can't cd to %s: %s\n", c.dir, strerror(errno));
        _exit(127);
    }
    for (const char *const *e = c.env; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        char *name = strndup(*e, (size_t)(eq - *e));
        if (!name || setenv(name, eq + 1, 1) != 0) _exit(127);
        free(name);
    }
    execvp(argv[0], argv);
    fprintf(stderr, "profile: can't run %s: %s\n", argv[0], strerror(errno));
    _exit(127);
}

// Waits for a command; its exit status (0: success), 128 + the signal if a signal ended it.
// Ctrl+C reaches the command (same process group); we ignore it meanwhile, like system(), so that
// we get to clean up after it.
static int Wait(pid_t pid, const char *name)
{
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) Fail("waitpid for %s failed: %s", name, strerror(errno));
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "profile: %s was ended by signal %d\n", name, WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return WEXITSTATUS(status);
}

// Runs a command and waits for it: its exit status
static int Run(char *const argv[], Child c)
{
    PrintCommand(argv, c, false);
    void (*oldInt)(int) = signal(SIGINT, SIG_IGN);
    void (*oldQuit)(int) = signal(SIGQUIT, SIG_IGN);
    int status = Wait(Start(argv, c, -1), argv[0]);
    signal(SIGINT, oldInt);
    signal(SIGQUIT, oldQuit);
    return status;
}

// Runs a command and reads its stdout back through a pipe: a heap string. A failed command ends
// profile.
static char *Capture(char *const argv[], Child c)
{
    PrintCommand(argv, c, true);
    int fds[2];
    if (pipe(fds) != 0) Fail("pipe failed: %s", strerror(errno));
    void (*oldInt)(int) = signal(SIGINT, SIG_IGN);
    void (*oldQuit)(int) = signal(SIGQUIT, SIG_IGN);
    pid_t pid = Start(argv, c, fds[1]);
    close(fds[1]);
    char *text = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        GROW(text, cap, len + 65536 + 1);
        ssize_t got = read(fds[0], text + len, cap - len - 1);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        len += (size_t)got;
    }
    close(fds[0]);
    text[len] = '\0';
    int status = Wait(pid, argv[0]);
    signal(SIGINT, oldInt);
    signal(SIGQUIT, oldQuit);
    if (status != 0) Fail("%s failed (exit status %d)", argv[0], status);
    return text;
}

// #endregion

// #region The report (CPU: top functions, GPU: top R3D passes)

// Waiting for the GPU (sleeps, yields, futex waits) and one-time work, by function name
#define WAIT_PATTERN "^(usleep|__nanosleep|clock_nanosleep|nanosleep|sched_yield|__sched_yield|" \
                     "poll|__poll|pthread_cond_wait|___pthread_cond_wait|__futex_abstimed_wait[[:alnum:]_]*)$"
#define ONCE_PATTERN "^(MS_Load|MS_Reload|RestartRenderer|InitWindow|R3D_Init|MS_InitRenderer|CloseWindow|" \
                     "R3D_Close|FreeMainHeap|load_shader|link_shader|compile_shader|r3d_shader_load_[[:alnum:]_]+|" \
                     "BakeOcclusionVolume|R3D_BakeFogSunVisibility|_dl_[[:alnum:]_]+|dl_main|__run_exit_handlers)$"
#define NO_CALLER_OF_OURS "(driver/library code with no caller of ours)"

typedef struct {
    char *sym;          // function name without its +0x offset
    int ours;           // 1: in our executable, 0: a shared library, the vdso or the kernel,
                        // -1 (only while reading): inlined, i.e. the same as the frame below it
    bool hasOff;
    long long off;      // offset into sym (perf's +0x...)
} Frame;

typedef struct {
    long long period;   // cycles this sample stands for
    Frame *frames;      // leaf first
    size_t count, cap;
} Sample;

// Reads perf.txt (`perf script -F comm,tid,time,period,ip,sym,symoff,dso`) one sample at a time:
//   final2-profile  722919 43757.661227:    5692478            <- a sample: comm tid time: period
//       563df98393ab ParseFloat+0x5b (inlined)                 <- its stack, leaf first:
//       563df98393ab NumF+0x5b (/path/to/monolith)                address sym+off (dso)
typedef struct {
    FILE *file;
    char *line;
    size_t lineCap;
    regex_t head, frame, off;
    bool pending;               // a sample's head line was read: pendingPeriod is its period
    long long pendingPeriod;
} SampleReader;

static void OpenSamples(SampleReader *r, const char *path)
{
    memset(r, 0, sizeof(*r));
    r->file = fopen(path, "r");
    if (!r->file) Fail("can't read %s: %s", path, strerror(errno));
    r->head = Regex("^[[:space:]]*[^[:space:]]+[[:space:]]+[0-9]+[[:space:]]+[0-9.]+:[[:space:]]+([0-9]+)[[:space:]]*$");
    // sym is the text up to the first blank before "(": it may contain "(" (operator()) and blanks,
    // but no blank directly before a "("
    r->frame = Regex("^[[:space:]]+[0-9a-f]+[[:space:]]+(([^[:space:]]|[[:space:]]+[^[:space:](])*)"
                     "[[:space:]]+\\((.*)\\)[[:space:]]*$");
    r->off = Regex("\\+(0x[0-9a-f]+)$");
}

static void CloseSamples(SampleReader *r)
{
    fclose(r->file);
    free(r->line);
    regfree(&r->head);
    regfree(&r->frame);
    regfree(&r->off);
}

static void ClearSample(Sample *s)
{
    for (size_t i = 0; i < s->count; i++) free(s->frames[i].sym);
    s->count = 0;
}

// The next sample into *s (false at the end of the file). Lines before the first sample and
// lines that are neither a head nor a frame are skipped.
static bool NextSample(SampleReader *r, Sample *s)
{
    ClearSample(s);
    bool have = r->pending;
    s->period = r->pendingPeriod;
    r->pending = false;
    regmatch_t m[4];
    ssize_t len;
    while ((len = getline(&r->line, &r->lineCap, r->file)) >= 0) {
        char *line = r->line;
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (regexec(&r->head, line, 2, m, 0) == 0) {
            long long period = strtoll(line + m[1].rm_so, NULL, 10);
            if (have) {             // the end of the current sample: keep this head for the next
                r->pending = true;
                r->pendingPeriod = period;
                break;
            }
            have = true;
            s->period = period;
            continue;
        }
        if (!have || regexec(&r->frame, line, 4, m, 0) != 0) continue;
        char *sym = line + m[1].rm_so;
        char *dso = line + m[3].rm_so;
        line[m[1].rm_eo] = '\0';
        line[m[3].rm_eo] = '\0';
        GROW(s->frames, s->cap, s->count + 1);
        Frame *f = &s->frames[s->count++];
        regmatch_t o[2];
        f->hasOff = regexec(&r->off, sym, 2, o, 0) == 0;
        f->off = f->hasOff ? strtoll(sym + o[1].rm_so, NULL, 16) : 0;
        if (f->hasOff) sym[o[0].rm_so] = '\0';
        f->sym = Dup(sym);
        // ours: the executable, not a shared library, the vdso or the kernel
        if (strcmp(dso, "inlined") == 0) f->ours = -1;
        else f->ours = !(dso[0] == '[' || strstr(dso, ".so"));
    }
    if (!have) return false;
    // an inlined frame belongs to the function it was inlined into (the next frame towards main)
    for (size_t i = s->count; i-- > 1; ) {
        if (s->frames[i - 1].ours == -1) s->frames[i - 1].ours = s->frames[i].ours;
    }
    return true;
}

// The cycles charged to one function; `first` orders ties by first appearance
typedef struct {
    char *name;
    long long cycles;
    size_t first;
} Charge;

static int ByName(const void *a, const void *b)
{
    const Charge *x = a, *y = b;
    int c = strcmp(x->name, y->name);
    return c ? c : (x->first > y->first) - (x->first < y->first);
}

static int ByCyclesDown(const void *a, const void *b)
{
    const Charge *x = a, *y = b;
    if (x->cycles != y->cycles) return x->cycles < y->cycles ? 1 : -1;
    return (x->first > y->first) - (x->first < y->first);
}

static bool Matches(const regex_t *re, const char *s)
{
    return regexec(re, s, 0, NULL, 0) == 0;
}

// The CPU table. finishSite: "Run+0x1a2" or NULL; all: keep one-time work.
static void CpuTable(FILE *out, const char *perfTxt, const char *finishSite, bool all, int top)
{
    char *finishSym = NULL;
    long long finishOff = 0;
    if (finishSite) {
        const char *plus = strchr(finishSite, '+');
        char *end;
        if (!plus || strchr(plus + 1, '+')) Fail("--finish-site wants SYM+0xOFF, not %s", finishSite);
        finishOff = strtoll(plus + 1, &end, 16);
        if (end == plus + 1 || *end) Fail("--finish-site wants SYM+0xOFF, not %s", finishSite);
        finishSym = strndup(finishSite, (size_t)(plus - finishSite));
        if (!finishSym) Fail("out of memory");
    }
    regex_t waitRe = Regex(WAIT_PATTERN), onceRe = Regex(ONCE_PATTERN);

    // One charge per sample that counts, then summed per function
    Charge *charges = NULL;
    size_t nCharges = 0, capCharges = 0;
    long long total = 0, waiting = 0, once = 0;
    SampleReader reader;
    Sample s = { 0 };
    OpenSamples(&reader, perfTxt);
    while (NextSample(&reader, &s)) {
        if (s.count == 0) continue;
        total += s.period;
        Frame *own = NULL;
        for (size_t i = 0; i < s.count && !own; i++) {
            if (s.frames[i].ours == 1) own = &s.frames[i];
        }
        bool isWait = false, isOnce = false;
        for (size_t i = 0; i < s.count; i++) {
            if (Matches(&waitRe, s.frames[i].sym)) isWait = true;
            if (Matches(&onceRe, s.frames[i].sym)) isOnce = true;
        }
        // the driver polling at the glFinish() call: our first frame is that call site (perf
        // prints a caller as its return address - 1, give or take one), and not the leaf itself
        if (finishSym && own && own != &s.frames[0] && strcmp(own->sym, finishSym) == 0 && own->hasOff
            && llabs(own->off - finishOff) <= 1) {
            isWait = true;
        }
        if (isWait) { waiting += s.period; continue; }
        if (!all && isOnce) { once += s.period; continue; }
        GROW(charges, capCharges, nCharges + 1);
        charges[nCharges] = (Charge){ Dup(own ? own->sym : NO_CALLER_OF_OURS), s.period, nCharges };
        nCharges++;
    }
    ClearSample(&s);
    free(s.frames);
    CloseSamples(&reader);
    regfree(&waitRe);
    regfree(&onceRe);
    free(finishSym);

    // Sum per function: sort by name, add up each run of equal names into its first charge
    qsort(charges, nCharges, sizeof(*charges), ByName);
    size_t nFunctions = 0;
    long long work = 0;
    for (size_t i = 0; i < nCharges; i++) {
        work += charges[i].cycles;
        if (nFunctions > 0 && strcmp(charges[nFunctions - 1].name, charges[i].name) == 0) {
            charges[nFunctions - 1].cycles += charges[i].cycles;
            free(charges[i].name);
        } else {
            charges[nFunctions++] = charges[i];
        }
    }
    if (!total) Fail("no samples in %s", perfTxt);
    if (!work) Fail("no samples left (a load profile? try --all)");

    qsort(charges, nFunctions, sizeof(*charges), ByCyclesDown);
    fprintf(out, "## CPU: top %d functions %s(own code + the driver/library work they call)\n\n",
            top, all ? "incl. one-time work " : "per frame ");
    for (size_t i = 0; i < nFunctions && i < (size_t)top; i++) {
        fprintf(out, "  %6.2f%%  %s\n", 100.0 * charges[i].cycles / work, charges[i].name);
    }
    fprintf(out, "\n  left out: %.1f%% of the samples waiting for the GPU, %.1f%% one-time work%s\n",
            100.0 * waiting / total, 100.0 * once / total, all ? "" : " (--all keeps it)");
    for (size_t i = 0; i < nFunctions; i++) free(charges[i].name);
    free(charges);
}

typedef struct {
    double ms;
    char *name;
} Pass;

static int ByMsDown(const void *a, const void *b)
{
    const Pass *x = a, *y = b;
    if (x->ms != y->ms) return x->ms < y->ms ? 1 : -1;
    return -strcmp(x->name, y->name);
}

// s without the characters of `chars` at both ends (cut in place)
static char *Strip(char *s, const char *chars)
{
    while (*s && strchr(chars, *s)) s++;
    size_t n = strlen(s);
    while (n > 0 && strchr(chars, s[n - 1])) s[--n] = '\0';
    return s;
}
#define BLANKS " \t\r\n\v\f"

// The GPU table: the "## GPU passes" table of a bench report, its rows up to "sum of passes":
//   | GPU ms per frame | Canyon | ... | **All** |
//   | geometry | 2.253 | ... | **1.787** |
static void GpuTable(FILE *out, const char *benchReport, int top)
{
    char *text = ReadFile(benchReport);
    if (!text) Fail("can't read %s", benchReport);
    Pass *rows = NULL;
    size_t nRows = 0, capRows = 0, pos = 0;
    bool inTable = false;
    char *line;
    while ((line = NextLine(text, &pos))) {
        if (strncmp(line, "## ", 3) == 0) {
            inTable = strcmp(Strip(line, BLANKS), "## GPU passes") == 0;
            continue;
        }
        if (!inTable) continue;
        // the cells: the line without its outer "|", split at "|", each without blanks and "*";
        // only the first (the pass) and the last (All) are needed
        line = Strip(Strip(line, BLANKS), "|");
        char *bar = strchr(line, '|');
        if (!bar) continue;                 // fewer than two cells
        char *lastBar = strrchr(line, '|');
        *bar = '\0';
        char *first = Strip(Strip(line, BLANKS), "*");
        char *last = Strip(Strip(lastBar + 1, BLANKS), "*");
        if (strncmp(first, "GPU ms", 6) == 0 || strncmp(first, "---", 3) == 0) continue;
        if (strcmp(first, "sum of passes") == 0) break;
        char *end;
        double ms = strtod(last, &end);
        if (end == last || *Strip(end, BLANKS)) Fail("%s: GPU passes: \"%s\" isn't a number", benchReport, last);
        GROW(rows, capRows, nRows + 1);
        rows[nRows++] = (Pass){ ms, Dup(first) };
    }
    free(text);
    if (!nRows) Fail("no \"GPU passes\" table in %s", benchReport);

    double gpu = 0;
    for (size_t i = 0; i < nRows; i++) gpu += rows[i].ms;
    qsort(rows, nRows, sizeof(*rows), ByMsDown);
    fprintf(out, "\n## GPU: top %d R3D passes (ms per frame, All views)\n\n", top);
    for (size_t i = 0; i < nRows && i < (size_t)top; i++) {
        fprintf(out, "  %6.2f%%  %6.3f ms  %s\n", 100.0 * rows[i].ms / gpu, rows[i].ms, rows[i].name);
    }
    for (size_t i = 0; i < nRows; i++) free(rows[i].name);
    free(rows);
}

// #endregion

// #region The steps of a profile run

typedef struct {
    // profile runs
    const char *name;           // names the output folder
    char **gameArgs;            // the monolith arguments after "--" (NULL-terminated)
    int nGameArgs;
    long long period;           // 0: pick one
    bool spin, keepData;
    const char *out;            // NULL: benchmark/profile/<stamp>_<name>
    char *bin;                  // the profiled binary (absolute)
    // --report DIR
    const char *reportDir;
    // --report-only <perf.txt>
    const char *reportOnly;
    const char *bench, *finishSite;
    bool all;
    int top;
} Options;

static void Usage(void)
{
    fputs("usage: profile [--period N] [--spin] [--keep-data] [--out DIR] [--bin PATH] <name> -- <monolith args...>\n"
          "       profile [--bin PATH] --report DIR\n"
          "       profile --report-only <perf.txt> [--bench REPORT.md] [--finish-site SYM+0xOFF] [--all] [--top N]\n"
          "e.g.   profile spin -- --bench --headless --note \"profile: spinning\"\n"
          "       profile load -- --shot /tmp/x.png --view 1 --reload-test maps/<name>\n"
          "Only --bench --headless and --shot runs (no visible window). See the header of tools/profile.c.\n",
          stderr);
    exit(2);
}

// The repo: two folders above the folder of this program (build/linux-profile/profile)
static char *FindRepoRoot(void)
{
    char *self = realpath("/proc/self/exe", NULL);
    if (!self) Fail("can't find my own path: %s", strerror(errno));
    for (int i = 0; i < 3; i++) {
        char *slash = strrchr(self, '/');
        if (!slash || slash == self) Fail("can't find the repo above %s", self);
        *slash = '\0';
    }
    return self;
}

// The value of option argv[*i] (the next argument)
static const char *Value(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) { fprintf(stderr, "profile: %s needs a value\n", argv[*i]); Usage(); }
    return argv[++*i];
}

static long long Number(const char *option, const char *text)
{
    char *end;
    long long v = strtoll(text, &end, 10);
    if (end == text || *end || v <= 0) Fail("%s wants a positive number, not %s", option, text);
    return v;
}

static Options ParseArgs(int argc, char **argv, const char *root)
{
    Options o = { .top = 20 };
    const char *bin = NULL;
    bool reportOption = false;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        else if (strcmp(a, "--spin") == 0) o.spin = true;
        else if (strcmp(a, "--keep-data") == 0) o.keepData = true;
        else if (strcmp(a, "--period") == 0) o.period = Number(a, Value(argc, argv, &i));
        else if (strcmp(a, "--out") == 0) o.out = Value(argc, argv, &i);
        else if (strcmp(a, "--bin") == 0) bin = Value(argc, argv, &i);
        else if (strcmp(a, "--report") == 0) o.reportDir = Value(argc, argv, &i);
        else if (strcmp(a, "--report-only") == 0) o.reportOnly = Value(argc, argv, &i);
        else if (strcmp(a, "--bench") == 0) { o.bench = Value(argc, argv, &i); reportOption = true; }
        else if (strcmp(a, "--finish-site") == 0) { o.finishSite = Value(argc, argv, &i); reportOption = true; }
        else if (strcmp(a, "--all") == 0) { o.all = true; reportOption = true; }
        else if (strcmp(a, "--top") == 0) {
            long long top = Number(a, Value(argc, argv, &i));
            o.top = top > 1000000 ? 1000000 : (int)top;
            reportOption = true;
        }
        else if (a[0] == '-') { fprintf(stderr, "profile: unknown option %s\n", a); Usage(); }
        else o.name = a;
    }
    o.gameArgs = argv + i;
    o.nGameArgs = argc - i;
    if (reportOption && !o.reportOnly) {
        fprintf(stderr, "profile: --bench, --finish-site, --all and --top go with --report-only\n");
        Usage();
    }
    if (o.reportOnly) return o;

    // the binary, as an absolute path: perf runs it from the repo root
    char *defaultBin = Format("%s/build/linux-profile/monolith", root);
    o.bin = realpath(bin ? bin : defaultBin, NULL);
    if (!o.bin || access(o.bin, X_OK) != 0) Fail("no %s: run ./build.sh profile first", bin ? bin : defaultBin);
    free(defaultBin);
    if (!o.reportDir && (!o.name || o.nGameArgs == 0)) Usage();
    return o;
}

// Step 1. Anything but --bench --headless or --shot would open a visible window
static void RefuseWindowedRuns(const Options *o)
{
    bool bench = false, headless = false, shot = false;
    for (int i = 0; i < o->nGameArgs; i++) {
        bench |= strcmp(o->gameArgs[i], "--bench") == 0;
        headless |= strcmp(o->gameArgs[i], "--headless") == 0;
        shot |= strcmp(o->gameArgs[i], "--shot") == 0;
    }
    if (!(bench && headless) && !shot) {
        fprintf(stderr, "profile: only --bench --headless or --shot runs (anything else opens a visible window)\n");
        exit(2);
    }
}

// A number from a one-line file such as /proc/sys/kernel/perf_event_max_sample_rate; -1 if unreadable
static long long ReadNumber(const char *path)
{
    char *text = ReadFile(path);
    if (!text) return -1;
    char *end;
    long long v = strtoll(text, &end, 10);
    bool ok = end != text;
    free(text);
    return ok ? v : -1;
}

#define MAX_FREQ_FILE "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq"
#define MAX_RATE_FILE "/proc/sys/kernel/perf_event_max_sample_rate"

// Step 2. Cycles per sample: max clock (kHz) x 2.5 / max sample rate = 40% of the limit at the
// top clock
static long long PickPeriod(void)
{
    long long khz = ReadNumber(MAX_FREQ_FILE);
    if (khz <= 0) khz = 5000000;     // no cpufreq (e.g. a VM): assume 5 GHz
    long long rate = ReadNumber(MAX_RATE_FILE);
    if (rate <= 0) Fail("can't read %s", MAX_RATE_FILE);
    return khz * 2500 / rate;
}

// The default output folder: benchmark/profile/<date>_<time>_<name>
static char *DefaultOutDir(const char *root, const char *name)
{
    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", localtime(&now));
    return Format("%s/benchmark/profile/%s_%s", root, stamp, name);
}

// perf.data is large: it goes into a temporary folder of our own under /tmp (the only place where
// the repo's rules allow a plain delete), never into the repo. Removed at the end, and on any
// failure (atexit).
static char g_tempDir[] = "/tmp/monolith-profile.XXXXXX";
static bool g_haveTempDir;

static int RemoveEntry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st;
    (void)ftw;
    return type == FTW_DP ? rmdir(path) : unlink(path);
}

static void RemoveTempDir(void)
{
    if (!g_haveTempDir) return;
    g_haveTempDir = false;
    if (nftw(g_tempDir, RemoveEntry, 16, FTW_DEPTH | FTW_PHYS) != 0) {
        fprintf(stderr, "profile: couldn't delete %s: %s\n", g_tempDir, strerror(errno));
    }
}

static void MakeTempDir(void)
{
    if (!mkdtemp(g_tempDir)) Fail("can't create %s: %s", g_tempDir, strerror(errno));
    g_haveTempDir = true;
    atexit(RemoveTempDir);
}

// Prints the last `n` lines of a file (the end of a failed run's log)
static void PrintTail(const char *path, int n)
{
    char *text = ReadFile(path);
    if (!text) return;
    char *start = text + strlen(text);
    if (start > text && start[-1] == '\n') start--;
    while (start > text && n > 0) {
        start--;
        if (*start == '\n' && --n == 0) start++;
    }
    fputs(start, stderr);
    if (*start && start[strlen(start) - 1] != '\n') fputc('\n', stderr);
    free(text);
}

// Step 3. perf record of the game. The game's output goes to run.log.
static void Record(const Options *o, const char *root, const char *outDir, long long period, const char *perfData)
{
    char *periodText = Format("%lld", period);
    char *runLog = Format("%s/run.log", outDir);
    // the argv: perf record ... -- <bin> <game args...>
    char **argv = Alloc(sizeof(char *) * (size_t)(o->nGameArgs + 16));
    int n = 0;
    char *perfArgs[] = { "perf", "record", "-q", "-e", "cycles:u", "-c", periodText,
                         "--call-graph", "dwarf,16384", "-o", (char *)perfData, "--", o->bin };
    for (size_t i = 0; i < sizeof(perfArgs) / sizeof(*perfArgs); i++) argv[n++] = perfArgs[i];
    for (int i = 0; i < o->nGameArgs; i++) argv[n++] = o->gameArgs[i];
    argv[n] = NULL;
    // vsync off; and glFinish sleeps instead of spinning, so its wait doesn't look like driver
    // work (--spin keeps the driver's default spin-wait)
    const char *env[] = { "__GL_SYNC_TO_VBLANK=0", o->spin ? NULL : "__GL_YIELD=USLEEP", NULL };

    int status = Run(argv, (Child){ .dir = root, .env = env, .stdinPath = "/dev/null",
                                    .stdoutPath = runLog, .stderrPath = STDOUT_TOO });
    if (status != 0) {
        PrintTail(runLog, 20);
        Fail("the run failed (exit status %d, log: %s)", status, runLog);
    }
    free(argv);
    free(runLog);
    free(periodText);
}

// Step 4. perf script: perf.data as text (perf.txt), what the report reads
static void Convert(const char *perfData, const char *perfTxt)
{
    char *argv[] = { "perf", "script", "-i", (char *)perfData, "-F", "comm,tid,time,period,ip,sym,symoff,dso", NULL };
    int status = Run(argv, (Child){ .stdoutPath = perfTxt, .stderrPath = "/dev/null" });
    if (status != 0) Fail("perf script failed (exit status %d)", status);
}

static int CompareStrings(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// Step 5. perf-stats.txt: the period and perf's own counts of samples, throttling and lost
// samples (throttling should be 0: then every share is unbiased). True if perf was throttled.
static bool WriteStats(const char *perfData, const char *statsPath, long long period)
{
    char *argv[] = { "perf", "report", "-i", (char *)perfData, "--stats", NULL };
    char *text = Capture(argv, (Child){ .stderrPath = "/dev/null" });

    // its lines "  SAMPLE events: 10850", ... sorted, without duplicates (sort -u)
    regex_t counts = Regex("^ *(SAMPLE|THROTTLE|LOST|LOST_SAMPLES) events");
    char **lines = NULL;
    size_t nLines = 0, cap = 0, pos = 0;
    char *line;
    while ((line = NextLine(text, &pos))) {
        if (!Matches(&counts, line)) continue;
        GROW(lines, cap, nLines + 1);
        lines[nLines++] = line;
    }
    regfree(&counts);
    qsort(lines, nLines, sizeof(*lines), CompareStrings);

    FILE *f = fopen(statsPath, "w");
    if (!f) Fail("can't write %s: %s", statsPath, strerror(errno));
    fprintf(f, "period %lld cycles, perf_event_max_sample_rate %lld\n", period, ReadNumber(MAX_RATE_FILE));
    bool throttled = false;
    for (size_t i = 0; i < nLines; i++) {
        if (i > 0 && strcmp(lines[i], lines[i - 1]) == 0) continue;
        fprintf(f, "%s\n", lines[i]);
        throttled |= strstr(lines[i], "THROTTLE events") != NULL;
    }
    if (fclose(f) != 0) Fail("can't write %s", statsPath);
    free(lines);
    free(text);
    return throttled;
}

// The fields of a line split at blanks (awk's $1, $2, ...): field `n` (1-based) copied into a heap
// string, or NULL if the line has fewer
static char *Field(const char *line, int n)
{
    for (int i = 1; ; i++) {
        line += strspn(line, BLANKS);
        size_t len = strcspn(line, BLANKS);
        if (len == 0) return NULL;
        if (i == n) {
            char *f = strndup(line, len);
            if (!f) Fail("out of memory");
            return f;
        }
        line += len;
    }
}

// Step 6. The headless bench's glFinish(): the return address after `call glFinish@plt` in Run(),
// as perf prints a caller (return address - 1), e.g. "Run+0x5bfd". NULL if not found.
static char *FindFinishSite(const char *bin)
{
    // Run's address: the nm line "0000000000072870 t Run"
    char *nmArgv[] = { "nm", (char *)bin, NULL };
    char *nm = Capture(nmArgv, (Child){ 0 });
    char *runAddr = NULL, *line;
    size_t pos = 0;
    while (!runAddr && (line = NextLine(nm, &pos))) {
        char *sym = Field(line, 3);
        if (sym && strcmp(sym, "Run") == 0) runAddr = Field(line, 1);
        free(sym);
    }
    free(nm);

    // Run's code: the address of the instruction after its first `call ... <glFinish@plt>`
    //   000000000007286f <Run>:
    //      7846e:	call   2a930 <glFinish@plt>
    //      78473:	cmpq   $0x0,-0xfa0(%rbp)        <- the return address
    char *objdumpArgv[] = { "objdump", "-d", "--no-show-raw-insn", "--disassemble=Run", (char *)bin, NULL };
    char *code = Capture(objdumpArgv, (Child){ 0 });
    regex_t runStart = Regex("^[0-9a-f]+ <Run>:"), anyStart = Regex("^[0-9a-f]+ <.*>:");
    regex_t call = Regex("call.*<glFinish@plt>");
    char *retAddr = NULL;
    bool inRun = false;
    pos = 0;
    while (!retAddr && (line = NextLine(code, &pos))) {
        if (Matches(&runStart, line)) { inRun = true; continue; }
        if (Matches(&anyStart, line)) { inRun = false; continue; }
        if (inRun && Matches(&call, line)) {
            char *next = NextLine(code, &pos);
            char *addr = next ? Field(next, 1) : NULL;
            if (addr) addr[strcspn(addr, ":")] = '\0';
            retAddr = addr;
        }
    }
    regfree(&runStart);
    regfree(&anyStart);
    regfree(&call);
    free(code);

    char *site = NULL;
    if (runAddr && retAddr && *retAddr) {
        site = Format("Run+0x%llx", strtoull(retAddr, NULL, 16) - strtoull(runAddr, NULL, 16) - 1);
    }
    free(runAddr);
    free(retAddr);
    return site;
}

// Step 7. The bench report: the last "report: <path>" line of run.log, if that file exists (NULL
// for a --shot run)
static char *FindBenchReport(const char *runLog)
{
    char *text = ReadFile(runLog);
    if (!text) Fail("can't read %s", runLog);
    char *report = NULL, *line;
    size_t pos = 0;
    while ((line = NextLine(text, &pos))) {
        if (strncmp(line, "report: ", 8) != 0) continue;
        free(report);
        report = Field(line, 2);
    }
    free(text);
    if (report && !IsFile(report)) { free(report); report = NULL; }
    return report;
}

// Step 8. report.txt: the CPU table, and for bench runs the GPU table
static void WriteReport(const char *path, const char *perfTxt, const char *benchReport, const char *finishSite, bool all)
{
    FILE *f = fopen(path, "w");
    if (!f) Fail("can't write %s: %s", path, strerror(errno));
    CpuTable(f, perfTxt, finishSite, all, 20);
    if (benchReport) GpuTable(f, benchReport, 20);
    if (fclose(f) != 0) Fail("can't write %s", path);
}

// #endregion

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    char *root = FindRepoRoot();
    Options o = ParseArgs(argc, argv, root);

    // A report of any perf.txt, to stdout
    if (o.reportOnly) {
        CpuTable(stdout, o.reportOnly, o.finishSite, o.all, o.top);
        if (o.bench) GpuTable(stdout, o.bench, o.top);
        free(root);
        return 0;
    }

    // A profile run (steps 1-5), or --report DIR: the report again from that run's files
    char *outDir;
    if (!o.reportDir) {
        RefuseWindowedRuns(&o);
        long long period = o.period ? o.period : PickPeriod();
        outDir = o.out ? Dup(o.out) : DefaultOutDir(root, o.name);
        MakeDirs(outDir);
        MakeTempDir();
        char *perfData = Format("%s/perf.data", g_tempDir);
        char *perfTxt = Format("%s/perf.txt", outDir);
        char *stats = Format("%s/perf-stats.txt", outDir);
        printf("profile: %s: perf record every %lld cycles into %s ...\n", o.name, period, outDir);

        Record(&o, root, outDir, period, perfData);
        Convert(perfData, perfTxt);
        if (WriteStats(perfData, stats, period)) {
            fprintf(stderr, "profile: WARNING: perf was throttled (see %s): use a larger --period\n", stats);
        }
        if (o.keepData) {
            // (rename() can't move across file systems: /tmp is usually a tmpfs)
            char *mvArgv[] = { "mv", perfData, outDir, NULL };
            if (Run(mvArgv, (Child){ 0 }) != 0) Fail("couldn't move %s to %s", perfData, outDir);
        }
        RemoveTempDir();
        free(perfData);
        free(perfTxt);
        free(stats);
    } else {
        outDir = Dup(o.reportDir);
    }

    // Steps 6-8: the report
    char *perfTxt = Format("%s/perf.txt", outDir);
    char *runLog = Format("%s/run.log", outDir);
    char *reportTxt = Format("%s/report.txt", outDir);
    if (!IsFile(perfTxt) || !IsFile(runLog)) Fail("no %s or %s", perfTxt, runLog);
    char *finishSite = FindFinishSite(o.bin);
    char *benchReport = FindBenchReport(runLog);
    printf("profile: glFinish site %s, bench report %s\n", finishSite ? finishSite : "(none)",
           benchReport ? benchReport : "(none: --shot run, one-time work kept)");
    // (no bench report: a --shot run, where the load is the point, so keep the one-time work)
    WriteReport(reportTxt, perfTxt, benchReport, finishSite, benchReport == NULL);
    printf("profile: %s\n", reportTxt);

    free(finishSite);
    free(benchReport);
    free(reportTxt);
    free(runLog);
    free(perfTxt);
    free(outDir);
    free(o.bin);
    free(root);
    return 0;
}
