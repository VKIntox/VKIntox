#include "framegen/present.hh"
#include "framegen/engine.hh"

#include "render/device/logical_device.hh"
#include "render/resources/format.hh"
#include "render/swapchain/logical_swapchain.hh"
#include "overlay/shell/imgui_overlay.hh"
#include "util/logger.hh"
#include "overlay/shell/fps_metrics.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

namespace VKIntox::FrameGen
{
    namespace
    {
        // the host program and the game's device exchange image ownership, so
        // the exchange images are created for concurrent sharing between the
        // game's queue family and the external queue family
        uint32_t externalQueueFamily() { return VK_QUEUE_FAMILY_EXTERNAL; }

        std::vector<uint32_t> sharingFamilies(LogicalDevice* logicalDevice)
        {
            return {logicalDevice->queueFamilyIndex, externalQueueFamily()};
        }

        bool deviceSupportsFgExtensions(LogicalDevice* logicalDevice)
        {
            if (logicalDevice->vki.EnumerateDeviceExtensionProperties == nullptr)
                return false;
            uint32_t count = 0;
            VkResult vr = logicalDevice->vki.EnumerateDeviceExtensionProperties(
                logicalDevice->physicalDevice, nullptr, &count, nullptr);
            if (vr != VK_SUCCESS || count == 0)
                return false;
            std::vector<VkExtensionProperties> props(count);
            vr = logicalDevice->vki.EnumerateDeviceExtensionProperties(
                logicalDevice->physicalDevice, nullptr, &count, props.data());
            if (vr != VK_SUCCESS)
                return false;
            bool memory = false, semaphoreFd = false, timeline = false, pushDescriptor = false;
            for (const auto& p : props)
            {
                memory |= std::strcmp(p.extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0;
                semaphoreFd |= std::strcmp(p.extensionName, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) == 0;
                timeline |= std::strcmp(p.extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0
                         || logicalDevice->gpuApiVersion >= VK_API_VERSION_1_2;
                pushDescriptor |= std::strcmp(p.extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0;
            }
            return memory && semaphoreFd && timeline && pushDescriptor;
        }

        VkSemaphore createTimelineSemaphore(LogicalDevice* logicalDevice, bool exportable)
        {
            VkExportSemaphoreCreateInfo exportInfo{};
            VkSemaphoreTypeCreateInfo typeInfo{};
            typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
            typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            typeInfo.initialValue = 0;
            VkSemaphoreCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (exportable)
            {
                exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
                exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
                typeInfo.pNext = &exportInfo;
                info.pNext = &typeInfo;
            }
            else
            {
                info.pNext = &typeInfo;
            }
            VkSemaphore semaphore = VK_NULL_HANDLE;
            if (logicalDevice->vkd.CreateSemaphore(logicalDevice->device, &info, nullptr, &semaphore) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            return semaphore;
        }

        VkSemaphore createBinarySemaphore(LogicalDevice* logicalDevice)
        {
            VkSemaphoreCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkSemaphore semaphore = VK_NULL_HANDLE;
            if (logicalDevice->vkd.CreateSemaphore(logicalDevice->device, &info, nullptr, &semaphore) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            return semaphore;
        }

        // a plain device-local image for the pre-overlay copy; no sharing or
        // export, it never leaves the layer's queue
        bool createLocalImage(LogicalDevice* logicalDevice, uint32_t width, uint32_t height,
                              VkFormat format, VkImageUsageFlags usage,
                              VkImage& outImage, VkDeviceMemory& outMemory)
        {
            VkImageCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = format;
            info.extent = {width, height, 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = usage;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (logicalDevice->vkd.CreateImage(logicalDevice->device, &info, nullptr, &outImage) != VK_SUCCESS)
                return false;

            VkMemoryRequirements requirements{};
            logicalDevice->vkd.GetImageMemoryRequirements(logicalDevice->device, outImage, &requirements);
            VkPhysicalDeviceMemoryProperties memoryProps{};
            logicalDevice->vki.GetPhysicalDeviceMemoryProperties(logicalDevice->physicalDevice, &memoryProps);
            int32_t memoryType = -1;
            for (uint32_t i = 0; i < memoryProps.memoryTypeCount; ++i)
            {
                if ((requirements.memoryTypeBits & (1u << i))
                    && (memoryProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                {
                    memoryType = static_cast<int32_t>(i);
                    break;
                }
            }
            if (memoryType < 0)
            {
                logicalDevice->vkd.DestroyImage(logicalDevice->device, outImage, nullptr);
                outImage = VK_NULL_HANDLE;
                return false;
            }
            VkMemoryAllocateInfo alloc{};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = requirements.size;
            alloc.memoryTypeIndex = static_cast<uint32_t>(memoryType);
            if (logicalDevice->vkd.AllocateMemory(logicalDevice->device, &alloc, nullptr, &outMemory) != VK_SUCCESS)
            {
                logicalDevice->vkd.DestroyImage(logicalDevice->device, outImage, nullptr);
                outImage = VK_NULL_HANDLE;
                return false;
            }
            if (logicalDevice->vkd.BindImageMemory(logicalDevice->device, outImage, outMemory, 0) != VK_SUCCESS)
            {
                logicalDevice->vkd.FreeMemory(logicalDevice->device, outMemory, nullptr);
                logicalDevice->vkd.DestroyImage(logicalDevice->device, outImage, nullptr);
                outImage = VK_NULL_HANDLE;
                outMemory = VK_NULL_HANDLE;
                return false;
            }
            return true;
        }

        bool createExportedImage(LogicalDevice* logicalDevice,
                                 uint32_t width, uint32_t height,
                                 VkFormat format, VkImageUsageFlags usage,
                                 VkImage& outImage, VkDeviceMemory& outMemory, int& outFd)
        {
            const std::vector<uint32_t> families = sharingFamilies(logicalDevice);

            VkExternalMemoryImageCreateInfo externalInfo{};
            externalInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
            externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

            VkImageCreateInfo imageInfo{};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.pNext = &externalInfo;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent = {width, height, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = usage;
            imageInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
            imageInfo.queueFamilyIndexCount = static_cast<uint32_t>(families.size());
            imageInfo.pQueueFamilyIndices = families.data();
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VkImage image = VK_NULL_HANDLE;
            if (logicalDevice->vkd.CreateImage(logicalDevice->device, &imageInfo, nullptr, &image) != VK_SUCCESS)
                return false;

            VkMemoryRequirements requirements{};
            logicalDevice->vkd.GetImageMemoryRequirements(logicalDevice->device, image, &requirements);

            VkExportMemoryAllocateInfo exportInfo{};
            exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
            exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            VkMemoryDedicatedAllocateInfo dedicatedInfo{};
            dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            dedicatedInfo.pNext = &exportInfo;
            dedicatedInfo.image = image;

            VkPhysicalDeviceMemoryProperties memoryProps{};
            logicalDevice->vki.GetPhysicalDeviceMemoryProperties(logicalDevice->physicalDevice, &memoryProps);
            int32_t memoryType = -1;
            for (uint32_t i = 0; i < memoryProps.memoryTypeCount; ++i)
            {
                if ((requirements.memoryTypeBits & (1u << i))
                    && (memoryProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                {
                    memoryType = static_cast<int32_t>(i);
                    break;
                }
            }
            if (memoryType < 0)
            {
                logicalDevice->vkd.DestroyImage(logicalDevice->device, image, nullptr);
                return false;
            }

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.pNext = &dedicatedInfo;
            allocInfo.allocationSize = requirements.size;
            allocInfo.memoryTypeIndex = static_cast<uint32_t>(memoryType);

            VkDeviceMemory memory = VK_NULL_HANDLE;
            if (logicalDevice->vkd.AllocateMemory(logicalDevice->device, &allocInfo, nullptr, &memory) != VK_SUCCESS)
            {
                Logger::err("FG: engine image allocation failed");
                logicalDevice->vkd.DestroyImage(logicalDevice->device, image, nullptr);
                return false;
            }
            if (logicalDevice->vkd.BindImageMemory(logicalDevice->device, image, memory, 0) != VK_SUCCESS)
            {
                logicalDevice->vkd.FreeMemory(logicalDevice->device, memory, nullptr);
                logicalDevice->vkd.DestroyImage(logicalDevice->device, image, nullptr);
                return false;
            }

            int fd = -1;
            if (logicalDevice->vkd.GetMemoryFdKHR == nullptr)
            {
                logicalDevice->vkd.FreeMemory(logicalDevice->device, memory, nullptr);
                logicalDevice->vkd.DestroyImage(logicalDevice->device, image, nullptr);
                return false;
            }
            VkMemoryGetFdInfoKHR fdInfo{};
            fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
            fdInfo.memory = memory;
            fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            if (logicalDevice->vkd.GetMemoryFdKHR(logicalDevice->device, &fdInfo, &fd) != VK_SUCCESS)
            {
                Logger::err("FG: engine image fd export failed");
                logicalDevice->vkd.FreeMemory(logicalDevice->device, memory, nullptr);
                logicalDevice->vkd.DestroyImage(logicalDevice->device, image, nullptr);
                return false;
            }
            outImage = image;
            outMemory = memory;
            outFd = fd;   // ownership moves to the backend on openContext
            return true;
        }

        VkImageMemoryBarrier imageBarrier(VkImage image,
                                          VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                                          VkImageLayout oldLayout, VkImageLayout newLayout,
                                          uint32_t srcFamily, uint32_t dstFamily)
        {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = srcAccess;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = srcFamily;
            barrier.dstQueueFamilyIndex = dstFamily;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            return barrier;
        }

        void recordBlit(LogicalDevice* logicalDevice, VkCommandBuffer cmd,
                        VkImage from, VkImage to, VkExtent2D extent)
        {
            VkImageBlit region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.srcOffsets[1] = {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstOffsets[1] = {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1};
            logicalDevice->vkd.CmdBlitImage(cmd, from, VK_IMAGE_LAYOUT_GENERAL,
                                            to, VK_IMAGE_LAYOUT_GENERAL,
                                            1, &region, VK_FILTER_LINEAR);
        }

        // the exchange format is plain UNORM or half float; blitting decodes
        // an sRGB swapchain on the way in and re-encodes on the way out
        VkFormat exchangeFormat(bool highPrecision)
        {
            return highPrecision ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        }
    }

    bool modifySwapchainCreateInfo(LogicalDevice* logicalDevice,
                                   VkSwapchainCreateInfoKHR& info,
                                   const FgProfile& profile)
    {
        VKINTOX_LOG_TRACE([&](std::string& s) {
            s = std::string("FG: create hook entered (enabled=")
                + (profile.enabled ? "true" : "false")
                + ", multiplier=" + std::to_string(profile.multiplier) + ")";
        });
        if (!profile.enabled || profile.multiplier < 2
            || !engineAvailable() || !deviceSupportsFgExtensions(logicalDevice))
            return false;

        info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        VkSurfaceCapabilitiesKHR caps{};
        if (logicalDevice->vki.GetPhysicalDeviceSurfaceCapabilitiesKHR)
            logicalDevice->vki.GetPhysicalDeviceSurfaceCapabilitiesKHR(
                logicalDevice->physicalDevice, info.surface, &caps);
        info.minImageCount += 4;
        info.minImageCount = std::min<uint32_t>({info.minImageCount, 8u,
                                                 caps.maxImageCount == 0 ? 999u : caps.maxImageCount});
        Logger::info("FG: swapchain image pool grown to " + std::to_string(info.minImageCount)
                     + " for live frame generation");

        // Generated frames are only ever seen when the presentation engine
        // shows every queued image: under MAILBOX the real frame replaces the
        // generated one in the queue, under IMMEDIATE they tear into one
        // scanout. Both count as presented, so the fps pill rose while motion
        // stayed exactly as smooth as before. FIFO is always supported.
        if (profile.enabled && profile.forceVsync && !presentModeQueuesEveryFrame(info.presentMode))
        {
            // with VK_EXT_swapchain_maintenance1 the app lists the modes it
            // may switch between; FIFO must be one of them to be valid here
            bool fifoAllowed = true;
            for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next; next = next->pNext)
            {
                if (next->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT)
                    continue;
                const auto* modes = reinterpret_cast<const VkSwapchainPresentModesCreateInfoEXT*>(next);
                fifoAllowed = false;
                for (uint32_t i = 0; i < modes->presentModeCount; ++i)
                    fifoAllowed |= modes->pPresentModes[i] == VK_PRESENT_MODE_FIFO_KHR;
            }
            if (fifoAllowed)
            {
                Logger::info("FG: present mode " + std::to_string(info.presentMode)
                             + " -> FIFO so generated frames reach the display");
                info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            }
            else
            {
                Logger::warn("FG: the app's present mode list excludes FIFO; generated frames "
                             "fall back to CPU pacing and may be dropped by the compositor");
            }
        }
        return true;
    }

    bool presentModeQueuesEveryFrame(VkPresentModeKHR mode)
    {
        // FIFO_RELAXED only tears when the app is late, which with FG on means
        // the generator fell behind; it still queues every image otherwise
        return mode == VK_PRESENT_MODE_FIFO_KHR || mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }

    void forceFifoInPresentChain(const VkPresentInfoKHR* presentInfo)
    {
        // same approach as lsfg-vk: the per-present array belongs to the app,
        // and is overwritten in place for the duration of the call
        static const VkPresentModeKHR fifo[8] = {
            VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR,
            VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR,
        };
        for (auto* next = static_cast<const VkBaseInStructure*>(presentInfo->pNext); next; next = next->pNext)
        {
            if (next->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT)
                continue;
            auto* modeInfo = const_cast<VkSwapchainPresentModeInfoEXT*>(
                reinterpret_cast<const VkSwapchainPresentModeInfoEXT*>(next));
            if (modeInfo->swapchainCount <= 8)
                modeInfo->pPresentModes = fifo;
        }
    }

    bool FgSwapchain::active(LogicalDevice* logicalDevice, const FgProfile& profile)
    {
        if (!profile.enabled || profile.multiplier < 2)
            return false;
        if (!engineAvailable())
            return false;
        if (!deviceSupportsFgExtensions(logicalDevice))
            return false;
        return true;
    }

    FgSwapchain::FgSwapchain(LogicalDevice* logicalDevice, LogicalSwapchain* swapchain, const FgProfile& profile)
        : m_logicalDevice(logicalDevice), m_multiplier(profile.multiplier), m_extent(swapchain->imageExtent),
          m_fifo(presentModeQueuesEveryFrame(swapchain->presentMode))
    {
        Logger::info(std::string("FG: pacing by ") + (m_fifo ? "vsync (FIFO)" : "CPU sleep (non-FIFO swapchain)"));
        m_lastReleaseProbe = std::chrono::steady_clock::now();
        try
        {
            if (!createEngineObjects(logicalDevice, swapchain, profile))
                throw std::runtime_error("FG engine objects failed to initialize");
        }
        catch (...)
        {
            destroy();
            throw;
        }
    }

    bool FgSwapchain::createEngineObjects(LogicalDevice* logicalDevice, LogicalSwapchain* swapchain, const FgProfile& profile)
    {
        Logger::info("FG: engine instance on '" + logicalDevice->gpuName + "'");
        try
        {
            m_engineInstance = std::make_unique<EngineInstance>(
                logicalDevice, profile.halfPrecision, profile.dllPath);
        }
        catch (const std::exception& e)
        {
            Logger::err("FG: engine instance failed: " + std::string(e.what()));
            return false;
        }

        const uint32_t generatedCount = m_multiplier - 1;
        const bool highPrecision =
            swapchain->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32
            || swapchain->format == VK_FORMAT_A2R10G10B10_UNORM_PACK32
            || swapchain->format == VK_FORMAT_R16G16B16A16_SFLOAT;
        const VkFormat format = exchangeFormat(highPrecision);

        // two source images alternate; one destination per generated frame
        FileDescriptorScope descriptors;
        std::vector<int> sourceFds(2, -1);
        std::vector<int> destFds(generatedCount, -1);
        for (uint32_t i = 0; i < 2; ++i)
        {
            if (!createExportedImage(logicalDevice, m_extent.width, m_extent.height, format,
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     m_sourceImages[i], m_sourceMemories[i], sourceFds[i]))
            {
                Logger::err("FG: source image export failed");
                return false;
            }
            if (i == 0)
                descriptors.setSourceA(sourceFds[i]);
            else
                descriptors.setSourceB(sourceFds[i]);
            sourceFds[i] = -1;
        }
        m_destinationImages.resize(generatedCount);
        m_destinationMemories.resize(generatedCount);
        for (uint32_t i = 0; i < generatedCount; ++i)
        {
            if (!createExportedImage(logicalDevice, m_extent.width, m_extent.height, format,
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                     m_destinationImages[i], m_destinationMemories[i], destFds[i]))
            {
                Logger::err("FG: destination image export failed");
                return false;
            }
            descriptors.addDestination(destFds[i]);
            destFds[i] = -1;
        }

        m_syncSemaphore = createTimelineSemaphore(logicalDevice, true);
        if (m_syncSemaphore == VK_NULL_HANDLE)
        {
            Logger::err("FG: timeline semaphore creation failed");
            return false;
        }
        int syncFd = -1;
        if (logicalDevice->vkd.GetSemaphoreFdKHR == nullptr)
        {
            Logger::err("FG: vkGetSemaphoreFdKHR unavailable");
            return false;
        }
        VkSemaphoreGetFdInfoKHR fdInfo{};
        fdInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
        fdInfo.semaphore = m_syncSemaphore;
        fdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (logicalDevice->vkd.GetSemaphoreFdKHR(logicalDevice->device, &fdInfo, &syncFd) != VK_SUCCESS)
        {
            Logger::err("FG: timeline semaphore fd export failed");
            return false;
        }
        descriptors.setSync(syncFd);
        syncFd = -1;

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = logicalDevice->queueFamilyIndex;
        if (logicalDevice->vkd.CreateCommandPool(logicalDevice->device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS)
        {
            Logger::err("FG: command pool creation failed");
            return false;
        }

        // The generator interpolates the app's frame as it looks before the
        // VKIntox overlay exists. The copy runs between the effect submit and
        // the overlay submit, so the game HUD and effects stay in the source.
        m_frameSourceImages.assign(swapchain->imageCount, VK_NULL_HANDLE);
        m_frameSourceMemories.assign(swapchain->imageCount, VK_NULL_HANDLE);
        m_frameCopyCmdbufs.assign(swapchain->imageCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < swapchain->imageCount; ++i)
        {
            if (!createLocalImage(logicalDevice, m_extent.width, m_extent.height, swapchain->format,
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  m_frameSourceImages[i], m_frameSourceMemories[i]))
            {
                Logger::err("FG: pre-overlay source image creation failed");
                return false;
            }
            VkCommandBufferAllocateInfo copyAlloc{};
            copyAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            copyAlloc.commandPool = m_commandPool;
            copyAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            copyAlloc.commandBufferCount = 1;
            if (logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &copyAlloc,
                                                          &m_frameCopyCmdbufs[i]) != VK_SUCCESS)
                return false;
            VkCommandBufferBeginInfo cbBegin{};
            cbBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            if (logicalDevice->vkd.BeginCommandBuffer(m_frameCopyCmdbufs[i], &cbBegin) != VK_SUCCESS)
                return false;
            VkImageMemoryBarrier preCopy[2] = {
                imageBarrier(swapchain->images[i],
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                             VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED),
                imageBarrier(m_frameSourceImages[i], VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED),
            };
            logicalDevice->vkd.CmdPipelineBarrier(m_frameCopyCmdbufs[i], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                                  2, preCopy);
            recordBlit(logicalDevice, m_frameCopyCmdbufs[i], swapchain->images[i],
                       m_frameSourceImages[i], m_extent);
            VkImageMemoryBarrier postCopy[2] = {
                imageBarrier(swapchain->images[i], VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_NONE,
                             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                             VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED),
                imageBarrier(m_frameSourceImages[i], VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED),
            };
            logicalDevice->vkd.CmdPipelineBarrier(m_frameCopyCmdbufs[i], VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                                  2, postCopy);
            if (logicalDevice->vkd.EndCommandBuffer(m_frameCopyCmdbufs[i]) != VK_SUCCESS)
                return false;
        }

        m_syncPasses.resize(swapchain->imageCount);
        for (auto& pass : m_syncPasses)
        {
            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = m_commandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            if (logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &allocInfo, &pass.uploadCmdbuf) != VK_SUCCESS)
            {
                Logger::err("FG: command buffer allocation failed");
                return false;
            }
            pass.presentSemaphore = createBinarySemaphore(logicalDevice);
            if (pass.presentSemaphore == VK_NULL_HANDLE)
            {
                Logger::err("FG: present semaphore creation failed");
                return false;
            }
            pass.innerSyncPasses.resize(generatedCount);
            for (auto& inner : pass.innerSyncPasses)
            {
                if (logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &allocInfo, &inner.copyCmdbuf) != VK_SUCCESS
                    || logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &allocInfo, &inner.releaseCmdbuf) != VK_SUCCESS)
                {
                    Logger::err("FG: inner command buffer allocation failed");
                    return false;
                }
                inner.acquireSemaphore = createBinarySemaphore(logicalDevice);
                inner.copySemaphore = createBinarySemaphore(logicalDevice);
                inner.releaseSemaphore = createBinarySemaphore(logicalDevice);
                if (inner.acquireSemaphore == VK_NULL_HANDLE || inner.copySemaphore == VK_NULL_HANDLE
                    || inner.releaseSemaphore == VK_NULL_HANDLE)
                    return false;
            }
        }

        try
        {
            // flow: inverse scale 1..4; the UI's scale maps 1.0 -> 1, 0.5 -> 2,
            // 0.25 -> 4
            uint32_t flow = profile.flowScale >= 0.75f ? 1u
                : profile.flowScale >= 0.375f ? 2u : 4u;
            m_engineContext = std::make_unique<EngineContext>(
                *m_engineInstance, descriptors,
                m_extent.width, m_extent.height,
                highPrecision, flow, profile.performanceMode);
        }
        catch (const std::exception& e)
        {
            Logger::err("FG: engine context failed: " + std::string(e.what()));
            return false;
        }

        if (!logicalDevice->vkd.GetDeviceProcAddr)
        {
            Logger::err("FG: vkGetDeviceProcAddr unavailable for host-loss release");
            return false;
        }
        m_lossState = std::make_shared<LossReleaseState>();
        m_lossState->device = logicalDevice->device;
        m_lossState->semaphore = m_syncSemaphore;
        m_lossState->signal = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
            logicalDevice->vkd.GetDeviceProcAddr(logicalDevice->device, "vkSignalSemaphoreKHR"));
        if (!m_lossState->signal)
        {
            m_lossState->signal = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
                logicalDevice->vkd.GetDeviceProcAddr(logicalDevice->device, "vkSignalSemaphore"));
        }
        if (!m_lossState->signal)
        {
            Logger::err("FG: timeline semaphore signalling unavailable for host-loss release");
            return false;
        }
        const auto lossState = m_lossState;
        if (!m_engineContext->setLossHandler([lossState] {
                lossState->lost.store(true, std::memory_order_release);
                VkSemaphoreSignalInfoKHR signal{};
                signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO_KHR;
                signal.semaphore = lossState->semaphore;
                signal.value = uint64_t{1} << 30;
                if (lossState->signal)
                    static_cast<void>(lossState->signal(lossState->device, &signal));
            }))
        {
            Logger::err("FG: host-loss handler installation failed");
            return false;
        }

        Logger::info("FG: frame generation ready (x" + std::to_string(m_multiplier) + ")");

        // Record the blank warmup now, but submit it only after real frames have
        // been presented. Pipeline compilation runs on the warmup thread.
        const uint32_t ours = logicalDevice->queueFamilyIndex;
        const uint32_t external = externalQueueFamily();
        const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkCommandBuffer warmCmd = m_syncPasses[0].uploadCmdbuf;
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (logicalDevice->vkd.BeginCommandBuffer(warmCmd, &beginInfo) != VK_SUCCESS)
            return false;
        VkClearColorValue black{};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.layerCount = 1;
        std::vector<VkImageMemoryBarrier> warmBarriers;
        std::vector<VkImage> warmImages;
        for (VkImage image : {m_sourceImages[0], m_sourceImages[1]})
            warmImages.push_back(image);
        for (VkImage image : m_destinationImages)
            warmImages.push_back(image);
        warmBarriers.reserve(warmImages.size());
        for (VkImage image : warmImages)
        {
            warmBarriers.push_back(imageBarrier(image, VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                                external, ours));
        }
        logicalDevice->vkd.CmdPipelineBarrier(warmCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, transfer,
                                              0, 0, nullptr, 0, nullptr,
                                              static_cast<uint32_t>(warmBarriers.size()), warmBarriers.data());
        for (VkImage image : warmImages)
            logicalDevice->vkd.CmdClearColorImage(warmCmd, image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
        std::vector<VkImageMemoryBarrier> releaseBarriers;
        releaseBarriers.reserve(warmImages.size());
        for (VkImage image : warmImages)
        {
            releaseBarriers.push_back(imageBarrier(image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_NONE,
                                                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                                   ours, external));
        }
        logicalDevice->vkd.CmdPipelineBarrier(warmCmd, transfer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                              0, 0, nullptr, 0, nullptr,
                                              static_cast<uint32_t>(releaseBarriers.size()), releaseBarriers.data());
        if (logicalDevice->vkd.EndCommandBuffer(warmCmd) != VK_SUCCESS)
            return false;
        Logger::info("FG: resources ready; warmup deferred until the swapchain settles");
        return true;
    }

    bool FgSwapchain::hostLost() const
    {
        return (m_lossState && m_lossState->lost.load(std::memory_order_acquire))
            || (m_engineContext && m_engineContext->contextLost());
    }

    bool FgSwapchain::startWarmup(LogicalDevice* logicalDevice)
    {
        if (m_warmupStarted.exchange(true) || !m_engineContext || m_syncPasses.empty())
            return false;
        if (hostLost())
        {
            m_warmupFailed.store(true, std::memory_order_release);
            return false;
        }

        VkSemaphore signal = m_syncSemaphore;
        uint64_t value = 1;
        VkTimelineSemaphoreSubmitInfo timeline{};
        timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &value;
        VkCommandBuffer command = m_syncPasses[0].uploadCmdbuf;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.pNext = &timeline;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &signal;
        if (logicalDevice->vkd.QueueSubmit(logicalDevice->queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
        {
            m_warmupFailed.store(true, std::memory_order_release);
            return false;
        }

        try
        {
            m_warmupThread = std::thread([this] {
                try
                {
                    bool complete = !m_stopWarmup.load(std::memory_order_acquire)
                        && !hostLost() && m_engineContext->scheduleFrames()
                        && m_engineContext->waitForIdle(2'000'000'000ull);
                    if (complete && !m_stopWarmup.load(std::memory_order_acquire))
                    {
                        m_syncValue = 1 + m_multiplier - 1;
                        m_sourceIndex = 1;
                        m_ready.store(true, std::memory_order_release);
                        Logger::info("FG: engine warmup complete");
                    }
                    else
                    {
                        m_warmupFailed.store(true, std::memory_order_release);
                    }
                }
                catch (...)
                {
                    m_warmupFailed.store(true, std::memory_order_release);
                }
            });
        }
        catch (...)
        {
            m_warmupFailed.store(true, std::memory_order_release);
            return false;
        }
        return true;
    }

    VkResult FgSwapchain::present(LogicalDevice* logicalDevice,
                                  LogicalSwapchain* swapchain,
                                  uint32_t index,
                                  VkQueue queue,
                                  VkSemaphore waitSemaphore,
                                  VkImage sourceImage,
                                  VkImageLayout sourceLayout,
                                  bool sourceReady,
                                  bool replayOverlay,
                                  const VkPresentInfoKHR* originalPresentInfo)
    {
        // Presents must wait the composited semaphore, never the application's
        // original list: the layer's effect submit already consumed those.
        VkPresentInfoKHR fallback = *originalPresentInfo;
        if (waitSemaphore != VK_NULL_HANDLE)
        {
            fallback.waitSemaphoreCount = 1;
            fallback.pWaitSemaphores = &waitSemaphore;
        }
        else
        {
            fallback.waitSemaphoreCount = 0;
            fallback.pWaitSemaphores = nullptr;
        }
        const auto presentComposited = [&]() -> VkResult {
            return logicalDevice->vkd.QueuePresentKHR(queue, &fallback);
        };
        if (!m_ready.load())
        {
            if (!m_warmupStarted.load(std::memory_order_acquire) && ++m_passThroughPresents >= 2)
                startWarmup(logicalDevice);
            return presentComposited();
        }
        if (hostLost())
        {
            m_ready.store(false, std::memory_order_release);
            return presentComposited();
        }
        if (!sourceReady || sourceImage == VK_NULL_HANDLE)
            return presentComposited();
        if (index >= m_syncPasses.size() || !m_engineContext)
            return presentComposited();
        SyncPass& pass = m_syncPasses[index];
        VkImage presentImage = sourceImage;
        VkImage source = m_sourceImages[m_sourceIndex];
        VkImage previousSource = m_sourceImages[m_sourceIndex ^ 1];
        const uint32_t generatedCount = m_multiplier - 1;
        // The upload signals a dedicated binary semaphore alongside its
        // timeline value. Every post-upload fallback waits on that fresh
        // signal instead of reusing the already-consumed composited semaphore.
        const auto presentAfterUpload = [&]() -> VkResult {
            VkPresentInfoKHR info = fallback;
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores = &pass.presentSemaphore;
            return logicalDevice->vkd.QueuePresentKHR(queue, &info);
        };
        const uint32_t ours = logicalDevice->queueFamilyIndex;
        const uint32_t external = externalQueueFamily();
        const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;

        // Adaptive insertion. The multiplier is a ceiling, never a divisor of
        // the game's rate: a generated frame may only take a display slot the
        // application is not already using. The target starts at one, every
        // raise is validated against the application's own gap, and a failed
        // raise reveals the display interval so later decisions need no trial.
        const auto presentNow = std::chrono::steady_clock::now();
        uint64_t presentGapUs = 0;
        if (m_lastPresentTime.time_since_epoch().count() != 0)
        {
            presentGapUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                presentNow - m_lastPresentTime).count());
        }
        m_lastPresentTime = presentNow;
        const uint32_t previousInserts = m_lastInsertCount;
        m_lastInsertCount = 0;

        const uint32_t insertCap = generatedCount;
        const bool warming = m_warmupFrames > 0;
        if (warming)
            --m_warmupFrames;

        if (presentGapUs > 150000)
        {
            // the surface was hidden or the app paused: the host history is
            // from another era, so restart it instead of interpolating across
            m_firstSourceUpload = true;
            m_appIntervalUs = 0;
            m_passIntervalUs = 0;
            m_refreshIntervalUs = 0;
            m_freeGapCount = 0;
            m_freeGapNext = 0;
            m_expectedGapUs = 0;
            m_insertTarget = 1;
            m_prevInsertTarget = 0;
            m_lastInsertCount = 0;
            m_goodFrames = 0;
            m_releaseFrames = 0;
            m_probeCooldown = 0;
            m_checkFrames = 0;
            m_checkSlowFrames = 0;
            m_checkSlowGapUs = 0;
            m_targetLatched = false;
            m_holdUntil = {};
        }
        else
        {
            // the warmup presents nothing, so they are the cleanest samples of
            // what the app can do on its own
            if (presentGapUs >= 2000 && presentGapUs <= 150000 && previousInserts == 0)
            {
                // free-running cadence: median of the recent free gaps, so one
                // duplicate or recreation frame cannot poison the estimate
                m_freeGaps[m_freeGapNext] = presentGapUs;
                m_freeGapNext = (m_freeGapNext + 1) % static_cast<uint32_t>(m_freeGaps.size());
                if (m_freeGapCount < m_freeGaps.size())
                    ++m_freeGapCount;
                std::array<uint64_t, 16> sortedGaps = m_freeGaps;
                std::sort(sortedGaps.begin(), sortedGaps.begin() + m_freeGapCount);
                const uint64_t medianGap = m_freeGapCount >= 3
                    ? sortedGaps[m_freeGapCount / 2] : sortedGaps[0];
                m_appIntervalUs = medianGap;
                m_passIntervalUs = m_appIntervalUs;

                // the app itself slowed down while nothing was inserted: the
                // display has room again, no probe needed
                if (!warming && m_refreshIntervalUs > 0 && insertCap > 0 && m_checkFrames == 0)
                {
                    uint32_t fits = static_cast<uint32_t>(m_appIntervalUs / m_refreshIntervalUs);
                    fits = fits > 0 ? fits - 1 : 0;
                    fits = std::min(fits, insertCap);
                    if (fits > m_insertTarget)
                    {
                        m_insertTarget = fits;
                        m_expectedGapUs = presentGapUs;
                        m_goodFrames = 0;
                        m_targetLatched = false;
                        Logger::debug("FG: app slowed, target=x" + std::to_string(m_insertTarget + 1));
                    }
                }
            }

            if (warming)
            {
                // still sampling; the first raise is armed when this ends
            }
            else if (m_checkFrames > 0)
            {
                // the raise is on trial. count the presents that took longer
                // than the cadence that justified it and decide on the whole
                // window, so queue slack cannot hide a frame the app needed.
                if (previousInserts == m_prevInsertTarget + 1 && presentGapUs >= 2000)
                {
                    if (presentGapUs > m_expectedGapUs + 1500)
                    {
                        ++m_checkSlowFrames;
                        m_checkSlowGapUs += presentGapUs;
                    }
                    if (--m_checkFrames == 0)
                    {
                        if (m_checkSlowFrames >= 3)
                        {
                            const uint64_t refresh = (m_checkSlowGapUs / m_checkSlowFrames)
                                / (m_prevInsertTarget + 2);
                            const bool plausible = refresh >= 3000 && refresh <= 40000
                                && (m_refreshIntervalUs == 0
                                    || (refresh * 100 > m_refreshIntervalUs * 70
                                        && refresh * 100 < m_refreshIntervalUs * 130));
                            if (plausible)
                                m_refreshIntervalUs = m_refreshIntervalUs == 0 ? refresh
                                    : (m_refreshIntervalUs * 3 + refresh) / 4;
                            m_insertTarget = m_prevInsertTarget;
                            m_targetLatched = true;
                            m_holdUntil = presentNow + std::chrono::seconds(5);
                            Logger::debug("FG: insert target does not fit, back to x"
                                + std::to_string(m_insertTarget + 1)
                                + " slow=" + std::to_string(m_checkSlowFrames)
                                + " gap=" + std::to_string(m_checkSlowGapUs / m_checkSlowFrames)
                                + " refresh=" + std::to_string(m_refreshIntervalUs));
                        }
                        else
                        {
                            m_targetLatched = false;
                        }
                        m_checkSlowFrames = 0;
                        m_checkSlowGapUs = 0;
                        m_goodFrames = 0;
                    }
                }
            }
            else if (previousInserts > 0 && presentGapUs >= 2000)
            {
                const uint64_t batchUs = m_refreshIntervalUs > 0
                    ? static_cast<uint64_t>(previousInserts + 1) * m_refreshIntervalUs : 0;
                if (!m_targetLatched && batchUs > 0 && presentGapUs > batchUs + batchUs / 8)
                {
                    // the app is now slower than the batch: it has room for
                    // more generated frames
                    m_appIntervalUs = (m_appIntervalUs * 3 + presentGapUs) / 4;
                    m_expectedGapUs = presentGapUs;
                    if (m_refreshIntervalUs > 0)
                    {
                        uint32_t fits = static_cast<uint32_t>(m_appIntervalUs / m_refreshIntervalUs);
                        fits = fits > 0 ? fits - 1 : 0;
                        fits = std::min(fits, insertCap);
                        if (fits > m_insertTarget)
                        {
                            m_insertTarget = fits;
                            m_goodFrames = 0;
                        }
                    }
                }
                else if (presentGapUs <= m_expectedGapUs + 1500)
                {
                    ++m_goodFrames;
                }
                else
                {
                    m_goodFrames = 0;
                }
            }

            if (m_checkFrames == 0 && !m_targetLatched && m_insertTarget < insertCap
                && m_goodFrames >= 30 && presentGapUs >= 2000)
            {
                bool room = true;
                if (m_refreshIntervalUs > 0 && m_appIntervalUs > 0)
                {
                    uint32_t fits = static_cast<uint32_t>(m_appIntervalUs / m_refreshIntervalUs);
                    fits = fits > 0 ? fits - 1 : 0;
                    fits = std::min(fits, insertCap);
                    room = fits > m_insertTarget;
                }
                if (room)
                {
                    m_prevInsertTarget = m_insertTarget;
                    m_expectedGapUs = presentGapUs;
                    ++m_insertTarget;
                    m_checkFrames = 24;
                    m_checkSlowFrames = 0;
                    m_checkSlowGapUs = 0;
                    m_goodFrames = 0;
                    Logger::debug("FG: raising insert target to x" + std::to_string(m_insertTarget + 1));
                }
                else
                {
                    m_goodFrames = 0;
                }
            }

            // a faster app cannot show up while we are pacing it, so every few
            // seconds present two free-running frames and re-learn its cadence
            if (m_releaseFrames > 0)
            {
                if (--m_releaseFrames == 0 && m_appIntervalUs > 0 && m_refreshIntervalUs > 0)
                {
                    const uint64_t freeGap = std::max(m_appIntervalUs, m_refreshIntervalUs);
                    uint32_t fits = static_cast<uint32_t>(freeGap / m_refreshIntervalUs);
                    fits = fits > 0 ? fits - 1 : 0;
                    fits = std::min(fits, insertCap);
                    if (fits != m_insertTarget)
                    {
                        m_insertTarget = fits;
                        m_checkFrames = 0;
                        m_targetLatched = false;
                        m_goodFrames = 0;
                    }
                    m_expectedGapUs = freeGap;
                }
            }
            else if (m_checkFrames == 0 && m_insertTarget > 0 && m_refreshIntervalUs > 0
                && (presentNow - m_lastReleaseProbe > std::chrono::seconds(12)
                    || (m_targetLatched && m_holdUntil.time_since_epoch().count() != 0
                        && presentNow >= m_holdUntil)))
            {
                m_lastReleaseProbe = presentNow;
                m_holdUntil = {};
                m_freeGapCount = 0;
                m_freeGapNext = 0;
                m_releaseFrames = 2;
            }
        }

        if (warming && m_warmupFrames == 0)
        {
            // first generated frame is a raise from zero: validate it next time
            m_expectedGapUs = m_appIntervalUs;
            m_prevInsertTarget = 0;
            m_insertTarget = 1;
            m_checkFrames = m_appIntervalUs > 0 ? 24 : 0;
            m_checkSlowFrames = 0;
            m_checkSlowGapUs = 0;
        }
        if (m_warmupFrames > 0)
        {
            // skipped frames never reach the host, so the next insertion must
            // not interpolate against a stale source
            m_firstSourceUpload = true;
            return presentComposited();
        }

        const uint32_t wanted = m_releaseFrames > 0 ? 0 : m_insertTarget;
        if (wanted == 0 || hostLost() || !m_engineContext->waitForIdle(0))
        {
            m_firstSourceUpload = true;
            return presentComposited();
        }

        // upload the composited frame into this batch's source image: acquire
        // it from the external host, copy, release back in GENERAL
        VkCommandBuffer uploadCmd = pass.uploadCmdbuf;
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult vr = logicalDevice->vkd.BeginCommandBuffer(uploadCmd, &beginInfo);
        if (vr != VK_SUCCESS)
            return presentComposited();

        VkImageMemoryBarrier uploadBarriers[3] = {};
        uint32_t uploadBarrierCount = 0;
        uploadBarriers[uploadBarrierCount++] =
            imageBarrier(source, VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         external, ours);
        if (m_firstSourceUpload)
        {
            uploadBarriers[uploadBarrierCount++] =
                imageBarrier(previousSource, VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             external, ours);
        }
        uploadBarriers[uploadBarrierCount++] =
            imageBarrier(presentImage, VK_ACCESS_NONE, VK_ACCESS_TRANSFER_READ_BIT,
                         sourceLayout, VK_IMAGE_LAYOUT_GENERAL,
                         VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        logicalDevice->vkd.CmdPipelineBarrier(uploadCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, transfer,
                                              0, 0, nullptr, 0, nullptr,
                                              uploadBarrierCount, uploadBarriers);
        recordBlit(logicalDevice, uploadCmd, presentImage, source, m_extent);
        if (m_firstSourceUpload)
            recordBlit(logicalDevice, uploadCmd, presentImage, previousSource, m_extent);
        VkImageMemoryBarrier backBarriers[3] = {};
        uint32_t backBarrierCount = 0;
        backBarriers[backBarrierCount++] =
            imageBarrier(source, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_NONE,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         ours, external);
        if (m_firstSourceUpload)
        {
            backBarriers[backBarrierCount++] =
                imageBarrier(previousSource, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_NONE,
                             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                             ours, external);
        }
        // the source is the app's image (restore PRESENT_SRC) or the HUD-free
        // scene (restore SHADER_READ_ONLY, which the effect chain samples)
        backBarriers[backBarrierCount++] =
            imageBarrier(presentImage, VK_ACCESS_TRANSFER_READ_BIT,
                         sourceLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                             ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_NONE,
                         VK_IMAGE_LAYOUT_GENERAL, sourceLayout,
                         VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        logicalDevice->vkd.CmdPipelineBarrier(uploadCmd, transfer,
                                              sourceLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                  ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                                  : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                              0, 0, nullptr, 0, nullptr,
                                              backBarrierCount, backBarriers);
        logicalDevice->vkd.EndCommandBuffer(uploadCmd);

        const uint64_t appSignal = m_syncValue + 1;
        uint64_t signalValues[2] = { appSignal, 0 };
        char waitText[32];
        std::snprintf(waitText, sizeof waitText, "%p", reinterpret_cast<void*>(waitSemaphore));
        Logger::debug("FG: batch image=" + std::to_string(index)
                      + " source=" + std::to_string(m_sourceIndex)
                      + " trigger=" + std::to_string(appSignal)
                      + " previous=" + std::to_string(m_syncValue)
                      + " wait=" + waitText
                      + (m_firstSourceUpload ? " first" : ""));

        // the host rejects a new batch while the previous one's signals are
        // unconsumed, so the upload also waits the highest timeline value so
        // far (nothing extra to wait on the very first frame)
        VkSemaphore waits[2] = {};
        uint64_t uploadWaitValues[2] = {};
        uint32_t waitCount = 0;
        if (waitSemaphore != VK_NULL_HANDLE)
        {
            waits[waitCount] = waitSemaphore;
            uploadWaitValues[waitCount] = 0;
            waitCount++;
        }
        if (m_syncValue > 0)
        {
            waits[waitCount] = m_syncSemaphore;
            uploadWaitValues[waitCount] = m_syncValue;
            waitCount++;
        }
        VkTimelineSemaphoreSubmitInfo timelineInfo{};
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.waitSemaphoreValueCount = waitCount;
        timelineInfo.pWaitSemaphoreValues = waitCount > 0 ? uploadWaitValues : nullptr;
        timelineInfo.signalSemaphoreValueCount = 2;
        timelineInfo.pSignalSemaphoreValues = signalValues;
        VkPipelineStageFlags waitStages[2] = {
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        };
        VkSemaphore signalSemaphores[2] = { m_syncSemaphore, pass.presentSemaphore };
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = &timelineInfo;
        submitInfo.waitSemaphoreCount = waitCount;
        submitInfo.pWaitSemaphores = waitCount > 0 ? waits : nullptr;
        submitInfo.pWaitDstStageMask = waitCount > 0 ? waitStages : nullptr;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &uploadCmd;
        submitInfo.signalSemaphoreCount = 2;
        submitInfo.pSignalSemaphores = signalSemaphores;
        vr = logicalDevice->vkd.QueueSubmit(logicalDevice->queue, 1, &submitInfo, VK_NULL_HANDLE);
        if (vr != VK_SUCCESS)
        {
            Logger::err("FG: upload submit failed (" + std::to_string(vr) + ")");
            return presentComposited();
        }
        if (hostLost() || !m_engineContext->scheduleFrames())
        {
            Logger::err("FG: host unavailable; frame generation stops until its settings change");
            m_ready.store(false, std::memory_order_release);
            return presentAfterUpload();
        }
        Logger::debug("FG: batch scheduled after host idle");

        // Acquire only after the host accepted the batch. A host failure can
        // therefore never strand swapchain images before their copies exist.
        if (hostLost())
        {
            m_ready.store(false, std::memory_order_release);
            return presentAfterUpload();
        }
        std::vector<uint32_t> acquired;
        acquired.reserve(wanted);
        if (m_probeCooldown > 0)
        {
            --m_probeCooldown;
        }
        else
        {
            for (uint32_t j = 0; j < wanted; ++j)
            {
                VkSemaphore acquire = pass.innerSyncPasses[j].acquireSemaphore;
                uint32_t acquiredIndex = 0;
                VkResult acquireResult = logicalDevice->vkd.AcquireNextImageKHR(
                    logicalDevice->device, swapchain->handle,
                    m_fifo ? 150'000'000ull : 30'000'000ull, acquire, VK_NULL_HANDLE, &acquiredIndex);
                if (acquireResult == VK_SUCCESS || acquireResult == VK_SUBOPTIMAL_KHR)
                {
                    acquired.push_back(acquiredIndex);
                    continue;
                }
                if (acquireResult == VK_TIMEOUT || acquireResult == VK_NOT_READY)
                    break;
                Logger::err("FG: inner acquire failed (" + std::to_string(acquireResult) + ")");
                if (acquireResult == VK_ERROR_DEVICE_LOST)
                    m_ready.store(false, std::memory_order_release);
                break;
            }
            if (acquired.empty())
                m_probeCooldown = 2;
        }
        if (acquired.empty())
        {
            m_syncValue = appSignal + generatedCount;
            m_sourceIndex ^= 1;
            m_firstSourceUpload = false;
            return presentAfterUpload();
        }

        m_probeCooldown = 0;
        const uint32_t insertCount = static_cast<uint32_t>(acquired.size());
        m_lastInsertCount = insertCount;
        Logger::debug("FG: insert batch " + std::to_string(insertCount) + "/"
            + std::to_string(wanted) + " target=x" + std::to_string(m_insertTarget + 1)
            + " good=" + std::to_string(m_goodFrames)
            + " refresh=" + std::to_string(m_refreshIntervalUs));

        // If a generated copy cannot be submitted, present the real frame in
        // each still-acquired slot. Swapchain images have no explicit release
        // API, so a successful present is the only bounded cleanup path.
        const auto releaseAcquired = [&](uint32_t begin) -> VkResult {
            const VkAccessFlags sourceAccess = sourceLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_NONE;
            for (uint32_t i = begin; i < insertCount; ++i)
            {
                InnerSyncPass& inner = pass.innerSyncPasses[i];
                const uint32_t targetIndex = acquired[i];
                VkImage target = swapchain->images[targetIndex];
                VkCommandBuffer command = inner.releaseCmdbuf;
                if (logicalDevice->vkd.BeginCommandBuffer(command, &beginInfo) != VK_SUCCESS)
                    return VK_ERROR_UNKNOWN;
                VkImageMemoryBarrier barriers[2] = {
                    imageBarrier(presentImage, sourceAccess, VK_ACCESS_TRANSFER_READ_BIT,
                                 sourceLayout, VK_IMAGE_LAYOUT_GENERAL,
                                 VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED),
                    imageBarrier(target, VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                 VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED),
                };
                logicalDevice->vkd.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, transfer,
                                                      0, 0, nullptr, 0, nullptr, 2, barriers);
                recordBlit(logicalDevice, command, presentImage, target, m_extent);
                barriers[0] = imageBarrier(target, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_NONE,
                                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                           VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
                barriers[1] = imageBarrier(presentImage, VK_ACCESS_TRANSFER_READ_BIT, sourceAccess,
                                           VK_IMAGE_LAYOUT_GENERAL, sourceLayout,
                                           VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
                logicalDevice->vkd.CmdPipelineBarrier(command, transfer,
                                                      sourceAccess ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                                                   : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                                      0, 0, nullptr, 0, nullptr, 2, barriers);
                if (logicalDevice->vkd.EndCommandBuffer(command) != VK_SUCCESS)
                    return VK_ERROR_UNKNOWN;

                VkSemaphore waits[2] = {m_syncSemaphore, inner.acquireSemaphore};
                uint64_t values[2] = {appSignal, 0};
                VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
                VkTimelineSemaphoreSubmitInfo timeline{};
                timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
                timeline.waitSemaphoreValueCount = 2;
                timeline.pWaitSemaphoreValues = values;
                VkSemaphore signal = inner.releaseSemaphore;
                VkSubmitInfo submit{};
                submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit.pNext = &timeline;
                submit.waitSemaphoreCount = 2;
                submit.pWaitSemaphores = waits;
                submit.pWaitDstStageMask = stages;
                submit.commandBufferCount = 1;
                submit.pCommandBuffers = &command;
                submit.signalSemaphoreCount = 1;
                submit.pSignalSemaphores = &signal;
                if (logicalDevice->vkd.QueueSubmit(logicalDevice->queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
                    return VK_ERROR_UNKNOWN;

                VkPresentInfoKHR info{};
                info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                info.waitSemaphoreCount = 1;
                info.pWaitSemaphores = &signal;
                info.swapchainCount = 1;
                info.pSwapchains = &swapchain->handle;
                info.pImageIndices = &targetIndex;
                const VkResult result = logicalDevice->vkd.QueuePresentKHR(queue, &info);
                if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
                    return result;
            }
            return VK_SUCCESS;
        };

        if (hostLost())
        {
            m_ready.store(false, std::memory_order_release);
            const VkResult released = releaseAcquired(0);
            return released == VK_SUCCESS ? presentAfterUpload() : released;
        }

        // generated frame i is copied from the host's destination image into
        // the i-th extra acquired image; the application's own image keeps the
        // real frame and is presented last. Present order is call order, not
        // acquisition order, so the real frame never needs a second copy.
        for (uint32_t i = 0; i < insertCount; ++i)
        {
            if (hostLost())
            {
                m_ready.store(false, std::memory_order_release);
                const VkResult released = releaseAcquired(i);
                return released == VK_SUCCESS ? presentAfterUpload() : released;
            }
            InnerSyncPass& inner = pass.innerSyncPasses[i];
            const uint32_t targetIndex = acquired[i];
            VkImage target = swapchain->images[targetIndex];
            VkImage destination = m_destinationImages[i];
            VkCommandBuffer innerCmd = inner.copyCmdbuf;
            vr = logicalDevice->vkd.BeginCommandBuffer(innerCmd, &beginInfo);
            if (vr != VK_SUCCESS)
            {
                m_ready.store(false, std::memory_order_release);
                const VkResult released = releaseAcquired(i);
                return released == VK_SUCCESS ? presentAfterUpload() : released;
            }

            // the host releases each generated frame to EXTERNAL; acquire,
            // blit into the swapchain image, hand it to the compositor, and
            // give the destination back for the next batch
            VkImageMemoryBarrier innerBarriers[2] = {
                imageBarrier(destination, VK_ACCESS_NONE, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                             external, ours),
                imageBarrier(target, VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED),
            };
            logicalDevice->vkd.CmdPipelineBarrier(innerCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, transfer,
                                                  0, 0, nullptr, 0, nullptr, 2, innerBarriers);
            recordBlit(logicalDevice, innerCmd, destination, target, m_extent);
            VkImageMemoryBarrier innerBackBarriers[2] = {
                imageBarrier(target, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_NONE,
                             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                             VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED),
                imageBarrier(destination, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_NONE,
                             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                             ours, external),
            };
            logicalDevice->vkd.CmdPipelineBarrier(innerCmd, transfer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                                  0, 0, nullptr, 0, nullptr, 2, innerBackBarriers);
            if (replayOverlay && logicalDevice->imguiOverlay)
                logicalDevice->imguiOverlay->recordOverlayDrawData(
                    innerCmd, targetIndex, swapchain->imageViews[targetIndex],
                    m_extent.width, m_extent.height);
            if (logicalDevice->vkd.EndCommandBuffer(innerCmd) != VK_SUCCESS)
            {
                m_ready.store(false, std::memory_order_release);
                const VkResult released = releaseAcquired(i);
                return released == VK_SUCCESS ? presentAfterUpload() : released;
            }

            // the host signals this frame at appSignal + i + 1; the copy also
            // waits for its acquired image to be released by the display
            VkSemaphore innerWaits[2] = { m_syncSemaphore, inner.acquireSemaphore };
            uint64_t waitValues[2] = { appSignal + i + 1, 0 };
            const uint32_t waitCount = 2;
            VkTimelineSemaphoreSubmitInfo innerTimeline{};
            innerTimeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            innerTimeline.waitSemaphoreValueCount = waitCount;
            innerTimeline.pWaitSemaphoreValues = waitValues;
            VkPipelineStageFlags innerStages[2] = {
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            };
            VkSubmitInfo innerSubmit{};
            innerSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            innerSubmit.pNext = &innerTimeline;
            innerSubmit.waitSemaphoreCount = waitCount;
            innerSubmit.pWaitSemaphores = innerWaits;
            innerSubmit.pWaitDstStageMask = innerStages;
            innerSubmit.commandBufferCount = 1;
            innerSubmit.pCommandBuffers = &innerCmd;
            innerSubmit.signalSemaphoreCount = 1;
            innerSubmit.pSignalSemaphores = &inner.copySemaphore;

            vr = logicalDevice->vkd.QueueSubmit(logicalDevice->queue, 1, &innerSubmit, VK_NULL_HANDLE);
            if (vr != VK_SUCCESS)
            {
                Logger::err("FG: inner copy submit failed (" + std::to_string(vr) + ")");
                m_ready.store(false, std::memory_order_release);
                const VkResult released = releaseAcquired(i);
                return released == VK_SUCCESS ? presentAfterUpload() : released;
            }

            VkSemaphore presentWait = inner.copySemaphore;
            VkPresentInfoKHR innerPresent{};
            innerPresent.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            // the application's pNext chain (present ids, timing) belongs to
            // the real frame, which is presented last
            innerPresent.waitSemaphoreCount = 1;
            innerPresent.pWaitSemaphores = &presentWait;
            innerPresent.swapchainCount = 1;
            innerPresent.pSwapchains = &swapchain->handle;
            innerPresent.pImageIndices = &targetIndex;
            VkResult presentResult = logicalDevice->vkd.QueuePresentKHR(queue, &innerPresent);
            if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR)
            {
                Logger::err("FG: inner present failed (" + std::to_string(presentResult) + ")");
                m_ready.store(false, std::memory_order_release);
                const VkResult released = releaseAcquired(i + 1);
                return released == VK_SUCCESS ? presentAfterUpload() : released;
            }
            if (!m_fifo && m_passIntervalUs > 0)
            {
                // without FIFO nothing paces the queue: space the commits one
                // slot apart, or the compositor keeps only the last one and
                // the generated frame never reaches the screen
                std::this_thread::sleep_for(std::chrono::microseconds(
                    m_passIntervalUs / m_multiplier));
            }
        }
        FpsMetrics::addGeneratedFrames(static_cast<int>(insertCount));

        // every batch advances the timeline by the trigger plus the frames
        // the host generated; the next trigger must follow the host's last
        // value, not the trigger's (an off-by-one here stalls the host)
        m_syncValue = appSignal + generatedCount;
        m_sourceIndex ^= 1;
        m_firstSourceUpload = false;

        // the real frame is still in the application's image (the upload only
        // read it). It is presented last, after the generated ones, and only
        // has to wait for the upload; the presentation engine shows it one
        // refresh after the last generated frame.
        return presentAfterUpload();
    }

    FgSwapchain::~FgSwapchain()
    {
        destroy();
    }

    void FgSwapchain::destroy() noexcept
    {
        m_ready.store(false, std::memory_order_release);
        m_stopWarmup.store(true, std::memory_order_release);
        if (m_warmupThread.joinable())
        {
            try
            {
                m_warmupThread.join();
            }
            catch (...)
            {
            }
        }

        if (m_engineContext)
        {
            if (m_lossState)
                m_engineContext->setLossHandler({});
            m_engineContext->waitForIdle(250'000'000ull);
            m_engineContext.reset();
        }
        m_lossState.reset();
        m_engineInstance.reset();

        LogicalDevice* logicalDevice = m_logicalDevice;
        if (logicalDevice == nullptr)
            return;

        for (size_t i = 0; i < m_frameSourceImages.size(); ++i)
        {
            if (m_frameSourceImages[i] != VK_NULL_HANDLE)
                logicalDevice->vkd.DestroyImage(logicalDevice->device, m_frameSourceImages[i], nullptr);
            if (m_frameSourceMemories[i] != VK_NULL_HANDLE)
                logicalDevice->vkd.FreeMemory(logicalDevice->device, m_frameSourceMemories[i], nullptr);
        }
        m_frameSourceImages.clear();
        m_frameSourceMemories.clear();
        m_frameCopyCmdbufs.clear();

        for (auto& pass : m_syncPasses)
        {
            if (pass.presentSemaphore != VK_NULL_HANDLE)
                logicalDevice->vkd.DestroySemaphore(logicalDevice->device, pass.presentSemaphore, nullptr);
            for (auto& inner : pass.innerSyncPasses)
            {
                if (inner.acquireSemaphore != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroySemaphore(logicalDevice->device, inner.acquireSemaphore, nullptr);
                if (inner.copySemaphore != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroySemaphore(logicalDevice->device, inner.copySemaphore, nullptr);
                if (inner.releaseSemaphore != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroySemaphore(logicalDevice->device, inner.releaseSemaphore, nullptr);
            }
        }
        m_syncPasses.clear();
        if (m_syncSemaphore != VK_NULL_HANDLE)
            logicalDevice->vkd.DestroySemaphore(logicalDevice->device, m_syncSemaphore, nullptr);
        if (m_commandPool != VK_NULL_HANDLE)
            logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, m_commandPool, nullptr);
        for (uint32_t i = 0; i < 2; ++i)
        {
            if (m_sourceImages[i] != VK_NULL_HANDLE)
                logicalDevice->vkd.DestroyImage(logicalDevice->device, m_sourceImages[i], nullptr);
            if (m_sourceMemories[i] != VK_NULL_HANDLE)
                logicalDevice->vkd.FreeMemory(logicalDevice->device, m_sourceMemories[i], nullptr);
            m_sourceImages[i] = VK_NULL_HANDLE;
            m_sourceMemories[i] = VK_NULL_HANDLE;
        }
        for (size_t i = 0; i < m_destinationImages.size(); ++i)
        {
            if (m_destinationImages[i] != VK_NULL_HANDLE)
                logicalDevice->vkd.DestroyImage(logicalDevice->device, m_destinationImages[i], nullptr);
            if (m_destinationMemories[i] != VK_NULL_HANDLE)
                logicalDevice->vkd.FreeMemory(logicalDevice->device, m_destinationMemories[i], nullptr);
        }
        m_destinationImages.clear();
        m_destinationMemories.clear();
        m_syncSemaphore = VK_NULL_HANDLE;
        m_commandPool = VK_NULL_HANDLE;
    }
} // namespace VKIntox::FrameGen
