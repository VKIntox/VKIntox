// The marshal patcher parses live ELF state: it must accept glibc's in-place
// absolute dynamic-section pointers, validate every address before reading
// it, and only redirect a jump slot that already holds the object's own
// resolved entry. This drives the real scan against a fixture carrying
// exactly that shape, so the test does not depend on how a distro built its
// wayland (Ubuntu binds the call directly and has no slot to hook).
#include "wayland_interpose_patch.hh"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>

// satisfies the patcher's replacement reference; the probe never installs it
extern "C" void wl_proxy_marshal_array_flags()
{
}

namespace
{
    int failures = 0;

    void check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
            ++failures;
    }
}

int main(int argc, char** argv)
{
    const char* path = std::getenv("VKINTOX_TEST_WAYLAND_LIB");
    if (!path && argc > 1)
        path = argv[1];
    if (!path)
    {
        std::printf("SKIP no fixture path given\n");
        return 77;
    }

    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        std::printf("SKIP could not load %s\n", path);
        return 77;
    }

    void* slot = nullptr;
    void* real = nullptr;
    check(VKIntox::waylandMarshalSlotForTests(&slot, &real), "jump slot located without faulting");
    if (!slot)
    {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }

    void* direct = dlsym(handle, "wl_proxy_marshal_array_flags");
    check(direct != nullptr, "dlsym resolves the marshal entry");
    check(real == direct, "slot's value is the real marshal entry");
    check(*static_cast<void**>(slot) == direct, "slot memory holds the real marshal entry");

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
