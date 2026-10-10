// Map loader and hot reload.
//
// ======================================================================================
// Map format (.map), one statement per line
// ======================================================================================
//
//   # comment                      '#' starts a comment anywhere outside a "string"
//   group "name"                   every following primitive belongs to this group,
//                                  until the next `group` (names: up to 63 chars, no '"')
//
// Primitives (meters; +Y is up; numbers are plain decimals, e.g. 12, -4.5, 0.75):
//   box    x0 y0 z0 x1 y1 z1                        two opposite corners, any order
//   block  cx y0 cz sx sy sz                        centered on (cx, cz), from y0 up to y0 + sy
//   stairs cx baseY startZ width steps rise run     `steps` (integer) solid steps climbing
//                                                   toward -Z from startZ, each down to baseY
//   waffle x0 x1 y0 y1 z depth bayW floorH          facade: back wall z-1..z, vertical fins
//                                                   every bayW along x, ledges every floorH,
//                                                   both sticking out from z to z + depth
//
// Placement prefixes, written before a primitive on the same line (instead of
// expanding loops into many lines):
//   repeat n dx dy dz              n copies, copy i (0-based) shifted by i * (dx, dy, dz);
//                                  up to 3 repeats on one line give a grid (the first is
//                                  the outer loop, so it decides the box order)
//   at x y z                       shift by (x, y, z), so the primitive's own coordinates
//                                  are relative to this point
// Shifts apply to the position arguments only (box: all six; block: cx y0 cz; stairs:
// cx baseY startZ; waffle: x0 x1 y0 y1 z), never to sizes or counts. Each position is
// computed as (at + i*d1 + j*d2 + k*d3) + value, in float. With whole or half numbers
// the order doesn't matter; with values like 2.6 it can change the last bit of a float
// ((-36 + 4.5) + 2.6 vs -33.4 + 4.5). `at` exists to reproduce the original C loops,
// which computed (start + i*step) + 2.6. Example, 17 roof beams 4.5 m apart, each
// 2.6 m deep, the first one at z = -36:
//   repeat 17 0 0 4.5 at 0 0 -36 box -40 20 0 40 22 2.6
//
// Every box remembers the map line of the statement that made it (world.refs[]).
// Errors are reported as "file:line: message" and the previous map is kept.
//
// ======================================================================================

#include "map.h"
#include <errno.h>
#include <math.h>

#define MAP_FILE_MAX     (4 * 1024 * 1024)
#define MAP_TOKENS_MAX   64
#define MAP_REPEATS_MAX  3
#define MAP_POLL_INTERVAL 0.5f
// After an mtime change, keep re-reading for this long: GetFileModTime has 1 s
// resolution, and editors that truncate + write (or rename) can leave an empty or
// partial file for a moment, then finish within the same second (same mtime).
#define MAP_SETTLE_TIME   2.0f

// ======================================================================================
// #region Primitives (the former BuildWorld() builders; float math kept exactly as it was)
// ======================================================================================

// Loading state shared by the primitives: target world and the reference stamped on
// every box. A primitive returns false when the world is full.
static World *dst;
static BoxRef curRef;
static bool badNumber;  // the last failed Box() overflowed float, rather than the world being full

static bool Box(float x0, float y0, float z0, float x1, float y1, float z1)
{
    if (dst->count >= MAX_BOXES) return false;
    BoundingBox b = {
        { fminf(x0, x1), fminf(y0, y1), fminf(z0, z1) },
        { fmaxf(x0, x1), fmaxf(y0, y1), fmaxf(z0, z1) },
    };
    // Each number is finite, but shifts (repeat * d, at) or the size can still overflow;
    // inf/NaN would reach the instance scale, collision and picking
    if (!isfinite(b.min.x) || !isfinite(b.min.y) || !isfinite(b.min.z) ||
        !isfinite(b.max.x) || !isfinite(b.max.y) || !isfinite(b.max.z) ||
        !isfinite(b.max.x - b.min.x) || !isfinite(b.max.y - b.min.y) || !isfinite(b.max.z - b.min.z)) {
        badNumber = true;
        return false;
    }
    dst->items[dst->count] = b;
    dst->refs[dst->count++] = curRef;
    return true;
}

// Box from center (x, z), ground level y0 and size
static bool Block(float cx, float y0, float cz, float sx, float sy, float sz)
{
    return Box(cx - sx * 0.5f, y0, cz - sz * 0.5f, cx + sx * 0.5f, y0 + sy, cz + sz * 0.5f);
}

// Straight staircase climbing toward -Z. Each step is a solid block down to baseY.
static bool Stairs(float cx, float baseY, float startZ, float width, int steps, float rise, float run)
{
    for (int i = 0; i < steps; i++) {
        float z0 = startZ - i * run;
        if (!Box(cx - width * 0.5f, baseY, z0, cx + width * 0.5f, baseY + (i + 1) * rise, z0 - run)) return false;
    }
    return true;
}

// Facade with a grid of deep window recesses ("waffle" facade):
// a back wall plus vertical fins and horizontal ledges in front of it.
// Every loop iteration adds a box, so MAX_BOXES bounds the loops even for silly input.
static bool WaffleFacade(float x0, float x1, float y0, float y1, float z, float depth, float bayW, float floorH)
{
    if (!Box(x0, y0, z - 1.0f, x1, y1, z)) return false;                // back wall
    for (float x = x0; x <= x1 + 0.01f; x += bayW)                       // vertical fins
        if (!Box(x - 0.3f, y0, z, x + 0.3f, y1, z + depth)) return false;
    for (float y = y0 + floorH; y <= y1 + 0.01f; y += floorH)            // horizontal ledges
        if (!Box(x0, y - 0.5f, z, x1, y, z + depth)) return false;
    return true;
}

// #endregion

// ======================================================================================
// #region Parser
// ======================================================================================

enum { AX_NONE, AX_X, AX_Y, AX_Z, AX_INT };   // what a primitive argument is

typedef struct {
    const char *name;
    int argc;
    unsigned char kind[8];     // per argument: axis it is shifted along, or AX_INT
} PrimDef;

static const PrimDef PRIMS[] = {
    { "box",    6, { AX_X, AX_Y, AX_Z, AX_X, AX_Y, AX_Z } },
    { "block",  6, { AX_X, AX_Y, AX_Z, AX_NONE, AX_NONE, AX_NONE } },
    { "stairs", 7, { AX_X, AX_Y, AX_Z, AX_NONE, AX_INT, AX_NONE, AX_NONE } },
    { "waffle", 8, { AX_X, AX_X, AX_Y, AX_Y, AX_Z, AX_NONE, AX_NONE, AX_NONE } },
};
#define PRIM_COUNT ((int)(sizeof(PRIMS) / sizeof(PRIMS[0])))

typedef struct {
    char *s;
    bool quoted;
} Token;

// Where Fail() writes: "file:line: message"
static const char *errPath;
static int errLine;
static char *errOut;
static int errOutSize;

static bool Fail(const char *fmt, const char *arg)
{
    char msg[256];
    snprintf(msg, sizeof(msg), fmt, arg ? arg : "");
    if (errLine > 0) snprintf(errOut, errOutSize, "%s:%d: %s", errPath, errLine, msg);
    else snprintf(errOut, errOutSize, "%s: %s", errPath, msg);
    return false;
}

static bool ParseFloat(const char *s, float *out)
{
    char *end;
    errno = 0;
    float v = strtof(s, &end);
    if (end == s || *end != '\0' || !isfinite(v)) return Fail("bad number '%s'", s);
    *out = v;
    return true;
}

static bool ParseInt(const char *s, int lo, int hi, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno) return Fail("bad integer '%s'", s);
    if (v < lo || v > hi) {
        char range[64];
        snprintf(range, sizeof(range), "%s (allowed %d..%d)", s, lo, hi);
        return Fail("integer out of range: %s", range);
    }
    *out = (int)v;
    return true;
}

// Splits a line into whitespace-separated tokens, in place. "..." is one token (no
// escapes); '#' outside quotes ends the line.
static bool Tokenize(char *line, Token *tok, int *count)
{
    int n = 0;
    char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') break;
        if (n == MAP_TOKENS_MAX) return Fail("too many words on one line", NULL);
        if (*p == '"') {
            char *close = strchr(p + 1, '"');
            if (!close) return Fail("unterminated string", NULL);
            *close = '\0';
            tok[n++] = (Token){ p + 1, true };
            p = close + 1;
            if (*p != '\0' && *p != ' ' && *p != '\t' && *p != '#') return Fail("missing space after string", NULL);
            continue;
        }
        tok[n++] = (Token){ p, false };
        while (*p && *p != ' ' && *p != '\t' && *p != '#' && *p != '"') p++;
        if (*p == '"') return Fail("unexpected '\"' inside a word", NULL);
        if (*p == '#') { *p = '\0'; break; }
        if (*p) *p++ = '\0';
    }
    *count = n;
    return true;
}

// One statement (already tokenized, at least one token)
static bool ParseStatement(Token *tok, int n, int *primIndex)
{
    for (int i = 0; i < n; i++)
        if (tok[i].quoted && !(i == 1 && strcmp(tok[0].s, "group") == 0))
            return Fail("unexpected string \"%s\"", tok[i].s);

    if (strcmp(tok[0].s, "group") == 0) {
        if (n != 2) return Fail("group takes one name in double quotes: group \"name\"", NULL);
        if (!tok[1].quoted) return Fail("group name must be in double quotes: group \"%s\"", tok[1].s);
        if (tok[1].s[0] == '\0') return Fail("empty group name", NULL);
        if (strlen(tok[1].s) >= GROUP_NAME_MAX) return Fail("group name too long (max 63 characters)", NULL);
        if (dst->groupCount >= MAX_GROUPS) return Fail("too many groups (max 256)", NULL);
        snprintf(dst->groups[dst->groupCount], GROUP_NAME_MAX, "%s", tok[1].s);
        curRef.group = dst->groupCount++;
        return true;
    }

    // Placement prefixes
    float at[3] = { 0 };
    bool hasAt = false;
    int repN[MAP_REPEATS_MAX];
    float repD[MAP_REPEATS_MAX][3];
    int reps = 0;
    long total = 1;
    int t = 0;
    for (;;) {
        if (t >= n) return Fail("missing primitive after repeat/at", NULL);
        if (strcmp(tok[t].s, "repeat") == 0) {
            if (reps == MAP_REPEATS_MAX) return Fail("at most 3 repeats per line", NULL);
            if (t + 4 >= n) return Fail("repeat takes 4 numbers: repeat n dx dy dz", NULL);
            if (!ParseInt(tok[t + 1].s, 0, MAX_BOXES, &repN[reps])) return false;
            for (int a = 0; a < 3; a++)
                if (!ParseFloat(tok[t + 2 + a].s, &repD[reps][a])) return false;
            total *= repN[reps] > 0 ? repN[reps] : 1;
            // Bounds the work even for primitives that make no boxes (stairs with 0 steps)
            if (total > MAX_BOXES) return Fail("too many copies (max 8192 boxes)", NULL);
            reps++;
            t += 5;
        } else if (strcmp(tok[t].s, "at") == 0) {
            if (hasAt) return Fail("only one 'at' per line", NULL);
            if (t + 3 >= n) return Fail("at takes 3 numbers: at x y z", NULL);
            for (int a = 0; a < 3; a++)
                if (!ParseFloat(tok[t + 1 + a].s, &at[a])) return false;
            hasAt = true;
            t += 4;
        } else break;
    }

    const PrimDef *def = NULL;
    for (int i = 0; i < PRIM_COUNT; i++)
        if (strcmp(tok[t].s, PRIMS[i].name) == 0) def = &PRIMS[i];
    if (!def) return Fail("unknown keyword '%s' (expected group, box, block, stairs, waffle, repeat or at)", tok[t].s);

    int argc = n - t - 1;
    if (argc != def->argc) {
        char what[64];
        snprintf(what, sizeof(what), "%s takes %d numbers, got %d", def->name, def->argc, argc);
        return Fail("%s", what);
    }
    float v[8];
    int steps = 0;
    for (int a = 0; a < argc; a++) {
        if (def->kind[a] == AX_INT) {
            if (!ParseInt(tok[t + 1 + a].s, 0, MAX_BOXES, &steps)) return false;
            v[a] = 0.0f;
        } else if (!ParseFloat(tok[t + 1 + a].s, &v[a])) return false;
    }
    if (def->argc == 8 && (v[6] <= 0.0f || v[7] <= 0.0f)) return Fail("waffle bayW and floorH must be > 0", NULL);

    curRef.prim = (*primIndex)++;
    bool shifted = hasAt || reps > 0;
    int cnt[MAP_REPEATS_MAX] = { 1, 1, 1 };
    for (int r = 0; r < reps; r++) cnt[r] = repN[r];

    for (int i0 = 0; i0 < cnt[0]; i0++)
    for (int i1 = 0; i1 < cnt[1]; i1++)
    for (int i2 = 0; i2 < cnt[2]; i2++) {
        int idx[MAP_REPEATS_MAX] = { i0, i1, i2 };
        float off[3];
        for (int a = 0; a < 3; a++) {
            off[a] = at[a];
            for (int r = 0; r < reps; r++) off[a] += (float)idx[r] * repD[r][a];
        }
        float p[8];
        for (int a = 0; a < argc; a++) {
            int k = def->kind[a];
            // Unshifted lines use the numbers as written (no "+ 0", which would turn -0 into 0)
            p[a] = (shifted && k >= AX_X && k <= AX_Z) ? off[k - AX_X] + v[a] : v[a];
        }
        bool ok;
        switch (def->argc) {
            case 7:  ok = Stairs(p[0], p[1], p[2], p[3], steps, p[5], p[6]); break;
            case 8:  ok = WaffleFacade(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]); break;
            default: ok = (def == &PRIMS[0]) ? Box(p[0], p[1], p[2], p[3], p[4], p[5])
                                             : Block(p[0], p[1], p[2], p[3], p[4], p[5]); break;
        }
        if (!ok) return Fail(badNumber ? "coordinate or size out of range" : "too many boxes (max 8192)", NULL);
    }
    return true;
}

static bool ParseMap(const char *text, int size)
{
    int primIndex = 0;
    int lineNo = 0;
    int pos = 0;
    char line[MAP_LINE_MAX + 2];
    Token tok[MAP_TOKENS_MAX];
    curRef = (BoxRef){ 0, -1, -1 };
    badNumber = false;
    while (pos < size) {
        int start = pos;
        while (pos < size && text[pos] != '\n') pos++;
        int len = pos - start;
        pos++;                              // skip '\n'
        errLine = ++lineNo;
        if (len > 0 && text[start + len - 1] == '\r') len--;
        if (len > MAP_LINE_MAX) return Fail("line too long (max 1024 characters)", NULL);
        memcpy(line, text + start, len);
        line[len] = '\0';
        if ((int)strlen(line) != len) return Fail("unexpected NUL byte", NULL);
        for (int i = 0; i < len; i++)
            if ((unsigned char)line[i] < 32 && line[i] != '\t') return Fail("unexpected control character", NULL);

        int n;
        if (!Tokenize(line, tok, &n)) return false;
        if (n == 0) continue;
        curRef.line = lineNo;
        if (!ParseStatement(tok, n, &primIndex)) return false;
    }
    errLine = 0;
    if (dst->count == 0) return Fail("map has no boxes", NULL);
    return true;
}

// #endregion

// ======================================================================================
// #region Loading, hot reload
// ======================================================================================

static char *mapText;           // text of the loaded map, for MapGetLine
static int mapTextSize;
static char mapError[512];

// Hot reload state: last seen mtime, and hash of the last contents we tried to load
static float pollTimer, settleTime;
static long seenMtime;
static unsigned long long triedHash;

static unsigned long long Fnv1a64(const char *data, int size)
{
    unsigned long long h = 14695981039346656037ULL;
    for (int i = 0; i < size; i++) {
        h ^= (unsigned char)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// Reads a whole file (malloc'd, NUL-terminated). NULL with a message in err on failure.
static char *ReadWholeFile(const char *path, int *size, char *err, int errSize)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errSize, "%s: cannot open file", path); return NULL; }
    char *buf = NULL;
    long len = -1;
    if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
    if (len < 0 || len > MAP_FILE_MAX || fseek(f, 0, SEEK_SET) != 0)
        snprintf(err, errSize, "%s: cannot read file (or larger than 4 MB)", path);
    else if (!(buf = malloc(len + 1)))
        snprintf(err, errSize, "%s: out of memory", path);
    else if ((long)fread(buf, 1, len, f) != len) {
        snprintf(err, errSize, "%s: read error", path);
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) { buf[len] = '\0'; *size = (int)len; }
    return buf;
}

// Parses `text` (takes ownership) and swaps it in on success
static bool LoadMapText(const char *path, char *text, int size, char *err, int errSize)
{
    // Static: a World is ~330 KB, too big for the stack
    static World tmp;
    tmp.count = 0;
    tmp.groupCount = 0;
    dst = &tmp;
    errPath = path;
    errLine = 0;
    errOut = err;
    errOutSize = errSize;
    bool ok = ParseMap(text, size);
    dst = NULL;
    if (!ok) {
        snprintf(mapError, sizeof(mapError), "%s", err);
        free(text);
        return false;
    }

    tmp.version = world.version + 1;
    snprintf(tmp.path, sizeof(tmp.path), "%s", path);
    tmp.mtime = GetFileModTime(path);
    tmp.hash = Fnv1a64(text, size);
    world = tmp;

    free(mapText);
    mapText = text;
    mapTextSize = size;
    mapError[0] = '\0';
    if (errSize > 0) err[0] = '\0';
    seenMtime = world.mtime;
    triedHash = world.hash;
    settleTime = 0.0f;
    return true;
}

bool LoadMap(const char *path, char *err, int errSize)
{
    int size;
    char *text = ReadWholeFile(path, &size, err, errSize);
    if (!text) {
        snprintf(mapError, sizeof(mapError), "%s", err);
        return false;
    }
    return LoadMapText(path, text, size, err, errSize);
}

MapPollResult MapPoll(float dt)
{
    pollTimer += dt;
    if (pollTimer < MAP_POLL_INTERVAL || !world.path[0]) return MAP_UNCHANGED;
    pollTimer = 0.0f;

    long mtime = GetFileModTime(world.path);
    if (mtime == 0) return MAP_UNCHANGED;       // missing for a moment (editor rename): wait
    if (mtime != seenMtime) {
        seenMtime = mtime;
        settleTime = MAP_SETTLE_TIME;
    }
    if (settleTime <= 0.0f) return MAP_UNCHANGED;
    settleTime -= MAP_POLL_INTERVAL;

    char err[512];
    int size;
    char *text = ReadWholeFile(world.path, &size, err, sizeof(err));
    if (!text) {
        // Usually transient (the next poll retries), but a file that stays unreadable or
        // too big must not fail silently: report it once the settle window has run out
        if (settleTime > 0.0f) return MAP_UNCHANGED;
        snprintf(mapError, sizeof(mapError), "%s", err);
        triedHash = 0;
        return MAP_RELOAD_FAILED;
    }
    unsigned long long hash = Fnv1a64(text, size);
    if (hash == triedHash) {                    // touched, or the same broken file again
        free(text);
        return MAP_UNCHANGED;
    }
    triedHash = hash;
    char path[sizeof(world.path)];
    snprintf(path, sizeof(path), "%s", world.path);
    return LoadMapText(path, text, size, err, sizeof(err)) ? MAP_RELOADED : MAP_RELOAD_FAILED;
}

const char *MapError(void)
{
    return mapError;
}

bool MapGetLine(int line, char *out, int outSize)
{
    if (!mapText || line < 1 || outSize <= 0) return false;
    int pos = 0;
    for (int l = 1; l < line; l++) {
        while (pos < mapTextSize && mapText[pos] != '\n') pos++;
        if (pos >= mapTextSize) return false;
        pos++;
    }
    if (pos >= mapTextSize) return false;
    int len = 0;
    while (pos + len < mapTextSize && mapText[pos + len] != '\n') len++;
    if (len > 0 && mapText[pos + len - 1] == '\r') len--;
    if (len > outSize - 1) len = outSize - 1;
    memcpy(out, mapText + pos, len);
    out[len] = '\0';
    return true;
}

// #endregion
