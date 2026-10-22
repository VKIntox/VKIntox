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
#include <string>
#include <unordered_map>
#include <vector>

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
                ImGui::Spacing();
            }

            // ReShade uniforms are pushed every frame by updateEffect, so a value
            // edit needs no rebuild.
            auto effectParams = effectRegistry->getParametersForEffect(effectName);
            if (effectParams.empty() && defs.empty())
                UI::TextSupport("This effect has no adjustable parameters.");
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

        // Profile row: the active shader preset, with its actions flush right.
        // The label is drawn rather than emitted so it can be centred against the
        // field it names, at the document face's weight.
        if (!activeGameName.empty())
        {
            const ImGuiM3Metrics& profileMetrics = ImGuiM3GetMetrics();
            const float profileD = profileMetrics.density;
            const bool hasShaderProfile = !activeShaderProfileName.empty();

            const float profileIconWidth = profileMetrics.icon_button_size * profileD;
            const int profileActionCount = hasShaderProfile ? 4 : 2;
            const float profileActionsWidth =
                profileIconWidth * (float)profileActionCount +
                ImGui::GetStyle().ItemSpacing.x * (float)(profileActionCount - 1);
            const float actionsX = ImGui::GetWindowWidth() - profileActionsWidth -
                                   ImGui::GetCurrentWindowRead()->WindowPadding.x;

            const float rowH = ImGui::GetFrameHeight();
            const ImVec2 rowPos = ImGui::GetCursorScreenPos();

            const std::string profileLabelText = std::string(Icon::BrushUtf8) + "  Profile:";
            ImFont* profileLabelFont = ImGuiM3FontBold();
            const float profileLabelPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * profileD;
            ImGui::PushFont(profileLabelFont, profileLabelPx);
            const ImVec2 profileLabelSize = ImGui::CalcTextSize(profileLabelText.c_str());
            ImGui::PopFont();
            ImGui::GetWindowDrawList()->AddText(
                profileLabelFont, profileLabelPx,
                ImVec2(rowPos.x + 2.0f * profileD, rowPos.y + (rowH - profileLabelSize.y) * 0.5f),
                ImGuiM3ColorU32(ImGuiM3Role_OnSurface), profileLabelText.c_str());

            // the field starts after the label and stops before the actions
            const float fieldX = rowPos.x + profileLabelSize.x + 20.0f * profileD;
            // fixed at its widest unless the panel is too narrow: it used to grow
            // with the window, so dragging the shell looked like resizing the field
            const float fieldRoom = actionsX - fieldX - 16.0f * profileD;
            const float fieldW = ImClamp(220.0f * profileD, 90.0f * profileD, ImMax(fieldRoom, 90.0f * profileD));
            ImGui::SetCursorScreenPos(ImVec2(fieldX, rowPos.y));
            // the pill the elevated buttons use: a small radius here put a tight
            // ring at the field's corners and read as a cut-off shadow
            const float fieldR = ImMin(rowH * 0.5f, ImGuiM3Radius(ImGuiM3Shape_Full));
            ImGuiM3ShapeRounding fieldRound{ fieldR, fieldR, fieldR, fieldR };
            ImGuiM3DrawElevation(ImGui::GetWindowDrawList(),
                                 ImRect(ImVec2(fieldX, rowPos.y),
                                        ImVec2(fieldX + fieldW, rowPos.y + rowH)),
                                 fieldRound, 1);
            ImGui::SetNextItemWidth(fieldW);
            const char* shaderLabel = activeShaderProfileName.empty() ? "None" : activeShaderProfileName.c_str();
            const float previewBudget = ImMax(24.0f, fieldW - ImGui::GetFrameHeight() - 8.0f);
            // The field and its menu carry an opaque container so a bright game frame
            // behind the blurred backdrop can't wash them out.
            const ImVec4 fieldBg = ImGuiM3Color(ImGuiM3Role_SurfaceContainerHighest);
            const ImVec4 menuBg = ImGuiM3Color(ImGuiM3Role_SurfaceContainerHigh);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, fieldBg);
            ImGui::PushStyleColor(ImGuiCol_PopupBg, menuBg);
            // the frame carries the same radius as its shadow
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, fieldR);
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
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            ImGui::SameLine(actionsX);
            if (ImGui::M3IconButton(Icon::AddUtf8, "Create shader INI profile", ImGuiM3Button_Tonal))
                ImGui::OpenPopup("NewShaderProfilePopup");
            static std::string deleteProfileTarget;
            static std::string renameProfileTarget;
            static char renameShaderProfileName[64] = "";
            if (!activeShaderProfileName.empty())
            {
                ImGui::SameLine();
                if (ImGui::M3IconButton(Icon::EditUtf8, "Rename shader INI profile", ImGuiM3Button_Tonal))
                {
                    renameProfileTarget = activeShaderProfileName;
                    std::snprintf(renameShaderProfileName, sizeof(renameShaderProfileName), "%s",
                                  activeShaderProfileName.c_str());
                    ImGui::OpenPopup("##rename_shader_profile");
                }

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
                if (ImGui::M3IconButton(Icon::DeleteUtf8, "Delete shader INI profile", ImGuiM3Button_Tonal))
                {
                    deleteProfileTarget = activeShaderProfileName;
                    ImGui::OpenPopup("##delete_shader_profile");
                }

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
            if (ImGui::M3IconButton(Icon::UploadUtf8, "Import a ReShade .ini preset", ImGuiM3Button_Tonal))
            {
                if (!settingsManager.getUsePortalIntegration()
                    || !startOpenFileDialog("Import ReShade preset", {"*.ini"}))
                    openPresetBrowser();
            }
            ImGui::EndDisabled();
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

        // The master switch, the clear/add actions and the card's hero now share
        // one row (in the card below), so all that is left up here is the hint.
        UI::TextSupport(("Toggle keybind bound to " + settingsManager.getToggleKey()).c_str());

        // Effect parameters live in their own foldable, resizable sheet at the
        // bottom, folded by default, one tab per effect. The handle between the
        // sheet and the list trades height between them.
        static bool  paramsFolded = true;
        static float paramsSheetHeight = -1.0f;   // px, remembered while unfolded
        static bool  paramsSheetUserSized = false;
        // 0 collapsed, 1 expanded. Spring-driven so a fold/unfold/fold burst keeps
        // its momentum instead of snapping to a stale sample.
        static float  paramsFoldT = 0.0f;
        // per-effect inline unfold state for the all-in-one card
        static std::unordered_map<std::string, bool>  inlineParamsOpen;
        static std::unordered_map<std::string, float> inlineParamsHeight;
        static std::unordered_map<std::string, int>   inlineParamsWarmup;
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

        // fold progress toward the target: the expressive spatial spring, fast and
        // critically damped, so it settles quickly and never rewinds on a re-click.
        const float foldTarget = paramsFolded ? 0.0f : 1.0f;
        if (splitParams)
            paramsFoldT = ImClamp(UI::FoldProgress(ImGui::GetID("##params_fold_spring"), !paramsFolded), 0.0f, 1.0f);
        else
            paramsFoldT = foldTarget;

        const float sheetReserve = footerHeight + resizeHandleH + cardSpacing * 2.0f;
        const float maxSheetH = ImMax(minSheetH, availH - sheetReserve - minListH);

        // the sheet's resting height while open (also the height the body lays
        // out at, so it is revealed rather than squashed)
        float expandedSheetH = sheetHeaderH;
        if (splitParams)
        {
            // lean tall, and taller the more effects there are to edit
            const float effectBias = ImClamp(0.70f + 0.05f * (float)displayedEffects.size(), 0.70f, 0.94f);
            if (!paramsSheetUserSized || paramsSheetHeight <= 0.0f)
                paramsSheetHeight = ImClamp(availH * effectBias, minSheetH, maxSheetH);
            expandedSheetH = ImClamp(paramsSheetHeight, minSheetH, maxSheetH);
        }

        const float foldOvershoot = splitParams ? ImClamp(paramsFoldT, 0.0f, 1.0f) : 0.0f;
        float sheetHeight = 0.0f;
        if (splitParams)
            sheetHeight = ImMin(sheetHeaderH + (expandedSheetH - sheetHeaderH) * foldOvershoot, maxSheetH);

        const float handleReserve = (splitParams && !paramsFolded) ? resizeHandleH + cardSpacing * 2.0f : 0.0f;
        const float listReserve = footerHeight + sheetHeight + handleReserve;
        const float listScrollbarReserve = ImGui::GetStyle().ScrollbarSize + 4.0f * listMetrics.density;

        // The effect list is its own labelled card: toggling and reordering only.
        // A level-1 shadow lifts it off the shell, like an expressive list card.
        {
            const ImVec2 cardPos = ImGui::GetCursorScreenPos();
            // hug the card, not the space reserved under it, or it reads as a band
            const float cardH = ImMax(ImGui::GetContentRegionAvail().y - listReserve, 1.0f);
            const ImVec2 cardSize(ImGui::GetContentRegionAvail().x, cardH);
            const float cr = ImGuiM3Radius(ImGuiM3Shape_LargeIncreased);
            ImGuiM3DrawElevation(ImGui::GetWindowDrawList(),
                                 ImRect(cardPos, ImVec2(cardPos.x + cardSize.x, cardPos.y + cardSize.y)),
                                 ImGuiM3ShapeRounding{ cr, cr, cr, cr }, 1);
        }
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainerLow));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ImGuiM3Radius(ImGuiM3Shape_LargeIncreased));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * listMetrics.density, 16.0f * listMetrics.density));
        ImGui::BeginChild("EffectListCard", ImVec2(0, -listReserve),
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            // Hero row: accent disc + "Effects" on the left, then Clear all and
            // Add effects, then the master switch, all on one line.
            const std::string clearAllLabel = std::string(Icon::DeleteUtf8) + "  Clear all";
            const std::string addEffectsLabel = std::string(Icon::AddUtf8) + "  Add effects...";
            const float d = listMetrics.density;
            const float actionH = 36.0f * d;

            // the buttons' own measurement, so both pills get identical padding
            const float clearW = ImGui::M3ButtonWidth(clearAllLabel.c_str());
            const float addW = ImGui::M3ButtonWidth(addEffectsLabel.c_str());
            const float switchScale = 1.15f;
            const float switchW = listMetrics.switch_track_width * d * switchScale;
            const float gap = 10.0f * d;
            const float seam = 2.0f * d;   // the connected group's seam

            ImRect heroRow;
            bool effectsOn = state.effectsEnabled;
            // the switch owns the right edge; the actions are laid out to its left
            if (ImGui::M3HeroSwitch(Icon::WandShineUtf8, "Effects", "##effects_master", &effectsOn,
                                    "Master switch for every effect in this preset.", switchScale,
                                    0.0f, &heroRow))
                toggleEffectsRequested = true;

            const float rowCy = heroRow.GetCenter().y;
            float x = heroRow.Max.x - switchW - gap - clearW - seam - addW;
            ImGui::SetCursorScreenPos(ImVec2(x, rowCy - actionH * 0.5f));
            ImGui::BeginDisabled(selectedEffects.empty());
            if (ImGui::M3Button(clearAllLabel.c_str(), ImGuiM3Button_DestructiveOutlined,
                                ImVec2(clearW, actionH), false, ImGui::ImGuiM3Button_Leading))
                ImGui::OpenPopup("##clear_all_confirm");
            ImGui::EndDisabled();
            x += clearW + seam;
            ImGui::SetCursorScreenPos(ImVec2(x, rowCy - actionH * 0.5f));
            if (ImGui::M3Button(addEffectsLabel.c_str(), ImGuiM3Button_Filled, ImVec2(addW, actionH), false,
                                ImGui::ImGuiM3Button_Trailing))
            {
                inSelectionMode = true;
                addEffectsFocusSearch = true;
                insertPosition = -1;  // Append to end
                pendingAddEffects.clear();
                addEffectsDuplicateCount.clear();
                addEffectsRemoved.clear();
            }
            // Destructive action, so confirm it through the shared dialog frame.
            // The dialog lives in this card's window too: popup ids are window
            // scoped, and a confirm begun in another window never answers the
            // OpenPopup from this one.
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
            ImGui::SetCursorScreenPos(ImVec2(heroRow.Min.x, heroRow.Max.y));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
        }
        ImGui::Spacing();
        // borderless child: no padding of its own, the card already insets it
        ImGui::BeginChild("EffectList", ImVec2(0, 0));

        // Show selected effects with their parameters
        float itemHeight = ImGui::GetFrameHeightWithSpacing();
        for (size_t i = 0; i < displayedEffects.size(); i++)
        {
            const std::string& effectName = displayedEffects[i];
            ImGui::PushID(effectName.c_str());

            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            float rowWidth = ImMax(1.0f, ImGui::GetContentRegionAvail().x - listScrollbarReserve);

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

            // name row: a rounded surface of its own, name on the document face,
            // parameter count as trailing metadata. In split mode it jumps to the
            // effect's tab in the parameters card; otherwise it unfolds that
            // effect's parameters in place, spring-animated.
            bool treeOpen = false;
            {
                const ImVec2 namePos = ImGui::GetCursorScreenPos();
                const float nameW = ImMax(ImGui::GetContentRegionAvail().x - listScrollbarReserve, 1.0f);
                const float nameH = ImGui::GetFrameHeight();
                ImGui::InvisibleButton("##name_row", ImVec2(nameW, nameH));
                const bool nameHovered = ImGui::IsItemHovered();
                const bool nameClicked = ImGui::IsItemClicked();

                if (splitParams)
                {
                    if (nameClicked)
                    {
                        paramsActiveEffect = effectName;
                        paramsFolded = false;
                    }
                }
                else
                {
                    if (nameClicked)
                        inlineParamsOpen[effectName] = !inlineParamsOpen[effectName];
                }
                treeOpen = !splitParams && inlineParamsOpen[effectName];

                ImDrawList* rowDl = ImGui::GetWindowDrawList();
                const ImRect nameRect(namePos, ImVec2(namePos.x + nameW, namePos.y + nameH));
                const float density = listMetrics.density;
                const float padX = 10.0f * density;
                const float nameRadius = ImGuiM3Radius(ImGuiM3Shape_Small);
                if (nameHovered)
                    ImGuiM3DrawStateLayer(rowDl, nameRect,
                                          ImGuiM3ShapeRounding{ nameRadius, nameRadius, nameRadius, nameRadius },
                                          ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);

                // trailing chevron, plus the parameter count as secondary metadata
                const float chevronPx = ImGui::GetFontSize() * 1.05f;
                const float chevronX = nameRect.Max.x - padX - chevronPx;
                ImGuiM3TreeNodeArrow(rowDl, ImVec2(chevronX, nameRect.Min.y + (nameH - chevronPx) * 0.5f),
                                     chevronPx, ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant),
                                     splitParams || treeOpen);

                ImFont* metaFont = ImGuiM3FontBodyBold();
                const float metaPx = 11.5f * density;
                int paramCount = 0;
                if (effectRegistry && !effectFailed)
                    paramCount = static_cast<int>(effectRegistry->getParametersForEffect(effectName).size());
                const std::string meta = effectFailed
                    ? std::string("failed to compile")
                    : std::to_string(paramCount) + (paramCount == 1 ? " parameter" : " parameters");
                ImGui::PushFont(metaFont, metaPx);
                const ImVec2 metaSize = ImGui::CalcTextSize(meta.c_str());
                ImGui::PopFont();
                rowDl->AddText(metaFont, metaPx,
                               ImVec2(chevronX - 10.0f * density - metaSize.x,
                                      nameRect.Min.y + (nameH - metaSize.y) * 0.5f),
                               effectFailed ? ImGuiM3ColorU32(ImGuiM3Role_Error)
                                            : ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant),
                               meta.c_str());

                // the name itself, on the document face
                ImFont* nameFont = ImGuiM3FontBold();
                const float namePx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * density;
                const float nameBudget = ImMax(24.0f, chevronX - 10.0f * density - metaSize.x -
                                                      12.0f * density - (nameRect.Min.x + padX));
                const char* shownName = UI::TruncateLabel(effectName.c_str(), nameBudget);
                ImGui::PushFont(nameFont, namePx);
                const ImVec2 nameSize = ImGui::CalcTextSize(shownName);
                ImGui::PopFont();
                rowDl->AddText(nameFont, namePx,
                               ImVec2(nameRect.Min.x + padX, nameRect.Min.y + (nameH - nameSize.y) * 0.5f),
                               effectFailed ? ImGuiM3ColorU32(ImGuiM3Role_Error)
                                            : ImGuiM3ColorU32(ImGuiM3Role_OnSurface),
                               shownName);

                if (effectFailed && nameHovered)
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

            // Inline parameters for the all-in-one card. The spring is stepped
            // every frame, so opening *and* closing both animate from wherever the
            // fold actually is; it only targets open once the body's height has
            // been measured (see the warm-up pass below).
            const ImGuiID inlineFoldId = ImGui::GetID("##inline_fold");
            int& inlineWarmup = inlineParamsWarmup[effectName];
            const bool inlineHasHeight = inlineParamsHeight[effectName] > 0.0f;
            if (!treeOpen && !inlineHasHeight && inlineWarmup <= 0)
                inlineWarmup = 2;
            const float unfold = ImClamp(
                UI::FoldProgress(inlineFoldId, treeOpen && inlineWarmup == 0, false, 1.6f), 0.0f, 1.0f);
            if (treeOpen || unfold > 0.002f)
            {
                float& fullH = inlineParamsHeight[effectName];
                const ImVec2 paramsPos = ImGui::GetCursorScreenPos();
                const float paramsW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
                const ImGuiChildFlags paramsFlags = ImGuiChildFlags_AutoResizeY;
                const ImGuiWindowFlags paramsWindowFlags = ImGuiWindowFlags_NoScrollbar |
                                                           ImGuiWindowFlags_NoScrollWithMouse;

                if (inlineWarmup > 0)
                {
                    // Measure before revealing. AutoResizeY only settles on the real
                    // content height on its second frame, and it cannot be measured
                    // while clipped (clipped items contribute no extent) - which is
                    // exactly why the old reveal came up short and then snapped open
                    // when the animation ended.
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);
                    ImGui::BeginChild("##inline_params", ImVec2(0, 0), paramsFlags, paramsWindowFlags);
                    renderEffectParams(effectName);
                    ImGui::EndChild();
                    ImGui::PopStyleVar();
                    const float measured = ImGui::GetItemRectSize().y;
                    if (measured > 0.0f)
                        fullH = measured;
                    --inlineWarmup;
                    ImGui::SetCursorScreenPos(paramsPos);
                    ImGuiWindow* listWindow = ImGui::GetCurrentWindow();
                    listWindow->DC.CursorMaxPos.y = ImMin(listWindow->DC.CursorMaxPos.y, paramsPos.y);
                    ImGui::Dummy(ImVec2(0.0f, 0.0f));
                }
                else
                {
                    const float targetH = ImMax(fullH, 1.0f);
                    const bool settled = unfold >= 0.999f;
                    const float revealH = settled ? targetH : targetH * unfold;
                    if (!settled)
                        ImGui::PushClipRect(paramsPos, ImVec2(paramsPos.x + paramsW, paramsPos.y + revealH), true);
                    ImGui::BeginChild("##inline_params", ImVec2(0, 0), paramsFlags, paramsWindowFlags);
                    if (renderEffectParams(effectName))
                    {
                        paramsDirty = true;
                        profileDirty = true;
                        lastChangeTime = std::chrono::steady_clock::now();
                    }
                    ImGui::EndChild();
                    if (!settled)
                    {
                        ImGui::PopClipRect();
                        // ride the reveal, so the rows below move with it
                        ImGui::SetCursorScreenPos(ImVec2(paramsPos.x, paramsPos.y + revealH));
                        ImGuiWindow* listWindow = ImGui::GetCurrentWindow();
                        listWindow->DC.CursorMaxPos.y = ImMin(listWindow->DC.CursorMaxPos.y, paramsPos.y + revealH);
                        ImGui::Dummy(ImVec2(0.0f, 0.0f));
                    }
                    else
                    {
                        // fully open: re-learn the height from a clean layout
                        const float measured = ImGui::GetItemRectSize().y;
                        if (measured > 0.0f)
                            fullH = measured;
                    }
                }
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
            {
                const ImVec2 cardPos = ImGui::GetCursorScreenPos();
                const ImVec2 cardSize(ImMax(ImGui::GetContentRegionAvail().x, 1.0f), sheetHeight);
                const float cr = ImGuiM3Radius(ImGuiM3Shape_LargeIncreased);
                ImGuiM3DrawElevation(ImGui::GetWindowDrawList(),
                                     ImRect(cardPos, ImVec2(cardPos.x + cardSize.x, cardPos.y + cardSize.y)),
                                     ImGuiM3ShapeRounding{ cr, cr, cr, cr }, 2);
            }
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainerLow));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ImGuiM3Radius(ImGuiM3Shape_LargeIncreased));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * listMetrics.density, 16.0f * listMetrics.density));
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
                {
                    paramsFolded = !paramsFolded;
                }

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
                const float titleSize = ImGuiM3GetTypeStyle(ImGuiM3Type_TitleMedium).size * listMetrics.density;
                headDl->AddText(titleFont, titleSize,
                                ImVec2(x, headMin.y + (headH - titleSize) * 0.5f),
                                ImGuiM3ColorU32(ImGuiM3Role_Primary), "Effect Parameters");
                ImGuiM3TreeNodeArrow(headDl,
                                     ImVec2(headMin.x + headW - padX - iconPx,
                                            headMin.y + (headH - titleSize) * 0.5f),
                                     iconPx, ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant), !paramsFolded);
            }

            if (paramsFoldT > 0.01f)
            {
                // the body lays out at its full open height and the card clips it,
                // so the fold reveals the sheet rather than squashing it
                const float bodyH = ImMax(0.0f, expandedSheetH - sheetHeaderH);
                ImGui::BeginChild("##params_body", ImVec2(0, bodyH), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                ImGui::Spacing();
                if (displayedEffects.empty())
                {
                    UI::TextSupport("No effects in this preset. Add effects to edit their parameters.");
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
                ImGui::EndChild();
            }
            else
            {
                // fully folded; keep the slide target in step so an unfold doesn't
                // animate a stale change
                paramsSlideShown = paramsActiveEffect;
                paramsSlideActive = false;
            }

            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        }

        // Apply lives in the band reserved below the cards: flush right, above the
        // dock. Positioned with SetCursorPos - SameLine() would snap it back onto
        // whatever row happened to be submitted last.
        const std::string applyLabel = std::string(Icon::CheckUtf8) + "  Apply";
        const float applyPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * listMetrics.density;
        ImGui::PushFont(ImGuiM3FontBold(), applyPx);
        const float applyTextW = ImGui::CalcTextSize(applyLabel.c_str()).x;
        ImGui::PopFont();
        const float applyWidth = ImMax(applyTextW + 44.0f * listMetrics.density,
                                       132.0f * listMetrics.density);
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - footerRowH);
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - applyWidth - ImGui::GetStyle().WindowPadding.x);

        if (ImGui::M3Button(applyLabel.c_str(), ImGuiM3Button_Filled, ImVec2(applyWidth, footerRowH)))
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
