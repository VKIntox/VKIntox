#ifndef DEFAULT_CONFIG_HH_INCLUDED
#define DEFAULT_CONFIG_HH_INCLUDED

#include <cstdint>
#include <string>

#include "config_serializer.hh"
#include "vendor/imgui/imgui_m3.h"

// every shipped default lives here so config_serializer and the overlay can
// read them instead of carrying their own literals.
namespace VKIntox::DefaultConfig
{
    // the shipped preset is pink-based Expressive and dark.
    inline constexpr uint32_t kThemeSource = 0xFFE91E63;
    inline constexpr ImGuiM3Variant kThemeVariant = ImGuiM3Variant_Expressive;
    inline constexpr ImGuiM3Contrast kThemeContrast = ImGuiM3Contrast_Medium;
    inline constexpr bool kThemeDark = true;

    inline ImGuiM3BlurSettings blurSettings()
    {
        ImGuiM3BlurSettings blur;
        blur.blur = true;
        blur.background_opacity = 0.70f;
        blur.size = 7.5f;
        blur.passes = 7;
        return blur;
    }

    inline ImGuiM3BackgroundObject backgroundObject()
    {
        ImGuiM3BackgroundObject object;
        object.BackgroundBlur = true;
        return object;
    }

    // depth capture is on by default so depth effects work on a game's first
    // launch instead of needing a restart.
    inline VkBasaltSettings appSettings()
    {
        VkBasaltSettings settings;
        settings.maxEffects = 10;
        settings.overlayBlockInput = true;
        settings.toggleKey = "End";
        settings.reloadKey = "F10";
        settings.overlayKey = "Home";
        settings.enableOnLaunch = true;
        settings.depthCapture = true;
        settings.autoApply = true;
        settings.autoApplyDelay = 200;
        settings.showDebugWindow = false;
        settings.separateEffectParams = true;
        settings.depthResolveMode = 0;
        settings.depthManualPin.clear();
        settings.depthTransientWorkaround = true;
        settings.depthCaptureMethod = 2;
        settings.depthSourceChannel = 0;
        settings.depthInvert = false;
        settings.depthFlip = false;
        settings.depthFlop = false;
        return settings;
    }

    // per-game depth orientation. sober's depth reads inverted; macoblox (mldr)
    // renders upside down, so it starts flipped. flop stays off for every game.
    inline bool depthInvertForGame(const std::string& executableName)
    {
        return executableName == "sober";
    }

    inline bool depthFlipForGame(const std::string& executableName)
    {
        return executableName == "mldr";
    }

    inline void applyGameDefaults(VkBasaltSettings& settings, const std::string& executableName)
    {
        settings.depthInvert = depthInvertForGame(executableName);
        settings.depthFlip = depthFlipForGame(executableName);
        settings.depthFlop = false;
    }
}

#endif // DEFAULT_CONFIG_HH_INCLUDED
