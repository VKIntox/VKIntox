#include "input/x11/x11_display.hh"

#include "util/logger.hh"
#include "input/wayland/wayland_display.hh"

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace VKIntox
{
    namespace
    {
        Display* g_display = nullptr;
        bool g_tried = false;
        int g_xiOpcode = 0;
        bool g_xiReady = false;

        X11KeyCallback g_keyCallback = nullptr;
        X11ButtonCallback g_buttonCallback = nullptr;
        bool g_pointerGrabActive = false;
        bool g_keyboardGrabActive = false;
        Window g_grabWindow = 0;
        bool g_xtestChecked = false;
        bool g_xtestReady = false;

        // Keys and buttons this layer released into the game when it grabbed.
        // The synthetic releases clear the server's key/button state, so a
        // re-query at release time cannot tell what is still physically down;
        // these sets are kept current from the events the grab delivers and are
        // what gets handed back. Same idea as the Wayland interpose's held keys.
        std::vector<unsigned int> g_handedKeys;
        std::vector<unsigned int> g_handedButtons;

        // The subset the player still holds: the server state says up but the
        // physical device says down. The layer polls that same state for its
        // own hotkeys and ImGui's mask, so it must OR this in or every grab
        // engage looks like the key was let go and every handback like a fresh
        // press — which re-fired the overlay toggle on close.
        std::vector<unsigned int> g_synthHeldKeys;
        std::vector<unsigned int> g_synthHeldButtons;

        template <typename T>
        static bool setContains(const std::vector<T>& set, T value)
        {
            return std::find(set.begin(), set.end(), value) != set.end();
        }

        template <typename T>
        static void setErase(std::vector<T>& set, T value)
        {
            set.erase(std::remove(set.begin(), set.end(), value), set.end());
        }

        void initX11()
        {
            if (g_tried)
                return;
            g_tried = true;

            const char* disVar = getenv("DISPLAY");
            if (!disVar || !*disVar)
                return;

            g_display = XOpenDisplay(disVar);
            if (!g_display)
            {
                Logger::warn("X11 input: could not open DISPLAY, keyboard/mouse/blocking disabled");
                return;
            }

            int event = 0, error = 0;
            if (!XQueryExtension(g_display, "XInputExtension", &g_xiOpcode, &event, &error))
                return;

            int major = 2, minor = 0;
            if (XIQueryVersion(g_display, &major, &minor) != Success)
                return;

            // Raw events ignore focus, so they carry keys and wheel between
            // grabs. While a grab is active they can stop arriving entirely,
            // so the grab's redirected core events (see x11SetGrabState) are
            // the key source instead.
            unsigned char mask[XIMaskLen(XI_RawButtonRelease)] = {0};
            XISetMask(mask, XI_RawKeyPress);
            XISetMask(mask, XI_RawKeyRelease);
            XISetMask(mask, XI_RawButtonPress);
            XISetMask(mask, XI_RawButtonRelease);
            XIEventMask em = {XIAllMasterDevices, sizeof(mask), mask};
            XISelectEvents(g_display, DefaultRootWindow(g_display), &em, 1);
            XFlush(g_display);
            g_xiReady = true;
        }
    }

    _XDisplay* x11Display()
    {
        initX11();
        return g_display;
    }

    bool x11Available()
    {
        initX11();
        return g_display != nullptr;
    }

    unsigned long x11GameWindow()
    {
        return getX11Window();
    }

    void setX11KeyCallback(X11KeyCallback cb)
    {
        g_keyCallback = cb;
    }

    void setX11ButtonCallback(X11ButtonCallback cb)
    {
        g_buttonCallback = cb;
    }

    // XTest is how held input is handed across a grab transition. Without it the
    // grab still works, the game is just left holding whatever was down.
    static bool xtestReady(_XDisplay* display)
    {
        if (g_xtestChecked)
            return g_xtestReady;
        g_xtestChecked = true;

        Display* d = reinterpret_cast<Display*>(display);
        int event = 0, error = 0, major = 0, minor = 0;
        g_xtestReady = d != nullptr && XTestQueryExtension(d, &event, &error, &major, &minor);
        if (!g_xtestReady)
            Logger::warn("X11 input: XTest extension missing; keys held across a grab stay stuck in the game");
        return g_xtestReady;
    }

    // Synthetic releases (toGame = false) for every key the server reports as
    // physically down, so the game's key state matches the keyboard before the
    // grab takes the events away; synthetic presses (toGame = true) for the keys
    // this layer released and has seen no real release for, so a held movement
    // key keeps working. XTest events travel the normal input path, so the game
    // sees them as its own. Must not run while our own keyboard grab is up: they
    // would be redirected into the layer instead of the game.
    static void handOffHeldKeys(bool toGame)
    {
        if (!g_display)
            return;

        if (toGame)
        {
            if (g_handedKeys.empty())
                return;
            char keys[32] = {0};
            XQueryKeymap(g_display, keys);
            int repressed = 0, dropped = 0;
            for (unsigned int keycode : g_handedKeys)
            {
                // the player let go and the release landed after the ungrab:
                // the game already saw it, so re-pressing would leave a key
                // down that nothing will ever release
                const bool bitDown = keys[keycode >> 3] & (1 << (keycode & 7));
                if (!bitDown && !setContains(g_synthHeldKeys, keycode))
                {
                    dropped++;
                    continue;
                }
                XTestFakeKeyEvent(g_display, keycode, True, 0);
                repressed++;
            }
            XSync(g_display, False);
            Logger::debug("X11 input: re-pressed " + std::to_string(repressed)
                          + " held key(s) after the keyboard grab"
                          + (dropped ? (" (dropped " + std::to_string(dropped) + " already released)") : ""));
            g_handedKeys.clear();
            g_synthHeldKeys.clear();
            return;
        }

        g_handedKeys.clear();
        g_synthHeldKeys.clear();
        if (!xtestReady(g_display))
            return;

        char keys[32] = {0};
        XQueryKeymap(g_display, keys);
        for (int keycode = 8; keycode < 256; ++keycode)
        {
            if (!(keys[keycode / 8] & (1 << (keycode % 8))))
                continue;
            XTestFakeKeyEvent(g_display, (unsigned int)keycode, False, 0);
            g_handedKeys.push_back((unsigned int)keycode);
            g_synthHeldKeys.push_back((unsigned int)keycode);
        }
        XSync(g_display, False);

        if (!g_handedKeys.empty())
            Logger::debug("X11 input: released " + std::to_string(g_handedKeys.size())
                          + " held key(s) before the keyboard grab");
    }

    // Same contract as handOffHeldKeys, for the buttons the pointer grab takes
    // away. Core button state only covers 1-5; anything else is not holdable.
    static void handOffHeldButtons(bool toGame)
    {
        if (g_display == nullptr)
            return;

        if (toGame)
        {
            if (g_handedButtons.empty())
                return;
            Window rootRet = 0, childRet = 0;
            int rootX = 0, rootY = 0, winX = 0, winY = 0;
            unsigned int mask = 0;
            int repressed = 0, dropped = 0;
            if (XQueryPointer(g_display, DefaultRootWindow(g_display), &rootRet, &childRet,
                              &rootX, &rootY, &winX, &winY, &mask))
            {
                for (unsigned int button : g_handedButtons)
                {
                    // released after the ungrab: the game already saw it
                    const bool bitDown = mask & (Button1Mask << (button - 1));
                    if (!bitDown && !setContains(g_synthHeldButtons, button))
                    {
                        dropped++;
                        continue;
                    }
                    XTestFakeButtonEvent(g_display, button, True, 0);
                    repressed++;
                }
            }
            XSync(g_display, False);
            Logger::debug("X11 input: re-pressed " + std::to_string(repressed)
                          + " held button(s) after the pointer grab"
                          + (dropped ? (" (dropped " + std::to_string(dropped) + " already released)") : ""));
            g_handedButtons.clear();
            g_synthHeldButtons.clear();
            return;
        }

        g_handedButtons.clear();
        g_synthHeldButtons.clear();
        if (!xtestReady(g_display))
            return;

        Window rootRet = 0, childRet = 0;
        int rootX = 0, rootY = 0, winX = 0, winY = 0;
        unsigned int mask = 0;
        if (!XQueryPointer(g_display, DefaultRootWindow(g_display), &rootRet, &childRet,
                           &rootX, &rootY, &winX, &winY, &mask))
            return;

        for (unsigned int button = 1; button <= 5; ++button)
        {
            if (!(mask & (Button1Mask << (button - 1))))
                continue;
            XTestFakeButtonEvent(g_display, button, False, 0);
            g_handedButtons.push_back(button);
            g_synthHeldButtons.push_back(button);
        }
        XSync(g_display, False);

        if (!g_handedButtons.empty())
            Logger::debug("X11 input: released " + std::to_string(g_handedButtons.size())
                          + " held button(s) before the pointer grab");
    }

    // Keeps the handed-over sets honest while a grab is up: the grab is where
    // press and release events for those keys and buttons arrive, so this is the
    // only place that can tell whether the player let go in the meantime.
    static void trackGrabbedInput(uint32_t keycode, uint32_t button, bool key, bool pressed)
    {
        std::vector<unsigned int>& set = key ? g_handedKeys : g_handedButtons;
        std::vector<unsigned int>& synth = key ? g_synthHeldKeys : g_synthHeldButtons;
        const unsigned int code = key ? keycode : button;
        auto it = std::find(set.begin(), set.end(), code);

        if (pressed)
        {
            if (it == set.end())
                set.push_back(code);
            // re-pressed by the player: the server bit is down again
            setErase(synth, code);
        }
        else if (it != set.end())
        {
            set.erase(it);
        }
        setErase(synth, code);
    }

    // Only select for what the active grabs actually redirect. Dropping back to
    // no mask on release matters: a left-over selection keeps feeding this
    // connection copies of the game's key and button events forever.
    static void applyGrabEventMask()
    {
        if (!g_display || g_grabWindow == 0)
            return;

        long mask = 0;
        if (g_pointerGrabActive)
            mask |= ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
        if (g_keyboardGrabActive)
            mask |= KeyPressMask | KeyReleaseMask;
        XSelectInput(g_display, g_grabWindow, mask);
    }

    // The one place grabs change. Both grabs are server-side and on this
    // connection, so the game stops seeing the events entirely; the held-input
    // handoff around the first engage and the last release is what keeps the
    // game's state honest.
    void applyGrabs(bool wantPointer, bool wantKeyboard)
    {
        // Open the connection here rather than requiring a poll first: a grab
        // request must not silently do nothing just because no frame has run.
        if (x11Display() == nullptr)
            return;
        if (wantPointer == g_pointerGrabActive && wantKeyboard == g_keyboardGrabActive)
            return;

        const bool wantAny = wantPointer || wantKeyboard;

        const unsigned long gameWindow = x11GameWindow();
        const Window grabWindow = gameWindow ? (Window)gameWindow : DefaultRootWindow(g_display);

        if (wantPointer != g_pointerGrabActive)
        {
            if (wantPointer)
            {
                // releases first, while the game can still see its own events
                handOffHeldButtons(false);

                const int status = XGrabPointer(g_display, grabWindow, False,
                                                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                                GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
                if (status != GrabSuccess)
                    Logger::warn("X11 input: pointer grab failed (" + std::to_string(status) + ")");
            }
            else
            {
                XUngrabPointer(g_display, CurrentTime);
                // now that button events reach the game again, hand back what we
                // took: the releases cleared the server's button state, so only
                // what we tracked can be re-pressed
                handOffHeldButtons(true);
            }
            g_pointerGrabActive = wantPointer;
        }

        if (wantKeyboard != g_keyboardGrabActive)
        {
            if (wantKeyboard)
            {
                // releases first: redirecting them would leave the game holding
                // every key that happened to be down when the overlay was used
                handOffHeldKeys(false);

                const int status = XGrabKeyboard(g_display, grabWindow, False,
                                                 GrabModeAsync, GrabModeAsync, CurrentTime);
                if (status != GrabSuccess)
                    Logger::warn("X11 input: keyboard grab failed (" + std::to_string(status) + ")");
            }
            else
            {
                XUngrabKeyboard(g_display, CurrentTime);
                handOffHeldKeys(true);
            }
            g_keyboardGrabActive = wantKeyboard;
        }

        g_grabWindow = wantAny ? grabWindow : 0;
        applyGrabEventMask();
        XFlush(g_display);

        Logger::debug(std::string("X11 input: grabs pointer=") + (wantPointer ? "on" : "off")
                      + " keyboard=" + (wantKeyboard ? "on" : "off"));
    }

    // The layer opens its own Display connection, so it is a distinct X client
    // from the game. A server-side grab on that connection takes pointer events,
    // or key events, away from the game's connection entirely.
    void x11SetGrabState(bool grab)
    {
        applyGrabs(grab, g_keyboardGrabActive);
    }

    // the handoff cleared these server-side while the player still holds them;
    // keymap and button-mask polls must OR them in to see one unbroken hold
    bool x11KeySynthHeld(unsigned int keycode)
    {
        return setContains(g_synthHeldKeys, keycode);
    }

    bool x11ButtonSynthHeld(unsigned int button)
    {
        return setContains(g_synthHeldButtons, button);
    }

    void x11SetKeyboardGrabState(bool grab)
    {
        applyGrabs(g_pointerGrabActive, grab);
    }

    bool x11GrabActive()
    {
        return g_pointerGrabActive || g_keyboardGrabActive;
    }

    void pumpX11Input()
    {
        if (!x11Available())
            return;

        while (XPending(g_display) > 0)
        {
            XEvent ev;
            XNextEvent(g_display, &ev);

            if (ev.type == GenericEvent && ev.xcookie.extension == g_xiOpcode
                && XGetEventData(g_display, &ev.xcookie))
            {
                switch (ev.xcookie.evtype)
                {
                case XI_RawKeyPress:
                case XI_RawKeyRelease:
                    // the keyboard grab makes core events authoritative; raw
                    // here would double every press on servers that still
                    // deliver it
                    if (!g_keyboardGrabActive && g_keyCallback)
                    {
                        const bool pressed = ev.xcookie.evtype == XI_RawKeyPress;
                        g_keyCallback(((XIRawEvent*)ev.xcookie.data)->detail, pressed);
                    }
                    break;
                case XI_RawButtonPress:
                case XI_RawButtonRelease:
                    if (!g_pointerGrabActive && g_buttonCallback)
                    {
                        const bool pressed = ev.xcookie.evtype == XI_RawButtonPress;
                        g_buttonCallback(((XIRawEvent*)ev.xcookie.data)->detail, pressed);
                    }
                    break;
                default:
                    break;
                }
                XFreeEventData(g_display, &ev.xcookie);
            }
            else if (ev.type == KeyPress || ev.type == KeyRelease)
            {
                // consumed while grabbed (raw events can stop during a core
                // grab) or as the no-XInput2 fallback; outside both, raw
                // already delivered this key and core would double it.
                if ((g_keyboardGrabActive || !g_xiReady) && g_keyCallback)
                    g_keyCallback(ev.xkey.keycode, ev.type == KeyPress);
                if (g_keyboardGrabActive)
                    trackGrabbedInput(ev.xkey.keycode, 0, true, ev.type == KeyPress);
            }
            else if (ev.type == ButtonPress || ev.type == ButtonRelease)
            {
                if ((g_pointerGrabActive || !g_xiReady) && g_buttonCallback)
                    g_buttonCallback(ev.xbutton.button, ev.type == ButtonPress);
                if (g_pointerGrabActive)
                    trackGrabbedInput(0, ev.xbutton.button, false, ev.type == ButtonPress);
            }
        }
    }
} // namespace VKIntox
