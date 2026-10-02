#include "config_serializer.hh"
#include "config_paths.hh"
#include "c_resource.hh"
#include "logger.hh"

#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <sys/stat.h>
#include <dirent.h>
#include <algorithm>
#include <array>
#include <set>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <climits>
#include <cerrno>
#include <cctype>

namespace VKIntox
{
    namespace
    {
        constexpr int MIN_MAX_EFFECTS = 1;
        constexpr int MAX_MAX_EFFECTS = 200;

        bool writeAtomically(const std::string& path, const std::string& contents)
        {
            std::string temporary = path + ".tmp-XXXXXX";
            std::vector<char> name(temporary.begin(), temporary.end());
            name.push_back('\0');
            // mkstemp rewrites name in place, so the real path only exists after
            // the call. The owner adopts the returned descriptor and the name is
            // read back out afterwards, which is also what guarantees the
            // unlink-on-failure below targets the file that was actually created.
            UniqueFd fd(mkstemp(name.data()));
            if (!fd)
                return false;
            size_t offset = 0;
            bool success = true;
            while (offset < contents.size())
            {
                const ssize_t count = write(fd.get(), contents.data() + offset, contents.size() - offset);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                {
                    success = false;
                    break;
                }
                offset += static_cast<size_t>(count);
            }
            if (success && fsync(fd.get()) != 0)
                success = false;
            // the close result is part of the answer, not cleanup noise: with
            // deferred writeback a full or failed disk only reports itself here,
            // so discarding it would report success for a truncated config
            if (!fd.close())
                success = false;
            if (success && std::rename(name.data(), path.c_str()) == 0)
                return true;
            unlink(name.data());
            return false;
        }
    } // namespace

    std::string ConfigSerializer::getBaseConfigDir()
    {
        const char* xdgConfig = std::getenv("XDG_CONFIG_HOME");
        if (xdgConfig)
            return std::string(xdgConfig) + "/VKIntox";

        const char* home = std::getenv("HOME");
        if (home)
            return std::string(home) + "/.config/VKIntox";

        return "";
    }

    std::string ConfigSerializer::getConfigsDir()
    {
        std::string baseDir = getBaseConfigDir();
        if (baseDir.empty())
            return "";
        return baseDir + "/configs";
    }

    std::vector<std::string> ConfigSerializer::listConfigs()
    {
        std::vector<std::string> configs;
        std::string dir = getConfigsDir();

        UniqueDir d(opendir(dir.c_str()));
        if (!d)
            return configs;

        struct dirent* entry;
        while ((entry = readdir(d.get())) != nullptr)
        {
            std::string name = entry->d_name;
            if (name.size() > 5 && name.substr(name.size() - 5) == ".conf")
                configs.push_back(name.substr(0, name.size() - 5));
        }

        std::sort(configs.begin(), configs.end());
        return configs;
    }

    static std::string joinEffects(const std::vector<std::string>& effects)
    {
        std::string result;
        for (size_t i = 0; i < effects.size(); i++)
        {
            if (i > 0)
                result += ":";
            result += effects[i];
        }
        return result;
    }

    bool ConfigSerializer::saveConfig(
        const std::string& configName,
        const std::vector<std::string>& effects,
        const std::vector<std::string>& disabledEffects,
        const std::vector<ConfigParam>& params,
        const std::map<std::string, std::string>& effectPaths,
        const std::vector<PreprocessorDefinition>& preprocessorDefs)
    {
        std::string configsDir = getConfigsDir();
        if (configsDir.empty())
        {
            Logger::err("Could not determine configs directory");
            return false;
        }

        mkdir(configsDir.c_str(), 0755);

        std::string filePath = configsDir + "/" + configName + ".conf";
        bool result = saveToPath(filePath, effects, disabledEffects, params, effectPaths, preprocessorDefs);
        if (result)
            Logger::info("Saved config to: " + filePath);
        return result;
    }

    bool ConfigSerializer::deleteConfig(const std::string& configName)
    {
        std::string configsDir = getConfigsDir();
        if (configsDir.empty())
            return false;

        std::string filePath = configsDir + "/" + configName + ".conf";
        if (std::remove(filePath.c_str()) == 0)
        {
            Logger::info("Deleted config: " + filePath);
            return true;
        }
        Logger::err("Failed to delete config: " + filePath);
        return false;
    }

    std::string ConfigSerializer::getDefaultConfigPath()
    {
        const char* home = std::getenv("HOME");
        if (home)
            return std::string(home) + "/.config/VKIntox/default_config";
        return "";
    }

    bool ConfigSerializer::setDefaultConfig(const std::string& configName)
    {
        std::string path = getDefaultConfigPath();
        if (path.empty())
            return false;

        std::ofstream file(path);
        if (!file.is_open())
        {
            Logger::err("Could not write default config file: " + path);
            return false;
        }

        file << configName;
        file.close();
        Logger::info("Set default config: " + configName);
        return true;
    }

    std::string ConfigSerializer::getDefaultConfig()
    {
        std::string path = getDefaultConfigPath();
        if (path.empty())
            return "";

        std::ifstream file(path);
        if (!file.is_open())
            return "";

        std::string configName;
        std::getline(file, configName);
        return configName;
    }

    VkBasaltSettings ConfigSerializer::loadSettings()
    {
        VkBasaltSettings settings;

        // Check user config first, then fall back to system paths (same order as Config)
        std::string userConfig = getBaseConfigDir() + "/VKIntox.conf";

        const std::array<std::string, 3> configPaths = {
            userConfig,
            std::string(SYSCONFDIR) + "/VKIntox/VKIntox.conf",
            std::string(SYSCONFDIR) + "/VKIntox.conf",
        };

        std::string configPath;
        for (const auto& path : configPaths)
        {
            std::ifstream test(path);
            if (test.is_open())
            {
                configPath = path;
                break;
            }
        }

        if (configPath.empty())
            return settings;

        Logger::info("SettingsManager loading from: " + configPath);

        std::ifstream file(configPath);
        if (!file.is_open())
            return settings;

        std::string line;
        while (std::getline(file, line))
        {
            // Skip comments and empty lines
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] == '#')
                continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            // Trim whitespace
            auto trimWs = [](std::string& s) {
                size_t start = s.find_first_not_of(" \t");
                size_t end = s.find_last_not_of(" \t");
                s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
            };
            trimWs(key);
            trimWs(value);

            if (key == "maxEffects")
            {
                try
                {
                    int parsed = std::stoi(value);
                    if (parsed < 0)
                    {
                        Logger::err("invalid maxEffects value (negative): " + value);
                    }
                    else
                    {
                        settings.maxEffects = std::clamp(parsed, MIN_MAX_EFFECTS, MAX_MAX_EFFECTS);
                        if (settings.maxEffects != parsed)
                        {
                            Logger::warn("clamped maxEffects from " + std::to_string(parsed) + " to "
                                         + std::to_string(settings.maxEffects));
                        }
                    }
                }
                catch (...)
                {
                    Logger::err("invalid maxEffects value: " + value);
                }
            }
            else if (key == "overlayBlockInput")
                settings.overlayBlockInput = (value == "true" || value == "1");
            else if (key == "toggleKey")
                settings.toggleKey = value;
            else if (key == "reloadKey")
                settings.reloadKey = value;
            else if (key == "overlayKey")
                settings.overlayKey = value;
            else if (key == "enableOnLaunch")
            {
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                settings.enableOnLaunch = (value == "true" || value == "1" || value == "on");
            }
            else if (key == "depthCapture")
                settings.depthCapture = (value == "on");
            else if (key == "autoApply")
                settings.autoApply = (value == "true" || value == "1");
            else if (key == "autoApplyDelay")
            {
                try { settings.autoApplyDelay = std::stoi(value); }
                catch (...) { Logger::err("invalid autoApplyDelay value: " + value); }
            }
            else if (key == "showDebugWindow")
                settings.showDebugWindow = (value == "true" || value == "1");
            else if (key == "depthResolveMode")
            {
                try { settings.depthResolveMode = std::stoi(value); }
                catch (...) { Logger::err("invalid depthResolveMode value: " + value); }
            }
            else if (key == "depthManualPin")
                settings.depthManualPin = value;
            else if (key == "depthTransientWorkaround")
                settings.depthTransientWorkaround = (value == "true" || value == "1");
            else if (key == "depthCaptureMethod")
            {
                try
                {
                    int parsed = std::stoi(value);
                    settings.depthCaptureMethod = std::clamp(parsed, 0, 2);
                }
                catch (...) { Logger::err("invalid depthCaptureMethod value: " + value); }
            }
            else if (key == "depthSourceChannel")
            {
                try
                {
                    int parsed = std::stoi(value);
                    settings.depthSourceChannel = std::clamp(parsed, 0, 6); // Support modes 0-6
                }
                catch (...) { Logger::err("invalid depthSourceChannel value: " + value); }
            }
            else if (key == "depthInvert")
                settings.depthInvert = (value == "true" || value == "1");
        }

        return settings;
    }

    bool ConfigSerializer::saveSettings(const VkBasaltSettings& settings)
    {
        std::string baseDir = getBaseConfigDir();
        if (baseDir.empty())
        {
            Logger::err("Could not determine config directory");
            return false;
        }

        mkdir(baseDir.c_str(), 0755);

        std::string configPath = baseDir + "/VKIntox.conf";
        std::ofstream file(configPath);
        if (!file.is_open())
        {
            Logger::err("Could not open VKIntox.conf for writing: " + configPath);
            return false;
        }

        // Write settings with comments
        file << "# VKIntox configuration\n\n";

        file << "# Overlay settings\n";
        const int clampedMaxEffects = std::clamp(settings.maxEffects, MIN_MAX_EFFECTS, MAX_MAX_EFFECTS);

        file << "overlayBlockInput = " << (settings.overlayBlockInput ? "true" : "false") << "\n";
        file << "maxEffects = " << clampedMaxEffects << "\n";
        file << "autoApply = " << (settings.autoApply ? "true" : "false") << "\n";
        file << "autoApplyDelay = " << settings.autoApplyDelay << "\n";

        file << "\n# Key bindings\n";
        file << "toggleKey = " << settings.toggleKey << "\n";
        file << "reloadKey = " << settings.reloadKey << "\n";
        file << "overlayKey = " << settings.overlayKey << "\n";

        file << "\n# Startup behavior\n";
        file << "enableOnLaunch = " << (settings.enableOnLaunch ? "true" : "false") << "\n";
        file << "depthCapture = " << (settings.depthCapture ? "on" : "off") << "\n";

        file << "\n# Debug\n";
        file << "showDebugWindow = " << (settings.showDebugWindow ? "true" : "false") << "\n";

        file << "\n# Advanced (depth buffer)\n";
        file << "# depthResolveMode: 0=auto (prefer average), 1=sample-zero, 2=average\n";
        file << "depthResolveMode = " << settings.depthResolveMode << "\n";
        file << "depthManualPin = " << settings.depthManualPin << "\n";
        file << "depthTransientWorkaround = " << (settings.depthTransientWorkaround ? "true" : "false") << "\n";
        file << "# depthCaptureMethod: 0=off, 1=renderpass-end (recommended), 2=queue-submit (robust fallback)\n";
        file << "depthCaptureMethod = " << settings.depthCaptureMethod << "\n";

        file << "\n# Alternative depth buffer handling\n";
        file << "# depthSourceChannel mode selection:\n";
        file << "#   0 = Luminance/Red (standard Vulkan depth)\n";
        file << "#   1 = Alpha (alpha-encoded depth)\n";
        file << "#   2 = Packed RGB (depth in RGB channels)\n";
        file << "#   3 = Logarithmic (log-encoded depth)\n";
        file << "#   4 = View-space Z (raw view-space Z)\n";
        file << "#   5 = NDC (Normalized Device Coordinates)\n";
        file << "#   6 = Reversed-Z (inverted for precision)\n";
        file << "depthSourceChannel = " << settings.depthSourceChannel << "\n";
        file << "# depthInvert: invert depth values (flips near/far planes)\n";
        file << "depthInvert = " << (settings.depthInvert ? "true" : "false") << "\n";

        file.close();
        Logger::info("Saved settings to: " + configPath);
        return true;
    }

    void ConfigSerializer::ensureConfigExists()
    {
        std::string baseDir = getBaseConfigDir();
        if (baseDir.empty())
            return;

        // Create directory if needed
        mkdir(baseDir.c_str(), 0755);

        std::string configPath = baseDir + "/VKIntox.conf";

        // Only create if no user config exists
        struct stat st;
        if (stat(configPath.c_str(), &st) == 0)
            return;

        VkBasaltSettings defaults;
        saveSettings(defaults);
        Logger::info("Created default VKIntox.conf");
    }

    std::string ConfigSerializer::detectGameName()
    {
        char buf[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len <= 0)
            return "";

        buf[len] = '\0';
        std::string exePath(buf);

        // Extract basename
        size_t lastSlash = exePath.rfind('/');
        if (lastSlash == std::string::npos)
            return exePath;

        return exePath.substr(lastSlash + 1);
    }

    std::string ConfigSerializer::autoDetectConfig()
    {
        std::string gameName = detectGameName();
        if (gameName.empty())
            return "";

        std::string configsDir = getConfigsDir();
        if (configsDir.empty())
            return "";

        std::string configPath = configsDir + "/" + gameName + ".conf";
        struct stat st;
        if (stat(configPath.c_str(), &st) != 0)
            return "";

        Logger::info("Auto-loaded config for: " + gameName);
        return gameName;
    }

    // Case-insensitive string comparison helper
    static bool equalsIgnoreCaseLocal(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++)
        {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    }

    // Scan a directory recursively for Shaders/ and Textures/ subdirectories.
    // For each Shaders/ folder we also add its parent, so shaders that live in
    // a subfolder and `#include "ReShade.fxh"` (with the .fxh sitting next to
    // the Shaders/ directory) resolve correctly. Without the parent entry the
    // preprocessor only knows about the Shaders/ subfolder itself.
    static void scanDirectoryForShaders(
        const std::string& dir,
        std::vector<std::string>& shaderPaths,
        std::vector<std::string>& texturePaths)
    {
        try
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                dir, std::filesystem::directory_options::skip_permission_denied))
            {
                if (!entry.is_directory())
                    continue;

                std::string dirName = entry.path().filename().string();
                if (equalsIgnoreCaseLocal(dirName, "Shaders"))
                {
                    shaderPaths.push_back(entry.path().string());
                    // Add the parent directory too — this is where shared
                    // headers like ReShade.fxh typically live. De-duplicated
                    // below so we don't accumulate duplicates across packages.
                    std::string parent = entry.path().parent_path().string();
                    if (std::find(shaderPaths.begin(), shaderPaths.end(), parent) == shaderPaths.end())
                        shaderPaths.push_back(parent);
                }
                else if (equalsIgnoreCaseLocal(dirName, "Textures"))
                {
                    texturePaths.push_back(entry.path().string());
                }
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            Logger::err("Error scanning directory " + dir + ": " + e.what());
        }
    }

    ShaderManagerConfig ConfigSerializer::loadShaderManagerConfig()
    {
        static ShaderManagerConfig cachedConfig;
        static std::filesystem::file_time_type cachedModifiedTime;
        static std::string cachedConfigPath;
        static bool cacheValid = false;

        ShaderManagerConfig config;
        std::string configPath = getBaseConfigDir() + "/shader_manager.conf";

        std::error_code timestampError;
        auto modifiedTime = std::filesystem::last_write_time(configPath, timestampError);
        if (cacheValid && !timestampError && configPath == cachedConfigPath &&
            modifiedTime == cachedModifiedTime)
            return cachedConfig;

        auto rememberConfig = [&]() {
            std::error_code ec;
            auto time = std::filesystem::last_write_time(configPath, ec);
            if (!ec)
            {
                cachedConfig = config;
                cachedConfigPath = configPath;
                cachedModifiedTime = time;
                cacheValid = true;
            }
        };

        auto refreshDiscoveredPaths = [&]() {
            std::set<std::string> shaderPaths;
            std::set<std::string> texturePaths;

            auto addExistingPaths = [](const std::vector<std::string>& paths, std::set<std::string>& output) {
                for (const auto& path : paths)
                {
                    std::error_code ec;
                    if (std::filesystem::is_directory(path, ec))
                        output.insert(std::filesystem::path(path).lexically_normal().string());
                }
            };

            addExistingPaths(config.discoveredShaderPaths, shaderPaths);
            addExistingPaths(config.discoveredTexturePaths, texturePaths);

            // Refresh paths on load so newly installed or moved shader packs
            // are available immediately, without requiring the UI rescan.
            for (const auto& parentDir : config.parentDirectories)
            {
                std::error_code ec;
                if (!std::filesystem::is_directory(parentDir, ec))
                    continue;

                std::vector<std::string> foundShaders;
                std::vector<std::string> foundTextures;
                scanDirectoryForShaders(parentDir, foundShaders, foundTextures);
                addExistingPaths(foundShaders, shaderPaths);
                addExistingPaths(foundTextures, texturePaths);
            }

            config.discoveredShaderPaths.assign(shaderPaths.begin(), shaderPaths.end());
            config.discoveredTexturePaths.assign(texturePaths.begin(), texturePaths.end());
        };

        std::ifstream file(configPath);
        if (!file.is_open())
        {
            // Config file doesn't exist - set up defaults
            std::string defaultReshadeDir = getBaseConfigDir() + "/reshade";

            // Create directories if they don't exist
            mkdir(defaultReshadeDir.c_str(), 0755);
            mkdir((defaultReshadeDir + "/Shaders").c_str(), 0755);
            mkdir((defaultReshadeDir + "/Textures").c_str(), 0755);

            config.parentDirectories.push_back(defaultReshadeDir);

            // Auto-scan to discover paths
            scanDirectoryForShaders(defaultReshadeDir,
                config.discoveredShaderPaths, config.discoveredTexturePaths);

            refreshDiscoveredPaths();

            // Save the config so it persists
            saveShaderManagerConfig(config);
            Logger::info("Created default shader manager config with reshade directory");
            rememberConfig();
            return config;
        }

        // File exists - parse it. Older setup versions wrote all discovered
        // paths on one comma-separated line, so accept both that format and
        // the current one-path-per-line format.
        auto appendPathList = [](const std::string& value, std::vector<std::string>& paths) {
            std::stringstream stream(value);
            std::string path;
            while (std::getline(stream, path, ','))
            {
                size_t start = path.find_first_not_of(" \t");
                size_t end = path.find_last_not_of(" \t");
                if (start != std::string::npos)
                    paths.push_back(path.substr(start, end - start + 1));
            }
        };

        std::string line;
        while (std::getline(file, line))
        {
            // Skip comments and empty lines
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] == '#')
                continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            // Trim whitespace
            auto trimWs = [](std::string& s) {
                size_t start = s.find_first_not_of(" \t");
                size_t end = s.find_last_not_of(" \t");
                s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
            };
            trimWs(key);
            trimWs(value);

            if (key == "parentDir" && !value.empty())
                config.parentDirectories.push_back(value);
            else if (key == "shaderPath" && !value.empty())
                appendPathList(value, config.discoveredShaderPaths);
            else if (key == "texturePath" && !value.empty())
                appendPathList(value, config.discoveredTexturePaths);
        }

        const auto previousShaderPaths = config.discoveredShaderPaths;
        const auto previousTexturePaths = config.discoveredTexturePaths;
        refreshDiscoveredPaths();
        if (config.discoveredShaderPaths != previousShaderPaths ||
            config.discoveredTexturePaths != previousTexturePaths)
        {
            saveShaderManagerConfig(config);
            Logger::info("ShaderManager: refreshed discovered shader and texture paths");
        }

        rememberConfig();
        return config;
    }

    bool ConfigSerializer::saveShaderManagerConfig(const ShaderManagerConfig& config)
    {
        std::string baseDir = getBaseConfigDir();
        if (baseDir.empty())
        {
            Logger::err("Could not determine config directory");
            return false;
        }

        mkdir(baseDir.c_str(), 0755);

        std::string configPath = baseDir + "/shader_manager.conf";
        std::ofstream file(configPath);
        if (!file.is_open())
        {
            Logger::err("Could not open shader_manager.conf for writing: " + configPath);
            return false;
        }

        file << "# Shader Manager configuration\n";
        file << "# Parent directories are scanned recursively for Shaders/ and Textures/ subdirs\n\n";

        file << "# Parent directories (user-added)\n";
        for (const auto& dir : config.parentDirectories)
            file << "parentDir = " << dir << "\n";

        file << "\n# Discovered shader paths (auto-generated on scan)\n";
        for (const auto& path : config.discoveredShaderPaths)
            file << "shaderPath = " << path << "\n";

        file << "\n# Discovered texture paths (auto-generated on scan)\n";
        for (const auto& path : config.discoveredTexturePaths)
            file << "texturePath = " << path << "\n";

        file.close();
        Logger::info("Saved shader manager config to: " + configPath);
        return true;
    }

    // --- Per-app profile system ---

    std::string ConfigSerializer::getProfilePath(const std::string& gameName)
    {
        std::string configsDir = getConfigsDir();
        if (configsDir.empty() || gameName.empty() ||
            gameName.find('/') != std::string::npos || gameName.find('\\') != std::string::npos ||
            gameName == "." || gameName == "..")
            return "";

        return configsDir + "/" + gameName + ".conf";
    }

    std::string ConfigSerializer::ensureGameProfile(const std::string& gameName)
    {
        if (gameName.empty())
            return "";

        std::string configsDir = getConfigsDir();
        if (configsDir.empty())
            return "";

        mkdir(configsDir.c_str(), 0755);

        const std::string profilePath = getProfilePath(gameName);
        if (profilePath.empty())
            return "";

        struct stat st;
        if (stat(profilePath.c_str(), &st) == 0)
        {
            Logger::info("Found existing profile for " + gameName);
            return profilePath;
        }

        // Move configs created by the experimental <game>@default.conf naming
        // to the canonical <game>.conf path. Both files are in the same
        // directory, so rename is atomic on the filesystem.
        const std::string previousPath = configsDir + "/" + gameName + "@default.conf";
        if (stat(previousPath.c_str(), &st) == 0)
        {
            if (std::rename(previousPath.c_str(), profilePath.c_str()) != 0)
            {
                Logger::err("Could not rename game config " + previousPath + " to " + profilePath);
                return previousPath;
            }
            Logger::info("Renamed game config " + previousPath + " to " + profilePath);
            return profilePath;
        }

        // Create default profile with empty effects
        std::ofstream file(profilePath);
        if (!file.is_open())
        {
            Logger::err("Could not create profile for " + gameName + ": " + profilePath);
            return "";
        }

        file << "# VKIntox profile for " << gameName << "\n";
        file << "# Auto-created on first launch\n\n";
        file << "effects = \n";

        file.close();
        Logger::info("Created default profile for " + gameName + ": " + profilePath);

        return profilePath;
    }

    std::string ConfigSerializer::getShaderProfilePath(const std::string& gameName, const std::string& profileName)
    {
        const std::string base = getBaseConfigDir();
        if (base.empty() || gameName.empty() || profileName.empty() ||
            gameName.find('/') != std::string::npos || gameName.find('\\') != std::string::npos ||
            gameName == "." || gameName == ".." || profileName.find('/') != std::string::npos ||
            profileName.find('\\') != std::string::npos || profileName == "." || profileName == "..")
            return "";
        const std::string shaderDir = base + "/configs/shaders/";
        const std::string gameProfilePath = shaderDir + gameName + "@" + profileName + ".ini";
        const std::string importedPresetPath = shaderDir + profileName + ".ini";
        std::error_code ec;
        if (std::filesystem::exists(gameProfilePath, ec))
            return gameProfilePath;
        ec.clear();
        if (std::filesystem::exists(importedPresetPath, ec))
            return importedPresetPath;
        return gameProfilePath;
    }

    namespace
    {
        std::string getGameShaderProfilePath(const std::string& gameName, const std::string& profileName)
        {
            const std::string base = ConfigSerializer::getBaseConfigDir();
            if (base.empty() || gameName.empty() || profileName.empty() ||
                gameName.find('/') != std::string::npos || gameName.find('\\') != std::string::npos ||
                profileName.find('/') != std::string::npos || profileName.find('\\') != std::string::npos ||
                gameName == "." || gameName == ".." || profileName == "." || profileName == "..")
                return "";
            return base + "/configs/shaders/" + gameName + "@" + profileName + ".ini";
        }
    }

    std::vector<std::string> ConfigSerializer::listShaderProfilesForGame(const std::string& gameName)
    {
        std::vector<std::string> profiles;
        const std::string base = getBaseConfigDir();
        if (base.empty() || gameName.empty())
            return profiles;
        const std::string dir = base + "/configs/shaders";
        UniqueDir d(opendir(dir.c_str()));
        if (!d)
            return profiles;
        const std::string prefix = gameName + "@";
        struct dirent* entry;
        while ((entry = readdir(d.get())) != nullptr)
        {
            const std::string name = entry->d_name;
            if (name.size() <= 4 || name.substr(name.size() - 4) != ".ini")
                continue;
            if (name.compare(0, prefix.size(), prefix) == 0)
                profiles.push_back(name.substr(prefix.size(), name.size() - prefix.size() - 4));
            else if (name.find('@') == std::string::npos)
                profiles.push_back(name.substr(0, name.size() - 4));
        }
        std::sort(profiles.begin(), profiles.end());
        profiles.erase(std::unique(profiles.begin(), profiles.end()), profiles.end());
        return profiles;
    }

    bool ConfigSerializer::setLastShaderProfile(const std::string& gameName, const std::string& profileName)
    {
        if (gameName.empty() || profileName.empty() || gameName.find('/') != std::string::npos ||
            gameName.find('\\') != std::string::npos || profileName.find('/') != std::string::npos ||
            profileName.find('\\') != std::string::npos)
            return false;
        const auto path = getBaseConfigDir() + "/configs/shaders/" + gameName + ".last-profile";
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        if (ec)
            return false;
        return writeAtomically(path, profileName + "\n");
    }

    std::string ConfigSerializer::getLastShaderProfile(const std::string& gameName)
    {
        if (gameName.empty() || gameName.find('/') != std::string::npos || gameName.find('\\') != std::string::npos)
            return "";
        std::ifstream file(getBaseConfigDir() + "/configs/shaders/" + gameName + ".last-profile");
        std::string profileName;
        std::getline(file, profileName);
        return profileName;
    }

    bool ConfigSerializer::createShaderProfile(const std::string& gameName, const std::string& profileName,
                                               const std::string& copyFromProfile)
    {
        // Creation always targets a game-specific file. The regular lookup
        // intentionally falls back to imported global presets for selection.
        const std::string path = getGameShaderProfilePath(gameName, profileName);
        if (path.empty() || profileName.find('/') != std::string::npos || profileName.find('\\') != std::string::npos)
            return false;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        if (ec)
            return false;

        std::string contents = "Techniques=\nTechniqueSorting=\n";
        if (!copyFromProfile.empty())
        {
            const std::string sourcePath = getShaderProfilePath(gameName, copyFromProfile);
            std::ifstream source(sourcePath, std::ios::binary);
            if (sourcePath.empty() || !source)
                return false;
            contents.assign(std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>());
            if (source.bad())
                return false;
        }

        UniqueFd fd(open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644));
        if (!fd)
            return false;
        size_t offset = 0;
        bool success = true;
        while (offset < contents.size())
        {
            const ssize_t written = write(fd.get(), contents.data() + offset, contents.size() - offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
            {
                success = false;
                break;
            }
            offset += static_cast<size_t>(written);
        }
        if (!fd.close())
            success = false;
        if (!success)
        {
            unlink(path.c_str());
        }
        return success;
    }

    bool ConfigSerializer::deleteShaderProfile(const std::string& gameName, const std::string& profileName)
    {
        const std::string path = getShaderProfilePath(gameName, profileName);
        return !path.empty() && std::remove(path.c_str()) == 0;
    }

    bool ConfigSerializer::saveShaderProfile(const std::string& path, const std::vector<ConfigParam>& params,
                                             const std::vector<std::string>& effects,
                                             const std::vector<std::string>& disabledEffects,
                                             const std::map<std::string, std::string>& effectPaths,
                                             const std::vector<std::string>& enabledTechniques,
                                             const std::vector<std::string>& techniqueSorting,
                                             const std::vector<ConfigParam>& disabledEffectParams)
    {
        if (path.empty())
            return false;
        std::error_code ec;
        const auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec);
        if (ec)
            return false;
        auto sectionFor = [&effectPaths](const std::string& effectName) {
            const auto it = effectPaths.find(effectName);
            if (it != effectPaths.end() && std::filesystem::path(it->second).extension() == ".fx")
                // ReShade looks up preset sections by the shader filename.
                // Using VKIntox instance names here makes imported presets lose
                // their parameter values in ReShade.
                return std::filesystem::path(it->second).filename().string();
            return effectName;
        };
        auto mergeParams = [&sectionFor](const std::vector<ConfigParam>& source) {
            std::map<std::pair<std::string, std::string>, std::string> merged;
            for (const auto& p : source)
                merged[{sectionFor(p.effectName), p.paramName}] = p.value;
            return merged;
        };
        const auto merged = mergeParams(params);
        const auto mergedDisabledParams = mergeParams(disabledEffectParams);
        std::set<std::string> disabled(disabledEffects.begin(), disabledEffects.end());
        std::vector<std::string> techniques = enabledTechniques;
        std::vector<std::string> sortedTechniques = techniqueSorting;
        if (techniques.empty())
        {
            for (const auto& effect : effects)
            {
                if (disabled.count(effect))
                    continue;
                const std::string section = sectionFor(effect);
                if (std::filesystem::path(section).extension() == ".fx")
                    techniques.push_back(std::filesystem::path(section).stem().string() + "@" + section);
            }
        }
        if (sortedTechniques.empty())
            sortedTechniques = techniques;
        auto deduplicate = [](std::vector<std::string>& values) {
            std::set<std::string> seen;
            values.erase(std::remove_if(values.begin(), values.end(), [&seen](const std::string& value) {
                return !seen.insert(value).second;
            }), values.end());
        };
        deduplicate(techniques);
        deduplicate(sortedTechniques);
        std::ostringstream file;
        file << "Techniques=";
        for (size_t i = 0; i < techniques.size(); ++i)
        {
            if (i) file << ',';
            file << techniques[i];
        }
        file << "\nTechniqueSorting=";
        for (size_t i = 0; i < sortedTechniques.size(); ++i)
        {
            if (i) file << ',';
            file << sortedTechniques[i];
        }
        file << "\n\n";
        std::map<std::pair<std::string, std::string>, std::string> outputValues;
        std::map<std::pair<std::string, std::string>, std::map<size_t, std::string>> vectorValues;
        for (const auto& [key, value] : merged)
        {
            if (!key.second.empty() && key.second.front() == '@')
                continue;
            const auto open = key.second.rfind('[');
            if (open != std::string::npos && key.second.back() == ']')
            {
                try
                {
                    const size_t component = std::stoul(key.second.substr(open + 1, key.second.size() - open - 2));
                    vectorValues[{key.first, key.second.substr(0, open)}][component] = value;
                    continue;
                }
                catch (...) {}
            }
            outputValues[key] = value;
        }
        for (const auto& [key, components] : vectorValues)
        {
            std::string value;
            for (const auto& [index, component] : components)
            {
                if (!value.empty()) value += ", ";
                value += component;
            }
            outputValues[key] = value;
        }
        std::map<std::string, std::map<std::string, std::string>> preprocessorValues;
        for (const auto& [key, value] : merged)
            if (!key.second.empty() && key.second.front() == '@')
                preprocessorValues[key.first][key.second.substr(1)] = value;
        auto writePreprocessorDefinitions = [&file](const std::map<std::string, std::string>& macros) {
            file << "PreprocessorDefinitions=";
            bool first = true;
            for (const auto& [name, macroValue] : macros)
            {
                if (!first) file << ", ";
                file << name << '=';
                for (const char c : macroValue)
                {
                    file << c;
                    if (c == ',')
                        file << ',';
                }
                first = false;
            }
            file << '\n';
        };
        const auto globalMacros = preprocessorValues.find("");
        if (globalMacros != preprocessorValues.end())
        {
            writePreprocessorDefinitions(globalMacros->second);
            preprocessorValues.erase(globalMacros);
        }
        std::string current;
        for (const auto& [key, value] : outputValues)
        {
            if (key.first != current)
            {
                file << '\n';
                current = key.first;
                file << "[" << current << "]\n";
                const auto macros = preprocessorValues.find(current);
                if (macros != preprocessorValues.end())
                {
                    writePreprocessorDefinitions(macros->second);
                    preprocessorValues.erase(macros);
                }
            }
            file << key.second << "=" << value << "\n";
        }
        if (!current.empty())
            file << "\n";
        for (const auto& [section, macros] : preprocessorValues)
        {
            file << "[" << section << "]\n";
            writePreprocessorDefinitions(macros);
            file << "\n";
        }
        if (!file.good() || !writeAtomically(path, file.str()))
            return false;

        // Preserve values for unchecked effects separately from the preset
        // ReShade consumes. Appending to the original filename keeps the
        // sidecar adjacent and stable even for presets with non-INI extensions.
        const auto presetPath = std::filesystem::path(path);
        const std::string disabledValuesPath = (presetPath.parent_path() /
            ("." + presetPath.filename().string() + "_disabled-effectvalues")).string();
        if (mergedDisabledParams.empty())
        {
            std::remove(disabledValuesPath.c_str());
            return true;
        }

        std::ostringstream disabledFile;
        for (const auto& [key, value] : mergedDisabledParams)
        {
            if (!key.second.empty() && key.second.front() == '@')
            {
                disabledFile << '[' << key.first << "]\nPreprocessorDefinitions="
                             << key.second.substr(1) << '=' << value << "\n\n";
            }
            else
                disabledFile << '[' << key.first << "]\n" << key.second << '=' << value << "\n\n";
        }
        return disabledFile.good() && writeAtomically(disabledValuesPath, disabledFile.str());
    }

    ShaderProfileData ConfigSerializer::loadShaderProfileData(const std::string& path)
    {
        ShaderProfileData data;
        std::ifstream file(path);
        std::string line, section;
        while (std::getline(file, line))
        {
            const auto first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos || line[first] == ';' || line[first] == '#')
                continue;
            if (line[first] == '[')
            {
                const auto end = line.find(']', first + 1);
                section = end == std::string::npos ? "" : line.substr(first + 1, end - first - 1);
                continue;
            }
            const auto eq = line.find('=', first);
            if (eq == std::string::npos)
                continue;
            auto trim = [](std::string s) {
                const auto b = s.find_first_not_of(" \t\r");
                if (b == std::string::npos) return std::string();
                const auto e = s.find_last_not_of(" \t\r");
                return s.substr(b, e - b + 1);
            };
            const auto key = trim(line.substr(first, eq - first));
            const auto value = trim(line.substr(eq + 1));
            auto splitValues = [&trim](const std::string& text, char delimiter) {
                std::vector<std::string> values;
                std::string item;
                for (size_t i = 0; i < text.size(); ++i)
                {
                    if (text[i] == delimiter)
                    {
                        if (delimiter == ',' && i + 1 < text.size() && text[i + 1] == ',')
                        {
                            item += delimiter;
                            ++i;
                        }
                        else
                        {
                            item = trim(std::move(item));
                            if (!item.empty()) values.push_back(std::move(item));
                            item.clear();
                        }
                    }
                    else
                        item += text[i];
                }
                item = trim(std::move(item));
                if (!item.empty()) values.push_back(std::move(item));
                return values;
            };
            if (section.empty() && key == "Techniques")
            {
                data.hasTechniques = true;
                data.techniques = splitValues(value, ',');
                continue;
            }
            if (section.empty() && key == "TechniqueSorting")
            {
                data.techniqueSorting = splitValues(value, ',');
                continue;
            }
            if (section.empty() && key == "VKIntoxEffects")
            {
                data.hasEffectList = true;
                data.effects = splitValues(value, ':');
                continue;
            }
            if (section.empty() && key == "VKIntoxDisabledEffects")
            {
                data.hasEffectList = true;
                data.disabledEffects = splitValues(value, ':');
                continue;
            }
            if (key == "PreprocessorDefinitions")
            {
                for (const auto& definition : splitValues(value, ','))
                {
                    const auto split = definition.find('=');
                    if (split != std::string::npos)
                        data.params.push_back({section, "@" + trim(definition.substr(0, split)), trim(definition.substr(split + 1))});
                }
                continue;
            }
            if (section.empty() || section == "GENERAL")
                continue;
            if (value.find(',') == std::string::npos)
                data.params.push_back({section, key, value});
            else
            {
                const auto components = splitValues(value, ',');
                for (size_t index = 0; index < components.size(); ++index)
                    data.params.push_back({section, key + "[" + std::to_string(index) + "]", components[index]});
            }
        }
        return data;
    }

    std::vector<ConfigParam> ConfigSerializer::loadShaderProfile(const std::string& path)
    {
        return loadShaderProfileData(path).params;
    }

    bool ConfigSerializer::saveToPath(
        const std::string& filePath,
        const std::vector<std::string>& effects,
        const std::vector<std::string>& disabledEffects,
        const std::vector<ConfigParam>& params,
        const std::map<std::string, std::string>& effectPaths,
        const std::vector<PreprocessorDefinition>& preprocessorDefs)
    {
        // Atomic write: write to temp file then rename to prevent corruption
        std::string tmpPath = filePath + ".tmp";
        std::ofstream file(tmpPath);
        if (!file.is_open())
        {
            Logger::err("Could not open for writing: " + tmpPath);
            return false;
        }

        // Group params by effect
        std::map<std::string, std::vector<const ConfigParam*>> paramsByEffect;
        for (const auto& param : params)
            paramsByEffect[param.effectName].push_back(&param);

        // Group preprocessor defs by effect
        std::map<std::string, std::vector<const PreprocessorDefinition*>> defsByEffect;
        for (const auto& def : preprocessorDefs)
            defsByEffect[def.effectName].push_back(&def);

        // Write params grouped by effect
        for (const auto& [effectName, effectParams] : paramsByEffect)
        {
            file << "# " << effectName << "\n";
            auto pathIt = effectPaths.find(effectName);
            if (pathIt != effectPaths.end() && !pathIt->second.empty())
                file << effectName << " = " << pathIt->second << "\n";
            for (const auto* param : effectParams)
                file << param->effectName << "." << param->paramName << " = " << param->value << "\n";
            auto defsIt = defsByEffect.find(effectName);
            if (defsIt != defsByEffect.end())
            {
                for (const auto* def : defsIt->second)
                    file << def->effectName << "@" << def->name << " = " << def->value << "\n";
            }
            file << "\n";
        }

        // Write preprocessor defs for effects that have defs but no params
        for (const auto& [effectName, effectDefs] : defsByEffect)
        {
            if (paramsByEffect.find(effectName) != paramsByEffect.end())
                continue;
            file << "# " << effectName << "\n";
            auto pathIt = effectPaths.find(effectName);
            if (pathIt != effectPaths.end() && !pathIt->second.empty())
                file << effectName << " = " << pathIt->second << "\n";
            for (const auto* def : effectDefs)
                file << def->effectName << "@" << def->name << " = " << def->value << "\n";
            file << "\n";
        }

        // Write paths for effects that have no params or defs
        for (const auto& [effectName, path] : effectPaths)
        {
            if (!path.empty() &&
                paramsByEffect.find(effectName) == paramsByEffect.end() &&
                defsByEffect.find(effectName) == defsByEffect.end())
            {
                file << "# " << effectName << "\n";
                file << effectName << " = " << path << "\n\n";
            }
        }

        // Write effects list
        file << "effects = " << joinEffects(effects) << "\n";

        if (!disabledEffects.empty())
            file << "disabledEffects = " << joinEffects(disabledEffects) << "\n";

        file.close();

        if (file.fail())
        {
            Logger::err("Failed to write config to: " + tmpPath);
            std::remove(tmpPath.c_str());
            return false;
        }

        // Atomic rename — if this fails, the original file is untouched
        if (std::rename(tmpPath.c_str(), filePath.c_str()) != 0)
        {
            Logger::err("Failed to rename temp config to: " + filePath);
            std::remove(tmpPath.c_str());
            return false;
        }

        return true;
    }

} // namespace VKIntox
