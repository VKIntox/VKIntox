#include "settings_manager.hh"
#include "logger.hh"

namespace VKIntox
{
    // Global instance
    SettingsManager settingsManager;

    void SettingsManager::initialize()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (initialized)
            return;

        settings = ConfigSerializer::loadSettings();
        initialized = true;
        Logger::info("SettingsManager initialized");
    }

    bool SettingsManager::save()
    {
        std::lock_guard<std::mutex> lock(mutex);
        bool success = ConfigSerializer::saveSettings(settings);
        if (success)
            Logger::debug("Settings saved to config");
        else
            Logger::err("Failed to save settings");
        return success;
    }

} // namespace VKIntox
