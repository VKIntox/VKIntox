#include "../field_editor.hh"
#include "../../../vendor/imgui/imgui.h"
#include "../../../vendor/imgui/imgui_internal.h"
#include "../../ui_dialog.hh"
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
                    activeModalParam = paramAddr;
                    snprintf(customValueBuf, sizeof(customValueBuf), "%u", p.value);
                    popupPosition = ImGui::GetMousePos();
                    focusCustomValueInput = true;
                    modalOpenRequested = true;
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
            if (modalOpenRequested && activeModalParam == paramAddr)
            {
                modalOpenRequested = false;
                const ImGuiViewport* viewport = ImGui::GetMainViewport();
                const ImVec2 popupSize = ImVec2(320.0f, 190.0f);
                ImGui::SetNextWindowPos(customValuePopupPosition(
                    popupPosition, viewport->Pos, viewport->Size, popupSize), ImGuiCond_Appearing);
                ImGui::SetNextWindowPos(customValuePopupPosition(
                    popupPosition, viewport->Pos, viewport->Size, popupSize), ImGuiCond_Appearing);
                ImGui::OpenPopup(modalId);
                popupOpenedThisFrame = true;
            }

            // === Render custom value popup ===
            if (activeModalParam == paramAddr &&
                !popupOpenedThisFrame && UI::BeginM3Dialog(modalId, ("Custom value: " + p.label).c_str()))
            {
                ImGui::TextDisabled("Current range: [%u, %u]", p.minValue, p.maxValue);
                ImGui::TextDisabled("(values outside range are allowed)");
                ImGui::Spacing();

                ImGui::SetNextItemWidth(220.0f);
                if (focusCustomValueInput)
                {
                    ImGui::SetKeyboardFocusHere();
                    focusCustomValueInput = false;
                }
                bool submit = ImGui::InputText(inputId, customValueBuf,
                    sizeof(customValueBuf), ImGuiInputTextFlags_EnterReturnsTrue);

                ImGui::Spacing();
                const UI::M3DialogAction actions[] = {
                    {"Cancel", ImGuiM3Button_Outlined},
                    {"OK", ImGuiM3Button_Filled},
                };
                const int action = UI::M3DialogActions(actions, 2);
                const bool ok = (action == 1);
                const bool cancel = (action == 0);

                if (submit || ok)
                {
                    unsigned int newVal;
                    if (sscanf(customValueBuf, "%u", &newVal) == 1)
                    {
                        p.value = newVal;
                        changed = true;
                    }
                    ImGui::CloseCurrentPopup();
                    activeModalParam = nullptr;
                }

                if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    ImGui::CloseCurrentPopup();
                    activeModalParam = nullptr;
                }

                UI::EndM3Dialog();
            }

            return changed;
        }

        void resetToDefault(EffectParam& param) override
        {
            param.resetToDefault();
        }

    private:
        static constexpr size_t BUF_SIZE = 128;
        char customValueBuf[BUF_SIZE] = "";
        const void* activeModalParam = nullptr;
        bool modalOpenRequested = false;
        bool focusCustomValueInput = false;
        ImVec2 popupPosition = ImVec2(0.0f, 0.0f);
    };

    REGISTER_FIELD_EDITOR(ParamType::Uint, UintFieldEditor)

} // namespace VKIntox
