#pragma once

#include <cstdint>

#include "input/common/input_blocker.hh"

struct _XDisplay;

namespace VKIntox
{
    // Shared X11 connection for the input backends and the input blocker. One
    // connection means one event queue: XInput2 raw events, keyboard-grab
    // events, and query-based polling all drain from the same place.
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

    // Maps input-only "shield" windows over the overlay hitboxes so clicks and
    // motion land on this connection instead of the game — the pointer
    // equivalent of the Wayland capture surface, without any grab. The game
    // window is the parent, so the rects are window-relative. Pass no rects or
    // active=false to unmap everything.
    void x11UpdateInputShields(const InputRect* rects, int count, bool active);

    // Takes a server-side keyboard grab on the game window while the overlay
    // consumes keys (a text field or popup has focus), and releases it
    // otherwise. Only ever engaged there: a grab transition makes Wine read
    // NotifyGrab as a window-manager grab and drop the game's cursor capture.
    // Every transition hands the game's held keys over with synthetic events,
    // because the grab would otherwise swallow their releases and leave the
    // game holding them. A watchdog releases the grab if the render loop
    // stalls, so a hung frame can never lock the keyboard.
    void x11SetKeyboardGrabState(bool grab);
    // keys the keyboard-grab handoff cleared server-side while still held
    bool x11KeySynthHeld(unsigned int keycode);
    bool x11GrabActive();
} // namespace VKIntox
