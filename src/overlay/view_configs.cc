#include "imgui_overlay.hh"
#include "config_serializer.hh"

#include <cstring>

#include "vendor/imgui/imgui.h"

namespace VKIntox
{
    void ImGuiOverlay::renderConfigManagerView()
    {
        // Config management mode
        ImGui::Text("Manage Configs");
        ImGui::Separator();

        // Refresh config list and get current default
        if (configListRefreshPending)
        {
            configList = ConfigSerializer::listConfigs();
            configListDefault = ConfigSerializer::getDefaultConfig();
            configListRefreshPending = false;
        }

        // Calculate button group width once
        float setDefaultWidth = ImGui::CalcTextSize("Set Default").x + ImGui::GetStyle().FramePadding.x * 2;
        float deleteWidth = ImGui::CalcTextSize("Delete").x + ImGui::GetStyle().FramePadding.x * 2;
        float buttonGroupWidth = setDefaultWidth + deleteWidth + ImGui::GetStyle().ItemSpacing.x;

        ImGui::BeginChild("ConfigList", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), false);
        float buttonGroupX = ImGui::GetContentRegionAvail().x - buttonGroupWidth;
        for (size_t i = 0; i < configList.size(); i++)
        {
            ImGui::PushID(static_cast<int>(i));
            const std::string& cfg = configList[i];

            // Selectable config name - click to load
            float nameWidth = buttonGroupX - ImGui::GetStyle().ItemSpacing.x;
            if (ImGui::Selectable(cfg.c_str(), false, 0, ImVec2(nameWidth, 0)))
            {
                // Signal to vkintox.cpp to load this config
                pendingConfigPath = ConfigSerializer::getConfigsDir() + "/" + cfg + ".conf";
                strncpy(saveConfigName, cfg.c_str(), sizeof(saveConfigName) - 1);
                applyRequested = true;
                inConfigManageMode = false;
            }
            ImGui::SameLine(buttonGroupX);

            bool isDefault = (cfg == configListDefault);
            if (isDefault)
                ImGui::BeginDisabled();
            if (ImGui::SmallButton("Set Default"))
            {
                if (ConfigSerializer::setDefaultConfig(cfg))
                    configListDefault = cfg;
            }
            if (isDefault)
                ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete"))
            {
                ConfigSerializer::deleteConfig(cfg);
                configListRefreshPending = true;
            }
            ImGui::PopID();
        }
        if (configList.empty())
        {
            ImGui::Text("No saved configs");
        }
        ImGui::EndChild();

        if (ImGui::Button("Back"))
        {
            inConfigManageMode = false;
        }
    }

} // namespace VKIntox
