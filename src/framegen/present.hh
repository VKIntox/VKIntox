#pragma once

// FG frame insertion in the layer's present path. Mirrors the frame generation engine's own
// swapchain wrapper: the composited frame is copied into the engine's source
// image, the engine generates the intermediate frames, and each one is blitted
// into an extra swapchain image and presented ahead of the real frame, which
// stays in the application's image. Pacing comes from FIFO (forced while FG is
// on); a CPU sleep only stands in when the swapchain could not be made FIFO.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "framegen/engine.hh"

namespace VKIntox
{
    struct LogicalDevice;
    struct LogicalSwapchain;

    namespace FrameGen
    {
        // Settings snapshot the present path reads without touching config locks
        struct FgProfile
        {
            bool enabled = false;
            uint32_t multiplier = 2;
            float flowScale = 1.0f;
            bool performanceMode = false;
            bool halfPrecision = false;
            // force FIFO while generating: under MAILBOX/IMMEDIATE the real
            // frame replaces the generated one before it is ever scanned out
            bool forceVsync = true;
            std::string dllPath;
        };

        // True for present modes that show every queued image in order, which
        // is what lets the presentation engine pace the inserted frames.
        bool presentModeQueuesEveryFrame(VkPresentModeKHR mode);

        // Patches a VK_EXT_swapchain_maintenance1 per-present mode switch in
        // the chain back to FIFO, so the app cannot leave vsync mid-session.
        void forceFifoInPresentChain(const VkPresentInfoKHR* presentInfo);

        // Swapchain creation: widen usage and grow the image pool so the
        // inserted frames have somewhere to land. Returns true when the info
        // was modified (FG enabled and usable).
        bool modifySwapchainCreateInfo(LogicalDevice* logicalDevice,
                                       VkSwapchainCreateInfoKHR& info,
                                       const FgProfile& profile);

        // Per-swapchain frame generation state. Created lazily on the first
        // present with FG enabled; destroyed with the swapchain.
        class FgSwapchain
        {
        public:
            static bool active(LogicalDevice* logicalDevice, const FgProfile& profile);

            FgSwapchain(LogicalDevice* logicalDevice, LogicalSwapchain* swapchain, const FgProfile& profile);
            ~FgSwapchain();
            FgSwapchain(const FgSwapchain&) = delete;
            FgSwapchain& operator=(const FgSwapchain&) = delete;
            // Runs the full insert sequence for one present: copies the
            // composited image into a source image, hands the pair to the
            // host, presents the generated frames, then the real one.
            // waitSemaphore is the layer's overlay-finished signal for this
            // image. Returns the result of the last real QueuePresentKHR.
            VkResult present(LogicalDevice* logicalDevice,
                             LogicalSwapchain* swapchain,
                             uint32_t index,
                             VkQueue queue,
                             VkSemaphore waitSemaphore,
                             VkImage sourceImage,
                             VkImageLayout sourceLayout,
                             bool sourceReady,
                             bool replayOverlay,
                             const VkPresentInfoKHR* originalPresentInfo);

            // The recorded command buffer that copies the app's composited
            // image into its private pre-overlay image. It must be submitted
            // after the effect pass and before the overlay draw.
            VkCommandBuffer copySourceCommand(uint32_t index) const
            {
                return m_ready && index < m_frameCopyCmdbufs.size()
                    ? m_frameCopyCmdbufs[index] : VK_NULL_HANDLE;
            }

            uint32_t multiplier() const { return m_multiplier; }

            VkImage frameSourceImage(uint32_t index) const
            {
                return index < m_frameSourceImages.size() ? m_frameSourceImages[index] : VK_NULL_HANDLE;
            }

        private:
            struct InnerSyncPass
            {
                VkCommandBuffer copyCmdbuf = VK_NULL_HANDLE;
                VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
                VkSemaphore copySemaphore = VK_NULL_HANDLE;
            };
            struct SyncPass
            {
                VkCommandBuffer uploadCmdbuf = VK_NULL_HANDLE;
                // the last inner copy's semaphore; the real frame waits on it
                VkSemaphore presentSemaphore = VK_NULL_HANDLE;
                // the real frame stays in the application's image and is
                // presented after the generated ones; this empty submit turns
                // the upload's timeline value into presentSemaphore
                VkCommandBuffer realCopyCmdbuf = VK_NULL_HANDLE;
                std::vector<InnerSyncPass> innerSyncPasses;
            };

            void destroy();
            bool createEngineObjects(LogicalDevice* logicalDevice, LogicalSwapchain* swapchain, const FgProfile& profile);

            LogicalDevice* m_logicalDevice = nullptr;
            uint32_t m_multiplier = 2;
            std::vector<VkImage> m_frameSourceImages;
            std::vector<VkDeviceMemory> m_frameSourceMemories;
            std::vector<VkCommandBuffer> m_frameCopyCmdbufs;
            VkExtent2D m_extent = {0, 0};
            // FIFO: the presentation engine shows one queued image per
            // refresh, so it paces the generated frames by itself
            bool m_fifo = false;
            // exchange image the next real frame is copied into; the host
            // swaps the pair on every trigger
            uint32_t m_sourceIndex = 0;
            // the first real frame fills both sources so the host's first
            // generated batch is not interpolated against the blank one
            bool m_firstSourceUpload = true;
            bool m_ready = false;
            uint64_t m_syncValue = 0;
            // frames to pass through after the swapchain showed no headroom,
            // so insertion cannot steal the application's own frames
            uint32_t m_probeCooldown = 0;
            // measure the application's own cadence before inserting, and stop
            // when inserting makes that cadence collapse
            std::chrono::steady_clock::time_point m_lastPresentTime{};
            uint64_t m_passIntervalUs = 0;
            uint32_t m_warmupFrames = 20;
            uint32_t m_slowStreak = 0;
            bool m_lastInserted = false;

            // caller-owned exchange images, exported to the host
            VkImage m_sourceImages[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
            VkDeviceMemory m_sourceMemories[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
            std::vector<VkImage> m_destinationImages;
            std::vector<VkDeviceMemory> m_destinationMemories;
            VkSemaphore m_syncSemaphore = VK_NULL_HANDLE;
            VkCommandPool m_commandPool = VK_NULL_HANDLE;
            std::vector<SyncPass> m_syncPasses;

            std::unique_ptr<EngineInstance> m_engineInstance;
            std::unique_ptr<EngineContext> m_engineContext;
        };
    } // namespace FrameGen
} // namespace VKIntox
