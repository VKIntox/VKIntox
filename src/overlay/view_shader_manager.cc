#include "imgui_overlay.hh"
#include "config_serializer.hh"
#include "logger.hh"
#include "overlay/ui_icons.hh"
#include "overlay/ui_theme.hh"
#include "util.hh"

#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <set>
#include <string>

#include "overlay/ui_menu.hh"

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_internal.h"
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

        UI::TextSupport("Add shader packs and scan them for ReShade effects.");
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
        ImGui::M3CardBegin("shmgr_dirs", "Shader Directories", NULL,
                           "Each folder is scanned for its Shaders/ and Textures/ subfolders.");
        ImGui::Spacing();

        const bool portalPending = fileDialogPending();
        ImGui::BeginDisabled(portalPending);
        const std::string browseLabel = std::string(Icon::FolderOpenUtf8) + (portalPending ? "  Opening..." : "  Browse...");
        if (ImGui::Button(browseLabel.c_str()))
        {
            // If the portal cannot even be started (no GIO, no session bus),
            // open the in-overlay browser right away.
            if (!settingsManager.getUsePortalIntegration()
                || !startOpenDirectoryDialog("Select Parent Directory"))
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
        {
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            const float d = m.density;
            const float rowH = 48.0f * d;
            const float rowGap = 6.0f * d;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImFont* brand = ImGuiM3FontBold();
            const float namePx = 14.0f * d;
            ImGui::PushFont(brand, namePx);
            const float nameLine = ImGui::GetTextLineHeight();
            ImGui::PopFont();

            int removeIdx = -1;
            for (size_t i = 0; i < shaderMgrParentDirs.size(); i++)
            {
                ImGui::PushID(static_cast<int>(i));
                if (i > 0)
                    ImGui::Dummy(ImVec2(0.0f, rowGap));
                const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                const float rowW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
                const ImRect rowRect(rowMin, ImVec2(rowMin.x + rowW, rowMin.y + rowH));
                ImGui::InvisibleButton("##dir_row", ImVec2(rowW, rowH));

                const float radius = ImGuiM3Radius(ImGuiM3Shape_Large);
                const ImGuiM3ShapeRounding round{ radius, radius, radius, radius };
                ImGuiM3PathRoundedRect(dl, rowRect, round, ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainer));
                if (ImGui::IsItemHovered())
                    ImGuiM3DrawStateLayer(dl, rowRect, round, ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);

                if (ImGuiM3IconFont())
                    ImGuiM3DrawIcon(dl, Icon::FolderOpenUtf8,
                                    ImRect(ImVec2(rowMin.x + 14.0f * d, rowMin.y),
                                           ImVec2(rowMin.x + 42.0f * d, rowMin.y + rowH)),
                                    20.0f * d, ImGuiM3ColorU32(ImGuiM3Role_Primary));

                const float textX = rowMin.x + 54.0f * d;
                const float textW = ImMax(24.0f, rowW - (54.0f + 52.0f) * d);
                dl->AddText(brand, namePx, ImVec2(textX, rowMin.y + (rowH - nameLine) * 0.5f),
                            ImGuiM3ColorU32(ImGuiM3Role_OnSurface),
                            UI::TruncateLabel(shaderMgrParentDirs[i].c_str(), textW));

                ImGui::SetCursorScreenPos(ImVec2(rowRect.Max.x - 48.0f * d, rowMin.y + (rowH - 40.0f * d) * 0.5f));
                if (ImGui::M3IconButton(Icon::CloseUtf8, "Remove this directory", ImGuiM3Button_Text))
                    removeIdx = static_cast<int>(i);
                ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowRect.Max.y));
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
                ImGui::PopID();
            }
            if (shaderMgrParentDirs.empty())
                UI::TextSupport("No directories added yet.");

            if (removeIdx >= 0)
            {
                shaderMgrParentDirs.erase(shaderMgrParentDirs.begin() + removeIdx);
                saveConfig();
            }
        }
        ImGui::M3CardEnd();

        // --- Discovered paths ---
        ImGui::Spacing();
        ImGui::M3CardBegin("shmgr_paths", "Discovered Paths", Icon::LayersUtf8);
        ImGui::Spacing();

        // Two folded groups, revealed with the M3 spring. Each body is a
        // scrolling, virtualized list capped in height, so a pack with hundreds
        // of paths costs the same as one with three - an unvirtualized list was
        // what made unfolding hitch.
        static bool shaderPathsOpen = false;
        static bool texturePathsOpen = false;
        {
            const float d = ImGuiM3GetMetrics().density;
            const float pathRowH = 38.0f * d;
            const float pathGap = 4.0f * d;
            const float pathStride = pathRowH + pathGap;
            const float maxBodyH = 240.0f * d;

            struct PathGroup
            {
                const char* id;
                const char* label;
                std::vector<std::string>* paths;
                bool* open;
            };
            PathGroup groups[] = {
                { "##shader_path_group", "Shader paths", &shaderMgrShaderPaths, &shaderPathsOpen },
                { "##texture_path_group", "Texture paths", &shaderMgrTexturePaths, &texturePathsOpen },
            };

            for (PathGroup& group : groups)
            {
                const std::string badge = std::to_string(group.paths->size());
                const float reveal = UI::FoldHeader(group.id, group.label, badge.c_str(), group.open);

                const size_t count = group.paths->size();
                if (count == 0)
                {
                    UI::TextSupport("None yet - press Rescan all.");
                    continue;
                }

                const std::string bodyId = std::string(group.id) + "_body";
                if (!UI::FoldBodyBegin(bodyId.c_str(), reveal, count * pathStride, maxBodyH))
                    continue;

                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImFont* bodyBold = ImGuiM3FontBodyBold();
                const float pathPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodySmall).size * d;
                int removeIdx = -1;

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(count), pathStride);
                while (clipper.Step())
                {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                    {
                        ImGui::PushID(i);
                        const ImVec2 itemMin = ImGui::GetCursorScreenPos();
                        const float itemW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
                        ImGui::InvisibleButton("##path", ImVec2(itemW, pathStride));
                        const bool itemHovered = ImGui::IsItemHovered();
                        const ImRect itemRect(ImVec2(itemMin.x, itemMin.y + pathGap * 0.5f),
                                              ImVec2(itemMin.x + itemW, itemMin.y + pathStride - pathGap * 0.5f));
                        const float ir = ImGuiM3Radius(ImGuiM3Shape_Small);
                        if (itemHovered)
                            ImGuiM3DrawStateLayer(dl, itemRect, ImGuiM3ShapeRounding{ ir, ir, ir, ir },
                                                  ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);

                        const float textX = itemMin.x + 14.0f * d;
                        const float textW = ImMax(24.0f, itemW - 14.0f * d - 52.0f * d);
                        dl->AddText(bodyBold, pathPx,
                                    ImVec2(textX, itemRect.GetCenter().y - pathPx * 0.7f),
                                    ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant),
                                    UI::TruncateLabel((*group.paths)[i].c_str(), textW));

                        ImGui::SetCursorScreenPos(ImVec2(itemRect.Max.x - 44.0f * d,
                                                         itemRect.GetCenter().y - 20.0f * d));
                        if (ImGui::M3IconButton(Icon::CloseUtf8, "Remove this path", ImGuiM3Button_Text))
                            removeIdx = i;
                        ImGui::SetCursorScreenPos(ImVec2(itemMin.x, itemMin.y + pathStride));
                        ImGui::Dummy(ImVec2(0.0f, 0.0f));
                        ImGui::PopID();
                    }
                }

                if (removeIdx >= 0)
                {
                    group.paths->erase(group.paths->begin() + removeIdx);
                    saveConfig();
                }
                UI::FoldBodyEnd();
            }
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
