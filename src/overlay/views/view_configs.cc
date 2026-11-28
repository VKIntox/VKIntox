#include "overlay/shell/imgui_overlay.hh"
#include "config/config_serializer.hh"
#include "overlay/theme/ui_icons.hh"
#include "overlay/theme/ui_theme.hh"
#include "overlay/shell/ui_dialog.hh"
#include "overlay/shell/ui_menu.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_internal.h"

namespace VKIntox
{
    void ImGuiOverlay::renderConfigManagerView()
    {
        const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
        const float d = m.density;

        if (configListRefreshPending)
        {
            configList = ConfigSerializer::listConfigs();
            configListDefault = ConfigSerializer::getDefaultConfig();
            configListRefreshPending = false;
        }

        std::string activeConfigName;
        auto nameFromPath = [](std::string name) {
            if (name.size() > 5 && name.substr(name.size() - 5) == ".conf")
                name.resize(name.size() - 5);
            return name;
        };
        if (!state.configName.empty())
            activeConfigName = nameFromPath(state.configName);
        if (std::find(configList.begin(), configList.end(), activeConfigName) == configList.end())
            activeConfigName = activeProfileName;
        if (std::find(configList.begin(), configList.end(), activeConfigName) == configList.end())
            activeConfigName = saveConfigName;

        ImGui::M3HeroHeader(Icon::SaveUtf8, "Saved configs",
                            "Load a saved setup. The default config is applied on launch.");
        ImGui::Spacing();

        const float rowH = 72.0f * d;
        const float rowGap = 6.0f * d;
        const float footerH = m.button_height_default * d + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        static std::string deleteTarget;
        static bool deletePopupPending = false;

        ImGui::BeginChild("ConfigList", ImVec2(0, -footerH), false);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* brand = ImGuiM3FontBold();
        const float namePx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * d;
        const float iconSize = 40.0f * d;
        const float actionSize = m.icon_button_size * d;
        const float actionGap = 6.0f * d;

        auto chipWidth = [&](const char* label) {
            return ImGui::CalcTextSize(label).x + 24.0f * d;
        };

        for (size_t i = 0; i < configList.size(); i++)
        {
            ImGui::PushID(static_cast<int>(i));
            const std::string& cfg = configList[i];
            const bool isDefault = cfg == configListDefault;
            const bool isActive = cfg == activeConfigName;
            const bool isProtected = isDefault || isActive;

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
            ImGuiM3PathRoundedRect(dl, rowRect, round,
                                   ImGuiM3ColorU32(isActive ? ImGuiM3Role_SecondaryContainer
                                                            : ImGuiM3Role_SurfaceContainer));
            if (hovered)
                ImGuiM3DrawStateLayer(dl, rowRect, round,
                                      isActive ? ImGuiM3Role_OnSecondaryContainer : ImGuiM3Role_OnSurface,
                                      ImGuiM3State_Hovered);

            if (ImGuiM3IconFont())
            {
                ImGuiM3DrawIcon(dl, Icon::SaveUtf8,
                                ImRect(ImVec2(rowMin.x + 12.0f * d, rowMin.y + (rowH - iconSize) * 0.5f),
                                       ImVec2(rowMin.x + 12.0f * d + iconSize,
                                              rowMin.y + (rowH + iconSize) * 0.5f)),
                                22.0f * d,
                                ImGuiM3ColorU32(isActive ? ImGuiM3Role_OnSecondaryContainer
                                                         : ImGuiM3Role_Primary));
            }

            const float actionsLeft = rowRect.Max.x - 12.0f * d - actionSize * 2.0f - actionGap;
            const ImVec2 mousePos = ImGui::GetIO().MousePos;
            const bool overActions = mousePos.x >= actionsLeft && mousePos.x <= rowRect.Max.x &&
                                     mousePos.y >= rowRect.Min.y && mousePos.y <= rowRect.Max.y;
            const bool loadRequested = load && !overActions;
            float trailingX = actionsLeft - 10.0f * d;
            if (isDefault)
            {
                const float w = chipWidth("Default");
                ImGui::SetCursorScreenPos(ImVec2(trailingX - w, rowMin.y + (rowH - m.chip_height * d) * 0.5f));
                ImGui::M3StatusChip("Default", ImGuiM3Role_SecondaryContainer);
                trailingX -= w + 6.0f * d;
            }
            if (isActive)
            {
                const float w = chipWidth("Active");
                ImGui::SetCursorScreenPos(ImVec2(trailingX - w, rowMin.y + (rowH - m.chip_height * d) * 0.5f));
                ImGui::M3StatusChip("Active", ImGuiM3Role_PrimaryContainer);
                trailingX -= w + 6.0f * d;
            }

            const float textX = rowMin.x + 64.0f * d;
            const float textW = ImMax(1.0f, trailingX - textX);
            ImGui::PushFont(brand, namePx);
            const char* shownName = textW > 12.0f * d ? UI::TruncateLabel(cfg.c_str(), textW) : "";
            const ImVec2 nameSize = ImGui::CalcTextSize(shownName);
            ImGui::PopFont();
            dl->AddText(brand, namePx,
                        ImVec2(textX, rowMin.y + (rowH - nameSize.y) * 0.5f),
                        isActive ? ImGuiM3ColorU32(ImGuiM3Role_OnSecondaryContainer)
                                 : ImGuiM3ColorU32(ImGuiM3Role_OnSurface),
                        shownName);
            if (hovered)
                ImGui::SetTooltip("%s", cfg.c_str());

            const float btnY = rowMin.y + (rowH - actionSize) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(actionsLeft, btnY));
            ImGui::SetNextItemAllowOverlap();
            ImGui::BeginDisabled(isDefault);
            if (ImGui::M3IconButton(Icon::CheckUtf8,
                                    isDefault ? "This is the default config" : "Set as default",
                                    isDefault ? ImGuiM3Button_Text : ImGuiM3Button_Tonal))
            {
                if (ConfigSerializer::setDefaultConfig(cfg))
                {
                    configListDefault = cfg;
                    pushToast(LogLevel::Info, "Set \"" + cfg + "\" as the default config.");
                }
                else
                    pushToast(LogLevel::Error, "Could not set \"" + cfg + "\" as the default config.");
            }
            ImGui::EndDisabled();

            ImGui::SetCursorScreenPos(ImVec2(actionsLeft + actionSize + actionGap, btnY));
            ImGui::SetNextItemAllowOverlap();
            ImGui::BeginDisabled(isProtected);
            if (ImGui::M3IconButton(Icon::DeleteUtf8,
                                    isProtected ? "The active and default configs are protected"
                                                : "Delete this config",
                                    ImGuiM3Button_Text))
            {
                deleteTarget = cfg;
                deletePopupPending = true;
            }
            ImGui::EndDisabled();

            if (loadRequested)
            {
                pendingConfigPath = ConfigSerializer::getConfigsDir() + "/" + cfg + ".conf";
                std::snprintf(saveConfigName, sizeof(saveConfigName), "%s", cfg.c_str());
                applyRequested = true;
                paramsDirty = false;
                reloadNeeded = false;
                profileDirty = false;
                inConfigManageMode = false;
                pushToast(LogLevel::Info, "Loading config \"" + cfg + "\".");
            }

            ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowRect.Max.y));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::PopID();
        }

        if (configList.empty())
        {
            ImGui::Spacing();
            const ImVec2 emptyPos = ImGui::GetCursorScreenPos();
            const float emptyIcon = 40.0f * d;
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(emptyPos.x + emptyIcon * 0.5f, emptyPos.y + emptyIcon * 0.5f), emptyIcon * 0.5f,
                ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainerHighest), 40);
            if (ImGuiM3IconFont())
            {
                ImGuiM3DrawIcon(dl, Icon::SaveUtf8,
                                ImRect(emptyPos, ImVec2(emptyPos.x + emptyIcon, emptyPos.y + emptyIcon)),
                                emptyIcon * 0.56f,
                                ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant));
            }
            ImGui::SameLine(0.0f, 12.0f * d);
            ImGui::BeginGroup();
            ImGui::PushFont(ImGuiM3FontBold(),
                            ImGuiM3GetTypeStyle(ImGuiM3Type_TitleMedium).size * d);
            ImGui::TextUnformatted("No saved configs yet");
            ImGui::PopFont();
            UI::TextSupport("Use Save in the config bar to create one.");
            ImGui::EndGroup();
        }

        if (deletePopupPending)
        {
            ImGui::OpenPopup("##delete_config_confirm");
            deletePopupPending = false;
        }
        if (UI::BeginM3Dialog("##delete_config_confirm", "Delete this config?"))
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 380.0f * d);
            ImGui::TextUnformatted(("\"" + deleteTarget +
                "\" will be permanently deleted. This can't be undone.").c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            const bool stillProtected = deleteTarget == configListDefault || deleteTarget == activeConfigName;
            const UI::M3DialogAction actions[] = {
                {"Cancel", ImGuiM3Button_Outlined},
                {"Delete config", ImGuiM3Button_Destructive, stillProtected},
            };
            const int action = UI::M3DialogActions(actions, 2);
            if (action == 1)
            {
                if (stillProtected)
                    pushToast(LogLevel::Error, "The active and default configs can't be deleted.");
                else if (ConfigSerializer::deleteConfig(deleteTarget))
                {
                    configListRefreshPending = true;
                    pushToast(LogLevel::Info, "Deleted config \"" + deleteTarget + "\".");
                }
                else
                    pushToast(LogLevel::Error, "Could not delete config \"" + deleteTarget + "\".");
            }
            if (action >= 0)
                ImGui::CloseCurrentPopup();
            UI::EndM3Dialog();
        }
        ImGui::EndChild();

        if (ImGui::M3Button("Back", ImGuiM3Button_Outlined))
            inConfigManageMode = false;
    }

} // namespace VKIntox
