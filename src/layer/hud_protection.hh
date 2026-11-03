#ifndef HUD_PROTECTION_HPP_INCLUDED
#define HUD_PROTECTION_HPP_INCLUDED

#include "render/device/vulkan_include.hh"
#include "render/device/logical_device.hh"
#include "render/swapchain/logical_swapchain.hh"

namespace VKIntox::Hud
{
    bool enabled();
    bool init(LogicalSwapchain* logicalSwapchain);
    void destroy(LogicalSwapchain* logicalSwapchain);
    bool rebuildCommandBuffers(LogicalDevice* logicalDevice, LogicalSwapchain* logicalSwapchain,
                               const DepthState& depthState, VkCommandPool pool);
    bool activeFor(const LogicalSwapchain* logicalSwapchain, uint32_t imageIndex);

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
