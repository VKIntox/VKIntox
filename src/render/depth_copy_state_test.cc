// Tests for DepthCopyState, the mutex-guarded handoff between CmdEndRenderPass*
// and QueueSubmit. The lock is the whole point, so it is exercised directly:
// the functional cases pin the semantics, and the threaded cases fail loudly if
// the mutex ever goes away (torn DepthState reads, lost ring increments).

#include "depth_copy_state.hh"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using VKIntox::DepthCopyState;
using VKIntox::DepthState;

namespace
{
    int g_failures = 0;
    int g_checks   = 0;

    void expect(bool condition, const std::string& what)
    {
        g_checks++;
        if (!condition)
        {
            g_failures++;
            std::printf("  FAILED: %s\n", what.c_str());
        }
    }

    DepthState makeDepth(VkImage image, VkFormat format = VK_FORMAT_D32_SFLOAT)
    {
        DepthState depth;
        depth.image = image;
        depth.imageView = reinterpret_cast<VkImageView>(image);
        depth.format = format;
        depth.extent = {64, 64, 1};
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        return depth;
    }

    // sentinel images are never VK_NULL_HANDLE, so a consumed copy from a torn
    // publish is detectable as a null image
    VkImage sentinelImage(uintptr_t n)
    {
        return reinterpret_cast<VkImage>(n);
    }

    void test_consume_without_publish_is_empty()
    {
        DepthCopyState state;
        DepthState depth;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

        expect(!state.hasPending(), "a fresh state has nothing pending");
        expect(!state.consume(depth, layout), "consume on empty returns false");
        expect(!state.hasPending(), "consume on empty leaves nothing pending");
    }

    void test_publish_consume_round_trips_exactly_once()
    {
        DepthCopyState state;
        const DepthState published = makeDepth(sentinelImage(0x42));

        state.publish(published, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        expect(state.hasPending(), "publish marks a copy pending");

        DepthState depth;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        expect(state.consume(depth, layout), "consume takes the pending copy");
        expect(depth.image == published.image, "consumed image matches published");
        expect(depth.imageView == published.imageView, "consumed view matches published");
        expect(depth.format == published.format, "consumed format matches published");
        expect(depth.extent.width == 64 && depth.extent.height == 64, "consumed extent matches published");
        expect(layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, "consumed layout matches published");

        expect(!state.hasPending(), "consume clears the pending flag");
        expect(!state.consume(depth, layout), "a second consume returns false");
    }

    void test_later_publish_wins()
    {
        DepthCopyState state;
        state.publish(makeDepth(sentinelImage(0x1)), VK_IMAGE_LAYOUT_GENERAL);
        state.publish(makeDepth(sentinelImage(0x2)), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        DepthState depth;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        expect(state.consume(depth, layout), "the newest copy is pending");
        expect(depth.image == sentinelImage(0x2), "the newest publish overwrote the older one");
        expect(layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, "the newest layout is kept");
    }

    void test_ring_starts_unready_and_installs()
    {
        DepthCopyState state;
        state.withRing([&](DepthCopyState::RingAccess& ring) {
            expect(!ring.ready(), "a fresh ring is not ready");
            expect(ring.slotCount() == 0, "a fresh ring has no slots");
            expect(ring.fence(0) == VK_NULL_HANDLE, "an empty ring returns no fence");
        });

        VkCommandPool pool = (VkCommandPool)0x1234;
        std::vector<VkCommandBuffer> buffers = {
            (VkCommandBuffer)0x11, (VkCommandBuffer)0x22,
            (VkCommandBuffer)0x33, (VkCommandBuffer)0x44,
        };
        std::vector<VkFence> fences = {
            (VkFence)0x51, (VkFence)0x52, (VkFence)0x53, (VkFence)0x54,
        };

        state.withRing([&](DepthCopyState::RingAccess& ring) {
            ring.install(pool, buffers, fences);
        });

        state.withRing([&](DepthCopyState::RingAccess& ring) {
            expect(ring.ready(), "the ring is ready after install");
            expect(ring.pool() == pool, "install keeps the pool handle");
            expect(ring.slotCount() == 4, "install keeps four slots");
            expect(ring.buffer(2) == (VkCommandBuffer)0x33, "buffers are indexed by slot");
            expect(ring.fence(3) == (VkFence)0x54, "fences are indexed by slot");
            expect(ring.fence(9) == VK_NULL_HANDLE, "an out-of-range slot returns no fence");
        });
    }

    void test_ring_slots_wrap_in_order()
    {
        DepthCopyState state;
        state.withRing([&](DepthCopyState::RingAccess& ring) {
            ring.install((VkCommandPool)0x1,
                         {(VkCommandBuffer)0x10, (VkCommandBuffer)0x20, (VkCommandBuffer)0x30, (VkCommandBuffer)0x40},
                         {(VkFence)0x1, (VkFence)0x2, (VkFence)0x3, (VkFence)0x4});
        });

        const uint32_t wanted[] = {0, 1, 2, 3, 0, 1, 2, 3, 0};
        state.withRing([&](DepthCopyState::RingAccess& ring) {
            for (uint32_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); ++i)
                expect(ring.reserveSlot() == wanted[i], "slots are handed out in ring order");
        });
    }

    void test_ring_detach_clears()
    {
        DepthCopyState state;
        const VkCommandPool pool = (VkCommandPool)0xabc;
        state.withRing([&](DepthCopyState::RingAccess& ring) {
            ring.install(pool,
                         {(VkCommandBuffer)0x1, (VkCommandBuffer)0x2},
                         {(VkFence)0x1, (VkFence)0x2});
        });

        VkCommandPool detachedPool = VK_NULL_HANDLE;
        std::vector<VkFence> detachedFences;
        state.withRing([&](DepthCopyState::RingAccess& ring) {
            ring.detach(detachedPool, detachedFences);
            expect(!ring.ready(), "detach leaves the ring unready");
            expect(ring.slotCount() == 0, "detach drops the slots");
        });

        expect(detachedPool == pool, "detach hands back the pool");
        expect(detachedFences.size() == 2, "detach hands back every fence");
        expect(detachedFences[0] == (VkFence)0x1, "detach keeps fence order");
    }

    void test_concurrent_publish_consume_never_tears()
    {
        DepthCopyState state;
        const int kIterations = 200000;
        const int kConsumers  = 4;

        std::atomic<int> consumed{0};
        std::atomic<int> torn{0};

        std::thread producer([&] {
            for (int i = 0; i < kIterations; ++i)
                state.publish(makeDepth(sentinelImage(static_cast<uintptr_t>(i) + 1)),
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        });

        std::vector<std::thread> consumers;
        consumers.reserve(kConsumers);
        for (int c = 0; c < kConsumers; ++c)
        {
            consumers.emplace_back([&] {
                for (int i = 0; i < kIterations; ++i)
                {
                    DepthState depth;
                    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
                    if (!state.consume(depth, layout))
                        continue;
                    consumed.fetch_add(1, std::memory_order_relaxed);
                    // a copy published under the lock always has every field set;
                    // a torn read would surface a null image or an empty extent
                    if (depth.image == VK_NULL_HANDLE || depth.extent.width == 0 || depth.imageView == VK_NULL_HANDLE)
                        torn.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        producer.join();
        for (auto& t : consumers)
            t.join();

        expect(torn.load() == 0, "concurrent consume never observes a torn publish");
        expect(consumed.load() > 0, "some published copies were consumed");
    }

    void test_concurrent_ring_reservation_is_atomic()
    {
        DepthCopyState state;
        state.withRing([&](DepthCopyState::RingAccess& ring) {
            ring.install((VkCommandPool)0x1,
                         {(VkCommandBuffer)0x10, (VkCommandBuffer)0x20, (VkCommandBuffer)0x30, (VkCommandBuffer)0x40},
                         {(VkFence)0x1, (VkFence)0x2, (VkFence)0x3, (VkFence)0x4});
        });

        const int kThreads = 8;
        const int kPerThread = 50000;
        std::atomic<long long> total{0};
        std::atomic<bool> bad{false};

        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t)
        {
            workers.emplace_back([&] {
                for (int i = 0; i < kPerThread; ++i)
                {
                    state.withRing([&](DepthCopyState::RingAccess& ring) {
                        const uint32_t slot = ring.reserveSlot();
                        total.fetch_add(1, std::memory_order_relaxed);
                        if (slot >= DepthCopyState::RING_SIZE || ring.buffer(slot) == VK_NULL_HANDLE)
                            bad.store(true, std::memory_order_relaxed);
                    });
                }
            });
        }
        for (auto& w : workers)
            w.join();

        expect(total.load() == static_cast<long long>(kThreads) * kPerThread,
               "every concurrent reservation is counted");
        expect(!bad.load(), "every reserved slot maps to a real command buffer");
    }
} // namespace

int main()
{
    test_consume_without_publish_is_empty();
    test_publish_consume_round_trips_exactly_once();
    test_later_publish_wins();
    test_ring_starts_unready_and_installs();
    test_ring_slots_wrap_in_order();
    test_ring_detach_clears();
    test_concurrent_publish_consume_never_tears();
    test_concurrent_ring_reservation_is_atomic();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
