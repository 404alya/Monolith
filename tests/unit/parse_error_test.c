// Error messages of the map-text parser (runtime/monolith_scene.c, region "Parser"): they are written
// straight into the caller's buffer, "<file>:<line>: <message>", with no message buffer in between.
// A bad token of any length arrives whole in a large buffer; in every smaller one the message is cut
// at its end, NUL-terminated, never in half a UTF-8 character and never past errSize. Short messages
// keep their exact text. Built like the editor (the runtime with MONOLITH_EDITOR) with ASan + UBSan:
//   tests/unit/parse_error_test.sh       (builds it and runs it in build/parse-error-test/)
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

// Loads the map (no GPU) into err[errSize]: true if it was refused, as every map here must be
static bool Refused(const char *path, char *err, int errSize)
{
    loads++;
    MS_Scene *s = MS_Load(path, &(MS_LoadOptions){ .noGpu = true }, err, errSize);
    if (s) { MS_Unload(s); return false; }
    return true;
}

// The map must be refused with exactly "<path>:<line>: <message>"
static void ExpectExact(const char *what, const char *text, int line, const char *message)
{
    char path[1100], want[2048], err[2048];
    WriteMap(what, text, path, sizeof(path));
    snprintf(want, sizeof(want), "%s:%d: %s", path, line, message);
    if (!Refused(path, err, sizeof(err))) { printf("FAIL %s: loaded\n", what); fails++; }
    else if (strcmp(err, want) != 0) { printf("FAIL %s:\n  got  %s\n  want %s\n", what, err, want); fails++; }
    else printf("ok   %s: %s\n", what, err);
}

// A long message: whole in a large buffer (it starts with "<path>:<line>: ", contains `token` and
// ends with `tail`), cleanly cut in every buffer of 1 .. its length + 1 bytes
static void ExpectLong(const char *what, const char *text, int line, const char *token, const char *tail)
{
    char path[1100], head[1200];
    WriteMap(what, text, path, sizeof(path));
    snprintf(head, sizeof(head), "%s:%d: ", path, line);
    static char full[16384];
    if (!Refused(path, full, sizeof(full))) { printf("FAIL %s: loaded\n", what); fails++; return; }
    size_t len = strlen(full), tailLen = strlen(tail);
    if (strncmp(full, head, strlen(head)) != 0 || !strstr(full, token) || len < tailLen ||
        strcmp(full + len - tailLen, tail) != 0 || !WholeUtf8(full)) {
        printf("FAIL %s: the message isn't whole: %s\n", what, full); fails++; return;
    }
    printf("ok   %s: whole (%zu bytes, the token %zu): %.*s…%s\n", what, len, strlen(token),
           (int)strlen(head) + 24, full, full + len - tailLen - 8);

    int trimmed = 0, bad = 0;
    char *buf = malloc(len + 64);
    if (!buf) exit(1);
    for (int size = 1; size <= (int)len + 1; size++) {
        memset(buf, 0x7F, len + 64);
        if (!Refused(path, buf, size)) { bad++; continue; }
        size_t got = strnlen(buf, (size_t)size), room = (size_t)size - 1;
        bool ok = got <= room && memcmp(buf, full, got) == 0 && WholeUtf8(buf) &&
                  (got == (room < len ? room : len) ||                          // as much as fits, or
                   (room - got <= 3 && (unsigned char)full[got] >= 0xC0)) &&    // back to a character's start
                  (unsigned char)buf[size] == 0x7F;                             // nothing past errSize
        if (!ok) {
            if (bad++ < 5) printf("FAIL %s: errSize %d: %zu bytes \"%.*s\"\n", what, size, got, (int)got, buf);
            continue;
        }
        if (got < room) trimmed++;
        if (size == 1 || size == 16) printf("ok   %s: errSize %d: \"%s\"\n", what, size, buf);
    }
    fails += bad;
    printf("%s   %s: errSize 1..%zu: every message a clean cut (%d cut back to a whole UTF-8 character)\n",
           bad ? "FAIL" : "ok", what, len + 1, trimmed);
    free(buf);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: parse_error_test <scratch folder>\n"); return 2; }
    SetTraceLogLevel(LOG_ERROR);
    snprintf(dir, sizeof(dir), "%s", argv[1]);

    // Short messages: the exact text
    ExpectExact("short-number", "monolith 2\nbox 0 0 0 1 1 abc\n", 2, "bad number 'abc'");
    ExpectExact("short-keyword", "monolith 2\nenv fog.mode fuzzy\n", 2,
                "bad fog mode 'fuzzy' (expected disabled linear exp2 exp)");
    ExpectExact("short-format", "monolith 99\n", 1,
                "map format 99 is newer than this runtime supports (2); update monolith_scene");

    // A bad token of 300 'é' (600 bytes) and 400 'x': longer than the parser's old message buffers
    char token[1200] = "", text[1400];
    for (int i = 0; i < 300; i++) strcat(token, "\xC3\xA9");
    for (int i = 0; i < 400; i++) strcat(token, "x");
    snprintf(text, sizeof(text), "monolith 2\nbox 0 0 0 1 1 %s\n", token);
    ExpectLong("long-number", text, 2, token, "x'");
    snprintf(text, sizeof(text), "monolith 2\n\nenv fog.mode %s\n", token);
    ExpectLong("long-keyword", text, 3, token, "' (expected disabled linear exp2 exp)");

    printf("%s: %d loads, %d failures\n", fails ? "FAILED" : "PASSED", loads, fails);
    return fails ? 1 : 0;
}
