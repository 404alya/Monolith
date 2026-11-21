// drawsettings -- the draw mode's kept state, <package>/draw.ini (drawsettings.h; docs/painting.md 18.6)
//
// `key = value` lines as settings.ini: a full-line `#` is a comment (no comment after a value: the
// colours start with '#'), blank lines are skipped. Loading follows GfxLoad's rules: a missing file is
// the defaults, an unknown key or a bad value is one warning with its line and that key keeps its
// value, the rest loads; a number out of its range is clamped (as GfxLoad clamps). Saving writes the
// whole file to <path>.tmp and renames it over <path>, so a crash never leaves half a file.
// Pure: no window, no scene; the palette is the only heap part.
#include "drawsettings.h"
#include "drawkeys.h"
#include "monolith_scene.h"     // MS_RESERVE, MS_StrDup

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The brush shapes as drawmode.h's PenKind numbers them (PEN_ROUND, PEN_SQUARE, PEN_SOFT); this file
// stays free of drawmode.h (it pulls in the scene)
static const char *const BRUSHES[] = { "round", "square", "soft" };
static const char *const BUCKETS[] = { "contiguous", "global", "region" };     // DrawBucketMode
static const char *const PICKS[] = { "paint", "screen" };                      // DrawPickMode

#define SIZE_MAX_TEXELS 256     // DRAW_SIZE_MAX
#define SPRAY_MAX 256

void DrawStateDefaults(DrawState *s)
{
    memset(s, 0, sizeof(*s));
    s->tool = TOOL_PENCIL;
    s->shape = 0;               // round
    s->size = 4;
    s->hardness = 0.5f;
    s->pixelPerfect = false;
    s->fg = (Color){ 200, 50, 50, 255 };
    s->bg = (Color){ 255, 255, 255, 255 };
    s->sprayWidth = 16;
    s->spraySpeed = 32;
    s->bucket = DRAW_BUCKET_CONTIGUOUS;
    s->tolerance = 0;
    s->gradientRadial = false;
    s->pickAlt = DRAW_PICK_PAINT;
    s->pickTool = DRAW_PICK_SCREEN;
    s->density = 64.0f;         // DRAW_DENSITY_DEFAULT
    s->pixelLook = false;
    s->panelHidden = false;
}

void DrawStateFree(DrawState *s)
{
    free(s->palette);
    s->palette = NULL;
    s->paletteCount = s->paletteCap = 0;
}

bool DrawStateCopy(DrawState *dst, const DrawState *src)
{
    if (dst == src) return true;
    DrawStateFree(dst);
    *dst = *src;
    dst->palette = NULL;
    dst->paletteCount = dst->paletteCap = 0;
    if (src->paletteCount <= 0) return true;
    if (!MS_RESERVE(dst->palette, dst->paletteCap, src->paletteCount)) return false;
    memcpy(dst->palette, src->palette, sizeof(Color) * (size_t)src->paletteCount);
    dst->paletteCount = src->paletteCount;
    return true;
}

static bool SameColor(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

static bool SameColors(const Color *a, const Color *b, int n)
{
    for (int i = 0; i < n; i++) if (!SameColor(a[i], b[i])) return false;
    return true;
}

bool DrawStateEqual(const DrawState *a, const DrawState *b)
{
    return a->tool == b->tool && a->shape == b->shape && a->size == b->size && a->hardness == b->hardness &&
           a->pixelPerfect == b->pixelPerfect && SameColor(a->fg, b->fg) && SameColor(a->bg, b->bg) &&
           a->recentCount == b->recentCount && SameColors(a->recent, b->recent, a->recentCount) &&
           a->paletteCount == b->paletteCount && SameColors(a->palette, b->palette, a->paletteCount) &&
           a->sprayWidth == b->sprayWidth && a->spraySpeed == b->spraySpeed && a->bucket == b->bucket &&
           a->tolerance == b->tolerance && a->gradientRadial == b->gradientRadial && a->pickAlt == b->pickAlt &&
           a->pickTool == b->pickTool && a->density == b->density && a->pixelLook == b->pixelLook &&
           a->panelHidden == b->panelHidden;
}

char *DrawStatePath(const char *root)
{
    size_t n = strlen(root);
    bool slash = n > 0 && (root[n - 1] == '/' || root[n - 1] == '\\');
    char *out = malloc(n + 1 + sizeof(DRAWSTATE_FILE));
    if (!out) return NULL;
    memcpy(out, root, n);
    if (!slash && n > 0) out[n++] = '/';
    memcpy(out + n, DRAWSTATE_FILE, sizeof(DRAWSTATE_FILE));
    return out;
}

// #region Values

static int ClampI(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int NameIndex(const char *v, const char *const *names, int n)
{
    for (int i = 0; i < n; i++) if (strcmp(v, names[i]) == 0) return i;
    return -1;
}

static bool ParseOnOff(const char *v, bool *out)
{
    if (!strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "1")) { *out = true; return true; }
    if (!strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "no") || !strcmp(v, "0")) { *out = false; return true; }
    return false;
}

// A whole integer, clamped to lo..hi
static bool ParseInt(const char *v, int lo, int hi, int *out)
{
    char *end;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (end == v || *end || errno == EINVAL) return false;
    *out = n < lo ? lo : n > hi ? hi : (int)n;      // (a number past long's range: LONG_MIN / LONG_MAX, clamped)
    return true;
}

// A finite number, clamped to lo..hi (strtof takes "nan" / "inf": refused, as GfxLoad does)
static bool ParseFloat(const char *v, float lo, float hi, float *out)
{
    char *end;
    errno = 0;
    float f = strtof(v, &end);
    if (end == v || *end || !isfinite(f)) return false;
    *out = f < lo ? lo : f > hi ? hi : f;
    return true;
}

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "#RRGGBB" or "#RRGGBBAA" (the '#' optional) in s[0 .. n)
static bool ParseColorN(const char *s, size_t n, Color *out)
{
    while (n > 0 && isspace((unsigned char)*s)) { s++; n--; }
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    if (n > 0 && *s == '#') { s++; n--; }
    if (n != 6 && n != 8) return false;
    unsigned char ch[4] = { 0, 0, 0, 255 };
    for (size_t k = 0; k < n / 2; k++) {
        int hi = HexDigit(s[2 * k]), lo = HexDigit(s[2 * k + 1]);
        if (hi < 0 || lo < 0) return false;
        ch[k] = (unsigned char)(hi * 16 + lo);
    }
    *out = (Color){ ch[0], ch[1], ch[2], ch[3] };
    return true;
}

// A comma-separated colour list into a heap array (*out, *count, *cap: the caller's, filled only on
// success); at most `max` kept, the rest ignored (*cut: how many). An empty value is no colours
static bool ParseColorList(const char *v, int max, Color **out, int *count, int *cap, int *cut)
{
    Color *list = NULL;
    int n = 0, c = 0;
    *cut = 0;
    while (isspace((unsigned char)*v)) v++;
    if (*v) {
        const char *s = v;
        for (;;) {
            const char *e = strchr(s, ',');
            size_t len = e ? (size_t)(e - s) : strlen(s);
            Color col;
            if (!ParseColorN(s, len, &col)) { free(list); return false; }
            if (n < max) {
                Color *slot = MS_PUSH(list, n, c);
                if (!slot) { free(list); return false; }
                *slot = col;
            } else (*cut)++;
            if (!e) break;
            s = e + 1;
        }
    }
    free(*out);
    *out = list;
    *count = n;
    *cap = c;
    return true;
}

// key, v: trimmed. False on an unknown key or a bad value (s unchanged then); a list longer than its
// bound keeps its first colours and says so
static bool SetKeyValue(DrawState *s, const char *key, const char *v)
{
    int i;
    bool b;
    float f;
    Color col;
    if (strcmp(key, "tool") == 0) {
        if ((i = DrawToolByScriptName(v)) < 0) return false;
        s->tool = i;
    } else if (strcmp(key, "brush") == 0) {
        if ((i = NameIndex(v, BRUSHES, 3)) < 0) return false;
        s->shape = i;
    } else if (strcmp(key, "size") == 0) {
        if (!ParseInt(v, 1, SIZE_MAX_TEXELS, &i)) return false;
        s->size = i;
    } else if (strcmp(key, "hardness") == 0) {
        if (!ParseFloat(v, 0.0f, 1.0f, &f)) return false;
        s->hardness = f;
    } else if (strcmp(key, "pixel_perfect") == 0) {
        if (!ParseOnOff(v, &b)) return false;
        s->pixelPerfect = b;
    } else if (strcmp(key, "fg") == 0 || strcmp(key, "bg") == 0) {
        if (!ParseColorN(v, strlen(v), &col)) return false;
        if (key[0] == 'f') s->fg = col;
        else s->bg = col;
    } else if (strcmp(key, "recent") == 0) {
        Color *list = NULL;
        int n = 0, cap = 0, cut = 0;
        if (!ParseColorList(v, DRAWSTATE_RECENT, &list, &n, &cap, &cut)) return false;
        memset(s->recent, 0, sizeof(s->recent));
        if (n > 0) memcpy(s->recent, list, sizeof(Color) * (size_t)n);
        s->recentCount = n;
        free(list);
        if (cut > 0) TraceLog(LOG_WARNING, "monolith: draw state: recent: %d colour%s past the first %d ignored", cut, cut == 1 ? "" : "s", DRAWSTATE_RECENT);
    } else if (strcmp(key, "palette") == 0) {
        int cut = 0;
        if (!ParseColorList(v, DRAWSTATE_PALETTE_MAX, &s->palette, &s->paletteCount, &s->paletteCap, &cut)) return false;
        if (cut > 0) TraceLog(LOG_WARNING, "monolith: draw state: palette: %d colour%s past the first %d ignored", cut, cut == 1 ? "" : "s", DRAWSTATE_PALETTE_MAX);
    } else if (strcmp(key, "spray_width") == 0 || strcmp(key, "spray_speed") == 0) {
        if (!ParseInt(v, 1, SPRAY_MAX, &i)) return false;
        if (key[6] == 'w') s->sprayWidth = i;
        else s->spraySpeed = i;
    } else if (strcmp(key, "bucket") == 0) {
        if ((i = NameIndex(v, BUCKETS, 3)) < 0) return false;
        s->bucket = (DrawBucketMode)i;
    } else if (strcmp(key, "tolerance") == 0) {
        if (!ParseInt(v, 0, 255, &i)) return false;
        s->tolerance = i;
    } else if (strcmp(key, "gradient") == 0) {
        if (strcmp(v, "linear") == 0) s->gradientRadial = false;
        else if (strcmp(v, "radial") == 0) s->gradientRadial = true;
        else return false;
    } else if (strcmp(key, "pick_alt") == 0 || strcmp(key, "pick_tool") == 0) {
        if ((i = NameIndex(v, PICKS, 2)) < 0) return false;
        if (key[5] == 'a') s->pickAlt = (DrawPickMode)i;
        else s->pickTool = (DrawPickMode)i;
    } else if (strcmp(key, "density") == 0) {
        if (!ParseFloat(v, 8.0f, 512.0f, &f)) return false;
        s->density = f;
    } else if (strcmp(key, "pixel_look") == 0) {
        if (!ParseOnOff(v, &b)) return false;
        s->pixelLook = b;
    } else if (strcmp(key, "panel") == 0) {
        if (strcmp(v, "shown") == 0) s->panelHidden = false;
        else if (strcmp(v, "hidden") == 0) s->panelHidden = true;
        else return false;
    } else return false;
    return true;
}

static char *Trim(char *t)
{
    while (*t && isspace((unsigned char)*t)) t++;
    char *e = t + strlen(t);
    while (e > t && isspace((unsigned char)e[-1])) *--e = 0;
    return t;
}

bool DrawStateSetKey(DrawState *s, const char *keyValue)
{
    char *buf = MS_StrDup(keyValue);
    char *eq = buf ? strchr(buf, '=') : NULL;
    bool ok = false;
    if (eq) {
        *eq = 0;
        ok = SetKeyValue(s, Trim(buf), Trim(eq + 1));
    }
    free(buf);
    return ok;
}

// #endregion

// #region The file

// The next line of f without its '\n' (any length: a palette line is long) in *buf; false at the end
static bool ReadLine(FILE *f, char **buf, int *cap, bool *oom)
{
    int len = 0, c;
    while ((c = fgetc(f)) != EOF && c != '\n') {
        if (!MS_RESERVE(*buf, *cap, len + 2)) { *oom = true; return false; }
        (*buf)[len++] = (char)c;
    }
    if (c == EOF && len == 0) return false;
    if (!MS_RESERVE(*buf, *cap, len + 1)) { *oom = true; return false; }
    (*buf)[len] = 0;
    return true;
}

bool DrawStateLoad(DrawState *s, const char *path, int *ignored)
{
    *ignored = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return true;       // (no file yet: the defaults, silently)
        TraceLog(LOG_WARNING, "monolith: draw state %s: %s", path, strerror(errno));
        return false;
    }
    char *line = NULL;
    int cap = 0, n = 0, bad = 0;
    bool oom = false;
    while (ReadLine(f, &line, &cap, &oom)) {
        n++;
        if (line[0] && line[strlen(line) - 1] == '\r') line[strlen(line) - 1] = 0;     // (CRLF)
        char *t = Trim(line);
        if (!*t || *t == '#') continue;
        char *eq = strchr(t, '=');
        if (!eq) {
            TraceLog(LOG_WARNING, "monolith: %s:%d: expected key = value (ignored)", path, n);
            bad++;
            continue;
        }
        *eq = 0;
        char *key = Trim(t), *val = Trim(eq + 1);
        if (!SetKeyValue(s, key, val)) {
            TraceLog(LOG_WARNING, "monolith: %s:%d: unknown key or bad value '%s = %s' (ignored)", path, n, key, val);
            bad++;
        }
    }
    free(line);
    bool ok = !oom && !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) TraceLog(LOG_WARNING, "monolith: draw state %s: could not be read whole%s", path, oom ? " (out of memory)" : "");
    *ignored = bad;
    return ok;
}

// A float as the shortest of %.6g / %.9g that reads back to the same value (a round trip is exact)
static void PutFloat(FILE *f, const char *key, float v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6g", (double)v);
    if (strtof(buf, NULL) != v) snprintf(buf, sizeof(buf), "%.9g", (double)v);
    fprintf(f, "%s = %s\n", key, buf);
}

static void PutColor(FILE *f, Color c)
{
    if (c.a == 255) fprintf(f, "#%02X%02X%02X", c.r, c.g, c.b);
    else fprintf(f, "#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
}

static void PutColorList(FILE *f, const char *key, const Color *list, int n)
{
    fprintf(f, "%s = ", key);
    for (int i = 0; i < n; i++) {
        if (i > 0) fputc(',', f);
        PutColor(f, list[i]);
    }
    fputc('\n', f);
}

bool DrawStateSave(const DrawState *s, const char *path)
{
    size_t n = strlen(path);
    char *tmp = malloc(n + 5);
    if (!tmp) {
        TraceLog(LOG_WARNING, "monolith: draw state %s: not written (out of memory)", path);
        return false;
    }
    memcpy(tmp, path, n);
    memcpy(tmp + n, ".tmp", 5);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        TraceLog(LOG_WARNING, "monolith: draw state %s: not written: %s", tmp, strerror(errno));
        free(tmp);
        return false;
    }
    int tool = s->tool >= 0 && s->tool < TOOL_COUNT ? s->tool : TOOL_PENCIL;
    fprintf(f, "# monolith draw mode: tool, brush, colours and options of this map (docs/painting.md 18.6).\n"
               "# Written by the editor; not part of the map (not exported, not in its look or hash).\n");
    fprintf(f, "tool = %s\n", DrawToolScriptName((DrawTool)tool));
    fprintf(f, "brush = %s\n", BRUSHES[s->shape >= 0 && s->shape < 3 ? s->shape : 0]);
    fprintf(f, "size = %d\n", s->size);
    PutFloat(f, "hardness", s->hardness);
    fprintf(f, "pixel_perfect = %s\n", s->pixelPerfect ? "on" : "off");
    fprintf(f, "fg = ");
    PutColor(f, s->fg);
    fprintf(f, "\nbg = ");
    PutColor(f, s->bg);
    fputc('\n', f);
    PutColorList(f, "recent", s->recent, ClampI(s->recentCount, 0, DRAWSTATE_RECENT));
    PutColorList(f, "palette", s->palette, s->paletteCount > 0 ? s->paletteCount : 0);
    fprintf(f, "spray_width = %d\n", s->sprayWidth);
    fprintf(f, "spray_speed = %d\n", s->spraySpeed);
    fprintf(f, "bucket = %s\n", BUCKETS[s->bucket >= DRAW_BUCKET_CONTIGUOUS && s->bucket <= DRAW_BUCKET_REGION ? s->bucket : 0]);
    fprintf(f, "tolerance = %d\n", s->tolerance);
    fprintf(f, "gradient = %s\n", s->gradientRadial ? "radial" : "linear");
    fprintf(f, "pick_alt = %s\n", PICKS[s->pickAlt == DRAW_PICK_SCREEN]);
    fprintf(f, "pick_tool = %s\n", PICKS[s->pickTool == DRAW_PICK_SCREEN]);
    PutFloat(f, "density", s->density);
    fprintf(f, "pixel_look = %s\n", s->pixelLook ? "on" : "off");
    fprintf(f, "panel = %s\n", s->panelHidden ? "hidden" : "shown");
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
#ifdef _WIN32
    if (ok) remove(path);       // (Windows' rename doesn't replace a file)
#endif
    if (ok && rename(tmp, path) != 0) ok = false;
    if (!ok) {
        TraceLog(LOG_WARNING, "monolith: draw state %s: not written: %s", path, strerror(errno));
        remove(tmp);
    }
    free(tmp);
    return ok;
}

// #endregion
