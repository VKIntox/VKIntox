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
                          std::vector<VkImage>      processedImages,
                          std::vector<VkImageView>  processedViews,
                          std::vector<VkImageView>  sceneViews,
                          std::vector<VkImageView>  finalViews,
                          std::vector<VkImageView>  validViews,
                          uint32_t                  imageCount);
        ~HudPreserveEffect() override;

        void applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) override;
        void updateProcessedViews(const std::vector<VkImage>& images,
                                  const std::vector<VkImageView>& views);

        bool valid() const { return initialized; }

    private:
        bool initialize();
        void releaseResources();

        LogicalDevice*             logicalDevice = nullptr;
        VkFormat                   format = VK_FORMAT_UNDEFINED;
        VkExtent2D                 imageExtent = {0, 0};
        std::vector<VkImage>       outputImages;
        std::vector<VkImageView>   outputImageViews;
        std::vector<VkImage>       processedImages;
        std::vector<VkImageView>   processedViews;
        std::vector<VkImageView>   sceneViews;
        std::vector<VkImageView>   finalViews;
        std::vector<VkImageView>   validViews;
        std::vector<VkFramebuffer> framebuffers;
        std::vector<VkDescriptorSet> descriptorSets;
        uint32_t                   imageCount = 0;

        bool                       initialized = false;
        VkRenderPass               renderPass = VK_NULL_HANDLE;
        VkPipelineLayout           pipelineLayout = VK_NULL_HANDLE;
        VkPipeline                 pipeline = VK_NULL_HANDLE;
        VkDescriptorSetLayout      descriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool           descriptorPool = VK_NULL_HANDLE;
        VkSampler                  sampler = VK_NULL_HANDLE;
    };

    class HudSceneInputEffect : public Effect
    {
    public:
        HudSceneInputEffect(LogicalDevice*           logicalDevice,
                            VkFormat                 format,
                            VkExtent2D               imageExtent,
                            std::vector<VkImageView>  outputImageViews,
                            std::vector<VkImageView>  sceneViews,
                            std::vector<VkImageView>  validViews,
                            uint32_t                 imageCount);
        ~HudSceneInputEffect() override;

        void applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) override;
        bool valid() const { return initialized; }

    private:
        bool initialize();
        void releaseResources();

        LogicalDevice*             logicalDevice = nullptr;
        VkFormat                   format = VK_FORMAT_UNDEFINED;
        VkExtent2D                 imageExtent = {0, 0};
        std::vector<VkImageView>   outputImageViews;
        std::vector<VkImageView>   sceneViews;
        std::vector<VkImageView>   validViews;
        std::vector<VkFramebuffer> framebuffers;
        std::vector<VkDescriptorSet> descriptorSets;
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
