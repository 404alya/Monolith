// tools/run-sh.c -> build/run-sh.o: the shell every Claude session's commands run through. It behaves like
// `bash -c` (same stdin, stdout, stderr, exit status, signals and background jobs) but first looks at the
// command line and refuses the ordinary mistakes that break the guard rails (CLAUDE.md), with a message that
// says why and what to use instead. A guard against mistakes, not a sandbox: the checks are simple on
// purpose (the developer's call; "What it doesn't see" below). Written on nob.h with tools/tool.h.
//
// Modes:
//   run-sh.o -c '<command line>' [arg0 [args...]]
//       checks the line; when it passes, execs `bash -c '<command line>' [arg0 [args...]]` (always bash; a
//       `cd` in it doesn't carry over to the next command). A refused line prints
//           run-sh: refused: <rule>: <the word that broke it>; <what to do instead>
//       on stderr and exits 126 without running anything. A benchmark takes the benchmark lock first.
//   run-sh.o --check '<command line>'
//       prints the decision ("run-sh: allowed" or the refusal line) and exits 0 or 126; runs nothing, no lock.
//   run-sh.o --hook [--rewrite unprompted|all]
//       the PreToolUse hook: the tool call's JSON on stdin, the answer on stdout ("The hook" below).
//   run-sh.o --help
// Every decision (all three modes) is appended to a log, one line each: <local time> TAB <mode> TAB
// <decision> TAB <working directory> TAB <command, with \ \t \n escaped>.
//
// Environment: RUN_SH_DIR, the folder of the log and the benchmark lock (default <repo>/build/run-sh;
// <dir>/log, <dir>/bench.lock; the unit test points it at its own folder). HOME for `~` and a bare `cd`.
// __GL_SYNC_TO_VBLANK / vblank_mode: a benchmark needs one of them 0, in front of the command (also after
// env), exported earlier in the line, or in run-sh.o's own environment.
//
// Exit codes: -c: the command's own status, 126 refused, 127 bash can't start; --check: 0 allowed, 126
// refused; --hook: 0 with the answer, 2 when its input isn't the expected JSON (the call is then blocked);
// 2 for a usage error.
//
// Callers: the PreToolUse hook in .claude/settings.json (`run-sh.o --hook` for every Bash and Monitor
// call). Built by `./build.sh --tools run-sh`, then it rebuilds itself when this file, tools/tool.h or nob.h
// is newer (ToolInit). tests/unit/run_sh_test.c (`./build.sh test run_sh`) checks every rule.
//
// How it reads a command (regions "Splitting" and "Checking a segment"):
//   1. The line is cut into segments at ; & | && || newlines ( ) $( and backticks, but not inside '…' or
//      "…". Each segment's words are split at blanks, quotes removed; a backslash keeps the next character;
//      `# …` comments are dropped. `>` / `>>` (also `>file`, `2>`, `&>`) take the next word as a file the
//      segment writes; `<`, `<<<` and `2>&1` are dropped; a here-document's body is skipped unread.
//   2. The command word is the first word after a keyword in front (if then do else elif while until ! {),
//      after any NAME=value words and after one wrapper with its options: sudo, env (and its NAME=value
//      words), nice, nohup, timeout (and its duration), time, command, exec, xargs. Names compare by basename.
//   3. `cd <dir>` sets the working directory of the segments after it (`cd` alone: $HOME; `cd -` or a dir
//      with $ etc.: unknown). The end of a ( … ) or $( … ) restores the directory it started in.
//
// The rules (each enforces a CLAUDE.md line, named in the refusal):
//   no deletion outside /tmp   ("Never delete with rm ... gio trash"; "temporary files you created under /tmp")
//       rm / rmdir / unlink / shred with an operand that doesn't resolve under /tmp/ (never /tmp itself) or
//       can't be checked (a word with $ ` * ? [ {, a relative path in an unknown directory); xargs feeding
//       one of them; find -delete / -exec rm… with a root outside /tmp/; git clean without -n / --dry-run;
//       git rm without --cached; git worktree remove outside /tmp/. gio trash is always allowed.
//   no lost work               ("Never delete"; the work in the tree is the developer's)
//       git reset --hard; git checkout with -- . -f --force; git restore without --staged; git stash drop|clear.
//   no visible window          ("Never open a visible window")
//       the game (monolith, monolith.exe, also as wine's program; build.sh [--win] run; nob run) without
//       --shot, --dump-boxes, --dump-scene, --dump-scene-diff, --dump-entities, --apply-edits,
//       --inspect-model, --new-map, --export, --compile, --classify, --bake-surface or --bench --headless
//       (old-monolith builds lack the game's own guard); the plain examples (examples/build/linux/, win/;
//       their test builds linux-test/, linux-lsan/ are fine); a word starting MONOLITH_ALLOW_WINDOW=, or
//       export / declare / typeset / readonly / local MONOLITH_ALLOW_WINDOW….
//   never touch the screen     ("Never switch the developer's workspace, desktop or window focus")
//       i3-msg / swaymsg other than `-t get_*` without a payload; xdotool; wmctrl.
//   benchmarks                 ("only --bench --headless, one at a time ... __GL_SYNC_TO_VBLANK=0 and --note")
//       the game's --bench without --headless, vsync off (Environment) or --note "<text>"; an example test
//       build's --bench without vsync off; a second benchmark while one runs: -c takes an exclusive flock on
//       <RUN_SH_DIR>/bench.lock through an fd without O_CLOEXEC before it execs bash, so bash and every child
//       hold it until the run ends (also bench-export-ab). --check doesn't look at the lock.
//   stay in the repo           ("Work only inside the repo, plus your own scratch and download folders")
//       an absolute path (or ~) outside the repo and /tmp as a > / >> target, the last operand of cp / mv,
//       or an operand of tee / mkdir / touch. /dev/null, /dev/std*, /dev/tty, /dev/fd/N are fine.
//
// "Under /tmp" (Resolve): the path made absolute against the working directory, then walked: each existing
// component replaced by its realpath (links followed, `..` physical), the rest lexical. The last component
// isn't followed (rm removes a link, not its target) unless the path ends in '/', or for find roots and git
// worktree remove. The result must start with "/tmp/".
//
// What it doesn't see (these pass unchecked):
//   - commands inside nested `bash -c '…'` / `eval` strings, interpreters (python3 -c, awk), scripts and
//     compiled programs; a command word that only exists after expansion ($X, $(…));
//   - wrappers other than the listed ones, or a second one (sudo env rm);
//   - here-documents and here-strings (`bash <<EOF`: the body is skipped), text inside quotes ("$(rm x)");
//   - destroying a file's content: `> file`, truncate, `cp /dev/null file`, sed -i;
//   - two-step deletions: `mv <repo file> /tmp/x` then `rm /tmp/x`, or a link made and deleted through;
//   - a cd that fails, or runs in a pipeline, after || or in the background (it still counts);
//   - windows of other programs (xdg-open, image viewers), or the game copied and run under another name.
//
// The hook (--hook): reads {"tool_name", "tool_input", "cwd", "permission_mode", ...}.
//   - Bash or Monitor with a "command": checked with the JSON's "cwd". Refused: {"hookSpecificOutput":
//     {"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"run-sh: refused:
//     ..."}}, in every mode. Allowed: "updatedInput" is tool_input with "command" replaced by
//     `<repo>/build/run-sh.o -c '<command>'` (' as '\''), every other field copied as written; no
//     permissionDecision, so the normal permission flow continues on the rewritten command (Claude Code
//     2.1.323 applies a bare updatedInput in every mode, also for Monitor, and matches allow rules against
//     the rewritten command). The rewrite happens only in the modes that never ask (bypassPermissions,
//     dontAsk, auto, or no / an unknown mode); in default, acceptEdits and plan an allowed command passes
//     unchanged (empty answer: a rewritten one would miss the allow rules and every command would ask),
//     unless `--rewrite all`. A command that already is exactly `<this run-sh.o> -c '…'` passes unchanged.
//   - Monitor with "ws" and no command, and every other tool: passed (empty answer).
//   - Input that isn't the expected JSON: exit 2 with the reason on stderr (blocks the call).
//   <repo> is this program's repo: the hook runs $CLAUDE_PROJECT_DIR/build/run-sh.o, the main checkout's
//   even in a worktree agent.
//
// Other limits: in the modes that only check, allowed commands run in the session's own shell (no lock, a
// cd carries over, logged once as --hook). When tools/run-sh.c changed but doesn't compile, every call tries
// again (a second or two) and then runs the old build with a warning. The log grows without limit.

#include "tool.h"

#include <ctype.h>
#include <stddef.h>
#include <sys/file.h>

// #region Memory: one arena for everything

// Every string and array this program builds lives in one arena: a list of heap chunks freed together
// (FreeAll) before the program exits or execs bash. The checks build many small strings whose lifetimes
// all end with the program, so one arena keeps "zero leaks" (the leak check) without freeing each.
// Chunks are 64 KiB, or larger for a single big request: no limit on a command's length.
typedef struct Chunk {
    struct Chunk *next;
    size_t used, size;
    max_align_t data[];
} Chunk;

static Chunk *gChunks;

// n zeroed bytes from the arena (16-byte aligned); exits 126 when out of memory.
static void *Alloc(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (!gChunks || gChunks->size - gChunks->used < n) {
        size_t size = n > 65536 ? n : 65536;
        Chunk *c = malloc(sizeof *c + size);
        if (!c) {
            fputs("run-sh: out of memory\n", stderr);
            exit(126);
        }
        c->next = gChunks;
        c->used = 0;
        c->size = size;
        gChunks = c;
    }
    void *p = (unsigned char *)gChunks->data + gChunks->used;
    gChunks->used += n;
    memset(p, 0, n);
    return p;
}

// Frees every chunk (the end of the program, or just before exec).
static void FreeAll(void)
{
    while (gChunks) {
        Chunk *next = gChunks->next;
        free(gChunks);
        gChunks = next;
    }
}

// A dynamic array's new storage, twice as large, with the old items copied (the old storage stays in the
// arena until the end). Used by PUSH.
static void *GrowArray(void *items, size_t count, size_t *capacity, size_t elem)
{
    size_t cap = *capacity ? *capacity * 2 : 8;
    void *n = Alloc(cap * elem);
    if (count) memcpy(n, items, count * elem);
    *capacity = cap;
    return n;
}

// Appends an item to a {items, count, capacity} array in the arena.
#define PUSH(da, item)                                                                                   \
    do {                                                                                                 \
        if ((da)->count == (da)->capacity) {                                                             \
            (da)->items = GrowArray((da)->items, (da)->count, &(da)->capacity, sizeof *(da)->items);    \
        }                                                                                                \
        (da)->items[(da)->count++] = (item);                                                             \
    } while (0)

// A growing string in the arena (not NUL-terminated until StrC).
typedef struct {
    char *items;
    size_t count, capacity;
} Str;

// A list of strings in the arena.
typedef struct {
    const char **items;
    size_t count, capacity;
} Strings;

// Appends n bytes of p.
static void StrAdd(Str *s, const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) PUSH(s, p[i]);
}

// Appends a NUL-terminated string.
static void StrAddC(Str *s, const char *p)
{
    StrAdd(s, p, strlen(p));
}

// The string's text, NUL-terminated (the terminator isn't counted, so more can be appended later).
static char *StrC(Str *s)
{
    PUSH(s, '\0');
    s->count--;
    return s->items;
}

// A copy of n bytes of p, NUL-terminated, in the arena.
static char *Dup(const char *p, size_t n)
{
    char *d = Alloc(n + 1);
    memcpy(d, p, n);
    return d;
}

// A copy of a NUL-terminated string in the arena.
static char *DupC(const char *p)
{
    return Dup(p, strlen(p));
}

// printf into the arena (any length).
NOB_PRINTF_FORMAT(1, 2) static char *Fmt(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    char *d = Alloc((size_t)(n < 0 ? 0 : n) + 1);
    va_start(args, fmt);
    vsnprintf(d, (size_t)n + 1, fmt, args);
    va_end(args);
    return d;
}

// A heap string from libc (realpath, ToolNormalize) moved into the arena; NULL stays NULL.
static char *Adopt(char *heap)
{
    if (!heap) return NULL;
    char *d = DupC(heap);
    free(heap);
    return d;
}

// #endregion

// #region The decision

// The rules' names, as the refusal line prints them (header comment: what each refuses and why).
#define RULE_DELETE "no deletion outside /tmp"
#define RULE_LOST   "no lost work"
#define RULE_WINDOW "no visible window"
#define RULE_SCREEN "never touch the screen"
#define RULE_BENCH  "benchmarks"
#define RULE_STAY   "stay in the repo"

// What to use instead, per refusal (the text after the ';').
#define HINT_TRASH "move it to the trash with `gio trash <path>` (rm and the other deletion commands only " \
                   "take literal paths under /tmp/, e.g. your scratchpad)"
#define HINT_UNSEEN "run-sh.o can't check this path (a variable, $( ), a glob, braces, names from stdin, or a " \
                    "relative path after a cd it can't follow); write each path out literally under /tmp/, or " \
                    "use `gio trash <path>`"
#define HINT_GAME "run the game only with --shot <file>, --dump-boxes, --dump-scene, --dump-scene-diff, " \
                  "--dump-entities, --apply-edits, --inspect-model, --new-map, --export, --compile, " \
                  "--classify, --bake-surface or --bench --headless (CLAUDE.md)"
#define HINT_STAY "write only inside the repo or under /tmp/ (your scratchpad)"

// The characters that make a word uncheckable: what bash would expand (variables, substitutions, globs,
// braces).
#define UNSEEN_CHARS "$`*?[{"

// The first refusal found ("run-sh: refused: <rule>: <word>; <hint>"), or NULL while the command passes.
static const char *gRefusal;
// True when the command runs a benchmark (the game's --bench, an example's --bench, bench-export-ab): -c
// takes the benchmark lock.
static bool gBench;
// True once `export __GL_SYNC_TO_VBLANK=0` (or vblank_mode=0) appeared earlier in the line.
static bool gLineVsync;

// Records a refusal (only the first one counts: it is the one the message names).
static void Refuse(const char *rule, const char *word, const char *hint)
{
    if (gRefusal) return;
    gRefusal = Fmt("run-sh: refused: %s: %s; %s", rule, word, hint);
}

// #endregion

// #region Splitting

// One piece of a command line between separators: its words (quotes removed) and the files it writes
// with > / >>. A segment with `mark` '(' or ')' is only a marker: a subshell or $( ) starts or ends there.
typedef struct {
    Strings words, writes;
    char mark;
} Segment;

typedef struct {
    Segment *items;
    size_t count, capacity;
} Segments;

// The splitter's state: the text and position, the segments so far, the segment and word being read.
// `next` says what the word being read is: 'w' a word, '>' a file written, '-' a word to drop (an input
// file, a here-string), '<' a here-document's delimiter. `dup` marks a >& target (a digit or '-' is a
// descriptor, not a file). `heredocs` are the delimiters whose bodies start after the next newline, each
// with a first character '-' (<<-: leading tabs ignored) or ' '.
typedef struct {
    const char *s;
    size_t pos;
    Segments out;
    Segment cur;
    Str word;
    bool inWord, dup, heredocDash;
    char next;
    Strings heredocs;
} Splitter;

// Is the text a non-empty run of digits (a descriptor number)?
static bool IsDigits(const char *t)
{
    if (!*t) return false;
    for (; *t; t++) {
        if (!isdigit((unsigned char)*t)) return false;
    }
    return true;
}

// Ends the word being read and files it where `next` says.
static void EndWord(Splitter *sp)
{
    if (!sp->inWord) return;
    const char *w = StrC(&sp->word);
    sp->word = (Str){0};
    sp->inWord = false;
    char next = sp->next;
    sp->next = 'w';
    if (next == 'w') {
        PUSH(&sp->cur.words, w);
    } else if (next == '>') {
        if (!(sp->dup && (IsDigits(w) || strcmp(w, "-") == 0))) PUSH(&sp->cur.writes, w);
    } else if (next == '<') {
        PUSH(&sp->heredocs, Fmt("%c%s", sp->heredocDash ? '-' : ' ', w));
    }
    sp->dup = false;
}

// Ends the segment being read (kept when it has words or writes), then adds a marker when `mark` is set.
static void EndSegment(Splitter *sp, char mark)
{
    EndWord(sp);
    sp->next = 'w';
    if (sp->cur.words.count || sp->cur.writes.count) PUSH(&sp->out, sp->cur);
    sp->cur = (Segment){0};
    if (mark) PUSH(&sp->out, ((Segment){.mark = mark}));
}

// After a newline: skips the bodies of the here-documents opened on the line before, each up to its
// delimiter line (or the end of the text).
static void SkipHeredocs(Splitter *sp)
{
    for (size_t h = 0; h < sp->heredocs.count; h++) {
        const char *delim = sp->heredocs.items[h] + 1;
        bool dash = sp->heredocs.items[h][0] == '-';
        while (sp->s[sp->pos]) {
            const char *line = sp->s + sp->pos;
            const char *eol = strchr(line, '\n');
            size_t len = eol ? (size_t)(eol - line) : strlen(line);
            sp->pos += len + (eol ? 1 : 0);
            if (dash) {
                while (len > 0 && *line == '\t') line++, len--;
            }
            if (len == strlen(delim) && strncmp(line, delim, len) == 0) break;
        }
    }
    sp->heredocs.count = 0;
}

// A redirection operator at the character `c` ('<' or '>', already read): says what the next word is.
// A descriptor number right before it (2>) is not a word; <( and >( start a subshell.
static void Redirection(Splitter *sp, char c)
{
    if (sp->inWord && IsDigits(StrC(&sp->word))) {
        sp->word = (Str){0};
        sp->inWord = false;
    }
    EndWord(sp);
    const char *s = sp->s;
    if (s[sp->pos] == '(') {
        sp->pos++;
        EndSegment(sp, '(');
        return;
    }
    if (c == '<') {
        if (s[sp->pos] == '<' && s[sp->pos + 1] == '<') {
            sp->pos += 2;  // <<< here-string: its word is data
            sp->next = '-';
        } else if (s[sp->pos] == '<') {
            sp->pos++;  // << here-document: its delimiter
            sp->heredocDash = s[sp->pos] == '-';
            if (sp->heredocDash) sp->pos++;
            sp->next = '<';
        } else {
            if (s[sp->pos] == '&' || s[sp->pos] == '>') sp->pos++;  // <& and <>
            sp->next = '-';
        }
        return;
    }
    if (s[sp->pos] == '>' || s[sp->pos] == '|') sp->pos++;  // >> and >|
    if (s[sp->pos] == '&') {
        sp->pos++;  // >&: a descriptor, or a file
        sp->dup = true;
    }
    sp->next = '>';
}

// Cuts a command line into segments (header comment, "How it reads a command", step 1).
static Segments Split(const char *s)
{
    Splitter sp = {.s = s, .next = 'w'};
    char quote = 0;
    while (s[sp.pos]) {
        char c = s[sp.pos++];

        // inside quotes: '…' is literal; in "…" a backslash keeps only " \ $ `
        if (quote == '\'') {
            if (c == '\'') quote = 0;
            else PUSH(&sp.word, c);
            continue;
        }
        if (quote == '"') {
            if (c == '"') quote = 0;
            else if (c == '\\' && s[sp.pos] && strchr("\"\\$`", s[sp.pos])) PUSH(&sp.word, s[sp.pos++]);
            else PUSH(&sp.word, c);
            continue;
        }

        // outside quotes: escapes, quotes, comments, blanks
        if (c == '\\') {
            if (s[sp.pos] == '\n') {
                sp.pos++;  // a line continuation
            } else if (s[sp.pos]) {
                PUSH(&sp.word, s[sp.pos++]);
                sp.inWord = true;
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            sp.inWord = true;
            continue;
        }
        if (c == '#' && !sp.inWord) {
            while (s[sp.pos] && s[sp.pos] != '\n') sp.pos++;
            continue;
        }
        if (c == ' ' || c == '\t') {
            EndWord(&sp);
            continue;
        }

        // separators and redirections
        if (c == '\n') {
            EndSegment(&sp, 0);
            SkipHeredocs(&sp);
        } else if (c == '$' && s[sp.pos] == '(') {
            sp.pos++;
            EndSegment(&sp, '(');
        } else if (c == '(' || c == ')') {
            EndSegment(&sp, c);
        } else if (c == ';' || c == '|' || c == '`') {
            EndSegment(&sp, 0);
        } else if (c == '&') {
            if (s[sp.pos] == '>') EndWord(&sp);  // &> writes: the '>' comes next
            else EndSegment(&sp, 0);
        } else if (c == '<' || c == '>') {
            Redirection(&sp, c);
        } else {
            PUSH(&sp.word, c);
            sp.inWord = true;
        }
    }
    EndSegment(&sp, 0);
    return sp.out;
}

// #endregion

// #region Paths

// The repo root as named (gToolRoot) and as resolved: "stay in the repo" accepts either.
static const char *gRootReal;

// The last path component.
static const char *Base(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

// Is `path` `root` or inside it?
static bool Under(const char *path, const char *root)
{
    size_t n = strlen(root);
    if (n > 0 && root[n - 1] == '/') n--;
    return strncmp(path, root, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

// A leading `~` or `~/` replaced by $HOME (the word as it was when HOME isn't set).
static const char *Tilde(const char *p)
{
    const char *home = getenv("HOME");
    if (p[0] != '~' || (p[1] && p[1] != '/') || !home) return p;
    return Fmt("%s%s", home, p + 1);
}

// `path` made absolute against `cwd` (NULL when it is relative and cwd is unknown), lexically normalised.
static const char *Absolute(const char *cwd, const char *path)
{
    if (path[0] != '/' && !cwd) return NULL;
    const char *joined = path[0] == '/' ? path : Fmt("%s/%s", cwd, path);
    return Adopt(ToolNormalize(joined));
}

// Where a path really points (header comment, "How under /tmp is decided"): absolute against cwd, then
// each existing component replaced by its realpath (symlinks followed, `..` physical), the rest lexical.
// The last component is followed only when `followLast` or the path ends in '/'. NULL when the path is
// relative and the working directory unknown.
static const char *Resolve(const char *cwd, const char *path, bool followLast)
{
    if (path[0] != '/' && !cwd) return NULL;
    const char *abs = path[0] == '/' ? path : Fmt("%s/%s", cwd, path);
    size_t n = strlen(abs);
    if (n > 1 && abs[n - 1] == '/') followLast = true;

    // the components
    Strings parts = {0};
    for (const char *p = abs; *p;) {
        while (*p == '/') p++;
        const char *s = p;
        while (*p && *p != '/') p++;
        if (p > s) PUSH(&parts, Dup(s, (size_t)(p - s)));
    }

    // the walk: `cur` is a real directory while nothing was missing
    Str cur = {0};
    bool missing = false;
    for (size_t i = 0; i < parts.count; i++) {
        const char *c = parts.items[i];
        if (strcmp(c, ".") == 0) continue;
        if (strcmp(c, "..") == 0) {
            while (cur.count > 0 && cur.items[cur.count - 1] != '/') cur.count--;
            if (cur.count > 0) cur.count--;
            continue;
        }
        PUSH(&cur, '/');
        StrAddC(&cur, c);
        if (missing || (i + 1 == parts.count && !followLast)) continue;
        struct stat st;
        char *real = lstat(StrC(&cur), &st) == 0 ? Adopt(realpath(StrC(&cur), NULL)) : NULL;
        if (!real) {
            missing = true;  // missing, or a dangling link: the rest is lexical
            continue;
        }
        // the real path replaces everything walked so far (it is absolute)
        cur.count = 0;
        if (strcmp(real, "/") != 0) StrAddC(&cur, real);
    }
    return cur.count == 0 ? "/" : StrC(&cur);
}

// A path a deletion removes (`word` names it in the message): it must be checkable and resolve strictly
// under /tmp/.
static void CheckDeletePath(const char *cwd, const char *path, bool followLast, const char *word)
{
    if (strpbrk(path, UNSEEN_CHARS)) {
        Refuse(RULE_DELETE, word, HINT_UNSEEN);
        return;
    }
    const char *r = Resolve(cwd, Tilde(path), followLast);
    if (!r) Refuse(RULE_DELETE, word, HINT_UNSEEN);
    else if (strncmp(r, "/tmp/", 5) != 0 || r[5] == '\0') Refuse(RULE_DELETE, word, HINT_TRASH);
}

// A path a command writes to ("stay in the repo"): only a literal absolute path (or ~) is checked; it
// must resolve into the repo or /tmp, or be one of the terminal / descriptor devices.
static void CheckWritePath(const char *cwd, const char *path, const char *word)
{
    if (strpbrk(path, UNSEEN_CHARS)) return;
    const char *p = Tilde(path);
    if (p[0] != '/') return;
    if (strcmp(p, "/dev/null") == 0 || strcmp(p, "/dev/stdout") == 0 || strcmp(p, "/dev/stderr") == 0 ||
        strcmp(p, "/dev/tty") == 0 || strncmp(p, "/dev/fd/", 8) == 0) {
        return;
    }
    const char *r = Resolve(cwd, p, true);
    if (Under(r, "/tmp") || Under(r, gToolRoot) || Under(r, gRootReal)) return;
    Refuse(RULE_STAY, word, HINT_STAY);
}

// #endregion

// #region Checking a segment

// The command of a segment: its words from the command word on, whether vsync was switched off in front
// of it (NAME=value or env's), and the wrapper it runs under (NULL: none).
typedef struct {
    const char **w;
    size_t n;
    bool vsync;
    const char *wrapper;
} Command;

// Does a list of words contain this flag, as a word of its own or as flag=value?
static bool Has(const char **a, size_t n, const char *flag)
{
    size_t fl = strlen(flag);
    for (size_t i = 0; i < n; i++) {
        if (strncmp(a[i], flag, fl) == 0 && (a[i][fl] == '\0' || a[i][fl] == '=')) return true;
    }
    return false;
}

// Does a short-option cluster (-xyz, not --long) among the words contain this letter?
static bool HasShort(const char **a, size_t n, char letter)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i][0] == '-' && a[i][1] != '-' && strchr(a[i] + 1, letter)) return true;
    }
    return false;
}

// Is a word one of the names in a NULL-terminated list?
static bool OneOf(const char *w, const char *const *list)
{
    for (; *list; list++) {
        if (strcmp(w, *list) == 0) return true;
    }
    return false;
}

// Is the word a shell assignment NAME=value?
static bool IsAssign(const char *w)
{
    if (!(isalpha((unsigned char)w[0]) || w[0] == '_')) return false;
    while (isalnum((unsigned char)*w) || *w == '_') w++;
    return *w == '=';
}

// Is the word an assignment that switches vsync off (__GL_SYNC_TO_VBLANK=0, vblank_mode=0)?
static bool IsVsyncOff(const char *w)
{
    return strcmp(w, "__GL_SYNC_TO_VBLANK=0") == 0 || strcmp(w, "vblank_mode=0") == 0;
}

// Is vsync already off in run-sh.o's own environment (exported by the caller)?
static bool EnvVsync(void)
{
    const char *a = getenv("__GL_SYNC_TO_VBLANK"), *b = getenv("vblank_mode");
    return (a && strcmp(a, "0") == 0) || (b && strcmp(b, "0") == 0);
}

// The wrappers that run the rest of the segment as a command, and their options that take a value.
static const char *const WRAPPERS[] = {"sudo", "env", "nice", "nohup", "timeout", "time", "command", "exec",
                                       "xargs", NULL};
static const char *const WRAPPER_VALUES[] = {"sudo -u", "sudo -g", "sudo -h", "sudo -p", "sudo -C", "sudo -D",
                                             "env -u", "env -C", "nice -n", "timeout -s", "timeout -k",
                                             "time -f", "time -o", "exec -a", "xargs -a", "xargs -d", "xargs -E",
                                             "xargs -I", "xargs -L", "xargs -n", "xargs -P", "xargs -s", NULL};
// The keywords that may stand in front of a command.
static const char *const KEYWORDS[] = {"if", "then", "do", "else", "elif", "while", "until", "!", "{", NULL};

// Finds a segment's command (header comment, "How it reads a command", step 2). False when the segment
// has none (only assignments, `command -v`).
static bool FindCommand(const Segment *seg, Command *c)
{
    const char **w = seg->words.items;
    size_t n = seg->words.count, i = 0;
    *c = (Command){0};

    // keywords and NAME=value words in front
    while (i < n && OneOf(w[i], KEYWORDS)) i++;
    for (; i < n && IsAssign(w[i]); i++) c->vsync = c->vsync || IsVsyncOff(w[i]);

    // one wrapper with its options (env: then its NAME=value words; timeout: then its duration)
    if (i < n && OneOf(Base(w[i]), WRAPPERS)) {
        c->wrapper = Base(w[i++]);
        while (i < n && w[i][0] == '-' && w[i][1]) {
            if (strcmp(w[i], "--") == 0) {
                i++;
                break;
            }
            if (strcmp(c->wrapper, "command") == 0 && (strcmp(w[i], "-v") == 0 || strcmp(w[i], "-V") == 0)) {
                return false;  // only a lookup
            }
            i += OneOf(Fmt("%s %s", c->wrapper, w[i]), WRAPPER_VALUES) ? 2 : 1;
        }
        if (strcmp(c->wrapper, "env") == 0) {
            for (; i < n && IsAssign(w[i]); i++) c->vsync = c->vsync || IsVsyncOff(w[i]);
        }
        if (strcmp(c->wrapper, "timeout") == 0 && i < n) i++;
    }
    if (i >= n) return false;
    c->w = w + i;
    c->n = n - i;
    return true;
}

// The deletion commands.
static const char *const DELETERS[] = {"rm", "rmdir", "unlink", "shred", NULL};

// Rule "no deletion outside /tmp" for rm / rmdir / unlink / shred and find (git: RuleGit).
static void RuleDelete(const char *cwd, const Command *c)
{
    const char *b = Base(c->w[0]);
    if (OneOf(b, DELETERS)) {
        // with xargs the names come from stdin
        if (c->wrapper && strcmp(c->wrapper, "xargs") == 0) {
            Refuse(RULE_DELETE, Fmt("xargs %s", b), HINT_UNSEEN);
            return;
        }
        // every operand (options until --)
        bool ended = false;
        for (size_t i = 1; i < c->n; i++) {
            const char *a = c->w[i];
            if (!ended && strcmp(a, "--") == 0) ended = true;
            else if (ended || a[0] != '-' || !a[1]) CheckDeletePath(cwd, a, false, Fmt("%s %s", c->w[0], a));
        }
        return;
    }
    if (strcmp(b, "find") != 0) return;

    // find: the roots are the words before the expression (after -H / -L / -P / -O / -D x); "." without
    static const char *const starts[] = {"-H", "-L", "-P", NULL};
    size_t i = 1;
    while (i < c->n && (OneOf(c->w[i], starts) || strncmp(c->w[i], "-O", 2) == 0 || strcmp(c->w[i], "-D") == 0)) {
        i += strcmp(c->w[i], "-D") == 0 ? 2 : 1;
    }
    size_t first = i;
    while (i < c->n && c->w[i][0] != '-' && strcmp(c->w[i], "(") != 0 && strcmp(c->w[i], "!") != 0) i++;
    size_t roots = i;
    // a deletion: -delete, or -exec / -execdir / -ok / -okdir running a deletion command
    static const char *const execs[] = {"-exec", "-execdir", "-ok", "-okdir", NULL};
    bool del = false;
    for (; i < c->n; i++) {
        if (strcmp(c->w[i], "-delete") == 0) del = true;
        if (OneOf(c->w[i], execs) && i + 1 < c->n && OneOf(Base(c->w[i + 1]), DELETERS)) del = true;
    }
    if (!del) return;
    if (roots == first) CheckDeletePath(cwd, ".", true, "find .");
    for (size_t k = first; k < roots; k++) CheckDeletePath(cwd, c->w[k], true, Fmt("find %s", c->w[k]));
}

// Rules "no deletion outside /tmp" and "no lost work" for git.
static void RuleGit(const char *cwd, const Command *c)
{
    // git's own options before the subcommand (-C and -c take a value; -C changes the directory)
    size_t i = 1;
    while (i < c->n && c->w[i][0] == '-') {
        if (strcmp(c->w[i], "-C") == 0 && i + 1 < c->n) cwd = Absolute(cwd, c->w[i + 1]);
        i += (strcmp(c->w[i], "-C") == 0 || strcmp(c->w[i], "-c") == 0) ? 2 : 1;
    }
    if (i >= c->n) return;
    const char *sub = c->w[i];
    const char **r = c->w + i + 1;
    size_t rn = c->n - i - 1;

    if (strcmp(sub, "clean") == 0 && !Has(r, rn, "--dry-run") && !HasShort(r, rn, 'n')) {
        Refuse(RULE_DELETE, "git clean",
               "it deletes untracked files for good: look with `git clean -n`, then `gio trash` what should go");
    } else if (strcmp(sub, "rm") == 0 && !Has(r, rn, "--cached")) {
        Refuse(RULE_DELETE, "git rm",
               "use `git rm --cached <path>` (the file stays), or `gio trash <path>` and let git see it gone");
    } else if (strcmp(sub, "worktree") == 0 && rn > 0 && strcmp(r[0], "remove") == 0) {
        for (size_t k = 1; k < rn; k++) {
            if (r[k][0] != '-') CheckDeletePath(cwd, r[k], true, Fmt("git worktree remove %s", r[k]));
        }
    } else if (strcmp(sub, "reset") == 0 && Has(r, rn, "--hard")) {
        Refuse(RULE_LOST, "git reset --hard",
               "it throws away uncommitted work: `git stash` keeps it; ask the developer before discarding");
    } else if (strcmp(sub, "checkout") == 0) {
        const char *bad = Has(r, rn, "--") ? "--" : Has(r, rn, ".") ? "." : Has(r, rn, "--force") ? "--force"
                        : Has(r, rn, "-f") ? "-f" : NULL;
        if (bad) {
            Refuse(RULE_LOST, Fmt("git checkout %s", bad),
                   "it overwrites uncommitted changes: `git stash` keeps them; ask the developer before discarding");
        }
    } else if (strcmp(sub, "restore") == 0 && !Has(r, rn, "--staged") && !HasShort(r, rn, 'S')) {
        Refuse(RULE_LOST, "git restore",
               "only `git restore --staged <path>` (unstage) is allowed: discarding changes needs the developer");
    } else if (strcmp(sub, "stash") == 0 && rn > 0 && (strcmp(r[0], "drop") == 0 || strcmp(r[0], "clear") == 0)) {
        Refuse(RULE_LOST, Fmt("git stash %s", r[0]), "the stashed work would be lost: leave the stash, or ask the developer");
    }
}

// The game's arguments (after `monolith`, `build.sh run`, ...): it must not open a window, and a benchmark
// must be headless, vsync-off, with a note. `word` names it in the message.
static void CheckGame(const char *word, const char **a, size_t n, bool vsync)
{
    if (Has(a, n, "--bench")) {
        gBench = true;
        if (!Has(a, n, "--headless")) {
            Refuse(RULE_BENCH, Fmt("%s --bench", word), "add --headless (CLAUDE.md, the benchmarking skill)");
        }
        if (!vsync) {
            Refuse(RULE_BENCH, Fmt("%s --bench", word),
                   "put __GL_SYNC_TO_VBLANK=0 in front of the command (vblank_mode=0 on Mesa)");
        }
        bool note = false;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(a[i], "--note") == 0 && i + 1 < n && a[i + 1][0]) note = true;
            if (strncmp(a[i], "--note=", 7) == 0 && a[i][7]) note = true;
        }
        if (!note) Refuse(RULE_BENCH, Fmt("%s --bench", word), "add --note \"what changed\"");
        if (Has(a, n, "--headless")) return;
    }
    static const char *const windowless[] = {"--shot", "--dump-boxes", "--dump-scene", "--dump-scene-diff",
                                             "--dump-entities", "--apply-edits", "--inspect-model", "--new-map",
                                             "--export", "--compile", "--classify", "--bake-surface", NULL};
    for (const char *const *f = windowless; *f; f++) {
        if (Has(a, n, *f)) return;
    }
    Refuse(RULE_WINDOW, word, HINT_GAME);
}

// Rules "no visible window" and "benchmarks" for the game (also under wine, `build.sh run`, `nob run`) and
// the examples.
static void RuleWindow(const char *cwd, const Command *c, bool vsync)
{
    const char *b = Base(c->w[0]);
    size_t k = 0;  // the program's word: the command, or wine's program
    if (strcmp(b, "wine") == 0 || strcmp(b, "wine64") == 0) {
        k = 1;
        while (k < c->n && c->w[k][0] == '-') k++;
        if (k >= c->n) return;
        b = Base(c->w[k]);
    }
    const char *word = c->w[k];
    const char **a = c->w + k + 1;
    size_t n = c->n - k - 1;

    if (strcmp(b, "monolith") == 0 || strcmp(b, "monolith.exe") == 0) {
        CheckGame(word, a, n, vsync);
    } else if (strcmp(b, "build.sh") == 0 || strcmp(b, "nob") == 0) {
        size_t i = n > 0 && strcmp(a[0], "--win") == 0 ? 1 : 0;
        if (i < n && strcmp(a[i], "run") == 0) CheckGame(Fmt("%s run", word), a + i + 1, n - i - 1, vsync);
    } else if (strcmp(b, "bench-export-ab") == 0) {
        gBench = true;
    } else if (strchr(word, '/')) {
        // the plain examples open a window; their test builds (linux-test, linux-lsan) don't
        const char *abs = Absolute(cwd, word);
        const char *where = abs ? abs : word;
        if (strstr(where, "examples/build/linux/") || strstr(where, "examples/build/win/")) {
            Refuse(RULE_WINDOW, word,
                   "the plain examples always open a window: run only their test build "
                   "(examples/build/linux-test/<example>/<program> --shot <file>)");
        } else if ((strstr(where, "examples/build/linux-test/") || strstr(where, "examples/build/linux-lsan/")) &&
                   Has(a, n, "--bench")) {
            gBench = true;
            if (!vsync) Refuse(RULE_BENCH, Fmt("%s --bench", word), "put __GL_SYNC_TO_VBLANK=0 in front of the command");
        }
    }
}

// Rule "never touch the screen": i3-msg / swaymsg only with -t get_* and no payload; xdotool, wmctrl never.
static void RuleScreen(const Command *c)
{
    const char *b = Base(c->w[0]);
    if (strcmp(b, "xdotool") == 0 || strcmp(b, "wmctrl") == 0) {
        Refuse(RULE_SCREEN, b, "never move windows, focus or workspaces (CLAUDE.md); `i3-msg -t get_tree` reads the tree");
        return;
    }
    if (strcmp(b, "i3-msg") != 0 && strcmp(b, "swaymsg") != 0) return;
    const char *type = NULL;
    bool payload = false;
    for (size_t k = 1; k < c->n; k++) {
        const char *o = c->w[k];
        if ((strcmp(o, "-t") == 0 || strcmp(o, "--type") == 0) && k + 1 < c->n) type = c->w[++k];
        else if (strncmp(o, "--type=", 7) == 0) type = o + 7;
        else if (strncmp(o, "-t", 2) == 0 && o[2]) type = o + 2;
        else if (strcmp(o, "-s") == 0 || strcmp(o, "--socket") == 0) k++;
        else if (o[0] != '-') payload = true;
    }
    if (!type || strncmp(type, "get_", 4) != 0 || payload) {
        Refuse(RULE_SCREEN, b, "only reading is allowed: `i3-msg -t get_tree` (get_workspaces, get_outputs, ...)");
    }
}

// Rule "stay in the repo" for the commands that write their operands: cp / mv (the last of two or more),
// tee / mkdir / touch (every one; -m, -d, -r and -t take a value).
static void RuleStay(const char *cwd, const Command *c)
{
    const char *b = Base(c->w[0]);
    bool copy = strcmp(b, "cp") == 0 || strcmp(b, "mv") == 0;
    bool each = strcmp(b, "tee") == 0 || strcmp(b, "mkdir") == 0 || strcmp(b, "touch") == 0;
    if (!copy && !each) return;
    static const char *const values[] = {"-m", "-d", "-r", "-t", NULL};
    const char *last = NULL;
    size_t operands = 0;
    for (size_t k = 1; k < c->n; k++) {
        const char *o = c->w[k];
        if (o[0] == '-' && o[1]) {
            if (each && OneOf(o, values)) k++;
            continue;
        }
        operands++;
        last = o;
        if (each) CheckWritePath(cwd, o, Fmt("%s %s", b, o));
    }
    if (copy && operands >= 2) CheckWritePath(cwd, last, Fmt("%s %s", b, last));
}

// Checks one segment with `*cwd` as its working directory (NULL: unknown); a cd changes it.
static void CheckSegment(const char **cwd, const Segment *seg)
{
    // MONOLITH_ALLOW_WINDOW=… anywhere, and the files the segment writes with > / >>
    for (size_t i = 0; i < seg->words.count; i++) {
        if (strncmp(seg->words.items[i], "MONOLITH_ALLOW_WINDOW=", 22) == 0) {
            Refuse(RULE_WINDOW, "MONOLITH_ALLOW_WINDOW",
                   "never set it: a session must not open a window (CLAUDE.md); run the game with --shot <file> "
                   "or another windowless flag");
        }
    }
    for (size_t i = 0; i < seg->writes.count; i++) {
        CheckWritePath(*cwd, seg->writes.items[i], Fmt("> %s", seg->writes.items[i]));
    }
    Command c;
    if (gRefusal || !FindCommand(seg, &c)) return;
    const char *b = Base(c.w[0]);

    // cd: where the segments after it run
    if (strcmp(b, "cd") == 0) {
        const char *home = getenv("HOME");
        const char *to = c.n > 1 && strcmp(c.w[1], "--") == 0 ? (c.n > 2 ? c.w[2] : NULL) : (c.n > 1 ? c.w[1] : NULL);
        if (!to) *cwd = home && home[0] == '/' ? home : NULL;
        else if (to[0] == '-' || strpbrk(to, UNSEEN_CHARS)) *cwd = NULL;
        else *cwd = Absolute(*cwd, Tilde(to));
        return;
    }
    // export and its kin: MONOLITH_ALLOW_WINDOW never, vsync off for the rest of the line
    static const char *const exports[] = {"export", "declare", "typeset", "readonly", "local", NULL};
    if (OneOf(b, exports)) {
        for (size_t i = 1; i < c.n; i++) {
            if (strncmp(c.w[i], "MONOLITH_ALLOW_WINDOW", 21) == 0) {
                Refuse(RULE_WINDOW, "MONOLITH_ALLOW_WINDOW",
                       "never set it: a session must not open a window (CLAUDE.md)");
            }
            if (IsVsyncOff(c.w[i])) gLineVsync = true;
        }
        return;
    }

    // the rules
    RuleDelete(*cwd, &c);
    if (strcmp(b, "git") == 0) RuleGit(*cwd, &c);
    RuleWindow(*cwd, &c, c.vsync || gLineVsync || EnvVsync());
    RuleScreen(&c);
    RuleStay(*cwd, &c);
}

// The decision for one command line, with `cwd` as its working directory: NULL when allowed, else the
// refusal line. Sets gBench when it runs a benchmark. A subshell's or $( )'s cd ends with it.
static const char *Decide(const char *command, const char *cwd)
{
    gRefusal = NULL;
    gBench = false;
    gLineVsync = false;
    Segments segs = Split(command);
    Strings saved = {0};
    const char *dir = cwd && cwd[0] == '/' ? cwd : NULL;
    for (size_t i = 0; i < segs.count && !gRefusal; i++) {
        const Segment *s = &segs.items[i];
        if (s->mark == '(') PUSH(&saved, dir);
        else if (s->mark == ')') dir = saved.count ? saved.items[--saved.count] : dir;
        else CheckSegment(&dir, s);
    }
    return gRefusal;
}

// #endregion

// #region Log and benchmark lock

// The folder of the log and the lock: $RUN_SH_DIR or <repo>/build/run-sh.
static const char *StateDir(void)
{
    const char *d = getenv("RUN_SH_DIR");
    return d && d[0] ? d : Fmt("%s/build/run-sh", gToolRoot);
}

// Appends one line to the log (header comment: the format). A log that can't be written is skipped
// silently: the command still runs (the log is for auditing, not a guard).
static void LogDecision(const char *mode, const char *command, const char *cwd, const char *decision)
{
    const char *dir = StateDir();
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        // ToolMkdirP reports its failure; the log must stay silent, so stderr is muted for it
        int saved = dup(STDERR_FILENO), null = open("/dev/null", O_WRONLY);
        if (null >= 0) dup2(null, STDERR_FILENO);
        ToolMkdirP(dir);
        if (saved >= 0) dup2(saved, STDERR_FILENO);
        if (saved >= 0) close(saved);
        if (null >= 0) close(null);
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    Str line = {0};
    StrAddC(&line, Fmt("%04d-%02d-%02dT%02d:%02d:%02d\t%s\t%s\t%s\t", tm.tm_year + 1900, tm.tm_mon + 1,
                       tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, mode, decision ? decision : "allowed",
                       cwd ? cwd : "?"));
    for (const char *p = command; *p; p++) {
        if (*p == '\\') StrAddC(&line, "\\\\");
        else if (*p == '\n') StrAddC(&line, "\\n");
        else if (*p == '\t') StrAddC(&line, "\\t");
        else PUSH(&line, *p);
    }
    PUSH(&line, '\n');
    // one write() with O_APPEND: lines of sessions running at once don't interleave
    int fd = open(Fmt("%s/log", dir), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    ssize_t w = write(fd, line.items, line.count);
    (void)w;
    close(fd);
}

// Takes the benchmark lock for the rest of this process and everything it execs: an fd without
// O_CLOEXEC, flock'ed. False when another run holds it.
static bool TakeBenchLock(void)
{
    const char *dir = StateDir();
    ToolMkdirP(dir);
    int fd = open(Fmt("%s/bench.lock", dir), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        ToolErr("can't open the benchmark lock %s/bench.lock: %s", dir, strerror(errno));
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return false;
    }
    return true;  // fd stays open: bash and its children inherit it, and the lock ends with the last of them
}

// #endregion

// #region -c and --check

// run-sh.o -c '<command>' [args] (run) or --check '<command>': decide in the current directory and log;
// --check prints the decision; -c takes the benchmark lock when needed and execs bash, or prints the refusal.
static int CheckAndRun(bool run, int argc, char **argv)
{
    const char *command = argv[2];
    char *cwd = ToolLogicalCwd();
    const char *where = cwd ? DupC(cwd) : NULL;
    free(cwd);
    const char *refusal = Decide(command, where);
    if (run && !refusal && gBench && !TakeBenchLock()) {
        refusal = "run-sh: refused: " RULE_BENCH ": --bench; another benchmark is running (one at a time, "
                  "CLAUDE.md): wait for it to end";
    }
    LogDecision(run ? "-c" : "--check", command, where, refusal);
    if (!run || refusal) {
        if (run) fprintf(stderr, "%s\n", refusal);
        else printf("%s\n", refusal ? refusal : "run-sh: allowed");
        FreeAll();
        return refusal ? 126 : 0;
    }
    // bash -c '<command>' [args...]: the arguments after the command are $0, $1, ... as with bash -c
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "bash", "-c", command);
    for (int i = 3; i < argc; i++) nob_cmd_append(&cmd, argv[i]);
    FreeAll();
    ToolExec(&cmd);  // returns only on failure (it exits 127 / 126)
    return 126;
}

// #endregion

// #region --hook: the PreToolUse hook

// A JSON value as the hook needs it: its type, its source span (copied verbatim into the answer), the
// decoded text of a string, and an object's members.
typedef struct JMember JMember;

typedef struct {
    char type;            // 'o' object, 'a' array, 's' string, 'n' number, 'b' true/false, 'z' null
    size_t start, end;    // the value's source text
    const char *str;      // 's': the decoded text
    struct {
        JMember *items;
        size_t count, capacity;
    } members;            // 'o'
} JValue;

struct JMember {
    const char *key;
    JValue value;
};

typedef struct {
    const char *s;
    size_t n, pos;
    const char *error;
} JReader;

// Steps over JSON whitespace.
static void JSpace(JReader *r)
{
    while (r->pos < r->n && strchr(" \t\r\n", r->s[r->pos])) r->pos++;
}

// Records the first JSON error and returns false.
static bool JFail(JReader *r, const char *what)
{
    if (!r->error) r->error = what;
    return false;
}

// Four hex digits at r->pos into *out (r->pos moves past them); false when they aren't there.
static bool JHex4(JReader *r, unsigned long *out)
{
    if (r->pos + 4 > r->n) return false;
    unsigned long v = 0;
    for (size_t i = 0; i < 4; i++) {
        int c = tolower((unsigned char)r->s[r->pos + i]);
        int d = isdigit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (d < 0) return false;
        v = v * 16 + (unsigned long)d;
    }
    r->pos += 4;
    *out = v;
    return true;
}

// Appends a code point as UTF-8.
static void AddUtf8(Str *s, unsigned long c)
{
    // the lead byte carries the length; each continuation byte 6 bits, highest first
    static const unsigned char lead[] = {0x00, 0xC0, 0xE0, 0xF0};
    int extra = c < 0x80 ? 0 : c < 0x800 ? 1 : c < 0x10000 ? 2 : 3;
    PUSH(s, (char)(lead[extra] | (c >> (6 * extra))));
    for (int i = extra - 1; i >= 0; i--) PUSH(s, (char)(0x80 | ((c >> (6 * i)) & 0x3F)));
}

// A JSON string at r->pos, decoded (\uXXXX with surrogate pairs to UTF-8). A NUL in it is refused: a
// command can't hold one.
static bool JString(JReader *r, const char **out)
{
    if (r->pos >= r->n || r->s[r->pos] != '"') return JFail(r, "a string was expected");
    r->pos++;
    Str w = {0};
    for (;;) {
        if (r->pos >= r->n) return JFail(r, "unterminated string");
        unsigned char c = (unsigned char)r->s[r->pos++];
        if (c == '"') break;
        if (c < 0x20) return JFail(r, "a control character in a string");
        if (c != '\\') {
            PUSH(&w, (char)c);
            continue;
        }
        if (r->pos >= r->n) return JFail(r, "unterminated string");
        char e = r->s[r->pos++];
        switch (e) {
        case '"': case '\\': case '/': PUSH(&w, e); break;
        case 'b': PUSH(&w, '\b'); break;
        case 'f': PUSH(&w, '\f'); break;
        case 'n': PUSH(&w, '\n'); break;
        case 'r': PUSH(&w, '\r'); break;
        case 't': PUSH(&w, '\t'); break;
        case 'u': {
            unsigned long cp, lo;
            if (!JHex4(r, &cp)) return JFail(r, "a bad \\u escape");
            // a high surrogate followed by \u and a low one: one code point
            if (cp >= 0xD800 && cp < 0xDC00 && r->pos + 6 <= r->n && r->s[r->pos] == '\\' && r->s[r->pos + 1] == 'u') {
                size_t back = r->pos;
                r->pos += 2;
                if (JHex4(r, &lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                else r->pos = back;
            }
            if (cp == 0) return JFail(r, "a NUL character in a string");
            AddUtf8(&w, cp);
            break;
        }
        default: return JFail(r, "a bad escape in a string");
        }
    }
    *out = StrC(&w);
    return true;
}

// Any JSON value at r->pos (objects keep their members, arrays are only skipped).
static bool JRead(JReader *r, JValue *v, int depth)
{
    if (depth > 64) return JFail(r, "nested too deep");
    JSpace(r);
    if (r->pos >= r->n) return JFail(r, "a value was expected");
    v->start = r->pos;
    char c = r->s[r->pos];
    if (c == '{' || c == '[') {
        // an object (members kept) or an array (items read and dropped)
        char close = c == '{' ? '}' : ']';
        v->type = c == '{' ? 'o' : 'a';
        r->pos++;
        JSpace(r);
        if (r->pos < r->n && r->s[r->pos] == close) {
            r->pos++;
        } else {
            for (;;) {
                JSpace(r);
                JMember m = {0};
                if (v->type == 'o') {
                    if (!JString(r, &m.key)) return false;
                    JSpace(r);
                    if (r->pos >= r->n || r->s[r->pos] != ':') return JFail(r, "':' was expected");
                    r->pos++;
                }
                if (!JRead(r, &m.value, depth + 1)) return false;
                if (v->type == 'o') PUSH(&v->members, m);
                JSpace(r);
                if (r->pos < r->n && r->s[r->pos] == ',') {
                    r->pos++;
                    continue;
                }
                if (r->pos < r->n && r->s[r->pos] == close) {
                    r->pos++;
                    break;
                }
                return JFail(r, v->type == 'o' ? "',' or '}' was expected" : "',' or ']' was expected");
            }
        }
    } else if (c == '"') {
        v->type = 's';
        if (!JString(r, &v->str)) return false;
    } else if (strncmp(r->s + r->pos, "true", 4) == 0 || strncmp(r->s + r->pos, "null", 4) == 0) {
        v->type = c == 't' ? 'b' : 'z';
        r->pos += 4;
    } else if (strncmp(r->s + r->pos, "false", 5) == 0) {
        v->type = 'b';
        r->pos += 5;
    } else if (c == '-' || isdigit((unsigned char)c)) {
        v->type = 'n';
        while (r->pos < r->n && strchr("+-.eE0123456789", r->s[r->pos])) r->pos++;
    } else {
        return JFail(r, "an unknown value");
    }
    v->end = r->pos;
    return true;
}

// An object's member by key, or NULL.
static const JValue *JGet(const JValue *o, const char *key)
{
    if (!o || o->type != 'o') return NULL;
    for (size_t i = 0; i < o->members.count; i++) {
        if (strcmp(o->members.items[i].key, key) == 0) return &o->members.items[i].value;
    }
    return NULL;
}

// A string member's text, or NULL.
static const char *JGetStr(const JValue *o, const char *key)
{
    const JValue *v = JGet(o, key);
    return v && v->type == 's' ? v->str : NULL;
}

// Appends s as a JSON string (", \, control characters escaped; UTF-8 passed through).
static void JPutString(Str *out, const char *s)
{
    PUSH(out, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': StrAddC(out, "\\\""); break;
        case '\\': StrAddC(out, "\\\\"); break;
        case '\n': StrAddC(out, "\\n"); break;
        case '\r': StrAddC(out, "\\r"); break;
        case '\t': StrAddC(out, "\\t"); break;
        default:
            if (*p < 0x20) StrAddC(out, Fmt("\\u%04x", *p));
            else PUSH(out, (char)*p);
        }
    }
    PUSH(out, '"');
}

// A word quoted for the shell ('…' with ' written as '\'').
static const char *ShellQuote(const char *s)
{
    Str q = {0};
    PUSH(&q, '\'');
    for (; *s; s++) {
        if (*s == '\'') StrAddC(&q, "'\\''");
        else PUSH(&q, *s);
    }
    PUSH(&q, '\'');
    return StrC(&q);
}

// The path the rewritten command names: <repo>/build/run-sh.o (quoted when it holds characters the shell
// would read).
static const char *WrapperPath(void)
{
    const char *p = Fmt("%s/build/run-sh.o", gToolRoot);
    for (const char *c = p; *c; c++) {
        if (!(isalnum((unsigned char)*c) || strchr("/._-+", *c))) return ShellQuote(p);
    }
    return p;
}

// Is the command already exactly `<this run-sh.o> -c '<text>' [args]`: one segment without writes, its
// first word an absolute path resolving to this program or to <repo>/build/run-sh.o?
static bool AlreadyWrapped(const char *command)
{
    Segments segs = Split(command);
    if (segs.count != 1 || segs.items[0].mark || segs.items[0].writes.count) return false;
    const Strings *w = &segs.items[0].words;
    if (w->count < 3 || strcmp(w->items[1], "-c") != 0) return false;
    const char *first = w->items[0];
    if (first[0] != '/' || strpbrk(first, "$`")) return false;
    const char *real = Adopt(realpath(first, NULL));
    const char *self = Adopt(realpath(gToolSelf, NULL));
    const char *canonical = Adopt(realpath(Fmt("%s/build/run-sh.o", gToolRoot), NULL));
    return real && ((self && strcmp(real, self) == 0) || (canonical && strcmp(real, canonical) == 0));
}

// Prints the hook's deny answer with the reason.
static void HookDeny(const char *reason)
{
    Str out = {0};
    StrAddC(&out, "{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"permissionDecision\":\"deny\","
                  "\"permissionDecisionReason\":");
    JPutString(&out, reason);
    StrAddC(&out, "}}\n");
    fwrite(out.items, 1, out.count, stdout);
}

// Does the hook rewrite commands in this permission mode? Measured in Claude Code 2.1.323: allow rules
// and the read-only auto-approval (ls, git status) are matched against the rewritten command, so in the
// modes that ask (default, acceptEdits, plan) every wrapped command would need approval. There the hook only
// checks (refused: deny; allowed: unchanged, the normal permission flow), unless `--rewrite all`. The modes
// that never ask (bypassPermissions: request sessions and the coordinator; dontAsk; auto, whose classifier
// approves wrapped commands) and an unknown or missing mode are rewritten.
static bool RewriteInMode(const char *mode, bool all)
{
    if (all || !mode) return true;
    return !(strcmp(mode, "default") == 0 || strcmp(mode, "acceptEdits") == 0 || strcmp(mode, "plan") == 0);
}

// run-sh.o --hook [--rewrite all]: the PreToolUse answer for one tool call (header comment, "The hook").
static int Hook(bool rewriteAll)
{
    // the tool call's JSON, all of stdin
    Nob_String_Builder in = {0};
    char buf[65536];
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        nob_sb_append_buf(&in, buf, (size_t)n);
    }
    const char *text = Dup(in.items ? in.items : "", in.count);
    size_t len = in.count;
    nob_sb_free(in);

    // the parts the hook needs
    JReader r = {.s = text, .n = len};
    JValue root = {0};
    if (!JRead(&r, &root, 0) || root.type != 'o') {
        ToolErr("--hook: can't read the tool call's JSON on stdin (%s); the call is blocked. Turn the hook off "
                "in /hooks if this keeps happening.", r.error ? r.error : "not an object");
        FreeAll();
        return 2;
    }
    const char *tool = JGetStr(&root, "tool_name");
    const JValue *input = JGet(&root, "tool_input");
    const char *command = JGetStr(input, "command");
    const char *cwd = JGetStr(&root, "cwd");
    // only Bash and Monitor run shell commands; a Monitor on a WebSocket has no command
    if (!tool || (strcmp(tool, "Bash") != 0 && strcmp(tool, "Monitor") != 0) || !command) {
        FreeAll();
        return 0;
    }

    // the decision: deny, pass unchanged, or rewrite
    const char *refusal = Decide(command, cwd);
    LogDecision(Fmt("--hook %s", tool), command, cwd, refusal);
    if (refusal) {
        HookDeny(refusal);
        FreeAll();
        return 0;
    }
    if (AlreadyWrapped(command) || !RewriteInMode(JGetStr(&root, "permission_mode"), rewriteAll)) {
        FreeAll();
        return 0;
    }

    // the rewrite: tool_input with "command" replaced, every other member copied as it was written
    Str out = {0};
    StrAddC(&out, "{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"updatedInput\":{");
    for (size_t i = 0; i < input->members.count; i++) {
        const JMember *m = &input->members.items[i];
        if (i) PUSH(&out, ',');
        JPutString(&out, m->key);
        PUSH(&out, ':');
        if (strcmp(m->key, "command") == 0) {
            JPutString(&out, Fmt("%s -c %s", WrapperPath(), ShellQuote(command)));
        } else {
            StrAdd(&out, text + m->value.start, m->value.end - m->value.start);
        }
    }
    StrAddC(&out, "}}}\n");
    fwrite(out.items, 1, out.count, stdout);
    fflush(stdout);
    FreeAll();
    return 0;
}

// #endregion

// The usage text, on stderr.
static void Usage(void)
{
    fprintf(stderr,
            "usage: run-sh.o -c '<command line>' [arg0 [args...]]   check, then run it with bash -c\n"
            "       run-sh.o --check '<command line>'             only print the decision\n"
            "       run-sh.o --hook [--rewrite unprompted|all]    the PreToolUse hook (JSON on stdin)\n"
            "The rules, the log (build/run-sh/log) and the limits: the header comment of tools/run-sh.c.\n");
}

// Rebuilds itself when its source changed (ToolInit), then runs the mode its arguments name: -c, --check,
// --hook or --help; anything else is a usage error (exit 2).
int main(int argc, char **argv)
{
    // name, repo root, rebuild when tools/run-sh.c, tools/tool.h or nob.h is newer than this binary
    ToolInit(argv, "run-sh", "run-sh");
    gRootReal = Adopt(realpath(gToolRoot, NULL));
    if (!gRootReal) gRootReal = gToolRoot;
    // (gRootReal lives in the arena, which CheckAndRun / Hook free at their end; nothing reads
    // it after that)

    if (argc >= 3 && strcmp(argv[1], "-c") == 0) return CheckAndRun(true, argc, argv);
    if (argc == 3 && strcmp(argv[1], "--check") == 0) return CheckAndRun(false, argc, argv);
    if (argc == 2 && strcmp(argv[1], "--hook") == 0) return Hook(false);
    if (argc == 4 && strcmp(argv[1], "--hook") == 0 && strcmp(argv[2], "--rewrite") == 0) {
        if (strcmp(argv[3], "all") == 0) return Hook(true);
        if (strcmp(argv[3], "unprompted") == 0) return Hook(false);
    }
    FreeAll();
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        Usage();
        return 0;
    }
    Usage();
    return 2;
}
