#pragma once

struct wl_proxy;

namespace VKIntox
{
    // Register overlay-owned proxies so the interposition layer skips them.
    // Call BEFORE wl_pointer_add_listener / wl_keyboard_add_listener.
    void registerOverlayProxy(wl_proxy* proxy);
    void unregisterOverlayProxy(wl_proxy* proxy);

    // True once the interposer wrapped a game pointer/keyboard listener. Games
    // that bound their seat before the layer loaded are never wrapped; their
    // input then flows to the layer only through the layer's own bindings.
    bool waylandPointerWrapped();
    bool waylandKeyboardWrapped();

    // Release every key the wrapped game keyboards saw a press for (or
    // re-press whatever it still owns). Edge-triggered by the input blocker
    // when keyboard blocking engages or lifts.
    void withholdGameKeys(bool withhold);
}
