#include "vk_handle.hh"

#include <algorithm>

#include "logger.hh"
#include "logical_device.hh"
#include "util.hh"

namespace VKIntox
{
    DeferredDestroyQueue& DeferredDestroyQueue::instance()
    {
        // function-local static rather than a namespace-scope object: the queue is
        // touched from static destructors during layer teardown, and a global would
        // already be destroyed by then, which is the classic exit-time-order fiasco
        static DeferredDestroyQueue queue;
        return queue;
    }

    void DeferredDestroyQueue::push(DestroyPhase phase, std::function<void()> release)
    {
        if (!release)
            return;
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingReleases.push_back({phase, nextSequence++, std::move(release)});
    }

    void DeferredDestroyQueue::flush()
    {
        std::vector<DeferredRelease> batch;
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            if (pendingReleases.empty())
                return;

            // Ascending phase, because the phase number is the release order: a
            // framebuffer goes before the render pass it was built from, and device
            // memory goes last of all.
            //
            // Within a phase, reverse registration. A view registered after the image
            // it was made from must therefore be released first, and reverse order
            // gives that without every call site having to know it. stable_sort keeps
            // that exact reverse rather than letting the comparator shuffle equal
            // phases, which is why the tie-break is on sequence alone.
            std::stable_sort(
                pendingReleases.begin(),
                pendingReleases.end(),
                [](const DeferredRelease& a, const DeferredRelease& b) {
                    if (a.phase != b.phase)
                        return static_cast<int>(a.phase) < static_cast<int>(b.phase);
                    return a.sequence > b.sequence;
                });

            // swap out under the lock, then run the releases after unlocking: a
            // callback may push again, and holding the lock across it would deadlock
            batch.swap(pendingReleases);
        }

        // logged because "the queue drained" is the difference between deferring
        // and leaking. a run that grows pending without ever dropping it back to
        // zero is silently losing driver objects, and that is invisible from the
        // outside without this line.
        Logger::debug("[vk-handle] flushing " + std::to_string(batch.size()) + " deferred release(s)");

        for (auto& release : batch)
        {
            // traced per release rather than just counted: when a driver faults
            // inside one of these, the last line in the log is the only thing that
            // says which destroy call did it.
            Logger::debug("[vk-handle] release phase=" + std::to_string(static_cast<int>(release.phase)));
            if (release.release)
                release.release();
        }
    }

    void DeferredDestroyQueue::discard()
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingReleases.clear();
    }

    void recordSubmittedBatch(LogicalDevice* logicalDevice, const char* kind, uint32_t imageIndex,
                              VkSemaphore waitSemaphore, VkSemaphore signalSemaphore, VkFence fence)
    {
        if (!logicalDevice)
            return;

        const uint64_t seq = logicalDevice->submitJournalSeq.fetch_add(1, std::memory_order_relaxed);
        auto& entry = logicalDevice->submitJournal[seq % LogicalDevice::kSubmitJournalSize];
        entry.seq             = seq;
        entry.kind            = kind;
        entry.imageIndex      = imageIndex;
        entry.waitSemaphore   = waitSemaphore;
        entry.signalSemaphore = signalSemaphore;
        entry.fence           = fence;
    }

    bool waitQueueIdleBounded(LogicalDevice* logicalDevice, const char* reason, uint32_t timeoutMs)
    {
        if (!logicalDevice || logicalDevice->queue == VK_NULL_HANDLE)
            return true;

        // vkQueueWaitIdle has no timeout, so it is replaced with an empty
        // submit of our own and a fence we are allowed to walk away from.
        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (logicalDevice->vkd.CreateFence(logicalDevice->device, &fenceInfo, nullptr, &fence) != VK_SUCCESS
            || fence == VK_NULL_HANDLE)
        {
            // nothing to time out on; behave exactly as before
            return logicalDevice->vkd.QueueWaitIdle(logicalDevice->queue) == VK_SUCCESS;
        }

        VkSubmitInfo emptySubmit = {};
        emptySubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkResult result = logicalDevice->vkd.QueueSubmit(logicalDevice->queue, 1, &emptySubmit, fence);
        if (result == VK_SUCCESS)
        {
            result = logicalDevice->vkd.WaitForFences(logicalDevice->device, 1, &fence, VK_TRUE,
                                                      static_cast<uint64_t>(timeoutMs) * 1000000ull);
            if (result == VK_SUCCESS)
            {
                logicalDevice->vkd.DestroyFence(logicalDevice->device, fence, nullptr);
                return true;
            }
        }

        // the submission this fence tracks is still pending, so the fence has to
        // outlive this call; the deferred queue is the only release point that
        // can wait for it
        VkDevice device = logicalDevice->device;
        auto vkd = logicalDevice->vkd;
        DeferredDestroyQueue::instance().push(DestroyPhase::Resource,
                                              [vkd, device, fence] { vkd.DestroyFence(device, fence, nullptr); });

        Logger::warn(std::string("bounded queue idle timed out (vkWaitForFences=") + std::to_string(result) + "): "
                     + (reason ? reason : "unspecified")
                     + "; a submission that never completes is holding the queue");

        // the batches the queue might be stuck on, oldest first with fence state
        const uint64_t journalSeq = logicalDevice->submitJournalSeq.load(std::memory_order_relaxed);
        const uint32_t journalCount = journalSeq < LogicalDevice::kSubmitJournalSize
            ? static_cast<uint32_t>(journalSeq) : LogicalDevice::kSubmitJournalSize;
        Logger::warn("submit journal, newest last (" + std::to_string(journalCount) + " entries):");
        for (uint32_t i = 0; i < journalCount; i++)
        {
            const auto& entry = logicalDevice->submitJournal[(journalSeq - journalCount + i) % LogicalDevice::kSubmitJournalSize];
            const VkResult fenceStatus = entry.fence != VK_NULL_HANDLE
                ? logicalDevice->vkd.GetFenceStatus(logicalDevice->device, entry.fence)
                : VK_SUCCESS;
            Logger::warn("  #" + std::to_string(entry.seq)
                         + " " + std::string(entry.kind ? entry.kind : "?")
                         + " img=" + std::to_string(entry.imageIndex)
                         + " wait=" + convertToString(entry.waitSemaphore)
                         + " signal=" + convertToString(entry.signalSemaphore)
                         + " fence=" + convertToString(entry.fence)
                         + " fenceStatus=" + std::to_string(fenceStatus));
        }
        return false;
    }

} // namespace VKIntox
