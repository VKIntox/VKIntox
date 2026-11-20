#pragma once

#include <cstdint>
#include <string>

namespace VKIntox
{
    struct KeyboardState
    {
        std::string typedChars;     // Characters typed since last call
        std::string lastKeyName;    // X11 name of last key pressed (for keybind capture)
        bool backspace = false;
        bool del = false;
        bool enter = false;
        bool left = false;
        bool right = false;
        bool home = false;
        bool end = false;
    };

    // exported so the test harness can drive the routing functions directly
    __attribute__((visibility("default"))) uint32_t convertToKeySym(std::string key);
    __attribute__((visibility("default"))) void beginKeyboardInputFrame();
    __attribute__((visibility("default"))) bool isKeyPressed(uint32_t ks);
    __attribute__((visibility("default"))) bool isKeyHeld(uint32_t ks);
    __attribute__((visibility("default"))) KeyboardState getKeyboardState();
} // namespace VKIntox
