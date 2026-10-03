#pragma once

#include <cstdint>

struct _XDisplay;

namespace VKIntox
{
    // Shared X11 connection for the input backends and the input blocker. One
    // connection means one event queue: XInput2 raw events, active-grab events,
    // and query-based polling all drain from the same place.
    _XDisplay* x11Display();
    bool x11Available();

    // The game window captured from the Vulkan surface, as an XID, or 0.
    unsigned long x11GameWindow();

    // Raw key transitions, from XInput2 and from an active grab.
    using X11KeyCallback = void (*)(uint32_t keycode, bool pressed);
    using X11ButtonCallback = void (*)(uint32_t button, bool pressed);
    void setX11KeyCallback(X11KeyCallback cb);
    void setX11ButtonCallback(X11ButtonCallback cb);

    // Drains pending events and hands key/button transitions to the callbacks.
    // Call once per frame; a no-op when X11 is absent.
    void pumpX11Input();
} // namespace VKIntox
