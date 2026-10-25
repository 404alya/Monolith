// mapedit -- manual map edits as minimal text edits to the map file, with undo / redo
//
// The map file stays the only source of truth: every edit changes the numbers on a line,
// deletes a line, splits a `repeat` line or adds lines, and leaves every other byte as it
// was (comments, spacing, groups, line endings). The game then reloads the file (MS_Reload).
//   remove     deletes the object's line; one copy of a `repeat` line: the line is split so
//              the other copies keep their exact positions, the removed copy is left out
//   move       adds the shift to the line's position numbers (box: all six corners, block:
//              cx y0 cz, stairs: cx baseY startZ, waffle: x0 x1 y0 y1 z, model: x y z, light:
//              x y z and its clip box); one copy of a `repeat` line: split, the moved copy
//              gets its own plain line with its world coordinates
//   turn       models: the yaw of `rot` (added when the line has none); boxes: 90 degree yaw
//              turns about the box's own centre (box: the x / z extents swap, block: sx and
//              sz swap); stairs and waffles can't turn
//   duplicate  a new line after each source line: the copy, shifted
//   add        `id N model "models/x.glb" x y z origin bottom` after the line of the object it
//              stands on (so it joins that object's group); wrapped in `use "default"` ...
//              `use "<active>"` when a `use` is active there, which would restyle the model
//   id         gives the objects' line an `id N` prefix (or changes it) and nothing else: a chosen
//              N for one line (unique in the file), or the next free one for each selected line
//              without an id; a `repeat` line is not split (the id names the whole line)
// Splitting a `repeat` line keeps the copies in their map order: the rows before the
// changed copy, its row split around it, the rows after. Each untouched piece is written
// with plain `repeat` prefixes and adjusted start values when that gives exactly the same
// floats for every copy (whole and half numbers), else with `repeat n dx dy dz from k`
// (the same i * d products as before, so always exact).
//
// Entity ids (runtime/monolith_scene.h, "Entities"): a line edited by hand gets an `id N`
// prefix (N = the largest id in the file + 1) unless it has one, so its objects keep their
// identity although the line's text changed; new lines (moved copies, duplicates, added
// models) get a new id. The selection follows the edited objects through their ids.
//
// Undo / redo: every edit is a list of hunks (offset, old bytes, new bytes). Undo writes the
// old bytes back, so an edit followed by undo gives back the file byte for byte. Both work
// only while the file on disk is exactly the one the step produced (resp. started from):
// when the map changes for any other reason (a request finished, another editor saved),
// MapEditSync drops the whole history, so an undo never reverts a change the developer
// didn't make here.
//
// Edit script (--apply-edits <file>; one command per line, '#' starts a comment):
//   select line <n> [copy <c>]     the selection becomes every object of map line n (with
//                                  copy: only that `repeat` copy, all objects it makes)
//   select id <n> [index <i>]      the objects of the line with `id n` (index: only that one)
//   select entity <hex> <index>    the entity with this id (as --dump-entities prints it)
//   select none
//   also ...                       like select, but adds to the selection
//   move <dx> <dy> <dz>            metres
//   turn <degrees>                 yaw (boxes: multiples of 90)
//   remove
//   duplicate <dx> <dy> <dz>       copies shifted by this
//   add "<models/x.glb>" <x> <y> <z>   after the line of the first selected object
//   id [<n>]                       the selection's line gets `id n` (one line), or without n each
//                                  selected line without an id gets the next free one
//   undo, redo
//   print                          the selection (entity id, kind, line, copy)
// Each command prints one "edits: ..." line; the first failing one stops the script (exit 1).
#include "mapedit.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>

#define FNV_OFFSET 14695981039346656037ULL
#define NUMBER_MAX 64       // longest number token the editor rewrites (bytes)

static unsigned long long Fnv1a64(const char *p, int n)
{
    unsigned long long h = FNV_OFFSET;
    for (int i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// ======================================================================================
// #region Strings and files
// ======================================================================================

typedef struct { char *s; int len, cap; bool oom; } StrBuf;

static void SbAdd(StrBuf *b, const char *p, int n)
{
    if (b->oom || n < 0) return;
    if (!MS_RESERVE(b->s, b->cap, b->len + n + 1)) { b->oom = true; return; }
    memcpy(b->s + b->len, p, (size_t)n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void SbStr(StrBuf *b, const char *s) { SbAdd(b, s, (int)strlen(s)); }

// Formatted text of one number or short phrase (bounded scratch)
static void SbFmt(StrBuf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    SbAdd(b, tmp, n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1);
}

static char *ReadFileBytes(const char *path, int *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    int len = 0, cap = 0;
    for (;;) {
        if (!MS_RESERVE(buf, cap, len + 65536 + 1) || len > MS_MAX_FILE_BYTES) { free(buf); fclose(f); return NULL; }
        size_t got = fread(buf + len, 1, 65536, f);
        len += (int)got;
        if (got < 65536) break;
    }
    bool err = ferror(f);
    fclose(f);
    if (err) { free(buf); return NULL; }
    buf[len] = '\0';
    *size = len;
    return buf;
}

// Written in place (not a temp file + rename): the file keeps its inode and permissions,
// and the hot reload settles over partial writes anyway (MS_Poll re-reads for 2 s)
static bool WriteFileBytes(const char *path, const char *data, int size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, (size_t)size, f) == (size_t)size;
    return (fclose(f) == 0) && ok;
}

// A decimal with at most `decimals` places, trailing zeros dropped, never "-0"
static void FormatNum(double v, int decimals, char *out, int outSize)
{
    if (decimals < 0) decimals = 0;
    if (decimals > 9) decimals = 9;
    double scale = pow(10.0, decimals);
    v = round(v * scale) / scale;
    if (v == 0.0) v = 0.0;
    snprintf(out, outSize, "%.*f", decimals, v);
    char *dot = strchr(out, '.');
    if (dot) {
        char *e = out + strlen(out) - 1;
        while (e > dot && *e == '0') *e-- = '\0';
        if (e == dot) *e = '\0';
    }
    if (strcmp(out, "-0") == 0) snprintf(out, outSize, "0");
}

// #endregion

// ======================================================================================
// #region Line analysis: the tokens of one statement and what each number means
// ======================================================================================

typedef struct { int s, e; bool quoted; } LTok;    // [s, e) of the line; quoted: with the quotes

enum { ST_BOX, ST_LIGHT, ST_MODEL };
#define POS_MAX 16          // positional numbers of a statement: waffle 5, light 3 + clip 6

typedef struct {
    const char *text;
    int len;
    LTok *tok;
    int n, cap;
    int comment;            // offset of the '#' that starts the comment, len = none
    int idTok, id;          // `id n`: its token index (-1 = none)
    int reps;
    int repTok[3], repEnd[3];       // first token of each `repeat`, and one past its last
    int repN[3], repFrom[3];
    double repD[3][3];
    float repDf[3][3];
    int atTok;              // -1 = no `at`
    double at[3];
    float atF[3];
    int kw;                 // the primitive keyword
    int kind;               // ST_*
    char prim[16];
    int posTok[POS_MAX], posAxis[POS_MAX], posCount;
    int rotTok;             // model: the `rot` key, -1
    int zTok;               // model: the z of the position
} Stmt;

static bool TokIs(const Stmt *st, int i, const char *word)
{
    if (i < 0 || i >= st->n || st->tok[i].quoted) return false;
    int n = st->tok[i].e - st->tok[i].s;
    return (int)strlen(word) == n && memcmp(st->text + st->tok[i].s, word, (size_t)n) == 0;
}

static bool TokCopy(const Stmt *st, int i, char *out, int outSize)
{
    if (i < 0 || i >= st->n) return false;
    int n = st->tok[i].e - st->tok[i].s;
    if (n >= outSize) return false;
    memcpy(out, st->text + st->tok[i].s, (size_t)n);
    out[n] = '\0';
    return true;
}

static bool TokNum(const Stmt *st, int i, double *d, float *f)
{
    char buf[NUMBER_MAX];
    if (i < 0 || i >= st->n || st->tok[i].quoted || !TokCopy(st, i, buf, sizeof(buf))) return false;
    char *end;
    double v = strtod(buf, &end);
    if (end == buf || *end) return false;
    if (d) *d = v;
    if (f) *f = strtof(buf, NULL);     // the parser's value (ParseFloat)
    return true;
}

static bool TokInt(const Stmt *st, int i, int *out)
{
    char buf[NUMBER_MAX];
    if (!TokCopy(st, i, buf, sizeof(buf))) return false;
    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf || *end || v < 0 || v > INT_MAX) return false;
    *out = (int)v;
    return true;
}

// Decimal places a number token is written with (exponent forms: 9)
static int TokDecimals(const Stmt *st, int i)
{
    const char *p = st->text + st->tok[i].s, *e = st->text + st->tok[i].e;
    for (const char *q = p; q < e; q++) if (*q == 'e' || *q == 'E') return 9;
    const char *dot = memchr(p, '.', (size_t)(e - p));
    return dot ? (int)(e - dot - 1) : 0;
}

// Splits the line like the runtime's tokenizer: words, "..." strings, '#' ends the line
static bool Tokenize(Stmt *st)
{
    st->n = 0;
    st->comment = st->len;
    int p = 0;
    if (st->len >= 3 && memcmp(st->text, "\xEF\xBB\xBF", 3) == 0) p = 3;
    for (;;) {
        while (p < st->len && (st->text[p] == ' ' || st->text[p] == '\t')) p++;
        if (p >= st->len) break;
        if (st->text[p] == '#') { st->comment = p; break; }
        LTok t = { p, p, false };
        if (st->text[p] == '"') {
            const char *close = memchr(st->text + p + 1, '"', (size_t)(st->len - p - 1));
            if (!close) return false;
            t.e = (int)(close - st->text) + 1;
            t.quoted = true;
        } else {
            while (t.e < st->len && st->text[t.e] != ' ' && st->text[t.e] != '\t' && st->text[t.e] != '#' && st->text[t.e] != '"') t.e++;
        }
        LTok *slot = MS_PUSH(st->tok, st->n, st->cap);
        if (!slot) return false;
        *slot = t;
        p = t.e;
    }
    return true;
}

// Reads the statement's prefixes, keyword and positional numbers. False with a reason.
static bool Analyze(Stmt *st, const char *text, int len, char *why, int whySize)
{
    st->text = text;
    st->len = len;
    st->idTok = st->atTok = st->rotTok = st->zTok = -1;
    st->reps = st->posCount = 0;
    memset(st->repN, 0, sizeof(st->repN));
    memset(st->repFrom, 0, sizeof(st->repFrom));
    memset(st->at, 0, sizeof(st->at));
    memset(st->atF, 0, sizeof(st->atF));
    if (!Tokenize(st)) { snprintf(why, whySize, "cannot read the line"); return false; }
    int t = 0;
    for (;;) {
        if (TokIs(st, t, "id")) {
            if (!TokInt(st, t + 1, &st->id)) break;
            st->idTok = t;
            t += 2;
        } else if (TokIs(st, t, "repeat") && st->reps < 3) {
            int r = st->reps;
            st->repTok[r] = t;
            if (!TokInt(st, t + 1, &st->repN[r])) break;
            for (int a = 0; a < 3; a++)
                if (!TokNum(st, t + 2 + a, &st->repD[r][a], &st->repDf[r][a])) { snprintf(why, whySize, "bad repeat"); return false; }
            t += 5;
            st->repFrom[r] = 0;
            if (TokIs(st, t, "from")) {
                if (!TokInt(st, t + 1, &st->repFrom[r])) break;
                t += 2;
            }
            st->repEnd[r] = t;
            st->reps++;
        } else if (TokIs(st, t, "at")) {
            st->atTok = t;
            for (int a = 0; a < 3; a++)
                if (!TokNum(st, t + 1 + a, &st->at[a], &st->atF[a])) { snprintf(why, whySize, "bad at"); return false; }
            t += 4;
        } else break;
    }
    st->kw = t;
    if (t >= st->n || !TokCopy(st, t, st->prim, sizeof(st->prim))) { snprintf(why, whySize, "not an object line"); return false; }
    if (TokIs(st, t, "light")) {
        st->kind = ST_LIGHT;
        int nPos = TokIs(st, t + 1, "dir") ? 0 : 3, nDir = TokIs(st, t + 1, "point") ? 0 : 3;
        for (int a = 0; a < nPos; a++) { st->posTok[st->posCount] = t + 2 + a; st->posAxis[st->posCount++] = a; }
        for (int k = t + 2 + nPos + nDir; k < st->n; k++)
            if (TokIs(st, k, "clip"))
                for (int a = 0; a < 6; a++) { st->posTok[st->posCount] = k + 1 + a; st->posAxis[st->posCount++] = a % 3; }
    } else if (TokIs(st, t, "model")) {
        st->kind = ST_MODEL;
        for (int a = 0; a < 3; a++) { st->posTok[st->posCount] = t + 2 + a; st->posAxis[st->posCount++] = a; }
        st->zTok = t + 4;
        for (int k = t + 5; k < st->n; k++) if (TokIs(st, k, "rot")) st->rotTok = k;
    } else {
        unsigned char axes[8];
        int argc = MS_PrimitiveArgs(st->prim, axes);
        if (argc < 0) { snprintf(why, whySize, "not an object line"); return false; }
        st->kind = ST_BOX;
        for (int a = 0; a < argc; a++)
            if (axes[a] >= MS_AXIS_X && axes[a] <= MS_AXIS_Z) {
                st->posTok[st->posCount] = t + 1 + a;
                st->posAxis[st->posCount++] = axes[a] - MS_AXIS_X;
            }
    }
    for (int k = 0; k < st->posCount; k++)
        if (!TokNum(st, st->posTok[k], NULL, NULL)) { snprintf(why, whySize, "unexpected line layout"); return false; }
    return true;
}

static int CopyCount(const Stmt *st)
{
    int n = 1;
    for (int r = 0; r < st->reps; r++) n *= st->repN[r];
    return n;
}

static int LevelCount(const Stmt *st, int r) { return r < st->reps ? st->repN[r] : 1; }

// Global repeat indices (with `from`) of local copy c
static void CopyIndices(const Stmt *st, int c, int g[3])
{
    for (int r = 2; r >= 0; r--) {
        int cnt = LevelCount(st, r);
        int i = c % cnt;
        c /= cnt;
        g[r] = r < st->reps ? i + st->repFrom[r] : 0;
    }
}

// The decimal world value of positional number k for copy indices g (double: what the
// line means; the floats the parser makes are SimValue's)
static double WorldValue(const Stmt *st, int k, const int g[3])
{
    double v;
    TokNum(st, st->posTok[k], &v, NULL);
    if (st->atTok < 0 && st->reps == 0) return v;
    int a = st->posAxis[k];
    double off = st->at[a];
    for (int r = 0; r < st->reps; r++) off += g[r] * st->repD[r][a];
    return off + v;
}

// Decimal places for a new value of positional number k: at least 6 (micrometres), more
// when the line already uses more
static int ValueDecimals(const Stmt *st, int k)
{
    int d = TokDecimals(st, st->posTok[k]);
    for (int r = 0; r < st->reps; r++) for (int a = 0; a < 3; a++) d = d > TokDecimals(st, st->repTok[r] + 2 + a) ? d : TokDecimals(st, st->repTok[r] + 2 + a);
    if (st->atTok >= 0) for (int a = 0; a < 3; a++) d = d > TokDecimals(st, st->atTok + 1 + a) ? d : TokDecimals(st, st->atTok + 1 + a);
    return d > 6 ? d : 6;
}

// #endregion

// ======================================================================================
// #region Rewriting a line: splices of the original bytes
// ======================================================================================

typedef struct { int s, e; int repOff, repLen; } Splice;   // [s, e) of the line -> pool bytes

typedef struct {
    Splice *sp;
    int n, cap;
    StrBuf pool;
    bool oom;
} Rewrite;

static void RwReset(Rewrite *w) { w->n = 0; w->pool.len = 0; }

static void RwSet(Rewrite *w, int s, int e, const char *rep, int repLen)
{
    Splice *x = MS_PUSH(w->sp, w->n, w->cap);
    if (!x) { w->oom = true; return; }
    *x = (Splice){ s, e, w->pool.len, repLen };
    SbAdd(&w->pool, rep, repLen);
}

static void RwStr(Rewrite *w, int s, int e, const char *rep) { RwSet(w, s, e, rep, (int)strlen(rep)); }

static int CompareSplices(const void *a, const void *b)
{
    const Splice *x = a, *y = b;
    return x->s != y->s ? (x->s < y->s ? -1 : 1) : (x->e < y->e ? -1 : x->e > y->e);
}

// The line with the splices applied (they must not overlap; an insertion has s == e)
static void RwBuild(Rewrite *w, const Stmt *st, StrBuf *out)
{
    qsort(w->sp, (size_t)w->n, sizeof(Splice), CompareSplices);
    int p = 0;
    for (int i = 0; i < w->n; i++) {
        SbAdd(out, st->text + p, w->sp[i].s - p);
        SbAdd(out, w->pool.s + w->sp[i].repOff, w->sp[i].repLen);
        p = w->sp[i].e;
    }
    SbAdd(out, st->text + p, st->len - p);
    if (w->pool.oom) out->oom = true;
}

// Removes tokens [from, to) with the blanks after them (up to the next token)
static void RwDropTokens(Rewrite *w, const Stmt *st, int from, int to)
{
    int e = to < st->n ? st->tok[to].s : st->tok[to - 1].e;
    RwStr(w, st->tok[from].s, e, "");
}

// Removes the comment with the blanks before it
static void RwDropComment(Rewrite *w, const Stmt *st)
{
    if (st->comment >= st->len) return;
    int s = st->comment;
    while (s > 0 && (st->text[s - 1] == ' ' || st->text[s - 1] == '\t')) s--;
    RwStr(w, s, st->len, "");
}

static void RwNumber(Rewrite *w, const Stmt *st, int tok, double v, int decimals)
{
    char buf[NUMBER_MAX];
    FormatNum(v, decimals, buf, sizeof(buf));
    RwStr(w, st->tok[tok].s, st->tok[tok].e, buf);
}

static void RwId(Rewrite *w, const Stmt *st, int id)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "id %d ", id);
    if (st->idTok >= 0) RwDropTokens(w, st, st->idTok, st->idTok + 2);
    int at = st->tok[0].s;
    RwStr(w, at, at, buf);
}

// `repeat n dx dy dz [from k]` for level r with the line's own dx dy dz text
static void RwRepeat(Rewrite *w, const Stmt *st, int r, int count, int from)
{
    StrBuf b = { 0 };
    SbFmt(&b, "repeat %d", count);
    for (int a = 0; a < 3; a++) {
        SbStr(&b, " ");
        const LTok *t = &st->tok[st->repTok[r] + 2 + a];
        SbAdd(&b, st->text + t->s, t->e - t->s);
    }
    if (from > 0) SbFmt(&b, " from %d", from);
    if (b.oom) w->oom = true;
    else RwSet(w, st->tok[st->repTok[r]].s, st->tok[st->repEnd[r] - 1].e, b.s, b.len);
    free(b.s);
}

// #endregion

// ======================================================================================
// #region Pieces of a split `repeat` line
// ======================================================================================

typedef struct { int count[3], from[3]; } Piece;    // per repeat level: copies and first global index

// The parser's float of positional number k for copy g of the original line
static float SimOriginal(const Stmt *st, int k, const int g[3])
{
    float v;
    TokNum(st, st->posTok[k], NULL, &v);
    if (st->atTok < 0 && st->reps == 0) return v;
    int a = st->posAxis[k];
    float off = st->atF[a];
    for (int r = 0; r < st->reps; r++) off += (float)g[r] * st->repDf[r][a];
    return off + v;
}

static bool LevelChanged(const Stmt *st, const Piece *pc, int r)
{
    return pc->count[r] != st->repN[r] || pc->from[r] != st->repFrom[r];
}

// A piece with the levels in fold[] folded: a folded level loses its `from` (its from * d
// goes into the position numbers) and disappears when it has one copy left; the other
// changed levels are written as `repeat n dx dy dz from k`. Exact when every copy gets the
// same floats as before. vals[k] / decs[k]: the new decimal of positional number k.
static bool FoldExact(const Stmt *st, const Piece *pc, const bool *fold, double *vals, int *decs)
{
    bool kept[3] = { false, false, false };
    int keptReps = 0;
    for (int r = 0; r < st->reps; r++) {
        kept[r] = !fold[r] || pc->count[r] > 1;
        keptReps += kept[r];
    }
    float vf[POS_MAX];
    for (int k = 0; k < st->posCount; k++) {
        double v;
        TokNum(st, st->posTok[k], &v, NULL);
        int a = st->posAxis[k];
        for (int r = 0; r < st->reps; r++)
            if (fold[r]) v += pc->from[r] * st->repD[r][a];
        decs[k] = ValueDecimals(st, k);
        vals[k] = v;
        char buf[NUMBER_MAX];
        FormatNum(v, decs[k], buf, sizeof(buf));
        vf[k] = strtof(buf, NULL);
    }
    bool shifted = st->atTok >= 0 || keptReps > 0;
    int total = pc->count[0] * pc->count[1] * pc->count[2];
    for (int c = 0; c < total; c++) {
        int j[3] = { c / (pc->count[1] * pc->count[2]), (c / pc->count[2]) % pc->count[1], c % pc->count[2] };
        int g[3];
        for (int r = 0; r < 3; r++) g[r] = pc->from[r] + j[r];
        for (int k = 0; k < st->posCount; k++) {
            int a = st->posAxis[k];
            float p = vf[k];
            if (shifted) {
                float off = st->atF[a];
                for (int r = 0; r < st->reps; r++)
                    if (kept[r]) off += (float)(fold[r] ? j[r] : g[r]) * st->repDf[r][a];
                p = off + vf[k];
            }
            float o = SimOriginal(st, k, g);
            if (memcmp(&p, &o, sizeof(float)) != 0) return false;
        }
    }
    return true;
}

// The text of an untouched piece. keepHead: it keeps the line's id and comment (the first
// piece of the line), else both are dropped. Each changed level is folded (plain numbers)
// where that stays exact, outer levels first; the rest keep `from`.
static void PieceText(Rewrite *w, const Stmt *st, const Piece *pc, bool keepHead, StrBuf *out)
{
    RwReset(w);
    if (!keepHead) {
        if (st->idTok >= 0) RwDropTokens(w, st, st->idTok, st->idTok + 2);
        RwDropComment(w, st);
    }
    double vals[POS_MAX];
    int decs[POS_MAX];
    bool fold[3] = { false, false, false };
    for (int r = 0; r < st->reps; r++) {
        if (!LevelChanged(st, pc, r)) continue;
        fold[r] = true;
        if (!FoldExact(st, pc, fold, vals, decs)) fold[r] = false;
    }
    FoldExact(st, pc, fold, vals, decs);     // (vals of the final choice)
    bool anyShift = false;
    for (int r = 0; r < st->reps; r++) {
        if (!LevelChanged(st, pc, r)) continue;
        if (fold[r] && pc->count[r] == 1) RwDropTokens(w, st, st->repTok[r], st->repEnd[r]);
        else RwRepeat(w, st, r, pc->count[r], fold[r] ? 0 : pc->from[r]);
        anyShift = anyShift || (fold[r] && pc->from[r] != 0);
    }
    if (anyShift)
        for (int k = 0; k < st->posCount; k++) RwNumber(w, st, st->posTok[k], vals[k], decs[k]);
    RwBuild(w, st, out);
}

// #endregion

// ======================================================================================
// #region Transforms: one copy as its own line, or the whole line
// ======================================================================================

typedef struct {
    const EditOp *op;
    int nextId;             // the next free `id`
    Rewrite rw;
    char why[200];          // first refusal
} Ctx;

static double NormalizeYaw(double y)
{
    y = fmod(y, 360.0);
    if (y > 180.0) y -= 360.0;
    if (y <= -180.0) y += 360.0;
    return y;
}

// Turns of 90 degrees in op (0..3)
static int QuarterTurns(double deg) { return (((int)lround(deg / 90.0)) % 4 + 4) % 4; }

// The turn of the values vals[] (positional numbers in order) of a box / block / model line:
// the splices that do it. vals: the line's own numbers (whole line) or world numbers.
static bool RwTurn(Ctx *C, const Stmt *st, const double *vals, const int *decs)
{
    Rewrite *w = &C->rw;
    double deg = C->op->degrees;
    if (st->kind == ST_MODEL) {
        char buf[NUMBER_MAX + 16];
        if (st->rotTok >= 0) {
            double yaw;
            if (!TokNum(st, st->rotTok + 1, &yaw, NULL)) return false;
            FormatNum(NormalizeYaw(yaw + deg), 6, buf, sizeof(buf));
            RwStr(w, st->tok[st->rotTok + 1].s, st->tok[st->rotTok + 1].e, buf);
        } else {
            char num[NUMBER_MAX];
            FormatNum(NormalizeYaw(deg), 6, num, sizeof(num));
            snprintf(buf, sizeof(buf), " rot %s 0 0", num);
            int at = st->tok[st->zTok].e;
            RwStr(w, at, at, buf);
        }
        return true;
    }
    if (st->kind != ST_BOX) return false;
    if (QuarterTurns(deg) % 2 == 0) return true;    // 180: the same box
    if (strcmp(st->prim, "box") == 0) {
        double cx = (vals[0] + vals[3]) * 0.5, cz = (vals[2] + vals[5]) * 0.5;
        double hx = fabs(vals[3] - vals[0]) * 0.5, hz = fabs(vals[5] - vals[2]) * 0.5;
        double nv[6] = { cx - hz, vals[1], cz - hx, cx + hz, vals[4], cz + hx };
        for (int k = 0; k < 6; k++) RwNumber(w, st, st->posTok[k], nv[k], decs[k]);
        return true;
    }
    if (strcmp(st->prim, "block") == 0) {     // cx y0 cz sx sy sz: swap sx and sz
        int sx = st->kw + 4, sz = st->kw + 6;
        RwSet(w, st->tok[sx].s, st->tok[sx].e, st->text + st->tok[sz].s, st->tok[sz].e - st->tok[sz].s);
        RwSet(w, st->tok[sz].s, st->tok[sz].e, st->text + st->tok[sx].s, st->tok[sx].e - st->tok[sx].s);
        return true;
    }
    return false;
}

// Copy c of the line as a plain line of its own (world numbers, no repeat / at, new id),
// moved / turned / shifted by the op (EDIT_REMOVE never gets here)
static bool CopyLine(Ctx *C, const Stmt *st, int c, StrBuf *out)
{
    Rewrite *w = &C->rw;
    RwReset(w);
    int g[3];
    CopyIndices(st, c, g);
    for (int r = 0; r < st->reps; r++) RwDropTokens(w, st, st->repTok[r], st->repEnd[r]);
    if (st->atTok >= 0) RwDropTokens(w, st, st->atTok, st->atTok + 4);
    RwDropComment(w, st);
    double vals[POS_MAX];
    int decs[POS_MAX];
    for (int k = 0; k < st->posCount; k++) {
        vals[k] = WorldValue(st, k, g);
        decs[k] = ValueDecimals(st, k);
        if (C->op->kind == EDIT_MOVE || C->op->kind == EDIT_DUPLICATE) vals[k] += C->op->delta[st->posAxis[k]];
    }
    if (C->op->kind == EDIT_ROTATE) {
        if (!RwTurn(C, st, vals, decs)) return false;
        // (a box turn rewrote the corners; the other numbers still need their world values)
        if (!(st->kind == ST_BOX && strcmp(st->prim, "box") == 0 && QuarterTurns(C->op->degrees) % 2))
            for (int k = 0; k < st->posCount; k++) RwNumber(w, st, st->posTok[k], vals[k], decs[k]);
    } else
        for (int k = 0; k < st->posCount; k++) RwNumber(w, st, st->posTok[k], vals[k], decs[k]);
    RwId(w, st, C->nextId++);
    RwBuild(w, st, out);
    return !w->oom;
}

// The whole line moved / turned (keeps its prefixes; gets an id if it has none), or for
// EDIT_DUPLICATE a shifted copy of it with a new id
static bool WholeLine(Ctx *C, const Stmt *st, int *idOut, StrBuf *out)
{
    Rewrite *w = &C->rw;
    RwReset(w);
    double vals[POS_MAX];
    int decs[POS_MAX];
    for (int k = 0; k < st->posCount; k++) {
        TokNum(st, st->posTok[k], &vals[k], NULL);
        decs[k] = ValueDecimals(st, k);
    }
    if (C->op->kind == EDIT_ROTATE) {
        if (!RwTurn(C, st, vals, decs)) return false;
    } else {
        for (int k = 0; k < st->posCount; k++)
            RwNumber(w, st, st->posTok[k], vals[k] + C->op->delta[st->posAxis[k]], decs[k]);
    }
    if (C->op->kind == EDIT_DUPLICATE) {
        RwDropComment(w, st);
        RwId(w, st, *idOut = C->nextId++);
    } else if (st->idTok < 0) RwId(w, st, *idOut = C->nextId++);
    else *idOut = st->id;
    RwBuild(w, st, out);
    return !w->oom;
}

// #endregion

// ======================================================================================
// #region Building a patch from a selection
// ======================================================================================

typedef struct { int start, contentEnd, end; } LineSpan;    // [start, end) incl. the line break

typedef struct {
    int line;
    StrBuf text;            // the new lines, '\n'-separated (the original break is used on output)
    bool deleted;           // no lines at all
} LineOut;

typedef struct {
    const char *text;
    int size;
    LineSpan *lines;
    int lineCount, lineCap;
    LineOut *outs;
    int outCount, outCap;
    MS_EntityId *sel;       // the selection after the edit
    int selCount, selCap;
    char what[200];
} Patch;

static bool IndexLines(Patch *P)
{
    int p = 0;
    while (p < P->size) {
        LineSpan *l = MS_PUSH(P->lines, P->lineCount, P->lineCap);
        if (!l) return false;
        l->start = p;
        while (p < P->size && P->text[p] != '\n') p++;
        l->contentEnd = p > l->start && P->text[p - 1] == '\r' ? p - 1 : p;
        if (p < P->size) p++;
        l->end = p;
    }
    return true;
}

static void SelAdd(Patch *P, unsigned int stmt, int index)
{
    MS_EntityId *e = MS_PUSH(P->sel, P->selCount, P->selCap);
    if (e) *e = (MS_EntityId){ stmt, index };
}

// The largest `id n` of the file (+ 1 = the next free one)
static int MaxId(const Patch *P)
{
    int best = 0;
    Stmt st = { 0 };
    for (int l = 0; l < P->lineCount; l++) {
        const char *s = P->text + P->lines[l].start;
        int len = P->lines[l].contentEnd - P->lines[l].start, i = 0;
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
        if (len - i < 3 || memcmp(s + i, "id", 2) != 0 || (s[i + 2] != ' ' && s[i + 2] != '\t')) continue;
        st.text = s;
        st.len = len;
        int id;
        if (Tokenize(&st) && st.n >= 2 && TokIs(&st, 0, "id") && TokInt(&st, 1, &id) && id > best) best = id;
    }
    free(st.tok);
    return best;
}

static LineOut *OutFor(Patch *P, int line)
{
    LineOut *o = MS_PUSH(P->outs, P->outCount, P->outCap);
    if (o) o->line = line;
    return o;
}

// The line (1-based) whose `id` is `id`, 0 for none
static int IdLine(const Patch *P, int id)
{
    int found = 0;
    Stmt st = { 0 };
    for (int l = 0; l < P->lineCount && !found; l++) {
        const char *s = P->text + P->lines[l].start;
        int len = P->lines[l].contentEnd - P->lines[l].start, i = 0, v;
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
        if (len - i < 3 || memcmp(s + i, "id", 2) != 0 || (s[i + 2] != ' ' && s[i + 2] != '\t')) continue;
        st.text = s;
        st.len = len;
        if (Tokenize(&st) && st.n >= 2 && TokIs(&st, 0, "id") && TokInt(&st, 1, &v) && v == id) found = l + 1;
    }
    free(st.tok);
    return found;
}

// Lines and entities of one statement among the selected objects
typedef struct {
    int prim, line;
    int firstEntity, entityCount;   // the statement's entities (contiguous)
    bool *copySel;                  // per copy: selected (heap)
    int copies, selCopies;
} StmtSel;

static int CompareStmtSel(const void *a, const void *b)
{
    const StmtSel *x = a, *y = b;
    return (x->line > y->line) - (x->line < y->line);
}

// Objects of copy c of a statement (its parts) = entity indices with that copy
static int PartsOf(const StmtSel *S, int c)
{
    int n = 0;
    for (int e = S->firstEntity; e < S->firstEntity + S->entityCount; e++) n += scene->entitySources[e].copy == c;
    return n;
}

// One statement: its new lines into o->text
static bool EditStatement(Ctx *C, Patch *P, const StmtSel *S, const Stmt *st, LineOut *o)
{
    EditOpKind k = C->op->kind;
    StrBuf *out = &o->text;
    bool all = S->selCopies == S->copies;
    if (k == EDIT_DUPLICATE) {
        SbAdd(out, st->text, st->len);
        if (all) {
            int id;
            SbStr(out, "\n");
            if (!WholeLine(C, st, &id, out)) return false;
            for (int i = 0; i < S->entityCount; i++) SelAdd(P, (unsigned int)id, i);
        } else
            for (int c = 0; c < S->copies; c++) {
                if (!S->copySel[c]) continue;
                int id = C->nextId;
                SbStr(out, "\n");
                if (!CopyLine(C, st, c, out)) return false;
                for (int i = 0, n = PartsOf(S, c); i < n; i++) SelAdd(P, (unsigned int)id, i);
            }
        return !out->oom;
    }
    if (all) {
        if (k == EDIT_REMOVE) { o->deleted = true; return true; }
        int id;
        if (!WholeLine(C, st, &id, out)) return false;
        for (int i = 0; i < S->entityCount; i++) SelAdd(P, (unsigned int)id, i);
        return !out->oom;
    }
    // Split: walk the copy grid level by level; runs of untouched rows / copies become pieces
    int cnt[3] = { LevelCount(st, 0), LevelCount(st, 1), LevelCount(st, 2) };
    bool head = true, first = true;
    // An explicit stack instead of recursion: level r with the fixed indices of the outer ones
    for (int i0 = 0; i0 < cnt[0]; ) {
        // rows of level 0 without a selected copy: one piece
        int run = 0;
        while (i0 + run < cnt[0]) {
            bool any = false;
            for (int c = (i0 + run) * cnt[1] * cnt[2]; c < (i0 + run + 1) * cnt[1] * cnt[2] && !any; c++) any = S->copySel[c];
            if (any) break;
            run++;
        }
        if (run > 0) {
            Piece pc = { { run, cnt[1], cnt[2] }, { st->repFrom[0] + i0, st->reps > 1 ? st->repFrom[1] : 0, st->reps > 2 ? st->repFrom[2] : 0 } };
            if (!first) SbStr(out, "\n");
            PieceText(&C->rw, st, &pc, head, out);
            head = first = false;
            i0 += run;
            continue;
        }
        for (int i1 = 0; i1 < cnt[1]; ) {
            int run1 = 0;
            while (i1 + run1 < cnt[1]) {
                bool any = false;
                for (int i2 = 0; i2 < cnt[2] && !any; i2++) any = S->copySel[(i0 * cnt[1] + i1 + run1) * cnt[2] + i2];
                if (any) break;
                run1++;
            }
            if (run1 > 0) {
                Piece pc = { { 1, run1, cnt[2] }, { st->repFrom[0] + i0, st->repFrom[1] + i1, st->reps > 2 ? st->repFrom[2] : 0 } };
                if (!first) SbStr(out, "\n");
                PieceText(&C->rw, st, &pc, head, out);
                head = first = false;
                i1 += run1;
                continue;
            }
            for (int i2 = 0; i2 < cnt[2]; ) {
                int c = (i0 * cnt[1] + i1) * cnt[2] + i2;
                if (S->copySel[c]) {
                    if (k != EDIT_REMOVE) {
                        int id = C->nextId;
                        if (!first) SbStr(out, "\n");
                        if (!CopyLine(C, st, c, out)) return false;
                        first = false;
                        for (int i = 0, n = PartsOf(S, c); i < n; i++) SelAdd(P, (unsigned int)id, i);
                    }
                    i2++;
                    continue;
                }
                int run2 = 0;
                while (i2 + run2 < cnt[2] && !S->copySel[c + run2]) run2++;
                Piece pc = { { 1, 1, run2 }, { st->repFrom[0] + i0, st->repFrom[1] + i1, st->repFrom[2] + i2 } };
                if (!first) SbStr(out, "\n");
                PieceText(&C->rw, st, &pc, head, out);
                head = first = false;
                i2 += run2;
            }
            i1++;
        }
        i0++;
    }
    if (first) o->deleted = true;   // (every copy removed: can't happen, `all` covers it)
    return !out->oom && !C->rw.oom;
}

// The `use` material active after line `line` (1-based), "" = none / "default"
static void ActiveUse(const Patch *P, int line, char *out, int outSize)
{
    out[0] = '\0';
    Stmt st = { 0 };
    for (int l = 0; l < line && l < P->lineCount; l++) {
        const char *s = P->text + P->lines[l].start;
        int len = P->lines[l].contentEnd - P->lines[l].start, i = 0;
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
        if (len - i < 4 || memcmp(s + i, "use", 3) != 0) continue;
        st.text = s;
        st.len = len;
        if (Tokenize(&st) && st.n == 2 && TokIs(&st, 0, "use") && st.tok[1].quoted) {
            int n = st.tok[1].e - st.tok[1].s - 2;
            if (n < outSize) { memcpy(out, s + st.tok[1].s + 1, (size_t)n); out[n] = '\0'; }
        }
    }
    free(st.tok);
    if (strcmp(out, "default") == 0) out[0] = '\0';
}

static void FreePatch(Patch *P)
{
    free(P->lines);
    for (int i = 0; i < P->outCount; i++) free(P->outs[i].text.s);
    free(P->outs);
    free(P->sel);
}

// EDIT_ID on the selected statements (sorted by line): only their `id` prefixes change
static bool IdLines(Ctx *C, Patch *P, const StmtSel *ss, int ssCount, Stmt *st)
{
    int want = C->op->id;
    if (want < 0) { snprintf(C->why, sizeof(C->why), "an id is a number from 1 to 2147483647"); return false; }
    if (want > 0 && ssCount > 1) {
        snprintf(C->why, sizeof(C->why), "one id names one line: select objects of a single line (the selection has %d)", ssCount);
        return false;
    }
    int other = want > 0 ? IdLine(P, want) : 0;
    if (other && other != ss[0].line) { snprintf(C->why, sizeof(C->why), "id %d is used on line %d already", want, other); return false; }
    int changed = 0, objects = 0, firstId = 0, lastId = 0, oldId = 0, lastCount = 0, lastLine = 0;
    for (int k = 0; k < ssCount; k++) {
        const StmtSel *S = &ss[k];
        if (S->line < 1 || S->line > P->lineCount) { snprintf(C->why, sizeof(C->why), "line %d is not in the file", S->line); return false; }
        const LineSpan *ls = &P->lines[S->line - 1];
        if (!Analyze(st, P->text + ls->start, ls->contentEnd - ls->start, C->why, sizeof(C->why))) return false;
        if (CopyCount(st) != S->copies) { snprintf(C->why, sizeof(C->why), "line %d: the copies don't match the loaded map", S->line); return false; }
        int has = st->idTok >= 0 ? st->id : 0;
        int id = want > 0 ? want : has ? has : C->nextId++;
        // The selection keeps the same objects (their line's new key, the same index)
        for (int i = 0; i < S->entityCount; i++)
            if (S->copySel[scene->entitySources[S->firstEntity + i].copy]) SelAdd(P, (unsigned int)id, i);
        if (id == has) continue;
        LineOut *o = OutFor(P, S->line);
        if (!o) { snprintf(C->why, sizeof(C->why), "out of memory"); return false; }
        RwReset(&C->rw);
        RwId(&C->rw, st, id);
        RwBuild(&C->rw, st, &o->text);
        if (C->rw.oom || o->text.oom) { snprintf(C->why, sizeof(C->why), "out of memory"); return false; }
        if (!changed) firstId = id;
        lastId = id;
        oldId = has;
        lastCount = S->entityCount;
        lastLine = S->line;
        objects += S->entityCount;
        changed++;
    }
    if (!changed) {
        if (ssCount == 1) snprintf(C->why, sizeof(C->why), "line %d has id %d already", ss[0].line, want > 0 ? want : st->id);
        else snprintf(C->why, sizeof(C->why), "every selected line has an id already");
        return false;
    }
    const char *kept = changed < ssCount ? TextFormat("; %d other line%s had an id already", ssCount - changed,
                                                      ssCount - changed == 1 ? "" : "s") : "";
    if (changed == 1)
        snprintf(P->what, sizeof(P->what), "id %d on line %d (%d object%s: %d:0%s)%s%s", lastId, lastLine, lastCount,
                 lastCount == 1 ? "" : "s", lastId, lastCount > 1 ? TextFormat(" .. %d:%d", lastId, lastCount - 1) : "",
                 oldId ? TextFormat(", was id %d", oldId) : "", kept);
    else snprintf(P->what, sizeof(P->what), "ids %d..%d on %d lines (%d objects)%s", firstId, lastId, changed, objects, kept);
    return true;
}

// Fills P->outs (one per changed line) and P->sel for op on sel[]. False with C->why.
static bool BuildPatch(Ctx *C, Patch *P, const MS_ObjRef *sel, int selCount)
{
    const EditOp *op = C->op;
    if (!IndexLines(P)) { snprintf(C->why, sizeof(C->why), "out of memory"); return false; }
    C->nextId = MaxId(P) + 1;
    Stmt st = { 0 };
    bool ok = false;

    if (op->kind == EDIT_ADD) {
        const MS_Ref *r = MS_ObjectRef(scene, op->anchor);
        if (!r || r->line < 1 || r->line > P->lineCount) { snprintf(C->why, sizeof(C->why), "nothing to place it on"); goto done; }
        if (strchr(op->modelPath, '"')) { snprintf(C->why, sizeof(C->why), "bad model path"); goto done; }
        const LineSpan *ls = &P->lines[r->line - 1];
        LineOut *o = OutFor(P, r->line);
        if (!o) goto oom;
        char use[256], x[NUMBER_MAX], y[NUMBER_MAX], z[NUMBER_MAX];
        ActiveUse(P, r->line, use, sizeof(use));
        FormatNum(op->pos[0], 4, x, sizeof(x));
        FormatNum(op->pos[1], 4, y, sizeof(y));
        FormatNum(op->pos[2], 4, z, sizeof(z));
        int id = C->nextId++;
        SbAdd(&o->text, P->text + ls->start, ls->contentEnd - ls->start);
        if (use[0]) SbStr(&o->text, "\nuse \"default\"");
        SbFmt(&o->text, "\nid %d model \"%s\" %s %s %s origin bottom", id, op->modelPath, x, y, z);
        if (use[0]) SbFmt(&o->text, "\nuse \"%s\"", use);
        if (o->text.oom) goto oom;
        SelAdd(P, (unsigned int)id, 0);
        snprintf(P->what, sizeof(P->what), "added %s after line %d%s", op->modelPath, r->line, use[0] ? " (in use \"default\")" : "");
        ok = true;
        goto done;
    }

    // Group the selection by statement
    StmtSel *ss = NULL;
    int ssCount = 0, ssCap = 0;
    int boxes = 0, models = 0, lights = 0;
    for (int i = 0; i < selCount; i++) {
        int e = MS_ObjectEntity(scene, sel[i]);
        if (e < 0) continue;
        const MS_Source *src = &scene->entitySources[e];
        boxes += sel[i].kind == MS_OBJ_BOX;
        models += sel[i].kind == MS_OBJ_MODEL;
        lights += sel[i].kind == MS_OBJ_LIGHT;
        StmtSel *S = NULL;
        for (int k = 0; k < ssCount && !S; k++) if (ss[k].prim == src->prim) S = &ss[k];
        if (!S) {
            if (!(S = MS_PUSH(ss, ssCount, ssCap))) goto oomSel;
            S->prim = src->prim;
            S->line = src->line;
            int a = e, b = e;
            while (a > 0 && scene->entitySources[a - 1].prim == src->prim) a--;
            while (b + 1 < scene->entityCount && scene->entitySources[b + 1].prim == src->prim) b++;
            S->firstEntity = a;
            S->entityCount = b - a + 1;
            S->copies = scene->entitySources[b].copy + 1;
            if (!(S->copySel = calloc((size_t)S->copies, sizeof(bool)))) goto oomSel;
        }
        if (!S->copySel[src->copy]) { S->copySel[src->copy] = true; S->selCopies++; }
    }
    if (ssCount == 0) { snprintf(C->why, sizeof(C->why), "nothing selected"); goto doneSel; }
    if (op->kind == EDIT_ROTATE && lights > 0) { snprintf(C->why, sizeof(C->why), "lights can't be turned (edit a spot light's direction on its line)"); goto doneSel; }
    if (op->kind == EDIT_ROTATE && boxes > 0 && models > 0) { snprintf(C->why, sizeof(C->why), "turn boxes and models separately (boxes turn in 90 degree steps)"); goto doneSel; }
    if (op->kind == EDIT_ROTATE && boxes > 0 && fmod(fabs(op->degrees), 90.0) != 0.0) { snprintf(C->why, sizeof(C->why), "boxes turn only in 90 degree steps"); goto doneSel; }
    if (op->kind == EDIT_ROTATE && boxes > 0 && QuarterTurns(op->degrees) % 2 == 0) { snprintf(C->why, sizeof(C->why), "a box turned by 180 degrees is the same box"); goto doneSel; }
    qsort(ss, (size_t)ssCount, sizeof(StmtSel), CompareStmtSel);
    if (op->kind == EDIT_ID) {
        ok = IdLines(C, P, ss, ssCount, &st);
        goto doneSel;
    }

    int splits = 0, extraLines = 0;
    for (int k = 0; k < ssCount; k++) {
        StmtSel *S = &ss[k];
        if (S->line < 1 || S->line > P->lineCount) { snprintf(C->why, sizeof(C->why), "line %d is not in the file", S->line); goto doneSel; }
        const LineSpan *ls = &P->lines[S->line - 1];
        if (!Analyze(&st, P->text + ls->start, ls->contentEnd - ls->start, C->why, sizeof(C->why))) goto doneSel;
        if (CopyCount(&st) != S->copies) { snprintf(C->why, sizeof(C->why), "line %d: the copies don't match the loaded map", S->line); goto doneSel; }
        if (op->kind == EDIT_ROTATE && st.kind == ST_BOX && strcmp(st.prim, "box") != 0 && strcmp(st.prim, "block") != 0) {
            snprintf(C->why, sizeof(C->why), "line %d: %s can't turn (only box and block lines)", S->line, st.prim);
            goto doneSel;
        }
        if ((op->kind == EDIT_MOVE || op->kind == EDIT_DUPLICATE) && st.posCount == 0) {
            snprintf(C->why, sizeof(C->why), "line %d: a directional light has no position", S->line);
            goto doneSel;
        }
        LineOut *o = OutFor(P, S->line);
        if (!o) goto oomSel;
        if (!EditStatement(C, P, S, &st, o)) {
            if (!C->why[0]) snprintf(C->why, sizeof(C->why), "line %d: cannot rewrite it", S->line);
            goto doneSel;
        }
        int lines = o->deleted ? 0 : 1;
        for (int i = 0; i < o->text.len; i++) lines += o->text.s[i] == '\n';
        if (S->selCopies < S->copies && op->kind != EDIT_DUPLICATE) splits++;
        extraLines += lines - 1;
    }
    {
        int objs = 0;
        for (int k = 0; k < ssCount; k++) objs += ss[k].selCopies;
        const char *verb = op->kind == EDIT_MOVE ? "moved" : op->kind == EDIT_ROTATE ? "turned" : op->kind == EDIT_REMOVE ? "removed" : "duplicated";
        char amount[3 * NUMBER_MAX + 16] = "";
        if (op->kind == EDIT_MOVE || op->kind == EDIT_DUPLICATE) {
            char a[3][NUMBER_MAX];
            for (int i = 0; i < 3; i++) FormatNum(op->delta[i], 4, a[i], sizeof(a[i]));
            snprintf(amount, sizeof(amount), " by %s %s %s", a[0], a[1], a[2]);
        } else if (op->kind == EDIT_ROTATE) {
            char a[NUMBER_MAX];
            FormatNum(op->degrees, 4, a, sizeof(a));
            snprintf(amount, sizeof(amount), " by %s deg", a);
        }
        char where[64];
        if (ssCount == 1) snprintf(where, sizeof(where), "line %d", ss[0].line);
        else snprintf(where, sizeof(where), "%d lines from line %d", ssCount, ss[0].line);
        snprintf(P->what, sizeof(P->what), "%s %d object%s%s (%s%s%s)", verb, objs, objs == 1 ? "" : "s", amount, where,
                 splits ? ", split" : "", extraLines > 0 ? TextFormat(", +%d line%s", extraLines, extraLines == 1 ? "" : "s")
                 : extraLines < 0 ? TextFormat(", %d line%s", extraLines, extraLines == -1 ? "" : "s") : "");
    }
    ok = true;
    goto doneSel;
oomSel:
    snprintf(C->why, sizeof(C->why), "out of memory");
doneSel:
    for (int k = 0; k < ssCount; k++) free(ss[k].copySel);
    free(ss);
    goto done;
oom:
    snprintf(C->why, sizeof(C->why), "out of memory");
done:
    free(st.tok);
    return ok;
}

// The hunks of P (sorted by line) against P->text
static bool MakeHunks(const Patch *P, EditStep *step)
{
    // the line break of the file (for lines added after its last line)
    const char *brk = "\n";
    for (int l = 0; l < P->lineCount; l++)
        if (P->lines[l].end > P->lines[l].contentEnd) { brk = P->lines[l].end - P->lines[l].contentEnd == 2 ? "\r\n" : "\n"; break; }
    for (int i = 0; i < P->outCount; i++) {
        const LineOut *o = &P->outs[i];
        const LineSpan *ls = &P->lines[o->line - 1];
        const char *eol = ls->end > ls->contentEnd ? P->text + ls->contentEnd : brk;
        int eolLen = ls->end > ls->contentEnd ? ls->end - ls->contentEnd : (int)strlen(brk);
        bool last = ls->end == ls->contentEnd;      // no line break after it (end of file)
        StrBuf nb = { 0 };
        if (!o->deleted) {
            int p = 0;
            for (int j = 0; j <= o->text.len; j++) {
                if (j < o->text.len && o->text.s[j] != '\n') continue;
                SbAdd(&nb, o->text.s + p, j - p);
                if (j < o->text.len || !last) SbAdd(&nb, eol, eolLen);
                p = j + 1;
            }
        }
        EditHunk *h = MS_PUSH(step->hunks, step->hunkCount, step->hunkCap);
        char *old = MS_StrNDup(P->text + ls->start, (size_t)(ls->end - ls->start));
        if (!h || !old || nb.oom) { free(old); free(nb.s); return false; }
        *h = (EditHunk){ ls->start, old, nb.s ? nb.s : MS_StrDup(""), ls->end - ls->start, nb.len };
        if (!h->newText) return false;
    }
    return true;
}

// text with the step's hunks applied (inverse: undone; the hunks are in ascending order)
static char *ApplyHunks(const char *text, int size, const EditStep *step, bool inverse, int *outSize)
{
    StrBuf b = { 0 };
    int p = 0, shift = 0;
    for (int i = 0; i < step->hunkCount; i++) {
        const EditHunk *h = &step->hunks[i];
        int at = inverse ? h->offset + shift : h->offset;
        int fromLen = inverse ? h->newLen : h->oldLen;
        const char *from = inverse ? h->newText : h->oldText, *to = inverse ? h->oldText : h->newText;
        int toLen = inverse ? h->oldLen : h->newLen;
        if (at < p || at + fromLen > size || memcmp(text + at, from, (size_t)fromLen) != 0) { free(b.s); return NULL; }
        SbAdd(&b, text + p, at - p);
        SbAdd(&b, to, toLen);
        p = at + fromLen;
        shift += h->newLen - h->oldLen;
    }
    SbAdd(&b, text + p, size - p);
    if (b.oom) { free(b.s); return NULL; }
    if (!b.s) b.s = MS_StrDup("");
    *outSize = b.len;
    return b.s;
}

// #endregion

// ======================================================================================
// #region Apply, undo, redo
// ======================================================================================

static void FreeStep(EditStep *s)
{
    for (int i = 0; i < s->hunkCount; i++) { free(s->hunks[i].oldText); free(s->hunks[i].newText); }
    free(s->hunks);
    free(s->selBefore);
    free(s->selAfter);
    memset(s, 0, sizeof(*s));
}

static void ClearStack(EditStep *stack, int *count)
{
    for (int i = 0; i < *count; i++) FreeStep(&stack[i]);
    *count = 0;
}

void MapEditFree(MapEditor *E)
{
    ClearStack(E->undo, &E->undoCount);
    ClearStack(E->redo, &E->redoCount);
    free(E->undo);
    free(E->redo);
    memset(E, 0, sizeof(*E));
}

bool MapEditSync(MapEditor *E, char *msg, int msgSize)
{
    if ((E->undoCount == 0 && E->redoCount == 0) || scene->hash == E->knownHash) return false;
    ClearStack(E->undo, &E->undoCount);
    ClearStack(E->redo, &E->redoCount);
    snprintf(msg, msgSize, "the map changed on disk (not by a manual edit): undo history cleared");
    return true;
}

// The file on disk, which must be the loaded map (hash). NULL with msg otherwise.
static char *ReadLoadedMap(unsigned long long expect, int *size, char *msg, int msgSize)
{
    char *disk = ReadFileBytes(scene->path, size);
    if (!disk) { snprintf(msg, msgSize, "cannot read %s", scene->path); return NULL; }
    if (Fnv1a64(disk, *size) != expect) {
        free(disk);
        snprintf(msg, msgSize, "the map on disk changed since it was loaded (a request? another editor?): wait for the reload");
        return NULL;
    }
    return disk;
}

// Writes text over the map and reloads it; on a failed reload writes `restore` back
static bool WriteAndReload(const char *text, int size, const char *restore, int restoreSize, char *msg, int msgSize)
{
    if (!WriteFileBytes(scene->path, text, size)) {
        WriteFileBytes(scene->path, restore, restoreSize);
        snprintf(msg, msgSize, "cannot write %s", scene->path);
        return false;
    }
    char err[512];
    if (MS_Reload(scene, NULL, err, sizeof(err)) == MS_RELOADED) return true;
    WriteFileBytes(scene->path, restore, restoreSize);
    char err2[512];
    MS_Reload(scene, NULL, err2, sizeof(err2));
    snprintf(msg, msgSize, "the edit would break the map, reverted: %s", err);
    return false;
}

static MS_EntityId *CopyIds(const MS_EntityId *ids, int n)
{
    MS_EntityId *c = malloc(sizeof(MS_EntityId) * (n > 0 ? n : 1));
    if (c && n > 0) memcpy(c, ids, sizeof(MS_EntityId) * n);
    return c;
}

EditResult MapEditApply(MapEditor *E, const MS_ObjRef *sel, int selCount, const EditOp *op,
                        MS_EntityId **outSel, int *outSelCount, char *msg, int msgSize)
{
    *outSel = NULL;
    *outSelCount = 0;
    if (MS_CompiledSource(scene, NULL) != MS_SOURCE_TEXT) {
        snprintf(msg, msgSize, "%s is a compiled map: edits change the map's text (load its .map)", scene->path);
        return EDIT_FAILED;
    }
    unsigned long long before = scene->hash;
    int size;
    char *disk = ReadLoadedMap(before, &size, msg, msgSize);
    if (!disk) return EDIT_FAILED;

    Ctx C = { .op = op };
    Patch P = { .text = disk, .size = size };
    EditStep step = { 0 };
    EditResult res = EDIT_FAILED;
    char *after = NULL;
    if (!BuildPatch(&C, &P, sel, selCount)) { snprintf(msg, msgSize, "%s", C.why); goto done; }
    if (!MakeHunks(&P, &step)) { snprintf(msg, msgSize, "out of memory"); goto done; }
    int afterSize;
    if (!(after = ApplyHunks(disk, size, &step, false, &afterSize))) { snprintf(msg, msgSize, "out of memory"); goto done; }
    if (afterSize == size && memcmp(after, disk, (size_t)size) == 0) { snprintf(msg, msgSize, "%s: nothing changes", P.what); goto done; }

    // The selection before (for undo), as entity ids
    step.selBefore = malloc(sizeof(MS_EntityId) * (selCount > 0 ? selCount : 1));
    step.selAfter = CopyIds(P.sel, P.selCount);
    if (!step.selBefore || !step.selAfter) { snprintf(msg, msgSize, "out of memory"); goto done; }
    for (int i = 0; i < selCount; i++) {
        int e = MS_ObjectEntity(scene, sel[i]);
        if (e >= 0) step.selBefore[step.selBeforeCount++] = scene->entityIds[e];
    }
    step.selAfterCount = P.selCount;
    step.hashBefore = before;
    step.hashAfter = Fnv1a64(after, afterSize);
    snprintf(step.what, sizeof(step.what), "%s", P.what);

    EditStep *slot = MS_RESERVE(E->undo, E->undoCap, E->undoCount + 1) ? &E->undo[E->undoCount] : NULL;
    if (!slot) { snprintf(msg, msgSize, "out of memory"); goto done; }
    if (!WriteAndReload(after, afterSize, disk, size, msg, msgSize)) goto done;
    *slot = step;
    E->undoCount++;
    memset(&step, 0, sizeof(step));
    ClearStack(E->redo, &E->redoCount);
    E->knownHash = scene->hash;
    *outSel = CopyIds(P.sel, P.selCount);
    *outSelCount = *outSel ? P.selCount : 0;
    snprintf(msg, msgSize, "%s", E->undo[E->undoCount - 1].what);
    res = EDIT_DONE;
done:
    FreeStep(&step);
    FreePatch(&P);
    free(C.rw.sp);
    free(C.rw.pool.s);
    free(after);
    free(disk);
    return res;
}

// Moves the top of one stack to the other, writing its inverse (undo) or itself (redo)
static EditResult Replay(MapEditor *E, bool undo, MS_EntityId **outSel, int *outSelCount, char *msg, int msgSize)
{
    *outSel = NULL;
    *outSelCount = 0;
    EditStep *from = undo ? E->undo : E->redo;
    int *fromCount = undo ? &E->undoCount : &E->redoCount;
    if (*fromCount == 0) { snprintf(msg, msgSize, undo ? "nothing to undo" : "nothing to redo"); return EDIT_FAILED; }
    EditStep *step = &from[*fromCount - 1];
    int size, outSize;
    char *disk = ReadLoadedMap(undo ? step->hashAfter : step->hashBefore, &size, msg, msgSize);
    if (!disk) return EDIT_FAILED;
    char *text = ApplyHunks(disk, size, step, undo, &outSize);
    EditResult res = EDIT_FAILED;
    if (!text || Fnv1a64(text, outSize) != (undo ? step->hashBefore : step->hashAfter)) {
        snprintf(msg, msgSize, "%s does not apply to the map any more", undo ? "undo" : "redo");
        goto done;
    }
    EditStep **toArr = undo ? &E->redo : &E->undo;
    int *toCount = undo ? &E->redoCount : &E->undoCount, *toCap = undo ? &E->redoCap : &E->undoCap;
    if (!MS_RESERVE(*toArr, *toCap, *toCount + 1)) { snprintf(msg, msgSize, "out of memory"); goto done; }
    if (!WriteAndReload(text, outSize, disk, size, msg, msgSize)) goto done;
    const MS_EntityId *ids = undo ? step->selBefore : step->selAfter;
    int n = undo ? step->selBeforeCount : step->selAfterCount;
    *outSel = CopyIds(ids, n);
    *outSelCount = *outSel ? n : 0;
    snprintf(msg, msgSize, "%s: %s", undo ? "undone" : "redone", step->what);
    (*toArr)[(*toCount)++] = *step;
    (*fromCount)--;
    E->knownHash = scene->hash;
    res = EDIT_DONE;
done:
    free(text);
    free(disk);
    return res;
}

EditResult MapEditUndo(MapEditor *E, MS_EntityId **outSel, int *outSelCount, char *msg, int msgSize)
{
    return Replay(E, true, outSel, outSelCount, msg, msgSize);
}

EditResult MapEditRedo(MapEditor *E, MS_EntityId **outSel, int *outSelCount, char *msg, int msgSize)
{
    return Replay(E, false, outSel, outSelCount, msg, msgSize);
}

// #endregion

// ======================================================================================
// #region Queries for the edit mode
// ======================================================================================

static double Round4(float v) { return round((double)v * 10000.0) / 10000.0; }

void MapEditAnchor(MS_ObjRef o, double out[3])
{
    Vector3 p = { 0 };
    if (o.kind == MS_OBJ_BOX && o.index >= 0 && o.index < scene->boxCount) p = scene->boxes[o.index].min;
    else if (o.kind == MS_OBJ_MODEL && o.index >= 0 && o.index < scene->modelCount) p = scene->models[o.index].position;
    else if (o.kind == MS_OBJ_LIGHT && o.index >= 0 && o.index < scene->lightCount) p = scene->lights[o.index].light.position;
    out[0] = Round4(p.x);
    out[1] = Round4(p.y);
    out[2] = Round4(p.z);
}

void MapEditIdText(MS_ObjRef o, char *out, int outSize)
{
    int e = MS_ObjectEntity(scene, o);
    if (e < 0) { if (outSize > 0) out[0] = '\0'; return; }
    MS_EntityId id = scene->entityIds[e];
    if (MS_ENTITY_EXPLICIT(id)) snprintf(out, outSize, "id %u:%d", id.stmt, id.index);
    else snprintf(out, outSize, "id %08x:%d (implicit)", id.stmt, id.index);
}

int MapEditNextId(void)
{
    int size;
    const char *text = MS_MapText(scene, &size);
    Patch P = { .text = text, .size = size };
    int next = IndexLines(&P) ? MaxId(&P) + 1 : 1;
    free(P.lines);
    return next > 0 ? next : 1;
}

void MapEditDescribe(MS_ObjRef o, char *out, int outSize)
{
    int e = MS_ObjectEntity(scene, o);
    if (e < 0) { snprintf(out, outSize, "-"); return; }
    const MS_Source *s = &scene->entitySources[e];
    int a = e, b = e;
    while (a > 0 && scene->entitySources[a - 1].prim == s->prim) a--;
    while (b + 1 < scene->entityCount && scene->entitySources[b + 1].prim == s->prim) b++;
    int copies = scene->entitySources[b].copy + 1;
    char idText[48];
    MapEditIdText(o, idText, sizeof(idText));
    if (copies > 1) snprintf(out, outSize, "line %d (copy %d of %d), %s", s->line, s->copy, copies, idText);
    else snprintf(out, outSize, "line %d, %s", s->line, idText);
}

// #endregion

// ======================================================================================
// #region Edit script (--apply-edits)
// ======================================================================================

typedef struct { MS_ObjRef *v; int n, cap; } ObjList;

static void ListAdd(ObjList *l, MS_ObjRef o)
{
    for (int i = 0; i < l->n; i++) if (l->v[i].kind == o.kind && l->v[i].index == o.index) return;
    MS_ObjRef *s = MS_PUSH(l->v, l->n, l->cap);
    if (s) *s = o;
}

static void SelectIds(ObjList *l, const MS_EntityId *ids, int n)
{
    l->n = 0;
    for (int i = 0; i < n; i++) {
        int e = MS_FindEntity(scene, ids[i]);
        if (e >= 0) ListAdd(l, scene->entityObjects[e]);
    }
}

static const char *KindName(MS_ObjKind k) { return k == MS_OBJ_BOX ? "box" : k == MS_OBJ_MODEL ? "model" : k == MS_OBJ_LIGHT ? "light" : "none"; }

// Script words: whitespace-separated, "..." one word, '#' ends the line (in place)
static int ScriptWords(char *line, char **w, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p || *p == '#') break;
        if (*p == '"') {
            char *close = strchr(p + 1, '"');
            if (!close) return -1;
            *close = '\0';
            w[n++] = p + 1;
            p = close + 1;
            continue;
        }
        w[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '#') p++;
        if (*p == '#') { *p = '\0'; break; }
        if (*p) *p++ = '\0';
    }
    return n;
}

static bool ScriptNum(const char *s, double *out)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end || !isfinite(v) || fabs(v) > 1e6) return false;
    *out = v;
    return true;
}

static bool ScriptInt(const char *s, long *out)
{
    char *end;
    long v = strtol(s, &end, 0);
    if (end == s || *end) return false;
    *out = v;
    return true;
}

int MapEditRunScript(const char *scriptPath)
{
    int size;
    char *text = ReadFileBytes(scriptPath, &size);
    if (!text) { fprintf(stderr, "monolith: cannot read the edit script %s\n", scriptPath); return 1; }
    MapEditor E = { 0 };
    ObjList sel = { 0 };
    int status = 0, lineNo = 0;
    char *save = text;
    while (save && *save && status == 0) {
        char *line = save, *nl = strchr(save, '\n');
        if (nl) { *nl = '\0'; save = nl + 1; } else save = NULL;
        lineNo++;
        char *w[12];
        int n = ScriptWords(line, w, 12);
        if (n == 0) continue;
        char msg[512] = "";
        bool ok = false;
        EditOp op = { 0 };
        bool isEdit = false;
        if (n < 0) snprintf(msg, sizeof(msg), "unterminated string");
        else if (strcmp(w[0], "select") == 0 || strcmp(w[0], "also") == 0) {
            if (strcmp(w[0], "select") == 0) sel.n = 0;
            long a = 0, b = -1;
            if (n == 2 && strcmp(w[1], "none") == 0) { sel.n = 0; ok = true; }
            else if (n >= 3 && strcmp(w[1], "line") == 0 && ScriptInt(w[2], &a) &&
                     (n == 3 || (n == 5 && strcmp(w[3], "copy") == 0 && ScriptInt(w[4], &b)))) {
                for (int e = 0; e < scene->entityCount; e++)
                    if (scene->entitySources[e].line == a && (b < 0 || scene->entitySources[e].copy == b)) ListAdd(&sel, scene->entityObjects[e]);
                ok = true;
            } else if (n >= 3 && strcmp(w[1], "id") == 0 && ScriptInt(w[2], &a) &&
                       (n == 3 || (n == 5 && strcmp(w[3], "index") == 0 && ScriptInt(w[4], &b)))) {
                for (int e = 0; e < scene->entityCount; e++)
                    if (scene->entityIds[e].stmt == (unsigned int)a && (b < 0 || scene->entityIds[e].index == b)) ListAdd(&sel, scene->entityObjects[e]);
                ok = true;
            } else if (n == 4 && strcmp(w[1], "entity") == 0) {
                char *end;
                unsigned long k = strtoul(w[2], &end, 16);
                if (!*end && ScriptInt(w[3], &b)) {
                    int e = MS_FindEntity(scene, (MS_EntityId){ (unsigned int)k, (int)b });
                    if (e >= 0) ListAdd(&sel, scene->entityObjects[e]);
                    ok = true;
                }
            }
            if (!ok) snprintf(msg, sizeof(msg), "select line <n> [copy <c>] | id <n> [index <i>] | entity <hex> <index> | none");
            else snprintf(msg, sizeof(msg), "%d object%s selected", sel.n, sel.n == 1 ? "" : "s");
        } else if (strcmp(w[0], "move") == 0 || strcmp(w[0], "duplicate") == 0) {
            op.kind = w[0][0] == 'm' ? EDIT_MOVE : EDIT_DUPLICATE;
            isEdit = n == 4 && ScriptNum(w[1], &op.delta[0]) && ScriptNum(w[2], &op.delta[1]) && ScriptNum(w[3], &op.delta[2]);
            if (!isEdit) snprintf(msg, sizeof(msg), "%s <dx> <dy> <dz>", w[0]);
        } else if (strcmp(w[0], "turn") == 0) {
            op.kind = EDIT_ROTATE;
            isEdit = n == 2 && ScriptNum(w[1], &op.degrees);
            if (!isEdit) snprintf(msg, sizeof(msg), "turn <degrees>");
        } else if (strcmp(w[0], "remove") == 0 && n == 1) {
            op.kind = EDIT_REMOVE;
            isEdit = true;
        } else if (strcmp(w[0], "add") == 0) {
            op.kind = EDIT_ADD;
            op.modelPath = n == 5 ? w[1] : "";
            isEdit = n == 5 && ScriptNum(w[2], &op.pos[0]) && ScriptNum(w[3], &op.pos[1]) && ScriptNum(w[4], &op.pos[2]) && sel.n > 0;
            if (isEdit) op.anchor = sel.v[0];
            else snprintf(msg, sizeof(msg), "add \"<model file>\" <x> <y> <z> (after the first selected object's line)");
        } else if (strcmp(w[0], "id") == 0) {
            long v = 0;
            op.kind = EDIT_ID;
            isEdit = n == 1 || (n == 2 && ScriptInt(w[1], &v) && v >= 1 && v <= 2147483647);
            op.id = (int)v;
            if (!isEdit) snprintf(msg, sizeof(msg), "id [<n>] (1..2147483647; without n: the next free id for each line without one)");
        } else if ((strcmp(w[0], "undo") == 0 || strcmp(w[0], "redo") == 0) && n == 1) {
            MS_EntityId *ids;
            int count;
            ok = (w[0][0] == 'u' ? MapEditUndo : MapEditRedo)(&E, &ids, &count, msg, sizeof(msg)) == EDIT_DONE;
            if (ok) SelectIds(&sel, ids, count);
            free(ids);
        } else if (strcmp(w[0], "print") == 0 && n == 1) {
            for (int i = 0; i < sel.n; i++) {
                int e = MS_ObjectEntity(scene, sel.v[i]);
                const MS_Source *s = &scene->entitySources[e];
                printf("edits:   %08x:%d %s %d line %d copy %d part %d\n", scene->entityIds[e].stmt, scene->entityIds[e].index,
                       KindName(sel.v[i].kind), sel.v[i].index, s->line, s->copy, s->part);
            }
            snprintf(msg, sizeof(msg), "%d selected", sel.n);
            ok = true;
        } else snprintf(msg, sizeof(msg), "unknown command '%s' (select also move turn remove duplicate add id undo redo print)", w[0]);

        if (isEdit) {
            MS_EntityId *ids;
            int count;
            ok = MapEditApply(&E, sel.v, sel.n, &op, &ids, &count, msg, sizeof(msg)) == EDIT_DONE;
            if (ok) SelectIds(&sel, ids, count);
            free(ids);
        }
        printf("edits: %s:%d: %s%s\n", scriptPath, lineNo, ok ? "" : "FAILED: ", msg);
        if (!ok) status = 1;
    }
    fflush(stdout);
    free(sel.v);
    MapEditFree(&E);
    free(text);
    return status;
}

// #endregion
