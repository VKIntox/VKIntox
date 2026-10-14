#include "imgui_overlay.hh"
#include "reshade_parser.hh"
#include "config_serializer.hh"
#include "logger.hh"
#include "overlay/ui_icons.hh"
#include "overlay/ui_theme.hh"

#include <filesystem>
#include <set>
#include <algorithm>

#ifdef __linux__
#include <malloc.h>  // malloc_trim — reclaim fragmented heap memory
#endif

#include "vendor/imgui/imgui.h"

namespace VKIntox
{
    void ImGuiOverlay::startShaderTest()
    {
        // Only an in-flight run blocks a new one: pressing the button again is an
        // explicit "refresh the paths and compile them all over".
        if (shaderTestRunning)
            return;

        // Ensure shader manager paths are loaded
        if (!shaderMgrInitialized)
        {
            ShaderManagerConfig config = ConfigSerializer::loadShaderManagerConfig();
            shaderMgrParentDirs = config.parentDirectories;
            shaderMgrShaderPaths = config.discoveredShaderPaths;
            shaderMgrTexturePaths = config.discoveredTexturePaths;
            shaderMgrInitialized = true;
        }

        // Build test queue from all .fx files in discovered shader paths
        shaderTestQueue.clear();
        shaderTestResults.clear();
        shaderTestCurrentIndex = 0;
        shaderTestComplete = false;
        shaderTestDuplicateCount = 0;
        // the set this run covers; a later overlayStateVersion bump means new shaders
        shaderTestStateVersion = overlayStateVersion;

        // Cache include paths once — avoids re-reading shader_manager.conf per shader
        ShaderManagerConfig smConfig = ConfigSerializer::loadShaderManagerConfig();
        shaderTestIncludePaths = smConfig.discoveredShaderPaths;

        std::set<std::string> seenCanonicalPaths;
        for (const auto& shaderPath : shaderMgrShaderPaths)
        {
            try
            {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(
                         shaderPath, std::filesystem::directory_options::skip_permission_denied))
                {
                    if (!entry.is_regular_file())
                        continue;
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext != ".fx")
                        continue;

                    // Deduplicate by canonical path — catches symlinks pointing
                    // to the same nix store file across shader packs
                    std::string filePath = entry.path().string();
                    std::string canonical = filePath;
                    try { canonical = std::filesystem::canonical(entry.path()).string(); }
                    catch (...) {}
                    if (seenCanonicalPaths.count(canonical))
                    {
                        shaderTestDuplicateCount++;
                        continue;
                    }
                    seenCanonicalPaths.insert(canonical);

                    std::string effectName = entry.path().stem().string();
                    shaderTestQueue.emplace_back(effectName, filePath);
                }
            }
            catch (const std::filesystem::filesystem_error&)
            {
                // Skip inaccessible directories
            }
        }

        if (!shaderTestQueue.empty())
        {
            shaderTestRunning = true;
            Logger::info("Shader test auto-started: " + std::to_string(shaderTestQueue.size()) + " shaders");
        }
    }

    void ImGuiOverlay::processShaderTest()
    {
        if (!shaderTestRunning)
            return;

        if (shaderTestCurrentIndex < shaderTestQueue.size())
        {
            const auto& [name, path] = shaderTestQueue[shaderTestCurrentIndex];
            ShaderTestResult result = testShaderCompilation(name, path, shaderTestIncludePaths);
            shaderTestResults.emplace_back(result.effectName, result.filePath,
                result.success, result.errorMessage);
            if (result.usesDepth)
                depthShaders.insert(result.effectName);
            shaderTestCurrentIndex++;

            // Reclaim fragmented heap memory every 25 shaders to prevent
            // OOM from accumulating freed-but-unreturned allocations
#ifdef __linux__
            if (shaderTestCurrentIndex % 25 == 0)
                malloc_trim(0);
#endif
        }
        else
        {
            shaderTestRunning = false;
            shaderTestComplete = true;
#ifdef __linux__
            malloc_trim(0);  // Final cleanup after all tests
#endif
            Logger::info("Shader test complete: tested " +
                std::to_string(shaderTestResults.size()) + " shaders");
        }
    }

    void ImGuiOverlay::renderShaderTestSection()
    {
        // The action never greys out on its own: a finished run just changes what
        // it offers, from the first compile-test to a re-run over refreshed paths.
        // Only an in-flight run blocks it.
        if (shaderTestRunning)
        {
            const float progress = shaderTestQueue.empty() ? 1.0f :
                static_cast<float>(shaderTestCurrentIndex) / static_cast<float>(shaderTestQueue.size());
            ImGui::M3LinearProgress(progress, ImVec2(-1, 0), true);
            UI::TextSupport(("Testing " + std::to_string(shaderTestCurrentIndex) + "/" +
                             std::to_string(shaderTestQueue.size()) + " shaders...").c_str());
        }
        else
        {
            const bool alreadyTested = shaderTestComplete && overlayStateVersion == shaderTestStateVersion;
            const std::string label = alreadyTested
                ? std::string(Icon::RefreshUtf8) + "  Refresh paths and compile-test again"
                : std::string(Icon::ScienceUtf8) + "  Compile-test shaders";
            if (ImGui::M3Button(label.c_str(), alreadyTested ? ImGuiM3Button_Tonal : ImGuiM3Button_Filled))
                startShaderTest();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(alreadyTested
                    ? "Re-scan every shader path and compile-test them all again."
                    : "Test every .fx shader for compilation errors.");
        }

        // summary line
        if (shaderTestComplete && !shaderTestResults.empty())
        {
            int passCount = 0, failCount = 0;
            for (const auto& [name, path, success, error] : shaderTestResults)
            {
                if (success)
                    passCount++;
                else
                    failCount++;
            }
            const int depthCount = static_cast<int>(depthShaders.size());
            UI::PushSupport();
            if (failCount == 0)
                ImGui::TextColored(UI::Success(), "All %d passed", passCount);
            else
                ImGui::TextColored(UI::Warning(), "%d passed, %d failed", passCount, failCount);
            if (depthCount > 0)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("\xC2\xB7 %d use depth", depthCount);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("These shaders require depth buffer access.");
            }
            if (shaderTestDuplicateCount > 0)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("\xC2\xB7 %d duplicates skipped", shaderTestDuplicateCount);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Identical files (symlinks or copies) found in multiple shader paths.\nEach unique file is tested exactly once.");
            }
            UI::PopSupport();
        }

        ImGui::Spacing();
        UI::TextSupport("A clean compile does not guarantee that a shader looks or behaves correctly.");
    }

    // Render detailed test results in collapsible sections
    // Call this separately after the main shader manager content
    static void renderShaderTestResults(
        const std::vector<std::tuple<std::string, std::string, bool, std::string>>& results,
        const std::set<std::string>& depthShaderNames)
    {
        if (results.empty())
            return;

        const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
        const float d = m.density;
        const float rowStride = 30.0f * d;
        const float maxBodyH = 240.0f * d;

        // bucket the results once, so the bodies can be virtualized
        std::vector<size_t> failed, depth, safe;
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
                               const char* tooltip, bool* open, ImVec4 nameColor) {
            if (indices.empty())
                return;
            const std::string badge = std::to_string(indices.size());
            const float reveal = UI::FoldHeader(id, label, badge.c_str(), open);
            if (!UI::FoldBodyBegin(id, reveal, indices.size() * rowStride, maxBodyH))
                return;

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float gap = 4.0f * d;
            ImFont* nameFont = ImGuiM3FontMedium();
            const float namePx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyMedium).size * d;
            const ImU32 nameCol = ImGui::GetColorU32(nameColor);

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
                    ImGui::InvisibleButton("##shader_row", ImVec2(itemW, rowStride));
                    const bool hovered = ImGui::IsItemHovered();
                    const ImRect itemRect(ImVec2(itemMin.x, itemMin.y + gap * 0.5f),
                                          ImVec2(itemMin.x + itemW, itemMin.y + rowStride - gap * 0.5f));
                    const float rr = ImGuiM3Radius(ImGuiM3Shape_Small);
                    const ImGuiM3ShapeRounding round{ rr, rr, rr, rr };
                    ImGuiM3PathRoundedRect(dl, itemRect, round, ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainer));
                    if (hovered)
                        ImGuiM3DrawStateLayer(dl, itemRect, round, ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);

                    const ImVec2 nameSize = ImGui::CalcTextSize(name.c_str());
                    dl->AddText(nameFont, namePx,
                                ImVec2(itemMin.x + 14.0f * d, itemRect.GetCenter().y - nameSize.y * 0.5f),
                                nameCol, name.c_str());

                    if (!error.empty())
                    {
                        const float warnPx = 11.0f * d;
                        ImFont* bodyBold = ImGuiM3FontBodyBold();
                        dl->AddText(bodyBold, warnPx,
                                    ImVec2(itemRect.Max.x - 16.0f * d - ImGui::CalcTextSize("warnings").x * 0.75f,
                                           itemRect.GetCenter().y - warnPx * 0.7f),
                                    ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant), "warnings");
                    }

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
                    ImGui::PopID();
                }
            }
            UI::FoldBodyEnd();
            (void)tooltip;
        };

        renderGroup("##failed_shader_group", "Failed shaders", failed,
                    "These shaders did not compile.", &failedOpen, UI::Error());
        renderGroup("##depth_shader_group", "Depth shaders", depth,
                    "These shaders require depth buffer access.", &depthOpen, UI::Secondary());
        renderGroup("##safe_shader_group", "Safe shaders", safe,
                    "Compiled cleanly and need no depth buffer.", &safeOpen, UI::Success());
    }

} // namespace VKIntox

// Expose renderShaderTestResults for use by view_shader_manager.cpp
namespace VKIntox
{
    void renderShaderTestResultsUI(
        const std::vector<std::tuple<std::string, std::string, bool, std::string>>& results,
        const std::set<std::string>& depthShaderNames)
    {
        renderShaderTestResults(results, depthShaderNames);
    }
}
