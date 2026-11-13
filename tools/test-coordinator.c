// tools/test-coordinator.c: end-to-end test of the request queue and the coordinator launch (src/launcher.c,
// build/tools/request-queue), with no Claude session and no window. Built by
// `./build.sh --tools test-coordinator` into build/tools/test-coordinator; it replaced
// tools/test-coordinator.sh (2026-11-13) and runs the same cases with the same output.
//
//   build/tools/test-coordinator [--keep]    --keep: leave the scratch folder (else it goes to the trash)
//
// How it stays away from everything real:
//   MONOLITH_TERMINAL=build/tools/stub-terminal   runs the command it is given (MONOLITH_STUB_RUN) instead of
//                                                 opening a window, and counts "windows" (MONOLITH_STUB_OPENED)
//   MONOLITH_CLAUDE=build/tools/stub-coordinator  stands in for claude: the coordinator's rules, no session
//   MONOLITH_REQUESTS_DIR=<scratch>/requests      bundles, queue and lock in a scratch folder
//                                                 (build/coordinator-test/<stamp>/), so the developer's own
//                                                 coordinator and requests/ are never touched
//   MONOLITH_WORKSPACE=                           (empty) no i3 workspace switch
// The game runs only with --shot (a hidden window), --new-map and --submit.
// It also sets MONOLITH_LAUNCH=coordinator and MONOLITH_STUB_WORK=6 (each stub "subagent" works 6 s, long
// enough to see overlaps in the queue log), and unsets MONOLITH_STUB_LOG, MONOLITH_CLAUDE_ARGS and
// MONOLITH_MODEL so the developer's settings don't leak in.
//
// Checks: three submits open exactly one terminal; two requests on the same map package never overlap;
// requests on two packages run in parallel; a code request runs alone; the HUD side follows the state file;
// CTRL+K n (--close-submitted) cancels a running request; killing the coordinator mid-run requeues untouched
// requests and reports touched ones on the next start; without a coordinator a queued request is cancelled
// by the game itself; per-request mode still opens one terminal per request without a state file.
// Overlaps are read from the queue log's timestamps (the claim and the end of each request).
//
// Output: one line per step ("  ok: ..." / "  FAIL: ..."), ending "test-coordinator: OK" (exit 0) or
// "test-coordinator: FAILED" (exit 1). Exit 1 also when build/linux/monolith is missing (./build.sh), when
// flock (util-linux) is missing, or when --new-map fails; when a program of build/tools/ is missing, the
// message says to run ./build.sh --tools <name>.
//
// Called by: people and sessions after a change to the launcher, the queue or the coordinator's rules (the
// engine-code and large-engine-work skills, CONTRIBUTING.md). It takes about a minute.

#include "tool.h"

#include <sys/file.h>

// #region The scratch folder and small helpers

static const char *gGame;     // build/linux/monolith (absolute)
static const char *gT;        // the scratch folder build/coordinator-test/<stamp> (absolute)
static const char *gR;        // <scratch>/requests (MONOLITH_REQUESTS_DIR)
static const char *gLog;      // <scratch>/requests/.coordinator/log, the queue log
static bool gFail = false;    // set by Bad: the test's result
static int gN = 0;            // the number of submits so far (names submit-<n>.out)

// "  ok: <text>".
NOB_PRINTF_FORMAT(1, 2) static void Ok(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fputs("  ok: ", stdout);
    vprintf(fmt, args);
    fputc('\n', stdout);
    va_end(args);
    fflush(stdout);
}

// "  FAIL: <text>", and the test fails.
NOB_PRINTF_FORMAT(1, 2) static void Bad(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fputs("  FAIL: ", stdout);
    vprintf(fmt, args);
    fputc('\n', stdout);
    va_end(args);
    fflush(stdout);
    gFail = true;
}

// A plain line of output (the script's echo).
NOB_PRINTF_FORMAT(1, 2) static void Echo(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    fputc('\n', stdout);
    va_end(args);
    fflush(stdout);
}

// The whole file as a heap string ("" when it can't be read), silently.
static char *Slurp(const char *path)
{
    Nob_String_Builder sb = {0};
    FILE *f = fopen(path, "rb");
    if (f) {
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) nob_sb_append_buf(&sb, buf, n);
        fclose(f);
    }
    nob_sb_append_null(&sb);
    return sb.items;
}

// Calls fn for each line of text (without its newline; a last line without one counts, as grep reads it),
// until fn returns false.
static void EachLine(const char *text, bool (*fn)(const char *line, size_t len, void *ctx), void *ctx)
{
    for (const char *p = text; *p;) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (!fn(p, len, ctx)) return;
        if (!end) return;
        p = end + 1;
    }
}

// The context of the line searches below: what to look for and the line found (heap string or NULL).
typedef struct {
    const char *needle;
    bool (*match)(const char *line, size_t len, const char *needle);
    char *found;
    bool last;  // keep the last match (`grep ... | tail -n1`) instead of the first (`grep -m1`)
} Search;

// EachLine callback of FindLine.
static bool SearchLine(const char *line, size_t len, void *ctx)
{
    Search *s = ctx;
    if (!s->match(line, len, s->needle)) return true;
    free(s->found);
    s->found = strndup(line, len);
    return s->last;
}

// The first (or last) line of the file for which match() is true, as a heap string; NULL when none.
static char *FindLine(const char *path, bool (*match)(const char *, size_t, const char *), const char *needle,
                      bool last)
{
    char *text = Slurp(path);
    Search s = {needle, match, NULL, last};
    EachLine(text, SearchLine, &s);
    free(text);
    return s.found;
}

// match(): the line contains the needle (`grep -q 'text'` for a pattern without special characters).
static bool Contains(const char *line, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(line + i, needle, n) == 0) return true;
    }
    return false;
}

// match(): the line starts with the needle (`grep '^text'`).
static bool StartsWith(const char *line, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    return len >= n && memcmp(line, needle, n) == 0;
}

// match(): the line is exactly the needle (`grep -x`).
static bool Exactly(const char *line, size_t len, const char *needle)
{
    return strlen(needle) == len && memcmp(line, needle, len) == 0;
}

// `grep -q`: whether some line of the file matches.
static bool FileHas(const char *path, bool (*match)(const char *, size_t, const char *), const char *needle)
{
    char *line = FindLine(path, match, needle, false);
    bool found = line != NULL;
    free(line);
    return found;
}

// `head -n1 file`, without the newline; heap string ("" for a missing file).
static char *Head1(const char *path)
{
    char *text = Slurp(path);
    text[strcspn(text, "\n")] = '\0';
    return text;
}

// Opened: how many "windows" the stub terminal opened (`wc -l < opened.txt`: its newlines), 0 without the file.
static int Opened(void)
{
    char *text = Slurp(nob_temp_sprintf("%s/opened.txt", gT));
    int n = 0;
    for (char *p = text; *p; p++) n += *p == '\n';
    free(text);
    return n;
}

// StateOf <id>: the first word of the bundle's state file, or "none". Heap string.
static char *StateOf(const char *id)
{
    char *text = Slurp(nob_temp_sprintf("%s/%s/state", gR, id));
    char *p = text + strspn(text, " \t");
    size_t n = strcspn(p, " \t\n");
    char *word = n ? strndup(p, n) : strdup("none");
    free(text);
    return word;
}

// Whether the bundle's state is the given one.
static bool StateIs(const char *id, const char *state)
{
    char *s = StateOf(id);
    bool is = strcmp(s, state) == 0;
    free(s);
    return is;
}

// The second space-separated field of the last line of opened.txt (`tail -n1 | cut -d' ' -f2`): the process
// group of the newest stub terminal, i.e. of its coordinator. Heap string ("" when there is none).
static char *LastGroup(void)
{
    char *text = Slurp(nob_temp_sprintf("%s/opened.txt", gT));
    size_t len = strlen(text);
    while (len > 0 && text[len - 1] == '\n') len--;  // tail -n1: the last line, before its newline
    text[len] = '\0';
    char *line = strrchr(text, '\n');
    line = line ? line + 1 : text;
    char *space = strchr(line, ' ');
    char *field = strdup(space ? nob_temp_sprintf("%.*s", (int)strcspn(space + 1, " "), space + 1) : "");
    free(text);
    return field;
}

// `kill -TERM -- -<group>`: true when the signal was sent.
static bool KillGroup(const char *group)
{
    char *end;
    long pg = strtol(group, &end, 10);
    if (!*group || *end || pg <= 0) return false;
    return kill(-(pid_t)pg, SIGTERM) == 0;
}

// Sleeps the given seconds.
static void Sleep(double s)
{
    struct timespec ts = {(time_t)s, (long)((s - (double)(time_t)s) * 1e9)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

// #endregion

// #region Submits and waiting

// Submit <package> <text> [extra game flags...]: the game with --shot (hidden window), the package and the
// text as a submitted request, its output in submit-<n>.out. Prints "  submit <n> (<package name>): <id>
// (<the game's first submit: line, cut to 160 bytes>)". Returns the new bundle's id (heap string), or NULL
// when the game reported none (then the test fails).
static char *Submit(const char *map, const char *text, const char **extra, size_t extraCount)
{
    gN++;
    const char *outPath = strdup(nob_temp_sprintf("%s/submit-%d.out", gT, gN));
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, gGame, "--shot", nob_temp_sprintf("%s/shot.png", gT), "--shot-size", "320", "180", "--map",
                   map);
    nob_da_append_many(&cmd, extra, extraCount);
    nob_cmd_append(&cmd, "--submit", text);
    ToolRun(&cmd, (ToolIo){.out = outPath, .errToOut = true});
    nob_cmd_free(cmd);

    // the id: from "submit: ok: request queued [n]...: <id> ..." or "submit: ok: request sent (...): <id> ..."
    char *out = Slurp(outPath);
    char *id = NULL;
    for (int pass = 0; pass < 2 && !id; pass++) {
        for (char *line = out; *line && !id;) {
            char *nl = strchr(line, '\n');
            size_t len = nl ? (size_t)(nl - line) : strlen(line);
            char *l = strndup(line, len);
            const char *p = NULL;
            if (pass == 0 && strncmp(l, "submit: ok: request queued [", 28) == 0) {
                // [0-9]+ then ] then [^:]* then ": "
                const char *q = l + 28;
                const char *digits = q;
                while (*q >= '0' && *q <= '9') q++;
                if (q > digits && *q == ']') {
                    q = strchr(q, ':');
                    if (q && q[1] == ' ') p = q + 2;
                }
            } else if (pass == 1 && strncmp(l, "submit: ok: request sent (", 26) == 0) {
                // [^)]* then "): "
                const char *q = strchr(l + 26, ')');
                if (q && q[1] == ':' && q[2] == ' ') p = q + 3;
            }
            if (p) {
                // ([^ ]+) followed by a space
                size_t n = strcspn(p, " ");
                if (n > 0 && p[n] == ' ') id = strndup(p, n);
            }
            free(l);
            line = nl ? nl + 1 : line + len;
        }
    }

    // the first "submit:" line, cut to 160 bytes (cut -c1-160 counts bytes)
    char *first = FindLine(outPath, StartsWith, "submit:", false);
    if (first && strlen(first) > 160) first[160] = '\0';
    const char *slash = strrchr(map, '/');
    Echo("  submit %d (%s): %s (%s)", gN, slash ? slash + 1 : map, id ? id : "FAILED", first ? first : "");
    if (!id) Bad("submit %d failed (see %s)", gN, outPath);
    free(first);
    free(out);
    free((char *)outPath);
    return id;
}

// A condition WaitFor polls: called with up to 3 ids.
typedef bool (*Condition)(const char **ids, size_t count);

// AllFinal: every bundle is done, failed or cancelled.
static bool AllFinal(const char **ids, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        char *s = StateOf(ids[i] ? ids[i] : "");
        bool final = strcmp(s, "done") == 0 || strcmp(s, "failed") == 0 || strcmp(s, "cancelled") == 0;
        free(s);
        if (!final) return false;
    }
    return true;
}

// AllClaimed: every bundle is claimed.
static bool AllClaimed(const char **ids, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (!StateIs(ids[i] ? ids[i] : "", "claimed")) return false;
    }
    return true;
}

// LockFree: nobody holds the coordinator's lock (`flock -n lock true`: open it, creating it as flock(1)
// does, and try to take it without waiting; the lock goes with the descriptor).
static bool LockFree(const char **ids, size_t count)
{
    (void)ids;
    (void)count;
    int fd = open(nob_temp_sprintf("%s/.coordinator/lock", gR), O_RDONLY | O_CREAT | O_NOCTTY, 0666);
    if (fd < 0) return false;
    bool got = flock(fd, LOCK_EX | LOCK_NB) == 0;
    close(fd);
    return got;
}

// WaitFor <seconds> <condition>: polls every 0.2 s; false when the condition still fails after the given
// whole seconds (the script's SECONDS arithmetic).
static bool WaitFor(int seconds, Condition cond, const char **ids, size_t count)
{
    time_t t0 = time(NULL);
    while (!cond(ids, count)) {
        if (time(NULL) - t0 >= seconds) return false;
        Sleep(0.2);
    }
    return true;
}

// #endregion

// #region Run intervals from the queue log

// match(): a queue-log line " claimed <id>" followed by a space or the line's end (`grep -E " claimed $id( |$)"`).
static bool ClaimedLine(const char *line, size_t len, const char *id)
{
    const char *needle = nob_temp_sprintf(" claimed %s", id);
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(line + i, needle, n) == 0 && (i + n == len || line[i + n] == ' ')) return true;
    }
    return false;
}

// match(): a queue-log line ending in " done <id>", " failed <id>" or " cancelled <id>"
// (`grep -E " (done|failed|cancelled) $id$"`).
static bool EndLine(const char *line, size_t len, const char *id)
{
    const char *words[] = {" done ", " failed ", " cancelled "};
    for (size_t k = 0; k < NOB_ARRAY_LEN(words); k++) {
        const char *needle = nob_temp_sprintf("%s%s", words[k], id);
        size_t n = strlen(needle);
        if (len >= n && memcmp(line + len - n, needle, n) == 0) return true;
    }
    return false;
}

// Sec: the time of day of a log line ("YYYY-MM-DD HH:MM:SS.mmm ...") in seconds, formatted "%.3f" (the
// script's awk -F'[: ]' '{ printf "%.3f", $2 * 3600 + $3 * 60 + $4 }'). Heap string.
static char *Sec(const char *line)
{
    const char *p = strchr(line, ' ');
    double h = 0, m = 0, s = 0;
    if (p) {
        h = strtod(p + 1, NULL);
        const char *c1 = strchr(p + 1, ':');
        if (c1) {
            m = strtod(c1 + 1, NULL);
            const char *c2 = strchr(c1 + 1, ':');
            if (c2) s = strtod(c2 + 1, NULL);
        }
    }
    return strdup(nob_temp_sprintf("%.3f", h * 3600 + m * 60 + s));
}

// Interval <id>: "<claimed s> <end s>" from the queue log (the last claim and the last end of the id; seconds
// of the day, ms precision), or "? ?" when either is missing. Heap string.
static char *Interval(const char *id)
{
    char *c = FindLine(gLog, ClaimedLine, id, true);
    char *e = FindLine(gLog, EndLine, id, true);
    char *result;
    if (!c || !e) {
        result = strdup("? ?");
    } else {
        char *a = Sec(c), *b = Sec(e);
        result = strdup(nob_temp_sprintf("%s %s", a, b));
        free(a);
        free(b);
    }
    free(c);
    free(e);
    return result;
}

// Overlap <id> <id>: 0 when their run intervals overlap, 1 when they don't, 2 when one is unknown.
static int Overlap(const char *a, const char *b)
{
    char *ia = Interval(a ? a : ""), *ib = Interval(b ? b : "");
    int result;
    if (ia[0] == '?' || ib[0] == '?') {
        result = 2;
    } else {
        // the numbers as awk compared them: the formatted values read back
        char *end;
        double a0 = strtod(ia, &end), a1 = strtod(end, NULL);
        double b0 = strtod(ib, &end), b1 = strtod(end, NULL);
        result = (a0 < b1 && b0 < a1) ? 0 : 1;
    }
    free(ia);
    free(ib);
    return result;
}

// Show <ids...>: one line per bundle: id, state and run interval in columns.
static void Show(const char **ids, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const char *id = ids[i] ? ids[i] : "";
        char *s = StateOf(id);
        char *in = Interval(id);
        Echo("    %-60s %-9s %s", id, s, in);
        free(s);
        free(in);
    }
}

// #endregion

// #region main

// `command -v name`: whether the program is found in PATH.
static bool InPath(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) return false;
    for (const char *d = path;;) {
        size_t len = strcspn(d, ":");
        const char *cand = nob_temp_sprintf("%.*s/%s", (int)(len ? len : 1), len ? d : ".", name);
        if (access(cand, X_OK) == 0) return true;
        if (!d[len]) return false;
        d += len + 1;
    }
}

// Sets an environment variable for one call (the script's `VAR=value Submit ...`), remembering the old
// value; RestoreEnv puts it back.
typedef struct {
    const char *name;
    char *old;  // heap copy, or NULL when it was unset
} SavedEnv;

// Sets name=value in this process's environment (inherited by the next command it runs) and returns the old value.
static SavedEnv SetEnvFor(const char *name, const char *value)
{
    const char *old = getenv(name);
    SavedEnv saved = {name, old ? strdup(old) : NULL};
    setenv(name, value, 1);
    return saved;
}

// Puts back the value SetEnvFor replaced (or unsets the variable when it was unset) and frees the copy.
static void RestoreEnv(SavedEnv saved)
{
    if (saved.old) setenv(saved.name, saved.old, 1);
    else unsetenv(saved.name);
    free(saved.old);
}

// The text of a check that compares the number of terminals.
static const char *Terminals(void)
{
    return nob_temp_sprintf("%d", Opened());
}

int main(int argc, char **argv)
{
    ToolInit(argv, "test-coordinator", "test-coordinator");
    // cd "$ROOT": the game, the bundles' paths and the messages are relative to the repo root
    if (chdir(gToolRoot) != 0) ToolDie(1, "cd %s: %s", gToolRoot, strerror(errno));
    bool keep = argc > 1 && strcmp(argv[1], "--keep") == 0;

    // what the test needs: the game, flock (the game's lock wrapper uses it), the three programs
    gGame = strdup(nob_temp_sprintf("%s/build/linux/monolith", gToolRoot));
    if (access(gGame, X_OK) != 0) ToolDie(1, "%s missing (./build.sh)", gGame);
    if (!InPath("flock")) ToolDie(1, "flock (util-linux) missing");
    const char *terminal = strdup(ToolProgram("stub-terminal"));
    const char *claude = strdup(ToolProgram("stub-coordinator"));
    ToolProgram("request-queue");  // stub-coordinator runs it: say now, not inside coordinator.out

    // the scratch folder build/coordinator-test/<YYYYmmddHHMMSS>/requests
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d%H%M%S", &tm);
    gT = strdup(nob_temp_sprintf("%s/build/coordinator-test/%s", gToolRoot, stamp));
    gR = strdup(nob_temp_sprintf("%s/requests", gT));
    gLog = strdup(nob_temp_sprintf("%s/.coordinator/log", gR));
    if (!ToolMkdirP(gR)) return 1;

    // the environment every game run and its children see
    setenv("MONOLITH_REQUESTS_DIR", gR, 1);
    setenv("MONOLITH_TERMINAL", terminal, 1);
    setenv("MONOLITH_STUB_RUN", nob_temp_sprintf("%s/coordinator.out", gT), 1);
    setenv("MONOLITH_STUB_OPENED", nob_temp_sprintf("%s/opened.txt", gT), 1);
    setenv("MONOLITH_CLAUDE", claude, 1);
    setenv("MONOLITH_WORKSPACE", "", 1);
    setenv("MONOLITH_LAUNCH", "coordinator", 1);
    setenv("MONOLITH_STUB_WORK", "6", 1);
    unsetenv("MONOLITH_STUB_LOG");
    unsetenv("MONOLITH_CLAUDE_ARGS");
    unsetenv("MONOLITH_MODEL");
    const char *rootSlash = nob_temp_sprintf("%s/", gToolRoot);
    size_t rootLen = strlen(rootSlash);

    Echo("test-coordinator: scratch folder %s", gT + rootLen);
    // two fresh map packages in the scratch folder
    const char *A = strdup(nob_temp_sprintf("%s/map_a", gT));
    const char *B = strdup(nob_temp_sprintf("%s/map_b", gT));
    const char *names[2] = {"a", "b"};
    const char *maps[2] = {A, B};
    for (int i = 0; i < 2; i++) {
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, gGame, "--new-map", maps[i]);
        const char *out = nob_temp_sprintf("%s/newmap-%s.out", gT, names[i]);
        int status = ToolRun(&cmd, (ToolIo){.out = out, .errToOut = true});
        nob_cmd_free(cmd);
        if (status != 0) {
            fflush(stdout);
            fprintf(stderr, "--new-map failed\n");
            return 1;
        }
    }

    // 1. three submits, one terminal, the same package in sequence, two packages in parallel
    Echo("1. three submits (two on map_a, one on map_b): one terminal, same map in sequence, two maps in parallel");
    char *A1 = Submit(A, "test one on map a", NULL, 0);
    char *A2 = Submit(A, "test two on map a", NULL, 0);
    char *B1 = Submit(B, "test one on map b", NULL, 0);
    if (Opened() == 1) Ok("exactly one terminal opened for three submits");
    else Bad("%s terminals opened (want 1)", Terminals());
    const char *s1[] = {A1, A2, B1};
    if (!WaitFor(60, AllFinal, s1, 3)) Bad("requests not finished after 60 s");
    Show(s1, 3);
    int st = Overlap(A1, A2);
    if (st == 1) Ok("same package: %s and %s did not overlap", A1 ? A1 : "", A2 ? A2 : "");
    else Bad("same package: overlap (status %d)", st);
    if (Overlap(A1, B1) == 0 || Overlap(A2, B1) == 0) Ok("different packages ran in parallel");
    else Bad("map_b's request did not run in parallel with map_a's");
    if (Opened() == 1) Ok("still one terminal");
    else Bad("%s terminals", Terminals());

    // 2. a code request runs alone; the game's HUD follows the state file
    Echo("2. a code request runs alone; the game's HUD side follows the state");
    char *C1 = Submit(A, "[code] test engine change", NULL, 0);
    const char *waitLong[] = {"--submit-wait", "20"};
    char *B2 = Submit(B, "test two on map b", waitLong, 2);
    const char *s2[] = {C1, B2};
    if (!WaitFor(60, AllFinal, s2, 2)) Bad("not finished after 60 s");
    Show(s2, 2);
    st = Overlap(C1, B2);
    if (st == 1) Ok("the code request ran alone");
    else Bad("the code request overlapped (status %d)", st);
    // (the game counts a request done as soon as result.md has a line, so it may never see "claimed -> done":
    // the stub writes result.md a moment before finishing the state)
    const char *out2 = strdup(nob_temp_sprintf("%s/submit-%d.out", gT, gN));
    if (FileHas(out2, Contains, nob_temp_sprintf("request %s: queued -> claimed", B2 ? B2 : "")) &&
        FileHas(out2, Contains, nob_temp_sprintf("request %s done: Stub coordinator", B2 ? B2 : ""))) {
        Ok("the game saw queued -> claimed, then done with result.md line 1");
    } else {
        Bad("the game's log lacks the state changes of %s (see %s)", B2 ? B2 : "", out2);
    }

    // 3. CTRL+K n cancels a running request
    Echo("3. CTRL+K n on a running request cancels it");
    const char *cancelFlags[] = {"--submit-wait", "2.5", "--close-submitted"};
    char *X1 = Submit(A, "test cancel me", cancelFlags, 3);
    const char *out3 = strdup(nob_temp_sprintf("%s/submit-%d.out", gT, gN));
    // grep -q "^close \[[0-9]*\]: cancelling", and the message is the first line "^close .*cancelling"
    bool asked = false;
    char *closeText = Slurp(out3);
    for (char *line = closeText; *line && !asked;) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (len >= 7 && strncmp(line, "close [", 7) == 0) {
            size_t q = 7;
            while (q < len && line[q] >= '0' && line[q] <= '9') q++;
            asked = len - q >= 13 && strncmp(line + q, "]: cancelling", 13) == 0;
        }
        line = nl ? nl + 1 : line + len;
    }
    free(closeText);
    if (asked) {
        char *msg = NULL;
        char *text = Slurp(out3);
        for (char *line = text; *line && !msg;) {
            char *nl = strchr(line, '\n');
            size_t len = nl ? (size_t)(nl - line) : strlen(line);
            if (len >= 6 && strncmp(line, "close ", 6) == 0 && Contains(line + 6, len - 6, "cancelling")) {
                msg = strndup(line, len);
            }
            line = nl ? nl + 1 : line + len;
        }
        free(text);
        Ok("the game asked for the cancel (%s)", msg ? msg : "");
        free(msg);
    } else {
        Bad("no cancel message (see %s)", out3);
    }
    const char *s3[] = {X1};
    if (!WaitFor(20, AllFinal, s3, 1)) Bad("not finished");
    if (StateIs(X1 ? X1 : "", "cancelled")) {
        char *h = Head1(nob_temp_sprintf("%s/%s/result.md", gR, X1));
        Ok("state cancelled: %s", h);
        free(h);
    } else {
        char *s = StateOf(X1 ? X1 : "");
        Bad("state %s", s);
        free(s);
    }

    // 4. killing the coordinator mid-run: the next submit starts a new one, which requeues or reports
    Echo("4. killing the coordinator mid-run: the next submit starts a new one, which requeues / reports");
    char *A3 = Submit(A, "[edit] test interrupted on map a", NULL, 0);
    char *B3 = Submit(B, "test untouched on map b", NULL, 0);
    const char *s4[] = {A3, B3};
    if (!WaitFor(30, AllClaimed, s4, 2)) Bad("not claimed");
    char *pg = LastGroup();
    if (KillGroup(pg)) Ok("killed the coordinator's process group %s while both were claimed", pg);
    else Bad("kill %s failed", pg);
    free(pg);
    if (WaitFor(10, LockFree, NULL, 0)) Ok("the lock was released");
    else Bad("the lock is still held");
    if (StateIs(A3 ? A3 : "", "claimed") && StateIs(B3 ? B3 : "", "claimed")) {
        Ok("both still claimed after the kill");
    } else {
        char *sa = StateOf(A3 ? A3 : ""), *sb = StateOf(B3 ? B3 : "");
        Bad("states: %s %s", sa, sb);
        free(sa);
        free(sb);
    }
    char *B4 = Submit(B, "test after restart on map b", NULL, 0);
    if (Opened() == 2) Ok("the submit started a second coordinator");
    else Bad("%s terminals (want 2)", Terminals());
    const char *s4b[] = {A3, B3, B4};
    if (!WaitFor(60, AllFinal, s4b, 3)) Bad("not finished after 60 s");
    Show(s4b, 3);
    if (FileHas(gLog, Contains, nob_temp_sprintf("requeued %s", B3 ? B3 : "")) && StateIs(B3 ? B3 : "", "done")) {
        Ok("untouched %s was requeued and done", B3);
    } else {
        char *s = StateOf(B3 ? B3 : "");
        Bad("%s: %s", B3 ? B3 : "", s);
        free(s);
    }
    {
        const char *result = nob_temp_sprintf("%s/%s/result.md", gR, A3 ? A3 : "");
        char *h = Head1(result);
        if (StateIs(A3 ? A3 : "", "failed") && strncmp(h, "Interrupted", 11) == 0 &&
            FileHas(result, Contains, "stub-edit.txt")) {
            Ok("touched %s was reported: %s (lists stub-edit.txt)", A3, h);
        } else {
            char *s = StateOf(A3 ? A3 : "");
            Bad("%s: %s", A3 ? A3 : "", s);
            free(s);
        }
        free(h);
    }
    st = Overlap(B3, B4);
    if (st == 1) Ok("after the restart map_b's requests still ran in sequence");
    else Bad("overlap %s / %s (status %d)", B3 ? B3 : "", B4 ? B4 : "", st);

    // 5. no coordinator: the game cancels a queued request itself
    Echo("5. no coordinator: a queued request is cancelled by the game itself");
    pg = LastGroup();
    KillGroup(pg);
    free(pg);
    if (!WaitFor(10, LockFree, NULL, 0)) Bad("the lock is still held");
    SavedEnv noClaude = SetEnvFor("MONOLITH_CLAUDE", "/nonexistent/claude");
    const char *noCoordFlags[] = {"--submit-wait", "0.5", "--close-submitted"};
    char *Y1 = Submit(A, "test no coordinator", noCoordFlags, 3);
    RestoreEnv(noClaude);
    if (FileHas(nob_temp_sprintf("%s/submit-%d.out", gT, gN), Contains, "coordinator could not start")) {
        Ok("the start failure was reported, the request stayed queued");
    } else {
        Bad("no start failure message");
    }
    if (StateIs(Y1 ? Y1 : "", "cancelled")) {
        char *h = Head1(nob_temp_sprintf("%s/%s/result.md", gR, Y1));
        Ok("the game cancelled it: %s", h);
        free(h);
    } else {
        char *s = StateOf(Y1 ? Y1 : "");
        Bad("state %s", s);
        free(s);
    }

    // 6. per-request mode: one terminal per request, no state file, the old prompt
    Echo("6. per-request mode (MONOLITH_LAUNCH=per-request): one terminal per request, no state file");
    const char *argvLog = strdup(nob_temp_sprintf("%s/per-request-argv.txt", gT));
    SavedEnv launch = SetEnvFor("MONOLITH_LAUNCH", "per-request");
    SavedEnv run = SetEnvFor("MONOLITH_STUB_RUN", "");
    SavedEnv stubLog = SetEnvFor("MONOLITH_STUB_LOG", argvLog);
    char *P1 = Submit(A, "test per request mode", NULL, 0);
    RestoreEnv(stubLog);
    RestoreEnv(run);
    RestoreEnv(launch);
    struct stat st6;
    if (P1 && stat(nob_temp_sprintf("%s/%s/state", gR, P1), &st6) != 0 &&
        FileHas(argvLog, Exactly,
                nob_temp_sprintf("Map edit request: read requests/%s/request.md and carry it out.", P1))) {
        Ok("per-request terminal got the old prompt; no state file");
    } else {
        Bad("per-request mode (see %s)", argvLog);
    }

    Echo("--- coordinator output: %s/coordinator.out, queue log: %s", gT + rootLen,
         strncmp(gLog, rootSlash, rootLen) == 0 ? gLog + rootLen : gLog);
    if (!keep && ToolTrash(gT, true) != 0) Echo("test-coordinator: could not trash %s", gT);
    if (!gFail) {
        Echo("test-coordinator: OK");
        return 0;
    }
    Echo("test-coordinator: FAILED");
    return 1;
}

// #endregion
