#include "input/common/mouse_input.hh"

#include "util/logger.hh"
#include "input/wayland/wayland_display.hh"

#include "input/wayland/mouse_input_wayland.hh"
#include "input/x11/mouse_input_x11.hh"

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
