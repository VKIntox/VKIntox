#include "effect_lut.hh"

#include "vk_handle.hh"

#include <cstring>
#include <stdexcept>

#include "image_view.hh"
#include "descriptor_set.hh"
#include "buffer.hh"
#include "renderpass.hh"
#include "graphics_pipeline.hh"
#include "framebuffer.hh"
#include "shader.hh"
#include "sampler.hh"
#include "image.hh"
#include "lut_cube.hh"

#include "stb_image.h"

#include "shader_sources.hh"

namespace VKIntox
{
    LutEffect::LutEffect(LogicalDevice*       pLogicalDevice,
                         VkFormat             format,
                         VkExtent2D           imageExtent,
                         std::vector<VkImage> inputImages,
                         std::vector<VkImage> outputImages,
                         Config*              pConfig)
    {
        vertexCode   = full_screen_triangle_vert;
        fragmentCode = lut_frag;

        std::string lutFile = pConfig->getOption<std::string>("lutFile");

        if (lutFile.empty())
        {
            throw std::runtime_error("LUT effect requires 'lutFile' to be set in config");
        }

        int      height = 0;
        LutCube  lutCube;
        stbi_uc* pixels = nullptr;
        int32_t  usingPNG = (int32_t)(lutFile.find(".cube") == std::string::npos && lutFile.find(".CUBE") == std::string::npos);
        if (!usingPNG)
        {
            lutCube = LutCube(lutFile);
            pixels  = lutCube.colorCube.data();
            height  = lutCube.size;
            if (height == 0 || pixels == nullptr)
            {
                throw std::runtime_error("Failed to load LUT cube file: " + lutFile);
            }
        }
        else
        {
            int channels, width;
            pixels = stbi_load(lutFile.c_str(), &width, &height, &channels, STBI_rgb_alpha);
            if (pixels == nullptr)
            {
                throw std::runtime_error("Failed to load LUT image file: " + lutFile);
            }
            if (width != height * height)
            {
                stbi_image_free(pixels);
                throw std::runtime_error("Invalid LUT image dimensions (width must equal height*height): " + lutFile);
            }
        }

        std::vector<VkSpecializationMapEntry> specMapEntrys(2);
        for (uint32_t i = 0; i < specMapEntrys.size(); i++)
        {
            specMapEntrys[i].constantID = i;
            specMapEntrys[i].offset     = sizeof(int32_t) * i;
            specMapEntrys[i].size       = sizeof(int32_t);
        }
        std::vector<int32_t> specData = {height, usingPNG};

        VkSpecializationInfo fragmentSpecializationInfo;
        fragmentSpecializationInfo.mapEntryCount = specMapEntrys.size();
        fragmentSpecializationInfo.pMapEntries   = specMapEntrys.data();
        fragmentSpecializationInfo.dataSize      = specMapEntrys.size() * sizeof(int32_t);
        fragmentSpecializationInfo.pData         = specData.data();

        pVertexSpecInfo   = nullptr;
        pFragmentSpecInfo = &fragmentSpecializationInfo;

        VkExtent3D lutImageExtent = {(uint32_t) height, (uint32_t) height, (uint32_t) height};

        lutImage = createSingleImage(pLogicalDevice,
                                lutImageExtent,
                                VK_FORMAT_R8G8B8A8_UNORM, // TODO search for format and save it
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                lutMemory);

        uploadToImage(pLogicalDevice, lutImage, lutImageExtent, height * height * height * 4, pixels);

        if (usingPNG)
        {
            stbi_image_free(pixels);
        }

        lutImageView = createSingleImageView(pLogicalDevice, VK_FORMAT_R8G8B8A8_UNORM, lutImage, VK_IMAGE_VIEW_TYPE_3D);

        lutDescriptorSetLayout = createImageSamplerDescriptorSetLayout(pLogicalDevice, 1);
        descriptorSetLayouts.push_back(lutDescriptorSetLayout);

        VkDescriptorPoolSize imagePoolSize;
        imagePoolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        imagePoolSize.descriptorCount = 1;

        std::vector<VkDescriptorPoolSize> poolSizes = {imagePoolSize};

        lutDescriptorPool = createDescriptorPool(pLogicalDevice, poolSizes);

        init(pLogicalDevice, format, imageExtent, inputImages, outputImages, pConfig);

        lutDescriptorSet =
            allocateAndWriteImageSamplerDescriptorSets(pLogicalDevice,
                                                       lutDescriptorPool,
                                                       lutDescriptorSetLayout,
                                                       {sampler},
                                                       std::vector<std::vector<VkImageView>>(1, std::vector<VkImageView>(1, lutImageView)))[0];
    }
    LutEffect::~LutEffect()
    {
        // Skip cleanup if init() was never called (e.g., constructor threw exception)
        if (!pLogicalDevice || pLogicalDevice->device == VK_NULL_HANDLE)
            return;

        // Handed to the deferred queue rather than released here, for the same
        // reason as every other owner: nothing is destroyed at scope exit, so a
        // teardown that races the GPU cannot pull a handle out from under an
        // in-flight command buffer. The queue releases at the next swapchain
        // teardown or device destroy, where QueueWaitIdle has already run.
        //
        // Registration order is the reverse of release order within a phase, so
        // the image goes in before its view: that way the view is destroyed
        // first, and the memory last of all.
        auto& queue   = DeferredDestroyQueue::instance();
        auto  device = pLogicalDevice->device;
        auto& vkd    = pLogicalDevice->vkd;

        const VkDeviceMemory        memory    = lutMemory;
        const VkImage               image     = lutImage;
        const VkImageView           view      = lutImageView;
        const VkDescriptorSetLayout dsLayout  = lutDescriptorSetLayout;
        const VkDescriptorPool      dsPool    = lutDescriptorPool;

        if (memory != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Memory, [vkd, device, memory] { vkd.FreeMemory(device, memory, nullptr); });
        if (image != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [vkd, device, image] { vkd.DestroyImage(device, image, nullptr); });
        if (view != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [vkd, device, view] { vkd.DestroyImageView(device, view, nullptr); });
        if (dsLayout != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Layout, [vkd, device, dsLayout] { vkd.DestroyDescriptorSetLayout(device, dsLayout, nullptr); });
        if (dsPool != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Descriptor, [vkd, device, dsPool] { vkd.DestroyDescriptorPool(device, dsPool, nullptr); });
    }
    void LutEffect::applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer)
    {
        pLogicalDevice->vkd.CmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, 1, &(lutDescriptorSet), 0, nullptr);
        SimpleEffect::applyEffect(imageIndex, commandBuffer);
    }
} // namespace VKIntox
