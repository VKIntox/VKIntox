#include "imgui_overlay.hh"
#include "config_serializer.hh"
#include "overlay/ui_icons.hh"
#include "overlay/ui_theme.hh"

#include <cstring>

#include "overlay/ui_menu.hh"

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_internal.h"

namespace VKIntox
{
    void ImGuiOverlay::renderConfigManagerView()
    {
        const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
        const float d = m.density;

        // ---- Header: expressive page head -----------------------------------
        ImGui::PushFont(ImGuiM3FontBold(), ImGuiM3GetTypeStyle(ImGuiM3Type_TitleLarge).size * d);
        ImGui::TextUnformatted("Manage Configs");
        ImGui::PopFont();
        UI::TextSupport("Click a config to load it. The default one is applied on launch.");
        ImGui::Spacing();

        // Refresh config list and get current default
        if (configListRefreshPending)
        {
            configList = ConfigSerializer::listConfigs();
            configListDefault = ConfigSerializer::getDefaultConfig();
            configListRefreshPending = false;
        }

        const float rowH = 56.0f * d;
        const float rowGap = 6.0f * d;
        const float footerH = m.button_height_default * d + ImGui::GetStyle().ItemSpacing.y * 2.0f;

        ImGui::BeginChild("ConfigList", ImVec2(0, -footerH), false);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* brand = ImGuiM3FontBold();
        ImFont* bodyBold = ImGuiM3FontBodyBold();
        for (size_t i = 0; i < configList.size(); i++)
        {
            ImGui::PushID(static_cast<int>(i));
            const std::string& cfg = configList[i];
            const bool isDefault = (cfg == configListDefault);

            if (i > 0)
                ImGui::Dummy(ImVec2(0.0f, rowGap));
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const float rowW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
            const ImRect rowRect(rowMin, ImVec2(rowMin.x + rowW, rowMin.y + rowH));

            ImGui::PushStyleColor(ImGuiCol_Header, 0);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, 0);
            const ImGuiSelectableFlags rowFlags = (ImGuiSelectableFlags)(
                (int)ImGuiSelectableFlags_AllowOverlap | (int)ImGuiSelectableFlags_NoPadWithHalfSpacing);
            const bool load = ImGui::Selectable("##row", false, rowFlags, ImVec2(rowW, rowH));
            ImGui::PopStyleColor(3);
            const bool hovered = ImGui::IsItemHovered();

            const float radius = ImGuiM3Radius(ImGuiM3Shape_Large);
            const ImGuiM3ShapeRounding round{ radius, radius, radius, radius };
            ImGuiM3PathRoundedRect(dl, rowRect, round, ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainer));
            if (hovered)
                ImGuiM3DrawStateLayer(dl, rowRect, round, ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);

            // leading file mark
            if (ImGuiM3IconFont())
            {
                ImGuiM3DrawIcon(dl, Icon::SaveUtf8,
                                ImRect(ImVec2(rowMin.x + 16.0f * d, rowMin.y),
                                       ImVec2(rowMin.x + 44.0f * d, rowMin.y + rowH)),
                                20.0f * d, ImGuiM3ColorU32(isDefault ? ImGuiM3Role_Primary
                                                                    : ImGuiM3Role_OnSurfaceVariant));
            }

            const float namePx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * d;
            const ImVec2 nameSize = ImGui::CalcTextSize(cfg.c_str());
            dl->AddText(brand, namePx, ImVec2(rowMin.x + 54.0f * d, rowMin.y + (rowH - nameSize.y) * 0.5f),
                        ImGuiM3ColorU32(ImGuiM3Role_OnSurface), cfg.c_str());

            float trailingX = rowRect.Max.x - 14.0f * d;
            if (isDefault)
            {
                const char* badge = "DEFAULT";
                const float badgePx = 10.0f * d;
                ImGui::PushFont(bodyBold, badgePx);
                const ImVec2 badgeText = ImGui::CalcTextSize(badge);
                ImGui::PopFont();
                const float badgeH = 20.0f * d;
                const ImVec2 badgeMax(trailingX, rowMin.y + (rowH - badgeH) * 0.5f + badgeH);
                const ImVec2 badgeMin(badgeMax.x - badgeText.x - 16.0f * d, badgeMax.y - badgeH);
                const float badgeR = badgeH * 0.5f;
                ImGuiM3PathRoundedRect(dl, ImRect(badgeMin, badgeMax),
                                       ImGuiM3ShapeRounding{ badgeR, badgeR, badgeR, badgeR },
                                       ImGuiM3ColorU32(ImGuiM3Role_SecondaryContainer));
                dl->AddText(bodyBold, badgePx,
                            ImVec2(badgeMin.x + 8.0f * d, badgeMin.y + (badgeH - badgeText.y) * 0.5f),
                            ImGuiM3ColorU32(ImGuiM3Role_OnSecondaryContainer), badge);
                trailingX = badgeMin.x - 12.0f * d;
            }

            // trailing actions, laid out right to left on the row
            const float btnH = 34.0f * d;
            const float btnY = rowMin.y + (rowH - btnH) * 0.5f;
            {
                const std::string deleteLabel = std::string(Icon::DeleteUtf8) + "  Delete";
                const float deleteW = ImGui::CalcTextSize(deleteLabel.c_str()).x + 24.0f * d;
                ImGui::SetCursorScreenPos(ImVec2(trailingX - deleteW, btnY));
                if (ImGui::M3Button(deleteLabel.c_str(), ImGuiM3Button_Text, ImVec2(deleteW, btnH)))
                {
                    ConfigSerializer::deleteConfig(cfg);
                    configListRefreshPending = true;
                    ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowRect.Max.y));
                    ImGui::Dummy(ImVec2(0.0f, 0.0f));
                    ImGui::PopID();
                    continue;
                }
                trailingX -= deleteW + 6.0f * d;

                const float defW = ImGui::CalcTextSize("Set default").x + 28.0f * d;
                ImGui::SetCursorScreenPos(ImVec2(trailingX - defW, btnY));
                ImGui::BeginDisabled(isDefault);
                if (ImGui::M3Button("Set default", ImGuiM3Button_Tonal, ImVec2(defW, btnH)))
                {
                    if (ConfigSerializer::setDefaultConfig(cfg))
                        configListDefault = cfg;
                }
                ImGui::EndDisabled();
                ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowRect.Max.y));
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
            }

            if (load)
            {
                // Signal to vkintox.cpp to load this config
                pendingConfigPath = ConfigSerializer::getConfigsDir() + "/" + cfg + ".conf";
                strncpy(saveConfigName, cfg.c_str(), sizeof(saveConfigName) - 1);
                applyRequested = true;
                inConfigManageMode = false;
            }

            ImGui::PopID();
        }
        if (configList.empty())
        {
            UI::TextSupport("No saved configs yet. Use Save As in the config bar to create one.");
        }
        ImGui::EndChild();

        if (ImGui::M3Button("Back", ImGuiM3Button_Outlined))
        {
            inConfigManageMode = false;
        }
    }

} // namespace VKIntox
