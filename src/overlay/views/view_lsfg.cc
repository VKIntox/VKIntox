#include "overlay/shell/imgui_overlay.hh"
#include "config/settings_manager.hh"
#include "framegen/lsfg_present.hh"
#include "util/logger.hh"
#include "overlay/theme/ui_theme.hh"
#include "overlay/theme/ui_icons.hh"

#include <cstdio>
#include <filesystem>

#include "vendor/imgui/imgui.h"

namespace VKIntox
{
    void ImGuiOverlay::renderLsfgView()
    {
        auto saveSettings = [&]() {
            settingsManager.save();
            settingsSaved = true;
        };

        ImGui::BeginChild("LsfgContent", ImVec2(0, 0), false);

        ImGui::M3HeroHeader(Icon::MaskedTransitionsUtf8, "Frame generation",
                            "Lossless Scaling interpolation through the lsfg-vk engine");

        ImGui::Spacing();

        bool enabled = settingsManager.getLsfgEnabled();
        if (ImGui::M3HeroSwitch(Icon::MaskedTransitionsUtf8, "Generate frames", "##lsfg_enabled", &enabled,
                                "Interpolates extra frames between the game's own ones.\n"
                                "The swapchain is rebuilt when this changes: the next frame asks the game to recreate it.",
                                1.15f))
        {
            settingsManager.setLsfgEnabled(enabled);
            saveSettings();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Toggling or changing the multiplier recreates the swapchain:\n"
                              "expect a brief flicker, no restart needed.");

        if (enabled)
        {
            ImGui::Spacing();

            // multiplier: x2/x3/x4 connected segment
            int multiplier = settingsManager.getLsfgMultiplier();
            int index = multiplier - 2;
            const char* const labels[3] = {"x2", "x3", "x4"};
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Multiplier");
            ImGui::SameLine(160.0f);
            if (ImGui::M3ConnectedButtonGroup("##lsfg_multiplier", labels, 3, &index, NULL, false, NULL))
            {
                settingsManager.setLsfgMultiplier(index + 2);
                saveSettings();
            }

            // flow scale
            ImGui::Spacing();
            float flowScale = settingsManager.getLsfgFlowScale();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Flow scale");
            ImGui::SameLine(160.0f);
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderFloat("##lsfg_flowscale", &flowScale, 0.25f, 1.0f, "%.2f"))
            {
                settingsManager.setLsfgFlowScale(flowScale);
                saveSettings();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Resolution the motion vectors are estimated at.\n"
                                  "Lower values are faster but less accurate on fine motion.");

            bool performanceMode = settingsManager.getLsfgPerformanceMode();
            if (ImGui::M3Switch("Performance mode##lsfg_perf", &performanceMode))
            {
                settingsManager.setLsfgPerformanceMode(performanceMode);
                saveSettings();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Faster interpolation at a small quality cost.");

            bool halfPrecision = settingsManager.getLsfgHalfPrecision();
            if (ImGui::M3Switch("Half precision##lsfg_fp16", &halfPrecision))
            {
                settingsManager.setLsfgHalfPrecision(halfPrecision);
                saveSettings();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Run the engine's shaders in fp16 where the GPU has a 2:1 rate.\n"
                                  "Notable on some iGPUs; harmless elsewhere.");
        }

        // engine + DLL diagnostics
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextUnformatted("Engine");
        const std::string engineState = FrameGen::engineStatus();
        if (FrameGen::engineAvailable())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGuiM3ColorU32(ImGuiM3Role_Primary));
            ImGui::TextUnformatted("lsfg-vk engine found");
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGuiM3ColorU32(ImGuiM3Role_Error));
            ImGui::TextUnformatted(engineState.c_str());
            ImGui::PopStyleColor();
            UI::TextSupport("Install lsfg-vk so liblsfg-vk-pipeline.so.2 sits next to the layer, or in the system library path.");
        }

        const std::string dllPath = settingsManager.getLsfgDllPath();
        char dllBuffer[512];
        snprintf(dllBuffer, sizeof(dllBuffer), "%s", dllPath.c_str());
        ImGui::Spacing();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Lossless Scaling DLL");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##lsfg_dll", dllBuffer, sizeof(dllBuffer)))
        {
            settingsManager.setLsfgDllPath(dllBuffer);
            saveSettings();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Path to the lsfg-vk.dll from the 'lsfg-vk' branch of Lossless Scaling.\n"
                              "Leave empty to let the engine search its default locations.");

        if (!dllPath.empty())
        {
            const bool present = std::filesystem::exists(dllPath);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGuiM3ColorU32(present ? ImGuiM3Role_Primary : ImGuiM3Role_Error));
            ImGui::TextUnformatted(present ? "DLL found" : "DLL not found at this path");
            ImGui::PopStyleColor();
        }
        else
        {
            UI::TextSupport("The engine searches Steam's Lossless Scaling install when this is empty.");
        }

        ImGui::EndChild();
    }
} // namespace VKIntox
