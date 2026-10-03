#include "mouse_input_x11.hh"

#include "x11_display.hh"

#include <X11/Xlib.h>

namespace VKIntox
{
    static float scrollAccumulator = 0.0f;

    static void onRawButton(uint32_t button, bool pressed)
    {
        if (!pressed)
            return;
        if (button == 4)
            scrollAccumulator += 1.0f;   // wheel up
        else if (button == 5)
            scrollAccumulator -= 1.0f;   // wheel down
    }

    static void ensureRegistered()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        setX11ButtonCallback(onRawButton);
    }

    MouseState getMouseStateX11()
    {
        MouseState state;
        Display* display = x11Display();
        if (!display)
            return state;

        ensureRegistered();
        pumpX11Input();

        const Window root = DefaultRootWindow(display);
        Window rootRet = 0, childRet = 0;
        int rootX = 0, rootY = 0, winX = 0, winY = 0;
        unsigned int mask = 0;
        if (!XQueryPointer(display, root, &rootRet, &childRet, &rootX, &rootY, &winX, &winY, &mask))
            return state;

        // the overlay draws in framebuffer space, so report window-relative
        // coordinates rather than root ones.
        int x = rootX;
        int y = rootY;
        if (const unsigned long gameWindow = x11GameWindow())
        {
            Window child = 0;
            int wx = 0, wy = 0;
            if (XTranslateCoordinates(display, root, (Window)gameWindow, rootX, rootY, &wx, &wy, &child))
            {
                x = wx;
                y = wy;
            }
        }

        state.x = x;
        state.y = y;
        state.leftButton = (mask & Button1Mask) != 0;
        state.middleButton = (mask & Button2Mask) != 0;
        state.rightButton = (mask & Button3Mask) != 0;
        state.scrollDelta = scrollAccumulator;
        scrollAccumulator = 0.0f;
        return state;
    }
} // namespace VKIntox
