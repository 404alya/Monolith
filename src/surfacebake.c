// surfacebake -- the draw mode's shader bake (docs/painting.md 3.6)
//
// The first stroke on a box whose material has a surface shader gives it a copy without the
// shader; this file renders what the shader shows into the copy's maps. The box is drawn alone,
// the way the scene draws it (MS_BoxDrawOf: an instanced material's own instance buffer and the
// box's slot in it, so INSTANCE_* and the model-space POSITION are the scene's; a merged material's
// unit cube placed where its merged faces lie), with the material's own shader alias, uniforms and
// samplers. Per face, an orthographic camera looks at it from outside along its normal (a convex
// box: the other faces are edge-on) and four R3D frames go into a small colour target:
//   R3D_OUTPUT_ALBEDO over a black backdrop, then over a white one: a texel that shows the backdrop
//     in both (exactly 0 0 0, then exactly 255 255 255) is a hole the shader discarded;
//   R3D_OUTPUT_ORM: occlusion, roughness, metalness as written (8-bit linear, as the G-buffer has them);
//   R3D_OUTPUT_NORMAL: the world normal, turned into the face's tangent space (the unit cube's
//     tangent, bitangent and normal of that face, turned with a turned box), kept only when some
//     covered texel differs from the face's own normal.
// The backdrop is a thin slab just behind the face plane, inside the box, so the rest of the box
// (seen through a hole of a `cull none` material) lies behind it. Nothing is lit: the G-buffer
// holds the bytes the shader wrote (the albedo is linear and goes into the canvas as sRGB).
//
// Sampling: the target has R3D's internal resolution (the game's render size), so its pixels are
// the G-buffer's texels one to one and a pixel is covered when its centre is. The camera is laid
// out so the pixels are square in the world, a third of a canvas texel along the face's finer axis
// where the face fits (else as small as the internal resolution allows), and the face's u = 0 and
// v = 0 edges lie on pixel edges. Each canvas texel takes the pixel at its centre (point sampling:
// a pixel-art shader's edges stay crisp; the finer pixels give the shader the derivatives of a close
// view, so its texture LOD and pixel-size levels are its finest). Gutter
// texels (docs/painting.md 4.1) copy the nearest texel inside the face.
//
// The frame: R3D's environment is replaced by a plain one (no sky, ambient map, screen-space effects,
// fog, bloom or auto exposure) and copied back byte for byte; the output mode and the depth-output
// flag are restored (the model picker's thumbnail pattern, docs/live-editing.md "Phase 3b"). No
// light is pushed, so no shadow job runs. R3D's frame counter (FRAME_INDEX) advances by one per
// frame rendered here.
#include "surfacebake.h"
#include <math.h>

#define PASSES 4                // albedo over black, albedo over white, ORM, normal
#define SUPERSAMPLE 3           // target pixels per texel along the face's finer axis (odd: a texel centre is a pixel centre)

// A colour-only RGBA8 target (R3D writes its own depth internally; R3D_SetOutputDepth(false))
static RenderTexture2D ColorTarget(int w, int h)
{
    RenderTexture2D t = { 0 };
    t.id = rlLoadFramebuffer();
    if (!t.id) return t;
    rlEnableFramebuffer(t.id);
    t.texture.id = rlLoadTexture(NULL, w, h, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
    t.texture.width = w;
    t.texture.height = h;
    t.texture.mipmaps = 1;
    t.texture.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    rlFramebufferAttach(t.id, t.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    bool ok = t.texture.id && rlFramebufferComplete(t.id);
    rlDisableFramebuffer();
    if (!ok) {
        if (t.texture.id) rlUnloadTexture(t.texture.id);
        rlUnloadFramebuffer(t.id);
        return (RenderTexture2D){ 0 };
    }
    return t;
}

static void FreeTarget(RenderTexture2D *t)
{
    if (t->texture.id) rlUnloadTexture(t->texture.id);
    if (t->id) rlUnloadFramebuffer(t->id);
    *t = (RenderTexture2D){ 0 };
}

// The face of a cube normal: 0..5 = +x -x +y -y +z -z
static int FaceOf(Vector3 n)
{
    float ax = fabsf(n.x), ay = fabsf(n.y), az = fabsf(n.z);
    if (ax >= ay && ax >= az) return n.x > 0.0f ? 0 : 1;
    if (ay >= az) return n.y > 0.0f ? 2 : 3;
    return n.z > 0.0f ? 4 : 5;
}

// One face of the box in the world: the point at face uv (0, 0), the steps per unit of u and v,
// its normal, tangent and bitangent (the unit cube's, as R3D's vertex stage builds them)
typedef struct {
    Vector3 origin, eu, ev;
    Vector3 n, t, b;
} FaceFrame;

// The box's frame: centre, size along its axes, the axes (world)
typedef struct {
    Vector3 c;
    float s[3];
    Vector3 ax[3];
} BoxFrame;

static Vector3 BoxPoint(const BoxFrame *B, Vector3 v)
{
    Vector3 p = B->c;
    float l[3] = { v.x, v.y, v.z };
    for (int j = 0; j < 3; j++) p = Vector3Add(p, Vector3Scale(B->ax[j], B->s[j] * l[j]));
    return p;
}

static Vector3 BoxDir(const BoxFrame *B, Vector3 v, bool scaled)
{
    Vector3 p = { 0 };
    float l[3] = { v.x, v.y, v.z };
    for (int j = 0; j < 3; j++) p = Vector3Add(p, Vector3Scale(B->ax[j], (scaled ? B->s[j] : 1.0f) * l[j]));
    return p;
}

// Face f's frame from R3D's unit cube data (its four vertices: positions, UVs, normal, tangent)
static bool FaceFrameOf(const R3D_MeshData *cube, const BoxFrame *B, int f, FaceFrame *F)
{
    int idx[4], n = 0;
    for (int i = 0; i < cube->vertexCount && n < 4; i++)
        if (FaceOf(R3D_UnpackNormal(cube->vertices[i].normal)) == f) idx[n++] = i;
    if (n < 3) return false;
    Vector3 p[3];
    Vector2 uv[3];
    for (int k = 0; k < 3; k++) {
        const R3D_Vertex *v = &cube->vertices[idx[k]];
        p[k] = v->position;
        uv[k] = (Vector2){ R3D_UnpackFloat16(v->texcoord[0]), R3D_UnpackFloat16(v->texcoord[1]) };
    }
    float a = uv[1].x - uv[0].x, b = uv[1].y - uv[0].y, c = uv[2].x - uv[0].x, d = uv[2].y - uv[0].y;
    float det = a * d - b * c;
    if (fabsf(det) < 1e-6f) return false;
    Vector3 d1 = Vector3Subtract(p[1], p[0]), d2 = Vector3Subtract(p[2], p[0]);
    Vector3 eu = Vector3Scale(Vector3Subtract(Vector3Scale(d1, d), Vector3Scale(d2, b)), 1.0f / det);
    Vector3 ev = Vector3Scale(Vector3Subtract(Vector3Scale(d2, a), Vector3Scale(d1, c)), 1.0f / det);
    Vector3 o = Vector3Subtract(Vector3Subtract(p[0], Vector3Scale(eu, uv[0].x)), Vector3Scale(ev, uv[0].y));
    const R3D_Vertex *v0 = &cube->vertices[idx[0]];
    Vector4 tg = R3D_UnpackTangent(v0->tangent);
    F->origin = BoxPoint(B, o);
    F->eu = BoxDir(B, eu, true);
    F->ev = BoxDir(B, ev, true);
    F->n = Vector3Normalize(BoxDir(B, R3D_UnpackNormal(v0->normal), false));
    F->t = Vector3Normalize(BoxDir(B, (Vector3){ tg.x, tg.y, tg.z }, false));
    F->b = Vector3Normalize(Vector3Scale(Vector3CrossProduct(F->n, F->t), tg.w < 0.0f ? -1.0f : 1.0f));
    return true;
}

// Linear 8-bit -> sRGB 8-bit
static void SrgbTable(unsigned char lut[256])
{
    for (int i = 0; i < 256; i++) {
        float l = i / 255.0f;
        float s = l <= 0.0031308f ? 12.92f * l : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
        int v = (int)lroundf(s * 255.0f);
        lut[i] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

static void Draw(const MS_BoxDraw *bd, R3D_Material backdrop, Matrix slab)
{
    R3D_DrawMeshPro(bd->cube, backdrop, slab);
    if (bd->instanced) R3D_DrawMeshInstancedEx(bd->cube, bd->material, bd->instances, bd->index, 1);
    else R3D_DrawMeshPro(bd->cube, bd->material, bd->transform);
}

// A unit cube placed as a slab: centre c, axes r, u, n with lengths sr, su, sn
static Matrix Slab(Vector3 c, Vector3 r, Vector3 u, Vector3 n, float sr, float su, float sn)
{
    Matrix M = { 0 };
    M.m0 = r.x * sr; M.m1 = r.y * sr; M.m2 = r.z * sr;
    M.m4 = u.x * su; M.m5 = u.y * su; M.m6 = u.z * su;
    M.m8 = n.x * sn; M.m9 = n.y * sn; M.m10 = n.z * sn;
    M.m12 = c.x; M.m13 = c.y; M.m14 = c.z; M.m15 = 1.0f;
    return M;
}

// Uncovered texels take the colour of covered neighbours (a few rings), so filtering at a hole's
// edge doesn't pull in black; their alpha stays 0
static void Dilate(Image *img)
{
    int w = img->width, h = img->height;
    unsigned char *px = img->data;
    unsigned char *done = malloc((size_t)w * h);
    if (!done) return;
    for (int i = 0; i < w * h; i++) done[i] = px[4 * i + 3] != 0;
    for (int ring = 0; ring < 4; ring++) {
        bool any = false;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int i = y * w + x;
                if (done[i]) continue;
                int sum[3] = { 0 }, k = 0;
                const int NB[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
                for (int q = 0; q < 4; q++) {
                    int nx = x + NB[q][0], ny = y + NB[q][1];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h || done[ny * w + nx] != 1) continue;
                    for (int c = 0; c < 3; c++) sum[c] += px[4 * (ny * w + nx) + c];
                    k++;
                }
                if (!k) continue;
                for (int c = 0; c < 3; c++) px[4 * i + c] = (unsigned char)((sum[c] + k / 2) / k);
                done[i] = 2;       // (filled this ring: a source from the next one on)
                any = true;
            }
        for (int i = 0; i < w * h; i++) if (done[i] == 2) done[i] = 1;
        if (!any) break;
    }
    free(done);
}

void SurfaceBakeFree(SurfaceBake *b)
{
    if (b->albedo.data) UnloadImage(b->albedo);
    if (b->orm.data) UnloadImage(b->orm);
    if (b->normal.data) UnloadImage(b->normal);
    *b = (SurfaceBake){ 0 };
}

bool SurfaceBakeBox(const MS_Scene *s, int box, const unsigned char cells[6], int W, int H, SurfaceBake *out,
                    char *why, int whySize)
{
    *out = (SurfaceBake){ 0 };
    double t0 = GetTime();
    MS_BoxDraw bd;
    if (!MS_BoxDrawOf(s, box, &bd)) {
        snprintf(why, (size_t)whySize, "the box can't be drawn alone (no GPU state or draw order)");
        return false;
    }
    BoxFrame B = { .ax = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
    if (s->boxTurned && s->boxTurned[box] >= 0) {
        const MS_TurnedBox *t = &s->turnedBoxes[s->boxTurned[box]];
        B.c = t->center;
        B.s[0] = t->size.x; B.s[1] = t->size.y; B.s[2] = t->size.z;
        memcpy(B.ax, t->axis, sizeof(B.ax));
    } else {
        const BoundingBox *b = &s->boxes[box];
        B.c = Vector3Scale(Vector3Add(b->min, b->max), 0.5f);
        B.s[0] = b->max.x - b->min.x; B.s[1] = b->max.y - b->min.y; B.s[2] = b->max.z - b->min.z;
    }
    R3D_MeshData cube = R3D_GenMeshDataCube(1.0f, 1.0f, 1.0f);
    out->albedo = GenImageColor(W, H, (Color){ 0, 0, 0, 0 });
    out->orm = GenImageColor(W, H, (Color){ 255, 255, 0, 255 });
    out->normal = GenImageColor(W, H, (Color){ 128, 128, 255, 255 });
    if (!cube.vertices || cube.vertexCount != 24 || !out->albedo.data || !out->orm.data || !out->normal.data) {
        R3D_UnloadMeshData(cube);
        SurfaceBakeFree(out);
        snprintf(why, (size_t)whySize, "out of memory");
        return false;
    }
    // One colour target of R3D's internal resolution for every face and view
    int tw = 0, th = 0;
    R3D_GetResolution(&tw, &th);
    RenderTexture2D rt = tw >= 8 && th >= 8 ? ColorTarget(tw, th) : (RenderTexture2D){ 0 };
    if (!rt.id) {
        R3D_UnloadMeshData(cube);
        SurfaceBakeFree(out);
        snprintf(why, (size_t)whySize, "no render target of %d x %d", tw, th);
        return false;
    }
    unsigned char lut[256];
    SrgbTable(lut);
    R3D_Material backdrop[2] = { R3D_GetDefaultMaterial(), R3D_GetDefaultMaterial() };
    backdrop[0].albedo.color = (Color){ 0, 0, 0, 255 };
    backdrop[1].albedo.color = (Color){ 255, 255, 255, 255 };
    for (int k = 0; k < 2; k++) {
        backdrop[k].emission.energy = 0.0f;
        backdrop[k].alphaCutoff = 0.0f;
        backdrop[k].cullMode = R3D_CULL_NONE;
    }

    // Swap a plain environment in (every byte of R3D's kept) and the output state
    R3D_Environment *env = R3D_GetEnvironment();
    R3D_Environment saved;
    memcpy(&saved, env, sizeof(saved));
    R3D_OutputMode mode = R3D_GetOutputMode();
    bool depth = R3D_GetOutputDepth();
    R3D_Environment plain = R3D_ENVIRONMENT_BASE;
    R3D_SetEnvironment(&plain);
    R3D_SetOutputDepth(false);

    static const R3D_OutputMode MODES[PASSES] = { R3D_OUTPUT_ALBEDO, R3D_OUTPUT_ALBEDO, R3D_OUTPUT_ORM, R3D_OUTPUT_NORMAL };
    unsigned char *A = out->albedo.data, *O = out->orm.data, *N = out->normal.data;
    bool ok = true, normalChanged = false;
    for (int f = 0; f < 6 && ok; f++) {
        FaceFrame F;
        if (!FaceFrameOf(&cube, &B, f, &F)) { ok = false; snprintf(why, (size_t)whySize, "R3D's cube has no face %d", f); break; }
        // The face's cell (texels) and where the face's 0..1 lies in it (the gutter inset)
        Rectangle cr = MS_FaceCellRect(cells, f, false), ir = MS_FaceCellRect(cells, f, true);
        int x0 = (int)lroundf(cr.x * W), y0 = (int)lroundf(cr.y * H);
        int x1 = (int)lroundf((cr.x + cr.width) * W), y1 = (int)lroundf((cr.y + cr.height) * H);
        float Lu = Vector3Length(F.eu), Lv = Vector3Length(F.ev);
        float thick = B.s[f / 2];
        if (x1 <= x0 || y1 <= y0) continue;
        if (!(Lu > 1e-6f) || !(Lv > 1e-6f)) {
            // (a face of no area: nothing shows it; opaque, flat)
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) A[4 * ((size_t)y * W + x) + 3] = 255;
            continue;
        }
        // The target is R3D's internal resolution, so its pixels are the G-buffer's (the visualizer
        // copies them 1:1) and coverage is decided at their centres. Pixels are square, a third of a
        // texel along the face's finer axis where the face fits (else as small as it allows: a face
        // of more texels than the target has pixels is sampled at the target's resolution), and the
        // face's u = 0 and v = 0 edges lie on pixel edges one pixel in from the target's
        float du = Lu / (ir.width * W), dv = Lv / (ir.height * H), p = fminf(du, dv) / SUPERSAMPLE;
        p = fmaxf(p, fmaxf(Lu / (float)(tw - 2), Lv / (float)(th - 2)));
        Vector3 udir = Vector3Scale(F.eu, 1.0f / Lu), up = Vector3Scale(F.ev, 1.0f / Lv);
        int lastU = (int)ceilf(Lu / p + 0.5f) - 1, lastV = (int)ceilf(Lv / p + 0.5f) - 1;   // last pixel inside
        if (lastU > tw - 2) lastU = tw - 2;
        if (lastV > th - 2) lastV = th - 2;
        Vector3 fc = Vector3Add(F.origin, Vector3Add(Vector3Scale(udir, 0.5f * tw * p - p), Vector3Scale(up, 0.5f * th * p - p)));
        Vector3 right = Vector3Normalize(Vector3CrossProduct(Vector3Negate(F.n), up));
        Camera3D cam = { .position = Vector3Add(fc, F.n), .target = fc, .up = up, .fovy = th * p, .projection = CAMERA_ORTHOGRAPHIC };
        R3D_Camera rc = R3D_CameraFromRL(cam);
        float delta = fminf(0.001f, 0.25f * fabsf(thick));
        if (!(delta > 1e-6f)) delta = 1e-6f;
        rc.nearPlane = 0.5;
        rc.farPlane = 2.5 + delta;
        Matrix slab = Slab(Vector3Subtract(fc, Vector3Scale(F.n, delta + 0.0005f)), right, up, F.n, 2.0f * tw * p, 2.0f * th * p, 0.001f);
        Image img[PASSES] = { 0 };
        for (int k = 0; k < PASSES && ok; k++) {
            R3D_SetOutputMode(MODES[k]);
            R3D_BeginPro((R3D_View){ .camera = rc, .target = rt });
            Draw(&bd, backdrop[k == 1], slab);
            R3D_End();
            img[k] = LoadImageFromTexture(rt.texture);      // (GL order: row 0 = the bottom of the view)
            if (img[k].data && img[k].format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) ImageFormat(&img[k], PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            if (!img[k].data || img[k].width != tw || img[k].height != th) { ok = false; snprintf(why, (size_t)whySize, "cannot read the bake back"); }
        }
        if (ok) {
            // The texels inside the face (their centres at face uv 0..1); the gutter takes the nearest
            int xi0 = x1, xi1 = x0 - 1, yi0 = y1, yi1 = y0 - 1;
            for (int x = x0; x < x1; x++) {
                float u = ((x + 0.5f) / W - ir.x) / ir.width;
                if (u >= 0.0f && u <= 1.0f) { if (x < xi0) xi0 = x; xi1 = x; }
            }
            for (int y = y0; y < y1; y++) {
                float v = ((y + 0.5f) / H - ir.y) / ir.height;
                if (v >= 0.0f && v <= 1.0f) { if (y < yi0) yi0 = y; yi1 = y; }
            }
            if (xi1 < xi0) xi0 = xi1 = (x0 + x1) / 2;
            if (yi1 < yi0) yi0 = yi1 = (y0 + y1) / 2;
            bool flipU = Vector3DotProduct(right, udir) < 0.0f;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    // The pixel at the texel's centre (a gutter texel: the nearest texel inside's), counted
                    // from the face's u = 0 / v = 0 side. (Point sampling keeps a pixel-art shader's edges
                    // crisp; a box filter of the texel's pixels was tried and softened them.) The pixels
                    // are finer than the texel, so the shader's derivatives (texture LOD, distance levels
                    // by pixel size) are those of a close view
                    int cx = x < xi0 ? xi0 : x > xi1 ? xi1 : x, cy = y < yi0 ? yi0 : y > yi1 ? yi1 : y;
                    float u = ((cx + 0.5f) / W - ir.x) / ir.width, v = ((cy + 0.5f) / H - ir.y) / ir.height;
                    int iu = (int)floorf(u * Lu / p) + 1, iv = (int)floorf(v * Lv / p) + 1;
                    iu = iu < 1 ? 1 : iu > lastU ? lastU : iu;
                    iv = iv < 1 ? 1 : iv > lastV ? lastV : iv;
                    int ix = flipU ? tw - 1 - iu : iu, iy = iv;     // (screen up is +v, GL row 0 the bottom)
                    size_t si = 4 * ((size_t)iy * tw + ix), di = 4 * ((size_t)y * W + x);
                    const unsigned char *a0 = (unsigned char *)img[0].data + si, *a1 = (unsigned char *)img[1].data + si;
                    const unsigned char *o = (unsigned char *)img[2].data + si, *nb = (unsigned char *)img[3].data + si;
                    // (a pixel showing the backdrop in both albedo views is a hole)
                    bool hole = a0[0] == 0 && a0[1] == 0 && a0[2] == 0 && a1[0] == 255 && a1[1] == 255 && a1[2] == 255;
                    for (int c = 0; c < 3; c++) {
                        A[di + c] = hole ? 0 : lut[a0[c]];
                        O[di + c] = o[c];
                    }
                    A[di + 3] = hole ? 0 : 255;
                    O[di + 3] = 255;
                    out->holes |= hole;
                    Vector3 tn = { 0, 0, 1 };
                    if (!hole) {
                        Vector3 wn = Vector3Normalize((Vector3){ nb[0] / 127.5f - 1.0f, nb[1] / 127.5f - 1.0f, nb[2] / 127.5f - 1.0f });
                        if (Vector3DotProduct(wn, F.n) < 0.999f) normalChanged = true;
                        tn = Vector3Normalize((Vector3){ Vector3DotProduct(wn, F.t), Vector3DotProduct(wn, F.b), Vector3DotProduct(wn, F.n) });
                    }
                    float tc[3] = { tn.x, tn.y, tn.z };
                    for (int c = 0; c < 3; c++) {
                        int q = (int)lroundf((tc[c] * 0.5f + 0.5f) * 255.0f);
                        N[di + c] = (unsigned char)(q < 0 ? 0 : q > 255 ? 255 : q);
                    }
                    N[di + 3] = 255;
                }
        }
        for (int k = 0; k < PASSES; k++) if (img[k].data) UnloadImage(img[k]);
    }

    // Everything back as it was
    FreeTarget(&rt);
    memcpy(env, &saved, sizeof(saved));
    R3D_SetOutputMode(mode);
    R3D_SetOutputDepth(depth);
    R3D_UnloadMeshData(cube);
    if (!ok) {
        SurfaceBakeFree(out);
        return false;
    }
    if (!normalChanged) {
        UnloadImage(out->normal);
        out->normal = (Image){ 0 };
    }
    if (out->holes) Dilate(&out->albedo);
    out->ms = (GetTime() - t0) * 1000.0;
    return true;
}
