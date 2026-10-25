// Export: one map as a self-contained folder that builds and runs on its own (docs/export.md,
// docs/architecture.md "Export, import, examples")
//
// --export <dir> [--map <path>] (no window) and the in-game key F9 (the open map, into
// export/<map>-<stamp>/) write:
//
//   <dir>/README.md            build steps, how to use the scene in another raylib + R3D
//                              program (bundled or own R3D), quality presets, licences
//   <dir>/build.sh             one-step build of the viewer: Linux, --win for MinGW-w64
//                              (= runtime/export/build.sh with the map's name filled in)
//   <dir>/viewer.c             a plain example program: window, fly camera, the map's
//                              viewpoints (= runtime/export/viewer.c with the map's name)
//   <dir>/monolith_scene.h/.c  the scene runtime without the editor parts (below)
//   <dir>/<name>/              the map package: <name>.mapb (the compiled map the program loads,
//                              docs/compiled-map.md), <name>.map (its source text, byte for byte;
//                              the program never reads it), MAP.md, ASSETS.md and every file
//                              the map references (shaders, textures, panoramas, models), plus
//                              provenance sidecars (<file>.asset.md), licence files in the same
//                              folders and the files a model file refers to (.gltf: buffers and
//                              images; .obj: .mtl and its texture maps)
//   <dir>/r3d/                 the patched R3D monolith builds with (submodules/r3d, or the
//                              R3D_DIR it was built with), with patches.md and its vendored
//                              raylib, assimp, glad, tinycthread and uthash: only what a build
//                              needs (R3D_KEEP / R3D_SKIP below: no .git*, build folders,
//                              examples, docs or test data)
//   <dir>/prebuilt/<linux|win>/lib/   libr3d.a, libraylib.a, libassimp.a, libzlibstatic.a as
//                              monolith's build.sh made them from that tree (AddPrebuilt: only
//                              current static Release builds; a target without them builds from source)
//   <dir>/cmake/mingw-w64-x86_64.cmake   the toolchain file build.sh --win uses
//   <dir>/LICENSES/            the licence texts of monolith (MIT), R3D, raylib, assimp and uthash
//
// The runtime is copied without its editor parts: every `#ifdef MONOLITH_EDITOR` block is
// dropped and every `#ifndef MONOLITH_EDITOR` block kept without its directives (StripEditor),
// so the exported files have no MONOLITH_EDITOR left in them; anything else fails the export.
// R3D and the runtime load nothing from disk at run time besides the map package (R3D's
// shaders and lookup tables are compiled into the library; raylib's default font too).
//
// Paths: the loader only accepts package-relative paths (no absolute paths, no ".."), so the
// map file is copied byte for byte and every path in it stays valid; the map file is renamed
// to <name>.map only so that the folder loads by its name. Files of the source package that
// nothing refers to are not copied; the export lists them.
//
// Nothing partial: every source is checked before anything is written (a referenced file
// that is missing fails the export with the list of missing files), then the folder is
// written as <dir>.partial, its map compiled there (MS_Compile), the compiled map loaded again
// (no GPU) and the folder renamed to <dir>. If writing, compiling or the check fails, the files
// written into <dir>.partial are removed again. A compiled map exports from its source text,
// which must be next to it and unchanged since it was compiled.
//
// Sources: the repo (PROJECT_PATH: runtime/, cmake/; R3D_SOURCE_PATH, the R3D tree build.sh
// built against). Without the repo (a copied build folder) the export fails with a message.

#include "export.h"
#include "dirscan.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#   include <direct.h>
#   define RemoveDir _rmdir
#else
#   include <unistd.h>
#   define RemoveDir rmdir
#endif

#ifndef PROJECT_PATH
#   define PROJECT_PATH ""
#endif
#ifndef R3D_SOURCE_PATH
#   define R3D_SOURCE_PATH ""
#endif

// ======================================================================================
// #region Plan: every file of the export, before anything is written
// ======================================================================================

typedef struct {
    char *from;             // source file (NULL: generated, see data)
    char *to;               // path inside the export folder, '/'-separated
    char *pkgRel;           // path inside the map package, NULL for the other files
    char *data;             // generated content (from == NULL)
    size_t size;
    bool exec;              // build.sh
} PlanItem;

typedef struct {
    PlanItem *items;
    int count, cap;
    int errors;
} Plan;

static void FreePlan(Plan *P)
{
    for (int i = 0; i < P->count; i++) {
        free(P->items[i].from);
        free(P->items[i].to);
        free(P->items[i].pkgRel);
        free(P->items[i].data);
    }
    free(P->items);
    memset(P, 0, sizeof(*P));
}

// malloc'd printf (no length limit); NULL when out of memory
static char *Fmt(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (!s) return NULL;
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

static PlanItem *FindTo(const Plan *P, const char *to)
{
    for (int i = 0; i < P->count; i++) if (strcmp(P->items[i].to, to) == 0) return &P->items[i];
    return NULL;
}

// Adds a file to copy (once per destination). pkgRel: its path inside the map package, or NULL.
static void AddCopy(Plan *P, const char *from, const char *to, const char *pkgRel)
{
    if (FindTo(P, to)) return;
    char *f = MS_StrDup(from), *t = MS_StrDup(to), *r = pkgRel ? MS_StrDup(pkgRel) : NULL;
    PlanItem *it = f && t && (r || !pkgRel) ? MS_PUSH(P->items, P->count, P->cap) : NULL;
    if (!it) { free(f); free(t); free(r); fprintf(stderr, "monolith: export: out of memory\n"); P->errors++; return; }
    it->from = f;
    it->to = t;
    it->pkgRel = r;
}

// Adds a generated file (takes ownership of data)
static void AddData(Plan *P, const char *to, char *data, bool exec)
{
    char *t = data ? MS_StrDup(to) : NULL;
    PlanItem *it = t ? MS_PUSH(P->items, P->count, P->cap) : NULL;
    if (!it) { free(t); free(data); fprintf(stderr, "monolith: export: out of memory (%s)\n", to); P->errors++; return; }
    it->to = t;
    it->data = data;
    it->size = strlen(data);
    it->exec = exec;
}

// A file of the map package: <root><rel> -> <name>/<dstRel>
static void AddPkg(Plan *P, const char *root, const char *name, const char *rel, const char *dstRel)
{
    if (!dstRel) dstRel = rel;
    char *from = Fmt("%s%s", root, rel), *to = Fmt("%s/%s", name, dstRel);
    if (from && to) AddCopy(P, from, to, dstRel);
    else { fprintf(stderr, "monolith: export: out of memory\n"); P->errors++; }
    free(from);
    free(to);
}

// A file written into the export by other means than the plan (the compiled map): listed so that
// RemoveWritten removes it again
static void AddWritten(Plan *P, const char *to)
{
    char *t = MS_StrDup(to);
    PlanItem *it = t ? MS_PUSH(P->items, P->count, P->cap) : NULL;
    if (!it) { free(t); fprintf(stderr, "monolith: export: out of memory\n"); P->errors++; return; }
    it->to = t;
}

static bool IsPkgCopied(const Plan *P, const char *rel)
{
    for (int i = 0; i < P->count; i++) if (P->items[i].pkgRel && strcmp(P->items[i].pkgRel, rel) == 0) return true;
    return false;
}

// Files of the source package that the export didn't copy (DirScan callback)
typedef struct {
    const Plan *P;
    const char *mapPath;
    int skipped;
} UnusedScan;

static void UnusedFile(const char *path, const char *rel, void *user)
{
    UnusedScan *u = user;
    if (strcmp(path, u->mapPath) == 0 || IsPkgCopied(u->P, rel)) return;
    printf("monolith: export: not referenced, not copied: %s\n", path);
    u->skipped++;
}

// A path written inside a package file (a .gltf uri, an .mtl map): stays inside the package?
static bool SafeRelative(const char *p)
{
    if (!p[0] || p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':')) return false;
    for (const char *c = p; *c; ) {
        const char *e = c;
        while (*e && *e != '/' && *e != '\\') e++;
        if (e - c == 2 && c[0] == '.' && c[1] == '.') return false;
        c = *e ? e + 1 : e;
    }
    return true;
}

// "dir/of/model/" + "name" -> package-relative path (models may live in subfolders)
static void JoinRel(const char *modelRel, const char *ref, char *out, int outSize)
{
    const char *slash = strrchr(modelRel, '/');
    int dirLen = slash ? (int)(slash - modelRel + 1) : 0;
    snprintf(out, outSize, "%.*s%s", dirLen, modelRel, ref);
}

// Files a model file refers to by itself: .gltf "uri"s (buffers, images; not data: URIs),
// .obj mtllib -> .mtl -> map_* textures. .glb / .fbx with embedded data need nothing.
static void AddModelParts(Plan *P, const char *root, const char *name, const char *rel)
{
    const char *ext = GetFileExtension(rel);
    if (!ext) return;
    char full[MS_PATH_MAX * 2];
    snprintf(full, sizeof(full), "%s%s", root, rel);
    char *text = FileExists(full) ? LoadFileText(full) : NULL;
    if (!text) return;      // (a missing model file is reported with the other missing files)
    char ref[MS_PATH_MAX], joined[MS_PATH_MAX * 2];
    if (TextIsEqual(ext, ".gltf")) {
        for (char *p = strstr(text, "\"uri\""); p; p = strstr(p + 5, "\"uri\"")) {
            char *q = strchr(p + 5, '"');
            if (!q) break;
            char *e = strchr(q + 1, '"');
            if (!e) break;
            int len = (int)(e - q - 1);
            if (len <= 0 || len >= (int)sizeof(ref)) continue;
            memcpy(ref, q + 1, len);
            ref[len] = 0;
            if (strncmp(ref, "data:", 5) == 0) continue;
            // %20 is the only escape seen in practice; anything else is copied as written
            for (char *s = strstr(ref, "%20"); s; s = strstr(s, "%20")) { *s = ' '; memmove(s + 1, s + 3, strlen(s + 3) + 1); }
            if (!SafeRelative(ref)) { fprintf(stderr, "monolith: export: %s refers to %s outside its folder\n", rel, ref); P->errors++; continue; }
            JoinRel(rel, ref, joined, sizeof(joined));
            AddPkg(P, root, name, joined, NULL);
        }
    } else if (TextIsEqual(ext, ".obj") || TextIsEqual(ext, ".mtl")) {
        bool obj = TextIsEqual(ext, ".obj");
        for (char *line = text; line && *line; ) {
            char *next = strchr(line, '\n');
            if (next) *next++ = 0;
            while (*line == ' ' || *line == '\t') line++;
            bool isRef = obj ? strncmp(line, "mtllib ", 7) == 0
                             : (strncmp(line, "map_", 4) == 0 || strncmp(line, "bump ", 5) == 0 || strncmp(line, "norm ", 5) == 0);
            if (isRef) {
                // the file name is the last word (options like -bm 1.0 come first)
                char *end = line + strlen(line);
                while (end > line && isspace((unsigned char)end[-1])) *--end = 0;
                char *w = end;
                while (w > line && !isspace((unsigned char)w[-1])) w--;
                if (*w && SafeRelative(w)) {
                    JoinRel(rel, w, joined, sizeof(joined));
                    AddPkg(P, root, name, joined, NULL);
                    if (obj) AddModelParts(P, root, name, joined);
                } else if (*w) { fprintf(stderr, "monolith: export: %s refers to %s outside its folder\n", rel, w); P->errors++; }
            }
            line = next;
        }
    }
    UnloadFileText(text);
}

static bool IsModelFile(const char *rel)
{
    static const char *const EXT[] = { ".glb", ".gltf", ".obj", ".fbx", ".dae", ".ply", ".stl", ".3ds", ".blend", ".x" };
    const char *ext = GetFileExtension(rel);
    for (int i = 0; ext && i < (int)(sizeof(EXT) / sizeof(EXT[0])); i++) if (TextIsEqual(ext, EXT[i])) return true;
    return false;
}

// #endregion

// ======================================================================================
// #region R3D tree: only what a build needs
// ======================================================================================

// Top-level entries of the R3D tree that a build uses (CMakeLists.txt and what it includes,
// the embedded shaders and lookup tables with their scripts, the vendored dependencies)
static const char *const R3D_KEEP[] = {
    "CMakeLists.txt", "LICENSE", "README.md", "patches.md",
    "cmake", "include", "src", "shaders", "assets", "scripts", "external",
};
// Below those: what the configured build never reads (tests, samples, tools, docs; draco
// only with ASSIMP_BUILD_DRACO, googletest only with ASSIMP_BUILD_TESTS; raylib's examples
// only with BUILD_EXAMPLES). Names starting with '.' (.git, .github, ...) are skipped anywhere.
static const char *const R3D_SKIP[] = {
    "external/assimp/test", "external/assimp/samples", "external/assimp/doc", "external/assimp/port",
    "external/assimp/fuzz", "external/assimp/tools", "external/assimp/scripts",
    "external/assimp/contrib/googletest", "external/assimp/contrib/draco",
    "external/raylib/examples", "external/raylib/projects", "external/raylib/parser", "external/raylib/logo",
};

typedef struct {
    Plan *P;
    int files;
    long newest[3];         // newest source file per prebuilt library group (LIB_R3D, LIB_RAYLIB, LIB_ASSIMP)
} R3DScan;

enum { LIB_R3D, LIB_RAYLIB, LIB_ASSIMP };

static bool R3DWanted(const char *rel)
{
    bool top = false;
    for (int i = 0; i < (int)(sizeof(R3D_KEEP) / sizeof(R3D_KEEP[0])) && !top; i++) {
        size_t n = strlen(R3D_KEEP[i]);
        top = strncmp(rel, R3D_KEEP[i], n) == 0 && (rel[n] == 0 || rel[n] == '/');
    }
    if (!top) return false;
    for (int i = 0; i < (int)(sizeof(R3D_SKIP) / sizeof(R3D_SKIP[0])); i++) {
        size_t n = strlen(R3D_SKIP[i]);
        if (strncmp(rel, R3D_SKIP[i], n) == 0 && rel[n] == '/') return false;
    }
    for (const char *c = rel; *c; ) {               // no component starting with '.'
        if (*c == '.') return false;
        const char *slash = strchr(c, '/');
        if (!slash) break;
        c = slash + 1;
    }
    return true;
}

static void R3DFile(const char *path, const char *rel, void *user)
{
    R3DScan *r = user;
    if (!R3DWanted(rel)) return;
    char *to = Fmt("r3d/%s", rel);
    if (to) AddCopy(r->P, path, to, NULL);
    else r->P->errors++;
    free(to);
    r->files++;
    // Sources of the prebuilt libraries (docs and licences don't count)
    if (strcmp(rel, "README.md") == 0 || strcmp(rel, "patches.md") == 0 || strcmp(rel, "LICENSE") == 0) return;
    int group = strncmp(rel, "external/raylib/", 16) == 0 ? LIB_RAYLIB : strncmp(rel, "external/assimp/", 16) == 0 ? LIB_ASSIMP : LIB_R3D;
    long m = GetFileModTime(path);
    if (m > r->newest[group]) r->newest[group] = m;
}

// The static libraries monolith's build.sh made from this R3D tree (<r3d>/build-linux/lib,
// <r3d>/build-mingw/lib), as prebuilt/<target>/lib/ of the export, so the package's build.sh
// can skip the R3D build. A target is left out (with a message) when its libraries are
// missing or not a static Release build, or when the last build of that folder (build.sh
// appends cmake --build's output to monolith-build.log) failed or ran before the newest
// source file changed; the package then builds that target from source. (Not the libraries'
// own times: a change that recompiles nothing, e.g. an umbrella header, leaves them as they are.)
static const char *const PREBUILT_LIBS[] = { "libr3d.a", "libraylib.a", "libassimp.a", "libzlibstatic.a" };

static bool AddPrebuilt(Plan *P, const char *r3d, const R3DScan *rs, const char *buildDir, const char *target, char *why, int whySize)
{
    const char *again = strcmp(target, "win") == 0 ? "./build.sh --win" : "./build.sh";
    char *args = LoadFileText(TextFormat("%s%s/monolith-configure.args", r3d, buildDir));
    bool release = args && strstr(args, "-DCMAKE_BUILD_TYPE=Release\n") && strstr(args, "-DBUILD_SHARED_LIBS=OFF");
    if (args) UnloadFileText(args);
    if (!release) {
        snprintf(why, whySize, "%s%s is not monolith's static Release build (run %s)", r3d, buildDir, again);
        return false;
    }
    for (int i = 0; i < 4; i++) {
        const char *lib = TextFormat("%s%s/lib/%s", r3d, buildDir, PREBUILT_LIBS[i]);
        if (!FileExists(lib)) { snprintf(why, whySize, "%s is missing (run %s)", lib, again); return false; }
    }
    long newest = rs->newest[LIB_R3D];
    if (rs->newest[LIB_RAYLIB] > newest) newest = rs->newest[LIB_RAYLIB];
    if (rs->newest[LIB_ASSIMP] > newest) newest = rs->newest[LIB_ASSIMP];
    // The last build succeeded when the log's last non-empty line is cmake's "Built target ..."
    char *log = MS_StrDup(TextFormat("%s%s/monolith-build.log", r3d, buildDir));
    char *text = log && FileExists(log) ? LoadFileText(log) : NULL;
    bool built = false;
    if (text) {
        size_t n = strlen(text);
        while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r')) text[--n] = 0;
        const char *line = strrchr(text, '\n');
        built = strstr(line ? line + 1 : text, "Built target") != NULL;
        UnloadFileText(text);
    }
    bool fresh = built && GetFileModTime(log) >= newest;
    if (!fresh) snprintf(why, whySize, "the last build in %s%s %s (run %s)", r3d, buildDir,
                         built ? "ran before its newest source file changed" : "did not finish", again);
    free(log);
    if (!fresh) return false;
    for (int i = 0; i < 4; i++) {
        char *from = Fmt("%s%s/lib/%s", r3d, buildDir, PREBUILT_LIBS[i]), *to = Fmt("prebuilt/%s/lib/%s", target, PREBUILT_LIBS[i]);
        if (from && to) AddCopy(P, from, to, NULL);
        else P->errors++;
        free(from);
        free(to);
    }
    return true;
}

// #endregion

// ======================================================================================
// #region Runtime without the editor parts
// ======================================================================================

// The directive of a preprocessor line ("ifdef", "endif", ...) and its argument, or 0
static int Directive(const char *line, const char *end, const char **word, const char **arg)
{
    const char *c = line;
    while (c < end && (*c == ' ' || *c == '\t')) c++;
    if (c >= end || *c != '#') return 0;
    c++;
    while (c < end && (*c == ' ' || *c == '\t')) c++;
    *word = c;
    while (c < end && isalpha((unsigned char)*c)) c++;
    int n = (int)(c - *word);
    while (c < end && (*c == ' ' || *c == '\t')) c++;
    *arg = c;
    return n;
}

static bool IsEditorArg(const char *arg, const char *end)
{
    size_t n = strlen("MONOLITH_EDITOR");
    if ((size_t)(end - arg) < n || strncmp(arg, "MONOLITH_EDITOR", n) != 0) return false;
    const char *c = arg + n;
    while (c < end && (*c == ' ' || *c == '\t')) c++;
    return c == end || (c + 1 < end && c[0] == '/' && (c[1] == '/' || c[1] == '*'));
}

static bool IsBlank(const char *line, const char *end)
{
    for (const char *c = line; c < end; c++) if (!isspace((unsigned char)*c)) return false;
    return true;
}

// `text` without its MONOLITH_EDITOR blocks: `#ifdef MONOLITH_EDITOR` blocks dropped,
// `#ifndef MONOLITH_EDITOR` blocks kept, either one's `#else` part the other way round, the
// directives themselves removed; a blank line left doubled by a dropped block goes too.
// Nested #if blocks inside are followed; an editor block inside an editor block, or any
// other mention of MONOLITH_EDITOR left over, is an error (malloc'd result, NULL + err).
static char *StripEditor(const char *text, const char *what, char *err, int errSize)
{
    size_t len = strlen(text);
    char *out = malloc(len + 1);
    if (!out) { snprintf(err, errSize, "out of memory"); return NULL; }
    size_t o = 0;
    int inBlock = 0;        // 0 = outside, else the #if depth inside the editor block (1 = its own level)
    bool keep = true;       // inside a block: are its lines kept?
    bool dropped = false;   // a block was just dropped (for the blank-line rule)
    int lineNo = 0;
    for (const char *line = text; *line; ) {
        const char *end = strchr(line, '\n');
        const char *next = end ? end + 1 : line + strlen(line);
        if (!end) end = next;
        lineNo++;
        const char *word, *arg;
        int wl = Directive(line, end, &word, &arg);
        bool isIf = wl >= 2 && strncmp(word, "if", 2) == 0;
        bool emit = true;
        bool editorIf = (wl == 5 && strncmp(word, "ifdef", 5) == 0 && IsEditorArg(arg, end)) ||
                        (wl == 6 && strncmp(word, "ifndef", 6) == 0 && IsEditorArg(arg, end));
        if (editorIf) {
            if (inBlock) { snprintf(err, errSize, "%s:%d: MONOLITH_EDITOR block inside another one", what, lineNo); free(out); return NULL; }
            inBlock = 1;
            keep = wl == 6;         // #ifndef: kept
            emit = false;
        } else if (inBlock && isIf) {
            inBlock++;
        } else if (inBlock == 1 && wl == 4 && strncmp(word, "else", 4) == 0) {
            keep = !keep;
            emit = false;
        } else if (inBlock == 1 && wl == 4 && strncmp(word, "elif", 4) == 0) {
            snprintf(err, errSize, "%s:%d: #elif in a MONOLITH_EDITOR block", what, lineNo);
            free(out);
            return NULL;
        } else if (inBlock && wl == 5 && strncmp(word, "endif", 5) == 0) {
            if (--inBlock == 0) { emit = false; dropped = !keep || dropped; keep = true; }
        }
        if (inBlock && !keep) { emit = false; dropped = true; }
        if (emit) {
            // A dropped block between two blank lines leaves one of them
            bool blank = IsBlank(line, end);
            bool prevBlank = o == 0 || (o >= 2 && out[o - 1] == '\n' && out[o - 2] == '\n') || (o == 1 && out[0] == '\n');
            if (!(dropped && blank && prevBlank)) {
                memcpy(out + o, line, next - line);
                o += next - line;
            }
            dropped = false;
        }
        line = next;
    }
    out[o] = 0;
    if (inBlock) { snprintf(err, errSize, "%s: unterminated MONOLITH_EDITOR block", what); free(out); return NULL; }
    if (strstr(out, "MONOLITH_EDITOR")) {
        snprintf(err, errSize, "%s: MONOLITH_EDITOR is still mentioned after removing its blocks", what);
        free(out);
        return NULL;
    }
    return out;
}

// A text file with every "@MAP@" replaced by the map's name (malloc'd)
static char *LoadTemplate(const char *path, const char *name)
{
    char *t = FileExists(path) ? LoadFileText(path) : NULL;
    if (!t) return NULL;
    size_t nameLen = strlen(name), count = 0;
    for (const char *p = strstr(t, "@MAP@"); p; p = strstr(p + 5, "@MAP@")) count++;
    char *out = malloc(strlen(t) + count * nameLen + 1);
    if (out) {
        char *o = out;
        for (const char *p = t; *p; ) {
            if (strncmp(p, "@MAP@", 5) == 0) { memcpy(o, name, nameLen); o += nameLen; p += 5; }
            else *o++ = *p++;
        }
        *o = 0;
    }
    UnloadFileText(t);
    return out;
}

// #endregion

// ======================================================================================
// #region README.md
// ======================================================================================

// The table of ASSETS.md (its lines starting with '|'), if it has any data rows
static void WriteAssets(FILE *f, const char *assetsPath)
{
    char *text = FileExists(assetsPath) ? LoadFileText(assetsPath) : NULL;
    int rows = 0;
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : p + strlen(p))
        if (*p == '|') rows++;
    // header + separator only: no assets
    for (char *line = rows > 2 ? text : NULL; line && *line; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (line[0] == '|') fprintf(f, "%s\n", line);
        line = next;
    }
    if (text) UnloadFileText(text);
    if (rows <= 2) fprintf(f, "No external assets: everything in this package was made for it.\n");
    fprintf(f, "\n");
}

// README.md as a malloc'd string (NULL on failure)
static char *MakeReadme(const MS_Scene *s, const char *name, const char *assetsPath, int pkgFiles, const bool prebuilt[2])
{
    char *buf = NULL;
    size_t size = 0;
    FILE *f = tmpfile();                // (open_memstream is not on Windows)
    if (!f) return NULL;
    char when[32] = "";
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm);

    fprintf(f, "# %s: a monolith map for raylib + R3D\n\n", name);
    fprintf(f, "Exported from the monolith map designer on %s: map format %d, %d boxes, %d models, %d materials, "
               "%d lights besides the sun, %d viewpoints, %d package files. Everything needed to build and run it is in "
               "this folder.\n\n",
            when, s->formatVersion, s->boxCount, s->modelCount, s->materialCount, s->lightCount, s->viewpointCount, pkgFiles);
    fprintf(f,
        "| Path | What |\n|---|---|\n"
        "| `%s/` | The map package: `%s.mapb` (the compiled map: what the program loads), `%s.map` (its source "
        "text: geometry, materials, lights, sky, fog, post-processing; see \"The compiled map\"), "
        "`MAP.md` (its design brief), `ASSETS.md` (sources and licences of external files) and every file the map uses. "
        "All paths inside are relative to this folder, so it can live anywhere. |\n"
        "| `monolith_scene.h`, `monolith_scene.c` | The scene runtime: loads and draws the package, and answers "
        "collision and ray queries against it. C11, depends only on raylib (raymath, rlgl) and R3D. monolith draws "
        "its maps with the same code. |\n"
        "| `viewer.c` | A plain example program: opens a window, loads the package, flies the camera and switches "
        "between the map's viewpoints. |\n"
        "| `build.sh` | Builds the viewer with the bundled R3D in one step (Linux; `--win` for Windows). |\n"
        "| `r3d/` | The R3D renderer monolith uses, with its local patches (`r3d/patches.md`) and the raylib and "
        "assimp it is built with (`r3d/external/`). Only what a build needs. |\n"
        "| `prebuilt/` | R3D, raylib and assimp as static libraries, built by monolith from `r3d/` "
        "(`prebuilt/<linux|win>/lib/`; the targets are listed under \"Build and run\"). |\n"
        "| `cmake/` | The MinGW-w64 toolchain file for `./build.sh --win`. |\n"
        "| `LICENSES/` | Licence texts of monolith, R3D, raylib, assimp and uthash. |\n\n",
        name, name, name);

    fprintf(f,
        "## Build and run\n\n"
        "```sh\n"
        "./build.sh                # Linux x86_64   -> build/linux/viewer\n"
        "./build.sh --win          # Windows x86_64 -> build/win/viewer.exe (MinGW-w64 cross-compiler)\n"
        "build/linux/viewer        # opens %s (or: build/linux/viewer path/to/another/package)\n"
        "```\n\n"
        "%s"
        "The map package is copied next to the program, so `build/linux/` (or `build/win/`) can be moved as it is. "
        "The build defines `MS_PATCHED_R3D` because the R3D in `r3d/` is monolith's patched copy.\n\n"
        "Needs a C compiler (and the C++ runtime assimp uses) and on Linux the OpenGL / X11 libraries. Building R3D "
        "from source also needs cmake 3.22 or newer, python3 (R3D turns its shaders into C headers while building), "
        "a C++ compiler, and on Linux the X11 / OpenGL development headers raylib uses (Debian / Ubuntu: `libx11-dev "
        "libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev`; Fedora: `libX11-devel libXrandr-devel "
        "libXinerama-devel libXcursor-devel libXi-devel mesa-libGL-devel`; Arch: `libx11 libxrandr libxinerama "
        "libxcursor libxi mesa`). `--win` needs the `x86_64-w64-mingw32-gcc` / `-g++` cross-compilers (Debian / Ubuntu: "
        "`mingw-w64`; Arch: `mingw-w64-gcc`).\n\n"
        "In the viewer: hold the right mouse button to look around, `W` `A` `S` `D` to move, `E` / `Q` up and down, "
        "`SHIFT` faster, `LEFT` / `RIGHT` for the map's viewpoints (their names are shown at the top).\n\n", name,
        prebuilt[0] || prebuilt[1]
            ? TextFormat("Prebuilt R3D, raylib and assimp libraries are included for %s (`prebuilt/`, built by monolith "
                         "from `r3d/`), so `./build.sh` only compiles the viewer and the scene runtime and links them: "
                         "seconds. If linking against them fails (a compiler or C library older than the one they were "
                         "built with), `build.sh` says so and builds R3D, raylib and assimp from `r3d/` instead (a few "
                         "minutes, once); `./build.sh --from-source` always does. The libraries have R3D's GPU pass "
                         "profiler compiled in (monolith uses it for benchmarks): it stays off unless a program calls "
                         "`R3D_SetProfilerEnabled`, and never changes the image.\n\n",
                         prebuilt[0] && prebuilt[1] ? "Linux x86_64 and Windows x86_64" : prebuilt[0] ? "Linux x86_64" : "Windows x86_64")
            : "The first build compiles R3D, raylib and assimp from `r3d/` as static libraries (a few minutes), later "
              "builds only what changed.\n\n");

    fprintf(f,
        "## The compiled map\n\n"
        "`%s/%s.mapb` is `%s.map` compiled by monolith: the same scene, already parsed, stored as fixed-size binary "
        "records (map format %d). The scene runtime reads only the compiled file (it has no text parser) and checks "
        "it completely before using it; `%s.map` is here as the source and is never read by the program. To change "
        "the map, edit `%s.map` (in monolith, or by hand: the format is described in monolith's `docs/map-format.md`) "
        "and compile it again with monolith, from this folder:\n\n"
        "```sh\n"
        "monolith --map %s --compile %s/%s.mapb\n"
        "```\n\n"
        "A compiled map from an older or newer monolith is refused with a message to compile it again.\n\n",
        name, name, name, s->formatVersion, name, name, name, name, name);

    fprintf(f,
        "## Use the map in your own program\n\n"
        "Add `monolith_scene.c` to your sources, this folder to the include path and ship the `%s/` folder with the "
        "program (`MS_Load` on the folder opens its `.mapb`; the `.map` can stay behind). The scene only renders and "
        "answers queries: the window, the main loop, the camera, the player and the physics stay yours.\n\n"
        "```c\n"
        "#include \"monolith_scene.h\"\n\n"
        "char err[512];\n"
        "InitWindow(1280, 720, \"game\");\n"
        "// The sun's shadow map size is an R3D init hint: read the package once without the GPU first\n"
        "MS_Scene *peek = MS_Load(\"%s\", &(MS_LoadOptions){ .noGpu = true }, err, sizeof(err));\n"
        "if (peek) { MS_SetInitHints(peek); MS_Unload(peek); }\n"
        "R3D_Init(GetScreenWidth(), GetScreenHeight());\n"
        "R3D_SetAntiAliasingMode(R3D_ANTI_ALIASING_MODE_FXAA);   // what monolith uses\n"
        "MS_InitRenderer();          // GL state the scene needs (depth clamp), see \"Sun shadow\" below\n\n"
        "MS_Scene *scene = MS_Load(\"%s\", NULL, err, sizeof(err));\n"
        "if (!scene) { puts(err); return 1; }\n"
        "MS_ApplyEnvironment(scene);         // sky, ambient light, fog, SSAO/SSIL, bloom, tonemap ...\n\n"
        "while (!WindowShouldClose()) {\n"
        "    // ... your input, physics and camera ...\n"
        "    BeginDrawing();\n"
        "        MS_Draw(scene, camera, (RenderTexture){ 0 });   // one whole R3D frame\n"
        "    EndDrawing();\n"
        "}\n"
        "MS_Unload(scene);\n"
        "R3D_Close();\n"
        "CloseWindow();\n"
        "```\n\n"
        "**Inside your own R3D frame** (your objects and lights together with the scene):\n\n"
        "```c\n"
        "R3D_Begin(camera);\n"
        "    MS_PushLights(scene, camera, (float)GetScreenWidth() / GetScreenHeight());   // sun + the map's lights\n"
        "    MS_DrawGeometry(scene);\n"
        "    R3D_PushLight(myLamp);                      // your own lights and draws as before\n"
        "    R3D_DrawMesh(playerMesh, playerMaterial, playerPos, 1.0f);\n"
        "R3D_End();\n"
        "```\n\n"
        "The scene sets R3D's environment (`MS_ApplyEnvironment`); change fields of `R3D_GetEnvironment()` after it "
        "if your game needs other settings. **Map data for your game**: `scene->spawnFeet` / `spawnYawDeg` (the "
        "player start), `scene->viewpoints` (named cameras), `scene->colliders` (world AABBs of the boxes and the "
        "models' colliders, for your physics), `scene->bounds`. **Queries**: `MS_Collides(scene, box)` (strict AABB "
        "overlap with any collider), `MS_Raycast(scene, origin, dir, maxDist)` (boxes and model triangles), "
        "`MS_ObjectRef` / `MS_ObjectBounds` for what a ray hit. `MS_InitRenderer` calls `glEnable` from libGL / "
        "opengl32, so link against it (as every raylib program does).\n\n",
        name, name, name);

    fprintf(f,
        "### Building it into your project\n\n"
        "With this folder's R3D (recommended: the full look, see \"Patched vs. upstream R3D\"), e.g. in CMake with "
        "this folder copied to `external/monolith/` of your project:\n\n"
        "```cmake\n"
        "set(R3D_RAYLIB_VENDORED ON CACHE BOOL \"\")             # raylib and assimp from r3d/external\n"
        "set(R3D_ASSIMP_VENDORED ON CACHE BOOL \"\")\n"
        "set(ASSIMP_BUILD_ZLIB ON CACHE BOOL \"\")\n"
        "set(R3D_BUILD_EXAMPLES OFF CACHE BOOL \"\")\n"
        "add_subdirectory(external/monolith/r3d)\n"
        "add_executable(game main.c external/monolith/monolith_scene.c)\n"
        "target_include_directories(game PRIVATE external/monolith)\n"
        "target_compile_definitions(game PRIVATE MS_PATCHED_R3D)  # this R3D is the patched one\n"
        "target_link_libraries(game PRIVATE r3d raylib)\n"
        "```\n\n"
        "or, from this folder, against the prebuilt static libraries (`prebuilt/linux/lib`; after a build from "
        "source: `build/r3d-linux/lib`):\n\n"
        "```sh\n"
        "cc -std=c11 -O2 -DMS_PATCHED_R3D -I. -Ir3d/include -Ir3d/external/raylib/src main.c monolith_scene.c -o game \\\n"
        "   -Lprebuilt/linux/lib -lr3d -lraylib -lassimp -lzlibstatic -lstdc++ -lGL -lm -lpthread -ldl\n"
        "# Windows (MinGW-w64, prebuilt/win/lib): ... -lr3d -lraylib -lassimp -lzlibstatic -lstdc++ "
        "-lopengl32 -lgdi32 -lwinmm -lshell32 -static\n"
        "```\n\n"
        "**With your own R3D** (https://github.com/Bigfoot71/r3d) instead: the same lines against your R3D tree and its "
        "build folder, **without** `MS_PATCHED_R3D` (the runtime then uses upstream R3D's APIs only; the image "
        "differences are listed below). Define `MS_PATCHED_R3D` only when building against the R3D in `r3d/`. "
        "(`-lzlibstatic` only if your R3D bundles zlib, `ASSIMP_BUILD_ZLIB=ON`; a shared R3D build needs just "
        "`-lr3d -lraylib`.)\n\n");

    fprintf(f,
        "## Patched vs. upstream R3D\n\n"
        "The R3D in `r3d/` carries monolith's local patches (`r3d/patches.md`: symptom, cause, diff and verification of "
        "each). The runtime compiles against both:\n\n"
        "| | `-DMS_PATCHED_R3D` (`r3d/`) | upstream R3D |\n|---|---|---|\n"
        "| Sun shadow | static world-fixed shadow tiles, drawn once per map load (`R3D_PushLightFixedShadow`, patch 7) | "
        "R3D's camera-fitted map, re-rendered when it no longer covers the view (`R3D_PushLightEx`) |\n"
        "| Depth pre-pass | on (`MS_RenderSettings.depthPrepass`, patch 6): expensive surface shaders run once per pixel | "
        "not available: same image, more GPU time with overdraw |\n"
        "| Fog cache (`fogCacheVoxel`) | optional baked fog sun visibility (patch 8) | not available (off) |\n"
        "| Occlusion volume (`aoVolumeVoxel`) | optional baked world-space AO for the ambient light (patch 11) | not available (off) |\n"
        "| Post passes | bloom composite folded into tonemapping, FXAA writes the final target, depth pyramid only as deep as used (patch 10) | same image, ~0.25 ms more GPU |\n\n"
        "What that means for the image, honestly:\n\n"
        "- **Same image:** patches 2, 3, 6 and 10 skip work whose result is discarded (0 pixels differ).\n"
        "- **Slightly different:** patch 7 vs. the camera-fitted map: a few hundred pixels per 26 test shots differ "
        "by more than 2/255 (16-bit depth rounding). Patch 9 (fog march): rounding level, a handful of pixels > 2/255.\n"
        "- **Visibly different when the camera turns:** patch 4. Upstream fades the volumetric-fog shadow, the fog's "
        "transmittance, the distance fog and the sun-shadow fade by *view depth*, so the same spot looks different at "
        "the centre and at the edge of the screen (fog thinner toward the edges, a sunlit opening that glows in the "
        "centre and dims at the edge). With upstream R3D expect ~0.3%% of pixels to differ by more than 8/255 from "
        "monolith, up to ~70/255 in fog-heavy views at the screen edge.\n"
        "- **Broken on AMD / Intel GPUs:** patch 1. Upstream's volumetric fog shaders fail to compile on strict GLSL "
        "drivers (a uniform member named `length`); NVIDIA accepts them. Upstream builds lose volumetric fog there "
        "unless that bug is fixed upstream.\n"
        "- Later patches (light clip boxes, the volumetric fog distance fade and fade-in) are ignored by upstream R3D: "
        "maps that use them look different there.\n\n");

    fprintf(f,
        "## Graphics quality presets\n\n"
        "`MS_QualityPreset(MS_QUALITY_LOW / MEDIUM / HIGH / ULTRA)` gives an `MS_Quality` (render scale and upscale "
        "filter, anti-aliasing, sun shadow size / range / softness / mode, AO mode and the baked occlusion volume, "
        "SSAO/SSIL samples and radius, volumetric fog step / length / cache, distance fog, bloom, SSR/SSGI, exposure "
        "and colour trims, depth pre-pass). Every value is relative to the map, so ultra renders exactly what the map "
        "describes and an effect the map turns off stays off. Apply it with `MS_ApplyQuality(scene, &q)` right after "
        "every `MS_ApplyEnvironment(scene)`, then `MS_ApplyRenderResolution(scene, w, h)`; `q.shadowSize` is an R3D "
        "init hint, so pass it as `MS_LoadOptions.shadowSizeOverride` to the peek load before `R3D_Init` and to the "
        "real load. Lower presets need the patched R3D for the baked parts (fog cache, occlusion volume); with "
        "upstream R3D they fall back to the plain passes.\n\n");

    fprintf(f,
        "## Sun shadow and depth clamp\n\n"
        "R3D's sun shadow map has a limited depth range toward the sun; tall casters far away get clipped and their "
        "shadows pop as the camera turns. `MS_InitRenderer()` (call it once after `R3D_Init`) enables `GL_DEPTH_CLAMP` (R3D never "
        "changes that state), which flattens such casters onto the near plane instead, as monolith does. Without it "
        "such shadows can pop; the side effect is that geometry closer than the 1 cm near plane is clamped rather than cut away.\n\n");

    fprintf(f,
        "## Licences and attribution\n\n"
        "- R3D: zlib licence, `LICENSES/r3d.txt` (`r3d/LICENSE`)\n"
        "- raylib: zlib licence, `LICENSES/raylib.txt` (`r3d/external/raylib/LICENSE`); its own bundled libraries "
        "(GLFW, stb, miniaudio, ...) carry their licences in their sources under `r3d/external/raylib/src/external/`\n"
        "- assimp: BSD 3-clause licence, `LICENSES/assimp.txt` (`r3d/external/assimp/LICENSE`); its bundled libraries "
        "(zlib, pugixml, rapidjson, ...) carry theirs under `r3d/external/assimp/contrib/`\n"
        "- uthash: BSD licence, `LICENSES/uthash.txt`; tinycthread (zlib licence) and the glad loader carry theirs in "
        "their sources under `r3d/external/`\n"
        "- The scene runtime (`monolith_scene.h/.c`), `viewer.c` and `build.sh` are part of monolith: MIT licence, `LICENSES/monolith.txt`\n\n"
        "The map's own files, from `%s/ASSETS.md` (the package's record of every external file; keep it with the "
        "package and credit CC-BY assets as listed):\n\n", name);
    WriteAssets(f, assetsPath);

    bool ok = !ferror(f) && fseek(f, 0, SEEK_END) == 0;
    long n = ok ? ftell(f) : -1;
    if (n >= 0 && fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)n + 1))) {
        size = fread(buf, 1, (size_t)n, f);
        buf[size] = 0;
        if (size != (size_t)n) { free(buf); buf = NULL; }
    }
    fclose(f);
    return buf;
}

// #endregion

// ======================================================================================
// #region Writing, cleanup
// ======================================================================================

static bool WriteFileBytes(const char *path, const void *data, size_t size, bool exec)
{
    char *dir = MS_StrDup(GetDirectoryPath(path));
    if (dir && dir[0] && !DirectoryExists(dir)) MakeDirectory(dir);
    free(dir);
    FILE *out = fopen(path, "wb");
    if (!out) { fprintf(stderr, "monolith: export: cannot write %s\n", path); return false; }
    bool ok = fwrite(data, 1, size, out) == size;
    ok &= fclose(out) == 0;
#ifndef _WIN32
    if (ok && exec) chmod(path, 0755);
#else
    (void)exec;
#endif
    if (!ok) fprintf(stderr, "monolith: export: error writing %s\n", path);
    return ok;
}

static bool CopyFileTo(const char *from, const char *to)
{
    char *dir = MS_StrDup(GetDirectoryPath(to));
    if (dir && dir[0] && !DirectoryExists(dir)) MakeDirectory(dir);
    free(dir);
    FILE *in = fopen(from, "rb");
    if (!in) { fprintf(stderr, "monolith: export: cannot read %s\n", from); return false; }
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); fprintf(stderr, "monolith: export: cannot write %s\n", to); return false; }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) ok &= fwrite(buf, 1, n, out) == n;
    ok &= !ferror(in);
    fclose(in);
    ok &= fclose(out) == 0;
    if (!ok) fprintf(stderr, "monolith: export: error copying %s to %s\n", from, to);
    return ok;
}

static int CompareLongerFirst(const void *a, const void *b)
{
    size_t la = strlen(*(char *const *)a), lb = strlen(*(char *const *)b);
    return la < lb ? 1 : la > lb ? -1 : strcmp(*(char *const *)a, *(char *const *)b);
}

// Removes what the plan wrote into `root` (its files, then the folders they made, deepest
// first, then root): only paths of this export, never anything else. False if something
// is left.
static bool RemoveWritten(const char *root, const Plan *P)
{
    bool ok = true;
    char **dirs = NULL;
    int dirCount = 0, dirCap = 0;
    for (int i = 0; i < P->count; i++) {
        char *path = Fmt("%s/%s", root, P->items[i].to);
        if (path && FileExists(path) && remove(path) != 0) ok = false;
        free(path);
        // every parent folder of the file inside root
        char *rel = MS_StrDup(P->items[i].to);
        for (char *slash = rel ? strrchr(rel, '/') : NULL; slash; slash = strrchr(rel, '/')) {
            *slash = 0;
            bool seen = false;
            for (int k = 0; k < dirCount && !seen; k++) seen = strcmp(dirs[k], rel) == 0;
            if (seen) break;
            char *copy = MS_StrDup(rel);
            char **slot = copy ? MS_PUSH(dirs, dirCount, dirCap) : NULL;
            if (!slot) { free(copy); ok = false; break; }
            *slot = copy;
        }
        free(rel);
    }
    if (dirCount) qsort(dirs, dirCount, sizeof(*dirs), CompareLongerFirst);
    for (int k = 0; k < dirCount; k++) {
        char *path = Fmt("%s/%s", root, dirs[k]);
        if (path && DirectoryExists(path) && RemoveDir(path) != 0) ok = false;
        free(path);
        free(dirs[k]);
    }
    free(dirs);
    if (DirectoryExists(root) && RemoveDir(root) != 0) ok = false;
    return ok;
}

// #endregion

// ======================================================================================
// #region Export
// ======================================================================================

static bool DirIsEmptyOrMissing(const char *dir)
{
    if (!DirectoryExists(dir)) return !FileExists(dir);
    FilePathList l = LoadDirectoryFilesEx(dir, NULL, false);
    bool empty = l.count == 0;
    UnloadDirectoryFiles(l);
    return empty;
}

static long long FileBytes(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : 0;
}

bool ExportScene(const MS_Scene *s, const char *outDirIn, char *msg, int msgSize)
{
    // A compiled map: the export ships its text and compiles that, so the text must be current
    const char *source;
    MS_SourceState state = MS_CompiledSource(s, &source);
    if (state != MS_SOURCE_TEXT) {
        if (state != MS_SOURCE_CURRENT) {
            snprintf(msg, msgSize, state == MS_SOURCE_STALE
                     ? "export: %s was compiled from an older %s: export the .map (or compile it again); nothing written"
                     : "export: %s is a compiled map and its source %s is not next to it: the export needs the map's text; "
                       "nothing written", s->path, source);
            return false;
        }
        char err[512];
        MS_Scene *text = MS_Load(source, &(MS_LoadOptions){ .noGpu = true }, err, sizeof(err));
        if (!text) {
            snprintf(msg, msgSize, "export: %s; nothing written", err);
            return false;
        }
        bool ok = ExportScene(text, outDirIn, msg, msgSize);
        MS_Unload(text);
        return ok;
    }

    char outDir[MS_PATH_MAX];
    snprintf(outDir, sizeof(outDir), "%s", outDirIn);
    size_t ol = strlen(outDir);
    while (ol > 1 && (outDir[ol - 1] == '/' || outDir[ol - 1] == '\\')) outDir[--ol] = 0;
    if (!ol) { snprintf(msg, msgSize, "export: empty folder name"); return false; }
    if (!DirIsEmptyOrMissing(outDir)) {
        snprintf(msg, msgSize, DirectoryExists(outDir) ? "export: %s exists and is not empty (nothing written)"
                                                       : "export: %s is a file, not a folder (nothing written)", outDir);
        return false;
    }
    char staging[MS_PATH_MAX + 16];
    snprintf(staging, sizeof(staging), "%s.partial", outDir);
    if (FileExists(staging) || DirectoryExists(staging)) {
        snprintf(msg, msgSize, "export: %s is left from an earlier export: move it away first (nothing written)", staging);
        return false;
    }

    // Sources: the repo's runtime and templates, the R3D tree monolith was built with
    char rt[MS_PATH_MAX], r3d[MS_PATH_MAX];
    snprintf(rt, sizeof(rt), "%s", PROJECT_PATH);
    snprintf(r3d, sizeof(r3d), "%s", R3D_SOURCE_PATH[0] ? R3D_SOURCE_PATH : TextFormat("%ssubmodules/r3d/", PROJECT_PATH));
    if (!rt[0] || !FileExists(TextFormat("%sruntime/monolith_scene.c", rt)) || !FileExists(TextFormat("%sCMakeLists.txt", r3d))) {
        snprintf(msg, msgSize, "export: needs the monolith repo (runtime/, cmake/ and the R3D source %s): run it from "
                 "the repo's build (nothing written)", r3d[0] ? r3d : "submodules/r3d/");
        return false;
    }

    // Package name = the map file's name, so <dir>/<name>/<name>.map loads by folder
    char name[MS_PATH_MAX];
    snprintf(name, sizeof(name), "%s", GetFileNameWithoutExt(s->path));
    for (char *c = name; *c; c++) if (!(isalnum((unsigned char)*c) || *c == '-' || *c == '_')) *c = '_';
    const char *root = s->root;
    size_t rootLen = strlen(root);

    // --- Plan: the map package (the map file renamed to <name>.map, every referenced file + model parts)
    Plan P = { 0 };
    char mapRel[MS_PATH_MAX + 8];
    snprintf(mapRel, sizeof(mapRel), "%s.map", name);
    AddPkg(&P, "", name, s->path, mapRel);
    for (int i = 1; i < MS_DependencyCount(s); i++) {
        const char *dep = MS_Dependency(s, i);
        if (strncmp(dep, root, rootLen) != 0) {
            fprintf(stderr, "monolith: export: %s is outside the package %s\n", dep, root);
            P.errors++;
            continue;
        }
        const char *rel = dep + rootLen;
        AddPkg(&P, root, name, rel, NULL);
        if (IsModelFile(rel)) AddModelParts(&P, root, name, rel);
    }
    // Provenance and licence files next to what was copied: the <file>.asset.md sidecar of
    // every copied file (models and textures have them), and licence texts in the same folders
    // (e.g. models/kenney-License.txt), so the export carries the records ASSETS.md cites
    for (int k = 0, n = P.count; k < n; k++) {
        const char *rel = P.items[k].pkgRel;
        char side[MS_PATH_MAX + 16];
        snprintf(side, sizeof(side), "%s.asset.md", rel);
        if (FileExists(TextFormat("%s%s", root, side))) AddPkg(&P, root, name, side, NULL);
    }
    for (int k = 0, n = P.count; k < n; k++) {
        char dirRel[MS_PATH_MAX];
        snprintf(dirRel, sizeof(dirRel), "%s", P.items[k].pkgRel);
        char *slash = strrchr(dirRel, '/');
        if (!slash) continue;               // package root: only MAP.md / ASSETS.md, below
        *slash = 0;
        FilePathList files = LoadDirectoryFiles(TextFormat("%s%s", root, dirRel));
        for (unsigned int i = 0; i < files.count; i++) {
            const char *base = GetFileName(files.paths[i]);
            char lower[256];
            int j = 0;
            for (; base[j] && j < (int)sizeof(lower) - 1; j++) lower[j] = (char)tolower((unsigned char)base[j]);
            lower[j] = 0;
            if (IsPathFile(files.paths[i]) && (strstr(lower, "license") || strstr(lower, "licence") || strstr(lower, "copying")))
                AddPkg(&P, root, name, TextFormat("%s/%s", dirRel, base), NULL);
        }
        UnloadDirectoryFiles(files);
    }
    // Docs of the package (a single v1 file outside a package has neither)
    bool ownFolder = strcmp(GetFileNameWithoutExt(s->path), GetFileName(GetDirectoryPath(s->path))) == 0;
    static const char *const DOCS[] = { "MAP.md", "ASSETS.md" };
    for (int i = 0; i < 2; i++)
        if (FileExists(TextFormat("%s%s", root, DOCS[i])) && (ownFolder || s->formatVersion >= 2)) AddPkg(&P, root, name, DOCS[i], NULL);
    int pkgFiles = P.count + 1;         // (+ the compiled map, made after writing the package)

    // --- Plan: the runtime without its editor parts, viewer.c + build.sh from the templates
    char err[MS_PATH_MAX + 512] = "";      // (room for a source path in the message)
    static const char *const RT[] = { "monolith_scene.h", "monolith_scene.c" };
    for (int i = 0; i < 2 && !err[0]; i++) {
        char *text = LoadFileText(TextFormat("%sruntime/%s", rt, RT[i]));
        char *clean = text ? StripEditor(text, RT[i], err, sizeof(err)) : NULL;
        if (!text) snprintf(err, sizeof(err), "cannot read %sruntime/%s", rt, RT[i]);
        if (text) UnloadFileText(text);
        if (clean) AddData(&P, RT[i], clean, false);
    }
    char *viewer = LoadTemplate(TextFormat("%sruntime/export/viewer.c", rt), name);
    char *build = LoadTemplate(TextFormat("%sruntime/export/build.sh", rt), name);
    if (!err[0] && (!viewer || !build)) snprintf(err, sizeof(err), "cannot read %sruntime/export/viewer.c / build.sh", rt);
    AddData(&P, "viewer.c", viewer, false);
    AddData(&P, "build.sh", build, true);
    AddCopy(&P, TextFormat("%scmake/mingw-w64-x86_64.cmake", rt), "cmake/mingw-w64-x86_64.cmake", NULL);

    // --- Plan: R3D and its dependencies, their licences
    R3DScan rs = { .P = &P };
    DirScanStats st;
    if (!DirScan(r3d, true, R3DFile, &rs, &st) || st.stopped || st.linksSkipped) {
        if (!err[0]) snprintf(err, sizeof(err), "cannot list the R3D tree %s completely (symlinked folders or too many entries)", r3d);
    }
    static const char *const LIC[][2] = {
        { "LICENSE", "LICENSES/r3d.txt" }, { "external/raylib/LICENSE", "LICENSES/raylib.txt" },
        { "external/assimp/LICENSE", "LICENSES/assimp.txt" }, { "external/uthash/LICENSE", "LICENSES/uthash.txt" },
    };
    for (int i = 0; i < 4; i++) AddCopy(&P, TextFormat("%s%s", r3d, LIC[i][0]), LIC[i][1], NULL);
    AddCopy(&P, TextFormat("%sLICENSE", rt), "LICENSES/monolith.txt", NULL);   // the runtime, viewer.c, build.sh
    // Prebuilt R3D, raylib and assimp for each target monolith has built
    static const char *const TARGETS[][3] = { { "build-linux", "linux", "Linux" }, { "build-mingw", "win", "Windows" } };
    bool prebuilt[2];
    char skipped[2][MS_PATH_MAX + 128];
    for (int t = 0; t < 2; t++) {
        prebuilt[t] = AddPrebuilt(&P, r3d, &rs, TARGETS[t][0], TARGETS[t][1], skipped[t], sizeof(skipped[t]));
        if (!prebuilt[t]) printf("monolith: export: no prebuilt %s libraries: %s; the package builds them from source\n",
                                 TARGETS[t][2], skipped[t]);
    }

    char assets[MS_PATH_MAX + 16];
    snprintf(assets, sizeof(assets), "%sASSETS.md", root);
    AddData(&P, "README.md", MakeReadme(s, name, (ownFolder || s->formatVersion >= 2) ? assets : "", pkgFiles, prebuilt), false);

    // --- Every source must exist before anything is written
    int missing = 0;
    for (int i = 0; i < P.count; i++) {
        if (!P.items[i].from || FileExists(P.items[i].from)) continue;
        fprintf(stderr, "monolith: export: missing: %s%s\n", P.items[i].from, P.items[i].pkgRel ? " (referenced by the map)" : "");
        if (!missing && !err[0]) snprintf(err, sizeof(err), "%s is missing", P.items[i].from);
        missing++;
    }
    if (err[0] || P.errors) {
        if (missing > 1) snprintf(err + strlen(err), sizeof(err) - strlen(err), " (and %d more, see the log)", missing - 1);
        snprintf(msg, msgSize, "export: %s%s; nothing written", err[0] ? err : "errors while collecting the files (see the log)",
                 err[0] && P.errors ? " (more errors in the log)" : "");
        FreePlan(&P);
        return false;
    }

    // --- Write <dir>.partial, check that the package loads, rename it to <dir>
    bool ok = true;
    long long bytesPkg = 0, bytesR3d = 0, bytesOther = 0;
    for (int i = 0; i < P.count && ok; i++) {
        const PlanItem *it = &P.items[i];
        char *to = Fmt("%s/%s", staging, it->to);
        ok = to && (it->from ? CopyFileTo(it->from, to) : WriteFileBytes(to, it->data, it->size, it->exec));
#ifndef _WIN32
        if (ok && it->from && it->exec) chmod(to, 0755);
#endif
        long long b = ok ? (it->from ? FileBytes(it->from) : (long long)it->size) : 0;
        if (it->pkgRel) bytesPkg += b;
        else if (strncmp(it->to, "r3d/", 4) == 0 || strncmp(it->to, "prebuilt/", 9) == 0) bytesR3d += b;
        else bytesOther += b;
        free(to);
    }
    // The compiled map, from the copied text: what the exported program loads
    bool written = ok;
    char *mapText = ok ? Fmt("%s/%s/%s.map", staging, name, name) : NULL;
    char *compiledTo = ok ? Fmt("%s/%s.mapb", name, name) : NULL;
    char *compiled = compiledTo ? Fmt("%s/%s", staging, compiledTo) : NULL;
    if (ok && (!mapText || !compiledTo || !compiled)) {
        ok = false;
        snprintf(msg, msgSize, "export: out of memory; nothing written");
    }
    if (ok) {
        AddWritten(&P, compiledTo);
        if (!MS_Compile(mapText, compiled, err, sizeof(err))) {
            ok = false;
            snprintf(msg, msgSize, "export: the map does not compile: %s; nothing written", err);
        }
        bytesPkg += FileBytes(compiled);
    }
    if (ok) {
        MS_Scene *check = MS_Load(compiled, &(MS_LoadOptions){ .noGpu = true }, err, sizeof(err));
        if (!check) {
            ok = false;
            snprintf(msg, msgSize, "export: the exported package does not load: %s; nothing written", err);
        } else {
            if (check->boxCount != s->boxCount || check->modelCount != s->modelCount || check->entityCount != s->entityCount) {
                ok = false;
                snprintf(msg, msgSize, "export: the exported package has a different object count; nothing written");
            }
            MS_Unload(check);
        }
    } else if (!written) snprintf(msg, msgSize, "export: writing %s failed (see the log); nothing written", staging);
    free(mapText);
    free(compiledTo);
    free(compiled);
    if (ok && DirectoryExists(outDir) && RemoveDir(outDir) != 0) {      // (empty: checked above)
        ok = false;
        snprintf(msg, msgSize, "export: cannot replace the empty folder %s: %s; nothing written", outDir, strerror(errno));
    }
    if (ok && rename(staging, outDir) != 0) {
        ok = false;
        snprintf(msg, msgSize, "export: cannot rename %s to %s: %s; nothing written", staging, outDir, strerror(errno));
    }
    if (!ok) {
        if (!RemoveWritten(staging, &P))
            snprintf(msg + strlen(msg), msgSize - strlen(msg), " (%s could not be removed completely)", staging);
        FreePlan(&P);
        return false;
    }

    // Files of the source package nothing refers to (only for a real package folder)
    if (ownFolder && DirectoryExists(root)) {
        UnusedScan u = { .P = &P, .mapPath = s->path };
        DirScanStats ust;
        DirScan(root, true, UnusedFile, &u, &ust);    // (dirscan.c: no symlinked folders, no path limit)
        if (u.skipped) printf("monolith: export: %d file(s) of %s are not used by the map\n", u.skipped, root);
        if (ust.linksSkipped) printf("monolith: export: %d symlinked folder(s) in %s not listed\n", ust.linksSkipped, root);
        if (ust.stopped) printf("monolith: export: listing of %s stopped after %d entries or %d folder levels\n", root,
                                DIRSCAN_MAX_ENTRIES, DIRSCAN_MAX_DEPTH);
    }
    int libFiles = 4 * (prebuilt[0] + prebuilt[1]);
    printf("monolith: export: %s: map package %d files (%.1f MB), R3D + raylib + assimp %d source files + %d prebuilt "
           "libraries (%.1f MB), runtime, viewer, build, README and licences %d files (%.1f MB)\n", outDir, pkgFiles,
           bytesPkg / 1e6, rs.files, libFiles, bytesR3d / 1e6, P.count - pkgFiles - rs.files - libFiles, bytesOther / 1e6);

    snprintf(msg, msgSize, "exported %s (%d files, %.0f MB; prebuilt libraries: %s): build it with ./build.sh in that folder",
             outDir, P.count, (bytesPkg + bytesR3d + bytesOther) / 1e6,
             prebuilt[0] && prebuilt[1] ? "Linux, Windows" : prebuilt[0] ? "Linux only" : prebuilt[1] ? "Windows only" : "none");
    FreePlan(&P);
    return true;
}

void ExportDefaultDir(const MS_Scene *s, char *out, int outSize)
{
    char root[1024];
    if (PROJECT_PATH[0] && DirectoryExists(PROJECT_PATH)) snprintf(root, sizeof(root), "%s", PROJECT_PATH);
    else snprintf(root, sizeof(root), "%s", GetApplicationDirectory());
    char stamp[32] = "0000-00-00_000000";
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", tm);
    snprintf(out, outSize, "%sexport/%s-%s", root, GetFileNameWithoutExt(s->path), stamp);
}

// #endregion
