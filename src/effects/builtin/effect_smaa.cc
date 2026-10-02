#include "effect_smaa.hh"

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
#include "image.hh"
#include "util.hh"

#include "AreaTex.h"
#include "SearchTex.h"
#include "shader_sources.hh"

namespace VKIntox
{
    SmaaEffect::SmaaEffect(LogicalDevice*       pLogicalDevice,
                           VkFormat             format,
                           VkExtent2D           imageExtent,
                           std::vector<VkImage> inputImages,
                           std::vector<VkImage> outputImages,
                           Config*              pConfig)
    {
        Logger::debug("in creating SmaaEffect");

        this->pLogicalDevice = pLogicalDevice;
        this->format         = format;
        this->imageExtent    = imageExtent;
        this->inputImages    = inputImages;
        this->outputImages   = outputImages;
        this->pConfig        = pConfig;

        // create Images for the first and second pass at once -> less memory fragmentation
        std::vector<VkImage> edgeAndBlendImages = createImages(pLogicalDevice,
                                                               inputImages.size() * 2,
                                                               {imageExtent.width, imageExtent.height, 1},
                                                               VK_FORMAT_B8G8R8A8_UNORM, // TODO search for format and save it
                                                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                               imageMemory);

        edgeImages  = std::vector<VkImage>(edgeAndBlendImages.begin(), edgeAndBlendImages.begin() + edgeAndBlendImages.size() / 2);
        blendImages = std::vector<VkImage>(edgeAndBlendImages.begin() + edgeAndBlendImages.size() / 2, edgeAndBlendImages.end());

        inputImageViews = createImageViews(pLogicalDevice, format, inputImages);
        Logger::debug("created input ImageViews");
        edgeImageViews = createImageViews(pLogicalDevice, VK_FORMAT_B8G8R8A8_UNORM, edgeImages);
        Logger::debug("created edge  ImageViews");
        blendImageViews = createImageViews(pLogicalDevice, VK_FORMAT_B8G8R8A8_UNORM, blendImages);
        Logger::debug("created blend ImageViews");
        outputImageViews = createImageViews(pLogicalDevice, format, outputImages);
        Logger::debug("created output ImageViews");
        sampler = createSampler(pLogicalDevice);
        Logger::debug("created sampler");

        VkExtent3D areaImageExtent = {AREATEX_WIDTH, AREATEX_HEIGHT, 1};

        areaImage = createSingleImage(pLogicalDevice,
                                 areaImageExtent,
                                 VK_FORMAT_R8G8_UNORM, // TODO search for format and save it
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                 areaMemory);

        VkExtent3D searchImageExtent = {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1};

        searchImage = createSingleImage(pLogicalDevice,
                                   searchImageExtent,
                                   VK_FORMAT_R8_UNORM, // TODO search for format and save it
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                   searchMemory);

        uploadToImage(pLogicalDevice, areaImage, areaImageExtent, AREATEX_SIZE, areaTexBytes);

        uploadToImage(pLogicalDevice, searchImage, searchImageExtent, SEARCHTEX_SIZE, searchTexBytes);

        areaImageView = createSingleImageView(pLogicalDevice, VK_FORMAT_R8G8_UNORM, areaImage);
        Logger::debug("after creating area ImageView");
        searchImageView = createSingleImageView(pLogicalDevice, VK_FORMAT_R8_UNORM, searchImage);
        Logger::debug("created search ImageView");

        imageSamplerDescriptorSetLayout = createImageSamplerDescriptorSetLayout(pLogicalDevice, 5);
        Logger::debug("created descriptorSetLayouts");

        VkDescriptorPoolSize imagePoolSize;
        imagePoolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        imagePoolSize.descriptorCount = inputImages.size() * 5;

        std::vector<VkDescriptorPoolSize> poolSizes = {imagePoolSize};

        descriptorPool = createDescriptorPool(pLogicalDevice, poolSizes);
        Logger::debug("created descriptorPool");

        // get config options
        struct SmaaOptions
        {
            float   screenWidth;
            float   screenHeight;
            float   reverseScreenWidth;
            float   reverseScreenHeight;
            float   threshold;
            int32_t maxSearchSteps;
            int32_t maxSearchStepsDiag;
            int32_t cornerRounding;
        };

        SmaaOptions smaaOptions;
        smaaOptions.threshold          = pConfig->getOption<float>("smaaThreshold", 0.05f);
        smaaOptions.maxSearchSteps     = pConfig->getOption<int32_t>("smaaMaxSearchSteps", 32);
        smaaOptions.maxSearchStepsDiag = pConfig->getOption<int32_t>("smaaMaxSearchStepsDiag", 16);
        smaaOptions.cornerRounding     = pConfig->getOption<int32_t>("smaaCornerRounding", 25);

        createShaderModule(pLogicalDevice, smaa_edge_vert, &edgeVertexModule);

        bool useColor = pConfig->getOption<std::string>("smaaEdgeDetection", "luma") == "color";

        auto shaderCode = useColor ? smaa_edge_color_frag : smaa_edge_luma_frag;
        createShaderModule(pLogicalDevice, shaderCode, &edgeFragmentModule);

        createShaderModule(pLogicalDevice, smaa_blend_vert, &blendVertexModule);

        createShaderModule(pLogicalDevice, smaa_blend_frag, &blendFragmentModule);

        createShaderModule(pLogicalDevice, smaa_neighbor_vert, &neighborVertexModule);

        createShaderModule(pLogicalDevice, smaa_neighbor_frag, &neignborFragmentModule);

        renderPass      = createRenderPass(pLogicalDevice, format);
        unormRenderPass = createRenderPass(pLogicalDevice, VK_FORMAT_B8G8R8A8_UNORM);

        std::vector<VkDescriptorSetLayout> descriptorSetLayouts = {imageSamplerDescriptorSetLayout};
        pipelineLayout                                          = createGraphicsPipelineLayout(pLogicalDevice, descriptorSetLayouts);

        std::vector<VkSpecializationMapEntry> specMapEntrys(8);
        for (uint32_t i = 0; i < specMapEntrys.size(); i++)
        {
            specMapEntrys[i].constantID = i;
            specMapEntrys[i].offset     = sizeof(float) * i; // TODO not clean to assume that sizeof(int32_t) == sizeof(float)
            specMapEntrys[i].size       = sizeof(float);
        }
        smaaOptions.screenWidth = (float) imageExtent.width, smaaOptions.screenHeight = (float) imageExtent.height,
        smaaOptions.reverseScreenWidth  = 1.0f / imageExtent.width;
        smaaOptions.reverseScreenHeight = 1.0f / imageExtent.height;

        VkSpecializationInfo specializationInfo;
        specializationInfo.mapEntryCount = specMapEntrys.size();
        specializationInfo.pMapEntries   = specMapEntrys.data();
        specializationInfo.dataSize      = sizeof(smaaOptions);
        specializationInfo.pData         = &smaaOptions;

        edgePipeline = createGraphicsPipeline(pLogicalDevice,
                                              edgeVertexModule,
                                              &specializationInfo,
                                              "main",
                                              edgeFragmentModule,
                                              &specializationInfo,
                                              "main",
                                              imageExtent,
                                              unormRenderPass,
                                              pipelineLayout);

        blendPipeline = createGraphicsPipeline(pLogicalDevice,
                                               blendVertexModule,
                                               &specializationInfo,
                                               "main",
                                               blendFragmentModule,
                                               &specializationInfo,
                                               "main",
                                               imageExtent,
                                               unormRenderPass,
                                               pipelineLayout);

        neighborPipeline = createGraphicsPipeline(pLogicalDevice,
                                                  neighborVertexModule,
                                                  &specializationInfo,
                                                  "main",
                                                  neignborFragmentModule,
                                                  &specializationInfo,
                                                  "main",
                                                  imageExtent,
                                                  renderPass,
                                                  pipelineLayout);

        std::vector<std::vector<VkImageView>> imageViewsVector = {inputImageViews,
                                                                  edgeImageViews,
                                                                  std::vector<VkImageView>(inputImageViews.size(), areaImageView),
                                                                  std::vector<VkImageView>(inputImageViews.size(), searchImageView),
                                                                  blendImageViews};

        imageDescriptorSets = allocateAndWriteImageSamplerDescriptorSets(pLogicalDevice,
                                                                         descriptorPool,
                                                                         imageSamplerDescriptorSetLayout,
                                                                         std::vector<VkSampler>(imageViewsVector.size(), sampler),
                                                                         imageViewsVector);

        edgeFramebuffers     = createFramebuffers(pLogicalDevice, unormRenderPass, imageExtent, {edgeImageViews});
        blendFramebuffers    = createFramebuffers(pLogicalDevice, unormRenderPass, imageExtent, {blendImageViews});
        neignborFramebuffers = createFramebuffers(pLogicalDevice, renderPass, imageExtent, {outputImageViews});
    }
    void SmaaEffect::applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer)
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
        renderPassBeginInfo.renderPass        = unormRenderPass;
        renderPassBeginInfo.framebuffer       = edgeFramebuffers[imageIndex];
        renderPassBeginInfo.renderArea.offset = {0, 0};
        renderPassBeginInfo.renderArea.extent = imageExtent;
        VkClearValue clearValue               = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
        renderPassBeginInfo.clearValueCount   = 1;
        renderPassBeginInfo.pClearValues      = &clearValue;
        // edge renderPass
        pLogicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

        pLogicalDevice->vkd.CmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &(imageDescriptorSets[imageIndex]), 0, nullptr);

        pLogicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, edgePipeline);

        pLogicalDevice->vkd.CmdDraw(commandBuffer, 3, 1, 0, 0);

        pLogicalDevice->vkd.CmdEndRenderPass(commandBuffer);

        memoryBarrier.image             = edgeImages[imageIndex];
        renderPassBeginInfo.framebuffer = blendFramebuffers[imageIndex];
        // blend renderPass
        pLogicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

        // blend renderPass
        pLogicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

        pLogicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, blendPipeline);

        pLogicalDevice->vkd.CmdDraw(commandBuffer, 3, 1, 0, 0);

        pLogicalDevice->vkd.CmdEndRenderPass(commandBuffer);

        memoryBarrier.image             = blendImages[imageIndex];
        renderPassBeginInfo.framebuffer = neignborFramebuffers[imageIndex];
        renderPassBeginInfo.renderPass  = renderPass;
        // neighbor renderPass
        pLogicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

        // neighbor renderPass
        pLogicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

        pLogicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, neighborPipeline);

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
    SmaaEffect::~SmaaEffect()
    {
        Logger::debug("destroying smaa effect " + convertToString(this));

        // Skip cleanup if construction never assigned a device.
        if (!pLogicalDevice || pLogicalDevice->device == VK_NULL_HANDLE)
            return;

        auto& queue   = DeferredDestroyQueue::instance();
        auto  device = pLogicalDevice->device;
        auto& vkd    = pLogicalDevice->vkd;

        const VkPipeline           edgePipe = edgePipeline;
        const VkPipeline           blendPipe = blendPipeline;
        const VkPipeline           neighborPipe = neighborPipeline;
        const VkPipelineLayout     pipeLay  = pipelineLayout;
        const VkRenderPass         pass     = renderPass;
        const VkRenderPass         unormPass = unormRenderPass;
        const VkDescriptorSetLayout dsLayout = imageSamplerDescriptorSetLayout;
        const VkDescriptorPool     dsPool   = descriptorPool;
        const VkShaderModule       ev = edgeVertexModule,       ef = edgeFragmentModule;
        const VkShaderModule       bv = blendVertexModule,     bf = blendFragmentModule;
        const VkShaderModule       nv = neighborVertexModule,  nf = neignborFragmentModule;
        const VkSampler            smp = sampler;
        const VkDeviceMemory       imgMem = imageMemory, areaMem = areaMemory, searchMem = searchMemory;

        // Memory first so it is released last of everything, then images before
        // the views onto them, so reverse-registration within the phase unwinds
        // views -> images -> memory.
        if (imgMem != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Memory, [vkd, device, imgMem] { vkd.FreeMemory(device, imgMem, nullptr); });
        if (areaMem != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Memory, [vkd, device, areaMem] { vkd.FreeMemory(device, areaMem, nullptr); });
        if (searchMem != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Memory, [vkd, device, searchMem] { vkd.FreeMemory(device, searchMem, nullptr); });

        const VkImage areaImg = areaImage, searchImg = searchImage;
        for (auto image : edgeImages)
            if (image != VK_NULL_HANDLE)
                queue.push(DestroyPhase::Resource, [vkd, device, image] { vkd.DestroyImage(device, image, nullptr); });
        for (auto image : blendImages)
            if (image != VK_NULL_HANDLE)
                queue.push(DestroyPhase::Resource, [vkd, device, image] { vkd.DestroyImage(device, image, nullptr); });
        if (areaImg != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [vkd, device, areaImg] { vkd.DestroyImage(device, areaImg, nullptr); });
        if (searchImg != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [vkd, device, searchImg] { vkd.DestroyImage(device, searchImg, nullptr); });

        for (auto view : inputImageViews)
            if (view != VK_NULL_HANDLE)
                queue.push(DestroyPhase::Resource, [vkd, device, view] { vkd.DestroyImageView(device, view, nullptr); });
        for (auto view : edgeImageViews)
            if (view != VK_NULL_HANDLE)
                queue.push(DestroyPhase::Resource, [vkd, device, view] { vkd.DestroyImageView(device, view, nullptr); });
        for (auto view : blendImageViews)
            if (view != VK_NULL_HANDLE)
                queue.push(DestroyPhase::Resource, [vkd, device, view] { vkd.DestroyImageView(device, view, nullptr); });
        for (auto view : outputImageViews)
            if (view != VK_NULL_HANDLE)
                queue.push(DestroyPhase::Resource, [vkd, device, view] { vkd.DestroyImageView(device, view, nullptr); });
        const VkImageView areaView = areaImageView, searchView = searchImageView;
        if (areaView != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [vkd, device, areaView] { vkd.DestroyImageView(device, areaView, nullptr); });
        if (searchView != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [vkd, device, searchView] { vkd.DestroyImageView(device, searchView, nullptr); });

        for (auto module : {ev, ef, bv, bf, nv, nf})
            if (module != VK_NULL_HANDLE)
                queue.push(DestroyPhase::Resource, [vkd, device, module] { vkd.DestroyShaderModule(device, module, nullptr); });
        if (smp != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [vkd, device, smp] { vkd.DestroySampler(device, smp, nullptr); });

        // The pipeline layout names imageSamplerDescriptorSetLayout, so it is
        // pushed last within the phase and therefore released first.
        if (dsLayout != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Layout, [vkd, device, dsLayout] { vkd.DestroyDescriptorSetLayout(device, dsLayout, nullptr); });
        if (pipeLay != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Layout, [vkd, device, pipeLay] { vkd.DestroyPipelineLayout(device, pipeLay, nullptr); });

        if (dsPool != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Descriptor, [vkd, device, dsPool] { vkd.DestroyDescriptorPool(device, dsPool, nullptr); });

        // Framebuffers before the passes they were created from. Each vector is
        // walked on its own: the original loop drove eight of them off
        // edgeFramebuffers.size(), so any vector that ran short was indexed out
        // of bounds and every view past the end was leaked.
        for (auto framebuffer : edgeFramebuffers)
            if (framebuffer != VK_NULL_HANDLE)
                queue.push(DestroyPhase::RenderPass, [vkd, device, framebuffer] { vkd.DestroyFramebuffer(device, framebuffer, nullptr); });
        for (auto framebuffer : blendFramebuffers)
            if (framebuffer != VK_NULL_HANDLE)
                queue.push(DestroyPhase::RenderPass, [vkd, device, framebuffer] { vkd.DestroyFramebuffer(device, framebuffer, nullptr); });
        for (auto framebuffer : neignborFramebuffers)
            if (framebuffer != VK_NULL_HANDLE)
                queue.push(DestroyPhase::RenderPass, [vkd, device, framebuffer] { vkd.DestroyFramebuffer(device, framebuffer, nullptr); });
        if (pass != VK_NULL_HANDLE)
            queue.push(DestroyPhase::RenderPass, [vkd, device, pass] { vkd.DestroyRenderPass(device, pass, nullptr); });
        if (unormPass != VK_NULL_HANDLE)
            queue.push(DestroyPhase::RenderPass, [vkd, device, unormPass] { vkd.DestroyRenderPass(device, unormPass, nullptr); });

        for (auto pipeline : {edgePipe, blendPipe, neighborPipe})
            if (pipeline != VK_NULL_HANDLE)
                queue.push(DestroyPhase::Pipeline, [vkd, device, pipeline] { vkd.DestroyPipeline(device, pipeline, nullptr); });
    }
} // namespace VKIntox
