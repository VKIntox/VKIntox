#include "vkintox_internal.hh"

#include "image_view.hh"
#include "image.hh"
#include "command_buffer.hh"
#include "vk_handle.hh"
#include "util.hh"
#include "format.hh"
#include "keyboard_input.hh"
#include "renderpass.hh"
#include "framebuffer.hh"
#include "sampler.hh"
#include "descriptor_set.hh"
#include "graphics_pipeline.hh"
#include "buffer.hh"
#include "shader_sources.hh"
#include "shader.hh"

#include <algorithm>
#include <cstring>
#include <vector>

namespace VKIntox
{

// Extracted from vkintox.cc. Everything below is the depth subsystem: tracking
// the application's depth attachment as commands are recorded, choosing a
// presentable snapshot target for it, resolving it, and retrying when the
// attachment is not yet usable.
//
// The extraction is deliberately mechanical. No behaviour changed, no logic was
// reorganised, and the only edits are that the functions the interceptors call
// lost their `static` so they can be declared in vkintox_internal.hh, and the
// three names the block shares with vkintox.cc became extern.

    bool handleKeyPress(uint32_t keySymbol, bool& wasPressed)
    {
        if (isKeyPressed(keySymbol))
        {
            if (!wasPressed)
            {
                wasPressed = true;
                return true;
            }
        }
        else
        {
            wasPressed = false;
        }
        return false;
    }

    bool hasDepthState(const DepthState& state)
    {
        return state.image != VK_NULL_HANDLE && state.imageView != VK_NULL_HANDLE && state.format != VK_FORMAT_UNDEFINED;
    }

    // Forward declaration — defined later, after DepthImageMetadata helpers.
    // Used to gate depth-resolve recording against destroyed/invalid depth images.
    bool validateDepthStateForResolve(LogicalDevice* logicalDevice, const DepthState& depth);

    bool isDepthStencilAttachmentFormat(VkFormat format)
    {
        return isDepthFormat(format) || isStencilFormat(format);
    }

    static bool needsStoredAttachmentPreservation(VkAttachmentStoreOp storeOp)
    {
        return storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE
#ifdef VK_ATTACHMENT_STORE_OP_NONE
               || storeOp == VK_ATTACHMENT_STORE_OP_NONE
#endif
            ;
    }

    bool forceDepthAttachmentStoreOp(VkAttachmentDescription& attachment)
    {
        if (!isDepthStencilAttachmentFormat(attachment.format))
            return false;

        bool changed = false;
        if (needsStoredAttachmentPreservation(attachment.storeOp))
        {
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed = true;
        }
        if (isStencilFormat(attachment.format) && needsStoredAttachmentPreservation(attachment.stencilStoreOp))
        {
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed = true;
        }
        return changed;
    }

    bool forceDepthAttachmentStoreOp(VkAttachmentDescription2& attachment)
    {
        if (!isDepthStencilAttachmentFormat(attachment.format))
            return false;

        bool changed = false;
        if (needsStoredAttachmentPreservation(attachment.storeOp))
        {
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed = true;
        }
        if (isStencilFormat(attachment.format) && needsStoredAttachmentPreservation(attachment.stencilStoreOp))
        {
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed = true;
        }
        return changed;
    }

    bool forceDepthAttachmentStoreOp(VkRenderingAttachmentInfo& attachment, bool hasStencilAspect)
    {
        bool changed = false;
        if (attachment.imageView != VK_NULL_HANDLE && needsStoredAttachmentPreservation(attachment.storeOp))
        {
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed = true;
        }
        // NOTE: VkRenderingAttachmentInfo has no stencilStoreOp field.
        // Stencil preservation for dynamic rendering requires the application
        // to set it correctly on pStencilAttachment in VkRenderingInfo,
        // which is outside this per-attachment helper's scope.
        // The depth storeOp is forced above; stencil is a known limitation.
        (void)hasStencilAspect;
        return changed;
    }

    bool sameDepthState(const DepthState& a, const DepthState& b)
    {
        return a.image == b.image && a.imageView == b.imageView && a.format == b.format
            && a.extent.width == b.extent.width && a.extent.height == b.extent.height && a.extent.depth == b.extent.depth
            && a.observedLayout == b.observedLayout;
    }

    bool hasPresentableSnapshotTarget(const DepthSnapshotTarget& target)
    {
        return target.swapchain != VK_NULL_HANDLE
            && target.logicalSwapchain != nullptr
            && target.imageIndex < target.logicalSwapchain->imageCount;
    }

    static bool matchesPresentableSnapshotTargetExtent(const DepthState& depth, const DepthSnapshotTarget& target)
    {
        return hasPresentableSnapshotTarget(target)
            && depth.extent.width == target.logicalSwapchain->imageExtent.width
            && depth.extent.height == target.logicalSwapchain->imageExtent.height;
    }

    // A depth candidate must never be one of the application's real swapchain
    // images or one of VKIntox's fake colour images.  Size alone is not enough
    // to distinguish a 1920x1080 depth buffer from a 1920x1080 backbuffer.
    static bool isSwapchainImage(LogicalDevice* logicalDevice, VkImage image)
    {
        if (!logicalDevice || image == VK_NULL_HANDLE)
            return false;

        for (auto& [_, logicalSwapchain] : swapchainMap)
        {
            if (!logicalSwapchain || logicalSwapchain->logicalDevice != logicalDevice)
                continue;

            if (std::find(logicalSwapchain->images.begin(),
                          logicalSwapchain->images.end(), image) != logicalSwapchain->images.end())
                return true;

            if (std::find(logicalSwapchain->fakeImages.begin(),
                          logicalSwapchain->fakeImages.end(), image) != logicalSwapchain->fakeImages.end())
                return true;
        }

        return false;
    }

    static bool matchesAnySwapchainExtent(LogicalDevice* logicalDevice, const DepthState& depth)
    {
        if (!hasDepthState(depth))
            return false;

        for (auto& [_, logicalSwapchain] : swapchainMap)
        {
            if (!logicalSwapchain || logicalSwapchain->logicalDevice != logicalDevice)
                continue;

            if (depth.extent.width == logicalSwapchain->imageExtent.width
                && depth.extent.height == logicalSwapchain->imageExtent.height)
                return true;
        }

        return false;
    }

    static bool isQualifiedDepthCandidate(LogicalDevice* logicalDevice,
                                          const LogicalDevice::DepthScopeTrackingState& scopeState)
    {
        const DepthState& depth = scopeState.depthState;
        if (!logicalDevice || !hasDepthState(depth) || scopeState.drawCount == 0)
            return false;

        // Never accept a colour/backbuffer image as depth, even when its
        // dimensions happen to be identical to the swapchain extent.
        if (isSwapchainImage(logicalDevice, depth.image))
        {
            Logger::debug("rejecting depth candidate: image belongs to a swapchain/fake colour image (image="
                          + convertToString(depth.image) + ")");
            return false;
        }

        // The state must actually describe a depth/stencil format.
        if (!isDepthStencilAttachmentFormat(depth.format))
        {
            Logger::debug("rejecting depth candidate: non-depth format (image="
                          + convertToString(depth.image) + " format="
                          + convertToString(depth.format) + ")");
            return false;
        }

        // Exact swapchain extent is required.  Matching the window size is the
        // only automatic qualification rule; shadow maps and other depth
        // attachments remain candidates for bookkeeping but are never made
        // active depth state.
        return matchesAnySwapchainExtent(logicalDevice, depth);
    }

    static bool isQualifiedDepthStateForSwapchain(LogicalDevice* logicalDevice,
                                                   const DepthState& depth,
                                                   const LogicalSwapchain* logicalSwapchain)
    {
        if (!logicalDevice || !logicalSwapchain || !hasDepthState(depth))
            return false;

        if (isSwapchainImage(logicalDevice, depth.image))
            return false;

        if (!isDepthStencilAttachmentFormat(depth.format))
            return false;

        if (depth.extent.width != logicalSwapchain->imageExtent.width
            || depth.extent.height != logicalSwapchain->imageExtent.height)
            return false;

        return validateDepthStateForResolve(logicalDevice, depth);
    }

    // Re-discover depth after the renderer has settled.  The old startup
    // workaround reloaded the entire config/effect stack (equivalent to F10).
    // That worked because it happened late enough for Roblox's real depth
    // image to exist, but it also rebuilt unrelated state.  This keeps the
    // useful part: throw away the startup depth choice and select a currently
    // tracked, swapchain-sized depth image.
    bool selectDepthCandidateForSwapchainLocked(LogicalDevice* logicalDevice,
                                                        const LogicalSwapchain* logicalSwapchain,
                                                        DepthState& candidate)
    {
        if (!logicalDevice || !logicalSwapchain)
            return false;

        // Prefer the depth candidate observed from an actual render scope.
        // Unlike a raw image-view scan, this preserves the semantic signal
        // gathered while recording the game's main render pass.
        if (logicalDevice->bestDepthCandidate.valid
            && isQualifiedDepthStateForSwapchain(logicalDevice,
                                                  logicalDevice->bestDepthCandidate.depthState,
                                                  logicalSwapchain))
        {
            candidate = logicalDevice->bestDepthCandidate.depthState;
            return true;
        }

        // If the candidate cache is stale or was populated before the current
        // swapchain existed, inspect the live depth views directly.
        for (const auto& [_, depth] : logicalDevice->depthViewStates)
        {
            if (!isQualifiedDepthStateForSwapchain(logicalDevice, depth, logicalSwapchain))
                continue;

            candidate = depth;
            return true;
        }

        return false;
    }

    DepthState selectDepthStateFromRenderPassBegin(LogicalDevice* logicalDevice, const VkRenderPassBeginInfo* pRenderPassBegin)
    {
        DepthState depth;
        if (!pRenderPassBegin)
            return depth;

        bool found = false;
        for (const VkBaseInStructure* next = reinterpret_cast<const VkBaseInStructure*>(pRenderPassBegin->pNext); next; next = next->pNext)
        {
            if (next->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)
                continue;

            const auto* attachmentBeginInfo = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(next);
            for (uint32_t i = 0; i < attachmentBeginInfo->attachmentCount; i++)
            {
                auto viewIt = logicalDevice->depthViewStates.find(attachmentBeginInfo->pAttachments[i]);
                if (viewIt != logicalDevice->depthViewStates.end())
                {
                    depth = viewIt->second;
                    found = true;
                    break;
                }
            }

            if (found)
                break;
        }

        if (!found)
        {
            auto fbIt = logicalDevice->framebufferDepthStates.find(pRenderPassBegin->framebuffer);
            if (fbIt != logicalDevice->framebufferDepthStates.end())
                depth = fbIt->second;
        }

        return depth;
    }

    DepthState selectDepthStateFromRenderingInfo(LogicalDevice* logicalDevice, const VkRenderingInfo* pRenderingInfo)
    {
        DepthState depth;
        if (!pRenderingInfo || !pRenderingInfo->pDepthAttachment || pRenderingInfo->pDepthAttachment->imageView == VK_NULL_HANDLE)
            return depth;

        auto it = logicalDevice->depthViewStates.find(pRenderingInfo->pDepthAttachment->imageView);
        if (it != logicalDevice->depthViewStates.end())
            depth = it->second;

        depth.observedLayout = pRenderingInfo->pDepthAttachment->imageLayout;

        // When the app supplies its own resolve target (e.g. Roblox MSAA), the
        // resolve image is 1-sample and the actual depth data effects should
        // use. Track the resolve view instead of the MSAA source.
        //
        // CRITICAL: Only follow the resolve target if resolveMode != NONE.
        // VK_RESOLVE_MODE_NONE means the app explicitly does NOT want depth
        // resolved into the resolve target — the resolve target stays empty.
        // Following it would cause the layer to copy an empty image, producing
        // the "normal map shows but depth doesn't" symptom in Roblox (which
        // uses NONE on some configurations to skip depth resolve for perf).
        // In that case, fall through and let the layer do its OWN resolve via
        // the MSAA path (depthResolveIsMsaa).
        if (pRenderingInfo->pDepthAttachment->resolveImageView != VK_NULL_HANDLE
            && pRenderingInfo->pDepthAttachment->resolveMode != VK_RESOLVE_MODE_NONE)
        {
            auto resolveIt = logicalDevice->depthViewStates.find(pRenderingInfo->pDepthAttachment->resolveImageView);
            if (resolveIt != logicalDevice->depthViewStates.end())
            {
                depth = resolveIt->second;
                // The resolve image layout follows the attachment layout.
                depth.observedLayout = pRenderingInfo->pDepthAttachment->resolveImageLayout != VK_IMAGE_LAYOUT_UNDEFINED
                    ? pRenderingInfo->pDepthAttachment->resolveImageLayout
                    : pRenderingInfo->pDepthAttachment->imageLayout;
                Logger::debug("depth dynamic rendering: following resolve image view instead of MSAA source (resolveMode="
                              + std::to_string(static_cast<uint32_t>(pRenderingInfo->pDepthAttachment->resolveMode)) + ")");
            }
        }
        else if (pRenderingInfo->pDepthAttachment->resolveImageView != VK_NULL_HANDLE
                 && pRenderingInfo->pDepthAttachment->resolveMode == VK_RESOLVE_MODE_NONE)
        {
            Logger::debug("depth dynamic rendering: resolve target present but resolveMode=NONE; "
                          "keeping MSAA source for layer-side resolve");
        }

        return depth;
    }

    static bool findDepthSnapshotTargetForImageView(LogicalDevice* logicalDevice,
                                                    VkImageView imageView,
                                                    DepthSnapshotTarget& target)
    {
        if (imageView == VK_NULL_HANDLE)
            return false;

        auto trackedViewIt = logicalDevice->snapshotTargetViewStates.find(imageView);
        if (trackedViewIt != logicalDevice->snapshotTargetViewStates.end())
        {
            target = trackedViewIt->second;
            return target.swapchain != VK_NULL_HANDLE;
        }

        for (auto& [swapchainHandle, logicalSwapchain] : swapchainMap)
        {
            if (!logicalSwapchain || logicalSwapchain->logicalDevice != logicalDevice)
                continue;

            auto imageIt = std::find(logicalSwapchain->imageViews.begin(), logicalSwapchain->imageViews.end(), imageView);
            if (imageIt == logicalSwapchain->imageViews.end())
                continue;

            target.swapchain = swapchainHandle;
            target.logicalSwapchain = logicalSwapchain.get();
            target.imageIndex = static_cast<uint32_t>(imageIt - logicalSwapchain->imageViews.begin());
            return true;
        }

        return false;
    }

    DepthSnapshotTarget selectDepthSnapshotTargetFromImage(LogicalDevice* logicalDevice, VkImage image)
    {
        DepthSnapshotTarget target;
        if (image == VK_NULL_HANDLE)
            return target;

        for (auto& [swapchainHandle, logicalSwapchain] : swapchainMap)
        {
            if (!logicalSwapchain || logicalSwapchain->logicalDevice != logicalDevice)
                continue;

            auto realImageIt = std::find(logicalSwapchain->images.begin(), logicalSwapchain->images.end(), image);
            if (realImageIt != logicalSwapchain->images.end())
            {
                target.swapchain = swapchainHandle;
                target.logicalSwapchain = logicalSwapchain.get();
                target.imageIndex = static_cast<uint32_t>(realImageIt - logicalSwapchain->images.begin());
                return target;
            }

            const size_t presentableFakeCount = std::min<size_t>(logicalSwapchain->fakeImages.size(), logicalSwapchain->imageCount);
            auto fakeImageIt = std::find(logicalSwapchain->fakeImages.begin(),
                                         logicalSwapchain->fakeImages.begin() + presentableFakeCount,
                                         image);
            if (fakeImageIt != logicalSwapchain->fakeImages.begin() + presentableFakeCount)
            {
                target.swapchain = swapchainHandle;
                target.logicalSwapchain = logicalSwapchain.get();
                target.imageIndex = static_cast<uint32_t>(fakeImageIt - logicalSwapchain->fakeImages.begin());
                return target;
            }
        }

        return target;
    }

    DepthSnapshotTarget selectDepthSnapshotTargetFromImageViews(LogicalDevice* logicalDevice,
                                                                        const VkImageView* imageViews,
                                                                        uint32_t imageViewCount)
    {
        DepthSnapshotTarget target;
        if (!imageViews)
            return target;

        for (uint32_t i = 0; i < imageViewCount; ++i)
        {
            if (findDepthSnapshotTargetForImageView(logicalDevice, imageViews[i], target))
                return target;
        }

        return target;
    }

    DepthSnapshotTarget selectDepthSnapshotTargetFromRenderPassBegin(LogicalDevice* logicalDevice,
                                                                            const VkRenderPassBeginInfo* pRenderPassBegin)
    {
        DepthSnapshotTarget target;
        if (!pRenderPassBegin)
            return target;

        for (const VkBaseInStructure* next = reinterpret_cast<const VkBaseInStructure*>(pRenderPassBegin->pNext); next; next = next->pNext)
        {
            if (next->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)
                continue;

            const auto* attachmentBeginInfo = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(next);
            target = selectDepthSnapshotTargetFromImageViews(logicalDevice, attachmentBeginInfo->pAttachments, attachmentBeginInfo->attachmentCount);
            if (target.swapchain != VK_NULL_HANDLE)
                return target;
        }

        auto fbTargetIt = logicalDevice->framebufferSnapshotTargets.find(pRenderPassBegin->framebuffer);
        if (fbTargetIt != logicalDevice->framebufferSnapshotTargets.end())
            target = fbTargetIt->second;

        return target;
    }

    DepthSnapshotTarget selectDepthSnapshotTargetFromRenderingInfo(LogicalDevice* logicalDevice,
                                                                           const VkRenderingInfo* pRenderingInfo)
    {
        DepthSnapshotTarget target;
        if (!pRenderingInfo)
            return target;

        if (pRenderingInfo->pColorAttachments)
        {
            for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount; ++i)
            {
                if (findDepthSnapshotTargetForImageView(logicalDevice, pRenderingInfo->pColorAttachments[i].imageView, target))
                    return target;
            }
        }

        if (pRenderingInfo->pDepthAttachment && findDepthSnapshotTargetForImageView(logicalDevice, pRenderingInfo->pDepthAttachment->imageView, target))
            return target;

        if (pRenderingInfo->pStencilAttachment && findDepthSnapshotTargetForImageView(logicalDevice, pRenderingInfo->pStencilAttachment->imageView, target))
            return target;

        return target;
    }

    void beginTrackedDepthScope(LogicalDevice* logicalDevice,
                                VkCommandBuffer commandBuffer,
                                const DepthState& depthState,
                                DepthSnapshotTarget snapshotTarget,
                                VkImageLayout depthFinalLayout)
    {
        logicalDevice->pendingTransferLinkedDepthScopes.erase(commandBuffer);
        auto& scopeState = logicalDevice->commandBufferDepthStates[commandBuffer];
        scopeState.inRenderScope = true;
        scopeState.depthState = depthState;
        scopeState.snapshotTarget = snapshotTarget;
        scopeState.drawCount = 0;
        scopeState.depthFinalLayout = depthFinalLayout;
    }

    void countTrackedDepthDraw(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer, uint32_t drawCount)
    {
        // Single hash lookup via try_emplace — returns an iterator to the
        // existing entry (with the inserted flag set to false) or to a freshly
        // inserted entry (zero-initialized). The previous code did operator[]
        // (which inserts) followed by a separate find on
        // commandBufferDepthStates — two hash ops per draw call.
        auto emplaceResult = logicalDevice->commandBufferRecordedDrawCounts.try_emplace(commandBuffer, 0);
        emplaceResult.first->second += drawCount;

        auto scopeIt = logicalDevice->commandBufferDepthStates.find(commandBuffer);
        if (scopeIt == logicalDevice->commandBufferDepthStates.end() || !scopeIt->second.inRenderScope)
            return;

        scopeIt->second.drawCount += drawCount;
    }

    void accumulateExecutedCommandBufferDraws(LogicalDevice* logicalDevice,
                                              VkCommandBuffer primaryCommandBuffer,
                                              const VkCommandBuffer* pCommandBuffers,
                                              uint32_t commandBufferCount)
    {
        auto scopeIt = logicalDevice->commandBufferDepthStates.find(primaryCommandBuffer);
        if (scopeIt == logicalDevice->commandBufferDepthStates.end() || !scopeIt->second.inRenderScope || !pCommandBuffers)
            return;

        uint32_t additionalDraws = 0;
        for (uint32_t i = 0; i < commandBufferCount; ++i)
        {
            auto countIt = logicalDevice->commandBufferRecordedDrawCounts.find(pCommandBuffers[i]);
            if (countIt != logicalDevice->commandBufferRecordedDrawCounts.end())
                additionalDraws += countIt->second;
        }

        if (additionalDraws == 0)
            return;

        scopeIt->second.drawCount += additionalDraws;
        Logger::debug("accumulated secondary command buffer draws: primary="
                      + convertToString(primaryCommandBuffer)
                      + " draws=" + std::to_string(additionalDraws)
                      + " total=" + std::to_string(scopeIt->second.drawCount));
    }

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
                              DepthSnapshotTarget* snapshotTarget,
                              VkImageLayout* pDepthFinalLayout)
    {
        auto scopeIt = logicalDevice->commandBufferDepthStates.find(commandBuffer);
        if (scopeIt == logicalDevice->commandBufferDepthStates.end())
            return false;

        LogicalDevice::DepthScopeTrackingState scopeState = scopeIt->second;

        // Output the tracked depthFinalLayout before erasing the scope.
        if (pDepthFinalLayout != nullptr)
            *pDepthFinalLayout = scopeState.depthFinalLayout;

        logicalDevice->commandBufferDepthStates.erase(scopeIt);

        if (!scopeState.inRenderScope || !hasDepthState(scopeState.depthState))
            return false;

        if (pPromotedDepthState != nullptr)
            *pPromotedDepthState = scopeState.depthState;
        if (snapshotTarget != nullptr)
            *snapshotTarget = scopeState.snapshotTarget;

        const bool scopeHasPresentableSnapshotTarget = hasPresentableSnapshotTarget(scopeState.snapshotTarget);
        const bool scopeExtentMatchesPresentableTarget =
            matchesPresentableSnapshotTargetExtent(scopeState.depthState, scopeState.snapshotTarget);

        // Calculate if this scope perfectly matches the game window (swapchain) size
        const bool scopeMatchesWindowSize = matchesAnySwapchainExtent(logicalDevice, scopeState.depthState);

        // Calculate if the CURRENT best candidate matches the window size
        const bool bestMatchesWindowSize = logicalDevice->bestDepthCandidate.valid
                                        ? matchesAnySwapchainExtent(logicalDevice, logicalDevice->bestDepthCandidate.depthState)
                                        : false;

        bool shouldPromote = !logicalDevice->bestDepthCandidate.valid;
        if (!shouldPromote)
        {
            // PRIORITY 1: Exact game window size match (Absolute highest priority)
            if (scopeMatchesWindowSize != bestMatchesWindowSize)
            {
                shouldPromote = scopeMatchesWindowSize;
            }
            // PRIORITY 2: Presentable snapshot target (Directly linked to swapchain color)
            else if (scopeHasPresentableSnapshotTarget != logicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget)
            {
                shouldPromote = scopeHasPresentableSnapshotTarget;
            }
            // PRIORITY 3: Draw count (More draws usually means main 3D geometry)
            else if (scopeState.drawCount != logicalDevice->bestDepthCandidate.drawCount)
            {
                shouldPromote = scopeState.drawCount > logicalDevice->bestDepthCandidate.drawCount;
            }
            // PRIORITY 4: Extent matches presentable target (Fallback tiebreaker)
            else if (scopeExtentMatchesPresentableTarget != logicalDevice->bestDepthCandidate.extentMatchesPresentableTarget)
            {
                shouldPromote = scopeExtentMatchesPresentableTarget;
            }
        }

        if (!shouldPromote)
            return false;

        logicalDevice->bestDepthCandidate.valid = true;
        logicalDevice->bestDepthCandidate.depthState = scopeState.depthState;
        logicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget = scopeHasPresentableSnapshotTarget;
        logicalDevice->bestDepthCandidate.extentMatchesPresentableTarget = scopeExtentMatchesPresentableTarget;
        logicalDevice->bestDepthCandidate.drawCount = scopeState.drawCount;

        Logger::debug("depth candidate promoted from " + std::string(reason)
                      + ": draws=" + std::to_string(scopeState.drawCount)
                      + " presentable=" + std::string(scopeHasPresentableSnapshotTarget ? "true" : "false")
                      + " extentMatch=" + std::string(scopeExtentMatchesPresentableTarget ? "true" : "false")
                      + " image=" + convertToString(scopeState.depthState.image)
                      + " view=" + convertToString(scopeState.depthState.imageView)
                      + " format=" + convertToString(scopeState.depthState.format)
                      + " extent=" + std::to_string(scopeState.depthState.extent.width) + "x"
                      + std::to_string(scopeState.depthState.extent.height)
                      + " samples=" + convertToString(scopeState.depthState.samples)
                      + " transient=" + std::string(scopeState.depthState.transient ? "true" : "false"));

        if (isQualifiedDepthCandidate(logicalDevice, scopeState))
        {
            updateDeviceDepthStateLocked(logicalDevice, scopeState.depthState, reason);
        }
        else
        {
            if (hasDepthState(scopeState.depthState) && scopeState.drawCount > 0)
                logicalDevice->pendingTransferLinkedDepthScopes[commandBuffer] = scopeState;

            Logger::debug("depth candidate not activated from " + std::string(reason)
                          + ": draws=" + std::to_string(scopeState.drawCount)
                          + " presentable=" + std::string(scopeHasPresentableSnapshotTarget ? "true" : "false")
                          + " extentMatch=" + std::string(scopeExtentMatchesPresentableTarget ? "true" : "false"));
        }
        return true;
    }

    void tryActivatePendingTransferLinkedDepthScope(LogicalDevice* logicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    VkImage destinationImage,
                                                    const char* reason)
    {
        if (!logicalDevice || destinationImage == VK_NULL_HANDLE)
            return;

        auto pendingIt = logicalDevice->pendingTransferLinkedDepthScopes.find(commandBuffer);
        if (pendingIt == logicalDevice->pendingTransferLinkedDepthScopes.end())
            return;

        DepthSnapshotTarget snapshotTarget = selectDepthSnapshotTargetFromImage(logicalDevice, destinationImage);
        if (!hasPresentableSnapshotTarget(snapshotTarget))
            return;

        auto scopeState = pendingIt->second;
        scopeState.snapshotTarget = snapshotTarget;
        logicalDevice->pendingTransferLinkedDepthScopes.erase(pendingIt);

        VKINTOX_LOG_DEBUG([&](std::string& s) {
            s = "depth candidate transfer-linked from " + std::string(reason)
              + ": draws=" + std::to_string(scopeState.drawCount)
              + " swapchain=" + convertToString(snapshotTarget.swapchain)
              + " imageIndex=" + std::to_string(snapshotTarget.imageIndex)
              + " depthImage=" + convertToString(scopeState.depthState.image)
              + " depthView=" + convertToString(scopeState.depthState.imageView);
        });

        // Transfer-linked scopes used to bypass the normal qualification gate
        // here.  That meant a presentable colour target could cause an arbitrary
        // depth-sized image to become active depth state.  Apply the exact same
        // candidate checks as the normal render-scope path.
        if (!isQualifiedDepthCandidate(logicalDevice, scopeState))
        {
            Logger::debug("rejecting transfer-linked depth candidate: image="
                          + convertToString(scopeState.depthState.image)
                          + " extent=" + std::to_string(scopeState.depthState.extent.width)
                          + "x" + std::to_string(scopeState.depthState.extent.height));
            return;
        }

        logicalDevice->bestDepthCandidate.valid = true;
        logicalDevice->bestDepthCandidate.depthState = scopeState.depthState;
        logicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget = true;
        logicalDevice->bestDepthCandidate.extentMatchesPresentableTarget =
            matchesPresentableSnapshotTargetExtent(scopeState.depthState, snapshotTarget);
        logicalDevice->bestDepthCandidate.drawCount = scopeState.drawCount;

        updateDeviceDepthStateLocked(logicalDevice, scopeState.depthState, reason);
        recordDepthResolveSnapshotForCommandBuffer(logicalDevice, commandBuffer, scopeState.depthState, &snapshotTarget);
    }

    // Caller MUST hold globalLock. Acquiring it here would deadlock because
    // both callers (tryActivatePendingTransferLinkedDepthScope and
    // VKIntox_CmdClearAttachments) already hold it when invoking us.
    void recordDepthResolveSnapshotForCommandBuffer(LogicalDevice* logicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    const DepthState& depthState,
                                                    const DepthSnapshotTarget* snapshotTarget)
    {
        if (!logicalDevice || !hasDepthState(depthState))
            return;

        // VALIDATE the depth state before recording commands against it. This
        // catches the case where a depth view was promoted to active state but
        // its underlying image was destroyed between promotion and the snapshot
        // recording (a common Roblox failure mode — Roblox recycles depth
        // images aggressively).
        if (!validateDepthStateForResolve(logicalDevice, depthState))
        {
            Logger::debug("recordDepthResolveSnapshotForCommandBuffer: depth state failed validation; skipping (image="
                          + convertToString(depthState.image) + ")");
            return;
        }

        if (snapshotTarget == nullptr || snapshotTarget->swapchain == VK_NULL_HANDLE)
        {
            Logger::debug("skip depth resolve snapshot: missing snapshot target for commandBuffer="
                          + convertToString(commandBuffer)
                          + " depthImage=" + convertToString(depthState.image)
                          + " depthView=" + convertToString(depthState.imageView));
            return;
        }

        DepthSnapshotTarget target = *snapshotTarget;
        auto swapIt = swapchainMap.find(target.swapchain);
        if (swapIt == swapchainMap.end() || !swapIt->second || swapIt->second->logicalDevice != logicalDevice)
        {
            Logger::debug("skip depth resolve snapshot: snapshot target swapchain not found for commandBuffer="
                          + convertToString(commandBuffer)
                          + " swapchain=" + convertToString(target.swapchain));
            return;
        }
        target.logicalSwapchain = swapIt->second.get();

        recordDepthResolveSnapshot(logicalDevice, target.logicalSwapchain, commandBuffer, target.imageIndex, depthState);
    }

    // Caller MUST hold globalLock (same rationale as above).
    void recordDepthResolveSnapshotForAllSwapchains(LogicalDevice* logicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    const DepthState& depthState)
    {
        if (!logicalDevice || !hasDepthState(depthState))
            return;

        // VALIDATE: same rationale as recordDepthResolveSnapshotForCommandBuffer.
        if (!validateDepthStateForResolve(logicalDevice, depthState))
        {
            Logger::debug("recordDepthResolveSnapshotForAllSwapchains: depth state failed validation; skipping (image="
                          + convertToString(depthState.image) + ")");
            return;
        }

        for (auto& [swapchainHandle, logicalSwapchain] : swapchainMap)
        {
            if (!logicalSwapchain || logicalSwapchain->logicalDevice != logicalDevice)
                continue;

            for (uint32_t imageIndex = 0; imageIndex < logicalSwapchain->imageCount; ++imageIndex)
            {
                recordDepthResolveSnapshot(logicalDevice, logicalSwapchain.get(), commandBuffer, imageIndex, depthState);
            }
        }
    }

    VkImageView getOrCreateTrackedDepthSampleViewLocked(LogicalDevice* logicalDevice, VkImage image, VkFormat format)
    {
        auto imageIt = std::find(logicalDevice->depthImages.begin(), logicalDevice->depthImages.end(), image);
        if (imageIt == logicalDevice->depthImages.end())
            return VK_NULL_HANDLE;

        const size_t index = std::distance(logicalDevice->depthImages.begin(), imageIt);
        if (index >= logicalDevice->depthImageViews.size())
            logicalDevice->depthImageViews.resize(index + 1, VK_NULL_HANDLE);

        VkImageView& trackedView = logicalDevice->depthImageViews[index];
        if (trackedView == VK_NULL_HANDLE)
            trackedView = createSingleImageView(logicalDevice, format, image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT);

        return trackedView;
    }

    void updateDeviceDepthStateLocked(LogicalDevice* logicalDevice, const DepthState& depth, const char* reason);
    void reallocateCommandBuffers(LogicalDevice* logicalDevice, LogicalSwapchain* logicalSwapchain, const DepthState& depth);

    void destroyDepthResolveResources(LogicalSwapchain* logicalSwapchain)
    {
        logicalSwapchain->depthResolveSourceView = VK_NULL_HANDLE;

        for (auto& framebuffer : logicalSwapchain->depthResolveFramebuffers)
        {
            logicalSwapchain->logicalDevice->vkd.DestroyFramebuffer(logicalSwapchain->logicalDevice->device, framebuffer, nullptr);
        }
        logicalSwapchain->depthResolveFramebuffers.clear();

        for (auto& framebuffer : logicalSwapchain->depthResolveMsaaFramebuffers)
        {
            logicalSwapchain->logicalDevice->vkd.DestroyFramebuffer(logicalSwapchain->logicalDevice->device, framebuffer, nullptr);
        }
        logicalSwapchain->depthResolveMsaaFramebuffers.clear();

        if (logicalSwapchain->depthResolveMsaaRenderPass)
        {
            logicalSwapchain->logicalDevice->vkd.DestroyRenderPass(
                logicalSwapchain->logicalDevice->device, logicalSwapchain->depthResolveMsaaRenderPass, nullptr);
            logicalSwapchain->depthResolveMsaaRenderPass = VK_NULL_HANDLE;
        }

        if (logicalSwapchain->depthResolvePipeline)
        {
            logicalSwapchain->logicalDevice->vkd.DestroyPipeline(
                logicalSwapchain->logicalDevice->device, logicalSwapchain->depthResolvePipeline, nullptr);
            logicalSwapchain->depthResolvePipeline = VK_NULL_HANDLE;
        }
        if (logicalSwapchain->depthResolvePipelineLayout)
        {
            logicalSwapchain->logicalDevice->vkd.DestroyPipelineLayout(
                logicalSwapchain->logicalDevice->device, logicalSwapchain->depthResolvePipelineLayout, nullptr);
            logicalSwapchain->depthResolvePipelineLayout = VK_NULL_HANDLE;
        }
        if (logicalSwapchain->depthResolveRenderPass)
        {
            logicalSwapchain->logicalDevice->vkd.DestroyRenderPass(
                logicalSwapchain->logicalDevice->device, logicalSwapchain->depthResolveRenderPass, nullptr);
            logicalSwapchain->depthResolveRenderPass = VK_NULL_HANDLE;
        }
        if (logicalSwapchain->depthResolveDescriptorPool)
        {
            logicalSwapchain->logicalDevice->vkd.DestroyDescriptorPool(
                logicalSwapchain->logicalDevice->device, logicalSwapchain->depthResolveDescriptorPool, nullptr);
            logicalSwapchain->depthResolveDescriptorPool = VK_NULL_HANDLE;
        }
        if (logicalSwapchain->depthResolveDescriptorSetLayout)
        {
            logicalSwapchain->logicalDevice->vkd.DestroyDescriptorSetLayout(
                logicalSwapchain->logicalDevice->device, logicalSwapchain->depthResolveDescriptorSetLayout, nullptr);
            logicalSwapchain->depthResolveDescriptorSetLayout = VK_NULL_HANDLE;
        }
        if (logicalSwapchain->depthResolveSampler)
        {
            logicalSwapchain->logicalDevice->vkd.DestroySampler(
                logicalSwapchain->logicalDevice->device, logicalSwapchain->depthResolveSampler, nullptr);
            logicalSwapchain->depthResolveSampler = VK_NULL_HANDLE;
        }

        for (auto& perImg : logicalSwapchain->depthResolvePerImage)
        {
            if (perImg.imageView != VK_NULL_HANDLE)
                logicalSwapchain->logicalDevice->vkd.DestroyImageView(
                    logicalSwapchain->logicalDevice->device, perImg.imageView, nullptr);
            if (perImg.image != VK_NULL_HANDLE)
                logicalSwapchain->logicalDevice->vkd.DestroyImage(
                    logicalSwapchain->logicalDevice->device, perImg.image, nullptr);
            if (perImg.memory != VK_NULL_HANDLE)
                logicalSwapchain->logicalDevice->vkd.FreeMemory(
                    logicalSwapchain->logicalDevice->device, perImg.memory, nullptr);
        }
        logicalSwapchain->depthResolvePerImage.clear();

        logicalSwapchain->depthResolveDescriptorSets.clear();
        logicalSwapchain->depthResolveFormat = VK_FORMAT_UNDEFINED;
        logicalSwapchain->depthResolveExtent = {0, 0, 1};
        logicalSwapchain->depthResolveUsesShader = false;
        logicalSwapchain->depthResolveIsMsaa = false;
        logicalSwapchain->depthResolveSourceSamples = VK_SAMPLE_COUNT_1_BIT;
        logicalSwapchain->depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    }

    static std::vector<VkImageView> collectDepthResolveImageViews(const LogicalSwapchain* sc)
    {
        std::vector<VkImageView> views(sc->depthResolvePerImage.size());
        for (size_t i = 0; i < sc->depthResolvePerImage.size(); i++)
            views[i] = sc->depthResolvePerImage[i].imageView;
        return views;
    }

    void initializeDepthResolveLayout(LogicalSwapchain* logicalSwapchain, const DepthState& depth)
    {
        LogicalDevice* logicalDevice = logicalSwapchain->logicalDevice;
        const bool isMsaa = depth.samples != VK_SAMPLE_COUNT_1_BIT;
        const bool useShaderResolve = !isMsaa && (depth.observedLayout == VK_IMAGE_LAYOUT_GENERAL);
        // MSAA depth resolve via depth-stencil resolve subpass (core 1.2). Works
        // for both GENERAL and non-GENERAL layouts: when GENERAL, we barrier the
        // source to attachment-optimal before the resolve subpass and restore after.
        const bool useMsaaResolve = isMsaa
            && (logicalDevice->supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
        logicalSwapchain->depthResolveUsesShader = useShaderResolve;
        logicalSwapchain->depthResolveIsMsaa = useMsaaResolve;
        logicalSwapchain->depthResolveSourceSamples = depth.samples;
        // The MSAA subpass and 1-sample transfer-copy paths keep the native depth
        // format. Only the 1-sample+GENERAL shader fallback uses R32_SFLOAT.
        logicalSwapchain->depthResolveFormat = useShaderResolve ? VK_FORMAT_R32_SFLOAT : depth.format;
        logicalSwapchain->depthResolveExtent = {depth.extent.width, depth.extent.height, 1};
        logicalSwapchain->depthResolveSourceView = depth.imageView;

        // Decide the depth resolve mode (average preferred for MSAA depth).
        VkResolveModeFlagBits chosenMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        if (useMsaaResolve)
        {
            const int modePref = settingsManager.getDepthResolveMode();
            const bool wantsAverage =
                (modePref == 0 || modePref == 2) && (logicalDevice->supportedDepthResolveModes & VK_RESOLVE_MODE_AVERAGE_BIT);
            chosenMode = wantsAverage ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        }
        logicalSwapchain->depthResolveMode = chosenMode;

        logicalSwapchain->depthResolvePerImage.clear();
        logicalSwapchain->depthResolvePerImage.resize(logicalSwapchain->imageCount);
        logicalSwapchain->depthResolvePerImage.shrink_to_fit();
        for (uint32_t i = 0; i < logicalSwapchain->imageCount; ++i)
        {
            VkDeviceMemory imageMemory = VK_NULL_HANDLE;
            std::vector<VkImage> images = createImages(logicalDevice,
                                                       1,
                                                       logicalSwapchain->depthResolveExtent,
                                                       logicalSwapchain->depthResolveFormat,
                                                       useShaderResolve
                                                           ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                                                           : (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
                                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                       imageMemory);
            // createImages returns exactly 1 image for depth resolve.
            logicalSwapchain->depthResolvePerImage[i].image = images[0];
            logicalSwapchain->depthResolvePerImage[i].memory = imageMemory;
        }

        {
            std::vector<VkImage> rawImages(logicalSwapchain->depthResolvePerImage.size());
            for (size_t idx = 0; idx < rawImages.size(); idx++)
                rawImages[idx] = logicalSwapchain->depthResolvePerImage[idx].image;
            auto views = createImageViews(logicalDevice,
                                           logicalSwapchain->depthResolveFormat,
                                           rawImages,
                                           VK_IMAGE_VIEW_TYPE_2D,
                                           useShaderResolve ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT);
            for (size_t idx = 0; idx < views.size() && idx < logicalSwapchain->depthResolvePerImage.size(); idx++)
                logicalSwapchain->depthResolvePerImage[idx].imageView = views[idx];
        }

        // Build the MSAA depth-stencil resolve render pass + framebuffers. The
        // source MSAA view is the per-tracked depth view (1 per swapchain image
        // is overkill; we reuse the single active source view for all frames).
        if (useMsaaResolve)
        {
            const VkImageLayout resolveFinalLayout =
                isStencilFormat(depth.format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                              : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
            logicalSwapchain->depthResolveMsaaRenderPass = createDepthMsaaResolveRenderPass(
                logicalDevice, depth.format, depth.samples, chosenMode, resolveFinalLayout);
            if (logicalSwapchain->depthResolveMsaaRenderPass != VK_NULL_HANDLE)
            {
                VkExtent2D resolveExtent2D = {logicalSwapchain->depthResolveExtent.width, logicalSwapchain->depthResolveExtent.height};
                std::vector<VkImageView> sourceViews(logicalSwapchain->imageCount, depth.imageView);
                logicalSwapchain->depthResolveMsaaFramebuffers = createFramebuffers(
                    logicalDevice, logicalSwapchain->depthResolveMsaaRenderPass, resolveExtent2D,
                    {sourceViews, collectDepthResolveImageViews(logicalSwapchain)});
            }
            else
            {
                // Device refused the resolve render pass; disable and let the
                // copy path take over (it will no-op harmlessly on MSAA since
                // samples mismatch, but at least we won't crash).
                logicalSwapchain->depthResolveIsMsaa = false;
            }
        }

        if (!useShaderResolve)
            return;

        logicalSwapchain->depthResolveSampler = createSampler(logicalDevice);
        logicalSwapchain->depthResolveDescriptorSetLayout = createImageSamplerDescriptorSetLayout(logicalDevice, 1);

        VkDescriptorPoolSize imagePoolSize = {};
        imagePoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        imagePoolSize.descriptorCount = logicalSwapchain->imageCount;
        logicalSwapchain->depthResolveDescriptorPool = createDescriptorPool(logicalDevice, {imagePoolSize});

        std::vector<VkImageView> sourceViews(logicalSwapchain->imageCount, depth.imageView);
        logicalSwapchain->depthResolveDescriptorSets = allocateAndWriteImageSamplerDescriptorSets(
            logicalDevice,
            logicalSwapchain->depthResolveDescriptorPool,
            logicalSwapchain->depthResolveDescriptorSetLayout,
            {logicalSwapchain->depthResolveSampler},
            {sourceViews});

        logicalSwapchain->depthResolveRenderPass =
            createRenderPass(logicalDevice, logicalSwapchain->depthResolveFormat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // Set up push constant range for depth resolve shaders
        // CRITICAL: Size must match the GLSL struct exactly!
        // GLSL layout: int(4) + bool(4) + bool(4) = 12 bytes
        // (GLSL bool is always int32/4 bytes, not C++ bool which is 1 byte)
        VkPushConstantRange depthPushConstantRange = {};
        depthPushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        depthPushConstantRange.offset = 0;
        depthPushConstantRange.size = sizeof(int32_t) * 3; // 12 bytes: depthMode + invertDepth + normalize

        logicalSwapchain->depthResolvePipelineLayout = createGraphicsPipelineLayout(
            logicalDevice,
            {logicalSwapchain->depthResolveDescriptorSetLayout},
            {depthPushConstantRange});

        VkShaderModule vertexModule = VK_NULL_HANDLE;
        VkShaderModule fragmentModule = VK_NULL_HANDLE;
        createShaderModule(logicalDevice, full_screen_triangle_vert, &vertexModule);

        // Universal shader handles all depth channel modes via push constants
        createShaderModule(logicalDevice, depth_resolve_universal_frag, &fragmentModule);

        VkExtent2D resolveExtent2D = {logicalSwapchain->depthResolveExtent.width, logicalSwapchain->depthResolveExtent.height};
        logicalSwapchain->depthResolvePipeline = createGraphicsPipeline(logicalDevice,
                                                                         vertexModule,
                                                                         nullptr,
                                                                         "main",
                                                                         fragmentModule,
                                                                         nullptr,
                                                                         "main",
                                                                         resolveExtent2D,
                                                                         logicalSwapchain->depthResolveRenderPass,
                                                                         logicalSwapchain->depthResolvePipelineLayout);
        logicalDevice->vkd.DestroyShaderModule(logicalDevice->device, fragmentModule, nullptr);
        logicalDevice->vkd.DestroyShaderModule(logicalDevice->device, vertexModule, nullptr);

        logicalSwapchain->depthResolveFramebuffers = createFramebuffers(
            logicalDevice, logicalSwapchain->depthResolveRenderPass, resolveExtent2D, {collectDepthResolveImageViews(logicalSwapchain)});
    }
    // Validate that a DepthState is safe to use for resolve/copy. Returns false
    // if any required field is missing, the underlying image is no longer
    // tracked, or the image lacks the usage flags the layer needs.
    //
    // This is the single chokepoint that all depth-resolve paths must pass
    // through before recording commands. Calling this prevents:
    //   - using a depth view whose image was destroyed out from under us
    //   - using a depth image that lacks SAMPLED/TRANSFER_SRC (shouldn't happen
    //     because VKIntox_CreateImage forces them, but defensive)
    //   - using a depth state with zero extent (e.g. right after a DestroyImage
    //     race where the state hasn't been cleared yet)
    bool validateDepthStateForResolve(LogicalDevice* logicalDevice, const DepthState& depth)
    {
        if (!hasDepthState(depth))
            return false;
        if (depth.extent.width == 0 || depth.extent.height == 0)
            return false;
        if (!isDepthStencilAttachmentFormat(depth.format))
            return false;
        if (isSwapchainImage(logicalDevice, depth.image))
        {
            Logger::debug("validateDepthStateForResolve: refusing swapchain/fake colour image as depth (image="
                          + convertToString(depth.image) + ")");
            return false;
        }

        // check if image is still tracked before proceeding
        auto extentIt = logicalDevice->depthImageExtents.find(depth.image);
        if (extentIt == logicalDevice->depthImageExtents.end())
        {
            Logger::debug("validateDepthStateForResolve: image handle not tracked (image="
                         + convertToString(depth.image) + "), likely destroyed by app");
            return false;
        }
        const VkExtent3D& tracked = extentIt->second;
        if (tracked.width != depth.extent.width || tracked.height != depth.extent.height || tracked.depth != depth.extent.depth)
        {
            Logger::debug("validateDepthStateForResolve: extent mismatch (tracked "
                         + std::to_string(tracked.width) + "x" + std::to_string(tracked.height)
                         + " vs depth " + std::to_string(depth.extent.width) + "x" + std::to_string(depth.extent.height)
                         + "), recycled handle detected");
            return false;
        }

        // Underlying image must still be tracked, UNLESS it's our persistent
        // storage (which is tracked separately via persistentStorageTracked).
        if (depth.image != logicalDevice->depthCaptureStorage.image
            || !logicalDevice->persistentStorageTracked)
        {
            if (std::find(logicalDevice->depthImages.begin(),
                          logicalDevice->depthImages.end(),
                          depth.image) == logicalDevice->depthImages.end())
            {
                Logger::warn("validateDepthStateForResolve: depth image no longer tracked (image="
                             + convertToString(depth.image) + "); skipping resolve");
                return false;
            }
        }

        // Image must have the usage flags we need.
        auto metadataIt = logicalDevice->depthImageMetadata.find(depth.image);
        if (metadataIt != logicalDevice->depthImageMetadata.end())
        {
            const VkImageUsageFlags required = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if ((metadataIt->second.usage & required) != required)
            {
                Logger::warn("validateDepthStateForResolve: depth image missing required usage flags"
                             " (image=" + convertToString(depth.image)
                             + " usage=0x" + formatHexU64(static_cast<uint64_t>(metadataIt->second.usage))
                             + "); skipping resolve");
                return false;
            }
        }

        return true;
    }

    // --- Persistent depth storage (depthCaptureMethod 1 & 2) ---

    void destroyPersistentDepthStorage(LogicalDevice* logicalDevice)
    {
        auto& s = logicalDevice->depthCaptureStorage;
        if (s.view != VK_NULL_HANDLE)
        {
            logicalDevice->vkd.DestroyImageView(logicalDevice->device, s.view, nullptr);
            s.view = VK_NULL_HANDLE;
        }
        if (s.image != VK_NULL_HANDLE)
        {
            logicalDevice->vkd.DestroyImage(logicalDevice->device, s.image, nullptr);
            logicalDevice->depthImageMetadata.erase(s.image);
            s.image = VK_NULL_HANDLE;
        }
        if (s.memory != VK_NULL_HANDLE)
        {
            logicalDevice->vkd.FreeMemory(logicalDevice->device, s.memory, nullptr);
            s.memory = VK_NULL_HANDLE;
        }
        s.extent = {0, 0, 1};
        s.format = VK_FORMAT_UNDEFINED;
        s.valid = false;
        logicalDevice->persistentStorageTracked = false;

        Logger::debug("persistent depth storage destroyed");
    }

    void ensurePersistentDepthStorage(LogicalDevice* logicalDevice, VkFormat format, const VkExtent3D& extent)
    {
        auto& s = logicalDevice->depthCaptureStorage;
        if (s.image != VK_NULL_HANDLE && s.format == format
            && s.extent.width == extent.width && s.extent.height == extent.height)
            return;  // Already created with matching format/size

        if (s.image != VK_NULL_HANDLE)
            destroyPersistentDepthStorage(logicalDevice);

        if (!isDepthFormat(format))
            return;
        if (extent.width == 0 || extent.height == 0)
            return;

        VkImageCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = extent;
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                    | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                    | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkResult vr = logicalDevice->vkd.CreateImage(logicalDevice->device, &ici, nullptr, &s.image);
        if (vr != VK_SUCCESS)
        {
            Logger::warn("ensurePersistentDepthStorage: CreateImage failed (" + std::to_string(vr) + ")");
            return;
        }

        VkMemoryRequirements memReq;
        logicalDevice->vkd.GetImageMemoryRequirements(logicalDevice->device, s.image, &memReq);

        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        // Prefer DEVICE_LOCAL (GPU-only) for performance.
        mai.memoryTypeIndex = 0;
        VkPhysicalDeviceMemoryProperties memProps;
        logicalDevice->vki.GetPhysicalDeviceMemoryProperties(logicalDevice->physicalDevice, &memProps);
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
        {
            if ((memReq.memoryTypeBits & (1u << i))
                && (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            {
                mai.memoryTypeIndex = i;
                break;
            }
        }

        vr = logicalDevice->vkd.AllocateMemory(logicalDevice->device, &mai, nullptr, &s.memory);
        if (vr != VK_SUCCESS)
        {
            Logger::warn("ensurePersistentDepthStorage: AllocateMemory failed (" + std::to_string(vr) + ")");
            logicalDevice->vkd.DestroyImage(logicalDevice->device, s.image, nullptr);
            s.image = VK_NULL_HANDLE;
            return;
        }

        logicalDevice->vkd.BindImageMemory(logicalDevice->device, s.image, s.memory, 0);

        VkImageViewCreateInfo ivci = {};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = s.image;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = format;
        ivci.subresourceRange.aspectMask = isStencilFormat(format)
            ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
            : VK_IMAGE_ASPECT_DEPTH_BIT;
        ivci.subresourceRange.baseMipLevel = 0;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.baseArrayLayer = 0;
        ivci.subresourceRange.layerCount = 1;

        vr = logicalDevice->vkd.CreateImageView(logicalDevice->device, &ivci, nullptr, &s.view);
        if (vr != VK_SUCCESS)
        {
            Logger::warn("ensurePersistentDepthStorage: CreateImageView failed (" + std::to_string(vr) + ")");
            destroyPersistentDepthStorage(logicalDevice);
            return;
        }

        s.extent = extent;
        s.format = format;

        // Register metadata so validateDepthStateForResolve accepts it.
        // We do NOT push into depthImages/depthFormats — those parallel-indexed
        // vectors are for app-created depth images only. Mixing in layer-internal
        // images breaks the index invariant and causes vector OOB crashes.
        DepthImageMetadata meta;
        meta.usage = ici.usage;
        meta.samples = VK_SAMPLE_COUNT_1_BIT;
        meta.tiling = VK_IMAGE_TILING_OPTIMAL;
        logicalDevice->depthImageMetadata[s.image] = meta;
        logicalDevice->persistentStorageTracked = true;

        Logger::info("persistent depth storage created: " + std::to_string(extent.width) + "x"
                     + std::to_string(extent.height) + " format=" + std::to_string(format));
    }
    void ensureDepthResolveResources(LogicalSwapchain* logicalSwapchain, const DepthState& depth)
    {
        // Destroy resolve resources if depth state is no longer valid (e.g.
        // after the depth image was destroyed or the pin was cleared).  This
        // prevents command buffers from sampling stale resolve images.
        if (!hasDepthState(depth) || depth.extent.width == 0 || depth.extent.height == 0)
        {
            destroyDepthResolveResources(logicalSwapchain);
            return;
        }

        const bool isMsaa = depth.samples != VK_SAMPLE_COUNT_1_BIT;
        const bool useShaderResolve = !isMsaa && (depth.observedLayout == VK_IMAGE_LAYOUT_GENERAL);
        const bool wantsMsaaResolve = isMsaa
            && (logicalSwapchain->logicalDevice->supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
        const VkFormat wantedResolveFormat = useShaderResolve ? VK_FORMAT_R32_SFLOAT : depth.format;

        // SOURCE-VIEW CHANGE DETECTION (Critical Bug 1, 2, 4 fix):
        // The MSAA resolve framebuffers and the shader-resolve descriptor sets
        // both bake the depth source view in at creation time. If the active
        // depth view changes (e.g. user pins a different 1920x1080 depth buffer
        // in Roblox), we MUST tear down and rebuild — otherwise the resolve
        // silently reads from the previous depth image, which may have been
        // destroyed or contain unrelated data, producing the "blank depth"
        // symptom.
        const bool sourceViewChanged = logicalSwapchain->depthResolveSourceView != depth.imageView;

        if (!sourceViewChanged
            && logicalSwapchain->depthResolveFormat == wantedResolveFormat
            && logicalSwapchain->depthResolveExtent.width == depth.extent.width
            && logicalSwapchain->depthResolveExtent.height == depth.extent.height
            && logicalSwapchain->depthResolveExtent.depth == 1
            && logicalSwapchain->depthResolveUsesShader == useShaderResolve
            && logicalSwapchain->depthResolveIsMsaa == wantsMsaaResolve
            && logicalSwapchain->depthResolveSourceSamples == depth.samples
            && logicalSwapchain->depthResolvePerImage.size() == logicalSwapchain->imageCount
            && (!wantsMsaaResolve
                || (logicalSwapchain->depthResolveMsaaRenderPass != VK_NULL_HANDLE
                    && logicalSwapchain->depthResolveMsaaFramebuffers.size() == logicalSwapchain->imageCount))
            && (!useShaderResolve
                || (logicalSwapchain->depthResolveDescriptorSets.size() == logicalSwapchain->imageCount
                    && logicalSwapchain->depthResolveRenderPass != VK_NULL_HANDLE
                    && logicalSwapchain->depthResolvePipelineLayout != VK_NULL_HANDLE
                    && logicalSwapchain->depthResolvePipeline != VK_NULL_HANDLE
                    && logicalSwapchain->depthResolveFramebuffers.size() == logicalSwapchain->imageCount)))
        {
            return;
        }

        if (sourceViewChanged)
        {
            Logger::info("depth resolve source view changed: old="
                         + convertToString(logicalSwapchain->depthResolveSourceView)
                         + " new=" + convertToString(depth.imageView)
                         + " — rebuilding descriptor sets and MSAA framebuffers");
        }

        destroyDepthResolveResources(logicalSwapchain);
        initializeDepthResolveLayout(logicalSwapchain, depth);
    }

    bool depthMatchesSwapchainExtent(const DepthState& depth, const LogicalSwapchain* sc)
    {
        return sc != nullptr
            && hasDepthState(depth)
            && depth.extent.width == sc->imageExtent.width
            && depth.extent.height == sc->imageExtent.height;
    }

    void armDepthRetryLocked(LogicalDevice* logicalDevice, const LogicalSwapchain* sc, const char* reason)
    {
        if (!logicalDevice)
            return;

        auto& retry = depthRetryStates[logicalDevice];
        retry.disabled = true;
        retry.retryPending = true;
        retry.retryAt = std::chrono::steady_clock::now() + DEPTH_RETRY_DELAY;
        retry.blockedExtent = sc ? sc->imageExtent : VkExtent2D{0, 0};

        logicalDevice->activeDepthState = {};
        logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
        logicalDevice->depthReallocPending = false;
        for (auto& [_, swapchain] : swapchainMap)
            if (swapchain && swapchain->logicalDevice == logicalDevice)
                swapchain->depthReallocPending = true;

        Logger::info(std::string("depth injection temporarily disabled: ") + reason
                     + "; retrying in ~5s at swapchain extent "
                     + std::to_string(retry.blockedExtent.width) + "x"
                     + std::to_string(retry.blockedExtent.height));
    }

    bool depthRetryDueLocked(LogicalDevice* logicalDevice)
    {
        auto it = depthRetryStates.find(logicalDevice);
        if (it == depthRetryStates.end() || !it->second.retryPending)
            return false;
        return std::chrono::steady_clock::now() >= it->second.retryAt;
    }

    void scheduleDepthRetryLocked(LogicalDevice* logicalDevice, bool fastPoll)
    {
        auto& retry = depthRetryStates[logicalDevice];
        retry.disabled = true;
        retry.retryPending = true;
        retry.retryAt = std::chrono::steady_clock::now()
                      + (fastPoll ? DEPTH_REBUILD_POLL_DELAY : DEPTH_RETRY_DELAY);
    }

    bool depthRebuildFencesReady(LogicalDevice* logicalDevice, const LogicalSwapchain* sc)
    {
        if (!logicalDevice || !sc)
            return false;

        // Non-blocking safety check.  Never WaitForFences/QueueWaitIdle here.
        // If any effect command buffer is still in flight, simply try again on
        // a later present.
        for (size_t index = 0; index < sc->effectSubmitFences.size(); ++index)
        {
            // Newly allocated swapchain command buffers have unsignaled fences
            // that have never been submitted; they are already safe to rewrite.
            if (index >= sc->effectSubmitFenceUsed.size() || !sc->effectSubmitFenceUsed[index])
                continue;

            const VkFence fence = sc->effectSubmitFences[index];
            if (fence == VK_NULL_HANDLE)
                continue;

            VkResult vr = reinterpret_cast<PFN_vkGetFenceStatus>(logicalDevice->vkd.GetDeviceProcAddr(logicalDevice->device, "vkGetFenceStatus"))(logicalDevice->device, fence);
            if (vr == VK_NOT_READY)
                return false;
            if (vr == VK_ERROR_DEVICE_LOST)
            {
                reportDeviceLostDiagnostics(logicalDevice, logicalDevice->queue,
                                            "depth rebuild fence status", vr);
                panicLayer(logicalDevice, "Device lost while checking depth rebuild fences");
                return false;
            }
            if (vr != VK_SUCCESS)
            {
                Logger::warn("depth rebuild fence status returned " + std::to_string(vr));
                return false;
            }
        }

        return true;
    }

    // Get depth state from logical device (returns null handles if no depth images)
    DepthState getDepthState(LogicalDevice* logicalDevice)
    {
        if (logicalDevice->pinnedDepthImageView != VK_NULL_HANDLE)
        {
            auto it = logicalDevice->depthViewStates.find(logicalDevice->pinnedDepthImageView);
            if (it != logicalDevice->depthViewStates.end())
            {
                // Extra safety: verify the underlying image is still tracked
                const DepthState& pinned = it->second;
                bool imageStillTracked = !logicalDevice->depthImages.empty() &&
                    std::find(logicalDevice->depthImages.begin(), logicalDevice->depthImages.end(), pinned.image)
                        != logicalDevice->depthImages.end();
                if (imageStillTracked)
                    return pinned;
                // Image was destroyed but view entry wasn't cleaned up.  Do not
                // silently fall back to the previous active image: that can point
                // at a different extent and rebuild stale resolve resources.
                Logger::debug("getDepthState: pinned view's image no longer tracked; disabling depth");
            }
            else
            {
                Logger::debug("getDepthState: pinned depth view no longer tracked; disabling depth");
            }
            logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
            return {};
        }
        return logicalDevice->activeDepthState;
    }

    // Perform the actual command buffer reallocation for all swapchains on a device.
    // MUST be called from a safe context (QueuePresentKHR or reload path) where the
    // GPU has been idle'd first, to avoid freeing in-flight command buffers.
    void performDeferredDepthRealloc(LogicalDevice* logicalDevice)
    {
        // clear ALL swapchain resolve views FIRST, before any validation
        for (auto& [handle, sc] : swapchainMap)
        {
            if (sc && sc->logicalDevice == logicalDevice)
            {
                sc->depthResolveSourceView = VK_NULL_HANDLE;
                for (auto& img : sc->depthResolvePerImage)
                    img.image = VK_NULL_HANDLE;
            }
        }

        DepthState effectiveDepth = getDepthState(logicalDevice);

        // VALIDATE before rebuilding resolve resources. If the depth state is
        // stale (image destroyed, missing usage flags, zero extent), we tear
        // down any existing resolve resources for each swapchain instead of
        // rebuilding against garbage. This is the chokepoint for the
        // "depth buffer doesn't work in Roblox" symptom: if the active depth
        // view points at a destroyed image, we must not let ensureDepthResolveResources
        // build descriptor sets against it.
        const bool depthValid = validateDepthStateForResolve(logicalDevice, effectiveDepth);
        if (hasDepthState(effectiveDepth) && !depthValid)
        {
            Logger::warn("performDeferredDepthRealloc: effective depth state failed validation; "
                         "tearing down resolve resources and clearing active state");
            // Clear the active state so getDepthState() returns empty next time
            // (otherwise we'd keep retrying with the same invalid state).
            logicalDevice->activeDepthState = {};
            if (logicalDevice->pinnedDepthImageView != VK_NULL_HANDLE)
                logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
            effectiveDepth = DepthState{};
        }

        for (auto& [swapchainHandle, logicalSwapchain] : swapchainMap)
        {
            if (logicalSwapchain->logicalDevice != logicalDevice)
                continue;
            if (logicalSwapchain->commandBuffersEffect.empty())
                continue;

            reallocateCommandBuffers(logicalDevice, logicalSwapchain.get(), effectiveDepth);
            Logger::debug("reallocated command buffers for swapchain " + convertToString(swapchainHandle) + " (deferred depth change)");
        }
        logicalDevice->depthReallocPending = false;
    }

    void updateDeviceDepthStateLocked(LogicalDevice* logicalDevice, const DepthState& depth, const char* reason)
    {
        if (sameDepthState(logicalDevice->activeDepthState, depth))
            return;

        logicalDevice->activeDepthState = depth;
        Logger::debug(std::string("active depth state updated from ") + reason + ": image=" + convertToString(depth.image)
                      + " view=" + convertToString(depth.imageView) + " format=" + convertToString(depth.format)
                      + " extent=" + std::to_string(depth.extent.width) + "x" + std::to_string(depth.extent.height)
                      + " observedLayout=" + convertToString(depth.observedLayout));

        auto metadataIt = logicalDevice->depthImageMetadata.find(depth.image);
        if (metadataIt != logicalDevice->depthImageMetadata.end())
        {
            Logger::debug(std::string("active depth state metadata from ") + reason
                          + ": image=" + convertToString(depth.image)
                          + " usage=0x" + formatHexU64(static_cast<uint64_t>(metadataIt->second.usage))
                          + " samples=" + convertToString(metadataIt->second.samples)
                          + " tiling=" + convertToString(metadataIt->second.tiling)
                          + " transient=" + std::string((metadataIt->second.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0 ? "true" : "false"));
        }

        // Defer command-buffer rebuild to a non-blocking safe point in
        // QueuePresentKHR.  No QueueWaitIdle is performed by the depth path.
        logicalDevice->depthReallocPending = true;
        for (auto& [_, swapchain] : swapchainMap)
            if (swapchain && swapchain->logicalDevice == logicalDevice)
                swapchain->depthReallocPending = true;
    }

    // Helper to reallocate and rewrite command buffers for a swapchain
    void reallocateCommandBuffers(
        LogicalDevice* logicalDevice,
        LogicalSwapchain* logicalSwapchain,
        const DepthState& depth)
    {
        // Free existing command buffers
        if (!logicalSwapchain->commandBuffersEffect.empty())
        {
            logicalDevice->vkd.FreeCommandBuffers(
                logicalDevice->device, logicalDevice->commandPool,
                logicalSwapchain->commandBuffersEffect.size(),
                logicalSwapchain->commandBuffersEffect.data());
        }
        if (!logicalSwapchain->commandBuffersNoEffect.empty())
        {
            logicalDevice->vkd.FreeCommandBuffers(
                logicalDevice->device, logicalDevice->commandPool,
                logicalSwapchain->commandBuffersNoEffect.size(),
                logicalSwapchain->commandBuffersNoEffect.data());
        }

        ensureDepthResolveResources(logicalSwapchain, depth);

        // Allocate and write effect command buffers
        logicalSwapchain->commandBuffersEffect = allocateCommandBuffer(logicalDevice, logicalSwapchain->imageCount);
        writeCommandBuffers(logicalDevice,
                            logicalSwapchain,
                            logicalSwapchain->effects,
                            logicalSwapchain->commandBuffersEffect,
                            depth);

        // Allocate and write no-effect command buffers
        logicalSwapchain->commandBuffersNoEffect = allocateCommandBuffer(logicalDevice, logicalSwapchain->imageCount);
        writeCommandBuffers(logicalDevice,
                            logicalSwapchain,
                            {logicalSwapchain->defaultTransfer},
                            logicalSwapchain->commandBuffersNoEffect,
                            depth);
    }

} // namespace VKIntox
