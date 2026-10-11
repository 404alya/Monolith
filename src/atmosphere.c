#include "atmosphere.h"

// ======================================================================================
// #region AO / indirect trade-offs (--ao, --fx)
// ======================================================================================
// SSAO and SSIL run every frame at half resolution (~0.2 + ~0.5 ms GPU at 1920x1006, plus
// the normal / radiance downsamples they need), yet in this scene they change the image
// very little: the ambient they modulate is small next to the sun, the volumetric fog and
// the IBL ground fill (PROMPT-INSTRUCTIONS.md 7.5). --ao picks one of the runtime's AO modes
// (MS_Quality.ao, applied by MS_ApplyQuality) so the developer can trade them away and judge
// the look live. The default ("full") is the map's setup, unchanged. --bloom off is
// MS_Quality.bloom = false (bloom here is mostly a uniform ~20% lift: threshold 0, MIX 0.04;
// "off" skips the pass, ~0.18 ms, and raises exposure x1.19 instead, which matched the mean
// of the 26 verification shots best: +0.07/255 on average, every shot within -1.2..+0.8/255).

typedef struct { const char *name, *help; MS_AoMode mode; } AoMode;
static const AoMode AO_MODES[] = {
    { "full",  "SSAO + SSIL as set up (default)", MS_AO_FULL },
    { "ssao",  "SSIL off, SSAO unchanged", MS_AO_SSAO },
    { "lite",  "SSIL off, SSAO with 8 samples instead of 16", MS_AO_LITE },
    { "off",   "SSAO and SSIL off", MS_AO_OFF },
    { "baked", "SSAO and SSIL off, replaced by the baked world-space occlusion volume (0.5 m, 0.6)", MS_AO_BAKED },
};

bool AoModeFromName(const char *name, MS_AoMode *mode)
{
    for (int i = 0; i < (int)(sizeof(AO_MODES) / sizeof(AO_MODES[0])); i++)
        if (strcmp(name, AO_MODES[i].name) == 0) { *mode = AO_MODES[i].mode; return true; }
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
        { "vfog.step",      &env->volumetricFog.stepSize, NULL },
        { "vfog.length",    &env->volumetricFog.length, NULL },
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
           "  ssil.samples ssil.radius ssil.gi ssil.ao bloom.levels bloom.intensity exposure\n"
           "  vfog.step vfog.length\n";
}

// #endregion
