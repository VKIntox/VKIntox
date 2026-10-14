#include "imgui_overlay.hh"
#include "overlay/ui_icons.hh"
#include "overlay/ui_theme.hh"
#include "version.hh"

#include <algorithm>
#include <string>

#include "vendor/imgui/imgui.h"

namespace VKIntox
{
    namespace
    {
        // compiled in from the top-level VERSION file, so the label and the
        // footer agree and neither depends on a file the installer may not have
        // written.
        const char* runtimeVersion()
        {
            return VKINTOX_VERSION;
        }
    }

    void ImGuiOverlay::renderAboutView()
    {
        const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
        const float d = m.density;

        // Reserve the footer before laying out the content so it stays visible
        // when the view is short; the content scrolls in what is left.
        const char* disclaimer = "This project uses a design heavily inspired by Google's Material 3 / Material You. Not affiliated with or endorsed by Google.";
        const float wrapWidth = ImGui::GetContentRegionAvail().x;
        const float disclaimerHeight = ImGui::CalcTextSize(disclaimer, nullptr, false, wrapWidth).y;
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        const float footerHeight = disclaimerHeight + ImGui::GetTextLineHeight() * 2.5f + spacing * 6.0f + 8.0f;

        ImGui::BeginChild("about_content", ImVec2(0, -footerHeight));

        // Brand block: mark on the left, wordmark over a tagline and the build
        // line to its right. Sized from the theme density so it tracks density /
        // shape controls instead of scaling with the view.
        {
            const float d = m.density;
            const float iconSize = 96.0f * d;
            const float gap = 20.0f * d;
            const ImVec2 blockPos = ImGui::GetCursorScreenPos();
            const float availW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
            const float textW = ImMax(120.0f * d, availW - iconSize - gap);

            ImFont* wordFont = ImGuiM3FontBold();
            ImFont* tagFont = ImGuiM3FontMedium();
            ImFont* metaFont = ImGuiM3FontBodyBold();
            const float wordPx = 30.0f * d;
            const float tagPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyMedium).size * d;
            const float metaPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodySmall).size * d;

            const char* tagline = "ReShade-like Vulkan post processing layer, made Linux-first.";
            const std::string version = std::string("version ") + runtimeVersion() + "  \xC2\xB7  beta";

            ImGui::PushFont(wordFont, wordPx);
            const ImVec2 wordSize = ImGui::CalcTextSize("VKIntox");
            ImGui::PopFont();
            ImGui::PushFont(tagFont, tagPx);
            const ImVec2 tagSize = ImGui::CalcTextSize(tagline, nullptr, false, textW);
            ImGui::PopFont();
            ImGui::PushFont(metaFont, metaPx);
            const ImVec2 versionSize = ImGui::CalcTextSize(version.c_str());
            ImGui::PopFont();

            const float lineGap = 4.0f * d;
            const float textH = wordSize.y + tagSize.y + versionSize.y + lineGap * 2.0f;
            const float blockH = ImMax(iconSize, textH);
            ImGui::Dummy(ImVec2(ImMax(availW, blockH), blockH));

            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (titleIconDescriptor != VK_NULL_HANDLE)
                dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(titleIconDescriptor)),
                             ImVec2(blockPos.x, blockPos.y + (blockH - iconSize) * 0.5f),
                             ImVec2(blockPos.x + iconSize, blockPos.y + (blockH + iconSize) * 0.5f));

            const float textX = blockPos.x + iconSize + gap;
            float ty = blockPos.y + (blockH - textH) * 0.5f;
            dl->AddText(wordFont, wordPx, ImVec2(textX, ty), ImGuiM3ColorU32(ImGuiM3Role_OnSurface), "VKIntox");
            ty += wordSize.y + lineGap;
            dl->AddText(tagFont, tagPx, ImVec2(textX, ty), ImGuiM3ColorU32(ImGuiM3Role_OnSurface), tagline,
                        nullptr, textW);
            ty += tagSize.y + lineGap;
            dl->AddText(metaFont, metaPx, ImVec2(textX, ty), ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant),
                        version.c_str());
        }
        ImGui::Spacing();

        // Credits: the project each piece came from, with its handle as a link.
        ImGui::M3CardBegin("about_credits", "Credits", Icon::InfoUtf8);
        auto credit = [&](const char* what, const char* handle, const char* url) {
            const float namePx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * d;
            ImGui::PushFont(ImGuiM3FontBold(), namePx);
            ImGui::TextUnformatted(what);
            ImGui::PopFont();
            ImGui::SameLine(190.0f * d);
            UI::PushSupport();
            ImGui::TextLinkOpenURL(handle, url);
            UI::PopSupport();
        };
        credit("VKIntox by", "@buwryme", "https://github.com/buwryme");
        credit("vkShade by", "@slobodaapl", "https://github.com/slobodaapl");
        credit("vkBasalt by", "@DadSchoorse", "https://github.com/DadSchoorse/vkBasalt");
        credit("Overlay fork by", "@Boux", "https://github.com/Boux/vkBasalt_overlay");
        credit("Wayland overlay by", "@Daaboulex", "https://github.com/Daaboulex/vkBasalt_overlay_wayland");
        credit("ReShade FX support by", "@crosire", "https://github.com/crosire/reshade");
        credit("Dear ImGui by", "@ocornut", "https://github.com/ocornut/imgui");
        ImGui::M3CardEnd();

        ImGui::EndChild();

        // Footer, pinned below the content by the reserved height.
        ImGui::Spacing();
        UI::PushSupport();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", disclaimer);
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::TextDisabled("VKIntox by buwryme  \xC2\xB7  version %s", runtimeVersion());
        ImGui::TextDisabled("Report issues:");
        ImGui::SameLine();
        ImGui::TextLinkOpenURL("github.com/buwryme/VKIntox/issues", "https://github.com/buwryme/VKIntox/issues");
        UI::PopSupport();
    }

} // namespace VKIntox
