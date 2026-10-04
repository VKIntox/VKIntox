// Material 3 Expressive theming — implementation.
//
// scheme generation follows material-color-utilities: seed → HCT, variant picks
// each palette's chroma/hue, roles read off a palette at a tone. the role→tone
// table is transcribed from material-web `_md-sys-color*.scss` (v34.0.21).

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS   // ImVec2/ImVec4 courtesy operators, as the other .cc files do
#endif
#include "imgui_m3.h"
#include "imgui_m3_color.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

using namespace ImGuiM3Palette;

// Role names, kebab-case to match the `.colors` file and Matugen's scheme keys.

static const char* g_role_names[ImGuiM3Role_COUNT] = {
    "primary",
    "on-primary",
    "primary-container",
    "on-primary-container",
    "inverse-primary",
    "secondary",
    "on-secondary",
    "secondary-container",
    "on-secondary-container",
    "tertiary",
    "on-tertiary",
    "tertiary-container",
    "on-tertiary-container",
    "error",
    "on-error",
    "error-container",
    "on-error-container",
    "background",
    "on-background",
    "surface",
    "on-surface",
    "surface-variant",
    "on-surface-variant",
    "surface-dim",
    "surface-bright",
    "surface-container-lowest",
    "surface-container-low",
    "surface-container",
    "surface-container-high",
    "surface-container-highest",
    "inverse-surface",
    "inverse-on-surface",
    "inverse-surface-variant",
    "outline",
    "outline-variant",
    "shadow",
    "scrim",
    "surface-tint",
    "primary-fixed",
    "primary-fixed-dim",
    "on-primary-fixed",
    "on-primary-fixed-variant",
    "secondary-fixed",
    "secondary-fixed-dim",
    "on-secondary-fixed",
    "on-secondary-fixed-variant",
    "tertiary-fixed",
    "tertiary-fixed-dim",
    "on-tertiary-fixed",
    "on-tertiary-fixed-variant",
};


// The role that sits "on" each container, used by ImGuiM3OnColor().
static const ImGuiM3Role g_on_role_of[ImGuiM3Role_COUNT] = {
    ImGuiM3Role_OnPrimary,
    ImGuiM3Role_Primary,
    ImGuiM3Role_OnPrimaryContainer,
    ImGuiM3Role_PrimaryContainer,
    ImGuiM3Role_InverseSurface,
    ImGuiM3Role_OnSecondary,
    ImGuiM3Role_Secondary,
    ImGuiM3Role_OnSecondaryContainer,
    ImGuiM3Role_SecondaryContainer,
    ImGuiM3Role_OnTertiary,
    ImGuiM3Role_Tertiary,
    ImGuiM3Role_OnTertiaryContainer,
    ImGuiM3Role_TertiaryContainer,
    ImGuiM3Role_OnError,
    ImGuiM3Role_Error,
    ImGuiM3Role_OnErrorContainer,
    ImGuiM3Role_ErrorContainer,
    ImGuiM3Role_OnBackground,
    ImGuiM3Role_Background,
    ImGuiM3Role_OnSurface,
    ImGuiM3Role_Surface,
    ImGuiM3Role_OnSurfaceVariant,
    ImGuiM3Role_SurfaceVariant,
    ImGuiM3Role_OnSurface,
    ImGuiM3Role_OnSurface,
    ImGuiM3Role_OnSurface,
    ImGuiM3Role_OnSurface,
    ImGuiM3Role_OnSurface,
    ImGuiM3Role_OnSurface,
    ImGuiM3Role_OnSurface,
    ImGuiM3Role_InverseOnSurface,
    ImGuiM3Role_InverseSurface,
    ImGuiM3Role_InverseOnSurface,
    ImGuiM3Role_OnSurfaceVariant,
    ImGuiM3Role_Outline,
    ImGuiM3Role_Surface,
    ImGuiM3Role_Surface,
    ImGuiM3Role_Surface,
    ImGuiM3Role_OnPrimaryFixed,
    ImGuiM3Role_OnPrimaryFixedVariant,
    ImGuiM3Role_PrimaryFixed,
    ImGuiM3Role_PrimaryFixedDim,
    ImGuiM3Role_OnSecondaryFixed,
    ImGuiM3Role_OnSecondaryFixedVariant,
    ImGuiM3Role_SecondaryFixed,
    ImGuiM3Role_SecondaryFixedDim,
    ImGuiM3Role_OnTertiaryFixed,
    ImGuiM3Role_OnTertiaryFixedVariant,
    ImGuiM3Role_TertiaryFixed,
    ImGuiM3Role_TertiaryFixedDim,
};


// Palette indices and the role→tone table.

enum PaletteIdx
{
    Pal_Primary = 0,
    Pal_Secondary,
    Pal_Tertiary,
    Pal_Neutral,
    Pal_NeutralVariant,
    Pal_Error,
    Pal_COUNT
};

struct RoleTone
{
    uint8_t palette;
    // [mode][contrast]: 0 = light, 1 = dark; contrast 0=std 1=med 2=high
    uint8_t tone[2][3];
};

static const RoleTone g_role_tone[ImGuiM3Role_COUNT] = {
    /* primary,                      */ { Pal_Primary,         {{40, 30, 20}, {80,  90, 95}}},
    /* on-primary,                   */ { Pal_Primary,         {{100, 100, 100}, {20,  10,  0}}},
    /* primary-container,            */ { Pal_Primary,         {{90, 40, 30}, {30,  60, 80}}},
    /* on-primary-container,         */ { Pal_Primary,         {{30, 100, 100}, {90,   0,  0}}},
    /* inverse-primary,              */ { Pal_Primary,         {{80, 80, 80}, {40,  30, 20}}},
    /* secondary,                    */ { Pal_Secondary,       {{40, 30, 20}, {80,  90, 95}}},
    /* on-secondary,                 */ { Pal_Secondary,       {{100, 100, 100}, {20,  10,  0}}},
    /* secondary-container,          */ { Pal_Secondary,       {{90, 40, 30}, {30,  60, 80}}},
    /* on-secondary-container,       */ { Pal_Secondary,       {{30, 100, 100}, {90,   0,  0}}},
    /* tertiary,                     */ { Pal_Tertiary,        {{40, 30, 20}, {80,  90, 95}}},
    /* on-tertiary,                  */ { Pal_Tertiary,        {{100, 100, 100}, {20,  10,  0}}},
    /* tertiary-container,           */ { Pal_Tertiary,        {{90, 40, 30}, {30,  60, 80}}},
    /* on-tertiary-container,        */ { Pal_Tertiary,        {{30, 100, 100}, {90,   0,  0}}},
    /* error,                        */ { Pal_Error,           {{40, 30, 20}, {80,  90, 95}}},
    /* on-error,                     */ { Pal_Error,           {{100, 100, 100}, {20,  10,  0}}},
    /* error-container,              */ { Pal_Error,           {{90, 40, 30}, {30,  60, 80}}},
    /* on-error-container,           */ { Pal_Error,           {{30, 100, 100}, {90,   0,  0}}},
    /* background,                   */ { Pal_Neutral,         {{98, 98, 98}, { 6,   6,  6}}},
    /* on-background,                */ { Pal_Neutral,         {{10,  0,  0}, {90, 100, 100}}},
    /* surface,                      */ { Pal_Neutral,         {{98, 98, 98}, { 6,   6,  6}}},
    /* on-surface,                   */ { Pal_Neutral,         {{10,  0,  0}, {90, 100, 100}}},
    /* surface-variant,              */ { Pal_NeutralVariant,  {{90, 90, 90}, {30,  30, 30}}},
    /* on-surface-variant,           */ { Pal_NeutralVariant,  {{30, 20,  0}, {80,  90, 100}}},
    /* surface-dim,                  */ { Pal_Neutral,         {{87, 87, 87}, { 6,   6,  6}}},
    /* surface-bright,               */ { Pal_Neutral,         {{98, 98, 98}, {24,  24, 24}}},
    /* surface-container-lowest,     */ { Pal_Neutral,         {{100, 100, 100}, { 4,   4,  4}}},
    /* surface-container-low,        */ { Pal_Neutral,         {{96, 96, 96}, {10,  10, 10}}},
    /* surface-container,            */ { Pal_Neutral,         {{94, 94, 94}, {12,  12, 12}}},
    /* surface-container-high,       */ { Pal_Neutral,         {{92, 92, 92}, {17,  17, 17}}},
    /* surface-container-highest,    */ { Pal_Neutral,         {{90, 90, 90}, {22,  22, 22}}},
    /* inverse-surface,              */ { Pal_Neutral,         {{20, 20, 20}, {90,  90, 90}}},
    /* inverse-on-surface,           */ { Pal_Neutral,         {{95, 100, 100}, {20,  10,  0}}},
    /* inverse-surface-variant,      */ { Pal_NeutralVariant,  {{90, 90, 90}, {30,  30, 30}}},
    /* outline,                      */ { Pal_NeutralVariant,  {{50, 30, 20}, {60,  70, 95}}},
    /* outline-variant,              */ { Pal_NeutralVariant,  {{80, 50, 30}, {30,  60, 80}}},
    /* shadow,                       */ { Pal_Neutral,         {{0,  0,  0}, { 0,   0,  0}}},
    /* scrim,                        */ { Pal_Neutral,         {{0,  0,  0}, { 0,   0,  0}}},
    /* surface-tint,                 */ { Pal_Primary,         {{40, 30, 20}, {80,  90, 95}}},
    /* primary-fixed,                */ { Pal_Primary,         {{90, 40, 30}, {90,  90, 90}}},
    /* primary-fixed-dim,            */ { Pal_Primary,         {{80, 30, 20}, {80,  80, 80}}},
    /* on-primary-fixed,             */ { Pal_Primary,         {{10, 100, 100}, {10,   0,  0}}},
    /* on-primary-fixed-variant,     */ { Pal_Primary,         {{30, 100, 100}, {30,  20,  0}}},
    /* secondary-fixed,              */ { Pal_Secondary,       {{90, 40, 30}, {90,  90, 90}}},
    /* secondary-fixed-dim,          */ { Pal_Secondary,       {{80, 30, 20}, {80,  80, 80}}},
    /* on-secondary-fixed,           */ { Pal_Secondary,       {{10, 100, 100}, {10,   0,  0}}},
    /* on-secondary-fixed-variant,   */ { Pal_Secondary,       {{30, 100, 100}, {30,  20,  0}}},
    /* tertiary-fixed,               */ { Pal_Tertiary,        {{90, 40, 30}, {90,  90, 90}}},
    /* tertiary-fixed-dim,           */ { Pal_Tertiary,        {{80, 30, 20}, {80,  80, 80}}},
    /* on-tertiary-fixed,            */ { Pal_Tertiary,        {{10, 100, 100}, {10,   0,  0}}},
    /* on-tertiary-fixed-variant,    */ { Pal_Tertiary,        {{30, 100, 100}, {30,  20,  0}}},
};


static const char* g_variant_names[ImGuiM3Variant_COUNT] = {
    "tonal-spot", "expressive", "vibrant", "neutral", "monochrome", "fidelity", "content"
};

static const char* g_contrast_names[ImGuiM3Contrast_COUNT] = { "standard", "medium", "high" };

const char* ImGuiM3RoleName(ImGuiM3Role role)
{
    if (role < 0 || role >= ImGuiM3Role_COUNT)
        return "";
    return g_role_names[role];
}

ImGuiM3Role ImGuiM3RoleFromName(const char* name)
{
    if (!name)
        return ImGuiM3Role_COUNT;
    for (int i = 0; i < ImGuiM3Role_COUNT; i++)
        if (strcmp(g_role_names[i], name) == 0)
            return (ImGuiM3Role)i;
    return ImGuiM3Role_COUNT;
}

const char* ImGuiM3VariantName(ImGuiM3Variant v)
{
    return (v >= 0 && v < ImGuiM3Variant_COUNT) ? g_variant_names[v] : "tonal-spot";
}

ImGuiM3Variant ImGuiM3VariantFromName(const char* name)
{
    if (!name)
        return ImGuiM3Variant_COUNT;
    for (int i = 0; i < ImGuiM3Variant_COUNT; i++)
        if (strcmp(g_variant_names[i], name) == 0)
            return (ImGuiM3Variant)i;
    // Accept the camel-case spellings Matugen's config uses.
    if (strcmp(name, "TonalSpot") == 0)   return ImGuiM3Variant_TonalSpot;
    if (strcmp(name, "Expressive") == 0)  return ImGuiM3Variant_Expressive;
    if (strcmp(name, "Vibrant") == 0)     return ImGuiM3Variant_Vibrant;
    if (strcmp(name, "Neutral") == 0)     return ImGuiM3Variant_Neutral;
    if (strcmp(name, "Monochrome") == 0)  return ImGuiM3Variant_Monochrome;
    if (strcmp(name, "Fidelity") == 0)    return ImGuiM3Variant_Fidelity;
    if (strcmp(name, "Content") == 0)     return ImGuiM3Variant_Content;
    return ImGuiM3Variant_COUNT;
}

const char* ImGuiM3ContrastName(ImGuiM3Contrast c)
{
    return (c >= 0 && c < ImGuiM3Contrast_COUNT) ? g_contrast_names[c] : "standard";
}

ImGuiM3Contrast ImGuiM3ContrastFromName(const char* name)
{
    if (!name)
        return ImGuiM3Contrast_COUNT;
    for (int i = 0; i < ImGuiM3Contrast_COUNT; i++)
        if (strcmp(g_contrast_names[i], name) == 0)
            return (ImGuiM3Contrast)i;
    return ImGuiM3Contrast_COUNT;
}

// Colour helpers.

static ImVec4 Vec4FromArgb(uint32_t argb)
{
    return ImVec4((float)redFromArgb(argb) / 255.0f, (float)greenFromArgb(argb) / 255.0f, (float)blueFromArgb(argb) / 255.0f,
                  (float)alphaFromArgb(argb) / 255.0f);
}

// Applies imgui's global style alpha the same way GetColorU32() does, so M3
// content honours PushStyleVar(ImGuiStyleVar_Alpha). Without this a view faded
// with that style var still drew at full opacity and overlapped whatever it
// was supposed to be covering.
static ImU32 PackU32(const ImVec4& c)
{
    const float globalAlpha = ImGui::GetCurrentContext()
        ? ImClamp(ImGui::GetStyle().Alpha, 0.0f, 1.0f) : 1.0f;
    const float alpha = ImSaturate(c.w) * globalAlpha;
    const ImU32 r = (ImU32)clampInt(0, 255, (int)(ImSaturate(c.x) * 255.0f + 0.5f));
    const ImU32 g = (ImU32)clampInt(0, 255, (int)(ImSaturate(c.y) * 255.0f + 0.5f));
    const ImU32 b = (ImU32)clampInt(0, 255, (int)(ImSaturate(c.z) * 255.0f + 0.5f));
    const ImU32 a = (ImU32)clampInt(0, 255, (int)(alpha * 255.0f + 0.5f));
    return (a << 24) | (b << 16) | (g << 8) | r;
}

// Flattens `over` onto `base` using `over`'s alpha.
static ImVec4 Over(const ImVec4& base, const ImVec4& over)
{
    const float a = ImSaturate(over.w);
    return ImVec4(base.x * (1.0f - a) + over.x * a, base.y * (1.0f - a) + over.y * a, base.z * (1.0f - a) + over.z * a, base.w);
}

static ImVec4 WithAlpha(const ImVec4& c, float a)
{
    return ImVec4(c.x, c.y, c.z, a);
}

// Variant → palette rules, transcribed from material-color-utilities' 2021
// delegate, the same set Matugen applies (Aiving/material-colors).

static double RotatedHue(double source_hue, const double breakpoints[], const double rotations[], int count)
{
    double rotation = source_hue;
    for (int i = 0; i < count - 1; i++)
        if (source_hue >= breakpoints[i] && source_hue < breakpoints[i + 1])
        {
            rotation = rotations[i];
            break;
        }
    return sanitizeDegrees(source_hue + rotation);
}

struct SchemePalettes
{
    TonalPalette palettes[Pal_COUNT];
};

static void BuildPalettes(uint32_t source_argb, ImGuiM3Variant variant, bool dark, SchemePalettes& out)
{
    const Cam16 cam = Cam16::FromInt(source_argb);
    const double hue = cam.hue;
    const double chroma = cam.chroma;
    const int mode = dark ? 1 : 0;

    static const double kExpSecBreak[] = {0, 21, 51, 121, 151, 191, 271, 321, 360};
    static const double kExpSecRot[]   = {45, 95, 45, 20, 45, 90, 45, 45, 45};
    static const double kExpTerBreak[] = {0, 21, 51, 121, 151, 191, 271, 321, 360};
    static const double kExpTerRot[]   = {120, 120, 20, 45, 20, 15, 20, 120, 120};
    static const double kVibSecBreak[] = {0, 41, 61, 101, 131, 181, 251, 301, 360};
    static const double kVibSecRot[]   = {18, 15, 10, 12, 15, 18, 15, 12, 12};
    static const double kVibTerBreak[] = {0, 41, 61, 101, 131, 181, 251, 301, 360};
    static const double kVibTerRot[]   = {35, 30, 20, 25, 30, 35, 30, 25, 25};

    const int nSec = (int)(sizeof(kExpSecBreak) / sizeof(double[1]));
    const int nTer = (int)(sizeof(kExpTerBreak) / sizeof(double[1]));
    const int nVib = (int)(sizeof(kVibSecBreak) / sizeof(double[1]));

    double p1_chroma = chroma > 48.0 ? chroma : 48.0;
    double p1_hue = hue;
    double p2_chroma = 16.0, p2_hue = hue;
    double p3_chroma = 24.0, p3_hue = sanitizeDegrees(hue + 60.0);
    double n1_chroma = 4.0, n1_hue = hue;
    double n2_chroma = 8.0, n2_hue = hue;

    switch (variant)
    {
    case ImGuiM3Variant_Content:
    case ImGuiM3Variant_Fidelity:
        p1_chroma = chroma;            p1_hue = hue;
        p2_chroma = std::max(chroma - 32.0, chroma * 0.5); p2_hue = hue;
        p3_chroma = chroma / 3.0;      p3_hue = sanitizeDegrees(hue + 60.0);
        n1_chroma = chroma / 12.0;     n1_hue = hue;
        n2_chroma = std::min(chroma / 6.0, 8.0); n2_hue = hue;
        break;
    case ImGuiM3Variant_Monochrome:
        p1_chroma = p2_chroma = p3_chroma = n1_chroma = n2_chroma = 0.0;
        break;
    case ImGuiM3Variant_Neutral:
        p1_chroma = 12.0; p2_chroma = 8.0; p3_chroma = 16.0; n1_chroma = 2.0; n2_chroma = 2.0;
        break;
    case ImGuiM3Variant_TonalSpot:
        p1_chroma = 36.0;
        n1_chroma = 6.0;
        n2_chroma = 8.0;
        break;
    case ImGuiM3Variant_Expressive:
        // 2025 keeps the source hue for primary. the 2021 rule rotated it +240°
        // and turned a violet seed into a cyan UI.
        p1_hue = hue;                               p1_chroma = dark ? 36.0 : 48.0;
        p2_hue = RotatedHue(hue, kExpSecBreak, kExpSecRot, nSec);  p2_chroma = dark ? 16.0 : 24.0;
        p3_hue = RotatedHue(hue, kExpTerBreak, kExpTerRot, nTer);  p3_chroma = 48.0;
        n1_hue = hue;                               n1_chroma = dark ? 14.0 : 18.0;
        n2_hue = hue;                               n2_chroma = 12.0;
        break;
    case ImGuiM3Variant_Vibrant:
        p1_chroma = 200.0;
        p2_hue = RotatedHue(hue, kVibSecBreak, kVibSecRot, nVib);  p2_chroma = 24.0;
        p3_hue = RotatedHue(hue, kVibTerBreak, kVibTerRot, nVib);  p3_chroma = 32.0;
        n1_chroma = 10.0;
        n2_chroma = 12.0;
        break;
    default:
        break;
    }
    IM_UNUSED(mode);

    out.palettes[Pal_Primary] = TonalPalette(p1_hue, p1_chroma);
    out.palettes[Pal_Secondary] = TonalPalette(p2_hue, p2_chroma);
    out.palettes[Pal_Tertiary] = TonalPalette(p3_hue, p3_chroma);
    out.palettes[Pal_Neutral] = TonalPalette(n1_hue, n1_chroma);
    out.palettes[Pal_NeutralVariant] = TonalPalette(n2_hue, n2_chroma);
    out.palettes[Pal_Error] = TonalPalette(25.0, 84.0);
}

// Theme state.

struct ThemeState
{
    bool                       initialised = false;
    uint32_t                   source = 0xFFE91E63;
    ImGuiM3Variant             variant = ImGuiM3Variant_Expressive;
    ImGuiM3Contrast            contrast = ImGuiM3Contrast_Standard;
    bool                       dark = true;
    ImVec4                     colors[ImGuiM3Role_COUNT];
    // per-role overrides from the .colors file; role_flags marks which are pinned.
    uint64_t                   role_flags[2] = {0, 0};
    ImVec4                     overrides[ImGuiM3Role_COUNT];
    ImGuiM3Metrics             metrics;
    // non-color tokens from [expressive], with their dirty flags.
    bool                       density_dirty = false;
    bool                       shape_scale_dirty = false;
    std::string                file_path;
    std::string                last_reload_message;
    std::string                last_error;
    bool                       reloaded = false;
    uint64_t                   file_mtime_ns = 0;
    uint64_t                   file_size = 0;
    bool                       file_existed = false;
    double                     last_poll_time = 0.0;
    // live editor state. RGBA so ColorEdit4 doesn't read past the array.
    bool                       picker_open = false;
    float                      source_rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

static ThemeState g_theme;

// Material Symbols face, cleared by the shutdown hook alongside the theme.
static ImFont* g_icon_font = nullptr;

// 2025 reference palette for #6750A4, so the first run is the real baseline.
struct PresetHexes { const char* role; uint32_t light; uint32_t dark; };

static const PresetHexes g_baseline_preset[ImGuiM3Role_COUNT] = {
    { "primary", 0x6750A4, 0xD0BCFF },
    { "on-primary", 0xFFFFFF, 0x381E72 },
    { "primary-container", 0xEADDFF, 0x4F378B },
    { "on-primary-container", 0x4F378B, 0xEADDFF },
    { "inverse-primary", 0xD0BCFF, 0x6750A4 },
    { "secondary", 0x625B71, 0xCCC2DC },
    { "on-secondary", 0xFFFFFF, 0x332D41 },
    { "secondary-container", 0xE8DEF8, 0x4A4458 },
    { "on-secondary-container", 0x4A4458, 0xE8DEF8 },
    { "tertiary", 0x7D5260, 0xEFB8C8 },
    { "on-tertiary", 0xFFFFFF, 0x492532 },
    { "tertiary-container", 0xFFD8E4, 0x633B48 },
    { "on-tertiary-container", 0x633B48, 0xFFD8E4 },
    { "error", 0xB3261E, 0xF2B8B5 },
    { "on-error", 0xFFFFFF, 0x601410 },
    { "error-container", 0xF9DEDC, 0x8C1D18 },
    { "on-error-container", 0x8C1D18, 0xF9DEDC },
    { "background", 0xFEF7FF, 0x141218 },
    { "on-background", 0x1D1B20, 0xE6E0E9 },
    { "surface", 0xFEF7FF, 0x141218 },
    { "on-surface", 0x1D1B20, 0xE6E0E9 },
    { "surface-variant", 0xE7E0EC, 0x49454F },
    { "on-surface-variant", 0x49454F, 0xCAC4D0 },
    { "surface-dim", 0xDED8E1, 0x141218 },
    { "surface-bright", 0xFEF7FF, 0x3B383E },
    { "surface-container-lowest", 0xFFFFFF, 0x0F0D13 },
    { "surface-container-low", 0xF7F2FA, 0x1D1B20 },
    { "surface-container", 0xF3EDF7, 0x211F26 },
    { "surface-container-high", 0xECE6F0, 0x2B2930 },
    { "surface-container-highest", 0xE6E0E9, 0x36343B },
    { "inverse-surface", 0x322F35, 0xE6E0E9 },
    { "inverse-on-surface", 0xF5EFF7, 0x322F35 },
    { "inverse-surface-variant", 0xE7E0EC, 0x49454F },
    { "outline", 0x79747E, 0x938F99 },
    { "outline-variant", 0xCAC4D0, 0x49454F },
    { "shadow", 0x000000, 0x000000 },
    { "scrim", 0x000000, 0x000000 },
    { "surface-tint", 0x6750A4, 0xD0BCFF },
    { "primary-fixed", 0xEADDFF, 0xEADDFF },
    { "primary-fixed-dim", 0xD0BCFF, 0xD0BCFF },
    { "on-primary-fixed", 0x21005D, 0x21005D },
    { "on-primary-fixed-variant", 0x4F378B, 0x4F378B },
    { "secondary-fixed", 0xE8DEF8, 0xE8DEF8 },
    { "secondary-fixed-dim", 0xCCC2DC, 0xCCC2DC },
    { "on-secondary-fixed", 0x1D192B, 0x1D192B },
    { "on-secondary-fixed-variant", 0x4A4458, 0x4A4458 },
    { "tertiary-fixed", 0xFFD8E4, 0xFFD8E4 },
    { "tertiary-fixed-dim", 0xEFB8C8, 0xEFB8C8 },
    { "on-tertiary-fixed", 0x31111D, 0x31111D },
    { "on-tertiary-fixed-variant", 0x633B48, 0x633B48 },
};

static bool RoleOverridden(ImGuiM3Role role)
{
    return (g_theme.role_flags[role >> 6] & (1ull << (role & 63))) != 0;
}

static void SetRoleOverridden(ImGuiM3Role role, bool on)
{
    const uint64_t bit = 1ull << (role & 63);
    if (on)
        g_theme.role_flags[role >> 6] |= bit;
    else
        g_theme.role_flags[role >> 6] &= ~bit;
}

static void ResolveTheme()
{
    if (!g_theme.initialised)
        return;

    // exact 2025 baseline hexes for the preset; other sources derive, file roles win.
    if (g_theme.source == 0xFF6750A4 && g_theme.variant == ImGuiM3Variant_TonalSpot && g_theme.contrast == ImGuiM3Contrast_Standard)
    {
        for (int i = 0; i < ImGuiM3Role_COUNT; i++)
        {
            const uint32_t argb = g_theme.dark ? g_baseline_preset[i].dark : g_baseline_preset[i].light;
            g_theme.colors[i] = Vec4FromArgb(argb);
        }
    }
    else
    {
        SchemePalettes palettes;
        BuildPalettes(g_theme.source, g_theme.variant, g_theme.dark, palettes);
        const int mode = g_theme.dark ? 1 : 0;
        const int contrast = (int)g_theme.contrast;
        for (int i = 0; i < ImGuiM3Role_COUNT; i++)
        {
            const RoleTone& rt = g_role_tone[i];
            const uint32_t argb = palettes.palettes[rt.palette].tone(rt.tone[mode][contrast]);
            g_theme.colors[i] = Vec4FromArgb(argb);
        }
    }

    // file roles win over the derivation.
    for (int i = 0; i < ImGuiM3Role_COUNT; i++)
        if (RoleOverridden((ImGuiM3Role)i))
            g_theme.colors[i] = g_theme.overrides[i];

    // keep source_rgba in lock-step with the seed so the picker can't drift.
    g_theme.source_rgba[0] = (float)redFromArgb(g_theme.source) / 255.0f;
    g_theme.source_rgba[1] = (float)greenFromArgb(g_theme.source) / 255.0f;
    g_theme.source_rgba[2] = (float)blueFromArgb(g_theme.source) / 255.0f;
    g_theme.source_rgba[3] = 1.0f;
}

// Non-color token parsing: every entry maps a `.colors` key to a metrics field.

static void ApplyMetricToken(const char* key, const char* value, ImGuiM3Metrics& m, std::string& err)
{
    // strtod tells a real zero from an unparseable one by where it stops, so a
    // written "0.000000" parses instead of being rejected as no number.
    char* end = nullptr;
    const double parsed = strtod(value, &end);
    while (*end == ' ' || *end == '\t')
        end++;
    if (end == value || *end != '\0')
    {
        err = std::string("expected a number for '") + key + "', got '" + value + "'";
        return;
    }
    const float v = (float)parsed;

#define MET(field) if (strcmp(key, #field) == 0) { m.field = v; return; }
    MET(density)
    MET(shape_scale)
    MET(corner_none) MET(corner_xs) MET(corner_s) MET(corner_m) MET(corner_l)
    MET(corner_l_increased) MET(corner_xl) MET(corner_xl_increased) MET(corner_xxl)
    MET(button_height_xsmall) MET(button_height_small) MET(button_height_default)
    MET(button_height_medium) MET(button_height_large)
    MET(button_icon_xsmall) MET(button_icon_small) MET(button_icon_default)
    MET(button_icon_medium) MET(button_icon_large)
    MET(button_outline_width) MET(button_padding_x)
    MET(icon_button_size) MET(icon_button_size_medium) MET(icon_button_icon)
    MET(fab_size) MET(fab_size_small) MET(fab_size_large) MET(fab_icon) MET(fab_icon_large)
    MET(card_radius) MET(card_outline_width)
    MET(chip_height) MET(chip_radius) MET(chip_icon)
    MET(dialog_radius) MET(dialog_min_width)
    MET(menu_radius) MET(menu_item_height)
    MET(snackbar_radius) MET(snackbar_height_1) MET(snackbar_height_2)
    MET(search_bar_height)
    MET(nav_bar_height) MET(nav_rail_width) MET(nav_indicator_width) MET(nav_indicator_height)
    MET(nav_rail_indicator_width) MET(badge_size) MET(badge_size_large)
    MET(switch_track_width) MET(switch_track_height) MET(switch_track_outline)
    MET(switch_handle) MET(switch_handle_inset) MET(switch_handle_with_icon)
    MET(switch_pressed_track_width) MET(switch_pressed_handle)
    MET(checkbox_size) MET(checkbox_radius) MET(checkbox_outline) MET(checkbox_icon)
    MET(radio_size) MET(radio_inner)
    MET(slider_track_height) MET(slider_handle_width) MET(slider_handle_height)
    MET(slider_handle_padding) MET(slider_stop_indicator)
    MET(slider_handle_width_pressed) MET(slider_track_inside_corner)
    MET(tab_height) MET(tab_height_with_icon) MET(tab_icon) MET(tab_indicator_height) MET(tab_indicator_radius)
    MET(text_field_height) MET(text_field_top_radius) MET(text_field_outline)
    MET(text_field_active_indicator) MET(text_field_icon)
    MET(list_item_height_1) MET(list_item_height_2) MET(list_item_height_3)
    MET(list_item_radius) MET(list_item_radius_expressive)
    MET(list_leading_icon) MET(list_leading_icon_expressive) MET(list_avatar) MET(list_image)
    MET(divider_thickness) MET(progress_track_height)
    MET(state_layer_size) MET(state_layer_target_size)
    MET(focus_indicator_thickness) MET(focus_indicator_outer_offset) MET(focus_indicator_inner_offset)
    MET(scrim_opacity)
    MET(key_shadow_opacity) MET(ambient_shadow_opacity)
    MET(spring_spatial_fast_damping) MET(spring_spatial_fast_stiffness)
    MET(spring_spatial_default_damping) MET(spring_spatial_default_stiffness)
    MET(spring_spatial_slow_damping) MET(spring_spatial_slow_stiffness)
    MET(spring_effects_fast_damping) MET(spring_effects_fast_stiffness)
    MET(spring_effects_default_damping) MET(spring_effects_default_stiffness)
    MET(spring_effects_slow_damping) MET(spring_effects_slow_stiffness)
    MET(duration_short1) MET(duration_short2) MET(duration_short3) MET(duration_short4)
    MET(duration_medium1) MET(duration_medium2) MET(duration_medium3) MET(duration_medium4)
    MET(duration_long1) MET(duration_long2) MET(duration_long3) MET(duration_long4)
    MET(duration_extra_long1) MET(duration_extra_long2) MET(duration_extra_long3) MET(duration_extra_long4)
#undef MET

    err = std::string("unknown token '") + key + "'";
}

static std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::string Lowercase(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

// #rgb, #rgba, #rrggbb, #rrggbbaa, 0x-prefixed, 0..255 triplets.
static bool ParseColor(const std::string& text, ImVec4* out)
{
    std::string s = Trim(text);
    if (s.empty())
        return false;
    if (s[0] == '#')
        s = s.substr(1);
    else if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s = s.substr(2);

    if (s.size() != 3 && s.size() != 4 && s.size() != 6 && s.size() != 8)
        return false;
    for (char c : s)
        if (!std::isxdigit((unsigned char)c))
            return false;

    auto nib = [](char c) -> int
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
    };
    auto byte = [&](size_t i) { return nib(s[i]) * 16 + nib(s[i + 1]); };

    if (s.size() == 3)
    {
        out->x = (nib(s[0]) * 17) / 255.0f;
        out->y = (nib(s[1]) * 17) / 255.0f;
        out->z = (nib(s[2]) * 17) / 255.0f;
        out->w = 1.0f;
        return true;
    }
    if (s.size() == 4)
    {
        out->x = (nib(s[0]) * 17) / 255.0f;
        out->y = (nib(s[1]) * 17) / 255.0f;
        out->z = (nib(s[2]) * 17) / 255.0f;
        out->w = (nib(s[3]) * 17) / 255.0f;
        return true;
    }
    out->x = byte(0) / 255.0f;
    out->y = byte(2) / 255.0f;
    out->z = byte(4) / 255.0f;
    out->w = s.size() == 8 ? byte(6) / 255.0f : 1.0f;
    return true;
}

// never fails hard: a bad line goes into `error` and is skipped, so one typo
// can't black out the overlay.
static void LoadThemeFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
        return;

    // reset to defaults first, so deleting a line really removes the override.
    g_theme.role_flags[0] = 0;
    g_theme.role_flags[1] = 0;
    g_theme.metrics = ImGuiM3Metrics();
    g_theme.source = 0xFFE91E63;
    g_theme.variant = ImGuiM3Variant_Expressive;
    g_theme.contrast = ImGuiM3Contrast_Standard;
    g_theme.dark = true;
    g_theme.last_error.clear();

    bool in_metrics_section = false;
    int applied = 0;
    int line_no = 0;
    std::string line;
    std::string err;

    // role lines are applied after the whole file is read, so a generated
    // snapshot can be told from a hand-edited pin.
    ImVec4 file_roles[ImGuiM3Role_COUNT];
    bool   file_role_seen[ImGuiM3Role_COUNT] = {false};

    while (std::getline(file, line))
    {
        line_no++;
        // '#' is both colour prefix and comment marker, so context decides: leading
        // is a comment, before '=' trails a comment, after '=' is a value.
        const size_t hash = line.find('#');
        const size_t first_nonspace = line.find_first_not_of(" \t");
        const size_t equals_before_hash = line.find('=');
        if (hash != std::string::npos && hash == first_nonspace)
            continue;
        if (hash != std::string::npos &&
            (equals_before_hash == std::string::npos || hash < equals_before_hash))
            line = line.substr(0, hash);
        line = Trim(line);
        if (line.empty())
            continue;

        if (line[0] == '[')
        {
            in_metrics_section = (line == "[expressive]");
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            err += "line " + std::to_string(line_no) + ": expected key = value\n";
            continue;
        }
        const std::string key = Lowercase(Trim(line.substr(0, eq)));
        const std::string value = Trim(line.substr(eq + 1));

        if (in_metrics_section)
        {
            std::string metric_err;
            ApplyMetricToken(key.c_str(), value.c_str(), g_theme.metrics, metric_err);
            if (!metric_err.empty())
                err += "line " + std::to_string(line_no) + ": " + metric_err + "\n";
            else
                applied++;
            continue;
        }

        if (key == "variant")
        {
            const ImGuiM3Variant v = ImGuiM3VariantFromName(Lowercase(value).c_str());
            if (v == ImGuiM3Variant_COUNT)
                err += "line " + std::to_string(line_no) + ": unknown variant '" + value + "'\n";
            else
            {
                g_theme.variant = v;
                applied++;
            }
        }
        else if (key == "generated")
        {
            // accepted for file compatibility; it no longer suppresses role overrides.
            applied++;
        }
        else if (key == "contrast")
        {
            const ImGuiM3Contrast c = ImGuiM3ContrastFromName(Lowercase(value).c_str());
            if (c == ImGuiM3Contrast_COUNT)
                err += "line " + std::to_string(line_no) + ": unknown contrast '" + value + "'\n";
            else
            {
                g_theme.contrast = c;
                applied++;
            }
        }
        else if (key == "dark" || key == "mode")
        {
            const std::string v = Lowercase(value);
            if (v == "dark")        { g_theme.dark = true; applied++; }
            else if (v == "light")  { g_theme.dark = false; applied++; }
            else                    { err += "line " + std::to_string(line_no) + ": dark must be true/false or dark/light\n"; }
        }
        else if (key == "source" || key == "seed" || key == "source_color" || key == "source-color")
        {
            ImVec4 c(0, 0, 0, 1);
            if (!ParseColor(value, &c))
                err += "line " + std::to_string(line_no) + ": '" + value + "' is not a colour\n";
            else
            {
                g_theme.source = argbFromRgb((int)(c.x * 255.0f + 0.5f), (int)(c.y * 255.0f + 0.5f), (int)(c.z * 255.0f + 0.5f));
                applied++;
            }
        }
        else if (key == "density" || key == "shape_scale" || key == "shape-scale")
        {
            // also accepted at top level, since these are the ones poked by hand.
            std::string metric_err;
            ApplyMetricToken(key.c_str(), value.c_str(), g_theme.metrics, metric_err);
            if (!metric_err.empty())
                err += "line " + std::to_string(line_no) + ": " + metric_err + "\n";
            else
                applied++;
        }
        else
        {
            ImGuiM3Role role = ImGuiM3RoleFromName(key.c_str());
            ImVec4 c(0, 0, 0, 1);
            if (role == ImGuiM3Role_COUNT)
            {
                // Accept Matugen's underscore spelling too.
                std::string underscored = key;
                for (char& ch : underscored)
                    if (ch == '_') ch = '-';
                role = ImGuiM3RoleFromName(underscored.c_str());
            }
            if (role == ImGuiM3Role_COUNT)
            {
                err += "line " + std::to_string(line_no) + ": unknown token '" + key + "'\n";
                continue;
            }
            if (!ParseColor(value, &c))
            {
                err += "line " + std::to_string(line_no) + ": '" + value + "' is not a colour\n";
                continue;
            }
            file_roles[role] = c;
            file_role_seen[role] = true;
            applied++;
        }
    }

    // reconcile the file's role lines against the derivation: an equal line stays
    // derived, a differing one is pinned. keeps write→reload idempotent without
    // dropping hand edits.
    ResolveTheme();
    for (int i = 0; i < ImGuiM3Role_COUNT; i++)
    {
        if (!file_role_seen[i])
            continue;
        if (PackU32(file_roles[i]) != PackU32(g_theme.colors[i]))
        {
            g_theme.overrides[i] = file_roles[i];
            SetRoleOverridden((ImGuiM3Role)i, true);
        }
        else
        {
            SetRoleOverridden((ImGuiM3Role)i, false);
        }
    }

    g_theme.last_error = err;
    g_theme.last_reload_message = "loaded " + std::to_string(applied) + " tokens from " + path;
    ResolveTheme();
}

// Public theme accessors.

const ImVec4& ImGuiM3Color(ImGuiM3Role role)
{
    if (!g_theme.initialised)
        ResolveTheme();
    return g_theme.colors[clampInt(0, ImGuiM3Role_COUNT - 1, (int)role)];
}

ImU32 ImGuiM3ColorU32(ImGuiM3Role role) { return PackU32(ImGuiM3Color(role)); }

ImVec4 ImGuiM3OnColor(ImGuiM3Role role)
{
    if (!g_theme.initialised)
        ResolveTheme();
    return g_theme.colors[g_on_role_of[clampInt(0, ImGuiM3Role_COUNT - 1, (int)role)]];
}

ImU32 ImGuiM3OnColorU32(ImGuiM3Role role) { return PackU32(ImGuiM3OnColor(role)); }

static const float g_state_opacity[ImGuiM3State_COUNT] = {
    0.00f,  // enabled
    0.08f,  // hover
    0.10f,  // focus
    0.10f,  // press
    0.16f,  // drag
    0.00f,  // selected is a container swap, not a state layer
    0.12f,  // disabled container
};

ImVec4 ImGuiM3StateLayer(ImGuiM3Role role, ImGuiM3State state)
{
    return WithAlpha(ImGuiM3Color(role), g_state_opacity[clampInt(0, ImGuiM3State_COUNT - 1, (int)state)]);
}

ImU32 ImGuiM3StateLayerU32(ImGuiM3Role role, ImGuiM3State state)
{
    return PackU32(ImGuiM3StateLayer(role, state));
}

ImVec4 ImGuiM3Elevate(ImGuiM3Role role, int level)
{
    level = clampInt(0, 5, level);
    const ImVec4 base = ImGuiM3Color(role);
    if (level == 0)
        return base;
    // 2025 dropped tint-per-level, but the legacy blend still nudges a role. kept
    // for callers.
    return Over(base, WithAlpha(ImGuiM3Color(ImGuiM3Role_SurfaceTint), g_theme.metrics.elevation_tint[level]));
}

const ImGuiM3Metrics& ImGuiM3GetMetrics()
{
    if (!g_theme.initialised)
        ResolveTheme();
    return g_theme.metrics;
}

bool  ImGuiM3IsDark() { return g_theme.dark; }
ImU32 ImGuiM3SourceColor() { return g_theme.source; }
ImGuiM3Variant  ImGuiM3GetVariant()  { return g_theme.variant; }
ImGuiM3Contrast ImGuiM3GetContrast() { return g_theme.contrast; }

ImGuiM3Role ImGuiM3SurfaceContainerForElevation(ImGuiM3Role role, int elevation)
{
    // maps a surface role onto the container scale for its elevation.
    const ImGuiM3Role containers[5] = {
        ImGuiM3Role_SurfaceContainerLowest, ImGuiM3Role_SurfaceContainerLow, ImGuiM3Role_SurfaceContainer,
        ImGuiM3Role_SurfaceContainerHigh, ImGuiM3Role_SurfaceContainerHighest};
    if (role != ImGuiM3Role_Surface && role != ImGuiM3Role_Background && role != ImGuiM3Role_SurfaceBright &&
        role != ImGuiM3Role_SurfaceDim)
        return role;
    return containers[clampInt(0, 4, elevation)];
}

bool ImGuiM3ConsumeReloadedFlag()
{
    const bool v = g_theme.reloaded;
    g_theme.reloaded = false;
    return v;
}

const char* ImGuiM3ConsumeReloadMessage()
{
    static std::string message;
    message = g_theme.last_reload_message;
    if (message.empty())
        return nullptr;
    return message.c_str();
}

const char* ImGuiM3GetError()
{
    return g_theme.last_error.empty() ? nullptr : g_theme.last_error.c_str();
}

// Writing the file out.

static void StatFile(const std::string& path, uint64_t* mtime_ns, uint64_t* size, bool* exists);

static void WriteU32Color(std::string& out, ImU32 col)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", (unsigned)(col & 0xFF), (unsigned)((col >> 8) & 0xFF),
             (unsigned)((col >> 16) & 0xFF));
    out += buf;
}

bool ImGuiM3WriteThemeFile(const char* path)
{    if (!path || !*path)
        return false;

    std::string out;
    out += "# VKIntox theme tokens — Material 3 Expressive.\n";
    out += "#\n";
    out += "# Every M3 color role is listed below. Values are regenerated from\n";
    out += "# `source`, `variant`, `contrast` and `dark` unless you edit a hex by\n";
    out += "# hand: a role you write is pinned, and a role you delete goes back to\n";
    out += "# being derived. This is the same flat key/value shape Matugen emits,\n";
    out += "# so you can point a Matugen template at this file.\n";
    out += "#\n";
    out += "# Editing this file while VKIntox is running reloads it live.\n";
    out += "\n";
    out += "variant  = " + std::string(ImGuiM3VariantName(g_theme.variant)) + "\n";
    out += "contrast = " + std::string(ImGuiM3ContrastName(g_theme.contrast)) + "\n";
    out += "mode     = " + std::string(g_theme.dark ? "dark" : "light") + "\n";
    out += "generated = true\n";
    out += "source   = ";
    {
        // source is 0xAARRGGBB, not IM_COL32 order, so write it byte-by-byte.
        char sbuf[16];
        snprintf(sbuf, sizeof(sbuf), "#%02x%02x%02x",
                 (unsigned)redFromArgb(g_theme.source), (unsigned)greenFromArgb(g_theme.source),
                 (unsigned)blueFromArgb(g_theme.source));
        out += sbuf;
    }
    out += "\n\n";

    for (int i = 0; i < ImGuiM3Role_COUNT; i++)
    {
        out += ImGuiM3RoleName((ImGuiM3Role)i);
        out += " = ";
        WriteU32Color(out, PackU32(g_theme.colors[i]));
        out += "\n";
    }

    const ImGuiM3Metrics& m = g_theme.metrics;
    out += "\n# Non-color tokens: shape scale, component metrics, motion springs.\n";
    out += "[expressive]\n";
    out += "density                  = " + std::to_string(m.density) + "\n";
    out += "shape_scale              = " + std::to_string(m.shape_scale) + "\n";
    out += "corner_none              = " + std::to_string(m.corner_none) + "\n";
    out += "corner_xs                = " + std::to_string(m.corner_xs) + "\n";
    out += "corner_s                 = " + std::to_string(m.corner_s) + "\n";
    out += "corner_m                 = " + std::to_string(m.corner_m) + "\n";
    out += "corner_l                 = " + std::to_string(m.corner_l) + "\n";
    out += "corner_l_increased      = " + std::to_string(m.corner_l_increased) + "\n";
    out += "corner_xl                = " + std::to_string(m.corner_xl) + "\n";
    out += "corner_xl_increased     = " + std::to_string(m.corner_xl_increased) + "\n";
    out += "corner_xxl               = " + std::to_string(m.corner_xxl) + "\n";
    out += "button_height_default    = " + std::to_string(m.button_height_default) + "\n";
    out += "button_height_medium     = " + std::to_string(m.button_height_medium) + "\n";
    out += "button_icon_default      = " + std::to_string(m.button_icon_default) + "\n";
    out += "icon_button_size         = " + std::to_string(m.icon_button_size) + "\n";
    out += "fab_size                 = " + std::to_string(m.fab_size) + "\n";
    out += "fab_icon                 = " + std::to_string(m.fab_icon) + "\n";
    out += "card_radius              = " + std::to_string(m.card_radius) + "\n";
    out += "chip_height              = " + std::to_string(m.chip_height) + "\n";
    out += "dialog_radius            = " + std::to_string(m.dialog_radius) + "\n";
    out += "menu_radius              = " + std::to_string(m.menu_radius) + "\n";
    out += "nav_bar_height           = " + std::to_string(m.nav_bar_height) + "\n";
    out += "nav_indicator_width      = " + std::to_string(m.nav_indicator_width) + "\n";
    out += "switch_track_width       = " + std::to_string(m.switch_track_width) + "\n";
    out += "switch_track_height      = " + std::to_string(m.switch_track_height) + "\n";
    out += "switch_handle            = " + std::to_string(m.switch_handle) + "\n";
    out += "slider_track_height      = " + std::to_string(m.slider_track_height) + "\n";
    out += "slider_handle_width      = " + std::to_string(m.slider_handle_width) + "\n";
    out += "slider_handle_width_pressed = " + std::to_string(m.slider_handle_width_pressed) + "\n";
    out += "slider_handle_height     = " + std::to_string(m.slider_handle_height) + "\n";
    out += "slider_handle_padding    = " + std::to_string(m.slider_handle_padding) + "\n";
    out += "slider_track_inside_corner = " + std::to_string(m.slider_track_inside_corner) + "\n";
    out += "tab_height               = " + std::to_string(m.tab_height) + "\n";
    out += "tab_indicator_height     = " + std::to_string(m.tab_indicator_height) + "\n";
    out += "text_field_height        = " + std::to_string(m.text_field_height) + "\n";
    out += "checkbox_size            = " + std::to_string(m.checkbox_size) + "\n";
    out += "checkbox_radius          = " + std::to_string(m.checkbox_radius) + "\n";
    out += "radio_size               = " + std::to_string(m.radio_size) + "\n";
    out += "divider_thickness        = " + std::to_string(m.divider_thickness) + "\n";
    out += "state_layer_size         = " + std::to_string(m.state_layer_size) + "\n";
    out += "focus_indicator_thickness= " + std::to_string(m.focus_indicator_thickness) + "\n";
    out += "scrim_opacity            = " + std::to_string(m.scrim_opacity) + "\n";
    out += "spring_spatial_fast_damping      = " + std::to_string(m.spring_spatial_fast_damping) + "\n";
    out += "spring_spatial_fast_stiffness    = " + std::to_string(m.spring_spatial_fast_stiffness) + "\n";
    out += "spring_spatial_default_damping   = " + std::to_string(m.spring_spatial_default_damping) + "\n";
    out += "spring_spatial_default_stiffness = " + std::to_string(m.spring_spatial_default_stiffness) + "\n";
    out += "spring_spatial_slow_damping      = " + std::to_string(m.spring_spatial_slow_damping) + "\n";
    out += "spring_spatial_slow_stiffness    = " + std::to_string(m.spring_spatial_slow_stiffness) + "\n";
    out += "spring_effects_fast_damping      = " + std::to_string(m.spring_effects_fast_damping) + "\n";
    out += "spring_effects_fast_stiffness    = " + std::to_string(m.spring_effects_fast_stiffness) + "\n";
    out += "spring_effects_default_damping   = " + std::to_string(m.spring_effects_default_damping) + "\n";
    out += "spring_effects_default_stiffness = " + std::to_string(m.spring_effects_default_stiffness) + "\n";
    out += "spring_effects_slow_damping      = " + std::to_string(m.spring_effects_slow_damping) + "\n";
    out += "spring_effects_slow_stiffness    = " + std::to_string(m.spring_effects_slow_stiffness) + "\n";

    // temp file + rename, so a reader never sees a half-written theme.
    const std::string tmp = std::string(path) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open())
            return false;
        f << out;
        f.close();
        if (f.fail())
        {
            f.clear();
            std::remove(tmp.c_str());
            return false;
        }
    }
    if (std::rename(tmp.c_str(), path) != 0)
    {
        std::remove(tmp.c_str());
        return false;
    }

    // adopt the new file's identity so the poll doesn't read our own write as an
    // external edit and reload it.
    uint64_t mtime = 0, size = 0;
    bool exists = false;
    StatFile(path, &mtime, &size, &exists);
    g_theme.file_mtime_ns = mtime;
    g_theme.file_size = size;
    g_theme.file_existed = exists;
    return true;
}

// Theme file location + live reload.

const char* ImGuiM3DefaultThemeFilePath()
{
    static std::string path;
    if (!path.empty())
        return path.c_str();

    if (const char* env = getenv("VKINTOX_THEME"))
    {
        if (*env)
        {
            path = env;
            return path.c_str();
        }
    }
    if (const char* xdg = getenv("XDG_CONFIG_HOME"))
    {
        if (*xdg)
            path = std::string(xdg) + "/VKIntox/theme.colors";
        else
            path = "";
    }
    else if (const char* home = getenv("HOME"))
    {
        if (*home)
            path = std::string(home) + "/.config/VKIntox/theme.colors";
        else
            path = "";
    }
    else
    {
        path = "";
    }
    return path.c_str();
}

static void StatFile(const std::string& path, uint64_t* mtime_ns, uint64_t* size, bool* exists)
{
    *exists = false;
    *mtime_ns = 0;
    *size = 0;
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return;
    *exists = true;
    *size = (uint64_t)st.st_size;
#if defined(__APPLE__)
    *mtime_ns = (uint64_t)st.st_mtimespec.tv_sec * 1000000000ull + (uint64_t)st.st_mtimespec.tv_nsec;
#elif defined(st_mtime)
    *mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
#else
    *mtime_ns = (uint64_t)st.st_mtime * 1000000000ull;
#endif
}

bool ImGuiM3SetThemeFile(const char* path)
{
    g_theme.file_path = path ? path : "";
    g_theme.initialised = true;
    ResolveTheme();

    if (g_theme.file_path.empty())
        return false;

    uint64_t mtime = 0, size = 0;
    bool exists = false;
    StatFile(g_theme.file_path, &mtime, &size, &exists);

    if (exists)
    {
        LoadThemeFile(g_theme.file_path);
        g_theme.file_existed = true;
    }
    else
    {
        // ship the preset when the file is absent, so there is always something to edit.
        ImGuiM3WriteThemeFile(g_theme.file_path.c_str());
        StatFile(g_theme.file_path, &mtime, &size, &exists);
        g_theme.file_existed = exists;
        g_theme.last_reload_message = "wrote preset theme to " + g_theme.file_path;
    }
    g_theme.file_mtime_ns = mtime;
    g_theme.file_size = size;
    ResolveTheme();
    ImGuiM3ApplyToStyle(1.0f);
    return exists;
}

const char* ImGuiM3GetThemeFile()
{
    return g_theme.file_path.empty() ? nullptr : g_theme.file_path.c_str();
}

void ImGuiM3Shutdown()
{
    g_theme.role_flags[0] = 0;
    g_theme.role_flags[1] = 0;
    g_theme.file_path.clear();
    g_theme.initialised = false;
    g_icon_font = nullptr;
}

// fills every ImGuiCol_ slot from roles, leaving nothing on imgui's defaults.

void ImGuiM3ApplyToStyle(float ui_scale)
{
    if (!g_theme.initialised)
    {
        g_theme.initialised = true;
        ResolveTheme();
    }

    // style needs a context; token resolution doesn't, which keeps this testable headless.
    if (ImGui::GetCurrentContext() == nullptr)
        return;

    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    const ImVec4 on_surface      = ImGuiM3Color(ImGuiM3Role_OnSurface);
    const ImVec4 on_surface_var  = ImGuiM3Color(ImGuiM3Role_OnSurfaceVariant);
    const ImVec4 surface         = ImGuiM3Color(ImGuiM3Role_Surface);
    const ImVec4 sc_lowest       = ImGuiM3Color(ImGuiM3Role_SurfaceContainerLowest);
    const ImVec4 sc_low          = ImGuiM3Color(ImGuiM3Role_SurfaceContainerLow);
    const ImVec4 sc               = ImGuiM3Color(ImGuiM3Role_SurfaceContainer);
    const ImVec4 sc_high          = ImGuiM3Color(ImGuiM3Role_SurfaceContainerHigh);
    const ImVec4 sc_highest       = ImGuiM3Color(ImGuiM3Role_SurfaceContainerHighest);
    const ImVec4 surface_variant  = ImGuiM3Color(ImGuiM3Role_SurfaceVariant);
    const ImVec4 primary          = ImGuiM3Color(ImGuiM3Role_Primary);
    const ImVec4 on_primary       = ImGuiM3Color(ImGuiM3Role_OnPrimary);
    const ImVec4 secondary        = ImGuiM3Color(ImGuiM3Role_Secondary);
    const ImVec4 primary_cont     = ImGuiM3Color(ImGuiM3Role_PrimaryContainer);
    const ImVec4 on_primary_cont  = ImGuiM3Color(ImGuiM3Role_OnPrimaryContainer);
    const ImVec4 outline          = ImGuiM3Color(ImGuiM3Role_Outline);
    const ImVec4 outline_variant  = ImGuiM3Color(ImGuiM3Role_OutlineVariant);
    const ImVec4 shadow           = ImGuiM3Color(ImGuiM3Role_Shadow);
    const ImVec4 scrim            = ImGuiM3Color(ImGuiM3Role_Scrim);
    const ImVec4 inverse_surface  = ImGuiM3Color(ImGuiM3Role_InverseSurface);
    const ImVec4 inverse_on_surface = ImGuiM3Color(ImGuiM3Role_InverseOnSurface);

    // State layers, precomputed so the widget code can just index them.
    const ImVec4 hover_on_surface     = WithAlpha(on_surface, 0.08f);
    const ImVec4 pressed_on_surface   = WithAlpha(on_surface, 0.10f);
    const ImVec4 focus_on_surface     = WithAlpha(on_surface, 0.10f);
    const ImVec4 hover_on_primary     = WithAlpha(on_primary, 0.08f);
    const ImVec4 pressed_on_primary   = WithAlpha(on_primary, 0.10f);
    const ImVec4 hover_on_surface_var = WithAlpha(on_surface_var, 0.08f);

    style.Alpha = 1.0f;
    style.DisabledAlpha = 1.0f;  // handled per-role below, this knob fights tokens

    // Text.
    c[ImGuiCol_Text]                  = on_surface;
    c[ImGuiCol_TextDisabled]          = WithAlpha(on_surface, 0.38f);
    c[ImGuiCol_TextLink]              = primary;
    c[ImGuiCol_TextSelectedBg]        = WithAlpha(primary, 0.32f);

    // menus/dialogs sit on surface-container-high, the shell on plain surface
    // so child cards lift off it.
    c[ImGuiCol_WindowBg]             = surface;
    c[ImGuiCol_ChildBg]              = WithAlpha(sc, 0.0f);
    c[ImGuiCol_PopupBg]              = sc_high;
    c[ImGuiCol_TitleBg]              = surface;
    c[ImGuiCol_TitleBgActive]        = sc;
    c[ImGuiCol_TitleBgCollapsed]     = sc_high;
    c[ImGuiCol_MenuBarBg]            = surface;
    c[ImGuiCol_ScrollbarBg]          = WithAlpha(surface, 0.0f);

    // Outlines.
    c[ImGuiCol_Border]               = outline_variant;
    c[ImGuiCol_BorderShadow]         = WithAlpha(shadow, m.key_shadow_opacity);
    c[ImGuiCol_TableBorderStrong]    = outline;
    c[ImGuiCol_TableBorderLight]     = outline_variant;

    // text fields: surface-container-high, no border, hover/press as layers.
    c[ImGuiCol_FrameBg]              = sc_high;
    c[ImGuiCol_FrameBgHovered]       = Over(sc_high, hover_on_surface);
    c[ImGuiCol_FrameBgActive]        = Over(sc_high, pressed_on_surface);
    c[ImGuiCol_InputTextCursor]      = primary;

    // Scrollbars: M3 uses a thin always-visible thumb on surface-variant.
    c[ImGuiCol_ScrollbarGrab]        = WithAlpha(outline_variant, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = on_surface_var;
    c[ImGuiCol_ScrollbarGrabActive]  = primary;

    // Selection controls.
    c[ImGuiCol_CheckMark]            = primary;
    c[ImGuiCol_SliderGrab]           = primary;
    c[ImGuiCol_SliderGrabActive]     = primary;

    // Buttons default to tonal: surface-container-high with on-surface text.
    c[ImGuiCol_Button]               = sc_high;
    c[ImGuiCol_ButtonHovered]        = Over(sc_high, hover_on_surface);
    c[ImGuiCol_ButtonActive]         = Over(sc_high, pressed_on_surface);

    // Headers (list items, tree nodes, menu items): no fill until touched.
    c[ImGuiCol_Header]               = WithAlpha(on_surface, 0.0f);
    c[ImGuiCol_HeaderHovered]        = hover_on_surface;
    c[ImGuiCol_HeaderActive]         = pressed_on_surface;

    c[ImGuiCol_Separator]            = outline_variant;
    c[ImGuiCol_SeparatorHovered]     = on_surface_var;
    c[ImGuiCol_SeparatorActive]      = primary;

    c[ImGuiCol_ResizeGrip]           = WithAlpha(outline_variant, 0.6f);
    c[ImGuiCol_ResizeGripHovered]    = WithAlpha(primary, 0.6f);
    c[ImGuiCol_ResizeGripActive]     = primary;

    // tabs: pill indicator drawn separately, background stays clear.
    c[ImGuiCol_TabHovered]           = hover_on_surface;
    c[ImGuiCol_Tab]                  = WithAlpha(on_surface, 0.0f);
    c[ImGuiCol_TabSelected]          = WithAlpha(primary, 0.0f);
    c[ImGuiCol_TabDimmed]            = WithAlpha(on_surface, 0.0f);
    c[ImGuiCol_TabDimmedSelected]    = WithAlpha(primary, 0.0f);
    c[ImGuiCol_TabSelectedOverline]  = primary;
    c[ImGuiCol_TabDimmedSelectedOverline] = primary;

    c[ImGuiCol_DockingPreview]       = WithAlpha(primary, 0.40f);
    c[ImGuiCol_DockingEmptyBg]       = WithAlpha(scrim, m.scrim_opacity);

    // Plots.
    c[ImGuiCol_PlotLines]            = primary;
    c[ImGuiCol_PlotLinesHovered]     = on_primary;
    c[ImGuiCol_PlotHistogram]        = primary;
    c[ImGuiCol_PlotHistogramHovered] = on_primary;

    // Tables.
    c[ImGuiCol_TableHeaderBg]        = sc_high;
    c[ImGuiCol_TableRowBg]           = WithAlpha(sc_low, 1.0f);
    c[ImGuiCol_TableRowBgAlt]        = WithAlpha(sc, 1.0f);

    c[ImGuiCol_TreeLines]            = WithAlpha(outline_variant, 0.8f);
    c[ImGuiCol_DragDropTarget]       = primary;
    c[ImGuiCol_DragDropTargetBg]     = WithAlpha(primary, 0.16f);
    c[ImGuiCol_UnsavedMarker]        = primary;

    c[ImGuiCol_NavCursor]            = primary;
    c[ImGuiCol_NavWindowingHighlight]= primary;
    c[ImGuiCol_NavWindowingDimBg]    = WithAlpha(scrim, m.scrim_opacity);
    c[ImGuiCol_ModalWindowDimBg]     = WithAlpha(scrim, m.scrim_opacity);

    // Geometry. dp values scale by density * ui_scale.
    const float d = m.density * ui_scale;
    style.WindowPadding        = ImVec2(24.0f * d, 20.0f * d);
    style.WindowRounding       = ImGuiM3Radius(ImGuiM3Shape_ExtraLarge);
    style.ChildRounding        = ImGuiM3Radius(ImGuiM3Shape_Medium);
    style.PopupRounding        = 24.0f * d;
    style.FrameRounding        = ImGuiM3Radius(ImGuiM3Shape_Full);
    style.ScrollbarRounding    = ImGuiM3Radius(ImGuiM3Shape_Full);
    style.GrabRounding         = ImGuiM3Radius(ImGuiM3Shape_Full);
    style.TabRounding          = ImGuiM3Radius(ImGuiM3Shape_Medium);
    style.TabBarOverlineSize   = 0.0f;   // the pill indicator replaces the overline
    style.TabBarBorderSize     = ImGuiM3Radius(ImGuiM3Shape_None);
    style.TreeLinesRounding    = ImGuiM3Radius(ImGuiM3Shape_Full);
    style.DragDropTargetRounding = ImGuiM3Radius(ImGuiM3Shape_Medium);
    style.SeparatorTextBorderSize = 0.0f;

    // M3 draws outlines inside the container, and only on components that have
    // them, so the global frame border stays off.
    style.WindowBorderSize     = 0.0f;
    style.ChildBorderSize      = 0.0f;
    style.PopupBorderSize      = 0.0f;
    style.FrameBorderSize      = 0.0f;
    style.TabBorderSize        = 0.0f;

    // frame height is the M3 40dp button height, also the default row height for
    // sliders, combos, checkboxes and text fields.
    const float frame_height = m.button_height_default * d;
    const float font_size = style.FontSizeBase > 0.0f ? style.FontSizeBase : 16.0f;
    style.FramePadding        = ImVec2(m.button_padding_x * d, ImMax(4.0f, (frame_height - font_size) * 0.5f));
    style.ItemSpacing         = ImVec2(16.0f * d, 12.0f * d);
    style.ItemInnerSpacing    = ImVec2(12.0f * d, 12.0f * d);
    style.CellPadding         = ImVec2(16.0f * d, 12.0f * d);
    style.IndentSpacing       = 24.0f * d;
    style.ScrollbarSize       = 16.0f * d;
    style.ScrollbarPadding    = 2.0f * d;
    style.GrabMinSize         = 40.0f * d;
    style.TabMinWidthBase     = 64.0f * d;
    style.TabMinWidthShrink   = 48.0f * d;
    style.WindowTitleAlign    = ImVec2(0.5f, 0.5f);
    style.ButtonTextAlign     = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    style.Colors[ImGuiCol_NavCursor].w = 1.0f;

    // some roles are locals only to document the palette; the rest are unused, so
    // silence the warnings rather than delete the names.
    (void)secondary;
    (void)primary_cont;
    (void)on_primary_cont;
    (void)surface_variant;
    (void)sc_lowest;
    (void)sc_highest;
    (void)inverse_surface;
    (void)inverse_on_surface;
    (void)hover_on_primary;
    (void)pressed_on_primary;
    (void)focus_on_surface;
    (void)hover_on_surface_var;
}

// Springs.

struct SpringEntry
{
    ImGuiM3Spring spring;
    bool initialized = false;
    double last_seen = 0.0;
};

static std::unordered_map<ImGuiID, SpringEntry> g_springs;
static double g_spring_clock = 0.0;

void ImGuiM3ClearSprings()
{
    // drop springs untouched for a while so a long session can't grow the map forever.
    for (auto it = g_springs.begin(); it != g_springs.end();)
    {
        if (g_spring_clock - it->second.last_seen > 30.0)
            it = g_springs.erase(it);
        else
            ++it;
    }
}

static float SpringStep(ImGuiID id, float target, float damping_ratio, float stiffness)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    SpringEntry& entry = g_springs[id];
    entry.last_seen = g_spring_clock;

    const float dt = ImClamp(ImGui::GetIO().DeltaTime > 0.0f ? ImGui::GetIO().DeltaTime : 1.0f / 60.0f, 1.0f / 240.0f, 1.0f / 15.0f);
    const float omega = ImSqrt(ImMax(1.0f, stiffness));

    if (!entry.initialized)
    {
        // first call for this id: adopt the target without animating, so a widget
        // that starts already pressed does not fly in.
        entry.initialized = true;
        entry.spring.value = target;
        entry.spring.velocity = 0.0f;
        return target;
    }

    // Semi-implicit Euler on a damped harmonic oscillator.
    const float displacement = entry.spring.value - target;
    const float acceleration = -stiffness * displacement - 2.0f * damping_ratio * omega * entry.spring.velocity;
    entry.spring.velocity += acceleration * dt;
    entry.spring.value += entry.spring.velocity * dt;

    // settle below a thousandth of a pixel and stop touching the entry.
    if (ImAbs(displacement) < 0.001f && ImAbs(entry.spring.velocity) < 0.001f)
    {
        entry.spring.value = target;
        entry.spring.velocity = 0.0f;
    }
    (void)m;
    return entry.spring.value;
}

float ImGuiM3SpringStep(ImGuiID id, float target, float damping_ratio, float stiffness)
{
    return SpringStep(id, target, damping_ratio, stiffness);
}

float ImGuiM3SpringStepSpatialFast(ImGuiID id, float target)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    return SpringStep(id, target, m.spring_spatial_fast_damping, m.spring_spatial_fast_stiffness);
}

float ImGuiM3SpringStepSpatialDefault(ImGuiID id, float target)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    return SpringStep(id, target, m.spring_spatial_default_damping, m.spring_spatial_default_stiffness);
}

float ImGuiM3SpringStepSpatialSlow(ImGuiID id, float target)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    return SpringStep(id, target, m.spring_spatial_slow_damping, m.spring_spatial_slow_stiffness);
}

float ImGuiM3SpringStepEffectsFast(ImGuiID id, float target)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    return SpringStep(id, target, m.spring_effects_fast_damping, m.spring_effects_fast_stiffness);
}

float ImGuiM3SpringStepEffectsDefault(ImGuiID id, float target)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    return SpringStep(id, target, m.spring_effects_default_damping, m.spring_effects_default_stiffness);
}

float ImGuiM3SpringStepEffectsSlow(ImGuiID id, float target)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    return SpringStep(id, target, m.spring_effects_slow_damping, m.spring_effects_slow_stiffness);
}

float ImGuiM3PressMorph(ImGuiID id, bool held)
{
    // One state key holds linear progress 0..1; the returned value is an
    // ease-out of it, so the corner eases down while held and eases back on
    // release, never overshooting. Linear progress (not a clock) keeps the
    // storage use to the single key the working tween already relied on.
    static constexpr float kMorphSeconds = 0.30f;

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float dt = ImClamp(ImGui::GetIO().DeltaTime > 0.0f ? ImGui::GetIO().DeltaTime : 1.0f / 60.0f,
                             1.0f / 240.0f, 1.0f / 10.0f);
    const float target = held ? 1.0f : 0.0f;

    float progress = window->StateStorage.GetFloat(id, 0.0f);
    const float step = dt / kMorphSeconds;
    progress = (progress < target) ? ImMin(target, progress + step)
                                   : ImMax(target, progress - step);
    window->StateStorage.SetFloat(id, progress);

    const float inv = 1.0f - progress;
    return 1.0f - inv * inv * inv;
}

// Named font weights.

static ImFont* g_font_regular = nullptr;
static ImFont* g_font_medium = nullptr;
static ImFont* g_font_bold = nullptr;
static ImFont* g_font_extra_bold = nullptr;

void ImGuiM3SetTextFonts(ImFont* regular, ImFont* medium, ImFont* bold, ImFont* extra_bold)
{
    g_font_regular = regular;
    g_font_medium = medium;
    g_font_bold = bold;
    g_font_extra_bold = extra_bold;
}

ImFont* ImGuiM3FontMedium()    { return g_font_medium ? g_font_medium : g_font_regular; }
ImFont* ImGuiM3FontBold()      { return g_font_bold ? g_font_bold : g_font_regular; }
ImFont* ImGuiM3FontExtraBold() { return g_font_extra_bold ? g_font_extra_bold : g_font_regular; }

// Material Symbols icon face.

bool ImGuiM3LoadIconFont(const char* path, float size_px, const ImWchar* ranges)
{
    if (!path || !ImGui::GetCurrentContext())
        return false;
    // icons live in the Private Use Area; callers pass the exact subset they use
    // so a full font does not build every PUA glyph into the atlas
    static const ImWchar all_icon_ranges[] = {0xE000, 0xF8FF, 0};
    ImFontConfig cfg;
    cfg.PixelSnapH = true;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    g_icon_font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path, size_px, &cfg, ranges ? ranges : all_icon_ranges);
    return g_icon_font != nullptr;
}

ImFont* ImGuiM3IconFont() { return g_icon_font; }

bool ImGuiM3MergeIconFont(const char* path, float size_px, const ImWchar* ranges)
{
    if (!path || !ImGui::GetCurrentContext())
        return false;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (atlas->Fonts.Size == 0)
        return false;
    static const ImWchar all_icon_ranges[] = {0xE000, 0xF8FF, 0};
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    return atlas->AddFontFromFileTTF(path, size_px, &cfg, ranges ? ranges : all_icon_ranges) != nullptr;
}

// Easing curves.

static float CubicBezierEase(float t, float x1, float y1, float x2, float y2)
{
    t = ImSaturate(t);
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;

    auto curve = [](float a, float b, float u) { float k = 1.0f - u; return 3.0f * k * k * u * a + 3.0f * k * u * u * b + u * u * u; };

    // solve curve(t, x1, x2) == t, then read y. bisection is slower than Newton
    // but monotone and cannot diverge on these curves.
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 20; i++)
    {
        const float mid = (lo + hi) * 0.5f;
        if (curve(x1, x2, mid) < t)
            lo = mid;
        else
            hi = mid;
    }
    return curve(y1, y2, (lo + hi) * 0.5f);
}

float ImGuiM3EaseStandard(float t)              { return CubicBezierEase(t, 0.2f, 0.0f, 0.0f, 1.0f); }
float ImGuiM3EaseStandardDecelerate(float t)   { return CubicBezierEase(t, 0.0f, 0.0f, 0.0f, 1.0f); }
float ImGuiM3EaseStandardAccelerate(float t)   { return CubicBezierEase(t, 0.3f, 0.0f, 1.0f, 1.0f); }
float ImGuiM3EaseEmphasizedAccelerate(float t) { return CubicBezierEase(t, 0.3f, 0.0f, 0.8f, 0.15f); }
float ImGuiM3EaseEmphasizedDecelerate(float t) { return CubicBezierEase(t, 0.05f, 0.7f, 0.1f, 1.0f); }
float ImGuiM3EaseLegacy(float t)               { return CubicBezierEase(t, 0.4f, 0.0f, 0.2f, 1.0f); }
float ImGuiM3EaseExpressiveFastSpatial(float t)    { return CubicBezierEase(t, 0.42f, 1.67f, 0.21f, 0.90f); }
float ImGuiM3EaseExpressiveDefaultSpatial(float t) { return CubicBezierEase(t, 0.38f, 1.21f, 0.22f, 1.00f); }
float ImGuiM3EaseExpressiveSlowSpatial(float t)    { return CubicBezierEase(t, 0.39f, 1.29f, 0.35f, 0.98f); }
float ImGuiM3EaseExpressiveFastEffects(float t)    { return CubicBezierEase(t, 0.31f, 0.94f, 0.34f, 1.00f); }
float ImGuiM3EaseExpressiveDefaultEffects(float t) { return CubicBezierEase(t, 0.34f, 0.80f, 0.34f, 1.00f); }
float ImGuiM3EaseExpressiveSlowEffects(float t)    { return CubicBezierEase(t, 0.34f, 0.88f, 0.34f, 1.00f); }

// Shape helpers.

float ImGuiM3Radius(ImGuiM3Shape shape)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    float base = 0.0f;
    switch (shape)
    {
    case ImGuiM3Shape_None:                base = m.corner_none; break;
    case ImGuiM3Shape_ExtraSmall:          base = m.corner_xs; break;
    case ImGuiM3Shape_Small:               base = m.corner_s; break;
    case ImGuiM3Shape_Medium:              base = m.corner_m; break;
    case ImGuiM3Shape_Large:               base = m.corner_l; break;
    case ImGuiM3Shape_LargeIncreased:      base = m.corner_l_increased; break;
    case ImGuiM3Shape_ExtraLarge:          base = m.corner_xl; break;
    case ImGuiM3Shape_ExtraLargeIncreased: base = m.corner_xl_increased; break;
    case ImGuiM3Shape_ExtraExtraLarge:     base = m.corner_xxl; break;
    case ImGuiM3Shape_Full:
    {
        // corner-full is a token, not "50% of size", so return a large radius
        // that callers clamp per-widget.
        base = 9999.0f;
        break;
    }
    default: base = m.corner_s; break;
    }
    return base * m.density * m.shape_scale;
}

float ImGuiM3PillRadius(ImVec2 size, float radius)
{
    return ImMin(radius, ImMin(size.x, size.y) * 0.5f);
}

ImGuiM3ShapeRounding ImGuiM3MorphedRounding(ImVec2 size, float radius, float pressed_radius, bool pressed, ImGuiID id)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    // corner morphs run on the default spatial spring.
    const float t = ImGuiM3SpringStepSpatialDefault(id, pressed ? 1.0f : 0.0f);
    float r = radius + (pressed_radius - radius) * t;
    if (radius > 1000.0f)
        r = ImMin(radius, ImMin(size.x, size.y) * 0.5f);
    r = ImMin(r, ImMin(size.x, size.y) * 0.5f);
    (void)m;
    (void)pressed;
    return ImGuiM3ShapeRounding{ r, r, r, r };
}

float ImGuiM3ConnectedInnerRadius()
{
    // md.comp.button-group.connected inner-corner at size S: 8dp.
    return ImGuiM3Radius(ImGuiM3Shape_Small);
}

ImGuiM3ShapeRounding ImGuiM3ConnectedSegmentRounding(ImVec2 size, int index, int count)
{
    const float full = ImGuiM3PillRadius(size, ImGuiM3Radius(ImGuiM3Shape_Full));
    ImGuiM3ShapeRounding r{ full, full, full, full };
    if (count <= 1)
        return r;
    const float inner = ImMin(ImGuiM3ConnectedInnerRadius(), ImMin(size.x, size.y) * 0.5f);
    if (index <= 0)
    {
        // Leading segment: the start edge is a pill, the shared edge modest.
        r.tr = inner;
        r.br = inner;
    }
    else if (index >= count - 1)
    {
        // Trailing segment: mirror of the leading one.
        r.tl = inner;
        r.bl = inner;
    }
    else
    {
        r = ImGuiM3ShapeRounding{ inner, inner, inner, inner };
    }
    return r;
}

// Drawing helpers.

// builds a per-corner rounded-rect path, since ImDrawList only rounds uniformly.
// the sweep runs clockwise from the top-right; the start point is not repeated so
// a zero-length closing segment does not stroke as a spiked dotted line.
static void AddRoundedRectPath(ImDrawList* draw_list, const ImRect& bb, ImGuiM3ShapeRounding rounding)
{
    const float w = bb.GetWidth();
    const float h = bb.GetHeight();
    if (w <= 0.0f || h <= 0.0f)
        return;
    const float max_r = ImMin(w, h) * 0.5f;
    const float tl = ImClamp(rounding.tl, 0.0f, max_r);
    const float tr = ImClamp(rounding.tr, 0.0f, max_r);
    const float br = ImClamp(rounding.br, 0.0f, max_r);
    const float bl = ImClamp(rounding.bl, 0.0f, max_r);

    if (tr > 0.0f) draw_list->PathArcTo(ImVec2(bb.Max.x - tr, bb.Min.y + tr), tr, -IM_PI * 0.5f, 0.0f);
    else           draw_list->PathLineTo(ImVec2(bb.Max.x, bb.Min.y));
    if (br > 0.0f) draw_list->PathArcTo(ImVec2(bb.Max.x - br, bb.Max.y - br), br, 0.0f, IM_PI * 0.5f);
    else           draw_list->PathLineTo(ImVec2(bb.Max.x, bb.Max.y));
    if (bl > 0.0f) draw_list->PathArcTo(ImVec2(bb.Min.x + bl, bb.Max.y - bl), bl, IM_PI * 0.5f, IM_PI);
    else           draw_list->PathLineTo(ImVec2(bb.Min.x, bb.Max.y));
    if (tl > 0.0f) draw_list->PathArcTo(ImVec2(bb.Min.x + tl, bb.Min.y + tl), tl, IM_PI, IM_PI * 1.5f);
    else           draw_list->PathLineTo(ImVec2(bb.Min.x, bb.Min.y));
}

// drops consecutive duplicate vertices: where a pill's cap arcs meet, a
// zero-length segment would stroke as a flat notch.
static void DedupeDrawPath(ImDrawList* draw_list)
{
    ImVector<ImVec2>& path = draw_list->_Path;
    if (path.Size < 3)
        return;
    int w = 0;
    for (int r = 0; r < path.Size; r++)
    {
        if (w > 0 && ImLengthSqr(path[r] - path[w - 1]) < 1e-4f)
            continue;
        path[w++] = path[r];
    }
    path.Size = w;
    if (path.Size > 1 && ImLengthSqr(path[0] - path[path.Size - 1]) < 1e-4f)
        path.Size--;
}

void ImGuiM3PathRoundedRect(ImDrawList* draw_list, const ImRect& bb, ImGuiM3ShapeRounding rounding, ImU32 col)
{
    if (col == 0 || bb.GetWidth() <= 0.0f || bb.GetHeight() <= 0.0f)
        return;

    // uniform rounding uses ImGui's own anti-aliased AddRectFilled; the rest use
    // the exact per-corner path so a shape's fill and its outline coincide.
    if (rounding.tl == rounding.tr && rounding.tr == rounding.br && rounding.br == rounding.bl)
    {
        const float radius = ImMin(rounding.tl, ImMin(bb.GetWidth(), bb.GetHeight()) * 0.5f);
        draw_list->AddRectFilled(bb.Min, bb.Max, col, radius);
        return;
    }
    AddRoundedRectPath(draw_list, bb, rounding);
    DedupeDrawPath(draw_list);
    draw_list->PathFillConvex(col);
}

void ImGuiM3DrawContainer(ImDrawList* draw_list, const ImRect& bb, ImGuiM3ShapeRounding rounding, ImU32 fill, ImU32 outline, float outline_width)
{
    if (fill != 0)
        ImGuiM3PathRoundedRect(draw_list, bb, rounding, fill);
    if (outline != 0 && outline_width > 0.0f)
    {
        // Outlines are stroked just inside the shape so the fill keeps its size.
        const float half = outline_width * 0.5f;
        ImRect inner(bb.Min + ImVec2(half, half), bb.Max - ImVec2(half, half));
        if (inner.GetWidth() > 0.0f && inner.GetHeight() > 0.0f)
        {
            if (rounding.tl == rounding.tr && rounding.tr == rounding.br && rounding.br == rounding.bl)
            {
                const float radius = ImMin(ImMax(0.0f, rounding.tl - half), ImMin(inner.GetWidth(), inner.GetHeight()) * 0.5f);
                draw_list->AddRect(inner.Min, inner.Max, outline, radius, 0, outline_width);
            }
            else
            {
                ImGuiM3ShapeRounding inner_r{ ImMax(0.0f, rounding.tl - half), ImMax(0.0f, rounding.tr - half),
                                              ImMax(0.0f, rounding.br - half), ImMax(0.0f, rounding.bl - half) };
                AddRoundedRectPath(draw_list, inner, inner_r);
                DedupeDrawPath(draw_list);
                draw_list->PathStroke(outline, ImDrawFlags_Closed, outline_width);
            }
        }
    }
}

void ImGuiM3DrawStateLayer(ImDrawList* draw_list, const ImRect& bb, ImGuiM3ShapeRounding rounding, ImGuiM3Role role, ImGuiM3State state)
{
    const ImU32 col = ImGuiM3StateLayerU32(role, state);
    if (col == 0)
        return;
    // inherits the container's shape, and clips to it rather than the bounding
    // box: a box clip shears the anti-aliased fringe off the curved edges.
    ImGuiM3PathRoundedRect(draw_list, bb, rounding, col);
}

void ImGuiM3DrawIcon(ImDrawList* draw_list, const char* glyph, const ImRect& bb, float px, ImU32 col)
{
    ImFont* icon = ImGuiM3IconFont();
    if (!icon || !glyph || px <= 0.0f)
        return;
    // centre the glyph's *ink*, not its advance box: Material Symbols have
    // asymmetric side bearings, so advance-centring pushes the mark off to one side.
    ImVec2 ink_center;
    if (!ImGuiM3IconInkCenterXY(icon, px, glyph, ink_center))
    {
        ImGui::PushFont(icon, px);
        const ImVec2 sz = ImGui::CalcTextSize(glyph);
        ImGui::PopFont();
        ink_center = ImVec2(sz.x * 0.5f, px * 0.5f);
    }
    const ImVec2 pos(bb.GetCenter().x - ink_center.x, bb.GetCenter().y - ink_center.y);
    draw_list->AddText(icon, px, pos, col, glyph);
}

bool ImGuiM3IconInkCenterXY(ImFont* font, float px, const char* text, ImVec2& out_center)
{
    if (!font || !text || px <= 0.0f)
        return false;
    // lone glyphs only; a mixed icon+label run shares a baseline, so bail on extra
    // codepoints.
    unsigned int cp = 0;
    const int consumed = ImTextCharFromUtf8(&cp, text, text + strlen(text));
    if (consumed <= 0 || (size_t)consumed != strlen(text))
        return false;
    ImFontBaked* baked = font->GetFontBaked(px);
    ImFontGlyph* g = baked ? baked->FindGlyph((ImWchar)cp) : nullptr;
    if (!g)
        return false;
    out_center = ImVec2((g->X0 + g->X1) * 0.5f, (g->Y0 + g->Y1) * 0.5f);
    return true;
}

float ImGuiM3IconInkCenter(ImFont* font, float px, const char* glyph)
{
    if (!font || !glyph)
        return px * 0.5f;
    ImFontBaked* baked = font->GetFontBaked(px);
    unsigned int cp = 0;
    ImTextCharFromUtf8(&cp, glyph, glyph + strlen(glyph));
    ImFontGlyph* g = baked->FindGlyph((ImWchar)cp);
    if (!g)
        return px * 0.5f;
    return (g->Y0 + g->Y1) * 0.5f;
}

void ImGuiM3DrawElevation(ImDrawList* draw_list, const ImRect& bb, ImGuiM3ShapeRounding rounding, int level)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    level = clampInt(0, 5, level);
    if (level == 0)
        return;
    const ImVec4 shadow_color = ImGuiM3Color(ImGuiM3Role_Shadow);
    const float key_y = m.key_shadow_y[level];
    const float ambient_y = m.ambient_shadow_y[level];
    const float spread = 3.0f + m.ambient_shadow_spread[level];
    const float total_alpha = ImMin(1.0f, m.key_shadow_opacity + m.ambient_shadow_opacity);

    // no blur in ImGui, so stack rounded rects that grow outward while fading.
    // a single offset rect reads as a hard band; the stack reads as a soft M3
    // shadow, densest at the card edge and gone by the outer bound.
    const int steps = 6;
    for (int step = steps; step >= 1; --step)
    {
        const float t = static_cast<float>(step) / static_cast<float>(steps);
        const float grow = spread * t;
        const float dy = ambient_y * t + key_y * (1.0f - t);
        ImRect r(ImVec2(bb.Min.x - grow, bb.Min.y - grow + dy),
                 ImVec2(bb.Max.x + grow, bb.Max.y + grow + dy));
        ImGuiM3ShapeRounding rr = rounding;
        rr.tl += grow; rr.tr += grow; rr.br += grow; rr.bl += grow;
        const float alpha = total_alpha * (1.0f - t) * 0.45f;
        ImGuiM3PathRoundedRect(draw_list, r, rr, PackU32(WithAlpha(shadow_color, alpha)));
    }
}

void ImGuiM3DrawScrim(ImDrawList* draw_list, const ImRect& bb)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    draw_list->AddRectFilled(bb.Min, bb.Max, PackU32(WithAlpha(ImGuiM3Color(ImGuiM3Role_Scrim), m.scrim_opacity)));
}

// Frame hook.

void ImGuiM3NewFrame()
{
    g_spring_clock += ImGui::GetIO().DeltaTime > 0.0f ? ImGui::GetIO().DeltaTime : 1.0f / 60.0f;

    if (g_theme.file_path.empty())
        return;

    // Poll at ~4Hz. Statting once per frame would be wasteful in a game overlay,
    // and Matugen writes the file atomically so a size+mtime check is enough.
    if (g_spring_clock - g_theme.last_poll_time < 0.25)
        return;
    g_theme.last_poll_time = g_spring_clock;

    uint64_t mtime = 0, size = 0;
    bool exists = false;
    StatFile(g_theme.file_path, &mtime, &size, &exists);

    if (!exists)
    {
        if (g_theme.file_existed)
        {
            // file removed: fall back to the preset but keep the path for the next write.
            g_theme.file_existed = false;
            g_theme.role_flags[0] = 0;
            g_theme.role_flags[1] = 0;
            g_theme.metrics = ImGuiM3Metrics();
            g_theme.source = 0xFFE91E63;
    g_theme.variant = ImGuiM3Variant_Expressive;
            g_theme.contrast = ImGuiM3Contrast_Standard;
            g_theme.dark = true;
            g_theme.last_reload_message = "theme file removed, back to preset";
            ResolveTheme();
            ImGuiM3ApplyToStyle(1.0f);
            g_theme.reloaded = true;
        }
        return;
    }

    if (exists && (!g_theme.file_existed || mtime != g_theme.file_mtime_ns || size != g_theme.file_size))
    {
        g_theme.file_existed = true;
        g_theme.file_mtime_ns = mtime;
        g_theme.file_size = size;
        LoadThemeFile(g_theme.file_path);
        ImGuiM3ApplyToStyle(1.0f);
        g_theme.reloaded = true;
    }
}

// M3 widgets.

namespace
{
    // container, label, state-layer roles and elevation per button variant.
    struct ButtonPaint
    {
        ImGuiM3Role container_role;
        ImGuiM3Role label_role;
        ImGuiM3Role layer_role;
        bool        has_outline;
        int         elevation;
    };

    ButtonPaint PaintFor(ImGuiM3ButtonVariant variant)
    {
        switch (variant)
        {
        case ImGuiM3Button_Tonal:                return {ImGuiM3Role_SecondaryContainer, ImGuiM3Role_OnSecondaryContainer, ImGuiM3Role_OnSecondaryContainer, false, 0};
        case ImGuiM3Button_Outlined:             return {ImGuiM3Role_Surface, ImGuiM3Role_Primary, ImGuiM3Role_Primary, true, 0};
        case ImGuiM3Button_Elevated:             return {ImGuiM3Role_SurfaceContainerLow, ImGuiM3Role_Primary, ImGuiM3Role_Primary, false, 1};
        case ImGuiM3Button_Text:                 return {ImGuiM3Role_Surface, ImGuiM3Role_Primary, ImGuiM3Role_Primary, false, 0};
        case ImGuiM3Button_Destructive:          return {ImGuiM3Role_Error, ImGuiM3Role_OnError, ImGuiM3Role_OnError, false, 0};
        case ImGuiM3Button_DestructiveTonal:     return {ImGuiM3Role_ErrorContainer, ImGuiM3Role_OnErrorContainer, ImGuiM3Role_OnErrorContainer, false, 0};
        case ImGuiM3Button_DestructiveOutlined:  return {ImGuiM3Role_Surface, ImGuiM3Role_Error, ImGuiM3Role_Error, true, 0};
        case ImGuiM3Button_Filled:
        default:                                 return {ImGuiM3Role_Primary, ImGuiM3Role_OnPrimary, ImGuiM3Role_OnPrimary, false, 0};
        }
    }

    int CardDepth = 0;
}

bool ImGui::M3Button(const char* label, ImGuiM3ButtonVariant variant, const ImVec2& size_arg, bool strong_label)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    const ButtonPaint paint = PaintFor(variant);
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = CalcTextSize(label, NULL, true);

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size = CalcItemSize(size_arg, label_size.x + m.button_padding_x * 2.0f * m.density, m.button_height_default * m.density);
    const ImRect bb(pos, pos + size);
    ItemSize(size);
    if (!ItemAdd(bb, id))
        return false;

    bool hovered, held;
    const bool pressed = ButtonBehavior(bb, id, &hovered, &held);
    RenderNavCursor(bb, id);

    // corners morph corner-full → corner-small while pressed. a direct press
    // tween so a normal click reaches the small radius and springs back visibly.
    const float rest_radius = ImGuiM3Radius(ImGuiM3Shape_Full);
    const float press_radius = ImGuiM3Radius(ImGuiM3Shape_ExtraSmall);
    const float morph = ImGuiM3PressMorph(id ^ 0x42544F, held);
    const float radius = ImMin(rest_radius + (press_radius - rest_radius) * morph, ImMin(size.x, size.y) * 0.5f);
    const ImGuiM3ShapeRounding rounding{ radius, radius, radius, radius };

    if (paint.elevation > 0)
        ImGuiM3DrawElevation(window->DrawList, bb, rounding, paint.elevation);

    const ImU32 fill = paint.has_outline ? 0u : ImGuiM3ColorU32(paint.container_role);
    const ImU32 outline = paint.has_outline ? ImGuiM3ColorU32(paint.container_role == ImGuiM3Role_Surface ? ImGuiM3Role_Outline : ImGuiM3Role_Outline)
                                            : 0u;
    ImGuiM3DrawContainer(window->DrawList, bb, rounding, fill, outline, m.button_outline_width * m.density);

    // State layer, drawn after the container and before the label.
    ImGuiM3State state = ImGuiM3State_Enabled;
    if (hovered && held) state = ImGuiM3State_Pressed;
    else if (hovered)      state = ImGuiM3State_Hovered;
    ImGuiM3DrawStateLayer(window->DrawList, bb, rounding, paint.layer_role, state);

    PushStyleColor(ImGuiCol_Text, ImGuiM3ColorU32(paint.label_role));
    // Dialog actions read a step stronger than the default medium: M3's
    // label-large at a touch more size, bold when the caller asks for it.
    ImFont* label_font = strong_label ? ImGuiM3FontBold() : ImGuiM3FontMedium();
    if (!label_font)
        label_font = ImGuiM3FontMedium();
    const float label_scale = strong_label ? 1.05f : 1.0f;
    if (label_font)
        PushFont(label_font, GetFontSize() * label_scale);
    const ImVec2 weighted_label_size = label_font ? CalcTextSize(label, NULL, true) : label_size;
    // a lone icon glyph is ink-centred in the square; text labels use the normal
    // centred/clipped layout.
    ImVec2 ink_center;
    if (ImGuiM3IconInkCenterXY(GetFont(), GetFontSize(), label, ink_center))
        RenderText(ImVec2(bb.GetCenter().x - ink_center.x, bb.GetCenter().y - ink_center.y), label);
    else
        RenderTextClipped(bb.Min + ImVec2(m.button_padding_x * m.density, 0.0f), bb.Max - ImVec2(m.button_padding_x * m.density, 0.0f), label, NULL, &weighted_label_size, ImVec2(0.5f, 0.5f), &bb);
    if (label_font)
        PopFont();
    PopStyleColor();

    IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags);
    return pressed;
}

bool ImGui::M3IconButton(const char* glyph, const char* tooltip, ImGuiM3ButtonVariant variant)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    const float size = m.icon_button_size * m.density;
    if (M3Button(glyph, variant, ImVec2(size, size)))
    {
        if (tooltip)
            SetItemTooltip("%s", tooltip);
        return true;
    }
    if (tooltip && IsItemHovered())
        SetItemTooltip("%s", tooltip);
    return false;
}

bool ImGui::M3SwitchWithID(const char* label, const char* id_str, bool* v)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    const ImGuiID id = window->GetID(id_str);
    const ImVec2 label_size = label ? CalcTextSize(label, NULL, true) : ImVec2(0, 0);

    const float track_w = m.switch_track_width * m.density;
    const float track_h = m.switch_track_height * m.density;
    const ImVec2 pos = window->DC.CursorPos;
    const float row_h = ImMax(track_h, m.list_item_height_1 * m.density);
    const ImRect total_bb(pos, pos + ImVec2(track_w + (label_size.x > 0.0f ? g.Style.ItemInnerSpacing.x + label_size.x : 0.0f), row_h));
    const ImRect track_bb(ImVec2(pos.x, pos.y + (row_h - track_h) * 0.5f), ImVec2(pos.x + track_w, pos.y + (row_h - track_h) * 0.5f + track_h));
    ItemSize(total_bb);
    if (!ItemAdd(total_bb, id))
        return false;

    bool hovered, held;
    bool pressed = ButtonBehavior(total_bb, id, &hovered, &held);

    // selected track is primary with a check; unselected is surface-container-highest.
    const bool on = *v;
    const ImVec4 track_on_color = ImGuiM3Color(ImGuiM3Role_Primary);
    const ImVec4 track_off_color = ImGuiM3Color(ImGuiM3Role_SurfaceContainerHighest);
    const ImVec4 outline_off_color = ImGuiM3Color(ImGuiM3Role_Outline);
    const float pill = ImMin(track_h, track_w) * 0.5f;
    const ImGuiM3ShapeRounding track_rounding{ pill, pill, pill, pill };

    // Press narrows the track, which is the switch's own shape morph. Slower
    // and less bouncy than the shared default spatial spring (0.9 + 120 settles
    // in ~405ms, where the default's 0.8 + 380 settled in ~257ms and read as a
    // snap).
    const float press_t = ImGuiM3SpringStep(id ^ 0x5357, held ? 1.0f : 0.0f, 0.9f, 120.0f);

    // Handle: 22dp normally, 26dp when pressed, with a spring so it grows.
    // on and off share the icon handle size so the side padding is identical,
    // and the inset tracks the vertical centring so all four gaps match.
    // The spring's overshoot is clamped to the track: without it the circle can
    // grow past the pill's height or push its travel negative and leave the pill.
    const float handle_size = ImMin(
        (m.switch_handle_with_icon + (m.switch_pressed_handle - m.switch_handle_with_icon) * press_t) * m.density,
        track_h);
    const float track_width = ImMax(
        track_w + (m.switch_pressed_track_width - m.switch_track_width) * m.density * press_t,
        handle_size);
    const float inset = (track_h - handle_size) * 0.5f;

    // The toggle animation proper: the handle springs from its previous side to
    // the new one, settling in ~410ms with a light bounce (damping 0.75,
    // stiffness 170). Its resting spot is clamped to the track, so the overshoot
    // can use the padding but the circle can never leave the pill.
    const float toggle_t = ImGuiM3SpringStep(id ^ 0x5351, on ? 1.0f : 0.0f, 0.75f, 170.0f);

    ImGuiM3DrawContainer(window->DrawList, track_bb, track_rounding, PackU32(on ? track_on_color : track_off_color),
                         PackU32(on ? ImGuiM3Color(ImGuiM3Role_SurfaceTint) : outline_off_color),
                         m.switch_track_outline * m.density);

    // State layer on the track.
    ImGuiM3State state = (hovered && held) ? ImGuiM3State_Pressed : hovered ? ImGuiM3State_Hovered : ImGuiM3State_Enabled;
    ImGuiM3DrawStateLayer(window->DrawList, track_bb, track_rounding, on ? ImGuiM3Role_OnPrimary : ImGuiM3Role_OnSurface, state);

    const float handle_half = handle_size * 0.5f;
    const float x_off = track_bb.Min.x + inset + handle_half;
    const float x_on  = track_bb.Min.x + track_width - inset - handle_half;
    const float handle_x = ImClamp(x_off + (x_on - x_off) * toggle_t, ImMin(x_off, x_on), ImMax(x_off, x_on));
    const ImVec2 handle_center(handle_x, track_bb.GetCenter().y);
    const float handle_r = handle_size * 0.5f;
    const ImU32 handle_col = on ? PackU32(ImGuiM3Color(ImGuiM3Role_OnPrimary)) : PackU32(ImGuiM3Color(ImGuiM3Role_Outline));
    window->DrawList->AddCircleFilled(handle_center, handle_r, handle_col, window->DrawList->_CalcCircleAutoSegmentCount(handle_r));

    // Handle shadow, so the thumb reads as raised.
    window->DrawList->AddCircleFilled(handle_center + ImVec2(0.0f, 1.0f), handle_r,
                                      PackU32(WithAlpha(ImGuiM3Color(ImGuiM3Role_Shadow), 0.30f)),
                                      window->DrawList->_CalcCircleAutoSegmentCount(handle_r));
    window->DrawList->AddCircleFilled(handle_center, handle_r, handle_col, window->DrawList->_CalcCircleAutoSegmentCount(handle_r));

    // when selected, M3 draws a check inside the handle.
    if (on)
    {
        static const char kCheckGlyph[] = "\xEE\x97\x8A";
        ImFont* icon = ImGuiM3IconFont();
        if (icon)
        {
            const float px = handle_size * 0.95f;
            const ImRect handle_bb(ImVec2(handle_center.x - handle_r, handle_center.y - handle_r),
                                   ImVec2(handle_center.x + handle_r, handle_center.y + handle_r));
            ImGuiM3DrawIcon(window->DrawList, kCheckGlyph, handle_bb, px, PackU32(ImGuiM3Color(ImGuiM3Role_OnPrimary)));
        }
        else
        {
            const float s = handle_size * 0.34f;
            const ImVec2 a = handle_center + ImVec2(-s * 0.7f, 0.0f);
            const ImVec2 b = handle_center + ImVec2(-s * 0.2f, s * 0.55f);
            const ImVec2 c = handle_center + ImVec2(s * 0.8f, -s * 0.6f);
            window->DrawList->AddLine(a, b, PackU32(ImGuiM3Color(ImGuiM3Role_OnPrimary)), 2.0f * m.density);
            window->DrawList->AddLine(b, c, PackU32(ImGuiM3Color(ImGuiM3Role_OnPrimary)), 2.0f * m.density);
        }
    }

    if (pressed)
    {
        *v = !*v;
        MarkItemEdited(id);
    }

    if (label && label_size.x > 0.0f)
        RenderText(ImVec2(track_bb.Max.x + g.Style.ItemInnerSpacing.x, total_bb.Min.y + (total_bb.GetHeight() - label_size.y) * 0.5f), label);

    IMGUI_TEST_ENGINE_ITEM_INFO(id, label ? label : id_str, g.LastItemData.StatusFlags);
    return pressed;
}

bool ImGui::M3Switch(const char* label, bool* v)
{
    return M3SwitchWithID(label, label, v);
}

bool ImGui::M3Fab(const char* glyph, const char* tooltip, bool large)
{
    ImGuiContext& g = *GImGui;
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    const float size = (large ? m.fab_size_large : m.fab_size) * m.density;
    const float radius = ImGuiM3Radius(large ? ImGuiM3Shape_ExtraLarge : ImGuiM3Shape_Large);
    const float press_radius = ImGuiM3Radius(large ? ImGuiM3Shape_Large : ImGuiM3Shape_Medium);

    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const ImGuiID id = window->GetID(glyph);
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, pos + ImVec2(size, size));
    ItemSize(bb.GetSize());
    if (!ItemAdd(bb, id))
        return false;

    bool hovered, held;
    const bool pressed = ButtonBehavior(bb, id, &hovered, &held);
    const ImGuiM3ShapeRounding rounding = ImGuiM3MorphedRounding(bb.GetSize(), radius, press_radius, held && hovered, id);

    // FAB rests at elevation 3, drops to 0 while pressed.
    const float press_t = ImGuiM3SpringStepSpatialFast(id ^ 0x464142, held ? 1.0f : 0.0f);
    const int elevation = (int)ImLerp(3.0f, 0.0f, press_t);
    ImGuiM3DrawElevation(window->DrawList, bb, rounding, elevation);
    ImGuiM3DrawContainer(window->DrawList, bb, rounding, ImGuiM3ColorU32(ImGuiM3Role_PrimaryContainer), 0, 0.0f);
    ImGuiM3State state = (hovered && held) ? ImGuiM3State_Pressed : hovered ? ImGuiM3State_Hovered : ImGuiM3State_Enabled;
    ImGuiM3DrawStateLayer(window->DrawList, bb, rounding, ImGuiM3Role_OnPrimaryContainer, state);

    const ImVec2 text_size = CalcTextSize(glyph, NULL, true);
    PushStyleColor(ImGuiCol_Text, ImGuiM3ColorU32(ImGuiM3Role_OnPrimaryContainer));
    // ink-centre the glyph; non-icon glyphs fall back to normal centred layout.
    ImVec2 ink_center;
    if (ImGuiM3IconInkCenterXY(GetFont(), GetFontSize(), glyph, ink_center))
        RenderText(ImVec2(bb.GetCenter().x - ink_center.x, bb.GetCenter().y - ink_center.y), glyph);
    else
        RenderTextClipped(bb.Min, bb.Max, glyph, NULL, &text_size, ImVec2(0.5f, 0.5f), &bb);
    PopStyleColor();

    if (tooltip)
        SetItemTooltip("%s", tooltip);
    IMGUI_TEST_ENGINE_ITEM_INFO(id, glyph, g.LastItemData.StatusFlags);
    return pressed;
}

void ImGui::M3Icon(const char* glyph, ImVec2 size_arg)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    const float s = size_arg.x > 0.0f ? size_arg.x : m.button_icon_default * m.density;
    const ImVec2 old = GetCursorPos();
    const ImVec2 pos = GetCursorScreenPos();
    Dummy(ImVec2(s, s));
    if (ImFont* icon = ImGuiM3IconFont())
    {
        // ink-centre the glyph in the s×s box; Material Symbols have asymmetric bearings.
        PushFont(icon, s);
        ImVec2 ink_center;
        const ImVec2 text_size = CalcTextSize(glyph, NULL, true);
        if (ImGuiM3IconInkCenterXY(icon, s, glyph, ink_center))
            SetCursorScreenPos(ImVec2(pos.x + s * 0.5f - ink_center.x, pos.y + s * 0.5f - ink_center.y));
        else
            SetCursorScreenPos(ImVec2(pos.x + (s - text_size.x) * 0.5f, pos.y + (s - text_size.y) * 0.5f));
        TextUnformatted(glyph);
        PopFont();
    }
    else
    {
        // icon face missing: scale whatever glyph is available.
        const ImVec2 text_size = CalcTextSize(glyph, NULL, true);
        const float font_size = ImClamp(GetFontSize() * s / ImMax(text_size.y, 1.0f), 6.0f, 128.0f);
        PushFont(NULL, font_size);
        SetCursorScreenPos(pos);
        TextUnformatted(glyph);
        PopFont();
    }
    SetCursorScreenPos(old + ImVec2(0.0f, s));
}

void ImGui::M3SectionHeader(const char* label, const char* icon)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    // M3 "emphasized" title-small: bold face plus primary colour.
    PushStyleColor(ImGuiCol_Text, ImGuiM3ColorU32(ImGuiM3Role_Primary));
    ImFont* bold = ImGuiM3FontBold();
    if (bold)
        PushFont(bold, GetFontSize());
    if (icon)
    {
        TextUnformatted(icon);
        SameLine();
    }
    TextUnformatted(label);
    if (bold)
        PopFont();
    PopStyleColor();
    M3Divider();
    (void)m;
}

void ImGui::M3Divider(ImGuiM3Role role)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    ImGuiWindow* window = GetCurrentWindow();
    const ImVec2 pos = window->DC.CursorPos;
    const ImVec2 end = ImVec2(ImMax(GetContentRegionAvail().x + GetCursorPosX(), pos.x + 1.0f), pos.y + m.divider_thickness * m.density);
    const ImRect bb(pos, end);
    ItemSize(ImVec2(0.0f, m.divider_thickness * m.density));
    if (ItemAdd(bb, 0))
        window->DrawList->AddRectFilled(bb.Min, bb.Max, ImGuiM3ColorU32(role));
}

void ImGui::M3CardBegin(const char* id, const char* title, const char* icon)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    PushID(id);
    PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f) * m.density);
    PushStyleVar(ImGuiStyleVar_WindowRounding, ImGuiM3Radius(ImGuiM3Shape_Large));
    PushStyleColor(ImGuiCol_ChildBg, ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainerLow));
    // AutoResizeY so a stack of cards each hug their contents.
    BeginChild("##card", ImVec2(0, 0),
               ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
               ImGuiWindowFlags_None);
    CardDepth++;
    if (title)
        M3SectionHeader(title, icon);
}

void ImGui::M3CardEnd()
{
    if (CardDepth > 0)
        CardDepth--;
    EndChild();
    PopStyleColor(1);
    PopStyleVar(2);
    PopID();
}

void ImGui::M3ListItem(const char* label, bool selected, bool* p_selected)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = GetCurrentWindow();
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();

    const ImVec2 label_size = CalcTextSize(label, NULL, true);
    const float row_h = m.list_item_height_1 * m.density;
    const ImVec2 pos = window->DC.CursorPos;
    const ImVec2 avail = GetContentRegionAvail();
    const ImRect bb(pos, pos + ImVec2(ImMax(avail.x, label_size.x + 24.0f), row_h));
    const ImGuiID id = window->GetID(label);

    ItemSize(bb.GetSize());
    bool pressed = false;
    if (ItemAdd(bb, id))
    {
        bool hovered, held;
        pressed = ButtonBehavior(bb, id, &hovered, &held);

        // selected items use secondary-container and a larger radius; hovered use a
        // state layer.
        const float rest_radius = selected ? ImGuiM3Radius(ImGuiM3Shape_Large) : ImGuiM3Radius(ImGuiM3Shape_ExtraSmall);
        const float press_radius = ImGuiM3Radius(ImGuiM3Shape_Large);
        const ImGuiM3ShapeRounding rounding = ImGuiM3MorphedRounding(bb.GetSize(), rest_radius, press_radius, hovered && held, id);

        if (selected)
            ImGuiM3PathRoundedRect(window->DrawList, bb, rounding, ImGuiM3ColorU32(ImGuiM3Role_SecondaryContainer));
        if (selected)
            ImGuiM3DrawStateLayer(window->DrawList, bb, rounding, ImGuiM3Role_OnSecondaryContainer,
                                  (hovered && held) ? ImGuiM3State_Pressed : hovered ? ImGuiM3State_Hovered : ImGuiM3State_Enabled);
        else
            ImGuiM3DrawStateLayer(window->DrawList, bb, rounding, ImGuiM3Role_OnSurface,
                                  (hovered && held) ? ImGuiM3State_Pressed : hovered ? ImGuiM3State_Hovered : ImGuiM3State_Enabled);

        PushStyleColor(ImGuiCol_Text, selected ? ImGuiM3ColorU32(ImGuiM3Role_OnSecondaryContainer) : ImGuiM3ColorU32(ImGuiM3Role_OnSurface));
        RenderText(ImVec2(bb.Min.x + 16.0f * m.density, bb.Min.y + (bb.GetHeight() - label_size.y) * 0.5f), label);
        PopStyleColor();
    }

    if (p_selected)
    {
        if (pressed && !selected)
            *p_selected = true;
        else if (pressed && selected && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            *p_selected = false;
    }
    IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags);
}

void ImGui::M3LinearProgress(float fraction, const ImVec2& size_arg)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    ImGuiWindow* window = GetCurrentWindow();
    const float track_h = m.progress_track_height * m.density;
    ImVec2 size = CalcItemSize(size_arg, CalcItemWidth(), track_h);
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, pos + size);
    ItemSize(size);
    if (!ItemAdd(bb, 0))
        return;

    fraction = ImSaturate(fraction);
    ImGuiM3PathRoundedRect(window->DrawList, bb, ImGuiM3ShapeRounding{ 0, 0, 0, 0 }, ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainerHighest));

    ImRect indicator(bb.Min, ImVec2(bb.Min.x + bb.GetWidth() * fraction, bb.Max.y));
    if (indicator.GetWidth() > 0.0f)
        ImGuiM3PathRoundedRect(window->DrawList, indicator, ImGuiM3ShapeRounding{ 0, 0, 0, 0 }, ImGuiM3ColorU32(ImGuiM3Role_Primary));
}

bool ImGui::M3Chip(const char* label, bool* p_selected)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    const bool selected = p_selected ? *p_selected : false;

    PushID(label);
    const float h = m.chip_height * m.density;
    const ImVec2 label_size = CalcTextSize(label, NULL, true);
    const float leading = m.chip_icon * m.density;

    const bool changed = M3Button(label, selected ? ImGuiM3Button_Filled : ImGuiM3Button_Outlined, ImVec2(label_size.x + leading + 24.0f * m.density, h));
    if (changed && p_selected)
        *p_selected = !selected;
    PopID();
    return changed;
}

void ImGui::M3StatusChip(const char* label, ImGuiM3Role role)
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    const ImVec2 label_size = CalcTextSize(label, NULL, true);
    const float h = m.chip_height * m.density;
    const ImVec2 pos = GetCursorScreenPos();
    const ImRect bb(pos, pos + ImVec2(label_size.x + 24.0f * m.density, h));
    const float r = ImGuiM3Radius(ImGuiM3Shape_Full);
    const ImGuiM3ShapeRounding rounding = ImGuiM3PillRadius(bb.GetSize(), r) > 0.0f
        ? ImGuiM3ShapeRounding{ ImMin(r, h * 0.5f), ImMin(r, h * 0.5f), ImMin(r, h * 0.5f), ImMin(r, h * 0.5f) }
        : ImGuiM3ShapeRounding{ 0, 0, 0, 0 };

    ImGuiWindow* window = GetCurrentWindow();
    ItemSize(bb.GetSize());
    if (ItemAdd(bb, 0))
    {
        ImGuiM3PathRoundedRect(window->DrawList, bb, rounding, ImGuiM3ColorU32(role));
        PushStyleColor(ImGuiCol_Text, ImGuiM3ColorU32(g_on_role_of[role]));
        RenderText(ImVec2(bb.Min.x + 12.0f * m.density, bb.Min.y + (bb.GetHeight() - label_size.y) * 0.5f), label);
        PopStyleColor();
    }
}

// Connected button group.

bool ImGui::M3ConnectedButtonGroup(const char* id, const char* const* labels, int count, int* selected, const ImWchar* icons)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems || count <= 0 || !labels || !selected || *selected < 0 || *selected >= count)
        return false;

    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    // size S: 40dp tall, 16dp inline padding, 2dp between segments (md.comp.button-group).
    const float h = m.button_height_default * m.density;
    const float gap = 2.0f * m.density;
    const float pad_x = 16.0f * m.density;
    const float outline_w = m.button_outline_width * m.density;
    const float icon_px = m.button_icon_default * m.density;   // 20dp
    const float icon_gap = 8.0f * m.density;

    const float avail = ImMax(GetContentRegionAvail().x, 1.0f);

    ImFont* icon_font = ImGuiM3IconFont();
    ImFont* medium = ImGuiM3FontMedium();
    ImFont* bold = ImGuiM3FontBold();

    // if the row is too narrow the whole thing scales down rather than dropping icons.
    const bool use_icons = (icons != NULL) && (icon_font != NULL);

    ImVector<float> widths;
    widths.resize(count);
    float total = gap * (count - 1);
    for (int i = 0; i < count; i++)
    {
        widths[i] = CalcTextSize(labels[i], NULL, true).x + pad_x * 2.0f;
        if (use_icons)
            widths[i] += icon_px + icon_gap;
        total += widths[i];
    }
    if (total <= avail)
    {
        const float extra = (avail - total) / (float)count;
        for (int i = 0; i < count; i++)
            widths[i] += extra;
    }
    else
    {
        const float scale = avail / total;
        for (int i = 0; i < count; i++)
            widths[i] *= scale;
    }

    // scale the icon with the (possibly shrunken) segment, quantized so the dynamic
    // font atlas only bakes a couple of sizes.
    const float fit = (total <= avail) ? 1.0f : avail / total;
    const float icon_fit = ImMax(0.5f, ImFloor(fit * 4.0f + 0.5f) / 4.0f);
    const float icon_px_draw = icon_px * icon_fit;
    const float icon_gap_draw = icon_gap * icon_fit;

    PushID(id);
    bool changed = false;
    int new_selection = *selected;

    for (int i = 0; i < count; i++)
    {
        if (i > 0)
            SameLine(0.0f, gap);
        PushID(i);

        const ImVec2 pos = window->DC.CursorPos;
        const ImRect bb(pos, pos + ImVec2(widths[i], h));
        const ImGuiID seg_id = window->GetID("##segment");
        ItemSize(bb.GetSize());
        if (ItemAdd(bb, seg_id))
        {
            bool hovered, held;
            const bool pressed = ButtonBehavior(bb, seg_id, &hovered, &held);
            const bool is_selected = (i == *selected);
            const ImGuiM3ShapeRounding rounding = ImGuiM3ConnectedSegmentRounding(bb.GetSize(), i, count);

            // selected segments use secondary-container; the rest rely on the 1dp outline.
            if (is_selected)
                ImGuiM3PathRoundedRect(window->DrawList, bb, rounding, ImGuiM3ColorU32(ImGuiM3Role_SecondaryContainer));

            const ImGuiM3State state = (hovered && held) ? ImGuiM3State_Pressed : hovered ? ImGuiM3State_Hovered : ImGuiM3State_Enabled;
            ImGuiM3DrawStateLayer(window->DrawList, bb, rounding,
                                  is_selected ? ImGuiM3Role_OnSecondaryContainer : ImGuiM3Role_OnSurface, state);

            // 1dp outline just inside the shared shape.
            ImGuiM3DrawContainer(window->DrawList, bb, rounding, 0, ImGuiM3ColorU32(ImGuiM3Role_Outline), outline_w);

            RenderNavCursor(bb, seg_id);

            // weight carries the selected state: selected is bold, the rest medium.
            ImFont* font = is_selected ? bold : medium;
            const ImU32 label_col = ImGuiM3ColorU32(is_selected ? ImGuiM3Role_OnSecondaryContainer : ImGuiM3Role_OnSurfaceVariant);
            const ImU32 icon_col = label_col;
            PushStyleColor(ImGuiCol_Text, label_col);

            char icon_utf8[8] = {};
            if (use_icons)
            {
                const int n = ImTextCharToUtf8(icon_utf8, (unsigned int)icons[i]);
                icon_utf8[n] = '\0';
            }
            if (font)
                PushFont(font, GetFontSize());
            const ImVec2 label_size = CalcTextSize(labels[i], NULL, true);
            if (font)
                PopFont();

            ImVec2 icon_size(0.0f, 0.0f);
            float icon_top = bb.GetCenter().y;
            if (use_icons)
            {
                // Align the icon's ink centre with the label's line-box centre.
                icon_top = bb.GetCenter().y - ImGuiM3IconInkCenter(icon_font, icon_px_draw, icon_utf8);
                PushFont(icon_font, icon_px_draw);
                icon_size = CalcTextSize(icon_utf8, NULL, true);
                PopFont();
            }

            const float content_w = (use_icons ? icon_size.x + icon_gap_draw : 0.0f) + label_size.x;
            float x = bb.GetCenter().x - content_w * 0.5f;

            if (use_icons)
            {
                PushFont(icon_font, icon_px_draw);
                PushStyleColor(ImGuiCol_Text, icon_col);
                RenderText(ImVec2(x, icon_top), icon_utf8);
                PopStyleColor();
                PopFont();
                x += icon_size.x + icon_gap_draw;
            }

            if (font)
                PushFont(font, GetFontSize());
            RenderTextClipped(ImVec2(x, bb.Min.y), ImVec2(bb.Max.x - 4.0f * m.density, bb.Max.y),
                              labels[i], NULL, &label_size, ImVec2(0.0f, 0.5f), &bb);
            if (font)
                PopFont();
            PopStyleColor();

            if (pressed && !is_selected)
            {
                new_selection = i;
                changed = true;
            }
        }
        PopID();
    }

    PopID();
    if (changed)
        *selected = new_selection;
    return changed;
}

// Theme editor.

void ImGui::M3ThemeEditor()
{
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();

    // source + generation knobs
    M3SectionHeader("Source");

    ImGuiColorEditFlags flags = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_AlphaPreviewHalf;

    // source_rgba is kept in lock-step with the seed by ResolveTheme, so the picker
    // shows the real value and commits edits.
    if (ImGui::ColorEdit4("##source", g_theme.source_rgba, flags))
    {
        g_theme.source = argbFromRgb((int)(ImSaturate(g_theme.source_rgba[0]) * 255.0f + 0.5f),
                                     (int)(ImSaturate(g_theme.source_rgba[1]) * 255.0f + 0.5f),
                                     (int)(ImSaturate(g_theme.source_rgba[2]) * 255.0f + 0.5f));
        ResolveTheme();
        ImGuiM3ApplyToStyle(1.0f);
        ImGuiM3WriteThemeFile(ImGuiM3GetThemeFile());
    }

    const char* variant_items = "tonal-spot\0expressive\0vibrant\0neutral\0monochrome\0fidelity\0content\0";
    int variant = (int)g_theme.variant;
    if (ImGui::Combo("Variant", &variant, variant_items))
    {
        g_theme.variant = (ImGuiM3Variant)variant;
        ResolveTheme();
        ImGuiM3ApplyToStyle(1.0f);
        ImGuiM3WriteThemeFile(ImGuiM3GetThemeFile());
    }

    const char* contrast_items = "standard\0medium\0high\0";
    int contrast = (int)g_theme.contrast;
    if (ImGui::Combo("Contrast", &contrast, contrast_items))
    {
        g_theme.contrast = (ImGuiM3Contrast)contrast;
        ResolveTheme();
        ImGuiM3ApplyToStyle(1.0f);
        ImGuiM3WriteThemeFile(ImGuiM3GetThemeFile());
    }

    bool dark = g_theme.dark;
    if (ImGui::Checkbox("Dark", &dark))
    {
        g_theme.dark = dark;
        ResolveTheme();
        ImGuiM3ApplyToStyle(1.0f);
        ImGuiM3WriteThemeFile(ImGuiM3GetThemeFile());
    }

    // live reload status
    M3SectionHeader("Theme file");
    ImGui::TextDisabled("%s", ImGuiM3GetThemeFile() ? ImGuiM3GetThemeFile() : "(no file — in-memory only)");
    if (ImGui::Button("Write preset to file"))
        ImGuiM3WriteThemeFile(ImGuiM3GetThemeFile());
    ImGui::SameLine();
    if (ImGui::Button("Reload now"))
    {
        const char* path = ImGuiM3GetThemeFile();
        if (path)
        {
            LoadThemeFile(path);
            ImGuiM3ApplyToStyle(1.0f);
            g_theme.reloaded = true;
        }
    }
    if (const char* msg = ImGuiM3ConsumeReloadMessage())
        ImGui::TextColored(ImGuiM3Color(ImGuiM3Role_Tertiary), "%s", msg);
    if (const char* err = ImGuiM3GetError())
        ImGui::TextColored(ImGuiM3Color(ImGuiM3Role_Error), "%s", err);

    // colour roles
    M3SectionHeader("Color roles");
    if (ImGui::SmallButton("Reset overrides"))
    {
        g_theme.role_flags[0] = 0;
        g_theme.role_flags[1] = 0;
        ResolveTheme();
        ImGuiM3ApplyToStyle(1.0f);
        ImGuiM3WriteThemeFile(ImGuiM3GetThemeFile());
    }

    // Three columns of role swatches, each with a hex read-out and a picker.
    const int per_column = (ImGuiM3Role_COUNT + 2) / 3;
    ImGui::Columns(3, "m3_roles", true);
    for (int i = 0; i < ImGuiM3Role_COUNT; i++)
    {
        if (i > 0 && i % per_column == 0)
            NextColumn();
        const ImGuiM3Role role = (ImGuiM3Role)i;
        PushID((int)role);
        ImGui::ColorButton("##c", ImGuiM3Color(role), flags, ImVec2(28.0f, 18.0f));
        SameLine();
        ImGui::TextUnformatted(ImGuiM3RoleName(role));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            g_theme.overrides[role] = ImGuiM3Color(role);
            SetRoleOverridden(role, false);
            ResolveTheme();
            ImGuiM3ApplyToStyle(1.0f);
        }
        PopID();
    }
    ImGui::Columns(1);

    // shape and metric tokens
    M3SectionHeader("Shape and metrics");
    ImGui::SliderFloat("Density", &g_theme.metrics.density, 0.5f, 2.0f, "%.2fx");
    ImGui::SliderFloat("Shape scale", &g_theme.metrics.shape_scale, 0.0f, 2.0f, "%.2f");
    ImGui::TextDisabled("corner scale: none %.0f  xs %.0f  s %.0f  m %.0f  l %.0f  l+ %.0f  xl %.0f  xl+ %.0f  xxl %.0f",
                        m.corner_none, m.corner_xs, m.corner_s, m.corner_m, m.corner_l, m.corner_l_increased, m.corner_xl,
                        m.corner_xl_increased, m.corner_xxl);
    ImGui::TextDisabled("button %.0f  icon button %.0f  fab %.0f  list item %.0f  tab %.0f",
                        m.button_height_default, m.icon_button_size, m.fab_size, m.list_item_height_1, m.tab_height);

    // component showcase
    M3SectionHeader("Components");
    if (M3Button("Filled", ImGuiM3Button_Filled, ImVec2(0, 0))) {}
    ImGui::SameLine();
    if (M3Button("Tonal", ImGuiM3Button_Tonal, ImVec2(0, 0))) {}
    ImGui::SameLine();
    if (M3Button("Elevated", ImGuiM3Button_Elevated, ImVec2(0, 0))) {}
    ImGui::SameLine();
    if (M3Button("Outlined", ImGuiM3Button_Outlined, ImVec2(0, 0))) {}
    ImGui::SameLine();
    if (M3Button("Text", ImGuiM3Button_Text, ImVec2(0, 0))) {}

    static bool sw = true;
    if (M3Switch("Switch", &sw))
        MarkItemEdited(0);
    ImGui::SameLine();
    static bool chip = true;
    M3Chip("Chip", &chip);
    ImGui::SameLine();
    M3StatusChip("ERROR", ImGuiM3Role_Error);

    M3LinearProgress(0.65f);
    (void)ImGui::IsItemHovered();
}
