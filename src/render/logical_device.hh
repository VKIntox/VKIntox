#ifndef LOGICAL_DEVICE_HPP_INCLUDED
#define LOGICAL_DEVICE_HPP_INCLUDED
#include <atomic>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

#include "vulkan_include.hh"
#include "vkdispatch.hh"
#include "depth_state.hh"
#include "depth_copy_state.hh"
#include <mutex>

namespace VKIntox
{
    struct LogicalSwapchain;  // Forward declaration

    // Global lock guarding all layer state (defined in vkintox.cpp). Shared with
    // the overlay so it can read LogicalDevice state for the Advanced tab.
    extern std::mutex globalLock;

    struct DepthSnapshotTarget
    {
        VkSwapchainKHR  swapchain = VK_NULL_HANDLE;
        LogicalSwapchain* logicalSwapchain = nullptr;
        uint32_t        imageIndex = 0;
    };

    // Persistent depth storage image — survives app depth image destruction.
    // Used by the v3 deferred copy mechanism.
    struct PersistentDepthStorage
    {
        VkImage        image = VK_NULL_HANDLE;
        VkImageView    view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkExtent3D     extent = {0, 0, 1};
        VkFormat       format = VK_FORMAT_UNDEFINED;
        bool           valid = false;  // True when depth has been blitted this frame
    };

    struct DepthImageMetadata
    {
        VkImageUsageFlags     usage = 0;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        VkImageTiling         tiling = VK_IMAGE_TILING_OPTIMAL;
    };

    struct OverlayPersistentState;  // Forward declaration
    class ImGuiOverlay;  // Forward declaration

    struct LogicalDevice
    {
        struct DepthScopeTrackingState
        {
            bool     inRenderScope = false;
            DepthState depthState;
            DepthSnapshotTarget snapshotTarget;
            uint32_t  drawCount = 0;
            // The depth attachment's finalLayout from the render pass description.
            // This is the EXACT layout the depth image will be in after the render
            // pass ends — ground truth, no guessing. For dynamic rendering, this
            // comes from VkRenderingAttachmentInfo::imageLayout.
            VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        };

        struct DepthCandidateTrackingState
        {
            bool     valid = false;
            DepthState depthState;
            bool     hasPresentableSnapshotTarget = false;
            bool     extentMatchesPresentableTarget = false;
            uint32_t  drawCount = 0;
        };

        DeviceDispatch           vkd;
        InstanceDispatch         vki;
        VkDevice                 device;
        VkPhysicalDevice         physicalDevice;
        VkInstance               instance;
        VkQueue                  queue;
        uint32_t                 queueFamilyIndex;
        VkCommandPool            commandPool;
        // Shared driver pipeline cache. Rebuilt effects reuse compiled pipelines
        // instead of making the driver recompile them on every chain reload.
        VkPipelineCache          pipelineCache = VK_NULL_HANDLE;
        bool                     supportsMutableFormat;
        bool                     isNvidiaGpu;
        // Real identity of the device the game is actually running on. The
        // sysfs DRM scan can't tell hybrid GPUs apart, so the UI reports these
        // instead of guessing a vendor from card enumeration order.
        std::string              gpuName;
        std::string              gpuDriverInfo;
        std::string              gpuPciSlot;  // "0000:01:00.0", empty if none
        uint32_t                 gpuApiVersion = 0;
        uint32_t                 gpuVendorId = 0;
        bool                     gpuCrashDiagnosticsEnabled = false;
        bool                     supportsNvDiagnosticCheckpoints = false;
        bool                     supportsNvDiagnosticsConfig = false;
        bool                     supportsDeviceFaultExt = false;
        // Queried once at device init via VK_KHR_depth_resolve_mode / core 1.2.
        // OR of VK_RESOLVE_MODE_SAMPLE_ZERO_BIT / AVERAGE_BIT / MIN/MAX. Effects
        // the MSAA depth resolve path and the Advanced UI mode selector.
        VkResolveModeFlags       supportedDepthResolveModes = 0;
        // Optional manual override of the depth buffer promotion. When set to
        // a known tracked image view, the layer pins to it instead of using the
        // best-candidate heuristic. Set from the Advanced UI (empty = auto).
        VkImageView              pinnedDepthImageView = VK_NULL_HANDLE;
        std::vector<VkImage>     depthImages;
        std::vector<VkFormat>    depthFormats;
        std::vector<VkImageView> depthImageViews;
        std::unordered_map<VkImage, VkExtent3D> depthImageExtents;
        std::unordered_map<VkImage, DepthImageMetadata> depthImageMetadata;
        std::unordered_map<VkImageView, DepthState> depthViewStates;
        std::unordered_map<VkImageView, DepthSnapshotTarget> snapshotTargetViewStates;
        std::unordered_map<VkFramebuffer, DepthState> framebufferDepthStates;
        std::unordered_map<VkFramebuffer, DepthSnapshotTarget> framebufferSnapshotTargets;
        std::unordered_map<VkCommandBuffer, DepthScopeTrackingState> commandBufferDepthStates;
        std::unordered_map<VkCommandBuffer, DepthScopeTrackingState> pendingTransferLinkedDepthScopes;
        std::unordered_map<VkCommandBuffer, uint32_t> commandBufferRecordedDrawCounts;
        DepthCandidateTrackingState  bestDepthCandidate;
        DepthState               activeDepthState;

        // When a depth candidate is promoted during CmdEndRenderPass, we MUST NOT
        // free/reallocate effect command buffers immediately — the GPU may still
        // be executing the old ones submitted by the previous QueuePresentKHR.
        // Instead, set this flag and handle the reallocation in QueuePresentKHR
        // where we can safely QueueWaitIdle first.
        bool depthReallocPending = false;

        // Persistent depth storage — layer-internal depth image that outlives
        // app depth image destruction. NOT stored in the parallel
        // depthImages/depthFormats/depthImageViews vectors.
        PersistentDepthStorage depthCaptureStorage;
        bool persistentStorageTracked = false;

        // v3 deferred copy: depth that needs blitting at QueueSubmit time.
        // Owns its own mutex; see DepthCopyState for the lock-order contract.
        DepthCopyState depthCopy;

        // A chain reload requested by the UI, deferred until every swapchain's
        // in-flight effect submissions have completed. Replaces the global
        // QueueWaitIdle that used to stall the present thread on every toggle.
        bool pendingChainReload = false;
        std::vector<std::string> pendingChainEffects;

        // Persistent overlay state that survives swapchain recreation
        std::unique_ptr<OverlayPersistentState> overlayPersistentState;

        // ImGui overlay - lives at device level to survive swapchain recreation
        std::unique_ptr<ImGuiOverlay> imguiOverlay;

        // Set by panicLayer() when the layer hits a fatal error. While true,
        // interceptors take the pass-through fast path: the swapchain keeps
        // presenting, the toast stays visible, but no effect work is done.
        // Cleared only by DestroyDevice — the user must restart the game to
        // re-enable effects after a panic.
        std::atomic<bool> softDisabled{false};
    };
} // namespace VKIntox

#endif // LOGICAL_DEVICE_HPP_INCLUDED