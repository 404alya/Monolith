// Launcher: request bundles, the request queue + coordinator session, polling for result.md
//
// A submitted request becomes a self-contained folder requests/<YYYY-MM-DD_HHMMSS>_<slug>/
// (view.png, view-marked.png, region-<n>.png per screen region, request.md, request.json;
// bundle format 2, .claude/skills/map-request/bundle-reference.md). The model is chosen per request (fast /
// default / heavy tier, see "Model choice" below) and recorded in request.md / request.json.
// The user's prompt never goes on a command line: it only lives in the bundle, and every
// process is started from an argv array (no shell).
//
// Two ways to hand the bundle to Claude Code (MONOLITH_LAUNCH):
//   - coordinator (default; region "Coordinator and queue", docs/request-queue.md): the
//     game writes requests/<id>/state = "queued" as the bundle's last file. Unless a
//     coordinator holds requests/.coordinator/lock, one terminal titled "monolith-claude:
//     coordinator" runs a small /bin/sh wrapper that holds the lock (flock) around `claude
//     <permission flags> --model $MONOLITH_COORDINATOR_MODEL (default sonnet)
//     [MONOLITH_CLAUDE_ARGS] "You are the monolith request coordinator. Read
//     .claude/skills/coordinator/SKILL.md ... Requests folder: <path>"`. Later requests only go into the queue;
//     they never open a window. The coordinator (tools/request-queue.sh) claims bundles,
//     hands each to a subagent on the request's model and writes every later state (claimed,
//     question, done, failed, cancelled). The HUD follows state + result.md. Without flock
//     (util-linux) the game falls back to per-request mode.
//   - per-request: a new terminal per request runs `claude <permission flags> --model <name>
//     [MONOLITH_CLAUDE_ARGS] "Map edit request: read requests/<id>/request.md and carry it
//     out."` in the repo root (no state file); the behaviour before 2026-10-24.
//
// Where things go:
//   - requests/ and the working directory of Claude Code: the repo root baked in by
//     build.sh (PROJECT_PATH) while it exists, i.e. in a repo checkout; otherwise the
//     directory of the executable (a copied build/linux or build/win folder).
//     MONOLITH_REQUESTS_DIR (tests only) moves requests/ (bundles, queue, lock) elsewhere.
//   - terminal: $MONOLITH_TERMINAL, else $TERMINAL, else the first of alacritty, kitty,
//     foot, wezterm, xterm found in PATH. The variables may hold a program name or path
//     plus extra arguments ("kitty --single-instance"); the program's basename picks
//     the syntax from TERMINALS below, unknown terminals get `-e <command>`.
//   - claude: $MONOLITH_CLAUDE (tests: tools/stub-coordinator.sh), else searched in PATH,
//     then ~/.local/bin/claude (where the native installer puts it, which a game started
//     from a desktop launcher may not have in PATH). It is passed to the terminal as an
//     absolute path, so a terminal daemon with another environment (kitty
//     --single-instance) still finds it.
//   - permissions: every session starts with --allow-dangerously-skip-permissions
//     --dangerously-skip-permissions, so the developer isn't asked to approve each
//     command; the guard rails live in CLAUDE.md and the coordinator / map-request
//     skills (.claude/skills/) instead. MONOLITH_CLAUDE_SAFE=1 (any value but empty or "0") drops the two flags.
//     So does a working directory without CLAUDE.md (Windows, a copied build folder): with no
//     guard rails to read, the session keeps Claude Code's approval prompts.
//   - model: per request MONOLITH_MODEL, MONOLITH_MODEL_FAST / _DEFAULT / _HEAVY and an @tier
//     prompt prefix steer it (region "Model choice"); the coordinator passes it to the
//     request's subagent. The coordinator's own model: MONOLITH_COORDINATOR_MODEL.
//   - extra Claude Code flags: $MONOLITH_CLAUDE_ARGS, split on whitespace (no quoting),
//     after --model, on the coordinator's (or the request's) command line. Default none.
//     A --model in them replaces the chosen one (and the coordinator's).
//   - workspace (i3 / sway): the new terminal is moved to workspace $MONOLITH_WORKSPACE
//     (default 10; empty = leave it where it opens) with `i3-msg` (`swaymsg` when
//     $SWAYSOCK is set) and the criteria [title="^<its title>$"]. A detached
//     /bin/sh helper retries every 0.1 s for up to 10 s until the window exists, so the
//     game never waits. A criteria `move container to workspace` doesn't switch the
//     visible workspace; focus returns to the previously focused window. The window may
//     still flash up for a moment where it opened: an i3 `for_window` rule on the title
//     (terminals set it before the window is mapped) avoids that, and the IPC move then
//     finds it already there. Other window managers: i3-msg finds no i3 (exit 1) and the
//     helper stops at once. The same command also gives the window an i3 mark,
//     _monolith_coordinator or _monolith_<id> (also with MONOLITH_WORKSPACE empty), for
//     closing it later.
//   - closing (region "Closing request windows"): the HUD list numbers the requests.
//     Coordinator mode: K / SHIFT+K take finished requests off the list, CTRL+K n cancels
//     request n (a running one on a second press: the coordinator stops its subagent),
//     CTRL+SHIFT+K closes the coordinator's terminal. Per-request mode: K / SHIFT+K / CTRL+K
//     n close a request's terminal. Closing goes through the mark (`i3-msg [con_mark=...]
//     kill`), else SIGTERM / SIGKILL to the terminal's process group. MONOLITH_CLOSE_ON_EXIT=1
//     closes the finished requests' terminals (coordinator mode: the coordinator, if none of
//     this run's requests is still open) when the game quits.
//
// Windows: the same bundle and queue, then `cmd /c start "<title>" /D <root> cmd /k
// "<claude>" <permission flags> --model <name> ...` via _spawnvp (no workspace move)
// (<process.h>, so no <windows.h> clash with raylib); the coordinator's command ends in
// ` 9>"<requests>\.coordinator\lock"`, which cmd holds open while claude runs. Only
// compile-tested; failures of `start` itself can't be seen from here.

#ifndef _WIN32
#   define _XOPEN_SOURCE 700           // POSIX 2008 + XSI: fork, setsid, readlink, realpath
#   define _DEFAULT_SOURCE             // flock (the coordinator's lock)
#endif

#include "launcher.h"
#include "wincmd.h"
#include "ui.h"
#include "png.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#   include <direct.h>     // _mkdir
#   include <io.h>         // _access
#   include <process.h>    // _spawnvp
#   include <share.h>      // _SH_DENYRW (the coordinator's lock)
#   include <fcntl.h>
#   include <sys/stat.h>
#   include <stdint.h>
// Declared by hand like the GL functions in main.c (<windows.h> clashes with raylib).
// x64 has one calling convention, so this matches kernel32.dll.
extern int CloseHandle(void *handle);
#else
#   include <fcntl.h>
#   include <signal.h>
#   include <sys/file.h>   // flock
#   include <sys/stat.h>
#   include <sys/types.h>
#   include <sys/wait.h>
#   include <unistd.h>
extern char **environ;
#endif

#ifndef PROJECT_PATH
#   define PROJECT_PATH ""
#endif

// Memory: the open requests, the tracked child processes, a bundle's selection / nearby
// tables and the split command lines are heap arrays (MS_RESERVE / MS_PUSH); no count
// limits. Bundles are built and freed per submit; LauncherShutdown frees the rest.
#define POLL_INTERVAL       0.5f    // seconds between result.md checks
#define DONE_SHOW_TIME      20.0f   // finished requests stay in the HUD this long
#define DONE_FADE_TIME      3.0f    // ... fading out over the last seconds
#define NEARBY_RADIUS       30.0f   // "nearby context" radius around the hit point, metres
#define SLUG_MAX            40

// ======================================================================================
// #region Paths and small helpers
// ======================================================================================

static char rootDir[1024];          // repo root (or exe dir), trailing slash
static char requestsDir[1100];      // rootDir + "requests/" (or $MONOLITH_REQUESTS_DIR), trailing slash
static char coordDir[1200];         // requestsDir + ".coordinator/": the coordinator's lock, seen, log
static unsigned requestsHash;       // 0, or with MONOLITH_REQUESTS_DIR a hash of it (window title / mark)

static void InitCoordinatorMark(void);
static void AbsPath(const char *in, char *out, int outSize);

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
    // MONOLITH_REQUESTS_DIR (tests only): bundles, queue and coordinator lock elsewhere, so a
    // test never meets the developer's coordinator. Bundles still name requests/<id>/ in
    // their commands.
    const char *over = getenv("MONOLITH_REQUESTS_DIR");
    if (over && *over) {
        MakeDirectory(over);
        AbsPath(over, requestsDir, sizeof(requestsDir));
        n = strlen(requestsDir);
        if (n && requestsDir[n - 1] != '/' && requestsDir[n - 1] != '\\' && n + 1 < sizeof(requestsDir)) strcat(requestsDir, "/");
        requestsHash = 2166136261u;
        for (const char *c = requestsDir; *c; c++) requestsHash = (requestsHash ^ (unsigned char)*c) * 16777619u;
        if (!requestsHash) requestsHash = 1;
    } else snprintf(requestsDir, sizeof(requestsDir), "%srequests/", rootDir);
    snprintf(coordDir, sizeof(coordDir), "%s.coordinator/", requestsDir);
    InitCoordinatorMark();
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

// Words of a command line (an environment variable): a heap copy of the text, split in place
typedef struct { char *buf; char **tok; int count, cap; } ArgList;

static void FreeArgs(ArgList *a)
{
    free(a->buf);
    free(a->tok);
    memset(a, 0, sizeof(*a));
}

// Splits s on whitespace (s NULL = no words). False when out of memory (a is empty then).
static bool SplitArgs(const char *s, ArgList *a)
{
    memset(a, 0, sizeof(*a));
    if (!(a->buf = MS_StrDup(s ? s : ""))) return false;
    char *p = a->buf;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char **slot = MS_PUSH(a->tok, a->count, a->cap);
        if (!slot) { FreeArgs(a); return false; }
        *slot = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = 0;
    }
    return true;
}

// $MONOLITH_CLAUDE_ARGS split into words, without a --model that has no value: a bare
// `--model` as the last word, or `--model=` with nothing after the '='. Passed on, such a
// word would take the next argument as the model name, which is the prompt (Claude Code
// would then start without a prompt and with a bogus model). Logs a warning (warn) and sets
// *dropped when it removed one. False when out of memory (a is empty then).
static bool ClaudeArgs(ArgList *a, bool *dropped, bool warn)
{
    if (dropped) *dropped = false;
    if (!SplitArgs(getenv("MONOLITH_CLAUDE_ARGS"), a)) return false;
    int n = 0;
    for (int i = 0; i < a->count; i++) {
        const char *t = a->tok[i];
        if ((strcmp(t, "--model") == 0 && i + 1 == a->count) || strcmp(t, "--model=") == 0) {
            if (warn) TraceLog(LOG_WARNING, "monolith: MONOLITH_CLAUDE_ARGS: '%s' without a model name ignored", t);
            if (dropped) *dropped = true;
            continue;
        }
        a->tok[n++] = a->tok[i];
    }
    a->count = n;
    return true;
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
    return (g >= 0 && g < scene->groupCount) ? scene->groups[g] : "(no group)";
}

// Object references ({kind, index}, runtime/monolith_scene.h). The bundle's per-object
// lookups go through these and the runtime's MS_ObjectRef / MS_ObjectBounds, so models
// work here once the runtime has them (MS_ObjectRef covers MS_OBJ_MODEL).
static bool ObjValid(MS_ObjRef o) { return MS_ObjectRef(scene, o) != NULL; }
static const char *ObjKindName(MS_ObjRef o) { return o.kind == MS_OBJ_BOX ? "box" : o.kind == MS_OBJ_MODEL ? "model" : "none"; }
static BoundingBox ObjBounds(MS_ObjRef o) { return MS_ObjectBounds(scene, o); }
// A turned box's record (a box line with `rot`; its bounds are then its world AABB), NULL otherwise
static const MS_TurnedBox *ObjTurn(MS_ObjRef o)
{
    return o.kind == MS_OBJ_BOX && scene->boxTurned && scene->boxTurned[o.index] >= 0 ? &scene->turnedBoxes[scene->boxTurned[o.index]]
                                                                                    : NULL;
}
static int ObjLine(MS_ObjRef o) { return MS_ObjectRef(scene, o)->line; }
static int ObjGroup(MS_ObjRef o) { return MS_ObjectRef(scene, o)->group; }
// Material name (models: their override material; NULL = the model's own materials)
static const char *ObjMaterial(MS_ObjRef o)
{
    int m = MS_ObjectRef(scene, o)->material;
    return m >= 0 && m < scene->materialCount ? scene->materials[m].name : NULL;
}
#define MAP_MATERIALS (scene->materialCount)
#define MAP_FORMAT_VERSION (scene->formatVersion)

// Index of an object among the objects of its kind on its map line (copy number of a repeat)
static int ObjNth(MS_ObjRef o) { return DesignerObjNth(o); }

// Where an object sits among the objects its map line made: nth (0-based) of total, and
// for a `repeat` line the copy's index per repeat (outer first) plus the object within the
// copy (stairs / waffle make several per copy). Splitting a repeat line needs exactly this.
typedef struct {
    int nth, total;
    int repeats;            // repeat prefixes found (0..3); idx[] valid when decomposed
    int counts[3], idx[3];
    int perCopy, inCopy;    // objects per copy, and this one's index within its copy
    bool decomposed;
} CopyInfo;

static const char *SkipSpace(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}
static const char *SkipWord(const char *p, const char *end)
{
    while (p < end && *p != ' ' && *p != '\t') p++;
    return p;
}

static CopyInfo ObjCopy(MS_ObjRef o)
{
    CopyInfo c = { .nth = ObjNth(o), .perCopy = 1 };
    int line = ObjLine(o), count = o.kind == MS_OBJ_MODEL ? scene->modelCount : scene->boxCount;
    c.total = c.nth + 1;
    for (int i = o.index + 1; i < count && MS_ObjectRef(scene, (MS_ObjRef){ o.kind, i })->line == line; i++) c.total++;
    // The line's placement prefixes: repeat n dx dy dz (up to 3), at x y z
    int len = 0;
    const char *p = line > 0 ? MS_LineText(scene, line, &len) : NULL;
    const char *end = p ? p + len : NULL;
    while (p && p < end) {
        p = SkipSpace(p, end);
        const char *w = p;
        p = SkipWord(p, end);
        int skip;
        if (p - w == 6 && strncmp(w, "repeat", 6) == 0) {
            if (c.repeats == 3) break;
            p = SkipSpace(p, end);
            c.counts[c.repeats++] = (int)strtol(p, NULL, 10);
            p = SkipWord(p, end);
            skip = 3;
        } else if (p - w == 2 && strncmp(w, "at", 2) == 0) skip = 3;
        else break;
        for (int k = 0; k < skip; k++) p = SkipWord(SkipSpace(p, end), end);
    }
    long long copies = c.repeats > 0 ? 1 : 0;
    for (int k = 0; k < c.repeats; k++) copies *= c.counts[k] > 0 ? c.counts[k] : 0;
    if (copies > 0 && c.total % copies == 0) {
        c.perCopy = (int)(c.total / copies);
        int copy = c.nth / c.perCopy;
        c.inCopy = c.nth % c.perCopy;
        for (int k = c.repeats - 1; k >= 0; k--) { c.idx[k] = copy % c.counts[k]; copy /= c.counts[k]; }
        c.decomposed = true;
    }
    return c;
}

// "11 of 32 (i = 1, j = 3)": nth of the line's objects, then the repeat indices (outer first)
static void MdCopy(FILE *f, MS_ObjRef o)
{
    CopyInfo c = ObjCopy(o);
    fprintf(f, "%d of %d", c.nth, c.total);
    if (!c.decomposed) return;
    fprintf(f, " (");
    for (int k = 0; k < c.repeats; k++) fprintf(f, "%s%c = %d", k ? ", " : "", "ijk"[k], c.idx[k]);
    if (c.perCopy > 1) fprintf(f, "; object %d of %d in that copy", c.inCopy, c.perCopy);
    fprintf(f, ")");
}

// "repeat 4 x 8" of the object's line, or "" (for the explanations next to MdCopy)
static void MdRepeats(FILE *f, MS_ObjRef o)
{
    CopyInfo c = ObjCopy(o);
    if (!c.decomposed) return;
    fprintf(f, "; the line is `repeat ");
    for (int k = 0; k < c.repeats; k++) fprintf(f, "%s%d", k ? " x " : "", c.counts[k]);
    fprintf(f, "`, i = the first (outer) repeat%s", c.repeats > 1 ? ", j = the next" : "");
}

static void JsonCopy(FILE *f, MS_ObjRef o)
{
    CopyInfo c = ObjCopy(o);
    fprintf(f, "\"nth\": %d, \"lineObjects\": %d, \"repeat\": ", c.nth, c.total);
    if (!c.decomposed) { fprintf(f, "null"); return; }
    fprintf(f, "{ \"counts\": [");
    for (int k = 0; k < c.repeats; k++) fprintf(f, "%s%d", k ? ", " : "", c.counts[k]);
    fprintf(f, "], \"copy\": [");
    for (int k = 0; k < c.repeats; k++) fprintf(f, "%s%d", k ? ", " : "", c.idx[k]);
    fprintf(f, "], \"objectsPerCopy\": %d, \"objectInCopy\": %d }", c.perCopy, c.inCopy);
}

static const char *const ORIGIN_NAMES[] = { "model", "bottom", "center" };
static const char *const COLLIDE_NAMES[] = { "aabb", "none", "box" };

// #endregion

// ======================================================================================
// #region Model choice
// ======================================================================================
//
// Each request runs on one of three tiers: fast (default model `haiku`), default (`sonnet`),
// heavy (`opus`); Claude Code's aliases, so they follow the latest model of each family.
// ClassifyRequest() is pure and deterministic. Words are runs of ASCII letters, compared
// case-insensitively as whole words; a keyword also matches its plural (+s, +es, y->ies).
// First rule that applies wins:
//   heavy   1. kind "import" or "new-map"
//           2. the map is (nearly) empty (<= 2 objects) or has no MAP.md: the session must
//              write or extend the design brief
//           3. an asset word (model, asset, download, tree, car, statue, lamp, ...): it means
//              finding / importing models
//           4. a look / engine word (shader, material, texture, light, fog, sky, sun, ...)
//           5. a large-design word (build, design, create, district, city, building, ...)
//           6. long text: > 280 bytes or 3+ sentences; or more than 2 screen regions +
//              volumes together
//   fast    7. at most 120 bytes, one sentence, a target (crosshair hit, a selection of at
//              most 100 objects, or exactly one region or volume and not both), a local edit word (move, raise,
//              taller, delete, rename, mirror, ..., or "make"/"set" + a number), and no
//              creation word (add, put, place, insert, open, replace, row, ...)
//   default 8. everything else
// Overrides, strongest first: --model in MONOLITH_CLAUDE_ARGS (no second --model is added);
// MONOLITH_MODEL=<name> (one model for every request); a leading @fast / @default / @heavy
// (or @haiku / @sonnet / @opus) in the prompt picks the tier and is stripped from the
// prompt written to the bundle. MONOLITH_MODEL_FAST / _DEFAULT / _HEAVY rename the tiers.
// --classify "text" (main.c) prints the decision without opening a window.

static const char *const TIER_NAMES[] = { "fast", "default", "heavy" };
static const char *const TIER_MODELS[] = { "haiku", "sonnet", "opus" };
static const char *const TIER_ENV[] = { "MONOLITH_MODEL_FAST", "MONOLITH_MODEL_DEFAULT", "MONOLITH_MODEL_HEAVY" };

const char *ModelTierName(ModelTier tier) { return TIER_NAMES[tier]; }

#define FAST_MAX_BYTES      120
#define HEAVY_MIN_BYTES     280
#define FAST_MAX_SELECTION  100

static const char *const KW_ASSET[] = {
    "model", "asset", "download", "tree", "car", "statue", "lamp", "furniture", "prop", "vehicle",
    "plant", "character", "sculpture", "sketchfab", "poly pizza", "polypizza", "mesh", "glb", "gltf",
    "fbx", "person", "people", "human", "animal", "bush", "chair", "table", NULL };
static const char *const KW_LOOK[] = {
    "shader", "material", "texture", "light", "lighting", "fog", "sky", "sun", "mood", "atmosphere",
    "render", "rendering", "engine", "code", "performance", "shadow", "color", "colour", "bloom",
    "exposure", "glow", "emissive", "neon", "night", "weather", "rain", "snow", "fps", "dark", "darker",
    "bright", "brighter", "lighter", "moody", "moodier", NULL };
static const char *const KW_DESIGN[] = {
    "build", "design", "create", "generate", "redesign", "district", "city", "building", "level",
    "layout", "maze", "labyrinth", "interior", "town", "village", "street", "neighborhood",
    "neighbourhood", "quarter", "campus", "park", "garden", "castle", "temple", "house", NULL };
static const char *const KW_EDIT[] = {
    "move", "raise", "lower", "tall", "taller", "short", "shorter", "high", "higher", "wide", "wider",
    "narrow", "narrower", "thick", "thicker", "thin", "thinner", "long", "longer", "deep", "deeper",
    "delete", "remove", "rename", "shift", "align", "copy", "duplicate", "mirror", "scale", "resize",
    "flip", "hide", "rotate", "twice", "double", "half", "extend", "shrink", "stretch", "nudge",
    "lift", "drop", "flatten", "trim", "widen", "lengthen", "shorten", "heighten", "smaller",
    "bigger", "larger", "push", "pull", NULL };
static const char *const KW_CREATE[] = {
    "add", "put", "place", "insert", "new", "open", "opening", "connect", "attach", "fill",
    "replace", "convert", "turn", "row", "grid", "array", "pattern", NULL };

typedef char Word[24];      // lowercase; letters, or a number ("2.5"); longer words are cut
                            // (no keyword is that long, so a cut word never matches by accident)
typedef struct {
    Word *w;                // heap: every word of the prompt
    int n, cap;
} Words;

// False when out of memory (W then holds the words found so far)
static bool SplitPromptWords(const char *s, Words *W)
{
    W->n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        bool alpha = *p < 128 && isalpha(*p), digit = *p < 128 && isdigit(*p);
        if (!alpha && !digit) { p++; continue; }
        int len = 0;
        if (!MS_RESERVE(W->w, W->cap, W->n + 1)) return false;
        char *w = W->w[W->n++];
        while (*p < 128 && (alpha ? isalpha(*p) : (isdigit(*p) || (*p == '.' && isdigit(p[1]))))) {
            if (len < 23) w[len++] = (char)tolower(*p);
            p++;
        }
        w[len] = 0;
    }
    return true;
}

static bool WordIs(const char *w, const char *k)
{
    size_t n = strlen(k);
    if (strncmp(w, k, n) == 0) return !w[n] || strcmp(w + n, "s") == 0 || strcmp(w + n, "es") == 0;
    return n > 1 && k[n - 1] == 'y' && strncmp(w, k, n - 1) == 0 && strcmp(w + n - 1, "ies") == 0;
}

// First keyword of the list found in the words (a "two words" keyword matches a word pair)
static const char *FindKeyword(const Words *W, const char *const *list)
{
    for (int i = 0; i < W->n; i++)
        for (const char *const *k = list; *k; k++) {
            const char *sp = strchr(*k, ' ');
            if (!sp) { if (WordIs(W->w[i], *k)) return *k; continue; }
            char first[24];
            snprintf(first, sizeof(first), "%.*s", (int)(sp - *k), *k);
            if (i + 1 < W->n && strcmp(W->w[i], first) == 0 && WordIs(W->w[i + 1], sp + 1)) return *k;
        }
    return NULL;
}

static bool IsNumber(const char *w) { return isdigit((unsigned char)w[0]); }

// Sentences: text runs separated by . ! ? (followed by a blank) or line breaks
static int CountSentences(const char *s)
{
    int count = 0;
    bool inText = false;
    for (const char *p = s; *p; p++) {
        bool end = *p == '\n' || ((*p == '.' || *p == '!' || *p == '?') && (p[1] == 0 || isspace((unsigned char)p[1])));
        if (end) inText = false;
        else if (!isspace((unsigned char)*p) && !inText) { inText = true; count++; }
    }
    return count;
}

static ModelTier Classify(const ModelQuery *q, const Words *wordsIn, char *reason, int reasonSize);

ModelTier ClassifyRequest(const ModelQuery *q, char *reason, int reasonSize)
{
    Words W = { 0 };
    SplitPromptWords(q->prompt ? q->prompt : "", &W);
    ModelTier t = Classify(q, &W, reason, reasonSize);
    free(W.w);
    return t;
}

static ModelTier Classify(const ModelQuery *q, const Words *wordsIn, char *reason, int reasonSize)
{
    const Words W = *wordsIn;
    const char *prompt = q->prompt ? q->prompt : "";
    int bytes = (int)strlen(prompt), sentences = CountSentences(prompt);
    const char *k;

    if (q->kind && (strcmp(q->kind, "import") == 0 || strcmp(q->kind, "new-map") == 0)) {
        snprintf(reason, reasonSize, "%s request", q->kind);
        return MODEL_TIER_HEAVY;
    }
    if (q->emptyMap || q->noBrief) {
        snprintf(reason, reasonSize, "%s: the session writes the design brief", q->emptyMap ? "empty map" : "no MAP.md");
        return MODEL_TIER_HEAVY;
    }
    if ((k = FindKeyword(&W, KW_ASSET))) { snprintf(reason, reasonSize, "asset word \"%s\"", k); return MODEL_TIER_HEAVY; }
    if ((k = FindKeyword(&W, KW_LOOK))) { snprintf(reason, reasonSize, "look/engine word \"%s\"", k); return MODEL_TIER_HEAVY; }
    if ((k = FindKeyword(&W, KW_DESIGN))) { snprintf(reason, reasonSize, "design word \"%s\"", k); return MODEL_TIER_HEAVY; }
    if (bytes > HEAVY_MIN_BYTES) { snprintf(reason, reasonSize, "long prompt (%d bytes)", bytes); return MODEL_TIER_HEAVY; }
    if (sentences >= 3) { snprintf(reason, reasonSize, "%d sentences", sentences); return MODEL_TIER_HEAVY; }
    if (q->regions + q->volumes > 2) {
        if (q->volumes == 0) snprintf(reason, reasonSize, "%d screen regions", q->regions);
        else if (q->regions == 0) snprintf(reason, reasonSize, "%d volumes", q->volumes);
        else snprintf(reason, reasonSize, "%d screen regions + %d volumes", q->regions, q->volumes);
        return MODEL_TIER_HEAVY;
    }

    const char *edit = FindKeyword(&W, KW_EDIT);
    for (int i = 0; !edit && i + 1 < W.n; i++)
        if ((strcmp(W.w[i], "make") == 0 || strcmp(W.w[i], "set") == 0))
            for (int j = i + 1; j < W.n && j <= i + 4 && !edit; j++) if (IsNumber(W.w[j])) edit = "make/set + number";
    const char *create = FindKeyword(&W, KW_CREATE);
    bool target = q->hit || (q->selected > 0 && q->selected <= FAST_MAX_SELECTION) || q->regions + q->volumes == 1;
    if (q->selected > FAST_MAX_SELECTION) target = false;

    if (!edit) snprintf(reason, reasonSize, "no local edit word");
    else if (create) snprintf(reason, reasonSize, "creation word \"%s\"", create);
    else if (bytes > FAST_MAX_BYTES) snprintf(reason, reasonSize, "edit \"%s\" but %d bytes", edit, bytes);
    else if (sentences > 1) snprintf(reason, reasonSize, "edit \"%s\" but %d sentences", edit, sentences);
    else if (!target) snprintf(reason, reasonSize, "edit \"%s\" but no small target (hit/selection/1 region or volume)", edit);
    else {
        snprintf(reason, reasonSize, "short local edit \"%s\" with a target", edit);
        return MODEL_TIER_FAST;
    }
    return MODEL_TIER_DEFAULT;
}

static const char *EnvOr(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return v && *v ? v : fallback;
}

void LauncherChooseModel(const ModelQuery *q, ModelChoice *out)
{
    memset(out, 0, sizeof(*out));
    out->source = "classifier";
    out->addFlag = true;
    const char *prompt = q->prompt ? q->prompt : "";

    // Leading @token: only when some text follows it
    char why[200];
    const char *p = prompt;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    int tier = -1;
    if (*p == '@') {
        static const char *const TOKENS[] = { "fast", "default", "heavy", "haiku", "sonnet", "opus" };
        for (int t = 0; t < 6 && tier < 0; t++) {
            size_t n = strlen(TOKENS[t]), i = 0;
            while (i < n && tolower((unsigned char)p[1 + i]) == TOKENS[t][i]) i++;
            if (i == n && (p[1 + n] == 0 || isspace((unsigned char)p[1 + n]))) {
                const char *rest = p + 1 + n;
                while (*rest && isspace((unsigned char)*rest)) rest++;
                if (*rest) { tier = t % 3; out->promptSkip = (int)(rest - prompt); snprintf(why, sizeof(why), "@%s in the prompt", TOKENS[t]); }
            }
        }
    }
    if (tier >= 0) {
        out->tier = (ModelTier)tier;
        out->override = true;
        out->source = "prompt";
    } else {
        ModelQuery stripped = *q;
        stripped.prompt = prompt + out->promptSkip;
        out->tier = ClassifyRequest(&stripped, why, sizeof(why));
    }
    snprintf(out->name, sizeof(out->name), "%s", EnvOr(TIER_ENV[out->tier], TIER_MODELS[out->tier]));
    snprintf(out->reason, sizeof(out->reason), "%s", why);

    const char *fixed = EnvOr("MONOLITH_MODEL", NULL);
    if (fixed) {
        snprintf(out->name, sizeof(out->name), "%s", fixed);
        snprintf(out->reason, sizeof(out->reason), "MONOLITH_MODEL fixes the model (would be %s: %s)", TIER_NAMES[out->tier], why);
        out->source = "MONOLITH_MODEL";
    }
    // A --model (or --model=x) in MONOLITH_CLAUDE_ARGS wins; no second --model then
    ArgList args;
    ClaudeArgs(&args, &out->droppedModelArg, true);
    char **tok = args.tok;
    int n = args.count;
    for (int i = 0; i < n; i++) {
        const char *m = NULL;
        if (strcmp(tok[i], "--model") == 0 && i + 1 < n) m = tok[i + 1];
        else if (strncmp(tok[i], "--model=", 8) == 0) m = tok[i] + 8;
        if (!m) continue;
        snprintf(out->name, sizeof(out->name), "%s", m);
        snprintf(out->reason, sizeof(out->reason), "--model in MONOLITH_CLAUDE_ARGS (would be %s: %s)", TIER_NAMES[out->tier], why);
        out->source = "MONOLITH_CLAUDE_ARGS";
        out->addFlag = false;
        break;
    }
    FreeArgs(&args);
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
    float dist;         // nearest object (box or model) of the group
    int nearCount;      // objects of the group within NEARBY_RADIUS
    int firstLine, lastLine;    // over all objects of the group
} Nearby;

typedef struct {
    const DesignRequest *req;
    char id[128], slug[SLUG_MAX + 1], created[32];
    Vector3 fwd, right;
    float yawDeg, pitchDeg, aspect;
    bool hit;
    MS_ObjRef *sel;                     // valid selected objects (heap, like the tables below)
    int selCount;
    SelLine *selLines;
    int selLineCount, selLineCap;
    MS_ObjRef *selOrder;                // sel[] reordered so each line's objects are adjacent
    Nearby *nearby;                     // per group + one for objects without a group
    int nearbyCount;
    Vector3 nearbyCenter;
    char binary[1024], mapPath[1024], prompt[256];
    char package[1024];                 // folder of the map file
    char brief[1100];                   // <package>/MAP.md if it exists, else ""
    char binaryQ[2100], mapPathQ[2100];     // shell-quoted
    char shotCmd[6144];
    bool stale;                         // world changed between the snapshot and now
    ModelChoice model;                  // which Claude model runs it, and why
    const char *userPrompt;             // req->prompt without a leading @tier token
} Bundle;

static int CompareNearby(const void *a, const void *b)
{
    float d = ((const Nearby *)a)->dist - ((const Nearby *)b)->dist;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

static void FreeBundle(Bundle *B)
{
    free(B->sel);
    free(B->selLines);
    free(B->selOrder);
    free(B->nearby);
    free(B);
}

// False when out of memory
static bool BuildBundle(Bundle *B, const DesignRequest *req)
{
    B->req = req;
    B->stale = req->mapVersion != scene->version;
    B->yawDeg = Wrap(Deg(req->yaw), 0.0f, 360.0f);
    B->pitchDeg = Deg(req->pitch);
    B->fwd = (Vector3){ cosf(req->pitch) * sinf(req->yaw), sinf(req->pitch), cosf(req->pitch) * cosf(req->yaw) };
    B->right = (Vector3){ -cosf(req->yaw), 0.0f, sinf(req->yaw) };
    B->aspect = req->height > 0 ? (float)req->width / req->height : 0.0f;

    B->hit = ObjValid(req->hit.obj);

    // Selection: drop invalid references, then group by line
    int n = req->selectedCount > 0 ? req->selectedCount : 1;
    B->sel = malloc(sizeof(MS_ObjRef) * n);
    B->selOrder = malloc(sizeof(MS_ObjRef) * n);
    B->nearby = malloc(sizeof(Nearby) * (scene->groupCount + 1));
    if (!B->sel || !B->selOrder || !B->nearby) return false;
    for (int i = 0; i < req->selectedCount; i++)
        if (ObjValid(req->selected[i])) B->sel[B->selCount++] = req->selected[i];
    for (int i = 0; i < B->selCount; i++) {
        int line = ObjLine(B->sel[i]), k = 0;
        while (k < B->selLineCount && B->selLines[k].line != line) k++;
        if (k == B->selLineCount) {
            SelLine *sl = MS_PUSH(B->selLines, B->selLineCount, B->selLineCap);
            if (!sl) return false;
            *sl = (SelLine){ line, 0, 0 };
        }
        B->selLines[k].count++;
    }
    int at = 0;
    for (int k = 0; k < B->selLineCount; k++) {
        B->selLines[k].first = at;
        for (int i = 0; i < B->selCount; i++)
            if (ObjLine(B->sel[i]) == B->selLines[k].line) B->selOrder[at++] = B->sel[i];
    }

    // Nearby groups around the hit point (or the eye if the crosshair points at the sky)
    B->nearbyCenter = B->hit ? req->hit.point : req->eye;
    for (int g = 0; g <= scene->groupCount; g++)
        B->nearby[g] = (Nearby){ g < scene->groupCount ? g : -1, 1e30f, 0, 1 << 30, 0 };
    // (boxes and models: a model counts by its bounds)
    for (int i = 0; i < scene->boxCount + scene->modelCount; i++) {
        MS_ObjRef o = i < scene->boxCount ? (MS_ObjRef){ MS_OBJ_BOX, i } : (MS_ObjRef){ MS_OBJ_MODEL, i - scene->boxCount };
        int g = ObjGroup(o);
        Nearby *n = &B->nearby[(g >= 0 && g < scene->groupCount) ? g : scene->groupCount];
        float d = DistToBox(B->nearbyCenter, ObjBounds(o));
        if (d < n->dist) n->dist = d;
        if (d <= NEARBY_RADIUS) n->nearCount++;
        int line = ObjLine(o);
        if (line > 0 && line < n->firstLine) n->firstLine = line;
        if (line > n->lastLine) n->lastLine = line;
    }
    for (int g = 0; g <= scene->groupCount; g++)
        if (B->nearby[g].nearCount > 0) B->nearby[B->nearbyCount++] = B->nearby[g];
    qsort(B->nearby, B->nearbyCount, sizeof(Nearby), CompareNearby);

    BinaryPath(B->binary, sizeof(B->binary));
    AbsPath(scene->path, B->mapPath, sizeof(B->mapPath));
    snprintf(B->package, sizeof(B->package), "%s", GetDirectoryPath(B->mapPath));
    B->brief[0] = 0;                    // the runtime found <package>/MAP.md, or ""
    if (scene->brief[0]) AbsPath(scene->brief, B->brief, sizeof(B->brief));
    ShellQuote(B->binary, B->binaryQ, sizeof(B->binaryQ));
    ShellQuote(B->mapPath, B->mapPathQ, sizeof(B->mapPathQ));
    snprintf(B->shotCmd, sizeof(B->shotCmd), "%s --shot requests/%s/before.png --shot-size %d %d --eye %.6f %.6f %.6f --look %.6f %.6f --map %s",
             B->binaryQ, B->id, req->width, req->height, req->eye.x, req->eye.y, req->eye.z, B->yawDeg, B->pitchDeg, B->mapPathQ);
    snprintf(B->prompt, sizeof(B->prompt), "Map edit request: read requests/%s/request.md and carry it out.", B->id);
    return true;
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

// Map line `line` as a heap string (free it), NULL if there is none
static char *LineDup(int line)
{
    int len;
    const char *text = line > 0 ? MS_LineText(scene, line, &len) : NULL;
    return text ? MS_StrNDup(text, len) : NULL;
}

static void MdLine(FILE *f, int line)
{
    char *text = LineDup(line);
    if (text) {
        fprintf(f, "Map line %d:\n\n", line);
        MdFence(f, text, "");
    } else fprintf(f, "Not from a map line.\n\n");
    free(text);
}

// A model instance's file and transform, as list items
static void MdModel(FILE *f, int index, const char *bullet)
{
    const MS_ModelInstance *m = &scene->models[index];
    fprintf(f, "%smodel file: `%s` (package-relative)\n", bullet, scene->modelFiles[m->file].path);
    fprintf(f, "%sposition as written: ", bullet); MdVec(f, m->position);
    fprintf(f, ", origin `%s` (the file's origin is at ", ORIGIN_NAMES[m->origin % 3]); MdVec(f, m->translation);
    fprintf(f, ")\n%srotation yaw/pitch/roll (deg): ", bullet); MdVec(f, m->rotationDeg);
    fprintf(f, ", scale: "); MdVec(f, m->scale);
    fprintf(f, "\n%scollide `%s`, shadow %s", bullet, COLLIDE_NAMES[m->collide % 3], m->castShadow ? "on" : "off");
    for (int k = 0; k < m->slotCount; k++) {
        const MS_SlotMaterial *s = &scene->modelSlots[m->slotFirst + k];
        fprintf(f, "%s material.%d \"%s\"", k ? "," : ";", s->slot, scene->materials[s->material].name);
    }
    fprintf(f, "\n");
}

static void MdRegionObject(FILE *f, MS_ObjRef o)
{
    fprintf(f, "%s #%d, group \"%s\", line %d", ObjKindName(o), o.index, GroupName(ObjGroup(o)), ObjLine(o));
    if (o.kind == MS_OBJ_MODEL) fprintf(f, ", file `%s`", scene->modelFiles[scene->models[o.index].file].path);
    fprintf(f, ", material \"%s\"", DesignerMaterialLabel(o));
}

#define MD_REGION_ROWS 40   // objects / map lines per region in request.md (request.json lists them all)
#define MD_EDGE_ROWS 20     // edge objects per region in request.md

// A region's objects summed per map line: one `repeat` line can be dozens of small objects
// (stair steps, roof beams) whose shares only make sense together
typedef struct { int line; MS_ObjRef first; int objects; float share; } LineShare;

static int CompareLineShare(const void *pa, const void *pb)
{
    const LineShare *a = pa, *b = pb;
    if (a->share != b->share) return a->share > b->share ? -1 : 1;
    return a->line - b->line;
}

// Heap array (free it) of the region's map lines, largest share first; NULL with *count 0
// when the region has no objects or memory ran out
static LineShare *RegionLines(const DesignRegionInfo *g, int *count)
{
    *count = 0;
    LineShare *ls = g->objectCount > 0 ? malloc(sizeof(LineShare) * g->objectCount) : NULL;
    if (!ls) return NULL;
    int n = 0;
    for (int k = 0; k < g->objectCount; k++) {
        MS_ObjRef o = g->objects[k].obj;
        if (!ObjValid(o)) continue;
        int line = ObjLine(o), j = 0;
        while (j < n && ls[j].line != line) j++;
        if (j == n) ls[n++] = (LineShare){ line, o, 0, 0.0f };
        ls[j].objects++;
        ls[j].share += g->objects[k].share;
    }
    if (n > 1) qsort(ls, n, sizeof(ls[0]), CompareLineShare);
    *count = n;
    return ls;
}

static void MdSides(FILE *f, unsigned int sides)
{
    static const char *const NAMES[] = { "left", "right", "top", "bottom" };
    int n = 0;
    for (int k = 0; k < 4; k++)
        if (sides & (1u << k)) fprintf(f, "%s%s", n++ ? ", " : "", NAMES[k]);
}

static void WriteRegionsMd(FILE *f, const Bundle *B)
{
    const DesignRequest *r = B->req;
    fprintf(f, "## Screen regions (%d)\n\n", r->regionCount);
    if (r->regionCount == 0) {
        fprintf(f, "None: no region was dragged out.\n\n");
        return;
    }
    fprintf(f, "The developer dragged these rectangles over the frame (snip mode). Rects are pixels of "
               "`view.png` (x, y of the top-left corner, then width x height); `view-marked.png` outlines and "
               "numbers them. *Objects* are what the region shows, from a regular grid of rays through it: "
               "share = fraction of the rays that hit that object first, sky = fraction that hit nothing. "
               "Depth = distance from the eye over the rays that hit something. *By map line* adds the shares "
               "up per map line (one `repeat` line can be many small objects). *Edge objects* are what rays "
               "just outside the rect's border hit, nearest first: for a gap or an opening the region looks "
               "through, these bound the gap, while the region's own objects are what is seen through it. "
               "Computed at submit time.\n\n");
    for (int i = 0; i < r->regionCount; i++) {
        const DesignRegionInfo *g = &r->regions[i];
        fprintf(f, "### Region %d (`region-%d.png`)\n\n", i + 1, i + 1);
        fprintf(f, "| | |\n|---|---|\n");
        fprintf(f, "| rect | x %.0f, y %.0f, %.0f x %.0f px (normalised %.4f, %.4f, %.4f, %.4f) |\n",
                g->rect.x, g->rect.y, g->rect.width, g->rect.height, g->rect.x / r->width, g->rect.y / r->height,
                g->rect.width / r->width, g->rect.height / r->height);
        fprintf(f, "| centre | ");
        if (ObjValid(g->centerObj)) {
            MdRegionObject(f, g->centerObj);
            fprintf(f, "; point "); MdVec(f, g->centerPoint);
            fprintf(f, ", normal "); MdVec(f, g->centerNormal);
            fprintf(f, ", %.2f m |\n", g->centerDist);
        } else fprintf(f, "sky (no object under the centre) |\n");
        if (g->hasDepth) fprintf(f, "| depth | %.2f - %.2f m |\n", g->depthMin, g->depthMax);
        else fprintf(f, "| depth | - (only sky) |\n");
        fprintf(f, "| sky | %.1f%% |\n", 100.0f * g->sky);
        fprintf(f, "| rays | %d x %d |\n\n", g->gridW, g->gridH);

        // Edge objects first: they matter most when the region is all sky or far away
        if (g->edgeSamples > 0) {
            fprintf(f, "Edge objects (%d rays %.0f px outside the border, sky %.1f%%), nearest first:\n\n",
                    g->edgeSamples, g->edgeOffset, 100.0f * g->edgeSky);
            if (g->edgeCount == 0) fprintf(f, "None: only sky just outside the region.\n\n");
            else {
                fprintf(f, "| distance | object | group | line | sides | rays | nearest point |\n|---|---|---|---|---|---|---|\n");
                int rows = g->edgeCount < MD_EDGE_ROWS ? g->edgeCount : MD_EDGE_ROWS;
                for (int k = 0; k < rows; k++) {
                    const DesignRegionEdge *e = &g->edges[k];
                    if (!ObjValid(e->obj)) continue;
                    fprintf(f, "| %.2f m | %s #%d", e->dist, ObjKindName(e->obj), e->obj.index);
                    if (e->obj.kind == MS_OBJ_MODEL) fprintf(f, " `%s`", scene->modelFiles[scene->models[e->obj.index].file].path);
                    fprintf(f, " | %s | %d | ", GroupName(ObjGroup(e->obj)), ObjLine(e->obj));
                    MdSides(f, e->sides);
                    fprintf(f, " | %d | ", e->samples);
                    MdVec(f, e->point);
                    fprintf(f, " |\n");
                }
                if (g->edgeCount > rows) fprintf(f, "| | %d more (see `request.json`) | | | | | |\n", g->edgeCount - rows);
                fprintf(f, "\n");
            }
        } else fprintf(f, "Edge objects: none (the region fills the frame to its borders).\n\n");

        if (g->objectCount == 0) { fprintf(f, "No objects in this region.\n\n"); continue; }
        int lineCount = 0;
        LineShare *ls = RegionLines(g, &lineCount);
        if (ls && lineCount > 0) {
            fprintf(f, "By map line:\n\n| share | line | group | objects |\n|---|---|---|---|\n");
            int rows = lineCount < MD_REGION_ROWS ? lineCount : MD_REGION_ROWS;
            for (int k = 0; k < rows; k++) {
                fprintf(f, "| %.1f%% | %d | %s | %d %s%s", 100.0f * ls[k].share, ls[k].line, GroupName(ObjGroup(ls[k].first)),
                        ls[k].objects, ObjKindName(ls[k].first), ls[k].objects == 1 ? "" : ls[k].first.kind == MS_OBJ_BOX ? "es" : "s");
                if (ls[k].first.kind == MS_OBJ_MODEL) fprintf(f, " (`%s`)", scene->modelFiles[scene->models[ls[k].first.index].file].path);
                fprintf(f, " |\n");
            }
            if (lineCount > rows) fprintf(f, "| | %d more lines (see `request.json`) | | |\n", lineCount - rows);
            fprintf(f, "\n");
        }
        fprintf(f, "Objects:\n\n| share | object | group | line | min | max |\n|---|---|---|---|---|---|\n");
        int rows = g->objectCount < MD_REGION_ROWS ? g->objectCount : MD_REGION_ROWS;
        for (int k = 0; k < rows; k++) {
            MS_ObjRef o = g->objects[k].obj;
            if (!ObjValid(o)) continue;
            BoundingBox b = ObjBounds(o);
            fprintf(f, "| %.1f%% | %s #%d | %s | %d | ", 100.0f * g->objects[k].share, ObjKindName(o), o.index,
                    GroupName(ObjGroup(o)), ObjLine(o));
            MdVec(f, b.min); fprintf(f, " | "); MdVec(f, b.max); fprintf(f, " |\n");
        }
        int more = g->objectCount - rows + g->omittedObjects;
        if (more > 0) {
            float share = g->omittedShare;
            for (int k = rows; k < g->objectCount; k++) share += g->objects[k].share;
            fprintf(f, "| %.1f%% | %d more object%s (see `request.json`) | | | | |\n", 100.0f * share, more, more == 1 ? "" : "s");
        }
        fprintf(f, "\n");

        // The map lines of the region, largest share first, each quoted once (a heap text:
        // lines have no length limit)
        int quoted = 0, n = 0, cap = 0;
        char *block = NULL;
        for (int k = 0; ls && k < lineCount && k < MD_REGION_ROWS; k++) {
            int line = ls[k].line, len;
            const char *text = line > 0 ? MS_LineText(scene, line, &len) : NULL;
            if (!text) continue;
            quoted++;
            char num[16];
            int nl = snprintf(num, sizeof(num), "%d: ", line);
            if (!MS_RESERVE(block, cap, n + nl + len + 2)) break;
            memcpy(block + n, num, nl);
            memcpy(block + n + nl, text, len);
            n += nl + len;
            block[n++] = '\n';
            block[n] = 0;
        }
        if (quoted > 0 && block) {
            fprintf(f, "Map lines of these objects:\n\n");
            MdFence(f, block, "");
        }
        free(block);
        free(ls);
    }
}

#define VOLUME_JSON_OBJECTS 10000   // objects per volume in request.json (sanity cap; the
                                    // lines[] and groups[] summaries always cover all of them)

// A volume's objects summed per map line or per group (heap, free it): largest summed
// shareOfVolume first
typedef struct { int key; MS_ObjRef first; int objects, inside; float share; } VolumeTally;

static int CompareTallyKey(const void *pa, const void *pb)
{
    const VolumeTally *a = pa, *b = pb;
    return a->key - b->key;
}

static int CompareTallyShare(const void *pa, const void *pb)
{
    const VolumeTally *a = pa, *b = pb;
    if (a->share != b->share) return a->share > b->share ? -1 : 1;
    return a->key - b->key;
}

// byGroup false: per map line (key = line); true: per group (key = group index, -1 = none).
// Sorted by key, then merged, so thousands of objects stay cheap.
static VolumeTally *VolumeTallies(const DesignVolumeInfo *v, bool byGroup, int *count)
{
    *count = 0;
    VolumeTally *t = v->objectCount > 0 ? malloc(sizeof(VolumeTally) * v->objectCount) : NULL;
    if (!t) return NULL;
    int n = 0;
    for (int k = 0; k < v->objectCount; k++) {
        MS_ObjRef o = v->objects[k].obj;
        if (!ObjValid(o)) continue;
        t[n++] = (VolumeTally){ byGroup ? ObjGroup(o) : ObjLine(o), o, 1, v->objects[k].fullyInside, v->objects[k].shareOfVolume };
    }
    // (first: any object of the key; one map line makes one kind of object)
    if (n > 1) qsort(t, n, sizeof(t[0]), CompareTallyKey);
    int m = 0;
    for (int k = 0; k < n; k++) {
        if (m > 0 && t[m - 1].key == t[k].key) {
            t[m - 1].objects++;
            t[m - 1].inside += t[k].inside;
            t[m - 1].share += t[k].share;
        } else t[m++] = t[k];
    }
    if (m > 1) qsort(t, m, sizeof(t[0]), CompareTallyShare);
    *count = m;
    return t;
}

static void WriteVolumesMd(FILE *f, const Bundle *B)
{
    const DesignRequest *r = B->req;
    fprintf(f, "## Volumes (%d)\n\n", r->volumeCount);
    if (r->volumeCount == 0) {
        fprintf(f, "None: no volume was marked.\n\n");
        return;
    }
    fprintf(f, "The developer marked these axis-aligned boxes in world space (map coordinates, metres, +Y up); "
               "`view-marked.png` draws them in green and labels them V1, V2, ... \"This volume\", \"in here\" or "
               "\"V2\" in the prompt mean such a box: the space itself, which may be hidden from the camera or "
               "empty. *Objects* are every box and model whose world bounds overlap the volume: *of volume* = the "
               "overlap / the volume, *of object* = the overlap / the object's bounds, *inside* = the object lies "
               "entirely in the volume. *Empty* = the share of the volume outside every object's bounds, over a "
               "grid of cells (models count by their bounds, overlapping objects twice). *Floor* = the first surface straight down from the "
               "volume's centre. Computed at submit time.\n\n");
    for (int i = 0; i < r->volumeCount; i++) {
        const DesignVolumeInfo *v = &r->volumes[i];
        Vector3 size = Vector3Subtract(v->box.max, v->box.min);
        fprintf(f, "### Volume V%d\n\n", i + 1);
        fprintf(f, "| | |\n|---|---|\n");
        fprintf(f, "| min | "); MdVec(f, v->box.min); fprintf(f, " |\n");
        fprintf(f, "| max | "); MdVec(f, v->box.max); fprintf(f, " |\n");
        fprintf(f, "| centre | "); MdVec(f, Vector3Scale(Vector3Add(v->box.min, v->box.max), 0.5f)); fprintf(f, " |\n");
        fprintf(f, "| size | %.3f x %.3f x %.3f m (%.2f m^3), snap %g m |\n", size.x, size.y, size.z, size.x * size.y * size.z, v->snap);
        if (isfinite(v->empty)) fprintf(f, "| empty | %.1f%% (%d x %d x %d cells) |\n", 100.0f * v->empty, v->grid[0], v->grid[1], v->grid[2]);
        else fprintf(f, "| empty | unknown (out of memory) |\n");
        fprintf(f, "| floor | ");
        if (ObjValid(v->floor.obj)) {
            fprintf(f, "y %.3f (%.3f m below the volume's bottom): ", v->floor.point.y, v->box.min.y - v->floor.point.y);
            MdRegionObject(f, v->floor.obj);
            fprintf(f, " |\n\n");
        } else fprintf(f, "nothing below the centre |\n\n");

        if (v->objectCount == 0) { fprintf(f, "No objects overlap this volume: it is empty space.\n\n"); continue; }

        int groupCount = 0, lineCount = 0;
        VolumeTally *groups = VolumeTallies(v, true, &groupCount);
        VolumeTally *lines = VolumeTallies(v, false, &lineCount);
        if (groups && groupCount > 0) {
            fprintf(f, "Groups it touches: ");
            for (int k = 0; k < groupCount; k++)
                fprintf(f, "%s\"%s\" (%d object%s, %d inside)", k ? ", " : "", GroupName(groups[k].key), groups[k].objects,
                        groups[k].objects == 1 ? "" : "s", groups[k].inside);
            fprintf(f, ".\n\n");
        }
        if (lines && lineCount > 0) {
            fprintf(f, "By map line:\n\n| of volume | line | group | objects | inside |\n|---|---|---|---|---|\n");
            int rows = lineCount < MD_REGION_ROWS ? lineCount : MD_REGION_ROWS;
            for (int k = 0; k < rows; k++) {
                const VolumeTally *t = &lines[k];
                fprintf(f, "| %.2f%% | %d | %s | %d %s%s", 100.0f * t->share, t->key, GroupName(ObjGroup(t->first)), t->objects,
                        ObjKindName(t->first), t->objects == 1 ? "" : t->first.kind == MS_OBJ_BOX ? "es" : "s");
                if (t->first.kind == MS_OBJ_MODEL) fprintf(f, " (`%s`)", scene->modelFiles[scene->models[t->first.index].file].path);
                fprintf(f, " | %d |\n", t->inside);
            }
            if (lineCount > rows) fprintf(f, "| | %d more lines (see `request.json`) | | | |\n", lineCount - rows);
            fprintf(f, "\n");
        }
        fprintf(f, "Objects:\n\n| of volume | of object | inside | object | group | line | min | max |\n|---|---|---|---|---|---|---|---|\n");
        int rows = v->objectCount < MD_REGION_ROWS ? v->objectCount : MD_REGION_ROWS;
        for (int k = 0; k < rows; k++) {
            const DesignVolumeObject *vo = &v->objects[k];
            if (!ObjValid(vo->obj)) continue;
            BoundingBox b = ObjBounds(vo->obj);
            fprintf(f, "| %.2f%% | %.1f%% | %s | %s #%d", 100.0f * vo->shareOfVolume, 100.0f * vo->shareOfObject,
                    vo->fullyInside ? "yes" : "no", ObjKindName(vo->obj), vo->obj.index);
            if (vo->obj.kind == MS_OBJ_MODEL) fprintf(f, " `%s`", scene->modelFiles[scene->models[vo->obj.index].file].path);
            fprintf(f, " | %s | %d | ", GroupName(ObjGroup(vo->obj)), ObjLine(vo->obj));
            MdVec(f, b.min); fprintf(f, " | "); MdVec(f, b.max); fprintf(f, " |\n");
        }
        if (v->objectCount > rows) fprintf(f, "| | | | %d more object%s (see `request.json`) | | | | |\n", v->objectCount - rows,
                                           v->objectCount - rows == 1 ? "" : "s");
        fprintf(f, "\n");

        // The map lines, largest share first, each quoted once
        int quoted = 0, n = 0, cap = 0;
        char *block = NULL;
        for (int k = 0; lines && k < lineCount && k < MD_REGION_ROWS; k++) {
            int line = lines[k].key, len;
            const char *text = line > 0 ? MS_LineText(scene, line, &len) : NULL;
            if (!text) continue;
            quoted++;
            char num[16];
            int nl = snprintf(num, sizeof(num), "%d: ", line);
            if (!MS_RESERVE(block, cap, n + nl + len + 2)) break;
            memcpy(block + n, num, nl);
            memcpy(block + n + nl, text, len);
            n += nl + len;
            block[n++] = '\n';
            block[n] = 0;
        }
        if (quoted > 0 && block) {
            fprintf(f, "Map lines of these objects:\n\n");
            MdFence(f, block, "");
        }
        free(block);
        free(groups);
        free(lines);
    }
}

static bool WriteRequestMd(const char *path, const Bundle *B)
{
    const DesignRequest *r = B->req;
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    fprintf(f, "# Map edit request: %s\n\n", B->slug);
    fprintf(f, "Model: %s (%s tier: %s)\n\n", B->model.name, ModelTierName(B->model.tier), B->model.reason);
    fprintf(f, "Request `%s`, submitted %s from the monolith map designer. Bundle format 4: besides the "
               "crosshair hit and the selected objects it may contain **screen regions** the developer "
               "dragged over the frame and **volumes**, boxes marked in world space (see *Screen regions* and "
               "*Volumes* below and `.claude/skills/map-request/bundle-reference.md`).\n\n", B->id, B->created);
    fprintf(f, "**First** read `CLAUDE.md` and the map-request skill `.claude/skills/map-request/SKILL.md` in the repository root (if they "
               "are not loaded already); they explain the workflow, the map format and how to verify an edit. "
               "Then carry out the request below.\n\n"
               "**When you are done**, write `requests/%s/result.md`. Its **first line is a one-line "
               "summary** (the game shows it in its HUD), followed by what changed (map lines before -> "
               "after), the paths of your after-image(s), and anything left open. If the request is "
               "genuinely ambiguous, ask the developer instead of guessing (the map-request skill, section 3, says how).\n\n", B->id);
    if (B->stale)
        fprintf(f, "**Warning:** the map was reloaded between the selection and the submit; box indices "
                   "below may not match what the user saw.\n\n");

    fprintf(f, "## Prompt\n\nThe user's words, verbatim:\n\n");
    MdFence(f, B->userPrompt, "text");

    fprintf(f, "## Design brief\n\n");
    if (B->brief[0]) fprintf(f, "Read `%s` before anything else: it is this map's design brief (style, scale, "
                                "materials, rules for new content).\n\n", B->brief);
    else fprintf(f, "This map has no design brief (no `MAP.md` next to the map file).\n\n");

    fprintf(f, "## Camera\n\n");
    fprintf(f, "- eye: "); MdVec(f, r->eye); fprintf(f, "\n");
    fprintf(f, "- yaw %.2f deg, pitch %.2f deg. Convention: yaw 0 looks toward +Z, yaw 180 toward -Z, "
               "yaw 90 toward +X; LookDir = (cos p * sin y, sin p, cos p * cos y)\n", B->yawDeg, B->pitchDeg);
    fprintf(f, "- forward: "); MdVec(f, B->fwd); fprintf(f, "\n");
    fprintf(f, "- right: "); MdVec(f, B->right); fprintf(f, " = (-cos yaw, 0, sin yaw); up is +Y\n");
    fprintf(f, "- fovy %.1f deg (vertical), aspect %.4f, resolution %d x %d\n\n", r->fovy, B->aspect, r->width, r->height);
    fprintf(f, "Images in this folder: `view.png` (the frame without HUD) and `view-marked.png` (the same "
               "frame with the selection highlighted, the crosshair hit marked, the screen regions "
               "outlined and numbered and the volumes drawn as green boxes labelled V1, V2, ...)%s.\n\n", r->regionCount ? ", plus one crop `region-<n>.png` per region" : "");

    fprintf(f, "## Crosshair hit\n\n");
    if (B->hit) {
        MS_ObjRef o = r->hit.obj;
        BoundingBox b = ObjBounds(o);
        fprintf(f, "- point: "); MdVec(f, r->hit.point); fprintf(f, "\n");
        fprintf(f, "- face normal: "); MdVec(f, r->hit.normal); fprintf(f, "\n");
        fprintf(f, "- distance from the eye: %.2f m\n", r->hit.dist);
        fprintf(f, "- object: %s #%d (kind `%s`, index %d): %smin ", ObjKindName(o), o.index, ObjKindName(o), o.index,
                o.kind == MS_OBJ_MODEL ? "world bounds " : "");
        MdVec(f, b.min); fprintf(f, ", max "); MdVec(f, b.max); fprintf(f, "\n");
        if (o.kind == MS_OBJ_MODEL) MdModel(f, o.index, "- ");
        const MS_TurnedBox *t = ObjTurn(o);
        if (t) fprintf(f, "- turned: `rot %g %g %g` (yaw, pitch, roll in degrees, about its centre); min / max are its world "
                          "bounds\n", t->rotationDeg.x, t->rotationDeg.y, t->rotationDeg.z);
        fprintf(f, "- copy: ");
        MdCopy(f, o);
        fprintf(f, " among the %s%s of map line %d (0-based", ObjKindName(o), o.kind == MS_OBJ_BOX ? "es" : "s", ObjLine(o));
        MdRepeats(f, o);
        fprintf(f, ")\n");
        fprintf(f, "- group: \"%s\", material \"%s\"\n\n", GroupName(ObjGroup(o)), DesignerMaterialLabel(o));
        MdLine(f, ObjLine(o));
    } else {
        fprintf(f, "Nothing: the crosshair points at the sky (no object hit).\n\n");
    }

    int selModels = 0;
    for (int i = 0; i < B->selCount; i++) selModels += B->sel[i].kind == MS_OBJ_MODEL;
    fprintf(f, "## Selection (%d box%s, %d model%s)\n\n", B->selCount - selModels, B->selCount - selModels == 1 ? "" : "es",
            selModels, selModels == 1 ? "" : "s");
    if (B->selCount == 0) fprintf(f, "Nothing is selected.\n\n");
    for (int k = 0; k < B->selLineCount; k++) {
        const SelLine *sl = &B->selLines[k];
        MS_ObjRef first = B->selOrder[sl->first];
        bool model = first.kind == MS_OBJ_MODEL;
        fprintf(f, "### Line %d, group \"%s\", material \"%s\", %d %s%s\n\n", sl->line, GroupName(ObjGroup(first)),
                DesignerMaterialLabel(first), sl->count, model ? "model" : "box",
                sl->count == 1 ? "" : model ? "s" : "es");
        MdLine(f, sl->line);
        fprintf(f, "*copy* = the object's index among the line's objects (0-based) of their total");
        MdRepeats(f, first);
        fprintf(f, ".\n\n");
        if (model) fprintf(f, "| model | copy | file | position | rotation (yaw pitch roll) | scale | world min | world max |\n"
                              "|---|---|---|---|---|---|---|---|\n");
        bool turned = false;    // a line with turned boxes gets a rotation column (min / max: world bounds)
        for (int i = sl->first; i < sl->first + sl->count && !model; i++) turned |= ObjTurn(B->selOrder[i]) != NULL;
        if (turned) fprintf(f, "Turned boxes: min, max, centre and size are their world bounds; `rot` turns the box as written "
                               "about its centre.\n\n| box | copy | min | max | centre | size | rot (yaw pitch roll) |\n"
                               "|---|---|---|---|---|---|---|\n");
        else if (!model) fprintf(f, "| box | copy | min | max | centre | size |\n|---|---|---|---|---|---|\n");
        for (int i = sl->first; i < sl->first + sl->count; i++) {
            MS_ObjRef o = B->selOrder[i];
            BoundingBox b = ObjBounds(o);
            if (o.kind == MS_OBJ_MODEL) {
                const MS_ModelInstance *m = &scene->models[o.index];
                fprintf(f, "| #%d | ", o.index);
                MdCopy(f, o);
                fprintf(f, " | `%s` | ", scene->modelFiles[m->file].path); MdVec(f, m->position);
                fprintf(f, " | "); MdVec(f, m->rotationDeg);
                fprintf(f, " | "); MdVec(f, m->scale);
                fprintf(f, " | "); MdVec(f, b.min);
                fprintf(f, " | "); MdVec(f, b.max);
                fprintf(f, " |\n");
                continue;
            }
            fprintf(f, "| #%d | ", o.index);
            MdCopy(f, o);
            fprintf(f, " | "); MdVec(f, b.min);
            fprintf(f, " | "); MdVec(f, b.max);
            fprintf(f, " | "); MdVec(f, Vector3Scale(Vector3Add(b.min, b.max), 0.5f));
            fprintf(f, " | "); MdVec(f, Vector3Subtract(b.max, b.min));
            const MS_TurnedBox *t = ObjTurn(o);
            if (t) fprintf(f, " | `rot %g %g %g`", t->rotationDeg.x, t->rotationDeg.y, t->rotationDeg.z);
            else if (turned) fprintf(f, " | -");
            fprintf(f, " |\n");
        }
        fprintf(f, "\n");
    }

    WriteRegionsMd(f, B);
    WriteVolumesMd(f, B);

    fprintf(f, "## Nearby\n\nGroups with an object (box or model) within %.0f m of the %s ", NEARBY_RADIUS, B->hit ? "hit point" : "eye");
    MdVec(f, B->nearbyCenter);
    fprintf(f, ", nearest first. The map lines are the range over the whole group.\n\n");
    fprintf(f, "| group | nearest object | objects within %.0f m | map lines |\n|---|---|---|---|\n", NEARBY_RADIUS);
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
    fprintf(f, "- file: `%s` (format %d)\n", B->mapPath, MAP_FORMAT_VERSION);
    if (B->brief[0]) fprintf(f, "- package (folder): `%s`; design brief: `%s`\n", B->package, B->brief);
    else fprintf(f, "- package (folder): `%s`; design brief: none\n", B->package);
    char when[32] = "?";
    time_t mt = (time_t)scene->mtime;
    struct tm *tm = localtime(&mt);
    if (tm) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", tm);
    fprintf(f, "- at submit: FNV-1a 64 hash `%016llx`, mtime %ld (%s), load #%u, %d boxes, %d models, %d groups, %d material%s\n\n",
            scene->hash, scene->mtime, when, scene->version, scene->boxCount, scene->modelCount, scene->groupCount, MAP_MATERIALS, MAP_MATERIALS == 1 ? "" : "s");
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
    char *text = LineDup(line);
    if (text) JsonStr(f, text);
    else fputs("null", f);
    free(text);
}

static void JsonMaterial(FILE *f, MS_ObjRef o)
{
    if (ObjMaterial(o)) JsonStr(f, ObjMaterial(o));
    else fputs("null", f);
}

// A model instance (.claude/skills/map-request/bundle-reference.md): file, transform as written, the resulting
// translation, origin, collision, shadow, material overrides
static void JsonModel(FILE *f, int index)
{
    const MS_ModelInstance *m = &scene->models[index];
    fprintf(f, "{ \"file\": "); JsonStr(f, scene->modelFiles[m->file].path);
    fprintf(f, ", \"position\": "); JsonVec(f, m->position);
    fprintf(f, ", \"rotationDeg\": "); JsonVec(f, m->rotationDeg);
    fprintf(f, ", \"scale\": "); JsonVec(f, m->scale);
    fprintf(f, ", \"origin\": \"%s\", \"translation\": ", ORIGIN_NAMES[m->origin % 3]); JsonVec(f, m->translation);
    fprintf(f, ", \"collide\": \"%s\", \"shadow\": %s, \"slotMaterials\": [", COLLIDE_NAMES[m->collide % 3], m->castShadow ? "true" : "false");
    for (int k = 0; k < m->slotCount; k++) {
        const MS_SlotMaterial *s = &scene->modelSlots[m->slotFirst + k];
        fprintf(f, "%s{ \"slot\": %d, \"material\": ", k ? ", " : "", s->slot);
        JsonStr(f, scene->materials[s->material].name);
        fprintf(f, " }");
    }
    fprintf(f, "] }");
}

static void WriteVolumesJson(FILE *f, const DesignRequest *r)
{
    fprintf(f, ",\n  \"volumes\": [");
    for (int i = 0; i < r->volumeCount; i++) {
        const DesignVolumeInfo *v = &r->volumes[i];
        Vector3 size = Vector3Subtract(v->box.max, v->box.min);
        fprintf(f, "%s\n    {\n      \"index\": %d,\n      \"label\": \"V%d\",\n      \"min\": ", i ? "," : "", i + 1, i + 1);
        JsonVec(f, v->box.min);
        fprintf(f, ",\n      \"max\": "); JsonVec(f, v->box.max);
        fprintf(f, ",\n      \"center\": "); JsonVec(f, Vector3Scale(Vector3Add(v->box.min, v->box.max), 0.5f));
        fprintf(f, ",\n      \"size\": "); JsonVec(f, size);
        fprintf(f, ",\n      \"cubicMetres\": "); JsonNum(f, size.x * size.y * size.z);
        fprintf(f, ",\n      \"snap\": "); JsonNum(f, v->snap);
        fprintf(f, ",\n      \"empty\": "); JsonNum(f, v->empty);
        fprintf(f, ",\n      \"emptyGrid\": [%d, %d, %d]", v->grid[0], v->grid[1], v->grid[2]);
        fprintf(f, ",\n      \"emptyMethod\": \"per cell of emptyGrid, the overlap of every object's bounds (models: world AABB) with the cell, summed and capped at the cell\"");
        fprintf(f, ",\n      \"floor\": ");
        if (ObjValid(v->floor.obj)) {
            MS_ObjRef o = v->floor.obj;
            fprintf(f, "{ \"y\": "); JsonNum(f, v->floor.point.y);
            fprintf(f, ", \"below\": "); JsonNum(f, v->box.min.y - v->floor.point.y);
            fprintf(f, ", \"point\": "); JsonVec(f, v->floor.point);
            fprintf(f, ", \"kind\": \"%s\", \"index\": %d, \"line\": %d, \"group\": ", ObjKindName(o), o.index, ObjLine(o));
            JsonStr(f, GroupName(ObjGroup(o)));
            fprintf(f, " }");
        } else fprintf(f, "null");
        int listed = 0;
        fprintf(f, ",\n      \"objects\": [");
        for (int k = 0; k < v->objectCount && listed < VOLUME_JSON_OBJECTS; k++) {
            const DesignVolumeObject *vo = &v->objects[k];
            MS_ObjRef o = vo->obj;
            if (!ObjValid(o)) continue;
            BoundingBox b = ObjBounds(o);
            fprintf(f, "%s\n        { \"kind\": \"%s\", \"index\": %d", listed++ ? "," : "", ObjKindName(o), o.index);
            if (o.kind == MS_OBJ_BOX) fprintf(f, ", \"box\": %d", o.index);
            fprintf(f, ", \"line\": %d, \"group\": ", ObjLine(o)); JsonStr(f, GroupName(ObjGroup(o)));
            if (o.kind == MS_OBJ_MODEL) { fprintf(f, ", \"file\": "); JsonStr(f, scene->modelFiles[scene->models[o.index].file].path); }
            fprintf(f, ", \"material\": "); JsonMaterial(f, o);
            fprintf(f, ", \"shareOfVolume\": "); JsonNum(f, vo->shareOfVolume);
            fprintf(f, ", \"shareOfObject\": "); JsonNum(f, vo->shareOfObject);
            fprintf(f, ", \"fullyInside\": %s", vo->fullyInside ? "true" : "false");
            fprintf(f, ", \"min\": "); JsonVec(f, b.min);
            fprintf(f, ", \"max\": "); JsonVec(f, b.max);
            fprintf(f, " }");
        }
        fprintf(f, "%s],\n      \"objectCount\": %d,\n      \"omittedObjects\": %d", listed ? "\n      " : "",
                v->objectCount, v->objectCount - listed);
        int lineCount = 0, groupCount = 0;
        VolumeTally *lines = VolumeTallies(v, false, &lineCount);
        VolumeTally *groups = VolumeTallies(v, true, &groupCount);
        fprintf(f, ",\n      \"lines\": [");
        for (int k = 0; k < lineCount; k++) {
            const VolumeTally *t = &lines[k];
            fprintf(f, "%s\n        { \"line\": %d, \"lineText\": ", k ? "," : "", t->key);
            JsonLineText(f, t->key);
            fprintf(f, ", \"group\": "); JsonStr(f, GroupName(ObjGroup(t->first)));
            fprintf(f, ", \"kind\": \"%s\", \"objects\": %d, \"fullyInside\": %d, \"shareOfVolume\": ", ObjKindName(t->first), t->objects, t->inside);
            JsonNum(f, t->share);
            fprintf(f, " }");
        }
        fprintf(f, "%s],\n      \"groups\": [", lineCount ? "\n      " : "");
        for (int k = 0; k < groupCount; k++) {
            const VolumeTally *t = &groups[k];
            fprintf(f, "%s\n        { \"group\": ", k ? "," : "");
            JsonStr(f, GroupName(t->key));
            fprintf(f, ", \"objects\": %d, \"fullyInside\": %d, \"shareOfVolume\": ", t->objects, t->inside);
            JsonNum(f, t->share);
            fprintf(f, " }");
        }
        fprintf(f, "%s]", groupCount ? "\n      " : "");
        free(lines);
        free(groups);
        fprintf(f, "\n    }");
    }
    fprintf(f, "%s]", r->volumeCount ? "\n  " : "");
}

static bool WriteRequestJson(const char *path, const Bundle *B)
{
    const DesignRequest *r = B->req;
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    fprintf(f, "{\n  \"format\": 4,\n  \"kind\": \"edit\",\n  \"id\": "); JsonStr(f, B->id);
    fprintf(f, ",\n  \"slug\": "); JsonStr(f, B->slug);
    fprintf(f, ",\n  \"created\": "); JsonStr(f, B->created);
    fprintf(f, ",\n  \"instructions\": ");
    JsonStr(f, TextFormat("Read CLAUDE.md and .claude/skills/map-request/SKILL.md in the repository root first, then carry out the "
                          "prompt. When done, write requests/%s/result.md; its first line is a one-line summary.", B->id));
    fprintf(f, ",\n  \"resultFile\": "); JsonStr(f, TextFormat("requests/%s/result.md", B->id));
    fprintf(f, ",\n  \"mapReloadedSinceSnapshot\": %s", B->stale ? "true" : "false");
    fprintf(f, ",\n  \"prompt\": "); JsonStr(f, B->userPrompt);
    fprintf(f, ",\n  \"model\": { \"tier\": \"%s\", \"name\": ", ModelTierName(B->model.tier)); JsonStr(f, B->model.name);
    fprintf(f, ", \"reason\": "); JsonStr(f, B->model.reason);
    fprintf(f, ", \"override\": %s, \"source\": \"%s\" }", B->model.override ? "true" : "false", B->model.source);

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
    fprintf(f, ",\n  \"images\": { \"view\": \"view.png\", \"marked\": \"view-marked.png\", \"regions\": [");
    for (int i = 0; i < r->regionCount; i++) fprintf(f, "%s\"region-%d.png\"", i ? ", " : "", i + 1);
    fprintf(f, "] }");

    fprintf(f, ",\n  \"hit\": ");
    if (B->hit) {
        MS_ObjRef o = r->hit.obj;
        BoundingBox b = ObjBounds(o);
        fprintf(f, "{\n    \"kind\": \"%s\",\n    \"index\": %d", ObjKindName(o), o.index);
        fprintf(f, ",\n    \"material\": "); JsonMaterial(f, o);
        fprintf(f, ",\n    \"point\": "); JsonVec(f, r->hit.point);
        fprintf(f, ",\n    \"normal\": "); JsonVec(f, r->hit.normal);
        fprintf(f, ",\n    \"distance\": "); JsonNum(f, r->hit.dist);
        if (o.kind == MS_OBJ_BOX) fprintf(f, ",\n    \"box\": %d", o.index);
        fprintf(f, ",\n    \"min\": "); JsonVec(f, b.min);
        fprintf(f, ",\n    \"max\": "); JsonVec(f, b.max);
        if (ObjTurn(o)) { fprintf(f, ",\n    \"rotationDeg\": "); JsonVec(f, ObjTurn(o)->rotationDeg); }
        if (o.kind == MS_OBJ_MODEL) { fprintf(f, ",\n    \"model\": "); JsonModel(f, o.index); }
        fprintf(f, ",\n    \"group\": "); JsonStr(f, GroupName(ObjGroup(o)));
        fprintf(f, ",\n    "); JsonCopy(f, o);
        fprintf(f, ",\n    \"line\": %d,\n    \"lineText\": ", ObjLine(o)); JsonLineText(f, ObjLine(o));
        fprintf(f, "\n  }");
    } else fprintf(f, "null");

    fprintf(f, ",\n  \"selection\": [");
    for (int k = 0; k < B->selLineCount; k++) {
        const SelLine *sl = &B->selLines[k];
        fprintf(f, "%s\n    {\n      \"line\": %d,\n      \"lineText\": ", k ? "," : "", sl->line);
        JsonLineText(f, sl->line);
        fprintf(f, ",\n      \"group\": "); JsonStr(f, GroupName(ObjGroup(B->selOrder[sl->first])));
        fprintf(f, ",\n      \"objects\": [");
        for (int i = sl->first; i < sl->first + sl->count; i++) {
            MS_ObjRef o = B->selOrder[i];
            BoundingBox b = ObjBounds(o);
            fprintf(f, "%s\n        { \"kind\": \"%s\", \"index\": %d", i > sl->first ? "," : "", ObjKindName(o), o.index);
            if (o.kind == MS_OBJ_BOX) fprintf(f, ", \"box\": %d", o.index);
            fprintf(f, ", ");
            JsonCopy(f, o);
            fprintf(f, ", \"material\": ");
            JsonMaterial(f, o);
            fprintf(f, ", \"min\": ");
            JsonVec(f, b.min);
            fprintf(f, ", \"max\": "); JsonVec(f, b.max);
            fprintf(f, ", \"center\": "); JsonVec(f, Vector3Scale(Vector3Add(b.min, b.max), 0.5f));
            fprintf(f, ", \"size\": "); JsonVec(f, Vector3Subtract(b.max, b.min));
            if (ObjTurn(o)) { fprintf(f, ", \"rotationDeg\": "); JsonVec(f, ObjTurn(o)->rotationDeg); }
            if (o.kind == MS_OBJ_MODEL) { fprintf(f, ", \"model\": "); JsonModel(f, o.index); }
            fprintf(f, " }");
        }
        fprintf(f, "\n      ]\n    }");
    }
    fprintf(f, "%s],\n  \"selectedCount\": %d", B->selLineCount ? "\n  " : "", B->selCount);

    fprintf(f, ",\n  \"regions\": [");
    for (int i = 0; i < r->regionCount; i++) {
        const DesignRegionInfo *g = &r->regions[i];
        fprintf(f, "%s\n    {\n      \"index\": %d,\n      \"rect\": [%.0f, %.0f, %.0f, %.0f],\n      \"rectNorm\": [",
                i ? "," : "", i + 1, g->rect.x, g->rect.y, g->rect.width, g->rect.height);
        JsonNum(f, g->rect.x / r->width); fputs(", ", f); JsonNum(f, g->rect.y / r->height); fputs(", ", f);
        JsonNum(f, g->rect.width / r->width); fputs(", ", f); JsonNum(f, g->rect.height / r->height);
        fprintf(f, "],\n      \"crop\": \"region-%d.png\",\n      \"center\": ", i + 1);
        if (ObjValid(g->centerObj)) {
            fprintf(f, "{ \"point\": "); JsonVec(f, g->centerPoint);
            fprintf(f, ", \"normal\": "); JsonVec(f, g->centerNormal);
            fprintf(f, ", \"kind\": \"%s\", \"index\": %d, \"distance\": ", ObjKindName(g->centerObj), g->centerObj.index);
            JsonNum(f, g->centerDist);
            fprintf(f, ", \"line\": %d, \"group\": ", ObjLine(g->centerObj)); JsonStr(f, GroupName(ObjGroup(g->centerObj)));
            fprintf(f, " }");
        } else fprintf(f, "null");
        fprintf(f, ",\n      \"depth\": ");
        if (g->hasDepth) {
            fprintf(f, "{ \"min\": "); JsonNum(f, g->depthMin); fprintf(f, ", \"max\": "); JsonNum(f, g->depthMax); fprintf(f, " }");
        } else fprintf(f, "null");
        fprintf(f, ",\n      \"samples\": %d,\n      \"grid\": [%d, %d],\n      \"objects\": [", g->gridW * g->gridH, g->gridW, g->gridH);
        int listed = 0;
        for (int k = 0; k < g->objectCount; k++) {
            MS_ObjRef o = g->objects[k].obj;
            if (!ObjValid(o)) continue;
            BoundingBox b = ObjBounds(o);
            fprintf(f, "%s\n        { \"kind\": \"%s\", \"index\": %d, \"line\": %d, \"group\": ",
                    listed++ ? "," : "", ObjKindName(o), o.index, ObjLine(o));
            JsonStr(f, GroupName(ObjGroup(o)));
            if (o.kind == MS_OBJ_MODEL) { fprintf(f, ", \"file\": "); JsonStr(f, scene->modelFiles[scene->models[o.index].file].path); }
            fprintf(f, ", \"material\": "); JsonMaterial(f, o);
            fprintf(f, ", \"share\": "); JsonNum(f, g->objects[k].share);
            fprintf(f, ", \"min\": "); JsonVec(f, b.min);
            fprintf(f, ", \"max\": "); JsonVec(f, b.max);
            fprintf(f, " }");
        }
        fprintf(f, "%s],\n      \"omittedObjects\": %d,\n      \"omittedShare\": ", listed ? "\n      " : "", g->omittedObjects);
        JsonNum(f, g->omittedShare);
        fprintf(f, ",\n      \"sky\": "); JsonNum(f, g->sky);
        int lineCount = 0;
        LineShare *ls = RegionLines(g, &lineCount);
        fprintf(f, ",\n      \"lines\": [");
        for (int k = 0; k < lineCount; k++) {
            fprintf(f, "%s\n        { \"line\": %d, \"group\": ", k ? "," : "", ls[k].line);
            JsonStr(f, GroupName(ObjGroup(ls[k].first)));
            fprintf(f, ", \"objects\": %d, \"share\": ", ls[k].objects);
            JsonNum(f, ls[k].share);
            fprintf(f, " }");
        }
        fprintf(f, "%s]", lineCount ? "\n      " : "");
        free(ls);
        fprintf(f, ",\n      \"edge\": { \"offsetPx\": "); JsonNum(f, g->edgeOffset);
        fprintf(f, ", \"samples\": %d, \"sky\": ", g->edgeSamples); JsonNum(f, g->edgeSky);
        fprintf(f, ", \"objects\": [");
        for (int k = 0; k < g->edgeCount; k++) {
            const DesignRegionEdge *e = &g->edges[k];
            fprintf(f, "%s\n        { \"kind\": \"%s\", \"index\": %d, \"line\": %d, \"group\": ", k ? "," : "",
                    ObjKindName(e->obj), e->obj.index, ObjValid(e->obj) ? ObjLine(e->obj) : 0);
            JsonStr(f, ObjValid(e->obj) ? GroupName(ObjGroup(e->obj)) : "");
            if (e->obj.kind == MS_OBJ_MODEL && ObjValid(e->obj)) {
                fprintf(f, ", \"file\": "); JsonStr(f, scene->modelFiles[scene->models[e->obj.index].file].path);
            }
            fprintf(f, ", \"distance\": "); JsonNum(f, e->dist);
            fprintf(f, ", \"point\": "); JsonVec(f, e->point);
            fprintf(f, ", \"samples\": %d, \"sides\": [", e->samples);
            static const char *const SIDES[] = { "left", "right", "top", "bottom" };
            for (int s = 0, m = 0; s < 4; s++) if (e->sides & (1u << s)) fprintf(f, "%s\"%s\"", m++ ? ", " : "", SIDES[s]);
            fprintf(f, "] }");
        }
        fprintf(f, "%s] }", g->edgeCount ? "\n      " : "");
        fprintf(f, "\n    }");
    }
    fprintf(f, "%s]", r->regionCount ? "\n  " : "");
    WriteVolumesJson(f, r);

    fprintf(f, ",\n  \"nearby\": {\n    \"center\": "); JsonVec(f, B->nearbyCenter);
    fprintf(f, ",\n    \"centerIs\": \"%s\",\n    \"radius\": ", B->hit ? "hit" : "eye"); JsonNum(f, NEARBY_RADIUS);
    fprintf(f, ",\n    \"groups\": [");
    for (int i = 0; i < B->nearbyCount; i++) {
        const Nearby *n = &B->nearby[i];
        fprintf(f, "%s\n      { \"group\": ", i ? "," : ""); JsonStr(f, GroupName(n->group));
        fprintf(f, ", \"nearest\": "); JsonNum(f, n->dist);
        fprintf(f, ", \"objectsWithinRadius\": %d, \"boxesWithinRadius\": %d, ", n->nearCount, n->nearCount);
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
    fprintf(f, ",\n    \"package\": "); JsonStr(f, B->package);
    fprintf(f, ",\n    \"brief\": ");
    if (B->brief[0]) JsonStr(f, B->brief); else fputs("null", f);
    fprintf(f, ",\n    \"formatVersion\": %d", MAP_FORMAT_VERSION);
    fprintf(f, ",\n    \"hash\": \"%016llx\",\n    \"hashAlgorithm\": \"FNV-1a 64 of the file bytes\"", scene->hash);
    fprintf(f, ",\n    \"mtime\": %ld,\n    \"version\": %u,\n    \"boxes\": %d,\n    \"models\": %d,\n    \"groups\": %d,\n    \"materials\": %d\n  }\n}\n",
            scene->mtime, scene->version, scene->boxCount, scene->modelCount, scene->groupCount, MAP_MATERIALS);

    return fclose(f) == 0;
}

// #endregion

// ======================================================================================
// #region Open requests (HUD list + result.md polling)
// ======================================================================================

// A terminal window we started: a request's (per-request mode) or the coordinator's
typedef struct {
    bool launched;              // false: launching failed (or never tried)
    bool exited;                // the terminal process is gone (result may still come)
#ifndef _WIN32
    pid_t pid;                  // the terminal (session + process group leader, setsid)
    char mark[160];             // i3/sway mark set on its window by the workspace helper
    int killStage;              // after a close: 1 = WM closed it, wait; 2 = SIGTERM sent, wait
    double killTime;            // clockTime of the next escalation step
#endif
} TermWindow;

typedef struct {
    bool used;
    long seq;                   // submit order, for "most recent first"
    char id[128], slug[SLUG_MAX + 1];
    char dir[1300];             // bundle folder, trailing slash
    char map[1024];             // absolute path of the map file it edits ("" = imports)
    double submitTime, doneTime;
    bool queue;                 // coordinator mode: tracked through requests/<id>/state
    char state[16];             // queue: first word of the state file ("" = not read yet)
    bool cancelAsked;           // queue: the developer asked to cancel it (cancel file written)
    bool done;
    bool closed;                // closed from the game: off the list, the bundle stays
    int number;                 // [n] in the HUD list, stable while listed (the lowest free number)
    long resultSize;            // size of result.md at the last poll (wait until it settles)
    char summary[160];          // first line of result.md (queue: of question.md while asking)
    Vector3 eye;
    float yaw, pitch;
    bool hasCamera;             // eye / yaw / pitch are set (not for imports)
    TermWindow term;            // per-request mode: its terminal
} OpenRequest;

static OpenRequest *reqs;       // heap; a retired entry (Retired) is reused by the next request
static int reqCount, reqCap;
static long nextSeq = 1;
static double clockTime;        // sum of LauncherUpdate dt, the HUD's time base
static float pollTimer;
static long lastDoneSeq;        // most recently finished request (by finish order)
static long doneCounter;
static Vector3 lastDoneEye;
static float lastDoneYaw, lastDonePitch;
static long confirmSeq;         // running request whose close waits for a second key press
static double confirmUntil;     // ... until this clockTime

// The coordinator (coordinator mode, region "Coordinator and queue")
static struct {
    TermWindow term;            // its terminal, if this game run started it
    bool tried;                 // this game run tried to start one
    double startTime;           // clockTime of that attempt
    char error[256];            // why the last attempt failed ("" = it didn't)
    int alive;                  // last lock test: 1 = running, 0 = not, -1 = not tested yet
    float checkTimer;
    double confirmUntil;        // CTRL+SHIFT+K with requests open: second press until then
} coord = { .alive = -1 };

// Its window's i3 / sway mark: fixed, so a restarted game can still close it through the WM
static void InitCoordinatorMark(void)
{
#ifndef _WIN32
    if (requestsHash) snprintf(coord.term.mark, sizeof(coord.term.mark), "_monolith_coordinator_%08x", requestsHash);
    else snprintf(coord.term.mark, sizeof(coord.term.mark), "_monolith_coordinator");
#endif
}

#ifndef _WIN32
// Every child is reaped with waitpid(pid, WNOHANG) from LauncherUpdate. Not waitpid(-1):
// that would also collect children some other code might start and wait for. Not a
// SIGCHLD handler: it would interrupt raylib's and the driver's system calls. Children
// are tracked here separately from reqs[] (a heap list; reaped ones are removed).
static pid_t *children;
static int childCount, childCap;

static void TrackChild(pid_t pid)
{
    pid_t *slot = MS_PUSH(children, childCount, childCap);
    if (slot) *slot = pid;
    // out of memory: not tracked, init reaps it after we exit
    else TraceLog(LOG_WARNING, "monolith: out of memory, pid %d not tracked", (int)pid);
}
#endif

static bool Listed(const OpenRequest *q) { return q->used && !q->closed; }

// The terminal window may still be open (the process we started is still running). Unknown
// for terminals that hand the window to a daemon and exit (kitty --single-instance, wezterm)
static bool WindowMaybeOpen(const OpenRequest *q)
{
    return q->term.launched && !q->term.exited;
}

// An entry nobody needs any more: closed and its terminal shut down, or finished, off the
// HUD and its terminal gone. Its slot is reused (the list only grows to the most requests
// that were alive at once).
static bool Retired(const OpenRequest *q)
{
    if (!q->used) return true;
#ifndef _WIN32
    if (q->closed) return q->term.killStage == 0;
#else
    if (q->closed) return true;
#endif
    return q->done && clockTime - q->doneTime > DONE_SHOW_TIME && !WindowMaybeOpen(q);
}

// A new request entry (pointer valid until the next NewSlot); NULL when out of memory
static OpenRequest *NewSlot(void)
{
    OpenRequest *pick = NULL;
    for (int i = 0; i < reqCount && !pick; i++) if (Retired(&reqs[i])) pick = &reqs[i];
    if (!pick && !(pick = MS_PUSH(reqs, reqCount, reqCap))) return NULL;
    memset(pick, 0, sizeof(*pick));
    pick->used = true;
    pick->seq = nextSeq++;
    // Lowest number no listed request has, so the numbers in the HUD don't shift
    for (int n = 1; !pick->number; n++) {
        bool taken = false;
        for (int i = 0; i < reqCount; i++) if (&reqs[i] != pick && Listed(&reqs[i]) && reqs[i].number == n) taken = true;
        if (!taken) pick->number = n;
    }
    return pick;
}

void LauncherShutdown(void)
{
    free(reqs);
    reqs = NULL;
    reqCount = reqCap = 0;
#ifndef _WIN32
    free(children);
    children = NULL;
    childCount = childCap = 0;
#endif
}

#ifndef _WIN32
// Is this child of ours still running? Reaps it (and marks its requests exited) if not.
// While a child is unreaped its pid, and so its process group id, can't be reused: that
// is what makes kill(-pid) safe in CloseRequest.
static bool ChildAlive(pid_t pid)
{
    if (pid <= 0) return false;
    int slot = -1;
    for (int i = 0; i < childCount; i++) if (children[i] == pid) slot = i;
    if (slot < 0) return false;             // reaped already (or never tracked)
    if (waitpid(pid, NULL, WNOHANG) == 0) return true;
    // Reaped: from now on the kernel may hand this pid to any new process, including a
    // later terminal of ours. Forget it in every window that holds it (and cancel a
    // pending WM close -> SIGTERM -> SIGKILL escalation), so a close of the old request
    // can never signal the new process.
    for (int k = 0; k <= reqCount; k++) {
        TermWindow *t = k < reqCount ? (reqs[k].used ? &reqs[k].term : NULL) : &coord.term;
        if (!t || t->pid != pid) continue;
        t->exited = true;
        t->pid = 0;
        t->killStage = 0;
        TraceLog(LOG_INFO, "monolith: terminal of %s exited (pid %d reaped)", k < reqCount ? reqs[k].id : "the coordinator", (int)pid);
    }
    children[slot] = children[--childCount];    // reaped (or not ours any more: ECHILD)
    return false;
}

static void CloseEscalate(void);
#endif

// Reads the first non-empty line of a bundle file (result.md, question.md) into out, without
// leading '#'s and blanks. *lastSize: the file's size at the previous call (NULL: accept only
// a complete line)
static bool ReadFirstLine(const OpenRequest *q, const char *name, long *lastSize, char *out, int outSize)
{
    char path[1400];
    snprintf(path, sizeof(path), "%s%s", q->dir, name);
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
    bool settled = lastSize && size == *lastSize;
    if (lastSize) *lastSize = size;
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
            if (len > outSize - 1) len = outSize - 1;
            memcpy(out, s, len);
            out[len] = 0;
            // No control characters in the HUD line (non-ASCII shows as '?' in raylib's font)
            for (char *c = out; *c; c++) if ((unsigned char)*c < 0x20) *c = ' ';
            return true;
        }
        if (!*e) return false;
        p = e;      // a line of only '#'s: try the next one
    }
}

static bool ReadResult(OpenRequest *q)
{
    return ReadFirstLine(q, "result.md", &q->resultSize, q->summary, (int)sizeof(q->summary));
}

static bool StateFinal(const char *s)
{
    return strcmp(s, "done") == 0 || strcmp(s, "failed") == 0 || strcmp(s, "cancelled") == 0;
}

static void CoordinatorCheck(void);
static void ReadState(OpenRequest *q);

void LauncherUpdate(float dt)
{
    clockTime += dt;
    pollTimer += dt;
    if (pollTimer < POLL_INTERVAL) return;
    pollTimer = 0.0f;

#ifndef _WIN32
    // (a reaped child leaves the list: its slot then holds another one, so don't advance)
    for (int i = 0; i < childCount; ) if (ChildAlive(children[i])) i++;
    CloseEscalate();
#endif

    CoordinatorCheck();

    for (int i = 0; i < reqCount; i++) {
        OpenRequest *q = &reqs[i];
        if (!q->used || q->done) continue;
        bool final = false;
        if (q->queue) {
            ReadState(q);
            final = StateFinal(q->state);
            if (strcmp(q->state, "question") == 0 && !ReadFirstLine(q, "question.md", NULL, q->summary, (int)sizeof(q->summary)))
                snprintf(q->summary, sizeof(q->summary), "see the coordinator terminal");
        }
        bool result = ReadResult(q);
        if (!result && !final) continue;
        if (!result) snprintf(q->summary, sizeof(q->summary), "%s (no result.md)", q->state);
        q->done = true;
        q->doneTime = clockTime;
        // R flies back to requests that ended with a result (not to cancelled ones)
        if (q->hasCamera && result && strcmp(q->state, "cancelled") != 0) {
            lastDoneSeq = ++doneCounter;
            lastDoneEye = q->eye;
            lastDoneYaw = q->yaw;
            lastDonePitch = q->pitch;
        }
        TraceLog(LOG_INFO, "monolith: request %s %s: %s", q->id, final ? q->state : "done", q->summary);
    }
}

// HUD line of the coordinator; NULL when there is nothing to say
static const char *CoordinatorLine(Color *c);

int LauncherDrawHud(int x, int y)
{
    const int size = UI_M, step = 24, maxRows = 6;
    int rows = 0;
    Color cc;
    const char *coordText = CoordinatorLine(&cc);
    if (coordText) {
        UiHudText(coordText, x, y, step, size, cc);
        rows++;
    }
    long below = 1L << 30;      // walk the list from the newest request down
    while (rows < maxRows) {
        OpenRequest *q = NULL;
        for (int i = 0; i < reqCount; i++)
            if (Listed(&reqs[i]) && reqs[i].seq < below && (!q || reqs[i].seq > q->seq)) q = &reqs[i];
        if (!q) break;
        below = q->seq;

        const char *text;
        Color c = { 235, 232, 225, 220 };
        int t = (int)(clockTime - q->submitTime);
        if (q->queue && q->done) {
            float age = (float)(clockTime - q->doneTime);
            if (age > DONE_SHOW_TIME) continue;
            bool ok = strcmp(q->state, "failed") != 0 && strcmp(q->state, "cancelled") != 0;
            Color base = ok ? (Color){ 150, 230, 150, 255 } : strcmp(q->state, "failed") == 0 ? (Color){ 255, 170, 110, 255 } : (Color){ 190, 190, 190, 255 };
            c = Fade(base, fminf((DONE_SHOW_TIME - age) / DONE_FADE_TIME, 1.0f));
            text = TextFormat("[%d] [%s] %s: %s", q->number, ok ? "done" : q->state, q->slug, q->summary);
        } else if (q->queue) {
            const char *cancel = q->cancelAsked ? ", cancelling" : "";
            if (strcmp(q->state, "question") == 0) {
                c = (Color){ 255, 225, 120, 240 };
                text = TextFormat("[%d] [?] %s: %s (answer in the coordinator terminal)", q->number, q->slug, q->summary);
            } else if (strcmp(q->state, "claimed") == 0)
                text = TextFormat("[%d] ... %s running (%02d:%02d%s)", q->number, q->slug, t / 60, t % 60, cancel);
            else {
                c = (Color){ 200, 205, 215, 200 };
                text = TextFormat("[%d] queued %s (%02d:%02d%s)", q->number, q->slug, t / 60, t % 60, cancel);
            }
        } else if (q->done) {
            float age = (float)(clockTime - q->doneTime);
            if (age > DONE_SHOW_TIME) {
                // After its 20 s a finished request stays as a dim line while its terminal
                // is still open, so it can still be closed by number (K / CTRL+K n)
                if (!WindowMaybeOpen(q)) continue;
                c = (Color){ 150, 230, 150, 110 };
                text = TextFormat("[%d] done %s (terminal open)", q->number, q->slug);
            } else {
                c = Fade((Color){ 150, 230, 150, 255 }, fminf((DONE_SHOW_TIME - age) / DONE_FADE_TIME, 1.0f));
                if (WindowMaybeOpen(q)) c.a = (unsigned char)fmaxf(c.a, 110.0f);
                text = TextFormat("[%d] [done] %s: %s", q->number, q->slug, q->summary);
            }
        } else {
            if (!q->term.launched) {
                c = (Color){ 255, 170, 110, 230 };
                text = TextFormat("[%d] [!] %s not launched, run it by hand (see log), waiting (%02d:%02d)", q->number, q->slug, t / 60, t % 60);
            } else if (q->term.exited)
                // Not necessarily a failure: some terminals hand the window to a daemon
                // and exit right away (kitty --single-instance, wezterm with a mux)
                text = TextFormat("[%d] ... %s waiting for result.md, terminal process gone (%02d:%02d)", q->number, q->slug, t / 60, t % 60);
            else
                text = TextFormat("[%d] ... %s running (%02d:%02d)", q->number, q->slug, t / 60, t % 60);
        }
        UiHudText(text, x, y + rows * step, step, size, c);
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

// The request terminal's window title: "monolith-claude: <slug> <HHMMSS>" plus the bundle
// id's "-n" suffix if it has one, i.e. unique per bundle (ids are). Two requests with the
// same text then never share a title, so MarkAndMove's exact title match can't mark the
// other one's window (K would close the wrong terminal). Ids that don't start with the
// time stamp give "monolith-claude: <slug> <id>". Characters: [0-9a-z_ -] only.
static void RequestTitle(const char *id, const char *slug, char *out, int outSize)
{
    size_t sl = strlen(slug), il = strlen(id);
    bool stamped = il >= 18 + sl && id[10] == '_' && id[17] == '_' && strncmp(id + 18, slug, sl) == 0;
    if (stamped) snprintf(out, outSize, "monolith-claude: %s %.6s%s", slug, id + 11, id + 18 + sl);
    else snprintf(out, outSize, "monolith-claude: %s %s", slug, id);
}

// Every Claude Code session starts with these (when its folder has CLAUDE.md), unless MONOLITH_CLAUDE_SAFE is set
// (to anything but "" or "0")
static const char *const SKIP_PERMISSION_FLAGS[] = {
    "--allow-dangerously-skip-permissions", "--dangerously-skip-permissions",
};
#define SKIP_PERMISSION_COUNT 2

static bool ClaudeSafe(void)
{
    const char *s = getenv("MONOLITH_CLAUDE_SAFE");
    return s && *s && strcmp(s, "0") != 0;
}

// The permission flags are passed only where the session can read its guard rails: the
// working directory (rootDir) must hold CLAUDE.md. Without it (Windows, a copied build folder)
// the session keeps Claude Code's approval prompts, and the log says why once.
static bool SkipPermissions(void)
{
    if (ClaudeSafe()) return false;
    if (FileExists(TextFormat("%sCLAUDE.md", rootDir))) return true;
    static bool warned = false;
    if (!warned) {
        TraceLog(LOG_WARNING, "monolith: no CLAUDE.md in %s: Claude Code sessions keep their approval prompts", rootDir);
        warned = true;
    }
    return false;
}

static bool FindProgram(const char *name, char *out, int outSize);

// The claude program: $MONOLITH_CLAUDE (tests: tools/stub-coordinator.sh), else claude in PATH,
// else ~/.local/bin/claude (where the native installer puts it, which a game started from a
// desktop launcher may not have in PATH)
static bool FindClaude(char *out, int outSize)
{
    const char *env = getenv("MONOLITH_CLAUDE");
    if (env && *env) return FindProgram(env, out, outSize);
    if (FindProgram("claude", out, outSize)) return true;
#ifdef _WIN32
    return false;
#else
    if (!getenv("HOME")) return false;
    char cand[2048];
    snprintf(cand, sizeof(cand), "%s/.local/bin/claude", getenv("HOME"));
    return FindProgram(cand, out, outSize);
#endif
}

#ifdef _WIN32

static bool FindProgram(const char *name, char *out, int outSize)
{
    static const char *exts[] = { ".exe", ".cmd", ".bat", "" };
    if (strpbrk(name, "/\\")) {         // a path (MONOLITH_CLAUDE)
        snprintf(out, outSize, "%s", name);
        return _access(out, 0) == 0;
    }
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

static bool Launch(const char *title, const char *mark, const char *prompt, const char *model, const char *lock,
                   TermWindow *t, char *err, int errSize)
{
    (void)t;
    (void)mark;
    char claude[1024];
    if (!FindClaude(claude, sizeof(claude))) {
        snprintf(err, errSize, "claude not found in PATH (or MONOLITH_CLAUDE)");
        return false;
    }
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", rootDir);
    for (char *c = dir; *c; c++) if (*c == '/') *c = '\\';
    size_t n = strlen(dir);
    if (n > 1 && dir[n - 1] == '\\') dir[n - 1] = 0;     // "C:\x\" would escape the closing quote

    // (quoting for cmd /k: see wincmd.c)
    ArgList args;
    if (!ClaudeArgs(&args, NULL, false)) { snprintf(err, errSize, "out of memory"); return false; }
    int skipped = 0;
    char *cmd = WinClaudeCommand(claude, SKIP_PERMISSION_FLAGS, SkipPermissions() ? SKIP_PERMISSION_COUNT : 0, model,
                                 args.tok, args.count, prompt, &skipped);
    FreeArgs(&args);
    if (!cmd) { snprintf(err, errSize, "out of memory"); return false; }
    if (skipped) TraceLog(LOG_WARNING, "monolith: %d word%s with a double quote left out of the claude command line (cmd can't pass them)",
                          skipped, skipped == 1 ? "" : "s");
    // The coordinator: ` 9>"<lock>"` inside the outer quotes, so cmd keeps the lock file
    // open (without delete / write sharing) while claude runs (CoordinatorAlive)
    if (lock) {
        if (strchr(lock, '"')) { free(cmd); snprintf(err, errSize, "the requests folder has a double quote in its path"); return false; }
        size_t cl = strlen(cmd);
        char *with = malloc(cl + strlen(lock) + 8);
        if (!with) { free(cmd); snprintf(err, errSize, "out of memory"); return false; }
        memcpy(with, cmd, cl - 1);                              // without the closing outer quote
        sprintf(with + cl - 1, " 9>\"%s\"\"", lock);
        free(cmd);
        cmd = with;
        for (char *c = cmd + cl; *c; c++) if (*c == '/') *c = '\\';
    }
    char titleArg[300], dirArg[1100];
    snprintf(titleArg, sizeof(titleArg), "\"%s\"", title);
    snprintf(dirArg, sizeof(dirArg), "\"%s\"", dir);
    // cmd.exe takes command lines of at most 8191 characters: say so instead of a bare
    // "cannot start" (or a window that drops the end)
    size_t total = strlen(cmd) + strlen(titleArg) + strlen(dirArg) + 40;
    if (total > 8191) {
        snprintf(err, errSize, "the claude command line is %zu characters, cmd.exe takes at most 8191 (shorten MONOLITH_CLAUDE_ARGS)", total);
        free(cmd);
        return false;
    }
    const char *argv[] = { "cmd.exe", "/c", "start", titleArg, "/D", dirArg, "cmd.exe", "/k", cmd, NULL };
    intptr_t h = _spawnvp(_P_NOWAIT, "cmd.exe", argv);
    free(cmd);
    if (h == -1) {
        snprintf(err, errSize, "cannot start cmd.exe: %s", strerror(errno));
        return false;
    }
    CloseHandle((void *)h);     // cmd /c start exits right away; nothing to wait for
    TraceLog(LOG_INFO, "monolith: launched '%s' via cmd start", title);
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

// i3-msg, or swaymsg when $SWAYSOCK is set; false if neither is installed
static bool WmTool(char *out, int outSize, const char **name)
{
    const char *sway = getenv("SWAYSOCK");
    *name = sway && *sway ? "swaymsg" : "i3-msg";
    return FindProgram(*name, out, outSize);
}

// On i3 or sway, marks the window titled `title` with `mark` (for closing it later, see
// CloseRequest) and moves it to workspace $MONOLITH_WORKSPACE (default 10; empty = mark
// only), without waiting for it: a detached /bin/sh retries `i3-msg <criteria> mark --add
// <mark>, move container to workspace ...` every 0.1 s for up to 10 s. i3-msg / swaymsg
// exit with 2 while no window matches (keep trying), 0 once it worked, 1 when there is no
// i3 / sway to talk to (stop). The command reaches the shell as a positional parameter,
// never as script text. Best effort: nothing is reported back.
// Why a mark: Claude Code retitles its terminal (OSC title) once it runs, so the title is
// only reliable in the first second or so, when the window appears. Marks starting with
// '_' are never drawn in title bars.
static void MarkAndMove(const char *title, const char *mark)
{
    const char *ws = getenv("MONOLITH_WORKSPACE");
    if (!ws) ws = "10";
    if (strpbrk(ws, "\"\\")) {
        TraceLog(LOG_WARNING, "monolith: MONOLITH_WORKSPACE has quotes or backslashes, terminal not moved");
        ws = "";
    }
    const char *tool;
    char toolPath[4096];
    if (!WmTool(toolPath, sizeof(toolPath), &tool)) return;     // no i3 / sway here
    bool number = ws[strspn(ws, "0123456789")] == 0;
    // The title is RequestTitle's (unique per request; [0-9a-z_ -]), the mark
    // "_monolith_<id>" (id: [0-9_a-z-]): no regex specials, no quotes
    char cmd[640];
    int len = snprintf(cmd, sizeof(cmd), "[title=\"^%s$\"] mark --add \"%s\"", title, mark);
    if (*ws && number) snprintf(cmd + len, sizeof(cmd) - len, ", move container to workspace number %s", ws);
    else if (*ws) snprintf(cmd + len, sizeof(cmd) - len, ", move container to workspace \"%s\"", ws);
    static const char script[] =
        "i=0; while [ $i -lt 100 ]; do \"$0\" \"$1\" >/dev/null 2>&1; r=$?; "
        "[ $r -eq 2 ] || exit $r; sleep 0.1; i=$((i + 1)); done; exit 2";
    char *argv[] = { "/bin/sh", "-c", (char *)script, toolPath, cmd, NULL };
    pid_t pid = fork();
    if (pid < 0) { TraceLog(LOG_WARNING, "monolith: workspace move: fork: %s", strerror(errno)); return; }
    if (pid == 0) {
        setsid();       // outlives the game (a --shot --submit run exits right away)
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); if (nul > 2) close(nul); }
        execve("/bin/sh", argv, environ);
        _exit(127);
    }
    TrackChild(pid);    // reaped by LauncherUpdate like the terminals
    if (*ws) TraceLog(LOG_INFO, "monolith: moving '%s' to workspace %s via %s (pid %d)", title, ws, tool, (int)pid);
    else TraceLog(LOG_INFO, "monolith: marking '%s' via %s (pid %d)", title, tool, (int)pid);
}

// /bin/sh script around the coordinator's claude ($0 = the lock file, "$@" = claude and its
// arguments): holds an flock on the lock for as long as claude runs (CoordinatorAlive tests
// it), without passing the descriptor on, so a background process claude leaves behind can't
// keep the lock alive. A second coordinator started within moments (two quick submits) finds
// the lock taken after 2 s, says so and exits.
static const char COORDINATOR_WRAPPER[] =
    "exec 9>>\"$0\" || exit 1\n"
    "if ! flock -w 2 9; then echo 'monolith: a request coordinator is already running; this window closes in 5 s'; sleep 5; exit 0; fi\n"
    "\"$@\" 9>&-\n"
    "st=$?\n"
    "if [ $st -ne 0 ]; then echo \"monolith: the coordinator exited with status $st; this window closes in 30 s\"; sleep 30; fi\n"
    "exit $st\n";

// Opens a terminal titled `title` running claude [flags] [--model model] [extra] prompt in the
// repo root; lock != NULL: the coordinator, wrapped in COORDINATOR_WRAPPER with that lock file.
// mark: the i3 / sway mark for its window (MarkAndMove). Fills t (pid, mark).
static bool Launch(const char *title, const char *mark, const char *prompt, const char *model, const char *lock,
                   TermWindow *t, char *err, int errSize)
{
    // Terminal: env override (program + extra args) or the first known one in PATH
    ArgList term = { 0 }, args = { 0 };
    char termPath[4096];
    const char *envVar = NULL;
    const char *env = getenv("MONOLITH_TERMINAL");
    if (env && *env) envVar = "MONOLITH_TERMINAL";
    else if ((env = getenv("TERMINAL")) && *env) envVar = "TERMINAL";
    if (envVar) {
        if (!SplitArgs(env, &term)) { snprintf(err, errSize, "out of memory"); return false; }
        if (term.count == 0 || !FindProgram(term.tok[0], termPath, sizeof(termPath))) {
            snprintf(err, errSize, "terminal '%s' from $%s not found", term.count ? term.tok[0] : "", envVar);
            FreeArgs(&term);
            return false;
        }
    } else {
        for (int i = 0; i < TERMINAL_COUNT && !term.count; i++)
            if (FindProgram(TERMINALS[i].name, termPath, sizeof(termPath))) {
                FreeArgs(&term);
                if (!SplitArgs(TERMINALS[i].name, &term)) { snprintf(err, errSize, "out of memory"); return false; }
            }
        if (!term.count) {
            FreeArgs(&term);
            snprintf(err, errSize, "no terminal found (tried alacritty kitty foot wezterm xterm; set MONOLITH_TERMINAL)");
            return false;
        }
    }
    char **termTok = term.tok;
    int termCount = term.count;
    const TermSyntax *ts = &UNKNOWN_TERMINAL;
    for (int i = 0; i < TERMINAL_COUNT; i++)
        if (strcmp(BaseName(termTok[0]), TERMINALS[i].name) == 0) ts = &TERMINALS[i];

    // From here on every return goes through `fail` / the end, which free term, args, argv
    char **argv = NULL;
    bool ok = false;
    char claude[4096];
    if (!FindClaude(claude, sizeof(claude))) {
        snprintf(err, errSize, "claude not found in PATH or ~/.local/bin (or MONOLITH_CLAUDE)");
        goto fail;
    }

    // argv: terminal [its extra args] [pre] [title] [dir] [exec] [wrapper] claude [claude args] prompt
    char titleBuf[300], dir[1024];
    snprintf(titleBuf, sizeof(titleBuf), "%s", title);
    snprintf(dir, sizeof(dir), "%s", rootDir);
    size_t dl = strlen(dir);
    if (dl > 1 && dir[dl - 1] == '/') dir[dl - 1] = 0;
    if (!ClaudeArgs(&args, NULL, false)) { snprintf(err, errSize, "out of memory"); goto fail; }
    char **extra = args.tok;
    int extraCount = args.count;
    // (allocated before the fork: the child may only call async-signal-safe functions)
    argv = malloc(sizeof(char *) * (termCount + 8 + 4 + SKIP_PERMISSION_COUNT + 2 + extraCount + 4));
    if (!argv) { snprintf(err, errSize, "out of memory"); goto fail; }
    int n = 0;
    for (int i = 0; i < termCount; i++) argv[n++] = termTok[i];
    if (ts->pre) argv[n++] = (char *)ts->pre;
    if (ts->titleFlag) { argv[n++] = (char *)ts->titleFlag; argv[n++] = titleBuf; }
    if (ts->dirFlag) { argv[n++] = (char *)ts->dirFlag; argv[n++] = dir; }
    if (ts->exec) argv[n++] = (char *)ts->exec;
    if (lock) { argv[n++] = "/bin/sh"; argv[n++] = "-c"; argv[n++] = (char *)COORDINATOR_WRAPPER; argv[n++] = (char *)lock; }
    argv[n++] = claude;
    bool skip = SkipPermissions();
    for (int i = 0; i < SKIP_PERMISSION_COUNT && skip; i++) argv[n++] = (char *)SKIP_PERMISSION_FLAGS[i];
    if (model) { argv[n++] = "--model"; argv[n++] = (char *)model; }
    for (int i = 0; i < extraCount; i++) argv[n++] = extra[i];
    argv[n++] = (char *)prompt;
    argv[n] = NULL;

    // exec failures in the child are reported through a close-on-exec pipe: a successful
    // exec closes it (the parent reads EOF), a failed one writes errno first. The read
    // only waits for the exec itself, not for the terminal.
    int fds[2];
    if (pipe(fds) != 0) { snprintf(err, errSize, "pipe: %s", strerror(errno)); goto fail; }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        snprintf(err, errSize, "fork: %s", strerror(errno));
        goto fail;
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
        goto fail;
    }

    t->pid = pid;
    t->exited = false;
    t->killStage = 0;
    TrackChild(pid);
    TraceLog(LOG_INFO, "monolith: launched %s (pid %d) for '%s'", termPath, (int)pid, title);
    // (keep the mark free of quotes in any case)
    snprintf(t->mark, sizeof(t->mark), "%s", mark);
    for (char *c = t->mark; *c; c++) if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') *c = '_';
    MarkAndMove(title, t->mark);
    ok = true;
fail:
    free(argv);
    FreeArgs(&args);
    FreeArgs(&term);
    return ok;
}

#endif

// #endregion

// ======================================================================================
// #region Closing request windows (K / SHIFT+K / CTRL+K n, MONOLITH_CLOSE_ON_EXIT)
// ======================================================================================
// Coordinator mode has no request windows: K / SHIFT+K only take finished requests off the
// list and CTRL+K n cancels (CancelRequest); the coordinator's own window (CTRL+SHIFT+K,
// LauncherCloseCoordinator) closes the way a request window does in per-request mode.
// Linux, in this order:
//   1. i3 / sway: `i3-msg [con_mark="^_monolith_<id>$"] kill` (swaymsg on sway). The mark
//      was set by MarkAndMove when the window appeared, so it still matches after Claude
//      Code retitled the terminal. `kill` asks the window to close (WM_DELETE_WINDOW); it
//      closes exactly that window and changes neither focus nor the visible workspace. If
//      our terminal process still runs CLOSE_GRACE seconds later, step 2.
//   2. Signals: the terminal was started with setsid, so its pid is also its process group
//      id: SIGTERM to the group, SIGKILL CLOSE_GRACE seconds later if it is still alive.
//      Only while that pid is our own unreaped child (ChildAlive), so the pid / group can't
//      belong to anything else. Claude Code runs on the terminal's pty and gets SIGHUP when
//      the terminal goes. This is the route on other window managers and when the mark is
//      missing (the window appeared after Claude Code had already retitled it).
// Terminals that hand the window to a daemon and exit at once (kitty --single-instance,
// wezterm with a mux) leave no process to signal: only step 1 can close those.
// MONOLITH_CLOSE_METHOD=wm / signal restricts closing to one of the two (for tests).
// Windows: `cmd /c start` leaves no handle to the window, so a close only takes the entry
// off the list and says to close the window by hand.

#define CLOSE_GRACE         3.0     // seconds before escalating (WM close -> SIGTERM -> SIGKILL)
#define CONFIRM_TIME        3.0     // seconds to press again to stop a running request

typedef enum { CLOSED_WINDOW, CLOSED_GONE, CLOSED_LISTED } CloseOutcome;

#ifndef _WIN32
// 1: the WM closed a window with the mark, 0: no such window, -1: no i3 / sway
static int WmCloseMarked(const TermWindow *t)
{
    if (!t->mark[0]) return 0;
    const char *tool;
    char toolPath[4096];
    if (!WmTool(toolPath, sizeof(toolPath), &tool)) return -1;
    // `kill` alone reports success even when nothing matches; re-adding the mark first
    // fails (exit 2) when no window has it, so the exit status says whether one was closed
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "[con_mark=\"^%s$\"] mark --add \"%s\", kill", t->mark, t->mark);
    char *argv[] = { toolPath, cmd, NULL };
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); if (nul > 2) close(nul); }
        execve(toolPath, argv, environ);
        _exit(127);
    }
    // i3-msg answers within milliseconds; this only runs on a key press
    int st = 0;
    pid_t r;
    do r = waitpid(pid, &st, 0); while (r < 0 && errno == EINTR);
    if (r != pid || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st) == 0 ? 1 : WEXITSTATUS(st) == 2 ? 0 : -1;
}

static void SignalGroup(const TermWindow *t, const char *what, int sig)
{
    if (t->pid > 0 && ChildAlive(t->pid) && kill(-t->pid, sig) == 0)
        TraceLog(LOG_INFO, "monolith: %s sent to the terminal of %s (process group %d)",
                 sig == SIGKILL ? "SIGKILL" : "SIGTERM", what, (int)t->pid);
}

static void EscalateTerm(TermWindow *t, const char *what)
{
    if (!t->killStage || clockTime < t->killTime) return;
    // (ChildAlive: only a pid that is still our unreaped child; reaping clears t->pid)
    if (t->pid <= 0 || !ChildAlive(t->pid)) { t->killStage = 0; return; }
    if (t->killStage == 1) { SignalGroup(t, what, SIGTERM); t->killStage = 2; t->killTime = clockTime + CLOSE_GRACE; }
    else { SignalGroup(t, what, SIGKILL); t->killStage = 0; }
}

// From LauncherUpdate: WM close -> SIGTERM -> SIGKILL for terminals that linger
static void CloseEscalate(void)
{
    for (int i = 0; i < reqCount; i++) if (reqs[i].used) EscalateTerm(&reqs[i].term, reqs[i].id);
    EscalateTerm(&coord.term, "the coordinator");
}
#endif

// Closes a terminal window: through the WM, then by signals (above). what: for the log
static CloseOutcome CloseTerm(TermWindow *t, const char *what)
{
#ifdef _WIN32
    (void)t;
    (void)what;
    return CLOSED_LISTED;
#else
    const char *method = getenv("MONOLITH_CLOSE_METHOD");
    bool useWm = !method || strcmp(method, "signal") != 0;
    bool useSignal = !method || strcmp(method, "wm") != 0;
    int wm = useWm ? WmCloseMarked(t) : -1;
    bool alive = ChildAlive(t->pid);
    TraceLog(LOG_INFO, "monolith: closing %s: window manager %s, terminal process %s", what,
             wm == 1 ? "closed its window" : wm == 0 ? "found no window" : "not used", alive ? "running" : "gone");
    if (wm == 1) {
        if (alive && useSignal) { t->killStage = 1; t->killTime = clockTime + CLOSE_GRACE; }
        return CLOSED_WINDOW;
    }
    if (alive && useSignal) {
        SignalGroup(t, what, SIGTERM);
        t->killStage = 2;
        t->killTime = clockTime + CLOSE_GRACE;
        return CLOSED_WINDOW;
    }
    return CLOSED_GONE;
#endif
}

static CloseOutcome CloseRequest(OpenRequest *q)
{
    q->closed = true;
    if (confirmSeq == q->seq) confirmSeq = 0;
    if (!q->term.launched) return CLOSED_LISTED;
    return CloseTerm(&q->term, q->id);
}

static void CloseMessage(const OpenRequest *q, CloseOutcome o, char *msg, int msgSize)
{
    if (q->queue) snprintf(msg, msgSize, "%s off the list", q->slug);
    else if (o == CLOSED_WINDOW) snprintf(msg, msgSize, "closed %s", q->slug);
#ifdef _WIN32
    else if (q->term.launched) snprintf(msg, msgSize, "%s off the list; close its window by hand (not supported on Windows)", q->slug);
#endif
    else if (o == CLOSED_LISTED) snprintf(msg, msgSize, "%s off the list (it was never launched)", q->slug);
    else snprintf(msg, msgSize, "%s: window already gone", q->slug);
    TraceLog(LOG_INFO, "monolith: %s", msg);
}

static int CancelRequest(OpenRequest *q, char *msg, int msgSize);

bool LauncherCloseLatestDone(char *msg, int msgSize)
{
    OpenRequest *q = NULL;
    bool running = false, queue = false;
    for (int i = 0; i < reqCount; i++) {
        if (!Listed(&reqs[i])) continue;
        if (!reqs[i].done) { running = true; queue |= reqs[i].queue; }
        else if (!q || reqs[i].doneTime > q->doneTime) q = &reqs[i];
    }
    if (!q) {
        snprintf(msg, msgSize, "no finished request to close%s", !running ? "" : queue ? " (CTRL+K, then its number, cancels a queued or running one)"
                                                                               : " (CTRL+K, then its number, stops a running one)");
        return false;
    }
    CloseMessage(q, CloseRequest(q), msg, msgSize);
    return true;
}

int LauncherCloseAllDone(char *msg, int msgSize)
{
    int count = 0, gone = 0;
    OpenRequest *last = NULL;
    CloseOutcome lastOutcome = CLOSED_GONE;
    for (int i = 0; i < reqCount; i++) {
        OpenRequest *q = &reqs[i];
        if (!Listed(q) || !q->done) continue;
        lastOutcome = CloseRequest(q);
        last = q;
        count++;
        if (lastOutcome == CLOSED_GONE && !q->queue) gone++;
    }
    if (!last) snprintf(msg, msgSize, "no finished request to close");
    else if (count == 1) CloseMessage(last, lastOutcome, msg, msgSize);
    else {
        snprintf(msg, msgSize, "%s %d finished requests", last->queue ? "took off the list" : "closed", count);
        if (gone) snprintf(msg + strlen(msg), msgSize - strlen(msg), " (%d window%s already gone)", gone, gone == 1 ? "" : "s");
        TraceLog(LOG_INFO, "monolith: %s", msg);
    }
    return count;
}

int LauncherCloseNumber(int number, char *msg, int msgSize)
{
    OpenRequest *q = NULL;
    for (int i = 0; i < reqCount; i++) if (Listed(&reqs[i]) && reqs[i].number == number) q = &reqs[i];
    if (!q) { snprintf(msg, msgSize, "no request [%d] in the list", number); return -1; }
    if (q->queue && !q->done) return CancelRequest(q, msg, msgSize);
    // A request without result.md may still be working: ask for a second press first
    if (!q->done && !(confirmSeq == q->seq && clockTime <= confirmUntil)) {
        confirmSeq = q->seq;
        confirmUntil = clockTime + CONFIRM_TIME;
        snprintf(msg, msgSize, "press %d again to stop running request %s", number % 10, q->slug);
        return 0;
    }
    CloseMessage(q, CloseRequest(q), msg, msgSize);
    return 1;
}

int LauncherNewestNumber(void)
{
    OpenRequest *q = NULL;
    for (int i = 0; i < reqCount; i++) if (Listed(&reqs[i]) && (!q || reqs[i].seq > q->seq)) q = &reqs[i];
    return q ? q->number : 0;
}

bool LauncherClosing(void)
{
#ifndef _WIN32
    for (int i = 0; i < reqCount; i++)
        if (reqs[i].used && reqs[i].closed && reqs[i].term.launched && ChildAlive(reqs[i].term.pid)) return true;
    if (coord.term.killStage && ChildAlive(coord.term.pid)) return true;
#endif
    return false;
}

static bool QueueMode(void);
static bool CoordinatorAlive(void);
static int OpenQueueRequests(void);

void LauncherCloseOnExit(void)
{
    const char *e = getenv("MONOLITH_CLOSE_ON_EXIT");
    if (!e || !*e || strcmp(e, "0") == 0) return;
    // No escalation after this (the game is gone): the WM close, or SIGTERM where the WM
    // route isn't available, is all there is
    if (QueueMode()) {
        int open = OpenQueueRequests();
        if (open) TraceLog(LOG_INFO, "monolith: on exit: %d request%s still queued or running, the coordinator stays open", open, open == 1 ? "" : "s");
        else if (CoordinatorAlive()) CloseTerm(&coord.term, "the coordinator");
        return;
    }
    char msg[256];
    if (LauncherCloseAllDone(msg, sizeof(msg)) > 0) TraceLog(LOG_INFO, "monolith: on exit: %s", msg);
}

// #endregion

// ======================================================================================
// #region Coordinator and queue (MONOLITH_LAUNCH=coordinator, the default)
// ======================================================================================
// docs/request-queue.md. A submitted bundle gets requests/<id>/state = "queued <time>"
// as its last file; then, unless requests/.coordinator/lock is held, one terminal titled
// "monolith-claude: coordinator" runs COORDINATOR_WRAPPER + claude [permission flags]
// --model $MONOLITH_COORDINATOR_MODEL (default sonnet) [MONOLITH_CLAUDE_ARGS] "<fixed
// prompt naming .claude/skills/coordinator/SKILL.md>". The coordinator (tools/request-queue.sh) claims bundles
// and hands each to a subagent; after "queued" only it writes state (claimed, question,
// done, failed, cancelled). The game asks for a cancel with requests/<id>/cancel, and turns
// a queued bundle into "cancelled" itself only while no coordinator holds the lock.
// Windows: the lock is the coordinator's `9>` redirection held open by cmd; the game tests
// it with an exclusive open (_SH_DENYRW).

#define COORD_CHECK_INTERVAL 2.0f   // seconds between lock tests (only while relevant)
#define COORD_START_TIME     20.0   // after a start, "starting" this long before "not running"

static int queueMode = -1;

static bool QueueMode(void)
{
    if (queueMode >= 0) return queueMode;
    queueMode = 1;
    const char *m = getenv("MONOLITH_LAUNCH");
    if (m && *m) {
        if (strcmp(m, "per-request") == 0) queueMode = 0;
        else if (strcmp(m, "coordinator") != 0)
            TraceLog(LOG_WARNING, "monolith: MONOLITH_LAUNCH=%s unknown (coordinator or per-request); using coordinator", m);
    }
#ifndef _WIN32
    char path[4096];
    if (queueMode && !FindProgram("flock", path, sizeof(path))) {
        TraceLog(LOG_WARNING, "monolith: flock (util-linux) not found: one terminal per request instead of the coordinator");
        queueMode = 0;
    }
#endif
    return queueMode;
}

static void Stamp(char *out, int outSize)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm || !strftime(out, outSize, "%Y-%m-%dT%H:%M:%S", tm)) snprintf(out, outSize, "0000-00-00T00:00:00");
}

static bool WriteText(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

// requests/<id>/state = "<state> <time> game" (Linux: temp file + rename, so a reader never
// sees half a line; Windows' rename doesn't replace a file, and the line is one write anyway)
static bool WriteState(const char *dir, const char *state)
{
    char path[1400], line[96], stamp[32];
    Stamp(stamp, sizeof(stamp));
    snprintf(line, sizeof(line), "%s %s game\n", state, stamp);
    snprintf(path, sizeof(path), "%sstate", dir);
#ifdef _WIN32
    return WriteText(path, line);
#else
    char tmp[1400];
    snprintf(tmp, sizeof(tmp), "%sstate.tmp", dir);
    return WriteText(tmp, line) && rename(tmp, path) == 0;
#endif
}

static void ReadState(OpenRequest *q)
{
    char path[1400], buf[64] = "";
    snprintf(path, sizeof(path), "%sstate", q->dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char word[16];
    int len = 0;
    while (len < (int)sizeof(word) - 1 && isalpha((unsigned char)buf[len])) { word[len] = buf[len]; len++; }
    word[len] = 0;
    if (!len || strcmp(word, q->state) == 0) return;     // half written (or unchanged)
    TraceLog(LOG_INFO, "monolith: request %s: %s -> %s", q->id, q->state[0] ? q->state : "(new)", word);
    snprintf(q->state, sizeof(q->state), "%s", word);
}

// Is a coordinator running (does anyone hold its lock)?
static bool CoordinatorAlive(void)
{
    char path[1300];
    snprintf(path, sizeof(path), "%slock", coordDir);
#ifdef _WIN32
    int fd = -1;
    int e = _sopen_s(&fd, path, _O_RDWR | _O_CREAT, _SH_DENYRW, _S_IREAD | _S_IWRITE);
    if (e == 0) { _close(fd); return false; }
    return e == EACCES;
#else
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return false;                   // no requests folder: nobody runs
    bool held = flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    close(fd);                                  // (drops the lock if we just took it)
    return held;
#endif
}

static bool CoordinatorStarting(void)
{
    return coord.tried && !coord.error[0] && clockTime - coord.startTime < COORD_START_TIME && !coord.term.exited;
}

// Listed coordinator-mode requests that aren't finished yet
static int OpenQueueRequests(void)
{
    int n = 0;
    for (int i = 0; i < reqCount; i++) n += Listed(&reqs[i]) && reqs[i].queue && !reqs[i].done;
    return n;
}

static void CoordinatorCheck(void)
{
    if (!QueueMode()) return;
    bool relevant = coord.tried || coord.alive == 1;
    for (int i = 0; i < reqCount && !relevant; i++) relevant = Listed(&reqs[i]) && reqs[i].queue;
    if (!relevant) return;
    coord.checkTimer -= POLL_INTERVAL;
    if (coord.checkTimer > 0.0f && coord.alive >= 0) return;
    coord.checkTimer = COORD_CHECK_INTERVAL;
    int alive = CoordinatorAlive();
    if (alive != coord.alive) TraceLog(LOG_INFO, "monolith: coordinator %s", alive ? "running" : "not running");
    coord.alive = alive;
}

static const char *CoordinatorLine(Color *c)
{
    if (!QueueMode()) return NULL;
    int listed = 0, queued = 0, running = 0;
    for (int i = 0; i < reqCount; i++) {
        const OpenRequest *q = &reqs[i];
        if (!Listed(q) || !q->queue) continue;
        listed++;
        if (q->done) continue;
        if (strcmp(q->state, "queued") == 0) queued++;
        else running++;
    }
    if (!listed) return NULL;
    *c = (Color){ 200, 205, 215, 200 };
    if (coord.alive == 1) {
        if (!queued && !running) return "coordinator: running, idle";
        return TextFormat("coordinator: running, %d queued, %d running", queued, running);
    }
    if (CoordinatorStarting()) return "coordinator: starting ...";
    *c = (Color){ 255, 170, 110, 230 };
    if (coord.error[0]) return TextFormat("coordinator: not started: %s (the requests stay queued)", coord.error);
    if (!queued && !running) { *c = (Color){ 200, 205, 215, 160 }; return "coordinator: not running"; }
    return "coordinator: not running (the next request starts one; queued requests wait for it)";
}

static void CoordinatorTitle(char *out, int outSize)
{
    if (requestsHash) snprintf(out, outSize, "monolith-claude: coordinator %08x", requestsHash);
    else snprintf(out, outSize, "monolith-claude: coordinator");
}

// Value of --model for the coordinator; NULL when MONOLITH_CLAUDE_ARGS has its own --model
static const char *CoordinatorModel(void)
{
    ArgList a;
    if (!ClaudeArgs(&a, NULL, false)) return NULL;
    bool own = false;
    for (int i = 0; i < a.count; i++) own |= strcmp(a.tok[i], "--model") == 0 || strncmp(a.tok[i], "--model=", 8) == 0;
    FreeArgs(&a);
    return own ? NULL : EnvOr("MONOLITH_COORDINATOR_MODEL", "sonnet");
}

// Makes sure a coordinator runs or is starting; note: a few words for the HUD message
static bool StartCoordinator(char *note, int noteSize)
{
    char lock[1300];
    MakeDirectory(coordDir);        // raylib: recursive, fine if it exists
    snprintf(lock, sizeof(lock), "%slock", coordDir);
    if (CoordinatorAlive()) {
        coord.alive = 1;
        snprintf(note, noteSize, "the coordinator takes it");
        return true;
    }
    if (CoordinatorStarting()) { snprintf(note, noteSize, "the coordinator is starting"); return true; }
    coord.tried = true;
    coord.startTime = clockTime;
    coord.error[0] = 0;
    coord.alive = 0;
    coord.checkTimer = 0.0f;

    char title[200], prompt[1600], err[512] = "";
    CoordinatorTitle(title, sizeof(title));
    snprintf(prompt, sizeof(prompt), "You are the monolith request coordinator. Read .claude/skills/coordinator/SKILL.md in the repository root "
                                     "and follow it exactly. Requests folder: %s", requestsDir);
    TermWindow t = { 0 };
    char mark[160] = "";
#ifndef _WIN32
    snprintf(mark, sizeof(mark), "%s", coord.term.mark);
#endif
    if (!Launch(title, mark, prompt, CoordinatorModel(), lock, &t, err, sizeof(err))) {
        snprintf(coord.error, sizeof(coord.error), "%s", err);
        snprintf(note, noteSize, "the coordinator could not start: %s; the request stays queued", err);
        TraceLog(LOG_WARNING, "monolith: coordinator not started: %s", err);
        return false;
    }
    t.launched = true;
    coord.term = t;
    snprintf(note, noteSize, "starting the request coordinator in a new terminal");
    return true;
}

// The bundle in q->dir is complete: queue it and make sure a coordinator runs
static bool QueueBundle(OpenRequest *q, const char *what, const char *model, char *msg, int msgSize)
{
    q->queue = true;
    if (!WriteState(q->dir, "queued")) {
        snprintf(msg, msgSize, "cannot write %sstate (disk full?)", q->dir);
        TraceLog(LOG_WARNING, "monolith: %s", msg);
        return false;
    }
    snprintf(q->state, sizeof(q->state), "queued");
    TraceLog(LOG_INFO, "monolith: request %s queued", q->id);
    char note[700];
    StartCoordinator(note, sizeof(note));
    if (model) snprintf(msg, msgSize, "%s queued [%d] (%s): %s (%s)", what, q->number, model, q->id, note);
    else snprintf(msg, msgSize, "%s queued [%d]: %s (%s)", what, q->number, q->id, note);
    return true;
}

// CTRL+K n on a queued / running request (LauncherCloseNumber)
static int CancelRequest(OpenRequest *q, char *msg, int msgSize)
{
    if (q->cancelAsked) {
        snprintf(msg, msgSize, "%s: cancel already asked, waiting for the coordinator", q->slug);
        return 1;
    }
    ReadState(q);
    bool queued = strcmp(q->state, "queued") == 0;
    if (!queued && !(confirmSeq == q->seq && clockTime <= confirmUntil)) {
        confirmSeq = q->seq;
        confirmUntil = clockTime + CONFIRM_TIME;
        snprintf(msg, msgSize, "press %d again to cancel running request %s", q->number % 10, q->slug);
        return 0;
    }
    if (confirmSeq == q->seq) confirmSeq = 0;
    char path[1400], stamp[32], line[64];
    Stamp(stamp, sizeof(stamp));
    snprintf(line, sizeof(line), "%s\n", stamp);
    snprintf(path, sizeof(path), "%scancel", q->dir);
    if (!WriteText(path, line)) {
        snprintf(msg, msgSize, "cannot write %s", path);
        return -1;
    }
    q->cancelAsked = true;
    if (queued && !CoordinatorAlive() && !CoordinatorStarting()) {
        // Nobody else can: finish it here (result.md first, the HUD reads it on "cancelled")
        snprintf(path, sizeof(path), "%sresult.md", q->dir);
        WriteText(path, "Cancelled by the developer before it started\n\nNothing was changed.\n");
        WriteState(q->dir, "cancelled");
        snprintf(msg, msgSize, "cancelled %s", q->slug);
    } else snprintf(msg, msgSize, "cancelling %s (the coordinator %s)", q->slug, queued ? "skips it" : "stops its subagent and reports what changed");
    TraceLog(LOG_INFO, "monolith: %s", msg);
    return 1;
}

int LauncherCloseCoordinator(char *msg, int msgSize)
{
    InitPaths();
    if (!QueueMode()) { snprintf(msg, msgSize, "no coordinator (one terminal per request: MONOLITH_LAUNCH=per-request)"); return -1; }
    bool alive = CoordinatorAlive();
    if (!alive && !(coord.term.launched && !coord.term.exited)) {
        snprintf(msg, msgSize, "no coordinator running");
        return -1;
    }
    int open = OpenQueueRequests();
    bool confirmed = coord.confirmUntil > 0.0 && clockTime <= coord.confirmUntil;
    if (open && !confirmed) {
        coord.confirmUntil = clockTime + CONFIRM_TIME;
        snprintf(msg, msgSize, "%d request%s queued or running: CTRL+SHIFT+K again closes the coordinator anyway (the next one takes them up)",
                 open, open == 1 ? "" : "s");
        return 0;
    }
    coord.confirmUntil = 0.0;
    CloseOutcome o = CloseTerm(&coord.term, "the coordinator");
    if (o == CLOSED_WINDOW) snprintf(msg, msgSize, "closed the coordinator");
#ifdef _WIN32
    else snprintf(msg, msgSize, "close the coordinator window by hand (not supported on Windows)");
#else
    else if (alive) snprintf(msg, msgSize, "the coordinator runs, but its window can't be closed from here (not started by this game run, or no i3 / sway): close it by hand");
    else snprintf(msg, msgSize, "the coordinator's window is already gone");
#endif
    coord.alive = -1;               // test the lock again at the next poll
    TraceLog(LOG_INFO, "monolith: %s", msg);
    return 1;
}

// #endregion

// ======================================================================================
// #region Submit
// ======================================================================================

bool LauncherSubmit(const DesignRequest *req, Image view, Image marked, char *msg, int msgSize)
{
    if (MS_CompiledSource(scene, NULL) != MS_SOURCE_TEXT) {
        snprintf(msg, msgSize, "%s is a compiled map: requests change the map's text (load its .map)", scene->path);
        return false;
    }
    InitPaths();
    Bundle *Bp = calloc(1, sizeof(Bundle));
    if (!Bp) { snprintf(msg, msgSize, "out of memory"); return false; }
    bool result = false;
#define B (*Bp)

    // Folder name: local time stamp + slug, made unique if a request with the same
    // name was submitted within the same second
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char stamp[32] = "0000-00-00_000000";
    if (tm) {
        strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", tm);
        strftime(B.created, sizeof(B.created), "%Y-%m-%d %H:%M:%S", tm);
    }
    ModelQuery mq = { .prompt = req->prompt, .kind = "edit", .selected = req->selectedCount,
                      .regions = req->regionCount, .volumes = req->volumeCount, .hit = req->hit.obj.kind != MS_OBJ_NONE,
                      .emptyMap = scene->boxCount + scene->modelCount <= 2, .noBrief = !scene->brief[0] };
    LauncherChooseModel(&mq, &B.model);
    B.userPrompt = req->prompt + B.model.promptSkip;
    TraceLog(LOG_INFO, "monolith: model %s (%s tier: %s)", B.model.name, ModelTierName(B.model.tier), B.model.reason);
    MakeSlug(B.userPrompt, B.slug, sizeof(B.slug));

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
        goto done;
    }
    strcat(dir, "/");

    if (!BuildBundle(&B, req)) {
        snprintf(msg, msgSize, "out of memory (bundle %s not written)", dir);
        goto done;
    }

    bool ok = true;
    char path[1400];
    snprintf(path, sizeof(path), "%sview.png", dir);
    if (view.data) ok &= PngWrite(view, path);
    snprintf(path, sizeof(path), "%sview-marked.png", dir);
    if (marked.data) ok &= PngWrite(marked, path);
    // One crop per region, from view.png's pixels
    for (int i = 0; i < req->regionCount && view.data; i++) {
        Rectangle r = req->regions[i].rect;
        float x0 = Clamp(r.x, 0.0f, (float)view.width), y0 = Clamp(r.y, 0.0f, (float)view.height);
        float x1 = Clamp(r.x + r.width, 0.0f, (float)view.width), y1 = Clamp(r.y + r.height, 0.0f, (float)view.height);
        if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) continue;
        Image crop = ImageFromImage(view, (Rectangle){ x0, y0, x1 - x0, y1 - y0 });
        snprintf(path, sizeof(path), "%sregion-%d.png", dir, i + 1);
        ok &= PngWrite(crop, path);
        UnloadImage(crop);
    }
    snprintf(path, sizeof(path), "%srequest.md", dir);
    ok &= WriteRequestMd(path, &B);
    snprintf(path, sizeof(path), "%srequest.json", dir);
    ok &= WriteRequestJson(path, &B);
    if (!ok) {
        snprintf(msg, msgSize, "cannot write the request bundle %s (disk full?)", dir);
        TraceLog(LOG_WARNING, "monolith: %s", msg);
        goto done;
    }

    OpenRequest *q = NewSlot();
    if (!q) { snprintf(msg, msgSize, "out of memory. Bundle kept: %s", dir); goto done; }
    snprintf(q->id, sizeof(q->id), "%s", B.id);
    snprintf(q->slug, sizeof(q->slug), "%s", B.slug);
    snprintf(q->dir, sizeof(q->dir), "%s", dir);
    snprintf(q->map, sizeof(q->map), "%s", B.mapPath);
    q->submitTime = clockTime;
    q->eye = req->eye;
    q->yaw = req->yaw;
    q->pitch = req->pitch;
    q->hasCamera = true;

    // Coordinator mode: queued even when the coordinator can't start (the next one takes
    // it), so the prompt box closes; a second Enter would only queue it twice
    if (QueueMode()) {
        result = QueueBundle(q, "request", B.model.name, msg, msgSize);
        if (result && B.model.droppedModelArg)
            snprintf(msg + strlen(msg), msgSize - strlen(msg), "; a --model without a name in MONOLITH_CLAUDE_ARGS was ignored");
        goto done;
    }

    char err[512] = "", title[300], mark[160];
    RequestTitle(B.id, B.slug, title, sizeof(title));
    snprintf(mark, sizeof(mark), "_monolith_%s", B.id);
    q->term.launched = Launch(title, mark, B.prompt, B.model.addFlag ? B.model.name : NULL, NULL, &q->term, err, sizeof(err));
    if (!q->term.launched) {
        snprintf(msg, msgSize, "launch failed: %s. Bundle kept: %s", err, dir);
        TraceLog(LOG_WARNING, "monolith: %s", msg);
        TraceLog(LOG_WARNING, "monolith: run it by hand: cd '%s' && claude --model %s \"%s\"", rootDir, B.model.name, B.prompt);
        goto done;
    }
    snprintf(msg, msgSize, "request sent (%s): %s (Claude Code opens in a new terminal)%s", B.model.name, B.id,
             B.model.droppedModelArg ? "; a --model without a name in MONOLITH_CLAUDE_ARGS was ignored" : "");
    result = true;
done:
#undef B
    FreeBundle(Bp);
    return result;
}

bool LauncherStartBundle(const char *id, const char *slug, const char *claudePrompt, const char *model, char *msg, int msgSize)
{
    InitPaths();
    OpenRequest *q = NewSlot();
    if (!q) { snprintf(msg, msgSize, "out of memory"); return false; }
    snprintf(q->id, sizeof(q->id), "%s", id);
    snprintf(q->slug, sizeof(q->slug), "%s", slug);
    snprintf(q->dir, sizeof(q->dir), "%s%s/", requestsDir, id);
    q->submitTime = clockTime;
    if (QueueMode()) return QueueBundle(q, "request", model, msg, msgSize);
    char err[512] = "", title[300], mark[160];
    RequestTitle(id, slug, title, sizeof(title));
    snprintf(mark, sizeof(mark), "_monolith_%s", id);
    q->term.launched = Launch(title, mark, claudePrompt, model, NULL, &q->term, err, sizeof(err));
    if (!q->term.launched) {
        snprintf(msg, msgSize, "launch failed: %s. Bundle kept: %s", err, q->dir);
        TraceLog(LOG_WARNING, "monolith: run it by hand: cd '%s' && claude \"%s\"", rootDir, claudePrompt);
        return false;
    }
    snprintf(msg, msgSize, "request sent: %s (Claude Code opens in a new terminal)", id);
    return true;
}

int LauncherOpenOnMap(const char *mapPath, int *newest)
{
    char abs[1024];
    AbsPath(mapPath, abs, sizeof(abs));
    int count = 0;
    long seq = -1;
    *newest = 0;
    for (int i = 0; i < reqCount; i++) {
        const OpenRequest *q = &reqs[i];
        if (!Listed(q) || q->done || strcmp(q->map, abs) != 0) continue;
        count++;
        if (q->seq > seq) { seq = q->seq; *newest = q->number; }
    }
    return count;
}

const char *LauncherRequestsDir(void)
{
    InitPaths();
    return requestsDir;
}

const char *LauncherRootDir(void)
{
    InitPaths();
    return rootDir;
}

void LauncherScrubEnv(void)
{
#ifdef _WIN32
    _putenv("MONOLITH_ALLOW_WINDOW=");
#else
    unsetenv("MONOLITH_ALLOW_WINDOW");
#endif
}

// #endregion
