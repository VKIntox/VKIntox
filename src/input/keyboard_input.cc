#include "keyboard_input.hh"

#include "logger.hh"
#include "wayland_display.hh"

#include "keyboard_input_wayland.hh"
#include "keyboard_input_x11.hh"

namespace VKIntox
{
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

    bool isKeyDown(uint32_t ks)
    {
        if (isWayland())
            return isKeyDownWayland(ks);
        if (isX11())
            return isKeyDownX11(ks);

        warnUnsupportedKeyboardOnce("unsupported Vulkan surface: keyboard polling disabled; returning no input");
        return false;
    }

    KeyboardState getKeyboardState()
    {
        if (isWayland())
            return getKeyboardStateWayland();
        if (isX11())
            return getKeyboardStateX11();

        warnUnsupportedKeyboardOnce("unsupported Vulkan surface: keyboard polling disabled; returning default state");
        return KeyboardState();
    }
} // namespace VKIntox
