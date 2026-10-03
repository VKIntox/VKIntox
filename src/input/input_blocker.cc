#include "input_blocker.hh"
#include "wayland_display.hh"
#include "wayland_interpose.hh"
#include "x11_display.hh"
#include "logger.hh"

#include <X11/Xlib.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace VKIntox
{
    static std::atomic<bool> blockingEnabled{false};
    static std::atomic<bool> blocked{false};

    static std::mutex rectMutex;
    static std::vector<InputRect> inputRects;

    // updated every frame by the overlay, so hitboxes apply without the game
    // having to tell us anything.
    static std::atomic<float> pointerX{-100000.0f};
    static std::atomic<float> pointerY{-100000.0f};

    static bool x11Grabbed = false;
    // mirrors the wayland keyboard state so focus only flips on a crossing.
    static bool keyboardWithheld = false;

    static bool pointInRects(float x, float y)
    {
        std::lock_guard<std::mutex> lock(rectMutex);
        for (const InputRect& r : inputRects)
            if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height)
                return true;
        return false;
    }

    void setInputRects(const InputRect* rects, int count)
    {
        std::lock_guard<std::mutex> lock(rectMutex);
        inputRects.assign(rects, rects + (count > 0 ? count : 0));
    }

    // X11: the layer opens its own Display connection, so it is a distinct X
    // client from the game. A server-side grab on that connection takes pointer
    // and key events away from the game's connection entirely. Held only while
    // the pointer sits over an overlay hitbox, so it stays directional.
    static void setX11GrabState(bool grab)
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

    void setInputBlocked(bool onScreen)
    {
        blocked.store(onScreen, std::memory_order_release);

        if (isX11())
        {
            const bool enabled = blockingEnabled.load(std::memory_order_acquire);
            const bool inside = enabled && onScreen
                && pointInRects(pointerX.load(std::memory_order_acquire), pointerY.load(std::memory_order_acquire));
            setX11GrabState(inside);
            return;
        }

        if (isWayland() && !onScreen && keyboardWithheld)
        {
            // the overlay went away while it held the keyboard: hand it back.
            keyboardWithheld = false;
            withholdGameKeys(false);
        }
    }

    bool isInputBlockedAt(float x, float y)
    {
        return blocked.load(std::memory_order_acquire) && pointInRects(x, y);
    }

    bool isInputBlocked()
    {
        return blocked.load(std::memory_order_acquire);
    }

    void updatePointerPosition(float x, float y)
    {
        pointerX.store(x, std::memory_order_release);
        pointerY.store(y, std::memory_order_release);

        const bool onScreen = blocked.load(std::memory_order_acquire);
        const bool inside = onScreen && pointInRects(x, y);

        if (isX11())
        {
            setX11GrabState(blockingEnabled.load(std::memory_order_acquire) && inside);
            return;
        }

        if (isWayland() && inside != keyboardWithheld)
        {
            // withhold keys only while the cursor is over the overlay, so the
            // rest of the game keeps playing when the pointer is elsewhere.
            // releases/re-presses the exact held keys; mouse is left alone.
            keyboardWithheld = inside;
            withholdGameKeys(inside);
        }
    }

    void initInputBlocker(bool enabled)
    {
        blockingEnabled = enabled;

        if (isWayland())
        {
            Logger::debug(std::string("Input blocking ") + (enabled ? "enabled (Wayland: hitbox)" : "disabled"));
            return;
        }

        if (isX11())
        {
            Logger::debug(std::string("Input blocking ") + (enabled ? "enabled (X11: hitbox grab)" : "disabled"));
            return;
        }

        blocked.store(false, std::memory_order_release);
        Logger::debug(std::string("Input blocking ") + (enabled ? "disabled for unsupported surface" : "disabled"));
    }
} // namespace VKIntox
