#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include "overlay/shell/imgui_overlay.hh"
#include "overlay/theme/ui_icons.hh"
#include "overlay/theme/ui_theme.hh"
#include "overlay/shell/ui_menu.hh"
#include "version.hh"

#include <string>

#include "vendor/imgui/imgui.h"

namespace VKIntox
{
    namespace
    {
        const char* runtimeVersion()
        {
            return VKINTOX_VERSION;
        }

        struct Credit
        {
            const char* what;
            const char* handle;
            const char* url;
        };

        void renderCreditRow(const Credit& credit, int index)
        {
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            const float d = m.density;
            const float rowH = 40.0f * d;
            const float rowPadding = 4.0f * d;
            const float lineGap = 2.0f * d;
            ImGui::PushID(index);
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const float rowW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
            ImGui::InvisibleButton("##credit_row", ImVec2(rowW, rowH));
            const bool hovered = ImGui::IsItemHovered();
            const ImRect rowRect(rowMin, ImVec2(rowMin.x + rowW, rowMin.y + rowH));
            const float radius = ImGuiM3Radius(ImGuiM3Shape_Small);
            if (hovered)
                ImGuiM3DrawStateLayer(ImGui::GetWindowDrawList(), rowRect,
                                      ImGuiM3ShapeRounding{ radius, radius, radius, radius },
                                      ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);

            const float textX = rowMin.x + rowPadding;
            const float textW = ImMax(1.0f, rowW - rowPadding * 2.0f);
            const float namePx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyMedium).size * d;
            ImFont* nameFont = ImGuiM3FontMedium();
            ImGui::PushFont(nameFont, namePx);
            const char* shownWhat = UI::TruncateLabel(credit.what, textW);
            const ImVec2 whatSize = ImGui::CalcTextSize(shownWhat);
            ImGui::PopFont();
            UI::PushSupport();
            const ImVec2 handleSize = ImGui::CalcTextSize(credit.handle);
            UI::PopSupport();

            const float textH = whatSize.y + lineGap + handleSize.y;
            const float textY = rowMin.y + (rowH - textH) * 0.5f;
            ImGui::GetWindowDrawList()->AddText(
                nameFont, namePx, ImVec2(textX, textY),
                ImGuiM3ColorU32(ImGuiM3Role_OnSurface), shownWhat);

            UI::PushSupport();
            ImGui::SetCursorScreenPos(ImVec2(textX, textY + whatSize.y + lineGap));
            ImGui::SetNextItemAllowOverlap();
            ImGui::TextLinkOpenURL(credit.handle, credit.url);
            const bool linkHovered = ImGui::IsItemHovered();
            UI::PopSupport();
            if (linkHovered)
                ImGui::SetTooltip("%s", credit.url);
            else if (hovered)
                ImGui::SetTooltip("%s", credit.what);
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowRect.Max.y));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::PopID();
        }
    }

    void ImGuiOverlay::renderAboutView()
    {
        const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
        const float d = m.density;
        const char* disclaimer = "This project uses a design heavily inspired by Google's Material 3 / Material You. Not affiliated with or endorsed by Google.";
        const float wrapWidth = ImFloor(ImMax(200.0f, ImGui::GetContentRegionAvail().x) / 8.0f) * 8.0f;
        const float disclaimerHeight = ImGui::CalcTextSize(disclaimer, nullptr, false, wrapWidth).y;
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        const float footerHeight = disclaimerHeight + ImGui::GetTextLineHeight() * 2.5f + spacing * 6.0f + 8.0f;

        ImGui::BeginChild("about_content", ImVec2(0, -footerHeight));
        ImGui::M3CardBegin("about_identity");
        {
            const float logoSize = 56.0f * d;
            const float gap = 14.0f * d;
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float rowW = ImMax(ImGui::GetContentRegionAvail().x, logoSize + gap);
            ImGui::Dummy(ImVec2(rowW, logoSize));
            const ImRect shapeRect(pos, ImVec2(pos.x + logoSize, pos.y + logoSize));
            ImGuiM3DrawShapeMorph(ImGui::GetWindowDrawList(), shapeRect,
                                  ImGuiM3MorphShape_Cookie, ImGuiM3MorphShape_Cookie,
                                  1.0f, ImGuiM3ColorU32(ImGuiM3Role_PrimaryContainer));
            const float markSize = 30.0f * d;
            const ImRect markRect(shapeRect.GetCenter() - ImVec2(markSize, markSize) * 0.5f,
                                  shapeRect.GetCenter() + ImVec2(markSize, markSize) * 0.5f);
            if (titleIconDescriptor != VK_NULL_HANDLE)
                ImGui::GetWindowDrawList()->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(titleIconDescriptor)),
                                                     markRect.Min, markRect.Max);
            else if (ImGuiM3IconFont())
                ImGuiM3DrawIcon(ImGui::GetWindowDrawList(), Icon::WandShineUtf8,
                                markRect, markSize, ImGuiM3ColorU32(ImGuiM3Role_OnPrimaryContainer));

            const float textX = pos.x + logoSize + gap;
            const float titlePx = ImGuiM3GetTypeStyle(ImGuiM3Type_TitleMedium).size * d;
            ImFont* titleFont = ImGuiM3FontBold();
            const float titleH = titleFont->CalcTextSizeA(titlePx, FLT_MAX, 0.0f, "VKIntox").y;
            UI::PushSupport();
            const std::string version = std::string("version ") + runtimeVersion() + "  \xC2\xB7  beta";
            const float versionH = ImGui::CalcTextSize(version.c_str()).y;
            const float textH = titleH + 2.0f * d + versionH;
            const float textY = pos.y + (logoSize - textH) * 0.5f;
            ImGui::GetWindowDrawList()->AddText(titleFont, titlePx, ImVec2(textX, textY),
                                                ImGuiM3ColorU32(ImGuiM3Role_OnSurface), "VKIntox");
            ImGui::GetWindowDrawList()->AddText(ImVec2(textX, textY + titleH + 2.0f * d),
                                                ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant),
                                                version.c_str());
            UI::PopSupport();
        }
        ImGui::M3CardEnd();
        ImGui::Spacing();

        ImGui::M3CardBegin("about_credits", "Credits", Icon::AttributionUtf8);
        static const Credit credits[] = {
            {"VKIntox by", "@buwryy", "https://git.buwryy.net/buwryy"},
            {"MAKO framegen by", "@eugeniosegala", "https://git.buwryy.net/eugeniosegala/MAKO"},
            {"vkShade by", "@slobodaapl", "https://git.buwryy.net/slobodaapl/vkShade"},
            {"vkBasalt by", "@DadSchoorse", "https://git.buwryy.net/DadSchoorse/vkBasalt"},
            {"Overlay fork by", "@Boux", "https://git.buwryy.net/Boux/vkBasalt_overlay"},
            {"Wayland overlay by", "@Daaboulex", "https://git.buwryy.net/Daaboulex/vkBasalt_overlay_wayland"},
            {"ReShade FX support by", "@crosire", "https://git.buwryy.net/crosire/reshade"},
            {"Dear ImGui by", "@ocornut", "https://git.buwryy.net/ocornut/imgui"},
        };
        for (size_t i = 0; i < sizeof(credits) / sizeof(credits[0]); ++i)
            renderCreditRow(credits[i], static_cast<int>(i));
        ImGui::M3CardEnd();

        ImGui::EndChild();

        // Footer, pinned below the content by the reserved height.
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - footerHeight);
        UI::PushSupport();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
        ImGui::TextDisabled("%s", disclaimer);
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::TextDisabled("%s  Report issues:", Icon::BugReportUtf8);
        ImGui::SameLine(0.0f, 6.0f * d);
        ImGui::TextLinkOpenURL("git.buwryy.net/VKIntox/VKIntox/issues",
                               "https://git.buwryy.net/VKIntox/VKIntox/issues");
        UI::PopSupport();
    }

} // namespace VKIntox
