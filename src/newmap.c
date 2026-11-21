#include "newmap.h"

#if MONOLITH_EDITING
// git is started through nob.h (an argument list, no shell), as the classifier starts claude; its
// implementation is in classifier.c, which only the editing (Linux) build compiles
#include "vendor/nob/nob.h"
#include <unistd.h>
#endif

// ======================================================================================
// #region New map package
// ======================================================================================

static const char *MAP_MD =
    "# %s: design brief\n"
    "\n"
    "Read this file first on every request for this map. It decides how new content looks and\n"
    "how it is built. A first request on an empty map writes this brief before building anything.\n"
    "\n"
    "## Style and mood\n"
    "\n"
    "(What kind of place is this? Era, culture, genre, the feeling it should give.)\n"
    "\n"
    "- Paint grid: 0 (no pixel look). A map with a pixel look writes here the texels per metre its\n"
    "  shaders and textures draw their pixels at (e.g. 24 for 4.2 cm pixels) and sets\n"
    "  `env paint.grid <n>` in the map (`paint_grid <n>` on a material whose pixels differ), so\n"
    "  paint from the draw mode lies on the same pixel grid.\n"
    "\n"
    "## Scale and walkability\n"
    "\n"
    "- Player: radius 0.3 m, height 1.75 m, eye at 1.62 m; steps up to 0.45 m; jumps ~0.9 m.\n"
    "- Walkways need >= 2.2 m headroom; openings >= 0.6 m wide (better >= 1 m).\n"
    "- (Typical dimensions of this map's buildings, rooms, props.)\n"
    "\n"
    "## Materials and palette\n"
    "\n"
    "(Which materials exist in %s.map, what each is for, the colour palette, how dark or\n"
    "bright albedos may be.)\n"
    "\n"
    "## Lighting and atmosphere\n"
    "\n"
    "(Time of day, sun direction, sky, fog density, extra lights and why.)\n"
    "\n"
    "## Rules for new content\n"
    "\n"
    "- When to build with boxes, when a model is acceptable.\n"
    "- How downloaded models are restyled (material overrides in the map file).\n"
    "- Polygon budget (project default: <= 50k triangles per model) and texture size limit\n"
    "  (project default: <= 2K).\n"
    "\n"
    "## Groups and landmarks\n"
    "\n"
    "| Group | What it is | Where |\n"
    "|---|---|---|\n"
    "| `ground` | Ground slab, 200 x 200 m, top at y = 0 | x, z +-100 |\n";

static const char *ASSETS_MD =
    "# %s: external assets\n"
    "\n"
    "Every file in this package that came from elsewhere (models, textures, HDRIs): one row each.\n"
    "Only CC0 / public domain, or CC-BY with the attribution recorded here.\n"
    "\n"
    "| File | Source URL | Author | Licence (+ URL) | Downloaded | sha256 of the original | Changes |\n"
    "|---|---|---|---|---|---|---|\n";

static bool WriteText(const char *path, const char *fmt, const char *a, const char *b)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "monolith: cannot write %s\n", path); return false; }
    fprintf(f, fmt, a, b);
    return fclose(f) == 0;
}

// The package's .gitignore: what a package holds that is not map content. A nested repository
// doesn't inherit the main repo's .gitignore. Keep in step with PACKAGE_GITIGNORE in
// tools/request-queue.c (docs/map-format.md section 1).
static const char *GITIGNORE =
    "# Not map content (docs/map-format.md section 1): the compiled map (monolith --compile)\n"
    "# and the draw mode's UI state, which the editor rewrites\n"
    "*.mapb\n"
    "draw.ini\n";

static bool IsSep(char c) { return c == '/' || c == '\\'; }

char *NewMapDir(const char *mapsDir, const char *target, const char **name)
{
    bool path = false;
    for (const char *c = target; *c; c++) path |= IsSep(*c);
    if (!path) {
        *name = target;
        size_t a = strlen(mapsDir), b = strlen(target);
        char *dir = malloc(a + b + 1);
        if (dir) { memcpy(dir, mapsDir, a); memcpy(dir + a, target, b + 1); }
        return dir;
    }
    // A path: drop trailing separators; the name is its last component
    size_t len = strlen(target);
    while (len > 1 && IsSep(target[len - 1])) len--;
    char *dir = MS_StrNDup(target, len);
    if (!dir) return NULL;
    const char *n = dir + len;
    while (n > dir && !IsSep(n[-1])) n--;
    *name = n;
    return dir;
}

bool NewMap(const char *mapsDir, const char *target)
{
    const char *name = NULL;
    char *dir = NewMapDir(mapsDir, target, &name);
    if (!dir) { fprintf(stderr, "monolith: --new-map: out of memory\n"); return false; }
    bool ok = NewMapIn(dir, name, mapsDir);
    free(dir);
    return ok;
}

// path = a + b (heap; NULL when out of memory)
static char *Join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *p = malloc(la + lb + 1);
    if (p) { memcpy(p, a, la); memcpy(p + la, b, lb + 1); }
    return p;
}

#if MONOLITH_EDITING
// nob's own messages ("command exited with exit code 1" for a git check that answers no) stay out
// of the output: the caller reports the outcome in one line
static void QuietNobLog(Nob_Log_Level level, const char *fmt, va_list args)
{
    (void)level;
    (void)fmt;
    (void)args;
}

// Whether an executable `name` is in one of $PATH's folders
static bool InPath(const char *name)
{
    const char *path = getenv("PATH");
    for (const char *d = path; d && *d;) {
        size_t len = strcspn(d, ":");
        char *cand = MonoStrf("%.*s/%s", (int)(len ? len : 1), len ? d : ".", name);
        bool found = cand && access(cand, X_OK) == 0;
        free(cand);
        if (found) return true;
        d += len;
        if (*d == ':') d++;
    }
    return false;
}

// `git -C <dir> <args...>` (NULL-terminated), its output to /dev/null; true when it exits 0
static bool Git(const char *dir, ...)
{
    Nob_Cmd cmd = { 0 };
    nob_cmd_append(&cmd, "git", "-C", dir);
    va_list args;
    va_start(args, dir);
    for (const char *a = va_arg(args, const char *); a; a = va_arg(args, const char *)) nob_cmd_append(&cmd, a);
    va_end(args);
    bool ok = nob_cmd_run(&cmd, .stdout_path = "/dev/null", .stderr_path = "/dev/null");
    nob_cmd_free(cmd);
    return ok;
}
#endif

// The new package as a git repository of its own (docs/map-format.md section 1): git init, then a
// first commit of every file. Not inside another repository's work tree unless that one ignores
// the folder (a nested repository there would become an embedded one). 0: a repository with its
// first commit (the configured identity only: user.useConfigOnly, git doesn't guess one); 1: no repository (git missing, or inside another work tree); 2: a repository
// without the commit (no git identity, or the commit failed). Prints one warning line for 1 and 2.
static unsigned char NewMapGit(const char *dir, const char *name)
{
#if MONOLITH_EDITING
    Nob_Log_Handler *oldLog = nob_get_log_handler();
    nob_set_log_handler(QuietNobLog);
    unsigned char result = 0;
    char *message = MonoStrf("Create %s with monolith --new-map", name);
    if (!message) {
        fprintf(stderr, "monolith: --new-map: warning: out of memory: the package is not a git repository\n");
        result = 1;
    } else if (!InPath("git")) {
        fprintf(stderr, "monolith: --new-map: warning: git not found: the package is not a git repository; install git, "
                        "then run: git -C %s init && git -C %s add -A && git -C %s commit -m \"%s\"\n",
                dir, dir, dir, message);
        result = 1;
    } else if (Git(dir, "rev-parse", "--is-inside-work-tree", NULL) && !Git(dir, "check-ignore", "-q", ".", NULL)) {
        fprintf(stderr, "monolith: --new-map: warning: %s is inside another git repository's work tree, which doesn't "
                        "ignore it: no git init (a nested repository there would become an embedded one); create "
                        "packages under maps/ to give them a history\n", dir);
        result = 1;
    } else if (!Git(dir, "init", "-q", NULL)) {
        fprintf(stderr, "monolith: --new-map: warning: git init failed: the package is not a git repository; "
                        "run: git -C %s init\n", dir);
        result = 1;
    } else if (!Git(dir, "add", "-A", NULL) || !Git(dir, "-c", "user.useConfigOnly=true", "commit", "-q", "-m", message, NULL)) {
        bool identity = Git(dir, "config", "user.name", NULL) && Git(dir, "config", "user.email", NULL);
        fprintf(stderr, "monolith: --new-map: warning: %s: the repository has no commit yet; %srun: "
                        "git -C %s add -A && git -C %s commit -m \"%s\"\n",
                identity ? "git commit failed" : "no git identity",
                identity ? "" : "set one (git config --global user.name \"...\"; git config --global user.email ...), then ",
                dir, dir, message);
        result = 2;
    }
    free(message);
    nob_set_log_handler(oldLog);
    return result;
#else
    (void)dir;
    (void)name;
    return 1;  // (the view-only build refuses --new-map before it gets here)
#endif
}

bool NewMapIn(const char *dir, const char *name, const char *mapsDir)
{
    // A folder and file name: letters, digits, '_' and '-', short enough for any file system
    // once ".map" is appended
    size_t len = strlen(name);
    bool ok = len > 0 && len <= 200;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        ok &= (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }
    if (!ok) {
        fprintf(stderr, "monolith: --new-map: the map name (the last component of a path) may only use letters, "
                        "digits, '_' and '-' (max 200)\n");
        return false;
    }
    if (DirectoryExists(dir) || FileExists(dir)) {
        fprintf(stderr, "monolith: --new-map: %s already exists (nothing changed)\n", dir);
        return false;
    }
    // maps/ itself may be created; any other parent folder must exist already
    const char *parent = GetPrevDirectoryPath(dir);
    bool inMaps = strncmp(dir, mapsDir, strlen(mapsDir)) == 0 && !strchr(dir + strlen(mapsDir), '/');
    if (inMaps && !DirectoryExists(mapsDir)) MakeDirectory(mapsDir);
    else if (!inMaps && parent && parent[0] && !DirectoryExists(parent)) {
        fprintf(stderr, "monolith: --new-map: the folder %s does not exist (nothing changed)\n", parent);
        return false;
    }
    char *path = NULL, *mapFile = NULL;
    static const char *const SUBDIRS[] = { "", "/shaders", "/models", "/textures" };
    for (int i = 0; i < 4; i++) {
        free(path);
        path = Join(dir, SUBDIRS[i]);
        if (!path || MakeDirectory(path) != 0 || !DirectoryExists(path)) {
            fprintf(stderr, "monolith: --new-map: cannot create %s\n", path ? path : dir);
            free(path);
            return false;
        }
    }
    free(path);
    char *sub = Join("/", name);
    mapFile = sub ? Join(dir, sub) : NULL;
    path = mapFile ? Join(mapFile, ".map") : NULL;
    free(sub);
    free(mapFile);
    if (!path) { fprintf(stderr, "monolith: --new-map: out of memory\n"); return false; }
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "monolith: cannot write %s\n", path); free(path); return false; }
    fprintf(f,
        "monolith 3\n"
        "# %s.map -- created by `monolith --new-map %s`. Edit while the editor runs: it reloads on save.\n"
        "# Format: docs/code/monolith_scene.md (and docs/map-format.md). The\n"
        "# design brief is MAP.md next to this file: read it first, and write it before building.\n"
        "# Paths in this file are relative to this folder.\n"
        "\n"
        "# --- Materials (boxes use \"default\" until a `use` line picks another)\n"
        "material \"default\" albedo 180 180 180 roughness 0.9 metalness 0\n"
        "\n"
        "# --- Environment: the built-in defaults, every key written out\n", name, name);
    MS_WriteEnvironment(NULL, f);
    fprintf(f,
        "\n"
        "# --- Player start (feet, yaw) and spectator viewpoints (eye, target)\n"
        "spawn 0 0 20 180\n"
        "viewpoint \"Start\" 0 1.62 20 0 1.62 0\n"
        "viewpoint \"Overview\" 40 25 40 0 0 0\n"
        "\n"
        "# --- Geometry\n"
        "group \"ground\"\n"
        "box -100 -1 -100 100 0 100\n");
    if (fclose(f) != 0) { fprintf(stderr, "monolith: cannot write %s\n", path); free(path); return false; }
    free(path);

    path = Join(dir, "/MAP.md");
    ok = path && WriteText(path, MAP_MD, name, name);
    free(path);
    if (!ok) return false;
    path = Join(dir, "/ASSETS.md");
    ok = path && WriteText(path, ASSETS_MD, name, "");
    free(path);
    if (!ok) return false;
    path = Join(dir, "/.gitignore");
    ok = path && WriteText(path, "%s%s", GITIGNORE, "");
    free(path);
    if (!ok) return false;

    // The package must load (it is generated, so a failure is a bug here)
    MS_Scene *check = MS_Load(dir, &(MS_LoadOptions){ .noGpu = true });
    if (!check) {
        fprintf(stderr, "monolith: --new-map: the new package does not load:\n%s\n", MS_LastError());
        return false;
    }
    MS_Unload(check);
    static const char *const REPO[] = { "a git repository with its first commit", "not a git repository",
                                        "a git repository, nothing committed yet" };
    unsigned char git = NewMapGit(dir, name);
    printf("monolith: created %s/ (%s.map, MAP.md, ASSETS.md, .gitignore, shaders/, models/, textures/; %s)\n", dir,
           name, REPO[git]);
    return true;
}

// #endregion
