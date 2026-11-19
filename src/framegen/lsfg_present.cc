#include "framegen/lsfg_present.hh"
#include "framegen/lsfg_engine.hh"

#include "render/device/logical_device.hh"
#include "render/resources/format.hh"
#include "render/swapchain/logical_swapchain.hh"
#include "util/logger.hh"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace VKIntox::FrameGen
{
    namespace
    {
        bool deviceSupportsLsfgExtensions(LogicalDevice* logicalDevice)
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
            bool memory = false, semaphoreFd = false, timeline = false;
            for (const auto& p : props)
            {
                memory |= std::strcmp(p.extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0;
                semaphoreFd |= std::strcmp(p.extensionName, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) == 0;
                timeline |= std::strcmp(p.extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0;
            }
            return memory && semaphoreFd && timeline;
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

        bool importEngineSemaphoreFd(LogicalDevice* logicalDevice, VkSemaphore semaphore, int fd)
        {
            if (logicalDevice->vkd.ImportSemaphoreFdKHR == nullptr)
                return false;
            VkImportSemaphoreFdInfoKHR importInfo{};
            importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
            importInfo.semaphore = semaphore;
            importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
            importInfo.fd = fd;
            return logicalDevice->vkd.ImportSemaphoreFdKHR(logicalDevice->device, &importInfo) == VK_SUCCESS;
        }

        bool importImage(LogicalDevice* logicalDevice,
                         uint32_t width, uint32_t height, uint32_t layers,
                         VkFormat format, VkImageUsageFlags usage,
                         int fd, VkImage& outImage, VkDeviceMemory& outMemory)
        {
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
            imageInfo.arrayLayers = layers;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = usage;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VkImage image = VK_NULL_HANDLE;
            if (logicalDevice->vkd.CreateImage(logicalDevice->device, &imageInfo, nullptr, &image) != VK_SUCCESS)
                return false;

            VkMemoryRequirements requirements{};
            logicalDevice->vkd.GetImageMemoryRequirements(logicalDevice->device, image, &requirements);

            VkMemoryDedicatedAllocateInfo dedicatedInfo{};
            dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            dedicatedInfo.image = image;
            VkImportMemoryFdInfoKHR importInfo{};
            importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
            importInfo.pNext = &dedicatedInfo;
            importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            importInfo.fd = fd;

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
            allocInfo.pNext = &importInfo;
            allocInfo.allocationSize = requirements.size;
            allocInfo.memoryTypeIndex = static_cast<uint32_t>(memoryType);

            VkDeviceMemory memory = VK_NULL_HANDLE;
            if (logicalDevice->vkd.AllocateMemory(logicalDevice->device, &allocInfo, nullptr, &memory) != VK_SUCCESS)
            {
                Logger::err("LSFG: engine memory import failed");
                logicalDevice->vkd.DestroyImage(logicalDevice->device, image, nullptr);
                return false;
            }
            if (logicalDevice->vkd.BindImageMemory(logicalDevice->device, image, memory, 0) != VK_SUCCESS)
            {
                logicalDevice->vkd.FreeMemory(logicalDevice->device, memory, nullptr);
                logicalDevice->vkd.DestroyImage(logicalDevice->device, image, nullptr);
                return false;
            }
            outImage = image;
            outMemory = memory;
            return true;
        }

        VkImageMemoryBarrier imageBarrier(VkImage image,
                                          VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                                          VkImageLayout oldLayout, VkImageLayout newLayout,
                                          uint32_t baseLayer = 0, uint32_t layerCount = 1)
        {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = srcAccess;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = baseLayer;
            barrier.subresourceRange.layerCount = layerCount;
            return barrier;
        }

        void recordBlit(LogicalDevice* logicalDevice, VkCommandBuffer cmd,
                        VkImage from, uint32_t fromLayer, VkImage to, uint32_t toLayer,
                        VkExtent2D extent)
        {
            VkImageBlit region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, fromLayer, 1};
            region.srcOffsets[1] = {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, toLayer, 1};
            region.dstOffsets[1] = {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1};
            logicalDevice->vkd.CmdBlitImage(cmd, from, VK_IMAGE_LAYOUT_GENERAL,
                                            to, VK_IMAGE_LAYOUT_GENERAL,
                                            1, &region, VK_FILTER_LINEAR);
        }

        void recordPipelineBarrier(LogicalDevice* logicalDevice, VkCommandBuffer cmd,
                                   const VkImageMemoryBarrier* barriers, uint32_t count,
                                   VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
        {
            logicalDevice->vkd.CmdPipelineBarrier(cmd, srcStage, dstStage,
                                                  0, 0, nullptr, 0, nullptr, count, barriers);
        }

        VkFormat engineFormatFor(VkFormat swapchainFormat)
        {
            if (swapchainFormat == VK_FORMAT_A2B10G10R10_UNORM_PACK32
                || swapchainFormat == VK_FORMAT_A2R10G10B10_UNORM_PACK32)
                return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            if (swapchainFormat == VK_FORMAT_R16G16B16A16_SFLOAT)
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            if (isSRGB(swapchainFormat))
                return VK_FORMAT_R8G8B8A8_SRGB;
            return VK_FORMAT_R8G8B8A8_UNORM;
        }
    }

    bool modifySwapchainCreateInfo(LogicalDevice* logicalDevice,
                                   VkSwapchainCreateInfoKHR& info,
                                   const LsfgProfile& profile)
    {
        if (!profile.enabled || profile.multiplier < 2)
            return false;
        if (!LsfgSwapchain::active(logicalDevice, profile))
            return false;

        info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        // inserted frames plus the frame the compositor holds back; capped the
        // same way lsfg-vk caps it so drivers with small pools stay happy
        VkSurfaceCapabilitiesKHR caps{};
        if (logicalDevice->vki.GetPhysicalDeviceSurfaceCapabilitiesKHR)
            logicalDevice->vki.GetPhysicalDeviceSurfaceCapabilitiesKHR(
                logicalDevice->physicalDevice, info.surface, &caps);
        info.minImageCount += profile.multiplier;
        info.minImageCount = std::min<uint32_t>({info.minImageCount, 8u,
                                                 caps.maxImageCount == 0 ? 999u : caps.maxImageCount});
        Logger::info("LSFG: swapchain image pool grown to " + std::to_string(info.minImageCount)
                     + " for a x" + std::to_string(profile.multiplier) + " multiplier");
        return true;
    }

    bool LsfgSwapchain::active(LogicalDevice* logicalDevice, const LsfgProfile& profile)
    {
        if (!profile.enabled || profile.multiplier < 2)
            return false;
        if (!engineAvailable())
            return false;
        if (!deviceSupportsLsfgExtensions(logicalDevice))
            return false;
        return true;
    }

    LsfgSwapchain::LsfgSwapchain(LogicalDevice* logicalDevice, LogicalSwapchain* swapchain, const LsfgProfile& profile)
        : m_logicalDevice(logicalDevice), m_multiplier(profile.multiplier), m_extent(swapchain->imageExtent)
    {
        if (!createEngineObjects(logicalDevice, swapchain, profile))
        {
            destroy();
            throw std::runtime_error("LSFG engine objects failed to initialize");
        }
    }

    bool LsfgSwapchain::createEngineObjects(LogicalDevice* logicalDevice, LogicalSwapchain* swapchain, const LsfgProfile& profile)
    {
        VkPhysicalDeviceProperties props{};
        logicalDevice->vki.GetPhysicalDeviceProperties(logicalDevice->physicalDevice, &props);
        std::string deviceId = logicalDevice->gpuPciSlot.empty() ? std::string(props.deviceName) : logicalDevice->gpuPciSlot;
        Logger::info("LSFG: engine instance on '" + deviceId + "'");

        try
        {
            m_engineInstance = std::make_unique<EngineInstance>(
                deviceId,
                std::filesystem::path(profile.dllPath),
                profile.halfPrecision,
                [](const std::string& message) { Logger::debug("lsfg-vk: " + message); });
        }
        catch (const std::exception& e)
        {
            Logger::err("LSFG: engine instance failed: " + std::string(e.what()));
            return false;
        }

        try
        {
            m_engineContext = std::make_unique<EngineContext>(
                *m_engineInstance,
                m_extent.width, m_extent.height,
                profile.flowScale, profile.performanceMode);
        }
        catch (const std::exception& e)
        {
            Logger::err("LSFG: engine context failed: " + std::string(e.what()));
            return false;
        }

        FileDescriptors fds = m_engineContext->exportFds();

        // created exportable then imported over: the import replaces the
        // payload with the engine's timeline semaphore
        VkSemaphoreCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkExportSemaphoreCreateInfo exportInfo{};
        exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        createInfo.pNext = &exportInfo;
        if (logicalDevice->vkd.CreateSemaphore(logicalDevice->device, &createInfo, nullptr, &m_syncSemaphore) != VK_SUCCESS
            || !importEngineSemaphoreFd(logicalDevice, m_syncSemaphore, fds.syncFd))
        {
            Logger::err("LSFG: engine semaphore import failed");
            return false;
        }

        VkFormat format = engineFormatFor(swapchain->format);
        if (!importImage(logicalDevice, m_extent.width, m_extent.height, 2, format,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT, fds.sourceFd,
                         m_sourceImage, m_sourceMemory))
        {
            Logger::err("LSFG: source image import failed");
            return false;
        }
        if (!importImage(logicalDevice, m_extent.width, m_extent.height, 1, format,
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT, fds.destinationFd,
                         m_destinationImage, m_destinationMemory))
        {
            Logger::err("LSFG: destination image import failed");
            return false;
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (logicalDevice->vkd.CreateFence(logicalDevice->device, &fenceInfo, nullptr, &m_completionFence) != VK_SUCCESS)
        {
            Logger::err("LSFG: completion fence creation failed");
            return false;
        }

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = logicalDevice->queueFamilyIndex;
        if (logicalDevice->vkd.CreateCommandPool(logicalDevice->device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS)
        {
            Logger::err("LSFG: command pool creation failed");
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
            if (logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &allocInfo, &pass.copyCmdbuf) != VK_SUCCESS)
            {
                Logger::err("LSFG: command buffer allocation failed");
                return false;
            }
            pass.presentSemaphore = createBinarySemaphore(logicalDevice);
            pass.innerSyncPasses.resize(m_multiplier - 1);
            for (auto& inner : pass.innerSyncPasses)
            {
                if (logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &allocInfo, &inner.copyCmdbuf) != VK_SUCCESS)
                {
                    Logger::err("LSFG: inner command buffer allocation failed");
                    return false;
                }
                inner.acquireSemaphore = createBinarySemaphore(logicalDevice);
                inner.copySemaphore = createBinarySemaphore(logicalDevice);
                if (inner.acquireSemaphore == VK_NULL_HANDLE || inner.copySemaphore == VK_NULL_HANDLE)
                    return false;
            }
        }
        Logger::info("LSFG: frame generation ready (x" + std::to_string(m_multiplier) + ")");
        return true;
    }

    VkResult LsfgSwapchain::present(LogicalDevice* logicalDevice,
                                    LogicalSwapchain* swapchain,
                                    uint32_t index,
                                    VkQueue queue,
                                    VkSemaphore waitSemaphore,
                                    VkSemaphore originalWaitSemaphore,
                                    const VkPresentInfoKHR* originalPresentInfo)
    {
        if (index >= m_syncPasses.size() || m_engineContext == nullptr)
            return logicalDevice->vkd.QueuePresentKHR(queue, originalPresentInfo);
        SyncPass& pass = m_syncPasses[index];
        VkImage& presentImage = swapchain->images[index];

        // previous generation must be complete before its images are reused
        if (m_fenceArmed)
        {
            VkResult waitResult = logicalDevice->vkd.WaitForFences(
                logicalDevice->device, 1, &m_completionFence, VK_TRUE, 1'000'000'000ull);
            if (waitResult != VK_SUCCESS)
                Logger::warn("LSFG: previous generation wait returned " + std::to_string(waitResult));
            logicalDevice->vkd.ResetFences(logicalDevice->device, 1, &m_completionFence);
            m_fenceArmed = false;
        }

        // copy the composited frame into the engine's source image, chained on
        // the overlay's completion and the app's original dependency
        VkCommandBuffer copyCmd = pass.copyCmdbuf;
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult vr = logicalDevice->vkd.BeginCommandBuffer(copyCmd, &beginInfo);
        if (vr != VK_SUCCESS)
            return logicalDevice->vkd.QueuePresentKHR(queue, originalPresentInfo);

        VkImageMemoryBarrier copyBarriers[2] = {
            imageBarrier(presentImage,
                         VK_ACCESS_NONE, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_GENERAL),
            imageBarrier(m_sourceImage,
                         VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         m_iteration % 2, 1),
        };
        recordPipelineBarrier(logicalDevice, copyCmd, copyBarriers, 2,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        recordBlit(logicalDevice, copyCmd, presentImage, 0,
                   m_sourceImage, m_iteration % 2, m_extent);
        VkImageMemoryBarrier backBarrier = imageBarrier(presentImage,
                                                        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_NONE,
                                                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        recordPipelineBarrier(logicalDevice, copyCmd, &backBarrier, 1,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        logicalDevice->vkd.EndCommandBuffer(copyCmd);

        VkSemaphore waits[2] = { waitSemaphore, originalWaitSemaphore };
        uint32_t waitCount = 2;
        if (originalWaitSemaphore == VK_NULL_HANDLE)
            waitCount = 1;
        else if (waitSemaphore == VK_NULL_HANDLE)
        {
            waits[0] = originalWaitSemaphore;
            waitCount = 1;
        }

        uint64_t signalValues[1] = { m_syncValue + 1 };
        VkTimelineSemaphoreSubmitInfo timelineInfo{};
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
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
        submitInfo.pWaitSemaphores = waits;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &copyCmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;
        vr = logicalDevice->vkd.QueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        if (vr != VK_SUCCESS)
        {
            Logger::err("LSFG: copy submit failed (" + std::to_string(vr) + ")");
            return logicalDevice->vkd.QueuePresentKHR(queue, originalPresentInfo);
        }

        m_engineContext->dispatch(m_multiplier - 1);
        m_remaining = m_multiplier - 1;
        m_syncValue++;
        m_iteration++;

        // insert the generated frames ahead of the real one
        for (uint32_t i = 0; i < m_multiplier - 1; ++i)
        {
            InnerSyncPass& inner = pass.innerSyncPasses[i];

            uint32_t acquiredIndex = 0;
            if (logicalDevice->vkd.AcquireNextImageKHR == nullptr)
                return VK_ERROR_OUT_OF_DATE_KHR;
            vr = logicalDevice->vkd.AcquireNextImageKHR(
                logicalDevice->device, swapchain->handle,
                UINT64_MAX, inner.acquireSemaphore, VK_NULL_HANDLE, &acquiredIndex);
            if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR)
            {
                Logger::err("LSFG: inner acquire failed (" + std::to_string(vr) + ")");
                return vr;
            }

            VkImage innerImage = swapchain->images[acquiredIndex];
            VkCommandBuffer innerCmd = inner.copyCmdbuf;
            vr = logicalDevice->vkd.BeginCommandBuffer(innerCmd, &beginInfo);
            if (vr != VK_SUCCESS)
                return vr;

            VkImageMemoryBarrier innerBarriers[2] = {
                imageBarrier(m_destinationImage,
                             VK_ACCESS_NONE, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
                imageBarrier(innerImage,
                             VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
            };
            recordPipelineBarrier(logicalDevice, innerCmd, innerBarriers, 2,
                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            recordBlit(logicalDevice, innerCmd, m_destinationImage, 0, innerImage, 0, m_extent);
            VkImageMemoryBarrier innerBack = imageBarrier(innerImage,
                                                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                                                          VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            recordPipelineBarrier(logicalDevice, innerCmd, &innerBack, 1,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            logicalDevice->vkd.EndCommandBuffer(innerCmd);

            const bool last = (i == m_multiplier - 2);
            VkSemaphore innerWaits[2] = { inner.acquireSemaphore, m_syncSemaphore };
            VkSemaphore innerSignals[2] = { inner.copySemaphore, m_syncSemaphore };
            // the binary acquire semaphore ignores its wait value
            uint64_t waitValues[2] = { 0, m_syncValue + 1 };
            uint64_t signalValues[1] = { m_syncValue + 2 };

            VkTimelineSemaphoreSubmitInfo innerTimeline{};
            innerTimeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            innerTimeline.waitSemaphoreValueCount = 2;
            innerTimeline.pWaitSemaphoreValues = waitValues;
            innerTimeline.signalSemaphoreValueCount = last ? 0u : 1u;
            innerTimeline.pSignalSemaphoreValues = signalValues;

            VkPipelineStageFlags innerStages[2] = {
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            };
            VkSubmitInfo innerSubmit{};
            innerSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            innerSubmit.pNext = &innerTimeline;
            innerSubmit.waitSemaphoreCount = 2;
            innerSubmit.pWaitSemaphores = innerWaits;
            innerSubmit.pWaitDstStageMask = innerStages;
            innerSubmit.commandBufferCount = 1;
            innerSubmit.pCommandBuffers = &innerCmd;
            innerSubmit.signalSemaphoreCount = last ? 1u : 2u;
            innerSubmit.pSignalSemaphores = innerSignals;

            vr = logicalDevice->vkd.QueueSubmit(queue, 1, &innerSubmit,
                                                last ? m_completionFence : VK_NULL_HANDLE);
            if (vr != VK_SUCCESS)
            {
                Logger::err("LSFG: inner copy submit failed (" + std::to_string(vr) + ")");
                return vr;
            }
            if (last)
                m_fenceArmed = true;

            VkSemaphore presentWait = inner.copySemaphore;
            VkPresentInfoKHR innerPresent{};
            innerPresent.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            if (i == 0)
                innerPresent.pNext = originalPresentInfo->pNext;
            innerPresent.waitSemaphoreCount = 1;
            innerPresent.pWaitSemaphores = &presentWait;
            innerPresent.swapchainCount = 1;
            innerPresent.pSwapchains = &swapchain->handle;
            innerPresent.pImageIndices = &acquiredIndex;
            VkResult presentResult = logicalDevice->vkd.QueuePresentKHR(queue, &innerPresent);
            if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR)
                return presentResult;

            m_engineContext->acquire();
            m_remaining--;
            m_syncValue++;
            if (m_remaining > 0)
                m_syncValue++;
        }

        // finally the real composited frame, waiting for the last insertion
        VkSemaphore finalWait = pass.presentSemaphore;
        VkPresentInfoKHR finalPresent = *originalPresentInfo;
        finalPresent.waitSemaphoreCount = 1;
        finalPresent.pWaitSemaphores = &finalWait;
        return logicalDevice->vkd.QueuePresentKHR(queue, &finalPresent);
    }

    LsfgSwapchain::~LsfgSwapchain()
    {
        destroy();
    }

    void LsfgSwapchain::destroy()
    {
        // the engine context dtor calls idle() and waits for the last generation
        m_engineContext.reset();
        m_engineInstance.reset();

        LogicalDevice* logicalDevice = m_logicalDevice;
        if (logicalDevice == nullptr)
            return;

        for (auto& pass : m_syncPasses)
        {
            for (auto& inner : pass.innerSyncPasses)
            {
                if (inner.acquireSemaphore != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroySemaphore(logicalDevice->device, inner.acquireSemaphore, nullptr);
                if (inner.copySemaphore != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroySemaphore(logicalDevice->device, inner.copySemaphore, nullptr);
            }
            if (pass.presentSemaphore != VK_NULL_HANDLE)
                logicalDevice->vkd.DestroySemaphore(logicalDevice->device, pass.presentSemaphore, nullptr);
        }
        m_syncPasses.clear();
        if (m_syncSemaphore != VK_NULL_HANDLE)
            logicalDevice->vkd.DestroySemaphore(logicalDevice->device, m_syncSemaphore, nullptr);
        if (m_completionFence != VK_NULL_HANDLE)
            logicalDevice->vkd.DestroyFence(logicalDevice->device, m_completionFence, nullptr);
        if (m_commandPool != VK_NULL_HANDLE)
            logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, m_commandPool, nullptr);
        if (m_sourceImage != VK_NULL_HANDLE)
            logicalDevice->vkd.DestroyImage(logicalDevice->device, m_sourceImage, nullptr);
        if (m_sourceMemory != VK_NULL_HANDLE)
            logicalDevice->vkd.FreeMemory(logicalDevice->device, m_sourceMemory, nullptr);
        if (m_destinationImage != VK_NULL_HANDLE)
            logicalDevice->vkd.DestroyImage(logicalDevice->device, m_destinationImage, nullptr);
        if (m_destinationMemory != VK_NULL_HANDLE)
            logicalDevice->vkd.FreeMemory(logicalDevice->device, m_destinationMemory, nullptr);
        m_syncSemaphore = VK_NULL_HANDLE;
        m_completionFence = VK_NULL_HANDLE;
        m_commandPool = VK_NULL_HANDLE;
        m_sourceImage = VK_NULL_HANDLE;
        m_sourceMemory = VK_NULL_HANDLE;
        m_destinationImage = VK_NULL_HANDLE;
        m_destinationMemory = VK_NULL_HANDLE;
    }
} // namespace VKIntox::FrameGen
