#pragma once

#include "input/common/mouse_input.hh"

namespace VKIntox
{
    MouseState getMouseStateWayland();

    // Initialize Wayland mouse from a captured wl_display
    bool initWaylandMouse();

    // Cleanup
    void cleanupWaylandMouse();

    // Mirror a button event from the game's pointer into overlay state.
    // The game's wl_pointer receives releases via implicit grab that our
    // overlay's private pointer never sees. Called from wayland_interpose.cpp.
    void mirrorButtonState(uint32_t button, bool pressed);

    // Feeds from the game's wrapped pointer (interposer only). Position is
    // already translated into game-framebuffer space.
    void mirrorPointerPosition(double x, double y);
    void mirrorScrollAxis(uint32_t axis, double continuous);
    void mirrorScrollDiscrete(uint32_t axis, int32_t discrete);
    void mirrorScrollValue120(uint32_t axis, int32_t value120);
    void resetScrollFrame();
} // namespace VKIntox
