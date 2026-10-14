#ifndef UI_THEME_HPP_INCLUDED
#define UI_THEME_HPP_INCLUDED

// Semantic aliases over the Material 3 token set.
//
// The overlay used to hand-write hexes at every call site, which meant ~30
// near-duplicate colours (six reds, seven ambers) that no theme could ever move
// together. Everything here resolves through the M3 roles, so the `.colors`
// file drives the whole UI.
//
// M3 has no "success" or "warning" role — only primary, secondary, tertiary and
// error — so status colours borrow from that set: tertiary is the natural
// positive, primary the informational one.

#include "vendor/imgui/imgui_internal.h"   // ImRect / ImMax for the shared row helpers
#include <string>

#include "vendor/imgui/imgui_m3.h"
#include "logger.hh"

namespace VKIntox::UI
{
    inline ImVec4 Primary()    { return ImGuiM3Color(ImGuiM3Role_Primary); }
    inline ImVec4 Secondary()  { return ImGuiM3Color(ImGuiM3Role_Secondary); }
    inline ImVec4 Tertiary()   { return ImGuiM3Color(ImGuiM3Role_Tertiary); }
    inline ImVec4 Error()      { return ImGuiM3Color(ImGuiM3Role_Error); }
    inline ImVec4 OnError()    { return ImGuiM3Color(ImGuiM3Role_OnError); }
    inline ImVec4 Outline()    { return ImGuiM3Color(ImGuiM3Role_Outline); }
    inline ImVec4 OnSurface()  { return ImGuiM3Color(ImGuiM3Role_OnSurface); }
    inline ImVec4 Muted()      { return ImGuiM3Color(ImGuiM3Role_OnSurfaceVariant); }
    inline ImVec4 Container()  { return ImGuiM3Color(ImGuiM3Role_SurfaceContainer); }
    inline ImVec4 ContainerLow() { return ImGuiM3Color(ImGuiM3Role_SurfaceContainerLow); }
    inline ImVec4 Surface()    { return ImGuiM3Color(ImGuiM3Role_Surface); }

    // Text on a role's own container.
    inline ImVec4 On(ImGuiM3Role role) { return ImGuiM3OnColor(role); }

    // Status colours.
    inline ImVec4 Success()   { return Tertiary(); }
    inline ImVec4 Warning()   { return ImGuiM3Color(ImGuiM3Role_Primary); }
    inline ImVec4 Attention() { return Secondary(); }

    // The one place log levels map to colours, so toasts, the log filter and the
    // shader-test summary can no longer drift apart.
    inline ImVec4 LogLevelColor(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Error: return Error();
        case LogLevel::Warn:  return Warning();
        default:              return Primary();
        }
    }

    // M3 supporting text: the M3 body-small step (12sp) on-surface-variant, in
    // the body family's bold so secondary lines read as emphasis rather than as
    // washed-out grey. Gives the overlay a real hierarchy instead of one flat
    // 14px everywhere.
    inline void PushSupport()
    {
        ImGuiM3PushTextStyle(ImGuiM3Type_BodySmall);
        ImGui::PushFont(ImGuiM3FontBodyBold(),
                        ImGuiM3GetTypeStyle(ImGuiM3Type_BodySmall).size * ImGuiM3GetMetrics().density);
    }
    inline void PopSupport()
    {
        ImGui::PopFont();
        ImGuiM3PopTextStyle();
    }
    inline void TextSupport(const char* text)
    {
        PushSupport();
        ImGui::TextDisabled("%s", text);
        PopSupport();
    }

    // Sub-view crumb: the round back button plus the destination it returns to.
    // Returns true when the crumb was clicked. Sub-views slide in over the dock
    // and are the overlay's answer to pushing a page inside a tab.
    inline bool BreadcrumbBar(const char* parent_label, const char* back_glyph)
    {
        bool back = ImGui::M3IconButton(back_glyph, "Back", ImGuiM3Button_Tonal);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(ImGuiM3FontBold(),
                        ImGuiM3GetTypeStyle(ImGuiM3Type_TitleMedium).size * ImGuiM3GetMetrics().density);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant));
        ImGui::TextUnformatted(parent_label);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        return back;
    }

    // Folded group header with the M3 spring reveal: rounded surface, label on
    // the document face, optional count chip, chevron on the right. Returns the
    // eased reveal (0 folded, 1 open) the caller should feed FoldBodyBegin.
    inline float FoldHeader(const char* id, const char* label, const char* badge, bool* open)
    {
        const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
        const float d = m.density;
        const float reveal = ImClamp(ImGuiM3SpringStepEffectsFast(
            ImGui::GetID(id) ^ 0x464F4C44u, *open ? 1.0f : 0.0f), 0.0f, 1.0f);

        const float rowH = ImGui::GetFrameHeight() + 10.0f * d;
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float rowW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
        ImGui::InvisibleButton(id, ImVec2(rowW, rowH));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked())
            *open = !*open;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImRect rowRect(rowMin, ImVec2(rowMin.x + rowW, rowMin.y + rowH));
        const float rr = ImGuiM3Radius(ImGuiM3Shape_Small);
        const ImGuiM3ShapeRounding round{ rr, rr, rr, rr };
        ImGuiM3PathRoundedRect(dl, rowRect, round, ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainer));
        if (hovered)
            ImGuiM3DrawStateLayer(dl, rowRect, round, ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);

        ImFont* brand = ImGuiM3FontBold();
        const float labelPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * d;
        ImGui::PushFont(brand, labelPx);
        const ImVec2 labelSize = ImGui::CalcTextSize(label);
        ImGui::PopFont();
        dl->AddText(brand, labelPx, ImVec2(rowMin.x + 14.0f * d, rowMin.y + (rowH - labelSize.y) * 0.5f),
                    ImGuiM3ColorU32(ImGuiM3Role_OnSurface), label);

        if (badge && badge[0])
        {
            ImFont* bodyBold = ImGuiM3FontBodyBold();
            const float chipPx = 11.0f * d;
            ImGui::PushFont(bodyBold, chipPx);
            const ImVec2 chipText = ImGui::CalcTextSize(badge);
            ImGui::PopFont();
            const float chipH = 18.0f * d;
            const ImVec2 chipMin(rowMin.x + 14.0f * d + labelSize.x + 8.0f * d,
                                 rowMin.y + (rowH - chipH) * 0.5f);
            const ImVec2 chipMax(chipMin.x + chipText.x + 14.0f * d, chipMin.y + chipH);
            ImGuiM3PathRoundedRect(dl, ImRect(chipMin, chipMax),
                                   ImGuiM3ShapeRounding{ chipH * 0.5f, chipH * 0.5f, chipH * 0.5f, chipH * 0.5f },
                                   ImGuiM3ColorU32(ImGuiM3Role_SecondaryContainer));
            dl->AddText(bodyBold, chipPx,
                        ImVec2(chipMin.x + 7.0f * d, chipMin.y + (chipH - chipText.y) * 0.5f),
                        ImGuiM3ColorU32(ImGuiM3Role_OnSecondaryContainer), badge);
        }

        const float chevronPx = 20.0f * d;
        ImGuiM3TreeNodeArrow(dl, ImVec2(rowRect.Max.x - 14.0f * d - chevronPx,
                                        rowMin.y + (rowH - chevronPx) * 0.5f),
                             chevronPx, ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant), *open);
        return reveal;
    }

    // Body of a folded group: a scrolling child whose height follows the reveal,
    // capped so a long list can never take over the view. Pair with FoldBodyEnd.
    inline bool FoldBodyBegin(const char* id, float reveal, float content_h, float max_h)
    {
        // a child never shrinks below its title-bar allowance (WindowRounding - 1),
        // so the fold stalled with a row or two on screen then blinked out
        // size for the content plus the child's own padding, or the last row's
        // bottom margin is clipped and the fold ends with a snap
        const float padding = ImGui::GetStyle().WindowPadding.y * 2.0f;
        const float height = content_h <= 0.0f ? 0.0f : ImMin(content_h + padding, max_h) * reveal;
        if (height < 1.0f)
            return false;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_None);
        ImGui::PopStyleVar();
        // pin the scroll while it moves, or a collapsing list shows a middle slice
        if (reveal < 0.999f)
            ImGui::SetScrollY(0.0f);
        return true;
    }
    inline void FoldBodyEnd()
    {
        ImGui::EndChild();
    }

    // a row of pill page openers: destinations, not toggles. each carries the
    // chevron SubViewRow uses. returns the pressed index, or -1.
    inline int PillPageRow(const char* const* labels, const char* const* glyphs, int count)
    {
        const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
        const float d = m.density;
        const float h = m.button_height_default * d;
        const float padX = 18.0f * d;
        const float iconPx = m.button_icon_default * d;
        const float iconGap = 8.0f * d;
        const float chevronPx = 18.0f * d;
        const float chevronGap = 4.0f * d;
        const float labelPx = ImGuiM3GetTypeStyle(ImGuiM3Type_LabelLarge).size * d;
        ImFont* labelFont = ImGuiM3FontMedium();
        const char* chevron = "\xEE\x90\x89";   // chevron_right, the "this goes somewhere" mark
        int pressed = -1;

        for (int i = 0; i < count; ++i)
        {
            if (i > 0)
                ImGui::SameLine(0.0f, 2.0f * d);   // 2dp seam, like the connected group
            ImGui::PushID(i);
            ImGui::PushFont(labelFont, labelPx);
            const ImVec2 textSize = ImGui::CalcTextSize(labels[i]);
            ImGui::PopFont();

            float segW = textSize.x + padX * 2.0f;
            if (glyphs && glyphs[i])
                segW += iconPx + iconGap;
            if (ImGuiM3IconFont())
                segW += chevronGap + chevronPx;

            const ImVec2 segPos = ImGui::GetCursorScreenPos();
            const ImRect bb(segPos, ImVec2(segPos.x + segW, segPos.y + h));
            ImGui::InvisibleButton("##page", bb.GetSize());
            const bool hovered = ImGui::IsItemHovered();
            const bool held = ImGui::IsItemActive();
            if (ImGui::IsItemClicked())
                pressed = i;

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImGuiM3ShapeRounding rounding = ImGuiM3ConnectedSegmentRounding(bb.GetSize(), i, count);
            ImGuiM3PathRoundedRect(dl, bb, rounding, ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainerHigh));
            if (hovered || held)
                ImGuiM3DrawStateLayer(dl, bb, rounding, ImGuiM3Role_OnSurface,
                                      held ? ImGuiM3State_Pressed : ImGuiM3State_Hovered);

            float textX = bb.Min.x + padX;
            if (glyphs && glyphs[i] && ImGuiM3IconFont())
            {
                ImGuiM3DrawIcon(dl, glyphs[i],
                                ImRect(ImVec2(textX, bb.Min.y), ImVec2(textX + iconPx, bb.Max.y)),
                                iconPx, ImGuiM3ColorU32(ImGuiM3Role_Primary));
                textX += iconPx + iconGap;
            }
            dl->AddText(labelFont, labelPx,
                        ImVec2(textX, bb.GetCenter().y - textSize.y * 0.5f),
                        ImGuiM3ColorU32(ImGuiM3Role_OnSurface), labels[i]);
            if (ImGuiM3IconFont())
                ImGuiM3DrawIcon(dl, chevron,
                                ImRect(ImVec2(textX + textSize.x + chevronGap, bb.Min.y),
                                       ImVec2(textX + textSize.x + chevronGap + chevronPx, bb.Max.y)),
                                chevronPx, ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant));
            ImGui::PopID();
        }
        return pressed;
    }

    // A row that pushes a sub-view, mirroring the breadcrumb it opens: label on
    // the left, trailing metadata, chevron on the right. Returns true when it
    // was clicked.
    inline bool SubViewRow(const char* label, const char* trailing = nullptr,
                           const char* glyph = nullptr)
    {
        const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
        const float d = m.density;
        const float rowH = ImMax(ImGui::GetFrameHeight() + 8.0f * d, 52.0f * d);
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float rowW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
        ImGui::InvisibleButton("##subview_row", ImVec2(rowW, rowH));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const ImRect rowRect(rowMin, ImVec2(rowMin.x + rowW, rowMin.y + rowH));

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float radius = ImGuiM3Radius(ImGuiM3Shape_Large);
        const ImGuiM3ShapeRounding round{ radius, radius, radius, radius };
        ImGuiM3PathRoundedRect(dl, rowRect, round, ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainer));
        if (hovered)
            ImGuiM3DrawStateLayer(dl, rowRect, round, ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);

        float textX = rowMin.x + 16.0f * d;
        if (glyph && ImGuiM3IconFont())
        {
            ImGuiM3DrawIcon(dl, glyph,
                            ImRect(ImVec2(textX, rowMin.y), ImVec2(textX + 28.0f * d, rowMin.y + rowH)),
                            22.0f * d, ImGuiM3ColorU32(ImGuiM3Role_Primary));
            textX += 40.0f * d;
        }

        const float labelPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * d;
        ImFont* brand = ImGuiM3FontBold();
        const float chevronPx = 20.0f * d;
        const float chevronX = rowRect.Max.x - 16.0f * d - chevronPx;
        float textRight = chevronX - 12.0f * d;
        if (trailing && trailing[0])
        {
            ImFont* metaFont = ImGuiM3FontBodyBold();
            const float metaPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodySmall).size * d;
            ImGui::PushFont(metaFont, metaPx);
            const ImVec2 metaSize = ImGui::CalcTextSize(trailing);
            ImGui::PopFont();
            dl->AddText(metaFont, metaPx,
                        ImVec2(textRight - metaSize.x, rowMin.y + (rowH - metaSize.y) * 0.5f),
                        ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant), trailing);
            textRight -= metaSize.x + 12.0f * d;
        }

        ImGui::PushFont(brand, labelPx);
        const ImVec2 labelSize = ImGui::CalcTextSize(label);
        ImGui::PopFont();
        dl->AddText(brand, labelPx, ImVec2(textX, rowMin.y + (rowH - labelSize.y) * 0.5f),
                    ImGuiM3ColorU32(ImGuiM3Role_OnSurface), label);
        ImGuiM3TreeNodeArrow(dl, ImVec2(chevronX, rowMin.y + (rowH - chevronPx) * 0.5f), chevronPx,
                             ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant), false);
        return clicked;
    }

    // Animated accordion progress: 0 folded, 1 unfolded. Spring-driven, so a
    // toggle reversed mid-flight eases from wherever it is instead of snapping —
    // which is what made the old clock-based fold glitch on fast clicks.
    // `overshoot` picks the livelier spatial curve (good for reveals that have
    // headroom); the default is critically damped, which never runs past 1.
    inline float FoldProgress(ImGuiID id, bool open, bool overshoot = false, float speed = 1.0f)
    {
        const float target = open ? 1.0f : 0.0f;
        if (overshoot)
            return ImGuiM3SpringStepSpatialFast(id, target);
        // effects-fast is critically damped; `speed` scales its stiffness only, so
        // a faster fold still lands without bouncing.
        return ImGuiM3SpringStep(id, target, 1.0f, 1600.0f * ImMax(speed, 0.1f));
    }

    // Diagnostics graphs need visually separable series that stay legible on any
    // scheme, so they walk the palette instead of hardcoding hues.
    inline ImVec4 GraphColor(int index)
    {
        static const ImGuiM3Role order[] = {
            ImGuiM3Role_Primary, ImGuiM3Role_Tertiary, ImGuiM3Role_Secondary, ImGuiM3Role_Error, ImGuiM3Role_Outline};
        const int n = (int)(sizeof(order) / sizeof(order[0]));
        return ImGuiM3Color(order[((index % n) + n) % n]);
    }
}

#endif // UI_THEME_HPP_INCLUDED