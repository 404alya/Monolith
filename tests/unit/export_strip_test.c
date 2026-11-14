// tests/unit/export_strip_test.c: the runtime an export ships has no map-text parser.
//
// An export (docs/export.md) carries runtime/monolith_scene.c and .h as a game has them: compiled maps only
// (docs/compiled-map.md, "The two builds"). So these files must hold none of the editor's parts:
//   - no tokenizer or statement parser,
//   - no model-file reference scan,
//   - no compiler,
//   - no number parsing (strtod / strtof / strtol / sscanf ...),
//   - no ".map" file lookup,
//   - no MONOLITH_EDITOR left.
// Each is a search of the two files for names or text; any hit fails the test.
//
// Replaces tests/unit/export_strip_test.sh (a grep per rule). Built and run by `./build.sh test export_strip`
// (nob.c, UNIT_TESTS): plain C and libc, with the unit tests' sanitizers.
//
// Arguments (nob.c passes the first two; anything after `--` on the command line follows them):
//   argv[1]  the repo root
//   argv[2]  the test's own folder (build/unit/export_strip), where it makes its export
//   argv[3]  optional: an existing export folder to check instead of making one
//
// Without argv[3] it exports tests/models with build/linux/monolith into <argv[2]>/export-<YYYYmmddHHMMSS>/
// (monolith's --export, hidden: no window), checks that, and moves the export to the trash afterwards
// (`gio trash`, the repo's only way to delete). With argv[3] it checks that folder and leaves it alone.
//
// Output (the script's, line for line): "ok   no <rule>" per rule that passed, "FAIL <rule>:" and the hits
// (path:line:text, indented four spaces) per rule that didn't, then "export_strip_test: OK (<dir>)" or
// "export_strip_test: FAILED (<dir>)".
//
// Exit codes: 0 all rules passed; 1 any hit, a missing file, a missing monolith binary or a failed export.
//
// Pattern matching is POSIX extended regular expressions (<regex.h>), as `grep -E` read them. A rule marked
// "whole word" is `grep -w`: a match counts only when it is not part of a longer word (the characters next to
// it are not letters, digits or '_'); that is done by wrapping the pattern in a boundary check, which finds
// the same lines grep -w does (it tries every match, not only the first).

#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// The files an export's runtime consists of, relative to the export folder, and the count of them.
#define FILE_COUNT 2
static const char *FILE_NAMES[FILE_COUNT] = {"monolith_scene.c", "monolith_scene.h"};

// The two files' full paths (heap strings, set in main).
static char *gFiles[FILE_COUNT];

// Whether any rule has failed so far (the script's FAIL variable).
static int gFail = 0;

// A heap copy of the formatted text; exits when memory is out (a test has nothing better to do).
static char *Format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *Format(const char *fmt, ...)
{
    char *text = NULL;
    va_list ap;
    va_start(ap, fmt);
    int n = vasprintf(&text, fmt, ap);
    va_end(ap);
    if (n < 0 || !text) {
        fprintf(stderr, "export_strip_test: out of memory\n");
        exit(1);
    }
    return text;
}

// The whole text of a file as a heap string (NUL-terminated), or NULL when it can't be read.
static char *ReadFile(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    char *text = malloc(cap);
    while (text) {
        size_t got = fread(text + len, 1, cap - len - 1, f);
        len += got;
        if (len + 1 < cap) break;  // a short read: end of file
        cap *= 2;
        char *bigger = realloc(text, cap);
        if (!bigger) {
            free(text);
            text = NULL;
        } else {
            text = bigger;
        }
    }
    fclose(f);
    if (text) text[len] = '\0';
    return text;
}

// One rule: searches both files for `pattern` (an ERE; whole-word when `word`) and reports as the script's
// Forbid function did: "ok   no <what>" when no line matches, else "FAIL <what>:" and every matching line as
// "    <path>:<line number>:<line>", files in order, lines in order.
static void Forbid(const char *what, const char *pattern, bool word)
{
    // the pattern to compile: for a whole-word rule, wrapped as <start or non-word><pattern><non-word or end>
    char *full = word ? Format("(^|[^[:alnum:]_])(%s)([^[:alnum:]_]|$)", pattern) : Format("%s", pattern);
    regex_t re;
    int rc = regcomp(&re, full, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
        char msg[256];
        regerror(rc, &re, msg, sizeof msg);
        fprintf(stderr, "export_strip_test: bad pattern for %s: %s\n", what, msg);
        exit(1);
    }

    // every line of both files, matched one at a time (a pattern never spans lines, as in grep)
    char *hits = NULL;
    size_t hitsLen = 0;
    for (int i = 0; i < FILE_COUNT; i++) {
        char *text = ReadFile(gFiles[i]);
        if (!text) continue;  // main() already checked that both files exist
        int number = 0;
        for (char *line = text; *line;) {
            char *end = strchr(line, '\n');
            size_t len = end ? (size_t)(end - line) : strlen(line);
            number++;
            char saved = line[len];
            line[len] = '\0';
            if (regexec(&re, line, 0, NULL, 0) == 0) {
                char *hit = Format("    %s:%d:%s\n", gFiles[i], number, line);
                size_t n = strlen(hit);
                hits = realloc(hits, hitsLen + n + 1);
                if (!hits) {
                    fprintf(stderr, "export_strip_test: out of memory\n");
                    exit(1);
                }
                memcpy(hits + hitsLen, hit, n + 1);
                hitsLen += n;
                free(hit);
            }
            line[len] = saved;
            line += len + (end ? 1 : 0);
        }
        free(text);
    }

    if (hits) {
        printf("FAIL %s:\n%s", what, hits);
        gFail = 1;
    } else {
        printf("ok   no %s\n", what);
    }
    free(hits);
    regfree(&re);
    free(full);
}

// `gio trash <path>`: the desktop trash is the repo's only way to delete (CLAUDE.md). Returns gio's exit
// status; its own error text is discarded (the caller says what to do by hand).
static int Trash(const char *path)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        // the child: stderr to /dev/null (`2>/dev/null`), then gio
        if (!freopen("/dev/null", "w", stderr)) _exit(126);
        execlp("gio", "gio", "trash", path, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "export_strip_test: usage: export_strip_test <repo root> <test folder> [export dir]\n");
        return 1;
    }
    const char *root = argv[1];
    const char *folder = argv[2];
    const char *dir = argc > 3 && argv[3][0] ? argv[3] : NULL;
    char *made = NULL;

    // Step 1: no folder given, so make an export of tests/models with the built monolith: build/unit/export_strip/
    // export-<timestamp>. monolith is run with --export (hidden window) and its output thrown away; a failure
    // only says "the export failed".
    if (!dir) {
        char *game = Format("%s/build/linux/monolith", root);
        if (access(game, X_OK) != 0) {
            fprintf(stderr, "export_strip_test: run ./build.sh first\n");
            return 1;
        }
        char stamp[32];
        time_t now = time(NULL);
        strftime(stamp, sizeof stamp, "%Y%m%d%H%M%S", localtime(&now));
        made = Format("%s/export-%s", folder, stamp);
        char *models = Format("%s/tests/models", root);
        fflush(NULL);
        pid_t pid = fork();
        if (pid == 0) {
            // the child: `>/dev/null 2>&1`, then monolith
            if (!freopen("/dev/null", "w", stdout) || !freopen("/dev/null", "w", stderr)) _exit(126);
            execl(game, game, "--export", made, "--map", models, (char *)NULL);
            _exit(127);
        }
        int status = 1;
        if (pid > 0) {
            while (waitpid(pid, &status, 0) < 0) {
            }
        }
        free(game);
        free(models);
        if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "export_strip_test: the export failed\n");
            return 1;
        }
        dir = made;
    }

    // Step 2: the two files must be there.
    for (int i = 0; i < FILE_COUNT; i++) {
        gFiles[i] = Format("%s/%s", dir, FILE_NAMES[i]);
        struct stat st;
        if (stat(gFiles[i], &st) != 0 || !S_ISREG(st.st_mode)) {
            fprintf(stderr, "export_strip_test: %s missing\n", gFiles[i]);
            return 1;
        }
    }

    // Step 3: the rules, in the script's order and with its patterns.
    Forbid("tokenizer / statement parser",
           "Tokenize|ParseText|ParseStatement|ParsePlacement|ParsePrimitive|ParseLight|ParseModel|ParseMaterial|"
           "ParseEnv|ParseValue|ParseFloat|ParseInt|ParseScene|AddStatement|Parser \\*",
           true);
    Forbid("model-file reference scan",
           "GltfUriDeps|ObjDeps|MtlDeps|MtlFileDeps|ModelDeps|ModelRef|RefResolve|JsonString|MS_CheckModelReferences",
           true);
    Forbid("compiler", "WriteCompiled|MS_Compile|CmWriter", true);
    Forbid("number parsing", "strto(d|f|l|ld|ul|ull|ll)|ato(f|i|l)|sscanf", true);
    Forbid("\".map\" file lookup", "\"\\.map\"", false);
    Forbid("MONOLITH_EDITOR", "MONOLITH_EDITOR", false);

    // Step 4: the verdict, and the export this run made goes to the trash.
    printf(gFail == 0 ? "export_strip_test: OK (%s)\n" : "export_strip_test: FAILED (%s)\n", dir);
    fflush(stdout);
    if (made && Trash(made) != 0) fprintf(stderr, "export_strip_test: move %s to the trash by hand\n", made);
    free(made);
    for (int i = 0; i < FILE_COUNT; i++) free(gFiles[i]);
    return gFail;
}
