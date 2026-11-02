#pragma once

namespace VKIntox
{
    // An overlay hitbox, in surface pixels — the same space as wl_pointer
    // coordinates and x11 window-relative coordinates.
    struct InputRect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;

        bool operator==(const InputRect&) const = default;
    };

    // Call once at startup with the config value.
    void initInputBlocker(bool enabled);

    // Overlay hitboxes, refreshed every frame. Pointer events inside these are
    // withheld from the game; everything outside passes through to it.
    void setInputRects(const InputRect* rects, int count);

    // Global gate: is the overlay on screen at all. Cheap to check.
    void setInputBlocked(bool blocked);

    // The interactive shell is up (toasts alone do not count). The keyboard is
    // only taken away from the game while this is on: every grab transition
    // makes Wine drop the game's cursor capture, so the keyboard grab must not
    // follow a bare pointer crossing.
    void setInputInteractive(bool interactive);

    // The overlay is consuming keys right now (a text field is active or a popup
    // is open); with the shell up, this is what engages the keyboard grab.
    void setOverlayWantsKeyboard(bool wantsKeyboard);

    // Newest pointer position in surface pixels. Drives the x11 grabs and the
    // follow-the-pointer keyboard rule.
    void updatePointerPosition(float x, float y);

    // True when (x, y) is inside an overlay hitbox and blocking is on.
    bool isInputBlockedAt(float x, float y);

    // True when the overlay is on screen (regardless of hitbox).
    bool isInputBlocked();

    // True while the overlay consumes keys (a text field is active or a popup
    // is open). Wayland withholds key events on this, hitbox or not.
    bool isOverlayConsumingKeys();

    // Diagnostics: the configured gate plus the live per-backend state.
    bool inputBlockingEnabled();
    bool inputGrabActive();        // X11: a server-side pointer or keyboard grab is up
    bool inputKeyboardWithheld();  // Wayland keys withheld from the game
    bool inputPointerOverOverlay();
} // namespace VKIntox
