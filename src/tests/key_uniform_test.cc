// KeyUniform plumbing, against the layer's real keyboard state: the held-key
// query must follow keys injected through XTest, without consuming the
// hotkey poll's press events.
//
// It synthesises real input on the developer's display, so it only runs with
// VKINTOX_KEY_TEST=1 set; otherwise it reports SKIP (exit 77).
//
// usage: vkintox-key-uniform-tests <path to libvkintox.so>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <unistd.h>

typedef void (*InitFn)(bool);
typedef void (*SetWindowFn)(unsigned long);
typedef void (*BeginFrameFn)();
typedef int (*IsKeyFn)(uint32_t);
// std::string passes as an invisible pointer under the itanium ABI, so a
// const-reference signature here matches the by-value one in the layer
typedef uint32_t (*ConvertKeySymFn)(const std::string&);

int main(int argc, char** argv)
{
    if (getenv("VKINTOX_KEY_TEST") == nullptr)
    {
        printf("skipped: set VKINTOX_KEY_TEST=1 to run (it synthesises real input)\n");
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

    // mangled names: VKIntox::isKeyHeld(uint32_t), VKIntox::isKeyPressed(uint32_t),
    // VKIntox::convertToKeySym(std::string), VKIntox::initInputBlocker(bool)
    InitFn        initInputBlocker = (InitFn) dlsym(lib, "_ZN7VKIntox16initInputBlockerEb");
    SetWindowFn   setX11Window     = (SetWindowFn) dlsym(lib, "_ZN7VKIntox12setX11WindowEm");
    BeginFrameFn  beginFrame       = (BeginFrameFn) dlsym(lib, "_ZN7VKIntox23beginKeyboardInputFrameEv");
    IsKeyFn       isKeyHeld        = (IsKeyFn) dlsym(lib, "_ZN7VKIntox9isKeyHeldEj");
    IsKeyFn       isKeyPressed     = (IsKeyFn) dlsym(lib, "_ZN7VKIntox12isKeyPressedEj");
    ConvertKeySymFn convertToKeySym = (ConvertKeySymFn) dlsym(lib, "_ZN7VKIntox15convertToKeySymENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
    if (!initInputBlocker || !setX11Window || !beginFrame || !isKeyHeld || !isKeyPressed || !convertToKeySym)
    {
        printf("missing symbols: %s\n", dlerror());
        return 1;
    }

    Display* d = XOpenDisplay(NULL);
    if (!d) { printf("no display\n"); return 1; }

    // claim the X11 surface kind, like a captured Vulkan surface would
    const Window win = XCreateSimpleWindow(d, DefaultRootWindow(d), 10, 10, 100, 100, 0, 0, 0);
    XMapWindow(d, win);
    XSync(d, False);
    setX11Window((unsigned long)win);
    initInputBlocker(false);

    // hold semantics for a key that exists in the keymap: VK_F8 (0x77)
    const unsigned int vkF8 = 0x77;
    const uint32_t f8 = convertToKeySym(std::string("F8"));
    if (f8 == 0) { printf("convertToKeySym(F8) failed\n"); return 1; }
    const unsigned int f8code = XKeysymToKeycode(d, f8);
    if (f8code == 0) { printf("no keycode for F8 on this keymap\n"); return 77; }
    printf("keysym 0x%X, keycode %u\n", f8, f8code);

    // drain initial state
    XTestFakeKeyEvent(d, f8code, False, 0);
    XSync(d, False);
    usleep(150000);
    beginFrame(); isKeyHeld(f8);

    printf("\n1. idle: F8 reports up\n");
    beginFrame();
    int ok1 = isKeyHeld(f8) == 0;
    printf("    held=%d\n", ok1 ? 0 : 1);

    printf("\n2. XTest press: F8 reports held, hotkey poll unaffected\n");
    XTestFakeKeyEvent(d, f8code, True, 0);
    XSync(d, False);
    usleep(150000);
    beginFrame();
    int held = isKeyHeld(f8);
    beginFrame();
    int held2 = isKeyHeld(f8);            // repeat query must not consume anything
    int hotkeySawPress = isKeyPressed(f8);
    int ok2 = held == 1 && held2 == 1 && hotkeySawPress == 1;

    printf("\n3. release: F8 reports up again\n");
    XTestFakeKeyEvent(d, f8code, False, 0);
    XSync(d, False);
    usleep(150000);
    beginFrame();
    int ok3 = isKeyHeld(f8) == 0;

    // clean up the physical state
    XTestFakeKeyEvent(d, f8code, False, 0);
    XSync(d, False);

    printf("\nresult: 1=%s 2=%s 3=%s\n", ok1 ? "PASS" : "FAIL", ok2 ? "PASS" : "FAIL", ok3 ? "PASS" : "FAIL");
    printf("%s\n", (ok1 && ok2 && ok3) ? "ALL PASS" : "FAILURES PRESENT");

    XCloseDisplay(d);
    return (ok1 && ok2 && ok3) ? 0 : 1;
}
