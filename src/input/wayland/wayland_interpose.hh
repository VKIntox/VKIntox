#pragma once

struct wl_proxy;

namespace VKIntox
{
    // Register overlay-owned proxies so the interposition layer skips them.
    // Call BEFORE wl_pointer_add_listener / wl_keyboard_add_listener.
    void registerOverlayProxy(wl_proxy* proxy);
    void unregisterOverlayProxy(wl_proxy* proxy);

    // Release every key the wrapped game keyboards currently hold (or re-press
    // whatever is still physically held). Called when the pointer crosses the
    // overlay hitbox, so a held movement key stops without dropping keyboard
    // focus — a synthetic leave/enter stalls input until the next press.
    // Mouse buttons are deliberately untouched: a synthetic release would break
    // click/drag ordering. Only works when the listener/dispatcher interposition
    // is active.
    void withholdGameKeys(bool withhold);
}
