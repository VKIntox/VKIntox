#include "mouse_input.hh"

#include "logger.hh"
#include "wayland_display.hh"
#include "mouse_input_wayland.hh"

namespace VKIntox
{
    static void warnNonWaylandMouseOnce(const char* message)
    {
        static bool warned = false;
        if (!warned && isNonWaylandSurface())
        {
            Logger::warn(message);
            warned = true;
        }
    }

    MouseState getMouseState()
    {
        if (isWayland())
            return getMouseStateWayland();

        warnNonWaylandMouseOnce("non-Wayland Vulkan surface: mouse polling disabled; returning default state");
        return MouseState();
    }

} // namespace VKIntox
