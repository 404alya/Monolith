// tools/stub-coordinator.c: a fake request coordinator for tests: no Claude, no window. Built by
// `./build.sh --tools stub-coordinator` into build/tools/stub-coordinator; it replaced
// tools/stub-coordinator.sh (2026-11-13) and behaves exactly like it.
//
// The editor starts it in place of claude (MONOLITH_CLAUDE=build/tools/stub-coordinator, with
// MONOLITH_TERMINAL=build/tools/stub-terminal and MONOLITH_STUB_RUN=<log>, see build/tools/test-coordinator).
// It ignores its arguments and applies the scheduling rules of the coordinator skill
// (.claude/skills/coordinator/SKILL.md, §4) through build/tools/request-queue, so the tests exercise the real
// queue:
//   - a queued request starts only if no claimed request uses the same map package, and an older queued one
//     on that package goes first;
//   - a code request runs alone: it starts only when nothing else runs, and while it waits or runs nothing
//     newer starts;
//   - at most 3 at once.
// Keep these rules in step with the skill (the large-engine-work skill, §5): when the skill's rules change,
// this program changes with them.
//
// A "subagent" is a child process that sleeps $MONOLITH_STUB_WORK seconds and writes result.md. Markers in
// the prompt: "[code]" = a code request; "[edit]" = the worker appends to <package>/stub-edit.txt right
// after the claim (for the recovery test). A cancel file stops the worker and finishes the bundle as
// cancelled.
//
// Output (stdout, which the stub terminal appends to its MONOLITH_STUB_RUN file), one line per event with the
// time of day in ms:
//   HH:MM:SS.mmm stub-coordinator: started (pid <pid>), args: <count>
//   (request-queue start's own output)
//   HH:MM:SS.mmm start <id> <package>[ [code]]
//   HH:MM:SS.mmm end <id>
//   HH:MM:SS.mmm cancelled <id>[ (queued)]
//   HH:MM:SS.mmm wait <id> (code request: waits until it is alone)
// request-queue's error messages (stderr) land there too.
//
// Environment:
//   MONOLITH_STUB_WORK      seconds a worker "works" (default 3; fractions allowed, like sleep's)
//   MONOLITH_REQUESTS_DIR   the requests folder (also read by request-queue); default <repo>/requests
//
// It runs until it is killed (the tests send SIGTERM to its process group, which ends its workers too).
// Exit status: 1 when build/tools/request-queue is not built (with the message saying how to build it).

#include "tool.h"

// The most subagents at once (one GPU renders every --shot).
#define MAX_RUNNING 3

// The request-queue program (absolute path), set in main().
static const char *gQueue;

// T: `date '+%H:%M:%S.%3N'`, the time of day with milliseconds (cut, not rounded), into buf[32].
static const char *T(char buf[32])
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char hms[16];
    strftime(hms, sizeof hms, "%H:%M:%S", &tm);
    snprintf(buf, 32, "%s.%03d", hms, (int)(ts.tv_nsec / 1000000L) % 1000);
    return buf;
}

// Runs request-queue with the given words; `quiet` sends its stdout to /dev/null (the script's
// `"$Q" ... > /dev/null`; its stderr stays ours). Returns its exit status.
static int Queue(bool quiet, const char *a, const char *b, const char *c, const char *d)
{
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, gQueue, a);
    if (b) nob_cmd_append(&cmd, b);
    if (c) nob_cmd_append(&cmd, c);
    if (d) nob_cmd_append(&cmd, d);
    int status = ToolRun(&cmd, (ToolIo){.out = quiet ? "/dev/null" : NULL});
    nob_cmd_free(cmd);
    return status;
}

// One worker: the bundle id and the pid of the child doing its "work" (0 = none).
typedef struct {
    char *id;
    pid_t pid;
} Worker;

// The workers started and not yet finished or cancelled (the script's `worker` array), and every child
// pid not yet reaped (a bash background job is reaped by bash; here the loop reaps them, so none stays a
// zombie, and a reaped pid is never signalled).
static struct {
    Worker *items;
    size_t count, capacity;
} gWorkers;
static struct {
    pid_t *items;
    size_t count, capacity;
} gChildren;

// The worker of a bundle, or NULL.
static Worker *FindWorker(const char *id)
{
    for (size_t i = 0; i < gWorkers.count; i++) {
        if (strcmp(gWorkers.items[i].id, id) == 0) return &gWorkers.items[i];
    }
    return NULL;
}

// `unset "worker[$id]"`: forgets the bundle's worker (the child itself is reaped by ReapChildren).
static void ForgetWorker(const char *id)
{
    for (size_t i = 0; i < gWorkers.count; i++) {
        if (strcmp(gWorkers.items[i].id, id) == 0) {
            free(gWorkers.items[i].id);
            gWorkers.items[i] = gWorkers.items[--gWorkers.count];
            return;
        }
    }
}

// Reaps every child that has exited, without waiting.
static void ReapChildren(void)
{
    for (size_t i = 0; i < gChildren.count;) {
        if (waitpid(gChildren.items[i], NULL, WNOHANG) == gChildren.items[i]) {
            gChildren.items[i] = gChildren.items[--gChildren.count];
        } else {
            i++;
        }
    }
}

// Whether the pid is a child not yet reaped (so signalling it reaches our worker, not a reused pid).
static bool Alive(pid_t pid)
{
    for (size_t i = 0; i < gChildren.count; i++) {
        if (gChildren.items[i] == pid) return true;
    }
    return false;
}

// One bundle of the listing: the id and the fields the rules need, the rest of its row (" state=... ...", with
// a leading space, as the script's `rest=" $rest"`), and its prompt line's text.
typedef struct {
    char *id;
    char *rest;
    char *prompt;
} Row;

// The value of " <key>=" in rest: sed -nE 's/.* key=([^ ]*).*/\1/p', i.e. after the last " key=", up to the
// next space. Heap string ("" when the key is missing).
static char *Field(const char *rest, const char *key)
{
    const char *want = nob_temp_sprintf(" %s=", key);
    const char *at = NULL;
    for (const char *p = strstr(rest, want); p; p = strstr(p + 1, want)) at = p;
    if (!at) return strdup("");
    at += strlen(want);
    return strndup(at, strcspn(at, " "));
}

// Reads `request-queue list` into rows: a line starting with "    prompt " gives the prompt of the row
// before it (the text after its first ": "), a line with " state=" is a row (`read -r id rest`: the first
// word, then the rest with outer blanks removed); "(N open)" is skipped.
static void ReadList(Row **rows, size_t *count, char ***prompts, size_t *promptCount)
{
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, gQueue, "list");
    Nob_String_Builder out = {0};
    ToolCapture(&cmd, &out, (ToolIo){0});
    nob_cmd_free(cmd);
    *rows = NULL;
    *count = 0;
    *prompts = NULL;
    *promptCount = 0;
    for (size_t i = 0; i < out.count;) {
        size_t end = i;
        while (end < out.count && out.items[end] != '\n') end++;
        char *line = strndup(out.items + i, end - i);
        i = end + 1;
        if (strncmp(line, "    prompt ", 11) == 0) {
            const char *colon = strstr(line, ": ");
            *prompts = realloc(*prompts, (*promptCount + 1) * sizeof **prompts);
            (*prompts)[(*promptCount)++] = strdup(colon ? colon + 2 : line);
        } else if (strstr(line, " state=")) {
            // read -r id rest: blanks around the words go
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            size_t idLen = strcspn(p, " \t");
            char *rest = p + idLen;
            while (*rest == ' ' || *rest == '\t') rest++;
            size_t restLen = strlen(rest);
            while (restLen > 0 && (rest[restLen - 1] == ' ' || rest[restLen - 1] == '\t')) restLen--;
            *rows = realloc(*rows, (*count + 1) * sizeof **rows);
            (*rows)[*count] = (Row){
                .id = strndup(p, idLen),
                .rest = strdup(nob_temp_sprintf(" %.*s", (int)restLen, rest)),
            };
            (*count)++;
        }
        free(line);
    }
    nob_sb_free(out);
}

// The set of busy packages for one pass (the script's `busy` array).
typedef struct {
    char **items;
    size_t count, capacity;
} Names;

// Whether name is in the set.
static bool Has(Names *set, const char *name)
{
    for (size_t i = 0; i < set->count; i++) {
        if (strcmp(set->items[i], name) == 0) return true;
    }
    return false;
}

// Starts the "subagent" of a claimed bundle: a child that (with [edit]) appends a line to the package's
// stub-edit.txt, sleeps MONOLITH_STUB_WORK seconds and writes the bundle's result.md, then exits. Returns its
// pid.
static pid_t StartWorker(const char *id, const char *pkg, bool edit, double work)
{
    // abs: the package as an absolute path (relative ones are relative to the repo root)
    char *abs = pkg[0] == '/' ? strdup(pkg) : strdup(nob_temp_sprintf("%s/%s", gToolRoot, pkg));
    const char *requests = ToolEnv("MONOLITH_REQUESTS_DIR", nob_temp_sprintf("%s/requests", gToolRoot));
    char *result = strdup(nob_temp_sprintf("%s/%s/result.md", requests, id));
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        // the child: no stdio buffers of the parent are flushed again (_exit), nothing else is touched
        if (edit) {
            FILE *f = fopen(nob_temp_sprintf("%s/stub-edit.txt", abs), "ab");
            if (f) {
                fprintf(f, "stub edit for %s\n", id);
                fclose(f);
            }
        }
        struct timespec ts = {(time_t)work, (long)((work - (double)(time_t)work) * 1e9)};
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
        FILE *f = fopen(result, "wb");
        if (f) {
            fprintf(f, "Stub coordinator: %s done\n\nNothing was changed (test double).\n", id);
            fclose(f);
        }
        _exit(0);
    }
    free(abs);
    free(result);
    return pid;
}

int main(int argc, char **argv)
{
    ToolInit(argv, "stub-coordinator", "stub-coordinator");
    gQueue = strdup(ToolProgram("request-queue"));
    // its output goes to a file (the stub terminal's MONOLITH_STUB_RUN) and it ends by being killed: every
    // line must be in the file as soon as it is printed, as the script's echo put it there
    setvbuf(stdout, NULL, _IOLBF, 0);
    char *workText = strdup(ToolEnv("MONOLITH_STUB_WORK", "3"));
    double work = strtod(workText, NULL);
    char t[32];

    printf("%s stub-coordinator: started (pid %ld), args: %d\n", T(t), (long)getpid(), argc - 1);
    Queue(false, "start", NULL, NULL, NULL);

    for (;;) {
        ReapChildren();
        Row *rows;
        size_t count;
        char **prompts;
        size_t promptCount;
        ReadList(&rows, &count, &prompts, &promptCount);
        Names busy = {0};
        int running = 0;
        bool code = false;

        // The running ones: finished (result.md), cancelled, or still busy
        for (size_t i = 0; i < count; i++) {
            char *state = Field(rows[i].rest, "state");
            char *pkg = Field(rows[i].rest, "package");
            const char *id = rows[i].id;
            const char *prompt = i < promptCount ? prompts[i] : "";
            if (strcmp(state, "claimed") == 0 || strcmp(state, "question") == 0) {
                if (strstr(rows[i].rest, " result=yes")) {
                    if (Queue(true, "finish", id, "done", NULL) == 0) printf("%s end %s\n", T(t), id);
                    ForgetWorker(id);
                } else if (strstr(rows[i].rest, " cancel=yes")) {
                    Worker *w = FindWorker(id);
                    if (w && w->pid > 0 && Alive(w->pid)) kill(w->pid, SIGTERM);
                    ForgetWorker(id);
                    if (Queue(true, "finish", id, "cancelled", "Cancelled by the developer while running") == 0) {
                        printf("%s cancelled %s\n", T(t), id);
                    }
                } else {
                    running++;
                    nob_da_append(&busy, strdup(pkg));
                    if (strstr(prompt, "[code]")) code = true;
                }
            }
            free(state);
            free(pkg);
        }

        // The queued ones, oldest first
        for (size_t i = 0; i < count; i++) {
            char *state = Field(rows[i].rest, "state");
            char *pkg = Field(rows[i].rest, "package");
            const char *id = rows[i].id;
            const char *prompt = i < promptCount ? prompts[i] : "";
            bool stop = false;
            if (strcmp(state, "queued") == 0) {
                bool isCode = strstr(prompt, "[code]") != NULL;
                if (strstr(rows[i].rest, " cancel=yes")) {
                    if (Queue(true, "finish", id, "cancelled", "Cancelled by the developer before it started") == 0) {
                        printf("%s cancelled %s (queued)\n", T(t), id);
                    }
                } else if (code) {
                    stop = true;  // a code request runs or waits: nothing newer starts
                } else if (isCode && running > 0) {
                    code = true;
                    printf("%s wait %s (code request: waits until it is alone)\n", T(t), id);
                    stop = true;
                } else if (Has(&busy, pkg)) {
                    // same package is claimed (or an older one waits): the next one
                } else if (running >= MAX_RUNNING) {
                    stop = true;
                } else if (Queue(true, "claim", id, NULL, NULL) == 0) {
                    printf("%s start %s %s%s\n", T(t), id, pkg, isCode ? " [code]" : "");
                    pid_t pid = StartWorker(id, pkg, strstr(prompt, "[edit]") != NULL, work);
                    if (pid > 0) nob_da_append(&gChildren, pid);
                    ForgetWorker(id);
                    nob_da_append(&gWorkers, ((Worker){strdup(id), pid}));
                    running++;
                    nob_da_append(&busy, strdup(pkg));
                    if (isCode) code = true;
                }
            }
            free(state);
            free(pkg);
            if (stop) break;
        }

        // this pass's lists go
        for (size_t i = 0; i < busy.count; i++) free(busy.items[i]);
        nob_da_free(busy);
        for (size_t i = 0; i < count; i++) {
            free(rows[i].id);
            free(rows[i].rest);
        }
        free(rows);
        for (size_t i = 0; i < promptCount; i++) free(prompts[i]);
        free(prompts);
        nob_temp_reset();

        // Wait for the next event, or a second (workers finish without an event)
        Queue(true, "wait", "--timeout", "1", NULL);
    }
}
