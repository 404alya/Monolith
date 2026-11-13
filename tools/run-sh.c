// tools/run-sh.c -> build/run-sh.o: the shell every Claude session's commands run through. It behaves like
// `bash -c` (same stdin, stdout, stderr, exit status, signals and background jobs) but first reads the
// command line and refuses what breaks the project's guard rails (CLAUDE.md), with a message that says why
// and what to use instead. Written on nob.h with tools/tool.h, like every program in tools/.
//
// Modes:
//   run-sh.o -c '<command line>' [arg0 [args...]]
//                  checks the command line; when it passes, replaces itself with `bash -c '<command line>'
//                  [arg0 [args...]]` (always bash, whatever the session's own shell is; a `cd` inside the
//                  command no longer carries over to the next command, since it runs in this child shell).
//                  A refused command prints
//                      run-sh: refused: <rule>: <the word that broke it>; <what to do instead>
//                  on stderr and exits 126 without running anything. A command that runs the game with
//                  --bench takes the benchmark lock first (below).
//   run-sh.o --check '<command line>'
//                  only prints the decision on stdout ("run-sh: allowed" or the refusal line) and exits 0
//                  (allowed) or 126 (refused); runs nothing and takes no lock. For tests and for a session
//                  that wants to know before running.
//   run-sh.o --hook [--rewrite unprompted|all]
//                  the PreToolUse hook (.claude/settings.json): reads the tool call's JSON on stdin and
//                  prints the hook's answer on stdout (see "The hook" below). --rewrite says in which
//                  permission modes allowed commands are rewritten: `unprompted` (the default) only where
//                  Claude Code never asks, `all` in every mode.
//   run-sh.o --help
//                  this summary.
// Every command and its decision (all three modes) is appended to a log, one line each:
// <local time> TAB <mode> TAB <decision> TAB <working directory> TAB <command, with \ \t \n escaped>.
//
// Environment:
//   RUN_SH_DIR   the folder of the log and the benchmark lock (default <repo>/build/run-sh, gitignored with
//                build/): <dir>/log and <dir>/bench.lock. The unit test points it at its own build folder.
//   HOME         `~` and a bare `cd` resolve against it.
//   __GL_SYNC_TO_VBLANK, vblank_mode
//                a benchmark needs vsync off: one of them set to 0 in front of the command, exported earlier
//                in the same command line, or already in run-sh.o's own environment.
//
// Exit codes: -c: the command's own status (bash's), 126 when refused, 127 when bash can't be started;
// --check: 0 allowed, 126 refused; --hook: 0 with the answer on stdout, 2 when the input can't be read
// (the call is then blocked and the reason shown to the model); 2 for a usage error in any mode.
//
// Who calls it: the PreToolUse hook in .claude/settings.json runs `run-sh.o --hook` for every Bash and
// Monitor tool call; the hook rewrites each command to `<repo>/build/run-sh.o -c '<command>'`. Built by
// `./build.sh --tools run-sh` (nob.c, region "Tools"), then it rebuilds itself when this file, tools/tool.h
// or nob.h is newer (ToolInit). tests/unit/run_sh_test.c (`./build.sh test run_sh`) checks every rule.
//
// ---------------------------------------------------------------------------------------------------------
// How it reads a command (region "Lexer", region "Walking the commands"):
// a shell lexer, not a pattern: quotes ('…', "…", $'…' with its escapes), backslashes and line
// continuations, comments, the operators ; && || | |& & and newlines, ( ) subshells and { } groups, the
// keywords (if/then/elif/else/fi, while/until/do/done, for/select … in, case … in … esac, function, [[ ]],
// (( ))), redirections with fd prefixes (2> &> >> <& >& <> >| ...), here-documents (<<, <<-, quoted or not;
// the body of an unquoted one is searched for $( ) and backticks) and here-strings, $( ) and backticks,
// <( ) and >( ), ${ }, $(( )), `VAR=x` prefixes, brace expansion ({a,b}, {1..3}) and ~. Every command word
// is found and checked by its basename (so /bin/rm and \rm are rm), also through the wrappers command,
// builtin, exec, env (with -C and -S), nice, nohup, timeout, time, stdbuf, ionice, sudo, doas, wine, setsid,
// chronic, strace / ltrace, gdb --args, flock, watch (-x), busybox / toybox (the applet), xargs, parallel,
// find -exec / -execdir / -ok / -okdir, and recursively into bash/sh/zsh/dash/ksh -c, run-sh.o -c, eval,
// flock -c, script -c, su -c, watch's command, parallel's command, the literal commands of an awk program
// (system("…"), "…" | getline, print … | "…") and git's -c alias.x='!…' with the string's text (expansions inside it stay unresolved, so a command word
// or path that comes from them is treated as unknown). A shell or interpreter that reads its program from
// a here-document or here-string has that text checked too. The text of the substitutions runs in a
// subshell of the command, so it is checked with the same working directory.
//
// The rules (each enforces a CLAUDE.md line; the developer may add or drop rows):
//   no deletion outside /tmp   (CLAUDE.md "Never delete with rm ... move files to the trash instead with gio
//                              trash"; "Exception: temporary files you created under /tmp may be deleted")
//       refused: rm, rmdir, unlink, shred, gio remove, find -delete, find -exec rm, xargs rm, git worktree
//       remove on any path that doesn't resolve under /tmp/ (the session scratch folders are under
//       /tmp/claude-<uid>/); a path run-sh.o can't resolve from the text ("$X", $(…), a glob outside the
//       last path component or right in /tmp, xargs reading the names from stdin, a relative path after a
//       `cd` that may have failed); a deletion in a command line that also makes a link (ln in any form,
//       cp -s / --symbolic-link: the link would only exist after the path was checked); find -L / -follow
//       with a deletion; git clean (unless -n / --dry-run);
//       git rm without --cached; deletion calls in interpreter one-liners (python -c, perl -e, node -e,
//       ruby -e, php -r: os.remove, shutil.rmtree, unlink, rmdir, rmSync, rm_rf, a word "rm", ...); an awk
//       command that isn't one literal string (system(cmd), cmd | getline, print | cmd); tar --remove-files,
//       zip -m / --move and rsync --remove-source-files on sources, rsync --delete… on a destination,
//       outside /tmp/ (a remote host:path isn't checked); a deletion parallel or xargs gives names to.
//       allowed: gio trash anywhere; the deletion commands on literal paths that resolve under /tmp/.
//   no lost work               (CLAUDE.md "Never delete"; work in the tree is the developer's)
//       refused: git reset --hard, git checkout with -- or . or -f/--force, git restore without --staged
//       (or with --worktree), git stash drop / clear, git switch -f / --force / --discard-changes.
//       allowed: the rest of git.
//   no visible window          (CLAUDE.md "Never open a visible window" and "Never take over the
//                              developer's screen")
//       refused: the game (monolith, monolith.exe, also under wine; `build.sh run`, `build.sh --win run`,
//       `nob run`) without one of --shot, --dump-boxes, --dump-scene, --dump-scene-diff, --dump-entities,
//       --apply-edits, --inspect-model, --new-map, --export, --compile, --classify, --bake-surface,
//       --bench with --headless (this also covers old binaries built by old-monolith, which lack the game's
//       own guard); the plain examples (examples/build/linux/, examples/build/win/); setting or exporting
//       MONOLITH_ALLOW_WINDOW (a `NAME=` prefix or statement, export, declare, typeset, local, readonly,
//       env, or the name in interpreter code).
//       allowed: the examples' test builds (examples/build/linux-test/, linux-lsan/: their harness itself
//       refuses to run without --shot or --bench); MONOLITH_ALLOW_WINDOW as a plain word in an argument
//       (`grep -rn MONOLITH_ALLOW_WINDOW src`).
//   never touch the screen     (CLAUDE.md "Never switch the developer's workspace, desktop or window focus")
//       refused: i3-msg / swaymsg other than `-t get_*` (get_tree, get_workspaces, get_outputs, ...);
//       xdotool; wmctrl.
//   benchmarks                 (CLAUDE.md "only --bench --headless, one benchmark at a time ...
//                              __GL_SYNC_TO_VBLANK=0 and --note on every run")
//       refused: the game's --bench without --headless, without __GL_SYNC_TO_VBLANK=0 (or vblank_mode=0) or
//       without --note "<text>"; an example test build's --bench without vsync off; a second benchmark while
//       one runs: `-c` takes an exclusive flock on <RUN_SH_DIR>/bench.lock through an fd without
//       O_CLOEXEC before it execs bash, so bash and every child keep the lock until the whole run ends
//       (also bench-export-ab). --check doesn't look at the lock.
//   stay in the repo           (CLAUDE.md "Work only inside the repo, plus your own scratch and download
//                              folders")
//       refused: a literal path outside the repo and /tmp as a write redirection (> >> &> >| <> >&file),
//       or as a destination of cp / mv / install (the last operand or -t DIR; every operand of install -d),
//       tee, mkdir, touch, /usr/bin/time -o.
//       allowed: /dev/null, /dev/stdout, /dev/stderr, /dev/tty, /dev/fd/N; any path that isn't literal.
//   readable command
//       refused: a command line the lexer can't read (an unterminated quote or $( ), a redirection without
//       its target) or nesting deeper than 32 levels. bash would refuse most of these too.
//
// How "under /tmp" is decided (Resolve): the path is made absolute against the working directory (the
// command's own, after any `cd` earlier in the same command line; a `cd` that may have failed makes it
// unknown), then walked component by component: an existing component is replaced by its realpath (so
// symlinks are followed and `..` goes to the physical parent), a missing one ends the physical walk and the
// rest is taken lexically. The last component is not followed (rm removes a link, not its target) unless
// the path ends in '/', or for find roots and git worktree remove, which are followed. The resolved path
// must start with "/tmp/" (never /tmp itself). Every path argument of the command must pass, or the whole
// command is refused.
//
// The hook (--hook): reads {"tool_name", "tool_input", "cwd", "permission_mode", ...} on stdin.
//   - Bash or Monitor with a "command": the command is checked with the JSON's "cwd" as the working
//     directory (it follows the session's cd and worktree). Refused: {"hookSpecificOutput":
//     {"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"run-sh: refused:
//     ..."}}: the session sees the reason. Allowed: the answer's "updatedInput" is the whole tool_input with
//     "command" replaced by `<repo>/build/run-sh.o -c '<command>'` (' written as '\''), every other field
//     copied unchanged; no permissionDecision, so the normal permission flow continues on the rewritten
//     command (tested in Claude Code 2.1.323: Claude Code applies a bare updatedInput in every permission
//     mode, also for Monitor, and matches allow rules against the rewritten command). The rewrite happens
//     only in the permission modes that never ask (bypassPermissions, dontAsk, auto; also a missing or unknown
//     mode): in default, acceptEdits and plan an allowed command passes unchanged (empty answer), because a
//     rewritten one would no longer match the developer's allow rules (Bash(git status:*)) or Claude Code's
//     own approval of read-only commands, and every command would ask. `--rewrite all` rewrites in every
//     mode. Refused commands are denied in every mode. A command that already
//     is exactly `<repo>/build/run-sh.o -c '…'` (this program, by its resolved path) is passed unchanged
//     (empty answer); `-c` checks it again anyway.
//   - Monitor with "ws" and no command, and every other tool: passed (empty answer).
//   - Input that isn't the expected JSON: exit 2 with the reason on stderr (blocks the call).
//   <repo> is this program's own repo root: the hook runs $CLAUDE_PROJECT_DIR/build/run-sh.o, which stays
//   the main checkout even in a worktree agent, so worktree agents use the main checkout's run-sh.o.
//
// Known limits (the rules guard against mistakes, they are not a sandbox):
//   - A command word or path that only exists after expansion ("$X" file, $(cat list), `$RM x`) can't be
//     checked from the text: a deletion with such a path is refused, but an unknown command word is let
//     through.
//   - A script file the command runs (bash x.sh, python3 x.py, source x, make) is not read, nor commands a
//     shell reads from a pipe (`... | bash`, awk's print | "sh"); a compiled program can delete or open
//     whatever it wants.
//   - Aliases and functions defined earlier in the same command line are not followed.
//   - Deletions in two steps aren't seen: `mv <repo file> /tmp/` (or a link made in one command) and an rm
//     there in a later command delete repo files, and each command alone passes. In one line too:
//     `mv README.md /tmp/x && rm /tmp/x` passes (the rm's path is under /tmp when it is checked).
//   - Destroying a file's content isn't a rule: `> file`, truncate, `cp /dev/null file`, `dd of=file`,
//     `sed -i`, `ln -sf` over a file, chmod.
//   - A game binary copied elsewhere and run by another name (cp build/linux/monolith /tmp/g && /tmp/g) is
//     not recognised as the game; windows of other programs (xdg-open, image viewers) aren't refused.
//   - In the hook, a command that already starts with a relative `build/run-sh.o -c` is wrapped a second
//     time (only an absolute path is recognised); harmless, it is checked twice.
//   - Interpreter one-liners are searched for deletion calls by name only (a false positive is possible,
//     e.g. list.remove(x)); other things they do (open windows, run git) are not seen.
//   - A glob in the last path component relies on bash >= 5.2 (globskipdots: `.*` never matches ..).
//   - In the permission modes that ask (default, acceptEdits, plan) the hook only checks (unless --rewrite
//     all): allowed commands run in the session's own shell, not through -c, so they take no benchmark
//     lock, a cd carries over, and they are logged once (as --hook) instead of twice.
//   - When tools/run-sh.c changed but doesn't compile, every call tries the compile again (a second or
//     two per shell command) and then runs the old build with a warning, until the source is fixed.
//   - The log grows without limit (build/run-sh/log; move it to the trash when it gets big).

#include "tool.h"

#include <ctype.h>
#include <fnmatch.h>
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
#define RULE_READ   "readable command"

// What to use instead, per refusal (the text after the ';').
#define HINT_TRASH "move it to the trash with `gio trash <path>` (rm and the other deletion commands only " \
                   "take literal paths under /tmp/, e.g. your scratchpad)"
#define HINT_UNSEEN "run-sh.o can't see this path in the command's text (a variable, $( ), a glob, names from " \
                    "stdin, or a cd that may have failed); write each path out literally under /tmp/, or use " \
                    "`gio trash <path>`"
#define HINT_GAME "run the game only with --shot <file>, --dump-boxes, --dump-scene, --dump-scene-diff, " \
                  "--dump-entities, --apply-edits, --inspect-model, --new-map, --export, --compile, " \
                  "--classify, --bake-surface or --bench --headless (CLAUDE.md)"
#define HINT_STAY "write only inside the repo or under /tmp/ (your scratchpad)"

// The first refusal found ("run-sh: refused: <rule>: <word>; <hint>"), or NULL while the command passes.
static const char *gRefusal;
// True when the command runs a benchmark (the game's --bench, an example's --bench, bench-export-ab): -c
// takes the benchmark lock.
static bool gBench;
// A command in the line makes a link (ln in any form, cp -s), and the first deletion path seen (for the
// message): both together are refused after the walk (Decide), because the link may only exist when the
// deletion runs, after its path was resolved from the text.
static const char *gMakesLink;
static const char *gFirstDeletion;

// Records a refusal (only the first one counts: it is the one the message names).
static void Refuse(const char *rule, const char *word, const char *hint)
{
    if (gRefusal) return;
    gRefusal = Fmt("run-sh: refused: %s: %s; %s", rule, word, hint);
}

// #endregion

// #region Lexer

// What made each byte of a word's text: an unquoted character (globs and braces are live), a quoted one,
// or the source text of an expansion ($X, ${…}, $( ), `…`, $(( ))) kept as written, which the checks treat
// as unknown.
enum { K_LIT = 0, K_QUOTED = 1, K_EXP = 2 };

// One word as the lexer read it: quotes removed, expansions kept as their source text (so a nested
// `bash -c "rm $X"` is read again with $X still an expansion), plus the command lines of every
// substitution in it ($( ), backticks, <( ), >( )), which the walk checks as commands of their own.
typedef struct {
    Str text;
    struct {
        unsigned char *items;
        size_t count, capacity;
    } kind;                // one entry per byte of text: K_LIT, K_QUOTED or K_EXP
    bool quoted;           // any quoting at all (a quoted word is never a keyword)
    bool arith;            // a (( … )) arithmetic command
    Strings subs;          // the command lines of its substitutions
} Word;

// One token: a word, an operator (; && || | |& & newline ( ) ;; ;& ;;&) or a redirection with its target.
enum { T_WORD, T_OP, T_REDIR };

typedef struct {
    int type;
    const char *op;        // T_OP, T_REDIR: the operator's text
    Word word;             // T_WORD: the word; T_REDIR: the target (a here-document's subs go here too)
    bool fdTarget;         // T_REDIR: the target is a file descriptor (>&2, <&-), not a path
    bool heredoc;          // T_REDIR << or <<-: body is the here-document's text
    bool hdQuoted;         // the delimiter was quoted: the body is taken literally
    const char *body;
} Token;

typedef struct {
    Token *items;
    size_t count, capacity;
} Tokens;

// The lexer's position in one command line. `pending` holds the here-documents (token indices) whose
// body starts after the next newline.
typedef struct {
    const char *s;
    size_t n, pos;
    const char *error;
    struct {
        size_t *items;
        size_t count, capacity;
    } pending;
} Lexer;

// (The lexer's functions call each other: $( ) holds a whole command line, "…" holds $( ).)
static bool LexList(Lexer *L, Tokens *out, bool untilParen);
static bool LexDollar(Lexer *L, Word *w, bool dq);
static bool LexBacktick(Lexer *L, Word *w, bool dq);
static bool LexDouble(Lexer *L, Word *w);

// Adds one byte to a word with its kind.
static void WAdd(Word *w, char c, unsigned char k)
{
    PUSH(&w->text, c);
    PUSH(&w->kind, k);
}

// Adds n bytes of one kind.
static void WAddN(Word *w, const char *p, size_t n, unsigned char k)
{
    for (size_t i = 0; i < n; i++) WAdd(w, p[i], k);
}

// Moves the substitutions found in a scratch word into w (used where the scratch word's text isn't kept).
static void TakeSubs(Word *w, const Word *from)
{
    for (size_t i = 0; i < from->subs.count; i++) PUSH(&w->subs, from->subs.items[i]);
}

// The character `ahead` places after the lexer's position, or '\0' past the end.
static char Peek(const Lexer *L, size_t ahead)
{
    return L->pos + ahead < L->n ? L->s[L->pos + ahead] : '\0';
}

// The characters that end an unquoted word.
static bool IsMeta(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' || c == '|' || c == '<' || c == '>' ||
           c == '(' || c == ')';
}

// Sets the first error and returns false.
static bool LexFail(Lexer *L, const char *what)
{
    if (!L->error) L->error = what;
    return false;
}

// Appends the UTF-8 encoding of a code point (for $'\u…').
static void AddUtf8(Word *w, unsigned long c)
{
    if (c < 0x80) {
        WAdd(w, (char)c, K_QUOTED);
    } else if (c < 0x800) {
        WAdd(w, (char)(0xC0 | (c >> 6)), K_QUOTED);
        WAdd(w, (char)(0x80 | (c & 0x3F)), K_QUOTED);
    } else if (c < 0x10000) {
        WAdd(w, (char)(0xE0 | (c >> 12)), K_QUOTED);
        WAdd(w, (char)(0x80 | ((c >> 6) & 0x3F)), K_QUOTED);
        WAdd(w, (char)(0x80 | (c & 0x3F)), K_QUOTED);
    } else {
        WAdd(w, (char)(0xF0 | ((c >> 18) & 0x07)), K_QUOTED);
        WAdd(w, (char)(0x80 | ((c >> 12) & 0x3F)), K_QUOTED);
        WAdd(w, (char)(0x80 | ((c >> 6) & 0x3F)), K_QUOTED);
        WAdd(w, (char)(0x80 | (c & 0x3F)), K_QUOTED);
    }
}

// Reads up to `max` digits of `base` at the lexer's position; returns how many were read.
static int ReadDigits(Lexer *L, int base, int max, unsigned long *value)
{
    int n = 0;
    *value = 0;
    while (n < max && L->pos < L->n) {
        char c = L->s[L->pos];
        int d = isdigit((unsigned char)c) ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10
              : 99;
        if (d >= base) break;
        *value = *value * (unsigned long)base + (unsigned long)d;
        L->pos++;
        n++;
    }
    return n;
}

// $'…' (ANSI-C quoting): decodes bash's escapes, so $'\x72\x6d' is read as "rm". The text is quoted (no
// globs). As in bash, a NUL ends the string's value.
static bool LexAnsiC(Lexer *L, Word *w)
{
    L->pos += 2;  // $'
    w->quoted = true;
    bool cut = false;
    for (;;) {
        if (L->pos >= L->n) return LexFail(L, "unterminated $'");
        char c = L->s[L->pos++];
        if (c == '\'') return true;
        unsigned long v = (unsigned char)c;
        if (c == '\\' && L->pos < L->n) {
            char e = L->s[L->pos++];
            switch (e) {
            case 'a': v = 7; break;
            case 'b': v = 8; break;
            case 'e': case 'E': v = 27; break;
            case 'f': v = 12; break;
            case 'n': v = 10; break;
            case 'r': v = 13; break;
            case 't': v = 9; break;
            case 'v': v = 11; break;
            case '\\': case '\'': case '"': case '?': v = (unsigned char)e; break;
            case 'x':
                if (ReadDigits(L, 16, 2, &v) == 0) v = 0x100 + 'x';  // no digits: kept as written
                break;
            case 'u':
                if (ReadDigits(L, 16, 4, &v) == 0) v = 0x100 + 'u';
                else v |= 0x80000000UL;  // a code point, not a byte (below)
                break;
            case 'U':
                if (ReadDigits(L, 16, 8, &v) == 0) v = 0x100 + 'U';
                else v |= 0x80000000UL;
                break;
            case 'c':
                v = L->pos < L->n ? (unsigned long)(L->s[L->pos++] & 0x1F) : 0x100 + 'c';
                break;
            default:
                if (e >= '0' && e <= '7') {
                    L->pos--;
                    ReadDigits(L, 8, 3, &v);
                    v &= 0xFF;
                } else {
                    v = 0x100 + (unsigned char)e;  // unknown escape: backslash and character as written
                }
            }
        }
        if (cut) continue;
        if (v == 0) {
            cut = true;
        } else if (v & 0x80000000UL) {
            AddUtf8(w, v & 0x7FFFFFFFUL);
        } else if (v >= 0x100) {
            WAdd(w, '\\', K_QUOTED);
            WAdd(w, (char)(v - 0x100), K_QUOTED);
        } else {
            WAdd(w, (char)v, K_QUOTED);
        }
    }
}

// $( … ): the command line inside is read by a nested lexer up to its closing ')' (so quotes, case
// patterns and nested substitutions are handled), kept as a substitution, and its source text added to
// the word as an expansion.
static bool LexCommandSubst(Lexer *L, Word *w, size_t skip)
{
    size_t start = L->pos;
    Lexer sub = {.s = L->s, .n = L->n, .pos = L->pos + skip};
    Tokens tmp = {0};
    if (!LexList(&sub, &tmp, true)) return LexFail(L, sub.error);
    PUSH(&w->subs, Dup(L->s + start + skip, sub.pos - 1 - (start + skip)));
    WAddN(w, L->s + start, sub.pos - start, K_EXP);
    L->pos = sub.pos;
    return true;
}

// $(( … )) or a (( … )) command: arithmetic, read up to the matching "))", with any $( ) or backticks
// inside kept as substitutions. `skip` is the length of the opening ("$((" or "((").
static bool LexArith(Lexer *L, Word *w, size_t skip)
{
    size_t start = L->pos;
    L->pos += skip;
    Word scratch = {0};
    int depth = 0;
    for (;;) {
        if (L->pos >= L->n) return LexFail(L, "unterminated (( ))");
        char c = L->s[L->pos];
        if (c == '\\') {
            L->pos += 2;
        } else if (c == '\'') {
            const char *e = memchr(L->s + L->pos + 1, '\'', L->n - L->pos - 1);
            if (!e) return LexFail(L, "unterminated '");
            L->pos = (size_t)(e - L->s) + 1;
        } else if (c == '"') {
            if (!LexDouble(L, &scratch)) return false;
        } else if (c == '$') {
            if (!LexDollar(L, &scratch, true)) return false;
        } else if (c == '`') {
            if (!LexBacktick(L, &scratch, true)) return false;
        } else if (c == '(') {
            depth++;
            L->pos++;
        } else if (c == ')') {
            if (depth > 0) {
                depth--;
                L->pos++;
            } else if (Peek(L, 1) == ')') {
                L->pos += 2;
                break;
            } else {
                return LexFail(L, "unreadable (( ))");
            }
        } else {
            L->pos++;
        }
    }
    TakeSubs(w, &scratch);
    WAddN(w, L->s + start, L->pos - start, K_EXP);
    return true;
}

// ${ … }: a parameter expansion, read to its matching '}' (nested ${ }, $( ), quotes and backticks
// inside are followed, their substitutions kept).
static bool LexParam(Lexer *L, Word *w)
{
    size_t start = L->pos;
    L->pos += 2;
    Word scratch = {0};
    for (;;) {
        if (L->pos >= L->n) return LexFail(L, "unterminated ${");
        char c = L->s[L->pos];
        if (c == '}') {
            L->pos++;
            break;
        } else if (c == '\\') {
            L->pos += 2;
        } else if (c == '\'') {
            const char *e = memchr(L->s + L->pos + 1, '\'', L->n - L->pos - 1);
            if (!e) return LexFail(L, "unterminated '");
            L->pos = (size_t)(e - L->s) + 1;
        } else if (c == '"') {
            if (!LexDouble(L, &scratch)) return false;
        } else if (c == '$') {
            if (!LexDollar(L, &scratch, true)) return false;
        } else if (c == '`') {
            if (!LexBacktick(L, &scratch, true)) return false;
        } else {
            L->pos++;
        }
    }
    if (L->pos > L->n) L->pos = L->n;
    TakeSubs(w, &scratch);
    WAddN(w, L->s + start, L->pos - start, K_EXP);
    return true;
}

// A '$' at the lexer's position: $'…', $"…", $(( )), $( ), ${ }, $name, $1 / $@ / $? …, or a plain '$'.
// `dq`: inside double quotes (no $'…' there, and a plain '$' is quoted).
static bool LexDollar(Lexer *L, Word *w, bool dq)
{
    char d = Peek(L, 1);
    if (!dq && d == '\'') return LexAnsiC(L, w);
    if (!dq && d == '"') {
        L->pos++;
        return LexDouble(L, w);
    }
    if (d == '(' && Peek(L, 2) == '(') return LexArith(L, w, 3);
    if (d == '(') return LexCommandSubst(L, w, 2);
    if (d == '{') return LexParam(L, w);
    if (isalpha((unsigned char)d) || d == '_') {
        size_t start = L->pos++;
        while (L->pos < L->n && (isalnum((unsigned char)L->s[L->pos]) || L->s[L->pos] == '_')) L->pos++;
        WAddN(w, L->s + start, L->pos - start, K_EXP);
        return true;
    }
    if (d != '\0' && (isdigit((unsigned char)d) || strchr("@*#?$!-", d))) {
        WAddN(w, L->s + L->pos, 2, K_EXP);
        L->pos += 2;
        return true;
    }
    WAdd(w, '$', dq ? K_QUOTED : K_LIT);
    L->pos++;
    return true;
}

// `…`: the old command substitution. Its text is unescaped (\$ \` \\, and \" inside double quotes) and
// kept as a substitution; the source text goes into the word as an expansion.
static bool LexBacktick(Lexer *L, Word *w, bool dq)
{
    size_t start = L->pos++;
    Str inner = {0};
    for (;;) {
        if (L->pos >= L->n) return LexFail(L, "unterminated `");
        char c = L->s[L->pos];
        if (c == '`') {
            L->pos++;
            break;
        }
        char e = Peek(L, 1);
        if (c == '\\' && (e == '$' || e == '`' || e == '\\' || (dq && e == '"'))) {
            PUSH(&inner, e);
            L->pos += 2;
            continue;
        }
        PUSH(&inner, c);
        L->pos++;
    }
    PUSH(&w->subs, StrC(&inner));
    WAddN(w, L->s + start, L->pos - start, K_EXP);
    return true;
}

// "…": quoted text; \ escapes only $ ` " \ and newline; $ and backticks still expand.
static bool LexDouble(Lexer *L, Word *w)
{
    L->pos++;
    w->quoted = true;
    for (;;) {
        if (L->pos >= L->n) return LexFail(L, "unterminated \"");
        char c = L->s[L->pos];
        if (c == '"') {
            L->pos++;
            return true;
        }
        if (c == '\\') {
            char e = Peek(L, 1);
            if (e == '\n') {
                L->pos += 2;
            } else if (e == '$' || e == '`' || e == '"' || e == '\\') {
                WAdd(w, e, K_QUOTED);
                L->pos += 2;
            } else {
                WAdd(w, '\\', K_QUOTED);
                L->pos++;
            }
        } else if (c == '$') {
            if (!LexDollar(L, w, true)) return false;
        } else if (c == '`') {
            if (!LexBacktick(L, w, true)) return false;
        } else {
            WAdd(w, c, K_QUOTED);
            L->pos++;
        }
    }
}

// One word at the lexer's position, up to an unquoted blank or operator character. `name=(…)` (an array
// assignment) takes its parenthesised list into the word.
static bool LexWord(Lexer *L, Word *w)
{
    size_t start = L->pos;
    while (L->pos < L->n) {
        char c = L->s[L->pos];
        if (c == '(' && w->text.count > 0 && w->text.items[w->text.count - 1] == '=' &&
            w->kind.items[w->kind.count - 1] == K_LIT) {
            // an array assignment: the elements' substitutions are kept, the whole is an expansion
            size_t open = L->pos;
            Lexer sub = {.s = L->s, .n = L->n, .pos = L->pos + 1};
            Tokens tmp = {0};
            if (!LexList(&sub, &tmp, true)) return LexFail(L, sub.error);
            for (size_t i = 0; i < tmp.count; i++) TakeSubs(w, &tmp.items[i].word);
            WAddN(w, L->s + open, sub.pos - open, K_EXP);
            L->pos = sub.pos;
            continue;
        }
        if (IsMeta(c)) break;
        if (c == '\\') {
            w->quoted = true;
            if (L->pos + 1 >= L->n) {
                WAdd(w, '\\', K_QUOTED);
                L->pos++;
            } else {
                char e = L->s[L->pos + 1];
                L->pos += 2;
                if (e != '\n') WAdd(w, e, K_QUOTED);
            }
        } else if (c == '\'') {
            const char *e = memchr(L->s + L->pos + 1, '\'', L->n - L->pos - 1);
            if (!e) return LexFail(L, "unterminated '");
            WAddN(w, L->s + L->pos + 1, (size_t)(e - (L->s + L->pos + 1)), K_QUOTED);
            L->pos = (size_t)(e - L->s) + 1;
            w->quoted = true;
        } else if (c == '"') {
            if (!LexDouble(L, w)) return false;
        } else if (c == '$') {
            if (!LexDollar(L, w, false)) return false;
        } else if (c == '`') {
            if (!LexBacktick(L, w, false)) return false;
        } else if (c == '~' && L->pos == start && (IsMeta(Peek(L, 1)) || Peek(L, 1) == '/' || !Peek(L, 1))) {
            // ~ and ~/…: $HOME (an unknown expansion when HOME isn't set)
            const char *home = getenv("HOME");
            if (home && home[0]) WAddN(w, home, strlen(home), K_QUOTED);
            else WAdd(w, '~', K_EXP);
            L->pos++;
        } else {
            WAdd(w, c, K_LIT);
            L->pos++;
        }
    }
    return true;
}

// Reads the bodies of the here-documents waiting for this newline, in order. An unquoted delimiter means
// the body expands: its $( ) and backticks are kept as substitutions of the redirection's target word.
static bool LexHeredocBodies(Lexer *L, Tokens *out)
{
    for (size_t k = 0; k < L->pending.count; k++) {
        Token *t = &out->items[L->pending.items[k]];
        const char *delim = StrC(&t->word.text);
        size_t dlen = strlen(delim);
        bool strip = strcmp(t->op, "<<-") == 0;
        Str body = {0};
        while (L->pos < L->n) {
            size_t e = L->pos;
            while (e < L->n && L->s[e] != '\n') e++;
            const char *line = L->s + L->pos;
            size_t len = e - L->pos, skip = 0;
            while (strip && skip < len && line[skip] == '\t') skip++;
            L->pos = e < L->n ? e + 1 : e;
            if (len - skip == dlen && memcmp(line + skip, delim, dlen) == 0) break;
            StrAdd(&body, line + skip, len - skip);
            PUSH(&body, '\n');
        }
        t->body = StrC(&body);
        if (t->hdQuoted) continue;
        // the expansions of an unquoted here-document: read like the inside of double quotes
        Lexer B = {.s = t->body, .n = body.count};
        Word scratch = {0};
        while (B.pos < B.n) {
            char c = B.s[B.pos];
            if (c == '\\') {
                B.pos += 2;
            } else if (c == '$') {
                if (!LexDollar(&B, &scratch, true)) return LexFail(L, B.error);
            } else if (c == '`') {
                if (!LexBacktick(&B, &scratch, true)) return LexFail(L, B.error);
            } else {
                B.pos++;
            }
        }
        TakeSubs(&t->word, &scratch);
    }
    L->pending.count = 0;
    return true;
}

// A redirection operator at the lexer's position (after an optional fd prefix, already read) and its
// target word. `op` is the operator's text, `len` its length in the input.
static bool LexRedir(Lexer *L, Tokens *out, const char *op, size_t len)
{
    L->pos += len;
    while (L->pos < L->n && (L->s[L->pos] == ' ' || L->s[L->pos] == '\t')) L->pos++;
    if (L->pos >= L->n || IsMeta(L->s[L->pos])) return LexFail(L, "a redirection without its target");
    Token t = {.type = T_REDIR, .op = op};
    if (!LexWord(L, &t.word)) return false;
    const char *target = StrC(&t.word.text);
    if (strcmp(op, ">&") == 0 || strcmp(op, "<&") == 0) {
        // >&2, <&0, >&- and 2>&1-: a descriptor; anything else after >& is a file (as &>)
        size_t i = 0;
        while (isdigit((unsigned char)target[i])) i++;
        t.fdTarget = (i > 0 && (target[i] == '\0' || (target[i] == '-' && target[i + 1] == '\0'))) ||
                     strcmp(target, "-") == 0;
    }
    if (strcmp(op, "<<") == 0 || strcmp(op, "<<-") == 0) {
        t.heredoc = true;
        t.hdQuoted = t.word.quoted;
        PUSH(&L->pending, out->count);  // the index this token gets below
    }
    PUSH(out, t);
    return true;
}

// Is this word made only of unquoted digits (an fd prefix like the 2 of 2>), or {name} (bash's
// {var}>file)?
static bool IsFdPrefix(const Word *w)
{
    if (w->text.count == 0 || w->quoted || w->subs.count > 0) return false;
    bool digits = true;
    for (size_t i = 0; i < w->text.count; i++) digits = digits && isdigit((unsigned char)w->text.items[i]);
    if (digits) return true;
    return w->text.count > 2 && w->text.items[0] == '{' && w->text.items[w->text.count - 1] == '}';
}

// Appends an operator token.
static void PushOp(Tokens *out, const char *op)
{
    Token t = {.type = T_OP, .op = op};
    PUSH(out, t);
}

// Reads tokens up to the end of the text, or (untilParen) up to the ')' that closes a $( ) / <( ) / array
// list; L->pos is then just past it. A ')' that ends a case pattern inside doesn't close it (a "case" word
// seen and its "esac" not yet).
static bool LexList(Lexer *L, Tokens *out, bool untilParen)
{
    int parenDepth = 0, caseDepth = 0;
    for (;;) {
        // blanks and line continuations between tokens
        while (L->pos < L->n) {
            char c = L->s[L->pos];
            if (c == ' ' || c == '\t') L->pos++;
            else if (c == '\\' && Peek(L, 1) == '\n') L->pos += 2;
            else break;
        }
        if (L->pos >= L->n) {
            if (untilParen) return LexFail(L, "unterminated $( (or <( / >( )");
            return LexHeredocBodies(L, out);  // here-documents without a body end at the end of the text
        }
        char c = L->s[L->pos], d = Peek(L, 1), e = Peek(L, 2);

        // a comment, up to the newline
        if (c == '#') {
            while (L->pos < L->n && L->s[L->pos] != '\n') L->pos++;
            continue;
        }
        // newline: an operator, then the bodies of the here-documents opened on this line
        if (c == '\n') {
            L->pos++;
            PushOp(out, "\n");
            if (!LexHeredocBodies(L, out)) return false;
            continue;
        }
        // ) : the end of the substitution, or a subshell's / case pattern's ')'
        if (c == ')') {
            if (untilParen && parenDepth == 0 && caseDepth == 0) {
                L->pos++;
                return true;
            }
            if (parenDepth > 0) parenDepth--;
            L->pos++;
            PushOp(out, ")");
            continue;
        }
        // (( … )): an arithmetic command (also `for (( … ))`)
        if (c == '(' && d == '(') {
            Token t = {.type = T_WORD};
            t.word.arith = true;
            if (!LexArith(L, &t.word, 2)) return false;
            PUSH(out, t);
            continue;
        }
        if (c == '(') {
            parenDepth++;
            L->pos++;
            PushOp(out, "(");
            continue;
        }
        // ; ;; ;& ;;&
        if (c == ';') {
            const char *op = d == ';' ? (e == '&' ? ";;&" : ";;") : d == '&' ? ";&" : ";";
            L->pos += strlen(op);
            PushOp(out, op);
            continue;
        }
        // && &>> &> &
        if (c == '&') {
            if (d == '&') {
                L->pos += 2;
                PushOp(out, "&&");
            } else if (d == '>') {
                if (!LexRedir(L, out, e == '>' ? "&>>" : "&>", e == '>' ? 3 : 2)) return false;
            } else {
                L->pos++;
                PushOp(out, "&");
            }
            continue;
        }
        // || |& |
        if (c == '|') {
            const char *op = d == '|' ? "||" : d == '&' ? "|&" : "|";
            L->pos += strlen(op);
            PushOp(out, op);
            continue;
        }
        // <( … ) and >( … ): process substitution, a word whose value is unknown
        if ((c == '<' || c == '>') && d == '(') {
            Token t = {.type = T_WORD};
            if (!LexCommandSubst(L, &t.word, 2)) return false;
            PUSH(out, t);
            continue;
        }
        // redirections
        if (c == '<' || c == '>') {
            static const char *ops[] = {"<<<", "<<-", "<<", "<>", "<&", "<", ">>", ">&", ">|", ">"};
            for (size_t i = 0; i < NOB_ARRAY_LEN(ops); i++) {
                size_t n = strlen(ops[i]);
                if (L->pos + n <= L->n && memcmp(L->s + L->pos, ops[i], n) == 0) {
                    if (!LexRedir(L, out, ops[i], n)) return false;
                    break;
                }
            }
            continue;
        }

        // a word; digits or {name} right before < or > are the redirection's fd
        Token t = {.type = T_WORD};
        size_t before = L->pos;
        if (!LexWord(L, &t.word)) return false;
        if (L->pos == before) return LexFail(L, "unreadable character");
        char next = L->pos < L->n ? L->s[L->pos] : '\0';
        if ((next == '<' || next == '>') && Peek(L, 1) != '(' && IsFdPrefix(&t.word)) continue;
        if (!t.word.quoted) {
            const char *text = StrC(&t.word.text);
            if (strcmp(text, "case") == 0) caseDepth++;
            else if (strcmp(text, "esac") == 0 && caseDepth > 0) caseDepth--;
        }
        PUSH(out, t);
    }
}

// #endregion

// #region Words after expansion (Arg)

// A word as a command sees it after brace expansion: its text (expansions as source text), the kind of
// each byte, whether it is fully known (no expansion) and whether it is a glob.
typedef struct {
    const char *text;
    const unsigned char *kind;
    size_t len;
    bool literal;
    bool glob;
} Arg;

typedef struct {
    Arg *items;
    size_t count, capacity;
} Args;

// An Arg from a word's bytes and kinds (copied into the arena), with `literal` and `glob` worked out.
static Arg MakeArg(const char *text, const unsigned char *kind, size_t len)
{
    Arg a = {.text = Dup(text, len), .len = len, .literal = true};
    unsigned char *k = Alloc(len + 1);
    memcpy(k, kind, len);
    a.kind = k;
    for (size_t i = 0; i < len; i++) {
        if (kind[i] == K_EXP) a.literal = false;
        if (kind[i] == K_LIT && (text[i] == '*' || text[i] == '?' || text[i] == '[')) a.glob = true;
    }
    return a;
}

// An Arg from plain text (the parts of a find -exec, a cd target): every byte quoted.
static Arg TextArg(const char *text)
{
    size_t n = strlen(text);
    unsigned char *k = Alloc(n + 1);
    memset(k, K_QUOTED, n);
    return MakeArg(text, k, n);
}

// Is the argument exactly this known text?
static bool Eq(const Arg *a, const char *s)
{
    return a->literal && strcmp(a->text, s) == 0;
}

// prefix [0, from) + middle + suffix [to, len) of a word, text and kinds.
static void Splice(const char *t, const unsigned char *k, size_t len, size_t from, size_t to, const char *mt,
                   const unsigned char *mk, size_t mlen, char **ot, unsigned char **ok, size_t *olen)
{
    size_t n = from + mlen + (len - to);
    char *nt = Alloc(n + 1);
    unsigned char *nk = Alloc(n + 1);
    memcpy(nt, t, from);
    memcpy(nk, k, from);
    memcpy(nt + from, mt, mlen);
    memcpy(nk + from, mk, mlen);
    memcpy(nt + from + mlen, t + to, len - to);
    memcpy(nk + from + mlen, k + to, len - to);
    *ot = nt;
    *ok = nk;
    *olen = n;
}

// Brace expansion state: the words made, and how many more may be made before the rest is "unknown"
// (a guard against {1..99999999}).
typedef struct {
    Args *out;
    size_t budget;
    bool overflow;
} Braces;

// (BraceSequence and BraceExpand call each other.)
static void BraceExpand(Braces *B, const char *t, const unsigned char *k, size_t len);

// {x..y} or {x..y..step} with integers or single letters, expanded into B; false when the inside isn't
// such a sequence (then the braces are literal).
static bool BraceSequence(Braces *B, const char *t, const unsigned char *k, size_t len, size_t open,
                          size_t close)
{
    char *inner = Dup(t + open + 1, close - open - 1);
    for (size_t i = open + 1; i < close; i++) {
        if (k[i] != K_LIT) return false;
    }
    long a, b, step = 1;
    char ca = 0, cb = 0;
    int used = -1;
    size_t il = strlen(inner);
    if (sscanf(inner, "%ld..%ld..%ld%n", &a, &b, &step, &used) == 3 && (size_t)used == il) {
        // {x..y..step}
    } else if ((used = -1, sscanf(inner, "%ld..%ld%n", &a, &b, &used) == 2) && (size_t)used == il) {
        step = 1;  // {x..y}
    } else if (il == 4 && isalpha((unsigned char)inner[0]) && inner[1] == '.' && inner[2] == '.' &&
               isalpha((unsigned char)inner[3])) {
        ca = inner[0];
        cb = inner[3];
        a = ca;
        b = cb;
    } else {
        return false;
    }
    if (step == 0) step = 1;
    if (step < 0) step = -step;
    for (long v = a;; v += a <= b ? step : -step) {
        if ((a <= b && v > b) || (a > b && v < b)) break;
        if (B->budget == 0) {
            B->overflow = true;
            return true;
        }
        const char *s = ca ? Fmt("%c", (char)v) : Fmt("%ld", v);
        size_t sl = strlen(s);
        unsigned char *sk = Alloc(sl + 1);
        memset(sk, K_QUOTED, sl);
        char *nt;
        unsigned char *nk;
        size_t nl;
        Splice(t, k, len, open, close + 1, s, sk, sl, &nt, &nk, &nl);
        BraceExpand(B, nt, nk, nl);
    }
    return true;
}

// Bash's brace expansion of one word ({a,b}c -> ac bc, nested braces, sequences) on its unquoted braces;
// the results are added to B->out in bash's order.
static void BraceExpand(Braces *B, const char *t, const unsigned char *k, size_t len)
{
    for (size_t open = 0; open < len; open++) {
        if (t[open] != '{' || k[open] != K_LIT) continue;
        // the matching '}' and whether a top-level ',' is inside
        int depth = 0;
        size_t close = 0;
        bool comma = false, found = false;
        for (size_t i = open; i < len && !found; i++) {
            if (k[i] != K_LIT) continue;
            if (t[i] == '{') depth++;
            else if (t[i] == '}' && --depth == 0) close = i, found = true;
            else if (t[i] == ',' && depth == 1) comma = true;
        }
        if (!found) break;
        if (comma) {
            // one word per alternative: prefix + alternative + suffix, each expanded again
            depth = 0;
            size_t partStart = open + 1;
            for (size_t i = open + 1; i <= close; i++) {
                bool lit = k[i] == K_LIT;
                if (lit && t[i] == '{') {
                    depth++;
                } else if (lit && t[i] == '}' && i != close) {
                    depth--;
                } else if (i == close || (lit && t[i] == ',' && depth == 0)) {
                    char *nt;
                    unsigned char *nk;
                    size_t nl;
                    Splice(t, k, len, open, close + 1, t + partStart, k + partStart, i - partStart, &nt, &nk,
                           &nl);
                    BraceExpand(B, nt, nk, nl);
                    partStart = i + 1;
                }
            }
            return;
        }
        if (BraceSequence(B, t, k, len, open, close)) return;
    }
    if (B->budget == 0) {
        B->overflow = true;
        return;
    }
    B->budget--;
    PUSH(B->out, MakeArg(t, k, len));
}

// The command's words after brace expansion. Too many results: an unknown word stands for the rest.
static Args ExpandWords(Word *words, size_t count)
{
    Args out = {0};
    Braces B = {.out = &out, .budget = 4096};
    for (size_t i = 0; i < count; i++) {
        StrC(&words[i].text);
        PUSH(&words[i].kind, (unsigned char)0);
        words[i].kind.count--;
        BraceExpand(&B, words[i].text.items, words[i].kind.items, words[i].text.count);
    }
    if (B.overflow) {
        unsigned char k[] = {K_EXP, K_EXP, K_EXP};
        PUSH(&out, MakeArg("{…}", k, 3));
    }
    return out;
}

// #endregion

// #region Paths

// The repo root as named (gToolRoot) and as resolved: "stay in the repo" accepts either.
static const char *gRootReal;

// The last path component (basename without trailing slashes kept as the text is).
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

// Is there a folder at this path (links followed)?
static bool IsDir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
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
    bool trailing = n > 1 && abs[n - 1] == '/';
    if (trailing) followLast = true;

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
        bool last = i + 1 == parts.count;
        if (strcmp(c, ".") == 0) continue;
        if (strcmp(c, "..") == 0) {
            while (cur.count > 0 && cur.items[cur.count - 1] != '/') cur.count--;
            if (cur.count > 0) cur.count--;
            continue;
        }
        PUSH(&cur, '/');
        StrAddC(&cur, c);
        if (missing || (last && !followLast)) continue;
        struct stat st;
        if (lstat(StrC(&cur), &st) != 0) {
            missing = true;
            continue;
        }
        char *real = Adopt(realpath(StrC(&cur), NULL));
        if (!real) {
            missing = true;  // a dangling link: the walk ends here, the rest is lexical
            continue;
        }
        // the real path replaces everything walked so far (it is absolute)
        cur.count = 0;
        if (strcmp(real, "/") != 0) StrAddC(&cur, real);
    }
    if (cur.count == 0) return "/";
    return StrC(&cur);
}

// Is a resolved path strictly inside /tmp?
static bool InTmp(const char *resolved)
{
    return strncmp(resolved, "/tmp/", 5) == 0 && resolved[5] != '\0';
}

// A path a deletion command would remove: it must be literal and resolve under /tmp/. A glob is allowed
// only in its last component (which can't match "." or ".."), with the folder before it under /tmp/
// (not /tmp itself: that would take everyone's temporary files). `what` names the command for the message.
static void CheckDeletePath(const char *cwd, const Arg *a, bool followLast, const char *what)
{
    const char *word = Fmt("%s %s", what, a->text);
    if (!gFirstDeletion) gFirstDeletion = word;
    if (!a->literal) {
        Refuse(RULE_DELETE, word, HINT_UNSEEN);
        return;
    }
    const char *path = a->text;
    if (a->glob) {
        // the glob may only be in the last component
        size_t slash = a->len;
        while (slash > 0 && a->text[slash - 1] != '/') slash--;
        for (size_t i = 0; i < slash; i++) {
            if (a->kind[i] == K_LIT && strchr("*?[", a->text[i])) {
                Refuse(RULE_DELETE, word, HINT_UNSEEN);
                return;
            }
        }
        const char *last = a->text + slash;
        if (fnmatch(last, "..", 0) == 0 || fnmatch(last, ".", 0) == 0) {
            Refuse(RULE_DELETE, word, HINT_UNSEEN);
            return;
        }
        const char *dir = slash > 0 ? Dup(a->text, slash) : ".";
        const char *r = Resolve(cwd, dir, true);
        if (!r || !InTmp(Fmt("%s/", r)) || strcmp(r, "/tmp") == 0) {
            Refuse(RULE_DELETE, word, !r || strcmp(r, "/tmp") == 0 ? HINT_UNSEEN : HINT_TRASH);
        }
        return;
    }
    const char *r = Resolve(cwd, path, followLast);
    if (!r) Refuse(RULE_DELETE, word, HINT_UNSEEN);
    else if (!InTmp(r)) Refuse(RULE_DELETE, word, HINT_TRASH);
}

// A path a command writes to ("stay in the repo"): only a literal path is checked; it must resolve into
// the repo or /tmp, or be one of the terminal / descriptor devices.
static void CheckWritePath(const char *cwd, const Arg *a, const char *what)
{
    if (!a->literal || a->glob) return;
    const char *p = a->text;
    if (strcmp(p, "/dev/null") == 0 || strcmp(p, "/dev/stdout") == 0 || strcmp(p, "/dev/stderr") == 0 ||
        strcmp(p, "/dev/tty") == 0 || strncmp(p, "/dev/fd/", 8) == 0 || strncmp(p, "/proc/self/fd/", 14) == 0) {
        return;
    }
    const char *r = Resolve(cwd, p, true);
    if (!r) return;  // relative with an unknown working directory: can't tell, let it pass
    if (Under(r, "/tmp") || Under(r, gToolRoot) || Under(r, gRootReal)) return;
    Refuse(RULE_STAY, Fmt("%s %s", what, p), HINT_STAY);
}

// #endregion

// #region Checking one command

// What the walk knows while it checks a command line: the working directory (NULL: unknown), whether a
// cd to a folder that doesn't exist yet is assumed to succeed only for the rest of an && chain, whether
// vsync was switched off for the commands that follow, how deep the nesting is, and (namesFrom) the
// wrapper whose arguments the commands get when run-sh.o can't see them (a `sh -c` under xargs, parallel's
// command string): their deletions are refused.
typedef struct {
    const char *cwd;
    bool cwdTentative;
    bool vsync;
    int depth;
    const char *namesFrom;
} Ctx;

// What a command is run with besides its words: its working directory (env -C), vsync off in its
// environment, text fed to its stdin (a here-document or here-string), names it gets that run-sh.o can't
// see (namesFrom: "xargs", "parallel"), and find's roots (the paths {} stands for in find -exec).
typedef struct {
    const char *cwd;
    bool vsync;
    const char *stdinText;
    const char *namesFrom;
    const Arg *roots;
    size_t rootCount;
    bool top;               // the command itself, not one a wrapper runs (only then a cd counts)
} Extra;

// A cd's effect on the commands after it: `set` when the command was a cd / pushd / popd; target NULL
// when the new directory is unknown.
typedef struct {
    bool set;
    const char *target;
} CdEffect;

// (The checks call back into ParseText for nested command lines.)
static void ParseText(Ctx *ctx, const char *text);
static void CheckCommand(Ctx *ctx, Arg *a, size_t n, Extra ex, CdEffect *eff);

// The context of a command line a command runs (a -c string, eval's text, a wrapper's string): one level
// deeper, in the command's working directory, with the names a wrapper passes it still unknown.
static Ctx SubCtx(const Ctx *ctx, const Extra *ex)
{
    Ctx sub = *ctx;
    sub.depth++;
    sub.cwd = ex->cwd;
    if (ex->namesFrom) sub.namesFrom = ex->namesFrom;
    return sub;
}

// Each substitution of a word runs as a command line of its own, in a subshell with the same working
// directory.
static void CheckSubs(Ctx *ctx, const Word *w)
{
    for (size_t i = 0; i < w->subs.count && !gRefusal; i++) {
        Ctx sub = *ctx;
        sub.depth++;
        ParseText(&sub, w->subs.items[i]);
    }
}

// The vsync variables a benchmark needs set to 0.
static bool IsVsyncName(const char *name)
{
    return strcmp(name, "__GL_SYNC_TO_VBLANK") == 0 || strcmp(name, "vblank_mode") == 0;
}

// Is vsync already off in run-sh.o's own environment (exported by the caller)?
static bool EnvVsync(void)
{
    const char *a = getenv("__GL_SYNC_TO_VBLANK"), *b = getenv("vblank_mode");
    return (a && strcmp(a, "0") == 0) || (b && strcmp(b, "0") == 0);
}

// A NAME=value (or NAME+=value) assignment, as a prefix, a statement, or an argument of export / env /
// declare: MONOLITH_ALLOW_WINDOW may never be set; vsync off is remembered. `text` may also be a bare NAME
// (export NAME).
static void CheckAssignment(const char *text, bool *vsync)
{
    size_t n = 0;
    while (text[n] && (isalnum((unsigned char)text[n]) || text[n] == '_')) n++;
    const char *name = Dup(text, n);
    if (strcmp(name, "MONOLITH_ALLOW_WINDOW") == 0) {
        Refuse(RULE_WINDOW, "MONOLITH_ALLOW_WINDOW",
               "never set it: a session must not open a window (CLAUDE.md); run the game with --shot <file> "
               "or another windowless flag");
        return;
    }
    const char *eq = strchr(text, '=');
    if (IsVsyncName(name) && eq && strcmp(eq + 1, "0") == 0) *vsync = true;
}

// Is this word a shell assignment (NAME=…, NAME+=…, NAME[i]=…) with an unquoted name?
static bool IsAssignment(const Word *w)
{
    const char *t = w->text.items;
    size_t n = w->text.count, i = 0;
    if (n == 0 || !(isalpha((unsigned char)t[0]) || t[0] == '_') || w->kind.items[0] != K_LIT) return false;
    while (i < n && (isalnum((unsigned char)t[i]) || t[i] == '_') && w->kind.items[i] == K_LIT) i++;
    if (i < n && t[i] == '[') {
        while (i < n && t[i] != ']') i++;
        i++;
    }
    if (i < n && t[i] == '+') i++;
    return i < n && t[i] == '=' && w->kind.items[i] == K_LIT;
}

// Does the argument list contain this flag, as a word of its own or as flag=value?
static bool HasFlag(Arg *a, size_t n, const char *flag)
{
    size_t fl = strlen(flag);
    for (size_t i = 0; i < n; i++) {
        if (!a[i].literal) continue;
        if (strcmp(a[i].text, flag) == 0 || (strncmp(a[i].text, flag, fl) == 0 && a[i].text[fl] == '=')) {
            return true;
        }
    }
    return false;
}

// Does a short-option cluster (-xyz, not --long) among the arguments contain this letter?
static bool HasShort(Arg *a, size_t n, char letter)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i].literal && a[i].text[0] == '-' && a[i].text[1] != '-' && strchr(a[i].text + 1, letter)) {
            return true;
        }
    }
    return false;
}

// The game's arguments (after `monolith`, `build.sh run`, ...): it must not open a window, and a benchmark
// must be headless, vsync-off, with a note.
static void CheckGame(const char *word, Arg *a, size_t n, const Extra *ex, const Ctx *ctx)
{
    if (HasFlag(a, n, "--bench")) {
        if (!HasFlag(a, n, "--headless")) {
            Refuse(RULE_BENCH, Fmt("%s --bench", word), "add --headless (CLAUDE.md, the benchmarking skill)");
        }
        if (!ex->vsync && !ctx->vsync && !EnvVsync()) {
            Refuse(RULE_BENCH, Fmt("%s --bench", word),
                   "put __GL_SYNC_TO_VBLANK=0 in front of the command (vblank_mode=0 on Mesa)");
        }
        bool note = false;
        for (size_t i = 0; i < n; i++) {
            if (Eq(&a[i], "--note") && i + 1 < n && a[i + 1].len > 0) note = true;
            if (a[i].literal && strncmp(a[i].text, "--note=", 7) == 0 && a[i].text[7]) note = true;
        }
        if (!note) Refuse(RULE_BENCH, Fmt("%s --bench", word), "add --note \"what changed\"");
        gBench = true;
    }
    static const char *windowless[] = {"--shot", "--dump-boxes", "--dump-scene", "--dump-scene-diff",
                                       "--dump-entities", "--apply-edits", "--inspect-model", "--new-map",
                                       "--export", "--compile", "--classify", "--bake-surface"};
    for (size_t i = 0; i < NOB_ARRAY_LEN(windowless); i++) {
        if (HasFlag(a, n, windowless[i])) return;
    }
    if (HasFlag(a, n, "--bench") && HasFlag(a, n, "--headless")) return;
    Refuse(RULE_WINDOW, word, HINT_GAME);
}

// git: the subcommands that delete files or throw away work.
static void CheckGit(Ctx *ctx, Arg *a, size_t n, const Extra *ex)
{
    const char *cwd = ex->cwd;
    size_t i = 1;
    // git's own options before the subcommand
    while (i < n && a[i].literal && a[i].text[0] == '-') {
        const char *o = a[i].text;
        if (strcmp(o, "-C") == 0 && i + 1 < n) {
            cwd = a[i + 1].literal ? Absolute(cwd, a[i + 1].text) : NULL;
            i += 2;
        } else if (strcmp(o, "-c") == 0 && i + 1 < n) {
            // an alias whose value starts with '!' is a shell command
            const char *kv = a[i + 1].text, *eq = strchr(kv, '=');
            if (strncmp(kv, "alias.", 6) == 0 && eq && eq[1] == '!') {
                Ctx sub = *ctx;
                sub.depth++;
                ParseText(&sub, eq + 2);
            }
            i += 2;
        } else if ((strcmp(o, "--git-dir") == 0 || strcmp(o, "--work-tree") == 0 || strcmp(o, "--namespace") == 0 ||
                    strcmp(o, "--super-prefix") == 0 || strcmp(o, "--config-env") == 0) && i + 1 < n) {
            i += 2;
        } else {
            i++;
        }
    }
    if (i >= n || !a[i].literal) return;
    const char *sub = a[i].text;
    Arg *r = a + i + 1;
    size_t rn = n - i - 1;

    if (strcmp(sub, "clean") == 0) {
        if (!HasFlag(r, rn, "--dry-run") && !HasShort(r, rn, 'n')) {
            Refuse(RULE_DELETE, "git clean",
                   "it deletes untracked files for good: look with `git clean -n`, then `gio trash` what should go");
        }
    } else if (strcmp(sub, "rm") == 0) {
        if (!HasFlag(r, rn, "--cached")) {
            Refuse(RULE_DELETE, "git rm",
                   "use `git rm --cached <path>` (the file stays), or `gio trash <path>` and let git see it gone");
        }
    } else if (strcmp(sub, "reset") == 0) {
        if (HasFlag(r, rn, "--hard")) {
            Refuse(RULE_LOST, "git reset --hard",
                   "it throws away uncommitted work: `git stash` keeps it; ask the developer before discarding");
        }
    } else if (strcmp(sub, "checkout") == 0) {
        const char *bad = HasFlag(r, rn, "--") ? "--" : HasFlag(r, rn, ".") ? "." : HasFlag(r, rn, "--force") ? "--force"
                        : HasFlag(r, rn, "-f") ? "-f" : NULL;
        if (bad) {
            Refuse(RULE_LOST, Fmt("git checkout %s", bad),
                   "it overwrites uncommitted changes: `git stash` keeps them; ask the developer before discarding");
        }
    } else if (strcmp(sub, "restore") == 0) {
        bool staged = HasFlag(r, rn, "--staged") || HasShort(r, rn, 'S');
        bool worktree = HasFlag(r, rn, "--worktree") || HasShort(r, rn, 'W');
        if (!staged || worktree) {
            Refuse(RULE_LOST, "git restore",
                   "only `git restore --staged <path>` (unstage) is allowed: discarding changes needs the developer");
        }
    } else if (strcmp(sub, "stash") == 0) {
        if (rn > 0 && (Eq(&r[0], "drop") || Eq(&r[0], "clear"))) {
            Refuse(RULE_LOST, Fmt("git stash %s", r[0].text), "the stashed work would be lost: leave the stash, or ask the developer");
        }
    } else if (strcmp(sub, "switch") == 0) {
        if (HasFlag(r, rn, "--force") || HasFlag(r, rn, "--discard-changes") || HasShort(r, rn, 'f')) {
            Refuse(RULE_LOST, "git switch --force",
                   "it throws away uncommitted changes: `git stash` first, or ask the developer");
        }
    } else if (strcmp(sub, "worktree") == 0 && rn > 0 && Eq(&r[0], "remove")) {
        for (size_t k = 1; k < rn; k++) {
            if (r[k].literal && r[k].text[0] == '-') continue;
            CheckDeletePath(cwd, &r[k], true, "git worktree remove");
        }
    }
}

// The deletion commands (rm, rmdir, unlink, shred, gio remove): every operand must resolve under /tmp/.
// In find -exec, {} stands for find's roots (already checked there); with xargs the names come from stdin.
// `shown` is the command as typed (/bin/rm), for the message.
static void CheckDelete(const char *name, const char *shown, Arg *a, size_t n, const Extra *ex)
{
    if (ex->namesFrom) {
        Refuse(RULE_DELETE, Fmt("%s %s", ex->namesFrom, shown), HINT_UNSEEN);
        return;
    }
    bool ended = false;
    for (size_t i = 0; i < n; i++) {
        if (!ended && Eq(&a[i], "--")) {
            ended = true;
            continue;
        }
        if (!ended && a[i].literal && a[i].text[0] == '-' && a[i].text[1]) {
            // shred's options with a value
            if (strcmp(name, "shred") == 0 && (Eq(&a[i], "-n") || Eq(&a[i], "-s"))) i++;
            continue;
        }
        if (ex->roots && strstr(a[i].text, "{}")) {
            if (!Eq(&a[i], "{}")) Refuse(RULE_DELETE, Fmt("%s %s", shown, a[i].text), HINT_UNSEEN);
            continue;
        }
        CheckDeletePath(ex->cwd, &a[i], false, shown);
    }
}

// find: -delete removes the roots' trees; -exec / -execdir / -ok / -okdir run commands (checked with {}
// standing for the roots). -L / -follow with a deletion could walk out of /tmp through a link.
static void CheckFind(Ctx *ctx, Arg *a, size_t n, const Extra *ex)
{
    size_t i = 1;
    bool follow = false, del = false;
    while (i < n && a[i].literal && (Eq(&a[i], "-H") || Eq(&a[i], "-L") || Eq(&a[i], "-P") ||
                                      Eq(&a[i], "-D") || strncmp(a[i].text, "-O", 2) == 0)) {
        if (Eq(&a[i], "-L")) follow = true;
        if (Eq(&a[i], "-D")) i++;
        i++;
    }
    // the roots: the operands before the expression
    Args roots = {0};
    while (i < n && !(a[i].literal && (a[i].text[0] == '-' || Eq(&a[i], "(") || Eq(&a[i], "!") ||
                                       Eq(&a[i], ")") || Eq(&a[i], ",")))) {
        PUSH(&roots, a[i]);
        i++;
    }
    if (roots.count == 0) PUSH(&roots, TextArg("."));
    // the expression: deletions and executed commands
    for (; i < n && !gRefusal; i++) {
        if (Eq(&a[i], "-delete")) del = true;
        if (Eq(&a[i], "-follow")) follow = true;
        if (Eq(&a[i], "-exec") || Eq(&a[i], "-execdir") || Eq(&a[i], "-ok") || Eq(&a[i], "-okdir")) {
            size_t s = i + 1, e = s;
            while (e < n && !Eq(&a[e], ";") && !Eq(&a[e], "+")) e++;
            Extra sub = *ex;
            sub.top = false;
            sub.roots = roots.items;
            sub.rootCount = roots.count;
            CheckCommand(ctx, a + s, e - s, sub, NULL);
            // a deletion command run on {} removes what find found: the roots must be under /tmp/
            if (e > s && a[s].literal) {
                const char *b = Base(a[s].text);
                if (strcmp(b, "rm") == 0 || strcmp(b, "rmdir") == 0 || strcmp(b, "unlink") == 0 ||
                    strcmp(b, "shred") == 0) {
                    del = true;
                }
            }
            i = e;
        }
    }
    if (!del) return;
    if (follow) {
        Refuse(RULE_DELETE, "find -L / -follow with a deletion",
               "it follows links out of /tmp; drop -L / -follow, or use `gio trash <path>`");
        return;
    }
    for (size_t k = 0; k < roots.count; k++) CheckDeletePath(ex->cwd, &roots.items[k], true, "find");
}

// Deletion calls in an interpreter's program text (python -c, perl -e, ...): refused by name, and so is
// setting MONOLITH_ALLOW_WINDOW there.
static void CheckCode(const char *interp, const char *code)
{
    static const char *calls[] = {"remove(", "unlink", "rmdir", "rmtree", "removedirs", "rmSync", "rm_rf",
                                  "rm_r", "rm_f", "remove_tree", "rimraf", "File.delete", "FileUtils.rm",
                                  "fs.rm", ".rm("};
    for (size_t i = 0; i < NOB_ARRAY_LEN(calls); i++) {
        if (strstr(code, calls[i])) {
            Refuse(RULE_DELETE, Fmt("%s: %s", interp, calls[i]),
                   "no deletion from interpreter one-liners: `gio trash <path>`, or rm on literal paths under /tmp/");
            return;
        }
    }
    // the word rm (a shell command in a string: os.system('rm -rf x'))
    for (const char *p = code; (p = strstr(p, "rm")) != NULL; p += 2) {
        bool before = p == code || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        bool after = !(isalnum((unsigned char)p[2]) || p[2] == '_');
        if (before && after) {
            Refuse(RULE_DELETE, Fmt("%s: rm", interp),
                   "no deletion from interpreter one-liners: `gio trash <path>`, or rm on literal paths under /tmp/");
            return;
        }
    }
    if (strstr(code, "MONOLITH_ALLOW_WINDOW")) {
        Refuse(RULE_WINDOW, Fmt("%s: MONOLITH_ALLOW_WINDOW", interp),
               "never set it: a session must not open a window (CLAUDE.md)");
    }
}

// Which interpreters' one-liners are read, and how their program text is given: short options (in a
// cluster: the rest of the word or the next word) and long options (the next word or =value).
typedef struct {
    const char *name;      // basename; "python" also covers python3, python3.12, ...
    const char *shortCode; // option letters that take the program text
    const char *longCode[2];
} Interpreter;

static const Interpreter INTERPRETERS[] = {
    {"python", "c", {NULL, NULL}},
    {"perl", "eE", {NULL, NULL}},
    {"node", "ep", {"--eval", "--print"}},
    {"nodejs", "ep", {"--eval", "--print"}},
    {"ruby", "e", {NULL, NULL}},
    {"php", "r", {NULL, NULL}},
};

// The interpreter this basename runs (python3.12 counts as python), or NULL.
static const Interpreter *FindInterpreter(const char *name)
{
    if (strncmp(name, "python", 6) == 0 || strncmp(name, "pypy", 4) == 0) {
        const char *p = name + (name[1] == 'y' && name[2] == 'p' ? 4 : 6);
        while (*p && (isdigit((unsigned char)*p) || *p == '.')) p++;
        if (*p == '\0') return &INTERPRETERS[0];
        return NULL;
    }
    for (size_t i = 1; i < NOB_ARRAY_LEN(INTERPRETERS); i++) {
        if (strcmp(name, INTERPRETERS[i].name) == 0) return &INTERPRETERS[i];
    }
    return NULL;
}

// An interpreter's program text from its options; with neither program text nor a script, it reads its
// program from stdin (a here-document is then the program).
static void CheckInterpreter(const Interpreter *in, const char *name, Arg *a, size_t n, const Extra *ex)
{
    bool program = false;
    for (size_t i = 1; i < n && !gRefusal; i++) {
        const char *t = a[i].text;
        if (!a[i].literal || t[0] != '-' || strcmp(t, "-") == 0) {
            program = program || strcmp(t, "-") != 0;  // a script file (not read) or "-" (stdin)
            break;
        }
        if (strcmp(t, "--") == 0) {
            program = program || i + 1 < n;
            break;
        }
        if (t[1] == '-') {
            for (size_t k = 0; k < 2; k++) {
                const char *lo = in->longCode[k];
                size_t ll = lo ? strlen(lo) : 0;
                if (lo && strcmp(t, lo) == 0 && i + 1 < n) {
                    CheckCode(name, a[++i].text);
                    program = true;
                } else if (lo && strncmp(t, lo, ll) == 0 && t[ll] == '=') {
                    CheckCode(name, t + ll + 1);
                    program = true;
                }
            }
            continue;
        }
        for (const char *p = t + 1; *p; p++) {
            if (strchr(in->shortCode, *p)) {
                if (p[1]) CheckCode(name, p + 1);
                else if (i + 1 < n) CheckCode(name, a[++i].text);
                program = true;
                break;
            }
            if (in == &INTERPRETERS[0] && *p == 'm') {
                program = true;  // python -m module: a program on disk
                break;
            }
        }
    }
    if (!program && ex->stdinText) CheckCode(name, ex->stdinText);
}

// Is this the basename of a shell that takes -c?
static bool IsShell(const char *b)
{
    return strcmp(b, "bash") == 0 || strcmp(b, "sh") == 0 || strcmp(b, "zsh") == 0 || strcmp(b, "dash") == 0 ||
           strcmp(b, "ksh") == 0 || strcmp(b, "mksh") == 0 || strcmp(b, "rbash") == 0;
}

// The words joined with spaces (eval's argument, env -S's string with the rest).
static const char *JoinArgs(Arg *a, size_t n)
{
    Str s = {0};
    for (size_t i = 0; i < n; i++) {
        if (i) PUSH(&s, ' ');
        StrAddC(&s, a[i].text);
    }
    return StrC(&s);
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

// Skips a wrapper's options: words starting with '-' (each in `withValue` takes the next word too,
// unless written as --opt=value), up to "--" or the first operand. Returns the operand's index.
static size_t SkipOptions(Arg *a, size_t n, size_t i, const char *const *withValue)
{
    while (i < n && a[i].literal && a[i].text[0] == '-' && a[i].text[1]) {
        if (strcmp(a[i].text, "--") == 0) return i + 1;
        bool value = false;
        for (const char *const *w = withValue; w && *w; w++) value = value || strcmp(a[i].text, *w) == 0;
        i += value ? 2 : 1;
    }
    return i;
}

// The string of a -c / --command option (also in a cluster: -qc '<string>', -lc '<string>', --command=…)
// among a[from..n), or NULL.
static const char *CommandOption(Arg *a, size_t n, size_t from)
{
    for (size_t k = from; k < n; k++) {
        const char *o = a[k].text;
        if (!a[k].literal || o[0] != '-' || !o[1]) continue;
        if (strcmp(o, "--command") == 0) return k + 1 < n ? a[k + 1].text : NULL;
        if (strncmp(o, "--command=", 10) == 0) return o + 10;
        if (o[1] == '-') continue;
        const char *c = strchr(o + 1, 'c');
        if (c) return c[1] ? c + 1 : (k + 1 < n ? a[k + 1].text : NULL);
    }
    return NULL;
}

// Is this the basename of an awk?
static bool IsAwk(const char *b)
{
    return strcmp(b, "awk") == 0 || strcmp(b, "gawk") == 0 || strcmp(b, "mawk") == 0 || strcmp(b, "nawk") == 0;
}

// Can this character be part of an awk name?
static bool IsIdentChar(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

// An awk string literal starting at t[p] ('"'): its value (\n, \t, \\, \" and \/ decoded) in *out; returns the
// index just past the closing quote.
static size_t AwkString(const char *t, size_t n, size_t p, const char **out)
{
    Str v = {0};
    for (p++; p < n && t[p] != '"'; p++) {
        if (t[p] == '\\' && p + 1 < n) {
            p++;
            PUSH(&v, t[p] == 'n' ? '\n' : t[p] == 't' ? '\t' : t[p]);
        } else {
            PUSH(&v, t[p]);
        }
    }
    *out = StrC(&v);
    return p < n ? p + 1 : n;
}

// Refuses an awk command run-sh.o can't read as one literal string.
static void AwkUnseen(const char *name, const char *what)
{
    Refuse(RULE_DELETE, Fmt("%s %s", name, what),
           "run-sh.o can't see the command awk would run (it isn't one literal string); write the whole command "
           "as one string, or run it outside awk");
}

// The commands an awk program runs: system("…"), "…" | getline and print … | "…". A single literal string
// is checked as a command line of its own; anything else (a variable, a concatenation) is refused. String
// literals, regular expressions and comments are stepped over, so a "|" or "system" inside them counts for
// nothing.
static void CheckAwkProgram(Ctx *ctx, const Extra *ex, const char *name, const char *t)
{
    size_t n = strlen(t);
    char prev = 0;  // the last significant character, to tell a regex from a division
    for (size_t p = 0; p < n && !gRefusal;) {
        char c = t[p];
        if (c == '"') {
            const char *ignored;
            p = AwkString(t, n, p, &ignored);
            prev = '"';
        } else if (c == '#') {
            while (p < n && t[p] != '\n') p++;
        } else if (c == '/' && (prev == 0 || strchr("~(,!{;&|\n=", prev))) {
            // a regex literal: up to the unescaped '/', a [...] class may hold one
            bool klass = false;
            for (p++; p < n && (klass || t[p] != '/'); p++) {
                if (t[p] == '\\') p++;
                else if (t[p] == '[') klass = true;
                else if (t[p] == ']') klass = false;
            }
            p++;
            prev = '/';
        } else if (isalpha((unsigned char)c) || c == '_') {
            size_t s = p;
            while (p < n && IsIdentChar(t[p])) p++;
            prev = 'a';
            if (p - s != 6 || strncmp(t + s, "system", 6) != 0) continue;
            size_t q = p;
            while (q < n && isspace((unsigned char)t[q])) q++;
            if (q >= n || t[q] != '(') continue;
            // system( "<literal>" )
            q++;
            while (q < n && isspace((unsigned char)t[q])) q++;
            const char *cmd = NULL;
            if (q < n && t[q] == '"') q = AwkString(t, n, q, &cmd);
            while (q < n && isspace((unsigned char)t[q])) q++;
            if (!cmd || q >= n || t[q] != ')') {
                AwkUnseen(name, "system(…)");
                return;
            }
            Ctx sub = SubCtx(ctx, ex);
            ParseText(&sub, cmd);
            p = q + 1;
        } else if (c == '|' && p + 1 < n && t[p + 1] == '|') {
            p += 2;
            prev = '|';
        } else if (c == '|') {
            size_t q = p + 1;
            if (q < n && t[q] == '&') q++;  // gawk's |& (a coprocess)
            while (q < n && (t[q] == ' ' || t[q] == '\t')) q++;
            if (q + 7 <= n && strncmp(t + q, "getline", 7) == 0 && (q + 7 == n || !IsIdentChar(t[q + 7]))) {
                // "<literal>" | getline: the string must be the whole expression before the '|'
                size_t r = p;
                while (r > 0 && (t[r - 1] == ' ' || t[r - 1] == '\t')) r--;
                size_t open = r;
                if (r > 0 && t[r - 1] == '"') {
                    open = r - 1;
                    while (open > 0 && !(t[open - 1] == '"' && (open < 2 || t[open - 2] != '\\'))) open--;
                    open = open > 0 ? open - 1 : 0;
                }
                size_t before = open;
                while (before > 0 && (t[before - 1] == ' ' || t[before - 1] == '\t')) before--;
                bool whole = open < r && t[open] == '"' && (before == 0 || strchr("({;\n=,&|!", t[before - 1]));
                if (!whole) {
                    AwkUnseen(name, "… | getline");
                    return;
                }
                const char *cmd;
                AwkString(t, n, open, &cmd);
                Ctx sub = SubCtx(ctx, ex);
                ParseText(&sub, cmd);
            } else if (q < n && t[q] == '"') {
                // print … | "<literal>": the output goes into that command
                const char *cmd;
                size_t e = AwkString(t, n, q, &cmd);
                while (e < n && (t[e] == ' ' || t[e] == '\t')) e++;
                if (e < n && !strchr(";}\n)#", t[e])) {
                    AwkUnseen(name, "print … | …");
                    return;
                }
                Ctx sub = SubCtx(ctx, ex);
                ParseText(&sub, cmd);
            } else {
                AwkUnseen(name, "print … | …");
                return;
            }
            p++;
            prev = '|';
        } else {
            if (!isspace((unsigned char)c)) prev = c;
            p++;
        }
    }
}

// awk's program text: the first operand, unless -f / --file names a program file (not read); gawk's
// -e / --source add text. Each text goes through CheckAwkProgram.
static void CheckAwk(Ctx *ctx, const Extra *ex, const char *name, Arg *a, size_t n)
{
    bool file = false;
    for (size_t k = 1; k < n && !gRefusal; k++) {
        const char *o = a[k].text;
        bool option = a[k].literal && o[0] == '-' && o[1];
        if (option && strcmp(o, "--") == 0) {
            if (!file && k + 1 < n) CheckAwkProgram(ctx, ex, name, a[k + 1].text);
            return;
        }
        if (option && (strcmp(o, "-f") == 0 || strcmp(o, "--file") == 0)) {
            file = true;
            k++;
        } else if (option && strncmp(o, "--file=", 7) == 0) {
            file = true;
        } else if (option && (strcmp(o, "-e") == 0 || strcmp(o, "--source") == 0)) {
            if (k + 1 < n) CheckAwkProgram(ctx, ex, name, a[++k].text);
        } else if (option && strncmp(o, "--source=", 9) == 0) {
            CheckAwkProgram(ctx, ex, name, o + 9);
        } else if (option && (strcmp(o, "-F") == 0 || strcmp(o, "-v") == 0 || strcmp(o, "-W") == 0 ||
                              strcmp(o, "--assign") == 0 || strcmp(o, "--field-separator") == 0)) {
            k++;
        } else if (option) {
            continue;
        } else {
            if (!file) CheckAwkProgram(ctx, ex, name, o);
            return;  // the rest are input files
        }
    }
}

// tar --remove-files: every file operand is deleted once archived. The archive (-f) isn't; -C changes the
// folder the operands after it are read from; -T (names from a file) can't be seen. Short options may be
// clustered (-czf archive) or old style without the dash (czf archive), their values taking the next
// words in order.
static void CheckTarRemove(const Extra *ex, Arg *c, size_t cn)
{
    static const char valueLetters[] = "fCTXbHKLNVgI";
    static const char *const longValues[] = {"--file", "--directory", "--files-from", "--exclude-from",
                                             "--blocking-factor", "--format", "--starting-file", "--newer",
                                             "--label", "--group", "--owner", "--use-compress-program", NULL};
    const char *cwd = ex->cwd;
    Str want = {0};  // the letters whose values are the next words
    size_t wi = 0;
    bool ended = false;
    for (size_t k = 1; k < cn && !gRefusal; k++) {
        const char *o = c[k].text;
        char letter = 0;
        const Arg *value = NULL;
        if (wi < want.count) {
            letter = want.items[wi++];
            value = &c[k];
        } else if (!ended && c[k].literal && strcmp(o, "--") == 0) {
            ended = true;
            continue;
        } else if (!ended && c[k].literal && o[0] == '-' && o[1] == '-') {
            // a long option: its value after '=' or in the next word
            const char *eq = strchr(o, '=');
            size_t ol = eq ? (size_t)(eq - o) : strlen(o);
            letter = strncmp(o, "--directory", ol) == 0 && ol == 11 ? 'C'
                   : strncmp(o, "--files-from", ol) == 0 && ol == 12 ? 'T' : 'x';
            if (eq) {
                Arg *v = Alloc(sizeof *v);
                *v = TextArg(eq + 1);
                value = v;
            } else {
                bool takes = false;
                for (const char *const *w = longValues; *w; w++) takes = takes || strcmp(o, *w) == 0;
                if (!takes) continue;
                if (k + 1 >= cn) break;
                value = &c[++k];
            }
        } else if (!ended && c[k].literal && ((o[0] == '-' && o[1]) || (k == 1 && isalpha((unsigned char)o[0])))) {
            // a cluster of short options (or the old-style first word): a value letter takes the rest of a
            // dashed word, or the next words in order
            for (const char *q = o + (o[0] == '-'); *q; q++) {
                if (!strchr(valueLetters, *q)) continue;
                if (o[0] == '-' && q[1]) {
                    letter = *q;
                    Arg *v = Alloc(sizeof *v);
                    *v = TextArg(q + 1);
                    value = v;
                    break;
                }
                PUSH(&want, *q);
            }
            if (!value) continue;
        } else {
            // an operand: archived, then deleted
            CheckDeletePath(cwd, &c[k], false, "tar --remove-files");
            continue;
        }
        if (letter == 'C') cwd = value->literal ? Absolute(cwd, value->text) : NULL;
        if (letter == 'T') Refuse(RULE_DELETE, "tar --remove-files -T", HINT_UNSEEN);
    }
}

// zip -m / --move: the files after the archive are deleted once zipped (-x / -i patterns aren't files;
// -@ reads the names from stdin, which can't be seen).
static void CheckZipMove(const Extra *ex, Arg *c, size_t cn)
{
    static const char *const values[] = {"-b", "-n", "-t", "-tt", "-O", "-P", "-Z", "-s", "--temp-path",
                                         "--suffixes", "--output-file", "--password", "--split-size", NULL};
    bool archive = false, patterns = false;
    for (size_t k = 1; k < cn && !gRefusal; k++) {
        const char *o = c[k].text;
        bool option = c[k].literal && o[0] == '-' && o[1];
        if (option) {
            if (o[1] != '-' && strchr(o + 1, '@')) {
                Refuse(RULE_DELETE, "zip -m -@", HINT_UNSEEN);
                return;
            }
            patterns = strcmp(o, "-x") == 0 || strcmp(o, "-i") == 0 || strcmp(o, "--exclude") == 0 ||
                       strcmp(o, "--include") == 0;
            for (const char *const *w = values; *w; w++) {
                if (strcmp(o, *w) == 0) k++;
            }
            continue;
        }
        if (patterns) continue;
        if (!archive) {
            archive = true;  // the zip file itself is written, not deleted
            continue;
        }
        CheckDeletePath(ex->cwd, &c[k], false, "zip -m");
    }
}

// rsync --delete… (deletes in the destination what the source lacks) and --remove-source-files (deletes
// the sources it sent): the destination, or every source, must be under /tmp/. A remote path (host:path)
// is on another machine and isn't checked; --files-from with --remove-source-files can't be seen.
static void CheckRsync(const Extra *ex, Arg *c, size_t cn)
{
    static const char *const values[] = {"-e", "--rsh", "-f", "--filter", "-B", "--block-size", "-T",
                                         "--temp-dir", "--exclude", "--include", "--exclude-from",
                                         "--include-from", "--files-from", "--partial-dir", "--backup-dir",
                                         "--compare-dest", "--copy-dest", "--link-dest", "--log-file", "--suffix",
                                         "--chmod", "--max-size", "--min-size", "--timeout", "--bwlimit", "-M",
                                         "--remote-option", "--password-file", "--port", "--out-format",
                                         "--info", "--debug", "--chown", "--usermap", "--groupmap",
                                         "--max-delete", "--modify-window", "--iconv", NULL};
    bool del = false, removeSources = false, filesFrom = false;
    Args operands = {0};
    bool ended = false;
    for (size_t k = 1; k < cn; k++) {
        const char *o = c[k].text;
        bool option = !ended && c[k].literal && o[0] == '-' && o[1];
        if (option && strcmp(o, "--") == 0) {
            ended = true;
        } else if (option) {
            if (strncmp(o, "--del", 5) == 0) del = true;
            if (strcmp(o, "--remove-source-files") == 0 || strcmp(o, "--remove-sent-files") == 0) removeSources = true;
            if (strncmp(o, "--files-from", 12) == 0) filesFrom = true;
            for (const char *const *w = values; *w; w++) {
                if (strcmp(o, *w) == 0) k++;
            }
        } else {
            PUSH(&operands, c[k]);
        }
    }
    if (operands.count < 2) return;  // a listing
    for (size_t k = 0; k < operands.count && !gRefusal; k++) {
        const Arg *op = &operands.items[k];
        const char *colon = op->literal ? strchr(op->text, ':') : NULL;
        const char *slash = op->literal ? strchr(op->text, '/') : NULL;
        if (colon && (!slash || colon < slash)) continue;  // host:path, rsync://host/...
        bool dest = k + 1 == operands.count;
        if (dest && del) CheckDeletePath(ex->cwd, op, true, "rsync --delete");
        if (!dest && removeSources) CheckDeletePath(ex->cwd, op, false, "rsync --remove-source-files");
    }
    if (removeSources && filesFrom) Refuse(RULE_DELETE, "rsync --remove-source-files --files-from", HINT_UNSEEN);
}

// The destination of cp / mv / install ("stay in the repo"): -t DIR (or --target-directory=DIR), else the last
// of two or more operands; with `everyOperand` (install -d) every operand is a folder it creates. `values`:
// the options whose value is the next word.
static void CheckDestination(const char *cwd, const char *b, Arg *c, size_t cn, const char *const *values,
                             bool everyOperand)
{
    const Arg *target = NULL, *last = NULL;
    size_t operands = 0;
    bool ended = false;
    for (size_t k = 1; k < cn; k++) {
        const char *o = c[k].text;
        bool option = !ended && c[k].literal && o[0] == '-' && o[1];
        if (option && strcmp(o, "--") == 0) {
            ended = true;
        } else if (option && (strcmp(o, "-t") == 0 || strcmp(o, "--target-directory") == 0)) {
            if (k + 1 < cn) target = &c[++k];
        } else if (option && strncmp(o, "--target-directory=", 19) == 0) {
            Arg *t = Alloc(sizeof *t);
            *t = TextArg(o + 19);
            target = t;
        } else if (option) {
            for (const char *const *w = values; *w; w++) {
                if (strcmp(o, *w) == 0) k++;
            }
        } else if (everyOperand) {
            CheckWritePath(cwd, &c[k], b);
        } else {
            last = &c[k];
            operands++;
        }
    }
    if (target) CheckWritePath(cwd, target, b);
    else if (last && operands >= 2) CheckWritePath(cwd, last, b);
}

// The command a[0..n) as it would run: wrappers are unwrapped (their own checks on the way), nested
// shells and eval read again, then the rules for the command itself. `eff` gets a cd's effect (only for
// the top command).
static void CheckCommand(Ctx *ctx, Arg *a, size_t n, Extra ex, CdEffect *eff)
{
    size_t i = 0;
    while (i < n && !gRefusal) {
        if (!a[i].literal) return;  // an unknown command word (known limit)
        const char *word = a[i].text;
        const char *b = Base(word);
        if (b[0] == '\0') return;

        // wrappers that run the rest of the line as a command
        if (strcmp(b, "command") == 0) {
            i++;
            while (i < n && a[i].literal && a[i].text[0] == '-') {
                if (Eq(&a[i], "-v") || Eq(&a[i], "-V")) return;  // only a lookup
                i++;
            }
            ex.top = false;
            continue;
        }
        if (strcmp(b, "builtin") == 0 || strcmp(b, "nohup") == 0 || strcmp(b, "wine") == 0 ||
            strcmp(b, "wine64") == 0) {
            i++;
            if (i < n && Eq(&a[i], "--")) i++;
            ex.top = ex.top && strcmp(b, "builtin") == 0;
            continue;
        }
        if (strcmp(b, "exec") == 0) {
            static const char *const v[] = {"-a", NULL};
            i = SkipOptions(a, n, i + 1, v);
            ex.top = false;
            continue;
        }
        if (strcmp(b, "nice") == 0) {
            static const char *const v[] = {"-n", "--adjustment", NULL};
            i = SkipOptions(a, n, i + 1, v);
            ex.top = false;
            continue;
        }
        if (strcmp(b, "timeout") == 0) {
            static const char *const v[] = {"-s", "-k", "--signal", "--kill-after", NULL};
            i = SkipOptions(a, n, i + 1, v) + 1;  // then the duration
            ex.top = false;
            continue;
        }
        if (strcmp(b, "time") == 0) {
            static const char *const v[] = {"-f", "-o", "--format", "--output", NULL};
            size_t s = i + 1;
            i = SkipOptions(a, n, s, v);
            for (size_t k = s; k + 1 < i; k++) {
                if (Eq(&a[k], "-o") || Eq(&a[k], "--output")) CheckWritePath(ex.cwd, &a[k + 1], "time -o");
            }
            ex.top = false;
            continue;
        }
        if (strcmp(b, "stdbuf") == 0) {
            static const char *const v[] = {"-i", "-o", "-e", NULL};
            i = SkipOptions(a, n, i + 1, v);
            ex.top = false;
            continue;
        }
        if (strcmp(b, "ionice") == 0) {
            static const char *const v[] = {"-c", "-n", "--class", "--classdata", NULL};
            size_t s = i + 1;
            i = SkipOptions(a, n, s, v);
            for (size_t k = s; k < i; k++) {
                if (Eq(&a[k], "-p") || Eq(&a[k], "-P") || Eq(&a[k], "-u")) return;  // sets a running process
            }
            ex.top = false;
            continue;
        }
        if (strcmp(b, "sudo") == 0 || strcmp(b, "doas") == 0) {
            static const char *const v[] = {"-u", "-g", "-h", "-p", "-C", "-D", "-r", "-t", "-U", "-T", NULL};
            i = SkipOptions(a, n, i + 1, v);
            ex.top = false;
            continue;
        }
        if (strcmp(b, "env") == 0) {
            // env's options (-C changes the directory, -S splits a string into the command), then NAME=value
            i++;
            ex.top = false;
            while (i < n && a[i].literal) {
                const char *o = a[i].text;
                if (strcmp(o, "--") == 0) {
                    i++;
                    break;
                }
                if (strcmp(o, "-u") == 0 || strcmp(o, "--unset") == 0) {
                    i += 2;
                } else if (strcmp(o, "-C") == 0 || strcmp(o, "--chdir") == 0) {
                    if (i + 1 < n) ex.cwd = a[i + 1].literal ? Absolute(ex.cwd, a[i + 1].text) : NULL;
                    i += 2;
                } else if (strncmp(o, "--chdir=", 8) == 0) {
                    ex.cwd = Absolute(ex.cwd, o + 8);
                    i++;
                } else if (strcmp(o, "-S") == 0 || strcmp(o, "--split-string") == 0 ||
                           strncmp(o, "--split-string=", 15) == 0 || (strncmp(o, "-S", 2) == 0 && o[2])) {
                    const char *str = o[1] == 'S' ? (o[2] ? o + 2 : (i + 1 < n ? a[++i].text : ""))
                                    : strchr(o, '=') ? strchr(o, '=') + 1 : (i + 1 < n ? a[++i].text : "");
                    Ctx sub = SubCtx(ctx, &ex);
                    ParseText(&sub, Fmt("%s %s", str, JoinArgs(a + i + 1, n - i - 1)));
                    return;
                } else if (o[0] == '-' && o[1]) {
                    i++;
                } else if (strchr(o, '=') && (isalpha((unsigned char)o[0]) || o[0] == '_')) {
                    CheckAssignment(o, &ex.vsync);
                    i++;
                } else {
                    break;
                }
            }
            continue;
        }
        if (strcmp(b, "xargs") == 0) {
            static const char *const v[] = {"-a", "-d", "-E", "-I", "-L", "-n", "-P", "-s", "--arg-file",
                                            "--delimiter", "--max-args", "--max-procs", "--max-chars", NULL};
            i = SkipOptions(a, n, i + 1, v);
            ex.namesFrom = "xargs";
            ex.top = false;
            continue;
        }
        if (strcmp(b, "find") == 0) {
            Extra fe = ex;
            fe.top = false;
            CheckFind(ctx, a + i, n - i, &fe);
            return;
        }
        if (strcmp(b, "run-sh.o") == 0 || IsShell(b)) {
            // -c '<text>' is read again; a shell without -c and without a script reads its stdin
            bool c = false;
            size_t k = i + 1;
            while (k < n && a[k].literal && (a[k].text[0] == '-' || a[k].text[0] == '+') && a[k].text[1]) {
                const char *o = a[k].text;
                if (strcmp(o, "--") == 0) {
                    k++;
                    break;
                }
                if (strcmp(o, "--check") == 0 || strcmp(o, "--hook") == 0 || strcmp(o, "--help") == 0) return;
                if (o[1] != '-' && strchr(o + 1, 'c')) c = true;
                if ((strcmp(o, "-o") == 0 || strcmp(o, "+o") == 0 || strcmp(o, "-O") == 0 || strcmp(o, "+O") == 0 ||
                     strcmp(o, "--rcfile") == 0 || strcmp(o, "--init-file") == 0)) {
                    k++;
                }
                k++;
            }
            Ctx sub = SubCtx(ctx, &ex);
            if (c && k < n) {
                ParseText(&sub, a[k].text);
            } else if (k >= n && ex.stdinText) {
                ParseText(&sub, ex.stdinText);
            } else if (k < n && a[k].literal && strcmp(Base(a[k].text), "build.sh") == 0) {
                i = k;  // bash build.sh run ...: checked as build.sh
                continue;
            }
            return;
        }
        if (strcmp(b, "eval") == 0) {
            Ctx sub = SubCtx(ctx, &ex);
            ParseText(&sub, JoinArgs(a + i + 1, n - i - 1));
            return;
        }
        if (strcmp(b, "setsid") == 0 || strcmp(b, "chronic") == 0) {
            i = SkipOptions(a, n, i + 1, NULL);
            ex.top = false;
            continue;
        }
        if (strcmp(b, "busybox") == 0 || strcmp(b, "toybox") == 0) {
            i++;  // the applet (busybox rm x) is the command word
            ex.top = false;
            continue;
        }
        if (strcmp(b, "strace") == 0 || strcmp(b, "ltrace") == 0) {
            static const char *const v[] = {"-e", "-o", "-p", "-s", "-u", "-a", "-b", "-E", "-I", "-O", "-P", "-S",
                                            "-X", "-U", "-n", NULL};
            size_t s = i + 1;
            i = SkipOptions(a, n, s, v);
            for (size_t k = s; k < i; k++) {
                if (Eq(&a[k], "-p")) return;  // attaches to a running process: no command of its own
            }
            ex.top = false;
            continue;
        }
        if (strcmp(b, "gdb") == 0) {
            // gdb --args <command>: the program and its arguments; without --args the program runs only by
            // gdb's own commands, which aren't read
            size_t k = i + 1;
            while (k < n && !Eq(&a[k], "--args")) k++;
            if (k >= n) return;
            i = k + 1;
            ex.top = false;
            continue;
        }
        if (strcmp(b, "flock") == 0) {
            // flock [options] <file> <command...> or flock [options] <file> -c '<string>' (options anywhere)
            static const char *const v[] = {"-w", "--wait", "--timeout", "-E", "--conflict-exit-code", NULL};
            bool file = false;
            size_t k = i + 1;
            while (k < n) {
                const char *o = a[k].text;
                bool option = a[k].literal && o[0] == '-' && o[1];
                if (option && (strcmp(o, "-c") == 0 || strcmp(o, "--command") == 0)) {
                    if (k + 1 < n) {
                        Ctx sub = SubCtx(ctx, &ex);
                        ParseText(&sub, a[k + 1].text);
                    }
                    return;
                }
                if (option && strncmp(o, "--command=", 10) == 0) {
                    Ctx sub = SubCtx(ctx, &ex);
                    ParseText(&sub, o + 10);
                    return;
                }
                if (option) {
                    bool value = false;
                    for (const char *const *w = v; *w; w++) value = value || strcmp(o, *w) == 0;
                    k += value ? 2 : 1;
                } else if (!file) {
                    file = true;  // the lock file (or an fd number: then there is no command)
                    k++;
                } else {
                    break;
                }
            }
            if (k >= n) return;
            i = k;
            ex.top = false;
            continue;
        }
        if (strcmp(b, "watch") == 0) {
            // watch runs its words with `sh -c` (joined by spaces), or directly with -x / --exec
            static const char *const v[] = {"-n", "--interval", "-q", "--equexit", NULL};
            size_t s = i + 1;
            size_t k = SkipOptions(a, n, s, v);
            if (k >= n) return;
            if (HasFlag(a + s, k - s, "--exec") || HasShort(a + s, k - s, 'x')) {
                i = k;
                ex.top = false;
                continue;
            }
            Ctx sub = SubCtx(ctx, &ex);
            ParseText(&sub, JoinArgs(a + k, n - k));
            return;
        }
        if (strcmp(b, "script") == 0 || strcmp(b, "su") == 0) {
            // script -c '<string>' / su -c '<string>' run the string with a shell; without -c they start an
            // interactive shell (nothing to check)
            const char *str = CommandOption(a, n, i + 1);
            if (str) {
                Ctx sub = SubCtx(ctx, &ex);
                ParseText(&sub, str);
            }
            return;
        }
        if (strcmp(b, "parallel") == 0) {
            // parallel [options] <command words> ::: <arguments>: the command words are a shell command line
            // that gets each argument (or a line of stdin): names run-sh.o can't see
            static const char *const v[] = {"-j", "--jobs", "-N", "-n", "--max-args", "-S", "--sshlogin", "-a",
                                            "--arg-file", "-I", "--colsep", "-d", "--delimiter", "--joblog",
                                            "--results", "--tmpdir", "-E", NULL};
            size_t k = SkipOptions(a, n, i + 1, v);
            size_t e = k;
            while (e < n && !(a[e].literal && strncmp(a[e].text, ":::", 3) == 0)) e++;
            if (e > k) {
                Ctx sub = SubCtx(ctx, &ex);
                sub.namesFrom = "parallel";
                ParseText(&sub, JoinArgs(a + k, e - k));
            }
            return;
        }
        break;
    }
    if (gRefusal || i >= n) return;

    // the command itself
    Arg *c = a + i;
    size_t cn = n - i;
    const char *word = c[0].text;
    const char *b = Base(word);
    const Interpreter *interp = FindInterpreter(b);

    // a link (ln without -s makes hard links, which can't redirect a folder, but every ln counts: simpler);
    // remembered for the check against deletions in the same line (Decide). cp goes on to its checks below.
    if (!gMakesLink && strcmp(b, "ln") == 0) gMakesLink = "ln";
    if (!gMakesLink && strcmp(b, "cp") == 0 &&
        (HasFlag(c + 1, cn - 1, "--symbolic-link") || HasShort(c + 1, cn - 1, 's'))) {
        gMakesLink = "cp -s";
    }

    if (strcmp(b, "rm") == 0 || strcmp(b, "rmdir") == 0 || strcmp(b, "unlink") == 0 || strcmp(b, "shred") == 0) {
        CheckDelete(b, word, c + 1, cn - 1, &ex);
    } else if (strcmp(b, "gio") == 0 && cn > 1 && (Eq(&c[1], "remove") || Eq(&c[1], "rm"))) {
        CheckDelete("gio remove", "gio remove", c + 2, cn - 2, &ex);
    } else if (strcmp(b, "git") == 0) {
        CheckGit(ctx, c, cn, &ex);
    } else if (interp) {
        CheckInterpreter(interp, b, c, cn, &ex);
    } else if (IsAwk(b)) {
        CheckAwk(ctx, &ex, b, c, cn);
    } else if (strcmp(b, "tar") == 0 && HasFlag(c + 1, cn - 1, "--remove-files")) {
        CheckTarRemove(&ex, c, cn);
    } else if (strcmp(b, "zip") == 0 && (HasFlag(c + 1, cn - 1, "--move") || HasShort(c + 1, cn - 1, 'm'))) {
        CheckZipMove(&ex, c, cn);
    } else if (strcmp(b, "rsync") == 0) {
        CheckRsync(&ex, c, cn);
    } else if (strcmp(b, "cd") == 0 || strcmp(b, "pushd") == 0 || strcmp(b, "popd") == 0) {
        // where the commands after it run (Walk applies it)
        if (!eff || !ex.top) return;
        eff->set = true;
        size_t k = 1;
        while (k < cn && c[k].literal && c[k].text[0] == '-' && c[k].text[1] && strcmp(c[k].text, "-") != 0) {
            k++;
        }
        const char *home = getenv("HOME");
        if (strcmp(b, "popd") == 0 || (k < cn && (!c[k].literal || Eq(&c[k], "-")))) {
            eff->target = NULL;
        } else if (k >= cn) {
            eff->target = home && home[0] == '/' ? DupC(home) : NULL;
        } else {
            eff->target = Absolute(ex.cwd, c[k].text);
        }
    } else if (strcmp(b, "export") == 0 || strcmp(b, "declare") == 0 || strcmp(b, "typeset") == 0 ||
               strcmp(b, "local") == 0 || strcmp(b, "readonly") == 0) {
        for (size_t k = 1; k < cn; k++) {
            if (c[k].text[0] == '-') continue;
            CheckAssignment(c[k].text, &ctx->vsync);
        }
    } else if (strcmp(b, "monolith") == 0 || strcmp(b, "monolith.exe") == 0) {
        CheckGame(word, c + 1, cn - 1, &ex, ctx);
    } else if (strcmp(b, "build.sh") == 0 || strcmp(b, "nob") == 0) {
        // build.sh run [args] / build.sh --win run [args] run the game
        size_t k = 1;
        if (k < cn && Eq(&c[k], "--win")) k++;
        if (k < cn && Eq(&c[k], "run")) CheckGame(Fmt("%s run", word), c + k + 1, cn - k - 1, &ex, ctx);
    } else if (strcmp(b, "i3-msg") == 0 || strcmp(b, "swaymsg") == 0) {
        // only the read-only message types (-t get_*), without a payload
        const char *type = NULL;
        bool payload = false;
        for (size_t k = 1; k < cn; k++) {
            const char *o = c[k].text;
            if ((strcmp(o, "-t") == 0 || strcmp(o, "--type") == 0) && k + 1 < cn) type = c[++k].text;
            else if (strncmp(o, "--type=", 7) == 0) type = o + 7;
            else if (strncmp(o, "-t", 2) == 0 && o[2]) type = o + 2;
            else if (strcmp(o, "-s") == 0 || strcmp(o, "--socket") == 0) k++;
            else if (o[0] != '-') payload = true;
        }
        if (!type || strncmp(type, "get_", 4) != 0 || payload) {
            Refuse(RULE_SCREEN, b, "only reading is allowed: `i3-msg -t get_tree` (get_workspaces, get_outputs, ...)");
        }
    } else if (strcmp(b, "xdotool") == 0 || strcmp(b, "wmctrl") == 0) {
        Refuse(RULE_SCREEN, b, "never move windows, focus or workspaces (CLAUDE.md); `i3-msg -t get_tree` reads the tree");
    } else if (strcmp(b, "bench-export-ab") == 0 || strcmp(b, "bench-export-ab.sh") == 0) {
        gBench = true;
    } else {
        // the stay-in-the-repo destinations
        if (strcmp(b, "cp") == 0 || strcmp(b, "mv") == 0) {
            static const char *const v[] = {"-S", NULL};  // --suffix's value
            CheckDestination(ex.cwd, b, c, cn, v, false);
        } else if (strcmp(b, "install") == 0) {
            static const char *const v[] = {"-m", "-o", "-g", "-S", "--mode", "--owner", "--group", "--suffix", NULL};
            bool dirs = HasFlag(c + 1, cn - 1, "--directory") || HasShort(c + 1, cn - 1, 'd');
            CheckDestination(ex.cwd, b, c, cn, v, dirs);
        } else if (strcmp(b, "tee") == 0 || strcmp(b, "mkdir") == 0 || strcmp(b, "touch") == 0) {
            for (size_t k = 1; k < cn; k++) {
                const char *o = c[k].text;
                if (c[k].literal && o[0] == '-' && o[1]) {
                    if (strcmp(o, "-m") == 0 || strcmp(o, "-d") == 0 || strcmp(o, "-r") == 0 || strcmp(o, "-t") == 0) {
                        k++;
                    }
                    continue;
                }
                CheckWritePath(ex.cwd, &c[k], b);
            }
        }
        // the plain examples open a window; their test builds (linux-test, linux-lsan) don't
        const char *abs = strchr(word, '/') ? Absolute(ex.cwd, word) : NULL;
        const char *where = abs ? abs : word;
        if (strstr(where, "examples/build/linux/") || strstr(where, "examples/build/win/") ||
            strstr(word, "examples/build/linux/") || strstr(word, "examples/build/win/")) {
            Refuse(RULE_WINDOW, word,
                   "the plain examples always open a window: run only their test build "
                   "(examples/build/linux-test/<example>/<program> --shot <file>)");
        } else if ((strstr(where, "examples/build/linux-test/") || strstr(where, "examples/build/linux-lsan/")) &&
                   HasFlag(c + 1, cn - 1, "--bench")) {
            if (!ex.vsync && !ctx->vsync && !EnvVsync()) {
                Refuse(RULE_BENCH, Fmt("%s --bench", word), "put __GL_SYNC_TO_VBLANK=0 in front of the command");
            }
            gBench = true;
        }
    }
}

// #endregion

// #region Walking the commands

// One simple command as the walk collects it: assignments before it, its words, its redirections.
typedef struct {
    struct {
        Word *items;
        size_t count, capacity;
    } words, assigns;
    struct {
        Token *items;
        size_t count, capacity;
    } redirs;
} Simple;

// Is this operator a pipe (| or |&)?
static bool IsPipeOp(const char *op)
{
    return strcmp(op, "|") == 0 || strcmp(op, "|&") == 0;
}

// Is this redirection a write to a file (not a descriptor)?
static bool IsWriteRedir(const Token *t)
{
    const char *op = t->op;
    if (t->fdTarget) return false;
    return strcmp(op, ">") == 0 || strcmp(op, ">>") == 0 || strcmp(op, ">|") == 0 || strcmp(op, "&>") == 0 ||
           strcmp(op, "&>>") == 0 || strcmp(op, "<>") == 0 || strcmp(op, ">&") == 0;
}

// Checks one simple command (its substitutions, assignments, redirections and the command), returning a
// cd's effect.
static void CheckSimple(Ctx *ctx, Simple *s, CdEffect *eff)
{
    // every substitution runs, whatever the command is
    for (size_t i = 0; i < s->words.count; i++) CheckSubs(ctx, &s->words.items[i]);
    for (size_t i = 0; i < s->assigns.count; i++) CheckSubs(ctx, &s->assigns.items[i]);
    for (size_t i = 0; i < s->redirs.count; i++) CheckSubs(ctx, &s->redirs.items[i].word);
    if (gRefusal) return;

    Extra ex = {.cwd = ctx->cwd, .top = true, .namesFrom = ctx->namesFrom};
    // NAME=value prefixes (or statements: then they hold for the rest of the line)
    for (size_t i = 0; i < s->assigns.count; i++) {
        CheckAssignment(StrC(&s->assigns.items[i].text), s->words.count ? &ex.vsync : &ctx->vsync);
    }
    // redirections: where it writes, and what its stdin is
    for (size_t i = 0; i < s->redirs.count; i++) {
        Token *t = &s->redirs.items[i];
        if (t->heredoc) {
            ex.stdinText = t->body;
            continue;
        }
        if (strcmp(t->op, "<<<") == 0) {
            ex.stdinText = StrC(&t->word.text);
            continue;
        }
        if (IsWriteRedir(t)) {
            Args target = ExpandWords(&t->word, 1);
            for (size_t k = 0; k < target.count; k++) CheckWritePath(ctx->cwd, &target.items[k], t->op);
        }
    }
    if (s->words.count == 0 || gRefusal) return;
    Args args = ExpandWords(s->words.items, s->words.count);
    CheckCommand(ctx, args.items, args.count, ex, eff);
}

// Walks the tokens of one command line: keywords, subshells, case patterns and function definitions are
// stepped over, every simple command is checked, and a cd changes the working directory of the commands
// after it (not in a pipeline or in the background; unknown after a cd that may have failed).
static void Walk(Ctx *ctx, Tokens *toks)
{
    Simple cur = {0};
    const char *opBefore = "";
    Strings cwdStack = {0};
    bool skipToSep = false, awaitIn = false, pattern = false, inTest = false, funcName = false,
         funcParens = false;
    int caseDepth = 0;

    for (size_t ti = 0; ti <= toks->count && !gRefusal; ti++) {
        Token *t = ti < toks->count ? &toks->items[ti] : NULL;
        const char *opAfter = t ? (t->type == T_OP ? t->op : NULL) : "";
        bool isWord = t && t->type == T_WORD;
        const char *text = isWord ? StrC(&t->word.text) : NULL;
        bool kw = isWord && !t->word.quoted && t->word.subs.count == 0;

        // the parts of compound commands that aren't commands
        if (pattern) {
            if (isWord) CheckSubs(ctx, &t->word);
            if (t && t->type == T_OP && strcmp(t->op, ")") == 0) pattern = false;
            if (kw && strcmp(text, "esac") == 0) {
                caseDepth--;
                pattern = false;
            }
            continue;
        }
        if (inTest) {
            if (isWord) CheckSubs(ctx, &t->word);
            if (kw && strcmp(text, "]]") == 0) inTest = false;
            continue;
        }
        if (awaitIn) {
            if (isWord) CheckSubs(ctx, &t->word);
            if (kw && strcmp(text, "in") == 0) {
                awaitIn = false;
                pattern = true;
            }
            continue;
        }
        if (skipToSep) {
            if (isWord) CheckSubs(ctx, &t->word);
            if (t && t->type == T_OP) skipToSep = false;  // the header ends at ; or a newline (then `do`)
            continue;
        }

        if (isWord) {
            bool commandPosition = cur.words.count == 0 && cur.assigns.count == 0 && cur.redirs.count == 0;
            if (funcName) {
                funcName = false;
                continue;
            }
            if (t->word.arith) {
                CheckSubs(ctx, &t->word);
                continue;
            }
            if (commandPosition && kw) {
                static const char *skip[] = {"if", "then", "else", "elif", "fi", "do", "done", "while", "until",
                                             "!", "{", "}", "coproc"};
                bool skipped = false;
                for (size_t k = 0; k < NOB_ARRAY_LEN(skip) && !skipped; k++) skipped = strcmp(text, skip[k]) == 0;
                if (skipped) continue;
                if (strcmp(text, "esac") == 0) {
                    if (caseDepth > 0) caseDepth--;
                    continue;
                }
                if (strcmp(text, "for") == 0 || strcmp(text, "select") == 0) {
                    skipToSep = true;
                    continue;
                }
                if (strcmp(text, "case") == 0) {
                    caseDepth++;
                    awaitIn = true;
                    continue;
                }
                if (strcmp(text, "function") == 0) {
                    funcName = true;
                    continue;
                }
                if (strcmp(text, "[[") == 0) {
                    inTest = true;
                    continue;
                }
            }
            if (cur.words.count == 0 && IsAssignment(&t->word)) PUSH(&cur.assigns, t->word);
            else PUSH(&cur.words, t->word);
            continue;
        }
        if (t && t->type == T_REDIR) {
            PUSH(&cur.redirs, *t);
            continue;
        }

        // an operator (or the end): ( ) groups, then the end of a simple command
        if (t && strcmp(t->op, "(") == 0) {
            if (cur.words.count == 1 && cur.assigns.count == 0 && cur.redirs.count == 0) {
                // name ( ) { body }: a function definition; its body is checked as if it ran
                cur.words.count = 0;
                funcParens = true;
                continue;
            }
            PUSH(&cwdStack, ctx->cwd);
            opBefore = "";
            continue;
        }
        if (t && strcmp(t->op, ")") == 0 && funcParens) {
            funcParens = false;
            continue;
        }

        // the end of a simple command: check it, then apply a cd
        bool empty = cur.words.count == 0 && cur.assigns.count == 0 && cur.redirs.count == 0;
        if (!empty) {
            bool pipe = IsPipeOp(opBefore) || IsPipeOp(opAfter);
            bool background = strcmp(opAfter, "&") == 0;
            CdEffect eff = {0};
            CheckSimple(ctx, &cur, &eff);
            bool chained = strcmp(opAfter, "&&") == 0;
            if (eff.set && !pipe && !background) {
                if (!eff.target) {
                    ctx->cwd = NULL;
                } else if (IsDir(eff.target)) {
                    ctx->cwd = eff.target;
                    ctx->cwdTentative = false;
                } else if (chained) {
                    ctx->cwd = eff.target;  // created earlier in the chain, perhaps: holds while the chain does
                    ctx->cwdTentative = true;
                } else {
                    ctx->cwd = NULL;
                }
                if (strcmp(opAfter, "||") == 0) ctx->cwd = NULL;
            } else if (ctx->cwdTentative && !chained && !pipe) {
                ctx->cwd = NULL;
                ctx->cwdTentative = false;
            }
            memset(&cur, 0, sizeof cur);
        }
        if (t && strcmp(t->op, ")") == 0) {
            if (cwdStack.count > 0) ctx->cwd = cwdStack.items[--cwdStack.count];
            ctx->cwdTentative = false;
        }
        if (t && (strcmp(t->op, ";;") == 0 || strcmp(t->op, ";&") == 0 || strcmp(t->op, ";;&") == 0) &&
            caseDepth > 0) {
            pattern = true;
        }
        opBefore = opAfter;
    }
}

// Checks one command line (the top one, or a nested -c string, eval text, substitution).
static void ParseText(Ctx *ctx, const char *text)
{
    if (gRefusal) return;
    if (ctx->depth > 32) {
        Refuse(RULE_READ, "nesting deeper than 32 levels", "write the command out without so many nested shells");
        return;
    }
    Lexer L = {.s = text, .n = strlen(text)};
    Tokens toks = {0};
    if (!LexList(&L, &toks, false)) {
        Refuse(RULE_READ, L.error ? L.error : "unreadable",
               "check the quoting: run-sh.o reads the command like bash and couldn't");
        return;
    }
    Walk(ctx, &toks);
}

// The decision for one command line, with `cwd` as its working directory: NULL when allowed, else the
// refusal line. Sets gBench when it runs a benchmark. A line that both makes a link and deletes is refused
// (anywhere in it, nested shells and eval included, in either order): `ln -s /home /tmp/zz; rm -rf
// /tmp/zz/x` resolves /tmp/zz/x before the link exists, so the path check can't see where it leads.
static const char *Decide(const char *command, const char *cwd)
{
    gRefusal = NULL;
    gBench = false;
    gMakesLink = NULL;
    gFirstDeletion = NULL;
    Ctx ctx = {.cwd = cwd && cwd[0] == '/' ? cwd : NULL};
    ParseText(&ctx, command);
    if (!gRefusal && gMakesLink && gFirstDeletion) {
        Refuse(RULE_DELETE, Fmt("%s with %s", gFirstDeletion, gMakesLink),
               "a link made in the same command could redirect the deletion; make the link and delete in "
               "separate commands");
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
    if (!IsDir(dir)) {
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

// run-sh.o -c '<command>' [args]: check, log, take the benchmark lock when needed, exec bash.
static int RunCommand(int argc, char **argv)
{
    const char *command = argv[2];
    char *cwd = ToolLogicalCwd();
    const char *where = cwd ? DupC(cwd) : NULL;
    free(cwd);
    const char *refusal = Decide(command, where);
    if (!refusal && gBench && !TakeBenchLock()) {
        refusal = "run-sh: refused: " RULE_BENCH ": --bench; another benchmark is running (one at a time, "
                  "CLAUDE.md): wait for it to end";
    }
    LogDecision("-c", command, where, refusal);
    if (refusal) {
        fprintf(stderr, "%s\n", refusal);
        FreeAll();
        return 126;
    }
    // bash -c '<command>' [args...]: the arguments after the command are $0, $1, ... as with bash -c
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "bash", "-c", command);
    for (int i = 3; i < argc; i++) nob_cmd_append(&cmd, argv[i]);
    FreeAll();
    ToolExec(&cmd);  // returns only on failure (it exits 127 / 126)
    return 126;
}

// run-sh.o --check '<command>': print the decision.
static int CheckOnly(const char *command)
{
    char *cwd = ToolLogicalCwd();
    const char *where = cwd ? DupC(cwd) : NULL;
    free(cwd);
    const char *refusal = Decide(command, where);
    LogDecision("--check", command, where, refusal);
    printf("%s\n", refusal ? refusal : "run-sh: allowed");
    FreeAll();
    return refusal ? 126 : 0;
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

// A JSON string at r->pos, decoded (\uXXXX with surrogate pairs to UTF-8). A NUL in it is refused: a
// command can't hold one.
static bool JString(JReader *r, const char **out)
{
    if (r->pos >= r->n || r->s[r->pos] != '"') return JFail(r, "a string was expected");
    r->pos++;
    Word w = {0};
    for (;;) {
        if (r->pos >= r->n) return JFail(r, "unterminated string");
        unsigned char c = (unsigned char)r->s[r->pos++];
        if (c == '"') break;
        if (c < 0x20) return JFail(r, "a control character in a string");
        if (c != '\\') {
            WAdd(&w, (char)c, K_QUOTED);
            continue;
        }
        if (r->pos >= r->n) return JFail(r, "unterminated string");
        char e = r->s[r->pos++];
        switch (e) {
        case '"': case '\\': case '/': WAdd(&w, e, K_QUOTED); break;
        case 'b': WAdd(&w, '\b', K_QUOTED); break;
        case 'f': WAdd(&w, '\f', K_QUOTED); break;
        case 'n': WAdd(&w, '\n', K_QUOTED); break;
        case 'r': WAdd(&w, '\r', K_QUOTED); break;
        case 't': WAdd(&w, '\t', K_QUOTED); break;
        case 'u': {
            Lexer h = {.s = r->s, .n = r->n, .pos = r->pos};
            unsigned long cp;
            if (ReadDigits(&h, 16, 4, &cp) != 4) return JFail(r, "a bad \\u escape");
            r->pos = h.pos;
            if (cp >= 0xD800 && cp < 0xDC00 && r->pos + 6 <= r->n && r->s[r->pos] == '\\' && r->s[r->pos + 1] == 'u') {
                h.pos = r->pos + 2;
                unsigned long lo;
                if (ReadDigits(&h, 16, 4, &lo) == 4 && lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    r->pos = h.pos;
                }
            }
            if (cp == 0) return JFail(r, "a NUL character in a string");
            AddUtf8(&w, cp);
            break;
        }
        default: return JFail(r, "a bad escape in a string");
        }
    }
    *out = StrC(&w.text);
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
    if (c == '{') {
        v->type = 'o';
        r->pos++;
        JSpace(r);
        if (r->pos < r->n && r->s[r->pos] == '}') {
            r->pos++;
        } else {
            for (;;) {
                JSpace(r);
                JMember m = {0};
                if (!JString(r, &m.key)) return false;
                JSpace(r);
                if (r->pos >= r->n || r->s[r->pos] != ':') return JFail(r, "':' was expected");
                r->pos++;
                if (!JRead(r, &m.value, depth + 1)) return false;
                PUSH(&v->members, m);
                JSpace(r);
                if (r->pos < r->n && r->s[r->pos] == ',') {
                    r->pos++;
                    continue;
                }
                if (r->pos < r->n && r->s[r->pos] == '}') {
                    r->pos++;
                    break;
                }
                return JFail(r, "',' or '}' was expected");
            }
        }
    } else if (c == '[') {
        v->type = 'a';
        r->pos++;
        JSpace(r);
        if (r->pos < r->n && r->s[r->pos] == ']') {
            r->pos++;
        } else {
            for (;;) {
                JValue item = {0};
                if (!JRead(r, &item, depth + 1)) return false;
                JSpace(r);
                if (r->pos < r->n && r->s[r->pos] == ',') {
                    r->pos++;
                    continue;
                }
                if (r->pos < r->n && r->s[r->pos] == ']') {
                    r->pos++;
                    break;
                }
                return JFail(r, "',' or ']' was expected");
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

// Is the command already exactly `<this run-sh.o> -c '<text>' [args]`: one simple command, no operators or
// redirections, its first word resolving to this program or to <repo>/build/run-sh.o?
static bool AlreadyWrapped(const char *command)
{
    Lexer L = {.s = command, .n = strlen(command)};
    Tokens toks = {0};
    if (!LexList(&L, &toks, false)) return false;
    size_t words = 0;
    for (size_t i = 0; i < toks.count; i++) {
        if (toks.items[i].type != T_WORD) return false;
        words++;
    }
    if (words < 3) return false;
    Word *w = &toks.items[0].word;
    const char *first = StrC(&w->text);
    for (size_t i = 0; i < w->kind.count; i++) {
        if (w->kind.items[i] == K_EXP) return false;
    }
    if (strcmp(StrC(&toks.items[1].word.text), "-c") != 0 || first[0] != '/') return false;
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
    // (gRootReal lives in the arena, which RunCommand / CheckOnly / Hook free at their end; nothing reads
    // it after that)

    if (argc >= 3 && strcmp(argv[1], "-c") == 0) return RunCommand(argc, argv);
    if (argc == 3 && strcmp(argv[1], "--check") == 0) return CheckOnly(argv[2]);
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
