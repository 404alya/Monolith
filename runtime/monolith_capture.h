// monolith_capture.h -- record what an R3D program draws in one frame and write it as a
// monolith map package (the "capture shim" import route; AI-MAP-TOOL.md "Import requests").
//
// For scenes that are hard to read from source (built in loops, from data files, procedurally):
// compile this header into the program, run it once with MONOLITH_CAPTURE set, and it writes a
// package from what actually reached R3D. No change to the program's own source is needed:
//
//   cc -c main.c -include monolith_capture.h -DMONOLITH_CAPTURE_IMPLEMENTATION -I<monolith>/runtime ...
//   cc main.o <monolith>/runtime/monolith_scene.c ... -o game_capture     (link the runtime too)
//   MONOLITH_CAPTURE=/tmp/cap/forest ./game_capture --shot /tmp/x.png     (any run that renders)
//
// (-include puts the header in front of the file; or #include it after <r3d/r3d.h> in every
// file that draws, with MONOLITH_CAPTURE_IMPLEMENTATION defined in exactly one of them.)
// The macros below route R3D_LoadModel, R3D_GenMeshCube, R3D_GenProceduralSky,
// R3D_LoadCubemap, R3D_LoadSurfaceShader, R3D_Begin*, R3D_PushLight*, R3D_DrawMesh*,
// R3D_DrawModel* and R3D_End through recorders that call the real function. Without
// MONOLITH_CAPTURE in the environment they only pass through. With it, frame
// MONOLITH_CAPTURE_FRAME (default 3, so lazy setup has happened) is recorded and written at its
// R3D_End to <dir>/<name>.map (+ models/, textures/, shaders/, MAP.md, ASSETS.md, CAPTURE.md);
// <name> = the folder's name. An existing non-empty folder is never overwritten.
//
// What it translates:
//   - cube meshes from R3D_GenMeshCube drawn with an axis-aligned transform -> `box`
//     (rotated ones become their world AABB and are listed as approximations)
//   - models loaded with R3D_LoadModel -> the file copied to models/ + `model` (position,
//     yaw/pitch/roll, per-axis scale; R = Ry * Rx * Rz like the map format)
//   - plain material values of boxes (albedo, roughness, metalness, emission, unlit) and
//     their textures (read back from the GPU and saved as PNG) -> `material` + `use`
//   - surface shaders loaded from a file -> copied to shaders/ (uniform values: not seen)
//   - the first shadowed directional light -> env sun.*; other lights -> `light`
//   - R3D_GetEnvironment() at R3D_End -> every `env` key (MS_WriteR3DEnvironment); a sky
//     from R3D_GenProceduralSky -> sky.*; a panorama from R3D_LoadCubemap -> sky.panorama
//   - the frame's camera -> `spawn` + `viewpoint "Captured"`
// What it lists in CAPTURE.md instead: every other mesh (generated spheres, capsules, ...,
// meshes from R3D_LoadMesh: their vertices never come back from the GPU), instanced and
// animated draws, decals, material changes on loaded models, shader uniform values, the
// camera's field of view if it is not 70 degrees, and everything outside R3D's draw calls
// (gameplay, custom render passes, animation state).
#ifndef MONOLITH_CAPTURE_H
#define MONOLITH_CAPTURE_H

#include <r3d/r3d.h>

#ifdef __cplusplus
extern "C" {
#endif

R3D_Model MSC_LoadModel(const char *filePath);
R3D_Mesh MSC_GenMeshCube(float width, float height, float length);
R3D_Cubemap MSC_GenProceduralSky(int size, R3D_ProceduralSky params);
R3D_Cubemap MSC_LoadCubemap(const char *fileName, R3D_CubemapLayout layout);
R3D_SurfaceShader *MSC_LoadSurfaceShader(const char *filePath);
void MSC_Begin(Camera3D camera);
void MSC_BeginPro(R3D_View view);
void MSC_End(void);
void MSC_PushLight(R3D_Light light);
void MSC_PushLightEx(R3D_Light light, R3D_ShadowMap map, bool updateShadow);
void MSC_DrawMesh(R3D_Mesh mesh, R3D_Material material, Vector3 position, float scale);
void MSC_DrawMeshEx(R3D_Mesh mesh, R3D_Material material, Vector3 position, Quaternion rotation, Vector3 scale);
void MSC_DrawMeshPro(R3D_Mesh mesh, R3D_Material material, Matrix transform);
void MSC_DrawMeshInstanced(R3D_Mesh mesh, R3D_Material material, R3D_InstanceBuffer instances, int count);
void MSC_DrawModel(R3D_Model model, Vector3 position, float scale);
void MSC_DrawModelEx(R3D_Model model, Vector3 position, Quaternion rotation, Vector3 scale);
void MSC_DrawModelPro(R3D_Model model, Matrix transform);
void MSC_DrawModelInstanced(R3D_Model model, R3D_InstanceBuffer instances, int count);

#ifdef __cplusplus
}
#endif

#ifdef MONOLITH_CAPTURE_IMPLEMENTATION
#include "monolith_scene.h"     // MS_WriteR3DEnvironment (link monolith_scene.c)
#include <raymath.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Every list below is a heap array (MS_RESERVE / MS_PUSH of monolith_scene.h) with heap
// strings: no count limits. Resources are only recorded while a capture is pending or
// running (MONOLITH_CAPTURE set), and everything is freed once the package is written.
typedef struct { int vo, io; Vector3 size; } MscCube;                     // a generated cube mesh
typedef struct { Color albedo; float rough, metal; bool unlit; } MscMatState;
typedef struct { const void *meshes; char *path; MscMatState *mats; int matCount; } MscModelFile;   // a loaded model
typedef struct { uint32_t tex; R3D_ProceduralSky p; int size; char *panorama; } MscSky;    // panorama NULL = procedural
typedef struct { const void *shader; char *path; } MscShader;
typedef struct { Color albedo, emission; float rough, metal, emit; bool unlit; Texture2D tex[4]; const void *shader; } MscMatKey;
typedef struct { char *text; int count; } MscNoteEntry;

static struct {
    int state;                  // 0 = not checked, 1 = off, 2 = waiting, 3 = recording, 4 = done
    int frame, target;
    char dir[1024], name[256];
    MscCube *cubes; int cubeCount, cubeCap;
    MscModelFile *files; int fileCount, fileCap;
    MscSky *skies; int skyCount, skyCap;
    MscShader *shaders; int shaderCount, shaderCap;
    char *geo;                  // the frame's geometry lines (written to the map at R3D_End)
    size_t geoSize, geoCap;
    MscMatKey *mats; int matCount, matCap, curMat;
    int boxes, models, lights, approx;
    bool sunTaken;
    R3D_Light sun; R3D_ShadowMap sunMap; bool hasSun;
    char **lightLines; int lightLineCount, lightLineCap;
    Vector3 camPos, camTarget; double fovy; bool hasCam;
    MscNoteEntry *notes; int noteCount, noteCap;
} msc;

static void MscFreeAll(void)
{
    for (int i = 0; i < msc.fileCount; i++) { free(msc.files[i].path); free(msc.files[i].mats); }
    for (int i = 0; i < msc.skyCount; i++) free(msc.skies[i].panorama);
    for (int i = 0; i < msc.shaderCount; i++) free(msc.shaders[i].path);
    for (int i = 0; i < msc.lightLineCount; i++) free(msc.lightLines[i]);
    for (int i = 0; i < msc.noteCount; i++) free(msc.notes[i].text);
    free(msc.cubes); free(msc.files); free(msc.skies); free(msc.shaders); free(msc.geo);
    free(msc.mats); free(msc.lightLines); free(msc.notes);
    msc.cubes = NULL; msc.files = NULL; msc.skies = NULL; msc.shaders = NULL; msc.geo = NULL;
    msc.mats = NULL; msc.lightLines = NULL; msc.notes = NULL;
    msc.cubeCount = msc.cubeCap = msc.fileCount = msc.fileCap = msc.skyCount = msc.skyCap = 0;
    msc.shaderCount = msc.shaderCap = msc.matCount = msc.matCap = msc.lightLineCount = msc.lightLineCap = 0;
    msc.noteCount = msc.noteCap = 0;
    msc.geoSize = msc.geoCap = 0;
}

// "Not translated" lines for CAPTURE.md; the same text is counted, not repeated
static void MscNote(const char *fmt, const char *a, int n)
{
    char text[512];
    snprintf(text, sizeof(text), fmt, a);
    for (int i = 0; i < msc.noteCount; i++)
        if (strcmp(msc.notes[i].text, text) == 0) { msc.notes[i].count += n; return; }
    char *copy = MS_StrDup(text);
    MscNoteEntry *e = copy ? MS_PUSH(msc.notes, msc.noteCount, msc.noteCap) : NULL;
    if (!e) { free(copy); return; }
    e->text = copy;
    e->count = n;
}

// Appends to the frame's geometry text (a growing buffer; plain C, no open_memstream)
static void MscGeo(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    char *line = malloc((size_t)n + 1);
    if (!line) return;
    va_start(ap, fmt);
    vsnprintf(line, (size_t)n + 1, fmt, ap);
    va_end(ap);
    if (msc.geoSize + n + 1 > msc.geoCap) {
        size_t cap = msc.geoCap ? msc.geoCap * 2 : 65536;
        while (cap < msc.geoSize + n + 1) cap *= 2;
        char *g = realloc(msc.geo, cap);
        if (!g) { free(line); return; }
        msc.geo = g;
        msc.geoCap = cap;
    }
    memcpy(msc.geo + msc.geoSize, line, n);
    msc.geoSize += n;
    msc.geo[msc.geoSize] = 0;
    free(line);
}

static bool MscRecording(void)
{
    if (msc.state == 0) {
        const char *d = getenv("MONOLITH_CAPTURE");
        if (!d || !*d) { msc.state = 1; return false; }
        snprintf(msc.dir, sizeof(msc.dir), "%s", d);
        size_t l = strlen(msc.dir);
        while (l > 1 && (msc.dir[l - 1] == '/' || msc.dir[l - 1] == '\\')) msc.dir[--l] = 0;
        snprintf(msc.name, sizeof(msc.name), "%s", GetFileName(msc.dir));
        const char *f = getenv("MONOLITH_CAPTURE_FRAME");
        msc.target = f && atoi(f) > 0 ? atoi(f) : 3;
        msc.state = 2;
    }
    return msc.state == 3;
}

// Resources are recorded only while a capture is pending or running
static bool MscWants(void)
{
    MscRecording();
    return msc.state == 2 || msc.state == 3;
}

// #region Recording resources (always, so the frame can refer to them)
R3D_Model MSC_LoadModel(const char *filePath)
{
    R3D_Model m = R3D_LoadModel(filePath);
    if (!MscWants()) return m;
    char *path = MS_StrDup(filePath);
    MscMatState *mats = m.materialCount > 0 ? malloc(sizeof(MscMatState) * m.materialCount) : NULL;
    MscModelFile *mf = path && (mats || m.materialCount <= 0) ? MS_PUSH(msc.files, msc.fileCount, msc.fileCap) : NULL;
    if (!mf) { free(path); free(mats); return m; }
    mf->meshes = m.meshes;
    mf->path = path;
    mf->mats = mats;
    // the file's own material values, to see later whether the program changed them
    mf->matCount = m.materialCount > 0 ? m.materialCount : 0;
    for (int i = 0; i < mf->matCount; i++)
        mf->mats[i] = (MscMatState){ m.materials[i].albedo.color, m.materials[i].orm.roughness,
                                     m.materials[i].orm.metalness, m.materials[i].unlit };
    return m;
}

R3D_Mesh MSC_GenMeshCube(float width, float height, float length)
{
    R3D_Mesh m = R3D_GenMeshCube(width, height, length);
    MscCube *c = MscWants() ? MS_PUSH(msc.cubes, msc.cubeCount, msc.cubeCap) : NULL;
    if (c) *c = (MscCube){ m.vertexOffset, m.indexOffset, { width, height, length } };
    return m;
}

R3D_Cubemap MSC_GenProceduralSky(int size, R3D_ProceduralSky params)
{
    R3D_Cubemap c = R3D_GenProceduralSky(size, params);
    MscSky *k = MscWants() ? MS_PUSH(msc.skies, msc.skyCount, msc.skyCap) : NULL;
    if (k) *k = (MscSky){ c.texture, params, size, NULL };
    return c;
}

R3D_Cubemap MSC_LoadCubemap(const char *fileName, R3D_CubemapLayout layout)
{
    R3D_Cubemap c = R3D_LoadCubemap(fileName, layout);
    char *path = MscWants() ? MS_StrDup(fileName) : NULL;
    MscSky *k = path ? MS_PUSH(msc.skies, msc.skyCount, msc.skyCap) : NULL;
    if (!k) { free(path); return c; }
    k->tex = c.texture;
    k->size = c.size;
    k->panorama = path;
    return c;
}

R3D_SurfaceShader *MSC_LoadSurfaceShader(const char *filePath)
{
    R3D_SurfaceShader *s = R3D_LoadSurfaceShader(filePath);
    char *path = MscWants() ? MS_StrDup(filePath) : NULL;
    MscShader *k = path ? MS_PUSH(msc.shaders, msc.shaderCount, msc.shaderCap) : NULL;
    if (!k) { free(path); return s; }
    k->shader = s;
    k->path = path;
    return s;
}
// #endregion

// #region Files
static bool MscCopy(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    if (!in) return false;
    MakeDirectory(GetDirectoryPath(to));
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return false; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    return fclose(out) == 0;
}

// Shortest plain decimal (the map format has no exponents) that reads back as the same
// float, or the closest with 9 decimals. A ring of 32 buffers: one printf may use many.
static const char *MscFmt(float v)
{
    static char buf[32][48];
    static int k;
    char *b = buf[k++ & 31];
    for (int d = 0; d <= 9; d++) {
        snprintf(b, 48, "%.*f", d, v);
        if (strtof(b, NULL) == v) break;
    }
    if (strcmp(b, "-0") == 0) snprintf(b, 48, "0");
    return b;
}
// #endregion

// #region Frame
static void MscCamera(Vector3 pos, Vector3 fwd, double fovy)
{
    if (!MscRecording() || msc.hasCam) return;
    msc.camPos = pos;
    msc.camTarget = Vector3Add(pos, Vector3Scale(Vector3Normalize(fwd), 10.0f));
    msc.fovy = fovy;
    msc.hasCam = true;
}

static void MscStartFrame(void)
{
    if (!MscRecording() && msc.state == 2 && ++msc.frame == msc.target) {
        msc.state = 3;
        msc.curMat = -1;
    }
}

void MSC_Begin(Camera3D camera)
{
    MscStartFrame();
    MscCamera(camera.position, Vector3Subtract(camera.target, camera.position), camera.fovy);
    R3D_Begin(camera);
}

void MSC_BeginPro(R3D_View view)
{
    MscStartFrame();
    MscCamera(view.camera.position, Vector3RotateByQuaternion((Vector3){ 0, 0, -1 }, view.camera.rotation), view.camera.fovy);
    R3D_BeginPro(view);
}

static void MscLight(R3D_Light l, bool shadow)
{
    if (!MscRecording()) return;
    if (l.type == R3D_LIGHT_DIR && shadow && !msc.hasSun) { msc.sun = l; msc.hasSun = true; return; }
    char s[512];        // one `light` line: a fixed number of keys, each a short number
    int n = 0;
    if (l.type == R3D_LIGHT_OMNI) n = snprintf(s, sizeof(s), "light point %s %s %s", MscFmt(l.position.x), MscFmt(l.position.y), MscFmt(l.position.z));
    else if (l.type == R3D_LIGHT_SPOT)
        n = snprintf(s, sizeof(s), "light spot %s %s %s %s %s %s inner %s outer %s", MscFmt(l.position.x), MscFmt(l.position.y),
                     MscFmt(l.position.z), MscFmt(l.direction.x), MscFmt(l.direction.y), MscFmt(l.direction.z),
                     MscFmt(l.innerCutOff), MscFmt(l.outerCutOff));
    else n = snprintf(s, sizeof(s), "light dir %s %s %s", MscFmt(l.direction.x), MscFmt(l.direction.y), MscFmt(l.direction.z));
    n += snprintf(s + n, sizeof(s) - n, " color %d %d %d energy %s specular %s fogEnergy %s", l.color.r, l.color.g, l.color.b,
                  MscFmt(l.energy), MscFmt(l.specular), MscFmt(l.fogEnergy));
    if (l.type != R3D_LIGHT_DIR) n += snprintf(s + n, sizeof(s) - n, " range %s falloff %s", MscFmt(l.range), MscFmt(l.falloff));
    snprintf(s + n, sizeof(s) - n, " shadow %s", shadow ? "on" : "off");
    char *copy = MS_StrDup(s);
    char **slot = copy ? MS_PUSH(msc.lightLines, msc.lightLineCount, msc.lightLineCap) : NULL;
    if (slot) *slot = copy;
    else free(copy);
}

void MSC_PushLight(R3D_Light light) { MscLight(light, false); R3D_PushLight(light); }
void MSC_PushLightEx(R3D_Light light, R3D_ShadowMap map, bool updateShadow)
{
    if (MscRecording() && light.type == R3D_LIGHT_DIR && !msc.hasSun) msc.sunMap = map;
    MscLight(light, true);
    R3D_PushLightEx(light, map, updateShadow);
}

// Index of the material (deduplicated by its plain values); writes `use` when it changes
static void MscUse(R3D_Material m)
{
    // Compared bytewise: zero the padding first (an initializer list leaves it indeterminate)
    MscMatKey k;
    memset(&k, 0, sizeof(k));
    k.albedo = m.albedo.color; k.emission = m.emission.color; k.rough = m.orm.roughness; k.metal = m.orm.metalness;
    k.emit = m.emission.energy; k.unlit = m.unlit; k.shader = m.shader;
    k.tex[0] = m.albedo.texture; k.tex[1] = m.normal.texture; k.tex[2] = m.orm.texture; k.tex[3] = m.emission.texture;
    int i = 0;
    while (i < msc.matCount && memcmp(&msc.mats[i], &k, sizeof(k)) != 0) i++;
    if (i == msc.matCount) {
        MscMatKey *slot = MS_PUSH(msc.mats, msc.matCount, msc.matCap);
        if (!slot) { MscNote("materials beyond memory%s: drawn with the last one", "", 1); i = msc.matCount - 1; }
        else memcpy(slot, &k, sizeof(k));
        // textures are read back when the map is written; keep the GL ids for that
        if (i < 0) return;
    }
    if (i != msc.curMat) MscGeo("use \"m%d\"\n", i);
    msc.curMat = i;
}

static void MscMesh(R3D_Mesh mesh, R3D_Material material, Matrix t)
{
    if (!MscRecording()) return;
    int c = 0;
    while (c < msc.cubeCount && (msc.cubes[c].vo != mesh.vertexOffset || msc.cubes[c].io != mesh.indexOffset)) c++;
    if (c == msc.cubeCount) { MscNote("meshes that are not generated cubes (vertices stay on the GPU)%s", "", 1); return; }
    // World AABB of the cube; axis-aligned when every column of the 3x3 has one non-zero entry
    Vector3 h = Vector3Scale(msc.cubes[c].size, 0.5f);
    float m[3][3] = { { t.m0, t.m4, t.m8 }, { t.m1, t.m5, t.m9 }, { t.m2, t.m6, t.m10 } };
    float ext[3];
    bool aligned = true;
    for (int r = 0; r < 3; r++) {
        ext[r] = fabsf(m[r][0]) * h.x + fabsf(m[r][1]) * h.y + fabsf(m[r][2]) * h.z;
        int nz = (fabsf(m[r][0]) > 1e-5f) + (fabsf(m[r][1]) > 1e-5f) + (fabsf(m[r][2]) > 1e-5f);
        if (nz > 1) aligned = false;
    }
    Vector3 ctr = { t.m12, t.m13, t.m14 };
    MscUse(material);
    MscGeo("box %s %s %s ", MscFmt(ctr.x - ext[0]), MscFmt(ctr.y - ext[1]), MscFmt(ctr.z - ext[2]));
    MscGeo("%s %s %s%s\n", MscFmt(ctr.x + ext[0]), MscFmt(ctr.y + ext[1]), MscFmt(ctr.z + ext[2]),
            aligned ? "" : "   # approximation: rotated cube, its world AABB");
    msc.boxes++;
    if (!aligned) { msc.approx++; MscNote("rotated cubes written as their world AABB (approximation)%s", "", 1); }
}

void MSC_DrawMesh(R3D_Mesh mesh, R3D_Material material, Vector3 position, float scale)
{
    MscMesh(mesh, material, MatrixMultiply(MatrixScale(scale, scale, scale), MatrixTranslate(position.x, position.y, position.z)));
    R3D_DrawMesh(mesh, material, position, scale);
}

void MSC_DrawMeshEx(R3D_Mesh mesh, R3D_Material material, Vector3 position, Quaternion rotation, Vector3 scale)
{
    Matrix t = MatrixMultiply(MatrixMultiply(MatrixScale(scale.x, scale.y, scale.z), QuaternionToMatrix(rotation)),
                              MatrixTranslate(position.x, position.y, position.z));
    MscMesh(mesh, material, t);
    R3D_DrawMeshEx(mesh, material, position, rotation, scale);
}

void MSC_DrawMeshPro(R3D_Mesh mesh, R3D_Material material, Matrix transform)
{
    MscMesh(mesh, material, transform);
    R3D_DrawMeshPro(mesh, material, transform);
}

void MSC_DrawMeshInstanced(R3D_Mesh mesh, R3D_Material material, R3D_InstanceBuffer instances, int count)
{
    if (MscRecording()) MscNote("instanced mesh draws (instance data stays on the GPU)%s", "", count);
    R3D_DrawMeshInstanced(mesh, material, instances, count);
}

static void MscModel(R3D_Model model, Matrix t)
{
    if (!MscRecording()) return;
    int f = 0;
    while (f < msc.fileCount && msc.files[f].meshes != model.meshes) f++;
    if (f == msc.fileCount) { MscNote("models not loaded from a file with R3D_LoadModel%s", "", 1); return; }
    // T * R * S: scale = column lengths, R = Ry(yaw) Rx(pitch) Rz(roll) as in the map format
    Vector3 c0 = { t.m0, t.m1, t.m2 }, c1 = { t.m4, t.m5, t.m6 }, c2 = { t.m8, t.m9, t.m10 };
    Vector3 s = { Vector3Length(c0), Vector3Length(c1), Vector3Length(c2) };
    if (s.x == 0 || s.y == 0 || s.z == 0) return;
    float r02 = c2.x / s.z, r12 = c2.y / s.z, r22 = c2.z / s.z, r10 = c0.y / s.x, r11 = c1.y / s.y;
    float pitch = asinf(Clamp(-r12, -1, 1)), yaw = atan2f(r02, r22), roll = atan2f(r10, r11);
    MscGeo("model \"models/%s\" %s %s %s rot %s %s %s scale %s %s %s\n", GetFileName(msc.files[f].path),
            MscFmt(t.m12), MscFmt(t.m13), MscFmt(t.m14), MscFmt(yaw * RAD2DEG), MscFmt(pitch * RAD2DEG), MscFmt(roll * RAD2DEG),
            MscFmt(s.x), MscFmt(s.y), MscFmt(s.z));
    msc.models++;
    msc.curMat = -1;    // a model line doesn't take the box material; keep `use` explicit
    const MscModelFile *mf = &msc.files[f];
    for (int i = 0; i < mf->matCount; i++) {
        const R3D_Material *m = &model.materials[i];
        MscMatState now = { m->albedo.color, m->orm.roughness, m->orm.metalness, m->unlit };
        if (memcmp(&now, &mf->mats[i], sizeof(now)) != 0) {
            MscNote("material values changed in code after loading %s (translate them as material overrides by hand)",
                    GetFileName(mf->path), 1);
            break;
        }
    }
}

void MSC_DrawModel(R3D_Model model, Vector3 position, float scale)
{
    MscModel(model, MatrixMultiply(MatrixScale(scale, scale, scale), MatrixTranslate(position.x, position.y, position.z)));
    R3D_DrawModel(model, position, scale);
}

void MSC_DrawModelEx(R3D_Model model, Vector3 position, Quaternion rotation, Vector3 scale)
{
    MscModel(model, MatrixMultiply(MatrixMultiply(MatrixScale(scale.x, scale.y, scale.z), QuaternionToMatrix(rotation)),
                                   MatrixTranslate(position.x, position.y, position.z)));
    R3D_DrawModelEx(model, position, rotation, scale);
}

void MSC_DrawModelPro(R3D_Model model, Matrix transform)
{
    MscModel(model, transform);
    R3D_DrawModelPro(model, transform);
}

void MSC_DrawModelInstanced(R3D_Model model, R3D_InstanceBuffer instances, int count)
{
    if (MscRecording()) MscNote("instanced model draws (instance data stays on the GPU)%s", "", count);
    R3D_DrawModelInstanced(model, instances, count);
}
// #endregion

// #region Writing the package
static void MscColor(FILE *f, const char *key, Color c) { fprintf(f, " %s %d %d %d", key, c.r, c.g, c.b); }

static void MscTexture(FILE *f, const char *key, Texture2D tex, int mat, int slot)
{
    if (!tex.id) return;
    char rel[64], full[1400];
    snprintf(rel, sizeof(rel), "textures/m%d_%d.png", mat, slot);
    snprintf(full, sizeof(full), "%s/%s", msc.dir, rel);
    Image img = LoadImageFromTexture(tex);
    if (img.data && ExportImage(img, full)) fprintf(f, " %s \"%s\"", key, rel);
    else MscNote("textures that could not be read back%s", "", 1);
    UnloadImage(img);
}

static void MscWrite(void)
{
    FilePathList existing = DirectoryExists(msc.dir) ? LoadDirectoryFiles(msc.dir) : (FilePathList){ 0 };
    bool busy = existing.count > 0;
    if (existing.paths) UnloadDirectoryFiles(existing);
    if (busy) { fprintf(stderr, "monolith_capture: %s exists and is not empty, nothing written\n", msc.dir); return; }
    MakeDirectory(TextFormat("%s/models", msc.dir));
    MakeDirectory(TextFormat("%s/textures", msc.dir));
    MakeDirectory(TextFormat("%s/shaders", msc.dir));

    char path[1400];
    snprintf(path, sizeof(path), "%s/%s.map", msc.dir, msc.name);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "monolith_capture: cannot write %s\n", path); return; }
    fprintf(f, "monolith 2\n# %s.map -- captured from a running R3D program (runtime/monolith_capture.h), frame %d.\n"
               "# What could not be translated is listed in CAPTURE.md.\n\n# --- Materials (m<n>: one per distinct material of the boxes)\n",
            msc.name, msc.target);
    for (int i = 0; i < msc.matCount; i++) {
        MscMatKey *k = &msc.mats[i];
        fprintf(f, "material \"m%d\"", i);
        fprintf(f, " albedo %d %d %d %d roughness %s metalness %s", k->albedo.r, k->albedo.g, k->albedo.b, k->albedo.a,
                MscFmt(k->rough), MscFmt(k->metal));
        if (k->emit > 0) { MscColor(f, "emission", k->emission); fprintf(f, " emission_energy %s", MscFmt(k->emit)); }
        if (k->unlit) fprintf(f, " unlit on");
        static const char *const KEYS[4] = { "albedo_map", "normal_map", "orm_map", "emission_map" };
        for (int t = 0; t < 4; t++) MscTexture(f, KEYS[t], k->tex[t], i, t);    // read back from the GPU
        if (k->shader) {
            int s = 0;
            while (s < msc.shaderCount && msc.shaders[s].shader != k->shader) s++;
            if (s < msc.shaderCount) {
                char rel[300];
                snprintf(rel, sizeof(rel), "shaders/%s", GetFileName(msc.shaders[s].path));
                MscCopy(msc.shaders[s].path, TextFormat("%s/%s", msc.dir, rel));
                fprintf(f, " shader \"%s\"", rel);
                MscNote("surface shader uniform values (set them with `uniform` lines by hand)%s", "", 1);
            } else MscNote("surface shaders not loaded from a file%s", "", 1);
        }
        fprintf(f, "\n");
    }

    fprintf(f, "\n# --- Environment\n");
    R3D_Environment *env = R3D_GetEnvironment();
    int sky = -1;
    for (int i = 0; i < msc.skyCount; i++) if (env->background.sky.texture && msc.skies[i].tex == env->background.sky.texture) sky = i;
    if (sky >= 0 && !msc.skies[sky].panorama) {
        R3D_ProceduralSky *p = &msc.skies[sky].p;
        fprintf(f, "env sky.mode procedural\nenv sky.size %d\n", msc.skies[sky].size);
        fprintf(f, "env sky.skyTopColor %d %d %d\nenv sky.skyHorizonColor %d %d %d\nenv sky.skyHorizonCurve %s\nenv sky.skyEnergy %s\n",
                p->skyTopColor.r, p->skyTopColor.g, p->skyTopColor.b, p->skyHorizonColor.r, p->skyHorizonColor.g,
                p->skyHorizonColor.b, MscFmt(p->skyHorizonCurve), MscFmt(p->skyEnergy));
        fprintf(f, "env sky.groundBottomColor %d %d %d\nenv sky.groundHorizonColor %d %d %d\nenv sky.groundHorizonCurve %s\nenv sky.groundEnergy %s\n",
                p->groundBottomColor.r, p->groundBottomColor.g, p->groundBottomColor.b, p->groundHorizonColor.r,
                p->groundHorizonColor.g, p->groundHorizonColor.b, MscFmt(p->groundHorizonCurve), MscFmt(p->groundEnergy));
        fprintf(f, "env sky.sunDirection %s %s %s\nenv sky.sunColor %d %d %d\nenv sky.sunSizeDeg %s\nenv sky.sunCurve %s\nenv sky.sunEnergy %s\n",
                MscFmt(p->sunDirection.x), MscFmt(p->sunDirection.y), MscFmt(p->sunDirection.z), p->sunColor.r, p->sunColor.g,
                p->sunColor.b, MscFmt(p->sunSize * RAD2DEG), MscFmt(p->sunCurve), MscFmt(p->sunEnergy));
    } else if (sky >= 0) {
        char rel[300];
        snprintf(rel, sizeof(rel), "textures/%s", GetFileName(msc.skies[sky].panorama));
        MscCopy(msc.skies[sky].panorama, TextFormat("%s/%s", msc.dir, rel));
        fprintf(f, "env sky.mode panorama\nenv sky.panorama \"%s\"\n", rel);
    } else {
        fprintf(f, "env sky.mode color\n");
        if (env->background.sky.texture) MscNote("a sky cubemap not made by R3D_GenProceduralSky / R3D_LoadCubemap%s", "", 1);
    }
    // The ambient map: assumed to come from the sky (R3D_GenAmbientMap(sky)); none = off
    fprintf(f, "env ambientSky.mode %s\n", env->ambient.map.irradiance ? "same" : "off");
    if (msc.hasSun) {
        R3D_Light *s = &msc.sun;
        fprintf(f, "env sun.enabled on\nenv sun.shadow on\nenv sun.direction %s %s %s\nenv sun.color %d %d %d\n",
                MscFmt(s->direction.x), MscFmt(s->direction.y), MscFmt(s->direction.z), s->color.r, s->color.g, s->color.b);
        fprintf(f, "env sun.energy %s\nenv sun.specular %s\nenv sun.fogEnergy %s\nenv sun.range %s\nenv sun.softness %s\n",
                MscFmt(s->energy), MscFmt(s->specular), MscFmt(s->fogEnergy), MscFmt(s->range > 0 ? s->range : 100),
                MscFmt(msc.sunMap.softness));
        fprintf(f, "env sun.depthBias %s\nenv sun.slopeBias %s\n", MscFmt(msc.sunMap.depthBias), MscFmt(msc.sunMap.slopeBias));
    } else fprintf(f, "env sun.enabled off\n");
    MS_WriteR3DEnvironment(env, f);

    fprintf(f, "\n# --- Camera of the captured frame (monolith's viewpoints use fovy 70)\n");
    if (msc.hasCam) {
        Vector3 d = Vector3Subtract(msc.camTarget, msc.camPos);
        float yaw = atan2f(d.x, d.z) * RAD2DEG;
        fprintf(f, "spawn %s %s %s %s\n", MscFmt(msc.camPos.x), MscFmt(msc.camPos.y - 1.62f), MscFmt(msc.camPos.z), MscFmt(yaw));
        fprintf(f, "viewpoint \"Captured\" %s %s %s %s %s %s\n", MscFmt(msc.camPos.x), MscFmt(msc.camPos.y), MscFmt(msc.camPos.z),
                MscFmt(msc.camTarget.x), MscFmt(msc.camTarget.y), MscFmt(msc.camTarget.z));
        if (fabs(msc.fovy - 70.0) > 0.01) MscNote("camera field of view %s (monolith viewpoints use 70 degrees)", TextFormat("%.1f", msc.fovy), 1);
    }

    fprintf(f, "\n# --- Lights\n");
    for (int i = 0; i < msc.lightLineCount; i++) fprintf(f, "%s\n", msc.lightLines[i]);
    fprintf(f, "\n# --- Geometry, in draw order\ngroup \"captured\"\n");
    if (msc.geo) fwrite(msc.geo, 1, msc.geoSize, f);
    fclose(f);

    // Model files (as loaded; their sidecar and licence are for the import session to fill in)
    for (int i = 0; i < msc.fileCount; i++)
        if (!MscCopy(msc.files[i].path, TextFormat("%s/models/%s", msc.dir, GetFileName(msc.files[i].path))))
            MscNote("model files that could not be copied: %s", GetFileName(msc.files[i].path), 1);

    snprintf(path, sizeof(path), "%s/MAP.md", msc.dir);
    if ((f = fopen(path, "wb"))) {
        fprintf(f, "# %s: design brief\n\nCaptured from a running R3D program. Write the brief from the program's intent: style and mood, "
                   "scale, materials and palette, lighting, rules for new content, groups and landmarks.\n", msc.name);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/ASSETS.md", msc.dir);
    if ((f = fopen(path, "wb"))) {
        fprintf(f, "# %s: external assets\n\nFill in one row per file under models/ (and textures/ that came from elsewhere): "
                   "source URL, author, licence, date, sha256, changes.\n\n"
                   "| File | Source URL | Author | Licence (+ URL) | Downloaded | sha256 of the original | Changes |\n|---|---|---|---|---|---|---|\n", msc.name);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/CAPTURE.md", msc.dir);
    if ((f = fopen(path, "wb"))) {
        fprintf(f, "# Capture report: %s\n\nFrame %d. Translated: %d boxes (%d approximated), %d model instances of %d files, "
                   "%d materials, %d lights + %s sun, the environment.\n\n## Not translated (count, what)\n\n",
                msc.name, msc.target, msc.boxes, msc.approx, msc.models, msc.fileCount, msc.matCount, msc.lightLineCount,
                msc.hasSun ? "a" : "no");
        for (int i = 0; i < msc.noteCount; i++) fprintf(f, "- %d x %s\n", msc.notes[i].count, msc.notes[i].text);
        if (!msc.noteCount) fprintf(f, "- nothing seen in R3D's draw calls\n");
        fprintf(f, "\nNever seen by a capture: gameplay, input, animation state, custom render passes outside R3D, "
                   "and anything the program draws in other frames.\n");
        fclose(f);
    }
    printf("monolith_capture: wrote %s/ (%d boxes, %d models, %d lights; see CAPTURE.md)\n", msc.dir, msc.boxes, msc.models,
           msc.lightLineCount + msc.hasSun);
}

void MSC_End(void)
{
    R3D_End();
    if (MscRecording()) { MscWrite(); msc.state = 4; MscFreeAll(); }
}
// #endregion

#endif // MONOLITH_CAPTURE_IMPLEMENTATION

// The redirects (after the implementation, which calls the real functions)
#ifndef MONOLITH_CAPTURE_NO_MACROS
#define R3D_LoadModel           MSC_LoadModel
#define R3D_GenMeshCube         MSC_GenMeshCube
#define R3D_GenProceduralSky    MSC_GenProceduralSky
#define R3D_LoadCubemap         MSC_LoadCubemap
#define R3D_LoadSurfaceShader   MSC_LoadSurfaceShader
#define R3D_Begin               MSC_Begin
#define R3D_BeginPro            MSC_BeginPro
#define R3D_End                 MSC_End
#define R3D_PushLight           MSC_PushLight
#define R3D_PushLightEx         MSC_PushLightEx
#define R3D_DrawMesh            MSC_DrawMesh
#define R3D_DrawMeshEx          MSC_DrawMeshEx
#define R3D_DrawMeshPro         MSC_DrawMeshPro
#define R3D_DrawMeshInstanced   MSC_DrawMeshInstanced
#define R3D_DrawModel           MSC_DrawModel
#define R3D_DrawModelEx         MSC_DrawModelEx
#define R3D_DrawModelPro        MSC_DrawModelPro
#define R3D_DrawModelInstanced  MSC_DrawModelInstanced
#endif

#endif // MONOLITH_CAPTURE_H
