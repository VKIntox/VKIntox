#include "overlay/shell/imgui_overlay.hh"
#include "config/settings_manager.hh"
#include "util/logger.hh"
#include "overlay/theme/ui_theme.hh"
#include "overlay/theme/ui_icons.hh"
#include "overlay/shell/ui_menu.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include "vendor/imgui/imgui.h"

namespace VKIntox
{
    namespace
    {
        bool settingSwitchRow(const char* id, const char* label, const char* support, bool* value)
        {
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            const float d = m.density;
            const float rowH = (support && support[0] ? 64.0f : 56.0f) * d;
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const float rowW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
            const float switchW = m.switch_track_width * d;
            const float switchH = m.list_item_height_1 * d;
            const float leftW = ImMax(1.0f, rowW - switchW - 24.0f * d);
            const bool rowClicked = ImGui::InvisibleButton(id, ImVec2(leftW, rowH));
            if (rowClicked)
            {
                *value = !*value;
                ImGui::MarkItemEdited(ImGui::GetItemID());
            }
            const bool rowHovered = ImGui::IsItemHovered();
            const ImRect rowRect(rowMin, ImVec2(rowMin.x + rowW, rowMin.y + rowH));
            const float radius = ImGuiM3Radius(ImGuiM3Shape_Large);
            if (rowHovered)
                ImGuiM3DrawStateLayer(ImGui::GetWindowDrawList(), rowRect,
                                      ImGuiM3ShapeRounding{ radius, radius, radius, radius },
                                      ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);

            const float textRight = rowRect.Max.x - switchW - 16.0f * d;
            const float labelPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * d;
            ImFont* labelFont = ImGuiM3FontBold();
            ImGui::PushFont(labelFont, labelPx);
            const char* shownLabel = UI::TruncateLabel(label, ImMax(1.0f, textRight - rowMin.x - 4.0f * d));
            const ImVec2 labelSize = ImGui::CalcTextSize(shownLabel);
            ImGui::PopFont();
            const bool hasSupport = support && support[0];
            const float supportPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodySmall).size * d;
            ImFont* supportFont = ImGuiM3FontRegular();
            float supportH = 0.0f;
            if (hasSupport)
            {
                ImGui::PushFont(supportFont, supportPx);
                supportH = ImGui::GetTextLineHeight();
                ImGui::PopFont();
            }
            const float textH = labelSize.y + (hasSupport ? supportH + 2.0f * d : 0.0f);
            const float textY = rowMin.y + (rowH - textH) * 0.5f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddText(labelFont, labelPx, ImVec2(rowMin.x + 4.0f * d, textY),
                        ImGuiM3ColorU32(ImGuiM3Role_OnSurface), shownLabel);
            if (hasSupport)
            {
                ImGui::PushFont(supportFont, supportPx);
                const char* shownSupport = UI::TruncateLabel(
                    support, ImMax(1.0f, textRight - rowMin.x - 8.0f * d));
                dl->AddText(supportFont, supportPx,
                            ImVec2(rowMin.x + 4.0f * d, textY + labelSize.y + 2.0f * d),
                            ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant), shownSupport);
                ImGui::PopFont();
            }

            ImGui::SetCursorScreenPos(ImVec2(rowRect.Max.x - switchW - 12.0f * d,
                                             rowMin.y + (rowH - switchH) * 0.5f));
            const std::string switchId = std::string(id) + "_switch";
            const bool switchChanged = ImGui::M3SwitchWithID("", switchId.c_str(), value);
            if (rowHovered || ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", hasSupport ? support : label);
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowRect.Max.y));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            return rowClicked || switchChanged;
        }
    }

    void ImGuiOverlay::renderSettingsView(const KeyboardState& keyboard)
    {
        auto saveSettings = [&]() {
            settingsManager.save();
            settingsSaved = true;
        };

        const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
        const float d = m.density;
        ImGui::BeginChild("SettingsContent", ImVec2(0, 0), false);

        ImGui::M3HeroHeader(Icon::SettingsUtf8, "Settings",
                            "Controls, effect behaviour and appearance for this overlay.");
        ImGui::Spacing();

        auto renderKeyBind = [&](const char* label, const char* tooltip,
                                 const std::string& currentKey,
                                 std::function<void(const std::string&)> setter,
                                 int bindingId) {
            ImGui::PushID(bindingId);
            const float rowH = 56.0f * d;
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const float rowW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
            const float buttonW = 148.0f * d;
            const float leftW = ImMax(1.0f, rowW - buttonW - 16.0f * d);
            const bool rowClicked = ImGui::InvisibleButton(label, ImVec2(leftW, rowH));
            if (rowClicked)
                listeningForKey = listeningForKey == bindingId ? 0 : bindingId;
            const bool rowHovered = ImGui::IsItemHovered();
            const ImRect rowRect(rowMin, ImVec2(rowMin.x + rowW, rowMin.y + rowH));
            const float radius = ImGuiM3Radius(ImGuiM3Shape_Large);
            if (rowHovered)
                ImGuiM3DrawStateLayer(ImGui::GetWindowDrawList(), rowRect,
                                      ImGuiM3ShapeRounding{ radius, radius, radius, radius },
                                      ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);

            const float buttonH = m.button_height_default * d;
            const float textW = ImMax(1.0f, rowW - buttonW - 28.0f * d);
            const float labelPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * d;
            ImFont* labelFont = ImGuiM3FontBold();
            ImGui::PushFont(labelFont, labelPx);
            const char* shownLabel = UI::TruncateLabel(label, textW);
            const ImVec2 labelSize = ImGui::CalcTextSize(shownLabel);
            ImGui::PopFont();
            ImGui::GetWindowDrawList()->AddText(
                labelFont, labelPx,
                ImVec2(rowMin.x + 4.0f * d, rowMin.y + (rowH - labelSize.y) * 0.5f),
                ImGuiM3ColorU32(ImGuiM3Role_OnSurface), shownLabel);

            const bool isListening = listeningForKey == bindingId;
            const std::string buttonText = isListening ? "Press a key..." : currentKey;
            ImGui::SetCursorScreenPos(ImVec2(rowRect.Max.x - buttonW - 4.0f * d,
                                             rowMin.y + (rowH - buttonH) * 0.5f));
            if (ImGui::M3Button(buttonText.c_str(), isListening ? ImGuiM3Button_Filled : ImGuiM3Button_Tonal,
                                ImVec2(buttonW, buttonH)))
                listeningForKey = isListening ? 0 : bindingId;
            if (rowHovered)
                ImGui::SetTooltip("%s", tooltip);
            if (isListening && !keyboard.lastKeyName.empty())
            {
                setter(keyboard.lastKeyName);
                listeningForKey = 0;
                saveSettings();
            }
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowRect.Max.y));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::PopID();
        };

        ImGui::M3CardBegin("set_controls", "Controls", NULL,
                           "Keybinds work while the overlay is open or hidden.");
        renderKeyBind("Toggle Effects", "Enable or disable every effect in the active preset.",
                      settingsManager.getToggleKey(),
                      [](const std::string& key) { settingsManager.setToggleKey(key); }, 1);
        renderKeyBind("Reload Config", "Reload the active configuration from disk.",
                      settingsManager.getReloadKey(),
                      [](const std::string& key) { settingsManager.setReloadKey(key); }, 2);
        renderKeyBind("Toggle Overlay", "Show or hide this overlay.",
                      settingsManager.getOverlayKey(),
                      [](const std::string& key) { settingsManager.setOverlayKey(key); }, 3);
        bool blockInput = settingsManager.getOverlayBlockInput();
        if (settingSwitchRow("##block_input", "Block input while open",
                             "Keep keyboard and mouse input inside the overlay.", &blockInput))
        {
            settingsManager.setOverlayBlockInput(blockInput);
            saveSettings();
        }
        bool usePortal = settingsManager.getUsePortalIntegration();
        if (settingSwitchRow("##portal_integration", "Use desktop portal",
                             "Use the XDG portal for file and folder pickers when available.", &usePortal))
        {
            settingsManager.setUsePortalIntegration(usePortal);
            saveSettings();
        }
        ImGui::M3CardEnd();

        ImGui::Spacing();
        ImGui::M3CardBegin("set_effects", "Effects", NULL,
                           "Choose how the active preset starts and how edits reach the layer.");
        bool enableOnLaunch = settingsManager.getEnableOnLaunch();
        if (settingSwitchRow("##enable_on_launch", "Enable effects on launch",
                             "Start the preset with its effect chain active.", &enableOnLaunch))
        {
            settingsManager.setEnableOnLaunch(enableOnLaunch);
            saveSettings();
        }
        bool fxOverUi = settingsManager.getFxOverUi();
        if (settingSwitchRow("##fx_over_ui", "FX-Over-UI",
                             "Draw effects before the game HUD. Applied on the next launch.", &fxOverUi))
        {
            settingsManager.setFxOverUi(fxOverUi);
            saveSettings();
            pushToastOnce(LogLevel::Info, "FX-Over-UI toggled. Applies on next launch.");
        }
        bool autoApply = settingsManager.getAutoApply();
        if (settingSwitchRow("##auto_apply", "Auto-apply changes", "", &autoApply))
        {
            settingsManager.setAutoApply(autoApply);
            saveSettings();
        }
        if (autoApply)
        {
            const float rowH = 64.0f * d;
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const float rowW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
            const float sliderW = 180.0f * d;
            const float leftW = ImMax(1.0f, rowW - sliderW - 16.0f * d);
            ImGui::InvisibleButton("##auto_apply_delay_row", ImVec2(leftW, rowH));
            const ImRect rowRect(rowMin, ImVec2(rowMin.x + rowW, rowMin.y + rowH));
            const float labelPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * d;
            ImFont* labelFont = ImGuiM3FontBold();
            ImGui::PushFont(labelFont, labelPx);
            const ImVec2 labelSize = ImGui::CalcTextSize("Apply delay");
            ImGui::PopFont();
            ImGui::GetWindowDrawList()->AddText(
                labelFont, labelPx, ImVec2(rowMin.x + 4.0f * d, rowMin.y + (rowH - labelSize.y) * 0.5f),
                ImGuiM3ColorU32(ImGuiM3Role_OnSurface), "Apply delay");
            ImGui::SetCursorScreenPos(ImVec2(rowRect.Max.x - sliderW - 4.0f * d,
                                             rowMin.y + (rowH - ImGui::GetFrameHeight()) * 0.5f));
            ImGui::SetNextItemWidth(sliderW);
            int delayVal = settingsManager.getAutoApplyDelay();
            if (ImGui::SliderInt("##autoApplyDelay", &delayVal, 20, 1000, "%d ms"))
                settingsManager.setAutoApplyDelay(delayVal);
            if (ImGui::IsItemDeactivatedAfterEdit())
                saveSettings();
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowRect.Max.y));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
        }
        ImGui::M3CardEnd();

        ImGui::Spacing();
        ImGui::M3CardBegin("set_performance", "Performance", NULL,
                           "Tweak the FPS counter and maximum effect count.");
        bool showFps = settingsManager.getShowFps();
        if (settingSwitchRow("##show_fps", "Show FPS counter",
                             "Show your game's frame counter.", &showFps))
        {
            settingsManager.setShowFps(showFps);
            saveSettings();
        }
        ImGui::BeginDisabled(!showFps);
        bool fpsExtended = settingsManager.getFpsExtended();
        if (settingSwitchRow("##fps_extended", "Extended frame info",
                             "Include 1% and 0.1% lows, plus real and generated frame rates.", &fpsExtended))
        {
            settingsManager.setFpsExtended(fpsExtended);
            saveSettings();
        }
        if (showFps)
        {
            ImGui::Spacing();
            UI::PushSupport();
            ImGui::TextDisabled("FPS counter position");
            UI::PopSupport();
            const char* corners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
            int fpsCorner = settingsManager.getFpsCorner();
            if (ImGui::M3ConnectedButtonGroup("##fps_corner", corners, 4, &fpsCorner, NULL))
            {
                settingsManager.setFpsCorner(fpsCorner);
                saveSettings();
            }
            ImGui::Spacing();
        }
        ImGui::EndDisabled();

        {
            const float rowH = 72.0f * d;
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const float rowW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
            const float inputW = 104.0f * d;
            const float leftW = ImMax(1.0f, rowW - inputW - 16.0f * d);
            ImGui::InvisibleButton("##max_effects_row", ImVec2(leftW, rowH));
            const ImRect rowRect(rowMin, ImVec2(rowMin.x + rowW, rowMin.y + rowH));
            const float labelPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * d;
            ImFont* labelFont = ImGuiM3FontBold();
            ImGui::PushFont(labelFont, labelPx);
            const ImVec2 labelSize = ImGui::CalcTextSize("Max effects");
            ImGui::PopFont();
            ImGui::GetWindowDrawList()->AddText(
                labelFont, labelPx, ImVec2(rowMin.x + 4.0f * d, rowMin.y + 10.0f * d),
                ImGuiM3ColorU32(ImGuiM3Role_OnSurface), "Max effects");
            UI::PushSupport();
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x + 4.0f * d, rowMin.y + 30.0f * d));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                                   ImMax(24.0f * d, rowW - inputW - 24.0f * d));
            int maxEffectsVal = settingsManager.getMaxEffects();
            const float bytesPerSlot = 2.0f * currentWidth * currentHeight * 4.0f;
            const int estimatedVramMB = static_cast<int>((maxEffectsVal * bytesPerSlot) / (1024.0f * 1024.0f));
            if (maxEffectsVal > 20)
                ImGui::TextColored(UI::Attention(), "~%d MB at %ux%u. High values use significant VRAM.",
                                   estimatedVramMB, currentWidth, currentHeight);
            else
                ImGui::TextDisabled("~%d MB at %ux%u.",
                                    estimatedVramMB, currentWidth, currentHeight);
            ImGui::PopTextWrapPos();
            UI::PopSupport();
            ImGui::SetCursorScreenPos(ImVec2(rowRect.Max.x - inputW - 4.0f * d,
                                             rowMin.y + (rowH - ImGui::GetFrameHeight()) * 0.5f));
            ImGui::SetNextItemWidth(inputW);
            if (ImGui::InputInt("##maxEffects", &maxEffectsVal, 0, 0))
            {
                maxEffectsVal = std::clamp(maxEffectsVal, 1, 200);
                settingsManager.setMaxEffects(maxEffectsVal);
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                saveSettings();
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowRect.Max.y));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            (void)labelSize;
        }
        ImGui::M3CardEnd();

        ImGui::Spacing();
        ImGui::M3CardBegin("set_appearance", "Appearance", NULL,
                           "Customize VKIntox's appearance to your liking.");
        static const char* const themePills[] = {"Colors", "Blur", "Shape and metrics", "Navigation bar"};
        static const char* const themeGlyphs[] = {
            Icon::ColorizeUtf8, Icon::BlurOnUtf8, Icon::StraightenUtf8, Icon::GridViewUtf8};
        ImGui::PushID("theme_pages_primary");
        const int firstPill = UI::PillPageRow(themePills, themeGlyphs, 2);
        ImGui::PopID();
        ImGui::PushID("theme_pages_secondary");
        const int secondPill = UI::PillPageRow(themePills + 2, themeGlyphs + 2, 2);
        ImGui::PopID();
        const int pill = firstPill >= 0 ? firstPill : (secondPill >= 0 ? secondPill + 2 : -1);
        if (pill == 0)
            openSubView = SubView_ThemeColors;
        else if (pill == 1)
            openSubView = SubView_ThemeBlur;
        else if (pill == 2)
            openSubView = SubView_ThemeShape;
        else if (pill == 3)
            openSubView = SubView_Navbar;

        ImGui::Spacing();
        bool separateParams = settingsManager.getSeparateEffectParams();
        if (settingSwitchRow("##separate_params", "Separate effect parameters",
                             "Open parameters in a tabbed sheet below the effect list.", &separateParams))
        {
            settingsManager.setSeparateEffectParams(separateParams);
            saveSettings();
        }
        ImGui::Spacing();
        ImGui::M3SectionHeader("Theme file", Icon::SaveUtf8);
        ImGui::M3ThemeEditorFile();
        ImGui::M3CardEnd();

        ImGui::Spacing();
        ImGui::M3CardBegin("set_depth", "Depth buffer", Icon::LayersUtf8,
                           "Capture, selection and decoding controls for depth-based effects.");
        if (UI::SubViewRow("Open depth settings", "Capture, pin, decode", Icon::StraightenUtf8))
            openSubView = SubView_DepthSettings;
        ImGui::M3CardEnd();

        ImGui::Spacing();
        ImGui::M3CardBegin("set_dev", "Developer", NULL);
        if (UI::SubViewRow("Open diagnostics", NULL, Icon::MonitorHeartUtf8))
            openSubView = SubView_Diagnostics;
        ImGui::M3CardEnd();

        ImGui::EndChild();
    }

    // --- Pushed theme-editor pages -------------------------------------------
    void ImGuiOverlay::renderThemeColorsView()
    {
        if (UI::BreadcrumbBar("Go back", Icon::ChevronLeftUtf8))
        {
            openSubView = SubView_None;
            return;
        }
        ImGui::Spacing();
        ImGui::M3HeroHeader(Icon::ColorizeUtf8, "Colors");
        ImGui::Spacing();
        ImGui::M3ThemeEditorColors();
    }

    void ImGuiOverlay::renderThemeBlurView()
    {
        if (UI::BreadcrumbBar("Go back", Icon::ChevronLeftUtf8))
        {
            openSubView = SubView_None;
            return;
        }
        ImGui::Spacing();
        ImGui::M3HeroHeader(Icon::BlurOnUtf8, "Blur");
        ImGui::Spacing();
        ImGui::M3ThemeEditorBlur();
    }

    void ImGuiOverlay::renderNavbarView()
    {
        if (UI::BreadcrumbBar("Go back", Icon::ChevronLeftUtf8))
        {
            openSubView = SubView_None;
            return;
        }
        ImGui::Spacing();
        ImGui::M3HeroHeader(Icon::GridViewUtf8, "Navigation bar");
        ImGui::Spacing();

        ImGui::M3CardBegin("nav_shape", "Layout", NULL,
                           "Choose where the navigation dock sits and how its buttons group.");
        UI::PushSupport();
        ImGui::TextDisabled("Position");
        UI::PopSupport();
        {
            const char* positions[] = {"Top", "Bottom"};
            int position = settingsManager.getNavPosition();
            if (ImGui::M3ConnectedButtonGroup("##nav_position", positions, 2, &position, NULL))
            {
                settingsManager.setNavPosition(position);
                settingsManager.save();
            }
        }
        ImGui::Spacing();
        UI::PushSupport();
        ImGui::TextDisabled("Style");
        UI::PopSupport();
        {
            const char* styles[] = {"Dock", "Pill-shaped group of buttons"};
            int style = settingsManager.getNavStyle();
            if (ImGui::M3ConnectedButtonGroup("##nav_style", styles, 2, &style, NULL))
            {
                settingsManager.setNavStyle(style);
                settingsManager.save();
            }
        }
        ImGui::M3CardEnd();
    }

    void ImGuiOverlay::renderThemeShapeView()
    {
        if (UI::BreadcrumbBar("Go back", Icon::ChevronLeftUtf8))
        {
            openSubView = SubView_None;
            return;
        }
        ImGui::Spacing();
        ImGui::M3HeroHeader(Icon::StraightenUtf8, "Shape and metrics");
        ImGui::Spacing();
        ImGui::M3ThemeEditorShape();
    }

} // namespace VKIntox
