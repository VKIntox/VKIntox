// X11 input blocking, against the layer's real blocker: the pointer grab must
// follow the cursor, the keyboard grab must only engage when the overlay consumes
// keys (a grab transition makes Wine drop the game's cursor capture), and held
// keys and buttons must be handed to the game across each grab, or the game keeps
// holding whatever was down when the overlay was used.
//
// It synthesises real input on the developer's display, so it only runs with
// VKINTOX_X11_INPUT_TEST=1 set; otherwise it reports SKIP (exit 77).
//
// usage: vkintox-x11-input-handoff-tests <path to libvkintox.so>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void (*InitFn)(bool);
typedef void (*SetWindowFn)(unsigned long);
typedef void (*SetRectsFn)(const void*, int);
typedef void (*SetBoolFn)(bool);
typedef void (*SetPosFn)(float, float);

struct InputRect { float x, y, width, height; };

static void pump(Display* d, const char* tag, int* sawPress, int* sawRelease, int* sawGrabFocus)
{
    while (XPending(d) > 0)
    {
        XEvent ev;
        XNextEvent(d, &ev);
        if (ev.type == KeyPress)   { (*sawPress)++;   printf("    [%s] game: KeyPress keycode=%u\n", tag, ev.xkey.keycode); }
        if (ev.type == KeyRelease) { (*sawRelease)++; printf("    [%s] game: KeyRelease keycode=%u\n", tag, ev.xkey.keycode); }
        if ((ev.type == FocusIn || ev.type == FocusOut) && ev.xfocus.mode != 0)
        {
            (*sawGrabFocus)++;
            printf("    [%s] game: %s mode=%d (grab notification Wine reacts to)\n", tag,
                   ev.type == FocusIn ? "FocusIn" : "FocusOut", ev.xfocus.mode);
        }
    }
    fflush(stdout);
}

static const char* grabStatus(Display* probe, Window w, int keyboard)
{
    int status = keyboard
        ? XGrabKeyboard(probe, w, False, GrabModeAsync, GrabModeAsync, CurrentTime)
        : XGrabPointer(probe, w, False, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    if (keyboard) XUngrabKeyboard(probe, CurrentTime);
    else          XUngrabPointer(probe, CurrentTime);
    XSync(probe, False);
    return status == AlreadyGrabbed ? "held by the layer" : (status == GrabSuccess ? "free" : "other");
}

int main(int argc, char** argv)
{
    if (getenv("VKINTOX_X11_INPUT_TEST") == nullptr)
    {
        printf("skipped: set VKINTOX_X11_INPUT_TEST=1 to run (it synthesises real input)\n");
        return 77;
    }
    if (XOpenDisplay(NULL) == nullptr)
    {
        printf("skipped: no X display\n");
        return 77;
    }

    const char* libPath = argc > 1 ? argv[1] : "build/src/libvkintox.so";
    void* lib = dlopen(libPath, RTLD_NOW);
    if (!lib) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    InitFn        initInputBlocker        = (InitFn) dlsym(lib, "_ZN7VKIntox16initInputBlockerEb");
    SetWindowFn   setX11Window            = (SetWindowFn) dlsym(lib, "_ZN7VKIntox12setX11WindowEm");
    SetRectsFn    setInputRects           = (SetRectsFn) dlsym(lib, "_ZN7VKIntox13setInputRectsEPKNS_9InputRectEi");
    SetBoolFn     setInputBlocked         = (SetBoolFn) dlsym(lib, "_ZN7VKIntox15setInputBlockedEb");
    SetBoolFn     setInputInteractive     = (SetBoolFn) dlsym(lib, "_ZN7VKIntox19setInputInteractiveEb");
    SetBoolFn     setOverlayWantsKeyboard = (SetBoolFn) dlsym(lib, "_ZN7VKIntox23setOverlayWantsKeyboardEb");
    SetPosFn      updatePointerPosition   = (SetPosFn) dlsym(lib, "_ZN7VKIntox21updatePointerPositionEff");
    if (!initInputBlocker || !setX11Window || !setInputRects || !setInputBlocked
        || !setInputInteractive || !setOverlayWantsKeyboard || !updatePointerPosition)
    {
        printf("missing symbols: %s\n", dlerror());
        return 1;
    }

    Display* game = XOpenDisplay(NULL);
    Display* probe = XOpenDisplay(NULL);
    if (!game || !probe) { printf("no display\n"); return 1; }

    const Window root = DefaultRootWindow(game);
    const Window win = XCreateSimpleWindow(game, root, 90, 90, 380, 260, 0, 0, 0x101820);
    XSelectInput(game, win, KeyPressMask | KeyReleaseMask | FocusChangeMask | StructureNotifyMask);
    XStoreName(game, win, "vkintox input test");
    XMapWindow(game, win);
    XSync(game, False);
    usleep(300000);
    int p = 0, r = 0, f = 0;
    pump(game, "setup", &p, &r, &f);
    XSetInputFocus(game, win, RevertToParent, CurrentTime);
    XSync(game, False);
    usleep(150000);
    pump(game, "setup", &p, &r, &f);

    const KeyCode w = XKeysymToKeycode(game, XK_w);

    setX11Window((unsigned long)win);
    initInputBlocker(true);
    struct InputRect rect = {0.0f, 0.0f, 380.0f, 260.0f};
    setInputRects(&rect, 1);
    setInputBlocked(true);
    updatePointerPosition(100.0f, 100.0f);   // pointer over the overlay hitbox

    printf("\n1. pointer over the overlay: pointer grab taken, keyboard left alone\n");
    printf("    pointer grab : %s\n", grabStatus(probe, root, 0));
    printf("    keyboard grab: %s\n", grabStatus(probe, root, 1));
    int ok1 = strcmp(grabStatus(probe, root, 0), "held by the layer") == 0
           && strcmp(grabStatus(probe, root, 1), "free") == 0;

    printf("\n2. a key held across the keyboard grab is released into the game\n");
    XTestFakeKeyEvent(probe, w, True, 0);            // player presses w
    XSync(probe, False);
    usleep(150000);
    p = r = f = 0;
    pump(game, "held", &p, &r, &f);
    if (p == 0) { printf("    game never saw the press; aborting\n"); return 1; }

    setInputInteractive(true);
    setOverlayWantsKeyboard(true);                   // overlay takes the keyboard
    usleep(200000);
    p = r = f = 0;
    pump(game, "grab-engaged", &p, &r, &f);
    printf("    keyboard grab: %s\n", grabStatus(probe, root, 1));
    int ok2 = r > 0 && strcmp(grabStatus(probe, root, 1), "held by the layer") == 0;

    printf("\n3. releasing the keyboard hands still-held keys back\n");
    setOverlayWantsKeyboard(false);
    usleep(200000);
    p = r = f = 0;
    pump(game, "grab-released", &p, &r, &f);
    printf("    keyboard grab: %s\n", grabStatus(probe, root, 1));
    int ok3 = p > 0 && strcmp(grabStatus(probe, root, 1), "free") == 0;

    // clean up the simulated physical key
    XTestFakeKeyEvent(probe, w, False, 0);
    XSync(probe, False);
    pump(game, "cleanup", &p, &r, &f);

    printf("\nresult: 1=%s 2=%s 3=%s\n", ok1 ? "PASS" : "FAIL", ok2 ? "PASS" : "FAIL", ok3 ? "PASS" : "FAIL");
    printf("%s\n", (ok1 && ok2 && ok3) ? "ALL PASS" : "FAILURES PRESENT");

    XDestroyWindow(game, win);
    XCloseDisplay(probe);
    XCloseDisplay(game);
    return (ok1 && ok2 && ok3) ? 0 : 1;
}
