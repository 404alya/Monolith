// tools/bench-export-ab.c: interleaved A/B benchmark pairs of two exported maps (monolith --export), each drawn
// by its own exported runtime (the game's frame time, not the editor's). It replaced tools/bench-export-ab.sh
// (2026-11-13) with the same arguments, output, files and exit codes; its table is the script's embedded Python,
// ported line by line (Summarize).
//
//   build/tools/bench-export-ab <base export> <new export> [--map <name>] [--pairs N] [--frames N]
//                               [--note "what changed"] [--out <dir>] [--dry-run]
//   build/tools/bench-export-ab --summarize <out dir>
//
// <base export> / <new export>: folders written by `monolith --export` (or F9). The program compiles each one's
// viewer.c with the test harness's bench mode (tests/examples/harness.h, force-included into viewer.c only)
// against that folder's own runtime (monolith_scene.c/.h) and R3D libraries (prebuilt/linux/lib, or
// build/r3d-linux/lib when the export's ./build.sh built them from source), into <out>/A/viewer-bench and
// <out>/B/viewer-bench. Then it runs A, B, A, B, ... (N pairs, default 3, at least 3) on the map package in each
// folder (--map <name>: the package folder; default the one folder holding a <name>.mapb), every run a
// hidden-window bench (`--bench <report>`, 1920 x 1006, glFinish per frame, the camera spinning at every
// viewpoint as in `monolith --bench`) with __GL_SYNC_TO_VBLANK=0 and vblank_mode=0, and prints per pair the
// overall average frame time of A and B and their difference, then the mean and median of the paired
// differences, overall and per viewpoint. Reports: <out>/A-<pair>.md, <out>/B-<pair>.md, each run's output in
// <out>/<A|B>-<pair>.log, the plan and every run's command in <out>/runs.txt, the table also in
// <out>/summary.txt (default <out>: benchmark/export-ab/<stamp> in the repo).
//   --map <name>     the package folder in both exports (default: the one folder <name>/ holding <name>.mapb)
//   --pairs N        the number of A/B pairs (default 3, at least 3)
//   --frames N       timed frames per viewpoint (default 600, the full run; 150 to screen)
//   --note "..."     what the two exports differ in (required); goes into every report
//   --out <dir>      the output folder (created)
//   --dry-run        builds both binaries and prints the commands (shell-quoted, as bash's printf %q) without
//                    running them
//   --summarize <d>  prints the table again from the reports in <d> (nothing is built or run)
//
// A benchmark like any other (the benchmarking skill): one at a time on the machine (one GPU), never beside
// another benchmark or shot, results relative to the base on the same machine. Linux only (the bench runs here;
// a Windows export is built with its ./build.sh --win as before).
//
// Environment: CC, the compiler of the two bench binaries (default cc, as an export's build.sh uses).
// Exit codes: 0 done (or planned, with --dry-run); 1 a build or run failed, an export or package wasn't found,
// --summarize found no report pairs; 2 usage (also a bad --pairs / --frames or a missing --note). A failing
// compile exits with the compiler's status, as the script's `set -e` did.
// Called by: the developer and the benchmarking skill (an export A/B); built by ./build.sh --tools.

#include "tool.h"

#include <glob.h>
#include <locale.h>
#include <math.h>
#include <regex.h>
#include <wchar.h>
#include <wctype.h>

// #region Usage and small helpers

// The usage text (the script printed lines 4-7 of its own header: a blank line and the synopsis) on stderr, then
// exit 2.
static void Usage(void)
{
    fprintf(stderr,
            "\n"
            "  build/tools/bench-export-ab <base export> <new export> [--map <name>] [--pairs N] [--frames N]\n"
            "                              [--note \"what changed\"] [--out <dir>] [--dry-run]\n"
            "  build/tools/bench-export-ab --summarize <out dir>\n");
    exit(2);
}

// `cd "$dir" && pwd`: the folder's absolute path as the shell names it (made absolute against the logical
// current directory and normalised lexically, symlinks kept). Exits 1 like the script's failed `cd` when it
// isn't a folder. Returns a heap string.
static char *AbsoluteDir(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ToolDie(1, "cd: %s: %s", dir, stat(dir, &st) != 0 ? strerror(errno) : "Not a directory");
    }
    if (dir[0] == '/') return ToolNormalize(dir);
    char *cwd = ToolLogicalCwd();
    if (!cwd) ToolDie(1, "can't read the current directory");
    char *joined = strdup(nob_temp_sprintf("%s/%s", cwd, dir));
    free(cwd);
    char *result = ToolNormalize(joined);
    free(joined);
    return result;
}

// [[ "$s" =~ ^[0-9]+$ ]]: only ASCII digits, at least one.
static bool AllDigits(const char *s)
{
    if (!*s) return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
    }
    return true;
}

// [[ -f path ]]: a regular file (following symlinks).
static bool IsFile(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// The text of `path` with `prefix` taken off its front when it starts with it (the shell's ${path#"$prefix"}).
static const char *StripPrefix(const char *path, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(path, prefix, n) == 0 ? path + n : path;
}

// Appends one word as bash's `printf %q` writes it (bash 5, lib/sh/shquote.c): '' for an empty word; $'...'
// with C escapes when it holds a character that isn't printable in the current locale (control characters,
// invalid multibyte text); otherwise the word with a backslash before each shell-special character, before a
// leading '#' or '~', and nothing else changed (printable multibyte characters stay as they are).
static void AppendShellQuoted(Nob_String_Builder *sb, const char *word)
{
    if (!*word) {
        nob_sb_append_cstr(sb, "''");
        return;
    }

    // ansic_shouldquote: any character that isn't printable -> the $'...' form
    bool ansi = false;
    mbstate_t state = {0};
    for (const char *p = word; *p && !ansi;) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, p, strlen(p), &state);
        if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
            ansi = true;
        } else {
            ansi = !iswprint((wint_t)wc);
            p += n;
        }
    }

    if (ansi) {
        // ansic_quote: $'...', the usual C escapes, printable characters as they are, everything else \ooo
        nob_sb_append_cstr(sb, "$'");
        memset(&state, 0, sizeof state);
        for (const char *p = word; *p;) {
            unsigned char c = (unsigned char)*p;
            const char *esc = NULL;
            switch (c) {
            case '\a': esc = "\\a"; break;
            case '\b': esc = "\\b"; break;
            case 0x1b: esc = "\\E"; break;
            case '\f': esc = "\\f"; break;
            case '\n': esc = "\\n"; break;
            case '\r': esc = "\\r"; break;
            case '\t': esc = "\\t"; break;
            case '\v': esc = "\\v"; break;
            case '\\': esc = "\\\\"; break;
            case '\'': esc = "\\'"; break;
            default: break;
            }
            if (esc) {
                nob_sb_append_cstr(sb, esc);
                p++;
                continue;
            }
            // a printable (possibly multibyte) character is copied whole; any other byte is written in octal
            wchar_t wc;
            size_t n = mbrtowc(&wc, p, strlen(p), &state);
            if (n != (size_t)-1 && n != (size_t)-2 && n != 0 && iswprint((wint_t)wc)) {
                nob_sb_append_buf(sb, p, n);
                p += n;
            } else {
                memset(&state, 0, sizeof state);
                nob_sb_appendf(sb, "\\%03o", c);
                p++;
            }
        }
        nob_da_append(sb, '\'');
        return;
    }

    // sh_backslash_quote: a backslash before the characters the shell treats specially
    static const char special[] = " \t\n!\"$&'()*,;<>?[\\]^`{|}";
    for (const char *p = word; *p; p++) {
        bool quote = *p != '\0' && strchr(special, *p) != NULL;
        if (p == word && (*p == '#' || *p == '~')) quote = true;   // a comment or a tilde at the start
        if (quote) nob_da_append(sb, '\\');
        nob_da_append(sb, *p);
    }
}

// #endregion

// #region Summarize: the paired table from <out>/A-<i>.md and B-<i>.md (the script's embedded Python)

// One view's average frame time from a report's Results table, in the order the table lists them (a Python
// dict keeps insertion order; a name seen again keeps its first place and takes the new value).
typedef struct {
    char *name;
    double ms;
} ViewMs;

typedef struct {
    ViewMs *items;
    size_t count, capacity;
} Views;

// One report: every view's average and the overall one ("**All**").
typedef struct {
    Views views;
    bool hasTotal;
    double total;
} Report;

// One pair: its number and the A and B reports.
typedef struct {
    long index;
    Report a, b;
} Pair;

typedef struct {
    Pair *items;
    size_t count, capacity;
} Pairs;

// A view's value in a report, or NULL when the report doesn't list it (Python's `v in views`).
static const double *ViewFind(const Views *v, const char *name)
{
    for (size_t i = 0; i < v->count; i++) {
        if (strcmp(v->items[i].name, name) == 0) return &v->items[i].ms;
    }
    return NULL;
}

// Python's float(text) for the numbers a report holds: optional blanks, a decimal number (or inf / nan), optional
// blanks, nothing else. False when the text isn't one (Python raised ValueError and the script stopped).
static bool ParseFloat(const char *text, double *out)
{
    while (*text == ' ' || *text == '\t') text++;
    if (!*text || (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))) return false;   // no hex floats
    char *end;
    errno = 0;
    double v = strtod(text, &end);
    if (end == text) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end) return false;
    *out = v;
    return true;
}

// str.strip(chars) of Python: the text between the first and last character not in `chars`, as a new heap string.
static char *StripChars(const char *s, size_t n, const char *chars)
{
    size_t a = 0, b = n;
    while (a < b && strchr(chars, s[a]) && s[a]) a++;
    while (b > a && strchr(chars, s[b - 1]) && s[b - 1]) b--;
    return strndup(s + a, b - a);
}

// results(path): reads a bench report's "## Results" table (the rows up to the next "## " heading; header and
// separator rows skipped): each row's first cell is the view's name, its second the average ms (bold markers
// stripped); the row named "**All**" is the overall average. Exits 1 with a message when the file can't be read
// or a row is malformed (the Python stopped with a traceback there).
static void ReadReport(const char *path, Report *r)
{
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(path, &sb)) ToolDie(1, "cannot read %s", path);
    nob_sb_append_null(&sb);
    static const char ws[] = " \t\n\r\v\f";
    bool rows = false;
    for (char *line = sb.items; line && *line;) {
        // one line (Python's universal newlines: "\r\n" ends a line like "\n")
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char *next = nl ? nl + 1 : NULL;
        if (len > 0 && line[len - 1] == '\r') len--;
        char *text = strndup(line, len);

        if (strncmp(text, "## Results", 10) == 0) {
            rows = true;
        } else if (rows && strncmp(text, "## ", 3) == 0) {
            free(text);
            break;
        } else if (rows && strncmp(text, "| ", 2) == 0 && strncmp(text, "| View", 6) != 0 &&
                   strncmp(text, "|---", 4) != 0) {
            // cells = [c.strip() for c in line.strip().strip("|").split("|")]
            char *trimmed = StripChars(text, strlen(text), ws);
            char *inner = StripChars(trimmed, strlen(trimmed), "|");
            char *bar = strchr(inner, '|');
            if (!bar) ToolDie(1, "%s: a Results row has no second cell: %s", path, text);
            char *name = StripChars(inner, (size_t)(bar - inner), ws);
            char *end = strchr(bar + 1, '|');
            char *cell = StripChars(bar + 1, end ? (size_t)(end - bar - 1) : strlen(bar + 1), ws);
            // avg = float(cells[1].strip("*"))
            char *num = StripChars(cell, strlen(cell), "*");
            double avg;
            if (!ParseFloat(num, &avg)) ToolDie(1, "%s: not a number in the Results table: '%s'", path, num);
            if (strcmp(name, "**All**") == 0) {
                r->hasTotal = true;
                r->total = avg;
                free(name);
            } else {
                bool seen = false;
                for (size_t i = 0; i < r->views.count && !seen; i++) {
                    if (strcmp(r->views.items[i].name, name) == 0) {
                        r->views.items[i].ms = avg;
                        seen = true;
                    }
                }
                if (seen) {
                    free(name);
                } else {
                    nob_da_append(&r->views, ((ViewMs){.name = name, .ms = avg}));
                }
            }
            free(trimmed);
            free(inner);
            free(cell);
            free(num);
        }
        free(text);
        line = next;
    }
    nob_sb_free(sb);
}

// The exact sum of a list of doubles, divided by n and rounded once to the nearest double (ties to even):
// Python's statistics.mean, which sums the values as exact fractions and converts only the result. A naive
// floating-point sum can differ from it in the last bit, which can change a printed digit; this keeps the
// table identical to the script's. Every double is an integer multiple of 2^-1074, so the sum is an integer in
// that unit: a fixed-point two's-complement number of LIMBS 32-bit limbs (2^1024 * 2^1074 * the count fits).
#define LIMBS 72
static double ExactMean(const double *values, size_t n)
{
    uint32_t acc[LIMBS] = {0};
    for (size_t i = 0; i < n; i++) {
        double x = values[i];
        if (!isfinite(x)) return NAN;   // (not in reports; Python would give nan or inf too)
        if (x == 0) continue;
        // x = m * 2^(e - 53) with m a 53-bit integer: add (or subtract) m shifted by e - 53 + 1074 bits
        int e;
        double frac = frexp(fabs(x), &e);
        uint64_t m = (uint64_t)ldexp(frac, 53);
        int shift = e - 53 + 1074;
        uint32_t add[LIMBS] = {0};
        int limb = shift / 32, bit = shift % 32;
        unsigned __int128 wide = (unsigned __int128)m << bit;
        for (int k = 0; k < 4 && limb + k < LIMBS; k++) add[limb + k] = (uint32_t)(wide >> (32 * k));
        if (x < 0) {
            // two's complement: invert and add one
            uint64_t carry = 1;
            for (int k = 0; k < LIMBS; k++) {
                uint64_t v = (uint64_t)(uint32_t)~add[k] + carry;
                add[k] = (uint32_t)v;
                carry = v >> 32;
            }
        }
        uint64_t carry = 0;
        for (int k = 0; k < LIMBS; k++) {
            uint64_t v = (uint64_t)acc[k] + add[k] + carry;
            acc[k] = (uint32_t)v;
            carry = v >> 32;
        }
    }

    // the sign, and the magnitude of the sum
    bool negative = acc[LIMBS - 1] >> 31;
    if (negative) {
        uint64_t carry = 1;
        for (int k = 0; k < LIMBS; k++) {
            uint64_t v = (uint64_t)(uint32_t)~acc[k] + carry;
            acc[k] = (uint32_t)v;
            carry = v >> 32;
        }
    }

    // divide by n, top limb first: quotient in acc, remainder in rem
    uint64_t rem = 0;
    for (int k = LIMBS - 1; k >= 0; k--) {
        uint64_t cur = (rem << 32) | acc[k];
        acc[k] = (uint32_t)(cur / n);
        rem = cur % n;
    }

    // the quotient's highest set bit
    int top = -1;
    for (int k = LIMBS - 1; k >= 0 && top < 0; k--) {
        if (acc[k]) top = k * 32 + 31 - __builtin_clz(acc[k]);
    }
    if (top < 0) {
        // the quotient is 0: the mean is below 2^-1074, rounded by the remainder alone
        double r = 2 * rem > n ? ldexp(1, -1074) : 0;
        return negative ? -r : r;
    }

    // keep the top 53 bits (fewer below 2^53 units: those are exact), round the rest to nearest, ties to even
    int drop = top + 1 > 53 ? top + 1 - 53 : 0;
    uint64_t keep = 0;
    for (int b = top; b >= drop; b--) keep = (keep << 1) | ((acc[b / 32] >> (b % 32)) & 1);
    // what is dropped, compared with half a unit of the last kept bit: the dropped bits, then the remainder
    // (a fraction of the lowest unit, rem / n)
    int cmp = 0;   // <0 below half, 0 exactly half, >0 above half
    if (drop > 0) {
        int half = drop - 1;
        if (!((acc[half / 32] >> (half % 32)) & 1)) {
            cmp = -1;
        } else {
            bool below = rem != 0;
            for (int b = half - 1; b >= 0 && !below; b--) below = (acc[b / 32] >> (b % 32)) & 1;
            cmp = below ? 1 : 0;
        }
    } else {
        cmp = 2 * rem > n ? 1 : 2 * rem == n ? 0 : -1;
    }
    if (cmp > 0 || (cmp == 0 && (keep & 1))) keep++;
    double r = ldexp((double)keep, drop - 1074);
    return negative ? -r : r;
}

// statistics.median: the middle value of the sorted list, or the mean of the two middle ones (in floating
// point, as Python computes it).
static int CompareDoubles(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

// The median of n values (n > 0), on a sorted copy so the caller's order stays.
static double Median(const double *values, size_t n)
{
    double *sorted = malloc(n * sizeof *sorted);
    memcpy(sorted, values, n * sizeof *sorted);
    qsort(sorted, n, sizeof *sorted, CompareDoubles);
    double m = n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
    free(sorted);
    return m;
}

// Python's f"{text:<24}": the text padded with spaces to `width` characters (code points, not bytes: a view
// name may be UTF-8).
static void AppendPadded(Nob_String_Builder *sb, const char *text, size_t width)
{
    size_t chars = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) chars += (*p & 0xC0) != 0x80;
    nob_sb_append_cstr(sb, text);
    for (; chars < width; chars++) nob_da_append(sb, ' ');
}

// One A-<i>.md file name found by the glob, with its pair number.
typedef struct {
    const char *path;
    const char *digits;   // the number as written in the name (B-<digits>.md is its partner)
    long index;
} AFile;

// qsort order of the A-<i>.md files: by pair number i, as the Python sorted them (numerically, not by name).
static int CompareAFiles(const void *a, const void *b)
{
    long x = ((const AFile *)a)->index, y = ((const AFile *)b)->index;
    return x < y ? -1 : x > y ? 1 : 0;
}

// Summarize <out dir>: the paired table, appended to `sb` (the caller prints it, and tees it into summary.txt
// after a run). The reports are the A-<i>.md files (sorted by i) that have a B-<i>.md. Returns false after
// printing "no ... pairs" on stderr when there are none (the Python's sys.exit: exit status 1).
static bool Summarize(const char *out, Nob_String_Builder *sb)
{
    // glob.glob(os.path.join(out, "A-*.md")), each name's number by the regex A-(\d+)\.md$
    glob_t g;
    int found = glob(nob_temp_sprintf("%s/A-*.md", out), 0, NULL, &g);
    regex_t re;
    regcomp(&re, "A-([0-9]+)\\.md$", REG_EXTENDED);
    AFile *files = NULL;
    size_t fileCount = 0;
    if (found == 0) {
        files = calloc(g.gl_pathc, sizeof *files);
        for (size_t i = 0; i < g.gl_pathc; i++) {
            regmatch_t m[2];
            if (regexec(&re, g.gl_pathv[i], 2, m, 0) != 0) ToolDie(1, "%s: no pair number in the name", g.gl_pathv[i]);
            files[fileCount].path = g.gl_pathv[i];
            files[fileCount].digits = strndup(g.gl_pathv[i] + m[1].rm_so, (size_t)(m[1].rm_eo - m[1].rm_so));
            files[fileCount].index = strtol(files[fileCount].digits, NULL, 10);
            fileCount++;
        }
        // sorted(..., key=int): a stable sort by number (merge sort order for equal numbers isn't relied on)
        qsort(files, fileCount, sizeof *files, CompareAFiles);
    }
    regfree(&re);

    // the pairs: every A-<i>.md whose B-<i>.md exists
    Pairs pairs = {0};
    for (size_t i = 0; i < fileCount; i++) {
        const char *b = nob_temp_sprintf("%s/B-%s.md", out, files[i].digits);
        struct stat st;
        if (stat(b, &st) != 0) continue;
        Pair p = {.index = files[i].index};
        ReadReport(files[i].path, &p.a);
        ReadReport(b, &p.b);
        if (!p.a.hasTotal || !p.b.hasTotal) ToolDie(1, "%s: a report of pair %ld has no **All** row", out, p.index);
        nob_da_append(&pairs, p);
    }
    for (size_t i = 0; i < fileCount; i++) free((char *)files[i].digits);
    free(files);
    if (found == 0) globfree(&g);
    if (pairs.count == 0) {
        fprintf(stderr, "bench-export-ab: no A-<n>.md / B-<n>.md pairs in %s\n", out);
        return false;
    }

    // per pair: A, B, their difference in ms and in percent of A
    nob_sb_appendf(sb, "\n%4s %9s %9s %9s %8s\n", "pair", "A ms", "B ms", "B-A ms", "B-A %");
    double *diffs = malloc(pairs.count * sizeof *diffs);
    double *rel = malloc(pairs.count * sizeof *rel);
    for (size_t i = 0; i < pairs.count; i++) {
        double at = pairs.items[i].a.total, bt = pairs.items[i].b.total;
        if (at == 0) ToolDie(1, "pair %ld: A's overall average is 0 ms", pairs.items[i].index);
        double d = bt - at;
        diffs[i] = d;
        rel[i] = d / at * 100.0;
        nob_sb_appendf(sb, "%4ld %9.3f %9.3f %+9.3f %+7.2f%%\n", pairs.items[i].index, at, bt, d, d / at * 100.0);
    }

    // overall: mean and median of the paired differences, their range
    double lo = diffs[0], hi = diffs[0];
    for (size_t i = 1; i < pairs.count; i++) {
        if (diffs[i] < lo) lo = diffs[i];
        if (diffs[i] > hi) hi = diffs[i];
    }
    nob_sb_appendf(sb,
                   "\noverall: mean of paired differences %+.3f ms (%+.2f %%), median %+.3f ms (%+.2f %%), range "
                   "%+.3f .. %+.3f ms, %zu pairs\n",
                   ExactMean(diffs, pairs.count), ExactMean(rel, pairs.count), Median(diffs, pairs.count),
                   Median(rel, pairs.count), lo, hi, pairs.count);

    // per view (in the first pair's A order): A's mean and the mean / median difference over the pairs that
    // have the view on both sides
    nob_sb_append_cstr(sb, "\n");
    AppendPadded(sb, "view", 24);
    nob_sb_appendf(sb, " %12s %14s %8s %16s\n", "A ms (mean)", "B-A ms (mean)", "B-A %", "B-A ms (median)");
    double *a = malloc(pairs.count * sizeof *a);
    double *d = malloc(pairs.count * sizeof *d);
    const Views *first = &pairs.items[0].a.views;
    for (size_t v = 0; v < first->count; v++) {
        const char *name = first->items[v].name;
        size_t n = 0;
        for (size_t i = 0; i < pairs.count; i++) {
            const double *av = ViewFind(&pairs.items[i].a.views, name);
            const double *bv = ViewFind(&pairs.items[i].b.views, name);
            if (!av || !bv) continue;
            a[n] = *av;
            d[n] = *bv - *av;
            n++;
        }
        if (n == 0) continue;
        double am = ExactMean(a, n), dm = ExactMean(d, n);
        AppendPadded(sb, name, 24);
        nob_sb_appendf(sb, " %12.3f %+14.3f %+7.2f%% %+16.3f\n", am, dm, dm / am * 100.0, Median(d, n));
    }

    free(a);
    free(d);
    free(diffs);
    free(rel);
    for (size_t i = 0; i < pairs.count; i++) {
        for (size_t k = 0; k < pairs.items[i].a.views.count; k++) free(pairs.items[i].a.views.items[k].name);
        for (size_t k = 0; k < pairs.items[i].b.views.count; k++) free(pairs.items[i].b.views.items[k].name);
        nob_da_free(pairs.items[i].a.views);
        nob_da_free(pairs.items[i].b.views);
    }
    nob_da_free(pairs);
    return true;
}

// #endregion

// #region Build and run

// The settings of a run (the script's variables).
static const char *gMap = "";   // --map
static long gPairs = 3;         // --pairs
static const char *gFrames = "600";   // --frames, as typed (it goes into the commands as text)
static const char *gNote = "";  // --note
static const char *gOutDir;     // --out, absolute after mkdir
static bool gDry;               // --dry-run

// Package <export dir>: the map package folder in it: <dir>/<--map> when --map names one (its <name>.mapb must
// exist), else the one folder <dir>/<name>/ that holds <name>.mapb. Exits 1 when it isn't there or isn't
// unique. Returns a heap string.
static char *Package(const char *dir)
{
    if (*gMap) {
        const char *mapb = nob_temp_sprintf("%s/%s/%s.mapb", dir, gMap, gMap);
        if (!IsFile(mapb)) ToolDie(1, "%s not found", mapb);
        return strdup(nob_temp_sprintf("%s/%s", dir, gMap));
    }
    // "$dir"/*/*.mapb (glob order), each kept when its name is its folder's
    glob_t g;
    Nob_File_Paths found = {0};
    if (glob(nob_temp_sprintf("%s/*/*.mapb", dir), 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            const char *m = g.gl_pathv[i];
            if (!IsFile(m)) continue;
            const char *slash = strrchr(m, '/');
            const char *folderEnd = slash;
            const char *folderStart = folderEnd;
            while (folderStart > m && folderStart[-1] != '/') folderStart--;
            size_t nameLen = strlen(slash + 1) - strlen(".mapb");
            if ((size_t)(folderEnd - folderStart) == nameLen && strncmp(folderStart, slash + 1, nameLen) == 0) {
                nob_da_append(&found, strndup(m, (size_t)(slash - m)));
            }
        }
        globfree(&g);
    }
    if (found.count != 1) ToolDie(1, "%s: %zu map packages; name one with --map", dir, found.count);
    char *pkg = (char *)found.items[0];
    nob_da_free(found);
    return pkg;
}

// Build <export dir> <out sub dir>: viewer.c with the harness (its own object, so the harness never reaches the
// runtime), the export's runtime and libraries, the export's compiler flags, into <sub>/viewer-bench. A failing
// compile exits with the compiler's status. Prints "built: <binary> (<library folder>)".
static void Build(const char *dir, const char *sub)
{
    // the export's R3D libraries: prebuilt, or built from source by its ./build.sh
    const char *lib = NULL;
    const char *candidates[] = {nob_temp_sprintf("%s/prebuilt/linux/lib", dir),
                                nob_temp_sprintf("%s/build/r3d-linux/lib", dir)};
    for (size_t i = 0; i < NOB_ARRAY_LEN(candidates) && !lib; i++) {
        if (IsFile(nob_temp_sprintf("%s/libr3d.a", candidates[i]))) lib = candidates[i];
    }
    if (!lib) ToolDie(1, "%s has no R3D libraries (prebuilt/linux/lib or build/r3d-linux/lib)", dir);
    if (!IsFile(nob_temp_sprintf("%s/viewer.c", dir)) || !IsFile(nob_temp_sprintf("%s/monolith_scene.c", dir))) {
        ToolDie(1, "%s is not an export", dir);
    }

    // the export's own flags (its build program's), so the binary is what its build makes plus the harness
    const char *cc = ToolEnv("CC", "cc");
    const char *flags[] = {"-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-missing-field-initializers", "-DMS_PATCHED_R3D",
                           nob_temp_sprintf("-I%s", dir), nob_temp_sprintf("-I%s/r3d/include", dir),
                           nob_temp_sprintf("-I%s/r3d/external/raylib/src", dir)};
    const char *viewerO = nob_temp_sprintf("%s/viewer.o", sub);
    const char *sceneO = nob_temp_sprintf("%s/monolith_scene.o", sub);
    Nob_Cmd cmd = {0};
    int status;

    // viewer.c with the harness force-included (bench mode)
    nob_cmd_append(&cmd, cc);
    nob_da_append_many(&cmd, flags, NOB_ARRAY_LEN(flags));
    nob_cmd_append(&cmd, "-include", nob_temp_sprintf("%s/tests/examples/harness.h", gToolRoot), "-c",
                   nob_temp_sprintf("%s/viewer.c", dir), "-o", viewerO);
    if ((status = ToolRun(&cmd, (ToolIo){0})) != 0) exit(status);

    // the runtime as the export ships it
    nob_cmd_append(&cmd, cc);
    nob_da_append_many(&cmd, flags, NOB_ARRAY_LEN(flags));
    nob_cmd_append(&cmd, "-c", nob_temp_sprintf("%s/monolith_scene.c", dir), "-o", sceneO);
    if ((status = ToolRun(&cmd, (ToolIo){0})) != 0) exit(status);

    // the link, against the export's libraries
    nob_cmd_append(&cmd, cc, viewerO, sceneO, "-o", nob_temp_sprintf("%s/viewer-bench", sub),
                   nob_temp_sprintf("-L%s", lib), "-lr3d", "-lraylib", "-lassimp", "-lzlibstatic", "-lstdc++", "-lGL",
                   "-lm", "-lpthread", "-ldl");
    if ((status = ToolRun(&cmd, (ToolIo){0})) != 0) exit(status);
    nob_cmd_free(cmd);
    printf("built: %s/viewer-bench (%s)\n", sub, StripPrefix(lib, nob_temp_sprintf("%s/", dir)));
    fflush(stdout);
}

// Appends text to a file (the script's `>> file`); exits 1 when it can't.
static void AppendToFile(const char *path, const char *text)
{
    FILE *f = fopen(path, "a");
    if (!f || fputs(text, f) == EOF || fclose(f) != 0) ToolDie(1, "%s: %s", path, strerror(errno));
}

// Run <A|B> <pair>: one bench run of side A (the base) or B (the new export) on its package, its report at
// <out>/<side>-<i>.md and its output in <out>/<side>-<i>.log. The command goes into runs.txt; with --dry-run it
// is printed shell-quoted instead of run. Exits 1 when the run fails.
static void Run(char side, long i, const char *pkg)
{
    const char *bin = nob_temp_sprintf("%s/%c/viewer-bench", gOutDir, side);
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "env", "__GL_SYNC_TO_VBLANK=0", "vblank_mode=0", bin, "--bench",
                   nob_temp_sprintf("%s/%c-%ld.md", gOutDir, side, i), "--bench-frames", gFrames, "--note",
                   nob_temp_sprintf("%s (%c = %s, pair %ld of %ld)", gNote, side, side == 'A' ? "base" : "new", i,
                                    gPairs),
                   pkg);

    // "run <side> <i>: <the words joined by spaces>" into runs.txt (as the script's ${cmd[*]})
    Nob_String_Builder line = {0};
    nob_sb_appendf(&line, "run %c %ld:", side, i);
    for (size_t k = 0; k < cmd.count; k++) nob_sb_appendf(&line, " %s", cmd.items[k]);
    nob_sb_append_cstr(&line, "\n");
    nob_sb_append_null(&line);
    AppendToFile(nob_temp_sprintf("%s/runs.txt", gOutDir), line.items);
    nob_sb_free(line);

    // --dry-run: `printf '%q ' "${cmd[@]}"; echo`, every word quoted and followed by a space
    if (gDry) {
        Nob_String_Builder q = {0};
        for (size_t k = 0; k < cmd.count; k++) {
            AppendShellQuoted(&q, cmd.items[k]);
            nob_da_append(&q, ' ');
        }
        nob_da_append(&q, '\n');
        fwrite(q.items, 1, q.count, stdout);
        fflush(stdout);
        nob_sb_free(q);
        nob_cmd_free(cmd);
        return;
    }

    // the run: stdout and stderr into its log
    printf("bench-export-ab: %c, pair %ld of %ld ...\n", side, i, gPairs);
    fflush(stdout);
    const char *log = nob_temp_sprintf("%s/%c-%ld.log", gOutDir, side, i);
    if (ToolRun(&cmd, (ToolIo){.out = log, .errToOut = true}) != 0) {
        ToolDie(1, "run %c %ld failed (see %s)", side, i, log);
    }
    nob_cmd_free(cmd);
}

// #endregion

int main(int argc, char **argv)
{
    // the name for messages, the repo root (the harness is in it), a rebuild when this file changed
    ToolInit(argv, "bench-export-ab", "bench-export-ab");
    // --dry-run's quoting decides what is printable by the locale's character set, as bash does; numbers keep
    // the "C" format (Python's f-strings ignore the locale)
    setlocale(LC_CTYPE, "");

    // --summarize <out dir>: the table again, nothing else
    if (argc > 1 && strcmp(argv[1], "--summarize") == 0) {
        if (argc != 3) Usage();
        Nob_String_Builder sb = {0};
        bool ok = Summarize(argv[2], &sb);
        fwrite(sb.items, 1, sb.count, stdout);
        nob_sb_free(sb);
        return ok ? 0 : 1;
    }

    // <base export> <new export> [options]
    if (argc < 3) Usage();
    char *aDir = AbsoluteDir(argv[1]);
    char *bDir = AbsoluteDir(argv[2]);
    const char *out = "";
    const char *pairs = "3";
    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        bool takesValue = strcmp(a, "--map") == 0 || strcmp(a, "--pairs") == 0 || strcmp(a, "--frames") == 0 ||
                          strcmp(a, "--note") == 0 || strcmp(a, "--out") == 0;
        if (takesValue && i + 1 >= argc) ToolDie(1, "%s: needs a value", a);   // (the script: unbound variable)
        if (strcmp(a, "--map") == 0) gMap = argv[++i];
        else if (strcmp(a, "--pairs") == 0) pairs = argv[++i];
        else if (strcmp(a, "--frames") == 0) gFrames = argv[++i];
        else if (strcmp(a, "--note") == 0) gNote = argv[++i];
        else if (strcmp(a, "--out") == 0) out = argv[++i];
        else if (strcmp(a, "--dry-run") == 0) gDry = true;
        else Usage();
    }
    // the checks, in the script's order: pairs, frames, note
    if (!AllDigits(pairs) || strtol(pairs, NULL, 10) < 3) ToolDie(2, "--pairs must be 3 or more");
    gPairs = strtol(pairs, NULL, 10);
    if (!AllDigits(gFrames)) ToolDie(2, "--frames must be a number");
    if (!*gNote) ToolDie(2, "give a --note (what the two exports differ in)");

    // the output folder: --out, or benchmark/export-ab/<date_time> in the repo; created with A/ and B/ in it
    if (!*out) {
        char stamp[64];
        time_t now = time(NULL);
        strftime(stamp, sizeof stamp, "%Y-%m-%d_%H%M%S", localtime(&now));
        out = nob_temp_sprintf("%s/benchmark/export-ab/%s", gToolRoot, stamp);
    }
    if (!ToolMkdirP(nob_temp_sprintf("%s/A", out)) || !ToolMkdirP(nob_temp_sprintf("%s/B", out))) return 1;
    gOutDir = AbsoluteDir(out);

    // the packages, then both binaries
    char *aPkg = Package(aDir);
    char *bPkg = Package(bDir);
    Build(aDir, nob_temp_sprintf("%s/A", gOutDir));
    Build(bDir, nob_temp_sprintf("%s/B", gOutDir));

    // the plan, on stdout and as the start of runs.txt (the script's `{ ...; } | tee runs.txt`)
    const char *plan = nob_temp_sprintf("base (A): %s (%s)\nnew  (B): %s (%s)\npairs: %ld, frames per viewpoint: %s, "
                                        "note: %s\n",
                                        aDir, aPkg, bDir, bPkg, gPairs, gFrames, gNote);
    fputs(plan, stdout);
    fflush(stdout);
    if (!nob_write_entire_file(nob_temp_sprintf("%s/runs.txt", gOutDir), plan, strlen(plan))) return 1;

    // A, B, A, B, ...: interleaved, so a drift of the GPU's clocks hits both sides alike
    for (long i = 1; i <= gPairs; i++) {
        Run('A', i, aPkg);
        Run('B', i, bPkg);
    }
    if (gDry) return 0;

    // the table, on stdout and into summary.txt (the script's `Summarize | tee summary.txt`), then where the
    // reports are
    Nob_String_Builder sb = {0};
    bool ok = Summarize(gOutDir, &sb);
    fwrite(sb.items, 1, sb.count, stdout);
    nob_write_entire_file(nob_temp_sprintf("%s/summary.txt", gOutDir), sb.items, sb.count);
    nob_sb_free(sb);
    if (!ok) return 1;
    printf("reports: %s\n", gOutDir);
    return 0;
}
