#include "ui_menu.hh"

#include <cfloat>
#include <cstring>
#include <string>

#include "vendor/imgui/imgui_internal.h"
#include "vendor/imgui/imgui_m3.h"

namespace VKIntox
{
    namespace UI
    {
        namespace
        {
            constexpr float kMenuItemGap = 8.0f;

            // A few rotating slots so several truncations in one expression
            // don't stomp each other.
            const char* RotatingBuffer(const std::string& s)
            {
                static std::string slots[4];
                static int next = 0;
                std::string& slot = slots[next];
                next = (next + 1) & 3;
                slot = s;
                return slot.c_str();
            }

            void PushM3MenuStyle()
            {
                const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    ImVec2(m.menu_padding_x * m.density, m.menu_padding_y * m.density));
                ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, m.menu_radius * m.density);
                // rows stack almost flush; M3MenuItem subtracts this from the
                // themed row height so the pitch stays at menu_item_height
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(ImGui::GetStyle().ItemSpacing.x, kMenuItemGap * m.density));
            }

            void PopM3MenuStyle()
            {
                ImGui::PopStyleVar(3);
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
            // NoPadWithHalfSpacing keeps the highlight at the row rect instead of
            // bleeding half the gap into it, so the gaps stay visible; the row is
            // sized down by the gap so the pitch still lands on menu_item_height.
            const float height = ImMax(ImGui::GetFrameHeight(),
                                       m.menu_item_height * m.density - ImGui::GetStyle().ItemSpacing.y);
            const bool pressed = ImGui::Selectable(TruncateLabel(label), selected,
                                                   ImGuiSelectableFlags_NoPadWithHalfSpacing,
                                                   ImVec2(0.0f, height));
            if (selected)
                ImGui::SetItemDefaultFocus();
            return pressed;
        }

        const char* TruncateLabel(const char* label, size_t maxChars)
        {
            if (!label)
                return "";
            const size_t len = std::strlen(label);
            if (len <= maxChars || maxChars < 4)
                return label;
            std::string out(label, maxChars - 3);
            out += "...";
            return RotatingBuffer(out);
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
            if (!ImGui::BeginCombo(label, preview))
                return false;
            // rows stack almost flush inside the popup; the padding and corner
            // were already captured by BeginComboPopup itself
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(ImGui::GetStyle().ItemSpacing.x, kMenuItemGap * m.density));
            return true;
        }

        void EndM3Combo()
        {
            ImGui::PopStyleVar();
            ImGui::EndCombo();
        }
    } // namespace UI
} // namespace VKIntox
