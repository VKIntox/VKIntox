#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace VKIntox
{
    struct KeyboardEvent
    {
        uint32_t keysym = 0;
        bool down = false;
        bool repeat = false;
    };

    struct KeyboardState
    {
        std::string typedChars;     // Characters typed since last call
        std::string lastKeyName;    // X11 name of last key pressed (for keybind capture)
        std::vector<KeyboardEvent> events;
        bool backspace = false;
        bool del = false;
        bool enter = false;
        bool left = false;
        bool right = false;
        bool home = false;
        bool end = false;
    };

    void queueKeyboardEvent(uint32_t keysym, bool down, bool repeat);
    std::vector<KeyboardEvent> drainKeyboardEvents();
    // exported so the test harness can drive the routing functions directly
    __attribute__((visibility("default"))) uint32_t convertToKeySym(std::string key);
    __attribute__((visibility("default"))) void beginKeyboardInputFrame();
    __attribute__((visibility("default"))) bool isKeyPressed(uint32_t ks);
    __attribute__((visibility("default"))) bool isKeyHeld(uint32_t ks);
    __attribute__((visibility("default"))) KeyboardState getKeyboardState();
} // namespace VKIntox
