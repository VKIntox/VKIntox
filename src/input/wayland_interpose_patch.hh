#pragma once

namespace VKIntox
{
    // Redirect libwayland's internal marshal entry (the jump slot behind
    // wl_proxy_marshal_flags) at our array-flags wrapper so every caller's
    // requests cross the explicit-sync gate. Idempotent and crash-safe:
    // every address read out of an object's dynamic section is validated
    // against that object's own load ranges before use. Retried on later
    // calls so copies of libwayland that load after us are gated too.
    void installWaylandInterposeHooks();

    // The real marshal entry of the libwayland copy the given caller lives
    // in, or null when nothing has been gated yet.
    void* waylandMarshalArrayFlagsForCaller(void* caller);

    // Test hook: locate the jump slot without redirecting it. Returns false
    // when no loaded object carries the relocation.
    bool waylandMarshalSlotForTests(void** slotOut, void** realOut);
} // namespace VKIntox
