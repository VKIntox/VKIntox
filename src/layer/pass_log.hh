#ifndef VKINTOX_PASS_LOG_HPP_INCLUDED
#define VKINTOX_PASS_LOG_HPP_INCLUDED

#include "render/device/vulkan_include.hh"
#include "render/device/logical_device.hh"

namespace VKIntox::PassLog
{
    struct RenderPassInfo
    {
        bool                  hasDepth = false;
        VkFormat              depthFormat = VK_FORMAT_UNDEFINED;
        VkFormat              colorFormat = VK_FORMAT_UNDEFINED;
        VkAttachmentLoadOp    colorLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        VkAttachmentStoreOp   colorStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        VkImageLayout         colorInitialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout         colorFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout         colorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        VkSubpassDescriptionFlags subpassFlags = 0;
        bool                  multiview = false;
        uint32_t              attachmentCount = 0;
        uint32_t              colorCount = 0;
        uint32_t              subpassCount = 0;
        uint32_t              colorIndex = 0;
    };

    bool enabled();
    bool queryRenderPass(VkRenderPass renderPass, RenderPassInfo& info);
    VkImage colorAttachmentImage(VkFramebuffer framebuffer, VkRenderPass renderPass);
    void renderPassCreated(VkRenderPass renderPass, const VkRenderPassCreateInfo* pCreateInfo);
    void renderPassCreated(VkRenderPass renderPass, const VkRenderPassCreateInfo2* pCreateInfo);
    void renderPassDestroyed(VkRenderPass renderPass);
    void imageViewCreated(VkImageView imageView, VkImage image);
    void imageViewDestroyed(VkImageView imageView);
    void framebufferCreated(VkFramebuffer framebuffer, const VkFramebufferCreateInfo* pCreateInfo);
    void framebufferDestroyed(VkFramebuffer framebuffer);
    void commandBufferBegan(VkCommandBuffer commandBuffer);
    void commandBufferFreed(VkCommandBuffer commandBuffer);
    void begin(VkCommandBuffer commandBuffer, VkRenderPass renderPass, VkFramebuffer framebuffer,
               const VkRect2D& renderArea, const DepthSnapshotTarget& target);
    void beginDynamic(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo,
                      const DepthSnapshotTarget& target);
    void countDraw(VkCommandBuffer commandBuffer, uint32_t drawCount, uint32_t vertices = 0, bool indexed = false);
    void end(VkCommandBuffer commandBuffer);
    void present(const VkPresentInfoKHR* pPresentInfo);
    void copyImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImage dstImage,
                   const VkImageCopy* pRegions, uint32_t regionCount);
    void blitImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImage dstImage,
                   const VkImageBlit* pRegions, uint32_t regionCount, VkFilter filter);
}

#endif
