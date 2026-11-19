#include "input/x11/x11_display.hh"

#include "util/logger.hh"
#include "input/wayland/wayland_display.hh"

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace VKIntox
{
    namespace
    {
        Display* g_display = nullptr;
        bool g_tried = false;
        int g_xiOpcode = 0;

        X11KeyCallback g_keyCallback = nullptr;
        X11ButtonCallback g_buttonCallback = nullptr;
        std::atomic<bool> g_keyboardGrabActive{false};
        Window g_grabWindow = 0;
        bool g_xtestChecked = false;
        bool g_xtestReady = false;

        std::mutex g_grabMutex;
        std::atomic<long long> g_lastPumpMs{0};
        std::atomic<bool> g_watchdogStarted{false};

        // Keys this layer released into the game when the keyboard grab
        // engaged. The synthetic releases clear the server's key state, so a
        // re-query at release time cannot tell what is still physically down;
        // these sets are kept current from the events the grab delivers and
        // are what gets handed back. Same idea as the Wayland interpose's
        // owned keys.
        std::vector<unsigned int> g_handedKeys;

        // The subset the player still holds: the server state says up but the
        // physical device says down. The layer polls that same state for its
        // own hotkeys and ImGui's mask, so it must OR this in or every grab
        // engage looks like the key was let go and every handback like a fresh
        // press — which re-fired the overlay toggle on close.
        std::vector<unsigned int> g_synthHeldKeys;

        // Input-only shield windows over the overlay hitboxes. Created as
        // children of the game window, so they swallow clicks and motion the
        // way the Wayland capture surface does, without any grab.
        struct InputShield
        {
            Window window = 0;
            Window parent = 0;
        };
        std::vector<InputShield> g_shields;

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

        // Errors on the game's connection keep the default fatal behavior;
        // this private connection only gets logged, so a failed grab or a
        // shield over a dead parent window cannot abort the game.
        static int x11ErrorHandler(Display* d, XErrorEvent* e)
        {
            if (d != g_display)
                exit(EXIT_FAILURE);
            char text[64] = {};
            XGetErrorText(d, e->error_code, text, sizeof(text));
            Logger::warn(std::string("X11 input: ignored X error (") + text
                         + ", request " + std::to_string(static_cast<int>(e->request_code)) + ")");
            return 0;
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
            XSetErrorHandler(x11ErrorHandler);

            int event = 0, error = 0;
            if (!XQueryExtension(g_display, "XInputExtension", &g_xiOpcode, &event, &error))
                return;

            int major = 2, minor = 0;
            if (XIQueryVersion(g_display, &major, &minor) != Success)
                return;

            // Raw events ignore focus, so they carry keys and wheel between
            // grabs. While a grab is active they can stop arriving entirely,
            // so the grab's redirected core events (see applyKeyboardGrab)
            // are the key source instead.
            unsigned char mask[XIMaskLen(XI_RawButtonRelease)] = {0};
            XISetMask(mask, XI_RawKeyPress);
            XISetMask(mask, XI_RawKeyRelease);
            XISetMask(mask, XI_RawButtonPress);
            XISetMask(mask, XI_RawButtonRelease);
            XIEventMask em = {XIAllMasterDevices, sizeof(mask), mask};
            XISelectEvents(g_display, DefaultRootWindow(g_display), &em, 1);
            XFlush(g_display);
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

    // Same tracking for keys the keyboard grab redirects: the grab is where
    // press and release events for those keys arrive, so this is the only
    // place that can tell whether the player let go in the meantime.
    static void trackGrabbedInput(uint32_t keycode, bool pressed)
    {
        auto it = std::find(g_handedKeys.begin(), g_handedKeys.end(), keycode);

        if (pressed)
        {
            if (it == g_handedKeys.end())
                g_handedKeys.push_back(keycode);
            // re-pressed by the player: the server bit is down again
            setErase(g_synthHeldKeys, keycode);
        }
        else if (it != g_handedKeys.end())
        {
            g_handedKeys.erase(it);
        }
        setErase(g_synthHeldKeys, keycode);
    }

    // Only select for what the keyboard grab actually redirects. Dropping back
    // to no mask on release matters: a left-over selection keeps feeding this
    // connection copies of the game's key events forever.
    static void applyGrabEventMask()
    {
        if (!g_display || g_grabWindow == 0)
            return;

        XSelectInput(g_display, g_grabWindow,
                     g_keyboardGrabActive ? (KeyPressMask | KeyReleaseMask) : 0);
    }

    // The one place the grab changes. The grab is server-side and on this
    // connection, so the game stops seeing keys entirely; the held-input
    // handoff around the first engage and the last release is what keeps the
    // game's state honest.
    static void startGrabWatchdog();

    void applyKeyboardGrab(bool wantKeyboard)
    {
        if (x11Display() == nullptr)
            return;
        std::lock_guard<std::mutex> lock(g_grabMutex);
        if (wantKeyboard == g_keyboardGrabActive)
            return;

        const unsigned long gameWindow = x11GameWindow();
        const Window grabWindow = gameWindow ? (Window)gameWindow : DefaultRootWindow(g_display);

        if (wantKeyboard)
        {
            // releases first: redirecting them would leave the game holding
            // every key that happened to be down when the overlay was used
            handOffHeldKeys(false);

            const int status = XGrabKeyboard(g_display, grabWindow, False,
                                             GrabModeAsync, GrabModeAsync, CurrentTime);
            if (status != GrabSuccess)
                Logger::warn("X11 input: keyboard grab failed (" + std::to_string(status) + ")");
            startGrabWatchdog();
        }
        else
        {
            XUngrabKeyboard(g_display, CurrentTime);
            // now that key events reach the game again, hand back what we
            // took: the releases cleared the server's key state, so only what
            // we tracked can be re-pressed
            handOffHeldKeys(true);
        }

        g_keyboardGrabActive = wantKeyboard;
        g_grabWindow = wantKeyboard ? grabWindow : 0;
        applyGrabEventMask();
        XFlush(g_display);

        Logger::debug(std::string("X11 input: keyboard grab ") + (wantKeyboard ? "on" : "off"));
    }

    // A stalled render loop with the grab up would lock the keyboard out with
    // nobody left to release it, so the watchdog lets go once the per-frame
    // pump stops.
    static void startGrabWatchdog()
    {
        bool expected = false;
        if (!g_watchdogStarted.compare_exchange_strong(expected, true))
            return;

        std::thread([] {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (!g_keyboardGrabActive.load(std::memory_order_acquire))
                    continue;
                const long long last = g_lastPumpMs.load(std::memory_order_acquire);
                const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (last == 0 || now - last < 2000)
                    continue;

                std::lock_guard<std::mutex> lock(g_grabMutex);
                if (!g_keyboardGrabActive.load(std::memory_order_relaxed))
                    continue;
                if (g_grabWindow)
                {
                    XSelectInput(g_display, g_grabWindow, 0);
                    XUngrabKeyboard(g_display, CurrentTime);
                    handOffHeldKeys(true);
                }
                g_keyboardGrabActive = false;
                g_grabWindow = 0;
                XFlush(g_display);
                Logger::warn("X11 input: render loop stalled with the keyboard grab up; released it");
            }
        }).detach();
    }

    // The layer opens its own Display connection, so it is a distinct X client
    // from the game. A server-side keyboard grab on that connection takes key
    // events away from the game's connection entirely.
    void x11SetKeyboardGrabState(bool grab)
    {
        applyKeyboardGrab(grab);
    }

    // the handoff cleared these server-side while the player still holds them;
    // keymap and button-mask polls must OR them in to see one unbroken hold
    bool x11KeySynthHeld(unsigned int keycode)
    {
        return setContains(g_synthHeldKeys, keycode);
    }

    // Map input-only shield windows over the hitboxes. They are children of
    // the game window, so rect coordinates are window-relative, and they get
    // clicks and motion instead of the game. Nothing is grabbed: Wine never
    // sees grab focus events from a pointer crossing.
    void x11UpdateInputShields(const InputRect* rects, int count, bool active)
    {
        if (x11Display() == nullptr)
            return;

        const Window parent = (Window)x11GameWindow();
        if (!active || parent == 0)
        {
            for (InputShield& shield : g_shields)
            {
                if (shield.window)
                    XUnmapWindow(g_display, shield.window);
            }
            XFlush(g_display);
            return;
        }

        std::vector<const InputRect*> valid;
        valid.reserve(static_cast<size_t>(count > 0 ? count : 0));
        for (int i = 0; i < count; ++i)
        {
            if (rects[i].width > 0.0f && rects[i].height > 0.0f)
                valid.push_back(&rects[i]);
        }

        if (g_shields.size() > valid.size())
        {
            // unmapped before the record is dropped, or the window would stay
            // up swallowing clicks with nothing left to track it
            for (size_t i = valid.size(); i < g_shields.size(); ++i)
            {
                if (g_shields[i].window)
                    XUnmapWindow(g_display, g_shields[i].window);
            }
        }
        g_shields.resize(valid.size());

        for (size_t i = 0; i < valid.size(); ++i)
        {
            InputShield& shield = g_shields[i];
            const InputRect& rect = *valid[i];
            if (!shield.window || shield.parent != parent)
            {
                // a dead parent took its shields with it; stale ids are never
                // touched, just abandoned
                XSetWindowAttributes attr{};
                attr.override_redirect = True;
                shield.window = XCreateWindow(g_display, parent, 0, 0, 1, 1, 0,
                                              CopyFromParent, InputOnly, CopyFromParent,
                                              CWOverrideRedirect, &attr);
                XSelectInput(g_display, shield.window,
                             ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
                shield.parent = parent;
            }
            XMoveResizeWindow(g_display, shield.window,
                              static_cast<int>(rect.x), static_cast<int>(rect.y),
                              static_cast<int>(rect.width), static_cast<int>(rect.height));
            XRaiseWindow(g_display, shield.window);
            XMapWindow(g_display, shield.window);
        }
        XFlush(g_display);
    }

    bool x11GrabActive()
    {
        return g_keyboardGrabActive.load(std::memory_order_acquire);
    }

    void pumpX11Input()
    {
        if (!x11Available())
            return;

        g_lastPumpMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_release);

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
                    if (g_buttonCallback)
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
                if (g_keyboardGrabActive && g_keyCallback)
                    g_keyCallback(ev.xkey.keycode, ev.type == KeyPress);
                if (g_keyboardGrabActive)
                    trackGrabbedInput(ev.xkey.keycode, ev.type == KeyPress);
            }
            // core button events on this connection are only the shields'
            // swallowed clicks; the wheel comes from the raw feed above
        }
    }
} // namespace VKIntox
