#include "effect_simple.hh"

#include "vk_handle.hh"

#include <cstring>

#include "image_view.hh"
#include "descriptor_set.hh"
#include "buffer.hh"
#include "renderpass.hh"
#include "graphics_pipeline.hh"
#include "framebuffer.hh"
#include "shader.hh"
#include "sampler.hh"
#include "util.hh"

namespace VKIntox
{
    SimpleEffect::SimpleEffect()
    {
    }
    void SimpleEffect::init(LogicalDevice*       pLogicalDevice,
                            VkFormat             format,
                            VkExtent2D           imageExtent,
                            std::vector<VkImage> inputImages,
                            std::vector<VkImage> outputImages,
                            Config*              pConfig)
    {
        Logger::debug("in creating SimpleEffect");

        this->pLogicalDevice = pLogicalDevice;
        this->format         = format;
        this->imageExtent    = imageExtent;
        this->inputImages    = inputImages;
        this->outputImages   = outputImages;
        this->pConfig        = pConfig;

        inputImageViews = createImageViews(pLogicalDevice, format, inputImages);
        Logger::debug("created input ImageViews");
        outputImageViews = createImageViews(pLogicalDevice, format, outputImages);
        Logger::debug("created ImageViews");
        sampler = createSampler(pLogicalDevice);
        Logger::debug("created sampler");

        imageSamplerDescriptorSetLayout = createImageSamplerDescriptorSetLayout(pLogicalDevice, 1);
        Logger::debug("created descriptorSetLayouts");

        VkDescriptorPoolSize imagePoolSize;
        imagePoolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        imagePoolSize.descriptorCount = inputImages.size() + 10;

        std::vector<VkDescriptorPoolSize> poolSizes = {imagePoolSize};

        descriptorPool = createDescriptorPool(pLogicalDevice, poolSizes);
        Logger::debug("created descriptorPool");

        createShaderModule(pLogicalDevice, vertexCode, &vertexModule);
        createShaderModule(pLogicalDevice, fragmentCode, &fragmentModule);

        renderPass = createRenderPass(pLogicalDevice, format);

        descriptorSetLayouts.insert(descriptorSetLayouts.begin(), imageSamplerDescriptorSetLayout);
        pipelineLayout = createGraphicsPipelineLayout(pLogicalDevice, descriptorSetLayouts);

        graphicsPipeline = createGraphicsPipeline(pLogicalDevice,
                                                  vertexModule,
                                                  pVertexSpecInfo,
                                                  "main",
                                                  fragmentModule,
                                                  pFragmentSpecInfo,
                                                  "main",
                                                  imageExtent,
                                                  renderPass,
                                                  pipelineLayout);

        imageDescriptorSets = allocateAndWriteImageSamplerDescriptorSets(
            pLogicalDevice, descriptorPool, imageSamplerDescriptorSetLayout, {sampler}, std::vector<std::vector<VkImageView>>(1, inputImageViews));

        framebuffers = createFramebuffers(pLogicalDevice, renderPass, imageExtent, {outputImageViews});
    }
    void SimpleEffect::applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer)
    {
        // Used to make the Image accessable by the shader
        VkImageMemoryBarrier memoryBarrier;
        memoryBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        memoryBarrier.pNext               = nullptr;
        memoryBarrier.srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT;
        memoryBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        memoryBarrier.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        memoryBarrier.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.image               = inputImages[imageIndex];

        memoryBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        memoryBarrier.subresourceRange.baseMipLevel   = 0;
        memoryBarrier.subresourceRange.levelCount     = 1;
        memoryBarrier.subresourceRange.baseArrayLayer = 0;
        memoryBarrier.subresourceRange.layerCount     = 1;

        // Reverses the first Barrier
        VkImageMemoryBarrier secondBarrier;
        secondBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        secondBarrier.pNext               = nullptr;
        secondBarrier.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        secondBarrier.dstAccessMask       = 0;
        secondBarrier.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        secondBarrier.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        secondBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        secondBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        secondBarrier.image               = inputImages[imageIndex];

        secondBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        secondBarrier.subresourceRange.baseMipLevel   = 0;
        secondBarrier.subresourceRange.levelCount     = 1;
        secondBarrier.subresourceRange.baseArrayLayer = 0;
        secondBarrier.subresourceRange.layerCount     = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

        VkRenderPassBeginInfo renderPassBeginInfo;
        renderPassBeginInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBeginInfo.pNext             = nullptr;
        renderPassBeginInfo.renderPass        = renderPass;
        renderPassBeginInfo.framebuffer       = framebuffers[imageIndex];
        renderPassBeginInfo.renderArea.offset = {0, 0};
        renderPassBeginInfo.renderArea.extent = imageExtent;
        VkClearValue clearValue               = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
        renderPassBeginInfo.clearValueCount   = 1;
        renderPassBeginInfo.pClearValues      = &clearValue;

        pLogicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

        pLogicalDevice->vkd.CmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &(imageDescriptorSets[imageIndex]), 0, nullptr);

        pLogicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);

        pLogicalDevice->vkd.CmdDraw(commandBuffer, 3, 1, 0, 0);

        pLogicalDevice->vkd.CmdEndRenderPass(commandBuffer);

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &secondBarrier);
    }
    SimpleEffect::~SimpleEffect()
    {
        Logger::debug("destroying SimpleEffect " + convertToString(this));

        // Skip cleanup if init() was never called (e.g., constructor threw exception)
        if (!pLogicalDevice || pLogicalDevice->device == VK_NULL_HANDLE)
            return;

        // Deferred like every other owner, so nothing is destroyed at scope exit
        // while the GPU may still be reading it.
        auto& queue   = DeferredDestroyQueue::instance();
        auto  device = pLogicalDevice->device;
        auto& vkd    = pLogicalDevice->vkd;

        const VkPipeline           pipeline    = graphicsPipeline;
        const VkPipelineLayout     pipelineLay = pipelineLayout;
        const VkRenderPass         pass        = renderPass;
        const VkDescriptorSetLayout dsLayout    = imageSamplerDescriptorSetLayout;
        const VkDescriptorPool     dsPool      = descriptorPool;
        const VkShaderModule       vert        = vertexModule;
        const VkShaderModule       frag        = fragmentModule;
        const VkSampler            smp         = sampler;

        // Pipeline before its layout, and the layout before the set layouts it
        // names, which the phase ordering already guarantees; within Layout the
        // reverse-registration rule means the pipeline layout must be pushed last.
        if (dsLayout != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Layout, [vkd, device, dsLayout] { vkd.DestroyDescriptorSetLayout(device, dsLayout, nullptr); });
        if (pipelineLay != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Layout, [vkd, device, pipelineLay] { vkd.DestroyPipelineLayout(device, pipelineLay, nullptr); });

        if (dsPool != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Descriptor, [vkd, device, dsPool] { vkd.DestroyDescriptorPool(device, dsPool, nullptr); });

        // Framebuffers reference the render pass and the image views, so they go
        // in the RenderPass phase, which the queue releases before Layout and
        // Resource. The old destructor relied on them merely being adjacent lines.
        for (auto framebuffer : framebuffers)
        {
            if (framebuffer != VK_NULL_HANDLE)
                queue.push(DestroyPhase::RenderPass, [vkd, device, framebuffer] { vkd.DestroyFramebuffer(device, framebuffer, nullptr); });
        }
        if (pass != VK_NULL_HANDLE)
            queue.push(DestroyPhase::RenderPass, [vkd, device, pass] { vkd.DestroyRenderPass(device, pass, nullptr); });

        if (pipeline != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Pipeline, [vkd, device, pipeline] { vkd.DestroyPipeline(device, pipeline, nullptr); });

        if (vert != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [vkd, device, vert] { vkd.DestroyShaderModule(device, vert, nullptr); });
        if (frag != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [vkd, device, frag] { vkd.DestroyShaderModule(device, frag, nullptr); });
        if (smp != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [vkd, device, smp] { vkd.DestroySampler(device, smp, nullptr); });

        // These were walked in a single loop alongside framebuffers, indexed by
        // the same i, so a framebuffers vector shorter than either view vector
        // walked off the end of it. Nothing guaranteed the three stay in step --
        // framebuffers are built per swapchain image while the view vectors come
        // from the caller. Walking each vector on its own removes the coupling
        // and releases views that a short framebuffers vector would have leaked.
        for (auto view : inputImageViews)
        {
            if (view != VK_NULL_HANDLE)
                queue.push(DestroyPhase::Resource, [vkd, device, view] { vkd.DestroyImageView(device, view, nullptr); });
        }
        for (auto view : outputImageViews)
        {
            if (view != VK_NULL_HANDLE)
                queue.push(DestroyPhase::Resource, [vkd, device, view] { vkd.DestroyImageView(device, view, nullptr); });
        }
    }
} // namespace VKIntox
