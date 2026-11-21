// tools/test-coordinator.c -> build/tools/test-coordinator: end-to-end test of the request queue and the
// coordinator launch, with no Claude session and no window (stub terminal, stub coordinator, a scratch
// requests folder; monolith only with --shot, --new-map and --submit). Ends "test-coordinator: OK" or FAILED.
// Reference: docs/code/test-coordinator.md

#include "tool.h"

#include <dirent.h>
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
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    fputs("  ok: ", stdout);
    vprintf(fmt, arg_ptr);
    fputc('\n', stdout);
    va_end(arg_ptr);
    fflush(stdout);
}

// "  FAIL: <text>", and the test fails.
NOB_PRINTF_FORMAT(1, 2) static void Bad(const char *fmt, ...)
{
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    fputs("  FAIL: ", stdout);
    vprintf(fmt, arg_ptr);
    fputc('\n', stdout);
    va_end(arg_ptr);
    fflush(stdout);
    gFail = true;
}

// A plain line of output (the script's echo).
NOB_PRINTF_FORMAT(1, 2) static void Echo(const char *fmt, ...)
{
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    vprintf(fmt, arg_ptr);
    fputc('\n', stdout);
    va_end(arg_ptr);
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
        if (fclose(f) != 0) ToolErr("%s: %s", path, strerror(errno));
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

// Submit <package> <text> [extra monolith flags...]: monolith with --shot (hidden window), the package and the
// text as a submitted request, its output in submit-<n>.out. Prints "  submit <n> (<package name>): <id>
// (<the editor's first submit: line, cut to 160 bytes>)". Returns the new bundle's id (heap string), or NULL
// when the editor reported none (then the test fails).
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
            } else if (pass == 1 && strncmp(l, "submit: ok: request classifying [", 33) == 0) {
                // a classified submit: "... classifying [n]: <id>" (the id ends the line)
                const char *q = strstr(l + 33, "]: ");
                if (q) {
                    id = strndup(q + 3, strcspn(q + 3, " "));
                    if (!*id) { free(id); id = NULL; }
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
    if (close(fd) != 0) ToolErr("lock: %s", strerror(errno));
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

// The folder <scratch>/no-gio-bin: a symlink to every program in $PATH's folders but gio (the first of a name
// wins, as in a PATH search), so a run with PATH=<it> finds everything except gio, without anything being
// removed from the system. Made on the first call; returns its path (a temp string). The test fails when it
// can't be made.
static const char *NoGioFolder(void)
{
    const char *dir = nob_temp_sprintf("%s/no-gio-bin", gT);
    struct stat st;
    if (stat(dir, &st) == 0) return dir;
    if (!ToolMkdirP(dir)) {
        Bad("can't make %s", dir);
        return dir;
    }
    const char *path = getenv("PATH");
    for (const char *d = path ? path : ""; *d;) {
        size_t len = strcspn(d, ":");
        char *entry = strndup(len ? d : ".", len ? len : 1);
        char *folder = entry ? realpath(entry, NULL) : NULL;  // absolute link targets, whatever the entry says
        DIR *h = folder ? opendir(folder) : NULL;
        for (struct dirent *e; h && (e = readdir(h));) {
            if (e->d_name[0] == '.' || strcmp(e->d_name, "gio") == 0) continue;
            size_t mark = nob_temp_save();  // thousands of names: give the temp buffer back after each
            const char *from = nob_temp_sprintf("%s/%s", folder, e->d_name);
            const char *to = nob_temp_sprintf("%s/%s", dir, e->d_name);
            if (stat(from, &st) == 0 && !S_ISDIR(st.st_mode) && access(from, X_OK) == 0 && lstat(to, &st) != 0 &&
                symlink(from, to) != 0) {
                Bad("symlink %s: %s", to, strerror(errno));
            }
            nob_temp_rewind(mark);
        }
        if (h && closedir(h) != 0) ToolErr("%s: %s", folder, strerror(errno));
        free(folder);
        free(entry);
        d += len;
        if (*d == ':') d++;
    }
    return dir;
}

// The text of a check that compares the number of terminals.
static const char *Terminals(void)
{
    return nob_temp_sprintf("%d", Opened());
}

int main(int argc, char **argv)
{
    ToolInit(argv, "test-coordinator", "test-coordinator");
    // cd "$ROOT": monolith, the bundles' paths and the messages are relative to the repo root
    if (chdir(gToolRoot) != 0) ToolDie(1, "cd %s: %s", gToolRoot, strerror(errno));
    bool keep = argc > 1 && strcmp(argv[1], "--keep") == 0;

    // what the test needs: monolith, flock (the editor's lock wrapper uses it), gio (the scratch folder goes to
    // the trash at the end, and the editor refuses to start a coordinator without it), the three programs
    gGame = strdup(nob_temp_sprintf("%s/build/linux/monolith", gToolRoot));
    if (access(gGame, X_OK) != 0) ToolDie(1, "%s missing (./build.sh)", gGame);
    ToolNeed("flock", "the editor's coordinator lock needs it; install util-linux and run again");
    ToolNeedGio();
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

    // the environment every monolith run and its children see
    setenv("MONOLITH_REQUESTS_DIR", gR, 1);
    setenv("MONOLITH_TERMINAL", terminal, 1);
    setenv("MONOLITH_STUB_RUN", nob_temp_sprintf("%s/coordinator.out", gT), 1);
    setenv("MONOLITH_STUB_OPENED", nob_temp_sprintf("%s/opened.txt", gT), 1);
    setenv("MONOLITH_CLAUDE", claude, 1);
    setenv("MONOLITH_WORKSPACE", "", 1);
    setenv("MONOLITH_LAUNCH", "coordinator", 1);
    setenv("MONOLITH_STUB_WORK", "6", 1);
    setenv("MONOLITH_CLASSIFIER", "off", 1);
    unsetenv("MONOLITH_STUB_LOG");
    unsetenv("MONOLITH_CLAUDE_ARGS");
    unsetenv("MONOLITH_MODEL");
    // the developer's accounts stay out (sections 7 and 8 set their own)
    unsetenv("MONOLITH_CLAUDE_FALLBACK");
    unsetenv("MONOLITH_CLAUDE_ENV");
    unsetenv("MONOLITH_ACCOUNT");
    unsetenv("MONOLITH_STUB_LIMITED");
    unsetenv("MONOLITH_STUB_COORD_LIMIT");
    unsetenv("MONOLITH_LIMIT_RETRY");
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

    // 2. a code request runs alone; the editor's HUD follows the state file
    Echo("2. a code request runs alone; the editor's HUD side follows the state");
    char *C1 = Submit(A, "[code] test engine change", NULL, 0);
    const char *waitLong[] = {"--submit-wait", "20"};
    char *B2 = Submit(B, "test two on map b", waitLong, 2);
    const char *s2[] = {C1, B2};
    if (!WaitFor(60, AllFinal, s2, 2)) Bad("not finished after 60 s");
    Show(s2, 2);
    st = Overlap(C1, B2);
    if (st == 1) Ok("the code request ran alone");
    else Bad("the code request overlapped (status %d)", st);
    // (the editor counts a request done as soon as result.md has a line, so it may never see "claimed -> done":
    // the stub writes result.md a moment before finishing the state)
    const char *out2 = strdup(nob_temp_sprintf("%s/submit-%d.out", gT, gN));
    if (FileHas(out2, Contains, nob_temp_sprintf("request %s: queued -> claimed", B2 ? B2 : "")) &&
        FileHas(out2, Contains, nob_temp_sprintf("request %s done: Stub coordinator", B2 ? B2 : ""))) {
        Ok("the editor saw queued -> claimed, then done with result.md line 1");
    } else {
        Bad("the editor's log lacks the state changes of %s (see %s)", B2 ? B2 : "", out2);
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
        Ok("the editor asked for the cancel (%s)", msg ? msg : "");
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

    // 5. no coordinator: the editor cancels a queued request itself
    Echo("5. no coordinator: a queued request is cancelled by the editor itself");
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
        Ok("the editor cancelled it: %s", h);
        free(h);
    } else {
        char *s = StateOf(Y1 ? Y1 : "");
        Bad("state %s", s);
        free(s);
    }

    // 5b. no gio: the editor refuses to start a coordinator (its sessions delete only with `gio trash`), the
    // request stays queued and no terminal opens
    Echo("5b. no gio in PATH: the editor starts no coordinator, the request stays queued");
    const char *noGioOpened = strdup(nob_temp_sprintf("%s/opened-no-gio.txt", gT));
    SavedEnv noGioPath = SetEnvFor("PATH", nob_temp_sprintf("%s:%s/build/tools", NoGioFolder(), gToolRoot));
    SavedEnv noGioStub = SetEnvFor("MONOLITH_STUB_OPENED", noGioOpened);
    char *Z1 = Submit(A, "test no gio", NULL, 0);
    RestoreEnv(noGioStub);
    RestoreEnv(noGioPath);
    char *noGioLine = FindLine(nob_temp_sprintf("%s/submit-%d.out", gT, gN), Contains,
                               "monolith: coordinator not started: gio not found", false);
    if (noGioLine) Ok("the editor refused: %s", strstr(noGioLine, "monolith: "));
    else Bad("no \"coordinator not started: gio not found\" line (see submit-%d.out)", gN);
    free(noGioLine);
    struct stat stNoGio;
    if (stat(noGioOpened, &stNoGio) != 0 || stNoGio.st_size == 0) Ok("no terminal opened");
    else Bad("a terminal opened (see %s)", noGioOpened);
    if (StateIs(Z1 ? Z1 : "", "queued")) {
        Ok("%s stayed queued", Z1);
    } else {
        char *s = StateOf(Z1 ? Z1 : "");
        Bad("state %s (want queued)", s);
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

    // 7. the usage limit: the coordinator on main hits it, the editor restarts it on the fallback account
    Echo("7. usage limit: the coordinator restarts on the fallback account; after the reset the next start uses main");
    {
        // a requests folder of its own (no marker or coordinator of the sections before), the developer's
        // CLAUDE_CONFIG_DIR out of the way (main = the inherited environment)
        gR = strdup(nob_temp_sprintf("%s/requests-limit", gT));
        gLog = strdup(nob_temp_sprintf("%s/.coordinator/log", gR));
        ToolMkdirP(gR);
        const char *fbDir = strdup(nob_temp_sprintf("%s/fallback-config", gT));
        const char *out7 = strdup(nob_temp_sprintf("%s/coordinator-limit.out", gT));
        SavedEnv e1 = SetEnvFor("MONOLITH_REQUESTS_DIR", gR);
        SavedEnv e2 = SetEnvFor("MONOLITH_CLAUDE_FALLBACK", nob_temp_sprintf("CLAUDE_CONFIG_DIR=%s", fbDir));
        SavedEnv e3 = SetEnvFor("MONOLITH_STUB_LIMITED", "main");
        SavedEnv e4 = SetEnvFor("MONOLITH_STUB_COORD_LIMIT", "6");  // (both claimed by then)
        SavedEnv e5 = SetEnvFor("MONOLITH_LIMIT_RETRY", "14");
        SavedEnv e6 = SetEnvFor("MONOLITH_STUB_RUN", out7);
        SavedEnv e7 = SetEnvFor("MONOLITH_STUB_WORK", "12");  // (the limit comes in the middle of L1's work)
        char *oldConfig = getenv("CLAUDE_CONFIG_DIR") ? strdup(getenv("CLAUDE_CONFIG_DIR")) : NULL;
        unsetenv("CLAUDE_CONFIG_DIR");
        int opened0 = Opened();

        char *L1 = Submit(A, "[edit] test limit touched on map a", NULL, 0);
        char *L2 = Submit(B, "test limit b1 untouched", NULL, 0);  // (b1 < b2: ids of one second sort by slug)
        const char *waitLimit[] = {"--submit-wait", "30"};
        char *L3 = Submit(B, "test limit b2 queued", waitLimit, 2);
        const char *out3 = strdup(nob_temp_sprintf("%s/submit-%d.out", gT, gN));
        const char *s7[] = {L1, L2, L3};
        if (!WaitFor(60, AllFinal, s7, 3)) Bad("requests not finished after 60 s");
        Show(s7, 3);
        if (Opened() == opened0 + 2) Ok("two coordinators: main, then the fallback");
        else Bad("%d terminals opened in this section (want 2)", Opened() - opened0);
        if (FileHas(out7, Contains, "stub-coordinator: account=main config=- prompt-account-line=yes") &&
            FileHas(out7, Contains, nob_temp_sprintf("stub-coordinator: account=fallback-config config=%s prompt-account-line=yes", fbDir))) {
            Ok("the first ran on main, the second with CLAUDE_CONFIG_DIR=%s", fbDir + rootLen);
        } else {
            Bad("the coordinators' accounts (see %s)", out7);
        }
        if (FileHas(out7, Contains, "--settings {\"hooks\":{\"StopFailure\":[{\"matcher\":\"rate_limit|billing_error\"")) {
            Ok("the coordinator got the StopFailure hook (--settings)");
        } else {
            Bad("no --settings with the StopFailure hook (see %s)", out7);
        }
        const char *marker = nob_temp_sprintf("%s/.coordinator/limit", gR);
        if (FileHas(marker, Contains, " account=main reset=") && FileHas(marker, Contains, " error=rate_limit")) {
            char *h = Head1(marker);
            Ok("limit marker: %s", h);
            free(h);
        } else {
            Bad("no limit line for main in %s", marker);
        }
        if (FileHas(out3, Contains, "usage limit on main: the coordinator restarted on fallback-config")) {
            Ok("the editor restarted the coordinator (usage limit on main: ... fallback-config)");
        } else {
            Bad("the editor's log has no restart line (see %s)", out3);
        }
        {
            const char *result = nob_temp_sprintf("%s/%s/result.md", gR, L1 ? L1 : "");
            char *h = Head1(result);
            if (StateIs(L1 ? L1 : "", "failed") &&
                strcmp(h, "Interrupted: the account's usage limit was reached; restarted on fallback-config; files changed, check them") == 0 &&
                FileHas(result, Contains, "stub-edit.txt")) {
                Ok("touched %s was reported: %s", L1, h);
            } else {
                char *s = StateOf(L1 ? L1 : "");
                Bad("%s: %s: %s", L1 ? L1 : "", s, h);
                free(s);
            }
            free(h);
        }
        if (FileHas(gLog, Contains, nob_temp_sprintf("requeued %s", L2 ? L2 : "")) && StateIs(L2 ? L2 : "", "done")) {
            Ok("untouched %s was requeued and ran on the fallback", L2);
        } else {
            Bad("%s was not requeued and done", L2 ? L2 : "");
        }
        if (StateIs(L3 ? L3 : "", "done")) Ok("queued %s ran on the fallback", L3);
        else Bad("%s not done", L3 ? L3 : "");

        // back to main: the fallback coordinator stops (here: killed), main's reset time passes, the next
        // start uses main (MONOLITH_STUB_LIMITED empty: main answers again)
        char *pg7 = LastGroup();
        KillGroup(pg7);
        free(pg7);
        if (!WaitFor(10, LockFree, NULL, 0)) Bad("the lock is still held");
        {
            char *h = Head1(marker);
            struct tm tm = {0};
            time_t at = strptime(h, "%Y-%m-%dT%H:%M:%S", &tm) ? (tm.tm_isdst = -1, mktime(&tm)) : time(NULL);
            free(h);
            double left = (double)(at + 15 - time(NULL));
            if (left > 0) {
                Echo("  (waiting %.0f s for main's reset time, MONOLITH_LIMIT_RETRY=14)", left);
                Sleep(left);
            }
        }
        RestoreEnv(e3);
        SavedEnv e3b = SetEnvFor("MONOLITH_STUB_LIMITED", "");
        char *L4 = Submit(B, "test limit back on main", NULL, 0);
        const char *s7b[] = {L4};
        if (!WaitFor(30, AllFinal, s7b, 1)) Bad("not finished after 30 s");
        char *last = FindLine(out7, Contains, "stub-coordinator: account=", true);
        if (StateIs(L4 ? L4 : "", "done") && last && strstr(last, "account=main ")) Ok("after the reset the next coordinator ran on main");
        else Bad("after the reset: %s", last ? last : "(no start line)");
        free(last);
        pg7 = LastGroup();
        KillGroup(pg7);
        free(pg7);
        if (!WaitFor(10, LockFree, NULL, 0)) Bad("the lock is still held");

        // the hook's reset time: "resets 3:45pm" is read by request-queue limit --hook
        {
            const char *rq = nob_temp_sprintf("%s/requests-hook", gT);
            ToolMkdirP(rq);
            const char *in = nob_temp_sprintf("%s/hook.json", gT);
            FILE *f = fopen(in, "wb");
            if (f) {
                fputs("{\"hook_event_name\":\"StopFailure\",\"error\":\"rate_limit\",\"last_assistant_message\":\"You've hit your session limit \\u00b7 resets 3:45pm\"}\n", f);
                if (fclose(f) != 0) ToolErr("%s: %s", in, strerror(errno));
            }
            SavedEnv h1 = SetEnvFor("MONOLITH_REQUESTS_DIR", rq);
            SavedEnv h2 = SetEnvFor("MONOLITH_ACCOUNT", "main");
            Nob_Cmd cmd = {0};
            nob_cmd_append(&cmd, ToolProgram("request-queue"), "limit", "--hook");
            ToolRun(&cmd, (ToolIo){.in = in, .out = "/dev/null"});
            nob_cmd_free(cmd);
            RestoreEnv(h2);
            RestoreEnv(h1);
            char *h = Head1(nob_temp_sprintf("%s/.coordinator/limit", rq));
            if (strstr(h, " account=main reset=") && strstr(h, "T15:45:00 error=rate_limit")) Ok("hook text \"resets 3:45pm\": %s", h);
            else Bad("hook marker line: %s", h);
            free(h);
        }

        // every account limited: main -> the fallback once, then it stays (no back-and-forth)
        {
            gR = strdup(nob_temp_sprintf("%s/requests-limit-all", gT));
            gLog = strdup(nob_temp_sprintf("%s/.coordinator/log", gR));
            ToolMkdirP(gR);
            SavedEnv a = SetEnvFor("MONOLITH_REQUESTS_DIR", gR);
            SavedEnv b = SetEnvFor("MONOLITH_STUB_LIMITED", "main,fallback-config");
            SavedEnv c = SetEnvFor("MONOLITH_STUB_COORD_LIMIT", "2");
            int opened1 = Opened();
            const char *waitAll[] = {"--submit-wait", "16"};
            char *Z = Submit(A, "test every account limited", waitAll, 2);
            const char *outZ = nob_temp_sprintf("%s/submit-%d.out", gT, gN);
            if (Opened() == opened1 + 2 && FileHas(outZ, Contains, "every other account is limited too: the coordinator stays")) {
                Ok("every account limited: one switch to the fallback, then the coordinator stays");
            } else {
                Bad("every account limited: %d terminals (want 2; see %s)", Opened() - opened1, outZ);
            }
            char *pgZ = LastGroup();
            KillGroup(pgZ);
            free(pgZ);
            if (!WaitFor(10, LockFree, NULL, 0)) Bad("the lock is still held");
            free(Z);
            RestoreEnv(c);
            RestoreEnv(b);
            RestoreEnv(a);
        }

        RestoreEnv(e3b);
        RestoreEnv(e7);
        RestoreEnv(e6);
        RestoreEnv(e5);
        RestoreEnv(e4);
        RestoreEnv(e2);
        RestoreEnv(e1);
        if (oldConfig) setenv("CLAUDE_CONFIG_DIR", oldConfig, 1);
        free(oldConfig);
    }

    // 8. the classifier: a usage limit on main runs it again on the fallback; other failures don't
    Echo("8. the classifier on a usage limit: once more on the fallback account; every account limited: default tier");
    {
        const char *stubClassifier = strdup(ToolProgram("stub-classifier"));
        const char *fbDir = strdup(nob_temp_sprintf("%s/fallback-config", gT));
        char *oldConfig = getenv("CLAUDE_CONFIG_DIR") ? strdup(getenv("CLAUDE_CONFIG_DIR")) : NULL;
        unsetenv("CLAUDE_CONFIG_DIR");
        SavedEnv c1 = SetEnvFor("MONOLITH_CLASSIFIER", stubClassifier);
        // one case: its own requests folder (the marker) and classifier log; returns the --classify output path
        // fallback "set": one fallback account (fallback-config); "two": two (fallback-config;fallback-two)
        struct {
            const char *name, *fallback, *mode, *limited;
        } cases[] = {
            {"limit-main", "set", "heavy", "main"},
            {"limit-all", "set", "heavy", "main,fallback-config"},
            {"other-error", "set", "fail", ""},
            {"no-fallback", "", "limit", ""},
            {"two-fallbacks", "two", "heavy", "main,fallback-config"},
            {"two-all", "two", "heavy", "main,fallback-config,fallback-two"},
        };
        enum { CASES = sizeof cases / sizeof cases[0] };
        const char *fbTwo = strdup(nob_temp_sprintf("%s/fallback-two", gT));
        const char *outs[CASES], *logs[CASES], *dirs[CASES];
        for (int k = 0; k < CASES; k++) {
            dirs[k] = strdup(nob_temp_sprintf("%s/requests-classify-%s", gT, cases[k].name));
            logs[k] = strdup(nob_temp_sprintf("%s/classifier-%s.log", gT, cases[k].name));
            outs[k] = strdup(nob_temp_sprintf("%s/classify-%s.out", gT, cases[k].name));
            ToolMkdirP(dirs[k]);
            SavedEnv a = SetEnvFor("MONOLITH_REQUESTS_DIR", dirs[k]);
            const char *fb = !*cases[k].fallback ? ""
                             : strcmp(cases[k].fallback, "two") == 0 ? nob_temp_sprintf("CLAUDE_CONFIG_DIR=%s; CLAUDE_CONFIG_DIR=%s", fbDir, fbTwo)
                                                                     : nob_temp_sprintf("CLAUDE_CONFIG_DIR=%s", fbDir);
            SavedEnv b = SetEnvFor("MONOLITH_CLAUDE_FALLBACK", fb);
            SavedEnv c = SetEnvFor("MONOLITH_STUB_CLASSIFIER", cases[k].mode);
            SavedEnv d = SetEnvFor("MONOLITH_STUB_LIMITED", cases[k].limited);
            SavedEnv e = SetEnvFor("MONOLITH_STUB_CLASSIFIER_LOG", logs[k]);
            Nob_Cmd cmd = {0};
            nob_cmd_append(&cmd, gGame, "--map", A, "--classify", "test classifier limit");
            ToolRun(&cmd, (ToolIo){.out = outs[k], .errToOut = true});
            nob_cmd_free(cmd);
            RestoreEnv(e);
            RestoreEnv(d);
            RestoreEnv(c);
            RestoreEnv(b);
            RestoreEnv(a);
        }
        // (a) main limited: the fallback answers
        if (FileHas(outs[0], Exactly, "tier: heavy") && FileHas(outs[0], Exactly, "classifier: ai") &&
            FileHas(outs[0], Exactly, "classifier account: fallback-config") &&
            FileHas(logs[0], Contains, "stub-classifier account: main config: -") &&
            FileHas(logs[0], Contains, nob_temp_sprintf("stub-classifier account: fallback-config config: %s", fbDir)) &&
            FileHas(nob_temp_sprintf("%s/.coordinator/limit", dirs[0]), Contains, " account=main reset=")) {
            Ok("limit on main: the fallback answered (heavy, classifier account fallback-config; main marked limited)");
        } else {
            Bad("limit on main (see %s, %s)", outs[0], logs[0]);
        }
        // (b) every account limited: default tier, the reason says so
        if (FileHas(outs[1], Exactly, "tier: default") && FileHas(outs[1], Exactly, "classifier: failed") &&
            FileHas(outs[1], Exactly, "reason: AI classifier failed (the usage limit was reached on every account)")) {
            Ok("limit on every account: default tier, \"the usage limit was reached on every account\"");
        } else {
            Bad("limit on every account (see %s)", outs[1]);
        }
        // (c) another error: no second run, no marker
        int runs = 0;
        {
            char *text = Slurp(logs[2]);
            for (char *p = strstr(text, "stub-classifier argv:"); p; p = strstr(p + 1, "stub-classifier argv:")) runs++;
            free(text);
        }
        struct stat stc;
        if (runs == 1 && FileHas(outs[2], Exactly, "classifier: failed") &&
            stat(nob_temp_sprintf("%s/.coordinator/limit", dirs[2]), &stc) != 0) {
            Ok("another failure: one run, no retry, no marker");
        } else {
            Bad("another failure: %d runs (see %s)", runs, outs[2]);
        }
        // (d) no fallback configured: the limit is an ordinary failure, no account fields
        if (FileHas(outs[3], Exactly, "reason: AI classifier failed (the account's usage limit was reached)") &&
            !FileHas(outs[3], StartsWith, "classifier account:") && !FileHas(logs[3], Contains, "account: main")) {
            Ok("no fallback: the limit fails as before (default tier), no MONOLITH_ACCOUNT");
        } else {
            Bad("no fallback (see %s, %s)", outs[3], logs[3]);
        }
        // (f) two fallbacks, tried in order: main and the first limited, the second answers; all three
        // limited: default tier
        if (FileHas(outs[4], Exactly, "tier: heavy") && FileHas(outs[4], Exactly, "classifier account: fallback-two") &&
            FileHas(logs[4], Contains, "stub-classifier account: main ") &&
            FileHas(logs[4], Contains, "stub-classifier account: fallback-config ") &&
            FileHas(logs[4], Contains, nob_temp_sprintf("stub-classifier account: fallback-two config: %s", fbTwo))) {
            Ok("two fallbacks: main, then fallback-config, then fallback-two answered");
        } else {
            Bad("two fallbacks (see %s, %s)", outs[4], logs[4]);
        }
        if (FileHas(outs[5], Exactly, "reason: AI classifier failed (the usage limit was reached on every account)")) {
            Ok("two fallbacks, all three limited: default tier");
        } else {
            Bad("two fallbacks, all limited (see %s)", outs[5]);
        }
        // (e) a submit (the classifier polled by the frame loop): the bundle records the account, and the
        // coordinator starts on the fallback (the classifier marked main limited)
        {
            gR = strdup(nob_temp_sprintf("%s/requests-classify-submit", gT));
            ToolMkdirP(gR);
            const char *outE = strdup(nob_temp_sprintf("%s/coordinator-classify.out", gT));
            SavedEnv a = SetEnvFor("MONOLITH_REQUESTS_DIR", gR);
            SavedEnv b = SetEnvFor("MONOLITH_CLAUDE_FALLBACK", nob_temp_sprintf("CLAUDE_CONFIG_DIR=%s", fbDir));
            SavedEnv c = SetEnvFor("MONOLITH_STUB_CLASSIFIER", "fast");
            SavedEnv d = SetEnvFor("MONOLITH_STUB_LIMITED", "main");
            SavedEnv e = SetEnvFor("MONOLITH_STUB_CLASSIFIER_LOG", nob_temp_sprintf("%s/classifier-submit.log", gT));
            SavedEnv f = SetEnvFor("MONOLITH_STUB_RUN", outE);
            const char *waitE[] = {"--submit-wait", "3"};
            char *E1 = Submit(A, "test classifier limit submit", waitE, 2);
            const char *json = nob_temp_sprintf("%s/%s/request.json", gR, E1 ? E1 : "");
            const char *md = nob_temp_sprintf("%s/%s/request.md", gR, E1 ? E1 : "");
            if (FileHas(json, Contains, "\"classifierAccount\": \"fallback-config\"") && FileHas(json, Contains, "\"format\": 7") &&
                FileHas(md, Contains, " on fallback-config in ") && FileHas(md, Contains, "(fast tier, chosen by the AI classifier")) {
                Ok("submit: the bundle says the fallback classified it (request.json classifierAccount, request.md Model line)");
            } else {
                Bad("submit: the bundle's model fields (see %s)", json);
            }
            if (FileHas(outE, Contains, "stub-coordinator: account=fallback-config ")) Ok("its coordinator started on the fallback");
            else Bad("its coordinator's account (see %s)", outE);
            const char *sE[] = {E1};
            WaitFor(20, AllFinal, sE, 1);
            char *pgE = LastGroup();
            KillGroup(pgE);
            free(pgE);
            if (!WaitFor(10, LockFree, NULL, 0)) Bad("the lock is still held");
            free(E1);
            RestoreEnv(f);
            RestoreEnv(e);
            RestoreEnv(d);
            RestoreEnv(c);
            RestoreEnv(b);
            RestoreEnv(a);
            free((char *)outE);
        }
        free((char *)fbTwo);
        for (int k = 0; k < CASES; k++) {
            free((char *)outs[k]);
            free((char *)logs[k]);
            free((char *)dirs[k]);
        }
        RestoreEnv(c1);
        if (oldConfig) setenv("CLAUDE_CONFIG_DIR", oldConfig, 1);
        free(oldConfig);
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
