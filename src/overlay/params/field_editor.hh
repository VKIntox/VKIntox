#ifndef FIELD_EDITOR_HPP_INCLUDED
#define FIELD_EDITOR_HPP_INCLUDED

#include "effects/params/effect_param.hh"
#include "../../vendor/imgui/imgui.h"
#include <algorithm>
#include <memory>
#include <map>
#include <functional>

namespace VKIntox
{
    // Base class for field editors
    // Each field type (float, int, bool, float2, etc.) has its own editor
    class FieldEditor
    {
    public:
        virtual ~FieldEditor() = default;

        // Render the field UI, returns true if value changed
        virtual bool render(EffectParam& param) = 0;

        // Reset parameter to default value
        virtual void resetToDefault(EffectParam& param) = 0;
    };

    // Position custom numeric popups beside the pointer and keep them onscreen.
    inline ImVec2 customValuePopupPosition(const ImVec2& cursorPos,
                                           const ImVec2& displayPos,
                                           const ImVec2& displaySize,
                                           const ImVec2& popupSize)
    {
        const float maxX = displaySize.x - popupSize.x;
        const float maxY = displaySize.y - popupSize.y;
        ImVec2 position(cursorPos.x + 12.0f, cursorPos.y + 12.0f);
        if (maxX >= 0.0f)
            position.x = std::min(position.x, displayPos.x + maxX);
        if (maxY >= 0.0f)
            position.y = std::min(position.y, displayPos.y + maxY);
        position.x = std::max(position.x, displayPos.x);
        position.y = std::max(position.y, displayPos.y);
        return position;
    }

    // Factory for creating field editors
    class FieldEditorFactory
    {
    public:
        using CreatorFunc = std::function<std::unique_ptr<FieldEditor>()>;

        static FieldEditorFactory& instance();

        // Register a field editor for a param type
        void registerEditor(ParamType type, CreatorFunc creator);

        // Get editor for a param type (returns nullptr if not found)
        FieldEditor* getEditor(ParamType type);

    private:
        FieldEditorFactory() = default;
        std::map<ParamType, std::unique_ptr<FieldEditor>> editors;
        std::map<ParamType, CreatorFunc> creators;
    };

    // Helper macro for auto-registration
    #define REGISTER_FIELD_EDITOR(ParamTypeValue, EditorClass) \
        namespace { \
            static bool _registered_##EditorClass = []() { \
                FieldEditorFactory::instance().registerEditor( \
                    ParamTypeValue, \
                    []() { return std::make_unique<EditorClass>(); }); \
                return true; \
            }(); \
        }

    // Main entry point - renders the appropriate editor for a parameter
    bool renderFieldEditor(EffectParam& param);

} // namespace VKIntox

#endif // FIELD_EDITOR_HPP_INCLUDED
