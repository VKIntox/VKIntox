#include "layer/pass_log.hh"
#include "layer/internal.hh"

#include "render/resources/format.hh"
#include "util/logger.hh"
#include "util/util.hh"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace VKIntox::PassLog
{
    namespace
    {
        struct ActivePass
        {
            bool                    dynamic = false;
            VkRect2D                area = {};
            DepthSnapshotTarget     target = {};
            bool                    hasDepth = false;
            VkFormat                depthFormat = VK_FORMAT_UNDEFINED;
            VkFormat                colorFormat = VK_FORMAT_UNDEFINED;
            VkAttachmentLoadOp      colorLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            VkImageLayout           colorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            uint32_t                colorCount = 0;
            VkSampleCountFlagBits   samples = VK_SAMPLE_COUNT_1_BIT;
            VkImage                 colorImage = VK_NULL_HANDLE;
            uint32_t                drawCount = 0;
            uint32_t                firstDrawSize = 0;
            bool                    firstDrawIndexed = false;
            bool                    firstDrawSeen = false;
            uint32_t                index = 0;
            uint64_t                sequence = 0;
            uint64_t                frame = 0;
        };

        std::unordered_map<VkRenderPass, RenderPassInfo> g_renderPasses;
        std::unordered_map<VkCommandBuffer, ActivePass>  g_activePasses;
        std::unordered_map<VkCommandBuffer, uint32_t>    g_passIndices;
        std::unordered_map<VkImageView, VkImage>         g_viewImages;
        std::unordered_map<VkFramebuffer, std::vector<VkImageView>> g_framebufferViews;
        uint64_t                                         g_sequence = 0;
        uint64_t                                         g_presentCount = 0;

        const char* formatName(VkFormat format)
        {
            switch (format)
            {
                case VK_FORMAT_UNDEFINED: return "undefined";
                case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
                case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
                case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
                case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
                case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
                case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
                case VK_FORMAT_D16_UNORM: return "D16_UNORM";
                case VK_FORMAT_D32_SFLOAT: return "D32_SFLOAT";
                case VK_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
                case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32S8";
                default: return nullptr;
            }
        }

        std::string formatLabel(VkFormat format)
        {
            const char* name = formatName(format);
            return name ? std::string(name) : ("fmt#" + std::to_string(static_cast<uint32_t>(format)));
        }

        const char* loadOpName(VkAttachmentLoadOp loadOp)
        {
            switch (loadOp)
            {
                case VK_ATTACHMENT_LOAD_OP_LOAD: return "LOAD";
                case VK_ATTACHMENT_LOAD_OP_CLEAR: return "CLEAR";
                case VK_ATTACHMENT_LOAD_OP_DONT_CARE: return "DONT_CARE";
                default: return "?";
            }
        }

        const char* layoutName(VkImageLayout layout)
        {
            switch (layout)
            {
                case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEFINED";
                case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
                case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR_ATTACHMENT_OPTIMAL";
                case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "PRESENT_SRC";
                case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "TRANSFER_SRC";
                case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "TRANSFER_DST";
                default: return "?";
            }
        }

        uint32_t sampleCount(VkSampleCountFlagBits samples)
        {
            switch (samples)
            {
                case VK_SAMPLE_COUNT_1_BIT: return 1;
                case VK_SAMPLE_COUNT_2_BIT: return 2;
                case VK_SAMPLE_COUNT_4_BIT: return 4;
                case VK_SAMPLE_COUNT_8_BIT: return 8;
                case VK_SAMPLE_COUNT_16_BIT: return 16;
                case VK_SAMPLE_COUNT_32_BIT: return 32;
                case VK_SAMPLE_COUNT_64_BIT: return 64;
                default: return static_cast<uint32_t>(samples);
            }
        }

        std::string targetLabel(const DepthSnapshotTarget& target)
        {
            if (target.swapchain == VK_NULL_HANDLE)
                return "offscreen";
            return "swapchain[" + std::to_string(target.imageIndex) + "]";
        }

        VkImage colorImageFor(VkFramebuffer framebuffer, uint32_t colorIndex)
        {
            auto framebufferIt = g_framebufferViews.find(framebuffer);
            if (framebufferIt == g_framebufferViews.end() || colorIndex >= framebufferIt->second.size())
                return VK_NULL_HANDLE;

            auto viewIt = g_viewImages.find(framebufferIt->second[colorIndex]);
            return viewIt == g_viewImages.end() ? VK_NULL_HANDLE : viewIt->second;
        }
    }

    bool enabled()
    {
        static const bool value = [] {
            const char* env = getenv("VKINTOX_PASS_LOG");
            if (!env)
                return false;
            std::string text(env);
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text == "1" || text == "true" || text == "yes" || text == "on";
        }();
        return value;
    }

    void renderPassCreated(VkRenderPass renderPass, const VkRenderPassCreateInfo* pCreateInfo)
    {
        if (!pCreateInfo)
            return;

        RenderPassInfo info;
        info.attachmentCount = pCreateInfo->attachmentCount;
        info.subpassCount = pCreateInfo->subpassCount;
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
        {
            const VkAttachmentDescription& attachment = pCreateInfo->pAttachments[i];
            if (isDepthFormat(attachment.format) || isStencilFormat(attachment.format))
            {
                info.hasDepth = true;
                info.depthFormat = attachment.format;
                continue;
            }
            if (info.colorCount == 0)
            {
                info.colorFormat = attachment.format;
                info.colorLoadOp = attachment.loadOp;
                info.colorStoreOp = attachment.storeOp;
                info.colorInitialLayout = attachment.initialLayout;
                info.colorFinalLayout = attachment.finalLayout;
                info.samples = attachment.samples;
                info.colorIndex = i;
                if (pCreateInfo->subpassCount > 0 && pCreateInfo->pSubpasses[0].colorAttachmentCount > 0
                    && pCreateInfo->pSubpasses[0].pColorAttachments)
                {
                    info.colorLayout = pCreateInfo->pSubpasses[0].pColorAttachments[0].layout;
                    info.subpassFlags = pCreateInfo->pSubpasses[0].flags;
                }
            }
            ++info.colorCount;
        }
        g_renderPasses[renderPass] = info;
    }

    void renderPassCreated(VkRenderPass renderPass, const VkRenderPassCreateInfo2* pCreateInfo)
    {
        if (!pCreateInfo)
            return;

        RenderPassInfo info;
        info.attachmentCount = pCreateInfo->attachmentCount;
        info.subpassCount = pCreateInfo->subpassCount;
        if (pCreateInfo->correlatedViewMaskCount > 0)
            info.multiview = true;
        for (uint32_t s = 0; s < pCreateInfo->subpassCount; ++s)
        {
            if (pCreateInfo->pSubpasses && pCreateInfo->pSubpasses[s].viewMask != 0)
                info.multiview = true;
        }
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
        {
            const VkAttachmentDescription2& attachment = pCreateInfo->pAttachments[i];
            if (isDepthFormat(attachment.format) || isStencilFormat(attachment.format))
            {
                info.hasDepth = true;
                info.depthFormat = attachment.format;
                continue;
            }
            if (info.colorCount == 0)
            {
                info.colorFormat = attachment.format;
                info.colorLoadOp = attachment.loadOp;
                info.colorStoreOp = attachment.storeOp;
                info.colorInitialLayout = attachment.initialLayout;
                info.colorFinalLayout = attachment.finalLayout;
                info.samples = attachment.samples;
                info.colorIndex = i;
                if (pCreateInfo->subpassCount > 0 && pCreateInfo->pSubpasses[0].colorAttachmentCount > 0
                    && pCreateInfo->pSubpasses[0].pColorAttachments)
                {
                    info.colorLayout = pCreateInfo->pSubpasses[0].pColorAttachments[0].layout;
                    info.subpassFlags = pCreateInfo->pSubpasses[0].flags;
                }
            }
            ++info.colorCount;
        }
        g_renderPasses[renderPass] = info;
    }

    void renderPassDestroyed(VkRenderPass renderPass)
    {
        g_renderPasses.erase(renderPass);
    }

    bool queryRenderPass(VkRenderPass renderPass, RenderPassInfo& info)
    {
        auto it = g_renderPasses.find(renderPass);
        if (it == g_renderPasses.end())
            return false;
        info = it->second;
        return true;
    }

    VkImage colorAttachmentImage(VkFramebuffer framebuffer, VkRenderPass renderPass)
    {
        auto it = g_renderPasses.find(renderPass);
        if (it == g_renderPasses.end())
            return VK_NULL_HANDLE;
        return colorImageFor(framebuffer, it->second.colorIndex);
    }

    void imageViewCreated(VkImageView imageView, VkImage image)
    {
        if (imageView == VK_NULL_HANDLE)
            return;
        g_viewImages[imageView] = image;
    }

    void imageViewDestroyed(VkImageView imageView)
    {
        g_viewImages.erase(imageView);
    }

    void framebufferCreated(VkFramebuffer framebuffer, const VkFramebufferCreateInfo* pCreateInfo)
    {
        if (!pCreateInfo || framebuffer == VK_NULL_HANDLE)
            return;
        if (pCreateInfo->pAttachments && pCreateInfo->attachmentCount > 0)
            g_framebufferViews[framebuffer] = std::vector<VkImageView>(
                pCreateInfo->pAttachments, pCreateInfo->pAttachments + pCreateInfo->attachmentCount);
        else
            g_framebufferViews[framebuffer] = {};
    }

    void framebufferDestroyed(VkFramebuffer framebuffer)
    {
        g_framebufferViews.erase(framebuffer);
    }

    void commandBufferBegan(VkCommandBuffer commandBuffer)
    {
        if (!enabled())
            return;
        g_activePasses.erase(commandBuffer);
        g_passIndices.erase(commandBuffer);
    }

    void commandBufferFreed(VkCommandBuffer commandBuffer)
    {
        if (!enabled())
            return;
        g_activePasses.erase(commandBuffer);
        g_passIndices.erase(commandBuffer);
    }

    void begin(VkCommandBuffer commandBuffer, VkRenderPass renderPass, VkFramebuffer framebuffer,
               const VkRect2D& renderArea, const DepthSnapshotTarget& target)
    {
        if (!enabled())
            return;

        ActivePass pass;
        auto infoIt = g_renderPasses.find(renderPass);
        if (infoIt != g_renderPasses.end())
        {
            pass.hasDepth = infoIt->second.hasDepth;
            pass.depthFormat = infoIt->second.depthFormat;
            pass.colorFormat = infoIt->second.colorFormat;
            pass.colorLoadOp = infoIt->second.colorLoadOp;
            pass.colorLayout = infoIt->second.colorLayout;
            pass.colorCount = infoIt->second.colorCount;
            pass.samples = infoIt->second.samples;
            pass.colorImage = colorImageFor(framebuffer, infoIt->second.colorIndex);
        }
        pass.area = renderArea;
        pass.target = target;
        pass.index = g_passIndices[commandBuffer]++;
        pass.sequence = g_sequence++;
        pass.frame = g_presentCount;
        g_activePasses[commandBuffer] = pass;
    }

    void beginDynamic(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo,
                      const DepthSnapshotTarget& target)
    {
        if (!enabled() || !pRenderingInfo)
            return;

        ActivePass pass;
        pass.dynamic = true;
        pass.area = pRenderingInfo->renderArea;
        pass.target = target;
        pass.hasDepth = pRenderingInfo->pDepthAttachment
            && pRenderingInfo->pDepthAttachment->imageView != VK_NULL_HANDLE;
        pass.colorCount = pRenderingInfo->colorAttachmentCount;
        if (pRenderingInfo->pColorAttachments && pRenderingInfo->colorAttachmentCount > 0)
        {
            pass.colorLoadOp = pRenderingInfo->pColorAttachments[0].loadOp;
            pass.colorLayout = pRenderingInfo->pColorAttachments[0].imageLayout;
            auto viewIt = g_viewImages.find(pRenderingInfo->pColorAttachments[0].imageView);
            if (viewIt != g_viewImages.end())
                pass.colorImage = viewIt->second;
        }
        pass.index = g_passIndices[commandBuffer]++;
        pass.sequence = g_sequence++;
        pass.frame = g_presentCount;
        g_activePasses[commandBuffer] = pass;
    }

    void countDraw(VkCommandBuffer commandBuffer, uint32_t drawCount, uint32_t vertices, bool indexed)
    {
        if (!enabled())
            return;

        auto passIt = g_activePasses.find(commandBuffer);
        if (passIt == g_activePasses.end())
            return;

        if (!passIt->second.firstDrawSeen && vertices > 0)
        {
            passIt->second.firstDrawSeen = true;
            passIt->second.firstDrawSize = vertices;
            passIt->second.firstDrawIndexed = indexed;
        }
        passIt->second.drawCount += drawCount;
    }

    void end(VkCommandBuffer commandBuffer)
    {
        if (!enabled())
            return;

        auto passIt = g_activePasses.find(commandBuffer);
        if (passIt == g_activePasses.end())
            return;

        const ActivePass& pass = passIt->second;
        Logger::info("[passlog] frame=" + std::to_string(pass.frame)
                     + " seq=" + std::to_string(pass.sequence)
                     + " pass=" + std::to_string(pass.index)
                     + " cb=" + convertToString(commandBuffer)
                     + " kind=" + (pass.dynamic ? "dynamic" : "renderpass")
                     + " target=" + targetLabel(pass.target)
                     + " image=" + (pass.colorImage != VK_NULL_HANDLE ? convertToString(pass.colorImage) : std::string("?"))
                     + " depth=" + (pass.hasDepth
                            ? (pass.depthFormat == VK_FORMAT_UNDEFINED ? std::string("yes") : formatLabel(pass.depthFormat))
                            : std::string("none"))
                     + " color=" + std::to_string(pass.colorCount) + "x" + formatLabel(pass.colorFormat)
                     + " load=" + loadOpName(pass.colorLoadOp)
                     + " layout=" + layoutName(pass.colorLayout)
                     + " draws=" + std::to_string(pass.drawCount)
                     + " first=" + (pass.firstDrawSeen
                            ? std::to_string(pass.firstDrawSize) + (pass.firstDrawIndexed ? "i" : "v")
                            : std::string("-"))
                     + " area=" + std::to_string(pass.area.extent.width) + "x" + std::to_string(pass.area.extent.height)
                     + " samples=" + std::to_string(sampleCount(pass.samples)));
        g_activePasses.erase(passIt);
    }

    void present(const VkPresentInfoKHR* pPresentInfo)
    {
        if (!enabled() || !pPresentInfo)
            return;

        std::string images;
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
        {
            if (i > 0)
                images += ",";
            images += std::to_string(pPresentInfo->pImageIndices[i]);

            auto swapchainIt = swapchainMap.find(pPresentInfo->pSwapchains[i]);
            if (swapchainIt == swapchainMap.end() || !swapchainIt->second)
                continue;

            const LogicalSwapchain& swapchain = *swapchainIt->second;
            const uint32_t imageIndex = pPresentInfo->pImageIndices[i];
            if (imageIndex < swapchain.fakeImages.size())
                images += "(fake=" + convertToString(swapchain.fakeImages[imageIndex]) + ")";
            if (imageIndex < swapchain.images.size())
                images += "(real=" + convertToString(swapchain.images[imageIndex]) + ")";
        }

        Logger::info("[passlog] frame=" + std::to_string(g_presentCount)
                     + " present swapchains=" + std::to_string(pPresentInfo->swapchainCount)
                     + " image=" + images);
        ++g_presentCount;
    }

    void copyImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImage dstImage,
                   const VkImageCopy* pRegions, uint32_t regionCount)
    {
        if (!enabled() || !pRegions || regionCount == 0)
            return;

        Logger::info("[passlog] copy cb=" + convertToString(commandBuffer)
                     + " src=" + convertToString(srcImage)
                     + " dst=" + convertToString(dstImage)
                     + " extent=" + std::to_string(pRegions[0].extent.width) + "x" + std::to_string(pRegions[0].extent.height));
    }

    void blitImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImage dstImage,
                   const VkImageBlit* pRegions, uint32_t regionCount, VkFilter filter)
    {
        if (!enabled() || !pRegions || regionCount == 0)
            return;

        const uint32_t dstWidth = static_cast<uint32_t>(
            std::abs(pRegions[0].dstOffsets[1].x - pRegions[0].dstOffsets[0].x));
        const uint32_t dstHeight = static_cast<uint32_t>(
            std::abs(pRegions[0].dstOffsets[1].y - pRegions[0].dstOffsets[0].y));

        Logger::info("[passlog] blit cb=" + convertToString(commandBuffer)
                     + " src=" + convertToString(srcImage)
                     + " dst=" + convertToString(dstImage)
                     + " extent=" + std::to_string(dstWidth) + "x" + std::to_string(dstHeight)
                     + " filter=" + (filter == VK_FILTER_LINEAR ? "LINEAR" : "NEAREST"));
    }
}
