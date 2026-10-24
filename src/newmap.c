#include "newmap.h"

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
        "monolith 2\n"
        "# %s.map -- created by `monolith --new-map %s`. Edit while the game runs: it reloads on save.\n"
        "# Format: the header comment of runtime/monolith_scene.c (and docs/map-format.md). The\n"
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

    // The package must load (it is generated, so a failure is a bug here)
    char err[512];
    MS_Scene *check = MS_Load(dir, &(MS_LoadOptions){ .noGpu = true }, err, sizeof(err));
    if (!check) {
        fprintf(stderr, "monolith: --new-map: the new package does not load:\n%s\n", err);
        return false;
    }
    MS_Unload(check);
    printf("monolith: created %s/ (%s.map, MAP.md, ASSETS.md, shaders/, models/, textures/)\n", dir, name);
    return true;
}

// #endregion
