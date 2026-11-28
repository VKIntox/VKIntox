#include "overlay/shell/imgui_overlay.hh"
#include "config/config_serializer.hh"
#include "util/async_writer.hh"
#include "util/logger.hh"
#include "util/util.hh"
#include "overlay/theme/ui_icons.hh"
#include "overlay/theme/ui_theme.hh"
#include "overlay/shell/ui_menu.hh"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_internal.h"
#include "vendor/imgui/imfilebrowser.h"

namespace VKIntox
{
    namespace
    {
        ImGui::FileBrowser dirBrowser(
            ImGuiFileBrowserFlags_SelectDirectory |
            ImGuiFileBrowserFlags_HideRegularFiles |
            ImGuiFileBrowserFlags_CloseOnEsc |
            ImGuiFileBrowserFlags_CreateNewDir);

        bool equalsIgnoreCase(const std::string& a, const std::string& b)
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

        bool scanDirectory(const std::filesystem::path& dir,
                           std::set<std::string>& shaderPaths,
                           std::set<std::string>& texturePaths,
                           std::string& error)
        {
            try
            {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(
                         dir, std::filesystem::directory_options::skip_permission_denied))
                {
                    if (!entry.is_directory())
                        continue;
                    const std::string dirName = entry.path().filename().string();
                    if (equalsIgnoreCase(dirName, "Shaders"))
                        shaderPaths.insert(entry.path().string());
                    else if (equalsIgnoreCase(dirName, "Textures"))
                        texturePaths.insert(entry.path().string());
                }
                return true;
            }
            catch (const std::filesystem::filesystem_error& e)
            {
                error = e.what();
                Logger::err("Shader Manager: Error scanning " + dir.string() + ": " + error);
                return false;
            }
        }

        struct ShaderRescanJob
        {
            std::atomic<bool> done{false};
            std::atomic<size_t> scanned{0};
            size_t total = 0;
            std::set<std::string> shaderPaths;
            std::set<std::string> texturePaths;
            std::vector<std::string> errors;
        };

        std::shared_ptr<ShaderRescanJob> g_rescanJob;
        bool g_lastRescanHadErrors = false;
        std::string g_lastRescanError;

        void drawShaderTestResults(
            const std::vector<std::tuple<std::string, std::string, bool, std::string>>& results,
            const std::set<std::string>& depthShaderNames)
        {
            if (results.empty())
                return;

            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            const float d = m.density;
            const float rowH = 56.0f * d;
            const float rowStride = rowH + 4.0f * d;
            const float maxBodyH = 280.0f * d;
            std::vector<size_t> failed;
            std::vector<size_t> depth;
            std::vector<size_t> safe;
            for (size_t i = 0; i < results.size(); ++i)
            {
                const auto& [name, path, success, error] = results[i];
                if (!success)
                    failed.push_back(i);
                else if (depthShaderNames.count(name))
                    depth.push_back(i);
                else
                    safe.push_back(i);
            }

            static bool failedOpen = false;
            static bool depthOpen = false;
            static bool safeOpen = false;
            auto renderGroup = [&](const char* id, const char* label, const std::vector<size_t>& indices,
                                   bool* open, const char* chip, ImGuiM3Role chipRole) {
                if (indices.empty())
                    return;
                const std::string badge = std::to_string(indices.size());
                const float reveal = UI::FoldHeader(id, label, badge.c_str(), open);
                if (!UI::FoldBodyBegin((std::string(id) + "_body").c_str(), reveal,
                                       indices.size() * rowStride, maxBodyH))
                    return;

                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImFont* nameFont = ImGuiM3FontMedium();
                ImFont* pathFont = ImGuiM3FontBodyBold();
                const float namePx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyMedium).size * d;
                const float pathPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodySmall).size * d;
                const float chipW = ImGui::CalcTextSize(chip).x + 24.0f * d;
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(indices.size()), rowStride);
                while (clipper.Step())
                {
                    for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k)
                    {
                        const auto& [name, path, success, error] = results[indices[static_cast<size_t>(k)]];
                        ImGui::PushID(k);
                        const ImVec2 itemMin = ImGui::GetCursorScreenPos();
                        const float itemW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
                        ImGui::InvisibleButton("##shader_row", ImVec2(itemW, rowH));
                        const bool hovered = ImGui::IsItemHovered();
                        const ImRect itemRect(itemMin, ImVec2(itemMin.x + itemW, itemMin.y + rowH));
                        const float rr = ImGuiM3Radius(ImGuiM3Shape_Large);
                        const ImGuiM3ShapeRounding round{ rr, rr, rr, rr };
                        ImGuiM3PathRoundedRect(dl, itemRect, round,
                                               ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainer));
                        if (hovered)
                            ImGuiM3DrawStateLayer(dl, itemRect, round, ImGuiM3Role_OnSurface,
                                                  ImGuiM3State_Hovered);

                        const float textX = itemMin.x + 12.0f * d;
                        const float textW = ImMax(1.0f, itemRect.Max.x - chipW - 28.0f * d - textX);
                        ImGui::PushFont(nameFont, namePx);
                        const char* shownName = UI::TruncateLabel(name.c_str(), textW);
                        const ImVec2 nameSize = ImGui::CalcTextSize(shownName);
                        ImGui::PopFont();
                        ImGui::PushFont(pathFont, pathPx);
                        const char* shownPath = UI::TruncateLabel(path.c_str(), textW);
                        const ImVec2 pathSize = ImGui::CalcTextSize(shownPath);
                        ImGui::PopFont();
                        const float blockH = nameSize.y + pathSize.y + 2.0f * d;
                        const float textY = itemMin.y + (rowH - blockH) * 0.5f;
                        const ImU32 nameColor = success ? ImGuiM3ColorU32(ImGuiM3Role_OnSurface)
                                                        : ImGuiM3ColorU32(ImGuiM3Role_Error);
                        dl->AddText(nameFont, namePx, ImVec2(textX, textY), nameColor, shownName);
                        dl->AddText(pathFont, pathPx, ImVec2(textX, textY + nameSize.y + 2.0f * d),
                                    ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant), shownPath);
                        ImGui::SetCursorScreenPos(ImVec2(itemRect.Max.x - chipW - 12.0f * d,
                                                         itemMin.y + (rowH - m.chip_height * d) * 0.5f));
                        ImGui::M3StatusChip(chip, chipRole);
                        if (hovered)
                        {
                            ImGui::BeginTooltip();
                            ImGui::TextUnformatted(path.c_str());
                            if (!error.empty())
                            {
                                ImGui::PushTextWrapPos(460.0f * d);
                                ImGui::TextColored(UI::Attention(), "%s", error.c_str());
                                ImGui::PopTextWrapPos();
                            }
                            ImGui::EndTooltip();
                        }
                        ImGui::SetCursorScreenPos(ImVec2(itemMin.x, itemMin.y + rowStride));
                        ImGui::Dummy(ImVec2(0.0f, 0.0f));
                        ImGui::PopID();
                    }
                }
                UI::FoldBodyEnd();
            };

            renderGroup("##failed_shader_group", "Failed shaders", failed, &failedOpen,
                        "Failed", ImGuiM3Role_ErrorContainer);
            renderGroup("##depth_shader_group", "Depth shaders", depth, &depthOpen,
                        "Depth", ImGuiM3Role_PrimaryContainer);
            renderGroup("##safe_shader_group", "Safe shaders", safe, &safeOpen,
                        "Passed", ImGuiM3Role_TertiaryContainer);
        }
    }

    void ImGuiOverlay::renderShaderManagerView()
    {
        if (!shaderMgrInitialized)
        {
            ShaderManagerConfig config = ConfigSerializer::loadShaderManagerConfig();
            shaderMgrParentDirs = config.parentDirectories;
            shaderMgrShaderPaths = config.discoveredShaderPaths;
            shaderMgrTexturePaths = config.discoveredTexturePaths;
            shaderMgrInitialized = true;
        }

        const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
        const float d = m.density;
        auto saveConfig = [&]() {
            ShaderManagerConfig config;
            config.parentDirectories = shaderMgrParentDirs;
            config.discoveredShaderPaths = shaderMgrShaderPaths;
            config.discoveredTexturePaths = shaderMgrTexturePaths;
            if (ConfigSerializer::saveShaderManagerConfig(config))
                shaderPathsChanged = true;
            else
                pushToast(LogLevel::Error, "Could not save shader manager paths.");
        };

        if (g_rescanJob && g_rescanJob->done.load(std::memory_order_acquire))
        {
            shaderMgrShaderPaths.assign(g_rescanJob->shaderPaths.begin(), g_rescanJob->shaderPaths.end());
            shaderMgrTexturePaths.assign(g_rescanJob->texturePaths.begin(), g_rescanJob->texturePaths.end());
            g_lastRescanHadErrors = !g_rescanJob->errors.empty();
            g_lastRescanError = g_lastRescanHadErrors ? g_rescanJob->errors.front() : std::string();
            saveConfig();
            if (g_lastRescanHadErrors)
                pushToast(LogLevel::Warn, "Shader scan finished with errors. Some paths may be missing.");
            else
                pushToast(LogLevel::Info, "Found " + std::to_string(shaderMgrShaderPaths.size()) +
                    " shader paths and " + std::to_string(shaderMgrTexturePaths.size()) + " texture paths.");
            g_rescanJob.reset();
        }

        ImGui::M3HeroHeader(Icon::FunctionUtf8, "Shaders",
                            "Add shader packs and scan them for ReShade effects.");
        ImGui::Spacing();
        ImGui::BeginChild("ShaderMgrContent", ImVec2(0, 0), false);

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
                        pushToast(LogLevel::Info, "Added shader directory. Rescan to discover its paths.");
                    }
                    else
                        pushToast(LogLevel::Info, "That shader directory is already in the list.");
                }
                else if (result == FileDialogResult::Unavailable)
                {
                    dirBrowser.SetTitle("Select Parent Directory");
                    const char* home = std::getenv("HOME");
                    dirBrowser.SetPwd(home ? home : "/");
                    dirBrowser.Open();
                }
            }
        }

        const bool portalPending = fileDialogPending();
        const bool rescanning = g_rescanJob != nullptr;
        ImGui::M3CardBegin("shmgr_dirs", "Shader directories", NULL,
                           "Each folder is scanned for its Shaders/ and Textures/ subfolders.");
        ImGui::Spacing();
        ImGui::BeginDisabled(portalPending || rescanning);
        const std::string browseLabel = std::string(Icon::FolderOpenUtf8) +
                                        (portalPending ? "  Opening..." : "  Browse...");
        if (ImGui::M3Button(browseLabel.c_str(), ImGuiM3Button_Tonal))
        {
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
        ImGui::BeginDisabled(rescanning);
        const std::string rescanLabel = std::string(Icon::RefreshUtf8) +
                                        (rescanning ? "  Scanning..." : "  Rescan all");
        if (ImGui::M3Button(rescanLabel.c_str(), rescanning ? ImGuiM3Button_Text : ImGuiM3Button_Outlined))
        {
            auto job = std::make_shared<ShaderRescanJob>();
            job->total = shaderMgrParentDirs.size();
            g_rescanJob = job;
            AsyncWriter::instance().submit([job, parents = shaderMgrParentDirs]() {
                for (const auto& parent : parents)
                {
                    std::string error;
                    std::error_code ec;
                    const bool exists = std::filesystem::exists(parent, ec);
                    const bool isDirectory = !ec && std::filesystem::is_directory(parent, ec);
                    if (ec)
                        error = "Could not inspect " + parent + ": " + ec.message();
                    else if (!exists || !isDirectory)
                        error = "Not a directory: " + parent;
                    else
                        scanDirectory(parent, job->shaderPaths, job->texturePaths, error);
                    if (!error.empty())
                        job->errors.push_back(error);
                    job->scanned.fetch_add(1, std::memory_order_relaxed);
                }
                job->done.store(true, std::memory_order_release);
            });
        }
        ImGui::EndDisabled();
        if (portalPending)
        {
            ImGui::Spacing();
            ImGui::M3StatusChip("Portal picker pending", ImGuiM3Role_PrimaryContainer);
            ImGui::SameLine();
            UI::TextSupport("Choose a parent folder in the desktop picker.");
        }
        if (rescanning)
        {
            const size_t total = g_rescanJob->total;
            const size_t scanned = g_rescanJob->scanned.load(std::memory_order_relaxed);
            const float progress = total > 0 ? static_cast<float>(scanned) / static_cast<float>(total) : 0.5f;
            ImGui::Spacing();
            ImGui::M3LinearProgress(progress, ImVec2(-1, 0), true);
            UI::TextSupport(("Scanning " + std::to_string(scanned) + " / " + std::to_string(total) +
                             " directories...").c_str());
        }
        else if (g_lastRescanHadErrors)
        {
            ImGui::Spacing();
            ImGui::M3StatusChip("Scan completed with errors", ImGuiM3Role_ErrorContainer);
            if (!g_lastRescanError.empty())
                UI::TextSupport(g_lastRescanError.c_str());
        }

        ImGui::Spacing();
        if (shaderMgrParentDirs.empty())
        {
            const ImVec2 emptyPos = ImGui::GetCursorScreenPos();
            const float emptyIcon = 40.0f * d;
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(emptyPos.x + emptyIcon * 0.5f, emptyPos.y + emptyIcon * 0.5f), emptyIcon * 0.5f,
                ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainerHighest), 40);
            if (ImGuiM3IconFont())
            {
                ImGuiM3DrawIcon(ImGui::GetWindowDrawList(), Icon::FolderOpenUtf8,
                                ImRect(emptyPos, ImVec2(emptyPos.x + emptyIcon, emptyPos.y + emptyIcon)),
                                emptyIcon * 0.56f,
                                ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant));
            }
            ImGui::SameLine(0.0f, 12.0f * d);
            ImGui::BeginGroup();
            ImGui::PushFont(ImGuiM3FontBold(),
                            ImGuiM3GetTypeStyle(ImGuiM3Type_TitleMedium).size * d);
            ImGui::TextUnformatted("No shader directories");
            ImGui::PopFont();
            UI::TextSupport("Browse to a shader pack folder, then rescan to discover its paths.");
            ImGui::EndGroup();
        }
        else
        {
            const float rowH = 56.0f * d;
            const float rowGap = 4.0f * d;
            int removeIdx = -1;
            for (size_t i = 0; i < shaderMgrParentDirs.size(); i++)
            {
                ImGui::PushID(static_cast<int>(i));
                if (i > 0)
                    ImGui::Dummy(ImVec2(0.0f, rowGap));
                const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                const float rowW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
                ImGui::InvisibleButton("##dir_row", ImVec2(rowW, rowH));
                const bool hovered = ImGui::IsItemHovered();
                const ImRect rowRect(rowMin, ImVec2(rowMin.x + rowW, rowMin.y + rowH));
                const float radius = ImGuiM3Radius(ImGuiM3Shape_Large);
                const ImGuiM3ShapeRounding round{ radius, radius, radius, radius };
                ImGuiM3PathRoundedRect(ImGui::GetWindowDrawList(), rowRect, round,
                                       ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainer));
                if (hovered)
                    ImGuiM3DrawStateLayer(ImGui::GetWindowDrawList(), rowRect, round,
                                          ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);
                if (ImGuiM3IconFont())
                    ImGuiM3DrawIcon(ImGui::GetWindowDrawList(), Icon::FolderOpenUtf8,
                                    ImRect(ImVec2(rowMin.x + 12.0f * d, rowMin.y + 8.0f * d),
                                           ImVec2(rowMin.x + 48.0f * d, rowMin.y + 44.0f * d)),
                                    22.0f * d, ImGuiM3ColorU32(ImGuiM3Role_Primary));
                const float textX = rowMin.x + 56.0f * d;
                const float textW = ImMax(1.0f, rowRect.Max.x - textX - 56.0f * d);
                const float pathPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyMedium).size * d;
                ImFont* pathFont = ImGuiM3FontMedium();
                ImGui::PushFont(pathFont, pathPx);
                const char* shownPath = UI::TruncateLabel(shaderMgrParentDirs[i].c_str(), textW);
                const ImVec2 pathSize = ImGui::CalcTextSize(shownPath);
                ImGui::PopFont();
                ImGui::GetWindowDrawList()->AddText(
                    pathFont, pathPx, ImVec2(textX, rowMin.y + (rowH - pathSize.y) * 0.5f),
                    ImGuiM3ColorU32(ImGuiM3Role_OnSurface), shownPath);
                if (hovered)
                    ImGui::SetTooltip("%s", shaderMgrParentDirs[i].c_str());
                ImGui::SetCursorScreenPos(ImVec2(rowRect.Max.x - 48.0f * d,
                                                 rowMin.y + (rowH - m.icon_button_size * d) * 0.5f));
                ImGui::SetNextItemAllowOverlap();
                ImGui::BeginDisabled(rescanning);
                if (ImGui::M3IconButton(Icon::CloseUtf8, "Remove this directory", ImGuiM3Button_Text))
                    removeIdx = static_cast<int>(i);
                ImGui::EndDisabled();
                ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowRect.Max.y));
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
                ImGui::PopID();
            }
            if (removeIdx >= 0)
            {
                const std::string removed = shaderMgrParentDirs[removeIdx];
                shaderMgrParentDirs.erase(shaderMgrParentDirs.begin() + removeIdx);
                saveConfig();
                pushToast(LogLevel::Info, "Removed shader directory. Rescan to update discovered paths.");
                (void)removed;
            }
        }
        ImGui::M3CardEnd();

        ImGui::Spacing();
        ImGui::M3CardBegin("shmgr_paths", "Discovered paths", NULL,
                           "Rescan refreshes these lists. Remove a path to exclude it from this pack.");
        static bool shaderPathsOpen = false;
        static bool texturePathsOpen = false;
        {
            const float pathRowH = 56.0f * d;
            const float pathStride = pathRowH + 4.0f * d;
            const float maxBodyH = 280.0f * d;
            struct PathGroup
            {
                const char* id;
                const char* label;
                const char* emptyText;
                const char* glyph;
                std::vector<std::string>* paths;
                bool* open;
            };
            PathGroup groups[] = {
                {"##shader_path_group", "Shader paths", "No shader paths discovered. Add a pack and rescan.",
                 Icon::LayersUtf8, &shaderMgrShaderPaths, &shaderPathsOpen},
                {"##texture_path_group", "Texture paths", "No texture paths discovered. Add a pack and rescan.",
                 Icon::TextureUtf8, &shaderMgrTexturePaths, &texturePathsOpen},
            };
            for (PathGroup& group : groups)
            {
                const std::string badge = std::to_string(group.paths->size());
                const float reveal = UI::FoldHeader(group.id, group.label, badge.c_str(), group.open);
                if (group.paths->empty())
                {
                    UI::TextSupport(group.emptyText);
                    continue;
                }
                const std::string bodyId = std::string(group.id) + "_body";
                if (!UI::FoldBodyBegin(bodyId.c_str(), reveal, group.paths->size() * pathStride, maxBodyH))
                    continue;
                int removeIdx = -1;
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(group.paths->size()), pathStride);
                while (clipper.Step())
                {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                    {
                        ImGui::PushID(i);
                        const ImVec2 itemMin = ImGui::GetCursorScreenPos();
                        const float itemW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
                        ImGui::InvisibleButton("##path", ImVec2(itemW, pathRowH));
                        const bool hovered = ImGui::IsItemHovered();
                        const ImRect itemRect(itemMin, ImVec2(itemMin.x + itemW, itemMin.y + pathRowH));
                        const float rr = ImGuiM3Radius(ImGuiM3Shape_Large);
                        const ImGuiM3ShapeRounding round{ rr, rr, rr, rr };
                        ImGuiM3PathRoundedRect(ImGui::GetWindowDrawList(), itemRect, round,
                                               ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainer));
                        if (hovered)
                            ImGuiM3DrawStateLayer(ImGui::GetWindowDrawList(), itemRect, round,
                                                  ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);
                        if (ImGuiM3IconFont())
                            ImGuiM3DrawIcon(ImGui::GetWindowDrawList(), group.glyph,
                                            ImRect(ImVec2(itemMin.x + 10.0f * d, itemMin.y + 12.0f * d),
                                                   ImVec2(itemMin.x + 42.0f * d, itemMin.y + 44.0f * d)),
                                            20.0f * d, ImGuiM3ColorU32(ImGuiM3Role_Primary));
                        const float textX = itemMin.x + 50.0f * d;
                        const float textW = ImMax(1.0f, itemRect.Max.x - textX - 52.0f * d);
                        const float pathPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodySmall).size * d;
                        ImFont* pathFont = ImGuiM3FontBodyBold();
                        ImGui::PushFont(pathFont, pathPx);
                        const char* shownPath = UI::TruncateLabel((*group.paths)[i].c_str(), textW);
                        const ImVec2 pathSize = ImGui::CalcTextSize(shownPath);
                        ImGui::PopFont();
                        ImGui::GetWindowDrawList()->AddText(
                            pathFont, pathPx,
                            ImVec2(textX, itemMin.y + (pathRowH - pathSize.y) * 0.5f),
                            ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant), shownPath);
                        if (hovered)
                            ImGui::SetTooltip("%s", (*group.paths)[i].c_str());
                        ImGui::SetCursorScreenPos(ImVec2(itemRect.Max.x - 48.0f * d,
                                                         itemMin.y + (pathRowH - m.icon_button_size * d) * 0.5f));
                        ImGui::SetNextItemAllowOverlap();
                        ImGui::BeginDisabled(rescanning);
                        if (ImGui::M3IconButton(Icon::CloseUtf8, "Remove this path", ImGuiM3Button_Text))
                            removeIdx = i;
                        ImGui::EndDisabled();
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

        ImGui::Spacing();
        ImGui::M3CardBegin("shmgr_test", "Shader test", NULL,
                           "Compile every unique .fx shader and separate failures from depth shaders.");
        ImGui::BeginDisabled(rescanning || shaderMgrShaderPaths.empty());
        renderShaderTestSection();
        ImGui::EndDisabled();
        if (shaderMgrShaderPaths.empty() && !shaderTestRunning)
            UI::TextSupport("No shader paths yet. Add a pack and rescan before compile-testing.");
        else if (shaderTestComplete && shaderTestResults.empty())
            UI::TextSupport("No .fx shaders were found in the discovered paths.");
        if (shaderTestComplete && !shaderTestResults.empty())
            drawShaderTestResults(shaderTestResults, depthShaders);
        ImGui::M3CardEnd();

        ImGui::EndChild();

        dirBrowser.Display();
        if (dirBrowser.HasSelected())
        {
            const std::string selectedPath = dirBrowser.GetSelected().string();
            if (std::find(shaderMgrParentDirs.begin(), shaderMgrParentDirs.end(), selectedPath) == shaderMgrParentDirs.end())
            {
                shaderMgrParentDirs.push_back(selectedPath);
                saveConfig();
                pushToast(LogLevel::Info, "Added shader directory. Rescan to discover its paths.");
            }
            dirBrowser.ClearSelected();
        }
    }

} // namespace VKIntox
