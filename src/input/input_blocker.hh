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

    // Newest pointer position in surface pixels. Drives the x11 grab and the
    // follow-the-pointer keyboard rule.
    void updatePointerPosition(float x, float y);

    // True when (x, y) is inside an overlay hitbox and blocking is on.
    bool isInputBlockedAt(float x, float y);

    // True when the overlay is on screen (regardless of hitbox).
    bool isInputBlocked();
} // namespace VKIntox
