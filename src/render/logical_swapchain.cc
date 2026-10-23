#include "logical_swapchain.hh"

#include "vk_handle.hh"
#include "util.hh"

namespace VKIntox
{
    void destroyDepthResolveResources(LogicalSwapchain* logicalSwapchain);

    bool LogicalSwapchain::destroy()
    {
        if (imageCount > 0)
        {
            // Wait for GPU to finish before destroying resources. Bounded: if
            // the idle times out, freeing anything here is a use-after-free, so
            // leaking this swapchain is the survivable trade.
            Logger::info("[DESTROY-TRACE] LogicalSwapchain::destroy: QueueWaitIdle");
            if (!waitQueueIdleBounded(logicalDevice, "swapchain destroy"))
            {
                Logger::warn("[DESTROY-TRACE] swapchain destroy idle timed out; leaving this swapchain's resources alone");
                return false;
            }

            // destroy the pool before the effects: that frees this swapchain's
            // buffers and clears driver tracking without touching a
            // replacement swapchain's pool
            if (commandPool != VK_NULL_HANDLE)
            {
                Logger::info("[DESTROY-TRACE] DestroyCommandPool");
                logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, commandPool, nullptr);
                commandPool = VK_NULL_HANDLE;
            }
            commandBuffersEffect.clear();
            commandBuffersNoEffect.clear();
            Logger::info("[DESTROY-TRACE] command buffers freed");

            Logger::info("[DESTROY-TRACE] destroying " + std::to_string(effects.size()) + " effects");
            effects.clear();
            defaultTransfer.reset();
            Logger::info("[DESTROY-TRACE] effects destroyed");

            // The flush has to come AFTER the effects are cleared, not before.
            // Clearing is what hands their handles to the queue, so a flush placed
            // earlier drains an empty queue and the releases sit there until some
            // later teardown -- which, if the process exits first, is a straight
            // leak of every object the effect owned. The queue wait above is what
            // makes this the legal moment to run them.
            DeferredDestroyQueue::instance().flush();

            destroyDepthResolveResources(this);

            // Fake images may still have application image views attached.
            // Destroying the image first is a use-after-free on Sober's
            // translation layer, so hand them to the view-count gate instead.
            for (VkImage image : fakeImages)
            {
                auto memIt = logicalDevice->fakeImageMemory.find(image);
                VkDeviceMemory memory = memIt != logicalDevice->fakeImageMemory.end() ? memIt->second : VK_NULL_HANDLE;
                auto countIt = logicalDevice->fakeImageViewCounts.find(image);
                if (countIt == logicalDevice->fakeImageViewCounts.end() || countIt->second == 0)
                {
                    logicalDevice->vkd.DestroyImage(logicalDevice->device, image, nullptr);
                    if (memory != VK_NULL_HANDLE)
                    {
                        logicalDevice->vkd.FreeMemory(logicalDevice->device, memory, nullptr);
                        logicalDevice->fakeImageMemory.erase(image);
                    }
                    logicalDevice->fakeImageViewCounts.erase(image);
                }
                else
                {
                    logicalDevice->pendingFakeImageDestroy.emplace_back(image, memory);
                    Logger::debug("swapchain destroy: deferring fake image " + convertToString(image)
                                  + " with " + std::to_string(countIt->second) + " live app view(s)");
                }
            }
            fakeImageMemories.clear();

            // Walked per vector rather than indexed by imageCount: the two
            // semaphore vectors are filled in at different points during
            // setup, and a short one would be read past its end.
            for (auto sem : semaphores)
            {
                if (sem != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroySemaphore(logicalDevice->device, sem, nullptr);
            }
            semaphores.clear();

            for (auto sem : overlaySemaphores)
            {
                if (sem != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroySemaphore(logicalDevice->device, sem, nullptr);
            }
            overlaySemaphores.clear();

            // Destroy per-image effect submit fences
            for (VkFence f : effectSubmitFences)
            {
                if (f != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroyFence(logicalDevice->device, f, nullptr);
            }
            effectSubmitFences.clear();

            Logger::debug("after DestroySemaphore/Fence");

            // Destroy image views for overlay
            for (auto& view : imageViews)
            {
                if (view != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroyImageView(logicalDevice->device, view, nullptr);
            }
            imageViews.clear();

            // Note: ImGui overlay is now at device level, not destroyed here
        }
        return true;
    }
} // namespace VKIntox
