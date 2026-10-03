#include "wayland_display.hh"

#include "logger.hh"

#include <atomic>
#include <string>

namespace VKIntox
{
    static std::atomic<int> surfaceKind{static_cast<int>(SurfaceKind::Unknown)};
    static std::atomic<wl_display*> waylandDisplay{nullptr};
    static std::atomic<wl_surface*> waylandSurface{nullptr};
    static std::atomic<unsigned long> x11Window{0};

    // first surface wins: an app drives one WSI, and a late second kind must not
    // steal the capture from the one it actually presents with.
    static bool claimKind(SurfaceKind kind)
    {
        int current = surfaceKind.load(std::memory_order_acquire);
        if (current == static_cast<int>(SurfaceKind::Unknown))
            return surfaceKind.compare_exchange_strong(current, static_cast<int>(kind),
                                                       std::memory_order_acq_rel);
        return current == static_cast<int>(kind);
    }

    SurfaceKind getSurfaceKind()
    {
        return static_cast<SurfaceKind>(surfaceKind.load(std::memory_order_acquire));
    }

    bool isWayland()
    {
        return getSurfaceKind() == SurfaceKind::Wayland;
    }

    bool isX11()
    {
        return getSurfaceKind() == SurfaceKind::X11;
    }

    void setWaylandDisplay(wl_display* display)
    {
        if (!display || !claimKind(SurfaceKind::Wayland))
            return;
        waylandDisplay.store(display, std::memory_order_release);
        Logger::info("captured Wayland display from vkCreateWaylandSurfaceKHR");
    }

    wl_display* getWaylandDisplay()
    {
        return waylandDisplay.load(std::memory_order_acquire);
    }

    void setWaylandSurface(wl_surface* surface)
    {
        if (!surface || !claimKind(SurfaceKind::Wayland))
            return;
        waylandSurface.store(surface, std::memory_order_release);
        Logger::info("captured Wayland surface from vkCreateWaylandSurfaceKHR");
    }

    wl_surface* getWaylandSurface()
    {
        return waylandSurface.load(std::memory_order_acquire);
    }

    void setX11Window(unsigned long window)
    {
        if (!window || !claimKind(SurfaceKind::X11))
            return;
        x11Window.store(window, std::memory_order_release);
        Logger::info("captured X11 window from a Vulkan surface; processing X11 surface");
    }

    unsigned long getX11Window()
    {
        return x11Window.load(std::memory_order_acquire);
    }
} // namespace VKIntox
