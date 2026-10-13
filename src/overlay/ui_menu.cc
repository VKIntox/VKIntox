#include "ui_menu.hh"

#include <cfloat>
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
            constexpr float kMenuMinWidthDp = 320.0f;

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

            // Widen the popup to a legible floor so short labels don't produce a
            // cramped, clipping surface.
            void PushMenuWidthFloor()
            {
                const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
                ImGui::SetNextWindowSizeConstraints(ImVec2(kMenuMinWidthDp * m.density, 0.0f),
                                                    ImVec2(FLT_MAX, FLT_MAX));
            }
        }

        const char* TruncateLabel(const char* label, float maxWidth)
        {
            if (!label)
                return "";
            if (maxWidth <= 0.0f || ImGui::CalcTextSize(label).x <= maxWidth)
                return label;
            std::string out(label);
            while (!out.empty() && ImGui::CalcTextSize((out + "...").c_str()).x > maxWidth)
                out.pop_back();
            return RotatingBuffer(out + "...");
        }

        bool BeginM3Menu(const char* id)
        {
            PushM3MenuStyle();
            PushMenuWidthFloor();
            const bool open = ImGui::BeginPopup(id);
            if (!open)
                PopM3MenuStyle();
            return open;
        }

        bool BeginM3MenuContext(const char* id)
        {
            PushM3MenuStyle();
            PushMenuWidthFloor();
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
            const ImGuiM3Metrics& mm = m;
            const float rowWidth = ImGui::GetContentRegionAvail().x;
            const float sideRoom = 10.0f * mm.density;
            const char* shown = TruncateLabel(label, ImMax(16.0f, rowWidth - sideRoom * 2.0f));
            // empty row, then the centred name: default menus read as a centred
            // list (no leading bullet — that is the shader-preset row's mark).
            ImGui::PushID(label);
            const bool pressed = ImGui::Selectable("##row", selected,
                                                   ImGuiSelectableFlags_NoPadWithHalfSpacing,
                                                   ImVec2(0.0f, height));
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            const ImU32 text = selected ? ImGuiM3ColorU32(ImGuiM3Role_OnPrimary)
                                        : ImGuiM3ColorU32(ImGuiM3Role_OnSurface);
            const ImVec2 ts = ImGui::CalcTextSize(shown);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2((mn.x + mx.x - ts.x) * 0.5f, (mn.y + mx.y - ts.y) * 0.5f), text, shown);
            ImGui::PopID();
            if (selected)
                ImGui::SetItemDefaultFocus();
            return pressed;
        }

        bool M3MenuRadioItem(const char* label, bool selected)
        {
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            const float height = ImMax(ImGui::GetFrameHeight(),
                                       m.menu_item_height * m.density - ImGui::GetStyle().ItemSpacing.y);
            // draw the row without its own label, then emplace the dot and the
            // centred name so the pill stays empty for our own layout
            ImGui::PushID(label);
            const bool pressed = ImGui::Selectable("##row", selected,
                                                   ImGuiSelectableFlags_NoPadWithHalfSpacing,
                                                   ImVec2(0.0f, height));
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            const float rowWidth = mx.x - mn.x;
            const float bulletInset = 14.0f * m.density;
            const float bullet_r = ImMax(2.0f, ImGui::GetFontSize() * 0.19f);
            const ImU32 text = selected ? ImGuiM3ColorU32(ImGuiM3Role_OnPrimary)
                                        : ImGuiM3ColorU32(ImGuiM3Role_OnSurface);
            // keep the centred name clear of the leading dot on both sides
            const float sideRoom = bulletInset + bullet_r + 6.0f * m.density;
            const char* shown = TruncateLabel(label, ImMax(16.0f, rowWidth - sideRoom * 2.0f));
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(mn.x + bulletInset, (mn.y + mx.y) * 0.5f), bullet_r, text);
            const ImVec2 ts = ImGui::CalcTextSize(shown);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2((mn.x + mx.x - ts.x) * 0.5f, (mn.y + mx.y - ts.y) * 0.5f), text, shown);
            ImGui::PopID();
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
            // The popup is at least as wide as a menu and taller than the field, so
            // a short list shows in full instead of sprouting a scrollbar. The cap
            // still keeps a long list on screen.
            const float min_w = kMenuMinWidthDp * m.density;
            const float max_h = 420.0f * m.density;
            ImGui::SetNextWindowSizeConstraints(ImVec2(min_w, 0.0f), ImVec2(FLT_MAX, max_h));
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
