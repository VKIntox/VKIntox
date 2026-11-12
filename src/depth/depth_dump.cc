#include "depth/depth_dump.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#include "config/settings_manager.hh"
#include "layer/internal.hh"
#include "render/device/logical_device.hh"
#include "render/resources/buffer.hh"
#include "render/resources/image.hh"
#include "util/async_writer.hh"
#include "util/logger.hh"
#include "util/util.hh"

namespace VKIntox
{
    namespace
    {
        // armed from the overlay's thread, consumed by whichever device blits
        // depth next. one dump at a time.
        std::mutex s_mutex;
        std::atomic<bool> s_active{false};
        std::string s_path;
        bool s_requested = false;
        bool s_inFlight = false;
        bool s_resultReady = false;
        bool s_resultOk = false;
        bool s_skipLogged = false;
        int s_waitPresents = 0;
        std::chrono::steady_clock::time_point s_suppressUntil{};
        std::string s_resultMessage;
        std::chrono::steady_clock::time_point s_deadline{};

        constexpr auto kRequestWindow = std::chrono::seconds(10);

        uint32_t depthBytesPerTexel(VkFormat format)
        {
            switch (format)
            {
            case VK_FORMAT_D16_UNORM:
                return 2;
            case VK_FORMAT_D32_SFLOAT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_R32_SFLOAT:
                return 4;
            default:
                return 0;
            }
        }

        void setResultLocked(bool ok, std::string message)
        {
            s_resultReady = true;
            s_resultOk = ok;
            s_resultMessage = std::move(message);
        }

        uint32_t pngCrc(const uint8_t* data, size_t length)
        {
            static uint32_t table[256];
            static bool initialized = false;
            if (!initialized)
            {
                for (uint32_t i = 0; i < 256; i++)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; k++)
                        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                    table[i] = c;
                }
                initialized = true;
            }
            uint32_t crc = 0xFFFFFFFFu;
            for (size_t i = 0; i < length; i++)
                crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return crc ^ 0xFFFFFFFFu;
        }

        uint32_t pngAdler(const uint8_t* data, size_t length)
        {
            uint32_t a = 1, b = 0;
            for (size_t i = 0; i < length; i++)
            {
                a = (a + data[i]) % 65521u;
                b = (b + a) % 65521u;
            }
            return (b << 16) | a;
        }

        void appendBE32(std::string& out, uint32_t v)
        {
            out.push_back(char((v >> 24) & 0xFF));
            out.push_back(char((v >> 16) & 0xFF));
            out.push_back(char((v >> 8) & 0xFF));
            out.push_back(char(v & 0xFF));
        }

        void appendChunk(std::string& out, const char* type, const std::string& payload)
        {
            appendBE32(out, static_cast<uint32_t>(payload.size()));
            const size_t start = out.size();
            out.append(type, 4);
            out += payload;
            appendBE32(out, pngCrc(reinterpret_cast<const uint8_t*>(out.data() + start), out.size() - start));
        }

        // grayscale 16-bit, deflate in stored blocks so no zlib is needed to
        // write it. bigger file, still a plain PNG to every reader.
        std::string encodeDepthPng(const std::vector<uint16_t>& depth, uint32_t width, uint32_t height,
                                   const std::string& metaText)
        {
            std::string raw;
            raw.reserve(size_t(height) * (1 + size_t(width) * 2));
            for (uint32_t y = 0; y < height; y++)
            {
                raw.push_back(char(0));
                for (uint32_t x = 0; x < width; x++)
                {
                    const uint16_t v = depth[size_t(y) * width + x];
                    raw.push_back(char((v >> 8) & 0xFF));
                    raw.push_back(char(v & 0xFF));
                }
            }

            std::string zlib;
            zlib.push_back(char(0x78));
            zlib.push_back(char(0x01));
            size_t offset = 0;
            do
            {
                const size_t block = std::min<size_t>(raw.size() - offset, 0xFFFFu);
                zlib.push_back(offset + block >= raw.size() ? char(1) : char(0));
                zlib.push_back(char(block & 0xFF));
                zlib.push_back(char((block >> 8) & 0xFF));
                zlib.push_back(char(~block & 0xFF));
                zlib.push_back(char((~block >> 8) & 0xFF));
                zlib.append(raw, offset, block);
                offset += block;
            } while (offset < raw.size());
            appendBE32(zlib, pngAdler(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()));

            std::string out;
            out.append("\x89PNG\r\n\x1a\n", 8);

            std::string ihdr;
            appendBE32(ihdr, width);
            appendBE32(ihdr, height);
            ihdr.push_back(char(16));  // bit depth
            ihdr.push_back(char(0));   // grayscale
            ihdr.push_back(char(0));
            ihdr.push_back(char(0));
            ihdr.push_back(char(0));
            appendChunk(out, "IHDR", ihdr);

            if (!metaText.empty())
            {
                std::string text = "vkintox";
                text.push_back(char(0));
                text += metaText;
                appendChunk(out, "tEXt", text);
            }

            appendChunk(out, "IDAT", zlib);
            appendChunk(out, "IEND", std::string());
            return out;
        }

        // one 8-bit RGBA PNG holding both halves: scene left, depth bytes right
        std::string encodeFramePng(const std::vector<uint8_t>& left, const std::vector<uint8_t>& right,
                                   uint32_t width, uint32_t height, const std::string& metaText)
        {
            const uint32_t outW = width * 2;
            std::string raw;
            raw.reserve(size_t(height) * (1 + size_t(outW) * 4));
            for (uint32_t y = 0; y < height; y++)
            {
                raw.push_back(char(0));
                raw.append(reinterpret_cast<const char*>(left.data() + size_t(y) * width * 4), size_t(width) * 4);
                raw.append(reinterpret_cast<const char*>(right.data() + size_t(y) * width * 4), size_t(width) * 4);
            }

            std::string zlib;
            zlib.push_back(char(0x78));
            zlib.push_back(char(0x01));
            size_t offset = 0;
            do
            {
                const size_t block = std::min<size_t>(raw.size() - offset, 0xFFFFu);
                zlib.push_back(offset + block >= raw.size() ? char(1) : char(0));
                zlib.push_back(char(block & 0xFF));
                zlib.push_back(char((block >> 8) & 0xFF));
                zlib.push_back(char(~block & 0xFF));
                zlib.push_back(char((~block >> 8) & 0xFF));
                zlib.append(raw, offset, block);
                offset += block;
            } while (offset < raw.size());
            appendBE32(zlib, pngAdler(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()));

            std::string out;
            out.append("\x89PNG\r\n\x1a\n", 8);

            std::string ihdr;
            appendBE32(ihdr, outW);
            appendBE32(ihdr, height);
            ihdr.push_back(char(8));
            ihdr.push_back(char(6));  // rgba
            ihdr.push_back(char(0));
            ihdr.push_back(char(0));
            ihdr.push_back(char(0));
            appendChunk(out, "IHDR", ihdr);

            std::string text = "vkintox";
            text.push_back(char(0));
            text += metaText;
            appendChunk(out, "tEXt", text);

            appendChunk(out, "IDAT", zlib);
            appendChunk(out, "IEND", std::string());
            return out;
        }

        // depth as ReShade sees it: a 0..1 float channel. D24 keeps its range
        // scaled down, D16 maps one-to-one, float clamped.
        void convertTexels(const uint8_t* src, size_t rowPitch, uint32_t width, uint32_t height,
                           VkFormat format, std::vector<uint16_t>& out)
        {
            out.assign(size_t(width) * height, 0);

            for (uint32_t y = 0; y < height; y++)
            {
                const uint8_t* row = src + size_t(y) * rowPitch;
                uint16_t* dst = out.data() + size_t(y) * width;

                switch (format)
                {
                case VK_FORMAT_D16_UNORM:
                    for (uint32_t x = 0; x < width; x++)
                        dst[x] = uint16_t(row[2 * x] | (row[2 * x + 1] << 8));
                    break;

                case VK_FORMAT_D24_UNORM_S8_UINT:
                case VK_FORMAT_X8_D24_UNORM_PACK32:
                    for (uint32_t x = 0; x < width; x++)
                    {
                        const uint32_t word = uint32_t(row[4 * x]) | (uint32_t(row[4 * x + 1]) << 8)
                            | (uint32_t(row[4 * x + 2]) << 16) | (uint32_t(row[4 * x + 3]) << 24);
                        dst[x] = uint16_t(((word & 0xFFFFFFu) * 65535u + 0x800000u) / 0xFFFFFFu);
                    }
                    break;

                case VK_FORMAT_D32_SFLOAT:
                case VK_FORMAT_D32_SFLOAT_S8_UINT:
                case VK_FORMAT_R32_SFLOAT:
                    for (uint32_t x = 0; x < width; x++)
                    {
                        float v;
                        std::memcpy(&v, row + 4 * x, sizeof(v));
                        dst[x] = uint16_t(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
                    }
                    break;

                default:
                    break;
                }
            }
        }
    } // anonymous namespace

    namespace
    {
        bool isDepthFormatForDump(VkFormat format)
        {
            switch (format)
            {
            case VK_FORMAT_D16_UNORM:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_D32_SFLOAT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return true;
            default:
                return false;
            }
        }

        VkImageAspectFlags aspectFor(VkFormat format)
        {
            return isDepthFormatForDump(format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        }

        bool isReadOnlyLayout(VkImageLayout layout)
        {
            return layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                || layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                || layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        }

        VkImageLayout settledLayout(VkFormat format)
        {
            return isDepthFormatForDump(format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        // who owns the image in this layout, for the barriers around the copy
        VkPipelineStageFlags stagesIn(VkImageLayout layout, VkFormat format)
        {
            if (layout == VK_IMAGE_LAYOUT_GENERAL)
                return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            if (isReadOnlyLayout(layout))
                return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            return isDepthFormatForDump(format)
                ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
                : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        }

        VkAccessFlags accessIn(VkImageLayout layout, VkFormat format)
        {
            if (layout == VK_IMAGE_LAYOUT_GENERAL)
                return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            if (isReadOnlyLayout(layout))
                return VK_ACCESS_SHADER_READ_BIT;
            return isDepthFormatForDump(format)
                ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
                : (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        }
    } // anonymous namespace

    namespace DepthDump
    {
        void request(const std::string& path)
        {
            std::lock_guard l(s_mutex);
            s_path = path;
            s_requested = true;
            s_inFlight = false;
            s_skipLogged = false;
            s_waitPresents = 0;
            s_active.store(true, std::memory_order_release);
            s_suppressUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(2330);
            s_deadline = std::chrono::steady_clock::now() + kRequestWindow;
        }

        bool wanted()
        {
            return s_active.load(std::memory_order_acquire);
        }

        bool overlaySuppressed()
        {
            std::lock_guard l(s_mutex);
            // hidden for the whole request, then 2330 ms on top: a slow frame
            // cannot land the capture after the hold has already expired
            return s_requested || s_inFlight || std::chrono::steady_clock::now() < s_suppressUntil;
        }

        void noteSkip(const std::string& reason)
        {
            std::lock_guard l(s_mutex);
            if (!s_requested || s_skipLogged)
                return;
            s_skipLogged = true;
            Logger::info("depth dump: skipping this frame, " + reason);
        }

        bool readBackAtPresent(LogicalDevice* logicalDevice, VkQueue queue, const DepthState& source,
                               bool transformed, VkImage backbuffer, VkFormat backbufferFormat,
                               const VkExtent2D& backbufferExtent)
        {
            if (!logicalDevice || queue == VK_NULL_HANDLE || !wanted())
                return false;
            if (!hasDepthState(source) || depthBytesPerTexel(source.format) == 0
                || source.extent.width == 0 || source.extent.height == 0)
            {
                noteSkip("this frame has no dumpable depth");
                return false;
            }

            {
                std::lock_guard l(s_mutex);
                if (!s_requested || s_inFlight)
                    return false;
                // the real swapchain image the present path hands us still
                // carries the composite from an earlier present, so wait a few
                // presents with the UI suppressed before copying it
                if (s_waitPresents < 3)
                {
                    s_waitPresents++;
                    return false;
                }
                s_inFlight = true;
            }

            const VkFormat format = source.format;
            const VkExtent3D extent = source.extent;
            const uint32_t bytesPerTexel = depthBytesPerTexel(format);
            const VkDeviceSize needed = VkDeviceSize(extent.width) * extent.height * bytesPerTexel + 256;

            auto fail = [&](const std::string& message) {
                std::lock_guard l(s_mutex);
                s_requested = false;
                s_inFlight = false;
                s_active.store(false, std::memory_order_release);
                setResultLocked(false, message);
                return false;
            };

            auto& staging = logicalDevice->depthDumpStaging;
            if (staging.buffer == VK_NULL_HANDLE || staging.size < needed)
            {
                destroyStaging(logicalDevice);
                VkBuffer buffer = VK_NULL_HANDLE;
                VkDeviceMemory memory = VK_NULL_HANDLE;
                createBuffer(logicalDevice, needed, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             buffer, memory);
                if (buffer == VK_NULL_HANDLE || memory == VK_NULL_HANDLE)
                    return fail("Depth dump failed: no host-visible buffer for the readback.");
                staging.buffer = buffer;
                staging.memory = memory;
                staging.size = needed;
            }
            staging.format = format;
            staging.extent = extent;
            staging.bytesPerTexel = bytesPerTexel;

            // one-shot pool, so this never records into a pool another thread
            // is filling with overlay or effect commands
            VkBuffer colorBuffer = VK_NULL_HANDLE;
            VkDeviceMemory colorMemory = VK_NULL_HANDLE;
            if (backbuffer != VK_NULL_HANDLE && backbufferExtent.width != 0 && backbufferExtent.height != 0)
            {
                createBuffer(logicalDevice, VkDeviceSize(backbufferExtent.width) * backbufferExtent.height * 4 + 256,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             colorBuffer, colorMemory);
            }

            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandBuffer cb = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;

            VkCommandPoolCreateInfo poolCI = {};
            poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolCI.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolCI.queueFamilyIndex = logicalDevice->queueFamilyIndex;
            if (logicalDevice->vkd.CreateCommandPool(logicalDevice->device, &poolCI, nullptr, &pool) != VK_SUCCESS)
                return fail("Depth dump failed: no command pool.");

            VkCommandBufferAllocateInfo cbai = {};
            cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbai.commandPool = pool;
            cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = 1;
            if (logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &cbai, &cb) != VK_SUCCESS)
            {
                logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, pool, nullptr);
                return fail("Depth dump failed: no command buffer.");
            }
            initializeDispatchTable(cb, logicalDevice->device);

            VkFenceCreateInfo fci = {};
            fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (logicalDevice->vkd.CreateFence(logicalDevice->device, &fci, nullptr, &fence) != VK_SUCCESS)
            {
                logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, pool, nullptr);
                return fail("Depth dump failed: no fence.");
            }

            auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to,
                               VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                               VkAccessFlags dstAccess, VkPipelineStageFlags dstStage)
            {
                VkImageMemoryBarrier b = {};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.image = image;
                b.oldLayout = from;
                b.newLayout = to;
                b.srcAccessMask = srcAccess;
                b.dstAccessMask = dstAccess;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.subresourceRange = {aspectFor(format), 0, 1, 0, 1};
                logicalDevice->vkd.CmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
            };

            VkCommandBufferBeginInfo bi = {};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            logicalDevice->vkd.BeginCommandBuffer(cb, &bi);

            VkImageLayout sourceLayout = source.observedLayout;
            if (sourceLayout == VK_IMAGE_LAYOUT_UNDEFINED)
                sourceLayout = isDepthFormatForDump(format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                            : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            // a multisampled depth has to be resolved before it can be copied
            VkImage readImage = source.image;
            VkImage resolveImage = VK_NULL_HANDLE;
            VkDeviceMemory resolveMemory = VK_NULL_HANDLE;
            bool resolved = false;
            if (source.samples != VK_SAMPLE_COUNT_1_BIT)
            {
                std::vector<VkImage> images = createImages(
                    logicalDevice, 1, extent, format,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, resolveMemory);
                resolveImage = images.empty() ? VK_NULL_HANDLE : images[0];
                if (resolveImage == VK_NULL_HANDLE)
                {
                    logicalDevice->vkd.EndCommandBuffer(cb);
                    logicalDevice->vkd.DestroyFence(logicalDevice->device, fence, nullptr);
                    logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, pool, nullptr);
                    return fail("Depth dump failed: no image for the msaa resolve.");
                }
                const VkImage storageImage = resolveImage;

                barrier(source.image, sourceLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        stagesIn(sourceLayout, format), accessIn(sourceLayout, format),
                        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                barrier(storageImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

                VkImageResolve region = {};
                region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
                region.extent = {extent.width, extent.height, 1};
                logicalDevice->vkd.CmdResolveImage(cb, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                   storageImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                barrier(storageImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                barrier(source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sourceLayout,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        accessIn(sourceLayout, format), stagesIn(sourceLayout, format));
                readImage = storageImage;
                resolved = true;
            }
            else
            {
                barrier(source.image, sourceLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        stagesIn(sourceLayout, format), accessIn(sourceLayout, format),
                        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            }

            VkBufferImageCopy region = {};
            region.imageSubresource = {aspectFor(format), 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            logicalDevice->vkd.CmdCopyImageToBuffer(cb, readImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                    staging.buffer, 1, &region);

            // put the image back where the layer expects to find it next frame
            const VkImageLayout restoreLayout = resolved ? settledLayout(format) : sourceLayout;
            barrier(readImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, restoreLayout,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    accessIn(restoreLayout, format), stagesIn(restoreLayout, format));

            // the presented image still holds this frame, so it is copied in
            // the same command buffer as the depth
            if (colorBuffer != VK_NULL_HANDLE)
            {
                VkImageMemoryBarrier toSource = {};
                toSource.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toSource.image = backbuffer;
                toSource.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                toSource.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                toSource.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toSource.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                logicalDevice->vkd.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                                      1, &toSource);

                VkBufferImageCopy colorRegion = {};
                colorRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                colorRegion.imageExtent = {backbufferExtent.width, backbufferExtent.height, 1};
                logicalDevice->vkd.CmdCopyImageToBuffer(cb, backbuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                        colorBuffer, 1, &colorRegion);

                VkImageMemoryBarrier toPresent = toSource;
                toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                toPresent.dstAccessMask = 0;
                logicalDevice->vkd.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                                                      1, &toPresent);
            }

            logicalDevice->vkd.EndCommandBuffer(cb);

            VkSubmitInfo si = {};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;

            constexpr uint64_t kReadbackTimeoutNs = 2ull * 1000ull * 1000ull * 1000ull;
            const VkResult submitted = logicalDevice->vkd.QueueSubmit(queue, 1, &si, fence);
            const VkResult waited = submitted == VK_SUCCESS
                ? logicalDevice->vkd.WaitForFences(logicalDevice->device, 1, &fence, VK_TRUE, kReadbackTimeoutNs)
                : submitted;

            if (waited != VK_SUCCESS)
            {
                // the copy may still be running: leave the pool and fence to the
                // device teardown rather than freeing what the GPU can touch
                Logger::warn("depth dump: readback submit/fence failed (" + std::to_string(waited) + ")");
                return fail("Depth dump failed: the readback never completed.");
            }

            if (resolveImage != VK_NULL_HANDLE)
            {
                logicalDevice->vkd.DestroyImage(logicalDevice->device, resolveImage, nullptr);
                logicalDevice->vkd.FreeMemory(logicalDevice->device, resolveMemory, nullptr);
            }

            const bool floatFormat = format == VK_FORMAT_D32_SFLOAT
                || format == VK_FORMAT_D32_SFLOAT_S8_UINT
                || format == VK_FORMAT_R32_SFLOAT;

            std::vector<uint8_t> right(size_t(extent.width) * extent.height * 4, 0);
            void* mapped = nullptr;
            if (logicalDevice->vkd.MapMemory(logicalDevice->device, staging.memory, 0, staging.size, 0, &mapped) != VK_SUCCESS
                || !mapped)
            {
                logicalDevice->vkd.DestroyFence(logicalDevice->device, fence, nullptr);
                logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, pool, nullptr);
                return fail("Depth dump failed: the readback buffer would not map.");
            }
            {
                const uint8_t* src = static_cast<const uint8_t*>(mapped);
                const size_t rowPitch = size_t(extent.width) * bytesPerTexel;
                for (uint32_t y = 0; y < extent.height; y++)
                {
                    const uint8_t* row = src + size_t(y) * rowPitch;
                    uint8_t* dst = right.data() + size_t(y) * extent.width * 4;
                    for (uint32_t x = 0; x < extent.width; x++)
                    {
                        float v = 0.0f;
                        if (floatFormat)
                            std::memcpy(&v, row + size_t(x) * 4, sizeof(v));
                        else if (bytesPerTexel == 4)
                        {
                            const uint32_t word = uint32_t(row[4 * x]) | (uint32_t(row[4 * x + 1]) << 8)
                                | (uint32_t(row[4 * x + 2]) << 16) | (uint32_t(row[4 * x + 3]) << 24);
                            v = float(word & 0xFFFFFFu) / 16777215.0f;
                        }
                        else
                        {
                            const uint16_t u = uint16_t(row[2 * x] | (row[2 * x + 1] << 8));
                            v = float(u) / 65535.0f;
                        }
                        std::memcpy(dst + size_t(x) * 4, &v, sizeof(v));
                    }
                }
            }
            logicalDevice->vkd.UnmapMemory(logicalDevice->device, staging.memory);
            logicalDevice->vkd.DestroyFence(logicalDevice->device, fence, nullptr);
            logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, pool, nullptr);
            destroyStaging(logicalDevice);

            const uint32_t width = backbuffer != VK_NULL_HANDLE ? backbufferExtent.width : extent.width;
            const uint32_t height = backbuffer != VK_NULL_HANDLE ? backbufferExtent.height : extent.height;
            std::vector<uint8_t> left(size_t(width) * height * 4, 255);

            if (colorBuffer != VK_NULL_HANDLE)
            {
                void* colorMapped = nullptr;
                if (logicalDevice->vkd.MapMemory(logicalDevice->device, colorMemory, 0,
                                                 VkDeviceSize(width) * height * 4 + 256, 0, &colorMapped) == VK_SUCCESS
                    && colorMapped)
                {
                    const bool bgra = backbufferFormat == VK_FORMAT_B8G8R8A8_UNORM
                        || backbufferFormat == VK_FORMAT_B8G8R8A8_SRGB;
                    const uint8_t* src = static_cast<const uint8_t*>(colorMapped);
                    for (uint32_t y = 0; y < height; y++)
                    {
                        const uint8_t* row = src + size_t(y) * width * 4;
                        uint8_t* dst = left.data() + size_t(y) * width * 4;
                        for (uint32_t x = 0; x < width; x++)
                        {
                            const uint8_t b0 = row[4 * x + 0];
                            const uint8_t b1 = row[4 * x + 1];
                            const uint8_t b2 = row[4 * x + 2];
                            dst[4 * x + 0] = bgra ? b2 : b0;
                            dst[4 * x + 1] = b1;
                            dst[4 * x + 2] = bgra ? b0 : b2;
                            dst[4 * x + 3] = 255;
                        }
                    }
                    logicalDevice->vkd.UnmapMemory(logicalDevice->device, colorMemory);
                }
                logicalDevice->vkd.DestroyBuffer(logicalDevice->device, colorBuffer, nullptr);
                logicalDevice->vkd.FreeMemory(logicalDevice->device, colorMemory, nullptr);
            }

            if (width != extent.width || height != extent.height)
            {
                std::vector<uint8_t> fixed(size_t(width) * height * 4, 0);
                const uint32_t cw = std::min(width, extent.width);
                const uint32_t ch = std::min(height, extent.height);
                for (uint32_t y = 0; y < ch; y++)
                    std::memcpy(fixed.data() + size_t(y) * width * 4,
                                right.data() + size_t(y) * extent.width * 4, size_t(cw) * 4);
                right.swap(fixed);
            }

            {
                std::lock_guard l(s_mutex);
                s_requested = false;
                s_inFlight = false;
                s_active.store(false, std::memory_order_release);
            }

            std::string formatName = "D32_SFLOAT";
            switch (format)
            {
            case VK_FORMAT_D16_UNORM: formatName = "D16_UNORM"; break;
            case VK_FORMAT_D24_UNORM_S8_UINT: formatName = "D24_UNORM_S8_UINT"; break;
            case VK_FORMAT_X8_D24_UNORM_PACK32: formatName = "X8_D24_UNORM_PACK32"; break;
            case VK_FORMAT_D32_SFLOAT_S8_UINT: formatName = "D32_SFLOAT_S8_UINT"; break;
            case VK_FORMAT_R32_SFLOAT: formatName = "R32_SFLOAT"; break;
            default: break;
            }

            const std::string meta = "format=" + formatName
                + ",width=" + std::to_string(width)
                + ",height=" + std::to_string(height)
                + ",layout=backbuffer_left_depth_right"
                + ",depth_encoding=float32_bits_rgba"
                + ",transformed=" + std::string(transformed ? "1" : "0")
                + ",invert=" + std::string(settingsManager.getDepthInvert() ? "1" : "0")
                + ",flip=" + std::string(settingsManager.getDepthFlip() ? "1" : "0")
                + ",flop=" + std::string(settingsManager.getDepthFlop() ? "1" : "0")
                + ",source_channel=" + std::to_string(settingsManager.getDepthSourceChannel());

            std::string path;
            {
                std::lock_guard l(s_mutex);
                path = s_path;
            }

            Logger::info("depth dump: leaving " + std::to_string(width) + "x" + std::to_string(height)
                         + " fmt=" + formatName + " for " + path);

            AsyncWriter::instance().submit([path, meta, left = std::move(left), right = std::move(right),
                                            width, height]() mutable
            {
                const std::string png = encodeFramePng(left, right, width, height, meta);
                const bool written = writeAtomically(path, png);
                if (written)
                    Logger::info("depth dump: wrote " + path);
                std::lock_guard l(s_mutex);
                setResultLocked(written, written ? "Frame written to " + path
                                                 : "Could not write the frame dump to " + path);
            });
            return true;
        }

        bool pollResult(std::string& message, bool& success)
        {
            std::lock_guard l(s_mutex);
            if (!s_resultReady)
                return false;
            s_resultReady = false;
            message = s_resultMessage;
            success = s_resultOk;
            return true;
        }

        void abandon()
        {
            std::lock_guard l(s_mutex);
            s_requested = false;
            s_inFlight = false;
            s_active.store(false, std::memory_order_release);
            s_suppressUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(2330);
            setResultLocked(false, "Depth dump failed: the readback never completed.");
        }

        void destroyStaging(LogicalDevice* logicalDevice)
        {
            if (!logicalDevice)
                return;

            auto& staging = logicalDevice->depthDumpStaging;
            if (staging.buffer != VK_NULL_HANDLE)
            {
                logicalDevice->vkd.DestroyBuffer(logicalDevice->device, staging.buffer, nullptr);
                staging.buffer = VK_NULL_HANDLE;
            }
            if (staging.memory != VK_NULL_HANDLE)
            {
                logicalDevice->vkd.FreeMemory(logicalDevice->device, staging.memory, nullptr);
                staging.memory = VK_NULL_HANDLE;
            }
            staging.size = 0;
            staging.format = VK_FORMAT_UNDEFINED;
            staging.extent = {0, 0, 1};
            staging.bytesPerTexel = 0;
        }
    } // namespace DepthDump
} // namespace VKIntox
