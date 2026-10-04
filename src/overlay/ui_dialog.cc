#include "ui_dialog.hh"

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
    } // namespace UI
} // namespace VKIntox
