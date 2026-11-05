#ifndef HUD_PROTECTION_HPP_INCLUDED
#define HUD_PROTECTION_HPP_INCLUDED

#include <memory>
#include <vector>

#include "render/device/vulkan_include.hh"
#include "render/device/logical_device.hh"
#include "render/swapchain/logical_swapchain.hh"

namespace VKIntox::Hud
{
    bool enabled();
    bool init(LogicalSwapchain* logicalSwapchain);
    void destroy(LogicalSwapchain* logicalSwapchain);

    // Builds the chain the present path records: scene-only input first when
    // protection is live, then the user effects, then the HUD composite and
    // its transfer to the real swapchain image.
    std::vector<std::shared_ptr<Effect>> composeEffects(
        LogicalSwapchain* logicalSwapchain,
        const std::vector<std::shared_ptr<Effect>>& effects);

    // Points the composite pass at the slot holding the last effect's output.
    // Called after a chain rebuild, when the enabled effect count changed.
    void refresh(LogicalSwapchain* logicalSwapchain, size_t processedSlot);

    void commandBufferBegan(VkCommandBuffer commandBuffer);
    void commandBufferFreed(VkCommandBuffer commandBuffer);
    void beginRenderPass(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer,
                         const VkRenderPassBeginInfo* pRenderPassBegin);
    void beginRendering(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer,
                        const VkRenderingInfo* pRenderingInfo);
    void endRenderPass(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer);
    void draw(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer, bool plainDraw,
              uint32_t vertexCount, uint32_t instanceCount);
    void blitImage(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer, VkImage srcImage, VkImage dstImage);
    void copyImage(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer, VkImage srcImage, VkImage dstImage);
    void renderPassDestroyed(LogicalDevice* logicalDevice, VkRenderPass renderPass);
}

#endif
