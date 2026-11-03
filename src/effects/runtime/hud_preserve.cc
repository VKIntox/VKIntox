#include "effects/runtime/hud_preserve.hh"

#include "effects/compiler/shader_sources.hh"
#include "render/device/vk_handle.hh"
#include "render/pipeline/graphics_pipeline.hh"
#include "render/pipeline/shader.hh"
#include "render/resources/descriptor_set.hh"
#include "render/resources/image.hh"
#include "render/resources/image_view.hh"
#include "util/logger.hh"

namespace VKIntox
{
    HudPreserveEffect::HudPreserveEffect(LogicalDevice*           logicalDevice,
                                         VkFormat                 format,
                                         VkExtent2D               imageExtent,
                                         std::vector<VkImage>     outputImages,
                                         std::vector<VkImageView> outputImageViews,
                                         std::vector<VkImageView> sceneViews,
                                         std::vector<VkImageView> finalViews,
                                         uint32_t                 imageCount)
        : logicalDevice(logicalDevice),
          format(format),
          imageExtent(imageExtent),
          outputImages(std::move(outputImages)),
          outputImageViews(std::move(outputImageViews)),
          sceneViews(std::move(sceneViews)),
          finalViews(std::move(finalViews)),
          imageCount(imageCount)
    {
        initialized = initialize();
        if (!initialized)
        {
            Logger::warn("HUD preserve pass unavailable; effects will run without HUD protection");
            releaseResources();
        }
    }

    bool HudPreserveEffect::initialize()
    {
        if (!logicalDevice || format == VK_FORMAT_UNDEFINED || imageExtent.width == 0 || imageExtent.height == 0)
            return false;
        if (imageCount == 0 || outputImages.size() < imageCount || outputImageViews.size() < imageCount
            || sceneViews.size() < imageCount || finalViews.size() < imageCount)
            return false;

        perImage.resize(imageCount);

        descriptorSetLayout = createImageSamplerDescriptorSetLayout(logicalDevice, 3);
        if (descriptorSetLayout == VK_NULL_HANDLE)
            return false;

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = imageCount * 3;
        descriptorPool = createDescriptorPool(logicalDevice, {poolSize});
        if (descriptorPool == VK_NULL_HANDLE)
            return false;

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0f;
        if (logicalDevice->vkd.CreateSampler(logicalDevice->device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
            return false;

        VkAttachmentDescription attachment{};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorReference{};
        colorReference.attachment = 0;
        colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;

        VkSubpassDependency dependencies[2]{};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &attachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 2;
        renderPassInfo.pDependencies = dependencies;
        if (logicalDevice->vkd.CreateRenderPass(logicalDevice->device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS)
            return false;

        pipelineLayout = createGraphicsPipelineLayout(logicalDevice, {descriptorSetLayout}, {});
        if (pipelineLayout == VK_NULL_HANDLE)
            return false;

        VkShaderModule vertexModule = VK_NULL_HANDLE;
        VkShaderModule fragmentModule = VK_NULL_HANDLE;
        createShaderModule(logicalDevice, full_screen_triangle_vert, &vertexModule);
        createShaderModule(logicalDevice, hud_preserve_frag, &fragmentModule);
        if (vertexModule != VK_NULL_HANDLE && fragmentModule != VK_NULL_HANDLE)
            pipeline = createGraphicsPipeline(logicalDevice, vertexModule, nullptr, "main",
                                              fragmentModule, nullptr, "main",
                                              imageExtent, renderPass, pipelineLayout);
        if (vertexModule != VK_NULL_HANDLE)
            logicalDevice->vkd.DestroyShaderModule(logicalDevice->device, vertexModule, nullptr);
        if (fragmentModule != VK_NULL_HANDLE)
            logicalDevice->vkd.DestroyShaderModule(logicalDevice->device, fragmentModule, nullptr);
        if (pipeline == VK_NULL_HANDLE)
            return false;

        std::vector<VkImageView> scratchViews(imageCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < imageCount; ++i)
        {
            perImage[i].scratch = createSingleImage(logicalDevice, {imageExtent.width, imageExtent.height, 1}, format,
                                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, perImage[i].scratchMemory);
            if (perImage[i].scratch == VK_NULL_HANDLE)
                return false;
            perImage[i].scratchView = createSingleImageView(logicalDevice, format, perImage[i].scratch);
            if (perImage[i].scratchView == VK_NULL_HANDLE)
                return false;
            scratchViews[i] = perImage[i].scratchView;

            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &outputImageViews[i];
            framebufferInfo.width = imageExtent.width;
            framebufferInfo.height = imageExtent.height;
            framebufferInfo.layers = 1;
            if (logicalDevice->vkd.CreateFramebuffer(logicalDevice->device, &framebufferInfo, nullptr,
                                                     &perImage[i].framebuffer) != VK_SUCCESS)
                return false;
        }

        std::vector<VkDescriptorSet> sets = allocateAndWriteImageSamplerDescriptorSets(
            logicalDevice, descriptorPool, descriptorSetLayout,
            std::vector<VkSampler>{sampler, sampler, sampler},
            std::vector<std::vector<VkImageView>>{scratchViews, finalViews, sceneViews});
        if (sets.size() < imageCount)
            return false;
        for (uint32_t i = 0; i < imageCount; ++i)
            perImage[i].descriptorSet = sets[i];

        return true;
    }

    void HudPreserveEffect::applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer)
    {
        if (!initialized || imageIndex >= perImage.size() || imageIndex >= outputImages.size())
            return;

        static bool logged = false;
        if (!logged)
        {
            logged = true;
            Logger::info("HUD protection: preserve pass active");
        }

        PerImage& state = perImage[imageIndex];
        if (state.scratch == VK_NULL_HANDLE || state.scratchView == VK_NULL_HANDLE
            || state.framebuffer == VK_NULL_HANDLE || state.descriptorSet == VK_NULL_HANDLE)
            return;

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        barrier.image = outputImages[imageIndex];
        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0, 0, nullptr, 0, nullptr, 1, &barrier);

        barrier.image = state.scratch;
        barrier.oldLayout = state.scratchInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = state.scratchInitialized ? VK_ACCESS_SHADER_READ_BIT : 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkImageCopy copy{};
        copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.srcSubresource.mipLevel = 0;
        copy.srcSubresource.baseArrayLayer = 0;
        copy.srcSubresource.layerCount = 1;
        copy.dstSubresource = copy.srcSubresource;
        copy.extent = {imageExtent.width, imageExtent.height, 1};
        logicalDevice->vkd.CmdCopyImage(commandBuffer,
                                         outputImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         state.scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         1, &copy);
        state.scratchInitialized = true;

        barrier.image = state.scratch;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                               0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkRenderPassBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = renderPass;
        begin.framebuffer = state.framebuffer;
        begin.renderArea.offset = {0, 0};
        begin.renderArea.extent = imageExtent;
        logicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
        logicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        logicalDevice->vkd.CmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                                  0, 1, &state.descriptorSet, 0, nullptr);
        logicalDevice->vkd.CmdDraw(commandBuffer, 3, 1, 0, 0);
        logicalDevice->vkd.CmdEndRenderPass(commandBuffer);
    }

    HudPreserveEffect::~HudPreserveEffect()
    {
        releaseResources();
    }

    void HudPreserveEffect::releaseResources()
    {
        if (!logicalDevice)
            return;

        std::vector<VkFramebuffer> framebuffers;
        std::vector<VkImageView> scratchViews;
        std::vector<VkImage> scratchImages;
        std::vector<VkDeviceMemory> scratchMemories;
        for (PerImage& state : perImage)
        {
            if (state.framebuffer != VK_NULL_HANDLE)
                framebuffers.push_back(state.framebuffer);
            if (state.scratchView != VK_NULL_HANDLE)
                scratchViews.push_back(state.scratchView);
            if (state.scratch != VK_NULL_HANDLE)
                scratchImages.push_back(state.scratch);
            if (state.scratchMemory != VK_NULL_HANDLE)
                scratchMemories.push_back(state.scratchMemory);
            state = {};
        }
        perImage.clear();

        if (renderPass == VK_NULL_HANDLE && pipeline == VK_NULL_HANDLE && descriptorPool == VK_NULL_HANDLE
            && descriptorSetLayout == VK_NULL_HANDLE && sampler == VK_NULL_HANDLE && framebuffers.empty()
            && scratchImages.empty())
        {
            initialized = false;
            return;
        }

        auto& queue = DeferredDestroyQueue::instance();
        queue.push(DestroyPhase::Resource, [vkd = logicalDevice->vkd,
                                            device = logicalDevice->device,
                                            descriptorPool = descriptorPool,
                                            pipeline = pipeline,
                                            pipelineLayout = pipelineLayout,
                                            renderPass = renderPass,
                                            sampler = sampler,
                                            descriptorSetLayout = descriptorSetLayout,
                                            framebuffers,
                                            scratchViews,
                                            scratchImages,
                                            scratchMemories]() mutable {
            if (descriptorPool != VK_NULL_HANDLE)
                vkd.DestroyDescriptorPool(device, descriptorPool, nullptr);
            if (pipeline != VK_NULL_HANDLE)
                vkd.DestroyPipeline(device, pipeline, nullptr);
            if (pipelineLayout != VK_NULL_HANDLE)
                vkd.DestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (renderPass != VK_NULL_HANDLE)
                vkd.DestroyRenderPass(device, renderPass, nullptr);
            for (VkFramebuffer framebuffer : framebuffers)
                vkd.DestroyFramebuffer(device, framebuffer, nullptr);
            for (VkImageView view : scratchViews)
                vkd.DestroyImageView(device, view, nullptr);
            for (VkImage image : scratchImages)
                vkd.DestroyImage(device, image, nullptr);
            for (VkDeviceMemory memory : scratchMemories)
                vkd.FreeMemory(device, memory, nullptr);
            if (sampler != VK_NULL_HANDLE)
                vkd.DestroySampler(device, sampler, nullptr);
            if (descriptorSetLayout != VK_NULL_HANDLE)
                vkd.DestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
        });

        renderPass = VK_NULL_HANDLE;
        pipeline = VK_NULL_HANDLE;
        pipelineLayout = VK_NULL_HANDLE;
        descriptorPool = VK_NULL_HANDLE;
        descriptorSetLayout = VK_NULL_HANDLE;
        sampler = VK_NULL_HANDLE;
        initialized = false;
    }
}
