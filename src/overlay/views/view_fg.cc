#include "overlay/shell/imgui_overlay.hh"
#include "config/settings_manager.hh"
#include "framegen/present.hh"
#include "util/logger.hh"
#include "overlay/theme/ui_theme.hh"
#include "overlay/theme/ui_icons.hh"

#include <cstdio>
#include <filesystem>

#include "vendor/imgui/imgui.h"

namespace VKIntox
{
    void ImGuiOverlay::renderFgView()
    {
        auto saveSettings = [&]() {
            settingsManager.save();
            settingsSaved = true;
        };

        ImGui::BeginChild("FgContent", ImVec2(0, 0), false);

        ImGui::M3HeroHeader(Icon::MaskedTransitionsUtf8, "Frame generation",
                            "Motion-interpolated frames through the bundled MAKO FG model");

        ImGui::Spacing();

        bool enabled = settingsManager.getFgEnabled();
        if (ImGui::M3HeroSwitch(Icon::MaskedTransitionsUtf8, "Generate frames", "##fg_enabled", &enabled,
                                "Interpolates extra frames between the game's own ones.\n"
                                "The swapchain is rebuilt when this changes: the next frame asks the game to recreate it.",
                                1.15f))
        {
            settingsManager.setFgEnabled(enabled);
            saveSettings();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Toggling or changing the multiplier recreates the swapchain:\n"
                              "expect a brief flicker, no restart needed.");

        if (enabled)
        {
            ImGui::Spacing();

            // multiplier: x2/x3/x4 connected segment
            int multiplier = settingsManager.getFgMultiplier();
            int index = multiplier - 2;
            const char* const labels[3] = {"x2", "x3", "x4"};
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Multiplier");
            ImGui::SameLine(160.0f);
            if (ImGui::M3ConnectedButtonGroup("##fg_multiplier", labels, 3, &index, NULL, false, NULL))
            {
                settingsManager.setFgMultiplier(index + 2);
                saveSettings();
            }

            // flow scale
            ImGui::Spacing();
            float flowScale = settingsManager.getFgFlowScale();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Flow scale");
            ImGui::SameLine(160.0f);
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderFloat("##fg_flowscale", &flowScale, 0.25f, 1.0f, "%.2f"))
            {
                settingsManager.setFgFlowScale(flowScale);
                saveSettings();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Resolution the motion analysis runs at.\n"
                                  "Lower values are faster but less accurate on fine motion.");

            bool performanceMode = settingsManager.getFgPerformanceMode();
            if (ImGui::M3Switch("Performance mode##fg_perf", &performanceMode))
            {
                settingsManager.setFgPerformanceMode(performanceMode);
                saveSettings();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Faster interpolation at a small quality cost.");
        }

        // engine diagnostics
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextUnformatted("Engine");
        if (FrameGen::engineAvailable())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGuiM3ColorU32(ImGuiM3Role_Primary));
            ImGui::TextUnformatted("MAKO FG backend linked");
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGuiM3ColorU32(ImGuiM3Role_Error));
            ImGui::TextUnformatted(FrameGen::engineStatus().c_str());
            ImGui::PopStyleColor();
        }

        const std::string package = FrameGen::modelPackagePath();
        if (!package.empty() && std::filesystem::exists(package))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGuiM3ColorU32(ImGuiM3Role_Primary));
            ImGui::TextUnformatted("MAKO FG model installed");
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGuiM3ColorU32(ImGuiM3Role_Error));
            ImGui::TextUnformatted("MAKO FG model missing");
            ImGui::PopStyleColor();
            UI::TextSupport("The bundled mako-fg package should sit beside the layer (mako-fg/ in the layer's directory).");
        }

        ImGui::Spacing();
        UI::TextSupport("The model is open-weight and needs no Lossless Scaling. Generated frames run in a separate process, so a failure there never takes the game down.");

        ImGui::EndChild();
    }
} // namespace VKIntox
