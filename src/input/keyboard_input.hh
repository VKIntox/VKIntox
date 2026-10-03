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

    uint32_t convertToKeySym(std::string key);
    void beginKeyboardInputFrame();
    bool     isKeyPressed(uint32_t ks);
    // Non-consuming held-state query. Unlike isKeyPressed it never eats the
    // one-shot press event, and letters match either case.
    bool     isKeyDown(uint32_t ks);
    KeyboardState getKeyboardState();
} // namespace VKIntox
