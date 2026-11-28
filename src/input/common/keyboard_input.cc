#include "input/common/keyboard_input.hh"

#include "util/logger.hh"
#include "input/wayland/wayland_display.hh"

#include "input/wayland/keyboard_input_wayland.hh"
#include "input/x11/keyboard_input_x11.hh"
#include "input/common/input_blocker.hh"

#include <mutex>
#include <unordered_set>

namespace VKIntox
{
    namespace
    {
        std::mutex keyboardEventMutex;
        std::vector<KeyboardEvent> keyboardEvents;
        std::unordered_set<uint32_t> queuedKeys;
    }

    void queueKeyboardEvent(uint32_t keysym, bool down, bool repeat)
    {
        if (!keysym)
            return;
        std::lock_guard<std::mutex> lock(keyboardEventMutex);
        if (down)
        {
            if (!isOverlayHoldingKeyboard())
                return;
            queuedKeys.insert(keysym);
        }
        else if (!queuedKeys.erase(keysym) && !isOverlayHoldingKeyboard())
        {
            return;
        }
        keyboardEvents.push_back({keysym, down, repeat});
    }

    std::vector<KeyboardEvent> drainKeyboardEvents()
    {
        std::lock_guard<std::mutex> lock(keyboardEventMutex);
        std::vector<KeyboardEvent> events;
        events.swap(keyboardEvents);
        return events;
    }

    static void warnUnsupportedKeyboardOnce(const char* message)
    {
        static bool warned = false;
        if (!warned && !isWayland() && !isX11())
        {
            Logger::warn(message);
            warned = true;
        }
    }

    uint32_t convertToKeySym(std::string key)
    {
        if (isWayland())
            return convertToKeySymWayland(key);
        if (isX11())
            return convertToKeySymX11(key);
        return 0u;
    }

    void beginKeyboardInputFrame()
    {
        // Wayland resets its own frame in beginWaylandInputFrame(); the x11
        // backend needs the keymap cache invalidated once per present.
        if (isX11())
            beginKeyboardInputFrameX11();
    }

    bool isKeyPressed(uint32_t ks)
    {
        if (isWayland())
            return isKeyPressedWayland(ks);
        if (isX11())
            return isKeyPressedX11(ks);

        warnUnsupportedKeyboardOnce("unsupported Vulkan surface: keyboard polling disabled; returning no input");
        return false;
    }

    KeyboardState getKeyboardState()
    {
        KeyboardState state;
        if (isWayland())
            state = getKeyboardStateWayland();
        else if (isX11())
            state = getKeyboardStateX11();
        else
            warnUnsupportedKeyboardOnce("unsupported Vulkan surface: keyboard polling disabled; returning default state");
        state.events = drainKeyboardEvents();
        return state;
    }
} // namespace VKIntox
