// tools/library.c -> build/tools/library: the model catalog library/library.db (docs/model-library.md). It
// ingests the harvested and described .jsonl files, embeds the descriptions with the vendored llama.cpp
// (Qwen3-Embedding-0.6B, CPU only), searches by meaning and by keywords (three vector lists and an FTS5 list,
// fused by reciprocal rank), shows a row with its fetch command, pins a file hash, checks and dumps the
// database and measures search quality (eval). No network access. The only program that links SQLite,
// sqlite-vec and llama.cpp (src/vendor/, built by nob.c's LibraryVendor). Linux only, like every tool.
// tests/unit/library_test.c includes this file whole with LIBRARY_NO_MAIN and calls LibraryMain in-process.
// Reference: docs/code/library.md

#include "tool.h"

#include <math.h>
#include <stdint.h>

#include "sqlite3.h"
// sqlite-vec is linked into SQLite (built with SQLITE_CORE by nob.c), so its header takes sqlite3.h, not the
// loadable-extension header
#define SQLITE_CORE 1
#include "sqlite-vec.h"
#include "llama.h"

// #region Constants

#define LIB_DIMS 512           // dimensions kept: the first 512 of the model's 1024 (Matryoshka cut)
#define LIB_RRF_K 60           // reciprocal rank fusion: score += weight / (LIB_RRF_K + rank)
#define LIB_TOP 50             // rows kept per ranked list before fusion
#define LIB_THREADS 4          // llama.cpp's threads: fixed, so a text always gives the same vector
#define LIB_CTX 2048           // tokens per text: context, batch and micro-batch size (fixed, like the threads)
#define LIB_LIMIT 10           // search's default --limit
#define LIB_EMBED_COMMIT 64    // embed commits after this many rows, so a long run keeps its progress

// The embedding model the catalog is built with (docs/model-library.md §5; library/EMBEDDING.md). A new
// database records these in `meta`; the GGUF on disk is checked against meta.embed_sha256.
#define EMBED_MODEL "Qwen3-Embedding-0.6B"
#define EMBED_FILE "Qwen3-Embedding-0.6B-Q8_0.gguf"
#define EMBED_SHA256 "06507c7b42688469c4e7298b0a1e16deff06caf291cf0a5b278c308249c3e439"
#define EMBED_FETCH                                                                                           \
    "python3 -I tools/fetch_asset.py huggingface get Qwen/Qwen3-Embedding-0.6B-GGUF " EMBED_FILE                \
    " --expect-sha256 " EMBED_SHA256

// The vendored releases, recorded in `meta` (keep in step with src/vendor/*/README.md and nob.c's
// LLAMA_CPP_VERSION / LLAMA_CPP_COMMIT)
#define LLAMA_CPP_RELEASE "v1.0.0 b1b770d90d94cad9e9ea5b993bdbc383a522fb28"

// The three vector lists: their vec0 tables, the query instructions (the model card's format, stored in
// `meta` as instruct_shape / _style / _use), and with the keyword list their names in eval's output
static const char *VEC_TABLES[3] = {"vec_shape", "vec_style", "vec_use"};
static const char *INSTRUCT[3] = {
    "Given a description of an object's shape, retrieve 3D models with that shape",
    "Given a description of a geometry style, retrieve 3D models modelled in that style",
    "Given where and how an object is used in a level, retrieve 3D models that fit that use",
};

// The sources and licences a row may have (docs/model-library.md §2), and the style vocabulary (§4)
static const char *SOURCES[] = {"polyhaven", "kenney", "polyfork", "polypizza"};
#define LICENCE_CC0 "CC0 1.0"
#define LICENCE_CCBY "CC BY 4.0"    // Polyfork (the developer's choice), Poly Pizza where the API says so
#define LICENCE_CCBY3 "CC BY 3.0"   // Poly Pizza's other CC BY licence (docs/model-library.md §2)
static const char *STYLES[] = {"lowpoly", "stylised", "realistic", "photoscan", "voxel", "toon"};

// The colour, material and texture words a description must not contain (docs/model-library.md §4: the
// descriptions are about geometry and use; a map's material overrides replace colour and texture anyway).
// Matched as whole words, case-insensitive, in category, shape_desc, style_desc and use_desc. The one list:
// ingest and check use it through BannedWord, and tests/unit/library_test.c checks it the same way.
static const char *BANNED_WORDS[] = {
    // colours, and colour itself
    "amber", "aqua", "azure", "beige", "black", "blue", "bronze", "brown", "color", "colored", "colorful",
    "colors", "colour", "coloured", "colourful", "colours", "cream", "crimson", "cyan", "gold", "golden", "gray",
    "green", "grey", "indigo", "ivory", "khaki", "lavender", "lilac", "magenta", "maroon", "mauve", "navy", "olive",
    "orange", "pastel", "pink", "purple", "red", "scarlet", "silver", "teal", "turquoise", "violet", "white",
    "yellow",
    // materials
    "brass", "brick", "bricks", "cardboard", "ceramic", "chrome", "cloth", "concrete", "copper", "fabric", "glass",
    "granite", "iron", "leather", "marble", "metal", "metallic", "plaster", "plastic", "porcelain", "rubber",
    "steel", "stone", "stones", "timber", "wood", "wooden", "wicker",
    // surfaces and textures
    "dirty", "dusty", "emissive", "enamel", "gilded", "glossy", "glowing", "lacquered", "matte", "moss", "mossy",
    "muddy", "neon", "painted", "polished", "rust", "rusted", "rusty", "shiny", "texture", "textured", "textures",
    "translucent", "transparent", "varnished",
};

// The columns of `models` that a .jsonl line carries, in the order of the schema (docs/model-library.md §3);
// `embed_hash` and `indexed_at` follow them in the table and come only from this program. Types: 's' text,
// 'i' integer, 'b' 0/1 (a JSON boolean is taken too), 'r' real (a JSON integer is taken too). `required`: the
// key must be there with a non-empty value (NOT NULL in the schema).
typedef struct {
    const char *name;
    char type;
    bool required;
} Column;

static const Column COLUMNS[] = {
    {"id", 's', true},          {"source", 's', true},     {"name", 's', true},        {"collection", 's', false},
    {"page_url", 's', true},    {"file_url", 's', true},   {"path_in_zip", 's', false}, {"file_hash", 's', false},
    {"hash_kind", 's', false},  {"zip_sha256", 's', false}, {"licence", 's', true},    {"licence_url", 's', true},
    {"attribution", 's', false}, {"authors", 's', false},  {"rigged", 'b', false},     {"ik", 'b', false},
    {"category", 's', true},    {"style", 's', true},      {"style_desc", 's', true},  {"shape_desc", 's', true},
    {"use_desc", 's', true},    {"tags", 's', true},       {"tris", 'i', false},       {"sx", 'r', false},
    {"sy", 'r', false},         {"sz", 'r', false},        {"slots", 'i', false},      {"flat_shaded", 'b', false},
    {"textured", 'b', false},   {"factor_colours_only", 'b', false},
};
#define NCOLS (sizeof COLUMNS / sizeof *COLUMNS)

// Indexes into COLUMNS of the fields the code reads by name
enum {
    C_ID, C_SOURCE, C_NAME, C_COLLECTION, C_PAGE_URL, C_FILE_URL, C_PATH_IN_ZIP, C_FILE_HASH, C_HASH_KIND,
    C_ZIP_SHA256, C_LICENCE, C_LICENCE_URL, C_ATTRIBUTION, C_AUTHORS, C_RIGGED, C_IK, C_CATEGORY, C_STYLE,
    C_STYLE_DESC, C_SHAPE_DESC, C_USE_DESC, C_TAGS, C_TRIS, C_SX, C_SY, C_SZ, C_SLOTS,
};

// Keys a .jsonl line may carry that are not stored (docs/model-library.md §8): read and ignored
static const char *IGNORED_KEYS[] = {"preview", "source_tags"};

// The schema (docs/model-library.md §3), one statement each; `check` compares sqlite_master with these texts.
static const char *SCHEMA[] = {
    "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID",
    ("CREATE TABLE models (rowid INTEGER PRIMARY KEY, id TEXT NOT NULL UNIQUE, source TEXT NOT NULL, "
    "name TEXT NOT NULL, collection TEXT, page_url TEXT NOT NULL, file_url TEXT NOT NULL, path_in_zip TEXT, "
    "file_hash TEXT, hash_kind TEXT, zip_sha256 TEXT, licence TEXT NOT NULL, licence_url TEXT NOT NULL, "
    "attribution TEXT, authors TEXT, rigged INTEGER, ik INTEGER, category TEXT NOT NULL, style TEXT NOT NULL, "
    "style_desc TEXT NOT NULL, shape_desc TEXT NOT NULL, use_desc TEXT NOT NULL, tags TEXT NOT NULL, "
    "tris INTEGER, sx REAL, sy REAL, sz REAL, slots INTEGER, flat_shaded INTEGER, textured INTEGER, "
     "factor_colours_only INTEGER, embed_hash TEXT, indexed_at TEXT NOT NULL)"),
    "CREATE INDEX models_source ON models(source)",
    ("CREATE VIRTUAL TABLE models_fts USING fts5(category, tags, shape_desc, style_desc, use_desc, name, "
     "content='models', content_rowid='rowid', tokenize='unicode61')"),
    "CREATE VIRTUAL TABLE vec_shape USING vec0(embedding int8[512] distance_metric=cosine)",
    "CREATE VIRTUAL TABLE vec_style USING vec0(embedding int8[512] distance_metric=cosine)",
    "CREATE VIRTUAL TABLE vec_use USING vec0(embedding int8[512] distance_metric=cosine)",
};
// The names of the schema's objects, in SCHEMA's order
static const char *SCHEMA_NAMES[] = {"meta", "models", "models_source", "models_fts", "vec_shape", "vec_style", "vec_use"};

// #endregion

// #region Small helpers: strings, lists, output

// Where data goes (search lines, show, dump, eval): stdout, or a file the unit test reads. Messages
// ("library: ...") always go to stderr.
static FILE *gLibOut = NULL;

// The data stream (gLibOut, else stdout).
static FILE *Out(void)
{
    return gLibOut ? gLibOut : stdout;
}

// A heap array of heap strings.
typedef struct {
    char **items;
    size_t count, capacity;
} Strs;

// Frees every string and the array, and empties it.
static void StrsFree(Strs *s)
{
    for (size_t i = 0; i < s->count; i++) free(s->items[i]);
    free(s->items);
    *s = (Strs){0};
}

// printf into a new heap string (sized to fit; NULL only when out of memory).
NOB_PRINTF_FORMAT(1, 2) static char *Strf(const char *fmt, ...)
{
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    int n = vsnprintf(NULL, 0, fmt, a);
    va_end(a);
    char *s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (s) vsnprintf(s, (size_t)n + 1, fmt, b);
    va_end(b);
    return s;
}

// strdup that also takes NULL (and returns NULL for it).
static char *Dup(const char *s)
{
    return s ? strdup(s) : NULL;
}

// Appends a formatted message to a list of problems (heap strings).
NOB_PRINTF_FORMAT(2, 3) static void Problem(Strs *problems, const char *fmt, ...)
{
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    int n = vsnprintf(NULL, 0, fmt, a);
    va_end(a);
    char *s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (s) vsnprintf(s, (size_t)n + 1, fmt, b);
    va_end(b);
    if (s) nob_da_append(problems, s);
}

// Whether `s` is one of the `n` strings of `list`.
static bool InList(const char *s, const char **list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(s, list[i]) == 0) return true;
    }
    return false;
}

// Whether `s` is `n` lower-case hex digits and nothing else.
static bool IsHex(const char *s, size_t n)
{
    if (strlen(s) != n) return false;
    for (size_t i = 0; i < n; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    }
    return true;
}

// Seconds on a monotonic clock (for --timing).
static double Now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Whether `s` is a date written YYYY-MM-DD (digits in those places; the month 01-12, the day 01-31).
static bool IsDate(const char *s)
{
    if (strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
    for (int i = 0; i < 10; i++) {
        if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) return false;
    }
    int month = (s[5] - '0') * 10 + (s[6] - '0'), day = (s[8] - '0') * 10 + (s[9] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

// A word for the shell: as it is when it holds only characters a shell leaves alone, else in single quotes
// (a ' inside becomes '\''). Heap string.
static char *ShellWord(const char *s)
{
    bool plain = *s != '\0';
    for (const char *p = s; *p && plain; p++) {
        plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                strchr("_./:@%+=,-", *p) != NULL;
    }
    if (plain) return strdup(s);
    Nob_String_Builder sb = {0};
    nob_da_append(&sb, '\'');
    for (const char *p = s; *p; p++) {
        if (*p == '\'') nob_sb_append_cstr(&sb, "'\\''");
        else nob_da_append(&sb, *p);
    }
    nob_da_append(&sb, '\'');
    nob_sb_append_null(&sb);
    return sb.items;
}

// #endregion

// #region SHA-256 (embed_hash, the stub embedder, the GGUF check)

typedef struct {
    uint32_t h[8];
    uint64_t length;        // bytes hashed so far
    unsigned char block[64];
    size_t used;            // bytes waiting in `block`
} Sha256;

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

// The initial state (FIPS 180-4).
static void Sha256Init(Sha256 *s)
{
    static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, h0, sizeof h0);
    s->length = 0;
    s->used = 0;
}

// Rotate right.
static uint32_t Ror(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

// One 64-byte block into the state.
static void Sha256Block(Sha256 *s, const unsigned char *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25)) + ((e & f) ^ (~e & g)) + SHA256_K[i] + w[i];
        uint32_t t2 = (Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

// Hashes `n` more bytes.
static void Sha256Update(Sha256 *s, const void *data, size_t n)
{
    const unsigned char *p = data;
    s->length += n;
    while (n > 0) {
        size_t take = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used == 64) {
            Sha256Block(s, s->block);
            s->used = 0;
        }
    }
}

// The digest (32 bytes) after the padding.
static void Sha256Final(Sha256 *s, unsigned char out[32])
{
    uint64_t bits = s->length * 8;
    unsigned char pad = 0x80, zero = 0;
    Sha256Update(s, &pad, 1);
    while (s->used != 56) Sha256Update(s, &zero, 1);
    unsigned char len[8];
    for (int i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - 8 * i));
    Sha256Update(s, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (unsigned char)(s->h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(s->h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(s->h[i] >> 8);
        out[4 * i + 3] = (unsigned char)s->h[i];
    }
}

// The digest as 64 lower-case hex digits plus the terminator.
static void Sha256Hex(Sha256 *s, char out[65])
{
    unsigned char d[32];
    Sha256Final(s, d);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

// #endregion

// #region Rows: values, the texts that are embedded, the checks every row must pass

// One field's value: NULL (t = 0), text ('s', heap), integer ('i', also the 0/1 flags) or real ('r').
typedef struct {
    char t;
    char *s;
    long long i;
    double r;
} Value;

// One model: every column of COLUMNS, plus where it came from (a .jsonl line number, 0 for a database row)
// and, for a database row, its rowid and embed_hash.
typedef struct {
    Value v[NCOLS];
    size_t line;
    long long rowid;
    char *embedHash;
} Row;

typedef struct {
    Row *items;
    size_t count, capacity;
} Rows;

// The text of a field ("" when it isn't text: only for required text fields, which are always text).
static const char *Text(const Row *r, size_t c)
{
    return r->v[c].t == 's' ? r->v[c].s : "";
}

// Frees a row's strings.
static void RowFree(Row *r)
{
    for (size_t c = 0; c < NCOLS; c++) free(r->v[c].s);
    free(r->embedHash);
    *r = (Row){0};
}

// Frees every row and the array.
static void RowsFree(Rows *rows)
{
    for (size_t i = 0; i < rows->count; i++) RowFree(&rows->items[i]);
    free(rows->items);
    *rows = (Rows){0};
}

// Whether two values are the same (type and content; reals compared exactly).
static bool ValueEqual(const Value *a, const Value *b)
{
    if (a->t != b->t) return false;
    switch (a->t) {
    case 's': return strcmp(a->s, b->s) == 0;
    case 'i': return a->i == b->i;
    case 'r': return a->r == b->r;
    default: return true;
    }
}

// qsort order of rows: by id (byte order, as SQLite's BINARY collation sorts `ORDER BY id`).
static int CompareRowId(const void *a, const void *b)
{
    return strcmp(Text(a, C_ID), Text(b, C_ID));
}

// The first word of `text` that is in BANNED_WORDS (words: runs of ASCII letters, compared in lower case),
// or NULL. The one check of the colour / material / texture rule, for ingest, check and the unit test.
static const char *BannedWord(const char *text)
{
    const char *p = text;
    while (*p) {
        while (*p && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) p++;
        const char *start = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) p++;
        size_t n = (size_t)(p - start);
        if (n == 0) continue;
        for (size_t k = 0; k < NOB_ARRAY_LEN(BANNED_WORDS); k++) {
            const char *w = BANNED_WORDS[k];
            if (strlen(w) != n) continue;
            bool same = true;
            for (size_t j = 0; j < n && same; j++) {
                char c = start[j];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                same = c == w[j];
            }
            if (same) return w;
        }
    }
    return NULL;
}

// The three texts embedded for a row (docs/model-library.md §5), as heap strings:
//   shape "<category>: <name>. <shape_desc>", style "<style>. <style_desc>", use "<category>. <use_desc>"
static void EmbedTexts(const Row *r, char *texts[3])
{
    texts[0] = Strf("%s: %s. %s", Text(r, C_CATEGORY), Text(r, C_NAME), Text(r, C_SHAPE_DESC));
    texts[1] = Strf("%s. %s", Text(r, C_STYLE), Text(r, C_STYLE_DESC));
    texts[2] = Strf("%s. %s", Text(r, C_CATEGORY), Text(r, C_USE_DESC));
}

// embed_hash: the sha256 (hex) of the three texts joined by "\n\0" (two bytes).
static void EmbedHash(char *const texts[3], char out[65])
{
    Sha256 s;
    Sha256Init(&s);
    for (int k = 0; k < 3; k++) {
        if (k > 0) Sha256Update(&s, "\n\0", 2);
        Sha256Update(&s, texts[k], strlen(texts[k]));
    }
    Sha256Hex(&s, out);
}

// The embed_hash a row's texts give now.
static void RowEmbedHash(const Row *r, char out[65])
{
    char *texts[3];
    EmbedTexts(r, texts);
    EmbedHash(texts, out);
    for (int k = 0; k < 3; k++) free(texts[k]);
}

// Every rule a row must pass (docs/model-library.md §2-§4, §7 `ingest`), each failure appended to `problems`
// as "<where>: <id>: <what>". Used by ingest (on the file's lines) and check (on the database's rows).
static void ValidateRow(const Row *r, const char *where, Strs *problems)
{
    const char *id = r->v[C_ID].t == 's' ? r->v[C_ID].s : "?";
    // required fields: present, text, not empty
    for (size_t c = 0; c < NCOLS; c++) {
        if (COLUMNS[c].required && (r->v[c].t != 's' || r->v[c].s[0] == '\0')) {
            Problem(problems, "%s: %s: missing field %s", where, id, COLUMNS[c].name);
        }
    }
    // no control characters in any text (the dump is one `key: value` line per field)
    for (size_t c = 0; c < NCOLS; c++) {
        if (r->v[c].t != 's') continue;
        for (const unsigned char *p = (const unsigned char *)r->v[c].s; *p; p++) {
            if (*p < 0x20 || *p == 0x7f) {
                Problem(problems, "%s: %s: %s holds a control character", where, id, COLUMNS[c].name);
                break;
            }
        }
    }
    // the source, and the id's prefix
    const char *source = Text(r, C_SOURCE);
    if (*source && !InList(source, SOURCES, NOB_ARRAY_LEN(SOURCES))) {
        Problem(problems, "%s: %s: unknown source '%s' (polyhaven, kenney, polyfork or polypizza)", where, id, source);
    } else if (*source) {
        size_t n = strlen(source);
        if (strncmp(id, source, n) != 0 || id[n] != '-' || id[n + 1] == '\0') {
            Problem(problems, "%s: %s: the id must start with '%s-'", where, id, source);
        }
    }
    for (const char *p = id; *p; p++) {
        if (*p == ' ' || *p == '\t') {
            Problem(problems, "%s: %s: the id holds a blank", where, id);
            break;
        }
    }
    // the licence: CC0 1.0, or CC BY 4.0 / 3.0 with its attribution
    const char *licence = Text(r, C_LICENCE);
    bool ccby = strcmp(licence, LICENCE_CCBY) == 0 || strcmp(licence, LICENCE_CCBY3) == 0;
    if (*licence && strcmp(licence, LICENCE_CC0) != 0 && !ccby) {
        Problem(problems, "%s: %s: licence '%s' is not '" LICENCE_CC0 "', '" LICENCE_CCBY "' or '" LICENCE_CCBY3 "'",
                where, id, licence);
    }
    if (ccby && (r->v[C_ATTRIBUTION].t != 's' || r->v[C_ATTRIBUTION].s[0] == '\0')) {
        Problem(problems, "%s: %s: %s without attribution", where, id, licence);
    }
    // the three descriptions are text, not codes: at least two words each (a placeholder or a table key such as
    // "PV", which a description agent once wrote, would embed as noise and is refused)
    const size_t descs[] = {C_STYLE_DESC, C_SHAPE_DESC, C_USE_DESC};
    for (size_t k = 0; k < NOB_ARRAY_LEN(descs); k++) {
        const char *d = Text(r, descs[k]);
        int words = 0;
        for (const char *p = d; *p; p++) {
            if (*p != ' ' && (p == d || p[-1] == ' ')) words++;
        }
        if (*d && words < 2) {
            Problem(problems, "%s: %s: %s '%s' is not a description (fewer than two words)", where, id,
                    COLUMNS[descs[k]].name, d);
        }
    }
    // the style tag
    const char *style = Text(r, C_STYLE);
    if (*style && !InList(style, STYLES, NOB_ARRAY_LEN(STYLES))) {
        Problem(problems, "%s: %s: style '%s' is not one of lowpoly, stylised, realistic, photoscan, voxel, toon",
                where, id, style);
    }
    // the links
    const size_t urls[] = {C_PAGE_URL, C_FILE_URL, C_LICENCE_URL};
    for (size_t k = 0; k < NOB_ARRAY_LEN(urls); k++) {
        const char *u = Text(r, urls[k]);
        if (*u && strncmp(u, "https://", 8) != 0) {
            Problem(problems, "%s: %s: %s is not an https:// link", where, id, COLUMNS[urls[k]].name);
        }
    }
    // the hashes: file_hash with its kind (md5: 32 hex digits, sha256: 64), zip_sha256
    const Value *hash = &r->v[C_FILE_HASH], *kind = &r->v[C_HASH_KIND];
    if ((hash->t == 0) != (kind->t == 0)) {
        Problem(problems, "%s: %s: file_hash and hash_kind go together", where, id);
    } else if (hash->t == 's') {
        if (strcmp(kind->s, "md5") == 0) {
            if (!IsHex(hash->s, 32)) Problem(problems, "%s: %s: file_hash is not 32 lower-case hex digits", where, id);
        } else if (strcmp(kind->s, "sha256") == 0) {
            if (!IsHex(hash->s, 64)) Problem(problems, "%s: %s: file_hash is not 64 lower-case hex digits", where, id);
        } else {
            Problem(problems, "%s: %s: hash_kind '%s' is neither md5 nor sha256", where, id, kind->s);
        }
    }
    if (r->v[C_ZIP_SHA256].t == 's' && !IsHex(r->v[C_ZIP_SHA256].s, 64)) {
        Problem(problems, "%s: %s: zip_sha256 is not 64 lower-case hex digits", where, id);
    }
    if (strcmp(source, "kenney") == 0 && r->v[C_PATH_IN_ZIP].t != 's') {
        Problem(problems, "%s: %s: a kenney row needs path_in_zip", where, id);
    }
    // lower case where the schema says so
    const size_t lower[] = {C_CATEGORY, C_TAGS};
    for (size_t k = 0; k < NOB_ARRAY_LEN(lower); k++) {
        for (const char *p = Text(r, lower[k]); *p; p++) {
            if (*p >= 'A' && *p <= 'Z') {
                Problem(problems, "%s: %s: %s must be lower case", where, id, COLUMNS[lower[k]].name);
                break;
            }
        }
    }
    // numbers: counts and sizes not negative (and finite), the flags 0 or 1
    for (size_t c = 0; c < NCOLS; c++) {
        const Value *v = &r->v[c];
        if (v->t == 'i' && COLUMNS[c].type == 'b' && v->i != 0 && v->i != 1) {
            Problem(problems, "%s: %s: %s must be 0 or 1", where, id, COLUMNS[c].name);
        } else if (v->t == 'i' && v->i < 0) {
            Problem(problems, "%s: %s: %s is negative", where, id, COLUMNS[c].name);
        } else if (v->t == 'r' && (!isfinite(v->r) || v->r < 0)) {
            Problem(problems, "%s: %s: %s must be a size of 0 or more", where, id, COLUMNS[c].name);
        }
    }
    // the descriptions: geometry and use only (BANNED_WORDS)
    const size_t described[] = {C_CATEGORY, C_SHAPE_DESC, C_STYLE_DESC, C_USE_DESC};
    for (size_t k = 0; k < NOB_ARRAY_LEN(described); k++) {
        const char *w = BannedWord(Text(r, described[k]));
        if (w) {
            Problem(problems, "%s: %s: %s uses the colour or material word '%s'", where, id,
                    COLUMNS[described[k]].name, w);
        }
    }
}

// #endregion

// #region Reading .jsonl (through SQLite's JSON functions: no JSON parser of our own)

// The statements that take one JSON text apart, on an in-memory database.
typedef struct {
    sqlite3 *db;
    sqlite3_stmt *type;   // the JSON's top-level type, or NULL when it isn't valid JSON (RFC 8259)
    sqlite3_stmt *each;   // its members: key, type, atom
} Json;

// Opens the in-memory database and prepares the statements. 0 ok, 1 failed (said why).
static unsigned char JsonOpen(Json *j)
{
    *j = (Json){0};
    if (sqlite3_open(":memory:", &j->db) != SQLITE_OK ||
        sqlite3_prepare_v2(j->db, "SELECT CASE WHEN json_valid(?1) THEN json_type(?1) END", -1, &j->type, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(j->db, "SELECT key, type, atom, value FROM json_each(?1)", -1, &j->each, NULL) != SQLITE_OK) {
        ToolErr("sqlite: %s", j->db ? sqlite3_errmsg(j->db) : "out of memory");
        return 1;
    }
    return 0;
}

// Finalises the statements and closes the database.
static void JsonClose(Json *j)
{
    sqlite3_finalize(j->type);
    sqlite3_finalize(j->each);
    sqlite3_close(j->db);
    *j = (Json){0};
}

// The JSON type of `text` ("object", "array", "text", ...) as a heap string, or NULL when it isn't valid JSON.
static char *JsonType(Json *j, const char *text, size_t n)
{
    sqlite3_reset(j->type);
    sqlite3_bind_text(j->type, 1, text, (int)n, SQLITE_TRANSIENT);
    char *t = NULL;
    if (sqlite3_step(j->type) == SQLITE_ROW && sqlite3_column_type(j->type, 0) != SQLITE_NULL) {
        t = strdup((const char *)sqlite3_column_text(j->type, 0));
    }
    sqlite3_reset(j->type);
    return t;
}

// One .jsonl line into `row`: every key must be a column of COLUMNS (or one of IGNORED_KEYS), once, with a
// value of the column's type or null. Missing keys are NULL (ValidateRow then checks the required ones).
// Problems are appended as "line <n>: ..."; returns 0 when the line was read, 1 when it had problems.
static unsigned char ParseRow(Json *j, const char *text, size_t n, size_t lineNo, Row *row, Strs *problems)
{
    *row = (Row){.line = lineNo};
    size_t before = problems->count;
    char *type = JsonType(j, text, n);
    if (!type || strcmp(type, "object") != 0) {
        Problem(problems, "line %zu: not a JSON object", lineNo);
        free(type);
        return 1;
    }
    free(type);
    bool seen[NCOLS] = {0};
    sqlite3_stmt *st = j->each;
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, text, (int)n, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *key = (const char *)sqlite3_column_text(st, 0);
        const char *jt = (const char *)sqlite3_column_text(st, 1);
        if (!key || !jt) continue;
        if (InList(key, IGNORED_KEYS, NOB_ARRAY_LEN(IGNORED_KEYS))) continue;
        size_t c = 0;
        while (c < NCOLS && strcmp(COLUMNS[c].name, key) != 0) c++;
        if (c == NCOLS) {
            Problem(problems, "line %zu: unknown key '%s'", lineNo, key);
            continue;
        }
        if (seen[c]) {
            Problem(problems, "line %zu: key '%s' given twice", lineNo, key);
            continue;
        }
        seen[c] = true;
        Value *v = &row->v[c];
        char want = COLUMNS[c].type;
        if (strcmp(jt, "null") == 0) {
            v->t = 0;
        } else if (want == 's' && strcmp(jt, "text") == 0) {
            v->t = 's';
            v->s = strdup((const char *)sqlite3_column_text(st, 2));
        } else if ((want == 'i' || want == 'b') && strcmp(jt, "integer") == 0) {
            v->t = 'i';
            v->i = sqlite3_column_int64(st, 2);
        } else if (want == 'b' && (strcmp(jt, "true") == 0 || strcmp(jt, "false") == 0)) {
            v->t = 'i';
            v->i = strcmp(jt, "true") == 0;
        } else if (want == 'r' && (strcmp(jt, "integer") == 0 || strcmp(jt, "real") == 0)) {
            v->t = 'r';
            v->r = sqlite3_column_double(st, 2);
        } else {
            Problem(problems, "line %zu: %s: a %s where %s is wanted", lineNo, key, jt,
                    want == 's' ? "text" : want == 'r' ? "number" : want == 'b' ? "0 or 1" : "an integer");
        }
    }
    sqlite3_reset(st);
    return problems->count > before;
}

// Reads every line of a .jsonl file into `rows` (blank lines are skipped; a trailing \r is dropped) and
// checks each with ValidateRow; problems go to `problems`. 0 read, 1 the file can't be read.
static unsigned char ReadJsonl(const char *path, Rows *rows, Strs *problems)
{
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(path, &sb)) {
        ToolErr("can't read %s", path);
        return 1;
    }
    Json j;
    if (JsonOpen(&j) != 0) {
        nob_sb_free(sb);
        JsonClose(&j);
        return 1;
    }
    size_t lineNo = 0;
    for (size_t at = 0; at < sb.count;) {
        size_t end = at;
        while (end < sb.count && sb.items[end] != '\n') end++;
        lineNo++;
        size_t n = end - at;
        if (n > 0 && sb.items[at + n - 1] == '\r') n--;
        bool blank = true;
        for (size_t k = 0; k < n && blank; k++) blank = strchr(" \t", sb.items[at + k]) != NULL;
        if (!blank) {
            Row row;
            unsigned char bad = ParseRow(&j, sb.items + at, n, lineNo, &row, problems);
            if (bad == 0) {
                char *where = Strf("line %zu", lineNo);
                ValidateRow(&row, where ? where : "line", problems);
                free(where);
            }
            nob_da_append(rows, row);
        }
        at = end + 1;
    }
    JsonClose(&j);
    nob_sb_free(sb);
    return 0;
}

// #endregion

// #region The database: open, schema, meta, rows

// Prints SQLite's last error for `what` and returns 1.
static unsigned char SqlFail(sqlite3 *db, const char *what)
{
    ToolErr("%s: %s", what, sqlite3_errmsg(db));
    return 1;
}

// Runs SQL without results. 0 ok, 1 failed (said why).
static unsigned char Exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        ToolErr("sqlite: %s: %s", sql, err ? err : "error");
        sqlite3_free(err);
        return 1;
    }
    return 0;
}

// How a command opens the database.
enum { OPEN_READ, OPEN_WRITE, OPEN_CREATE };

// Opens the database at `path` with sqlite-vec registered. OPEN_CREATE creates it when missing or empty (an
// empty file is a new SQLite database; then *created is set); the others need it to exist. 0 ok, 1 failed
// (said why; *db is NULL).
static unsigned char OpenDb(const char *path, int mode, sqlite3 **db, bool *created)
{
    *db = NULL;
    struct stat st;
    bool exists = stat(path, &st) == 0 && st.st_size > 0;
    if (created) *created = !exists;
    if (!exists && mode != OPEN_CREATE) {
        ToolErr("no database at %s (build one with: build/tools/library ingest <file.jsonl>)", path);
        return 1;
    }
    int flags = mode == OPEN_READ ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | (mode == OPEN_CREATE ? SQLITE_OPEN_CREATE : 0);
    if (sqlite3_open_v2(path, db, flags, NULL) != SQLITE_OK) {
        ToolErr("can't open %s: %s", path, *db ? sqlite3_errmsg(*db) : "out of memory");
        sqlite3_close(*db);
        *db = NULL;
        return 1;
    }
    char *err = NULL;
    if (sqlite3_vec_init(*db, &err, NULL) != SQLITE_OK) {
        ToolErr("sqlite-vec: %s", err ? err : "can't register");
        sqlite3_free(err);
        sqlite3_close(*db);
        *db = NULL;
        return 1;
    }
    sqlite3_busy_timeout(*db, 5000);
    return 0;
}

// A `meta` value as a heap string, NULL when the key is missing.
static char *MetaGet(sqlite3 *db, const char *key)
{
    sqlite3_stmt *st = NULL;
    char *value = NULL;
    if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = ?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) value = Dup((const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    return value;
}

// Sets a `meta` value. 0 ok, 1 failed.
static unsigned char MetaSet(sqlite3 *db, const char *key, const char *value)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO meta(key, value) VALUES(?1, ?2) ON CONFLICT(key) DO UPDATE SET value = ?2",
                           -1, &st, NULL) != SQLITE_OK) {
        return SqlFail(db, "meta");
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : SqlFail(db, "meta");
}

// The `meta` keys and values a new database starts with (docs/model-library.md §3). `embedder` is added by
// the first embed (llama or stub: which program made the vectors).
static const char *META_INITIAL[][2] = {
    {"schema_version", "1"},
    {"embed_model", EMBED_MODEL},
    {"embed_file", EMBED_FILE},
    {"embed_sha256", EMBED_SHA256},
    {"embed_quant", "Q8_0"},
    {"embed_pooling", "last"},
    {"dims", "512"},
    {"vector_format", "int8"},
    {"quantisation", "maxabs-127"},
    {"instruct_shape", "Given a description of an object's shape, retrieve 3D models with that shape"},
    {"instruct_style", "Given a description of a geometry style, retrieve 3D models modelled in that style"},
    {"instruct_use", "Given where and how an object is used in a level, retrieve 3D models that fit that use"},
    {"llama_cpp", LLAMA_CPP_RELEASE},
    {"sqlite_vec", SQLITE_VEC_VERSION},
};

// Creates the tables of a new database and fills `meta` (inside the caller's transaction). 0 ok, 1 failed.
static unsigned char CreateSchema(sqlite3 *db)
{
    for (size_t i = 0; i < NOB_ARRAY_LEN(SCHEMA); i++) {
        if (Exec(db, SCHEMA[i]) != 0) return 1;
    }
    for (size_t i = 0; i < NOB_ARRAY_LEN(META_INITIAL); i++) {
        if (MetaSet(db, META_INITIAL[i][0], META_INITIAL[i][1]) != 0) return 1;
    }
    return 0;
}

// The column list of COLUMNS ("id, source, ..."), as a heap string.
static char *ColumnList(void)
{
    Nob_String_Builder sb = {0};
    for (size_t c = 0; c < NCOLS; c++) {
        if (c > 0) nob_sb_append_cstr(&sb, ", ");
        nob_sb_append_cstr(&sb, COLUMNS[c].name);
    }
    nob_sb_append_null(&sb);
    return sb.items;
}

// Reads the columns of COLUMNS from a result row, starting at result column `first`, into `r`.
static void ReadValues(sqlite3_stmt *st, int first, Row *r)
{
    for (size_t c = 0; c < NCOLS; c++) {
        int k = first + (int)c;
        Value *v = &r->v[c];
        *v = (Value){0};
        if (sqlite3_column_type(st, k) == SQLITE_NULL) continue;
        switch (COLUMNS[c].type) {
        case 's':
            v->t = 's';
            v->s = strdup((const char *)sqlite3_column_text(st, k));
            break;
        case 'r':
            v->t = 'r';
            v->r = sqlite3_column_double(st, k);
            break;
        default:
            v->t = 'i';
            v->i = sqlite3_column_int64(st, k);
            break;
        }
    }
}

// Binds the columns of COLUMNS as parameters `first`, first + 1, ...
static void BindValues(sqlite3_stmt *st, int first, const Row *r)
{
    for (size_t c = 0; c < NCOLS; c++) {
        int k = first + (int)c;
        const Value *v = &r->v[c];
        switch (v->t) {
        case 's': sqlite3_bind_text(st, k, v->s, -1, SQLITE_TRANSIENT); break;
        case 'i': sqlite3_bind_int64(st, k, v->i); break;
        case 'r': sqlite3_bind_double(st, k, v->r); break;
        default: sqlite3_bind_null(st, k); break;
        }
    }
}

// Every row of `models`, ordered by id, with rowid and embed_hash. 0 ok, 1 failed.
static unsigned char LoadRows(sqlite3 *db, Rows *rows)
{
    char *cols = ColumnList();
    char *sql = Strf("SELECT rowid, embed_hash, %s FROM models ORDER BY id", cols);
    free(cols);
    sqlite3_stmt *st = NULL;
    unsigned char rc = 0;
    if (!sql || sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        rc = SqlFail(db, "read models");
    } else {
        while (sqlite3_step(st) == SQLITE_ROW) {
            Row r = {0};
            r.rowid = sqlite3_column_int64(st, 0);
            r.embedHash = Dup((const char *)sqlite3_column_text(st, 1));
            ReadValues(st, 2, &r);
            nob_da_append(rows, r);
        }
    }
    sqlite3_finalize(st);
    free(sql);
    return rc;
}

// Deletes a row's three vectors (rows without vectors are left as they are). 0 ok, 1 failed.
static unsigned char DeleteVectors(sqlite3 *db, long long rowid)
{
    for (int k = 0; k < 3; k++) {
        char *sql = Strf("DELETE FROM %s WHERE rowid = ?1", VEC_TABLES[k]);
        sqlite3_stmt *st = NULL;
        int rc = sql ? sqlite3_prepare_v2(db, sql, -1, &st, NULL) : SQLITE_NOMEM;
        free(sql);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, rowid);
            rc = sqlite3_step(st);
        }
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return SqlFail(db, "delete vectors");
    }
    return 0;
}

// #endregion

// #region Embeddings: the stub, llama.cpp, the GGUF check, int8 quantisation

// The embedder of a run: the stub (--embedder stub: vectors from a SHA-256 stream of the text, for tests
// without the model) or llama.cpp with the GGUF, loaded on first use.
typedef struct {
    bool stub;
    bool loaded;
    struct llama_model *model;
    struct llama_context *ctx;
    const struct llama_vocab *vocab;
    llama_token endOfText;   // <|endoftext|>: the last token of every text (last-token pooling)
    int nEmbd;
    double loadSeconds;      // the model load (for --timing)
    int eosByTokenizer;      // -1 not seen yet, 1 the tokenizer appends <|endoftext|> itself, 0 we append it
} Embedder;

// llama.cpp's log, through to stderr only for errors (the rest is chatter); a continued line keeps the level
// of the line it continues.
static void LlamaLog(enum ggml_log_level level, const char *text, void *user)
{
    int *last = user;
    if (level != GGML_LOG_LEVEL_CONT) *last = (int)level;
    if (*last == GGML_LOG_LEVEL_ERROR) fputs(text, stderr);
}

// The level of the last llama.cpp log line (LlamaLog's user pointer).
static int gLlamaLogLevel = 0;

// L2-normalises the first LIB_DIMS of `x` and quantises them to int8 per vector: q = round(127 * x / max|x|)
// (docs/model-library.md §5, meta.quantisation 'maxabs-127'). An all-zero vector stays zero.
static void Quantise(const float *x, signed char out[LIB_DIMS])
{
    double norm = 0;
    for (int i = 0; i < LIB_DIMS; i++) norm += (double)x[i] * x[i];
    norm = sqrt(norm);
    float y[LIB_DIMS];
    float maxAbs = 0;
    for (int i = 0; i < LIB_DIMS; i++) {
        y[i] = norm > 0 ? (float)(x[i] / norm) : 0.0f;
        if (fabsf(y[i]) > maxAbs) maxAbs = fabsf(y[i]);
    }
    for (int i = 0; i < LIB_DIMS; i++) out[i] = maxAbs > 0 ? (signed char)lroundf(127.0f * y[i] / maxAbs) : 0;
}

// The stub's floats for a text: block b = SHA-256(text || b as 4 big-endian bytes) gives 32 values
// (byte - 127.5) / 127.5, for b = 0, 1, ... until LIB_DIMS are filled. The same text always gives the same
// vector; different texts give unrelated ones.
static void StubFloats(const char *text, float out[LIB_DIMS])
{
    for (int b = 0; b * 32 < LIB_DIMS; b++) {
        Sha256 s;
        Sha256Init(&s);
        Sha256Update(&s, text, strlen(text));
        unsigned char be[4] = {(unsigned char)(b >> 24), (unsigned char)(b >> 16), (unsigned char)(b >> 8), (unsigned char)b};
        Sha256Update(&s, be, 4);
        unsigned char d[32];
        Sha256Final(&s, d);
        for (int i = 0; i < 32; i++) out[b * 32 + i] = ((float)d[i] - 127.5f) / 127.5f;
    }
}

// The GGUF's folder: $MONOLITH_EMBED_DIR, else ~/.cache/monolith/embed. Heap string, NULL without HOME.
static char *EmbedDir(void)
{
    const char *dir = getenv("MONOLITH_EMBED_DIR");
    if (dir && *dir) return strdup(dir);
    const char *home = getenv("HOME");
    return home && *home ? Strf("%s/.cache/monolith/embed", home) : NULL;
}

// Checks the GGUF `file` in EmbedDir against `sha` (64 hex digits). The hash is cached in <file>.sha256 next
// to it, keyed by size and modification time, so only the first run after a change reads the 600 MB. Sets
// *path (heap) to the file. 0 matches, 3 missing or different (said why, with the fetch command).
static unsigned char CheckGguf(const char *file, const char *sha, char **path)
{
    *path = NULL;
    char *dir = EmbedDir();
    if (!dir) {
        ToolErr("neither MONOLITH_EMBED_DIR nor HOME is set: can't find %s", file);
        return 3;
    }
    char *p = Strf("%s/%s", dir, file);
    char *cache = Strf("%s/%s.sha256", dir, file);
    free(dir);
    struct stat st;
    if (!p || !cache || stat(p, &st) != 0) {
        ToolErr("the embedding model %s is missing (library/EMBEDDING.md; fetch it with: %s, into $MONOLITH_EMBED_DIR "
                "or ~/.cache/monolith/embed/)", p ? p : file, EMBED_FETCH);
        free(p);
        free(cache);
        return 3;
    }
    // the cache: "<size> <mtime s>.<mtime ns> <sha256>"
    char *key = Strf("%lld %lld.%09ld", (long long)st.st_size, (long long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
    char *cached = access(cache, F_OK) == 0 ? ToolReadTrimmed(cache) : NULL;  // (nob logs an error for a missing file)
    char hex[65] = {0};
    size_t keyLen = key ? strlen(key) : 0;
    if (key && cached && strncmp(cached, key, keyLen) == 0 && cached[keyLen] == ' ' && IsHex(cached + keyLen + 1, 64)) {
        memcpy(hex, cached + keyLen + 1, 64);
    } else {
        // hash the file (in 1 MiB reads) and remember it
        FILE *f = fopen(p, "rb");
        unsigned char *buf = malloc(1 << 20);
        if (!f || !buf) {
            ToolErr("can't read %s: %s", p, strerror(errno));
            if (f) fclose(f);
            free(buf);
            free(key);
            free(cached);
            free(cache);
            free(p);
            return 3;
        }
        Sha256 s;
        Sha256Init(&s);
        size_t n;
        while ((n = fread(buf, 1, 1 << 20, f)) > 0) Sha256Update(&s, buf, n);
        fclose(f);
        free(buf);
        Sha256Hex(&s, hex);
        char *line = Strf("%s %s\n", key ? key : "", hex);
        if (!line || !nob_write_entire_file(cache, line, strlen(line))) {
            ToolErr("warning: can't write the hash cache %s", cache);
        }
        free(line);
    }
    free(key);
    free(cached);
    free(cache);
    if (strcmp(hex, sha) != 0) {
        ToolErr("the embedding model %s has sha256 %s, the database expects %s: refusing to embed with it "
                "(library/EMBEDDING.md)", p, hex, sha);
        free(p);
        return 3;
    }
    *path = p;
    return 0;
}

// Makes the embedder ready: nothing for the stub; for llama, the GGUF check (against `db`'s meta, else the
// built-in model) and the model load with the fixed settings (CPU, LIB_THREADS threads, LIB_CTX tokens,
// embeddings with last-token pooling). 0 ready, 3 GGUF missing or different, 1 the model doesn't load.
static unsigned char EmbedderReady(Embedder *e, sqlite3 *db)
{
    if (e->stub || e->loaded) return 0;
    char *file = db ? MetaGet(db, "embed_file") : NULL;
    char *sha = db ? MetaGet(db, "embed_sha256") : NULL;
    char *path = NULL;
    unsigned char rc = CheckGguf(file ? file : EMBED_FILE, sha ? sha : EMBED_SHA256, &path);
    free(file);
    free(sha);
    if (rc != 0) return rc;

    double t0 = Now();
    // no crash backtrace through gdb (ggml forks a debugger on an abort otherwise): this program runs nothing
    setenv("GGML_NO_BACKTRACE", "1", 1);
    llama_log_set(LlamaLog, &gLlamaLogLevel);
    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    e->model = llama_model_load_from_file(path, mp);
    if (!e->model) {
        ToolErr("llama.cpp could not load %s", path);
        free(path);
        llama_backend_free();
        return 1;
    }
    free(path);
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = LIB_CTX;
    cp.n_batch = LIB_CTX;
    cp.n_ubatch = LIB_CTX;
    cp.n_seq_max = 1;
    cp.n_threads = LIB_THREADS;
    cp.n_threads_batch = LIB_THREADS;
    cp.embeddings = true;
    cp.pooling_type = LLAMA_POOLING_TYPE_LAST;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
    cp.no_perf = true;
    e->ctx = llama_init_from_model(e->model, cp);
    e->vocab = llama_model_get_vocab(e->model);
    e->nEmbd = llama_model_n_embd(e->model);
    // <|endoftext|> as one token (parsed as a special token here only: the texts themselves never are)
    llama_token eot[4];
    int n = e->vocab ? llama_tokenize(e->vocab, "<|endoftext|>", 13, eot, 4, false, true) : 0;
    if (!e->ctx || n != 1 || e->nEmbd < LIB_DIMS) {
        ToolErr("llama.cpp: %s", !e->ctx ? "no context" : n != 1 ? "no <|endoftext|> token" : "fewer than 512 dimensions");
        if (e->ctx) llama_free(e->ctx);
        llama_model_free(e->model);
        llama_backend_free();
        *e = (Embedder){.eosByTokenizer = -1};
        return 1;
    }
    e->endOfText = eot[0];
    e->loaded = true;
    e->loadSeconds = Now() - t0;
    return 0;
}

// Frees the model (nothing for the stub).
static void EmbedderClose(Embedder *e)
{
    if (e->loaded) {
        llama_free(e->ctx);
        llama_model_free(e->model);
        llama_backend_free();
    }
    bool stub = e->stub;
    *e = (Embedder){.stub = stub, .eosByTokenizer = -1};
}

// One text's vector, int8. Queries pass their task: llama then embeds "Instruct: <task>\nQuery:<text>" (the
// model card's format); documents pass NULL. The stub always hashes the text alone, so in tests a query equal
// to a row's embedded text finds that row with cosine 1. The text is tokenised without special tokens (a
// description can't smuggle one in), <|endoftext|> appended unless the tokenizer did, then decoded alone
// (the memory cleared first), and the pooled vector cut, normalised and quantised. 0 ok, 1 failed.
static unsigned char Embed(Embedder *e, const char *task, const char *text, signed char out[LIB_DIMS])
{
    if (e->stub) {
        float x[LIB_DIMS];
        StubFloats(text, x);
        Quantise(x, out);
        return 0;
    }
    char *full = task ? Strf("Instruct: %s\nQuery:%s", task, text) : strdup(text);
    if (!full) return 1;
    int len = (int)strlen(full);
    int n = -llama_tokenize(e->vocab, full, len, NULL, 0, true, false);
    llama_token *tokens = n > 0 ? malloc(((size_t)n + 1) * sizeof *tokens) : NULL;
    if (!tokens || llama_tokenize(e->vocab, full, len, tokens, n, true, false) != n) {
        ToolErr("llama.cpp: can't tokenise a text of %d bytes", len);
        free(tokens);
        free(full);
        return 1;
    }
    free(full);
    bool own = tokens[n - 1] == e->endOfText;
    if (e->eosByTokenizer < 0) e->eosByTokenizer = own;
    if (!own) tokens[n++] = e->endOfText;
    if (n > LIB_CTX) {
        ToolErr("a text of %d tokens is longer than %d", n, LIB_CTX);
        free(tokens);
        return 1;
    }
    llama_memory_clear(llama_get_memory(e->ctx), true);
    int rc = llama_decode(e->ctx, llama_batch_get_one(tokens, n));
    free(tokens);
    const float *v = rc == 0 ? llama_get_embeddings_seq(e->ctx, 0) : NULL;
    if (!v) {
        ToolErr("llama.cpp: the decode failed (%d)", rc);
        return 1;
    }
    Quantise(v, out);
    return 0;
}

// #endregion

// #region Search: filters, the four ranked lists, reciprocal rank fusion

// The default RRF weights (shape, style, use, words), measured in phase 4 on tests/library/queries.jsonl
// (docs/model-library.md §12): the keyword list counts 3 times, the style list half, because request sessions
// name the object and the name is the strongest signal; RRF then beats both single methods.
#define LIB_WEIGHTS {1, 0.5, 1, 3}

static bool ParseNumber(const char *s, double *out);  // below, with the other argument parsers

// --weights s,t,u,w: four numbers, comma-separated, into weight[4] (shape, style, use, words). true when valid.
static bool ParseWeights(const char *v, double weight[4])
{
    char *copy = strdup(v);
    int k = 0;
    bool ok = copy != NULL;
    double w[4] = {0};
    for (char *save = NULL, *t = copy ? strtok_r(copy, ",", &save) : NULL; t && ok; t = strtok_r(NULL, ",", &save)) {
        ok = k < 4 && ParseNumber(t, &w[k]) && w[k] >= 0;
        k++;
    }
    free(copy);
    if (!ok || k != 4) return false;
    for (int i = 0; i < 4; i++) weight[i] = w[i];
    return true;
}

// One search: the query texts (NULL when not given), the filters, the limit and the lists' weights.
typedef struct {
    char *text[3];       // shape, style, use
    char *words;
    Strs styleTags;
    Strs sources;
    bool hasMaxTris;
    long long maxTris;
    bool hasMaxSize;
    double maxSize;
    bool cc0Only;
    int limit;
    double weight[4];    // shape, style, use, words
} Query;

// Frees a query's strings.
static void QueryFree(Query *q)
{
    for (int k = 0; k < 3; k++) free(q->text[k]);
    free(q->words);
    StrsFree(&q->styleTags);
    StrsFree(&q->sources);
    *q = (Query){0};
}

// One entry of a ranked list: the id and its value (cosine distance or bm25; lower is better).
typedef struct {
    char *id;
    double value;
} Ranked;

typedef struct {
    Ranked *items;
    size_t count, capacity;
} RankedList;

// The four lists of a search (shape, style, use, words), which of them ran, and the query vectors.
typedef struct {
    RankedList list[4];
    bool used[4];
    signed char vec[3][LIB_DIMS];
    size_t missingVectors;   // rows without vectors (embed not run since they changed)
} Lists;

// Frees the lists' ids.
static void ListsFree(Lists *L)
{
    for (int k = 0; k < 4; k++) {
        for (size_t i = 0; i < L->list[k].count; i++) free(L->list[k].items[i].id);
        free(L->list[k].items);
    }
    *L = (Lists){0};
}

// The filter conditions (docs/model-library.md §6, step 1) as " AND ..." SQL on `m` (models), with
// parameters ?100 and up (BindFilters binds them). A NULL tris or size passes.
static char *FilterSql(const Query *q)
{
    Nob_String_Builder sb = {0};
    int p = 100;
    if (q->styleTags.count > 0) {
        nob_sb_append_cstr(&sb, " AND m.style IN (");
        for (size_t i = 0; i < q->styleTags.count; i++) nob_sb_appendf(&sb, "%s?%d", i ? ", " : "", p++);
        nob_sb_append_cstr(&sb, ")");
    }
    if (q->sources.count > 0) {
        nob_sb_append_cstr(&sb, " AND m.source IN (");
        for (size_t i = 0; i < q->sources.count; i++) nob_sb_appendf(&sb, "%s?%d", i ? ", " : "", p++);
        nob_sb_append_cstr(&sb, ")");
    }
    if (q->hasMaxTris) nob_sb_appendf(&sb, " AND (m.tris IS NULL OR m.tris <= ?%d)", p++);
    if (q->hasMaxSize) {
        nob_sb_appendf(&sb, " AND (m.sx IS NULL OR m.sy IS NULL OR m.sz IS NULL OR max(m.sx, m.sy, m.sz) <= ?%d)", p++);
    }
    if (q->cc0Only) nob_sb_append_cstr(&sb, " AND m.licence = '" LICENCE_CC0 "'");
    nob_sb_append_null(&sb);
    return sb.items;
}

// Binds the parameters of FilterSql, in its order.
static void BindFilters(sqlite3_stmt *st, const Query *q)
{
    int p = 100;
    for (size_t i = 0; i < q->styleTags.count; i++) sqlite3_bind_text(st, p++, q->styleTags.items[i], -1, SQLITE_STATIC);
    for (size_t i = 0; i < q->sources.count; i++) sqlite3_bind_text(st, p++, q->sources.items[i], -1, SQLITE_STATIC);
    if (q->hasMaxTris) sqlite3_bind_int64(st, p++, q->maxTris);
    if (q->hasMaxSize) sqlite3_bind_double(st, p++, q->maxSize);
}

// The FTS5 query for a text (docs/model-library.md §6, step 3): its words (runs of ASCII letters and digits,
// or bytes of multi-byte UTF-8 characters), lower case, each once, sorted, each quoted as an FTS5 string and
// joined by OR (FtsQuery); quoting keeps any FTS5 syntax in the request out. This part: the new words of
// `text` appended to `words`; returns how many.
static size_t FtsWords(const char *text, Strs *words)
{
    size_t added = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        while (*p && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p >= 0x80)) p++;
        const unsigned char *start = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p >= 0x80) p++;
        if (p == start) continue;
        char *w = strndup((const char *)start, (size_t)(p - start));
        if (!w) continue;
        for (char *c = w; *c; c++) {
            if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        }
        bool dup = false;
        for (size_t i = 0; i < words->count && !dup; i++) dup = strcmp(words->items[i], w) == 0;
        if (dup) {
            free(w);
        } else {
            nob_da_append(words, w);
            added++;
        }
    }
    return added;
}

// qsort order of strings (byte order).
static int CompareStr(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// The whole FTS5 query of a search (heap), or NULL when there are no words: --words, else --shape and --use.
static char *FtsQuery(const Query *q)
{
    Strs words = {0};
    if (q->words) {
        FtsWords(q->words, &words);
    } else {
        if (q->text[0]) FtsWords(q->text[0], &words);
        if (q->text[2]) FtsWords(q->text[2], &words);
    }
    if (words.count == 0) {
        StrsFree(&words);
        return NULL;
    }
    qsort(words.items, words.count, sizeof *words.items, CompareStr);
    Nob_String_Builder sb = {0};
    for (size_t i = 0; i < words.count; i++) {
        if (i > 0) nob_sb_append_cstr(&sb, " OR ");
        nob_da_append(&sb, '"');
        for (const char *c = words.items[i]; *c; c++) {
            if (*c == '"') nob_da_append(&sb, '"');  // FTS5 escapes a quote by doubling it
            nob_da_append(&sb, *c);
        }
        nob_da_append(&sb, '"');
    }
    nob_sb_append_null(&sb);
    StrsFree(&words);
    return sb.items;
}

// Runs one prepared list query (its first result column the id, the second the value) into `list`.
// 0 ok, 1 failed.
static unsigned char CollectList(sqlite3 *db, sqlite3_stmt *st, RankedList *list)
{
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        Ranked r = {.id = strdup((const char *)sqlite3_column_text(st, 0)), .value = sqlite3_column_double(st, 1)};
        nob_da_append(list, r);
    }
    return rc == SQLITE_DONE ? 0 : SqlFail(db, "search");
}

// The embedder the database's vectors were made with (meta.embedder: "llama" or "stub"), NULL before the
// first embed. A search or embed --missing with the other one would compare vectors of two different spaces.
static unsigned char CheckEmbedder(sqlite3 *db, const Embedder *e, bool *haveVectors)
{
    char *made = MetaGet(db, "embedder");
    const char *mine = e->stub ? "stub" : "llama";
    *haveVectors = made != NULL;
    if (made && strcmp(made, mine) != 0) {
        ToolErr("the database's vectors were made by the %s embedder, this run uses %s (run with --embedder %s)", made, mine, made);
        free(made);
        return 1;
    }
    free(made);
    return 0;
}

// The four ranked lists of a query (docs/model-library.md §6, steps 1-3): per vector query text, the exact
// cosine distance to every filtered row with vectors, top LIB_TOP by (distance, id); then FTS5 bm25 over the
// filtered rows, top LIB_TOP by (bm25, id). 0 ok, 1 failed, 3 the GGUF (EmbedderReady).
static unsigned char SearchLists(sqlite3 *db, const Query *q, Embedder *e, Lists *L, double *embedSeconds)
{
    *L = (Lists){0};
    *embedSeconds = 0;
    char *filters = FilterSql(q);
    unsigned char rc = 0;
    bool anyText = q->text[0] || q->text[1] || q->text[2];
    bool haveVectors = false;
    if (anyText) rc = CheckEmbedder(db, e, &haveVectors);
    if (rc == 0 && anyText && !haveVectors) ToolErr("warning: the database has no vectors yet (run: build/tools/library embed)");
    if (rc == 0 && anyText && haveVectors) {
        // rows the vector lists can't see
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, "SELECT count(*) FROM models WHERE embed_hash IS NULL", -1, &st, NULL) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) {
            L->missingVectors = (size_t)sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
        if (L->missingVectors > 0) {
            ToolErr("warning: %zu rows have no vectors and appear only in the keyword list (run: build/tools/library embed)",
                    L->missingVectors);
        }
        rc = EmbedderReady(e, db);
    }
    for (int k = 0; k < 3 && rc == 0 && haveVectors; k++) {
        if (!q->text[k]) continue;
        double t0 = Now();
        if (Embed(e, INSTRUCT[k], q->text[k], L->vec[k]) != 0) {
            rc = 1;
            break;
        }
        *embedSeconds += Now() - t0;
        L->used[k] = true;
        char *sql = Strf("SELECT m.id, vec_distance_cosine(v.embedding, vec_int8(?1)) AS d FROM models m "
                         "JOIN %s v ON v.rowid = m.rowid WHERE 1%s ORDER BY d, m.id LIMIT %d",
                         VEC_TABLES[k], filters, LIB_TOP);
        sqlite3_stmt *st = NULL;
        if (!sql || sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
            rc = SqlFail(db, "search");
        } else {
            sqlite3_bind_blob(st, 1, L->vec[k], LIB_DIMS, SQLITE_STATIC);
            BindFilters(st, q);
            rc = CollectList(db, st, &L->list[k]);
        }
        sqlite3_finalize(st);
        free(sql);
    }
    char *match = rc == 0 ? FtsQuery(q) : NULL;
    if (match) {
        L->used[3] = true;
        char *sql = Strf("SELECT m.id, bm25(models_fts) AS s FROM models_fts JOIN models m ON m.rowid = models_fts.rowid "
                         "WHERE models_fts MATCH ?1%s ORDER BY s, m.id LIMIT %d",
                         filters, LIB_TOP);
        sqlite3_stmt *st = NULL;
        if (!sql || sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
            rc = SqlFail(db, "search");
        } else {
            sqlite3_bind_text(st, 1, match, -1, SQLITE_STATIC);
            BindFilters(st, q);
            rc = CollectList(db, st, &L->list[3]);
        }
        sqlite3_finalize(st);
        free(sql);
    }
    free(match);
    free(filters);
    return rc;
}

// One fused result: the id (borrowed from a list), its RRF score and its rank in each list (0: not in it).
typedef struct {
    const char *id;
    double score;
    int rank[4];
} Fused;

typedef struct {
    Fused *items;
    size_t count, capacity;
} FusedList;

// qsort order of fused results: score descending, then id ascending.
static int CompareFused(const void *a, const void *b)
{
    const Fused *x = a, *y = b;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    return strcmp(x->id, y->id);
}

// Reciprocal rank fusion (docs/model-library.md §6, steps 4-5): score(id) = sum over the lists in `use` of
// weight / (LIB_RRF_K + rank), rank from 1; the lists are added in their order (shape, style, use, words),
// so the same lists always give the same doubles. Sorted by score descending, then id.
static void Fuse(const RankedList lists[4], const bool use[4], const double weight[4], FusedList *out)
{
    out->count = 0;
    for (int k = 0; k < 4; k++) {
        if (!use[k]) continue;
        for (size_t i = 0; i < lists[k].count; i++) {
            const char *id = lists[k].items[i].id;
            size_t at = 0;
            while (at < out->count && strcmp(out->items[at].id, id) != 0) at++;
            if (at == out->count) nob_da_append(out, ((Fused){.id = id}));
            out->items[at].score += weight[k] / (double)(LIB_RRF_K + (int)i + 1);
            out->items[at].rank[k] = (int)i + 1;
        }
    }
    if (out->count > 1) qsort(out->items, out->count, sizeof *out->items, CompareFused);
}

// The best cosine similarity of row `id` over the vector lists that ran (docs/model-library.md §6: best-cos),
// computed exactly for that row (it may be outside a list's top LIB_TOP). False when no vector list ran or
// the row has no vectors.
static bool BestCos(sqlite3 *db, const Lists *L, const char *id, double *best)
{
    bool found = false;
    for (int k = 0; k < 3; k++) {
        if (!L->used[k]) continue;
        char *sql = Strf("SELECT 1.0 - vec_distance_cosine(v.embedding, vec_int8(?1)) FROM %s v "
                         "WHERE v.rowid = (SELECT rowid FROM models WHERE id = ?2)",
                         VEC_TABLES[k]);
        sqlite3_stmt *st = NULL;
        if (sql && sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_blob(st, 1, L->vec[k], LIB_DIMS, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, id, -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) {
                double c = sqlite3_column_double(st, 0);
                if (!found || c > *best) *best = c;
                found = true;
            }
        }
        sqlite3_finalize(st);
        free(sql);
    }
    return found;
}

// #endregion

// #region Commands

// The global flags and what every command needs.
typedef struct {
    const char *db;      // --db PATH
    bool stub;           // --embedder stub
    const char *now;     // --now YYYY-MM-DD (else today)
    char today[16];      // today's date, when --now isn't given
    bool timing;         // --timing: model load and query times on stderr
} Options;

// The usage text on stderr; returns 2 (the usage exit code).
static int Usage(void)
{
    fprintf(stderr,
            "usage: build/tools/library [--db PATH] [--embedder llama|stub] [--now YYYY-MM-DD] [--timing] <command> ...\n"
            "  ingest <file.jsonl> [--prune]\n"
            "  embed [--all | --missing]\n"
            "  search [--shape T] [--style T] [--use T] [--words T] [--style-tag S]... [--source S]...\n"
            "         [--max-tris N] [--max-size M] [--cc0-only] [--limit N] [--weights s,t,u,w]\n"
            "  show <id>\n"
            "  pin <id> <sha256> [--force]\n"
            "  check\n"
            "  dump\n"
            "  eval [--weights s,t,u,w] <queries.jsonl>\n"
            "(docs/code/library.md)\n");
    return 2;
}

// The date a change is stamped with: --now, else today (local time).
static const char *Today(Options *o)
{
    return o->now ? o->now : o->today;
}

// ingest <file.jsonl> [--prune]: upserts every line by id, in id order, in one transaction; then rebuilds the
// keyword index and VACUUMs. Nothing is written when any line is invalid, or when nothing changed (the file
// stays byte for byte as it was). Exit 0, 1 (invalid input), 2 (usage).
static int CmdIngest(Options *o, int argc, char **argv)
{
    const char *file = NULL;
    bool prune = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--prune") == 0) prune = true;
        else if (!file && argv[i][0] != '-') file = argv[i];
        else return Usage();
    }
    if (!file) return Usage();

    // Step 1: read and check every line, then the ids (sorted: duplicates are neighbours)
    Rows rows = {0};
    Strs problems = {0};
    if (ReadJsonl(file, &rows, &problems) != 0) {
        RowsFree(&rows);
        StrsFree(&problems);
        return 1;
    }
    if (rows.count > 1) qsort(rows.items, rows.count, sizeof *rows.items, CompareRowId);
    for (size_t i = 1; i < rows.count; i++) {
        if (rows.items[i].v[C_ID].t == 's' && strcmp(Text(&rows.items[i], C_ID), Text(&rows.items[i - 1], C_ID)) == 0) {
            Problem(&problems, "line %zu: %s: the id is also on line %zu", rows.items[i].line, Text(&rows.items[i], C_ID),
                    rows.items[i - 1].line);
        }
    }
    if (problems.count > 0) {
        for (size_t i = 0; i < problems.count; i++) ToolErr("ingest: %s: %s", file, problems.items[i]);
        ToolErr("ingest: refused %s: %zu problem%s; nothing written", file, problems.count, problems.count == 1 ? "" : "s");
        RowsFree(&rows);
        StrsFree(&problems);
        return 1;
    }
    StrsFree(&problems);

    // Step 2: the database, in one transaction (a new one gets its schema in it)
    sqlite3 *db = NULL;
    bool created = false;
    if (OpenDb(o->db, OPEN_CREATE, &db, &created) != 0) {
        RowsFree(&rows);
        return 1;
    }
    unsigned char rc = Exec(db, "BEGIN IMMEDIATE");
    if (rc == 0 && created) rc = CreateSchema(db);

    char *cols = ColumnList();
    Nob_String_Builder ins = {0}, upd = {0};
    nob_sb_appendf(&ins, "INSERT INTO models(%s, embed_hash, indexed_at) VALUES(", cols);
    for (size_t c = 0; c < NCOLS; c++) nob_sb_appendf(&ins, "?%zu, ", c + 1);
    nob_sb_appendf(&ins, "NULL, ?%zu)", NCOLS + 1);
    nob_sb_append_null(&ins);
    nob_sb_append_cstr(&upd, "UPDATE models SET ");
    for (size_t c = 0; c < NCOLS; c++) nob_sb_appendf(&upd, "%s = ?%zu, ", COLUMNS[c].name, c + 1);
    nob_sb_appendf(&upd, "indexed_at = ?%zu, embed_hash = CASE WHEN ?%zu THEN NULL ELSE embed_hash END WHERE rowid = ?%zu",
                   NCOLS + 1, NCOLS + 2, NCOLS + 3);
    nob_sb_append_null(&upd);
    char *selSql = Strf("SELECT rowid, embed_hash, %s FROM models WHERE id = ?1", cols);
    free(cols);
    sqlite3_stmt *sel = NULL, *insert = NULL, *update = NULL;
    if (rc == 0 && (sqlite3_prepare_v2(db, selSql, -1, &sel, NULL) != SQLITE_OK ||
                    sqlite3_prepare_v2(db, ins.items, -1, &insert, NULL) != SQLITE_OK ||
                    sqlite3_prepare_v2(db, upd.items, -1, &update, NULL) != SQLITE_OK)) {
        rc = SqlFail(db, "ingest");
    }
    free(selSql);
    nob_sb_free(ins);
    nob_sb_free(upd);

    // Step 3: every row, by id: new, changed or the same
    size_t added = 0, changed = 0, same = 0, pruned = 0, cleared = 0;
    for (size_t i = 0; i < rows.count && rc == 0; i++) {
        Row *r = &rows.items[i];
        Row old = {0};
        bool exists = false;
        sqlite3_reset(sel);
        sqlite3_bind_text(sel, 1, Text(r, C_ID), -1, SQLITE_STATIC);
        if (sqlite3_step(sel) == SQLITE_ROW) {
            exists = true;
            old.rowid = sqlite3_column_int64(sel, 0);
            old.embedHash = Dup((const char *)sqlite3_column_text(sel, 1));
            ReadValues(sel, 2, &old);
        }
        sqlite3_reset(sel);
        if (!exists) {
            sqlite3_reset(insert);
            BindValues(insert, 1, r);
            sqlite3_bind_text(insert, (int)NCOLS + 1, Today(o), -1, SQLITE_STATIC);
            if (sqlite3_step(insert) != SQLITE_DONE) rc = SqlFail(db, "ingest");
            added++;
            continue;
        }
        // a NULL hash in the file keeps the one pinned in the database; a different one replaces it
        if (r->v[C_FILE_HASH].t == 0 && old.v[C_FILE_HASH].t == 's') {
            r->v[C_FILE_HASH] = (Value){.t = 's', .s = strdup(old.v[C_FILE_HASH].s)};
            free(r->v[C_HASH_KIND].s);
            r->v[C_HASH_KIND] = (Value){.t = 's', .s = Dup(old.v[C_HASH_KIND].s)};
        } else if (r->v[C_FILE_HASH].t == 's' && old.v[C_FILE_HASH].t == 's' &&
                   strcmp(r->v[C_FILE_HASH].s, old.v[C_FILE_HASH].s) != 0) {
            ToolErr("ingest: %s: the file hash changed from %s %s to %s %s", Text(r, C_ID), Text(&old, C_HASH_KIND),
                    old.v[C_FILE_HASH].s, Text(r, C_HASH_KIND), r->v[C_FILE_HASH].s);
        }
        bool equal = true;
        for (size_t c = 0; c < NCOLS && equal; c++) equal = ValueEqual(&r->v[c], &old.v[c]);
        if (equal) {
            same++;
        } else {
            // a change to the embedded texts clears the vectors
            char before[65], after[65];
            RowEmbedHash(&old, before);
            RowEmbedHash(r, after);
            bool textChanged = strcmp(before, after) != 0;
            if (textChanged && old.embedHash) {
                rc = DeleteVectors(db, old.rowid);
                cleared++;
            }
            sqlite3_reset(update);
            BindValues(update, 1, r);
            sqlite3_bind_text(update, (int)NCOLS + 1, Today(o), -1, SQLITE_STATIC);
            sqlite3_bind_int(update, (int)NCOLS + 2, textChanged);
            sqlite3_bind_int64(update, (int)NCOLS + 3, old.rowid);
            if (rc == 0 && sqlite3_step(update) != SQLITE_DONE) rc = SqlFail(db, "ingest");
            changed++;
        }
        RowFree(&old);
    }

    // Step 4: --prune: rows of the file's sources that the file doesn't list
    if (rc == 0 && prune) {
        Rows have = {0};
        rc = LoadRows(db, &have);
        for (size_t i = 0; i < have.count && rc == 0; i++) {
            Row *h = &have.items[i];
            bool sourceInFile = false;
            for (size_t k = 0; k < rows.count && !sourceInFile; k++) sourceInFile = strcmp(Text(&rows.items[k], C_SOURCE), Text(h, C_SOURCE)) == 0;
            Row key = {0};
            key.v[C_ID] = (Value){.t = 's', .s = h->v[C_ID].s};
            bool listed = rows.count > 0 && bsearch(&key, rows.items, rows.count, sizeof *rows.items, CompareRowId) != NULL;
            if (!sourceInFile || listed) continue;
            if (h->embedHash) rc = DeleteVectors(db, h->rowid);
            sqlite3_stmt *del = NULL;
            if (rc == 0 && sqlite3_prepare_v2(db, "DELETE FROM models WHERE rowid = ?1", -1, &del, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(del, 1, h->rowid);
                if (sqlite3_step(del) != SQLITE_DONE) rc = SqlFail(db, "prune");
            } else if (rc == 0) {
                rc = SqlFail(db, "prune");
            }
            sqlite3_finalize(del);
            ToolErr("ingest: pruned %s", Text(h, C_ID));
            pruned++;
        }
        RowsFree(&have);
    }
    sqlite3_finalize(sel);
    sqlite3_finalize(insert);
    sqlite3_finalize(update);

    // Step 5: nothing changed: roll back (the file is untouched); else the keyword index, commit, VACUUM
    bool any = created || added + changed + pruned > 0;
    if (rc == 0 && !any) {
        rc = Exec(db, "ROLLBACK");
        ToolErr("ingest: %s: %zu rows, nothing changed", file, rows.count);
    } else if (rc == 0) {
        rc = Exec(db, "INSERT INTO models_fts(models_fts) VALUES('rebuild')");
        if (rc == 0) rc = Exec(db, "COMMIT");
        if (rc == 0) rc = Exec(db, "VACUUM");
        size_t missing = 0;
        sqlite3_stmt *st = NULL;
        if (rc == 0 && sqlite3_prepare_v2(db, "SELECT count(*) FROM models WHERE embed_hash IS NULL", -1, &st, NULL) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) {
            missing = (size_t)sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
        if (rc == 0) {
            ToolErr("ingest: %s: %zu rows: %zu added, %zu changed (%zu with new texts), %zu unchanged, %zu pruned; "
                    "%zu rows need vectors%s",
                    file, rows.count, added, changed, cleared, same, pruned, missing,
                    missing ? " (run: build/tools/library embed)" : "");
        }
    } else {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
    sqlite3_close(db);
    RowsFree(&rows);
    return rc == 0 ? 0 : 1;
}

// embed [--all | --missing]: the three vectors of every row (--all) or of the rows whose embed_hash is NULL
// or no longer matches their texts (--missing, the default), committed every LIB_EMBED_COMMIT rows; then
// meta.embedder and a VACUUM. Exit 0, 1 (failed, or vectors of the other embedder), 2, 3 (GGUF).
static int CmdEmbed(Options *o, int argc, char **argv)
{
    bool all = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0) all = true;
        else if (strcmp(argv[i], "--missing") == 0) all = false;
        else return Usage();
    }
    sqlite3 *db = NULL;
    if (OpenDb(o->db, OPEN_WRITE, &db, NULL) != 0) return 1;
    Embedder e = {.stub = o->stub, .eosByTokenizer = -1};
    bool have = false;
    Rows rows = {0};
    unsigned char rc = all ? 0 : CheckEmbedder(db, &e, &have);
    if (rc == 0) rc = LoadRows(db, &rows);

    // the rows to embed
    size_t todo = 0;
    bool *pick = calloc(rows.count + 1, sizeof *pick);
    for (size_t i = 0; i < rows.count && rc == 0; i++) {
        char hash[65];
        RowEmbedHash(&rows.items[i], hash);
        pick[i] = all || !rows.items[i].embedHash || strcmp(rows.items[i].embedHash, hash) != 0;
        todo += pick[i];
    }
    if (rc == 0 && todo > 0) rc = EmbedderReady(&e, db);
    if (rc == 0 && o->timing && !e.stub) ToolErr("timing: model load %.3f s", e.loadSeconds);

    sqlite3_stmt *ins[3] = {0}, *upd = NULL;
    for (int k = 0; k < 3 && rc == 0; k++) {
        char *sql = Strf("INSERT INTO %s(rowid, embedding) VALUES(?1, vec_int8(?2))", VEC_TABLES[k]);
        if (!sql || sqlite3_prepare_v2(db, sql, -1, &ins[k], NULL) != SQLITE_OK) rc = SqlFail(db, "embed");
        free(sql);
    }
    if (rc == 0 && sqlite3_prepare_v2(db, "UPDATE models SET embed_hash = ?1 WHERE rowid = ?2", -1, &upd, NULL) != SQLITE_OK) {
        rc = SqlFail(db, "embed");
    }
    if (rc == 0) rc = Exec(db, "BEGIN IMMEDIATE");
    double t0 = Now();
    size_t done = 0;
    for (size_t i = 0; i < rows.count && rc == 0; i++) {
        if (!pick[i]) continue;
        Row *r = &rows.items[i];
        char *texts[3];
        EmbedTexts(r, texts);
        char hash[65];
        EmbedHash(texts, hash);
        signed char vec[3][LIB_DIMS];
        for (int k = 0; k < 3 && rc == 0; k++) rc = Embed(&e, NULL, texts[k], vec[k]);
        for (int k = 0; k < 3; k++) free(texts[k]);
        // the old vectors out, the new ones in, then the hash that says they are current
        if (rc == 0 && r->embedHash) rc = DeleteVectors(db, r->rowid);
        for (int k = 0; k < 3 && rc == 0; k++) {
            sqlite3_reset(ins[k]);
            sqlite3_bind_int64(ins[k], 1, r->rowid);
            sqlite3_bind_blob(ins[k], 2, vec[k], LIB_DIMS, SQLITE_STATIC);
            if (sqlite3_step(ins[k]) != SQLITE_DONE) rc = SqlFail(db, "embed");
        }
        if (rc == 0) {
            sqlite3_reset(upd);
            sqlite3_bind_text(upd, 1, hash, -1, SQLITE_STATIC);
            sqlite3_bind_int64(upd, 2, r->rowid);
            if (sqlite3_step(upd) != SQLITE_DONE) rc = SqlFail(db, "embed");
        }
        done++;
        if (rc == 0 && done % LIB_EMBED_COMMIT == 0) {
            rc = Exec(db, "COMMIT");
            if (rc == 0) rc = Exec(db, "BEGIN IMMEDIATE");
            ToolErr("embed: %zu of %zu rows (%.1f s)", done, todo, Now() - t0);
        }
    }
    // which embedder made the vectors (only when some were made: a run with nothing to do writes nothing)
    if (rc == 0 && done > 0) rc = MetaSet(db, "embedder", e.stub ? "stub" : "llama");
    if (rc == 0) rc = Exec(db, "COMMIT");
    else sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    if (rc == 0 && done > 0) rc = Exec(db, "VACUUM");
    for (int k = 0; k < 3; k++) sqlite3_finalize(ins[k]);
    sqlite3_finalize(upd);
    if (rc == 0) {
        ToolErr("embed: %zu of %zu rows embedded (%s, %.1f s)%s", done, rows.count, e.stub ? "stub" : "llama", Now() - t0,
                e.eosByTokenizer < 0  ? ""
                : e.eosByTokenizer    ? "; the tokenizer appends <|endoftext|>"
                                      : "; <|endoftext|> appended by library (the tokenizer doesn't)");
    }
    free(pick);
    RowsFree(&rows);
    EmbedderClose(&e);
    sqlite3_close(db);
    return rc;
}

// Parses a non-negative number (strtod: the whole text); false when it isn't one.
static bool ParseNumber(const char *s, double *out)
{
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (!*s || *end || errno != 0 || !isfinite(v) || v < 0) return false;
    *out = v;
    return true;
}

// Parses a non-negative integer (the whole text); false when it isn't one.
static bool ParseCount(const char *s, long long *out)
{
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (!*s || *end || errno != 0 || v < 0) return false;
    *out = v;
    return true;
}

// Adds a --style-tag or --source value after checking it against its list; false (said why) when it isn't one.
static bool AddChecked(Strs *to, const char *value, const char **list, size_t n, const char *flag)
{
    if (!InList(value, list, n)) {
        ToolErr("%s %s: not one of the known values", flag, value);
        return false;
    }
    nob_da_append(to, strdup(value));
    return true;
}

// Fills a query from search's arguments (docs/model-library.md §6). 0 ok, 2 usage.
static int ParseSearchArgs(int argc, char **argv, Query *q)
{
    *q = (Query){.limit = LIB_LIMIT, .weight = LIB_WEIGHTS};
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--cc0-only") == 0) {
            q->cc0Only = true;
            continue;
        }
        if (!v) return Usage();
        i++;
        if (strcmp(a, "--shape") == 0) { free(q->text[0]); q->text[0] = strdup(v); }
        else if (strcmp(a, "--style") == 0) { free(q->text[1]); q->text[1] = strdup(v); }
        else if (strcmp(a, "--use") == 0) { free(q->text[2]); q->text[2] = strdup(v); }
        else if (strcmp(a, "--words") == 0) { free(q->words); q->words = strdup(v); }
        else if (strcmp(a, "--style-tag") == 0) {
            if (!AddChecked(&q->styleTags, v, STYLES, NOB_ARRAY_LEN(STYLES), a)) return 2;
        } else if (strcmp(a, "--source") == 0) {
            if (!AddChecked(&q->sources, v, SOURCES, NOB_ARRAY_LEN(SOURCES), a)) return 2;
        } else if (strcmp(a, "--max-tris") == 0) {
            if (!ParseCount(v, &q->maxTris)) return Usage();
            q->hasMaxTris = true;
        } else if (strcmp(a, "--max-size") == 0) {
            if (!ParseNumber(v, &q->maxSize)) return Usage();
            q->hasMaxSize = true;
        } else if (strcmp(a, "--limit") == 0) {
            long long n;
            if (!ParseCount(v, &n) || n < 1 || n > 1000000) return Usage();
            q->limit = (int)n;
        } else if (strcmp(a, "--weights") == 0) {
            if (!ParseWeights(v, q->weight)) return Usage();
        } else {
            return Usage();
        }
    }
    if (!q->text[0] && !q->text[1] && !q->text[2] && !q->words) {
        ToolErr("search: give at least one of --shape, --style, --use, --words");
        return 2;
    }
    return 0;
}

// The part of a row search prints after the scores: tris|?, sx×sy×sz|?, category, shape_desc (a heap string,
// fields joined by tabs). NULL when the id isn't there.
static char *ResultTail(sqlite3 *db, const char *id)
{
    sqlite3_stmt *st = NULL;
    char *tail = NULL;
    if (sqlite3_prepare_v2(db, "SELECT tris, sx, sy, sz, category, shape_desc FROM models WHERE id = ?1", -1, &st, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            char *tris = sqlite3_column_type(st, 0) == SQLITE_NULL ? strdup("?") : Strf("%lld", sqlite3_column_int64(st, 0));
            bool sized = sqlite3_column_type(st, 1) != SQLITE_NULL && sqlite3_column_type(st, 2) != SQLITE_NULL &&
                         sqlite3_column_type(st, 3) != SQLITE_NULL;
            char *size = sized ? Strf("%.2f\xc3\x97%.2f\xc3\x97%.2f", sqlite3_column_double(st, 1), sqlite3_column_double(st, 2),
                                      sqlite3_column_double(st, 3))
                               : strdup("?");
            tail = Strf("%s\t%s\t%s\t%s", tris ? tris : "?", size ? size : "?", sqlite3_column_text(st, 4),
                        sqlite3_column_text(st, 5));
            free(tris);
            free(size);
        }
    }
    sqlite3_finalize(st);
    return tail;
}

// search ...: docs/model-library.md §6. Prints up to --limit lines, tab-separated: id, score (6 decimals),
// best-cos (4 decimals, '-' without vector lists), tris|?, sx×sy×sz|?, category, shape_desc. Exit 0 (also
// with no results), 1 (database), 2 (usage), 3 (GGUF).
static int CmdSearch(Options *o, int argc, char **argv)
{
    Query q;
    int usage = ParseSearchArgs(argc, argv, &q);
    if (usage != 0) {
        QueryFree(&q);
        return usage;
    }
    sqlite3 *db = NULL;
    if (OpenDb(o->db, OPEN_READ, &db, NULL) != 0) {
        QueryFree(&q);
        return 1;
    }
    Embedder e = {.stub = o->stub, .eosByTokenizer = -1};
    Lists L;
    double t0 = Now(), embedSeconds = 0;
    unsigned char rc = SearchLists(db, &q, &e, &L, &embedSeconds);
    if (rc == 0) {
        FusedList fused = {0};
        Fuse(L.list, L.used, q.weight, &fused);
        for (size_t i = 0; i < fused.count && i < (size_t)q.limit; i++) {
            double best = 0;
            bool hasCos = BestCos(db, &L, fused.items[i].id, &best);
            char *tail = ResultTail(db, fused.items[i].id);
            char cos[32];
            if (hasCos) snprintf(cos, sizeof cos, "%.4f", best);
            else snprintf(cos, sizeof cos, "-");
            fprintf(Out(), "%s\t%.6f\t%s\t%s\n", fused.items[i].id, fused.items[i].score, cos, tail ? tail : "");
            free(tail);
        }
        free(fused.items);
        if (o->timing) {
            ToolErr("timing: model load %.3f s, query embedding %.3f s, search with embedding %.3f s", e.loadSeconds,
                    embedSeconds, Now() - t0 - e.loadSeconds);
        }
    }
    ListsFree(&L);
    EmbedderClose(&e);
    QueryFree(&q);
    sqlite3_close(db);
    return rc;
}

// Prints a row as `key: value` lines (schema order, `rowid` left out; NULL as "NULL").
static void PrintRow(const Row *r, const char *indexedAt)
{
    FILE *f = Out();
    for (size_t c = 0; c < NCOLS; c++) {
        const Value *v = &r->v[c];
        fprintf(f, "%s: ", COLUMNS[c].name);
        switch (v->t) {
        case 's': fputs(v->s, f); break;
        case 'i': fprintf(f, "%lld", v->i); break;
        case 'r': fprintf(f, "%.17g", v->r); break;
        default: fputs("NULL", f); break;
        }
        fputc('\n', f);
    }
    fprintf(f, "embed_hash: %s\n", r->embedHash ? r->embedHash : "NULL");
    fprintf(f, "indexed_at: %s\n", indexedAt ? indexedAt : "NULL");
}

// The exact command that downloads a row's file (docs/model-library.md §7 `show`, §9), as a heap string:
//   polyhaven  python3 -I tools/fetch_asset.py polyhaven get <slug> --expect-md5 <hash>
//   kenney     python3 -I tools/fetch_asset.py kenney get <row id> --expect-sha256 <entry hash>
//              --expect-zip-sha256 <zip hash>   (from the local zip cache: kenney.nl isn't fetched, §2)
//   polyfork   python3 -I tools/fetch_asset.py polyfork get <asset id> --expect-sha256 <hash>
//   polypizza  python3 -I tools/fetch_asset.py polypizza get <model id> --expect-sha256 <hash>
// The hash flag is --expect-md5 or --expect-sha256 by hash_kind, and left out when the row has no hash.
static char *FetchCommand(const Row *r)
{
    const char *source = Text(r, C_SOURCE);
    const char *id = Text(r, C_ID);
    const char *rest = id + strlen(source) + (strlen(id) > strlen(source) ? 1 : 0);  // the id after "<source>-"
    Nob_String_Builder sb = {0};
    nob_sb_appendf(&sb, "python3 -I tools/fetch_asset.py %s get ", source);
    // Kenney takes the whole row id (kenney-<pack>/<name>), the others the source's own id
    char *w = ShellWord(strcmp(source, "kenney") == 0 ? id : rest);
    nob_sb_append_cstr(&sb, w);
    free(w);
    if (r->v[C_FILE_HASH].t == 's') {
        nob_sb_appendf(&sb, " --expect-%s %s", Text(r, C_HASH_KIND), r->v[C_FILE_HASH].s);
    }
    if (strcmp(source, "kenney") == 0 && r->v[C_ZIP_SHA256].t == 's') {
        nob_sb_appendf(&sb, " --expect-zip-sha256 %s", r->v[C_ZIP_SHA256].s);
    }
    nob_sb_append_null(&sb);
    return sb.items;
}

// Reads one row by id (with rowid, embed_hash and indexed_at). 0 found, 1 not found or failed.
static unsigned char LoadRow(sqlite3 *db, const char *id, Row *r, char **indexedAt)
{
    char *cols = ColumnList();
    char *sql = Strf("SELECT rowid, embed_hash, indexed_at, %s FROM models WHERE id = ?1", cols);
    free(cols);
    sqlite3_stmt *st = NULL;
    unsigned char rc = 1;
    *r = (Row){0};
    *indexedAt = NULL;
    if (sql && sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            r->rowid = sqlite3_column_int64(st, 0);
            r->embedHash = Dup((const char *)sqlite3_column_text(st, 1));
            *indexedAt = Dup((const char *)sqlite3_column_text(st, 2));
            ReadValues(st, 3, r);
            rc = 0;
        }
    }
    sqlite3_finalize(st);
    free(sql);
    return rc;
}

// show <id>: the row as `key: value` lines, then `fetch: <command>` (and a note when the row has no hash).
// Exit 0, 1 (unknown id), 2.
static int CmdShow(Options *o, int argc, char **argv)
{
    if (argc != 1) return Usage();
    sqlite3 *db = NULL;
    if (OpenDb(o->db, OPEN_READ, &db, NULL) != 0) return 1;
    Row r;
    char *indexedAt = NULL;
    int rc = 0;
    if (LoadRow(db, argv[0], &r, &indexedAt) != 0) {
        ToolErr("show: no model with id %s", argv[0]);
        rc = 1;
    } else {
        PrintRow(&r, indexedAt);
        char *cmd = FetchCommand(&r);
        fprintf(Out(), "fetch: %s\n", cmd);
        if (r.v[C_FILE_HASH].t != 's') {
            fprintf(Out(), "note: no file hash is recorded; after the download record it with: build/tools/library pin %s "
                           "<sha256 from provenance.json>\n",
                    argv[0]);
        }
        free(cmd);
    }
    RowFree(&r);
    free(indexedAt);
    sqlite3_close(db);
    return rc;
}

// pin <id> <sha256> [--force]: records a file's sha256 after its first download, where the row has no hash;
// --force replaces a different one (reported). The row's indexed_at becomes today. Exit 0, 1, 2.
static int CmdPin(Options *o, int argc, char **argv)
{
    const char *args[2] = {0};
    int n = 0;
    bool force = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0) force = true;
        else if (n < 2) args[n++] = argv[i];
        else return Usage();
    }
    if (n != 2) return Usage();
    if (!IsHex(args[1], 64)) {
        ToolErr("pin: %s is not a sha256 (64 lower-case hex digits)", args[1]);
        return 1;
    }
    sqlite3 *db = NULL;
    if (OpenDb(o->db, OPEN_WRITE, &db, NULL) != 0) return 1;
    Row r;
    char *indexedAt = NULL;
    int rc = 0;
    if (LoadRow(db, args[0], &r, &indexedAt) != 0) {
        ToolErr("pin: no model with id %s", args[0]);
        rc = 1;
    } else if (r.v[C_FILE_HASH].t == 's' && strcmp(r.v[C_FILE_HASH].s, args[1]) == 0 &&
               strcmp(Text(&r, C_HASH_KIND), "sha256") == 0) {
        ToolErr("pin: %s already has that sha256", args[0]);
    } else if (r.v[C_FILE_HASH].t == 's' && !force) {
        ToolErr("pin: %s already has %s %s; --force replaces it", args[0], Text(&r, C_HASH_KIND), r.v[C_FILE_HASH].s);
        rc = 1;
    } else {
        if (r.v[C_FILE_HASH].t == 's') {
            ToolErr("pin: %s: replacing %s %s", args[0], Text(&r, C_HASH_KIND), r.v[C_FILE_HASH].s);
        }
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, "UPDATE models SET file_hash = ?1, hash_kind = 'sha256', indexed_at = ?2 WHERE id = ?3",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, args[1], -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, Today(o), -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 3, args[0], -1, SQLITE_STATIC);
            rc = sqlite3_step(st) == SQLITE_DONE ? 0 : SqlFail(db, "pin");
        } else {
            rc = SqlFail(db, "pin");
        }
        sqlite3_finalize(st);
        if (rc == 0) ToolErr("pin: %s: sha256 %s", args[0], args[1]);
    }
    RowFree(&r);
    free(indexedAt);
    sqlite3_close(db);
    return rc;
}

// dump: every row of `models` by id as a block of `key: value` lines (no vectors: they follow from the texts
// and the GGUF), a blank line after each, then `meta` as `meta.<key>: value` lines by key. Exit 0, 1.
static int CmdDump(Options *o, int argc, char **argv)
{
    (void)argv;
    if (argc != 0) return Usage();
    sqlite3 *db = NULL;
    if (OpenDb(o->db, OPEN_READ, &db, NULL) != 0) return 1;
    char *cols = ColumnList();
    char *sql = Strf("SELECT rowid, embed_hash, indexed_at, %s FROM models ORDER BY id", cols);
    free(cols);
    sqlite3_stmt *st = NULL;
    int rc = 0;
    if (!sql || sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        rc = SqlFail(db, "dump");
    } else {
        while (sqlite3_step(st) == SQLITE_ROW) {
            Row r = {0};
            r.embedHash = Dup((const char *)sqlite3_column_text(st, 1));
            ReadValues(st, 3, &r);
            PrintRow(&r, (const char *)sqlite3_column_text(st, 2));
            fputc('\n', Out());
            RowFree(&r);
        }
    }
    sqlite3_finalize(st);
    free(sql);
    if (rc == 0 && sqlite3_prepare_v2(db, "SELECT key, value FROM meta ORDER BY key", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            fprintf(Out(), "meta.%s: %s\n", sqlite3_column_text(st, 0), sqlite3_column_text(st, 1));
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return rc;
}

// check: the schema and schema_version; meta against the GGUF on disk (not with --embedder stub); every row
// with three vectors and a current embed_hash; the keyword index against `models` (FTS5 integrity-check);
// SQLite's integrity check; every row through the ingest checks. Lists every problem. Exit 0 (none), 1.
static int CmdCheck(Options *o, int argc, char **argv)
{
    (void)argv;
    if (argc != 0) return Usage();
    // read-write only because FTS5's integrity-check is spelled as an INSERT; it runs in a transaction that
    // is rolled back, and nothing else here writes
    sqlite3 *db = NULL;
    if (OpenDb(o->db, OPEN_WRITE, &db, NULL) != 0) return 1;
    Strs problems = {0};

    // the schema: each object's SQL as this program creates it
    for (size_t i = 0; i < NOB_ARRAY_LEN(SCHEMA); i++) {
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(db, "SELECT sql FROM sqlite_master WHERE name = ?1", -1, &st, NULL);
        sqlite3_bind_text(st, 1, SCHEMA_NAMES[i], -1, SQLITE_STATIC);
        if (sqlite3_step(st) != SQLITE_ROW) Problem(&problems, "schema: %s is missing", SCHEMA_NAMES[i]);
        else if (strcmp((const char *)sqlite3_column_text(st, 0), SCHEMA[i]) != 0) {
            Problem(&problems, "schema: %s differs from schema version 1", SCHEMA_NAMES[i]);
        }
        sqlite3_finalize(st);
    }
    // meta: every key a new database gets, schema_version 1
    for (size_t i = 0; i < NOB_ARRAY_LEN(META_INITIAL); i++) {
        char *v = MetaGet(db, META_INITIAL[i][0]);
        if (!v) Problem(&problems, "meta: %s is missing", META_INITIAL[i][0]);
        else if (strcmp(META_INITIAL[i][0], "schema_version") == 0 && strcmp(v, "1") != 0) {
            Problem(&problems, "meta: schema_version is %s, this program reads 1", v);
        }
        free(v);
    }
    // the GGUF on disk against meta (only the hash: no model load), and who made the vectors
    char *embedder = MetaGet(db, "embedder");
    if (!o->stub) {
        char *file = MetaGet(db, "embed_file"), *sha = MetaGet(db, "embed_sha256"), *path = NULL;
        if (CheckGguf(file ? file : EMBED_FILE, sha ? sha : EMBED_SHA256, &path) != 0) {
            Problem(&problems, "the embedding model on disk is missing or differs from meta.embed_sha256 (see above)");
        }
        free(file);
        free(sha);
        free(path);
        if (embedder && strcmp(embedder, "llama") != 0) Problem(&problems, "meta: the vectors were made by the %s embedder", embedder);
    }
    free(embedder);
    // every row: the ingest checks, three vectors, a current embed_hash
    Rows rows = {0};
    if (LoadRows(db, &rows) != 0) Problem(&problems, "models can't be read");
    for (size_t i = 0; i < rows.count; i++) {
        Row *r = &rows.items[i];
        ValidateRow(r, "row", &problems);
        char hash[65];
        RowEmbedHash(r, hash);
        if (!r->embedHash) Problem(&problems, "row: %s: no vectors (run: build/tools/library embed)", Text(r, C_ID));
        else if (strcmp(r->embedHash, hash) != 0) Problem(&problems, "row: %s: embed_hash is stale (run: build/tools/library embed)", Text(r, C_ID));
        for (int k = 0; k < 3 && r->embedHash; k++) {
            char *sql = Strf("SELECT count(*) FROM %s WHERE rowid = ?1", VEC_TABLES[k]);
            sqlite3_stmt *st = NULL;
            long long have = 0;
            if (sql && sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, r->rowid);
                if (sqlite3_step(st) == SQLITE_ROW) have = sqlite3_column_int64(st, 0);
            }
            sqlite3_finalize(st);
            free(sql);
            if (have != 1) Problem(&problems, "row: %s: no vector in %s", Text(r, C_ID), VEC_TABLES[k]);
        }
    }
    // vectors without a row
    for (int k = 0; k < 3; k++) {
        char *sql = Strf("SELECT count(*) FROM %s WHERE rowid NOT IN (SELECT rowid FROM models)", VEC_TABLES[k]);
        sqlite3_stmt *st = NULL;
        if (sql && sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW &&
            sqlite3_column_int64(st, 0) > 0) {
            Problem(&problems, "%s holds %lld vectors of rows that are gone", VEC_TABLES[k], sqlite3_column_int64(st, 0));
        }
        sqlite3_finalize(st);
        free(sql);
    }
    // the keyword index against its content table, and SQLite's own check
    char *err = NULL;
    Exec(db, "BEGIN");
    if (sqlite3_exec(db, "INSERT INTO models_fts(models_fts, rank) VALUES('integrity-check', 1)", NULL, NULL, &err) != SQLITE_OK) {
        Problem(&problems, "the keyword index doesn't match models: %s", err ? err : "error");
    }
    sqlite3_free(err);
    Exec(db, "ROLLBACK");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *line = (const char *)sqlite3_column_text(st, 0);
            if (line && strcmp(line, "ok") != 0) Problem(&problems, "integrity_check: %s", line);
        }
    }
    sqlite3_finalize(st);

    for (size_t i = 0; i < problems.count; i++) ToolErr("check: %s", problems.items[i]);
    int rc = problems.count == 0 ? 0 : 1;
    if (rc == 0) ToolErr("check: OK: %zu rows", rows.count);
    else ToolErr("check: %zu problem%s", problems.count, problems.count == 1 ? "" : "s");
    RowsFree(&rows);
    StrsFree(&problems);
    sqlite3_close(db);
    return rc;
}

// Reads a JSON value that is a string or an array of strings into `to` (checked against `list` when given).
// false (with a problem) for anything else.
static bool JsonStrings(Json *j, const char *jt, const char *atom, const char *value, Strs *to, const char **list,
                        size_t n, const char *what, size_t lineNo, Strs *problems)
{
    Strs got = {0};
    if (strcmp(jt, "text") == 0) {
        nob_da_append(&got, strdup(atom));
    } else if (strcmp(jt, "array") == 0) {
        // a second statement on the same in-memory database (the outer json_each is still running)
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(j->db, "SELECT type, atom FROM json_each(?1)", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, value, -1, SQLITE_STATIC);
            while (sqlite3_step(st) == SQLITE_ROW) {
                if (strcmp((const char *)sqlite3_column_text(st, 0), "text") != 0) {
                    Problem(problems, "line %zu: %s: only strings", lineNo, what);
                    break;
                }
                nob_da_append(&got, strdup((const char *)sqlite3_column_text(st, 1)));
            }
        }
        sqlite3_finalize(st);
    } else {
        Problem(problems, "line %zu: %s: a string or a list of strings", lineNo, what);
    }
    bool ok = true;
    for (size_t i = 0; i < got.count; i++) {
        if (list && !InList(got.items[i], list, n)) {
            Problem(problems, "line %zu: %s: unknown value '%s'", lineNo, what, got.items[i]);
            ok = false;
        }
    }
    if (ok) {
        for (size_t i = 0; i < got.count; i++) nob_da_append(to, got.items[i]);
        free(got.items);
    } else {
        StrsFree(&got);
    }
    return ok;
}

// One query of an eval file.
typedef struct {
    char *name;      // "q"
    Query query;
    Strs expect;
} EvalQuery;

typedef struct {
    EvalQuery *items;
    size_t count, capacity;
} EvalQueries;

// Reads an eval file: one JSON object per line, {"q": name, "shape", "style", "use", "words": text or null,
// "filters": {"style_tag", "source": string or list, "max_tris": integer, "max_size": number,
// "cc0_only": bool}, "expect": [ids]}. Problems are appended. 0 read, 1 can't read the file.
static unsigned char ReadEval(const char *path, EvalQueries *out, Strs *problems)
{
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(path, &sb)) {
        ToolErr("can't read %s", path);
        return 1;
    }
    Json j;
    if (JsonOpen(&j) != 0) {
        nob_sb_free(sb);
        JsonClose(&j);
        return 1;
    }
    size_t lineNo = 0;
    for (size_t at = 0; at < sb.count;) {
        size_t end = at;
        while (end < sb.count && sb.items[end] != '\n') end++;
        lineNo++;
        size_t n = end - at;
        char *line = strndup(sb.items + at, n);
        at = end + 1;
        bool blank = true;
        for (const char *p = line; p && *p && blank; p++) blank = strchr(" \t\r", *p) != NULL;
        if (!line || blank) {
            free(line);
            continue;
        }
        EvalQuery e = {.query = {.limit = LIB_LIMIT, .weight = LIB_WEIGHTS}};
        char *type = JsonType(&j, line, strlen(line));
        if (!type || strcmp(type, "object") != 0) {
            Problem(problems, "line %zu: not a JSON object", lineNo);
        } else {
            sqlite3_stmt *st = NULL;
            sqlite3_prepare_v2(j.db, "SELECT key, type, atom, value FROM json_each(?1)", -1, &st, NULL);
            sqlite3_bind_text(st, 1, line, -1, SQLITE_STATIC);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *key = (const char *)sqlite3_column_text(st, 0);
                const char *jt = (const char *)sqlite3_column_text(st, 1);
                const char *atom = (const char *)sqlite3_column_text(st, 2);
                const char *value = (const char *)sqlite3_column_text(st, 3);
                bool isText = strcmp(jt, "text") == 0, isNull = strcmp(jt, "null") == 0;
                const char *textKeys[4] = {"shape", "style", "use", "words"};
                int t = 0;
                while (t < 4 && strcmp(key, textKeys[t]) != 0) t++;
                if (strcmp(key, "q") == 0 && isText) {
                    free(e.name);
                    e.name = strdup(atom);
                } else if (t < 4 && (isText || isNull)) {
                    char **slot = t < 3 ? &e.query.text[t] : &e.query.words;
                    free(*slot);
                    *slot = isText ? strdup(atom) : NULL;
                } else if (strcmp(key, "expect") == 0) {
                    JsonStrings(&j, jt, atom, value, &e.expect, NULL, 0, "expect", lineNo, problems);
                } else if (strcmp(key, "filters") == 0 && strcmp(jt, "object") == 0) {
                    sqlite3_stmt *f = NULL;
                    sqlite3_prepare_v2(j.db, "SELECT key, type, atom, value FROM json_each(?1)", -1, &f, NULL);
                    sqlite3_bind_text(f, 1, value, -1, SQLITE_STATIC);
                    while (sqlite3_step(f) == SQLITE_ROW) {
                        const char *fk = (const char *)sqlite3_column_text(f, 0);
                        const char *ft = (const char *)sqlite3_column_text(f, 1);
                        const char *fa = (const char *)sqlite3_column_text(f, 2);
                        const char *fv = (const char *)sqlite3_column_text(f, 3);
                        if (strcmp(fk, "style_tag") == 0) {
                            JsonStrings(&j, ft, fa, fv, &e.query.styleTags, STYLES, NOB_ARRAY_LEN(STYLES), "style_tag", lineNo, problems);
                        } else if (strcmp(fk, "source") == 0) {
                            JsonStrings(&j, ft, fa, fv, &e.query.sources, SOURCES, NOB_ARRAY_LEN(SOURCES), "source", lineNo, problems);
                        } else if (strcmp(fk, "max_tris") == 0 && strcmp(ft, "integer") == 0 && sqlite3_column_int64(f, 2) >= 0) {
                            e.query.hasMaxTris = true;
                            e.query.maxTris = sqlite3_column_int64(f, 2);
                        } else if (strcmp(fk, "max_size") == 0 && (strcmp(ft, "integer") == 0 || strcmp(ft, "real") == 0) &&
                                   sqlite3_column_double(f, 2) >= 0) {
                            e.query.hasMaxSize = true;
                            e.query.maxSize = sqlite3_column_double(f, 2);
                        } else if (strcmp(fk, "cc0_only") == 0 && (strcmp(ft, "true") == 0 || strcmp(ft, "false") == 0)) {
                            e.query.cc0Only = strcmp(ft, "true") == 0;
                        } else {
                            Problem(problems, "line %zu: filters: unknown or invalid '%s'", lineNo, fk);
                        }
                    }
                    sqlite3_finalize(f);
                } else {
                    Problem(problems, "line %zu: unknown or invalid key '%s'", lineNo, key);
                }
            }
            sqlite3_finalize(st);
            if (!e.name) Problem(problems, "line %zu: no \"q\"", lineNo);
            if (e.expect.count == 0) Problem(problems, "line %zu: no \"expect\" ids", lineNo);
            if (!e.query.text[0] && !e.query.text[1] && !e.query.text[2] && !e.query.words) {
                Problem(problems, "line %zu: no query text", lineNo);
            }
        }
        free(type);
        free(line);
        nob_da_append(out, e);
    }
    JsonClose(&j);
    nob_sb_free(sb);
    return 0;
}

// The 1-based position of the first expected id in a fused ranking (0: none of them in it), and how many of
// the expected ids are in its first 10.
static int FirstExpected(const FusedList *f, const Strs *expect, size_t *inTop10)
{
    int first = 0;
    *inTop10 = 0;
    for (size_t i = 0; i < f->count; i++) {
        bool hit = InList(f->items[i].id, (const char **)expect->items, expect->count);
        if (hit && first == 0) first = (int)i + 1;
        if (hit && i < 10) (*inTop10)++;
    }
    return first;
}

// eval <queries.jsonl>: per query, recall@10 of three rankings (vectors only: RRF of the vector lists; words
// only: the keyword list; RRF of all), the rank of the first expected id in each, the best-cos of the best
// expected id and of the best wrong answer in the RRF top 10; then a summary. Exit 0, 1, 2, 3.
static int CmdEval(Options *o, int argc, char **argv)
{
    // eval [--weights s,t,u,w] <queries.jsonl>: the weights replace every query's RRF weights (phase 4 tunes them)
    double weights[4];
    bool hasWeights = false;
    if (argc == 3 && strcmp(argv[0], "--weights") == 0) {
        if (!ParseWeights(argv[1], weights)) return Usage();
        hasWeights = true;
        argc -= 2;
        argv += 2;
    }
    if (argc != 1) return Usage();
    EvalQueries qs = {0};
    Strs problems = {0};
    int readRc = ReadEval(argv[0], &qs, &problems);
    if (hasWeights) {
        for (size_t i = 0; i < qs.count; i++) memcpy(qs.items[i].query.weight, weights, sizeof weights);
    }
    if (readRc != 0 || problems.count > 0) {
        for (size_t i = 0; i < problems.count; i++) ToolErr("eval: %s: %s", argv[0], problems.items[i]);
        for (size_t i = 0; i < qs.count; i++) {
            free(qs.items[i].name);
            QueryFree(&qs.items[i].query);
            StrsFree(&qs.items[i].expect);
        }
        free(qs.items);
        StrsFree(&problems);
        return 1;
    }
    sqlite3 *db = NULL;
    int rc = OpenDb(o->db, OPEN_READ, &db, NULL);
    Embedder e = {.stub = o->stub, .eosByTokenizer = -1};
    FILE *f = Out();
    // the sums for the summary: per method (vectors, words, rrf) the queries it ran on, recall@10, reciprocal rank
    double recallSum[3] = {0}, rrSum[3] = {0};
    size_t ran[3] = {0};
    double minExpectedCos = 2, maxWrongCos = -2;
    if (rc == 0) fprintf(f, "q\tvec_r@10\twords_r@10\trrf_r@10\tvec_rank\twords_rank\trrf_rank\texpected_cos\twrong_cos\n");
    for (size_t i = 0; i < qs.count && rc == 0; i++) {
        EvalQuery *q = &qs.items[i];
        Lists L;
        double embedSeconds;
        rc = SearchLists(db, &q->query, &e, &L, &embedSeconds);
        if (rc != 0) {
            ListsFree(&L);
            break;
        }
        // the three rankings: which lists each one fuses
        bool masks[3][4] = {{L.used[0], L.used[1], L.used[2], false}, {false, false, false, L.used[3]},
                            {L.used[0], L.used[1], L.used[2], L.used[3]}};
        char cols[3][2][32];
        FusedList all = {0};
        for (int m = 0; m < 3; m++) {
            FusedList fl = {0};
            Fuse(L.list, masks[m], q->query.weight, &fl);
            bool any = masks[m][0] || masks[m][1] || masks[m][2] || masks[m][3];
            size_t top = 0;
            int first = FirstExpected(&fl, &q->expect, &top);
            if (any) {
                double recall = (double)top / (double)q->expect.count;
                recallSum[m] += recall;
                rrSum[m] += first ? 1.0 / first : 0;
                ran[m]++;
                snprintf(cols[m][0], sizeof cols[m][0], "%.2f", recall);
                if (first) snprintf(cols[m][1], sizeof cols[m][1], "%d", first);
                else snprintf(cols[m][1], sizeof cols[m][1], "-");
            } else {
                snprintf(cols[m][0], sizeof cols[m][0], "-");
                snprintf(cols[m][1], sizeof cols[m][1], "-");
            }
            if (m == 2) all = fl;
            else free(fl.items);
        }
        // best-cos of the best-ranked expected id, and the highest best-cos of a wrong answer in the top 10
        char expCos[32] = "-", wrongCos[32] = "-";
        for (size_t k = 0; k < all.count; k++) {
            double c;
            bool expected = InList(all.items[k].id, (const char **)q->expect.items, q->expect.count);
            if (expected && strcmp(expCos, "-") == 0 && BestCos(db, &L, all.items[k].id, &c)) {
                snprintf(expCos, sizeof expCos, "%.4f", c);
                if (c < minExpectedCos) minExpectedCos = c;
            }
        }
        double worst = -2;
        for (size_t k = 0; k < all.count && k < 10; k++) {
            double c;
            if (!InList(all.items[k].id, (const char **)q->expect.items, q->expect.count) && BestCos(db, &L, all.items[k].id, &c) &&
                c > worst) {
                worst = c;
            }
        }
        if (worst > -2) {
            snprintf(wrongCos, sizeof wrongCos, "%.4f", worst);
            if (worst > maxWrongCos) maxWrongCos = worst;
        }
        fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", q->name, cols[0][0], cols[1][0], cols[2][0], cols[0][1], cols[1][1],
                cols[2][1], expCos, wrongCos);
        free(all.items);
        ListsFree(&L);
    }
    if (rc == 0) {
        // the summary table
        const char *names[3] = {"vectors", "words", "rrf"};
        fprintf(f, "\nmethod\tqueries\trecall@10\tmrr\n");
        for (int m = 0; m < 3; m++) {
            if (ran[m] == 0) fprintf(f, "%s\t0\t-\t-\n", names[m]);
            else fprintf(f, "%s\t%zu\t%.3f\t%.3f\n", names[m], ran[m], recallSum[m] / (double)ran[m], rrSum[m] / (double)ran[m]);
        }
        if (minExpectedCos <= 1) fprintf(f, "lowest expected best-cos\t%.4f\n", minExpectedCos);
        else fprintf(f, "lowest expected best-cos\t-\n");
        if (maxWrongCos >= -1) fprintf(f, "highest wrong best-cos\t%.4f\n", maxWrongCos);
        else fprintf(f, "highest wrong best-cos\t-\n");
    }
    for (size_t i = 0; i < qs.count; i++) {
        free(qs.items[i].name);
        QueryFree(&qs.items[i].query);
        StrsFree(&qs.items[i].expect);
    }
    free(qs.items);
    StrsFree(&problems);
    EmbedderClose(&e);
    sqlite3_close(db);
    return rc;
}

// #endregion

// #region main

// The program: global flags (anywhere on the line), then the command and its arguments. Returns the exit code
// (main returns it; the unit test calls this in-process).
static int LibraryMain(int argc, char **argv)
{
    Options o = {0};
    char *defaultDb = gToolRoot ? Strf("%s/library/library.db", gToolRoot) : NULL;
    o.db = defaultDb;
    time_t t = time(NULL);
    struct tm tm;
    if (localtime_r(&t, &tm)) strftime(o.today, sizeof o.today, "%Y-%m-%d", &tm);

    // the global flags out, the rest kept in order
    char **rest = calloc((size_t)argc + 1, sizeof *rest);
    int n = 0, code = 0;
    for (int i = 1; i < argc && code == 0; i++) {
        const char *a = argv[i];
        bool takes = strcmp(a, "--db") == 0 || strcmp(a, "--embedder") == 0 || strcmp(a, "--now") == 0;
        if (takes && i + 1 >= argc) {
            code = Usage();
        } else if (strcmp(a, "--db") == 0) {
            o.db = argv[++i];
        } else if (strcmp(a, "--embedder") == 0) {
            const char *v = argv[++i];
            if (strcmp(v, "stub") == 0) o.stub = true;
            else if (strcmp(v, "llama") == 0) o.stub = false;
            else code = Usage();
        } else if (strcmp(a, "--now") == 0) {
            o.now = argv[++i];
            if (!IsDate(o.now)) {
                ToolErr("--now %s: not a date YYYY-MM-DD", o.now);
                code = 2;
            }
        } else if (strcmp(a, "--timing") == 0) {
            o.timing = true;
        } else {
            rest[n++] = argv[i];
        }
    }
    if (code == 0 && (n == 0 || !o.db)) code = Usage();
    if (code == 0) {
        const char *cmd = rest[0];
        int cargc = n - 1;
        char **cargv = rest + 1;
        if (strcmp(cmd, "ingest") == 0) code = CmdIngest(&o, cargc, cargv);
        else if (strcmp(cmd, "embed") == 0) code = CmdEmbed(&o, cargc, cargv);
        else if (strcmp(cmd, "search") == 0) code = CmdSearch(&o, cargc, cargv);
        else if (strcmp(cmd, "show") == 0) code = CmdShow(&o, cargc, cargv);
        else if (strcmp(cmd, "pin") == 0) code = CmdPin(&o, cargc, cargv);
        else if (strcmp(cmd, "check") == 0) code = CmdCheck(&o, cargc, cargv);
        else if (strcmp(cmd, "dump") == 0) code = CmdDump(&o, cargc, cargv);
        else if (strcmp(cmd, "eval") == 0) code = CmdEval(&o, cargc, cargv);
        else code = Usage();
    }
    fflush(Out());
    free(rest);
    free(defaultDb);
    return code;
}

#ifndef LIBRARY_NO_MAIN
int main(int argc, char **argv)
{
    // the name for messages, the repo root (the default --db), and the rebuild through ./build.sh --tools
    // library when this file changed (the vendored libraries make it more than a one-file compile)
    ToolInitBuilt(argv, "library");
    return LibraryMain(argc, argv);
}
#endif

// #endregion
