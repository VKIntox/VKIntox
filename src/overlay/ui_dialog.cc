#include "ui_dialog.hh"

#include <cfloat>
#include <unordered_map>

#include "vendor/imgui/imgui_internal.h"

namespace VKIntox
{
    namespace UI
    {
        namespace
        {
            // The dialog appear: slide up from 100dp and fade in over the same
            // curve as the view slide. The container fades and moves with it,
            // and the elevation only shows once the motion has settled.
            constexpr float kAppearSeconds = 0.28f;
            constexpr float kAppearDropDp = 100.0f;
            constexpr float kBezX1 = 0.05f, kBezY1 = 0.90f;
            constexpr float kBezX2 = 0.10f, kBezY2 = 1.06f;

            struct DialogAppear
            {
                float  baseX = 0.0f;
                float  baseY = 0.0f;
                double start = 0.0;
                double lastSeen = 0.0;
            };
            std::unordered_map<ImGuiID, DialogAppear> g_dialogAppear;

            float AppearEase(const DialogAppear& s, double now)
            {
                const float clock = static_cast<float>((now - s.start) / kAppearSeconds);
                if (clock >= 1.0f)
                    return 1.0f;
                return CubicBezierEase(clock, kBezX1, kBezY1, kBezX2, kBezY2);
            }
        }

        float CubicBezierEase(float x, float x1, float y1, float x2, float y2)
        {
            x = ImClamp(x, 0.0f, 1.0f);
            auto bezier = [](float t, float p1, float p2) {
                const float u = 1.0f - t;
                return 3.0f * p1 * t * u * u + 3.0f * p2 * t * t * u + t * t * t;
            };

            float lo = 0.0f, hi = 1.0f;
            for (int i = 0; i < 24; ++i)
            {
                const float mid = (lo + hi) * 0.5f;
                if (bezier(mid, x1, x2) < x)
                    lo = mid;
                else
                    hi = mid;
            }
            return bezier((lo + hi) * 0.5f, y1, y2);
        }

        bool BeginM3Dialog(const char* id, const char* title)
        {
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            const float pad = 24.0f * m.density;
            const double now = ImGui::GetTime();

            // captured before the popup becomes current, so the surface and
            // elevation can be drawn on the parent list (under the popup).
            ImDrawList* parentDraw = ImGui::GetWindowDrawList();
            const ImGuiID popupId = ImGui::GetCurrentWindow()->GetID(id);

            // drop stale entries from popups that closed
            for (auto it = g_dialogAppear.begin(); it != g_dialogAppear.end();)
            {
                if (now - it->second.lastSeen > 2.0)
                    it = g_dialogAppear.erase(it);
                else
                    ++it;
            }

            auto stateIt = g_dialogAppear.find(popupId);
            const float bgAlpha = stateIt != g_dialogAppear.end()
                ? ImClamp(AppearEase(stateIt->second, now), 0.0f, 1.0f) : 1.0f;

            ImVec4 popupBg = ImGui::GetStyle().Colors[ImGuiCol_PopupBg];
            popupBg.w *= bgAlpha;
            ImGui::PushStyleColor(ImGuiCol_PopupBg, popupBg);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, m.dialog_radius * m.density);

            if (!ImGui::BeginPopup(id))
            {
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor();
                g_dialogAppear.erase(popupId);
                return false;
            }

            ImGuiWindow* window = ImGui::GetCurrentWindow();
            if (ImGui::IsWindowAppearing())
            {
                DialogAppear fresh;
                fresh.baseX = window->Pos.x;
                fresh.baseY = window->Pos.y;
                fresh.start = now;
                fresh.lastSeen = now;
                g_dialogAppear[popupId] = fresh;
                stateIt = g_dialogAppear.find(popupId);
            }
            if (stateIt == g_dialogAppear.end())
            {
                DialogAppear fresh;
                fresh.baseX = window->Pos.x;
                fresh.baseY = window->Pos.y;
                fresh.start = now;
                fresh.lastSeen = now;
                g_dialogAppear[popupId] = fresh;
                stateIt = g_dialogAppear.find(popupId);
            }

            DialogAppear& appear = stateIt->second;
            appear.lastSeen = now;
            const float ease = AppearEase(appear, now);
            const float alpha = ImClamp(ease, 0.0f, 1.0f);

            // slide the whole window (its own background is drawn from this
            // position next frame, the content moves with the cursor now)
            ImGui::SetWindowPos(ImVec2(appear.baseX, appear.baseY + kAppearDropDp * m.density * (1.0f - ease)));

            // elevation only once the motion has settled
            if (alpha >= 1.0f)
            {
                const ImVec2 p = ImGui::GetWindowPos();
                const ImVec2 s = ImGui::GetWindowSize();
                const float r = ImMin(m.dialog_radius * m.density, ImMin(s.x, s.y) * 0.5f);
                const ImGuiM3ShapeRounding rounding{ r, r, r, r };
                ImGuiM3DrawElevation(parentDraw, ImRect(p, ImVec2(p.x + s.x, p.y + s.y)), rounding, 3);
            }
            (void)bgAlpha;

            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);

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
            // The alpha var is pushed after BeginPopup, so it has to come off
            // before EndPopup: ImGui records the style-var stack depth when the
            // window begins and asserts it again when it ends.
            ImGui::PopStyleVar();    // alpha
            ImGui::EndPopup();
            ImGui::PopStyleVar(2);   // rounding, padding
            ImGui::PopStyleColor();  // popup background tint
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
                if (actions[i].disabled)
                    ImGui::BeginDisabled();
                if (M3DialogButton(actions[i].label, actions[i].variant, widths[i]))
                    clicked = i;
                if (actions[i].disabled)
                    ImGui::EndDisabled();
            }
            return clicked;
        }
    } // namespace UI
} // namespace VKIntox
