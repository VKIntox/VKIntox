#ifndef VK_HANDLE_HPP_INCLUDED
#define VK_HANDLE_HPP_INCLUDED

#include "vulkan_include.hh"

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace VKIntox
{
    // Destruction order for owned handles, earliest first.
    //
    // The number IS the order: a handle is released before any handle with a
    // higher number. That reads backwards from a "priority" enum, so it is worth
    // being blunt about it, because getting it inverted releases device memory
    // first and turns every resource above it into a use-after-free.
    //
    // Vulkan lets you destroy a render pass while framebuffers that reference it
    // are still alive; the spec only forbids destroying an object that something
    // *in use* still depends on. Drivers vary in how forgiving they are about it,
    // and the ReshadeEffect teardown comment in effect_reshade.cc records that
    // some NVIDIA versions crash when a shader module is destroyed while the
    // driver still has tracking records for it. Ordering therefore cannot be
    // incidental: it has to be stated, and enforced in one place, rather than
    // being whatever order a particular destructor happened to use.
    enum class DestroyPhase : uint8_t
    {
        // swapped-in and attached. nothing depends on these, so they go first
        // while the device is otherwise idle.
        Swapchain = 0,

        // command pools and their buffers, plus fences and semaphases. these
        // must go before anything a recorded command buffer refers to.
        Sync = 1,

        // the compute/graphics pipeline. a pipeline holds internal references to
        // its layout, descriptor sets and shader stages, so it goes before all
        // three. its own VkPipelineLayout is NOT here -- see Layout.
        Pipeline = 2,

        // render passes and the framebuffers built from them. framebuffers
        // reference the render pass, so framebuffers are registered one phase
        // earlier and land here first.
        RenderPass = 3,

        // descriptor pools implicitly free the sets allocated from them, so a
        // pool has to outlive any code still holding one of its sets. the sets
        // themselves need no destroy call, which is why there is no phase here.
        Descriptor = 4,

        // descriptor set layouts and the pipeline layout. a pipeline layout
        // references the descriptor set layouts it names, so it shares this
        // phase rather than Pipeline: two handles where one depends on the other
        // must not share a phase, because within a phase the order is
        // reverse-registration and that would put the layout first.
        Layout = 5,

        // images, buffers, image views, samplers and shader modules. views
        // reference the images they were made from, and stages reference the
        // module, so the queue releases within a phase in reverse registration
        // order, which puts the dependent object first.
        Resource = 6,

        // device memory last: it backs everything above, and freeing it early is
        // the classic use-after-free in a Vulkan layer.
        Memory = 7,
    };

    // One deferred destruction. The function closes over whatever it needs, so
    // the queue stays ignorant of handle types entirely -- adding a new kind of
    // resource to defer costs nothing here.
    struct DeferredRelease
    {
        DestroyPhase           phase;
        uint64_t               sequence;  // registration order, so ties are FIFO
        std::function<void()> release;
    };

    // Resources are handed to the queue on scope exit and actually destroyed at a
    // point the caller has established is safe. The reason this exists rather
    // than a scope-bound deleter: Vulkan forbids destroying anything the GPU is
    // still using, and the moment a handle goes out of scope is almost never
    // that moment. Frames are in flight, command buffers are recorded, and the
    // driver holds internal references. The safe points are queue idle and
    // swapchain recreation, and the queue is what turns "scoped handle" into
    // "destroyed when it is actually legal to".
    class DeferredDestroyQueue
    {
    public:
        static DeferredDestroyQueue& instance();

        // Registers a destruction, to run at or before the next flush.
        void push(DestroyPhase phase, std::function<void()> release);

        // Runs every pending release, highest phase first, and within a phase in
        // reverse registration order so a dependent object goes before the thing
        // it depends on. Safe to call when nothing is pending.
        void flush();

        // Number of releases still waiting. Tests assert this reaches zero after a
        // teardown, which is the only way to catch a double-free or a leak of
        // driver objects that the layer silently forgot about.
        size_t pending() const
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            return pendingReleases.size();
        }

        // Drops pending releases without running them. Only for the case where
        // the device is already gone, so calling Vulkan would be undefined; a
        // leak at that point is preferable to a crash during teardown.
        void discard();

    private:
        DeferredDestroyQueue() = default;

        // push runs on the present thread while flush also runs from the submit
        // thread and swapchain teardown, so the container needs its own lock
        mutable std::mutex           pendingMutex;
        std::vector<DeferredRelease> pendingReleases;
        uint64_t                     nextSequence = 0;
    };

} // namespace VKIntox

#endif // VK_HANDLE_HPP_INCLUDED
