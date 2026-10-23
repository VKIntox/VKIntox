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

        // syncobj id -> the wl_surface it was created for
        std::unordered_map<uint32_t, uint32_t> surfaceOfSyncobj;
        // surface id -> the presenter thread whose sequence is open
        std::unordered_map<uint32_t, std::thread::id> openSequences;

        std::atomic<bool> anyOpen{false};
        std::atomic<uint64_t> held{0};
        std::atomic<uint64_t> budgetMicros{100'000};

        // callers must hold mutex
        void closeLocked(uint32_t surfaceId)
        {
            openSequences.erase(surfaceId);
            if (openSequences.empty())
                anyOpen.store(false, std::memory_order_release);
            cv.notify_all();
        }

        // waits out another thread's open sequence; true if it actually waited
        bool waitForCloseLocked(std::unique_lock<std::mutex>& lock, uint32_t surfaceId)
        {
            const auto deadline = Clock::now() + std::chrono::microseconds(budgetMicros.load(std::memory_order_relaxed));
            bool waited = false;

            while (true)
            {
                auto it = openSequences.find(surfaceId);
                if (it == openSequences.end() || it->second == std::this_thread::get_id())
                    return waited;

                waited = true;
                if (cv.wait_until(lock, deadline) == std::cv_status::timeout)
                {
                    // never let one stuck presenter queue up every later commit
                    Logger::warn("Wayland: explicit-sync present left its sequence open, releasing a held commit");
                    closeLocked(surfaceId);
                    return waited;
                }
            }
        }
    } // namespace

    void noteSyncobjSurface(uint32_t syncobjId, uint32_t surfaceId)
    {
        std::lock_guard<std::mutex> lock(mutex);
        surfaceOfSyncobj[syncobjId] = surfaceId;
    }

    void noteSyncobjDestroyed(uint32_t syncobjId)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = surfaceOfSyncobj.find(syncobjId);
        if (it == surfaceOfSyncobj.end())
            return;

        closeLocked(it->second);
        surfaceOfSyncobj.erase(it);
    }

    void notePointSetForSyncobj(uint32_t syncobjId)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = surfaceOfSyncobj.find(syncobjId);
        if (it == surfaceOfSyncobj.end())
            return;

        openSequences[it->second] = std::this_thread::get_id();
        anyOpen.store(true, std::memory_order_release);
    }

    void beginCommit(uint32_t surfaceId)
    {
        if (!anyOpen.load(std::memory_order_acquire))
            return;

        std::unique_lock<std::mutex> lock(mutex);
        auto it = openSequences.find(surfaceId);
        if (it == openSequences.end() || it->second == std::this_thread::get_id())
            return;

        if (waitForCloseLocked(lock, surfaceId))
        {
            held.fetch_add(1, std::memory_order_relaxed);
            Logger::info("Wayland: held a window commit behind an open explicit-sync present");
        }
    }

    void endCommit(uint32_t surfaceId)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = openSequences.find(surfaceId);
        if (it != openSequences.end() && it->second == std::this_thread::get_id())
            closeLocked(surfaceId);
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
        surfaceOfSyncobj.clear();
        openSequences.clear();
        anyOpen.store(false, std::memory_order_relaxed);
        held.store(0, std::memory_order_relaxed);
        budgetMicros.store(100'000, std::memory_order_relaxed);
    }
} // namespace VKIntox::WaylandSyncobjGate
