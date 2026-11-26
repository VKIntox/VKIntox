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

        ImGui::M3CardBegin("fg_card");

        bool enabled = settingsManager.getFgEnabled();
        if (ImGui::M3HeroSwitch(Icon::MaskedTransitionsUtf8, "Generate frames", "##fg_enabled", &enabled,
                                "Generated frames are inserted between the game's own frames.\n"
                                "Changes rebuild the generator between frames.",
                                1.15f))
        {
            settingsManager.setFgEnabled(enabled);
            saveSettings();
        }
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 6.0f * ImGuiM3GetMetrics().density);
        ImGui::Indent(72.0f * ImGuiM3GetMetrics().density);
        UI::TextSupport("Generate frames using MAKO. This forces FIFO frame presenting.");
        ImGui::Unindent(72.0f * ImGuiM3GetMetrics().density);

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
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Rebuilds the generator live.");

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

            bool fxOverUi = settingsManager.getFxOverUi();
            if (ImGui::M3Switch("FX-Over-UI for generated frames##fg_overui", &fxOverUi))
            {
                settingsManager.setFxOverUi(fxOverUi);
                saveSettings();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Keeps the game's HUD and the overlay out of the interpolated image where the game supports it.\n"
                                  "Applies on the next launch.");

        }

        ImGui::M3CardEnd();
        ImGui::EndChild();
    }
} // namespace VKIntox
