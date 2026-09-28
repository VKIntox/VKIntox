#include "../field_editor.hh"
#include "../../../vendor/imgui/imgui.h"
#include "../../../vendor/imgui/imgui_internal.h"
#include <cstring>
#include <cstdio>
#include <string>

namespace VKIntox
{
    // Robust unsigned int field editor with custom value input via right-click
    class UintFieldEditor : public FieldEditor
    {
    public:
        bool render(EffectParam& param) override
        {
            auto& p = static_cast<UintParam&>(param);
            bool changed = false;

            // Generate stable unique IDs using parameter address
            const void* paramAddr = static_cast<const void*>(&param);
            char contextId[64];
            char modalId[64];
            char inputId[64];
            snprintf(contextId, sizeof(contextId), "##uintctx_%p", paramAddr);
            snprintf(modalId, sizeof(modalId), "##uintmodal_%p", paramAddr);
            snprintf(inputId, sizeof(inputId), "##uintinput_%p", paramAddr);

            // === Render the slider (using SliderScalar for unsigned) ===
            if (ImGui::SliderScalar(p.label.c_str(), ImGuiDataType_U32, 
                &p.value, &p.minValue, &p.maxValue))
            {
                if (p.step > 0.0f)
                {
                    uint32_t step = static_cast<uint32_t>(p.step);
                    if (step > 0)
                        p.value = (p.value / step) * step;
                }
                changed = true;
            }

            // === Right-click context menu ===
            if (ImGui::BeginPopupContextItem(contextId))
            {
                if (ImGui::MenuItem("Enter Custom Value..."))
                {
                    m_activeModalParam = paramAddr;
                    snprintf(m_customValueBuf, sizeof(m_customValueBuf), "%u", p.value);
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
            // === Deferred modal opening ===
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

            // === Render custom value popup ===
            if (m_activeModalParam == paramAddr &&
                !popupOpenedThisFrame && ImGui::BeginPopup(modalId))
            {
                ImGui::Text("Custom value: %s", p.label.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("Current range: [%u, %u]", p.minValue, p.maxValue);
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

                if (submit || ok)
                {
                    unsigned int newVal;
                    if (sscanf(m_customValueBuf, "%u", &newVal) == 1)
                    {
                        p.value = newVal;
                        changed = true;
                    }
                    ImGui::CloseCurrentPopup();
                    m_activeModalParam = nullptr;
                }

                if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    ImGui::CloseCurrentPopup();
                    m_activeModalParam = nullptr;
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
        const void* m_activeModalParam = nullptr;
        bool m_modalOpenRequested = false;
        bool m_focusCustomValueInput = false;
        ImVec2 m_popupPosition = ImVec2(0.0f, 0.0f);
    };

    REGISTER_FIELD_EDITOR(ParamType::Uint, UintFieldEditor)

} // namespace VKIntox
