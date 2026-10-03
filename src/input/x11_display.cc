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

            // Raw events are delivered without focus and are unaffected by a
            // grab, so they feed text input and wheel deltas in both states.
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
                    if (g_keyCallback)
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
            else if (!g_xiReady && (ev.type == KeyPress || ev.type == KeyRelease))
            {
                // fallback only: when XInput2 is present, raw events already
                // delivered this key, and a grabbed core event would double it.
                if (g_keyCallback)
                    g_keyCallback(ev.xkey.keycode, ev.type == KeyPress);
            }
            else if (!g_xiReady && (ev.type == ButtonPress || ev.type == ButtonRelease))
            {
                if (g_buttonCallback)
                    g_buttonCallback(ev.xbutton.button, ev.type == ButtonPress);
            }
        }
    }
} // namespace VKIntox
