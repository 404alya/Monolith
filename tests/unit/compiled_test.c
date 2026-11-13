// Corruption test of the compiled-map reader (runtime/monolith_scene.c, region "Compiled maps";
// the layout is docs/compiled-map.md): truncated files, flipped bytes, random damage, huge counts,
// bad offsets, bad indices, floats, strings and paths. Every damaged file must be refused with a
// "file: message" error or load as a valid scene; nothing may crash. Built like a game (the
// runtime without MONOLITH_EDITOR) with ASan + UBSan, which also report leaks:
//   ./build.sh test compiled           (builds it, compiles tests/models and tests/rotated, runs it on both)
//   compiled_test <package folder>       (a copy of a package with its <name>.mapb in it)
// A package with turned boxes (box turns, section 17) gets the box-turn cases; one without them
// the cases that need tests/models' uniforms, lights and model paths.
#include "monolith_scene.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int fails, loads, accepted;
static char dir[1024], fuzzPath[1100];

// The header and section table fields this test changes (docs/compiled-map.md, section 2)
enum { H_VERSION = 8, H_SIZE = 12, H_ORDER = 16, H_FORMAT = 20, H_COUNT = 24, H_TABLE = 28, H_FILESIZE = 32 };
enum { S_STRINGS = 1, S_BOXES = 2, S_MATERIALS = 4, S_STATEMENTS = 13, S_ENV = 14, S_BOX_TURNS = 17, S_PAINT = 18, S_SECTIONS = 18 };
// A material record and its face layout, filter and padding bytes, its paint atlas path,
// paint filter and padding, noPrepass, drawDistance
enum { MAT_SIZE = 172, MAT_UV_FACES = 148, MAT_FILTER = 154, MAT_PAD = 155, MAT_PAINT = 156, MAT_PAINT_FILTER = 160, MAT_PAD2 = 161, MAT_NO_PREPASS = 164,
       MAT_DRAW_DISTANCE = 168 };

static uint32_t Rd32(const unsigned char *b, uint32_t at) { uint32_t v; memcpy(&v, b + at, 4); return v; }
static void Wr32(unsigned char *b, uint32_t at, uint32_t v) { memcpy(b + at, &v, 4); }

// The table entry of section `id`: its offset in the file (the entry's own position in *entry)
static uint32_t Section(const unsigned char *b, int id, uint32_t *entry)
{
    uint32_t table = Rd32(b, H_TABLE);
    for (int i = 0; i < S_SECTIONS; i++)
        if (Rd32(b, table + 16 * i) == (uint32_t)id) {
            if (entry) *entry = table + 16 * i;
            return Rd32(b, table + 16 * i + 4);
        }
    return 0;
}

// Writes the bytes as fuzz.mapb and loads it without the GPU: true if it loaded. A refused file
// leaves its reason in MS_LastError(); a loaded one leaves it "" (a success clears it).
static bool Load(const unsigned char *b, size_t size)
{
    FILE *f = fopen(fuzzPath, "wb");
    if (!f || fwrite(b, 1, size, f) != size) { printf("FAIL cannot write %s\n", fuzzPath); exit(1); }
    fclose(f);
    loads++;
    MS_Scene *s = MS_Load(fuzzPath, &(MS_LoadOptions){ .noGpu = true });
    if (!MS_LastError()) { printf("FAIL MS_LastError() is NULL\n"); fails++; return s != NULL; }
    if (!s && !MS_LastError()[0]) { printf("FAIL a refused file gave no message\n"); fails++; }
    if (s && MS_LastError()[0]) { printf("FAIL a loaded file left a message: %s\n", MS_LastError()); fails++; }
    if (s) {
        // A scene that loaded must be usable: touch what a game reads
        volatile float sum = 0.0f;
        for (int i = 0; i < s->boxCount; i++) sum += s->boxes[i].min.x;
        for (int i = 0; i < s->entityCount; i++) sum += (float)MS_FindEntity(s, s->entityIds[i]);
        (void)sum;
        accepted++;
        MS_Unload(s);
    }
    return s != NULL;
}

// A damaged copy that must be refused
static void ExpectRefused(const char *what, const unsigned char *b, size_t size)
{
    if (Load(b, size)) { printf("FAIL %s: loaded\n", what); fails++; }
    else printf("ok   %s: %s\n", what, MS_LastError());
}

// A damaged copy that must be refused with a message containing `want`
static void ExpectMessage(const char *what, const unsigned char *b, size_t size, const char *want)
{
    if (Load(b, size)) { printf("FAIL %s: loaded\n", what); fails++; }
    else if (!strstr(MS_LastError(), want)) {
        printf("FAIL %s: the message lacks \"%s\": %s\n", what, want, MS_LastError());
        fails++;
    } else printf("ok   %s: %s\n", what, MS_LastError());
}

static unsigned char *Copy(const unsigned char *b, size_t size)
{
    unsigned char *c = malloc(size);
    if (!c) exit(1);
    memcpy(c, b, size);
    return c;
}

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

// Messages longer than any fixed buffer (./build.sh test compiled makes the package "longname": a
// material whose texture lies three folders of 120 'é' deep). With the texture renamed away the load
// fails with "<file>: file not found: \"<~740 bytes>\" (…)", built in the runtime's last error: whole
// (no length limit, never cut, valid UTF-8), the same on a second failing load (nothing stale or
// appended), and cleared by the successful load once the texture is back.
static void MessageCases(const char *mapb)
{
    char tex[1400], away[1400];
    const char *E = "\xC3\xA9";     // é
    int n = snprintf(tex, sizeof(tex), "%s/textures/", dir);
    for (int f = 0; f < 3; f++) {
        for (int i = 0; i < 120; i++) n += snprintf(tex + n, sizeof(tex) - n, "%s", E);
        n += snprintf(tex + n, sizeof(tex) - n, "/");
    }
    snprintf(tex + n, sizeof(tex) - n, "tile.png");
    snprintf(away, sizeof(away), "%s.away", tex);
    if (rename(tex, away) != 0) { printf("FAIL cannot rename %s\n", tex); fails++; return; }

    // The whole message, longer than the 512 bytes callers used to give
    MS_Scene *s = MS_Load(mapb, &(MS_LoadOptions){ .noGpu = true });
    loads++;
    const char *full = MS_LastError();
    const char *tail = "(paths are relative to the package folder)";
    size_t len = strlen(full), tailLen = strlen(tail);
    if (s) { printf("FAIL loaded without its texture\n"); fails++; MS_Unload(s); }
    else if (strncmp(full, mapb, strlen(mapb)) != 0 || !strstr(full, tex + strlen(dir) + 1) || len <= 512 ||
             len < tailLen || strcmp(full + len - tailLen, tail) != 0 || !WholeUtf8(full)) {
        printf("FAIL the long message isn't whole: %s\n", full); fails++;
    } else printf("ok   long message whole (%zu bytes): %s\n", len, full);
    char *first = MS_StrDup(full);      // (full is valid only until the next MS_* call)
    if (!first) exit(1);

    // The same failing load again: the same message (a new one, nothing appended to the old)
    s = MS_Load(mapb, &(MS_LoadOptions){ .noGpu = true });
    loads++;
    if (s) { printf("FAIL loaded without its texture\n"); fails++; MS_Unload(s); }
    else if (strcmp(MS_LastError(), first) != 0) { printf("FAIL the second message differs: %s\n", MS_LastError()); fails++; }
    else printf("ok   the same message on a second failing load\n");
    free(first);

    // The texture back: the load succeeds and the last error is ""
    if (rename(away, tex) != 0) { printf("FAIL cannot rename %s back\n", away); fails++; return; }
    s = MS_Load(mapb, &(MS_LoadOptions){ .noGpu = true });
    loads++;
    if (!s) { printf("FAIL the package doesn't load with its texture: %s\n", MS_LastError()); fails++; }
    else if (MS_LastError()[0]) { printf("FAIL a success left the last error: %s\n", MS_LastError()); fails++; }
    else printf("ok   a successful load after the failure: MS_LastError() is \"\"\n");
    if (s) MS_Unload(s);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: compiled_test <package folder with <name>.mapb>\n"); return 2; }
    SetTraceLogLevel(LOG_ERROR);
    snprintf(dir, sizeof(dir), "%s", argv[1]);
    snprintf(fuzzPath, sizeof(fuzzPath), "%s/fuzz.mapb", dir);
    char path[1200];
    const char *base = strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir;
    snprintf(path, sizeof(path), "%s/%s.mapb", dir, base);
    if (strcmp(base, "longname") == 0) {
        MessageCases(path);
        printf("%s: %d loads, %d failures\n", fails ? "FAILED" : "PASSED", loads, fails);
        MS_ClearError();
        return fails ? 1 : 0;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *orig = malloc(size);
    if (!orig || fread(orig, 1, size, f) != size) return 1;
    fclose(f);

    // The undamaged file loads
    if (!Load(orig, size)) { printf("FAIL the undamaged file: %s\n", MS_LastError()); return 1; }
    printf("ok   undamaged file (%zu bytes) loads\n", size);

    // Every truncation is refused
    int truncOk = 0;
    for (size_t n = 0; n < size; n++) {
        if (Load(orig, n)) { printf("FAIL truncated to %zu bytes: loaded\n", n); fails++; }
        else truncOk++;
    }
    printf("ok   %d truncations refused\n", truncOk);

    // Every byte flipped, one at a time: refused or a valid scene, never a crash
    int before = accepted;
    for (size_t i = 0; i < size; i++) {
        unsigned char *c = Copy(orig, size);
        c[i] ^= 0xFF;
        Load(c, size);
        free(c);
    }
    printf("ok   %zu single-byte flips (%d loaded as valid scenes)\n", size, accepted - before);

    // Random damage: 1..8 bytes set to random values (fixed seed)
    uint32_t seed = 12345;
    before = accepted;
    for (int k = 0; k < 2000; k++) {
        unsigned char *c = Copy(orig, size);
        seed = seed * 1664525u + 1013904223u;
        int n = 1 + (int)(seed >> 29);
        for (int j = 0; j < n; j++) {
            seed = seed * 1664525u + 1013904223u;
            size_t at = (seed >> 8) % size;
            seed = seed * 1664525u + 1013904223u;
            c[at] = (unsigned char)(seed >> 24);
        }
        Load(c, size);
        free(c);
    }
    printf("ok   2000 random damages (%d loaded as valid scenes)\n", accepted - before);

    // Targeted damage that must be refused
    unsigned char *c;
    #define CASE(what, change) do { c = Copy(orig, size); change; ExpectRefused(what, c, size); free(c); } while (0)
    CASE("magic", c[1] = 'X');
    // A version this runtime doesn't read, older or newer: the message names the file's version and
    // the one version read (the reader holds no layout asserts: the version and the record sizes are
    // the guard)
    uint32_t version = Rd32(orig, H_VERSION);
    char want[160];
    snprintf(want, sizeof(want), "compiled map version %u; this runtime reads compiled map version %u: compile the map again",
             version + 1, version);
    c = Copy(orig, size); Wr32(c, H_VERSION, version + 1); ExpectMessage("version + 1", c, size, want); free(c);
    snprintf(want, sizeof(want), "compiled map version %u; this runtime reads compiled map version %u: compile the map again",
             version - 1, version);
    c = Copy(orig, size); Wr32(c, H_VERSION, version - 1); ExpectMessage("version - 1", c, size, want); free(c);
    // The map format it was compiled from: exactly the runtime's (MS_FORMAT_VERSION, 3)
    snprintf(want, sizeof(want), "compiled map of map format 2; this runtime reads map format %d: compile the map again",
             MS_FORMAT_VERSION);
    c = Copy(orig, size); Wr32(c, H_FORMAT, 2); ExpectMessage("map format 2", c, size, want); free(c);
    snprintf(want, sizeof(want), "compiled map of map format 4 is newer than this runtime (%d): update monolith", MS_FORMAT_VERSION);
    c = Copy(orig, size); Wr32(c, H_FORMAT, 4); ExpectMessage("map format 4", c, size, want); free(c);
    CASE("header size", Wr32(c, H_SIZE, 60));
    CASE("big-endian byte order", Wr32(c, H_ORDER, 0x04030201u));
    CASE("map format 99", Wr32(c, H_FORMAT, 99));
    CASE("file size + 1", Wr32(c, H_FILESIZE, Rd32(c, H_FILESIZE) + 1));
    CASE("section count 15", Wr32(c, H_COUNT, 15));
    CASE("section table past the end", Wr32(c, H_TABLE, (uint32_t)size));
    for (int id = 1; id <= S_SECTIONS; id++) {
        uint32_t e;
        Section(orig, id, &e);
        char what[96];
        snprintf(what, sizeof(what), "section %d: count 0xFFFFFFFF", id);
        CASE(what, Wr32(c, e + 8, 0xFFFFFFFFu));
        snprintf(what, sizeof(what), "section %d: count past the end", id);
        CASE(what, Wr32(c, e + 8, (uint32_t)size));
        snprintf(what, sizeof(what), "section %d: offset past the end", id);
        CASE(what, Wr32(c, e + 4, (uint32_t)size + 8));
        snprintf(what, sizeof(what), "section %d: offset 0xFFFFFFF0", id);
        CASE(what, Wr32(c, e + 4, 0xFFFFFFF0u));
        snprintf(what, sizeof(what), "section %d: offset misaligned", id);
        CASE(what, Wr32(c, e + 4, Rd32(c, e + 4) + 2));
        snprintf(what, sizeof(what), "section %d: record size + 4", id);
        CASE(what, Wr32(c, e + 12, Rd32(c, e + 12) + 4));
        snprintf(what, sizeof(what), "section %d: unknown id", id);
        CASE(what, Wr32(c, e, 99));
    }
    uint32_t e0, e1;
    Section(orig, 1, &e0);
    Section(orig, 2, &e1);
    CASE("section id twice", Wr32(c, e1, Rd32(c, e0)));

    uint32_t strE, strings = Section(orig, S_STRINGS, &strE), strCount = Rd32(orig, strE + 8);
    uint32_t mats = Section(orig, S_MATERIALS, NULL), boxes = Section(orig, S_BOXES, NULL);
    uint32_t stmts = Section(orig, S_STATEMENTS, NULL), env = Section(orig, S_ENV, NULL);
    CASE("string table without its final NUL", c[strings + strCount - 1] = 'x');
    CASE("control character in the string table", c[strings + strCount - 2] = 0x01);
    CASE("material name offset outside the string table", Wr32(c, mats, strCount));
    CASE("material name offset 0xFFFFFFFE", Wr32(c, mats, 0xFFFFFFFEu));
    CASE("material 0 not \"default\"", Wr32(c, mats, Rd32(c, mats + MAT_SIZE)));   // (material 1's name)
    // Face layouts (uv_faces) and the filter: only what the parser accepts
    static const unsigned char GAP[6] = { 0, 0, 3, 3, 3, 3 }, SIX[6] = { 6, 0, 1, 2, 3, 4 }, PART[6] = { 255, 0, 0, 0, 0, 0 },
                               ALL[6] = { 0, 1, 2, 3, 4, 5 };
    CASE("material uv_faces with a gap (0 0 3 3 3 3)", memcpy(c + mats + MAT_UV_FACES, GAP, 6));
    CASE("material uv_faces cell 6", memcpy(c + mats + MAT_UV_FACES, SIX, 6));
    CASE("material uv_faces partly none", memcpy(c + mats + MAT_UV_FACES, PART, 6));
    CASE("material filter 2", c[mats + MAT_FILTER] = 2);
    CASE("material padding 1", c[mats + MAT_PAD] = 1);
    // The paint atlas: a path inside the package that exists, paint_filter 0 / 1, padding 0
    CASE("material paint atlas offset outside the string table", Wr32(c, mats + MAT_PAINT, strCount));
    CASE("material paint atlas not a file", Wr32(c, mats + MAT_PAINT, Rd32(c, mats)));   // ("default")
    CASE("material paint_filter 2", c[mats + MAT_PAINT_FILTER] = 2);
    CASE("material padding 2 nonzero", c[mats + MAT_PAD2 + 2] = 1);
    CASE("material prepass flag 2", Wr32(c, mats + MAT_NO_PREPASS, 2));
    // The draw distance: 0 auto, -1 off, else above 0 and at most 1e6 m
    CASE("material draw distance -2", Wr32(c, mats + MAT_DRAW_DISTANCE, 0xC0000000u));
    CASE("material draw distance 2e6", Wr32(c, mats + MAT_DRAW_DISTANCE, 0x49F42400u));
    CASE("material draw distance NaN", Wr32(c, mats + MAT_DRAW_DISTANCE, 0x7FC00000u));
    // A layout needs uv_scale 1 1 (tests/models: material 2, "tiles", has 2 2); material 0 loads with one
    if (Rd32(orig, mats + 2 * MAT_SIZE + 80) != 0x3F800000u)       // (its uvScale.x is not 1)
        CASE("uv_faces on a material with uv_scale 2 2", memcpy(c + mats + 2 * MAT_SIZE + MAT_UV_FACES, ALL, 6));
    {
        unsigned char *ok = Copy(orig, size);
        memcpy(ok + mats + MAT_UV_FACES, ALL, 6);
        ok[mats + MAT_FILTER] = 1;
        if (!Load(ok, size)) { printf("FAIL uv_faces 0 1 2 3 4 5 and filter nearest on material 0: %s\n", MS_LastError()); fails++; }
        else printf("ok   uv_faces 0 1 2 3 4 5 and filter nearest on material 0 load\n");
        free(ok);
    }
    CASE("material cull mode 7", Wr32(c, mats + 100, 7));
    CASE("box material 9999", Wr32(c, boxes + 36, 9999));
    CASE("box statement -1", Wr32(c, boxes + 28, 0xFFFFFFFFu));
    CASE("box group 9999", Wr32(c, boxes + 32, 9999));
    CASE("box min.x NaN", Wr32(c, boxes, 0x7FC00000u));
    CASE("box min.x infinite", Wr32(c, boxes, 0x7F800000u));
    CASE("box min.x above max.x", Wr32(c, boxes, 0x7149F2CAu));     // (1e30)
    CASE("statement keys not unique", Wr32(c, stmts + 4, Rd32(c, stmts)));
    CASE("statement key 0", Wr32(c, stmts, 0));
    CASE("env key unknown", Wr32(c, env, Rd32(c, mats)));     // (the name "default")
    CASE("env value type wrong", Wr32(c, env + 4, Rd32(c, env + 4) + 1));
    uint32_t turnsE, turns = Section(orig, S_BOX_TURNS, &turnsE), turnCount = Rd32(orig, turnsE + 8);
    if (turnCount >= 2) {
        // Box turns: { u32 box; f32 rotationDeg[3]; }, sorted by box, each box once, finite, not 0 0 0
        uint32_t boxE;
        Section(orig, S_BOXES, &boxE);
        uint32_t boxCount = Rd32(orig, boxE + 8);
        uint32_t b0 = Rd32(orig, turns), b1 = Rd32(orig, turns + 16);
        CASE("box turn: box out of range", Wr32(c, turns + 16 * (turnCount - 1), boxCount));
        CASE("box turn: box 0xFFFFFFFF", Wr32(c, turns, 0xFFFFFFFFu));
        CASE("box turns unsorted", { Wr32(c, turns, b1); Wr32(c, turns + 16, b0); });
        CASE("box turned twice", Wr32(c, turns + 16, b0));
        CASE("more box turns than boxes", Wr32(c, turnsE + 8, boxCount + 1));
        CASE("box turn yaw NaN", Wr32(c, turns + 4, 0x7FC00000u));
        CASE("box turn pitch infinite", Wr32(c, turns + 8, 0x7F800000u));
        CASE("box turn roll -infinite", Wr32(c, turns + 12, 0xFF800000u));
        CASE("box turn 0 0 0", { for (int k = 1; k < 4; k++) Wr32(c, turns + 4 * k, 0); });
        CASE("box turn -0 0 -0", { Wr32(c, turns + 4, 0x80000000u); Wr32(c, turns + 8, 0); Wr32(c, turns + 12, 0x80000000u); });
        // A turned box whose centre overflows: its written box is flat at x = 3e38 (the box record
        // alone is valid; the turned record is not)
        CASE("turned box centre out of range", { Wr32(c, boxes + 48 * b0, 0x7F61B1E6u); Wr32(c, boxes + 48 * b0 + 12, 0x7F61B1E6u); });
        // Loaded as valid: the same turns with another (finite) angle
        unsigned char *ok = Copy(orig, size);
        Wr32(ok, turns + 4, 0x42340000u);    // (yaw 45)
        if (!Load(ok, size)) { printf("FAIL box turn yaw 45: %s\n", MS_LastError()); fails++; }
        else printf("ok   box turn yaw 45 loads\n");
        free(ok);
        printf("ok   %u box turns in the file\n", turnCount);
    } else if (turnCount == 0) {
        // Damage the parser could never produce (found by review): shared uniform ranges, light directions
        uint32_t uniE, lightE, lights = Section(orig, 7, &lightE);
        Section(orig, 5, &uniE);
        // (tests/models: material 3, "awning", has the uniforms; light 1 is the spot light)
        uint32_t awning = mats + 3 * MAT_SIZE, bark = mats + 4 * MAT_SIZE;
        if (Rd32(orig, uniE + 8) > 0 && Rd32(orig, awning + 136) > 0)
            CASE("two materials sharing one uniform range",
                 { Wr32(c, bark + 132, Rd32(c, awning + 132)); Wr32(c, bark + 136, Rd32(c, awning + 136)); });
        else { printf("FAIL the test package needs uniforms on material 3\n"); fails++; }
        if (Rd32(orig, lightE + 8) > 1) {
            CASE("point light with a direction", Wr32(c, lights + 16, 0x3F800000u));
            CASE("spot light direction scaled", Wr32(c, lights + 116 + 16, Rd32(c, lights + 116 + 16) ^ 0x01000000u));
            CASE("spot light direction zero", { for (int k = 0; k < 3; k++) Wr32(c, lights + 116 + 16 + 4 * k, 0); });
            CASE("light clip box flat", { Wr32(c, lights + 64, 1); for (int k = 0; k < 3; k++) { Wr32(c, lights + 68 + 4 * k, 0); Wr32(c, lights + 80 + 4 * k, 0); } });
        } else { printf("FAIL the test package needs two lights\n"); fails++; }
        // A path that leaves the package: the first "models/" in the string table becomes "../els/"
        unsigned char *m = memmem(orig + strings, strCount, "models/", 7);
        if (m) CASE("path with ..", memcpy(c + (m - orig), "../", 3));
        else { printf("FAIL no \"models/\" path in the string table\n"); fails++; }
    } else { printf("FAIL a package with turned boxes needs two of them\n"); fails++; }

    printf("%s: %d loads, %d valid scenes, %d failures\n", fails ? "FAILED" : "PASSED", loads, accepted, fails);
    free(orig);
    remove(fuzzPath);
    MS_ClearError();
    return fails ? 1 : 0;
}
