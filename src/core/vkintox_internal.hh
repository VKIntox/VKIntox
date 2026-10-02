#ifndef VKINTOX_INTERNAL_HPP_INCLUDED
#define VKINTOX_INTERNAL_HPP_INCLUDED

// Shared between vkintox.cc and vkintox_depth.cc. Not a public header: this is
// the seam between the layer's Vulkan interceptors and its depth subsystem, and
// nothing outside src/core should include it.

#include "vulkan_include.hh"
#include "logical_device.hh"
#include "logical_swapchain.hh"
#include "settings_manager.hh"

#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <cstdint>

namespace VKIntox
{
    // Defined in vkintox.cc. The depth subsystem walks the swapchain map to find
    // the swapchains it has to record snapshots into, and takes the same lock the
    // interceptors take, so both need the one instance rather than two.
    extern std::unordered_map<VkSwapchainKHR, std::shared_ptr<LogicalSwapchain>> swapchainMap;
    extern std::mutex globalLock;

    // Moved here for the same reason as the map above: the depth subsystem is
    // what arms and polls these retries, so it needs the one instance rather
    // than a copy.
    struct DepthRetryState
    {
        bool disabled = false;
        bool retryPending = false;
        std::chrono::steady_clock::time_point retryAt{};
        VkExtent2D blockedExtent{0, 0};
    };
    extern std::unordered_map<LogicalDevice*, DepthRetryState> depthRetryStates;
    using scoped_lock = std::lock_guard<std::mutex>;

    // Moved here from vkintox.cc because the depth subsystem is what schedules
    // the retries. constexpr implies inline, so exactly one definition exists
    // no matter how many translation units include this.
    inline constexpr auto DEPTH_RETRY_DELAY = std::chrono::seconds(5);
    inline constexpr auto DEPTH_REBUILD_POLL_DELAY = std::chrono::milliseconds(100);

    // ---- shared with vkintox.cc, called from the depth subsystem ----
    std::string formatHexU64(uint64_t value);
    void reportDeviceLostDiagnostics(LogicalDevice* logicalDevice, VkQueue queue, const char* context, VkResult result);
    void panicLayer(LogicalDevice* logicalDevice, const std::string& reason);

    // ---- depth subsystem, defined in vkintox_depth.cc ----
    //
    // Generated from the definitions rather than typed, so overloads and
    // templates both survive. Default arguments live here and not on the
    // definitions; a default may be spelled in only one of the two.

    bool handleKeyPress(uint32_t keySymbol, bool& wasPressed);
    bool hasDepthState(const DepthState& state);
    bool validateDepthStateForResolve(LogicalDevice* logicalDevice, const DepthState& depth);

    bool isDepthStencilAttachmentFormat(VkFormat format);
    bool forceDepthAttachmentStoreOp(VkAttachmentDescription& attachment);
    bool forceDepthAttachmentStoreOp(VkAttachmentDescription2& attachment);
    bool forceDepthAttachmentStoreOp(VkRenderingAttachmentInfo& attachment, bool hasStencilAspect);
    bool sameDepthState(const DepthState& a, const DepthState& b);
    bool hasPresentableSnapshotTarget(const DepthSnapshotTarget& target);
    bool selectDepthCandidateForSwapchainLocked(LogicalDevice* logicalDevice,
                                                        const LogicalSwapchain* logicalSwapchain,
                                                        DepthState& candidate);
// Definition, not just a declaration. This is a template instantiated in
    // vkintox.cc with lambdas written there, and a template defined in a different
    // translation unit cannot be instantiated with a type that has no linkage,
    // so the body has to travel with the declaration.
    template <typename Predicate>
    void clearTrackedDepthScopesLocked(LogicalDevice* logicalDevice, Predicate predicate)
    {
        for (auto it = logicalDevice->commandBufferDepthStates.begin(); it != logicalDevice->commandBufferDepthStates.end();)
        {
            if (predicate(it->second.depthState))
                it = logicalDevice->commandBufferDepthStates.erase(it);
            else
                ++it;
        }

        for (auto it = logicalDevice->pendingTransferLinkedDepthScopes.begin(); it != logicalDevice->pendingTransferLinkedDepthScopes.end();)
        {
            if (predicate(it->second.depthState))
                it = logicalDevice->pendingTransferLinkedDepthScopes.erase(it);
            else
                ++it;
        }

        if (logicalDevice->bestDepthCandidate.valid && predicate(logicalDevice->bestDepthCandidate.depthState))
            logicalDevice->bestDepthCandidate = {};
    }
    DepthState selectDepthStateFromRenderPassBegin(LogicalDevice* logicalDevice, const VkRenderPassBeginInfo* pRenderPassBegin);
    DepthState selectDepthStateFromRenderingInfo(LogicalDevice* logicalDevice, const VkRenderingInfo* pRenderingInfo);
    DepthSnapshotTarget selectDepthSnapshotTargetFromImage(LogicalDevice* logicalDevice, VkImage image);
    DepthSnapshotTarget selectDepthSnapshotTargetFromImageViews(LogicalDevice* logicalDevice,
                                                                        const VkImageView* imageViews,
                                                                        uint32_t imageViewCount);
    DepthSnapshotTarget selectDepthSnapshotTargetFromRenderPassBegin(LogicalDevice* logicalDevice,
                                                                            const VkRenderPassBeginInfo* pRenderPassBegin);
    DepthSnapshotTarget selectDepthSnapshotTargetFromRenderingInfo(LogicalDevice* logicalDevice,
                                                                           const VkRenderingInfo* pRenderingInfo);
    void beginTrackedDepthScope(LogicalDevice* logicalDevice,
                                VkCommandBuffer commandBuffer,
                                const DepthState& depthState,
                                DepthSnapshotTarget snapshotTarget,
                                VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED);
    void countTrackedDepthDraw(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer, uint32_t drawCount = 1);
    void accumulateExecutedCommandBufferDraws(LogicalDevice* logicalDevice,
                                              VkCommandBuffer primaryCommandBuffer,
                                              const VkCommandBuffer* pCommandBuffers,
                                              uint32_t commandBufferCount);
    void updateDeviceDepthStateLocked(LogicalDevice* logicalDevice, const DepthState& depth, const char* reason);
    void recordDepthResolveSnapshotForCommandBuffer(LogicalDevice* logicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    const DepthState& depthState,
                                                    const DepthSnapshotTarget* snapshotTarget);
    void recordDepthResolveSnapshotForAllSwapchains(LogicalDevice* logicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    const DepthState& depthState);

    bool endTrackedDepthScope(LogicalDevice* logicalDevice,
                              VkCommandBuffer commandBuffer,
                              const char* reason,
                              DepthState* pPromotedDepthState,
                              DepthSnapshotTarget* snapshotTarget = nullptr,
                              VkImageLayout* pDepthFinalLayout = nullptr);
    void tryActivatePendingTransferLinkedDepthScope(LogicalDevice* logicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    VkImage destinationImage,
                                                    const char* reason);
    void recordDepthResolveSnapshotForCommandBuffer(LogicalDevice* logicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    const DepthState& depthState,
                                                    const DepthSnapshotTarget* snapshotTarget);
    void recordDepthResolveSnapshotForAllSwapchains(LogicalDevice* logicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    const DepthState& depthState);
    VkImageView getOrCreateTrackedDepthSampleViewLocked(LogicalDevice* logicalDevice, VkImage image, VkFormat format);
    void updateDeviceDepthStateLocked(LogicalDevice* logicalDevice, const DepthState& depth, const char* reason);
    void reallocateCommandBuffers(LogicalDevice* logicalDevice, LogicalSwapchain* logicalSwapchain, const DepthState& depth);

    void destroyDepthResolveResources(LogicalSwapchain* logicalSwapchain);
    bool validateDepthStateForResolve(LogicalDevice* logicalDevice, const DepthState& depth);
    void destroyPersistentDepthStorage(LogicalDevice* logicalDevice);
    void ensurePersistentDepthStorage(LogicalDevice* logicalDevice, VkFormat format, const VkExtent3D& extent);
    void ensureDepthResolveResources(LogicalSwapchain* logicalSwapchain, const DepthState& depth);
    bool depthMatchesSwapchainExtent(const DepthState& depth, const LogicalSwapchain* sc);
    void armDepthRetryLocked(LogicalDevice* logicalDevice, const LogicalSwapchain* sc, const char* reason);
    bool depthRetryDueLocked(LogicalDevice* logicalDevice);
    void scheduleDepthRetryLocked(LogicalDevice* logicalDevice, bool fastPoll = false);
    bool depthRebuildFencesReady(LogicalDevice* logicalDevice, const LogicalSwapchain* sc);
    DepthState getDepthState(LogicalDevice* logicalDevice);
    void updateDeviceDepthStateLocked(LogicalDevice* logicalDevice, const DepthState& depth, const char* reason);
    void reallocateCommandBuffers(
        LogicalDevice* logicalDevice,
        LogicalSwapchain* logicalSwapchain,
        const DepthState& depth);

} // namespace VKIntox

#endif // VKINTOX_INTERNAL_HPP_INCLUDED
