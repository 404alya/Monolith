// --bake-surface: bakes a material's surface shader into a tile texture.
//
// A procedural world-space material (a surface shader that computes its look from POSITION)
// costs its full noise work for every pixel it covers. Most of that work doesn't depend on
// where the surface is, only on its position within the surface plane, so it can be computed
// once into a tile that a cheaper shader fetches. This renders the material onto one square
// quad with an orthographic camera, through R3D's own G-buffer (R3D_OUTPUT_ALBEDO and
// R3D_OUTPUT_ORM, both 8-bit linear targets, shown 1:1 on a target of the same size), so the
// tile comes from the same GLSL as the live material and can be made again whenever the
// shader changes. Nothing is lit: the bytes are what the shader wrote.
//
// The quad covers [0, P) x [0, P) metres, P = size / texels (the tile's period):
//   top   face: y = 0, normal +y; tile u = world x, v = world z
//   side  face: z = 0, normal +z; tile u = world x, v = world y
// Texel (i, j) of the PNG (row j from the top of the file, i.e. texelFetch's ivec2(i, j)
// once the PNG is a texture) is the pixel whose centre is at u = (i + 0.5) / texels,
// v = (j + 0.5) / texels.
//
// Output <prefix>.png, RGBA 8-bit: RGB = the shader's ALBEDO, A = its ROUGHNESS. A material
// that bakes data rather than a colour writes its channels there (the map loads such a tile
// with `data_sampler`, which skips the sRGB decode).
//
// The shader is compiled with these lines in front of its source (MS_LoadOptions.shaderDefines):
//   #define MS_BAKE 1
//   #define MS_BAKE_FACE 0 | 1              top | side
//   #define MS_BAKE_LEVEL n                 the level being baked (0 = the finest)
//   #define MS_BAKE_PERIOD p                the tile's period in metres (size / texels)
//   #define MS_BAKE_TEXELS t                level-0 texels per metre
// so it can force its texel grid to level n (instead of choosing one by distance), make its
// noise periodic over MS_BAKE_PERIOD (or the tile shows a seam) and leave out what it
// computes at run time.
//
// Levels (--bake-levels n > 1): level L is the tile baked with MS_BAKE_LEVEL L at
// size / 2^L px (texels / 2^L per metre, the same period). All levels share one atlas,
// (size + size / 2) x size px: level 0 at (0, 0), level L >= 1 at x = size,
// y = size - size / 2^(L-1) (level 1 at the top right, each next one below the last).
//
// Only the material's own shader file gets the defines (others may sit at R3D's 16 KB limit).
// Every level reloads the map with that level's defines (the whole package: a shader that
// fails to compile fails the bake with R3D's log).

#include "bake.h"
#include "png.h"

// One level: an R3D render of the quad into a size x size target, twice (albedo, ORM);
// returns the RGBA tile (top-down rows as described above) or an empty image
static Image BakeLevel(const BakeArgs *a, int level, char *err, int errSize)
{
    int px = a->size >> level;
    float period = (float)a->size / a->texels;
    char defines[512];
    snprintf(defines, sizeof(defines),
             "#define MS_BAKE 1\n#define MS_BAKE_FACE %d\n#define MS_BAKE_LEVEL %d\n#define MS_BAKE_PERIOD %.9g\n#define MS_BAKE_TEXELS %.9g\n",
             (int)a->face, level, period, a->texels);
    MS_LoadOptions opt = { .shaderDefines = a->shaderFile ? defines : NULL, .shaderDefinesFile = a->shaderFile };
    MS_Scene *s = MS_Load(a->mapPath, &opt, err, errSize);
    if (!s) return (Image){ 0 };
    const MS_Material *mat = NULL;
    for (int i = 0; i < s->materialCount; i++)
        if (strcmp(s->materials[i].name, a->material) == 0) mat = &s->materials[i];
    if (!mat) {
        snprintf(err, errSize, "%s: no material \"%s\"", s->path, a->material);
        MS_Unload(s);
        return (Image){ 0 };
    }

    bool top = a->face == BAKE_FACE_TOP;
    R3D_Mesh quad = R3D_GenMeshQuad(period, period, 1, 1, top ? (Vector3){ 0, 1, 0 } : (Vector3){ 0, 0, 1 });
    Vector3 centre = top ? (Vector3){ period / 2, 0, period / 2 } : (Vector3){ period / 2, period / 2, 0 };
    // Looking down -y with up = -z keeps +x to the right (so the rows run along -z: flipped
    // below); looking down -z with up = +y has +x right and +y up
    Camera3D cam = {
        .position = Vector3Add(centre, top ? (Vector3){ 0, 10, 0 } : (Vector3){ 0, 0, 10 }),
        .target = centre,
        .up = top ? (Vector3){ 0, 0, -1 } : (Vector3){ 0, 1, 0 },
        .fovy = period,
        .projection = CAMERA_ORTHOGRAPHIC,
    };
    R3D_Camera rc = R3D_CameraFromRL(cam);
    rc.nearPlane = 1.0;
    rc.farPlane = 20.0;

    R3D_SetResolution(px, px);
    RenderTexture rt = LoadRenderTexture(px, px);
    Image out = GenImageColor(px, px, BLANK);
    const R3D_OutputMode MODES[2] = { R3D_OUTPUT_ALBEDO, R3D_OUTPUT_ORM };
    for (int m = 0; m < 2; m++) {
        R3D_SetOutputMode(MODES[m]);
        R3D_BeginPro((R3D_View){ .camera = rc, .target = rt });
        R3D_DrawMesh(quad, mat->material, centre, 1.0f);
        R3D_End();
        Image img = LoadImageFromTexture(rt.texture);   // GL order: row 0 = the bottom of the view
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        if (top) ImageFlipVertical(&img);
        const unsigned char *src = img.data;
        unsigned char *dst = out.data;
        for (int k = 0; k < px * px; k++) {
            if (m == 0) memcpy(dst + 4 * k, src + 4 * k, 3);
            else dst[4 * k + 3] = src[4 * k + 1];           // ORM: occlusion, roughness, metalness
        }
        UnloadImage(img);
    }
    R3D_SetOutputMode(R3D_OUTPUT_SCENE);
    UnloadRenderTexture(rt);
    R3D_UnloadMesh(quad);
    MS_Unload(s);
    return out;
}

int BakeSurface(const BakeArgs *a)
{
    if (a->size < 1 || a->levels < 1 || a->size % (1 << (a->levels - 1)) != 0) {
        fprintf(stderr, "monolith: --bake-size %d is not divisible by 2^(--bake-levels - 1) = %d\n", a->size, 1 << (a->levels - 1));
        return 1;
    }
    char path[MS_PATH_MAX];
    snprintf(path, sizeof(path), "%s.png", a->outPrefix);

    // A parse first: a broken map fails before any window, and R3D's init hints (the sun's
    // shadow map size) match the map, as in a --shot
    char err[512];
    MS_Scene *peek = MS_Load(a->mapPath, &(MS_LoadOptions){ .noGpu = true }, err, sizeof(err));
    if (!peek) {
        fprintf(stderr, "monolith: cannot load the map\n%s\n", err);
        return 1;
    }
    const MS_Material *mat = NULL;
    for (int i = 0; i < peek->materialCount; i++)
        if (strcmp(peek->materials[i].name, a->material) == 0) mat = &peek->materials[i];
    if (!mat) {
        fprintf(stderr, "monolith: %s has no material \"%s\"\n", peek->path, a->material);
        MS_Unload(peek);
        return 1;
    }
    BakeArgs b = *a;
    char shaderFile[MS_PATH_MAX];
    snprintf(shaderFile, sizeof(shaderFile), "%s", mat->shader ? mat->shader : "");
    b.shaderFile = mat->shader ? shaderFile : NULL;
    a = &b;
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(64, 64, "monolith-shot");
    MS_SetInitHints(peek);
    MS_Unload(peek);
    R3D_Init(a->size, a->size);
    MS_InitRenderer();

    int w = a->levels > 1 ? a->size + a->size / 2 : a->size;
    Image atlas = GenImageColor(w, a->size, BLANK);
    bool ok = true;
    for (int level = 0; level < a->levels && ok; level++) {
        Image tile = BakeLevel(a, level, err, sizeof(err));
        if (!tile.data) {
            fprintf(stderr, "monolith: cannot bake \"%s\"\n%s\n", a->material, err);
            ok = false;
            break;
        }
        int x = level == 0 ? 0 : a->size, y = level == 0 ? 0 : a->size - (a->size >> (level - 1));
        // (a plain copy: ImageDraw would blend with the alpha channel, which holds data)
        for (int row = 0; row < tile.height; row++)
            memcpy((unsigned char *)atlas.data + 4 * ((size_t)(y + row) * w + x),
                   (unsigned char *)tile.data + 4 * (size_t)row * tile.width, 4 * (size_t)tile.width);
        UnloadImage(tile);
    }
    if (ok) {
        ok = PngWrite(atlas, path);
        if (ok) printf("monolith: baked \"%s\" (%s face, %d px, %g texels/m: a %g m tile, %d level%s) into %s\n", a->material,
                       a->face == BAKE_FACE_TOP ? "top" : "side", a->size, a->texels, a->size / a->texels, a->levels,
                       a->levels > 1 ? "s" : "", path);
        else fprintf(stderr, "monolith: cannot write %s\n", path);
    }
    UnloadImage(atlas);
    R3D_Close();
    CloseWindow();
    return ok ? 0 : 1;
}
