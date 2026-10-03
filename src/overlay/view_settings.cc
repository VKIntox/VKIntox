#include "imgui_overlay.hh"
#include "settings_manager.hh"
#include "logger.hh"
#include "overlay/ui_theme.hh"
#include "overlay/ui_icons.hh"

#include <algorithm>
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

        ImGui::TextDisabled("Controls, behaviour, debugging and theming.");
        ImGui::Spacing();

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
        ImGui::TextDisabled("Click a binding, then press a key.");
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

        bool depthCapture = settingsManager.getDepthCapture();
        if (ImGui::Checkbox("Depth capture (requires restart)", &depthCapture))
        {
            settingsManager.setDepthCapture(depthCapture);
            saveSettings();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Enable depth buffer capture for effects that use depth.\nMay impact performance.");

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
        if (ImGui::InputInt("##maxEffects", &maxEffectsVal))
        {
            maxEffectsVal = std::clamp(maxEffectsVal, 1, 200);
            settingsManager.setMaxEffects(maxEffectsVal);
            saveSettings();
        }
        float bytesPerSlot = 2.0f * currentWidth * currentHeight * 4.0f;
        int estimatedVramMB = static_cast<int>((maxEffectsVal * bytesPerSlot) / (1024.0f * 1024.0f));
        ImGui::SameLine();
        if (maxEffectsVal > 20)
            ImGui::TextColored(UI::Attention(), "~%d MB @ %ux%u", estimatedVramMB, currentWidth, currentHeight);
        else
            ImGui::TextDisabled("~%d MB @ %ux%u", estimatedVramMB, currentWidth, currentHeight);
        ImGui::TextDisabled("Requires restart");
        ImGui::M3CardEnd();

        // --- Developer ---
        ImGui::Spacing();
        ImGui::M3CardBegin("set_dev", "Developer", Icon::BugReportUtf8);
        bool showDebugWindow = settingsManager.getShowDebugWindow();
        if (ImGui::Checkbox("Show debug window", &showDebugWindow))
        {
            settingsManager.setShowDebugWindow(showDebugWindow);
            saveSettings();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Effect registry data and log output.");

        const std::string resetLayoutLabel = std::string(Icon::StraightenUtf8) + "  Reset window position";
        if (ImGui::Button(resetLayoutLabel.c_str()))
        {
            resetLayoutRequested = true;
            Logger::info("Window layout reset to default");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reset the overlay window position and size to defaults.");
        ImGui::M3CardEnd();

        // --- Appearance ---
        ImGui::Spacing();
        ImGui::M3CardBegin("set_appearance", "Appearance", Icon::PaletteUtf8);
        const std::string themeEditorLabel = std::string(themeEditorOpen ? Icon::ExpandMoreUtf8 : Icon::ChevronRightUtf8) + "  Theme editor";
        if (ImGui::Button(themeEditorLabel.c_str()))
            themeEditorOpen = !themeEditorOpen;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Source colour, variant, contrast, and every colour token.");

        if (themeEditorOpen)
        {
            ImGui::Spacing();
            ImGui::BeginChild("ThemeEditor", ImVec2(0, 440), ImGuiChildFlags_None, ImGuiWindowFlags_None);
            ImGui::M3ThemeEditor();
            ImGui::EndChild();
        }
        ImGui::M3CardEnd();

        ImGui::EndChild();
    }

} // namespace VKIntox
