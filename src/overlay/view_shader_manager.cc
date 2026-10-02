#include "imgui_overlay.hh"
#include "config_serializer.hh"
#include "logger.hh"
#include "overlay/ui_icons.hh"

#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <set>
#include <chrono>
#include <string>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imfilebrowser.h"

#ifdef VKINTOX_HAVE_GIO
#include <gio/gio.h>
#include <glib.h>

namespace
{
    // XDG Desktop Portal file chooser, driven asynchronously so the game never
    // blocks. GIO talks to org.freedesktop.portal on the session bus, which is
    // the sanctioned path inside Flatpak (the sandbox has no direct filesystem
    // browsing) and works on the desktop too. The ImGui browser stays as the
    // fallback when the portal is missing or fails.
    //
    // We start the request from the render thread, then pump the default GMainContext
    // non-blockingly once per frame in pollPortalRequest().

    enum class PortalResult { None, Success, Cancelled, Unavailable };

    struct PortalRequest
    {
        bool pending = false;
        bool done = false;
        PortalResult result = PortalResult::None;
        std::string path;
        GDBusConnection* bus = nullptr;
        guint subscription = 0;
        std::chrono::steady_clock::time_point deadline;
    };

    PortalRequest g_portal;

    void onPortalResponse(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                          GVariant* parameters, gpointer)
    {
        guint response = 2;   // 0 = success, 1 = cancelled, 2 = other
        GVariant* results = nullptr;
        g_variant_get(parameters, "(u@a{sv})", &response, &results);
        if (response == 0 && results)
        {
            GVariant* uris = g_variant_lookup_value(results, "uris", G_VARIANT_TYPE_STRING_ARRAY);
            if (uris)
            {
                gsize n = 0;
                const gchar** items = g_variant_get_strv(uris, &n);
                if (n > 0 && items[0])
                {
                    gchar* path = g_filename_from_uri(items[0], nullptr, nullptr);
                    if (path)
                    {
                        g_portal.path = path;
                        g_free(path);
                    }
                }
                g_free((gpointer)items);
                g_variant_unref(uris);
            }
        }
        if (results)
            g_variant_unref(results);
        g_portal.result = (!g_portal.path.empty()) ? PortalResult::Success : PortalResult::Cancelled;
        g_portal.done = true;
    }

    void onPortalCallDone(GObject* source, GAsyncResult* res, gpointer)
    {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
        if (!reply)
        {
            if (error)
                g_error_free(error);
            g_portal.result = PortalResult::Unavailable;
            g_portal.done = true;
        }
        else
        {
            g_variant_unref(reply);
        }
    }

    bool startPortalDirectoryRequest(const std::string& title)
    {
        if (g_portal.pending)
            return true;

        g_portal = PortalRequest{};
        g_portal.pending = true;
        g_portal.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);

        GError* error = nullptr;
        GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (!bus)
        {
            if (error)
                g_error_free(error);
            g_portal = PortalRequest{};
            return false;
        }
        g_portal.bus = bus;

        // Request path is /org/freedesktop/portal/desktop/request/<sender>/<token>
        // where <sender> is our unique name without ':' and with '.' -> '_'.
        const gchar* sender = g_dbus_connection_get_unique_name(bus);
        std::string sender_token = sender ? (sender + 1) : "0";
        std::replace(sender_token.begin(), sender_token.end(), '.', '_');
        const std::string token = "vkintox" + std::to_string(g_random_int());
        const std::string request_path =
            "/org/freedesktop/portal/desktop/request/" + sender_token + "/" + token;

        // Subscribe before calling, or a fast response can be missed.
        g_portal.subscription = g_dbus_connection_signal_subscribe(
            bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request", "Response",
            request_path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE, onPortalResponse, nullptr, nullptr);

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
        g_variant_builder_add(&options, "{sv}", "directory", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(FALSE));
        g_variant_builder_add(&options, "{sv}", "modal", g_variant_new_boolean(TRUE));

        // Empty parent_window is valid and required under Flatpak; an X11/Wayland
        // handle from inside the sandbox would be rejected. Async so we return now.
        g_dbus_connection_call(
            bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.FileChooser", "OpenFile",
            g_variant_new("(ssa{sv})", "", title.c_str(), &options),
            G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, onPortalCallDone, nullptr);
        return true;
    }

    // Pumps the portal's callbacks for this frame. Returns true once a result is
    // ready, writing it to outResult/outPath.
    bool pollPortalRequest(PortalResult& outResult, std::string& outPath)
    {
        if (!g_portal.pending)
            return false;

        while (g_main_context_iteration(nullptr, FALSE))
        {
        }

        if (!g_portal.done && std::chrono::steady_clock::now() > g_portal.deadline)
        {
            g_portal.result = PortalResult::Unavailable;
            g_portal.done = true;
        }
        if (!g_portal.done)
            return false;

        outResult = g_portal.result;
        outPath = g_portal.path;

        if (g_portal.bus && g_portal.subscription)
            g_dbus_connection_signal_unsubscribe(g_portal.bus, g_portal.subscription);
        if (g_portal.bus)
            g_object_unref(g_portal.bus);
        g_portal = PortalRequest{};
        return true;
    }

    bool portalRequestPending() { return g_portal.pending; }
}
#else
namespace
{
    enum class PortalResult { None, Success, Cancelled, Unavailable };
    bool startPortalDirectoryRequest(const std::string&) { return false; }
    bool pollPortalRequest(PortalResult&, std::string&) { return false; }
    bool portalRequestPending() { return false; }
}
#endif

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
            PortalResult result = PortalResult::None;
            std::string picked;
            if (pollPortalRequest(result, picked))
            {
                if (result == PortalResult::Success && !picked.empty())
                {
                    if (std::find(shaderMgrParentDirs.begin(), shaderMgrParentDirs.end(), picked) == shaderMgrParentDirs.end())
                    {
                        shaderMgrParentDirs.push_back(picked);
                        saveConfig();
                    }
                }
                else if (result == PortalResult::Unavailable)
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

        const bool portalPending = portalRequestPending();
        ImGui::BeginDisabled(portalPending);
        const std::string browseLabel = std::string(Icon::FolderOpenUtf8) + (portalPending ? "  Opening..." : "  Browse...");
        if (ImGui::Button(browseLabel.c_str()))
        {
            // If the portal cannot even be started (no GIO, no session bus),
            // open the in-overlay browser right away.
            if (!startPortalDirectoryRequest("Select Parent Directory"))
            {
                dirBrowser.SetTitle("Select Parent Directory");
                const char* home = std::getenv("HOME");
                dirBrowser.SetPwd(home ? home : "/");
                dirBrowser.Open();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        const std::string rescanLabel = std::string(Icon::RefreshUtf8) + "  Rescan All";
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
                ImGui::TextDisabled("None - click Rescan All");
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
                ImGui::TextDisabled("None - click Rescan All");
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
