#include "input_blocker.hh"
#include "wayland_display.hh"
#include "wayland_interpose.hh"
#include "x11_display.hh"
#include "logger.hh"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace VKIntox
{
    static std::atomic<bool> blockingEnabled{false};
    static std::atomic<bool> blocked{false};

    // the interactive shell is up (toasts alone do not count): the blocker may
    // take the keyboard away from the game at all
    static std::atomic<bool> interactive{false};
    // a text field or popup is consuming keys right now
    static std::atomic<bool> wantsKeyboard{false};

    static std::mutex rectMutex;
    static std::vector<InputRect> inputRects;

    // updated every frame by the overlay, so hitboxes apply without the game
    // having to tell us anything.
    static std::atomic<float> pointerX{-100000.0f};
    static std::atomic<float> pointerY{-100000.0f};

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

    static bool pointerOverOverlay()
    {
        return blocked.load(std::memory_order_acquire)
            && pointInRects(pointerX.load(std::memory_order_acquire), pointerY.load(std::memory_order_acquire));
    }

    static bool overlayConsumingKeys()
    {
        return blocked.load(std::memory_order_acquire)
            && interactive.load(std::memory_order_acquire)
            && wantsKeyboard.load(std::memory_order_acquire);
    }

    // X11: the pointer grab follows the cursor, the keyboard grab follows the
    // overlay actually consuming keys. Both make the server emit
    // NotifyGrab/NotifyUngrab focus events on the game window, and Wine reads
    // those as a window-manager grab and drops the game's cursor capture, so a
    // mere pointer crossing must never engage the keyboard grab. The grab
    // helpers hand held keys and buttons across each transition.
    static void applyX11Blocking()
    {
        const bool enabled = blockingEnabled.load(std::memory_order_acquire);
        const bool pointerGrab = enabled && pointerOverOverlay();
        const bool keyboardGrab = enabled && overlayConsumingKeys();

        x11SetGrabState(pointerGrab);
        x11SetKeyboardGrabState(keyboardGrab);
    }

    // Wayland withholds on the game's own event stream, which costs it nothing
    // (no grabs, no focus churn), so there the keyboard is withheld for the
    // pointer crossing as well as while the overlay consumes keys.
    static void applyWaylandBlocking()
    {
        const bool enabled = blockingEnabled.load(std::memory_order_acquire);
        const bool withhold = enabled && (pointerOverOverlay() || overlayConsumingKeys());

        if (withhold != keyboardWithheld)
        {
            // releases/re-presses the exact held keys; mouse is left alone.
            keyboardWithheld = withhold;
            withholdGameKeys(withhold);
        }
    }

    static void applyBlocking()
    {
        if (isX11())
        {
            applyX11Blocking();
            return;
        }

        if (isWayland())
            applyWaylandBlocking();
    }

    void setInputRects(const InputRect* rects, int count)
    {
        std::lock_guard<std::mutex> lock(rectMutex);
        inputRects.assign(rects, rects + (count > 0 ? count : 0));
    }

    void setInputBlocked(bool onScreen)
    {
        blocked.store(onScreen, std::memory_order_release);
        applyBlocking();
    }

    void setInputInteractive(bool value)
    {
        interactive.store(value, std::memory_order_release);
        applyBlocking();
    }

    void setOverlayWantsKeyboard(bool value)
    {
        wantsKeyboard.store(value, std::memory_order_release);
        applyBlocking();
    }

    bool isInputBlockedAt(float x, float y)
    {
        return blocked.load(std::memory_order_acquire) && pointInRects(x, y);
    }

    bool isInputBlocked()
    {
        return blocked.load(std::memory_order_acquire);
    }

    bool inputBlockingEnabled()
    {
        return blockingEnabled.load(std::memory_order_acquire);
    }

    bool inputGrabActive()
    {
        return isX11() && x11GrabActive();
    }

    bool inputKeyboardWithheld()
    {
        return keyboardWithheld;
    }

    bool inputPointerOverOverlay()
    {
        return pointerOverOverlay();
    }

    void updatePointerPosition(float x, float y)
    {
        pointerX.store(x, std::memory_order_release);
        pointerY.store(y, std::memory_order_release);
        applyBlocking();
    }

    void initInputBlocker(bool enabled)
    {
        blockingEnabled = enabled;

        if (isX11())
        {
            Logger::debug(std::string("Input blocking ") + (enabled ? "enabled (X11: hitbox grab)" : "disabled"));
            return;
        }

        if (isWayland())
        {
            Logger::debug(std::string("Input blocking ") + (enabled ? "enabled (Wayland: hitbox)" : "disabled"));
            return;
        }

        blocked.store(false, std::memory_order_release);
        Logger::debug(std::string("Input blocking ") + (enabled ? "disabled for unsupported surface" : "disabled"));
    }
} // namespace VKIntox
