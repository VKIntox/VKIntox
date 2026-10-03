#include "util.hh"

#include "c_resource.hh"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

#ifdef VKINTOX_HAVE_GIO
#include <gio/gio.h>
#include <glib.h>
#endif

namespace VKIntox
{
    void addUniqueCString(std::vector<const char*>& stringVector, const char* addString)
    {
        for (const char* other : stringVector)
        {
            if (other == std::string(addString))
            {
                return;
            }
        }
        stringVector.push_back(addString);
    }

    void outputInColor(std::string output, Color foreground, Color background)
    {
        std::vector<std::string> magicNumbers;
        switch (foreground)
        {
            case Color::black: magicNumbers.push_back("30"); break;
            case Color::red: magicNumbers.push_back("31"); break;
            case Color::green: magicNumbers.push_back("32"); break;
            case Color::yellow: magicNumbers.push_back("33"); break;
            case Color::blue: magicNumbers.push_back("34"); break;
            case Color::magenta: magicNumbers.push_back("35"); break;
            case Color::cyan: magicNumbers.push_back("36"); break;
            case Color::white: magicNumbers.push_back("37"); break;
            default: break;
        }
        switch (background)
        {
            case Color::black: magicNumbers.push_back("40"); break;
            case Color::red: magicNumbers.push_back("41"); break;
            case Color::green: magicNumbers.push_back("42"); break;
            case Color::yellow: magicNumbers.push_back("43"); break;
            case Color::blue: magicNumbers.push_back("44"); break;
            case Color::magenta: magicNumbers.push_back("45"); break;
            case Color::cyan: magicNumbers.push_back("46"); break;
            case Color::white: magicNumbers.push_back("47"); break;
            default: break;
        }
        std::string magicString = "";
        for (bool first = true; auto& magicNumber : magicNumbers)
        {
            if (!first)
            {
                magicString += ";";
            }
            magicString += magicNumber;
            first = false;
        }
        if (magicString.size() == 0 || !isatty(fileno(stdout)))
        {
            std::cout << output << std::endl;
        }
        else
        {
            std::cout << "\033[" << magicString << "m" << output << "\033[0m" << std::endl;
        }
    }
    bool openInShell(const char* url)
    {
        if (url == nullptr || *url == '\0')
            return false;

        // a flatpak sandbox cannot reach the host's openers directly, so hand the
        // url to the host. everywhere else xdg-open is the standard opener.
        const bool flatpak = access("/.flatpak-info", F_OK) == 0;
        const char* args[5] = {};
        int arg = 0;
        if (flatpak)
        {
            args[arg++] = "flatpak-spawn";
            args[arg++] = "--host";
        }
        args[arg++] = "xdg-open";
        args[arg++] = url;

        // double-fork so the opener is detached from the game and cannot linger
        // as a zombie. only async-signal-safe calls run between fork and exec.
        const pid_t pid = fork();
        if (pid < 0)
            return false;
        if (pid == 0)
        {
            setsid();
            const pid_t grandchild = fork();
            if (grandchild == 0)
            {
                execvp(args[0], const_cast<char* const*>(args));
                _exit(127);
            }
            _exit(grandchild < 0 ? 127 : 0);
        }

        int status = 0;
        waitpid(pid, &status, 0);
        return true;
    }

    bool writeAtomically(const std::string& path, const std::string& contents)
    {
        std::string temporary = path + ".tmp-XXXXXX";
        std::vector<char> name(temporary.begin(), temporary.end());
        name.push_back('\0');
        // mkstemp rewrites name in place, so the real path only exists after
        // the call, which is what makes unlink-on-failure target the right file
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
        // deferred writeback a full or failed disk only reports itself here
        if (!fd.close())
            success = false;

        if (success && std::rename(name.data(), path.c_str()) == 0)
            return true;
        unlink(name.data());
        return false;
    }

#ifdef VKINTOX_HAVE_GIO
    namespace
    {
        // GIO talks to org.freedesktop.portal over the session bus, which is the
        // sanctioned path inside Flatpak (the sandbox has no direct filesystem
        // browsing) and works on the desktop too. callbacks fire on the default
        // GMainContext, pumped non-blockingly by pollFileDialog.
        enum class PortalResult { None, Success, Cancelled, Unavailable };

        struct PortalRequest
        {
            bool pending = false;
            bool done = false;
            FileDialogKind kind = FileDialogKind::OpenFile;
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

        bool startPortalRequest(const std::string& title, FileDialogKind kind,
                                const std::vector<std::string>& globFilters)
        {
            if (g_portal.pending)
                return true;

            g_portal = PortalRequest{};
            g_portal.pending = true;
            g_portal.kind = kind;
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
            g_variant_builder_add(&options, "{sv}", "directory", g_variant_new_boolean(kind == FileDialogKind::OpenDirectory));
            g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(FALSE));
            g_variant_builder_add(&options, "{sv}", "modal", g_variant_new_boolean(TRUE));
            if (kind == FileDialogKind::OpenFile && !globFilters.empty())
            {
                // filters is a(sa(us)): (name, [(0 = glob, pattern)]). all the
                // patterns share one user-visible filter.
                GVariantBuilder patterns;
                g_variant_builder_init(&patterns, G_VARIANT_TYPE("a(us)"));
                for (const auto& glob : globFilters)
                    g_variant_builder_add(&patterns, "(us)", static_cast<guint32>(0), glob.c_str());
                GVariantBuilder filters;
                g_variant_builder_init(&filters, G_VARIANT_TYPE("a(sa(us))"));
                g_variant_builder_add(&filters, "(sa(us))", "Matching files", &patterns);
                g_variant_builder_add(&options, "{sv}", "filters", g_variant_builder_end(&filters));
            }

            // Empty parent_window is valid and required under Flatpak; an X11/Wayland
            // handle from inside the sandbox would be rejected. Async so we return now.
            g_dbus_connection_call(
                bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.FileChooser", "OpenFile",
                g_variant_new("(ssa{sv})", "", title.c_str(), &options),
                G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, onPortalCallDone, nullptr);
            return true;
        }

        // pumps the portal's callbacks for this frame. returns false while the
        // request is pending or belongs to another kind, so it stays queued for
        // whoever started it.
        bool pollPortal(FileDialogKind kind, PortalResult& outResult, std::string& outPath)
        {
            if (!g_portal.pending || g_portal.kind != kind)
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
    } // anonymous namespace
#endif

    bool startOpenFileDialog(const std::string& title, const std::vector<std::string>& globFilters)
    {
#ifdef VKINTOX_HAVE_GIO
        return startPortalRequest(title, FileDialogKind::OpenFile, globFilters);
#else
        (void)title;
        (void)globFilters;
        return false;
#endif
    }

    bool startOpenDirectoryDialog(const std::string& title)
    {
#ifdef VKINTOX_HAVE_GIO
        return startPortalRequest(title, FileDialogKind::OpenDirectory, {});
#else
        (void)title;
        return false;
#endif
    }

    bool pollFileDialog(FileDialogKind kind, FileDialogResult& outResult, std::string& outPath)
    {
#ifdef VKINTOX_HAVE_GIO
        PortalResult result = PortalResult::None;
        if (!pollPortal(kind, result, outPath))
            return false;
        switch (result)
        {
            case PortalResult::Success: outResult = FileDialogResult::Success; break;
            case PortalResult::Cancelled: outResult = FileDialogResult::Cancelled; break;
            default: outResult = FileDialogResult::Unavailable; break;
        }
        return true;
#else
        (void)kind;
        (void)outResult;
        (void)outPath;
        return false;
#endif
    }

    bool fileDialogPending()
    {
#ifdef VKINTOX_HAVE_GIO
        return g_portal.pending;
#else
        return false;
#endif
    }
} // namespace VKIntox
