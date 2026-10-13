#include "imgui_overlay.hh"
#include "effects/effect_registry.hh"
#include "settings_manager.hh"
#include "config_serializer.hh"
#include "async_writer.hh"
#include "params/field_editor.hh"
#include "logger.hh"
#include "overlay/ui_theme.hh"
#include "overlay/ui_icons.hh"
#include "overlay/effect_icons.hh"
#include "overlay/ui_dialog.hh"
#include "overlay/ui_menu.hh"
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

            if (UI::BeginM3MenuContext("##preproc_reset"))
            {
                if (UI::M3MenuItem("Reset to default"))
                {
                    registry->setPreprocessorDefValue(effectName, def.name, def.defaultValue);
                    changed = true;
                }
                UI::EndM3Menu();
            }

            ImGui::PopID();
            return changed;
        }

        // Section title for a card: leading icon plus an emphasized primary
        // label, matching the parameters sheet header. Reserves the row.
        inline float drawCardHeaderRow(const char* iconUtf8, const char* title)
        {
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            const float h = ImGui::GetFrameHeight() + 4.0f * m.density;
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            ImGui::Dummy(ImVec2(w, h));

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float iconPx = ImGui::GetFontSize() * 1.35f;
            const float padX = 6.0f * m.density;
            float x = min.x + padX;
            if (ImGuiM3IconFont() && iconUtf8)
            {
                ImGuiM3DrawIcon(dl, iconUtf8,
                                ImRect(ImVec2(x, min.y + (h - iconPx) * 0.5f),
                                       ImVec2(x + iconPx, min.y + (h + iconPx) * 0.5f)),
                                iconPx, ImGuiM3ColorU32(ImGuiM3Role_Primary));
                x += iconPx + 8.0f * m.density;
            }
            ImFont* font = ImGuiM3FontBold();
            if (!font)
                font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            dl->AddText(font, fs, ImVec2(x, min.y + (h - fs) * 0.5f),
                        ImGuiM3ColorU32(ImGuiM3Role_Primary), title);
            return h;
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

        // When on, each effect edits in its own tabbed card at the bottom; when
        // off, the parameters live inline in the effect list (all-in-one card).
        const bool splitParams = settingsManager.getSeparateEffectParams();

        // Renders one effect's preprocessor definitions and parameters, shared by
        // the inline list and the separate parameters card. Returns changed.
        auto renderEffectParams = [&](const std::string& effectName) -> bool {
            bool changed = false;
            if (effectRegistry->hasEffectFailed(effectName))
            {
                ImGui::PushStyleColor(ImGuiCol_Text, UI::Error());
                ImGui::TextWrapped("Failed to compile: %s",
                                   effectRegistry->getEffectError(effectName).c_str());
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            auto& defs = effectRegistry->getPreprocessorDefs(effectName);
            if (!defs.empty())
            {
                // folded by default; opened on demand
                if (ImGui::TreeNodeEx("##preprocessor",
                                      ImGuiTreeNodeFlags_SpanAvailWidth,
                                      "%s  Preprocessor (%zu)", Icon::BuildUtf8, defs.size()))
                {
                    ImGui::Spacing();
                    // align every field to the widest definition name
                    float nameColumn = 60.0f;
                    for (const auto& def : defs)
                        nameColumn = ImMax(nameColumn, ImGui::CalcTextSize(def.name.c_str()).x + 20.0f);
                    for (size_t defIdx = 0; defIdx < defs.size(); defIdx++)
                    {
                        ImGui::PushID(static_cast<int>(defIdx + 1000));
                        if (renderPreprocessorDef(defs[defIdx], effectRegistry, effectName, nameColumn))
                        {
                            changed = true;
                            reloadNeeded = true;  // macros are compile-time
                        }
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
            }

            // ReShade uniforms are pushed every frame by updateEffect, so a value
            // edit needs no rebuild.
            auto effectParams = effectRegistry->getParametersForEffect(effectName);
            if (effectParams.empty() && defs.empty())
                ImGui::TextDisabled("This effect has no adjustable parameters.");
            for (size_t paramIdx = 0; paramIdx < effectParams.size(); paramIdx++)
            {
                ImGui::PushID(static_cast<int>(paramIdx));
                if (renderFieldEditor(*effectParams[paramIdx]))
                    changed = true;
                ImGui::PopID();
            }
            return changed;
        };

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
            const float previewBudget = ImMax(24.0f, ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - 8.0f);
            if (UI::BeginM3Combo("##shaderprofile", UI::TruncateLabel(shaderLabel, previewBudget)))
            {
                for (const auto& profile : shaderProfiles)
                {
                    const bool selected = profile == activeShaderProfileName;
                    if (UI::M3MenuRadioItem(profile.c_str(), selected) && !selected)
                    {
                        if (!switchShaderProfile(profile))
                            pushToast(LogLevel::Error, "Could not save the active shader profile.");
                    }
                }
                UI::EndM3Combo();
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
        // Effect parameters live in their own foldable, resizable sheet at the
        // bottom, folded by default, one tab per effect. The handle between the
        // sheet and the list trades height between them.
        static bool  paramsFolded = true;
        static float paramsSheetHeight = -1.0f;   // px, remembered while unfolded
        static bool  paramsSheetUserSized = false;
        static std::string paramsActiveEffect;
        static std::string paramsSlideShown;      // last effect whose body was shown
        static std::string paramsSlideFrom;       // effect sliding out
        static bool   paramsSlideActive = false;
        static double paramsSlideStart = 0.0;
        static int    paramsSlideDir = 1;

        const ImGuiM3Metrics& listMetrics = ImGuiM3GetMetrics();
        const float cardSpacing = ImGui::GetStyle().ItemSpacing.y;
        const float sheetHeaderH = ImGui::GetFrameHeight() + 24.0f * listMetrics.density;
        const float resizeHandleH = 12.0f * listMetrics.density;
        const float footerRowH = ImGui::GetFrameHeight();
        const float footerHeight = footerRowH + cardSpacing;

        const float availH = ImGui::GetContentRegionAvail().y;
        const float minListH = 110.0f * listMetrics.density;
        const float minSheetH = 210.0f * listMetrics.density;
        const float sheetReserve = footerHeight + (paramsFolded ? 0.0f : resizeHandleH + cardSpacing * 2.0f);
        const float maxSheetH = ImMax(minSheetH, availH - sheetReserve - minListH);

        float sheetHeight = 0.0f;
        if (splitParams)
        {
            sheetHeight = sheetHeaderH;
            if (!paramsFolded)
            {
                // lean tall, and taller the more effects there are to edit
                const float effectBias = ImClamp(0.70f + 0.05f * (float)displayedEffects.size(), 0.70f, 0.94f);
                if (!paramsSheetUserSized || paramsSheetHeight <= 0.0f)
                    paramsSheetHeight = ImClamp(availH * effectBias, minSheetH, maxSheetH);
                sheetHeight = ImClamp(paramsSheetHeight, minSheetH, maxSheetH);
            }
        }

        const float handleReserve = (splitParams && !paramsFolded) ? resizeHandleH + cardSpacing * 2.0f : 0.0f;
        const float listReserve = footerHeight + sheetHeight + handleReserve;

        // The effect list is its own labelled card: toggling and reordering only.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainerLow));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ImGuiM3Radius(ImGuiM3Shape_Large));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * listMetrics.density, 10.0f * listMetrics.density));
        ImGui::BeginChild("EffectListCard", ImVec2(0, -listReserve),
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawCardHeaderRow(Icon::AutoAwesomeUtf8, "Effects");
        ImGui::Spacing();
        // borderless child: no padding of its own, the card already insets it
        ImGui::BeginChild("EffectList", ImVec2(0, 0));

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

            // name row: in split mode it jumps to the effect's tab in the
            // parameters card; otherwise it expands to edit params inline.
            bool treeOpen = false;
            if (splitParams)
            {
                if (effectFailed)
                    ImGui::PushStyleColor(ImGuiCol_Text, UI::Error());
                ImGui::Selectable(effectName.c_str(), false, ImGuiSelectableFlags_None,
                                  ImVec2(ImGui::GetContentRegionAvail().x, 0.0f));
                if (effectFailed)
                    ImGui::PopStyleColor();

                if (effectFailed && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", effectError.c_str());
                if (ImGui::IsItemClicked())
                {
                    paramsActiveEffect = effectName;
                    paramsFolded = false;
                }
            }
            else
            {
                if (effectFailed)
                    ImGui::PushStyleColor(ImGuiCol_Text, UI::Error());
                treeOpen = ImGui::TreeNode("effect", "%s%s", effectName.c_str(),
                                           effectFailed ? " (FAILED)" : "");
                if (effectFailed)
                    ImGui::PopStyleColor();
                if (effectFailed && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", effectError.c_str());
            }

            // Right-click context menu
            if (UI::BeginM3MenuContext("effect_context"))
            {
                // Toggle ON/OFF
                if (UI::M3MenuItem(effectEnabled ? "Disable" : "Enable"))
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
                if (UI::M3MenuItem("Reset to Defaults"))
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
                if (UI::M3MenuItem("Insert effects here..."))
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
                if (UI::M3MenuItem("Remove"))
                {
                    auto removeIt = std::find(selectedEffects.begin(), selectedEffects.end(), effectName);
                    if (removeIt == selectedEffects.end())
                    {
                        UI::EndM3Menu();
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
                    UI::EndM3Menu();
                    if (treeOpen) ImGui::TreePop();
                    ImGui::PopID();
                    break;  // Iterator invalidated — exit loop safely
                }

                UI::EndM3Menu();
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

            // inline parameters for the all-in-one card
            if (treeOpen)
            {
                if (renderEffectParams(effectName))
                {
                    paramsDirty = true;
                    profileDirty = true;
                    lastChangeTime = std::chrono::steady_clock::now();
                }
                ImGui::TreePop();
            }

            ImGui::PopID();
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

        ImGui::EndChild();  // EffectList
        ImGui::EndChild();  // EffectListCard
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        // Resize handle: drag to trade height between the list and the sheet.
        if (splitParams && !paramsFolded)
        {
            const ImVec2 handleMin = ImGui::GetCursorScreenPos();
            const float handleW = ImGui::GetContentRegionAvail().x;
            ImGui::InvisibleButton("##params_resize", ImVec2(handleW, resizeHandleH));
            const ImGuiID handleId = ImGui::GetItemID();
            const bool handleHovered = ImGui::IsItemHovered();
            const bool handleActive = ImGui::IsItemActive();
            if (handleHovered || handleActive)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (handleActive)
            {
                // the handle rides the boundary: moving it down grows the list
                // above and shrinks the sheet below, so the delta is subtracted.
                const float dy = ImGui::GetIO().MouseDelta.y;
                if (dy != 0.0f)
                {
                    paramsSheetHeight = ImClamp(paramsSheetHeight - dy, minSheetH, maxSheetH);
                    paramsSheetUserSized = true;
                }
            }
            if (handleHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                paramsSheetUserSized = false;  // back to the automatic height

            // same widen-and-tint-on-touch the scrollbar thumb uses
            const float grow = ImSaturate(
                ImGuiM3SpringStepEffectsFast(handleId ^ 0x52535A, (handleHovered || handleActive) ? 1.0f : 0.0f));
            const float restW = 40.0f * listMetrics.density;
            const float hoverW = 64.0f * listMetrics.density;
            const float pillW = ImLerp(restW, hoverW, grow);
            const float pillH = (3.0f + grow) * listMetrics.density;
            const ImVec4 restCol = ImGuiM3Color(ImGuiM3Role_OutlineVariant);
            const ImVec4 activeCol = handleActive ? ImGuiM3Color(ImGuiM3Role_Primary)
                                                  : ImGuiM3Color(ImGuiM3Role_OnSurface);
            const ImU32 pillCol = ImGui::GetColorU32(ImVec4(ImLerp(restCol.x, activeCol.x, grow),
                                                            ImLerp(restCol.y, activeCol.y, grow),
                                                            ImLerp(restCol.z, activeCol.z, grow),
                                                            ImLerp(restCol.w, activeCol.w, grow)));

            ImDrawList* handleDl = ImGui::GetWindowDrawList();
            const float cx = handleMin.x + handleW * 0.5f;
            const float cy = handleMin.y + resizeHandleH * 0.5f;
            handleDl->AddRectFilled(ImVec2(cx - pillW * 0.5f, cy - pillH * 0.5f),
                                    ImVec2(cx + pillW * 0.5f, cy + pillH * 0.5f),
                                    pillCol, pillH * 0.5f);
        }

        // Effect Parameters: a foldable sheet at the bottom, folded by default,
        // one tab per effect. Only shown when the all-in-one card is off.
        if (splitParams)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainerLow));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ImGuiM3Radius(ImGuiM3Shape_Large));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * listMetrics.density, 10.0f * listMetrics.density));
            ImGui::BeginChild("EffectParamsCard", ImVec2(0, sheetHeight),
                              ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

            // fold header: chevron plus emphasized title, the whole row toggles
            {
                const float headH = ImGui::GetFrameHeight() + 4.0f * listMetrics.density;
                const ImVec2 headMin = ImGui::GetCursorScreenPos();
                const float headW = ImGui::GetContentRegionAvail().x;
                ImGui::InvisibleButton("##params_fold", ImVec2(headW, headH));
                const bool headHovered = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked())
                    paramsFolded = !paramsFolded;

                ImDrawList* headDl = ImGui::GetWindowDrawList();
                const ImRect headRect(headMin, ImVec2(headMin.x + headW, headMin.y + headH));
                const float headRadius = ImGuiM3Radius(ImGuiM3Shape_Large);
                if (headHovered)
                    ImGuiM3DrawStateLayer(headDl, headRect,
                                          ImGuiM3ShapeRounding{ headRadius, headRadius, headRadius, headRadius },
                                          ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);

                const float iconPx = ImGui::GetFontSize() * 1.35f;
                const float padX = 6.0f * listMetrics.density;
                float x = headMin.x + padX;
                if (ImGuiM3IconFont())
                {
                    ImGuiM3DrawIcon(headDl, Icon::TuneUtf8,
                                    ImRect(ImVec2(x, headMin.y + (headH - iconPx) * 0.5f),
                                           ImVec2(x + iconPx, headMin.y + (headH + iconPx) * 0.5f)),
                                    iconPx, ImGuiM3ColorU32(ImGuiM3Role_Primary));
                    x += iconPx + 8.0f * listMetrics.density;
                }
                ImFont* titleFont = ImGuiM3FontBold();
                if (!titleFont)
                    titleFont = ImGui::GetFont();
                const float titleSize = ImGui::GetFontSize();
                headDl->AddText(titleFont, titleSize,
                                ImVec2(x, headMin.y + (headH - titleSize) * 0.5f),
                                ImGuiM3ColorU32(ImGuiM3Role_Primary), "Effect Parameters");
                ImGuiM3TreeNodeArrow(headDl,
                                     ImVec2(headMin.x + headW - padX - iconPx,
                                            headMin.y + (headH - titleSize) * 0.5f),
                                     iconPx, ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant), !paramsFolded);
            }

            if (!paramsFolded)
            {
                ImGui::Spacing();
                if (displayedEffects.empty())
                {
                    ImGui::TextDisabled("No effects in this preset. Add effects to edit their parameters.");
                }
                else
                {
                    // tabs are the navbar's connected button group: one segment
                    // per effect, each carrying its add-effects icon.
                    int activeIndex = 0;
                    for (int i = 0; i < static_cast<int>(displayedEffects.size()); i++)
                        if (displayedEffects[i] == paramsActiveEffect)
                        {
                            activeIndex = i;
                            break;
                        }

                    std::vector<const char*> tabLabels;
                    std::vector<ImWchar>     tabIcons;
                    tabLabels.reserve(displayedEffects.size());
                    tabIcons.reserve(displayedEffects.size());
                    for (const auto& effectName : displayedEffects)
                    {
                        tabLabels.push_back(effectName.c_str());
                        tabIcons.push_back(effectIconCodepointFor(effectName));
                    }

                    // the tabs scroll horizontally when they can't all fit
                    ImGui::BeginChild("##effect_param_tabs", ImVec2(0, 0),
                                      ImGuiChildFlags_AutoResizeY,
                                      ImGuiWindowFlags_HorizontalScrollbar);
                    if (ImGui::M3ConnectedButtonGroup("##effect_param_segments", tabLabels.data(),
                                                      static_cast<int>(tabLabels.size()), &activeIndex, tabIcons.data(),
                                                      true))
                        paramsActiveEffect = displayedEffects[activeIndex];
                    ImGui::EndChild();

                    // keep the selection valid when the active effect is removed
                    if (std::find(displayedEffects.begin(), displayedEffects.end(), paramsActiveEffect) == displayedEffects.end())
                        paramsActiveEffect = displayedEffects.front();

                    // slide transition between effects, same clock and bezier as
                    // the top-level view swipes.
                    if (paramsActiveEffect != paramsSlideShown)
                    {
                        auto indexOf = [&](const std::string& n) {
                            auto it = std::find(displayedEffects.begin(), displayedEffects.end(), n);
                            return it == displayedEffects.end()
                                ? 0 : static_cast<int>(std::distance(displayedEffects.begin(), it));
                        };
                        paramsSlideFrom = paramsSlideShown;
                        paramsSlideActive = !paramsSlideShown.empty() && paramsSlideShown != paramsActiveEffect;
                        paramsSlideDir = (indexOf(paramsActiveEffect) >= indexOf(paramsSlideFrom)) ? 1 : -1;
                        paramsSlideStart = ImGui::GetTime();
                        paramsSlideShown = paramsActiveEffect;
                    }

                    constexpr float kParamsSlideSeconds = 0.30f;
                    constexpr float kParamBezX1 = 0.05f, kParamBezY1 = 0.90f;
                    constexpr float kParamBezX2 = 0.10f, kParamBezY2 = 1.06f;
                    float paramsEase = 1.0f;
                    if (paramsSlideActive)
                    {
                        const float clock = static_cast<float>((ImGui::GetTime() - paramsSlideStart) / kParamsSlideSeconds);
                        if (clock >= 1.0f)
                            paramsSlideActive = false;
                        else
                            paramsEase = UI::CubicBezierEase(clock, kParamBezX1, kParamBezY1, kParamBezX2, kParamBezY2);
                    }

                    ImGui::Spacing();
                    const ImVec2 slidePos = ImGui::GetCursorScreenPos();
                    const ImVec2 slideSize = ImGui::GetContentRegionAvail();
                    const bool showFrom = paramsSlideActive && paramsSlideFrom != paramsActiveEffect;
                    const float travel = 0.20f * slideSize.x * static_cast<float>(paramsSlideDir);
                    const float offsetIn = showFrom ? (1.0f - paramsEase) * travel : 0.0f;
                    const float offsetOut = showFrom ? -paramsEase * travel : 0.0f;
                    const float alphaIn = showFrom ? ImClamp(paramsEase, 0.0f, 1.0f) : 1.0f;
                    const float alphaOut = showFrom ? ImClamp(1.0f - paramsEase * 2.5f, 0.0f, 1.0f) : 1.0f;

                    auto renderSlidingParams = [&](const std::string& effectName, float offset, float alpha) {
                        ImGui::SetCursorScreenPos(ImVec2(slidePos.x + offset, slidePos.y));
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha * ImGui::GetStyle().Alpha);
                        ImGui::BeginChild(("##params_" + effectName).c_str(), slideSize,
                                          ImGuiChildFlags_AlwaysUseWindowPadding);
                        if (renderEffectParams(effectName))
                        {
                            paramsDirty = true;
                            profileDirty = true;
                            lastChangeTime = std::chrono::steady_clock::now();
                        }
                        ImGui::EndChild();
                        ImGui::PopStyleVar();
                    };

                    if (showFrom && alphaOut > 0.01f)
                        renderSlidingParams(paramsSlideFrom, offsetOut, alphaOut);
                    renderSlidingParams(paramsActiveEffect, offsetIn, alphaIn);
                    ImGui::SetCursorScreenPos(slidePos);
                }
            }
            else
            {
                // no body while folded; keep the slide target in step so an unfold
                // doesn't animate a stale change
                paramsSlideShown = paramsActiveEffect;
                paramsSlideActive = false;
            }

            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        }

        // pin the apply row to the bottom of the view
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - footerRowH);

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
