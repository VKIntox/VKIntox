#pragma once

#include <cstdint>
#include <string>
#include "keyboard_input.hh"

namespace VKIntox
{
    uint32_t convertToKeySymX11(std::string key);
    void     beginKeyboardInputFrameX11();
    bool     isKeyPressedX11(uint32_t ks);
    bool     isKeyDownX11(uint32_t ks);
    KeyboardState getKeyboardStateX11();
} // namespace VKIntox
