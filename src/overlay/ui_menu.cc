#include "ui_menu.hh"

#include <cfloat>

#include "vendor/imgui/imgui_internal.h"
#include "vendor/imgui/imgui_m3.h"

namespace VKIntox
{
    namespace UI
    {
        namespace
        {
            void PushM3MenuStyle()
            {
                const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    ImVec2(m.menu_padding_x * m.density, m.menu_padding_y * m.density));
                ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, m.menu_radius * m.density);
            }

            void PopM3MenuStyle()
            {
                ImGui::PopStyleVar(2);
            }
        }

        bool BeginM3Menu(const char* id)
        {
            PushM3MenuStyle();
            const bool open = ImGui::BeginPopup(id);
            if (!open)
                PopM3MenuStyle();
            return open;
        }

        bool BeginM3MenuContext(const char* id)
        {
            PushM3MenuStyle();
            const bool open = ImGui::BeginPopupContextItem(id);
            if (!open)
                PopM3MenuStyle();
            return open;
        }

        bool M3MenuItem(const char* label, bool selected)
        {
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            // Selectable stretches its highlight by half the item spacing on each
            // side, so sizing the row to item height minus the spacing keeps the
            // visual pitch at the themed 48dp without gaps between pills.
            const float height = ImMax(ImGui::GetFrameHeight(),
                                       m.menu_item_height * m.density - ImGui::GetStyle().ItemSpacing.y);
            const bool pressed = ImGui::Selectable(label, selected, ImGuiSelectableFlags_None,
                                                   ImVec2(0.0f, height));
            if (selected)
                ImGui::SetItemDefaultFocus();
            return pressed;
        }

        void EndM3Menu()
        {
            ImGui::EndPopup();
            PopM3MenuStyle();
        }

        bool BeginM3Combo(const char* label, const char* preview)
        {
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            // cap the popup height so a long list scrolls instead of running off
            // the overlay.
            ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                                ImVec2(FLT_MAX, 280.0f * m.density));
            return ImGui::BeginCombo(label, preview);
        }

        void EndM3Combo()
        {
            ImGui::EndCombo();
        }
    } // namespace UI
} // namespace VKIntox
