#include "imgui_overlay.hh"
#include "overlay/ui_icons.hh"
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
        const float brandSize = std::min(512.0f, ImGui::GetContentRegionAvail().x * 0.288f);
        renderCenteredBrandIcon(brandSize);

        // dimmed build line under the wordmark, centred the same way.
        {
            const std::string label = std::string("version ") + runtimeVersion();
            const float textWidth = ImGui::CalcTextSize(label.c_str()).x;
            const float contentWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (contentWidth - textWidth) * 0.5f));
            ImGui::TextDisabled("%s", label.c_str());
        }
        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::M3CardBegin("about_credits", "Credits", Icon::InfoUtf8);
        auto credit = [](const char* what, const char* handle, const char* url) {
            ImGui::TextDisabled("%s", what);
            ImGui::SameLine();
            ImGui::TextLinkOpenURL(handle, url);
        };
        credit("VKIntox maintained by", "@buwryme", "https://github.com/buwryme");
        credit("vkShade by", "@slobodaapl", "https://github.com/slobodaapl");
        credit("vkBasalt by", "@DadSchoorse", "https://github.com/DadSchoorse/vkBasalt");
        credit("Overlay fork by", "@Boux", "https://github.com/Boux/vkBasalt_overlay");
        credit("Wayland overlay by", "@Daaboulex", "https://github.com/Daaboulex/vkBasalt_overlay_wayland");
        credit("ReShade FX support by", "@crosire", "https://github.com/crosire/reshade");
        credit("Dear ImGui by", "@ocornut", "https://github.com/ocornut/imgui");
        ImGui::M3CardEnd();

        ImGui::Spacing();

        // pin the footer to the bottom of the view, with a separator above it
        const char* disclaimer = "This project uses a design heavily inspired by Google's Material 3/Material You. Not affiliated with or endorsed by Google.";
        const float wrapWidth = ImGui::GetContentRegionAvail().x;
        const float disclaimerHeight = ImGui::CalcTextSize(disclaimer, nullptr, false, wrapWidth).y;
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        const float footerHeight = 1.0f + disclaimerHeight + ImGui::GetTextLineHeight() * 2.0f + spacing * 5.0f;
        const float remaining = ImGui::GetContentRegionAvail().y;
        if (remaining > footerHeight)
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (remaining - footerHeight));

        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", disclaimer);
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::TextDisabled("VKIntox version %s", runtimeVersion());
        ImGui::TextDisabled("Report issues:");
        ImGui::SameLine();
        ImGui::TextLinkOpenURL("github.com/buwryme/VKIntox/issues", "https://github.com/buwryme/VKIntox/issues");
    }
} // namespace VKIntox
