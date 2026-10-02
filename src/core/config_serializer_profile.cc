#include "config_serializer.hh"

#include "c_resource.hh"
#include "logger.hh"

#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <sys/stat.h>
#include <algorithm>
#include <set>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cctype>

namespace VKIntox
{
    namespace
    {
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

    std::string ConfigSerializer::getShaderProfileSidecarPath(const std::string& path)
    {
        if (path.empty())
            return "";
        const auto presetPath = std::filesystem::path(path);
        return (presetPath.parent_path() /
                ("." + presetPath.filename().string() + "_disabled-effectvalues")).string();
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
        std::string sourceSidecar;
        if (!copyFromProfile.empty())
        {
            const std::string sourcePath = getShaderProfilePath(gameName, copyFromProfile);
            std::ifstream source(sourcePath, std::ios::binary);
            if (sourcePath.empty() || !source)
                return false;
            contents.assign(std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>());
            if (source.bad())
                return false;
            // an owned profile's sidecar is part of what "inherit" means:
            // without it the copy loses built-ins and applies as foreign
            std::ifstream sidecar(getShaderProfileSidecarPath(sourcePath), std::ios::binary);
            if (sidecar)
                sourceSidecar.assign(std::istreambuf_iterator<char>(sidecar), std::istreambuf_iterator<char>());
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
        if (success && !sourceSidecar.empty())
            success = writeAtomically(getShaderProfileSidecarPath(path), sourceSidecar);
        if (!success)
        {
            unlink(path.c_str());
            std::remove(getShaderProfileSidecarPath(path).c_str());
        }
        return success;
    }

    bool ConfigSerializer::deleteShaderProfile(const std::string& gameName, const std::string& profileName)
    {
        const std::string path = getShaderProfilePath(gameName, profileName);
        if (path.empty())
            return false;
        std::remove(getShaderProfileSidecarPath(path).c_str());
        return std::remove(path.c_str()) == 0;
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
        if (const auto parent = std::filesystem::path(path).parent_path(); !parent.empty())
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
            for (const auto& [effectName, paramName, value] : source)
                merged[{sectionFor(effectName), paramName}] = value;
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

        // the sidecar keeps VKIntox-only state out of the ReShade preset:
        // the ordered instance list ("*" marks unchecked) plus values for
        // disabled effects. it is always written so the .ini is recognised
        // as ours and applies its stack exactly on the next load.
        auto instanceEntry = [&effectPaths](const std::string& effectName, bool isDisabled) {
            std::string type;
            const auto it = effectPaths.find(effectName);
            if (it != effectPaths.end() && !it->second.empty())
            {
                const std::filesystem::path p(it->second);
                type = p.extension() == ".fx" ? p.filename().string() : it->second;
            }
            std::string entry = (type.empty() || type == effectName) ? effectName : effectName + "@" + type;
            if (isDisabled)
                entry.insert(entry.begin(), '*');
            return entry;
        };
        const std::string sidecarPath = getShaderProfileSidecarPath(path);
        std::ostringstream sidecar;
        sidecar << "[VKINTOX]\nEffects=";
        for (size_t i = 0; i < effects.size(); ++i)
        {
            if (i) sidecar << ", ";
            sidecar << instanceEntry(effects[i], disabled.count(effects[i]) != 0);
        }
        sidecar << "\n\n";
        for (const auto& [key, value] : mergedDisabledParams)
        {
            if (!key.second.empty() && key.second.front() == '@')
            {
                sidecar << '[' << key.first << "]\nPreprocessorDefinitions="
                        << key.second.substr(1) << '=' << value << "\n\n";
            }
            else
                sidecar << '[' << key.first << "]\n" << key.second << '=' << value << "\n\n";
        }
        return sidecar.good() && writeAtomically(sidecarPath, sidecar.str());
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
            if (section == "VKINTOX")
            {
                data.owned = true;
                if (key == "Effects")
                {
                    for (const auto& entry : splitValues(value, ','))
                    {
                        std::string item = entry;
                        ShaderProfileInstance instance;
                        if (!item.empty() && item.front() == '*')
                        {
                            instance.enabled = false;
                            item.erase(item.begin());
                        }
                        const auto at = item.rfind('@');
                        if (at == std::string::npos || at + 1 >= item.size())
                        {
                            instance.name = item;
                            instance.type = item;
                        }
                        else
                        {
                            instance.name = item.substr(0, at);
                            instance.type = item.substr(at + 1);
                        }
                        if (!instance.name.empty())
                            data.instances.push_back(instance);
                    }
                }
                continue;
            }
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


} // namespace VKIntox
