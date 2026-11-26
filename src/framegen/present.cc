#include "framegen/present.hh"
#include "framegen/engine.hh"

#include "render/device/logical_device.hh"
#include "render/resources/format.hh"
#include "render/swapchain/logical_swapchain.hh"
#include "util/logger.hh"
#include "overlay/shell/fps_metrics.hh"

#include <algorithm>
#include <chrono>
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

        // an exchange image, created for concurrent sharing between the game's
        // queue and the external host, with dedicated memory exported over an
        // opaque fd
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
        // the pool is sized for the largest multiplier so a live settings
        // change never has to wait for the application to recreate its swapchain
        if (profile.multiplier < 2 || !engineAvailable() || !deviceSupportsFgExtensions(logicalDevice))
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
        if (!createEngineObjects(logicalDevice, swapchain, profile))
        {
            destroy();
            throw std::runtime_error("FG engine objects failed to initialize");
        }
    }

    bool FgSwapchain::createEngineObjects(LogicalDevice* logicalDevice, LogicalSwapchain* swapchain, const FgProfile& profile)
    {
        Logger::info("FG: engine instance on '" + logicalDevice->gpuName + "'");
        try
        {
            m_engineInstance = std::make_unique<EngineInstance>(logicalDevice);
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

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = logicalDevice->queueFamilyIndex;
        if (logicalDevice->vkd.CreateCommandPool(logicalDevice->device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS)
        {
            Logger::err("FG: command pool creation failed");
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
            if (logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &allocInfo, &pass.realCopyCmdbuf) != VK_SUCCESS)
            {
                Logger::err("FG: real frame command buffer allocation failed");
                return false;
            }
            pass.innerSyncPasses.resize(generatedCount);
            for (auto& inner : pass.innerSyncPasses)
            {
                if (logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &allocInfo, &inner.copyCmdbuf) != VK_SUCCESS)
                {
                    Logger::err("FG: inner command buffer allocation failed");
                    return false;
                }
                inner.acquireSemaphore = createBinarySemaphore(logicalDevice);
                inner.copySemaphore = createBinarySemaphore(logicalDevice);
                if (inner.acquireSemaphore == VK_NULL_HANDLE || inner.copySemaphore == VK_NULL_HANDLE)
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
                *m_engineInstance,
                sourceFds[0], sourceFds[1], destFds, syncFd,
                m_extent.width, m_extent.height,
                highPrecision, flow, profile.performanceMode);
        }
        catch (const std::exception& e)
        {
            Logger::err("FG: engine context failed: " + std::string(e.what()));
            return false;
        }
        // the backend took ownership of every descriptor on success
        (void)sourceFds;
        (void)destFds;

        Logger::info("FG: frame generation ready (x" + std::to_string(m_multiplier) + ")");

        // warmup: the host compiles its pipelines on the first schedule, which
        // can take seconds. Doing it here with blank frames keeps that cost out
        // of the first game frame, where a slow queue submission would break the
        // depth rebuild's bounded idle.
        const uint32_t ours = logicalDevice->queueFamilyIndex;
        const uint32_t external = externalQueueFamily();
        const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkCommandBuffer warmCmd = m_syncPasses[0].uploadCmdbuf;
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (logicalDevice->vkd.BeginCommandBuffer(warmCmd, &beginInfo) == VK_SUCCESS)
        {
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
            logicalDevice->vkd.EndCommandBuffer(warmCmd);

            VkSemaphore warmSignals[1] = { m_syncSemaphore };
            uint64_t warmSignalValues[1] = { 1 };
            VkTimelineSemaphoreSubmitInfo warmTimeline{};
            warmTimeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            warmTimeline.signalSemaphoreValueCount = 1;
            warmTimeline.pSignalSemaphoreValues = warmSignalValues;
            VkSubmitInfo warmSubmit{};
            warmSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            warmSubmit.pNext = &warmTimeline;
            warmSubmit.commandBufferCount = 1;
            VkCommandBuffer cmd = warmCmd;
            warmSubmit.pCommandBuffers = &cmd;
            warmSubmit.signalSemaphoreCount = 1;
            warmSubmit.pSignalSemaphores = warmSignals;
            if (logicalDevice->vkd.QueueSubmit(logicalDevice->queue, 1, &warmSubmit, VK_NULL_HANDLE) == VK_SUCCESS)
            {
                try
                {
                    m_engineContext->scheduleFrames();
                    m_engineContext->waitForIdle();
                    // the host signaled its warmup frames; the counter advances
                    // past them so the first real frame continues the protocol
                    m_syncValue = 1 + generatedCount;
                    // trigger 2 swaps the pair, so the first real frame lands
                    // in the second source image
                    m_sourceIndex = 1;
                    m_ready = true;
                    Logger::info("FG: engine warmup complete");
                }
                catch (const std::exception& e)
                {
                    Logger::warn(std::string("FG: warmup failed; frame generation stays off: ")
                                 + e.what());
                }
            }
        }
        return true;
    }

    VkResult FgSwapchain::present(LogicalDevice* logicalDevice,
                                  LogicalSwapchain* swapchain,
                                  uint32_t index,
                                  VkQueue queue,
                                  VkSemaphore waitSemaphore,
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
        if (!m_ready || index >= m_syncPasses.size() || m_engineContext == nullptr)
            return presentComposited();
        SyncPass& pass = m_syncPasses[index];
        VkImage& presentImage = swapchain->images[index];
        VkImage source = m_sourceImages[m_sourceIndex];
        VkImage previousSource = m_sourceImages[m_sourceIndex ^ 1];
        const uint32_t generatedCount = m_multiplier - 1;
        // Once the upload is queued its timeline signal is the only handle on
        // the frame, so fallbacks wait that value instead of the consumed
        // composited semaphore.
        const auto presentAfterUpload = [&](uint64_t value) -> VkResult {
            VkCommandBuffer cmd = pass.realCopyCmdbuf;
            VkCommandBufferBeginInfo cbInfo{};
            cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            cbInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (logicalDevice->vkd.BeginCommandBuffer(cmd, &cbInfo) != VK_SUCCESS)
                return presentComposited();
            logicalDevice->vkd.EndCommandBuffer(cmd);
            VkTimelineSemaphoreSubmitInfo timeline{};
            timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timeline.waitSemaphoreValueCount = 1;
            timeline.pWaitSemaphoreValues = &value;
            VkPipelineStageFlags stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.pNext = &timeline;
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &m_syncSemaphore;
            submit.pWaitDstStageMask = &stages;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &pass.presentSemaphore;
            if (logicalDevice->vkd.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
                return presentComposited();
            VkPresentInfoKHR info = fallback;
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores = &pass.presentSemaphore;
            return logicalDevice->vkd.QueuePresentKHR(queue, &info);
        };
        const uint32_t ours = logicalDevice->queueFamilyIndex;
        const uint32_t external = externalQueueFamily();
        const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;

        // Learn the application's own cadence first. If inserting later makes
        // its presents slower, the display had no headroom: back off instead
        // of halving the game's frame rate.
        const auto presentNow = std::chrono::steady_clock::now();
        uint64_t presentGapUs = 0;
        if (m_lastPresentTime.time_since_epoch().count() != 0)
        {
            presentGapUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                presentNow - m_lastPresentTime).count());
        }
        m_lastPresentTime = presentNow;
        const bool hadHeadroomLastFrame = m_lastInserted;
        m_lastInserted = false;
        // the pacing sleep is inside the previous present, so subtract it before
        // judging the application's own cadence; otherwise the sleep that makes
        // generated frames visible looks like the game slowing down
        uint64_t pacingUs = 0;
        if (hadHeadroomLastFrame && m_passIntervalUs > 0 && m_multiplier > 1)
            pacingUs = (m_passIntervalUs * (m_multiplier - 1)) / m_multiplier;
        const uint64_t appGapUs = presentGapUs > pacingUs ? presentGapUs - pacingUs : 0;
        // Under FIFO a slower app cadence while inserting is the intended
        // back-pressure (the game runs at refresh / multiplier), not a lack of
        // headroom. Backing off there toggled FG on and off every couple of
        // seconds, which is the stutter that made x2 feel worse than x1.
        if (m_fifo)
        {
            m_slowStreak = 0;
        }
        else if (hadHeadroomLastFrame && m_passIntervalUs > 0
            && appGapUs > std::max<uint64_t>(m_passIntervalUs * 3 / 2 + 1000, m_passIntervalUs + 6000)
            && presentGapUs < 500000)
        {
            Logger::debug("FG: cadence slip gap=" + std::to_string(presentGapUs)
                + " app=" + std::to_string(appGapUs)
                + " pass=" + std::to_string(m_passIntervalUs)
                + " streak=" + std::to_string(m_slowStreak + 1));
            if (++m_slowStreak >= 3)
            {
                m_probeCooldown = 120;
                m_slowStreak = 0;
                Logger::debug("FG: headroom probe backing off for 120 presents");
            }
        }
        else if (!hadHeadroomLastFrame && presentGapUs >= 2000 && presentGapUs <= 200000)
        {
            m_slowStreak = 0;
            m_passIntervalUs = m_passIntervalUs == 0
                ? presentGapUs
                : (m_passIntervalUs * 7 + presentGapUs) / 8;
        }
        else
        {
            m_slowStreak = 0;
        }
        if (presentGapUs > 150000)
        {
            // the surface was hidden or the app paused: the host history is
            // from another era, so restart it instead of interpolating across
            m_firstSourceUpload = true;
            m_probeCooldown = 60;
            m_slowStreak = 0;
        }
        if (m_warmupFrames > 0)
        {
            --m_warmupFrames;
            // skipped frames never reach the host, so the next insertion must
            // not interpolate against a stale source
            m_firstSourceUpload = true;
            return presentComposited();
        }

        // One extra image per generated frame. Under FIFO the acquire blocks
        // until the display frees one, which caps the game at refresh /
        // multiplier: that is the intended pacing, and every presented frame
        // gets its own refresh. Without FIFO a short timeout keeps insertion
        // from stealing the application's own vblanks.
        std::vector<uint32_t> acquired;
        acquired.reserve(generatedCount);
        if (m_probeCooldown > 0)
        {
            --m_probeCooldown;
        }
        else
        {
            for (uint32_t j = 0; j < generatedCount; ++j)
            {
                VkSemaphore acquire = pass.innerSyncPasses[j].acquireSemaphore;
                uint32_t acquiredIndex = 0;
                // under FIFO the acquire blocking until the display frees an
                // image IS the pacing; give it a few refreshes before falling
                // back, or a 30 Hz stretch would read as "no headroom"
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
                m_ready = false;
                return presentComposited();
            }
            if (acquired.empty())
                m_probeCooldown = 30;
        }
        if (acquired.empty())
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
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_GENERAL,
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
        backBarriers[backBarrierCount++] =
            imageBarrier(presentImage, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_NONE,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        logicalDevice->vkd.CmdPipelineBarrier(uploadCmd, transfer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                              0, 0, nullptr, 0, nullptr,
                                              backBarrierCount, backBarriers);
        logicalDevice->vkd.EndCommandBuffer(uploadCmd);

        const uint64_t appSignal = m_syncValue + 1;
        uint64_t signalValues[1] = { appSignal };
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
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues = signalValues;
        VkPipelineStageFlags waitStages[2] = {
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        };
        VkSemaphore signalSemaphores[1] = { m_syncSemaphore };
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = &timelineInfo;
        submitInfo.waitSemaphoreCount = waitCount;
        submitInfo.pWaitSemaphores = waitCount > 0 ? waits : nullptr;
        submitInfo.pWaitDstStageMask = waitCount > 0 ? waitStages : nullptr;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &uploadCmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;
        vr = logicalDevice->vkd.QueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        if (vr != VK_SUCCESS)
        {
            Logger::err("FG: upload submit failed (" + std::to_string(vr) + ")");
            return presentComposited();
        }
        // the host rejects a new schedule while its previous batch's GPU work
        // is still running, so the present thread waits it out here; the inner
        // frames' timeline waits then gate the GPU work in order
        bool hostIdle = false;
        try
        {
            hostIdle = m_engineContext->waitForIdle();
            if (hostIdle)
                m_engineContext->scheduleFrames();
        }
        catch (const std::exception& e)
        {
            Logger::err("FG: host schedule failed: " + std::string(e.what()));
            hostIdle = false;
        }
        if (!hostIdle)
        {
            Logger::err("FG: host unavailable; frame generation stops until its settings change");
            m_ready = false;
            m_syncValue = appSignal;
            return presentAfterUpload(appSignal);
        }
        Logger::debug("FG: batch scheduled after host idle");
        m_probeCooldown = 0;
        m_lastInserted = true;
        const uint32_t insertCount = static_cast<uint32_t>(acquired.size());
        Logger::debug("FG: insert batch " + std::to_string(insertCount) + "/"
            + std::to_string(generatedCount) + " cooldown=" + std::to_string(m_probeCooldown));

        // generated frame i is copied from the host's destination image into
        // the i-th extra acquired image; the application's own image keeps the
        // real frame and is presented last. Present order is call order, not
        // acquisition order, so the real frame never needs a second copy.
        for (uint32_t i = 0; i < insertCount; ++i)
        {
            InnerSyncPass& inner = pass.innerSyncPasses[i];
            const uint32_t targetIndex = acquired[i];
            VkImage target = swapchain->images[targetIndex];
            VkImage destination = m_destinationImages[i];
            VkCommandBuffer innerCmd = inner.copyCmdbuf;
            vr = logicalDevice->vkd.BeginCommandBuffer(innerCmd, &beginInfo);
            if (vr != VK_SUCCESS)
                return presentAfterUpload(appSignal);

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
            logicalDevice->vkd.EndCommandBuffer(innerCmd);

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

            vr = logicalDevice->vkd.QueueSubmit(queue, 1, &innerSubmit, VK_NULL_HANDLE);
            if (vr != VK_SUCCESS)
            {
                Logger::err("FG: inner copy submit failed (" + std::to_string(vr) + ")");
                m_ready = false;
                return presentAfterUpload(appSignal);
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
                m_ready = false;
                return presentAfterUpload(appSignal);
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
        return presentAfterUpload(appSignal);
    }

    FgSwapchain::~FgSwapchain()
    {
        destroy();
    }

    void FgSwapchain::destroy()
    {
        // the host may still be reading the exchange images even when the app
        // queue went idle; let it finish before the engine and images go away
        if (m_engineContext)
            m_engineContext->waitForIdle();
        m_engineContext.reset();
        m_engineInstance.reset();

        LogicalDevice* logicalDevice = m_logicalDevice;
        if (logicalDevice == nullptr)
            return;

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
