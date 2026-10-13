#include "x11_display.hh"

#include "logger.hh"
#include "wayland_display.hh"

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#include <cstdlib>

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
        bool g_grabActive = false;

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

    // The layer opens its own Display connection, so it is a distinct X client
    // from the game. A server-side grab on that connection takes pointer and
    // key events away from the game's connection entirely.
    void x11SetGrabState(bool grab)
    {
        if (!g_display || grab == g_grabActive)
            return;

        const unsigned long gameWindow = x11GameWindow();
        const Window grabWindow = gameWindow ? (Window)gameWindow : DefaultRootWindow(g_display);

        int keyboardStatus = GrabSuccess;
        int pointerStatus = GrabSuccess;
        if (grab)
        {
            // owner_events=False reports the redirected events against the
            // grab window, so this connection must select for them there or
            // the grab starves the overlay of keys and wheel along with the
            // game.
            XSelectInput(g_display, grabWindow,
                         KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask);
            keyboardStatus = XGrabKeyboard(g_display, grabWindow, False, GrabModeAsync, GrabModeAsync, CurrentTime);
            pointerStatus = XGrabPointer(g_display, grabWindow, False,
                                         ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        }
        else
        {
            XUngrabKeyboard(g_display, CurrentTime);
            XUngrabPointer(g_display, CurrentTime);
        }
        XFlush(g_display);
        g_grabActive = grab;

        if (grab && (keyboardStatus != GrabSuccess || pointerStatus != GrabSuccess))
            Logger::warn("X11 input grab incomplete: keyboard=" + std::to_string(keyboardStatus)
                         + " pointer=" + std::to_string(pointerStatus));
    }

    bool x11GrabActive()
    {
        return g_grabActive;
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
                    // the active grab makes core events authoritative; raw here
                    // would double every press on servers that still deliver it
                    if (!g_grabActive && g_keyCallback)
                    {
                        const bool pressed = ev.xcookie.evtype == XI_RawKeyPress;
                        g_keyCallback(((XIRawEvent*)ev.xcookie.data)->detail, pressed);
                    }
                    break;
                case XI_RawButtonPress:
                case XI_RawButtonRelease:
                    if (!g_grabActive && g_buttonCallback)
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
                if ((g_grabActive || !g_xiReady) && g_keyCallback)
                    g_keyCallback(ev.xkey.keycode, ev.type == KeyPress);
            }
            else if (ev.type == ButtonPress || ev.type == ButtonRelease)
            {
                if ((g_grabActive || !g_xiReady) && g_buttonCallback)
                    g_buttonCallback(ev.xbutton.button, ev.type == ButtonPress);
            }
        }
    }
} // namespace VKIntox
