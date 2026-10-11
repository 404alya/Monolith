// 04_import_source -- a hand-written raylib + R3D scene, the test input for `monolith --import`
//
// A forest clearing at dusk, set up the way many R3D programs do it, directly in C: a ground
// slab and a small cabin from generated cube meshes, CC0 models (Kenney Nature Kit: trees and
// rocks, see assets/ASSETS.md) placed with position / yaw / scale and their unlit glTF
// materials switched to lit ones, three lights (a low shadowed sun, a warm campfire, a cold
// lantern spot), a dusk procedural sky with its ambient map, exp2 fog, bloom and ACES
// tonemapping. Nothing here knows about monolith; README.md shows how to import it.
//
//   import_source [--shot <file>]      a still camera; ESC quits

#include <r3d/r3d.h>
#include <raymath.h>
#include <math.h>
#include "../common/shot.h"

typedef struct { Vector3 pos; float yawDeg, scale; } Placement;

// Trees and rocks of the clearing (feet position, yaw in degrees, uniform scale)
static const Placement TREES[] = {
    { { -9, 0, -6 }, 0, 6.0f }, { { -12, 0, 3 }, 40, 7.0f }, { { 8, 0, -10 }, 75, 6.5f },
    { { 13, 0, -1 }, 160, 5.5f }, { { -4, 0, -14 }, 220, 7.5f }, { { 4, 0, -17 }, 300, 6.0f },
};
static const Placement ROCKS[] = { { { 6, 0, -3 }, 30, 3.0f }, { { -7, 0, 5 }, 200, 2.5f } };
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void DrawPlaced(R3D_Model model, Placement p)
{
    Quaternion q = QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, p.yawDeg * DEG2RAD);
    R3D_DrawModelEx(model, p.pos, q, (Vector3){ p.scale, p.scale, p.scale });
}

// Kenney's glTF files ask for unlit materials (KHR_materials_unlit); light them like the rest
static void MakeLit(R3D_Model *model, float roughness)
{
    for (int i = 0; i < model->materialCount; i++) {
        model->materials[i].unlit = false;
        model->materials[i].orm.roughness = roughness;
    }
}

int main(int argc, char **argv)
{
    Shot shot = ShotParse(argc, argv);
    ShotInitWindow(&shot, 1280, 720, "forest clearing");
    R3D_Init(GetScreenWidth(), GetScreenHeight());
    R3D_SetAntiAliasingMode(R3D_ANTI_ALIASING_MODE_FXAA);
    ShotBegin(&shot);

    // --- Geometry: generated cubes (ground, cabin) and model files (trees, rocks)
    R3D_Mesh cube = R3D_GenMeshCube(1, 1, 1);
    R3D_Material groundMat = R3D_GetDefaultMaterial();
    groundMat.albedo.color = (Color){ 70, 82, 52, 255 };
    groundMat.orm.roughness = 0.95f;
    R3D_Material woodMat = R3D_GetDefaultMaterial();
    woodMat.albedo.color = (Color){ 120, 84, 56, 255 };
    woodMat.orm.roughness = 0.85f;
    R3D_Material roofMat = R3D_GetDefaultMaterial();
    roofMat.albedo.color = (Color){ 60, 50, 46, 255 };
    roofMat.orm.roughness = 0.7f;
    R3D_Material emberMat = R3D_GetDefaultMaterial();
    emberMat.albedo.color = (Color){ 40, 20, 10, 255 };
    emberMat.emission.color = (Color){ 255, 120, 40, 255 };
    emberMat.emission.energy = 4.0f;

    const char *dir = GetApplicationDirectory();
    R3D_Model tree = R3D_LoadModel(TextFormat("%sassets/tree_oak.glb", dir));
    R3D_Model rock = R3D_LoadModel(TextFormat("%sassets/rock_largeA.glb", dir));
    MakeLit(&tree, 0.8f);
    MakeLit(&rock, 0.9f);

    // --- Environment: dusk sky + its ambient map, fog, bloom, tonemap
    R3D_ProceduralSky skyParams = R3D_PROCEDURAL_SKY_BASE;
    skyParams.skyTopColor = (Color){ 40, 52, 90, 255 };
    skyParams.skyHorizonColor = (Color){ 230, 140, 90, 255 };
    skyParams.groundHorizonColor = (Color){ 120, 90, 80, 255 };
    skyParams.sunDirection = (Vector3){ -0.8f, -0.25f, 0.5f };
    skyParams.sunColor = (Color){ 255, 170, 110, 255 };
    skyParams.sunSize = 3.0f * DEG2RAD;
    R3D_Cubemap sky = R3D_GenProceduralSky(1024, skyParams);
    R3D_AmbientMap ambient = R3D_GenAmbientMap(sky, R3D_AMBIENT_ILLUMINATION | R3D_AMBIENT_REFLECTION);
    R3D_Environment *env = R3D_GetEnvironment();
    env->background.sky = sky;
    env->ambient.map = ambient;
    env->ambient.energy = 0.6f;
    env->fog.mode = R3D_FOG_EXP2;
    env->fog.color = (Color){ 110, 100, 120, 255 };
    env->fog.density = 0.02f;
    env->fog.skyAffect = 0.4f;
    env->bloom.mode = R3D_BLOOM_MIX;
    env->bloom.intensity = 0.06f;
    env->tonemap.mode = R3D_TONEMAP_ACES;
    env->tonemap.exposure = 1.1f;
    env->ssao.enabled = true;

    // --- Lights
    R3D_Light sun = R3D_CreateDirLight(skyParams.sunDirection, (Color){ 255, 170, 110, 255 }, 1.6f);
    sun.range = 60.0f;
    R3D_ShadowMap sunShadow = R3D_LoadShadowMap(R3D_LIGHT_DIR);
    sunShadow.softness = 3.0f;
    R3D_Light fire = R3D_CreateOmniLight((Vector3){ 0, 0.8f, 1 }, 9.0f, (Color){ 255, 130, 50, 255 }, 4.0f);
    R3D_Light lantern = R3D_CreateSpotLight((Vector3){ -1.2f, 3.2f, -6.4f }, (Vector3){ 0.2f, -1, 0.6f }, 14.0f,
                                            (Color){ 170, 200, 255, 255 }, 5.0f);
    lantern.innerCutOff = 20.0f;
    lantern.outerCutOff = 35.0f;

    Camera3D camera = { .position = { 6, 2.4f, 12 }, .target = { -1, 1.5f, -4 }, .up = { 0, 1, 0 }, .fovy = 60,
                        .projection = CAMERA_PERSPECTIVE };

    while (!WindowShouldClose()) {
        BeginDrawing();
            ClearBackground(BLACK);
            R3D_BeginPro((R3D_View){ .camera = R3D_CameraFromRL(camera), .target = shot.target });
                R3D_PushLightEx(sun, sunShadow, true);
                R3D_PushLight(fire);
                R3D_PushLight(lantern);
                // ground: 60 x 1 x 60 slab, top at y = 0
                R3D_DrawMeshEx(cube, groundMat, (Vector3){ 0, -0.5f, 0 }, QuaternionIdentity(), (Vector3){ 60, 1, 60 });
                // cabin: walls, roof, door step
                R3D_DrawMeshEx(cube, woodMat, (Vector3){ 0, 1.5f, -8 }, QuaternionIdentity(), (Vector3){ 6, 3, 4 });
                R3D_DrawMeshEx(cube, roofMat, (Vector3){ 0, 3.25f, -8 }, QuaternionIdentity(), (Vector3){ 7, 0.5f, 5 });
                R3D_DrawMeshEx(cube, woodMat, (Vector3){ 0, 0.1f, -5.6f }, QuaternionIdentity(), (Vector3){ 1.6f, 0.2f, 0.8f });
                // campfire: a glowing ember box with a ring of stones
                R3D_DrawMeshEx(cube, emberMat, (Vector3){ 0, 0.15f, 1 }, QuaternionIdentity(), (Vector3){ 0.6f, 0.3f, 0.6f });
                for (int i = 0; i < 6; i++) {
                    float a = i * PI / 3.0f;
                    R3D_DrawMeshEx(cube, roofMat, (Vector3){ cosf(a) * 0.7f, 0.12f, 1 + sinf(a) * 0.7f },
                                   QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, -a), (Vector3){ 0.35f, 0.24f, 0.25f });
                }
                for (int i = 0; i < COUNT(TREES); i++) DrawPlaced(tree, TREES[i]);
                for (int i = 0; i < COUNT(ROCKS); i++) DrawPlaced(rock, ROCKS[i]);
            R3D_End();
        EndDrawing();
        if (ShotFrameDone(&shot)) break;
    }

    ShotEnd(&shot);
    R3D_UnloadShadowMap(sunShadow);
    R3D_UnloadAmbientMap(ambient);
    R3D_UnloadCubemap(sky);
    R3D_UnloadModel(rock, true);
    R3D_UnloadModel(tree, true);
    R3D_UnloadMesh(cube);
    R3D_Close();
    CloseWindow();
    return 0;
}
