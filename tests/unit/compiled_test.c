// Corruption test of the compiled-map reader (runtime/monolith_scene.c, region "Compiled maps";
// the layout is docs/compiled-map.md): truncated files, flipped bytes, random damage, huge counts,
// bad offsets, bad indices, floats, strings and paths. Every damaged file must be refused with a
// "file: message" error or load as a valid scene; nothing may crash. Built like a game (the
// runtime without MONOLITH_EDITOR) with ASan + UBSan, which also report leaks:
//   tests/unit/compiled_test.sh          (builds it, compiles tests/models, runs it)
//   compiled_test <package folder>       (a copy of a package with its <name>.mapb in it)
#include "monolith_scene.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int fails, loads, accepted;
static char dir[1024], fuzzPath[1100];

// The header and section table fields this test changes (docs/compiled-map.md, section 2)
enum { H_VERSION = 8, H_SIZE = 12, H_ORDER = 16, H_FORMAT = 20, H_COUNT = 24, H_TABLE = 28, H_FILESIZE = 32 };
enum { S_STRINGS = 1, S_BOXES = 2, S_MATERIALS = 4, S_STATEMENTS = 13, S_ENV = 14, S_SECTIONS = 16 };

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

// Writes the bytes as fuzz.mapb and loads it without the GPU: true if it loaded
static bool Load(const unsigned char *b, size_t size, char *err, int errSize)
{
    FILE *f = fopen(fuzzPath, "wb");
    if (!f || fwrite(b, 1, size, f) != size) { printf("FAIL cannot write %s\n", fuzzPath); exit(1); }
    fclose(f);
    loads++;
    err[0] = '\0';
    MS_Scene *s = MS_Load(fuzzPath, &(MS_LoadOptions){ .noGpu = true }, err, errSize);
    if (!s && !err[0]) { printf("FAIL a refused file gave no message\n"); fails++; }
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
    char err[512];
    if (Load(b, size, err, sizeof(err))) { printf("FAIL %s: loaded\n", what); fails++; }
    else printf("ok   %s: %s\n", what, err);
}

static unsigned char *Copy(const unsigned char *b, size_t size)
{
    unsigned char *c = malloc(size);
    if (!c) exit(1);
    memcpy(c, b, size);
    return c;
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
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *orig = malloc(size);
    if (!orig || fread(orig, 1, size, f) != size) return 1;
    fclose(f);
    char err[512];

    // The undamaged file loads
    if (!Load(orig, size, err, sizeof(err))) { printf("FAIL the undamaged file: %s\n", err); return 1; }
    printf("ok   undamaged file (%zu bytes) loads\n", size);

    // Every truncation is refused
    int truncOk = 0;
    for (size_t n = 0; n < size; n++) {
        if (Load(orig, n, err, sizeof(err))) { printf("FAIL truncated to %zu bytes: loaded\n", n); fails++; }
        else truncOk++;
    }
    printf("ok   %d truncations refused\n", truncOk);

    // Every byte flipped, one at a time: refused or a valid scene, never a crash
    int before = accepted;
    for (size_t i = 0; i < size; i++) {
        unsigned char *c = Copy(orig, size);
        c[i] ^= 0xFF;
        Load(c, size, err, sizeof(err));
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
        Load(c, size, err, sizeof(err));
        free(c);
    }
    printf("ok   2000 random damages (%d loaded as valid scenes)\n", accepted - before);

    // Targeted damage that must be refused
    unsigned char *c;
    #define CASE(what, change) do { c = Copy(orig, size); change; ExpectRefused(what, c, size); free(c); } while (0)
    CASE("magic", c[1] = 'X');
    CASE("format version", Wr32(c, H_VERSION, Rd32(c, H_VERSION) + 1));
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
    CASE("material 0 not \"default\"", Wr32(c, mats, Rd32(c, mats + 148)));   // (material 1's name)
    CASE("material cull mode 7", Wr32(c, mats + 100, 7));
    CASE("box material 9999", Wr32(c, boxes + 36, 9999));
    CASE("box statement -1", Wr32(c, boxes + 28, 0xFFFFFFFFu));
    CASE("box group 9999", Wr32(c, boxes + 32, 9999));
    CASE("box min.x NaN", Wr32(c, boxes, 0x7FC00000u));
    CASE("box min.x infinite", Wr32(c, boxes, 0x7F800000u));
    CASE("box min.x above max.x", Wr32(c, boxes, 0x7149F2CAu));     // (1e30)
    // Damage the parser could never produce (found by review): shared uniform ranges, light directions
    uint32_t uniE, lightE, lights = Section(orig, 7, &lightE);
    Section(orig, 5, &uniE);
    // (tests/models: material 3, "awning", has the uniforms; light 1 is the spot light)
    uint32_t awning = mats + 3 * 148, bark = mats + 4 * 148;
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
    CASE("statement keys not unique", Wr32(c, stmts + 4, Rd32(c, stmts)));
    CASE("statement key 0", Wr32(c, stmts, 0));
    CASE("env key unknown", Wr32(c, env, Rd32(c, mats)));     // (the name "default")
    CASE("env value type wrong", Wr32(c, env + 4, Rd32(c, env + 4) + 1));
    // A path that leaves the package: the first "models/" in the string table becomes "../els/"
    unsigned char *m = memmem(orig + strings, strCount, "models/", 7);
    if (m) CASE("path with ..", memcpy(c + (m - orig), "../", 3));
    else { printf("FAIL no \"models/\" path in the string table\n"); fails++; }

    printf("%s: %d loads, %d valid scenes, %d failures\n", fails ? "FAILED" : "PASSED", loads, accepted, fails);
    free(orig);
    remove(fuzzPath);
    return fails ? 1 : 0;
}
