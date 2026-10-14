// Headless tests for the stb_truetype variable-font extension: fvar parsing,
// axis normalization, gvar outlines and HVAR advances. The two shipped faces are
// committed, so this guards them against a bad download or a regression in the
// variation code without needing a GPU.

#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

static void expect(bool condition, const char* what)
{
    g_checks++;
    if (!condition)
    {
        g_failures++;
        std::printf("FAIL: %s\n", what);
    }
}

#define TAG(a, b, c, d) ((stbtt_uint32)(((unsigned)(a) << 24) | ((unsigned)(b) << 16) | ((unsigned)(c) << 8) | (unsigned)(d)))

static std::vector<unsigned char> loadFile(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.good())
        return {};
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static const char* findFont(const char* name)
{
    static std::string resolved;
    const char* prefixes[] = {"assets/font/", "../assets/font/", "../../assets/font/"};
    for (const char* p : prefixes)
    {
        resolved = std::string(p) + name;
        std::ifstream probe(resolved);
        if (probe.good())
            return resolved.c_str();
    }
    return nullptr;
}

static int axisIndex(const stbtt_fontinfo* info, stbtt_uint32 tag)
{
    const int n = stbtt_GetVariationAxisCount(info);
    for (int i = 0; i < n; i++)
        if (stbtt_GetVariationAxisTag(info, i) == tag)
            return i;
    return -1;
}

static void setAxis(stbtt_fontinfo* info, stbtt_uint32 tag, float value)
{
    stbtt_SetVariationCoords(info, &tag, &value, 1);
}

static long shapeHash(const stbtt_fontinfo* info, int glyph)
{
    stbtt_vertex* v = 0;
    const int n = stbtt_GetGlyphShape(info, glyph, &v);
    long h = 1469598103L;
    for (int i = 0; i < n; i++)
        h = h * 131 + v[i].x * 7 + v[i].y * 13 + v[i].cx * 3 + v[i].cy * 5 + v[i].type;
    stbtt_FreeShape(info, v);
    return h;
}

static int glyphAdvance(const stbtt_fontinfo* info, int glyph)
{
    int a = 0;
    stbtt_GetGlyphHMetrics(info, glyph, &a, 0);
    return a;
}

static void testFace(const char* name, int expected_axes, float wght_min, float wght_def, float wght_max,
                     bool has_rond, bool has_wdth)
{
    const char* path = findFont(name);
    if (!path)
    {
        std::printf("SKIP: %s not found next to the build\n", name);
        return;
    }
    std::vector<unsigned char> buf = loadFile(path);
    expect(!buf.empty(), "variable font file reads");

    stbtt_fontinfo info;
    expect(stbtt_InitFont(&info, buf.data(), stbtt_GetFontOffsetForIndex(buf.data(), 0)) != 0,
           "variable font parses");
    expect(info.hasVariations == 1, "font reports variations");

    const int axis_count = stbtt_GetVariationAxisCount(&info);
    char msg[128];
    std::snprintf(msg, sizeof(msg), "%s exposes %d axes", name, expected_axes);
    expect(axis_count == expected_axes, msg);

    const int wi = axisIndex(&info, TAG('w', 'g', 'h', 't'));
    expect(wi >= 0, "wght axis is present");
    expect(std::fabs(stbtt_GetVariationAxisMin(&info, wi) - wght_min) < 0.5f, "wght min matches");
    expect(std::fabs(stbtt_GetVariationAxisDefault(&info, wi) - wght_def) < 0.5f, "wght default matches");
    expect(std::fabs(stbtt_GetVariationAxisMax(&info, wi) - wght_max) < 0.5f, "wght max matches");
    expect(axisIndex(&info, TAG('R', 'O', 'N', 'D')) >= 0 == has_rond, "ROND axis presence matches");
    expect(axisIndex(&info, TAG('w', 'd', 't', 'h')) >= 0 == has_wdth, "wdth axis presence matches");

    const int gH = stbtt_FindGlyphIndex(&info, 'H');
    expect(gH > 0, "glyph 'H' resolves");

    // baseline (no coords) is the default instance.
    const long base_shape = shapeHash(&info, gH);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBox(&info, gH, &x0, &y0, &x1, &y1);
    const int base_h = y1 - y0;

    // heavier weight changes the outline (gvar deltas are applied).
    setAxis(&info, TAG('w', 'g', 'h', 't'), wght_max);
    const long heavy_shape = shapeHash(&info, gH);
    expect(heavy_shape != base_shape, "gvar changes the outline at a different weight");
    stbtt_GetGlyphBox(&info, gH, &x0, &y0, &x1, &y1);
    expect((y1 - y0) >= base_h, "a heavier weight does not shrink the glyph box");

    // setting the default coordinate reproduces the default instance.
    setAxis(&info, TAG('w', 'g', 'h', 't'), wght_def);
    expect(shapeHash(&info, gH) == base_shape, "resetting to the default reproduces the base outline");

    if (has_wdth)
    {
        const int wi2 = axisIndex(&info, TAG('w', 'd', 't', 'h'));
        setAxis(&info, TAG('w', 'd', 't', 'h'), stbtt_GetVariationAxisMin(&info, wi2));
        const int narrow = glyphAdvance(&info, gH);
        setAxis(&info, TAG('w', 'd', 't', 'h'), stbtt_GetVariationAxisMax(&info, wi2));
        const int wide = glyphAdvance(&info, gH);
        expect(wide > narrow, "HVAR widens the advance along the width axis");
    }

    if (has_rond)
    {
        setAxis(&info, TAG('R', 'O', 'N', 'D'), 0.0f);
        const long square = shapeHash(&info, gH);
        setAxis(&info, TAG('R', 'O', 'N', 'D'), 100.0f);
        const long rounded = shapeHash(&info, gH);
        expect(square != rounded, "ROND changes the outline (rounded document cut)");
    }
}

int main()
{
    testFace("GoogleSansFlex.ttf", 6, 1.0f, 400.0f, 1000.0f, /*rond*/ true, /*wdth*/ true);
    testFace("RobotoFlex.ttf", 13, 100.0f, 400.0f, 1000.0f, /*rond*/ false, /*wdth*/ true);

    // A static face must degrade cleanly: zero axes, coords are a no-op.
    const char* icon = findFont("MaterialSymbolsRounded.ttf");
    if (icon)
    {
        std::vector<unsigned char> buf = loadFile(icon);
        stbtt_fontinfo info;
        stbtt_InitFont(&info, buf.data(), 0);
        expect(stbtt_GetVariationAxisCount(&info) == 0, "a static instance reports no axes");
        setAxis(&info, TAG('w', 'g', 'h', 't'), 900.0f); // must not crash / must be ignored
        expect(shapeHash(&info, stbtt_FindGlyphIndex(&info, 0xE5CA)) != 0, "static outline still builds");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
