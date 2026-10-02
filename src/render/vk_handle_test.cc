// Tests for the deferred destroy queue's ordering. The ordering is the whole
// point of the type, so it gets tested directly rather than only through a
// Vulkan run that would need a driver to observe.

#include "vk_handle.hh"

#include <cstdio>
#include <string>
#include <vector>

using VKIntox::DeferredDestroyQueue;
using VKIntox::DestroyPhase;

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

    void expectEq(const std::string& actual, const std::string& wanted, const std::string& what)
    {
        g_checks++;
        if (actual != wanted)
        {
            g_failures++;
            std::printf("  FAILED: %s\n    wanted: %s\n    got:    %s\n", what.c_str(), wanted.c_str(), actual.c_str());
        }
    }

    std::string join(const std::vector<std::string>& items)
    {
        std::string out;
        for (size_t i = 0; i < items.size(); i++)
        {
            if (i)
                out += ",";
            out += items[i];
        }
        return out;
    }

    void reset()
    {
        DeferredDestroyQueue::instance().flush();
    }

    void test_flush_is_noop_when_empty()
    {
        reset();
        expect(DeferredDestroyQueue::instance().pending() == 0, "queue starts empty");
        DeferredDestroyQueue::instance().flush();
        expect(DeferredDestroyQueue::instance().pending() == 0, "flushing an empty queue is safe");
    }

    void test_phase_order_dominates()
    {
        reset();
        auto& queue = DeferredDestroyQueue::instance();

        // registered in the *wrong* order on purpose, so passing means the phase
        // sort did the work rather than the call order happening to be right
        queue.push(DestroyPhase::Memory, [] {});
        queue.push(DestroyPhase::Pipeline, [] {});
        queue.push(DestroyPhase::Swapchain, [] {});
        queue.push(DestroyPhase::Resource, [] {});

        std::vector<std::string> order;
        queue.flush();
        expect(queue.pending() == 0, "queue drains after flush");

        // re-run capturing the order
        queue.push(DestroyPhase::Memory, [&] { order.push_back("memory"); });
        queue.push(DestroyPhase::Pipeline, [&] { order.push_back("pipeline"); });
        queue.push(DestroyPhase::Swapchain, [&] { order.push_back("swapchain"); });
        queue.push(DestroyPhase::Resource, [&] { order.push_back("resource"); });
        queue.flush();

        expectEq(join(order), "swapchain,pipeline,resource,memory", "phases release highest-last-registered first");
    }

    void test_within_phase_is_reverse_registration()
    {
        reset();
        auto& queue = DeferredDestroyQueue::instance();

        std::vector<std::string> order;
        // a view registered after its image must be destroyed first
        queue.push(DestroyPhase::Resource, [&] { order.push_back("image"); });
        queue.push(DestroyPhase::Resource, [&] { order.push_back("view"); });
        queue.flush();

        expectEq(join(order), "view,image", "dependent object released before what it depends on");
    }

    // Index of a name in the order, or -1.
    int at(const std::vector<std::string>& order, const std::string& name)
    {
        for (size_t i = 0; i < order.size(); i++)
            if (order[i] == name)
                return static_cast<int>(i);
        return -1;
    }

    // Asserts "a must be released before b", which is the actual requirement.
    void expectBefore(const std::vector<std::string>& order, const std::string& a, const std::string& b)
    {
        g_checks++;
        const int ia = at(order, a);
        const int ib = at(order, b);
        if (ia < 0 || ib < 0 || ia > ib)
        {
            g_failures++;
            std::printf("  FAILED: %s must be released before %s\n    order was: %s\n", a.c_str(), b.c_str(), join(order).c_str());
        }
    }

    void test_realistic_reshade_order()
    {
        reset();
        auto& queue = DeferredDestroyQueue::instance();
        std::vector<std::string> order;

        // the handles effect_reshade.cc's destructor tears down, which its comment
        // says some NVIDIA drivers crash without
        queue.push(DestroyPhase::Resource, [&] { order.push_back("shaderModule"); });
        queue.push(DestroyPhase::Layout, [&] { order.push_back("descriptorSetLayout"); });
        queue.push(DestroyPhase::Descriptor, [&] { order.push_back("descriptorPool"); });
        queue.push(DestroyPhase::RenderPass, [&] { order.push_back("renderPass"); });
        queue.push(DestroyPhase::Pipeline, [&] { order.push_back("pipeline"); });
        queue.push(DestroyPhase::Layout, [&] { order.push_back("pipelineLayout"); });
        queue.push(DestroyPhase::RenderPass, [&] { order.push_back("framebuffer"); });
        queue.push(DestroyPhase::Memory, [&] { order.push_back("memory"); });

        queue.flush();

        // asserted as the dependencies that actually exist, not as one exact
        // permutation. framebuffers and pipelines are independent -- neither
        // references the other -- so pinning their relative order would encode
        // whatever the old destructor happened to do and fail on a legal change.
        expectBefore(order, "framebuffer", "renderPass");
        expectBefore(order, "pipeline", "pipelineLayout");
        expectBefore(order, "pipelineLayout", "descriptorSetLayout");
        expectBefore(order, "descriptorPool", "descriptorSetLayout");
        expectBefore(order, "descriptorSetLayout", "shaderModule");
        expectBefore(order, "shaderModule", "memory");

        expect(order.back() == "memory", "device memory is released last of all");
    }

    void test_memory_is_always_last()
    {
        reset();
        auto& queue = DeferredDestroyQueue::instance();
        std::vector<std::string> order;

        queue.push(DestroyPhase::Memory, [&] { order.push_back("memory"); });
        queue.push(DestroyPhase::Resource, [&] { order.push_back("resource"); });
        queue.push(DestroyPhase::Sync, [&] { order.push_back("sync"); });
        queue.flush();

        expect(order.back() == "memory", "device memory is released last");
    }

    void test_discard_drops_without_running()
    {
        reset();
        auto& queue = DeferredDestroyQueue::instance();
        bool ran = false;

        queue.push(DestroyPhase::Resource, [&] { ran = true; });
        expect(queue.pending() == 1, "one release pending");

        queue.discard();
        expect(queue.pending() == 0, "discard empties the queue");
        expect(!ran, "discarded releases do not run");
    }

    void test_empty_callable_is_ignored()
    {
        reset();
        auto& queue = DeferredDestroyQueue::instance();
        queue.push(DestroyPhase::Resource, nullptr);
        expect(queue.pending() == 0, "a null release is not queued");
    }

    void test_pending_drains_exactly_once()
    {
        reset();
        auto& queue = DeferredDestroyQueue::instance();
        int count = 0;

        queue.push(DestroyPhase::Resource, [&] { count++; });
        queue.push(DestroyPhase::Resource, [&] { count++; });
        expect(queue.pending() == 2, "two pending");

        queue.flush();
        expect(count == 2, "each release ran once");
        expect(queue.pending() == 0, "queue empty after flush");

        queue.flush();
        expect(count == 2, "a second flush does not re-run anything");
    }
} // namespace

int main()
{
    test_flush_is_noop_when_empty();
    test_phase_order_dominates();
    test_within_phase_is_reverse_registration();
    test_realistic_reshade_order();
    test_memory_is_always_last();
    test_discard_drops_without_running();
    test_empty_callable_is_ignored();
    test_pending_drains_exactly_once();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
