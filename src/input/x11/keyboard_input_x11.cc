#include "input/x11/keyboard_input_x11.hh"

#include "util/logger.hh"
#include "input/x11/x11_display.hh"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>

#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace VKIntox
{
    static std::string typedCharsAccumulator;
    static std::string lastKeyNameAccumulator;
    static bool backspacePressed = false;
    static bool deletePressed = false;
    static bool enterPressed = false;
    static bool leftPressed = false;
    static bool rightPressed = false;
    static bool homePressed = false;
    static bool endPressed = false;
    static uint64_t keymapFrameId = 0;
    static uint64_t lastQueriedKeymapFrame = std::numeric_limits<uint64_t>::max();
    static char cachedKeymap[32] = {};
    static std::unordered_map<uint32_t, KeyCode> keysymToKeycodeCache;
    static std::unordered_set<KeyCode> heldKeycodes;

    static void processKeycode(KeyCode keycode, unsigned int state, bool pressed)
    {
        Display* display = x11Display();
        if (!display)
            return;

        const KeySym keysym = XkbKeycodeToKeysym(display, keycode, 0, 0);
        const bool repeat = pressed && !heldKeycodes.insert(keycode).second;
        if (!pressed)
            heldKeycodes.erase(keycode);
        queueKeyboardEvent((uint32_t)keysym, pressed, repeat);
        if (!pressed)
            return;

        // capture the key name for the keybind editor, skipping modifiers.
        if (keysym != XK_Shift_L && keysym != XK_Shift_R &&
            keysym != XK_Control_L && keysym != XK_Control_R &&
            keysym != XK_Alt_L && keysym != XK_Alt_R &&
            keysym != XK_Super_L && keysym != XK_Super_R)
        {
            const char* keyName = XKeysymToString(keysym);
            if (keyName)
                lastKeyNameAccumulator = keyName;
        }

        if (keysym == XK_BackSpace) backspacePressed = true;
        else if (keysym == XK_Delete) deletePressed = true;
        else if (keysym == XK_Return || keysym == XK_KP_Enter) enterPressed = true;
        else if (keysym == XK_Left) leftPressed = true;
        else if (keysym == XK_Right) rightPressed = true;
        else if (keysym == XK_Home) homePressed = true;
        else if (keysym == XK_End) endPressed = true;
        else
        {
            bool shifted = (state & ShiftMask) != 0;
            if (!shifted)
            {
                const KeyCode shiftL = XKeysymToKeycode(display, XK_Shift_L);
                const KeyCode shiftR = XKeysymToKeycode(display, XK_Shift_R);
                shifted = (cachedKeymap[shiftL >> 3] & (1 << (shiftL & 7))) ||
                          (cachedKeymap[shiftR >> 3] & (1 << (shiftR & 7)));
            }

            const KeySym actualSym = XkbKeycodeToKeysym(display, keycode, 0, shifted ? 1 : 0);
            if (actualSym >= 0x20 && actualSym <= 0x7E)
                typedCharsAccumulator += (char)actualSym;
        }
    }

    static void onRawKey(uint32_t keycode, bool pressed)
    {
        processKeycode((KeyCode)keycode, 0, pressed);
    }

    static void ensureRegistered()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        setX11KeyCallback(onRawKey);
        // cache the keymap once so the first shifted character is correct.
        if (Display* display = x11Display())
            XQueryKeymap(display, cachedKeymap);
    }

    void beginKeyboardInputFrameX11()
    {
        keymapFrameId++;
    }

    uint32_t convertToKeySymX11(std::string key)
    {
        const uint32_t result = (uint32_t)XStringToKeysym(key.c_str());
        if (!result)
            Logger::err("invalid key");
        return result;
    }

    // isKeyPressedX11 is already a held-state check: the keymap bit plus what
    // the grab handoff cleared server-side. The keymap query is cached per
    // frame, so callers can ask as often as they like.
    bool isKeyPressedX11(uint32_t ks)
    {
        return isKeyHeldX11(ks);
    }

    bool isKeyHeldX11(uint32_t ks)
    {
        Display* display = x11Display();
        if (!display)
            return false;

        ensureRegistered();

        if (lastQueriedKeymapFrame != keymapFrameId)
        {
            XQueryKeymap(display, cachedKeymap);
            lastQueriedKeymapFrame = keymapFrameId;
        }

        KeyCode keycode = 0;
        auto cached = keysymToKeycodeCache.find(ks);
        if (cached != keysymToKeycodeCache.end())
            keycode = cached->second;
        else
        {
            keycode = XKeysymToKeycode(display, (KeySym)ks);
            keysymToKeycodeCache.emplace(ks, keycode);
        }

        if (keycode == 0)
            return false;

        // the grab handoff clears held keys server-side; report the hold so the
        // toggle-key edge tracker never sees a synthetic up/down pair
        return !!(cachedKeymap[keycode >> 3] & (1 << (keycode & 7))) || x11KeySynthHeld(keycode);
    }

    KeyboardState getKeyboardStateX11()
    {
        KeyboardState state;
        if (!x11Available())
            return state;

        ensureRegistered();
        pumpX11Input();

        state.typedChars = std::move(typedCharsAccumulator);
        state.lastKeyName = std::move(lastKeyNameAccumulator);
        state.backspace = backspacePressed;
        state.del = deletePressed;
        state.enter = enterPressed;
        state.left = leftPressed;
        state.right = rightPressed;
        state.home = homePressed;
        state.end = endPressed;

        typedCharsAccumulator.clear();
        lastKeyNameAccumulator.clear();
        backspacePressed = false;
        deletePressed = false;
        enterPressed = false;
        leftPressed = false;
        rightPressed = false;
        homePressed = false;
        endPressed = false;

        return state;
    }
} // namespace VKIntox
