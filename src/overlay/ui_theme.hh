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