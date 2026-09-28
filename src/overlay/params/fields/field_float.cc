#include "../field_editor.hh"
#include "../../../vendor/imgui/imgui.h"
#include "../../../vendor/imgui/imgui_internal.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>

namespace VKIntox
{
    // Robust float field editor with custom value input via right-click
    // Uses deferred modal opening to avoid ImGui nested-popup issues
    class FloatFieldEditor : public FieldEditor
    {
    public:
        bool render(EffectParam& param) override
        {
            auto& p = static_cast<FloatParam&>(param);
            bool changed = false;

            // Generate stable unique IDs using parameter address (avoids label collisions)
            const void* paramAddr = static_cast<const void*>(&param);
            char contextId[64];
            char modalId[64];
            char inputId[64];
            snprintf(contextId, sizeof(contextId), "##flctx_%p", paramAddr);
            snprintf(modalId, sizeof(modalId), "##flmodal_%p", paramAddr);
            snprintf(inputId, sizeof(inputId), "##flinput_%p", paramAddr);

            // === Render the slider ===
            if (ImGui::SliderFloat(p.label.c_str(), &p.value, p.minValue, p.maxValue))
            {
                const float step = p.step > 0.0f ? p.step : 0.01f;
                p.value = std::round(p.value / step) * step;
                changed = true;
            }

            // === Right-click context menu ===
            if (ImGui::BeginPopupContextItem(contextId))
            {
                if (ImGui::MenuItem("Enter Custom Value..."))
                {
                    // Mark modal as pending for THIS parameter, initialize buffer
                    m_activeModalParam = paramAddr;
                    snprintf(m_customValueBuf, sizeof(m_customValueBuf), "%.6g", p.value);
                    // Don't call OpenPopup here - defer to after context menu closes
                    m_popupPosition = ImGui::GetMousePos();
                    m_focusCustomValueInput = true;
                    m_modalOpenRequested = true;
                }
                if (ImGui::MenuItem("Reset to default"))
                {
                    resetToDefault(param);
                    changed = true;
                }
                ImGui::EndPopup();
            }

            bool popupOpenedThisFrame = false;
            // === Deferred modal opening (must happen outside any popup context) ===
            if (m_modalOpenRequested && m_activeModalParam == paramAddr)
            {
                m_modalOpenRequested = false;
                const ImGuiViewport* viewport = ImGui::GetMainViewport();
                const ImVec2 popupSize = ImVec2(320.0f, 190.0f);
                ImGui::SetNextWindowPos(customValuePopupPosition(
                    m_popupPosition, viewport->Pos, viewport->Size, popupSize), ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(popupSize, ImGuiCond_Appearing);
                ImGui::OpenPopup(modalId);
                popupOpenedThisFrame = true;
            }

            // === Render custom value popup for THIS parameter only ===
            if (m_activeModalParam == paramAddr && 
                !popupOpenedThisFrame && ImGui::BeginPopup(modalId))
            {
                ImGui::Text("Custom value: %s", p.label.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("Current range: [%.4g, %.4g]", p.minValue, p.maxValue);
                ImGui::TextDisabled("(values outside range are allowed)");
                ImGui::Spacing();
                
                ImGui::SetNextItemWidth(220.0f);
                if (m_focusCustomValueInput)
                {
                    ImGui::SetKeyboardFocusHere();
                    m_focusCustomValueInput = false;
                }
                bool submit = ImGui::InputText(inputId, m_customValueBuf, 
                    sizeof(m_customValueBuf), ImGuiInputTextFlags_EnterReturnsTrue);

                ImGui::Spacing();
                bool ok = ImGui::Button("OK", ImVec2(100, 0));
                ImGui::SameLine();
                bool cancel = ImGui::Button("Cancel", ImVec2(100, 0));

                // Handle submission
                if (submit || ok)
                {
                    float newVal;
                    if (sscanf(m_customValueBuf, "%f", &newVal) == 1)
                    {
                        p.value = newVal;
                        changed = true;
                    }
                    ImGui::CloseCurrentPopup();
                    m_activeModalParam = nullptr;  // Clear active state
                }

                // Handle cancellation
                if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    ImGui::CloseCurrentPopup();
                    m_activeModalParam = nullptr;  // Clear active state
                }
                
                ImGui::EndPopup();
            }

            return changed;
        }

        void resetToDefault(EffectParam& param) override
        {
            param.resetToDefault();
        }

    private:
        static constexpr size_t BUF_SIZE = 128;
        char m_customValueBuf[BUF_SIZE] = "";
        const void* m_activeModalParam = nullptr;  // Which param has modal open
        bool m_modalOpenRequested = false;
        bool m_focusCustomValueInput = false;
        ImVec2 m_popupPosition = ImVec2(0.0f, 0.0f);
    };

    REGISTER_FIELD_EDITOR(ParamType::Float, FloatFieldEditor)

} // namespace VKIntox
