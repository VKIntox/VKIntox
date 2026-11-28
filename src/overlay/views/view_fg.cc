#include "overlay/shell/imgui_overlay.hh"
#include "config/settings_manager.hh"
#include "framegen/present.hh"
#include "util/logger.hh"
#include "overlay/theme/ui_theme.hh"
#include "overlay/theme/ui_icons.hh"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "vendor/imgui/imgui.h"

namespace VKIntox
{
    namespace
    {
        int FlowScaleIndex(float scale)
        {
            if (scale <= 0.375f)
                return 2;
            if (scale <= 0.75f)
                return 1;
            return 0;
        }

        float FlowScaleValue(int index)
        {
            static constexpr float values[] = {1.0f, 0.5f, 0.25f};
            return values[std::clamp(index, 0, 2)];
        }
    }

    void ImGuiOverlay::renderFgView()
    {
        auto saveSettings = [&]() {
            settingsManager.save();
            settingsSaved = true;
        };

        ImGui::BeginChild("FgContent", ImVec2(0, 0), false,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);

        const bool available = FrameGen::engineAvailable();
        bool enabled = settingsManager.getFgEnabled();

        ImGui::M3CardBegin("fg_hero");
        {
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            const float d = m.density;
            const float shape = 96.0f * d;
            const float switchScale = 1.15f;
            const float switchW = m.switch_track_width * d * switchScale;
            const float rowH = std::max(shape + 20.0f * d, m.list_item_height_1 * d * switchScale);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float rowW = std::max(ImGui::GetContentRegionAvail().x, shape + switchW + 24.0f * d);
            const ImRect row(pos, ImVec2(pos.x + rowW, pos.y + rowH));
            ImGui::Dummy(row.GetSize());

            const ImGuiID shapeId = ImGui::GetID("##fg_shape");
            ImGuiStorage* storage = ImGui::GetStateStorage();
            const float target = enabled ? 1.0f : 0.0f;
            float progress = storage->GetFloat(shapeId, target);
            const float duration = target > progress ? 0.35f : 0.22f;
            progress += ImClamp(target - progress, -ImGui::GetIO().DeltaTime / duration,
                                ImGui::GetIO().DeltaTime / duration);
            storage->SetFloat(shapeId, progress);
            const float morph = target > 0.5f ? 1.0f - ImPow(1.0f - progress, 3.0f) : progress;
            const ImRect shapeRect(pos, ImVec2(pos.x + shape, pos.y + shape));
            ImGuiM3DrawShapeMorph(ImGui::GetWindowDrawList(), shapeRect,
                                  ImGuiM3MorphShape_Circle, ImGuiM3MorphShape_Cookie,
                                  morph, ImGuiM3ColorU32(ImGuiM3Role_PrimaryContainer));
            ImGuiM3DrawIcon(ImGui::GetWindowDrawList(), Icon::MaskedTransitionsUtf8,
                            shapeRect, 48.0f * d, ImGuiM3ColorU32(ImGuiM3Role_OnPrimaryContainer));

            const float textX = pos.x + shape + 20.0f * d;
            const float titlePx = ImGuiM3GetTypeStyle(ImGuiM3Type_TitleLarge).size * d;
            ImFont* titleFont = ImGuiM3FontBold();
            const ImVec2 titleSize = titleFont->CalcTextSizeA(titlePx, FLT_MAX, 0.0f, "Generate frames");
            ImGui::GetWindowDrawList()->AddText(titleFont, titlePx,
                ImVec2(textX, row.Min.y + 12.0f * d), ImGuiM3ColorU32(ImGuiM3Role_OnSurface),
                "Generate frames");
            ImGui::SetCursorScreenPos(ImVec2(textX, row.Min.y + 12.0f * d + titleSize.y + 4.0f * d));
            ImGui::PushTextWrapPos(row.Max.x - switchW - 20.0f * d);
            UI::TextSupport("Generate frames using MAKO. This forces FIFO while on.");
            ImGui::PopTextWrapPos();
            ImGui::SetCursorScreenPos(ImVec2(row.Max.x - switchW, row.GetCenter().y - rowH * 0.5f));
            if (!available)
                ImGui::BeginDisabled();
            const bool toggled = ImGui::M3SwitchScaled("", "##fg_enabled", &enabled, switchScale);
            if (!available)
                ImGui::EndDisabled();
            if (toggled)
            {
                settingsManager.setFgEnabled(enabled);
                saveSettings();
            }
            if (!available && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", FrameGen::engineStatus().c_str());
            ImGui::SetCursorScreenPos(ImVec2(pos.x, row.Max.y));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
        }

        if (enabled && available)
        {
            ImGui::Spacing();
            {
                const float d = ImGuiM3GetMetrics().density;
                int multiplierIndex = settingsManager.getFgMultiplier() - 2;
                const char* const multiplierLabels[] = {"x2", "x3", "x4"};
                ImGui::TextUnformatted("Multiplier");
                ImGui::SameLine(160.0f * d);
                if (ImGui::M3ConnectedButtonGroup("##fg_multiplier", multiplierLabels, 3,
                                                  &multiplierIndex, nullptr, false, nullptr))
                {
                    settingsManager.setFgMultiplier(multiplierIndex + 2);
                    saveSettings();
                }

                int flowIndex = FlowScaleIndex(settingsManager.getFgFlowScale());
                const char* const flowLabels[] = {"Full", "Half", "Quarter"};
                ImGui::TextUnformatted("Flow scale");
                ImGui::SameLine(160.0f * d);
                if (ImGui::M3ConnectedButtonGroup("##fg_flowscale", flowLabels, 3,
                                                  &flowIndex, nullptr, false, nullptr))
                {
                    settingsManager.setFgFlowScale(FlowScaleValue(flowIndex));
                    saveSettings();
                }

                bool performanceMode = settingsManager.getFgPerformanceMode();
                if (ImGui::M3Switch("Performance mode##fg_perf", &performanceMode))
                {
                    settingsManager.setFgPerformanceMode(performanceMode);
                    saveSettings();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Slightly lowers quality for better performance.");

                bool halfPrecision = settingsManager.getFgHalfPrecision();
                if (ImGui::M3Switch("Half precision##fg_half", &halfPrecision))
                {
                    settingsManager.setFgHalfPrecision(halfPrecision);
                    saveSettings();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Quantizes the frame generation model.");

                bool fxOverUi = settingsManager.getFxOverUi();
                if (ImGui::M3Switch("FX-Over-UI for framegen##fg_overui", &fxOverUi))
                {
                    settingsManager.setFxOverUi(fxOverUi);
                    saveSettings();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Occlude the HUD for frame generation.");
            }
        }
        ImGui::M3CardEnd();

        ImGui::EndChild();
    }
} // namespace VKIntox
