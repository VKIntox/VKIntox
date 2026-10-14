# Rendering widgets headlessly

The overlay normally needs a GPU, a swapchain and a running game. Widget
behaviour, geometry and timing do not: Dear ImGui is a CPU-side library that
bakes every widget into vertex buffers before any backend touches it. A small
harness can create a context, submit the widget, and read back the triangles it
produced. That is enough to answer most questions about a UI change — where the
pixels land, how wide a button is, whether an animation settles — deterministically
and in a fraction of a second, with no device and no game.

This is how the progress indicator, the connected button group, the fold
animation and the button metrics were checked.

## What it is good for

- Geometry: item rectangles, alignment, padding, whether a bar spans the box it
  should, whether a label overflows.
- Colour and shape: the actual triangles and their vertex colours, including the
  anti-aliasing fringe ImGui bakes into vertex alpha.
- Animation: any spring, tween or time-driven value, frame by frame, because the
  clock is `io.DeltaTime` and the harness owns it.
- Cost: how many microseconds a widget spends building its geometry, and how that
  scales with size.

## What it cannot show

Anything sampled from a texture: text glyphs and icon-font codepoints live in the
font atlas, and a triangle-only rasteriser will draw their quads as solid blocks.
Read the layout from vertex positions and item rects instead, or stub the text and
check the surrounding geometry. A GPU capture is still the tool for judging
texture filtering.

## The shape of a harness

1. Create a context and declare that the renderer handles texture updates.
   Without `ImGuiBackendFlags_RendererHasTextures`, `NewFrame` asserts that the
   font atlas was pre-built by a backend that no longer exists.
2. Install the real fonts. Layout depends on them: this project's text faces are
   variable instances (weights are different advances) with the icon font merged
   in, and a button sized against the wrong face is sized wrong. Mirror what
   `ImGuiOverlay` does at startup.
3. Set `DisplaySize` and `DeltaTime`, then drive `NewFrame` / widget / `Render`
   yourself.
4. Read `ImGui::GetDrawData()`: for each command list, for each command, walk
   `IdxBuffer` in threes and fetch `VtxBuffer` entries. Skip commands with a
   `UserCallback`.
5. Rasterise or measure. Rasterising a PPM is the quickest way to *look* at the
   result; measuring rectangles is the quickest way to *assert* it.

## A complete example

Save, build against the already-compiled ImGui
objects, run.

```cpp
#define IMGUI_DEFINE_MATH_OPERATORS
#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_internal.h"
#include "vendor/imgui/imgui_m3.h"
#include "overlay/ui_icons.hh"

#include <cstdio>
#include <vector>

// One triangle with per-vertex colour, which is all an ImGui draw list contains.
struct Tri { ImVec2 a, b, c; ImU32 ca, cb, cc; };

static void InstallFonts()
{
    constexpr float kBody = 14.0f;
    constexpr ImU32 kTagWght = 0x77676874u, kTagROND = 0x524F4E44u, kTagOpsz = 0x6F70737Au;
    const char* body  = "assets/font/RobotoFlex.ttf";
    const char* doc   = "assets/font/GoogleSansFlex.ttf";
    const char* icons = "assets/font/MaterialSymbolsRoundedBold.ttf";

    auto addFace = [&](const char* path, float wght, bool rounded, float opsz) -> ImFont* {
        ImFontConfig cfg;
        cfg.SizePixels = kBody;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        cfg.PixelSnapH = true;
        int n = 0;
        cfg.VariationAxisTags[n] = kTagWght; cfg.VariationAxisValues[n++] = wght;
        cfg.VariationAxisTags[n] = kTagOpsz; cfg.VariationAxisValues[n++] = opsz;
        if (rounded) { cfg.VariationAxisTags[n] = kTagROND; cfg.VariationAxisValues[n++] = 100.0f; }
        cfg.VariationAxisCount = n;
        ImFont* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(path, kBody, &cfg);
        if (f)
            ImGuiM3MergeIconFont(icons, kBody, VKIntox::Icon::GlyphRanges());
        return f;
    };

    ImFont* regular   = addFace(body, 400.0f, false, 14.0f);
    ImFont* medium    = addFace(body, 500.0f, false, 14.0f);
    ImFont* bodyBold  = addFace(body, 700.0f, false, 14.0f);
    ImFont* bold      = addFace(doc,  650.0f, true,  18.0f);
    ImFont* extraBold = addFace(doc,  700.0f, true,  32.0f);
    ImGuiM3SetTextFonts(regular, medium, bold, extraBold);
    ImGuiM3SetBodyBoldFont(bodyBold);
    ImGuiM3LoadIconFont(icons, 24.0f, VKIntox::Icon::GlyphRanges());
}

// Submits one frame and returns everything it emitted. `mouse` may be null.
static std::vector<Tri> Frame(const char* theme, float w, float h,
                              float fraction, const ImVec2* mouse)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(w, h);
    io.DeltaTime = 1.0f / 60.0f;            // the harness owns the clock
    if (mouse)
        io.AddMousePosEvent(mouse->x, mouse->y);
    if (theme)
        ImGuiM3SetThemeFile(theme);

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("probe", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar);

    ImGui::M3LinearProgress(fraction, ImVec2(-1, 0), true);

    ImGui::End();
    ImGui::Render();

    std::vector<Tri> tris;
    ImDrawData* dd = ImGui::GetDrawData();
    for (int n = 0; n < dd->CmdListsCount; ++n)
    {
        ImDrawList* cl = dd->CmdLists[n];
        for (int c = 0; c < cl->CmdBuffer.Size; ++c)
        {
            ImDrawCmd* cmd = &cl->CmdBuffer[c];
            if (cmd->UserCallback)
                continue;
            for (unsigned int e = cmd->IdxOffset + cmd->ElemCount; cmd->IdxOffset < e; cmd->IdxOffset += 3)
            {
                const ImDrawIdx* idx = cl->IdxBuffer.Data + cmd->IdxOffset;
                const ImDrawVert& v0 = cl->VtxBuffer[idx[0]];
                const ImDrawVert& v1 = cl->VtxBuffer[idx[1]];
                const ImDrawVert& v2 = cl->VtxBuffer[idx[2]];
                tris.push_back({ v0.pos, v1.pos, v2.pos, v0.col, v1.col, v2.col });
            }
        }
    }
    return tris;
}

int main()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    InstallFonts();
    ImGuiM3ApplyToStyle(1.0f);

    // one warm-up frame: auto-resizing windows settle a frame after they first
    // lay out, and this project uses them everywhere.
    Frame(nullptr, 900.0f, 60.0f, 0.4f, nullptr);

    // the wave grows in over the first frames after it starts, so step the clock
    // by running frames rather than by jumping straight to the state of interest
    for (int i = 0; i < 40; ++i)
    {
        const std::vector<Tri> tris = Frame("/home/you/.config/VKIntox/theme.colors",
                                            900.0f, 60.0f, 0.4f, nullptr);
        int miny = 1 << 30, maxy = -(1 << 30), count = (int)tris.size() / 3;
        for (const Tri& t : tris)
            for (const ImVec2& p : { t.a, t.b, t.c }) { miny = ImMin(miny, (int)p.y); maxy = ImMax(maxy, (int)p.y); }
        std::printf("frame %2d  verts %5d  y[%d,%d]\n", i, count * 3, miny, maxy);
    }

    ImGui::DestroyContext();
    return 0;
}
```

Build it against the objects the test targets already produce. The theme test
compiles the same ImGui sources, so its object files are a convenient link set:

```sh
R=$PWD
clang++ -std=c++20 -O1 -I$R/src -I$R/src/vendor/imgui -I$R/src/overlay \
    -I$R/src/core -I$R/src/effects -I$R/src/effects/params -I$R/src/core/config \
    probe.cc \
    $R/build/src/vkintox-m3-theme-tests.p/vendor_imgui_imgui_m3.cc.o \
    $R/build/src/vkintox-m3-theme-tests.p/vendor_imgui_imgui_draw.cc.o \
    $R/build/src/vkintox-m3-theme-tests.p/vendor_imgui_imgui.cc.o \
    $R/build/src/vkintox-m3-theme-tests.p/vendor_imgui_imgui_widgets.cc.o \
    $R/build/src/vkintox-m3-theme-tests.p/vendor_imgui_imgui_tables.cc.o \
    -o probe
./probe
```

For a permanent harness, add a `test()` target in `src/meson.build` next to the
existing ones so it builds and runs with `meson test`.

## Rasterising the result

Triangles carry their own alpha (ImGui's anti-aliasing is a per-vertex alpha ramp
around every contour), so a barycentric fill that accumulates alpha reproduces the
edge quality faithfully:

```cpp
for (const Tri& t : tris)
{
    const float area = (t.b.y - t.c.y) * (t.a.x - t.c.x) + (t.c.x - t.b.x) * (t.a.y - t.c.y);
    if (std::fabs(area) < 1e-9f)
        continue;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const float px = x + 0.5f, py = y + 0.5f;
            float l1 = ((t.b.y - t.c.y) * (px - t.c.x) + (t.c.x - t.b.x) * (py - t.c.y)) / area;
            float l2 = ((t.c.y - t.a.y) * (px - t.c.x) + (t.a.x - t.c.x) * (py - t.c.y)) / area;
            float l3 = 1.0f - l1 - l2;
            if (l1 < 0 || l2 < 0 || l3 < 0)
                continue;
            const ImU32 cols[3] = { t.ca, t.cb, t.cc };
            const float ws[3] = { l1, l2, l3 };
            float col[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < 3; ++k)
            {
                const float a = ((cols[k] >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f * ws[k];
                col[0] += ((cols[k] >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f * a;
                col[1] += ((cols[k] >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f * a;
                col[2] += ((cols[k] >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f * a;
                col[3] += a;
            }
            // composite col over the destination, clamped
        }
}
```

Write a `P6` PPM (three bytes per pixel, header `P6\n<w> <h>\n255\n`), then
upscale it before looking at it:

```sh
python3 -c "
from PIL import Image
im = Image.open('/tmp/probe.ppm')
im.resize((im.width*2, im.height*2), Image.LANCZOS).save('/tmp/probe.png')"
```

Upscaling matters: at 1:1 a one-pixel anti-aliasing fringe is invisible, and the
question is usually whether an edge is soft or stepped, or whether two paddings
match.

## Asserting instead of looking

Most checks do not need a picture. Item rectangles come straight out of ImGui:

```cpp
ImGui::M3Button(label, ImGuiM3Button_Filled, ImVec2(w, h));
const ImVec2 min = ImGui::GetItemRectMin();
const ImVec2 size = ImGui::GetItemRectSize();
```

and the draw data gives the same for anything drawn by hand. Useful invariants:

- A full-width widget spans `GetContentRegionAvail()`; a right-aligned row ends on
  the content's right edge.
- Padding is `button width - label width`, and both labels in a row should report
  the same padding.
- A folded body's `GetWindowSize().y` at `reveal == 1` must exceed its content
  height (`ScrollMax.y == 0`), otherwise the last row is clipped.

## Driving input

State that depends on the pointer (hover, press, tooltips) works if the events are
queued *before* `NewFrame`:

```cpp
io.AddMousePosEvent(x, y);            // applied during NewFrame
io.AddMouseButtonEvent(0, true);
```

One event per frame: ImGui trickles them by default, one per `NewFrame`, so queue,
step a frame, then assert.

## Pitfalls

- **Warm-up frames.** Auto-resizing windows and some widgets read the previous
  frame's size. Submit one frame before measuring.
- **The clock is yours.** `io.DeltaTime` drives every spring and tween, and
  `ImGui::GetTime()` advances with it. To observe an animation, run frames; to
  skip to a stable state, run enough frames that it settles. A single frame
  reports the animation mid-flight.
- **Per-widget state persists** in the window's storage, keyed by ID, for the
  lifetime of the context. Two different widgets in one harness must not share an
  ID, or one will inherit the other's animation.
- **Fonts first.** Install faces and call `ImGuiM3ApplyToStyle` before submitting
  anything; item heights and paddings derive from the font metrics.
- **`IMGUI_DEFINE_MATH_OPERATORS`** must be defined before including `imgui.h` if
  the harness uses `ImVec2` arithmetic.
- **Keep the theme out of the geometry.** Point the harness at a fixed
  `.colors` file (`ImGuiM3SetThemeFile`) so results do not drift with whatever the
  running instance last saved.
