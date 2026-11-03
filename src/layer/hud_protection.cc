#include "layer/hud_protection.hh"

#include "layer/internal.hh"
#include "layer/pass_log.hh"

#include "effects/runtime/hud_preserve.hh"
#include "render/command_buffer.hh"
#include "render/device/vk_handle.hh"
#include "render/resources/image.hh"
#include "render/resources/image_view.hh"
#include "util/logger.hh"
#include "util/util.hh"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace VKIntox::Hud
{
    namespace
    {
        struct Candidate
        {
            bool             active = false;
            bool             splitDone = false;
            bool             finalCopyDone = false;
            bool             seenDepthPass = false;
            uint32_t         drawsInPass = 0;
            LogicalSwapchain* owner = nullptr;
            VkRenderPass     originalPass = VK_NULL_HANDLE;
            VkRenderPass     compatiblePass = VK_NULL_HANDLE;
            VkFramebuffer    framebuffer = VK_NULL_HANDLE;
            VkRect2D         area = {};
            VkImage          colorImage = VK_NULL_HANDLE;
            VkFormat         colorFormat = VK_FORMAT_UNDEFINED;
            VkImageLayout    colorFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageLayout    colorSubpassLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkSubpassDescriptionFlags subpassFlags = 0;
            bool             splitLogged = false;
        };

        std::unordered_map<VkCommandBuffer, Candidate> g_candidates;
        std::unordered_map<VkRenderPass, VkRenderPass> g_compatiblePasses;
        std::unordered_map<VkRenderPass, LogicalDevice*> g_compatibleOwners;
        std::unordered_set<VkRenderPass> g_loggedArmed;
        std::unordered_set<VkRenderPass> g_loggedSkipped;

        std::string lowerCopy(const std::string& text)
        {
            std::string result = text;
            std::transform(result.begin(), result.end(), result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return result;
        }

        std::string trim(const std::string& text)
        {
            size_t begin = text.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
                return {};
            size_t end = text.find_last_not_of(" \t\r\n");
            return text.substr(begin, end - begin + 1);
        }

        std::string baseNameLower(const std::string& path)
        {
            size_t slash = path.find_last_of('/');
            std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
            return lowerCopy(name);
        }

        bool readFirstLine(const char* path, std::string& out)
        {
            std::ifstream file(path);
            if (!file.is_open())
                return false;
            std::getline(file, out);
            out = trim(out);
            return !out.empty();
        }

        bool readFirstArgument(const char* path, std::string& out)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
                return false;
            std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            size_t end = data.find('\0');
            out = end == std::string::npos ? data : data.substr(0, end);
            return !out.empty();
        }

        bool isRoselineProcess()
        {
            const char* flatpakId = getenv("FLATPAK_ID");
            if (flatpakId && lowerCopy(flatpakId) == "net.buwryy.roseline")
                return true;

            std::string comm;
            if (readFirstLine("/proc/self/comm", comm) && lowerCopy(comm) == "roseline")
                return true;

            std::string argument;
            if (readFirstArgument("/proc/self/cmdline", argument) && baseNameLower(argument) == "roseline")
                return true;

            char exePath[4096] = {};
            ssize_t length = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
            if (length > 0)
            {
                exePath[length] = '\0';
                if (baseNameLower(exePath) == "roseline")
                    return true;
            }
            return false;
        }

        void imageBarrier(LogicalDevice* device, VkCommandBuffer commandBuffer, VkImage image,
                          VkImageLayout oldLayout, VkImageLayout newLayout,
                          VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                          VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);
        void copyImageRegion(LogicalDevice* device, VkCommandBuffer commandBuffer,
                             VkImage src, VkImage dst, VkExtent2D extent);

        bool initializeSnapshots(LogicalDevice* device, LogicalSwapchain* swapchain)
        {
            if (!device || !swapchain || device->queue == VK_NULL_HANDLE)
                return false;

            std::vector<VkCommandBuffer> commandBuffers = allocateCommandBuffer(device, 1, device->commandPool);
            if (commandBuffers.empty() || commandBuffers[0] == VK_NULL_HANDLE)
                return false;

            VkCommandBuffer commandBuffer = commandBuffers[0];
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (device->vkd.BeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
            {
                device->vkd.FreeCommandBuffers(device->device, device->commandPool, 1, &commandBuffer);
                return false;
            }

            VkClearColorValue zero{};
            for (size_t i = 0; i < swapchain->hud.sceneImages.size(); ++i)
            {
                VkImage images[2] = {swapchain->hud.sceneImages[i], swapchain->hud.finalImages[i]};
                for (VkImage image : images)
                {
                    if (image == VK_NULL_HANDLE)
                        continue;
                    imageBarrier(device, commandBuffer, image,
                                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    VkImageSubresourceRange range{};
                    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    range.baseMipLevel = 0;
                    range.levelCount = 1;
                    range.baseArrayLayer = 0;
                    range.layerCount = 1;
                    device->vkd.CmdClearColorImage(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                    &zero, 1, &range);
                    imageBarrier(device, commandBuffer, image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                }
            }

            VkImage pendingImages[2] = {swapchain->hud.pendingFrame.sceneImage,
                                        swapchain->hud.pendingFrame.finalImage};
            for (VkImage image : pendingImages)
            {
                if (image == VK_NULL_HANDLE)
                    continue;
                imageBarrier(device, commandBuffer, image,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             0, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkImageSubresourceRange range{};
                range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                range.baseMipLevel = 0;
                range.levelCount = 1;
                range.baseArrayLayer = 0;
                range.layerCount = 1;
                device->vkd.CmdClearColorImage(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                &zero, 1, &range);
                imageBarrier(device, commandBuffer, image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            }

            if (device->vkd.EndCommandBuffer(commandBuffer) != VK_SUCCESS)
            {
                device->vkd.FreeCommandBuffers(device->device, device->commandPool, 1, &commandBuffer);
                return false;
            }

            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence fence = VK_NULL_HANDLE;
            if (device->vkd.CreateFence(device->device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
            {
                device->vkd.FreeCommandBuffers(device->device, device->commandPool, 1, &commandBuffer);
                return false;
            }

            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffer;
            bool success = false;
            if (device->vkd.QueueSubmit(device->queue, 1, &submitInfo, fence) == VK_SUCCESS)
            {
                VkResult waitResult = device->vkd.WaitForFences(device->device, 1, &fence, VK_TRUE, 1000000000ull);
                success = waitResult == VK_SUCCESS;
                if (!success)
                    Logger::warn("HUD protection: snapshot clear fence wait did not finish");
            }

            if (success)
                device->vkd.DestroyFence(device->device, fence, nullptr);
            if (success)
                device->vkd.FreeCommandBuffers(device->device, device->commandPool, 1, &commandBuffer);
            return success;
        }

        LogicalSwapchain* findSwapchainLocked(LogicalDevice* device, VkFormat format, VkExtent2D extent)
        {
            LogicalSwapchain* found = nullptr;
            for (auto& entry : swapchainMap)
            {
                LogicalSwapchain* swapchain = entry.second.get();
                if (!swapchain || swapchain->logicalDevice != device || swapchain->passthrough)
                    continue;
                if (!swapchain->hud.initialized || !swapchain->hudPreserve)
                    continue;
                if (swapchain->format != format)
                    continue;
                if (swapchain->imageExtent.width != extent.width || swapchain->imageExtent.height != extent.height)
                    continue;
                if (found)
                    return nullptr;
                found = swapchain;
            }
            return found;
        }

        void imageBarrier(LogicalDevice* device, VkCommandBuffer commandBuffer, VkImage image,
                          VkImageLayout oldLayout, VkImageLayout newLayout,
                          VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                          VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
        {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcAccessMask = srcAccess;
            barrier.dstAccessMask = dstAccess;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            device->vkd.CmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0,
                                           0, nullptr, 0, nullptr, 1, &barrier);
        }

        void copyImageRegion(LogicalDevice* device, VkCommandBuffer commandBuffer,
                             VkImage src, VkImage dst, VkExtent2D extent)
        {
            VkImageCopy region{};
            region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.srcSubresource.mipLevel = 0;
            region.srcSubresource.baseArrayLayer = 0;
            region.srcSubresource.layerCount = 1;
            region.dstSubresource = region.srcSubresource;
            region.extent = {extent.width, extent.height, 1};
            device->vkd.CmdCopyImage(commandBuffer, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }

        VkRenderPass getCompatiblePassLocked(LogicalDevice* device, Candidate& candidate)
        {
            auto cached = g_compatiblePasses.find(candidate.originalPass);
            if (cached != g_compatiblePasses.end())
            {
                candidate.compatiblePass = cached->second;
                return cached->second;
            }

            if (candidate.colorFinalLayout == VK_IMAGE_LAYOUT_UNDEFINED
                || candidate.colorSubpassLayout == VK_IMAGE_LAYOUT_UNDEFINED)
                return VK_NULL_HANDLE;

            VkAttachmentDescription attachment{};
            attachment.format = candidate.colorFormat;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = candidate.colorFinalLayout;
            attachment.finalLayout = candidate.colorFinalLayout;

            VkAttachmentReference colorReference{};
            colorReference.attachment = 0;
            colorReference.layout = candidate.colorSubpassLayout;

            VkSubpassDescription subpass{};
            subpass.flags = candidate.subpassFlags;
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
            dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

            VkRenderPassCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            createInfo.attachmentCount = 1;
            createInfo.pAttachments = &attachment;
            createInfo.subpassCount = 1;
            createInfo.pSubpasses = &subpass;
            createInfo.dependencyCount = 2;
            createInfo.pDependencies = dependencies;

            VkRenderPass compatible = VK_NULL_HANDLE;
            if (device->vkd.CreateRenderPass(device->device, &createInfo, nullptr, &compatible) != VK_SUCCESS
                || compatible == VK_NULL_HANDLE)
                return VK_NULL_HANDLE;

            g_compatiblePasses[candidate.originalPass] = compatible;
            g_compatibleOwners[candidate.originalPass] = device;
            candidate.compatiblePass = compatible;
            return compatible;
        }

        bool performSplitLocked(LogicalDevice* device, VkCommandBuffer commandBuffer, Candidate& candidate)
        {
            LogicalSwapchain* swapchain = candidate.owner;
            if (!swapchain || !swapchain->hud.initialized || !swapchain->hudPreserve)
                return false;

            VkRenderPass compatible = getCompatiblePassLocked(device, candidate);
            if (compatible == VK_NULL_HANDLE)
            {
                Logger::warn("HUD protection: compatible render pass creation failed");
                return false;
            }

            VkExtent2D extent = swapchain->imageExtent;
            HudPendingImages& pending = swapchain->hud.pendingFrame;
            if (pending.sceneImage == VK_NULL_HANDLE || pending.finalImage == VK_NULL_HANDLE)
            {
                Logger::warn("HUD protection: pending snapshot unavailable");
                return false;
            }

            device->vkd.CmdEndRenderPass(commandBuffer);

            imageBarrier(device, commandBuffer, candidate.colorImage,
                         candidate.colorFinalLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

            imageBarrier(device, commandBuffer, pending.sceneImage,
                         pending.sceneUsed ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         pending.sceneUsed ? VK_ACCESS_TRANSFER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyImageRegion(device, commandBuffer, candidate.colorImage, pending.sceneImage, extent);
            imageBarrier(device, commandBuffer, pending.sceneImage,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            pending.sceneUsed = true;

            imageBarrier(device, commandBuffer, candidate.colorImage,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, candidate.colorFinalLayout,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

            VkRenderPassBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            begin.renderPass = compatible;
            begin.framebuffer = candidate.framebuffer;
            begin.renderArea = candidate.area;
            device->vkd.CmdBeginRenderPass(commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);

            candidate.splitDone = true;
            candidate.finalCopyDone = false;
            candidate.active = false;

            if (!candidate.splitLogged)
            {
                candidate.splitLogged = true;
                Logger::info("HUD protection: split final pass at "
                             + std::to_string(candidate.area.extent.width) + "x"
                             + std::to_string(candidate.area.extent.height)
                             + " image=" + convertToString(candidate.colorImage));
            }
            return true;
        }
    }

    bool enabled()
    {
        static const bool value = [] {
            const char* env = getenv("VKINTOX_HUD_PROTECT");
            if (env)
            {
                std::string text = lowerCopy(env);
                if (text == "0" || text == "false" || text == "no" || text == "off")
                    return false;
                return true;
            }
            return isRoselineProcess();
        }();
        return value;
    }

    bool init(LogicalSwapchain* swapchain)
    {
        if (!enabled() || !swapchain || !swapchain->logicalDevice)
            return true;
        if (swapchain->hud.initialized)
            return true;

        LogicalDevice* device = swapchain->logicalDevice;
        if (swapchain->imageCount == 0 || swapchain->format == VK_FORMAT_UNDEFINED
            || swapchain->imageExtent.width == 0 || swapchain->imageExtent.height == 0
            || swapchain->images.size() < swapchain->imageCount
            || swapchain->imageViews.size() < swapchain->imageCount)
            return false;

        const uint32_t count = swapchain->imageCount;
        swapchain->hud.sceneImages.assign(count, VK_NULL_HANDLE);
        swapchain->hud.sceneViews.assign(count, VK_NULL_HANDLE);
        swapchain->hud.sceneMemory.assign(count, VK_NULL_HANDLE);
        swapchain->hud.finalImages.assign(count, VK_NULL_HANDLE);
        swapchain->hud.finalViews.assign(count, VK_NULL_HANDLE);
        swapchain->hud.finalMemory.assign(count, VK_NULL_HANDLE);
        swapchain->hud.sceneInitialized.assign(count, false);
        swapchain->hud.finalInitialized.assign(count, false);
        swapchain->hud.snapshotValid.assign(count, false);

        VkExtent3D extent{swapchain->imageExtent.width, swapchain->imageExtent.height, 1};
        for (uint32_t i = 0; i < count; ++i)
        {
            swapchain->hud.sceneImages[i] = createSingleImage(
                device, extent, swapchain->format,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, swapchain->hud.sceneMemory[i]);
            if (swapchain->hud.sceneImages[i] == VK_NULL_HANDLE)
            {
                destroy(swapchain);
                return false;
            }
            swapchain->hud.sceneViews[i] = createSingleImageView(device, swapchain->format, swapchain->hud.sceneImages[i]);
            if (swapchain->hud.sceneViews[i] == VK_NULL_HANDLE)
            {
                destroy(swapchain);
                return false;
            }

            swapchain->hud.finalImages[i] = createSingleImage(
                device, extent, swapchain->format,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, swapchain->hud.finalMemory[i]);
            if (swapchain->hud.finalImages[i] == VK_NULL_HANDLE)
            {
                destroy(swapchain);
                return false;
            }
            swapchain->hud.finalViews[i] = createSingleImageView(device, swapchain->format, swapchain->hud.finalImages[i]);
            if (swapchain->hud.finalViews[i] == VK_NULL_HANDLE)
            {
                destroy(swapchain);
                return false;
            }
        }

        swapchain->hud.pendingFrame.sceneImage = createSingleImage(
            device, extent, swapchain->format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, swapchain->hud.pendingFrame.sceneMemory);
        if (swapchain->hud.pendingFrame.sceneImage == VK_NULL_HANDLE)
        {
            destroy(swapchain);
            return false;
        }
        swapchain->hud.pendingFrame.finalImage = createSingleImage(
            device, extent, swapchain->format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, swapchain->hud.pendingFrame.finalMemory);
        if (swapchain->hud.pendingFrame.finalImage == VK_NULL_HANDLE)
        {
            destroy(swapchain);
            return false;
        }

        const bool snapshotsInitialized = initializeSnapshots(device, swapchain);
        if (snapshotsInitialized)
        {
            std::fill(swapchain->hud.sceneInitialized.begin(), swapchain->hud.sceneInitialized.end(), true);
            std::fill(swapchain->hud.finalInitialized.begin(), swapchain->hud.finalInitialized.end(), true);
            swapchain->hud.pendingFrame.sceneUsed = true;
            swapchain->hud.pendingFrame.finalUsed = true;
        }
        else
        {
            Logger::warn("HUD protection: snapshot clear skipped");
        }

        auto preserve = std::make_shared<HudPreserveEffect>(
            device, swapchain->format, swapchain->imageExtent,
            swapchain->images, swapchain->imageViews,
            swapchain->hud.sceneViews, swapchain->hud.finalViews, count);
        if (!preserve || !preserve->valid())
        {
            destroy(swapchain);
            return false;
        }

        swapchain->hudPreserve = preserve;
        swapchain->hud.hudPreserveValid = true;
        swapchain->hud.initialized = true;
        Logger::info("HUD protection enabled for ROSELINE: snapshots=" + std::to_string(count)
                     + " format=" + std::to_string(static_cast<uint32_t>(swapchain->format))
                     + " extent=" + std::to_string(swapchain->imageExtent.width) + "x"
                     + std::to_string(swapchain->imageExtent.height));
        return true;
    }

    void destroy(LogicalSwapchain* swapchain)
    {
        if (!swapchain)
            return;

        for (auto it = g_candidates.begin(); it != g_candidates.end();)
        {
            if (it->second.owner == swapchain)
                it = g_candidates.erase(it);
            else
                ++it;
        }

        LogicalDevice* device = swapchain->logicalDevice;
        swapchain->hudPreserve.reset();
        swapchain->hud.hudPreserveValid = false;

        if (device)
        {
            auto releaseImage = [device](VkImage& image, VkImageView& view, VkDeviceMemory& memory) {
                if (view != VK_NULL_HANDLE)
                    device->vkd.DestroyImageView(device->device, view, nullptr);
                if (image != VK_NULL_HANDLE)
                    device->vkd.DestroyImage(device->device, image, nullptr);
                if (memory != VK_NULL_HANDLE)
                    device->vkd.FreeMemory(device->device, memory, nullptr);
                image = VK_NULL_HANDLE;
                view = VK_NULL_HANDLE;
                memory = VK_NULL_HANDLE;
            };

            for (size_t i = 0; i < swapchain->hud.sceneImages.size(); ++i)
                releaseImage(swapchain->hud.sceneImages[i], swapchain->hud.sceneViews[i], swapchain->hud.sceneMemory[i]);
            for (size_t i = 0; i < swapchain->hud.finalImages.size(); ++i)
                releaseImage(swapchain->hud.finalImages[i], swapchain->hud.finalViews[i], swapchain->hud.finalMemory[i]);

            HudPendingImages& pending = swapchain->hud.pendingFrame;
            if (pending.sceneImage != VK_NULL_HANDLE)
                device->vkd.DestroyImage(device->device, pending.sceneImage, nullptr);
            if (pending.sceneMemory != VK_NULL_HANDLE)
                device->vkd.FreeMemory(device->device, pending.sceneMemory, nullptr);
            if (pending.finalImage != VK_NULL_HANDLE)
                device->vkd.DestroyImage(device->device, pending.finalImage, nullptr);
            if (pending.finalMemory != VK_NULL_HANDLE)
                device->vkd.FreeMemory(device->device, pending.finalMemory, nullptr);
            pending = {};
        }

        swapchain->hud = {};
        swapchain->commandBuffersEffectHud.clear();
    }

    bool rebuildCommandBuffers(LogicalDevice* device, LogicalSwapchain* swapchain,
                               const DepthState& depthState, VkCommandPool pool)
    {
        if (!enabled() || !device || !swapchain || !swapchain->hud.initialized
            || !swapchain->hud.hudPreserveValid || !swapchain->hudPreserve)
            return false;

        std::vector<VkCommandBuffer> commandBuffers =
            allocateCommandBuffer(device, swapchain->imageCount, pool);
        if (commandBuffers.size() != swapchain->imageCount)
            return false;

        std::vector<std::shared_ptr<Effect>> effects = swapchain->effects;
        effects.push_back(swapchain->hudPreserve);
        writeCommandBuffers(device, swapchain, effects, commandBuffers, depthState);
        swapchain->commandBuffersEffectHud = std::move(commandBuffers);
        return true;
    }

    bool activeFor(const LogicalSwapchain* swapchain, uint32_t imageIndex)
    {
        if (!enabled() || !swapchain || !swapchain->hud.initialized
            || !swapchain->hud.hudPreserveValid || !swapchain->hudPreserve)
            return false;
        if (swapchain->commandBuffersEffectHud.empty())
            return false;

        static const bool forced = getenv("VKINTOX_HUD_FORCE") != nullptr;
        if (forced)
            return true;

        if (imageIndex >= swapchain->hud.snapshotValid.size())
            return false;
        return swapchain->hud.snapshotValid[imageIndex];
    }

    void commandBufferBegan(VkCommandBuffer commandBuffer)
    {
        if (!enabled())
            return;
        Candidate& candidate = g_candidates[commandBuffer];
        candidate.active = false;
        candidate.splitDone = false;
        candidate.finalCopyDone = false;
        candidate.seenDepthPass = false;
        candidate.drawsInPass = 0;
        candidate.owner = nullptr;
    }

    void commandBufferFreed(VkCommandBuffer commandBuffer)
    {
        if (!enabled())
            return;
        g_candidates.erase(commandBuffer);
    }

    void beginRenderPass(LogicalDevice* device, VkCommandBuffer commandBuffer,
                         const VkRenderPassBeginInfo* pRenderPassBegin)
    {
        if (!enabled() || !device || !pRenderPassBegin)
            return;

        Candidate& candidate = g_candidates[commandBuffer];
        candidate.active = false;
        candidate.splitDone = false;
        candidate.finalCopyDone = false;
        candidate.drawsInPass = 0;

        PassLog::RenderPassInfo info;
        if (!PassLog::queryRenderPass(pRenderPassBegin->renderPass, info))
            return;

        if (info.hasDepth)
            candidate.seenDepthPass = true;

        if (!candidate.seenDepthPass)
            return;
        if (info.attachmentCount != 1 || info.hasDepth || info.subpassCount != 1 || info.colorCount != 1)
            return;
        if (info.samples != VK_SAMPLE_COUNT_1_BIT)
            return;
        if (info.multiview)
            return;
        if (info.colorLoadOp != VK_ATTACHMENT_LOAD_OP_DONT_CARE)
            return;
        if (info.colorStoreOp != VK_ATTACHMENT_STORE_OP_STORE)
            return;
        if (info.colorLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
            && info.colorLayout != VK_IMAGE_LAYOUT_GENERAL)
            return;
        if (info.colorFinalLayout == VK_IMAGE_LAYOUT_UNDEFINED)
            return;
        if (pRenderPassBegin->renderArea.offset.x != 0 || pRenderPassBegin->renderArea.offset.y != 0)
            return;

        LogicalSwapchain* swapchain = findSwapchainLocked(device, info.colorFormat, pRenderPassBegin->renderArea.extent);
        if (!swapchain)
            return;

        VkImage colorImage = PassLog::colorAttachmentImage(pRenderPassBegin->framebuffer, pRenderPassBegin->renderPass);
        if (colorImage == VK_NULL_HANDLE)
            return;
        if (selectDepthSnapshotTargetFromImage(device, colorImage).swapchain != VK_NULL_HANDLE)
            return;

        candidate.active = true;
        candidate.owner = swapchain;
        candidate.originalPass = pRenderPassBegin->renderPass;
        candidate.compatiblePass = VK_NULL_HANDLE;
        candidate.framebuffer = pRenderPassBegin->framebuffer;
        candidate.area = pRenderPassBegin->renderArea;
        candidate.colorImage = colorImage;
        candidate.colorFormat = info.colorFormat;
        candidate.colorFinalLayout = info.colorFinalLayout;
        candidate.colorSubpassLayout = info.colorLayout;
        candidate.subpassFlags = info.subpassFlags;
        candidate.splitLogged = false;

        if (g_loggedArmed.insert(candidate.originalPass).second)
        {
            Logger::info("HUD protection: armed pass renderPass=" + convertToString(candidate.originalPass)
                         + " image=" + convertToString(candidate.colorImage)
                         + " area=" + std::to_string(candidate.area.extent.width) + "x"
                         + std::to_string(candidate.area.extent.height));
        }
    }

    void beginRendering(LogicalDevice* device, VkCommandBuffer commandBuffer,
                        const VkRenderingInfo* pRenderingInfo)
    {
        (void) device;
        if (!enabled())
            return;

        Candidate& candidate = g_candidates[commandBuffer];
        candidate.active = false;
        candidate.splitDone = false;
        candidate.finalCopyDone = false;
        candidate.drawsInPass = 0;
        if (pRenderingInfo && pRenderingInfo->pDepthAttachment
            && pRenderingInfo->pDepthAttachment->imageView != VK_NULL_HANDLE)
            candidate.seenDepthPass = true;
    }

    void endRenderPass(LogicalDevice* device, VkCommandBuffer commandBuffer)
    {
        if (!enabled() || !device)
            return;

        auto it = g_candidates.find(commandBuffer);
        if (it == g_candidates.end())
            return;
        Candidate& candidate = it->second;
        if (!candidate.splitDone || candidate.finalCopyDone || !candidate.owner)
            return;

        HudPendingImages& pending = candidate.owner->hud.pendingFrame;
        if (pending.finalImage == VK_NULL_HANDLE)
            return;

        imageBarrier(device, commandBuffer, candidate.colorImage,
                     candidate.colorFinalLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        imageBarrier(device, commandBuffer, pending.finalImage,
                     pending.finalUsed ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     pending.finalUsed ? VK_ACCESS_TRANSFER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        copyImageRegion(device, commandBuffer, candidate.colorImage, pending.finalImage, candidate.area.extent);
        imageBarrier(device, commandBuffer, pending.finalImage,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        pending.finalUsed = true;
        imageBarrier(device, commandBuffer, candidate.colorImage,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, candidate.colorFinalLayout,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        candidate.finalCopyDone = true;

        static bool logged = false;
        if (!logged)
        {
            logged = true;
            Logger::info("HUD protection: final snapshot captured");
        }
    }

    void draw(LogicalDevice* device, VkCommandBuffer commandBuffer, bool plainDraw,
              uint32_t vertexCount, uint32_t instanceCount)
    {
        if (!enabled() || !device)
            return;

        auto it = g_candidates.find(commandBuffer);
        if (it == g_candidates.end())
            return;
        Candidate& candidate = it->second;
        if (!candidate.active || candidate.splitDone)
            return;

        if (candidate.drawsInPass == 0)
        {
            if (!plainDraw || vertexCount != 3 || instanceCount != 1)
            {
                candidate.active = false;
                if (g_loggedSkipped.insert(candidate.originalPass).second)
                {
                    Logger::info("HUD protection: split skipped, first draw was "
                                 + std::to_string(vertexCount) + " vertices and "
                                 + std::to_string(instanceCount) + " instances");
                }
                return;
            }
            if (!performSplitLocked(device, commandBuffer, candidate))
            {
                candidate.active = false;
                return;
            }
        }
        ++candidate.drawsInPass;
    }

    void blitImage(LogicalDevice* device, VkCommandBuffer commandBuffer, VkImage srcImage, VkImage dstImage)
    {
        if (!enabled() || !device)
            return;

        DepthSnapshotTarget target = selectDepthSnapshotTargetFromImage(device, dstImage);
        if (!target.logicalSwapchain || target.imageIndex >= target.logicalSwapchain->hud.snapshotValid.size())
            return;

        LogicalSwapchain* swapchain = target.logicalSwapchain;
        const uint32_t index = target.imageIndex;
        if (index >= swapchain->hud.sceneImages.size() || index >= swapchain->hud.finalImages.size())
            return;

        HudPendingImages& pending = swapchain->hud.pendingFrame;
        if (pending.sceneImage == VK_NULL_HANDLE || pending.finalImage == VK_NULL_HANDLE)
        {
            swapchain->hud.snapshotValid[index] = false;
            return;
        }

        imageBarrier(device, commandBuffer, swapchain->hud.sceneImages[index],
                     swapchain->hud.sceneInitialized[index] ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     swapchain->hud.sceneInitialized[index] ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        imageBarrier(device, commandBuffer, swapchain->hud.finalImages[index],
                     swapchain->hud.finalInitialized[index] ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     swapchain->hud.finalInitialized[index] ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        copyImageRegion(device, commandBuffer, pending.sceneImage, swapchain->hud.sceneImages[index], swapchain->imageExtent);
        copyImageRegion(device, commandBuffer, pending.finalImage, swapchain->hud.finalImages[index], swapchain->imageExtent);
        imageBarrier(device, commandBuffer, swapchain->hud.sceneImages[index],
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        imageBarrier(device, commandBuffer, swapchain->hud.finalImages[index],
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        swapchain->hud.sceneInitialized[index] = true;
        swapchain->hud.finalInitialized[index] = true;
        swapchain->hud.snapshotValid[index] = true;

        static bool logged = false;
        if (!logged)
        {
            logged = true;
            Logger::info("HUD protection: snapshot ready for slot " + std::to_string(index));
        }
    }

    void copyImage(LogicalDevice* device, VkCommandBuffer commandBuffer, VkImage srcImage, VkImage dstImage)
    {
        blitImage(device, commandBuffer, srcImage, dstImage);
    }

    void renderPassDestroyed(LogicalDevice* device, VkRenderPass renderPass)
    {
        auto it = g_compatiblePasses.find(renderPass);
        if (it == g_compatiblePasses.end())
            return;
        VkRenderPass compatible = it->second;
        g_compatiblePasses.erase(it);
        g_compatibleOwners.erase(renderPass);
        if (device && compatible != VK_NULL_HANDLE)
        {
            DeferredDestroyQueue::instance().push(
                DestroyPhase::Resource,
                [vkd = device->vkd, deviceHandle = device->device, compatible]() {
                    if (compatible != VK_NULL_HANDLE)
                        vkd.DestroyRenderPass(deviceHandle, compatible, nullptr);
                });
        }
    }
}
