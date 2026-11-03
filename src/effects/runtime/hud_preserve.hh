#ifndef HUD_PRESERVE_HPP_INCLUDED
#define HUD_PRESERVE_HPP_INCLUDED

#include <vector>

#include "effects/runtime/effect.hh"
#include "render/device/logical_device.hh"
#include "render/device/vulkan_include.hh"

namespace VKIntox
{
    class HudPreserveEffect : public Effect
    {
    public:
        HudPreserveEffect(LogicalDevice*            logicalDevice,
                          VkFormat                  format,
                          VkExtent2D                imageExtent,
                          std::vector<VkImage>      outputImages,
                          std::vector<VkImageView>  outputImageViews,
                          std::vector<VkImageView>  sceneViews,
                          std::vector<VkImageView>  finalViews,
                          uint32_t                  imageCount);
        ~HudPreserveEffect() override;

        void applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) override;

        bool valid() const { return initialized; }

    private:
        struct PerImage
        {
            VkImage         scratch = VK_NULL_HANDLE;
            VkDeviceMemory  scratchMemory = VK_NULL_HANDLE;
            VkImageView     scratchView = VK_NULL_HANDLE;
            VkFramebuffer   framebuffer = VK_NULL_HANDLE;
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
            bool            scratchInitialized = false;
        };

        bool initialize();
        void releaseResources();

        LogicalDevice*             logicalDevice = nullptr;
        VkFormat                   format = VK_FORMAT_UNDEFINED;
        VkExtent2D                 imageExtent = {0, 0};
        std::vector<VkImage>       outputImages;
        std::vector<VkImageView>   outputImageViews;
        std::vector<VkImageView>   sceneViews;
        std::vector<VkImageView>   finalViews;
        std::vector<PerImage>      perImage;
        uint32_t                   imageCount = 0;

        bool                       initialized = false;
        VkRenderPass               renderPass = VK_NULL_HANDLE;
        VkPipelineLayout           pipelineLayout = VK_NULL_HANDLE;
        VkPipeline                 pipeline = VK_NULL_HANDLE;
        VkDescriptorSetLayout      descriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool           descriptorPool = VK_NULL_HANDLE;
        VkSampler                  sampler = VK_NULL_HANDLE;
    };
}

#endif
