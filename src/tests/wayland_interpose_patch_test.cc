// glibc rewrites the dynamic section's pointer entries (DT_SYMTAB, DT_STRTAB,
// DT_JMPREL) to absolute addresses in place at load time. A patcher that adds
// the load base again dereferences a non-canonical pointer and takes the whole
// client down. This drives the real scan against a real libwayland and
// requires the jump slot's current value to match dlsym.
#include "wayland_interpose_patch.hh"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>

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

int main()
{
    const char* override = std::getenv("VKINTOX_TEST_WAYLAND_LIB");
    void* handle = override ? dlopen(override, RTLD_NOW | RTLD_LOCAL)
                            : dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        std::printf("SKIP no libwayland available to scan\n");
        return 77;
    }

    void* slot = nullptr;
    void* real = nullptr;
    check(VKIntox::waylandMarshalSlotForTests(&slot, &real), "jump slot located without faulting");
    if (!slot)
    {
        std::printf("%d failure(s)\n", failures ? failures : 1);
        return 1;
    }

    void* direct = dlsym(handle, "wl_proxy_marshal_array_flags");
    check(direct != nullptr, "dlsym resolves the marshal entry");
    check(real == direct, "slot's value is the real marshal entry");
    check(*static_cast<void**>(slot) == direct, "slot memory holds the real marshal entry");

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
