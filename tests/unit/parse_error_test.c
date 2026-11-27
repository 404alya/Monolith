// Error messages of the map-text parser (runtime/monolith_scene.c, region "Parser"): "<file>:<line>:
// <message>", built straight into the runtime's last error (MS_LastError), with no message buffer in
// between and no length limit. A bad token of any length (here up to 100 000 bytes) arrives whole and
// valid UTF-8; a successful load after a failure clears the message. Short messages keep their exact
// text. The messages of imports (a map in several map-text files) name the file that holds the line;
// LiveEditCases checks the file list, the hashes and a live edit of an imported file. Built like the
// editor (the runtime with MONOLITH_EDITOR) with ASan + UBSan:
//   ./build.sh test parse_error          (builds it and runs it in build/parse-error-test/)
//   parse_error_test <scratch folder>
#include "monolith_scene.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

// Writes `text` as <dir>/<rel>, a map-text file another map imports (rel may name a subfolder of
// dir: it is made)
static void WritePart(const char *rel, const char *text)
{
    char path[1100];
    snprintf(path, sizeof(path), "%s/%s", dir, rel);
    char *slash = strrchr(path, '/');
    if (slash && strchr(rel, '/')) {
        *slash = '\0';
        mkdir(path, 0755);
        *slash = '/';
    }
    FILE *f = fopen(path, "wb");
    if (!f || fputs(text, f) == EOF) { printf("FAIL cannot write %s\n", path); exit(1); }
    fclose(f);
}

// A map with imports, refused with exactly "<dir>/<errRel>:<line>: <message>": the message names the
// map-text file that holds the line (errRel: package-relative; the main file is <what>.map)
static void ExpectIn(const char *what, const char *text, const char *errRel, int line, const char *message)
{
    char path[1100], want[4096];
    WriteMap(what, text, path, sizeof(path));
    snprintf(want, sizeof(want), "%s/%s:%d: %s", dir, errRel, line, message);
    if (!Refused(path)) { printf("FAIL %s: loaded\n", what); fails++; }
    else if (strcmp(MS_LastError(), want) != 0) { printf("FAIL %s:\n  got  %s\n  want %s\n", what, MS_LastError(), want); fails++; }
    else printf("ok   %s: %s\n", what, MS_LastError());
}

// FNV-1a 64 of n bytes, continuing from h
static unsigned long long Fnv(unsigned long long h, const void *data, size_t n)
{
    for (size_t i = 0; i < n; i++) h = (h ^ ((const unsigned char *)data)[i]) * 1099511628211ULL;
    return h;
}

// A check of the live-edit cases: prints ok / FAIL with what
static void Check(bool ok, const char *what)
{
    if (ok) printf("ok   %s\n", what);
    else { printf("FAIL %s (last error: %s)\n", what, MS_LastError()); fails++; }
}

// A map with an import, loaded and edited live (MS_EditSetText, noGpu): the file list, the lines of
// each file, the map text's hash (docs/code/monolith_scene.md, "Imports": path NUL size NUL bytes of
// each file), and an import taking the text given for its path (an error in it names the part's
// line; the scene stays as it was), else the file on disk
static void LiveEditCases(void)
{
    const char *mainText = "monolith 3\nbox 0 0 0 1 1 1\nimport \"parts/live.map\"\n";
    const char *partText = "monolith 3\nbox 2 0 0 3 1 1\n";
    char path[1100], want[2048];
    WritePart("parts/live.map", partText);
    WriteMap("live", mainText, path, sizeof(path));
    loads++;
    MS_Scene *s = MS_Load(path, &(MS_LoadOptions){ .noGpu = true });
    if (!s) { printf("FAIL live: refused: %s\n", MS_LastError()); fails++; return; }

    // The file list and the lines
    const MS_MapFile *mainFile = MS_MapFileAt(s, 0), *part = MS_MapFileAt(s, 1);
    Check(MS_MapFileCount(s) == 2 && mainFile && part && strcmp(mainFile->path, "live.map") == 0 &&
          strcmp(part->path, "parts/live.map") == 0 && part->importFile == 0 && part->importLine == 3 &&
          mainFile->importFile == -1 && MS_MapFileFind(s, "parts/live.map") == 1 && !MS_MapFileAt(s, 2),
          "live: two map-text files, the part imported at live.map:3");
    int len;
    const char *line = MS_LineText(s, 1, 2, &len);
    Check(line && len == 15 && memcmp(line, "box 2 0 0 3 1 1", 15) == 0 && !MS_LineText(s, 1, 3, &len) &&
          s->boxCount == 2 && s->boxRefs[1].file == 1 && s->boxRefs[1].line == 2 && s->boxRefs[0].file == 0,
          "live: the part's lines, its box at parts/live.map:2");
    unsigned long long h = 0xcbf29ce484222325ULL;
    const char *names[2] = { "live.map", "parts/live.map" }, *texts[2] = { mainText, partText };
    for (int i = 0; i < 2; i++) {
        char size[24];
        int n = snprintf(size, sizeof(size), "%zu", strlen(texts[i]));
        h = Fnv(h, names[i], strlen(names[i]) + 1);
        h = Fnv(h, size, (size_t)n + 1);
        h = Fnv(h, texts[i], strlen(texts[i]));
    }
    Check(s->hash == h && mainFile->hash == Fnv(0xcbf29ce484222325ULL, mainText, strlen(mainText)) &&
          part->hash == Fnv(0xcbf29ce484222325ULL, partText, strlen(partText)) && part->size == (long long)strlen(partText),
          "live: the combined hash and each file's own");
    bool watched = false;
    for (int i = 0; i < MS_DependencyCount(s); i++) {
        const char *dep = MS_Dependency(s, i);
        size_t n = strlen(dep);
        watched = watched || (n >= 14 && strcmp(dep + n - 14, "parts/live.map") == 0);
    }
    Check(watched, "live: the part is a dependency (hot reload watches it)");

    // A broken text given for the part: refused at the part's line, the scene unchanged
    const char *bad = "monolith 3\nbox 2 0 0 3 1 abc\n";
    MS_MapFileText files[2] = { { "live.map", mainText, (int)strlen(mainText) }, { "parts/live.map", bad, (int)strlen(bad) } };
    snprintf(want, sizeof(want), "%s/parts/live.map:2: bad number 'abc'", dir);
    Check(MS_EditSetText(s, files, 2, NULL) == MS_RELOAD_FAILED && strcmp(MS_LastError(), want) == 0 && s->boxCount == 2,
          "live: a broken part text is refused with the part's line");

    // A good one: the part's text is the given one (a box more), the hash follows it
    const char *more = "monolith 3\nbox 2 0 0 3 1 1\nbox 4 0 0 5 1 1\n";
    files[1].text = more;
    files[1].size = (int)strlen(more);
    int size;
    Check(MS_EditSetText(s, files, 2, NULL) == MS_RELOADED && s->boxCount == 3 && s->boxRefs[2].file == 1 &&
          s->boxRefs[2].line == 3 && strcmp(MS_MapText(s, 1, &size), more) == 0 && size == (int)strlen(more) &&
          MS_MapFileAt(s, 1)->hash == Fnv(0xcbf29ce484222325ULL, more, strlen(more)) && s->hash != h,
          "live: the text given for the part is parsed");

    // Only the main file's text: the part is read from disk again
    Check(MS_EditSetText(s, files, 1, NULL) == MS_RELOADED && s->boxCount == 2 && s->hash == h,
          "live: without a text for the part, the file on disk");
    MS_Unload(s);
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

    // The paint grid (docs/painting.md 17.1): 0 or 1..1024 texels per metre, on a material and map-wide
    ExpectExact("paint-grid-low", "monolith 3\nmaterial \"a\" paint_grid 0.5\nbox 0 0 0 1 1 1\n", 2,
                "paint_grid must be 0 (none) or 1..1024 texels per metre");
    ExpectExact("paint-grid-high", "monolith 3\nmaterial \"a\" paint_grid 2000\nbox 0 0 0 1 1 1\n", 2,
                "paint_grid must be 0 (none) or 1..1024 texels per metre");
    ExpectExact("paint-grid-none", "monolith 3\nmaterial \"a\" paint_grid\nbox 0 0 0 1 1 1\n", 2,
                "material key 'paint_grid' takes 1 number");
    ExpectExact("env-paint-grid-low", "monolith 3\nenv paint.grid 0.25\nbox 0 0 0 1 1 1\n", 2,
                "paint.grid must be 0 (none) or 1..1024 texels per metre");
    ExpectExact("env-paint-grid-high", "monolith 3\nenv paint.grid 1025\nbox 0 0 0 1 1 1\n", 2,
                "paint.grid must be >= 0 and <= 1024");
    ExpectExact("env-paint-grid-negative", "monolith 3\nenv paint.grid -1\nbox 0 0 0 1 1 1\n", 2,
                "paint.grid must be >= 0 and <= 1024");
    // A light's shadow biases: one number each, not negative
    ExpectExact("light-depth-bias-negative", "monolith 3\nlight point 0 1 0 shadow on depthBias -0.1\nbox 0 0 0 1 1 1\n", 2,
                "light key 'depthBias' must not be negative");
    ExpectExact("light-slope-bias-none", "monolith 3\nlight point 0 1 0 slopeBias shadow on\nbox 0 0 0 1 1 1\n", 2,
                "light key 'slopeBias' takes one number");

    // The format line: exactly the current format (3). Without one a file is format 1, reported at
    // its first statement; a file of only comments has nothing to migrate
    ExpectOlder("format-none", "# an old map\n\nbox 0 0 0 1 1 1\n", 3, 1);
    ExpectOlder("format-2", "monolith 2\nbox 0 0 0 1 1 1\n", 1, 2);
    ExpectExact("format-4", "# a newer map\nmonolith 4\nbox 0 0 0 1 1 1\n", 2,
                "map format 4 is newer than this monolith (3): update monolith");
    ExpectExact("format-3-at", "monolith 3\nrepeat 2 0 0 1 at 0 0 5 box 0 0 0 1 1 1\n", 2,
                "unknown keyword 'at' (expected group, box, block, stairs, waffle, barrier, shape, model, light, "
                "material, use, env, fogzone, paint, import, spawn, viewpoint, id or repeat)");
    ExpectFile("comments-only", "# nothing\n\n   # but comments\n", "map is empty (no boxes, models or shapes)");

    // Imports (docs/map-format.md 2.10): each file once (a second import names where it was first
    // imported; a cycle shows the chain of open files), inside the package, there, a .map file, one
    // quoted path; env / spawn / viewpoint only in the main file; each part with the current format
    // line (the migration named for the main file); an error in a part names the part and its line;
    // a duplicate id names both files' lines
    char msg[2048];
    WritePart("imp-a.map", "monolith 3\nbox 0 0 0 1 1 1\n");
    ExpectExact("import-twice", "monolith 3\nimport \"imp-a.map\"\nimport \"./imp-a.map\"\n", 3,
                "\"imp-a.map\" is already imported (at import-twice.map:2): a file is read once per map");
    ExpectExact("import-self", "monolith 3\nbox 0 0 0 1 1 1\nimport \"import-self.map\"\n", 3,
                "import cycle: \"import-self.map\" is already being read: import-self.map -> import-self.map");
    WritePart("cyc-a.map", "monolith 3\nimport \"cyc-b.map\"\n");
    WritePart("cyc-b.map", "monolith 3\nbox 0 0 0 1 1 1\nimport \"cyc-a.map\"\n");
    ExpectIn("import-cycle", "monolith 3\nimport \"cyc-a.map\"\n", "cyc-b.map", 3,
             "import cycle: \"cyc-a.map\" is already being read: cyc-a.map -> cyc-b.map -> cyc-a.map");
    ExpectExact("import-outside", "monolith 3\nimport \"../imp-a.map\"\n", 2,
                "path must stay inside the package: \"../imp-a.map\"");
    ExpectExact("import-missing", "monolith 3\nbox 0 0 0 1 1 1\nimport \"parts/nope.map\"\n", 3,
                "file not found: \"parts/nope.map\" (paths are relative to the package folder)");
    ExpectExact("import-not-map", "monolith 3\nimport \"textures/a.png\"\n", 2,
                "import: \"textures/a.png\" is not a map-text file (an imported file's name ends in .map)");
    ExpectExact("import-unquoted", "monolith 3\nimport imp-a.map\n", 2, "a path must be in double quotes: \"imp-a.map\"");
    ExpectExact("import-two-paths", "monolith 3\nimport \"imp-a.map\" \"cyc-a.map\"\n", 2,
                "import takes one path in double quotes: import \"parts/name.map\"");
    ExpectExact("import-none", "monolith 3\nimport\n", 2, "import takes one path in double quotes: import \"parts/name.map\"");
    ExpectExact("import-id-prefix", "monolith 3\nid 4 import \"imp-a.map\"\n", 2,
                "import takes no id or repeat prefix: import \"parts/name.map\"");
    ExpectExact("import-repeat-prefix", "monolith 3\nrepeat 3 1 0 0 import \"imp-a.map\"\n", 2,
                "import takes no id or repeat prefix: import \"parts/name.map\"");
    // Nesting cap: deep/d1.map imports deep/d2.map ... each one level deeper; the main file is
    // level 1, so deep/d256.map's import is the 257th open file
    for (int k = 1; k <= 257; k++) {
        char rel[64], text[128];
        snprintf(rel, sizeof(rel), "deep/d%d.map", k);
        snprintf(text, sizeof(text), "monolith 3\nbox 0 0 0 1 1 1\nimport \"deep/d%d.map\"\n", k + 1);
        WritePart(rel, text);
    }
    ExpectIn("import-too-deep", "monolith 3\nimport \"deep/d1.map\"\n", "deep/d255.map", 3,
             "imports nested more than 256 files deep");
    WritePart("parts/env.map", "monolith 3\nbox 0 0 0 1 1 1\nenv fog.density 0.01\n");
    WritePart("parts/spawn.map", "monolith 3\nspawn 0 0 3 0\n");
    WritePart("parts/view.map", "monolith 3\n\nviewpoint \"v\" 0 1 5 0 1 0\n");
    ExpectIn("import-env", "monolith 3\nimport \"parts/env.map\"\n", "parts/env.map", 3,
             "env lines belong in the main map file (import-env.map)");
    ExpectIn("import-spawn", "monolith 3\nbox 0 0 0 1 1 1\nimport \"parts/spawn.map\"\n", "parts/spawn.map", 2,
             "spawn lines belong in the main map file (import-spawn.map)");
    ExpectIn("import-viewpoint", "monolith 3\nbox 0 0 0 1 1 1\nimport \"parts/view.map\"\n", "parts/view.map", 3,
             "viewpoint lines belong in the main map file (import-viewpoint.map)");
    WritePart("parts/fogzone.map", "monolith 3\nfogzone 0 0 0 1 1 1 fade 5 fog.density 0.01\n");
    ExpectIn("import-fogzone", "monolith 3\nbox 0 0 0 1 1 1\nimport \"parts/fogzone.map\"\n", "parts/fogzone.map", 2,
             "fogzone lines belong in the main map file (import-fogzone.map)");
    // fogzone (docs/map-format.md 2.6): the fade, the keys and their values
    ExpectExact("fogzone-no-fade", "monolith 3\nbox 0 0 0 1 1 1\nfogzone 0 0 0 1 1 1 fog.density 0.01\n", 3,
                "fogzone takes a box, a fade and fog keys: fogzone x0 y0 z0 x1 y1 z1 fade 20 fog.density 0.015 "
                "fog.color 114 94 230");
    ExpectExact("fogzone-key", "monolith 3\nbox 0 0 0 1 1 1\nfogzone 0 0 0 1 1 1 fade 5 fog.mode exp\n", 3,
                "fogzone: unknown key 'fog.mode' (fog.density, fog.color, fog.start, fog.end, fog.skyAffect)");
    ExpectExact("fogzone-twice", "monolith 3\nbox 0 0 0 1 1 1\nfogzone 0 0 0 1 1 1 fade 5 fog.density 1 fog.density 2\n", 3,
                "fogzone: fog.density given twice");
    ExpectExact("fogzone-fade", "monolith 3\nbox 0 0 0 1 1 1\nfogzone 0 0 0 1 1 1 fade -1 fog.density 0.01\n", 3,
                "fogzone: fade must be 0 or more metres");
    ExpectExact("fogzone-density", "monolith 3\nbox 0 0 0 1 1 1\nfogzone 0 0 0 1 1 1 fade 5 fog.density -1\n", 3,
                "fog.density must be >= 0 and <= 1000000");
    // barrier (docs/map-format.md 2.2): two corners and an optional rot, like a box line
    ExpectExact("barrier-few", "monolith 3\nbox 0 0 0 1 1 1\nbarrier 0 0 0 1 1\n", 3,
                "barrier takes two corners: barrier x0 y0 z0 x1 y1 z1 [rot y p r]");
    ExpectExact("barrier-many", "monolith 3\nbox 0 0 0 1 1 1\nbarrier 0 0 0 1 1 1 2\n", 3,
                "barrier takes two corners: barrier x0 y0 z0 x1 y1 z1 [rot y p r]");
    ExpectExact("barrier-rot", "monolith 3\nbox 0 0 0 1 1 1\nbarrier 0 0 0 1 1 1 rot 30 0\n", 3,
                "rot takes 3 numbers: rot yawDeg pitchDeg rollDeg");
    ExpectExact("barrier-number", "monolith 3\nbox 0 0 0 1 1 1\nbarrier 0 0 0 1 x 1\n", 3, "bad number 'x'");
    ExpectExact("barrier-string", "monolith 3\nbox 0 0 0 1 1 1\nbarrier \"a\" 0 0 1 1 1\n", 3, "unexpected string \"a\"");
    ExpectExact("barrier-overflow", "monolith 3\nbox 0 0 0 1 1 1\nrepeat 2 3e38 0 0 barrier 3e38 0 0 3.1e38 1 1\n", 3,
                "coordinate or size out of range");
    ExpectFile("barrier-only", "monolith 3\nbarrier 0 0 0 1 1 1\n", "map is empty (no boxes, models or shapes)");
    // shape (docs/map-format.md 2, Shapes): a known kind and a block's six numbers, an optional rot;
    // sx, sz > 0, sy > 0 (a plane: 0); a paint line names parts of the shape's kind
    static const char SHAPE_USAGE[] = "shape takes a kind and a block's six numbers: shape cube|sphere|cylinder|cone|capsule|plane "
                                      "cx y0 cz sx sy sz [rot y p r]";
    ExpectExact("shape-kind", "monolith 3\nbox 0 0 0 1 1 1\nshape cubes 0 0 0 1 1 1\n", 3,
                "unknown shape kind 'cubes' (cube, sphere, cylinder, cone, capsule or plane)");
    ExpectExact("shape-none", "monolith 3\nbox 0 0 0 1 1 1\nshape\n", 3, SHAPE_USAGE);
    ExpectExact("shape-few", "monolith 3\nbox 0 0 0 1 1 1\nshape cube 0 0 0 1 1\n", 3, SHAPE_USAGE);
    ExpectExact("shape-many", "monolith 3\nbox 0 0 0 1 1 1\nshape cube 0 0 0 1 1 1 2\n", 3, SHAPE_USAGE);
    ExpectExact("shape-rot", "monolith 3\nbox 0 0 0 1 1 1\nshape cone 0 0 0 1 1 1 rot 30 0\n", 3,
                "rot takes 3 numbers: rot yawDeg pitchDeg rollDeg");
    ExpectExact("shape-number", "monolith 3\nbox 0 0 0 1 1 1\nshape sphere 0 0 0 1 x 1\n", 3, "bad number 'x'");
    ExpectExact("shape-string", "monolith 3\nbox 0 0 0 1 1 1\nshape \"cube\" 0 0 0 1 1 1\n", 3, "unexpected string \"cube\"");
    ExpectExact("shape-width", "monolith 3\nbox 0 0 0 1 1 1\nshape sphere 0 0 0 0 1 1\n", 3,
                "a shape's sizes sx and sz must be > 0");
    ExpectExact("shape-depth", "monolith 3\nbox 0 0 0 1 1 1\nshape capsule 0 0 0 1 1 -1\n", 3,
                "a shape's sizes sx and sz must be > 0");
    ExpectExact("shape-height", "monolith 3\nbox 0 0 0 1 1 1\nshape cylinder 0 0 0 1 0 1\n", 3,
                "a shape's height sy must be > 0 (only a plane has none: 0)");
    ExpectExact("shape-plane-height", "monolith 3\nbox 0 0 0 1 1 1\nshape plane 0 0 0 2 1 2\n", 3,
                "a plane has no height: write 0");
    ExpectExact("shape-overflow", "monolith 3\nbox 0 0 0 1 1 1\nrepeat 2 3e38 0 0 shape cube 3e38 0 0 1 1 1\n", 3,
                "coordinate or size out of range");
    WritePart("textures/p.png", "an atlas (only its name is checked here)\n");
    ExpectExact("shape-paint-part", "monolith 3\nmaterial \"p\" paint \"textures/p.png\"\nuse \"p\"\nid 5 shape sphere 0 0 0 1 1 1\n"
                "paint \"p\" 5:0 top 0 0 4 4\n", 5, "paint: a sphere has no part top (its parts: surface)");
    ExpectExact("shape-paint-cone", "monolith 3\nmaterial \"p\" paint \"textures/p.png\"\nuse \"p\"\nid 5 shape cone 0 0 0 1 1 1\n"
                "paint \"p\" 5:0 side,+x 0 0 4 4\n", 5, "paint: a cone has no part +x (its parts: side bottom)");
    ExpectExact("shape-paint-twice", "monolith 3\nmaterial \"p\" paint \"textures/p.png\"\nuse \"p\"\nid 5 shape cylinder 0 0 0 1 1 1\n"
                "paint \"p\" 5:0 side 0 0 4 4\npaint \"p\" 5:0 top,side 8 0 4 4\n", 6,
                "paint: 5:0: a face of it is already in line 5's entry of p");
    ExpectExact("box-paint-part", "monolith 3\nmaterial \"p\" paint \"textures/p.png\"\nuse \"p\"\nid 5 box 0 0 0 1 1 1\n"
                "paint \"p\" 5:0 side 0 0 4 4\n", 5, "paint: side is not a face of a box (faces +x -x +y -y +z -z)");
    ExpectExact("shape-paint-slot", "monolith 3\nmaterial \"p\" paint \"textures/p.png\"\nuse \"p\"\nid 5 shape cube 0 0 0 1 1 1\n"
                "paint \"p\" 5:0 slot 0 0 0 4 4\n", 5, "paint: line 4 (id 5) makes no models (a box is painted by faces)");
    WritePart("parts/old.map", "monolith 2\nbox 0 0 0 1 1 1\n");
    WritePart("parts/none.map", "# no format line\nbox 0 0 0 1 1 1\n");
    snprintf(msg, sizeof(msg), "map format 2 is older than this monolith (3): run /migrate-map %s/import-old.map in "
             "Claude Code (docs/map-format.md \xC2\xA7" "1.3)", dir);
    ExpectIn("import-old", "monolith 3\nimport \"parts/old.map\"\n", "parts/old.map", 1, msg);
    snprintf(msg, sizeof(msg), "map format 1 is older than this monolith (3): run /migrate-map %s/import-format-none.map in "
             "Claude Code (docs/map-format.md \xC2\xA7" "1.3)", dir);
    ExpectIn("import-format-none", "monolith 3\nimport \"parts/none.map\"\n", "parts/none.map", 2, msg);
    WritePart("parts/bad.map", "monolith 3\n# a part\nbox 0 0 0 1 1 abc\n");
    ExpectIn("import-error-in-part", "monolith 3\nbox 0 0 0 1 1 1\nimport \"parts/bad.map\"\nbox 0 0 0 1 1 1\n",
             "parts/bad.map", 3, "bad number 'abc'");
    WritePart("parts/dup.map", "monolith 3\n\nid 5 box 0 0 0 1 1 1\n");
    ExpectIn("import-duplicate-id", "monolith 3\nid 5 box 0 0 0 1 1 1\nimport \"parts/dup.map\"\n", "parts/dup.map", 3,
             "duplicate id 5 (also at import-duplicate-id.map:2)");
    ExpectExact("duplicate-id", "monolith 3\nid 5 box 0 0 0 1 1 1\nid 5 box 0 0 0 1 1 1\n", 3, "duplicate id 5 (also on line 2)");
    WritePart("parts/empty.map", "monolith 3\n# nothing\n");
    ExpectFile("import-empty", "monolith 3\nimport \"parts/empty.map\"\n", "map is empty (no boxes, models or shapes)");

    LiveEditCases();

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
