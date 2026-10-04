#ifndef UI_DIALOG_HPP_INCLUDED
#define UI_DIALOG_HPP_INCLUDED

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_m3.h"

namespace VKIntox
{
    namespace UI
    {
        // Shared M3 dialog frame: a padded surface with a bold title. The popup
        // must already have been opened with OpenPopup(id). Returns true while
        // the dialog is open; draw the body and footer, then call EndM3Dialog().
        bool BeginM3Dialog(const char* id, const char* title);
        void EndM3Dialog();

        // Dialog footer action: pill-shaped and a step stronger than M3Button,
        // sized for a dialog rather than a full-height form button.
        bool M3DialogButton(const char* label, ImGuiM3ButtonVariant variant, float width = 0.0f);
    }
} // namespace VKIntox

#endif // UI_DIALOG_HPP_INCLUDED
