// tests/unit/library_test.c: the model catalog tool (tools/library.c), included whole and run in-process
// with the stub embedder (no GGUF needed) on the fixture tests/library/catalog.jsonl. Covers the schema,
// ingest (byte-identical results, changed texts, pinned hashes, refused files), reciprocal rank fusion on a
// hand-made example, the search filters, int8 quantisation, stable search output and tie order, show's fetch
// commands, the colour / material word list, SHA-256, eval and the exit codes. Built and run by
// `./build.sh test library` (nob.c's UNIT_TESTS: ASan + UBSan, leaks fail it).
// Usage: library_test <scratch folder> <repo root>

#define LIBRARY_NO_MAIN
#include "../../tools/library.c"

// #region Harness

static int gChecks = 0, gFailed = 0;
static const char *gOutDir;   // the scratch folder (build/unit/library)
static const char *gRoot;     // the repo root

// One check: counted; a failure is printed with its line.
#define CHECK(cond, ...)                                                                                   \
    do {                                                                                                   \
        gChecks++;                                                                                         \
        if (!(cond)) {                                                                                     \
            gFailed++;                                                                                     \
            fprintf(stderr, "library_test: FAILED (line %d): ", __LINE__);                                 \
            fprintf(stderr, __VA_ARGS__);                                                                  \
            fputc('\n', stderr);                                                                           \
        }                                                                                                  \
    } while (0)

// "<scratch>/<name>" as a heap string.
static char *Path(const char *name)
{
    return Strf("%s/%s", gOutDir, name);
}

// Empties a file (an empty file is a new database for ingest): the test's scratch databases are reused
// between runs, never deleted.
static void Fresh(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

// A file's bytes (heap; *n its length), NULL when it can't be read.
static char *ReadAll(const char *path, size_t *n)
{
    Nob_String_Builder sb = {0};
    *n = 0;
    if (!nob_read_entire_file(path, &sb)) return NULL;
    *n = sb.count;
    nob_sb_append_null(&sb);
    return sb.items;
}

// Whether two files hold the same bytes.
static bool SameBytes(const char *a, const char *b)
{
    size_t na, nb;
    char *x = ReadAll(a, &na), *y = ReadAll(b, &nb);
    bool same = x && y && na == nb && memcmp(x, y, na) == 0;
    free(x);
    free(y);
    return same;
}

// Writes text into a file.
static void WriteFile(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

// Runs the program in-process with the arguments (NULL-terminated, after the program name); its data output
// (gLibOut) goes to a file whose text is returned in *out (heap; may be NULL). Returns the exit code.
static int Run(char **out, ...)
{
    Strs args = {0};
    nob_da_append(&args, strdup("library"));
    va_list ap;
    va_start(ap, out);
    for (const char *a = va_arg(ap, const char *); a; a = va_arg(ap, const char *)) nob_da_append(&args, strdup(a));
    va_end(ap);
    char *outPath = Path("out.txt");
    gLibOut = fopen(outPath, "w");
    int code = LibraryMain((int)args.count, args.items);
    fclose(gLibOut);
    gLibOut = NULL;
    size_t n;
    char *text = ReadAll(outPath, &n);
    if (out) *out = text;
    else free(text);
    free(outPath);
    StrsFree(&args);
    return code;
}

// Runs one SQL query that returns one value and gives it as a heap string (NULL for SQL NULL or no row).
static char *Query1(const char *dbPath, const char *sql)
{
    sqlite3 *db = NULL;
    char *v = NULL;
    if (OpenDb(dbPath, OPEN_READ, &db, NULL) != 0) return NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        v = Dup((const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return v;
}

// Whether Query1's answer equals `want` (NULL: SQL NULL or no row).
static bool QueryIs(const char *dbPath, const char *sql, const char *want)
{
    char *v = Query1(dbPath, sql);
    bool same = (v == NULL && want == NULL) || (v && want && strcmp(v, want) == 0);
    if (!same) fprintf(stderr, "library_test: %s -> %s (want %s)\n", sql, v ? v : "NULL", want ? want : "NULL");
    free(v);
    return same;
}

// The ids in search output (the first field of each line), joined by spaces (heap).
static char *Ids(const char *text)
{
    Nob_String_Builder sb = {0};
    for (const char *p = text ? text : ""; *p;) {
        const char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
        if (!nl) break;
        if (tab && tab < nl) {
            if (sb.count) nob_da_append(&sb, ' ');
            nob_sb_append_buf(&sb, p, (size_t)(tab - p));
        }
        p = nl + 1;
    }
    nob_sb_append_null(&sb);
    return sb.items;
}

// The fixture with one line's text replaced (heap): `from` must occur once.
static char *Replace(const char *text, const char *from, const char *to)
{
    const char *at = strstr(text, from);
    if (!at) return strdup(text);
    return Strf("%.*s%s%s", (int)(at - text), text, to, at + strlen(from));
}

// #endregion

// #region Tests

// The schema: every object as SCHEMA has it, meta, the rows; check fails without vectors and passes after
// embed.
static void TestSchema(const char *fixture)
{
    char *db = Path("schema.db");
    Fresh(db);
    CHECK(Run(NULL, "--db", db, "--now", "2026-11-20", "ingest", fixture, NULL) == 0, "ingest the fixture");
    for (size_t i = 0; i < NOB_ARRAY_LEN(SCHEMA); i++) {
        char *sql = Strf("SELECT sql FROM sqlite_master WHERE name = '%s'", SCHEMA_NAMES[i]);
        CHECK(QueryIs(db, sql, SCHEMA[i]), "schema object %s", SCHEMA_NAMES[i]);
        free(sql);
    }
    CHECK(QueryIs(db, "SELECT value FROM meta WHERE key = 'schema_version'", "1"), "schema_version 1");
    CHECK(QueryIs(db, "SELECT value FROM meta WHERE key = 'dims'", "512"), "meta dims");
    CHECK(QueryIs(db, "SELECT value FROM meta WHERE key = 'quantisation'", "maxabs-127"), "meta quantisation");
    CHECK(QueryIs(db, "SELECT count(*) FROM models", "9"), "nine rows");
    CHECK(QueryIs(db, "SELECT count(*) FROM models WHERE embed_hash IS NULL", "9"), "no vectors yet");
    CHECK(QueryIs(db, "SELECT group_concat(id, ' ') FROM (SELECT id FROM models ORDER BY rowid)",
                  "kenney-city-kit-roads/streetlight kenney-nature-kit/rock_largeA kenney-nature-kit/tree_pineTallA "
                  "polyfork-crate-small polyfork-fox-base polyfork-lamp-post polyhaven-boulder_01 polyhaven-pine_tree_01 "
                  "polyhaven-wooden_crate_01"),
          "rowids in id order");
    CHECK(QueryIs(db, "SELECT tris FROM models WHERE id = 'polyhaven-boulder_01'", NULL), "NULL tris kept");
    CHECK(QueryIs(db, "SELECT sx FROM models WHERE id = 'kenney-nature-kit/rock_largeA'", "1.2"), "a size");
    CHECK(QueryIs(db, "SELECT rigged FROM models WHERE id = 'polyfork-fox-base'", "1"), "a flag");
    CHECK(Run(NULL, "--db", db, "--embedder", "stub", "check", NULL) == 1, "check without vectors fails");
    CHECK(Run(NULL, "--db", db, "--embedder", "stub", "embed", NULL) == 0, "stub embed");
    CHECK(QueryIs(db, "SELECT value FROM meta WHERE key = 'embedder'", "stub"), "meta embedder");
    CHECK(QueryIs(db, "SELECT count(*) FROM vec_shape", "9"), "nine shape vectors");
    CHECK(QueryIs(db, "SELECT count(*) FROM vec_use", "9"), "nine use vectors");
    CHECK(Run(NULL, "--db", db, "--embedder", "stub", "check", NULL) == 0, "check passes");
    // embed with nothing to do leaves the file as it was
    char *copy = Path("schema-copy.db");
    size_t n;
    char *bytes = ReadAll(db, &n);
    FILE *f = fopen(copy, "wb");
    if (f && bytes) fwrite(bytes, 1, n, f);
    if (f) fclose(f);
    free(bytes);
    CHECK(Run(NULL, "--db", db, "--embedder", "stub", "embed", NULL) == 0, "embed again");
    CHECK(SameBytes(db, copy), "embed with nothing to do writes nothing");
    free(copy);
    free(db);
}

// Ingest is reproducible: two fresh databases from the same file are byte-identical, and ingesting the same
// file again leaves the database byte for byte as it was.
static void TestIngestIdentical(const char *fixture)
{
    char *a = Path("same-a.db"), *b = Path("same-b.db"), *c = Path("same-c.db");
    Fresh(a);
    Fresh(b);
    Fresh(c);
    CHECK(Run(NULL, "--db", a, "--now", "2026-11-20", "ingest", fixture, NULL) == 0, "ingest a");
    CHECK(Run(NULL, "--db", b, "--now", "2026-11-20", "ingest", fixture, NULL) == 0, "ingest b");
    CHECK(SameBytes(a, b), "two ingests of the same file are byte-identical");
    CHECK(Run(NULL, "--db", c, "--now", "2026-11-20", "ingest", fixture, NULL) == 0, "ingest c");
    CHECK(Run(NULL, "--db", c, "--now", "2026-12-01", "ingest", fixture, NULL) == 0, "ingest c again, a later day");
    CHECK(SameBytes(a, c), "ingesting the same file again changes nothing");
    free(a);
    free(b);
    free(c);
}

// A changed embedded text clears that row's vectors and embed_hash; a change elsewhere (tags) keeps them;
// both get the new indexed_at; unchanged rows keep theirs. --prune removes rows the file no longer lists.
static void TestChanges(const char *fixture, const char *fixtureText)
{
    char *db = Path("changes.db");
    Fresh(db);
    CHECK(Run(NULL, "--db", db, "--now", "2026-11-20", "ingest", fixture, NULL) == 0, "ingest");
    CHECK(Run(NULL, "--db", db, "--embedder", "stub", "embed", NULL) == 0, "embed");
    char *t1 = Replace(fixtureText, "Large faceted rock, wider than tall", "Large faceted rock, much wider than tall");
    char *t2 = Replace(t1, "\"tags\": \"crate warehouse box\"", "\"tags\": \"crate warehouse box loot\"");
    char *changed = Path("changed.jsonl");
    WriteFile(changed, t2);
    CHECK(Run(NULL, "--db", db, "--now", "2026-11-21", "ingest", changed, NULL) == 0, "ingest the changed file");
    CHECK(QueryIs(db, "SELECT embed_hash FROM models WHERE id = 'kenney-nature-kit/rock_largeA'", NULL),
          "changed text clears embed_hash");
    CHECK(QueryIs(db, "SELECT count(*) FROM vec_shape WHERE rowid = (SELECT rowid FROM models WHERE id = 'kenney-nature-kit/rock_largeA')", "0"),
          "changed text clears the vectors");
    CHECK(QueryIs(db, "SELECT count(*) FROM vec_shape", "8"), "the other vectors stay");
    CHECK(QueryIs(db, "SELECT indexed_at FROM models WHERE id = 'kenney-nature-kit/rock_largeA'", "2026-11-21"), "new date (text)");
    CHECK(QueryIs(db, "SELECT embed_hash IS NOT NULL FROM models WHERE id = 'polyfork-crate-small'", "1"), "tags keep vectors");
    CHECK(QueryIs(db, "SELECT indexed_at FROM models WHERE id = 'polyfork-crate-small'", "2026-11-21"), "new date (tags)");
    CHECK(QueryIs(db, "SELECT indexed_at FROM models WHERE id = 'polyfork-fox-base'", "2026-11-20"), "unchanged keeps its date");
    CHECK(QueryIs(db, "SELECT count(*) FROM models_fts WHERE models_fts MATCH 'loot'", "1"), "keyword index rebuilt");
    CHECK(Run(NULL, "--db", db, "--embedder", "stub", "embed", "--missing", NULL) == 0, "embed --missing");
    CHECK(QueryIs(db, "SELECT count(*) FROM vec_use", "9"), "vectors back");
    CHECK(Run(NULL, "--db", db, "--embedder", "stub", "check", NULL) == 0, "check after the change");
    // the other embedder's vectors: a search with the llama embedder refuses them (exit 1)
    CHECK(Run(NULL, "--db", db, "search", "--shape", "tree", NULL) == 1, "search with the other embedder");
    // --prune: a file of only the kenney rows prunes nothing of the other sources; without one kenney row it
    // prunes that row
    Nob_String_Builder kenney = {0};
    for (const char *p = fixtureText; *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) + 1 : strlen(p);
        if (strstr(p, "\"source\": \"kenney\"") && strstr(p, "\"source\": \"kenney\"") < p + n &&
            !(strstr(p, "rock_largeA") && strstr(p, "rock_largeA") < p + n)) {
            nob_sb_append_buf(&kenney, p, n);
        }
        p += n;
    }
    nob_sb_append_null(&kenney);
    char *kenneyFile = Path("kenney.jsonl");
    WriteFile(kenneyFile, kenney.items);
    nob_sb_free(kenney);
    CHECK(Run(NULL, "--db", db, "--now", "2026-11-22", "ingest", kenneyFile, "--prune", NULL) == 0, "ingest --prune");
    CHECK(QueryIs(db, "SELECT count(*) FROM models", "8"), "one row pruned");
    CHECK(QueryIs(db, "SELECT count(*) FROM models WHERE id = 'kenney-nature-kit/rock_largeA'", "0"), "the right row pruned");
    CHECK(QueryIs(db, "SELECT count(*) FROM vec_shape", "8"), "its vectors pruned");
    CHECK(Run(NULL, "--db", db, "--embedder", "stub", "check", NULL) == 0, "check after prune");
    free(kenneyFile);
    free(t1);
    free(t2);
    free(changed);
    free(db);
}

// pin, and a NULL hash in the file keeping the pinned one; a different hash in the file replaces it.
static void TestPin(const char *fixture, const char *fixtureText)
{
    char *db = Path("pin.db");
    Fresh(db);
    const char *h1 = "1111111111111111111111111111111111111111111111111111111111111111";
    const char *h2 = "2222222222222222222222222222222222222222222222222222222222222222";
    CHECK(Run(NULL, "--db", db, "--now", "2026-11-20", "ingest", fixture, NULL) == 0, "ingest");
    CHECK(Run(NULL, "--db", db, "--now", "2026-11-21", "pin", "polyfork-fox-base", h1, NULL) == 0, "pin");
    CHECK(Run(NULL, "--db", db, "pin", "polyfork-fox-base", h2, NULL) == 1, "pin over another hash needs --force");
    CHECK(Run(NULL, "--db", db, "pin", "polyfork-fox-base", "abc", NULL) == 1, "pin refuses a bad hash");
    CHECK(Run(NULL, "--db", db, "pin", "polyfork-nothing", h1, NULL) == 1, "pin refuses an unknown id");
    CHECK(Run(NULL, "--db", db, "--now", "2026-11-22", "ingest", fixture, NULL) == 0, "ingest again (hash NULL)");
    CHECK(QueryIs(db, "SELECT file_hash || ' ' || hash_kind FROM models WHERE id = 'polyfork-fox-base'",
                  "1111111111111111111111111111111111111111111111111111111111111111 sha256"),
          "a NULL hash in the file keeps the pinned one");
    CHECK(QueryIs(db, "SELECT indexed_at FROM models WHERE id = 'polyfork-fox-base'", "2026-11-21"), "and its date");
    char *withHash = Replace(fixtureText, "\"id\": \"polyfork-fox-base\", \"source\": \"polyfork\", \"name\": \"Fox\", \"collection\": \"animals\", \"page_url\": \"https://polyfork.dev/asset/fox-base\", \"file_url\": \"https://polyfork.dev/cdn/fox-base.glb\", \"path_in_zip\": null, \"file_hash\": null, \"hash_kind\": null",
                             "\"id\": \"polyfork-fox-base\", \"source\": \"polyfork\", \"name\": \"Fox\", \"collection\": \"animals\", \"page_url\": \"https://polyfork.dev/asset/fox-base\", \"file_url\": \"https://polyfork.dev/cdn/fox-base.glb\", \"path_in_zip\": null, \"file_hash\": \"2222222222222222222222222222222222222222222222222222222222222222\", \"hash_kind\": \"sha256\"");
    CHECK(strcmp(withHash, fixtureText) != 0, "the fixture line for the fox was found");
    char *file = Path("hash.jsonl");
    WriteFile(file, withHash);
    CHECK(Run(NULL, "--db", db, "--now", "2026-11-23", "ingest", file, NULL) == 0, "ingest with another hash");
    CHECK(QueryIs(db, "SELECT file_hash FROM models WHERE id = 'polyfork-fox-base'", h2), "a different hash replaces it");
    CHECK(Run(NULL, "--db", db, "pin", "polyfork-fox-base", h1, "--force", NULL) == 0, "pin --force");
    CHECK(QueryIs(db, "SELECT file_hash FROM models WHERE id = 'polyfork-fox-base'", h1), "forced");
    free(file);
    free(withHash);
    free(db);
}

// Invalid lines refuse the whole file with nothing written (an existing database keeps its bytes; a new one
// isn't written), each problem named.
static void TestRefused(const char *fixture, const char *fixtureText)
{
    char *db = Path("refused.db"), *copy = Path("refused-copy.db");
    Fresh(db);
    Fresh(copy);
    CHECK(Run(NULL, "--db", db, "--now", "2026-11-20", "ingest", fixture, NULL) == 0, "ingest");
    CHECK(Run(NULL, "--db", copy, "--now", "2026-11-20", "ingest", fixture, NULL) == 0, "ingest the copy");
    // one problem per file: each must refuse it
    struct { const char *from, *to, *what; } bad[] = {
        {"\"tags\": \"nature rock large\"", "\"tags\": \"nature rock large\", \"colour\": \"grey\"", "unknown key"},
        {"\"name\": \"Fox\", ", "", "missing field"},
        {"\"licence\": \"CC0 1.0\"", "\"licence\": \"CC BY-NC 4.0\"", "licence outside the list"},
        {"\"style\": \"photoscan\"", "\"style\": \"gritty\"", "style outside the list"},
        {"\"attribution\": \"Fox by Polyfork (https://polyfork.dev/asset/fox-base), CC BY 4.0\"", "\"attribution\": null", "CC BY without attribution"},
        {"Knee-high four-legged animal", "Knee-high rusty four-legged animal", "a material word"},
        {"forest floors", "forest floors", "(placeholder)"},
        {"Forest floors, meadows", "Green forest floors, meadows", "a colour word"},
        {"\"style_desc\": \"Low facet count, flat-shaded planes, simplified silhouettes with blunt corners.\"",
         "\"style_desc\": \"PV\"", "a description that is a code, not a sentence"},
        {"\"tris\": 880", "\"tris\": \"880\"", "a text where a number goes"},
        {"\"rigged\": 1", "\"rigged\": 2", "a flag that isn't 0 or 1"},
        {"\"id\": \"polyfork-fox-base\"", "\"id\": \"polyfork-crate-small\"", "a duplicate id"},
        {"\"id\": \"polyfork-fox-base\"", "\"id\": \"kenney-fox-base\"", "an id without its source's prefix"},
    };
    char *file = Path("bad.jsonl");
    for (size_t i = 0; i < NOB_ARRAY_LEN(bad); i++) {
        if (strcmp(bad[i].from, bad[i].to) == 0) continue;
        char *text = Replace(fixtureText, bad[i].from, bad[i].to);
        CHECK(strcmp(text, fixtureText) != 0, "fixture text for '%s' found", bad[i].what);
        WriteFile(file, text);
        free(text);
        CHECK(Run(NULL, "--db", db, "--now", "2026-12-01", "ingest", file, NULL) == 1, "refused: %s", bad[i].what);
        CHECK(SameBytes(db, copy), "nothing written: %s", bad[i].what);
    }
    // not JSON, and a JSON array
    WriteFile(file, "{\"id\": \"polyhaven-x\"\n[1, 2]\n");
    CHECK(Run(NULL, "--db", db, "ingest", file, NULL) == 1, "refused: not JSON objects");
    CHECK(SameBytes(db, copy), "nothing written: not JSON");
    // a new database isn't written either
    char *never = Path("never.db");
    Fresh(never);
    CHECK(Run(NULL, "--db", never, "ingest", file, NULL) == 1, "refused into a new database");
    struct stat st;
    CHECK(stat(never, &st) == 0 && st.st_size == 0, "a new database stays empty");
    CHECK(Run(NULL, "--db", db, "ingest", NULL) == 2, "ingest without a file: usage");
    CHECK(Run(NULL, "--db", db, "ingest", "/nonexistent/missing.jsonl", NULL) == 1, "missing file");
    free(never);
    free(file);
    free(copy);
    free(db);
}

// Reciprocal rank fusion on a hand-made example: shape [A, B, C], words [B, D], weights 1 (then words 2):
// A = 1/61, B = 1/62 + 1/61, C = 1/63, D = 1/62 -> B, A, D, C; with the words weighted 2, D = 2/62 passes A.
static void TestRrf(void)
{
    char *ids[] = {"A", "B", "C", "D"};
    RankedList lists[4] = {0};
    Ranked shape[3] = {{ids[0], 0.1}, {ids[1], 0.2}, {ids[2], 0.3}};
    Ranked words[2] = {{ids[1], -5}, {ids[3], -4}};
    lists[0] = (RankedList){.items = shape, .count = 3};
    lists[3] = (RankedList){.items = words, .count = 2};
    bool use[4] = {true, false, false, true};
    double w[4] = {1, 1, 1, 1};
    FusedList f = {0};
    Fuse(lists, use, w, &f);
    CHECK(f.count == 4, "four fused ids");
    const char *order[] = {"B", "A", "D", "C"};
    double scores[] = {1.0 / 61 + 1.0 / 62, 1.0 / 61, 1.0 / 62, 1.0 / 63};
    for (size_t i = 0; i < 4 && i < f.count; i++) {
        CHECK(strcmp(f.items[i].id, order[i]) == 0, "RRF rank %zu is %s (got %s)", i + 1, order[i], f.items[i].id);
        CHECK(fabs(f.items[i].score - scores[i]) < 1e-15, "RRF score of %s: %.9f (want %.9f)", order[i], f.items[i].score, scores[i]);
    }
    CHECK(f.count == 4 && f.items[0].rank[0] == 2 && f.items[0].rank[3] == 1, "B's ranks in the lists");
    double w2[4] = {1, 1, 1, 2};
    Fuse(lists, use, w2, &f);
    const char *order2[] = {"B", "D", "A", "C"};
    for (size_t i = 0; i < 4 && i < f.count; i++) CHECK(strcmp(f.items[i].id, order2[i]) == 0, "weighted rank %zu", i + 1);
    CHECK(f.count == 4 && fabs(f.items[0].score - (1.0 / 62 + 2.0 / 61)) < 1e-15, "weighted score of B");
    // an unused list is left out; equal scores sort by id
    Ranked tie[2] = {{ids[3], 0.5}, {ids[2], 0.5}};
    RankedList tl[4] = {{.items = tie, .count = 2}, {.items = tie, .count = 2}};
    bool use2[4] = {true, true, false, false};
    Ranked swapped[2] = {{ids[2], 0.5}, {ids[3], 0.5}};
    tl[1] = (RankedList){.items = swapped, .count = 2};
    Fuse(tl, use2, w, &f);
    CHECK(f.count == 2 && strcmp(f.items[0].id, "C") == 0 && strcmp(f.items[1].id, "D") == 0, "equal scores sort by id");
    free(f.items);
}

// The filters: every row matches the keyword query, so each filter's result is exactly the rows it lets
// through; NULL tris or size pass and print '?'.
static void TestFilters(const char *fixture)
{
    char *db = Path("filters.db");
    Fresh(db);
    CHECK(Run(NULL, "--db", db, "--now", "2026-11-20", "ingest", fixture, NULL) == 0, "ingest");
    const char *words = "nature city town warehouse animal props";
    struct { const char *flag, *value, *ids; } cases[] = {
        {NULL, NULL, "kenney-city-kit-roads/streetlight kenney-nature-kit/rock_largeA kenney-nature-kit/tree_pineTallA "
                     "polyfork-crate-small polyfork-fox-base polyfork-lamp-post polyhaven-boulder_01 polyhaven-pine_tree_01 "
                     "polyhaven-wooden_crate_01"},
        {"--max-tris", "100", "kenney-nature-kit/rock_largeA polyfork-crate-small polyfork-lamp-post polyhaven-boulder_01"},
        {"--max-size", "1", "polyfork-crate-small polyfork-fox-base polyhaven-boulder_01 polyhaven-wooden_crate_01"},
        {"--cc0-only", NULL, "kenney-city-kit-roads/streetlight kenney-nature-kit/rock_largeA kenney-nature-kit/tree_pineTallA "
                             "polyhaven-boulder_01 polyhaven-pine_tree_01 polyhaven-wooden_crate_01"},
        {"--source", "kenney", "kenney-city-kit-roads/streetlight kenney-nature-kit/rock_largeA kenney-nature-kit/tree_pineTallA"},
        {"--style-tag", "photoscan", "polyhaven-boulder_01 polyhaven-pine_tree_01"},
    };
    for (size_t i = 0; i < NOB_ARRAY_LEN(cases); i++) {
        char *out = NULL;
        int code = cases[i].flag && cases[i].value
                       ? Run(&out, "--db", db, "search", "--words", words, "--limit", "20", cases[i].flag, cases[i].value, NULL)
                   : cases[i].flag ? Run(&out, "--db", db, "search", "--words", words, "--limit", "20", cases[i].flag, NULL)
                                   : Run(&out, "--db", db, "search", "--words", words, "--limit", "20", NULL);
        CHECK(code == 0, "search %s", cases[i].flag ? cases[i].flag : "(no filter)");
        // sort the ids to compare sets
        char *ids = Ids(out);
        Strs list = {0};
        char *save = NULL;
        for (char *t = strtok_r(ids, " ", &save); t; t = strtok_r(NULL, " ", &save)) nob_da_append(&list, strdup(t));
        if (list.count > 1) qsort(list.items, list.count, sizeof *list.items, CompareStr);
        Nob_String_Builder sorted = {0};
        for (size_t k = 0; k < list.count; k++) {
            if (k) nob_da_append(&sorted, ' ');
            nob_sb_append_cstr(&sorted, list.items[k]);
        }
        nob_sb_append_null(&sorted);
        CHECK(strcmp(sorted.items, cases[i].ids) == 0, "filter %s %s: got [%s]", cases[i].flag ? cases[i].flag : "-",
              cases[i].value ? cases[i].value : "", sorted.items);
        // without vector lists, best-cos is '-'; NULL tris / size print '?'
        if (out && strstr(out, "polyhaven-boulder_01\t")) {
            CHECK(strstr(out, "polyhaven-boulder_01\t") && strstr(strstr(out, "polyhaven-boulder_01\t"), "\t-\t?\t?\tboulder\t"),
                  "boulder's NULL tris and size print '?'");
        }
        if (out && strstr(out, "polyfork-lamp-post\t")) {
            CHECK(strstr(strstr(out, "polyfork-lamp-post\t"), "\t-\t?\t0.40\xc3\x97" "3.20\xc3\x97" "0.40\tstreet lamp\t") != NULL,
                  "lamp post's NULL tris prints '?', its size the numbers");
        }
        nob_sb_free(sorted);
        StrsFree(&list);
        free(ids);
        free(out);
    }
    // usage errors: no query text, an unknown style tag or source, a bad number
    CHECK(Run(NULL, "--db", db, "search", NULL) == 2, "search without text");
    CHECK(Run(NULL, "--db", db, "search", "--words", "x", "--style-tag", "gritty", NULL) == 2, "unknown style tag");
    CHECK(Run(NULL, "--db", db, "search", "--words", "x", "--source", "sketchfab", NULL) == 2, "unknown source");
    CHECK(Run(NULL, "--db", db, "search", "--words", "x", "--max-tris", "-1", NULL) == 2, "bad --max-tris");
    CHECK(Run(NULL, "--db", db, "search", "--words", "x", "--weights", "1,1,1", NULL) == 2, "three weights");
    char *out = NULL;
    CHECK(Run(&out, "--db", db, "search", "--words", "zeppelin", NULL) == 0 && out && out[0] == '\0', "no results: exit 0, no lines");
    free(out);
    // FTS5 syntax in the request is only words
    CHECK(Run(&out, "--db", db, "search", "--words", "tree\" OR NEAR(crate AND -fox) * ^", NULL) == 0, "FTS5 syntax is quoted");
    free(out);
    free(db);
}

// int8 quantisation keeps the direction: cosine of the dequantised vector with the float one > 0.99, for
// the stub's uniform values and for a vector with a few large components.
static void TestQuantise(void)
{
    const char *texts[] = {"tree: Pine. Tall narrow conifer", "crate. Warehouses", "x"};
    for (size_t t = 0; t < NOB_ARRAY_LEN(texts) + 1; t++) {
        float x[LIB_DIMS];
        if (t < NOB_ARRAY_LEN(texts)) {
            StubFloats(texts[t], x);
        } else {
            // roughly bell-shaped values (sum of four stub values) with three outliers, like real embeddings
            float a[LIB_DIMS], b[LIB_DIMS], c[LIB_DIMS], d[LIB_DIMS];
            StubFloats("a", a);
            StubFloats("b", b);
            StubFloats("c", c);
            StubFloats("d", d);
            for (int i = 0; i < LIB_DIMS; i++) x[i] = (a[i] + b[i] + c[i] + d[i]) / 4;
            x[3] = 4;
            x[100] = -3;
            x[400] = 2.5f;
        }
        signed char q[LIB_DIMS];
        Quantise(x, q);
        double dot = 0, nx = 0, nq = 0;
        int maxq = 0;
        for (int i = 0; i < LIB_DIMS; i++) {
            dot += (double)x[i] * q[i];
            nx += (double)x[i] * x[i];
            nq += (double)q[i] * q[i];
            if (abs(q[i]) > maxq) maxq = abs(q[i]);
        }
        double cos = dot / (sqrt(nx) * sqrt(nq));
        CHECK(cos > 0.99, "int8 round trip %zu: cosine %.5f", t, cos);
        CHECK(maxq == 127, "the largest component maps to 127 (%d)", maxq);
    }
    // the same text, the same bytes
    signed char a[LIB_DIMS], b[LIB_DIMS];
    Embedder e = {.stub = true, .eosByTokenizer = -1};
    CHECK(Embed(&e, NULL, "tall conifer", a) == 0 && Embed(&e, INSTRUCT[0], "tall conifer", b) == 0, "stub embeds");
    CHECK(memcmp(a, b, LIB_DIMS) == 0, "the stub ignores the instruction and repeats itself");
    float zero[LIB_DIMS] = {0};
    Quantise(zero, a);
    bool allZero = true;
    for (int i = 0; i < LIB_DIMS; i++) allZero = allZero && a[i] == 0;
    CHECK(allZero, "a zero vector stays zero");
}

// Search output is the same on every run, and equal vectors rank by id: two rows with the same texts.
static void TestStable(const char *fixture, const char *fixtureText)
{
    char *db = Path("stable.db");
    Fresh(db);
    // two twins: the same category, name and descriptions, so the same vectors
    const char *twin = "{\"id\": \"polyfork-twin-%s\", \"source\": \"polyfork\", \"name\": \"Twin\", \"collection\": null, "
                       "\"page_url\": \"https://polyfork.dev/asset/twin-%s\", \"file_url\": \"https://polyfork.dev/cdn/twin-%s.glb\", "
                       "\"licence\": \"CC BY 4.0\", \"licence_url\": \"https://creativecommons.org/licenses/by/4.0/\", "
                       "\"attribution\": \"Twin by Polyfork (https://polyfork.dev/asset/twin-%s), CC BY 4.0\", "
                       "\"category\": \"bench\", \"style\": \"lowpoly\", \"style_desc\": \"Flat planes and hard edges.\", "
                       "\"shape_desc\": \"Long low seat on four legs.\", \"use_desc\": \"Parks and gardens.\", \"tags\": \"bench\"}\n";
    char *b = Strf(twin, "b", "b", "b", "b"), *a = Strf(twin, "a", "a", "a", "a");
    char *text = Strf("%s%s%s", fixtureText, b, a);
    char *file = Path("stable.jsonl");
    WriteFile(file, text);
    CHECK(Run(NULL, "--db", db, "--now", "2026-11-20", "ingest", file, NULL) == 0, "ingest with twins");
    CHECK(Run(NULL, "--db", db, "--embedder", "stub", "embed", NULL) == 0, "embed");
    char *o1 = NULL, *o2 = NULL;
    const char *query = "bench: Twin. Long low seat on four legs.";
    CHECK(Run(&o1, "--db", db, "--embedder", "stub", "search", "--shape", query, NULL) == 0, "search 1");
    CHECK(Run(&o2, "--db", db, "--embedder", "stub", "search", "--shape", query, NULL) == 0, "search 2");
    CHECK(o1 && o2 && strcmp(o1, o2) == 0, "the same output twice");
    CHECK(o1 && strncmp(o1, "polyfork-twin-a\t", 16) == 0, "the tie goes to the smaller id first");
    char *second = o1 ? strchr(o1, '\n') : NULL;
    CHECK(second && strncmp(second + 1, "polyfork-twin-b\t", 16) == 0, "then its twin");
    // the shape list (the twins tie at distance 0) and the words of --shape (the twins tie on bm25), with the
    // default weights (shape 1, words 3, LIB_WEIGHTS): twin-a is first in both, 1/61 + 3/61; twin-b second, 4/62
    CHECK(o1 && strstr(o1, "polyfork-twin-a\t0.065574\t1.0000\t?\t?\tbench\t") != NULL,
          "rank 1 in shape and words: 4/61, best-cos 1 (got %.80s)", o1 ? o1 : "");
    CHECK(o1 && strstr(o1, "polyfork-twin-b\t0.064516\t1.0000\t") != NULL, "rank 2 in both: 4/62");
    // the keyword list alone (no vector text): the weights pick which lists count
    char *o3 = NULL;
    CHECK(Run(&o3, "--db", db, "--embedder", "stub", "search", "--words", "bench", "--weights", "1,1,1,0", NULL) == 0,
          "weights 0 for words");
    CHECK(o3 && strstr(o3, "\t0.000000\t") != NULL, "a zero weight gives zero scores");
    free(o3);
    // the database's vectors and the run's embedder must agree
    CHECK(Run(NULL, "--db", db, "--embedder", "stub", "search", "--words", "bench", NULL) == 0, "words only need no embedder");
    free(o1);
    free(o2);
    free(file);
    free(text);
    free(a);
    free(b);
    (void)fixture;
    free(db);
}

// show prints the row and the right fetch command per source: md5 for Poly Haven, the row id (the local zip
// cache) and both hashes for Kenney, no hash and a note to pin for an unpinned Polyfork row, --expect-sha256 once pinned.
static void TestShow(const char *fixture)
{
    char *db = Path("show.db");
    Fresh(db);
    CHECK(Run(NULL, "--db", db, "--now", "2026-11-20", "ingest", fixture, NULL) == 0, "ingest");
    char *out = NULL;
    char *md5 = Query1(db, "SELECT file_hash FROM models WHERE id = 'polyhaven-pine_tree_01'");
    char *want = Strf("fetch: python3 -I tools/fetch_asset.py polyhaven get pine_tree_01 --expect-md5 %s\n", md5 ? md5 : "");
    CHECK(Run(&out, "--db", db, "show", "polyhaven-pine_tree_01", NULL) == 0, "show polyhaven");
    CHECK(out && strstr(out, want), "polyhaven fetch command");
    CHECK(out && strstr(out, "id: polyhaven-pine_tree_01\nsource: polyhaven\n") && strstr(out, "collection: NULL\n") &&
              strstr(out, "tris: 48210\n") && strstr(out, "indexed_at: 2026-11-20\n"),
          "show's key: value lines");
    free(out);
    free(want);
    free(md5);
    char *sha = Query1(db, "SELECT file_hash FROM models WHERE id = 'kenney-nature-kit/rock_largeA'");
    char *zip = Query1(db, "SELECT zip_sha256 FROM models WHERE id = 'kenney-nature-kit/rock_largeA'");
    want = Strf("fetch: python3 -I tools/fetch_asset.py kenney get kenney-nature-kit/rock_largeA --expect-sha256 %s "
                "--expect-zip-sha256 %s\n",
                sha ? sha : "", zip ? zip : "");
    CHECK(Run(&out, "--db", db, "show", "kenney-nature-kit/rock_largeA", NULL) == 0, "show kenney");
    CHECK(out && strstr(out, want), "kenney fetch command (got %s)", out ? out : "");
    free(out);
    free(want);
    free(sha);
    free(zip);
    CHECK(Run(&out, "--db", db, "show", "polyfork-fox-base", NULL) == 0, "show polyfork");
    CHECK(out && strstr(out, "fetch: python3 -I tools/fetch_asset.py polyfork get fox-base\n"), "polyfork fetch without a hash");
    CHECK(out && strstr(out, "note: no file hash is recorded; after the download record it with: build/tools/library pin polyfork-fox-base"),
          "the note to pin");
    free(out);
    const char *h = "abababababababababababababababababababababababababababababababab";
    CHECK(Run(NULL, "--db", db, "pin", "polyfork-fox-base", h, NULL) == 0, "pin");
    CHECK(Run(&out, "--db", db, "show", "polyfork-fox-base", NULL) == 0, "show pinned");
    want = Strf("fetch: python3 -I tools/fetch_asset.py polyfork get fox-base --expect-sha256 %s\n", h);
    CHECK(out && strstr(out, want) && !strstr(out, "note:"), "polyfork fetch with the pinned hash");
    free(out);
    free(want);
    CHECK(Run(NULL, "--db", db, "show", "polyfork-nothing", NULL) == 1, "show an unknown id: 1");
    CHECK(Run(NULL, "--db", "/nonexistent/missing.db", "show", "x", NULL) == 1, "no database: 1");
    // dump: one block per row by id, then meta
    CHECK(Run(&out, "--db", db, "dump", NULL) == 0, "dump");
    CHECK(out && strncmp(out, "id: kenney-city-kit-roads/streetlight\n", 38) == 0, "dump starts with the first id");
    CHECK(out && strstr(out, "\nmeta.schema_version: 1\n") && strstr(out, "\nmeta.dims: 512\n"), "dump ends with meta");
    free(out);
    free(db);
}

// The colour / material word list (BANNED_WORDS through BannedWord), and SHA-256.
static void TestWordsAndHash(const char *fixtureText)
{
    CHECK(BannedWord("A red box") && strcmp(BannedWord("A red box"), "red") == 0, "red");
    CHECK(BannedWord("WOODEN crate") && strcmp(BannedWord("WOODEN crate"), "wooden") == 0, "upper case");
    CHECK(BannedWord("half-rusty pipe") && strcmp(BannedWord("half-rusty pipe"), "rusty") == 0, "after a hyphen");
    CHECK(BannedWord("Tall narrow conifer: straight trunk, conical crown.") == NULL, "a clean description");
    CHECK(BannedWord("redwood, bluebell, steelyard, cobblestone") == NULL, "whole words only");
    CHECK(BannedWord("") == NULL, "empty");
    for (size_t i = 0; i < NOB_ARRAY_LEN(BANNED_WORDS); i++) {
        char *s = Strf("Some %s thing.", BANNED_WORDS[i]);
        CHECK(BannedWord(s) && strcmp(BannedWord(s), BANNED_WORDS[i]) == 0, "every listed word is found: %s", BANNED_WORDS[i]);
        free(s);
    }
    // the fixture's descriptions pass (they are the examples of the rule)
    Rows rows = {0};
    Strs problems = {0};
    char *file = Path("words.jsonl");
    WriteFile(file, fixtureText);
    CHECK(ReadJsonl(file, &rows, &problems) == 0 && problems.count == 0 && rows.count == 9, "the fixture is valid");
    for (size_t i = 0; i < problems.count; i++) fprintf(stderr, "library_test: %s\n", problems.items[i]);
    RowsFree(&rows);
    StrsFree(&problems);
    free(file);
    // SHA-256 test vectors (FIPS 180-2)
    struct { const char *in, *out; } v[] = {
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    for (size_t i = 0; i < NOB_ARRAY_LEN(v); i++) {
        Sha256 s;
        Sha256Init(&s);
        Sha256Update(&s, v[i].in, strlen(v[i].in));
        char hex[65];
        Sha256Hex(&s, hex);
        CHECK(strcmp(hex, v[i].out) == 0, "sha256(\"%s\")", v[i].in);
    }
}

// eval: the table on a few queries, and the exit codes of a bad file and of a missing GGUF.
static void TestEval(const char *fixture)
{
    char *db = Path("eval.db");
    Fresh(db);
    CHECK(Run(NULL, "--db", db, "--now", "2026-11-20", "ingest", fixture, NULL) == 0, "ingest");
    // a llama embed without the GGUF: 3 (MONOLITH_EMBED_DIR points at an empty folder)
    char *empty = Path("no-gguf");
    ToolMkdirP(empty);
    setenv("MONOLITH_EMBED_DIR", empty, 1);
    CHECK(Run(NULL, "--db", db, "embed", NULL) == 3, "embed without the GGUF: 3");
    CHECK(QueryIs(db, "SELECT count(*) FROM vec_shape", "0"), "nothing embedded");
    unsetenv("MONOLITH_EMBED_DIR");
    free(empty);
    CHECK(Run(NULL, "--db", db, "--embedder", "stub", "embed", NULL) == 0, "embed");
    char *queries = Path("queries.jsonl");
    WriteFile(queries,
              "{\"q\": \"exact-tree\", \"shape\": \"tree: tree_pineTallA. Tall narrow conifer: short trunk, conical crown of three stacked tiers.\", "
              "\"expect\": [\"kenney-nature-kit/tree_pineTallA\"]}\n"
              "{\"q\": \"words-crate\", \"words\": \"crate\", \"filters\": {\"source\": [\"polyfork\"], \"max_tris\": 1000, \"cc0_only\": false}, "
              "\"expect\": [\"polyfork-crate-small\"]}\n"
              "{\"q\": \"lamp\", \"use\": \"street lamp. Town squares, road sides, park paths.\", \"words\": \"lamp\", "
              "\"filters\": {\"style_tag\": \"lowpoly\", \"max_size\": 5}, \"expect\": [\"polyfork-lamp-post\", \"kenney-city-kit-roads/streetlight\"]}\n");
    char *out = NULL;
    CHECK(Run(&out, "--db", db, "--embedder", "stub", "eval", queries, NULL) == 0, "eval");
    CHECK(out && strncmp(out, "q\tvec_r@10\twords_r@10\trrf_r@10\tvec_rank\twords_rank\trrf_rank\texpected_cos\twrong_cos\n", 79) == 0,
          "eval header");
    CHECK(out && strstr(out, "\nexact-tree\t1.00\t1.00\t1.00\t1\t") != NULL, "an exact text finds its row first (got %s)", out ? out : "");
    CHECK(out && strstr(out, "\nwords-crate\t-\t1.00\t1.00\t-\t1\t1\t-\t-\n") != NULL, "a words-only query");
    CHECK(out && strstr(out, "\nlamp\t1.00\t") != NULL, "the use text finds the lamp");
    CHECK(out && strstr(out, "\nmethod\tqueries\trecall@10\tmrr\n") && strstr(out, "\nrrf\t3\t1.000\t"), "eval summary");
    free(out);
    WriteFile(queries, "{\"q\": \"x\", \"shape\": \"tree\", \"expect\": [], \"filters\": {\"colour\": 1}}\n");
    CHECK(Run(NULL, "--db", db, "--embedder", "stub", "eval", queries, NULL) == 1, "a bad eval file: 1");
    CHECK(Run(NULL, "--db", db, "--embedder", "stub", "eval", NULL) == 2, "eval without a file: 2");
    free(queries);
    free(db);
}

// #endregion

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: library_test <scratch folder> <repo root>\n");
        return 2;
    }
    gToolName = "library";
    gOutDir = argv[1];
    gRoot = argv[2];
    ToolMkdirP(gOutDir);
    char *fixture = Strf("%s/tests/library/catalog.jsonl", gRoot);
    size_t n;
    char *fixtureText = ReadAll(fixture, &n);
    if (!fixtureText) {
        fprintf(stderr, "library_test: can't read %s\n", fixture);
        free(fixture);
        return 1;
    }
    TestSchema(fixture);
    TestIngestIdentical(fixture);
    TestChanges(fixture, fixtureText);
    TestPin(fixture, fixtureText);
    TestRefused(fixture, fixtureText);
    TestRrf();
    TestFilters(fixture);
    TestQuantise();
    TestStable(fixture, fixtureText);
    TestShow(fixture);
    TestWordsAndHash(fixtureText);
    TestEval(fixture);
    free(fixtureText);
    free(fixture);
    fprintf(stderr, "library_test: %d checks, %d failed\n", gChecks, gFailed);
    return gFailed == 0 ? 0 : 1;
}
