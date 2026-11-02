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

    // Holds or releases a server-side grab on the game window so the game stops
    // seeing input while the overlay is used. Call pumpX11Input() on every frame
    // to keep the redirected events flowing.
    //
    // The pointer grab blocks clicks and motion, the keyboard grab blocks keys.
    // Both make the server emit NotifyGrab/NotifyUngrab focus events on the game
    // window, and Wine reads those as "the window manager is manipulating the
    // window" and throws away the game's cursor capture, so the keyboard grab is
    // only requested while the overlay is actually consuming keys (see the input
    // blocker). Every transition also hands the game's held keys/buttons over to
    // it with synthetic events, because the grab would otherwise swallow their
    // releases and leave the game holding them.
    void x11SetGrabState(bool grab);
    void x11SetKeyboardGrabState(bool grab);
    bool x11GrabActive();
} // namespace VKIntox
