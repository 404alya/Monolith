// Launcher: request bundles, Claude Code in a new terminal, polling for result.md
//
// A submitted request becomes a self-contained folder requests/<YYYY-MM-DD_HHMMSS>_<slug>/
// (view.png, view-marked.png, region-<n>.png per screen region, request.md, request.json;
// bundle format 2, docs/design-v2.md section 5). Then a new terminal window runs
// `claude --allow-dangerously-skip-permissions --dangerously-skip-permissions --model <name>
// [MONOLITH_CLAUDE_ARGS] "Map edit request: read requests/<id>/request.md and carry it
// out."` in the repo root. The model is chosen per request (fast / default / heavy tier,
// see "Model choice" below) and recorded in request.md / request.json. The user's prompt never goes on a command line: it
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
//   - permissions: every session starts with --allow-dangerously-skip-permissions
//     --dangerously-skip-permissions, so the developer isn't asked to approve each
//     command; the guard rails live in CLAUDE.md / AI-MAP-TOOL.md instead.
//     MONOLITH_CLAUDE_SAFE=1 (any value but empty or "0") drops the two flags.
//   - model: --model <name> after the permission flags; MONOLITH_MODEL, MONOLITH_MODEL_FAST /
//     _DEFAULT / _HEAVY and an @tier prompt prefix steer it (region "Model choice").
//   - extra Claude Code flags: $MONOLITH_CLAUDE_ARGS, split on whitespace (no quoting),
//     after --model. Default none. A --model in them replaces the chosen one.
//   - workspace (i3 / sway): the new terminal is moved to workspace $MONOLITH_WORKSPACE
//     (default 10; empty = leave it where it opens) with `i3-msg` (`swaymsg` when
//     $SWAYSOCK is set) and the criteria [title="^monolith-claude: <slug>$"]. A detached
//     /bin/sh helper retries every 0.1 s for up to 10 s until the window exists, so the
//     game never waits. A criteria `move container to workspace` doesn't switch the
//     visible workspace; focus returns to the previously focused window. The window may
//     still flash up for a moment where it opened: an i3 `for_window` rule on the title
//     (terminals set it before the window is mapped) avoids that, and the IPC move then
//     finds it already there. Other window managers: i3-msg finds no i3 (exit 1) and the
//     helper stops at once. The same command also gives the window an i3 mark,
//     _monolith_<id> (also with MONOLITH_WORKSPACE empty), for closing it later.
//   - closing (region "Closing request windows"): the HUD list numbers the requests;
//     K / SHIFT+K / CTRL+K n in the game close a request's terminal through the mark
//     (`i3-msg [con_mark=...] kill`), else by SIGTERM / SIGKILL to the terminal's process
//     group, and take it off the list (the bundle stays). MONOLITH_CLOSE_ON_EXIT=1 closes
//     the finished ones when the game quits.
//
// Windows: the same bundle, then `cmd /c start "monolith-claude: <slug>" /D <root> cmd /k
// "<claude>" <permission flags> --model <name> ...` via _spawnvp (no workspace move) (<process.h>, so no <windows.h> clash with raylib). Only
// compile-tested; failures of `start` itself can't be seen from here.

#ifndef _WIN32
#   define _XOPEN_SOURCE 700           // POSIX 2008 + XSI: fork, setsid, readlink, realpath
#endif

#include "launcher.h"
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
#   include <signal.h>
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
    return (g >= 0 && g < scene->groupCount) ? scene->groups[g] : "(no group)";
}

// Object references ({kind, index}, runtime/monolith_scene.h). The bundle's per-object
// lookups go through these and the runtime's MS_ObjectRef / MS_ObjectBounds, so models
// work here once the runtime has them (MS_ObjectRef covers MS_OBJ_MODEL).
static bool ObjValid(MS_ObjRef o) { return MS_ObjectRef(scene, o) != NULL; }
static const char *ObjKindName(MS_ObjRef o) { return o.kind == MS_OBJ_BOX ? "box" : o.kind == MS_OBJ_MODEL ? "model" : "none"; }
static BoundingBox ObjBounds(MS_ObjRef o) { return MS_ObjectBounds(scene, o); }
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
static int ObjNth(MS_ObjRef o)
{
    int nth = 0, line = ObjLine(o);
    for (int i = 0; i < o.index; i++)
        if (MS_ObjectRef(scene, (MS_ObjRef){ o.kind, i })->line == line) nth++;
    return nth;
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
//           6. long text: > 280 bytes or 3+ sentences; or more than 2 screen regions
//   fast    7. at most 120 bytes, one sentence, a target (crosshair hit, a selection of at
//              most 100 objects, or exactly one region), a local edit word (move, raise,
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

#define CLASSIFY_MAX_WORDS 1024
typedef struct {
    char w[CLASSIFY_MAX_WORDS][24];     // lowercase; letters, or a number ("2.5")
    int n;
} Words;

static void SplitPromptWords(const char *s, Words *W)
{
    W->n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && W->n < CLASSIFY_MAX_WORDS) {
        bool alpha = *p < 128 && isalpha(*p), digit = *p < 128 && isdigit(*p);
        if (!alpha && !digit) { p++; continue; }
        int len = 0;
        char *w = W->w[W->n++];
        while (*p < 128 && (alpha ? isalpha(*p) : (isdigit(*p) || (*p == '.' && isdigit(p[1]))))) {
            if (len < 23) w[len++] = (char)tolower(*p);
            p++;
        }
        w[len] = 0;
    }
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

ModelTier ClassifyRequest(const ModelQuery *q, char *reason, int reasonSize)
{
    static Words W;
    const char *prompt = q->prompt ? q->prompt : "";
    SplitPromptWords(prompt, &W);
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
    if (q->regions > 2) { snprintf(reason, reasonSize, "%d screen regions", q->regions); return MODEL_TIER_HEAVY; }

    const char *edit = FindKeyword(&W, KW_EDIT);
    for (int i = 0; !edit && i + 1 < W.n; i++)
        if ((strcmp(W.w[i], "make") == 0 || strcmp(W.w[i], "set") == 0))
            for (int j = i + 1; j < W.n && j <= i + 4 && !edit; j++) if (IsNumber(W.w[j])) edit = "make/set + number";
    const char *create = FindKeyword(&W, KW_CREATE);
    bool target = q->hit || (q->selected > 0 && q->selected <= FAST_MAX_SELECTION) || q->regions == 1;
    if (q->selected > FAST_MAX_SELECTION) target = false;

    if (!edit) snprintf(reason, reasonSize, "no local edit word");
    else if (create) snprintf(reason, reasonSize, "creation word \"%s\"", create);
    else if (bytes > FAST_MAX_BYTES) snprintf(reason, reasonSize, "edit \"%s\" but %d bytes", edit, bytes);
    else if (sentences > 1) snprintf(reason, reasonSize, "edit \"%s\" but %d sentences", edit, sentences);
    else if (!target) snprintf(reason, reasonSize, "edit \"%s\" but no small target (hit/selection/1 region)", edit);
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
    char buf[1024], *tok[CLAUDE_ARGS_MAX];
    int n = SplitWords(getenv("MONOLITH_CLAUDE_ARGS"), buf, sizeof(buf), tok, CLAUDE_ARGS_MAX);
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
    char hitLine[MS_LINE_MAX + 1];
    bool hitHasLine;
    MS_ObjRef sel[MAX_SELECTION];       // valid selected objects
    int selCount;
    SelLine selLines[MAX_SELECTION];
    int selLineCount;
    MS_ObjRef selOrder[MAX_SELECTION];  // sel[] reordered so each line's objects are adjacent
    Nearby nearby[MS_MAX_GROUPS + 1];
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

static void BuildBundle(Bundle *B, const DesignRequest *req)
{
    B->req = req;
    B->stale = req->mapVersion != scene->version;
    B->yawDeg = Wrap(Deg(req->yaw), 0.0f, 360.0f);
    B->pitchDeg = Deg(req->pitch);
    B->fwd = (Vector3){ cosf(req->pitch) * sinf(req->yaw), sinf(req->pitch), cosf(req->pitch) * cosf(req->yaw) };
    B->right = (Vector3){ -cosf(req->yaw), 0.0f, sinf(req->yaw) };
    B->aspect = req->height > 0 ? (float)req->width / req->height : 0.0f;

    B->hit = ObjValid(req->hit.obj);
    if (B->hit) B->hitHasLine = MS_GetLine(scene, ObjLine(req->hit.obj), B->hitLine, sizeof(B->hitLine));

    // Selection: drop invalid references, then group by line
    for (int i = 0; i < req->selectedCount && i < MAX_SELECTION; i++)
        if (ObjValid(req->selected[i])) B->sel[B->selCount++] = req->selected[i];
    for (int i = 0; i < B->selCount; i++) {
        int line = ObjLine(B->sel[i]), k = 0;
        while (k < B->selLineCount && B->selLines[k].line != line) k++;
        if (k == B->selLineCount) B->selLines[B->selLineCount++] = (SelLine){ line, 0, 0 };
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
    char text[MS_LINE_MAX + 1];
    if (line > 0 && MS_GetLine(scene, line, text, sizeof(text))) {
        fprintf(f, "Map line %d:\n\n", line);
        MdFence(f, text, "");
    } else fprintf(f, "Not from a map line.\n\n");
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
    if (ObjMaterial(o)) fprintf(f, ", material \"%s\"", ObjMaterial(o));
}

#define MD_REGION_ROWS 40   // objects per region in request.md (request.json lists them all)

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
               "Depth = distance from the eye over the rays that hit something. Computed at submit time.\n\n");
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

        if (g->objectCount == 0) { fprintf(f, "No objects in this region.\n\n"); continue; }
        fprintf(f, "| share | object | group | line | min | max |\n|---|---|---|---|---|---|\n");
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

        // The map lines behind the listed objects, each quoted once
        static char text[MS_LINE_MAX + 1], block[16384];
        int lines[MD_REGION_ROWS], lineCount = 0, n = 0;
        block[0] = 0;
        for (int k = 0; k < rows; k++) {
            if (!ObjValid(g->objects[k].obj)) continue;
            int line = ObjLine(g->objects[k].obj);
            bool seen = false;
            for (int j = 0; j < lineCount; j++) seen |= lines[j] == line;
            if (seen || line <= 0 || !MS_GetLine(scene, line, text, sizeof(text))) continue;
            lines[lineCount++] = line;
            if (n < (int)sizeof(block) - 1) n += snprintf(block + n, sizeof(block) - n, "%d: %s\n", line, text);
        }
        if (lineCount > 0) {
            fprintf(f, "Map lines of these objects:\n\n");
            MdFence(f, block, "");
        }
    }
}

static bool WriteRequestMd(const char *path, const Bundle *B)
{
    const DesignRequest *r = B->req;
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    fprintf(f, "# Map edit request: %s\n\n", B->slug);
    fprintf(f, "Model: %s (%s tier: %s)\n\n", B->model.name, ModelTierName(B->model.tier), B->model.reason);
    fprintf(f, "Request `%s`, submitted %s from the monolith map designer. Bundle format 2: besides the "
               "crosshair hit and the selected objects it may contain **screen regions** the developer "
               "dragged over the frame (see *Screen regions* below and `AI-MAP-TOOL.md`).\n\n", B->id, B->created);
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
               "frame with the selection highlighted, the crosshair hit marked and the screen regions "
               "outlined and numbered)%s.\n\n", r->regionCount ? ", plus one crop `region-<n>.png` per region" : "");

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
        fprintf(f, "- group: \"%s\", material \"%s\"\n\n", GroupName(ObjGroup(o)), ObjMaterial(o) ? ObjMaterial(o) : "(own)");
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
                ObjMaterial(first) ? ObjMaterial(first) : "(own)", sl->count, model ? "model" : "box",
                sl->count == 1 ? "" : model ? "s" : "es");
        MdLine(f, sl->line);
        if (model) fprintf(f, "| model | copy | file | position | rotation (yaw pitch roll) | scale | world min | world max |\n"
                              "|---|---|---|---|---|---|---|---|\n");
        else fprintf(f, "| box | min | max | centre | size |\n|---|---|---|---|---|\n");
        for (int i = sl->first; i < sl->first + sl->count; i++) {
            MS_ObjRef o = B->selOrder[i];
            BoundingBox b = ObjBounds(o);
            if (o.kind == MS_OBJ_MODEL) {
                const MS_ModelInstance *m = &scene->models[o.index];
                fprintf(f, "| #%d | %d | `%s` | ", o.index, ObjNth(o), scene->modelFiles[m->file].path); MdVec(f, m->position);
                fprintf(f, " | "); MdVec(f, m->rotationDeg);
                fprintf(f, " | "); MdVec(f, m->scale);
                fprintf(f, " | "); MdVec(f, b.min);
                fprintf(f, " | "); MdVec(f, b.max);
                fprintf(f, " |\n");
                continue;
            }
            fprintf(f, "| #%d | ", o.index); MdVec(f, b.min);
            fprintf(f, " | "); MdVec(f, b.max);
            fprintf(f, " | "); MdVec(f, Vector3Scale(Vector3Add(b.min, b.max), 0.5f));
            fprintf(f, " | "); MdVec(f, Vector3Subtract(b.max, b.min));
            fprintf(f, " |\n");
        }
        fprintf(f, "\n");
    }

    WriteRegionsMd(f, B);

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
    char text[MS_LINE_MAX + 1];
    if (line > 0 && MS_GetLine(scene, line, text, sizeof(text))) JsonStr(f, text);
    else fputs("null", f);
}

static void JsonMaterial(FILE *f, MS_ObjRef o)
{
    if (ObjMaterial(o)) JsonStr(f, ObjMaterial(o));
    else fputs("null", f);
}

// A model instance (docs/design-v2.md 5): file, transform as written, the resulting
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

static bool WriteRequestJson(const char *path, const Bundle *B)
{
    const DesignRequest *r = B->req;
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    fprintf(f, "{\n  \"format\": 2,\n  \"kind\": \"edit\",\n  \"id\": "); JsonStr(f, B->id);
    fprintf(f, ",\n  \"slug\": "); JsonStr(f, B->slug);
    fprintf(f, ",\n  \"created\": "); JsonStr(f, B->created);
    fprintf(f, ",\n  \"instructions\": ");
    JsonStr(f, TextFormat("Read CLAUDE.md and AI-MAP-TOOL.md in the repository root first, then carry out the "
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
        if (o.kind == MS_OBJ_MODEL) { fprintf(f, ",\n    \"model\": "); JsonModel(f, o.index); }
        fprintf(f, ",\n    \"group\": "); JsonStr(f, GroupName(ObjGroup(o)));
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
            fprintf(f, ", \"nth\": %d, \"material\": ", ObjNth(o));
            JsonMaterial(f, o);
            fprintf(f, ", \"min\": ");
            JsonVec(f, b.min);
            fprintf(f, ", \"max\": "); JsonVec(f, b.max);
            fprintf(f, ", \"center\": "); JsonVec(f, Vector3Scale(Vector3Add(b.min, b.max), 0.5f));
            fprintf(f, ", \"size\": "); JsonVec(f, Vector3Subtract(b.max, b.min));
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
        fprintf(f, "\n    }");
    }
    fprintf(f, "%s]", r->regionCount ? "\n  " : "");

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

typedef struct {
    bool used;
    long seq;                   // submit order, for "most recent first"
    char id[128], slug[SLUG_MAX + 1];
    char dir[1300];             // bundle folder, trailing slash
    double submitTime, doneTime;
    bool launched;              // false: launching failed, the developer runs it by hand
    bool exited;                // the terminal process is gone (result may still come)
    bool done;
    bool closed;                // closed from the game: off the list, the bundle stays
    int number;                 // [n] in the HUD list, stable while listed (1..MAX_OPEN_REQUESTS)
    long resultSize;            // size of result.md at the last poll (wait until it settles)
    char summary[160];          // first line of result.md
    Vector3 eye;
    float yaw, pitch;
#ifndef _WIN32
    pid_t pid;                  // the terminal (session + process group leader, setsid)
    char mark[160];             // i3/sway mark set on its window by the workspace helper
    int killStage;              // after a close: 1 = WM closed it, wait; 2 = SIGTERM sent, wait
    double killTime;            // clockTime of the next escalation step
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
static long confirmSeq;         // running request whose close waits for a second key press
static double confirmUntil;     // ... until this clockTime

#ifndef _WIN32
// Every child is reaped with waitpid(pid, WNOHANG) from LauncherUpdate. Not waitpid(-1):
// that would also collect children some other code might start and wait for. Not a
// SIGCHLD handler: it would interrupt raylib's and the driver's system calls. Children
// are tracked here separately from reqs[], whose slots get recycled.
#define MAX_CHILDREN 64
static pid_t children[MAX_CHILDREN];
#endif

static bool Listed(const OpenRequest *q) { return q->used && !q->closed; }

static OpenRequest *NewSlot(void)
{
    OpenRequest *pick = NULL;
    for (int i = 0; i < MAX_OPEN_REQUESTS && !pick; i++) if (!reqs[i].used) pick = &reqs[i];
    // Closed requests next (preferably ones whose terminal is no longer being shut down)
    for (int pass = 0; pass < 2 && !pick; pass++)
        for (int i = 0; i < MAX_OPEN_REQUESTS && !pick; i++)
#ifndef _WIN32
            if (reqs[i].closed && (pass == 1 || reqs[i].killStage == 0)) pick = &reqs[i];
#else
            if (reqs[i].closed) pick = &reqs[i];
#endif
    // Full: recycle the oldest finished request, else the oldest one at all
    for (int pass = 0; pass < 2 && !pick; pass++)
        for (int i = 0; i < MAX_OPEN_REQUESTS; i++)
            if ((pass == 1 || reqs[i].done) && (!pick || reqs[i].seq < pick->seq)) pick = &reqs[i];
    memset(pick, 0, sizeof(*pick));
    pick->used = true;
    pick->seq = nextSeq++;
    // Lowest number no listed request has, so the numbers in the HUD don't shift
    for (int n = 1; !pick->number; n++) {
        bool taken = false;
        for (int i = 0; i < MAX_OPEN_REQUESTS; i++) if (&reqs[i] != pick && Listed(&reqs[i]) && reqs[i].number == n) taken = true;
        if (!taken) pick->number = n;
    }
    return pick;
}

#ifndef _WIN32
// Is this child of ours still running? Reaps it (and marks its requests exited) if not.
// While a child is unreaped its pid, and so its process group id, can't be reused: that
// is what makes kill(-pid) safe in CloseRequest.
static bool ChildAlive(pid_t pid)
{
    if (pid <= 0) return false;
    int slot = -1;
    for (int i = 0; i < MAX_CHILDREN; i++) if (children[i] == pid) slot = i;
    if (slot < 0) return false;             // reaped already (or never tracked)
    if (waitpid(pid, NULL, WNOHANG) == 0) return true;
    for (int k = 0; k < MAX_OPEN_REQUESTS; k++)
        if (reqs[k].used && reqs[k].pid == pid) reqs[k].exited = true;
    children[slot] = 0;                     // reaped (or not ours any more: ECHILD)
    return false;
}

static void CloseEscalate(void);
#endif

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
    for (int i = 0; i < MAX_CHILDREN; i++) if (children[i] > 0) ChildAlive(children[i]);
    CloseEscalate();
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

// The terminal window may still be open (the process we started is still running). Unknown
// for terminals that hand the window to a daemon and exit (kitty --single-instance, wezterm)
static bool WindowMaybeOpen(const OpenRequest *q)
{
    return q->launched && !q->exited;
}

int LauncherDrawHud(int x, int y)
{
    const int size = 18, step = 22, maxRows = 6;
    int rows = 0;
    long below = 1L << 30;      // walk the list from the newest request down
    while (rows < maxRows) {
        OpenRequest *q = NULL;
        for (int i = 0; i < MAX_OPEN_REQUESTS; i++)
            if (Listed(&reqs[i]) && reqs[i].seq < below && (!q || reqs[i].seq > q->seq)) q = &reqs[i];
        if (!q) break;
        below = q->seq;

        const char *text;
        Color c = { 235, 232, 225, 220 };
        if (q->done) {
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
            int t = (int)(clockTime - q->submitTime);
            if (!q->launched) {
                c = (Color){ 255, 170, 110, 230 };
                text = TextFormat("[%d] [!] %s not launched, run it by hand (see log), waiting (%02d:%02d)", q->number, q->slug, t / 60, t % 60);
            } else if (q->exited)
                // Not necessarily a failure: some terminals hand the window to a daemon
                // and exit right away (kitty --single-instance, wezterm with a mux)
                text = TextFormat("[%d] ... %s waiting for result.md, terminal process gone (%02d:%02d)", q->number, q->slug, t / 60, t % 60);
            else
                text = TextFormat("[%d] ... %s running (%02d:%02d)", q->number, q->slug, t / 60, t % 60);
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

// Every Claude Code session starts with these, unless MONOLITH_CLAUDE_SAFE is set
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

static bool Launch(const char *id, const char *slug, const char *prompt, const char *model, OpenRequest *q, char *err, int errSize)
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
    for (int i = 0; i < SKIP_PERMISSION_COUNT && !ClaudeSafe(); i++)
        len += snprintf(cmd + len, sizeof(cmd) - len, " %s", SKIP_PERMISSION_FLAGS[i]);
    if (model && len < (int)sizeof(cmd)) len += snprintf(cmd + len, sizeof(cmd) - len, " --model \"%s\"", model);
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
    // The title is "monolith-claude: <slug>" with a [a-z0-9-] slug, the mark "_monolith_<id>"
    // (id: [0-9_a-z-]): no regex specials, no quotes
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
    for (int i = 0; i < MAX_CHILDREN; i++)      // reaped by LauncherUpdate like the terminals
        if (children[i] <= 0) { children[i] = pid; break; }
    if (*ws) TraceLog(LOG_INFO, "monolith: moving '%s' to workspace %s via %s (pid %d)", title, ws, tool, (int)pid);
    else TraceLog(LOG_INFO, "monolith: marking '%s' via %s (pid %d)", title, tool, (int)pid);
}

static bool Launch(const char *id, const char *slug, const char *prompt, const char *model, OpenRequest *q, char *err, int errSize)
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
    char *argv[16 + 8 + SKIP_PERMISSION_COUNT + 2 + CLAUDE_ARGS_MAX + 4];
    int n = 0;
    for (int i = 0; i < termCount; i++) argv[n++] = termTok[i];
    if (ts->pre) argv[n++] = (char *)ts->pre;
    if (ts->titleFlag) { argv[n++] = (char *)ts->titleFlag; argv[n++] = title; }
    if (ts->dirFlag) { argv[n++] = (char *)ts->dirFlag; argv[n++] = dir; }
    if (ts->exec) argv[n++] = (char *)ts->exec;
    argv[n++] = claude;
    for (int i = 0; i < SKIP_PERMISSION_COUNT && !ClaudeSafe(); i++) argv[n++] = (char *)SKIP_PERMISSION_FLAGS[i];
    if (model) { argv[n++] = "--model"; argv[n++] = (char *)model; }
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
    // (an id is [0-9_a-z-] for edit requests; keep the mark free of quotes in any case)
    snprintf(q->mark, sizeof(q->mark), "_monolith_%s", id);
    for (char *c = q->mark; *c; c++) if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') *c = '_';
    MarkAndMove(title, q->mark);
    return true;
}

#endif

// #endregion

// ======================================================================================
// #region Closing request windows (K / SHIFT+K / CTRL+K n, MONOLITH_CLOSE_ON_EXIT)
// ======================================================================================
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
// 1: the WM closed a window with the request's mark, 0: no such window, -1: no i3 / sway
static int WmCloseMarked(const OpenRequest *q)
{
    if (!q->mark[0]) return 0;
    const char *tool;
    char toolPath[4096];
    if (!WmTool(toolPath, sizeof(toolPath), &tool)) return -1;
    // `kill` alone reports success even when nothing matches; re-adding the mark first
    // fails (exit 2) when no window has it, so the exit status says whether one was closed
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "[con_mark=\"^%s$\"] mark --add \"%s\", kill", q->mark, q->mark);
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

static void SignalGroup(const OpenRequest *q, int sig)
{
    if (ChildAlive(q->pid) && kill(-q->pid, sig) == 0)
        TraceLog(LOG_INFO, "monolith: %s sent to the terminal of %s (process group %d)",
                 sig == SIGKILL ? "SIGKILL" : "SIGTERM", q->id, (int)q->pid);
}

// From LauncherUpdate: WM close -> SIGTERM -> SIGKILL for terminals that linger
static void CloseEscalate(void)
{
    for (int i = 0; i < MAX_OPEN_REQUESTS; i++) {
        OpenRequest *q = &reqs[i];
        if (!q->used || !q->killStage || clockTime < q->killTime) continue;
        if (!ChildAlive(q->pid)) { q->killStage = 0; continue; }
        if (q->killStage == 1) { SignalGroup(q, SIGTERM); q->killStage = 2; q->killTime = clockTime + CLOSE_GRACE; }
        else { SignalGroup(q, SIGKILL); q->killStage = 0; }
    }
}
#endif

static CloseOutcome CloseRequest(OpenRequest *q)
{
    q->closed = true;
    if (confirmSeq == q->seq) confirmSeq = 0;
    if (!q->launched) return CLOSED_LISTED;
#ifdef _WIN32
    return CLOSED_LISTED;
#else
    const char *method = getenv("MONOLITH_CLOSE_METHOD");
    bool useWm = !method || strcmp(method, "signal") != 0;
    bool useSignal = !method || strcmp(method, "wm") != 0;
    int wm = useWm ? WmCloseMarked(q) : -1;
    bool alive = ChildAlive(q->pid);
    TraceLog(LOG_INFO, "monolith: closing %s: window manager %s, terminal process %s", q->id,
             wm == 1 ? "closed its window" : wm == 0 ? "found no window" : "not used", alive ? "running" : "gone");
    if (wm == 1) {
        if (alive && useSignal) { q->killStage = 1; q->killTime = clockTime + CLOSE_GRACE; }
        return CLOSED_WINDOW;
    }
    if (alive && useSignal) {
        SignalGroup(q, SIGTERM);
        q->killStage = 2;
        q->killTime = clockTime + CLOSE_GRACE;
        return CLOSED_WINDOW;
    }
    return CLOSED_GONE;
#endif
}

static void CloseMessage(const OpenRequest *q, CloseOutcome o, char *msg, int msgSize)
{
    if (o == CLOSED_WINDOW) snprintf(msg, msgSize, "closed %s", q->slug);
#ifdef _WIN32
    else if (q->launched) snprintf(msg, msgSize, "%s off the list; close its window by hand (not supported on Windows)", q->slug);
#endif
    else if (o == CLOSED_LISTED) snprintf(msg, msgSize, "%s off the list (it was never launched)", q->slug);
    else snprintf(msg, msgSize, "%s: window already gone", q->slug);
    TraceLog(LOG_INFO, "monolith: %s", msg);
}

bool LauncherCloseLatestDone(char *msg, int msgSize)
{
    OpenRequest *q = NULL;
    bool running = false;
    for (int i = 0; i < MAX_OPEN_REQUESTS; i++) {
        if (!Listed(&reqs[i])) continue;
        if (!reqs[i].done) running = true;
        else if (!q || reqs[i].doneTime > q->doneTime) q = &reqs[i];
    }
    if (!q) {
        snprintf(msg, msgSize, "no finished request to close%s", running ? " (CTRL+K, then its number, stops a running one)" : "");
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
    for (int i = 0; i < MAX_OPEN_REQUESTS; i++) {
        OpenRequest *q = &reqs[i];
        if (!Listed(q) || !q->done) continue;
        lastOutcome = CloseRequest(q);
        last = q;
        count++;
        if (lastOutcome == CLOSED_GONE) gone++;
    }
    if (!last) snprintf(msg, msgSize, "no finished request to close");
    else if (count == 1) CloseMessage(last, lastOutcome, msg, msgSize);
    else {
        snprintf(msg, msgSize, "closed %d finished requests", count);
        if (gone) snprintf(msg + strlen(msg), msgSize - strlen(msg), " (%d window%s already gone)", gone, gone == 1 ? "" : "s");
        TraceLog(LOG_INFO, "monolith: %s", msg);
    }
    return count;
}

int LauncherCloseNumber(int number, char *msg, int msgSize)
{
    OpenRequest *q = NULL;
    for (int i = 0; i < MAX_OPEN_REQUESTS; i++) if (Listed(&reqs[i]) && reqs[i].number == number) q = &reqs[i];
    if (!q) { snprintf(msg, msgSize, "no request [%d] in the list", number); return -1; }
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
    for (int i = 0; i < MAX_OPEN_REQUESTS; i++) if (Listed(&reqs[i]) && (!q || reqs[i].seq > q->seq)) q = &reqs[i];
    return q ? q->number : 0;
}

bool LauncherClosing(void)
{
#ifndef _WIN32
    for (int i = 0; i < MAX_OPEN_REQUESTS; i++)
        if (reqs[i].used && reqs[i].closed && reqs[i].launched && ChildAlive(reqs[i].pid)) return true;
#endif
    return false;
}

void LauncherCloseOnExit(void)
{
    const char *e = getenv("MONOLITH_CLOSE_ON_EXIT");
    if (!e || !*e || strcmp(e, "0") == 0) return;
    // No escalation after this (the game is gone): the WM close, or SIGTERM where the WM
    // route isn't available, is all there is
    char msg[256];
    if (LauncherCloseAllDone(msg, sizeof(msg)) > 0) TraceLog(LOG_INFO, "monolith: on exit: %s", msg);
}

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
    ModelQuery mq = { .prompt = req->prompt, .kind = "edit", .selected = req->selectedCount,
                      .regions = req->regionCount, .hit = req->hit.obj.kind != MS_OBJ_NONE,
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
    // One crop per region, from view.png's pixels
    for (int i = 0; i < req->regionCount && view.data; i++) {
        Rectangle r = req->regions[i].rect;
        float x0 = Clamp(r.x, 0.0f, (float)view.width), y0 = Clamp(r.y, 0.0f, (float)view.height);
        float x1 = Clamp(r.x + r.width, 0.0f, (float)view.width), y1 = Clamp(r.y + r.height, 0.0f, (float)view.height);
        if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) continue;
        Image crop = ImageFromImage(view, (Rectangle){ x0, y0, x1 - x0, y1 - y0 });
        snprintf(path, sizeof(path), "%sregion-%d.png", dir, i + 1);
        ok &= ExportImage(crop, path);
        UnloadImage(crop);
    }
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
    q->launched = Launch(B.id, B.slug, B.prompt, B.model.addFlag ? B.model.name : NULL, q, err, sizeof(err));
    if (!q->launched) {
        snprintf(msg, msgSize, "launch failed: %s. Bundle kept: %s", err, dir);
        TraceLog(LOG_WARNING, "monolith: %s", msg);
        TraceLog(LOG_WARNING, "monolith: run it by hand: cd '%s' && claude --model %s \"%s\"", rootDir, B.model.name, B.prompt);
        return false;
    }
    snprintf(msg, msgSize, "request sent (%s): %s (Claude Code opens in a new terminal)", B.model.name, B.id);
    return true;
}

bool LauncherStartBundle(const char *id, const char *slug, const char *claudePrompt, const char *model, char *msg, int msgSize)
{
    InitPaths();
    OpenRequest *q = NewSlot();
    snprintf(q->id, sizeof(q->id), "%s", id);
    snprintf(q->slug, sizeof(q->slug), "%s", slug);
    snprintf(q->dir, sizeof(q->dir), "%s%s/", requestsDir, id);
    q->submitTime = clockTime;
    char err[512] = "";
    q->launched = Launch(id, slug, claudePrompt, model, q, err, sizeof(err));
    if (!q->launched) {
        snprintf(msg, msgSize, "launch failed: %s. Bundle kept: %s", err, q->dir);
        TraceLog(LOG_WARNING, "monolith: run it by hand: cd '%s' && claude \"%s\"", rootDir, claudePrompt);
        return false;
    }
    snprintf(msg, msgSize, "request sent: %s (Claude Code opens in a new terminal)", id);
    return true;
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

// #endregion
