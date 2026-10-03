#include "input_blocker.hh"
#include "wayland_display.hh"
#include "wayland_interpose.hh"
#include "x11_display.hh"
#include "logger.hh"

#include <X11/Xlib.h>

#include <atomic>
#include <string>

namespace VKIntox
{
    static std::atomic<bool> blockingEnabled{false};
    // Atomic: written by the overlay thread (setInputBlocked), read by the game
    // thread (isInputBlocked via Wayland interpose wrapper callbacks).
    static std::atomic<bool> blocked{false};
    static bool x11Grabbed = false;

    static void warnUnsupportedInputOnce(const char* message)
    {
        static bool warned = false;
        if (!warned && !isWayland() && !isX11())
        {
            Logger::warn(message);
            warned = true;
        }
    }

    // X11 has no event-consumption mode like the Wayland interpose, so blocking
    // is a server-side grab: while held, key/button events route to us and the
    // game sees none. Raw XInput2 events are unaffected and still feed our own
    // text/wheel polling.
    static void setX11Grab(bool grab)
    {
        Display* display = x11Display();
        if (!display || grab == x11Grabbed)
            return;

        const unsigned long gameWindow = x11GameWindow();
        const Window grabWindow = gameWindow ? (Window)gameWindow : DefaultRootWindow(display);

        int keyboardStatus = GrabSuccess;
        int pointerStatus = GrabSuccess;
        if (grab)
        {
            keyboardStatus = XGrabKeyboard(display, grabWindow, False, GrabModeAsync, GrabModeAsync, CurrentTime);
            pointerStatus = XGrabPointer(display, grabWindow, False,
                                         ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        }
        else
        {
            XUngrabKeyboard(display, CurrentTime);
            XUngrabPointer(display, CurrentTime);
        }
        XFlush(display);
        x11Grabbed = grab;

        if (grab && (keyboardStatus != GrabSuccess || pointerStatus != GrabSuccess))
            Logger::warn("X11 input grab incomplete: keyboard=" + std::to_string(keyboardStatus)
                         + " pointer=" + std::to_string(pointerStatus));
    }

    void initInputBlocker(bool enabled)
    {
        blockingEnabled = enabled;

        if (isWayland())
        {
            // Wayland doesn't support global input grabs.
            // Input events are delivered to our private event queue
            // and consumed by the overlay when visible.
            Logger::debug(std::string("Input blocking ") + (enabled ? "enabled (Wayland: event consumption mode)" : "disabled"));
            return;
        }

        if (isX11())
        {
            Logger::debug(std::string("Input blocking ") + (enabled ? "enabled (X11: active grab)" : "disabled"));
            return;
        }

        blocked.store(false, std::memory_order_release);
        warnUnsupportedInputOnce("unsupported Vulkan surface: input blocking disabled; pass-through only");
        Logger::debug(std::string("Input blocking ") + (enabled ? "disabled for unsupported surface" : "disabled"));
    }

    void setInputBlocked(bool shouldBlock)
    {
        if (!blockingEnabled.load(std::memory_order_acquire))
            return;

        if (isX11())
        {
            if (shouldBlock == blocked.load(std::memory_order_acquire))
                return;
            blocked.store(shouldBlock, std::memory_order_release);
            setX11Grab(shouldBlock);
            return;
        }

        if (!isWayland())
        {
            blocked.store(false, std::memory_order_release);
            return;
        }

        if (shouldBlock == blocked.load(std::memory_order_acquire))
            return;

        blocked.store(shouldBlock, std::memory_order_release);

        // On Wayland, interposed wl_proxy_add_listener wrapper callbacks
        // check isInputBlocked() and suppress events to the game.
        // NOTE: This does NOT work for Wine Wayland games — Wine loads
        // winewayland.so via dlopen(RTLD_LOCAL), so libwayland-client
        // resolves in Wine's local scope, bypassing our LD_PRELOAD
        // interposition entirely. No workaround exists without LD_AUDIT
        // or a wrapper libwayland-client.so.
        Logger::debug(std::string("Wayland input blocking: ") + (shouldBlock ? "suppressing game events" : "forwarding game events"));
        // Send synthetic leave/enter to game keyboards so held keys
        // are released when overlay opens (prevents stuck movement/actions).
        // Only works when wl_proxy_add_listener interposition is active.
        notifyGameKeyboardFocus(!shouldBlock);
    }

    bool isInputBlocked()
    {
        return blocked.load(std::memory_order_acquire);
    }
} // namespace VKIntox
