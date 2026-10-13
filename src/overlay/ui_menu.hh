#ifndef UI_MENU_HPP_INCLUDED
#define UI_MENU_HPP_INCLUDED

#include "vendor/imgui/imgui.h"

namespace VKIntox
{
    namespace UI
    {
        // One menu surface for every dropdown and context menu so a popup
        // opened from a value looks and behaves like a combo's: the theme's
        // menu insets and corner, and rows drawn with M3MenuItem.
        bool BeginM3Menu(const char* id);
        bool BeginM3MenuContext(const char* id);
        bool M3MenuItem(const char* label, bool selected = false);
        // Preset-style row: a leading dot with the name centred. Used by the
        // shader INI dropdown; the plain M3MenuItem keeps the default layout.
        bool M3MenuRadioItem(const char* label, bool selected = false);
        void EndM3Menu();

        // Combo wrapper: caps the popup height so long lists scroll inside the
        // overlay. Draw rows with M3MenuItem, then EndM3Combo().
        bool BeginM3Combo(const char* label, const char* preview);
        void EndM3Combo();

        // Clip a label to a character budget, ending with an ellipsis if it was
        // shortened. A safe ceiling for names that would otherwise overrun the
        // narrow menu/combo surfaces. Returns a view into a thread-local buffer.
        const char* TruncateLabel(const char* label, size_t maxChars = 22);
    }
} // namespace VKIntox

#endif // UI_MENU_HPP_INCLUDED
