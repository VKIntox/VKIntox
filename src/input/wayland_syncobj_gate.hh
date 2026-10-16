#pragma once

#include <cstdint>

namespace VKIntox
{
    // Holds foreign window commits out of an open linux-drm-syncobj point
    // sequence: a commit landing between set_acquire_point and the presenter's
    // own commit is a protocol error, and hyprland kills the connection for it.
    // Only surfaces that carry a syncobj ever arm; everything else is a no-op.
    namespace WaylandSyncobjGate
    {
        // wp_linux_drm_syncobj_manager_v1.get_surface: remember the pairing.
        void noteSyncobjSurface(uint32_t syncobjId, uint32_t surfaceId);

        // destroy: the pairing and any open sequence on it are gone.
        void noteSyncobjDestroyed(uint32_t syncobjId);

        // set_acquire_point / set_release_point: opens the sequence for the
        // mapped surface, owned by the calling thread.
        void notePointSetForSyncobj(uint32_t syncobjId);

        // wl_surface.commit. Blocks while another thread's sequence is open.
        // endCommit must follow the actual marshal so a released waiter cannot
        // overtake the presenter's commit on the wire.
        void beginCommit(uint32_t surfaceId);
        void endCommit(uint32_t surfaceId);

        // Diagnostics: commits that were actually made to wait.
        uint64_t heldCount();

        // Test hooks.
        void setWaitBudgetForTests(uint64_t microseconds);
        void resetForTests();
    }
} // namespace VKIntox
