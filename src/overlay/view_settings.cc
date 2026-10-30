#include "imgui_overlay.hh"
#include "settings_manager.hh"
#include "logger.hh"
#include "overlay/ui_theme.hh"
#include "overlay/ui_icons.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>

#include "vendor/imgui/imgui.h"

namespace VKIntox
{
    void ImGuiOverlay::renderSettingsView(const KeyboardState& keyboard)
    {
        // Helper to save settings to file
        auto saveSettings = [&]() {
            settingsManager.save();
            settingsSaved = true;
        };

        ImGui::BeginChild("SettingsContent", ImVec2(0, 0), false);

        // Helper lambda to render a keybind button.
        auto renderKeyBind = [&](const char* label, const char* tooltip,
                                 const std::string& currentKey,
                                 std::function<void(const std::string&)> setter,
                                 int bindingId) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tooltip);
            ImGui::SameLine(160.0f);

            bool isListening = (listeningForKey == bindingId);
            const char* buttonText = isListening ? "Press a key..." : currentKey.c_str();

            if (isListening)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGuiM3ColorU32(ImGuiM3Role_TertiaryContainer));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGuiM3ColorU32(ImGuiM3Role_OnTertiaryContainer));
            }

            if (ImGui::Button(buttonText, ImVec2(140, 0)))
                listeningForKey = isListening ? 0 : bindingId;

            if (isListening)
                ImGui::PopStyleColor(2);

            if (isListening && !keyboard.lastKeyName.empty())
            {
                setter(keyboard.lastKeyName);
                listeningForKey = 0;
                saveSettings();
            }
        };

        // --- Controls ---
        ImGui::M3CardBegin("set_controls", "Controls", Icon::TuneUtf8);
        ImGui::Spacing();
        renderKeyBind("Toggle Effects", "Key to enable/disable all effects",
                      settingsManager.getToggleKey(),
                      [](const std::string& key) { settingsManager.setToggleKey(key); }, 1);
        renderKeyBind("Reload Config", "Key to reload the configuration file",
                      settingsManager.getReloadKey(),
                      [](const std::string& key) { settingsManager.setReloadKey(key); }, 2);
        renderKeyBind("Toggle Overlay", "Key to show/hide this overlay",
                      settingsManager.getOverlayKey(),
                      [](const std::string& key) { settingsManager.setOverlayKey(key); }, 3);

        ImGui::Spacing();
        bool blockInput = settingsManager.getOverlayBlockInput();
        if (ImGui::Checkbox("Block input while the overlay is open", &blockInput))
        {
            settingsManager.setOverlayBlockInput(blockInput);
            saveSettings();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("When enabled, keyboard and mouse input is captured by the overlay.");
            ImGui::EndTooltip();
        }
        ImGui::M3CardEnd();

        // --- Behaviour ---
        ImGui::Spacing();
        ImGui::M3CardBegin("set_behaviour", "Behaviour", Icon::PowerUtf8);

        bool enableOnLaunch = settingsManager.getEnableOnLaunch();
        if (ImGui::Checkbox("Enable effects on launch", &enableOnLaunch))
        {
            settingsManager.setEnableOnLaunch(enableOnLaunch);
            saveSettings();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("If enabled, effects are active when the game starts.");

        bool autoApply = settingsManager.getAutoApply();
        if (ImGui::Checkbox("Auto-apply changes", &autoApply))
        {
            settingsManager.setAutoApply(autoApply);
            saveSettings();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Automatically apply parameter changes after a short delay.");

        if (autoApply)
        {
            ImGui::Indent();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Delay");
            ImGui::SameLine(160.0f);
            ImGui::SetNextItemWidth(160);
            int delayVal = settingsManager.getAutoApplyDelay();
            if (ImGui::SliderInt("##autoApplyDelay", &delayVal, 20, 1000, "%d ms"))
                settingsManager.setAutoApplyDelay(delayVal);
            if (ImGui::IsItemDeactivatedAfterEdit())
                saveSettings();
            ImGui::Unindent();
        }

        bool usePortal = settingsManager.getUsePortalIntegration();
        if (ImGui::Checkbox("Use XDG Desktop Portal integrations when available", &usePortal))
        {
            settingsManager.setUsePortalIntegration(usePortal);
            saveSettings();
        }
        ImGui::Spacing();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s  Max effects", Icon::MemoryUtf8);
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("Maximum number of effects that can be active simultaneously.");
            ImGui::TextColored(UI::Warning(), "High values use significant VRAM.");
            ImGui::EndTooltip();
        }
        ImGui::SameLine(160.0f);
        ImGui::SetNextItemWidth(120);
        int maxEffectsVal = settingsManager.getMaxEffects();
        // step 0 hides InputInt's inline +/- buttons: the M3 frame padding makes
        // them huge, which squeezed the value field down to an unreadable sliver.
        if (ImGui::InputInt("##maxEffects", &maxEffectsVal, 0, 0))
        {
            maxEffectsVal = std::clamp(maxEffectsVal, 1, 200);
            settingsManager.setMaxEffects(maxEffectsVal);
            saveSettings();
        }
        float bytesPerSlot = 2.0f * currentWidth * currentHeight * 4.0f;
        int estimatedVramMB = static_cast<int>((maxEffectsVal * bytesPerSlot) / (1024.0f * 1024.0f));
        ImGui::SameLine();
        UI::PushSupport();
        if (maxEffectsVal > 20)
            ImGui::TextColored(UI::Attention(), "~%d MB @ %ux%u", estimatedVramMB, currentWidth, currentHeight);
        else
            ImGui::TextDisabled("~%d MB @ %ux%u", estimatedVramMB, currentWidth, currentHeight);
        ImGui::TextDisabled("Requires restart");
        UI::PopSupport();
        ImGui::M3CardEnd();

        // --- Appearance: theme entry points first, then the layout option and
        // the theme file, each on its own surface ---
        ImGui::Spacing();
        ImGui::M3CardBegin("set_appearance", "Appearance", Icon::PaletteUtf8);

        // the three theme pages, as one connected pill row
        {
            static const char* const themePills[] = { "Colors", "Blur", "Shape and metrics", "Navigation bar" };
            static const char* const themeGlyphs[] = { Icon::ColorizeUtf8, Icon::BlurOnUtf8, Icon::StraightenUtf8, Icon::GridViewUtf8 };
            const int pill = UI::PillPageRow(themePills, themeGlyphs, 4);
            if (pill == 0)
                openSubView = SubView_ThemeColors;
            else if (pill == 1)
                openSubView = SubView_ThemeBlur;
            else if (pill == 2)
                openSubView = SubView_ThemeShape;
            else if (pill == 3)
                openSubView = SubView_Navbar;
        }

        ImGui::Spacing();
        bool separateParams = settingsManager.getSeparateEffectParams();
        if (ImGui::Checkbox("Separate effect parameters into its own card", &separateParams))
        {
            settingsManager.setSeparateEffectParams(separateParams);
            saveSettings();
        }

        // Theme file: always revealed, so a reload is one tap from here
        ImGui::Spacing();
        ImGui::M3SectionHeader("Theme file", Icon::SaveUtf8);
        ImGui::M3ThemeEditorFile();
        ImGui::M3CardEnd();

        // --- Depth buffer: capture, decoding and selection all live here ---
        ImGui::Spacing();
        ImGui::M3CardBegin("set_depth");
        renderAdvancedView();
        ImGui::M3CardEnd();

        // --- Developer: pages that live a level deeper, plus layout recovery ---
        ImGui::Spacing();
        ImGui::M3CardBegin("set_dev", "Developer", Icon::CodeUtf8);
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

        ImGui::M3CardBegin("nav_shape", "Layout", Icon::GridViewUtf8);
        ImGui::TextDisabled("Position");
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
        ImGui::TextDisabled("Style");
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
