#include "imgui_overlay.hh"
#include "config_serializer.hh"
#include "logger.hh"
#include "overlay/ui_icons.hh"
#include "util.hh"

#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <set>
#include <string>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imfilebrowser.h"

namespace VKIntox
{
    // Defined in view_shader_test.cpp
    void renderShaderTestResultsUI(
        const std::vector<std::tuple<std::string, std::string, bool, std::string>>& results,
        const std::set<std::string>& depthShaderNames);
}

namespace VKIntox
{
    // Static file browser for adding directories
    static ImGui::FileBrowser dirBrowser(
        ImGuiFileBrowserFlags_SelectDirectory |
        ImGuiFileBrowserFlags_HideRegularFiles |
        ImGuiFileBrowserFlags_CloseOnEsc |
        ImGuiFileBrowserFlags_CreateNewDir);

    // Case-insensitive string comparison
    static bool equalsIgnoreCase(const std::string& a, const std::string& b)
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

    // Recursively scan directory for Shaders/ and Textures/ subdirectories
    static void scanDirectory(
        const std::filesystem::path& dir,
        std::set<std::string>& shaderPaths,
        std::set<std::string>& texturePaths)
    {
        try
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                dir, std::filesystem::directory_options::skip_permission_denied))
            {
                if (!entry.is_directory())
                    continue;

                std::string dirName = entry.path().filename().string();
                if (equalsIgnoreCase(dirName, "Shaders"))
                    shaderPaths.insert(entry.path().string());
                else if (equalsIgnoreCase(dirName, "Textures"))
                    texturePaths.insert(entry.path().string());
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            Logger::err("Shader Manager: Error scanning " + dir.string() + ": " + e.what());
        }
    }

    void ImGuiOverlay::renderShaderManagerView()
    {
        // Load config on first open
        if (!shaderMgrInitialized)
        {
            ShaderManagerConfig config = ConfigSerializer::loadShaderManagerConfig();
            shaderMgrParentDirs = config.parentDirectories;
            shaderMgrShaderPaths = config.discoveredShaderPaths;
            shaderMgrTexturePaths = config.discoveredTexturePaths;
            shaderMgrInitialized = true;
        }

        // Helper to save config (auto-save on any change)
        auto saveConfig = [&]() {
            ShaderManagerConfig config;
            config.parentDirectories = shaderMgrParentDirs;
            config.discoveredShaderPaths = shaderMgrShaderPaths;
            config.discoveredTexturePaths = shaderMgrTexturePaths;
            ConfigSerializer::saveShaderManagerConfig(config);
            shaderPathsChanged = true;
        };

        ImGui::BeginChild("ShaderMgrContent", ImVec2(0, 0), false);

        ImGui::TextDisabled("Add shader packs and scan them for ReShade effects.");
        ImGui::Spacing();

        // Harvest any completed portal request (non-blocking), then offer Browse.
        {
            FileDialogResult result = FileDialogResult::Cancelled;
            std::string picked;
            if (pollFileDialog(FileDialogKind::OpenDirectory, result, picked))
            {
                if (result == FileDialogResult::Success && !picked.empty())
                {
                    if (std::find(shaderMgrParentDirs.begin(), shaderMgrParentDirs.end(), picked) == shaderMgrParentDirs.end())
                    {
                        shaderMgrParentDirs.push_back(picked);
                        saveConfig();
                    }
                }
                else if (result == FileDialogResult::Unavailable)
                {
                    // No portal: fall back to the in-overlay browser.
                    dirBrowser.SetTitle("Select Parent Directory");
                    const char* home = std::getenv("HOME");
                    dirBrowser.SetPwd(home ? home : "/");
                    dirBrowser.Open();
                }
            }
        }

        // --- Directories ---
        ImGui::M3CardBegin("shmgr_dirs", "Shader Directories", Icon::FolderOpenUtf8);
        ImGui::TextDisabled("Directories containing shader packs. Each is scanned for Shaders/ and Textures/ subfolders.");
        ImGui::Spacing();

        const bool portalPending = fileDialogPending();
        ImGui::BeginDisabled(portalPending);
        const std::string browseLabel = std::string(Icon::FolderOpenUtf8) + (portalPending ? "  Opening..." : "  Browse...");
        if (ImGui::Button(browseLabel.c_str()))
        {
            // If the portal cannot even be started (no GIO, no session bus),
            // open the in-overlay browser right away.
            if (!startOpenDirectoryDialog("Select Parent Directory"))
            {
                dirBrowser.SetTitle("Select Parent Directory");
                const char* home = std::getenv("HOME");
                dirBrowser.SetPwd(home ? home : "/");
                dirBrowser.Open();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        const std::string rescanLabel = std::string(Icon::RefreshUtf8) + "  Rescan all";
        if (ImGui::Button(rescanLabel.c_str()))
        {
            std::set<std::string> shaderSet, textureSet;
            for (const auto& parentDir : shaderMgrParentDirs)
            {
                if (std::filesystem::exists(parentDir) && std::filesystem::is_directory(parentDir))
                    scanDirectory(parentDir, shaderSet, textureSet);
            }
            shaderMgrShaderPaths.assign(shaderSet.begin(), shaderSet.end());
            shaderMgrTexturePaths.assign(textureSet.begin(), textureSet.end());
            Logger::info("Shader Manager: Found " + std::to_string(shaderMgrShaderPaths.size()) +
                " shader paths, " + std::to_string(shaderMgrTexturePaths.size()) + " texture paths");
            saveConfig();
        }

        ImGui::Spacing();
        ImGui::BeginChild("ParentDirList", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        int removeIdx = -1;
        for (size_t i = 0; i < shaderMgrParentDirs.size(); i++)
        {
            ImGui::PushID(static_cast<int>(i));
            const std::string removeLabel = std::string(Icon::CloseUtf8) + "##rm";
            if (ImGui::Button(removeLabel.c_str()))
                removeIdx = static_cast<int>(i);
            ImGui::SameLine();
            ImGui::TextUnformatted(shaderMgrParentDirs[i].c_str());
            ImGui::PopID();
        }
        if (shaderMgrParentDirs.empty())
            ImGui::TextDisabled("No directories added yet.");
        ImGui::EndChild();

        if (removeIdx >= 0)
        {
            shaderMgrParentDirs.erase(shaderMgrParentDirs.begin() + removeIdx);
            saveConfig();
        }
        ImGui::M3CardEnd();

        // --- Discovered paths ---
        ImGui::Spacing();
        ImGui::M3CardBegin("shmgr_paths", "Discovered Paths", Icon::LayersUtf8);
        ImGui::TextDisabled("%zu shader paths, %zu texture paths.", shaderMgrShaderPaths.size(), shaderMgrTexturePaths.size());
        ImGui::Spacing();

        if (ImGui::TreeNode("Shader Paths"))
        {
            if (shaderMgrShaderPaths.empty())
                ImGui::TextDisabled("None - click Rescan all");
            else
            {
                int removeShaderIdx = -1;
                for (size_t i = 0; i < shaderMgrShaderPaths.size(); i++)
                {
                    ImGui::PushID(static_cast<int>(i));
                    const std::string rm = std::string(Icon::CloseUtf8) + "##rm";
                    if (ImGui::SmallButton(rm.c_str()))
                        removeShaderIdx = static_cast<int>(i);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(shaderMgrShaderPaths[i].c_str());
                    ImGui::PopID();
                }
                if (removeShaderIdx >= 0)
                {
                    shaderMgrShaderPaths.erase(shaderMgrShaderPaths.begin() + removeShaderIdx);
                    saveConfig();
                }
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Texture Paths"))
        {
            if (shaderMgrTexturePaths.empty())
                ImGui::TextDisabled("None - click Rescan all");
            else
            {
                int removeTextureIdx = -1;
                for (size_t i = 0; i < shaderMgrTexturePaths.size(); i++)
                {
                    ImGui::PushID(static_cast<int>(i) + 1000);
                    const std::string rm = std::string(Icon::CloseUtf8) + "##rm";
                    if (ImGui::SmallButton(rm.c_str()))
                        removeTextureIdx = static_cast<int>(i);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(shaderMgrTexturePaths[i].c_str());
                    ImGui::PopID();
                }
                if (removeTextureIdx >= 0)
                {
                    shaderMgrTexturePaths.erase(shaderMgrTexturePaths.begin() + removeTextureIdx);
                    saveConfig();
                }
            }
            ImGui::TreePop();
        }
        ImGui::M3CardEnd();

        // --- Shader test ---
        ImGui::Spacing();
        ImGui::M3CardBegin("shmgr_test", "Shader Test", Icon::ScienceUtf8);
        renderShaderTestSection();
        if (shaderTestComplete)
            renderShaderTestResultsUI(shaderTestResults, depthShaders);
        ImGui::M3CardEnd();

        ImGui::EndChild();

        // Display file browser (must be called every frame when open)
        dirBrowser.Display();
        if (dirBrowser.HasSelected())
        {
            std::string selectedPath = dirBrowser.GetSelected().string();
            // Avoid duplicates
            bool exists = false;
            for (const auto& dir : shaderMgrParentDirs)
            {
                if (dir == selectedPath)
                {
                    exists = true;
                    break;
                }
            }
            if (!exists)
            {
                shaderMgrParentDirs.push_back(selectedPath);
                saveConfig();
            }
            dirBrowser.ClearSelected();
        }

    }

} // namespace VKIntox
