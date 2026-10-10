#include "atmosphere.h"

// ======================================================================================
// #region Atmosphere
// ======================================================================================

// Direction the sunlight travels: high enough to fall through the roof gaps,
// from the side so it rakes across the facades.
const Vector3 SUN_DIR = { 0.55f, -0.78f, 0.30f };

void SetupEnvironment(R3D_Cubemap *sky, R3D_AmbientMap *ambient)
{
    // Pale, overcast-ish sky with a hazy sun
    R3D_ProceduralSky sp = R3D_PROCEDURAL_SKY_BASE;
    sp.skyTopColor        = (Color){ 120, 132, 145, 255 };
    sp.skyHorizonColor    = (Color){ 200, 196, 186, 255 };
    sp.groundBottomColor  = (Color){ 48, 46, 44, 255 };
    sp.groundHorizonColor = (Color){ 190, 186, 178, 255 };
    sp.sunDirection       = SUN_DIR;
    sp.sunColor           = (Color){ 255, 236, 205, 255 };
    sp.sunSize            = 2.5f * DEG2RAD;
    sp.sunEnergy          = 2.0f;
    *sky = R3D_GenProceduralSky(1024, sp);

    // The ambient map comes from a second, ambient-only sky whose lower hemisphere
    // stands in for light bounced off the sunlit ground. With the visible sky's
    // dark ground every downward-facing surface (soffits, beam undersides, the hall
    // ceiling) got almost no ambient and rendered black; SSIL can't fill in, as it
    // only sees bounce from lit pixels on screen. 120 sRGB = 0.19 linear, somewhat
    // above fully sunlit 0.25-albedo ground (~0.12), tuned by eye: undersides end
    // up at ~10-25% of sunlit faces. The visible sky (*sky) is unchanged.
    R3D_ProceduralSky ap = sp;
    ap.groundBottomColor  = (Color){ 120, 116, 108, 255 };
    ap.groundHorizonCurve = 0.1f;   // softer blend into the bright horizon colour
    R3D_Cubemap ambientSky = R3D_GenProceduralSky(256, ap);
    *ambient = R3D_GenAmbientMap(ambientSky, R3D_AMBIENT_ILLUMINATION | R3D_AMBIENT_REFLECTION);
    R3D_UnloadCubemap(ambientSky);

    R3D_Environment *env = R3D_GetEnvironment();
    env->background.sky = *sky;
    env->ambient.map = *ambient;
    env->ambient.energy = 0.9f;

    // Distance fog: swallows the megastructures
    env->fog.mode = R3D_FOG_EXP2;
    env->fog.color = (Color){ 150, 148, 143, 255 };
    env->fog.density = 0.0085f;
    env->fog.skyAffect = 0.7f;

    // Volumetric fog: god rays through the roof slits and between the pillars
    env->volumetricFog.enabled = true;
    // Keep the density low: R3D's volumetric fog is lit only by lights, not by
    // the sky, so dense fog in shadow turns into a black curtain that hides
    // everything behind it. The emission stands in for the missing sky light.
    env->volumetricFog.scatteringDensity = 0.012f;
    env->volumetricFog.absortionDensity = 0.001f;
    env->volumetricFog.scatteringColor = (Color){ 255, 244, 228, 255 };
    env->volumetricFog.emissionColor = (Color){ 150, 148, 143, 255 };
    env->volumetricFog.emissionEnergy = 0.12f;
    env->volumetricFog.anisotropy = 0.4f;
    env->volumetricFog.length = 90.0f;
    env->volumetricFog.stepSize = 0.75f;

    // Contact shadows in concrete corners + bounced light
    env->ssao.enabled = true;
    env->ssao.radius = 1.2f;
    env->ssao.intensity = 1.2f;
    env->ssil.enabled = true;
    env->ssil.radius = 5.0f;
    env->ssil.giIntensity = 1.2f;

    env->bloom.mode = R3D_BLOOM_MIX;
    env->bloom.intensity = 0.04f;

    env->tonemap.mode = R3D_TONEMAP_AGX;
    env->tonemap.exposure = 0.9f;
    env->color.saturation = 0.8f;
    env->color.contrast = 1.05f;
}

// #endregion

// ======================================================================================
// #region AO / indirect trade-offs (--ao, --fx)
// ======================================================================================
// SSAO and SSIL run every frame at half resolution (~0.2 + ~0.5 ms GPU at 1920x1006, plus
// the normal / radiance downsamples they need), yet in this scene they change the image
// very little: the ambient they modulate is small next to the sun, the volumetric fog and
// the IBL ground fill (PROMPT-INSTRUCTIONS.md 7.5). These presets let the developer trade
// them away and judge the look live. The default ("full") is the setup above, unchanged.

typedef struct { const char *name, *help; } AoMode;
static const AoMode AO_MODES[] = {
    { "full",  "SSAO + SSIL as set up (default)" },
    { "ssao",  "SSIL off, SSAO unchanged" },
    { "lite",  "SSIL off, SSAO with 8 samples instead of 16" },
    { "off",   "SSAO and SSIL off" },
};

bool ApplyAoMode(R3D_Environment *env, const char *mode)
{
    if (strcmp(mode, "full") == 0) return true;
    if (strcmp(mode, "ssao") == 0) { env->ssil.enabled = false; return true; }
    if (strcmp(mode, "lite") == 0) { env->ssil.enabled = false; env->ssao.sampleCount = 8; return true; }
    if (strcmp(mode, "off") == 0)  { env->ssil.enabled = false; env->ssao.enabled = false; return true; }
    return false;
}

const char *AoModeHelp(void)
{
    static char buf[512];
    int n = 0;
    for (int i = 0; i < (int)(sizeof(AO_MODES) / sizeof(AO_MODES[0])); i++)
        n += snprintf(buf + n, sizeof(buf) - n, "  %-5s %s\n", AO_MODES[i].name, AO_MODES[i].help);
    return buf;
}

// Bloom here is not a highlight glow: with threshold 0 and R3D's additive upsample chain,
// MIX at 0.04 blends in a wide blur of the whole frame that is several times brighter than
// the scene, i.e. mostly a uniform ~20% lift (bloom off: mean luminance -9/255) plus a faint
// halo at bright openings. "off" skips the pass (~0.18 ms) and replaces the lift with
// exposure; 1.19 matched the mean of the 26 verification shots best (+0.07/255 on
// average, every shot within -1.2..+0.8/255).
bool ApplyBloomMode(R3D_Environment *env, const char *mode)
{
    if (strcmp(mode, "on") == 0) return true;
    if (strcmp(mode, "off") == 0) {
        env->bloom.mode = R3D_BLOOM_DISABLED;
        env->tonemap.exposure *= 1.19f;
        return true;
    }
    return false;
}

// Numeric knobs for experiments: --fx ssil.radius=2.5 etc. Integers are rounded.
typedef struct { const char *key; float *f; int *i; } FxKnob;

bool ApplyFxSetting(R3D_Environment *env, const char *keyValue)
{
    const FxKnob knobs[] = {
        { "ssao.samples",   NULL, &env->ssao.sampleCount },
        { "ssao.radius",    &env->ssao.radius, NULL },
        { "ssao.intensity", &env->ssao.intensity, NULL },
        { "ssao.power",     &env->ssao.power, NULL },
        { "ssil.samples",   NULL, &env->ssil.sampleCount },
        { "ssil.radius",    &env->ssil.radius, NULL },
        { "ssil.gi",        &env->ssil.giIntensity, NULL },
        { "ssil.ao",        &env->ssil.aoIntensity, NULL },
        { "bloom.levels",   &env->bloom.levels, NULL },
        { "bloom.intensity", &env->bloom.intensity, NULL },
        { "exposure",       &env->tonemap.exposure, NULL },
    };
    const char *eq = strchr(keyValue, '=');
    if (!eq || !eq[1]) return false;
    char *end;
    float v = strtof(eq + 1, &end);
    if (*end) return false;
    for (int k = 0; k < (int)(sizeof(knobs) / sizeof(knobs[0])); k++) {
        if (strlen(knobs[k].key) != (size_t)(eq - keyValue) || strncmp(knobs[k].key, keyValue, eq - keyValue) != 0) continue;
        if (knobs[k].f) *knobs[k].f = v;
        else *knobs[k].i = (int)lroundf(v);
        return true;
    }
    return false;
}

const char *FxSettingHelp(void)
{
    return "  ssao.samples ssao.radius ssao.intensity ssao.power\n"
           "  ssil.samples ssil.radius ssil.gi ssil.ao bloom.levels bloom.intensity exposure\n";
}

// #endregion
