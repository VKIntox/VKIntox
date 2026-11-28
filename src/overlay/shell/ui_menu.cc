#include "overlay/shell/ui_menu.hh"

#include <cfloat>
#include <string>
#include <unordered_map>

#include "vendor/imgui/imgui_internal.h"
#include "vendor/imgui/imgui_m3.h"

namespace VKIntox
{
    namespace UI
    {
        namespace
        {
            // compact rows and a strongly rounded surface, with every highlight
            // locked to the same inner width
            constexpr float kMenuMinWidthDp = 200.0f;
            constexpr float kMenuMaxWidthDp = 320.0f;
            constexpr float kMenuMaxHeightDp = 420.0f;
            constexpr float kItemPadXDp = 16.0f;
            constexpr float kMenuItemHeightDp = 20.0f;
            constexpr float kRowGapDp = 0.0f;

            // Appear: a pure fade. Position and size are left to ImGui's own
            // popup placement — animating the window ourselves fought the
            // auto-fit size and left the surface drawn where the window used to
            // be, which is what cropped the rows on the first frames.
            constexpr float kAppearSeconds = 0.14f;

            struct MenuAppear
            {
                double start = 0.0;
                double lastSeen = 0.0;
            };
            std::unordered_map<ImGuiID, MenuAppear> g_menuAppear;
            ImVec4 g_menuBackground = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

            float AppearEase(const MenuAppear& s, double now)
            {
                if (ImGuiM3GetMetrics().reduced_motion)
                    return 1.0f;
                if (now <= s.start)
                    return 0.0f;
                const float clock = static_cast<float>((now - s.start) / kAppearSeconds);
                if (clock >= 1.0f)
                    return 1.0f;
                // emphasized decelerate: quick to arrive, soft to land
                return ImGuiM3EaseEmphasizedDecelerate(clock);
            }

            void ReapMenus(double now)
            {
                for (auto it = g_menuAppear.begin(); it != g_menuAppear.end();)
                {
                    if (now - it->second.lastSeen > 2.0)
                        it = g_menuAppear.erase(it);
                    else
                        ++it;
                }
            }

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

            // Container insets + corner. Items draw their own highlight, so no
            // FrameRounding override is needed.
            void PushM3MenuStyle()
            {
                const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    ImVec2(m.menu_padding_x * m.density, m.menu_padding_y * m.density));
                const float radius = ImGuiM3Radius(ImGuiM3Shape_LargeIncreased);
                ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, radius);
                ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(0.0f, kRowGapDp * m.density));
                ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f * m.density);
            }

            void PopM3MenuStyle()
            {
                ImGui::PopStyleVar(5);
            }

            void PushMenuWidthFloor()
            {
                const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
                ImGui::SetNextWindowSizeConstraints(ImVec2(kMenuMinWidthDp * m.density, 0.0f),
                                                    ImVec2(kMenuMaxWidthDp * m.density,
                                                           kMenuMaxHeightDp * m.density));
            }

            // Fade the popup in. The position is ImGui's to own: the window is
            // auto-sized from its rows, and nudging it ourselves moved the
            // surface out from under a size that was still fitting.
            void ApplyMenuAppear(ImGuiID popupId, double now)
            {
                auto it = g_menuAppear.find(popupId);
                if (ImGui::IsWindowAppearing() || it == g_menuAppear.end())
                {
                    MenuAppear fresh;
                    fresh.start = now;
                    fresh.lastSeen = now;
                    g_menuAppear[popupId] = fresh;
                    it = g_menuAppear.find(popupId);
                }
                MenuAppear& s = it->second;
                s.lastSeen = now;
                const float alpha = ImClamp(AppearEase(s, now), 0.0f, 1.0f);
                const ImRect rect = ImGui::GetCurrentWindow()->Rect();
                const float radius = ImGuiM3Radius(ImGuiM3Shape_LargeIncreased);
                const ImGuiM3ShapeRounding rounding{ radius, radius, radius, radius };
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImGuiM3DrawElevation(dl, rect, rounding, 2, alpha);
                ImVec4 bg = g_menuBackground;
                bg.w *= alpha;
                ImGuiM3PathRoundedRect(dl, rect, rounding, ImGui::ColorConvertFloat4ToU32(bg));
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
            }

            void PushFadedPopupBg(ImGuiID, double)
            {
                g_menuBackground = ImGui::GetStyle().Colors[ImGuiCol_PopupBg];
                ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            }
            // One row's highlight: nothing by default, a state layer on hover/press,
            // and the accent container when selected. Radii stay well under the
            // row's half-height so a row reads as a rounded rectangle, not a pill.
            void DrawRowHighlight(const ImRect& bb, bool selected, bool hovered, bool held)
            {
                const float r = ImMin(ImGuiM3Radius(ImGuiM3Shape_Medium), bb.GetHeight() * 0.5f);
                const ImGuiM3ShapeRounding round{ r, r, r, r };
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (selected)
                    ImGuiM3PathRoundedRect(dl, bb, round, ImGuiM3ColorU32(ImGuiM3Role_SecondaryContainer));
                else if (held || hovered)
                    ImGuiM3DrawStateLayer(dl, bb, round, ImGuiM3Role_OnSurface,
                                          held ? ImGuiM3State_Pressed : ImGuiM3State_Hovered);
            }

            // The width every row in this popup is locked to: the surface's own
            // inner width, so highlights never come in different lengths.
            float LockedRowWidth(const ImVec2& text_size, float padX)
            {
                const float avail = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
                return ImMax(avail, text_size.x + padX * 2.0f);
            }

            float ItemFontSize() { return ImGuiM3GetTypeStyle(ImGuiM3Type_LabelMedium).size * ImGuiM3GetMetrics().density; }
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
            const double now = ImGui::GetTime();
            const ImGuiID popupId = ImGui::GetCurrentWindow()->GetID(id);
            ReapMenus(now);
            PushFadedPopupBg(popupId, now);
            if (!ImGui::BeginPopup(id))
            {
                ImGui::GetCurrentContext()->NextWindowData.ClearFlags();
                ImGui::PopStyleColor();
                PopM3MenuStyle();
                return false;
            }
            ApplyMenuAppear(popupId, now);
            return true;
        }

        bool BeginM3MenuContext(const char* id)
        {
            PushM3MenuStyle();
            PushMenuWidthFloor();
            const double now = ImGui::GetTime();
            const ImGuiID popupId = ImGui::GetCurrentWindow()->GetID(id);
            ReapMenus(now);
            PushFadedPopupBg(popupId, now);
            if (!ImGui::BeginPopupContextItem(id))
            {
                ImGui::GetCurrentContext()->NextWindowData.ClearFlags();
                ImGui::PopStyleColor();
                PopM3MenuStyle();
                return false;
            }
            ApplyMenuAppear(popupId, now);
            return true;
        }

        bool M3MenuItem(const char* label, bool selected)
        {
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            const float padX = kItemPadXDp * m.density;
            // menu text: the document face (Google Sans Flex, rounded), label-large.
            ImFont* brand = ImGuiM3FontBold();
            if (brand)
                ImGui::PushFont(brand, ItemFontSize());
            // the row hugs its text instead of a fixed height.
            const float height = kMenuItemHeightDp * m.density;
            const float maxContent = kMenuMaxWidthDp * m.density - 2.0f * m.menu_padding_x * m.density;
            const ImVec2 full = ImGui::CalcTextSize(label ? label : "");
            const float rowWidth = LockedRowWidth(full, padX);
            const float textBudget = ImMax(16.0f, ImMin(rowWidth, maxContent) - padX * 2.0f);
            const char* shown = TruncateLabel(label, textBudget);
            ImGui::PushID(label);
            const bool pressed = ImGui::InvisibleButton("##row", ImVec2(rowWidth, height));
            const bool hovered = ImGui::IsItemHovered();
            const bool held = ImGui::IsItemActive();
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            DrawRowHighlight(ImRect(mn, mx), selected, hovered, held);
            const ImU32 text = selected ? ImGuiM3ColorU32(ImGuiM3Role_OnSecondaryContainer)
                                        : ImGuiM3ColorU32(ImGuiM3Role_OnSurface);
            const ImVec2 ts = ImGui::CalcTextSize(shown);
            ImGui::GetWindowDrawList()->AddText(ImVec2(mn.x + padX, (mn.y + mx.y - ts.y) * 0.5f), text, shown);
            ImGui::PopID();
            if (selected)
                ImGui::SetItemDefaultFocus();
            if (brand)
                ImGui::PopFont();
            return pressed;
        }

        bool M3MenuRadioItem(const char* label, bool selected)
        {
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            const float padX = kItemPadXDp * m.density;
            ImFont* brand = ImGuiM3FontBold();
            if (brand)
                ImGui::PushFont(brand, ItemFontSize());
            const float height = kMenuItemHeightDp * m.density;
            const float bullet_r = ImMax(2.0f, ImGui::GetFontSize() * 0.19f);
            const float textX = padX + bullet_r * 2.0f + 10.0f * m.density;
            const float maxContent = kMenuMaxWidthDp * m.density - 2.0f * m.menu_padding_x * m.density;
            const ImVec2 full = ImGui::CalcTextSize(label ? label : "");
            const float rowWidth = LockedRowWidth(ImVec2(full.x + textX - padX, full.y), padX);
            const float textBudget = ImMax(16.0f, ImMin(rowWidth, maxContent) - textX - padX);
            ImGui::PushID(label);
            const bool pressed = ImGui::InvisibleButton("##row", ImVec2(rowWidth, height));
            const bool hovered = ImGui::IsItemHovered();
            const bool held = ImGui::IsItemActive();
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            DrawRowHighlight(ImRect(mn, mx), selected, hovered, held);
            const ImU32 text = selected ? ImGuiM3ColorU32(ImGuiM3Role_OnSecondaryContainer)
                                        : ImGuiM3ColorU32(ImGuiM3Role_OnSurface);
            const char* shown = TruncateLabel(label, textBudget);
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(mn.x + padX + bullet_r, (mn.y + mx.y) * 0.5f), bullet_r, text);
            const ImVec2 ts = ImGui::CalcTextSize(shown);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(mn.x + textX, (mn.y + mx.y - ts.y) * 0.5f), text, shown);
            ImGui::PopID();
            if (selected)
                ImGui::SetItemDefaultFocus();
            if (brand)
                ImGui::PopFont();
            return pressed;
        }

        void EndM3Menu()
        {
            ImGui::PopStyleVar();        // appear alpha
            ImGui::EndPopup();
            ImGui::PopStyleColor();      // faded popup bg
            PopM3MenuStyle();
        }

        bool BeginM3Combo(const char* label, const char* preview)
        {
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            const double now = ImGui::GetTime();
            const ImGuiID comboId = ImGui::GetCurrentWindow()->GetID(label);
            const ImGuiID popupId = ImHashStr("##ComboPopup", 0, comboId);
            ReapMenus(now);
            PushM3MenuStyle();
            PushFadedPopupBg(popupId, now);
            // The popup's floor is the field width, so dragging the window would
            // re-flow an open menu under the cursor. Latch it at open time and the
            // menu holds its size for as long as it is up.
            static std::unordered_map<ImGuiID, float> s_comboWidthLatch;
            const float fieldW = ImGui::CalcItemWidth();
            const float minWidth = kMenuMinWidthDp * m.density;
            const float maxWidth = kMenuMaxWidthDp * m.density;
            if (ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None))
            {
                float& latched = s_comboWidthLatch[popupId];
                if (latched <= 0.0f)
                    latched = fieldW;
                const float popupWidth = ImClamp(ImMax(minWidth, latched), minWidth, maxWidth);
                const float max_h = kMenuMaxHeightDp * m.density;
                ImGui::SetNextWindowSize(ImVec2(popupWidth, 0.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSizeConstraints(ImVec2(popupWidth, 0.0f),
                                                    ImVec2(popupWidth, max_h));
                const ImVec2 anchor = ImGui::GetCursorScreenPos();
                ImGui::SetNextWindowPos(ImVec2(anchor.x + (fieldW - popupWidth) * 0.5f,
                                               anchor.y + ImGui::GetFrameHeight()), ImGuiCond_Always);
            }
            else
            {
                s_comboWidthLatch.erase(popupId);
            }
            // the combo field is a rounded select, not the global pill.
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGuiM3Radius(ImGuiM3Shape_Small));
            if (!ImGui::BeginCombo(label, preview))
            {
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                PopM3MenuStyle();
                return false;
            }
            ApplyMenuAppear(popupId, now);
            return true;
        }

        void EndM3Combo()
        {
            ImGui::PopStyleVar();       // appear alpha
            ImGui::EndCombo();
            ImGui::PopStyleColor();     // faded popup bg
            ImGui::PopStyleVar();       // combo frame rounding
            PopM3MenuStyle();
        }
    } // namespace UI
} // namespace VKIntox
