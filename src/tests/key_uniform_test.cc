// KeyUniform modes and plumbing, against the layer's real keyboard state:
// hold, press and toggle semantics, invert, and unusable keycodes, with keys
// injected through XTest.
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
#include <vector>

#include "vendor/reshade/effect_expression.hh"
#include "vendor/reshade/effect_module.hh"

typedef void (*InitFn)(bool);
typedef void (*SetWindowFn)(unsigned long);
typedef void (*BeginFrameFn)();
typedef int (*IsKeyFn)(uint32_t);
// std::string passes as an invisible pointer under the itanium ABI, so a
// const-reference signature here matches the by-value one in the layer
typedef uint32_t (*ConvertKeySymFn)(const std::string&);

// member functions through dlsym: the this pointer comes first
typedef void KeyUniformCtorFn(void* self, const reshadefx::uniform_info*);
typedef void KeyUniformUpdateFn(void* self, void* mappedBuffer);
typedef void KeyUniformDtorFn(void* self);

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

    InitFn        initInputBlocker = (InitFn) dlsym(lib, "_ZN7VKIntox16initInputBlockerEb");
    SetWindowFn   setX11Window     = (SetWindowFn) dlsym(lib, "_ZN7VKIntox12setX11WindowEm");
    BeginFrameFn  beginFrame       = (BeginFrameFn) dlsym(lib, "_ZN7VKIntox23beginKeyboardInputFrameEv");
    IsKeyFn       isKeyHeld        = (IsKeyFn) dlsym(lib, "_ZN7VKIntox9isKeyHeldEj");
    IsKeyFn       isKeyPressed     = (IsKeyFn) dlsym(lib, "_ZN7VKIntox12isKeyPressedEj");
    ConvertKeySymFn convertToKeySym = (ConvertKeySymFn) dlsym(lib, "_ZN7VKIntox15convertToKeySymENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
    auto keyUniformCtor   = (KeyUniformCtorFn*) dlsym(lib, "_ZN7VKIntox10KeyUniformC1EN9reshadefx12uniform_infoE");
    auto keyUniformUpdate = (KeyUniformUpdateFn*) dlsym(lib, "_ZN7VKIntox10KeyUniform6updateEPv");
    auto keyUniformDtor   = (KeyUniformDtorFn*) dlsym(lib, "_ZN7VKIntox10KeyUniformD1Ev");
    if (!initInputBlocker || !setX11Window || !beginFrame || !isKeyHeld || !isKeyPressed || !convertToKeySym
        || !keyUniformCtor || !keyUniformUpdate || !keyUniformDtor)
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

    auto intAnnotation = [](const char* name, int32_t value)
    {
        reshadefx::annotation a{};
        a.name = name;
        a.type = { reshadefx::type::t_int, 1, 1 };
        a.value.as_int[0] = value;
        return a;
    };
    auto stringAnnotation = [](const char* name, const char* value)
    {
        reshadefx::annotation a{};
        a.name = name;
        a.type = { reshadefx::type::t_string, 1, 1 };
        a.value.string_data = value;
        return a;
    };

    auto makeUniform = [&](std::vector<reshadefx::annotation> annotations)
    {
        reshadefx::uniform_info info{};
        info.type = { reshadefx::type::t_bool, 1, 1 };
        info.offset = 0;
        info.size = 4;
        info.annotations = std::move(annotations);
        void* storage = new unsigned char[256];
        keyUniformCtor(storage, &info);
        return storage;
    };

    int failures = 0;
    auto expect = [&](const char* label, void* uniform, uint32_t expected)
    {
        keyUniformUpdate(uniform, uniform);
        uint32_t got = 0;
        memcpy(&got, uniform, 4);
        printf("    %-28s got=%u expected=%u %s\n", label, got, expected, got == expected ? "" : "FAIL");
        if (got != expected) failures++;
    };
    auto press = [&]()
    {
        XTestFakeKeyEvent(d, f8code, True, 0);
        XSync(d, False);
        usleep(120000);
        beginFrame();
    };
    auto release = [&]()
    {
        XTestFakeKeyEvent(d, f8code, False, 0);
        XSync(d, False);
        usleep(120000);
        beginFrame();
    };
    auto dropUniform = [&](void* u)
    {
        keyUniformDtor(u);
        delete[] (unsigned char*)u;
    };

    // drain: make sure the key is up and both edges have seen it
    release();

    printf("\n1. hold\n");
    {
        void* u = makeUniform({stringAnnotation("source", "key"), intAnnotation("keycode", (int32_t)vkF8)});
        expect("up", u, 0);
        press();
        expect("down", u, 1);
        expect("still down", u, 1);
        release();
        expect("up again", u, 0);
        dropUniform(u);
    }

    printf("\n2. hold inverted\n");
    {
        void* u = makeUniform({stringAnnotation("source", "key"), intAnnotation("keycode", (int32_t)vkF8), intAnnotation("invert", 1)});
        expect("up", u, 1);
        press();
        expect("down", u, 0);
        release();
        expect("up again", u, 1);
        dropUniform(u);
    }

    printf("\n3. press\n");
    {
        void* u = makeUniform({stringAnnotation("source", "key"), intAnnotation("keycode", (int32_t)vkF8), stringAnnotation("mode", "press")});
        expect("up", u, 0);
        press();
        expect("press frame", u, 1);
        expect("still held", u, 0);
        release();
        expect("up", u, 0);
        dropUniform(u);
    }

    printf("\n4. toggle\n");
    {
        void* u = makeUniform({stringAnnotation("source", "key"), intAnnotation("keycode", (int32_t)vkF8), stringAnnotation("mode", "toggle")});
        expect("start", u, 0);
        press();
        expect("first press", u, 1);
        release();
        expect("after release", u, 1);
        press();
        expect("second press", u, 0);
        release();
        expect("after release", u, 0);
        dropUniform(u);
    }

    printf("\n5. unusable keycode writes nothing\n");
    {
        void* u = makeUniform({stringAnnotation("source", "key"), intAnnotation("keycode", 400)});
        memset(u, 0xAB, 4);
        keyUniformUpdate(u, u);
        uint32_t got = 0;
        memcpy(&got, u, 4);
        printf("    untouched=%s\n", got == 0xABABABABu ? "yes" : "no");
        if (got != 0xABABABABu) failures++;
        dropUniform(u);
    }

    printf("\n6. int and float uniform types\n");
    {
        auto makeTyped = [&](reshadefx::type::datatype base, uint32_t size)
        {
            reshadefx::uniform_info info{};
            info.type = { base, 1, 1 };
            info.offset = 0;
            info.size = size;
            info.annotations = {stringAnnotation("source", "key"), intAnnotation("keycode", (int32_t)vkF8)};
            void* storage = new unsigned char[256];
            keyUniformCtor(storage, &info);
            return storage;
        };
        void* u = makeTyped(reshadefx::type::t_int, 4);
        keyUniformUpdate(u, u);
        int32_t iv = 0;
        memcpy(&iv, u, 4);
        press();
        keyUniformUpdate(u, u);
        int32_t ivDown = 0;
        memcpy(&ivDown, u, 4);
        release();
        keyUniformDtor(u); delete[] (unsigned char*)u;
        printf("    int: up=%d down=%d\n", iv, ivDown);
        if (!(iv == 0 && ivDown == 1)) failures++;

        void* f = makeTyped(reshadefx::type::t_float, 4);
        press();
        keyUniformUpdate(f, f);
        float fv = 0;
        memcpy(&fv, f, 4);
        release();
        keyUniformUpdate(f, f);
        float fvUp = 0;
        memcpy(&fvUp, f, 4);
        keyUniformDtor(f); delete[] (unsigned char*)f;
        printf("    float: down=%.1f up=%.1f\n", fv, fvUp);
        if (!(fv == 1.0f && fvUp == 0.0f)) failures++;
    }

    // clean up the physical state
    release();

    printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    XDestroyWindow(d, win);
    XCloseDisplay(d);
    return failures == 0 ? 0 : 1;
}
