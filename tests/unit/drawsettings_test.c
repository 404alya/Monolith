// Unit check of the draw mode's kept state (src/drawsettings.c, docs/painting.md 18.6): the defaults
// (DrawModeInit's), a round trip of every key through a file, each key on its own, bad values (one
// warning each, that key keeps its value, the rest loads), an unknown key, a missing file, a long
// palette (heap) and the bound on it, the clamping of numbers, the atomic write (no .tmp left) and
// an unwritable path. Runs under ASan + UBSan (leaks too) through ./build.sh test drawsettings.
//   drawsettings_test <scratch folder>
#include "drawsettings.h"
#include "drawkeys.h"
#include "monolith_scene.h"     // MS_PUSH

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails;
static int warnings;        // TraceLog warnings since the last ResetWarnings

static void Expect(const char *what, bool ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

static void Log(int level, const char *text, va_list args)
{
    if (level == LOG_WARNING) {
        warnings++;
        printf("     (warning) ");
        vprintf(text, args);
        printf("\n");
    }
}

static bool SameColor(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

static bool WriteFile(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(text, f);
    return fclose(f) == 0;
}

static bool Exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

// s = defaults, then the file's text loaded; *ignored, the warnings
static bool LoadText(DrawState *s, const char *path, const char *text, int *ignored)
{
    DrawStateFree(s);
    DrawStateDefaults(s);
    if (!WriteFile(path, text)) { printf("cannot write %s\n", path); exit(1); }
    warnings = 0;
    return DrawStateLoad(s, path, ignored);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: drawsettings_test <scratch folder>\n"); return 2; }
    SetTraceLogCallback(Log);
    const char *dir = argv[1];
    char path[2048], missing[2048], tmp[2100];
    snprintf(path, sizeof(path), "%s/draw.ini", dir);
    snprintf(missing, sizeof(missing), "%s/no-such-file.ini", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    remove(path);

    // Defaults: DrawModeInit's (src/drawmode.c)
    DrawState d;
    DrawStateDefaults(&d);
    Expect("defaults: pencil, round, size 4, hardness 0.5",
           d.tool == TOOL_PENCIL && d.shape == 0 && d.size == 4 && d.hardness == 0.5f && !d.pixelPerfect);
    Expect("defaults: fg #C83232FF, bg #FFFFFFFF",
           SameColor(d.fg, (Color){ 200, 50, 50, 255 }) && SameColor(d.bg, (Color){ 255, 255, 255, 255 }));
    Expect("defaults: spray 16 / 32, bucket contiguous, tolerance 0, linear",
           d.sprayWidth == 16 && d.spraySpeed == 32 && d.bucket == DRAW_BUCKET_CONTIGUOUS && d.tolerance == 0 && !d.gradientRadial);
    Expect("defaults: pick_alt paint, pick_tool screen, density 64, no pixel look, panel shown",
           d.pickAlt == DRAW_PICK_PAINT && d.pickTool == DRAW_PICK_SCREEN && d.density == 64.0f && !d.pixelLook && !d.panelHidden);
    Expect("defaults: no recent colours, no palette", d.recentCount == 0 && d.paletteCount == 0 && d.palette == NULL);

    // Path: with and without the trailing '/'
    char *p1 = DrawStatePath("maps/x/"), *p2 = DrawStatePath("maps/x");
    Expect("DrawStatePath with and without '/'", p1 && p2 && strcmp(p1, "maps/x/draw.ini") == 0 && strcmp(p2, "maps/x/draw.ini") == 0);
    free(p1);
    free(p2);

    // Missing file: true, s untouched, nothing ignored, no warning
    int ignored = -1;
    DrawState m;
    DrawStateDefaults(&m);
    m.size = 77;
    warnings = 0;
    Expect("missing file: true, untouched, silent", DrawStateLoad(&m, missing, &ignored) && ignored == 0 && m.size == 77 && warnings == 0);

    // Round trip: every key off its default
    DrawState a;
    DrawStateDefaults(&a);
    a.tool = TOOL_ELLIPSE_FILL;
    a.shape = 2;
    a.size = 37;
    a.hardness = 0.37f;
    a.pixelPerfect = true;
    a.fg = (Color){ 1, 2, 3, 4 };
    a.bg = (Color){ 250, 128, 0, 255 };
    a.recentCount = 3;
    a.recent[0] = (Color){ 10, 20, 30, 255 };
    a.recent[1] = (Color){ 40, 50, 60, 70 };
    a.recent[2] = (Color){ 0, 0, 0, 0 };
    for (int i = 0; i < 300; i++) {     // a long palette (heap; one line of the file)
        Color *c = MS_PUSH(a.palette, a.paletteCount, a.paletteCap);
        if (!c) { printf("out of memory\n"); return 1; }
        *c = (Color){ (unsigned char)i, (unsigned char)(i * 7), (unsigned char)(255 - i), (unsigned char)(i % 3 ? 255 : 128) };
    }
    a.sprayWidth = 99;
    a.spraySpeed = 3;
    a.bucket = DRAW_BUCKET_REGION;
    a.tolerance = 42;
    a.gradientRadial = true;
    a.pickAlt = DRAW_PICK_SCREEN;
    a.pickTool = DRAW_PICK_PAINT;
    a.density = 123.456f;
    a.pixelLook = true;
    a.panelHidden = true;
    Expect("save", DrawStateSave(&a, path));
    Expect("save: no .tmp left", !Exists(tmp));
    DrawState b;
    DrawStateDefaults(&b);
    warnings = 0;
    Expect("load what was saved: no line ignored", DrawStateLoad(&b, path, &ignored) && ignored == 0 && warnings == 0);
    Expect("round trip: equal (300-colour palette too)", DrawStateEqual(&a, &b) && b.paletteCount == 300);
    Expect("round trip: saved again over the file", DrawStateSave(&b, path) && !Exists(tmp));

    // Copy (deep) and Equal
    DrawState c;
    DrawStateDefaults(&c);
    Expect("copy: equal, own palette", DrawStateCopy(&c, &a) && DrawStateEqual(&c, &a) && c.palette != a.palette);
    c.palette[5].r ^= 1;
    Expect("equal: a palette colour differs", !DrawStateEqual(&c, &a));
    DrawStateFree(&c);
    Expect("free: palette gone", c.palette == NULL && c.paletteCount == 0);

    // Every key on its own (each line alone, from the defaults)
    static const struct { const char *line; } KEYS[] = {
        { "tool = rectfill" }, { "brush = square" }, { "size = 12" }, { "hardness = 0.25" }, { "pixel_perfect = on" },
        { "fg = #102030" }, { "bg = #10203040" }, { "recent = #FF0000, #00FF00FF" }, { "palette = #000000,#FFFFFF,#123456" },
        { "spray_width = 5" }, { "spray_speed = 200" }, { "bucket = global" }, { "tolerance = 9" }, { "gradient = radial" },
        { "pick_alt = screen" }, { "pick_tool = paint" }, { "density = 32" }, { "pixel_look = on" }, { "panel = hidden" },
    };
    for (size_t k = 0; k < sizeof(KEYS) / sizeof(KEYS[0]); k++) {
        DrawState s;
        DrawStateDefaults(&s);
        bool ok = DrawStateSetKey(&s, KEYS[k].line);
        DrawState def;
        DrawStateDefaults(&def);
        char what[160];
        snprintf(what, sizeof(what), "key: %s (changes the state)", KEYS[k].line);
        Expect(what, ok && !DrawStateEqual(&s, &def));
        DrawStateFree(&s);
    }
    {
        DrawState s;
        DrawStateDefaults(&s);
        DrawStateSetKey(&s, "tool=rectfill");
        DrawStateSetKey(&s, "brush=square");
        DrawStateSetKey(&s, "bg = #10203040");
        DrawStateSetKey(&s, "recent = #FF0000, #00FF00FF");
        DrawStateSetKey(&s, "palette = #000000,#FFFFFF,#123456");
        DrawStateSetKey(&s, "pick_tool = paint");
        Expect("key values: tool, brush, bg alpha, recent, palette, pick_tool",
               s.tool == TOOL_RECT_FILL && s.shape == 1 && SameColor(s.bg, (Color){ 16, 32, 48, 64 }) && s.recentCount == 2 &&
               SameColor(s.recent[1], (Color){ 0, 255, 0, 255 }) && s.paletteCount == 3 &&
               SameColor(s.palette[2], (Color){ 0x12, 0x34, 0x56, 255 }) && s.pickTool == DRAW_PICK_PAINT);
        Expect("palette = (empty): no colours", DrawStateSetKey(&s, "palette =") && s.paletteCount == 0);
        DrawStateFree(&s);
    }

    // Bad values and an unknown key: one warning each, that key keeps its value, the rest loads
    DrawState s;
    DrawStateDefaults(&s);
    bool ok = LoadText(&s, path,
                       "# a comment\n"
                       "\n"
                       "size = 9\n"
                       "tool = zoom\n"                  // bad: no such tool
                       "brush = triangle\n"             // bad
                       "hardness = nan\n"               // bad: not finite
                       "fg = #12345\n"                  // bad: 5 digits
                       "bg = red\n"                     // bad
                       "palette = #000000, nope\n"      // bad: the whole list is refused
                       "tolerance = 3.5\n"              // bad: not a whole number
                       "pixel_look = maybe\n"           // bad
                       "panel = gone\n"                 // bad
                       "colour = #FFFFFF\n"             // unknown key
                       "just words\n"                   // no '='
                       "spray_width = 7\n"
                       "  pick_alt   =   screen  \r\n", // spaces and CRLF
                       &ignored);
    Expect("bad lines: load still true", ok);
    Expect("bad lines: 11 ignored, 11 warnings", ignored == 11 && warnings == 11);
    Expect("bad lines: the good keys loaded", s.size == 9 && s.sprayWidth == 7 && s.pickAlt == DRAW_PICK_SCREEN);
    Expect("bad lines: the bad keys kept their defaults",
           s.tool == TOOL_PENCIL && s.shape == 0 && s.hardness == 0.5f && SameColor(s.fg, d.fg) && SameColor(s.bg, d.bg) &&
           s.paletteCount == 0 && s.tolerance == 0 && !s.pixelLook && !s.panelHidden);
    Expect("SetKey: unknown key false", !DrawStateSetKey(&s, "zoom = 2"));
    Expect("SetKey: no '=' false", !DrawStateSetKey(&s, "size"));
    {
        DrawState before;
        DrawStateDefaults(&before);
        DrawStateCopy(&before, &s);
        Expect("SetKey: a bad value leaves s unchanged", !DrawStateSetKey(&s, "recent = #FFFFFF,#GG0000") && DrawStateEqual(&s, &before));
        DrawStateFree(&before);
    }

    // Clamping: numbers out of range are clamped (GfxLoad's rule), not refused
    ok = LoadText(&s, path,
                  "size = 0\nhardness = 1.5\nspray_width = 1000\nspray_speed = -4\ntolerance = 300\ndensity = 4\n", &ignored);
    Expect("clamp: low / high ends, nothing ignored", ok && ignored == 0 && warnings == 0);
    Expect("clamp: size 1, hardness 1, spray 256 / 1, tolerance 255, density 8",
           s.size == 1 && s.hardness == 1.0f && s.sprayWidth == 256 && s.spraySpeed == 1 && s.tolerance == 255 && s.density == 8.0f);
    ok = LoadText(&s, path, "size = 99999999999999999999\ndensity = 1e9\n", &ignored);
    Expect("clamp: a number past long's range, a huge float", ok && ignored == 0 && s.size == 256 && s.density == 512.0f);

    // Recent: more than 8 keeps the first 8 (one warning, not ignored)
    ok = LoadText(&s, path, "recent = #010101,#020202,#030303,#040404,#050505,#060606,#070707,#080808,#090909,#0A0A0A\n", &ignored);
    Expect("recent: 10 given, the first 8 kept, warned", ok && ignored == 0 && warnings == 1 && s.recentCount == DRAWSTATE_RECENT &&
           SameColor(s.recent[7], (Color){ 8, 8, 8, 255 }));

    // Palette past DRAWSTATE_PALETTE_MAX: the rest ignored, warned
    {
        int n = DRAWSTATE_PALETTE_MAX + 10;
        size_t len = 16 + (size_t)n * 8;
        char *text = malloc(len);
        if (!text) return 1;
        size_t at = (size_t)snprintf(text, len, "palette = ");
        for (int i = 0; i < n; i++) at += (size_t)snprintf(text + at, len - at, "%s#%06X", i ? "," : "", i);
        snprintf(text + at, len - at, "\n");
        ok = LoadText(&s, path, text, &ignored);
        free(text);
        Expect("palette: 4106 given, 4096 kept, warned", ok && ignored == 0 && warnings == 1 && s.paletteCount == DRAWSTATE_PALETTE_MAX &&
               SameColor(s.palette[DRAWSTATE_PALETTE_MAX - 1], (Color){ 0x00, 0x0F, 0xFF, 255 }));
    }

    // Every tool's script name reads back
    bool tools = true;
    for (int t = 0; t < TOOL_COUNT; t++) {
        char line[64];
        snprintf(line, sizeof(line), "tool = %s", DrawToolScriptName((DrawTool)t));
        DrawState one;
        DrawStateDefaults(&one);
        tools = tools && DrawStateSetKey(&one, line) && one.tool == t;
    }
    Expect("tool: every DrawTool's script name", tools);

    // An unwritable path: false (logged), nothing left behind
    char bad[2100];
    snprintf(bad, sizeof(bad), "%s/no-such-folder/draw.ini", dir);
    warnings = 0;
    Expect("save into a missing folder: false, warned", !DrawStateSave(&a, bad) && warnings >= 1);

    DrawStateFree(&s);
    DrawStateFree(&a);
    DrawStateFree(&b);
    DrawStateFree(&m);
    DrawStateFree(&d);
    remove(path);
    printf("%s: %d failed\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}
