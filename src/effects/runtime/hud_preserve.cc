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
    namespace
    {
        VkRenderPass createCompositePass(LogicalDevice* device, VkFormat format, bool loadExisting)
        {
            VkAttachmentDescription attachment{};
            attachment.format = format;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = loadExisting ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = loadExisting ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
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
            dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependencies[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT
                | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dependencies[1].srcSubpass = 0;
            dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;

            VkRenderPassCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            createInfo.attachmentCount = 1;
            createInfo.pAttachments = &attachment;
            createInfo.subpassCount = 1;
            createInfo.pSubpasses = &subpass;
            createInfo.dependencyCount = 2;
            createInfo.pDependencies = dependencies;

            VkRenderPass renderPass = VK_NULL_HANDLE;
            if (device->vkd.CreateRenderPass(device->device, &createInfo, nullptr, &renderPass) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            return renderPass;
        }

        VkSampler createHudSampler(LogicalDevice* device)
        {
            VkSamplerCreateInfo samplerInfo{};
            samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.maxLod = 0.0f;
            VkSampler sampler = VK_NULL_HANDLE;
            if (device->vkd.CreateSampler(device->device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            return sampler;
        }

        std::vector<VkFramebuffer> createHudFramebuffers(LogicalDevice* device, VkRenderPass renderPass,
                                                          VkExtent2D extent, const std::vector<VkImageView>& views)
        {
            std::vector<VkFramebuffer> result(views.size(), VK_NULL_HANDLE);
            for (size_t i = 0; i < views.size(); ++i)
            {
                VkFramebufferCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                info.renderPass = renderPass;
                info.attachmentCount = 1;
                info.pAttachments = &views[i];
                info.width = extent.width;
                info.height = extent.height;
                info.layers = 1;
                if (device->vkd.CreateFramebuffer(device->device, &info, nullptr, &result[i]) != VK_SUCCESS)
                    return {};
            }
            return result;
        }
    }

    HudPreserveEffect::HudPreserveEffect(LogicalDevice*           logicalDevice,
                                         VkFormat                 format,
                                         VkExtent2D               imageExtent,
                                         std::vector<VkImage>     outputImages,
                                         std::vector<VkImageView> outputImageViews,
                                         std::vector<VkImage>     processedImages,
                                         std::vector<VkImageView> processedViews,
                                         std::vector<VkImageView> sceneViews,
                                         std::vector<VkImageView> finalViews,
                                         std::vector<VkImageView> validViews,
                                         uint32_t                 imageCount)
        : logicalDevice(logicalDevice),
          format(format),
          imageExtent(imageExtent),
          outputImages(std::move(outputImages)),
          outputImageViews(std::move(outputImageViews)),
          processedImages(std::move(processedImages)),
          processedViews(std::move(processedViews)),
          sceneViews(std::move(sceneViews)),
          finalViews(std::move(finalViews)),
          validViews(std::move(validViews)),
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
            || processedImages.size() < imageCount || processedViews.size() < imageCount
            || sceneViews.size() < imageCount || finalViews.size() < imageCount || validViews.size() < imageCount)
            return false;

        descriptorSetLayout = createImageSamplerDescriptorSetLayout(logicalDevice, 4);
        if (descriptorSetLayout == VK_NULL_HANDLE)
            return false;

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = imageCount * 4;
        descriptorPool = createDescriptorPool(logicalDevice, {poolSize});
        if (descriptorPool == VK_NULL_HANDLE)
            return false;

        sampler = createHudSampler(logicalDevice);
        if (sampler == VK_NULL_HANDLE)
            return false;

        renderPass = createCompositePass(logicalDevice, format, false);
        if (renderPass == VK_NULL_HANDLE)
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

        framebuffers = createHudFramebuffers(logicalDevice, renderPass, imageExtent, outputImageViews);
        if (framebuffers.size() != imageCount)
            return false;

        std::vector<VkDescriptorSet> sets = allocateAndWriteImageSamplerDescriptorSets(
            logicalDevice, descriptorPool, descriptorSetLayout,
            std::vector<VkSampler>{sampler, sampler, sampler, sampler},
            std::vector<std::vector<VkImageView>>{processedViews, finalViews, sceneViews, validViews});
        if (sets.size() < imageCount)
            return false;
        descriptorSets.assign(sets.begin(), sets.begin() + imageCount);

        return true;
    }

    void HudPreserveEffect::updateProcessedViews(const std::vector<VkImage>& images,
                                                 const std::vector<VkImageView>& views)
    {
        if (!initialized || images.size() < imageCount || views.size() < imageCount
            || descriptorSets.size() < imageCount)
            return;

        processedImages = images;
        processedViews = views;
        for (uint32_t i = 0; i < imageCount; ++i)
        {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.sampler = sampler;
            imageInfo.imageView = processedViews[i];
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptorSets[i];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfo;
            logicalDevice->vkd.UpdateDescriptorSets(logicalDevice->device, 1, &write, 0, nullptr);
        }
    }

    void HudPreserveEffect::applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer)
    {
        if (!initialized || imageIndex >= framebuffers.size() || imageIndex >= processedImages.size()
            || imageIndex >= descriptorSets.size())
            return;

        static bool logged = false;
        if (!logged)
        {
            logged = true;
            Logger::info("HUD protection: preserve pass active");
        }

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        barrier.image = processedImages[imageIndex];
        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                               0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkRenderPassBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = renderPass;
        begin.framebuffer = framebuffers[imageIndex];
        begin.renderArea.offset = {0, 0};
        begin.renderArea.extent = imageExtent;
        logicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
        logicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        logicalDevice->vkd.CmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                                  0, 1, &descriptorSets[imageIndex], 0, nullptr);
        logicalDevice->vkd.CmdDraw(commandBuffer, 3, 1, 0, 0);
        logicalDevice->vkd.CmdEndRenderPass(commandBuffer);

        barrier.image = processedImages[imageIndex];
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = 0;
        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                               0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    HudPreserveEffect::~HudPreserveEffect()
    {
        releaseResources();
    }

    void HudPreserveEffect::releaseResources()
    {
        if (!logicalDevice)
            return;

        auto& queue = DeferredDestroyQueue::instance();
        for (VkFramebuffer framebuffer : framebuffers)
        {
            if (framebuffer != VK_NULL_HANDLE)
                queue.push(DestroyPhase::RenderPass,
                           [vkd = logicalDevice->vkd, device = logicalDevice->device, framebuffer]() {
                               vkd.DestroyFramebuffer(device, framebuffer, nullptr);
                           });
        }
        framebuffers.clear();

        if (descriptorPool != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Descriptor,
                       [vkd = logicalDevice->vkd, device = logicalDevice->device, pool = descriptorPool]() {
                           vkd.DestroyDescriptorPool(device, pool, nullptr);
                       });
        if (pipeline != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Pipeline,
                       [vkd = logicalDevice->vkd, device = logicalDevice->device, pipeline = pipeline]() {
                           vkd.DestroyPipeline(device, pipeline, nullptr);
                       });
        if (pipelineLayout != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Layout,
                       [vkd = logicalDevice->vkd, device = logicalDevice->device, layout = pipelineLayout]() {
                           vkd.DestroyPipelineLayout(device, layout, nullptr);
                       });
        if (renderPass != VK_NULL_HANDLE)
            queue.push(DestroyPhase::RenderPass,
                       [vkd = logicalDevice->vkd, device = logicalDevice->device, pass = renderPass]() {
                           vkd.DestroyRenderPass(device, pass, nullptr);
                       });
        if (descriptorSetLayout != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Layout,
                       [vkd = logicalDevice->vkd, device = logicalDevice->device, layout = descriptorSetLayout]() {
                           vkd.DestroyDescriptorSetLayout(device, layout, nullptr);
                       });
        if (sampler != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource,
                       [vkd = logicalDevice->vkd, device = logicalDevice->device, sampler = sampler]() {
                           vkd.DestroySampler(device, sampler, nullptr);
                       });

        descriptorSets.clear();
        initialized = false;
        renderPass = VK_NULL_HANDLE;
        pipelineLayout = VK_NULL_HANDLE;
        pipeline = VK_NULL_HANDLE;
        descriptorSetLayout = VK_NULL_HANDLE;
        descriptorPool = VK_NULL_HANDLE;
        sampler = VK_NULL_HANDLE;
    }

    HudSceneInputEffect::HudSceneInputEffect(LogicalDevice*           logicalDevice,
                                             VkFormat                 format,
                                             VkExtent2D               imageExtent,
                                             std::vector<VkImageView> outputImageViews,
                                             std::vector<VkImageView> sceneViews,
                                             std::vector<VkImageView> validViews,
                                             std::vector<VkImage>     appImages,
                                             std::vector<VkImage>     presentFinalImages,
                                             uint32_t                 imageCount)
        : logicalDevice(logicalDevice),
          format(format),
          imageExtent(imageExtent),
          outputImageViews(std::move(outputImageViews)),
          sceneViews(std::move(sceneViews)),
          validViews(std::move(validViews)),
          appImages(std::move(appImages)),
          presentFinalImages(std::move(presentFinalImages)),
          imageCount(imageCount)
    {
        initialized = initialize();
        if (!initialized)
        {
            Logger::warn("HUD scene input pass unavailable; effects will see the HUD");
            releaseResources();
        }
    }

    bool HudSceneInputEffect::initialize()
    {
        if (!logicalDevice || format == VK_FORMAT_UNDEFINED || imageExtent.width == 0 || imageExtent.height == 0)
            return false;
        if (imageCount == 0 || outputImageViews.size() < imageCount
            || sceneViews.size() < imageCount || validViews.size() < imageCount)
            return false;

        descriptorSetLayout = createImageSamplerDescriptorSetLayout(logicalDevice, 2);
        if (descriptorSetLayout == VK_NULL_HANDLE)
            return false;

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = imageCount * 2;
        descriptorPool = createDescriptorPool(logicalDevice, {poolSize});
        if (descriptorPool == VK_NULL_HANDLE)
            return false;

        sampler = createHudSampler(logicalDevice);
        if (sampler == VK_NULL_HANDLE)
            return false;

        renderPass = createCompositePass(logicalDevice, format, true);
        if (renderPass == VK_NULL_HANDLE)
            return false;

        pipelineLayout = createGraphicsPipelineLayout(logicalDevice, {descriptorSetLayout}, {});
        if (pipelineLayout == VK_NULL_HANDLE)
            return false;

        VkShaderModule vertexModule = VK_NULL_HANDLE;
        VkShaderModule fragmentModule = VK_NULL_HANDLE;
        createShaderModule(logicalDevice, full_screen_triangle_vert, &vertexModule);
        createShaderModule(logicalDevice, hud_scene_input_frag, &fragmentModule);
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

        framebuffers = createHudFramebuffers(logicalDevice, renderPass, imageExtent, outputImageViews);
        if (framebuffers.size() != imageCount)
            return false;

        std::vector<VkDescriptorSet> sets = allocateAndWriteImageSamplerDescriptorSets(
            logicalDevice, descriptorPool, descriptorSetLayout,
            std::vector<VkSampler>{sampler, sampler},
            std::vector<std::vector<VkImageView>>{sceneViews, validViews});
        if (sets.size() < imageCount)
            return false;
        descriptorSets.assign(sets.begin(), sets.begin() + imageCount);

        return true;
    }

    void HudSceneInputEffect::applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer)
    {
        if (!initialized || imageIndex >= framebuffers.size() || imageIndex >= descriptorSets.size())
            return;

        static bool logged = false;
        if (!logged)
        {
            logged = true;
            Logger::info("HUD protection: scene-only input active");
        }

        if (imageIndex < appImages.size() && imageIndex < presentFinalImages.size()
            && appImages[imageIndex] != VK_NULL_HANDLE && presentFinalImages[imageIndex] != VK_NULL_HANDLE)
        {
            auto barrier = [&](VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                               VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                               VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
                VkImageMemoryBarrier b{};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = image;
                b.oldLayout = oldLayout;
                b.newLayout = newLayout;
                b.srcAccessMask = srcAccess;
                b.dstAccessMask = dstAccess;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                logicalDevice->vkd.CmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0,
                                                      0, nullptr, 0, nullptr, 1, &b);
            };

            const VkImage app = appImages[imageIndex];
            const VkImage dst = presentFinalImages[imageIndex];
            barrier(app, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            barrier(dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstSubresource = copy.srcSubresource;
            copy.extent = {imageExtent.width, imageExtent.height, 1};
            logicalDevice->vkd.CmdCopyImage(commandBuffer, app, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                            dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            barrier(app, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }

        VkRenderPassBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = renderPass;
        begin.framebuffer = framebuffers[imageIndex];
        begin.renderArea.offset = {0, 0};
        begin.renderArea.extent = imageExtent;
        logicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
        logicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        logicalDevice->vkd.CmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                                  0, 1, &descriptorSets[imageIndex], 0, nullptr);
        logicalDevice->vkd.CmdDraw(commandBuffer, 3, 1, 0, 0);
        logicalDevice->vkd.CmdEndRenderPass(commandBuffer);
    }

    HudSceneInputEffect::~HudSceneInputEffect()
    {
        releaseResources();
    }

    void HudSceneInputEffect::releaseResources()
    {
        if (!logicalDevice)
            return;

        auto& queue = DeferredDestroyQueue::instance();
        for (VkFramebuffer framebuffer : framebuffers)
        {
            if (framebuffer != VK_NULL_HANDLE)
                queue.push(DestroyPhase::RenderPass,
                           [vkd = logicalDevice->vkd, device = logicalDevice->device, framebuffer]() {
                               vkd.DestroyFramebuffer(device, framebuffer, nullptr);
                           });
        }
        framebuffers.clear();

        if (descriptorPool != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Descriptor,
                       [vkd = logicalDevice->vkd, device = logicalDevice->device, pool = descriptorPool]() {
                           vkd.DestroyDescriptorPool(device, pool, nullptr);
                       });
        if (pipeline != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Pipeline,
                       [vkd = logicalDevice->vkd, device = logicalDevice->device, pipeline = pipeline]() {
                           vkd.DestroyPipeline(device, pipeline, nullptr);
                       });
        if (pipelineLayout != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Layout,
                       [vkd = logicalDevice->vkd, device = logicalDevice->device, layout = pipelineLayout]() {
                           vkd.DestroyPipelineLayout(device, layout, nullptr);
                       });
        if (renderPass != VK_NULL_HANDLE)
            queue.push(DestroyPhase::RenderPass,
                       [vkd = logicalDevice->vkd, device = logicalDevice->device, pass = renderPass]() {
                           vkd.DestroyRenderPass(device, pass, nullptr);
                       });
        if (descriptorSetLayout != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Layout,
                       [vkd = logicalDevice->vkd, device = logicalDevice->device, layout = descriptorSetLayout]() {
                           vkd.DestroyDescriptorSetLayout(device, layout, nullptr);
                       });
        if (sampler != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource,
                       [vkd = logicalDevice->vkd, device = logicalDevice->device, sampler = sampler]() {
                           vkd.DestroySampler(device, sampler, nullptr);
                       });

        descriptorSets.clear();
        initialized = false;
        renderPass = VK_NULL_HANDLE;
        pipelineLayout = VK_NULL_HANDLE;
        pipeline = VK_NULL_HANDLE;
        descriptorSetLayout = VK_NULL_HANDLE;
        descriptorPool = VK_NULL_HANDLE;
        sampler = VK_NULL_HANDLE;
    }
}
