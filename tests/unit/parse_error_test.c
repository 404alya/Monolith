// Error messages of the map-text parser (runtime/monolith_scene.c, region "Parser"): "<file>:<line>:
// <message>", built straight into the runtime's last error (MS_LastError), with no message buffer in
// between and no length limit. A bad token of any length (here up to 100 000 bytes) arrives whole and
// valid UTF-8; a successful load after a failure clears the message. Short messages keep their exact
// text. Built like the editor (the runtime with MONOLITH_EDITOR) with ASan + UBSan:
//   ./build.sh test parse_error          (builds it and runs it in build/parse-error-test/)
//   parse_error_test <scratch folder>
#include "monolith_scene.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, loads;
static char dir[1024];

// True if s is valid UTF-8 to its end (in particular: it doesn't end in half a character)
static bool WholeUtf8(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        int n = *p < 0x80 ? 1 : *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : *p >= 0xC0 ? 2 : 0;
        if (n == 0) return false;
        for (int k = 1; k < n; k++) if ((p[k] & 0xC0) != 0x80) return false;
        p += n;
    }
    return true;
}

// Writes `text` as <dir>/<name>.map; its path in *path
static void WriteMap(const char *name, const char *text, char *path, int pathSize)
{
    snprintf(path, pathSize, "%s/%s.map", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f || fputs(text, f) == EOF) { printf("FAIL cannot write %s\n", path); exit(1); }
    fclose(f);
}

// Loads the map (no GPU): true if it was refused, as every map here must be (the reason in
// MS_LastError())
static bool Refused(const char *path)
{
    loads++;
    MS_Scene *s = MS_Load(path, &(MS_LoadOptions){ .noGpu = true });
    if (s) { MS_Unload(s); return false; }
    return true;
}

// The map must be refused with exactly "<path>:<line>: <message>"
static void ExpectExact(const char *what, const char *text, int line, const char *message)
{
    char path[1100], want[2048];
    WriteMap(what, text, path, sizeof(path));
    snprintf(want, sizeof(want), "%s:%d: %s", path, line, message);
    if (!Refused(path)) { printf("FAIL %s: loaded\n", what); fails++; }
    else if (strcmp(MS_LastError(), want) != 0) { printf("FAIL %s:\n  got  %s\n  want %s\n", what, MS_LastError(), want); fails++; }
    else printf("ok   %s: %s\n", what, MS_LastError());
}

// An older map: refused at `line` with the message that names the map's own path (format N)
static void ExpectOlder(const char *what, const char *text, int line, int format)
{
    char path[1100], message[1400];
    snprintf(path, sizeof(path), "%s/%s.map", dir, what);
    snprintf(message, sizeof(message), "map format %d is older than this monolith (3): run /migrate-map %s in "
             "Claude Code (docs/map-format.md \xC2\xA7" "1.3)", format, path);
    ExpectExact(what, text, line, message);
}

// A map refused with exactly "<path>: <message>" (no line: a message about the whole file)
static void ExpectFile(const char *what, const char *text, const char *message)
{
    char path[1100], want[2048];
    WriteMap(what, text, path, sizeof(path));
    snprintf(want, sizeof(want), "%s: %s", path, message);
    if (!Refused(path)) { printf("FAIL %s: loaded\n", what); fails++; }
    else if (strcmp(MS_LastError(), want) != 0) { printf("FAIL %s:\n  got  %s\n  want %s\n", what, MS_LastError(), want); fails++; }
    else printf("ok   %s: %s\n", what, MS_LastError());
}

// A long message: whole (it starts with "<path>:<line>: ", contains `token` and ends with `tail`),
// valid UTF-8, the same on a second failing load
static void ExpectLong(const char *what, const char *text, int line, const char *token, const char *tail)
{
    char path[1100], head[1200];
    WriteMap(what, text, path, sizeof(path));
    snprintf(head, sizeof(head), "%s:%d: ", path, line);
    if (!Refused(path)) { printf("FAIL %s: loaded\n", what); fails++; return; }
    const char *full = MS_LastError();
    size_t len = strlen(full), tailLen = strlen(tail);
    if (strncmp(full, head, strlen(head)) != 0 || !strstr(full, token) || len < tailLen ||
        strcmp(full + len - tailLen, tail) != 0 || !WholeUtf8(full)) {
        printf("FAIL %s: the message isn't whole: %.2000s\n", what, full); fails++; return;
    }
    printf("ok   %s: whole (%zu bytes, the token %zu): %.*s…%s\n", what, len, strlen(token),
           (int)strlen(head) + 24, full, full + len - tailLen - 8);
    char *first = MS_StrDup(full);      // (full is valid only until the next MS_* call)
    if (!first) exit(1);
    if (!Refused(path) || strcmp(MS_LastError(), first) != 0) { printf("FAIL %s: a second load gave another message\n", what); fails++; }
    free(first);
}

// Writes `text` as a map that loads: the load must succeed and leave MS_LastError() ""
static void ExpectCleared(const char *what, const char *text)
{
    char path[1100];
    WriteMap(what, text, path, sizeof(path));
    bool hadError = MS_LastError()[0] != 0;
    loads++;
    MS_Scene *s = MS_Load(path, &(MS_LoadOptions){ .noGpu = true });
    if (!s) { printf("FAIL %s: refused: %s\n", what, MS_LastError()); fails++; return; }
    MS_Unload(s);
    if (!hadError) { printf("FAIL %s: no error before the load to clear\n", what); fails++; }
    else if (MS_LastError()[0]) { printf("FAIL %s: the last error is still \"%s\"\n", what, MS_LastError()); fails++; }
    else printf("ok   %s: loaded after a failure, MS_LastError() is \"\"\n", what);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: parse_error_test <scratch folder>\n"); return 2; }
    SetTraceLogLevel(LOG_ERROR);
    snprintf(dir, sizeof(dir), "%s", argv[1]);

    // Short messages: the exact text
    ExpectExact("short-number", "monolith 3\nbox 0 0 0 1 1 abc\n", 2, "bad number 'abc'");
    ExpectExact("short-keyword", "monolith 3\nenv fog.mode fuzzy\n", 2,
                "bad fog mode 'fuzzy' (expected disabled linear exp2 exp)");
    ExpectExact("short-format", "monolith 99\n", 1, "map format 99 is newer than this monolith (3): update monolith");

    // The format line: exactly the current format (3). Without one a file is format 1, reported at
    // its first statement; a file of only comments has nothing to migrate
    ExpectOlder("format-none", "# an old map\n\nbox 0 0 0 1 1 1\n", 3, 1);
    ExpectOlder("format-2", "monolith 2\nbox 0 0 0 1 1 1\n", 1, 2);
    ExpectExact("format-4", "# a newer map\nmonolith 4\nbox 0 0 0 1 1 1\n", 2,
                "map format 4 is newer than this monolith (3): update monolith");
    ExpectExact("format-3-at", "monolith 3\nrepeat 2 0 0 1 at 0 0 5 box 0 0 0 1 1 1\n", 2,
                "unknown keyword 'at' (expected group, box, block, stairs, waffle, model, light, material, use, env, "
                "paint, spawn, viewpoint, id or repeat)");
    ExpectFile("comments-only", "# nothing\n\n   # but comments\n", "map is empty (no boxes and no models)");

    // A bad token of 300 'é' (600 bytes) and 400 'x': longer than the parser's old message buffers
    char token[1200] = "", text[1400];
    for (int i = 0; i < 300; i++) strcat(token, "\xC3\xA9");
    for (int i = 0; i < 400; i++) strcat(token, "x");
    snprintf(text, sizeof(text), "monolith 3\nbox 0 0 0 1 1 %s\n", token);
    ExpectLong("long-number", text, 2, token, "x'");
    snprintf(text, sizeof(text), "monolith 3\n\nenv fog.mode %s\n", token);
    ExpectLong("long-keyword", text, 3, token, "' (expected disabled linear exp2 exp)");

    // No length limit at all: a token of 100 000 bytes
    size_t hugeLen = 100000;
    char *huge = malloc(hugeLen + 1), *hugeText = malloc(hugeLen + 64);
    if (!huge || !hugeText) exit(1);
    memset(huge, 'y', hugeLen);
    huge[hugeLen] = '\0';
    snprintf(hugeText, hugeLen + 64, "monolith 3\nbox 0 0 0 1 1 %s\n", huge);
    ExpectLong("huge-number", hugeText, 2, huge, "y'");
    free(hugeText);
    free(huge);

    // A success after the failures clears the last error
    ExpectCleared("loads", "monolith 3\nbox 0 0 0 1 1 1\nspawn 0 0 3 0\n");

    printf("%s: %d loads, %d failures\n", fails ? "FAILED" : "PASSED", loads, fails);
    MS_ClearError();
    return fails ? 1 : 0;
}
