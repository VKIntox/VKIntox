#include "imgui_overlay.hh"
#include "config_serializer.hh"
#include "overlay/ui_icons.hh"

#include <algorithm>
#include <fstream>
#include <string>

#include "vendor/imgui/imgui.h"

namespace VKIntox
{
    namespace
    {
        // the setup script drops a `version` file next to the config; read it
        // once so the label and the footer cannot disagree.
        const std::string& runtimeVersion()
        {
            static const std::string version = [] {
                std::ifstream file(ConfigSerializer::getBaseConfigDir() + "/version");
                std::string value;
                if (file.is_open() && std::getline(file, value) && !value.empty())
                {
                    if (value.back() == '\r')
                        value.pop_back();
                    if (!value.empty())
                        return value;
                }
                return std::string("unknown");
            }();
            return version;
        }
    }

    void ImGuiOverlay::renderAboutView()
    {
        const float brandSize = std::min(512.0f, ImGui::GetContentRegionAvail().x * 0.288f);
        renderCenteredBrandIcon(brandSize);

        // dimmed build line under the wordmark, centred the same way.
        {
            const std::string label = "version " + runtimeVersion();
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
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("This project uses a design heavily inspired by Google's Material 3/Material You. Not affiliated with or endorsed by Google.");
        ImGui::PopTextWrapPos();

        // build footer: the version again, plus the issue tracker.
        ImGui::Spacing();
        ImGui::TextDisabled("VKIntox version %s", runtimeVersion().c_str());
        ImGui::TextDisabled("Report issues:");
        ImGui::SameLine();
        ImGui::TextLinkOpenURL("github.com/buwryme/VKIntox/issues", "https://github.com/buwryme/VKIntox/issues");
    }
} // namespace VKIntox
