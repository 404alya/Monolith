// --inspect-model: what a model file contains, as stable plain text for an AI (or a human)
// to read before placing it in a map. Uses assimp directly (R3D's importer is assimp with the
// same post-processing), so it needs no window and no GPU. The numbers match what the scene
// runtime gets from R3D_LoadModel: node transforms baked into the vertices, no unit scale.
#include "inspect.h"
#include <assimp/cimport.h>
#include <assimp/material.h>
#include <assimp/metadata.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TRI_BUDGET      50000   // project default per model (docs/design-v2.md 6)
#define TEX_BUDGET      2048    // px

// The post-processing R3D's importer always applies (submodules/r3d/src/r3d_importer.c), so
// triangle counts match
#define R3D_POSTPROCESS (aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices | aiProcess_Triangulate | \
                         aiProcess_GenUVCoords | aiProcess_SortByPType | aiProcess_FlipUVs | \
                         aiProcess_RemoveRedundantMaterials | aiProcess_GenNormals)

typedef struct {
    struct aiVector3D mn, mx;
    long triangles, vertices;
    int lineOrPointMeshes;
} Totals;

static void Walk(const struct aiScene *sc, const struct aiNode *node, struct aiMatrix4x4 parent, Totals *t, int *meshUse)
{
    struct aiMatrix4x4 global = parent;
    aiMultiplyMatrix4(&global, &node->mTransformation);     // global = parent * local
    for (unsigned i = 0; i < node->mNumMeshes; i++) {
        unsigned mi = node->mMeshes[i];
        const struct aiMesh *m = sc->mMeshes[mi];
        // R3D loads each mesh once, with the transform of the last node that uses it
        if (meshUse[mi]++) continue;
        if (!(m->mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) t->lineOrPointMeshes++;
        else t->triangles += m->mNumFaces;
        t->vertices += m->mNumVertices;
        for (unsigned v = 0; v < m->mNumVertices; v++) {
            struct aiVector3D p = m->mVertices[v];
            aiTransformVecByMatrix4(&p, &global);
            t->mn.x = fminf(t->mn.x, p.x); t->mn.y = fminf(t->mn.y, p.y); t->mn.z = fminf(t->mn.z, p.z);
            t->mx.x = fmaxf(t->mx.x, p.x); t->mx.y = fmaxf(t->mx.y, p.y); t->mx.z = fmaxf(t->mx.z, p.z);
        }
    }
    for (unsigned c = 0; c < node->mNumChildren; c++) Walk(sc, node->mChildren[c], global, t, meshUse);
}

// glTF JSON (a .gltf file, or the JSON chunk of a .glb), malloc'd; NULL if not glTF
static char *GltfJson(const char *path)
{
    const char *ext = GetFileExtension(path);
    if (!ext) return NULL;
    char e[16];
    snprintf(e, sizeof(e), "%s", TextToLower(ext));
    if (strcmp(e, ".gltf") == 0) {
        int size = 0;
        unsigned char *data = LoadFileData(path, &size);
        if (!data) return NULL;
        char *s = malloc(size + 1);
        if (s) { memcpy(s, data, size); s[size] = 0; }
        UnloadFileData(data);
        return s;
    }
    if (strcmp(e, ".glb") != 0) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char h[20];
    char *s = NULL;
    if (fread(h, 1, 20, f) == 20 && memcmp(h, "glTF", 4) == 0 && memcmp(h + 16, "JSON", 4) == 0) {
        unsigned len = h[12] | (h[13] << 8) | (h[14] << 16) | ((unsigned)h[15] << 24);
        if (len < 64u * 1024 * 1024 && (s = malloc(len + 1))) {
            if (fread(s, 1, len, f) != len) { free(s); s = NULL; }
            else s[len] = 0;
        }
    }
    fclose(f);
    return s;
}

// The strings of the JSON array "key": [ "a", "b" ] (first occurrence), comma-separated
static void JsonStringArray(const char *json, const char *key, char *out, int outSize)
{
    out[0] = 0;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = json ? strstr(json, pat) : NULL;
    if (!p) return;
    p = strchr(p + strlen(pat), '[');
    if (!p) return;
    const char *end = strchr(p, ']');
    int n = 0;
    for (const char *q = p; end && q < end && n < outSize - 1; q++) {
        if (*q != '"') continue;
        const char *e = strchr(q + 1, '"');
        if (!e || e > end) break;
        n += snprintf(out + n, outSize - n, "%s%.*s", n ? ", " : "", (int)(e - q - 1), q + 1);
        q = e;
    }
}

// "key": "value" inside the glTF "asset" object
static void JsonAssetString(const char *json, const char *key, char *out, int outSize)
{
    out[0] = 0;
    const char *a = json ? strstr(json, "\"asset\"") : NULL;
    if (!a) return;
    const char *close = strchr(a, '}');
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(a, pat);
    if (!p || (close && p > close)) return;
    p = strchr(p + strlen(pat), '"');
    if (!p) return;
    const char *e = strchr(p + 1, '"');
    if (e) snprintf(out, outSize, "%.*s", (int)(e - p - 1), p + 1);
}

static int warnings;
static void Warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  - ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    warnings++;
}

// The warning lines collected while printing (a heap text: a model can have any number)
typedef struct { char *s; int len, cap; } WarnText;

static void WarnAdd(WarnText *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || !MS_RESERVE(w->s, w->cap, w->len + n + 1)) return;
    va_start(ap, fmt);
    vsnprintf(w->s + w->len, n + 1, fmt, ap);
    va_end(ap);
    w->len += n;
}

// One texture slot of a material: where it comes from and its size
typedef struct { const char *name; enum aiTextureType types[3]; } SlotDef;

static void PrintTexture(const struct aiScene *sc, const char *modelDir, const char *path, WarnText *warn)
{
    int w = 0, h = 0;
    const char *kind;
    char where[1300];
    if (path[0] == '*' || aiGetEmbeddedTexture(sc, path)) {
        const struct aiTexture *t = aiGetEmbeddedTexture(sc, path);
        kind = "embedded";
        if (t && t->mHeight == 0) {
            char hint[16];
            snprintf(hint, sizeof(hint), ".%.*s", (int)sizeof(t->achFormatHint), t->achFormatHint);
            Image img = LoadImageFromMemory(hint, (const unsigned char *)t->pcData, (int)t->mWidth);
            w = img.width; h = img.height;
            UnloadImage(img);
            snprintf(where, sizeof(where), "%s (%s, %u bytes)", path, hint + 1, t->mWidth);
        } else if (t) {
            w = (int)t->mWidth; h = (int)t->mHeight;
            snprintf(where, sizeof(where), "%s (raw texels)", path);
        } else snprintf(where, sizeof(where), "%s (missing in the file)", path);
    } else {
        kind = "external";
        char full[2200];
        snprintf(full, sizeof(full), "%s/%s", modelDir, path);
        for (char *c = full; *c; c++) if (*c == '\\') *c = '/';
        if (FileExists(full)) {
            Image img = LoadImage(full);
            w = img.width; h = img.height;
            UnloadImage(img);
            snprintf(where, sizeof(where), "\"%s\"", path);
            if (w == 0) WarnAdd(warn, "  - texture \"%s\" cannot be decoded (R3D loads PNG, JPG, HDR, DDS, GIF, QOI; not TGA/BMP/PSD/KTX/WebP)\n", path);
        } else {
            snprintf(where, sizeof(where), "\"%s\" (NOT FOUND next to the model)", path);
            WarnAdd(warn, "  - external texture \"%s\" not found\n", path);
        }
    }
    if (w > 0) printf("%s %s, %d x %d\n", kind, where, w, h);
    else printf("%s %s, size unknown\n", kind, where);
    if (w > TEX_BUDGET || h > TEX_BUDGET)
        WarnAdd(warn, "  - texture %s is %d x %d, larger than %d (project budget)\n", where, w, h, TEX_BUDGET);
}

static const char *ShadingName(int m)
{
    switch (m) {
        case aiShadingMode_Flat: return "flat";
        case aiShadingMode_Gouraud: return "gouraud";
        case aiShadingMode_Phong: return "phong";
        case aiShadingMode_Blinn: return "blinn";
        case aiShadingMode_Unlit: return "unlit";
        case aiShadingMode_PBR_BRDF: return "pbr";
        default: return "other";
    }
}

int InspectModel(const char *path)
{
    SetTraceLogLevel(LOG_WARNING);     // raylib logs to stdout: keep the report clean
    if (!FileExists(path)) {
        printf("inspect-model: %s: file not found\n", path);
        return 1;
    }
    // What the file refers to must stay in its folder (and exist) before assimp opens any of
    // it: a downloaded model could name ../../../etc/passwd as a buffer or texture (QA2 F13);
    // the map loader would refuse it anyway (same check, package root instead of the folder)
    {
        char dir[1100], err[1400];
        snprintf(dir, sizeof(dir), "%s/", GetDirectoryPath(path));
        if (!MS_CheckModelReferences(dir, GetFileName(path), err, sizeof(err))) {
            printf("inspect-model: %s: not imported: %s\n", path, err);
            printf("  (a model may only refer to files in its own folder or below it; the map loader rejects it too)\n");
            return 1;
        }
    }
    const struct aiScene *sc = aiImportFile(path, R3D_POSTPROCESS);
    if (!sc || !sc->mRootNode) {
        printf("inspect-model: %s: cannot import: %s\n", path, aiGetErrorString());
        printf("  (R3D cannot load it either. Common causes: KHR_draco_mesh_compression, EXT_meshopt_compression,\n"
               "   KHR_texture_basisu, a missing .bin, or an unsupported format; R3D's assimp has the glTF/GLB, OBJ,\n"
               "   FBX, DAE and IQM importers)\n");
        return 1;
    }
    char modelDir[1100];
    snprintf(modelDir, sizeof(modelDir), "%s", GetDirectoryPath(path));
    char *json = GltfJson(path);
    WarnText warn = { 0 };
    warnings = 0;

    printf("inspect-model: %s\n", path);
    printf("file: %s, %d bytes\n", GetFileName(path), GetFileLength(path));
    const char *ext = GetFileExtension(path);
    char extLow[16];
    snprintf(extLow, sizeof(extLow), "%s", ext ? TextToLower(ext) : "");
    bool gltf = strcmp(extLow, ".gltf") == 0 || strcmp(extLow, ".glb") == 0;

    // glTF facts
    char used[1024], required[1024], gen[256], ver[64];
    JsonStringArray(json, "extensionsUsed", used, sizeof(used));
    JsonStringArray(json, "extensionsRequired", required, sizeof(required));
    JsonAssetString(json, "generator", gen, sizeof(gen));
    JsonAssetString(json, "version", ver, sizeof(ver));
    if (gltf) {
        printf("format: glTF %s %s\n", ver[0] ? ver : "?", strcmp(extLow, ".glb") == 0 ? "binary (.glb)" : "JSON (.gltf)");
        printf("generator: %s\n", gen[0] ? gen : "(none)");
        printf("extensionsUsed: %s\n", used[0] ? used : "(none)");
        printf("extensionsRequired: %s\n", required[0] ? required : "(none)");
    } else printf("format: %s\n", extLow[0] ? extLow + 1 : "?");

    // Metadata assimp exposes (FBX: UnitScaleFactor, UpAxis, ...; glTF: generator, copyright)
    printf("metadata:");
    int metaCount = 0;
    if (sc->mMetaData) {
        const struct aiMetadata *md = sc->mMetaData;
        for (unsigned i = 0; i < md->mNumProperties; i++) {
            const struct aiMetadataEntry *e = &md->mValues[i];
            printf("\n  %s = ", md->mKeys[i].data);
            metaCount++;
            switch (e->mType) {
                case AI_BOOL: printf("%s", *(bool *)e->mData ? "true" : "false"); break;
                case AI_INT32: printf("%d", *(int32_t *)e->mData); break;
                case AI_UINT64: printf("%llu", (unsigned long long)*(uint64_t *)e->mData); break;
                case AI_FLOAT: printf("%g", *(float *)e->mData); break;
                case AI_DOUBLE: printf("%g", *(double *)e->mData); break;
                case AI_AISTRING: printf("\"%s\"", ((struct aiString *)e->mData)->data); break;
                case AI_AIVECTOR3D: { struct aiVector3D *v = e->mData; printf("%g %g %g", v->x, v->y, v->z); break; }
                default: printf("(type %d)", (int)e->mType); break;
            }
        }
    }
    printf("%s\n", metaCount ? "" : " (none)");

    // Geometry, as R3D bakes it
    int *meshUse = calloc(sc->mNumMeshes + 1, sizeof(int));
    Totals t = { { INFINITY, INFINITY, INFINITY }, { -INFINITY, -INFINITY, -INFINITY }, 0, 0, 0 };
    struct aiMatrix4x4 id;
    aiIdentityMatrix4(&id);
    Walk(sc, sc->mRootNode, id, &t, meshUse);
    int unused = 0;
    for (unsigned i = 0; i < sc->mNumMeshes; i++) unused += meshUse[i] == 0;
    printf("meshes: %u (%d not referenced by any node: not drawn)\n", sc->mNumMeshes, unused);
    printf("triangles: %ld\nvertices: %ld\n", t.triangles, t.vertices);
    if (t.lineOrPointMeshes) printf("line/point meshes: %d (not picked)\n", t.lineOrPointMeshes);
    if (t.vertices == 0) { t.mn = (struct aiVector3D){ 0, 0, 0 }; t.mx = t.mn; }
    float sx = t.mx.x - t.mn.x, sy = t.mx.y - t.mn.y, sz = t.mx.z - t.mn.z;
    printf("bounds: min %.4f %.4f %.4f  max %.4f %.4f %.4f (model units, node transforms applied)\n",
           t.mn.x, t.mn.y, t.mn.z, t.mx.x, t.mx.y, t.mx.z);
    printf("size: %.4f x %.4f x %.4f (x y z)\n", sx, sy, sz);
    printf("origin: bottom centre of the bounds is at %.4f %.4f %.4f; the origin is %.4f above the bottom\n",
           (t.mn.x + t.mx.x) * 0.5f, t.mn.y, (t.mn.z + t.mx.z) * 0.5f, -t.mn.y);

    // Up axis and units
    float big = fmaxf(sx, fmaxf(sy, sz));
    if (gltf) printf("up axis: +Y (glTF is Y-up by definition)\nunits: metres (glTF is metres by definition; the size above is the truth)\n");
    else {
        int up = -1, upSign = 1;
        double unit = 0.0;
        if (sc->mMetaData) {
            for (unsigned i = 0; i < sc->mMetaData->mNumProperties; i++) {
                const char *k = sc->mMetaData->mKeys[i].data;
                const struct aiMetadataEntry *e = &sc->mMetaData->mValues[i];
                if (strcmp(k, "UpAxis") == 0 && e->mType == AI_INT32) up = *(int32_t *)e->mData;
                if (strcmp(k, "UpAxisSign") == 0 && e->mType == AI_INT32) upSign = *(int32_t *)e->mData;
                if (strcmp(k, "UnitScaleFactor") == 0 && e->mType == AI_DOUBLE) unit = *(double *)e->mData;
                if (strcmp(k, "UnitScaleFactor") == 0 && e->mType == AI_FLOAT) unit = *(float *)e->mData;
            }
        }
        if (up >= 0) printf("up axis: %c%c (file metadata UpAxis; check the size: the model's height should be in Y)\n",
                            upSign < 0 ? '-' : '+', "XYZ"[up % 3]);
        else printf("up axis: unknown (no metadata; check the size: the model's height should be in Y)\n");
        if (unit > 0.0) printf("units: UnitScaleFactor %g (FBX: centimetres per file unit; R3D applies no unit scale, "
                               "so 1 = the numbers are cm -> scale 0.01, 100 = metres)\n", unit);
        else printf("units: unknown (no metadata; R3D applies no unit scale: judge by the size)\n");
        if (sz > 1.5f * sy && sz > 1.5f * sx)
            WarnAdd(&warn, "  - the model is tallest in Z: probably Z-up; use rot 0 -90 0 (pitch) or re-export Y-up\n");
    }
    if (big > 0.0f && big < 0.05f)
        WarnAdd(&warn, "  - largest size %.4f: tiny; the file may be in other units or a kit in tile units (use scale)\n", big);
    if (big > 500.0f)
        WarnAdd(&warn, "  - largest size %.1f: huge; the file is probably in centimetres (scale 0.01)\n", big);

    // Materials (= the slots for material.N)
    printf("materials: %u (slots for material.N, 0-based)\n", sc->mNumMaterials);
    static const SlotDef SLOTS[] = {
        { "albedo", { aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE, aiTextureType_NONE } },
        { "normal", { aiTextureType_NORMALS, aiTextureType_NONE, aiTextureType_NONE } },
        { "occlusion", { aiTextureType_AMBIENT_OCCLUSION, aiTextureType_LIGHTMAP, aiTextureType_NONE } },
        { "roughness", { aiTextureType_DIFFUSE_ROUGHNESS, aiTextureType_SHININESS, aiTextureType_NONE } },
        { "metalness", { aiTextureType_METALNESS, aiTextureType_NONE, aiTextureType_NONE } },
        { "emission", { aiTextureType_EMISSIVE, aiTextureType_NONE, aiTextureType_NONE } },
    };
    int textures = 0, unlit = 0, metal = 0;
    for (unsigned i = 0; i < sc->mNumMaterials; i++) {
        const struct aiMaterial *m = sc->mMaterials[i];
        struct aiString name;
        if (aiGetMaterialString(m, AI_MATKEY_NAME, &name) != AI_SUCCESS) name.data[0] = 0, name.length = 0;
        int meshes = 0;
        long tris = 0;
        for (unsigned k = 0; k < sc->mNumMeshes; k++)
            if (sc->mMeshes[k]->mMaterialIndex == i) { meshes++; tris += sc->mMeshes[k]->mNumFaces; }
        int shading = -1;
        aiGetMaterialInteger(m, AI_MATKEY_SHADING_MODEL, &shading);
        bool isUnlit = shading == aiShadingMode_Unlit;
        unlit += isUnlit;
        printf("  [%u] \"%s\": %d mesh%s, %ld triangles, shading %s%s\n", i, name.data, meshes, meshes == 1 ? "" : "es",
               tris, shading >= 0 ? ShadingName(shading) : "?", isUnlit ? " (UNLIT in R3D: no light, no shadow on it)" : "");
        struct aiColor4D c;
        if (aiGetMaterialColor(m, AI_MATKEY_BASE_COLOR, &c) == AI_SUCCESS || aiGetMaterialColor(m, AI_MATKEY_COLOR_DIFFUSE, &c) == AI_SUCCESS)
            printf("      colour: %d %d %d %d (0-255, multiplies the albedo map)\n", (int)lroundf(fminf(c.r, 1) * 255),
                   (int)lroundf(fminf(c.g, 1) * 255), (int)lroundf(fminf(c.b, 1) * 255), (int)lroundf(fminf(c.a, 1) * 255));
        float f;
        if (aiGetMaterialFloat(m, AI_MATKEY_METALLIC_FACTOR, &f) == AI_SUCCESS) {
            printf("      metallic: %g\n", f);
            struct aiString mt;
            if (f >= 0.99f && !isUnlit && aiGetMaterialTexture(m, aiTextureType_METALNESS, 0, &mt, NULL, NULL, NULL, NULL, NULL, NULL) != AI_SUCCESS)
                metal++, WarnAdd(&warn, "  - material [%u] \"%s\" is fully metallic (metallic 1, no metalness map): "
                               "unless it is metal it renders as dark tinted metal (common in kits exported with the glTF default); "
                               "override it with a lit map material (metalness 0)\n", i, name.data);
        }
        if (aiGetMaterialFloat(m, AI_MATKEY_ROUGHNESS_FACTOR, &f) == AI_SUCCESS) printf("      roughness: %g\n", f);
        struct aiString am;
        if (aiGetMaterialString(m, "$mat.gltf.alphaMode", 0, 0, &am) == AI_SUCCESS) printf("      alphaMode: %s\n", am.data);
        int two = 0;
        if (aiGetMaterialInteger(m, AI_MATKEY_TWOSIDED, &two) == AI_SUCCESS && two) printf("      double-sided (cull none)\n");
        for (int s = 0; s < (int)(sizeof(SLOTS) / sizeof(SLOTS[0])); s++) {
            for (int k = 0; k < 3 && SLOTS[s].types[k] != aiTextureType_NONE; k++) {
                struct aiString tp;
                if (aiGetMaterialTexture(m, SLOTS[s].types[k], 0, &tp, NULL, NULL, NULL, NULL, NULL, NULL) != AI_SUCCESS) continue;
                printf("      %s map: ", SLOTS[s].name);
                PrintTexture(sc, modelDir, tp.data, &warn);
                textures++;
                break;
            }
        }
    }
    printf("textures: %d map slot%s use a texture; embedded images in the file: %u\n", textures, textures == 1 ? "" : "s", sc->mNumTextures);
    printf("animations: %u%s\n", sc->mNumAnimations, sc->mNumAnimations ? " (ignored: monolith draws the static pose)" : "");

    // Warnings
    printf("warnings:\n");
    if (!unlit && strstr(used, "KHR_materials_unlit"))
        Warn("KHR_materials_unlit is listed in extensionsUsed but no material uses it: the materials are lit PBR");
    if (unlit) Warn("%d material(s) are unlit (KHR_materials_unlit / unlit shading): they ignore the sun and fog light; "
                    "override them with a lit map material (material / material.N) to fit a lit scene", unlit);
    if (t.triangles > TRI_BUDGET) Warn("%ld triangles: over the project budget of 50000 per model (decimate, or check MAP.md)", t.triangles);
    static const char *const BAD_EXT[] = { "KHR_draco_mesh_compression", "EXT_meshopt_compression", "KHR_texture_basisu",
                                           "EXT_texture_webp", "KHR_mesh_quantization" };
    for (int i = 0; i < 5; i++)
        if (strstr(used, BAD_EXT[i])) Warn("extension %s: not supported by R3D's importer (re-export without it)", BAD_EXT[i]);
    if (strstr(used, "KHR_texture_transform")) Warn("extension KHR_texture_transform: ignored by R3D (UV offsets/scales in the file are lost)");
    if (sc->mNumMeshes == 0) Warn("no meshes: the map loader rejects this file (\"no meshes in the file\")");
    else if (t.triangles == 0) Warn("no triangles (only points or lines): nothing of it would be drawn");
    if (warn.len > 0) {
        printf("%s", warn.s);
        for (const char *c = warn.s; *c; c++) warnings += *c == '\n';
    }
    free(warn.s);
    if (warnings == 0) printf("  (none)\n");

    // How to place it
    printf("placement:\n");
    printf("  model \"models/%s\" x y z origin bottom%s\n", GetFileName(path),
           big > 0.0f && big < 0.05f ? " scale <s>" : (unlit || metal) ? " material \"<lit material>\"" : "");
    printf("  origin bottom stands it on y; a 1.8 m person is %.2f model units tall at scale 1\n", 1.8f);

    free(meshUse);
    free(json);
    aiReleaseImport(sc);
    return 0;
}
