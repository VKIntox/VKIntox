#pragma once

#include <cstdint>
#include <string>
#include "input/common/keyboard_input.hh"

namespace VKIntox
{
    uint32_t convertToKeySymWayland(std::string key);
    bool     isKeyPressedWayland(uint32_t ks);
    bool     isKeyHeldWayland(uint32_t ks);
    KeyboardState getKeyboardStateWayland();

    // Initialize Wayland keyboard from a captured wl_display
    bool initWaylandKeyboard();

    // Cleanup
    void cleanupWaylandKeyboard();

    // Feeds from the game's wrapped keyboard (interposer only). ownsFd is
    // false when the descriptor belongs to the forwarded game event and the
    // game's listener closes it.
    void mirrorKeyboardKeymap(uint32_t format, int fd, uint32_t size, bool ownsFd);
    void mirrorKeyboardModifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
    void mirrorKeyboardKey(uint32_t key, uint32_t state);
    void mirrorKeyboardFocusLost();
} // namespace VKIntox
