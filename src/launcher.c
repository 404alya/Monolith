// Launcher: request bundles, Claude Code in a new terminal, polling for result.md
//
// A submitted request becomes a self-contained folder requests/<YYYY-MM-DD_HHMMSS>_<slug>/
// (view.png, view-marked.png, request.md, request.json). Then a new terminal window runs
// `claude [MONOLITH_CLAUDE_ARGS] "Map edit request: read requests/<id>/request.md and
// carry it out."` in the repo root. The user's prompt never goes on a command line: it
// only lives in the bundle, and the process is started from an argv array (no shell).
// The game then polls the bundle for result.md and shows its first line in the HUD.
//
// Where things go:
//   - requests/ and the working directory of Claude Code: the repo root baked in by
//     build.sh (PROJECT_PATH) while it exists, i.e. on the dev machine; otherwise the
//     directory of the executable (a copied build/linux or build/win folder).
//   - terminal: $MONOLITH_TERMINAL, else $TERMINAL, else the first of alacritty, kitty,
//     foot, wezterm, xterm found in PATH. The variables may hold a program name or path
//     plus extra arguments ("kitty --single-instance"); the program's basename picks
//     the syntax from TERMINALS below, unknown terminals get `-e <command>`.
//   - claude: searched in PATH, then ~/.local/bin/claude (where the native installer puts
//     it, which a game started from a desktop launcher may not have in PATH). It is
//     passed to the terminal as an absolute path, so a terminal daemon with another
//     environment (kitty --single-instance) still finds it.
//   - extra Claude Code flags: $MONOLITH_CLAUDE_ARGS, split on whitespace (no quoting).
//     Default none; permissions are never bypassed by default.
//
// Windows: the same bundle, then `cmd /c start "monolith-claude: <slug>" /D <root> cmd /k
// "<claude>" ...` via _spawnvp (<process.h>, so no <windows.h> clash with raylib). Only
// compile-tested; failures of `start` itself can't be seen from here.

#ifndef _WIN32
#   define _XOPEN_SOURCE 700           // POSIX 2008 + XSI: fork, setsid, readlink, realpath
#endif

#include "launcher.h"
#include "map.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#   include <direct.h>     // _mkdir
#   include <io.h>         // _access
#   include <process.h>    // _spawnvp
#   include <stdint.h>
// Declared by hand like the GL functions in main.c (<windows.h> clashes with raylib).
// x64 has one calling convention, so this matches kernel32.dll.
extern int CloseHandle(void *handle);
#else
#   include <fcntl.h>
#   include <sys/stat.h>
#   include <sys/types.h>
#   include <sys/wait.h>
#   include <unistd.h>
extern char **environ;
#endif

#ifndef PROJECT_PATH
#   define PROJECT_PATH ""
#endif

#define MAX_OPEN_REQUESTS   16
#define POLL_INTERVAL       0.5f    // seconds between result.md checks
#define DONE_SHOW_TIME      20.0f   // finished requests stay in the HUD this long
#define DONE_FADE_TIME      3.0f    // ... fading out over the last seconds
#define NEARBY_RADIUS       30.0f   // "nearby context" radius around the hit point, metres
#define SLUG_MAX            40
#define CLAUDE_ARGS_MAX     24

// ======================================================================================
// #region Paths and small helpers
// ======================================================================================

static char rootDir[1024];          // repo root (or exe dir), trailing slash
static char requestsDir[1100];      // rootDir + "requests/"

static void InitPaths(void)
{
    if (requestsDir[0]) return;
#ifdef _WIN32
    snprintf(rootDir, sizeof(rootDir), "%s", GetApplicationDirectory());
#else
    if (PROJECT_PATH[0] && DirectoryExists(PROJECT_PATH)) snprintf(rootDir, sizeof(rootDir), "%s", PROJECT_PATH);
    else snprintf(rootDir, sizeof(rootDir), "%s", GetApplicationDirectory());
#endif
    size_t n = strlen(rootDir);
    if (n == 0) snprintf(rootDir, sizeof(rootDir), "./");
    else if (rootDir[n - 1] != '/' && rootDir[n - 1] != '\\' && n + 1 < sizeof(rootDir)) strcat(rootDir, "/");
    snprintf(requestsDir, sizeof(requestsDir), "%srequests/", rootDir);
}

// Absolute path of a file (falls back to the input if it can't be resolved)
static void AbsPath(const char *in, char *out, int outSize)
{
#ifdef _WIN32
    if (!_fullpath(out, in, outSize)) snprintf(out, outSize, "%s", in);
#else
    char buf[4096];
    if (realpath(in, buf)) snprintf(out, outSize, "%s", buf);
    else snprintf(out, outSize, "%s", in);
#endif
}

static void BinaryPath(char *out, int outSize)
{
#ifdef _WIN32
    snprintf(out, outSize, "%smonolith.exe", GetApplicationDirectory());
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = 0; snprintf(out, outSize, "%s", buf); }
    else snprintf(out, outSize, "%smonolith", GetApplicationDirectory());
#endif
}

static int MakeDir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

// Splits s on whitespace into up to max tokens, stored in buf (modified copy of s)
static int SplitWords(const char *s, char *buf, int bufSize, char **tok, int max)
{
    snprintf(buf, bufSize, "%s", s ? s : "");
    int n = 0;
    char *p = buf;
    while (*p && n < max) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        tok[n++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = 0;
    }
    return n;
}

#ifndef _WIN32
static const char *BaseName(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}
#endif

// Lowercase ASCII words of the prompt joined by '-', cut at a word boundary
static void MakeSlug(const char *prompt, char *out, int outSize)
{
    char s[SLUG_MAX * 2 + 2];
    int n = 0;
    bool sep = false;
    for (const unsigned char *p = (const unsigned char *)prompt; *p && n < (int)sizeof(s) - 2; p++) {
        if (*p < 128 && isalnum(*p)) {
            if (sep && n > 0) s[n++] = '-';
            s[n++] = (char)tolower(*p);
            sep = false;
        } else sep = true;     // punctuation, spaces and non-ASCII bytes separate words
    }
    s[n] = 0;
    if (n > SLUG_MAX) {
        int cut = SLUG_MAX;
        while (cut > 0 && s[cut] != '-') cut--;
        s[cut > 0 ? cut : SLUG_MAX] = 0;
    }
    snprintf(out, outSize, "%s", s[0] ? s : "request");
}

// A path as one shell word: as is when it only has safe characters, else in single
// quotes (' becomes '\''), so the copy-paste commands in request.md survive spaces
static void ShellQuote(const char *in, char *out, int outSize)
{
    bool safe = in[0] != 0;
    for (const char *p = in; *p && safe; p++) safe = isalnum((unsigned char)*p) || strchr("/._-+:,@%=", *p);
    if (safe) { snprintf(out, outSize, "%s", in); return; }
    int n = 0;
    if (n < outSize - 1) out[n++] = '\'';
    for (const char *p = in; *p && n < outSize - 5; p++) {
        if (*p == '\'') { memcpy(out + n, "'\\''", 4); n += 4; }
        else out[n++] = *p;
    }
    if (n < outSize - 1) out[n++] = '\'';
    out[n] = 0;
}

static float Deg(float rad) { return rad * RAD2DEG; }

static float DistToBox(Vector3 p, BoundingBox b)
{
    float dx = fmaxf(fmaxf(b.min.x - p.x, 0.0f), p.x - b.max.x);
    float dy = fmaxf(fmaxf(b.min.y - p.y, 0.0f), p.y - b.max.y);
    float dz = fmaxf(fmaxf(b.min.z - p.z, 0.0f), p.z - b.max.z);
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static const char *GroupName(int g)
{
    return (g >= 0 && g < world.groupCount) ? world.groups[g] : "(no group)";
}

// #endregion

// ======================================================================================
// #region Bundle contents (computed once, written as Markdown and JSON)
// ======================================================================================

// Selected boxes grouped by source line (a `repeat` line can make dozens of boxes; its
// text is quoted once). Groups keep the order in which their first box was selected.
typedef struct {
    int line;
    int first;          // index into sel[] of the first box of this line
    int count;
} SelLine;

typedef struct {
    int group;          // -1 = boxes without a group
    float dist;         // nearest box of the group
    int nearCount;      // boxes of the group within NEARBY_RADIUS
    int firstLine, lastLine;    // over all boxes of the group
} Nearby;

typedef struct {
    const DesignRequest *req;
    char id[128], slug[SLUG_MAX + 1], created[32];
    Vector3 fwd, right;
    float yawDeg, pitchDeg, aspect;
    bool hit;
    char hitLine[MAP_LINE_MAX + 1];
    bool hitHasLine;
    int sel[MAX_SELECTION];             // valid selected box indices
    int selCount;
    SelLine selLines[MAX_SELECTION];
    int selLineCount;
    int selOrder[MAX_SELECTION];        // sel[] reordered so each line's boxes are adjacent
    Nearby nearby[MAX_GROUPS + 1];
    int nearbyCount;
    Vector3 nearbyCenter;
    char binary[1024], mapPath[1024], prompt[256];
    char binaryQ[2100], mapPathQ[2100];     // shell-quoted
    char shotCmd[6144];
    bool stale;                         // world changed between the snapshot and now
} Bundle;

static int CompareNearby(const void *a, const void *b)
{
    float d = ((const Nearby *)a)->dist - ((const Nearby *)b)->dist;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

static void BuildBundle(Bundle *B, const DesignRequest *req)
{
    B->req = req;
    B->stale = req->mapVersion != world.version;
    B->yawDeg = Wrap(Deg(req->yaw), 0.0f, 360.0f);
    B->pitchDeg = Deg(req->pitch);
    B->fwd = (Vector3){ cosf(req->pitch) * sinf(req->yaw), sinf(req->pitch), cosf(req->pitch) * cosf(req->yaw) };
    B->right = (Vector3){ -cosf(req->yaw), 0.0f, sinf(req->yaw) };
    B->aspect = req->height > 0 ? (float)req->width / req->height : 0.0f;

    B->hit = req->hit.box >= 0 && req->hit.box < world.count;
    if (B->hit) B->hitHasLine = MapGetLine(world.refs[req->hit.box].line, B->hitLine, sizeof(B->hitLine));

    // Selection: drop invalid indices, then group by line
    for (int i = 0; i < req->selectedCount && i < MAX_SELECTION; i++)
        if (req->selected[i] >= 0 && req->selected[i] < world.count) B->sel[B->selCount++] = req->selected[i];
    for (int i = 0; i < B->selCount; i++) {
        int line = world.refs[B->sel[i]].line, k = 0;
        while (k < B->selLineCount && B->selLines[k].line != line) k++;
        if (k == B->selLineCount) B->selLines[B->selLineCount++] = (SelLine){ line, 0, 0 };
        B->selLines[k].count++;
    }
    int at = 0;
    for (int k = 0; k < B->selLineCount; k++) {
        B->selLines[k].first = at;
        for (int i = 0; i < B->selCount; i++)
            if (world.refs[B->sel[i]].line == B->selLines[k].line) B->selOrder[at++] = B->sel[i];
    }

    // Nearby groups around the hit point (or the eye if the crosshair points at the sky)
    B->nearbyCenter = B->hit ? req->hit.point : req->eye;
    for (int g = 0; g <= world.groupCount; g++)
        B->nearby[g] = (Nearby){ g < world.groupCount ? g : -1, 1e30f, 0, 1 << 30, 0 };
    for (int i = 0; i < world.count; i++) {
        int g = world.refs[i].group;
        Nearby *n = &B->nearby[(g >= 0 && g < world.groupCount) ? g : world.groupCount];
        float d = DistToBox(B->nearbyCenter, world.items[i]);
        if (d < n->dist) n->dist = d;
        if (d <= NEARBY_RADIUS) n->nearCount++;
        int line = world.refs[i].line;
        if (line > 0 && line < n->firstLine) n->firstLine = line;
        if (line > n->lastLine) n->lastLine = line;
    }
    for (int g = 0; g <= world.groupCount; g++)
        if (B->nearby[g].nearCount > 0) B->nearby[B->nearbyCount++] = B->nearby[g];
    qsort(B->nearby, B->nearbyCount, sizeof(Nearby), CompareNearby);

    BinaryPath(B->binary, sizeof(B->binary));
    AbsPath(world.path, B->mapPath, sizeof(B->mapPath));
    ShellQuote(B->binary, B->binaryQ, sizeof(B->binaryQ));
    ShellQuote(B->mapPath, B->mapPathQ, sizeof(B->mapPathQ));
    snprintf(B->shotCmd, sizeof(B->shotCmd), "%s --shot requests/%s/before.png --shot-size %d %d --eye %.6f %.6f %.6f --look %.6f %.6f --map %s",
             B->binaryQ, B->id, req->width, req->height, req->eye.x, req->eye.y, req->eye.z, B->yawDeg, B->pitchDeg, B->mapPathQ);
    snprintf(B->prompt, sizeof(B->prompt), "Map edit request: read requests/%s/request.md and carry it out.", B->id);
}

// #endregion

// ======================================================================================
// #region request.md
// ======================================================================================

// Fenced code block that the text can't break out of: the fence is longer than the
// longest run of backticks in the text
static void MdFence(FILE *f, const char *text, const char *info)
{
    int longest = 0, run = 0;
    for (const char *p = text; *p; p++) {
        run = (*p == '`') ? run + 1 : 0;
        if (run > longest) longest = run;
    }
    int fence = longest >= 3 ? longest + 1 : 3;
    for (int i = 0; i < fence; i++) fputc('`', f);
    fprintf(f, "%s\n%s", info, text);
    if (!text[0] || text[strlen(text) - 1] != '\n') fputc('\n', f);
    for (int i = 0; i < fence; i++) fputc('`', f);
    fputs("\n\n", f);
}

static void MdVec(FILE *f, Vector3 v) { fprintf(f, "(%.3f, %.3f, %.3f)", v.x, v.y, v.z); }

static void MdLine(FILE *f, int line)
{
    char text[MAP_LINE_MAX + 1];
    if (line > 0 && MapGetLine(line, text, sizeof(text))) {
        fprintf(f, "Map line %d:\n\n", line);
        MdFence(f, text, "");
    } else fprintf(f, "Not from a map line.\n\n");
}

static bool WriteRequestMd(const char *path, const Bundle *B)
{
    const DesignRequest *r = B->req;
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    fprintf(f, "# Map edit request: %s\n\n", B->slug);
    fprintf(f, "Request `%s`, submitted %s from the monolith map designer.\n\n", B->id, B->created);
    fprintf(f, "**First** read `CLAUDE.md` and `AI-MAP-TOOL.md` in the repository root (if they are not "
               "loaded already); they explain the map format, the conventions and how to verify an edit. "
               "Then carry out the request below.\n\n"
               "**When you are done**, write `requests/%s/result.md`. Its **first line is a one-line "
               "summary** (the game shows it in its HUD), followed by what changed (map lines before -> "
               "after), the paths of your after-image(s), and anything left open. If the request is "
               "genuinely ambiguous, ask in the terminal instead of guessing.\n\n", B->id);
    if (B->stale)
        fprintf(f, "**Warning:** the map was reloaded between the selection and the submit; box indices "
                   "below may not match what the user saw.\n\n");

    fprintf(f, "## Prompt\n\nThe user's words, verbatim:\n\n");
    MdFence(f, r->prompt, "text");

    fprintf(f, "## Camera\n\n");
    fprintf(f, "- eye: "); MdVec(f, r->eye); fprintf(f, "\n");
    fprintf(f, "- yaw %.2f deg, pitch %.2f deg. Convention: yaw 0 looks toward +Z, yaw 180 toward -Z, "
               "yaw 90 toward +X; LookDir = (cos p * sin y, sin p, cos p * cos y)\n", B->yawDeg, B->pitchDeg);
    fprintf(f, "- forward: "); MdVec(f, B->fwd); fprintf(f, "\n");
    fprintf(f, "- right: "); MdVec(f, B->right); fprintf(f, " = (-cos yaw, 0, sin yaw); up is +Y\n");
    fprintf(f, "- fovy %.1f deg (vertical), aspect %.4f, resolution %d x %d\n\n", r->fovy, B->aspect, r->width, r->height);
    fprintf(f, "Images in this folder: `view.png` (the frame without HUD) and `view-marked.png` (the same "
               "frame with the selection highlighted and the crosshair hit marked).\n\n");

    fprintf(f, "## Crosshair hit\n\n");
    if (B->hit) {
        BoundingBox b = world.items[r->hit.box];
        BoxRef ref = world.refs[r->hit.box];
        fprintf(f, "- point: "); MdVec(f, r->hit.point); fprintf(f, "\n");
        fprintf(f, "- face normal: "); MdVec(f, r->hit.normal); fprintf(f, "\n");
        fprintf(f, "- distance from the eye: %.2f m\n", r->hit.dist);
        fprintf(f, "- box #%d: min ", r->hit.box); MdVec(f, b.min); fprintf(f, ", max "); MdVec(f, b.max); fprintf(f, "\n");
        fprintf(f, "- group: \"%s\"\n\n", GroupName(ref.group));
        MdLine(f, ref.line);
    } else {
        fprintf(f, "Nothing: the crosshair points at the sky (no box hit).\n\n");
    }

    fprintf(f, "## Selection (%d box%s)\n\n", B->selCount, B->selCount == 1 ? "" : "es");
    if (B->selCount == 0) fprintf(f, "Nothing is selected.\n\n");
    for (int k = 0; k < B->selLineCount; k++) {
        const SelLine *sl = &B->selLines[k];
        int first = B->selOrder[sl->first];
        fprintf(f, "### Line %d, group \"%s\", %d box%s\n\n", sl->line, GroupName(world.refs[first].group),
                sl->count, sl->count == 1 ? "" : "es");
        MdLine(f, sl->line);
        fprintf(f, "| box | min | max | centre | size |\n|---|---|---|---|---|\n");
        for (int i = sl->first; i < sl->first + sl->count; i++) {
            BoundingBox b = world.items[B->selOrder[i]];
            fprintf(f, "| #%d | ", B->selOrder[i]); MdVec(f, b.min);
            fprintf(f, " | "); MdVec(f, b.max);
            fprintf(f, " | "); MdVec(f, Vector3Scale(Vector3Add(b.min, b.max), 0.5f));
            fprintf(f, " | "); MdVec(f, Vector3Subtract(b.max, b.min));
            fprintf(f, " |\n");
        }
        fprintf(f, "\n");
    }

    fprintf(f, "## Nearby\n\nGroups with a box within %.0f m of the %s ", NEARBY_RADIUS, B->hit ? "hit point" : "eye");
    MdVec(f, B->nearbyCenter);
    fprintf(f, ", nearest first. The map lines are the range over the whole group.\n\n");
    fprintf(f, "| group | nearest box | boxes within %.0f m | map lines |\n|---|---|---|---|\n", NEARBY_RADIUS);
    for (int i = 0; i < B->nearbyCount; i++) {
        const Nearby *n = &B->nearby[i];
        fprintf(f, "| %s | %.1f m | %d | ", GroupName(n->group), n->dist, n->nearCount);
        if (n->lastLine > 0) fprintf(f, "%d-%d |\n", n->firstLine, n->lastLine);
        else fprintf(f, "- |\n");
    }
    if (B->nearbyCount == 0) fprintf(f, "| (none) | | | |\n");
    fprintf(f, "\n");

    fprintf(f, "## Reproduce this view\n\n");
    fprintf(f, "Renders this camera in a hidden window (never on the developer's screen) and saves the "
               "image into this folder; render `after.png` the same way after the edit and compare. "
               "`--shot-size` matches the size of `view.png`, so the framing is identical.\n\n");
    MdFence(f, B->shotCmd, "sh");

    fprintf(f, "## Map\n\n");
    fprintf(f, "- file: `%s`\n", B->mapPath);
    char when[32] = "?";
    time_t mt = (time_t)world.mtime;
    struct tm *tm = localtime(&mt);
    if (tm) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", tm);
    fprintf(f, "- at submit: FNV-1a 64 hash `%016llx`, mtime %ld (%s), load #%u, %d boxes, %d groups\n\n",
            world.hash, world.mtime, when, world.version, world.count, world.groupCount);
    fprintf(f, "Several requests can run at once. Before editing, check that the file still has this hash; "
               "if not, it changed since this request was submitted (maybe another request edited it): "
               "re-read it, the line numbers above may have moved.\n\n");
    char hashCmd[2400];     // not TextFormat: its buffer is only 1 KB
    snprintf(hashCmd, sizeof(hashCmd), "python3 -c \"import sys,functools as f;print('%%016x'%%f.reduce(lambda h,b:((h^b)"
             "*0x100000001b3)&(2**64-1),open(sys.argv[1],'rb').read(),0xcbf29ce484222325))\" %s", B->mapPathQ);
    MdFence(f, hashCmd, "sh");

    return fclose(f) == 0;
}

// #endregion

// ======================================================================================
// #region request.json
// ======================================================================================

// JSON string: escapes quotes, backslashes and control characters, passes valid UTF-8
// through and replaces invalid bytes with U+FFFD, so the file always parses
static void JsonStr(FILE *f, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    fputc('"', f);
    while (*p) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); p++; }
        else if (c == '\n') { fputs("\\n", f); p++; }
        else if (c == '\r') { fputs("\\r", f); p++; }
        else if (c == '\t') { fputs("\\t", f); p++; }
        else if (c < 0x20) { fprintf(f, "\\u%04x", c); p++; }
        else if (c < 0x80) { fputc(c, f); p++; }
        else {
            int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
            bool ok = len > 0 && !(c == 0xC0 || c == 0xC1 || c > 0xF4);
            for (int i = 1; ok && i < len; i++) ok = (p[i] & 0xC0) == 0x80;
            if (ok && len == 3 && c == 0xE0 && p[1] < 0xA0) ok = false;            // overlong
            if (ok && len == 3 && c == 0xED && p[1] >= 0xA0) ok = false;           // surrogates
            if (ok && len == 4 && c == 0xF0 && p[1] < 0x90) ok = false;            // overlong
            if (ok && len == 4 && c == 0xF4 && p[1] >= 0x90) ok = false;           // > U+10FFFF
            if (ok) { fwrite(p, 1, len, f); p += len; }
            else { fputs("\\ufffd", f); p++; }
        }
    }
    fputc('"', f);
}

static void JsonNum(FILE *f, float x)
{
    if (isfinite(x)) fprintf(f, "%.4f", x);
    else fputs("null", f);
}

static void JsonVec(FILE *f, Vector3 v)
{
    fputc('[', f); JsonNum(f, v.x); fputs(", ", f); JsonNum(f, v.y); fputs(", ", f); JsonNum(f, v.z); fputc(']', f);
}

static void JsonLineText(FILE *f, int line)
{
    char text[MAP_LINE_MAX + 1];
    if (line > 0 && MapGetLine(line, text, sizeof(text))) JsonStr(f, text);
    else fputs("null", f);
}

static bool WriteRequestJson(const char *path, const Bundle *B)
{
    const DesignRequest *r = B->req;
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    fprintf(f, "{\n  \"format\": 1,\n  \"id\": "); JsonStr(f, B->id);
    fprintf(f, ",\n  \"slug\": "); JsonStr(f, B->slug);
    fprintf(f, ",\n  \"created\": "); JsonStr(f, B->created);
    fprintf(f, ",\n  \"instructions\": ");
    JsonStr(f, TextFormat("Read CLAUDE.md and AI-MAP-TOOL.md in the repository root first, then carry out the "
                          "prompt. When done, write requests/%s/result.md; its first line is a one-line summary.", B->id));
    fprintf(f, ",\n  \"resultFile\": "); JsonStr(f, TextFormat("requests/%s/result.md", B->id));
    fprintf(f, ",\n  \"mapReloadedSinceSnapshot\": %s", B->stale ? "true" : "false");
    fprintf(f, ",\n  \"prompt\": "); JsonStr(f, r->prompt);

    fprintf(f, ",\n  \"camera\": {\n    \"eye\": "); JsonVec(f, r->eye);
    fprintf(f, ",\n    \"yawDeg\": "); JsonNum(f, B->yawDeg);
    fprintf(f, ",\n    \"pitchDeg\": "); JsonNum(f, B->pitchDeg);
    fprintf(f, ",\n    \"convention\": "); JsonStr(f, "yaw 0 looks toward +Z, yaw 180 toward -Z, yaw 90 toward +X; "
                                                   "LookDir = (cos p * sin y, sin p, cos p * cos y); right = (-cos yaw, 0, sin yaw); up = +Y");
    fprintf(f, ",\n    \"forward\": "); JsonVec(f, B->fwd);
    fprintf(f, ",\n    \"right\": "); JsonVec(f, B->right);
    fprintf(f, ",\n    \"fovyDeg\": "); JsonNum(f, r->fovy);
    fprintf(f, ",\n    \"aspect\": "); JsonNum(f, B->aspect);
    fprintf(f, ",\n    \"width\": %d,\n    \"height\": %d\n  }", r->width, r->height);
    fprintf(f, ",\n  \"images\": { \"view\": \"view.png\", \"marked\": \"view-marked.png\" }");

    fprintf(f, ",\n  \"hit\": ");
    if (B->hit) {
        BoundingBox b = world.items[r->hit.box];
        BoxRef ref = world.refs[r->hit.box];
        fprintf(f, "{\n    \"point\": "); JsonVec(f, r->hit.point);
        fprintf(f, ",\n    \"normal\": "); JsonVec(f, r->hit.normal);
        fprintf(f, ",\n    \"distance\": "); JsonNum(f, r->hit.dist);
        fprintf(f, ",\n    \"box\": %d,\n    \"min\": ", r->hit.box); JsonVec(f, b.min);
        fprintf(f, ",\n    \"max\": "); JsonVec(f, b.max);
        fprintf(f, ",\n    \"group\": "); JsonStr(f, GroupName(ref.group));
        fprintf(f, ",\n    \"line\": %d,\n    \"lineText\": ", ref.line); JsonLineText(f, ref.line);
        fprintf(f, "\n  }");
    } else fprintf(f, "null");

    fprintf(f, ",\n  \"selection\": [");
    for (int k = 0; k < B->selLineCount; k++) {
        const SelLine *sl = &B->selLines[k];
        fprintf(f, "%s\n    {\n      \"line\": %d,\n      \"lineText\": ", k ? "," : "", sl->line);
        JsonLineText(f, sl->line);
        fprintf(f, ",\n      \"group\": "); JsonStr(f, GroupName(world.refs[B->selOrder[sl->first]].group));
        fprintf(f, ",\n      \"boxes\": [");
        for (int i = sl->first; i < sl->first + sl->count; i++) {
            BoundingBox b = world.items[B->selOrder[i]];
            fprintf(f, "%s\n        { \"box\": %d, \"min\": ", i > sl->first ? "," : "", B->selOrder[i]);
            JsonVec(f, b.min);
            fprintf(f, ", \"max\": "); JsonVec(f, b.max);
            fprintf(f, ", \"center\": "); JsonVec(f, Vector3Scale(Vector3Add(b.min, b.max), 0.5f));
            fprintf(f, ", \"size\": "); JsonVec(f, Vector3Subtract(b.max, b.min));
            fprintf(f, " }");
        }
        fprintf(f, "\n      ]\n    }");
    }
    fprintf(f, "%s],\n  \"selectedCount\": %d", B->selLineCount ? "\n  " : "", B->selCount);

    fprintf(f, ",\n  \"nearby\": {\n    \"center\": "); JsonVec(f, B->nearbyCenter);
    fprintf(f, ",\n    \"centerIs\": \"%s\",\n    \"radius\": ", B->hit ? "hit" : "eye"); JsonNum(f, NEARBY_RADIUS);
    fprintf(f, ",\n    \"groups\": [");
    for (int i = 0; i < B->nearbyCount; i++) {
        const Nearby *n = &B->nearby[i];
        fprintf(f, "%s\n      { \"group\": ", i ? "," : ""); JsonStr(f, GroupName(n->group));
        fprintf(f, ", \"nearest\": "); JsonNum(f, n->dist);
        fprintf(f, ", \"boxesWithinRadius\": %d, ", n->nearCount);
        if (n->lastLine > 0) fprintf(f, "\"firstLine\": %d, \"lastLine\": %d }", n->firstLine, n->lastLine);
        else fprintf(f, "\"firstLine\": null, \"lastLine\": null }");
    }
    fprintf(f, "%s]\n  }", B->nearbyCount ? "\n    " : "");

    fprintf(f, ",\n  \"reproduce\": {\n    \"command\": "); JsonStr(f, B->shotCmd);
    fprintf(f, ",\n    \"argv\": ["); JsonStr(f, B->binary);
    fprintf(f, ", \"--shot\", \"requests/%s/before.png\", \"--shot-size\", \"%d\", \"%d\", \"--eye\", \"%.6f\", \"%.6f\", \"%.6f\", \"--look\", \"%.6f\", \"%.6f\", \"--map\", ",
            B->id, r->width, r->height, r->eye.x, r->eye.y, r->eye.z, B->yawDeg, B->pitchDeg);
    JsonStr(f, B->mapPath);
    fprintf(f, "]\n  }");

    fprintf(f, ",\n  \"map\": {\n    \"path\": "); JsonStr(f, B->mapPath);
    fprintf(f, ",\n    \"hash\": \"%016llx\",\n    \"hashAlgorithm\": \"FNV-1a 64 of the file bytes\"", world.hash);
    fprintf(f, ",\n    \"mtime\": %ld,\n    \"version\": %u,\n    \"boxes\": %d,\n    \"groups\": %d\n  }\n}\n",
            world.mtime, world.version, world.count, world.groupCount);

    return fclose(f) == 0;
}

// #endregion

// ======================================================================================
// #region Open requests (HUD list + result.md polling)
// ======================================================================================

typedef struct {
    bool used;
    long seq;                   // submit order, for "most recent first"
    char id[128], slug[SLUG_MAX + 1];
    char dir[1300];             // bundle folder, trailing slash
    double submitTime, doneTime;
    bool launched;              // false: launching failed, the developer runs it by hand
    bool exited;                // the terminal process is gone (result may still come)
    bool done;
    long resultSize;            // size of result.md at the last poll (wait until it settles)
    char summary[160];          // first line of result.md
    Vector3 eye;
    float yaw, pitch;
#ifndef _WIN32
    pid_t pid;
#endif
} OpenRequest;

static OpenRequest reqs[MAX_OPEN_REQUESTS];
static long nextSeq = 1;
static double clockTime;        // sum of LauncherUpdate dt, the HUD's time base
static float pollTimer;
static long lastDoneSeq;        // most recently finished request (by finish order)
static long doneCounter;
static Vector3 lastDoneEye;
static float lastDoneYaw, lastDonePitch;

#ifndef _WIN32
// Every child is reaped with waitpid(pid, WNOHANG) from LauncherUpdate. Not waitpid(-1):
// that would also collect children some other code might start and wait for. Not a
// SIGCHLD handler: it would interrupt raylib's and the driver's system calls. Children
// are tracked here separately from reqs[], whose slots get recycled.
#define MAX_CHILDREN 64
static pid_t children[MAX_CHILDREN];
#endif

static OpenRequest *NewSlot(void)
{
    OpenRequest *pick = NULL;
    for (int i = 0; i < MAX_OPEN_REQUESTS && !pick; i++) if (!reqs[i].used) pick = &reqs[i];
    // Full: recycle the oldest finished request, else the oldest one at all
    for (int pass = 0; pass < 2 && !pick; pass++)
        for (int i = 0; i < MAX_OPEN_REQUESTS; i++)
            if ((pass == 1 || reqs[i].done) && (!pick || reqs[i].seq < pick->seq)) pick = &reqs[i];
    memset(pick, 0, sizeof(*pick));
    pick->used = true;
    pick->seq = nextSeq++;
    return pick;
}

// Reads the first non-empty line of result.md, without leading '#'s and blanks
static bool ReadResult(OpenRequest *q)
{
    char path[1400];
    snprintf(path, sizeof(path), "%sresult.md", q->dir);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    buf[n] = 0;
    // Still being written? Accept once it has a complete line or its size held still
    // for one poll interval.
    bool settled = size == q->resultSize;
    q->resultSize = size;
    if (size <= 0) return false;
    char *p = buf;
    for (;;) {
        while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') p++;
        if (!*p) return false;
        char *e = p;
        while (*e && *e != '\n') e++;
        if (!*e && !settled) return false;
        char *s = p;
        while (*s == '#' || *s == ' ' || *s == '\t') s++;
        char *end = e;
        while (end > s && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
        if (end > s) {
            int len = (int)(end - s);
            if (len > (int)sizeof(q->summary) - 1) len = (int)sizeof(q->summary) - 1;
            memcpy(q->summary, s, len);
            q->summary[len] = 0;
            // No control characters in the HUD line (non-ASCII shows as '?' in raylib's font)
            for (char *c = q->summary; *c; c++) if ((unsigned char)*c < 0x20) *c = ' ';
            return true;
        }
        if (!*e) return false;
        p = e;      // a line of only '#'s: try the next one
    }
}

void LauncherUpdate(float dt)
{
    clockTime += dt;
    pollTimer += dt;
    if (pollTimer < POLL_INTERVAL) return;
    pollTimer = 0.0f;

#ifndef _WIN32
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (children[i] <= 0) continue;
        pid_t r = waitpid(children[i], NULL, WNOHANG);
        if (r == 0) continue;               // still running
        for (int k = 0; k < MAX_OPEN_REQUESTS; k++)
            if (reqs[k].used && reqs[k].pid == children[i]) reqs[k].exited = true;
        children[i] = 0;                    // reaped (or not ours any more: ECHILD)
    }
#endif

    for (int i = 0; i < MAX_OPEN_REQUESTS; i++) {
        OpenRequest *q = &reqs[i];
        if (!q->used || q->done) continue;
        if (ReadResult(q)) {
            q->done = true;
            q->doneTime = clockTime;
            lastDoneSeq = ++doneCounter;
            lastDoneEye = q->eye;
            lastDoneYaw = q->yaw;
            lastDonePitch = q->pitch;
            TraceLog(LOG_INFO, "monolith: request %s done: %s", q->id, q->summary);
        }
    }
}

int LauncherDrawHud(int x, int y)
{
    const int size = 18, step = 22, maxRows = 6;
    int rows = 0;
    long below = 1L << 30;      // walk the list from the newest request down
    while (rows < maxRows) {
        OpenRequest *q = NULL;
        for (int i = 0; i < MAX_OPEN_REQUESTS; i++)
            if (reqs[i].used && reqs[i].seq < below && (!q || reqs[i].seq > q->seq)) q = &reqs[i];
        if (!q) break;
        below = q->seq;

        const char *text;
        Color c = { 235, 232, 225, 220 };
        if (q->done) {
            float age = (float)(clockTime - q->doneTime);
            if (age > DONE_SHOW_TIME) continue;
            c = Fade((Color){ 150, 230, 150, 255 }, fminf((DONE_SHOW_TIME - age) / DONE_FADE_TIME, 1.0f));
            text = TextFormat("[done] %s: %s", q->slug, q->summary);
        } else {
            int t = (int)(clockTime - q->submitTime);
            if (!q->launched) {
                c = (Color){ 255, 170, 110, 230 };
                text = TextFormat("[!] %s not launched, run it by hand (see log), waiting (%02d:%02d)", q->slug, t / 60, t % 60);
            } else if (q->exited)
                // Not necessarily a failure: some terminals hand the window to a daemon
                // and exit right away (kitty --single-instance, wezterm with a mux)
                text = TextFormat("... %s waiting for result.md, terminal process gone (%02d:%02d)", q->slug, t / 60, t % 60);
            else
                text = TextFormat("... %s running (%02d:%02d)", q->slug, t / 60, t % 60);
        }
        DrawText(text, x, y + rows * step, size, c);
        rows++;
    }
    return rows * step;
}

bool LauncherLastDone(Vector3 *eye, float *yaw, float *pitch)
{
    if (!lastDoneSeq) return false;
    *eye = lastDoneEye;
    *yaw = lastDoneYaw;
    *pitch = lastDonePitch;
    return true;
}

// #endregion

// ======================================================================================
// #region Launching (Linux: fork + execve; Windows: cmd start)
// ======================================================================================

#ifdef _WIN32

static bool FindProgram(const char *name, char *out, int outSize)
{
    static const char *exts[] = { ".exe", ".cmd", ".bat", "" };
    const char *path = getenv("PATH");
    char dirs[8192];
    snprintf(dirs, sizeof(dirs), "%s", path ? path : "");
    for (char *d = dirs; d && *d; ) {
        char *semi = strchr(d, ';');
        if (semi) *semi = 0;
        for (int e = 0; e < 4; e++) {
            int w = snprintf(out, outSize, "%s\\%s%s", d, name, exts[e]);
            if (*d && w > 0 && w < outSize && _access(out, 0) == 0) return true;
        }
        d = semi ? semi + 1 : NULL;
    }
    const char *home = getenv("USERPROFILE");
    if (home) {
        snprintf(out, outSize, "%s\\.local\\bin\\%s.exe", home, name);
        if (_access(out, 0) == 0) return true;
    }
    return false;
}

static bool Launch(const char *id, const char *slug, const char *prompt, OpenRequest *q, char *err, int errSize)
{
    (void)q;
    char claude[1024];
    if (!FindProgram("claude", claude, sizeof(claude))) {
        snprintf(err, errSize, "claude not found in PATH");
        return false;
    }
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", rootDir);
    for (char *c = dir; *c; c++) if (*c == '/') *c = '\\';
    size_t n = strlen(dir);
    if (n > 1 && dir[n - 1] == '\\') dir[n - 1] = 0;     // "C:\x\" would escape the closing quote

    // msvcrt joins the argv with spaces and adds no quotes, so quote here. cmd /k strips
    // the outer pair of quotes of its command when it contains more than two.
    char argsBuf[1024], *extra[CLAUDE_ARGS_MAX];
    int extraCount = SplitWords(getenv("MONOLITH_CLAUDE_ARGS"), argsBuf, sizeof(argsBuf), extra, CLAUDE_ARGS_MAX);
    char cmd[4096];
    int len = snprintf(cmd, sizeof(cmd), "\"\"%s\"", claude);
    for (int i = 0; i < extraCount && len < (int)sizeof(cmd); i++) len += snprintf(cmd + len, sizeof(cmd) - len, " %s", extra[i]);
    if (len < (int)sizeof(cmd)) snprintf(cmd + len, sizeof(cmd) - len, " \"%s\"\"", prompt);
    char title[128], dirArg[1100];
    snprintf(title, sizeof(title), "\"monolith-claude: %s\"", slug);
    snprintf(dirArg, sizeof(dirArg), "\"%s\"", dir);
    const char *argv[] = { "cmd.exe", "/c", "start", title, "/D", dirArg, "cmd.exe", "/k", cmd, NULL };
    intptr_t h = _spawnvp(_P_NOWAIT, "cmd.exe", argv);
    if (h == -1) {
        snprintf(err, errSize, "cannot start cmd.exe: %s", strerror(errno));
        return false;
    }
    CloseHandle((void *)h);     // cmd /c start exits right away; nothing to wait for
    TraceLog(LOG_INFO, "monolith: launched request %s via cmd start", id);
    return true;
}

#else

// Like execvp's PATH search, done in the parent (the child may only call
// async-signal-safe functions between fork and exec)
static bool FindProgram(const char *name, char *out, int outSize)
{
    struct stat st;
    if (strchr(name, '/')) {
        AbsPath(name, out, outSize);
        return stat(out, &st) == 0 && S_ISREG(st.st_mode) && access(out, X_OK) == 0;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    for (const char *d = path; ; ) {
        const char *colon = strchr(d, ':');
        int len = colon ? (int)(colon - d) : (int)strlen(d);
        char cand[4096];
        snprintf(cand, sizeof(cand), "%.*s/%s", len ? len : 1, len ? d : ".", name);
        if (stat(cand, &st) == 0 && S_ISREG(st.st_mode) && access(cand, X_OK) == 0) {
            // Keep symlinks as they are: ~/.local/bin/claude points at a versioned binary
            // that self-updates replace, so the link is the stable name to run
            if (cand[0] == '/') snprintf(out, outSize, "%s", cand);
            else AbsPath(cand, out, outSize);
            return true;
        }
        if (!colon) break;
        d = colon + 1;
    }
    return false;
}

// "Run this argv" syntax per terminal. pre: subcommand after the program name;
// exec: the flag before the command (NULL = the command just follows the options).
typedef struct {
    const char *name, *pre, *titleFlag, *dirFlag, *exec;
} TermSyntax;

static const TermSyntax TERMINALS[] = {
    { "alacritty", NULL,    "--title", "--working-directory", "-e" },
    { "kitty",     NULL,    "--title", "--directory",         NULL },
    { "foot",      NULL,    "--title", "--working-directory", NULL },
    { "wezterm",   "start", NULL,      "--cwd",               "--" },
    { "xterm",     NULL,    "-T",      NULL,                  "-e" },
};
#define TERMINAL_COUNT ((int)(sizeof(TERMINALS) / sizeof(TERMINALS[0])))
static const TermSyntax UNKNOWN_TERMINAL = { NULL, NULL, NULL, NULL, "-e" };

static bool Launch(const char *id, const char *slug, const char *prompt, OpenRequest *q, char *err, int errSize)
{
    (void)id;
    // Terminal: env override (program + extra args) or the first known one in PATH
    char termBuf[1024], *termTok[16], termPath[4096];
    int termCount = 0;
    const char *envVar = NULL;
    const char *env = getenv("MONOLITH_TERMINAL");
    if (env && *env) envVar = "MONOLITH_TERMINAL";
    else if ((env = getenv("TERMINAL")) && *env) envVar = "TERMINAL";
    if (envVar) {
        termCount = SplitWords(env, termBuf, sizeof(termBuf), termTok, 16);
        if (termCount == 0 || !FindProgram(termTok[0], termPath, sizeof(termPath))) {
            snprintf(err, errSize, "terminal '%s' from $%s not found", termCount ? termTok[0] : "", envVar);
            return false;
        }
    } else {
        for (int i = 0; i < TERMINAL_COUNT && !termCount; i++)
            if (FindProgram(TERMINALS[i].name, termPath, sizeof(termPath)))
                termCount = SplitWords(TERMINALS[i].name, termBuf, sizeof(termBuf), termTok, 16);
        if (!termCount) {
            snprintf(err, errSize, "no terminal found (tried alacritty kitty foot wezterm xterm; set MONOLITH_TERMINAL)");
            return false;
        }
    }
    const TermSyntax *ts = &UNKNOWN_TERMINAL;
    for (int i = 0; i < TERMINAL_COUNT; i++)
        if (strcmp(BaseName(termTok[0]), TERMINALS[i].name) == 0) ts = &TERMINALS[i];

    char claude[4096];
    bool found = FindProgram("claude", claude, sizeof(claude));
    if (!found && getenv("HOME")) {
        char cand[2048];
        snprintf(cand, sizeof(cand), "%s/.local/bin/claude", getenv("HOME"));
        found = FindProgram(cand, claude, sizeof(claude));
    }
    if (!found) {
        snprintf(err, errSize, "claude not found in PATH or ~/.local/bin");
        return false;
    }

    // argv: terminal [its extra args] [pre] [title] [dir] [exec] claude [claude args] prompt
    char title[128], dir[1024], argsBuf[1024], *extra[CLAUDE_ARGS_MAX];
    snprintf(title, sizeof(title), "monolith-claude: %s", slug);
    snprintf(dir, sizeof(dir), "%s", rootDir);
    size_t dl = strlen(dir);
    if (dl > 1 && dir[dl - 1] == '/') dir[dl - 1] = 0;
    int extraCount = SplitWords(getenv("MONOLITH_CLAUDE_ARGS"), argsBuf, sizeof(argsBuf), extra, CLAUDE_ARGS_MAX);
    char *argv[16 + 8 + CLAUDE_ARGS_MAX + 4];
    int n = 0;
    for (int i = 0; i < termCount; i++) argv[n++] = termTok[i];
    if (ts->pre) argv[n++] = (char *)ts->pre;
    if (ts->titleFlag) { argv[n++] = (char *)ts->titleFlag; argv[n++] = title; }
    if (ts->dirFlag) { argv[n++] = (char *)ts->dirFlag; argv[n++] = dir; }
    if (ts->exec) argv[n++] = (char *)ts->exec;
    argv[n++] = claude;
    for (int i = 0; i < extraCount; i++) argv[n++] = extra[i];
    argv[n++] = (char *)prompt;
    argv[n] = NULL;

    // exec failures in the child are reported through a close-on-exec pipe: a successful
    // exec closes it (the parent reads EOF), a failed one writes errno first. The read
    // only waits for the exec itself, not for the terminal.
    int fds[2];
    if (pipe(fds) != 0) { snprintf(err, errSize, "pipe: %s", strerror(errno)); return false; }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        snprintf(err, errSize, "fork: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        // Child: only async-signal-safe calls from here on. setsid: own session, so the
        // terminal survives the game (and Ctrl+C in the game's console doesn't reach it).
        close(fds[0]);
        setsid();
        int e = 0;
        if (chdir(dir) != 0) e = errno;
        if (!e) {
            int nul = open("/dev/null", O_RDONLY);  // don't share the game's stdin
            if (nul > 0) { dup2(nul, 0); close(nul); }
            execve(termPath, argv, environ);
            e = errno;
        }
        ssize_t w = write(fds[1], &e, sizeof(e));
        (void)w;
        _exit(127);
    }
    close(fds[1]);
    int childErr = 0;
    ssize_t r;
    do r = read(fds[0], &childErr, sizeof(childErr)); while (r < 0 && errno == EINTR);
    close(fds[0]);
    if (r == (ssize_t)sizeof(childErr)) {
        waitpid(pid, NULL, 0);      // it has _exit()ed already
        snprintf(err, errSize, "cannot run %s: %s", termPath, strerror(childErr));
        return false;
    }

    q->pid = pid;
    bool tracked = false;
    for (int i = 0; i < MAX_CHILDREN && !tracked; i++)
        if (children[i] <= 0) { children[i] = pid; tracked = true; }
    // All slots busy (64 terminals still open): it gets reaped by init after we exit
    if (!tracked) TraceLog(LOG_WARNING, "monolith: too many open terminals, pid %d not tracked", (int)pid);
    TraceLog(LOG_INFO, "monolith: launched %s (pid %d) for request %s", termPath, (int)pid, id);
    return true;
}

#endif

// #endregion

// ======================================================================================
// #region Submit
// ======================================================================================

bool LauncherSubmit(const DesignRequest *req, Image view, Image marked, char *msg, int msgSize)
{
    InitPaths();
    static Bundle B;    // ~10 KB of tables, and only one submit runs at a time
    memset(&B, 0, sizeof(B));

    // Folder name: local time stamp + slug, made unique if a request with the same
    // name was submitted within the same second
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char stamp[32] = "0000-00-00_000000";
    if (tm) {
        strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", tm);
        strftime(B.created, sizeof(B.created), "%Y-%m-%d %H:%M:%S", tm);
    }
    MakeSlug(req->prompt, B.slug, sizeof(B.slug));

    MakeDirectory(requestsDir);     // raylib: recursive, fine if it exists
    char dir[1300];
    bool made = false;
    for (int n = 1; n < 100 && !made; n++) {
        if (n == 1) snprintf(B.id, sizeof(B.id), "%s_%s", stamp, B.slug);
        else snprintf(B.id, sizeof(B.id), "%s_%s-%d", stamp, B.slug, n);
        snprintf(dir, sizeof(dir), "%s%s", requestsDir, B.id);
        if (MakeDir(dir) == 0) made = true;
        else if (errno != EEXIST) break;
    }
    if (!made) {
        snprintf(msg, msgSize, "cannot create a request folder in %s: %s", requestsDir, strerror(errno));
        TraceLog(LOG_WARNING, "monolith: %s", msg);
        return false;
    }
    strcat(dir, "/");

    BuildBundle(&B, req);

    bool ok = true;
    char path[1400];
    snprintf(path, sizeof(path), "%sview.png", dir);
    if (view.data) ok &= ExportImage(view, path);
    snprintf(path, sizeof(path), "%sview-marked.png", dir);
    if (marked.data) ok &= ExportImage(marked, path);
    snprintf(path, sizeof(path), "%srequest.md", dir);
    ok &= WriteRequestMd(path, &B);
    snprintf(path, sizeof(path), "%srequest.json", dir);
    ok &= WriteRequestJson(path, &B);
    if (!ok) {
        snprintf(msg, msgSize, "cannot write the request bundle %s (disk full?)", dir);
        TraceLog(LOG_WARNING, "monolith: %s", msg);
        return false;
    }

    OpenRequest *q = NewSlot();
    snprintf(q->id, sizeof(q->id), "%s", B.id);
    snprintf(q->slug, sizeof(q->slug), "%s", B.slug);
    snprintf(q->dir, sizeof(q->dir), "%s", dir);
    q->submitTime = clockTime;
    q->eye = req->eye;
    q->yaw = req->yaw;
    q->pitch = req->pitch;

    char err[512] = "";
    q->launched = Launch(B.id, B.slug, B.prompt, q, err, sizeof(err));
    if (!q->launched) {
        snprintf(msg, msgSize, "launch failed: %s. Bundle kept: %s", err, dir);
        TraceLog(LOG_WARNING, "monolith: %s", msg);
        TraceLog(LOG_WARNING, "monolith: run it by hand: cd '%s' && claude \"%s\"", rootDir, B.prompt);
        return false;
    }
    snprintf(msg, msgSize, "request sent: %s (Claude Code opens in a new terminal)", B.id);
    return true;
}

// #endregion
