// Graphics settings: defaults, presets, the row table (panel + file) and the settings file.
// The scene part is the runtime's MS_Quality (runtime/monolith_scene.c applies it); this file
// only describes, steps, reads and writes the values.
#include "settings.h"
#include <stddef.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

GfxSettings GfxDefaults(void)
{
    GfxSettings s;
    memset(&s, 0, sizeof(s));   // (padding too: main.c's frame key compares the bytes)
    s.q = MS_QualityPreset(MS_QUALITY_ULTRA);
    s.base = MS_QUALITY_ULTRA;
    s.windowMsaa = false;
    s.vsync = false;
    s.fpsCap = 0;
    s.frameReuse = true;
    return s;
}

void GfxSetPreset(GfxSettings *s, MS_QualityLevel level)
{
    if (level == MS_QUALITY_CUSTOM) return;
    s->q = MS_QualityPreset(level);
    s->base = level;
}

MS_QualityLevel GfxPreset(const GfxSettings *s)
{
    return MS_QualityMatch(&s->q);
}

// ======================================================================================
// #region Rows
// ======================================================================================

typedef enum { V_NONE, V_BOOL, V_INT, V_FLOAT, V_ENUM } VType;

typedef struct {
    GfxGroup group;
    GfxRowKind kind;
    const char *label, *key, *hint;
    VType type;
    size_t off;                 // offset of the field in GfxSettings
    const float *opts;          // V_INT / V_FLOAT: the values the arrows step through
    int optCount;
    const char *const *names;   // V_ENUM: value = nameBase + index
    int nameCount, nameBase;
    const char *fmt;            // V_INT / V_FLOAT display format
    const char *zero;           // label of 0 ("map's", "off"), NULL = show the number
    float min, max;             // file values are clamped to this (0 is allowed when `zero`)
    bool restart, heavy;
} Row;

#define OFF(f) offsetof(GfxSettings, f)
#define OPTS(...) (const float[]){ __VA_ARGS__ }, (int)(sizeof((const float[]){ __VA_ARGS__ }) / sizeof(float))
#define NAMES(...) (const char *const[]){ __VA_ARGS__ }, (int)(sizeof((const char *const[]){ __VA_ARGS__ }) / sizeof(const char *))

static const Row ROWS[] = {
    // --- Display
    { GFX_GROUP_DISPLAY, GFX_ROW_PRESET, "Preset", "preset",
      "low / medium / high / ultra (= the map exactly); a manual change makes it custom", .type = V_NONE },
    { GFX_GROUP_DISPLAY, 0, "Render scale", "render_scale",
      "R3D renders at this fraction of the window and upscales: 0.75 ~ -39% frame time, 0.5 ~ -55%",
      V_FLOAT, OFF(q.renderScale), OPTS(0.5f, 0.6f, 0.67f, 0.75f, 0.85f, 1.0f), .fmt = "%.2f", .min = 0.25f, .max = 1.0f },
    { GFX_GROUP_DISPLAY, 0, "Upscale filter", "upscale", "filter for render scale < 1 (bicubic: soft but stable)",
      V_ENUM, OFF(q.upscale), .names = NAMES("nearest", "linear", "bicubic", "lanczos") },
    { GFX_GROUP_DISPLAY, 0, "Anti-aliasing", "anti_aliasing", "R3D's post AA: FXAA (default) or SMAA (sharper, a bit more GPU)",
      V_ENUM, OFF(q.antiAliasing), .names = NAMES("none", "fxaa", "smaa") },
    { GFX_GROUP_DISPLAY, 0, "Window MSAA 4x", "window_msaa", "smoother overlay lines only (~0.1 ms); applies at the next start",
      V_BOOL, OFF(windowMsaa), .restart = true },
    { GFX_GROUP_DISPLAY, 0, "Exposure", "exposure", "display trim: x the map's tonemap exposure",
      V_FLOAT, OFF(q.exposure), OPTS(0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f, 1.35f, 1.5f), .fmt = "x%.2f", .min = 0.1f, .max = 4.0f },
    { GFX_GROUP_DISPLAY, 0, "Brightness", "brightness", "display trim: x the map's colour brightness",
      V_FLOAT, OFF(q.brightness), OPTS(0.8f, 0.9f, 0.95f, 1.0f, 1.05f, 1.1f, 1.2f), .fmt = "x%.2f", .min = 0.1f, .max = 4.0f },
    { GFX_GROUP_DISPLAY, 0, "Contrast", "contrast", "display trim: x the map's contrast",
      V_FLOAT, OFF(q.contrast), OPTS(0.8f, 0.9f, 0.95f, 1.0f, 1.05f, 1.1f, 1.2f), .fmt = "x%.2f", .min = 0.1f, .max = 4.0f },
    { GFX_GROUP_DISPLAY, 0, "Saturation", "saturation", "display trim: x the map's saturation",
      V_FLOAT, OFF(q.saturation), OPTS(0.0f, 0.5f, 0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f), .fmt = "x%.2f", .min = 0.0f, .max = 4.0f },
    // --- Shadows
    { GFX_GROUP_SHADOWS, 0, "Sun shadow", "sun_shadow", "off: no sun shadows, no shadowed light shafts",
      V_BOOL, OFF(q.sunShadow) },
    { GFX_GROUP_SHADOWS, 0, "Shadow map size", "shadow_size",
      "px; 8192 = 3.7 cm texels, 256 MB VRAM; 4096 = 7.4 cm, 64 MB. Restarts the renderer (~1 s)",
      V_INT, OFF(q.shadowSize), OPTS(0, 2048, 4096, 6144, 8192, 16384), .fmt = "%.0f", .zero = "map's",
      .min = 256, .max = 16384, .heavy = true },
    { GFX_GROUP_SHADOWS, 0, "Shadow range", "shadow_range",
      "m; texels grow with it; below the fog length (90 m) the shafts fade early",
      V_FLOAT, OFF(q.shadowRange), OPTS(0, 60, 80, 100, 120, 150, 200, 300), .fmt = "%.0f m", .zero = "map's",
      .min = 10, .max = 2000, .heavy = true },
    { GFX_GROUP_SHADOWS, 0, "Shadow softness", "shadow_softness", "PCF filter radius (the map's: 2)",
      V_FLOAT, OFF(q.shadowSoftness), OPTS(0, 0.5f, 1, 1.5f, 2, 3, 4, 6), .fmt = "%.1f", .zero = "map's", .min = 0, .max = 64 },
    { GFX_GROUP_SHADOWS, 0, "Shadow mode", "shadow_mode",
      "tiles: drawn once per map load (default); camera: re-fitted when the view leaves it; always: every frame",
      V_ENUM, OFF(q.shadowMode), .names = NAMES("tiles", "camera", "always"), .heavy = true },
    // --- Ambient occlusion
    { GFX_GROUP_AO, 0, "Mode", "ao",
      "full: SSAO+SSIL (~0.7 ms); baked: world-space volume (~-27%); off: none (~-0.9 ms)",
      V_ENUM, OFF(q.ao), .names = NAMES("full", "ssao", "lite", "baked", "off"), .heavy = true },
    { GFX_GROUP_AO, 0, "Baked voxel", "ao_volume_voxel", "m; baked occlusion volume resolution (auto: 0.5 with mode baked)",
      V_FLOAT, OFF(q.aoVolumeVoxel), OPTS(0, 0.25f, 0.5f, 1, 2), .fmt = "%.2f m", .zero = "auto", .min = 0.25f, .max = 4, .heavy = true },
    { GFX_GROUP_AO, 0, "Baked intensity", "ao_volume_intensity", "strength of the baked occlusion (auto: 0.6 with mode baked)",
      V_FLOAT, OFF(q.aoVolumeIntensity), OPTS(0, 0.3f, 0.45f, 0.6f, 0.8f, 1, 1.5f, 2), .fmt = "%.2f", .zero = "auto", .min = 0.01f, .max = 4 },
    { GFX_GROUP_AO, 0, "SSAO samples", "ssao_samples", "per pixel (the map's: 16)",
      V_INT, OFF(q.ssaoSamples), OPTS(0, 4, 8, 12, 16, 24, 32), .fmt = "%.0f", .zero = "map's", .min = 1, .max = 64 },
    { GFX_GROUP_AO, 0, "SSAO radius", "ssao_radius", "m (the map's: 1)",
      V_FLOAT, OFF(q.ssaoRadius), OPTS(0, 0.5f, 1, 1.5f, 2, 3), .fmt = "%.1f m", .zero = "map's", .min = 0.05f, .max = 20 },
    { GFX_GROUP_AO, 0, "SSAO intensity", "ssao_intensity", "the map's: 1.2",
      V_FLOAT, OFF(q.ssaoIntensity), OPTS(0, 0.5f, 0.8f, 1, 1.2f, 1.5f, 2), .fmt = "%.1f", .zero = "map's", .min = 0.01f, .max = 10 },
    { GFX_GROUP_AO, 0, "SSIL samples", "ssil_samples", "per pixel (the map's: 16)",
      V_INT, OFF(q.ssilSamples), OPTS(0, 4, 8, 12, 16, 24, 32), .fmt = "%.0f", .zero = "map's", .min = 1, .max = 64 },
    { GFX_GROUP_AO, 0, "SSIL radius", "ssil_radius", "m (the map's: 5)",
      V_FLOAT, OFF(q.ssilRadius), OPTS(0, 1, 2, 3, 4, 5, 6, 8), .fmt = "%.0f m", .zero = "map's", .min = 0.1f, .max = 50 },
    // --- Fog
    { GFX_GROUP_FOG, 0, "Volumetric fog", "volumetric_fog", "light shafts; off is a big change of the look",
      V_BOOL, OFF(q.volumetricFog) },
    { GFX_GROUP_FOG, 0, "March step", "fog_step", "x the map's step (0.75 m); x2 = half the steps, a little grain in shafts",
      V_FLOAT, OFF(q.fogStepScale), OPTS(0.5f, 0.75f, 1, 1.5f, 2, 3), .fmt = "x%.2f", .min = 0.1f, .max = 10 },
    { GFX_GROUP_FOG, 0, "March length", "fog_length", "x the map's length (90 m)",
      V_FLOAT, OFF(q.fogLengthScale), OPTS(0.5f, 0.75f, 1, 1.25f, 1.5f), .fmt = "x%.2f", .min = 0.1f, .max = 10 },
    { GFX_GROUP_FOG, 0, "Fog cache voxel", "fog_cache_voxel",
      "m; bake the shafts' sun visibility once per map load (-0.2 ms, edges ~0.5 m softer)",
      V_FLOAT, OFF(q.fogCacheVoxel), OPTS(0, 0.25f, 0.5f, 1, 2), .fmt = "%.2f m", .zero = "off", .min = 0.25f, .max = 8, .heavy = true },
    { GFX_GROUP_FOG, 0, "Fog cache samples", "fog_cache_sub", "sub^3 samples per voxel of the bake",
      V_INT, OFF(q.fogCacheSub), OPTS(1, 2, 3, 4), .fmt = "%.0f", .min = 1, .max = 4, .heavy = true },
    { GFX_GROUP_FOG, 0, "Distance fog", "distance_fog", "the far haze that turns megastructures into silhouettes",
      V_BOOL, OFF(q.distanceFog) },
    // --- Post
    { GFX_GROUP_POST, 0, "Bloom", "bloom", "off: -6%, exposure x the map's bloom.offExposure instead (same mean brightness)",
      V_BOOL, OFF(q.bloom) },
    { GFX_GROUP_POST, 0, "Bloom intensity", "bloom_intensity", "x the map's bloom intensity",
      V_FLOAT, OFF(q.bloomIntensityScale), OPTS(0.5f, 0.75f, 1, 1.5f, 2), .fmt = "x%.2f", .min = 0, .max = 10 },
    { GFX_GROUP_POST, 0, "SSR", "ssr", "screen-space reflections (off in the default map)",
      V_ENUM, OFF(q.ssr), .names = NAMES("off", "map's", "on"), .nameBase = -1 },
    { GFX_GROUP_POST, 0, "SSGI", "ssgi", "screen-space global illumination (off in the default map)",
      V_ENUM, OFF(q.ssgi), .names = NAMES("off", "map's", "on"), .nameBase = -1 },
    // --- Performance
    { GFX_GROUP_PERF, 0, "Frame reuse", "frame_reuse", "a still camera reuses the last image (~0.2 ms a frame)",
      V_BOOL, OFF(frameReuse) },
    { GFX_GROUP_PERF, 0, "FPS cap", "fps_cap", "frames per second; monitor = its refresh rate",
      V_INT, OFF(fpsCap), OPTS(0, -1, 30, 60, 120, 144, 165, 240), .fmt = "%.0f", .zero = "off", .min = -1, .max = 1000 },
    { GFX_GROUP_PERF, 0, "Vsync", "vsync", "wait for the display (no tearing; caps at its refresh rate)",
      V_BOOL, OFF(vsync) },
    { GFX_GROUP_PERF, 0, "Depth pre-pass", "depth_prepass", "same image; off only for A/B tests (slower)",
      V_BOOL, OFF(q.depthPrepass) },
};
#define ROW_COUNT ((int)(sizeof(ROWS) / sizeof(ROWS[0])))

// Every enum field is stored as an int (C enums with these small values are int-sized)
_Static_assert(sizeof(MS_AoMode) == sizeof(int) && sizeof(R3D_UpscaleMode) == sizeof(int) &&
               sizeof(R3D_AntiAliasingMode) == sizeof(int) && sizeof(MS_ShadowMode) == sizeof(int), "enum size");

static const char *const GROUP_NAMES[GFX_GROUP_COUNT] = {
    "Display", "Shadows", "Ambient occlusion", "Fog", "Post", "Performance" };

int GfxRowCount(void) { return ROW_COUNT; }
static bool RowOk(int r) { return r >= 0 && r < ROW_COUNT; }
GfxGroup GfxRowGroup(int r) { return RowOk(r) ? ROWS[r].group : GFX_GROUP_DISPLAY; }
GfxRowKind GfxRowKindOf(int r) { return RowOk(r) ? ROWS[r].kind : GFX_ROW_VALUE; }
const char *GfxRowLabel(int r) { return RowOk(r) ? ROWS[r].label : ""; }
const char *GfxRowKey(int r) { return RowOk(r) ? ROWS[r].key : ""; }
const char *GfxRowHint(int r) { return RowOk(r) ? ROWS[r].hint : ""; }
const char *GfxGroupName(GfxGroup g) { return (g >= 0 && g < GFX_GROUP_COUNT) ? GROUP_NAMES[g] : ""; }
bool GfxRowNeedsRestart(int r) { return RowOk(r) && ROWS[r].restart; }
bool GfxRowIsHeavy(int r) { return RowOk(r) && (ROWS[r].heavy || ROWS[r].kind == GFX_ROW_PRESET); }

static void *Field(GfxSettings *s, const Row *row) { return (char *)s + row->off; }
static const void *CField(const GfxSettings *s, const Row *row) { return (const char *)s + row->off; }

static float GetNum(const GfxSettings *s, const Row *row)
{
    if (row->type == V_FLOAT) return *(const float *)CField(s, row);
    if (row->type == V_BOOL) return *(const bool *)CField(s, row) ? 1.0f : 0.0f;
    return (float)*(const int *)CField(s, row);
}

static void SetNum(GfxSettings *s, const Row *row, float v)
{
    if (row->type == V_FLOAT) *(float *)Field(s, row) = v;
    else if (row->type == V_BOOL) *(bool *)Field(s, row) = v != 0.0f;
    else *(int *)Field(s, row) = (int)lroundf(v);
}

void GfxRowFormat(const GfxSettings *s, int r, char *out, int outSize)
{
    if (!RowOk(r) || outSize <= 0) return;
    const Row *row = &ROWS[r];
    if (row->kind == GFX_ROW_PRESET) {
        MS_QualityLevel p = GfxPreset(s);
        if (p == MS_QUALITY_CUSTOM) snprintf(out, outSize, "custom (from %s)", MS_QualityName(s->base));
        else snprintf(out, outSize, "%s", MS_QualityName(p));
        return;
    }
    float v = GetNum(s, row);
    switch (row->type) {
    case V_BOOL: snprintf(out, outSize, "%s", v != 0.0f ? "on" : "off"); break;
    case V_ENUM: {
        int i = (int)v - row->nameBase;
        snprintf(out, outSize, "%s", (i >= 0 && i < row->nameCount) ? row->names[i] : "?");
        break;
    }
    default:
        if (v == 0.0f && row->zero) snprintf(out, outSize, "%s", row->zero);
        else if (v == -1.0f && row->off == OFF(fpsCap)) snprintf(out, outSize, "monitor");
        else snprintf(out, outSize, row->fmt, v);
        break;
    }
}

bool GfxRowStep(GfxSettings *s, int r, int dir)
{
    if (!RowOk(r) || dir == 0) return false;
    const Row *row = &ROWS[r];
    if (row->kind == GFX_ROW_PRESET) {
        // From custom, the first press goes back to the preset it came from
        MS_QualityLevel cur = GfxPreset(s);
        int next = cur == MS_QUALITY_CUSTOM ? (int)s->base : (int)cur + dir;
        if (next < MS_QUALITY_LOW || next > MS_QUALITY_ULTRA) return false;
        GfxSetPreset(s, (MS_QualityLevel)next);
        return true;
    }
    float v = GetNum(s, row);
    float nv = v;
    switch (row->type) {
    case V_BOOL: nv = v != 0.0f ? 0.0f : 1.0f; break;
    case V_ENUM: {
        int i = (int)v - row->nameBase + dir;
        if (i < 0 || i >= row->nameCount) return false;
        nv = (float)(i + row->nameBase);
        break;
    }
    default: {
        // The option list's order is the stepping order (fps cap: off, monitor, 30, 60 ...)
        int at = -1;
        for (int k = 0; k < row->optCount; k++) if (row->opts[k] == v) at = k;
        if (at >= 0) {
            if (at + dir < 0 || at + dir >= row->optCount) return false;
            nv = row->opts[at + dir];
        } else {
            // A value from the file or the command line: the nearest option in that direction
            int best = -1;
            for (int k = 0; k < row->optCount; k++) {
                float o = row->opts[k];
                if (o <= 0.0f && row->zero) continue;
                if ((dir > 0 && o > v && (best < 0 || o < row->opts[best])) ||
                    (dir < 0 && o < v && (best < 0 || o > row->opts[best]))) best = k;
            }
            if (best < 0) return false;
            nv = row->opts[best];
        }
        break;
    }
    }
    if (nv == v) return false;
    SetNum(s, row, nv);
    return true;
}

void GfxRowCopy(GfxSettings *dst, const GfxSettings *src, int r)
{
    if (!RowOk(r)) return;
    const Row *row = &ROWS[r];
    if (row->kind == GFX_ROW_PRESET) { dst->q = src->q; dst->base = src->base; return; }
    SetNum(dst, row, GetNum(src, row));
}

// #endregion

// ======================================================================================
// #region The file
// ======================================================================================

const char *GfxDefaultPath(void)
{
    static char path[1200];
    const char *base;
#ifdef _WIN32
    base = getenv("APPDATA");
    if (base && base[0]) { snprintf(path, sizeof(path), "%s\\monolith\\settings.ini", base); return path; }
#else
    base = getenv("XDG_CONFIG_HOME");
    if (base && base[0] == '/') { snprintf(path, sizeof(path), "%s/monolith/settings.ini", base); return path; }
    base = getenv("HOME");
    if (base && base[0] == '/') { snprintf(path, sizeof(path), "%s/.config/monolith/settings.ini", base); return path; }
#endif
    snprintf(path, sizeof(path), "%ssettings.ini", GetApplicationDirectory());
    return path;
}

static bool ParseBool(const char *v, bool *out)
{
    if (!strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "1")) { *out = true; return true; }
    if (!strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "no") || !strcmp(v, "0")) { *out = false; return true; }
    return false;
}

// key, value: trimmed, NUL-terminated
static bool SetKeyValue(GfxSettings *s, const char *key, const char *v)
{
    if (strcmp(key, "base") == 0) {
        MS_QualityLevel l;
        if (!MS_QualityFromName(v, &l) || l == MS_QUALITY_CUSTOM) return false;
        s->base = l;
        return true;
    }
    for (int r = 0; r < ROW_COUNT; r++) {
        const Row *row = &ROWS[r];
        if (strcmp(key, row->key) != 0) continue;
        if (row->kind == GFX_ROW_PRESET) {
            MS_QualityLevel l;
            if (!MS_QualityFromName(v, &l)) return false;
            GfxSetPreset(s, l);         // custom: the keys that follow say what
            return true;
        }
        switch (row->type) {
        case V_BOOL: {
            bool b;
            if (!ParseBool(v, &b)) return false;
            *(bool *)Field(s, row) = b;
            return true;
        }
        case V_ENUM:
            for (int i = 0; i < row->nameCount; i++)
                if (strcmp(v, row->names[i]) == 0) { *(int *)Field(s, row) = i + row->nameBase; return true; }
            if (row->nameBase == -1 && strcmp(v, "map") == 0) { *(int *)Field(s, row) = 0; return true; }
            return false;
        default: {
            float f;
            if ((row->zero && (strcmp(v, row->zero) == 0 || strcmp(v, "map") == 0)) || (!strcmp(v, "off") && row->zero)) f = 0.0f;
            else if (row->off == OFF(fpsCap) && strcmp(v, "monitor") == 0) f = -1.0f;
            else {
                char *end;
                errno = 0;
                f = strtof(v, &end);
                if (end == v || *end || errno) return false;
                // strtof takes "nan" / "inf"; a NaN passes the clamp below and turns the
                // frame black (and the panel would save it), so no non-finite value anywhere
                if (!isfinite(f)) {
                    TraceLog(LOG_WARNING, "monolith: setting %s: '%s' is not a finite number (rejected)", key, v);
                    return false;
                }
                bool zeroOk = row->zero && f == 0.0f;
                bool negOk = row->off == OFF(fpsCap) && f == -1.0f;
                if (!zeroOk && !negOk) f = Clamp(f, row->min, row->max);
            }
            SetNum(s, row, f);
            return true;
        }
        }
    }
    return false;
}

static char *Trim(char *t)
{
    while (*t && isspace((unsigned char)*t)) t++;
    char *e = t + strlen(t);
    while (e > t && isspace((unsigned char)e[-1])) *--e = 0;
    return t;
}

bool GfxSetKey(GfxSettings *s, const char *keyValue)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", keyValue);
    char *eq = strchr(buf, '=');
    if (!eq) return false;
    *eq = 0;
    return SetKeyValue(s, Trim(buf), Trim(eq + 1));
}

const char *GfxKeyHelp(void)
{
    static char buf[2048];
    int n = snprintf(buf, sizeof(buf), "  preset (low medium high ultra custom), base");
    for (int r = 1; r < ROW_COUNT && n < (int)sizeof(buf) - 40; r++)
        n += snprintf(buf + n, sizeof(buf) - n, "%s%s", r % 8 == 1 ? "\n  " : ", ", ROWS[r].key);
    snprintf(buf + n, sizeof(buf) - n, "\n");
    return buf;
}

bool GfxLoad(GfxSettings *s, const char *path, char *err, int errSize)
{
    if (errSize > 0) err[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    int n = 0, bad = 0;
    while (fgets(line, sizeof(line), f)) {
        n++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *t = Trim(line);
        if (!*t) continue;
        char *eq = strchr(t, '=');
        if (!eq) { TraceLog(LOG_WARNING, "monolith: %s:%d: expected key = value", path, n); bad++; continue; }
        *eq = 0;
        char *key = Trim(t), *val = Trim(eq + 1);
        if (!SetKeyValue(s, key, val)) {
            TraceLog(LOG_WARNING, "monolith: %s:%d: unknown setting or bad value '%s = %s' (ignored)", path, n, key, val);
            bad++;
        }
    }
    fclose(f);
    if (bad && errSize > 0) snprintf(err, errSize, "%s: %d line%s ignored", path, bad, bad == 1 ? "" : "s");
    return true;
}

bool GfxSave(const GfxSettings *s, const char *path)
{
    const char *dir = GetDirectoryPath(path);
    if (dir && dir[0] && !DirectoryExists(dir)) MakeDirectory(dir);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    MS_QualityLevel p = GfxPreset(s);
    fprintf(f, "# monolith graphics settings: written by the game (F2 opens the settings panel).\n"
               "# preset low|medium|high|ultra sets every scene setting; keys after it override single\n"
               "# ones (custom = only the keys). Command-line flags override these for one run.\n");
    fprintf(f, "preset = %s\n", MS_QualityName(p));
    if (p == MS_QUALITY_CUSTOM) fprintf(f, "base = %s\n", MS_QualityName(s->base));
    for (int r = 0; r < ROW_COUNT; r++) {
        const Row *row = &ROWS[r];
        if (row->kind == GFX_ROW_PRESET) continue;
        bool scene = row->off < OFF(q) + sizeof(MS_Quality);
        if (scene && p != MS_QUALITY_CUSTOM) continue;      // the preset says it
        char v[64];
        if ((row->type == V_FLOAT || row->type == V_INT) && row->zero && GetNum(s, row) == 0.0f)
            snprintf(v, sizeof(v), "%s", strcmp(row->zero, "map's") == 0 ? "map" : row->zero);
        else if (row->type == V_INT && GetNum(s, row) == -1.0f && row->off == OFF(fpsCap)) snprintf(v, sizeof(v), "monitor");
        else if (row->type == V_FLOAT) snprintf(v, sizeof(v), "%g", GetNum(s, row));
        else if (row->type == V_INT) snprintf(v, sizeof(v), "%d", *(const int *)CField(s, row));
        else GfxRowFormat(s, r, v, sizeof(v));
        if (row->type == V_ENUM && row->nameBase == -1 && strcmp(v, "map's") == 0) snprintf(v, sizeof(v), "map");
        fprintf(f, "%s = %s\n", row->key, v);
    }
    return fclose(f) == 0;
}

// #endregion
