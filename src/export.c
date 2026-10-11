// Export: a map package as a folder a plain raylib + R3D project can use (docs/design-v2.md
// section 3, PROMPT-INSTRUCTIONS.md "Export, import, examples")
//
// --export <dir> (no window) and the in-game key F9 (into export/<map>-<stamp>/) write:
//
//   <dir>/README.md            how to build it into a project, minimal code, hot reload,
//                              patched vs. upstream R3D, licences (from ASSETS.md)
//   <dir>/monolith_scene.h/.c  the scene runtime, the same source the editor runs
//   <dir>/viewer.c             a minimal viewer (= examples/01_minimal/main.c)
//   <dir>/<name>/              the map package: <name>.map, MAP.md, ASSETS.md and every file
//                              the map references (shaders, textures, panoramas, models), plus
//                              per model its provenance sidecar <file>.asset.md and the files
//                              the model file itself refers to (.gltf: buffers and images;
//                              .obj: .mtl and its texture maps)
//
// Paths: the loader only accepts package-relative paths (no absolute paths, no ".."), so the
// map file is copied byte for byte and every path in it stays valid; the map file is renamed
// to <name>.map only so that the folder loads by its name. The copy is loaded again (parse
// only) before the export reports success. Files of the source package that nothing refers
// to are not copied; the export lists them.
//
// The runtime sources come from the repo (runtime/, examples/01_minimal/main.c) while it
// exists, else from runtime/ next to the executable (build.sh copies them there, viewer.c
// included), so a copied build folder can export too.

#include "export.h"
#include <ctype.h>
#include <errno.h>
#include <time.h>

#ifndef PROJECT_PATH
#   define PROJECT_PATH ""
#endif

// ======================================================================================
// #region Files
// ======================================================================================

typedef struct {
    char **rel;             // package-relative paths already copied (heap strings)
    int count, cap;
    int errors;
} CopyList;

static void FreeCopyList(CopyList *L)
{
    for (int i = 0; i < L->count; i++) free(L->rel[i]);
    free(L->rel);
    memset(L, 0, sizeof(*L));
}

static bool CopyFile(const char *from, const char *to)
{
    char dir[MS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", GetDirectoryPath(to));
    if (dir[0] && !DirectoryExists(dir)) MakeDirectory(dir);
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

// Copies srcRoot + rel to dstRoot + rel, once per rel
static void CopyRel(CopyList *L, const char *srcRoot, const char *dstRoot, const char *rel, const char *dstRel)
{
    if (!dstRel) dstRel = rel;
    for (int i = 0; i < L->count; i++) if (strcmp(L->rel[i], dstRel) == 0) return;
    char *copy = MS_StrDup(dstRel);
    char **slot = copy ? MS_PUSH(L->rel, L->count, L->cap) : NULL;
    if (!slot) { free(copy); fprintf(stderr, "monolith: export: out of memory\n"); L->errors++; return; }
    *slot = copy;
    char from[MS_PATH_MAX * 2], to[MS_PATH_MAX * 2];
    snprintf(from, sizeof(from), "%s%s", srcRoot, rel);
    snprintf(to, sizeof(to), "%s%s", dstRoot, dstRel);
    if (!CopyFile(from, to)) L->errors++;
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
static void CopyModelParts(CopyList *L, const char *srcRoot, const char *dstRoot, const char *rel)
{
    const char *ext = GetFileExtension(rel);
    if (!ext) return;
    char full[MS_PATH_MAX * 2];
    snprintf(full, sizeof(full), "%s%s", srcRoot, rel);
    char *text = LoadFileText(full);
    if (!text) return;
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
            if (!SafeRelative(ref)) { fprintf(stderr, "monolith: export: %s refers to %s outside its folder: not copied\n", rel, ref); L->errors++; continue; }
            JoinRel(rel, ref, joined, sizeof(joined));
            CopyRel(L, srcRoot, dstRoot, joined, NULL);
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
                    CopyRel(L, srcRoot, dstRoot, joined, NULL);
                    if (obj) CopyModelParts(L, srcRoot, dstRoot, joined);
                } else if (*w) { fprintf(stderr, "monolith: export: %s refers to %s outside its folder: not copied\n", rel, w); L->errors++; }
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

static bool WriteReadme(const char *path, const MS_Scene *s, const char *name, const char *assetsPath, int fileCount)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    char when[32] = "";
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm);

    fprintf(f, "# %s: a monolith scene for raylib + R3D\n\n", name);
    fprintf(f, "Exported from the monolith map designer on %s: map format %d, %d boxes, %d models, %d materials, "
               "%d lights besides the sun, %d viewpoints, %d package files.\n\n",
            when, s->formatVersion, s->boxCount, s->modelCount, s->materialCount, s->lightCount, s->viewpointCount, fileCount);
    fprintf(f,
        "| Path | What |\n|---|---|\n"
        "| `%s/` | The map package: `%s.map` (geometry, materials, lights, sky, fog, post-processing), "
        "`MAP.md` (its design brief), `ASSETS.md` (sources and licences of external files) and every file the map uses. "
        "All paths inside are relative to this folder, so it can live anywhere. |\n"
        "| `monolith_scene.h`, `monolith_scene.c` | The scene runtime: loads, hot-reloads, draws, collides with and picks the "
        "package. C11, depends only on raylib (raymath, rlgl) and R3D. The editor runs the same code. |\n"
        "| `viewer.c` | A complete minimal program: load the package, orbit around it, `--shot` for a hidden-window screenshot. |\n\n",
        name, name);

    fprintf(f,
        "## Build\n\n"
        "You need R3D (https://github.com/Bigfoot71/r3d) and the raylib it is built with. Add `monolith_scene.c` to your "
        "sources and this folder to the include path. Define `MS_PATCHED_R3D` **only** when you build against monolith's "
        "R3D copy (`submodules/r3d` in the monolith repo), never against upstream R3D (see below).\n\n"
        "Plain compiler line (Linux, static R3D build; `$R3D` = the R3D source tree, `$R3D_BUILD` = its CMake build dir):\n\n"
        "```sh\n"
        "cc -std=c11 -O2 -I. -I\"$R3D/include\" -I\"$R3D_BUILD/external/raylib/raylib/include\" \\\n"
        "   viewer.c monolith_scene.c -o viewer \\\n"
        "   -L\"$R3D_BUILD/lib\" -lr3d -lraylib -lassimp -lzlibstatic -lstdc++ -lGL -lm -lpthread -ldl\n"
        "# Windows (MinGW-w64): ... -lr3d -lraylib -lassimp -lzlibstatic -lstdc++ -lopengl32 -lgdi32 -lwinmm -lshell32 -static\n"
        "```\n\n"
        "(`-lzlibstatic` only if R3D bundles zlib, `ASSIMP_BUILD_ZLIB=ON`; a shared R3D build needs just `-lr3d -lraylib`.)\n\n"
        "CMake, with R3D as a subdirectory of your project:\n\n"
        "```cmake\n"
        "add_subdirectory(external/r3d)                       # builds R3D and its vendored raylib / assimp\n"
        "add_executable(game main.c monolith/monolith_scene.c)\n"
        "target_include_directories(game PRIVATE monolith external/r3d/include)\n"
        "target_link_libraries(game PRIVATE r3d raylib)\n"
        "# target_compile_definitions(game PRIVATE MS_PATCHED_R3D)   # only with monolith's patched R3D copy\n"
        "```\n\n"
        "Then run `./viewer %s` (or `./viewer %s --shot shot.png --view 1` for an image without opening a visible window).\n\n",
        name, name);

    fprintf(f,
        "## Minimal code\n\n"
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
        "if your game needs other settings. **Collision and picking**: `MS_Collides(scene, box)` (strict AABB overlap "
        "against every box and model collider), `MS_Raycast(scene, origin, dir, maxDist)`. The map's player start "
        "(`scene->spawnFeet`, `spawnYawDeg`) and viewpoints (`scene->viewpoints`) are there too. "
        "`MS_InitRenderer` calls `glEnable` from libGL / opengl32, so link against it "
        "(as every raylib program does).\n\n",
        name, name);

    fprintf(f,
        "## Hot reload during development\n\n"
        "```c\n"
        "if (MS_Poll(scene, GetFrameTime()) == MS_RELOADED) {\n"
        "    MS_ApplyEnvironment(scene);       // then re-apply your own environment overrides\n"
        "}\n"
        "```\n\n"
        "`MS_Poll` checks every file of the package about twice a second (map, shaders, textures, models) and swaps "
        "the new scene in when one changed; a broken edit keeps the old scene and `MS_Error(scene)` says why "
        "(`file:line: message`). Point your game at the package monolith is editing (`maps/<name>/` in the monolith "
        "repo) and run both side by side: every save in monolith, or every Claude Code edit, appears in your game "
        "within a second. In a release build, simply don't call `MS_Poll`.\n\n");

    fprintf(f,
        "## Patched vs. upstream R3D\n\n"
        "monolith builds against its own R3D copy, which carries local patches (`submodules/r3d/patches.md` in the "
        "monolith repo). The runtime compiles against both:\n\n"
        "| | `-DMS_PATCHED_R3D` (monolith's R3D) | upstream R3D |\n|---|---|---|\n"
        "| Sun shadow | static world-fixed shadow tiles, drawn once per map load (`R3D_PushLightFixedShadow`, patch 7) | "
        "R3D's camera-fitted map, re-rendered when it no longer covers the view (`R3D_PushLightEx`) |\n"
        "| Depth pre-pass | on (`MS_RenderSettings.depthPrepass`, patch 6): expensive surface shaders run once per pixel | "
        "not available: same image, more GPU time with overdraw |\n"
        "| Fog cache (`fogCacheVoxel`) | optional baked fog sun visibility (patch 8) | not available (off) |\n"
        "| Occlusion volume (`aoVolumeVoxel`) | optional baked world-space AO for the ambient light (patch 11) | not available (off) |\n"
        "| Post passes | bloom composite folded into tonemapping, FXAA writes the final target, depth pyramid only as deep as used (patch 10) | same image, ~0.25 ms more GPU |\n"
        "| GPU profiler | available (patch 5) | not available |\n\n"
        "What that means for the image, honestly:\n\n"
        "- **Same image:** patches 2, 3, 6 and 10 skip work whose result is discarded (0 pixels differ); patch 5 only times.\n"
        "- **Slightly different:** patch 7 vs. the camera-fitted map: a few hundred pixels per 26 test shots differ "
        "by more than 2/255 (16-bit depth rounding). Patch 9 (fog march): rounding level, a handful of pixels > 2/255.\n"
        "- **Visibly different when the camera turns:** patch 4. Upstream fades the volumetric-fog shadow, the fog's "
        "transmittance, the distance fog and the sun-shadow fade by *view depth*, so the same spot looks different at "
        "the centre and at the edge of the screen (fog thinner toward the edges, a sunlit opening that glows in the "
        "centre and dims at the edge). With upstream R3D expect ~0.3%% of pixels to differ by more than 8/255 from "
        "the monolith editor, up to ~70/255 in fog-heavy views at the screen edge.\n"
        "- **Broken on AMD / Intel GPUs:** patch 1. Upstream's volumetric fog shaders fail to compile on strict GLSL "
        "drivers (a uniform member named `length`); NVIDIA accepts them. Upstream builds lose volumetric fog there "
        "unless that bug is fixed upstream.\n\n");

    fprintf(f,
        "## Sun shadow and depth clamp\n\n"
        "R3D's sun shadow map has a limited depth range toward the sun; tall casters far away get clipped and their "
        "shadows pop as the camera turns. `MS_InitRenderer()` (call it once after `R3D_Init`) enables `GL_DEPTH_CLAMP` (R3D never "
        "changes that state), which flattens such casters onto the near plane instead, as the editor does. Without it "
        "such shadows can pop; the side effect is that geometry closer than the 1 cm near plane is clamped rather than cut away.\n\n");

    fprintf(f, "## Licences and attribution\n\nFrom `%s/ASSETS.md` (the package's record of every external file; keep it with "
               "the package and credit CC-BY assets as listed):\n\n", name);
    WriteAssets(f, assetsPath);
    fprintf(f, "The scene runtime (`monolith_scene.h/.c`) and `viewer.c` are part of monolith; R3D and raylib are zlib-licensed.\n");
    return fclose(f) == 0;
}

// #endregion

// ======================================================================================
// #region Export
// ======================================================================================

// Where the runtime sources are: repo while it exists, else runtime/ next to the executable
static bool FindRuntime(char *dir, int dirSize, char *viewer, int viewerSize)
{
    if (PROJECT_PATH[0] && DirectoryExists(PROJECT_PATH)) {
        snprintf(dir, dirSize, "%sruntime/", PROJECT_PATH);
        snprintf(viewer, viewerSize, "%sexamples/01_minimal/main.c", PROJECT_PATH);
        if (FileExists(TextFormat("%smonolith_scene.c", dir)) && FileExists(viewer)) return true;
    }
    snprintf(dir, dirSize, "%sruntime/", GetApplicationDirectory());
    snprintf(viewer, viewerSize, "%sruntime/viewer.c", GetApplicationDirectory());
    return FileExists(TextFormat("%smonolith_scene.c", dir)) && FileExists(viewer);
}

static bool DirIsEmptyOrMissing(const char *dir)
{
    if (!DirectoryExists(dir)) return !FileExists(dir);
    FilePathList l = LoadDirectoryFilesEx(dir, NULL, false);
    bool empty = l.count == 0;
    UnloadDirectoryFiles(l);
    return empty;
}

bool ExportScene(const MS_Scene *s, const char *outDirIn, char *msg, int msgSize)
{
    char outDir[MS_PATH_MAX];
    snprintf(outDir, sizeof(outDir), "%s", outDirIn);
    size_t ol = strlen(outDir);
    while (ol > 1 && (outDir[ol - 1] == '/' || outDir[ol - 1] == '\\')) outDir[--ol] = 0;
    if (!ol) { snprintf(msg, msgSize, "export: empty folder name"); return false; }
    if (!DirIsEmptyOrMissing(outDir)) {
        snprintf(msg, msgSize, "export: %s exists and is not empty (nothing written)", outDir);
        return false;
    }
    char rtDir[MS_PATH_MAX], viewer[MS_PATH_MAX];
    if (!FindRuntime(rtDir, sizeof(rtDir), viewer, sizeof(viewer))) {
        snprintf(msg, msgSize, "export: runtime sources not found (runtime/monolith_scene.c, viewer.c)");
        return false;
    }

    // Package name = the map file's name, so <dir>/<name>/<name>.map loads by folder
    char name[MS_PATH_MAX];
    snprintf(name, sizeof(name), "%s", GetFileNameWithoutExt(s->path));
    for (char *c = name; *c; c++) if (!(isalnum((unsigned char)*c) || *c == '-' || *c == '_')) *c = '_';
    char pkgDir[2 * MS_PATH_MAX + 8];
    snprintf(pkgDir, sizeof(pkgDir), "%s/%s/", outDir, name);
    MakeDirectory(pkgDir);
    if (!DirectoryExists(pkgDir)) {
        snprintf(msg, msgSize, "export: cannot create %s: %s", pkgDir, strerror(errno));
        return false;
    }

    CopyList L = { 0 };
    const char *root = s->root;
    size_t rootLen = strlen(root);

    // The map file (renamed to <name>.map), then every referenced file + model parts
    char mapRel[MS_PATH_MAX + 8];
    snprintf(mapRel, sizeof(mapRel), "%s.map", name);
    CopyRel(&L, "", pkgDir, s->path, mapRel);
    for (int i = 1; i < MS_DependencyCount(s); i++) {
        const char *dep = MS_Dependency(s, i);
        if (strncmp(dep, root, rootLen) != 0) {
            fprintf(stderr, "monolith: export: %s is outside the package %s\n", dep, root);
            L.errors++;
            continue;
        }
        const char *rel = dep + rootLen;
        CopyRel(&L, root, pkgDir, rel, NULL);
        if (IsModelFile(rel)) {
            CopyModelParts(&L, root, pkgDir, rel);
            char side[MS_PATH_MAX + 16];
            snprintf(side, sizeof(side), "%s.asset.md", rel);
            if (FileExists(TextFormat("%s%s", root, side))) CopyRel(&L, root, pkgDir, side, NULL);
        }
    }
    // Provenance and licence files next to what was copied: the <file>.asset.md sidecar of
    // every copied file (textures have them too), and licence texts in the same folders
    // (e.g. models/kenney-License.txt), so the export carries the records ASSETS.md cites
    for (int k = 0; k < L.count; k++) {
        char side[MS_PATH_MAX + 16];
        snprintf(side, sizeof(side), "%s.asset.md", L.rel[k]);
        if (FileExists(TextFormat("%s%s", root, side))) CopyRel(&L, root, pkgDir, side, NULL);
    }
    for (int k = 0, n = L.count; k < n; k++) {
        char dirRel[MS_PATH_MAX];
        snprintf(dirRel, sizeof(dirRel), "%s", L.rel[k]);
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
            if (strstr(lower, "license") || strstr(lower, "licence") || strstr(lower, "copying"))
                CopyRel(&L, root, pkgDir, TextFormat("%s/%s", dirRel, base), NULL);
        }
        UnloadDirectoryFiles(files);
    }
    // Docs of the package (a single v1 file outside a package has neither)
    bool ownFolder = strcmp(GetFileNameWithoutExt(s->path), GetFileName(GetDirectoryPath(s->path))) == 0;
    static const char *const DOCS[] = { "MAP.md", "ASSETS.md" };
    for (int i = 0; i < 2; i++)
        if (FileExists(TextFormat("%s%s", root, DOCS[i])) && (ownFolder || s->formatVersion >= 2)) CopyRel(&L, root, pkgDir, DOCS[i], NULL);

    // Runtime + viewer
    static const char *const RT[] = { "monolith_scene.h", "monolith_scene.c" };
    for (int i = 0; i < 2; i++)
        if (!CopyFile(TextFormat("%s%s", rtDir, RT[i]), TextFormat("%s/%s", outDir, RT[i]))) L.errors++;
    if (!CopyFile(viewer, TextFormat("%s/viewer.c", outDir))) L.errors++;

    char assets[sizeof(pkgDir) + 16];
    snprintf(assets, sizeof(assets), "%sASSETS.md", pkgDir);
    if (!WriteReadme(TextFormat("%s/README.md", outDir), s, name, assets, L.count)) L.errors++;

    // Files of the source package nothing refers to (only for a real package folder)
    if (ownFolder && DirectoryExists(root)) {
        char scanDir[MS_PATH_MAX];     // raylib joins with '/', so no trailing slash
        snprintf(scanDir, sizeof(scanDir), "%.*s", (int)(rootLen > 1 ? rootLen - 1 : rootLen), root);
        FilePathList all = LoadDirectoryFilesEx(scanDir, NULL, true);
        int skipped = 0;
        for (unsigned int i = 0; i < all.count; i++) {
            const char *p = all.paths[i];
            if (strncmp(p, root, rootLen) != 0) continue;
            const char *rel = p + rootLen;
            bool copied = strcmp(p, s->path) == 0;
            for (int k = 0; k < L.count && !copied; k++) copied = strcmp(L.rel[k], rel) == 0;
            if (!copied) {
                printf("monolith: export: not referenced, not copied: %s\n", p);
                skipped++;
            }
        }
        UnloadDirectoryFiles(all);
        if (skipped) printf("monolith: export: %d file(s) of %s are not used by the map\n", skipped, root);
    }

    // The copy must load on its own (parse + every referenced file present)
    char err[512];
    MS_Scene *check = MS_Load(pkgDir, &(MS_LoadOptions){ .noGpu = true }, err, sizeof(err));
    if (!check) {
        snprintf(msg, msgSize, "export: the exported package does not load: %s", err);
        FreeCopyList(&L);
        return false;
    }
    bool same = check->boxCount == s->boxCount && check->modelCount == s->modelCount;
    MS_Unload(check);
    bool ok = same && !L.errors;
    if (!ok) snprintf(msg, msgSize, "export: %s written with %d error(s)%s (see the log)", outDir, L.errors,
                      same ? "" : ", and the copy has a different object count");
    else snprintf(msg, msgSize, "exported %s: %d package files + runtime + README.md", outDir, L.count);
    FreeCopyList(&L);
    return ok;
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
