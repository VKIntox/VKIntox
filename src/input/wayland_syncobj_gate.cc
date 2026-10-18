#include "wayland_syncobj_gate.hh"

#include "logger.hh"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace VKIntox::WaylandSyncobjGate
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        std::mutex mutex;
        std::condition_variable cv;

        // surface id -> the presenting thread, while it is inside the driver
        std::unordered_map<uint32_t, std::thread::id> openPresents;

        std::atomic<bool> anyOpen{false};
        std::atomic<uint64_t> held{0};
        std::atomic<uint64_t> budgetMicros{100'000};

        // waits out another thread's present; true if it actually waited
        bool waitForCloseLocked(std::unique_lock<std::mutex>& lock, uint32_t surfaceId)
        {
            const auto deadline = Clock::now() + std::chrono::microseconds(budgetMicros.load(std::memory_order_relaxed));
            bool waited = false;

            while (true)
            {
                auto it = openPresents.find(surfaceId);
                if (it == openPresents.end() || it->second == std::this_thread::get_id())
                    return waited;

                waited = true;
                if (cv.wait_until(lock, deadline) == std::cv_status::timeout)
                {
                    // a present this long means the app is wedged elsewhere
                    Logger::warn("Wayland: present still in flight after the wait budget, letting a commit through");
                    return waited;
                }
            }
        }
    } // namespace

    void beginPresent(uint32_t surfaceId)
    {
        std::lock_guard<std::mutex> lock(mutex);
        openPresents[surfaceId] = std::this_thread::get_id();
        anyOpen.store(true, std::memory_order_release);
    }

    void endPresent(uint32_t surfaceId)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = openPresents.find(surfaceId);
        if (it == openPresents.end() || it->second != std::this_thread::get_id())
            return;

        openPresents.erase(it);
        if (openPresents.empty())
            anyOpen.store(false, std::memory_order_release);
        cv.notify_all();
    }

    bool anyPresentOpen()
    {
        return anyOpen.load(std::memory_order_acquire);
    }

    void beginCommit(uint32_t surfaceId)
    {
        if (!anyOpen.load(std::memory_order_acquire))
            return;

        std::unique_lock<std::mutex> lock(mutex);
        auto it = openPresents.find(surfaceId);
        if (it == openPresents.end() || it->second == std::this_thread::get_id())
            return;

        if (waitForCloseLocked(lock, surfaceId))
        {
            held.fetch_add(1, std::memory_order_relaxed);
            Logger::info("Wayland: held a window commit behind an in-flight present");
        }
    }

    uint64_t heldCount()
    {
        return held.load(std::memory_order_relaxed);
    }

    void setWaitBudgetForTests(uint64_t microseconds)
    {
        budgetMicros.store(microseconds, std::memory_order_relaxed);
    }

    void resetForTests()
    {
        std::lock_guard<std::mutex> lock(mutex);
        openPresents.clear();
        anyOpen.store(false, std::memory_order_relaxed);
        held.store(0, std::memory_order_relaxed);
        budgetMicros.store(100'000, std::memory_order_relaxed);
    }
} // namespace VKIntox::WaylandSyncobjGate
