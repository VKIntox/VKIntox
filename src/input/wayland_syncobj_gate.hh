#pragma once

#include <cstdint>

namespace VKIntox
{
    // Keeps a foreign window commit out of an in-flight present. Mesa's WSI
    // sets linux-drm-syncobj points inside its present call, and a commit from
    // another thread landing between a point and the attach is a protocol
    // error hyprland answers by killing the connection. Mesa does not route
    // through any marshalling we can see, so the presenter's side is bracketed
    // around the driver present call instead, and window commits are held for
    // its duration.
    namespace WaylandSyncobjGate
    {
        // surface id of the captured wayland window, 0 when not on wayland
        uint32_t waylandSurfaceId();

        // bracket around the driver's present call
        void beginPresent(uint32_t surfaceId);
        void endPresent(uint32_t surfaceId);

        // true while any present bracket is open; lets the marshal wrapper
        // skip all work outside presents
        bool anyPresentOpen();

        // before marshalling wl_surface.commit: waits out another thread's
        // present on this surface, bounded by a budget so a wedged presenter
        // cannot park the ui thread forever
        void beginCommit(uint32_t surfaceId);

        // diagnostics: commits that were actually made to wait
        uint64_t heldCount();

        // test hooks
        void setWaitBudgetForTests(uint64_t microseconds);
        void resetForTests();
    }
} // namespace VKIntox
