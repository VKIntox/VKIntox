#include "logical_swapchain.hh"

#include "vk_handle.hh"

namespace VKIntox
{
    void destroyDepthResolveResources(LogicalSwapchain* logicalSwapchain);

    void LogicalSwapchain::destroy()
    {
        if (imageCount > 0)
        {
            // Wait for GPU to finish before destroying resources
            Logger::info("[DESTROY-TRACE] LogicalSwapchain::destroy: QueueWaitIdle");
            logicalDevice->vkd.QueueWaitIdle(logicalDevice->queue);

            // Reset the command pool BEFORE destroying effects.  This puts all
            // allocated command buffers back into the initial state, clearing
            // any driver-internal tracking of which pipelines / render passes
            // they reference.  Some NVIDIA driver versions crash if effect
            // objects are destroyed while the driver still has tracking records.
            Logger::info("[DESTROY-TRACE] ResetCommandPool");
            logicalDevice->vkd.ResetCommandPool(logicalDevice->device, logicalDevice->commandPool, 0);

            // Free command buffers
            if (!commandBuffersEffect.empty())
            {
                logicalDevice->vkd.FreeCommandBuffers(
                    logicalDevice->device, logicalDevice->commandPool, commandBuffersEffect.size(), commandBuffersEffect.data());
                commandBuffersEffect.clear();
            }
            if (!commandBuffersNoEffect.empty())
            {
                logicalDevice->vkd.FreeCommandBuffers(
                    logicalDevice->device, logicalDevice->commandPool, commandBuffersNoEffect.size(), commandBuffersNoEffect.data());
                commandBuffersNoEffect.clear();
            }
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

            // Images before the memory backing them. These two loops used to run
            // the other way round, which freed every allocation while the image
            // living in it was still alive -- destroying an image whose memory has
            // already been returned is a use-after-free, and it is the exact
            // ordering the deferred queue exists to enforce everywhere else.
            // QueueWaitIdle above is what makes it safe to do either way round
            // from the GPU's perspective, but not from the allocator's.
            for (uint32_t i = 0; i < fakeImages.size(); i++)
            {
                logicalDevice->vkd.DestroyImage(logicalDevice->device, fakeImages[i], nullptr);
            }

            for (VkDeviceMemory mem : fakeImageMemories)
                logicalDevice->vkd.FreeMemory(logicalDevice->device, mem, nullptr);
            fakeImageMemories.clear();

            // Walked per vector rather than indexed by imageCount: the two
            // semaphore vectors are filled in at different points during
            // setup, and a short one would be read past its end. Same coupling
            // that bit SmaaEffect and SimpleEffect.
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
                logicalDevice->vkd.DestroyImageView(logicalDevice->device, view, nullptr);
            }
            imageViews.clear();

            // Note: ImGui overlay is now at device level, not destroyed here
        }
    }
} // namespace VKIntox
