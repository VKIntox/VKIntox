#include "mouse_input.hh"

#include "logger.hh"
#include "wayland_display.hh"

#include "mouse_input_wayland.hh"
#include "mouse_input_x11.hh"

namespace VKIntox
{
    static void warnUnsupportedMouseOnce(const char* message)
    {
        static bool warned = false;
        if (!warned && !isWayland() && !isX11())
        {
            Logger::warn(message);
            warned = true;
        }
    }

    MouseState getMouseState()
    {
        if (isWayland())
            return getMouseStateWayland();
        if (isX11())
            return getMouseStateX11();

        warnUnsupportedMouseOnce("unsupported Vulkan surface: mouse polling disabled; returning default state");
        return MouseState();
    }
} // namespace VKIntox
