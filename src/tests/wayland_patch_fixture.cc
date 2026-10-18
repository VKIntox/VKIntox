// The ELF shape the patcher targets: an exported marshal entry whose internal
// call goes through a jump slot. Ubuntu's wayland binds that call directly
// (no slot), so this fixture stands in for the shape the patcher must handle
// and keeps the test independent of how a distro built its wayland.
extern "C" __attribute__((noinline, visibility("default")))
void wl_proxy_marshal_array_flags()
{
}

extern "C" __attribute__((noinline, visibility("default")))
void vkintox_patch_fixture_caller()
{
    wl_proxy_marshal_array_flags();
}
