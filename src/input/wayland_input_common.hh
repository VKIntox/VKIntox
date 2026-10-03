#pragma once

#include <wayland-client.h>

namespace VKIntox
{
    // Shared Wayland input state — single wl_seat and event queue used by both
    // keyboard and mouse input backends. Avoids binding two separate seats and
    // creating redundant event queues for the same display.
    //
    // The seat listener is managed here to avoid the wl_seat single-listener
    // constraint. Keyboard and mouse modules register their device listeners
    // via callbacks set before initialization.

    // Callbacks for device binding — set by keyboard/mouse modules before init
    using KeyboardBindCallback = void (*)(wl_seat* seat);
    using PointerBindCallback = void (*)(wl_seat* seat);

    void setKeyboardBindCallback(KeyboardBindCallback cb);
    void setPointerBindCallback(PointerBindCallback cb);

    // Initialize shared Wayland input resources (idempotent).
    // Returns true if the shared seat is ready.
    bool initWaylandInputCommon();

    // Clean up shared resources. Called from cleanupWaylandKeyboard/Mouse.
    void cleanupWaylandInputCommon();

    // Access the shared event queue (for dispatching events)
    wl_event_queue* getWaylandInputQueue();

    // Access the shared seat (for pointer constraints, etc.)
    wl_seat* getWaylandSeat();

    // Input-capture surface: a transparent wl_subsurface of the game's surface
    // whose input region is the overlay hitbox. The compositor routes pointer
    // events over the overlay to it, so the game's own surface never sees them
    // over the overlay — no symbol interposition required.
    wl_surface* getWaylandInputSurface();
    float getWaylandInputSurfaceX();
    float getWaylandInputSurfaceY();

    // Position/size the capture surface in the parent's surface coordinates.
    // Pass width or height <= 0 to disable it (empty input region), letting the
    // pointer pass through to the game.
    void setWaylandInputSurfaceRect(float x, float y, float width, float height);

    // Call once at the start of each frame to allow a fresh dispatch.
    // Without this, dispatchWaylandInputEvents() deduplicates within a frame.
    void beginWaylandInputFrame();

    // Read and dispatch pending Wayland events (non-blocking).
    // Actively reads from the socket to ensure events like button release
    // are not stuck in the kernel buffer. Deduplicated per frame — only the
    // first call after beginWaylandInputFrame() does real work.
    void dispatchWaylandInputEvents();

} // namespace VKIntox
