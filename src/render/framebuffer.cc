#include "framebuffer.hh"
#include "logger.hh"

namespace VKIntox
{
    std::vector<VkFramebuffer>
    createFramebuffers(LogicalDevice* logicalDevice, VkRenderPass renderPass, VkExtent2D& extent, std::vector<std::vector<VkImageView>> imageViews)
    {
        if (imageViews.empty() || imageViews[0].empty())
        {
            Logger::warn("createFramebuffers: empty imageViews");
            return {};
        }
        std::vector<VkFramebuffer> framebuffers(imageViews[0].size());
        std::vector<VkImageView>   perFrameImageViews;
        for (uint32_t i = 0; i < imageViews[0].size(); i++)
        {
            for (auto& iv : imageViews)
            {
                perFrameImageViews.push_back(iv[i]);
            }

            VkFramebufferCreateInfo framebufferCreateInfo;
            framebufferCreateInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferCreateInfo.pNext           = nullptr;
            framebufferCreateInfo.flags           = 0;
            framebufferCreateInfo.renderPass      = renderPass;
            framebufferCreateInfo.attachmentCount = perFrameImageViews.size();
            framebufferCreateInfo.pAttachments    = perFrameImageViews.data();
            framebufferCreateInfo.width           = extent.width;
            framebufferCreateInfo.height          = extent.height;
            framebufferCreateInfo.layers          = 1;

            VkResult result = logicalDevice->vkd.CreateFramebuffer(logicalDevice->device, &framebufferCreateInfo, nullptr, &(framebuffers[i]));
            if (result != VK_SUCCESS)
            {
                Logger::err("createFramebuffers: vkCreateFramebuffer failed: " + std::to_string(result));
                for (uint32_t created = 0; created < i; created++)
                    logicalDevice->vkd.DestroyFramebuffer(logicalDevice->device, framebuffers[created], nullptr);
                return {};
            }
            perFrameImageViews.clear();
        }
        return framebuffers;
    }
} // namespace VKIntox
