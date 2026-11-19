#pragma once

// LSFG frame insertion in the layer's present path. Mirrors lsfg-vk's own
// swapchain wrapper: the composited frame is copied into the engine's source
// image, the engine generates the intermediate frames, and each one is blitted
// into an extra swapchain image and presented ahead of the real frame.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "framegen/lsfg_engine.hh"

namespace VKIntox
{
    struct LogicalDevice;
    struct LogicalSwapchain;

    namespace FrameGen
    {
        // Settings snapshot the present path reads without touching config locks
        struct LsfgProfile
        {
            bool enabled = false;
            uint32_t multiplier = 2;
            float flowScale = 1.0f;
            bool performanceMode = false;
            bool halfPrecision = false;
            std::string dllPath;
        };

        // Swapchain creation: widen usage and grow the image pool so the
        // inserted frames have somewhere to land. Returns true when the info
        // was modified (LSFG enabled and usable).
        bool modifySwapchainCreateInfo(LogicalDevice* logicalDevice,
                                       VkSwapchainCreateInfoKHR& info,
                                       const LsfgProfile& profile);

        // Per-swapchain frame generation state. Created lazily on the first
        // present with LSFG enabled; destroyed with the swapchain.
        class LsfgSwapchain
        {
        public:
            static bool active(LogicalDevice* logicalDevice, const LsfgProfile& profile);

            LsfgSwapchain(LogicalDevice* logicalDevice, LogicalSwapchain* swapchain, const LsfgProfile& profile);
            ~LsfgSwapchain();
            LsfgSwapchain(const LsfgSwapchain&) = delete;
            LsfgSwapchain& operator=(const LsfgSwapchain&) = delete;
            // Runs the full insert sequence for one present: copies the
            // composited image into the engine, presents the generated frames,
            // then the real one. waitSemaphore is the layer's overlay-finished
            // signal for this image; originalWaitSemaphore keeps the app's own
            // dependency chain alive inside the first submit. Returns the
            // result of the last real QueuePresentKHR.
            VkResult present(LogicalDevice* logicalDevice,
                             LogicalSwapchain* swapchain,
                             uint32_t index,
                             VkQueue queue,
                             VkSemaphore waitSemaphore,
                             VkSemaphore originalWaitSemaphore,
                             const VkPresentInfoKHR* originalPresentInfo);

            uint32_t multiplier() const { return m_multiplier; }

        private:
            struct InnerSyncPass
            {
                VkCommandBuffer copyCmdbuf = VK_NULL_HANDLE;
                VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
                VkSemaphore copySemaphore = VK_NULL_HANDLE;
            };
            struct SyncPass
            {
                VkCommandBuffer copyCmdbuf = VK_NULL_HANDLE;
                VkSemaphore presentSemaphore = VK_NULL_HANDLE;
                std::vector<InnerSyncPass> innerSyncPasses;
            };

            void destroy();
            bool createEngineObjects(LogicalDevice* logicalDevice, LogicalSwapchain* swapchain, const LsfgProfile& profile);

            LogicalDevice* m_logicalDevice = nullptr;
            uint32_t m_multiplier = 2;
            VkExtent2D m_extent = {0, 0};
            uint32_t m_iteration = 0;
            uint32_t m_remaining = 0;
            uint64_t m_syncValue = 0;

            VkImage m_sourceImage = VK_NULL_HANDLE;
            VkDeviceMemory m_sourceMemory = VK_NULL_HANDLE;
            VkImage m_destinationImage = VK_NULL_HANDLE;
            VkDeviceMemory m_destinationMemory = VK_NULL_HANDLE;
            VkSemaphore m_syncSemaphore = VK_NULL_HANDLE;
            VkFence m_completionFence = VK_NULL_HANDLE;
            bool m_fenceArmed = false;
            VkCommandPool m_commandPool = VK_NULL_HANDLE;
            std::vector<SyncPass> m_syncPasses;

            std::unique_ptr<EngineInstance> m_engineInstance;
            std::unique_ptr<EngineContext> m_engineContext;
        };
    } // namespace FrameGen
} // namespace VKIntox
