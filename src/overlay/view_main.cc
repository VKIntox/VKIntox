#include "imgui_overlay.hh"
#include "effects/effect_registry.hh"
#include "settings_manager.hh"
#include "config_serializer.hh"
#include "async_writer.hh"
#include "params/field_editor.hh"
#include "logger.hh"
#include "overlay/ui_theme.hh"
#include "overlay/ui_icons.hh"
#include "overlay/ui_dialog.hh"
#include "util.hh"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_internal.h"
#include "vendor/imgui/imfilebrowser.h"

namespace VKIntox
{
    namespace
    {
        constexpr const char* kEffectReorderPayload = "VKINTOX_EFFECT_REORDER";

        // Render a single preprocessor definition input, returns true if value changed
        bool renderPreprocessorDef(PreprocessorDefinition& def, EffectRegistry* registry, const std::string& effectName, float nameColumn)
        {
            bool changed = false;
            char valueBuf[64];
            strncpy(valueBuf, def.value.c_str(), sizeof(valueBuf) - 1);
            valueBuf[sizeof(valueBuf) - 1] = '\0';
            const bool modified = def.value != def.defaultValue;

            ImGui::PushID(def.name.c_str());

            // Name in a fixed leading column, vertically centred against the field.
            ImGui::AlignTextToFramePadding();
            ImFont* medium = ImGuiM3FontMedium();
            if (medium)
                ImGui::PushFont(medium, ImGui::GetFontSize());
            ImGui::TextUnformatted(def.name.c_str());
            if (medium)
                ImGui::PopFont();
            ImGui::SameLine(nameColumn);

            ImGui::SetNextItemWidth(-96.0f);
            if (ImGui::InputText("##value", valueBuf, sizeof(valueBuf)))
            {
                registry->setPreprocessorDefValue(effectName, def.name, valueBuf);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Default: %s", def.defaultValue.c_str());

            // Reset affordance, only meaningful once modified.
            ImGui::SameLine();
            ImGui::BeginDisabled(!modified);
            const std::string refreshLabel = std::string(Icon::RefreshUtf8) + "##reset";
            if (ImGui::Button(refreshLabel.c_str()))
            {
                registry->setPreprocessorDefValue(effectName, def.name, def.defaultValue);
                changed = true;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(modified ? "Reset to default (%s)" : "Using default (%s)", def.defaultValue.c_str());

            if (modified)
            {
                ImGui::SameLine();
                ImGui::TextColored(UI::Warning(), "%s", Icon::WarningUtf8);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Modified from default");
            }

            if (ImGui::BeginPopupContextItem("##preproc_reset"))
            {
                if (ImGui::MenuItem("Reset to default"))
                {
                    registry->setPreprocessorDefValue(effectName, def.name, def.defaultValue);
                    changed = true;
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
            return changed;
        }

        // Fallback picker when the xdg portal is unavailable; mirrors the
        // Shaders tab. only displayed while the main view is the active tab.
        ImGui::FileBrowser presetBrowser(ImGuiFileBrowserFlags_CloseOnEsc);

    } // anonymous namespace

    // keyboard is part of the shared view signature; this particular view reads no
    // key state directly, so the name is omitted here.
    void ImGuiOverlay::renderMainView(const KeyboardState& /* keyboard */)
    {
        if (!effectRegistry)
            return;

        auto openPresetBrowser = [&]() {
            presetBrowser.SetTitle("Import ReShade preset");
            presetBrowser.SetTypeFilters({".ini"});
            const char* home = std::getenv("HOME");
            presetBrowser.SetPwd(home ? home : "/");
            presetBrowser.Open();
        };

        // Harvest a completed portal import. a request started from another tab
        // is ignored by pollFileDialog's kind check and stays queued.
        {
            FileDialogResult result = FileDialogResult::Cancelled;
            std::string picked;
            if (pollFileDialog(FileDialogKind::OpenFile, result, picked))
            {
                if (result == FileDialogResult::Success && !picked.empty())
                    importShaderPreset(picked);
                else if (result == FileDialogResult::Unavailable)
                    openPresetBrowser();
            }
        }

        // Get a mutable copy of selected effects for this frame
        std::vector<std::string> selectedEffects = effectRegistry->getSelectedEffects();
        static bool reorderPreviewActive = false;
        static std::string reorderPreviewName;
        static std::string reorderPreviewTargetName;
        static std::vector<std::string> reorderPreviewEffects;

        auto applySwapByName = [](std::vector<std::string>& effects, const std::string& movingName, int targetIndex) {
            if (targetIndex < 0 || targetIndex >= static_cast<int>(effects.size()))
                return false;

            auto sourceIt = std::find(effects.begin(), effects.end(), movingName);
            if (sourceIt == effects.end())
                return false;

            int sourceIndex = static_cast<int>(std::distance(effects.begin(), sourceIt));
            if (sourceIndex == targetIndex)
                return false;

            std::swap(effects[sourceIndex], effects[targetIndex]);
            return true;
        };

        if (reorderPreviewActive)
        {
            auto currentIt = std::find(selectedEffects.begin(), selectedEffects.end(), reorderPreviewName);
            auto previewIt = std::find(reorderPreviewEffects.begin(), reorderPreviewEffects.end(), reorderPreviewName);
            if (currentIt == selectedEffects.end() || previewIt == reorderPreviewEffects.end())
            {
                reorderPreviewActive = false;
                reorderPreviewName.clear();
                reorderPreviewTargetName.clear();
                reorderPreviewEffects.clear();
            }
        }

        const std::vector<std::string>& displayedEffects = reorderPreviewActive ? reorderPreviewEffects : selectedEffects;

        // Normal mode - show profile and effect controls

        // Profile section — auto-detected game with fixed config and shader INI selector
        if (!activeGameName.empty())
        {
            ImGui::TextColored(UI::Success(), "%s  %s", Icon::JoystickUtf8, activeGameName.c_str());

            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s  Shader INI:", Icon::BrushUtf8);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            const char* shaderLabel = activeShaderProfileName.empty() ? "None" : activeShaderProfileName.c_str();
            // cap the dropdown height so a long preset list scrolls instead of
            // running off the bottom of the overlay.
            ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                                ImVec2(FLT_MAX, 280.0f * ImGuiM3GetMetrics().density));
            if (ImGui::BeginCombo("##shaderprofile", shaderLabel))
            {
                for (const auto& profile : shaderProfiles)
                {
                    const bool selected = profile == activeShaderProfileName;
                    if (ImGui::Selectable(profile.c_str(), selected) && !selected)
                    {
                        if (!switchShaderProfile(profile))
                            pushToast(LogLevel::Error, "Could not save the active shader profile.");
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            // the profile actions sit flush right, like the effects row below
            const ImGuiM3Metrics& profileMetrics = ImGuiM3GetMetrics();
            const bool hasShaderProfile = !activeShaderProfileName.empty();
            const float profileIconWidth = ImGui::CalcTextSize(Icon::AddUtf8).x +
                                           profileMetrics.button_padding_x * 2.0f * profileMetrics.density;
            const int profileActionCount = hasShaderProfile ? 4 : 2;
            const float profileActionsWidth =
                profileIconWidth * (float)profileActionCount +
                ImGui::GetStyle().ItemSpacing.x * (float)(profileActionCount - 1);
            ImGui::SameLine(ImGui::GetWindowWidth() - profileActionsWidth -
                            ImGui::GetCurrentWindowRead()->WindowPadding.x);
            const std::string newProfileLabel = std::string(Icon::AddUtf8) + "##newshaderprofile";
            if (ImGui::Button(newProfileLabel.c_str()))
                ImGui::OpenPopup("NewShaderProfilePopup");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Create shader INI profile");
            static std::string deleteProfileTarget;
            static std::string renameProfileTarget;
            static char renameShaderProfileName[64] = "";
            if (!activeShaderProfileName.empty())
            {
                ImGui::SameLine();
                const std::string editProfileLabel = std::string(Icon::EditUtf8) + "##editshaderprofile";
                if (ImGui::Button(editProfileLabel.c_str()))
                {
                    renameProfileTarget = activeShaderProfileName;
                    std::snprintf(renameShaderProfileName, sizeof(renameShaderProfileName), "%s",
                                  activeShaderProfileName.c_str());
                    ImGui::OpenPopup("##rename_shader_profile");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Rename shader INI profile");

                if (UI::BeginM3Dialog("##rename_shader_profile", "Rename shader preset"))
                {
                    ImGui::TextDisabled("Name");
                    ImGui::SetNextItemWidth(240.0f);
                    ImGui::InputText("##renameshaderprofilename", renameShaderProfileName,
                                     sizeof(renameShaderProfileName));
                    ImGui::Spacing();
                    ImGui::Spacing();
                    const UI::M3DialogAction actions[] = {
                        {"Cancel", ImGuiM3Button_Outlined, false},
                        {"Rename", ImGuiM3Button_Filled, renameShaderProfileName[0] == '\0'},
                    };
                    const int action = UI::M3DialogActions(actions, 2);
                    if (action == 1)
                    {
                        const std::string newName = renameShaderProfileName;
                        const std::string oldName = renameProfileTarget;
                        if (newName == oldName)
                        {
                            // name unchanged: nothing to rename
                        }
                        else if (!ConfigSerializer::isValidShaderProfileName(newName))
                            pushToast(LogLevel::Error, "Preset names can't contain /, \\ or @.");
                        else if (std::find(shaderProfiles.begin(), shaderProfiles.end(), newName) != shaderProfiles.end())
                            pushToast(LogLevel::Error, "A shader preset named \"" + newName + "\" already exists.");
                        else if (!autoSaveProfile(true))
                            pushToast(LogLevel::Error, "Could not save the active shader profile.");
                        else
                        {
                            // run the rename on the writer thread so a queued
                            // save can't race the files, then update the UI's
                            // name/path only after it has landed
                            auto renamed = std::make_shared<std::atomic<bool>>(false);
                            AsyncWriter::instance().submit([game = activeGameName, oldName, newName, renamed]() {
                                renamed->store(ConfigSerializer::renameShaderProfile(game, oldName, newName),
                                               std::memory_order_relaxed);
                            });
                            AsyncWriter::instance().waitForIdle();
                            if (renamed->load(std::memory_order_relaxed))
                            {
                                if (activeShaderProfileName == oldName)
                                    setActiveShaderProfile(newName);
                                refreshShaderProfiles();
                                pendingShaderProfilePath = activeShaderProfilePath;
                                pendingShaderProfile = true;
                                applyRequested = true;
                                paramsDirty = false;
                                profileDirty = false;
                                pushToast(LogLevel::Info, "Renamed shader preset to \"" + newName + "\".");
                            }
                            else
                                pushToast(LogLevel::Error, "Could not rename the shader profile.");
                        }
                    }
                    if (action >= 0)
                        ImGui::CloseCurrentPopup();
                    UI::EndM3Dialog();
                }

                ImGui::SameLine();
                const std::string delProfileLabel = std::string(Icon::DeleteUtf8) + "##delshaderprofile";
                if (ImGui::Button(delProfileLabel.c_str()))
                {
                    deleteProfileTarget = activeShaderProfileName;
                    ImGui::OpenPopup("##delete_shader_profile");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Delete shader INI profile");

                if (UI::BeginM3Dialog("##delete_shader_profile", "Delete this shader preset?"))
                {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 380.0f * ImGuiM3GetMetrics().density);
                    ImGui::TextUnformatted(("This preset, \"" + deleteProfileTarget + "\", will be deleted. This can't be undone.").c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::Spacing();
                    ImGui::Spacing();
                    const UI::M3DialogAction actions[] = {
                        {"No, keep", ImGuiM3Button_Outlined},
                        {"Yes, I'm sure", ImGuiM3Button_Destructive},
                    };
                    const int action = UI::M3DialogActions(actions, 2);
                    if (action == 1)
                    {
                        if (!ConfigSerializer::deleteShaderProfile(activeGameName, deleteProfileTarget))
                            pushToast(LogLevel::Error, "Could not delete the shader profile.");
                        else
                        {
                            if (deleteProfileTarget == activeShaderProfileName)
                            {
                                profileDirty = false;
                                paramsDirty = false;
                                activeShaderProfileName.clear();
                            }
                            refreshShaderProfiles();
                            if (!shaderProfiles.empty())
                            {
                                pendingShaderProfilePath = activeShaderProfilePath;
                                pendingShaderProfile = true;
                                applyRequested = true;
                            }
                            else
                            {
                                activeShaderProfileName.clear();
                                activeShaderProfilePath.clear();
                                pendingShaderProfilePath.clear();
                                pendingShaderProfile = true;
                                applyRequested = true;
                            }
                        }
                    }
                    if (action >= 0)
                        ImGui::CloseCurrentPopup();
                    UI::EndM3Dialog();
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(fileDialogPending() || presetBrowser.IsOpened());
            const std::string importProfileLabel = std::string(Icon::UploadUtf8) + "##importshaderprofile";
            if (ImGui::Button(importProfileLabel.c_str()))
            {
                if (!startOpenFileDialog("Import ReShade preset", {"*.ini"}))
                    openPresetBrowser();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Import a ReShade .ini preset");
            if (UI::BeginM3Dialog("NewShaderProfilePopup", "New shader INI preset"))
            {
                static char newShaderProfileName[64] = "";
                ImGui::TextDisabled("Name");
                ImGui::SetNextItemWidth(240.0f);
                ImGui::InputText("##newshaderprofilename", newShaderProfileName, sizeof(newShaderProfileName));
                ImGui::Spacing();
                ImGui::Spacing();
                const UI::M3DialogAction actions[] = {
                    {"Cancel", ImGuiM3Button_Outlined, false},
                    {"Create", ImGuiM3Button_Filled, newShaderProfileName[0] == '\0'},
                };
                const int action = UI::M3DialogActions(actions, 2);
                if (action == 1)
                {
                    if (!autoSaveProfile(true))
                        pushToast(LogLevel::Error, "Could not save the active shader profile.");
                    else if (ConfigSerializer::createShaderProfile(activeGameName, newShaderProfileName,
                                                                   activeShaderProfileName))
                    {
                        switchShaderProfile(newShaderProfileName);
                        refreshShaderProfiles();
                        newShaderProfileName[0] = '\0';
                    }
                    else
                        pushToast(LogLevel::Error, "Could not create the shader profile.");
                }
                if (action >= 0)
                    ImGui::CloseCurrentPopup();
                UI::EndM3Dialog();
            }

        }
        else
        {
            // Fallback: legacy config UI for unknown executables
            ImGui::Text("Config:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            ImGui::InputText("##configname", saveConfigName, sizeof(saveConfigName));
            ImGui::SameLine();
            ImGui::BeginDisabled(saveConfigName[0] == '\0');
            if (ImGui::Button("Save"))
                saveCurrentConfig();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("..."))
            {
                inConfigManageMode = true;
                configListRefreshPending = true;
            }
        }
        ImGui::Separator();

        // toggle and actions share one row: switch on the left, actions
        // right-aligned, with a single separator above and below the group.
        bool effectsOn = state.effectsEnabled;
        if (ImGui::Checkbox(effectsOn ? "Effects ON" : "Effects OFF", &effectsOn))
            toggleEffectsRequested = true;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("(Toggle keybind bound to %s)", settingsManager.getToggleKey().c_str());

        const std::string clearAllLabel = std::string(Icon::DeleteUtf8) + "  Clear all";
        const std::string addEffectsLabel = std::string(Icon::AddUtf8) + "  Add effects...";
        const ImGuiM3Metrics& m3m = ImGuiM3GetMetrics();
        const float clearW = ImGui::CalcTextSize(clearAllLabel.c_str()).x + m3m.button_padding_x * 2.0f * m3m.density;
        const float addW = ImGui::CalcTextSize(addEffectsLabel.c_str()).x + m3m.button_padding_x * 2.0f * m3m.density;
        ImGui::SameLine(ImGui::GetWindowWidth() - (clearW + addW + ImGui::GetStyle().ItemSpacing.x) -
                        ImGui::GetCurrentWindowRead()->WindowPadding.x);
        ImGui::BeginDisabled(selectedEffects.empty());
        if (ImGui::M3Button(clearAllLabel.c_str(), ImGuiM3Button_DestructiveOutlined))
            ImGui::OpenPopup("##clear_all_confirm");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::M3Button(addEffectsLabel.c_str(), ImGuiM3Button_Filled))
        {
            inSelectionMode = true;
            addEffectsFocusSearch = true;
            insertPosition = -1;  // Append to end
            pendingAddEffects.clear();
            addEffectsDuplicateCount.clear();
            addEffectsRemoved.clear();
        }

        // Destructive action, so confirm it through the shared dialog frame.
        const std::string clearPresetName = !activeShaderProfileName.empty() ? activeShaderProfileName : activeProfileName;
        if (UI::BeginM3Dialog("##clear_all_confirm", "Are you sure you want to clear all effects from this preset?"))
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 380.0f * ImGuiM3GetMetrics().density);
            ImGui::TextUnformatted(("This preset, \"" + clearPresetName + "\", will have its effects cleared. This can't be undone.").c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::Spacing();
            const UI::M3DialogAction actions[] = {
                {"No, keep", ImGuiM3Button_Outlined},
                {"Yes, I'm sure", ImGuiM3Button_Destructive},
            };
            const int action = UI::M3DialogActions(actions, 2);
            if (action == 1)
            {
                selectedEffects.clear();
                effectRegistry->clearSelectedEffects();
                paramsDirty = true;
                lastChangeTime = std::chrono::steady_clock::now();
                applyRequested = true;
                profileDirty = true;
            }
            if (action >= 0)
                ImGui::CloseCurrentPopup();
            UI::EndM3Dialog();
        }
        // Scrollable effect list, carded and scrolled independently of the view
        float footerHeight = ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetStyle().ItemSpacing.y;
        const ImGuiM3Metrics& listMetrics = ImGuiM3GetMetrics();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainerLow));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ImGuiM3Radius(ImGuiM3Shape_Large));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * listMetrics.density, 10.0f * listMetrics.density));
        ImGui::BeginChild("EffectList", ImVec2(0, -footerHeight), ImGuiChildFlags_AlwaysUseWindowPadding);

        // Show selected effects with their parameters
        float itemHeight = ImGui::GetFrameHeightWithSpacing();
        for (size_t i = 0; i < displayedEffects.size(); i++)
        {
            const std::string& effectName = displayedEffects[i];
            ImGui::PushID(effectName.c_str());

            ImVec2 rowMin = ImGui::GetCursorScreenPos();
            float rowWidth = ImGui::GetContentRegionAvail().x;

            // Dedicated drag handle (left-most): three gray bars
            const float handleSide = ImGui::GetFrameHeight();
            ImGui::InvisibleButton("##drag_handle", ImVec2(handleSide, handleSide));
            ImVec2 handleMin = ImGui::GetItemRectMin();
            ImVec2 handleMax = ImGui::GetItemRectMax();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const bool handleHovered = ImGui::IsItemHovered();
            if (handleHovered)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            // The grip needs a concrete resting colour: an "enabled" state layer
            // is fully transparent, which made the handle invisible until hover.
            ImU32 gripColor = handleHovered ? ImGuiM3ColorU32(ImGuiM3Role_OnSurface)
                                             : ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant);
            float cx = (handleMin.x + handleMax.x) * 0.5f;
            float cy = (handleMin.y + handleMax.y) * 0.5f;
            for (int bar = -1; bar <= 1; ++bar)
            {
                float y = cy + bar * 4.0f;
                drawList->AddLine(ImVec2(cx - 5.0f, y), ImVec2(cx + 5.0f, y), gripColor, 2.0f);
            }
            if (ImGui::BeginDragDropSource(
                    ImGuiDragDropFlags_SourceNoDisableHover |
                    ImGuiDragDropFlags_SourceNoPreviewTooltip))
            {
                ImGui::SetDragDropPayload(
                    kEffectReorderPayload,
                    effectName.c_str(),
                    effectName.size() + 1);
                ImGui::EndDragDropSource();
            }

            ImGui::SameLine();

            // Check if effect failed to compile
            bool effectFailed = effectRegistry ? effectRegistry->hasEffectFailed(effectName) : false;
            std::string effectError = effectFailed && effectRegistry ? effectRegistry->getEffectError(effectName) : "";

            // Checkbox to enable/disable effect (read/write via registry)
            // Disabled for failed effects
            if (effectFailed)
                ImGui::BeginDisabled();

            bool effectEnabled = effectRegistry ? effectRegistry->isEffectEnabled(effectName) : true;
            if (ImGui::Checkbox("##enabled", &effectEnabled))
            {
                if (effectRegistry)
                    effectRegistry->setEffectEnabled(effectName, effectEnabled);
                paramsDirty = true;
                reloadNeeded = true;  // enabled set decides the chain
                lastChangeTime = std::chrono::steady_clock::now();
            }

            if (effectFailed)
                ImGui::EndDisabled();

            ImGui::SameLine();

            // Show failed effects in red
            if (effectFailed)
                ImGui::PushStyleColor(ImGuiCol_Text, UI::Error());

            bool treeOpen = ImGui::TreeNode("effect", "%s%s", effectName.c_str(), effectFailed ? " (FAILED)" : "");

            if (effectFailed)
                ImGui::PopStyleColor();

            // Right-click context menu
            if (ImGui::BeginPopupContextItem("effect_context"))
            {
                // Toggle ON/OFF
                if (ImGui::MenuItem(effectEnabled ? "Disable" : "Enable"))
                {
                    if (effectRegistry)
                    {
                        effectRegistry->setEffectEnabled(effectName, !effectEnabled);
                        paramsDirty = true;
                        reloadNeeded = true;  // enabled set decides the chain
                        lastChangeTime = std::chrono::steady_clock::now();
                    }
                }

                // Reset to defaults
                if (ImGui::MenuItem("Reset to Defaults"))
                {
                    for (auto* param : effectRegistry->getParametersForEffect(effectName))
                    {
                        FieldEditor* editor = FieldEditorFactory::instance().getEditor(param->getType());
                        if (editor)
                            editor->resetToDefault(*param);
                    }
                    paramsDirty = true;
                    lastChangeTime = std::chrono::steady_clock::now();
                }

                ImGui::Separator();

                // Insert effects here
                if (ImGui::MenuItem("Insert effects here..."))
                {
                    auto baseIt = std::find(selectedEffects.begin(), selectedEffects.end(), effectName);
                    insertPosition = (baseIt != selectedEffects.end())
                        ? static_cast<int>(std::distance(selectedEffects.begin(), baseIt))
                        : static_cast<int>(i);
                    inSelectionMode = true;
                    addEffectsFocusSearch = true;
                    pendingAddEffects.clear();
                    addEffectsDuplicateCount.clear();
                    addEffectsRemoved.clear();
                }

                // Remove effect
                if (ImGui::MenuItem("Remove"))
                {
                    auto removeIt = std::find(selectedEffects.begin(), selectedEffects.end(), effectName);
                    if (removeIt == selectedEffects.end())
                    {
                        ImGui::EndPopup();
                        if (treeOpen) ImGui::TreePop();
                        ImGui::PopID();
                        break;
                    }
                    std::string removedName = *removeIt;
                    selectedEffects.erase(removeIt);
                    effectRegistry->setSelectedEffects(selectedEffects);
                    effectRegistry->removeEffect(removedName);
                    paramsDirty = true;
                    lastChangeTime = std::chrono::steady_clock::now();
                    applyRequested = true;
                    profileDirty = true;
                    ImGui::EndPopup();
                    // CRITICAL: Must TreePop before break if tree was opened, else assertion failure
                    if (treeOpen) ImGui::TreePop();
                    ImGui::PopID();
                    break;  // Iterator invalidated — exit loop safely
                }

                ImGui::EndPopup();
            }

            // Row drop target for reordering
            ImRect rowDropRect(rowMin, ImVec2(rowMin.x + rowWidth, rowMin.y + itemHeight));
            if (ImGui::BeginDragDropTargetCustom(rowDropRect, ImGui::GetID("##effect_row_drop_target")))
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                        kEffectReorderPayload,
                        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
                {
                    const char* movingName = static_cast<const char*>(payload->Data);
                    if (payload->IsPreview())
                    {
                        if (!reorderPreviewActive || reorderPreviewName != movingName)
                        {
                            reorderPreviewActive = true;
                            reorderPreviewName = movingName;
                            reorderPreviewTargetName.clear();
                            reorderPreviewEffects = selectedEffects;
                        }

                        if (reorderPreviewTargetName != effectName)
                        {
                            auto targetIt = std::find(
                                reorderPreviewEffects.begin(),
                                reorderPreviewEffects.end(),
                                effectName);
                            if (targetIt != reorderPreviewEffects.end())
                            {
                                int targetIndex = static_cast<int>(
                                    std::distance(reorderPreviewEffects.begin(), targetIt));
                                if (applySwapByName(reorderPreviewEffects, movingName, targetIndex))
                                    reorderPreviewTargetName = effectName;
                            }
                        }
                    }

                    if (payload->IsDelivery())
                    {
                        if (reorderPreviewActive && reorderPreviewName == movingName && !reorderPreviewEffects.empty())
                        {
                            if (selectedEffects != reorderPreviewEffects)
                            {
                                selectedEffects = reorderPreviewEffects;
                                effectRegistry->setSelectedEffects(selectedEffects);
                                paramsDirty = true;
                                lastChangeTime = std::chrono::steady_clock::now();
                                applyRequested = true;
                                profileDirty = true;
                            }
                        }
                        reorderPreviewActive = false;
                        reorderPreviewName.clear();
                        reorderPreviewTargetName.clear();
                        reorderPreviewEffects.clear();
                    }
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::PopID();

            if (!treeOpen)
                continue;

            // Show error for failed effects
            if (effectFailed)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, UI::Error());
                ImGui::TextWrapped("Error: %s", effectError.c_str());
                ImGui::PopStyleColor();
                ImGui::TreePop();
                continue;
            }

            // Show preprocessor definitions first (ReShade effects only)
            if (effectRegistry)
            {
                auto& defs = effectRegistry->getPreprocessorDefs(effectName);
                if (!defs.empty())
                {
                    // A framed section: the card background is drawn on a
                    // separate draw channel so it sits behind the tree node and
                    // its inputs, matching the M3 list/card treatment.
                    ImVec2 startPos = ImGui::GetCursorScreenPos();
                    float contentWidth = ImGui::GetContentRegionAvail().x;
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->ChannelsSplit(2);
                    drawList->ChannelsSetCurrent(1);  // Foreground for content

                    if (ImGui::TreeNode("preprocessor", "%s  Preprocessor (%zu)", Icon::BuildUtf8, defs.size()))
                    {
                        ImGui::TextDisabled("Applied when you press %s or Apply", settingsManager.getReloadKey().c_str());
                        ImGui::Spacing();

                        // Align every field to the widest definition name.
                        float nameColumn = 60.0f;
                        for (const auto& def : defs)
                            nameColumn = ImMax(nameColumn, ImGui::CalcTextSize(def.name.c_str()).x + 20.0f);

                        for (size_t defIdx = 0; defIdx < defs.size(); defIdx++)
                        {
                            ImGui::PushID(static_cast<int>(defIdx + 1000));
                            if (renderPreprocessorDef(defs[defIdx], effectRegistry, effectName, nameColumn))
                            {
                                paramsDirty = true;
                                profileDirty = true;
                                reloadNeeded = true;  // macros are compile-time
                                lastChangeTime = std::chrono::steady_clock::now();
                            }
                            ImGui::PopID();
                        }
                        ImGui::TreePop();
                    }

                    // Card background, on channel 0 (behind content).
                    ImVec2 endPos = ImGui::GetCursorScreenPos();
                    drawList->ChannelsSetCurrent(0);
                    const float card_r = ImGuiM3Radius(ImGuiM3Shape_Medium);
                    const ImRect card(ImVec2(startPos.x - 10.0f, startPos.y - 6.0f),
                                      ImVec2(startPos.x + contentWidth + 10.0f, endPos.y + 6.0f));
                    ImGuiM3PathRoundedRect(drawList, card, ImGuiM3ShapeRounding{ card_r, card_r, card_r, card_r },
                                           ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainerLow));
                    drawList->ChannelsMerge();
                }
            }

            // Show parameters for this effect. ReShade uniforms are pushed every
            // frame by updateEffect, so a value edit needs no rebuild.
            auto effectParams = effectRegistry->getParametersForEffect(effectName);
            for (size_t paramIdx = 0; paramIdx < effectParams.size(); paramIdx++)
            {
                ImGui::PushID(static_cast<int>(paramIdx));
                if (renderFieldEditor(*effectParams[paramIdx]))
                {
                    paramsDirty = true;
                    profileDirty = true;
                    lastChangeTime = std::chrono::steady_clock::now();
                }
                ImGui::PopID();
            }

            ImGui::TreePop();
        }

        const ImGuiPayload* activePayload = ImGui::GetDragDropPayload();
        const bool dragActive = activePayload && activePayload->IsDataType(kEffectReorderPayload);
        if (!dragActive)
        {
            // If drag ended outside a row target, still commit the last valid preview.
            if (reorderPreviewActive && !reorderPreviewTargetName.empty() && !reorderPreviewEffects.empty())
            {
                if (selectedEffects != reorderPreviewEffects)
                {
                    selectedEffects = reorderPreviewEffects;
                    effectRegistry->setSelectedEffects(selectedEffects);
                    paramsDirty = true;
                    lastChangeTime = std::chrono::steady_clock::now();
                    applyRequested = true;
                    profileDirty = true;
                }
            }

            reorderPreviewActive = false;
            reorderPreviewName.clear();
            reorderPreviewTargetName.clear();
            reorderPreviewEffects.clear();
        }

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        bool autoApplyVal = settingsManager.getAutoApply();
        if (ImGui::Checkbox("Apply automatically", &autoApplyVal))
        {
            settingsManager.setAutoApply(autoApplyVal);
            settingsManager.save();
        }
        const std::string applyLabel = std::string(Icon::CheckUtf8) + "  Apply";
        float applyWidth = ImGui::CalcTextSize(applyLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SameLine(ImGui::GetWindowWidth() - applyWidth - ImGui::GetStyle().WindowPadding.x);

        // Apply button is always clickable
        if (ImGui::Button(applyLabel.c_str()))
        {
            applyRequested = true;
            paramsDirty = false;
            profileDirty = true;  // Mark for auto-save to profile
        }
        // Note: Auto-apply is handled globally in imgui_overlay.cpp

        // The fallback picker is modal and must be displayed every frame.
        presetBrowser.Display();
        if (presetBrowser.HasSelected())
        {
            importShaderPreset(presetBrowser.GetSelected().string());
            presetBrowser.ClearSelected();
        }
    }

    void ImGuiOverlay::importShaderPreset(const std::string& sourcePath)
    {
        const std::string imported = ConfigSerializer::importShaderProfile(sourcePath);
        if (imported.empty())
        {
            pushToast(LogLevel::Error, "Could not import the shader preset.");
            return;
        }

        // save the outgoing profile, then activate the import. refresh can
        // already select the new name, so the reload is queued here directly.
        const bool saved = autoSaveProfile(true);
        refreshShaderProfiles();
        setActiveShaderProfile(imported);
        pendingShaderProfilePath = activeShaderProfilePath;
        pendingShaderProfile = true;
        applyRequested = true;
        paramsDirty = false;
        profileDirty = false;
        if (!saved)
            pushToast(LogLevel::Error, "Imported \"" + imported + "\", but the previous profile could not be saved.");
        else
            pushToast(LogLevel::Info, "Imported shader preset \"" + imported + "\".");
    }

} // namespace VKIntox
