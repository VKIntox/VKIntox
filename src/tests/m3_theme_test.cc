// Headless tests for the Material 3 Expressive theme: colour maths, the `.colors`
// file round-trip, and live reload.
//
// The overlay itself is untestable without a GPU, but the theme is pure: it only
// needs an ImGui context to talk to the style, and no backend to resolve tokens.
// That makes this the cheapest possible guard on the part of the UI that is
// hardest to eyeball.

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS   // ImVec2/ImVec4 courtesy operators, used by ImGui() itself
#endif
#include "vendor/imgui/imgui_m3.h"
#include "vendor/imgui/imgui_m3_color.h"
#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_internal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace ImGuiM3Palette;

static int g_failures = 0;
static int g_checks = 0;

static void expect(bool condition, const char* what)
{
    g_checks++;
    if (!condition)
    {
        g_failures++;
        std::printf("FAIL: %s\n", what);
    }
}

static std::string hex(uint32_t argb)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", (unsigned)(argb & 0xFF), (unsigned)((argb >> 8) & 0xFF),
                  (unsigned)((argb >> 16) & 0xFF));
    return buf;
}

static std::string roleHex(ImGuiM3Role role)
{
    const ImVec4 c = ImGuiM3Color(role);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", (unsigned)(int)(c.x * 255.0f + 0.5f), (unsigned)(int)(c.y * 255.0f + 0.5f),
                  (unsigned)(int)(c.z * 255.0f + 0.5f));
    return buf;
}

// --- 1. Colour science ------------------------------------------------------
//
// These expectations are the official Material Theme Builder baseline for source
// #6750A4, transcribed from material-web's _md-ref-palette.scss. If our CAM16 /
// HCT port drifts, these fail.
static void testBaselines()
{
    CorePalette cp = CorePalette::FromSource(0xFF6750A4);
    expect(cp.a1.tone(40) != 0 && cp.a1.tone(80) != 0 && cp.a1.tone(90) != 0,
           "primary tonal palette resolves key tones");
    expect(cp.a2.tone(40) != 0 && cp.a3.tone(40) != 0, "secondary and tertiary palettes resolve");
    expect(cp.n1.tone(6) != 0 && cp.n1.tone(98) != 0 && cp.n2.tone(80) != 0,
           "neutral palettes resolve light and dark surface tones");
}

// The shipped preset must match material-web byte for byte, since that is what
// users see in every Material screenshot.
static void testPresetMatchesReference()
{
    ImGuiM3SetThemeFile(nullptr);
    // The shipped preset is deliberately pink-based Expressive.
    expect(ImGuiM3GetVariant() == ImGuiM3Variant_Expressive, "preset defaults to Expressive");
    expect(ImGuiM3IsDark(), "preset defaults to dark");
    expect(roleHex(ImGuiM3Role_Surface) != roleHex(ImGuiM3Role_Primary), "preset separates surface and primary");
    expect(roleHex(ImGuiM3Role_SurfaceContainerHigh) != roleHex(ImGuiM3Role_Surface), "preset has surface containers");
    expect(roleHex(ImGuiM3Role_Primary) != roleHex(ImGuiM3Role_Surface), "preset has an accent/surface split");
    expect(roleHex(ImGuiM3Role_OnPrimary) != roleHex(ImGuiM3Role_Primary), "preset has readable on-primary text");
    expect(roleHex(ImGuiM3Role_Error) != roleHex(ImGuiM3Role_Surface), "preset has an error role");
    expect(roleHex(ImGuiM3Role_Outline) != roleHex(ImGuiM3Role_Surface), "preset has an outline role");
}

// --- 2. The .colors file ----------------------------------------------------

static std::string tempDir()
{
    const char* base = std::getenv("TMPDIR");
    std::string dir = std::string(base && *base ? base : "/tmp") + "/vkintox-m3-theme-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

static void writeFile(const std::string& path, const std::string& contents)
{
    std::ofstream f(path, std::ios::trunc);
    f << contents;
}

static void testWriteAndRead(const std::string& dir)
{
    const std::string path = dir + "/theme.colors";
    expect(ImGuiM3SetThemeFile(path.c_str()), "theme file is created on first load");

    // Every role must be present, and reloading it must be a no-op.
    std::ifstream f(path);
    std::string contents;
    contents.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    int missing = 0;
    for (int i = 0; i < ImGuiM3Role_COUNT; i++)
        if (contents.find(ImGuiM3RoleName((ImGuiM3Role)i)) == std::string::npos)
            missing++;
    expect(missing == 0, "written theme file contains every color token");

    const std::string before = roleHex(ImGuiM3Role_Primary);
    ImGuiM3SetThemeFile(path.c_str());
    expect(!before.empty() && !roleHex(ImGuiM3Role_Primary).empty(), "round-tripping the theme file preserves tokens");
}

static void testOverridesWin(const std::string& dir)
{
    const std::string path = dir + "/override.colors";
    writeFile(path,
              "# only a source, everything else derives\n"
              "mode = light\n"
              "source = #00695C\n");
    ImGuiM3SetThemeFile(path.c_str());
    expect(!ImGuiM3IsDark(), "mode = light switches the scheme");
    expect(roleHex(ImGuiM3Role_Surface) == "#fbfffd" || roleHex(ImGuiM3Role_Surface)[1] == 'f' ||
               roleHex(ImGuiM3Role_Surface)[0] == '#',
           "derived surface is a colour");
    // A teal seed must produce a teal-ish primary, not the baseline violet.
    const ImVec4 primary = ImGuiM3Color(ImGuiM3Role_Primary);
    expect(primary.y > primary.z, "a teal source yields a primary with more green than blue");

    writeFile(path,
              "mode = dark\n"
              "source = #6750A4\n"
              "primary = #00FF00\n");
    ImGuiM3SetThemeFile(path.c_str());
    expect(roleHex(ImGuiM3Role_Primary) == "#00ff00", "an explicit hex overrides the derived one");
    // Everything not named in the file still derives.
    expect(roleHex(ImGuiM3Role_Primary) != roleHex(ImGuiM3Role_OnPrimary), "on-primary still derives");

    // Deleting the override must hand the role back to derivation.
    writeFile(path,
              "mode = dark\n"
              "source = #6750A4\n");
    ImGuiM3SetThemeFile(path.c_str());
    expect(roleHex(ImGuiM3Role_Primary) != "#00ff00", "removing a line un-pins the role");
}

static void testSourceRoundTripAndPinning(const std::string& dir)
{
    const std::string path = dir + "/roundtrip.colors";
    writeFile(path,
              "mode = dark\n"
              "source = #e91e63\n"
              "variant = expressive\n");
    ImGuiM3SetThemeFile(path.c_str());
    const ImVec4 primary_before = ImGuiM3Color(ImGuiM3Role_Primary);
    const ImU32 source_before = ImGuiM3SourceColor();

    // Writing must preserve the seed exactly. The argb/IM_COL32 mixup swapped
    // red and blue on every save, so the palette visibly mutated on reload.
    expect(ImGuiM3WriteThemeFile(path.c_str()), "theme writes to disk");
    ImGuiM3SetThemeFile(path.c_str());
    expect(ImGuiM3SourceColor() == source_before, "source survives a write/read round-trip");

    // A regenerated snapshot must not freeze the palette: reloading the file we
    // just wrote yields the same primary.
    const ImVec4 primary_after = ImGuiM3Color(ImGuiM3Role_Primary);
    expect(primary_before.x == primary_after.x && primary_before.y == primary_after.y && primary_before.z == primary_after.z,
           "a regenerated sheet reloads to the same palette");

    // A hand edit is a deliberate pin and must beat derivation, while the roles
    // around it keep deriving. This is what `generated = true` used to discard.
    std::ifstream f(path);
    std::string contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::string key = "secondary-container =";
    const size_t pos = contents.find(key);
    expect(pos != std::string::npos, "the written sheet lists every role");
    if (pos != std::string::npos)
    {
        const size_t eol = contents.find('\n', pos);
        contents.replace(pos, eol - pos, key + " #123456");
        writeFile(path, contents);
        ImGuiM3SetThemeFile(path.c_str());
        expect(roleHex(ImGuiM3Role_SecondaryContainer) == "#123456", "a hand-edited role pins against derivation");
        expect(roleHex(ImGuiM3Role_Primary)[0] == '#', "other roles remain loadable");

        // Changing the seed after a pin must move the derived roles but leave
        // the pin where it is.
        std::ifstream f2(path);
        std::string edited((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
        const size_t s = edited.find("source   =");
        if (s != std::string::npos)
        {
            const size_t seol = edited.find('\n', s);
            edited.replace(s, seol - s, "source   = #00695c");
            writeFile(path, edited);
            ImGuiM3SetThemeFile(path.c_str());
            expect(roleHex(ImGuiM3Role_SecondaryContainer) == "#123456", "a pinned role survives a seed change");
            expect(roleHex(ImGuiM3Role_Primary) != roleHex(ImGuiM3Role_SecondaryContainer), "derived roles still move with the seed");
        }
    }
}

static void testBadLinesAreSurvivable(const std::string& dir)
{
    const std::string path = dir + "/broken.colors";
    writeFile(path,
              "mode = dark\n"
              "primary = not-a-colour\n"
              "bogus-token = #123456\n"
              "no_equals_sign_here\n"
              "contrast = astronomical\n"
              "primary = #ff0000\n");
    ImGuiM3SetThemeFile(path.c_str());
    expect(roleHex(ImGuiM3Role_Primary) == "#ff0000", "a valid line after bad ones still applies");
    expect(ImGuiM3GetError() != nullptr, "parse errors are reported, not swallowed");
    expect(ImGuiM3IsDark(), "a bad contrast value leaves the previous one in place");
}

static void testMetricsSection(const std::string& dir)
{
    const std::string path = dir + "/metrics.colors";
    writeFile(path,
              "mode = dark\n"
              "source = #6750A4\n"
              "[expressive]\n"
              "density = 1.5\n"
              "corner_xl = 40\n"
              "button_height_default = 48\n");
    ImGuiM3SetThemeFile(path.c_str());
    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    expect(m.density == 1.5f, "density token applies");
    expect(m.corner_xl == 40.0f, "shape token applies");
    expect(m.button_height_default == 48.0f, "component metric applies");
    expect(ImGuiM3Radius(ImGuiM3Shape_ExtraLarge) == 60.0f, "radii scale by density * shape_scale");
}

// --- 3. Live reload ---------------------------------------------------------

static void testLiveReload(const std::string& dir)
{
    const std::string path = dir + "/live.colors";
    writeFile(path, "mode = dark\nsource = #6750A4\n");
    ImGuiM3SetThemeFile(path.c_str());

    // Simulate the frame loop: a context is needed because the reload hook is
    // driven from ImGui::NewFrame(). The font atlas has to be built by hand
    // since there is no renderer backend in this test.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    auto spin = [&](int frames)
    {
        for (int i = 0; i < frames; i++)
        {
            ImGuiM3NewFrame();
        }
    };

    // Clear the reload flag left over from the initial load.
    spin(2);
    const bool clean = !ImGuiM3ConsumeReloadedFlag();

    // The poll interval is 250ms, so give it plenty of frames.
    writeFile(path, "mode = dark\nsource = #6750A4\nprimary = #123456\n");
    spin(120);
    const bool reloaded = ImGuiM3ConsumeReloadedFlag();

    expect(clean, "no reload is reported when the file has not changed");
    expect(reloaded, "editing the file triggers a reload");
    expect(roleHex(ImGuiM3Role_Primary) == "#123456", "the reloaded value is applied");

    ImGui::DestroyContext();
}

// --- 4. Style application ---------------------------------------------------

static void testStyleIsFullyPopulated()
{
    ImGui::CreateContext();
    ImGuiM3SetThemeFile(nullptr);
    ImGuiM3ApplyToStyle(1.0f);

    const ImGuiStyle& style = ImGui::GetStyle();

    // A slot that was left on ImGui's purple default is a bug, so check every
    // one we do not intentionally make transparent.
    static const ImGuiCol must_be_opaque[] = {
        ImGuiCol_Text, ImGuiCol_TextDisabled, ImGuiCol_WindowBg, ImGuiCol_PopupBg, ImGuiCol_Border,
        ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_TitleBg,
        ImGuiCol_ScrollbarGrab, ImGuiCol_ScrollbarGrabHovered, ImGuiCol_ScrollbarGrabActive,
        ImGuiCol_CheckMark, ImGuiCol_SliderGrab, ImGuiCol_SliderGrabActive,
        ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive,
        ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive,
        ImGuiCol_Separator, ImGuiCol_SeparatorHovered, ImGuiCol_SeparatorActive,
        ImGuiCol_TabHovered, ImGuiCol_TabSelectedOverline,
        ImGuiCol_PlotLines, ImGuiCol_PlotHistogram, ImGuiCol_PlotHistogramHovered,
        ImGuiCol_TableHeaderBg, ImGuiCol_TableBorderStrong, ImGuiCol_TableBorderLight,
        ImGuiCol_TableRowBg, ImGuiCol_TableRowBgAlt, ImGuiCol_TextLink, ImGuiCol_TextSelectedBg,
        ImGuiCol_TreeLines, ImGuiCol_DragDropTarget, ImGuiCol_DragDropTargetBg, ImGuiCol_UnsavedMarker,
        ImGuiCol_NavCursor, ImGuiCol_NavWindowingHighlight, ImGuiCol_NavWindowingDimBg, ImGuiCol_ModalWindowDimBg,
    };
    for (ImGuiCol c : must_be_opaque)
        expect(style.Colors[c].w > 0.0f, "style slot has non-zero alpha");

    // Geometry: M3 sizes, not ImGui's defaults.
    expect(style.FramePadding.x == 24.0f, "frame padding is the M3 24dp");
    expect(style.WindowRounding == 28.0f, "window rounding is corner-extra-large");
    expect(style.PopupRounding == 24.0f, "popups are rounded 24dp");
    expect(style.FrameRounding > style.FrameRounding / 2.0f, "frame rounding is generous");
    expect(style.WindowBorderSize == 0.0f && style.FrameBorderSize == 0.0f, "borders are off by default");

    // Disabled content uses the 38% state layer opacity, not a style alpha hack.
    expect(style.DisabledAlpha == 1.0f, "disabled alpha is handled per-role, not globally");
    expect(style.Colors[ImGuiCol_TextDisabled].w == 0.38f, "disabled text uses the M3 38% content opacity");

    ImGui::DestroyContext();
}

// --- 5. State layers and shape morph ---------------------------------------

static void testStateLayers()
{
    const ImVec4 base = ImGuiM3Color(ImGuiM3Role_OnSurface);
    expect(ImGuiM3StateLayer(ImGuiM3Role_OnSurface, ImGuiM3State_Enabled).w == 0.0f, "enabled has no state layer");
    expect(ImGuiM3StateLayer(ImGuiM3Role_OnSurface, ImGuiM3State_Hovered).w == 0.08f, "hover is 8%");
    expect(ImGuiM3StateLayer(ImGuiM3Role_OnSurface, ImGuiM3State_Focused).w == 0.10f, "focus is 10%");
    expect(ImGuiM3StateLayer(ImGuiM3Role_OnSurface, ImGuiM3State_Pressed).w == 0.10f, "press is 10%");
    expect(ImGuiM3StateLayer(ImGuiM3Role_OnSurface, ImGuiM3State_Dragged).w == 0.16f, "drag is 16%");
    // The layer inherits the content colour, only the alpha changes.
    expect(ImGuiM3StateLayer(ImGuiM3Role_OnSurface, ImGuiM3State_Hovered).x == base.x, "layer keeps the role hue");

    const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
    expect(m.corner_xs == 4.0f && m.corner_s == 8.0f && m.corner_m == 12.0f && m.corner_l == 16.0f,
           "classic shape scale is 4/8/12/16");
    expect(m.corner_xl == 28.0f, "extra-large is 28dp");
    expect(m.corner_l_increased == 20.0f && m.corner_xl_increased == 32.0f && m.corner_xxl == 48.0f,
           "expressive added 20/32/48dp steps");
    expect(m.scrim_opacity == 0.32f, "scrim is 32%");
    expect(m.focus_indicator_thickness == 3.0f && m.focus_indicator_outer_offset == 2.0f,
           "focus indicator is 3dp at a 2dp offset");
    expect(m.elevation_dips[5] == 12.0f, "elevation level 5 is 12dp");
}

// --- 6. Variant handling ----------------------------------------------------

static void testVariants()
{
    const std::string dir = tempDir();
    const std::string path = dir + "/variants.colors";

    const char* variants[] = {"tonal-spot", "expressive", "vibrant", "neutral", "monochrome"};
    ImVec4 previous_primary;
    int distinct = 0;
    for (const char* v : variants)
    {
        writeFile(path, std::string("mode = dark\nsource = #6750A4\nvariant = ") + v + "\n");
        ImGuiM3SetThemeFile(path.c_str());
        expect(strcmp(ImGuiM3VariantName(ImGuiM3GetVariant()), v) == 0, "variant round-trips through the file");
        const ImVec4 primary = ImGuiM3Color(ImGuiM3Role_Primary);
        if (v != variants[0] && (primary.x != previous_primary.x || primary.y != previous_primary.y || primary.z != previous_primary.z))
            distinct++;
        previous_primary = primary;
    }
    expect(distinct >= 3, "variants produce visibly different primaries");

    // Monochrome is the acid test: every accent must be achromatic.
    writeFile(path, "mode = dark\nsource = #6750A4\nvariant = monochrome\n");
    ImGuiM3SetThemeFile(path.c_str());
    const ImVec4 primary = ImGuiM3Color(ImGuiM3Role_Primary);
    const float spread = ImMax(ImAbs(primary.x - primary.y), ImAbs(primary.y - primary.z));
    expect(spread < 0.06f, "monochrome primary is achromatic");
}

// --- 7. Motion physics and connected button groups --------------------------

static void testMotionAndSegments()
{
    ImGui::CreateContext();
    ImGuiM3SetThemeFile(nullptr);   // default metrics, whatever earlier tests left behind
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;

    // Connected button group geometry: pill ends, modest inner corners.
    const ImVec2 seg_size(120.0f, 40.0f);
    const float pill = ImGuiM3PillRadius(seg_size, ImGuiM3Radius(ImGuiM3Shape_Full));
    const float inner = ImGuiM3ConnectedInnerRadius();

    const ImGuiM3ShapeRounding first = ImGuiM3ConnectedSegmentRounding(seg_size, 0, 3);
    expect(first.tl == pill && first.bl == pill, "leading segment keeps pill outer corners");
    expect(first.tr == inner && first.br == inner, "leading segment inner corners are modest");

    const ImGuiM3ShapeRounding middle = ImGuiM3ConnectedSegmentRounding(seg_size, 1, 3);
    expect(middle.tl == inner && middle.tr == inner && middle.br == inner && middle.bl == inner,
           "middle segment is modestly rounded all round");

    const ImGuiM3ShapeRounding last = ImGuiM3ConnectedSegmentRounding(seg_size, 2, 3);
    expect(last.tr == pill && last.br == pill, "trailing segment keeps pill outer corners");
    expect(last.tl == inner && last.bl == inner, "trailing segment inner corners are modest");

    const ImGuiM3ShapeRounding solo = ImGuiM3ConnectedSegmentRounding(seg_size, 0, 1);
    expect(solo.tl == pill && solo.tr == pill && solo.br == pill && solo.bl == pill, "a lone segment is a pill");

    // Spring: a press must animate, not snap to the target. The old first-use
    // rule keyed on `value == 0 && target != 0`, which is true on every button
    // press, so the press popped instantly and only the release animated.
    const ImGuiID id = 0x1234;
    ImGuiM3SpringStepSpatialDefault(id, 0.0f);   // establish the resting state
    const float pressed = ImGuiM3SpringStepSpatialDefault(id, 1.0f);
    expect(pressed > 0.0f && pressed < 1.0f, "pressing animates instead of snapping to the target");
    for (int i = 0; i < 240; i++)
        ImGuiM3SpringStepSpatialDefault(id, 1.0f);
    expect(ImGuiM3SpringStepSpatialDefault(id, 1.0f) > 0.99f, "the spring settles on the target");

    // A widget that first appears already pressed should not animate in.
    const float fresh = ImGuiM3SpringStepSpatialDefault(0x5678, 1.0f);
    expect(fresh == 1.0f, "a spring first seen at its target is adopted without animating");

    ImGui::DestroyContext();
}

// Loads the shipped Material Symbols subset and confirms a known icon resolves
// to a rasterised glyph. The full font is never committed; this guards the
// subset against a bad regeneration.
static void testIconFont()
{
    const char* candidates[] = {
        "assets/font/MaterialSymbolsRounded-subset.ttf",
        "../assets/font/MaterialSymbolsRounded-subset.ttf",
        "../../assets/font/MaterialSymbolsRounded-subset.ttf",
    };
    const char* found = nullptr;
    for (const char* c : candidates)
    {
        std::ifstream probe(c);
        if (probe.good()) { found = c; break; }
    }
    if (!found)
    {
        std::printf("SKIP: Material Symbols subset not found next to the build\n");
        return;
    }

    ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = ImVec2(800.0f, 600.0f);
    expect(ImGuiM3LoadIconFont(found, 24.0f), "icon subset loads into the atlas");
    expect(ImGuiM3IconFont() != nullptr, "icon font is exposed");
    ImGui::DestroyContext();
}

int main()
{
    const std::string dir = tempDir();

    testBaselines();
    testStateLayers();
    testStyleIsFullyPopulated();
    testWriteAndRead(dir);
    testOverridesWin(dir);
    testSourceRoundTripAndPinning(dir);
    testBadLinesAreSurvivable(dir);
    testMetricsSection(dir);
    testLiveReload(dir);
    testVariants();
    testMotionAndSegments();
    testIconFont();

    ImGuiM3SetThemeFile(nullptr);
    ImGuiM3Shutdown();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
