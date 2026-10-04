#include "ui_dialog.hh"

#include <cfloat>

namespace VKIntox
{
    namespace UI
    {
        bool BeginM3Dialog(const char* id, const char* title)
        {
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            const float pad = 24.0f * m.density;

            // Pushed before the popup window is created so it captures the M3
            // dialog padding and corner radius.
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, m.dialog_radius * m.density);

            if (!ImGui::BeginPopup(id))
            {
                ImGui::PopStyleVar(2);
                return false;
            }

            ImFont* bold = ImGuiM3FontBold();
            if (bold)
                ImGui::PushFont(bold, ImGui::GetFontSize() * 1.12f);
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 380.0f * m.density);
            ImGui::TextUnformatted(title);
            ImGui::PopTextWrapPos();
            if (bold)
                ImGui::PopFont();

            ImGui::Spacing();
            return true;
        }

        void EndM3Dialog()
        {
            ImGui::EndPopup();
            ImGui::PopStyleVar(2);
        }

        bool M3DialogButton(const char* label, ImGuiM3ButtonVariant variant, float width)
        {
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            return ImGui::M3Button(label, variant, ImVec2(width, 32.0f * m.density), true);
        }

        float M3DialogButtonWidth(const char* label)
        {
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            ImFont* bold = ImGuiM3FontBold();
            const float textWidth = bold
                ? bold->CalcTextSizeA(ImGui::GetFontSize() * 1.05f, FLT_MAX, 0.0f, label).x
                : ImGui::CalcTextSize(label).x;
            return textWidth + m.button_padding_x * 2.0f * m.density;
        }

        int M3DialogActions(const M3DialogAction* actions, int count)
        {
            if (!actions || count <= 0)
                return -1;
            if (count > 4)
                count = 4;

            const ImGuiStyle& style = ImGui::GetStyle();
            float widths[4];
            float total = style.ItemSpacing.x * static_cast<float>(count - 1);
            for (int i = 0; i < count; ++i)
            {
                widths[i] = M3DialogButtonWidth(actions[i].label);
                total += widths[i];
            }

            // right-align against the content edge, so the trailing margin matches
            // the dialog padding instead of the actions hugging the left
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - style.WindowPadding.x - total);

            int clicked = -1;
            for (int i = 0; i < count; ++i)
            {
                if (i > 0)
                    ImGui::SameLine();
                if (M3DialogButton(actions[i].label, actions[i].variant, widths[i]))
                    clicked = i;
            }
            return clicked;
        }
    } // namespace UI
} // namespace VKIntox
