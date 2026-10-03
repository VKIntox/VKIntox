#ifndef SETTINGS_MANAGER_HPP_INCLUDED
#define SETTINGS_MANAGER_HPP_INCLUDED

#include <string>
#include <algorithm>
#include <mutex>

#include "config_serializer.hh"

namespace VKIntox
{
    // Single source of truth for all VKIntox settings.
    // Similar to EffectRegistry for effect parameters.
    //
    // Usage:
    // - Call initialize() once at startup to load from config
    // - Read/write settings directly via getters/setters
    // - Call save() to persist changes to VKIntox.conf
    class SettingsManager
    {
    public:
        // Initialize from VKIntox.conf (call once at startup)
        void initialize();

        // Check if already initialized
        bool isInitialized() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return initialized;
        }

        // Save current settings to VKIntox.conf
        bool save();

        // Getters
        // The string getters return by value: callers on other threads would
        // otherwise hold a reference into a field a setter can reassign.
        int getMaxEffects() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return std::clamp(settings.maxEffects, 1, 200);
        }
        bool getOverlayBlockInput() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return settings.overlayBlockInput;
        }
        std::string getToggleKey() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return settings.toggleKey;
        }
        std::string getReloadKey() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return settings.reloadKey;
        }
        std::string getOverlayKey() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return settings.overlayKey;
        }
        bool getEnableOnLaunch() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return settings.enableOnLaunch;
        }
        bool getDepthCapture() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return settings.depthCapture;
        }
        bool getAutoApply() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return settings.autoApply;
        }
        int getAutoApplyDelay() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return settings.autoApplyDelay;
        }
        bool getShowDebugWindow() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return settings.showDebugWindow;
        }
        int getDepthResolveMode() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return settings.depthResolveMode;
        }
        std::string getDepthManualPin() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return settings.depthManualPin;
        }
        bool getDepthTransientWorkaround() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return settings.depthTransientWorkaround;
        }
        int getDepthCaptureMethod() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return std::clamp(settings.depthCaptureMethod, 0, 2);
        }
        int getDepthSourceChannel() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return std::clamp(settings.depthSourceChannel, 0, 6);
        }
        bool getDepthInvert() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return settings.depthInvert;
        }

        // Setters (update in-memory state, call save() to persist)
        void setMaxEffects(int value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.maxEffects = std::clamp(value, 1, 200);
        }
        void setOverlayBlockInput(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.overlayBlockInput = value;
        }
        void setToggleKey(const std::string& value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.toggleKey = value;
        }
        void setReloadKey(const std::string& value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.reloadKey = value;
        }
        void setOverlayKey(const std::string& value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.overlayKey = value;
        }
        void setEnableOnLaunch(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.enableOnLaunch = value;
        }
        void setDepthCapture(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthCapture = value;
        }
        void setAutoApply(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.autoApply = value;
        }
        void setAutoApplyDelay(int value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.autoApplyDelay = value;
        }
        void setShowDebugWindow(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.showDebugWindow = value;
        }
        void setDepthResolveMode(int value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthResolveMode = value;
        }
        void setDepthManualPin(const std::string& value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthManualPin = value;
        }
        void setDepthTransientWorkaround(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthTransientWorkaround = value;
        }
        void setDepthCaptureMethod(int value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthCaptureMethod = std::clamp(value, 0, 2);
        }
        void setDepthSourceChannel(int value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthSourceChannel = std::clamp(value, 0, 6);
        }
        void setDepthInvert(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthInvert = value;
        }

        // Get raw settings struct (for bulk operations)
        VkBasaltSettings getSettings() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return settings;
        }

    private:
        mutable std::mutex mutex;
        VkBasaltSettings settings;
        bool initialized = false;
    };

    // Global settings manager instance (like effectRegistry)
    extern SettingsManager settingsManager;

} // namespace VKIntox

#endif // SETTINGS_MANAGER_HPP_INCLUDED
