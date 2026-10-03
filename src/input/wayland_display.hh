#pragma once

struct wl_display;
struct wl_surface;

namespace VKIntox
{
    // Which graphics WSI the game handed us. The layer processes Wayland and
    // X11; anything else passes straight through.
    enum class SurfaceKind
    {
        Unknown = 0,
        Wayland,
        X11,
    };

    SurfaceKind getSurfaceKind();

    // True once the game created a Wayland Vulkan surface.
    bool isWayland();

    // True once the game created an Xlib/Xcb Vulkan surface.
    bool isX11();

    // Called from vkCreateWaylandSurfaceKHR to capture the game's wl_display.
    void setWaylandDisplay(wl_display* display);

    // Returns the captured wl_display, or nullptr if not on Wayland.
    wl_display* getWaylandDisplay();

    // Called from vkCreateWaylandSurfaceKHR to capture the game's wl_surface.
    void setWaylandSurface(wl_surface* surface);

    // Returns the captured wl_surface, or nullptr if not on Wayland.
    wl_surface* getWaylandSurface();

    // Called from vkCreateXlib/XcbSurfaceKHR to capture the game's XID. Xlib's
    // `None` and the rest of X11/X.h stay out of this header on purpose: they
    // collide with symbols the codebase already defines, so the X11 headers
    // live only in the isolated input translation units.
    void setX11Window(unsigned long window);

    // Returns the captured XID, or 0 if not on X11.
    unsigned long getX11Window();
} // namespace VKIntox
